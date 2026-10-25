// The PlayStation 2's renderer: carries out a frame plan with gsKit. A scene arrives baked (render/console_scene.hpp), its
// textures already 4-bit, 8-bit or 32-bit, and its vertices are lit and transformed on the CPU for now (the vector units are
// the way to do that fast; see docs/ps2-port-plan.md). Only built for the console.
#pragma once

#include "render/console_scene.hpp"
#include "render/frame_plan.hpp"
#include "render/shared_state.hpp"

#include <gsKit.h>

#include <functional>
#include <map>
#include <unordered_map>
#include <memory>
#include <vector>

namespace ts2::render {

struct GsTexture;

// One vertex as the GS takes it in the GIF's packed format (its ST, RGBAQ and XYZ2 registers, a quadword each): a triangle is three of them.
struct alignas(16) GsVertex {
    float s, t, q;
    std::uint32_t unused;
    std::uint32_t r, g, b, a;  // 0x80 is full
    std::uint32_t x, y, z, adc;
};
static_assert(sizeof(GsVertex) == 48);

class GsRenderer final : public RendererState, private ResourceQuery {
public:
    // Draws to the screen `gs` has set up (see gs_renderer.cpp for the mode it expects).
    explicit GsRenderer(GSGLOBAL* gs);
    ~GsRenderer() override;

    [[nodiscard]] Capabilities capabilities() const override;
    [[nodiscard]] Prepared prepare(const scene::Scene& scene) const override;
    // A baked scene read with readBakedHeadCompact: its meshes stay as bytes and are expanded when drawn, so they take no more memory
    // than the file. `scene` is what that returned, `meshes` what it filled.
    [[nodiscard]] Prepared prepareBaked(scene::Scene scene, BakedMeshes meshes) const;
    void upload(Prepared prepared) override { (void)swap(std::move(prepared)); }
    [[nodiscard]] Prepared swap(Prepared prepared) override;
    [[nodiscard]] bool uploaded() override { return true; }
    void setSprites(const std::vector<std::pair<int, const scene::Image*>>&) override {}
    [[nodiscard]] bool draw(const Camera& camera) override;
    [[nodiscard]] bool drawToFile(const Camera&, unsigned, unsigned, const std::filesystem::path&) override { return false; }
    [[nodiscard]] std::optional<scene::Image> drawInstancesToImage(const Camera&, unsigned, unsigned, const scene::ObjectLighting&) override {
        return std::nullopt;
    }
    std::string backend() const override { return "gskit"; }
    std::string device() const override { return "Graphics Synthesizer"; }
    void benchmark(const Camera&, unsigned, unsigned, int) override {}
    void addMaterials(const scene::Scene&) override {}
    void addMeshes(const scene::Scene&) override {}
    void setImages(const scene::Scene&, const std::vector<int>&, bool) override {}
    void setLightMapFade(float) override {}
    void setSkybox(const std::vector<scene::Image>&) override {}

    // What changes the lit colours of an object: those that name it bump its own count, the others the count of all. A cached set of colours is kept
    // while the counts it was made with are the counts now (the camera does not light anything, so moving it does not discard them).
    void setPlace(std::size_t instance, const scene::Transform& transform) override { RendererState::setPlace(instance, transform); touch(instance); }
    void setMaterials(std::size_t instance, std::vector<int> materials) override { RendererState::setMaterials(instance, std::move(materials)); touch(instance); }
    void setInstance(const scene::Scene& scene, std::size_t index) override { RendererState::setInstance(scene, index); touch(index); }
    void setLights(const scene::Scene& scene, std::size_t index) override { RendererState::setLights(scene, index); touch(index); }
    void setImposterLight(const std::array<float, 3>& light) override { RendererState::setImposterLight(light); ++lightGen_; }
    void setNight(bool night) override { RendererState::setNight(night); ++lightGen_; }
    void setObjectLighting(std::optional<scene::ObjectLighting> lighting) override { RendererState::setObjectLighting(std::move(lighting)); ++lightGen_; }
    void setHighlight(std::uint32_t object, float ambient) override { RendererState::setHighlight(object, ambient); ++lightGen_; }

