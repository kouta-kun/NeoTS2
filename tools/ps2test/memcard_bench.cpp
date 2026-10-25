// Memory-card throughput benchmark: writes and reads a file in several chunk sizes and prints KiB/s.
// Run on real hardware with an official card; emulators do not model flash timing.
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
// mcserv takes the IOP fio open flags, which differ from newlib's O_* values.
enum { FIO_O_RDONLY = 0x0001, FIO_O_WRONLY = 0x0002, FIO_O_CREAT = 0x0200, FIO_O_TRUNC = 0x0400 };
#include <cstring>
#include <ctime>
#include <vector>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <libmc.h>
#include <debug.h>

static double now() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

static int waitMc() { int cmd, res; mcSync(0, &cmd, &res); return res; }

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    std::fputs(buf, stdout);
    scr_printf("%s", buf);
}

int main() {
    SifInitRpc(0);
    init_scr();
    SifLoadModule("rom0:SIO2MAN", 0, nullptr);
    SifLoadModule("rom0:MCMAN", 0, nullptr);
    SifLoadModule("rom0:MCSERV", 0, nullptr);
    if (mcInit(MC_TYPE_MC) < 0) { say("mcInit failed\n"); SleepThread(); }

    int type, freeKb, format;
    mcGetInfo(0, 0, &type, &freeKb, &format);
    int r = waitMc();
    say("card: result %d type %d free %d KiB format %d\n", r, type, freeKb, format);
    if (r < -1) { say("no usable card in slot 1\n"); SleepThread(); }

    const int total = 256 * 1024;
    const int chunks[] = {128, 512, 4096, 16384, 65536};
    std::vector<unsigned char> data(total), back(total);
    for (int i = 0; i < total; ++i) data[i] = static_cast<unsigned char>(i * 31 + 7);

    mcMkDir(0, 0, "NEOTS2BENCH"); waitMc();
    for (int chunk : chunks) {
        mcDelete(0, 0, "NEOTS2BENCH/bench.bin"); waitMc();
        mcOpen(0, 0, "NEOTS2BENCH/bench.bin", FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
        int fd = waitMc();
        if (fd < 0) { say("open for write failed: %d\n", fd); break; }
        double t0 = now();
        bool ok = true;
        for (int off = 0; off < total && ok; off += chunk) {
            mcWrite(fd, data.data() + off, chunk);
            ok = waitMc() == chunk;
        }
        double tw = now() - t0;
        mcClose(fd); waitMc();

        mcOpen(0, 0, "NEOTS2BENCH/bench.bin", FIO_O_RDONLY);
        fd = waitMc();
        if (fd < 0) { say("open for read failed: %d\n", fd); break; }
        std::memset(back.data(), 0, total);
        t0 = now();
        for (int off = 0; off < total && ok; off += chunk) {
            mcRead(fd, back.data() + off, chunk);
            ok = waitMc() == chunk;
        }
        double tr = now() - t0;
        mcClose(fd); waitMc();
        bool same = std::memcmp(data.data(), back.data(), total) == 0;
        say("chunk %6d: write %7.1f KiB/s  read %7.1f KiB/s  %s\n", chunk,
            total / 1024.0 / tw, total / 1024.0 / tr, ok && same ? "ok" : "MISMATCH");
    }
    mcDelete(0, 0, "NEOTS2BENCH/bench.bin"); waitMc();
    say("done\n");
    SleepThread();
}
