// Records the last functions entered, for finding where a console program stops (compile the code of interest with -finstrument-functions, see
// NEOTS2_PS2_TRACE in CMakeLists.txt, and read g_traceRing / g_traceCount with tools/ps2test/pine_sym.sh; resolve the addresses with addr2line).
extern "C" {
volatile unsigned g_traceRing[64] = {0};
volatile unsigned g_traceCount = 0;

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* fn, void*) {
    g_traceRing[g_traceCount++ & 63] = static_cast<unsigned>(reinterpret_cast<unsigned long>(fn));
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void*, void*) {}
}
