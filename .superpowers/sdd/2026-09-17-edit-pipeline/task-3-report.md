# Task 3 Report: Pin staleness and prove the connectivity workaround

## Status

PASS. Characterization-only changes are limited to the two requested GDScript suites. No production source changes remain.

## Files changed

- `tests/test_connectivity.gd`
  - Added `make_world(override_bricks := 1)` and assigned `w.max_override_bricks = override_bricks`.
  - Added `test_a_consolidation_during_an_extraction_is_pinned` exactly as specified.
- `tests/test_consolidation.gd`
  - Added `test_a_full_op_list_does_not_stay_full_while_consolidation_runs` exactly as specified.
- `.superpowers/sdd/2026-09-17-edit-pipeline/task-3-report.md`
  - This report; intentionally not included in the task commit because the brief's commit command stages only the two suites.

## Test output

### Baseline

Command:

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_consolidation.gd
```

Result: PASS, exit 0.

```text
51 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
```

### After Task 3 changes

Command:

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_consolidation.gd
```

Result: PASS, exit 0.

```text
53 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
```

The two new cases passed, and the baseline 51 cases remained passing.

### Final verification after restoring all production sources

Production restoration:

```text
git checkout -- extension/src
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Build result: PASS, exit 0.

Fresh test command:

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd,res://tests/test_consolidation.gd
```

Fresh result: PASS, exit 0.

```text
53 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
```

## Bite-proof results

### Staleness predicate bite

Temporary change in `extension/src/physics/island_manager.cpp`:

```cpp
const bool stale = false;
```

- Rebuild: PASS, exit 0.
- Exact two-suite command: failed, exit 100; the existing stale-extraction characterization failed.
- Continuation run (`-c`) reached the new named case and failed as required:

```text
test_a_consolidation_during_an_extraction_is_pinned FAILED
"a consolidation during an extraction no longer reads as stale"
"the stale extraction still spawned a body"
```

The temporary file was restored and rebuilt before the second bite.

### Automatic consolidation queue bite

Temporary change in `extension/src/core/world_store.cpp`: the `queue_consolidation(region)` loop was commented out.

- Rebuild: PASS, exit 0.
- Exact two-suite command: failed, exit 100 before the new proof, at `test_teardown_releases_staged_override_slots_before_reinit`.
- Continuation run (`-c`) reached the new named case and failed as required:

```text
test_a_full_op_list_does_not_stay_full_while_consolidation_runs FAILED
"a full op list survived consolidation: the max_override_bricks = 1 workaround may be unnecessary. Stop and report before Task 4."
```

All temporary production edits were removed with `git checkout -- extension/src`, followed by a successful rebuild and final passing test run.

## Evidence: affected suites against the Task 1 baseline

Sources used:

- Task 1 baseline markdown: `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md`.
- Baseline XML: `reports/report_2/results.xml`.
- Focused pre-change XML: `reports/report_14/results.xml`.
- Focused post-change XML: `reports/report_15/results.xml`.
- Focused final XML after restoring production sources: `reports/report_20/results.xml`.

The baseline markdown and `reports/report_2/results.xml` agree on the affected suites: `test_connectivity` = 33 tests / 0 failures / 0 errors, and `test_consolidation` = 18 / 0 / 0. The focused pre-change run (`report_14`) has the same counts and the same case inventories. The focused post-change run (`report_15`) adds exactly these cases: `test_connectivity::test_a_consolidation_during_an_extraction_is_pinned` and `test_consolidation::test_a_full_op_list_does_not_stay_full_while_consolidation_runs`; both suites report zero failures and zero errors. The final focused run (`report_20`) has 34 / 0 / 0 and 19 / 0 / 0 respectively, with no affected-suite failure/error case messages.

Known flaky-by-case handling follows the plan: `test_connectivity` and `test_island_body` are compared by suite failure count, not case identity. The Task 1 baseline records `test_connectivity` as 33 / 0 / 0 and `test_island_body` as 5 / 0 / 0; `test_island_body` was not included in the focused two-suite command, so no focused result is claimed for it. The baseline's listed failure/error cases are unrelated (`test_voxel_settings::test_an_ambient_change_reaches_the_object_global` — `ERROR: res://tests/test_voxel_settings.gd:213`; `test_sun_cascades_gpu::test_the_min_level_clamp_does_not_peter_pan` — `FAILED: res://tests/test_sun_cascades_gpu.gd:101`), and neither is in an affected suite.

Validation command and output:

