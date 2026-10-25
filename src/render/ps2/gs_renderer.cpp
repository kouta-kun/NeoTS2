#include "render/ps2/gs_renderer.hpp"

#include "content/console_texture.hpp"
#include "render/mesh_pack.hpp"
#include "render/texture_convert.hpp"
#include "render/vertex_colors.hpp"

#include <dma.h>
#include <dmaKit.h>
#include <packet2.h>
#include <packet2_utils.h>
#include <gsInline.h>
#include <gsToolkit.h>
#include <kernel.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <kernel.h>
#include <malloc.h>
#include <span>

extern "C" u32 VU1Soup_CodeStart __attribute__((section(".vudata")));
extern "C" u32 VU1Soup_CodeEnd __attribute__((section(".vudata")));

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
    unsigned texelQw = 0, clutQw = 0;      // quadwords of each, as the GS takes them
    unsigned clutWidth = 0, clutHeight = 0;  // the palette's size as an image (8 x 2 or 16 x 16)
    bool resident = false;                 // its texels (and palette) are in VRAM at tex.Vram and tex.VramClut
    unsigned vramBytes = 0;                // what it holds there, from tex.Vram
    ~GsTexture() {
        std::free(texels);
        std::free(clut);
    }
    GsTexture() = default;
    GsTexture(const GsTexture&) = delete;
    GsTexture& operator=(const GsTexture&) = delete;
};

// A primitive as triangle soup, three corners a triangle and no indices, in the arrays the VIF unpacks into the VU1's memory (see vu1_soup.vsm).
// Built from the baked primitive when the scene is prepared. The arrays are 16-byte aligned and padded to a multiple of 12 corners with
// degenerate triangles so that every unpack covers whole quadwords.
struct Soup {
    unsigned corners = 0;
    float lo[3]{}, hi[3]{};
    float uvLo = 0, uvHi = 0;
    u16* pos = nullptr;        // x, y, z, 0 over [lo, hi]
    u16* uv = nullptr;         // u, v over [uvLo, uvHi]
    std::int8_t* normal = nullptr;  // x, y, z, 0 (/127)
    u8* vc = nullptr;          // r, g, b, a of the vertex colour and alpha, or null
    u16* luv = nullptr;        // light-map coordinates (/65535), or null
    ~Soup() {
        std::free(pos);
        std::free(uv);
        std::free(normal);
        std::free(vc);
        std::free(luv);
    }
    Soup() = default;
    Soup(const Soup&) = delete;
    Soup& operator=(const Soup&) = delete;
};

struct GsRenderer::VuColors {
    std::uint64_t hash = 0;
    unsigned corners = 0;
    u8* rgba = nullptr;  // r, g, b, a/2 a corner, 16-byte aligned
    ~VuColors() { std::free(rgba); }
    VuColors() = default;
    VuColors(const VuColors&) = delete;
    VuColors& operator=(const VuColors&) = delete;
};

struct alignas(16) Qw {
    std::uint64_t lo, hi;
};

struct alignas(16) VifTag {  // a DMA tag and the two VIF codes that go with it
    std::uint64_t tag;
    std::uint32_t vif0, vif1;
};

// What one draw's chain is built in, so that the EE can build the next while the DMA sends this one.
struct GsRenderer::VuSlot {
    Qw* header = nullptr;      // 7 quadwords of GS state
    Qw* tags = nullptr;        // the vertices' GIF tag for a full batch, and for the last
    float* constants = nullptr;  // 9 quadwords
    Qw* upload = nullptr;        // the packets that upload a texture and its palette (see drawListVu)
    VifTag* chain = nullptr;
    std::size_t chainCapacity = 0;
    u8* scratch = nullptr;     // lit colours of a draw that is not kept
    std::size_t scratchCapacity = 0;
    VuSlot() {
        header = static_cast<Qw*>(memalign(64, 8 * sizeof(Qw)));
        tags = static_cast<Qw*>(memalign(64, 2 * sizeof(Qw)));
        constants = static_cast<float*>(memalign(64, 9 * 16));
        upload = static_cast<Qw*>(memalign(64, 14 * sizeof(Qw)));
    }
    ~VuSlot() {
        std::free(header);
        std::free(tags);
        std::free(constants);
        std::free(upload);
        std::free(chain);
        std::free(scratch);
    }
};

