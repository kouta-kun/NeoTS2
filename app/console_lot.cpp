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
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
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
// What the program has printed or traced, the last 4 KB of it, in memory: ps2client dumpmem can read it from a console that no longer prints anything (tools/ps2test/ps2_dump.sh).
// The text is at g_traceRing[g_traceTotal % 4096] and after it, then from the start to there, once g_traceTotal passes 4096.
char g_traceRing[4096] = {0};
volatile unsigned g_traceTotal = 0;
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
    logf("neots2: memory %-24s in use %6.2f MiB, heap %6.2f MiB\n", where, m.uordblks / 1048576.0, m.arena / 1048576.0);
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
GSGLOBAL* g_gs = nullptr;
const char* g_loadText = "";

// A 5 x 7 font, as columns (bit 0 the top row), for the characters from space to Z: the console has no font of its own to count on.
constexpr std::uint8_t kFont[59][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08}, {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

// `text` (capitals; anything else is shown as a capital or a question mark) with its left edge at x and its top at y, each dot `scale` pixels, in runs of dots to keep the sprites few.
void drawText(float x, float y, float scale, const char* text, std::uint64_t colour) {
    for (; *text != 0; ++text, x += 6 * scale) {
        int c = static_cast<unsigned char>(*text);
        c = c >= 'a' && c <= 'z' ? c - 32 : c;
        const auto& glyph = kFont[c >= 0x20 && c <= 0x5A ? c - 0x20 : '?' - 0x20];
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[col] >> row & 1) == 0) {
                    continue;
                }
                int end = col;
                while (end + 1 < 5 && (glyph[end + 1] >> row & 1) != 0) {
                    ++end;
                }
                gsKit_prim_sprite(g_gs, x + col * scale, y + row * scale, x + (end + 1) * scale, y + (row + 1) * scale, 4, colour);
                col = end;
            }
        }
    }
}

// What the screen shows while the program loads (nothing else is drawn until the first frame, and a screen nobody draws on shows what was in the video memory): what it is doing and a bar.
void loadingScreen(float fraction, const char* text = nullptr) {
    if (g_gs == nullptr) {
        return;
    }
    if (text != nullptr) {
        g_loadText = text;
    }
    const float w = static_cast<float>(g_gs->Width), h = static_cast<float>(g_gs->Height);
    const float left = w * 0.2f, right = w * 0.8f, top = h * 0.5f, bottom = h * 0.56f;
    const float filled = left + (right - left) * std::clamp(fraction, 0.0f, 1.0f);
    gsKit_clear(g_gs, GS_SETREG_RGBAQ(0x14, 0x14, 0x24, 0x80, 0));
    gsKit_prim_sprite(g_gs, left - 4, top - 4, right + 4, bottom + 4, 1, GS_SETREG_RGBAQ(0xC0, 0xC0, 0xC0, 0x80, 0));
    gsKit_prim_sprite(g_gs, left, top, right, bottom, 2, GS_SETREG_RGBAQ(0x14, 0x14, 0x24, 0x80, 0));
    gsKit_prim_sprite(g_gs, left, top, filled, bottom, 3, GS_SETREG_RGBAQ(0x40, 0xA0, 0xE0, 0x80, 0));
    drawText(left, h * 0.5f - 66, 3, "NEOTS2", GS_SETREG_RGBAQ(0xE0, 0xE0, 0xE0, 0x80, 0));
    drawText(left, h * 0.5f - 30, 2, g_loadText, GS_SETREG_RGBAQ(0x90, 0xC8, 0xF0, 0x80, 0));
    gsKit_queue_exec(g_gs);
    gsKit_sync_flip(g_gs);
}

// Called as the lot loads, once for every step a script takes: now and then it moves the bar and says it is alive (a load takes a while and shows nothing else).
struct LoadProgress {
    std::uint64_t last = 0, steps = 0;
    void operator()(const ts2::sim::StepInfo&) {
        if ((++steps & 0xFF) != 0) {
            return;
        }
        const std::uint64_t now = ts2::platform::ticksNs();
        if (now - last < 500000000ull) {
            return;
        }
        last = now;
        logf("neots2: loading the lot: %llu script steps\n", static_cast<unsigned long long>(steps));
        loadingScreen(0.8f, "running the object scripts");
    }
};

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
    logf("neots2: loading the bank\n");
    {
        static float fraction;
        fraction = 0.25f;
        ts2::sim::LotSimulation::loadPhase = [](const char* name) {
            fraction = std::min(0.8f, fraction + 0.07f);
            logf("neots2: loading: %s\n", name);
            loadingScreen(fraction, name);
        };
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
                                                                optionalFile(tuning), LoadProgress{});
    if (support) {
        ts2::sim::installSupport(*simulation, support);
    }
    return simulation;
}

} // namespace

// Everything the program prints also goes to host:neots2.log (on a console, the folder ps2client serves): what a console prints may not reach the PC's terminal, a file does.
// The program's writes to its standard output and error pass through __wrap__write (the link wraps _write). Each is a file opened, appended to and closed again: a log
// kept open lost lines on a console (the PC's end of the connection holds what it was sent until the file closes, or not at all).
bool g_hostLog = false;
bool g_inHostLog = false;

