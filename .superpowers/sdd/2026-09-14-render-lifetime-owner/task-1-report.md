# Task 1 report — SP1 seam-probe migration

## Status

DONE_WITH_CONCERNS. Production changes are committed. The required gdUnit suites could not launch because of the pre-existing `GdUnitTestCIRunner` parse error; no addons or test infrastructure were modified.

## Implementation

- Replaced `VoxelDebugHooks::debug_seam_probe`'s hand-built raymarch, G-buffer, deferred, inject, and LoD rendering with `VoxelFrame::looking_at` plus `render_headless`.
- Preserved the existing probe signature and all existing result keys:
  `band_pixels`, `band_pixels_unclaimed`, `band_pixels_double_claimed`, `neither`, `both`, `near_pixels_lost_to_lod`, `far_pixels_lost_to_raymarch`, and `draw_pages`.
- Added the required `error` result when `near_field_scale < 1.0`.
- Attached `FrameDebug::marker`, `skip_far_field`, and `lod_viewport = Vector2i(2560, 1440)`.
- Classified against `FrameRecord::fade_start` / `fade_end` and required composite/inject stage success.
- Set `near_field_scale = 1.0` in both LoD suites so hit-position classification is full-resolution.

## TDD evidence

This task changes an existing probe and its existing GPU suites; no new test case was specified by SP1. The pre-migration suite run was attempted before production changes. A test-only red run was also attempted after the suite setup edits and before the C++ migration. Both were blocked before executing any test case by the same launcher parse error, so there was no semantic red/green gdUnit result to capture.

## Commands and exact results

### Pre-migration probe check

Command:

```bash
rg -n 'render_headless|world_->frame\(\)' extension/src/debug/hooks.cpp | rg -n seam || true
sed -n '/Dictionary VoxelDebugHooks::debug_seam_probe/,/^}/p' extension/src/debug/hooks.cpp | head -40
```

Result: no matching `render_headless` / `world_->frame()` seam line; the output showed the old hand-built probe body.

### Pre-migration suite run

Command:

```bash
./gdunit_tests.sh -a res://tests/test_lod_seam.gd,res://tests/test_lod_gbuffer.gd 2>&1 | tee .superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-seam_before.log
```

Result: exit code 1 before tests ran.

```text
SCRIPT ERROR: Parse Error: Could not find type "GdUnitTestCIRunner" in the current scope.
  at: GDScript::reload (res://addons/gdUnit4/bin/GdUnitCmdTool.gd:5)
SCRIPT ERROR: Parse Error: Identifier "GdUnitTestCIRunner" not declared in the current scope.
  at: GDScript::reload (res://addons/gdUnit4/bin/GdUnitCmdTool.gd:10)
ERROR: Failed to load script "res://addons/gdUnit4/bin/GdUnitCmdTool.gd" with error "Parse error".
```

### Test-only red run

Command:

```bash
./gdunit_tests.sh -a res://tests/test_lod_seam.gd,res://tests/test_lod_gbuffer.gd 2>&1 | tee .superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-seam_test_red.log
```

Result: exit code 1 with the identical `GdUnitTestCIRunner` parse error above; no test case ran.

### Build

Command:

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Result: exit code 0.

```text
==> Build OK: 3.1M libvoxel_everything.macos.template_debug.universal.dylib
==> Done.
```

### Post-migration suite run

Command:

```bash
./gdunit_tests.sh -a res://tests/test_lod_seam.gd,res://tests/test_lod_gbuffer.gd 2>&1 | tee .superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-seam_after.log
```

Result: exit code 1 with the identical pre-existing `GdUnitTestCIRunner` parse error; no test case ran. Therefore no seam measurements, case counts, or golden attribution were available. No golden file moved.

### Staged diff check

Command:

```bash
git diff --cached --check
```

Result: exit code 0, no output.

## Files changed

Implementation commit files:

- `extension/src/debug/hooks.cpp`
- `tests/test_lod_seam.gd`
- `tests/test_lod_gbuffer.gd`

Scratchpad logs:

- `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-seam_before.log`
- `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-seam_test_red.log`
- `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-seam_after.log`

This report is at `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-report.md`.

## Self-review

