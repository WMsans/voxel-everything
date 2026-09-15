# Render lifetime owner — measured results

Recorded 2026-09-14 19:27 PDT on macOS 26.4.1 / Apple M1. This report covers the
implementation revision `48eb783` (`refactor: split debug hooks by module (pure move)`).

## 1. Revision and environment

| Item | Result |
|---|---|
| SP2 baseline | `ca9b87b` (`docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md`) |
| Final implementation revision | `48eb783e100ef649fb23315e044c51945bb99f6b` |
| OS / GPU | macOS 26.4.1 / Apple M1 / Metal 4.0 |
| Godot | 4.7.2.stable.official.ed1daf0bf |
| Report | `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md` |
| gdUnit report path | None: launcher failed before discovery; no `reports/report_*` or `results.xml` was created |

Final build command and output:

```text
$ ./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc); echo "build=$?"
==> Building libvoxel_everything with scons (-j8)...
Building for architecture universal on platform macos
scons: `godot-cpp/bin/libgodot-cpp.macos.template_debug.universal.a' is up to date.
scons: `bin/libvoxel_everything.macos.template_debug.universal.dylib' is up to date.

==> Build OK: 3.2M libvoxel_everything.macos.template_debug.universal.dylib
    Registered native classes: VoxelWorld, RaymarchCompositor
    Open the project in Godot (or press the reload button in the
    GDExtension inspector) — the 'Could not find type VoxelWorld'
    parse errors will disappear once the library is loaded.

==> Done.
build=0
```

Final native command and output:

