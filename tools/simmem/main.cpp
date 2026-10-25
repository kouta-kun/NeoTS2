// Builds a lot's simulation and nothing else (no scene assets, no animation library, no tool-side index) and ticks it, for measuring what the simulation
// alone holds and how long it takes:
//   simmem <install-folder> <lot.package> [ticks]      from an install (heaptrack it for the heap)
//   simmem <bank-folder> [ticks]                        from a folder simbank wrote: what a console runs (also built for the PlayStation 2)
//
// On the console only the first lines of stdout reach PCSX2's log reliably, so the results are also kept in the globals below, which
// tools/ps2test/pine_sym.sh reads from outside the running emulator.
#include "sim/lot_simulation.hpp"

#include <malloc.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <memory>
#include <string>

extern "C" {
volatile unsigned g_stage = 0;  // 100 once the lot is loaded, 200 + tick while ticking, 1000 when done, 900 after an exception
char g_error[256] = {0};        // the exception's message
volatile unsigned g_loadMs = 0, g_ticksMs = 0, g_ticksDone = 0, g_tickMsMax = 0, g_heapLoaded = 0, g_heapAfter = 0;
}

namespace {

double now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) * 1e-9;
}

unsigned heapInUse() {
    return static_cast<unsigned>(mallinfo().uordblks);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "Usage: simmem <install-folder> <lot.package> [ticks] | simmem <bank-folder> [ticks]\n");
        return 1;
    }
    try {
        const std::filesystem::path bank = argv[1];
        std::ifstream cfg(bank / "sim.cfg");
        const double start = now();
        std::unique_ptr<ts2::sim::LotSimulation> simulation;
        std::size_t ticks = 300;
        if (cfg) {
            std::string line, lot;
            std::uint32_t packs = 0;
            while (std::getline(cfg, line)) {
                if (line.rfind("packs=", 0) == 0) {
                    packs = static_cast<std::uint32_t>(std::stoul(line.substr(6)));
                } else if (line.rfind("lot=", 0) == 0) {
                    lot = line.substr(4);
                }
            }
            ticks = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 300;
            simulation = std::make_unique<ts2::sim::LotSimulation>(std::vector<std::filesystem::path>{bank / "code.package"}, packs, bank / lot);
        } else {
            if (argc < 3) {
                std::fprintf(stderr, "no sim.cfg in %s: give a lot\n", argv[1]);
                return 1;
            }
            ticks = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 300;
            simulation = std::make_unique<ts2::sim::LotSimulation>(argv[1], argv[2], std::function<void(const ts2::sim::StepInfo&)>{});
        }
        g_loadMs = static_cast<unsigned>((now() - start) * 1000);
        g_heapLoaded = heapInUse();
        g_stage = 100;
        std::printf("simmem: lot loaded in %.2f s, heap in use %.2f MiB\n", now() - start, g_heapLoaded / 1048576.0);
        const double ticking = now();
        for (std::size_t i = 0; i < ticks; ++i) {
            g_stage = 200 + static_cast<unsigned>(i);
            const double t0 = now();
            simulation->tick();
            const auto ms = static_cast<unsigned>((now() - t0) * 1000);
            g_tickMsMax = ms > g_tickMsMax ? ms : g_tickMsMax;
            g_ticksDone = static_cast<unsigned>(i + 1);
            g_ticksMs = static_cast<unsigned>((now() - ticking) * 1000);
        }
        g_heapAfter = heapInUse();
        g_stage = 1000;
        std::printf("simmem: %zu ticks in %.2f s (%.2f ms a tick, slowest %u ms), heap in use %.2f MiB\n", ticks, now() - ticking,
                    (now() - ticking) * 1000.0 / static_cast<double>(ticks), static_cast<unsigned>(g_tickMsMax), g_heapAfter / 1048576.0);
    } catch (const std::exception& e) {
        std::snprintf(g_error, sizeof g_error, "%s", e.what());
        g_stage = 900;
        std::printf("simmem: failed: %s\n", e.what());  // stdout: a console shows nothing of stderr
        return 1;
    }
}
