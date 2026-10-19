# Forest Caching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Walking through the Fjords conifer forest averages at least 60 fps (≤ 16.7 ms) on the M1, because near-field pixels read per-voxel caches instead of re-running the terrain pipeline and the sun march every frame.

**Architecture:** One new 3D texture, the *brick cache*, mirrors the SDF atlas slot for slot at a 20 cm lattice (5³ samples per brick, apron included). `brick_gen` writes the field gradient into `.xyz` when it generates a brick. A new `sun_bake` pass writes sun visibility into `.w`: for this frame's freshly generated bricks, plus a rolling sweep over every slot. The raymarcher reads both with one filtered fetch at the hit. Two per-slot flag buffers say whether a slot's normal or sun entry is valid; an invalid entry falls back to today's per-pixel path. A render setting `brick_cache` switches the whole thing off for A/B runs. After the caches, the plan prices the tree cards and then the primary march, and stops at a report gate if those don't close the gap.

**Tech Stack:** Godot 4 GDExtension (C++), GLSL compute through RenderingDevice (Metal on this Mac), gdUnit4 GPU tests (`./gdunit_tests.sh`), doctest native tests (`./build.sh --test`), benchmark legs (`tools/run_benchmarks.sh`).

**Spec:** `docs/superpowers/specs/2026-10-18-forest-caching-design.md`

## Global Constraints

- Finish line: the new `fjords-forest-walk` leg reports `BENCH frame_avg_ms` ≤ 16.7. Stop as soon as it does.
- No other leg regresses by more than about 1 ms or 5% of its average, whichever is larger. Baselines (avg ms, 2026-10-18): steady 13.03, ridge 18.90, move 14.09, edit 29.67, edit-bounded 16.32, island 17.38, fjords-forest 32.22.
- Memory: the new caches have a fixed size and never grow. Brick cache: `RGBA16_SNORM`, (64·5)×(32·5)×(32·5) texels = 65.5 MB, plus two `uint` buffers of `atlas_slot_count()` entries each (256 KB each).
- Accepted quality trades only: reduced-resolution cached normals (ground and trunks), cached sun visibility (blockier edges, a few frames of lag on cast shadows from edits and streaming), fewer conifer cards or an earlier impostor hand-off. Never: reusing last frame's pixels, shorter or half-resolution sun rays.
- An edited brick shades with its new normal and its own sun visibility the frame its regenerated SDF lands.
- Measuring: GPU timestamps are invalid on this Mac. Use wall-clock frame time, one Godot process per variant, A/B/A bracketing, and never two Godot processes at once.
- Generated GLSL (`shaders/generated/*.glslh`) is regenerated from C++, never hand-edited: `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests`.
- Plan and spec commits carry no Co-Authored-By line. Code commits use the repo's `perf:` / `feat:` / `chore:` / `test:` prefixes.
- Grass, leaf and conifer-card scatter passes keep calling `terrain_sun_visibility` themselves; only raymarch hits use the sun cache.
- The gdUnit suite has a standing set of failures on clean `main`. Before blaming a change for a red test, stash and re-run it on the base commit.

## Review Focus

1. **A reused atlas slot** must not show the previous occupant's cached normal or sun. `brick_mark` zeroes both flags when it allocates (Task 2, `test_a_reallocated_slot_starts_with_no_cache`).
2. **Bricks holding a transparent or liquid material** (ice, water) keep today's opaque-view normal path (Task 2, `test_an_ice_brick_is_not_normal_cached`, plus the existing `test_transparency.gd` suite passing with the cache on).
3. **Override, volume and island-merged bricks** report inexact gradients at bake time and fall back to their stored-normal paths (Task 2: the existing `test_normal_artifact.gd` suite must pass unchanged with the cache on).
4. **An edit** clears the regenerated brick's sun flag, and this frame's bake sets it again (Task 3, `test_an_edited_brick_is_rebaked_the_same_frame`).
5. **A change in sun direction** invalidates every sun entry (Task 3, doctest `sun_dir_changed`). No shipped scene moves the sun, so nothing else would exercise this.

