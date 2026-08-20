# Task 10 — occupancy from the generated lattice

## Status

Implemented and committed in this branch as `fix: occupancy comes from the generated lattice, not a 27-sample probe`.
Generated bricks now publish occupancy from the exact 17³ SDF lattice they just wrote;
`brick_mark` only publishes the unambiguous air/full result for bricks it does not generate.
The CPU reference has the same encoded-min/max rule and keeps the old probe classifier as
`cell_state_probe`.

## Files touched

- `shaders/common.glslh`
- `shaders/brick_gen.comp.glsl`
- `shaders/brick_mark.comp.glsl`
- `shaders/region_free.comp.glsl`
- `extension/src/render/brick_gen_pass.cpp`
- `extension/src/world/brick_eval.h`
- `extension/src/world/brick_eval.cpp`
- `extension/src/voxel_world.h`
- `extension/src/voxel_world.cpp`
- `extension/tests/test_brick_eval.cpp`
- `tests/test_occupancy_lattice.gd`
- `docs/todo/opti.md`

## Implementation

- Moved `OCC_WORDS_PER_REGION` to `common.glslh`, removing duplicate shader definitions.
- Bound `GpuAtlas::region_occupancy()` at binding 15 in `BrickGenPass`.
- `brick_gen` uses the final lattice reduction (`mn`/`mx`) and the job's `rslot` plus brick
  coordinate to atomically replace the two occupancy bits for the generated brick.
- `brick_mark` writes only `CELL_AIR` or `CELL_FULL` when `!has_surface`; generated bricks
  are overwritten by the generator after the mark-to-generation barrier.
- Added `cell_state_probe` for the conservative mark semantics. `cell_state_field` now
  evaluates `eval_brick`, reduces its encoded lattice, and classifies with the same zero byte
  (`encode_sdf(0) == 128`) as the GPU.
- Added `debug_pump_occupancy()` and `debug_occupancy_diff(region)` diagnostics. The latter
  streams the requested region, compares every resident GPU brick byte with the CPU rule,
  and reports `compared`, `mismatches`, and the first mismatch brick.

## TDD evidence

### RED

The new GDScript suite was run before the helpers existed:

```text
./gdunit_tests.sh -c -a res://tests/test_occupancy_lattice.gd
Invalid call. Nonexistent function 'debug_pump_occupancy' in base 'VoxelWorld'.
2 test cases | 2 errors | 0 failures
Exit code: 100
```

The native regression was then added before changing `cell_state_field`. Against the old
probe implementation it failed for a 25 cm-radius cavity between probe samples:

```text
TEST CASE: cell_state_field follows the generated lattice for a thin carve
ERROR: CHECK( ve::cell_state_field(gen, &cut, 1, cell) == expected ) is NOT correct!
values: CHECK( 3 == 2 )
321 test cases | 320 passed | 1 failed
```

### GREEN

Fresh focused native run:

```text
cd extension && scons test
[doctest] test cases:     321 |     321 passed | 0 failed | 0 skipped
[doctest] assertions: 3962274 | 3962274 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Fresh lattice/GPU differential run:

```text
./gdunit_tests.sh -c -a res://tests/test_occupancy_lattice.gd
Statistics: 2 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 2 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

The existing occupancy suite also passed 4/4. The new differential compared more than 100
resident bricks and reported zero mismatches.

## Required connectivity/reproduction verification

The required combined command was run fresh:

```text
./gdunit_tests.sh -c \
  -a res://tests/test_occupancy.gd \
  -a res://tests/test_connectivity.gd \
  -a res://tests/test_repro_thin_sheet.gd \
  -a res://tests/test_repro_pillar_debris.gd \
  -a res://tests/test_island_body.gd
```

Results:

- `test_occupancy.gd`: 4/4 passed.
- `test_repro_pillar_debris.gd`: 1/1 passed.
- `test_island_body.gd`: 5/5 passed.
- `test_connectivity.gd`: 33 cases, 12 failures.
- `test_repro_thin_sheet.gd`: 1 case, 2 assertion failures.
- Combined: 44 cases, 14 failures, exit 100.

The 12 connectivity failures are in existing re-merge/body-pool scenarios; none of those
sources changed in this task. The prior Task 9 repository-wide gate already recorded the
connectivity suite as failing outside its affected collider set. They remain an open branch
baseline concern, not claimed as Task 10 regressions.

### Errata 8 — round 1 activation correction

The original report was incomplete: exact occupancy alone did not fix a probe-missed edit,
because `brick_mark` returned before `brick_gen`. Round 1 adds a distinct edit mark mode that
keeps the touched brick resident and queues it even when the 27-sample activation probe misses
the surface. The generator then owns the occupancy result from its 17³ lattice. Plain
stream-in still uses the probe only for the unambiguous no-surface fallback.

