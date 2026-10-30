# NeoTS2

A C++20 reimplementation engine for The Sims 2: it reads the game's own files (DBPF packages, scenegraph models, lots, Sim behaviours) and runs the
simulation, scene building and rendering itself. `README.md` and `CONTRIBUTING.md` describe the project; this file is for working on the **PlayStation 2 port**
(branch `feature/ps2-port`), which is the long-running effort in this checkout. The full account, with measurements, is in **`docs/ps2-port-plan.md`**: read it
before changing anything console-related; it is the memory of what was tried and what the numbers were.

## Layout

| Path | What |
|---|---|
| `src/formats`, `src/content`, `src/sim`, `src/scene`, `src/render` | Libraries: file formats, game content, the simulation, the scene builder (`scene::Assets`, `SceneBuilder`, `lot_scene`), and what every renderer shares (`FramePlan`, `RendererState`, `texture_fit`, `mesh_pack`, `cpu_light`) |
| `src/render/gl` | OpenGL renderer, with a fixed-function "GL1" mode (`--renderer gl1`) that the PS2 work grew out of |
| `src/render/ps2` | The PS2 renderer: `gs_renderer.*` (gsKit + VU1) and `vu1_soup.vsm` (the VU1 microprogram) |
| `src/render/console_scene.*`, `texture_convert.*`, `src/content/{bank,console_texture,mesh_decimate}.*` | The converter's formats and code: baked scenes, console textures, mesh decimation, asset banks |
| `app/` | The desktop game (SDL window, UI, panels); not built for the console |
| `tools/` | Headless tools (see below) and `tools/ps2test`, `tools/gs_lotview` (console test programs) |
| `cmake/ps2.cmake` | The cross-compile toolchain file |

## The conversion, in one paragraph

A PC "converter" step turns the OS X base game into console data; the console never runs the scene builder (it peaks at ~110 MiB; the console heap is **27 MiB**).
`tools/ts2bake` builds a lot's scene exactly as the game does, lights it, converts its textures (256 px cap, 4-bit / 8-bit palettes, mips) and
meshes (weld, quadric decimation, 16-bit quantisation) and writes one **baked scene file**: a head (materials, instances, lights, primitives as *triangle soup*)
followed by the images, which the console reads from disc on demand. `tools/ts2bank` is the older asset-bank experiment (resources a set of lots read, converted textures
and meshes). `src/render/ps2/gs_renderer.cpp` draws a baked scene: `FramePlan` orders the draws, lit vertex colours are cached per corner, positions/uvs/colours are
sent to the **VU1** with VIF DMA chains (interleaved unpack, no copy), the microprogram transforms, culls back faces and `XGKICK`s to the GS, textures live in a VRAM ring uploaded
through the same chain, and primitives that cross the guard band are clipped on the EE. Status and numbers: the last sections of `docs/ps2-port-plan.md`.

## Building

- **Host tools and tests** (no window needed): `cmake -S . -B build-headless -DNEOTS2_HEADLESS=ON` then `make -j12 ts2bake ts2bank lotscene lotdump dbpfdump gamecheck simrun`.
- **Console** (needs ps2dev: `PS2DEV=~/.sdk/ps2`, `PS2SDK=$PS2DEV/ps2sdk`, and `$PS2DEV/bin`, `$PS2DEV/ee/bin`, `$PS2DEV/dvp/bin` on `PATH`):
  `cmake -S . -B build-ps2 -DCMAKE_TOOLCHAIN_FILE=cmake/ps2.cmake -DNEOTS2_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Release && make -C build-ps2 -j12 gs_lotview`.
  `build-ps2-os` is the same at `-Os`. The libraries cross-compile; `app/` does not.
