# Edit Pipeline (Sub-project 5b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One module applies an edit and tells every consumer about it: `ve::EditPipeline::apply(ops, policy)` (atomic batches included) plus `invalidate()` for consolidation, fanned out to registered sinks that only queue under the edit lock, so nothing nests inside it and the lock order is stated in exactly one header.

**Architecture:** `core/edit_pipeline.{h,cpp}` is pure (native test globs). `WorldStore` owns one `EditPipeline` and keeps the edit mutex. Every consumer implements `ve::InvalidationSink`: `record()` runs with the edit lock held, takes no lock, and only appends to its own edit-lock-guarded queue; its owner drains that queue on its own thread. Colliders, pending edits and the consolidation queue are already such queues; LoD and the island windows become ones (Tasks 11 and 12), which removes the edit → LoD and edit → `windows_mutex_` edges. `IslandManager`'s three hand-written op-headroom preflights become `preflight()` + atomic `apply()`, which makes its post-carve restore branch unreachable and deletable. `test_connectivity.gd`'s `max_override_bricks = 1` workaround is replaced by an explicit consolidation hold.

**Tech Stack:** C++20, godot-cpp (Godot 4.7), doctest (native tests), gdUnit4 (GPU/scene tests), SCons, GDScript.

**Spec:** `docs/superpowers/specs/2026-09-17-edit-pipeline-design.md`. Roadmap: `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.4 and the pathway in `docs/superpowers/plans/2026-09-13-frame-module.md`. Inherited from 5a: `docs/superpowers/specs/2026-09-16-world-field-query-design.md` §8.

## Global Constraints

- Branch: `feat/edit-pipeline` (already checked out; spec committed as `11b0fb7`).
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)`. A full C++ rebuild can take ~20 min; a one-file change relinks in minutes.
- Native tests: `cd extension && scons -Q test; cd ..`. One case: `extension/build/tests/ve_tests -tc="<name>"`.
- GPU tests: `./gdunit_tests.sh -a res://tests/<suite>.gd` (comma list allowed). Full run: `./gdunit_tests.sh`. Reports: `reports/report_N/results.xml`.
- **Baseline failures:** compare against `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md` (Task 1) by case name *and* message. A suite's case **count** dropping is itself a failure, except where a task deletes a named case. The set drifts: stash and re-run on the parent commit before blaming a change.
- **Golden policy:** a `refactor:` commit must leave every golden and every pin in `tests/test_edit_fanout.gd` unchanged. Task 10 is the ONE task allowed to move a pin, and its commit message names the cause. If a pin moves anywhere else, stop the task and report.
- **The lock rule:** `record()` takes no lock and only appends to its own edit-lock-guarded queue. Tasks 6–10 keep LoD and islands synchronous (today's edit → LoD and edit → windows nesting) as a temporary exception; Tasks 11 and 12 end it. No OTHER lock may be taken inside the edit lock at any point, and no sink may be registered from inside another lock.
- **Bug fixes land in their own commits**, never inside a move.
- C++ and GDScript indentation is tabs. Match surrounding comment density.
- Commit messages are plain conventional messages with no attribution trailers.
- GPU timing *values* are invalid on this machine (`debug_gpu_timings()` returns -1); never pin them.

## Decided during planning (amends the spec)

Task 14 writes these into the spec.

1. **`EditPipeline::preflight(ops, full)` is public.** The re-merge paste and the landing carve must know a batch fits *before* they store, pin, upload or spawn; `apply({atomic})` then cannot fail under the same lock hold. `BatchResult` carries `bool refused` plus `std::vector<IVec3> full` (the regions at the cap), because a batch refused for a malformed or oversized op names no region.
2. **The streamer's regeneration stays behind `ConsolidationCoordinator`'s existing `streamer()` slot.** The streamer is created and destroyed inside render init/teardown, where taking the edit lock to register a sink is a new acquisition site this plan cannot prove safe. The coordinator's `record()` handles `kConsolidated` by queueing the regeneration, exactly as its commit does today.
3. **LoD needs no teardown drop rule.** `LodTree::mark_dirty` only touches nodes that exist (`lod_tree.cpp:477`), and `release_gpu` clears the tree, so queued marks drained afterwards mark nothing. `release_gpu` keeps taking no lock.
4. **`test_lod_stream.gd:60` reads after `hooks().debug_drain_invalidations()`, not after a tick.** A tick would clear the dirty flags it is trying to read (`note_building` clears dirty at submission, `lod_system.cpp:270`).
5. **The fan-out pin is split by observability.** `debug_edit_fanout()` returns collider chunk sets, island windows, the consolidation queue, pending-edit count and regions, the streamer's forced regenerations and `edit_rejections`. LoD marks need a settled LoD world, so they are a separate case reading `debug_lod_stats()`, and they are pinned as the spec's dirty **counts** per read rather than as chunk keys: `LodStats` reports `dirty_chunks` and `dirty_levels`, and `LodTree` exposes no key list. An op with `notify_islands = false` is not reachable from GDScript; it is covered by the native test and by `test_connectivity.gd`'s existing crumble case.
6. **Collider rows are pinned as expanded chunk SETS**, and every op in the pin is far enough from the others that no two queue entries overlap, so `merge_or_cap` (Task 7) cannot move a row. Where a pin does use overlapping ops (the consolidation cases), they share their y/z extents, so a merged bounding box equals the union.
7. **Characterization is three commits** (Tasks 2 and 3), not the spec's single milestone 2.
8. **Staleness uses the log's append seq:** `EditLog::last_seq()`, an `after_seq` parameter on `collect_ops_for_aabb`, `FieldSnapshot::log_seq`, and `FieldView::ops_since()`. `InFlight::ops` is replaced by `InFlight::log_seq`.
9. **Deleting the restore branch also deletes two gdUnit cases**: `test_post_spawn_carve_rejection_keeps_body_in_hole` (it forces the deleted toggles) and `test_near_cap_carve_is_refused_before_any_carve` (its 255-op premise reserved room for the restore volume-add, which no longer exists). `test_rejected_carve_keeps_component_attached` and the native all-or-nothing tests keep the cap refusal covered.
10. **`windows_mutex_` is deleted in Task 12** once the inbox exists: every remaining `windows_` reader (`run_frame`, `run_connectivity`, the retry/failure bookkeeping, `stats()`, `teardown()`) is main-thread.
11. **The island inbox is unbounded** and carries a `ponytail:` note. It only grows while a physics-initialized world never runs `IslandManager::run_frame`, which the shipped game does every frame.
12. **`Invalidation::consolidated(region)`** fills `lo`/`hi` from `brick_world_aabb` so no sink derives a brick range from a float box.
13. **`VoxelWorld` is the collider + rejection-stats sink; `WorldStore` is the pending-edits sink.** Lifetime-owned sinks register in the `VoxelWorld` constructor (and `WorldStore`'s own constructor); `IslandManager` registers in `ensure_physics_initialized` and unregisters in `teardown_physics`, both of which already hold the edit lock at those points.
14. **`pending_dirty_` becomes `std::vector<ve::Box3<int>>`** (Task 7) so one `merge_or_cap` template serves both it and the LoD marks.

## File Map

| File | Status | Responsibility |
|---|---|---|
| `extension/src/core/edit_pipeline.{h,cpp}` | Create | `EditPipeline`, `Invalidation`, `InvalidationSink`, `EditPolicy`, `BatchResult`, `merge_or_cap`, THE lock order |
| `extension/src/core/world_store.{h,cpp}` | Modify | Owns the pipeline; pending-edits sink; `append_edit{,_locked}`, `EditSink`, `ConsolidationSink`, `bump_edit_seq` deleted |
| `extension/src/voxel_world.{h,cpp}` | Modify | Collider + stats sink; registration; `append_edit_locked`/`on_edit_appended` deleted |
| `extension/src/lod/lod_system.{h,cpp}` | Modify | LoD sink; `pending_marks_` + `drain_invalidations()`; `note_edit` private then gone |
| `extension/src/physics/island_manager.{h,cpp}` | Modify | Island sink + inbox; atomic carve/crumble/paste; seq staleness; restore branch, toggles and `windows_mutex_` deleted |
| `extension/src/mesh/consolidation.{h,cpp}` | Modify | Commits call `invalidate()`; consolidation-queue sink; hold seam; `lod_tree`/`lod_mutex`/`pending_dirty` handles deleted |
| `extension/src/render/world_streamer.h` | Modify | `forced_regen()` accessor |
| `extension/src/world/edit_log.h` | Modify | `last_seq()`; `collect_ops_for_aabb(..., after_seq)` |
| `extension/src/world/world_field.{h,cpp}` | Modify | `FieldSnapshot::log_seq`; `FieldView::ops_since` |
| `extension/src/debug/hooks.{h,cpp}`, `hooks_world.cpp`, `hooks_physics.cpp` | Modify | `debug_edit_fanout`, `debug_drain_invalidations`, `debug_hold_consolidation`; fail-toggle hooks deleted |
| `extension/tests/test_edit_pipeline.cpp` | Create | Native pipeline + `merge_or_cap` contract |
| `extension/tests/test_edit_log.cpp` | Modify | `after_seq` filtering |
| `tests/test_edit_fanout.gd` | Create | Fan-out pins |
| `tests/test_consolidation.gd` | Modify | Workaround proof; hold seam |
| `tests/test_connectivity.gd` | Modify | Staleness pin; hold seam replaces `max_override_bricks = 1`; deleted cases |
| `tests/test_lod_stream.gd` | Modify | Drains before reading dirty counts |
| `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md` | Create | Baseline failure set |
| `docs/superpowers/plans/2026-09-17-edit-pipeline-results.md` | Create | Exit evidence |
| `docs/superpowers/specs/2026-09-17-edit-pipeline-design.md`, `plans/2026-09-13-frame-module.md` | Modify (Task 14) | Amendments and status |

---

### Task 1: Record the baseline failure set

No production change. Everything later compares against this file.

**Files:**
- Create: `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md`

**Interfaces:**
- Consumes: nothing.
- Produces: the baseline file.

- [ ] **Step 1: Build and run the native suite**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
```

Expected: build OK; record the doctest `test cases: N | N passed | M failed` line.

- [ ] **Step 2: Run the full gdUnit suite**

```bash
./gdunit_tests.sh
```

Expected: completes (a non-zero exit is normal).

- [ ] **Step 3: Extract per-suite counts and failing cases**

```bash
latest=$(ls -d reports/report_* | sort -V | tail -1)
echo "$latest"
python3 - "$latest/results.xml" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for suite in root.iter('testsuite'):
    print(f"# {suite.get('name')}: tests={suite.get('tests')} failures={suite.get('failures')} errors={suite.get('errors')}")
for tc in root.iter('testcase'):
    bad = tc.find('failure') if tc.find('failure') is not None else tc.find('error')
    if bad is not None:
        msg = (bad.get('message') or bad.text or '').strip().splitlines()
        print(f"{tc.get('classname')}::{tc.get('name')} — {msg[0] if msg else ''}")
EOF
```

- [ ] **Step 4: Write the baseline file**

Create `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md`, pasting the real output into each section:

```markdown
# Edit pipeline — baseline

Commit: `<git rev-parse --short HEAD>`. Recorded <date> on <machine / GPU / Godot version>.
Report: `reports/report_<N>`.

## Native
<doctest summary line from Step 1>

## gdUnit per-suite counts
<every "# suite: tests=… failures=… errors=…" line from Step 3>

## gdUnit failing cases
<every "suite::case — message" line from Step 3, or "none">

Known flaky-by-case suites (project memory): test_connectivity, test_island_body — compare the
suite's failure COUNT, not the case name. Known environment error: test_voxel_settings::
test_an_ambient_change_reaches_the_object_global (Godot/Metal returns Nil outside the editor).
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md
git commit -m "docs: edit pipeline baseline failure set"
```

---

### Task 2: Pin the edit fan-out

Pins which consumer hears which edit and what it will act on, and proves the pins bite. The two new hooks are the seam every later task reads through: `debug_drain_invalidations()` is empty today and gains a line per deferred sink in Tasks 11 and 12.

**Files:**
- Modify: `extension/src/voxel_world.h` (a `pending_dirty()` accessor)
- Modify: `extension/src/mesh/consolidation.h` (a `queued()` accessor)
- Modify: `extension/src/render/world_streamer.h` (a `forced_regen()` accessor)
- Modify: `extension/src/physics/island_manager.h`, `island_manager.cpp` (`debug_windows()`)
- Modify: `extension/src/debug/hooks.h`, `hooks.cpp`, `hooks_world.cpp`
- Create: `tests/test_edit_fanout.gd`

**Interfaces:**
- Consumes: existing hooks `debug_init_physics`, `debug_stream_region`, `debug_apply_sphere_subtract/add/paint`, `debug_store_volume`, `debug_apply_volume_add`, `debug_consolidate_region`, `debug_pump_consolidation_async`, `debug_stream_stats`, `debug_lod_stats`, `debug_lod_tick`, `debug_raycast`.
- Produces: `VoxelDebugHooks::debug_edit_fanout() -> Dictionary`, `VoxelDebugHooks::debug_drain_invalidations() -> void`, `IslandManager::debug_windows() -> Array`, `VoxelWorld::pending_dirty()`, `ConsolidationCoordinator::queued()`, `WorldStreamer::forced_regen()`, and four pinned cases every later task runs.

- [ ] **Step 1: Add the accessors**

In `extension/src/voxel_world.h`, in the public physics block next to `std::vector<float> &physics_bubble_centers()`:

```cpp
	// The collider remesh queue, for debug_edit_fanout. Guarded by edit_mutex: the caller
	// must hold it.
	const std::vector<std::pair<ve::IVec3, ve::IVec3>> &pending_dirty() const { return pending_dirty_; }
```

In `extension/src/mesh/consolidation.h`, next to the stats accessors:

```cpp
	// The queue, for debug_edit_fanout. Written and read under edit_mutex(); the caller
	// must hold it.
	const std::vector<ve::IVec3> &queued() const { return consolidation_queue_; }
```

In `extension/src/render/world_streamer.h`, next to `queue_region_regeneration_locked`:

```cpp
	// Regions a consolidation forced, for debug_edit_fanout. Caller holds edit_mutex_.
	const std::vector<ve::IVec3> &forced_regen() const { return forced_regen_; }
```

In `extension/src/physics/island_manager.h`, inside the existing `#ifdef DEBUG_ENABLED` hook block (next to `debug_body_info`):

```cpp
	// Diagnostic: the pending connectivity windows, for debug_edit_fanout. One row per
	// window: [[lo x, y, z], [hi x, y, z], seq, impulse_scale].
	Array debug_windows();
```

and in the `#else` block next to the other stubs:

```cpp
	Array debug_windows() { return Array(); }
```

Add `#include <godot_cpp/variant/array.hpp>` to `island_manager.h` next to the other godot-cpp includes.

In `extension/src/physics/island_manager.cpp`, next to `debug_body_info` (inside its `#ifdef DEBUG_ENABLED`):

```cpp
Array IslandManager::debug_windows() {
	std::lock_guard<std::mutex> lock(windows_mutex_);
	Array out;
	for (const PendingWindow &w : windows_) {
		Array row, lo, hi;
		lo.push_back(w.lo.x); lo.push_back(w.lo.y); lo.push_back(w.lo.z);
		hi.push_back(w.hi.x); hi.push_back(w.hi.y); hi.push_back(w.hi.z);
		row.push_back(lo);
		row.push_back(hi);
		row.push_back(static_cast<int64_t>(w.seq));
		row.push_back(w.impulse_scale);
		out.push_back(row);
	}
	return out;
}
```

- [ ] **Step 2: Add the two hooks**

In `extension/src/debug/hooks.h`, next to `int debug_region_op_count(Vector3i region);`:

```cpp
	// Sub-project 5b: what every edit consumer will act on, after each of them has drained
	// its own queue through debug_drain_invalidations().
	Dictionary debug_edit_fanout();

	// Runs each consumer's own drain -- the function its tick calls, never a copy -- so a
	// read taken after this sees what the consumer will act on. Empty until a sink becomes
	// deferred.
	void debug_drain_invalidations();
```

In `extension/src/debug/hooks.cpp`, next to the `debug_region_op_count` binding:

```cpp
	ClassDB::bind_method(D_METHOD("debug_edit_fanout"), &VoxelDebugHooks::debug_edit_fanout);
	ClassDB::bind_method(D_METHOD("debug_drain_invalidations"), &VoxelDebugHooks::debug_drain_invalidations);
```

In `extension/src/debug/hooks_world.cpp`, add `#include "physics/island_manager.h"`, `#include <set>` and `#include <tuple>` if they are not already there, and write the bodies next to `debug_region_op_count`:

```cpp
void VoxelDebugHooks::debug_drain_invalidations() {
	// Each deferred consumer's own drain goes here, so debug_edit_fanout reads what the
	// consumer will act on rather than what a hook-local copy would compute. Nothing is
	// deferred yet.
}

Dictionary VoxelDebugHooks::debug_edit_fanout() {
	Dictionary d;
	WorldStore *store = world_->context().store;
	if (!store || !store->edit_log()) return d;
	debug_drain_invalidations();
	using Key = std::tuple<int, int, int>;
	const auto to_array = [](const std::set<Key> &s) {
		Array a;
		for (const Key &k : s) {
			Array v;
			v.push_back(std::get<0>(k));
			v.push_back(std::get<1>(k));
			v.push_back(std::get<2>(k));
			a.push_back(v);
		}
		return a;
	};
	// One hold: every queue below is guarded by the edit mutex.
	std::lock_guard<std::mutex> lock(store->edit_mutex());
	std::set<Key> chunks;
	for (const auto &range : world_->pending_dirty())
		for (int z = range.first.z; z <= range.second.z; z++)
			for (int y = range.first.y; y <= range.second.y; y++)
				for (int x = range.first.x; x <= range.second.x; x++)
					chunks.insert(Key{x, y, z});
	d["colliders"] = to_array(chunks);
	Array queued;
	for (const ve::IVec3 &r : world_->context().consolidation->queued()) {
		Array v;
		v.push_back(r.x); v.push_back(r.y); v.push_back(r.z);
		queued.push_back(v);
	}
	d["consolidation_queue"] = queued;
	std::set<Key> regions;
	for (const PendingEdit &e : *store->pending_edits())
		for (const ve::IVec3 &r : e.result.touched) regions.insert(Key{r.x, r.y, r.z});
	d["pending_edits"] = static_cast<int>(store->pending_edits()->size());
	d["pending_regions"] = to_array(regions);
	std::set<Key> regen;
	if (WorldStreamer *s = world_->context().render->streamer())
		for (const ve::IVec3 &r : s->forced_regen()) regen.insert(Key{r.x, r.y, r.z});
	d["forced_regen"] = to_array(regen);
	d["edit_rejections"] = world_->stats().edit_rejections;
	d["islands"] = world_->island_manager() ? world_->island_manager()->debug_windows() : Array();
	return d;
}
```

- [ ] **Step 3: Build**

Run: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)`
Expected: build OK. (If `PendingEdit::result` is named differently, read `core/world_store.h` and use the real member name.)

- [ ] **Step 4: Write the pin suite with empty goldens**

Create `tests/test_edit_fanout.gd`:

```gdscript
extends GdUnitTestSuite
# Characterization for sub-project 5b (docs/superpowers/plans/2026-09-17-edit-pipeline.md,
# Task 2): which consumer hears which edit, and what each one will act on. Every later task
# must leave these unchanged; Task 10 (staleness) is the only task allowed to move a pin, and
# it moves one in test_connectivity.gd, not here.
#
# Goldens are recorded, not derived: run once, paste each printed "<NAME> <json>" line's JSON
# into the matching const.
#
# debug_edit_fanout() drains every deferred consumer through its own drain first, so the rows
# say what each consumer WILL act on, whether it acts now (today) or at its next tick (once
# the sinks are deferred). Collider rows are expanded chunk SETS and the ops below are tens of
# metres apart, so merging overlapping queue entries cannot move a row. The consolidation
# cases use overlapping ops that share their y and z extents, where a merged bounding box is
# exactly the union.

const EDITS_GOLDEN := {}
const FORCED_COMMIT_GOLDEN := {}
const ASYNC_COMMIT_GOLDEN := {}
const LOD_GOLDEN := {}

# The LoD world settles over hundreds of ticks; these are test_lod_stream.gd's numbers.
const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func own(w: VoxelWorld) -> VoxelWorld:
	add_child(w)
	_worlds.append(w)
	return w

# Region (0, 1, 0) spans y 25.6 .. 51.2; the golden pipeline has at least 2.4 m of rock
# everywhere at y = 38.4, so every op below lands in solid ground, and the ops sit in the
# region's interior so their chunk ranges stay inside the region's own.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	own(w)
	w.ensure_initialized()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func make_lod_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1000.0
	w.max_lod_pages = 32768
	own(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.hooks().debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

func check(name: String, measured: Dictionary, golden: Dictionary) -> void:
	print(name, " ", JSON.stringify(measured))
	assert_bool(golden.is_empty()).override_failure_message(
		"no golden recorded: paste the %s line above into the const" % name).is_false()
	assert_str(JSON.stringify(measured)).override_failure_message(
		"%s moved" % name).is_equal(JSON.stringify(golden))

# A small valid volume for the kOpVolumeAdd row.
func dummy_volume(dim: int) -> PackedByteArray:
	var a := PackedByteArray()
	a.resize(dim * dim * dim)
	a.fill(0)
	return a

# One op at a time, each read straight after, so a row can be attributed to its op. No await:
# nothing else runs between an edit and its read.
func test_each_edit_reaches_its_consumers(timeout := 300000) -> void:
	var w := make_world()
	var h := w.hooks()
	h.debug_stream_region(Vector3i(0, 1, 0))
	var measured := {}
	measured["start"] = h.debug_edit_fanout()
	h.debug_apply_sphere_subtract(Vector3(12.8, 38.4, 12.8), 1.5)
	measured["subtract"] = h.debug_edit_fanout()
	h.debug_apply_sphere_add(Vector3(64.0, 38.4, 12.8), 1.5, 1)
	measured["add"] = h.debug_edit_fanout()
	h.debug_apply_sphere_paint(Vector3(115.2, 38.4, 12.8), 1.5, 4)
	measured["paint"] = h.debug_edit_fanout()
	h.debug_store_volume(7, dummy_volume(4), dummy_volume(4), 4)
	h.debug_apply_volume_add(7, Vector3(166.4, 38.4, 12.8), 0.5, 4)
	measured["volume"] = h.debug_edit_fanout()
	# Region (0, 1, 0) ends at x = 25.6: this one lands in two regions.
	h.debug_apply_sphere_subtract(Vector3(25.6, 38.4, 64.0), 1.0)
	measured["border"] = h.debug_edit_fanout()
	# Fill one region's op list: the 192nd op queues the region for consolidation, and the
	# 257th is rejected.
	for i in range(256):
		h.debug_apply_sphere_paint(Vector3(12.8, 38.4, 115.2), 0.1, 4)
	measured["fill"] = h.debug_edit_fanout()
	h.debug_apply_sphere_paint(Vector3(12.8, 38.4, 115.2), 0.1, 4)
	measured["rejected"] = h.debug_edit_fanout()
	# Beyond ve::kMaxOpRegionSpan: refused outright, nothing touched.
	h.debug_apply_sphere_subtract(Vector3(0.0, 38.4, 300.0), 5000.0)
	measured["oversized"] = h.debug_edit_fanout()
	check("EDITS_GOLDEN", measured, EDITS_GOLDEN)

# The synchronous commit (ConsolidationCoordinator::force_region).
func test_a_forced_consolidation_reaches_its_consumers(timeout := 300000) -> void:
	var w := make_world()
	var h := w.hooks()
	h.debug_stream_region(Vector3i(0, 1, 0))
	for i in range(12):
		h.debug_apply_sphere_subtract(Vector3(8.0 + float(i) * 0.5, 38.4, 12.8), 1.2)
	var measured := {}
	measured["before"] = h.debug_edit_fanout()
	assert_bool(h.debug_consolidate_region(Vector3i(0, 1, 0))).override_failure_message(
		"the forced consolidation refused; the fixture is wrong, not the code").is_true()
	measured["after"] = h.debug_edit_fanout()
	check("FORCED_COMMIT_GOLDEN", measured, FORCED_COMMIT_GOLDEN)

# The asynchronous commit (ConsolidationCoordinator::pump_async), reached by filling a region
# to the 192-op queue threshold.
func test_an_async_consolidation_reaches_its_consumers(timeout := 300000) -> void:
	var w := make_world()
	var h := w.hooks()
	h.debug_stream_region(Vector3i(0, 1, 0))
	for i in range(192):
		h.debug_apply_sphere_subtract(Vector3(8.0 + float(i % 12) * 0.5, 38.4, 12.8), 0.9)
	var measured := {}
	measured["before"] = h.debug_edit_fanout()
	var done := false
	for i in range(600):
		h.debug_pump_consolidation_async()
		if int(h.debug_stream_stats().get("consolidations", 0)) > 0:
			done = true
			break
		await get_tree().process_frame
	assert_bool(done).override_failure_message("the queued region never consolidated").is_true()
	measured["after"] = h.debug_edit_fanout()
	check("ASYNC_COMMIT_GOLDEN", measured, ASYNC_COMMIT_GOLDEN)

# LoD hears an edit and a consolidation. Its marks are only observable in a settled LoD world,
# because LodTree::mark_dirty only touches nodes that exist.
func test_lod_hears_edits_and_consolidations(timeout := 900000) -> void:
	var w := make_lod_world()
	w.set_effect_enabled("near_field", false) # build the fine levels near the edit too
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var surface: Dictionary = w.hooks().debug_raycast(Vector3(400.0, 180.0, 380.0), Vector3.DOWN)
	assert_bool(surface["hit"]).is_true()
	var at: Vector3 = surface["pos"]
	w.hooks().debug_apply_sphere_subtract(at, 8.0)
	w.hooks().debug_drain_invalidations()
	var after_edit := w.hooks().debug_lod_stats()
	var measured := {"edit": [after_edit["dirty_chunks"], after_edit["dirty_levels"]]}
	assert_bool(await settle(w, pos, fwd)).is_true()
	assert_int(w.hooks().debug_lod_stats()["dirty_chunks"]).override_failure_message(
		"the crater never finished rebuilding, so the consolidation row would be ambiguous"
		).is_equal(0)
	var region := Vector3i(floori(at.x / 25.6), floori(at.y / 25.6), floori(at.z / 25.6))
	w.hooks().debug_stream_region(region)
	assert_bool(w.hooks().debug_consolidate_region(region)).override_failure_message(
		"the crater's region did not consolidate; the fixture is wrong, not the code").is_true()
	w.hooks().debug_drain_invalidations()
	var after_commit := w.hooks().debug_lod_stats()
	measured["consolidated"] = [after_commit["dirty_chunks"], after_commit["dirty_levels"]]
	check("LOD_GOLDEN", measured, LOD_GOLDEN)
```

- [ ] **Step 5: Record the goldens**

Run: `./gdunit_tests.sh -a res://tests/test_edit_fanout.gd`
Expected: all four FAIL with `no golden recorded`. Copy each printed `EDITS_GOLDEN {...}`, `FORCED_COMMIT_GOLDEN {...}`, `ASYNC_COMMIT_GOLDEN {...}`, `LOD_GOLDEN {...}` JSON (from the Godot log under `reports/report_N/`) into the matching `const`.

Sanity-check before pasting. If any of these is wrong, the fixture is wrong — fix the fixture, never production code:
- `EDITS_GOLDEN.subtract.islands` has one window with a non-zero last element (the impulse); `EDITS_GOLDEN.paint.islands` has no more windows than `.add.islands` (paint moves no matter).
- `EDITS_GOLDEN.subtract.colliders` is non-empty and `EDITS_GOLDEN.start` is empty everywhere.
- `EDITS_GOLDEN.fill.consolidation_queue` contains `[0, 1, 0]`.
- `EDITS_GOLDEN.rejected.edit_rejections` is greater than `EDITS_GOLDEN.fill.edit_rejections`.
- `EDITS_GOLDEN.oversized` is identical to `EDITS_GOLDEN.rejected` — an oversized op changes no row.
- `FORCED_COMMIT_GOLDEN.after.forced_regen` and `ASYNC_COMMIT_GOLDEN.after.forced_regen` contain `[0, 1, 0]`; both `after.colliders` sets are supersets of their `before`.
- `LOD_GOLDEN.edit[0]` > 0 and `LOD_GOLDEN.edit[1]` >= 4; `LOD_GOLDEN.consolidated[0]` > 0.

- [ ] **Step 6: Run the pins**

Run: `./gdunit_tests.sh -a res://tests/test_edit_fanout.gd`
Expected: all four PASS.

- [ ] **Step 7: Prove the pins bite**

Make all nine breaks below in one edit pass, rebuild once, run the suite, and confirm **every** case fails. For each row, confirm in the printed JSON that the row it names is the one that moved.

| Break | File | Row that must move |
|---|---|---|
| Comment out `edit_sink_->on_edit_appended(op, notify_islands);` | `core/world_store.cpp` | `EDITS_GOLDEN.*.islands` |
| Comment out the `for (const ve::IVec3 &region : r.touched) if (... >= ve::kConsolidateAtOps) consolidation_sink_->queue_consolidation(region);` loop | `core/world_store.cpp` | `EDITS_GOLDEN.fill.consolidation_queue` |
| Comment out `pending_edits_.push_back({op, r});` | `core/world_store.cpp` | `EDITS_GOLDEN.*.pending_edits` |
| Comment out `pending_dirty_.push_back({clo, chi});` | `voxel_world.cpp` | `EDITS_GOLDEN.*.colliders` |
| Comment out `stats_.edit_rejections += static_cast<int>(r.rejected.size());` | `voxel_world.cpp` | `EDITS_GOLDEN.rejected.edit_rejections` |
| Comment out `if (!r.touched.empty()) context_.lod->note_edit(op);` | `voxel_world.cpp` | `LOD_GOLDEN.edit` |
| Comment out `if (streamer()) streamer()->queue_region_regeneration_locked(r);` in the `force_region` commit (`consolidation.cpp:559`) | `mesh/consolidation.cpp` | `FORCED_COMMIT_GOLDEN.after.forced_regen` |
| Comment out the same call in the `pump_async` commit (`consolidation.cpp:170`) | `mesh/consolidation.cpp` | `ASYNC_COMMIT_GOLDEN.after.forced_regen` |
| Comment out the `lod_tree()->mark_dirty(lo, hi);` in the `force_region` commit (`consolidation.cpp:557`) | `mesh/consolidation.cpp` | `LOD_GOLDEN.consolidated` |

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_edit_fanout.gd
git checkout -- extension/src
git status --short extension/src   # must be empty
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_edit_fanout.gd   # PASS again
```

- [ ] **Step 8: Commit**

```bash
git add tests/test_edit_fanout.gd extension/src
git commit -m "test: pin the edit fan-out before the pipeline move"
```

---

### Task 3: Pin the staleness rule and prove the test_connectivity workaround is needed

Two characterizations that later tasks change or remove on purpose.

**Files:**
- Modify: `tests/test_connectivity.gd`
- Modify: `tests/test_consolidation.gd`

**Interfaces:**
- Consumes: `debug_island_stats()["land_stale"]`, `debug_consolidate_region`, `debug_region_op_count`, `debug_pump_consolidation`.
- Produces: `test_a_consolidation_during_an_extraction_is_pinned` (Task 10 moves it), `test_a_full_op_list_does_not_stay_full_while_consolidation_runs` (Task 4 builds on it), and `make_world(override_bricks)` in `test_connectivity.gd`.

- [ ] **Step 1: Give test_connectivity's make_world a pool argument**

In `tests/test_connectivity.gd`, change the signature and the assignment:

```gdscript
func make_world(override_bricks := 1) -> VoxelWorld:
```

```gdscript
	w.max_override_bricks = override_bricks
```

(The comment above that line stays as it is; Task 4 replaces both.)

- [ ] **Step 2: Write the staleness pin**

Append to `tests/test_connectivity.gd`:

```gdscript
# Pins today's staleness rule for sub-project 5b (docs/superpowers/plans/2026-09-17-edit-pipeline.md,
# Task 3). land_extraction compares the ops it captured with the ops the log holds now, so a
# consolidation that bakes those ops away over the component's boxes reads as "the field moved"
# and the extraction is refused. Task 10 moves this pin on purpose: a bake changes no field
# value, so the extraction lands instead. A real override pool is needed here (the suite's
# one-brick default cannot bake a region).
func test_a_consolidation_during_an_extraction_is_pinned(timeout := 180000) -> void:
	var w := make_world(8192)
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(120):
		await get_tree().physics_frame
		step(w, 1)
		st = w.hooks().debug_island_stats()
		if st["in_flight"] > 0:
			break
	assert_int(st["in_flight"]).override_failure_message(
		"the connectivity pass did not submit an extraction: %s" % st).is_greater(0)
	# The pillar stands in region (0, 2, 0); baking it clears the ops the extraction captured.
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).override_failure_message(
		"the pillar's region did not consolidate; the fixture is wrong, not the code").is_true()
	var stale_before: int = st["land_stale"]
	for i in range(240):
		await get_tree().physics_frame
		step(w, 1)
		st = w.hooks().debug_island_stats()
		if st["land_stale"] > stale_before or st["islands_spawned"] + st["debris_spawned"] > 0:
			break
	assert_int(st["land_stale"]).override_failure_message(
		"a consolidation during an extraction no longer reads as stale: %s" % st
		).is_greater(stale_before)
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"the stale extraction still spawned a body: %s" % st).is_equal(0)
```

- [ ] **Step 3: Write the workaround proof**

Append to `tests/test_consolidation.gd`:

```gdscript
# The premise behind test_connectivity.gd's max_override_bricks = 1 (sub-project 5b,
# docs/superpowers/plans/2026-09-17-edit-pipeline.md, Task 3). That suite fills a region's op
# list on purpose to exercise the fail-soft rejection paths; with a normal override pool the
# automatic consolidation bakes the list away first, so a test built on a full list would be
# vacuous. Task 4 replaces the pool trick with an explicit hold, and this case is what proves
# the trick was doing something.
func test_a_full_op_list_does_not_stay_full_while_consolidation_runs(timeout := 120000) -> void:
	var w := make_world()
	w.hooks().debug_stream_region(Vector3i(0, 1, 0))
	for i in range(256):
		w.hooks().debug_apply_sphere_paint(Vector3(12.8, 38.4, 12.8), 0.1, 4)
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 1, 0))).override_failure_message(
		"the region did not reach its op cap; the fixture is wrong").is_equal(256)
	for i in range(8):
		w.hooks().debug_pump_consolidation() # what _process does once a frame
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 1, 0))).override_failure_message(
		"a full op list survived consolidation: the max_override_bricks = 1 workaround may be "
		+ "unnecessary. Stop and report before Task 4.").is_less(256)
