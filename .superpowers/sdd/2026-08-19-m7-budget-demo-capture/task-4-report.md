# M7 Task 4 — region-level DDA

## Status

Round 1 review fixes are implemented for commit `2bc8baf` (`perf: region-level DDA,
the traversal level spec 3 always described`). The residency-leaving test now asserts a
concrete marcher result in both analytic-oracle branches. The benchmark Errata wording now
states that screenshot artifacts are ignored local files, not committed files, and includes
reproducible capture commands and observed file-size output.

Round 1 fix commit: see the conventional commit recorded in the repository history for this report.

## Scope and reviewed material

- Brief: `.superpowers/sdd/2026-08-19-m7-budget-demo-capture/task-4-brief.md`
- Implementation commit: `2bc8baf`
- Reviewer findings: `.superpowers/sdd/2026-08-19-m7-budget-demo-capture/review-3b8eee5..2bc8baf.diff`
- Implementation files reviewed: `shaders/raymarch.comp.glsl`,
  `tests/test_region_dda.gd`, and Errata entry 4 in
  `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md`
- Round 1 fix files: `tests/test_region_dda.gd`,
  `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md`, and this report.

The Task 4 implementation remains unchanged. The required binding 22 and cost-probe
unpacking are present in the current tree from the preceding implementation work; this fix
does not alter shader, C++, or edit-pipeline behavior.

## TDD evidence

### RED — pre-Task-4 traversal

The focused suite was run with the pre-Task-4 shader from `HEAD^` temporarily installed.
The shader was restored immediately after the command, and `git diff -- shaders/raymarch.comp.glsl`
was empty. Exact command:

```text
set +e
backup=$(mktemp)
cp shaders/raymarch.comp.glsl "$backup"
git show HEAD^:shaders/raymarch.comp.glsl > shaders/raymarch.comp.glsl
./gdunit_tests.sh -c -a res://tests/test_region_dda.gd > /tmp/task4-red-region-dda.txt 2>&1
status=$?
cp "$backup" shaders/raymarch.comp.glsl
rm -f "$backup"
printf 'RED_EXIT_STATUS=%s\\n' "$status"
grep -E 'test_(sky|ground|ray_leaving)|Statistics:|Overall Summary:|Exit code:|ERROR:|FAIL' /tmp/task4-red-region-dda.txt | tail -40
exit 0
```

Observed output:

```text
RED_EXIT_STATUS=100
... test_sky_ray_walks_regions_not_bricks ... FAILED
32 but was 251
Expecting to be greater than:
0 but was 0
... test_ground_hits_still_match_the_analytic_raycast ... PASSED
... test_ray_leaving_residency_finds_nothing_rather_than_something ... PASSED
Statistics: 3 test cases | 0 errors | 2 failures | 0 flaky | 0 skipped | 0 orphans
Overall Summary: 3 test cases | 0 errors | 2 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 100
```

This demonstrates the intended regression: the old traversal visited 251 brick cells and
reported zero region cells; it was not a test that passed accidentally.

### GREEN — Task 4 traversal and review fix

After the implementation shader was restored and the residency test was changed from a
one-sided conditional to explicit true/false assertions, the focused command was:

```text
./gdunit_tests.sh -c -a res://tests/test_region_dda.gd
```

Observed result:

```text
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Overall Summary: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
Run tests ends with 0
```

The changed test now executes one of these concrete assertions:

```gdscript
if truth["hit"]:
    assert_bool(d["hit"]).is_true()
else:
    assert_bool(d["hit"]).is_false()
```

There is no empty conditional branch.

## Verification commands and outputs

### Build

```text
./build.sh -j$(nproc)
```

```text
==> Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so
Registered native classes: VoxelWorld, RaymarchCompositor
==> Done.
```

### Native full test suite

```text
cd extension && scons test
```

```text
[doctest] test cases:     299 |     299 passed | 0 failed | 0 skipped
[doctest] assertions: 3961657 | 3961657 passed | 0 failed |
[doctest] Status: SUCCESS!
scons: done building targets.
```