struct GsRenderer::Gpu {
    std::vector<std::unique_ptr<Soup>> soups;  // by GpuPrimitive::firstVertex, like refs
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
    gsKit_setup_tbw(&t->tex);
    t->texelQw = static_cast<unsigned>((texelBytes + 15) / 16);
    SyncDCache(t->texels, t->texels + texelBytes + 16);
    if (t->clut) {
        t->clutWidth = view.bits == 8 ? 16 : 8;
        t->clutHeight = view.bits == 8 ? 16 : 2;
        t->clutQw = t->clutWidth * t->clutHeight / 4;
        SyncDCache(t->clut, reinterpret_cast<u8*>(t->clut) + t->clutQw * 16);
    }
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
    initVu();
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
        if (auto found = std::find(vramList_.begin(), vramList_.end(), g.textures[victim].get()); found != vramList_.end()) {
            vramList_.erase(found);
        }
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
    // Each primitive as triangle soup, for the vector unit.
    g.soups.resize(g.refs.size());
    for (std::size_t r = 0; r < g.refs.size(); ++r) {
        const BakedPrimitive& b = *g.refs[r];
        if (b.skinned) {
            continue;
        }
        PrimitiveView view;
        viewPrimitive(g.bakedMeshes, b, view);
        if (view.positions == nullptr || view.indexData == nullptr || view.indices < 3) {
            continue;
        }
        const unsigned real = view.indices / 3 * 3;
        const unsigned corners = (real + 11) / 12 * 12;
        auto soup = std::make_unique<Soup>();
        soup->corners = corners;
        std::copy_n(view.lo, 3, soup->lo);
        std::copy_n(view.hi, 3, soup->hi);
        soup->uvLo = view.uvLo;
        soup->uvHi = view.uvHi;
        soup->pos = static_cast<u16*>(memalign(16, std::size_t{corners} * 8));
        soup->uv = static_cast<u16*>(memalign(16, std::size_t{corners} * 4));
        soup->normal = static_cast<std::int8_t*>(memalign(16, std::size_t{corners} * 4));
        const bool hasVc = view.colors != nullptr || view.alphas != nullptr;
        if (hasVc) {
            soup->vc = static_cast<u8*>(memalign(16, std::size_t{corners} * 4));
        }
        if (view.lightUvs) {
            soup->luv = static_cast<u16*>(memalign(16, std::size_t{corners} * 4));
        }
        if (!soup->pos || !soup->uv || !soup->normal || (hasVc && !soup->vc) || (view.lightUvs && !soup->luv)) {
            continue;  // out of memory: the primitive is not drawn
        }
        const auto* index16 = static_cast<const std::uint16_t*>(view.indexData);
        const auto* index32 = static_cast<const std::uint32_t*>(view.indexData);
        for (unsigned c = 0; c < corners; ++c) {
            const unsigned source = c < real ? c : real - 1;
            const std::uint32_t v = view.wideIndices ? index32[source] : index16[source];
            if (v >= view.vertices) {
                continue;
            }
            soup->pos[c * 4] = view.positions[v * 3];
            soup->pos[c * 4 + 1] = view.positions[v * 3 + 1];
            soup->pos[c * 4 + 2] = view.positions[v * 3 + 2];
            soup->pos[c * 4 + 3] = 0;
            soup->uv[c * 2] = view.uvs ? view.uvs[v * 2] : 0;
            soup->uv[c * 2 + 1] = view.uvs ? view.uvs[v * 2 + 1] : 0;
            for (int k = 0; k < 3; ++k) {
                soup->normal[c * 4 + static_cast<unsigned>(k)] = view.normals ? view.normals[v * 3 + static_cast<unsigned>(k)] : (k == 2 ? 127 : 0);
            }
            soup->normal[c * 4 + 3] = 0;
            if (soup->vc) {
                for (int k = 0; k < 3; ++k) {
                    soup->vc[c * 4 + static_cast<unsigned>(k)] = view.colors ? view.colors[v * 3 + static_cast<unsigned>(k)] : 255;
                }
                soup->vc[c * 4 + 3] = view.alphas ? view.alphas[v] : 255;
            }
            if (soup->luv) {
                soup->luv[c * 2] = view.lightUvs[v * 2];
                soup->luv[c * 2 + 1] = view.lightUvs[v * 2 + 1];
            }
        }
        g.soups[r] = std::move(soup);
    }
    FlushCache(0);  // the DMA reads the soup arrays from memory

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
    waitVuIdle();
    vramList_.clear();  // the textures in VRAM belong to the scene that is going
    vramHead_ = vramBegin_;
    litVu_.clear();
    litVuBytes_ = 0;
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

} // namespace