---

### Task 1: Walking leg and baseline

**Files:**
- Modify: `demo/scripts/benchmark.gd` (mode list in `_ready`, `_settle`, the pose branch, `_process` movement)
- Modify: `tools/run_benchmarks.sh` (`NAMES`, `leg_args`)
- Test: `tests/test_benchmark.gd`

**Interfaces:**
- Produces: mode `--benchmark-walk`; `func _walk_step(pos: Vector3, yaw_deg: float, dt: float) -> Vector3` (returns the next position before ground snapping); leg name `fjords-forest-walk`.

- [ ] **Step 1: Write the failing test** in `tests/test_benchmark.gd`

```gdscript
func test_walk_step_moves_at_walk_speed_along_the_heading() -> void:
	var bench := benchmark_node()
	var p: Vector3 = bench.call("_walk_step", Vector3(1, 2, 3), 90.0, 0.5)
	# Godot forward is -Z; yaw 90 deg about +Y turns it to -X. Walk speed is player.gd's 6 m/s.
	assert_vector(p).is_equal_approx(Vector3(1 - 3.0, 2, 3), Vector3(1e-4, 1e-4, 1e-4))
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `./gdunit_tests.sh -a res://tests/test_benchmark.gd`
Expected: FAIL, `_walk_step` not found.

- [ ] **Step 3: Implement the mode**

- Add `const WALK_SPEED := 6.0` (player.gd `walk_speed`) and `_walk_step` (`pos + Basis(Vector3.UP, deg_to_rad(yaw_deg)) * Vector3.FORWARD * WALK_SPEED * dt`).
- Add `"--benchmark-walk"` to the `_ready` mode list (before `"--benchmark"`). Include it in `_settle`: it walks only after the world settles, so it measures walking, not world load.
- Pose: walk uses the same `_pose` branch as the default case. Keep the yaw in a new `_walk_yaw` (degrees; from `_pose[3]`, 0 without a pose).
- In `_process`, once `_settled_at >= 0`: move the player by `_walk_step(..., movement_delta)`, then snap to the ground with `_world.raycast(p + Vector3(0, 2, 0), Vector3.DOWN, 50.0)`. On a hit, `y = hit.pos.y + 0.9`, the capsule rest height `_stand_on_ground` uses. On a miss, keep `y`.
- Update the header comment block with one line for the new mode.

- [ ] **Step 4: Run the test and confirm it passes**

