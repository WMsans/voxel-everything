# Task 9 — Round 1 review fixes

## Status

Implemented the round-1 review fixes for the collider octant split.

- Overflowed results with no usable geometry now call `note_failed`, retain the previous
  collider bodies, and retry; they never cache `note_empty`.
- Mesh indices are filtered for complete triangles and uint32 vertex bounds before centroid
  splitting; the de-index path repeats the unsigned bounds check before reading positions.
- Octant splitting retains source triangle order and the established Jolt winding swap.
  All PhysicsServer3D/Jolt calls remain on the existing main-thread `run_frame` path.
- `body_of_slot` finds any populated octant instead of assuming octant zero is populated.
- `bodies` remains the resident-chunk count; `bodies_raw` reports bodies actually in the space.
  `max_build_tris` only counts a real single-shape payload.
- `build_ms` and `phys_setdata_ms` are per-frame maxima of individual calls, so
  `BENCH max_ms build_ms` is a true per-call maximum rather than an accumulated frame total.
- Added native multi-octant coverage and Godot coverage for atomic replacement, collision
  agreement, raw/chunk body diagnostics, and the explicit `test_collider_stream.gd` contract.

## Files touched

- `demo/benchmark.gd`
- `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md`
- `docs/todo/opti.md`
- `extension/src/physics/collider_streamer.cpp`
- `extension/src/physics/collider_streamer.h`
- `extension/src/voxel_world.cpp`
- `extension/tests/test_octant_split.cpp`
- `tests/test_collider_octants.gd`
- `tests/test_collider_stream.gd`

## TDD evidence

### RED

Command, run against the pre-fix extension after adding the regression assertions:

```text
./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd
```

Exact relevant result:

```text
Expecting: 'true' but is 'false' at 'test_octant_bodies_replace_atomically_and_report_diagnostics' in res://tests/test_collider_octants.gd:80
Expecting: 'true' but is 'false' at 'test_octant_bodies_replace_atomically_and_report_diagnostics' in res://tests/test_collider_octants.gd:92
Statistics: 3 test cases | 0 errors | 2 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 3 test cases | 0 errors | 2 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 100
```

The failures were the old `body_of_slot` octant-zero assumption: the selected resident
chunk had a valid populated octant, but octant zero was empty, so both old and replacement
body diagnostics reported an invalid RID.

### GREEN

Native command:

```text
cd extension && scons test
```

Exact result:

```text
[doctest] test cases:     319 |     319 passed | 0 failed | 0 skipped
[doctest] assertions: 3962265 | 3962265 passed | 0 failed |
[doctest] Status: SUCCESS!
scons: done building targets.
```

Focused Godot commands and exact result lines:

```text
./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_collider_stream.gd
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_collider_edits.gd
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_player_kick.gd
Statistics: 1 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 1 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0

./gdunit_tests.sh -c -a res://tests/test_island_body.gd
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

The native test now includes 319 cases / 3,962,265 assertions, including the new test
that populates all eight octants. The octant Godot test includes the ray-vs-field oracle,
max single-build diagnostic, and an edit/remesh atomic replacement with raw/chunk body
and valid-RID assertions.

## Build checks

```text
./build.sh -j$(nproc)
==> Build OK: 4.7M libvoxel_everything.linux.template_debug.x86_64.so
==> Done.

