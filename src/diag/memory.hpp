#pragma once

#include <cstdint>

// How much memory the program is using, for sizing it against machines with little. Only Linux can be asked (zeros elsewhere).
namespace ts2::diag {

struct MemoryUse {
    std::uint64_t anonymous = 0;  // bytes in the heap and stacks: what a machine with no files mapped would need
    std::uint64_t mapped = 0;     // bytes of files mapped in and read (the page cache's, which a machine can drop)
    std::uint64_t peak = 0;       // the most of both together there has been
};

[[nodiscard]] MemoryUse memoryUse();

// Writes "memory <when>: ..." to the log. Nothing where memory can't be asked.
void logMemory(const char* when);

} // namespace ts2::diag
