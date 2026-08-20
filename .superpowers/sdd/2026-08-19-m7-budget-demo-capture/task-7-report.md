# M7 Task 7 — GPU override pool and consolidation bake

## Status

Implemented the GPU mirror and worker-device consolidation path. The render atlas and worker meshing device now expose packed override SDF/material pools, 32 region tables, and a region-to-table map. The worker runs `brick_consolidate.comp.glsl`, reads back baked `OverrideBrick` values, and the world publishes them to CPU, render, mesh, and LoD consumers. Consolidated edits are cleared only after a successful bake/publication; overrides replace the analytic base and do not consume the edit-op list.

The prescribed GdUnit test required one compatibility adjustment: the existing Task 2–6 `debug_op_counts()` API returns its SSBO `RID` and is consumed by existing tests, while the brief's snippet treats it as a Dictionary. The new test uses `debug_region_op_count()` for the same post-consolidation assertion without changing the established API.

## Files

- `extension/src/render/override_pool.h/.cpp` — packed byte SSBO pool, 32 tables, region table map, uploads and publication.
- `extension/src/render/consolidate_pass.h/.cpp` — worker compute pass and baked-brick readback.
- `extension/src/render/gpu_atlas.*` — render-device override resources.
- `extension/src/render/mesh_pass.*`, `lod_build_pass.*`, `island_extract_pass.*` — worker override bindings and shared worker pool.
- `extension/src/render/mesh_service.*` — consolidation queue, worker execution, collection and publication.
- `extension/src/voxel_world.*` — CPU override store, debug bake/diff/consolidate hooks and CPU field/raycast threading.
- `shaders/field.glslh`, `brick_consolidate.comp.glsl`, `brick_gen.comp.glsl`, `brick_mark.comp.glsl`, `mesh_field.comp.glsl`, `lod_field.comp.glsl`, `island_extract.comp.glsl`, plus push-constant common headers.
- `tests/test_consolidation.gd` — bake and post-consolidation raycast coverage.

## TDD evidence

### RED

```text
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
Invalid call. Nonexistent function 'debug_consolidate_diff' in base 'VoxelWorld'.
Invalid call. Nonexistent function 'debug_consolidate_region' in base 'VoxelWorld'.
2 test cases | 2 errors | 0 failures
```

The first attempt also exposed the pre-existing `debug_op_counts()` RID-vs-Dictionary API mismatch in the brief snippet; the test was corrected to use the non-invasive `debug_region_op_count()` hook before the production implementation.

### GREEN

```text
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
2 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped
Exit code: 0
```

## Fresh verification

| command | result |
|---|---|
| `./build.sh -j$(nproc)` | Build OK, 4.6M debug GDExtension |
| `cd extension && scons test` | 314/314 doctest cases; 3,962,241/3,962,241 assertions |
| `test_consolidation.gd` | 2/2 passed |
| `test_brick_diff.gd` | 6/6 passed |
| `test_mesh_diff.gd` | 4/4 passed |
| `test_lod_mesh_diff.gd` | 3/3 passed |
| `test_island_extract.gd` | 5/5 passed |
| `test_field_diff.gd` | 5/5 passed |
| `test_op_filter_gpu.gd` | 4/4 passed |
| `test_field_volume_diff.gd` | 5/5 passed |
| `git diff --check` | clean |

No benchmark was assigned by the Task 7 brief, so no benchmark measurement was recorded.

## Errata / concerns

- GPU tests use the available NVIDIA Vulkan device through the Wayland fallback; Godot emits the existing X11-unavailable/GTK/FIFO warnings and two ObjectDB leak warnings at test shutdown, but each suite exits 0 with zero test errors/failures.
- The brief's `debug_op_counts()` Dictionary example conflicts with the established RID-returning API used by Task 2–6 tests; the compatibility hook avoids breaking those consumers.
- The fixed pool is fail-soft at 8192 bricks and the CPU publication path enforces the 32-table cap. Future automatic consolidation scheduling remains Task 8 work.
