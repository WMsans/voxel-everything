# M7 Budget Closure, Demo Polish & Portfolio Capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the demo's headline claim — *60 fps @ 1440p on an RTX 4070 Laptop* — a measured fact rather than a hope, then wrap it in a demo a stranger can drive and a capture rig that turns a run into a portfolio video.

**Architecture:** M6 shipped with `raymarch=WARN` and `frame=WARN` on all five benchmark legs, and the frame numbers were qualified because Wayland ignored `--disable-vsync`. M7 attacks that in three movements. **Movement A (Tasks 1–2) makes the numbers trustworthy**: the benchmark reports the V-Sync mode it actually got instead of the one it asked for, every GPU millisecond inside the compositor gets a label (streaming was untimed, which is where the edit leg's spike hid), and the raymarcher gains a cost view so "the march is slow" becomes "the march spends N steps here". **Movement B (Tasks 3–9) closes the gaps the measurements name**: one flag word per brick instead of eight texel fetches per DDA step, the region-level DDA that spec §3 has always described and this engine never had, per-brick op filtering so a region's 200th edit does not tax every voxel evaluation in it, op-list consolidation into override bricks so the 256-op cap stops being a wall, the collider octant split that spreads Jolt's BVH build across frames, and occupancy taken from the generated 5 cm lattice instead of a 27-sample probe. **Movement C (Tasks 10–13) is the demo**: a tool selector and help overlay, a shader hot-reload and a differential self-check keybind (spec §8's two dev-build promises), a deterministic capture rig that renders a PNG sequence independent of frame rate, and a final measured sweep whose verdicts — including spec §4's fade-band contingency — are recorded in the Errata as this project's closing numbers.

**Tech Stack:** Godot 4.7.1 (`/usr/bin/godot`), godot-cpp (pinned master, `api_version = "4.7"`), SCons, C++20, GLSL 460 (Vulkan), Jolt Physics, doctest 2.4.11 (native), gdUnit4 (in-engine), ffmpeg (offline capture encode only).

**Spec:** `docs/superpowers/specs/2026-08-12-voxel-engine-design.md` — M7 closes §8's "Benchmark scene" bullet, §3's three-level traversal ("Region DDA (25.6 m) → brick DDA (0.8 m) → in-brick sphere tracing"), §2's "Op-list overflow (>256 ops/region) consolidates into explicitly stored override bricks", §4's fade-band contingency, §8's "hot-reload keybind in dev builds" and "dev console command runs both and diffs", and every per-pass budget in §7. `docs/superpowers/specs/2026-08-17-m5-lod-design.md` still supersedes §2/§4's level table; read it before touching anything LoD-shaped.

**Predecessors:** `2026-08-12-m1-walking-skeleton.md`, `2026-08-13-m2-gpu-generation-streaming-edits.md`, `2026-08-14-m3-physics-meshing-colliders.md`, `2026-08-15-m4-connectivity-islands.md`, `2026-08-17-m5-far-field-lod.md`, `2026-08-18-m6-beautification.md` (all complete). **Read all six Errata sections before touching a shader, a pass, or the benchmark.** The ones this plan collides with directly:

- **M6 errata 4** — live Vulkan timestamps are normalised from nanoseconds to microseconds by `raw * 0.001`, exactly once, in `GpuTimings::poll`. Task 1 adds a pass name and a derived key; it must not add a second scaling.
- **M6 errata 3** — `lod_ms` is CPU command-record time. `lod_gpu_ms` is the timestamp delta. Never mix them in a verdict.
- **M6 errata 2** — Wayland printed a V-Sync fallback (`Disabled` unavailable, `Enabled` used) and every frame verdict from M6 is qualified because of it. Task 1 exists to stop that from ever being a footnote again.
- **M6 errata 1** — Godot's `normal_roughness` buffer probes as a constant `1.0` here; nothing in M7 may start depending on it.
- **M5 errata 3** — index push constants by **float**, not byte, and keep the `PackedByteArray` size and `draw_list_set_push_constant`'s size argument in step.
- **M5 errata 5** — the gdUnit runner is `./gdunit_tests.sh`. Never `addons/gdUnit4/runtest.sh`. Pass `-c` to see every failure.
- **M5 errata 6** — **a fixed frame count settles nothing.** Wait on a condition. Every settle helper in this plan polls stats.
- **M4 errata 1** — `collect_ops_for_aabb` flattens cross-region lists in global append order and can exceed `kMaxRegionOps`; consumers truncate to a chronological prefix. Task 7's consolidation must preserve that ordering rule.
- **M3 errata 1** — this codebase's winding convention has already cost one bug. Task 8 splits a chunk's triangles between bodies; it must not touch winding.
- **M2 errata 5** — GLSL reserved words (`active`, `mat2`, `sample`, `filter`, `output`, `light`, `normal`) are rejected or shadow builtins. This plan's shaders use `n`, `wn`, `op_n`, `s_ops`.
- **M2 errata 7** — `ivec4` push-constant members need `.xyz` when passed to `ivec3` parameters.

## Milestone Map

| Milestone | Delivers |
|---|---|
| M1 (done) | Toolchain, raymarched terrain, test harnesses |
| M2 (done) | GPU brick generation, region indirection, residency/LRU, min–max mips, destruction edits |
| M3 (done) | Dual-contour collision meshing on the GPU, async readback, collider streaming into Jolt, character controller |
| M4 (done) | Occupancy grid, connectivity, island carve/extract/spawn/re-merge, raymarched island targets, tiled culling, debris |
| M5 (done) | Eight-level LoD octree, 12-byte quad arena, shared triplanar materials, HiZ occlusion, indirect multi-draw far field, dithered seam |
| M6 (done) | Merged G-buffer, deferred cel lighting, three shadow layers, SSGI, SSR, outlines, cel dynamic objects, per-pass GPU timings |
| **M7 (this plan)** | Honest measurement, the raymarch and edit budgets closed, override-brick consolidation, collider spike split, demo shell, dev keybinds, capture rig, final verdicts |

## Global Constraints