### Required Godot suites

These are the exact six commands required by the Task 4 brief. They were run sequentially
fresh after the build.

```text
./gdunit_tests.sh -c -a res://tests/test_region_dda.gd
```

```text
3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
COMMAND_EXIT_STATUS=0
```

```text
./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd
```

```text
5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
COMMAND_EXIT_STATUS=0
```

```text
./gdunit_tests.sh -c -a res://tests/test_raymarch_mips.gd
```

```text
2 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
COMMAND_EXIT_STATUS=0
```

```text
./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd
```

```text
Invalid access to property or key 'pos' on a base object of type 'Dictionary'.
... test_sphere_add_places_material_4_in_open_sky ... FAILED
Statistics: 8 test cases | 1 errors | 2 failures | 0 flaky | 0 skipped | 0 orphans
Overall Summary: 8 test cases | 1 errors | 2 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 100
COMMAND_EXIT_STATUS=100
```

```text
./gdunit_tests.sh -c -a res://tests/test_island_render.gd
```

```text
14 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
COMMAND_EXIT_STATUS=0
```

```text
./gdunit_tests.sh -c -a res://tests/test_lod_seam.gd
```

```text
3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
COMMAND_EXIT_STATUS=0
```

The aggregate required-suite command returned status 1 only because the known edit-pipeline
suite returned 100; the other five suites returned 0.

### Diff hygiene

```text
git diff --check
```

Output:

```text
(no output; exit status 0)
```

## Benchmark before / after / delta

The before column is Task 3's recorded pre-revert marcher run from
`.superpowers/sdd/2026-08-19-m7-budget-demo-capture/task-3-report.md`. The after column is
a fresh post-fix Task 4 run. Exact benchmark command:

```text
WAYLAND_DISPLAY=wayland-1 tools/run_benchmarks.sh m7-task4-round1
```

The five leg logs are in the ignored local directory `reports/m7-task4-round1/`. Each
benchmark process exited 0 and emitted:

```text
BENCH gpu_raymarch samples=287 p50_ms=6.838 p99_ms=8.927   # steady
BENCH gpu_raymarch samples=287 p50_ms=6.836 p99_ms=8.362   # move
BENCH gpu_raymarch samples=287 p50_ms=5.541 p99_ms=8.333   # ridge
BENCH gpu_raymarch samples=287 p50_ms=11.296 p99_ms=16.888 # edit
BENCH gpu_raymarch samples=807 p50_ms=7.182 p99_ms=9.253   # island
BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false
```

| leg | Task 3 before p50/p99 ms | Task 4 after p50/p99 ms | p50 delta | p99 delta |
|---|---:|---:|---:|---:|
| steady | 6.307 / 7.935 | 6.838 / 8.927 | +8.42% | +12.50% |
| move | 6.379 / 7.865 | 6.836 / 8.362 | +7.16% | +6.32% |
| ridge | 5.947 / 8.296 | 5.541 / 8.333 | -6.83% | +0.45% |
| edit | 10.815 / 14.365 | 11.296 / 16.888 | +4.45% | +17.56% |
| island | 6.746 / 8.516 | 7.182 / 9.253 | +6.46% | +8.65% |

All five legs retained `raymarch=WARN`; the other pass verdicts were
`lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS`, with `frame=WARN`. These
percentiles are qualified by the Wayland environment: the run reported
`vsync_requested=disabled` and `vsync_actual=disabled`, but `verdict_qualified=false` as
defined by the benchmark's display-driver qualification logic. The implementation is
retained because the Task 4 null-result condition was not met on every leg.

The direct cost probe mechanism remains covered by the focused suite: the sky case reports
nonzero `regions` and fewer than 32 `bricks`, while the analytic hit-oracle cases agree.

## Files

### Implementation commit under review (`2bc8baf`)

