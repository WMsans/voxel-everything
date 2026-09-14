# Render Lifetime Owner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `RenderOrchestrator` becomes the single owner of render-side lifetime and `VoxelWorld` becomes a node façade, with sub-project 1's unfinished tasks closed first and every move measured by a lifetime contract suite.

**Architecture:** Finish SP1 (seam probe, orphan deletion), record a baseline, then pin teardown/reload/shutdown behaviour in a new contract suite before any move. The island upload queue becomes a pure, native-tested `IslandHandoff` owned by the orchestrator; `IslandManager` gets narrow collaborators; streamer, lifetime flags, render knobs and `VoxelFrame` move under the orchestrator and `FrameHost` dies; 25 pass accessors collapse into one `RenderPasses` table; S8 is fixed test-first; both `friend VoxelDebugHooks` declarations go; `hooks.cpp` is split by a verified pure-move script.

**Tech Stack:** C++20, godot-cpp (Godot 4.7.2), GLSL through `RenderingDevice`, doctest (native), gdUnit4 (GPU), SCons, perl/python for mechanical rewrites.

**Spec:** `docs/superpowers/specs/2026-09-14-render-lifetime-owner-design.md`. Roadmap: `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.1. SP1 plan (Tasks 10–11 are executed from it): `docs/superpowers/plans/2026-09-13-frame-module.md`.

## Global Constraints

- Branch: `refactor/render-lifetime-owner` (already checked out; spec committed at `a6310e1`).
- **GDScript API byte-identical.** Every `ClassDB::bind_method` name, argument list and every Dictionary key returned by `VoxelWorld` (62 bindings) and `VoxelDebugHooks` (175 bindings) is unchanged.
- **No stage reordering** in `VoxelFrame` except S8's one-line move of the `deferred` timing begin (Task 11).
- **No pass internals, no shaders.** Do not edit any `render/*_pass.{h,cpp}` or `shaders/*`.
- **Locking:** no lock is acquired at a new call site. The only permitted changes are the four `island_mutex_` removals listed in Task 6 and the relocation of `debug_lod_stats`' `lod_mutex` hold into `LodSystem::stats()` (same hold, same caller, Task 12).
- **Teardown order:** the teardown trace (Task 4) must read exactly `passes, streamer, residency, island_graph, island_slots, atlas, lod, history, initialized` after every task.
- **Never call `teardown()`/`initialize()` on `DeferredPass` or `ContactShadowPass` from new code** (they mirror the orchestrator's sun UBO; `DeferredPass::teardown()` frees it).
- **Golden policy (SP1):** a moved golden needs a measured cause in the same commit. A diff that looks like a shipped bug stops the task: report, do not fix.
- **Stop conditions (spec §7):** teardown trace changes; a lock at a new site; shipped golden moves outside an attributed SP1-leftover change; a bound signature/Dictionary key changes; a suspected shipped bug. Stop and report.
- **Baseline:** compare gdUnit results against `docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md` (Task 3) by case name *and* message. A suite's case **count** dropping is a failure (a failing case aborts the rest of its suite). `test_connectivity` and `test_island_body` are flaky by case: compare their failure count.
- GPU timing *values* are invalid on this machine; never pin them. Marker *names* are captured in command order on Metal (verified 2026-09-14).
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` from the repo root (a clean C++ rebuild can take ~20 min). Native: `(cd extension && scons -Q test)`. GPU: `./gdunit_tests.sh -a res://tests/<suite>.gd[,res://tests/<suite>.gd]`; full run `./gdunit_tests.sh`. Reports: `reports/report_N/results.xml`.
- All commands run from the repo root unless a step says otherwise.

## Decisions made during planning (amend the spec; Task 14 writes them into it)

1. **S8 needs no fallback.** A throwaway probe showed Metal records `capture_timestamp` names in command order (values 0). The S8 test reads names directly from GDScript.
2. **`demo/benchmark.gd` has no `deferred` key** (it samples `raymarch, stream, lod, ssgi, ssr, ssao, shadows, outlines, unattributed, custom_frame`). S8 changes no label table; the fix corrects `unattributed` (it subtracted SSAO twice). Recorded in the S8 commit and results.
3. **Hooks and compositors reach modules through the existing `VoxelWorld::context()`** (`VoxelContext{store, render, lod, consolidation}`), not new `render()/lod()/store()` accessors — the accessor already exists.
4. **`WorldStats` is read-only; three narrow mutators exist for hook writes:** `note_overflow(int)`, `physics_bubble_centers()`, `test_bodies()`.
5. **`LodStats`:** `debug_lod_stats` took `lod_mutex` and called `ensure_lod()`; that body moves into `LodSystem::stats()` with the same hold. `builds_in_flight` (an atomic read) now happens outside the hold.
6. **`LodSystem`'s `near_field_enabled` collaborator is deleted**; it reads `render()->near_field_enabled()` through the render slot it already holds (it is constructed before the orchestrator, so it cannot take the atomic's address).
7. **The contract suite has no physics-teardown case.** `tests/test_island_render.gd` already pins the physics filter (`test_teardown_physics_preserves_committed_field_volume_uploads` and the stale-upload case). The survival bite proof targets GPU teardown.
8. **`WorldStore::region_window()`** replaces the duplicate bodies in `VoxelWorld::region_window` and `VoxelFrame::region_window`.
9. **`debug/hooks_common.h`** holds the two file-local helpers (`half_to_float`, `write_frame_record`) the split files share.
10. **`VoxelFrame` is a direct member of `RenderOrchestrator`, declared last**; `lod_` moves above `render_` in `voxel_world.h` so `LodSystem` outlives the frame.
11. **`VoxelWorld::set_generator`** has no callers and is deleted in Task 12.
12. **Native tests link with `-pthread`** (the handoff's concurrency smoke test is the first native test to start a thread).

## File Map

| File | Status | Responsibility |
|---|---|---|
| `extension/src/render/island_handoff.{h,cpp}` | Create | Pure main→render island queue, `IslandSlotDesc`, slot high-water marks, `release_volume_slot` |
| `extension/tests/test_island_handoff.cpp` | Create | Native tests for the handoff |
| `extension/SConstruct` | Modify | Handoff joins the native build; `-pthread` |
| `extension/src/render/island_atlas.{h,cpp}` | Modify | `IslandSlotDesc`/`kMaxIslands` move out |
| `extension/src/render/orchestrator.{h,cpp}` | Modify | Teardown trace, handoff, streamer, lifetime flags, knobs, frame, `RenderPasses`, 5-field `Collaborators` |
| `extension/src/render/frame.{h,cpp}` | Modify | `FrameHost` deleted; reads the orchestrator; S8 fix |
| `extension/src/lod/lod_system.{h,cpp}` | Modify | `release_gpu()`, `stats()`, friend removed |
| `extension/src/physics/island_manager.{h,cpp}` | Modify | `Collaborators` instead of `VoxelWorld*` |
| `extension/src/core/world_store.{h,cpp}` | Modify | `raycast_down`, `region_window` |
| `extension/src/voxel_world.{h,cpp}` | Modify | Façade; ≤ 250 header lines |
| `extension/src/raymarch_compositor.cpp`, `beauty_compositor.cpp` | Modify | Call through `context().render` |
| `extension/src/mesh/consolidation.*` | Unchanged | (only its wiring in `voxel_world.cpp` changes) |
| `extension/src/debug/hooks.{h,cpp}` | Modify | Seam probe, teardown trace hook, re-homed reads |
| `extension/src/debug/hooks_{render,lod,world,physics}.cpp`, `hooks_common.h` | Create | Pure-move split |
| `tests/test_render_lifetime_contract.gd` | Create | Lifetime contract |
| `tests/test_gpu_timing_scopes.gd` | Create | S8 |
| `tests/test_lod_seam.gd`, `tests/test_lod_gbuffer.gd` | Modify | SP1 Task 10 |
| `docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md` | Create | Baseline + evidence log |
| `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md` | Create | Results report |
| `docs/superpowers/specs/2026-09-13-frame-module-design.md`, `…/2026-09-14-render-lifetime-owner-design.md`, `docs/superpowers/plans/2026-09-13-frame-module.md` | Modify | Status and amendments |

---

### Task 1: SP1 Task 10 — migrate `debug_seam_probe` onto the frame

**Files:** as listed in SP1 plan Task 10 (`extension/src/debug/hooks.cpp`, `tests/test_lod_seam.gd`, `tests/test_lod_gbuffer.gd`).

**Interfaces:**
- Consumes: `VoxelFrame::looking_at`, `render_headless`, `FrameDebug::{marker, skip_far_field, lod_viewport}`, `FrameRecord` — all exist today.
- Produces: `debug_seam_probe` with unchanged signature and keys, plus `error` when `near_field_scale < 1`.

- [ ] **Step 1: Confirm the SP1 task still applies**

```bash
rg -n 'render_headless|world_->frame\(\)' extension/src/debug/hooks.cpp | rg -n seam || true
sed -n '/Dictionary VoxelDebugHooks::debug_seam_probe/,/^}/p' extension/src/debug/hooks.cpp | head -40
```
Expected: `debug_seam_probe` does not yet call `render_headless` (it rebuilds the frame by hand).

- [ ] **Step 2: Execute SP1 plan Task 10, Steps 1–5 exactly as written**

Open `docs/superpowers/plans/2026-09-13-frame-module.md`, section "Task 10: Migrate `debug_seam_probe` onto the frame". Its Step 2 contains the complete replacement body; Step 3 the two suite edits; Steps 4–5 build, run and attribute. Write its "before" and "after" logs to the scratchpad instead of `/tmp`. The SP1 baseline note it cites (`test_lod_gbuffer::test_far_field_pixels_carry_a_material_and_a_unit_normal`) may have drifted; compare against a fresh run of the two suites on the pre-change commit (Step 1 of that task).

- [ ] **Step 3: Commit**

```bash
git add extension/src/debug/hooks.cpp tests/test_lod_seam.gd tests/test_lod_gbuffer.gd
git diff --cached --check
git commit -m "refactor(probe): debug_seam_probe renders the shipped frame with a field marker

Moved values: <SP1 Task 10 Step 5 attribution, or 'none'>.

"
```
Replace the angle-bracket field with the measured attribution before committing.

---

### Task 2: SP1 Task 11 — consolidate pass-probe inputs and delete orphaned frame plumbing

**Files:** as listed in SP1 plan Task 11.

**Interfaces:**
- Consumes: `ve::probe_camera`, `ve::probe_up_hint`, `ve::set_near_field_world`, `ve::set_near_field_flags`, `VoxelFrame::{grass_layout, sun_ortho, last_frame}`.
- Produces: private `VoxelDebugHooks::render_probe_pixel(Vector3, Vector3)`; `VoxelWorld::note_lod_cull_debug`, `lod_cull_debug`, the three `lod_cull_*_` atomics, `set_beauty_compositor`, `beauty_compositor_` and `VoxelWorld::render_probe_pixel` no longer exist. `FrameHost` stays (Task 9 deletes it).

- [ ] **Step 1: Execute SP1 plan Task 11, Steps 1–7 exactly as written**

Section "Task 11: Consolidate pass-probe inputs and delete orphaned frame plumbing". Step 5 lists what may be deleted and what must stay; its caller searches are authoritative. Step 6 edits the SP1 spec; keep its status "pre-implementation" (Task 14 changes it).

- [ ] **Step 2: Verify the deletions**

```bash
rg -n 'note_lod_cull_debug|lod_cull_two_phase_|lod_cull_hiz_built_|lod_cull_first_pass_|set_beauty_compositor|beauty_compositor_|VoxelWorld::render_probe_pixel' extension/src
```
Expected: no output.

- [ ] **Step 3: Commit**

Use SP1 Task 11 Step 8's staging and message, with measured golden causes if any moved, plus the two attribution lines from Global Constraints.

---

### Task 3: Record the SP2 baseline

No production change.

**Files:**
- Create: `docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md`

**Interfaces:**
- Consumes: the commit produced by Task 2.
- Produces: the baseline file, whose "Evidence log" section later tasks append to.

- [ ] **Step 1: Build and run the native suite**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
(cd extension && scons -Q test) 2>&1 | tail -5
```
Record the doctest `test cases:` summary line.

- [ ] **Step 2: Run the full gdUnit suite**

```bash
./gdunit_tests.sh; echo "exit=$?"
```
A non-zero exit is normal.

- [ ] **Step 3: Extract per-suite counts and failing cases**

```bash
latest=$(ls -d reports/report_* | sort -V | tail -1); echo "$latest"
python3 - "$latest/results.xml" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for suite in root.iter('testsuite'):
    print(f"# {suite.get('name')}: tests={suite.get('tests')} failures={suite.get('failures')}")
for tc in root.iter('testcase'):
    bad = tc.find('failure') if tc.find('failure') is not None else tc.find('error')
    if bad is not None:
        msg = (bad.get('message') or bad.text or '').strip().splitlines()
        print(f"{tc.get('classname')}::{tc.get('name')} — {msg[0] if msg else ''}")
EOF
```

- [ ] **Step 4: Write the baseline file**

```markdown
# Render lifetime owner — baseline

Commit: <`git rev-parse --short HEAD`>. Recorded <YYYY-MM-DD> on <OS / GPU>.
Report: <reports/report_N path from Step 3>.

## Native
<doctest summary line>

## gdUnit per-suite counts
<every "# suite: tests=… failures=…" line>

## gdUnit failing cases
<every "suite::case — message" line>

Flaky by case (compare failure COUNT): test_connectivity, test_island_body.

## Evidence log
Appended by later tasks: bite proofs, attributions, per-milestone gate results.
```
Paste real output into every angle-bracket field.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git commit -m "docs: render lifetime owner baseline failure set

"
```

---

### Task 4: Render lifetime contract suite and teardown trace (characterization)

Pins teardown, reload, shutdown and upload survival **before** anything moves. The teardown trace is the only production addition.

**Files:**
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp` (`teardown_gpu`)
- Modify: `extension/src/debug/hooks.h`, `extension/src/debug/hooks.cpp` (`debug_teardown_trace`)
- Create: `tests/test_render_lifetime_contract.gd`
- Modify: `docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md` (evidence log)

**Interfaces:**
- Consumes: existing hooks `debug_init_atlas`, `debug_teardown_atlas` (= `VoxelWorld::teardown_gpu`), `debug_stream_frame`, `debug_render_frame`, `debug_pump_shader_reload`, `debug_shader_reload_stats`, `debug_queue_committed_field_volume_upload`, `debug_island_pending_uploads`, `debug_field_volume_upload_count`, `debug_init_physics`, `debug_lod_stats`; bound `request_shader_reload`, `shutdown_render_resources`, `try_begin_render_callback`, `is_initialized`; consts from `tests/test_frame_shipped_golden.gd`.
- Produces: `const std::vector<const char *> &RenderOrchestrator::teardown_trace() const`; bound `PackedStringArray VoxelDebugHooks::debug_teardown_trace()`; the suite later tasks run as their gate.

- [ ] **Step 1: Write the contract suite**

Create `tests/test_render_lifetime_contract.gd`:

```gdscript
extends GdUnitTestSuite

# Render lifetime contract (docs/superpowers/specs/2026-09-14-render-lifetime-owner-design.md
# §5.1). Characterization written BEFORE sub-project 2 moves any lifetime code: it pins what
# teardown, reload and shutdown do today, so "the move changed nothing" is a measurement.
#
# Leaks are measured only on the LOCAL device: freeing it makes the engine name every RID that
# is still alive ("N RID(s) of type "X" was leaked"). On this machine the RenderingDevice
# allocation counters read 0 and memory usage never decreases, and the main device is never
# freed, so main-device leaks are not measurable here and are not asserted.

const Golden := preload("res://tests/test_frame_shipped_golden.gd")

const CAM := Vector3(24.0, 70.0, 24.0)
const FWD := Vector3(0.2, -1.0, 0.2)
const TEARDOWN_ORDER := ["passes", "streamer", "residency", "island_graph", "island_slots",
	"atlas", "lod", "history", "initialized"]

class LeakLogger extends Logger:
	var lines := PackedStringArray()

	func _log_error(_function: String, _file: String, _line: int, code: String, rationale: String,
			_editor_notify: bool, _error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
		var text := "%s %s" % [code, rationale]
		if text.contains(" leaked"):
			lines.append(text)

	func _log_message(message: String, _error: bool) -> void:
		if message.contains(" leaked"):
			lines.append(message)

var _nodes: Array = []
var _logger: LeakLogger

func before_test() -> void:
	_logger = LeakLogger.new()
	OS.add_logger(_logger)

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()
	OS.remove_logger(_logger)

func make_local_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_nodes.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	settle(w)
	return w

# Several consecutive quiet streamer frames (see test_shader_reload.gd for why one is not enough).
func settle(w: VoxelWorld) -> void:
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(CAM) == 0 else 0
		if quiet >= 6:
			return

func render(w: VoxelWorld) -> Dictionary:
	return w.hooks().debug_render_frame(CAM, FWD.normalized(), 64, 64)

func assert_renders(w: VoxelWorld, why: String) -> void:
	var d := render(w)
	assert_bool(d["ok"]).override_failure_message("%s: frame aborted: %s" % [why, d]).is_true()
	assert_float(float(d["mean_luma"])).override_failure_message(
		"%s: the lit image is black" % why).is_greater(0.01)

# Frees the world -- _exit_tree shuts render resources down and drops the local device -- and
# returns the leak lines the engine printed while doing so.
func free_and_collect_leaks(w: VoxelWorld) -> PackedStringArray:
	_nodes.erase(w)
	_logger.lines.clear()
	w.free()
	return _logger.lines.duplicate()

func test_teardown_then_reinit_renders_and_frees_clean(timeout := 120000) -> void:
	var w := make_local_world()
	assert_renders(w, "before teardown")
	w.hooks().debug_teardown_atlas()
	assert_bool(w.is_initialized()).is_false()
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	settle(w)
	assert_renders(w, "after teardown + re-init")
	var leaks := free_and_collect_leaks(w)
	assert_array(Array(leaks)).override_failure_message(
		"teardown + re-init left RIDs alive: %s" % leaks).is_empty()

func test_reload_pump_renders_and_frees_clean(timeout := 120000) -> void:
	var w := make_local_world()
	assert_renders(w, "before reload")
	w.request_shader_reload()
	w.hooks().debug_pump_shader_reload()
	assert_bool(w.hooks().debug_shader_reload_stats()["last_ok"]).is_true()
	settle(w)
	assert_renders(w, "after reload")
	var leaks := free_and_collect_leaks(w)
	assert_array(Array(leaks)).override_failure_message(
		"reload left RIDs alive: %s" % leaks).is_empty()

func test_shutdown_closes_admission_and_frees_clean(timeout := 60000) -> void:
	var w := make_local_world()
	assert_renders(w, "before shutdown")
	w.shutdown_render_resources()
	assert_bool(w.is_initialized()).is_false()
	assert_bool(w.try_begin_render_callback()).override_failure_message(
		"admission reopened after shutdown").is_false()
	var leaks := free_and_collect_leaks(w)
	assert_array(Array(leaks)).override_failure_message(
		"shutdown left RIDs alive: %s" % leaks).is_empty()

# An upload queued before a GPU teardown is part of the CPU world and must reach the next pool.
# (Physics teardown's filter is pinned by test_island_render.gd.)
func test_a_queued_upload_survives_gpu_teardown(timeout := 120000) -> void:
	var w := make_local_world()
	assert_renders(w, "drain anything already queued")
	var before: int = w.hooks().debug_field_volume_upload_count()
	var dim := 2
	var bytes := PackedByteArray()
	bytes.resize(dim * dim * dim)
	bytes.fill(128)
	w.hooks().debug_queue_committed_field_volume_upload(5, bytes, bytes, dim)
	assert_int(w.hooks().debug_island_pending_uploads()).is_equal(1)
	w.hooks().debug_teardown_atlas()
	assert_int(w.hooks().debug_island_pending_uploads()).override_failure_message(
		"GPU teardown dropped a queued upload").is_equal(1)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	settle(w)
	assert_renders(w, "after re-init")
	assert_int(w.hooks().debug_island_pending_uploads()).is_equal(0)
	assert_int(w.hooks().debug_field_volume_upload_count()).override_failure_message(
		"the upload queued before teardown never landed").is_equal(before + 1)

func test_gpu_teardown_order_is_pinned(timeout := 60000) -> void:
	var w := make_local_world()
	w.hooks().debug_teardown_atlas()
	var trace: PackedStringArray = w.hooks().debug_teardown_trace()
	assert_array(Array(trace)).is_equal(TEARDOWN_ORDER)

# --- the shipped path (main device, real compositors). Mirrors test_frame_shipped_golden.gd's
# fixture; camera, sizes and tolerance come from that suite so the two cannot drift. ---

func make_shipped_scene() -> Dictionary:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = false
	world.physics_enabled = false
	add_child(world)
	_nodes.append(world)
	assert_bool(world.hooks().debug_init_physics()).is_true()
	var raymarch: RaymarchCompositor = ClassDB.instantiate("RaymarchCompositor")
	raymarch.world_path = world.get_path()
	var beauty: BeautyCompositor = ClassDB.instantiate("BeautyCompositor")
	beauty.world_path = world.get_path()
	var effects: Array[CompositorEffect] = [raymarch, beauty]
	var compositor := Compositor.new()
	compositor.compositor_effects = effects
	var vp := SubViewport.new()
	vp.size = Vector2i(Golden.W, Golden.H)
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(vp)
	_nodes.append(vp)
	var cam := Camera3D.new()
	cam.fov = 60.0
	cam.near = 0.05
	cam.far = 4000.0
	cam.compositor = compositor
	vp.add_child(cam)
	cam.current = true
	var pose: Array = Golden.CAMERAS["down_close"]
	var pos: Vector3 = pose[0]
	cam.global_position = pos
	cam.look_at(pos + (pose[1] as Vector3).normalized(), Vector3.UP)
	return {"world": world, "viewport": vp}

func settle_shipped(world: VoxelWorld) -> bool:
	for i in range(Golden.SETTLE_FRAMES):
		await get_tree().process_frame
	var quiet := 0
	for i in range(Golden.LOD_SETTLE_BUDGET):
		await get_tree().process_frame
		var s: Dictionary = world.hooks().debug_lod_stats()
		var idle: bool = int(s.get("requests_pending", 1)) == 0 and int(s.get("builds_in_flight", 1)) == 0
		quiet = quiet + 1 if idle else 0
		if quiet >= Golden.LOD_QUIET_FRAMES:
			return true
	return false

func tile_luma(img: Image) -> PackedFloat32Array:
	var sums := PackedFloat32Array()
	var counts := PackedInt32Array()
	sums.resize(Golden.TILES * Golden.TILES)
	counts.resize(Golden.TILES * Golden.TILES)
	for y in range(0, img.get_height(), Golden.SAMPLE_STEP):
		var ty := mini(y * Golden.TILES / img.get_height(), Golden.TILES - 1)
		for x in range(0, img.get_width(), Golden.SAMPLE_STEP):
			var tx := mini(x * Golden.TILES / img.get_width(), Golden.TILES - 1)
			var c := img.get_pixel(x, y)
			sums[ty * Golden.TILES + tx] += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			counts[ty * Golden.TILES + tx] += 1
	for i in range(sums.size()):
		sums[i] = sums[i] / maxf(1.0, float(counts[i]))
	return sums

func test_the_shipped_path_survives_reload_and_shuts_down(timeout := 900000) -> void:
	var s := make_shipped_scene()
	var world: VoxelWorld = s["world"]
	var vp: SubViewport = s["viewport"]
	assert_bool(await settle_shipped(world)).override_failure_message(
		"the far field never settled before reload").is_true()
	world.request_shader_reload()
	for i in range(8):
		await get_tree().process_frame
	var stats: Dictionary = world.hooks().debug_shader_reload_stats()
	assert_int(int(stats["reloads"])).is_equal(1)
	assert_bool(stats["last_ok"]).is_true()
	assert_bool(await settle_shipped(world)).override_failure_message(
		"the far field never settled after reload").is_true()
	var acc := PackedFloat32Array()
	acc.resize(Golden.TILES * Golden.TILES)
	for f in range(Golden.AVERAGE_FRAMES):
		await RenderingServer.frame_post_draw
		var t := tile_luma(vp.get_texture().get_image())
		for i in range(t.size()):
			acc[i] += t[i] / float(Golden.AVERAGE_FRAMES)
	var golden: Array = Golden.GOLDEN["down_close"]["tiles"]
	for i in range(acc.size()):
		assert_float(acc[i]).override_failure_message(
			"tile %d after reload: %f vs golden %f" % [i, acc[i], golden[i]]
			).is_equal_approx(float(golden[i]), Golden.TOL_TILE)
	world.shutdown_render_resources()
	assert_bool(world.is_initialized()).is_false()
	assert_bool(world.try_begin_render_callback()).is_false()
```

- [ ] **Step 2: Run it to see the missing hook fail**

```bash
./gdunit_tests.sh -a res://tests/test_render_lifetime_contract.gd
```
Expected: `test_gpu_teardown_order_is_pinned` fails (no `debug_teardown_trace` method). Every other case passes. **If a leak case reports leak lines, or the shipped-path case does not match the golden, stop: that is a suspected shipped bug on clean code (spec §7). Record the exact lines in the evidence log and report.**

- [ ] **Step 3: Add the teardown trace to the orchestrator**

In `extension/src/render/orchestrator.h`, add below `void teardown_gpu();`:

```cpp
	// Debug-only: the label of every step teardown_gpu() ran, in order, for its most recent
	// run. The render lifetime contract pins this sequence (spec 2026-09-14 §5.1); a change in
	// it means the deallocation order changed.
	const std::vector<const char *> &teardown_trace() const { return teardown_trace_; }
```

In the private section, after `Collaborators handles_;`:

```cpp
	std::vector<const char *> teardown_trace_;
```

Replace `RenderOrchestrator::teardown_gpu()` in `orchestrator.cpp` with (statements unchanged; one `push_back` after each step):

```cpp
void RenderOrchestrator::teardown_gpu() {
	// Passes before the atlas: their uniform sets reference atlas RIDs, and freeing a
	// texture cascades to referencing sets (M1's documented order). Islands sit between
	// passes and the atlas pool: RaymarchPass's uniform set references island buffers too.
	// The deletion sequence lives in the three teardown_*() halves below; the interleaved
	// world-owned statements keep their exact positions in the deallocation order via the
	// Collaborator addresses (Task 13 move -- placement unchanged).
	teardown_trace_.clear();
	teardown_render_passes();
	teardown_trace_.push_back("passes");
	if (*handles_.streamer) {
		(*handles_.streamer)->drain_readbacks(rd());
		delete *handles_.streamer;
		*handles_.streamer = nullptr;
	}
	teardown_trace_.push_back("streamer");
	handles_.store->clear_residency(); // slot assignments are meaningless pre-atlas
	teardown_trace_.push_back("residency");
	teardown_island_graph();
	teardown_trace_.push_back("island_graph");
	{
		// island_slot_count() can still be on the render thread during teardown; keep the
		// high-water mark's write under the same mutex.
		std::lock_guard<std::mutex> lock(*handles_.island_mutex);
		*handles_.island_slots = 0;
	}
	teardown_trace_.push_back("island_slots");
	teardown_atlas_pool();
	teardown_trace_.push_back("atlas");
	// The tree holds page indices the pool is about to free, and a stale index would be
	// handed to the next chunk. Pool first, then tree, then the page map.
	if (*handles_.lod_pool) (*handles_.lod_pool)->teardown();
	if (*handles_.lod_tree) (*handles_.lod_tree)->clear();
	handles_.lod_pages_of->clear();
	handles_.lod_page_quads->clear();
	handles_.lod_overflow_logged->clear();
	teardown_trace_.push_back("lod");
	reset_history_state();
	teardown_trace_.push_back("history");
	*handles_.initialized = false;
	teardown_trace_.push_back("initialized");
}
```

- [ ] **Step 4: Add the hook**

In `extension/src/debug/hooks.h`, add `#include <godot_cpp/variant/packed_string_array.hpp>` with the other variant includes, and in the public section after `Dictionary debug_hiz_shutdown_probe();`:

```cpp
	// The step labels RenderOrchestrator::teardown_gpu() recorded on its last run, in order.
	PackedStringArray debug_teardown_trace();
```

In `extension/src/debug/hooks.cpp` `_bind_methods`, after the `debug_hiz_shutdown_probe` binding:

```cpp
	ClassDB::bind_method(D_METHOD("debug_teardown_trace"), &VoxelDebugHooks::debug_teardown_trace);
```

and after the body of `VoxelDebugHooks::debug_hiz_shutdown_probe`:

```cpp
PackedStringArray VoxelDebugHooks::debug_teardown_trace() {
	PackedStringArray out;
	for (const char *step : world_->context().render->teardown_trace()) out.push_back(step);
	return out;
}
```

- [ ] **Step 5: Build and run the suite**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_render_lifetime_contract.gd,res://tests/test_frame_shipped_golden.gd,res://tests/test_shader_reload.gd,res://tests/test_render_shutdown.gd
```
Expected: all six contract cases pass; the other suites match the baseline.

- [ ] **Step 6: Commit the characterization**

```bash
git add tests/test_render_lifetime_contract.gd extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp extension/src/debug/hooks.h extension/src/debug/hooks.cpp
git diff --cached --check
git commit -m "test: characterize render lifetime (teardown, reload, shutdown, upload survival)

Adds a debug teardown trace so teardown order is observable.

"
```

- [ ] **Step 7: Prove each case bites (uncommitted breaks)**

Apply one break, rebuild, run only the contract suite, record the failing case and message, then restore with `git checkout -- <file>` before the next break.

(a) Leak — in `teardown_render_passes()` replace
`if (ssr_pass_) { delete ssr_pass_; ssr_pass_ = nullptr; }` with `ssr_pass_ = nullptr;`.
Expected: `test_teardown_then_reinit_renders_and_frees_clean` fails listing leaked RIDs. Restore `extension/src/render/orchestrator.cpp`.

(b) Order — in `teardown_gpu()` move the two lines
`handles_.store->clear_residency();` / `teardown_trace_.push_back("residency");` above the streamer block.
Expected: `test_gpu_teardown_order_is_pinned` fails. Restore `orchestrator.cpp`.

(c) Survival — in `VoxelWorld::teardown_gpu()` (`extension/src/voxel_world.cpp`) insert as its first line:
`{ std::lock_guard<std::mutex> lock(island_mutex_); island_uploads_.clear(); }`
Expected: `test_a_queued_upload_survives_gpu_teardown` fails with "GPU teardown dropped a queued upload". Restore `voxel_world.cpp`.

Rebuild once more after the last restore and confirm the suite is green.

- [ ] **Step 8: Log the bite proofs and commit**

Append to the evidence log in the baseline file:

```markdown
### Task 4 bite proofs
- (a) skipped SsrPass delete -> <case> failed: <message first line>
- (b) residency before streamer -> <case> failed: <message first line>
- (c) uploads cleared in teardown_gpu -> <case> failed: <message first line>
```

```bash
git add docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git commit -m "docs: lifetime contract bite proofs

"
```

---

### Task 5: `IslandHandoff` — pure module, test-first

A new pure unit with no callers yet. Task 6 wires it in.

**Files:**
- Create: `extension/src/render/island_handoff.h`, `extension/src/render/island_handoff.cpp`
- Create: `extension/tests/test_island_handoff.cpp`
- Modify: `extension/src/render/island_atlas.h` (drop `kMaxIslands`/`IslandSlotDesc`, include the new header), `extension/src/render/island_atlas.cpp` (drop `recompute_world_aabb`)
- Modify: `extension/SConstruct`

**Interfaces:**
- Consumes: `ve::VolumeData`, `ve::VolumeSet` (`generator/volume_set.h`, pure).
- Produces (namespace `godot`):
  - `inline constexpr int kMaxIslands = 32;`
  - `struct IslandSlotDesc` (moved verbatim) with `void recompute_world_aabb();`
  - `class IslandHandoff` with nested `struct Upload { int atlas_slot; int volume_slot; bool to_island_atlas; ve::VolumeData data; }` and `struct Batch { std::vector<Upload> uploads; std::vector<int> normal_releases; std::vector<IslandSlotDesc> descs; bool descs_dirty; }`; methods `void queue_island(int atlas_slot, int volume_slot, const ve::VolumeData &)`, `void queue_field_volume(int volume_slot, const ve::VolumeData &)`, `void discard_field_volume(int volume_slot)`, `void queue_normal_release(int volume_slot)`, `void publish_descriptors(std::vector<IslandSlotDesc>)`, `Batch take()`, `void drop_for_physics_teardown(const ve::VolumeSet &)`, `void note_debug_slot(int slot)`, `void reset_debug_slots()`, `int slot_count(bool islands_enabled) const`, `int pending_uploads() const`, `bool descs_dirty() const`, `int field_volume_uploads() const`, `void note_field_volume_uploaded()`; public member `std::atomic<int> manager_slots{0}`.
  - `bool release_volume_slot(ve::VolumeSet &volumes, IslandHandoff &handoff, int slot);`

- [ ] **Step 1: Write the failing native tests**

Create `extension/tests/test_island_handoff.cpp`:

```cpp
#include <doctest/doctest.h>
#include "render/island_handoff.h"
#include <thread>
#include <vector>

using godot::IslandHandoff;
using godot::IslandSlotDesc;

namespace {
ve::VolumeData volume(uint8_t fill, int dim = 2) {
	ve::VolumeData d;
	d.dim = dim;
	d.sdf.assign(static_cast<size_t>(dim * dim * dim), fill);
	d.mat.assign(static_cast<size_t>(dim * dim * dim), 1);
	return d;
}
} // namespace

TEST_CASE("island handoff: take returns uploads in queue order and empties the queue") {
	IslandHandoff h;
	h.queue_island(3, 7, volume(10));
	h.queue_field_volume(9, volume(20));
	CHECK(h.pending_uploads() == 2);
	IslandHandoff::Batch b = h.take();
	REQUIRE(b.uploads.size() == 2);
	CHECK(b.uploads[0].atlas_slot == 3);
	CHECK(b.uploads[0].volume_slot == 7);
	CHECK(b.uploads[0].to_island_atlas);
	CHECK(b.uploads[1].atlas_slot == -1);
	CHECK(b.uploads[1].volume_slot == 9);
	CHECK_FALSE(b.uploads[1].to_island_atlas);
	CHECK(b.uploads[1].data.sdf[0] == 20);
	CHECK(h.pending_uploads() == 0);
	CHECK(h.take().uploads.empty());
}

TEST_CASE("island handoff: discard removes only field-volume uploads for that slot") {
	IslandHandoff h;
	h.queue_island(0, 4, volume(1));
	h.queue_field_volume(4, volume(2));
	h.queue_field_volume(5, volume(3));
	h.discard_field_volume(4);
	IslandHandoff::Batch b = h.take();
	REQUIRE(b.uploads.size() == 2);
	CHECK(b.uploads[0].to_island_atlas);
	CHECK(b.uploads[0].volume_slot == 4);
	CHECK(b.uploads[1].volume_slot == 5);
}

TEST_CASE("island handoff: the dirty flag is consumed by one take; the descriptors are always handed over") {
	IslandHandoff h;
	CHECK_FALSE(h.descs_dirty());
	std::vector<IslandSlotDesc> d(2);
	d[1].live = true;
	h.publish_descriptors(d);
	CHECK(h.descs_dirty());
	IslandHandoff::Batch first = h.take();
	CHECK(first.descs_dirty);
	REQUIRE(first.descs.size() == 2);
	CHECK(first.descs[1].live);
	CHECK_FALSE(h.descs_dirty());
	IslandHandoff::Batch second = h.take();
	CHECK_FALSE(second.descs_dirty);
	CHECK(second.descs.size() == 2); // the last published set is copied every drain, as before
}

TEST_CASE("island handoff: physics teardown keeps pinned field volumes and drops the rest") {
	ve::VolumeSet volumes;
	REQUIRE(volumes.reserve(2));
	REQUIRE(volumes.store(2, volume(5)));
	REQUIRE(volumes.pin(2));
	REQUIRE(volumes.reserve(3));
	REQUIRE(volumes.store(3, volume(6))); // stored but not pinned
	IslandHandoff h;
	h.queue_field_volume(2, volume(5));
	h.queue_field_volume(3, volume(6));
	h.queue_island(0, 2, volume(5)); // island uploads never survive physics teardown
	h.publish_descriptors(std::vector<IslandSlotDesc>(1));
	h.drop_for_physics_teardown(volumes);
	CHECK_FALSE(h.descs_dirty());
	IslandHandoff::Batch b = h.take();
	REQUIRE(b.uploads.size() == 1);
	CHECK(b.uploads[0].volume_slot == 2);
	CHECK_FALSE(b.uploads[0].to_island_atlas);
	CHECK(b.descs.empty());
}

TEST_CASE("island handoff: slot count is the larger high-water mark, zero when islands are off") {
	IslandHandoff h;
	CHECK(h.slot_count(true) == 0);
	h.manager_slots.store(3);
	h.note_debug_slot(1); // slot 1 -> mark 2
	CHECK(h.slot_count(true) == 3);
	h.note_debug_slot(5); // -> 6
	CHECK(h.slot_count(true) == 6);
	h.note_debug_slot(0); // a lower slot never lowers the mark
	CHECK(h.slot_count(true) == 6);
	CHECK(h.slot_count(false) == 0);
	h.reset_debug_slots();
	CHECK(h.slot_count(true) == 3);
}

TEST_CASE("release_volume_slot queues a normal release only when the slot was freed") {
	ve::VolumeSet volumes;
	REQUIRE(volumes.reserve(1));
	REQUIRE(volumes.store(1, volume(9)));
	REQUIRE(volumes.reserve(2));
	REQUIRE(volumes.store(2, volume(9)));
	REQUIRE(volumes.pin(2));
	IslandHandoff h;
	CHECK(godot::release_volume_slot(volumes, h, 1));
	CHECK_FALSE(godot::release_volume_slot(volumes, h, 2)); // pinned: refused, normals kept
	CHECK_FALSE(godot::release_volume_slot(volumes, h, 1)); // already free
	IslandHandoff::Batch b = h.take();
	REQUIRE(b.normal_releases.size() == 1);
	CHECK(b.normal_releases[0] == 1);
}

TEST_CASE("island handoff: a producer thread and a draining thread lose nothing") {
	IslandHandoff h;
	const size_t kCount = 2000;
	std::thread producer([&] {
		for (size_t i = 0; i < kCount; i++) h.queue_field_volume(static_cast<int>(i % 64), volume(1));
	});
	size_t taken = 0;
	while (taken < kCount) taken += h.take().uploads.size();
	producer.join();
	taken += h.take().uploads.size();
	CHECK(taken == kCount);
}
```

- [ ] **Step 2: Add the source to the native build and link threads**

In `extension/SConstruct`, change the explicit list and add `-pthread`:

```python
test_env.Append(CXXFLAGS=["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-pthread"] + _fp_contract_flags)
test_env.Append(LINKFLAGS=["-pthread"])
```
(replace the existing `CXXFLAGS` line; add the `LINKFLAGS` line right after it) and

```python
for f in ["src/render/shader_loader.cpp", "src/render/camera_params.cpp",
          "src/render/frame_params.cpp", "src/render/island_handoff.cpp"]:
```

- [ ] **Step 3: Run the native build to see it fail**

```bash
(cd extension && scons -Q test) 2>&1 | tail -5
```
Expected: compile error — `render/island_handoff.h` not found.

- [ ] **Step 4: Write the header**

Create `extension/src/render/island_handoff.h`:

```cpp
#pragma once
// IslandHandoff -- island and field-volume bytes, normal releases and slot descriptors on their
// way from the main thread to the render device (spec 2026-09-14 §3.2). Pure: no godot-cpp, in
// the native test build. Owned by RenderOrchestrator and drained by
// RenderOrchestrator::drain_island_uploads on the render thread, before the streamer runs: an
// op that names a volume must never be evaluated before the volume is there.
//
// Locking: mutex_ is a LEAF. It is taken only inside these methods, never while calling out and
// never around another acquisition; callers may already hold WorldStore::edit_mutex(), so the
// order is edit_mutex -> (handoff leaf). Call sites: IslandManager (queue_*, discard_, publish_,
// release_volume_slot), VoxelWorld::teardown_physics (drop_for_physics_teardown),
// RenderOrchestrator::drain_island_uploads (take), RenderOrchestrator::teardown_gpu
// (reset_debug_slots), and the debug facade's island fixtures and diagnostics.
// The slot high-water marks are atomics so the render thread reads them without a lock.
#include <atomic>
#include <mutex>
#include <vector>
#include "generator/volume_set.h"

namespace godot {

// Spec §5's guardrail: "<=32 island bodies".
inline constexpr int kMaxIslands = 32;

// One live island as the raymarcher needs to see it. Written every frame from the body's
// transform, which is why nothing here is a Godot type: IslandManager fills it on the main
// thread and IslandAtlas uploads it on the render thread.
struct IslandSlotDesc {
	bool live = false;
	// The AUTHORITATIVE ve::VolumeSet slot whose bytes this island renders from. Task 6
	// removed IslandAtlas's duplicate SDF/material buffers: the raymarcher reads shared
	// atlas.volumes() buffers indexed by THIS slot (the descriptor's unused integer lane),
	// while the atlas slot keeps selecting descriptor/mip/tile-mask entries.
	int volume_slot = -1;
	// Local -> world rotation, COLUMN major: basis[a] is the world direction of local +a.
	// (ve::resample_volume takes the same rotation ROW major; the two conversions are
	// spelled out where they happen so the transpose is never implicit.)
	float basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	float origin[3] = {0, 0, 0};         // body translation, world
	float lattice_origin[3] = {0, 0, 0}; // the lattice's minimum corner in LOCAL space
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	float aabb_lo[3] = {0, 0, 0}; // world AABB of the rotated lattice box (Task 11's cull)
	float aabb_hi[3] = {0, 0, 0};

	// Fills aabb_lo/hi from the other fields. Called by whoever writes the descriptor.
	void recompute_world_aabb();
};

class IslandHandoff {
public:
	struct Upload {
		int atlas_slot = -1;          // island mip/descriptor entry; -1 = field-volume only
		int volume_slot = -1;         // authoritative ve::VolumeSet slot
		bool to_island_atlas = false; // true = also upload the island min-max mip
		ve::VolumeData data;
	};
	// Everything one drain hands to the render device.
	struct Batch {
		std::vector<Upload> uploads;
		std::vector<int> normal_releases;
		std::vector<IslandSlotDesc> descs; // the last published set, every drain
		bool descs_dirty = false;          // true once per publish
	};

	void queue_island(int atlas_slot, int volume_slot, const ve::VolumeData &data);
	void queue_field_volume(int volume_slot, const ve::VolumeData &data);
	// Removes queued field-volume uploads for the slot (never island uploads).
	void discard_field_volume(int volume_slot);
	void queue_normal_release(int volume_slot);
	void publish_descriptors(std::vector<IslandSlotDesc> descs);
	Batch take();
	// Physics teardown: stale island uploads and descriptors must not reach the next pool,
	// but a field-volume upload whose slot an op already pins is part of the surviving CPU
	// world and must be mirrored into any new GPU pool.
	void drop_for_physics_teardown(const ve::VolumeSet &volumes);

	// IslandManager's slot high-water mark (it stores here; 0 while no manager exists).
	std::atomic<int> manager_slots{0};
	// The debug facade's placed-island latch: raises the mark to slot + 1.
	void note_debug_slot(int slot);
	void reset_debug_slots();
	// High-water mark, not a population: max(debug, manager), or 0 with islands disabled.
	int slot_count(bool islands_enabled) const;

	int pending_uploads() const;
	bool descs_dirty() const;
	int field_volume_uploads() const;
	void note_field_volume_uploaded();

private:
	mutable std::mutex mutex_;
	std::vector<Upload> uploads_;
	std::vector<int> normal_releases_;
	std::vector<IslandSlotDesc> descs_;
	bool descs_dirty_ = false;
	std::atomic<int> debug_slots_{0};
	std::atomic<int> field_volume_uploads_{0};
};

// Releases an authoritative volume slot AND queues the render-thread teardown of its compact
// normals. Pinned slots are refused by VolumeSet::release() and keep their normals (a pasted
// volume-add still names them). Returns release()'s result.
bool release_volume_slot(ve::VolumeSet &volumes, IslandHandoff &handoff, int slot);

} // namespace godot
```

- [ ] **Step 5: Write the implementation**

Create `extension/src/render/island_handoff.cpp`:

```cpp
#include "render/island_handoff.h"
#include <algorithm>

namespace godot {

void IslandSlotDesc::recompute_world_aabb() {
	const float span = static_cast<float>(dim - 1) * voxel;
	for (int a = 0; a < 3; a++) {
		aabb_lo[a] = 1e30f;
		aabb_hi[a] = -1e30f;
	}
	for (int c = 0; c < 8; c++) {
		const float q[3] = {lattice_origin[0] + ((c & 1) ? span : 0.0f),
				lattice_origin[1] + ((c & 2) ? span : 0.0f),
				lattice_origin[2] + ((c & 4) ? span : 0.0f)};
		for (int a = 0; a < 3; a++) {
			// basis is COLUMN major: world_a = sum_k basis[k * 3 + a] * q[k].
			const float w = basis[0 * 3 + a] * q[0] + basis[1 * 3 + a] * q[1] +
					basis[2 * 3 + a] * q[2] + origin[a];
			aabb_lo[a] = std::min(aabb_lo[a], w);
			aabb_hi[a] = std::max(aabb_hi[a], w);
		}
	}
}

void IslandHandoff::queue_island(int atlas_slot, int volume_slot, const ve::VolumeData &data) {
	std::lock_guard<std::mutex> lock(mutex_);
	uploads_.push_back(Upload{atlas_slot, volume_slot, true, data});
}

void IslandHandoff::queue_field_volume(int volume_slot, const ve::VolumeData &data) {
	std::lock_guard<std::mutex> lock(mutex_);
	uploads_.push_back(Upload{-1, volume_slot, false, data});
}

void IslandHandoff::discard_field_volume(int volume_slot) {
	std::lock_guard<std::mutex> lock(mutex_);
	uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(),
						   [volume_slot](const Upload &u) {
							   return !u.to_island_atlas && u.volume_slot == volume_slot;
						   }),
			uploads_.end());
}

void IslandHandoff::queue_normal_release(int volume_slot) {
	std::lock_guard<std::mutex> lock(mutex_);
	normal_releases_.push_back(volume_slot);
}

void IslandHandoff::publish_descriptors(std::vector<IslandSlotDesc> descs) {
	std::lock_guard<std::mutex> lock(mutex_);
	descs_ = std::move(descs);
	descs_dirty_ = true;
}

IslandHandoff::Batch IslandHandoff::take() {
	Batch b;
	std::lock_guard<std::mutex> lock(mutex_);
	b.uploads.swap(uploads_);
	b.normal_releases.swap(normal_releases_);
	b.descs = descs_;
	b.descs_dirty = descs_dirty_;
	descs_dirty_ = false;
	return b;
}

void IslandHandoff::drop_for_physics_teardown(const ve::VolumeSet &volumes) {
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<Upload> keep;
	keep.reserve(uploads_.size());
	for (Upload &u : uploads_)
		if (!u.to_island_atlas && volumes.pinned(u.volume_slot)) keep.push_back(std::move(u));
	uploads_.swap(keep);
	descs_.clear();
	descs_dirty_ = false;
}

void IslandHandoff::note_debug_slot(int slot) {
	int current = debug_slots_.load(std::memory_order_relaxed);
	while (slot + 1 > current &&
			!debug_slots_.compare_exchange_weak(current, slot + 1, std::memory_order_relaxed)) {
	}
}

void IslandHandoff::reset_debug_slots() {
	debug_slots_.store(0, std::memory_order_relaxed);
}

int IslandHandoff::slot_count(bool islands_enabled) const {
	if (!islands_enabled) return 0;
	const int debug = debug_slots_.load(std::memory_order_relaxed);
	const int manager = manager_slots.load(std::memory_order_relaxed);
	return debug > manager ? debug : manager;
}

int IslandHandoff::pending_uploads() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return static_cast<int>(uploads_.size());
}

bool IslandHandoff::descs_dirty() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return descs_dirty_;
}

int IslandHandoff::field_volume_uploads() const {
	return field_volume_uploads_.load(std::memory_order_relaxed);
}

void IslandHandoff::note_field_volume_uploaded() {
	field_volume_uploads_.fetch_add(1, std::memory_order_relaxed);
}

bool release_volume_slot(ve::VolumeSet &volumes, IslandHandoff &handoff, int slot) {
	// The authoritative copy goes first; only a successful release (never a pinned slot --
	// a pasted volume-add still names it) queues the GPU-side normal teardown.
	const bool freed = volumes.release(slot);
	if (freed) handoff.queue_normal_release(slot);
	return freed;
}

} // namespace godot
```

- [ ] **Step 6: Move `IslandSlotDesc` out of `island_atlas`**

In `extension/src/render/island_atlas.h`: delete the `kMaxIslands` constant, the whole `struct IslandSlotDesc { ... };` block and its comment, and add `#include "render/island_handoff.h"` after `#include "generator/volume_set.h"`.

In `extension/src/render/island_atlas.cpp`: delete the `void IslandSlotDesc::recompute_world_aabb() { ... }` definition (lines 22–40 today).

- [ ] **Step 7: Run the native tests**

```bash
(cd extension && scons -Q test) 2>&1 | tail -5
```
Expected: all native tests pass, including 7 new `island handoff` cases.

- [ ] **Step 8: Build the extension (the moved struct must still compile everywhere)**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```
Expected: build succeeds. No gdUnit change is possible yet (no caller).

- [ ] **Step 9: Commit**

```bash
git add extension/src/render/island_handoff.h extension/src/render/island_handoff.cpp extension/tests/test_island_handoff.cpp extension/src/render/island_atlas.h extension/src/render/island_atlas.cpp extension/SConstruct
git diff --cached --check
git commit -m "feat: pure IslandHandoff queue with native tests

IslandSlotDesc and kMaxIslands move with it so the queue compiles without godot-cpp.

"
```

---

### Task 6: The orchestrator owns the handoff; `island_mutex_` is deleted (milestone 1)

**Files:**
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/physics/island_manager.h`, `extension/src/physics/island_manager.cpp`
- Modify: `extension/src/debug/hooks.cpp`

**Interfaces:**
- Consumes: Task 5's `IslandHandoff`, `release_volume_slot`.
- Produces: `IslandHandoff &RenderOrchestrator::handoff()`; `int RenderOrchestrator::island_slot_count() const`; `int RenderOrchestrator::drain_island_uploads(RenderingDevice *)`. `VoxelWorld::island_slot_count()` and `VoxelWorld::drain_island_uploads()` stay as one-line `FrameHost` forwards until Task 9. `RenderOrchestrator::Collaborators` loses `island_mutex` and `island_slots`.

**Locking change (the only one this task makes), deleting four holds and adding none:**
1. `island_slot_count()` no longer locks or dereferences `island_manager_`: it reads two atomics.
2. `ensure_physics_initialized()`: the `island_mutex_` hold around manager creation goes (the `edit_mutex` hold stays).
3. `teardown_physics()`: the `island_mutex_` hold around the detach goes; `manager_slots` is set to 0 at the detach (the `edit_mutex` hold stays).
4. `teardown_gpu()` and `debug_place_test_island_rotated`: the high-water write becomes an atomic store.

- [ ] **Step 1: Orchestrator — own the handoff and the drain**

In `orchestrator.h` add `#include "render/island_handoff.h"`. Remove from `Collaborators` the two fields and their comments:

```cpp
		std::mutex *island_mutex = nullptr; // guards *island_slots during teardown
		int *island_slots = nullptr;        // high-water mark reset under *island_mutex
```

Add to the public section (after `release_devices()`):

```cpp
	// --- island handoff (spec 2026-09-14 §3.2; moved from VoxelWorld) ---
	IslandHandoff &handoff() { return handoff_; }
	// High-water mark for the raymarcher. Render thread; lock-free (two atomics).
	int island_slot_count() const {
		return handoff_.slot_count(handles_.islands_enabled->load(std::memory_order_relaxed));
	}
	// Render thread, before the streamer runs. Returns how many uploads landed.
	int drain_island_uploads(RenderingDevice *device);
```

and to the private members, after `GpuTimings gpu_timings_;`:

```cpp
	IslandHandoff handoff_;
```

In `orchestrator.cpp` add the drain (body moved from `VoxelWorld::drain_island_uploads`; log strings unchanged):

```cpp
int RenderOrchestrator::drain_island_uploads(RenderingDevice *device) {
	if (!device) return 0;
	IslandHandoff::Batch batch = handoff_.take();
	for (const int slot : batch.normal_releases) {
		if (atlas_) atlas_->stored_normals().release_volume(device, slot);
	}
	for (const IslandHandoff::Upload &u : batch.uploads) {
		// SDF/material and compact normals land ONCE, in the shared authoritative pools,
		// indexed by the volume slot. An island upload additionally refreshes its mip at
		// the atlas slot; a field-volume upload follows the identical volume/normal path
		// without one. A missing/malformed/failed normal payload is fail-soft: the pool
		// publishes -1 and the shader falls back to differentiating the R8 atlas.
		if (atlas_ && u.volume_slot >= 0) {
			if (!atlas_->volumes().upload(device, u.volume_slot, u.data))
				UtilityFunctions::printerr("VoxelWorld: field volume upload failed for slot ",
						u.volume_slot);
			atlas_->stored_normals().upload_volume(device, u.volume_slot, u.data);
		} else if (!atlas_ && u.to_island_atlas) {
			UtilityFunctions::printerr("VoxelWorld: no GpuAtlas for island upload of slot ",
					u.volume_slot);
		}
		if (u.to_island_atlas && islands_ && u.atlas_slot >= 0 &&
				!islands_->upload_mip(device, u.atlas_slot, u.data))
			UtilityFunctions::printerr("VoxelWorld: island mip upload failed for slot ",
					u.atlas_slot);
		if (!u.to_island_atlas) handoff_.note_field_volume_uploaded();
	}
	if (batch.descs_dirty && islands_)
		islands_->upload_descriptors(device, batch.descs.data(), static_cast<int>(batch.descs.size()));
	return static_cast<int>(batch.uploads.size());
}
```

In `teardown_gpu()` replace

```cpp
	{
		// island_slot_count() can still be on the render thread during teardown; keep the
		// high-water mark's write under the same mutex.
		std::lock_guard<std::mutex> lock(*handles_.island_mutex);
		*handles_.island_slots = 0;
	}
```
with
```cpp
	// island_slot_count() can still be on the render thread during teardown; the mark is atomic.
	handoff_.reset_debug_slots();
```
(the `teardown_trace_.push_back("island_slots");` line after it stays).

- [ ] **Step 2: `VoxelWorld` — delete the queue, forward the rest**

In `voxel_world.h` delete: the `island_mutex_` member and its comment; the `struct IslandUpload {...}` block with its comment; `island_uploads_`; `pending_normal_releases_` and its comment; `island_slots_`; `island_descs_`; `island_descs_dirty_`; `debug_field_volume_upload_count_`.

In `voxel_world.cpp`:
- Constructor: remove `.island_mutex = &island_mutex_,` and `.island_slots = &island_slots_,`.
- Replace `island_slot_count`, the four queue methods, `release_volume_slot` and `drain_island_uploads` definitions with:

```cpp
int VoxelWorld::island_slot_count() const {
	return context_.render->island_slot_count();
}

void VoxelWorld::queue_island_upload(int atlas_slot, int volume_slot, const ve::VolumeData &d) {
	context_.render->handoff().queue_island(atlas_slot, volume_slot, d);
}

void VoxelWorld::queue_field_volume_upload(int slot, const ve::VolumeData &d) {
	context_.render->handoff().queue_field_volume(slot, d);
	// The worker's volume pool must see the paste before its next field job, otherwise the
	// mesher's collision against the new rubble lags a frame (or more) behind the main copy.
	if (mesh_) mesh_->submit_volume(slot, d);
}

void VoxelWorld::discard_field_volume_upload(int slot) {
	context_.render->handoff().discard_field_volume(slot);
	if (mesh_) mesh_->discard_pending_volume_upload(slot);
}

void VoxelWorld::publish_island_descriptors(const std::vector<IslandSlotDesc> &d) {
	context_.render->handoff().publish_descriptors(d);
}

bool VoxelWorld::release_volume_slot(int slot) {
	return godot::release_volume_slot(store_->volumes(), context_.render->handoff(), slot);
}

int VoxelWorld::drain_island_uploads(RenderingDevice *device) {
	return context_.render->drain_island_uploads(device);
}
```

- `ensure_physics_initialized()`: replace the manager-publication block with

```cpp
	// Publish the manager under edit_mutex_: append_edit_locked() can be called from a tool
	// thread and reads island_manager_ while holding that lock, so creation must not expose a
	// half-initialized pointer to it. The render thread never reads the pointer: it reads the
	// handoff's slot marks.
	{
		std::lock_guard<std::mutex> lock(store_->edit_mutex());
		island_manager_ = new IslandManager();
		island_manager_->initialize(this);
		island_manager_->set_generator(&store_->generator()->sampler());
	}
```

- `teardown_physics()`: replace every line from `// The manager owns the real island bodies;` down to, but not including, `// The worker is going away, but the render atlas and CPU store survive physics teardown.` (that span is the comment, the locked detach, `manager->teardown()`, `physics_bubble_centers_.clear();` and the locked upload filter) with

```cpp
	// The manager owns the real island bodies; tear it down before the mesher's worker and
	// the colliders so its volume-slot bookkeeping still has a live VolumeSet to ask. Hold
	// edit_mutex_ while detaching: a tool thread may already be inside append_edit_locked()
	// reading island_manager_ to call note_edit(). The render thread never reads the pointer;
	// its slot mark drops to 0 at the detach, which is what it saw from a null manager before.
	IslandManager *manager = island_manager_;
	island_manager_ = nullptr;
	context_.render->handoff().manager_slots.store(0, std::memory_order_relaxed);
	if (manager) {
		manager->teardown();
		delete manager;
	}
	physics_bubble_centers_.clear();
	// Drop uploads/descriptors the previous manager queued before the GPU pools are torn
	// down; keep a field-volume upload whose slot the edit log already pins -- those bytes
	// are part of the surviving CPU volume set and MUST reach any new GPU pool.
	context_.render->handoff().drop_for_physics_teardown(store_->volumes());
```

Leave everything after (`consolidation_->rollback_in_flight_for_worker_teardown();` onward) unchanged.

- [ ] **Step 3: `IslandManager` — the slot mark lives on the handoff**

In `island_manager.h`:
- Replace the inline `slot_high_water()` body with a declaration: `int slot_high_water() const;`
- Replace the `DEBUG_ENABLED` inline `debug_set_atlas_slot_used(int slot, bool used) { ... }` with a declaration `void debug_set_atlas_slot_used(int slot, bool used);` (keep the release-build no-op inline in the `#else` branch).
- Delete the member `std::atomic<int> slot_high_water_{0};` and its comment.

In `island_manager.cpp` add `#include "render/orchestrator.h"` (after `voxel_world.h`), and:

```cpp
int IslandManager::slot_high_water() const {
	return world_ ? world_->context().render->handoff().manager_slots.load(std::memory_order_relaxed)
				  : 0;
}

#ifdef DEBUG_ENABLED
void IslandManager::debug_set_atlas_slot_used(int slot, bool used) {
	// Test hook for the 32-island atlas ceiling. Out-of-range slots are ignored; used
	// may only be set for slots the manager can actually hand out.
	if (slot < 0 || slot >= kMaxIslands) return;
	atlas_used_[static_cast<size_t>(slot)] = used ? 1 : 0;
	if (used && world_) {
		std::atomic<int> &mark = world_->context().render->handoff().manager_slots;
		mark.store(std::max(mark.load(std::memory_order_relaxed), slot + 1), std::memory_order_relaxed);
	}
}
#endif
```

Replace `slot_high_water_ = 0;` in `initialize()` with
`world_->context().render->handoff().manager_slots.store(0, std::memory_order_relaxed);`

Replace the bump at `island_manager.cpp:641`:

```cpp
		const int high = std::max(slot_high_water_.load(std::memory_order_relaxed), atlas_slot + 1);
		slot_high_water_.store(high, std::memory_order_relaxed);
```
with
```cpp
		std::atomic<int> &mark = world_->context().render->handoff().manager_slots;
		mark.store(std::max(mark.load(std::memory_order_relaxed), atlas_slot + 1),
				std::memory_order_relaxed);
```

Then: `rg -n 'slot_high_water_' extension/src` — expected: no output.

- [ ] **Step 4: Hooks — read the handoff**

In `extension/src/debug/hooks.cpp` replace the bodies:

```cpp
int VoxelDebugHooks::debug_island_pending_uploads() {
	return world_->context().render->handoff().pending_uploads();
}

int VoxelDebugHooks::debug_field_volume_upload_count() const {
	return world_->context().render->handoff().field_volume_uploads();
}

int VoxelDebugHooks::debug_island_descriptors_pending() {
	return world_->context().render->handoff().descs_dirty() ? 1 : 0;
}
```

```cpp
void VoxelDebugHooks::debug_queue_test_island_descriptors() {
	world_->context().render->handoff().publish_descriptors(std::vector<IslandSlotDesc>(1));
}
```

In `debug_queue_committed_field_volume_upload`, replace

```cpp
	{
		std::lock_guard<std::mutex> lock(world_->island_mutex_);
		world_->island_uploads_.push_back(VoxelWorld::IslandUpload{-1, slot, false, d});
	}
```
with
```cpp
	world_->context().render->handoff().queue_field_volume(slot, d);
```

In `debug_place_test_island_rotated`, replace

```cpp
	{
		std::lock_guard<std::mutex> lock(world_->island_mutex_);
		world_->island_slots_ = std::max(world_->island_slots_, slot + 1);
	}
```
with
```cpp
	world_->context().render->handoff().note_debug_slot(slot);
```

Then:

```bash
rg -n 'island_mutex|island_uploads_|pending_normal_releases_|island_descs_|island_slots_|debug_field_volume_upload_count_' extension/src
```
Expected: no output.

- [ ] **Step 5: Build, native, and island/lifetime suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
(cd extension && scons -Q test) 2>&1 | tail -3
./gdunit_tests.sh -a res://tests/test_render_lifetime_contract.gd,res://tests/test_island_render.gd,res://tests/test_island_body.gd,res://tests/test_island_extract.gd,res://tests/test_connectivity.gd,res://tests/test_brick_diff.gd,res://tests/test_normal_artifact.gd,res://tests/test_field_volume_diff.gd,res://tests/test_repro_pillar_debris.gd,res://tests/test_render_shutdown.gd,res://tests/test_frame_contract.gd
```
Expected: build OK; native green; every suite matches the baseline (counts and failing cases); lifetime contract green including the teardown trace.

- [ ] **Step 6: Commit and log**

Append `### Task 6 gate` with the report path and "matches baseline" (or the attributed differences) to the evidence log.

```bash
git add extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/physics/island_manager.h extension/src/physics/island_manager.cpp extension/src/debug/hooks.cpp docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git diff --cached --check
git commit -m "refactor: RenderOrchestrator owns the island handoff; island_mutex_ is gone

The render thread reads two atomic slot marks instead of dereferencing island_manager_, so
the island_mutex_ holds around manager creation/detach, the teardown_gpu high-water reset and
the placed-island hook are deleted. No lock is added; the handoff mutex is a leaf.

"
```

---

### Task 7: `IslandManager` takes narrow collaborators (milestone 5)

**Files:**
- Modify: `extension/src/physics/island_manager.h`, `extension/src/physics/island_manager.cpp`
- Modify: `extension/src/core/world_store.h`, `extension/src/core/world_store.cpp` (`raycast_down`)
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/debug/hooks.cpp`

**Interfaces:**
- Consumes: Task 6's `RenderOrchestrator::handoff()`; `release_volume_slot(VolumeSet&, IslandHandoff&, int)`.
- Produces:
  - `struct IslandManager::Collaborators { WorldStore *store; IslandHandoff *handoff; MeshService *mesh; Node3D *scene_node; std::vector<float> *bubble_centers; std::function<ve::EditLog::AppendResult(const ve::EditOp &, bool)> append_edit_locked; };`
  - `void IslandManager::initialize(Collaborators handles);` (replaces `initialize(VoxelWorld *)`)
  - `ve::RayHit WorldStore::raycast_down(const float xz[2]);`
  - Deleted from `VoxelWorld`: `queue_island_upload`, `queue_field_volume_upload`, `discard_field_volume_upload`, `publish_island_descriptors`, `set_physics_bubbles`, `analytic_raycast_down`, `release_volume_slot`.

- [ ] **Step 1: `WorldStore::raycast_down`**

In `extension/src/core/world_store.h` add `#include "world/raycast.h"` and, in the public section next to `snapshot_field_sources`:

```cpp
	// A downward ve::raycast at (xz[0], xz[1]) from 200 m, 400 m long, on the generator plus
	// the region ops, volumes and overrides. Takes edit_mutex(). Was
	// VoxelWorld::analytic_raycast_down; IslandManager's one field query.
	ve::RayHit raycast_down(const float xz[2]);
```

In `extension/src/core/world_store.cpp`:

```cpp
ve::RayHit WorldStore::raycast_down(const float xz[2]) {
	ve::RayHit h;
	if (!edit_log()) return h;
	std::lock_guard<std::mutex> lock(edit_mutex());
	const ve::Generator &gen = generator()->sampler();
	const float o[3] = {xz[0], 200.0f, xz[1]};
	const float dir[3] = {0.0f, -1.0f, 0.0f};
	return ve::raycast(gen, *edit_log(), o, dir, 400.0f, &volumes(), overrides());
}
```

Run `(cd extension && scons -Q test) 2>&1 | tail -3`. Expected: native build still green (WorldStore is pure).

- [ ] **Step 2: `IslandManager` header**

In `island_manager.h`:
- Replace `class VoxelWorld;` with forward declarations `class IslandHandoff; class MeshService; class Node3D; class WorldStore;` and add `#include <functional>`.
- Add inside `class IslandManager`, before `~IslandManager();`:

```cpp
	// What the manager needs from the world, and nothing else (spec 2026-09-14 §3.4).
	struct Collaborators {
		// edit_log, edit_mutex, edit_seq, occupancy, volumes, override tables, field
		// snapshots, raycast_down.
		WorldStore *store = nullptr;
		// Island/field-volume bytes and descriptors for the render device; slot mark.
		IslandHandoff *handoff = nullptr;
		// Created before the manager and deleted after it (VoxelWorld physics lifetime).
		MeshService *mesh = nullptr;
		// get_world_3d() for body spaces.
		Node3D *scene_node = nullptr;
		// Body centres as xyz triples; VoxelWorld::physics_tick hands them to the colliders.
		std::vector<float> *bubble_centers = nullptr;
		// VoxelWorld::append_edit_locked. Named debt: sub-project 5's EditPipeline replaces it.
		std::function<ve::EditLog::AppendResult(const ve::EditOp &, bool)> append_edit_locked;
	};
```

- Replace `void initialize(VoxelWorld *world);` with `void initialize(Collaborators handles);`.
- In the private section, replace `VoxelWorld *world_ = nullptr;` with `Collaborators handles_;`, and replace the comment above `gen_` ("Borrowed from WorldStore via VoxelWorld, ...") wording `via VoxelWorld` with `through the store collaborator`.
- Add private helper declarations next to `void publish_descriptors();`:

```cpp
	// Handoff half + mesher half of a field-volume paste (was VoxelWorld::queue_field_volume_upload).
	void queue_field_volume(int slot, const ve::VolumeData &data);
	// Undo of the above for a paste rejected before the uploads drain.
	void discard_field_volume(int slot);
	// Spec §6's "small bubbles around active bodies" (was VoxelWorld::set_physics_bubbles).
	void publish_bubbles();
```

- [ ] **Step 3: Mechanical rewrite of `island_manager.cpp`**

```bash
perl -pi -e '
  s/\bworld_->release_volume_slot\(/release_volume_slot(handles_.store->volumes(), *handles_.handoff, /g;
  s/\bworld_->(occupancy|edit_log|edit_mutex|volumes|edit_seq)\(\)/handles_.store->$1()/g;
  s/\bworld_->(snapshot_field_sources|override_table_for_region)\(/handles_.store->$1(/g;
  s/\bworld_->analytic_raycast_down\(/handles_.store->raycast_down(/g;
  s/\bworld_->mesh_service\(\)/handles_.mesh/g;
  s/\bworld_->get_world_3d\(\)/handles_.scene_node->get_world_3d()/g;
  s/\bworld_->queue_field_volume_upload\(/queue_field_volume(/g;
  s/\bworld_->discard_field_volume_upload\(/discard_field_volume(/g;
  s/\bworld_->queue_island_upload\(/handles_.handoff->queue_island(/g;
  s/\bworld_->publish_island_descriptors\(/handles_.handoff->publish_descriptors(/g;
  s/\bworld_->set_physics_bubbles\(bodies_\)/publish_bubbles()/g;
  s/\bworld_->context\(\)\.render->handoff\(\)\./handles_.handoff->/g;
  s/\bworld_->append_edit_locked\(/handles_.append_edit_locked(/g;
' extension/src/physics/island_manager.cpp
rg -n 'world_' extension/src/physics/island_manager.cpp
```

Expected remaining `world_` hits: the `if (world_)` / `!world_` / `world_ ?` guards, `world_ = ...` assignments, and `slot_high_water()`. Fix each by hand:
- `if (world_)` → `if (handles_.store)`; `world_ ? handles_.store->volumes()...` → `handles_.store ? handles_.store->volumes()...`; `if (!world_ || !handles_.mesh) return 0;` → `if (!handles_.store || !handles_.mesh) return 0;`
- `slot_high_water()` body → `return handles_.handoff ? handles_.handoff->manager_slots.load(std::memory_order_relaxed) : 0;`
- `debug_set_atlas_slot_used`: `if (used && world_) { std::atomic<int> &mark = handles_.handoff->manager_slots;` → guard `if (used && handles_.handoff)`.

`append_edit_locked` had a default `notify_islands = true`; a `std::function` has none. Fix the three one-argument calls (the one at the old line 563 already passes `false`):

```cpp
			const ve::EditLog::AppendResult carve = handles_.append_edit_locked(
					ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM), true);
```
```cpp
					handles_.append_edit_locked(ve::make_volume_add(f.volume_slot, f.origin,
							f.voxel, f.dim), true);
```
```cpp
		const ve::EditLog::AppendResult paste = handles_.append_edit_locked(r.op, true);
```

Replace `initialize` and `teardown`:

```cpp
void IslandManager::initialize(Collaborators handles) {
	teardown();
	handles_ = std::move(handles);
	atlas_used_.assign(kMaxIslands, 0);
	next_id_ = 1;
	next_window_id_ = 1;
	handles_.handoff->manager_slots.store(0, std::memory_order_relaxed);
	connectivity_runs_ = 0;
	islands_spawned_ = 0;
	debris_spawned_ = 0;
	islands_merged_ = 0;
	refused_ = 0;
	last_ms_ = 0.0f;
}

void IslandManager::teardown() {
	for (IslandBody *b : bodies_) {
		if (!b) continue;
		if (handles_.store) release_volume_slot(handles_.store->volumes(), *handles_.handoff, b->info().volume_slot);
		delete b;
	}
	bodies_.clear();
	for (const InFlight &f : in_flight_)
		if (handles_.store) release_volume_slot(handles_.store->volumes(), *handles_.handoff, f.volume_slot);
	in_flight_.clear();
	for (const Merging &m : merging_)
		if (handles_.store) release_volume_slot(handles_.store->volumes(), *handles_.handoff, m.out_slot);
	merging_.clear();
	merge_retries_.clear();
	{
		std::lock_guard<std::mutex> lock(windows_mutex_);
		windows_.clear();
	}
	atlas_used_.clear();
	handles_ = Collaborators{};
}
```

Add the three helpers:

```cpp
void IslandManager::queue_field_volume(int slot, const ve::VolumeData &data) {
	handles_.handoff->queue_field_volume(slot, data);
	// The worker's volume pool must see the paste before its next field job, otherwise the
	// mesher's collision against the new rubble lags a frame (or more) behind the main copy.
	if (handles_.mesh) handles_.mesh->submit_volume(slot, data);
}

void IslandManager::discard_field_volume(int slot) {
	handles_.handoff->discard_field_volume(slot);
	if (handles_.mesh) handles_.mesh->discard_pending_volume_upload(slot);
}

void IslandManager::publish_bubbles() {
	std::vector<float> centers;
	centers.reserve(bodies_.size() * 3);
	for (IslandBody *b : bodies_) {
		if (!b || !b->live()) continue;
		const Vector3 o = b->transform().origin;
		centers.push_back(o.x);
		centers.push_back(o.y);
		centers.push_back(o.z);
	}
	handles_.bubble_centers->swap(centers);
}
```

Replace the includes at the top: `#include "voxel_world.h"` and `#include "render/orchestrator.h"` (added in Task 6) become

```cpp
#include "core/world_store.h"
#include "render/island_handoff.h"
#include <godot_cpp/classes/node3d.hpp>
```

Then `rg -n 'world_|VoxelWorld' extension/src/physics/island_manager.{h,cpp}` — expected: only comments mentioning `VoxelWorld::...` by name.

- [ ] **Step 4: `VoxelWorld` wiring and deletions**

In `ensure_physics_initialized()` replace `island_manager_->initialize(this);` with:

```cpp
		island_manager_->initialize(IslandManager::Collaborators{
				.store = store_.get(),
				.handoff = &context_.render->handoff(),
				.mesh = mesh_,
				.scene_node = this,
				.bubble_centers = &physics_bubble_centers_,
				.append_edit_locked = [this](const ve::EditOp &op, bool notify_islands) {
					return append_edit_locked(op, notify_islands);
				},
		});
```

Delete from `voxel_world.h` and `voxel_world.cpp`: `queue_island_upload`, `queue_field_volume_upload`, `discard_field_volume_upload`, `publish_island_descriptors`, `set_physics_bubbles`, `analytic_raycast_down`, `release_volume_slot` (declarations, comments and definitions).

- [ ] **Step 5: Hooks**

In `hooks.cpp`:
- `world_->queue_island_upload(slot, slot, d);` → `world_->context().render->handoff().queue_island(slot, slot, d);`
- every `world_->release_volume_slot(X)` → `release_volume_slot(world_->context().store->volumes(), world_->context().render->handoff(), X)`

```bash
perl -pi -e 's/\bworld_->release_volume_slot\(/release_volume_slot(world_->context().store->volumes(), world_->context().render->handoff(), /g; s/\bworld_->queue_island_upload\(/world_->context().render->handoff().queue_island(/g' extension/src/debug/hooks.cpp
rg -n 'release_volume_slot|queue_island_upload|analytic_raycast_down|set_physics_bubbles|IslandManager::initialize\(VoxelWorld|initialize\(this\)' extension/src
```
Expected: only `release_volume_slot(` calls of the free function, its declaration/definition in `island_handoff.*`, and none of the other names.

- [ ] **Step 6: Build and gate**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
(cd extension && scons -Q test) 2>&1 | tail -3
./gdunit_tests.sh -a res://tests/test_render_lifetime_contract.gd,res://tests/test_island_render.gd,res://tests/test_island_body.gd,res://tests/test_island_extract.gd,res://tests/test_connectivity.gd,res://tests/test_collider_stream.gd,res://tests/test_repro_pillar_debris.gd,res://tests/test_field_volume_diff.gd
```
Expected: matches baseline.

- [ ] **Step 7: Commit**

Append `### Task 7 gate` to the evidence log.

```bash
git add extension/src/physics/island_manager.h extension/src/physics/island_manager.cpp extension/src/core/world_store.h extension/src/core/world_store.cpp extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/debug/hooks.cpp docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git diff --cached --check
git commit -m "refactor: IslandManager takes its collaborators instead of VoxelWorld

"
```

---

### Task 8: Render lifetime state moves to the orchestrator; `Collaborators` has 5 fields (milestone 2)

**Files:**
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`
- Modify: `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/debug/hooks.cpp`

**Interfaces:**
- Consumes: Task 4 trace; Task 6 handoff.
- Produces on `RenderOrchestrator`:
  ```cpp
  struct Collaborators { const bool *use_local_device; WorldStore *store; LodSystem *lod; Object *callback_owner; std::function<void()> ensure_initialized; };
  bool initialized() const; void mark_initialized();
  WorldStreamer *streamer() const; WorldStreamer **streamer_slot();
  void set_normal_pool_bytes(uint32_t);
  bool last_hiz_readback_was_pending() const; bool last_hiz_readback_was_drained() const;
  void set_sun_state(const ve::SunState &); ve::SunState sun_state() const;
  void set_near_field_scale(float); float near_field_scale() const; bool near_field_enabled() const;
  void set_sun_cascade_min_level(bool); bool sun_cascade_min_level() const;
  FrameSettings frame_settings() const;
  ```
- Produces on `LodSystem`: `void release_gpu();`. Deleted: `Collaborators::near_field_enabled`, `pool_slot()`, `pages_of_slot()`, `page_quads_slot()`, `overflow_logged_slot()`.
- Deleted from `VoxelWorld`: `streamer_`, `initialized_`, `normal_pool_bytes_`, `last_hiz_readback_was_pending_`, `last_hiz_readback_was_drained_`, `islands_enabled_`, `near_field_enabled_`, `near_field_scale_`, `sun_cascade_min_level_`, `sun_mutex_`, `sun_state_`, `sun_state()`.

- [ ] **Step 1: `LodSystem::release_gpu` and the near-field read**

In `lod_system.h`: delete `const std::atomic<bool> *near_field_enabled = nullptr;` from `Collaborators`; delete `pool_slot()`, `pages_of_slot()`, `page_quads_slot()`, `overflow_logged_slot()`; after `void teardown();` add:

```cpp
	// RenderOrchestrator::teardown_gpu()'s LoD step, verbatim: pool, then tree, then the page
	// maps (the tree holds page indices the pool is about to free, and a stale index would be
	// handed to the next chunk). Takes no lock, exactly like the statements it replaces.
	void release_gpu();
```

In `lod_system.cpp`: in `fade_band`, replace `if (!handles_.near_field_enabled->load(std::memory_order_relaxed)) {` with `if (!render()->near_field_enabled()) {`, and add:

```cpp
void LodSystem::release_gpu() {
	if (lod_pool_) lod_pool_->teardown();
	if (lod_tree_) lod_tree_->clear();
	lod_pages_of_.clear();
	lod_page_quads_.clear();
	lod_overflow_logged_.clear();
}
```

- [ ] **Step 2: Orchestrator header**

In `orchestrator.h`:
- Add includes `<functional>` and `"render/frame.h"` (for `FrameSettings`); forward-declare `class LodSystem;`. Remove `#include "lod/lod_tree.h"` if nothing else in the header needs it (`ve::LodKey` was only for the page-map slots).
- Replace the whole `struct Collaborators { ... };` with:

```cpp
	struct Collaborators {
		// Device-selection seam: use_local_device_ stays a VoxelWorld property (ClassDB).
		const bool *use_local_device = nullptr;
		// Config/residency spine steps sit mid-sequence in ensure_gpu_graph().
		WorldStore *store = nullptr;
		// teardown_gpu() calls lod->release_gpu() where the LoD statements always sat.
		LodSystem *lod = nullptr;
		// Callable target for the queued render-thread teardown (see class comment).
		Object *callback_owner = nullptr;
		// pump_shader_reload()'s re-init arm: VoxelWorld::ensure_initialized().
		std::function<void()> ensure_initialized;
	};
```

- Add public, after the island-handoff block:

```cpp
	// --- render lifetime state and per-frame knobs (moved from VoxelWorld, spec 2026-09-14
	// §3.1). Guards unchanged: plain fields stay plain, atomics stay atomic, the sun keeps
	// its own mutex. ---
	bool initialized() const { return initialized_; }
	void mark_initialized() { initialized_ = true; }
	WorldStreamer *streamer() const { return streamer_; }
	WorldStreamer **streamer_slot() { return &streamer_; } // ConsolidationCoordinator wiring
	// Debug-settable compact-normal budget; 0 = GpuAtlasConfig's default. Set before init.
	void set_normal_pool_bytes(uint32_t bytes) { normal_pool_bytes_ = bytes; }
	bool last_hiz_readback_was_pending() const { return last_hiz_readback_was_pending_; }
	bool last_hiz_readback_was_drained() const { return last_hiz_readback_was_drained_; }
	void set_sun_state(const ve::SunState &sun);
	ve::SunState sun_state() const;
	void set_near_field_scale(float v); // clamps to [0.1, 1]
	float near_field_scale() const { return near_field_scale_.load(std::memory_order_relaxed); }
	bool near_field_enabled() const { return near_field_enabled_.load(std::memory_order_relaxed); }
	void set_sun_cascade_min_level(bool v) { sun_cascade_min_level_ = v; }
	bool sun_cascade_min_level() const { return sun_cascade_min_level_; }
	// Everything VoxelFrame samples once per frame.
	FrameSettings frame_settings() const;
```

- Change `island_slot_count()` to read `islands_enabled_.load(std::memory_order_relaxed)` instead of `handles_.islands_enabled->load(...)`.
- Add private members after `IslandHandoff handoff_;`:

```cpp
	WorldStreamer *streamer_ = nullptr; // created inside ensure_gpu_graph(), deleted in teardown_gpu()
	bool initialized_ = false;
	uint32_t normal_pool_bytes_ = 0;
	bool last_hiz_readback_was_pending_ = false;
	bool last_hiz_readback_was_drained_ = true;
	std::atomic<bool> islands_enabled_{true};
	std::atomic<bool> near_field_enabled_{true};
	std::atomic<float> near_field_scale_{0.66f};
	bool sun_cascade_min_level_ = true;
	mutable std::mutex sun_mutex_;
	ve::SunState sun_state_;
```

- [ ] **Step 3: Orchestrator implementation**

```bash
perl -pi -e '
  s/\*handles_\.normal_pool_bytes\b/normal_pool_bytes_/g;
  s/\(\*handles_\.streamer\)/streamer_/g;
  s/\*handles_\.streamer\b/streamer_/g;
  s/\*handles_\.last_hiz_readback_was_pending\b/last_hiz_readback_was_pending_/g;
  s/\*handles_\.last_hiz_readback_was_drained\b/last_hiz_readback_was_drained_/g;
  s/\*handles_\.initialized\b/initialized_/g;
  s/handles_\.islands_enabled->/islands_enabled_./g;
  s/handles_\.near_field_enabled->/near_field_enabled_./g;
  s/handles_\.ensure_initialized_thunk\(handles_\.ensure_initialized_self\)/handles_.ensure_initialized()/g;
' extension/src/render/orchestrator.cpp
rg -n 'handles_\.' extension/src/render/orchestrator.cpp
```
Expected remaining `handles_.` uses: `use_local_device`, `store`, `callback_owner`, `ensure_initialized()`, and the LoD block in `teardown_gpu()`. Replace that LoD block

```cpp
	if (*handles_.lod_pool) (*handles_.lod_pool)->teardown();
	if (*handles_.lod_tree) (*handles_.lod_tree)->clear();
	handles_.lod_pages_of->clear();
	handles_.lod_page_quads->clear();
	handles_.lod_overflow_logged->clear();
```
with
```cpp
	handles_.lod->release_gpu();
```
(keep its comment and the `teardown_trace_.push_back("lod");` after it). Add `#include "lod/lod_system.h"` to `orchestrator.cpp`, and:

```cpp
void RenderOrchestrator::set_sun_state(const ve::SunState &sun) {
	std::lock_guard<std::mutex> lock(sun_mutex_);
	sun_state_ = sun;
}

ve::SunState RenderOrchestrator::sun_state() const {
	std::lock_guard<std::mutex> lock(sun_mutex_);
	return sun_state_;
}

void RenderOrchestrator::set_near_field_scale(float v) {
	near_field_scale_.store(v < 0.1f ? 0.1f : (v > 1.0f ? 1.0f : v), std::memory_order_relaxed);
}

FrameSettings RenderOrchestrator::frame_settings() const {
	FrameSettings s;
	s.sun = sun_state();
	s.near_field_scale = near_field_scale();
	s.near_field_enabled = near_field_enabled();
	s.sun_cascade_min_level = sun_cascade_min_level_;
	return s;
}
```

- [ ] **Step 4: `VoxelWorld`**

In `voxel_world.h`, delete the members listed in Interfaces (with their comments), the `sun_state()` method, `LodPool`-slot comments, and change these inline bodies:

```cpp
	void set_near_field_scale(float v) { context_.render->set_near_field_scale(v); }
	float get_near_field_scale() const { return context_.render->near_field_scale(); }
	bool is_initialized() const { return context_.render->initialized(); }
	void set_sun_cascade_min_level(bool v) { context_.render->set_sun_cascade_min_level(v); }
	bool get_sun_cascade_min_level() const { return context_.render->sun_cascade_min_level(); }
	WorldStreamer *streamer() override { return context_.render->streamer(); }
```
and replace the `frame_settings()` declaration with `FrameSettings frame_settings() const override { return context_.render->frame_settings(); }`.

In `voxel_world.cpp`:
- Delete the `VoxelWorld::frame_settings()` definition.
- `update_sun_state()`: replace its last two lines (`std::lock_guard... sun_state_ = s;`) with `context_.render->set_sun_state(s);`.
- `publish_sun_state_to_local_device`: `ubo->update(device, sun_state())` → `ubo->update(device, context_.render->sun_state())`.
- `ensure_initialized()`: `if (initialized_) return;` → `if (context_.render->initialized()) return;`; `initialized_ = true;` → `context_.render->mark_initialized();`.
- `ensure_physics_initialized()`: `if (streamer_) streamer_->set_mesh_service(mesh_);` → `if (WorldStreamer *s = context_.render->streamer()) s->set_mesh_service(mesh_);`
- `teardown_physics()`: `if (streamer_) streamer_->set_mesh_service(nullptr);` → `if (WorldStreamer *s = context_.render->streamer()) s->set_mesh_service(nullptr);`
- Constructor: the LodSystem collaborators lose `.near_field_enabled = &near_field_enabled_,`. Replace the orchestrator construction with:

```cpp
	// The GPU pass graph, device ownership and every piece of render lifetime state live in
	// RenderOrchestrator. Created AFTER LodSystem, whose release_gpu() its teardown calls.
	render_ = std::make_unique<RenderOrchestrator>(RenderOrchestrator::Collaborators{
			.use_local_device = &use_local_device_,
			.store = store_.get(),
			.lod = lod_.get(),
			.callback_owner = this,
			.ensure_initialized = [this]() { ensure_initialized(); },
	});
```
- ConsolidationCoordinator collaborators: `.streamer = &streamer_,` → `.streamer = context_.render->streamer_slot(),`.

Then:

```bash
rg -n '\b(streamer_|initialized_|normal_pool_bytes_|last_hiz_readback_was_\w+_|islands_enabled_|near_field_enabled_|near_field_scale_|sun_cascade_min_level_|sun_mutex_|sun_state_)\b' extension/src/voxel_world.h extension/src/voxel_world.cpp
```
Expected: no output.

- [ ] **Step 5: Hooks**

```bash
perl -pi -e '
  s/\b(world_|w)->initialized_\b/$1->is_initialized()/g;
  s/\b(world_|w)->streamer_\b/$1->context().render->streamer()/g;
  s/\b(world_|w)->normal_pool_bytes_ = (.*);/$1->context().render->set_normal_pool_bytes($2);/g;
  s/\b(world_|w)->last_hiz_readback_was_(pending|drained)_\b/$1->context().render->last_hiz_readback_was_$2()/g;
  s/\b(world_|w)->islands_enabled_\.load\(std::memory_order_relaxed\)/$1->get_effect_enabled("islands")/g;
' extension/src/debug/hooks.cpp
rg -n '\b(world_|w)->(initialized_|streamer_|normal_pool_bytes_|last_hiz_readback_was_\w+_|islands_enabled_)' extension/src/debug/hooks.cpp
```
Expected: no output.

- [ ] **Step 6: Build and gate**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
(cd extension && scons -Q test) 2>&1 | tail -3
./gdunit_tests.sh -a res://tests/test_render_lifetime_contract.gd,res://tests/test_frame_shipped_golden.gd,res://tests/test_frame_contract.gd,res://tests/test_shader_reload.gd,res://tests/test_render_shutdown.gd,res://tests/test_world_store_contract.gd,res://tests/test_near_field_scale.gd,res://tests/test_lod_render.gd,res://tests/test_sun_shadow.gd,res://tests/test_island_render.gd,res://tests/test_gpu_timings.gd
```
Expected: matches baseline; the teardown trace is unchanged; the shipped golden is unchanged.

Count the fields: `sed -n '/struct Collaborators {/,/};/p' extension/src/render/orchestrator.h | rg -c ' = |ensure_initialized;'` — expected `5`.

- [ ] **Step 7: Commit**

Append `### Task 8 gate` to the evidence log.

```bash
git add extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp extension/src/lod/lod_system.h extension/src/lod/lod_system.cpp extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/debug/hooks.cpp docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git diff --cached --check
git commit -m "refactor: render lifetime state lives on RenderOrchestrator; Collaborators has 5 fields

Streamer, initialized flag, normal-pool budget, HiZ readback end state, effect toggles, near-field
scale, sun state and the cascade clamp move from VoxelWorld. LodSystem::release_gpu() replaces the
five LoD page-map slots; teardown order is unchanged (teardown trace pinned).

"
```

---

### Task 9: `VoxelFrame` lives under the orchestrator; `FrameHost` is deleted (milestone 3, part 1)

**Files:**
- Modify: `extension/src/render/frame.h`, `extension/src/render/frame.cpp`
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`
- Modify: `extension/src/core/world_store.h`, `extension/src/core/world_store.cpp` (`region_window`)
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/raymarch_compositor.cpp`, `extension/src/beauty_compositor.cpp`
- Modify: `extension/src/debug/hooks.cpp`

**Interfaces:**
- Consumes: Task 8's `frame_settings()`, `streamer()`; Task 6's `island_slot_count()`, `drain_island_uploads()`.
- Produces: `VoxelFrame(RenderOrchestrator &, LodSystem &, WorldStore &)`; `VoxelFrame &RenderOrchestrator::frame()`; `ve::RegionWindow WorldStore::region_window() const`. Deleted: `class FrameHost`, `VoxelWorld::frame()`, `VoxelWorld::frame_`, `VoxelFrame::region_window()`, and `VoxelWorld`'s `island_slot_count`, `drain_island_uploads`, `streamer`, `frame_settings`.

- [ ] **Step 1: `WorldStore::region_window`**

`world_store.h`, public, next to `residency()`:

```cpp
	// The near-field region map's current window; an empty window before residency exists.
	ve::RegionWindow region_window() const;
```
(add `#include "world/region_window.h"` if the header does not already see `ve::RegionWindow`.)

`world_store.cpp` (add `#include "world/residency.h"` if absent):

```cpp
ve::RegionWindow WorldStore::region_window() const {
	return residency_ ? residency_->window() : ve::RegionWindow{};
}
```

Run `(cd extension && scons -Q test) 2>&1 | tail -3` — expected green.

- [ ] **Step 2: Frame**

In `frame.h`: delete the `FrameHost` class and its comment; change the constructor to `VoxelFrame(RenderOrchestrator &render, LodSystem &lod, WorldStore &store);`; delete the private `ve::RegionWindow region_window() const;` and the `FrameHost &host_;` member; update the `FrameSettings` comment to "Per-frame values RenderOrchestrator owns, sampled once per call."

In `frame.cpp`:

```bash
perl -pi -e '
  s/host_\.frame_settings\(\)/render_.frame_settings()/g;
  s/host_\.drain_island_uploads\(/render_.drain_island_uploads(/g;
  s/host_\.streamer\(\)/render_.streamer()/g;
  s/host_\.island_slot_count\(\)/render_.island_slot_count()/g;
  s/\bregion_window\(\)/store_.region_window()/g;
' extension/src/render/frame.cpp
```
Then by hand: delete the `VoxelFrame::region_window()` definition (the perl turned its signature into nonsense — remove the whole 3-line function and its `// Was VoxelWorld::region_window().` comment), and change the constructor to:

```cpp
VoxelFrame::VoxelFrame(RenderOrchestrator &render, LodSystem &lod, WorldStore &store) :
		render_(render), lod_(lod), store_(store) {}
```

`rg -n 'host_|FrameHost' extension/src` — expected: no output after Step 4.

- [ ] **Step 3: Orchestrator owns the frame**

`orchestrator.h`, public after `frame_settings()`:

```cpp
	// The one ordered run of every voxel render stage (render/frame.h). Compositors and the
	// headless debug probes call it.
	VoxelFrame &frame() { return frame_; }
```

private, as the LAST member of the class (destroyed first; it references this object, the LoD
runtime and the store):

```cpp
	VoxelFrame frame_;
```

`orchestrator.cpp` constructor:

```cpp
RenderOrchestrator::RenderOrchestrator(Collaborators handles) :
		handles_(std::move(handles)), frame_(*this, *handles_.lod, *handles_.store) {}
```

- [ ] **Step 4: `VoxelWorld`**

`voxel_world.h`:
- `class VoxelWorld : public Node3D, public EditSink, public FrameHost {` → `class VoxelWorld : public Node3D, public EditSink {`
- Move the `std::unique_ptr<LodSystem> lod_;` declaration (with its comment) to directly **above** `std::unique_ptr<RenderOrchestrator> render_;`, and change its comment to: `// Declared before render_: the orchestrator owns the frame, which references this LoD runtime, so the LoD runtime must be destroyed after it.`
- Delete `std::unique_ptr<VoxelFrame> frame_;` with its comment, `VoxelFrame *frame() { ... }`, and the four FrameHost overrides (`island_slot_count`, `drain_island_uploads`, `streamer`, `frame_settings`) with their comments.

`voxel_world.cpp`:
- Delete the definitions of `VoxelWorld::island_slot_count` and `VoxelWorld::drain_island_uploads`.
- Delete the constructor's last statement `frame_ = std::make_unique<VoxelFrame>(*render_, *lod_, *store_, *this);` and its comment.
- `teardown_gpu()` and `_exit_tree()`: `if (frame_) frame_->release_gpu();` → `context_.render->frame().release_gpu();`
- `sun_ortho`: `return frame_->sun_ortho(cascade);` → `return context_.render->frame().sun_ortho(cascade);`
- Any remaining `island_slot_count()` call inside `voxel_world.cpp` → `context_.render->island_slot_count()`.

- [ ] **Step 5: Compositors**

In both compositor `.cpp` files add `#include "render/orchestrator.h"` and change the last line:

```cpp
	world->context().render->frame().render_pre_opaque(rd, in);   // raymarch_compositor.cpp
	world->context().render->frame().render_post_opaque(rd, in);  // beauty_compositor.cpp
```

- [ ] **Step 6: Hooks**

```bash
perl -pi -e '
  s/!(world_|w)->frame\(\) \|\| //g;
  s/ \|\| !(world_|w)->frame\(\)//g;
  s/\b(world_|w)->frame\(\)->/$1->context().render->frame()./g;
  s/\b(world_|w)->island_slot_count\(\)/$1->context().render->island_slot_count()/g;
  s/\b(world_|w)->drain_island_uploads\(/$1->context().render->drain_island_uploads(/g;
' extension/src/debug/hooks.cpp
rg -n '(world_|w)->frame\(\)|->island_slot_count\(\)|->drain_island_uploads\(' extension/src/debug/hooks.cpp | rg -v 'context\(\)\.render'
```
Expected: no output. If a guard such as `if (!world_->frame()) return d;` survives, delete that line: the frame is a member and cannot be null.

- [ ] **Step 7: Build and gate**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
(cd extension && scons -Q test) 2>&1 | tail -3
rg -n 'class FrameHost|FrameHost' extension/src
./gdunit_tests.sh -a res://tests/test_render_lifetime_contract.gd,res://tests/test_frame_shipped_golden.gd,res://tests/test_frame_contract.gd,res://tests/test_ssao.gd,res://tests/test_deferred.gd,res://tests/test_ssgi.gd,res://tests/test_contact_shadows.gd,res://tests/test_lod_seam.gd,res://tests/test_island_render.gd,res://tests/test_render_shutdown.gd
```
Use `ls tests | rg 'ssao|deferred|ssgi|contact'` first and pass the actual file names of the four migrated-probe suites. Expected: `rg` for `FrameHost` prints nothing; suites match the baseline; shipped golden unchanged.

- [ ] **Step 8: Commit**

Append `### Task 9 gate` to the evidence log.

```bash
git add extension/src/render/frame.h extension/src/render/frame.cpp extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp extension/src/core/world_store.h extension/src/core/world_store.cpp extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/raymarch_compositor.cpp extension/src/beauty_compositor.cpp extension/src/debug/hooks.cpp docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git diff --cached --check
git commit -m "refactor: RenderOrchestrator owns VoxelFrame; FrameHost is deleted

The frame reads the island handoff, streamer and frame settings from the orchestrator.
WorldStore::region_window() replaces two copies of the same body.

"
```

---

### Task 10: `RenderPasses` table; `VoxelWorld`'s C++-only forwarders are deleted (milestone 3, part 2)

**Files:**
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`
- Modify: `extension/src/render/frame.cpp`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/raymarch_compositor.cpp`, `extension/src/debug/hooks.cpp`

**Interfaces:**
- Consumes: Task 9.
- Produces:
  ```cpp
  struct RenderPasses { GpuAtlas *atlas; MaterialAtlas *materials; IslandAtlas *islands; IslandCullPass *island_cull;
      RegionPass *region; BrickGenPass *gen; RaymarchPass *raymarch; CompositePass *composite; DeferredPass *deferred;
      SunShadowPass *sun_shadow; SunUbo *sun_ubo; FieldContextSet *field_context; InjectPass *inject;
      LodRasterPass *lod_raster; LodCullPass *lod_cull; HizPass *hiz; GBuffer *gbuffer; CameraUbo *beauty_camera;
      ContactShadowPass *contact_shadow; SsgiPass *ssgi; SsaoPass *ssao; SsrPass *ssr; OutlinePass *outline;
      GrassScatterPass *grass_scatter; GrassRasterPass *grass_raster; };
  const RenderPasses &RenderOrchestrator::passes() const;
  ```
  The 25 per-pass accessors on the orchestrator and every C++-only forwarder on `VoxelWorld` are deleted. `VoxelWorld` keeps: bound methods, `rd()`, `sun_cascade_count()`, `mesh_service()`, `edit_seq()`, `append_edit*`, `teardown_gpu`, the lifetime/physics methods, `extract_component`, `set_generator` (Task 12 removes the last two from the private/dead list).

- [ ] **Step 1: The table in `orchestrator.h`**

Directly above `class RenderOrchestrator {` add:

```cpp
// Every GPU object of the pass graph. The orchestrator creates them in ensure_gpu_graph() and
// deletes them in teardown_gpu()'s halves, in the load-bearing orders documented there; a null
// field means "not built" (never initialized, failed soft, or torn down). VoxelFrame, LodSystem
// and the debug facade's pass probes read it; nobody else creates or deletes these objects.
struct RenderPasses {
	GpuAtlas *atlas = nullptr;
	MaterialAtlas *materials = nullptr;
	IslandAtlas *islands = nullptr;
	IslandCullPass *island_cull = nullptr;
	RegionPass *region = nullptr;
	BrickGenPass *gen = nullptr;
	RaymarchPass *raymarch = nullptr;
	CompositePass *composite = nullptr;
	DeferredPass *deferred = nullptr;
	SunShadowPass *sun_shadow = nullptr;
	SunUbo *sun_ubo = nullptr;
	FieldContextSet *field_context = nullptr;
	InjectPass *inject = nullptr;
	LodRasterPass *lod_raster = nullptr;
	LodCullPass *lod_cull = nullptr;
	HizPass *hiz = nullptr;
	GBuffer *gbuffer = nullptr;
	CameraUbo *beauty_camera = nullptr;
	ContactShadowPass *contact_shadow = nullptr;
	SsgiPass *ssgi = nullptr;
	SsaoPass *ssao = nullptr;
	SsrPass *ssr = nullptr;
	OutlinePass *outline = nullptr;
	GrassScatterPass *grass_scatter = nullptr;
	GrassRasterPass *grass_raster = nullptr;
};
```

Replace the accessor block from `// --- accessors: one per moved pass pointer ---` through `GrassRasterPass *grass_raster_pass() { return grass_raster_pass_; }` with:

```cpp
	const RenderPasses &passes() const { return passes_; }
```
(keep the following `grass_settings`, `set_grass_value`, `grass_value` and `gpu_timings` lines).

Replace the member block from `// Member ORDER mirrors the pre-split block in voxel_world.h.` / `GpuAtlas *atlas_ = nullptr;` through `GrassRasterPass *grass_raster_pass_ = nullptr;` with:

```cpp
	RenderPasses passes_;
```

Change `GpuAtlas **atlas_slot() { return &atlas_; }` to `GpuAtlas **atlas_slot() { return &passes_.atlas; }`.

- [ ] **Step 2: Rename the orchestrator's member uses (dry-run verified: 103 rewrites)**

```bash
perl -pi -e 's/\b(atlas|materials|islands|island_cull|field_context|gbuffer|beauty_camera|sun_ubo)_\b/passes_.$1/g; s/\b(region|gen|raymarch|composite|deferred|sun_shadow|inject|lod_raster|lod_cull|hiz|contact_shadow|ssgi|ssao|ssr|outline|grass_scatter|grass_raster)_pass_\b/passes_.$1/g' extension/src/render/orchestrator.cpp
rg -n '[a-z]_pass_\b|\b(atlas|materials|islands|gbuffer|sun_ubo|beauty_camera|field_context|island_cull)_\b' extension/src/render/orchestrator.cpp extension/src/render/orchestrator.h
```
Expected: no output (`islands_enabled_` does not match: no word boundary after `islands_`).

- [ ] **Step 3: Rename every caller's accessor calls (dry-run verified: frame 34, hooks 335 lines, LoD 7)**

Save as `$TMPDIR/passes_rename.pl` (a one-off tool; do not commit):

```perl
#!/usr/bin/perl -pi
# Accessor call -> RenderPasses field. Only receivers that name the orchestrator's accessors or
# VoxelWorld's forwarders are rewritten.
BEGIN {
  %f = (atlas=>'atlas', material_atlas=>'materials', materials=>'materials', islands=>'islands',
    island_cull=>'island_cull', region_pass=>'region', gen_pass=>'gen', raymarch_pass=>'raymarch',
    composite_pass=>'composite', deferred_pass=>'deferred', sun_shadow_pass=>'sun_shadow',
    sun_ubo=>'sun_ubo', field_context=>'field_context', inject_pass=>'inject',
    lod_raster_pass=>'lod_raster', lod_cull_pass=>'lod_cull', hiz_pass=>'hiz', gbuffer=>'gbuffer',
    beauty_camera=>'beauty_camera', contact_shadow_pass=>'contact_shadow', ssgi_pass=>'ssgi',
    ssao_pass=>'ssao', ssr_pass=>'ssr', outline_pass=>'outline',
    grass_scatter_pass=>'grass_scatter', grass_raster_pass=>'grass_raster');
  $alt = join '|', sort { length($b) <=> length($a) } keys %f;
}
s/\b(world_->|w->|world->)($alt)\(\)/$1context().render->passes().$f{$2}/g;
s/\b(render_\.|render\(\)->|context_\.render->|render->)($alt)\(\)/$1passes().$f{$2}/g;
```

```bash
for f in extension/src/render/frame.cpp extension/src/lod/lod_system.cpp extension/src/voxel_world.cpp extension/src/debug/hooks.cpp; do
  perl -pi "$TMPDIR/passes_rename.pl" "$f"
done
```

- [ ] **Step 4: Route the remaining forwarder calls through `context()`**

Save as `$TMPDIR/facade_rename.pl` (one-off, not committed):

```perl
#!/usr/bin/perl -pi
s/\b(world_|w)->(edit_mutex|edit_log|volumes|occupancy|region_window|drain_occupancy)\(\)/$1->context().store->$2()/g;
s/\b(world_|w)->(snapshot_field_sources|override_table_for_region)\(/$1->context().store->$2(/g;
s/\b(world_|w)->(beauty_settings|grass_settings|gpu_timings|has_history|prev_view_proj|beauty_frame|local_rd)\(\)/$1->context().render->$2()/g;
s/\b(world_|w|world)->(reload_snapshot|beauty_snapshot|finish_beauty_frame|downsample_history)\(/$1->context().render->$2(/g;
s/\b(world_|w)->get_normal_roughness_state\(\)/$1->context().render->normal_roughness_state()/g;
s/\b(world_|w|world)->pump_shader_reload\(\)/$1->context().render->pump_shader_reload()/g;
s/\b(world_|w)->lod_tick\(/$1->context().lod->tick(/g;
s/\b(world_|w)->prepare_lod_raster\(\)/$1->context().lod->prepare_raster()/g;
s/\b(world_|w)->prepare_lod_shadow_raster\(/$1->context().lod->prepare_shadow_raster(/g;
s/\b(world_|w)->lod_fade_band\(/$1->context().lod->fade_band(/g;
s/\b(world_|w)->gather_lod_ops\(/$1->context().lod->gather_ops(/g;
s/\b(world_|w)->lod_pool\(\)/$1->context().lod->pool()/g;
s/\b(world_|w)->sun_ortho\(/$1->context().render->frame().sun_ortho(/g;
```

```bash
for f in extension/src/debug/hooks.cpp extension/src/raymarch_compositor.cpp; do
  perl -pi "$TMPDIR/facade_rename.pl" "$f"
done
```

`drain_occupancy` is public on `WorldStore` already; `VoxelWorld::drain_occupancy` was a private one-liner.

- [ ] **Step 5: Delete the forwarders from `VoxelWorld`**

From `voxel_world.h` delete these declarations/inline bodies and their comments: `drain_occupancy`, `field_context`, `reload_snapshot`, `beauty_snapshot`, `set_normal_roughness_state`, `get_normal_roughness_state`, `lod_tick`, `prepare_lod_raster`, `prepare_lod_shadow_raster`, `sun_ortho`, `gpu_timings`, `atlas`, `material_atlas`, `islands`, `region_window`, `grass_reach_limit_m`, `edit_log`, `volumes`, every `*_pass()` accessor, `island_cull`, `sun_ubo`, `lod_pool`, `hiz_pass`, `gbuffer`, `beauty_camera`, `grass_settings`, `local_rd`, `prev_view_proj`, `has_history`, `beauty_frame`, `finish_beauty_frame`, `edit_mutex`, `downsample_history`, `occupancy`, `snapshot_field_sources`, `lod_fade_band`, `override_table_for_region`, `gather_lod_ops`, `beauty_settings`, `pump_shader_reload`.

From `voxel_world.cpp` delete their definitions: `beauty_settings`, `grass_scatter_pass`, `grass_raster_pass`, `grass_settings`, `downsample_history`, `finish_beauty_frame`, `field_context`, `gather_lod_ops`, `snapshot_field_sources`, `lod_tick`, `prepare_lod_raster`, `prepare_lod_shadow_raster`, `sun_ortho`, `lod_fade_band`, `override_table_for_region`, `pump_shader_reload`. Then inside the file:
- `_process`: `drain_occupancy();` → `store_->drain_occupancy();`
- `extract_component`: `override_table_for_region(` → `store_->override_table_for_region(`; `snapshot_field_sources(` → `store_->snapshot_field_sources(`
- `set_grass_value` / `get_grass_value` bodies are kept (bound).
- `rd()` is kept (it publishes the sun on a local device).

Also delete the blank-line runs left in `voxel_world.cpp` (the SP1 decomposition left ~200 empty lines): `cat -s extension/src/voxel_world.cpp > "$TMPDIR/vw.cpp" && mv "$TMPDIR/vw.cpp" extension/src/voxel_world.cpp`.

- [ ] **Step 6: Build, fixing residual callers without re-adding forwarders**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | rg -n 'error' | head -40
```
For each `no member named 'X' in 'godot::VoxelWorld'` error, route the call through `context().store` / `context().render` / `context().render->passes()` / `context().lod` exactly as the two scripts do. For each `no member named 'X' in 'godot::RenderOrchestrator'`, use `passes().<field>`. Never re-add a deleted forwarder. Repeat until the build is clean.

- [ ] **Step 7: Gate**

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
rg -n 'raymarch_pass\(\)|composite_pass\(\)|deferred_pass\(\)|gbuffer\(\)|material_atlas\(\)' extension/src
./gdunit_tests.sh
```
Expected: `rg` prints nothing; the **full** gdUnit run matches the baseline (counts and failing cases); lifetime contract, shipped golden and frame contract green. Record the report path.

- [ ] **Step 8: Commit**

Append `### Task 10 gate (full run)` with the report path to the evidence log.

```bash
git add extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp extension/src/render/frame.cpp extension/src/lod/lod_system.cpp extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/raymarch_compositor.cpp extension/src/debug/hooks.cpp docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git diff --cached --check
git commit -m "refactor: one RenderPasses table replaces 25 pass accessors and VoxelWorld's forwarders

C++ callers reach modules through VoxelWorld::context(); the GDScript surface is unchanged.

"
```

---

### Task 11: S8 — the SSAO timing scope must not sit inside "deferred"

Bug rule: failing test, then a `fix:` commit.

**Files:**
- Create: `tests/test_gpu_timing_scopes.gd`
- Modify: `extension/src/render/frame.cpp` (one moved line)

**Interfaces:**
- Consumes: marker format `ve:<serial>:<pass>:<occurrence>:<b|e>` (`gpu_timings.cpp:73`); bound `set_effect_enabled`.
- Produces: nothing new.

- [ ] **Step 1: Write the failing test**

Create `tests/test_gpu_timing_scopes.gd`:

```gdscript
extends GdUnitTestSuite

# S8 (docs/superpowers/specs/2026-09-13-frame-module-design.md §10): the "deferred" GPU timing
# scope contained the "ssao" scope, so "deferred" reported SSAO's time as its own and
# "unattributed" subtracted SSAO twice. Timestamp VALUES are invalid on this machine, but the
# capture's marker NAMES arrive in command order (verified on Metal, 2026-09-14), so this pins
# the order: ssao's end marker precedes deferred's begin marker within one frame serial.

const W := 128
const H := 72

var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

# Index of marker ve:<serial>:<pass>:<any occurrence>:<side>, or -1.
func marker_index(names: PackedStringArray, serial: String, pass_name: String, side: String) -> int:
	for i in range(names.size()):
		var f := names[i].split(":")
		if f.size() == 5 and f[0] == "ve" and f[1] == serial and f[2] == pass_name and f[4] == side:
			return i
	return -1

func test_ssao_scope_closes_before_deferred_opens(timeout := 120000) -> void:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = false
	world.physics_enabled = false
	add_child(world)
	_nodes.append(world)
	world.set_effect_enabled("ssao", true)
	var raymarch: RaymarchCompositor = ClassDB.instantiate("RaymarchCompositor")
	raymarch.world_path = world.get_path()
	var beauty: BeautyCompositor = ClassDB.instantiate("BeautyCompositor")
	beauty.world_path = world.get_path()
	var effects: Array[CompositorEffect] = [raymarch, beauty]
	var compositor := Compositor.new()
	compositor.compositor_effects = effects
	var vp := SubViewport.new()
	vp.size = Vector2i(W, H)
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(vp)
	_nodes.append(vp)
	var cam := Camera3D.new()
	cam.fov = 60.0
	cam.near = 0.05
	cam.far = 4000.0
	cam.compositor = compositor
	vp.add_child(cam)
	cam.current = true
	cam.global_position = Vector3(30.0, 70.0, 30.0)
	cam.look_at(Vector3(30.2, 69.0, 30.2), Vector3.UP)

	var rd := RenderingServer.get_rendering_device()
	var ssao_end := -1
	var deferred_begin := -1
	var names := PackedStringArray()
	for frame in range(600):
		await RenderingServer.frame_post_draw
		names = PackedStringArray()
		for k in range(rd.get_captured_timestamps_count()):
			names.append(rd.get_captured_timestamp_name(k))
		var serial := ""
		for n in names:
			var f := n.split(":")
			if f.size() == 5 and f[0] == "ve" and f[2] == "deferred" and f[4] == "b":
				serial = f[1]
		if serial.is_empty():
			continue
		ssao_end = marker_index(names, serial, "ssao", "e")
		deferred_begin = marker_index(names, serial, "deferred", "b")
		if ssao_end >= 0 and deferred_begin >= 0:
			break
	assert_int(deferred_begin).override_failure_message(
		"no captured frame held both scopes: %s" % names).is_greater(-1)
	assert_int(ssao_end).override_failure_message(
		"no captured frame held both scopes: %s" % names).is_greater(-1)
	assert_int(ssao_end).override_failure_message(
		"ssao closes at %d, after deferred opened at %d: %s" % [ssao_end, deferred_begin, names]
		).is_less(deferred_begin)
```

- [ ] **Step 2: Run it to see it fail**

```bash
./gdunit_tests.sh -a res://tests/test_gpu_timing_scopes.gd
```
Expected: FAIL on the last assertion ("ssao closes at … after deferred opened at …"). If it fails on either "no captured frame" assertion instead, stop: marker capture differs from the planning probe; report.

- [ ] **Step 3: Commit the failing test**

```bash
git add tests/test_gpu_timing_scopes.gd
git commit -m "test: the ssao timing scope must close before deferred opens (S8, failing)

"
```

- [ ] **Step 4: Fix**

In `extension/src/render/frame.cpp`, `render_pre_opaque`, delete the line

```cpp
	timings->begin(rd, "deferred");
```
that sits directly after `dp.probe_mode = in.debug.deferred_view;`, and insert, directly after the SSAO block's closing `}` and before `const bool deferred_ok = deferred->render(...`:

```cpp
	// S8: open "deferred" only after SSAO has closed, so the label times lighting alone.
	timings->begin(rd, "deferred");
```

- [ ] **Step 5: Build and verify**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_gpu_timing_scopes.gd,res://tests/test_gpu_timings.gd,res://tests/test_frame_contract.gd,res://tests/test_frame_shipped_golden.gd,res://tests/test_benchmark.gd
rg -n '"deferred"' demo/benchmark.gd
```
Expected: the S8 test passes; the others match the baseline; `rg` prints nothing (no benchmark label table to change — planning decision 2).

- [ ] **Step 6: Commit the fix**

```bash
git add extension/src/render/frame.cpp
git commit -m "fix: time SSAO outside the deferred scope (S8)

\"deferred\" included SSAO's GPU time and the parser's unattributed remainder subtracted SSAO
twice. demo/benchmark.gd samples no \"deferred\" key, so no benchmark label changes; its
unattributed figure becomes correct.

"
```

---

### Task 12: Remove both `friend VoxelDebugHooks`; `LodStats`, `WorldStats`; façade header ≤ 250 lines (milestone 4a)

Friend reads are re-homed **in place**; no hook moves files yet.

**Files:**
- Modify: `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/voxel_world.h` (rewritten), `extension/src/voxel_world.cpp`
- Modify: `extension/src/debug/hooks.cpp`
- Modify: `extension/src/generator/field_generator.h` (one comment line)
- Modify: any `.cpp` that loses a transitive include (compile-driven)

**Interfaces:**
- Consumes: Tasks 6–10.
- Produces:
  - `struct LodStats` and `LodStats LodSystem::stats()` (below).
  - `struct WorldStats { int overflow_seen; int edit_rejections; float last_physics_tick_ms; }`; on `VoxelWorld`: `WorldStats stats() const`, `void note_overflow(int bits)`, `MeshService *mesh_service()`, `ColliderStreamer *colliders()`, `ve::ChunkResidency *chunk_residency()`, `IslandManager *island_manager()`, `bool physics_ready() const`, `std::vector<IslandBody *> &test_bodies()`, `std::vector<float> &physics_bubble_centers()`, public `extract_component(...)`.
  - Deleted: both `friend class VoxelDebugHooks`, `VoxelWorld::set_generator`.

- [ ] **Step 1: `LodStats`**

In `lod_system.h`, above `class LodSystem {`:

```cpp
// What the debug facade reports about the LoD runtime, copied in ONE hold of the lod mutex
// (the hold debug_lod_stats used to take itself, through friendship). Plain data.
struct LodStats {
	int pages_total = 0;
	int pages_free = 0;
	int pages_high_water = 0;
	int chunk_records = 0;
	int chunk_records_used = 0;
	int chunk_records_high_water = 0;
	const char *budget_bound = "none";
	int chunks_resident = 0;
	int dirty_chunks = 0;
	int dirty_levels = 0;
	int draw_pages = 0;
	std::vector<int> draw_page_ids;     // the current cut's page identities, in draw order
	std::vector<int> resident_page_ids; // pages holding at least one quad
	std::vector<ve::LodBuildRequest> requests; // what the last walk still wants built
	int partial_allocations = 0;
};
```

In the class, public, after `LodPool *pool() const`:

```cpp
	// Runs ensure_lod() and copies LodStats under mutex(). Tool/main thread.
	LodStats stats();
```

Delete the `friend class VoxelDebugHooks;` line with its comment block, and the forward declaration `class VoxelDebugHooks;`.

In `lod_system.cpp` add `#include "lod/lod_arena.h"` if `ve::lod_collect_page_draws` / `ve::LodPageDraw` are not already visible (they live in `lod/lod_tree.h`), and:

```cpp
LodStats LodSystem::stats() {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	ensure_lod();
	LodStats s;
	if (lod_pool_) {
		s.pages_total = lod_pool_->page_count();
		s.pages_free = lod_pool_->free_pages();
		s.chunk_records = lod_pool_->chunk_record_count();
		s.chunk_records_used = lod_pool_->chunk_records_used();
		s.chunk_records_high_water = lod_pool_->chunk_records_high_water();
		s.pages_high_water = lod_pool_->pages_high_water();
		s.budget_bound = lod_pool_->budget_bound();
	}
	s.chunks_resident = static_cast<int>(lod_pages_of_.size());
	if (lod_tree_) lod_tree_->dirty_stats(&s.dirty_chunks, &s.dirty_levels);
	for (const ve::LodDrawItem &item : lod_walk_.draws) s.draw_pages += item.page_count;
	// The exact page identities of the current camera cut, not just their count: a bounded
	// pool may keep a drawable coarse cut while refinement requests remain pending.
	std::vector<ve::LodPageDraw> draw_page_list;
	ve::lod_collect_page_draws(lod_walk_.draws, lod_pages_of_, lod_page_quads_, &draw_page_list);
	for (const ve::LodPageDraw &page : draw_page_list) s.draw_page_ids.push_back(page.page);
	for (const auto &page : lod_page_quads_)
		if (page.second > 0) s.resident_page_ids.push_back(page.first);
	s.requests = lod_walk_.requests;
	// LodArena::alloc is all-or-nothing, so this should always be zero -- but a hardcoded 0
	// would make the test that asserts it vacuous. MEASURE the two shapes a partially funded
	// build would take: a chunk holding a page the per-page quad count never learned about,
	// and arena pages that no resident chunk owns.
	int partial = 0;
	size_t owned_pages = 0;
	for (const auto &kv : lod_pages_of_) {
		owned_pages += kv.second.size();
		for (int p : kv.second) {
			if (lod_page_quads_.find(p) == lod_page_quads_.end()) {
				partial++;
				break;
			}
		}
	}
	const int unowned = (s.pages_total - s.pages_free) - static_cast<int>(owned_pages);
	s.partial_allocations = partial + (unowned > 0 ? unowned : 0);
	return s;
}
```

Replace the whole `VoxelDebugHooks::debug_lod_stats()` body in `hooks.cpp` (same keys, same insertion order):

```cpp
Dictionary VoxelDebugHooks::debug_lod_stats() {
	const LodStats s = world_->context().lod->stats();
	Dictionary d;
	d["pages_total"] = s.pages_total;
	d["pages_free"] = s.pages_free;
	d["pages_used"] = s.pages_total - s.pages_free;
	d["chunks_resident"] = s.chunks_resident;
	d["chunk_records"] = s.chunk_records;
	d["chunk_records_used"] = s.chunk_records_used;
	d["chunk_records_high_water"] = s.chunk_records_high_water;
	d["pages_high_water"] = s.pages_high_water;
	d["budget_bound"] = String(s.budget_bound);
	d["dirty_chunks"] = s.dirty_chunks;
	d["dirty_levels"] = s.dirty_levels;
	d["draw_pages"] = s.draw_pages;
	PackedInt32Array draw_page_ids;
	for (int page : s.draw_page_ids) draw_page_ids.append(page);
	d["draw_page_ids"] = draw_page_ids;
	PackedInt32Array resident_page_ids;
	for (int page : s.resident_page_ids) resident_page_ids.append(page);
	d["resident_page_ids"] = resident_page_ids;
	// Zero pending requests with nothing in flight is what "the far field has converged for
	// this camera" means; tests wait on it instead of guessing a frame count.
	d["requests_pending"] = static_cast<int>(s.requests.size());
	Array pending_request_ids;
	for (const ve::LodBuildRequest &request : s.requests) {
		pending_request_ids.append(String::num_int64(request.level) + ":" +
				String::num_int64(request.coord.x) + ":" + String::num_int64(request.coord.y) + ":" +
				String::num_int64(request.coord.z));
	}
	d["pending_request_ids"] = pending_request_ids;
	d["partial_allocations"] = s.partial_allocations;
	d["builds_in_flight"] = world_->mesh_service() && world_->mesh_service()->lod_busy() ? 1 : 0;
	// The benchmark's horizon tracker reads this name; same count as requests_pending.
	d["lod_pending"] = static_cast<int>(s.requests.size());
	// Async cull stats readback; zero until the first readback lands (safe "nothing culled").
	d["culled_ratio"] = world_->context().render->passes().lod_cull
			? world_->context().render->passes().lod_cull->culled_ratio() : 0.0f;
	return d;
}
```

- [ ] **Step 2: Re-home the remaining `VoxelWorld` private reads in hooks**

```bash
perl -pi -e '
  s/\b(world_|w)->store_->/$1->context().store->/g;
  s/\b(world_|w)->mesh_\b/$1->mesh_service()/g;
  s/\b(world_|w)->colliders_\b/$1->colliders()/g;
  s/\b(world_|w)->chunks_\b/$1->chunk_residency()/g;
  s/\b(world_|w)->island_manager_\b/$1->island_manager()/g;
  s/\b(world_|w)->physics_ready_\b/$1->physics_ready()/g;
  s/\b(world_|w)->test_bodies_\b/$1->test_bodies()/g;
  s/\b(world_|w)->overflow_seen_ \|= (.*);/$1->note_overflow($2);/g;
  s/\b(world_|w)->overflow_seen_\b/$1->stats().overflow_seen/g;
  s/\b(world_|w)->edit_rejections_\b/$1->stats().edit_rejections/g;
  s/\b(world_|w)->last_physics_tick_ms_\b/$1->stats().last_physics_tick_ms/g;
  s/\b(world_|w)->physics_bubble_centers_\b/$1->physics_bubble_centers()/g;
  s/\b(world_|w)->context\(\)\.lod->lod_pool_\b/$1->context().lod->pool()/g;
' extension/src/debug/hooks.cpp
rg -n '\b(world_|w)->[a-z_]+_\b' extension/src/debug/hooks.cpp
rg -n 'lod->lod_[a-z_]+_\b|ensure_lod' extension/src/debug/hooks.cpp
```
Expected: both `rg` commands print nothing (`world_` itself is the hooks' member and never appears as `world_->x_`).

- [ ] **Step 3: Rewrite `voxel_world.h`**

Replace the entire file with:

```cpp
#pragma once
// VoxelWorld -- the scene node, and a façade: ClassDB properties and methods, the CPU-side init
// order (graphics, then physics), and ownership of the modules that do the work --
//   WorldStore                authoritative CPU data (config, edit log, overrides, volumes)
//   LodSystem                 far-field runtime
//   RenderOrchestrator        render lifetime: devices, passes, streamer, island handoff, frame
//   ConsolidationCoordinator  override consolidation
// C++ callers reach those modules through context(); this class forwards only what ClassDB
// binds. Spec: docs/superpowers/specs/2026-09-14-render-lifetime-owner-design.md.
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <memory>
#include <utility>
#include <vector>
#include "core/context.h"
#include "core/world_store.h"
#include "debug/hooks.h"
#include "mesh/box_merge.h"
#include "mesh/chunk_residency.h"
#include "world/edit_log.h"
#include "world/region.h"

namespace godot {

class ColliderStreamer;
class ConsolidationCoordinator;
class IslandBody;
class IslandManager;
class LodSystem;
class MeshService;
class RenderOrchestrator;
class VoxelWorld;
struct IslandExtractJob;

bool voxel_compositor_callbacks_enabled();
bool voxel_try_begin_compositor_callback(const NodePath &world_path, VoxelWorld **world);

// Counters the debug facade reports; copied out, never referenced.
struct WorldStats {
	int overflow_seen = 0;   // sticky OR of streamer overflow bits
	int edit_rejections = 0; // ops refused by a full region list
	float last_physics_tick_ms = 0.0f;
};

class VoxelWorld : public Node3D, public EditSink {
	GDCLASS(VoxelWorld, Node3D)

	VoxelDebugHooks *debug_hooks_ = nullptr;
	bool use_local_device_ = false;
	std::unique_ptr<WorldStore> store_; // created first: setters write its config pre-init
	VoxelContext context_;
	std::unique_ptr<ConsolidationCoordinator> consolidation_;
	// Declared before render_: the orchestrator owns the frame, which references this LoD
	// runtime, so the LoD runtime must be destroyed after it.
	std::unique_ptr<LodSystem> lod_;
	std::unique_ptr<RenderOrchestrator> render_;

	bool physics_enabled_ = true;
	NodePath physics_center_path_;
	NodePath sun_light_path_;
	float physics_radius_m_ = 64.0f;
	// Kept well under physics_radius_m_: see ColliderStreamer::set_body_bubble_radius_m.
	float physics_bubble_radius_m_ = 12.0f;
	int max_collider_chunks_ = 1280;
	int mesh_jobs_per_frame_ = 2;
	int shape_builds_per_frame_ = 2;
	// The golden corpora pin their own frozen pipeline through this.
	String terrain_pipeline_path_ = "res://assets/pipelines/default.pipeline";

	// Physics lifetime (ensure_physics_initialized / teardown_physics). The mesher owns its own
	// device on its own thread; nothing on the main thread touches that device.
	MeshService *mesh_ = nullptr;
	ve::ChunkResidency *chunks_ = nullptr;
	ColliderStreamer *colliders_ = nullptr;
	IslandManager *island_manager_ = nullptr; // published/detached under edit_mutex
	bool physics_ready_ = false;
	std::vector<std::pair<ve::IVec3, ve::IVec3>> pending_dirty_; // guarded by edit_mutex
	std::vector<float> physics_bubble_centers_;                  // xyz triples, main thread
	std::vector<IslandBody *> test_bodies_;                      // hand-driven test pool
	WorldStats stats_;

	void update_sun_state();
	void publish_sun_state_to_local_device(RenderingDevice *device);
	// EditSink: WorldStore's spine calls this with edit_mutex held.
	void on_edit_appended(const ve::EditOp &op, bool notify_islands) override;

protected:
	static void _bind_methods();

public:
	VoxelWorld();
	~VoxelWorld() override;
	void _ready() override;
	void _process(double delta) override;
	void _exit_tree() override;

	VoxelDebugHooks *hooks();
	// The material registry for the demo's picker; one dictionary per material, id ascending.
	Array material_table() const;
	VoxelContext &context() { return context_; }

	// --- ClassDB properties ---
	void set_use_local_device(bool v) { use_local_device_ = v; }
	bool get_use_local_device() const { return use_local_device_; }
	void set_atlas_bricks(Vector3i v);
	Vector3i get_atlas_bricks() const;
	void set_max_region_slots(int v);
	int get_max_region_slots() const;
	void set_max_brick_jobs(int v);
	int get_max_brick_jobs() const;
	void set_max_override_bricks(int v);
	int get_max_override_bricks() const;
	void set_stream_radius_m(float v);
	float get_stream_radius_m() const;
	void set_occupancy_retention_m(float v);
	float get_occupancy_retention_m() const;
	void set_residency_radius_m(float v);
	float get_residency_radius_m() const;
	// Fraction of the internal resolution the near-field marcher runs at: the frame budget's
	// coarsest dial. Stored on the orchestrator (read on the render thread).
	void set_near_field_scale(float v);
	float get_near_field_scale() const;
	void set_physics_enabled(bool v) { physics_enabled_ = v; }
	bool get_physics_enabled() const { return physics_enabled_; }
	void set_physics_center_path(const NodePath &p) { physics_center_path_ = p; }
	NodePath get_physics_center_path() const { return physics_center_path_; }
	void set_sun_light_path(const NodePath &p) { sun_light_path_ = p; }
	NodePath get_sun_light_path() const { return sun_light_path_; }
	// The A/B knob for the shadow cut's minimum-level clamp. On by default.
	void set_sun_cascade_min_level(bool v);
	bool get_sun_cascade_min_level() const;
	void set_physics_radius_m(float v) { physics_radius_m_ = v; }
	float get_physics_radius_m() const { return physics_radius_m_; }
	void set_physics_bubble_radius_m(float v); // applies live to the collider streamer
	float get_physics_bubble_radius_m() const { return physics_bubble_radius_m_; }
	void set_max_collider_chunks(int v) { max_collider_chunks_ = v; }
	int get_max_collider_chunks() const { return max_collider_chunks_; }
	void set_mesh_jobs_per_frame(int v) { mesh_jobs_per_frame_ = v; }
	int get_mesh_jobs_per_frame() const { return mesh_jobs_per_frame_; }
	void set_shape_builds_per_frame(int v) { shape_builds_per_frame_ = v; }
	int get_shape_builds_per_frame() const { return shape_builds_per_frame_; }
	void set_max_lod_pages(int v);
	int get_max_lod_pages() const;
	void set_max_lod_chunk_records(int v);
	int get_max_lod_chunk_records() const;
	void set_lod_builds_per_frame(int v);
	int get_lod_builds_per_frame() const;
	void set_terrain_pipeline_path(const String &v) { terrain_pipeline_path_ = v; }
	String get_terrain_pipeline_path() const { return terrain_pipeline_path_; }
	void set_quality_tier(int v);
	int get_quality_tier() const;
	// Fail-soft: an unknown effect name is ignored, not a crash.
	void set_effect_enabled(const String &name, bool on);
	bool get_effect_enabled(const String &name) const;
	void set_effect_value(const String &name, float value);
	float get_effect_value(const String &name) const;
	bool set_grass_value(const String &name, float v);
	float get_grass_value(const String &name) const;

	// --- lifetime ---
	void ensure_initialized();
	bool is_initialized() const;
	void load_terrain_pipeline(); // first successful load wins; see the definition
	void request_shader_reload(); // a latch; the render callback pumps it
	void teardown_gpu();          // every GPU object; CPU cores survive
	void shutdown_render_resources();
	// ClassDB-bound as "_shutdown_render_resources_on_render_thread" (the Callable target).
	void shutdown_render_resources_on_render_thread();
	// Compositor admission guard; _exit_tree waits for admitted callbacks.
	bool try_begin_render_callback();
	void end_render_callback();
	// On a local device this also refreshes and publishes the sun (see the definition).
	RenderingDevice *rd() const;
	int sun_cascade_count() const;

	// --- physics ---
	void ensure_physics_initialized();
	void teardown_physics();
	int physics_tick(Vector3 center); // actions taken
	MeshService *mesh_service() { return mesh_; }
	ColliderStreamer *colliders() { return colliders_; }
	ve::ChunkResidency *chunk_residency() { return chunks_; }
	IslandManager *island_manager() { return island_manager_; }
	bool physics_ready() const { return physics_ready_; }
	std::vector<IslandBody *> &test_bodies() { return test_bodies_; }
	std::vector<float> &physics_bubble_centers() { return physics_bubble_centers_; }
	WorldStats stats() const { return stats_; }
	void note_overflow(int bits) { stats_.overflow_seen |= bits; }
	// Synchronous island extraction for diagnostics (drives the mesher worker by hand).
	bool extract_component(const std::vector<ve::IVec3> &cells, IslandExtractJob *job,
			std::vector<ve::CellBox> *boxes, ve::VolumeData *out);

	// --- edits ---
	// Tool entry point; main thread; takes edit_mutex.
	ve::EditLog::AppendResult append_edit(const ve::EditOp &op);
	// GDScript "append_edit": one op in its 32-byte ve::EditOp encoding -> {touched, rejected}.
	Dictionary append_edit_op(const PackedByteArray &op_bytes);
	// Caller MUST hold edit_mutex. WorldStore's spine, then this node's fan-out remainder
	// (rejection stats, LoD dirty marks, collider remesh queue) under the same hold.
	ve::EditLog::AppendResult append_edit_locked(const ve::EditOp &op, bool notify_islands = true);
	int64_t edit_seq() const { return store_->edit_seq(); }
};

} // namespace godot
```

- [ ] **Step 4: `voxel_world.cpp` — bodies for the now out-of-line accessors**

Add near `_bind_methods` (the includes `render/orchestrator.h` and `lod/lod_system.h` must be present; add `#include "lod/lod_system.h"` if missing):

```cpp
void VoxelWorld::set_atlas_bricks(Vector3i v) { store_->set_atlas_bricks({v.x, v.y, v.z}); }
Vector3i VoxelWorld::get_atlas_bricks() const {
	const auto &b = store_->config().atlas_bricks;
	return {b.x, b.y, b.z};
}
void VoxelWorld::set_max_region_slots(int v) { store_->set_max_region_slots(v); }
int VoxelWorld::get_max_region_slots() const { return store_->config().max_region_slots; }
void VoxelWorld::set_max_brick_jobs(int v) { store_->set_max_brick_jobs(v); }
int VoxelWorld::get_max_brick_jobs() const { return store_->config().max_brick_jobs; }
void VoxelWorld::set_max_override_bricks(int v) { store_->set_max_override_bricks(v); }
int VoxelWorld::get_max_override_bricks() const { return store_->config().max_override_bricks; }
void VoxelWorld::set_stream_radius_m(float v) { store_->set_stream_radius_m(v); }
float VoxelWorld::get_stream_radius_m() const { return store_->config().stream_radius_m; }
void VoxelWorld::set_occupancy_retention_m(float v) { store_->set_occupancy_retention_m(v); }
float VoxelWorld::get_occupancy_retention_m() const { return store_->config().occupancy_retention_m; }
void VoxelWorld::set_residency_radius_m(float v) { store_->set_residency_radius_m(v); }
float VoxelWorld::get_residency_radius_m() const { return store_->config().residency_radius_m; }
void VoxelWorld::set_near_field_scale(float v) { context_.render->set_near_field_scale(v); }
float VoxelWorld::get_near_field_scale() const { return context_.render->near_field_scale(); }
void VoxelWorld::set_sun_cascade_min_level(bool v) { context_.render->set_sun_cascade_min_level(v); }
bool VoxelWorld::get_sun_cascade_min_level() const { return context_.render->sun_cascade_min_level(); }
void VoxelWorld::set_max_lod_pages(int v) { lod_->set_max_lod_pages(v); }
int VoxelWorld::get_max_lod_pages() const { return lod_->max_lod_pages(); }
void VoxelWorld::set_max_lod_chunk_records(int v) { lod_->set_max_lod_chunk_records(v); }
int VoxelWorld::get_max_lod_chunk_records() const { return lod_->max_lod_chunk_records(); }
void VoxelWorld::set_lod_builds_per_frame(int v) { lod_->set_lod_builds_per_frame(v); }
int VoxelWorld::get_lod_builds_per_frame() const { return lod_->lod_builds_per_frame(); }
bool VoxelWorld::is_initialized() const { return context_.render->initialized(); }
```

In the rest of `voxel_world.cpp`: `overflow_seen_ = 0;` → `stats_.overflow_seen = 0;`; `edit_rejections_ +=` → `stats_.edit_rejections +=`; `last_physics_tick_ms_ =` → `stats_.last_physics_tick_ms =`. Delete any remaining definition of a method no longer declared (`set_generator`, `get_normal_roughness_state`, etc.).

In `extension/src/generator/field_generator.h` line 4, change `VoxelWorld::set_generator()` to `WorldStore::set_generator()`.

- [ ] **Step 5: Build, fixing includes where they are used**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | rg -n 'error' | head -40
```
The header no longer pulls `render/orchestrator.h`, `lod/lod_system.h`, `physics/island_manager.h`, `physics/island_body.h`, `render/frame.h` and friends transitively. For each "incomplete type" / "unknown type" error, add the include to the **`.cpp` that uses it** (typically `debug/hooks.cpp`, `voxel_world.cpp`, the compositors, `register_types.cpp`, `mesh/consolidation.cpp`). For a "no member named" error, route through `context()` per Task 10's tables. Never widen `voxel_world.h` except to re-add a declaration of a method that is `ClassDB`-bound. Repeat until clean.

- [ ] **Step 6: Verify exit checks for this task**

```bash
rg -n 'friend class VoxelDebugHooks' extension/src
wc -l extension/src/voxel_world.h
rg -n 'set_generator' extension/src/voxel_world.h extension/src/voxel_world.cpp
```
Expected: no friend; ≤ 250 lines; no `set_generator` on the world.

- [ ] **Step 7: Gate (full run)**

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
./gdunit_tests.sh
```
Expected: matches the baseline in counts and failing cases; lifetime contract, shipped golden, frame contract and S8 test green.

- [ ] **Step 8: Commit**

Append `### Task 12 gate (full run)` to the evidence log.

```bash
git add -u extension/src docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git diff --cached --stat
git diff --cached --check
git commit -m "refactor: the debug facade uses public module APIs; both VoxelDebugHooks friends are gone

LodSystem::stats() takes the lod-mutex hold debug_lod_stats used to take through friendship.
voxel_world.h is a façade header.

"
```

---

### Task 13: Split `hooks.cpp` by module — pure move (milestone 4b)

**Files:**
- Modify: `extension/src/debug/hooks.cpp`
- Create: `extension/src/debug/hooks_render.cpp`, `hooks_lod.cpp`, `hooks_world.cpp`, `hooks_physics.cpp`, `hooks_common.h`

**Interfaces:**
- Consumes: Task 12's `hooks.cpp`.
- Produces: identical `VoxelDebugHooks` class; `hooks.cpp` keeps `_bind_methods` and `debug_render_frame`; `hooks_common.h` declares `inline float half_to_float(uint16_t)` and `inline void write_frame_record(Dictionary &, const FrameRecord &)`.

The splitting script was dry-run against today's `hooks.cpp` during planning: it reassembles the original exactly (172 units) and all five output files passed `clang++ -fsyntax-only`.

- [ ] **Step 1: Count definitions before the split**

```bash
rg -c '^[A-Za-z].*VoxelDebugHooks::[a-zA-Z_0-9]+\(' extension/src/debug/hooks.cpp
rg -c 'ClassDB::bind_method' extension/src/debug/hooks.cpp
```
Record both numbers in the evidence log.

- [ ] **Step 2: Save the split script (one-off, not committed)**

Save as `$TMPDIR/split_hooks.py`:

```python
#!/usr/bin/env python3
"""Pure-move split of extension/src/debug/hooks.cpp into per-module files.

Usage: split_hooks.py [--check]   (--check parses and reports, writes nothing)
A unit is everything after the previous unit up to a column-0 "}" closing a function; #if/#endif
groups stay whole. Each unit goes to the file of the first function it defines.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path("extension/src/debug")
SRC = ROOT / "hooks.cpp"

LOD = {
    "debug_lod_tick", "debug_lod_stats", "debug_lod_fade_band", "debug_lod_render_probe",
    "debug_lod_render_probe_culled", "debug_lod_gbuffer_probe", "debug_seam_probe",
    "debug_lod_cull_probe", "debug_lod_cull_debug", "debug_lod_diff", "debug_lod_submit",
    "debug_lod_collect",
}
PHYSICS = {
    "debug_physics_frame", "debug_set_physics_bubbles", "debug_physics_stats",
    "debug_island_pending_uploads", "debug_field_volume_upload_count",
    "debug_island_descriptors_pending", "debug_mesh_volume_slots",
    "debug_queue_test_island_upload", "debug_queue_test_island_descriptors",
    "debug_queue_committed_field_volume_upload", "debug_set_extraction_available",
    "debug_set_fail_extractions", "debug_set_fail_extract_submit", "debug_island_frame",
    "debug_island_stats", "debug_set_merge_sleep_seconds", "debug_set_max_dynamic_bodies",
    "debug_set_atlas_slot_used", "debug_set_fail_next_spawn", "debug_set_fail_next_restore",
    "debug_set_fail_next_carve", "debug_set_fail_next_resample",
    "debug_set_empty_next_extraction", "debug_wake_island_body", "debug_offset_island_body",
    "debug_island_body_info", "debug_body_of_chunk", "debug_chunk_collider_info",
    "debug_chunk_collider_octants", "debug_init_physics", "debug_teardown_physics",
    "debug_mesh_lattice_diff", "debug_mesh_diff", "debug_island_extract_diff",
    "debug_place_test_island_rotated", "debug_place_test_island", "debug_spawn_test_body",
    "debug_test_body_stats", "debug_tick_test_bodies", "debug_despawn_test_body",
    "debug_clear_test_island", "debug_mesh_submit", "debug_mesh_collect",
    "debug_extract_submit", "debug_extract_collect",
}
RENDER = {
    "debug_gpu_timings", "debug_ingest_gpu_timings", "debug_beauty_compositor_stats",
    "debug_contact_shadow_probe", "debug_ssgi_probe", "debug_ssao_probe",
    "debug_ssgi_history_latch_probe", "debug_ssgi_reprojection_probe", "debug_beauty_settings",
    "debug_stored_normal_stats", "debug_normal_pool_state", "debug_normal_upload_override",
    "debug_normal_release_override", "debug_grass_stats", "debug_gbuffer_stats",
    "debug_hiz_stats", "debug_hiz_shutdown_probe", "debug_hiz_probe_synthetic",
    "debug_hiz_occluded", "debug_island_tile_mask", "debug_raymarch_pixel",
    "debug_raymarch_probe", "debug_raymarch_cost_probe", "debug_raymarch_gbuffer",
    "debug_raymarch_hole_probe", "debug_raymarch_normal_probe", "debug_island_normal_probe",
    "debug_ssr_probe", "debug_outline_probe", "debug_glossy_sdf_probe", "debug_cel_reference",
    "debug_cel_diff", "debug_sun_shadow_stats", "debug_sun_shadow_build", "sun_shadow_probe",
    "debug_sun_shadow_visibility", "debug_sun_shadow_shading", "debug_deferred_probe",
    "debug_near_field_detail", "debug_load_shader", "debug_shader_reload_stats",
    "debug_set_shader_override", "debug_request_shader_reload",
    "debug_clear_shader_source_overrides", "debug_material_atlas_stats",
    "debug_material_alpha_stats", "debug_poke_material_normal",
    "debug_flatten_material_normal", "probe_material", "debug_material_normal_probe",
    "resolve_near_field", "debug_material_probe", "debug_pump_shader_reload",
    "debug_set_normal_pool_budget", "debug_local_rd", "render_probe_pixel",
    "debug_teardown_trace",
}
CORE = {"_bind_methods", "debug_render_frame", "half_to_float", "write_frame_record"}
HELPERS = {"half_to_float", "write_frame_record"}

DEF = re.compile(r"^[A-Za-z][^(]*?\b(?:VoxelDebugHooks::)?([A-Za-z_][A-Za-z_0-9]*)\(")


def target(name):
    if name in CORE:
        return "hooks.cpp"
    if name in LOD:
        return "hooks_lod.cpp"
    if name in PHYSICS:
        return "hooks_physics.cpp"
    if name in RENDER:
        return "hooks_render.cpp"
    return "hooks_world.cpp"


def split(lines):
    ns = lines.index("namespace godot {")
    end = max(i for i, l in enumerate(lines) if l == "} // namespace godot")
    units, cur, depth, closed_in_group, name = [], [], 0, False, None
    for line in lines[ns + 1:end]:
        cur.append(line)
        if name is None and not line.startswith((" ", "\t", "/", "#")):
            m = DEF.match(line)
            if m and ("VoxelDebugHooks::" in line or line.startswith("static ")):
                name = m.group(1)
        if line.startswith("#if"):
            depth += 1
        elif line.startswith("#endif"):
            depth -= 1
            if depth == 0 and closed_in_group:
                units.append((name, cur))
                cur, name, closed_in_group = [], None, False
                continue
        if line == "}":
            if depth == 0:
                units.append((name, cur))
                cur, name = [], None
            else:
                closed_in_group = True
    return lines[:ns + 1], units, cur, lines[end:]


def main():
    check = "--check" in sys.argv
    lines = SRC.read_text().split("\n")
    pre, units, leftover, tail = split(lines)
    rebuilt = pre + [l for _, u in units for l in u] + leftover + tail
    assert rebuilt == lines, "unit parse does not reassemble the original file"
    unnamed = [u[:3] for n, u in units if not n]
    assert not unnamed, "a unit defines no function: %s" % unnamed
    buckets = {}
    for n, u in units:
        if n not in HELPERS:
            buckets.setdefault(target(n), []).extend(u)
    for f, body in sorted(buckets.items()):
        print("%-18s %5d lines" % (f, len(body)))
    print("units:", len(units), "leftover lines:", len(leftover))
    if check:
        return
    head = pre[:-1] + ['#include "debug/hooks_common.h"', "", pre[-1]]
    for f, body in buckets.items():
        extra = leftover if f == "hooks.cpp" else []
        (ROOT / f).write_text("\n".join(head + body + extra + tail))
    shared = [l for n, u in units if n in HELPERS for l in u]
    common = ["#pragma once", "// File-local helpers shared by the hooks*.cpp translation units.",
              '#include "render/frame.h"', "#include <godot_cpp/variant/dictionary.hpp>",
              "#include <godot_cpp/variant/packed_string_array.hpp>", "#include <cstdint>", "",
              "namespace godot {"]
    common += [l.replace("static ", "inline ", 1) if l.startswith("static ") else l for l in shared]
    common += ["", "} // namespace godot", ""]
    (ROOT / "hooks_common.h").write_text("\n".join(common))


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Dry run**

```bash
python3 "$TMPDIR/split_hooks.py" --check
```
Expected: five files listed, `leftover lines: 1`, no assertion error. If a unit is unnamed or reassembly fails, the file gained a top-level construct the parser does not know: stop and report.

- [ ] **Step 4: Split**

```bash
python3 "$TMPDIR/split_hooks.py"
ls extension/src/debug
```

- [ ] **Step 5: Prove it is a pure move**

```bash
cat extension/src/debug/hooks*.cpp extension/src/debug/hooks_common.h | rg -v '^#include|^#pragma|^namespace godot \{$|^\} // namespace godot$|^// File-local helpers' | sed 's/^inline /static /' | sort > "$TMPDIR/after.txt"
git show HEAD:extension/src/debug/hooks.cpp | rg -v '^#include|^namespace godot \{$|^\} // namespace godot$' | sort > "$TMPDIR/before.txt"
diff <(uniq -c "$TMPDIR/before.txt" | rg -v '^\s+\d+ $') <(uniq -c "$TMPDIR/after.txt" | rg -v '^\s+\d+ $') && echo PURE
rg -c '^[A-Za-z].*VoxelDebugHooks::[a-zA-Z_0-9]+\(' extension/src/debug/hooks*.cpp | awk -F: '{s+=$2} END {print s}'
rg -c 'ClassDB::bind_method' extension/src/debug/hooks*.cpp | awk -F: '{s+=$2} END {print s}'
```
Expected: `PURE` (non-blank lines identical as a multiset; blank-line counts may differ at file seams), and both totals equal Step 1's numbers.

- [ ] **Step 6: Build and gate (full run)**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh
```
Expected: build OK (SConstruct globs `src/*/*.cpp`); every suite's case count and failing cases identical to Task 12's run.

- [ ] **Step 7: Commit**

Append `### Task 13 gate (full run)` and the Step 1/5 counts to the evidence log.

