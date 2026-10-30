# PlayStation 2 port: memory sheet and plan (draft)

Status: a draft to work from, not a design. Every number says where it comes from:

- **measured**: from a run on the Linux PC (logs, heaptrack) with this repository's code;
- **manual**: the GS User's Manual v6.0 (checked);
- **spec**: a well-known PS2 figure that has not been checked against a source here;
- **assumed**: a target or guess, to be replaced by a measurement.

## Scope

- Target: the base game as the OS X release has it (no expansions), converted offline on a PC into compact data.
- The PS2 build runs the engine's simulation, builds a lot's scene from converted data, and draws with the GS.
- **Decided: players build and edit lots on the console itself.** So the scene builder (`ts2scene`), the build catalog
  and the lot-lighting code stay in the console build and are not moved into the converter. What the converter can do is
  prepare what the builder reads: see "The seam" below.
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

## The package index, measured (`ts2index`, Legacy Collection, objects packages)

| | Entries |
|---|---|
| All 20 objects packages together, read | 1,593,198 |
| Overridden by a later package | 1,429,222 |
| Unique resources after overrides | 163,976 |
| **Base game's own `objects.package`** | **52,208** |

Each expansion's package repeats most of the earlier ones' entries, which is why the PC keeps 1.6 million entries for 164,000 resources. The converter
reads the base game only, so the console's index is about 52,000 entries: with 24 bytes each, about 1.3 MB flat and far less if it is
loaded per lot. (The scene and texture packages are indexed separately and have not been counted.) PC load time on this server: 2.3 s cold, 0.5 s warm.

## Saves, measured (N001 from the owner's PC, 73 MB; 26 lots, 170 Sims)

| Part | On disk | What is in it (packed bytes) |
|---|---|---|
| Lots, 26 files | 31 MB (0.5 to 2.4 MB each) | images 17.2 MB (56%: lot thumbnails, 231 of them), placed objects 4.0 MB (13,596), textures 3.6 MB (277), object extras 3.1 MB, directory 0.7 MB |
| Characters, 170 files | 35 MB (about 0.2 MB each) | meshes 17.9 MB (554 GMDC, embedded copies), another 8.9 MB (119 resources, kind not identified), images 3.4 MB, strings 1.8 MB |
| Neighborhood package | 1.7 MB | 1.4 MB packed: 0.65 MB in one kind of 280 resources, relationships, family data |
| `.reia` file | 6.1 MB | not examined (probably a picture or video of the neighborhood) |

What a console save needs is much less than what the PC writes: no thumbnails (56% of the lots), no embedded character meshes (the
console has the shared ones), no `.reia`. Without them a lot's description is about **0.37 MB** (lots minus images and textures: 9.7 MB / 26) and
a Sim about **0.05 MB** (characters minus meshes and the unidentified kind: 8.6 MB / 170), **estimated** from these totals.

**A smaller neighborhood (3 or 4 lots) would help**, with limits:
- Save size: about 4 lots x 0.37 MB + 12 Sims x 0.05 MB + a small neighborhood package is **2.5 to 3.5 MB**, in the PC's formats
  and without thumbnails perhaps 5 to 6 MB. Either fits an 8 MB card (the full 26-lot hood does not, at any compression of this kind).
- Neighborhood view: the PC holds 287 MiB of heap with the 26-lot, 128 x 128 tile neighborhood shown (terrain pictures 32 MiB, 763 plants, 30 decorations,
  26 imposters). A smaller terrain and fewer lots shrink most of that in proportion.
- It does **not** change a lot's own cost: the lot scene (199 MiB of images, 631,000 vertices, the simulation) is what limits the console, and
  four lots are as big as twenty-six when you stand in one.
- Whether such a neighborhood can be made in the game as it is, I do not know: the game's neighborhoods come from terrain templates. The console build
  could ship its own small templates (a design choice; **unverified** that the data format allows it).

The OS X edition (`/srv/files/sims2-osx`, 2.9 GB, 807 packages, `TSData/Res` layout) is read by the engine's index tool unchanged: 52,208 entries in the
base objects package, the same as the Legacy Collection's base package. The PowerPC edition is the converter's input.

## Headless measurements (arch-server, `build-headless`; lot 68 of N001, 4 Sims)

`tools/lotscene` builds a lot's scene as the game does, with no window (`--stub-pixels` makes every texture one white pixel);
`tools/simrun` runs the simulation. Server CPU: boosts to 4.4 GHz.

| Lot 68 scene build | Peak memory | Build time (CPU) |
|---|---|---|
| Legacy Collection, real textures | 350 MiB | 1.9 s |
| Legacy Collection, pixels stubbed | 198 MiB | 1.6 s |
| OS X base game, pixels stubbed | **112 MiB** | 0.8 s |

- Texture pixels are 152 MiB of the 350 MiB. Nothing broke with one-pixel textures and there were no warnings: the builder does not depend on picture contents.
- `Assets` costs 110 MiB with the Legacy Collection and **19 MiB** with the base game alone (the expansions' repeated index entries).
- What is left in the stubbed scene (heaptrack, base game, peak 108 MiB): `terrainMaterial` 25 MiB (six 1024 x 1024 ground textures composed for the
  lot; earlier I called these light maps, which was wrong: the floor light maps are 2.5 MiB), the index 17 MiB (map 12.9 + entries 4.3),
  `bumpNormals` 11.5 MiB (normal maps: not needed on the console), the models being parsed (`scenegraph::floats`) 11 MiB, vector and image copies 9 MiB,
  the room map 2.9 MiB. The finished meshes are 22 MiB (556,000 vertices, 692,000 indices).
- Build time on the first (cold) run was dominated by reading files, not by building.

| Simulation of lot 68 | Peak memory | CPU |
|---|---|---|
| Legacy Collection, load only | 292 MiB | 2.1 s |
| Legacy Collection, per tick | +10 MiB over 3,000 ticks | **0.27 ms** (4 Sims) |
| OS X base game | 68 to 74 MiB | 0.3 to 0.4 s load |

Heaptrack of the simulation alone on the base game (peak heap 61 MiB): the index 28 MiB (map 20.7 + entries 7.5, 234,640 entries in all the packages the
simulation opens), parsed behaviour scripts 4.7 MiB (32,291 of them), the route grid 4.3, the room map 2.9, object types and behaviour lookups 6.5, world restore 1.7.
The per-tick figure is on a fast modern core. The EE (about 295 MHz, in-order, small caches) is likely 30 to 100 times slower per thread: 8 to 27 ms a tick,
against 33 ms if the game runs 30 ticks a second. **That is an estimate, not a measurement**, and it is for four Sims and no rendering. It
says the simulation is probably within reach at normal speed and not at the game's faster speeds. The iMac G3 can check the estimate: `simrun` builds on PowerPC.

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

## The seam: what the builder reads, and what it must stop making

`scene::Assets` is where the builder meets the game's files:
- `byName` and `byKey` return a resource's raw bytes (models, materials, object definitions) which the builder parses with
  `ts2formats`; and `texture(...)` returns a **decoded RGBA `Image`**.
- A converter can stand behind it: a converted asset bank with the same logical resources, models simplified offline,
  textures already palette-based and sized for the console, and an index that needs no per-entry allocation. The parsing code stays.
- What cannot stay is the RGBA `Image`: the PC run holds 199 MiB of it for one lot (386 images), plus a second copy
  while uploading, plus runtime texture composites (`layeredImage` 24 MiB, `terrainMaterial` 25 MiB at peak, wall masks, blends).
  On the console a scene should hold texture *handles* into a bank, and anything built from several textures should be
  baked by the converter or composed in palette space.
- Editing means incremental work: moving a wall or a floor must not rebuild the lot. The builder rebuilds the floor and
  wall meshes of a level as a whole today. On the EE (about 295 MHz, against a modern PC core) that is likely tens of times slower, so
  the lot load (1.6 s on the PC) may take a minute or more, and an edit needs to touch only what changed. **Not measured.**
- Code: at `-Os` the core libraries are 3.65 MiB (sim 1.19, scene 1.07, content 0.46, formats 0.41, renderplan 0.15, diag 0.15, Lua 0.11,
  images 0.06, paths 0.04); the 14,000 lines of `app/` and the UI are not built yet. Overlays (ERL) could keep the neighborhood and lot
  code from being resident together.

## Modes behind loading screens

Idea (from the owner): load the builder's code as an overlay, behind a loading screen. Two ways to do it, for different joins:

| Join | How | Why |
|---|---|---|
| Neighborhood <-> lot | Separate programs (ELFs), each linking only what it needs; a loading screen runs the next one | Everything is on disc anyway, so nothing is lost by dropping RAM; no relocation, no C++ across module boundaries |
| Lot live <-> lot build | ERL overlays (ps2sdk `liberl`, `.erl` files), or a save-and-restart of the lot's state | The Sims, objects and relationships must survive the switch |

What it buys:
- **Code RAM**: the neighborhood's code (hood scene, paint, decals) and the lot's need not be resident together, and live play (sim) and building
  (floors, walls, roofs, catalog) need only part of each other. Likely 1 to 2 MiB of the 3.65 MiB (`-Os`) core, **estimated**.
- **Time-multiplexed data RAM**, the larger gain: the builder's working memory (parsed models, decoded textures, models before
  they are simplified) is needed while building, not while playing. With a loading screen between them the sheet need
  only close for the larger of the two, not their sum.