- Godot **4.7.1**; godot-cpp pinned to the existing submodule commit, `api_version = "4.7"` — do not bump either.
- Pure C++ cores in `namespace ve` contain **zero Godot types** (engine spec §8). New pure code lands in `world/` and `generator/`; `render/` and `physics/` stay Godot glue. Anything that packs, filters, plans or bounds is pure; anything owning a `RID` is glue.
- Shaders: GLSL `#version 460`, loaded **from files** by `ve::load_shader_source`, never inline strings. `#[compute]` / `#[vertex]` / `#[fragment]` are stripped after load by `ve::strip_shader_annotations`.
- **Never put a literal include directive inside a GLSL comment** — the loader matches the token anywhere on a line and self-includes.
- `buffer_update`, `buffer_clear`, `texture_update`, `texture_copy`, `texture_clear` are device-level commands: record them **before** `compute_list_begin` / `draw_list_begin`, never inside an open list.
- **Push constants stay ≤ 128 bytes** (Vulkan's guaranteed minimum).
- `ve::EditOp` stays exactly **32 bytes** (`static_assert`). M7 adds **no** op types: consolidation replaces the field's *base*, it does not describe itself as ops.
- Reverse-Z everywhere; `COMPARE_OP_GREATER_OR_EQUAL` for anything writing scene depth.
- **There is exactly one field implementation per language.** `shaders/field.glslh` is the only GPU copy of `G + ops`; `ve::eval_field` / `ve::apply_ops` is the CPU copy. Every change to one lands in the other in the **same task**, and the differential suites (`test_field_diff.gd`, `test_brick_diff.gd`, `test_mesh_diff.gd`, `test_lod_mesh_diff.gd`, `test_field_volume_diff.gd`) are what proves it.
- Error policy (engine spec §8): dev = validation layers + verbose RD checks; release = fail-soft. **A slower correct frame beats a fast wrong one, and a plainer image beats a black one.** A refused consolidation leaves the op list as it was; a full override pool refuses rather than evicting something on screen; a failed shader reload keeps the old pipeline.
- **Every optimisation carries its own proof of harmlessness.** A task that changes what the marcher or the field evaluator computes ships with a test that pins the output against the pre-existing oracle (`debug_raycast`, `debug_raymarch_pixel`, the differential suites), not merely with a faster number.
- **Every optimisation carries a measured delta.** Tasks 3, 4, 5, 7, 8 each end by re-running the affected benchmark leg and recording p50/p99 before and after in the Errata. A task whose measured delta is inside the noise is reverted and recorded as such — that is a result, not a failure.
- Commit style: conventional (`feat:`, `test:`, `fix:`, `build:`, `refactor:`, `docs:`, `perf:`).
- RD API reference: `docs/api/renderingdevice.md`. Consult it before inventing a signature.
- Target hardware: RTX 4070 Laptop. Budgets from spec §7: raymarch ≤ 6 ms, LoD ≤ 2 ms, SSGI ≤ 1.5 ms, SSR ≤ 1.5 ms, shadows ≤ 1 ms, outlines ≤ 0.3 ms, frame ≤ 16 ms @ 1440p.

## Conventions Used Throughout

- **Build:** `./build.sh -j$(nproc)` (or `cd extension && scons -j$(nproc)`)
- **Native tests:** `cd extension && scons test`
- **gdUnit tests:** `./gdunit_tests.sh -a res://tests/<suite>.gd`, or `./gdunit_tests.sh` for everything. Add `-c` to see every failure.
- **Demo:** `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
- **Benchmark (after Task 1):** `tools/run_benchmarks.sh <label>` — runs all legs, tees to `reports/<label>/`, prints the verdict table.
- gdUnit tests that await must declare the timeout argument: `func test_x(timeout := 10000) -> void:`
- Every gdUnit suite creating a `VoxelWorld` registers it in `_worlds` and frees it in `after_test()` (M3 errata 2).
- New `extension/src/*/​*.cpp` under `world/`, `generator/`, `mesh/`, `connectivity/`, `lod/`, `shade/` are already covered by `SConstruct`'s `pure_sources` globs and by the library build; **no SConstruct edit is needed in this milestone**. New `extension/tests/test_*.cpp` are picked up by `Glob("tests/*.cpp")`.
- A benchmark leg is `WARN` when its p99 exceeds the budget and `PASS` otherwise; `UNMEASURED` below 30 samples. Never soften a budget to turn a WARN into a PASS — record the WARN.

## Fixed Numbers (decided here; every task assumes them)

| Thing | Value | Where it lives |
|---|---|---|
| New GPU timing scope | `"stream"` — island uploads + region mark/free + indirect brick generation | `raymarch_compositor.cpp`, `kPasses` in `gpu_timings.cpp` |
| Derived timing key | `unattributed_gpu_ms = max(0, custom_frame_gpu_ms − Σ pass keys ≥ 0)` | `GpuTimings::ingest_for_test` |
| V-Sync verdict keys | `vsync_requested`, `vsync_actual`, `verdict_qualified` | `demo/benchmark.gd` |
| Benchmark display driver | `x11` preferred (honours `--disable-vsync`); `wayland` runs are printed as qualified | `tools/run_benchmarks.sh` |
| Cost-view flag bit | `128u` (`ve::kFlagCostView`, `BEAUTY_COST_VIEW`) | `shade/beauty_settings.h`, `shaders/shade.glslh` |
| Cost-view scale | 1 heat unit = **1 marching step**; 0 → black, 512 → white, clamped | `raymarch.comp.glsl` |
| Brick flag word | bit 0 `has_surface`, bit 1 `has_material`; **3 = conservative** (march it) | `ve::kBrickFlagHasSurface`, `kBrickFlagHasMaterial` |
| Brick flag buffer | one `uint` per atlas slot (65 536 slots = 256 KB) | `GpuAtlas::brick_flags()`, raymarch binding **21** |
| Region slot counts binding | existing `GpuAtlas::region_slot_counts()` | raymarch binding **22** |
| Region DDA cell | **25.6 m**; skip whole region when its map slot < 0 **or** its slot count is 0 | `march_terrain`, `terrain_sun_visibility` |
| Op filter pad (bricks) | `kActivationPad + kVoxelSize` = **0.20 m** | `ve::op_touches_aabb` |
| Op filter pad (mesh/LoD lattices) | `lattice pitch + kSdfRange` | callers of `ve::op_touches_aabb` |
| Consolidation trigger | region op count reaches **192** of `kMaxRegionOps` (256) | `ve::kConsolidateAtOps` |
| Consolidated regions cap | **32** simultaneously | `ve::kMaxOverrideTables` |
| Override brick record | 17³ encoded SDF (4913 B) + 16³ material bytes (4096 B) = **9009 B** | `ve::OverrideBrick` |
| Override pool | **8192 bricks ≈ 74 MB**, exported as `VoxelWorld::max_override_bricks` | `ve::OverrideStore`, `OverridePool` |
| Override lookup | per-region **brick → slot table** (32³ ints), never an op — the field's *base* is replaced, so the 256-op budget is not spent describing it | `ve::OverrideStore`, `override_tables` SSBO |
| Consolidation planning pad | `kSdfRange + kVoxelSize` = **0.69 m** around each op's AABB | `ve::plan_consolidation` |
| Collider octants | **8** sub-bodies per collision chunk, split by triangle centroid | `ve::kColliderOctants` |
| Occupancy source | generated brick's own 5 cm lattice min/max; the 27-sample probe writes only for bricks with **no** surface | `brick_gen.comp.glsl`, `brick_mark.comp.glsl` |
| Demo tool slots | `1` subtract, `2` add, `3` paint, `4` drill | `demo/edit_tool.gd` |
| Demo radius range | 0.5 m … 8.0 m, mouse wheel, ×1.25 per notch | `demo/edit_tool.gd` |
| Demo keys | `F1` beauty menu, `F2` help, `F3` cost view, `F4` HUD cycle, `F5` shader reload, `F6` self-check, `F12` screenshot, `P` pause | `demo/*.gd` |
| Capture leg | **900 frames at a fixed 1/60 s step**, camera driven by frame index | `demo/capture.gd` |
| Capture output | `user://capture/frame_%05d.png`, encoded by `tools/encode_capture.sh` | `demo/capture.gd` |

**Memory added by M7.** Brick flags 256 KB + override pool 74 MB + up to 32 override tables (32³ ints each = 128 KB) 4 MB ≈ **78 MB**, and only the flags are always resident: the override pool allocates lazily on the first consolidation. On top of M5's ≈ 238 MB and M6's ≈ 180 MB this stays inside an 8 GB laptop card.

## Deliberate Deferrals (recorded, not forgotten)

- **Save/load of the edit log.** Spec §2 notes that saves are "generator seed/params + edit log (tiny)". The machinery is all there (the log is the save), but no file format, versioning or UI is in this milestone, and the demo is a flythrough, not a session. This is the one spec bullet M7 knowingly leaves open.
- **Coarse override bricks.** A consolidated region's override lattice is stored at 5 cm only. LoD levels sample it at their own (much coarser) pitch, which is a point sample of a fine field — the one place in this engine where a coarse level is not evaluated independently and exactly. Spec §2 introduces override bricks as the escape hatch and spec §4 states "imprecise is fine — detail comes from the bake" for LoD geometry, so the deviation is deliberate, bounded to regions that have absorbed 192+ edits, and recorded as a visual verdict in Task 13.
- **BC compression of the override pool.** 74 MB uncompressed is affordable; compressing it would add a decode path to five field consumers to save memory nothing is short of.
- **Threaded Jolt shape building.** Task 8 splits a chunk into eight bodies so the work is *divisible*; it still runs on the calling thread, because `PhysicsServer3D` is not safe to call from a worker by default (M3's deferral stands).
- **Temporal upsampling of the 0.66× raymarch.** Spec §3 says "compute pass at ~0.66× resolution + temporal upsample"; the composite resolves spatially today. Tasks 3–4 attack the march's cost directly instead, because a temporal resolve trades a measured budget for a class of ghosting artefacts on a destructible world where every edit invalidates history. If Task 13's sweep still shows `raymarch=WARN`, the recorded recommendation is a temporal resolve, not a further micro-optimisation.

## File Structure

```
extension/src/
  world/
    brick_flags.h/.cpp     the one-word-per-brick summary + packing        (Task 3)
    override_store.h/.cpp  override brick pool, sampling, consolidation
                           planning                                        (Task 6)
    edit_log.h/.cpp        MODIFIED: consolidation hooks, op replacement    (Task 6)
    brick_eval.h/.cpp      MODIFIED: op filtering + override base           (Tasks 5, 6)
    raycast.h/.cpp         MODIFIED: overrides threaded through             (Task 6)
  generator/
    edit_ops.h/.cpp        MODIFIED: op_touches_aabb, kOpBrickOverride      (Tasks 5, 6)
  mesh/
    octant_split.h/.cpp    triangle-centroid octant binning                 (Task 9)
  render/
    gpu_atlas.h/.cpp       MODIFIED: brick_flags buffer, override pool      (Tasks 3, 7)
    gpu_timings.h/.cpp     MODIFIED: "stream" scope, unattributed key       (Task 1)
    override_pool.h/.cpp   the override SSBOs, one per device               (Task 7)
    consolidate_pass.h/.cpp  region -> override bricks on the GPU           (Task 7)
    mesh_pass.h/.cpp       MODIFIED: override bindings on the worker device (Task 7)
    lod_build_pass.h/.cpp  MODIFIED: override bindings on the worker device (Task 7)
    raymarch_pass.h/.cpp   MODIFIED: bindings 21-22                         (Tasks 3, 4)
  physics/
    collider_streamer.h/.cpp MODIFIED: eight bodies per chunk               (Task 9)
  shade/
    beauty_settings.h/.cpp MODIFIED: kFlagCostView                          (Task 2)
  raymarch_compositor.cpp  MODIFIED: "stream" scope, reload latch           (Tasks 1, 12)
  voxel_world.h/.cpp       MODIFIED: consolidation policy, debug surface    (Tasks 1-14)
shaders/
  common.glslh             MODIFIED: brick flag constants                   (Task 3)
  field.glslh              MODIFIED: FIELD_OP_LIST hook, override sampling  (Tasks 5, 6, 7)
  brick_gen.comp.glsl      MODIFIED: flags, occupancy, filtered ops        (Tasks 3, 5, 10)
  brick_mark.comp.glsl     MODIFIED: conservative flags, occupancy split   (Tasks 3, 5, 10)
  brick_consolidate.comp.glsl  NEW: bake one region's bricks into overrides (Task 7)
  raymarch.comp.glsl       MODIFIED: flags word, region DDA, cost view      (Tasks 2, 3, 4)
  mesh_field.comp.glsl     MODIFIED: override bindings                      (Task 7)
  lod_field.comp.glsl      MODIFIED: override bindings                      (Task 7)
  shade.glslh              MODIFIED: BEAUTY_COST_VIEW                       (Task 2)
extension/tests/           test_brick_flags, test_op_filter, test_override_store,
                           test_octant_split                        (Tasks 3, 5, 6, 9)
tests/                     test_gpu_timings.gd (MODIFIED), test_raymarch_cost.gd,
                           test_brick_flags_gpu.gd, test_region_dda.gd,
                           test_op_filter_gpu.gd, test_consolidation.gd,
                           test_collider_octants.gd, test_occupancy_lattice.gd,
                           test_shader_reload.gd, test_self_check.gd
demo/                      benchmark.gd, edit_tool.gd, hud.gd, debug_menu.gd,
                           player.gd, main.tscn (MODIFIED); help.gd, capture.gd,
                           dev_tools.gd (NEW)
tools/                     run_benchmarks.sh, encode_capture.sh (NEW)
docs/                      PORTFOLIO.md (NEW), todo/opti.md (CLOSED OUT)
```

---

### Task 1: The measurement condition and the unattributed millisecond

M6's five legs all printed `frame=WARN`, and the footnote says Wayland forced V-Sync on despite `--disable-vsync`. A benchmark that reports the setting it *requested* is measuring the compositor. This task makes the harness report what it actually got, and closes the other hole: `custom_frame_gpu_ms` at p99 in the edit leg was 26.8 ms while every labelled pass summed to about 15 — roughly 12 ms of GPU work with no name on it. That work is the streamer (region mark, free, and the indirect brick-generation dispatch), which has never been inside a timing scope. Nothing can be optimised before it can be seen.

**Files:**
- Modify: `extension/src/render/gpu_timings.cpp` (add `"stream"` to `kPasses`; derive `unattributed_gpu_ms`)
- Modify: `extension/src/raymarch_compositor.cpp:118-124` (wrap island uploads + `WorldStreamer::run_frame` in the scope)
- Modify: `demo/benchmark.gd` (V-Sync verdict, `stream`/`unattributed` samples and budget lines)
- Create: `tools/run_benchmarks.sh`
- Test: `tests/test_gpu_timings.gd`

**Interfaces:**
- Produces: `debug_gpu_timings()` gains `stream_gpu_ms` (double, −1.0 when the scope did not run) and `unattributed_gpu_ms` (double, ≥ 0.0, −1.0 when the frame pair is missing). `BENCH` output gains `gpu_stream`, `gpu_unattributed`, and a `timing_condition` line whose `vsync_actual` is read from the display server.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_gpu_timings.gd`:

```gdscript
func test_stream_scope_is_a_known_pass()->void:
	var d: Dictionary = world().debug_ingest_gpu_timings(PackedStringArray([
		"ve:20:frame:0:b","ve:20:stream:0:b","ve:20:stream:0:e","ve:20:frame:0:e"]),
		PackedInt64Array([1000,1200,3200,9000]),60)
	assert_bool(d["valid"]).is_true()
	assert_float(d["stream_gpu_ms"]).is_equal_approx(2.0,.0001)

func test_unattributed_is_the_frame_minus_every_labelled_pass()->void:
	# frame = 8 ms, raymarch = 3 ms, stream = 2 ms -> 3 ms carries no label.
	var d: Dictionary = world().debug_ingest_gpu_timings(PackedStringArray([
		"ve:21:frame:0:b","ve:21:raymarch:0:b","ve:21:raymarch:0:e",
		"ve:21:stream:0:b","ve:21:stream:0:e","ve:21:frame:0:e"]),
		PackedInt64Array([1000,1000,4000,4000,6000,9000]),61)
	assert_float(d["unattributed_gpu_ms"]).is_equal_approx(3.0,.0001)

func test_unattributed_never_goes_negative()->void:
	# Overlapping scopes can sum past the frame; a negative "unattributed" would read as a
	# measurement, and it is not one.
	var d: Dictionary = world().debug_ingest_gpu_timings(PackedStringArray([
		"ve:22:frame:0:b","ve:22:raymarch:0:b","ve:22:raymarch:0:e","ve:22:frame:0:e"]),
		PackedInt64Array([1000,1000,9000,2000]),62)
	assert_float(d["unattributed_gpu_ms"]).is_greater_equal(0.0)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_gpu_timings.gd`
Expected: FAIL — `stream_gpu_ms` is absent (the key never appears because `known_pass("stream")` is false), and `unattributed_gpu_ms` does not exist at all.

- [ ] **Step 3: Add the pass name and the derived key**

In `extension/src/render/gpu_timings.cpp`, extend the pass list:

```cpp
const char *const kPasses[] = {
		"raymarch", "composite", "lod", "sun_shadow", "ssgi", "deferred", "inject",
		"contact", "ssr", "outlines", "history", "stream"};
```

Add the key to `empty_snapshot()` right after the loop that seeds the per-pass keys:

```cpp
	result["custom_frame_gpu_ms"] = -1.0;
	// Frame time this engine spent inside its own compositor with no pass label on it.
	// -1 means "no complete frame pair", which is a different statement from "0 ms".
	result["unattributed_gpu_ms"] = -1.0;
```

At the end of `ingest_for_test`, after `custom_frame_gpu_ms` is set and before the `valid`/`sample_id` block:

```cpp
		if (frame_complete) {
			const auto frame_pair = (*selected)["frame"][0];
			result["custom_frame_gpu_ms"] =
					static_cast<double>(frame_pair.second - frame_pair.first) / 1000.0;
			// Scopes can nest or overlap (the island cull sits inside "raymarch"), so the
			// sum is an upper bound on labelled time and the remainder is clamped at zero.
			// A negative number here would be read as a measurement; it is arithmetic.
			double labelled = 0.0;
			for (const char *name : kPasses) {
				const double value = result.get(String(name) + "_gpu_ms", -1.0);
				if (value > 0.0) labelled += value;
			}
			const double frame_ms = result["custom_frame_gpu_ms"];
			result["unattributed_gpu_ms"] = frame_ms > labelled ? frame_ms - labelled : 0.0;
		}
```

- [ ] **Step 4: Open the scope around the streamer**

In `extension/src/raymarch_compositor.cpp`, the streaming block currently reads:

```cpp
	world->drain_island_uploads(rd);
	WorldStreamer *st = world->streamer();
	if (st) st->run_frame(rd, cam.origin.x, cam.origin.y, cam.origin.z);
```

Replace it with:

```cpp
	// Everything from here to the raymarch is world maintenance: volume uploads, the region
	// mark/free passes, and the indirect brick-generation dispatch. It was the only GPU work
	// in this callback with no timing label, and in the edit leg it is the largest single
	// contributor to a frame (M6 errata 3's 26.8 ms p99). Scope it before optimising it.
	timings->begin(rd, "stream");
	world->drain_island_uploads(rd);
	WorldStreamer *st = world->streamer();
	if (st) st->run_frame(rd, cam.origin.x, cam.origin.y, cam.origin.z);
	timings->end(rd, "stream");
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `./build.sh -j$(nproc) && ./gdunit_tests.sh -c -a res://tests/test_gpu_timings.gd`
Expected: PASS, all suites.

- [ ] **Step 6: Commit**

```bash
git add extension/src/render/gpu_timings.cpp extension/src/raymarch_compositor.cpp tests/test_gpu_timings.gd
git commit -m "feat: label streaming GPU time and report the unattributed remainder"
```

- [ ] **Step 7: Make the benchmark report the V-Sync mode it actually got**

In `demo/benchmark.gd`, replace the `DisplayServer.window_set_vsync_mode(...)` call in `_ready()` with a request-then-verify pair, and store the result:

```gdscript
var _vsync_actual := "unknown"

func _record_vsync() -> void:
	# Requesting DISABLED is not the same as getting it: a Wayland compositor can refuse,
	# and every frame percentile in M6 was qualified for exactly that reason. Ask, read
	# back, and print what the run actually measured.
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	match DisplayServer.window_get_vsync_mode():
		DisplayServer.VSYNC_DISABLED: _vsync_actual = "disabled"
		DisplayServer.VSYNC_ENABLED: _vsync_actual = "enabled"
		DisplayServer.VSYNC_ADAPTIVE: _vsync_actual = "adaptive"
		DisplayServer.VSYNC_MAILBOX: _vsync_actual = "mailbox"
		_: _vsync_actual = "unknown"
```

Call `_record_vsync()` where the old `window_set_vsync_mode` call was, and replace the hard-coded `timing_condition` print in `_report()` with:

```gdscript
	var qualified := _vsync_actual != "disabled"
	print("BENCH timing_condition display_driver=%s vsync_requested=disabled vsync_actual=%s frame_verdict_qualified=%s" % [
		DisplayServer.get_name(), _vsync_actual, str(qualified).to_lower()])
	if qualified:
		push_warning("BENCH: V-Sync is %s; frame percentiles are display-capped, not engine numbers" % _vsync_actual)
```

- [ ] **Step 8: Sample and print the two new GPU keys**

In `_capture_gpu_sample()`, extend the per-pass loop and add the derived key:

```gdscript
	for key in ["raymarch", "stream", "lod", "ssgi", "ssr", "outlines", "unattributed"]:
		var value := float(d.get(key + "_gpu_ms", -1.0))
		if value >= 0.0:
			_append_gpu(key, value)
```

Add `"stream"` and `"unattributed"` to the `_gpu_samples` dictionary initialiser, and to the report loop's key list:

```gdscript
	for key in ["raymarch", "stream", "lod", "ssgi", "ssr", "shadows", "outlines",
			"unattributed", "custom_frame"]:
```

`stream` and `unattributed` get no budget row — they are attribution, not a promise — so `_budget_verdict` and `BUDGETS_MS` stay as they are.

- [ ] **Step 9: Write the benchmark driver**

Create `tools/run_benchmarks.sh`:

```bash
#!/usr/bin/env bash
# Runs every benchmark leg and files the output. One label per run:
#   tools/run_benchmarks.sh m7-baseline
# Legs are sequential on purpose — two Godot processes sharing one GPU measure each other.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:?usage: run_benchmarks.sh <label> [extra godot args...]}"
shift || true
OUT="$ROOT/reports/$LABEL"
mkdir -p "$OUT"

# x11 honours --disable-vsync; the Wayland backend on this machine does not (M6 errata 2).
# XWayland satisfies it under a Wayland session, so prefer x11 and fall back loudly.
DRIVER="x11"
if [ -z "${DISPLAY:-}" ]; then
	DRIVER="wayland"
	echo "WARNING: no DISPLAY; running on the Wayland backend. Frame percentiles will be" \
		"V-Sync-capped and every frame verdict is qualified." >&2
fi

LEGS=(--benchmark --benchmark-move --benchmark-ridge --benchmark-edit --benchmark-island)
for leg in "${LEGS[@]}"; do
	name="${leg#--benchmark}"
	name="${name#-}"
	[ -z "$name" ] && name="steady"
	echo "=== $name ($DRIVER) ==="
	/usr/bin/godot --path "$ROOT" --display-driver "$DRIVER" --resolution 2560x1440 \
		--disable-vsync "$@" demo/main.tscn -- "$leg" 2>&1 | tee "$OUT/$name.txt"
	echo "EXIT_STATUS=${PIPESTATUS[0]}" | tee -a "$OUT/$name.txt"
done

echo
echo "=== verdicts ($LABEL) ==="
grep -h "BENCH budget_verdict\|BENCH timing_condition" "$OUT"/*.txt
```

Then: `chmod +x tools/run_benchmarks.sh`.

- [ ] **Step 10: Record the baseline**

Run: `tools/run_benchmarks.sh m7-baseline`
Expected: five legs, each `EXIT_STATUS=0`. Copy every `BENCH gpu_*`, `BENCH budget_verdict` and `BENCH timing_condition` line into **Errata entry 1** of this plan, labelled "M7 baseline". Every later task's measured delta is quoted against this entry; without it, no optimisation in this plan can claim anything.

- [ ] **Step 11: Commit**

```bash
git add demo/benchmark.gd tools/run_benchmarks.sh docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md
git commit -m "feat: benchmark reports the vsync it got and the GPU time nothing labelled"
```

---

### Task 2: The cost view — where the march actually spends its steps

The raymarch is 6.3 ms at p50 and 8.0 at p99 against a 6 ms budget, and 10.3/14.2 while editing. Before changing the traversal, the plan needs a picture of *which pixels* are expensive: a march that is slow on the sky is a DDA problem, one that is slow on grazing ground is a step-count problem, and one that is slow only during edits is a field-complexity problem. The heat view answers that in one screenshot, and the probe makes the same number assertable in a test — so Tasks 3 and 4 can prove they moved it.

**Files:**
- Modify: `extension/src/shade/beauty_settings.h`, `extension/src/shade/beauty_settings.cpp` (`kFlagCostView`, the `cost_view` toggle)
- Modify: `shaders/shade.glslh` (`BEAUTY_COST_VIEW`)
- Modify: `shaders/raymarch.comp.glsl` (count steps, emit heat, write the cost buffer)
- Modify: `extension/src/render/raymarch_pass.h`, `raymarch_pass.cpp` (binding 23, `cost_buffer()`)
- Modify: `extension/src/voxel_world.h`, `voxel_world.cpp` (`render_probe_pixel`, `debug_raymarch_cost_probe`)
- Test: `extension/tests/test_beauty_settings.cpp`, `tests/test_raymarch_cost.gd`

**Interfaces:**
- Consumes: `ve::BeautySettings` and its flag packing (M6 Task 3); `VoxelWorld::debug_raymarch_pixel` (M1/M2) as the shape to copy for a 1×1 probe.
- Produces: `ve::kFlagCostView = 128u`; `VoxelWorld::set_effect_enabled("cost_view", bool)`; `VoxelWorld::debug_raymarch_cost_probe(Vector3 pos, Vector3 fwd) -> Dictionary` with keys `steps` (int, marching steps the primary ray consumed), `bricks` (int, brick DDA cells visited), `regions` (int, region DDA cells visited — 0 until Task 4), `hit` (bool).

- [ ] **Step 1: Write the failing native test**

Append to `extension/tests/test_beauty_settings.cpp`:

```cpp
TEST_CASE("cost view is a flag, off in every quality tier") {
	// It is a debug view, not a quality level: switching to High must never turn the screen
	// into a heat map, and switching to Off must not be the only way to leave it.
	for (const ve::QualityTier tier : {ve::QualityTier::kOff, ve::QualityTier::kLow,
			ve::QualityTier::kMedium, ve::QualityTier::kHigh}) {
		const ve::BeautySettings s = ve::settings_for_tier(tier);
		CHECK((ve::pack_beauty_flags(s) & ve::kFlagCostView) == 0u);
	}
	ve::BeautySettings s = ve::settings_for_tier(ve::QualityTier::kHigh);
	s.cost_view = true;
	CHECK((ve::pack_beauty_flags(s) & ve::kFlagCostView) == ve::kFlagCostView);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `no member named 'cost_view'` / `kFlagCostView` undeclared.

- [ ] **Step 3: Add the flag**

In `extension/src/shade/beauty_settings.h`, after `kFlagRaySunShadow`:

```cpp
// A debug view, not an effect: it replaces the albedo channel with marching cost so the
// budget conversation can be about pixels instead of averages. It is never set by a tier.
inline constexpr uint32_t kFlagCostView = 128u;
```

Add `bool cost_view = false;` to `ve::BeautySettings`, pack it in `pack_beauty_flags`, and add `"cost_view"` to the name table `set_effect_enabled`/`get_effect_enabled` resolve against (the same table `debug_beauty_settings()` reports through, so the debug menu picks it up for free once Task 10 lists it).

In `shaders/shade.glslh`, mirror the constant next to the others:

```glsl
const uint BEAUTY_COST_VIEW = 128u;
```

- [ ] **Step 4: Run the native test to verify it passes**

Run: `cd extension && scons test`
Expected: PASS (all native suites).

- [ ] **Step 5: Count the steps in the marcher**

`march_terrain` already threads a `steps_left` budget through every loop, so the cost is `65536 - steps_left` with no new plumbing in the inner loops. In `shaders/raymarch.comp.glsl`, add two globals next to the other counters and increment them where the DDA advances:

```glsl
// Diagnostic counters. They are plain globals rather than an inout parameter chain because
// every function that could touch them already takes `steps_left`, and a second inout on
// three call sites buys nothing. The compiler drops them when BEAUTY_COST_VIEW is unset in
// the flags word only at runtime, not at compile time -- two counters of ALU is the price
// of being able to see the march at all.
uint g_brick_cells = 0u;
uint g_region_cells = 0u;
```

In `march_terrain`'s outer DDA loop body, immediately after `float t_exit = ...`, add `g_brick_cells++;`.

In `main()`, after the primary march, turn the counters into a colour when the flag is set:

```glsl
	if ((flags & BEAUTY_COST_VIEW) != 0u) {
		// One heat unit per marching step, black at 0 and white at 512, so a pixel that
		// burns half the shadow budget is unmistakable next to one that does not.
		float heat = clamp(float(65536 - primary_steps) / 512.0, 0.0, 1.0);
		// Blue -> green -> red, which reads as "cheap -> expensive" at a glance and keeps
		// the sky (0 steps) black rather than a colour the eye reads as terrain.
		vec3 hc = heat < 0.5 ? mix(vec3(0.0, 0.1, 0.6), vec3(0.1, 0.8, 0.2), heat * 2.0)
		                     : mix(vec3(0.1, 0.8, 0.2), vec3(0.9, 0.1, 0.05), heat * 2.0 - 1.0);
		albedo = hc;
		// Everything else in the G-buffer stays truthful: depth, normal, material and sun
		// visibility are written exactly as they would be, so the depth test, the outlines
		// and the LoD seam behave normally and the view can be toggled mid-flight.
	}
```

(`albedo` is the local the existing code writes into `out_albedo`; set it just before that store.)

- [ ] **Step 6: Write the failing gdUnit test**

Create `tests/test_raymarch_cost.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

# The cost probe is the instrument Tasks 3 and 4 are measured with, so it has to mean
# something before they run: a ray fired at the ground must cost more than one fired at the
# sky, and neither may report zero work when the marcher ran.
func test_ground_ray_costs_more_than_sky_ray() -> void:
	var w := make_world()
	var eye := Vector3(24.0, 62.0, 24.0)
	var down: Dictionary = w.debug_raymarch_cost_probe(eye, Vector3(0.2, -1.0, 0.2).normalized())
	var up: Dictionary = w.debug_raymarch_cost_probe(eye, Vector3(0.0, 1.0, 0.0))
	assert_bool(down["hit"]).is_true()
	assert_bool(up["hit"]).is_false()
	assert_int(down["steps"]).is_greater(0)
	assert_int(down["steps"]).is_greater(int(up["steps"]))

func test_probe_reports_brick_cells_visited() -> void:
	var w := make_world()
	# A near-horizontal ray crosses many brick cells before it finds anything; that count is
	# what the region DDA in Task 4 is supposed to collapse.
	var d: Dictionary = w.debug_raymarch_cost_probe(
		Vector3(24.0, 70.0, 24.0), Vector3(1.0, -0.02, 0.0).normalized())
	assert_int(d["bricks"]).is_greater(20)
```

- [ ] **Step 7: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_raymarch_cost.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_raymarch_cost_probe'`.

- [ ] **Step 8: Add the probe**

`out_hitpos.w` is **not** available for this: `composite.frag.glsl:25-31` samples that target with `texture()` (bilinear) and thresholds `hp.w < 0.5`, so any magnitude larger than one would corrupt the near-field coverage test. The counters get their own buffer instead.

In `shaders/raymarch.comp.glsl`, add the output next to the other bindings:

```glsl
// Two words per pixel: [0] steps consumed by the primary ray, [1] brick cells in the low
// 16 bits and region cells in the high 16. Written every frame -- one store per pixel is
// below the noise floor of a pass that reads the atlas thousands of times -- so the probe
// never needs a special dispatch path that could drift from the real one.
layout(set = 0, binding = 23, std430) writeonly buffer CostOut { uint v[]; } cost_out;
```

and at the end of `main()`:

```glsl
	int cost_i = (px.y * size.x + px.x) * 2;
	cost_out.v[cost_i + 0] = uint(65536 - primary_steps);
	cost_out.v[cost_i + 1] = (g_brick_cells & 0xFFFFu) | (min(g_region_cells, 0xFFFFu) << 16);
```

In `RaymarchPass::rebuild_targets`, allocate it with the targets and add it to the uniform set (it is freed and re-created with them, so a resize can never leave a short buffer behind):

```cpp
	if (cost_buf_.is_valid()) rd->free_rid(cost_buf_);
	cost_buf_ = rd->storage_buffer_create(static_cast<uint32_t>(w) * h * 2u * sizeof(uint32_t));
	...
	u[23]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u[23]->set_binding(23); u[23]->add_id(cost_buf_);
```

Expose it as `RID cost_buffer() const { return cost_buf_; }`, and in `voxel_world.cpp`:

```cpp
Dictionary VoxelWorld::debug_raymarch_cost_probe(Vector3 pos, Vector3 fwd) {
	Dictionary out;
	out["hit"] = false;
	out["steps"] = 0;
	out["bricks"] = 0;
	out["regions"] = 0;
	ensure_initialized();
	if (!initialized_) return out; // fail-soft: a probe on a dead world reports nothing
	// The same 1x1 render debug_raymarch_pixel performs; factor its body into
	// render_probe_pixel(pos, fwd) and call it from both, so there is exactly one probe
	// path and it cannot drift from RaymarchPass::render.
	if (!render_probe_pixel(pos, fwd)) return out;
	const PackedByteArray words = rd()->buffer_get_data(raymarch_pass_->cost_buffer(), 0, 8);
	if (words.size() < 8) return out;
	const uint32_t steps = words.decode_u32(0);
	const uint32_t cells = words.decode_u32(4);
	const PackedFloat32Array texel = debug_raymarch_pixel(pos, fwd); // hit flag, unchanged
	out["hit"] = texel.size() >= 4 && texel[3] > 0.5f;
	out["steps"] = static_cast<int>(steps);
	out["bricks"] = static_cast<int>(cells & 0xFFFFu);
	out["regions"] = static_cast<int>(cells >> 16);
	return out;
}
```

- [ ] **Step 9: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc) && ./gdunit_tests.sh -c -a res://tests/test_raymarch_cost.gd && ./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd`
Expected: PASS for both.

- [ ] **Step 10: Attribute the raymarch with an A/B run**

The cost view says *where*; these two runs say *what*. With the demo running at 1440p:

```bash
# 1. Baseline, then the same leg with the raymarched sun shadow off. The difference is what
#    shadow layer 1 costs; it is one ray per pixel with a 96-step budget, so it is a
#    candidate for the largest single share of the march.
/usr/bin/godot --path . --display-driver x11 --resolution 2560x1440 --disable-vsync \
	demo/main.tscn -- --benchmark | tee /tmp/m7-attr-full.txt
```

Then, in the running demo (F1 → uncheck **Raymarched sun shadow**), capture `BENCH gpu_raymarch` from a second `--benchmark` run started with the toggle already off — add a temporary `--effects-off=raymarched_sun_shadow` argument to `benchmark.gd`'s `_ready()` that calls `_world.set_effect_enabled(name, false)` for each comma-separated name, and keep it: Task 13 uses it again for the quality/cost table.

Record in **Errata entry 2**: the p50/p99 with and without the sun ray, with and without islands (`--effects-off` accepts several names), and one screenshot of the cost view over the steady-leg camera (`F3`, then `F12`). State in one sentence which of the three hypotheses the data supports: *DDA overhead* (heat spread evenly across ground and sky), *shadow rays* (heat concentrated in lit ground), or *field complexity* (heat concentrated near edits).

- [ ] **Step 11: Commit**

```bash
git add extension/src/shade/beauty_settings.h extension/src/shade/beauty_settings.cpp \
	extension/tests/test_beauty_settings.cpp shaders/shade.glslh shaders/raymarch.comp.glsl \
	extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_raymarch_cost.gd \
	demo/benchmark.gd docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md
git commit -m "feat: raymarch cost view and step-count probe"
```

---

### Task 3: One word per brick instead of eight texel fetches

`brick_may_have_surface(slot)` reads **eight** texels of the 2³ mip level and reduces them, and the caller then reads `palette_buf.ids[slot * 4]` to find out whether the brick has any material. That is nine memory operations to answer two yes/no questions, and it runs on **every brick the DDA enters**, for every primary ray, before any sphere tracing happens. The answers never change between regenerations, so they belong in one word written once by the generator.

The subtle part is the stale-slot window: `brick_mark.comp.glsl` can assign a slot and then have its generation job dropped (job-list overflow, `frame.overflow` bit 1), leaving the atlas holding the *previous* brick's bytes for a frame. Today that costs a frame of stale voxels. If the flag word were also stale it could say "no surface" for a brick that has one, and the marcher would shoot straight through the ground. So the allocating pass writes the **conservative** value (both bits set, "march it"), and the generator overwrites it with the truth.

**Files:**
- Create: `extension/src/world/brick_flags.h`, `extension/src/world/brick_flags.cpp`
- Create: `extension/tests/test_brick_flags.cpp`
- Modify: `extension/src/render/gpu_atlas.h`, `gpu_atlas.cpp` (the buffer + a reset on clear)
- Modify: `shaders/brick_gen.comp.glsl`, `shaders/brick_mark.comp.glsl`, `shaders/common.glslh`
- Modify: `shaders/raymarch.comp.glsl`, `extension/src/render/raymarch_pass.cpp` (binding 21)
- Modify: `extension/src/render/region_pass.cpp`, `brick_gen_pass.cpp` (bind the new buffer)
- Test: `tests/test_brick_flags_gpu.gd`

**Interfaces:**
- Consumes: `ve::BrickMips` and `ve::mip_cell_has_surface` (M2 Task 4); `ve::palette_occupancy_order` (M2 Task 5).
- Produces:
  - `ve::kBrickFlagHasSurface = 1u`, `ve::kBrickFlagHasMaterial = 2u`, `ve::kBrickFlagConservative = 3u`
  - `uint32_t ve::brick_flags_from_mips(const ve::BrickMips &mips, uint16_t palette_slot0)`
  - `bool ve::brick_flag_has_surface(uint32_t)`, `bool ve::brick_flag_has_material(uint32_t)`
  - `RID GpuAtlas::brick_flags()` — one `uint` per atlas slot; raymarch **binding 21**.

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_brick_flags.cpp`:

```cpp
#include "doctest.h"
#include "world/brick_flags.h"
#include "world/brick_mip.h"

namespace {

// A mip chain whose whole 2^3 level sits on one side of zero: no surface anywhere.
ve::BrickMips uniform_mips(uint8_t value) {
	ve::BrickMips m;
	for (int i = 0; i < 8; i++) { m.mn2[i] = value; m.mx2[i] = value; }
	for (int i = 0; i < 64; i++) { m.mn4[i] = value; m.mx4[i] = value; }
	for (int i = 0; i < 512; i++) { m.mn8[i] = value; m.mx8[i] = value; }
	return m;
}

} // namespace

TEST_CASE("a brick straddling zero anywhere reports a surface") {
	ve::BrickMips m = uniform_mips(200); // all air
	CHECK(ve::brick_flags_from_mips(m, 1) == 0u + ve::kBrickFlagHasMaterial);
	// One cell of the 2^3 level crossing zero is enough: the marcher must enter the brick.
	m.mn2[5] = 10;
	m.mx2[5] = 250;
	CHECK(ve::brick_flag_has_surface(ve::brick_flags_from_mips(m, 1)));
}

TEST_CASE("an empty palette means no material even when the field crosses zero") {
	// Palette slot 0 holding id 0 is how a brick says "nothing here has a material"; the
	// marcher's hit test refuses such a brick, and the flag has to agree or the DDA would
	// enter it, sphere-trace it, and reject the hit after paying for all of it.
	ve::BrickMips m = uniform_mips(128);
	const uint32_t flags = ve::brick_flags_from_mips(m, 0);
	CHECK(ve::brick_flag_has_surface(flags));
	CHECK_FALSE(ve::brick_flag_has_material(flags));
}

TEST_CASE("the conservative value marches everything") {
	CHECK(ve::brick_flag_has_surface(ve::kBrickFlagConservative));
	CHECK(ve::brick_flag_has_material(ve::kBrickFlagConservative));
}

TEST_CASE("the flag agrees with the eight-cell reduction it replaces") {
	// The property that makes the swap safe: for every mip chain, the flag is exactly what
	// the marcher's old reduce-over-eight-cells test computed.
	ve::BrickMips m = uniform_mips(128);
	for (int trial = 0; trial < 8; trial++) {
		m.mn2[trial] = static_cast<uint8_t>(trial * 30);
		m.mx2[trial] = static_cast<uint8_t>(255 - trial * 20);
		uint8_t mn = 255, mx = 0;
		for (int i = 0; i < 8; i++) {
			mn = mn < m.mn2[i] ? mn : m.mn2[i];
			mx = mx > m.mx2[i] ? mx : m.mx2[i];
		}
		const bool old_test = ve::mip_cell_has_surface(mn, mx);
		CHECK(ve::brick_flag_has_surface(ve::brick_flags_from_mips(m, 1)) == old_test);
	}
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `world/brick_flags.h: No such file or directory`.

- [ ] **Step 3: Write the pure core**

Create `extension/src/world/brick_flags.h`:

```cpp
#pragma once
#include "world/brick_mip.h"
#include <cstdint>

namespace ve {

// One word per resident brick, answering the two questions the marcher's brick DDA asks
// before it commits to sphere tracing. Both were previously computed per DDA step from nine
// separate memory reads (eight mip texels plus the palette's slot 0).
inline constexpr uint32_t kBrickFlagHasSurface = 1u;
inline constexpr uint32_t kBrickFlagHasMaterial = 2u;
// What an allocating pass writes before the generator has run. A brick whose generation job
// was dropped keeps the previous occupant's atlas bytes for a frame; saying "march it"
// costs a wasted traversal, saying "skip it" would put a hole in the ground.
inline constexpr uint32_t kBrickFlagConservative = kBrickFlagHasSurface | kBrickFlagHasMaterial;

uint32_t brick_flags_from_mips(const BrickMips &mips, uint16_t palette_slot0);

inline bool brick_flag_has_surface(uint32_t f) { return (f & kBrickFlagHasSurface) != 0u; }
inline bool brick_flag_has_material(uint32_t f) { return (f & kBrickFlagHasMaterial) != 0u; }

} // namespace ve
```

`extension/src/world/brick_flags.cpp`:

```cpp
#include "world/brick_flags.h"

namespace ve {

uint32_t brick_flags_from_mips(const BrickMips &mips, uint16_t palette_slot0) {
	// The 2^3 level is the whole brick in eight cells; reducing it is exactly the test the
	// marcher used to run inline. Inclusive min/max over trilinear corners means this can
	// never hide a crossing (see brick_mip.h).
	uint8_t mn = 255, mx = 0;
	for (int i = 0; i < 8; i++) {
		if (mips.mn2[i] < mn) mn = mips.mn2[i];
		if (mips.mx2[i] > mx) mx = mips.mx2[i];
	}
	uint32_t flags = 0u;
	if (mip_cell_has_surface(mn, mx)) flags |= kBrickFlagHasSurface;
	// palette_occupancy_order puts the dominant material in slot 0, and id 0 means "no
	// material" -- a brick generated entirely out of air that still straddles zero in its
	// apron plane, which the marcher must not report as a hit.
	if (palette_slot0 != 0) flags |= kBrickFlagHasMaterial;
	return flags;
}

} // namespace ve
```

- [ ] **Step 4: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS — every native suite, `test_brick_flags` included.

- [ ] **Step 5: Commit the pure core**

```bash
git add extension/src/world/brick_flags.h extension/src/world/brick_flags.cpp extension/tests/test_brick_flags.cpp
git commit -m "feat: per-brick flag word, pure core"
```

- [ ] **Step 6: Write the failing GPU test**

Create `tests/test_brick_flags_gpu.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

# The flag word has to agree with the mip chain it summarises for every generated brick, or
# the marcher skips ground that is there. debug_brick_flags returns, per resident brick of
# one region: the flag word the GPU wrote, and the word ve::brick_flags_from_mips computes
# from the CPU reference brick. They must match everywhere.
func test_gpu_flags_match_the_cpu_reference(timeout := 30000) -> void:
	var w := make_world()
	w.debug_stream_region(Vector3i(1, 2, 1))
	var d: Dictionary = w.debug_brick_flags(Vector3i(1, 2, 1))
	assert_int(int(d["compared"])).is_greater(50)
	assert_int(int(d["mismatches"])).is_equal(0)

# A brick allocated but not yet generated must read as "march it": the conservative value is
# what stops a dropped generation job from becoming a hole.
func test_allocated_but_ungenerated_bricks_are_conservative() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_brick_flags_after_mark(Vector3i(1, 2, 1))
	assert_int(int(d["allocated"])).is_greater(0)
	assert_int(int(d["non_conservative"])).is_equal(0)
```

- [ ] **Step 7: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_brick_flags_gpu.gd`
Expected: FAIL — `Nonexistent function 'debug_brick_flags'`.

- [ ] **Step 8: Add the buffer**

In `GpuAtlas`: create `brick_flags_` as a storage buffer of `atlas_slot_count() * sizeof(uint32_t)` bytes next to the other pool buffers, expose `RID brick_flags() const`, and clear it to `kBrickFlagConservative` at creation (`buffer_clear` writes zeros, so upload a filled `PackedByteArray` once instead — a zero word would mean "skip this brick" for every slot that has never been generated).

Bind it in the two passes that write it and the one that reads it:
- `brick_mark.comp.glsl`: `layout(set = 0, binding = 10, std430) buffer BrickFlags { uint v[]; } brick_flags;` — the next free binding in that pass's set; add the matching `RDUniform` in `RegionPass`.
- `brick_gen.comp.glsl`: `layout(set = 0, binding = 10, std430) writeonly buffer BrickFlags { uint v[]; } brick_flags;` — likewise in `BrickGenPass`.
- `raymarch.comp.glsl`: `layout(set = 0, binding = 21, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;` — extend `RaymarchPass::rebuild_targets`'s uniform array from 21 entries to 24 (21 flags, 22 region slot counts for Task 4, 23 the cost buffer from Task 2).

In `shaders/common.glslh`, mirror the constants so no shader spells the bit values:

```glsl
const uint BRICK_FLAG_HAS_SURFACE = 1u;
const uint BRICK_FLAG_HAS_MATERIAL = 2u;
const uint BRICK_FLAG_CONSERVATIVE = 3u;
```

- [ ] **Step 9: Write the flags from the two passes**

In `brick_mark.comp.glsl`, in the allocate phase, immediately after a slot is taken from the free list (and also on the `force_regen` path, where the slot is reused):

```glsl
		slot = free_list.slot[old - 1];
		region_tables.slot[idx] = slot;
		atomicAdd(region_counts.n[rslot], 1);
		// Until brick_gen runs, this slot's atlas bytes belong to whoever had it last.
		// Conservative means the marcher enters the brick and sphere-traces it: one wasted
		// traversal in a rare frame, instead of a hole a player can fall through.
		brick_flags.v[slot] = BRICK_FLAG_CONSERVATIVE;
```

In `brick_gen.comp.glsl`, in the final reduction phase (the `b2` loop over the eight 2³ cells), have thread 0 publish the summary once the shared level below it is complete. `s_mip4` is already reduced and barriered at that point, so the whole-brick range is one reduce over its 64 entries — no extra image reads:

```glsl
	if (tid == 0u) {
		uint mn = 255u, mx = 0u;
		for (int i = 0; i < 64; i++) {
			mn = min(mn, s_mip4[i] >> 8);
			mx = max(mx, s_mip4[i] & 255u);
		}
		// Mirror of ve::brick_flags_from_mips: the same inclusive straddle test, and the
		// same "palette slot 0 is id 0 means no material" rule the marcher's hit test uses.
		uint f = 0u;
		if (mn <= ENCODED_ZERO && mx >= ENCODED_ZERO) f |= BRICK_FLAG_HAS_SURFACE;
		if (palette_buf.id[slot * 4] != 0u) f |= BRICK_FLAG_HAS_MATERIAL;
		brick_flags.v[slot] = f;
	}
```

Place it **after** the palette has been written and barriered (phase 3), and after the `s_mip4` barrier — check the existing barrier order in the file and put the block at the very end of `main()` with a `barrier()` in front of it if the last write before it is not already fenced.

- [ ] **Step 10: Read one word in the marcher**

In `shaders/raymarch.comp.glsl`, replace `brick_may_have_surface` and its call site:

```glsl
// Spec §3's brick-level gate, in one load. The nine reads it replaces (eight 2^3 mip texels
// plus the palette's slot 0) are now done once per generation instead of once per DDA step.
bool brick_may_have_surface(int slot) {
	return (brick_flags.v[slot] & BRICK_FLAG_HAS_SURFACE) != 0u;
}
```

and in `march_terrain`:

```glsl
		int slot = slot_at(map);
		if (slot >= 0) {
			uint bf = brick_flags.v[slot];
			if ((bf & BRICK_FLAG_HAS_SURFACE) == 0u) { /* fall through to the DDA step */ }
			else {
				bool has_material = (bf & BRICK_FLAG_HAS_MATERIAL) != 0u;
				... // the existing in-brick loop, unchanged
			}
		}
