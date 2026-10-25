// Shows a baked lot (see tools/ts2bake) on the PlayStation 2 with the gsKit renderer: loads the file from host:, draws it from a
// fixed camera for a few frames and prints what a frame cost.
#include "render/console_scene.hpp"
#include "render/ps2/gs_renderer.hpp"

#include <dmaKit.h>
#include <gsKit.h>
#include <kernel.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = argc > 1 ? argv[1] : "host:lot.cscene";
    GSGLOBAL* gs = gsKit_init_global();
    gs->Width = 640;
    gs->Height = 448;
    gs->PSM = GS_PSM_CT32;
    gs->PSMZ = GS_PSMZ_24;
    gs->ZBuffering = GS_SETTING_ON;
    gs->DoubleBuffering = GS_SETTING_ON;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::printf("gs_lotview: cannot open %s\n", path);
        SleepThread();
    }
    try {
        std::vector<std::uint8_t> head(ts2::render::kBakedPreambleSize);
        if (std::fread(head.data(), 1, head.size(), file) != head.size()) {
            throw std::runtime_error("short read of the preamble");
        }
        head.resize(ts2::render::bakedHeadSize(head));
        if (std::fread(head.data() + ts2::render::kBakedPreambleSize, 1, head.size() - ts2::render::kBakedPreambleSize, file) !=
            head.size() - ts2::render::kBakedPreambleSize) {
            throw std::runtime_error("short read of the head");
        }
        ts2::scene::Scene scene = ts2::render::readBakedHead(head);
        head = {};
        std::printf("gs_lotview: head read: %zu images, %zu meshes, %zu instances\n", scene.images.size(), scene.meshes.size(), scene.instances.size());
        ts2::render::GsRenderer renderer(gs);
        renderer.setImageSource(
            [file](const ts2::scene::Image& image) {
                std::vector<std::uint8_t> blob(image.consoleSize);
                if (std::fseek(file, static_cast<long>(image.consoleOffset), SEEK_SET) != 0 || std::fread(blob.data(), 1, blob.size(), file) != blob.size()) {
                    return std::vector<std::uint8_t>{};
                }
                return blob;
            },
            6 * 1024 * 1024);
        renderer.upload(renderer.prepare(scene));
        std::printf("gs_lotview: uploaded\n");
        ts2::render::Camera camera;
        const float cx = scene.lotSize[0] * 0.5f, cy = scene.lotSize[1] * 0.5f;
        camera.target = {cx, cy, 0};
        camera.eye = {cx + scene.lotSize[0] * 0.9f, cy - scene.lotSize[1] * 0.9f, scene.lotSize[0] * 0.8f};
        camera.farPlane = 400;
        for (int frame = 0; frame < 600; ++frame) {
            if (!renderer.draw(camera)) {
                break;
            }
            if (frame < 3 || frame % 100 == 0) {
                const auto& s = renderer.lastFrame();
                std::printf("gs_lotview: frame %d: %zu draws, %zu triangles, %zu drawn, %zu culled, %zu clipped, %zu textures read, %zu KiB resident\n", frame,
                            s.draws, s.triangles, s.drawn, s.culled, s.clipped, s.uploads, s.textureBytes / 1024);
            }
        }
    } catch (const std::exception& e) {
        std::printf("gs_lotview: %s\n", e.what());
    }
    std::printf("gs_lotview: done\n");
    SleepThread();
}