The focused 10 cm sheet regression now passes with 64/64 shell samples negative and 98
matter-containing cells reported solid/full, with zero reported air cells. The test disables
physics/island stepping so body extraction does not intentionally remove the diagnostic shell;
connectivity behavior remains covered by its separate suites.

## Benchmark / Errata 9

Command:

```text
env WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 \
  tools/run_benchmarks.sh m7-task10-final
```

All five benchmark processes exited 0 on Godot 4.7.1 / Vulkan 1.4.341 / NVIDIA GeForce RTX
4070 Laptop GPU. Wayland rejected disabled V-Sync, so `verdict_qualified=false` was reported.
The fresh frame p99 / `brick_gen` `build_ms` max / GPU raymarch p99 evidence is:

| leg | frame p99 | max build_ms | GPU raymarch p99 |
|---|---:|---:|---:|
| steady | 23.71 ms | 0.50 ms | 8.937 ms |
| move | 31.66 ms | 0.65 ms | 8.370 ms |
| ridge | 32.04 ms | 0.37 ms | 8.899 ms |
| edit | 73.17 ms | 0.61 ms | 15.836 ms |
| island | 31.05 ms | 0.50 ms | 9.196 ms |

The benchmark verdict remained `raymarch=WARN ... frame=WARN`; the occupancy change does
not close the existing frame/raymarch budgets. The run retained the known GTK, Wayland FIFO,
mouse, and ObjectDB shutdown warnings. No visual acceptance claim is made.

## Final verification / commit

The final verification command was run after the report update:

```text
./build.sh -j$(nproc)
cd extension && scons test
./gdunit_tests.sh -c -a res://tests/test_occupancy_lattice.gd
./gdunit_tests.sh -c -a res://tests/test_occupancy.gd
./gdunit_tests.sh -c -a res://tests/test_repro_pillar_debris.gd
./gdunit_tests.sh -c -a res://tests/test_island_body.gd
git diff --check
```

Fresh final output:

```text
./build.sh -j$(nproc)
==> Build OK: 4.7M libvoxel_everything.linux.template_debug.x86_64.so

cd extension && scons test
[doctest] test cases: 321 | 321 passed | 0 failed | 0 skipped
[doctest] assertions: 3962274 | 3962274 passed | 0 failed |

./gdunit_tests.sh -c -a res://tests/test_occupancy_lattice.gd
Overall Summary: 2 test cases | 0 errors | 0 failures | 0 skipped | PASSED

./gdunit_tests.sh -c -a res://tests/test_occupancy.gd
Overall Summary: 4 test cases | 0 errors | 0 failures | 0 skipped | PASSED

./gdunit_tests.sh -c -a res://tests/test_repro_pillar_debris.gd
Overall Summary: 1 test cases | 0 errors | 0 failures | 0 skipped | PASSED

./gdunit_tests.sh -c -a res://tests/test_island_body.gd
Overall Summary: 5 test cases | 0 errors | 0 failures | 0 skipped | PASSED

git diff --check
# clean
```

### Round 1 review response

- Activation false negative: fixed by the edit-only mark mode described in Errata 8; stream-in
  mode remains probe-based and does not allocate probe-missed empty bricks.
- Lattice regression: `test_occupancy_lattice.gd` now places a 0.25 m carve at a 0.2 m lattice
  offset from the brick origin, 0.346 m from the nearest probe sample, and asserts literal
  `kCellSolid` plus the CPU rule.
- Fallback parity: `debug_occupancy_fallback_diff` force=false marks a resident region,
  synchronously reads the occupancy block before generation, and compares every no-surface
  result with `cell_state_probe`; the test requires >100 fallback cells and zero mismatches.
- Pump contract: `debug_pump_occupancy` harvests already-issued async readbacks and folds them;
  it does not advance streaming or issue a mark. Tests drive frames separately.

The known connectivity failures are explicitly not represented as green; they remain the
pre-existing re-merge/body-pool failures documented below.

Fresh round 1 verification after the helper-boundary fix:

```text
./build.sh -j$(nproc)                         Build OK
./gdunit_tests.sh -c -a res://tests/test_occupancy_lattice.gd   3 passed, 0 failed
./gdunit_tests.sh -c -a res://tests/test_occupancy.gd            4 passed, 0 failed
./gdunit_tests.sh -c -a res://tests/test_repro_thin_sheet.gd    1 passed, 0 failed
./gdunit_tests.sh -c -a res://tests/test_connectivity.gd        33 cases, 12 failures (known baseline)
./gdunit_tests.sh -c -a res://tests/test_repro_pillar_debris.gd 1 passed, 0 failed
./gdunit_tests.sh -c -a res://tests/test_island_body.gd         5 passed, 0 failed
cd extension && scons test                              321 passed, 0 failed
git diff --check                                      clean
```
