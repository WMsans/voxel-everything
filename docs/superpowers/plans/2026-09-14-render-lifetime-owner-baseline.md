# Render lifetime owner — baseline

Commit: `ca9b87b`. Recorded 2026-09-14 on macOS 26.4.1 / Apple M1.
Report: unavailable — `./gdunit_tests.sh` exited before report generation; no `reports/report_*` path or `results.xml` was created.

## Native
[doctest] test cases:     544 |     544 passed | 0 failed | 0 skipped

## gdUnit per-suite counts
Unavailable: the launcher failed during script parsing before test discovery, so no `results.xml` was generated.

## gdUnit failing cases
Unavailable: the launcher failed during script parsing before test discovery, so there are no testcase results to extract.

Launcher output:
```
SCRIPT ERROR: Parse Error: Could not find type "GdUnitTestCIRunner" in the current scope.
          at: GDScript::reload (res://addons/gdUnit4/bin/GdUnitCmdTool.gd:5)
SCRIPT ERROR: Parse Error: Identifier "GdUnitTestCIRunner" not declared in the current scope.
          at: GDScript::reload (res://addons/gdUnit4/bin/GdUnitCmdTool.gd:10)
ERROR: Failed to load script "res://addons/gdUnit4/bin/GdUnitCmdTool.gd" with error "Parse error".
   at: load (modules/gdscript/gdscript_resource_format.cpp:46)
exit=1
```

Flaky by case (compare failure COUNT): test_connectivity, test_island_body.

## Evidence log
Appended by later tasks: bite proofs, attributions, per-milestone gate results.

### Task 4 bite proofs
- (a) skipped SsrPass delete -> contract suite not discovered: `SCRIPT ERROR: Parse Error: Could not find type "GdUnitTestCIRunner" in the current scope.` (launcher exit 1)
- (b) residency before streamer -> contract suite not discovered: `SCRIPT ERROR: Parse Error: Could not find type "GdUnitTestCIRunner" in the current scope.` (launcher exit 1)
- (c) uploads cleared in teardown_gpu -> contract suite not discovered: `SCRIPT ERROR: Parse Error: Could not find type "GdUnitTestCIRunner" in the current scope.` (launcher exit 1)

### Task 6 gate
- Report: `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-6-report.md`
- Build: OK.
- Native: 551/551 passed, 0 failed; baseline was 544/544, with the +7 cases attributed to Task 5's pure handoff/native tests. Behavior matches baseline.
- gdUnit: same pre-existing `GdUnitTestCIRunner` parse error before discovery; launcher exit 1, no `results.xml` or suite counts. No blocker repair was made.
- Result: matches baseline after the attributed native-count difference; teardown-trace gate remains covered but could not be discovered because of the known launcher blocker.

### Task 7 gate
- Recorded 2026-09-15T01:01:32Z at HEAD `f3a1845` before commit.
- Static gate: `git diff --check` passed; no launcher paths changed; IslandManager lock-site counts remained 15 total / 6 `edit_mutex()` acquisitions.
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` exited 0; `Build OK`, universal macOS debug dylib linked.
- Native: `(cd extension && scons -Q test) 2>&1 | tail -3` — `551/551` test cases passed, `9,117,142/9,117,142` assertions passed, status success.
- gdUnit: the requested eight-suite launcher exited 1 before discovery with the known pre-existing `GdUnitTestCIRunner` parse error; no launcher repair or test modification was made.
- Result: native/build match Task 6 baseline; gdUnit remains blocked before discovery by the same known launcher error.
