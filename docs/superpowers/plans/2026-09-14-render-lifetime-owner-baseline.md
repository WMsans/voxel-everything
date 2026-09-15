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

### Task 8 gate
- Recorded 2026-09-15T01:17:27Z at HEAD `88a0482` before commit.
- Static ownership gates: VoxelWorld deleted-state audit had no matches; obsolete collaborator/slot audit had no matches; `Collaborators` field count was `5`; changed files were exactly the seven brief-listed sources; no pass/shader files or DeferredPass/ContactShadowPass lifecycle changes appeared in the diff.
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` exited 0; `Build OK`, universal macOS debug dylib linked.
- Native: `(cd extension && scons -Q test) 2>&1 | tail -3` — `551/551` test cases passed, `9,117,142/9,117,142` assertions passed, status success.
- gdUnit: the requested eleven-suite launcher exited 1 before discovery with the known pre-existing `GdUnitTestCIRunner` parse error; no launcher repair or test modification was made.
- Result: build/native match Task 7; teardown trace source remains `passes, streamer, residency, island_graph, island_slots, atlas, lod, history, initialized`; gdUnit remains blocked before discovery by the same known launcher error.

### Task 9 gate
- Recorded 2026-09-15 at HEAD `26e5f8f` before commit.
- Static ownership gates: `git diff --check` passed; `FrameHost` and obsolete `VoxelWorld` frame/handoff forwarders had no executable residuals; `LodSystem` precedes `render_` in `VoxelWorld`; `VoxelFrame` is the final `RenderOrchestrator` member; no pass/shader/test files changed; no new locks or stage-order edits.
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` exited 0; `Build OK`, universal macOS debug dylib linked.
- Native: `(cd extension && scons -Q test) 2>&1 | tail -3` — `551/551` test cases passed, `9,117,142/9,117,142` assertions passed, status success.
- gdUnit: the requested ten-suite launcher exited 1 before discovery with the known pre-existing `GdUnitTestCIRunner` parse error; no launcher repair or test modification was made.
- Result: build/native match Task 8; shipped golden/test sources are unchanged; gdUnit remains blocked before discovery by the same known launcher error.

### Task 10 gate (full run)
- Report: `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-10-report.md`
- Static/API audits: `RenderPasses` has the exact 25 requested fields; `passes()` is the sole pass-graph accessor; old pass accessors and deleted VoxelWorld forwarders have no residual declarations/calls; `_bind_methods()` is unchanged; all 384 unique Dictionary keys are unchanged; `git diff --check` passed.
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` exited 0; `Build OK`, universal macOS debug dylib linked.
- Native: `(cd extension && scons -Q test)` — `551/551` test cases passed, `9,117,142/9,117,142` assertions passed, status success.
- gdUnit: `./gdunit_tests.sh` exited 1 before discovery with the known pre-existing `GdUnitTestCIRunner` parse error; no `results.xml` or suite counts were produced. The launcher was not repaired.
- Result: implementation/build/native gates pass; full gdUnit remains blocked before discovery by the same baseline launcher error; teardown order is unchanged after mechanical member-name normalization.

### Task 12 gate (full run)
- Recorded 2026-09-15 at HEAD `e725994` before commit.
- Static/API checks: `LodStats` and `WorldStats` public APIs are present; `LodSystem::stats()` holds `lod_mutex_` and calls `ensure_lod()`; both `friend class VoxelDebugHooks` declarations are absent; `voxel_world.h` is 209 lines; no `VoxelWorld::set_generator` declaration/definition remains; the `_bind_methods()` block is byte-identical; all 653 Dictionary key occurrences in `hooks.cpp` are in the same order; no pass/shader/test files changed.
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` exited 0; `Build OK`, universal macOS debug dylib linked. The only compile-driven include fix was `#include "render/orchestrator.h"` in `extension/src/debug/hooks.cpp`.
- Native: `(cd extension && scons -Q test)` — `551/551` test cases passed, `9,117,142/9,117,142` assertions passed, status success.
- gdUnit: `./gdunit_tests.sh` exited 1 before discovery with the known pre-existing `GdUnitTestCIRunner` parse error; no `results.xml` or suite counts were produced. The launcher was not repaired.
- Result: implementation/build/native gates pass; full gdUnit remains blocked before discovery by the same baseline launcher error; no pass/shader changes or lifecycle-call changes were made.
