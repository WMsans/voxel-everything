# Voxel Everything — Raster Mode

**Date:** 2026-10-04
**Status:** implemented
**Start commit:** `6637bf2`
**Prior specs:** `2026-08-17-m5-lod-design.md` (the LoD tree this deepens);
`2026-10-01-transparent-voxels-design.md` (the island contouring and island-page draw this
reuses).

A settings switch, "Raymarching", that turns world ray marching off completely. With it off,
every surface — terrain near and far, and islands — is a rasterized surface-nets mesh, and
the near field is about as detailed as the transparency shell (0.1 m cells). It is a
performance mode: visual loss against the marched near field is accepted.

---

## 1. Intent

- **Who:** a player on hardware where the near-field marcher is too expensive.
- **What they get:** the same world, edits, destruction and islands, drawn without the
  compute marcher, at roughly transparency-shell quality up close.
- **Success:** the switch works live from the settings menu; raymarched mode renders
  exactly as today; raster mode has no holes while streaming, renders islands, and costs
  less frame time than raymarched mode in an interleaved A/B/A.

Goals:

- One Render-tab switch, live, no restart.
- No world ray march in raster mode: no `raymarch` dispatch, no `sun_march` in scatter.
- 0.1 m terrain near the camera, coarsening by screen-space error.
- Islands render.
- Raymarched mode is bit-identical to `main`.

Non-goals, decided explicitly:

- **Marched-quality shadows.** Raster mode shadows every pixel from the sun map, whose
  cascade 0 texel is 0.4 m.
- **Island sun-map shadows.** Islands cast none in raster mode (§5).
- **The edit visualiser and the cost view.** Both are drawn by the marcher and are absent
  in raster mode.
- **Stopping brick streaming.** Grass, leaf scatter and edits read the atlas, so it keeps
  streaming. Raster mode saves the marcher, not brick generation.
- **Screen-space passes.** SSR, SSGI, SSAO and contact shadows are unchanged.

## 2. The switch

`RenderSettings::raymarch` (default `true`), row `bool_row("raymarch", "Raymarching", ...)`
in `render_rows()`. `RenderOrchestrator` mirrors it into an atomic next to
`near_field_enabled_`, and `set_effect_enabled("raymarch", ...)` reaches it through the
existing render-switch branch, which also gives the benchmark `--effects-off=raymarch`.

`RenderOrchestrator::near_field_enabled()` returns `near_field && raymarch`. Every existing
consumer of that gate then behaves correctly in raster mode with no edit:

- `LodSystem::fade_band` reports 0 / 0, so the far field owns every distance (it read
  0 / 1e9, which left one pixel in sixteen; see deviation 2 in §8.1);
- `VoxelFrame` skips the HiZ build;
- the near transparency shell grid is off;
- `deferred.comp.glsl`'s `far_field_owns` is true for every pixel, so the sun map shades
  everything.

A new `RenderOrchestrator::raster_mode()` (`!raymarch`) gates only the additions in §3–§5.
The existing `near_field` toggle keeps its meaning (0.4 m LoD everywhere, marcher still
drawing islands); five test suites use it as a lever.

## 3. LoD levels

Levels `-1` and `-2` were added **below** level 0 instead of the renumbering this section
originally specified. `kLodMinLevel = -2`: level `-1` is 0.2 m and level `-2` is 0.1 m, both
on the existing ratio-2 ladder under the untouched `kLodBaseCell = 0.4 m`. Negative, so
every existing level number keeps the meaning it has everywhere else in the tree.

`LodTreeConfig::min_level` floors the descent:

| mode | `min_level` | finest cell |
|---|---|---|
| raymarched (default) | 0 | 0.4 m (as before this spec) |
| near field off, raymarch on | 0 | 0.4 m (as before this spec) |
| raster | `kLodMinLevel` (-2) | 0.1 m |

`LodSystem` sets it from `raster_mode()` each tick. `want_finer` and `children_ready` treat
`min_level` as the floor that level 0 used to be, and the near-dense rule only forces
descent while `level > 0` — below level 0, screen-space error alone decides.