```

- [ ] **Step 4: Run both**

```bash
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_consolidation.gd
```

Expected: both new cases PASS, and the rest of both suites match the baseline. If `test_a_full_op_list_does_not_stay_full_while_consolidation_runs` fails, stop and report: the workaround's premise is false and Task 4 needs re-planning.

- [ ] **Step 5: Prove both bite**

| Break | File | Case that must fail |
|---|---|---|
| In `land_extraction`, replace `const bool stale = now.size() != then.size() ||` … with `const bool stale = false;` | `physics/island_manager.cpp` | `test_a_consolidation_during_an_extraction_is_pinned` |
| Comment out the `consolidation_sink_->queue_consolidation(region)` loop | `core/world_store.cpp` | `test_a_full_op_list_does_not_stay_full_while_consolidation_runs` |

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_consolidation.gd
git checkout -- extension/src
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

- [ ] **Step 6: Commit**

```bash
git add tests/test_connectivity.gd tests/test_consolidation.gd
git commit -m "test: pin extraction staleness and the full-op-list workaround premise"
```

---

### Task 4: The consolidation hold seam

Replaces `max_override_bricks = 1` in `test_connectivity.gd` with a switch that says what it means.

**Files:**
- Modify: `extension/src/mesh/consolidation.h`, `consolidation.cpp`
- Modify: `extension/src/debug/hooks.h`, `hooks.cpp`, `hooks_world.cpp`
- Modify: `tests/test_consolidation.gd`, `tests/test_connectivity.gd`

**Interfaces:**
- Consumes: `ConsolidationCoordinator::pump_async`.
- Produces: `ConsolidationCoordinator::set_held(bool)`, `VoxelDebugHooks::debug_hold_consolidation(bool)`, `test_connectivity.gd`'s `make_world(hold_consolidation := true)`.

- [ ] **Step 1: Add the hold to the coordinator**

In `extension/src/mesh/consolidation.h`, next to `void pump_async();`:

```cpp
	// Test seam: while held, regions still queue and an in-flight transaction still finishes,
	// but pump_async starts no new bake. test_connectivity.gd holds consolidation so the full
	// op lists its fail-soft cases build on stay full (test_consolidation.gd pins why).
	// Written and read under edit_mutex().
	void set_held(bool held) { held_ = held; }
