# World Field Query (Sub-project 5a) and S3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One module (`ve::WorldField` / `FieldView`) answers "the world field here" for every CPU consumer and builds every GPU job's field inputs, gameplay raycasts go through a public `VoxelWorld.raycast`, and S3 (GPU override aliasing across region borders, LoD op truncation) is characterized and fixed first.

**Architecture:** S3a/S3b: every GPU override table is tagged with its region in the tail of the existing table buffer; `sample_field_override` resolves the table from the sample's own region (fast path: the job's table). S3c: `gather_ops` drops ops the LoD tree already treats as invisible, then refuses instead of truncating. `WorldField` (pure, `extension/src/world/`) is a value built by `WorldStore::field()`; `lock()` returns a `FieldView` holding the edit mutex, and `snapshot_lattice` / `snapshot_region` are its only copying products. It implements `ChunkProbe` and `ContactProbe` directly, so both adapters are deleted. Consumers migrate one per commit.

**Tech Stack:** C++20, godot-cpp (Godot 4.7), GLSL through `RenderingDevice`, doctest (native tests), gdUnit4 (GPU/scene tests), SCons, GDScript.

**Spec:** `docs/superpowers/specs/2026-09-16-world-field-query-design.md`. Roadmap: `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.4, §10.

## Global Constraints

- Branch: `feat/world-field-query` (already checked out; spec committed).
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)`. A full C++ rebuild can take ~20 min; a one-file change relinks in minutes.
- Native tests: `cd extension && scons -Q test; cd ..`. One case: `extension/build/tests/ve_tests -tc="<name>"`.
- Regenerate generated GLSL goldens after an intentional change: `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests; cd ..`, then inspect `git diff shaders/generated`.
- GPU tests: `./gdunit_tests.sh -a res://tests/<suite>.gd` (comma list allowed). Full run: `./gdunit_tests.sh`. Reports: `reports/report_N/results.xml`.
- Shaders load from disk at world init; a shader-only edit needs no rebuild.
- **Baseline failures:** compare against `docs/superpowers/plans/2026-09-16-world-field-query-baseline.md` (Task 1) by case name *and* message. A suite's case **count** dropping is itself a failure. The set drifts: stash and re-run on the parent commit before blaming a change.
- **Golden policy:** a `refactor:` commit must leave every golden and every `test_world_field_consumers.gd` pin unchanged. If one moves, stop the task and report; do not re-record. Only a `fix:` commit may move a golden, and its message names the measured cause.
- GPU timing *values* are invalid on this machine (`debug_gpu_timings()` returns -1); never pin them. Frame-time comparisons use an interleaved A/B/A run.
- **Lock order does not change.** A `FieldView` is taken exactly where the edit mutex is taken today; no lock is held longer; no lock is added. `locked_by_caller()` is only for code that already holds `WorldStore::edit_mutex()`.
- **Bug fixes land in `fix:` commits** separate from any move, each immediately after its failing test.
- C++ and GDScript indentation is tabs. Match surrounding comment density.
- Commit messages are plain conventional messages with no attribution trailers.

## Decided during planning (amends the spec)

Task 15 writes these into the spec.

1. **S3a and S3b share one mechanism and one `fix:` commit** (region tags); S3c has its own.
2. **Tags live in the tail of the existing override table buffer** (`[count, then x, y, z, valid per table]` at index `32 * 32768`), so no shader gains a binding and no uniform set changes. They are synced from the CPU region→table maps: once per `WorldStreamer::run_frame` for the render atlas, once per `MeshService` worker loop iteration for the worker pool, and explicitly in `debug_lod_diff`. An **untagged** table keeps today's lookup: debug fixtures (`debug_fill_override_pool`) install tables without a CPU map entry.
3. **§3.1 amended.** The oracle the S3 tests depend on is `debug_lod_diff`'s: it evaluates the gathered (truncated) job list and uploads only the origin region's overrides, so it cannot see S3b or S3c. Task 2 fixes that. `debug_mesh_lattice_diff` is self-consistent (no override table on either side) and moves in Task 13, where it gains the chunk region's table like `debug_mesh_diff`.
4. **The S3c oracle applies the same relevance rule as the fix.** The property under test is "no silent truncation". Dropping ops shorter than half a LoD cell is the approximation `LodTree::mark_dirty` already makes (those ops never trigger a rebuild), now applied consistently at build time.
5. **`FieldView::snapshot` is lattice-shaped:** `snapshot_lattice(ops_lo, ops_hi, origin, voxel, dim, out)`. All four island-extract sites collect ops over the box union's AABB but copy override bricks over the lattice's brick range and take the table of the lattice origin's region; one AABB cannot reproduce that verbatim. The same shape serves the chunk and LoD diff oracles in Task 13.
6. **`WorldField` takes a pointer to the region→table map**, not a lookup callable (one implementation).
7. **`FieldView` copies the field's pointers**, so `store->field().lock()` outlives the `WorldField` temporary. `WorldField::locked_by_caller()` builds a view that neither takes nor releases the mutex, for `ConsolidationCoordinator::pump_async`, `force_region` and `debug_consolidate_diff`, which hold the edit lock across far more than the snapshot.
8. **`ve::SnapshotSources`** (a materialized `FieldSourceSnapshot`) is added for §5 rows 4 and 7: CPU evaluation over exactly what the GPU job received, without the live store.
9. **Consumer characterization pins are printed and pasted** (the `test_frame_shipped_golden` pattern). The op-cap refusal and the merge-ground gate that consumes `raycast_down` are already pinned by `test_connectivity.gd` and `test_island_body.gd`; those suites are the pin.
10. **Change-cost scenario** is traced as "a new downward ground query in a consumer that already holds the store": today `raycast_down` needed `core/world_store.h`, `core/world_store.cpp` and the consumer (3 files); after 5a it is the consumer alone (1 file).
11. **The S3a brick-generation claim can be float-boundary dependent:** the +x apron sample sits exactly on x = 25.6. A green result closes that half with the test as evidence; the single-table job claims (collider chunk, island extract) do not depend on it.
12. **`IslandManager` gains `refine_config()`** so `debug_contact_samples` asks the field with the manager's `face_samples` after `IslandManager::contact_samples` is deleted.
13. **The S3c counter is `LodStats::op_overflow`**, reported as `debug_lod_stats()["op_overflow"]` (the spec's `lod_op_overflow` name, without the prefix every other key in that Dictionary omits). A refused chunk is marked with a new `LodTree::note_refused`: it keeps drawing its pages and is not re-gathered until an edit or consolidation dirties it again.
14. **S3 fixture worlds use the golden pipeline** and carve closed pockets at y = 38.8, where the analytic terrain is at least 2.4 m of rock everywhere, so every CPU/GPU difference is a lookup difference and never a surface-crossing one.

## File Map

| File | Status | Responsibility |
|---|---|---|
| `extension/src/world/world_field.{h,cpp}` | Create | `WorldField`, `FieldView`, `FieldSnapshot`, `RegionSnapshot`, `SnapshotSources` |
| `extension/src/world/override_store.{h,cpp}` | Modify | `kMaxOverrideTables`, tag layout constants, `override_table_tags()` |
| `extension/src/gpu_layout/constants.cpp` | Modify | Emit `MAX_OVERRIDE_TABLES`, `OVERRIDE_TABLE_TAG_BASE` |
| `shaders/generated/constants.glslh`, `shaders/generated/field.glslh.golden` | Regenerate | Generated constants; field golden embeds `field_ops.glslh` |
| `shaders/field_ops.glslh` | Modify | `override_table_at`; both override samplers use it (S3a/S3b) |
| `extension/src/render/override_pool.{h,cpp}` | Modify | Tag tail in `tables()`; `set_table_tags` |
| `extension/src/render/world_streamer.cpp` | Modify | Sync atlas tags per frame |
| `extension/src/render/mesh_service.cpp` | Modify | Sync worker tags per loop iteration |
| `extension/src/lod/lod_grid.{h,cpp}` | Modify | `lod_extent_visible`, `lod_op_visible`, `lod_cut_ops` |
| `extension/src/lod/lod_tree.{h,cpp}` | Modify | `mark_dirty` uses `lod_extent_visible`; `note_refused` |
| `extension/src/lod/lod_system.{h,cpp}` | Modify | `gather_ops` returns fit; tick refuses over-cap chunks; `op_overflow` stat |
| `extension/src/core/world_store.{h,cpp}` | Modify | `field()`; `snapshot_field_sources` and `raycast_down` deleted |
| `extension/src/connectivity/contact_refine.{h,cpp}` | Modify | `ContactProbe::contact_samples(cell, axis, face_samples)` |
| `extension/src/physics/collider_streamer.{h,cpp}` | Modify | Holds a `WorldField`; `LogProbe` deleted |
| `extension/src/physics/island_manager.{h,cpp}` | Modify | Field view for contact, extract inputs, ground rays; `LogContactProbe`, `contact_samples` deleted; `refine_config()` |
| `extension/src/mesh/consolidation.cpp` | Modify | `snapshot_region` |
| `extension/src/voxel_world.{h,cpp}` | Modify | Collider init, extract diff, public `raycast` |
| `extension/src/debug/hooks.h`, `hooks_lod.cpp`, `hooks_physics.cpp`, `hooks_world.cpp` | Modify | LoD diff mirrors the worker; hooks read the field |
| `extension/tests/override_bake.h` | Create | Shared CPU bake helper (moved out of `test_override_store.cpp`) |
| `extension/tests/test_world_field.cpp` | Create | Native `WorldField` contract |
| `extension/tests/test_override_store.cpp`, `test_lod_grid.cpp`, `test_lod_tree.cpp`, `test_contact_refine.cpp` | Modify | Tags, LoD cut, `note_refused`, probe argument |
| `tests/test_override_region_border.gd` | Create | S3 characterization |
| `tests/test_world_field_consumers.gd` | Create | Consumer pins |
| `tests/test_voxel_world_raycast.gd` | Create | Public raycast contract |
| `demo/edit_tool.gd`, `demo/hud.gd` | Modify | `VoxelWorld.raycast` |
| `docs/superpowers/plans/2026-09-16-world-field-query-baseline.md` | Create | Baseline failure set and change-cost "before" trace |
| `docs/superpowers/plans/2026-09-16-world-field-query-results.md` | Create | Exit evidence |
| `docs/superpowers/specs/2026-09-16-world-field-query-design.md`, `specs/2026-09-13-frame-module-design.md`, `plans/2026-09-13-frame-module.md` | Modify (Task 15) | Amendments and status rows |

---

### Task 1: Record the baseline failure set

No production change. Everything later compares against this file.

**Files:**
- Create: `docs/superpowers/plans/2026-09-16-world-field-query-baseline.md`

**Interfaces:**
- Consumes: nothing.
- Produces: the baseline file (Task 5 appends the change-cost trace to it).

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

Create `docs/superpowers/plans/2026-09-16-world-field-query-baseline.md`, pasting the real output into each section:

```markdown
# World field query — baseline

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
git add docs/superpowers/plans/2026-09-16-world-field-query-baseline.md
git commit -m "docs: world field query baseline failure set"
```

---

### Task 2: `debug_lod_diff` mirrors the shipped worker and reads the world as its oracle

A test-infrastructure change: the hook's GPU side gets every consolidated region's overrides (as the shipped worker has), and its CPU oracle evaluates each sample against that sample's own region op list, not against the job's gathered list. Without this, the S3b and S3c tests in Tasks 3–4 would compare the GPU against the same mistake.

**Files:**
- Modify: `extension/src/debug/hooks_lod.cpp` (`VoxelDebugHooks::debug_lod_diff`)

**Interfaces:**
- Consumes: `WorldStore::override_tables()`, `WorldStore::overrides()`, `EditLog::ops(region)`, `LodBuildPass::upload_override`, `LodBuildPass::set_override_table`.
- Produces: `debug_lod_diff(level, coord)` keeps its Dictionary keys; its oracle is now "the world".

- [ ] **Step 1: Copy every table and its bricks under the edit lock**

In `debug_lod_diff`, replace the two lines

```cpp
	const ve::IVec3 region = ve::region_of_point(origin[0], origin[1], origin[2]);
	const int override_table = world_->context().store->override_table_for_region(region);
```

with:

```cpp
	const ve::IVec3 region = ve::region_of_point(origin[0], origin[1], origin[2]);
	// Mirror the shipped worker: MeshService publishes EVERY consolidated region's bricks and
	// table into its one pool, and the job carries the origin region's table. Copied under the
	// edit lock so the upload below never reads the live store from the worker thread.
	struct TableCopy {
		int table = -1;
		std::vector<std::pair<int, int>> entries;
	};
	int override_table = -1;
	std::vector<TableCopy> table_copies;
	std::vector<std::pair<int, ve::OverrideBrick>> brick_copies;
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		override_table = world_->context().store->override_table_for_region(region);
		const ve::OverrideStore *store_overrides = world_->context().store->overrides();
		if (store_overrides) {
			for (const auto &it : world_->context().store->override_tables()) {
				const ve::IVec3 base{std::get<0>(it.first) * ve::kRegionBricks,
						std::get<1>(it.first) * ve::kRegionBricks,
						std::get<2>(it.first) * ve::kRegionBricks};
				TableCopy copy;
				copy.table = it.second;
				for (int z = 0; z < ve::kRegionBricks; z++)
					for (int y = 0; y < ve::kRegionBricks; y++)
						for (int x = 0; x < ve::kRegionBricks; x++) {
							const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
							const int slot = store_overrides->slot_of(b);
							if (slot < 0) continue;
							const ve::OverrideBrick *data = store_overrides->data(slot);
							if (!data) continue;
							brick_copies.emplace_back(slot, *data);
							copy.entries.emplace_back(ve::brick_index_in_region(b), slot);
						}
				table_copies.push_back(std::move(copy));
			}
		}
	}
```

- [ ] **Step 2: Upload the copies instead of the origin region's live bricks**

Inside the `run_sync` lambda, replace the whole block that starts `std::vector<std::pair<int, int>> override_entries;` and ends with `lod.set_override_table(0, override_table, override_entries);\n\t\t}` with:

```cpp
		for (const auto &brick : brick_copies) {
			if (!lod.upload_override(brick.first, brick.second)) {
				lod.set_field_context(nullptr);
				lod_context.teardown();
				lod.teardown();
				memdelete(rd);
				return;
			}
		}
		// Region slots are only the region map's index on this private device; the LoD
		// shader reads the table from the job's push constant.
		for (size_t i = 0; i < table_copies.size(); i++)
			lod.set_override_table(static_cast<int>(i), table_copies[i].table, table_copies[i].entries);
```

- [ ] **Step 3: Evaluate the oracle against the world, not the gathered list**

Replace the "1. The fine lattice against the CPU field." loop with:

```cpp
	// 1. The fine lattice against the CPU field. The oracle is the WORLD: each sample reads its
	// own region's op list (ve::raycast's rule -- an op is appended to every region it
	// touches), so a truncated or wrongly-tabled job shows up as a diff instead of agreeing
	// with the same mistake.
	int fine_max_diff = 0;
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		const ve::EditLog *log = world_->context().store->edit_log();
		std::map<std::tuple<int, int, int>, std::vector<ve::EditOp>> region_ops;
		const auto ops_at = [&](float x, float y, float z) -> const std::vector<ve::EditOp> & {
			const ve::IVec3 r = ve::region_of_point(x, y, z);
			const std::tuple<int, int, int> key{r.x, r.y, r.z};
			auto it = region_ops.find(key);
			if (it == region_ops.end())
				it = region_ops.emplace(key, log ? log->ops(r) : std::vector<ve::EditOp>{}).first;
			return it->second;
		};
		for (int z = 0; z < ve::kLodFineLattice; z++)
			for (int y = 0; y < ve::kLodFineLattice; y++)
				for (int x = 0; x < ve::kLodFineLattice; x++) {
					const float p[3] = {origin[0] + (static_cast<float>(x) - 3.0f) * cell * 0.5f,
							origin[1] + (static_cast<float>(y) - 3.0f) * cell * 0.5f,
							origin[2] + (static_cast<float>(z) - 3.0f) * cell * 0.5f};
					const std::vector<ve::EditOp> &here = ops_at(p[0], p[1], p[2]);
					const float s = ve::eval_field(gen, here.data(), static_cast<int>(here.size()),
							p[0], p[1], p[2], &world_->context().store->volumes(),
							world_->context().store->overrides()).sdf;
					const int idx = ve::lod_fine_index(x, y, z);
					const int diff = std::abs(static_cast<int>(fine_sdf[idx]) -
							static_cast<int>(ve::lod_encode_sdf(s, cell)));
					fine_max_diff = std::max(fine_max_diff, diff);
				}
	}
	d["fine_max_diff"] = fine_max_diff;
```

Add `#include <map>` and `#include <tuple>` at the top of `hooks_lod.cpp` if absent.

- [ ] **Step 4: Build and run the LoD diff suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_lod_mesh_diff.gd,res://tests/test_lod_build.gd,res://tests/test_consolidation.gd
```

Expected: every case matches the baseline (these suites have fewer than 256 ops and no cross-region consolidation, so the world oracle equals the old list oracle).

- [ ] **Step 5: Commit**

```bash
git add extension/src/debug/hooks_lod.cpp
git commit -m "test: debug_lod_diff mirrors the worker's override tables and reads the world as its oracle

The hook uploaded only the origin region's override bricks and compared the
GPU against the gathered (truncated) op list, so it could not see a LoD job
that reads the wrong region's table or drops ops past 256. It now copies every
consolidated table under the edit lock, as MeshService publishes them, and
evaluates each sample against its own region's op list."
```

---

### Task 3: S3a/S3b — override tables follow the sample's region

**Files:**
- Create: `tests/test_override_region_border.gd`
- Modify: `extension/src/world/override_store.h`, `extension/src/world/override_store.cpp`
- Modify: `extension/src/gpu_layout/constants.cpp`; regenerate `shaders/generated/constants.glslh`, `shaders/generated/field.glslh.golden`
- Modify: `shaders/field_ops.glslh`
- Modify: `extension/src/render/override_pool.h`, `extension/src/render/override_pool.cpp`
- Modify: `extension/src/render/world_streamer.cpp`, `extension/src/render/mesh_service.cpp`, `extension/src/debug/hooks_lod.cpp`
- Test: `extension/tests/test_override_store.cpp`

**Interfaces:**
- Consumes: hooks `debug_init_physics`, `debug_stream_region`, `debug_stream_frame`, `debug_slot_of_region`, `debug_field_sdf`, `debug_apply_sphere_subtract`, `debug_consolidate_region`, `debug_region_op_count`, `debug_brick_diff`, `debug_mesh_diff`, `debug_island_extract_diff`, `debug_lod_diff`.
- Produces:
  - `inline constexpr int ve::kMaxOverrideTables = 32;`, `ve::kOverrideTableTagBase`, `ve::kOverrideTableTagInts` (`world/override_store.h`)
  - `std::vector<int32_t> ve::override_table_tags(const std::map<std::tuple<int, int, int>, int> &tables)`
  - `void OverridePool::set_table_tags(RenderingDevice *rd, const std::vector<int32_t> &tags)`
  - GLSL `int override_table_at(ivec3 brick, int hint)`
  - `tests/test_override_region_border.gd` with helpers `make_world()`, `assert_rock()`, `carve_and_consolidate()`, `make_op()` (Task 4 appends to it).

- [ ] **Step 1: Write the S3a/S3b characterization suite**

Create `tests/test_override_region_border.gd`:

```gdscript
extends GdUnitTestSuite
# S3 (docs/superpowers/specs/2026-09-16-world-field-query-design.md §3). The GPU reads a baked
# override through ONE table per job and indexes it with `brick & 31`, so a sample outside
# that table's region reads a brick of the wrong region. The CPU OverrideStore resolves every
# brick globally and is the oracle.
#
# Fixture: the golden pipeline's analytic terrain. At y = 38.8 there is at least 2.4 m of rock
# everywhere (surface = 51.2 + hills, hills >= -10; the analytic cave spans y 44..54 around
# x = z = 30), so each carve below is a closed pocket and the CPU field beside it is clamped
# rock. Region R = (0, 1, 0) spans x 0..25.6, y 25.6..51.2, z 0..25.6.

const R := Vector3i(0, 1, 0)
const R_CENTER := Vector3(12.8, 38.4, 12.8)
const ROCK_Y := 38.8   # brick y 48
const ROCK_Z := 16.4   # brick z 20

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
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_stream_region(R)
	assert_int(w.hooks().debug_slot_of_region(R)).is_greater_equal(0)
	return w

func assert_rock(w: VoxelWorld, points: Array) -> void:
	for p in points:
		assert_float(w.hooks().debug_field_sdf(p)).override_failure_message(
			"fixture: %s is not deep rock on this terrain" % p).is_less(-0.6)

func carve_and_consolidate(w: VoxelWorld, centre: Vector3, radius: float) -> void:
	w.hooks().debug_apply_sphere_subtract(centre, radius)
	assert_bool(w.hooks().debug_consolidate_region(R)).is_true()
	assert_int(w.hooks().debug_region_op_count(R)).is_equal(0)

func make_op(type: int, material: int, pos: Vector3, radius: float,
		aux0: int = 0, aux1: int = 0) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type); b.put_u32(material)
	b.put_float(pos.x); b.put_float(pos.y); b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(aux0); b.put_u32(aux1)
	return b.data_array

