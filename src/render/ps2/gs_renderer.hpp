// The PlayStation 2's renderer: carries out a frame plan with gsKit. A scene arrives baked (render/console_scene.hpp), its
// textures already 4-bit, 8-bit or 32-bit, and its vertices are lit and transformed on the CPU for now (the vector units are
// the way to do that fast; see docs/ps2-port-plan.md). Only built for the console.
#pragma once

#include "render/frame_plan.hpp"
#include "render/shared_state.hpp"

#include <gsKit.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace ts2::render {

struct GsTexture;

class GsRenderer final : public RendererState, private ResourceQuery {
public:
    // Draws to the screen `gs` has set up (see gs_renderer.cpp for the mode it expects).
    explicit GsRenderer(GSGLOBAL* gs);
    ~GsRenderer() override;

    [[nodiscard]] Capabilities capabilities() const override;
    [[nodiscard]] Prepared prepare(const scene::Scene& scene) const override;
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

    void drawList(const FrameView& f, const std::vector<Draw>& list, int blend);
    // The texture of scene image `index`, read and made when it is first drawn; null when there is none.
    GsTexture* textureFor(int index);

    ImageSource imageSource_;
    std::size_t textureBudget_ = 6 * 1024 * 1024;
    std::size_t frameNumber_ = 0;
    GSGLOBAL* gs_;
    std::unique_ptr<Gpu> gpu_;
    std::vector<std::uint8_t> litColors_;
    FrameStats frame_;
};

} // namespace ts2::render
