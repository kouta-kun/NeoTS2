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