**No other constant changed.** `kLodBaseCell`, `kLodLevels`, `kLodResidentLevelFrom`,
`sun_cascades` and `lod_reduce` keep their values and their level numbers, so the cascade
floors and the shadow cut still stop at 0.4 m and need no `kLodFarBaseLevel`. `kShellLevel`
was a `-1` sentinel that `clamp_level` turned into level 0; a shell chunk is exactly a
level-`-2` chunk, so the sentinel became `kLodMinLevel` and `MeshService`'s
`lod_chunk_origin(job.level, …)` lookup became correct.

At `kLodTargetCellPx = 3` and 1080p this selects 0.1 m to roughly 45 m and 0.2 m to roughly
90 m. `children_ready` keeps the parent drawn until all children are built, so streaming
never shows a hole.

**Transparency.** Fine-level chunks contour their shell exactly as far chunks do today
(`lod_quads` shell mode), so raster mode needs no new transparency code; the near shell
grid stays off through the §2 gate.

**Page budget.** `max_lod_pages_` is 32768. A refused request already falls back to the
coarser parent. The plan measures peak pages in raster mode and raises the budget for
raster mode if it binds.

## 4. The frame in raster mode

```
stream → sky fill → LoD (levels 0–9, islands) → grass → leaves
  → shell → ssgi → ssao → deferred → transparency → inject → contact → ssr → outlines
```

- **Skipped:** the island cull and the `raymarch` dispatch. `RaymarchPass` allocates no
  targets in raster mode.
- **Sky fill.** `CompositePass` draws a `SKY_ONLY` variant of `composite.frag.glsl`: the
  same G-buffer clear and fullscreen triangle, `rd` rebuilt from the push block it already
  receives, and the miss branch's outputs (sky in albedo, material 0, gloss 0, depth 0)
  without sampling any marcher target. The LoD raster then draws over it with depth
  testing as it does today. Deferred, outlines and inject see the same G-buffer contract.
- **Scatter sun.** `grass_scatter.comp.glsl` and `leaf_scatter.comp.glsl` get a push flag;
  with it set they write `sun = 1` instead of calling `terrain_sun_visibility`. Deferred
  then shadows those pixels from the sun map, since `far_field_owns` is true everywhere.

Known ceiling: the 0.1 m surface sits a few cm off the ≥ 0.4 m mesh that drew the sun map,
which may show as acne. The far-grove capture (§6) decides; if it shows, the fix is a
normal-offset bias on the sun-map lookup in raster mode.

## 5. Islands

Today only the marcher draws an island's opaque part.

- **Contour.** `ve::island_shell_blocks` becomes `ve::island_blocks(v, origin, voxel, mode,
  out)`. *Shell* mode is today's function. *Opaque* mode maps a transparent solid to outside
  (the opaque view at the island's voxel pitch) and calls `lod_contour(..., false)`.
  `RenderOrchestrator::contour_island_shell` produces both meshes at extraction.
- **Both modes.** The `VolumeData` only passes through `island_shell_wait_` briefly, so an
  island extracted in raymarched mode would have no mesh after a live toggle. Opaque meshes
  are therefore contoured in both modes. The plan measures the cost on the island leg; if
  it hitches the render thread, contouring moves to the worker.
- **Storage.** Local-space pages via `LodPool::upload_at`, with an island flag distinct from
  the shell's so the shell passes skip them, tracked in `LodSystem::island_mesh_pages_` and
  released through the same slot-liveness walk as `island_shell_pages_`.
- **Draw.** The island transform branch of `shell.vert.glsl` moves into
  `shaders/island_xform.glslh`; `shell.vert.glsl` and `lod.vert.glsl` both include it.
  `LodRasterPass` binds `island_desc` and, in raster mode only, draws a second list of the
  live islands' opaque pages.
- **Merge-back:** unchanged. The island becomes a volume op that dirties its terrain chunks,
  which rebuild at 0.1 m.
- **Shadows:** islands cast none in raster mode; `lod_shadow.vert.glsl` draws the tree cut
  only. Adding them is the same include plus one draw.

## 6. Testing

**Characterization first**, before `kLodBaseCell` or `kLodLevels` changes:

- `test_lod_tree.cpp`: record the walk's draw and request sets for fixed cameras (flat
  ground, a ridge, looking straight down). After the change, raymarched mode must produce
  the identical sets, unshifted — the branch adds negative levels rather than renumbering
  (§8.1 deviation 1), so there is no shift to apply.
