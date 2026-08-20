# M7 Task 8 — Round 1: The 257th edit lands

## Status

Implemented round-1 review fixes for asynchronous consolidation. Automatic `_process` pumping now submits/collects worker bake and publication transactions without `run_sync()` or waits. Consolidation is limited to one queued region and remains behind existing extract/mesh/LoD work in `MeshService`; the worker publication is also asynchronous. A bake is refused/requeued unless its region has a valid resident slot, so slot 0 can never stand in for an off-screen region.

Queue/in-flight state, the edit log, CPU override store, override table map, dirty ranges, and policy counters are synchronized with `edit_mutex_`; physics teardown now holds the same lifetime boundary while stopping the worker. Streamer override-map/store reads use the edit lock. Render lifetime admission is held across the automatic transaction so teardown cannot invalidate the resources being used.

Automatic publication is transactional: new CPU slots are released on every refusal/rollback path, the old CPU/table/op state is retained until both consumers report success, and failed transactions are requeued or counted when the queue cannot accept them. Queue-cap refusal is logged once. `max_override_bricks` is wired through the CPU store, render `GpuAtlas`, worker `MeshPass`, and consolidation staging capacity; zero/negative values fail soft.

## Files

- `extension/src/voxel_world.cpp/.h` — nonblocking pump/publication state machine, resident-slot validation, locks, counters, queue refusal logging, capacity export, debug async hooks.
- `extension/src/render/mesh_service.cpp/.h` — asynchronous worker override publication with worker-side rollback; consolidation scheduling remains after existing Task 7 queues.
- `extension/src/render/gpu_atlas.cpp/.h` — configured override capacity validation and render pool sizing.
- `extension/src/render/mesh_pass.cpp/.h` — worker override pool capacity wiring.
- `extension/src/render/world_streamer.cpp/.h` — locked forced-regeneration entry point and synchronized CPU override/table reads.
- `tests/test_consolidation.gd` — stronger 300-edit policy assertions, no-resident-slot refusal, async sequence-tail integration, invalid/capped capacity assertions.

## TDD evidence

### RED

Command:

```text
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
```

Observed before the fixes:

```text
14 test cases | 2 errors | 2 failures
Invalid access to property or key 'edit_rejections' on a base object of type 'Dictionary'.
Expecting: false but is true (automatic consolidation used the unsafe slot-0 fallback)
Invalid call. Nonexistent function 'debug_pump_consolidation_async' in base 'VoxelWorld'.
Exit code: 100
```

The new native sequence-tail unit test was already present and passing; the new GDScript integration test specifically exercises an edit appended after asynchronous submission.

### GREEN

Command:

```text
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
```

Exact final result:

```text
Statistics: 14 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 16s 240ms
Overall Summary: 14 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0
```

The policy test now asserts `edit_rejections == 0`, a nonzero consolidation count, final op count below the trigger, and a final raycast hit. The async integration test asserts exactly one tail op remains after the captured prefix clears.

## Fresh verification

| Command | Result |
|---|---|
| `./build.sh -j$(nproc)` | Build OK; `4.7M` debug GDExtension |
| `cd extension && scons test` | `315/315` doctest cases and `3962244/3962244` assertions passed |
| `./gdunit_tests.sh -c -a res://tests/test_consolidation.gd` | `14/14` passed |
| `./gdunit_tests.sh -c -a res://tests/test_collider_edits.gd` | `3/3` passed |
| `./gdunit_tests.sh -c -a res://tests/test_lod_stream.gd` | `3/3` passed |
| `./gdunit_tests.sh -c -a res://tests/test_streaming.gd` | `6/6` passed |
| `git diff --check` | clean |

`./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd` was also run and remains a pre-existing failure: `test_sphere_add_places_material_4_in_open_sky` reports the existing missing `pos` key and two existing false raycast assertions. It was not changed.

## Benchmark / Errata entry 6 evidence

Command:

```text
WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task8-round1
```

All five benchmark processes exited `0`. The fresh edit leg reported:

```text
BENCH gpu_stream samples=287 p50_ms=0.243 p99_ms=2.996
BENCH gpu_raymarch samples=287 p50_ms=11.103 p99_ms=15.977
BENCH gpu_custom_frame samples=287 p50_ms=12.572 p99_ms=18.656
BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
BENCH regions=133 overflow=1 overrides=0/8192 consolidations=0 refusals=0
```

The required 3x edit run (temporary `FRAMES := 900`, restored afterward) reported:

```text
BENCH mode=--benchmark-edit frames=900
BENCH gpu_stream samples=807 p50_ms=0.241 p99_ms=0.972
BENCH gpu_raymarch samples=807 p50_ms=15.361 p99_ms=24.858
BENCH gpu_custom_frame samples=807 p50_ms=16.931 p99_ms=26.672
BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
BENCH regions=124 overflow=1 overrides=0/8192 consolidations=0 refusals=168
```

The moving benchmark does not reach a successful consolidation because its edits are distributed across regions; therefore it is not evidence of a consolidation performance PASS. The focused policy suite is the correctness evidence. V-Sync was requested disabled but actual Wayland V-Sync was enabled, so frame percentiles are unqualified. Existing X11/GTK/FIFO warnings, the Wayland V-Sync fallback, and two ObjectDB leak warnings remain environmental/pre-existing concerns.

## Concerns

- The benchmark's `overflow=1` is the existing atlas/job overflow counter; the focused 300-edit policy test reports `overflow_ever=0` and no edit-log rejection.
- Automatic consolidation only publishes resident regions by design; off-screen queued work is deferred and retried rather than guessing a region slot.
- The benchmark remains over the frame target under this Wayland/V-Sync environment; no unrelated physics/renderer tuning was attempted.

## Round 2 re-review — resolved

The re-review items are addressed as follows:

1. `WorldStreamer::run_frame()` now reads the override table ID and scans the matching override entries in one `edit_mutex_` critical section. Atlas/worker publication happens only after that coherent snapshot is complete.
2. `teardown_physics()` restores staged old atlas bytes/table, invalidates the region table if byte restoration fails, releases every newly acquired override brick, then clears transaction state and requeues the still-live edit prefix.
3. Worker async publication rollback now replays the retained old bytes/table, with an initial attempt plus three retries. If all attempts fail, the worker is marked non-authoritative and refuses work; the main thread rebuilds it from the CPU store/table map before requeueing. Corrupted worker bytes are never treated as authoritative.
4. `debug_fill_override_pool()` refuses when its target region has no resident slot; it no longer substitutes slot 0. Partial fixture setup is also released/cleared on failure.

Added regression coverage:
- `test_debug_fill_refuses_offscreen_region_without_touching_slot_zero`
- `test_teardown_releases_staged_override_slots_before_reinit`

## Round 2 fresh verification

Exact command result lines from this round:

```text
./build.sh -j$(nproc)
==> Build OK: 4.7M libvoxel_everything.linux.template_debug.x86_64.so
==> Done.

cd extension && scons test
[doctest] test cases:     315 |     315 passed | 0 failed | 0 skipped
[doctest] assertions: 3962244 | 3962244 passed | 0 failed |
[doctest] Status: SUCCESS!
scons: done building targets.

./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
Statistics: 16 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 16 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

./gdunit_tests.sh -c -a res://tests/test_streaming.gd
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

./gdunit_tests.sh -c -a res://tests/test_island_render.gd
Statistics: 14 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 14 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

./gdunit_tests.sh -c -a res://tests/test_collider_edits.gd
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

./gdunit_tests.sh -c -a res://tests/test_lod_stream.gd
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0
```

The first LoD command was bounded at 240 seconds and timed out while its intentionally expensive first case was still running; it was rerun with a 600-second timeout and completed successfully in `4min 32s 262ms`. No source change was made for that pre-existing duration. The existing `test_edit_pipeline.gd` failures from round 1 were not rerun or changed.