- `shaders/raymarch.comp.glsl` — region DDA, bounded brick DDA, and empty-region shadow skip.
- `tests/test_region_dda.gd` — Task 4 regression suite.
- `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md` — original Errata entry 4.

The binding 22 and probe unpacking used by this implementation are present in the current
Task 3 wiring and were not changed by this round-1 fix.

### Round 1 fix

- `tests/test_region_dda.gd` — assert `true` and `false` outcomes explicitly.
- `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md` — make screenshot artifact
  status truthful and record exact capture/stat/check-ignore output.
- `.superpowers/sdd/2026-08-19-m7-budget-demo-capture/task-4-report.md` — this report.

## Self-review

- The residency-leaving test has an assertion on both branches; neither branch is empty.
- No shader, C++, edit-pipeline, or unrelated production file was changed for Round 1.
- The pre-Task-4 RED run was performed against a temporary shader copy and the source was
  restored before the test edit.
- The required focused, native, build, and six Godot commands are recorded with their actual
  statuses.
- The benchmark table uses the exact fresh `gpu_raymarch` lines and computes deltas from the
  recorded Task 3 values.
- The screenshot wording explicitly says the PNGs are ignored local artifacts, not committed
  files; the reproducible capture command and observed byte counts are included.

## Concerns

1. `test_edit_pipeline.gd` remains pre-existing red in
   `test_sphere_add_places_material_4_in_open_sky`: 1 error and 2 failures, including the
   missing `pos` dictionary key. No edit-pipeline code was changed. The same failure was
   already documented in Task 3's report and was reproduced in this required run.
2. Benchmark timing is environment-qualified on Wayland; V-Sync/display qualification means
   the GPU percentile deltas are noisy and all frame/raymarch verdicts remain WARN.
3. Cost-view PNGs are deliberately not committed because `reports/` is ignored. The plan now
   reports their exact local paths, capture command, sizes, and ignore-check output instead of
   implying repository artifacts.
4. Godot runs emit existing X11 fallback, GTK theme, Wayland protocol, ObjectDB leak, and
   benchmark warning diagnostics; these did not change the stated test exit statuses.

## Round 1 fix section — exact commands and outputs

### Test assertion fix

Changed `tests/test_region_dda.gd` from:

```gdscript
if not truth["hit"]:
    assert_bool(d["hit"]).is_false()
```

to:

```gdscript
if truth["hit"]:
    assert_bool(d["hit"]).is_true()
else:
    assert_bool(d["hit"]).is_false()
```

Focused post-fix command/output:

```text
./gdunit_tests.sh -c -a res://tests/test_region_dda.gd
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Overall Summary: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
```

### Errata correction

Exact local-artifact verification command/output:

```text
stat -c '%n %s bytes' /tmp/m7-task4-cost-view-before3.png /tmp/m7-task4-cost-view-after.png reports/m7-task4/cost-view-before.png reports/m7-task4/cost-view-after.png
/tmp/m7-task4-cost-view-before3.png 1975168 bytes
/tmp/m7-task4-cost-view-after.png 2053828 bytes
reports/m7-task4/cost-view-before.png 1975168 bytes
reports/m7-task4/cost-view-after.png 2053828 bytes

git check-ignore -v reports/m7-task4/cost-view-before.png reports/m7-task4/cost-view-after.png
.gitignore:20:reports/ reports/m7-task4/cost-view-before.png
.gitignore:20:reports/ reports/m7-task4/cost-view-after.png
```

Reproduction capture commands, with the appropriate baseline or Task 4 demo running at
2560×1440 and cost view enabled, are:

```text
WAYLAND_DISPLAY=wayland-1 grim /tmp/m7-task4-cost-view-before3.png
WAYLAND_DISPLAY=wayland-1 grim /tmp/m7-task4-cost-view-after.png
```

`grim` emits no success text; the `stat` and `git check-ignore` output above is the recorded
verification. No claim is made that these PNG files are committed.
