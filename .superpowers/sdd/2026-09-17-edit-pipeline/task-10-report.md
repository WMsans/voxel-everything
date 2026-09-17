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

Changed files are limited to the seven files named by the brief:

- `extension/src/world/edit_log.h`
- `extension/src/world/world_field.h`
- `extension/src/world/world_field.cpp`
- `extension/src/physics/island_manager.h`
- `extension/src/physics/island_manager.cpp`
- `extension/tests/test_edit_log.cpp`
- `tests/test_connectivity.gd`

`git diff --check` passed. No subagents were dispatched.
