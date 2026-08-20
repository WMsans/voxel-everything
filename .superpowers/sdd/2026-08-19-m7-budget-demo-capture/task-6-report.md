# M7 Task 6 — override brick store and field base

## Status

Implemented the pure `world/override_store` pool, lookup, encoded-lattice sampler, deterministic consolidation planner, edit-log region clearing, override-aware CPU field evaluation, and override-threaded raycast path. `EditOp` remains the existing 32-byte record; overrides are stored field bases and are never added to op lists.

## Files

- `extension/src/world/override_store.h/.cpp` — fixed-capacity fail-soft override pool, brick lookup/sampling, and region plan.
- `extension/src/world/brick_eval.h/.cpp` — override base source threaded through field, probe, brick, and material evaluation while retaining Task 5's consumer-specific filtering and op order.
- `extension/src/world/raycast.h/.cpp` — override source threaded through march and normal taps.
- `extension/src/world/edit_log.h/.cpp` — `clear_region` erases the region's ops and sequence list without resetting global chronology.
- `extension/src/world/region.h/.cpp` — world-space brick AABB helper used by the plan and test oracle.
- `extension/tests/test_override_store.cpp` — pool, fail-soft capacity, encoded sampling, plan coverage, override precedence, and post-base-op tests.

## TDD evidence

### RED

The prescribed focused test was added before the implementation and run with:

```text
cd extension && scons test
```

It failed at test compilation with the expected missing production header:

```text
tests/test_override_store.cpp:2:10: fatal error: world/override_store.h: No such file or directory
```

The repository uses `<doctest/doctest.h>` rather than the brief's shorthand include. The doctest bit-shift assertions were parenthesized for doctest 2.4.11's expression parser. The sampling assertion also compares against `decode_sdf(encode_sdf(want.sdf))`, because the specified `uint8_t` SDF storage necessarily clamps values outside `kSdfRange`.

### GREEN

The native suite passed after the implementation:

```text
cd extension && scons test
[doctest] test cases:     313 |     313 passed | 0 failed | 0 skipped
[doctest] assertions: 3962240 | 3962240 passed | 0 failed |
[doctest] Status: SUCCESS!
```

## Verification

Build:

```text
./build.sh -j$(nproc)
==> Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so
==> Done.
```

Focused Godot/GPU differential and related suites were run fresh after the build:

| suite | result |
|---|---|
| `test_op_filter_gpu.gd` | 4/4 passed |
| `test_brick_diff.gd` | 6/6 passed |
| `test_field_diff.gd` | 5/5 passed |
| `test_field_volume_diff.gd` | 5/5 passed |
| `test_occupancy.gd` | 4/4 passed |
| `test_edit_pipeline.gd` | 5 passed, 1 error, 2 failures — pre-existing dictionary-shape failure in `test_sphere_add_places_material_4_in_open_sky` (`Dictionary` has no `pos` key), unchanged by this task |

`git diff --check` completed with no output.

## Benchmark

The required benchmark was run fresh with:

```text
WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task6
```

All five legs exited 0. Wayland reported `vsync_requested=disabled`, `vsync_actual=disabled`, and `verdict_qualified=false`; budget verdicts were `raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN` for every leg.

| leg | gpu_stream p50/p99 ms | gpu_raymarch p50/p99 ms | regions | overflow |
|---|---:|---:|---:|---:|
| steady | 0.003 / 0.005 | 6.835 / 8.917 | 154 | 0 |
| move | 0.004 / 0.959 | 6.764 / 8.662 | 162 | 0 |
| ridge | 0.011 / 0.721 | 5.439 / 8.783 | 175 | 0 |
| edit | 0.237 / 2.956 | 11.174 / 15.898 | 133 | 1 |
| island | 0.003 / 0.794 | 7.193 / 9.257 | 154 | 0 |

The task brief does not assign benchmark measurements to an Errata entry, so no Errata entry was added.

## Round 1 fix — override propagation and cap boundary

### Root cause

`eval_brick` accepted `const OverrideSource *overrides`, and its material projection already forwarded it, but the 17³ SDF/material lattice loop called `eval_field` with only `volumes`. After consolidation cleared the op list, brick generation therefore evaluated `G` instead of the stored override base. The fix forwards `overrides` through that lattice evaluation.

### Regression TDD evidence

The native regression test was added before the production fix:

```text
$ cd extension && scons test
[doctest] test cases:     314 |     313 passed | 1 failed | 0 skipped
[doctest] assertions: 3962241 | 3962240 passed | 1 failed |
[doctest] Status: FAILURE!
tests/test_override_store.cpp:157: ERROR: CHECK( evaluated.brick.sdf[ve::sdf_index(0, 0, 0)] == ve::encode_sdf(-0.5f) ) is NOT correct!
  values: CHECK( 255 == 28 )
scons: *** [test] Error 1
```

After the one-line propagation fix:

```text
$ cd extension && scons test
[doctest] test cases:     314 |     314 passed | 0 failed | 0 skipped
[doctest] assertions: 3962241 | 3962241 passed | 0 failed |
[doctest] Status: SUCCESS!
scons: done building targets.
```

### Fresh fix verification

```text
$ ./build.sh -j$(nproc)
==> Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so
==> Done.

$ ./gdunit_tests.sh -c -a res://tests/test_op_filter_gpu.gd
Overall Summary: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0

$ ./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd
Overall Summary: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0

$ ./gdunit_tests.sh -c -a res://tests/test_field_diff.gd
Overall Summary: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0

$ ./gdunit_tests.sh -c -a res://tests/test_field_volume_diff.gd
Overall Summary: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0

$ ./gdunit_tests.sh -c -a res://tests/test_occupancy.gd
Overall Summary: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

All focused suites used the NVIDIA Vulkan device. Godot emitted the existing X11-to-Wayland fallback and two ObjectDB leak warnings; each command exited 0 with zero test errors/failures.

### 32-region cap boundary

The M7 plan's fixed number is 32 simultaneous consolidated regions (`kMaxOverrideTables`), but the Task 6 brief does not expose a region-table or region-allocation API. Task 7 owns the render/worker override tables and their publication; Task 8 owns the consolidation queue cap, trigger, and fail-soft refusal/statistics. Task 6 therefore keeps its fail-soft `OverrideStore` capacity refusal at the **brick-pool** level and does not invent or duplicate a 32-region API. The Task 7/8 implementation must enforce the 32-table boundary before clearing an op list.

## Concerns

- `test_edit_pipeline.gd` retains the pre-existing 1-error/2-failure dictionary-shape issue; no edit-pipeline code was changed.
- Benchmark timing is Wayland-qualified because the available display falls back to V-Sync; all benchmark legs nevertheless exited 0.
- The provided sampling fixture's raw expected SDF is outside the representable `[-kSdfRange, +kSdfRange]` byte range; the test records the required encoded-storage comparison rather than claiming lossless representation outside that range.
- Task 6 does not implement Task 7 GPU integration or the Task 8 32-region queue/table policy; that boundary is recorded above.