namespace {

constexpr unsigned kHeaderQw = 8;    // the GS state packet (7 quadwords) and the vertices' GIF tag
constexpr unsigned kBatchCorners = 156;  // corners a batch: the header and three quadwords each fill one of two buffers in the VU memory
constexpr unsigned kBufferQw = 476;      // kHeaderQw + 3 * kBatchCorners

enum : std::uint32_t {
    kVifNop = 0x00, kVifStcycl = 0x01, kVifFlusha = 0x13, kVifMscal = 0x14,
    kUnpackV2_16 = 0x65, kUnpackV4_32 = 0x6C, kUnpackV4_16 = 0x6D, kUnpackV4_8 = 0x6E,
};

std::uint64_t giftag(std::uint64_t nloop, std::uint64_t eop, std::uint64_t pre, std::uint64_t prim, std::uint64_t flg, std::uint64_t nreg) {
    return nloop | (eop << 15) | (pre << 46) | (prim << 47) | (flg << 58) | (nreg << 60);
}

std::uint32_t vifCode(std::uint32_t imm, std::uint32_t num, std::uint32_t cmd) {
    return imm | (num << 16) | (cmd << 24);
}

std::uint32_t unpackAt(std::uint32_t address, bool usn, bool tops) {
    return address | (usn ? 1u << 14 : 0u) | (tops ? 1u << 15 : 0u);
}

std::uint64_t dmaRef(const void* data, unsigned qwc) {
    return qwc | (3ull << 28) | (static_cast<std::uint64_t>(reinterpret_cast<std::uint32_t>(data) & 0x0FFFFFFF) << 32);
}

} // namespace

