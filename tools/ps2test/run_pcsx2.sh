#!/bin/bash
# Runs an ELF in PCSX2 on a virtual display with the software renderer and saves a screen grab.
# Usage: run_pcsx2.sh <file.elf> <seconds before the grab> <out.png> [log file]
# Needs Xvfb, ffmpeg and vulkan-swrast (PCSX2 opens a Vulkan surface even for software rendering).
set -u
elf=$(realpath "$1"); wait_s=${2:-8}; out=$3; log=${4:-/dev/null}
ini=$HOME/.config/PCSX2/inis/PCSX2.ini; backup=$(mktemp)
cp "$ini" "$backup"; trap 'cp "$backup" "$ini"; rm -f "$backup"; kill $xpid $pid 2>/dev/null' EXIT
sed -i 's/^Renderer = .*/Renderer = 13/; s/^Adapter = .*/Adapter = llvmpipe (LLVM 23.1.3, 256 bits)/' "$ini"
display=:$((70 + RANDOM % 20))
Xvfb $display -screen 0 1024x768x24 >/dev/null 2>&1 & xpid=$!
sleep 1
DISPLAY=$display pcsx2-qt -batch -nogui -elf "$elf" > "$log" 2>&1 & pid=$!
sleep "$wait_s"
ffmpeg -loglevel error -y -f x11grab -video_size 1024x768 -i $display -frames:v 1 "$out"