```bash
git add extension/src/debug/ docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git diff --cached --check
git diff --cached --stat
git commit -m "refactor: split debug hooks by module (pure move)

hooks.cpp keeps the bindings and debug_render_frame; hooks_render/lod/world/physics.cpp hold
the rest verbatim; hooks_common.h holds the two shared file-local helpers.

"
```

---

### Task 14: Exit criteria, results report, spec amendments

**Files:**
- Create: `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md`
- Modify: `docs/superpowers/specs/2026-09-14-render-lifetime-owner-design.md` (status + planning decisions)
- Modify: `docs/superpowers/specs/2026-09-13-frame-module-design.md` (status, §4.6, §9.1, §9.3)
- Modify: `docs/superpowers/plans/2026-09-13-frame-module.md` (Tasks 10–12 checkboxes, acceptance checklist, SP4 entry gate wording)

**Interfaces:**
- Consumes: every earlier task's evidence log entries.
- Produces: the measured acceptance report.

- [ ] **Step 1: Final build and suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc); echo "build=$?"
(cd extension && scons -Q test) 2>&1 | tail -3
./gdunit_tests.sh; echo "gdunit=$?"
```
Extract counts and failures with Task 3 Step 3's script. Compare with the baseline.

- [ ] **Step 2: Exit-criteria checks**

```bash
rg 'class FrameHost|friend class VoxelDebugHooks|\*\*lod_pool|island_mutex = ' extension/src
rg 'island_mutex_|note_lod_cull_debug|set_beauty_compositor|VoxelWorld::render_probe_pixel|world_->render_probe_pixel|analytic_raycast_down|initialize\(VoxelWorld' extension/src
sed -n '/struct Collaborators {/,/};/p' extension/src/render/orchestrator.h
wc -l extension/src/voxel_world.h
```
Expected: both `rg` commands print nothing; `Collaborators` shows 5 fields; header ≤ 250.

- [ ] **Step 3: GDScript surface unchanged**

```bash
base=$(git log --format=%h --grep='render lifetime owner baseline' -1)
for f in voxel_world.cpp; do
  diff <(git show $base:extension/src/$f | rg -o 'D_METHOD\([^)]*\)' | sort) <(rg -o 'D_METHOD\([^)]*\)' extension/src/$f | sort)