- ~~`test_sun_cascades.cpp`: pin radii, texel sizes and `min_level`; after the change,
  `min_level` shifts by +2 and the rest is identical.~~ Not written, and not needed:
  `kLodBaseCell`, `kLodLevels`, `kLodResidentLevelFrom`, `sun_cascades` and `lod_reduce` are
  unchanged by the negative-level scheme, so there is nothing to re-pin. Neither this file
  nor `test_lod_reduce.cpp` is touched by the branch.
- ~~`test_lod_reduce.cpp`: pin the encode and outside bytes at 0.4 m and 0.8 m.~~ Not
  written, for the same reason.
- gdUnit goldens `test_lod_raster_golden`, `test_frame_shipped_golden`, `test_lod_seam`
  and `test_sun_shadow` stay byte-identical in raymarched mode. The baseline failure set
  drifts: stash and re-run on clean `main` before blaming the change.

**Native:**

- Raster mode descends to level `kLodMinLevel` (-2, 0.1 m cells) near the camera; raymarched
  mode never goes below level 0.
- `want_finer` stops forcing density at `kLodFarBaseLevel`.
- `island_blocks` opaque mode drops transparent solids; shell mode is byte-identical to
  today's `island_shell_blocks`.

**gdUnit**, through the shipping capture path rather than `hooks.cpp`, streamed at
(20,60,30):

1. Frame contract: with raymarch off the raymarch stage does no work and allocates no
   targets; the G-buffer holds sky where nothing is drawn and ground under the camera.
2. Level -2 (0.1 m) chunks are ready and drawn within a bounded number of frames.
3. An island severed in raster mode covers pixels with its material.
4. Live toggle raymarched → raster → raymarched leaks no LoD pages.
5. `test_settings_menu.gd`: the "Raymarching" row exists and round-trips.

**Measurement.** GPU timings are invalid on this machine, so cost is frame time from
interleaved A/B/A runs of `--benchmark` and `--benchmark-move` with
`--effects-off=raymarch` as B, plus peak LoD pages in raster mode and island-leg contour
cost (recorded in §8.5). One capture checks for sun-map acne on the 0.1 m surface — at the benchmark camera, not
the far grove, which has no screenshot flag (deviation 7 in §8.1). Result in §8.4: none.

## 7. Failure handling

- LoD pool exhausted: the walk refuses and draws the coarser parent (existing).
- Island page upload refused: that block is not drawn (existing), noted with a
  `ponytail:` comment as the shell's is.
- Sky-fill pipeline missing: the frame aborts as a failed composite does today.

---

## 8. Deviations and measurements

Recorded after implementation (branch `feat/raster-mode`, start commit `c59f382`).

### 8.1 Deviations from this spec

Decided while planning, before any code was written. Reproduced verbatim from the plan:

1. **Negative levels instead of renumbering.** The spec (§3) renumbered the LoD so that level 0 became 0.1 m, and restated about 20 sites against `kLodFarBaseLevel = 2`. Planning found raw level numbers in many more places: native tests, gdUnit tests (`debug_lod_diff(0, …)`, `debug_lod_submit([[0, …]])`, `got[0]["level"] == 0`) and hooks. Levels `-1` and `-2` give the same tree with none of that churn:
   - `kLodBaseCell`, `kLodLevels`, `kLodResidentLevelFrom`, `sun_cascades` and `lod_reduce` are unchanged.
   - `LodTreeConfig::min_level` is `0` in raymarched mode and `kLodMinLevel = -2` in raster mode.
   - The near-dense rule forces descent only above level 0.
2. **The "near field off" fade band was broken, and raster mode inherits it.**
   - `LodSystem::fade_band` reported `0 / 1e9` there, intending "the far field owns everything". But `lod.frag.glsl` keeps a fragment where `bayer4(px) < (d − start) / (end − start)`, which is about `d / 1e9`, so only the `bayer4 == 0` pixel of each 4×4 tile survived: 1/16 coverage. `deferred.comp.glsl`'s `far_field_owns` has the same shape.
   - Fix: report `0 / 0`, so `t = 1` everywhere.
   - `fade_end` also capped the near grass reach, so grass would vanish. With the near field off, grass is capped by residency alone.
