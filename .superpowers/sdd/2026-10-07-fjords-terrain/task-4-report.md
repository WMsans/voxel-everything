# Task 4 report — bake, read back, and mirror sectors on the render device

**Status:** Implemented and verified on `feat/fjords`.

**Commit:** `6588d61 feat: bake sectors on the render device, read them back and mirror them into set 1`

## Implementation

- Added the sector GLSL sampling API, map fixture and analytic field fixture, and compute bake shader.
- Added `SectorBakePush` and regenerated only `shaders/generated/blocks.glslh`.
- Added `SectorMirror` and `SectorContext`: fixed-size texture-array mirrors, toroidal window upload, per-sector GPU bake, asynchronous readback into the shared host cache, and render readiness.
- Wired the shared `SectorCache` into `WorldStore`, the CPU generator, `FieldContextSet`, render graph and frame loop; set 1 binds the only accepted sector sampler type (`texture2d_rg16` consumer) and its map.
- Added CPU fixture mirror, debug hooks, test fixture and GPU tests. Updated field differential tests to use the shipping `FieldContextSet`/`SectorMirror`, bake fixture sectors and compare surface samples.
- Updated the generated block-layout test's expected macro count from 33 to 34.

## TDD and debugging

- Test-first red: the new sector suite failed because the fixture pipeline and `debug_pump_sectors` hook did not exist yet.
- After integration, `test_field_diff.gd` exposed a fixture-only GPU/CPU mismatch. Data-flow diagnostics showed sector map bytes were correct on the device but the shader treated all sectors as absent. On Metal, `sector_map.slot.length()` returned zero for the runtime-sized SSBO array.
- Changed the guard in `shaders/sector.glslh` to the fixed `SECTOR_WINDOW * SECTOR_WINDOW * 3` capacity. This is safe because sector maps have a fixed 24×24×3 layout, while pipelines without a map stage do not include/call this sector API. The differential suite then passed, including far-offset coverage.

**Ruling:** Use a fixed sector-map capacity bound instead of GLSL runtime-array `.length()` — Metal reports zero for this runtime array in the exercised shader path, while the sector map layout is fixed and no-map pipelines do not call the helper — cost if wrong: an unexpected future variable-size sector map would need its capacity contract updated.

## Verification

- `./build.sh` — passed.
- `./build.sh --verify` — passed; extension loaded and native types resolved.
- `cd extension && scons -Q test` — `827/827` cases and `11,668,211/11,668,211` assertions passed; `Status: SUCCESS!`.
- `VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc="*generated*"` — `7/7` cases passed; `git diff --stat -- shaders/generated` showed only `blocks.glslh` changed.
- `./gdunit_tests.sh -a res://tests/test_sectors.gd` — `3/3` passed, GPU/Metal enabled.
- `./gdunit_tests.sh -a res://tests/test_field_diff.gd` — `2/2` passed, including every pipeline and far-seeded differential checks.
- Task 0 pin suites all passed: `test_field_diff` 2/2, `test_mesh_lattice` 3/3, `test_leaves` 18/18, `test_create_world` 9/9, `test_world_scene` 6/6, `test_material_picker` 5/5, `test_frame_shipped_golden` 1/1, `test_demo_scene_wiring` 5/5.
- `git diff --check` passed. `tests/golden` and `shaders/generated/field.glslh.golden` were not changed; the only generated shader golden change is `shaders/generated/blocks.glslh`.

## Concerns

The full gdUnit suite was not rerun; the Task 0 ledger records existing unrelated baseline failures. All Task 0 pin suites and the prescribed Task 4 GPU suites passed. No other known Task 4 concerns.

## Review fix — guard failed sector initialization

**Status:** Fixed the Important finding only; Minor findings were left untouched.

**Change:** `debug_pump_sectors()` now captures and checks the render context, ensures initialization, checks readiness and local-device mode, then accesses the render device and passes. Failed pipeline initialization returns `-1` without looking up `passes().sectors`. Added this failed-initialization expectation to `tests/test_generator_seam.gd`.

**TDD note:** Added the assertion before the implementation edit and ran it against the old code. The deterministic invalid-pipeline world returned `-1` even before the fix because this repository's `VoxelWorld` constructor always owns a `RenderOrchestrator`; a null orchestrator cannot be induced via the public test harness. The test pins the failed-initialization behavior, while the code change fixes the reviewed ordering. The failed pipeline intentionally logs `cannot read res://assets/pipelines/missing.pipeline` twice; suite result is still successful.

**Files touched:** `extension/src/debug/hooks_world.cpp`, `tests/test_generator_seam.gd`.

**Verification (fresh):**

- Command: `./build.sh`
  - Actual output: `Build OK: 3.5M libvoxel_everything.macos.template_debug.universal.dylib`; `Done.`
- Command: `./gdunit_tests.sh -a res://tests/test_generator_seam.gd`
  - Actual output: `3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED`; `Executed test cases : (3/3)`; `Exit code: 0`.
  - Expected invalid-pipeline diagnostic: `ERROR: terrain pipeline: cannot read res://assets/pipelines/missing.pipeline`.
- `git diff --check` — passed.

**Concern:** The regression uses the public failed-pipeline path; actual null `context().render` cannot be constructed through this harness. The guard is explicit nonetheless.