cd extension && scons target=template_release -j$(nproc)
scons: done building targets.
```

The combined final verification also ran `git diff --check` with no output.

## Benchmark evidence and Errata 7

Command:

```text
env WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 tools/run_benchmarks.sh m7-task9-round1-final
```

All five benchmark processes exited `0`. Environment: Godot 4.7.1, Vulkan 1.4.341,
NVIDIA GeForce RTX 4070 Laptop GPU, Wayland, requested 2560x1440, V-Sync requested
disabled but unavailable to the compositor. The logs report
`display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`
and the runtime logs the V-Sync fallback, so frame percentiles are qualified.

Exact move evidence:

```text
BENCH p50=18.75 p95=23.86 p99=29.03 max=47.76 min_fps=20.9 over_16.6ms=277 (92.3%)
BENCH max_ms build_ms=0.57 island_ms=0.02 lod_ms=0.08 phys_apply_ms=25.04 phys_body_ms=0.05 phys_collect_ms=1.01 phys_faces_ms=0.41 phys_plan_ms=4.02 phys_setdata_ms=0.33 phys_submit_ms=0.01 phys_tris=7625.00 physics_tick_ms=26.52 stream_readback_ms=0.05 stream_total_ms=0.30
BENCH chunks=941 pending=871 bodies=70 bodies_raw=294 failures=0 build_ms=0.28 collect_ms=0.55
```

Exact edit evidence:

```text
BENCH p50=22.92 p95=33.33 p99=63.21 max=66.67 min_fps=15.0 over_16.6ms=291 (97.0%)
BENCH max_ms build_ms=0.56 island_ms=38.26 lod_ms=0.06 phys_apply_ms=25.83 phys_body_ms=0.08 phys_collect_ms=1.23 phys_faces_ms=0.35 phys_plan_ms=1.85 phys_setdata_ms=0.34 phys_submit_ms=0.01 phys_tris=8519.00 physics_tick_ms=26.30 stream_readback_ms=0.02 stream_total_ms=0.25
BENCH chunks=664 pending=566 bodies=98 bodies_raw=460 failures=0 build_ms=0.37 collect_ms=0.69
```

Against Task 8's final 300-frame baseline (`reports/m7-task8-final`):

| leg | Task 8 p99 / max / over 16.6 ms | Task 9 p99 / max / over 16.6 ms | Task 8 max `phys_setdata_ms` | Task 9 max `build_ms` / `phys_setdata_ms` |
|---|---:|---:|---:|---:|
| move | 33.33 / 43.10 / 272 (90.7%) | 29.03 / 47.76 / 277 (92.3%) | 0.95 ms | 0.57 / 0.33 ms |
| edit | 56.09 / 77.43 / 298 (99.3%) | 63.21 / 66.67 / 291 (97.0%) | 22.41 ms | 0.56 / 0.34 ms |

The original `docs/todo/opti.md` figures were 18.6 ms moving and 24.1 ms editing.
The corrected instrumentation shows the atomic Jolt `shape_set_data` call at 0.33 ms
moving and 0.34 ms editing; `build_ms` includes the full one-octant build call and is
0.57/0.56 ms. The frame target remains open because other work dominates, including
edit `island_ms=38.26` and `phys_apply_ms=25.83`. Errata 7 in the plan and the opti note
now record this qualified result and the per-call-vs-frame-sum distinction.

## Concerns

- Wayland rejected disabled V-Sync; frame-budget numbers are qualified rather than an
  unqualified 60-fps claim.
- The benchmark retains the existing GTK/theme, FIFO-protocol, Wayland mouse, and two
  ObjectDB-leak warnings at Godot exit; no unrelated environment or renderer fixes were made.
- Benchmark reports under `reports/` are ignored by the repository; the exact benchmark
  evidence is reproduced above and the tracked Errata/report files contain it.

## Round 2 — test-design coverage

Round 2 adds the missing proof around the split's throttled replacement contract without
changing production code:

- `test_octant_split.cpp` now pins exact-centre tie-breaking (`>=` maps to octant 7), clears
  pre-existing output bins, and ignores an incomplete trailing index pair.
- `test_collider_octants.gd` now runs an edit with `shape_builds_per_frame = 1`, observes the
  in-flight staging window, and asserts that no partial octant set becomes visible: the old
  body remains valid, `bodies_raw` is unchanged, and at most one octant build is performed in
  that frame. It then waits for the complete replacement to settle.

Files touched in round 2:

- `extension/tests/test_octant_split.cpp`
- `tests/test_collider_octants.gd`
- `.superpowers/sdd/2026-08-19-m7-budget-demo-capture/task-9-report.md`

## Round 2 verification

Fresh commands:

```text
cd extension && scons test
[doctest] test cases:     320 |     320 passed | 0 failed | 0 skipped
[doctest] assertions: 3962273 | 3962273 passed | 0 failed |
[doctest] Status: SUCCESS!

./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd
Statistics: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

The Godot run retained the existing X11 fallback, GTK theme, FIFO, and ObjectDB-leak
warnings; no test failures or script errors occurred.

Final affected-suite verification after the report update:

```text
./build.sh -j$(nproc)
==> Build OK: 4.7M libvoxel_everything.linux.template_debug.x86_64.so

cd extension && scons test
[doctest] test cases:     320 |     320 passed | 0 failed | 0 skipped
[doctest] assertions: 3962273 | 3962273 passed | 0 failed |

./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd  # 4/4 passed
./gdunit_tests.sh -c -a res://tests/test_collider_stream.gd   # 6/6 passed
./gdunit_tests.sh -c -a res://tests/test_collider_edits.gd    # 3/3 passed
./gdunit_tests.sh -c -a res://tests/test_player_kick.gd       # 1/1 passed
./gdunit_tests.sh -c -a res://tests/test_island_body.gd      # 5/5 passed
git diff --check                                             # clean
```