- Toolchain quirks (GCC 15.2 for the EE, 32-bit `size_t`/`uint32_t` = `unsigned long`): `-U__SIZEOF_INT128__` (ICE in `<format>`), no floating-point `std::format`
  (`src/platform/ps2/float_format.hpp` supplies it), `std::atomic` needs `tools/ps2test/atomics_shim.cpp`, `rig_layers.cpp` only compiles at `-O3`, mixed-type `std::min/max/clamp`
  need explicit template arguments, **`shared_ptr`/`std::mutex` need `tools/ps2test/pthread_light.cpp`** (the kernel has a few hundred semaphores and libstdc++ takes one per `shared_ptr` here; when it runs out the program
  stalls in odd places, see the plan), `extern "C"` microprogram symbols must be at global scope. ERL overlays do not work for C++ here (`docs/ps2-port-plan.md`): the design is one ELF per mode.
- **Linking:** console builds use `cmake/ps2.ld` (the SDK's script plus `KEEP` for the unwind tables) with `--gc-sections`; without it the first exception aborts. Toolchain-file flags only apply when a build directory is
  first configured: `rm -rf` and configure again after changing `cmake/ps2.cmake`. The PS2 build of the app is `neots2` (`app/main.cpp` hands over to `app/console_lot.cpp` under `__PS2__`); `-Os` is `build-ps2-os*` with `-DCMAKE_BUILD_TYPE=MinSizeRel`.
- Do not compile the repo on the user's machine without being asked (they run builds); on the server (`ssh kouta@arch-server`) compiling and testing is fine.

## Running on the (emulated) console

- Data: the OS X install is at `/srv/files/sims2-osx` (game dir `The Sims 2.localized`), saves at `/srv/files/s2save-osx`, a Legacy-collection install and PC saves at `/srv/files/`.
- Bake: `build-headless/ts2bake /srv/files/sims2-osx "<lot.package>" out.cscene --mesh-keep 0.2 --mesh-error 0.1` (it also checks the file reads back).
- Run: `tools/ps2test/run_pcsx2_gpu.sh <elf> <seconds> <out.png> [log]` runs PCSX2 on display `:0` (the user's GPU-accelerated X session; it needs a connected output,
  see `~/xorg-headless/INSTALL.txt`) and grabs only the top-left 640x480 region. `ELF_ARGS="host:lot.cscene 1.0 0"` passes arguments (lot file, camera zoom, orbit angle). `host:` is the ELF's directory.
  `run_pcsx2.sh` is the older Xvfb + llvmpipe variant. **Ask before running any X11 program**: the user restricted it once and re-allowed `:0` explicitly.
- `gs_lotview` prints heap use, per-phase times and VU1 counters. PCSX2 does not model VU pipeline stalls or card timing, so those numbers are for proportions only; nothing here is measured on a real console.
- Headless measurement tools: `lotscene` (scene build time/heap), `simrun`, `gamecheck`, `ts2index`; heaptrack is installed.

## Conventions and standing agreements with the owner

- Match the surrounding code (comment density, naming). Commit locally only when asked (author identity: pass `-c user.name=kouta -c user.email=darkfm@vera.com.uy`; do not edit git config); the server clone cannot push.
- Never install system software without asking (the owner has installed ps2dev, heaptrack, PCSX2, vulkan-swrast). Passwordless `sudo` exists on the server but is not a licence.
- Priorities for detail cuts on the console: model and texture detail first, then texture variety, then object variety and lot size (prefer single-storey lots to smaller ones).
  The owner accepted 4-bit palettes and decimation to ~42% of triangles; they want editing/building on the console itself, saves on a real memory card, DVD-only assets.
- Be honest about what was measured where: PCSX2 on a PC is not a PS2.

## Open work (see the plan for detail)

Microprogram software pipelining (needs console timing); fewer vertices drawn (distance LODs, instancing); EE per-draw cost; terrain, roads, water, sky, Sims (skinned
meshes), sprites/UI and mips on the console path; the simulation's data in the bank; the lot editor behind a loading screen; the neighborhood view; real-hardware tests
(memory-card benchmark `tools/ps2test/memcard_bench.cpp` is waiting for a console).