3. **`kShellLevel` becomes `kLodMinLevel`.** It was `-1`, a sentinel that `clamp_level` turned into level 0. `MeshService` (`mesh_service.cpp`, `lod_chunk_origin(job.level, …)`) uses it to find a shell job's override table, so it looked in the region of a 12.8 m chunk instead of the 3.2 m shell chunk. A shell chunk is exactly a level `-2` chunk, so the sentinel becomes that level, and the lookup becomes correct. This is the branch's one substantive raymarched-mode behaviour change and it is not covered by any golden: what it does to the default mode, and what therefore is and is not pinned, is recorded in §8.7.
4. **Scatter sun flags use spare lanes:** `GrassParams::limits[2]` and `LeafParams::limits[3]`. No GPU layout change and no regenerated `blocks.glslh`.
5. **The island fixture contours through the shipping function.** `debug_place_test_island` (`hooks_physics.cpp`) duplicated the shell contour call. It now calls `RenderOrchestrator::contour_island_meshes`, the same function `drain_island_uploads` calls, so the gdUnit island tests exercise the shipping path.
6. **LoD-level assertions tick at 2560×1440.** A 64×64 `debug_render_frame` selects the coarse cut its own viewport asks for, so tests that check which levels are drawn settle through `debug_lod_tick`. Pixel assertions still go through `debug_render_frame`.
7. **The acne capture uses the benchmark camera** (`--screenshot=`) instead of the far grove. Only that camera has a screenshot flag, and acne is a terrain artifact, not a tree one.
8. **Marcher targets are not freed on a live toggle.** Raster mode never dispatches the marcher; targets allocated earlier in the session stay until the next resize. The frame-contract test checks that the stage did not run.

### 8.2 Cost, measured as interleaved A/B/A frame time

GPU pass timings are invalid on this machine, so cost is frame time only. A = raymarched
(default), B = `--effects-off=raymarch`. 2560×1440, `--disable-vsync`, 300 frames per run,
one Godot process at a time, runs in the order A1 B1 A2 B2 A3.

**Steady (`--benchmark`), ms**

| run | p50 | p99 | `pages_used` p50/p99 | pool `pages_high_water` |
|---|---|---|---|---|
| A1 | 20.83 | 22.22 | 2286 / 2286 | 2286 |
| B1 | 18.52 | 19.25 | 6005 / 6005 | 6005 |
| A2 | 21.13 | 23.92 | 2286 / 2286 | 2286 |
| B2 | 18.52 | 18.75 | 6005 / 6005 | 6005 |
| A3 | 20.83 | 22.29 | 2286 / 2286 | 2286 |

A/A spread: p50 20.83–21.13 (**0.30 ms**, 1.4%), p99 22.22–23.92 (**1.70 ms**).
B sits 2.41 ms (−11.5%) below A at p50 with an A/A spread of 0.30, and 3.81 ms (−16.7%)
below at p99 with an A/A spread of 1.70. Both separations are several times the noise
floor, so the steady difference is a measured one: **raster mode is about 11% cheaper.**

**Camera move (`--benchmark-move`), ms**

| run | p50 | p99 | `pages_used` p50/p99 | pool `pages_high_water` |
|---|---|---|---|---|
| A1 | 14.37 | 44.51 | 2247 / 2963 | 2968 |
| B1 | 11.82 | 36.56 | 2001 / 3260 | 3476 |
| A2 | 13.89 | 43.44 | 2312 / 3003 | 3008 |
| B2 | 12.75 | 43.31 | 2043 / 3283 | 3502 |
| A3 | 15.21 | 50.82 | 2339 / 3006 | 3011 |

A/A spread: p50 13.89–15.21 (**1.32 ms**), p99 43.44–50.82 (**7.38 ms**). The B p50s
(11.82, 12.75) average 2.20 ms below A, but B2 is only 1.14 ms below the *lowest* A leg,
which is inside the 1.32 ms A/A spread, and B2's p99 (43.31) sits inside the A p99 range
entirely. **The moving-camera difference is not a measured separation**: it points the same
way as the steady one and is probably real, but this run cannot distinguish it from noise.

### 8.3 Page use

