# M7 Task 5 — per-brick op filtering

## Status

Implemented and benchmarked. The CPU reference, brick generator, and mark pass now filter
region operations by the brick/slab AABB while preserving append order. Errata entry 5 in
`docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md` records the fresh benchmark.

## Files

- `extension/src/generator/edit_ops.h/.cpp` — added `op_touches_aabb`.
- `extension/src/world/brick_eval.cpp` — filters once per brick before lattice, probe, and
  material-projection evaluation.
- `shaders/field.glslh` — added the `FIELD_OP_INDEX` hook and the GPU AABB mirror.
- `shaders/brick_gen.comp.glsl` — ordered per-brick shared-memory compaction.
- `shaders/brick_mark.comp.glsl` — conservative per-workgroup slab compaction, including a
  barrier-safe tail workgroup path.
- `extension/tests/test_op_filter.cpp` — native overlap, pad, and equivalence tests.
- `tests/test_op_filter_gpu.gd` — long-list and order-preservation differential tests.
- `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md` — Errata entry 5.

## TDD evidence

### RED

The new native suite was first run before adding the declaration/implementation:

```text
cd extension && scons test
RED_EXIT_STATUS=2
error: 'op_touches_aabb' is not a member of 've'
```

The first attempt also exposed that this repository uses `<doctest/doctest.h>` rather than
`"doctest.h"`; the test include was corrected before the intended missing-symbol failure.

### GREEN

```text
cd extension && scons test
[doctest] test cases:     303 |     303 passed | 0 failed | 0 skipped
[doctest] assertions: 3961913 | 3961913 passed | 0 failed |
[doctest] Status: SUCCESS!
```

## Verification

Build:

```text
./build.sh -j$(nproc)
==> Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so
==> Done.
```

Required focused GPU/reference suites were run fresh after the build:

| suite | result |
|---|---|
| `test_op_filter_gpu.gd` | 2/2 passed |
| `test_brick_diff.gd` | 6/6 passed |
| `test_field_diff.gd` | 5/5 passed |
| `test_field_volume_diff.gd` | 5/5 passed |
| `test_occupancy.gd` | 4/4 passed |

`test_edit_pipeline.gd` remains pre-existing red, as documented by Task 4: 8 cases,
1 error, 2 failures, in `test_sphere_add_places_material_4_in_open_sky` (`Dictionary` has
no `pos` key). No edit-pipeline file was changed by Task 5.

`git diff --check` completed with no output.

## Benchmark

The first benchmark invocation lacked an active display and aborted in Godot's display
initialization. It was rerun with the available display explicitly selected:

```text
WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task5
```

All five legs exited 0. Wayland reported `vsync_requested=disabled`,
`vsync_actual=disabled`, `verdict_qualified=false`; every budget line remained
`raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN`.

Task 4 round-1 → Task 5 readings (ms):

| leg | Task 4 stream p50/p99 | Task 5 stream p50/p99 | Task 4 raymarch p50/p99 | Task 5 raymarch p50/p99 |
|---|---:|---:|---:|---:|
| steady | 0.003 / 0.004 | 0.003 / 0.004 | 6.838 / 8.927 | 6.835 / 8.945 |
| move | 0.004 / 0.801 | 0.004 / 0.952 | 6.836 / 8.362 | 6.790 / 8.459 |
| ridge | 0.012 / 0.684 | 0.010 / 0.628 | 5.541 / 8.333 | 5.437 / 8.501 |
| edit | 0.867 / 14.786 | 0.236 / 2.977 | 11.296 / 16.888 | 11.129 / 15.798 |
| island | 0.003 / 2.032 | 0.003 / 0.736 | 7.182 / 9.253 | 7.171 / 9.258 |

The edit stream p50/p99 fell 72.8%/79.9%; raymarch changed -1.5%/-6.5%. The filter is
retained. The edit leg emitted `regions=133 overflow=1`; this is existing stream/job
pressure and is not attributed to the AABB filter.

## Concerns

- Benchmark verdicts remain display-qualified on Wayland.
- The required edit-pipeline suite is still red for the pre-existing dictionary-shape bug;
  Task 5 does not touch that pipeline.
- The benchmark's edit leg still reports `overflow=1`, so the result should not be read as
  closure of Task 8's consolidation work.

## Round-1 review fix report

The fix separates the consumer pads: `kBrickFilterPad = 0.20 m` remains for brick
residency/generation, while `kLatticeFilterPad = kVoxelSize + kSdfRange = 0.69 m` is used by
volume extraction, mesh invalidation/collection, and LoD collection. CPU collection and GLSL
filter probes use the same AABB overlap predicate and append/iterate in chronological order.
Shared shader symbols replace the duplicated activation literal.

### TDD and verification commands

```text
cd extension && scons test
[doctest] test cases:     306 |     306 passed | 0 failed | 0 skipped
[doctest] assertions: 3961922 | 3961922 passed | 0 failed |
[doctest] Status: SUCCESS!

./build.sh -j$(nproc)
==> Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so
==> Done.

./gdunit_tests.sh -c -a res://tests/test_op_filter_gpu.gd
4 test cases | 0 errors | 0 failures | 0 skipped | 0 orphans
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd
6 test cases | 0 errors | 0 failures | 0 skipped | 0 orphans
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_field_diff.gd
5 test cases | 0 errors | 0 failures | 0 skipped | 0 orphans
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_field_volume_diff.gd
5 test cases | 0 errors | 0 failures | 0 skipped | 0 orphans
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_occupancy.gd
4 test cases | 0 errors | 0 failures | 0 skipped | 0 orphans
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_mesh_lattice.gd
3 test cases | 0 errors | 0 failures | 0 skipped | 0 orphans
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_mesh_diff.gd
4 test cases | 0 errors | 0 failures | 0 skipped | 0 orphans
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_lod_mesh_diff.gd
3 test cases | 0 errors | 0 failures | 0 skipped | 0 orphans
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd
8 test cases | 1 errors | 2 failures | 0 skipped | 0 orphans
Exit code: 100
SCRIPT ERROR: Invalid access to property or key 'pos' on a base object of type 'Dictionary'.
```

The edit-pipeline result is the pre-existing `test_sphere_add_places_material_4_in_open_sky`
dictionary-shape failure; no edit-pipeline file was changed.

### Fix benchmark

```text
WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task5
```

All five legs exited 0. The exact fresh lines were:

```text
steady: BENCH gpu_stream samples=287 p50_ms=0.003 p99_ms=0.004
        BENCH gpu_raymarch samples=287 p50_ms=6.837 p99_ms=8.966
move:   BENCH gpu_stream samples=287 p50_ms=0.004 p99_ms=1.156
        BENCH gpu_raymarch samples=287 p50_ms=6.775 p99_ms=8.360
ridge:  BENCH gpu_stream samples=287 p50_ms=0.012 p99_ms=0.868
        BENCH gpu_raymarch samples=287 p50_ms=5.535 p99_ms=8.557
edit:   BENCH gpu_stream samples=287 p50_ms=0.236 p99_ms=2.968
        BENCH gpu_raymarch samples=287 p50_ms=11.022 p99_ms=15.934
island: BENCH gpu_stream samples=807 p50_ms=0.003 p99_ms=0.261
        BENCH gpu_raymarch samples=807 p50_ms=7.229 p99_ms=9.243
BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
BENCH regions=133 overflow=1
EXIT_STATUS=0 (all five legs)
```

The edit stream result is effectively unchanged from the prior measurement; the correction
expands correctness coverage without changing the benchmark conclusion.