## Round 3 — deterministic eight-slot/RID and staged-replacement proof

Round 3 closes the review gap with a read-only `VoxelWorld::debug_chunk_collider_octants`
API. For one exact chunk it reports all eight raw body slots, each body RID and RID ID, plus
`staged`, `staged_next_octant`, `staged_built_octants`, and populated staged octants. Committed
chunks now retain all eight stable body RIDs; an empty geometry bin receives a body without a
shape, so it adds no collision geometry while making the slot invariant observable. The
existing staged-shape build and atomic old-body swap are unchanged; all PhysicsServer3D/Jolt
creation remains on the calling thread and the established M3 winding swap remains intact.

The test uses the deterministic cave/terrain chunk selected by `debug_raycast`, runs with
`shape_builds_per_frame = 1`, asserts all eight old slots are valid, asserts every staged
snapshot retains the exact old RID set after `staged_built_octants > 0`, then asserts all eight
replacement RIDs are valid and changed after commit.

### Round 3 RED

After writing the stronger test before the API, the focused command failed at the missing hook:

```text
./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd
Invalid call. Nonexistent function 'debug_chunk_collider_octants' in base 'VoxelWorld'.
Statistics: 4 test cases | 1 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 100
```

### Round 3 GREEN evidence

Fresh focused proof output:

```text
COLLIDER_OCTANT_PROOF chunk=(5, 8, 5) slot=0 old_rids=[21071109554991, 21075404522286, 21079699489581, 21083994456876, 21088289424171, 21092584391466, 21096879358761, 21101174326056] new_rids=[27195732919240, 27200027886537, 27204322853834, 27208617821131, 27212912788428, 27217207755725, 27221502723022, 27225797690319] max_staged_built_octants=6
Statistics: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

Final fresh affected verification:

```text
./build.sh -j$(nproc)
==> Build OK: 4.7M libvoxel_everything.linux.template_debug.x86_64.so

cd extension && scons test
[doctest] test cases:     320 |     320 passed | 0 failed | 0 skipped
[doctest] assertions: 3962273 | 3962273 passed | 0 failed |
[doctest] Status: SUCCESS!

./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd \
  -a res://tests/test_collider_stream.gd \
  -a res://tests/test_collider_edits.gd \
  -a res://tests/test_player_kick.gd \
  -a res://tests/test_island_body.gd
Overall Summary: 19 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Executed test suites: (5/5)
Executed test cases : (19/19)
Exit code: 0
```

The Godot run retained the existing X11-unavailable to Wayland fallback, GTK theme/FIFO
warnings, and two ObjectDB leak warnings at shutdown; these are environment/runtime concerns,
not test failures.

## Round 4 — restore sparse empty-octant slots

Round 3 made `commit_pending()` create a PhysicsServer body even when an octant had no
staged shape. That made the eight-RID assertion deterministic, but it changed production
behavior for a test: `bodies_raw` became exactly `bodies * 8`, and empty bins consumed
shape-less bodies despite Task 9's contract that raw bodies count populated bins.

Round 4 restores shape-conditional body creation. The flat vectors still reserve eight stable
indices per chunk, while an empty octant stores an invalid RID and contributes nothing to
`bodies_in_space()`. The deterministic test now treats the eight entries as a sparse RID mask:
it proves the exact old mask remains unchanged after replacement shapes begin building, then
proves every populated post-commit RID is new. The selected chunk deterministically has six
populated octants and two empty octants before and after the edit.

### Round 4 RED

The regression test was changed before production code. Against the round-3 implementation:

```text
./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd
Expecting to be less than: 968 but was 968 at test_collider_octants.gd:112
Expecting to be less than: 8 but was 8 at test_collider_octants.gd:133
Expecting to be less than: 8 but was 8 at test_collider_octants.gd:190
Statistics: 4 test cases | 0 errors | 3 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 4 test cases | 0 errors | 3 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 100
```

The three failures respectively proved global raw-body inflation, an all-valid old RID mask,
and an all-valid replacement RID mask.

### Round 4 focused GREEN

```text
./build.sh -j$(nproc)
==> Build OK: 4.7M libvoxel_everything.linux.template_debug.x86_64.so