# S3a, region-map shaders. Brick (31, 48, 20)'s +x apron plane is x = 25.6, brick 32 of the
# neighbour region; `32 & 31` is brick 0 of R, which holds the baked carve. The pocket makes
# brick 31 resident so the atlas holds it. This half may be float-boundary dependent (plan
# decision 11): green closes it with this test as evidence.
func test_a_brick_beside_a_region_border_reads_no_wrapped_override(timeout := 240000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(0.4, ROCK_Y, ROCK_Z), Vector3(25.6, ROCK_Y, ROCK_Z)])
	carve_and_consolidate(w, Vector3(0.4, ROCK_Y, ROCK_Z), 0.6)
	var pocket := Vector3(25.0, ROCK_Y, ROCK_Z)
	w.hooks().debug_apply_sphere_subtract(pocket, 0.25)
	for i in range(8):
		w.hooks().debug_stream_frame(R_CENTER)
	var slot: int = w.hooks().debug_slot_of_region(R)
	var d: Dictionary = w.hooks().debug_brick_diff(Vector3i(31, 48, 20), slot,
			make_op(0, 0, pocket, 0.25), 1)
	assert_int(int(d.get("slot", -1))).override_failure_message(
		"fixture: brick (31,48,20) is not resident: %s" % d).is_greater_equal(0)
	assert_int(int(d["sdf_max_diff"])).override_failure_message(
		"S3a: the generated brick read R's brick-0 override through the & 31 wrap: %s" % d
		).is_less_equal(1)

# S3a, single-table job. Chunk (0, 6, 2) starts at x = 0; its lattice's first plane is
# x = -0.1, brick -1, and `-1 & 31` is brick 31 of the chunk's own region, 25.5 m away.
func test_a_collider_chunk_lattice_reads_no_wrapped_override(timeout := 240000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(25.2, ROCK_Y, ROCK_Z), Vector3(-0.1, ROCK_Y, ROCK_Z)])
	carve_and_consolidate(w, Vector3(25.2, ROCK_Y, ROCK_Z), 0.6)
	var d: Dictionary = w.hooks().debug_mesh_diff(Vector3i(0, 6, 2))
	assert_bool(d.has("lattice_max_diff")).override_failure_message(
		"fixture: mesh diff did not run: %s" % d).is_true()
	assert_int(int(d["lattice_max_diff"])).override_failure_message(
		"S3a: the chunk lattice read brick 31's override through the & 31 wrap: %s" % d
		).is_less_equal(1)

# S3a, single-table job. The lattice for cells (31..32, 48, 20) starts in R and takes R's
# table; its samples at x >= 25.6 wrap onto R's bricks 0..1, which hold the baked carve.
func test_an_island_lattice_across_a_region_border_reads_no_wrapped_override(timeout := 240000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(0.4, ROCK_Y, ROCK_Z), Vector3(26.0, ROCK_Y, ROCK_Z)])
	carve_and_consolidate(w, Vector3(0.4, ROCK_Y, ROCK_Z), 0.6)
	var d: Dictionary = w.hooks().debug_island_extract_diff(Vector3i(31, 48, 20), Vector3i(32, 48, 20))
	assert_bool(d.get("ok", false)).override_failure_message("fixture: extraction failed: %s" % d).is_true()
	assert_int(int(d["worst_steps"])).override_failure_message(
		"S3a: the island lattice read R's brick-0 override across the border: %s" % d).is_less(2)

# S3b. The level-1 chunk (1, 1, 0) has its origin in R' = (1, 1, 0) and takes R's (none)
# table, but its fine lattice's first samples (x = 24.4, 24.8) lie in R, over a baked carve
# centred on sample (24.8, 38.8, 16.4).
func test_a_lod_chunk_reads_overrides_of_every_region_it_covers(timeout := 240000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(24.8, ROCK_Y, ROCK_Z)])
	carve_and_consolidate(w, Vector3(24.8, ROCK_Y, ROCK_Z), 0.6)
	var d: Dictionary = w.hooks().debug_lod_diff(1, Vector3i(1, 1, 0))
	assert_bool(d.has("fine_max_diff")).override_failure_message("fixture: LoD diff did not run: %s" % d).is_true()
	assert_int(int(d["fine_max_diff"])).override_failure_message(
		"S3b: the LoD chunk ignored the override of a region other than its origin's: %s" % d
		).is_less_equal(1)
```

- [ ] **Step 2: Run the suite and record which claims are red**

Run: `./gdunit_tests.sh -a res://tests/test_override_region_border.gd`

Expected on today's code: the S3b case and at least the two single-table S3a cases FAIL at their last assertion with the `S3a:`/`S3b:` message. Every `fixture:` assertion must PASS; if one fails, fix the fixture (move `ROCK_Y` down by whole bricks, 0.8 m, in all constants), not production code.

Write down, per case, PASS or FAIL and the failing value; it goes into the Step 13 commit message and the results report. If **every** case passes, stop this task after Step 13's commit of the suite alone with message `test: S3a/S3b region-border override characterization (CLOSED: not reproduced)` and the evidence; skip Steps 3–12.

- [ ] **Step 3: Write the native tag test**

Append to `extension/tests/test_override_store.cpp` (add `#include <map>` and `#include <tuple>` at the top):

```cpp
TEST_CASE("override table tags name each table's region and the highest table in use") {
	std::map<std::tuple<int, int, int>, int> tables;
	tables[{0, 1, 0}] = 0;
	tables[{-3, 2, 7}] = 5;
	tables[{9, 9, 9}] = ve::kMaxOverrideTables; // out of range: never tagged
	const std::vector<int32_t> tags = ve::override_table_tags(tables);
	REQUIRE(static_cast<int>(tags.size()) == ve::kOverrideTableTagInts);
	CHECK(tags[0] == 6);
	CHECK(tags[1 + 0 * 4] == 0);
	CHECK(tags[1 + 0 * 4 + 1] == 1);
	CHECK(tags[1 + 0 * 4 + 2] == 0);
	CHECK(tags[1 + 0 * 4 + 3] == 1);
	CHECK(tags[1 + 5 * 4] == -3);
	CHECK(tags[1 + 5 * 4 + 1] == 2);
	CHECK(tags[1 + 5 * 4 + 2] == 7);
	CHECK(tags[1 + 5 * 4 + 3] == 1);
	CHECK(tags[1 + 3 * 4 + 3] == 0); // an unused table is untagged
	CHECK(ve::override_table_tags({})[0] == 0);
	CHECK(ve::kOverrideTableTagBase == ve::kMaxOverrideTables * ve::kRegionBrickCount);
}
```

- [ ] **Step 4: Run it to verify it fails to compile**

Run: `cd extension && scons -Q test; cd ..`
Expected: FAIL, `kMaxOverrideTables` / `override_table_tags` not declared.

- [ ] **Step 5: Add the tag layout and helper**

In `extension/src/world/override_store.h`, add `#include <map>` and `#include <tuple>`, and above `struct OverrideBrick` add:

```cpp
// The GPU keeps at most this many override tables, one per consolidated region
// (render/override_pool.h aliases it).
inline constexpr int kMaxOverrideTables = 32;
// S3: which region each table belongs to, in the tail of the GPU table buffer:
// [count, then per table x, y, z, valid]. A sample outside its job's region finds its own
// region's table through these (shaders/field_ops.glslh override_table_at).
inline constexpr int kOverrideTableTagBase = kMaxOverrideTables * kRegionBrickCount;
inline constexpr int kOverrideTableTagInts = 1 + 4 * kMaxOverrideTables;
```

and below `plan_consolidation`'s declaration:

```cpp
// The tag tail for a region -> table map. `count` is one past the highest tagged table, so
// the shader's scan stops early; out-of-range tables are ignored.
std::vector<int32_t> override_table_tags(const std::map<std::tuple<int, int, int>, int> &tables);
```

In `extension/src/world/override_store.cpp` (add `#include <algorithm>` if absent), inside `namespace ve`:

```cpp
std::vector<int32_t> override_table_tags(const std::map<std::tuple<int, int, int>, int> &tables) {
	std::vector<int32_t> tags(static_cast<size_t>(kOverrideTableTagInts), 0);
	int count = 0;
	for (const auto &it : tables) {
		const int table = it.second;
		if (table < 0 || table >= kMaxOverrideTables) continue;
		const size_t at = 1 + static_cast<size_t>(table) * 4;
		tags[at] = std::get<0>(it.first);
		tags[at + 1] = std::get<1>(it.first);
		tags[at + 2] = std::get<2>(it.first);
		tags[at + 3] = 1;
		count = std::max(count, table + 1);
	}
	tags[0] = count;
	return tags;
}
```

In `extension/src/render/override_pool.h`, replace `static constexpr int kMaxOverrideTables = 32;` with `static constexpr int kMaxOverrideTables = ve::kMaxOverrideTables;`.

- [ ] **Step 6: Emit the constants to GLSL**

In `extension/src/gpu_layout/constants.cpp`, add `#include "world/override_store.h"`, and extend the stream after the `OVERRIDE_MAT_STRIDE_BYTES` line:

```cpp
	     "const int OVERRIDE_MAT_STRIDE_BYTES = " << kOverrideMatStrideBytes << ";\n"
	     "\n"
	     "// Override table tags (world/override_store.h)\n"
	     "const int MAX_OVERRIDE_TABLES = " << kMaxOverrideTables << ";\n"
	     "const int OVERRIDE_TABLE_TAG_BASE = " << kOverrideTableTagBase << ";\n";
```