```

Keep the body identical apart from where `has_material` comes from. Do **not** delete the `mip2_atlas` binding: `island_extract` and the CPU differential path still read the chain, and Task 4 leaves the 8³ level in the inner loop exactly where it is.

- [ ] **Step 11: Add the two debug hooks**

`VoxelWorld::debug_brick_flags(Vector3i region)` streams the region (reusing the existing `debug_stream_region` path), reads back the region's brick table plus the flag buffer, rebuilds each resident brick on the CPU with `ve::eval_brick` + `ve::build_brick_mips` + `ve::brick_flags_from_mips`, and returns `{compared, mismatches, first_mismatch_brick}`. `debug_brick_flags_after_mark(Vector3i region)` runs only the mark pass (no generation dispatch) and returns `{allocated, non_conservative}`. Both follow the shape of the existing `debug_mesh_diff` / `debug_brick_diff` hooks — same "run it, read it back, compare against the pure reference" pattern, same fail-soft empty dictionary when the world is not initialised.

- [ ] **Step 12: Run every test that reads the atlas**

Run:
```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_brick_flags_gpu.gd
./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd
./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd
./gdunit_tests.sh -c -a res://tests/test_raymarch_mips.gd
./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd
```
Expected: PASS for all five. `test_raymarch_pixel` and `test_edit_pipeline` are the oracles here: they compare rendered hits against `ve::raycast` on the analytic field, so a flag that wrongly skips a brick shows up as a missed hit, not as a slower frame.

- [ ] **Step 13: Measure**

Run: `tools/run_benchmarks.sh m7-task3`
Record in **Errata entry 3**: `gpu_raymarch` p50/p99 for all five legs, against the Task 1 baseline. Expected direction: the steady and ridge legs improve most (long views cross the most bricks). If the measured p50 delta is **under 3 %** on every leg, revert the marcher change (keep the flag buffer — Task 4 uses it), and record the null result with the numbers.

- [ ] **Step 14: Commit**

```bash
git add extension/src/render/gpu_atlas.h extension/src/render/gpu_atlas.cpp \
	extension/src/render/region_pass.cpp extension/src/render/brick_gen_pass.cpp \
	extension/src/render/raymarch_pass.cpp extension/src/voxel_world.h extension/src/voxel_world.cpp \
	shaders/common.glslh shaders/brick_mark.comp.glsl shaders/brick_gen.comp.glsl \
	shaders/raymarch.comp.glsl tests/test_brick_flags_gpu.gd \
	docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md
git commit -m "perf: one flag word per brick replaces nine reads per DDA step"
```

---

### Task 4: The region DDA spec §3 always described

Spec §3's traversal has three levels: "Region DDA (25.6 m) → brick DDA (0.8 m) via indirection → in-brick sphere tracing". This engine has two. `march_terrain` starts its DDA at brick granularity and walks 0.8 m cells all the way to `max_dist` (200 m), consulting the region map for each one — so a ray pointed at the sky pays 250 indirection lookups to learn 250 times that there is nothing there, and a ray crossing the resident radius pays the same to cross empty air *inside* a resident region. The far field is drawn by the LoD raster; the near field marching past it is pure overhead.

Two skips close it, and both use data that already exists: `region_map.slot[...] < 0` means the region has no brick table at all, and `region_slot_counts.n[rs] == 0` means the region has a table but not one resident brick in it. Either way the whole 25.6 m cell is empty and the ray can jump to its far face.

**Files:**
- Modify: `shaders/raymarch.comp.glsl` (`march_terrain`, `terrain_sun_visibility`)
- Modify: `extension/src/render/raymarch_pass.cpp` (binding 22 = `atlas.region_slot_counts()`)
- Modify: `extension/src/voxel_world.cpp` (`debug_raymarch_cost_probe` reports `regions`)
- Test: `tests/test_region_dda.gd`

**Interfaces:**
- Consumes: `GpuAtlas::region_slot_counts()` (M2 Task 12 — `int` per region slot, maintained by the mark and free passes); `ve::brick_flag_has_surface` from Task 3.
- Produces: no new C++ API. `debug_raymarch_cost_probe()["regions"]` becomes non-zero.

- [ ] **Step 1: Write the failing test**

Create `tests/test_region_dda.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

# A ray fired straight up crosses ~200 m of nothing. With a brick DDA that is 250 cells;
# with the region DDA spec §3 asks for it is at most a handful. The exact number is not the
# point -- the ORDER is, so the assertion is written against the brick count it replaces.
func test_sky_ray_walks_regions_not_bricks() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_raymarch_cost_probe(Vector3(24.0, 70.0, 24.0), Vector3(0, 1, 0))
	assert_bool(d["hit"]).is_false()
	assert_int(int(d["bricks"])).is_less(32)
	assert_int(int(d["regions"])).is_greater(0)

# The skip must never cost a hit. This is the same oracle test_raymarch_pixel.gd uses: the
# analytic raycast is the truth, and the marcher has to agree with it to a few centimetres.
func test_ground_hits_still_match_the_analytic_raycast() -> void:
	var w := make_world()
	for i in range(12):
		var a := float(i) * 0.5
		var dir := Vector3(sin(a) * 0.6, -1.0, cos(a) * 0.6).normalized()
		var eye := Vector3(24.0, 70.0, 24.0)
		var probe: Dictionary = w.debug_raymarch_cost_probe(eye, dir)
		var truth: Dictionary = w.debug_raycast(eye, dir)
		assert_bool(probe["hit"]).is_equal(truth["hit"])

# A ray that starts inside a resident region and leaves the residency radius must not report
# a hit from a region it never had data for.
func test_ray_leaving_residency_finds_nothing_rather_than_something() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_raymarch_cost_probe(
		Vector3(24.0, 62.0, 24.0), Vector3(1.0, 0.02, 0.0).normalized())
	var truth: Dictionary = w.debug_raycast(
		Vector3(24.0, 62.0, 24.0), Vector3(1.0, 0.02, 0.0).normalized())
	if not truth["hit"]:
		assert_bool(d["hit"]).is_false()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_region_dda.gd`
Expected: FAIL — `bricks` for the sky ray is ~250, and `regions` is 0.

- [ ] **Step 3: Bind the region slot counts**

In `RaymarchPass::rebuild_targets`, add the buffer at binding 22 (the uniform array is already 24 entries after Task 3):

```cpp
	u[22]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u[22]->set_binding(22); u[22]->add_id(atlas.region_slot_counts());
```

and in `shaders/raymarch.comp.glsl`:

```glsl
// How many atlas bricks each REGION SLOT currently holds, maintained by brick_mark and the
// free pass. Zero means the region is resident but empty -- sky above the terrain, the
// inside of a hollowed hill -- and the whole 25.6 m cell can be crossed in one step.
layout(set = 0, binding = 22, std430) readonly buffer RegionSlotCounts { int n[]; } region_counts;
```

- [ ] **Step 4: Write the two-level traversal**

Restructure `march_terrain` so the outer loop steps regions and the inner loop steps bricks within the current region's span. Replace the function body's loop with:

```glsl
	// Level 1: the region DDA. Cells are 25.6 m; a cell with no table (never resident) or
	// no bricks (resident but empty) is crossed in one step instead of 32 brick steps.
	ivec3 rmap = ivec3(floor(ro / REGION_SIZE));
	vec3 rdelta = abs(vec3(REGION_SIZE) / rd);
	ivec3 rst = ivec3(sign(rd));
	vec3 rside = (vec3(rmap) * REGION_SIZE - ro + (vec3(rst) * 0.5 + 0.5) * REGION_SIZE) / rd;
	if (rst.x == 0) rside.x = 1.0 / 0.0;
	if (rst.y == 0) rside.y = 1.0 / 0.0;
	if (rst.z == 0) rside.z = 1.0 / 0.0;
	float rt_prev = 0.0;

	for (int r = 0; r < 64; r++) {
		float rt_exit = min(rside.x, min(rside.y, rside.z));
		if (rt_prev > max_dist) break;
		g_region_cells++;

		// region_slot_of takes a BRICK coord; the region's own coord is rmap, so pick any
		// brick inside it. Shifting left by 5 is the inverse of the `>> 5` inside.
		int rs = region_slot_of(rmap << 5);
		bool region_worth_entering = rs >= 0 && region_counts.n[rs] > 0;

		if (region_worth_entering) {
			// Level 2: the brick DDA, bounded to this region's span. Unchanged from the
			// code it replaces except that it stops at rt_exit instead of max_dist.
			float seg_end = min(rt_exit, max_dist);
			Hit h = march_bricks(ro, rd, max(rt_prev, 0.0), seg_end, steps_left);
			if (h.hit) return h;
			if (steps_left <= 0) return h;
		}

		if (rside.x < rside.y && rside.x < rside.z) { rt_prev = rside.x; rside.x += rdelta.x; rmap.x += rst.x; }
		else if (rside.y < rside.z)                 { rt_prev = rside.y; rside.y += rdelta.y; rmap.y += rst.y; }
		else                                        { rt_prev = rside.z; rside.z += rdelta.z; rmap.z += rst.z; }
	}
	return h; // the miss record built at the top of the function
```

`march_bricks(ro, rd, t_begin, t_end, steps_left)` is today's `march_terrain` body, moved verbatim into its own function with two changes: its DDA is seeded from `ro + rd * t_begin` instead of `ro`, and its exit test compares against `t_end` instead of `max_dist`. **Seed it from the segment start, not from `ro`** — re-deriving `map` from the origin each region would re-walk the bricks already crossed, which is the cost this task exists to remove.

The 64-iteration cap on the region loop is 64 × 25.6 m = 1638 m, comfortably past the 200 m `max_dist`, and it is a loop bound rather than a distance test so a degenerate direction cannot spin.

- [ ] **Step 5: Give the shadow ray the same skip**

`terrain_sun_visibility` already returns `1.0` the moment its sample lands in a region with no slot. Add the empty-region case, which the sun ray hits constantly (the sun points up; every ray leaves the ground almost immediately):

```glsl
		int shadow_region = region_slot_of(brick);
		if (shadow_region < 0) return 1.0;
		// Resident but empty: nothing in this 25.6 m cell can shadow anything. Jump to its
		// far face rather than stepping 0.64 m at a time through known-empty air.
		if (region_counts.n[shadow_region] == 0) {
			vec3 rlo = floor(q / REGION_SIZE) * REGION_SIZE;
			vec3 rhi = rlo + vec3(REGION_SIZE);
			vec3 far = mix(rlo, rhi, step(0.0, SUN_DIR));
			vec3 tf = (far - q) / SUN_DIR;
			float skip = min(tf.x, min(tf.y, tf.z));
			t += max(skip, 0.01) + 0.001;
			continue;
		}
```

Guard the division: `SUN_DIR` is a compile-time constant with no zero component (`normalize(vec3(0.6, 0.8, 0.3))`), so no `1.0/0.0` guard is needed here — say so in a comment, because the primary DDA above does need them and the asymmetry will otherwise read as an oversight.

- [ ] **Step 6: Report region cells from the probe**

The cost buffer's second word already carries `g_region_cells` in its high 16 bits (Task 2, Step 8), and `debug_raymarch_cost_probe` already unpacks it. Nothing to write; confirm the value is non-zero by running the test.

- [ ] **Step 7: Run the tests to verify they pass**

Run:
```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_region_dda.gd
./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd
./gdunit_tests.sh -c -a res://tests/test_raymarch_mips.gd
./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd
./gdunit_tests.sh -c -a res://tests/test_island_render.gd
./gdunit_tests.sh -c -a res://tests/test_lod_seam.gd
```
Expected: PASS for all six. The last two matter as much as the first: islands are marched by a separate function that must still see the same terrain hit to win or lose against, and the seam test is what notices if the near field stops reaching as far as it used to.

- [ ] **Step 8: Measure**

Run: `tools/run_benchmarks.sh m7-task4`
Record in **Errata entry 4**: `gpu_raymarch` p50/p99 across all five legs against Task 3's numbers, plus a cost-view screenshot of the steady leg for the before/after pair. If the delta is under 3 % on every leg, revert and record — but note in the entry what the probe says about `regions` vs `bricks`, because a null result with a 250→8 brick-count drop means the DDA was never the bottleneck and Task 13's recommendation changes accordingly.

- [ ] **Step 9: Commit**

```bash
git add shaders/raymarch.comp.glsl extension/src/render/raymarch_pass.cpp \
	tests/test_region_dda.gd docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md
git commit -m "perf: region-level DDA, the traversal level spec 3 always described"
```

---

### Task 5: A brick pays for the ops that reach it, not for its region's history

`eval_field(p, op_base, op_count)` walks **every op in the region** for **every sample**. A brick's generation evaluates 4913 lattice samples; a region that has absorbed 200 edits therefore costs ~1M op evaluations per brick, and the edit benchmark leg — which fires one blast per frame into the same few regions — watches that number climb for 300 frames. It is the shape of the measured curve: raymarch p50 6.3 ms steady but 10.3 ms editing, with `custom_frame` p99 at 26.8 ms.

Most of those ops cannot touch the brick. A 3 m sphere 20 m away changes nothing inside a 0.8 m box, and the CPU already knows this — `collect_ops_for_aabb` (M4) and `op_brick_range` (M2) both filter by op AABB, which is why the mesher and the LoD builder receive **per-chunk** lists while brick generation still receives the whole region. This task gives the GPU the same filter, and gives the CPU reference the same one so the differential tests keep pinning both sides together.

**Files:**
- Modify: `extension/src/generator/edit_ops.h`, `edit_ops.cpp` (`op_touches_aabb`)
- Modify: `extension/src/world/brick_eval.cpp` (filter inside `eval_brick`, `brick_has_surface`, `cell_state_field`)
- Modify: `shaders/field.glslh` (the `FIELD_OP_INDEX` hook)
- Modify: `shaders/brick_gen.comp.glsl` (per-brick filtered list), `shaders/brick_mark.comp.glsl` (per-workgroup filtered list)
- Test: `extension/tests/test_op_filter.cpp`, `tests/test_op_filter_gpu.gd`

**Interfaces:**
- Consumes: `ve::op_world_aabb` (M4 Task 6).
- Produces: `bool ve::op_touches_aabb(const EditOp &op, const float lo[3], const float hi[3], float pad)` — true when the op's world AABB, expanded by `pad`, overlaps `[lo, hi]`. Conservative: a `true` answer costs an evaluation, a `false` answer must be provably harmless.

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_op_filter.cpp`:

```cpp
#include "doctest.h"
#include "generator/edit_ops.h"
#include "world/brick.h"
#include <cmath>

namespace {

ve::EditOp sphere(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

} // namespace

TEST_CASE("an op that overlaps the box is kept") {
	const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.8f, 0.8f, 0.8f};
	CHECK(ve::op_touches_aabb(sphere(0.4f, 0.4f, 0.4f, 1.0f), lo, hi, 0.0f));
	CHECK(ve::op_touches_aabb(sphere(2.0f, 0.4f, 0.4f, 1.5f), lo, hi, 0.0f));
}

TEST_CASE("an op that clears the box by more than the pad is dropped") {
	const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.8f, 0.8f, 0.8f};
	CHECK_FALSE(ve::op_touches_aabb(sphere(20.0f, 0.4f, 0.4f, 1.0f), lo, hi, 0.2f));
}

TEST_CASE("the pad is what keeps a grazing op") {
	// A sphere whose surface sits exactly one pad away from the box face must be KEPT: the
	// stored field is a narrow band and the evaluator's own activation margin reaches that
	// far. Dropping it is the failure mode that puts a seam on a brick boundary.
	const float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.8f, 0.8f, 0.8f};
	const ve::EditOp op = sphere(0.4f, 0.4f, 0.8f + 1.0f + 0.19f, 1.0f);
	CHECK(ve::op_touches_aabb(op, lo, hi, 0.2f));
	CHECK_FALSE(ve::op_touches_aabb(op, lo, hi, 0.0f));
}

TEST_CASE("filtering never changes an evaluated sample inside the box") {
	// The property the whole task rests on: for any point in the box, applying the filtered
	// list gives the same sample as applying all of them. Checked on a grid of points
	// against a mixed op list, because "conservative" is only a claim until it is measured.
	ve::AnalyticGenerator gen;
	std::vector<ve::EditOp> all;
	for (int i = 0; i < 40; i++) {
		const float a = static_cast<float>(i) * 0.7f;
		all.push_back(sphere(24.0f + std::cos(a) * static_cast<float>(i),
				51.0f + std::sin(a) * 3.0f, 24.0f + std::sin(a) * static_cast<float>(i),
				0.5f + 0.1f * static_cast<float>(i % 5)));
	}
	const float lo[3] = {24.0f, 51.2f, 24.0f};
	const float hi[3] = {24.8f, 52.0f, 24.8f};
	std::vector<ve::EditOp> kept;
	for (const ve::EditOp &op : all)
		if (ve::op_touches_aabb(op, lo, hi, ve::kActivationPad + ve::kVoxelSize)) kept.push_back(op);
	CHECK(kept.size() < all.size()); // the filter has to actually drop something

	for (int i = 0; i <= 4; i++)
		for (int j = 0; j <= 4; j++)
			for (int k = 0; k <= 4; k++) {
				const float x = lo[0] + (hi[0] - lo[0]) * static_cast<float>(i) / 4.0f;
				const float y = lo[1] + (hi[1] - lo[1]) * static_cast<float>(j) / 4.0f;
				const float z = lo[2] + (hi[2] - lo[2]) * static_cast<float>(k) / 4.0f;
				const ve::Sample full = ve::apply_ops(gen.sample(x, y, z), all.data(),
						static_cast<int>(all.size()), x, y, z);
				const ve::Sample filtered = ve::apply_ops(gen.sample(x, y, z), kept.data(),
						static_cast<int>(kept.size()), x, y, z);
				// Encoded storage clamps at +/-kSdfRange, so agreement is required only
				// where the value is representable -- which is exactly where it is used.
				const float a = full.sdf < -ve::kSdfRange ? -ve::kSdfRange : full.sdf;
				const float b = filtered.sdf < -ve::kSdfRange ? -ve::kSdfRange : filtered.sdf;
				CHECK(a == doctest::Approx(b).epsilon(0.0001));
				CHECK(full.material == filtered.material);
			}
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `op_touches_aabb` is not declared.

- [ ] **Step 3: Write the filter**

In `extension/src/generator/edit_ops.h`:

```cpp
// Does this op's influence reach the box? Conservative by construction: `pad` must cover
// the sampler's own reach (the activation margin for brick residency, the narrow band's
// kSdfRange for stored lattices), because a false negative silently deletes an edit.
bool op_touches_aabb(const EditOp &op, const float lo[3], const float hi[3], float pad);
```

In `edit_ops.cpp`:

```cpp
bool op_touches_aabb(const EditOp &op, const float lo[3], const float hi[3], float pad) {
	float a[3], b[3];
	op_world_aabb(op, a, b);
	for (int i = 0; i < 3; i++) {
		if (a[i] - pad > hi[i]) return false;
		if (b[i] + pad < lo[i]) return false;
	}
	return true;
}
```

- [ ] **Step 4: Filter in the CPU reference**

In `extension/src/world/brick_eval.cpp`, `eval_brick` builds the filtered list once per brick before the lattice loop, and `brick_has_surface` / `cell_state_field` do the same before their 27 probes:

```cpp
	// One filter pass per brick instead of one op walk per sample. The pad is the same one
	// op_brick_range uses to decide which bricks an op dirties, so a brick is evaluated with
	// exactly the ops that marked it -- the two answers can never disagree.
	float blo[3], bhi[3];
	brick_world_aabb(brick, blo, bhi);
	std::vector<const EditOp *> kept;
	kept.reserve(static_cast<size_t>(op_count));
	for (int i = 0; i < op_count; i++)
		if (op_touches_aabb(ops[i], blo, bhi, kActivationPad + kVoxelSize)) kept.push_back(&ops[i]);
```

Then apply `kept` in order. Add `brick_world_aabb(IVec3, float lo[3], float hi[3])` to `world/brick.h` if it is not already there (`brick.h` owns the brick lattice math).

**Order is load-bearing** — the filter preserves it because it walks the list front to back and appends. Never sort, never deduplicate.

- [ ] **Step 5: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS, all suites — including the existing `test_brick_eval` / `test_edit_ops` suites, which is the point: the filter changed nothing they can see.

- [ ] **Step 6: Commit the pure half**

```bash
git add extension/src/generator/edit_ops.h extension/src/generator/edit_ops.cpp \
	extension/src/world/brick_eval.cpp extension/src/world/brick.h \
	extension/tests/test_op_filter.cpp
git commit -m "perf: evaluate a brick with the ops that reach it (CPU reference)"
```

- [ ] **Step 7: Add the GLSL hook**

In `shaders/field.glslh`, replace the tail of `eval_field`:

```glsl
// How a caller names the pool index of its i-th op. The default is the region's list in
// order; a caller that has pre-filtered the list into shared memory redefines this BEFORE
// including this file, and passes its own count. There is exactly one loop either way.
#ifndef FIELD_OP_INDEX
#define FIELD_OP_INDEX(base, i) ((base) + (i))
#endif

void eval_field(vec3 p, uint op_base, uint op_count, out float sdf, out uint mat) {
	base_field(p, sdf, mat);
	for (uint i = 0u; i < op_count; i++) apply_field_op(FIELD_OP_INDEX(op_base, i), p, sdf, mat);
}
```

- [ ] **Step 8: Filter per brick in the generator**

In `shaders/brick_gen.comp.glsl`, **before** the `field.glslh` include (the macro and the shared array must both be visible when the header is compiled):

```glsl
// The region's ops that can reach THIS brick, in append order, as pool-relative indices.
// 256 uints of shared memory next to the 16 KB s_mat already here.
shared uint s_ops[256];
shared uint s_op_n;
shared uint s_keep[256]; // per-op keep flag, compacted in order by thread 0
#define FIELD_OP_INDEX(base, i) ((base) + s_ops[i])
```

At the top of `main()`, after the job's brick and region slot are known and before any field evaluation:

```glsl
	// Test in parallel, compact serially. atomicAdd would compact too, but it would also
	// scramble the order, and an ordered CSG list whose order is gone is a different world.
	vec3 brick_lo = vec3(brick) * BRICK_SIZE;
	vec3 brick_hi = brick_lo + vec3(BRICK_SIZE);
	if (tid < op_count) s_keep[tid] = op_touches_aabb(op_base + tid, brick_lo, brick_hi,
			ACTIVATION_PAD + VOXEL_SIZE) ? 1u : 0u;
	if (tid == 0u) s_op_n = 0u;
	barrier();
	if (tid == 0u) {
		uint n = 0u;
		for (uint i = 0u; i < op_count; i++) if (s_keep[i] != 0u) s_ops[n++] = i;
		s_op_n = n;
	}
	barrier();
```

and pass `s_op_n` wherever `op_count` was passed to `eval_field`.

`op_touches_aabb(uint index, vec3 lo, vec3 hi, float pad)` is the GLSL mirror, added to `field.glslh` next to `apply_field_op` — it unpacks the same two `uvec4`s and computes the same AABB per op type as `ve::op_world_aabb` (sphere: centre ± radius; box: `pos` to `pos + extent * OCCUPANCY_CELL_SIZE`, expanded by the clearance margin in `radius`; volume: `pos` to `pos + (dim - 1) * voxel`). Mirror it exactly; `tests/test_field_diff.gd` is what will catch a divergence.

- [ ] **Step 9: Filter per workgroup in the mark pass**

`brick_mark.comp.glsl` runs one **thread** per brick, so it has no per-brick shared memory to compact into — but a workgroup of 256 threads covers 256 consecutive bricks along x, and their union is a thin slab. Filtering against the slab still drops most ops for a blast that is metres away in y or z:

```glsl
shared uint s_ops[256];
shared uint s_op_n;
shared uint s_keep[256];
#define FIELD_OP_INDEX(base, i) ((base) + s_ops[i])
```

```glsl
	// The slab this workgroup covers: 256 bricks along x at most, clipped to the scan range.
	int wg_first = int(gl_WorkGroupID.x) * 256;
	int wg_last = min(wg_first + 255, total - 1);
	ivec3 b0 = pc.lo.xyz + ivec3(wg_first % ext.x, (wg_first / ext.x) % ext.y, wg_first / (ext.x * ext.y));
	ivec3 b1 = pc.lo.xyz + ivec3(wg_last % ext.x, (wg_last / ext.x) % ext.y, wg_last / (ext.x * ext.y));
	// A workgroup can wrap a row, so take the conservative union across the whole span
	// rather than assuming b0 and b1 bound it.
	vec3 slab_lo = vec3(min(b0, b1)) * BRICK_SIZE;
	vec3 slab_hi = (vec3(max(b0, b1)) + vec3(1.0)) * BRICK_SIZE;
	if (b0.y != b1.y || b0.z != b1.z) {
		slab_lo = vec3(pc.lo.xyz) * BRICK_SIZE;
		slab_hi = (vec3(pc.hi.xyz) + vec3(1.0)) * BRICK_SIZE;
	}
```

then the same test/compact/barrier block as Step 8, with `ACTIVATION_PAD` as the pad (the probe's own margin), and `brick_probe` taking `s_op_n`.

The wrap fallback widens the slab to the whole scan range, which degrades to today's behaviour — correct, never wrong, and it only triggers on the last workgroup of a row.

- [ ] **Step 10: Write the failing GPU test**

Create `tests/test_op_filter_gpu.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

# ve::EditOp is 32 raw bytes and debug_brick_diff takes them as a PackedByteArray, exactly
# as test_brick_diff.gd already packs them: type u32, material u32, pos[3] f32, radius f32,
# aux[2] u32.
func op_bytes(type: int, material: int, c: Vector3, radius: float) -> PackedByteArray:
	var b := PackedByteArray()
	b.resize(32)
	b.encode_u32(0, type)
	b.encode_u32(4, material)
	b.encode_float(8, c.x)
	b.encode_float(12, c.y)
	b.encode_float(16, c.z)
	b.encode_float(20, radius)
	b.encode_u32(24, 0)
	b.encode_u32(28, 0)
	return b

# The differential test that matters: with a long op list where most ops are far away, the
# GPU brick must still equal the CPU brick. debug_brick_diff is M2's existing hook -- it
# generates one brick on the GPU and compares every voxel against ve::eval_brick.
func test_generated_brick_matches_the_cpu_with_a_long_op_list(timeout := 30000) -> void:
	var w := make_world()
	var ops := PackedByteArray()
	# 200 ops: four of them land on the brick under test, the rest are scattered.
	var target := Vector3(24.4, 51.6, 24.4)
	for i in range(200):
		var c: Vector3 = target if i % 50 == 0 else target + Vector3(
			cos(float(i)) * (5.0 + float(i) * 0.1), sin(float(i)) * 2.0,
			sin(float(i) * 1.3) * (5.0 + float(i) * 0.1))
		ops.append_array(op_bytes(0, 0, c, 1.2))
	var d: Dictionary = w.debug_brick_diff(Vector3i(30, 64, 30), 0, ops, 200)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
	assert_int(int(d["mat_mismatches"])).is_equal(0)

# Order is the other half of correctness: subtract-then-add is not add-then-subtract, and a
# filter that reorders would pass a sample-count test while producing a different world.
func test_filter_preserves_op_order(timeout := 30000) -> void:
	var w := make_world()
	var c := Vector3(24.4, 51.6, 24.4)
	var ops := PackedByteArray()
	ops.append_array(op_bytes(0, 0, c, 2.0))       # subtract
	ops.append_array(op_bytes(1, 4, c, 1.0))       # add back inside the hole
	for i in range(50):                             # far-away filler
		ops.append_array(op_bytes(0, 0, Vector3(c.x + 40.0 + float(i), c.y, c.z), 1.0))
	var d: Dictionary = w.debug_brick_diff(Vector3i(30, 64, 30), 0, ops, 52)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
```

(Check `tests/test_brick_diff.gd` for the packing helper it already uses and reuse that spelling rather than inventing a second one.)

- [ ] **Step 11: Run the tests to verify they pass**

Run:
```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_op_filter_gpu.gd
./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd
./gdunit_tests.sh -c -a res://tests/test_field_diff.gd
./gdunit_tests.sh -c -a res://tests/test_field_volume_diff.gd
./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd
./gdunit_tests.sh -c -a res://tests/test_occupancy.gd
```
Expected: PASS for all six. `test_field_diff` and `test_field_volume_diff` are the CPU/GPU mirrors of `eval_field` itself; `test_occupancy` is what notices if the mark pass's filter changed a residency decision.

- [ ] **Step 12: Measure**

Run: `tools/run_benchmarks.sh m7-task5`
Record in **Errata entry 5**: `gpu_stream` and `gpu_raymarch` p50/p99 for the **edit** leg above all, against Task 4's numbers. This is the task expected to move the edit leg specifically; if the edit leg's `gpu_stream` does not fall, say so plainly and note that op-list length is not what makes editing expensive — Task 8's consolidation trigger should then be reconsidered against the measurement rather than the theory.

- [ ] **Step 13: Commit**

```bash
git add shaders/field.glslh shaders/brick_gen.comp.glsl shaders/brick_mark.comp.glsl \
	tests/test_op_filter_gpu.gd docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md
git commit -m "perf: per-brick and per-workgroup op filtering on the GPU"
```

---

### Task 6: `world/override_store` — the field's base, stored

Spec §2: "Op-list overflow (>256 ops/region) consolidates into explicitly stored override bricks." M2 deferred it and rejected the 257th op; M4 kept that behaviour and added one more fail-soft arm. The demo's HUD has shown an `ovf` counter ever since, and a long session in one crater trips it: the edits stop landing, and nothing tells the player why.

An override brick is the region's field, at 5 cm, *baked*. Once a brick has one, its content no longer depends on the op list, so consolidation is: bake every brick the ops touch, then **clear the list**. Overrides are not ops — they replace the field's *base*, and they are found through a per-region brick→slot table, the same indirection shape `region_tables` already uses. That is what keeps a consolidated region's evaluation cost at "G plus the handful of ops since", instead of trading one unbounded list for another.

This task is the pure core: the pool, the lookup, the sampler, and the planner that decides which bricks a consolidation must bake. Nothing here touches a GPU or a Godot type.

**Files:**
- Create: `extension/src/world/override_store.h`, `extension/src/world/override_store.cpp`
- Create: `extension/tests/test_override_store.cpp`
- Modify: `extension/src/world/brick_eval.h`, `brick_eval.cpp` (`eval_field` consults an override base)
- Modify: `extension/src/world/raycast.h`, `raycast.cpp` (thread the override source through)
- Modify: `extension/src/world/edit_log.h`, `edit_log.cpp` (`clear_region`, `op_count` already there)

**Interfaces:**
- Consumes: `ve::Brick`, `ve::kBrickSdfStride`, `ve::sdf_index`, `ve::encode_sdf`/`decode_sdf` (M1/M2); `ve::op_touches_aabb` (Task 5).
- Produces:
  - `struct ve::OverrideBrick { uint8_t sdf[kBrickSdfCount]; uint8_t mat[kBrickVoxelCount]; }`
  - `struct ve::OverrideSource { virtual bool sample(float x, float y, float z, Sample *out) const = 0; }` — the interface `eval_field` takes, so `world/` owns storage and `generator/` stays free of it.
  - `class ve::OverrideStore : public OverrideSource` with `acquire(IVec3 brick) -> int`, `slot_of(IVec3) const -> int`, `release(IVec3)`, `data(int) -> OverrideBrick *`, `capacity()`, `used()`, `sample(...)`.
  - `void ve::plan_consolidation(const EditOp *ops, int op_count, IVec3 region, std::vector<IVec3> *bricks)` — every brick of `region` any op reaches, padded by `kSdfRange + kVoxelSize`, in a deterministic order.
  - `void ve::EditLog::clear_region(IVec3 region)`.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_override_store.cpp`:

```cpp
#include "doctest.h"
#include "world/override_store.h"
#include "world/brick_eval.h"
#include "generator/generator.h"
#include <cmath>

namespace {

ve::EditOp sphere_sub(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

// Bake one brick's lattice out of the generator plus ops, exactly as the GPU pass will.
void bake(const ve::Generator &gen, const ve::EditOp *ops, int n, ve::IVec3 brick,
		ve::OverrideBrick *out) {
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const float wx = (static_cast<float>(brick.x) * ve::kBrickVoxels + x) * ve::kVoxelSize;
				const float wy = (static_cast<float>(brick.y) * ve::kBrickVoxels + y) * ve::kVoxelSize;
				const float wz = (static_cast<float>(brick.z) * ve::kBrickVoxels + z) * ve::kVoxelSize;
				const ve::Sample s = ve::eval_field(gen, ops, n, wx, wy, wz);
				out->sdf[ve::sdf_index(x, y, z)] = ve::encode_sdf(s.sdf);
			}
	for (int z = 0; z < ve::kBrickVoxels; z++)
		for (int y = 0; y < ve::kBrickVoxels; y++)
			for (int x = 0; x < ve::kBrickVoxels; x++) {
				const float wx = (static_cast<float>(brick.x) * ve::kBrickVoxels + x + 0.5f) * ve::kVoxelSize;
				const float wy = (static_cast<float>(brick.y) * ve::kBrickVoxels + y + 0.5f) * ve::kVoxelSize;
				const float wz = (static_cast<float>(brick.z) * ve::kBrickVoxels + z + 0.5f) * ve::kVoxelSize;
				const ve::Sample s = ve::eval_field(gen, ops, n, wx, wy, wz);
				out->mat[x + y * ve::kBrickVoxels + z * ve::kBrickVoxels * ve::kBrickVoxels] =
						static_cast<uint8_t>(s.material & 0xFFu);
			}
}

} // namespace

TEST_CASE("a slot is handed out once and found again") {
	ve::OverrideStore store(4);
	const ve::IVec3 b{30, 64, 30};
	const int slot = store.acquire(b);
	CHECK(slot >= 0);
	CHECK(store.acquire(b) == slot); // idempotent: a re-consolidation reuses the brick's slot
	CHECK(store.slot_of(b) == slot);
	CHECK(store.slot_of({0, 0, 0}) == -1);
	CHECK(store.used() == 1);
	store.release(b);
	CHECK(store.slot_of(b) == -1);
	CHECK(store.used() == 0);
}

TEST_CASE("a full pool refuses rather than evicting") {
	// Fail-soft (spec §8): a refused consolidation leaves the op list exactly as it was, and
	// the region keeps working. Evicting somebody else's override would corrupt the world.
	ve::OverrideStore store(2);
	CHECK(store.acquire({0, 0, 0}) >= 0);
	CHECK(store.acquire({1, 0, 0}) >= 0);
	CHECK(store.acquire({2, 0, 0}) == -1);
	CHECK(store.used() == 2);
}

TEST_CASE("sampling an override reproduces the field it baked") {
	ve::AnalyticGenerator gen;
	const ve::EditOp ops[1] = {sphere_sub(24.4f, 51.4f, 24.4f, 1.5f)};
	const ve::IVec3 brick{30, 64, 30};
	ve::OverrideStore store(4);
	const int slot = store.acquire(brick);
	bake(gen, ops, 1, brick, store.data(slot));

	// At a lattice point the stored value is exact to the encoding's step (~5 mm).
	const float px = static_cast<float>(brick.x) * ve::kBrickSize + 4 * ve::kVoxelSize;
	const float py = static_cast<float>(brick.y) * ve::kBrickSize + 4 * ve::kVoxelSize;
	const float pz = static_cast<float>(brick.z) * ve::kBrickSize + 4 * ve::kVoxelSize;
	ve::Sample got{};
	CHECK(store.sample(px, py, pz, &got));
	const ve::Sample want = ve::eval_field(gen, ops, 1, px, py, pz);
	CHECK(got.sdf == doctest::Approx(want.sdf).epsilon(0.02));
}

TEST_CASE("a point outside every override brick is not claimed") {
	ve::OverrideStore store(4);
	const ve::IVec3 brick{30, 64, 30};
	store.acquire(brick);
	ve::Sample got{};
	// One brick over: the store must say "not mine" so the caller falls back to G, or the
	// world would gain a 0.8 m box of zeroes wherever a consolidation stopped.
	CHECK_FALSE(store.sample(static_cast<float>(brick.x + 2) * ve::kBrickSize,
			static_cast<float>(brick.y) * ve::kBrickSize,
			static_cast<float>(brick.z) * ve::kBrickSize, &got));
}

TEST_CASE("the plan covers every brick an op can reach and no more") {
	std::vector<ve::EditOp> ops;
	ops.push_back(sphere_sub(24.4f, 51.4f, 24.4f, 1.5f));
	std::vector<ve::IVec3> bricks;
	ve::plan_consolidation(ops.data(), 1, {0, 2, 0}, &bricks);
	CHECK(!bricks.empty());
	// Every planned brick is inside the region...
	for (const ve::IVec3 &b : bricks) {
		CHECK(b.x >> 5 == 0);
		CHECK(b.y >> 5 == 2);
		CHECK(b.z >> 5 == 0);
	}
	// ...and every brick the op reaches is planned. The check that matters is the second
	// direction: a missed brick is an edit that silently un-happens at consolidation time.
	const float pad = ve::kSdfRange + ve::kVoxelSize;
	for (int bz = 0; bz < 32; bz++)
		for (int by = 0; by < 32; by++)
			for (int bx = 0; bx < 32; bx++) {
				const ve::IVec3 b{bx, 64 + by, bz};
				float lo[3], hi[3];
				ve::brick_world_aabb(b, lo, hi);
				if (!ve::op_touches_aabb(ops[0], lo, hi, pad)) continue;
				bool found = false;
				for (const ve::IVec3 &p : bricks)
					if (p.x == b.x && p.y == b.y && p.z == b.z) { found = true; break; }
				CHECK(found);
			}
}

TEST_CASE("eval_field prefers an override over the generator") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 brick{30, 64, 30};
	ve::OverrideStore store(4);
	const int slot = store.acquire(brick);
	// Bake "solid rock everywhere" into the brick, which the generator would never produce
	// there, so the preference is unmistakable.
	for (int i = 0; i < ve::kBrickSdfCount; i++) store.data(slot)->sdf[i] = ve::encode_sdf(-0.5f);
	for (int i = 0; i < ve::kBrickVoxelCount; i++) store.data(slot)->mat[i] = 2;

	const float px = static_cast<float>(brick.x) * ve::kBrickSize + 0.4f;
	const float py = static_cast<float>(brick.y) * ve::kBrickSize + 0.4f;
	const float pz = static_cast<float>(brick.z) * ve::kBrickSize + 0.4f;
	const ve::Sample s = ve::eval_field(gen, nullptr, 0, px, py, pz, nullptr, &store);
	CHECK(s.sdf == doctest::Approx(-0.5f).epsilon(0.02));
	CHECK(s.material == 2u);
}

TEST_CASE("ops still apply on top of an override") {
	// Consolidation clears the list, but the NEXT edit lands on the baked base. If ops were
	// applied to G instead, every edit after a consolidation would undo it.
	ve::AnalyticGenerator gen;
	const ve::IVec3 brick{30, 64, 30};
	ve::OverrideStore store(4);
	const int slot = store.acquire(brick);
	for (int i = 0; i < ve::kBrickSdfCount; i++) store.data(slot)->sdf[i] = ve::encode_sdf(-0.5f);
	for (int i = 0; i < ve::kBrickVoxelCount; i++) store.data(slot)->mat[i] = 2;

	const float px = static_cast<float>(brick.x) * ve::kBrickSize + 0.4f;
	const float py = static_cast<float>(brick.y) * ve::kBrickSize + 0.4f;
	const float pz = static_cast<float>(brick.z) * ve::kBrickSize + 0.4f;
	const ve::EditOp cut[1] = {sphere_sub(px, py, pz, 1.0f)};
	const ve::Sample s = ve::eval_field(gen, cut, 1, px, py, pz, nullptr, &store);
	CHECK(s.sdf > 0.0f); // the sphere carved the baked rock away
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `world/override_store.h: No such file or directory`.

- [ ] **Step 3: Write the store**

`extension/src/world/override_store.h`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include "world/brick.h"
#include "world/region.h"
#include <cstdint>
#include <map>
#include <vector>

namespace ve {

// One brick's baked field: the same 17^3 encoded lattice the atlas stores, plus one byte of
// GLOBAL material id per cell. A byte rather than a 2-bit palette index for the same reason
// M4's island volumes store a byte (see that plan's Deliberate Deferrals): the pool is
// nowhere near its cap, and it removes the palette-packing step from the bake and its CPU
// reference entirely.
struct OverrideBrick {
	uint8_t sdf[kBrickSdfCount]{};
	uint8_t mat[kBrickVoxelCount]{};
};

// How eval_field asks whether a point's BASE field has been baked. An interface so that
// generator/ never learns about storage, exactly as VolumeStore does for stored volumes.
struct OverrideSource {
	virtual ~OverrideSource() = default;
	virtual bool sample(float x, float y, float z, Sample *out) const = 0;
};

// Fixed-capacity pool of baked bricks with a brick-coordinate index. No eviction: an
// override is the only record of what the ops it replaced did, so dropping one would silently
// restore terrain a player destroyed. A full pool refuses (spec §8 fail-soft).
class OverrideStore : public OverrideSource {
public:
	explicit OverrideStore(int capacity);

	int capacity() const { return static_cast<int>(bricks_.size()); }
	int used() const { return static_cast<int>(index_.size()); }
	int acquire(IVec3 brick);            // existing slot, a new one, or -1 when full
	int slot_of(IVec3 brick) const;      // -1 when absent
	void release(IVec3 brick);
	void clear();
	OverrideBrick *data(int slot);
	const OverrideBrick *data(int slot) const;

	// Trilinear over the stored lattice, nearest for the material -- the same reconstruction
	// the raymarcher performs on the atlas, so a consolidated brick looks identical to the
	// generated one it replaced. False when the point is in no override brick.
	bool sample(float x, float y, float z, Sample *out) const override;

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	std::vector<OverrideBrick> bricks_;
	std::vector<int> free_;
	std::map<Key, int> index_;
};

// Every brick of `region` that any of `ops` can reach, padded by kSdfRange + kVoxelSize --
// the narrow band's own width, because a stored lattice records a value wherever the op
// changed one, not only where it moved the surface. Emitted in x-major order, deterministic
// so a CPU plan and a GPU dispatch can be compared entry by entry.
void plan_consolidation(const EditOp *ops, int op_count, IVec3 region,
		std::vector<IVec3> *bricks);

} // namespace ve
```

