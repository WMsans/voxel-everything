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
