// Reads a file through host: and reports its size and a checksum.
#include <cstdio>
#include <cstdlib>
#include <kernel.h>

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "host:lot_test.bin";
    std::FILE* f = std::fopen(path, "rb");
    std::printf("host_read: fopen(%s) -> %p\n", path, static_cast<void*>(f));
    if (!f) { SleepThread(); }
    unsigned long sum = 0, total = 0;
    static unsigned char buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        for (std::size_t i = 0; i < n; ++i) sum += buf[i];
        total += n;
    }
    std::fclose(f);
    std::printf("host_read: %lu bytes, sum %lu\n", total, sum);
    SleepThread();
}