- **Load time**: a whole-lot build of a minute or more is acceptable behind a loading screen.

What it does not do:
- It does not make editing fast. A player placing a wall will not wait through a loading screen for each one; edits must be
  incremental (rebuild only what the change touches) even with the builder resident.
- It does not shrink the data: textures, meshes and the content index still need the converter.
- It cannot be assumed to work: C++ across ERL boundaries (exceptions, vtables, static initialisers, shared libstdc++) is
  **untested**. Cheapest test: a small ERL that throws and catches, uses `std::vector` and `std::format`, called from a
  host program under PCSX2.

## Baked lots (caching the build)

Idea (from the owner): with the editor behind a loading screen, build a lot once and keep the result.

- **Baked lot**: the built architecture (floors, walls, roofs, the ground mesh, resolved materials and texture handles) plus the room graph and route
  grid, in a compact form that live mode loads directly, without running the builder. Objects stay separate (instances that
  move), and so do the lot's light maps, which depend on the time of day and the lamps (the PC relights as the clock moves on).
- **Where it is made**: for existing lots, **on the PC by the converter, running this same scene builder** (it already builds a lot in 1.6 s there).
  On the console, entering build mode loads the builder; leaving it re-bakes what the player changed.
- **What it removes**: the minute-or-more lot build from the common path (entering a lot to play), and the builder's code and working memory
  from live play altogether (the time-multiplexing above, taken further).
- **Size**: the estimate for one lot is about 1 MB (about 25,000 triangles of architecture and ground at 16 bytes a vertex is a few hundred KB, plus the
  graphs and light data). **Not measured.**
