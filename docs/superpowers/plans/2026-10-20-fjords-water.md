# Fjords Water Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A calm sea at the fjords' water level `W` that fills every valley with the existing water material, from the shore to the horizon, without moving the ground.

**Architecture:** A last field stage, `fjord_water`, copies the ground into two reserved channels (`ground_sdf`, `ground_material`). It then folds the half-space below `W` into `sdf`/`material` as water, and publishes `W` in a `liquid_level` channel. The codegen sees the reserved channels and emits `eval_base_field_views` plus `#define VE_FIELD_GROUND`. `field_ops.glslh` then seeds the opaque and solid accumulators, and the base gradient, from the ground instead of the union. The C++ `Generator` gains `sample_ground` and `liquid_level` to mirror this. The far field already meshes transparent material from the union, so it needs nothing. The near shell gains one terrain candidate box, the plane `y = W` across the candidate radius, and recomputes it when the sector cache changes.

**Tech Stack:** C++20 GDExtension (godot-cpp, Godot 4.7), GLSL 460, doctest (native), gdUnit4 (GPU), GDScript.

**Spec:** `docs/superpowers/specs/2026-10-20-fjords-water-design.md`. Read it first, together with `2026-10-06-water-voxels-design.md` §3 (the three views) and `2026-10-01-transparent-voxels-design.md` §5 (the shells).

## Global Constraints

- No `Co-Authored-By` or other AI attribution lines in any commit message. Use plain conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `perf:`).
- Work only in `.worktrees/fjords-water` on branch `feat/fjords-water`, created in Task 0 from `main` at `85a186a` or later. Never commit to `main`.
- Default, Mesas, Flat, Golden and Trees generate **byte-identical** `field.glslh`. `shaders/generated/field.glslh.golden` must not change. No file under `tests/golden/` may change. If one does, stop and report; do not re-record.
- Everything new in `field_ops.glslh` is behind `#ifdef VE_FIELD_GROUND`, and the `#else` branch is the current code verbatim. A pipeline without the reserved channels compiles exactly what it compiles today.
- The water level is `fjord_height.water_y`, read through `//!use`. There is no new knob.
- `fjords.pipeline` keeps `lipschitz 8.6`. `fjord_water` declares `//!lipschitz mul 1.0`, so the computed bound stays 8.54.
- `BeautySettings`, `deferred.comp.glsl`, `shade.glslh`, `ssr.comp.glsl` and `outline.comp.glsl` are not edited.
- Union readers that stay on the union: `brick_consolidate.comp.glsl`, `lod_field.comp.glsl`, `field_probe.comp.glsl`, `island_extract.comp.glsl:53`, `raymarch.comp.glsl:151`, and the CPU edit raycast. Accepted limit (spec §2 non-goals): override bricks store union bytes.
- Tabs for indentation in C++, GLSL and GDScript.
- Build: `./build.sh`. Native tests: `cd extension && scons -Q test` (single case: `cd extension && ./build/tests/ve_tests -tc="<name>"`). GPU tests: `./build.sh && ./gdunit_tests.sh -a res://tests/<file>.gd`.
- "RegionPass: uniform set creation failed" means a stale dylib: run `./build.sh` again.
- gdUnit has a standing, drifting set of failures on clean `main`. Task 0 records it, and every "no new failures" check compares against that list.
  - If a suite not on the list fails: `git stash -u`, re-run that suite on the clean tree, then decide.
  - A failing case aborts the rest of its suite, so a drop in a suite's case count is a failure.
- Every new `.gd` file gets its `.gd.uid` sidecar, created by `./build.sh --verify`. Never invent a uid.
- GPU timings are invalid on this machine. Frame cost is measured only as wall-clock percentiles from interleaved A/B/A runs.

## Deviations From The Spec (decided while planning; Task 6 records them in the spec)