    // Where a scene's images come from when `Image::console` is empty (see readBakedHead): called with the image when a draw first needs
    // it. At most `textureBudget` bytes of textures are kept in memory; the least recently drawn are let go, and read again if drawn.
    using ImageSource = std::function<std::vector<std::uint8_t>(const scene::Image&)>;
    void setImageSource(ImageSource source, std::size_t textureBudget) {
        imageSource_ = std::move(source);
        textureBudget_ = textureBudget;
    }

    // What the last frame did, for a test program to print.
    struct FrameStats {
        std::size_t draws = 0, triangles = 0, drawn = 0, culled = 0, clipped = 0, uploads = 0, textureBytes = 0;
        // Seconds spent in each part of the frame (wall clock; in an emulator only their proportions mean anything).
        std::size_t vertices = 0, litHits = 0, litMisses = 0, litCacheKiB = 0;
        std::size_t vuBatches = 0, vuCorners = 0, vuWaits = 0, eeCorners = 0;
        double vuBuild = 0, vuWait = 0;
        double collect = 0, expand = 0, light = 0, transform = 0, emit = 0, state = 0, submit = 0;
    };
    [[nodiscard]] const FrameStats& lastFrame() const { return frame_; }

private:
    struct Gpu;
    struct GpuPrepared;

    // ResourceQuery
    [[nodiscard]] const std::vector<GpuMesh>& meshes() const override;
    [[nodiscard]] bool hasImage(int index) const override;
    [[nodiscard]] bool hasEnvCube(int) const override { return false; }
    [[nodiscard]] bool hasFadeFrom(int) const override { return false; }
    [[nodiscard]] float lightMapFade() const override { return 0; }
    [[nodiscard]] bool skinReady(std::uint32_t) const override { return false; }

    void drawListVu(const FrameView& f, const std::vector<Draw>& list, int blend);
    void initVu();
    void waitVuIdle() const;
    // The texture of scene image `index`, read and made when it is first drawn; null when there is none.
    GsTexture* textureFor(int index);

    ImageSource imageSource_;
    std::size_t textureBudget_ = 6 * 1024 * 1024;
    std::size_t frameNumber_ = 0;
    GSGLOBAL* gs_;
    std::unique_ptr<Gpu> gpu_;
    std::vector<std::uint8_t> litColors_;
    ExpandedPrimitive expanded_;
    // A draw's lit vertex colours, kept while the lights it depends on stay as they are: lighting does not follow the camera, so it is
    // worked out once. By instance and primitive.
    struct LitColors {
        std::uint64_t hash = 0;
        std::vector<std::uint8_t> rgba;
    };
    std::unordered_map<std::uint64_t, LitColors> litCache_;
    std::size_t litCacheBytes_ = 0, litCacheBudget_ = 5 * 1024 * 1024;
    std::vector<GsVertex> screen_;
    std::vector<float> screenXy_;
    std::vector<std::uint8_t> codes_;
    std::vector<std::uint32_t> visibleTris_, clippedTris_;
    // VRAM the textures share: a ring, in the space the frame and Z buffers left. A texture that overlaps what comes next is let go.
    std::uint32_t vramBegin_ = 0, vramEnd_ = 0, vramHead_ = 0;
    std::vector<GsTexture*> vramList_;
    struct VuSlot;
    struct VuColors;
    std::unique_ptr<VuSlot> slots_[2];
    std::unordered_map<std::uint64_t, std::unique_ptr<VuColors>> litVu_;
    std::uint64_t lightGen_ = 1;
    std::vector<std::uint32_t> instanceGen_;
    void touch(std::size_t instance) {
        if (instanceGen_.size() <= instance) {
            instanceGen_.resize(instance + 1, 0);
        }
        ++instanceGen_[instance];
    }
    std::size_t litVuBytes_ = 0, litVuBudget_ = 5 * 1024 * 1024;
    unsigned slot_ = 0;
    std::vector<float> rows_;  // vertex rows (see kVertexFloats) of a chunk being lit
    FrameStats frame_;
};

} // namespace ts2::render
