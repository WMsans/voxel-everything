# Voxel Everything — Edit Pipeline (Sub-project 5b)

**Date:** 2026-09-17
**Status:** Design approved; not started
**Roadmap:** `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.4, and the pathway in
`docs/superpowers/plans/2026-09-13-frame-module.md` ("Sub-project 5 — World field query and edit
spine").
**Scope:** **5b, the write side.** Starts on 5a's merged result (`b06b4b1`) and takes over the
items listed in `docs/superpowers/specs/2026-09-16-world-field-query-design.md` §8.

---

## 1. Problem (as of `b06b4b1`)

- **The edit spine has two halves.** `VoxelWorld::append_edit_locked` (`voxel_world.cpp:689`) wraps
  `WorldStore::append_edit_locked` (`world_store.cpp:32`). The store half appends to the log,
  queues consolidation at 192 ops, bumps the seq, notifies islands through `EditSink`, and pushes
  onto `pending_edits_`. The `VoxelWorld` half adds rejection stats, the LoD mark and the
  collider `pending_dirty_` push. `WorldStore::append_edit` is public, unused, and warns tools
  not to call it.
- **Consolidation repeats the invalidation by hand**, twice (`consolidation.cpp:157-171`,
  `:550-559`): a collider range, a LoD mark under the LoD mutex, and a streamer region
  regeneration.
- **`IslandManager` works out op headroom by hand**, three times: the crumble preflight
  (`island_manager.cpp:543`), the landing carve preflight (`:662`) and the re-merge paste
  preflight (`:1139`).
  The carve keeps a "rejected after some boxes landed, restore the field" branch that its own
  comment calls unreachable. The staleness check at `:771` re-collects ops and compares bytes.
  The manager reaches the spine through `Collaborators::append_edit_locked`, a `std::function`
  that its header names as debt.
- **Lock nesting inside the edit lock.** The LoD mark takes `LodSystem::mutex()` and
  `IslandManager::note_edit` takes `windows_mutex_`, both while the edit lock is held. The lock
  order is restated in `world_store.h`, `lod_system.h`, `lod_system.cpp` and `consolidation.h`.
- **The `max_override_bricks = 1` workaround.** `test_connectivity.gd:40` shrinks the override
  pool so that consolidation cannot bake away the full op lists its rejection tests build.

Three of the five fan-out consumers are already queues drained elsewhere: collider
`pending_dirty_` (by `physics_tick`), `pending_edits_` (by `WorldStreamer::run_frame`) and
`consolidation_queue_` (by `pump_async`). Island windows are queued too, but behind their own
mutex. Only the LoD mark acts synchronously.

## 2. Decisions

| Topic | Decision |
|---|---|
| Fan-out shape | **B, deferred sinks.** Every sink only appends to its own queue guarded by the edit lock; its owner drains the queue on its own thread. Nothing nests inside the edit lock. |
| Rejected: A, synchronous sinks | Keeps today's timing, but also keeps the edit → LoD and edit → windows lock edges. |
| Rejected: C, merge the spines only | Consolidation's duplicated invalidation and `IslandManager`'s headroom code survive, so two roadmap exit criteria fail. |
| Batch | `apply(ops, policy)`: `atomic` preflights every op's regions and is all-or-nothing; non-atomic behaves exactly as today, op by op. |
| `max_override_bricks = 1` | Replaced by an explicit consolidation hold seam, after a test proves the workaround is still needed. |
| Restore branch | Deleted with its failure toggles: an atomic carve cannot partially apply. |
| Staleness | By log append seq instead of byte comparison. A consolidation bake during an extraction's flight no longer makes it stale. This is a deliberate change, made in its own commit. |
| Accepted cost of B | LoD marks land at the next `LodSystem::tick` (possibly one redundant rebuild, never a lost edit); LoD and collider queues need merging and a cap; each queue needs a drop rule at teardown; `test_lod_stream.gd:60` must tick before it reads. |

## 3. Shape and the one lock rule

New pure module `core/edit_pipeline.{h,cpp}` (namespace `ve`, in the native test globs).
`WorldStore` owns one instance. The edit mutex stays in `WorldStore`, because `WorldField`
already points at it.

```cpp
enum class InvalidationReason { kEdit, kRejected, kConsolidated };

