#include "console_lot.hpp"

#include "console_life.hpp"
#include "diag/log.hpp"
#include "lot_view.hpp"
#include "orbit.hpp"
#include "platform/platform.hpp"
#include "render/console_scene.hpp"
#include "render/ps2/gs_renderer.hpp"
#include "scene/view.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/route_grid.hpp"
#include "sim/support_table.hpp"

#include <dmaKit.h>
#include <gsKit.h>
#include <kernel.h>

#include <malloc.h>

#include <algorithm>
#include <cstdio>
#include <new>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Where the program is and what stopped it, for tools/ps2test/pine_sym.sh (a log line can be lost on the console; memory can be read).
extern "C" {
volatile unsigned g_oomArena = 0, g_oomUsed = 0, g_oomFree = 0, g_oomLargest = 0, g_oomStage = 0, g_consoleStopAt = 0, g_consoleStage = 0, g_consoleFrame = 0, g_consoleTicks = 0, g_consoleHeap = 0;
char g_consoleError[256] = {0};
}

#ifdef NEOTS2_PS2_DEBUG
extern "C" void ts2StartPcSampler();
extern "C" void ts2StartMonitor();
#endif

// Stops the program here when asked to (the third argument), with its memory intact, to read the globals above.
#define CONSOLE_STAGE(n) \
    do {                 \
        g_consoleStage = (n); \
        if (g_consoleStopAt == (n)) { \
            for (;;) {   \
            }            \
        }                \
    } while (0)

namespace app {

namespace {

// What the heap holds, to see where a 32 MB machine runs out.
void memory(const char* where) {
    const struct mallinfo m = mallinfo();
    std::printf("neots2: memory %-24s in use %6.2f MiB, heap %6.2f MiB\n", where, m.uordblks / 1048576.0, m.arena / 1048576.0);
}

// What the app itself needs of the scene is the instances' places, levels, parts and walls, the walls, the storeys and the floor's height: the pictures, materials, lights and
// per-instance light data are the renderer's, and it has its own copy of them.
void trimScene(ts2::scene::Scene& scene) {
    scene.images = {};
    scene.materials = {};
    scene.lightSets = {};
    scene.envCubes = {};
    scene.lightMaps.reset();
    scene.objectLighting.reset();
    scene.actors = {};
    scene.placedObjects = {};
    scene.thingLights = {};
    scene.thingSun = {};
    for (auto& instance : scene.instances) {
        instance.sun = {};
        instance.partLightSets = {};
        instance.name = {};
    }
}

// Only the storey being looked at is drawn (the rest would cost draw calls for what is hidden or cut away): the ground around the lot and the roofs stay as the
// view's own rules have them. An imposter for the other storeys' outsides is to come.
void keepCurrentStorey(const ts2::scene::Scene& scene, const ts2::scene::View& view, std::vector<std::uint8_t>& visible) {
    for (std::size_t i = 0; i < visible.size() && i < scene.instances.size(); ++i) {
        if (visible[i] == 0) {
            continue;
        }
        const auto& instance = scene.instances[i];
        if (instance.part == ts2::scene::Part::Other || instance.part == ts2::scene::Part::Roof || instance.part == ts2::scene::Part::Skirt) {
            continue;
        }
        if (instance.level != view.level) {
            visible[i] = 0;
        }
    }
}

// The lot's simulation from a bank folder simbank wrote, with the routes and animations the game's models gave when it was made.
std::unique_ptr<ts2::sim::LotSimulation> loadBank(const std::filesystem::path& bank) {
    std::ifstream cfg(bank / "sim.cfg");
    if (!cfg) {
        throw std::runtime_error("no sim.cfg in " + bank.string());
    }
    std::string line, lot, wants, tuning;
    std::uint32_t packs = 0;
    while (std::getline(cfg, line)) {
        if (line.rfind("packs=", 0) == 0) {
            packs = static_cast<std::uint32_t>(std::stoul(line.substr(6)));
        } else if (line.rfind("lot=", 0) == 0) {
            lot = line.substr(4);
        } else if (line.rfind("wants=", 0) == 0) {
            wants = line.substr(6);
        } else if (line.rfind("tuning=", 0) == 0) {
            tuning = line.substr(7);
        }
    }
    std::shared_ptr<const ts2::sim::SupportTable> support;
    if (std::ifstream in(bank / "support.bin", std::ios::binary); in) {
        const std::vector<char> data((std::istreambuf_iterator<char>(in)), {});
        support = std::make_shared<const ts2::sim::SupportTable>(
            ts2::sim::readSupportTable(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size())));
    }
    const auto optionalFile = [&](const std::string& name) {
        return name.empty() ? std::optional<std::filesystem::path>() : std::optional<std::filesystem::path>(bank / name);
    };
    auto simulation = std::make_unique<ts2::sim::LotSimulation>(std::vector<std::filesystem::path>{bank / "code.package"}, packs, bank / lot, optionalFile(wants),
                                                                optionalFile(tuning));
    if (support) {
        ts2::sim::installSupport(*simulation, support);
    }
    return simulation;
}

} // namespace

