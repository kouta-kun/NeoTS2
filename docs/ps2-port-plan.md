# PlayStation 2 port: memory sheet and plan (draft)

Status: a draft to work from, not a design. Every number says where it comes from:

- **measured**: from a run on the Linux PC (logs, heaptrack) with this repository's code;
- **manual**: the GS User's Manual v6.0 (checked);
- **spec**: a well-known PS2 figure that has not been checked against a source here;
- **assumed**: a target or guess, to be replaced by a measurement.

## Scope

- Target: the base game as the OS X release has it (no expansions), converted offline on a PC into compact data.
- The PS2 build runs the engine's simulation, builds a lot's scene from converted data, and draws with the GS.
- Allowed: lower texture quality (4-bit palettes, small sizes), dropped effects (no bump maps, specular, water
  reflection, light-map pixels finer than a tile), loading screens between neighborhood and lot.
- Order of what goes first if the sheet doesn't close: model and texture detail, then texture variety, then object
  variety and lot size. Rather than shrinking lots, limit them to a single floor.
- Storage: DVD only, read on demand, assumed. In practice it will run networked through OPL or Neutrino, so the
  file system is whatever those provide; a local cache on USB is not assumed (USB 1.1 is too slow for anything but
  saves and config, about 1.5 MB/s theoretical, from forum posts, not benchmarked; the memory card is for saves only).

## What the PC measurements say (the starting point, not the target)

| Thing | Measured on the PC |
|---|---|
| Heap, neighborhood shown | 287 MiB |
| Heap, lot shown | about 1000 MiB (a lot adds about 700 MiB) |
| Lot scene | 199 MiB of decoded RGBA images (386), 25 MiB of meshes, 33 MiB of terrain data |
| Lot geometry | 631,450 vertices, 748,815 indices (about 250,000 triangles), 2,114 primitives, 1,370 instances |
| Package index | about 113 MiB of entries plus 53 MiB of lookup map, kept more than once |
| Binary | 7.25 MB of code (x86-64 release, with SDL3, FreeType and Lua) |
| UI | 1,558 sprites, 72.7 MiB as RGBA |

None of these carries over directly: the PC holds decoded floats and RGBA, the PS2 data would be compact from the start.

## Toolchain check (done, 2026-10-24)