(the previous statement's terminating `;` moves to the new last line).

- [ ] **Step 7: Resolve the table from the sample's region in the shader**

In `shaders/field_ops.glslh`, directly after `override_mat_byte`'s closing brace, add:

```glsl
// S3: a table belongs to ONE region, and `brick & 31` indexes it correctly only for a sample
// inside that region. The tag tail (ve::override_table_tags) says which region each table
// belongs to. `hint` is the job's table; an UNTAGGED hint keeps the pre-S3 lookup, because
// debug fixtures install tables that no CPU region map names.
ivec4 override_table_tag(int table) {
	int t = OVERRIDE_TABLE_TAG_BASE + 1 + table * 4;
	return ivec4(field_override_tables.slot[t], field_override_tables.slot[t + 1],
			field_override_tables.slot[t + 2], field_override_tables.slot[t + 3]);
}
int override_table_at(ivec3 brick, int hint) {
	ivec3 region = ivec3(floor(vec3(brick) / float(REGION_BRICKS)));
	if (hint >= 0 && hint < MAX_OVERRIDE_TABLES) {
		ivec4 tag = override_table_tag(hint);
		if (tag.w == 0 || tag.xyz == region) return hint;
	}
	int count = min(field_override_tables.slot[OVERRIDE_TABLE_TAG_BASE], MAX_OVERRIDE_TABLES);
	for (int t = 0; t < count; t++) {
		ivec4 tag = override_table_tag(t);
		if (tag.w != 0 && tag.xyz == region) return t;
	}
	return -1;
}
```

In `sample_field_override_gradient`, replace

```glsl
	int table = FIELD_OVERRIDE_TABLE(op_base);
	if (table < 0 || table >= 32) return false;
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
```

with

```glsl
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int table = override_table_at(brick, FIELD_OVERRIDE_TABLE(op_base));
	if (table < 0 || table >= MAX_OVERRIDE_TABLES) return false;
```

and in `sample_field_override`, replace

```glsl
	int table = FIELD_OVERRIDE_TABLE(op_base);
	if (table < 0 || table >= 32) return false;
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
```

with the same three new lines.

- [ ] **Step 8: Reserve and write the tag tail in `OverridePool`**

In `extension/src/render/override_pool.h`, below `void clear_table(RenderingDevice *rd, int table);` add:

```cpp
	// S3: the region of every table (ve::override_table_tags), written into the tail of
	// tables(). A no-op when unchanged, so owners sync it every frame / worker iteration.
	void set_table_tags(RenderingDevice *rd, const std::vector<int32_t> &tags);
```

and beside `std::vector<int> region_tables_;` add `std::vector<int32_t> tags_;`.

In `extension/src/render/override_pool.cpp`, replace

```cpp
	PackedByteArray table_zero = filled_i32(kMaxOverrideTables * ve::kRegionBrickCount, -1);
	tables_ = rd_->storage_buffer_create(static_cast<uint32_t>(table_zero.size()), table_zero);
```

with

```cpp
	// Every table entry starts empty (-1); the tag tail starts untagged (all zero).
	PackedByteArray table_zero = filled_i32(ve::kOverrideTableTagBase + ve::kOverrideTableTagInts, -1);
	std::memset(table_zero.ptrw() + static_cast<int64_t>(ve::kOverrideTableTagBase) * 4, 0,
			static_cast<size_t>(ve::kOverrideTableTagInts) * 4);
	tables_ = rd_->storage_buffer_create(static_cast<uint32_t>(table_zero.size()), table_zero);
	tags_.assign(static_cast<size_t>(ve::kOverrideTableTagInts), 0);
```

In `teardown()`, after `region_tables_.clear();` add `tags_.clear();`. At the end of the file add:

```cpp
void OverridePool::set_table_tags(RenderingDevice *rd, const std::vector<int32_t> &tags) {
	if (!rd || !tables_.is_valid() || static_cast<int>(tags.size()) != ve::kOverrideTableTagInts ||
			tags == tags_)
		return;
	PackedByteArray b;
	b.resize(static_cast<int64_t>(tags.size()) * 4);
	std::memcpy(b.ptrw(), tags.data(), tags.size() * 4);
	rd->buffer_update(tables_, static_cast<uint32_t>(ve::kOverrideTableTagBase) * 4,
			static_cast<uint32_t>(b.size()), b);
	tags_ = tags;
}
```

- [ ] **Step 9: Sync the tags on the render atlas, the worker and the LoD diff hook**

In `extension/src/render/world_streamer.cpp` `run_frame`, directly after the block

```cpp
	{
		std::lock_guard<std::mutex> lock(*edit_mutex_);
		forced_regen.swap(forced_regen_);
	}
```

add

```cpp
	// S3: tag every atlas table with its region before this frame's mark/generate and the
	// raymarch read the pool. Unchanged tags cost one vector compare.
	std::vector<int32_t> table_tags;
	{
		std::lock_guard<std::mutex> lock(*edit_mutex_);
		if (override_tables_) table_tags = ve::override_table_tags(*override_tables_);
	}
	if (!table_tags.empty()) atlas_->overrides().set_table_tags(rd, table_tags);
```

In `extension/src/render/mesh_service.cpp` `run()`, directly after the `for (;;) {` loop's `std::unique_lock<std::mutex> lock(mu_); ... }` selection block closes (the line after `override_publications.swap(pending_override_publications_);\n\t\t\t}\n\t\t}`), add:

```cpp
		// S3: tag every worker table with its region before this iteration's work reads the
		// pool. override_tables_ is only mutated on this thread, by earlier iterations.
		pass.overrides().set_table_tags(rd, ve::override_table_tags(override_tables_));
```

In `extension/src/debug/hooks_lod.cpp` `debug_lod_diff`, inside the lambda after the `set_override_table` loop added in Task 2, add:

```cpp
		std::map<std::tuple<int, int, int>, int> tagged;
		for (const auto &region_table : table_regions) tagged[region_table.first] = region_table.second;
		lod.overrides().set_table_tags(rd, ve::override_table_tags(tagged));
```

and in Task 2's locked copy block, collect the keys: declare `std::vector<std::pair<std::tuple<int, int, int>, int>> table_regions;` next to `table_copies`, and push `table_regions.emplace_back(it.first, it.second);` next to `table_copies.push_back(std::move(copy));`.

- [ ] **Step 10: Regenerate goldens, build and run native tests**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests && ./build/tests/ve_tests; cd ..
git diff --stat shaders/generated
```

Expected: native tests PASS; the diff touches only `constants.glslh` (two new constants) and `field.glslh.golden` (the `override_table_at` block and the two sampler edits).

- [ ] **Step 11: Run the S3 suite and every override consumer**

```bash
./gdunit_tests.sh -a res://tests/test_override_region_border.gd,res://tests/test_consolidation.gd,res://tests/test_world_field_overrides.gd,res://tests/test_brick_diff.gd,res://tests/test_mesh_diff.gd,res://tests/test_lod_mesh_diff.gd,res://tests/test_field_volume_diff.gd,res://tests/test_stored_normals.gd,res://tests/test_frame_shipped_golden.gd,res://tests/test_lod_raster_golden.gd
```

Expected: every case PASSES. `test_frame_shipped_golden` and `test_lod_raster_golden` unchanged (their worlds have no consolidated region, so every table is untagged and `count` is 0).

- [ ] **Step 12: Measure frame time A/B/A**

The shader and C++ changes are uncommitted at this point; A is the parent commit, B the working tree. `tools/run_benchmarks.sh` runs every leg sequentially and files them under `reports/<label>/`.

```bash
git stash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) && tools/run_benchmarks.sh wfq-s3-a1
git stash pop
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) && tools/run_benchmarks.sh wfq-s3-b
git stash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) && tools/run_benchmarks.sh wfq-s3-a2
git stash pop && ./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
for l in a1 b a2; do echo "== $l"; grep -h "BENCH mode\|BENCH frame_avg_ms\|BENCH p50\|BENCH p99" reports/wfq-s3-$l/steady.txt reports/wfq-s3-$l/ridge.txt reports/wfq-s3-$l/edit-bounded.txt; done
```

Expected: for the steady, ridge and edit-bounded legs, B's average and p99 frame time lie within the A1–A2 spread (the edit-bounded leg is the one that consolidates, so it is the only leg with tagged tables). If B is outside it, stop and report the three runs. The `reports/wfq-s3-*` directories are not committed.

- [ ] **Step 13: Commit (test first, then fix)**

```bash
git add tests/test_override_region_border.gd
git commit -m "test: S3a/S3b region-border override characterization (failing)

<per case: PASS or FAIL with the failing value from Step 2>"
git add extension/src/world/override_store.h extension/src/world/override_store.cpp extension/tests/test_override_store.cpp extension/src/gpu_layout/constants.cpp shaders/generated/constants.glslh shaders/generated/field.glslh.golden shaders/field_ops.glslh extension/src/render/override_pool.h extension/src/render/override_pool.cpp extension/src/render/world_streamer.cpp extension/src/render/mesh_service.cpp extension/src/debug/hooks_lod.cpp
git commit -m "fix: GPU override lookups use the sample's own region table (S3a, S3b)

sample_field_override took one table per job (or per op region) and indexed it
with brick & 31, so a sample outside that region read a brick 25.6 m away, and a
LoD chunk ignored every region's overrides but its origin's. Each table is now
tagged with its region in the tail of the table buffer, synced from the CPU
region maps by the streamer and the mesh worker, and the shader resolves the
sample's own region (fast path: the job's table). Frame time A/B/A: <numbers>."
```

(If Step 2 found some cases already green, the fix message says which claims it fixes and the results report records the rest as CLOSED.)

---

### Task 4: S3c — LoD builds never silently truncate their op list

**Files:**
- Modify: `tests/test_override_region_border.gd`
- Modify: `extension/src/lod/lod_grid.h`, `extension/src/lod/lod_grid.cpp`
- Modify: `extension/src/lod/lod_tree.h`, `extension/src/lod/lod_tree.cpp`
- Modify: `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/debug/hooks_lod.cpp`
- Test: `extension/tests/test_lod_grid.cpp`, `extension/tests/test_lod_tree.cpp`

**Interfaces:**
- Consumes: Task 3's suite helpers; `ve::op_world_aabb`, `ve::lod_cell_size`, `ve::kMaxRegionOps`.
- Produces:
  - `bool ve::lod_extent_visible(int level, float longest_m)`
  - `bool ve::lod_op_visible(int level, const EditOp &op)`
  - `bool ve::lod_cut_ops(int level, std::vector<EditOp> *ops)` — drops invisible ops in place, returns `ops->size() <= kMaxRegionOps`
  - `void ve::LodTree::note_refused(int level, IVec3 c)`
  - `bool LodSystem::gather_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out)` — false = over cap after the cut
  - `LodStats::op_overflow`; `debug_lod_stats()["op_overflow"]`; `debug_lod_diff` returns `{"op_overflow": true}` for an over-cap chunk

- [ ] **Step 1: Append the failing S3c cases**

Append to `tests/test_override_region_border.gd`:

```gdscript
# S3c. Level-2 chunk (0, 0, 0) spans 51.2 m on each axis and so covers R and R' = (1, 1, 0).
# Its fine lattice sits on multiples of 0.8 m; every carve below is centred on a sample.
# 150 visible ops per region is 300 in the chunk: gather_ops used to keep the first 256.
func carve_grid(w: VoxelWorld, x0: float, radius: float, offset: float) -> void:
	for i in range(6):
		for k in range(5):
			for m in range(5):
				w.hooks().debug_apply_sphere_subtract(Vector3(
						x0 + 3.2 * i + offset, 27.2 + 2.4 * k + offset, 4.8 + 3.2 * m + offset), radius)

func test_an_over_cap_lod_chunk_is_refused_not_truncated(timeout := 300000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(4.8, 36.8, 4.8), Vector3(46.4, 36.8, 17.6)])
	carve_grid(w, 4.8, 0.6, 0.0)
	carve_grid(w, 30.4, 0.6, 0.0)
	assert_int(w.hooks().debug_region_op_count(R)).is_equal(150)
	assert_int(w.hooks().debug_region_op_count(Vector3i(1, 1, 0))).is_equal(150)
	var d: Dictionary = w.hooks().debug_lod_diff(2, Vector3i(0, 0, 0))
	assert_bool(bool(d.get("op_overflow", false)) or int(d.get("fine_max_diff", 99)) <= 1
		).override_failure_message(
		"S3c: a LoD chunk over the op cap was built from a truncated list: %s" % d).is_true()

# S3c, the cut. The neighbour's 150 ops are 5 cm pockets, shorter than half a level-2 cell
# (0.8 m), so they are dropped and the 150 visible ops build. Offset 0.4 m keeps them off the
# lattice; the oracle applies the same relevance rule (plan decision 4).
func test_ops_too_small_for_a_lod_level_do_not_count_against_its_cap(timeout := 300000) -> void:
	var w := make_world()
	carve_grid(w, 4.8, 0.6, 0.0)
	carve_grid(w, 30.4, 0.05, 0.4)
	var d: Dictionary = w.hooks().debug_lod_diff(2, Vector3i(0, 0, 0))
	assert_bool(bool(d.get("op_overflow", false))).override_failure_message(
		"S3c: sub-half-cell ops counted against the LoD op cap: %s" % d).is_false()
	assert_int(int(d.get("fine_max_diff", 99))).override_failure_message(
		"S3c: the cut LoD chunk disagrees with the world: %s" % d).is_less_equal(1)
```

- [ ] **Step 2: Run them to verify they fail**

Run: `./gdunit_tests.sh -a res://tests/test_override_region_border.gd`
Expected: both new cases FAIL with the `S3c:` message (the first with a `fine_max_diff` > 1 and no `op_overflow` key). Task 3's cases still PASS. If the first new case PASSES, S3c is not reproduced: commit the cases with message `test: S3c LoD op prefix characterization (CLOSED: not reproduced)` plus the evidence and skip Steps 3–12.

- [ ] **Step 3: Write the native tests**

Append to `extension/tests/test_lod_grid.cpp` (add `#include "world/edit_log.h"` and `#include <vector>` if absent):

```cpp
TEST_CASE("an op shorter than half a LoD cell on every axis is invisible at that level") {
	ve::EditOp tiny{};
	tiny.type = ve::kOpSphereSubtract;
	tiny.radius = 0.05f; // 0.1 m across
	ve::EditOp big = tiny;
	big.radius = 0.6f;   // 1.2 m across
	CHECK(ve::lod_op_visible(0, tiny) == ve::lod_extent_visible(0, 0.1f));
	CHECK_FALSE(ve::lod_op_visible(2, tiny)); // half of 1.6 m is 0.8 m
	CHECK(ve::lod_op_visible(2, big));
	CHECK_FALSE(ve::lod_extent_visible(3, 1.5f));
	CHECK(ve::lod_extent_visible(3, 1.6f));
}

TEST_CASE("the LoD op cut keeps order, drops invisible ops and reports whether the rest fits") {
	std::vector<ve::EditOp> ops;
	for (int i = 0; i < ve::kMaxRegionOps + 10; i++) {
		ve::EditOp op{};
		op.type = ve::kOpSphereSubtract;
		op.pos[0] = static_cast<float>(i);
		op.radius = (i % 2 == 0) ? 0.6f : 0.05f;
		ops.push_back(op);
	}
	std::vector<ve::EditOp> cut = ops;
	CHECK(ve::lod_cut_ops(2, &cut));
	REQUIRE(cut.size() == static_cast<size_t>((ve::kMaxRegionOps + 10 + 1) / 2));
	for (size_t i = 1; i < cut.size(); i++) CHECK(cut[i - 1].pos[0] < cut[i].pos[0]);
	for (ve::EditOp &op : ops) op.radius = 0.6f;
	CHECK_FALSE(ve::lod_cut_ops(2, &ops));
}
```

Append to `extension/tests/test_lod_tree.cpp`:

```cpp
TEST_CASE("a refused rebuild keeps drawing its pages and is not re-requested until dirtied again") {
	ve::LodTreeConfig cfg;
	ve::LodTree t(cfg);
	const ve::IVec3 c{0, 0, 0};
	t.note_ready(2, c, 5, 3);
	float lo[3] = {1.0f, 1.0f, 1.0f}, hi[3] = {4.0f, 4.0f, 4.0f};
	t.mark_dirty(lo, hi);
	REQUIRE(t.is_dirty(2, c));
	t.note_building(2, c);
	t.note_refused(2, c);
	CHECK(t.state_of(2, c) == ve::kLodReady);
	CHECK_FALSE(t.is_dirty(2, c));

	const ve::IVec3 fresh{3, 0, 0};
	t.note_building(2, fresh);
	t.note_refused(2, fresh);
	CHECK(t.state_of(2, fresh) == ve::kLodFailed);
}
```

- [ ] **Step 4: Run them to verify they fail to compile**

Run: `cd extension && scons -Q test; cd ..`
Expected: FAIL, `lod_op_visible`, `lod_extent_visible`, `lod_cut_ops`, `note_refused` not declared.

- [ ] **Step 5: Add the relevance rule and cut**

In `extension/src/lod/lod_grid.h`, add `#include <vector>` and, above `op_lod_chunk_range`:

```cpp
// The reduced lattice samples every half cell, so a feature shorter than half a cell on every
// axis is treated as unrepresentable at `level`: LodTree::mark_dirty does not rebuild for it
// and the build does not spend its op cap on it.
bool lod_extent_visible(int level, float longest_m);
bool lod_op_visible(int level, const EditOp &op);
// Drops the ops invisible at `level`, keeping order; true when the rest fits kMaxRegionOps.
// A false verdict leaves the cut list in place for diagnostics; the caller must not build it.
bool lod_cut_ops(int level, std::vector<EditOp> *ops);
```

In `extension/src/lod/lod_grid.cpp` (add `#include "world/edit_log.h"` and `#include <algorithm>` if absent):

```cpp
bool lod_extent_visible(int level, float longest_m) {
	return longest_m >= 0.5f * lod_cell_size(level);
}

bool lod_op_visible(int level, const EditOp &op) {
	float lo[3], hi[3];
	op_world_aabb(op, lo, hi);
	return lod_extent_visible(level, std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]));
}

bool lod_cut_ops(int level, std::vector<EditOp> *ops) {
	if (!ops) return true;
	ops->erase(std::remove_if(ops->begin(), ops->end(),
					   [level](const EditOp &op) { return !lod_op_visible(level, op); }),
			ops->end());
	return ops->size() <= static_cast<size_t>(kMaxRegionOps);
}
```

In `extension/src/lod/lod_tree.cpp` `mark_dirty`, replace `if (longest < 0.5f * lod_cell_size(level)) continue;` with `if (!lod_extent_visible(level, longest)) continue;`.

- [ ] **Step 6: Add `note_refused`**

In `extension/src/lod/lod_tree.h`, below `void note_failed(int level, IVec3 c);`:

```cpp
	// The build was refused before submission (over the op cap after the cut). A node with
	// pages keeps drawing them and stays clean -- the next edit or consolidation that
	// touches it dirties it again -- so a refused chunk is not re-gathered every frame.
	void note_refused(int level, IVec3 c);
```

In `extension/src/lod/lod_tree.cpp`, after `note_failed`:

```cpp
void LodTree::note_refused(int level, IVec3 c) {
	Node &n = nodes_[key(level, c)];
	n.building = false;
	n.state = n.page_count > 0 ? kLodReady : kLodFailed;
}
```

- [ ] **Step 7: Run the native tests**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS, including the three new cases.

- [ ] **Step 8: `gather_ops` cuts and refuses; the tick keeps the old mesh**

