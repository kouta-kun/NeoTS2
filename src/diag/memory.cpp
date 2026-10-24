#include "diag/memory.hpp"

#include "diag/log.hpp"

#include <format>

#ifdef __linux__
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace ts2::diag {

MemoryUse memoryUse() {
    MemoryUse use;
#ifdef __linux__
    // /proc/self/status lines look like "VmRSS:    123456 kB".
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string name;
        std::uint64_t kilobytes = 0;
        fields >> name >> kilobytes;
        if (name == "RssAnon:") {
            use.anonymous = kilobytes * 1024;
        } else if (name == "RssFile:") {
            use.mapped = kilobytes * 1024;
        } else if (name == "VmHWM:") {
            use.peak = kilobytes * 1024;
        }
    }
#endif
    return use;
}

void logMemory(const char* when) {
    const MemoryUse use = memoryUse();
    if (use.peak == 0) {
        return;
    }
    constexpr double kMiB = 1024.0 * 1024.0;
    info(std::format("memory {}: {:.0f} MiB of heap, {:.0f} MiB of mapped files, {:.0f} MiB at most", when, static_cast<double>(use.anonymous) / kMiB,
                     static_cast<double>(use.mapped) / kMiB, static_cast<double>(use.peak) / kMiB));
}

} // namespace ts2::diag
