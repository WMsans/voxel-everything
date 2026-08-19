# Task 11 Report — Half-resolution SSR and High-tier glossy SDF reflections

## Status

DONE. Base implementation was committed as `5508743` (`feat: screen-space and glossy sdf reflections`).

Erratum fix committed with `fix: skip uncalibrated dynamic ssr receivers`.

## RED evidence

Test-first file: `tests/test_ssr.gd`.

Initial command:

```text
./gdunit_tests.sh -a res://tests/test_ssr.gd -c
```

The first attempt exposed strict GDScript inference errors because the supplied brief used untyped local Dictionary inference. I added explicit `Dictionary` annotations, then reran the test. The corrected RED run failed for the intended missing production interfaces:

```text
Invalid call. Nonexistent function 'debug_ssr_probe' in base 'VoxelWorld'.
Invalid call. Nonexistent function 'debug_glossy_sdf_probe' in base 'VoxelWorld'.
Statistics: 6 test cases | 6 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Overall Summary: 6 test cases | 6 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
```

## GREEN implementation

### SSR

- Added `shaders/ssr.comp.glsl` with trace and `SSR_APPLY` variants.
- Trace reads post-opaque scene color/depth, voxel surface/depth, optional normal-roughness, and `CameraUbo`.
- Apply is a separate dispatch/list and preserves scene alpha.
- Added `SsrPass` in `extension/src/render/ssr_pass.{h,cpp}`.
- Half target uses `max(1, full / 2)` dimensions and `R16G16B16A16_SFLOAT` storage+sampling.
- Exact 32-byte trace push block and 16-byte apply push block are used.
- Uniform sets are rebuilt for RID/size changes; sampler and neutral fallback resources are allocated.
- Disabled/zero-step SSR returns before allocation/dispatch. Trace/apply failures leave scene color untouched.
- Integrated after contact shadows and before the Task 12 outline insertion point.
- `ssr_ms` reports CPU command-record time only.

### Glossy SDF reflection

- Parameterized the existing terrain/island marchers with shared step budgets.
- Primary terrain budget is 65536; each primary island receives 192; reflected terrain/islands share 64.
- Added the fixed 20m/64-step/high-gloss constants and the required albedo-space blend.
- Branch is gated by `BEAUTY_GLOSSY_RAYS` and `gloss > 0.5`.
- Counted reflection normal-gradient SDF evaluations in the shared secondary budget.
- No G-buffer attachment, texture, or RaymarchPass public interface was added.

### Probes and bindings

- Added and bound `VoxelWorld::debug_ssr_probe(int fixture, int w, int h)`.
- Added and bound `VoxelWorld::debug_glossy_sdf_probe(Vector3 origin, Vector3 dir)`.
- SSR fixture renders a real receiver into the voxel G-buffer, then uses separate post-opaque scene color/depth resources; fixture 0 adds the center-quarter blocker and fixture 1 omits it while retaining the receiver.
- Probe cleanup tears down/reinitializes the SSR pass before freeing temporary scene resources, avoiding cached-set RID lifetime errors.
- Glossy probe uses `params.w = -1.0` and compares medium/high while verifying only albedo changes.

## GREEN verification

### Build

Command:

```text
./build.sh -j$(nproc)
```

Exact final result:

```text
==> Build OK: 4.5M libvoxel_everything.linux.template_debug.x86_64.so
    Registered native classes: VoxelWorld, RaymarchCompositor
==> Done.
```

### Focused SSR/glossy suite

Command:

```text
./gdunit_tests.sh -a res://tests/test_ssr.gd -c
```

Exact final result:

```text
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Overall Summary: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

All six tests passed, including half resolution, merged scene-only blocker, fixture removal, medium/off quality controls, alpha/boundedness, and high-only glossy SDF behavior.

### Dependent regression suites

Command:

```text
./gdunit_tests.sh -a res://tests/test_contact_shadow.gd -a res://tests/test_ssgi.gd -a res://tests/test_deferred.gd -a res://tests/test_lod_gbuffer.gd -a res://tests/test_raymarch_gbuffer.gd -c
```

Exact final result:

```text
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Overall Summary: 25 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

### Diff/static checks

```text
git diff --check
```

Result: no output, exit code 0.

The SSR shader variants compile during local Vulkan initialization in the focused suite. The implementation uses the project RD API patterns from `docs/api/renderingdevice.md`, including device operations outside open lists, list boundaries between trace/apply, and explicit submit/sync only in local-device probes.

### Demo command

Command:

```text
godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn
```

Result: unable to start in this environment because both X11 and Wayland display initialization failed:

```text
ERROR: X11 Display is not available
ERROR: Can't connect to a Wayland display.
ERROR: Unable to create DisplayServer
ERROR: Could not initialize the Wayland thread.
exit=1
```

## Self-review

- Confirmed POST_OPAQUE ordering: contact shadows → SSR → existing history downsample; no outline implementation was added.
- Confirmed scene color/depth are the `RenderSceneBuffersRD` post-opaque resources in production.
- Confirmed reverse-Z reconstruction uses `CameraUbo` helpers and hit tests use reconstructed world distances.
- Confirmed trace push constants are 32 bytes and apply push constants are 16 bytes, both under the 128-byte limit.
- Confirmed all effect work is controlled by `BeautySettings`; Medium uses 12 SSR steps and disables glossy rays; Off dispatches nothing.
- Confirmed normal-roughness remains optional and is still fail-soft bound to the neutral fallback, but SSR no longer samples it or derives receiver gloss from it.
- Confirmed no extra G-buffer channel, reserved shader identifier, shader include-in-comment, or Task 12 outline code was introduced.
- Confirmed shader and pass teardown ordering frees uniform sets before pipelines/shaders/textures.
- Independent review initially identified secondary normal-gradient budget accounting and a fixture-1 depth-hole concern; both were corrected and the focused suite rerun green.

## Files

Created:

- `shaders/ssr.comp.glsl`
- `extension/src/render/ssr_pass.h`
- `extension/src/render/ssr_pass.cpp`
- `tests/test_ssr.gd`

Modified:

- `shaders/raymarch.comp.glsl`
- `extension/src/beauty_compositor.cpp`
- `extension/src/voxel_world.h`
- `extension/src/voxel_world.cpp`

## Concerns

- The requested interactive demo could not run because this execution environment has no usable display server. GPU-backed GdUnit tests did run successfully on Vulkan/NVIDIA.
- Godot reports two leaked ObjectDB instances at GdUnit process exit; this also appears in the existing test harness shutdown and did not produce test failures.

## Task 11 normal-roughness erratum fix

The Task 9 erratum says `forward_clustered/normal_roughness` is reachable but currently
constant/uncalibrated `1.0`. The previous SSR fallback treated every G-buffer-missing
pixel as a receiver and derived `gloss = 1.0 - normal_roughness.a`. That behavior is now
removed. `shaders/ssr.comp.glsl` keeps voxel receiver gloss sourced exclusively from
`gb_surface.g`, returns no receiver for dynamic-object pixels, and continues to sample the
post-opaque scene colour/depth for reflection targets. The optional normal-roughness binding,
neutral 1x1 fallback, `have_normal_roughness` push flag, fixed constants, pass ordering,
quality toggles, and fail-soft behavior are unchanged. No outlines were added.

The real GPU SSR probe now has fixtures 2 and 3 with the same dynamic receiver and scene
blocker but normal-roughness alpha 0 and 1. It creates a synthetic dynamic plane with no
G-buffer surface, counts dynamic versus scene reflection hits, and asserts both alpha
variants skip dynamic receivers while scene targets remain reflected. Before the shader
fix, the new GPU test failed with `dynamic_hit_pixels` 2 versus 0, mean delta
`0.00058136414737` versus `0.00046937260777`, and hit count 1115 versus 1095. After the
fix it passes with zero dynamic hits and equal results.

### Fix verification

Build command:

```text
./build.sh -j$(nproc)
```

Exact final output:

```text
==> Build OK: 4.5M libvoxel_everything.linux.template_debug.x86_64.so
    Registered native classes: VoxelWorld, RaymarchCompositor
==> Done.
```

Focused real-GPU command:

```text
./gdunit_tests.sh -a res://tests/test_ssr.gd -c
```

Exact final result:

```text
Statistics: 7 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Overall Summary: 7 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

Deferred/beauty regression command:

```text
./gdunit_tests.sh -a res://tests/test_contact_shadow.gd -a res://tests/test_ssgi.gd -a res://tests/test_deferred.gd -a res://tests/test_lod_gbuffer.gd -a res://tests/test_raymarch_gbuffer.gd -c
```

Exact final result:

```text
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Statistics: 5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Statistics: 3 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Statistics: 6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Overall Summary: 25 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans |
Exit code: 0
```

Extension command:

```text
cd extension && scons test
```

Exact final output:

```text
[doctest] test cases:     294 |     294 passed | 0 failed | 0 skipped
[doctest] assertions: 3961638 | 3961638 passed | 0 failed |
[doctest] Status: SUCCESS!
scons: done building targets.
```

`git diff --check` completed with no output and exit code 0 before commit.