In `extension/src/lod/lod_system.h`, change the declaration to:

```cpp
	// False when the chunk's visible ops exceed kMaxRegionOps (S3c): the caller must refuse
	// the build rather than submit a truncated list.
	bool gather_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out);
```

add `int op_overflow = 0; // LoD builds refused because their visible ops exceed the cap` to `struct LodStats` after `partial_allocations`, and beside `lod_overflow_logged_` add `int lod_op_overflow_ = 0; // guarded by lod_mutex_`.

In `extension/src/lod/lod_system.cpp`, replace `gather_ops` with:

```cpp
bool LodSystem::gather_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out) {
	if (!out) return false;
	out->clear();
	std::lock_guard<std::mutex> lock(store()->edit_mutex());
	if (!store()->edit_log()) return true;
	float lo[3], hi[3];
	ve::lod_chunk_aabb(level, coord, lo, hi);
	const float pad = std::max(2.0f * ve::lod_cell_size(level), ve::kLatticeFilterPad);
	for (int a = 0; a < 3; a++) {
		lo[a] -= pad;
		hi[a] += pad;
	}
	ve::collect_ops_for_aabb(*store()->edit_log(), lo, hi, out);
	// S3c: a chronological prefix used to be kept past the cap, silently dropping the newest
	// edits. Ops this level cannot represent go first; if the rest still does not fit, the
	// build is refused and the chunk keeps its last good pages.
	return ve::lod_cut_ops(level, out);
}
```

In `tick`, replace the block from `if (!batch_requests.empty()) {` through its closing brace with:

```cpp
	if (!batch_requests.empty()) {
		std::vector<LodBuildJob> batch;
		std::vector<ve::LodBuildRequest> submitted, refused;
		batch.reserve(batch_requests.size());
		for (const ve::LodBuildRequest &q : batch_requests) {
			LodBuildJob j;
			j.level = q.level;
			j.coord = q.coord;
			if (!gather_ops(q.level, q.coord, &j.ops)) {
				refused.push_back(q);
				continue;
			}
			submitted.push_back(q);
			batch.push_back(std::move(j));
		}
		if (!refused.empty()) {
			lock.lock();
			for (const ve::LodBuildRequest &q : refused) lod_tree_->note_refused(q.level, q.coord);
			lod_op_overflow_ += static_cast<int>(refused.size());
			lock.unlock();
		}
		if (!batch.empty() && !mesh()->submit_lod(std::move(batch))) {
			lock.lock();
			for (const ve::LodBuildRequest &q : submitted) {
				const LodKey key{q.level, q.coord.x, q.coord.y, q.coord.z};
				if (lod_pages_of_.find(key) != lod_pages_of_.end()) {
					lod_tree_->note_ready_dirty(q.level, q.coord);
				} else {
					lod_tree_->note_failed(q.level, q.coord);
				}
			}
			lock.unlock();
		}
	}
```

In `LodSystem::stats()`, next to `s.partial_allocations = …`, add `s.op_overflow = lod_op_overflow_;`. Wherever `lod_overflow_logged_.clear();` runs (teardown), add `lod_op_overflow_ = 0;`.

- [ ] **Step 9: Expose the verdict in the hooks**

In `extension/src/debug/hooks_lod.cpp` `debug_lod_stats`, after `d["pending_request_ids"] = pending_request_ids;` add `d["op_overflow"] = s.op_overflow;`.

In `debug_lod_diff`, replace `world_->context().lod->gather_ops(level, c, &ops);` with:

```cpp
	if (!world_->context().lod->gather_ops(level, c, &ops)) {
		d["op_overflow"] = true;
		return d;
	}
```

and in Task 2's oracle cache, apply the same relevance rule (plan decision 4) — replace the two lines `if (it == region_ops.end())` and the `it = region_ops.emplace(...)` statement under it with:

```cpp
			if (it == region_ops.end()) {
				std::vector<ve::EditOp> visible = log ? log->ops(r) : std::vector<ve::EditOp>{};
				ve::lod_cut_ops(level, &visible); // the oracle never truncates: fit is not asked
				it = region_ops.emplace(key, std::move(visible)).first;
			}
```

- [ ] **Step 10: Build and run the LoD suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_override_region_border.gd,res://tests/test_lod_mesh_diff.gd,res://tests/test_lod_build.gd,res://tests/test_lod_stream.gd,res://tests/test_lod_budget.gd,res://tests/test_lod_raster_golden.gd,res://tests/test_lod_seam.gd,res://tests/test_frame_shipped_golden.gd
```

Expected: every case PASSES; goldens unchanged. If a LoD golden moves, stop and report which edit sizes the golden world uses (a sub-half-cell op that used to be built is the only possible cause).

- [ ] **Step 11: Run the native suite**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS.

- [ ] **Step 12: Commit (test first, then fix)**

```bash
git add tests/test_override_region_border.gd
git commit -m "test: S3c LoD op prefix characterization (failing)

<the failing values from Step 2>"
git add extension/src/lod/lod_grid.h extension/src/lod/lod_grid.cpp extension/src/lod/lod_tree.h extension/src/lod/lod_tree.cpp extension/src/lod/lod_system.h extension/src/lod/lod_system.cpp extension/src/debug/hooks_lod.cpp extension/tests/test_lod_grid.cpp extension/tests/test_lod_tree.cpp
git commit -m "fix: LoD builds cut invisible ops and refuse instead of truncating (S3c)

gather_ops kept a chronological prefix of 256 ops across every region a LoD
chunk covers, silently dropping the newest edits. It now drops ops shorter than
half a cell at the chunk's level (the rule mark_dirty already applies) and, if
the rest still exceeds the cap, the build is refused: the chunk keeps its last
pages and debug_lod_stats reports op_overflow."
```

---

### Task 5: Characterize every consumer that will migrate

Pins what each consumer produces today, proves each pin bites, and records the change-cost "before" trace. No production change is committed.

**Files:**
- Create: `tests/test_world_field_consumers.gd`
- Modify: `docs/superpowers/plans/2026-09-16-world-field-query-baseline.md`

**Interfaces:**
- Consumes: hooks `debug_init_physics`, `debug_stream_region`, `debug_physics_frame`, `debug_physics_stats`, `debug_chunk_collider_info`, `debug_consolidate_region`, `debug_region_op_count`, `debug_store_volume`, `debug_apply_volume_add`, `debug_apply_sphere_subtract`, `debug_contact_samples`, `debug_island_extract_diff`, `debug_raycast`.
- Produces: four pinned cases every later refactor task runs.

- [ ] **Step 1: Write the pin suite with empty goldens**

Create `tests/test_world_field_consumers.gd`:

```gdscript
extends GdUnitTestSuite
# Characterization for sub-project 5a (docs/superpowers/plans/2026-09-16-world-field-query.md,
# Task 5): what every consumer that moves onto ve::WorldField produces today. A refactor task
# must leave every golden below unchanged. Goldens are recorded, not derived: run once, paste
# each printed "<NAME> <json>" line's JSON into the matching const.
#
# Pinned elsewhere (plan decision 9): the island op-cap refusal (test_connectivity.gd), the
# merge-ground gate that reads raycast_down (test_island_body.gd, test_connectivity.gd), and
# the consolidation bake inputs (test_consolidation.gd).

const COLLIDER_GOLDEN := {}
const CONTACT_GOLDEN := {}
const EXTRACT_GOLDEN := {}
const RAY_GOLDEN := {}

const PHYSICS_CENTER := Vector3(60.0, 55.0, 60.0)
const FILL_AT := Vector3(60.8, 73.6, 60.8)
const FILL_CHUNK := Vector3i(9, 11, 9)
const FILL_REGION := Vector3i(2, 2, 2)
const VDIM := 64
const VVOXEL := 0.05
const VRADIUS := 0.35
const VMATERIAL := 2
const VSLOT := 7

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

func make_physics_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 512
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	own(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func make_golden_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	own(w)
	w.ensure_initialized()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, center: Vector3, frames := 6000) -> bool:
	var quiet := 0
	for i in range(frames):
		w.hooks().debug_physics_frame(center)
		var st := w.hooks().debug_physics_stats()
		quiet = quiet + 1 if st["chunks_pending"] == 0 and st["queued"] == 0 else 0
		if quiet >= 4:
			return true
		OS.delay_msec(1)
	return false

func encode_sdf(d: float) -> int:
	var t := clampf((d + 0.64) / 1.28, 0.0, 1.0)
	return int(floor(t * 255.0 + 0.5))

func store_ball(w: VoxelWorld, origin: Vector3) -> void:
	var sdf := PackedByteArray()
	var mat := PackedByteArray()
	sdf.resize(VDIM * VDIM * VDIM)
	mat.resize(VDIM * VDIM * VDIM)
	var c := 0.5 * float(VDIM - 1) * VVOXEL
	for z in range(VDIM):
		for y in range(VDIM):
			for x in range(VDIM):
				var d := (Vector3(x, y, z) * VVOXEL - Vector3(c, c, c)).length() - VRADIUS
				var i := x + y * VDIM + z * VDIM * VDIM
				sdf[i] = encode_sdf(d)
				mat[i] = VMATERIAL if d <= 0.0 else 0
	w.hooks().debug_store_volume(VSLOT, sdf, mat, VDIM)
	w.hooks().debug_apply_volume_add(VSLOT, origin, VVOXEL, VDIM)

func v3(v: Vector3) -> Array:
	return [snappedf(v.x, 0.001), snappedf(v.y, 0.001), snappedf(v.z, 0.001)]

func check(name: String, measured: Dictionary, golden: Dictionary) -> void:
	print(name, " ", JSON.stringify(measured))
	assert_bool(golden.is_empty()).override_failure_message(
		"no golden recorded: paste the %s line above into the const" % name).is_false()
	assert_str(JSON.stringify(measured)).override_failure_message(
		"%s moved" % name).is_equal(JSON.stringify(golden))

func collider_slots(w: VoxelWorld) -> Array:
	var out := []
	for z in range(FILL_CHUNK.z - 1, FILL_CHUNK.z + 2):
		for y in range(FILL_CHUNK.y - 1, FILL_CHUNK.y + 2):
			for x in range(FILL_CHUNK.x - 1, FILL_CHUNK.x + 2):
				out.append(int(w.hooks().debug_chunk_collider_info(Vector3i(x, y, z)).get("slot", -1)) >= 0)
	return out

# ColliderStreamer's residency probe (LogProbe): which chunks around a consolidated fill and
# around a pasted volume get a collider.
func test_collider_residency_is_pinned(timeout := 600000) -> void:
	var measured := {}
	var fill := make_physics_world()
	fill.hooks().debug_stream_region(FILL_REGION)
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	fill.add_child(tool)
	tool.apply_sphere_add(FILL_AT, 2.0, 4)
	assert_bool(fill.hooks().debug_consolidate_region(FILL_REGION)).is_true()
	assert_bool(settle(fill, PHYSICS_CENTER)).is_true()
	measured["fill"] = collider_slots(fill)
	var pasted := make_physics_world()
	store_ball(pasted, Vector3(59.225, 72.025, 59.225))
	assert_bool(settle(pasted, PHYSICS_CENTER)).is_true()
	measured["volume"] = collider_slots(pasted)
	check("COLLIDER_GOLDEN", measured, COLLIDER_GOLDEN)

# IslandManager's contact refinement probe (LogContactProbe): 27 cells x 3 axes around a
# consolidated carve.
func test_contact_samples_are_pinned(timeout := 300000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	own(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_stream_region(Vector3i(0, 0, 0))
	w.hooks().debug_apply_sphere_subtract(Vector3(8.4, 16.8, 16.4), 1.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 0, 0))).is_true()
	var counts := []
	for z in range(19, 22):
		for y in range(19, 22):
			for x in range(9, 12):
				for axis in range(3):
					counts.append(w.hooks().debug_contact_samples(Vector3i(x, y, z), axis))
	check("CONTACT_GOLDEN", {"counts": counts}, CONTACT_GOLDEN)

# Island extract job inputs and the CPU reference: a lattice inside a consolidated region and
# one straddling its border.
func test_island_extract_is_pinned(timeout := 300000) -> void:
	var w := make_golden_world()
	w.hooks().debug_stream_region(Vector3i(0, 1, 0))
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 38.8, 16.4), 1.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 1, 0))).is_true()
	w.hooks().debug_apply_sphere_subtract(Vector3(25.2, 39.6, 16.4), 0.5)
	var measured := {}
	for key in ["inside", "border"]:
		var lo := Vector3i(29, 48, 20) if key == "inside" else Vector3i(31, 48, 20)
		var hi := Vector3i(30, 49, 20) if key == "inside" else Vector3i(32, 49, 20)
		var d: Dictionary = w.hooks().debug_island_extract_diff(lo, hi)
		measured[key] = [bool(d.get("ok", false)), int(d.get("worst_steps", -1)),
				int(d.get("mat_mismatch", -1)), int(d.get("gpu_solid", -1)),
				int(d.get("cpu_solid", -1)), int(d.get("boxes", -1))]
	check("EXTRACT_GOLDEN", measured, EXTRACT_GOLDEN)

# The CPU raycast behind debug_raycast (and so edit_tool/hud): a fan over plain terrain, an
# unconsolidated carve, a consolidated carve, the analytic cave, a pasted volume and a miss.
func test_raycasts_are_pinned(timeout := 300000) -> void:
	var w := make_golden_world()
	w.hooks().debug_stream_region(Vector3i(0, 2, 0))
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.hooks().debug_apply_sphere_subtract(Vector3(12.8, 55.2, 12.8), 3.0)
	store_ball(w, Vector3(39.225, 64.425, 39.225))
	var rays := [
		[Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0)],
		[Vector3(27.0, 70.0, 24.4), Vector3(0, -1, 0)],
		[Vector3(12.8, 70.0, 12.8), Vector3(0, -1, 0)],
		[Vector3(6.4, 70.0, 20.0), Vector3(0, -1, 0)],
		[Vector3(30.0, 70.0, 30.0), Vector3(0, -1, 0)],
		[Vector3(40.8, 80.0, 40.8), Vector3(0, -1, 0)],
		[Vector3(20.0, 70.0, 20.0), Vector3(0.3, -1.0, 0.2)],
		[Vector3(5.0, 60.0, 5.0), Vector3(1.0, -0.4, 0.6)],
		[Vector3(10.0, 80.0, 10.0), Vector3(0, 1, 0)],
	]
	var out := []
	for r in rays:
		var h: Dictionary = w.hooks().debug_raycast(r[0], r[1])
		if not h["hit"]:
			out.append([false])
			continue
		out.append([true, v3(h["pos"]), v3(h["normal"]), snappedf(float(h["distance"]), 0.001),
				int(h["material"])])
	check("RAY_GOLDEN", {"rays": out}, RAY_GOLDEN)
```

- [ ] **Step 2: Record the goldens**

Run: `./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd`
Expected: all four FAIL with `no golden recorded`. Copy each printed `COLLIDER_GOLDEN {...}`, `CONTACT_GOLDEN {...}`, `EXTRACT_GOLDEN {...}`, `RAY_GOLDEN {...}` JSON (from the Godot log in `reports/report_N/`) into the matching `const` (JSON dictionaries, arrays, numbers and booleans are valid GDScript literals).

Sanity-check before pasting: `COLLIDER_GOLDEN.fill` and `.volume` each contain at least one `true`; `EXTRACT_GOLDEN.inside[0]` and `.border[0]` are `true`; `RAY_GOLDEN.rays` has hits for the first eight rays and `[false]` for the last. If not, fix the fixture, not production code.

- [ ] **Step 3: Run the pins**

Run: `./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd`
Expected: all four PASS.

- [ ] **Step 4: Prove each pin bites**

For each row, make the break, rebuild, run the suite, confirm the named case FAILS with `moved`, then restore with `git checkout -- <file>`:

| Break | File | Case that must fail |
|---|---|---|
| `LogProbe::chunk_has_surface` passes `nullptr` instead of `overrides` | `extension/src/physics/collider_streamer.cpp` | `test_collider_residency_is_pinned` |
| `IslandManager::contact_samples` passes `nullptr` instead of `handles_.store->overrides()` | `extension/src/physics/island_manager.cpp` | `test_contact_samples_are_pinned` |
| `debug_island_extract_diff`'s CPU `extract_island_volume` passes `nullptr` instead of the overrides | `extension/src/debug/hooks_physics.cpp` | `test_island_extract_is_pinned` |
| `debug_raycast` passes `nullptr` instead of `overrides()` | `extension/src/debug/hooks_world.cpp` | `test_raycasts_are_pinned` |

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd
git checkout -- <file>
```

After the last restore: `git status --short extension/src` must be empty; rebuild once more and re-run Step 3 (PASS).

- [ ] **Step 5: Record the change-cost "before" trace**

Append to `docs/superpowers/plans/2026-09-16-world-field-query-baseline.md`:

