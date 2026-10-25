// libatomic-style out-of-line atomics for the R5900, which lacks ll/sc.
// Single core, so masking interrupts is enough to make them atomic.
#include <kernel.h>
#include <cstdint>

namespace {
struct Guard {
    int was;
    Guard() : was(DIntr()) {}
    ~Guard() { if (was) EIntr(); }
};
}

#define ATOMIC_SIZE(N, T)                                                              \
    extern "C" T __atomic_load_##N(const volatile void* p, int) {                     \
        Guard g; return *static_cast<const volatile T*>(p); }                         \
    extern "C" void __atomic_store_##N(volatile void* p, T v, int) {                  \
        Guard g; *static_cast<volatile T*>(p) = v; }                                  \
    extern "C" T __atomic_exchange_##N(volatile void* p, T v, int) {                  \
        Guard g; T o = *static_cast<volatile T*>(p); *static_cast<volatile T*>(p) = v; return o; } \
    extern "C" bool __atomic_compare_exchange_##N(volatile void* p, void* e, T d, bool, int, int) { \
        Guard g; T cur = *static_cast<volatile T*>(p);                                \
        if (cur == *static_cast<T*>(e)) { *static_cast<volatile T*>(p) = d; return true; } \
        *static_cast<T*>(e) = cur; return false; }                                    \
    extern "C" T __atomic_fetch_add_##N(volatile void* p, T v, int) {                 \
        Guard g; T o = *static_cast<volatile T*>(p); *static_cast<volatile T*>(p) = o + v; return o; } \
    extern "C" T __atomic_fetch_sub_##N(volatile void* p, T v, int) {                 \
        Guard g; T o = *static_cast<volatile T*>(p); *static_cast<volatile T*>(p) = o - v; return o; } \
    extern "C" T __atomic_fetch_and_##N(volatile void* p, T v, int) {                 \
        Guard g; T o = *static_cast<volatile T*>(p); *static_cast<volatile T*>(p) = o & v; return o; } \
    extern "C" T __atomic_fetch_or_##N(volatile void* p, T v, int) {                  \
        Guard g; T o = *static_cast<volatile T*>(p); *static_cast<volatile T*>(p) = o | v; return o; } \
    extern "C" T __atomic_fetch_xor_##N(volatile void* p, T v, int) {                 \
        Guard g; T o = *static_cast<volatile T*>(p); *static_cast<volatile T*>(p) = o ^ v; return o; }

ATOMIC_SIZE(1, unsigned char)
ATOMIC_SIZE(2, unsigned short)
ATOMIC_SIZE(4, unsigned int)
ATOMIC_SIZE(8, unsigned long long)

extern "C" bool __atomic_test_and_set(volatile void* p, int) {
    Guard g;
    auto* b = static_cast<volatile unsigned char*>(p);
    bool was = *b != 0;
    *b = 1;
    return was;
}
