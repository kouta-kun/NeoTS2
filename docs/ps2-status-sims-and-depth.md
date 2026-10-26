# PS2 port: status after the Sims and the depth-buffer fix

State of `feature/ps2-port` when the static Sims were put on screen. The long account (bank, semaphores, `.eh_frame`, memory numbers) is in
`ps2-port-plan.md`; this page is what was done since, what is known, and what is next. Everything was run on PCSX2 (not a PS2).

## What runs

- `neots2` (the real entry, `app/main.cpp`) in console lot mode: reads a bank and a baked scene (`cdrom0:\BANK`, `cdrom0:\LOT.CSC`, else `host:bank`,
  `host:lot.cscene`), runs `LotSimulation` through `ConsoleLife`, draws the **current storey only** (`keepCurrentStorey`), d-pad/shoulder input only.
- **Sims are drawn, static, in the bind pose** (no animation). `ts2bake` files each Sim's instances (hair, head, body, lit as `LightGroup::Sims`) under the
  Sim's object id and strips joints, weights and `mesh.skin`, so they are plain meshes; `ConsoleLife` moves all of a Sim's instances as it walks.
  `--markers` (red posts at the Sims' places) is opt-in.
- Sims cost 3 to 8 primitives and ~3,000 to 4,200 triangles each before decimation (`tools/simstat`); their textures at a 64 px cap are 27 KiB of VRAM for four Sims.
  The cost to watch is draw calls and per-frame disc reads, not VRAM. Sim bodies bake coarse at `--mesh-keep 0.15` (about 300 triangles); a per-mesh keep for Sims is a to-do.

## The depth-buffer bug (fixed)

Symptom: Sims (and, in close views, furniture) vanished under floors, with a hard horizontal split across the picture. Everything pointed elsewhere first
(winding, alpha, light group, occlusion by walls, VU vs EE clipping, DMA races): the Sims' draws were submitted correctly and rendered fine when drawn alone or last.

Cause: `gsKit_init_screen` forces a **16-bit Z buffer when the frame buffer is CT16** (`gsInit.c`: "only non-S 16-bit z depth works with this mode"), whatever
`PSMZ` the program asked for. The renderer wrote **24-bit** depth values (`kZScale = 16777215`), which a 16-bit buffer cannot hold. Depth compared as garbage, so
early opaque draws lost to later ones wherever the values wrapped.

Fix (`src/render/ps2/gs_renderer.cpp`):

- `kZScale = 65535` (both the VU1 table and the EE clip path use it).
- The console view uses a near plane of **0.5** (`kNearPlane`) instead of `farPlane / 10000` = 0.05. Depth is `n / distance`, so with 16 bits the near plane
  decides the resolution: at 0.5 about 80 levels per tile at 20 tiles, enough to separate a Sim from its floor.
- If a frame buffer of 32 bits is ever used (VRAM would drop to ~0.8 MiB for textures) the 24-bit scale and the old near plane could return.

Lesson recorded: a test that draws only the suspect object, then adds the rest back one pass at a time, found this; reading the VU code did not.

## Open items

- Sims: animation/skinning, per-Sim mesh detail, imposters for hidden storeys, the black patch where the pool's storey is hidden.
- `app/console_lot.cpp` still has the test arguments (stage park id, `nosim`, start storey, camera distance, target, follow Sim, wall mode) and a few debug prints.
- Day/night for baked lighting, UI, neighbourhood mode, audio, memory-card saves, disc layout.
- **Profiling** (requested): why the console path is slow. Known so far: ~5 ms average tick with a 1.3 s route-plan spike on the EE, first-frame texture reads from `host:`,
  EE-side lighting and per-draw cost. PCSX2 timings are proportions only.

## Debug recipes

- View a given place: `ELF_ARGS="host:bank host:<scene>.cscene 0 nosim <storey> <distance> <x> <y> 0 <wallmode>"`, e.g. `0 nosim 2 10 16 22 0 0` for Lot 68's Sims.
- A suspect object that does not appear: skip every other draw in `drawListVu`, then bring passes back; print the projected GS bounds and the first vertices.
