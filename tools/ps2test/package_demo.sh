#!/bin/bash
# Packs what is needed to try the console game in PCSX2 (or anything that gives a program a host: folder): the ELF, a bank (tools/simbank) and a baked scene
# (tools/ts2bake), with a README and a launcher, into one .tar.gz.
#   tools/ps2test/package_demo.sh [neots2.elf [bank-folder [scene.cscene [out.tar.gz]]]]
# The defaults are the build in build-ps2-demo and the Lot 68 files under ~/texture-check.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
elf=${1:-$here/build-ps2-demo/neots2}
bank=${2:-$HOME/texture-check/sim/bank}
scene=${3:-$HOME/texture-check/lot68/lot68_64b.cscene}
out=${4:-$PWD/neots2-demo.tar.gz}
for f in "$elf" "$bank/sim.cfg" "$scene"; do [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }; done
stage=$(mktemp -d); trap 'rm -rf "$stage"' EXIT
dir=$stage/neots2-demo
mkdir -p "$dir"
cp "$elf" "$dir/neots2.elf"
cp -r "$bank" "$dir/bank"
cp "$scene" "$dir/lot.cscene"
commit=$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo unknown)
cat > "$dir/run_pcsx2.sh" <<'RUN'
#!/bin/bash
# Starts the demo in PCSX2 (Qt). It needs "Enable Host Filesystem" (Settings > Advanced) switched on, and a BIOS, as for any homebrew.
cd "$(dirname "$0")"
exec pcsx2-qt -elf "$PWD/neots2.elf" -gameargs "host:bank host:lot.cscene" "$@"
RUN
chmod +x "$dir/run_pcsx2.sh"
cat > "$dir/README.txt" <<README
NeoTS2 on the PlayStation 2: a state-of-the-work demo (built from commit $commit)

What this is: the real NeoTS2 executable built for the PS2's Emotion Engine. It shows one lot (N001 Lot 68, from the Mac OS X base game) and runs that lot's Sims'
simulation beside it. The Sims are red markers for now; there is no UI and nothing to click. It was only ever run in PCSX2; nothing here has run on a real console.

Files
  neots2.elf       the program
  bank/            what the simulation reads (behaviours, objects, the lot's and its neighborhood's saves), made by tools/simbank
  lot.cscene       the lot's picture, baked by tools/ts2bake (textures capped at 64 px, meshes at 30% of the triangles)
  run_pcsx2.sh     starts it in PCSX2

Running it in PCSX2
  1. Settings > Advanced > tick "Enable Host Filesystem" (a BIOS must be set up as usual).
  2. ./run_pcsx2.sh        (or: pcsx2-qt -elf neots2.elf -gameargs "host:bank host:lot.cscene")
     "host:" is this folder. Loading takes about 10 seconds (PCSX2 does not model a disc). The software renderer is fine too, but slow.

Controls (the controller in port 1; in PCSX2 the default keyboard mapping applies)
  D-pad or left stick   move the camera over the lot
  L2 / R2               zoom in / out
  L1 / R1               turn the view a quarter
  Triangle / Cross      storey up / down
  Square                wall mode: down, cutaway, up (with roofs)
  Circle                roofs on / off
  Start                 leave

What to expect: a slow frame rate (the renderer is unoptimised and PCSX2 is not cycle accurate), the Sims' markers wandering as their scripts run, a game clock that
advances. After about two minutes of play something may still fail; it prints a line to the console and stops.

The whole thing fits in the console's 32 MB (about 26 MB of heap): that is why the pictures are small and soft.
README
(cd "$stage" && tar czf "$out" neots2-demo)
echo "wrote $out ($(du -h "$out" | cut -f1))"
tar tzf "$out" | head -20