- API signature is unchanged.
- All pre-existing result keys remain present; `error` is additive.
- The marker RID is released only after frame-owned pass targets release their cached framebuffers.
- No addon, shader, pass internals, or infrastructure files were changed.
- `git diff --cached --check` passed before commit.
- The C++ build passed after the migration.

## Concerns / blockers

- The required gdUnit launcher is blocked by the known parse error for `GdUnitTestCIRunner` in `addons/gdUnit4/bin/GdUnitCmdTool.gd`. Per task instructions, it was not modified.
- Because the suites did not execute, the expected `skip_lod == false` seam measurements and post-change suite case counts could not be attributed. The commit therefore records `Moved values: none.` and makes no golden changes.

## Commit

`78d7cac refactor(probe): debug_seam_probe renders the shipped frame with a field marker`

## Round 1 fix report — measured validation blocker

### Finding resolution

No legitimate existing direct test path can produce the required seam measurements. The only non-documentation callers of `debug_seam_probe` are `tests/test_lod_seam.gd` and `tests/test_lod_gbuffer.gd`, and both require the gdUnit runner. The existing direct `SceneTree` tools do not call the seam probe. The stop-and-report rule is explicit: when the launcher and an equivalent direct path are unavailable, stop; do not infer unchanged seam behavior or assign Step 5 attribution.

The launcher remains unmodified because its failure is pre-existing and outside Task 1. No addon or other test infrastructure was changed.

### Commands and exact results

#### Existing direct test path check

Command:

```bash
godot --path . --headless --check-only --script res://tests/test_lod_seam.gd
```

Output:

```text
SCRIPT ERROR: Parse Error: Could not find base class "GdUnitTestSuite".
  at: GDScript::reload (res://tests/test_lod_seam.gd:1)
ERROR: Failed to load script "res://tests/test_lod_seam.gd" with error "Parse error".
exit code: 0
```

This is not an executable seam test: direct script loading does not register the gdUnit base class and produces no probe output.

#### Fresh launcher check

Command:

```bash
./gdunit_tests.sh -a res://tests/test_lod_seam.gd,res://tests/test_lod_gbuffer.gd
```

Output:

```text
==> Running gdUnit4 against a real display (GPU rendering enabled, vsync on)
godot_binary: /opt/homebrew/bin/godot
tests: res://tests/test_lod_seam.gd res://tests/test_lod_gbuffer.gd
Godot Engine v4.7.2.stable.official.ed1daf0bf - https://godotengine.org
Metal 4.0 - Forward+ - Using Device #0: Apple - Apple M1 (Apple7)

SCRIPT ERROR: Parse Error: Could not find type "GdUnitTestCIRunner" in the current scope.
  at: GDScript::reload (res://addons/gdUnit4/bin/GdUnitCmdTool.gd:5)
SCRIPT ERROR: Identifier "GdUnitTestCIRunner" not declared in the current scope.
  at: GDScript::reload (res://addons/gdUnit4/bin/GdUnitCmdTool.gd:10)
ERROR: Failed to load script "res://addons/gdUnit4/bin/GdUnitCmdTool.gd" with error "Parse error".
   at: load (modules/gdscript/gdscript_resource_format.cpp:46)
exit code: 1
```

No test case ran, so there are no seam measurements, case counts, or Step 5 attribution to record. No golden file was moved.

#### Build re-check

Command:

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Output:

```text
==> Building libvoxel_everything with scons (-j8)...
Building for architecture universal on platform macos
scons: `godot-cpp/bin/libgodot-cpp.macos.template_debug.universal.a' is up to date.
scons: `bin/libvoxel_everything.macos.template_debug.universal.dylib' is up to date.
==> Build OK: 3.1M libvoxel_everything.macos.template_debug.universal.dylib
==> Done.
```

Exit code: 0.

#### Report integrity check

Command:

```bash
git diff --check -- .superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-report.md
```

Output: no output; exit code 0.

### Changed files

- `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-1-report.md` — appended this fix report only.

### Concern

The task remains unreviewable for measured seam preservation and Step 5 attribution until the pre-existing gdUnit launcher is repaired outside this task or an equivalent legitimate runner is supplied. The production build remains successful; this report makes no rendering-behavior claim beyond that build result.
