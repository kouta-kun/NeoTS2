#!/bin/bash
# Prints 32-bit words of named global variables of an ELF from a running PCSX2 (see pine_read.py): pine_sym.sh <elf> <symbol>[:<words>]...
elf=$1; shift
for spec in "$@"; do
    sym=${spec%%:*}; words=1; [[ $spec == *:* ]] && words=${spec##*:}
    addr=$(mips64r5900el-ps2-elf-nm "$elf" | awk -v s="$sym" '$3==s {print $1}')
    echo -n "$sym = "; python3 -I "$(dirname "$0")/pine_read.py" "$addr" "$words" | awk '{printf "%s ", $2}'; echo
done