done
diff <(git show $base:extension/src/debug/hooks.cpp | rg -o 'D_METHOD\([^)]*\)' | sort) <(cat extension/src/debug/hooks*.cpp | rg -o 'D_METHOD\([^)]*\)' | sort)
diff <(git show $base:extension/src/debug/hooks.cpp | rg -o 'd\["[a-z_0-9]+"\]' | sort -u) <(cat extension/src/debug/hooks*.cpp | rg -o 'd\["[a-z_0-9]+"\]' | sort -u)
```
Expected: the `VoxelWorld` diff is empty; the hooks diff adds only `D_METHOD("debug_teardown_trace")`; the key diff is empty (the seam probe's `error` key landed in Task 1, before the baseline).

- [ ] **Step 4: Re-trace "new render pass"**

Walk what adding a hypothetical `FogPass` (compute, reads the G-buffer, writes scene colour post-opaque) touches now, by reading the code rather than guessing: `render/fog_pass.h`, `render/fog_pass.cpp`, `shaders/fog.comp.glsl`, `render/orchestrator.h` (`RenderPasses::fog`), `render/orchestrator.cpp` (build in `ensure_gpu_graph`, delete in `teardown_render_passes`), `render/frame.h` (`kStageFog`), `render/frame.cpp` (stage + label), `render/gpu_timings.cpp` (`kPasses` entry — confirm whether an unknown label is dropped by `known_pass`), one gdUnit suite through `debug_render_frame`. Record the actual list and count. If the count exceeds 8, record by how much and why (the `kPasses` table is a candidate for generation from `FrameStage` in sub-project 4); do not change code in this task.

- [ ] **Step 5: Line counts**

```bash
base=$(git log --format=%h --grep='render lifetime owner baseline' -1)
for f in extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp extension/src/debug/hooks.cpp extension/src/physics/island_manager.cpp extension/src/render/frame.cpp; do
  printf '%s %s -> %s\n' "$f" "$(git show $base:$f | wc -l)" "$(wc -l < $f)"
