#include "render/ps2/gs_renderer.hpp"

#include "content/console_texture.hpp"
#include "render/mesh_pack.hpp"
#include "render/texture_convert.hpp"
#include "render/vertex_colors.hpp"

#include <dmaKit.h>
#include <gsInline.h>
#include <gsToolkit.h>
#include <kernel.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <span>

namespace ts2::render {

namespace {

constexpr float kGuard = 3.0f;           // clip to this many times the screen's width and height
constexpr float kZScale = 16777215.0f;   // 24-bit Z
constexpr std::size_t kMaxTrianglesPerPacket = 3000;  // nine quadwords each; a GIF tag counts at most 32767
constexpr float kBlendedLightMapShare = 0.5f;

double now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) * 1e-9;
}

unsigned log2Of(unsigned v) {
    unsigned n = 0;
    while ((1u << n) < v) {
        ++n;
    }
    return n;
}

// One vertex on its way to the screen: clip coordinates, the colour as the GS reads it (0x80 is 1), and the texture's.
struct ClipVertex {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
};

ClipVertex lerp(const ClipVertex& a, const ClipVertex& b, float t) {
    ClipVertex r;
    const float* pa = &a.x;
    const float* pb = &b.x;
    float* pr = &r.x;
    for (int i = 0; i < 10; ++i) {
        pr[i] = pa[i] + (pb[i] - pa[i]) * t;
    }
    return r;
}

// How far inside plane `which` a vertex is (negative: outside).
float inside(const ClipVertex& v, int which) {
    switch (which) {
    case 0: return v.w - 1e-4f;
    case 1: return v.w - v.z;  // nearer than the near plane (depth runs 1 at the near plane to 0 at the far one)
    case 2: return v.z;
    case 3: return v.x + kGuard * v.w;
    case 4: return kGuard * v.w - v.x;
    case 5: return v.y + kGuard * v.w;
    default: return kGuard * v.w - v.y;
    }
}

constexpr int kPlanes = 7;

unsigned outcode(const ClipVertex& v) {
    unsigned code = 0;
    for (int i = 0; i < kPlanes; ++i) {
        if (inside(v, i) < 0) {
            code |= 1u << i;
        }
    }
    return code;
}

// The polygon cut to the volume, in `out` (up to 3 + kPlanes vertices). Returns how many.
int clipPolygon(const ClipVertex* in, int count, ClipVertex* out) {
    ClipVertex a[16], b[16];
    std::copy(in, in + count, a);
    ClipVertex* from = a;
    ClipVertex* to = b;
    for (int plane = 0; plane < kPlanes && count > 0; ++plane) {
        int n = 0;
        for (int i = 0; i < count; ++i) {
            const ClipVertex& p = from[i];
            const ClipVertex& q = from[(i + 1) % count];
            const float dp = inside(p, plane), dq = inside(q, plane);
            if (dp >= 0) {
                to[n++] = p;
            }
            if ((dp >= 0) != (dq >= 0)) {
                to[n++] = lerp(p, q, dp / (dp - dq));
            }
        }
        std::swap(from, to);
        count = n;
    }
    std::copy(from, from + count, out);
    return count;
}

} // namespace

// A texture as the GS takes it: the texels (and the palette) in memory the DMA can read, and the gsKit texture that goes with them.
struct GsTexture {
    GSTEXTURE tex{};
    std::uint8_t* texels = nullptr;
    u32* clut = nullptr;
    ~GsTexture() {
        std::free(texels);
        std::free(clut);
    }
    GsTexture() = default;
    GsTexture(const GsTexture&) = delete;
    GsTexture& operator=(const GsTexture&) = delete;
};

