#!/bin/bash
# Reads what the program has printed or traced out of a running (or stopped) PS2's memory, over ps2link, with no help from the program's own output channels:
#   tools/ps2test/ps2_dump.sh [-i PS2_IP] [neots2.elf]
# The ELF must be the one the console is running (its symbol table says where the program keeps g_traceRing, g_traceTotal and g_console*). Needs ps2client and
# the EE nm from ps2dev on PATH. Prints the last 4 KB of text in order, then the stage numbers (g_consoleStage: 1 reading the scene .. 3 simulation loaded, 4 frames;
# g_consoleFrame: the frame it is on).
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
ip=${PS2_IP:-192.168.0.10}
while getopts "i:" opt; do
    case $opt in
        i) ip=$OPTARG ;;
        *) echo "usage: $0 [-i ip] [neots2.elf]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
elf=${1:-$here/build-ps2-demo/neots2}
nm=${NM:-mips64r5900el-ps2-elf-nm}
command -v "$nm" >/dev/null || { echo "$nm is not on PATH (ps2dev's ee/bin)" >&2; exit 1; }
sym() { "$nm" "$elf" | awk -v n="$1" '$3 == n { print $1 }'; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
ring=$(sym g_traceRing); total=$(sym g_traceTotal); stage=$(sym g_consoleStage); frame=$(sym g_consoleFrame)
[ -n "$ring" ] && [ -n "$total" ] || { echo "$elf has no g_traceRing (a build older than the trace ring?)" >&2; exit 1; }
dump() { ps2client -h "$ip" dumpmem "0x$1" "$2" "$tmp/$3" >/dev/null; }
dump "$ring" 4096 ring.bin
dump "$total" 4 total.bin
[ -n "$stage" ] && dump "$stage" 4 stage.bin
[ -n "$frame" ] && dump "$frame" 4 frame.bin
python3 - "$tmp" <<'PY'
import struct, sys, os
d = sys.argv[1]
ring = open(os.path.join(d, "ring.bin"), "rb").read()
total = struct.unpack("<I", open(os.path.join(d, "total.bin"), "rb").read()[:4])[0]
if total <= len(ring):
    text = ring[:total]
else:
    at = total % len(ring)
    text = ring[at:] + ring[:at]
print(text.decode("latin-1"))
for name in ("stage", "frame"):
    p = os.path.join(d, name + ".bin")
    if os.path.exists(p):
        print("g_console%s = %d" % (name.capitalize(), struct.unpack("<I", open(p, "rb").read()[:4])[0]))
print("(%d bytes of text so far)" % total)
PY
