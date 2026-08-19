# Task 9 report — POST_OPAQUE BeautyCompositor, contact shadows, and history

## Status

Implemented and committed as `2e25e23b3dd68bcbf7551fc489f4f2150481b294` (`feat: post-opaque beauty compositor with screen-space contact shadows`).

## RED/GREEN evidence

### RED

The specified GdUnit suite was written first at `tests/test_contact_shadow.gd`.

Initial command:

```text
./gdunit_tests.sh -a res://tests/test_contact_shadow.gd
```

The first attempt exposed strict GDScript type inference in this project:

```text
Parse Error: Cannot infer the type of "d" variable because the value doesn't have a set type.
```

The test was corrected to declare the returned dictionaries explicitly. The next run produced the intended missing-hook failure:

```text
Invalid call. Nonexistent function 'debug_contact_shadow_probe' in base 'VoxelWorld'.
5 test cases | 4 errors | 0 failures
Exit code: 100
```

During final verification, moving `bayer4` was initially placed incorrectly under `MATERIAL_LAYERS`; the runtime shader RED evidence was:

```text
ContactShadowPass: Failed parse:
ERROR: 0:269: 'bayer4' : no matching overloaded function found
```

The helper was moved outside the conditional include block before the final GREEN run.

### GREEN

Final focused contact-shadow run:

```text
./gdunit_tests.sh -a res://tests/test_contact_shadow.gd -c
```

```text
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Executed test suites: (1/1)
Executed test cases : (5/5)
Exit code: 0
Run tests ends with 0
```

Focused settings/G-buffer/deferred regression run:

```text
./gdunit_tests.sh -a res://tests/test_contact_shadow.gd -a res://tests/test_beauty_settings.gd -a res://tests/test_gbuffer.gd -a res://tests/test_deferred.gd -c
```

```text
Overall Summary: 19 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Executed test suites: (4/4)
Executed test cases : (19/19)
Exit code: 0
Run tests ends with 0
```

Native/build verification:

```text
./build.sh --test
```

```text
[doctest] test cases:     294 |     294 passed | 0 failed | 0 skipped
[doctest] assertions: 3961638 | 3961638 passed | 0 failed |
[doctest] Status: SUCCESS!
==> Done.
```

Extension-load verification:

```text
./build.sh --verify
```

```text
==> Verifying extension loads and native types resolve (headless)...
    OK: no 'Could not find type' parse errors reported by headless Godot.
==> Done.
```

## Implementation

- Added `BeautyCompositor` at `EFFECT_CALLBACK_TYPE_POST_OPAQUE`, with world lookup/guard parity with `RaymarchCompositor`, normal-roughness request/probe, contact-shadow invocation, and final history downsample.
- Added the 160-byte float-indexed `CameraUbo` block and reconstruction GLSL helpers.
- Added `ContactShadowPass`: half-resolution R8 mask, 12-step/0.6 m/0.85 strength defaults from `BeautySettings`, 1.5 m metric tolerance, reverse-Z-safe world-space march, barrier-separated march/apply dispatches, and record-time statistics.
- Added the half-resolution linear-sampled `downsample.comp.glsl` history writer owned by `VoxelWorld`.
- Moved `bayer4` outside `MATERIAL_LAYERS` so screen-space shaders can use it.
- Added the synthetic local-device probe and compositor statistics hook.
- Registered `BeautyCompositor` and added it after the pre-opaque effect in `demo/main.tscn`.
- SSR and outlines remain intentionally unimplemented for Tasks 10–12.

## Files

Created:

- `extension/src/beauty_compositor.h`
- `extension/src/beauty_compositor.cpp`
- `extension/src/render/beauty_camera.h`
- `extension/src/render/beauty_camera.cpp`
- `extension/src/render/contact_shadow_pass.h`
- `extension/src/render/contact_shadow_pass.cpp`
- `shaders/beauty_camera.glslh`
- `shaders/contact_shadow.comp.glsl`
- `shaders/downsample.comp.glsl`
- `tests/test_contact_shadow.gd`
- `.superpowers/sdd/task-9-report.md`

Modified:

- `extension/src/register_types.cpp`
- `extension/src/voxel_world.h`
- `extension/src/voxel_world.cpp`
- `shaders/common.glslh`
- `demo/main.tscn`

## Normal-roughness spike Errata

The live Godot 4.7.1 Forward+ probe reported:

```text
{ "normal_roughness": 1, "contact_ms": 0.005..., "ssr_ms": 0.0, "outline_ms": 0.0 }
```

Verdict: `forward_clustered/normal_roughness` is reachable from the GDExtension when
`set_needs_normal_roughness(true)` is enabled, but the probe currently reports a
constant/empty value of `1.0`. The channel encoding was not calibrated against a
known-orientation object; Tasks 10 and 12 must keep calibration pending and must not rely
on this texture as meaningful dynamic normal/roughness data until that verification is done.

## Demo verification

The required bounded command was attempted from this worktree:

```text
timeout 15s godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification demo/main.tscn
```

Exact result: exit status `1` before the timeout. Godot 4.7.1 reported `X11 Display is
not available`, then `Can't connect to a Wayland display`, `Could not initialize the
Wayland thread`, and `Unable to create DisplayServer, all display drivers failed`.
The environment prevented display startup and visual inspection, so no visual acceptance
evidence is claimed and no screenshot infrastructure was added.

## Review fixes

- Corrected the report metadata to the actual Task 9 commit:
  `2e25e23b3dd68bcbf7551fc489f4f2150481b294`.
- Added the numbered normal-roughness verdict to the plan Errata.
- Recorded the exact required demo attempt and its display initialization failure; visual
  acceptance remains unverified because the environment has no usable display.

### Covering checks

- `./gdunit_tests.sh -a res://tests/test_contact_shadow.gd -a res://tests/test_beauty_compositor.gd -c`
  — exit `0`; `5 test cases | 0 errors | 0 failures | ... | PASSED`. The runner also
  reported that `res://tests/test_beauty_compositor.gd` does not exist in this worktree.
- `cd extension && scons test` — exit `0`; `294 passed | 0 failed`, status `SUCCESS`.
- `./build.sh -j$(nproc)` — exit `0`; `Build OK: 4.4M` and `Done` (targets were up to date).

## Self-review

- `git diff --check` and cached diff checks were clean before commit.
- Push constants are 16–112 bytes and all fields are written by float/int index, never byte-offset indexing.
- Camera UBO updates and texture/resource setup happen before compute-list recording; the contact pass uses one list with an explicit barrier between modes.
- Contact-disabled and zero-step paths return before dispatch and preserve the source image.
- Resource teardown frees cached uniform sets before owned textures/G-buffer resources.
- Reverse-Z assumptions are explicit; no new depth-writing pipeline was introduced, and existing depth writers remain `GREATER_OR_EQUAL`.

## Concerns

- The interactive demo was time-limited rather than manually inspected frame-by-frame, so the visual “hovering object” assessment remains manual.
- Normal-roughness existence is verified (`1`), but channel encoding calibration is deliberately deferred to the consumers in Tasks 10 and 12.
