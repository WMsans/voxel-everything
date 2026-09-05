# Unbounded-world after evidence

Task 20 evidence for commit `f4cc937` (`unbounded-world`). No production code or tests
were modified for this report.

## Suite comparison

Commands run:

```text
mkdir -p reports/unbounded-after
cd extension && scons test -j8 2>&1 | tee ../reports/unbounded-after/native.txt; cd ..
./gdunit_tests.sh 2>&1 | tee reports/unbounded-after/gdunit.txt || true
diff <(grep -i "fail" reports/unbounded-baseline/gdunit.txt) \
     <(grep -i "fail" reports/unbounded-after/gdunit.txt) || true
```

Native: `459 test cases | 459 passed | 0 failed`, `9816469 assertions | 9816469
passed | 0 failed`, status success, exit 0. The baseline had 426 cases; the additional
33 cases are present on this branch and passed.

gdUnit: `378 test cases | 0 errors | 5 failures | 0 flaky | 0 skipped | 0 orphans`,
74/74 suites and 378/378 cases executed, exit 100 as documented by the baseline. The
four failing test cases and five assertions are exactly the baseline set; there are no
new failure cases:

| Test case | Baseline result | After result | Explanation |
|---|---|---|---|
| `test_collider_octants.gd > test_split_colliders_still_match_the_field` | false at line 41 | false at line 39 | Same pre-existing assertion; no new failure. |
| `test_contact_shadow.gd > test_a_crater_darkens_its_own_floor` | no pixel occluded at line 41 | no pixel occluded at line 39 | Same pre-existing assertion; no new failure. |
| `test_island_body.gd > test_a_body_lands_on_the_streamed_collider_and_sleeps` | body did not rest; `-118.597923 > 47.552086` failed at lines 69/73 | same body/result and value at lines 67/71 | Same two pre-existing assertions; no new failure. |
| `test_collider_stream.gd > test_colliders_appear_around_the_player` | expected 0, got 63 at line 89 | expected 0, got 63 at line 87 | Same pre-existing nondeterministic collider-stream assertion; no new failure. |

The raw `diff` also reports timing changes, changed source line numbers, and nearby
passed-statistic durations because the required grep matches `fail` inside `failures`;
these are not new test failures. No unexplained new failure was observed.

## Capture availability and visual verdict

`demo/capture.gd` documents the actual invocation as:

```text
godot --path . --resolution 2560x1440 demo/main.tscn -- --capture
```

It writes `user://capture/frame_%05d.png` (900 frames); `tools/encode_capture.sh` is the
checked-in encoder. No pre-refactor capture directory or frame sequence is available in
`reports/` or the local Godot user-data locations, so no honest before/after frame
comparison can be made. The capture was not run: without the required prior capture,
there is no valid comparison target, and this report does not fabricate one. Therefore the
no-visual-change claim is **not proven by this task**. The expected far-field horizon
qualification and the near-field window-indexing check remain unmeasured here.

## Benchmark availability and comparison

The checked-in `tools/run_benchmarks.sh` was run exactly as:

```text
tools/run_benchmarks.sh unbounded-after
```

All six legs (`steady`, `move`, `ridge`, `edit`, `edit-bounded`, `island`) exited 0 and
are captured in this directory. Environment was Godot 4.7.2, Apple M1, Metal, macOS;
actual viewport was 2675x1440 and requested 2560x1440. Every leg reported
`gpu_* samples=0` and `budget_verdict` GPU entries as `UNMEASURED`: no valid Metal GPU
timestamp samples were available. Thus after raymarch p99 is unavailable, not zero.

The closest prior raw `reports/m7-final` directory is absent. The repository's documented
M7 closing report (`docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md`, Errata
10 / `docs/PORTFOLIO.md`) provides the comparison values below. It was recorded on an
RTX 4070 Laptop/Vulkan/Wayland at actual 2560x2778, so it is not an apples-to-apples
performance comparison with this M1/Metal run.

| Leg | Closest prior frame p99 / raymarch p99 (ms) | After frame p99 (ms) | After raymarch p99 |
|---|---:|---:|---:|
| steady | 24.03 / 9.610 | 23.83 | unavailable (0 samples) |
| move | 32.33 / 8.810 | 38.89 | unavailable (0 samples) |
| ridge | 31.49 / 9.112 | 36.24 | unavailable (0 samples) |
| edit | 81.77 / 29.145 | 145.36 | unavailable (0 samples) |
| edit-bounded | 55.65 / 12.574 | 27.31 | unavailable (0 samples) |
| island | 33.33 / 9.899 | 25.93 | unavailable (0 samples) |

After wall-frame verdicts were WARN on all six legs; the frame values are environment
measurements only and do not establish neutrality. The documented prior M7 run had GPU
raymarch WARN on all six legs and frame WARN on all six legs.

Relevant observed benchmark details: steady settled at the configured cap (`settle
frames_to_quiet=1500 capped=true`); move/ridge remained actively streaming; edit reported
`overflow=3`; edit-bounded reported `overflow=0`, and island reported `islands=3` and
`spawned=3`. The `collider_stream` count remains the nondeterministic baseline assertion
noted above. The carried seam bar `1/40` and streaming slack `4096` are unchanged
acceptance tolerances; sun texel/cascade sizing (about 2.3 m per texel in the carried note)
is not measured by this Task 20 run.

## Knob check and revert

The scene was temporarily changed at the `VoxelWorld` node to:

```text
stream_radius_m = 4000.0
```

Then this exact command was run:

```text
./gdunit_tests.sh -a res://tests/test_self_check.gd
```

Result: 1/1 test case passed, 0 errors, 0 failures, exit 0. The scene line was removed
before committing. `demo/main.tscn` had SHA-256
`e0566ab5ac5d7eefc982ad076f373168aa007fd07e6e544fa4354b19e7ceb8d3` before and after;
`git diff --exit-code -- demo/main.tscn` returned 0.

## Definition checks

Both forbidden-symbol grep checks from the brief returned no matches:

```text
grep -rn "WorldBounds\|contains_region\|contains_brick" extension/src
grep -rn "world_size_regions\|world_origin_bricks" extension/src demo tests
```

The native suite included the unbounded characterization and 50000/-50000 edit coverage;
it passed. `ve::RegionArchive` exists, with the Sub-project C synchronous-IO constraint
documented in `extension/src/world/region_archive.h`. No A-path eviction was observed.
