#!/usr/bin/env python3
"""Emit a host source that exports every undefined symbol of an ERL to liberl."""
import subprocess, sys
nm, erl = sys.argv[1], sys.argv[2]
syms = [l.split()[1] for l in subprocess.run([nm, "-u", erl], capture_output=True, text=True, check=True).stdout.splitlines() if l.split()]
print('#include <erl.h>\n#include <cstdio>')
for i, s in enumerate(syms):
    print(f'extern "C" char sym{i}[] __asm__("{s}");')
print('void registerErlSymbols() {')
for i, s in enumerate(syms):
    print(f'    erl_add_global_symbol("{s}", reinterpret_cast<u32>(sym{i}));')
print('}')