void GsRenderer::initVu() {
    dma_channel_initialize(DMA_CHANNEL_VIF1, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_VIF1);
    const u32 size = packet2_utils_get_packet_size_for_program(&VU1Soup_CodeStart, &VU1Soup_CodeEnd) + 1;
    packet2_t* p = packet2_create(size, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
    packet2_vif_add_micro_program(p, 0, &VU1Soup_CodeStart, &VU1Soup_CodeEnd);
    packet2_utils_vu_add_end_tag(p);
    dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    packet2_free(p);
    p = packet2_create(1, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
    packet2_utils_vu_add_double_buffer(p, 16, kBufferQw);
    packet2_utils_vu_add_end_tag(p);
    dma_channel_send_packet2(p, DMA_CHANNEL_VIF1, 1);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    packet2_free(p);
    vramBegin_ = (static_cast<std::uint32_t>(gs_->CurrentPointer) + 8191) / 8192 * 8192;
    vramEnd_ = 4 * 1024 * 1024;
    vramHead_ = vramBegin_;
    slots_[0] = std::make_unique<VuSlot>();
    slots_[1] = std::make_unique<VuSlot>();
}

// Spins until the VIF1 has nothing to do, the VU1 is not running and the GIF has taken everything: the GS then has all that was drawn.
void GsRenderer::waitVuIdle() const {
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    const volatile std::uint32_t* vifStat = reinterpret_cast<const volatile std::uint32_t*>(0x10003C00);
    const volatile std::uint32_t* vpuStat = reinterpret_cast<const volatile std::uint32_t*>(0x10003800);
    const volatile std::uint32_t* gifStat = reinterpret_cast<const volatile std::uint32_t*>(0x10003020);
    for (;;) {
        const bool vifIdle = (*vifStat & 0x3) == 0;
        const bool vuIdle = (*vpuStat & 0x100) == 0;
        const bool gifIdle = (*gifStat & (3u << 10)) == 0 && ((*gifStat >> 24) & 0x1F) == 0;
        if (vifIdle && vuIdle && gifIdle) {
            return;
        }
    }
}

void GsRenderer::drawListVu(const FrameView& f, const std::vector<Draw>& list, int blend) {
    for (const Draw& d : list) {
        const GpuPrimitive& p = *d.primitive;
        if (p.skinned || !gpu_->baked || p.firstVertex >= gpu_->soups.size()) {
            continue;
        }
        const Soup* soup = gpu_->soups[p.firstVertex].get();
        if (soup == nullptr) {
            continue;
        }
        const scene::Material* m = d.material;
        const DrawUniforms u = FramePlan::uniformsFor(*this, *this, f, d, blend);
        const VertexUniforms& vu = u.vertex.frame;
        const FragmentUniforms& fu = u.fragment;
        ++frame_.draws;
        VuSlot& slot = *slots_[slot_];
        const unsigned corners = soup->corners;

        // The corners' lit colours (r, g, b and half the alpha, as the GS reads them): kept while the lights, place and material stay as they are.
        double t0 = now();
        const u8* colours = nullptr;
        {
            std::uint64_t hash = hashWords(reinterpret_cast<const std::uint8_t*>(&vu) + offsetof(VertexUniforms, model), sizeof vu - offsetof(VertexUniforms, model));
            hash = hashWords(&fu, sizeof fu, hash);
            const std::uint64_t key = (static_cast<std::uint64_t>(d.instance - instances_.data()) << 32) | p.firstVertex;
            auto found = litVu_.find(key);
            frame_.vertices += corners;
            if (found != litVu_.end() && found->second->hash == hash && found->second->corners == corners) {
                colours = found->second->rgba;
                ++frame_.litHits;
            } else {
                ++frame_.litMisses;
                if (found != litVu_.end()) {
                    litVuBytes_ -= std::size_t{found->second->corners} * 4;
                    litVu_.erase(found);
                }
                u8* destination = nullptr;
                std::unique_ptr<VuColors> keep;
                if (litVuBytes_ + std::size_t{corners} * 4 <= litVuBudget_) {
                    keep = std::make_unique<VuColors>();
                    keep->hash = hash;
                    keep->corners = corners;
                    keep->rgba = static_cast<u8*>(memalign(16, std::size_t{corners} * 4));
                    destination = keep->rgba;
                }
                if (destination == nullptr) {
                    keep.reset();
                    if (slot.scratchCapacity < std::size_t{corners} * 4) {
                        std::free(slot.scratch);
                        slot.scratchCapacity = std::size_t{corners} * 4;
                        slot.scratch = static_cast<u8*>(memalign(16, slot.scratchCapacity));
                    }
                    destination = slot.scratch;
                }
                if (destination == nullptr) {
                    continue;
                }
                const scene::Image* lightImage = nullptr;
                const bool lightMapped = fu.lightMap[0] > 0;
                if (lightMapped) {
                    if (const auto image = gpu_->lightImages.find(d.instance->lightMap); image != gpu_->lightImages.end()) {
                        lightImage = &image->second;
                    }
                }
                constexpr unsigned kChunk = 2048;
                rows_.resize(std::size_t{kChunk} * kVertexFloats);
                const float range[3] = {(soup->hi[0] - soup->lo[0]) / 65535.0f, (soup->hi[1] - soup->lo[1]) / 65535.0f, (soup->hi[2] - soup->lo[2]) / 65535.0f};
                const float uvRange = (soup->uvHi - soup->uvLo) / 65535.0f;
                for (unsigned start = 0; start < corners; start += kChunk) {
                    const unsigned n = std::min(kChunk, corners - start);
                    for (unsigned i = 0; i < n; ++i) {
                        const unsigned c = start + i;
                        float* row = &rows_[std::size_t{i} * kVertexFloats];
                        for (int k = 0; k < 3; ++k) {
                            row[k] = soup->lo[k] + range[k] * soup->pos[c * 4 + static_cast<unsigned>(k)];
                            row[3 + k] = soup->normal[c * 4 + static_cast<unsigned>(k)] * (1.0f / 127.0f);
                        }
                        row[6] = soup->uvLo + uvRange * soup->uv[c * 2];
                        row[7] = soup->uvLo + uvRange * soup->uv[c * 2 + 1];
                        row[8] = soup->vc ? soup->vc[c * 4 + 3] * (1.0f / 255.0f) : 1.0f;
                        row[9] = soup->luv ? soup->luv[c * 2] * (1.0f / 65535.0f) : 0.0f;
                        row[10] = soup->luv ? soup->luv[c * 2 + 1] * (1.0f / 65535.0f) : 0.0f;
                        for (int k = 0; k < 3; ++k) {
                            row[11 + k] = soup->vc ? soup->vc[c * 4 + static_cast<unsigned>(k)] * (1.0f / 255.0f) : 1.0f;
                        }
                        row[14] = row[15] = row[16] = 0;
                    }
                    // The GS reads a vertex colour of 0x80 as 1 and doubles up to 0xFF, as the texture environment scale of the OpenGL 1 path.
                    vertexColors(vu, fu, rows_.data(), n, lightMapped && lightImage == nullptr ? std::min(1.0f, kBlendedLightMapShare * fu.lightMap[0]) : 1.0f, 0.5f,
                                 lightImage, nullptr, litColors_);
                    for (unsigned i = 0; i < n; ++i) {
                        u8* out = destination + std::size_t{start + i} * 4;
                        out[0] = litColors_[std::size_t{i} * 4];
                        out[1] = litColors_[std::size_t{i} * 4 + 1];
                        out[2] = litColors_[std::size_t{i} * 4 + 2];
                        out[3] = static_cast<u8>(litColors_[std::size_t{i} * 4 + 3] >> 1);
                    }
                }
                SyncDCache(destination, destination + std::size_t{corners} * 4);
                colours = destination;
                if (keep) {
                    litVuBytes_ += std::size_t{corners} * 4;
                    litVu_[key] = std::move(keep);
                }
            }
        }
        frame_.light += now() - t0;
        t0 = now();

        // The texture. A texture that was not in VRAM is uploaded through gsKit's queue, which must not run while the VU1 is drawing.
        GsTexture* texture = nullptr;
        if (u.samplers[0].texture.kind == TextureRef::Kind::Image) {
            texture = textureFor(u.samplers[0].texture.index);
        }
        const unsigned ctx = gs_->PrimContext;
        // A texture not in VRAM is given a place there (letting go of what it overlaps) and uploaded at the head of this draw's chain, through
        // the VIF behind a FLUSHA, so that it goes after the draws before it and the EE does not wait.
        bool upload = false;
        if (texture && !texture->resident) {
            const auto aligned = [](unsigned n) { return (n + 255) / 256 * 256; };
            const unsigned textureBytes = aligned(static_cast<unsigned>(gsKit_texture_size(texture->tex.Width, texture->tex.Height, texture->tex.PSM)));
            const unsigned clutBytes = texture->clut ? aligned(texture->clutQw * 16) : 0;
            const unsigned need = textureBytes + clutBytes;
            if (vramHead_ + need > vramEnd_) {
                vramHead_ = vramBegin_;
            }
            for (auto it = vramList_.begin(); it != vramList_.end();) {
                GsTexture* other = *it;
                if (other->tex.Vram < vramHead_ + need && vramHead_ < other->tex.Vram + other->vramBytes) {
                    other->resident = false;
                    it = vramList_.erase(it);
                } else {
                    ++it;
                }
            }
            texture->tex.Vram = vramHead_;
            texture->tex.VramClut = texture->clut ? vramHead_ + textureBytes : 0;
            texture->vramBytes = need;
            texture->resident = true;
            vramList_.push_back(texture);
            vramHead_ += need;
            upload = true;
            ++frame_.uploads;
        }
        const bool additive = blend && m && m->alpha == scene::AlphaMode::Add;
        const bool blended = blend != 0;
        const bool alphaTest = fu.alphaCutoff > 0;
        const bool clamped = u.samplers[0].sampler == SamplerKind::Clamped;
        const unsigned reference = alphaTest ? static_cast<unsigned>(std::clamp(fu.alphaCutoff * 128.0f, 0.0f, 255.0f)) : 0;
        Qw* header = slot.header;
        header[0] = {giftag(6, 0, 0, 0, 0, 1), 0xE};
        if (texture) {
            const GSTEXTURE& t = texture->tex;
            const bool hasClut = t.Clut != nullptr;
            header[1] = {GS_SETREG_TEX0(t.Vram / 256, t.TBW, t.PSM, log2Of(t.Width), log2Of(t.Height), 1, 0, hasClut ? t.VramClut / 256 : 0, t.ClutPSM, 0, 0,
                                        hasClut ? GS_CLUT_STOREMODE_LOAD : GS_CLUT_STOREMODE_NOLOAD),
                         GS_TEX0_1 + ctx};
        } else {
            header[1] = {0, GS_TEX0_1 + ctx};
        }
        header[2] = {GS_SETREG_TEX1(1, 0, 1, 1, 0, 0, 0), GS_TEX1_1 + ctx};  // linear, no mip levels yet
        header[3] = {GS_SETREG_CLAMP(clamped ? 1 : 0, clamped ? 1 : 0, 0, 0, 0, 0), GS_CLAMP_1 + ctx};
        header[4] = {additive ? GS_SETREG_ALPHA(0, 2, 0, 1, 0) : GS_SETREG_ALPHA(0, 1, 0, 1, 0), GS_ALPHA_1 + ctx};
        header[5] = {GS_SETREG_TEST(alphaTest ? 1 : 0, 5, reference, 0, 0, 0, 1, 2), GS_TEST_1 + ctx};
        header[6] = {GS_SETREG_ZBUF_1(gs_->ZBuffer / 8192, gs_->PSMZ, blended ? 1 : 0), GS_ZBUF_1};
        header[7] = {0, 0};
        const std::uint64_t prim = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, texture ? 1 : 0, 0, blended ? 1 : 0, 0, 0, ctx, 0);
        const unsigned fullBatches = corners / kBatchCorners, remainder = corners % kBatchCorners;
        slot.tags[0] = {giftag(kBatchCorners, 1, 1, prim, 0, 3), 0x512};
        slot.tags[1] = {giftag(remainder, 1, 1, prim, 0, 3), 0x512};

        // The constants: the matrix with the positions' range folded in, and the screen, texture-coordinate and clip scales.
        const Mat4 mvp = f.viewProjection * fromRows(vu.model);
        const auto& r = mvp.m;
        float* c = slot.constants;
        for (int k = 0; k < 3; ++k) {
            const float scale = (soup->hi[k] - soup->lo[k]) / 65535.0f;
            for (int row = 0; row < 4; ++row) {
                c[k * 4 + row] = r[static_cast<std::size_t>(row * 4 + k)] * scale;
            }
        }
        for (int row = 0; row < 4; ++row) {
            c[12 + row] = r[static_cast<std::size_t>(row * 4)] * soup->lo[0] + r[static_cast<std::size_t>(row * 4 + 1)] * soup->lo[1] +
                          r[static_cast<std::size_t>(row * 4 + 2)] * soup->lo[2] + r[static_cast<std::size_t>(row * 4 + 3)];
        }
        const float halfW = static_cast<float>(gs_->Width) * 0.5f, halfH = static_cast<float>(gs_->Height) * 0.5f;
        const float originX = static_cast<float>(gs_->OffsetX) / 16.0f, originY = static_cast<float>(gs_->OffsetY) / 16.0f;
        const float uvScale = (soup->uvHi - soup->uvLo) / 65535.0f;
        const float tables[5][4] = {{halfW, -halfH, kZScale, 0}, {originX + halfW, originY + halfH, 0, 0}, {uvScale, uvScale, 0, 0}, {soup->uvLo, soup->uvLo, 0, 0},
                                    {1.0f / 3, 1.0f / 3, 1, 1}};
        std::memcpy(&c[16], tables, sizeof tables);

        // The chain: the constants, then for each batch the header and tag, the three arrays, and the start of the program.
        const std::size_t tagCount = 8 + std::size_t{fullBatches + 1} * 6;
        if (slot.chainCapacity < tagCount) {
            std::free(slot.chain);
            slot.chainCapacity = tagCount * 2;
            slot.chain = static_cast<VifTag*>(memalign(64, slot.chainCapacity * sizeof(VifTag)));
        }
        if (slot.chain == nullptr) {
            slot.chainCapacity = 0;
            continue;
        }
        unsigned t_ = 0;
        VifTag* chain = slot.chain;
        if (upload) {
            const auto direct = [](unsigned qw) { return vifCode(qw, 0, 0x50); };
            const GSTEXTURE& t = texture->tex;
            Qw* uq = slot.upload;
            uq[0] = {giftag(4, 0, 0, 0, 0, 1), 0xE};
            uq[1] = {GS_SETREG_BITBLTBUF(0, 0, 0, t.Vram / 256, t.TBW, t.PSM), GS_BITBLTBUF};
            uq[2] = {GS_SETREG_TRXPOS(0, 0, 0, 0, 0), GS_TRXPOS};
            uq[3] = {GS_SETREG_TRXREG(t.Width, t.Height), GS_TRXREG};
            uq[4] = {GS_SETREG_TRXDIR(0), GS_TRXDIR};
            uq[5] = {giftag(texture->texelQw, 0, 0, 0, 2, 0), 0};
            chain[t_++] = {1ull << 28, vifCode(0, 0, kVifNop), vifCode(0, 0, kVifFlusha)};
            chain[t_++] = {dmaRef(uq, 6), vifCode(0, 0, kVifNop), direct(6)};
            chain[t_++] = {dmaRef(texture->texels, texture->texelQw), vifCode(0, 0, kVifNop), direct(texture->texelQw)};
            if (texture->clut) {
                uq[6] = {giftag(4, 0, 0, 0, 0, 1), 0xE};
                uq[7] = {GS_SETREG_BITBLTBUF(0, 0, 0, t.VramClut / 256, 1, t.ClutPSM), GS_BITBLTBUF};
                uq[8] = {GS_SETREG_TRXPOS(0, 0, 0, 0, 0), GS_TRXPOS};
                uq[9] = {GS_SETREG_TRXREG(texture->clutWidth, texture->clutHeight), GS_TRXREG};
                uq[10] = {GS_SETREG_TRXDIR(0), GS_TRXDIR};
                uq[11] = {giftag(texture->clutQw, 0, 0, 0, 2, 0), 0};
                chain[t_++] = {dmaRef(&uq[6], 6), vifCode(0, 0, kVifNop), direct(6)};
                chain[t_++] = {dmaRef(texture->clut, texture->clutQw), vifCode(0, 0, kVifNop), direct(texture->clutQw)};
            }
            uq[12] = {giftag(1, 1, 0, 0, 0, 1), 0xE};
            uq[13] = {0, GS_TEXFLUSH};
            chain[t_++] = {dmaRef(&uq[12], 2), vifCode(0, 0, kVifNop), direct(2)};
            SyncDCache(uq, uq + 14);
        }
        chain[t_++] = {dmaRef(slot.constants, 9), vifCode(0x0101, 0, kVifStcycl), vifCode(unpackAt(0, false, false), 9, kUnpackV4_32)};
        for (unsigned b = 0; b <= fullBatches; ++b) {
            const unsigned n = b < fullBatches ? kBatchCorners : remainder;
            if (n == 0) {
                break;
            }
            const unsigned first = b * kBatchCorners;
            chain[t_++] = {dmaRef(header, 7), vifCode(0x0101, 0, kVifStcycl), vifCode(unpackAt(0, false, true), 7, kUnpackV4_32)};
            chain[t_++] = {dmaRef(&slot.tags[b < fullBatches ? 0 : 1], 1), vifCode(0, 0, kVifNop), vifCode(unpackAt(7, false, true), 1, kUnpackV4_32)};
            chain[t_++] = {dmaRef(&soup->pos[first * 4], n / 2), vifCode(3 | (1 << 8), 0, kVifStcycl), vifCode(unpackAt(kHeaderQw + 2, true, true), n, kUnpackV4_16)};
            chain[t_++] = {dmaRef(&soup->uv[first * 2], n / 4), vifCode(0, 0, kVifNop), vifCode(unpackAt(kHeaderQw + 0, true, true), n, kUnpackV2_16)};
            chain[t_++] = {dmaRef(&colours[first * 4], n / 4), vifCode(0, 0, kVifNop), vifCode(unpackAt(kHeaderQw + 1, true, true), n, kUnpackV4_8)};
            chain[t_++] = {1ull << 28, vifCode(0, 0, kVifNop), vifCode(0, 0, kVifMscal)};
            ++frame_.vuBatches;
        }
        chain[t_++] = {7ull << 28, vifCode(0, 0, kVifNop), vifCode(0, 0, kVifNop)};
        frame_.vuCorners += corners;
        SyncDCache(slot.header, slot.header + 8);
        SyncDCache(slot.tags, slot.tags + 2);
        SyncDCache(slot.constants, slot.constants + 36);
        SyncDCache(chain, chain + t_);
        frame_.vuBuild += now() - t0;
        t0 = now();
        dma_channel_wait(DMA_CHANNEL_VIF1, 0);  // the DMA is one chain at a time: the last draw's must have been read
        dma_channel_send_chain(DMA_CHANNEL_VIF1, chain, 0, DMA_FLAG_TRANSFERTAG, 0);
        frame_.vuWait += now() - t0;
        slot_ ^= 1;
    }
}

bool GsRenderer::draw(const Camera& camera) {
    frame_ = {};
    ++frameNumber_;
    const FrameView f = FrameView::make(camera, gs_->Width, gs_->Height);
    gsKit_clear(gs_, GS_SETREG_RGBAQ(static_cast<int>(clear_[0] * 255), static_cast<int>(clear_[1] * 255), static_cast<int>(clear_[2] * 255), 0x80, 0));
    // The clear goes out and is done before anything is kicked from the VU1 (two paths into the GIF are not ordered against each other).
    gsKit_queue_exec(gs_);
    dmaKit_wait_fast();
    gsKit_finish();
    if (drawScene_ && gpu_) {
        std::vector<Draw> opaque, layers, blended;
        const double c0 = now();
        FramePlan::collectDraws(*this, *this, f, opaque, layers, blended);
        frame_.collect = now() - c0;
        // Opaque draws may come in any order (the Z buffer sorts them): by texture, so that each is uploaded once.
        const auto imageOf = [](const Draw& d) { return d.material ? d.material->image : -1; };
        std::stable_sort(opaque.begin(), opaque.end(), [&](const Draw& a, const Draw& b) { return a.layer != b.layer ? a.layer < b.layer : imageOf(a) < imageOf(b); });
        drawListVu(f, opaque, 0);
        drawListVu(f, layers, 1);
        drawListVu(f, blended, 1);
        waitVuIdle();
    }
    frame_.textureBytes = gpu_ ? gpu_->resident : 0;
    frame_.litCacheKiB = litCacheBytes_ / 1024;
    const double s0 = now();
    gsKit_queue_exec(gs_);
    gsKit_sync_flip(gs_);
    frame_.submit = now() - s0;
    return true;
}

} // namespace ts2::render
