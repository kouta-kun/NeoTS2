// Shows a baked lot (see tools/ts2bake) on the PlayStation 2 with the gsKit renderer: loads the file from host:, draws it from a
// fixed camera for a few frames and prints what a frame cost.
#include "render/console_scene.hpp"
#include "render/ps2/gs_renderer.hpp"

#include <dmaKit.h>
#include <gsKit.h>
#include <kernel.h>

#include <cstdio>
#include <malloc.h>
#include <cstdlib>
#include <stdexcept>
#include <vector>

// What the heap holds and how far it has grown, to see where a 32 MB machine runs out.
static void memory(const char* where) {
    const struct mallinfo m = mallinfo();
    std::printf("gs_lotview: memory %-22s in use %6.2f MiB, free in heap %6.2f MiB, heap %6.2f MiB\n", where, m.uordblks / 1048576.0, m.fordblks / 1048576.0,
                m.arena / 1048576.0);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = argc > 1 ? argv[1] : "host:lot.cscene";
    GSGLOBAL* gs = gsKit_init_global();
    gs->Width = 640;
    gs->Height = 448;
    gs->PSM = GS_PSM_CT16;
    gs->Dithering = GS_SETTING_ON;
    gs->PSMZ = GS_PSMZ_24;
    gs->ZBuffering = GS_SETTING_ON;
    gs->DoubleBuffering = GS_SETTING_ON;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    {
        // How much can be allocated in all: take 1 MiB blocks until there are none (and give them back).
        std::vector<void*> blocks;
        while (void* b = std::malloc(1 << 20)) {
            blocks.push_back(b);
        }
        std::printf("gs_lotview: the heap holds %zu MiB in all\n", blocks.size());
        for (void* b : blocks) {
            std::free(b);
        }
    }
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
        memory("before the head");
        ts2::render::BakedMeshes meshes;
        ts2::scene::Scene scene = ts2::render::readBakedHeadCompact(std::move(head), meshes);
        memory("head read");
        std::printf("gs_lotview: head read: %zu images, %zu meshes, %zu instances\n", scene.images.size(), meshes.meshes.size(), scene.instances.size());
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
        memory("renderer made");
        auto prepared = renderer.prepareBaked(scene, std::move(meshes));
        memory("scene prepared");
        renderer.upload(std::move(prepared));
        std::printf("gs_lotview: VRAM left for textures: %.2f MiB\n", (4194304 - gs->CurrentPointer) / 1048576.0);
        memory("uploaded");
        std::printf("gs_lotview: uploaded\n");
        ts2::render::Camera camera;
        const float cx = scene.lotSize[0] * 0.5f, cy = scene.lotSize[1] * 0.5f;
        camera.target = {cx, cy, 0};
        camera.eye = {cx + scene.lotSize[0] * 0.9f, cy - scene.lotSize[1] * 0.9f, scene.lotSize[0] * 0.8f};
        camera.farPlane = 400;
        for (int frame = 0; frame < 3; ++frame) {
            if (!renderer.draw(camera)) {
                break;
            }
            if (frame == 1) {
                const auto& v = renderer.lastFrame();
                std::printf("gs_lotview: vu: %zu batches, %zu corners; build %.2f s, send/wait %.2f s\n", v.vuBatches, v.vuCorners, v.vuBuild, v.vuWait);
                std::printf("gs_lotview: %zu vertices in the draws, lit colours: %zu kept, %zu worked out, cache %zu KiB\n", renderer.lastFrame().vertices,
                            renderer.lastFrame().litHits, renderer.lastFrame().litMisses, renderer.lastFrame().litCacheKiB);
                const auto& s = renderer.lastFrame();
                std::printf("gs_lotview: phases (s): collect %.2f expand %.2f light %.2f transform %.2f state %.2f emit %.2f submit+sync %.2f\n", s.collect,
                            s.expand, s.light, s.transform, s.state, s.emit, s.submit);
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