```

and next to `bool consolidation_queue_refusal_logged_ = false;`:

```cpp
	bool held_ = false; // guarded by edit_mutex()
```

In `extension/src/mesh/consolidation.cpp`, in `pump_async`, change:

```cpp
	if (consolidation_queue_.empty()) return;
```

to:

```cpp
	// Held: the queue keeps growing and the in-flight transaction above still finished; only
	// new bakes are refused.
	if (held_ || consolidation_queue_.empty()) return;
```

- [ ] **Step 2: Add the hook**

In `extension/src/debug/hooks.h`, next to `void debug_pump_consolidation();`:

```cpp
	// Test seam: stop pump_async from starting new bakes (see ConsolidationCoordinator).
	void debug_hold_consolidation(bool held);
```

In `extension/src/debug/hooks.cpp`, next to the `debug_pump_consolidation` binding:

```cpp
	ClassDB::bind_method(D_METHOD("debug_hold_consolidation", "held"), &VoxelDebugHooks::debug_hold_consolidation);
```

In `extension/src/debug/hooks_world.cpp`, next to `debug_pump_consolidation`:

```cpp
void VoxelDebugHooks::debug_hold_consolidation(bool held) {
	std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
	world_->context().consolidation->set_held(held);
}
```

- [ ] **Step 3: Write the held case**

Append to `tests/test_consolidation.gd`:

```gdscript
# The hold seam that replaces test_connectivity.gd's max_override_bricks = 1: a held
# consolidation leaves the op list alone, and releasing the hold bakes it.
func test_a_held_consolidation_keeps_a_full_op_list_full(timeout := 120000) -> void:
	var w := make_world()
	w.hooks().debug_hold_consolidation(true)
	w.hooks().debug_stream_region(Vector3i(0, 1, 0))
	for i in range(256):
		w.hooks().debug_apply_sphere_paint(Vector3(12.8, 38.4, 12.8), 0.1, 4)
	for i in range(8):
		w.hooks().debug_pump_consolidation()
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 1, 0))).override_failure_message(
		"a held consolidation baked the op list anyway").is_equal(256)
	# A full list still rejects, which is what the connectivity suite builds its fail-soft
	# cases on.
	var before: int = int(w.hooks().debug_stream_stats()["edit_rejections"])
	w.hooks().debug_apply_sphere_paint(Vector3(12.8, 38.4, 12.8), 0.1, 4)
	assert_int(int(w.hooks().debug_stream_stats()["edit_rejections"])).override_failure_message(
		"the 257th op was not rejected while the list was full").is_greater(before)
	w.hooks().debug_hold_consolidation(false)
	for i in range(8):
		w.hooks().debug_pump_consolidation()
	assert_int(w.hooks().debug_region_op_count(Vector3i(0, 1, 0))).override_failure_message(
		"releasing the hold did not bake the queued region").is_less(256)
```

- [ ] **Step 4: Switch test_connectivity to the hold**

In `tests/test_connectivity.gd`, replace the signature from Task 3 and the pool block:

```gdscript
func make_world(hold_consolidation := true) -> VoxelWorld:
```

Delete these four lines:

```gdscript
	# Connectivity tests deliberately exercise fail-soft full op lists. M7's async
	# consolidation would otherwise bake those lists into override bricks and clear them
	# before the re-merge/preflight runs, so give this suite a one-brick override pool that
	# cannot absorb a real region bake and leaves the op lists full.
	w.max_override_bricks = override_bricks
```

and, after the `debug_init_physics()` assertion at the end of `make_world`, add:

```gdscript
	# Connectivity tests deliberately exercise fail-soft full op lists; the automatic
	# consolidation would bake those lists away before the re-merge/preflight runs
	# (test_consolidation.gd::test_a_full_op_list_does_not_stay_full_while_consolidation_runs
	# pins that). Hold it instead of crippling the override pool.
	if hold_consolidation:
		w.hooks().debug_hold_consolidation(true)
```

In `test_a_consolidation_during_an_extraction_is_pinned`, change `var w := make_world(8192)` to `var w := make_world(false)`.

- [ ] **Step 5: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_consolidation.gd,res://tests/test_connectivity.gd,res://tests/test_edit_fanout.gd
```

Expected: the new case passes; `test_connectivity` matches the baseline (its known flaky-by-case rule applies: compare the failure COUNT); the fan-out pins are unchanged.

- [ ] **Step 6: Commit**

```bash
git add extension/src tests/test_consolidation.gd tests/test_connectivity.gd
git commit -m "test: hold consolidation instead of starving the override pool"
```

---

### Task 5: `EditPipeline` and `merge_or_cap`

The pure module and its native contract. Nothing calls it yet.

**Files:**
- Create: `extension/src/core/edit_pipeline.h`, `extension/src/core/edit_pipeline.cpp`
- Create: `extension/tests/test_edit_pipeline.cpp`

**Interfaces:**
- Consumes: `ve::EditLog`, `ve::EditOp`, `ve::IVec3`, `ve::brick_world_aabb`, `ve::op_world_aabb`, `ve::op_region_range`, `ve::edit_op_is_well_formed`, `ve::op_region_span_ok`, `ve::kMaxRegionOps`, `ve::kRegionBricks`.
- Produces: `ve::InvalidationReason`, `ve::Invalidation` (+ `Invalidation::consolidated(IVec3)`), `ve::InvalidationSink::record(const Invalidation &)`, `ve::EditPolicy{atomic, notify_islands}`, `ve::BatchResult{ops, refused, full}`, `ve::EditPipeline{preflight, apply, invalidate, add_sink, remove_sink}`, `ve::Box3<T>`, `ve::merge_or_cap`, `ve::kInvalidationQueueCap`.

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_edit_pipeline.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/edit_pipeline.h"
#include <atomic>
#include <cmath>
#include <vector>

namespace {

ve::EditOp sphere(float x, float y, float z, float r, uint32_t type = ve::kOpSphereSubtract) {
	ve::EditOp op{};
	op.type = type;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

// Region (0, 0, 0) spans [0, 25.6) m; a small op at its centre touches only that region.
ve::EditOp in_r0() { return sphere(12.8f, 12.8f, 12.8f, 0.5f); }
// Region (2, 0, 0)'s centre, a region away from r0.
ve::EditOp in_r2() { return sphere(64.0f, 12.8f, 12.8f, 0.5f); }
// The boundary at x = 25.6 m: this one lands in regions (0, 0, 0) and (1, 0, 0).
ve::EditOp across() { return sphere(25.6f, 12.8f, 12.8f, 1.0f); }

const ve::IVec3 kR0{0, 0, 0};
const ve::IVec3 kR1{1, 0, 0};

struct Recorder : ve::InvalidationSink {
	std::vector<ve::InvalidationReason> reasons;
	std::vector<int64_t> seqs;
	std::vector<bool> notify;
	std::vector<ve::IVec3> regions;
	void record(const ve::Invalidation &inv) override {
		reasons.push_back(inv.reason);
		seqs.push_back(inv.seq);
		notify.push_back(inv.notify_islands);
		regions.push_back(inv.region);
	}
};

struct Fixture {
	ve::EditLog storage;
	ve::EditLog *log = &storage;
	std::atomic<int64_t> seq{0};
	ve::EditPipeline pipeline{&log, &seq};
	Recorder sink;
	Fixture() { pipeline.add_sink(&sink); }
	void fill(ve::EditOp op, int n) {
		for (int i = 0; i < n; i++) storage.append(op);
	}
};

} // namespace

TEST_CASE("an accepted op appends, bumps the seq once and tells the sinks") {
	Fixture f;
	const ve::EditOp op = in_r0();
	const ve::BatchResult r = f.pipeline.apply({&op, 1}, {});
	CHECK(r.ops.size() == 1);
	CHECK(r.ops[0].touched.size() == 1);
	CHECK(f.seq.load() == 1);
	REQUIRE(f.sink.reasons.size() == 1);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kEdit);
	CHECK(f.sink.seqs[0] == 1);
	CHECK(f.sink.notify[0] == true);
}

TEST_CASE("an op that touches nothing moves no seq and tells no sink") {
	Fixture f;
	const ve::EditOp bad = sphere(1.0f, 1.0f, 1.0f, std::nanf("")); // not well-formed
	const ve::BatchResult r = f.pipeline.apply({&bad, 1}, {});
	CHECK(r.ops[0].touched.empty());
	CHECK(f.seq.load() == 0);
	CHECK(f.sink.reasons.empty());
}