void traceToRing(const void* data, std::size_t size) {
    const char* bytes = static_cast<const char*>(data);
    unsigned total = g_traceTotal;
    for (std::size_t i = 0; i < size; ++i) {
        g_traceRing[total++ % sizeof g_traceRing] = bytes[i];
    }
    g_traceTotal = total;
}

void appendHostLog(const void* data, std::size_t size) {
    if (!g_hostLog || g_inHostLog) {
        return;
    }
    g_inHostLog = true;  // (the file's own writes pass through __wrap__write too)
    if (std::FILE* f = std::fopen("host:neots2.log", "ab")) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    }
    g_inHostLog = false;
}

void logf(const char* format, ...) {
    char text[400];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (n > 0) {
        const std::size_t size = std::min<std::size_t>(static_cast<std::size_t>(n), sizeof text - 1);
        traceToRing(text, size);
        appendHostLog(text, size);
    }
}

void teeToHost() {
    if (std::FILE* f = std::fopen("host:neots2.log", "wb")) {
        std::fclose(f);
        g_hostLog = true;
    }
}

int runConsoleLot(int argc, char** argv) {
    teeToHost();
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
    g_gs = gs;
    loadingScreen(0.0f, "starting");
    // The controller after the screen, as the viewer test program does it.
    if (!ts2::platform::init()) {
        logf("neots2: %s\n", ts2::platform::lastError().c_str());
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
        loadingScreen(0.1f, "reading the scene");
        ts2::render::GsRenderer renderer(gs);
        if (std::FILE* mode = std::fopen("host:vifmode", "rb")) {  // (a file with a digit: how much the VU1's batches may overlap, see GsRenderer::setBatchSync)
            const int digit = std::fgetc(mode);
            std::fclose(mode);
            if (digit >= '0' && digit <= '2') {
                renderer.setBatchSync(digit - '0');
                logf("neots2: VU1 batch sync mode %c\n", digit);
            }
        }
        renderer.setTrace([](const char* text) {
            const std::string line = std::string(text) + "\n";
            traceToRing(line.data(), line.size());
            appendHostLog(line.data(), line.size());
        });
        renderer.setImageSource(
            [file](const ts2::scene::Image& image) {
                std::vector<std::uint8_t> blob(image.consoleSize);
                if (std::fseek(file, static_cast<long>(image.consoleOffset), SEEK_SET) != 0 || std::fread(blob.data(), 1, blob.size(), file) != blob.size()) {
                    return std::vector<std::uint8_t>{};
                }
                return blob;
            },
            2 * 1024 * 1024);
        renderer.setCacheBudgets(256 * 1024, 2048 * 1024);  // (the simulation has the rest of the heap)
        auto prepared = renderer.prepareBaked(scene, std::move(meshes));
        renderer.upload(std::move(prepared));
        memory("scene uploaded");
        loadingScreen(0.25f, "loading the bank");
        // The lot's simulation.
        g_consoleStage = 2;
        const bool withSim = !(argc > 4 && std::string(argv[4]) == "nosim");  // (to see the scene alone: 4th argument)
        ConsoleLife life(withSim ? loadBank(bank) : nullptr, scene);
        ts2::sim::LotSimulation::loadPhase = nullptr;
        g_consoleStage = 3;
        memory("simulation loaded");
        loadingScreen(0.85f, "warming up the simulation");
        if (life.hasSimulation()) {
            // The route grids and the behaviours the first ticks of a run need, now and not in the middle of a tick (each was a stall of a second or more).
            const std::uint64_t w0 = ts2::platform::ticksNs();
            const std::size_t warmed = life.simulation().warm();
            logf("neots2: warmed %zu resources and the route grids in %.1f s\n", warmed, static_cast<double>(ts2::platform::ticksNs() - w0) / 1e9);
            memory("simulation warmed");
            loadingScreen(0.95f, "reading textures for the first frame");
        }
        trimScene(scene);
        memory("scene trimmed");
        logf("neots2: %zu Sims, clock %s\n", life.simCount(), life.clockText().c_str());

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
        // Where a frame's time goes (wall clock; in an emulator only the proportions mean anything): sums over a few seconds of frames, in seconds.
        unsigned framesInProfile = 0;
        double sumUpdate = 0, sumVisible = 0, sumDraw = 0, sumFrame = 0;
        ts2::render::GsRenderer::FrameStats sum;
        const auto seconds = [] { return static_cast<double>(ts2::platform::ticksNs()) / 1e9; };
        double lastProfile = seconds();
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
                logf("neots2: level %d of %d..%d: %zu instances shown, %zu with the storey filter\n", lotView.view.level, lotView.ground, lotView.top, before,
                            static_cast<std::size_t>(std::count(visible.begin(), visible.end(), std::uint8_t{1})));
            }
            renderer.show(std::move(visible));
            if (frame == 0) {
                // The textures of the first frame, one read after another from the scene file (over a network, each a round trip): now, with the count showing, and not inside the draw.
                const std::uint64_t t0 = ts2::platform::ticksNs();
                std::uint64_t shown = t0;
                static char text[48];
                const std::size_t count = renderer.preload(orbit.camera(), [&](std::size_t done, std::size_t total) {
                    const std::uint64_t now = ts2::platform::ticksNs();
                    if (done == 1 || done == total || now - shown >= 500000000ull) {
                        shown = now;
                        std::snprintf(text, sizeof text, "reading texture %zu of %zu", done, total);
                        logf("neots2: %s (%.1f s)\n", text, static_cast<double>(now - t0) / 1e9);
                        loadingScreen(0.95f, text);
                    }
                });
                logf("neots2: %zu textures read in %.1f s\n", count, static_cast<double>(ts2::platform::ticksNs() - t0) / 1e9);
            }
            CONSOLE_STAGE(16);
            if (frame == 2) {
                const auto cam = orbit.camera();
                logf("neots2: camera eye %.3f %.3f %.3f target %.3f %.3f %.3f fovX %.4f fovY %.4f far %.1f near %.4f\n", static_cast<double>(cam.eye[0]),
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
                sum.eeCorners += f.eeCorners, sum.vuCorners += f.vuCorners, sum.cacheHits += f.cacheHits, sum.draws += f.draws, sum.uploads += f.uploads, sum.clipped += f.clipped, sum.litHits += f.litHits, sum.litMisses += f.litMisses;
            }
            ++framesInProfile;
            if (seconds() - lastProfile >= 5.0) {  // (every few seconds, not every 60 frames: a console may draw a frame a second)
                const double n = framesInProfile, ms = 1000.0 / n;
                logf("neots2: profile %u frames in %.1f s, %.1f ms a frame: sim update %.1f, visibility %.1f, draw %.1f (collect %.1f, uniforms %.1f, light %.1f, vu build %.1f (texture %.1f, ee clip %.1f), vu wait %.1f,"
                            " submit %.1f); %zu ee corners, %zu vu corners; %.0f draws, %.1f uploads, %.1f clipped, lit cache %zu hits %zu misses, %zu chains from the cache a frame\n",
                            framesInProfile, seconds() - lastProfile, sumFrame * ms, sumUpdate * ms, sumVisible * ms, sumDraw * ms, sum.collect * ms, sum.uniforms * ms, sum.light * ms, sum.vuBuild * ms, sum.state * ms, sum.transform * ms,
                            sum.vuWait * ms, sum.submit * ms, sum.eeCorners / framesInProfile, sum.vuCorners / framesInProfile, static_cast<double>(sum.draws) / n, static_cast<double>(sum.uploads) / n, static_cast<double>(sum.clipped) / n,
                            sum.litHits / framesInProfile, sum.litMisses / framesInProfile, sum.cacheHits / framesInProfile);
                sumUpdate = sumVisible = sumDraw = sumFrame = 0;
                sum = {};
                framesInProfile = 0;
                lastProfile = seconds();
            }
            CONSOLE_STAGE(17);
            if (frame % 120 == 0) {
                if (argc > 9 && life.hasSimulation()) {
                    if (const auto* followed = life.simulation().world().find(static_cast<std::int16_t>(std::strtol(argv[9], nullptr, 10))); followed != nullptr) {
                        logf("neots2: Sim %s at x %.2f y %.2f level %d; camera target %.2f %.2f %.2f\n", argv[9], static_cast<double>(followed->x) / 65536.0,
                                    static_cast<double>(followed->y) / 65536.0, static_cast<int>(followed->level), static_cast<double>(orbit.target[0]),
                                    static_cast<double>(orbit.target[1]), static_cast<double>(orbit.target[2]));
                    }
                }
                const auto& stats = renderer.lastFrame();
                logf("neots2: frame %u, %zu ticks, clock %s; %zu draws, %zu triangles, %zu drawn, %zu culled, %zu clipped, %zu vu batches, %zu textures read\n", frame,
                            life.ticks(), life.clockText().c_str(), stats.draws, stats.triangles, stats.drawn, stats.culled, stats.clipped, stats.vuBatches, stats.uploads);
            }
        }
    } catch (const std::exception& ex) {
        logf("neots2: %s\n", ex.what());
        std::snprintf(g_consoleError, sizeof g_consoleError, "%s", ex.what());
        g_consoleStage = 900;
    }
    ts2::platform::quit();
    logf("neots2: done\n");
    for (;;) {  // (a program that returns is unloaded and its memory cleared: see tools/ps2test/pine_sym.sh)
        SleepThread();
    }
    return 0;
}

} // namespace app

extern "C" {
ssize_t __real__write(int fd, const void* data, size_t size);
ssize_t __wrap__write(int fd, const void* data, size_t size) {
    if (fd == 1 || fd == 2) {
        app::traceToRing(data, size);  // (first: whatever happens to the rest)
    }
    const ssize_t written = __real__write(fd, data, size);
    if (fd == 1 || fd == 2) {
        app::appendHostLog(data, size);
    }
    return written;
}
}