- **Where it lives: a real memory card (the owner's choice; a network virtual card is possible but not preferred).** Consequences:
  - An official card is 8 MB (**spec**, unchecked here), shared with the lot saves themselves. At about 1 MB a baked lot, only a handful of *edited* lots fit
    beside the saves; lots the player has not changed are baked by the converter and read from the disc or network, never from the card.
  - Speed is unknown: no measured read or write figures for the card were found (only a bus ceiling for SD adapters, which is not the card). Write only when
    leaving build mode, never per edit, both for speed and because flash wears. **A write/read benchmark on the real console is a prerequisite**
    (ps2sdk's `libmc`; rickgaiser's `ps2-rw-speed` is a ready-made tool, unverified).
  - What the *saves* are, and how big, decides whether even they fit on 8 MB: a Sims 2 neighborhood save may be larger than the whole card. Open.
- **Invalidation**: a baked lot is valid for one version of the lot's description (and of the converter); an edit or a changed asset makes it stale.

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

## Runtime checks on PCSX2 (tools/ps2test)

- `atomics_test.elf` (interrupt-masking `__atomic_*` shim): exceptions, `std::atomic`, `std::mutex`,
  4 `std::thread`s, `std::async` and `std::filesystem::path` all pass under PCSX2.
- `memcard_bench.elf` works functionally (data verified). PCSX2 reported ~370 KiB/s write and
  ~950 KiB/s read at 16-64 KiB chunks; this is the emulator's transfer model, not flash timing.
  No published figures exist for an official card, so real numbers need the console and a genuine card.
- Headless recipe: Xvfb + `vulkan-swrast`, with `Renderer = 11` (Null) and `Adapter = llvmpipe (...)`
  in PCSX2.ini, then `pcsx2-qt -batch -nogui -elf <file>`; EE stdout shows in the log.
  PCSX2 opens a Vulkan surface even for the Null renderer, and RADV cannot present on Xvfb.

### ERL overlay test (tools/ps2test/erl)

Setup: this ps2sdk ships no `erl-loader.elf` or `libc.erl`, so the host ELF links libstdc++ and registers the
module's undefined symbols (generated from `nm -u`) with `erl_add_global_symbol`, then `load_erl_from_mem`.
Run under PCSX2.

| Check | Result |
|---|---|
| Load, symbol lookup, plain call | pass |
| `std::vector` (new/delete shared with host) | pass |
| `std::format` with ints, module built `-O0` | pass |
| `std::format`, module built `-O1/-O2/-O3/-Os` | **fails** (`format_error`; wrong data) |
| `throw`/`catch` inside the module | **fails** (`terminate`: unwinder cannot see the module's frames) |

Causes seen in the log: liberl refuses 14 unaligned `R_MIPS_32` relocations in `.eh_frame` (the LSDA pointers
inside FDEs), so registering the module's `.eh_frame` with `__register_frame_info` does not help; `-fPIC` is not
available on this target. The optimised failures coincide with 108 `R_MIPS_HI16` against 112 `R_MIPS_LO16`
relocations (several LO16 sharing one HI16), which liberl probably pairs wrongly. Stripping with
`--strip-unneeded` also breaks string-literal relocations (`.LC*` labels), so only `--strip-debug` is safe.

Conclusion: ERL is not a safe vehicle for the editor overlay as it stands. Keep ELF-per-mode (a separate
executable per neighbourhood/lot/build mode, switched by loading it) as the plan, and treat ERL as
optional (fixing liberl's relocation handling would be the work to make it viable).

## Native OS X saves, and the converter's first step (2026-10-24)

**Base-game lots against the OS X install** (`/srv/files/s2save-osx`, 3 neighborhoods + the tutorial): all 52 lots of N001-N003
build a scene with `lotscene --stub-pixels` and no failures. Peak heap 26-137 MiB (pixels stubbed), geometry 0-999k triangles
if every instance were in view (median about 140k). The only asset warning, on 5 lots, is `treebamboo-bump` (a bump map the base
game does not have; unused on the console anyway). So the base-game lot loads cleanly; nothing there is missing.

**`ts2bank`** (`tools/ts2bank`, format in `src/content/bank.hpp`): builds the scenes of the lots it is given, records every
resource read through `Assets` (`Assets::observeReads`) and writes them to one bank file: 32-byte entries sorted by key (binary
search, no allocation), blobs 16-byte aligned and decompressed. `Bank` reads it in place. Round trip verified. What a bank holds,
before any conversion:

| Lots | Resources | Data |
|---|---|---|
| N001 Lot 11 (567k triangles) | 5,038 | 76 MiB |
| all of N001 (26 lots) | 10,860 | 248 MiB |
| all of N002 (11 lots) | 9,528 | 225 MiB |
| all of N003 (26 lots) | 9,043 | 207 MiB |

Where it goes (N001): texture pixel data (type `0xED534136`, 1,127 resources) 168 MiB; meshes (`GMDC`) 63 MiB; the rest under 10 MiB.
That is what the converter has to shrink: textures to console sizes/palettes, meshes to the triangle budget. It fits the disc
many times over; the point is what a lot reads at once. Not yet in the bank: what the simulation reads (behaviours, object
code), character files, UI.

**One base bank from every lot** (all 69 lots of N001-N003 and the Tutorial, one `ts2bank` run, 29 s, peak 1.3 GiB):
14,709 resources, 461 MiB. Texture pixel data (`0xED534136`, 1,736 resources) 316 MiB, meshes (`GMDC`, 1,097) 97 MiB,
the rest 48 MiB. Against the per-neighborhood banks (207-249 MiB each) that is the real size of what the base game's saved lots
use; it grows only with lots that bring other objects. Unconverted.

**Texture conversion** (`ts2bank --textures [--max-side N] [--palette-error E] [--dither]`, format in
`src/content/console_texture.hpp`): each TXTR is decoded, resampled to a power of two no larger than `--max-side` (default 256),
and stored as 4-bit if a 16-colour palette changes it by at most E RMS levels (default 8), else 8-bit if 256 colours change it by
at most E/2, else 32-bit. Bump maps and cube maps are dropped. The large-level (LIFO) resources fold into the texture. The
smaller levels follow (halved down to 8 texels, one palette for all levels, each level made from the picture and mapped to the
nearest palette colour). Results for the all-lots base bank (1,868 textures):

| Setting | Textures | Bank (all resources) |
|---|---|---|
| unconverted | 349 MiB | 461 MiB |
| `--max-side 256`, error 8 | 62 MiB (4-bit 1,470; 8-bit 216; 32-bit 80; 102 dropped) | 174 MiB |

RMS error over the textures: mean 2.8, max 8.0 levels. Meshes are now the largest part of the bank (97 MiB). Not checked by eye
yet: which 80 textures stay 32-bit, and how the 256 cap looks on Sims and objects. One lot's textures (N001 Lot 11): 54 MiB -> 9.6 MiB
at 256, 29 MiB at 512 with error 5. Runtime composites (Sims' skins, light maps) are not covered; see the discussion of runtime
pixel edits.

**Mips in the console textures.** The all-lots base bank with mips: textures 82 MiB (62 MiB without, as predicted +33%), whole bank
194 MiB. Contact sheets of original and converted textures (the 80 that stay 32-bit, the 54 with the largest error, a sample)
were looked at: the palettised ones look right in the sample; the worst 4-bit ones (error 7-8) show some banding on soft
gradients (a green panel, a bench's shading), which 8-bit or dithering would fix at some cost. The owner judged them good enough;
the thresholds can be revisited (`--palette-error`, `--dither`). The mip levels themselves have not been looked at.

**Mesh conversion** (`ts2bank --mesh-keep K [--mesh-error F] [--mesh-dump DIR]`; `src/content/mesh_decimate.*`, `writeMesh` in
`src/formats/scenegraph.*`): every GMDC is parsed, its triangle-list groups reduced by edge collapses (quadric error; the
surviving vertex keeps its own attributes, including blend weights, so skinned groups are reduced too; vertices at one place on a
texture seam collapse together; open edges are penalised; each connected part keeps at least 0.75 x K of its triangles; an
error cap stops collapses that would move the surface more than F of the group's size) and written back as a GMDC that the
existing parser reads (`writeMesh`; all 1,097 meshes round-trip). Groups under 24 triangles are left alone; morph elements the
engine does not read are dropped where they can't follow the vertices. Writing only what the parser reads halves the mesh
resources (96.8 -> 46.4 MiB) before any reduction.

All-lots base bank, unique meshes (844k triangles, once each): `--mesh-keep 0.2 --mesh-error 0.1` leaves **42%** (58% removed),
37-42 MiB of meshes. The owner accepted that setting for now (contact sheets of before/after for a sample were looked at; thin parts
survive, soft curves get faceted). A stricter cap keeps more: 0.003 leaves 75%, 0.01 63%, 0.03 56%, no cap 45%.

## gsKit backend: groundwork (2026-10-24)

`tools/ps2test/gs_hello.cpp` draws one picture as 4-bit, 8-bit and 32-bit textures with `gsKit_prim_list_triangle_goraud_texture_uv_3d`
(Gouraud, Z buffer on) and all three come out identical in PCSX2. Facts it settled:
- The mode is gsKit's default, **640x448 interlaced, FIELD** (`GS_MODE_NTSC`, 16-bit signed Z, double buffered). Setting
  `Interlace = GS_INTERLACED` with `Field = GS_FRAME` drew nothing, and non-interlaced with a 448 height stretched everything 2x.
- A 4-bit texture's palette is 16 entries uploaded as an 8x2 CT32 block; an 8-bit one is 256 entries as 16x16, **with entries 8-15 and
  16-23 of each 32 swapped** (the GS's CSM1 layout) before upload. Palette alpha is 0x80 for opaque; vertex colours 0x80 are 1.0.
- UVs are 12.4 fixed point; positions go through `vertex_to_XYZ2` (it applies the screen offset).
- Headless capture: `tools/ps2test/run_pcsx2.sh <elf> <seconds> <out.png>` runs PCSX2 on Xvfb (software renderer, llvmpipe adapter set
  in PCSX2.ini and put back afterwards) and grabs the display with ffmpeg. The PCSX2 window shows the 640x448 picture at the top left.

The renderer will be a `GsRenderer : RendererState` in `src/render/ps2/`, carrying out `FramePlan` with gsKit. The EE cannot build a lot
scene itself (the builder peaks at 112 MiB), so a lot is baked by the converter into a console scene file and loaded as it is.

### First picture from the gsKit renderer (2026-10-25)

`src/render/ps2/gs_renderer.*` (`GsRenderer : RendererState`, built by CMake only for the PS2: `build-ps2`, target `gs_lotview`) draws a
baked lot (`ts2bake` -> `readBakedScene`) in PCSX2: N001 Lot 80 (33,244 triangles, 18 images, 4-bit textures) shows its ground, road,
kerb and a lamp, perspective-correct, Z-buffered. How it works now:
- `FramePlan` collects and orders the draws; `vertexColors` (moved out of the GL1 code into `render/vertex_colors.*`) lights each vertex
  on the CPU with the colour scale 0.5, which is also how the GS reads a vertex colour (0x80 = 1).
- Vertices are transformed and clipped on the EE (Sutherland-Hodgman against near, far, a 3x guard band and w > 0), culled by winding,
  and sent as A+D GIF packets with ST and Q (so texturing is perspective-correct), 3000 triangles to a packet (a GIF tag counts at most
  32,767 quadwords; 5,000 hung the GS).
- Textures go through gsKit's texture manager (LRU in the VRAM left after two 640x448 32-bit buffers and a 24-bit Z buffer, about 0.75 MB);
  4-bit and 8-bit ones carry their palette (8-bit swizzled), alpha halved for the GS. Blend, additive, alpha test and Z mask are set per
  draw. Not yet: mip levels, skinned meshes, terrain/roads/water/sky, sprites, light-map multiply.
- Z is 24-bit: depth here runs 1 at the near plane to 0 at the far one, and a 16-bit Z buffer could not separate surfaces 0.01 apart.
- Speed in PCSX2 (software GS on llvmpipe, so only the EE side means anything): about 1.7 s a frame for 31.5k drawn triangles, all on the EE.
  That is the cost the vector unit is for; nothing here is measured on a console.

**Memory first (2026-10-25).** The first N001 Lot 11 run (33 MiB baked file read into one buffer) crashed on the EE's 32 MB, as the sheet
predicted. So the baked file changed: vertices are stored quantised (16-bit positions and texture coordinates over the primitive's
own range, 8-bit normals and colours, 16-bit light-map coordinates), meshes can be reduced while baking (`ts2bake --mesh-keep 0.2
--mesh-error 0.1`: Lot 11 goes from 201k to 86k triangles), and the file is a **head** (everything but pixels, 3.7 MiB for Lot 11)
followed by the **images**, which the console reads from the disc when a draw first needs them (`GsRenderer::setImageSource`, an LRU
kept under a byte budget, never letting go of a texture drawn this frame or the last). Lot 11: 33 MiB -> 20 MiB (meshes 16.2 -> 3.5,
textures 16.5 unchanged: the converter's 256 cap and 4-bit palettes; the console holds only what it draws). Not run in PCSX2 yet
(see below). The renderer still expands the head's meshes to 17 floats a vertex (68 B) in RAM; a compact in-RAM layout is next.

PCSX2 on the GPU session (`:0`): PCSX2 segfaults while opening its render window there (in every renderer and with or without
`-nogui`), almost certainly in its XRandR refresh-rate probe, because that Xorg has no connected output (`xrandr` lists only
disconnected ports). It needs an output (a dummy plug, or an Xorg `ConnectedMonitor` option) to work there.

### N001 Lot 11 on the console (2026-10-25, PCSX2 with its hardware renderer, `:0` made usable by forcing an HDMI output)

The whole lot draws: house, roof, trees, fences, road, kerbs, 331 images (4/8/32-bit, read from disc on demand). What it took:
- The console heap is **27 MiB** in all (probed). Keeping the head's meshes as floats needed 14.7 MiB before any draw buffer and ran out.
  Now each primitive stays as its quantised bytes inside the head (`readBakedHeadCompact`, `BakedMeshes`) and one primitive at a time is
  expanded to the 17-float layout when drawn: **7.2 MiB in use** with the scene prepared, 13.9 MiB of textures resident at the end
  (budget 6 MiB is only enforced for textures not drawn in the last two frames; this lot draws more than that every frame).
- Positions are quantised per axis (so the culling box needs no decoding), the image area starts on a 16-byte boundary.
- **Speed: about 17.7 s a frame in PCSX2** for 3,295 draws and 381k triangles considered, 205k drawn, 177k back-face culled. That is
  the EE doing lighting (`vertexColors`, eight lights a vertex), transform, clip and packet building for every vertex of every draw, every
  frame, plus expanding each primitive's bytes. It is far too slow whatever the exact ratio between PCSX2 and a real console. Next:
  profile the phases; cache or bake the vertex lighting (it does not change with the camera); cull at instance level; move transform
  to VU1.

**Speeding up the EE side (2026-10-25).** Profile of the first full-lot frame (PCSX2, 17.9 s): packet building 10.5 s, expanding primitives 3.3, vertex lighting 3.3,
transform 0.6. Changes and results, same camera, N001 Lot 11 (about 377k triangles in the draws, 203k drawn, 1.29 M vertices):

| Step | Frame |
|---|---|
| first full-lot frame | 17.9 s |
| each vertex projected once into a ready 48-byte GS vertex; triangles copied into packed-format GIF packets written straight into the draw queue; transform straight from the 16-bit coordinates (no expansion) | 4.0 s |
| lit vertex colours cached per instance and primitive, keyed by a hash of the lights/material/place (5 MiB budget; 3 MiB was too small and left 543 draws re-lit every frame) | 2.0 s |

Left per frame: transform 1.46 s (1.29 M vertices), emit 0.26 s, the rest small. Vertex counts are inherent: the meshes have 2.2 vertices per
triangle (hard edges and texture seams), welding identical vertices removes only 12% (kept: 0.4 MiB smaller). The remaining work is the vector unit
(transform, clip, cull, light and build packets on VU1), and drawing fewer vertices (distance LODs, smaller instances). A 16-bit colour buffer and Z
would give the textures about 2.5 MiB of VRAM instead of 0.75 MB. Format is version 5: every array aligned for the type it is read as.

**The vector unit (2026-10-25).** VU1 draws now (`src/render/ps2/vu1_soup.vsm`, built with `dvp-as`; `drawListVu`). Design, as it runs in PCSX2:
- Primitives are **triangle soup** (three corners a triangle, no indices); at load the baked indexed meshes are expanded into per-corner arrays the
  VIF can unpack straight from memory: positions as 4 x 16-bit (8 B), texture coordinates 2 x 16-bit, normals 4 x 8-bit, optional vertex colour and light-map
  coordinates. 16-byte aligned, padded to 12 corners with degenerate triangles.
- Per draw, one DMA chain on VIF1 (an END-terminated REF chain built by the EE, one at a time, two slots so the EE builds the next while the last is sent):
  constants (matrix with the positions' range folded in, screen/uv/guard scales), then per batch of 156 corners: the GS state packet and vertices' GIF tag (UNPACK to
  the double buffer's head), then positions, texture coordinates and colours unpacked with `STCYCL CL=3, WL=1` (write one quadword, skip two) so
  the three arrays land interleaved as the GIF's packed ST/RGBAQ/XYZ2 expects, `MSCAL`. The microprogram transforms in place (itof, matrix, clip flags,
  1/w, scale, ftoi) and `XGKICK`s the buffer: no copy, no indices. Corners outside the 3x guard band drop their triangle (ADC bit).
- Lit colours are per corner (r, g, b, alpha/2), computed by the same `vertexColors` in chunks and cached (5 MiB).
- Textures: gsKit's texture manager is not used (its uploads go through another GIF path and made the EE wait for the VU1 each time). A ring over the VRAM the
  buffers left; an upload (image, palette, TEXFLUSH) goes at the head of the chain of the first draw that needs it, after a `FLUSHA`. Opaque draws are sorted by texture.
- Not yet: back-face culling on the VU (every face is drawn), clipping of triangles that cross the guard band (they are dropped: needs an EE path for
  primitives that cross the frustum, or a bound on triangle size from the converter), and the 15 microprogram cycles a corner could lose by software pipelining.

N001 Lot 11, same camera: 17.9 s (EE only) -> 2.0 s (EE, cached colours) -> **0.33 s a frame** with VU1 (1.15 M corners, 9,858 batches, 3,295 draws, 276 texture uploads);
EE build 0.12 s, lighting lookup 0.06, DMA waits 0.04 and the rest is PCSX2 running the VU1 and the GS. The remaining EE cost is per draw (uniforms, hashing, chain).

**VU1 path, round two (2026-10-25).**
- *Clipping:* each draw's box is tested against the frustum widened to the 3x guard band (`boxInView` on the matrix with x and y scaled by 1/3). Primitives that
  cross it (near plane, far edges) are clipped and projected by the EE (Sutherland-Hodgman, as before), written as packed GS vertices and sent through the VIF behind a
  `FLUSHA` (`DIRECT`), so they keep their place in the draw order; all the others go to the VU1. Lot 11 from far away: none take the EE path; with the camera at 30%
  of the distance, 12 of 2,055 draws (20,529 corners) do, and the picture has no holes.
- *Back-face culling:* the microprogram now works a triangle at a time (three corners, six loads up front) and tests the cross product of two screen-space edges
  with a `CLIPW` (the sign of z against zero), then `FCAND` over the last four clip results: the guard test of the three corners and the facing. A culled or clipped
  triangle gets the ADC bit on its third vertex, which keeps the GS from drawing it. A per-draw constant turns the test off for double-sided materials. (Checked by
  inverting it once: only back faces were left.) PCSX2 does not model the VU's pipeline stalls, so the cycles per corner cannot be measured there; the program
  is written in a plain order (about 36 FMAC and 8 lower-pipe instructions a corner): software pipelining is left until it can be measured on the console.
- *Lit-colour cache:* the key is now generation counters (the renderer's setters bump a global count for lighting changes and a per-instance count for place,
  materials and lights), not a hash of the uniforms; that hash covered the eye position, so a moving camera would have missed the cache on every draw. With the camera orbiting
  the lot every frame, all 3,295 draws still hit the cache: 0.32 s a frame.

**Baked soup (format v6, 2026-10-25).** `bakeScene` writes each primitive as triangle soup, as the VU1 path wants it (three corners a triangle, padded to a multiple
of 12 with degenerate triangles; positions 4 x 16 bits over each axis's range, texture coordinates 2 x 16, normals 4 x 8, optional colour/alpha and light-map
coordinates, joints and weights for skinned ones, each array 16-byte aligned), and the head is held in a 16-byte aligned buffer (`AlignedBytes`, preamble 16 bytes, the
image area still on a 16-byte boundary), so the renderer points the DMA straight at the head: no copy made at load. N001 Lot 11: file 21.1 MiB (meshes 4.3 MiB: soup
costs 3.0 corners a triangle against 2.2 vertices indexed), heap in use after loading **8.2 MiB** (was 11.0 with the soups built at load), load time 0.1 s (was 0.2).
A bug that the orbiting camera showed: a frame that ends on a blended draw leaves the Z write mask on, so the next frame's clear did not clear Z and stale depth
hid triangles; the mask is reset before each clear.

**Eight lots through the console path (2026-10-25, PCSX2, camera framing the whole lot, `--mesh-keep 0.2 --mesh-error 0.1`).** All baked, read back and drew; none ran out of memory.

| Lot | File | Heap in use after load | Draws | Corners a frame | Frame (PCSX2) |
|---|---|---|---|---|---|
| N001 Lot 82 | 25.2 MiB | 10.8 MiB | 5,428 | 1.56 M | 1.30 s |
| N003 Lot 192 | 33.4 MiB | 10.5 MiB | 3,959 | 1.06 M | 0.37 s |
| N002 Lot 10 | 21.3 MiB | 8.6 MiB | 3,018 | 0.79 M | 0.28 s |
| N001 Lot 68 | 21.6 MiB | 8.4 MiB | 2,750 | 0.68 M | 0.27 s |
| N001 Lot 79 | 14.1 MiB | 7.0 MiB | 1,020 | 0.72 M | 0.10 s |
| N003 Lot 128 | 12.5 MiB | 6.9 MiB | 2,850 | 1.36 M | 0.35 s |
| N001 Lot 47 | 13.5 MiB | 5.9 MiB | 2,013 | 0.38 M | 0.18 s |
| N002 Lot 32 | 0.01 MiB | 2.3 MiB | 0 | 0 | 0.02 s |

Frame time follows draws more than corners (Lot 79 has 0.72 M corners in 1,020 draws and takes 0.10 s; Lot 82 has 5,428 draws): the per-draw EE work (uniforms, chain, DMA wait) is the next thing to cut, e.g. by
merging the many small instances of one mesh. N002 Lot 32 bakes to an empty scene (0 primitives, one image): an empty lot in the save, or its ground comes from somewhere other than the
lot scene; not yet looked at.

## Running the game on the console: where it stands and what is in the way (2026-10-25)

**Done:** `gs_lotview` is now an interactive viewer: the ROM's `SIO2MAN`/`PADMAN` modules, `libpad` port 1, a loop that runs until power-off (`pad` as the last argument), left stick turns and tilts the
camera, L1/R1 or the d-pad's up/down move in and out. It opens the pad port and loops in PCSX2; **the input itself is untested** (no controller or key events can be sent to PCSX2 headlessly, and
PCSX2's default pad may report buttons differently), so check it with a real pad.

**What "the game" needs next, in the order it should be tackled:**
1. **The simulation's memory.** Measured on the host with the OS X base install: `ts2sim` for a lot with four Sims peaks at **61 MiB** (index 28 MiB, `bhav::parse` 4.7, route grid 4.3, room map 2.9,
   object-type lookups ~6.5, world restore 1.7) against a **27 MiB** heap that already holds ~8-11 MiB of baked scene and 14 MiB of textures. So the sim needs its resources from the bank (`ts2bank`: gather what a
   lot's simulation reads, not only its scene), a compact sorted index read in place (`content::Bank` already is one) instead of the per-package hash maps, behaviours parsed on demand and dropped, and the
   texture budget cut (256 px cap -> 128 px for objects, or fewer resident textures) to buy back heap. Target: sim under ~10 MiB.
2. **Run the simulation headlessly on the EE first** (no rendering): `simrun`'s core over a bank, in PCSX2, timing ticks (0.27 ms a tick on the server; the EE is probably 30-100x slower, unmeasured). The Lua 5.0
   runtime and `std::filesystem`/`std::thread` use in `ts2sim` have not been run on the console at all.
3. **Sims in the scene:** skinned meshes are baked (joints/weights per corner) but not drawn; the VU1 path needs a skinning variant (or poses baked per animation) and the rig/animation data in the bank.
4. **Game loop and UI:** a frame loop that steps the sim, moves the camera, and draws the lot; a minimal UI (pie menu, speed buttons, status) as sprites through `GsRenderer::showSprites` (not implemented: the sprite pass
   and a font/UI atlas baked by the converter). Sprites are 1,558 images / 72.7 MiB on the PC; they need the same palette treatment.
5. **Launch into a lot:** one ELF per mode (neighborhood, lot, build), a loading screen, saves on the memory card (compact format, plan above). Starting with the lot ELF started from `host:` or the DVD with a baked lot and its
   bank is the first playable target.
Not started: audio, the neighborhood view (terrain, water, roads, imposters), build mode.

## What the simulation reads (measured 2026-10-25: `simrun <install> --lot <lot> 3000 --measure`, OS X base game, decompressed bytes)

`ResourceIndex::sharedObserver` (new) reports every resource read through any index; `simrun --measure` splits them into reads while the lot loads and while it ticks. N001 Lot 68, 3,000 ticks (the sim stops on two
unimplemented/erroring interactions, as it does on the PC):

| Resource (type) | Load: distinct / bytes | Ticks: distinct / bytes |
|---|---|---|
| BHAV behaviours (0x42484156) | 18,810 / 5.59 MB | |
| OBJD object definitions (0x4F424A44) | 3,484 / 1.08 MB | |
| SLOT slots (0x534C4F54) | 1,771 / 0.83 MB | 1 / 4.5 KB |
| 0xCCA8E925 (per-object tables) | 476 / 0.79 MB | |
| STR# string tables (0x53545223) | 37 / 0.45 MB | 40 / 1.99 MB |
| GLOB globals (0x474C4F42) | 1,380 / 0.14 MB | |
| OBJf function tables (0x4F424A66) | 331 / 0.09 MB | 12 / 3 KB |
| 0xFB00791E | | 123 / 1.11 MB |
| TTAs pie-menu strings (0x54544173) | | 8 / 0.87 MB |
| TTAB pie-menu tables (0x54544142) | | 63 / 0.39 MB |
| BCON constants (0x42434F4E), 0x2CB230B8, FCNS, 0xACA8EA06 | 103 / 0.07 MB | 179 / 15 KB |
| **simulation total** | **about 9.1 MB** | **4.2 MB** |
| models for routing (GMDC 0xAC4F8687, CRES, GMND, SHPE), read by the route-grid builder through `scene::Assets`, not by the simulation | 215 meshes / 17.3 MB + 0.65 MB | |

- Two more lots (N001 Lot 82, N003 Lot 128) read **the same 18.8 k behaviours, 3.5 k object definitions and 1.8 k slots (7.5 MB)** at load, so that part is the whole base game's object code, not the lot's: the simulation parses every behaviour of
  every object type when it loads. During 3,000 ticks those two lots read only 0.06 MiB more (their Sims did little), Lot 68 read 4.2 MB more (pie menus, strings) as its Sims started interactions.
- So what a bank for the simulation holds: BHAV/OBJD/SLOT/STR#/TTAB/TTAs/GLOB/OBJf/BCON, about 13 MB decompressed in about 10 k resources, nearly all of it shared by every lot (it can be one bank on the disc, not one per lot).
  The route-grid models (17 MB) are a converter job: the footprint of each object type baked once (a few bytes each) instead of reading its model at run time.
- Not yet measured: which of the 18.8 k behaviours a lot's Sims actually *execute* (the VM runs the ones objects call; lazy parsing of the rest would cut the 4.7 MiB parse), and the Lua scripts' share.

**Lua (2026-10-25).** `tools/opcodecount <install> 7E` over the OS X base game: **0 of 17,274 behaviours use the Lua-script primitive (opcode 0x7E)**. The OS X install has no `ObjectScripts.package`
either (that is where the Lua modules live in the Legacy collection), and the simulation's only use of the runtime is that primitive (`Execution::luaScript`, `World::scripts()`), so a base-game-only
console build needs neither the scripts nor `lua50` (110 KiB of code, plus the runtime's heap, which is zero today because `loadScripts` returns null here). If expansion content is ever wanted the primitive
must come back with the runtime; until then it can be compiled out.

**Working memory of the simulation (heaptrack of `simrun` on the base install, peak 61 MiB, earlier measurement, not re-run):** package index 28 MiB (hash maps 20.7, entries 7.5), parsed behaviours 4.7, route grid 4.3, room
map 2.9, object-type and behaviour lookups ~6.5, restored world 1.7, remaining ~13 MiB unattributed (strings, per-object state, the lot's load, containers). What a console build needs of that: the index becomes the bank's
sorted table in place (~0.3 MiB for ~10 k resources); behaviours 5.6 MB raw, parsed on demand (how many execute is not yet measured); route grid, room map and the world are per-lot state (about 9 MiB as they are,
compactable: the grids are byte or word per tile); the unattributed 13 MiB needs a fresh trace. A first estimate for the simulation on the console: **10 to 15 MiB working set plus 5.6 MB of resident behaviours**, which does
not fit beside an 8-11 MiB baked scene and 14 MiB of resident textures in 27 MiB without cutting textures and parsing behaviours lazily.

**Where the simulation's 61 MiB went (heaptrack, 2026-10-26; N001 Lot 68, OS X base).** `simrun` builds *three* indices over the same packages (its own, `scene::Assets`', `LotSimulation`'s) and `Assets` besides, so most of the
earlier "index 28 MiB" and the ~13 MiB left over were copies the simulation does not need. `tools/simmem` builds only a `LotSimulation` and ticks it: **peak 34.2 MiB** (heaptrack, 300 ticks, no routing: that needs `Assets`):

| Part | MiB |
|---|---|
| `LotSimulation`'s own index (hash map nodes 9.4, package entries 2.0, `addPackage` 0.5) | 11.9 |
| parsed behaviours and their lookups (`behavior` 4.9, `behaviorByName` 2.8, `namedTree` 1.4) | 9.1 |
| world restored from the lot (`buildWorld`) | 4.6 |
| parsed string tables (`IndexCodeSource::strings`) | 3.1 |
| lot file, object types, objects (`loadLot` 1.2, `objectType` 0.9, `cobject` 0.65), neighborhood 0.4, other | ~4.9 |

With routing (needs `Assets`): route grid 4.7 MiB and route world 0.4 more. What a console build keeps: the index becomes the bank's table (~0.3 MiB, **-11.6**), the 9.1 MiB of parsed behaviours and the 3.1 of strings go if they are stored
pre-parsed and read in place (see below), leaving about **8-9 MiB of world and lot state, +5 MiB with routing**: roughly 13-14 MiB, plus the resident part of the bank.

*Ahead-of-time forms:* native code for behaviours would be bigger than the bytecode (each 16-byte-operand node becomes a call sequence), cannot be paged in place, and gains little speed: a tick runs a few thousand nodes whose bodies (routing, animation,
object search) dominate. What helps is a **pre-parsed, pre-linked** form made by the converter: nodes in a fixed layout with targets, called trees and string references resolved to indices, stored in the bank and read where they lie, so
nothing is parsed or cached at run time (the 12 MiB above) and only the pages that run need be in RAM.

**Which behaviours can run (`tools/bhavclosure <install> <lot> [ticks]`, 2026-10-26, N001 Lot 68, OS X base).**
- Static closure from the 331 present object types (218 groups): their function tables and interaction tables give 4,717 entry trees; following calls (by number: <0x1000 global, <0x2000 the code owner's, else its semi-global group) and
  Run Tree by Name gives **4,408 of the install's 18,810 behaviours, 1.47 MB raw**.
- Run Tree by Name: the name is an immediate operand (index into a string table), never computed. All 1,999 sites in the base game use mode 0, 1 or 2 (group = the stack object's group: bounded by the present types) except one with mode 255; **none use mode 3**
  (group from a GUID in temps). So the name set is static and only the group searched depends on run time.
- Executed in 3,000 ticks: 567 trees, 0.21 MB; 561 are in the closure. The 6 outside are trees of objects **created at run time** (social-interaction objects such as "Social - Memory - Tease", groups 0x7F443754, 0x7F2BAC29) and a few global/semi-global ones they reach.
  Closing the gap: also seed the types that Create New Object Instance sites name (GUID operand), not done yet.
- The save-compatibility check (`typeChangedSinceSave` -> `treeSignature`) is what read 18.8 k behaviours at load; the converter can precompute those signatures.

**Routing footprints (`src/sim/object_footprint.cpp`).** The grid is *not* whole-cell: it is a quadtree whose leaves go down to 1/8 of a tile, over float rectangles. An object's footprint is, in order: a square (OBJD flag, side from OBJD field 0x43 in 1/16 tile, per-side insets from field 0x4D),
else the model's own "footprint" data list (per tile an 8x8 bit mask, 1/8-tile cells, merged into rectangles), else the model's x/y extent clipped to the tile. Rectangles get a 0.3-tile padding item that only the path search keeps clear of, and goal spots are
tested against the footprint. The models are read because the masks (and the extent fallback) live in the model, not in the object definition; the converter can bake them per object type and orientation (8 bytes a tile).

**Follow-ups (2026-10-26).** Seeding the closure with all 144 object types named `Social*` (the objects interactions create at run time) makes it 5,566 trees / 1.95 MB (from 4,408 / 1.47 MB) and covers 564 of the 567 trees executed in 3,000 ticks; the other three are global/semi-global
trees. So the interaction objects add about 1,160 trees and 0.48 MB: cheap enough to preload. Each BHAV carries a 64-byte name (`bhav.cpp: kNameSize`): 18,810 x 64 B = 1.2 MB of the 5.6 MB of raw behaviours is names that only by-name lookups and logs use; the by-name
operand can be rewritten to a number by the converter and the names dropped (a name->instance table per group, or the string table's index turned into the target instance, whichever the VM needs).

## The simulation runs on the console (2026-10-26, PCSX2, N001 Lot 68 from a simbank folder)

`tools/simbank <install> <lot> <out> [ticks]` gathers everything the simulation read while the lot loaded and ticked (26,093 resources, 10.4 MiB, the neighborhood's characters included) into `out/code.package`, with the lot's and its neighborhood's
packages and a `sim.cfg` (installed-packs mask, lot path). `LotSimulation(packages, mask, lot)` loads from that instead of an install (no Lua scripts, no want definitions, no character files). `tools/simmem <bank> [ticks]` runs it; built for the PS2 as well.

| | Host (x86-64) | PCSX2 EE (software-timed) |
|---|---|---|
| lot load (index, 3,484 object types, 508 objects, 170 Sims' state, hood) | 0.12 s | **6.1 s** |
| tick, mean over 300 | 0.23 ms | **about 1 ms**, slowest 2 ms |
| heap in use after load / after 300 ticks | 25.0 / 26.0 MiB | **14.6 / 14.6 MiB** (32-bit pointers make the node-heavy tables smaller) |

So the simulation fits the console's 27 MiB heap even unchanged (14.6 MiB against the ~8-11 MiB of a baked scene and the texture cache, which leaves the pre-parsed bank for later), and the EE is only about 5x slower than the host at ticking (PCSX2 is not cycle-accurate; the VM's
interpreter loop is not stalled by memory the way a real EE's would be: expect worse on hardware). Routing was not available (no route sources), as in `simmem` on the host, so Sims that must walk stop with "routing needs the lot's route grid": the tick cost is for the rest of the work.

**What was in the way, in the order it was found:**
1. `std::filesystem::exists()` on the neighborhood package stopped the program (every later line in the log vanished) **only after the simulation had loaded its code**; alone it works. Replacing it with an `ifstream` open check
   in `LotSimulation::load` lets the lot load; the cause (the `stat` path of the toolchain's libc, perhaps its descriptors or stack) is not known.
2. **PCSX2's log is lossy after a point** (prints stopped appearing while the program ran on), and stderr never shows. What fixed the debugging: read the program's own globals from outside with PCSX2's PINE interface
   (`EnablePINE = true`; `tools/ps2test/pine_read.py`, `pine_sym.sh`, `run_pcsx2_pine.sh`), so every result of `simmem` is also kept in `g_*` globals. A `-finstrument-functions` ring of the last functions entered
   (read the same way) showed where the program was; it makes everything 10x slower, so slow runs look like hangs.
3. False leads, for the record: a 128 KiB default stack, `std::hash` of 64-bit keys (a table with them did stop growing at 257 buckets in a test, but not in the real run), exceptions, double-precision math, host file latency (0.2 ms a read), the EE recompiler (the interpreter stalls the same).

## The simulation from a pre-parsed, compressed bank, and what stopped it (2026-10-26)

**Bank.** `simbank` now writes `code.package` with only what the run read (9,162 resources instead of 26,175), the lookups by name (`NAME` resource, group 0 instance 0: `behaviorByName` answers from it instead of
parsing every behaviour of the install to learn names), the neighborhoods' identities (`nhoodid` resources: the simulation no longer opens every neighborhood's packages), and every resource QFS-compressed when
that saves space (`qfs::compress`, checked by unpacking): **5.96 MiB of resources stored in 1.27 MiB**. The console reads the bank whole into memory in one sequential read (`dbpf::Package(path, inMemory)`), so
the roughly 10,700 random small reads the simulation made from the package are gone (they took 220 s from a disc image with fast CDVD, and would be far slower on a real disc). The host fingerprints did not change
(`4904081e` after 3,000 ticks, `4dfbd967` after 300).

**What was stopping the program: kernel semaphores.** For days the console run stalled in ways that looked like the host file system failing (`open` returning -1 with no trace in PCSX2's log, reads failing after
about 10,750, a `free` that never returned). Neither the file system nor memory was at fault. The EE kernel has a few hundred semaphores; this toolchain's libstdc++ uses the mutex lock policy for `shared_ptr`
(the R5900 has no compare-and-swap), so every `make_shared` runs `pthread_mutex_init`, which takes a semaphore that is never given back (`IndexCodeSource::behavior` alone made 1,552). Once the table was full,
`CreateSema` returned -1 and every lock that was created afterwards (the C library's, stdio's) waited on a semaphore that did not exist. `tools/ps2test/pthread_light.cpp` (linked with `-Wl,--wrap=pthread_mutex_*`)
makes mutexes cost nothing, which is right for a program with one thread. **Any console program that uses `shared_ptr` or `std::mutex` needs it.**

How it was found, for next time (all in `tools/ps2test`, `-DNEOTS2_PS2_DEBUG=ON` for the helpers): a sampler of the interrupted address on every vertical blank (`pc_sampler.cpp`; an address below 0x100000
every time means every thread is blocked in the kernel), a monitor thread that asks the kernel what the main thread waits on (`ReferThreadStatus`: wait type 2 = a semaphore, `ReferSemaStatus` for its state), and a
wrapper on `CreateSema` that logs who asks and what comes back. Earlier dead ends, so they are not tried again: heap size and address, descriptor count, stack size, interrupts, idle time, optimisation level,
the atomics shim, exceptions, `sbrk` trimming, a guard-band allocator, heap consistency.

**Result.** Lot 68's bank from `host:` and from an ISO (`tools/ps2test/run_pcsx2_iso.sh`; `cdrom0:` finds only upper-case names, `disc_paths.cpp` upper-cases them where `_open` is called): load 7.7 s, 3,000 ticks in
15 s (5 ms a tick on average, but the slowest tick is 1.3 s: a route plan), heap 15.5 MiB loaded and 18.8 MiB after the ticks. The fingerprint is not the host's (`ec2ee055`), which is expected: the EE's float unit
is not IEEE (it rounds toward zero, has no NaN or infinity), so routing differs and Sims take other paths. Because they do, the bank must hold every behaviour they could run, not only the ones the host's run
used: three stops on the EE are `BHAV not found` for behaviours the host never needed. The next step for the bank is the static closure (`tools/bhavclosure`) added to what the run recorded.

## The bank covers what Sims could do, and the first full run (2026-10-26, later)

The console's floats are not IEEE, so Sims leave the path the host's run took (the world matches the host's at every fifth tick until tick 120 and parts by 125; `SIMMEM_FINGERPRINTS=1` on the host and `g_fingerprints` on the
console show it). The bank therefore holds more than one trajectory: `simbank` adds the static closure of the lot's object types plus the `Social*` types (5,566 behaviours, `src/sim/behavior_closure`, shared with
`tools/bhavclosure`), the constants of their groups, the global string tables, and asks the animation library once for every animation of the global age lists (595 names), so the support table knows their lengths
(an animation the table does not know counts as one that does not exist; that was the cause of "Animation name is empty string"). `SIMBANK_NO_CLOSURE=1` leaves the closure out.

Memory was the next limit (the heap is about 26 MiB): the first route plan builds the route world and frees the loaded lot, and that is where the heap peaks. The neighborhood package in the bank now keeps the
wants and fears only of the neighbors whose Sims are on the lot (4 of 280; they were the largest single user, 5.5 MB on the host), and is compressed: 0.83 MiB.

Lot 68 on PCSX2 from `host:`: bank 8.99 MiB of resources, 2.34 MiB stored; load 6.3 s; 3,000 ticks in 15.5 s (5 ms a tick, slowest 1.3 s); heap 18.7 MiB loaded, 21.3 MiB after; two stops left, each once
(`Animation name is empty string` for list 0x81 entries 0 and 1). The fingerprint is `757e0f08` against the host's `4904081e`, which is the float difference, not an error.
Still to do: prune the object definitions to the types the lot and `Social*` need (about 2 MB of heap), a smaller program image, and the route plan that takes 1.3 s.

## NeoTS2 itself on the console: the first combined build (2026-10-26, evening)

**One executable.** `app/main.cpp` is the entry point on every platform: built for the console (`__PS2__`) it hands over to `app::runConsoleLot` (`app/console_lot.cpp`) and the desktop code is compiled out. The
console lot mode is what the desktop's lot view does with the baked scene in place of `scene::Assets` + `SceneBuilder`: `app::LotView` and `app::Orbit` (the desktop's own camera, storey and wall-view code),
`scene::visibleInstances` + `GsRenderer::show/draw`, and `app::ConsoleLife` (`app/console_life.cpp`) in place of `LotLife`: it runs `LotSimulation` from a bank in step with real time (at most 2 ticks a call, so
one slow tick is not followed by a pile) and moves a marker per Sim. `src/platform/ps2/platform_ps2.cpp` is the console's `platform.hpp`: the pad is the keyboard (d-pad and left stick pan, L2/R2 zoom, L1/R1 turn a
quarter, triangle/cross change storey, square the wall view, circle the roofs, Start leaves) and `pollEvent`/`keyHeld` report it, so the desktop's own `orbitStep` and `LotView::key` run unchanged. Target `neots2` in
`CMakeLists.txt` (PS2 block); run `neots2.elf host:<bank> host:<scene.cscene> [stage to stop at]`; with no arguments it reads `cdrom0:\BANK` and `cdrom0:\LOT.CSC`.

**Baked scene additions** (`tools/ts2bake`, format changed, rebake old files): `Scene::floorGrid` (the floor's height at the middle of every tile of every storey, 3 x 40 x 40 floats for Lot 68; `floorHeight` is rebuilt
from it on the console) and one "sim-marker" instance per Sim of the lot (a 0.3 x 0.3 x 0.9 post, `Instance::object` = the Sim's id). Real Sims (skinned, animated) are still to do.

**Memory is the limit, and these are the numbers (EE, Lot 68, PCSX2; the heap is about 26 MiB):** baked scene resident 8.45 MiB (head read 7.70, uploaded 8.45; the file's head is 4.9 MiB, the rest is the `Scene`
structure, and `prepareBaked` is given a copy of it) + simulation 13.3 MiB after loading and 15.6 MiB after 3,000 ticks (was 18.7 and 21.3 this morning) = 21.75 MiB in use after both are loaded, and it fits, but
the renderer's caches (textures 6 MiB, lit colours 5 + 5 MiB: budgets, not use) were sized for a program with the heap to itself and the console lot mode lowers the texture one to 2 MiB. Cuts made today, host
equivalents in brackets: object definitions pruned to the types the lot, the `Social*` objects and what trees create by a GUID they hold need (925 of 3,484; `SIMBANK_KEEP_ALL_TYPES=1` keeps all) [-2.7 MiB];
`RoomMap` as one byte per quarter tile with a palette of room ids (the lot's wall graph is 214 x 214 tiles x 4 levels, and two maps stay resident) [-4.4 MiB]; route grid cells 28 bytes, not 44, with one index
for their four consecutive children [-1 MiB]; the neighborhood package without the wants and fears of neighbors who are not on the lot [-5 MiB]. **Pruning changes what the simulation can enumerate** (Set to Next
over object types), so the host's replay of a pruned bank no longer matches the recording's fingerprint; that is expected. Still to cut: `Package::entries_` after the index copy and `ResourceIndex::byKey_` as a sorted
vector (~1 MiB), the baked scene's overhead (~2.8 MiB), a smaller program image (-Os, --gc-sections: the heap is RAM minus the image, so each byte saved is a byte of heap), items in route grid leaves.
`SIMMEM_KEEP=1` on the host (`simmem` leaves without freeing) with `heaptrack_print -l 1` lists what the simulation holds live, by allocation site.

**Open: the console lot mode crashes (the kernel reboots, "Get Reboot Request From EE") about 1.5 s after the simulation has loaded, before the first frame's stage 12 marker; being bisected.**
Debugging aids: `g_consoleStage` / `g_consoleFrame` / `g_consoleError` read with `tools/ps2test/pine_sym.sh`, and the third argument parks the program at a stage so its memory can be read (a program that crashes
or returns is cleared by the reboot).

**Bank additions today:** `simbank` runs the static closure of the lot's types (`src/sim/behavior_closure`, also used by `tools/bhavclosure`), iterating on object types created by a literal GUID; adds the
global string tables; asks the animation library once for every animation of the global age lists (so replayed lengths cover them); writes the neighborhood package filtered and compressed. `qfs::compress` is the
game's format's encoder (greedy, checked by unpacking). Lot 68's bank is 18,701 resources, 9.24 MiB, 2.56 MiB stored.

## The combined program runs; `--gc-sections` was eating the unwind tables (2026-10-26, night)

**State.** `neots2.elf host:<bank> host:<scene.cscene>` (Lot 68, scene baked with `--max-side 64 --mesh-keep 0.15`) draws the lot and runs the simulation beside it, tick after tick (1,176 frames and 528 ticks
when last looked at, 25 MiB in use, no out-of-memory, no abort): the first time the real executable has lot, Sims' markers and simulation together. Image 2.74 MB at `-Os`. The 64 px textures are soft; `--max-side` is the
dial (the owner is happy to go as low as 16). What it took, on top of this morning's cuts: texture cap 256 -> 64 px (the viewer needed 13.9 MiB of textures resident for one frame at 256, 3.8 at 128, about 1 at
64: they have to be in RAM because VRAM has about 1.8 MiB free for them); route grid down to a quarter tile (`RouteGrid::finestSize`, 0.25 on the console, -2.8 MiB; the host default stays 1/8); `Package::releaseEntries` once
a ResourceIndex has copied them; the renderer's chain buffer growing by a quarter instead of doubling; lit-colour cache budgets 256 KiB (`GsRenderer::setCacheBudgets`); `-Os` and `--gc-sections` (image 4.9 -> 2.7 MB: the heap
is RAM minus the image, so every byte of image is a byte of heap).

**The trap in `--gc-sections` (found after a day of misattributing it to `-Os`/`-O2`).** The SDK's linker script (`ps2sdk/ee/startup/linkfile`) names neither `.eh_frame` nor the per-function `.gcc_except_table.*` that
`-ffunction-sections` makes, so they are orphan sections, and with `--gc-sections` the linker drops all of them. The program runs until the first exception is *thrown* (the simulation throws its first about tick 120,
when Sims start to act), then `abort()` is called from inside libgcc's unwinder (`uw_init_context_1`) and the BIOS reboots the machine ("Get Reboot Request From EE"). `build-ps2` looked fine only because it had been
reconfigured, not reconfigured *from scratch*: toolchain `*_INIT` flags apply on the first configure only, so it never had `--gc-sections`. **Fix: `cmake/ps2.ld`, a copy of the SDK's script with `KEEP(*(.eh_frame))` and the
wildcard exception tables, substituted into the linker flags in `cmake/ps2.cmake`. Any new console build directory must be configured from scratch (`rm -rf` it) after a change to the toolchain file.** How it was found:
`-DNEOTS2_PS2_DEBUG=ON` parks the program in `abort`/`_exit` with the caller's address (`g_exitKind`, `g_exitRa`), `-DNEOTS2_PS2_TRACE=ON` (with it) traces the routing code's functions (`g_traceRing`), and
`nm`/`readelf -S` showed the build that aborted had no `.eh_frame` section at all. Also learnt: overwriting `CMAKE_EXE_LINKER_FLAGS` on the cmake command line replaces the toolchain's own flags (library paths, linker script): use
`-U` to go back; `mesh_decimate.cpp` and `rig_layers.cpp` must stay at `-O3` (compiler crash / wrong code at `-Os`); only `sceSifCallRpc` exists as a symbol to wrap, not `SifCallRpc`.

**Memory now (EE, PCSX2).** Scene resident 8.4 MiB, simulation 13.1 MiB after 3,000 ticks (was 21.3 this morning), textures about 1 MiB, renderer working set a few MiB. Remaining cuts if needed: `ResourceIndex::byKey_` as a sorted
vector (~0.5 MiB), `SimObject` (3.2 KB each, 508 of them: person data, walking and other records are inline for every object, ~0.8 MiB), strings and pie-menu text only the UI needs, object definitions pruned harder.
Not done yet: the baked lighting handling day/night (owner: later), showing only the current storey (with an imposter for the others' outsides), Sims' own models, UI, the menu and neighborhood modes, audio, memory-card saves,
disc layout (everything so far is read through `host:`).
