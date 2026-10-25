// Shows a baked lot (see tools/ts2bake) on the PlayStation 2 with the gsKit renderer: loads the file from host:, draws it from a
// fixed camera for a few frames and prints what a frame cost.
#include "render/console_scene.hpp"
#include "render/ps2/gs_renderer.hpp"

#include <dmaKit.h>
#include <libpad.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <gsKit.h>
#include <kernel.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <malloc.h>
#include <cstdlib>
#include <cstring>
#include <string>
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
    // The controller: the ROM's pad modules, port 1.
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, nullptr);
    SifLoadModule("rom0:PADMAN", 0, nullptr);
    padInit(0);
    static char padArea[256] __attribute__((aligned(64)));
    const bool padOpen = padPortOpen(0, 0, padArea) != 0;
    std::printf("gs_lotview: pad port %s\n", padOpen ? "open" : "failed");
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::printf("gs_lotview: cannot open %s\n", path);
        SleepThread();
    }
    try {
        ts2::render::AlignedBytes head(ts2::render::kBakedPreambleSize);
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
        const float zoom = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 1.0f;  // 1: the whole lot in view; smaller: closer
        camera.eye = {cx + scene.lotSize[0] * 0.9f * zoom, cy - scene.lotSize[1] * 0.9f * zoom, scene.lotSize[0] * 0.8f * zoom};
        camera.farPlane = 400;
        const float orbit = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 0.0f;  // radians a frame the camera turns about the lot
        const ts2::render::Vec3 eye0 = camera.eye;
        // "pad" as the last argument: run until the console is switched off, the camera on the controller (left stick turns and tilts, L1/R1 or the
        // d-pad's up and down move in and out). Otherwise four frames, for tests.
        const bool interactive = argc > 1 && std::string(argv[argc - 1]) == "pad";
        float turn = 0, height = 1.0f, distance = 1.0f;
        padButtonStatus pad;
        std::memset(&pad, 0, sizeof pad);
        for (int frame = 0; interactive || frame < 4; ++frame) {
            if (interactive && padOpen && padGetState(0, 0) == PAD_STATE_STABLE && padRead(0, 0, &pad) != 0) {
                const unsigned buttons = static_cast<unsigned>(pad.btns ^ 0xFFFF);  // active low
                const float stickX = (static_cast<int>(pad.ljoy_h) - 128) / 128.0f, stickY = (static_cast<int>(pad.ljoy_v) - 128) / 128.0f;
                turn += std::fabs(stickX) > 0.2f ? stickX * 0.03f : 0.0f;
                height = std::clamp(height - (std::fabs(stickY) > 0.2f ? stickY * 0.02f : 0.0f), 0.3f, 2.5f);
                if (buttons & (PAD_R1 | PAD_UP)) distance = std::max(0.25f, distance - 0.02f);
                if (buttons & (PAD_L1 | PAD_DOWN)) distance = std::min(2.0f, distance + 0.02f);
                const float c = std::cos(turn), sn = std::sin(turn);
                camera.eye = {cx + (eye0[0] - cx) * distance * c - (eye0[1] - cy) * distance * sn, cy + (eye0[0] - cx) * distance * sn + (eye0[1] - cy) * distance * c,
                              eye0[2] * distance * height};
            } else if (orbit != 0 && !interactive) {
                const float a = orbit * (argc > 4 ? 1.0f : static_cast<float>(frame)), c = std::cos(a), sn = std::sin(a);  // a fourth argument: a fixed angle
                camera.eye = {cx + (eye0[0] - cx) * c - (eye0[1] - cy) * sn, cy + (eye0[0] - cx) * sn + (eye0[1] - cy) * c, eye0[2]};
            }
            if (!renderer.draw(camera)) {
                break;
            }
            if (frame == 1) {
                const auto& v = renderer.lastFrame();
                std::printf("gs_lotview: vu: %zu batches, %zu corners; build %.2f s, send/wait %.2f s, uniforms %.2f s, lit lookup %.2f s\n", v.vuBatches, v.vuCorners, v.vuBuild, v.vuWait, v.uniforms, v.light);
                std::printf("gs_lotview: primitives clipped by the EE: %zu, its corners %zu\n", renderer.lastFrame().clipped, renderer.lastFrame().eeCorners);
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