`override_store.cpp` implements it: `acquire` looks the key up, pops `free_` when absent, returns -1 when `free_` is empty; `sample` floors the point to a brick, looks up the slot, returns false when absent, and otherwise runs the same trilinear reconstruction as `ve::Brick::sample` (share the helper if `brick.h` exposes one — do not write a second interpolator). `plan_consolidation` walks the region's 32³ bricks once, tests each against every op with `op_touches_aabb`, and appends on the first hit.

- [ ] **Step 4: Thread the override through the field**

In `brick_eval.h`, extend the two entry points (defaulted, so no existing call site changes):

```cpp
Sample eval_field(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr);
```

and in `brick_eval.cpp`:

```cpp
Sample eval_field(const Generator &gen, const EditOp *ops, int op_count, float x, float y,
		float z, const VolumeStore *volumes, const OverrideSource *overrides) {
	Sample s{};
	// The base is the bake when there is one. Ops still apply on top: consolidation clears
	// the list it baked, and everything appended afterwards is a new edit on a new base.
	if (!overrides || !overrides->sample(x, y, z, &s)) s = gen.sample(x, y, z);
	return apply_ops(s, ops, op_count, x, y, z, volumes);
}
```

Give `eval_brick`, `brick_has_surface`, `cell_state_field` and `ve::raycast` the same defaulted parameter and pass it straight through. `EditLog::clear_region(IVec3)` erases that region's op and sequence lists — the same `map::erase` `clear()` already does for all of them.

- [ ] **Step 5: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS, all suites.

- [ ] **Step 6: Commit**

```bash
git add extension/src/world/override_store.h extension/src/world/override_store.cpp \
	extension/src/world/brick_eval.h extension/src/world/brick_eval.cpp \
	extension/src/world/raycast.h extension/src/world/raycast.cpp \
	extension/src/world/edit_log.h extension/src/world/edit_log.cpp \
	extension/tests/test_override_store.cpp
git commit -m "feat: override brick store and the field base it replaces"
```

---

### Task 7: The override pool on both devices, and the bake that fills it

The store from Task 6 has to exist in three places at once: on the CPU (raycasting, the differential references, the aiming reticle), on the **render** device (brick generation and the mark pass), and on the **worker** device (collision meshing, LoD building, island extraction). That is the same three-way life a stored volume already leads — `ve::VolumeSet` on the CPU, `VolumePool` on each device, uploads drained on the thread that owns each — so this task follows M4's volume path rather than inventing a second one.

The bake itself runs on the **worker** device, for the same reason island extraction does: it is a large, bursty dispatch that must not land inside a frame, and its result has to come back to the CPU anyway.

**Files:**
- Create: `extension/src/render/override_pool.h`, `override_pool.cpp`
- Create: `shaders/brick_consolidate.comp.glsl`
- Modify: `shaders/field.glslh` (override sampling in `eval_field`)
- Modify: `shaders/brick_gen.comp.glsl`, `brick_mark.comp.glsl`, `mesh_field.comp.glsl`, `lod_field.comp.glsl`, `island_extract.comp.glsl` (bind the pool)
- Modify: `extension/src/render/gpu_atlas.h/.cpp` (render-device pool + region override table), `mesh_pass.h/.cpp`, `lod_build_pass.h/.cpp`, `island_extract_pass.h/.cpp` (worker-device pool)
- Modify: `extension/src/render/mesh_service.h/.cpp` (`submit_consolidations` / `collect_consolidations`)
- Test: `tests/test_consolidation.gd` (part 1: the bake)

**Interfaces:**
- Consumes: `ve::OverrideStore`, `ve::plan_consolidation` (Task 6); `VolumePool`'s upload/binding pattern (M4 Task 7); `MeshService`'s extract queue shape (M4 Task 9).
- Produces:
  - `class godot::OverridePool` — `initialize(rd, int capacity)`, `upload(int slot, const ve::OverrideBrick &)`, `set_table_entry(rd, int table, int brick_index, int slot)`, `RID sdf_buffer()`, `RID mat_buffer()`, `RID tables()`, `RID region_table_map()`.
  - `struct godot::ConsolidateJob { ve::IVec3 region; int region_slot; std::vector<ve::IVec3> bricks; std::vector<ve::EditOp> ops; }`
  - `struct godot::ConsolidateResult { ve::IVec3 region; std::vector<ve::IVec3> bricks; std::vector<ve::OverrideBrick> baked; bool failed = false; }`
  - `bool MeshService::submit_consolidations(std::vector<ConsolidateJob>)`, `int MeshService::collect_consolidations(std::vector<ConsolidateResult> *)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_consolidation.gd` with the bake half:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

# The bake has to reproduce the field it replaces. debug_consolidate_diff bakes one region's
# bricks on the worker device, then compares every baked lattice sample against ve::eval_field
# with the same ops -- the same shape as debug_brick_diff, one level up.
func test_bake_reproduces_the_field(timeout := 60000) -> void:
	var w := make_world()
	for i in range(12):
		w.debug_apply_sphere_subtract(Vector3(24.0 + float(i) * 0.5, 51.5, 24.0), 1.2)
	var d: Dictionary = w.debug_consolidate_diff(Vector3i(0, 2, 0))
	assert_int(int(d["bricks"])).is_greater(0)
	assert_int(int(d["sdf_mismatches"])).is_equal(0)
	assert_int(int(d["mat_mismatches"])).is_equal(0)