ps2dev under `$PS2DEV`: `mips64r5900el-ps2-elf-g++` is **GCC 15.2.0**; ps2sdk, gsKit and a `dvp` directory are present. One
small C++20 test file for each library feature the engine uses, compiled with `-std=c++20 -D_EE -O2 -G0` and linked
with `-lkernel -lc` (scratch files, not the repository's build):

| Feature | Result |
|---|---|
| ranges, span, optional, variant, bit, numbers, chrono, exceptions, iostreams, unordered_map, std::function, stringstream | compile and link |
| `std::to_chars` for integers | compile and link |
| `std::filesystem` (path, exists) | compiles and links; **runtime behaviour unchecked** (108 files use it) |
| `std::thread` | compiles and links (ps2sdk provides `pthread_create`) |
| `std::format` | **GCC internal compiler error** in `std::__countl_zero<unsigned __int128>` (the header's 128-bit integer path). **Works with `-std=c++20 -U__SIZEOF_INT128__`** (for integers and strings) |
| `std::format` of `float`/`double`, `std::to_chars` and `std::from_chars` for floating point | **not available** (`std::formatter<double>` is deleted; no floating-point charconv). The engine has 98 `std::format` calls in 22 files, 33 with a floating-point spec in 11 files (mostly logs and diagnostics, some in `sim/walk_path.cpp` and `scene/walls.cpp`) |
| `std::atomic`, `std::mutex`, `std::async` | compile, but **do not link**: undefined `__atomic_fetch_add_4`, `__atomic_test_and_set`. Needs a small shim of those helpers (the EE has no load-linked/store-conditional as I recall: **unverified**; interrupts can be masked instead). 14 files use these |

So the language level is fine. The library gaps are three small, local problems: a compiler flag, a float-formatting
helper, and an atomics shim.

Not yet checked: how big the code is when built, and whether `std::filesystem` and exceptions behave at run time (needs an
emulator or the console).

## Memory sheet: 32 MB of main RAM

The first three columns are facts about the PC; "target" is what this sheet asks the PS2 build to fit in. All targets
are **assumed** until the measurement named in the last column is done.

| Item | PC now | Target (MB) | Means | To measure |
|---|---|---|---|---|
| Kernel and reserved | n/a | 1.0 | spec (unverified) | check the ps2sdk memory map |
| Code and static data | 7.25 MB | 6.0 | size-optimised build, no SDL/FreeType, strip exceptions' tables if the toolchain allows | a PS2 build's size |
| Stacks, allocator overhead | n/a | 1.5 | | |
| GS packet and DMA buffers | n/a | 1.5 | double-buffered display lists | |
| Audio staging (SPU2 has its own 2 MB) | n/a | 0.5 | spec | |
| Scene textures in RAM | 2.0 MiB at 4-bit, after resizing (measured with the preview settings) | 2.5 | converted offline; 4-bit palettes; streamed to the GS | working set per frame |
| UI atlases | 72.7 MiB RGBA | 3.0 | few large 4-bit atlases at 640x480; drop unused sprites | which sprites are ever drawn |
| Meshes | 25 MiB (floats and 32-bit indices) | 4.0 | 16-bit positions and indices, level of detail, culling | triangle sheet below |
| Neighborhood and terrain | 40 MiB of terrain data | 1.5 | terrain pictures at 4-bit, coarse grid | |
| Content (object definitions, code, strings) | about 170 MiB of index alone | 4.0 | compact on-disc index, loaded per lot | base-game entry count |
| Simulation state and Lua | not measured | 3.0 | | heap by subsystem |
| **Total** | | **28.0** | 4 MB of slack | |

The weak spots are the three "not measured" rows: content, simulation and code size. If any is off by a factor of two
the sheet no longer closes.

## Geometry and speed

**What the GS can take** (manual): 2.4 Gpixel/s untextured and 1.2 Gpixel/s textured, with Z buffer and blending
on; triangle setup 37.5 M/s for textured Gouraud triangles (4 cycles each). At 30 fps that is 1.25 M triangles
and 40 M pixels a frame, against a 640x448 screen of 0.29 M pixels. The GS is not the limit.

**What limits it**, from secondary sources (not Sony documents, so treat as rough):
sustained rates of 15 to 20 M polygons/s are quoted for real scenes, which is 500,000 to 650,000 a frame at 30 fps for a graphics
demo; the GS's input bus (the GIF) is quoted as 64 bits at 150 MHz, about 1.2 GB/s, and as the bottleneck for submitted
geometry. A textured Gouraud vertex is three 16-byte registers in packed form (**spec**, unverified here), so about
48 bytes a vertex, 144 a triangle in a list and nearer 48 in a strip. A frame of 400,000 list triangles would be
about 58 MB, more than the bus carries in a frame; the same as strips, about 19 MB, would not. Vertices are meant to
be stored compactly in RAM and expanded by VU1 straight to the GIF.

**What the engine spends before the GS.** Not measured, and where the real cost is:
- Transform, clip and light on VU1 (microcode to write; no documented cycles-per-vertex figure found).
- This engine lights every vertex on the CPU (`render/cpu_light`, eight lights with ranges and spots). On a scalar
  295 MHz core that is the most expensive thing in the frame; it has to move to VU1, be cut to a few lights, or be done per object for
  small and distant things.
- The simulation, UI and streaming share the same EE time.

**Where a lot's triangles are** (measured, one lot, every instance counted, 414,000 in all; 249,000 with each mesh once):

| Kind | Triangles | Instances | Note |
|---|---|---|---|
| Ground (terrain mesh) | 81,000 | 1 | one dense mesh, 81 k triangles |
| Fences | about 102,000 | about 410 | rail 368 triangles x 182, post 166 x 188, brick rail 124 x 44 |
| Other objects | about 206,000 | about 280 | plants (poppy 1,051 triangles x 12), windows (996 x 9), chairs, shrubs |
| Floors | 11,000 | 11 | |
| Roofs | 5,000 | 7 | |
| Walls and hubs | 4,000 | 572 | |
| Skirt | 1,000 | 2 | |

Most of it is not what the player looks at. Three cuts, all offline, would take it to a tenth:
1. **Ground**: a coarser grid (one vertex a tile or a decimated height field) instead of 81,000 triangles. Neighbouring
   lots need not be shown. Likely 4,000 to 6,000 triangles.
2. **Fences and plants**: a 368-triangle rail and a 1,051-triangle poppy are far more than a PS2 screen can show. Simplified models of
   10 to 100 triangles, and a distance cut-off. Fences alone are 100,000 triangles today.
3. **Objects generally**: an average of 444 triangles an instance now; level-of-detail models bring the near ones to 100-200 and the far
   ones to 20 or none.

A first estimate after those: 70,000 triangles with everything in view and 30,000 to 40,000 in a typical view, with unique
mesh data of about 100,000 vertices (1.6 MB at 16 bytes). That is **assumed**: the real figures come from the converter.

Memory, not speed, is why the unique geometry matters: the lot today has 631,450 unique vertices, about 10 MB even at
16 bytes a vertex.

**Transform and lighting on the vector units.** Yes, and it is how the machine is meant to be used.
- VU1 (micromode only, with extra buses to feed the GS without using the main bus; Copetti's PS2 article) is the geometry
  unit: it runs microcode that transforms, clips, culls and lights a batch of vertices streamed in by DMA, and writes the
  GS packets out directly. VU0 is the CPU's coprocessor in macro mode (and can run microcode too), so it suits per-object
  and animation work: matrices, skinning, preparing batches for VU1.
- ps2gl's source (`vu1/*.vcl`, LGPL 2.1, with the assembled `.vsm` files) is a worked example: microcode that
  transforms with a matrix, clips, back-face culls, and lights with directional, point and spot lights (counts
  kept in VU memory), in variants for triangles, quads, specular, per-vertex colour and indexed input, with double-buffered
  input and output (three quadwords a vertex each way). It is a starting point, not a drop-in: its light model is GL's, and this
  engine's is a sum of eight lights with a range cut-off, `1/(1 + falloff x distance squared)`, and a spot cone in cosines
  (see `render/cpu_light.cpp`), which needs microcode of its own. It does no skinning.
- What I could not verify here: VU1's memory sizes and cycles per vertex, and whether the ps2dev toolchain ships the VU
  assembler and the VCL optimiser. Both decide how hard the microcode is; a prototype on real hardware answers them.
- Not everything needs per-frame lighting. Floors, walls, terrain and scenery change light only when the time of day or a
  lamp does, so their vertex colours can be computed on a change and kept; only Sims and the objects near them need it every frame.

**CPU.** The R5900 is about 295 MHz (**spec**). The simulation (SimAntics VM, pathfinding, Lua UI) has not been timed on
anything slow. The iMac G3 is a rough proxy (a different design, about the same clock): time a simulation tick and
a scene build there.

## What exists already

- A renderer path with no shaders (`--renderer gl1`): per-vertex CPU lighting, one texture per pass, 16-bit indices,
  arrays without stride, power-of-two textures, 4/8-bit palette preview, a console-subset checker (`NEOTS2_GL1_STRICT`).
  It runs on a PC; it is the reference for what a GS backend has to draw.
- `render/cpu_light`, `render/fixed_layout`, `render/texture_fit` are API-independent and testable.
- A platform layer (`src/platform`) with an SDL 1.2 backend; a ps2sdk backend would sit beside it.

## Work, in order

1. **Toolchain.** Done as far as compiling goes (see the check above). Next: cross-compile the engine's headless libraries
   (formats, content, scene, sim) for the EE and fix what breaks; then run something on an emulator or the console.
2. **Measurements.** Binary size on PS2; heap by subsystem (content, simulation, Lua); the geometry sheet; which UI
   sprites are drawn; the simulation tick on a slow machine.
3. **Converter (host tool).** Reads the base-game packages and writes: textures (4/8-bit, sizes chosen by the screen
   density rules already measured), meshes (quantised, 16-bit, with levels of detail), object definitions and code,
   strings, UI atlases, and a compact index. Reuses the engine's package and scene code on the PC.
4. **Runtime formats and loaders (EE).** Read the converted data without decoding; keep the index in a form that
   needs no per-entry allocation.
5. **GS backend.** Over gsKit, drawing what `gl1` draws: textured triangles with vertex colour, additive and alpha
   blends, a second pass for floors if needed, the sky as a box. Palette textures through the GS's CLUT.
6. **Platform backend** for ps2sdk: pad, DVD or hard drive, timing, audio.
7. **Simulation on the EE** with the compact content; profile; cut.
8. **UI** from atlases.

Milestones: M0 toolchain and platform hello-world; M1 one converted textured mesh drawn; M2 the neighborhood view;
M3 a lot view with fixed lighting; M4 a simulation tick; M5 playable loop with UI.

## Open questions for the owner

- Which GCC and libstdc++ the ps2dev toolchain gives today, and whether `std::filesystem` and `std::format` work.
- (Answered: detail first, then texture variety, then object variety and lot size; single-floor lots rather than smaller ones.)
- (Answered: DVD only, networked through OPL or Neutrino in practice.)
