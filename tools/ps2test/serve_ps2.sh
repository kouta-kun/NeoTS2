#!/bin/bash
# Runs the console game on a real PS2 whose ps2link is waiting on the network, serving host: from this machine with ps2client.
#   tools/ps2test/serve_ps2.sh [-i PS2_IP] [demo-folder | demo.tar.gz | neots2.elf bank-folder scene.cscene]
# The PS2 must be running ps2link (any launcher that starts it, with the network up) and be reachable at PS2_IP (default $PS2_IP, else 192.168.0.10).
# With no folder the files are the build in build-ps2-demo and the Lot 68 files under ~/texture-check, as package_demo.sh takes them.
# host: is this script's staging folder (symlinks to the three files), so the program finds "host:bank" and "host:lot.cscene" as it does under PCSX2.
# Leave it running: ps2client answers the console's file requests and prints its output; Ctrl-C ends it ("reset" below puts ps2link back to waiting).
#   -r   reset the PS2 to ps2link first (a program that crashed leaves it wedged)
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
ip=${PS2_IP:-192.168.0.10}
reset=0
while getopts "i:r" opt; do
    case $opt in
        i) ip=$OPTARG ;;
        r) reset=1 ;;
        *) echo "usage: $0 [-i ip] [-r] [demo-folder | demo.tar.gz | elf bank scene]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
command -v ps2client >/dev/null || { echo "ps2client is not on PATH (it comes with ps2dev: \$PS2DEV/bin)" >&2; exit 1; }
stage=$(mktemp -d); trap 'rm -rf "$stage"' EXIT
if [ $# -eq 3 ]; then
    elf=$1; bank=$2; scene=$3
elif [ $# -eq 1 ] && [ -d "$1" ]; then
    elf=$1/neots2.elf; bank=$1/bank; scene=$1/lot.cscene
elif [ $# -eq 1 ] && [ -f "$1" ]; then
    tar xzf "$1" -C "$stage"
    d=$(find "$stage" -maxdepth 2 -name neots2.elf -printf '%h\n' | head -1)
    elf=$d/neots2.elf; bank=$d/bank; scene=$d/lot.cscene
elif [ $# -eq 0 ]; then
    elf=$here/build-ps2-demo/neots2; bank=$HOME/texture-check/sim/bank; scene=$HOME/texture-check/lot68/lot68_demo.cscene
else
    echo "usage: $0 [-i ip] [-r] [demo-folder | demo.tar.gz | elf bank scene]" >&2; exit 2
fi
for f in "$elf" "$bank/sim.cfg" "$scene"; do [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }; done
serve=$stage/host
mkdir -p "$serve"
ln -s "$(readlink -f "$elf")" "$serve/neots2.elf"
ln -s "$(readlink -f "$bank")" "$serve/bank"
ln -s "$(readlink -f "$scene")" "$serve/lot.cscene"
cd "$serve"
if [ "$reset" = 1 ]; then
    ps2client -h "$ip" reset
    sleep 4
fi
echo "serving $serve to $ip (host:neots2.elf, host:bank, host:lot.cscene); Ctrl-C to stop"
ps2client -h "$ip" execee host:neots2.elf host:bank host:lot.cscene
# ps2client returns once the program has been started on some versions: stay as the file server and console.
exec ps2client -h "$ip" listen