Peak `pages_used` in raster mode is **6005 of the 32768 budget (18.3%)**, reached on the
steady camera in both B legs (`pool pages_high_water=6005`, `budget_bound=none`); the
moving camera peaks at 3502. The budget does not bind and was not raised. Raymarched mode
peaks at 2286 steady and 3011 moving. Raster mode costs roughly 2.6× the pages — that is
the price of 0.1 m chunks near the camera, and it is well inside the pool.

Two caveats on this number. In raster mode the LoD walk runs with `requests_pending`
pinned at 32, which equals `max_requests_per_walk`: that is request **truncation**, not
build starvation, and a refused request already falls back to the coarser parent, so the
figure is an upper bound on what the walk wanted rather than what it lacked. And the
benchmark's own settle reports `frames_to_quiet=1500 capped=true` in **both** modes, so
neither leg reaches quiescence before the 300 measured frames — the comparison is
settled-vs-settled, but a longer settle would move both legs, most likely B's more.

### 8.4 Acne capture

Two captures at the benchmark camera, `--screenshot=`, 1920×1018:
`raymarch.png` (A) and `raster.png` (B). **No shadow acne.**

The rock face fills most of both frames and is lit. Both show the same pre-existing dark
crackle lattice over the stone, in both modes — it is the material/SSAO detail that ships
today, not a raster-mode artifact. A 4×-amplified difference image is black over that
entire face: raster and raymarched mode agree pixel-for-pixel there apart from that shared
texture. There are no stripes, no shadow speckle and no moiré on the sunlit sand strip or
on the stone anywhere near the camera.

What *does* differ, in the diff image, is confined to two places:

- the grass blades, as scattered single-pixel differences over the field — the blade grid
  follows residency, which raster mode changes;
- one cluster of small dark fragments sitting just above the sand line at roughly
  (1030, 575). `raster.png` shows it and `raymarch.png` does not. It has hard-edged,
  individually shaded lumps rather than any periodic pattern, so it is not acne. It was
  first recorded as "consistent with a benchmark-spawned island's opaque mesh"; **that
  hypothesis is wrong.** Both captures ran `--benchmark`, and `demo/benchmark.gd` calls
  `_island_cycle` only under `--benchmark-island` — an island is spawned there, never in the
  steady leg. Islands are otherwise spawned only by an edit (`IslandManager::note_edit` →
  a connectivity pass), `demo/main.tscn` ships none, and the capture placed no ice sphere
  either (`--ice=` defaults to 0.0, so `_place_ice` never ran). So `live_islands` was 0 in
  both processes and the cluster is not island geometry. What it actually is remains
  **unresolved**; the raster-only surfaces it could be are the 0.1 m LoD cut near the
  camera and the near-field material change the grass residency follows. Settling it takes
  one re-capture with `hooks().debug_island_stats()` printed at screenshot time (to confirm
  `live_islands == 0` in the frame) plus the same capture with the islands effect forced
  off: if the cluster survives with islands disabled, it is terrain.

So the §4 known ceiling did not materialise at this camera. The proposed fix — a
normal-offset bias on the sun-map lookup in raster mode — is therefore **not** applied; it
stays a contingency, to be used only if acne is seen at some other camera or sun angle.

### 8.5 Island-leg contour cost

§5 requires the opaque island mesh to be contoured in **both** modes, so a live toggle finds
a mesh that already exists, and says "the plan measures the cost on the island leg; if it
hitches the render thread, contouring moves to the worker". §6 lists it in the measurement
set. Here is the number.

`RenderOrchestrator::contour_island_meshes` runs `island_blocks(..., kShell, ...)` when
transparency is on and `island_blocks(..., kOpaque, ...)` **always**, on the render thread,
from `drain_island_uploads`. The debug fixture calls the same function, so the shipping leg
can be timed from `debug_place_test_island` with no instrumentation: a throwaway script
(audited here, then deleted) placed a test island 30 times per leg, 3 interleaved rounds,
one Godot process, on a world settled exactly as `tests/test_raster_mode.gd`'s island
fixture settles it. Three legs, timed on `debug_place_test_island` only:

- **raster** — `raymarch` off, transparency on: shell + opaque.
- **raymarched** — `raymarch` on, transparency on: shell + opaque, and the mesh is never
  drawn.