```text
$ (cd extension && scons -Q test) 2>&1 | tail -3
[doctest] test cases:     551 |     551 passed | 0 failed | 0 skipped
[doctest] assertions: 9117142 | 9117142 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Final full gdUnit command and output:

```text
$ ./gdunit_tests.sh; echo "gdunit=$?"
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
gdunit=1
```

The required Task 3 extraction command was also run. It printed `ls: reports/report_*: No
such file or directory` and then failed to open `/results.xml`; therefore final suite counts
and testcase failures are unavailable, not zero.

## 2. Sub-project 1 acceptance

Evidence sources: Task 1 commit `78d7cac`, Task 2 final commit `ca9b87b`, Task 10
`fdb4511`, Task 11 `9ce7f97`/`e725994`, Task 12 `ef9250f`, and the Task 13 pure-move
report. Runtime GPU claims remain open because the same launcher parse error blocked every
suite before discovery.

| SP1 acceptance line | Result | Evidence / limitation |
|---|---|---|
| Both compositors call owned `VoxelFrame`; shipped golden passes unchanged | OPEN | Source routing landed in Task 9; no golden source moved, but the shipped golden could not execute. |
| Six frame-rebuilding probes use the shared frame and preserve signatures/keys | PASS (static) / runtime open | Task 1 migrated `debug_seam_probe`; Task 2 completed shared inputs and orphan deletion; Task 12 found `_bind_methods()` byte-identical and Task 13 preserved 183 definitions/176 bindings and 653 keys. GPU execution was blocked. |
| Isolated probes share camera/world inputs and grass reach while keeping fixtures | PASS (static) / runtime open | Task 2 implementation and Task 12 source checks show the shared helpers and retained callers; pass suites were not discovered. |
| Headless contracts cover successful stages, determinism, abort/recovery, HiZ-off and resize history | OPEN | `tests/test_frame_contract.gd` exists and routes through `debug_render_frame`; no case ran. |
| Native suite passes and GPU suite has no unexplained regression/missing cases | OPEN | Native passes 551/551; full GPU suite is blocked before discovery and has no counts. |
| Every moved golden/assertion has a measured cause; suspected bugs block completion | OPEN | No golden file moved in source, but no measured GPU run exists; no runtime attribution can be claimed. |
| Orphaned world plumbing is removed and sizes/callers are reported | PASS (static) | Removed symbols and callers are documented below; final line counts are measured in §7. |
| Stage order, admission/locking, lifetime, pass internals and shaders remain constrained | PASS (static) | Task 9–13 audits show no stage reorder except S8, no new lock site, and no pass/shader changes. |
| Spec, implementation status and results agree; `FrameHost` remains SP2 debt | OPEN until this amendment commit | This report records `FrameHost` deletion by SP2 and the status/gate amendments are made with it; runtime gates remain open. |

## 3. Verification

### Native and suite comparison

The SP2 baseline recorded `544/544` native cases and `9,117,092` assertions. The final
native run recorded `551/551` cases and `9,117,142` assertions, all passing. The increase of
7 cases and 50 assertions is the pure `IslandHandoff` native coverage from Task 5; it is not
a regression.

The baseline had no gdUnit suite counts, testcase failures, report directory, or results XML:
the launcher failed at script parse time. The final run has the identical pre-discovery
failure, so there are no per-suite counts to compare and no remaining testcase-level failure
messages. The only observed failure is the launcher error:

```text
Could not find type "GdUnitTestCIRunner" in the current scope.
Identifier "GdUnitTestCIRunner" not declared in the current scope.
Failed to load script "res://addons/gdUnit4/bin/GdUnitCmdTool.gd" with error "Parse error".
```

No addon, launcher, or test infrastructure was modified. This is a blocked gate, not a
runtime pass and not evidence that the runtime criteria pass.

### Lifetime, golden, frame contract and S8

- Lifetime contract: source suite and teardown trace exist; trace source remains
  `passes, streamer, residency, island_graph, island_slots, atlas, lod, history, initialized`.
  All six runtime cases are unverified because the launcher failed before discovery.
- Shipped golden: no golden file moved, but the golden suite did not run; pass is unverified.
- Frame contract: `test_frame_contract.gd` exercises `debug_render_frame` and its requested
  cases are present; all are unverified for the same reason.
- S8: failing test commit `9ce7f97` precedes fix commit `e725994`; the fix moves the deferred
  timing begin below SSAO. The marker-name test and post-fix green result are unverified.
  Planning evidence confirms Metal captures marker names; `demo/benchmark.gd` has no
  `deferred` key, so no benchmark label edit was made.

## 4. Bite proofs

The three deliberate lifetime breaks were applied and restored in Task 4. Each rebuild
completed, but each contract-suite invocation stopped at the pre-existing launcher parse
error before any case ran. Therefore none of these is claimed as a runtime failing proof:

| Break | Intended assertion | Measured result |
|---|---|---|
| Skip `SsrPass` deletion | Leak case detects the omitted delete | **UNVERIFIED** — build passed; gdUnit blocked before discovery. |
| Move residency teardown before streamer | Teardown-order case detects the swap | **UNVERIFIED** — build passed; gdUnit blocked before discovery. |
| Clear uploads in `VoxelWorld::teardown_gpu()` | Upload-survival case detects dropped queued data | **UNVERIFIED** — build passed; gdUnit blocked before discovery. |

## 5. Golden attribution

**No golden moved.** Task 1 and Task 2 reports explicitly record no measured golden movement,
and no golden/test source was changed by the SP2 implementation. This is a source/diff fact,
not a substitute for the blocked runtime comparison. No moved number or assertion can be
attributed until the gdUnit launcher is repaired outside this task.

## 6. Locking changes

| Change | Commit | Evidence |
|---|---|---|
| Removed `island_mutex_` hold around manager creation | `f3a1845` | Manager-pointer protection was replaced by atomic handoff state; `edit_mutex` remains. |
| Removed `island_mutex_` hold around manager detach in physics teardown | `f3a1845` | Detach/reset uses `edit_mutex` plus atomic `manager_slots`. |
| Removed `island_mutex_` hold around GPU high-water reset | `f3a1845` | `handoff_.reset_debug_slots()` is atomic. |
| Removed `island_mutex_` hold around the debug high-water poke | `f3a1845` | `handoff_.note_debug_slot()` is the leaf API. |
| Relocated the existing `lod_mutex_` hold from `debug_lod_stats` into `LodSystem::stats()` | `ef9250f` | One new `stats()` hold with `ensure_lod()`; Task 12 measured old 5 sites/new 6 sites, exactly the specified relocation. |

Task 6's lock audit found no new lock site; the only new lock is the specified relocated LoD
snapshot hold. The handoff mutex remains confined to handoff leaf methods.

## 7. Deletion and size

### Final line-count command

The required command used baseline `5f2d897` and printed:

```text
extension/src/voxel_world.h      509 ->      209
extension/src/voxel_world.cpp     1270 ->      921
extension/src/render/orchestrator.h      356 ->      363
extension/src/render/orchestrator.cpp      668 ->      726
extension/src/debug/hooks.cpp     5892 ->      376
extension/src/physics/island_manager.cpp     1518 ->     1565
extension/src/render/frame.cpp      632 ->      627
     122 extension/src/render/island_handoff.cpp
     106 extension/src/render/island_handoff.h
     541 extension/src/render/island_handoff.o
      51 extension/src/render/island_handoff.os
     882 extension/src/debug/hooks_lod.cpp
    1013 extension/src/debug/hooks_physics.cpp
    2601 extension/src/debug/hooks_render.cpp
    1322 extension/src/debug/hooks_world.cpp
      38 extension/src/debug/hooks_common.h
    6676 total
