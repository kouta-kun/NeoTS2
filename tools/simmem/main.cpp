// Builds a lot's simulation and nothing else (no scene assets, no animation library, no tool-side index) and ticks it, for measuring what the simulation
// alone holds: heaptrack simmem <install-folder> <lot.package> [ticks].
#include "sim/lot_simulation.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: simmem <install-folder> <lot.package> [ticks]\n");
        return 1;
    }
    try {
        ts2::sim::LotSimulation sim(argv[1], argv[2], {});
        const std::size_t ticks = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 300;
        for (std::size_t i = 0; i < ticks; ++i) {
            sim.tick();
        }
        std::printf("ticked %zu\n", ticks);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}
