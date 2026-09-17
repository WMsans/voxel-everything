# Voxel Everything — World Field Query (Sub-project 5a) + S3

**Date:** 2026-09-16
**Status:** Design approved; plan not yet written
**Roadmap:** `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.4 and §10, and the
pathway in `docs/superpowers/plans/2026-09-13-frame-module.md` ("Sub-project 5 — World field
query and edit spine").
**Scope:** sub-project 5 is cut into two spec → plan → implementation cycles. This spec is **5a,
the read side**: suspected bug S3, `ve::WorldField` / `FieldView`, the consumer migrations, and a
public `VoxelWorld.raycast` (moved here from the roadmap's write-side milestone because it is a
pure query). **5b, the write side** (`EditPipeline`), starts on 5a's merged result; §8 lists what
it inherits.

Vocabulary as in the roadmap spec (Ousterhout's deep/shallow modules).

---

## 1. Problem (as of `63d7fdc`)

- **No module answers "the world field here".** `eval_field(gen, ops, n, x, y, z, volumes =
  nullptr, overrides = nullptr)` and its siblings (`chunk_has_surface`, `contact_samples_field`,
  `raycast`, `extract_island_volume`) are called with inputs each caller assembles. The `nullptr`
  defaults are how S1 and S2 happened; both are fixed (`9757819`, `d9a41ea`) by threading sources
  through, not by removing the way to forget them.
- **Two probe adapters.** `LogProbe` (`physics/collider_streamer.cpp:34`) carries raw
  generator/log/mutex/volume/override pointers and locks per call. `LogContactProbe`
  (`physics/island_manager.cpp:85`) forwards to `IslandManager::contact_samples`, which does the
  same assembly.
- **Job inputs are hand-built six times.** Island extract: `collect_ops_for_aabb` + lattice bound
  + brick range + `WorldStore::snapshot_field_sources` at `island_manager.cpp:324`,
  `voxel_world.cpp:895`, `hooks_physics.cpp:612` and `:995`. Consolidation: region op list +
  brick bounding-box loop + `snapshot_field_sources` at `consolidation.cpp:275`, `:465` and
  `hooks_world.cpp:364`. `island_manager.cpp:334` reads the override table after releasing the
  lock the snapshot was taken under.
- **A race in the CPU extract diff.** `VoxelWorld::extract_component` (`voxel_world.cpp:920`)
  evaluates `extract_island_volume` against live overrides and volumes without the edit lock,
  after the GPU job returned.
- **Hook oracles omit sources.** `debug_mesh_lattice_diff` (`hooks_physics.cpp:396`) evaluates
  the CPU oracle with volumes but no overrides; a GPU-vs-CPU test near a consolidated region
  compares against a wrong answer.
- **Gameplay raycast is a debug hook.** `demo/edit_tool.gd` (two sites) and the `demo/hud.gd`
  reticle call `hooks().debug_raycast`.
- **S3 (roadmap §10), unverified.** CPU `OverrideStore` resolves overrides through a global brick
  index and is correct everywhere. The GPU is not:
  - **S3a — brick wrap.** `sample_field_override` (`shaders/field_ops.glslh:288`) takes one table
    (`FIELD_OVERRIDE_TABLE(op_base)`) and indexes it with `brick & 31`. A sample outside that
    table's region reads a brick 25.6 m away. Single-table jobs (`mesh_field`, `lod_field`,
    `island_extract`, `brick_consolidate`) take the job's table; region-map shaders
    (`brick_gen`, `brick_mark`, `raymarch`) take the table of the op region, which for
    `brick_gen`/`brick_mark` is the brick's region, not the pad sample's.
  - **S3b — LoD table from the origin.** `mesh_service.cpp:829-832` gives a LoD job the table of
    the region containing its origin; a coarse LoD chunk spans many regions.
  - **S3c — op prefix.** `LodSystem::gather_ops` (`lod_system.cpp:69-85`) keeps the first
    `kMaxRegionOps` (256) ops across every region a LoD chunk covers.

## 2. Decisions

| Topic | Decision |
|---|---|
| Cut | 5a read side (this spec), 5b write side (separate cycle on 5a's result) |
| Public raycast | Lands in 5a; `edit_tool.gd` and the `hud.gd` reticle switch; `benchmark.gd`, `capture.gd`, `dev_tools.gd`, stats hooks and test suites keep their `debug_*` calls (diagnostics, not gameplay) |
| S3 fix, if confirmed | The override table follows the **sample's** region, not the job's; S3c by relevance cut before the prefix cap |
| Query shape | Approach A: locked view plus explicit copy. `WorldField::lock() → FieldView` holds the edit lock (RAII); `FieldView::snapshot(...)` is the only copying product. Deviates from the roadmap's `snapshot(aabb) → FieldView` wording because a copy per collider probe or per 200 m raycast is the wrong cost |
| Rejected | B, copying snapshot for every query (copies ~6 KB per override brick on per-frame probes; unbounded for rays). C, an input-bundle struct (locking, pads and truncation stay with callers; adapters remain) |
| Bug-fix discipline | Each confirmed S3 claim gets a failing test, then its own `fix:` commit, before `WorldField` exists |

## 3. S3 — characterization and fix (milestone 1)

### 3.1 Test oracle first

A `test:` commit passes full sources (generator, region ops, volumes, overrides) to the CPU
oracle of every hook the S3 tests use, starting with `debug_mesh_lattice_diff`. The existing diff
suites (`test_mesh_diff`, `test_lod_mesh_diff`, `test_brick_diff`, `test_field_volume_diff`) must
stay green unchanged.

Each S3 test also checks that its hook's GPU job is assembled the way the shipped path assembles
it (`mesh_service.cpp:795` and `:829`, `island_manager.cpp:324`). Where a hook diverges, the test
drives the shipped path instead (hooks rebuild render inputs; a green hook test can test a path
that does not ship).

### 3.2 Tests

Shared fixture: region R consolidated with a carve in its local brick 0 on the −x face; its +x
neighbour R′ unconsolidated. Every test compares GPU against the CPU oracle and is proven to bite
by breaking the code on purpose.

| Claim | Test | Red if |
|---|---|---|
| S3a brick wrap | `debug_brick_diff` on R's local brick 31 (+x face), whose pad samples cross into R′ | GPU shows R's brick-0 carve |
| S3a single-table jobs | `debug_mesh_diff` for a collider chunk on the R/R′ border; `debug_island_extract_diff` for a lattice spanning the border | GPU ≠ CPU at the border |
| S3b LoD origin table | `debug_lod_diff` for a level-L chunk with origin in R′ covering R's carve | GPU lacks the carve |
| S3c op prefix | two unconsolidated regions with 150 visible ops each under one LoD chunk; `debug_lod_diff` | the later region's edits are missing |

A claim that is not red is CLOSED in the results report with the test as evidence.

### 3.3 Fix for S3a/S3b

- `sample_field_override` derives the sample's region as `floor(brick / REGION_BRICKS)`.
- **Fast path:** if it equals the job's region, use the job's table (today's behaviour).
- **Otherwise** scan a `table_region[kMaxOverrideTables]` array (32 entries of region xyz plus a
  valid flag), bound once per device and written wherever `set_override_table` already writes a
  table: `GpuAtlas`, `MeshPass`, `LodBuildPass`.
- Each shader's `FIELD_OVERRIDE_TABLE` macro supplies the fast-path table and its region; LoD
  jobs stop taking the origin's table as authoritative.
- Cost: none when no table exists; one integer compare in the common in-region case. Frame time
  is recorded with an interleaved A/B/A run (GPU timings are invalid on this machine).

### 3.4 Fix for S3c

`gather_ops` drops ops that `LodTree::mark_dirty`'s relevance cut already treats as invisible at
the job's level, then applies the chronological prefix cap. If the list still overflows, the
chunk keeps its last good mesh and a `lod_op_overflow` counter in `debug_lod_stats` increments;
a partial world is never built.

### 3.5 Stop condition

A fix that moves `test_frame_shipped_golden` or any LoD/raster golden without a measured S3 cause.

## 4. `WorldField` and `FieldView` (milestone 2)

### 4.1 Location and interface

`extension/src/world/world_field.{h,cpp}`: pure (no godot-cpp), compiled into the native test
build by the existing `src/world/*.cpp` glob.

```cpp
namespace ve {

class WorldField : public ChunkProbe, public ContactProbe {
public:
	WorldField(const Generator *gen, const EditLog *log, const VolumeSet *volumes,
			const OverrideStore *overrides, std::mutex *edit_mutex,
			const std::atomic<int64_t> *edit_seq, OverrideTableLookup tables);
	bool valid() const;       // false before the edit log exists / after release_cores
	FieldView lock() const;   // holds edit_mutex for the view's lifetime

	// Probe interfaces: lock once per call, exactly as the deleted adapters did.
	bool chunk_has_surface(IVec3 chunk) const override;
	int contact_samples(IVec3 cell, int axis, int face_samples) const override;
};

class FieldView {             // movable, non-copyable
public:
	Sample sample(float x, float y, float z) const;
	bool has_surface(IVec3 chunk) const;
	int contact_samples(IVec3 cell, int axis, int face_samples) const;
	RayHit raycast(const float origin[3], const float dir[3], float max_dist) const;
	bool snapshot(const float lo[3], const float hi[3], FieldSnapshot *out) const;
	bool snapshot_region(IVec3 region, RegionSnapshot *out) const;
};

struct FieldSnapshot {        // island extract lattice
	std::vector<EditOp> ops;
	FieldSourceSnapshot sources;
	int override_table = -1;
	int64_t edit_seq = 0;
	bool over_cap = false;    // ops.size() > kMaxRegionOps; never truncated
};

struct RegionSnapshot {       // consolidation bake
	std::vector<EditOp> ops;
	uint64_t through_seq = 0;
	std::vector<IVec3> bricks; // plan_consolidation order
	FieldSourceSnapshot sources;
};

} // namespace ve
```

`OverrideTableLookup` is a callable `(IVec3 region) → int` over `WorldStore::override_tables()`.
`sample` stays only if a migrated caller in §5 uses it (row 7 does); `gradient` is not added.

`WorldStore::field()` builds the value from the store's current pointers. Callers re-fetch it per
use, because the edit log and override store are created lazily and released across teardown;
`ColliderStreamer` may hold one for its initialize→teardown lifetime, which is the lifetime its
raw pointers have today.

### 4.2 Rules the module owns

- **Op choice.** A point query reads the op list of the region containing that point (the rule
  `world/raycast.cpp` states today: an op is appended to every region it touches). A chunk query
  reads the chunk's region list. Point and chunk queries never truncate.
- **Sources.** Every query passes the generator, volumes and overrides. The free functions keep
  their `nullptr` defaults only for native fixtures that test "generator plus ops".
- **`snapshot(lo, hi)`.** Collects ops with `kLatticeFilterPad`; copies override bricks for the
  inclusive brick range of `[lo, hi]` and every volume a `kOpVolumeAdd` references; captures
  `edit_seq` and the override table of the region of `lo`, all under the view's lock. Returns
  false where `snapshot_field_sources` returns false today (a missing override slot, a malformed
  normal span, an invalid volume).
- **`snapshot_region(r)`.** Copies the region's op list and its last seq as `through_seq`, runs
  `plan_consolidation`, snapshots sources over the planned bricks' bounding range. Returns false
  under the same conditions.
- **Invalid field.** Every query returns its empty value (`Sample{}`, `false`, `0`,
  `RayHit{hit=false}`, `false`), matching each caller's early return today.

### 4.3 Locking

Lock order does not change. A view is taken exactly where a caller takes `edit_mutex` today; no
lock is held longer and no lock is added. The probe interfaces lock per call. CPU work that runs
outside the lock today (the CPU extract after the GPU returns) stays outside the view.

### 4.4 Interface change

`ve::ContactProbe::contact_samples` gains `int face_samples`; `refine_anchoring` passes
`cfg.face_samples`. The native `ScriptedProbe` fake in `test_contact_refine.cpp` takes the extra
argument.

## 5. Consumer migration, deletions and public raycast (milestones 3 and 5)

One `refactor:` commit per row, each leaving its §6.1 characterization unchanged.

| # | Consumer | Becomes | Deleted |
|---|---|---|---|
| 1 | `ColliderStreamer` residency probe | `chunks_->update(..., field_)`; `initialize` takes a `ve::WorldField` | `struct LogProbe`; the `gen_`/`volumes_`/`overrides_` members only the probe used |
| 2 | Island contact refinement | `refine_anchoring(grid, store->field(), cfg, ...)`; `debug_contact_samples` calls `field().lock().contact_samples(...)` with the manager's `face_samples` | `struct LogContactProbe`, `IslandManager::contact_samples` |
| 3 | Island extract job inputs (`island_manager.cpp:324`, `voxel_world.cpp:895`, `hooks_physics.cpp:612`, `:995`) | `view.snapshot(lo, hi, &s)`; override table and op-cap refusal read from `s` | four hand-built copies |
| 4 | CPU `extract_island_volume` in the diff paths (`voxel_world.cpp:920`, `hooks_physics.cpp:636`) | evaluates the snapshot the GPU job received (`FieldSourceSnapshot::materialize`) | the unlocked read of live overrides |
| 5 | Raycasts: `debug_raycast`, both `raycast_down` callers in `island_manager.cpp` | `view.raycast(...)`; the down-ray (`{x, 200, z}`, `−y`, 400 m) moves to the caller | `WorldStore::raycast_down` |
| 6 | Consolidation job inputs (`consolidation.cpp:275`, `:465`, `hooks_world.cpp:364`) | `view.snapshot_region(r, &s)` | three bounding-box loops, `WorldStore::snapshot_field_sources` |
| 7 | Hooks reading the live world field (`hooks_lod.cpp:716`, `hooks_physics.cpp:396/446/536/557`, `hooks_world.cpp:397/412/1202`) | `view.sample(...)` | per-hook source assembly |

**Stays:** `debug_eval_field` (evaluates caller-supplied ops by contract); native fixtures calling
free functions; `LodSystem::gather_ops` (touched only by §3.4); the `land_extraction` staleness
check at `island_manager.cpp:781` (5b).

**Public raycast (milestone 5).** `VoxelWorld.raycast(origin: Vector3, dir: Vector3,
max_distance: float = 200.0) -> Dictionary` returns `{hit, pos, normal, distance, material}`
exactly as `debug_raycast` does and takes one view. `debug_raycast` becomes a one-line forward so
the ~20 suites using it are untouched. `demo/edit_tool.gd` (both sites) and the `demo/hud.gd`
reticle call `raycast`.

**Stop conditions.** A migrated consumer's pinned output moves; a view taken where no lock was
taken before; a lock held across a call that did not hold it before.

## 6. Testing

### 6.1 Characterization (before any production change; each shown to bite)

| Pins | How |
|---|---|
| Collider residency | gdUnit: surface/collider booleans from `debug_chunk_collider_info` for a fixed chunk set around an edited, consolidated and pasted-volume area (extends the S1 suite) |
| Contact refinement | gdUnit: `debug_contact_samples` over a cell × axis grid across a consolidated carve (extends the S2 suite) |
| Island extract inputs | gdUnit: `debug_island_extract_diff` op count, refusal reason and CPU ≡ GPU for a pillar straddling a region border and one over the op cap |
| Raycast | gdUnit: fixed ray fan (hit, pos, normal, material) over edited, consolidated and volume terrain; island merge-ground gating that consumes `raycast_down` |
| Consolidation inputs | `debug_consolidate_diff` counts and `test_consolidation.gd` unchanged |
| Hook oracles | the four diff suites unchanged after the §3.1 commit |

### 6.2 Native (`extension/tests/test_world_field.cpp`)

- **Consolidated ≡ unconsolidated** for `sample`, `has_surface`, `contact_samples`, `raycast`:
  build a region's ops, CPU-bake its planned bricks with `eval_brick` into an `OverrideStore`,
  clear the ops, compare. SDF agreement within one 8-bit encoding quantum (stated as a named
  constant in the test); fixtures keep boolean queries away from their thresholds.
- **Region border:** a point query beside a border reads only its own region's list; an op stored
  only in the neighbour's list does not leak in.
- **Snapshot:** equals a hand-assembled `collect_ops_for_aabb` + source copy; `over_cap` set at
  257 ops with all 257 kept; `edit_seq` and override table captured.
- **Region snapshot:** equals today's `consolidation.cpp:275` assembly for the same store.
- **Invalid field:** every query returns its empty value.
- **Probe interfaces:** `ChunkResidency::update` and `refine_anchoring` driven through
  `WorldField` match the free-function results.

### 6.3 Gates

Native suite passes. gdUnit has no unexplained regression or missing case against 5a's recorded
baseline (the set drifts; stash and re-run before blaming a change). `test_frame_shipped_golden`
and the LoD/raster goldens unchanged, or moved with a measured S3 cause named in the fix commit.

## 7. Order and exit criteria

**Commit order.**
1. `docs:` 5a baseline failure set.
2. `test:` hook oracles pass full sources (§3.1).
3. `test:` S3a/S3b/S3c characterization → one `fix:` per confirmed claim; A/B/A frame time for the
   shader fix.
4. `test:` consumer characterization (§6.1) and the Appendix A trace below on today's code.
5. `feat:` `WorldField` / `FieldView` with native tests; no callers.
6. `refactor:` §5 rows 1–7, one per commit.
7. `feat:` `VoxelWorld.raycast`; `refactor:` `edit_tool.gd` and `hud.gd` switch.
8. `docs:` results report `docs/superpowers/plans/<date>-world-field-query-results.md`.

**Exit criteria.**

```
rg 'struct LogProbe|struct LogContactProbe|snapshot_field_sources|raycast_down' extension/src  → empty
rg 'debug_raycast' demo/edit_tool.gd demo/hud.gd                                              → empty
rg 'edit_log\(\)->ops\(|collect_ops_for_aabb' extension/src/physics extension/src/mesh \
   extension/src/debug extension/src/voxel_world.cpp                                          → only
   sites listed as 5b-owned in the results report
```

- Roadmap §10 S3 row marked per claim: FIXED with test and commit, or CLOSED with evidence.
- §6.3 gates hold.
- **Change cost.** Appendix A has no world-query scenario; 5a adds **"a new CPU consumer of the
  world field"**, traced on today's code in commit 4 (expected 3–4 files: raw sources through an
  `initialize`, an adapter struct, the owner's wiring). Target after 5a: ≤ 2 files. Recorded in the
  results report with both counts.

## 8. Handed to 5b

`EditPipeline::apply(ops[], policy)` and `invalidate(aabb, reason)` fan-out; edit fan-out
characterization (which consumer hears which AABB); removing `max_override_bricks = 1` from
`test_connectivity.gd` behind a test proving it is still needed; lock order stated in exactly one
header; `IslandManager::land_extraction` op headroom and its staleness check
(`island_manager.cpp:781`); consolidation's hand-repeated invalidation (`consolidation.cpp:157-171`,
`554-564`); deleting `WorldStore::append_edit` and `VoxelWorld::append_edit_locked`'s tail; the exit
check `rg 'WorldStore::append_edit\('`.

## 9. Out of scope

`EditOp` layout and the op-type registry; `StreamingBudget`; `WorldStreamer::run_frame`'s repeated
lock/read/upload; terrain pipeline and stage authoring (sub-project 6); pass internals beyond the
`FIELD_OVERRIDE_TABLE` macros and the table-region binding; the stats hooks in `hud.gd`,
`benchmark.gd`, `capture.gd` and `dev_tools.gd`.

## 10. Risks

1. **The S3 shader fix costs frame time on the raymarch hot path.** Mitigation: the in-region fast
   path is today's lookup plus one compare; A/B/A measurement gates the commit against the Ridge
   p99 ceiling.
2. **Quantization makes "consolidated ≡ unconsolidated" flaky at thresholds.** Mitigation: named
   tolerance, fixtures away from zero crossings for boolean queries.
3. **Row 4 changes what the CPU extract diff compares.** It stops reading live state and reads the
   job's snapshot. A diff that was green only because live and snapshot agreed stays green; one
   that moves is a race the old code hid, and is reported, not re-pinned silently.
4. **`ColliderStreamer` holds a `WorldField` across a teardown.** Mitigation: it is rebuilt in
   `initialize` exactly where the raw pointers are assigned today, and cleared in `teardown`.