```markdown
## Change cost before 5a (spec §7, plan decision 10)

Scenario: "a new downward ground query in a consumer that already holds the store".
Traced from `IslandManager`'s merge-ground gate (`git log -S raycast_down --oneline`):
- `extension/src/core/world_store.h` — declaration of `WorldStore::raycast_down`
- `extension/src/core/world_store.cpp` — lock, generator, log, volumes, overrides, ray
- `extension/src/physics/island_manager.cpp` — the call
Files: 3. The query had to be written in the store because no module assembled the field
under the lock for a consumer.
```

- [ ] **Step 6: Commit**

```bash
git add tests/test_world_field_consumers.gd docs/superpowers/plans/2026-09-16-world-field-query-baseline.md
git commit -m "test: pin every world-field consumer before the WorldField move

Collider residency around a consolidated fill and a pasted volume, contact
samples around a consolidated carve, island extract inside and across a
consolidated region, and a raycast fan. Each pin was shown to fail when its
consumer drops the override source. Records the change-cost trace."
```

---

### Task 6: `WorldField` and `FieldView`

The module lands with its native contract and no callers, together with the one interface change its `ContactProbe` implementation needs.

**Files:**
- Create: `extension/src/world/world_field.h`, `extension/src/world/world_field.cpp`
- Create: `extension/tests/override_bake.h`, `extension/tests/test_world_field.cpp`
- Modify: `extension/tests/test_override_store.cpp` (use the shared bake)
- Modify: `extension/src/connectivity/contact_refine.h`, `extension/src/connectivity/contact_refine.cpp`, `extension/tests/test_contact_refine.cpp`
- Modify: `extension/src/physics/island_manager.cpp` (`LogContactProbe` forwards the new argument)
- Modify: `extension/src/core/world_store.h` (`field()`)

**Interfaces:**
- Consumes: `ve::eval_field`, `ve::chunk_has_surface`, `ve::contact_samples_field`, `ve::raycast`, `ve::collect_ops_for_aabb`, `ve::plan_consolidation`, `FieldSourceSnapshot::materialize`.
- Produces (exact):

```cpp
namespace ve {
using OverrideTableMap = std::map<std::tuple<int, int, int>, int>;
struct FieldSnapshot { std::vector<EditOp> ops; FieldSourceSnapshot sources; int override_table = -1; int64_t edit_seq = 0; bool over_cap = false; };
struct RegionSnapshot { std::vector<EditOp> ops; uint64_t through_seq = 0; std::vector<IVec3> bricks; FieldSourceSnapshot sources; };
struct SnapshotSources { explicit SnapshotSources(const FieldSourceSnapshot &s); OverrideStore overrides; VolumeSet volumes; bool ok = false; };
class FieldView {
public:
	bool valid() const;
	Sample sample(float x, float y, float z) const;
	bool has_surface(IVec3 chunk) const;
	int contact_samples(IVec3 cell, int axis, int face_samples) const;
	RayHit raycast(const float origin[3], const float dir[3], float max_dist) const;
	bool snapshot_lattice(const float ops_lo[3], const float ops_hi[3], const float origin[3], float voxel, int dim, FieldSnapshot *out) const;
	bool snapshot_region(IVec3 region, RegionSnapshot *out) const;
};
class WorldField : public ChunkProbe, public ContactProbe {
public:
	WorldField() = default;
	WorldField(const Generator *gen, const EditLog *log, const VolumeSet *volumes, const OverrideStore *overrides, const OverrideTableMap *tables, std::mutex *edit_mutex, const std::atomic<int64_t> *edit_seq);
	bool valid() const;
	FieldView lock() const;
	FieldView locked_by_caller() const;
	bool chunk_has_surface(IVec3 chunk) const override;
	int contact_samples(IVec3 cell, int axis, int face_samples) const override;
};
}
// godot::WorldStore
ve::WorldField field();
// ve::ContactProbe
virtual int contact_samples(IVec3 cell, int axis, int face_samples) const = 0;
```

- [ ] **Step 1: Move the CPU bake helper into a shared test header**

Create `extension/tests/override_bake.h` holding the `bake` function from `test_override_store.cpp` verbatim, as `inline void bake_override(...)` in `namespace ve_test`:

```cpp
#pragma once
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "world/override_store.h"

namespace ve_test {

// Bake one brick's lattice out of the generator plus ops, exactly as the GPU pass will.
inline void bake_override(const ve::Generator &gen, const ve::EditOp *ops, int n, ve::IVec3 brick,
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

} // namespace ve_test
```

In `extension/tests/test_override_store.cpp`, delete the local `bake` function, add `#include "override_bake.h"`, and replace every `bake(` call with `ve_test::bake_override(`.

- [ ] **Step 2: Write the native contract**

Create `extension/tests/test_world_field.cpp`:

```cpp
#include <doctest/doctest.h>
#include "override_bake.h"
#include "connectivity/contact_refine.h"
#include "mesh/chunk_residency.h"
#include "mesh/mesh_chunk.h"
#include "world/world_field.h"
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

namespace {

// One encoded SDF step is 1.28 m / 255 ~= 5 mm; trilinear reconstruction between lattice
// points of a smooth carve adds at most about one more step.
constexpr float kQuantumTolerance = 2.0f * 1.28f / 255.0f;

ve::EditOp sphere_sub(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

// A world whose region (0, 2, 0) holds one carve, and its consolidated twin: the same carve
// baked into override bricks with the region's op list cleared.
struct Worlds {
	ve::AnalyticGenerator gen;
	ve::EditLog live_log, baked_log;
	ve::VolumeSet volumes;
	ve::OverrideStore no_overrides{1};
	ve::OverrideStore overrides{512};
	ve::OverrideTableMap tables;
	std::mutex live_mu, baked_mu;
	std::atomic<int64_t> seq{7};
	const ve::EditOp carve = sphere_sub(12.4f, 51.4f, 12.4f, 1.5f);

	Worlds() {
		live_log.append(carve);
		std::vector<ve::IVec3> bricks;
		ve::plan_consolidation(&carve, 1, {0, 2, 0}, &bricks);
		for (const ve::IVec3 &b : bricks)
			ve_test::bake_override(gen, &carve, 1, b, overrides.data(overrides.acquire(b)));
		tables[{0, 2, 0}] = 3;
	}
	ve::WorldField live() {
		return ve::WorldField(&gen, &live_log, &volumes, &no_overrides, &tables, &live_mu, &seq);
	}
	ve::WorldField baked() {
		return ve::WorldField(&gen, &baked_log, &volumes, &overrides, &tables, &baked_mu, &seq);
	}
};

} // namespace

TEST_CASE("a consolidated region answers every query as its unconsolidated twin") {
	Worlds w;
	const ve::FieldView live = w.live().lock();
	const ve::FieldView baked = w.baked().lock();
	for (float dx = -2.0f; dx <= 2.0f; dx += 0.35f)
		for (float dy = -2.0f; dy <= 2.0f; dy += 0.35f) {
			const float x = 12.4f + dx, y = 51.4f + dy, z = 12.4f + 0.2f;
			const float want = ve::decode_sdf(ve::encode_sdf(live.sample(x, y, z).sdf));
			CHECK(std::fabs(baked.sample(x, y, z).sdf - want) <= kQuantumTolerance);
		}
	const ve::IVec3 chunk = ve::chunk_of_point(12.4f, 51.4f, 12.4f);
	CHECK(baked.has_surface(chunk) == live.has_surface(chunk));
	// The occupancy cell holding the carve's centre: its +y face is inside the pocket.
	const ve::IVec3 cell = ve::brick_of_point(12.4f, 51.4f, 12.4f);
	for (int axis = 0; axis < 3; axis++)
		CHECK(std::abs(baked.contact_samples(cell, axis, 9) - live.contact_samples(cell, axis, 9)) <= 2);
	const float o[3] = {12.4f, 70.0f, 12.4f};
	const float down[3] = {0.0f, -1.0f, 0.0f};
	const ve::RayHit a = live.raycast(o, down, 200.0f);
	const ve::RayHit b = baked.raycast(o, down, 200.0f);
	REQUIRE(a.hit);
	REQUIRE(b.hit);
	CHECK(std::fabs(a.pos[1] - b.pos[1]) <= ve::kVoxelSize);
}

TEST_CASE("a point query evaluates the op list of the region containing it") {
	ve::AnalyticGenerator gen;
	ve::EditLog log;
	ve::VolumeSet volumes;
	ve::OverrideTableMap tables;
	std::mutex mu;
	// A carve deep inside region (1, 2, 0), far from every border and every pad: only that
	// region's list holds it.
	const ve::EditOp far = sphere_sub(38.4f, 51.2f, 12.8f, 1.0f);
	log.append(far);
	REQUIRE(log.op_count({1, 2, 0}) == 1);
	REQUIRE(log.op_count({0, 2, 0}) == 0);
	const ve::FieldView v = ve::WorldField(&gen, &log, &volumes, nullptr, &tables, &mu, nullptr).lock();
	CHECK(v.sample(38.4f, 51.2f, 12.8f).sdf ==
			doctest::Approx(ve::eval_field(gen, &far, 1, 38.4f, 51.2f, 12.8f).sdf));
	CHECK(v.sample(12.8f, 51.2f, 12.8f).sdf == doctest::Approx(gen.sample(12.8f, 51.2f, 12.8f).sdf));
}

TEST_CASE("a lattice snapshot copies ops, overrides, seq and table") {
	Worlds w;
	ve::FieldSnapshot s;
	const float lo[3] = {12.0f, 51.0f, 12.0f}, hi[3] = {12.8f, 51.8f, 12.8f};
	const float origin[3] = {11.6f, 50.6f, 11.6f};
	{
		const ve::FieldView v = w.live().lock();
		REQUIRE(v.snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	}
	CHECK(s.ops.size() == 1);
	CHECK_FALSE(s.over_cap);
	CHECK(s.edit_seq == 7);
	CHECK(s.override_table == 3);
	CHECK(s.sources.overrides.empty()); // the live twin has no overrides
	{
		const ve::FieldView v = w.baked().lock();
		REQUIRE(v.snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	}
	CHECK(s.ops.empty());
	CHECK_FALSE(s.sources.overrides.empty());
}

TEST_CASE("a lattice snapshot across two regions reports the op cap and never truncates") {
	ve::AnalyticGenerator gen;
	ve::EditLog log;
	ve::VolumeSet volumes;
	ve::OverrideStore overrides(1);
	ve::OverrideTableMap tables;
	std::mutex mu;
	// A region holds at most kMaxRegionOps, so the cap is only reachable across a border:
	// 200 ops at x = 23.6 live in region 0 only, the rest at x = 27.6 in region 1 only.
	for (int i = 0; i < 200; i++) log.append(sphere_sub(23.6f, 51.2f + 0.01f * i, 12.8f, 0.1f));
	for (int i = 0; i < ve::kMaxRegionOps - 200; i++)
		log.append(sphere_sub(27.6f, 51.2f + 0.01f * i, 12.8f, 0.1f));
	const float lo[3] = {23.0f, 51.0f, 12.4f}, hi[3] = {28.2f, 54.0f, 13.2f};
	const float origin[3] = {23.0f, 51.0f, 12.4f};
	const ve::WorldField field(&gen, &log, &volumes, &overrides, &tables, &mu, nullptr);
	ve::FieldSnapshot s;
	REQUIRE(field.lock().snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	CHECK(s.ops.size() == static_cast<size_t>(ve::kMaxRegionOps));
	CHECK_FALSE(s.over_cap);
	log.append(sphere_sub(27.6f, 53.9f, 12.8f, 0.1f));
	REQUIRE(field.lock().snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	CHECK(s.ops.size() == static_cast<size_t>(ve::kMaxRegionOps + 1));
	CHECK(s.over_cap);
}

TEST_CASE("a region snapshot is the consolidation job's inputs") {
	Worlds w;
	ve::RegionSnapshot s;
	{
		const ve::FieldView v = w.live().lock();
		REQUIRE(v.snapshot_region({0, 2, 0}, &s));
	}
	REQUIRE(s.ops.size() == 1);
	CHECK(s.through_seq == w.live_log.seqs({0, 2, 0}).back());
	std::vector<ve::IVec3> planned;
	ve::plan_consolidation(&w.carve, 1, {0, 2, 0}, &planned);
	CHECK(s.bricks.size() == planned.size());
}

TEST_CASE("snapshot sources evaluate exactly as the live store they were copied from") {
	Worlds w;
	ve::FieldSnapshot s;
	const float lo[3] = {12.0f, 51.0f, 12.0f}, hi[3] = {12.8f, 51.8f, 12.8f};
	const float origin[3] = {11.6f, 50.6f, 11.6f};
	const ve::FieldView v = w.baked().lock();
	REQUIRE(v.snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	const ve::SnapshotSources sources(s.sources);
	REQUIRE(sources.ok);
	const ve::Sample a = ve::eval_field(w.gen, s.ops.data(), static_cast<int>(s.ops.size()),
			12.4f, 51.4f, 12.4f, &sources.volumes, &sources.overrides);
	CHECK(a.sdf == doctest::Approx(v.sample(12.4f, 51.4f, 12.4f).sdf));
}

TEST_CASE("an invalid field answers every query with its empty value") {
	const ve::WorldField none;
	CHECK_FALSE(none.valid());
	const ve::FieldView v = none.lock();
	CHECK_FALSE(v.valid());
	CHECK(v.sample(0.0f, 0.0f, 0.0f).sdf == 0.0f);
	CHECK_FALSE(v.has_surface({0, 0, 0}));
	CHECK(v.contact_samples({0, 0, 0}, 1, 9) == 0);
	const float o[3] = {0.0f, 0.0f, 0.0f}, d[3] = {0.0f, -1.0f, 0.0f};
	CHECK_FALSE(v.raycast(o, d, 10.0f).hit);
	ve::FieldSnapshot fs;
	CHECK_FALSE(v.snapshot_lattice(o, o, o, 0.05f, 2, &fs));
	ve::RegionSnapshot rs;
	CHECK_FALSE(v.snapshot_region({0, 0, 0}, &rs));
	CHECK_FALSE(none.chunk_has_surface({0, 0, 0}));
	CHECK(none.contact_samples({0, 0, 0}, 1, 9) == 0);
}

TEST_CASE("the probe interfaces answer as the free functions over the same sources") {
	Worlds w;
	const ve::WorldField field = w.baked();
	const ve::IVec3 chunk = ve::chunk_of_point(12.4f, 51.4f, 12.4f);
	const std::vector<ve::EditOp> &ops = w.baked_log.ops(ve::region_of_chunk(chunk));
	CHECK(static_cast<const ve::ChunkProbe &>(field).chunk_has_surface(chunk) ==
			ve::chunk_has_surface(w.gen, ops.data(), static_cast<int>(ops.size()), chunk,
					&w.volumes, &w.overrides));
	const ve::IVec3 cell = ve::brick_of_point(12.4f, 51.4f, 12.4f);
	CHECK(static_cast<const ve::ContactProbe &>(field).contact_samples(cell, 1, 9) ==
			ve::contact_samples_field(w.gen, ops.data(), static_cast<int>(ops.size()), cell, 1, 9,
					&w.volumes, &w.overrides));
}
```

- [ ] **Step 3: Run it to verify it fails to compile**

Run: `cd extension && scons -Q test; cd ..`
Expected: FAIL, `world/world_field.h` not found.

- [ ] **Step 4: Give `ContactProbe` the face sample count**

In `extension/src/connectivity/contact_refine.h`, change the probe to:

```cpp
struct ContactProbe {
	virtual ~ContactProbe() = default;
	// Solid samples on the 0.8 m face between `cell` and `cell + e_axis`, of face_samples^2.
	virtual int contact_samples(IVec3 cell, int axis, int face_samples) const = 0;
};
```

In `contact_refine.cpp` `refine_anchoring`, change the call to `probe.contact_samples(b.cell, b.axis, cfg.face_samples)`.

In `extension/tests/test_contact_refine.cpp` `ScriptedProbe`, change the override to `int contact_samples(IVec3 c, int axis, int) const override {`.

In `extension/src/physics/island_manager.cpp` `LogContactProbe`, change the override to:

```cpp
	int contact_samples(ve::IVec3 cell, int axis, int) const override {
		return manager->contact_samples(cell, axis);
	}
```

- [ ] **Step 5: Write the header**

Create `extension/src/world/world_field.h`:

