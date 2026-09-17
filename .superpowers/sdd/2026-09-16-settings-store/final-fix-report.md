# Final-review fix wave report — settings-store

## Files changed

- `extension/src/voxel_settings.h` — declared the stand-in render-store listener callback.
- `extension/src/voxel_settings.cpp` — wired the listener and rebased the beauty stand-in with `SettingsStore::rebase(settings_for_tier(...))`.
- `extension/src/connectivity/contact_refine.cpp` — rejected axes outside `[0, 2]` before indexing/modulo arithmetic.
- `demo/settings_menu.gd` — removed `toggle_key`; the panel toggle is now literal `KEY_F1` only.
- `extension/tests/test_contact_refine.cpp` — added invalid-axis regression coverage.
- `tests/test_voxel_settings.gd` — added stand-in tier rebase/override-preservation coverage without a live world.
- `tests/test_settings_menu.gd` — added coverage that the panel exposes no configurable toggle key.

## TDD and verification

### Regression tests first (red)

- `cd extension && scons -Q test`
  - Result: initial native build/run passed the existing 612 cases; the first invalid-axis test used an open-sky coordinate and did not expose the undefined behavior, so the test was tightened to a solid coordinate before production edits.
- `cd extension && scons -Q test` after tightening the invalid-axis test
  - Result: failed as expected: 611 passed, 1 failed; invalid axes returned `81` instead of `0`.
- `./gdunit_tests.sh -a res://tests/test_voxel_settings.gd -a res://tests/test_settings_menu.gd`
  - Result: failed as expected for the two new regressions: stand-in `ssgi_taps` remained `8` instead of `0`, and `toggle_key` was still exposed. The required ambient test also hit its known environment limitation below.

### Production fix and green verification

- `./build.sh --test`
  - Result: exit `0`; extension build succeeded and doctest reported `612` test cases and `9,119,704` assertions passed, `0` failed.
- `./gdunit_tests.sh -a res://tests/test_voxel_settings.gd -a res://tests/test_settings_menu.gd -i res://tests/test_voxel_settings.gd:test_an_ambient_change_reaches_the_object_global`
  - Result: exit `0`; `38` focused settings/panel test cases passed, `0` errors/failures.
- `./gdunit_tests.sh -a res://tests/test_voxel_settings.gd -a res://tests/test_settings_menu.gd`
  - Result: exit `100`; all fix-related tests passed, with `1` error from the required ambient test limitation below.
- `git diff --check`
  - Result: no whitespace errors.

## Known ambient limitation

`test_an_ambient_change_reaches_the_object_global` remains unchanged and was not skipped, deleted, weakened, or capability-gated. On Godot `4.7.2` with the Metal renderer, `RenderingServer.global_shader_parameter_get("ve_ambient")` logs that it is editor-only and returns `Nil`; assigning that value to `Vector3` errors. This is the pre-existing documented ambient limitation, not a failure caused by this fix wave.

## Constraints and concerns

- `frame.cpp` snapshot locals and `beauty_settings()` reads were left untouched.
- No new store, lock, separate architecture, tabs, C++20 change, or golden-file change was introduced.
- No concerns beyond the known ambient limitation above.