struct GsRenderer::Gpu {
    std::vector<GpuMesh> meshes;
    MeshData data;
    bool baked = false;                              // meshes are expanded from `bakedMeshes` when drawn (see prepareBaked)
    BakedMeshes bakedMeshes;
    std::vector<const BakedPrimitive*> refs;         // by GpuPrimitive::firstVertex
    std::vector<std::unique_ptr<GsTexture>> textures;  // by the scene's image index, null for none (or not read yet)
    std::vector<scene::Image> sources;                 // where each image is, for reading it when it is first drawn
    std::vector<std::size_t> lastUsed;                 // per image: the frame it was last drawn in
    std::vector<std::size_t> bytes;                    // per image: what its texture holds in memory
    std::size_t resident = 0;
    std::map<int, scene::Image> lightImages;           // the light maps, as pixels (they are read at the vertices)
    GsTexture* freeable = nullptr;
};

struct GsRenderer::GpuPrepared final : IRenderer::PreparedBase {
    std::unique_ptr<GsRenderer::Gpu> gpu;
    GSGLOBAL* gs = nullptr;
    void releaseSome(std::size_t) override {
        if (gpu) {
            for (auto& t : gpu->textures) {
                if (t) {
                    gsKit_TexManager_free(gs, &t->tex);
                }
            }
            gpu.reset();
        }
    }
};