TEST_CASE("a fully rejected op reports kRejected and nothing else") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps);
	const ve::EditOp op = in_r0();
	const ve::BatchResult r = f.pipeline.apply({&op, 1}, {});
	CHECK(r.ops[0].rejected.size() == 1);
	CHECK(r.ops[0].touched.empty());
	CHECK(f.seq.load() == 0);
	REQUIRE(f.sink.reasons.size() == 1);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kRejected);
}

TEST_CASE("an oversized op reports kRejected") {
	Fixture f;
	const ve::EditOp huge = sphere(0.0f, 0.0f, 0.0f, 5000.0f);
	const ve::BatchResult r = f.pipeline.apply({&huge, 1}, {});
	CHECK(r.ops[0].oversized);
	REQUIRE(f.sink.reasons.size() == 1);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kRejected);
}

TEST_CASE("a non-atomic op is partially accepted") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps);
	const ve::EditOp op = across();
	const ve::BatchResult r = f.pipeline.apply({&op, 1}, {});
	CHECK(r.ops[0].rejected.size() == 1);
	CHECK(r.ops[0].rejected[0] == kR0);
	CHECK(f.storage.op_count(kR1) == 1);
	CHECK(f.seq.load() == 1);
	REQUIRE(f.sink.reasons.size() == 2);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kRejected);
	CHECK(f.sink.reasons[1] == ve::InvalidationReason::kEdit);
}

TEST_CASE("an atomic batch is all or nothing") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps - 1);
	const ve::EditOp ops[2] = {in_r0(), in_r0()};
	const ve::BatchResult r = f.pipeline.apply({ops, 2}, {.atomic = true});
	CHECK(r.refused);
	REQUIRE(r.full.size() == 1);
	CHECK(r.full[0] == kR0);
	CHECK(f.storage.op_count(kR0) == ve::kMaxRegionOps - 1);
	CHECK(f.seq.load() == 0);
	CHECK(f.sink.reasons.empty());
	// One op still fits, and then the same batch does not.
	const ve::BatchResult one = f.pipeline.apply({ops, 1}, {.atomic = true});
	CHECK_FALSE(one.refused);
	CHECK(f.storage.op_count(kR0) == ve::kMaxRegionOps);
}

TEST_CASE("an atomic batch counts its own ops against the cap, region by region") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps - 2);
	// Each op fits on its own; two of them in the same region do not.
	std::vector<ve::EditOp> ops = {in_r0(), in_r2(), in_r0(), in_r0()};
	const ve::BatchResult r = f.pipeline.apply(ops, {.atomic = true});
	CHECK(r.refused);
	REQUIRE(r.full.size() == 1);
	CHECK(r.full[0] == kR0);
	CHECK(f.storage.op_count(kR0) == ve::kMaxRegionOps - 2);
	CHECK(f.storage.op_count({2, 0, 0}) == 0);
}

TEST_CASE("an atomic batch holding an oversized op is refused whole") {
	Fixture f;
	std::vector<ve::EditOp> ops = {in_r0(), sphere(0.0f, 0.0f, 0.0f, 5000.0f)};
	const ve::BatchResult r = f.pipeline.apply(ops, {.atomic = true});
	CHECK(r.refused);
	CHECK(r.full.empty()); // no region is at the cap; the op itself is the refusal
	CHECK(f.storage.op_count(kR0) == 0);
	CHECK(f.sink.reasons.empty());
}

TEST_CASE("preflight answers without appending") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps);
	const ve::EditOp op = in_r0();
	std::vector<ve::IVec3> full;
	CHECK_FALSE(f.pipeline.preflight({&op, 1}, &full));
	REQUIRE(full.size() == 1);
	CHECK(full[0] == kR0);
	CHECK(f.storage.op_count(kR0) == ve::kMaxRegionOps);
	CHECK(f.sink.reasons.empty());
}

TEST_CASE("notify_islands travels with the invalidation") {
	Fixture f;
	const ve::EditOp op = in_r0();
	f.pipeline.apply({&op, 1}, {.atomic = false, .notify_islands = false});
	REQUIRE(f.sink.notify.size() == 1);
	CHECK(f.sink.notify[0] == false);
}

TEST_CASE("a removed sink hears nothing") {
	Fixture f;
	f.pipeline.remove_sink(&f.sink);
	const ve::EditOp op = in_r0();
	f.pipeline.apply({&op, 1}, {});
	CHECK(f.sink.reasons.empty());
	CHECK(f.seq.load() == 1); // the edit still happened
}

TEST_CASE("invalidate carries the region and its world box") {
	Fixture f;
	const ve::Invalidation inv = ve::Invalidation::consolidated({1, 2, 3});
	CHECK(inv.reason == ve::InvalidationReason::kConsolidated);
	CHECK(inv.region == ve::IVec3{1, 2, 3});
	// One region is 32 bricks of 0.8 m.
	CHECK(inv.lo[0] == doctest::Approx(25.6f));
	CHECK(inv.lo[1] == doctest::Approx(51.2f));
	CHECK(inv.lo[2] == doctest::Approx(76.8f));
	CHECK(inv.hi[0] == doctest::Approx(51.2f));
	CHECK(inv.hi[1] == doctest::Approx(76.8f));
	CHECK(inv.hi[2] == doctest::Approx(102.4f));
	f.pipeline.invalidate(inv);
	REQUIRE(f.sink.reasons.size() == 1);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kConsolidated);
	CHECK(f.sink.regions[0] == ve::IVec3{1, 2, 3});
	CHECK(f.seq.load() == 0);
}

TEST_CASE("with no edit log an apply is a no-op with one empty result per op") {
	ve::EditLog *none = nullptr;
	std::atomic<int64_t> seq{0};
	ve::EditPipeline pipeline(&none, &seq);
	Recorder sink;
	pipeline.add_sink(&sink);
	std::vector<ve::EditOp> ops = {in_r0(), in_r2()};
	const ve::BatchResult r = pipeline.apply(ops, {});
	CHECK(r.ops.size() == 2);
	CHECK(r.ops[0].touched.empty());
	CHECK_FALSE(r.refused);
	CHECK(sink.reasons.empty());
	CHECK(seq.load() == 0);
}

TEST_CASE("merge_or_cap merges what overlaps and keeps what does not") {
	std::vector<ve::Box3<int>> q;
	ve::merge_or_cap(&q, ve::Box3<int>{{0, 0, 0}, {1, 1, 1}});
	ve::merge_or_cap(&q, ve::Box3<int>{{1, 0, 0}, {2, 1, 1}});
	REQUIRE(q.size() == 1);
	CHECK(q[0].lo[0] == 0);
	CHECK(q[0].hi[0] == 2);
	ve::merge_or_cap(&q, ve::Box3<int>{{10, 10, 10}, {11, 11, 11}});
	CHECK(q.size() == 2);
}

TEST_CASE("merge_or_cap absorbs every box a bridging box reaches") {
	std::vector<ve::Box3<int>> q;
	ve::merge_or_cap(&q, ve::Box3<int>{{0, 0, 0}, {1, 1, 1}});
	ve::merge_or_cap(&q, ve::Box3<int>{{5, 0, 0}, {6, 1, 1}});
	REQUIRE(q.size() == 2);
	ve::merge_or_cap(&q, ve::Box3<int>{{1, 0, 0}, {5, 1, 1}});
	REQUIRE(q.size() == 1);
	CHECK(q[0].lo[0] == 0);
	CHECK(q[0].hi[0] == 6);
}