int runConsoleLot(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef NEOTS2_PS2_DEBUG
    ts2StartPcSampler();
    ts2StartMonitor();
#endif
    // An allocation that fails leaves what the heap looked like where PINE can read it (see g_oom*).
    std::set_new_handler([] {
        const auto mi = mallinfo();
        g_oomArena = static_cast<unsigned>(mi.arena);
        g_oomUsed = static_cast<unsigned>(mi.uordblks);
        g_oomFree = static_cast<unsigned>(mi.fordblks);
        g_oomLargest = static_cast<unsigned>(mi.keepcost);
        g_oomStage = g_consoleStage;
        throw std::bad_alloc();
    });
    ts2::sim::RouteGrid::finestSize = 0.25f;  // a fourth of the route grid's cells
    g_consoleStopAt = argc > 3 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)) : 0;
    // Without arguments (a launcher that does not pass them): the disc's folders, else beside the program (host:), where the demo archive keeps them.
    std::filesystem::path bank = argc > 1 ? argv[1] : "cdrom0:\\BANK";
    std::string scenePath = argc > 2 ? argv[2] : "cdrom0:\\LOT.CSC";
    if (argc <= 2) {
        if (std::FILE* probe = std::fopen(scenePath.c_str(), "rb")) {
            std::fclose(probe);
        } else {
            bank = "host:bank";
            scenePath = "host:lot.cscene";
        }
    }
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
    // The controller after the screen, as the viewer test program does it.
    if (!ts2::platform::init()) {
        std::printf("neots2: %s\n", ts2::platform::lastError().c_str());
        return 1;
    }
    try {
        // The baked scene: its head is read now, its pictures from the disc when they are first drawn.
        std::FILE* file = std::fopen(scenePath.c_str(), "rb");
        if (!file) {
            throw std::runtime_error("cannot open " + scenePath);
        }
        ts2::render::AlignedBytes head(ts2::render::kBakedPreambleSize);
        if (std::fread(head.data(), 1, head.size(), file) != head.size()) {
            throw std::runtime_error("short read of the preamble");
        }
        head.resize(ts2::render::bakedHeadSize(head));
        if (std::fread(head.data() + ts2::render::kBakedPreambleSize, 1, head.size() - ts2::render::kBakedPreambleSize, file) !=
            head.size() - ts2::render::kBakedPreambleSize) {
            throw std::runtime_error("short read of the head");
        }
        g_consoleStage = 1;
        ts2::render::BakedMeshes meshes;
        ts2::scene::Scene scene = ts2::render::readBakedHeadCompact(std::move(head), meshes);
        memory("scene head read");
        ts2::render::GsRenderer renderer(gs);
        renderer.setImageSource(
            [file](const ts2::scene::Image& image) {
                std::vector<std::uint8_t> blob(image.consoleSize);
                if (std::fseek(file, static_cast<long>(image.consoleOffset), SEEK_SET) != 0 || std::fread(blob.data(), 1, blob.size(), file) != blob.size()) {
                    return std::vector<std::uint8_t>{};
                }
                return blob;
            },
            2 * 1024 * 1024);
        renderer.setCacheBudgets(256 * 1024, 1536 * 1024);  // (the simulation has the rest of the heap)
        auto prepared = renderer.prepareBaked(scene, std::move(meshes));
        renderer.upload(std::move(prepared));
        memory("scene uploaded");
        // The lot's simulation.
        g_consoleStage = 2;
        const bool withSim = !(argc > 4 && std::string(argv[4]) == "nosim");  // (to see the scene alone: 4th argument)
        ConsoleLife life(withSim ? loadBank(bank) : nullptr, scene);
        g_consoleStage = 3;
        memory("simulation loaded");
        trimScene(scene);
        memory("scene trimmed");
        std::printf("neots2: %zu Sims, clock %s\n", life.simCount(), life.clockText().c_str());

        Orbit orbit;
        LotView lotView;
        CONSOLE_STAGE(10);
        lotView.start(scene, orbit);
        {
            // The storeys a Sim can stand on are those with floors: the level above the last of them may hold only an attic's walls and roof.
            int highestFloor = lotView.ground;
            for (const auto& instance : scene.instances) {
                if (instance.part == ts2::scene::Part::Floor && instance.level > highestFloor) {
                    highestFloor = instance.level;
                }
            }
            lotView.top = std::min(lotView.top, highestFloor);
            lotView.view.level = lotView.top;
            // For tests, which cannot press buttons: the storey to start on, and how far the camera starts (arguments 5 and 6, and 7 and 8 for the lot position it looks at).
            if (argc > 5) {
                lotView.view.level = std::clamp(static_cast<int>(std::strtol(argv[5], nullptr, 10)), lotView.ground, lotView.top);
            }
            if (argc > 6) {
                orbit.distance = std::strtof(argv[6], nullptr);
            }
            if (argc > 10) {
                lotView.setWallMode(static_cast<int>(std::strtol(argv[10], nullptr, 10)));  // (argument 10: 0 walls down, 1 cutaway, 2 up, 3 up with roofs)
            }
            if (argc > 8 && scene.floorHeight) {
                const float x = std::strtof(argv[7], nullptr), y = std::strtof(argv[8], nullptr);  // (and where the camera looks: a lot position)
                orbit.target = {x, y, scene.floorHeight(lotView.view.level, x, y) + 0.8f};
            }
        }
        CONSOLE_STAGE(11);
        std::uint64_t last = ts2::platform::ticksNs();
        unsigned frame = 0;
        // Where a frame's time goes (wall clock; in an emulator only the proportions mean anything): sums over `kProfileFrames` frames, in seconds.
        constexpr unsigned kProfileFrames = 60;
        double sumUpdate = 0, sumVisible = 0, sumDraw = 0, sumFrame = 0;
        ts2::render::GsRenderer::FrameStats sum;
        const auto seconds = [] { return static_cast<double>(ts2::platform::ticksNs()) / 1e9; };
        g_consoleStage = 4;
        for (bool running = true; running; ++frame) {
            g_consoleFrame = frame;
            g_consoleTicks = static_cast<unsigned>(life.ticks());
            g_consoleHeap = static_cast<unsigned>(mallinfo().uordblks);
            CONSOLE_STAGE(12);
            ts2::platform::Event e;
            while (ts2::platform::pollEvent(e)) {
                if (e.type == ts2::platform::Event::Type::KeyDown) {
                    if (e.key == ts2::platform::Key::Escape) {
                        running = false;
                    } else {
                        lotView.key(e.key, orbit);
                    }
                }
            }
            const std::uint64_t now = ts2::platform::ticksNs();
            const float dt = std::min(0.1f, static_cast<float>(now - last) / 1e9f);
            last = now;
            CONSOLE_STAGE(13);
            orbitStep(orbit, dt);
            lotView.view.facing = orbit.facing();
            CONSOLE_STAGE(14);
            const double tFrame = seconds();
            life.update(static_cast<double>(dt) * 1000, scene, renderer);
            const double tUpdated = seconds();
            CONSOLE_STAGE(15);
            if (argc > 9 && scene.floorHeight && life.hasSimulation()) {
                // For tests (argument 9): the camera follows this Sim.
                if (const auto* followed = life.simulation().world().find(static_cast<std::int16_t>(std::strtol(argv[9], nullptr, 10)));
                    followed != nullptr && followed->x != ts2::sim::kOffLot) {
                    const float x = static_cast<float>(followed->y) / 65536.0f, y = static_cast<float>(followed->x) / 65536.0f;
                    orbit.target = {x, y, scene.floorHeight(followed->level, x, y) + 0.8f};
                    lotView.view.level = std::clamp(static_cast<int>(scene.groundLevel + followed->level - 1), lotView.ground, lotView.top);
                }
            }
            const double tVisible0 = seconds();
            auto visible = ts2::scene::visibleInstances(scene, lotView.view);
            const auto before = static_cast<std::size_t>(std::count(visible.begin(), visible.end(), std::uint8_t{1}));
            keepCurrentStorey(scene, lotView.view, visible);
            if (frame == 0) {
                std::printf("neots2: level %d of %d..%d: %zu instances shown, %zu with the storey filter\n", lotView.view.level, lotView.ground, lotView.top, before,
                            static_cast<std::size_t>(std::count(visible.begin(), visible.end(), std::uint8_t{1})));
            }
            renderer.show(std::move(visible));
            CONSOLE_STAGE(16);
            if (frame == 2) {
                const auto cam = orbit.camera();
                std::printf("neots2: camera eye %.3f %.3f %.3f target %.3f %.3f %.3f fovX %.4f fovY %.4f far %.1f near %.4f\n", static_cast<double>(cam.eye[0]),
                            static_cast<double>(cam.eye[1]), static_cast<double>(cam.eye[2]), static_cast<double>(cam.target[0]), static_cast<double>(cam.target[1]),
                            static_cast<double>(cam.target[2]), static_cast<double>(cam.fovX), static_cast<double>(cam.fovY), static_cast<double>(cam.farPlane),
                            static_cast<double>(cam.nearPlane));
            }
            const double tDraw0 = seconds();
            if (!renderer.draw(orbit.camera())) {
                break;
            }
            const double tDrawn = seconds();
            sumUpdate += tUpdated - tFrame;
            sumVisible += tDraw0 - tVisible0;
            sumDraw += tDrawn - tDraw0;
            sumFrame += tDrawn - tFrame;
            {
                const auto& f = renderer.lastFrame();
                sum.collect += f.collect, sum.uniforms += f.uniforms, sum.light += f.light, sum.vuBuild += f.vuBuild, sum.vuWait += f.vuWait, sum.expand += f.expand;
                sum.transform += f.transform, sum.emit += f.emit, sum.state += f.state, sum.submit += f.submit;
                sum.eeCorners += f.eeCorners, sum.vuCorners += f.vuCorners, sum.draws += f.draws, sum.uploads += f.uploads, sum.clipped += f.clipped, sum.litHits += f.litHits, sum.litMisses += f.litMisses;
            }
            if (frame % kProfileFrames == kProfileFrames - 1) {
                const double n = kProfileFrames, ms = 1000.0 / n;
                std::printf("neots2: profile %u frames, %.1f ms a frame: sim update %.1f, visibility %.1f, draw %.1f (collect %.1f, uniforms %.1f, light %.1f, vu build %.1f (texture %.1f, ee clip %.1f), vu wait %.1f,"
                            " submit %.1f); %zu ee corners, %zu vu corners; %.0f draws, %.1f uploads, %.1f clipped, lit cache %zu hits %zu misses a frame\n",
                            kProfileFrames, sumFrame * ms, sumUpdate * ms, sumVisible * ms, sumDraw * ms, sum.collect * ms, sum.uniforms * ms, sum.light * ms, sum.vuBuild * ms, sum.state * ms, sum.transform * ms,
                            sum.vuWait * ms, sum.submit * ms, sum.eeCorners / kProfileFrames, sum.vuCorners / kProfileFrames, static_cast<double>(sum.draws) / n, static_cast<double>(sum.uploads) / n, static_cast<double>(sum.clipped) / n,
                            sum.litHits / kProfileFrames, sum.litMisses / kProfileFrames);
                sumUpdate = sumVisible = sumDraw = sumFrame = 0;
                sum = {};
            }
            CONSOLE_STAGE(17);
            if (frame % 120 == 0) {
                if (argc > 9 && life.hasSimulation()) {
                    if (const auto* followed = life.simulation().world().find(static_cast<std::int16_t>(std::strtol(argv[9], nullptr, 10))); followed != nullptr) {
                        std::printf("neots2: Sim %s at x %.2f y %.2f level %d; camera target %.2f %.2f %.2f\n", argv[9], static_cast<double>(followed->x) / 65536.0,
                                    static_cast<double>(followed->y) / 65536.0, static_cast<int>(followed->level), static_cast<double>(orbit.target[0]),
                                    static_cast<double>(orbit.target[1]), static_cast<double>(orbit.target[2]));
                    }
                }
                const auto& stats = renderer.lastFrame();
                std::printf("neots2: frame %u, %zu ticks, clock %s; %zu draws, %zu triangles, %zu drawn, %zu culled, %zu clipped, %zu vu batches, %zu textures read\n", frame,
                            life.ticks(), life.clockText().c_str(), stats.draws, stats.triangles, stats.drawn, stats.culled, stats.clipped, stats.vuBatches, stats.uploads);
            }
        }
    } catch (const std::exception& ex) {
        std::printf("neots2: %s\n", ex.what());
        std::snprintf(g_consoleError, sizeof g_consoleError, "%s", ex.what());
        g_consoleStage = 900;
    }
    ts2::platform::quit();
    std::printf("neots2: done\n");
    for (;;) {  // (a program that returns is unloaded and its memory cleared: see tools/ps2test/pine_sym.sh)
        SleepThread();
    }
    return 0;
}

} // namespace app