```

The `island_handoff.o` and `.os` lines are generated build artifacts included by the
brief's `island_handoff.*` wildcard; source-only handoff files are 122 and 106 lines.

### Removed symbols and ownership fields

Removed symbols include `FrameHost`; the 25 per-pass accessors; the VoxelWorld render/LoD/atlas
forwarders deleted by Tasks 9–10; `island_mutex_` and its queue state; `note_lod_cull_debug`
and its old atomics; `set_beauty_compositor`/`beauty_compositor_`; `VoxelWorld::render_probe_pixel`;
`analytic_raycast_down`; `IslandManager::initialize(VoxelWorld*)`; both
`friend class VoxelDebugHooks` declarations; and `VoxelWorld::set_generator`.

`RenderOrchestrator::Collaborators` has exactly these five fields:

1. `const bool *use_local_device`
2. `WorldStore *store`
3. `LodSystem *lod`
4. `Object *callback_owner`
5. `std::function<void()> ensure_initialized`

### Hooks pure move

Task 13's prescribed reassembly check returned `PURE`; definition and binding totals remained
183 and 176. Sorted public method names and all 653 Dictionary-key occurrences matched. The
brief's Task 14 key command omits `hooks_common.h`, so it reports four moved frame-record keys
as deletions; the corrected command including `hooks_common.h` returns `corrected_keys=0`.

## 8. Change-cost retrace

A hypothetical `FogPass` that reads the G-buffer and writes scene colour post-opaque has this
measured path from the current code:

1. `render/fog_pass.h` (new pass interface; currently absent)
2. `render/fog_pass.cpp` (new pass implementation; currently absent)
3. `shaders/fog.comp.glsl` (new shader; currently absent)
4. `render/orchestrator.h` (`RenderPasses::fog`; existing table)
5. `render/orchestrator.cpp` (build in `ensure_gpu_graph`, delete in `teardown_render_passes`)
6. `render/frame.h` (`kStageFog`; existing stage enum)
7. `render/frame.cpp` (stage execution and label)
8. `render/gpu_timings.cpp` (`kPasses`; required for timing attribution)
9. `tests/test_frame_contract.gd` (existing `debug_render_frame` route; add the Fog assertion)

The timing-aware count is **9**, one over the ≤8 target, because `known_pass()` drops an
unknown label: `gpu_timings.cpp` accepts `frame` or a name in `kPasses`, and the current table
has no `fog`. The eight-file core path excluding timing registration is the target list; the
ninth file is the documented candidate for generation from `FrameStage` in sub-project 4.
No code was changed for this retrace.

## 9. Exit criteria

| Spec §6 criterion | Result | Evidence / open finding |
|---|---|---|
| No `FrameHost`, friends, `**lod_pool`, or `island_mutex =` residual | PASS | Exact first `rg` command printed no output. |
| No island mutex/orphan API residuals | OPEN / literal check not clean | Exact second `rg` printed only the historical comment `VoxelWorld::analytic_raycast_down` in `extension/src/core/world_store.h`; executable declarations/definitions are gone. No production comment cleanup was allowed in this docs-only task. |
| Collaborators ≤5; `voxel_world.h` ≤250 | PASS | Five fields; header is 209 lines. |
| New render pass retrace ≤8 files | OPEN / FAIL against timing-aware path | Nine files if Fog timing is retained; `kPasses` currently drops unknown labels. |
| Lifetime contract, shipped golden, frame contract and S8 pass unchanged | OPEN / unverified | All GPU suites were blocked before discovery; no runtime criterion is claimed. |
| Bound method names/signatures and Dictionary keys preserved | PASS (static) / runtime open | VoxelWorld bindings byte-identical; hooks retain 183 definitions/176 bindings; corrected all-source key comparison is empty; only `debug_teardown_trace` is added. |
| Roadmap status/gate amendments | PASS after this docs commit | SP1/SP2 status and RenderPasses gate wording are amended below. |
| Results report contains SP1 acceptance, verification, attribution, sizes, retrace and bite proofs | PASS | This report contains sections 2–8 and explicitly records blocked gates. |

### Open findings and deferred review minors

- The pre-existing `GdUnitTestCIRunner` parse error blocks all GPU discovery, including the
  lifetime contract, shipped golden, frame contract, S8 marker test, and bite proofs. It was
  not repaired here.
- The exact orphan grep is not literally empty because of one historical `WorldStore` comment;
  the executable API is absent. Cleaning that comment was deferred because this task makes no
  production changes.
- The Task 14 key command omitted `hooks_common.h`; the corrected all-source check passes.
- Task 13's planning note said 172 split units, while the exact script on the reviewed source
  reports 174; reassembly, pure-move equality and 183/176 counts pass. This is a deferred
  review-minor discrepancy, not a forced script change.
- An unchanged pass comment still names `world_->region_window()`; pass files were out of scope.
- The broad `set_generator` grep is overinclusive; qualified `WorldStore::set_generator` and
  `IslandManager::set_generator` calls are required, while `VoxelWorld::set_generator` is gone.

## 10. Files and self-review

Task 14 changes only these documentation files:

- `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md` (this report)
- `docs/superpowers/specs/2026-09-14-render-lifetime-owner-design.md`
- `docs/superpowers/specs/2026-09-13-frame-module-design.md`
- `docs/superpowers/plans/2026-09-13-frame-module.md`
- `docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md` (staged as required;
  no content change is needed beyond the prior evidence log)

Self-review completed before commit:

- No production, pass, shader, launcher, addon, or test infrastructure file is changed.
- No new lock, pass, shader, API, or stage-order change is introduced.
- Every blocked/open runtime gate is labeled unverified rather than claimed green.
- Exact command output, evidence commits, open findings, and deferred review minors are
  recorded; the baseline log remains part of the required staging set.
