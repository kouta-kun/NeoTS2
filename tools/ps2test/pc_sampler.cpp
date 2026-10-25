// Debugging helpers for a console program that stops (build with -DNEOTS2_PS2_DEBUG=ON; read the globals with tools/ps2test/pine_sym.sh, resolve addresses
// with the toolchain's addr2line):
//  - ts2StartPcSampler: on every vertical blank the address the program was interrupted at goes into g_pcRing. An address inside the kernel (below
//    0x100000) every time means all threads are blocked.
//  - ts2StartMonitor: a thread that now and then asks the kernel what the main thread waits for (g_thWaitType: 1 sleep, 2 semaphore; g_thWaitId and
//    g_sem*: that semaphore's state).
//  - g_semaLog, g_pteLog: the kernel semaphores created and who asked (the program had run out of them: see pthread_light.cpp).
#include <kernel.h>
#include <delaythread.h>
#include <cstdint>
#include <cstdio>

extern "C" {
volatile unsigned g_pcRing[256] = {0};
volatile unsigned g_thStatus = 0, g_thWaitType = 0, g_thWaitId = 0, g_thPrio = 0, g_thChecks = 0, g_semCount = 0xFFFF, g_semWaiting = 0xFFFF, g_semMax = 0, g_semAttr = 0;
volatile int g_mainTid = 0;
alignas(16) char g_monitorStack[8192];

volatile unsigned g_pcCount = 0, g_pcRa[256] = {0};

namespace {
int onVblank(int) {
    unsigned epc;
    asm volatile("mfc0 %0, $14" : "=r"(epc));
    g_pcRing[g_pcCount & 255] = epc;
    ++g_pcCount;
    return 0;
}
}


namespace {
// Looks at the main thread now and then: what it waits for, and what is wrong with that.
void monitor(void*) {
    for (;;) {
        DelayThread(1000 * 1000);
        ee_thread_status_t st;
        if (ReferThreadStatus(g_mainTid, &st) >= 0) {
            g_thStatus = static_cast<unsigned>(st.status);
            g_thWaitType = static_cast<unsigned>(st.waitType);
            g_thWaitId = static_cast<unsigned>(st.waitId);
            g_thPrio = static_cast<unsigned>(st.current_priority);
            if (st.waitType == 2 || st.waitType == 3 || st.waitType == 4) {  // a semaphore
                ee_sema_t sema;
                if (ReferSemaStatus(st.waitId, &sema) >= 0) {
                    g_semCount = static_cast<unsigned>(sema.count);
                    g_semWaiting = static_cast<unsigned>(sema.wait_threads);
                    g_semMax = static_cast<unsigned>(sema.max_count);
                    g_semAttr = static_cast<unsigned>(sema.attr);
                }
            }
        }
        ++g_thChecks;
    }
}
}

void ts2StartMonitor() {
    g_mainTid = GetThreadId();
    ee_thread_t t = {};
    t.func = reinterpret_cast<void*>(monitor);
    t.stack = g_monitorStack;
    t.stack_size = sizeof g_monitorStack;
    t.gp_reg = &_gp;
    t.initial_priority = 1;
    const int id = CreateThread(&t);
    if (id >= 0) {
        StartThread(id, nullptr);
    }
}

volatile unsigned g_semaLog[64][3] = {{0}}, g_semaN = 0;
int __real_CreateSema(ee_sema_t* sema);
int __wrap_CreateSema(ee_sema_t* sema) {
    const int id = __real_CreateSema(sema);
    g_semaLog[g_semaN & 63][0] = static_cast<unsigned>(id);
    g_semaLog[g_semaN & 63][1] = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
    g_semaLog[g_semaN & 63][2] = static_cast<unsigned>(sema->max_count) << 16 | static_cast<unsigned>(sema->init_count);
    ++g_semaN;
    return id;
}

volatile unsigned g_pteLog[64][2] = {{0}}, g_pteN = 0, g_pteFail = 0;
int __real_pte_osSemaphoreCreate(int initialValue, void* pHandle);
int __wrap_pte_osSemaphoreCreate(int initialValue, void* pHandle) {
    const int result = __real_pte_osSemaphoreCreate(initialValue, pHandle);
    if (result != 0) {
        ++g_pteFail;
    }
    if (g_pteFail == 1 || g_pteN < 64) {
        // The first creations and the first failure: who asks.
        const unsigned slot = g_pteFail >= 1 && g_pteN >= 63 ? 63 : g_pteN;
        g_pteLog[slot][0] = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
        g_pteLog[slot][1] = static_cast<unsigned>(result);
    }
    ++g_pteN;
    return result;
}

void ts2StartPcSampler() {
    AddIntcHandler(INTC_VBLANK_S, onVblank, 0);
    EnableIntc(INTC_VBLANK_S);
}
}