./gdunit_tests.sh -c -a res://tests/test_collider_octants.gd
COLLIDER_OCTANT_PROOF chunk=(5, 8, 5) slot=0 old_rids=[15985868276136, 15990163243431, 15994458210726, 15998753178021, 16003048145316, 16007343112611, 0, 0] new_rids=[19920058319306, 19924353286603, 19928648253900, 19932943221197, 19937238188494, 19941533155791, 0, 0] max_staged_built_octants=6
Statistics: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 4 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

The pre-fix unconditional-body mutation is exactly the implementation used for the RED run,
so the test's mutation check is direct rather than hypothetical.

### Round 4 benchmark

Command:

```text
env WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 tools/run_benchmarks.sh m7-task9-round4-final
```

All five processes exited 0. Wayland again rejected disabled V-Sync, so frame percentiles are
qualified. The move and edit evidence recorded in Errata 7 is:

```text
move: BENCH p50=19.23 p95=23.98 p99=27.29 max=35.67 min_fps=28.0 over_16.6ms=279 (93.0%)
move: BENCH max_ms build_ms=0.49 island_ms=0.01 lod_ms=0.08 phys_apply_ms=24.40 phys_body_ms=0.04 phys_collect_ms=1.12 phys_faces_ms=0.48 phys_plan_ms=3.78 phys_setdata_ms=0.31 phys_submit_ms=0.01 phys_tris=7625.00 physics_tick_ms=26.28 stream_readback_ms=0.02 stream_total_ms=0.29
move: BENCH chunks=929 pending=860 bodies=69 bodies_raw=282 failures=0 build_ms=0.18 collect_ms=0.38
edit: BENCH p50=22.86 p95=33.33 p99=50.39 max=77.28 min_fps=12.9 over_16.6ms=297 (99.0%)
edit: BENCH max_ms build_ms=1.03 island_ms=38.86 lod_ms=0.06 phys_apply_ms=28.72 phys_body_ms=0.07 phys_collect_ms=4.73 phys_faces_ms=0.67 phys_plan_ms=2.09 phys_setdata_ms=0.40 phys_submit_ms=0.01 phys_tris=8519.00 physics_tick_ms=29.54 stream_readback_ms=0.01 stream_total_ms=0.29
edit: BENCH chunks=664 pending=566 bodies=98 bodies_raw=460 failures=0 build_ms=0.48 collect_ms=2.83
```

Every leg reported `raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS
frame=WARN` and `display_driver=Wayland vsync_requested=disabled vsync_actual=disabled
verdict_qualified=false`. The raw counts are sparse again, and the maximum one-call
`shape_set_data` cost remains below the pre-split Task 8 values.

### Round 4 affected-suite verification

```text
./build.sh -j$(nproc)
==> Build OK: 4.7M libvoxel_everything.linux.template_debug.x86_64.so

cd extension && scons target=template_release -j$(nproc)
scons: done building targets.

cd extension && scons test
[doctest] test cases:     320 |     320 passed | 0 failed | 0 skipped
[doctest] assertions: 3962273 | 3962273 passed | 0 failed |
[doctest] Status: SUCCESS!

./gdunit_tests.sh -c \
  -a res://tests/test_collider_octants.gd \
  -a res://tests/test_collider_stream.gd \
  -a res://tests/test_collider_edits.gd \
  -a res://tests/test_player_kick.gd \
  -a res://tests/test_island_body.gd
Overall Summary: 19 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Executed test suites: (5/5)
Executed test cases : (19/19)
Exit code: 0

git diff --check
# no output
```

The Godot run retained the known X11-to-Wayland fallback, GTK theme, missing protocol, and
two ObjectDB leak warnings. No test failure or script error accompanied those warnings.

### Repository-wide completion gate

A post-change repository-wide run kept the native suite green but exposed failures outside
Task 9's affected suite:

```text
cd extension && scons test
[doctest] test cases: 320 | 320 passed | 0 failed | 0 skipped

./gdunit_tests.sh -c
Overall Summary: 281 test cases | 1 errors | 13 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 100
```

The same failing suites reproduce when launched in fresh, isolated Godot processes:

```text
test_connectivity.gd: 33 test cases | 0 errors | 10 failures
test_edit_pipeline.gd: 8 test cases | 1 errors | 2 failures
test_raymarch_gbuffer.gd: 9 test cases | 0 errors | 1 failure
```

None of those three test files or their render/connectivity implementations changed in round 4.
The 19-case collider/island affected set remains green as recorded above; this task does not
expand into those separate failures, and the branch is not presented as repository-wide green.