done
wc -l extension/src/render/island_handoff.* extension/src/debug/hooks_*.cpp extension/src/debug/hooks_common.h
```

- [ ] **Step 6: Write the results report**

Create `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md` with these sections, filled from Steps 1–5 and the evidence log:

1. **Revision and environment** — baseline commit, final commit, date, OS/GPU, build and test exit codes, report paths.
2. **Sub-project 1 acceptance** — SP1 plan's acceptance checklist, each line pass/fail with evidence (Tasks 1–2 commits, shipped golden, frame contract, orphan `rg`).
3. **Verification** — native summary; baseline vs final per-suite counts; each remaining failure by case and message; lifetime contract, shipped golden, frame contract and S8 results.
4. **Bite proofs** — Task 4 Step 7's three breaks and what failed.
5. **Golden attribution** — one row per moved number/assertion with cause and commit, or "no golden moved".
6. **Locking changes** — the four removed `island_mutex_` holds and the relocated `lod_mutex` hold, each with its commit.
7. **Deletion and size** — Step 5 table; removed symbols; the five `Collaborators` fields.
8. **Change-cost retrace** — Step 4's list and count against the ≤ 8 target.
9. **Exit criteria** — each spec §6 criterion, pass/fail with evidence. Unmet criteria are listed as open findings, not claimed.

- [ ] **Step 7: Amend the specs and the SP1 plan**

- SP2 spec (`2026-09-14-render-lifetime-owner-design.md`): status → `Implemented; see docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md`; add a section "Decisions made during planning" copying this plan's twelve numbered decisions.
- SP1 spec (`2026-09-13-frame-module-design.md`): status → `Implemented sub-project 1 (Tasks 10–12 completed within sub-project 2); roadmap continues`; §4.6 — append "Deleted by sub-project 2 (commit <Task 9 hash>)."; §9.1 — append "Implemented; results in docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md."; §9.3 — the entry gate's "passes reachable only through the frame" becomes "passes reachable only through `RenderPasses` (pass-level probes stay isolated by sub-project 1's classification)".
- SP1 plan (`2026-09-13-frame-module.md`): check Tasks 10 and 11's steps and the acceptance checklist lines that the results report marks passing; in "Sub-project 4 — Pass anatomy", change "passes reachable only through the frame" to "passes reachable only through `RenderPasses`".

Only mark a box when the report shows its evidence.

- [ ] **Step 8: Commit**

```bash
git add docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md docs/superpowers/specs/2026-09-14-render-lifetime-owner-design.md docs/superpowers/specs/2026-09-13-frame-module-design.md docs/superpowers/plans/2026-09-13-frame-module.md docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md
git diff --cached --check
git commit -m "docs: render lifetime owner results and roadmap amendments

"
```

## Acceptance checklist

- [ ] SP1 Tasks 10–11 landed; SP1 acceptance recorded in the results report.
- [ ] Lifetime contract written before any move, each case proven to bite, green at every gate.
- [ ] `IslandHandoff` native tests green; `island_mutex_` gone; no lock at a new site.
- [ ] `IslandManager` has no `VoxelWorld*`.
- [ ] `Collaborators` ≤ 5 fields; `FrameHost` deleted; `VoxelFrame` owned by the orchestrator.
- [ ] One `RenderPasses` table; no per-pass accessor remains.
- [ ] S8: failing test committed before the `fix:` commit; test green after.
- [ ] Both `friend class VoxelDebugHooks` gone; `voxel_world.h` ≤ 250 lines.
- [ ] `hooks.cpp` split verified as a pure move with identical definition and binding counts.
- [ ] GDScript surface unchanged except `debug_teardown_trace`; full gdUnit matches baseline; shipped golden unchanged.
- [ ] Change-cost retrace recorded; results, SP1 spec, SP2 spec and SP1 plan agree.