- **opaque only** — `raymarch` on, `transparency.enabled = 0`, which skips the `kShell`
  call and leaves the `kOpaque` one: the marginal cost of the shell contour, and by
  symmetry of the code path (both halves run the same per-32-cell-block `lod_opaque_lattice`
  + `lod_contour`) the same order of magnitude for the opaque half.

Two bodies: the fixture's 2x2x2-cell rock (**1** contour block) and a 7x7x7-cell body
(**8** blocks) — 5.6 m, which is as large as a component gets at `kIslandDim = 64`'s coarse
pitch (59 usable samples x 0.1 m = 5.9 m, so 2 blocks per axis).

**Island leg, ms per placement, 30 placements per cell (1 warmup placement discarded)**

| body | leg | mean | median | min | max | sd |
|---|---|---|---|---|---|---|
| 2x2x2 (1 block) | raster | 189.24 | 189.18 | 187.82 | 191.23 | 0.69 |
| 2x2x2 (1 block) | raymarched | 189.13 | 189.11 | 187.05 | 190.55 | 0.55 |
| 2x2x2 (1 block) | opaque only | 188.83 | 188.84 | 188.26 | 189.37 | 0.29 |
| 7x7x7 (8 blocks) | raster | 187.29 | 187.23 | 186.29 | 189.88 | 0.65 |
| 7x7x7 (8 blocks) | raymarched | 188.39 | 187.50 | 186.05 | 211.26 | 4.33 |
| 7x7x7 (8 blocks) | opaque only | 186.83 | 186.75 | 186.08 | 187.63 | 0.44 |

Reading it:

- **The switch does not move the island leg**: raster minus raymarched is +0.11 ms on the
  fixture and +0.27 ms of median on the 8-block body, against per-sample sd of 0.3-0.7 ms.
  That is the design (§5, both modes), not a coincidence.
- **The contour's own margin is under a millisecond.** Shell+opaque minus opaque-only is
  +0.29 ms (fixture) and +0.56 ms of median (8 blocks). The opaque half is the same code on
  the same block count, so it is the same order: **at most ~0.5 ms, under 0.3% of the leg**.
- **Block count does not show up at all.** 8 blocks cost no more than 1 (the 8-block body is
  if anything 2 ms *faster* than the fixture, inside the spread), which puts the whole
  `lod_opaque_lattice` + `lod_contour` per-block cost below this measurement's floor.
- Placement is ~187-189 ms either way, dominated by `extract_component`, the mip upload and
  the fixture's own `device->sync()`.

**Verdict: no render-thread hitch, so §5's contingency (moving the contour to the worker)
is not warranted at the current island size.** In raymarched mode the whole sub-millisecond
figure is waste — a mesh that mode will never draw — and it is 0.3% of a leg that already
costs 189 ms, on the render thread, once per island extraction. The ceiling that bounds
this: `kIslandDim = 64` at `kIslandVoxelCoarse` caps a component at 5.9 m, so an island
cannot exceed 8 contour blocks. If that cap is raised, or the per-block lattice cost grows,
re-measure before relying on this number.

### 8.6 Regression run

`./build.sh --test` → native **764/764, `Status: SUCCESS!`**. `./gdunit_tests.sh -c` →
99 suites, 570 cases, 28m27s, exit 100.

**The two totals are not comparable, and `548 → 570` is not "22 new cases".** The branch
adds **11** cases: 10 in `test_raster_mode.gd` and 1 in `test_settings_menu.gd`.
`git grep -c '^func test_' -- 'tests/*.gd'` gives **559** at `6637bf2` and **570** at
`63b0826` — the tree's own count, which is what the 570 above is (a `-c` run). Task 0's 548
came from a **fail-fast** run, in which a suite stops at its first failure, so it never
executed 11 cases the tree declares: 22 = 11 new + 11 never run. Four of the 11 are the
`test_sun_cascades_gpu.gd` cases after the first (explained below); the other seven are cases
after the first failure in the other four baseline-failing suites, and which ones is not in
the record. The honest comparison is 559 → 570 against the tree, and 548 is a
fail-fast floor, not a count.

Failures, against the baseline recorded on clean `main`:

