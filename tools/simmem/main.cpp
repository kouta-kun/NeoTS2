// Builds a lot's simulation and nothing else (no scene assets, no animation library, no tool-side index) and ticks it, for measuring what the simulation
// alone holds and how long it takes:
//   simmem <install-folder> <lot.package> [ticks]      from an install (heaptrack it for the heap)
//   simmem <bank-folder> [ticks]                        from a folder simbank wrote: what a console runs (also built for the PlayStation 2)
//
// On the console only the first lines of stdout reach PCSX2's log reliably, so the results are also kept in the globals below, which
// tools/ps2test/pine_sym.sh reads from outside the running emulator.
#include "content/wants.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/support_table.hpp"

#include <malloc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>

extern "C" {
volatile unsigned g_stage = 0;  // 100 once the lot is loaded, 200 + tick while ticking, 1000 when done, 900 after an exception
char g_error[256] = {0};        // the exception's message
volatile unsigned g_stops = 0, g_supportBytes = 0, g_checksum = 0, g_walkers = 0, g_supportMs = 0;
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

// On the console, a program that returns is unloaded and its memory cleared; this keeps the results readable (see tools/ps2test/pine_sym.sh).
void parkOnConsole() {
#ifdef __PS2__
    for (;;) {
    }
#endif
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef __PS2__
    // Booted from a disc there are no arguments: the bank is on the disc.
    static char* discArgs[] = {const_cast<char*>("simmem"), const_cast<char*>("cdrom0:\\BANK"), const_cast<char*>("300")};
    if (argc < 2) {
        argc = 3;
        argv = discArgs;
    }
#endif
    if (argc < 2) {
        std::fprintf(stderr, "Usage: simmem <install-folder> <lot.package> [ticks] | simmem <bank-folder> [ticks]\n");
        return 1;
    }
    try {
        const std::filesystem::path bank = argv[1];
        std::ifstream cfg(bank / "sim.cfg");
        const bool fromBank = static_cast<bool>(cfg);  // (reading the file to its end clears `cfg`)
        if (argc > 3 && std::string(argv[3]) == "tuning") {
            g_stage = 1;
            const std::string text = ts2::content::loadWantTuning(bank / "wanttuning.package");
            g_stage = 2;
            const auto categories = ts2::content::parseWantCategories(text);
            g_stage = 3;
            const auto influence = ts2::content::parseInfluenceCurve(text);
            g_stage = 4;
            const auto inverse = influence ? std::optional(influence->inverse()) : std::nullopt;
            g_stage = 5;
            std::printf("simmem: tuning %zu bytes, %zu categories, curve %d, inverse %d\n", text.size(), categories.size(), influence ? 1 : 0, inverse ? 1 : 0);
            parkOnConsole();
            return 0;
        }
        if (argc > 3 && std::string(argv[3]) == "at") {
            // One read at a given offset on a fresh stream: does the position itself fail?
            std::ifstream in(bank / "code.package", std::ios::binary);
            char buffer[4096];
            in.seekg(static_cast<std::streamoff>(std::strtoul(argv[4], nullptr, 10)));
            in.read(buffer, static_cast<std::streamsize>(std::strtoul(argv[5], nullptr, 10)));
            g_walkers = in ? 1 : 2;
            g_ticksDone = static_cast<unsigned>(in.gcount());
            g_stage = 60;
            parkOnConsole();
            return 0;
        }
        if (argc > 3 && std::string(argv[3]) == "grow") {
            // Grow the heap a MiB at a time, reading a little from a file after each: does a read fail past some size?
            std::ifstream in(bank / "code.package", std::ios::binary);
            char buffer[256];
            for (unsigned mib = 1; mib < 40; ++mib) {
                char* block = static_cast<char*>(std::malloc(1 << 20));
                if (!block) {
                    g_loadMs = 9000 + mib;
                    break;
                }
                std::memset(block, 1, 1 << 20);
                in.clear();
                in.seekg(static_cast<std::streamoff>(mib * 100003));
                if (!in.read(buffer, sizeof buffer)) {
                    g_loadMs = mib;  // the first MiB at which a read failed
                    break;
                }
                g_ticksDone = mib;
            }
            g_heapLoaded = heapInUse();
            g_stage = 70;
            parkOnConsole();
            return 0;
        }
        if (argc > 3 && std::string(argv[3]) == "reads") {
            // Small reads from one open file until one fails: does the host filesystem give out after a count or a time?
            std::ifstream in(bank / "code.package", std::ios::binary);
            char buffer[256];
            const double t = now();
            unsigned long long at = 12345;
            for (unsigned i = 0;; ++i) {
                at = (at * 6364136223846793005ULL + 1442695040888963407ULL);
                in.clear();
                in.seekg(static_cast<std::streamoff>((at >> 33) % 10000000));
                if (!in.read(buffer, sizeof buffer)) {
                    break;
                }
                g_ticksDone = i + 1;
                g_ticksMs = static_cast<unsigned>((now() - t) * 1000);
                if (i >= 200000) {
                    break;
                }
            }
            g_stage = 50;
            parkOnConsole();
            return 0;
        }
        if (argc > 3 && std::string(argv[3]) == "paths") {
            // Which spellings of a path the file system takes: bit i of g_walkers is set when argv[4 + i] opens.
            unsigned bits = 0;
            for (int i = 4; i < argc && i < 30; ++i) {
                std::ifstream in(argv[i], std::ios::binary);
                char c[16];
                if (in && in.read(c, sizeof c)) {
                    bits |= 1u << (i - 4);
                }
            }
            g_walkers = bits;
            g_stage = 80;
            parkOnConsole();
            return 0;
        }
        if (argc > 3 && std::string(argv[3]) == "held") {
            // Several files open at once, then the want definitions: does an open stall behind them?
            std::vector<std::unique_ptr<std::ifstream>> held;
            const std::size_t count = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 3;
            for (std::size_t i = 0; i < count; ++i) {
                g_stage = 1000 + static_cast<unsigned>(i);
                held.push_back(std::make_unique<std::ifstream>(bank / "code.package", std::ios::binary));
                if (!*held.back()) {
                    g_loadMs = static_cast<unsigned>(i);  // the first open that fails
                    break;
                }
                g_ticksDone = static_cast<unsigned>(i + 1);
            }
            char probe[64];
            held.front()->read(probe, sizeof probe);
            g_walkers = held.front()->good() ? 1 : 2;  // the first stream still reads after all the others opened
            g_stage = 20;
            const auto defs = ts2::content::loadWantDefinitions(bank / "wants.package");
            g_stage = 21;
            std::printf("simmem: held %zu, %zu defs\n", held.size(), defs.byGuid.size());
            parkOnConsole();
            return 0;
        }
        if (argc > 3 && std::string(argv[3]) == "wants") {
            // The want definitions alone: how long they take to load and what they hold.
            const double t = now();
            g_stage = 1;
            const auto defs = ts2::content::loadWantDefinitions(bank / "wants.package");
            g_stage = 2;
            g_loadMs = static_cast<unsigned>((now() - t) * 1000);
            std::printf("simmem: %zu want definitions in %.2f s\n", defs.byGuid.size(), now() - t);
            g_stage = 3;
            parkOnConsole();
            return 0;
        }
        const double start = now();
        g_stage = 2;  // the cfg and files below
        // (read before the lot: on the console, opening files after the heavy reads of the load can fail)
        std::shared_ptr<const ts2::sim::SupportTable> support;
        if (fromBank) {
            std::ifstream in(bank / "support.bin", std::ios::binary);
            if (in) {
                const std::vector<char> data((std::istreambuf_iterator<char>(in)), {});
                g_supportBytes = static_cast<unsigned>(data.size());
                support = std::make_shared<const ts2::sim::SupportTable>(
                    ts2::sim::readSupportTable(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size())));
            }
        }
        g_stage = 3;  // support read
        std::unique_ptr<ts2::sim::LotSimulation> simulation;
        std::size_t ticks = 300;
        if (cfg) {
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
            ticks = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 300;
            const auto optionalFile = [&](const std::string& name) {
                return name.empty() ? std::optional<std::filesystem::path>() : std::optional<std::filesystem::path>(bank / name);
            };
            simulation = std::make_unique<ts2::sim::LotSimulation>(std::vector<std::filesystem::path>{bank / "code.package"}, packs, bank / lot,
                                                                   optionalFile(wants), optionalFile(tuning));
        } else {
            if (argc < 3) {
                std::fprintf(stderr, "no sim.cfg in %s: give a lot\n", argv[1]);
                return 1;
            }
            ticks = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 300;
            simulation = std::make_unique<ts2::sim::LotSimulation>(argv[1], argv[2], std::function<void(const ts2::sim::StepInfo&)>{});
        }
        if (fromBank) {
            // What routing and the animations need of the game's models: replayed from support.bin.
            const double t = now();
            if (support) {
                ts2::sim::installSupport(*simulation, support);
            }
            g_supportMs = static_cast<unsigned>((now() - t) * 1000);
            std::printf("simmem: support %s, %u ms\n", support ? "read" : "missing", static_cast<unsigned>(g_supportMs));
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
        g_stops = static_cast<unsigned>(simulation->stops().size());
        if (!simulation->stops().empty()) {
            std::snprintf(g_error, sizeof g_error, "%s", simulation->stops().begin()->first.c_str());
        }
        // A fingerprint of where everything ended up, to compare runs: the objects' places (in 1/65536 tiles) and how many Sims are walking.
        unsigned checksum = 0, walkers = 0;
        simulation->world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
            checksum = checksum * 31 + static_cast<unsigned>(o.x) + 7 * static_cast<unsigned>(o.y) + static_cast<unsigned>(o.level);
            walkers += o.walking ? 1 : 0;
            return false;
        });
        g_checksum = checksum;
        g_walkers = walkers;
        std::printf("simmem: fingerprint %08x, %u Sims walking\n", checksum, walkers);
        g_heapAfter = heapInUse();
        g_stage = 1000;
        std::printf("simmem: %zu ticks in %.2f s (%.2f ms a tick, slowest %u ms), heap in use %.2f MiB\n", ticks, now() - ticking,
                    (now() - ticking) * 1000.0 / static_cast<double>(ticks), static_cast<unsigned>(g_tickMsMax), g_heapAfter / 1048576.0);
        parkOnConsole();
    } catch (const std::exception& e) {
        std::snprintf(g_error, sizeof g_error, "%s", e.what());
        g_stage = 900;
        std::printf("simmem: failed: %s\n", e.what());  // stdout: a console shows nothing of stderr
        return 1;
    }
}