struct Invalidation {
	InvalidationReason reason;
	float lo[3], hi[3];                  // kEdit: op world AABB
	IVec3 region;                        // kConsolidated only
	int64_t seq;                         // edit seq after the bump
	const EditOp *op;                    // kEdit, kRejected
	const EditLog::AppendResult *append; // kEdit, kRejected
	bool notify_islands;
};

struct InvalidationSink {
	virtual ~InvalidationSink() = default;
	// Edit lock HELD. Takes no lock; only appends to the sink's own edit-lock-guarded queue.
	virtual void record(const Invalidation &) = 0;
};

struct EditPolicy {
	bool atomic = false;
	bool notify_islands = true;
};

struct BatchResult {
	std::vector<EditLog::AppendResult> ops; // one per op, in order
	std::vector<IVec3> refused;             // atomic only: the regions that refused the batch
};

class EditPipeline {
public:
	BatchResult apply(std::span<const EditOp> ops, EditPolicy policy); // caller holds the lock
	void invalidate(const Invalidation &inv);                           // caller holds the lock
	void add_sink(InvalidationSink *sink);                              // caller holds the lock
	void remove_sink(InvalidationSink *sink);                           // caller holds the lock
};
```

**The rule, stated once in `edit_pipeline.h`:** nothing nests inside the edit lock. A sink
queues under the lock. Its owner swaps the queue out under the lock, releases the lock, and only
then acts (takes the LoD mutex, marks chunks, labels windows). The lock order that remains is
`admission → render_lifetime → edit`. It is stated only in this header; the restatements listed
in §1 are deleted, and the admission comments in `orchestrator.h` and `voxel_world.cpp` point
here.

## 4. `apply`

- **Non-atomic.** For each op: `EditLog::append`. If the op was rejected or oversized, record
  `kRejected`. If it touched nothing, stop: no seq bump and no `kEdit`, as today. Otherwise
  bump the seq (after the append, under the same lock, as today) and record `kEdit`.
- **Atomic.** Before appending anything, sum each op's `op_region_range` per region. If any
  region's `op_count + n` would pass `kMaxRegionOps`, or any op is oversized: append nothing,
  move no seq, record nothing, and fill `refused`. Otherwise append every op as in the
  non-atomic path; none can be rejected, because the preflight ran under the same lock.

### 4.1 `IslandManager` migrations

1. **Crumble:** the preflight loop is deleted; it becomes
   `apply(boxes, {atomic, notify_islands = false})`. A refusal returns false.
2. **Re-merge paste:** the preflight is deleted; it becomes `apply({paste}, {atomic})`.
   `note_merge_rejected` takes `refused`. `merge_retry_blocked`'s cooldown read stays.
3. **Landing carve:** becomes `apply(boxes, {atomic})`. The branch that restores the field after
   a partial carve is deleted, together with `debug_fail_next_carve_`, `debug_fail_next_restore_`
   and any test that forces them. The "accepted but touched nothing" case stays (the
   `carve_nothing` refusal).
4. **Staleness:** the job records the log's append seq with its field snapshot. At landing, under
   the edit lock, the job is stale if any op with a higher seq reaches its boxes (the existing
   strict-overlap predicate). `collect_ops_for_aabb` leaves `physics/`.
5. `Collaborators::append_edit_locked` is deleted; the manager takes the pipeline.

**Island drain timing.** The inbox is drained under the edit lock at the start of landing
(`run_frame` step 2) and again before connectivity (step 3). The manager's own carves are seen in
the same frame as today, so B costs this sink no tick.

### 4.2 Consolidation hold seam

`ConsolidationCoordinator` gains a held flag, exposed as `hooks().debug_hold_consolidation(bool)`.
While it is set, regions still queue and an in-flight bake still finishes, but `pump_async` starts
no new bake. `test_connectivity.gd`'s `make_world` sets it and drops `max_override_bricks = 1`.

## 5. Sinks

**Consolidation.** Both commit copies drop their hand-written invalidation block and call
`pipeline.invalidate({kConsolidated, region})`. The coordinator's own re-queue at ≥ 192 ops and
the table assignment stay. The commit already holds `render_lifetime → edit`, so this also
removes the edit → LoD edge from consolidation.

| Sink (owner) | `kEdit` | `kRejected` | `kConsolidated` |
|---|---|---|---|
| Colliders (`VoxelWorld`) | `op_chunk_range(op)` | none | `chunk_of_brick(base)..chunk_of_brick(base + 31)` |
| LoD (`LodSystem`) | `op_world_aabb(op)` | none | the region's brick world AABB |
| Islands (`IslandManager`) | a window (paint skipped; seq; impulse for sphere subtract), only if `notify_islands` | none | none |
| Consolidation queue (`ConsolidationCoordinator`) | queue every touched region with `op_count ≥ 192` | none | none (re-queues itself) |
| Pending edits (`WorldStore`) | push `{op, append}` | none | none |
| Streamer regeneration (`WorldStreamer`) | none | none | `queue_region_regeneration_locked(region)` |
| Stats (`VoxelWorld`) | none | `edit_rejections` and the two `printerr` lines, word for word | none |

The ranges each sink derives are the ones today's code computes; `region` is carried so that no
sink derives a brick range from a float box.

### 5.1 Lifetimes and drop rules

Registration and removal happen under the edit lock and never from inside another lock.

- **Registered for the owner's lifetime:** colliders, stats, pending edits, the consolidation
  queue. The collider queue is still cleared at physics teardown.
- **LoD:** registered with `LodSystem`, not with the tree. The tree is created lazily inside
  `tick` while holding the LoD mutex, and registering there would reverse the lock order. `tick`
  drains its queue before it creates the tree and discards the queue when there is no tree, which
  matches today's `if (!lod_tree_) return`. `release_gpu` clears the queue.
- **Islands:** registered in `IslandManager::initialize` and removed in `teardown`. This replaces
  `VoxelWorld`'s "publish the manager under `edit_mutex_`" wiring and its `on_edit_appended`
  adapter. The inbox is cleared with `windows_`.
- **Streamer regeneration:** registered in `WorldStreamer::initialize` and removed at teardown,
  replacing consolidation's `if (streamer())` guard.

### 5.2 `merge_or_cap<Box>`

One template with two users: float AABBs (LoD) and chunk ranges (colliders). It merges an incoming
box into any queued box it overlaps; past 1024 entries it folds the whole queue into one bounding
box. Over-dirtying is safe; losing a mark is not. The fold is marked `ponytail:` in code: it can
mark a huge span dirty, but reaching 1024 needs a drain that is not running. The upgrade, if that
ever happens with a live drain, is pairwise folding by smallest growth.

### 5.3 Deleted

`WorldStore::append_edit{,_locked}`, `EditSink`, `ConsolidationSink`,
`VoxelWorld::append_edit_locked`, `VoxelWorld::on_edit_appended`, `LodSystem::note_edit`, the
public `IslandManager::note_edit` (its body becomes the sink's), `Collaborators::append_edit_locked`,
the restore branch and its toggles (§4.1), the headroom preflights (§4.1), and
`ConsolidationCoordinator::Collaborators::{pending_dirty, lod_mutex, lod_tree}` where nothing else
reads them. `windows_mutex_` is deleted if `windows_` becomes main-thread only once the inbox
exists; the plan decides this by listing every reader.

## 6. Characterization and tests

**Baseline.** Re-record the gdUnit failure set at `b06b4b1` with the frame-module plan's Task 1
procedure.

**Characterization (milestone 2).** Every test passes on today's code and is shown to bite by
breaking the code on purpose.

1. **Fan-out pin** (`tests/test_edit_fanout.gd`). A new `hooks().debug_edit_fanout()` first calls
   each consumer's real drain function, the same one its tick calls (today these are no-ops), and
   then reports:
   - collider ranges
   - LoD dirty chunk keys by level
   - island windows (lo, hi, seq, impulse)
   - the consolidation queue
   - the count and regions of `pending_edits_`
   - the streamer's forced regenerations
   - `edit_rejections`

   Op set: sphere subtract, sphere add, paint, box subtract with `notify_islands = false`, volume
   add, one op crossing a region border, one rejected op, one oversized op, and one forced
   consolidation commit. The output is printed and pasted (the `test_frame_shipped_golden`
   pattern).
2. **Proof the workaround is needed** (`tests/test_consolidation.gd`). With a normal override
   pool, fill a region with 256 paint ops and pump frames until a consolidation lands. The op
   count drops below 256, so a rejection test built by filling a region is vacuous. When the
   seam lands, the same test gains a held case that keeps 256.
3. **Staleness pin.** An extraction is held in flight while a consolidation commits over its
   boxes. Today it is refused as stale.

**Native** (`extension/tests/test_edit_pipeline.cpp`, fake sinks):
- an atomic batch is all-or-nothing, including a cross-region sum that stays under the cap in each
  region alone, and an oversized op refuses the batch
- a non-atomic op is partially accepted
- the seq moves only when a region is touched
- each reason reaches the right sinks, and a removed sink hears nothing
- `merge_or_cap` merges, and folds past the cap

## 7. Milestones

One commit each. The fan-out pin, the shipped-frame golden and the baseline failure set stay
unchanged unless a milestone says otherwise.

1. `docs:` baseline failure set.
2. `test:` the three characterizations (§6).
3. `test:` consolidation hold seam; `test_connectivity.gd` drops `max_override_bricks = 1`.
4. `feat:` `EditPipeline`, `merge_or_cap`, native tests.
5. `refactor:` both spines move into `apply`; the existing queues become sinks. LoD and islands
   are still synchronous sinks at this step (they keep today's edit → LoD and edit → windows
   locks), which is the only exception to §3's rule, and it ends at milestones 10 and 11.
6. `refactor:` consolidation calls `invalidate`.
7. `refactor:` crumble, paste and landing carve use atomic `apply`; delete
   `Collaborators::append_edit_locked`.
8. `refactor:` delete the unreachable restore branch and its failure toggles.
9. `refactor:` staleness by seq. **The staleness pin changes here**, and the commit message names
   the cause (§2).
10. `refactor:` LoD becomes queue-and-drain; the edit → LoD edge is gone; `test_lod_stream.gd:60`
    ticks before it reads.
11. `refactor:` islands become an inbox; the edit → windows edge is gone.
12. `docs:` the lock order moves into `edit_pipeline.h`; the restatements are deleted.
13. `docs:` results report.

**Stop conditions.** A fan-out pin row changes in any commit other than milestone 9. A lock is
acquired inside the edit lock that today's code does not already take there (and, after milestone
11, any lock at all). A sink is registered from inside another lock.

## 8. Exit criteria

- `rg 'WorldStore::append_edit\(|append_edit_locked|struct EditSink|struct ConsolidationSink|on_edit_appended|LodSystem::note_edit' extension/src`
  returns nothing.
- `rg -il 'lock order' extension/src` returns only `core/edit_pipeline.h`.
- `rg 'max_override_bricks' tests/test_connectivity.gd` returns nothing.
- `rg 'collect_ops_for_aabb|edit_log\(\)->ops\(' extension/src/physics` returns nothing.
  `debug/hooks_physics.cpp:965` (`debug_mesh_submit`), handed over by 5a, is a diagnostic mesh-job
  op copy like its 5a siblings, not part of the edit spine; it stays, and the report records that.
- No `record()` body takes a mutex (checked by review; the report lists each body).
- Native tests pass; the gdUnit failure set matches the baseline.

**Change cost.** New scenario, recorded in the results report: "a new consumer that must hear
about edits". Today: `voxel_world.h`, `voxel_world.cpp`, `core/world_store.h`,
`mesh/consolidation.cpp` and the consumer (5 files). After: the consumer's `.h` and `.cpp` (2).

## 9. Out of scope

`EditOp` layout and the op-type registry; `StreamingBudget`; `WorldStreamer::run_frame`'s repeated
lock/read/upload; the diagnostic GPU-job op copies in `debug/`; `pending_dirty_` growth while
physics is off beyond the `merge_or_cap` bound; stage authoring (sub-project 6).

## 10. Risks

1. **A LoD build lands between its gather and the drain.** The drained mark dirties the chunk
   again, so it rebuilds once more for nothing. Every ordering B allows matches an ordering
   today's code handles when an edit simply arrives later; no edit is lost.
2. **The fan-out pin reads the post-drain state, and the hook could drain differently from the
   tick.** Mitigation: the hook calls the ticks' own drain functions, never a copy.
3. **Deleting the restore branch removes a defensive path.** Mitigation: the native all-or-nothing
   test lands first (milestone 4), and the landing carve uses atomic `apply` (milestone 7) before
   the branch is deleted (milestone 8).
4. **Seq staleness misses a change the byte comparison caught.** The only case the byte
   comparison caught that seq staleness does not is ops removed by consolidation, which does not
   change the field. The staleness pin makes the difference visible, and milestone 9 names it.
5. **A merge-and-cap fold dirties a huge LoD span.** It is reachable only while a drain is not
   running; the fold is marked `ponytail:` with its upgrade path.
