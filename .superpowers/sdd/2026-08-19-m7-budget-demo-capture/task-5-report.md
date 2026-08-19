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
