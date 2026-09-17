# Task 1 — baseline failure set

## Status
DONE_WITH_CONCERNS

## Commands and output

Working tree: `/Users/jeremyzhao/Development/godot/voxel-everything/.worktrees/edit-pipeline`

### Step 1: native build and suite

Command:

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
```

Result: build succeeded (`==> Build OK: 3.2M libvoxel_everything.macos.template_debug.universal.dylib`). The native doctest summary was:

```text
[doctest] test cases:     625 |     625 passed | 0 failed | 0 skipped
[doctest] assertions: 9120054 | 9120054 passed | 0 failed |
[doctest] Status: SUCCESS!
```

The build emitted existing compiler warnings, but exited successfully.

### Step 2: full gdUnit suite

Command:

```bash
./gdunit_tests.sh
```

Output:

```text
==> Running gdUnit4 against a real display (GPU rendering enabled, vsync on)
    godot_binary: /opt/homebrew/bin/godot
    tests:        res://tests
Godot Engine v4.7.2.stable.official.ed1daf0bf - https://godotengine.org
Metal 4.0 - Forward+ - Using Device #0: Apple - Apple M1 (Apple7)

SCRIPT ERROR: Parse Error: Could not find type "GdUnitTestCIRunner" in the current scope.
          at: GDScript::reload (res://addons/gdUnit4/bin/GdUnitCmdTool.gd:5)
SCRIPT ERROR: Parse Error: Identifier "GdUnitTestCIRunner" not declared in the current scope.
          at: GDScript::reload (res://addons/gdUnit4/bin/GdUnitCmdTool.gd:10)
ERROR: Failed to load script "res://addons/gdUnit4/bin/GdUnitCmdTool.gd" with error "Parse error".
   at: load (modules/gdscript/gdscript_resource_format.cpp:46)

Command exited with code 1
```

### Step 3: report extraction

Command:

```bash
latest=$(ls -d reports/report_* | sort -V | tail -1)
echo "$latest"
python3 - "$latest/results.xml" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for suite in root.iter('testsuite'):
    print(f"# {suite.get('name')}: tests={suite.get('tests')} failures={suite.get('failures')} errors={suite.get('errors')}")
for tc in root.iter('testcase'):
    bad = tc.find('failure') if tc.find('failure') is not None else tc.find('error')
    if bad is not None:
        msg = (bad.get('message') or bad.text or '').strip().splitlines()
        print(f"{tc.get('classname')}::{tc.get('name')} — {msg[0] if msg else ''}")
EOF
```

Output:

```text
ls: reports/report_*: No such file or directory

Traceback (most recent call last):
  File "<stdin>", line 2, in <module>
  File "/opt/homebrew/Cellar/python@3.14/3.14.7/Frameworks/Python.framework/Versions/3.14/lib/python3.14/xml/etree/ElementTree.py", line 1227, in parse
    tree.parse(source, parser)
    ~~~~~~~~~~^^^^^^^^^^^^^^^^
  File "/opt/homebrew/Cellar/python@3.14/3.14.7/Frameworks/Python.framework/Versions/3.14/lib/python3.14/xml/etree/ElementTree.py", line 570, in parse
    source = open(source, "rb")
FileNotFoundError: [Errno 2] No such file or directory: '/results.xml'
```

No `reports/report_*` directory or `results.xml` was produced, so no gdUnit per-suite counts or testcase failures can be truthfully recorded. The baseline records the runner failure instead of inventing counts.

## Files changed

- Created `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md`.
- Created `.superpowers/sdd/2026-09-17-edit-pipeline/task-1-report.md`.

## Commit

`8fb015a docs: edit pipeline baseline failure set`

No production files changed.

## TDD evidence

Not applicable. This task records an existing baseline and makes no production change.

## Self-review

- Native build and native suite commands match the brief.
- Full gdUnit command matches the brief.
- Baseline records the real native doctest summary.
- Baseline explicitly records that gdUnit failed before discovery and that no XML report exists.
- Known flaky suites and the documented Metal environment error are included verbatim.
- Only the requested baseline and task report files were created.

## Concerns

The gdUnit suite cannot currently reach test discovery because `GdUnitCmdTool.gd` cannot resolve `GdUnitTestCIRunner`. Consequently, the required report counts/failing testcase list are unavailable until the runner parse error is resolved.

## Round 1 fix

Status: DONE_WITH_CONCERNS

The runner parse error was caused by stale Godot generated class-cache state. No production code or gdUnit runner files were changed. Godot regenerated the class cache and three missing test `.uid` files.

### Generated-state refresh

Command:

```bash
/opt/homebrew/bin/godot --path . --editor --quit
```

Output:

```text
Godot Engine v4.7.2.stable.official.ed1daf0bf - https://godotengine.org
Metal 4.0 - Forward+ - Using Device #0: Apple - Apple M1 (Apple7)
GdUnit4 TCP Server: Successfully started checked port: 31002
Run dispose test resources
Unload GdUnit4 Plugin success
```

### Focused gdUnit check

Command:

```bash
./gdunit_tests.sh -a res://tests/test_edit_pipeline.gd
```

Output:

```text
Statistics: 8 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 8 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Executed test suites: (1/1)
Executed test cases : (8/8)
Exit code: 0
Run tests ends with 0
```

### Required full gdUnit run

Command:

```bash
./gdunit_tests.sh
```

Output:

```text
Overall Summary: 504 test cases | 1 errors | 2 failures | 0 flaky | 0 skipped | 0 orphans |
Executed test suites: (95/95)
Executed test cases : (504/504)
Open XML Report at: file:///Users/jeremyzhao/Development/godot/voxel-everything/.worktrees/edit-pipeline/reports/report_2/results.xml
Exit code: 100
Run dispose test resources
```

The non-zero exit is from the two observed failures, not runner discovery: `test_voxel_settings::test_an_ambient_change_reaches_the_object_global` errors at `res://tests/test_voxel_settings.gd:213` because Godot/Metal assigns `Nil` to a `Vector3`; `test_sun_cascades_gpu::test_the_min_level_clamp_does_not_peter_pan` has failures at lines 101 and 108 (`Expecting: 'true' but is 'false'`).

### XML extraction and amended baseline

Command:

```bash
latest=$(ls -d reports/report_* | sort -V | tail -1)
echo "$latest"
python3 - "$latest/results.xml" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for suite in root.iter('testsuite'):
    print(f"# {suite.get('name')}: tests={suite.get('tests')} failures={suite.get('failures')} errors={suite.get('errors')}")
for tc in root.iter('testcase'):
    bad = tc.find('failure') if tc.find('failure') is not None else tc.find('error')
    if bad is not None:
        msg = (bad.get('message') or bad.text or '').strip().splitlines()
        print(f"{tc.get('classname')}::{tc.get('name')} — {msg[0] if msg else ''}")
EOF
```

Output begins with `reports/report_2`, followed by all 95 per-suite count lines. The failing-case lines were:

```text
test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213
test_sun_cascades_gpu::test_the_min_level_clamp_does_not_peter_pan — FAILED: res://tests/test_sun_cascades_gpu.gd:101
```

The complete extracted count block and both real failing cases replaced the incomplete baseline in `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md`, which now points to `reports/report_2`.

### Tests covering the amended baseline

Command:

```bash
python3 - <<'EOF'
import re
import xml.etree.ElementTree as ET
from pathlib import Path
baseline = Path('docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md').read_text()
root = ET.parse('reports/report_2/results.xml').getroot()
expected = [f"# {s.get('name')}: tests={s.get('tests')} failures={s.get('failures')} errors={s.get('errors')}" for s in root.iter('testsuite')]
actual = re.findall(r'^# .*: tests=\d+ failures=\d+ errors=\d+$', baseline, re.M)
assert actual == expected
assert len(expected) == 95
assert len(list(root.iter('testcase'))) == 504
assert sum(int(s.get('failures')) for s in root.iter('testsuite')) == 2
assert sum(int(s.get('errors')) for s in root.iter('testsuite')) == 1
assert 'No per-suite counts available' not in baseline
assert 'No test cases extracted' not in baseline
print('baseline validation: 95 suites, 504 testcases, 2 failures, 1 error; PASS')
EOF
git diff --check
```

Output:

```text
baseline validation: 95 suites, 504 testcases, 2 failures, 1 error; PASS
```

## Fix files

- Modified `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md` with the real XML-derived counts and failures.
- Modified `.superpowers/sdd/2026-09-17-edit-pipeline/task-1-report.md` with this fix report.
- Generated `tests/test_override_region_border.gd.uid`, `tests/test_voxel_world_raycast.gd.uid`, and `tests/test_world_field_consumers.gd.uid`.
- No production code or runner files changed.
