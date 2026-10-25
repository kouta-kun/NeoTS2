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
#include <malloc.h>

namespace ts2::render {

namespace {

constexpr float kGuard = 3.0f;           // clip to this many times the screen's width and height
constexpr float kZScale = 16777215.0f;   // 24-bit Z
constexpr std::size_t kMaxTrianglesPerPacket = 3000;  // nine quadwords each; a GIF tag counts at most 32767
constexpr float kBlendedLightMapShare = 0.5f;

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
    std::vector<std::unique_ptr<GsTexture>> textures;  // by the scene's image index, null for none
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
std::unique_ptr<GsTexture> makeTexture(const scene::Image& image) {
    const content::ConsoleTextureView view = content::ConsoleTextureView::of(image.console);
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
    return gpu_ && index >= 0 && static_cast<std::size_t>(index) < gpu_->textures.size() && gpu_->textures[static_cast<std::size_t>(index)] != nullptr;
}

IRenderer::Prepared GsRenderer::prepare(const scene::Scene& scene) const {
    auto p = std::make_unique<GpuPrepared>();
    p->gs = gs_;
    p->gpu = std::make_unique<Gpu>();
    Gpu& g = *p->gpu;
    reserveMeshData(scene.meshes, 0, g.data);
    g.meshes.reserve(scene.meshes.size());
    for (const scene::Mesh& mesh : scene.meshes) {
        g.meshes.push_back(packMesh(mesh, scene.materials.size(), 0, g.data));
    }
    g.textures.resize(scene.images.size());
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        if (!scene.images[i].console.empty()) {
            g.textures[i] = makeTexture(scene.images[i]);
        }
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

void GsRenderer::drawList(const FrameView& f, const std::vector<Draw>& list, int blend) {
    std::vector<ClipVertex> clip;
    std::vector<std::uint64_t> packet;
    // What the GS was last told, so a draw that changes nothing sends nothing.
    for (const Draw& d : list) {
        const GpuPrimitive& p = *d.primitive;
        if (p.skinned || gpu_->data.vertices.empty()) {
            continue;
        }
        const scene::Material* m = d.material;
        const DrawUniforms u = FramePlan::uniformsFor(*this, *this, f, d, blend);
        const VertexUniforms& vu = u.vertex.frame;
        const FragmentUniforms& fu = u.fragment;
        ++frame_.draws;

        const float* vertices = &gpu_->data.vertices[static_cast<std::size_t>(p.firstVertex) * kVertexFloats];
        const scene::Image* lightImage = nullptr;
        const bool lightMapped = fu.lightMap[0] > 0;
        if (lightMapped) {
            if (const auto found = gpu_->lightImages.find(d.instance->lightMap); found != gpu_->lightImages.end()) {
                lightImage = &found->second;
            }
        }
        // The GS reads a vertex colour of 0x80 as 1 and doubles up to 0xFF, as the texture environment scale of the OpenGL 1 path.
        vertexColors(vu, fu, vertices, p.vertices, lightMapped && lightImage == nullptr ? std::min(1.0f, kBlendedLightMapShare * fu.lightMap[0]) : 1.0f,
                     0.5f, lightImage, nullptr, litColors_);

        // Where each vertex lands in clip space.
        const Mat4 mvp = f.viewProjection * fromRows(vu.model);
        clip.resize(p.vertices);
        for (unsigned i = 0; i < p.vertices; ++i) {
            const float* v = vertices + std::size_t{i} * kVertexFloats;
            ClipVertex& c = clip[i];
            const auto& r = mvp.m;
            c.x = r[0] * v[0] + r[1] * v[1] + r[2] * v[2] + r[3];
            c.y = r[4] * v[0] + r[5] * v[1] + r[6] * v[2] + r[7];
            c.z = r[8] * v[0] + r[9] * v[1] + r[10] * v[2] + r[11];
            c.w = r[12] * v[0] + r[13] * v[1] + r[14] * v[2] + r[15];
            const std::uint8_t* col = &litColors_[std::size_t{i} * 4];
            c.r = col[0];
            c.g = col[1];
            c.b = col[2];
            c.a = col[3] * 0.5f;  // 0x80 is opaque
            c.u = v[6];
            c.v = v[7];
        }

        // The texture, and the state it and the material ask for.
        GsTexture* texture = nullptr;
        if (u.samplers[0].texture.kind == TextureRef::Kind::Image && hasImage(u.samplers[0].texture.index)) {
            texture = gpu_->textures[static_cast<std::size_t>(u.samplers[0].texture.index)].get();
        }
        const bool additive = blend && m && m->alpha == scene::AlphaMode::Add;
        const bool blended = blend != 0;
        const bool alphaTest = fu.alphaCutoff > 0;
        const unsigned ctx = gs_->PrimContext;
        makeRoom(gs_, 4096);
        if (texture) {
            const std::size_t before = frame_.uploads;
            (void)before;
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

        // Triangles, clipped and culled.
        const std::uint32_t* indices = &gpu_->data.indices[p.firstIndex];
        const bool cull = !(m && m->doubleSided);
        const float width = static_cast<float>(gs_->Width), height = static_cast<float>(gs_->Height);
        packet.clear();
        std::size_t inPacket = 0;
        const auto flush = [&] {
            if (packet.empty()) {
                return;
            }
            const std::size_t qwords = packet.size() / 2 + 1;  // the PRIM register and the vertices
            makeRoom(gs_, qwords * 16 + 64);
            auto* out = static_cast<std::uint64_t*>(gsKit_heap_alloc(gs_, static_cast<int>(qwords), static_cast<int>(qwords * 16), GIF_AD));
            *out++ = GIF_TAG_AD(qwords);
            *out++ = GIF_AD;
            *out++ = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, texture ? 1 : 0, 0, blended ? 1 : 0, 0, 0, ctx, 0);
            *out++ = GS_PRIM;
            std::memcpy(out, packet.data(), packet.size() * 8);
            packet.clear();
            inPacket = 0;
        };
        const auto emit = [&](const ClipVertex& v) {
            const float q = 1.0f / v.w;
            const float sx = (v.x * q * 0.5f + 0.5f) * width, sy = (0.5f - v.y * q * 0.5f) * height;
            const float sz = std::clamp(v.z * q, 0.0f, 1.0f) * kZScale;
            const auto byte = [](float c) { return static_cast<std::uint64_t>(std::clamp(c, 0.0f, 255.0f)); };
            packet.push_back(byte(v.r) | (byte(v.g) << 8) | (byte(v.b) << 16) | (byte(v.a) << 24) | (floatBits(q) << 32));
            packet.push_back(GS_RGBAQ);
            packet.push_back(floatBits(v.u * q) | (floatBits(v.v * q) << 32));
            packet.push_back(GS_ST);
            packet.push_back(static_cast<std::uint64_t>(gsKit_float_to_int_x(gs_, sx) & 0xFFFF) | (static_cast<std::uint64_t>(gsKit_float_to_int_y(gs_, sy) & 0xFFFF) << 16) |
                             (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sz)) << 32));
            packet.push_back(GS_XYZ2);
        };
        const auto emitTriangle = [&](const ClipVertex& a, const ClipVertex& b, const ClipVertex& c) {
            if (cull) {
                const float ax = a.x / a.w, ay = a.y / a.w, bx = b.x / b.w, by = b.y / b.w, cx = c.x / c.w, cy = c.y / c.w;
                if ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay) <= 0) {
                    ++frame_.culled;
                    return;
                }
            }
            emit(a);
            emit(b);
            emit(c);
            ++frame_.drawn;
            if (++inPacket >= kMaxTrianglesPerPacket) {
                flush();
            }
        };
        for (unsigned k = 0; k + 2 < p.count; k += 3) {
            const ClipVertex &a = clip[indices[k]], &b = clip[indices[k + 1]], &c = clip[indices[k + 2]];
            ++frame_.triangles;
            const unsigned oa = outcode(a), ob = outcode(b), oc = outcode(c);
            if ((oa & ob & oc) != 0) {
                continue;  // all outside one plane
            }
            if ((oa | ob | oc) == 0) {
                emitTriangle(a, b, c);
                continue;
            }
            ++frame_.clipped;
            const ClipVertex in[3] = {a, b, c};
            ClipVertex poly[16];
            const int n = clipPolygon(in, 3, poly);
            for (int i = 1; i + 1 < n; ++i) {
                emitTriangle(poly[0], poly[i], poly[i + 1]);
            }
        }
        flush();
    }
}

bool GsRenderer::draw(const Camera& camera) {
    frame_ = {};
    const FrameView f = FrameView::make(camera, gs_->Width, gs_->Height);
    gsKit_clear(gs_, GS_SETREG_RGBAQ(static_cast<int>(clear_[0] * 255), static_cast<int>(clear_[1] * 255), static_cast<int>(clear_[2] * 255), 0x80, 0));
    if (drawScene_ && gpu_) {
        std::vector<Draw> opaque, layers, blended;
        FramePlan::collectDraws(*this, *this, f, opaque, layers, blended);
        drawList(f, opaque, 0);
        drawList(f, layers, 1);
        drawList(f, blended, 1);
    }
    gsKit_queue_exec(gs_);
    gsKit_sync_flip(gs_);
    gsKit_TexManager_nextFrame(gs_);
    return true;
}

} // namespace ts2::render
