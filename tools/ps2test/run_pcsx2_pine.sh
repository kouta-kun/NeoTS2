#!/bin/bash
# Runs an ELF in PCSX2 on :0 with PINE enabled and leaves it running for `seconds`, then runs the command in $PINE_CMD (default: nothing), then stops PCSX2.
# Usage: PINE_CMD='tools/ps2test/pine_read.py 5aa750 20' run_pcsx2_pine.sh <elf> <seconds> [log]
set -u
elf=$(realpath "$1"); wait_s=${2:-30}; log=${3:-/dev/null}
display=${DISPLAY_FOR_PCSX2:-:0}
ini=$HOME/.config/PCSX2/inis/PCSX2.ini; backup=$(mktemp)
cp "$ini" "$backup"; trap 'cp "$backup" "$ini"; rm -f "$backup"; kill $pid 2>/dev/null' EXIT
sed -i 's/^HostFs = .*/HostFs = true/; s/^EnablePINE = .*/EnablePINE = true/' "$ini"
DISPLAY=$display pcsx2-qt -batch -nogui -elf "$elf" ${ELF_ARGS:+-gameargs "$ELF_ARGS"} > "$log" 2>&1 & pid=$!
sleep "$wait_s"
if [ -n "${PINE_CMD:-}" ]; then eval "$PINE_CMD"; fi