namespace {

// A texture from a baked image: its largest level, with alpha in the GS's range (0x80 is opaque) and an 8-bit palette
// in the order the GS reads it (entries 8-15 and 16-23 of each 32 swapped).
std::unique_ptr<GsTexture> makeTexture(std::span<const std::uint8_t> blob) {
    const content::ConsoleTextureView view = content::ConsoleTextureView::of(blob);
    auto t = std::make_unique<GsTexture>();
    const std::size_t texelBytes = content::ConsoleTexture::texelBytes(view.width, view.height, view.bits, 1);
    t->texels = static_cast<std::uint8_t*>(memalign(64, (texelBytes + 63) / 64 * 64));
    std::memcpy(t->texels, view.texels.data(), texelBytes);
    const auto halve = [](std::uint8_t a) { return static_cast<std::uint8_t>((a + 1) >> 1); };
    if (view.bits == 32) {
        for (std::size_t i = 3; i < texelBytes; i += 4) {
            t->texels[i] = halve(t->texels[i]);
        }
        t->tex.PSM = GS_PSM_CT32;
    } else {
        const std::size_t entries = view.bits == 8 ? 256 : 16;
        t->clut = static_cast<u32*>(memalign(64, entries * 4));
        std::memset(t->clut, 0, entries * 4);
        for (std::size_t i = 0; i < view.palette.size() / 4 && i < entries; ++i) {
            const std::uint8_t* c = &view.palette[i * 4];
            const std::uint32_t value = c[0] | (c[1] << 8) | (c[2] << 16) | (std::uint32_t{halve(c[3])} << 24);
            const std::size_t block = (i >> 3) & 3;
            const std::size_t at = view.bits == 8 ? (block == 1 ? i + 8 : block == 2 ? i - 8 : i) : i;
            t->clut[at] = value;
        }
        t->tex.PSM = view.bits == 8 ? GS_PSM_T8 : GS_PSM_T4;
        t->tex.Clut = t->clut;
        t->tex.ClutPSM = GS_PSM_CT32;
    }
    t->tex.Width = view.width;
    t->tex.Height = view.height;
    t->tex.Mem = reinterpret_cast<u32*>(t->texels);
    t->tex.Filter = GS_FILTER_LINEAR;
    return t;
}

// Writes one A+D register to the draw queue.
void ad(GSGLOBAL* gs, std::uint64_t data, std::uint64_t reg) {
    auto* p = static_cast<std::uint64_t*>(gsKit_heap_alloc(gs, 1, 16, GIF_AD));
    if (static_cast<void*>(p) == gs->CurQueue->last_tag) {
        *p++ = GIF_TAG_AD(1);
        *p++ = GIF_AD;
    }
    *p++ = data;
    *p++ = reg;
}

// Sends what is queued when the pool is nearly full, so a frame's draws needn't fit in one.
void makeRoom(GSGLOBAL* gs, std::size_t bytes) {
    GSQUEUE* q = gs->CurQueue;
    if (static_cast<std::size_t>(static_cast<std::uint8_t*>(q->pool_max[q->dbuf]) - static_cast<std::uint8_t*>(q->pool_cur)) < bytes + 4096) {
        gsKit_queue_exec(gs);
    }
}

std::uint64_t floatBits(float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

Mat4 fromRows(const float* rows) {
    Mat4 r;
    std::copy_n(rows, 16, r.m.begin());
    return r;
}

} // namespace

GsRenderer::GsRenderer(GSGLOBAL* gs) : gs_(gs) {
    gs_->PrimAlphaEnable = GS_SETTING_OFF;
    gsKit_set_test(gs_, GS_ZTEST_ON);
}

GsRenderer::~GsRenderer() = default;

Capabilities GsRenderer::capabilities() const {
    Capabilities c;
    c.bumpMapping = false;
    c.floorWallNormalMapping = false;
    c.waterReflection = false;
    c.terrainLayers = false;
    c.gpuSkinning = false;
    c.anisotropic = false;
    c.threadedPrepare = false;
    c.offscreen = false;
    c.maxTextureSize = 1024;
    return c;
}

const std::vector<GpuMesh>& GsRenderer::meshes() const {
    static const std::vector<GpuMesh> none;
    return gpu_ ? gpu_->meshes : none;
}

bool GsRenderer::hasImage(int index) const {
    if (!gpu_ || index < 0 || static_cast<std::size_t>(index) >= gpu_->textures.size()) {
        return false;
    }
    const auto i = static_cast<std::size_t>(index);
    return gpu_->textures[i] != nullptr || (imageSource_ && gpu_->sources[i].consoleSize > 0);
}

// The texture of image `index`: read from the image source the first time it is drawn, after letting go of textures that have not been
// drawn for a while if the ones in memory would pass the budget. A texture drawn this frame or the last is never let go (its draws
// may still be waiting for the GS, which reads the space it holds).
GsTexture* GsRenderer::textureFor(int index) {
    if (!hasImage(index)) {
        return nullptr;
    }
    Gpu& g = *gpu_;
    const auto i = static_cast<std::size_t>(index);
    g.lastUsed[i] = frameNumber_;
    if (g.textures[i]) {
        return g.textures[i].get();
    }
    const std::vector<std::uint8_t> blob = imageSource_(g.sources[i]);
    if (blob.empty()) {
        return nullptr;
    }
    while (g.resident + blob.size() > textureBudget_) {
        std::size_t victim = g.textures.size();
        for (std::size_t k = 0; k < g.textures.size(); ++k) {
            if (g.textures[k] && g.lastUsed[k] + 1 < frameNumber_ && (victim == g.textures.size() || g.lastUsed[k] < g.lastUsed[victim])) {
                victim = k;
            }
        }
        if (victim == g.textures.size()) {
            break;  // everything in memory was drawn lately: go over the budget rather than disturb a draw in flight
        }
        gsKit_TexManager_free(gs_, &g.textures[victim]->tex);
        g.resident -= g.bytes[victim];
        g.bytes[victim] = 0;
        g.textures[victim].reset();
    }
    g.textures[i] = makeTexture(blob);
    g.bytes[i] = blob.size();
    g.resident += blob.size();
    ++frame_.uploads;
    return g.textures[i].get();
}

IRenderer::Prepared GsRenderer::prepare(const scene::Scene&) const {
    // A scene the builder made is not drawn here (the console cannot hold it); it arrives baked (see prepareBaked).
    return Prepared(std::make_unique<GpuPrepared>());
}

IRenderer::Prepared GsRenderer::prepareBaked(scene::Scene scene, BakedMeshes meshes) const {
    auto p = std::make_unique<GpuPrepared>();
    p->gs = gs_;
    p->gpu = std::make_unique<Gpu>();
    Gpu& g = *p->gpu;
    g.baked = true;
    g.bakedMeshes = std::move(meshes);
    const auto finite = [](const std::array<float, 3>& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); };
    for (const auto& primitives : g.bakedMeshes.meshes) {
        GpuMesh gm;
        for (std::size_t k = 0; k < primitives.size(); ++k) {
            const BakedPrimitive& b = primitives[k];
            if (b.vertices == 0 || b.indices == 0) {
                continue;
            }
            GpuPrimitive gp;
            gp.firstVertex = static_cast<std::uint32_t>(g.refs.size());
            g.refs.push_back(&b);
            gp.count = b.indices;
            gp.vertices = b.vertices;
            gp.material = b.material >= 0 && static_cast<std::size_t>(b.material) < scene.materials.size() ? b.material : -1;
            gp.lightMapped = b.lightMapped;
            gp.skinned = b.skinned;
            gp.source = k;
            gp.low = b.low;
            gp.high = b.high;
            gp.hasBox = finite(gp.low) && finite(gp.high);
            gp.cullable = !gp.skinned && gp.hasBox;
            if (gp.cullable) {
                for (std::size_t a = 0; a < 3; ++a) {
                    gm.low[a] = gm.anyCullable ? std::min(gm.low[a], gp.low[a]) : gp.low[a];
                    gm.high[a] = gm.anyCullable ? std::max(gm.high[a], gp.high[a]) : gp.high[a];
                }
                gm.anyCullable = true;
            }
            gm.primitives.push_back(gp);
        }
        g.meshes.push_back(std::move(gm));
    }
    g.textures.resize(scene.images.size());
    g.lastUsed.assign(scene.images.size(), 0);
    g.bytes.assign(scene.images.size(), 0);
    for (const scene::Image& image : scene.images) {
        scene::Image info;
        info.width = image.width;
        info.height = image.height;
        info.consoleOffset = image.consoleOffset;
        info.consoleSize = image.consoleSize;
        g.sources.push_back(std::move(info));
    }
    for (const scene::Instance& instance : scene.instances) {
        if (instance.lightMap >= 0 && static_cast<std::size_t>(instance.lightMap) < scene.images.size() && !g.lightImages.contains(instance.lightMap) &&
            !scene.images[static_cast<std::size_t>(instance.lightMap)].console.empty()) {
            g.lightImages[instance.lightMap] =
                decodeConsoleTexture(content::ConsoleTexture::parse(scene.images[static_cast<std::size_t>(instance.lightMap)].console));
        }
    }
    copyScene(*p, scene);
    return Prepared(std::move(p));
}