| suite / case | baseline | now |
|---|---|---|
| `test_voxel_settings.gd::test_an_ambient_change_reaches_the_object_global` | error | error |
| `test_ssao_golden.gd::test_ssao_statistics_match_the_recorded_golden` | 2 `lit_luma` failures | same 2; golden values unmoved (0.315139, 0.354178), got 0.304359 / 0.342565 vs baseline 0.304380 / 0.342561 |
| `test_lod_seam.gd::test_the_band_is_covered_exactly_once` | failure | failure |
| `test_edit_pipeline.gd::test_paint_recolours_grass_to_rock_without_moving_the_surface` | failure | failure |
| `test_sun_cascades_gpu.gd` — 5 of 7 cases | 1 case (first only) | 5 cases, one cause |

The `test_sun_cascades_gpu.gd` widening is a baseline-recording artifact, not a regression.
Task 0's baseline was taken **with gdUnit's fail-fast on**, so it recorded only each
suite's *first* failure: the log shows that suite aborting after 3 of 7 cases with the same
`ERROR: strict settle timeout` that the baseline attributes to
`test_sub_texel_motion_rebuilds_no_cascade`, at essentially the same pool state
(`pages_used` 19680 then, 19702 now, `chunks_resident` 5918 then, 5926 now). This run used
`-c`, so the four cases after that one ran too, and every one of them fails on the same
`settle()` call: at `stream_radius_m = 4000` the walk never reaches `requests_pending == 0`
within its 2500-tick budget, in any mode, and the two cases that never call `settle()`
pass. No golden moved and no new case outside that suite failed.

### 8.7 What the raymarched-mode guarantee covers

§1's promise is that raymarched mode is bit-identical to `main`. After this branch that
holds for some of the frame and is **unverified** for one thing.

**Pinned** — by a fail-then-pin written before the change and unchanged across it:

- the terrain LoD cut in raymarched mode: the `test_lod_tree.cpp` characterization
  (four fixed cameras, hashes recorded on a pre-change commit);
- the four gdUnit goldens: `test_lod_raster_golden`, `test_frame_shipped_golden`,
  `test_lod_seam`, `test_sun_shadow`, byte-identical, never re-recorded;
- HiZ and the composite, by inspection of every consumer of the changed inputs (the review
  in this branch's ledger enumerates them).

**Not pinned: the near transparency shell's override-table region lookup.** `kShellLevel`
became `kLodMinLevel` (§8.1 deviation 3) and `MeshService` resolves a shell job's override
table by the region containing `lod_chunk_origin(job.level, job.coord, …)`
(`mesh_service.cpp`, `region_of_point`). The arithmetic, with the numbers:

- regions are `kRegionBricks * kBrickSize = 32 * 0.8 = 25.6 m` (`world/region.h`);
- **before** (`main`): `kShellLevel = -1` clamped to level 0 by a `clamp_level` whose floor
  was 0, so `lod_chunk_size = 0.4 * 32 = 12.8 m`, origin `c * 12.8`, brick `floor(c * 16)`,
  region **`floor(c / 2)`**;
- **after**: level `kLodMinLevel = -2`, `lod_chunk_size = 0.1 * 32 = 3.2 m`, origin `c * 3.2`,
  brick `floor(c * 4)`, region **`floor(c / 8)`**.

Those agree only for `c` in `{0}` (and a handful of small multiples), so for essentially
every shell chunk the near shell now consults a **different region's** painted transparent
override table than it did on `main`. That is the point of the fix — the old lookup was
wrong (§8.1 deviation 3) — but nothing verifies it:

- there is no transparency golden (`tests/golden/` holds only `brick_baseline.txt`,
  `default_pipeline_field.txt`, `field_baseline.txt`, `leaf.png`);
- `test_transparency.gd` passes on both trees, so it does not discriminate this either.

**So: the near-shell override lookup is unverified for the default mode, and the
"bit-identical" claim covers terrain LoD, the goldens, HiZ and the composite — not this.**
Closing it needs one new assertion (a painted transparent override inside a near shell
chunk reaches that chunk's mesh, checked to fail on `6637bf2`) or one transparency golden.
Neither was in scope here. The other default-mode change this branch makes is the island
contour of §8.5, which costs time and changes no pixel.
