// Counts how many behaviours of an install use a primitive (by opcode), and in how many instructions: opcodecount <install-folder> <opcode, hex>.
#include "content/install.hpp"
#include "content/resource_index.hpp"
#include "formats/bhav.hpp"
#include "formats/resource_types.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <set>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: opcodecount <install-folder> <opcode, hex>\n");
        return 1;
    }
    try {
        ts2::content::ResourceIndex index;
        for (const auto& path : ts2::content::objectLoadOrder(ts2::content::findProducts(argv[1]))) {
            index.addPackage(path);
        }
        const auto opcode = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 16));
        std::size_t trees = 0, using_ = 0, instructions = 0;
        std::set<std::uint32_t> groups;
        for (const auto& r : index.resources()) {
            if (r.entry.key.type != ts2::types::kBehavior) {
                continue;
            }
            ++trees;
            const auto b = ts2::bhav::parse(index.read(r));
            std::size_t n = 0;
            for (const auto& in : b.instructions) {
                n += in.opcode == opcode;
            }
            if (n > 0) {
                ++using_;
                instructions += n;
                groups.insert(r.entry.key.group);
            }
        }
        std::printf("%zu behaviours, %zu use opcode 0x%04X (%zu instructions) in %zu object groups\n", trees, using_, opcode, instructions, groups.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed: %s\n", e.what());
        return 1;
    }
}
