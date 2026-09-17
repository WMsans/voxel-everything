# Task 10 Report: Staleness by append sequence

## Status

Implemented Task 10. Consolidation no longer makes an in-flight extraction stale: landing now checks for edits appended after the extraction snapshot sequence, rather than comparing the current op list with the captured op list.

## Root cause

`IslandManager::land_extraction()` previously collected the current ops for the extraction AABB and compared them with the ops captured at submission. Consolidation removes those captured edit-log ops and publishes override bricks that evaluate to the same field value. The list comparison therefore reported a field change even though no edit had been appended and no field value had changed, causing the extraction to be refused and retried.

## Changes

- Added `EditLog::last_seq()` and the optional `after_seq` filter to `collect_ops_for_aabb()`.
- Added `FieldSnapshot::log_seq`, stamped from `EditLog::last_seq()` while taking the lattice snapshot.
- Added `FieldView::ops_since()` for querying only intersecting ops appended after a sequence.
- Replaced `InFlight::ops` with `InFlight::log_seq`.
- Changed landing staleness to `std::any_of(newer, reaches_the_boxes)` while holding the edit mutex.
- Moved and renamed the consolidation characterization pin to
  `test_a_consolidation_during_an_extraction_does_not_make_it_stale`.
- Preserved `debug_mesh_submit` diagnostic copies and did not touch LoD/island deferred queue work or lock-order prose.
- No fan-out golden files or expected golden data changed.

## TDD evidence

### RED

Added the prescribed native test to `extension/tests/test_edit_log.cpp`, then ran:

```text
cd extension && scons -Q test; cd ..
```

It failed for the intended missing behavior:

```text
error: no member named 'last_seq' in 've::EditLog'
error: too many arguments to function call, expected 4, have 5
```

### GREEN

After the minimal implementation, the same command passed:

```text
645 test cases passed
9,120,150 assertions passed
0 failed
```

## Verification

Build:

```text
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Result: build succeeded and linked `libvoxel_everything.macos.template_debug.universal.dylib`.

Focused suites:

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_island_body.gd,res://tests/test_edit_fanout.gd
```

Result:

- Connectivity: 32/32 passed, including the renamed consolidation pin and the real-edit stale extraction refusal.
- Fan-out: 4/4 passed.
- Fan-out `EDITS_GOLDEN`, `FORCED_COMMIT_GOLDEN`, `ASYNC_COMMIT_GOLDEN`, and `LOD_GOLDEN` output remained unchanged.
- Combined run had one unrelated transient Metal fence timeout in `test_a_body_lands_on_the_streamed_collider_and_sleeps`.

The affected island-body suite was rerun in isolation:

```text
./gdunit_tests.sh -a res://tests/test_island_body.gd
```

Result: 5/5 passed, including the previously timed-out case.

## Scope check

The original Task 10 implementation commit contains 8 files: the seven task files named by
its brief plus this report:

- `extension/src/world/edit_log.h`
- `extension/src/world/world_field.h`
- `extension/src/world/world_field.cpp`
- `extension/src/physics/island_manager.h`
- `extension/src/physics/island_manager.cpp`
- `extension/tests/test_edit_log.cpp`
- `tests/test_connectivity.gd`
- `.superpowers/sdd/2026-09-17-edit-pipeline/task-10-report.md`

`git diff --check` passed. No subagents were dispatched.

## Task 10 fix round: append after snapshot, then consolidation

### Status

Implemented and committed as `a42b36e fix: reject extraction after an edit is consolidated away`.
The landing check now uses `EditLog::last_seq() > InFlight::log_seq` when `ops_since()` returns no
retained operations. This catches an append that consolidation removed before the extraction
landed, while preserving the precise box-overlap check when retained newer operations exist.

Added `test_an_append_after_snapshot_then_consolidation_stays_stale` to
`tests/test_connectivity.gd`. It appends a second edit after an extraction is in flight, forces
that region to consolidate so the edit-log list is empty, and requires the extraction to be
refused as stale.

### RED

With the regression test added before the production fallback, ran:

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd
```

Result: 33 test cases, 1 failure, 0 errors. The new case failed with `land_stale == 0` after the
post-snapshot edit had been consolidated away and the stale extraction landed.

### GREEN and required verification

After the fallback and its regression comment were added:

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd
```

Result: 33/33 passed, 0 errors, 0 failures, 0 flaky.

```text
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Result: passed; universal debug GDExtension linked.

```text
cd extension && scons -Q test
```

Result: 645/645 doctest cases and 9,120,150/9,120,150 assertions passed.

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_island_body.gd,res://tests/test_edit_fanout.gd
```

Result: 3/3 suites, 42/42 cases, 0 errors, 0 failures, 0 flaky. The existing real-edit stale
refusal and consolidation-does-not-make-stale cases both passed. `EDITS_GOLDEN`,
`FORCED_COMMIT_GOLDEN`, `ASYNC_COMMIT_GOLDEN`, and `LOD_GOLDEN` output remained unchanged.

### Concerns

The global fallback intentionally retries for unrelated appends when the spatial query is empty;
this is the documented conservative ceiling. `ponytail:` records the upgrade path: retain append
tombstones/history if that retry cost becomes measurable. No Task 13 lock-order prose or fan-out
golden data was touched.

The fix commit changes 2 source/test files. The report append is a separate documentation commit,
so the fix round is 3 files including this report.
