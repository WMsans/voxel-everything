# M7 Task 7 — GPU override pool and consolidation bake

## Status

Implemented the GPU mirror and worker-device consolidation path. The render atlas and worker meshing device now expose packed override SDF/material pools, 32 region tables, and a region-to-table map. CPU overrides are replayed into both newly created pools after render/physics reinit; eviction clears the render and worker region-slot mappings before reuse. Production island extraction now carries its override table. Consolidation publication is transactional across render and worker uploads, with fail-soft rollback, explicit region regeneration, collision dirtying, and LoD-chain invalidation. Consolidated edits are cleared only after a successful bake/publication; overrides replace the analytic base and do not consume the edit-op list.

The prescribed GdUnit test required one compatibility adjustment: the existing Task 2–6 `debug_op_counts()` API returns its SSBO `RID` and is consumed by existing tests, while the brief's snippet treats it as a Dictionary. The new test uses `debug_region_op_count()` for the same post-consolidation assertion without changing the established API.

## Files

- `extension/src/render/override_pool.h/.cpp` — packed byte SSBO pool, 32 tables, region table map, uploads and publication.
- `extension/src/render/consolidate_pass.h/.cpp` — worker compute pass and baked-brick readback.
- `extension/src/render/gpu_atlas.*` — render-device override resources.
- `extension/src/render/mesh_pass.*`, `lod_build_pass.*`, `island_extract_pass.*` — worker override bindings and shared worker pool.
- `extension/src/render/mesh_service.*` — consolidation queue, worker execution, collection, replay, eviction reset, and transactional publication.
- `extension/src/render/world_streamer.*` — override table replay on load, stale mapping clear on eviction/reuse, and forced post-consolidation regeneration.
- `extension/src/voxel_world.*` — CPU override store, device replay, debug bake/diff/consolidate hooks, rollback, dirty fan-out, and CPU field/raycast threading.
- `shaders/field.glslh`, `brick_consolidate.comp.glsl`, `brick_gen.comp.glsl`, `brick_mark.comp.glsl`, `mesh_field.comp.glsl`, `lod_field.comp.glsl`, `island_extract.comp.glsl`, plus push-constant common headers.
- `tests/test_consolidation.gd` — bake, reconsolidation, full-pool refusal, fail-bake/fail-publication rollback, render-pool replay, worker-pool replay, and GPU mesh-consumer coverage.

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
3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped
Exit code: 0
```

## Fresh verification

| command | result |
|---|---|
| `./build.sh -j$(nproc)` | Build OK, 4.7M debug GDExtension |
| `cd extension && scons test` | 314/314 doctest cases; 3,962,241/3,962,241 assertions |
| `./gdunit_tests.sh -c -a res://tests/test_consolidation.gd` | 8/8 passed |
| `./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd` | 6/6 passed |
| `./gdunit_tests.sh -c -a res://tests/test_mesh_diff.gd` | 4/4 passed |
| `./gdunit_tests.sh -c -a res://tests/test_lod_mesh_diff.gd` | 3/3 passed |
| `./gdunit_tests.sh -c -a res://tests/test_island_extract.gd` | 5/5 passed |
| `./gdunit_tests.sh -c -a res://tests/test_field_diff.gd` | 5/5 passed |
| `git diff --check` | clean |

No benchmark was assigned by the Task 7 brief, so no benchmark measurement was recorded.

## Errata / concerns

- GPU tests use the available NVIDIA Vulkan device through the Wayland fallback; Godot emits the existing X11-unavailable/GTK/FIFO warnings and two ObjectDB leak warnings at test shutdown, but each suite exits 0 with zero test errors/failures.
- The brief's `debug_op_counts()` Dictionary example conflicts with the established RID-returning API used by Task 2–6 tests; the compatibility hook avoids breaking those consumers.
- The fixed pool is fail-soft at 8192 bricks and the CPU publication path enforces the 32-table cap. Future automatic consolidation scheduling remains Task 8 work.

## Round 2 fresh command output

```text
$ ./build.sh -j$(nproc)
==> Build OK: 4.7M libvoxel_everything.linux.template_debug.x86_64.so
BUILD_EXIT=0

$ ./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
Statistics: 8 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 10s 800ms
Overall Summary: 8 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0
CONSOLIDATION_EXIT=0

$ ./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 3s 423ms
Overall Summary: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

$ ./gdunit_tests.sh -c -a res://tests/test_mesh_diff.gd
Statistics: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 2s 129ms
Overall Summary: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

$ ./gdunit_tests.sh -c -a res://tests/test_lod_mesh_diff.gd
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 5s 359ms
Overall Summary: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

$ ./gdunit_tests.sh -c -a res://tests/test_island_extract.gd
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 2s 563ms
Overall Summary: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

$ ./gdunit_tests.sh -c -a res://tests/test_field_diff.gd
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 1s 207ms
Overall Summary: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

$ ./gdunit_tests.sh -c -a res://tests/test_mesh_stream.gd
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 2s 519ms
Overall Summary: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

$ ./gdunit_tests.sh -c -a res://tests/test_lod_stream.gd
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 13s 266ms
Overall Summary: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

$ ./gdunit_tests.sh -c -a res://tests/test_streaming.gd
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 3s 415ms
Overall Summary: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
Run tests ends with 0

$ (cd extension && scons test)
[doctest] test cases:     314 |     314 passed | 0 failed | 0 skipped
[doctest] assertions: 3962241 | 3962241 passed | 0 failed |
[doctest] Status: SUCCESS!
NATIVE_EXIT=0
```

## Round 3 changes

- `MeshService` override table changes now use a non-blocking worker queue; `WorldStreamer::run_frame()` contains no `run_sync()` call and queued replay/invalidation is ordered ahead of worker consumers.
- Consolidation rollback checks `restore_overrides()`, restores both mesh and LoD worker consumers, retries a failure without releasing new CPU slots, and has a one-shot failure-injection assertion.
- Full-pool coverage publishes all 8192 override slots before refusal; render replay compares table entry plus SDF/material bytes, and LoD replay compares generated lattice data.
- Automatic consolidation scheduling remains deferred to Task 8.

## Round 3 concerns

- GPU suites use the existing Wayland fallback and emit the pre-existing X11/GTK/FIFO warnings; shutdown still reports two ObjectDB leaks while all suites exit 0.
- The restore failure-injection test intentionally logs `VoxelWorld: worker override rollback failed; retrying` before the compensating retry succeeds.