Run: `./gdunit_tests.sh -a res://tests/test_benchmark.gd`
Expected: PASS (all of the suite's tests).

- [ ] **Step 5: Add the leg** to `tools/run_benchmarks.sh`

Append `fjords-forest-walk` to `NAMES` and give it `leg_args`:
`--benchmark-walk --pipeline=res://assets/pipelines/fjords.pipeline --seed=880510467 --pose=-98.05,186.0,417.40,59.0,-5 --path-fps=60 --frames=600`.
That is 600 sampled frames at a fixed 1/60 s step, so the path is the same 60 m every run, whatever the frame rate. Add a comment on the leg like the fjords-forest one.

- [ ] **Step 6: Check the path stays in the forest.** Run the leg once with `--screenshot=/private/tmp/.../walk_end.png` appended, plus one at the start pose. Look at both images. If the 60 m line leaves the trees (open water, snow, a cliff), change the yaw in `leg_args` and re-check. Record the chosen yaw in the leg comment.

- [ ] **Step 7: Record the baseline**

Run: `tools/run_benchmarks.sh forest-cache-t1`
Expected: every leg `EXIT_STATUS=0`. Note `fjords-forest-walk`'s `frame_avg_ms`; this is the number every later task is judged against. Expected to be around 30–33 ms.

- [ ] **Step 8: Commit**

```bash
git add demo/scripts/benchmark.gd tools/run_benchmarks.sh tests/test_benchmark.gd
git commit -m "chore: fjords-forest-walk benchmark leg"
```

---

### Task 2: Brick cache with baked normals

**Files:**
- Modify: `extension/src/gpu_layout/constants.cpp` (emit the new constants), then regenerate `shaders/generated/constants.glslh`
- Create: `shaders/brick_cache.glslh`
- Modify: `extension/src/render/gpu_atlas.h`, `extension/src/render/gpu_atlas.cpp` (create/free the texture and buffers)
- Modify: `shaders/brick_gen.comp.glsl`, `extension/src/render/brick_gen_pass.cpp` (bindings 16–18)
- Modify: `shaders/brick_mark.comp.glsl`, `extension/src/render/region_pass.cpp` (bindings 17–18 on the mark set)
- Modify: `shaders/raymarch.comp.glsl`, `extension/src/render/raymarch_pass.cpp` (bindings 32–34)
- Modify: `extension/src/settings/render_settings.h`, `extension/src/settings/render_settings.cpp` (row `brick_cache`)
- Modify: `extension/src/render/frame_params.h`, `extension/src/render/frame_params.cpp`, `extension/src/render/frame.cpp`, every `set_near_field_flags` call in `extension/src/debug/hooks_render.cpp` (8 sites)
- Modify: `extension/src/debug/hooks.h`, `extension/src/debug/hooks.cpp`, `extension/src/debug/hooks_world.cpp` (two stats hooks)
- Test: `tests/test_brick_cache.gd` (new), `tests/test_transparency.gd`

**Interfaces:**
- Produces (C++ → generated GLSL constants): `BRICK_CACHE_DIM = 5` (samples per brick axis), `BRICK_CACHE_STEP = 4` (voxels between samples), `RENDER_BRICK_CACHE = 512u` (flags-word bit; 512 is free above `BEAUTY_SSAO = 256u`), `CACHE_OK = 1u`.
- Produces (`shaders/brick_cache.glslh`):
  - `ivec3 brick_cache_texel(int slot, ivec3 lattice, ivec3 atlas_bricks)`: an integer texel for `imageStore`/`imageLoad`; `lattice` is in [0, 4]³.
  - `vec3 brick_cache_uvw(int slot, vec3 vox, ivec3 atlas_bricks)`: normalized coordinates for a filtered fetch at brick-local voxel position `vox` in [0, 16]³. Clamp `vox`, then `(atlas_base(slot, atlas_bricks, BRICK_CACHE_DIM) + vox / BRICK_CACHE_STEP + 0.5) / (atlas_bricks * BRICK_CACHE_DIM)`.
- Produces (GpuAtlas): `RID brick_cache() const` (RGBA16_SNORM 3D, sampling + storage usage), `RID cache_normal_ok() const`, `RID cache_sun_ok() const` (uint per slot, zero-initialised).
- Produces (frame params): `void ve::set_near_field_flags(CameraParams *cp, uint32_t beauty_flags, bool brick_cache)`. It ORs `RENDER_BRICK_CACHE` in when `brick_cache` is set. The changed signature is deliberate: every caller, hooks included, has to choose.
- Produces (render setting): `RenderSettings::brick_cache` (bool, default true), settable as `set_effect_enabled("brick_cache", false)`, so `--effects-off=brick_cache` works on every leg.
- Produces (hooks): `Dictionary debug_brick_cache_stats()` returns `{slots, normal_ok, sun_ok}` counted over all slots. `int debug_brick_cache_flags(Vector3i brick)` returns `-1` when the brick isn't resident, else `normal_ok | (sun_ok << 1)`.
- Raymarch bindings: 32 `sampler3D brick_cache` on the existing LINEAR sampler, 33 `CacheNormalOk { uint v[]; }`, 34 `CacheSunOk { uint v[]; }` (read in Task 3).
- brick_gen bindings: 16 `rgba16_snorm image3D brick_cache` (writeonly), 17 normal_ok buffer, 18 sun_ok buffer.
- brick_mark bindings: 17 normal_ok buffer, 18 sun_ok buffer.

- [ ] **Step 1: Record today's numbers for the tests' references.** With the code unchanged, run `./gdunit_tests.sh -a res://tests/test_normal_artifact.gd,res://tests/test_transparency.gd,res://tests/test_conifers.gd`. Note which tests are already red. Those are baseline failures, not this task's.

- [ ] **Step 2: Write the failing tests** in `tests/test_brick_cache.gd`. Set the world up like `tests/test_normal_artifact.gd`'s `make_world()`, with `atlas_bricks = Vector3i(32, 16, 32)`. The forest test uses the Fjords helpers from `tests/test_conifers.gd` (`_open`, `_forest_hits`, `_stream_at`); copy the few lines you need, don't import them.

```gdscript
func test_streamed_surface_bricks_are_normal_cached() -> void:
	var world := make_world()
	var s: Dictionary = world.hooks().debug_brick_cache_stats()
	assert_int(s["normal_ok"]).is_greater(100)
	assert_int(s["sun_ok"]).is_equal(0) # nothing bakes sun until Task 3

func test_a_reallocated_slot_starts_with_no_cache() -> void:
	# Mark + generate a region, release its slot, mark a different region into the same slot
	# WITHOUT generating (debug_mark_region then no debug_generate_pending): every brick that
	# region marked reports debug_brick_cache_flags == 0.

func test_cached_trunk_normals_are_no_worse_than_today() -> void:
	# Fjords seed 0: find a trunk as test_trunks_are_bark does, stream there, then probe
	# 4 m in front of it looking at it, 15 deg down, 160x120.
	# Run debug_raymarch_normal_probe twice: brick_cache off, then on.
	# assert on.hits > 3000; on.rms_ndl <= off.rms_ndl + 0.005; on.rms_ndl <= 0.03
```

In `tests/test_transparency.gd`, alongside the ice-ball tests:

```gdscript
func test_an_ice_brick_is_not_normal_cached() -> void:
	# Same setup as test_the_marcher_sees_the_ground_through_an_added_ice_ball, then stream:
	assert_int(w.hooks().debug_brick_cache_flags(Vector3i((ground["pos"] / 0.8).floor())) & 1).is_equal(0)
```

- [ ] **Step 3: Run them and confirm they fail**

Run: `./gdunit_tests.sh -a res://tests/test_brick_cache.gd,res://tests/test_transparency.gd`
Expected: FAIL, the hooks don't exist yet.

- [ ] **Step 4: Constants, resources, switch.** Add the constants in C++ (next to the beauty flags in `gpu_layout/constants.cpp`) and regenerate the GLSL. Create the texture and buffers in `GpuAtlas::initialize`, freeing them in its teardown. Add the `brick_cache` row. Change `set_near_field_flags`, then update `frame.cpp` (from `settings.brick_cache`) and the 8 hook call sites (from `render->render_settings()` or the orchestrator's equivalent getter). Run `./build.sh --test`; expected: the native suite passes, including `test_generated_glsl`.

- [ ] **Step 5: Bake in `brick_gen.comp.glsl`.** After phase 5, threads `tid < 125` evaluate `eval_field_gradient` (op span `op_base`, `s_op_n`) at lattice point `bo + vec3(l) * (BRICK_CACHE_STEP * VOXEL_SIZE)`. Each stores `vec4(normalize(grad), 1.0)` through `brick_cache_texel`. The brick is a **miss** (it stores nothing meaningful, and `normal_ok[slot] = 0`) if any of these hold:
  - any thread's gradient is inexact or has length < 1e-8 (overrides and volumes land here, because brick_gen binds no stored-normal buffers);
  - `s_liquid != 0`;
  - any cell's `s_mat` is `mat_transparent`.

  Collect the verdict in a shared `uint` with `atomicOr`. After the barrier, thread 0 writes `normal_ok[slot] = hit ? CACHE_OK : 0u` and `sun_ok[slot] = 0u` (the brick changed, so its sun entry is stale). The conifer cell cache already covers the lattice points; they lie inside the brick plus its +16 face.

- [ ] **Step 6: Clear on allocation in `brick_mark.comp.glsl`.** Next to `brick_flags.v[slot] = BRICK_FLAG_CONSERVATIVE;`, zero both `normal_ok[slot]` and `sun_ok[slot]`.

- [ ] **Step 7: Read in `raymarch.comp.glsl`.**
  - In `march_bricks`, at the hit, when `(flags & RENDER_BRICK_CACHE) != 0u && cache_normal_ok.v[slot] == CACHE_OK`: `h.n = normalize(textureLod(brick_cache, brick_cache_uvw(slot, vox_at_hit, pc.atlas_bricks.xyz), 0.0).xyz)`. Otherwise call `terrain_source_normal` as today.
  - Read `flags` from `pc.cam_pos.w`, as `main()` does.
  - Delete the `MAT_BARK` early-out, and the `g_field_skip_detail = true/false` pair around `eval_field_gradient`, from `terrain_source_normal`. Drop its `hit_mat` parameter. That makes the miss path exactly what it was before `1b18a2b`.

- [ ] **Step 8: Add the hooks and run the tests**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_brick_cache.gd,res://tests/test_transparency.gd,res://tests/test_normal_artifact.gd,res://tests/test_conifers.gd,res://tests/test_raymarch_gbuffer.gd,res://tests/test_raymarch_pixel.gd`
Expected: the new tests pass. Every test that was green in Step 1 is still green; `test_normal_artifact.gd` is the characterization suite for the miss paths.

- [ ] **Step 9: Measure.** Run `tools/run_benchmarks.sh forest-cache-t2`. Bracket the walk and standing forest legs once more with `--effects-off=brick_cache`: run the leg with the cache on, then off, then on again.
  - Expected: the forest legs drop by roughly 7–8 ms (the measured ceiling is 8.6).
  - The move leg must stay inside the regression limit. Brick generation does about 15% more field work now.
  - If the move leg regresses past the limit, report it with the numbers before continuing.

- [ ] **Step 10: Look at it.** Capture the forest pose with the cache on and off (`--screenshot=` on the standing leg). Check trunks, the trunk/ground junction and the forest floor. Name any visible difference in the commit message body. Expected: bark loses its lattice speckle.

- [ ] **Step 11: Commit**

```bash
git add -A shaders extension/src tests
git commit -m "perf: bake terrain normals into a per-brick cache"
```

---

### Task 3: Sun visibility in the brick cache

**Files:**
- Create: `shaders/sun_bake.comp.glsl`
- Create: `extension/src/render/sun_bake_pass.h`, `extension/src/render/sun_bake_pass.cpp`
- Modify: `extension/src/gpu_layout/blocks.h` (`SunBakePush`), regenerate `shaders/generated/blocks.glslh`
- Modify: `extension/src/render/frame.cpp` (dispatch between the stream stage and the raymarch, inside the `!settings.raster_mode` branch, after `set_near_field_world`), the orchestrator's pass list (create and own the pass)
- Modify: `shaders/raymarch.comp.glsl` (use `.w`)
- Modify: `extension/src/debug/hooks.h`, `extension/src/debug/hooks.cpp`, `extension/src/debug/hooks_render.cpp`
- Test: `tests/test_brick_cache.gd`, `extension/tests/test_sun_bake.cpp` (new)

**Interfaces:**
- Consumes: Task 2's `brick_cache()`, `cache_normal_ok()`, `cache_sun_ok()`, `brick_cache_texel`, `brick_cache_uvw`, `RENDER_BRICK_CACHE`, `CACHE_OK`.
- Produces (`blocks.h`): `struct SunBakePush { IVec4 dims; IVec4 region_origin; IVec4 atlas_bricks; IVec4 cfg; }` as `SUN_BAKE_PUSH_FIELDS`. The field names match `brick_atlas.glslh`'s `pc` contract. `cfg.x` is the mode (0 = this frame's jobs, read through `jobs.v[wg * 2].w`; 1 = the slot range `[cfg.y, cfg.y + cfg.z)`).
- Produces (`sun_bake_pass.h`): `bool SunBakePass::initialize(RenderingDevice*, const GpuAtlas&, RID sun_ubo)`; `void SunBakePass::bake_jobs(RenderingDevice*, const GpuAtlas&, const ve::CameraParams&)` (indirect, from `atlas.dispatch_args()`); `void SunBakePass::bake_sweep(RenderingDevice*, const GpuAtlas&, const ve::CameraParams&, int first_slot, int count)`; `void SunBakePass::note_sun(RenderingDevice*, const GpuAtlas&, const float dir[3])`, which `buffer_clear`s `cache_sun_ok` when `ve::sun_dir_changed` says so.
- Produces (pure, in `sun_bake_pass.h`): `bool ve::sun_dir_changed(const float prev[3], const float cur[3])`, true when the angle exceeds 0.1° (dot < cos(0.1°)), or when `prev` is all zeros (first frame).
- Constant: `kSunSweepSlotsPerFrame = 512`. A full sweep of 65 536 slots takes 128 frames, about 2 s at 60 fps. Raise or lower it only on Step 8's numbers.
- Produces (hook): `Dictionary debug_raymarch_sun_probe(Vector3 origin, Vector3 dir, int w, int h)` → `{hits, mean_abs_diff, flip_fraction}`. It sweeps every slot, renders the probe once with the cache flag and once without (as `debug_raymarch_normal_probe` does), and compares `out_albedo.a` on hit pixels. `flip_fraction` is the share of hit pixels whose visibility differs by more than 0.5.

- [ ] **Step 1: Write the failing tests**

`extension/tests/test_sun_bake.cpp`:

```cpp
TEST_CASE("sun_dir_changed: first frame, still, and a real turn") {
	const float zero[3] = {0, 0, 0}, a[3] = {0.5746958f, 0.7662610f, 0.2873479f};
	const float a_jitter[3] = {0.5746960f, 0.7662608f, 0.2873480f};
	const float b[3] = {0.0f, 1.0f, 0.0f};
	CHECK(ve::sun_dir_changed(zero, a));
	CHECK_FALSE(ve::sun_dir_changed(a, a_jitter));
	CHECK(ve::sun_dir_changed(a, b));
}
```

`tests/test_brick_cache.gd`:

```gdscript
func test_cached_sun_matches_the_marched_sun() -> void:
	# Default terrain world from make_world(); the probe looks across hills that self-shadow:
	# origin (20, 72, 29), dir (0.1, -0.85, -0.5), 256x192, raymarched_sun_shadow ON.
	# assert hits > 12000; mean_abs_diff <= 0.08; flip_fraction <= 0.03

func test_an_edited_brick_is_rebaked_the_same_frame() -> void:
	# make_world(), debug_apply_sphere_subtract at a surface hit, then stream one frame:
	# flags for the edited brick == 1 (normal only: brick_gen cleared the sun bit).
	# Then run the job bake through the hook path the frame uses (debug_sun_bake_jobs())
	# and assert flags == 3.
```

(The second test needs `void debug_sun_bake_jobs()`, a thin hook that calls `SunBakePass::bake_jobs` with the probe camera's world half. Add it alongside the probe hook.)

- [ ] **Step 2: Run them and confirm they fail**

Run: `./build.sh --test` (expected: `sun_dir_changed` undefined) and `./gdunit_tests.sh -a res://tests/test_brick_cache.gd` (expected: missing hooks).

- [ ] **Step 3: Write `sun_bake.comp.glsl`.** One workgroup per slot, `local_size_x = 128`, threads `< 125` each own one lattice point.
  - Return early (the whole workgroup) when the slot's `brick_flags` lack `BRICK_FLAG_HAS_SURFACE`. Sweep slots are mostly empty, and this is what keeps the sweep cheap.
  - Per point, read `d = brick_sdf(slot, vec3(l * BRICK_CACHE_STEP))`. If `d >= SDF_RANGE * 0.99` (saturated, far from any surface), write `w = 1`.
  - Otherwise take the normal `n` from the cache's `.xyz` when `normal_ok`; else take the R8 central difference with `world_sdf` at ±`VOXEL_SIZE`. Project onto the surface: `ps = p - n * d`. Write `w = terrain_sun_visibility(ps + n * 0.06, RAY_SHADOW_DIST)`.
  - Preserve `.xyz` through `imageLoad`/`imageStore`. After a barrier, thread 0 sets `sun_ok[slot] = CACHE_OK`.
  - Include `brick_atlas.glslh`, `sun_light.glslh` (set 2, binding 24, as raymarch defines it) and `sun_march.glslh` with `SUN_MARCH_REGION_BOUNDS`. Bindings: the atlas buffers raymarch uses for the march (sdf atlas on the linear sampler, region map, tables, slot counts, bounds, brick flags), plus the brick cache as an image and both flag buffers.

- [ ] **Step 4: Write the pass and the frame wiring.** `SunBakePass` follows `BrickGenPass`'s shape (one compiled program, one cached uniform set, `gpu::` helpers). In `frame.cpp`, when `settings.brick_cache`, call these in one compute list before `rmp->render`:
  1. `note_sun` with this frame's sun direction;
  2. `bake_jobs`;
  3. `bake_sweep(cursor, kSunSweepSlotsPerFrame)`.

  Advance a member cursor modulo `atlas_slot_count()`. Time it under its own label `"sun_bake"`.

- [ ] **Step 5: Read in `raymarch.comp.glsl`.** Carry the hit's `slot` and brick-local `vox` out of `march_bricks` in `Hit`. Two new fields, filled only on terrain hits; islands leave `slot = -1`. In `main()`, inside the `BEAUTY_RAY_SUN_SHADOW` branch: when the cache flag is on, `best.slot >= 0` and `cache_sun_ok.v[best.slot] == CACHE_OK`, set `sun = min(cached.w, island_sun_visibility(sro, island_count, RAY_SHADOW_DIST))`. Otherwise keep today's two-march expression.

- [ ] **Step 6: Run the tests and confirm they pass**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_brick_cache.gd,res://tests/test_sun_shadow.gd,res://tests/test_normal_artifact.gd,res://tests/test_transparency.gd`
Expected: the new tests pass; nothing that was green in Task 2 Step 1 has gone red.

- [ ] **Step 7: Look at it.** Capture the standing forest pose with the cache on and off. Look at trunk shadows on the forest floor and at self-shadowing on slopes, and name what changed. Expected: edges a little softer or blockier at 20 cm; no light leaks under trunks.

- [ ] **Step 8: Measure.** Run `tools/run_benchmarks.sh forest-cache-t3`, then bracket the walk leg with `--effects-off=brick_cache` (on, off, on).
  - Expected: about 3 ms more saved on the forest legs, minus the bake's own cost.
  - The edit and edit-bounded legs must stay inside the limit; they generate bricks every frame, so they pay `bake_jobs` every frame.
  - If the walk leg now reads ≤ 16.7 ms, skip to Task 6.

- [ ] **Step 9: Commit**

```bash
git add -A shaders extension/src extension/tests tests
git commit -m "perf: bake sun visibility into the brick cache"
```

---

### Task 4: Price and trim the forest's raster passes

**Files:**
- Modify (only if a trim is adopted): `extension/src/conifers/conifer_settings.h` (defaults), `extension/src/grass/` or `extension/src/leaves/` settings defaults
- Test: `tests/test_conifers.gd` (only if a default changes and an existing assertion encodes the old value)

**Interfaces:**
- Consumes: the benchmark's existing `--conifer-value=name=v`, `--grass=0` and `--leaves=0` switches. These are the working ones; `--effects-off=conifers/grass/leaves` is silently ignored.

- [ ] **Step 1: Price each pass on the walk leg.** One process per variant, bracketed by plain runs at the start and the end:
  - `--conifer-value=enabled=0`
  - `--grass=0`
  - `--leaves=0`
  - `--conifer-value=card_reach_m=150`
  - `--conifer-value=card_reach_m=75`
  - `--conifer-value=clumps_per_tree=64`

  Write the table (variant, avg ms, saving) into the commit message body.

- [ ] **Step 2: Choose.** Pick the smallest change to the card / impostor knobs (`card_reach_m`, `impostor_reach_m`, `clumps_per_tree`) that brings the walk leg to ≤ 16.7 ms. Only these knobs are accepted trades. Capture the standing forest pose before and after and name what got thinner. If no setting of these knobs gets there, adopt none of them and go to Task 5.

- [ ] **Step 3: Apply the default and run the conifer suite**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_conifers.gd`
Expected: green. A test that asserts the old default (for example card trees inside 300 m) is updated to the new value in the same commit.

- [ ] **Step 4: Measure all legs**

Run: `tools/run_benchmarks.sh forest-cache-t4`. Expected: walk ≤ 16.7 ms, or a clear remaining gap; no leg past the regression limit.

- [ ] **Step 5: Commit** (only if a default changed)

```bash
git add extension/src tests
git commit -m "perf: trim conifer cards for the forest walk"
```

---

### Task 5: Primary march (only if the walk is still over 16.7 ms)

This is in scope only as **lossless** speed-ups to the primary march. It ends at a report gate, because what to build depends on what the measurement shows.

**Files:** none modified until the user picks; scratch scripts go in the session scratchpad.

- [ ] **Step 1: See where the steps go.** Turn on the cost view (`BEAUTY_COST_VIEW`, via the beauty `cost_view` setting) at the walk start pose. Capture it and read `cost_out` through `debug_raymarch_cost_probe` at the same pose. Report the median and p95 steps per pixel, split into trunk, ground and sky pixels.

- [ ] **Step 2: Price candidate cuts with shader overrides.** Use one process per variant, with a scratch SceneTree script that sets `debug_set_shader_override("raymarch.comp.glsl", ...)` before `add_child` and lets `benchmark.gd` time the walk leg. Variants:
  1. drop the 4-step secant refinement to 2;
  2. raise the minimum step `0.005` to `0.01`;
  3. start the march at `region_bounds` entry (already done; confirm) and at the brick's `HAS_SURFACE` entry.

  Any variant that changes the image is not lossless. Compare same-pose captures, and drop a variant whose capture differs visibly.

- [ ] **Step 3: Report and stop.** Send the user the per-variant table and captures, with a recommendation. Don't implement anything from this task before they answer.

---

### Task 6: Close out

**Files:**
- Modify: `/Users/jeremyzhao/.claude/projects/-Users-jeremyzhao-Development-godot-voxel-everything/memory/fjords-forest-cost-is-analytic-normal.md` (outcome line)

- [ ] **Step 1: Full suites**

Run: `./build.sh --test && ./gdunit_tests.sh`
Expected: native suite green; gdUnit red only in tests that are also red on `1b18a2b`. Re-run any newly red suite on a stash of the base commit to prove it.

- [ ] **Step 2: Final numbers.** Run `tools/run_benchmarks.sh forest-cache-final`. Compare every leg with the Global Constraints baselines and with `forest-cache-t1`'s walk number. Expected: walk ≤ 16.7 ms; no leg past the regression limit.

- [ ] **Step 3: Record the outcome** in the memory file named above: before and after per leg, the cache's memory cost, and which quality trades shipped.

- [ ] **Step 4: Commit** any test-threshold or comment fixes left over (memory files are outside the repo, so they aren't committed).
