// Host for the ERL test: registers the symbols the module imports, loads it from memory, calls each entry.
#include <cstdio>
#include <erl.h>
#include <kernel.h>

void registerErlSymbols();
extern "C" unsigned char module_erl[] __asm__("module_erl");

static int failures = 0;

static void run(erl_record_t* rec, const char* name, int arg, int expect) {
    symbol_t* sym = erl_find_local_symbol(name, rec);
    if (!sym) { std::printf("FAIL %s: symbol not found\n", name); ++failures; return; }
    std::printf("calling %s...\n", name);
    int r = reinterpret_cast<int (*)(int)>(sym->address)(arg);
    bool ok = r == expect;
    std::printf("%s %s(%d) = %d (expect %d)\n", ok ? "PASS" : "FAIL", name, arg, r, expect);
    if (!ok) ++failures;
}

int main() {
    std::printf("== erl host ==\n");
    registerErlSymbols();
    erl_record_t* rec = load_erl_from_mem(module_erl, 0, nullptr);
    std::printf("load_erl_from_mem -> %p\n", static_cast<void*>(rec));
    if (!rec) { std::printf("FAIL load\n"); return 1; }
    if (symbol_t* eh = erl_find_local_symbol("erl_register_eh", rec)) reinterpret_cast<void (*)()>(eh->address)();
    run(rec, "erl_test_plain", 21, 42);
    run(rec, "erl_test_vector", 10, 285);
    run(rec, "erl_test_format", 5, 3);
    // Last: with liberl an exception thrown in the module is not caught (see plan), and terminates.
    run(rec, "erl_test_catch", 7, 7);
    unload_erl(rec);
    std::printf("== done: %d failure(s) ==\n", failures);
    return failures;
}