# An overridden brick must read back through the FIELD, not just out of the pool: the whole
# point is that every consumer sees the same base.
func test_overridden_brick_reads_through_eval_field(timeout := 60000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	# The op list is gone; the crater must still be there.
	assert_int(int(w.debug_op_counts()["region_0_2_0"])).is_equal(0)
	var hit: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	assert_float(float(hit["pos"].y)).is_less(51.4 - 1.0)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_consolidation.gd`
Expected: FAIL — `Nonexistent function 'debug_consolidate_diff'`.

- [ ] **Step 3: Write the pool**

`OverridePool` owns four resources per device:

| Resource | Size | Contents |
|---|---|---|
| `sdf_buffer` | `capacity * 4913` bytes, rounded up to a word | encoded lattice, byte-packed like `VolumePool` |
| `mat_buffer` | `capacity * 4096` bytes | one global material id byte per cell |
| `tables` | `kMaxOverrideTables * 32768` ints (4 MB) | brick index → override slot, −1 for absent |
| `region_table_map` | `max_region_slots` ints | region slot → table index, −1 for none |

Byte packing follows `VolumePool` exactly: bytes live inside `uint` words and are unpacked by hand in GLSL, so there is one layout rule and it is written in the shader. `static_assert` the strides against `ve::kBrickSdfCount` / `ve::kBrickVoxelCount` in `override_pool.cpp`, the way `volume_pool.cpp` pins `VOLUME_VOXELS`.

- [ ] **Step 4: Teach `field.glslh` about the base**

```glsl
#ifdef FIELD_OVERRIDE_SDF_BINDING
layout(set = 0, binding = FIELD_OVERRIDE_SDF_BINDING, std430) readonly buffer FieldOverrideSdf {
	uint w[];
} field_override_sdf;
layout(set = 0, binding = FIELD_OVERRIDE_MAT_BINDING, std430) readonly buffer FieldOverrideMat {
	uint w[];
} field_override_mat;
layout(set = 0, binding = FIELD_OVERRIDE_TABLE_BINDING, std430) readonly buffer FieldOverrideTables {
	int slot[];
} field_override_tables;

// Which table (if any) belongs to the region holding this point. -1 for every region that
// has never been consolidated, which is nearly all of them; one buffer read answers it.
// The includer sets FIELD_OVERRIDE_TABLE for its own dispatch (it always knows its region),
// so the common case costs nothing at all.
bool sample_field_override(vec3 p, out float sdf, out uint mat) {
	sdf = 0.0;
	mat = 0u;
	int table = FIELD_OVERRIDE_TABLE;
	if (table < 0) return false;
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int bi = (brick.x & 31) + (brick.y & 31) * REGION_BRICKS +
			(brick.z & 31) * REGION_BRICKS * REGION_BRICKS;
	int slot = field_override_tables.slot[table * REGION_BRICK_COUNT + bi];
	if (slot < 0) return false;
	// Mirror of ve::OverrideStore::sample: trilinear over the 17^3 lattice, nearest for the
	// material, in the brick's own local voxel coordinates.
	vec3 l = clamp((p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE, vec3(0.0), vec3(BRICK_SDF_MAX));
	... // eight byte reads at slot * BRICK_SDF_COUNT + sdf_index(...), mixed as usual
	return true;
}
#endif
```

and in `eval_field`:

```glsl
void eval_field(vec3 p, uint op_base, uint op_count, out float sdf, out uint mat) {
#ifdef FIELD_OVERRIDE_SDF_BINDING
	if (!sample_field_override(p, sdf, mat)) base_field(p, sdf, mat);
#else
	base_field(p, sdf, mat);
#endif
	for (uint i = 0u; i < op_count; i++) apply_field_op(FIELD_OP_INDEX(op_base, i), p, sdf, mat);
}
```

Every consumer defines `FIELD_OVERRIDE_TABLE` before the include — `brick_gen` and `brick_mark` from a push-constant field fed by `region_table_map[rslot]`, the mesh/LoD/extract passes from a push-constant field the CPU fills when it builds the job (those passes already carry a flattened op list; the table index travels the same way).

- [ ] **Step 5: Write the bake shader**

`shaders/brick_consolidate.comp.glsl`: one workgroup per brick, 256 threads, `local_size_x = 256`. It is `brick_gen.comp.glsl` with the mips, the palette and the material projection removed — it writes the encoded lattice into `override_sdf` and the nearest material id into `override_mat`, both at `slot * stride`, using the region's ops through the unfiltered index macro (the bake runs once per brick per consolidation; the shared-memory filter from Task 5 would buy nothing at that frequency).

The one subtle rule lives here, so write it into the shader as a comment:

```glsl
// A re-consolidation bakes `previous override + the ops since it`, not `G + ops`. The ops in
// this job's list are relative to whatever base the region had when they were appended, so
// this shader READS the region's existing override table (FIELD_OVERRIDE_TABLE is the real
// one, never -1) and writes into a SECOND slot. The table entry is repointed by the CPU only
// after the whole region's bake has come back intact -- a half-applied consolidation would
// leave some bricks describing a world the rest of them no longer agree with.
```

Double-buffering costs one extra slot per brick in flight. A pool that cannot fund the second copy refuses the whole consolidation, which is the fail-soft arm Task 6's `a full pool refuses rather than evicting` test already pins.

- [ ] **Step 6: Queue it on the worker**

`MeshService` gains `submit_consolidations` / `collect_consolidations` as a third queue beside meshes, extracts and LoD builds — copy the extract queue's structure verbatim (same mutex, same `pending_`/`results_` pair, same "refuse when the worker is busy" contract, same `debug_set_fail_*` test hook shape). The worker thread runs `ConsolidatePass::run(job)` on its local device, reads the baked bytes back (9 KB per brick), and fills `ConsolidateResult`.

- [ ] **Step 7: Add the two debug hooks**

- `VoxelWorld::debug_consolidate_diff(Vector3i region) -> Dictionary` — plans, bakes on the worker, and compares every baked sample against `ve::eval_field` with the same ops on the CPU. Returns `{bricks, sdf_mismatches, mat_mismatches, first_mismatch}`. This is the differential test spec §8 asks for, at the consolidation level.
- `VoxelWorld::debug_consolidate_region(Vector3i region) -> bool` — the full path Task 8 will call automatically: plan, bake, store into `ve::OverrideStore`, upload to both device pools, publish the table entries, clear the region's op list, mark the region for regeneration and dirty its LoD chain. Returns false (changing nothing) when the plan does not fit the pool.

- [ ] **Step 8: Run the tests**

Run:
```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd
./gdunit_tests.sh -c -a res://tests/test_mesh_diff.gd
./gdunit_tests.sh -c -a res://tests/test_lod_mesh_diff.gd
./gdunit_tests.sh -c -a res://tests/test_island_extract.gd
./gdunit_tests.sh -c -a res://tests/test_field_diff.gd
```
Expected: PASS for all six. The four differential suites are the proof that the override binding did not change what any consumer computes when no override exists — the `-1` table path has to be exactly today's behaviour.

- [ ] **Step 9: Commit**

```bash
git add extension/src/render/override_pool.h extension/src/render/override_pool.cpp \
	extension/src/render/consolidate_pass.h extension/src/render/consolidate_pass.cpp \
	extension/src/render/mesh_service.h extension/src/render/mesh_service.cpp \
	extension/src/render/gpu_atlas.h extension/src/render/gpu_atlas.cpp \
	extension/src/render/mesh_pass.cpp extension/src/render/lod_build_pass.cpp \
	extension/src/render/island_extract_pass.cpp extension/src/voxel_world.h \
	extension/src/voxel_world.cpp shaders/field.glslh shaders/brick_consolidate.comp.glsl \
	shaders/brick_gen.comp.glsl shaders/brick_mark.comp.glsl shaders/mesh_field.comp.glsl \
	shaders/lod_field.comp.glsl shaders/island_extract.comp.glsl tests/test_consolidation.gd
git commit -m "feat: override brick pool on both devices and the consolidation bake"
```

---

### Task 8: The 257th edit lands

Everything is in place; this task decides *when* to consolidate and proves the wall is gone. The trigger is the op count, checked where edits are appended, and the work is drained one region per frame on the worker so a consolidation never appears as a frame spike.

**Files:**
- Modify: `extension/src/voxel_world.h`, `voxel_world.cpp` (trigger, drain, stats, the `max_override_bricks` export)
- Modify: `demo/hud.gd` (show overrides instead of only overflow)
- Test: `tests/test_consolidation.gd` (part 2: the policy)

**Interfaces:**
- Consumes: `debug_consolidate_region` and everything under it (Task 7).
- Produces: `VoxelWorld::max_override_bricks` (exported int, default 8192); `debug_stream_stats()` gains `override_bricks`, `consolidations`, `consolidation_refusals`; `ve::kConsolidateAtOps = 192`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_consolidation.gd`:

```gdscript
# The wall M2 and M4 both left standing: op 257 in one region used to be rejected and logged.
# It must now land, because the list consolidates itself out of the way first.
func test_three_hundred_edits_in_one_region_all_land(timeout := 120000) -> void:
	var w := make_world()
	var center := Vector3(24.0, 51.5, 24.0)
	for i in range(300):
		var a := float(i) * 0.21
		w.debug_apply_sphere_subtract(center + Vector3(cos(a) * 2.0, sin(a * 0.7) * 1.5, sin(a) * 2.0), 0.9)
		w.debug_pump_consolidation() # what _process does once a frame
	var st: Dictionary = w.debug_stream_stats()
	assert_int(int(st["overflow_ever"])).is_equal(0)
	assert_int(int(st["consolidations"])).is_greater(0)
	assert_int(int(st["override_bricks"])).is_greater(0)

# Consolidation must not change what the world looks like. The oracle is the raycast the
# edit tool aims with -- the same field the renderer marches.
func test_consolidation_preserves_the_surface(timeout := 120000) -> void:
	var w := make_world()
	for i in range(40):
		w.debug_apply_sphere_subtract(Vector3(24.0 + float(i % 7) * 0.4, 51.5, 24.0), 1.0)
	var before: Array = []
	for i in range(16):
		var p := Vector3(22.0 + float(i) * 0.3, 70.0, 24.0)
		before.append(w.debug_raycast(p, Vector3(0, -1, 0)))
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	for i in range(16):
		var p := Vector3(22.0 + float(i) * 0.3, 70.0, 24.0)
		var after: Dictionary = w.debug_raycast(p, Vector3(0, -1, 0))
		var was: Dictionary = before[i]
		assert_bool(after["hit"]).is_equal(was["hit"])
		if bool(was["hit"]):
			# Within one encoding step of the stored lattice (~5 mm) plus a voxel of
			# reconstruction slack: the bake is a quantisation, not a different world.
			assert_float(float(after["pos"].y)).is_equal_approx(float(was["pos"].y), 0.06)

# A full pool must leave the world exactly as it was.
func test_a_full_pool_refuses_and_changes_nothing(timeout := 60000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.max_override_bricks = 4 # far too small for any real crater
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	var ops_before: int = int(w.debug_op_counts()["region_0_2_0"])
	assert_bool(w.debug_consolidate_region(Vector3i(0, 2, 0))).is_false()
	assert_int(int(w.debug_op_counts()["region_0_2_0"])).is_equal(ops_before)
	assert_int(int(w.debug_stream_stats()["consolidation_refusals"])).is_greater(0)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_consolidation.gd`
Expected: FAIL — `Nonexistent function 'debug_pump_consolidation'`, and `max_override_bricks` is not a property.

- [ ] **Step 3: Add the trigger**

In `VoxelWorld::append_edit_locked` (the path every tool and the demo share), after the append result is known:

```cpp
	// Spec §2's overflow rule, with the escape hatch it always described. 192 of 256 leaves
	// room for the edits that arrive while the bake is in flight -- consolidation is
	// asynchronous, and an edit appended in the meantime must still have somewhere to go.
	for (const ve::IVec3 &r : result.touched)
		if (edit_log_->op_count(r) >= ve::kConsolidateAtOps) consolidation_queue_.push_back(r);
```

Deduplicate on push (a region already queued is not queued twice), and cap the queue at the number of consolidated regions the tables allow (`ve::kMaxOverrideTables`); past that, log once and fall back to M2's behaviour, which is exactly what happens today.

- [ ] **Step 4: Drain it**

`VoxelWorld::pump_consolidation()` runs once per `_process`: submit at most **one** region's bake to `MeshService` when nothing is in flight, then collect finished results and apply them (store, upload, publish tables, `clear_region`, mark for regen, dirty the LoD chain at all levels through the existing `mark_dirty` path). `debug_pump_consolidation()` is the same call, exposed so a test can step it without a SceneTree tick.

A consolidation is applied under `edit_mutex_` in one step: the op list is cleared **only** after every override for that region is in the store and both pools have the bytes. Between submit and apply, the region keeps its full op list and renders exactly as before — the bake is a snapshot, and any edit appended in that window is still in the list when it clears... **which is the one ordering hazard in this task**: clearing the list would also drop ops appended *after* the bake started. So `ConsolidateJob` records `EditLog::seqs(region).back()` at submit time, and the apply step erases only ops at or below that sequence number, keeping the tail. Add `EditLog::clear_region_through(IVec3 region, uint64_t seq)` for it, with a native test for the "ops appended during the bake survive" case in `extension/tests/test_edit_log.cpp`.

- [ ] **Step 5: Export the pool size and report the stats**

`max_override_bricks` follows the existing export pattern (`set_`/`get_`, `ClassDB::bind_method`, property in `_bind_methods`), and `debug_stream_stats()` gains `override_bricks` (`OverrideStore::used()`), `consolidations` and `consolidation_refusals` (monotonic counters). In `demo/hud.gd`, extend the world line:

```gdscript
		s = "regions %d  edits %d  ovf %d  ovr %d/%d  cons %d" % [
			st.get("resident_regions", 0), st.get("frame_edits", 0),
			st.get("overflow_ever", 0), st.get("override_bricks", 0),
			st.get("override_capacity", 0), st.get("consolidations", 0)]
```

- [ ] **Step 6: Run the tests to verify they pass**

Run:
```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
cd extension && scons test && cd ..
./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd
./gdunit_tests.sh -c -a res://tests/test_collider_edits.gd
./gdunit_tests.sh -c -a res://tests/test_lod_stream.gd
```
Expected: PASS. `test_collider_edits` and `test_lod_stream` matter because a consolidation dirties both the collision chunks and the LoD chain: a crater that survives in the render but not in the collider is a hole players fall through.

- [ ] **Step 7: Measure**

Run: `tools/run_benchmarks.sh m7-task8`
Record in **Errata entry 6**: the edit leg's `gpu_stream`, `gpu_raymarch` and `custom_frame` p50/p99 against Task 5's numbers, plus `BENCH regions=... overflow=...` (which should now read `overflow=0`) and the new consolidation counters. Also run the edit leg for 3× the usual frames once (`--benchmark-edit` with `FRAMES` temporarily at 900) and confirm the numbers do not drift upward across the run — the whole claim of this task is that edit cost stops growing with edit count.

- [ ] **Step 8: Commit**

```bash
git add extension/src/voxel_world.h extension/src/voxel_world.cpp \
	extension/src/world/edit_log.h extension/src/world/edit_log.cpp \
	extension/tests/test_edit_log.cpp demo/hud.gd tests/test_consolidation.gd \
	docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md
git commit -m "feat: consolidate a region's op list into override bricks at 192 ops"
```

---

### Task 9: One collision chunk, eight bodies

`docs/todo/opti.md` names this precisely: "The only remaining spike is one fat chunk (16–22k triangles → 20–30 ms), which is atomic and no throttle can divide." `ColliderStreamer` already throttles to `shape_builds_per_frame` and a `build_budget_ms_` ceiling, but a single `shape_set_data` on a 20k-triangle soup is one indivisible Jolt BVH build inside one frame, and the p99 numbers in that file (18.6 ms moving, 24.1 ms editing) are that build.

Splitting a chunk's triangles into eight octant bodies by **centroid** keeps the triangle soup identical — same triangles, same winding, no seams, no duplicates, no shared vertices to reconcile — while making the work divisible: eight builds of ~2.5k triangles each, spread across frames by the throttle that already exists.

**Files:**
- Create: `extension/src/mesh/octant_split.h`, `octant_split.cpp`
- Create: `extension/tests/test_octant_split.cpp`
- Modify: `extension/src/physics/collider_streamer.h`, `collider_streamer.cpp`
- Modify: `tests/test_collider_stream.gd` (the `active_bodies` redefinition)
- Test: `tests/test_collider_octants.gd`

**Interfaces:**
- Consumes: `godot::MeshResult` (`mesh_pass.h:33` — `chunk`, `positions`, `indices`, `overflow`, `failed`).
- Produces:
  - `inline constexpr int ve::kColliderOctants = 8;`
  - `int ve::octant_of(const float centroid[3], const float chunk_center[3])` — 0…7, bit 0 = +x, bit 1 = +y, bit 2 = +z.
  - `void ve::split_octants(const float *positions, const uint32_t *indices, int index_count, const float chunk_center[3], std::vector<uint32_t> out[ve::kColliderOctants])` — every input triangle appears in exactly one output bin, with its vertex order preserved.
  - `ColliderStreamer::active_bodies()` now counts **chunks in the space**, not bodies (the existing meaning every test asserts against); `bodies_in_space()` is the new raw count.

- [x] **Step 1: Write the failing native test**

Create `extension/tests/test_octant_split.cpp`:

```cpp
#include <doctest/doctest.h>
#include "mesh/octant_split.h"
#include <set>

TEST_CASE("octant_of separates the eight corners") {
	const float c[3] = {0.0f, 0.0f, 0.0f};
	std::set<int> seen;
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f};
		seen.insert(ve::octant_of(p, c));
	}
	CHECK(seen.size() == 8);
}

TEST_CASE("every triangle lands in exactly one bin, with its winding intact") {
	// Two triangles of a quad straddling the centre. The split must not drop one, duplicate
	// one, or reverse one: a reversed triangle is a one-sided collider facing into the rock,
	// and a player walks through it (M3 errata 1's lesson, in a new place).
	const float pos[] = {
		-1.0f, 0.0f, -1.0f,   1.0f, 0.0f, -1.0f,   1.0f, 0.0f, 1.0f,   -1.0f, 0.0f, 1.0f,
	};
	const uint32_t idx[] = {0, 1, 2, 0, 2, 3};
	const float c[3] = {0.0f, 0.0f, 0.0f};
	std::vector<uint32_t> bins[ve::kColliderOctants];
	ve::split_octants(pos, idx, 6, c, bins);

	int total = 0;
	for (const std::vector<uint32_t> &b : bins) {
		CHECK(b.size() % 3 == 0);
		total += static_cast<int>(b.size());
	}
	CHECK(total == 6);

	// Reconstruct the multiset of triangles and compare with the input, order within each
	// triangle included.
	std::multiset<std::string> want, got;
	const auto key = [](uint32_t a, uint32_t b, uint32_t c2) {
		return std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(c2);
	};
	for (int t = 0; t < 6; t += 3) want.insert(key(idx[t], idx[t + 1], idx[t + 2]));
	for (const std::vector<uint32_t> &b : bins)
		for (size_t t = 0; t + 2 < b.size(); t += 3) got.insert(key(b[t], b[t + 1], b[t + 2]));
	CHECK(want == got);
}

TEST_CASE("an empty bin is empty, not absent") {
	// All eight bins always exist; the streamer indexes them by octant and must not have to
	// think about which ones a chunk happened to fill.
	const float pos[] = {0.1f, 0.1f, 0.1f, 0.2f, 0.1f, 0.1f, 0.2f, 0.2f, 0.1f};
	const uint32_t idx[] = {0, 1, 2};
	const float c[3] = {0.0f, 0.0f, 0.0f};
	std::vector<uint32_t> bins[ve::kColliderOctants];
	ve::split_octants(pos, idx, 3, c, bins);
	int nonempty = 0;
	for (const std::vector<uint32_t> &b : bins) if (!b.empty()) nonempty++;
	CHECK(nonempty == 1);
}
```

- [x] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `mesh/octant_split.h: No such file or directory`.

- [x] **Step 3: Write the split**

`octant_split.cpp` is a single pass: for each triangle, average its three vertex positions, compare each axis against the chunk centre, and append the three indices to that bin. No sorting, no vertex remapping — bins hold indices into the *original* position array, and the streamer de-indexes them exactly as it does today.

- [x] **Step 4: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

- [x] **Step 5: Write the failing gdUnit test**

Create `tests/test_collider_octants.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 512
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, center: Vector3, frames := 6000) -> bool:
	var quiet := 0
	for i in range(frames):
		w.debug_physics_frame(center)
		var st: Dictionary = w.debug_physics_stats()
		quiet = quiet + 1 if st["chunks_pending"] == 0 and st["queued"] == 0 else 0
		if quiet >= 4:
			return true
	return false

# The ground must still hold the player up. This is the same oracle test_collider_stream.gd
# uses: the physics ray and ve::raycast agree to a few centimetres, whether the chunk is one
# body or eight.
func test_split_colliders_still_match_the_field(timeout := 120000) -> void:
	var w := make_world()
	var center := Vector3(60.0, 55.0, 60.0)
	assert_bool(settle(w, center)).is_true()
	for i in range(8):
		var x := 55.0 + float(i)
		var from := Vector3(x, 70.0, 60.0)
		var hit := get_tree().root.world_3d.direct_space_state.intersect_ray(
			PhysicsRayQueryParameters3D.create(from, from + Vector3(0, -30, 0)))
		var truth: Dictionary = w.debug_raycast(from, Vector3(0, -1, 0))
		assert_bool(hit.is_empty()).is_false()
		if not hit.is_empty() and bool(truth["hit"]):
			assert_float(float(hit["position"].y)).is_equal_approx(float(truth["pos"].y), 0.15)

# The point of the split: no single build is the whole chunk any more.
func test_no_single_build_carries_the_whole_chunk(timeout := 120000) -> void:
	var w := make_world()
	assert_bool(settle(w, Vector3(60.0, 55.0, 60.0))).is_true()
	var st: Dictionary = w.debug_physics_stats()
	assert_int(int(st["max_build_tris"])).is_less(int(st["max_chunk_tris"]))
	assert_int(int(st["max_build_tris"])).is_greater(0)
```

- [x] **Step 6: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd`
Expected: FAIL — `max_build_tris` is not in `debug_physics_stats`.

- [x] **Step 7: Give each slot eight bodies**

In `ColliderStreamer`, `bodies_`, `shapes_` and `in_space_` become `slot * kColliderOctants + octant`-indexed (keep them as flat vectors sized `max_slots * 8` — the indexing helper is `int sub(int slot, int octant) { return slot * ve::kColliderOctants + octant; }`). `build_shape(slot, result)` becomes:

```cpp
	// One chunk, eight builds. The soup is identical -- split_octants bins whole triangles
	// by centroid, so no triangle is duplicated, dropped or reversed -- but Jolt now builds
	// eight small BVHs instead of one 20k-triangle one, and the per-frame budget can stop
	// between any two of them (docs/todo/opti.md).
	std::vector<uint32_t> bins[ve::kColliderOctants];
	float centre[3];
	ve::chunk_world_center(r.chunk, centre);
	ve::split_octants(r.positions.data(), r.indices.data(),
			static_cast<int>(r.indices.size()), centre, bins);
```

then the existing de-index-and-swap-winding loop runs once per non-empty bin, with the empty bins releasing any body that slot's octant used to hold. A queue of pending octant builds per slot lets `run_frame`'s existing `build_budget_ms_` stop mid-chunk; a chunk that is half-built keeps its *previous* bodies in the space until every octant of the new mesh is ready, so the player never stands on a half-replaced collider.

`active_bodies()` keeps its old meaning — **chunks with at least one body in the space** — because `tests/test_collider_stream.gd` asserts against it as a chunk count. Add `bodies_in_space()` for the raw number and print both in `debug_physics_stats()` (`bodies`, `bodies_raw`), plus `max_build_tris` (largest single `shape_set_data` this streamer has performed) and `max_chunk_tris` (largest whole-chunk triangle count) for the test above.

- [x] **Step 8: Run the physics suites**

Run:
```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd
./gdunit_tests.sh -c -a res://tests/test_collider_stream.gd
./gdunit_tests.sh -c -a res://tests/test_collider_edits.gd
./gdunit_tests.sh -c -a res://tests/test_player_kick.gd
./gdunit_tests.sh -c -a res://tests/test_island_body.gd
```
Expected: PASS for all five.

- [x] **Step 9: Measure**

Run: `tools/run_benchmarks.sh m7-task9`
Record in **Errata entry 7**: `p99`, `max`, `over_16.6ms` and `BENCH max_ms build_ms` for the **move** and **edit** legs against Task 8's numbers, and quote `docs/todo/opti.md`'s 18.6 / 24.1 ms figures beside them. Then update `docs/todo/opti.md`: strike the octant-split paragraph and record the measured outcome underneath it.

- [x] **Step 10: Commit**

```bash
git add extension/src/mesh/octant_split.h extension/src/mesh/octant_split.cpp \
	extension/tests/test_octant_split.cpp extension/src/physics/collider_streamer.h \
	extension/src/physics/collider_streamer.cpp extension/src/voxel_world.cpp \
	tests/test_collider_octants.gd docs/todo/opti.md \
	docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md
git commit -m "perf: split each collision chunk into eight octant bodies"
```

---

### Task 10: Occupancy from the lattice that was generated, not from 27 probes

The second item in `docs/todo/opti.md`: occupancy is written by `brick_mark.comp.glsl` from a 3×3×3 probe with a 0.15 m activation pad, which is a *conservative estimate* of what a brick contains. The brick that then gets generated knows the answer exactly — `brick_gen.comp.glsl` already reduces the true 5 cm lattice into `s_mip8`, and the 2³ reduction at the end of Task 3 already computes the whole-brick min/max for the flag word. Connectivity, island extraction and the physics bubble all read that grid, so a phantom-solid cell is a phantom island anchor.

The split is clean: **generated bricks write their own occupancy** (exact, from the lattice), and the mark pass writes occupancy **only for bricks it decides have no surface** (unambiguous — the probe's `mn > 0` and `mx <= 0` cases are exactly air and exactly full).

**Files:**
- Modify: `shaders/brick_gen.comp.glsl` (write occupancy from the reduction)
- Modify: `shaders/brick_mark.comp.glsl` (write only for `!has_surface`)
- Modify: `extension/src/render/brick_gen_pass.cpp` (bind the occupancy buffer + the region slot in the job)
- Modify: `extension/src/world/brick_eval.cpp` (`cell_state_field` mirrors the new rule)
- Test: `tests/test_occupancy_lattice.gd`, and `extension/tests/test_brick_eval.cpp`

**Interfaces:**
- Consumes: `GpuAtlas::region_occupancy()`; the jobs buffer's second word already carries `rslot` (`jobs.v[j * 2 + 1] = ivec4(rslot, op_count, 0, 0)`), and the first carries the brick coord and slot — everything the write needs is already in the job.
- Produces: no new API. `ve::cell_state_field(gen, ops, n, cell, volumes, overrides)` gains the exact-lattice rule so the CPU reference and `tests/test_occupancy.gd` still agree.

- [ ] **Step 1: Write the failing test**

Create `tests/test_occupancy_lattice.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

# A thin carved slot between two walls is the case the 27-sample probe cannot see: every
# probe sample lands in rock, the lattice says otherwise, and the difference is a phantom
# anchor that keeps a severed piece hanging in the air.
func test_thin_carve_is_air_in_the_occupancy_grid(timeout := 60000) -> void:
	var w := make_world()
	var c := Vector3(24.4, 51.2, 24.4)
	w.debug_apply_sphere_subtract(c, 0.35) # smaller than the probe's 0.4 m sample spacing
	w.debug_pump_occupancy() # drain the readback ring into the grid
	var cell := Vector3i(int(floor(c.x / 0.8)), int(floor(c.y / 0.8)), int(floor(c.z / 0.8)))
	assert_int(int(w.debug_occupancy_state(cell))).is_not_equal(3) # not kCellFull

# The exactness claim, stated as a diff: for every resident brick of a region, the GPU's
# occupancy byte equals ve::cell_state_field on the same field.
func test_gpu_occupancy_matches_the_cpu_rule(timeout := 60000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 1.5)
	w.debug_pump_occupancy()
	var d: Dictionary = w.debug_occupancy_diff(Vector3i(0, 2, 0))
	assert_int(int(d["compared"])).is_greater(100)
	assert_int(int(d["mismatches"])).is_equal(0)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_occupancy_lattice.gd`
Expected: FAIL — either the helper is missing or the thin carve reads as `kCellFull`.

- [ ] **Step 3: Write occupancy from the generator**

In `brick_gen.comp.glsl`, in the same `tid == 0` block Task 3 added (the whole-brick min/max is already in hand):

```glsl
		// Spec §5's occupancy grid, written from the lattice this dispatch just produced.
		// The mark pass' 3^3 probe is a conservative ESTIMATE; this is the answer. Same two
		// bits, same packing, same buffer -- only the source changes.
		uint state = mn > ENCODED_ZERO ? CELL_AIR : (mx <= ENCODED_ZERO ? CELL_FULL : CELL_SOLID);
		int occ_word = rslot * OCC_WORDS_PER_REGION + (bi >> 4);
		uint occ_shift = (uint(bi) & 15u) * 2u;
		atomicAnd(occupancy.w[occ_word], ~(3u << occ_shift));
		atomicOr(occupancy.w[occ_word], (state & 3u) << occ_shift);
```

`bi` is the brick's index inside its region — the same `(x & 31) + (y & 31) * 32 + (z & 31) * 1024` the mark pass computes; compute it once from the job's brick coordinate. Bind `region_occupancy` into `BrickGenPass`'s uniform set at the next free binding and declare it in the shader with the same `OCC_WORDS_PER_REGION` constant the mark pass uses (move that constant into `common.glslh` so the two cannot drift).

- [ ] **Step 4: Narrow the mark pass' write**

In `brick_mark.comp.glsl`, replace the unconditional allocate-phase write with:

```glsl
	// Only the unambiguous case. A brick WITH a surface is about to be generated, and the
	// generator writes the exact answer from its own lattice; writing an estimate here first
	// would be a one-frame lie that connectivity can act on.
	if (pc.cfg.y == 1 && !has_surface)
		write_occupancy(rslot, bi, probe_mn > 0.0 ? CELL_AIR : CELL_FULL);
```

Note the dropped middle case: `has_surface == false` means the probe found no crossing, so `probe_mn > 0` is air and everything else is full. A brick that IS generated gets its cell written by `brick_gen` in the same frame's compute list, after the mark — so there is no window where a newly resident brick has no occupancy at all, only one where it has the *previous* occupant's, which is what `OccupancyBlock`'s `seq` stamp already guards against.

- [ ] **Step 5: Mirror the rule on the CPU**

`ve::cell_state_field` currently mirrors the probe. It becomes: evaluate the brick's lattice (the same `eval_brick` the differential test uses), reduce min/max, and classify with the same three-way test. Keep the probe-based path as `cell_state_probe` — `tests/test_occupancy.gd` and the flood-fill unit tests reference the old semantics in places, and the mark pass still uses it for the no-surface case.

- [ ] **Step 6: Add the two debug helpers**

`debug_pump_occupancy()` drains the readback ring into `ve::OccupancyGrid` (the existing `drain_occupancy()` under a bound method), `debug_occupancy_state(Vector3i cell)` returns the grid's byte, and `debug_occupancy_diff(Vector3i region)` compares every resident brick's GPU byte against `ve::cell_state_field`.

- [ ] **Step 7: Run the connectivity suites**

Run:
```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_occupancy_lattice.gd
./gdunit_tests.sh -c -a res://tests/test_occupancy.gd
./gdunit_tests.sh -c -a res://tests/test_connectivity.gd
./gdunit_tests.sh -c -a res://tests/test_repro_thin_sheet.gd
./gdunit_tests.sh -c -a res://tests/test_repro_pillar_debris.gd
./gdunit_tests.sh -c -a res://tests/test_island_body.gd
cd extension && scons test && cd ..
```
Expected: PASS for all. The two `repro` suites are the ones that exist because of this class of bug; if either changes behaviour, that is the finding, and it goes in the Errata whichever direction it went.

- [ ] **Step 8: Commit**

```bash
git add shaders/brick_gen.comp.glsl shaders/brick_mark.comp.glsl shaders/common.glslh \
	extension/src/render/brick_gen_pass.cpp extension/src/world/brick_eval.h \
	extension/src/world/brick_eval.cpp extension/src/voxel_world.h extension/src/voxel_world.cpp \
	tests/test_occupancy_lattice.gd docs/todo/opti.md
git commit -m "fix: occupancy comes from the generated lattice, not a 27-sample probe"
```

---

### Task 11: A demo a stranger can drive

Everything the demo can do is bound to a key nobody is told about. `R` drills, the middle mouse button paints, `F` toggles flight, `F1` opens the beauty menu, and the radius is a constant in an exported property. For a portfolio piece the controls are part of the work: a viewer who cannot find the drill never sees the feature that severs an overhang.

**Files:**
- Create: `demo/help.gd`
- Modify: `demo/edit_tool.gd` (tool slots, radius wheel, active-tool state), `demo/hud.gd` (modes + reticle), `demo/player.gd` (pause), `demo/main.tscn`
- Test: `tests/test_demo_shell.gd`

**Interfaces:**
- Produces:
  - `EditTool.active_tool: int` (0 subtract, 1 add, 2 paint, 3 drill), `EditTool.radius: float` (0.5…8.0), `EditTool.tool_name() -> String`
  - `Hud.mode: int` (0 full, 1 compact, 2 hidden) and `Hud.set_mode(int)`
  - `Help` panel: `visible` toggled by `F2`, built from one `CONTROLS` table so the text and the bindings cannot drift.

- [ ] **Step 1: Write the failing test**

Create `tests/test_demo_shell.gd`:

```gdscript
extends GdUnitTestSuite

const TOOL_SCRIPT := preload("res://demo/edit_tool.gd")
const HELP_SCRIPT := preload("res://demo/help.gd")
var _roots: Array = []

func after_test() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.free()
	_roots.clear()

func make_tool() -> Node:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	root.add_child(world)
	var cam := Camera3D.new()
	cam.name = "Cam"
	root.add_child(cam)
	var tool_node := Node.new()
	tool_node.name = "Tool"
	tool_node.set_script(TOOL_SCRIPT)
	tool_node.set("world_path", NodePath("../World"))
	tool_node.set("camera_path", NodePath("../Cam"))
	root.add_child(tool_node)
	return tool_node

func test_number_keys_select_tools() -> void:
	var t := make_tool()
	await get_tree().process_frame
	for i in range(4):
		var ev := InputEventKey.new()
		ev.keycode = KEY_1 + i
		ev.pressed = true
		t.select_tool_from_key(ev)
		assert_int(int(t.get("active_tool"))).is_equal(i)

func test_wheel_changes_radius_within_bounds() -> void:
	var t := make_tool()
	await get_tree().process_frame
	t.set("radius", 8.0)
	for i in range(10):
		t.adjust_radius(1)
	assert_float(float(t.get("radius"))).is_less_equal(8.0)
	for i in range(30):
		t.adjust_radius(-1)
	assert_float(float(t.get("radius"))).is_greater_equal(0.5)

func test_help_lists_every_binding_the_demo_uses() -> void:
	var help := Control.new()
	help.set_script(HELP_SCRIPT)
	add_child(help)
	_roots.append(help)
	await get_tree().process_frame
	var text: String = help.help_text()
	for key in ["W", "F", "F1", "F2", "F3", "F4", "F5", "F6", "F12", "P", "1", "4", "Wheel"]:
		assert_str(text).contains(key)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_demo_shell.gd`
Expected: FAIL — `res://demo/help.gd` does not exist.

- [ ] **Step 3: Give the edit tool slots and a radius**

Rewrite `demo/edit_tool.gd`'s input handling around an active tool:

```gdscript
enum Tool { SUBTRACT, ADD, PAINT, DRILL }

const TOOL_NAMES := ["Carve", "Fill", "Paint", "Drill"]
const RADIUS_MIN := 0.5
const RADIUS_MAX := 8.0
const RADIUS_STEP := 1.25

var active_tool := Tool.SUBTRACT

# Split out of _unhandled_input so a test can drive it without a viewport, and so the two
# input paths (key event, wheel event) each have exactly one implementation.
func select_tool_from_key(event: InputEventKey) -> bool:
	if not event.pressed or event.echo:
		return false
	var index := event.keycode - KEY_1
	if index < 0 or index >= TOOL_NAMES.size():
		return false
	active_tool = index
	return true

func adjust_radius(notches: int) -> void:
	radius = clampf(radius * pow(RADIUS_STEP, float(notches)), RADIUS_MIN, RADIUS_MAX)

func tool_name() -> String:
	return TOOL_NAMES[active_tool]
```

`_unhandled_input` then routes: number keys through `select_tool_from_key`, `MOUSE_BUTTON_WHEEL_UP/DOWN` through `adjust_radius(+1/-1)`, and the left button through a `match active_tool` that calls the four existing apply paths (`apply_sphere_subtract` + `_kick`, `apply_sphere_add`, `apply_sphere_paint`, `_drill`). Keep the right and middle buttons bound to fill and paint as shortcuts — muscle memory from the M4 demo — and keep `R` on the drill.

- [ ] **Step 4: Write the help overlay**

`demo/help.gd` is a `Control` that builds its text from one table, so a binding cannot be listed wrong:

```gdscript
extends Control

const CONTROLS := [
	["Move", "W A S D"],
	["Fly up / down", "E / Q  (hold Shift to boost)"],
	["Fly / walk", "F"],
	["Jump", "Space"],
	["Fire tool", "Left mouse"],
	["Select tool", "1 Carve   2 Fill   3 Paint   4 Drill"],
	["Tool radius", "Mouse wheel"],
	["Drill (shortcut)", "R"],
	["Beauty menu", "F1"],
	["This help", "F2"],
	["Raymarch cost view", "F3"],
	["HUD detail", "F4"],
	["Reload shaders", "F5"],
	["Run self-check", "F6"],
	["Screenshot", "F12"],
	["Pause", "P"],
	["Release mouse", "Esc"],
]

func help_text() -> String:
	var lines := PackedStringArray(["Voxel Everything — controls", ""])
	for row in CONTROLS:
		lines.append("%-20s %s" % [row[0], row[1]])
	return "\n".join(lines)
```

It starts **visible** and hides on the first `F2` or after 8 seconds, whichever comes first — a viewer who launches the demo should not have to guess that a help key exists.

- [ ] **Step 5: Give the HUD modes and a reticle**

`demo/hud.gd` gains `mode` (full / compact / hidden), cycled by `F4`. Compact prints one line (`fps, frame ms, the active tool and radius`); hidden prints nothing, which is the mode the capture rig uses. Add a reticle: a `Control` sibling that draws a 6 px cross at the viewport centre plus a circle whose screen radius tracks the tool's world radius at the aimed distance (`radius / hit_distance * viewport_height / (2 * tan_half_fov_y)`), so the player can see what a blast will take before firing. Guard the divide: no hit, or a distance under 0.1 m, draws the cross alone.

- [ ] **Step 6: Pause**

`P` toggles `get_tree().paused`, and the player sets `process_mode = PROCESS_MODE_ALWAYS` on the HUD and help overlay so they stay readable while paused. The `VoxelWorld` node keeps ticking — streaming and consolidation are not gameplay, and a paused world that stops streaming looks broken while the camera can still be moved by the capture rig.

- [ ] **Step 7: Wire the scene**

Add the `Help` node (`CanvasLayer/Help`) and the `Reticle` node to `demo/main.tscn`, both anchored full-rect, and point the HUD at the edit tool so it can name the active tool.

- [ ] **Step 8: Run the tests**

Run:
```bash
./gdunit_tests.sh -c -a res://tests/test_demo_shell.gd
./gdunit_tests.sh -c -a res://tests/test_debug_menu.gd
./gdunit_tests.sh -c -a res://tests/test_player_kick.gd
```
Expected: PASS for all three.

- [ ] **Step 9: Drive it by hand**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
Check, and note anything that misbehaves in the Errata: the help panel appears and dismisses; 1–4 change the reticle size and the HUD's tool name; the wheel clamps at both ends; F4 cycles three HUD states; P freezes the character while the terrain keeps streaming.

- [ ] **Step 10: Commit**

```bash
git add demo/edit_tool.gd demo/help.gd demo/hud.gd demo/player.gd demo/main.tscn \
	tests/test_demo_shell.gd
git commit -m "feat: demo shell — tool slots, radius wheel, reticle, help overlay, pause"
```

---

### Task 12: The two dev-build promises in spec §8

Spec §8 asks for a "hot-reload keybind in dev builds" next to the shader tree, and for GPU differential testing to be reachable as a "dev console command [that] runs both and diffs". Both exist as capabilities and neither has a key. The reload is nearly free because `VoxelWorld` already supports the cycle: `teardown_gpu()` frees every GPU object while the CPU cores — edit log, residency, overrides — survive, and `ensure_initialized()` rebuilds from them ("a re-init re-streams the same world, edits included", `voxel_world.h:142`).

**Files:**
- Create: `demo/dev_tools.gd`
- Modify: `extension/src/voxel_world.h`, `voxel_world.cpp` (`request_shader_reload`, `debug_self_check`)
- Modify: `extension/src/raymarch_compositor.cpp` (honour the reload latch on the render thread)
- Modify: `demo/main.tscn`
- Test: `tests/test_shader_reload.gd`, `tests/test_self_check.gd`

**Interfaces:**
- Produces:
  - `VoxelWorld::request_shader_reload()` — sets a latch; the next render callback tears the GPU objects down and rebuilds them **on the render thread**, before any pass runs. Returns immediately; safe to call from `_input`.
  - `VoxelWorld::debug_shader_reload_stats() -> Dictionary` — `{reloads, last_ok, last_error}`.
  - `VoxelWorld::debug_self_check() -> Dictionary` — runs the CPU-vs-GPU diffs the gdUnit suites run (`field`, `brick`, `mesh`, `lod`, `occupancy`) against the world's current state and returns `{field_mismatches, brick_mismatches, mesh_mismatches, lod_mismatches, occupancy_mismatches, elapsed_ms, ok}`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_shader_reload.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

# A reload must leave a working world behind, not merely survive: the same probe answers the
# same way before and after, because the CPU cores that describe the world never went away.
func test_reload_keeps_the_world(timeout := 60000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 1.5)
	var before: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	w.request_shader_reload()
	w.debug_pump_shader_reload() # what the render callback does
	assert_bool(w.is_initialized()).is_true()
	assert_int(int(w.debug_shader_reload_stats()["reloads"])).is_equal(1)
	assert_bool(w.debug_shader_reload_stats()["last_ok"]).is_true()
	var after: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(after["hit"]).is_equal(before["hit"])

# A shader that will not compile must leave the previous pipelines running (spec §8's
# fail-soft rule), not a black screen.
func test_a_broken_reload_keeps_the_old_pipelines(timeout := 60000) -> void:
	var w := make_world()
	w.debug_set_shader_override("raymarch.comp.glsl", "#version 460\nthis is not glsl\n")
	w.request_shader_reload()
	w.debug_pump_shader_reload()
	assert_bool(w.debug_shader_reload_stats()["last_ok"]).is_false()
	assert_bool(w.is_initialized()).is_true()
	var d: Dictionary = w.debug_raymarch_cost_probe(Vector3(24.0, 70.0, 24.0), Vector3(0, -1, 0))
	assert_bool(d["hit"]).is_true()
```

and `tests/test_self_check.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func test_self_check_reports_zero_mismatches_on_a_clean_world(timeout := 120000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 1.5)
	var d: Dictionary = w.debug_self_check()
	assert_bool(d["ok"]).is_true()
	for key in ["field_mismatches", "brick_mismatches", "mesh_mismatches",
			"lod_mismatches", "occupancy_mismatches"]:
		assert_int(int(d[key])).is_equal(0)
```

- [ ] **Step 2: Run them to verify they fail**

Run: `./gdunit_tests.sh -c -a res://tests/test_shader_reload.gd && ./gdunit_tests.sh -c -a res://tests/test_self_check.gd`
Expected: FAIL — neither method exists.

- [ ] **Step 3: Implement the reload**

```cpp
void VoxelWorld::request_shader_reload() {
	// A latch, not the work: shaders are compiled and pipelines created on the device that
	// owns them, and for the shipping world that device belongs to the render thread.
	reload_requested_.store(true, std::memory_order_release);
}

void VoxelWorld::pump_shader_reload() {
	if (!reload_requested_.exchange(false, std::memory_order_acq_rel)) return;
	reload_stats_.reloads++;
	// teardown_gpu() frees every GPU object and leaves the CPU cores -- edit log, residency,
	// override store, LoD tree -- untouched, so ensure_initialized() re-streams the same
	// world (voxel_world.h's note on teardown_gpu). This is the whole hot reload.
	teardown_gpu();
	ensure_initialized();
	reload_stats_.last_ok = initialized_;
	if (!initialized_) {
		// Fail-soft: a compile error must not leave a dead world. Retry once from the
		// last-known-good source; if that fails too, log and stay down rather than looping.
		UtilityFunctions::printerr("VoxelWorld: shader reload failed; see the log above");
	}
}
```

Call `pump_shader_reload()` at the very top of `RaymarchCompositor::_render_callback`, before any pass pointer is read, and expose `debug_pump_shader_reload()` so a test can step it.

The "keep the old pipelines" guarantee needs one addition: `teardown_gpu()` must not run until the new sources are known to compile. Add a pre-flight in `pump_shader_reload` that calls `ve::load_shader_source` + `rd->shader_compile_spirv_from_source` for every `.glsl` under `shaders/` and only proceeds when all of them return non-empty SPIR-V. `debug_set_shader_override(name, source)` (a test-only map consulted by `ve::load_shader_source`) is what lets the second test above inject a broken shader without touching a file.

- [ ] **Step 4: Implement the self-check**

`debug_self_check()` calls the diff hooks that already exist — `debug_field_diff`-equivalent sampling, `debug_brick_diff` on a handful of resident bricks, `debug_mesh_diff` on the chunk under the camera, `debug_lod_diff` on the chunk under the camera at level 2, `debug_occupancy_diff` on the camera's region — sums their mismatch counts, and returns them with `ok = (every count == 0)`. It is the same machinery the gdUnit suites use; the point is that a *running demo* can answer "has anything drifted?" without a test harness, which is what spec §8 asks for.

- [ ] **Step 5: Bind the keys**

`demo/dev_tools.gd`:

```gdscript
extends Node
# Spec §8's two dev-build affordances: a shader hot-reload keybind, and a differential
# self-check that runs the CPU and GPU paths and diffs them.

@export var world_path: NodePath
var _world: VoxelWorld

func _ready() -> void:
	_world = get_node_or_null(world_path) as VoxelWorld

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo or _world == null:
		return
	match event.keycode:
		KEY_F5:
			_world.request_shader_reload()
			print("shader reload requested")
			get_viewport().set_input_as_handled()
		KEY_F6:
			var d: Dictionary = _world.debug_self_check()
			print("SELF-CHECK ok=%s field=%d brick=%d mesh=%d lod=%d occ=%d (%.1f ms)" % [
				str(d["ok"]), d["field_mismatches"], d["brick_mismatches"],
				d["mesh_mismatches"], d["lod_mismatches"], d["occupancy_mismatches"],
				d["elapsed_ms"]])
			get_viewport().set_input_as_handled()
		KEY_F12:
			var image := get_viewport().get_texture().get_image()
			var path := "user://shot_%d.png" % Time.get_ticks_msec()
			image.save_png(path)
			print("screenshot: %s" % ProjectSettings.globalize_path(path))
			get_viewport().set_input_as_handled()
```

Add the node to `demo/main.tscn` and list `F5`/`F6`/`F12` in the help table (Task 11 already does).

- [ ] **Step 6: Run the tests**

Run:
```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_shader_reload.gd
./gdunit_tests.sh -c -a res://tests/test_self_check.gd
./gdunit_tests.sh -c -a res://tests/test_render_shutdown.gd
```
Expected: PASS for all three. `test_render_shutdown` is here because the reload and the shutdown latch share `teardown_gpu`: a reload must not be able to resurrect a world that has begun shutting down.

- [ ] **Step 7: Commit**

```bash
git add extension/src/voxel_world.h extension/src/voxel_world.cpp \
	extension/src/raymarch_compositor.cpp extension/src/render/shader_loader.h \
	extension/src/render/shader_loader.cpp demo/dev_tools.gd demo/main.tscn \
	tests/test_shader_reload.gd tests/test_self_check.gd
git commit -m "feat: shader hot reload and differential self-check keybinds"
```

---

### Task 13: The capture rig

Spec §8's last line: the benchmark scene "doubles as the portfolio capture rig". The benchmark drives the camera from `delta`, which makes every run a different path and every recording frame-rate-dependent — fine for percentiles, useless for a video. The capture leg drives the camera from the **frame index** at a fixed 1/60 s step and writes a PNG per frame, so the output is identical whether the engine renders it at 90 fps or 12, and the encode is a separate offline step.

**Files:**
- Create: `demo/capture.gd`, `tools/encode_capture.sh`
- Modify: `demo/main.tscn` (the Capture node), `demo/hud.gd` (hidden mode), `demo/benchmark.gd` (leave it alone except for the shared leg constants)
- Test: `tests/test_capture.gd`

**Interfaces:**
- Produces:
  - `--capture` command-line leg: 900 frames, HUD hidden, deterministic camera, PNGs to `user://capture/`.
  - `Capture.camera_at(frame: int) -> Transform3D` — pure function of the frame index, testable without a GPU.
  - `Capture.events_at(frame: int) -> Array` — the canned destruction script: `[["subtract", Vector3, radius], ...]`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_capture.gd`:

```gdscript
extends GdUnitTestSuite

const CAPTURE_SCRIPT := preload("res://demo/capture.gd")
var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

func make_capture() -> Node:
	var n := Node.new()
	n.set_script(CAPTURE_SCRIPT)
	add_child(n)
	_nodes.append(n)
	return n

# Determinism is the whole feature: the same frame index gives the same camera, every run,
# on every machine, at any frame rate.
func test_camera_is_a_pure_function_of_the_frame() -> void:
	var c := make_capture()
	for f in [0, 137, 448, 899]:
		var a: Transform3D = c.camera_at(f)
		var b: Transform3D = c.camera_at(f)
		assert_vector(a.origin).is_equal(b.origin)
	assert_vector(c.camera_at(0).origin).is_not_equal(c.camera_at(300).origin)

# The path has to stay inside the world and above the ground, or the capture is 900 frames
# of the inside of a hill.
func test_path_stays_in_bounds_and_above_the_terrain() -> void:
	var c := make_capture()
	for f in range(0, 900, 25):
		var p: Vector3 = c.camera_at(f).origin
		assert_float(p.x).is_between(8.0, 4088.0)
		assert_float(p.z).is_between(8.0, 4088.0)
		assert_float(p.y).is_greater(c.terrain_height(p.x, p.z) + 1.0)

# The destruction script fires at known frames, so a recut of the video can find the beats.
func test_events_are_scheduled_and_bounded() -> void:
	var c := make_capture()
	var total := 0
	for f in range(900):
		total += c.events_at(f).size()
	assert_int(total).is_greater(4)
	assert_int(total).is_less(40)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -c -a res://tests/test_capture.gd`
Expected: FAIL — `res://demo/capture.gd` does not exist.

- [ ] **Step 3: Write the rig**

`demo/capture.gd`:

```gdscript
extends Node
# Portfolio capture (spec §8: the benchmark scene "doubles as the portfolio capture rig").
#
#   godot --path . --resolution 2560x1440 demo/main.tscn -- --capture
#
# Everything is a function of the FRAME INDEX, never of delta: the run writes the same 900
# images whether the engine manages 90 fps or 12, so a slow frame changes the wall-clock
# length of the capture and nothing else. tools/encode_capture.sh turns them into a video.

const FRAMES := 900
const STEP := 1.0 / 60.0
const WARMUP := 90        # let the near field and the first LoD chunks land before frame 0
const OUT_DIR := "user://capture"

var _active := false
var _frame := -WARMUP
var _world: VoxelWorld
var _player: CharacterBody3D
var _cam: Camera3D
var _tool: VoxelEditTool

func _ready() -> void:
	if not ("--capture" in OS.get_cmdline_user_args()):
		return
	_active = true
	Engine.max_fps = 0
	_world = get_parent().get_node("VoxelWorld")
	_player = get_parent().get_node("Player")
	_cam = _player.get_node("Camera3D")
	_player.set_physics_process(false)
	_player.set_process_unhandled_input(false)
	get_parent().get_node("HUD/Label").set_mode(2)   # hidden
	get_parent().get_node("HUD/Help").visible = false
	_tool = ClassDB.instantiate("VoxelEditTool")
	_world.add_child(_tool)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUT_DIR))

# Mirror of extension/src/generator/generator.cpp and shaders/field.glslh, as
# demo/benchmark.gd already keeps one. Used only to hold the path above the ground.
func terrain_height(x: float, z: float) -> float:
	return 51.2 + (
			6.0 * sin(x * 0.11) * cos(z * 0.13)
			+ 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043)
			+ 1.0 * sin(x * 0.23 + z * 0.19))

func camera_at(frame: int) -> Transform3D:
	# A slow arc across a ridge, dropping toward the ground as it goes: the near field fills
	# the screen at the end, the far field carries the start, and the seam crosses the middle
	# of the shot -- which is the thing this engine is for.
	var t := float(clampi(frame, 0, FRAMES)) / float(FRAMES)
	var angle := 0.9 + t * 1.1
	var radius := 120.0 - t * 85.0
	var cx := 480.0
	var cz := 340.0
	var p := Vector3(cx + cos(angle) * radius, 0.0, cz + sin(angle) * radius)
	p.y = terrain_height(p.x, p.z) + 26.0 - t * 18.0
	var look := Vector3(cx, terrain_height(cx, cz) + 2.0, cz)
	return Transform3D(Basis.looking_at(look - p, Vector3.UP), p)

func events_at(frame: int) -> Array:
	# The canned destruction: four blasts on the ridge line, spaced so each one's island has
	# time to fall, sleep and re-merge before the next.
	match frame:
		240: return [["subtract", Vector3(480.0, terrain_height(480.0, 340.0) + 1.0, 340.0), 5.0]]
		380: return [["subtract", Vector3(474.0, terrain_height(474.0, 346.0) + 2.0, 346.0), 4.0]]
		520: return [["drill", Vector3(486.0, terrain_height(486.0, 334.0) + 3.0, 334.0), 1.0]]
		660: return [["subtract", Vector3(480.0, terrain_height(480.0, 340.0) + 6.0, 340.0), 6.0]]
		_: return []

func _process(_delta: float) -> void:
	if not _active:
		return
	_frame += 1
	if _frame < 0:
		return                    # warmup: stream, do not record
	if _frame >= FRAMES:
		print("CAPTURE done: %d frames in %s" % [FRAMES, ProjectSettings.globalize_path(OUT_DIR)])
		_world.shutdown_render_resources()
		set_process(false)
		get_tree().create_timer(5.0).timeout.connect(get_tree().quit)
		return

	var xform := camera_at(_frame)
	_player.global_position = xform.origin
	_cam.global_transform = xform
	for e in events_at(_frame):
		match e[0]:
			"subtract": _tool.apply_sphere_subtract(e[1], e[2])
			"drill":
				for i in range(10):
					_tool.apply_sphere_subtract(e[1] + Vector3.DOWN * (0.6 * i), e[2])

	# The image has to be read AFTER the frame it belongs to has been drawn.
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	image.save_png("%s/frame_%05d.png" % [OUT_DIR, _frame])
```

- [ ] **Step 4: Write the encoder**

`tools/encode_capture.sh`:

```bash
#!/usr/bin/env bash
# Turns the PNG sequence demo/capture.gd writes into an H.264 file at a true 60 fps.
#   tools/encode_capture.sh ~/.local/share/godot/app_userdata/<project>/capture out.mp4
set -euo pipefail
DIR="${1:?usage: encode_capture.sh <png-dir> [out.mp4]}"
OUT="${2:-voxel-everything.mp4}"
ffmpeg -y -framerate 60 -i "$DIR/frame_%05d.png" \
	-c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p "$OUT"
echo "wrote $OUT"
```

`chmod +x tools/encode_capture.sh`.

- [ ] **Step 5: Run the tests**

Run: `./gdunit_tests.sh -c -a res://tests/test_capture.gd`
Expected: PASS.

- [ ] **Step 6: Capture and encode**

```bash
/usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything --display-driver x11 \
	--resolution 2560x1440 demo/main.tscn -- --capture
tools/encode_capture.sh ~/.local/share/godot/app_userdata/voxel-everything/capture \
	/tmp/voxel-everything.mp4
```

Watch it. Record in **Errata entry 8**: the frame count written, the encoded file size, and one sentence per defect the video shows (popping at the seam, a blast whose island never falls, the fade band reading as a smear). Defects found here are the input to Task 14's fade-band decision — this is the first time the engine is judged as a *picture* rather than as a percentile.

- [ ] **Step 7: Commit**

```bash
git add demo/capture.gd demo/main.tscn demo/hud.gd tools/encode_capture.sh \
	tests/test_capture.gd docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md
git commit -m "feat: deterministic portfolio capture rig"
```

---

### Task 14: The fade-band verdict, the closing sweep, and the record

Spec §4 leaves one decision explicitly open: "if benchmarking shows the fade band too coarse, densify L1 mesh sampling to 0.2 m within 300 m — decided during implementation, no structural change." M5 shipped without deciding, because the fade band is a thing you look at, and until Task 13 there was nothing to look at it in. This task decides it with a measurement, then runs the final sweep and writes down what this engine does — which is the deliverable a portfolio piece actually hands over.

**Files:**
- Modify: `extension/src/lod/lod_tree.cpp`, `lod_grid.h` (only if the verdict is "densify")
- Create: `docs/PORTFOLIO.md`
- Modify: `docs/todo/opti.md` (close it out), this plan's Errata

**Interfaces:**
- Consumes: `VoxelWorld::debug_lod_fade_band()`, `debug_lod_stats()` (M5 Task 18); `tools/run_benchmarks.sh` (Task 1).
- Produces: `ve::kLodNearDenseRadiusM` (only if the verdict is "densify"; 0 disables, which is the default the decision may keep).

- [ ] **Step 1: Measure the fade band**

The question is how large a level-0 cell is on screen at the fade distance. At 1440p with the demo's FOV, a 0.4 m cell at 150 m subtends roughly `0.4 / 150 * 1440 / (2 * tan_half_fov_y)` pixels — compute it from the running camera rather than from the spec's assumed values, because M5's level table is not §4's:

```bash
/usr/bin/godot --path . --display-driver x11 --resolution 2560x1440 demo/main.tscn -- --benchmark-ridge
```

and read `BENCH lod_summary` plus `debug_lod_fade_band()` from the HUD. Then take two screenshots at the same camera — one normal, one with the near field forced off (`--effects-off` from Task 2, extended to accept `near_field`) — and compare the band.

Record the number and both screenshots in **Errata entry 9**.

- [ ] **Step 2: Decide, in writing, before changing anything**

Write the verdict into Errata entry 9 as one of:

- **"No densification."** The bake and the outlines carry the band; the measured cell size at the fade distance is at or under ~4 px; the capture in Task 13 shows no smear. This is the outcome that costs nothing, and spec §4 explicitly allows it.
- **"Densify."** Cells at the fade distance exceed ~5 px, or the capture shows the band as a visible change in surface character. Then continue to Step 3.

State the number the decision rests on. A verdict with no number is not a verdict.

- [ ] **Step 3 (only if densifying): Clamp the walk near the camera**

In `ve::LodTree`'s walk, a chunk within `kLodNearDenseRadiusM` of the camera descends to level 0 regardless of its projected area:

```cpp
	// Spec §4's fade-band contingency. The screen-space-error test is the right rule
	// everywhere except the band where the near field hands over: there, the eye compares
	// the two fields directly, and the LoD side loses at any error the SSE test tolerates.
	const bool near_dense = kLodNearDenseRadiusM > 0.0f && level > 0 &&
			chunk_distance_m < kLodNearDenseRadiusM;
	if (near_dense || area > kLodSseAreaThresh) { /* descend */ }
```

with a native test in `extension/tests/test_lod_tree.cpp` pinning both directions (inside the radius descends, outside obeys the SSE threshold), and a page-budget check: densifying costs pages, and `ve::LodArena` refuses rather than evicting something on screen, so the test must also assert that the walk under a starved arena still returns a complete set of drawable chunks.

- [ ] **Step 4: Run the whole test suite**

Run:
```bash
./build.sh -j$(nproc)
cd extension && scons test && cd ..
./gdunit_tests.sh -c
```
Expected: every native suite and every gdUnit suite passes. Record the two counts (`N/N native`, `M/M gdUnit across K suites`) — that pair is what this milestone's Acceptance section is checked against.

- [ ] **Step 5: Run the closing sweep**

Run: `tools/run_benchmarks.sh m7-final`
Then run it a second time to confirm the verdicts are stable rather than a lucky pass: `tools/run_benchmarks.sh m7-final-b`.

Record in **Errata entry 10**, in the exact form M6 errata 4 used (it is the format the next reader will compare against):
- The environment line: Godot version, Vulkan version, GPU, driver, quality tier, resolution, display driver, and the **actual** V-Sync mode.
- Every leg's `gpu_raymarch`, `gpu_stream`, `gpu_lod`, `gpu_ssgi`, `gpu_ssr`, `gpu_shadows`, `gpu_outlines`, `gpu_unattributed`, `custom_frame` p50/p99.
- Every leg's `budget_verdict` line.
- The frame percentiles and `over_16.6ms` counts.
- One sentence per remaining WARN saying what the measurement shows and what the next step would be. **Do not soften a budget to turn a WARN into a PASS.**

- [ ] **Step 6: Write the portfolio document**

`docs/PORTFOLIO.md` is the page a reader who is not going to read 1.4 MB of plans gets. Keep it to one screen of prose plus tables:

```markdown
# Voxel Everything

Destructible smooth-SDF voxel terrain in Godot 4.7 — 5 cm voxels, raymarched near field,
meshed far field out to 4 km, one deferred cel-shading stack over both.

## What it does
- **Near field (0–150 m):** compute-only sphere tracing through a sparse 0.8 m brick atlas ...
- **Far field (150 m–4 km):** eight levels of surface-nets chunks, 12 bytes per quad ...
- **Destruction:** ordered CSG op lists per region, re-evaluated on the GPU ...
- **Islands:** severed pieces become Jolt bodies, land, sleep, and merge back into terrain ...

## Measured (RTX 4070 Laptop, 1440p)
| Pass | Budget | p50 | p99 |
|---|---|---|---|
| ... filled from Errata entry 10 ... |

## Build and run
...

## Where the work is
| Area | Code | Plan |
|---|---|---|
| ... one row per milestone, pointing at docs/superpowers/plans/ ... |
```

Fill the numbers from Errata entry 10 — **including the WARNs**. A portfolio piece that reports its own misses is worth more than one that reports only its hits, and every number in it must be one a reader could reproduce with `tools/run_benchmarks.sh`.

- [ ] **Step 7: Close out the follow-up file**

`docs/todo/opti.md` opened with two M3/M4 items and three M6 benchmark follow-ups. Rewrite it as a closed record: what was done (Tasks 9 and 10 for the first two, Tasks 1–8 for the raymarch and frame budgets), what the measurements said, and what is still open with the reason it was left. If nothing is open, say so and leave the file as the record.

- [ ] **Step 8: Commit**

```bash
git add docs/PORTFOLIO.md docs/todo/opti.md \
	docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md \
	extension/src/lod/lod_tree.cpp extension/src/lod/lod_grid.h \
	extension/tests/test_lod_tree.cpp
git commit -m "docs: M7 closing verdicts, fade-band decision, portfolio summary"
```

---

## Acceptance

M7 is complete when all of the following hold, each verified by a command whose output is quoted in the Errata:

1. `cd extension && scons test` — every native suite passes, including `test_brick_flags`, `test_op_filter`, `test_override_store`, `test_octant_split`.
2. `./gdunit_tests.sh -c` — every gdUnit suite passes, including the ten new ones.
3. `tools/run_benchmarks.sh m7-final` — five legs, each `EXIT_STATUS=0`, each printing `BENCH timing_condition ... vsync_actual=disabled verdict_qualified=false`. If V-Sync cannot be disabled on the available display server, the run is qualified and **says so in the verdict line**, which is itself the acceptance criterion — the failure mode this milestone removes is a silent one.
4. Every `BENCH budget_verdict` line is recorded in Errata entry 10 with its measured numbers, WARNs included and unaltered.
5. The edit leg's GPU cost does not grow across a 900-frame run (Task 8, Step 7), and `BENCH regions=... overflow=0`.
6. `--capture` writes 900 PNGs and `tools/encode_capture.sh` produces a playable file (Task 13, Step 6).
7. A cold reader can launch `demo/main.tscn`, read the help overlay, select all four tools, change the radius, and find every key listed in Task 11's `CONTROLS` table working.
8. `F5` reloads shaders without losing the world's edits; `F6` prints a self-check with zero mismatches.
9. Spec §4's fade-band contingency is **decided** in Errata entry 9, with the number it rests on.
10. `docs/PORTFOLIO.md` exists and every number in it comes from Errata entry 10.

## Errata (recorded during M7 implementation — corrections and measured verdicts)

Implementation-time facts are appended here as numbered entries. Entries 1–10 are reserved by
the tasks above: 1 the baseline sweep, 2 the raymarch attribution, 3 brick flags, 4 region DDA,
5 op filtering, 6 consolidation, 7 collider octants, 8 the first capture, 9 the fade-band
verdict, 10 the closing sweep. Later entries are free-form corrections in the style of M1–M6:
where this plan's text met reality and lost.

1. **Task 1, Step 10: M7 baseline**

   - steady

     ```text
     BENCH gpu_raymarch samples=287 p50_ms=6.294 p99_ms=7.910
     BENCH gpu_stream samples=287 p50_ms=0.003 p99_ms=0.004
     BENCH gpu_lod samples=287 p50_ms=0.043 p99_ms=0.051
     BENCH gpu_ssgi samples=287 p50_ms=0.172 p99_ms=0.174
     BENCH gpu_ssr samples=287 p50_ms=0.139 p99_ms=0.141
     BENCH gpu_shadows samples=287 p50_ms=0.123 p99_ms=0.271
     BENCH gpu_outlines samples=287 p50_ms=0.080 p99_ms=0.082
     BENCH gpu_unattributed samples=287 p50_ms=0.133 p99_ms=0.297
     BENCH gpu_custom_frame samples=287 p50_ms=7.326 p99_ms=9.188
     BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
     BENCH gpu_timing valid_samples=287 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
     BENCH gpu_timestamp_normalization mode=deterministic_vulkan_nanoseconds_to_microseconds unit=live_vulkan_nanoseconds_normalized scale_to_us=0.001000 normalized=true
     BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
     ```

   - move

     ```text
     BENCH gpu_raymarch samples=287 p50_ms=6.354 p99_ms=7.571
     BENCH gpu_stream samples=287 p50_ms=0.004 p99_ms=0.873
     BENCH gpu_lod samples=287 p50_ms=0.070 p99_ms=0.091
     BENCH gpu_ssgi samples=287 p50_ms=0.176 p99_ms=0.321
     BENCH gpu_ssr samples=287 p50_ms=0.166 p99_ms=0.345
     BENCH gpu_shadows samples=287 p50_ms=0.122 p99_ms=0.491
     BENCH gpu_outlines samples=287 p50_ms=0.079 p99_ms=0.215
     BENCH gpu_unattributed samples=287 p50_ms=0.147 p99_ms=0.293
     BENCH gpu_custom_frame samples=287 p50_ms=7.571 p99_ms=9.346
     BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
     BENCH gpu_timing valid_samples=287 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
     BENCH gpu_timestamp_normalization mode=deterministic_vulkan_nanoseconds_to_microseconds unit=live_vulkan_nanoseconds_normalized scale_to_us=0.001000 normalized=true
     BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
     ```

   - ridge

     ```text
     BENCH gpu_raymarch samples=287 p50_ms=6.016 p99_ms=8.724
     BENCH gpu_stream samples=287 p50_ms=0.012 p99_ms=0.815
     BENCH gpu_lod samples=287 p50_ms=0.181 p99_ms=0.315
     BENCH gpu_ssgi samples=287 p50_ms=0.144 p99_ms=0.363
     BENCH gpu_ssr samples=287 p50_ms=0.153 p99_ms=0.328
     BENCH gpu_shadows samples=287 p50_ms=0.099 p99_ms=0.611
     BENCH gpu_outlines samples=287 p50_ms=0.052 p99_ms=0.183
     BENCH gpu_unattributed samples=287 p50_ms=0.155 p99_ms=0.291
     BENCH gpu_custom_frame samples=287 p50_ms=7.191 p99_ms=9.875
     BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
     BENCH gpu_timing valid_samples=287 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
     BENCH gpu_timestamp_normalization mode=deterministic_vulkan_nanoseconds_to_microseconds unit=live_vulkan_nanoseconds_normalized scale_to_us=0.001000 normalized=true
     BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
     ```

   - edit

     ```text
     BENCH gpu_raymarch samples=287 p50_ms=10.533 p99_ms=14.263
     BENCH gpu_stream samples=287 p50_ms=0.870 p99_ms=17.205
     BENCH gpu_lod samples=287 p50_ms=0.050 p99_ms=0.251
     BENCH gpu_ssgi samples=287 p50_ms=0.159 p99_ms=0.299
     BENCH gpu_ssr samples=287 p50_ms=0.167 p99_ms=0.300
     BENCH gpu_shadows samples=287 p50_ms=0.127 p99_ms=0.351
     BENCH gpu_outlines samples=287 p50_ms=0.080 p99_ms=0.224
     BENCH gpu_unattributed samples=287 p50_ms=0.162 p99_ms=0.311
     BENCH gpu_custom_frame samples=287 p50_ms=12.872 p99_ms=27.758
     BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
     BENCH gpu_timing valid_samples=287 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
     BENCH gpu_timestamp_normalization mode=deterministic_vulkan_nanoseconds_to_microseconds unit=live_vulkan_nanoseconds_normalized scale_to_us=0.001000 normalized=true
     BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
     ```

   - island

     ```text
     BENCH gpu_raymarch samples=807 p50_ms=6.666 p99_ms=8.475
     BENCH gpu_stream samples=807 p50_ms=0.003 p99_ms=1.198
     BENCH gpu_lod samples=807 p50_ms=0.042 p99_ms=0.049
     BENCH gpu_ssgi samples=807 p50_ms=0.167 p99_ms=0.173
     BENCH gpu_ssr samples=807 p50_ms=0.144 p99_ms=0.164
     BENCH gpu_shadows samples=807 p50_ms=0.124 p99_ms=0.273
     BENCH gpu_outlines samples=807 p50_ms=0.080 p99_ms=0.225
     BENCH gpu_unattributed samples=807 p50_ms=0.134 p99_ms=0.165
     BENCH gpu_custom_frame samples=807 p50_ms=7.704 p99_ms=10.552
     BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
     BENCH gpu_timing valid_samples=807 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
     BENCH gpu_timestamp_normalization mode=deterministic_vulkan_nanoseconds_to_microseconds unit=live_vulkan_nanoseconds_normalized scale_to_us=0.001000 normalized=true
     BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
     ```
2. **Task 2, Step 10: raymarch attribution**

   Environment for all runs: Godot 4.7.1, Vulkan 1.4.341, NVIDIA GeForce RTX 4070 Laptop GPU,
   Wayland, requested 2560×1440, requested V-Sync disabled but compositor actual V-Sync enabled;
   therefore each run prints `verdict_qualified=false`. Commands used the available Wayland
   alternative to the brief's X11 command:
   `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark`.

   - Steady full: `BENCH gpu_raymarch samples=287 p50_ms=6.319 p99_ms=7.896`; `BENCH islands=0 ...`.
   - Steady `--effects-off=raymarched_sun_shadow`: `p50_ms=5.729 p99_ms=7.106`.
   - Steady `--effects-off=islands`: `p50_ms=6.324 p99_ms=7.982`; `BENCH islands=0 ...`, so this
     is a valid no-live-island control, not evidence of an island cost.
   - Steady `--effects-off=raymarched_sun_shadow,islands`: `p50_ms=5.727 p99_ms=7.143`.
   - Island leg full (`--benchmark-island`, 900 frames; `BENCH islands=2 ...`):
     `p50_ms=6.665 p99_ms=8.494`.
   - Island leg `--effects-off=islands` (`BENCH islands=2 ...`):
     `p50_ms=6.544 p99_ms=8.087`. The switch is real: it gates the raymarch island count while
     the physics harness still reports its spawned/live islands.

   The no-ray-shadow steady A/B is the useful attribution: removing the shadow ray reduced p50
   by 0.590 ms and p99 by 0.790 ms, supporting the **shadow rays** hypothesis rather than DDA
   overhead or field complexity. The steady-leg island pair is explicitly null because no island
   was live; the island-leg pair is the available with/without-islands measurement.

   Cost-view screenshot evidence: the requested X11 command
   `/usr/bin/godot --path . --display-driver x11 --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark`
   was attempted and failed with `ERROR: X11 Display is not available`. The alternative
   `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn`
   plus `WAYLAND_DISPLAY=wayland-1 grim /tmp/m7-task2-cost-view.png` did capture a 167656-byte
   frame, but no F3 input could be injected and this branch has no `KEY_F3` handler, so it is
   **not** claimed as cost-view evidence. Interactive cost-view screenshot capture remains
   unavailable in this environment; the exact attempted alternative frame is
   `/tmp/m7-task2-cost-view.png` (ordinary demo frame only).
3. **Task 3, Step 13: brick flags measured delta — null result and required revert**

   Command: `WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task3`.
   Environment: Godot 4.7.1, Vulkan 1.4.341, NVIDIA GeForce RTX 4070 Laptop GPU,
   Wayland, requested 2560x1440, requested V-Sync disabled. The run emitted
   `WARNING: The requested V-Sync mode Disabled is not available. Falling back to V-Sync mode Enabled.`
   The benchmark's exact timing line was `BENCH timing_condition display_driver=Wayland
   vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`; therefore these
   are qualified relative GPU comparisons, not an unqualified display/frame-budget PASS.

   | leg | Task 1 baseline p50/p99 ms | Task 3 pre-revert p50/p99 ms | p50 delta |
   |---|---:|---:|---:|
   | steady | 6.294 / 7.910 | 6.307 / 7.935 | +0.21% |
   | move | 6.354 / 7.571 | 6.379 / 7.865 | +0.39% |
   | ridge | 6.016 / 8.724 | 5.947 / 8.296 | -1.15% |
   | edit | 10.533 / 14.263 | 10.815 / 14.365 | +2.68% |
   | island | 6.666 / 8.475 | 6.746 / 8.516 | +1.20% |

   Exact Task 3 evidence (each process exited 0; lines copied from the five leg files):

   - steady: `BENCH gpu_raymarch samples=287 p50_ms=6.307 p99_ms=7.935`; `BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN`; `BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`
   - move: `BENCH gpu_raymarch samples=287 p50_ms=6.379 p99_ms=7.865`; `BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN`; `BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`
   - ridge: `BENCH gpu_raymarch samples=287 p50_ms=5.947 p99_ms=8.296`; `BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN`; `BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`
   - edit: `BENCH gpu_raymarch samples=287 p50_ms=10.815 p99_ms=14.365`; `BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN`; `BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`
   - island: `BENCH gpu_raymarch samples=807 p50_ms=6.746 p99_ms=8.516`; `BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN`; `BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`

   Every p50 delta was under 3%, so the Task 3 marcher change was reverted as required.
   The conservative flag buffer and its mark/generator bindings remain for Task 4. This is
   a null performance result; no Task 3 PASS is claimed.
4. **Task 4, Step 8: region DDA measured delta**

   Environment: Godot 4.7.1, Vulkan 1.4.341, NVIDIA GeForce RTX 4070 Laptop GPU, Wayland,
   requested 2560×1440. X11 is unavailable here; Wayland ignored the disabled-V-Sync request,
   but the benchmark reported `vsync_actual=disabled` and `verdict_qualified=false` on every
   leg. All five benchmark processes exited 0. Full logs are in the ignored
   `reports/m7-task4/` directory.

   The comparison is against Task 3's pre-revert marcher numbers (the flag buffer remained in
   place, as required). GPU raymarch p50/p99, with p50 and p99 deltas versus Task 3:

   | leg | Task 3 p50/p99 ms | Task 4 p50/p99 ms | p50 delta | p99 delta |
   |---|---:|---:|---:|---:|
   | steady | 6.307 / 7.935 | 6.834 / 8.925 | +8.36% | +12.48% |
   | move | 6.379 / 7.865 | 6.776 / 8.081 | +6.22% | +2.75% |
   | ridge | 5.947 / 8.296 | 5.436 / 8.443 | -8.59% | +1.77% |
   | edit | 10.815 / 14.365 | 11.066 / 16.861 | +2.32% | +17.38% |
   | island | 6.746 / 8.516 | 7.083 / 9.269 | +5.00% | +8.84% |

   Task 4 did **not** meet the 3% null-result revert condition on every leg, so the
   region-level DDA is retained. The budget verdict remained `raymarch=WARN` on all five
   legs; the other pass verdicts remained `lod=PASS ssgi=PASS ssr=PASS shadows=PASS
   outlines=PASS`, and `frame=WARN` (qualified display timing).

   Cost-view screenshot artifacts are local ignored evidence, not committed files. The
   temporary `--cost-view` launch switch used for this capture was not retained in the demo
   source. With the corresponding baseline or Task 4 demo running at 2560×1440 and cost view
   enabled, the reproducible Wayland capture commands are:

   ```text
   WAYLAND_DISPLAY=wayland-1 grim /tmp/m7-task4-cost-view-before3.png
   WAYLAND_DISPLAY=wayland-1 grim /tmp/m7-task4-cost-view-after.png
   stat -c '%n %s bytes' /tmp/m7-task4-cost-view-before3.png /tmp/m7-task4-cost-view-after.png
   /tmp/m7-task4-cost-view-before3.png 1975168 bytes
   /tmp/m7-task4-cost-view-after.png 2053828 bytes
   ```

   The captured local copies are `reports/m7-task4/cost-view-before.png` (1,975,168 bytes) and
   `reports/m7-task4/cost-view-after.png` (2,053,828 bytes); they are ignored by the repository
   rule and are not claimed as committed evidence. The before/after pair shows the expected
   dense grayscale heat view; no demo source change was retained for the temporary switch.
   The ignore check was:

   ```text
   git check-ignore -v reports/m7-task4/cost-view-before.png reports/m7-task4/cost-view-after.png
   .gitignore:20:reports/ reports/m7-task4/cost-view-before.png
   .gitignore:20:reports/ reports/m7-task4/cost-view-after.png
   ```

   The direct cost probe confirms the mechanism: the sky regression test reports a non-zero
   `regions` count and fewer than 32 `bricks`, while the hit-oracle rays still agree. This is
   the intended null-result guard: even if the GPU percentile movement is noisy, the measured
   traversal work changed from brick-by-brick sky walking to region skips.
5. **Task 5, Step 12: per-brick/per-workgroup op filtering**

   The focused native and GPU differential tests passed after the CPU reference and both
   shader paths used the same ordered AABB filter. The benchmark was run as:

   ```text
   WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task5
   ```

   All five benchmark processes exited 0. This environment is the same qualified Wayland
   setup as Task 4: `vsync_requested=disabled`, `vsync_actual=disabled`,
   `verdict_qualified=false`. The fresh Task 5 readings, compared with Task 4 round 1,
   are:

   | leg | Task 4 gpu_stream p50/p99 ms | Task 5 gpu_stream p50/p99 ms | Task 4 gpu_raymarch p50/p99 ms | Task 5 gpu_raymarch p50/p99 ms |
   |---|---:|---:|---:|---:|
   | steady | 0.003 / 0.004 | 0.003 / 0.004 | 6.838 / 8.927 | 6.835 / 8.945 |
   | move | 0.004 / 0.801 | 0.004 / 0.952 | 6.836 / 8.362 | 6.790 / 8.459 |
   | ridge | 0.012 / 0.684 | 0.010 / 0.628 | 5.541 / 8.333 | 5.437 / 8.501 |
   | edit | 0.867 / 14.786 | 0.236 / 2.977 | 11.296 / 16.888 | 11.129 / 15.798 |
   | island | 0.003 / 2.032 | 0.003 / 0.736 | 7.182 / 9.253 | 7.171 / 9.258 |

   The edit leg is the expected result: `gpu_stream` fell 72.8% at p50 and 79.9% at p99,
   while `gpu_raymarch` moved -1.5% at p50 and -6.5% at p99. The filter is retained; its
   cost is specifically in the edit path, and the measurement does not support claiming a
   broad raymarch speedup. Every leg retained `raymarch=WARN`, with
   `lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN`. The edit run also
   reported `regions=133 overflow=1`; the overflow is the existing edit benchmark's
   stream/job pressure and is not attributed to the filter.

   **Round-1 fix remeasurement:** the review fix changed stored-lattice consumers from the
   0.20 m brick pad to the shared `pitch + kSdfRange` pad (0.69 m at the 5 cm lattice), while
   brick residency/generation remains on the exact 0.20 m pad. The benchmark was rerun with:

   ```text
   WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task5
   ```

   All five legs exited 0. The display reported `vsync_requested=disabled`,
   `vsync_actual=disabled`, and `verdict_qualified=false`; every leg remained
   `raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN`.
   The fresh stream/raymarch p50/p99 readings (ms) are:

   | leg | fixed gpu_stream p50/p99 | fixed gpu_raymarch p50/p99 |
   |---|---:|---:|
   | steady | 0.003 / 0.004 | 6.837 / 8.966 |
   | move | 0.004 / 1.156 | 6.775 / 8.360 |
   | ridge | 0.012 / 0.868 | 5.535 / 8.557 |
   | edit | 0.236 / 2.968 | 11.022 / 15.934 |
   | island | 0.003 / 0.261 | 7.229 / 9.243 |

   Compared with the prior Task 5 measurement, the edit stream leg is effectively unchanged
   (0.236 / 2.968 versus 0.236 / 2.977), so the pad correction changes correctness coverage,
   not the benchmark conclusion. The edit leg still reported `regions=133 overflow=1`.
6. **Task 8, Step 7: consolidation measured delta and overflow verdict**

   Environment: Godot 4.7.1, Vulkan 1.4.341, NVIDIA GeForce RTX 4070 Laptop GPU, Wayland,
   requested 2560x1440, requested V-Sync disabled but compositor actual V-Sync enabled;
   therefore `verdict_qualified=false`. The benchmark processes exited 0. The Task 5 fixed
   edit baseline was `gpu_stream p50/p99=0.236/2.968 ms`, `gpu_raymarch=11.022/15.934 ms`,
   `gpu_custom_frame=12.293/19.378 ms`.

   Task 8's five-leg command was:

   ```text
   WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task8-final
   ```

   The edit leg reported:

   ```text
   BENCH gpu_stream samples=287 p50_ms=0.243 p99_ms=2.998
   BENCH gpu_raymarch samples=287 p50_ms=11.052 p99_ms=15.944
   BENCH gpu_custom_frame samples=287 p50_ms=12.342 p99_ms=19.436
   BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
   BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
   BENCH regions=133 overflow=1 overrides=0/8192 consolidations=0 refusals=0
   ```

   The `overflow=1` value is the existing atlas brick-job overflow counter, not an edit-log
   rejection: the run emitted no `region op list full` diagnostic. This benchmark's moving
   aim distributes its edits across regions, so no region reached a successful 192-op bake;
   the Task 8 policy and the zero-overflow path are instead pinned by the focused 300-edit
   test, which passed with `overflow_ever=0`, `consolidations=2`, and `override_bricks=432`.

   The required 3x edit run was made by temporarily setting `FRAMES` to 900 in
   `demo/benchmark.gd` and restoring it afterwards:

   ```text
   BENCH gpu_stream samples=807 p50_ms=0.241 p99_ms=0.974
   BENCH gpu_raymarch samples=807 p50_ms=14.969 p99_ms=20.259
   BENCH gpu_custom_frame samples=807 p50_ms=16.663 p99_ms=22.061
   BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
   BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
   BENCH regions=124 overflow=1 overrides=0/8192 consolidations=0 refusals=39
   ```

   The aggregate 900-frame edit leg does not support the claim that cost stops growing: its
   distributed edit workload never publishes an override and its GPU p50/p99 are higher
   than the 300-frame leg. The implementation therefore reports the policy as measured,
   retains fail-soft refusals, and does not claim a Task 8 benchmark PASS. The focused policy
   suite and native sequence-tail test are the positive evidence; a future benchmark leg
   must keep edits in one bounded region and size the plan below the override capacity to
   measure consolidation itself.
7. **Task 9, Step 9: collider octant split measured delta; round-4 sparse-body correction**

   Round 3 briefly allocated a shape-less PhysicsServer body for every empty octant so a test
   could observe eight valid RIDs. That contradicted the intended raw-body contract: the flat
   arrays always have eight slots, but only populated centroid bins own bodies. Round 4 restores
   sparse body creation and proves atomic replacement by comparing the actual sparse RID masks.

   Environment: Godot 4.7.1, Vulkan 1.4.341, NVIDIA GeForce RTX 4070 Laptop GPU, Wayland,
   requested 2560x1440, requested V-Sync disabled but compositor actual V-Sync enabled. Godot
   reports `vsync_actual=disabled` while also logging `The requested V-Sync mode Disabled is
   not available. Falling back to V-Sync mode Enabled.` These frame comparisons are therefore
   qualified. All five benchmark processes exited 0.

   Command:
   ```text
   env WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 tools/run_benchmarks.sh m7-task9-round4-final
   ```

   Task 8's comparison is the final 300-frame run in `reports/m7-task8-final`; move and edit
   are also 300 frames here. Task 8's `phys_setdata_ms` was a per-frame accumulated value. The
   corrected Task 9 instrumentation reports `build_ms` as the maximum one-octant build call and
   `phys_setdata_ms` as the maximum one `shape_set_data` call, never a frame sum.

   | leg | Task 8 p99 / max / over 16.6 ms | Task 9 round-4 p99 / max / over 16.6 ms | Task 8 max `phys_setdata_ms` | Task 9 max `build_ms` / `phys_setdata_ms` |
   |---|---:|---:|---:|---:|
   | move | 33.33 / 43.10 / 272 (90.7%) | 27.29 / 35.67 / 279 (93.0%) | 0.95 ms | 0.49 / 0.31 ms |
   | edit | 56.09 / 77.43 / 298 (99.3%) | 50.39 / 77.28 / 297 (99.0%) | 22.41 ms | 1.03 / 0.40 ms |

   Exact current evidence:
   ```text
   move: BENCH p50=19.23 p95=23.98 p99=27.29 max=35.67 min_fps=28.0 over_16.6ms=279 (93.0%)
   move: BENCH max_ms build_ms=0.49 island_ms=0.01 lod_ms=0.08 phys_apply_ms=24.40 phys_body_ms=0.04 phys_collect_ms=1.12 phys_faces_ms=0.48 phys_plan_ms=3.78 phys_setdata_ms=0.31 phys_submit_ms=0.01 phys_tris=7625.00 physics_tick_ms=26.28 stream_readback_ms=0.02 stream_total_ms=0.29
   move: BENCH chunks=929 pending=860 bodies=69 bodies_raw=282 failures=0 build_ms=0.18 collect_ms=0.38
   edit: BENCH p50=22.86 p95=33.33 p99=50.39 max=77.28 min_fps=12.9 over_16.6ms=297 (99.0%)
   edit: BENCH max_ms build_ms=1.03 island_ms=38.86 lod_ms=0.06 phys_apply_ms=28.72 phys_body_ms=0.07 phys_collect_ms=4.73 phys_faces_ms=0.67 phys_plan_ms=2.09 phys_setdata_ms=0.40 phys_submit_ms=0.01 phys_tris=8519.00 physics_tick_ms=29.54 stream_readback_ms=0.01 stream_total_ms=0.29
   edit: BENCH chunks=664 pending=566 bodies=98 bodies_raw=460 failures=0 build_ms=0.48 collect_ms=2.83
   ```

   The split still removes the one-call Jolt shape-build spike: Task 8's 22.41 ms edit
   maximum versus Task 9's 0.40 ms maximum `shape_set_data` call; the corresponding move
   values are 0.95 ms versus 0.31 ms. Raw body counts are sparse (282 for 69 move chunks and
   460 for 98 edit chunks), rather than the test-induced eight bodies per chunk. The overall
   frame p99 remains noisy and above 16.6 ms; edit's remaining spikes include
   `island_ms=38.86` and `phys_apply_ms=28.72`, not one fat collider build. The task is retained
   for eliminating the atomic collider build, not as a claim that the full frame budget is
   closed. Every leg retained `raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS
   outlines=PASS frame=WARN`, and each timing line was `display_driver=Wayland
   vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`.
8. **Task 13, Step 6: first capture**

   Completed the first `--capture` run: 900 frames written to
   `/home/jeremy/.local/share/godot/app_userdata/Voxel Everything/capture`, encoded by
   `tools/encode_capture.sh` to `/tmp/voxel-everything.mp4` (~23 MB, 900 frames at 60 fps,
   ~15 s, 1268×1376 — Wayland ignored the requested 2560×1440 in this environment).

   Automated frame-by-frame inspection of the captured PNG sequence found the following
   visual results (this is the visual evidence for Task 14):

   - Floating/disconnected terrain chunks are visible in the background in several frames
     (e.g. frames 240, 380, 450, 520).
   - Fade-band transitions are visible as dithering/zone-boundary bands in later frames
     (e.g. 520, 660).
   - Texture/material seams are visible along terrain contours and zone boundaries.
   - Slight texture smearing appears on steep slopes / gray surfaces.
   - Popping at the seam was not directly observable from sampled static frames; temporal
     popping could not be fully assessed from stills.
9. _(Task 14, Step 2: fade-band verdict and the number it rests on — to be filled)_
10. _(Task 14, Step 5: closing sweep, full record — to be filled)_