```cpp
#pragma once
// ve::WorldField -- the one answer to "the world field here" (sub-project 5a,
// docs/superpowers/specs/2026-09-16-world-field-query-design.md). It hides which op list
// governs a point, the override and volume sources, the edit lock and the sequence stamp.
// A FieldView holds the edit mutex for its lifetime; snapshot_* are the only copying
// products, for GPU jobs and CPU references that run after the lock is released.
#include "connectivity/contact_refine.h"
#include "generator/volume_set.h"
#include "mesh/chunk_residency.h"
#include "world/edit_log.h"
#include "world/field_source_snapshot.h"
#include "world/override_store.h"
#include "world/raycast.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace ve {

using OverrideTableMap = std::map<std::tuple<int, int, int>, int>;

// A lattice job's inputs: ops over the job's box AABB (not truncated), the override bricks
// and referenced volumes over the lattice's brick range, the table of the lattice origin's
// region and the edit sequence they were read at.
struct FieldSnapshot {
	std::vector<EditOp> ops;
	FieldSourceSnapshot sources;
	int override_table = -1;
	int64_t edit_seq = 0;
	bool over_cap = false; // ops.size() > kMaxRegionOps; the caller decides
};

// A consolidation bake's inputs.
struct RegionSnapshot {
	std::vector<EditOp> ops;
	uint64_t through_seq = 0;
	std::vector<IVec3> bricks; // plan_consolidation order
	FieldSourceSnapshot sources;
};

// A snapshot made evaluable: what a GPU job received, as CPU stores.
struct SnapshotSources {
	explicit SnapshotSources(const FieldSourceSnapshot &s);
	OverrideStore overrides;
	VolumeSet volumes;
	bool ok = false;
};

class WorldField;

class FieldView {
public:
	FieldView(FieldView &&) = default;
	FieldView &operator=(FieldView &&) = default;
	FieldView(const FieldView &) = delete;
	FieldView &operator=(const FieldView &) = delete;

	bool valid() const { return gen_ != nullptr && log_ != nullptr; }
	Sample sample(float x, float y, float z) const;
	bool has_surface(IVec3 chunk) const;
	int contact_samples(IVec3 cell, int axis, int face_samples) const;
	RayHit raycast(const float origin[3], const float dir[3], float max_dist) const;
	// ops over [ops_lo, ops_hi] + kLatticeFilterPad; sources over the lattice
	// origin .. origin + (dim - 1) * voxel. False where the old snapshot_field_sources was.
	bool snapshot_lattice(const float ops_lo[3], const float ops_hi[3], const float origin[3],
			float voxel, int dim, FieldSnapshot *out) const;
	bool snapshot_region(IVec3 region, RegionSnapshot *out) const;

private:
	friend class WorldField;
	FieldView() = default;
	bool copy_sources(const std::vector<EditOp> &ops, IVec3 brick_lo, IVec3 brick_hi,
			FieldSourceSnapshot *out) const;

	const Generator *gen_ = nullptr;
	const EditLog *log_ = nullptr;
	const VolumeSet *volumes_ = nullptr;
	const OverrideStore *overrides_ = nullptr;
	const OverrideTableMap *tables_ = nullptr;
	const std::atomic<int64_t> *seq_ = nullptr;
	std::unique_lock<std::mutex> lock_;
};

class WorldField : public ChunkProbe, public ContactProbe {
public:
	WorldField() = default;
	WorldField(const Generator *gen, const EditLog *log, const VolumeSet *volumes,
			const OverrideStore *overrides, const OverrideTableMap *tables, std::mutex *edit_mutex,
			const std::atomic<int64_t> *edit_seq)
		: gen_(gen), log_(log), volumes_(volumes), overrides_(overrides), tables_(tables),
		  mutex_(edit_mutex), seq_(edit_seq) {}

	bool valid() const { return gen_ != nullptr && log_ != nullptr && volumes_ != nullptr && mutex_ != nullptr; }
	// Takes the edit mutex for the view's lifetime. An invalid field returns an invalid,
	// unlocked view.
	FieldView lock() const;
	// For code that already holds the edit mutex across more than this read.
	FieldView locked_by_caller() const;

	// Probe interfaces: one lock per call, as the adapters they replace.
	bool chunk_has_surface(IVec3 chunk) const override;
	int contact_samples(IVec3 cell, int axis, int face_samples) const override;

private:
	FieldView view() const;

	const Generator *gen_ = nullptr;
	const EditLog *log_ = nullptr;
	const VolumeSet *volumes_ = nullptr;
	const OverrideStore *overrides_ = nullptr;
	const OverrideTableMap *tables_ = nullptr;
	std::mutex *mutex_ = nullptr;
	const std::atomic<int64_t> *seq_ = nullptr;
};

} // namespace ve
```

- [ ] **Step 6: Write the implementation**

Create `extension/src/world/world_field.cpp`:

```cpp
#include "world/world_field.h"
#include "mesh/mesh_chunk.h"
#include "world/brick_eval.h"
#include <algorithm>
#include <set>

namespace ve {

SnapshotSources::SnapshotSources(const FieldSourceSnapshot &s)
	: overrides(std::max(1, static_cast<int>(s.overrides.size()))) {
	ok = s.materialize(&overrides, &volumes);
}

FieldView WorldField::view() const {
	FieldView v;
	if (!valid()) return v;
	v.gen_ = gen_;
	v.log_ = log_;
	v.volumes_ = volumes_;
	v.overrides_ = overrides_;
	v.tables_ = tables_;
	v.seq_ = seq_;
	return v;
}

FieldView WorldField::lock() const {
	FieldView v = view();
	if (v.valid()) v.lock_ = std::unique_lock<std::mutex>(*mutex_);
	return v;
}

FieldView WorldField::locked_by_caller() const {
	return view();
}

bool WorldField::chunk_has_surface(IVec3 chunk) const {
	return lock().has_surface(chunk);
}

int WorldField::contact_samples(IVec3 cell, int axis, int face_samples) const {
	return lock().contact_samples(cell, axis, face_samples);
}

Sample FieldView::sample(float x, float y, float z) const {
	if (!valid()) return {};
	const std::vector<EditOp> &ops = log_->ops(region_of_point(x, y, z));
	return eval_field(*gen_, ops.data(), static_cast<int>(ops.size()), x, y, z, volumes_, overrides_);
}

bool FieldView::has_surface(IVec3 chunk) const {
	if (!valid()) return false;
	const std::vector<EditOp> &ops = log_->ops(region_of_chunk(chunk));
	return chunk_has_surface(*gen_, ops.data(), static_cast<int>(ops.size()), chunk, volumes_,
			overrides_);
}

int FieldView::contact_samples(IVec3 cell, int axis, int face_samples) const {
	if (!valid()) return 0;
	const std::vector<EditOp> &ops = log_->ops(region_of_brick(cell));
	return contact_samples_field(*gen_, ops.data(), static_cast<int>(ops.size()), cell, axis,
			face_samples, volumes_, overrides_);
}

RayHit FieldView::raycast(const float origin[3], const float dir[3], float max_dist) const {
	if (!valid()) return {};
	return ve::raycast(*gen_, *log_, origin, dir, max_dist, volumes_, overrides_);
}

// Moved verbatim from WorldStore::snapshot_field_sources (sub-project 5a).
bool FieldView::copy_sources(const std::vector<EditOp> &ops, IVec3 brick_lo, IVec3 brick_hi,
		FieldSourceSnapshot *out) const {
	if (!out || !overrides_) return false;
	out->overrides.clear();
	out->volumes.clear();
	for (int z = brick_lo.z; z <= brick_hi.z; z++)
		for (int y = brick_lo.y; y <= brick_hi.y; y++)
			for (int x = brick_lo.x; x <= brick_hi.x; x++) {
				IVec3 b{x, y, z};
				int slot = overrides_->slot_of(b);
				if (slot >= 0) {
					const OverrideBrick *data = overrides_->data(slot);
					if (!data) return false;
					if (!data->normal_oct.empty() && data->normal_oct.size() != kBrickSdfCount) return false;
					out->overrides.push_back({b, *data});
				}
			}
	std::set<int> seen;
	for (const auto &op : ops) {
		if (op.type != kOpVolumeAdd) continue;
		int slot = static_cast<int>(op.aux[0]);
		if (seen.count(slot)) continue;
		seen.insert(slot);
		const VolumeData *vd = volumes_->get(slot);
		if (!vd || !vd->valid()) return false;
		out->volumes.push_back({slot, *vd});
	}
	return true;
}

bool FieldView::snapshot_lattice(const float ops_lo[3], const float ops_hi[3],
		const float origin[3], float voxel, int dim, FieldSnapshot *out) const {
	if (!out) return false;
	*out = FieldSnapshot{};
	if (!valid()) return false;
	collect_ops_for_aabb(*log_, ops_lo, ops_hi, &out->ops);
	out->over_cap = out->ops.size() > static_cast<size_t>(kMaxRegionOps);
	out->edit_seq = seq_ ? seq_->load(std::memory_order_relaxed) : 0;
	if (tables_) {
		const IVec3 r = region_of_point(origin[0], origin[1], origin[2]);
		const auto it = tables_->find(std::tuple<int, int, int>{r.x, r.y, r.z});
		out->override_table = it == tables_->end() ? -1 : it->second;
	}
	const float span = static_cast<float>(dim - 1) * voxel;
	const IVec3 blo = brick_of_point(origin[0], origin[1], origin[2]);
	const IVec3 bhi = brick_of_point(origin[0] + span, origin[1] + span, origin[2] + span);
	return copy_sources(out->ops, blo, bhi, &out->sources);
}

bool FieldView::snapshot_region(IVec3 region, RegionSnapshot *out) const {
	if (!out) return false;
	*out = RegionSnapshot{};
	if (!valid() || !overrides_) return false;
	out->ops = log_->ops(region);
	const std::vector<uint64_t> &seqs = log_->seqs(region);
	out->through_seq = seqs.empty() ? 0 : seqs.back();
	plan_consolidation(out->ops.data(), static_cast<int>(out->ops.size()), region, &out->bricks);
	if (out->bricks.empty()) return true;
	IVec3 lo = out->bricks[0], hi = out->bricks[0];
	for (const IVec3 &b : out->bricks) {
		lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z);
		hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z);
	}
	return copy_sources(out->ops, lo, hi, &out->sources);
}

} // namespace ve
```

The float `span` form of the lattice's upper corner is exactly the old call sites' `lattice_hi` (`origin + (dim - 1) * voxel` per axis).

- [ ] **Step 7: `WorldStore::field()`**

In `extension/src/core/world_store.h`, add `#include "world/world_field.h"` and, below `ve::RayHit raycast_down(const float xz[2]);`:

```cpp
	// The world field over this store's current cores. A cheap value: re-fetch it per use,
	// because the edit log and override store are created lazily and released at exit.
	ve::WorldField field() {
		return ve::WorldField(generator_ ? &generator_->sampler() : nullptr, edit_log_, &volumes_,
				overrides_, &override_tables_, &edit_mutex_, &edit_seq_);
	}
```

- [ ] **Step 8: Build and run the native suite**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS, including every `test_world_field.cpp` case and the unchanged `test_override_store.cpp` and `test_contact_refine.cpp` cases.

If "a lattice snapshot across two regions…" fails its `REQUIRE`s on op counts, print `log.op_count({0, 2, 0})` and `log.op_count({1, 2, 0})`: an op must land in exactly one region list, so move the two x columns further from x = 25.6 rather than changing the assertion.

- [ ] **Step 9: Build the extension and run the contact suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd,res://tests/test_world_field_overrides.gd,res://tests/test_connectivity.gd
```

Expected: PASS; pins unchanged.

- [ ] **Step 10: Commit**

```bash
git add extension/src/world/world_field.h extension/src/world/world_field.cpp extension/tests/override_bake.h extension/tests/test_world_field.cpp extension/tests/test_override_store.cpp extension/src/connectivity/contact_refine.h extension/src/connectivity/contact_refine.cpp extension/tests/test_contact_refine.cpp extension/src/physics/island_manager.cpp extension/src/core/world_store.h
git commit -m "feat: WorldField answers the world field under the edit lock

A pure module: point, chunk, contact and ray queries read each point's region
list with generator, volumes and overrides always passed; lattice and region
snapshots are the only copying products. It implements ChunkProbe and
ContactProbe, whose contact query now receives the face sample count. No
consumer uses it yet."
```

---

### Task 7: Colliders ask the field (row 1)

**Files:**
- Modify: `extension/src/physics/collider_streamer.h`, `extension/src/physics/collider_streamer.cpp`
- Modify: `extension/src/voxel_world.cpp` (`ensure_physics_initialized`)

**Interfaces:**
- Consumes: `WorldStore::field()`, `ve::WorldField` as `ve::ChunkProbe`.
- Produces: `void ColliderStreamer::initialize(ve::ChunkResidency *chunks, ve::EditLog *edit_log, std::mutex *edit_mutex, MeshService *mesh, int max_slots, ve::WorldField field)`.

- [ ] **Step 1: Replace the probe inputs with a field**

In `extension/src/physics/collider_streamer.h`, add `#include "world/world_field.h"`; change `initialize` to:

```cpp
	void initialize(ve::ChunkResidency *chunks, ve::EditLog *edit_log, std::mutex *edit_mutex,
			MeshService *mesh, int max_slots, ve::WorldField field);
```

Delete the `gen_`, `volumes_` and `overrides_` members with their comments, and in their place add:

```cpp
	// The residency probe's view of the world (sub-project 5a). Built by WorldStore::field()
	// at initialize, where the store's cores already exist, and valid until teardown -- the
	// lifetime the raw generator/volume/override pointers had. edit_log_/edit_mutex_ stay for
	// the mesh request's op copy, which sub-project 5b moves.
	ve::WorldField field_;
```

Remove any `namespace ve { struct OverrideSource; }` forward declaration that is now unused.

In `extension/src/physics/collider_streamer.cpp`, delete `struct LogProbe` with its comment. Change the `initialize` definition's signature to match; replace the three lines `gen_ = gen; volumes_ = volumes; overrides_ = overrides;` with `field_ = field;`. In `teardown`, replace `gen_ = nullptr;` with `field_ = ve::WorldField();`. Replace `if (gen_ == nullptr) return 0;` with `if (!field_.valid()) return 0;`. Replace the probe construction block

```cpp
	LogProbe probe;
	probe.gen = gen_;
	probe.log = edit_log_;
	probe.mu = edit_mutex_;
	probe.volumes = volumes_;
	probe.overrides = overrides_;
```

with nothing, and the `update` call's `probe` argument with `field_`.

- [ ] **Step 2: Wire the field**

In `extension/src/voxel_world.cpp` `ensure_physics_initialized`, the collider initialisation becomes:

```cpp
	colliders_->initialize(chunks_, store_->edit_log(), &store_->edit_mutex(), mesh_,
			max_collider_chunks_, store_->field());
```

- [ ] **Step 3: Build and run the collider pins**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd,res://tests/test_world_field_overrides.gd,res://tests/test_collider_edits.gd,res://tests/test_collider_stream.gd,res://tests/test_collider_octants.gd
rg 'struct LogProbe' extension/src
```

Expected: every case PASSES with pins unchanged; `rg` prints nothing.

- [ ] **Step 4: Commit**

```bash
git add extension/src/physics/collider_streamer.h extension/src/physics/collider_streamer.cpp extension/src/voxel_world.cpp
git commit -m "refactor: collider residency probes the WorldField

ColliderStreamer holds the field instead of the generator, volume and override
pointers, and ChunkResidency asks it directly. LogProbe is deleted."
```

---

### Task 8: Contact refinement asks the field (row 2)

**Files:**
- Modify: `extension/src/physics/island_manager.h`, `extension/src/physics/island_manager.cpp`
- Modify: `extension/src/debug/hooks_physics.cpp` (`debug_contact_samples`)

**Interfaces:**
- Consumes: `WorldStore::field()`, `ve::WorldField` as `ve::ContactProbe`.
- Produces: `const ve::ContactRefineConfig &IslandManager::refine_config() const`. Deleted: `IslandManager::contact_samples`, `LogContactProbe`.

- [ ] **Step 1: Refine against the field**

In `extension/src/physics/island_manager.h`, delete the `contact_samples` declaration and its comment; add in the public section:

```cpp
	// The marginal-contact refinement's tuning; debug_contact_samples asks the field with the
	// same face sample count the refinement uses.
	const ve::ContactRefineConfig &refine_config() const { return refine_cfg_; }
```

In `extension/src/physics/island_manager.cpp`, delete `struct LogContactProbe` with its comment and the `IslandManager::contact_samples` definition. In the connectivity run, replace

```cpp
	LogContactProbe probe;
	probe.manager = this;
```

with

```cpp
	// The field locks once per contact query, so an edit landing mid-refinement waits rather
	// than deadlocks.
	const ve::WorldField field = handles_.store->field();