TEST_CASE("past the cap merge_or_cap folds the queue into one box") {
	std::vector<ve::Box3<float>> q;
	for (int i = 0; i < 5; i++) {
		const float x = static_cast<float>(i) * 10.0f;
		ve::merge_or_cap(&q, ve::Box3<float>{{x, 0.0f, 0.0f}, {x + 1.0f, 1.0f, 1.0f}}, 4);
	}
	REQUIRE(q.size() == 1);
	CHECK(q[0].lo[0] == doctest::Approx(0.0f));
	CHECK(q[0].hi[0] == doctest::Approx(41.0f));
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd extension && scons -Q test; cd ..`
Expected: FAIL — `core/edit_pipeline.h` does not exist.

- [ ] **Step 3: Write the header**

Create `extension/src/core/edit_pipeline.h`:

```cpp
#pragma once
// EditPipeline — the one path an edit takes into the world, and the one place that tells every
// consumer about it (docs/superpowers/specs/2026-09-17-edit-pipeline-design.md).
//
// LOCK ORDER. This is the only place it is stated:
//
//	admission (g_voxel_compositor_admission_mutex) -> RenderOrchestrator::render_lifetime_mutex_
//	  -> WorldStore::edit_mutex()
//
// NOTHING NESTS INSIDE THE EDIT LOCK. apply(), preflight(), invalidate(), add_sink() and
// remove_sink() all run with the edit lock held, and a sink's record() only appends to that
// sink's own edit-lock-guarded queue -- it takes no lock of its own. The owner drains: it
// swaps its queue out under the edit lock, releases the lock, and only then acts (takes its
// own mutex, marks chunks, labels windows). That is what keeps LodSystem::mutex() and
// IslandManager's window bookkeeping off the edit path.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "generator/edit_ops.h"
#include "world/edit_log.h"
#include "world/region.h"

namespace ve {

enum class InvalidationReason {
	kEdit,         // an op changed field state in at least one region
	kRejected,     // an op was refused: a full region list, or an oversized op
	kConsolidated, // a region's ops became override bricks; the field did not change
};

struct Invalidation {
	InvalidationReason reason = InvalidationReason::kEdit;
	// kEdit: the op's own world AABB. kConsolidated: the region's.
	float lo[3] = {0.0f, 0.0f, 0.0f};
	float hi[3] = {0.0f, 0.0f, 0.0f};
	IVec3 region{}; // kConsolidated only: sinks derive brick/chunk ranges from this, never
	                // from the float box, so no rounding can move a range.
	int64_t seq = 0; // the world edit sequence AFTER this op's bump
	const EditOp *op = nullptr;                    // kEdit, kRejected
	const EditLog::AppendResult *append = nullptr; // kEdit, kRejected
	bool notify_islands = true;

	static Invalidation consolidated(IVec3 region);
};

struct InvalidationSink {
	virtual ~InvalidationSink() = default;
	// Called with the edit lock HELD. Append to your own edit-lock-guarded queue and return;
	// take no other lock, and do no work a drain could do.
	virtual void record(const Invalidation &inv) = 0;
};

struct EditPolicy {
	// All-or-nothing: every op's regions are checked against the cap before anything is
	// appended, so a batch that is accepted is accepted whole.
	bool atomic = false;
	// False only for the island manager's own crumble carve (see EditSink's old comment):
	// the matter it removes was already labelled unanchored.
	bool notify_islands = true;
};

struct BatchResult {
	std::vector<EditLog::AppendResult> ops; // one per input op, in order
	bool refused = false;                   // atomic only: nothing was appended
	std::vector<IVec3> full;                // atomic only: the regions at the op cap. Empty
	                                        // when the refusal was a malformed or oversized op
};

class EditPipeline {
public:
	// Both pointers are to slots that outlive the pipeline: the log is created lazily and
	// released at exit, so the slot is read at every use.
	EditPipeline(EditLog *const *log, std::atomic<int64_t> *seq) : log_(log), seq_(seq) {}

	// Would this batch be accepted whole? Callers that must not half-apply (the island
	// manager's paste, its landing carve) ask this BEFORE they store, pin, upload or spawn,
	// then apply({.atomic = true}) under the same lock hold.
	bool preflight(std::span<const EditOp> ops, std::vector<IVec3> *full) const;
	// Caller holds WorldStore::edit_mutex().
	BatchResult apply(std::span<const EditOp> ops, EditPolicy policy);
	// Consolidation's commit: the base bytes changed without an op. Caller holds the lock.
	void invalidate(const Invalidation &inv);
	// Caller holds the lock, and holds no other lock.
	void add_sink(InvalidationSink *sink);
	void remove_sink(InvalidationSink *sink);

private:
	void record(const Invalidation &inv);

	EditLog *const *log_ = nullptr;
	std::atomic<int64_t> *seq_ = nullptr;
	std::vector<InvalidationSink *> sinks_;
};

// How many boxes a deferred sink's queue holds before it folds (see merge_or_cap).
inline constexpr size_t kInvalidationQueueCap = 1024;

// An inclusive box, in whatever units the sink queues: world metres for LoD marks, chunk
// coordinates for the collider remesh queue.
template <typename T>
struct Box3 {
	T lo[3];
	T hi[3];
};

// Queue `box`, merging it into every queued box it overlaps, so a sink's queue stays bounded
// by the number of DISJOINT areas edited rather than by the number of edits. Over-covering is
// safe for every consumer here (it re-marks ground that did not change); losing a box is not.
template <typename T>
void merge_or_cap(std::vector<Box3<T>> *queue, Box3<T> box, size_t cap = kInvalidationQueueCap) {
	if (!queue) return;
	const auto overlaps = [](const Box3<T> &a, const Box3<T> &b) {
		for (int k = 0; k < 3; k++)
			if (a.lo[k] > b.hi[k] || a.hi[k] < b.lo[k]) return false;
		return true;
	};
	const auto unite = [](Box3<T> *a, const Box3<T> &b) {
		for (int k = 0; k < 3; k++) {
			if (b.lo[k] < a->lo[k]) a->lo[k] = b.lo[k];
			if (b.hi[k] > a->hi[k]) a->hi[k] = b.hi[k];
		}
	};
	// A box can bridge two queued boxes that did not overlap each other, so restart after
	// each absorption until nothing more overlaps.
	for (size_t i = 0; i < queue->size();) {
		if (!overlaps((*queue)[i], box)) {
			i++;
			continue;
		}
		unite(&box, (*queue)[i]);
		(*queue)[i] = queue->back();
		queue->pop_back();
		i = 0;
	}
	queue->push_back(box);
	if (queue->size() <= cap) return;
	// ponytail: past the cap everything folds into one bounding box, which can mark a huge
	// span dirty. Reaching the cap needs a drain that is not running, and the fold costs
	// rebuilds rather than correctness. If it is ever reached with a live drain, fold
	// pairwise by smallest growth instead.
	Box3<T> all = (*queue)[0];
	for (const Box3<T> &b : *queue) unite(&all, b);
	queue->assign(1, all);
}

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/core/edit_pipeline.cpp`:

```cpp
#include "core/edit_pipeline.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace ve {

Invalidation Invalidation::consolidated(IVec3 region) {
	Invalidation inv;
	inv.reason = InvalidationReason::kConsolidated;
	inv.region = region;
	const IVec3 base{region.x * kRegionBricks, region.y * kRegionBricks, region.z * kRegionBricks};
	float first_hi[3], last_lo[3];
	brick_world_aabb(base, inv.lo, first_hi);
	brick_world_aabb({base.x + kRegionBricks - 1, base.y + kRegionBricks - 1,
							 base.z + kRegionBricks - 1},
			last_lo, inv.hi);
	// A bake changes no field value, so nothing that was holding on can have been loosened.
	inv.notify_islands = false;
	return inv;
}

bool EditPipeline::preflight(std::span<const EditOp> ops, std::vector<IVec3> *full) const {
	if (full) full->clear();
	EditLog *log = log_ ? *log_ : nullptr;
	if (!log) return false;
	bool ok = true;
	std::map<std::tuple<int, int, int>, int> adds;
	for (const EditOp &op : ops) {
		// EditLog::append refuses these outright, so a batch holding one can never be
		// accepted whole. There is no region to name.
		if (!edit_op_is_well_formed(op) || !op_region_span_ok(op)) {
			ok = false;
			continue;
		}
		IVec3 lo{}, hi{};
		op_region_range(op, &lo, &hi);
		for (int z = lo.z; z <= hi.z; z++)
			for (int y = lo.y; y <= hi.y; y++)
				for (int x = lo.x; x <= hi.x; x++) adds[std::tuple<int, int, int>{x, y, z}]++;
	}
	for (const auto &entry : adds) {
		const IVec3 region{std::get<0>(entry.first), std::get<1>(entry.first),
				std::get<2>(entry.first)};
		if (log->op_count(region) + entry.second <= kMaxRegionOps) continue;
		ok = false;
		if (full) full->push_back(region);
	}
	return ok;
}

BatchResult EditPipeline::apply(std::span<const EditOp> ops, EditPolicy policy) {
	BatchResult out;
	out.ops.resize(ops.size());
	EditLog *log = log_ ? *log_ : nullptr;
	if (!log) return out;
	if (policy.atomic && !preflight(ops, &out.full)) {
		out.refused = true;
		return out;
	}
	for (size_t i = 0; i < ops.size(); i++) {
		const EditOp &op = ops[i];
		// A reference into a vector that was sized up front: it never reallocates, so the
		// pointer an Invalidation carries stays valid for the whole record() call.
		EditLog::AppendResult &r = out.ops[i];
		r = log->append(op);
		if (r.oversized || !r.rejected.empty()) {
			Invalidation inv;
			inv.reason = InvalidationReason::kRejected;
			inv.seq = seq_ ? seq_->load(std::memory_order_relaxed) : 0;
			inv.op = &op;
			inv.append = &r;
			inv.notify_islands = policy.notify_islands;
			record(inv);
		}
		// Empty results are fail-soft no-ops: malformed/oversized and fully rejected ops
		// changed no field state, so they must not advance the edit sequence, wake
		// connectivity, or enter the render-thread pending queue.
		if (r.touched.empty()) continue;
		// Bump AFTER the append and under the same lock the streamer uses to capture op
		// counts. If the seq moved before the append, a readback stamped between the bump and
		// the append would claim edits that are not in the GPU state it describes.
		if (seq_) seq_->fetch_add(1, std::memory_order_relaxed);
		Invalidation inv;
		inv.reason = InvalidationReason::kEdit;
		op_world_aabb(op, inv.lo, inv.hi);
		inv.seq = seq_ ? seq_->load(std::memory_order_relaxed) : 0;
		inv.op = &op;
		inv.append = &r;
		inv.notify_islands = policy.notify_islands;
		record(inv);
	}
	return out;
}

void EditPipeline::invalidate(const Invalidation &inv) { record(inv); }

void EditPipeline::add_sink(InvalidationSink *sink) {
	if (!sink) return;
	if (std::find(sinks_.begin(), sinks_.end(), sink) != sinks_.end()) return;
	sinks_.push_back(sink);
}

void EditPipeline::remove_sink(InvalidationSink *sink) {
	sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void EditPipeline::record(const Invalidation &inv) {
	for (InvalidationSink *sink : sinks_) sink->record(inv);
}

} // namespace ve
```

- [ ] **Step 5: Run the native tests**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS, with the whole suite green (`core/*.cpp` is already in the native build's pure globs, so the new file is picked up automatically).

- [ ] **Step 6: Build the extension**

Run: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)`
Expected: build OK (nothing calls the pipeline yet).

- [ ] **Step 7: Commit**

```bash
git add extension/src/core/edit_pipeline.h extension/src/core/edit_pipeline.cpp extension/tests/test_edit_pipeline.cpp
git commit -m "feat: one edit pipeline with atomic batches and invalidation sinks"
```

---

### Task 6: Both spines move into `apply`

The two halves of the append become one `apply()`; every consumer becomes a sink. LoD and islands stay synchronous inside `record()` for now (Tasks 11 and 12 defer them).

**Files:**
- Modify: `extension/src/core/world_store.h`, `world_store.cpp`
- Modify: `extension/src/voxel_world.h`, `voxel_world.cpp`
- Modify: `extension/src/lod/lod_system.h`, `lod_system.cpp`
- Modify: `extension/src/physics/island_manager.h`, `island_manager.cpp`
- Modify: `extension/src/mesh/consolidation.h`, `consolidation.cpp`

**Interfaces:**
- Consumes: `ve::EditPipeline` (Task 5).
- Produces: `WorldStore::edits() -> ve::EditPipeline &`; `WorldStore`, `VoxelWorld`, `LodSystem`, `IslandManager` and `ConsolidationCoordinator` all implement `ve::InvalidationSink`. `WorldStore::append_edit{,_locked}`, `EditSink`, `ConsolidationSink`, `WorldStore::bump_edit_seq`, `VoxelWorld::on_edit_appended` and `LodSystem::note_edit`'s public declaration are gone.

- [ ] **Step 1: Turn WorldStore into the pipeline's owner and the pending-edits sink**

In `extension/src/core/world_store.h`:

- add `#include "core/edit_pipeline.h"` next to the other includes;
- delete the whole `struct EditSink { ... };` and `struct ConsolidationSink { ... };` blocks;
- change the class declaration to `class WorldStore : public ve::InvalidationSink {`;
- delete `set_sinks(...)`, `append_edit(...)`, `append_edit_locked(...)`, `bump_edit_seq()`, and the members `EditSink *edit_sink_` and `ConsolidationSink *consolidation_sink_`;
- in place of the deleted spine declarations, add:

```cpp
	// THE edit spine. Tools take edit_mutex() and call edits().apply(...); consolidation
	// calls edits().invalidate(...). Lock order and the sink contract: core/edit_pipeline.h.
	ve::EditPipeline &edits() { return pipeline_; }
	// InvalidationSink: the streamer handoff queue. Edit lock held; queue only.
	void record(const ve::Invalidation &inv) override;
```

- replace the lock-order paragraph above `std::mutex &edit_mutex()` with:

```cpp
	// THE edit mutex; guards the edit log, override tables' append path, pending_edits_,
	// and everything the fan-out touches while an op is accepted. Acquisition order and the
	// rule that nothing nests inside it: core/edit_pipeline.h.
	std::mutex &edit_mutex() { return edit_mutex_; }
```

- add, as the last private member:

```cpp
	// Declared last: it holds the addresses of edit_log_ and edit_seq_ above.
	ve::EditPipeline pipeline_{&edit_log_, &edit_seq_};
```

In `extension/src/core/world_store.cpp`:

- delete `WorldStore::append_edit`, `WorldStore::append_edit_locked` and `WorldStore::bump_edit_seq`;
- add `pipeline_.add_sink(this);` as the constructor body (it currently has an empty body `{}`; make it `{ pipeline_.add_sink(this); }`), with the comment:

```cpp
	// The store is its own sink for the streamer handoff queue. Registered here because
	// nothing else exists yet: no lock is needed and none is held.
```

- add:

```cpp
void WorldStore::record(const ve::Invalidation &inv) {
	// The render thread's copy of the edit; WorldStreamer::run_frame swaps this queue out
	// under the same lock.
	if (inv.reason == ve::InvalidationReason::kEdit)
		pending_edits_.push_back({*inv.op, *inv.append});
}
```

- [ ] **Step 2: Make VoxelWorld the collider + stats sink**

In `extension/src/voxel_world.h`:

- change `class VoxelWorld : public Node3D, public EditSink {` to `class VoxelWorld : public Node3D, public ve::InvalidationSink {`;
- replace the `on_edit_appended` declaration with:

```cpp
	// InvalidationSink: the collider remesh queue (kEdit) and the rejection stats
	// (kRejected). Edit lock held; queue only (core/edit_pipeline.h).
	void record(const ve::Invalidation &inv) override;
```

In `extension/src/voxel_world.cpp` (`ve::op_chunk_range`, `ve::chunk_of_brick` and `ve::kRegionBricks` all come from headers it already includes):

- replace `VoxelWorld::on_edit_appended` with:

```cpp
void VoxelWorld::record(const ve::Invalidation &inv) {
	switch (inv.reason) {
	case ve::InvalidationReason::kRejected:
		if (inv.append->oversized)
			UtilityFunctions::printerr("VoxelWorld: edit op exceeds the bounded region span — spec §8 fail-soft");
		if (!inv.append->rejected.empty()) {
			stats_.edit_rejections += static_cast<int>(inv.append->rejected.size());
			UtilityFunctions::printerr("VoxelWorld: region op list full, op rejected (",
					inv.append->rejected[0].x, ", ", inv.append->rejected[0].y, ", ",
					inv.append->rejected[0].z, ") — spec §8 fail-soft");
		}
		return;
	case ve::InvalidationReason::kEdit: {
		// Collision's half of the fan-out (spec §5). Queued rather than applied, because this
		// may run on any thread that owns a tool while ChunkResidency belongs to the main
		// one; physics_tick drains it. Queued even when physics is off, so enabling it later
		// starts consistent.
		ve::IVec3 clo{}, chi{};
		ve::op_chunk_range(*inv.op, &clo, &chi);
		pending_dirty_.push_back({clo, chi});
		return;
	}
	case ve::InvalidationReason::kConsolidated: {
		const ve::IVec3 base{inv.region.x * ve::kRegionBricks, inv.region.y * ve::kRegionBricks,
				inv.region.z * ve::kRegionBricks};
		pending_dirty_.push_back({ve::chunk_of_brick(base),
				ve::chunk_of_brick({base.x + ve::kRegionBricks - 1, base.y + ve::kRegionBricks - 1,
						base.z + ve::kRegionBricks - 1})});
		return;
	}
	}
}
```

- replace `VoxelWorld::append_edit`'s body with:

```cpp
ve::EditLog::AppendResult VoxelWorld::append_edit(const ve::EditOp &op) {
	std::lock_guard<std::mutex> lock(store_->edit_mutex());
	return store_->edits().apply(std::span<const ve::EditOp>(&op, 1), {}).ops[0];
}
```

- replace `VoxelWorld::append_edit_locked`'s body with (it survives only until Task 8, as the island manager's handle):

```cpp
ve::EditLog::AppendResult VoxelWorld::append_edit_locked(const ve::EditOp &op,
		bool notify_islands) {
	// Named debt: IslandManager's handle. Task 8 has it call the pipeline directly.
	return store_->edits()
			.apply(std::span<const ve::EditOp>(&op, 1),
					{.atomic = false, .notify_islands = notify_islands})
			.ops[0];
}
```

- replace `store_->set_sinks(this, consolidation_.get());` (and its comment block) with:

```cpp
	// The fan-out (core/edit_pipeline.h). These three live as long as the world does; the
	// island manager registers and unregisters with physics. Registered under the edit lock,
	// holding no other lock.
	{
		std::lock_guard<std::mutex> lock(store_->edit_mutex());
		store_->edits().add_sink(this);                 // collider remesh queue + rejection stats
		store_->edits().add_sink(lod_.get());           // LoD dirty marks
		store_->edits().add_sink(consolidation_.get()); // the consolidation queue
	}
```

- add `#include <span>` to the includes if it is not already there.

- [ ] **Step 3: Make LodSystem a sink**

In `extension/src/lod/lod_system.h`:

- add `#include "core/edit_pipeline.h"` next to the other includes;
- change `class LodSystem {` to `class LodSystem : public ve::InvalidationSink {`;
- replace the `note_edit` declaration and its comment with:

```cpp
	// InvalidationSink: an edit or a consolidation marks the tree dirty. Edit lock held
	// (core/edit_pipeline.h). Task 11 makes this a queue-and-drain.
	void record(const ve::Invalidation &inv) override;
```

In `extension/src/lod/lod_system.cpp`, replace `LodSystem::note_edit` with:

```cpp
// The LoD half of the edit fan-out. Caller holds edit_mutex(); the lod_mutex_ acquisition
// site travels with the code (Task 11 removes it).
void LodSystem::record(const ve::Invalidation &inv) {
	if (inv.reason == ve::InvalidationReason::kRejected) return;
	if (!lod_tree_) return;
	// Every level: ve::LodTree::mark_dirty walks them itself, and the relevance cut is
	// at the HALF-CELL supersample resolution rather than the cell -- a 5 m crater still
	// registers at L4's 6.4 m cells, which is the point of the reduction change. Only
	// ops shorter than half a cell on every axis are genuinely unrepresentable.
	std::lock_guard<std::mutex> lock(lod_mutex_);
	lod_tree_->mark_dirty(inv.lo, inv.hi);
}
```

- [ ] **Step 4: Make IslandManager a sink**

In `extension/src/physics/island_manager.h`:

- add `#include "core/edit_pipeline.h"` next to the other includes;
- change `class IslandManager {` to `class IslandManager : public ve::InvalidationSink {`;
- move the `note_edit` declaration into the private section (keep its comment) and add to the public section:

```cpp
	// InvalidationSink: an accepted edit queues a connectivity window. Edit lock held
	// (core/edit_pipeline.h). Task 12 makes this a queue-and-drain.
	void record(const ve::Invalidation &inv) override;
```

In `extension/src/physics/island_manager.cpp`, add above `IslandManager::note_edit`:

```cpp
void IslandManager::record(const ve::Invalidation &inv) {
	if (inv.reason != ve::InvalidationReason::kEdit || !inv.notify_islands) return;
	note_edit(*inv.op, inv.seq);
}
```

- [ ] **Step 5: Make ConsolidationCoordinator a sink**

In `extension/src/mesh/consolidation.h`:

- change `class ConsolidationCoordinator : public ConsolidationSink { // satisfies the port from Task 8` to `class ConsolidationCoordinator : public ve::InvalidationSink {`;
- add to the public section:

```cpp
	// InvalidationSink: an accepted edit queues the region once its list nears the cap. Edit
	// lock held (core/edit_pipeline.h).
	void record(const ve::Invalidation &inv) override;
```

- change the `queue_consolidation` declaration in the private section (it no longer overrides anything):

```cpp
	// edit_mutex must be held.
	bool queue_consolidation(ve::IVec3 region);
```

In `extension/src/mesh/consolidation.cpp`, add above `ConsolidationCoordinator::queue_consolidation`:

```cpp
void ConsolidationCoordinator::record(const ve::Invalidation &inv) {
	if (inv.reason != ve::InvalidationReason::kEdit) return;
	// Queue before the list reaches its hard cap. The bake is asynchronous, so the spare 64
	// entries absorb edits appended while the worker is in flight.
	for (const ve::IVec3 &region : inv.append->touched)
		if (store_->edit_log()->op_count(region) >= ve::kConsolidateAtOps)
			queue_consolidation(region);
}
```

- [ ] **Step 6: Register the island manager with physics**

In `extension/src/voxel_world.cpp`, in `ensure_physics_initialized`, inside the existing `std::lock_guard<std::mutex> lock(store_->edit_mutex());` block, after `island_manager_->set_generator(...)`:

```cpp
		// The edit lock is already held here, which is where a sink must be registered.
		store_->edits().add_sink(island_manager_);
```

and in `teardown_physics`, right after `IslandManager *manager = island_manager_;` / `island_manager_ = nullptr;`:

```cpp
	// The same hold that detaches the pointer unregisters the sink, so no edit can reach a
	// manager that is being torn down.
	store_->edits().remove_sink(manager);
```

- [ ] **Step 7: Build and run everything that watches the fan-out**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_edit_fanout.gd,res://tests/test_connectivity.gd,res://tests/test_consolidation.gd,res://tests/test_world_store_contract.gd,res://tests/test_lod_stream.gd
```

Expected: native PASS; every fan-out pin unchanged; the other suites match the baseline. A moved pin means the move was not verbatim — stop and report.

- [ ] **Step 8: Commit**

```bash
git add extension/src
git commit -m "refactor: one edit spine, every consumer a sink"
```

---

### Task 7: Consolidation invalidates instead of repeating the fan-out

**Files:**
- Modify: `extension/src/mesh/consolidation.h`, `consolidation.cpp`
- Modify: `extension/src/voxel_world.h`, `voxel_world.cpp`
- Modify: `extension/src/lod/lod_system.h`
- Modify: `extension/src/debug/hooks_world.cpp`

**Interfaces:**
- Consumes: `ve::Invalidation::consolidated`, `ve::merge_or_cap`, `ve::Box3`.
- Produces: `pending_dirty_` is `std::vector<ve::Box3<int>>`; `ConsolidationCoordinator::Collaborators` loses `lod_tree`, `lod_mutex` and `pending_dirty`; `LodSystem::tree_slot()` and `mutex_slot()` are gone.

- [ ] **Step 1: Replace the async commit's fan-out**

In `extension/src/mesh/consolidation.cpp`, in `pump_async`'s publish branch, replace:

```cpp
			pending_dirty().push_back({ve::chunk_of_brick(base),
					ve::chunk_of_brick({base.x + ve::kRegionBricks - 1,
							base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1})});
			if (store_->edit_log()->op_count(r) >= ve::kConsolidateAtOps) queue_consolidation(r);
			float lo[3], first_hi[3], last_lo[3], hi[3];
			ve::brick_world_aabb(base, lo, first_hi);
			ve::brick_world_aabb({base.x + ve::kRegionBricks - 1,
					base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1}, last_lo, hi);
			if (lod_tree()) {
				std::lock_guard<std::mutex> lod_lock(lod_mutex());
				lod_tree()->mark_dirty(lo, hi);
			}
			if (streamer()) streamer()->queue_region_regeneration_locked(r);
```

with:

```cpp
			// Colliders, LoD and the streamer hear the same region through the one fan-out
			// (core/edit_pipeline.h); this used to be three hand-written calls, twice.
			store_->edits().invalidate(ve::Invalidation::consolidated(r));
			if (store_->edit_log()->op_count(r) >= ve::kConsolidateAtOps) queue_consolidation(r);
```

- [ ] **Step 2: Replace the forced commit's fan-out**

In the same file, in `force_region`, replace:

```cpp
	const ve::IVec3 hi_brick{base.x + ve::kRegionBricks - 1,
			base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1};
	pending_dirty().push_back({ve::chunk_of_brick(base), ve::chunk_of_brick(hi_brick)});
	float lo[3], first_hi[3], last_lo[3], hi[3];
	ve::brick_world_aabb(base, lo, first_hi);
	ve::brick_world_aabb({base.x + ve::kRegionBricks - 1, base.y + ve::kRegionBricks - 1,
			base.z + ve::kRegionBricks - 1}, last_lo, hi);
	if (lod_tree()) {
		std::lock_guard<std::mutex> lock(lod_mutex());
		lod_tree()->mark_dirty(lo, hi);
	}
	if (streamer()) streamer()->queue_region_regeneration_locked(r);
```

with:

```cpp
	store_->edits().invalidate(ve::Invalidation::consolidated(r));
```

If the compiler now reports `base` (or any other local) as unused in either commit, delete its declaration.

- [ ] **Step 3: Let the coordinator queue the regeneration**

In `extension/src/mesh/consolidation.cpp`, extend `record`:

```cpp
void ConsolidationCoordinator::record(const ve::Invalidation &inv) {
	if (inv.reason == ve::InvalidationReason::kConsolidated) {
		// The base bytes changed without an op, so the render atlas must not keep pre-bake
		// data. The streamer is created and destroyed with the render graph, which is why
		// this stays behind the coordinator's slot instead of the streamer registering a
		// sink of its own (plan decision 2).
		if (streamer()) streamer()->queue_region_regeneration_locked(inv.region);
		return;
	}
	if (inv.reason != ve::InvalidationReason::kEdit) return;
	// Queue before the list reaches its hard cap. The bake is asynchronous, so the spare 64
	// entries absorb edits appended while the worker is in flight.
	for (const ve::IVec3 &region : inv.append->touched)
		if (store_->edit_log()->op_count(region) >= ve::kConsolidateAtOps)
			queue_consolidation(region);
}
```

- [ ] **Step 4: Delete the handles the commits used**

In `extension/src/mesh/consolidation.h`: delete the `lod_tree`, `lod_mutex` and `pending_dirty` fields from `Collaborators`, the private `lod_tree()`, `lod_mutex()` and `pending_dirty()` accessors, and the `namespace ve { class LodTree; }` forward declaration if nothing else in the header uses it. In `consolidation.cpp`, delete `#include "lod/lod_tree.h"` if nothing else in the file uses it.

In `extension/src/voxel_world.cpp`, delete these three initialisers from the `ConsolidationCoordinator::Collaborators{...}` block (and the `// Task 15: tree/mutex handles now address LodSystem's state.` comment above them):

```cpp
					.lod_tree = context_.lod->tree_slot(),
					.lod_mutex = context_.lod->mutex_slot(),
					.pending_dirty = &pending_dirty_,
```

In `extension/src/lod/lod_system.h`, delete `mutex_slot()` and `tree_slot()` together with their comment block.

- [ ] **Step 5: Bound the collider queue**

In `extension/src/voxel_world.h`, change the member:

```cpp
	// Collider remesh queue; guarded by edit_mutex, drained by physics_tick. Bounded by
	// ve::merge_or_cap (core/edit_pipeline.h): overlapping ranges merge, so a thousand edits
	// in one place stay one entry.
	std::vector<ve::Box3<int>> pending_dirty_;
```

In `extension/src/voxel_world.cpp`, in `record`, replace the two `pending_dirty_.push_back(...)` calls:

```cpp
		ve::IVec3 clo{}, chi{};
		ve::op_chunk_range(*inv.op, &clo, &chi);
		ve::merge_or_cap(&pending_dirty_, ve::Box3<int>{{clo.x, clo.y, clo.z}, {chi.x, chi.y, chi.z}});
```

```cpp
		const ve::IVec3 lo = ve::chunk_of_brick(base);
		const ve::IVec3 hi = ve::chunk_of_brick({base.x + ve::kRegionBricks - 1,
				base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1});
		ve::merge_or_cap(&pending_dirty_, ve::Box3<int>{{lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}});
```

and in `physics_tick`, change the drain:

```cpp
	std::vector<ve::Box3<int>> dirty;
	{
		std::lock_guard<std::mutex> lock(store_->edit_mutex());
		dirty.swap(pending_dirty_);
	}
	for (const ve::Box3<int> &r : dirty)
		chunks_->mark_dirty({r.lo[0], r.lo[1], r.lo[2]}, {r.hi[0], r.hi[1], r.hi[2]});
```

In `extension/src/debug/hooks_world.cpp`, update `debug_edit_fanout`'s expansion loop:

```cpp
	for (const ve::Box3<int> &range : world_->pending_dirty())
		for (int z = range.lo[2]; z <= range.hi[2]; z++)
			for (int y = range.lo[1]; y <= range.hi[1]; y++)
				for (int x = range.lo[0]; x <= range.hi[0]; x++)
					chunks.insert(Key{x, y, z});
```

and in `voxel_world.h` update the accessor's type:

```cpp
	const std::vector<ve::Box3<int>> &pending_dirty() const { return pending_dirty_; }
```

- [ ] **Step 6: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_edit_fanout.gd,res://tests/test_consolidation.gd,res://tests/test_connectivity.gd,res://tests/test_lod_stream.gd
```

Expected: every fan-out pin unchanged (the collider rows are expanded sets, so merging cannot move them), the suites match the baseline.

- [ ] **Step 7: Commit**

```bash
git add extension/src
git commit -m "refactor: a consolidation commit invalidates its region"
```

---

### Task 8: The island manager applies atomic batches

**Files:**
- Modify: `extension/src/physics/island_manager.h`, `island_manager.cpp`
- Modify: `extension/src/voxel_world.h`, `voxel_world.cpp`

**Interfaces:**
- Consumes: `EditPipeline::preflight`, `EditPipeline::apply`.
- Produces: `IslandManager::Collaborators` without `append_edit_locked`; `VoxelWorld::append_edit_locked` deleted.

- [ ] **Step 1: The crumble carve becomes one atomic batch**

In `extension/src/physics/island_manager.cpp`, in `crumble_component`, replace the headroom block (from the comment `// Headroom region by region before the first append:` through the `for (const ve::CellBox &box : f.boxes) handles_.append_edit_locked(...)` loop) with:

```cpp
	// One atomic batch: a half-applied carve would leave half the sheet standing and spend
	// the ops anyway. notify_islands = false: this matter was already labelled unanchored, so
	// removing it cannot loosen anything that was not loose already, and a window per crumble
	// would put the connectivity pass back into the loop this function exists to break.
	std::vector<ve::EditOp> carve;
	carve.reserve(f.boxes.size());
	for (const ve::CellBox &box : f.boxes)
		carve.push_back(ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM));
	if (handles_.store->edits().apply(carve, {.atomic = true, .notify_islands = false}).refused)
		return false;
```

- [ ] **Step 2: The landing carve becomes one atomic batch**

In `land_extraction`, replace the carve loop:

```cpp
		bool carve_rejected = false;
		std::vector<ve::IVec3> carved_regions;
		for (const ve::CellBox &box : f.boxes) {
			const ve::EditLog::AppendResult carve = handles_.append_edit_locked(
					ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM), true);
			for (const ve::IVec3 &region : carve.touched) carved_regions.push_back(region);
			if (debug_fail_next_carve_) {
				debug_fail_next_carve_ = false;
				carve_rejected = true;
				break;
			}
			if (!carve.rejected.empty()) {
				carve_rejected = true;
				break;
			}
		}
		debug_fail_next_carve_ = false;
```

with:

```cpp
		bool carve_rejected = false;
		std::vector<ve::IVec3> carved_regions;
		std::vector<ve::EditOp> carve_ops;
		carve_ops.reserve(f.boxes.size());
		for (const ve::CellBox &box : f.boxes)
			carve_ops.push_back(ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM));
		const ve::BatchResult carve = handles_.store->edits().apply(carve_ops, {.atomic = true});
		for (const ve::EditLog::AppendResult &r : carve.ops)
			for (const ve::IVec3 &region : r.touched) carved_regions.push_back(region);
		if (carve.refused) carve_rejected = true;
		if (debug_fail_next_carve_) carve_rejected = true;
		debug_fail_next_carve_ = false;
```

and, further down in the same block, replace the restore append:

```cpp
			const ve::EditLog::AppendResult restore =
					handles_.append_edit_locked(ve::make_volume_add(f.volume_slot, f.origin,
							f.voxel, f.dim), true);
```

with:

```cpp
			const ve::EditOp restore_op =
					ve::make_volume_add(f.volume_slot, f.origin, f.voxel, f.dim);
			const ve::EditLog::AppendResult restore =
					handles_.store->edits().apply(std::span<const ve::EditOp>(&restore_op, 1), {}).ops[0];
```

- [ ] **Step 3: The re-merge paste preflights through the pipeline**

In `land_resample`, replace:

```cpp
		ve::EditLog::AppendResult preflight;
		for (const ve::IVec3 &region : paste_regions)
			if (handles_.store->edit_log()->op_count(region) >= ve::kMaxRegionOps)
				preflight.rejected.push_back(region);
		if (!preflight.rejected.empty()) {
```

with:

```cpp
		// Asked BEFORE the store/pin/upload below, so a refusal costs nothing to unwind; the
		// apply that follows runs under this same hold and therefore cannot fail.
		ve::EditLog::AppendResult preflight;
		if (!handles_.store->edits().preflight(std::span<const ve::EditOp>(&r.op, 1),
					&preflight.rejected)) {
```

and replace the paste append:

```cpp
		const ve::EditLog::AppendResult paste = handles_.append_edit_locked(r.op, true);
```

with:

```cpp
		const ve::EditLog::AppendResult paste =
				handles_.store->edits().apply(std::span<const ve::EditOp>(&r.op, 1), {.atomic = true}).ops[0];
```

Add `#include <span>` to `island_manager.cpp` if it is not already there.

- [ ] **Step 4: Delete the handle**

In `extension/src/physics/island_manager.h`, delete the `append_edit_locked` field from `Collaborators` (and its comment), and `#include <functional>` if nothing else in the header uses it.

In `extension/src/voxel_world.cpp`, delete the initialiser from the `IslandManager::Collaborators{...}` block:

```cpp
				.append_edit_locked = [this](const ve::EditOp &op, bool notify_islands) {
					return append_edit_locked(op, notify_islands);
				},
```

and delete `VoxelWorld::append_edit_locked` entirely; delete its declaration and comment from `voxel_world.h`.

- [ ] **Step 5: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_island_body.gd,res://tests/test_edit_fanout.gd
```

Expected: the fan-out pins unchanged; `test_connectivity` and `test_island_body` within their baseline failure counts (the flaky-by-case rule). `test_near_cap_carve_is_refused_before_any_carve` still passes here: `has_restore_headroom` is untouched by this task.

- [ ] **Step 6: Commit**

```bash
git add extension/src
git commit -m "refactor: island carves, crumbles and pastes are atomic batches"
```

---

### Task 9: Delete the unreachable restore branch

An atomic carve cannot half-apply, so the "carve rejected after some boxes landed, restore the field" branch and its fail-injection toggles go.

**Files:**
- Modify: `extension/src/physics/island_manager.h`, `island_manager.cpp`
- Modify: `extension/src/debug/hooks.h`, `hooks.cpp`, `hooks_physics.cpp`
- Modify: `tests/test_connectivity.gd`

**Interfaces:**
- Consumes: `EditPipeline::preflight` (Task 8).
- Produces: `land_extraction` without the restore path; `debug_set_fail_next_carve` / `debug_set_fail_next_restore` gone.

- [ ] **Step 1: Replace the preflight with the pipeline's**

In `extension/src/physics/island_manager.cpp`, in `land_extraction`, delete the whole `const auto has_restore_headroom = [&]() -> bool { ... };` lambda (it exists only to reserve room for the restore volume-add) and, just above `IslandBody *b = nullptr;`, add:

```cpp
	// The carve, as one atomic batch. Asked before anything is pinned or spawned.
	std::vector<ve::EditOp> carve_ops;
	carve_ops.reserve(f.boxes.size());
	for (const ve::CellBox &box : f.boxes)
		carve_ops.push_back(ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM));
```

Then replace the preflight call inside the lock:

```cpp
		if (!has_restore_headroom()) {
```

with:

```cpp
		// Refuse before any side effect: the apply below runs under this same hold, so a
		// batch that fits here is accepted whole, and one that does not leaves the component
		// attached — the safe fail-soft direction.
		if (!handles_.store->edits().preflight(carve_ops, nullptr)) {
```

- [ ] **Step 2: Delete the restore branch**

In the same block, replace everything from:

```cpp
		// 2. Carve (spec §5 step 1). The boxes tile the component exactly, so this removes the
```

through the end of the `} else { ... }` that holds the restore logic (ending just before the closing brace of the `std::lock_guard` scope and the comment `// The edit lock is released only after the carve either has a live body in the hole or`) with:

```cpp
		// 2. Carve (spec §5 step 1). The boxes tile the component exactly, so this removes the
		//    material that just became a body and nothing else. Ordered after the spawn so a
		//    live body is already in place before any rock is removed from the field. The
		//    batch is atomic and its preflight ran under this same hold, so it is accepted
		//    whole: there is no half-carved field to restore, and no branch for one.
		handles_.store->edits().apply(carve_ops, {.atomic = true});
		// Tell the occupancy grid straight away; the GPU readback that would say the same
		// thing is several frames out, and until it lands the next connectivity run would
		// find this component all over again and carve it twice.
		for (const ve::CellBox &box : f.boxes)
			for (int z = box.lo.z; z <= box.hi.z; z++)
				for (int y = box.lo.y; y <= box.hi.y; y++)
					for (int x = box.lo.x; x <= box.hi.x; x++)
						handles_.store->occupancy().set_cell(
								{x, y, z}, ve::kCellAir, handles_.store->edit_seq());
#ifdef DEBUG_ENABLED
		for (const ve::CellBox &box : f.boxes) debug_carved_boxes_.push_back(box);
#endif
		handles_.store->volumes().unpin(f.volume_slot);
		b = body;
```

Also delete, from the same function, the now-unused `restore_referenced_slot` variable and any `if (!restore_referenced_slot)` guard left behind, keeping `release_unreferenced_birth_slot` (the spawn-failure path still uses it).

- [ ] **Step 3: Delete the toggles and their counters**

In `extension/src/physics/island_manager.h`: delete `debug_set_fail_next_restore` and `debug_set_fail_next_carve` (both the `DEBUG_ENABLED` declarations with their comments and the `#else` stubs), the members `debug_fail_next_restore_` and `debug_fail_next_carve_`, and the `carve_nothing` and `carve_restored` fields of `debug_land_`.

In `island_manager.cpp`: delete the `d["land_carve_nothing"]` and `d["land_carve_restored"]` stats lines and any remaining `DBG_LAND(carve_nothing)` / `DBG_LAND(carve_restored)` uses.

In `extension/src/debug/hooks.h`, `hooks.cpp` and `hooks_physics.cpp`: delete the `debug_set_fail_next_restore` and `debug_set_fail_next_carve` declarations, bindings and bodies.

- [ ] **Step 4: Delete the two cases that exercised the branch**

In `tests/test_connectivity.gd`, delete `test_post_spawn_carve_rejection_keeps_body_in_hole` (with its comment block) and `test_near_cap_carve_is_refused_before_any_carve` (with its comment block). Add, above `test_rejected_carve_keeps_component_attached`:

```gdscript
# The two cases that used to sit here are gone with the restore branch they exercised
# (docs/superpowers/plans/2026-09-17-edit-pipeline.md, Task 9): an atomic carve is accepted
# whole or not at all, so there is no post-spawn rejection to restore from, and the near-cap
# case's 255-op premise reserved room for a restore volume-add that no longer exists. The cap
# refusal itself is covered below and in extension/tests/test_edit_pipeline.cpp.
```

- [ ] **Step 5: Check nothing else referenced them**

```bash
rg 'fail_next_carve|fail_next_restore|land_carve_restored|land_carve_nothing' extension demo tests
```

Expected: no output.

- [ ] **Step 6: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_island_body.gd,res://tests/test_edit_fanout.gd
```

Expected: the fan-out pins unchanged; `test_connectivity` has two fewer declared cases (this task's deletion, recorded in the results report) and otherwise matches the baseline.

- [ ] **Step 7: Commit**

```bash
git add extension/src tests/test_connectivity.gd
git commit -m "refactor: an atomic carve has no half-carved field to restore"
```

---

### Task 10: Staleness by append sequence

**This is the one task allowed to move a pin.** Its commit message names the cause.

**Files:**
- Modify: `extension/src/world/edit_log.h`
- Modify: `extension/src/world/world_field.h`, `world_field.cpp`
- Modify: `extension/src/physics/island_manager.h`, `island_manager.cpp`
- Modify: `extension/tests/test_edit_log.cpp`
- Modify: `tests/test_connectivity.gd`

**Interfaces:**
- Consumes: `FieldView::snapshot_lattice`.
- Produces: `EditLog::last_seq()`, `collect_ops_for_aabb(..., uint64_t after_seq = 0)`, `FieldSnapshot::log_seq`, `FieldView::ops_since(lo, hi, after_seq, out)`, `InFlight::log_seq` (replacing `InFlight::ops`).

- [ ] **Step 1: Write the failing native test**

Append to `extension/tests/test_edit_log.cpp`:

```cpp
TEST_CASE("collect_ops_for_aabb can skip everything appended at or before a sequence") {
	ve::EditLog log;
	log.append(sphere(12.8f, 12.8f, 12.8f, 1.0f));
	const uint64_t mark = log.last_seq();
	log.append(sphere(13.0f, 12.8f, 12.8f, 1.0f));
	const float lo[3] = {11.0f, 11.0f, 11.0f};
	const float hi[3] = {15.0f, 15.0f, 15.0f};
	std::vector<ve::EditOp> all, newer;
	ve::collect_ops_for_aabb(log, lo, hi, &all);
	ve::collect_ops_for_aabb(log, lo, hi, &newer, mark);
	CHECK(all.size() == 2);
	REQUIRE(newer.size() == 1);
	CHECK(newer[0].pos[0] == doctest::Approx(13.0f));
	// Nothing newer than the last append.
	std::vector<ve::EditOp> none;
	ve::collect_ops_for_aabb(log, lo, hi, &none, log.last_seq());
	CHECK(none.empty());
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cd extension && scons -Q test; cd ..`
Expected: FAIL — `last_seq` does not exist and `collect_ops_for_aabb` takes no sequence.

- [ ] **Step 3: Add the sequence filter**

In `extension/src/world/edit_log.h`, next to `int region_count() const`:

```cpp
	// The sequence of the most recent append. A consumer that captured a snapshot can ask
	// later whether anything has been appended since (collect_ops_for_aabb's after_seq).
	uint64_t last_seq() const { return next_seq_ - 1; }
```

Change `collect_ops_for_aabb`'s signature:

```cpp
inline void collect_ops_for_aabb(const EditLog &log, const float lo[3], const float hi[3],
		std::vector<EditOp> *out, uint64_t after_seq = 0) {
```

and its inner filter (the `for (size_t i = 0; i < n; i++)` body):

```cpp
				for (size_t i = 0; i < n; i++) {
					if (seqs[i] <= after_seq) continue; // older than the caller's snapshot
					if (!intersects(ops[i])) continue;
					found.push_back({ops[i], seqs[i]});
				}
```

Extend the comment block above `collect_ops_for_aabb` with:

```
// `after_seq` keeps only ops appended after that sequence, which is how a consumer holding a
// snapshot asks "has anything changed since?" without re-reading and comparing the whole list.
```

- [ ] **Step 4: Run the native test**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS.

- [ ] **Step 5: Carry the sequence on the snapshot**

In `extension/src/world/world_field.h`, add to `FieldSnapshot`:

```cpp
	uint64_t log_seq = 0; // EditLog::last_seq() when these ops were read
```

and declare on `FieldView`, next to `snapshot_lattice`:

```cpp
	// Ops appended after `after_seq` that can influence [lo, hi]. The cheap form of "has the
	// field moved since my snapshot?".
	void ops_since(const float lo[3], const float hi[3], uint64_t after_seq,
			std::vector<EditOp> *out) const;
```

In `world_field.cpp`, set the stamp inside `snapshot_lattice`, right after the `collect_ops_for_aabb` call:

```cpp
	out->log_seq = log_->last_seq();
```

and add:

```cpp
void FieldView::ops_since(const float lo[3], const float hi[3], uint64_t after_seq,
		std::vector<EditOp> *out) const {
	if (!out) return;
	out->clear();
	if (!valid()) return;
	collect_ops_for_aabb(*log_, lo, hi, out, after_seq);
}
```

- [ ] **Step 6: Use it in the landing check**

In `extension/src/physics/island_manager.h`, replace `InFlight`'s captured ops:

```cpp
		// The log sequence the ops were captured at, and the world AABB they were collected
		// from. land_extraction asks whether anything newer reaches the boxes; if so, the
		// extraction is stale and must not be carved.
		uint64_t log_seq = 0;
```

(delete `std::vector<ve::EditOp> ops;`, keep `aabb_lo` / `aabb_hi`).

In `island_manager.cpp`, in `run_connectivity`, replace `f.ops = job.ops;` with `f.log_seq = snap.log_seq;`.

In `land_extraction`, replace the staleness computation:

```cpp
			std::vector<ve::EditOp> current_ops;
			ve::collect_ops_for_aabb(*handles_.store->edit_log(), f.aabb_lo, f.aabb_hi, &current_ops);
			std::vector<ve::EditOp> now, then;
			for (const ve::EditOp &op : current_ops)
				if (reaches_the_boxes(op)) now.push_back(op);
			for (const ve::EditOp &op : f.ops)
				if (reaches_the_boxes(op)) then.push_back(op);
			const bool stale = now.size() != then.size() ||
					!std::equal(now.begin(), now.end(), then.begin(),
							[](const ve::EditOp &a, const ve::EditOp &b) {
								return std::memcmp(&a, &b, sizeof(ve::EditOp)) == 0;
							});
```

with:

```cpp
			// An op counts when it was appended AFTER the snapshot this extraction was
			// computed from. Comparing the captured list against the current one also
			// reported a consolidation as staleness -- a bake removes ops without changing a
			// single field value, so it made extractions retry for nothing.
			std::vector<ve::EditOp> newer;
			handles_.store->field().locked_by_caller().ops_since(f.aabb_lo, f.aabb_hi, f.log_seq,
					&newer);
			const bool stale = std::any_of(newer.begin(), newer.end(), reaches_the_boxes);
```

- [ ] **Step 7: Move the pin**

In `tests/test_connectivity.gd`, rewrite the case from Task 3 (rename it, keep its position):

```gdscript
# Was test_a_consolidation_during_an_extraction_is_pinned. The staleness check compares append
# sequences now instead of op lists (docs/superpowers/plans/2026-09-17-edit-pipeline.md, Task
# 10), and a consolidation appends nothing: it turns ops into override bricks that evaluate to
# the same field. An extraction in flight across a bake is therefore no longer stale, and the
# component it freed becomes a body instead of being thrown away and relabelled.
func test_a_consolidation_during_an_extraction_does_not_make_it_stale(timeout := 180000) -> void:
	var w := make_world(false)
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	var st: Dictionary = w.hooks().debug_island_stats()
	for i in range(120):
		await get_tree().physics_frame
		step(w, 1)
		st = w.hooks().debug_island_stats()
		if st["in_flight"] > 0:
			break
	assert_int(st["in_flight"]).override_failure_message(
		"the connectivity pass did not submit an extraction: %s" % st).is_greater(0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).override_failure_message(
		"the pillar's region did not consolidate; the fixture is wrong, not the code").is_true()
	var stale_before: int = st["land_stale"]
	for i in range(240):
		await get_tree().physics_frame
		step(w, 1)
		st = w.hooks().debug_island_stats()
		if st["islands_spawned"] + st["debris_spawned"] > 0 or st["land_stale"] > stale_before:
			break
	assert_int(st["land_stale"]).override_failure_message(
		"a consolidation still reads as a stale field: %s" % st).is_equal(stale_before)
	assert_int(st["islands_spawned"] + st["debris_spawned"]).override_failure_message(
		"the extraction did not land after the bake: %s" % st).is_greater(0)
```

- [ ] **Step 8: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_island_body.gd,res://tests/test_edit_fanout.gd
```

Expected: native PASS; the rewritten case PASSES; `test_stale_extraction_is_refused_before_any_carve` still PASSES (a real edit inside the boxes is still newer than the snapshot); the fan-out pins unchanged.

- [ ] **Step 9: Commit**

```bash
git add extension/src extension/tests/test_edit_log.cpp tests/test_connectivity.gd
git commit -m "fix: a consolidation no longer makes an in-flight extraction stale

The landing check compared the captured op list with the current one, so a bake
that turned those ops into override bricks read as a changed field and the
extraction was thrown away. It now compares append sequences: a bake appends
nothing and changes no field value. test_connectivity's pin moves with it."
```

---

### Task 11: LoD marks become queue-and-drain

Removes the edit → LoD lock edge.

**Files:**
- Modify: `extension/src/lod/lod_system.h`, `lod_system.cpp`
- Modify: `extension/src/debug/hooks_world.cpp`
- Modify: `tests/test_lod_stream.gd`

**Interfaces:**
- Consumes: `ve::merge_or_cap`, `ve::Box3<float>`.
- Produces: `LodSystem::drain_invalidations()`, called by `tick()` and by `debug_drain_invalidations()`.

- [ ] **Step 1: Queue instead of marking**

In `extension/src/lod/lod_system.h`, add next to `record`:

```cpp
	// Swap the queued marks out under the edit lock, then apply them under mutex(). Called at
	// the top of tick() and by the debug drain. Takes the edit lock: never call it while
	// holding mutex() or the edit lock.
	void drain_invalidations();
```

and as a private member next to `lod_op_overflow_`:

```cpp
	// Marks queued by record(), guarded by WorldStore::edit_mutex(); drained by
	// drain_invalidations(). Bounded by ve::merge_or_cap.
	std::vector<ve::Box3<float>> pending_marks_;
```

In `lod_system.cpp`, replace `record`'s body and add the drain:

```cpp
// The LoD half of the edit fan-out. Edit lock held: queue only, never the lod mutex
// (core/edit_pipeline.h).
void LodSystem::record(const ve::Invalidation &inv) {
	if (inv.reason == ve::InvalidationReason::kRejected) return;
	ve::merge_or_cap(&pending_marks_,
			ve::Box3<float>{{inv.lo[0], inv.lo[1], inv.lo[2]}, {inv.hi[0], inv.hi[1], inv.hi[2]}});
}

void LodSystem::drain_invalidations() {
	std::vector<ve::Box3<float>> marks;
	{
		std::lock_guard<std::mutex> edit_lock(store()->edit_mutex());
		marks.swap(pending_marks_);
	}
	if (marks.empty()) return;
	std::lock_guard<std::mutex> lock(lod_mutex_);
	// No tree yet: the marks predate it and the tree this tick builds reads the current world
	// anyway, exactly as the synchronous mark's `if (!lod_tree_) return` dropped them. After a
	// release_gpu the tree is cleared, so a drained mark finds no node and marks nothing --
	// which is why release_gpu needs no queue clearing and keeps taking no lock.
	if (!lod_tree_) return;
	// Every level: ve::LodTree::mark_dirty walks them itself, and the relevance cut is at the
	// HALF-CELL supersample resolution rather than the cell.
	for (const ve::Box3<float> &m : marks) lod_tree_->mark_dirty(m.lo, m.hi);
}
```

In `LodSystem::tick`, make the first statement (before `std::unique_lock<std::mutex> lock(lod_mutex_);`):

```cpp
	// Edits queued since the last tick, applied before the walk decides what to build. Takes
	// the edit lock and must therefore run before this tick takes lod_mutex_.
	drain_invalidations();
```

- [ ] **Step 2: Drain from the hook**

In `extension/src/debug/hooks_world.cpp`, fill in `debug_drain_invalidations`:

```cpp
void VoxelDebugHooks::debug_drain_invalidations() {
	// Each deferred consumer's own drain -- the function its tick calls, never a copy.
	if (world_->context().lod) world_->context().lod->drain_invalidations();
}
```

Add `#include "lod/lod_system.h"` if it is not already there.

- [ ] **Step 3: Watch test_lod_stream fail, then fix its read**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_lod_stream.gd
```

Expected: `test_an_edit_rebuilds_every_level_it_touches` FAILS — it reads `dirty_chunks` straight after the edit, which is now queued.

Then in `tests/test_lod_stream.gd`, insert before `var dirty := w.hooks().debug_lod_stats()`:

```gdscript
	# The LoD mark is queued under the edit lock and applied by the next tick's drain
	# (core/edit_pipeline.h). Drain it here rather than ticking: a tick would clear the dirty
	# flags this case is about to read, because note_building clears them at submission.
	w.hooks().debug_drain_invalidations()
```

- [ ] **Step 4: Run**

```bash
./gdunit_tests.sh -a res://tests/test_lod_stream.gd,res://tests/test_edit_fanout.gd,res://tests/test_connectivity.gd
```

Expected: all PASS; `LOD_GOLDEN` unchanged (the pin already reads through the drain).

- [ ] **Step 5: Commit**

```bash
git add extension/src tests/test_lod_stream.gd
git commit -m "refactor: LoD marks queue under the edit lock and drain at the tick"
```

---

### Task 12: Island windows become an inbox

Removes the edit → `windows_mutex_` edge, and the mutex with it.

**Files:**
- Modify: `extension/src/physics/island_manager.h`, `island_manager.cpp`
- Modify: `extension/src/debug/hooks_world.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `IslandManager::drain_inbox()`, called twice per `run_frame` and by `debug_drain_invalidations()`. `windows_mutex_` deleted.

- [ ] **Step 1: List the windows_ readers**

```bash
rg -n 'windows_' extension/src
```

Expected: every hit is `run_frame`, `run_connectivity`, `queue_retry_window`, `note_extract_failure`, `note_extract_success`, `stats()`, `teardown()`, `debug_windows()` and `note_edit` — all main-thread once the inbox exists. If a hit is reachable from another thread, stop and report instead of deleting the mutex.

- [ ] **Step 2: Add the inbox**

In `extension/src/physics/island_manager.h`, add to the public section next to `record`:

```cpp
	// Apply the queued edits to the window list. Main thread; takes the edit lock for the
	// swap, so never call it while holding that lock.
	void drain_inbox();
```

and replace the `std::mutex windows_mutex_;` member with:

```cpp
	// Edits queued by record(), guarded by WorldStore::edit_mutex(); drained on the main
	// thread by drain_inbox(). windows_ is main-thread only.
	// ponytail: unbounded. It only grows while a physics-initialized world never runs
	// run_frame; the shipped game runs it every frame. Bound it (merging by window, as
	// note_edit already does) if that ever stops being true.
	struct InboxEdit {
		ve::EditOp op;
		int64_t seq = 0;
	};
	std::vector<InboxEdit> inbox_;
```

- [ ] **Step 3: Queue instead of labelling**

In `island_manager.cpp`, replace `record`'s body:

```cpp
void IslandManager::record(const ve::Invalidation &inv) {
	// Edit lock held: queue only, never windows bookkeeping (core/edit_pipeline.h).
	if (inv.reason != ve::InvalidationReason::kEdit || !inv.notify_islands) return;
	inbox_.push_back({*inv.op, inv.seq});
}

void IslandManager::drain_inbox() {
	std::vector<InboxEdit> edits;
	{
		std::lock_guard<std::mutex> lock(handles_.store->edit_mutex());
		edits.swap(inbox_);
	}
	for (const InboxEdit &e : edits) note_edit(e.op, e.seq);
}
```

Delete `std::lock_guard<std::mutex> lock(windows_mutex_);` from `note_edit`, and delete every other `windows_mutex_` lock (`teardown`, `run_connectivity`'s two re-queue blocks, `queue_retry_window`, `note_extract_failure`, `note_extract_success`, `run_frame`'s window block, `stats()`, `debug_windows()`), keeping the blocks' bodies. In `teardown`, add to the `windows_.clear();` line's scope:

```cpp
	// teardown_physics holds the edit lock across this call, so the inbox is cleared without
	// taking it again.
	inbox_.clear();
```

- [ ] **Step 4: Drain in run_frame**

In `run_frame`, add before the `// 2. Results.` comment:

```cpp
	// Edits queued since the last frame become windows before anything lands or labels, so a
	// carve this frame is seen by this frame's connectivity exactly as it was when note_edit
	// ran under the edit lock.
	drain_inbox();
```

and again immediately before the `// 3. Connectivity, ONCE (spec §5).` comment:

```cpp
	drain_inbox(); // step 2's own carves
```

- [ ] **Step 5: Drain from the hook**

In `extension/src/debug/hooks_world.cpp`, extend:

```cpp
void VoxelDebugHooks::debug_drain_invalidations() {
	// Each deferred consumer's own drain -- the function its tick calls, never a copy.
	if (world_->context().lod) world_->context().lod->drain_invalidations();
	if (world_->island_manager()) world_->island_manager()->drain_inbox();
}
```

- [ ] **Step 6: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_island_body.gd,res://tests/test_edit_fanout.gd
```

Expected: the fan-out pins unchanged (the hook drains before reading), `test_connectivity` and `test_island_body` within their baseline failure counts.

- [ ] **Step 7: Commit**

```bash
git add extension/src
git commit -m "refactor: island windows queue under the edit lock and drain in run_frame"
```

---

### Task 13: One statement of the lock order

**Files:**
- Modify: `extension/src/core/world_store.h`, `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`, `extension/src/mesh/consolidation.h`, `extension/src/voxel_world.cpp`, `extension/src/render/orchestrator.h`

**Interfaces:**
- Consumes: the statement in `core/edit_pipeline.h`.
- Produces: `rg -il 'lock order' extension/src` returning only `core/edit_pipeline.h`.

- [ ] **Step 1: Find every restatement**

```bash
rg -in 'lock order' extension/src
```

- [ ] **Step 2: Replace each one with a pointer**

Rewrite every hit outside `core/edit_pipeline.h` so it names no order of its own. Keep each comment's other content. The wording must not contain the phrase itself, so the exit scan stays meaningful:

- `core/world_store.h` (the `edit_mutex()` comment): already rewritten in Task 6 — confirm it reads `Acquisition order and the rule that nothing nests inside it: core/edit_pipeline.h.`
- `lod/lod_system.h` (header comment, lines ~11-17): replace the indented `Lock order is …` paragraph with:

```cpp
// THE lod mutex lives here (Task 15 of the frame-module plan). Acquisition rules for it and
// for WorldStore::edit_mutex(): core/edit_pipeline.h. Nothing takes this mutex while holding
// the edit lock -- record() queues, drain_invalidations() applies.
```

- `lod/lod_system.h` (the `mutex()` comment): replace the sentence naming the order with `See core/edit_pipeline.h.`
- `lod/lod_system.cpp`: the comment on the old `note_edit` is gone with Tasks 6 and 11; confirm no hit remains.
- `mesh/consolidation.h` (header comment): replace `Lock order is verbatim from VoxelWorld: render lifetime mutex -> WorldStore::edit_mutex() -> lod mutex (VoxelWorld::lod_mutex_ today; LodSystem::mutex() after Phase 5).` with `Acquisition rules: core/edit_pipeline.h. The commit holds the render lifetime mutex and the edit lock; it takes no other lock.`
- `voxel_world.cpp` (both admission comments): replace `// Same lock order as before the Task 13 move: admission -> render_lifetime_mutex_` with `// Admission then render lifetime, as before the Task 13 move (see core/edit_pipeline.h)`.
- `render/orchestrator.h`: replace `(lock order: g_voxel_compositor_admission_mutex -> render_lifetime_mutex_, unchanged from the pre-move bodies)` with `(admission then render lifetime, unchanged from the pre-move bodies; see core/edit_pipeline.h)`.

- [ ] **Step 3: Check the scan**

```bash
rg -il 'lock order' extension/src
```

Expected: exactly `extension/src/core/edit_pipeline.h`.

- [ ] **Step 4: Build**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Expected: build OK (comments only).

- [ ] **Step 5: Commit**

```bash
git add extension/src
git commit -m "docs: the lock order is stated once, in edit_pipeline.h"
```

---

### Task 14: Exit evidence, results report and roadmap status

**Files:**
- Create: `docs/superpowers/plans/2026-09-17-edit-pipeline-results.md`
- Modify: `docs/superpowers/specs/2026-09-17-edit-pipeline-design.md`
- Modify: `docs/superpowers/plans/2026-09-13-frame-module.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the results report.

- [ ] **Step 1: Full regression**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh
```

Run Task 1 Step 3's extraction script on the new report and compare with the baseline by suite count and failing case name + message. The only expected count change is `test_connectivity` losing the two cases Task 9 deleted and gaining the two cases Tasks 3/4 added, plus the new `test_edit_fanout` suite. Any new failure: stash, rebuild the parent commit, re-run that suite, and only then attribute it.

- [ ] **Step 2: Exit scans**

```bash
rg 'WorldStore::append_edit\(|append_edit_locked|struct EditSink|struct ConsolidationSink|on_edit_appended|LodSystem::note_edit' extension/src
rg -il 'lock order' extension/src
rg 'max_override_bricks' tests/test_connectivity.gd
rg 'collect_ops_for_aabb|edit_log\(\)->ops\(' extension/src/physics
```

Expected: the first, third and fourth print nothing; the second prints only `extension/src/core/edit_pipeline.h`. Record each command with its real output in the report.

- [ ] **Step 3: Check the sink rule by reading**

```bash
rg -n 'void .*::record\(const ve::Invalidation' extension/src
```

Read each body and confirm it takes no mutex and only appends to its own queue (or, for the stats half, increments a counter and logs). List them in the report.

- [ ] **Step 4: Re-trace the change cost**

Count the files a new edit-hearing consumer touches today: its own `.h`/`.cpp` (implement `record`, drain in its own tick) plus the one registration site. Record the before (5) and after numbers in the report.

- [ ] **Step 5: Write the results report**

Create `docs/superpowers/plans/2026-09-17-edit-pipeline-results.md`:

```markdown
# Edit pipeline (sub-project 5b) — results

Implementation baseline tested through: `<short sha>`. Documentation commit: `<short sha>`.
Recorded <date> on <machine / GPU / Godot version>.
Baseline: `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md`. Report: `reports/report_<N>`.

## Regression
Build: `./build.sh …` — Build OK.

Native: `<doctest summary line>`

gdUnit: <cases / errors / failures>; differences from baseline:
- `test_edit_fanout`: new suite, 4 cases.
- `test_connectivity`: +2 cases (staleness pin, held world), -2 cases (Task 9 deletions, named).
- `test_consolidation`: +2 cases (workaround proof, hold seam).
- <every other difference, or "all other suites kept their counts and failure sets">

## Pins
| Pin | Result |
|---|---|
| `EDITS_GOLDEN` | unchanged |
| `FORCED_COMMIT_GOLDEN` | unchanged |
| `ASYNC_COMMIT_GOLDEN` | unchanged |
| `LOD_GOLDEN` | unchanged |
| staleness (test_connectivity) | moved in `<Task 10 sha>` — a bake appends nothing, so an in-flight extraction is no longer stale |
| `test_frame_shipped_golden` | unchanged |

## Lock edges removed
| Edge | Removed in |
|---|---|
| edit → `LodSystem::mutex()` | `<Task 11 sha>` |
| edit → `IslandManager::windows_mutex_` (mutex deleted) | `<Task 12 sha>` |

## Sinks (the record() rule)
<one line per record() body: file:line, what it queues, and that it takes no lock>

## Exit checks
<the four commands from Step 2 with their real output>

## Change cost (spec §8)
| Scenario | Before | After | Files after |
|---|---|---|---|
| A new consumer that must hear about edits | 5 | 2 | the consumer's `.h` and `.cpp` |

## Deletions
`WorldStore::append_edit{,_locked}`, `EditSink`, `ConsolidationSink`, `WorldStore::bump_edit_seq`,
`VoxelWorld::append_edit_locked`, `VoxelWorld::on_edit_appended`, `LodSystem::note_edit` (public),
`Collaborators::append_edit_locked`, `ConsolidationCoordinator::Collaborators::{lod_tree, lod_mutex,
pending_dirty}`, `LodSystem::{tree_slot, mutex_slot}`, `IslandManager::windows_mutex_`, the post-carve
restore branch with `debug_set_fail_next_carve` / `debug_set_fail_next_restore`, and
`test_connectivity.gd`'s `max_override_bricks = 1`.

## Open
`extension/src/debug/hooks_physics.cpp:<line>` (`debug_mesh_submit`), handed over by 5a, is a
diagnostic mesh-job op copy like its 5a siblings, not part of the edit spine; it stays.
<anything else recorded but not resolved, or "nothing else">
```

- [ ] **Step 6: Update the spec and the roadmap**

In `docs/superpowers/specs/2026-09-17-edit-pipeline-design.md`: set `**Status:** Implemented; see docs/superpowers/plans/2026-09-17-edit-pipeline-results.md`, and append a section `## 11. Decided during planning` holding this plan's fourteen numbered decisions verbatim.

In `docs/superpowers/plans/2026-09-13-frame-module.md`, under `### Sub-project 5 — World field query and edit spine`, replace the **Status.** paragraph with:

```markdown
**Status.** 5a (read side: S3, `WorldField`, public raycast) implemented; see `docs/superpowers/plans/2026-09-16-world-field-query-results.md`. 5b (write side: `EditPipeline`, fan-out, lock order, `max_override_bricks = 1`) implemented; see `docs/superpowers/plans/2026-09-17-edit-pipeline-results.md`.
```

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/plans/2026-09-17-edit-pipeline-results.md docs/superpowers/specs/2026-09-17-edit-pipeline-design.md docs/superpowers/plans/2026-09-13-frame-module.md
git commit -m "docs: record edit pipeline exit criteria and results"
```
