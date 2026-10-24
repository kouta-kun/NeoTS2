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
- Storage: DVD or hard drive for converted data, read on demand. USB 1.1 is too slow for anything but saves and
  config (about 1.5 MB/s theoretical, from forum posts, not benchmarked); the memory card is for saves only.

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

- The lot has about 250,000 triangles with every instance counted once. A PS2 game of this kind draws something like
  30,000 triangles a frame at 30 fps (**assumed**; depends on the renderer and on VU1 microcode). That is
  roughly 8 times fewer.
- Where the triangles are, by kind and by mesh, is not known. `lot geometry:` in the log now says; read it before
  designing level of detail.
- What reduces them: culling by room and view (the game already culls boxes), distance-based level of detail for
  objects, merging floor and wall tiles into larger faces, removing small objects far from the camera.
- The CPU is an R5900 at about 295 MHz (**spec**). The simulation (SimAntics VM, pathfinding, Lua UI) has not been
  timed on anything slow. The iMac G3 is a rough proxy (a different design, about the same clock): time a simulation
  tick and a scene build there.
- Lighting stays on the CPU or moves to VU1. ps2gl's VU1 code lights directional and point lights per vertex (**read in
  source**); this engine's model has eight lights with ranges and spots, which VU1 can do but needs microcode.

## What exists already

- A renderer path with no shaders (`--renderer gl1`): per-vertex CPU lighting, one texture per pass, 16-bit indices,
  arrays without stride, power-of-two textures, 4/8-bit palette preview, a console-subset checker (`NEOTS2_GL1_STRICT`).
  It runs on a PC; it is the reference for what a GS backend has to draw.
- `render/cpu_light`, `render/fixed_layout`, `render/texture_fit` are API-independent and testable.
- A platform layer (`src/platform`) with an SDL 1.2 backend; a ps2sdk backend would sit beside it.

## Work, in order

1. **Toolchain.** Build a C++20 hello-world with the current ps2dev toolchain: `std::format`, `std::filesystem`,
   `std::thread`, exceptions, the engine's `-std` and warnings. Unverified today; everything depends on it.
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
- Which kinds of detail may be dropped first if the sheet doesn't close (Sims' texture detail? object variety? lot size?).
- Whether lots may be limited in size or in number of objects on the PS2.
- Hard drive as a requirement, or DVD-only to start with.