```

and `ve::refine_anchoring(handles_.store->occupancy(), probe, refine_cfg_, &cuts, &r);` with `ve::refine_anchoring(handles_.store->occupancy(), field, refine_cfg_, &cuts, &r);`.

- [ ] **Step 2: The hook asks the same field**

In `extension/src/debug/hooks_physics.cpp`, replace `debug_contact_samples` and its comment with:

```cpp
// The shipped marginal-contact query: refine_anchoring asks WorldStore::field() with the
// manager's face sample count, and so does this.
int VoxelDebugHooks::debug_contact_samples(Vector3i cell, int axis) {
	world_->ensure_physics_initialized();
	if (!world_->island_manager()) return -1;
	return world_->context().store->field().contact_samples({cell.x, cell.y, cell.z}, axis,
			world_->island_manager()->refine_config().face_samples);
}
```

- [ ] **Step 3: Build and run the contact pins**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd,res://tests/test_world_field_overrides.gd,res://tests/test_connectivity.gd
rg 'struct LogContactProbe|IslandManager::contact_samples' extension/src
```

Expected: PASS with pins unchanged (compare `test_connectivity` by failure count); `rg` prints nothing.

- [ ] **Step 4: Commit**

```bash
git add extension/src/physics/island_manager.h extension/src/physics/island_manager.cpp extension/src/debug/hooks_physics.cpp
git commit -m "refactor: contact refinement probes the WorldField

refine_anchoring takes the field as its ContactProbe; LogContactProbe and
IslandManager::contact_samples are deleted, and debug_contact_samples asks the
same field with the manager's face sample count."
```

---

### Task 9: Island extract job inputs come from one snapshot (row 3)

**Files:**
- Modify: `extension/src/physics/island_manager.cpp` (the extraction job loop)
- Modify: `extension/src/voxel_world.cpp` (`extract_component`)
- Modify: `extension/src/debug/hooks_physics.cpp` (`debug_island_extract_diff`, `debug_extract_submit`)

**Interfaces:**
- Consumes: `FieldView::snapshot_lattice`, `ve::FieldSnapshot`.
- Produces: no new names; four hand-built input blocks become one call each.

- [ ] **Step 1: `IslandManager`**

In the extraction loop, replace the block from `{\n\t\t\tstd::lock_guard<std::mutex> lock(handles_.store->edit_mutex());` through the `refused_op_cap_` check that follows `job.override_table = …` with:

```cpp
		ve::FieldSnapshot snap;
		{
			const ve::FieldView view = handles_.store->field().lock();
			if (!view.valid()) {
				refused_++;
				continue;
			}
			if (!view.snapshot_lattice(wlo, whi, job.origin, job.voxel, job.dim, &snap)) {
				refused_++;
				refused_op_cap_++;
				continue;
			}
		}
		job.ops = std::move(snap.ops);
		job.snapshot = std::move(snap.sources);
		job.gen = gen_;
		// The table is captured under the same lock as the snapshot now; it used to be read
		// after the lock was released.
		job.override_table = snap.override_table;
		// Refuse before allocating a volume slot or submitting: the extraction pass cannot
		// evaluate more than kMaxRegionOps ops, so this component can never be carved by the
		// current field/worker limits. Fail-soft leaves it attached.
		if (snap.over_cap) {
			refused_++;
			refused_op_cap_++;
			continue;
		}
```

- [ ] **Step 2: `VoxelWorld::extract_component`**

Replace the two statements `job->override_table = store_->override_table_for_region(…);` and the `{ std::lock_guard … job->gen = &store_->generator()->sampler(); }` block with:

```cpp
	ve::FieldSnapshot snap;
	{
		const ve::FieldView view = store_->field().lock();
		if (!view.valid() || !view.snapshot_lattice(wlo, whi, job->origin, job->voxel, job->dim, &snap))
			return false;
	}
	job->ops = std::move(snap.ops);
	job->snapshot = std::move(snap.sources);
	job->override_table = snap.override_table;
	job->gen = &store_->generator()->sampler();
```

- [ ] **Step 3: The two hooks**

In `debug_island_extract_diff` and in `debug_extract_submit`, replace the `job.override_table = …;` statement and the following `{ std::lock_guard … job.gen = …; }` block with (using the local names `job`, `wlo`, `whi`; the hook returns `d` / `false` respectively on failure):

```cpp
	ve::FieldSnapshot snap;
	{
		const ve::FieldView view = world_->context().store->field().lock();
		if (!view.valid() || !view.snapshot_lattice(wlo, whi, job.origin, job.voxel, job.dim, &snap))
			return d; // `return false;` in debug_extract_submit
	}
	job.ops = std::move(snap.ops);
	job.snapshot = std::move(snap.sources);
	job.override_table = snap.override_table;
	job.gen = &world_->context().store->generator()->sampler();
```

- [ ] **Step 4: Build and run the extract pins**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd,res://tests/test_world_field_overrides.gd,res://tests/test_override_region_border.gd,res://tests/test_connectivity.gd,res://tests/test_island_body.gd,res://tests/test_island_render.gd
```

Expected: PASS with pins unchanged.

- [ ] **Step 5: Commit**

```bash
git add extension/src/physics/island_manager.cpp extension/src/voxel_world.cpp extension/src/debug/hooks_physics.cpp
git commit -m "refactor: island extract jobs take their inputs from one field snapshot

The four hand-built blocks (ops over the box AABB, lattice brick range, source
copy, origin region's table) become FieldView::snapshot_lattice. The table is
now read under the same lock as the snapshot."
```

---

### Task 10: The CPU extract reference evaluates the job's snapshot (row 4)

**Files:**
- Modify: `extension/src/voxel_world.cpp` (`extract_component`)
- Modify: `extension/src/debug/hooks_physics.cpp` (`debug_island_extract_diff`)

**Interfaces:**
- Consumes: `ve::SnapshotSources`.
- Produces: no new names.

- [ ] **Step 1: `VoxelWorld::extract_component`**

Replace the CPU extraction call

```cpp
	ve::extract_island_volume(gen, job->ops.data(), static_cast<int>(job->ops.size()),
			&store_->volumes(), store_->overrides(), job->origin, job->voxel, job->dim, aabbs.data(),
			static_cast<int>(boxes->size()), &cpu);
```

with

```cpp
	// The reference evaluates exactly what the GPU job received, not the live store, which
	// this used to read without the edit lock after the worker returned.
	const ve::SnapshotSources sources(job->snapshot);
	if (!sources.ok) return false;
	ve::extract_island_volume(gen, job->ops.data(), static_cast<int>(job->ops.size()),
			&sources.volumes, &sources.overrides, job->origin, job->voxel, job->dim, aabbs.data(),
			static_cast<int>(boxes->size()), &cpu);
```

- [ ] **Step 2: `debug_island_extract_diff`**

After the worker results are collected, before `std::vector<float> aabbs(...)`, add:

```cpp
	const ve::SnapshotSources sources(job.snapshot);
	if (!sources.ok) return d;
```

In the CPU `extract_island_volume` call, replace `&world_->context().store->volumes(), world_->context().store->overrides()` with `&sources.volumes, &sources.overrides`. In the normal-alignment loop's `eval_field_gradient` call, make the same replacement.

- [ ] **Step 3: Build and run the extract pins**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd,res://tests/test_world_field_overrides.gd,res://tests/test_override_region_border.gd,res://tests/test_island_body.gd
```

Expected: PASS with pins unchanged. A moved `EXTRACT_GOLDEN` means the live store and the job's snapshot disagreed — a race the old code hid (spec §10 risk 3): stop and report, do not re-pin.

- [ ] **Step 4: Commit**

```bash
git add extension/src/voxel_world.cpp extension/src/debug/hooks_physics.cpp
git commit -m "refactor: the CPU island extract reference evaluates the job's snapshot

It read live overrides and volumes without the edit lock after the GPU job
returned; it now materializes the snapshot the job was built from."
```

---

### Task 11: Raycasts ask the field (row 5)

**Files:**
- Modify: `extension/src/debug/hooks_world.cpp` (`debug_raycast`)
- Modify: `extension/src/physics/island_manager.h`, `extension/src/physics/island_manager.cpp`
- Modify: `extension/src/core/world_store.h`, `extension/src/core/world_store.cpp`

**Interfaces:**
- Consumes: `FieldView::raycast`.
- Produces: deleted `WorldStore::raycast_down`.

- [ ] **Step 1: `debug_raycast`**

Replace its body's lock/generator/raycast lines

```cpp
	if (!world_->context().store->edit_log()) return d;
	std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
	const ve::Generator &gen = world_->context().store->generator()->sampler();
	const float o[3] = {origin.x, origin.y, origin.z};
	const float f[3] = {dir.x, dir.y, dir.z};
	const ve::RayHit h = ve::raycast(gen, *world_->context().store->edit_log(), o, f, 200.0f, &world_->context().store->volumes(), world_->context().store->overrides());
```

with

```cpp
	const float o[3] = {origin.x, origin.y, origin.z};
	const float f[3] = {dir.x, dir.y, dir.z};
	const ve::RayHit h = world_->context().store->field().lock().raycast(o, f, 200.0f);
```

- [ ] **Step 2: `IslandManager`'s ground rays**

In `extension/src/physics/island_manager.cpp`'s anonymous namespace (after `same_rest_pose`), add:

```cpp
// A downward ray from 200 m, 400 m long: where the ground is under a sleeping body's
// footprint, read from the same field the paste goes into.
ve::RayHit ground_below(WorldStore *store, const float xz[2]) {
	const float origin[3] = {xz[0], 200.0f, xz[1]};
	const float down[3] = {0.0f, -1.0f, 0.0f};
	return store->field().lock().raycast(origin, down, 400.0f);
}
```

Replace `handles_.store->raycast_down(p)` with `ground_below(handles_.store, p)` and `handles_.store->raycast_down(last_merge_xz_)` with `ground_below(handles_.store, last_merge_xz_)`.

In `extension/src/physics/island_manager.h`, change the `store` collaborator comment's `field snapshots, raycast_down.` to `and the world field.`.

- [ ] **Step 3: Delete `raycast_down`**

Delete the declaration and comment from `extension/src/core/world_store.h` and the definition from `world_store.cpp`. If nothing else in `world_store.h` uses `ve::RayHit`, remove `#include "world/raycast.h"` (the field header already includes it).

- [ ] **Step 4: Build and run the ray pins**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_consumers.gd,res://tests/test_consolidation.gd,res://tests/test_island_body.gd,res://tests/test_connectivity.gd,res://tests/test_edit_pipeline.gd,res://tests/test_region_dda.gd
rg 'raycast_down' extension/src
```

Expected: PASS with pins unchanged; `rg` prints nothing.

- [ ] **Step 5: Commit**

```bash
git add extension/src/debug/hooks_world.cpp extension/src/physics/island_manager.h extension/src/physics/island_manager.cpp extension/src/core/world_store.h extension/src/core/world_store.cpp
git commit -m "refactor: raycasts ask the WorldField

debug_raycast and IslandManager's merge-ground rays take one field view;
WorldStore::raycast_down is deleted."
```

---

### Task 12: Consolidation inputs come from one region snapshot (row 6)

**Files:**
- Modify: `extension/src/mesh/consolidation.cpp` (`pump_async`, `force_region`)
- Modify: `extension/src/debug/hooks_world.cpp` (`debug_consolidate_diff`)
- Modify: `extension/src/core/world_store.h`, `extension/src/core/world_store.cpp`

**Interfaces:**
- Consumes: `WorldField::locked_by_caller`, `FieldView::snapshot_region`, `ve::RegionSnapshot`.
- Produces: deleted `WorldStore::snapshot_field_sources`.

- [ ] **Step 1: `pump_async`**

`pump_async` already holds `edit_lock` for the whole function. Replace the block from `const std::vector<ve::EditOp> &ops = store_->edit_log()->ops(region);` through the closing brace of `if (!job.bricks.empty()) { … }` with:

```cpp
	ve::RegionSnapshot snap;
	// edit_lock above spans this whole function, so the ops, overrides and volumes are read in
	// one consistent state.
	if (!store_->field().locked_by_caller().snapshot_region(region, &snap)) {
		consolidation_refusals_++;
		requeue(region);
		return;
	}
	if (snap.ops.empty()) return;
	ConsolidateJob job;
	job.region = region;
	job.region_slot = region_slot;
	job.ops = std::move(snap.ops);
	job.through_seq = snap.through_seq;
	job.gen = &store_->generator()->sampler();
	job.bricks = std::move(snap.bricks);
	job.source = std::move(snap.sources);
```

If later statements in `pump_async` still name the deleted `ops` local, point them at `job.ops`. Check the order against the old code: the old code returned on empty ops **before** snapshotting and refused on snapshot failure only when bricks were non-empty. `snapshot_region` returns true with empty bricks and empty ops, so an empty list still returns without a refusal; a snapshot failure can only occur when bricks are non-empty. Behaviour is unchanged.

- [ ] **Step 2: `force_region`**

`force_region` holds `edit_lock` from its first line. Replace

```cpp
	std::vector<ve::EditOp> ops = store_->edit_log()->ops(r);
	std::vector<ve::IVec3> bricks;
	ve::plan_consolidation(ops.data(), static_cast<int>(ops.size()), r, &bricks);
```

with

```cpp
	ve::RegionSnapshot snap;
	const bool sources_ok = store_->field().locked_by_caller().snapshot_region(r, &snap);
	std::vector<ve::EditOp> &ops = snap.ops;
	std::vector<ve::IVec3> &bricks = snap.bricks;
```

and replace the later

```cpp
	if (!bricks.empty()) {
		ve::IVec3 lo = bricks[0], hi = bricks[0];
		for (auto &b : bricks) { … }
		if (!store_->snapshot_field_sources(ops, lo, hi, &job.source)) return refuse();
	}
```

with

```cpp
	if (!bricks.empty()) {
		if (!sources_ok) return refuse();
		job.source = snap.sources;
	}
```

(`job.bricks = bricks; job.ops = ops;` copy from the references as before.)

- [ ] **Step 3: `debug_consolidate_diff`**

The hook holds `edit_lock` from its first line. Replace

```cpp
	std::vector<ve::EditOp> ops = world_->context().store->edit_log()->ops(r);
	std::vector<ve::IVec3> bricks;
	ve::plan_consolidation(ops.data(), static_cast<int>(ops.size()), r, &bricks);
```

with

```cpp
	ve::RegionSnapshot snap;
	const bool sources_ok =
			world_->context().store->field().locked_by_caller().snapshot_region(r, &snap);
	const std::vector<ve::EditOp> &ops = snap.ops;
	const std::vector<ve::IVec3> &bricks = snap.bricks;
```

and the `if (!bricks.empty()) { … snapshot_field_sources … job.gen = …; }` block with:

```cpp
	if (!bricks.empty()) {
		if (!sources_ok) return d;
		job.source = snap.sources;
		job.gen = &world_->context().store->generator()->sampler();
	}
```

- [ ] **Step 4: Delete `snapshot_field_sources`**

Delete its declaration and comment from `extension/src/core/world_store.h` and its definition from `world_store.cpp`; remove `#include <set>` from `world_store.cpp` if now unused.

- [ ] **Step 5: Build and run the consolidation suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_consolidation.gd,res://tests/test_world_field_consumers.gd,res://tests/test_world_field_overrides.gd,res://tests/test_override_region_border.gd,res://tests/test_stored_normals.gd
rg 'snapshot_field_sources' extension/src
```

Expected: PASS; `rg` prints nothing.

- [ ] **Step 6: Commit**

```bash
git add extension/src/mesh/consolidation.cpp extension/src/debug/hooks_world.cpp extension/src/core/world_store.h extension/src/core/world_store.cpp
git commit -m "refactor: consolidation takes its bake inputs from one region snapshot

pump_async, force_region and debug_consolidate_diff each assembled the op list,
seq, planned bricks, bounding box and source copy by hand; they now take
FieldView::snapshot_region under the lock they already hold.
WorldStore::snapshot_field_sources is deleted."
```

---

### Task 13: Diagnostic hooks read the field (row 7)

**Files:**
- Modify: `extension/src/debug/hooks_world.cpp` (`debug_field_sdf`, `debug_consolidate_diff` oracle)
- Modify: `extension/src/debug/hooks_physics.cpp` (`debug_mesh_lattice_diff`, `debug_mesh_diff`)
- Modify: `extension/src/debug/hooks_lod.cpp` (`debug_lod_diff` oracle)

**Interfaces:**
- Consumes: `FieldView::sample`, `FieldView::snapshot_lattice`, `ve::SnapshotSources`, `ve::lod_cut_ops`.
- Produces: no new names; hooks keep their Dictionary keys.

- [ ] **Step 1: `debug_field_sdf`**

Replace its body with:

```cpp
float VoxelDebugHooks::debug_field_sdf(Vector3 p) {
	const ve::FieldView view = world_->context().store->field().lock();
	return view.valid() ? view.sample(p.x, p.y, p.z).sdf : 1e30f;
}
```

- [ ] **Step 2: `debug_consolidate_diff`'s oracle**

The hook still holds `edit_lock`. Before the two oracle loops, add `const ve::FieldView view = world_->context().store->field().locked_by_caller();` and replace both `ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()), X, Y, Z, &world_->context().store->volumes(), world_->context().store->overrides())` expressions with `view.sample(X, Y, Z)` (same three coordinate expressions). Remove the now-unused `gen` local if nothing else uses it.

- [ ] **Step 3: The chunk diffs evaluate a lattice snapshot**

In `debug_mesh_lattice_diff` and `debug_mesh_diff`, replace the op copy block

```cpp
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		ops = world_->context().store->edit_log()->ops(ve::region_of_chunk(c));
	}