IRenderer::Prepared GsRenderer::swap(Prepared prepared) {
    if (!prepared.get()) {
        prepared = Prepared(std::make_unique<GpuPrepared>());
    }
    auto& p = static_cast<GpuPrepared&>(*prepared.get());
    if (!p.gpu) {
        p.gpu = std::make_unique<Gpu>();
    }
    std::swap(gpu_, p.gpu);
    p.gs = gs_;
    swapScene(p);
    return prepared;
}

namespace {

std::uint64_t hashWords(const void* data, std::size_t bytes, std::uint64_t h = 0xCBF29CE484222325ull) {
    const auto* w = static_cast<const std::uint32_t*>(data);
    for (std::size_t i = 0; i < bytes / 4; ++i) {
        h = (h ^ w[i]) * 0x100000001B3ull;
    }
    return h;
}

constexpr std::size_t kTrianglesPerPacket = 2000;  // three vertices of three quadwords each; a GIF tag counts at most 32,767 loops

} // namespace

void GsRenderer::drawList(const FrameView& f, const std::vector<Draw>& list, int blend) {
    const float width = static_cast<float>(gs_->Width), height = static_cast<float>(gs_->Height);
    std::vector<ClipVertex> clipVertices;
    std::vector<GsVertex> clipOut;
    for (const Draw& d : list) {
        const GpuPrimitive& p = *d.primitive;
        if (p.skinned || !gpu_->baked) {
            continue;
        }
        const BakedPrimitive& baked = *gpu_->refs[p.firstVertex];
        const scene::Material* m = d.material;
        const DrawUniforms u = FramePlan::uniformsFor(*this, *this, f, d, blend);
        const VertexUniforms& vu = u.vertex.frame;
        const FragmentUniforms& fu = u.fragment;
        ++frame_.draws;

        double t0 = now();
        PrimitiveView view;
        viewPrimitive(gpu_->bakedMeshes, baked, view);
        if (view.positions == nullptr || view.indexData == nullptr) {
            continue;
        }
        const unsigned n = view.vertices;

        // The vertices' lit colours: kept from the last frame while nothing that lights them changed (the lights, the object's place and
        // material), else worked out from the primitive expanded.
        const std::uint8_t* colours = nullptr;
        {
            std::uint64_t hash = hashWords(reinterpret_cast<const std::uint8_t*>(&vu) + offsetof(VertexUniforms, model), sizeof vu - offsetof(VertexUniforms, model));
            hash = hashWords(&fu, sizeof fu, hash);
            const std::uint64_t key = (static_cast<std::uint64_t>(d.instance - instances_.data()) << 32) | p.firstVertex;
            auto found = litCache_.find(key);
            frame_.vertices += n;
            if (found != litCache_.end() && found->second.hash == hash && found->second.rgba.size() == std::size_t{n} * 4) {
                colours = found->second.rgba.data();
                ++frame_.litHits;
            } else {
                ++frame_.litMisses;
                expandPrimitive(gpu_->bakedMeshes, baked, expanded_);
                const scene::Image* lightImage = nullptr;
                const bool lightMapped = fu.lightMap[0] > 0;
                if (lightMapped) {
                    if (const auto image = gpu_->lightImages.find(d.instance->lightMap); image != gpu_->lightImages.end()) {
                        lightImage = &image->second;
                    }
                }
                // The GS reads a vertex colour of 0x80 as 1 and doubles up to 0xFF, as the texture environment scale of the OpenGL 1 path.
                vertexColors(vu, fu, expanded_.vertices.data(), n, lightMapped && lightImage == nullptr ? std::min(1.0f, kBlendedLightMapShare * fu.lightMap[0]) : 1.0f,
                             0.5f, lightImage, nullptr, litColors_);
                const std::size_t bytes = litColors_.size();
                if (found != litCache_.end()) {
                    litCacheBytes_ -= found->second.rgba.size();
                    litCache_.erase(found);
                }
                if (litCacheBytes_ + bytes <= litCacheBudget_) {
                    LitColors& entry = litCache_[key];
                    entry.hash = hash;
                    entry.rgba = litColors_;
                    litCacheBytes_ += bytes;
                    colours = entry.rgba.data();
                } else {
                    colours = litColors_.data();
                }
            }
        }
        frame_.light += now() - t0;
        t0 = now();

        // Each vertex projected once, straight from its 16-bit coordinates: the range's scale is folded into the matrix's columns.
        const Mat4 mvp = f.viewProjection * fromRows(vu.model);
        const auto& r = mvp.m;
        const float scale[3] = {(view.hi[0] - view.lo[0]) / 65535.0f, (view.hi[1] - view.lo[1]) / 65535.0f, (view.hi[2] - view.lo[2]) / 65535.0f};
        float column[3][4], base[4];
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 3; ++k) {
                column[k][row] = r[static_cast<std::size_t>(row * 4 + k)] * scale[k];
            }
            base[row] = r[static_cast<std::size_t>(row * 4)] * view.lo[0] + r[static_cast<std::size_t>(row * 4 + 1)] * view.lo[1] +
                        r[static_cast<std::size_t>(row * 4 + 2)] * view.lo[2] + r[static_cast<std::size_t>(row * 4 + 3)];
        }
        const float uvScale = (view.uvHi - view.uvLo) / 65535.0f;
        screen_.resize(n);
        screenXy_.resize(std::size_t{n} * 2);
        codes_.resize(n);
        for (unsigned i = 0; i < n; ++i) {
            const float ux = view.positions[i * 3], uy = view.positions[i * 3 + 1], uz = view.positions[i * 3 + 2];
            ClipVertex c;
            c.x = base[0] + column[0][0] * ux + column[1][0] * uy + column[2][0] * uz;
            c.y = base[1] + column[0][1] * ux + column[1][1] * uy + column[2][1] * uz;
            c.z = base[2] + column[0][2] * ux + column[1][2] * uy + column[2][2] * uz;
            c.w = base[3] + column[0][3] * ux + column[1][3] * uy + column[2][3] * uz;
            const unsigned code = outcode(c);
            codes_[i] = static_cast<std::uint8_t>(code);
            if (code != 0) {
                continue;  // not drawn as it is: a triangle with this vertex is dropped or clipped (see below)
            }
            const float q = 1.0f / c.w;
            const float sx = (c.x * q * 0.5f + 0.5f) * width, sy = (0.5f - c.y * q * 0.5f) * height;
            screenXy_[i * 2] = sx;
            screenXy_[i * 2 + 1] = sy;
            GsVertex& v = screen_[i];
            const float u0 = view.uvs ? view.uvLo + uvScale * view.uvs[i * 2] : 0.0f, v0 = view.uvs ? view.uvLo + uvScale * view.uvs[i * 2 + 1] : 0.0f;
            v.s = u0 * q;
            v.t = v0 * q;
            v.q = q;
            v.unused = 0;
            v.r = colours[i * 4];
            v.g = colours[i * 4 + 1];
            v.b = colours[i * 4 + 2];
            v.a = colours[i * 4 + 3] >> 1;
            v.x = static_cast<std::uint32_t>(gsKit_float_to_int_x(gs_, sx));
            v.y = static_cast<std::uint32_t>(gsKit_float_to_int_y(gs_, sy));
            v.z = static_cast<std::uint32_t>(std::clamp(c.z * q, 0.0f, 1.0f) * kZScale);
            v.adc = 0;
        }
        frame_.transform += now() - t0;
        t0 = now();

        // The texture, and the state it and the material ask for.
        GsTexture* texture = nullptr;
        if (u.samplers[0].texture.kind == TextureRef::Kind::Image) {
            texture = textureFor(u.samplers[0].texture.index);
        }
        const bool additive = blend && m && m->alpha == scene::AlphaMode::Add;
        const bool blended = blend != 0;
        const bool alphaTest = fu.alphaCutoff > 0;
        const unsigned ctx = gs_->PrimContext;
        makeRoom(gs_, 4096);
        if (texture) {
            gsKit_TexManager_bind(gs_, &texture->tex);
            const GSTEXTURE& t = texture->tex;
            const bool hasClut = t.Clut != nullptr;
            ad(gs_,
               GS_SETREG_TEX0(t.Vram / 256, t.TBW, t.PSM, log2Of(t.Width), log2Of(t.Height), 1, 0, hasClut ? t.VramClut / 256 : 0, t.ClutPSM, 0, 0,
                              hasClut ? GS_CLUT_STOREMODE_LOAD : GS_CLUT_STOREMODE_NOLOAD),
               GS_TEX0_1 + ctx);
            ad(gs_, GS_SETREG_TEX1(1, 0, 1, 1, 0, 0, 0), GS_TEX1_1 + ctx);  // linear, no mip levels yet
            const bool clamped = u.samplers[0].sampler == SamplerKind::Clamped;
            ad(gs_, GS_SETREG_CLAMP(clamped ? 1 : 0, clamped ? 1 : 0, 0, 0, 0, 0), GS_CLAMP_1 + ctx);
        }
        if (blended) {
            ad(gs_, additive ? GS_SETREG_ALPHA(0, 2, 0, 1, 0) : GS_SETREG_ALPHA(0, 1, 0, 1, 0), GS_ALPHA_1 + ctx);
        }
        // Alpha test: pass where the alpha is at least the cutoff (the GS reads 0x80 as 1). Depth test: nearer or equal.
        const unsigned reference = alphaTest ? static_cast<unsigned>(std::clamp(fu.alphaCutoff * 128.0f, 0.0f, 255.0f)) : 0;
        ad(gs_, GS_SETREG_TEST(alphaTest ? 1 : 0, 5, reference, 0, 0, 0, 1, 2), GS_TEST_1 + ctx);
        ad(gs_, GS_SETREG_ZBUF_1(gs_->ZBuffer / 8192, gs_->PSMZ, blended ? 1 : 0), GS_ZBUF_1);
        frame_.state += now() - t0;
        t0 = now();

        // Which triangles are drawn: those with all three vertices inside the volume that face the camera are copied as they are; those that
        // cross a plane are clipped; the rest are dropped.
        const bool cull = !(m && m->doubleSided);
        const bool wide = view.wideIndices;
        const auto* index16 = static_cast<const std::uint16_t*>(view.indexData);
        const auto* index32 = static_cast<const std::uint32_t*>(view.indexData);
        const auto indexAt = [&](std::size_t k) { return wide ? index32[k] : std::uint32_t{index16[k]}; };
        visibleTris_.clear();
        clippedTris_.clear();
        const std::uint32_t triangles = view.indices / 3;
        for (std::uint32_t k = 0; k < triangles; ++k) {
            const std::uint32_t a = indexAt(k * 3), b = indexAt(k * 3 + 1), c = indexAt(k * 3 + 2);
            if (a >= n || b >= n || c >= n) {
                continue;
            }
            const unsigned ca = codes_[a], cb = codes_[b], cc = codes_[c];
            if ((ca & cb & cc) != 0) {
                continue;  // all outside one plane
            }
            if ((ca | cb | cc) != 0) {
                clippedTris_.push_back(k);
                continue;
            }
            if (cull) {
                const float* sa = &screenXy_[a * 2];
                const float* sb = &screenXy_[b * 2];
                const float* sc = &screenXy_[c * 2];
                // Screen y runs down, so the faces that look at the camera wind the other way.
                if ((sb[0] - sa[0]) * (sc[1] - sa[1]) - (sc[0] - sa[0]) * (sb[1] - sa[1]) >= 0) {
                    ++frame_.culled;
                    continue;
                }
            }
            visibleTris_.push_back(k);
        }
        frame_.triangles += triangles;
        frame_.clipped += clippedTris_.size();

        // The packets: one GIF tag, then three quadwords a vertex. Written straight into the draw queue.
        const std::uint64_t prim = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, texture ? 1 : 0, 0, blended ? 1 : 0, 0, 0, ctx, 0);
        const auto packet = [&](std::size_t count, const auto& vertexOf) {
            for (std::size_t done = 0; done < count;) {
                const std::size_t batch = std::min(kTrianglesPerPacket, count - done);
                const std::size_t qwords = batch * 9;
                makeRoom(gs_, qwords * 16 + 64);
                auto* out = static_cast<std::uint64_t*>(gsKit_heap_alloc(gs_, static_cast<int>(qwords), static_cast<int>(qwords * 16), GIF_AD));
                *out++ = GIF_TAG(batch * 3, 1, 1, prim, GSKIT_GIF_FLG_PACKED, 3);
                *out++ = 0x512;  // ST, RGBAQ, XYZ2
                auto* dst = reinterpret_cast<GsVertex*>(out);
                for (std::size_t t = 0; t < batch; ++t) {
                    vertexOf(done + t, dst);
                    dst += 3;
                }
                done += batch;
            }
        };
        packet(visibleTris_.size(), [&](std::size_t t, GsVertex* dst) {
            const std::uint32_t k = visibleTris_[t];
            dst[0] = screen_[indexAt(k * 3)];
            dst[1] = screen_[indexAt(k * 3 + 1)];
            dst[2] = screen_[indexAt(k * 3 + 2)];
        });
        frame_.drawn += visibleTris_.size();

        // The few that cross a plane: cut to it, each piece projected afresh.
        if (!clippedTris_.empty()) {
            clipOut.clear();
            const auto corner = [&](std::uint32_t index) {
                const float ux = view.positions[index * 3], uy = view.positions[index * 3 + 1], uz = view.positions[index * 3 + 2];
                ClipVertex c;
                c.x = base[0] + column[0][0] * ux + column[1][0] * uy + column[2][0] * uz;
                c.y = base[1] + column[0][1] * ux + column[1][1] * uy + column[2][1] * uz;
                c.z = base[2] + column[0][2] * ux + column[1][2] * uy + column[2][2] * uz;
                c.w = base[3] + column[0][3] * ux + column[1][3] * uy + column[2][3] * uz;
                c.r = colours[index * 4];
                c.g = colours[index * 4 + 1];
                c.b = colours[index * 4 + 2];
                c.a = colours[index * 4 + 3] * 0.5f;
                c.u = view.uvs ? view.uvLo + uvScale * view.uvs[index * 2] : 0.0f;
                c.v = view.uvs ? view.uvLo + uvScale * view.uvs[index * 2 + 1] : 0.0f;
                return c;
            };
            const auto project = [&](const ClipVertex& c, float& sx, float& sy) {
                const float q = 1.0f / c.w;
                sx = (c.x * q * 0.5f + 0.5f) * width;
                sy = (0.5f - c.y * q * 0.5f) * height;
                GsVertex v;
                v.s = c.u * q;
                v.t = c.v * q;
                v.q = q;
                v.unused = 0;
                const auto byte = [](float x) { return static_cast<std::uint32_t>(std::clamp(x, 0.0f, 255.0f)); };
                v.r = byte(c.r);
                v.g = byte(c.g);
                v.b = byte(c.b);
                v.a = byte(c.a);
                v.x = static_cast<std::uint32_t>(gsKit_float_to_int_x(gs_, sx));
                v.y = static_cast<std::uint32_t>(gsKit_float_to_int_y(gs_, sy));
                v.z = static_cast<std::uint32_t>(std::clamp(c.z * q, 0.0f, 1.0f) * kZScale);
                v.adc = 0;
                return v;
            };
            for (const std::uint32_t k : clippedTris_) {
                const ClipVertex in[3] = {corner(indexAt(k * 3)), corner(indexAt(k * 3 + 1)), corner(indexAt(k * 3 + 2))};
                ClipVertex poly[16];
                const int count = clipPolygon(in, 3, poly);
                for (int i = 1; i + 1 < count; ++i) {
                    float x[3], y[3];
                    const GsVertex a = project(poly[0], x[0], y[0]), b = project(poly[i], x[1], y[1]), c = project(poly[i + 1], x[2], y[2]);
                    if (cull && (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]) >= 0) {
                        continue;
                    }
                    clipOut.push_back(a);
                    clipOut.push_back(b);
                    clipOut.push_back(c);
                }
            }
            packet(clipOut.size() / 3, [&](std::size_t t, GsVertex* dst) {
                dst[0] = clipOut[t * 3];
                dst[1] = clipOut[t * 3 + 1];
                dst[2] = clipOut[t * 3 + 2];
            });
            frame_.drawn += clipOut.size() / 3;
        }
        frame_.emit += now() - t0;
    }
}

bool GsRenderer::draw(const Camera& camera) {
    frame_ = {};
    ++frameNumber_;
    const FrameView f = FrameView::make(camera, gs_->Width, gs_->Height);
    gsKit_clear(gs_, GS_SETREG_RGBAQ(static_cast<int>(clear_[0] * 255), static_cast<int>(clear_[1] * 255), static_cast<int>(clear_[2] * 255), 0x80, 0));
    if (drawScene_ && gpu_) {
        std::vector<Draw> opaque, layers, blended;
        const double c0 = now();
        FramePlan::collectDraws(*this, *this, f, opaque, layers, blended);
        frame_.collect = now() - c0;
        drawList(f, opaque, 0);
        drawList(f, layers, 1);
        drawList(f, blended, 1);
    }
    frame_.textureBytes = gpu_ ? gpu_->resident : 0;
    frame_.litCacheKiB = litCacheBytes_ / 1024;
    const double s0 = now();
    gsKit_queue_exec(gs_);
    gsKit_sync_flip(gs_);
    frame_.submit = now() - s0;
    gsKit_TexManager_nextFrame(gs_);
    return true;
}

} // namespace ts2::render