```text
python3 - <<'EOF'
import re
import xml.etree.ElementTree as ET
from pathlib import Path

baseline = Path('docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md').read_text()
expected = {}
for name, tests, failures, errors in re.findall(r'^# (test_(?:connectivity|consolidation|island_body)): tests=(\d+) failures=(\d+) errors=(\d+)$', baseline, re.M):
    expected[name] = (int(tests), int(failures), int(errors))
reports = {}
for number in (2, 14, 15, 20):
    root = ET.parse(f'reports/report_{number}/results.xml').getroot()
    reports[number] = {s.get('name'): s for s in root.iter('testsuite')}
def stats(report, name):
    s = reports[report][name]
    return tuple(int(s.get(k)) for k in ('tests', 'failures', 'errors'))
def cases(report, name):
    return [tc.get('name') for tc in reports[report][name].findall('testcase')]
def bad(report, name):
    out = []
    for tc in reports[report][name].findall('testcase'):
        for node in list(tc):
            if node.tag in ('failure', 'error'):
                text = (node.text or '').strip()
                out.append((tc.get('name'), text.splitlines()[0] if text else node.get('message', '')))
    return out
for name in ('test_connectivity', 'test_consolidation'):
    assert stats(2, name) == expected[name]
print('baseline markdown vs reports/report_2: affected suite counts match')
print('baseline markdown known flaky suites: test_connectivity=' + str(expected['test_connectivity']) + ', test_island_body=' + str(expected['test_island_body']))
for report in (14, 15, 20):
    print(f'report_{report}: ' + '; '.join(f'{name}={stats(report, name)}' for name in ('test_connectivity', 'test_consolidation')))
assert cases(14, 'test_connectivity') == cases(2, 'test_connectivity')
assert cases(14, 'test_consolidation') == cases(2, 'test_consolidation')
print('report_14 case inventory matches baseline report_2 for both affected suites')
print('report_15 added cases: connectivity=' + repr(sorted(set(cases(15, 'test_connectivity')) - set(cases(2, 'test_connectivity')))) + '; consolidation=' + repr(sorted(set(cases(15, 'test_consolidation')) - set(cases(2, 'test_consolidation')))))
assert sorted(set(cases(15, 'test_connectivity')) - set(cases(2, 'test_connectivity'))) == ['test_a_consolidation_during_an_extraction_is_pinned']
assert sorted(set(cases(15, 'test_consolidation')) - set(cases(2, 'test_consolidation'))) == ['test_a_full_op_list_does_not_stay_full_while_consolidation_runs']
assert not bad(2, 'test_connectivity') and not bad(2, 'test_consolidation')
assert not bad(15, 'test_connectivity') and not bad(15, 'test_consolidation')
assert not bad(20, 'test_connectivity') and not bad(20, 'test_consolidation')
print('affected-suite failure/error case messages: baseline report_2=[]; report_15=[]; report_20=[]')
print('known flaky-by-case handling: test_connectivity compared by suite failure count; test_island_body was not in the focused run')
EOF
```

Output:

```text
baseline markdown vs reports/report_2: affected suite counts match
baseline markdown known flaky suites: test_connectivity=(33, 0, 0), test_island_body=(5, 0, 0)
report_14: test_connectivity=(33, 0, 0); test_consolidation=(18, 0, 0)
report_15: test_connectivity=(34, 0, 0); test_consolidation=(19, 0, 0)
report_20: test_connectivity=(34, 0, 0); test_consolidation=(19, 0, 0)
report_14 case inventory matches baseline report_2 for both affected suites
report_15 added cases: connectivity=['test_a_consolidation_during_an_extraction_is_pinned']; consolidation=['test_a_full_op_list_does_not_stay_full_while_consolidation_runs']
affected-suite failure/error case messages: baseline report_2=[]; report_15=[]; report_20=[]
known flaky-by-case handling: test_connectivity compared by suite failure count; test_island_body was not in the focused run
```

## Self-review

- `git diff --check`: PASS.
- Production diff after restoration: clean (`git diff --quiet -- extension/src`).
- Only the two requested test suites are modified in the worktree.
- No production behavior was changed in the final state.
- Required hook interfaces and exact test names are used.
- The workaround premise is proven both positively (normal pool clears the full list) and negatively (without automatic queueing it stays full).

## Concerns

The GPU-backed runs emit pre-existing Metal fence timeout, upload-failure, and fail-soft rejection diagnostics. They also appeared in the baseline run; despite the diagnostics, gdUnit4 reported zero errors and zero failures in the final run.
