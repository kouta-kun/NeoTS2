#!/bin/bash
# Runs an ELF in PCSX2 on an existing GPU-accelerated X display (default :0) with the hardware renderer, and grabs only the
# region `WxH+X+Y` of the display (default 800x600+0+0) once, so nothing else on the screen is read.
# Usage: run_pcsx2_gpu.sh <file.elf> <seconds before the grab> <out.png> [log file]
set -u
elf=$(realpath "$1"); wait_s=${2:-8}; out=$3; log=${4:-/dev/null}
display=${DISPLAY_FOR_PCSX2:-:0}; region=${GRAB_REGION:-800x600+0+0}
ini=$HOME/.config/PCSX2/inis/PCSX2.ini; backup=$(mktemp)
cp "$ini" "$backup"; trap 'cp "$backup" "$ini"; rm -f "$backup"; kill $pid 2>/dev/null' EXIT
sed -i 's/^HostFs = .*/HostFs = true/' "$ini"
DISPLAY=$display pcsx2-qt -batch -nogui -elf "$elf" ${ELF_ARGS:+-gameargs "$ELF_ARGS"} > "$log" 2>&1 & pid=$!
sleep "$wait_s"
size=${region%%+*}; pos=${region#*+}; x=${pos%%+*}; y=${pos#*+}
ffmpeg -loglevel error -y -f x11grab -video_size "$size" -i "$display+$x,$y" -frames:v 1 "$out"