```

with

```cpp
	std::vector<ve::EditOp> ops;
	ve::FieldSnapshot snap;
	{
		const ve::FieldView view = world_->context().store->field().lock();
		if (!view.valid()) return d;
		ops = world_->context().store->edit_log()->ops(ve::region_of_chunk(c));
		// The oracle's sources: everything the chunk lattice (one cell below the origin to
		// the far face) can read, copied under the lock instead of read live afterwards.
		float o[3];
		ve::chunk_world_origin(c, o);
		const float lattice_origin[3] = {o[0] - ve::kChunkCellSize, o[1] - ve::kChunkCellSize,
				o[2] - ve::kChunkCellSize};
		const float span = static_cast<float>(ve::kChunkLattice - 1) * ve::kChunkCellSize;
		const float lattice_hi[3] = {lattice_origin[0] + span, lattice_origin[1] + span,
				lattice_origin[2] + span};
		if (!view.snapshot_lattice(lattice_origin, lattice_hi, lattice_origin, ve::kChunkCellSize,
					ve::kChunkLattice, &snap))
			return d;
	}
	const ve::SnapshotSources sources(snap.sources);
	if (!sources.ok) return d;
```

In `debug_mesh_lattice_diff`, add `job.override_table = snap.override_table;` after `MeshJob job{…};` (plan decision 3: the GPU side gains the chunk region's table, as `debug_mesh_diff` and the shipped mesher have). In both hooks, every oracle `ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()), X, Y, Z, <sources>)` becomes `ve::eval_field(gen, snap.ops.data(), static_cast<int>(snap.ops.size()), X, Y, Z, &sources.volumes, &sources.overrides)`, where `<sources>` is `&world_->context().store->volumes()` or `&world_->context().store->volumes(), world_->context().store->overrides()`.

- [ ] **Step 4: `debug_lod_diff`'s oracle evaluates a lattice snapshot**

Replace Task 2's locked per-region oracle block (the `{ std::lock_guard … region_ops … }` block computing `fine_max_diff`) with:

```cpp
	// 1. The fine lattice against the CPU field: every op the lattice can read (never
	// truncated), cut by the same relevance rule as the build (plan decision 4), over a
	// source copy taken under the lock.
	int fine_max_diff = 0;
	{
		ve::FieldSnapshot snap;
		const float half = cell * 0.5f;
		const float lattice_origin[3] = {origin[0] - 3.0f * half, origin[1] - 3.0f * half,
				origin[2] - 3.0f * half};
		const float span = static_cast<float>(ve::kLodFineLattice - 1) * half;
		const float lattice_hi[3] = {lattice_origin[0] + span, lattice_origin[1] + span,
				lattice_origin[2] + span};
		{
			const ve::FieldView view = world_->context().store->field().lock();
			if (!view.valid() || !view.snapshot_lattice(lattice_origin, lattice_hi, lattice_origin,
						half, ve::kLodFineLattice, &snap))
				return d;
		}
		ve::lod_cut_ops(level, &snap.ops);
		const ve::SnapshotSources sources(snap.sources);
		if (!sources.ok) return d;
		for (int z = 0; z < ve::kLodFineLattice; z++)
			for (int y = 0; y < ve::kLodFineLattice; y++)
				for (int x = 0; x < ve::kLodFineLattice; x++) {
					const float p[3] = {origin[0] + (static_cast<float>(x) - 3.0f) * half,
							origin[1] + (static_cast<float>(y) - 3.0f) * half,
							origin[2] + (static_cast<float>(z) - 3.0f) * half};
					const float s = ve::eval_field(gen, snap.ops.data(), static_cast<int>(snap.ops.size()),
							p[0], p[1], p[2], &sources.volumes, &sources.overrides).sdf;
					const int idx = ve::lod_fine_index(x, y, z);
					const int diff = std::abs(static_cast<int>(fine_sdf[idx]) -
							static_cast<int>(ve::lod_encode_sdf(s, cell)));
					fine_max_diff = std::max(fine_max_diff, diff);
				}
	}
	d["fine_max_diff"] = fine_max_diff;
```

- [ ] **Step 5: Build and run every diff suite**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_mesh_diff.gd,res://tests/test_lod_mesh_diff.gd,res://tests/test_consolidation.gd,res://tests/test_override_region_border.gd,res://tests/test_world_field_consumers.gd,res://tests/test_world_field_overrides.gd,res://tests/test_sun_shadow.gd
```

Expected: every case matches the baseline.

- [ ] **Step 6: Commit**

```bash
git add extension/src/debug/hooks_world.cpp extension/src/debug/hooks_physics.cpp extension/src/debug/hooks_lod.cpp
git commit -m "refactor: diagnostic hooks read the WorldField

debug_field_sdf and debug_consolidate_diff sample a field view; the chunk and
LoD diff oracles evaluate a lattice snapshot copied under the lock instead of
reading live overrides afterwards, and debug_mesh_lattice_diff's job carries the
chunk region's override table like the shipped mesher."
```

---

### Task 14: Public `VoxelWorld.raycast`; gameplay stops calling the hook

**Files:**
- Create: `tests/test_voxel_world_raycast.gd`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/debug/hooks_world.cpp` (`debug_raycast`)
- Modify: `demo/edit_tool.gd`, `demo/hud.gd`

**Interfaces:**
- Consumes: `WorldStore::field()`, `FieldView::raycast`.
- Produces: GDScript `VoxelWorld.raycast(origin: Vector3, dir: Vector3, max_distance: float = 200.0) -> Dictionary` with keys `hit`, and when hit `pos`, `normal`, `distance`, `material`.

- [ ] **Step 1: Write the failing contract**

Create `tests/test_voxel_world_raycast.gd`:

```gdscript
extends GdUnitTestSuite
# The gameplay raycast (docs/superpowers/specs/2026-09-16-world-field-query-design.md §5):
# VoxelWorld.raycast is what demo/edit_tool.gd and demo/hud.gd aim with, and debug_raycast is
# a forward to it.

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
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

func test_raycast_reports_what_debug_raycast_reports() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	for origin in [Vector3(24.4, 70.0, 24.4), Vector3(6.4, 70.0, 20.0), Vector3(10.0, 80.0, 10.0)]:
		var a: Dictionary = w.raycast(origin, Vector3(0.2, -1.0, 0.1))
		var b: Dictionary = w.hooks().debug_raycast(origin, Vector3(0.2, -1.0, 0.1))
		assert_dict(a).is_equal(b)

func test_a_hit_carries_position_normal_distance_and_material() -> void:
	var w := make_world()
	var h: Dictionary = w.raycast(Vector3(12.8, 80.0, 12.8), Vector3.DOWN)
	assert_bool(h["hit"]).is_true()
	assert_float(float(h["pos"].y)).is_between(41.0, 62.0)
	assert_float(float(h["normal"].y)).is_greater(0.0)
	assert_float(float(h["distance"])).is_equal_approx(80.0 - float(h["pos"].y), 0.01)
	assert_int(int(h["material"])).is_greater(0)

func test_max_distance_bounds_the_ray() -> void:
	var w := make_world()
	assert_bool(w.raycast(Vector3(12.8, 80.0, 12.8), Vector3.DOWN, 10.0)["hit"]).is_false()
	assert_bool(w.raycast(Vector3(12.8, 80.0, 12.8), Vector3.DOWN)["hit"]).is_true()
	assert_bool(w.raycast(Vector3(12.8, 80.0, 12.8), Vector3.UP)["hit"]).is_false()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_voxel_world_raycast.gd`
Expected: FAIL, `Invalid call. Nonexistent function 'raycast'`.

- [ ] **Step 3: Implement `VoxelWorld::raycast`**

In `extension/src/voxel_world.h`, next to `append_edit_op`:

```cpp
	// GDScript "raycast": the CPU field ray gameplay aims with -> {hit, pos, normal,
	// distance, material}. `material` is the struck solid's, for hardness-aware tools.
	Dictionary raycast(Vector3 origin, Vector3 dir, float max_distance = 200.0f);
```

In `extension/src/voxel_world.cpp` `_bind_methods`, next to the `append_edit` binding:

```cpp
	ClassDB::bind_method(D_METHOD("raycast", "origin", "dir", "max_distance"), &VoxelWorld::raycast, DEFVAL(200.0f));
```

and after `append_edit_op`:

```cpp
Dictionary VoxelWorld::raycast(Vector3 origin, Vector3 dir, float max_distance) {
	Dictionary d;
	d["hit"] = false;
	const float o[3] = {origin.x, origin.y, origin.z};
	const float f[3] = {dir.x, dir.y, dir.z};
	const ve::RayHit h = store_->field().lock().raycast(o, f, max_distance);
	if (!h.hit) return d;
	d["hit"] = true;
	d["pos"] = Vector3(h.pos[0], h.pos[1], h.pos[2]);
	d["normal"] = Vector3(h.normal[0], h.normal[1], h.normal[2]);
	d["distance"] = h.distance;
	d["material"] = static_cast<int>(h.material);
	return d;
}
```

In `extension/src/debug/hooks_world.cpp`, replace `debug_raycast`'s body with:

```cpp
Dictionary VoxelDebugHooks::debug_raycast(Vector3 origin, Vector3 dir) {
	// Kept for the test suites; gameplay calls VoxelWorld.raycast.
	return world_->raycast(origin, dir, 200.0f);
}
```

- [ ] **Step 4: Build and run the contract**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_voxel_world_raycast.gd,res://tests/test_world_field_consumers.gd
wc -l extension/src/voxel_world.h
```

Expected: PASS; `voxel_world.h` at most 250 lines (sub-project 2's exit criterion).

- [ ] **Step 5: Switch gameplay scripts**

In `demo/edit_tool.gd`, replace both

```gdscript
	var hit: Dictionary = _world.hooks().debug_raycast(
		_cam.global_position, -_cam.global_transform.basis.z)
```

with

```gdscript
	var hit: Dictionary = _world.raycast(
		_cam.global_position, -_cam.global_transform.basis.z)
```

In `demo/hud.gd` `_reticle_circle_radius`, replace `_world.hooks().debug_raycast(` with `_world.raycast(`.

- [ ] **Step 6: Run the demo suites and check the exit scan**

```bash
./gdunit_tests.sh -a res://tests/test_demo_shell.gd,res://tests/test_material_picker.gd,res://tests/test_edit_pipeline.gd
rg 'debug_raycast' demo/edit_tool.gd demo/hud.gd
```

Expected: PASS; `rg` prints nothing.

- [ ] **Step 7: Commit**

```bash
git add tests/test_voxel_world_raycast.gd extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/debug/hooks_world.cpp
git commit -m "feat: VoxelWorld.raycast is the public gameplay ray

Same {hit, pos, normal, distance, material} result as debug_raycast, with a
max_distance argument; debug_raycast forwards to it so the suites are untouched."
git add demo/edit_tool.gd demo/hud.gd
git commit -m "refactor: the edit tool and HUD reticle aim with VoxelWorld.raycast"
```

---

### Task 15: Exit evidence, results report and roadmap status

**Files:**
- Create: `docs/superpowers/plans/2026-09-16-world-field-query-results.md`
- Modify: `docs/superpowers/specs/2026-09-16-world-field-query-design.md`
- Modify: `docs/superpowers/specs/2026-09-13-frame-module-design.md` (§10 S3 row)
- Modify: `docs/superpowers/plans/2026-09-13-frame-module.md` (Sub-project 5 status)

**Interfaces:**
- Consumes: everything above.
- Produces: the results report.

- [ ] **Step 1: Full regression**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh
```

Run Task 1 Step 3's extraction script on the new report and compare with the baseline by suite count and failing case name + message. Any new failure: stash, rebuild the parent commit, re-run that suite, and only then attribute it.

- [ ] **Step 2: Exit scans**

```bash
rg 'struct LogProbe|struct LogContactProbe|snapshot_field_sources|raycast_down' extension/src
rg 'debug_raycast' demo/edit_tool.gd demo/hud.gd
rg 'edit_log\(\)->ops\(|collect_ops_for_aabb' extension/src/physics extension/src/mesh extension/src/debug extension/src/voxel_world.cpp
```

Expected: the first two print nothing. For the third, list every remaining site in the report and name its owner — each must be a 5b site (the collider mesh request op copy, `land_extraction`'s staleness check, consolidation's queue checks, the mesh diff GPU job op copies, the edit append path) or a diagnostic whose GPU job needs the region list; anything else is a missed migration: migrate it in a `refactor:` commit before writing the report.

- [ ] **Step 3: Write the results report**

Create `docs/superpowers/plans/2026-09-16-world-field-query-results.md`:

```markdown
# World field query (sub-project 5a) — results

Commit: `<git rev-parse --short HEAD>`. Recorded <date> on <machine / GPU / Godot version>.
Baseline: `docs/superpowers/plans/2026-09-16-world-field-query-baseline.md`. Report: `reports/report_<N>`.

## Regression
Native: `<doctest summary line>`
gdUnit: <cases / errors / failures>; differences from baseline:
- `test_override_region_border`: new suite, <n> cases.
- `test_world_field_consumers`: new suite, 4 cases.
- `test_voxel_world_raycast`: new suite, 3 cases.
- <every other difference, or "all other suites kept their counts and failure sets">

## Suspected bugs
| Claim | Result | Test | Commits |
|---|---|---|---|
| S3a brick wrap (region-map shaders) | FIXED / CLOSED | `test_a_brick_beside_a_region_border_reads_no_wrapped_override` | <test>, <fix> |
| S3a brick wrap (single-table jobs) | FIXED / CLOSED | `test_a_collider_chunk_lattice_reads_no_wrapped_override`, `test_an_island_lattice_across_a_region_border_reads_no_wrapped_override` | <test>, <fix> |
| S3b LoD origin table | FIXED / CLOSED | `test_a_lod_chunk_reads_overrides_of_every_region_it_covers` | <test>, <fix> |
| S3c LoD op prefix | FIXED / CLOSED | `test_an_over_cap_lod_chunk_is_refused_not_truncated`, `test_ops_too_small_for_a_lod_level_do_not_count_against_its_cap` | <test>, <fix> |

Frame time A/B/A for the S3a/S3b shader fix: <A1> / <B> / <A2>.

## Goldens
- `test_frame_shipped_golden` — unchanged / moved (cause)
- `test_lod_raster_golden` — unchanged / moved (cause)
- `test_world_field_consumers` — unchanged since Task 5

## Exit checks
<the three commands from Step 2 with their output; the third with each remaining site and its owner>

## Change cost (spec §7)
| Scenario | Before | After | Files after |
|---|---|---|---|
| New downward ground query in a consumer that holds the store | 3 | 1 | the consumer (`store->field().lock().raycast(...)`) |

## Handed to 5b
<spec §8 list, unchanged, plus any 5b-owned site found in Step 2>

## Open
<anything recorded but not resolved, or "none">
```

- [ ] **Step 4: Update the specs and roadmap**

In `docs/superpowers/specs/2026-09-16-world-field-query-design.md`: set `**Status:** Implemented; see docs/superpowers/plans/2026-09-16-world-field-query-results.md`, and add a section `## 11. Decided during planning` containing the fourteen numbered decisions from this plan verbatim.

In `docs/superpowers/specs/2026-09-13-frame-module-design.md` §10, append to the S3 row's evidence cell: `— **Sub-project 5a result:** <per-claim FIXED/CLOSED with commits and tests>.`

In `docs/superpowers/plans/2026-09-13-frame-module.md`, under `### Sub-project 5 — World field query and edit spine`, add after the heading:

```markdown
**Status.** 5a (read side: S3, `WorldField`, public raycast) implemented; see `docs/superpowers/plans/2026-09-16-world-field-query-results.md`. 5b (write side: `EditPipeline`, fan-out, lock order, `max_override_bricks = 1`) not started; its inheritance list is spec `2026-09-16-world-field-query-design.md` §8.
```

and in the suspected-bugs routing table, change the S3 row's "Fixed in" cell to `Sub-project 5a` and its milestone cell to `<FIXED/CLOSED per claim>`.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/plans/2026-09-16-world-field-query-results.md docs/superpowers/specs/2026-09-16-world-field-query-design.md docs/superpowers/specs/2026-09-13-frame-module-design.md docs/superpowers/plans/2026-09-13-frame-module.md
git commit -m "docs: record world field query exit criteria and results"
```