1. **Liquid is expressed as reserved channels**, `ground_sdf : float`, `ground_material : uint` and `liquid_level : float`, written by the last stage. There is no new manifest directive. The codegen keys off `ground_sdf`.
2. **The terrain candidate source is one unfiltered box**, the plane `y = W` across the candidate square. Chunks over ground above `W` build zero quads and are recorded empty. There is no per-chunk ground test (spec §6 item 3).
3. **The brick solid view under `VE_FIELD_GROUND` always pays the second evaluation** (`eval_field_solid`). It does not take the `s_liquid == 0` label-rule shortcut. This costs streaming time in Fjords only.
4. **The brick cache stays off where the union holds water** (`brick_gen`'s `mat_transparent(mat)` test is unchanged). Underwater ground pixels therefore take the per-pixel gradient. The sea view's cost is reported in Task 6.

## Review Focus

1. **Ground normals under water.** A pixel on the sea floor must shade with the ground's normal, never `(0, 1, 0)` from the water plane. Test: Task 3 `test_underwater_gradient_is_the_grounds`.
2. **The shell when a sector lands after the camera stops.** The candidates must appear without the camera moving. Test: Task 4 `test_candidates_appear_when_sectors_land`.
3. **A camera exactly at `W` on a chunk boundary** (51.2 = 16 × 3.2). Both chunk layers are candidates, and the surface is meshed once. Test: Task 4 `test_plane_box_covers_both_layers` (native) and the Task 5 shore capture.
4. **Grass at the shoreline.** Blades stand on the ground, never on the sea surface. Test: Task 3 `test_grass_finds_the_ground_not_the_sea`.
5. **Non-Fjords worlds.** Byte-identical source and unchanged goldens. Test: Task 1 (codegen golden) and every task's full gdUnit run.

---

## File Structure

**Create:**
- `shaders/stages/fjord_water.field.glslh`: the water stage.
- `tests/fixtures/fjords_no_water.pipeline`: `fjords.pipeline` without the last stage.
- `tests/test_fjords_water.gd`: GPU tests.
- `extension/tests/test_fjords_water.cpp`: native tests.

**Modify:**
- `extension/src/terrain/field_codegen.cpp`: emit the views function.
- `extension/src/generator/generator.h`, `generator.cpp`: `sample_ground`, `liquid_level`.
- `extension/src/terrain/pipeline_field_generator.{h,cpp}`: override both, and differentiate the ground.
- `extension/src/terrain/builtin_stages.cpp`: `ve::stage_fjord_water` mirror.
- `assets/pipelines/fjords.pipeline`: add the stage.
- `shaders/field_ops.glslh`: ground-seeded views and gradient.
- `shaders/brick_gen.comp.glsl`, `shaders/brick_mark.comp.glsl`: the solid shortcut.
- `shaders/grass_bricks.comp.glsl`: ground search.
- `extension/src/world/brick_eval.cpp`: CPU mirrors of the views.
- `extension/src/lod/lod_system.{h,cpp}`: the terrain candidate box and the sector-version refresh.
- `extension/src/transparency/shell_grid.{h,cpp}`: `liquid_plane_box`.
- `extension/tests/test_conifers_pipeline.cpp`: stage count.
- `tools/fjord_capture.gd`: water poses.

---

### Task 0: Worktree and baselines

- [ ] **Step 1:** `git worktree add .worktrees/fjords-water -b feat/fjords-water main`, then `./build.sh` inside it.
- [ ] **Step 2:** Run `./gdunit_tests.sh -a res://tests 2>&1 | tee "$TMPDIR/water-baseline.txt"` and `cd extension && scons -Q test 2>&1 | tail -5`. Record the failing gdUnit suites and their case counts at the top of `$TMPDIR/water-baseline.txt`.
- [ ] **Step 3:** Record the generated `field.glslh` for Mesas and Flat as baselines in `$TMPDIR/` (load each pipeline through `ve::load_pipeline` + `generate_field_glslh`, the way `test_field_codegen_golden.cpp` does for Default). Task 1 compares against them. Nothing is committed.

### Task 1: Ground views in codegen and the C++ Generator

**Files:**
- Modify: `extension/src/terrain/field_codegen.cpp`, `extension/src/generator/generator.h`, `extension/src/generator/generator.cpp`, `extension/src/terrain/pipeline_field_generator.{h,cpp}`
- Test: `extension/tests/test_fjords_water.cpp`

**Interfaces:**
- Produces (GLSL, generated only when the pipeline declares channel `ground_sdf`):
  `#define VE_FIELD_GROUND 1` and
  `void eval_base_field_views(vec3 p, out float sdf, out uint mat, out float gsdf, out uint gmat);`
  It runs the same stage calls as `eval_base_field` and returns `ctx.sdf, ctx.material, ctx.ground_sdf, ctx.ground_material`. Emit it immediately after `eval_base_field`, before the prelude.
- Produces (C++), in `ve::Generator`:
  `virtual Sample sample_ground(float x, float y, float z) const { return sample(x, y, z); }`
  `virtual bool liquid_level(float x, float z, float *y) const { return false; }`
- `PipelineFieldGenerator` overrides both when `channel_slot("ground_sdf") >= 0`. `sample_ground` reads `ground_sdf`/`ground_material`. `liquid_level` samples at `(x, kSurfaceY, z)` and returns the `liquid_level` channel. `sample_gradient` differentiates `sample_ground` (same `e = 0.01`), and its `sdf`/`material` are the ground's.

- [ ] **Step 1: Write the failing tests** in `test_fjords_water.cpp`, using an inline test pipeline with two stages: a flat ground at `y = 40` that writes `sdf`, `material`, and a water stage (Task 2 not yet written, so inline its body in the test's stage reader) that writes the three reserved channels with level `50`.
  - `TEST_CASE("a pipeline without ground_sdf generates no views")`: the Default pipeline's source does not contain `VE_FIELD_GROUND`, and equals `shaders/generated/field.glslh.golden`.
  - `TEST_CASE("a pipeline with ground_sdf emits eval_base_field_views")`: the source contains `#define VE_FIELD_GROUND 1` and `void eval_base_field_views(`.
  - `TEST_CASE("sample_ground and liquid_level")`: at `(0, 45, 0)`, `sample().material == water` and `sample_ground().sdf == doctest::Approx(5.0f)`. `liquid_level(0, 0, &y)` is true with `y == 50`. `sample_gradient(0, 45, 0).gradient` is approximately `(0, 1, 0)`, and `.sdf` is approximately 5. Approximately `(0, 1, 0)` is also the plane's gradient, so add a second case on a sloped ground `y = 40 + 0.5x` and check the gradient ≈ `(−0.5, 1, 0)`.
- [ ] **Step 2:** `cd extension && scons -Q test 2>&1 | tail -5`. Expected: the new cases fail to compile or fail.
- [ ] **Step 3:** Implement the interfaces above.
- [ ] **Step 4:** `cd extension && scons -Q test 2>&1 | tail -5`. Expected: all pass. `git diff --stat shaders/generated` is empty, and regenerating Mesas and Flat the way Task 0 Step 3 did gives output that `cmp`-matches the baselines.
- [ ] **Step 5:** Commit `feat: ground views for pipelines that declare a liquid`.

### Task 2: The `fjord_water` stage

**Files:**
- Create: `shaders/stages/fjord_water.field.glslh`, `tests/fixtures/fjords_no_water.pipeline`
- Modify: `extension/src/terrain/builtin_stages.cpp`, `assets/pipelines/fjords.pipeline`, `extension/tests/test_conifers_pipeline.cpp`
- Test: `extension/tests/test_fjords_water.cpp`

**Interfaces:**
- Consumes: Task 1's reserved channels.
- Manifest: `//!stage fjord_water`, `//!kind field`, `//!in sdf : float`, `//!in material : uint`, `//!out sdf`, `//!out material`, `//!out ground_sdf : float`, `//!out ground_material : uint`, `//!out liquid_level : float`, `//!use fjord_height.water_y`, `//!lipschitz mul 1.0`, `//!cpu ve::stage_fjord_water`.
- Body: `ground_sdf = sdf; ground_material = material; liquid_level = W; float w = p.y − W; if (w < sdf) { sdf = w; if (ground_sdf > 0.0) material = MAT_WATER; }`. So `material` is water exactly where the ground is air and `p.y < W`. Use the generated material constant for the `water` row and `ve::material_id("water")` in the mirror.
- The stage is appended as the **last** line of `fjords.pipeline`, after `conifers`. `fjords_no_water.pipeline` is today's `fjords.pipeline` verbatim.

- [ ] **Step 1: Write the failing tests:**
  - `TEST_CASE("fjords resolves with fjord_water last at an unchanged bound")`: 5 stages, `stages[4].name == "fjord_water"`, bound ≈ 8.54.
  - `TEST_CASE("fjord water fills below W over low ground")`: Load `fjords.pipeline` with no sector cache, so the ground is the `W − 64` fallback. Check: `sample(x, W − 1, z).material == water`, `sample(x, W + 1, z).material == 0`, `sample(x, W − 70, z).material != water`, and `liquid_level` returns `W`, for a few XZ.
  - `TEST_CASE("the ground view equals the no-water pipeline")`: For a grid of points from `y = W − 80` to `W + 20`, `fjords.sample_ground(p)` is bit-equal to `fjords_no_water.sample(p)`. So is `sample_gradient`.
  - Update `test_conifers_pipeline.cpp` to 5 stages; `stages[3]` stays `conifers`.
- [ ] **Step 2:** Run the native tests. Expected: FAIL.
- [ ] **Step 3:** Write the stage, its mirror and the fixture, and add the stage to the pipeline.
- [ ] **Step 4:** Native tests pass. Then `./build.sh && ./gdunit_tests.sh -a res://tests/test_field_diff.gd -a res://tests/test_fjords.gd`: no failures beyond the baseline. That shows CPU and GPU agree on the union with water present.
- [ ] **Step 5:** Commit `feat: fjord_water stage fills the valleys to W`.

### Task 3: Ground-seeded views on the GPU and CPU

**Files:**
- Modify: `shaders/field_ops.glslh`, `shaders/brick_gen.comp.glsl`, `shaders/brick_mark.comp.glsl`, `shaders/grass_bricks.comp.glsl`, `extension/src/world/brick_eval.cpp`
- Test: `tests/test_fjords_water.gd`

**Interfaces:**
- Produces, in `field_ops.glslh`:
  `void eval_base_views(vec3 p, out float sdf, out uint mat, out float gsdf, out uint gmat);` This is `eval_base_field_views` under `VE_FIELD_GROUND`; otherwise it is `eval_base_field` with `g = union`.
  `void eval_base_ground(vec3 p, out float sdf, out uint mat);`
- Changes, each with a CPU twin in `brick_eval.cpp` that uses `gen.sample_ground`:
  - `eval_field_pair`: with no override sample, the union comes from views and `osdf/omat` from the ground.
  - `eval_field_solid`: the solid accumulator is seeded from the ground; the union half is unchanged.
  - `base_field_gradient`: all seven taps call `eval_base_ground`.
  - `solid_sample` (`brick_gen`), and the `s_liquid` branch in `brick_mark`: under `VE_FIELD_GROUND`, always call `eval_field_solid` (deviation 3). The CPU twin is the matching shortcut in `cell_state_field` / `brick_eval.cpp:119-130`.
  - `grass_bricks.comp.glsl`: every `eval_field(…, 0u, 0u, …)` becomes `eval_base_ground(…)`.
- `tests/test_fjords_water.gd` opens two `VoxelWorld`s, one with `fjords.pipeline` and one with `res://tests/fixtures/fjords_no_water.pipeline`, set up like `test_fjords.gd`'s `_open` (seed 0). Each pumps sectors (`debug_pump_sectors`) around a valley point `V`, chosen in Step 1 as an XZ where `raycast` down hits below `W − 10`.

- [ ] **Step 1: Write the failing GPU tests:**
  - `test_underwater_gradient_is_the_grounds`: At 20 points on the floor around `V` (`y = ground + 0.05`), `debug_eval_field_gradient` in the water world equals the no-water world (`grad` within 1e-4). At least one of them is not within 0.05 of `(0, 1, 0)`, which proves the test is not looking at a flat floor.
  - `test_opaque_lattice_does_not_move`: Stream the same bricks around `V` in both worlds (`debug_stream_frame` until settled). Then `debug_brick_diff` (GPU against its CPU mirror) reports zero mismatches in the water world, and the opaque lattice bytes read from `debug_sdf_atlas` for a floor brick are equal across the two worlds.
  - `test_collision_lattice_does_not_move`: `debug_mesh_lattice_diff` on a floor chunk: the water world's result equals the no-water world's.
  - `test_grass_finds_the_ground_not_the_sea`: `debug_grass_stats` blade count in a settled shore view is within the no-water world's count ±1%. Pick the pose in Step 1 (the grass hook memory: avoid caves).
  - `test_water_is_in_the_union`: `debug_eval_field` at `V` with `y = W − 1` returns the water id; at `W + 1` it returns air.
- [ ] **Step 2:** Run `./build.sh && ./gdunit_tests.sh -a res://tests/test_fjords_water.gd`. Expected: the gradient, lattice and grass tests FAIL; the union test passes.
- [ ] **Step 3:** Implement the changes above.
- [ ] **Step 4:** Run `test_fjords_water.gd` (all pass), the native tests, then the full gdUnit run. Expected: no new failures, and `tests/golden/` is unchanged. `tests/test_brick_cache.gd` and `tests/test_conifers.gd` load `fjords.pipeline`. If either fails only because water is now present, point it at `fjords_no_water.pipeline` and say so in the commit message. Do not loosen its assertions.
- [ ] **Step 5:** Commit `feat: opaque, solid and gradient views read the ground under terrain water`.

### Task 4: Near-shell terrain candidates

**Files:**
- Modify: `extension/src/transparency/shell_grid.{h,cpp}`, `extension/src/lod/lod_system.{h,cpp}`
- Test: `extension/tests/test_fjords_water.cpp`, `tests/test_fjords_water.gd`

**Interfaces:**
- Produces: `ShellBox liquid_plane_box(const float cam[3], float radius_m, float level);` in `shell_grid.h`. It spans `[cam − radius, cam + radius]` in x and z and `[level, level]` in y.
- `LodSystem::refresh_shell_candidates`:
  - When `generator.liquid_level(cam.x, cam.z, &W)` is true, it appends `liquid_plane_box(cam.pos, radius, W)` to `boxes`. This happens inside the existing `recompute && enabled` block.
  - `recompute` is also true when the sector cache's `version()` differs from a new member, `shell_sector_version_`.
  - Reach the generator and cache the way the existing `lod_chunk_sectors_ready` does. Shell builds are already sector-gated, so nothing new gates them.

- [ ] **Step 1: Write the failing tests:**
  - Native `TEST_CASE("liquid_plane_box covers both layers at a chunk boundary")`: `shell_candidates({liquid_plane_box(cam, 10, 51.2)}, cam, 10)` returns chunks with y index 15 and 16 only. The count per layer equals the count for a box at `y = 52` (one layer).
  - GPU `test_shell_candidates_at_the_shore`: In the water world at a shore camera 3 m above `W`, after settling, `debug_lod_stats()["shell_pages"] > 0`. The no-water world reports 0.
  - GPU `test_candidates_appear_when_sectors_land`: Set the camera before any sector is resident, call `debug_lod_tick` once, then pump sectors without moving the camera. `shell_pages` becomes > 0.
- [ ] **Step 2:** Run both. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Both pass, and so does the full gdUnit run (no new failures). Record `shell_pages` at the shore camera in the commit message.
- [ ] **Step 5:** Commit `feat: near shell sees terrain water at the liquid level`.

### Task 5: Edit behaviour and captures

**Files:**
- Modify: `tools/fjord_capture.gd`
- Test: `tests/test_fjords_water.gd`

- [ ] **Step 1: Write the failing test** `test_removing_water_leaves_a_hole`: `debug_apply_sphere_subtract` of r = 2 at `(V.x, W − 1, V.z)`, then settle. `debug_eval_field` there is air, and it stays air after 120 more frames.
- [ ] **Step 2:** Run it. It may already pass, since ops carve the union. If it does, keep it as a pin.
- [ ] **Step 3:** Add four poses to `tools/fjord_capture.gd`, chosen with `raycast` the way the existing poses are:
  - `water_shore`: 3 m above `W`, looking along the shore.
  - `water_fjord`: 120 m up, looking down a long fjord to the horizon.
  - `water_under`: 5 m below `W`.
  - `water_seam`: 25 m up, looking across the fade band.
  Write the PNGs to `reports/fjords_water/`.
- [ ] **Step 4:** Run the capture and open every PNG at full size. Note, as the water spec §12.4 does, what was seen. Check specifically: water on both sides of the fade band with no seam or band; foam at the shore; the floor visible through shallow water; no opaque teal slab in the near field; the horizon reaching water.
- [ ] **Step 5:** Commit `test: fjords water edit pin and captures`.

### Task 6: Cost and the spec record

- [ ] **Step 1:** Run interleaved A/B/A with `demo/scripts/benchmark.gd --pipeline=`: A = `tests/fixtures/fjords_no_water.pipeline`, B = `assets/pipelines/fjords.pipeline`. Do this at two `--pose=`s: a dry wall view, and the `water_fjord` view. Use the water spec §12.3 table format: delta against the mean of the two A legs, the A/A spread, and quotable only where |delta| > spread. The dry view must show no quotable regression; if it does, stop and report.
- [ ] **Step 2:** Add a §7 "Deviations and results" to the spec: this plan's four deviations, any found during the build, the cost table, and the capture notes from Task 5.
- [ ] **Step 3:** Run the full native and gdUnit suites. Expected: no new failures against Task 0's baseline.
- [ ] **Step 4:** Commit `docs: fjords water results`.
