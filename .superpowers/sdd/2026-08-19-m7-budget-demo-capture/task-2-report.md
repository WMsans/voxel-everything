# Task 2 report — M7 budget demo capture

Status: DONE_WITH_CONCERNS

Date: 2026-08-19

Prior implementation commit SHA: 7936f75

Files changed:

- `extension/src/shade/beauty_settings.h`
- `extension/src/shade/beauty_settings.cpp`
- `extension/tests/test_beauty_settings.cpp`
- `shaders/shade.glslh`
- `shaders/raymarch.comp.glsl`
- `extension/src/render/raymarch_pass.h`
- `extension/src/render/raymarch_pass.cpp`
- `extension/src/voxel_world.h`
- `extension/src/voxel_world.cpp`
- `tests/test_raymarch_cost.gd`
- `tests/test_benchmark.gd`
- `demo/benchmark.gd`
- `.superpowers/sdd/2026-08-19-m7-budget-demo-capture/task-2-report.md`

What changed:

- Added `ve::kFlagCostView = 128u`, `BeautySettings.cost_view`, `pack_beauty_flags()`, and kept `pack_flags()` as the compatibility wrapper for existing callers.
- Added `"cost_view"` to the beauty effect name table and `debug_beauty_settings()`.
- Mirrored the flag in `shaders/shade.glslh` as `BEAUTY_COST_VIEW`.
- Added shader-side march cost instrumentation: primary step count, brick-cell counter, cost heat albedo override, and a per-pixel cost buffer write.
- Added `RaymarchPass` cost buffer allocation/binding/accessor at binding 23.
- Added `VoxelWorld::debug_raymarch_cost_probe(Vector3 pos, Vector3 fwd) -> Dictionary`.
- Factored the 1x1 raymarch render path into `render_probe_pixel()` so the cost probe and existing debug raymarch probes share one render path.
- Added `--effects-off=` parsing to `demo/benchmark.gd` and applied it via `_world.set_effect_enabled(name, false)`.

TDD evidence

Baseline before edits:

1. `cd extension && scons test`
   - PASS: `294/294` doctest cases, `3961638` assertions.
2. `./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd`
   - PASS: `5/5` test cases.

Native red:

1. Added the brief’s native test to `extension/tests/test_beauty_settings.cpp`.
2. `cd extension && scons test`
   - FAIL as expected.
   - Compile errors included:
     - `ve::pack_beauty_flags` not found
     - `ve::kFlagCostView` not found
     - `ve::BeautySettings` has no member `cost_view`
     - the verbatim initializer-list loop also required `#include <initializer_list>`

Native green:

1. Added the cost-view flag/toggle/packing support and shader flag constant.
2. `cd extension && scons test`
   - PASS: `295/295` doctest cases, `3961643` assertions.

GDScript red:

1. Added `tests/test_raymarch_cost.gd` from the brief.
2. `./gdunit_tests.sh -c -a res://tests/test_raymarch_cost.gd`
   - FAIL as expected.
   - Runtime errors:
     - `Invalid call. Nonexistent function 'debug_raymarch_cost_probe' in base 'VoxelWorld'.`

GDScript green, first attempt and fix:

1. After wiring the new probe/cost buffer:
   - `./build.sh -j$(nproc) && ./gdunit_tests.sh -c -a res://tests/test_raymarch_cost.gd && ./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd`
   - First result: BUILD OK, but `test_ground_ray_costs_more_than_sky_ray` failed because the downward probe reported `hit=false` and `steps=0`.
2. Root cause:
   - the new shared probe render path was using the real 1x1 dispatch, but unlike the older raymarch suite harness it was not settling streaming/residency around the probe origin first.
3. Fix:
   - settled residency inside `render_probe_pixel()` via `debug_stream_frame(origin)` before dispatch.
4. Re-ran:
   - `./build.sh -j$(nproc) && ./gdunit_tests.sh -c -a res://tests/test_raymarch_cost.gd && ./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd`
   - PASS:
     - `test_raymarch_cost.gd`: `2/2`
     - `test_raymarch_pixel.gd`: `5/5`

Benchmark parser TDD:

1. Added a parser test in `tests/test_benchmark.gd`.
2. `./gdunit_tests.sh -c -a res://tests/test_benchmark.gd`
   - FAIL as expected:
     - `_effects_off_from_args` did not exist.
3. Added `_effects_off_from_args()` and applied it in `_ready()`.
4. Re-ran:
   - `./gdunit_tests.sh -c -a res://tests/test_benchmark.gd`
   - PASS: `2/2`

Fresh verification before commit

1. `cd extension && scons test`
   - PASS: `295/295` doctest cases, `3961643` assertions.
2. `./gdunit_tests.sh -c -a res://tests/test_benchmark.gd`
   - PASS: `2/2`
3. `./gdunit_tests.sh -c -a res://tests/test_beauty_settings.gd`
   - PASS: `4/4`
4. `./gdunit_tests.sh -c -a res://tests/test_raymarch_cost.gd`
   - PASS: `2/2`
5. `./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd`
   - PASS: `5/5`

Benchmark evidence / blockers

Exact brief command:

1. `/usr/bin/godot --path . --display-driver x11 --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark`
   - BLOCKED in this shell environment.
   - Exact failure:
     - `ERROR: X11 Display is not available`
     - then attempted fallback:
     - `ERROR: Can't connect to a Wayland display.`
     - `ERROR: Unable to create DisplayServer, all display drivers failed.`

Feasible GPU-backed substitute in this environment:

1. `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark | tee /tmp/m7-attr-full-wayland.txt`
   - PASS, with environment qualifier:
     - `WARNING: The requested V-Sync mode Disabled is not available. Falling back to V-Sync mode Enabled.`
     - `BENCH timing_condition display_driver=Wayland vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`
   - Key output:
     - `BENCH frame_avg_ms=17.31 fps=57.8`
     - `BENCH p50=16.67 p95=20.44 p99=23.74 max=28.47`
     - `BENCH gpu_raymarch samples=287 p50_ms=6.324 p99_ms=7.900`
     - `BENCH budget_verdict raymarch=WARN ... frame=WARN`
2. `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark --effects-off=raymarched_sun_shadow | tee /tmp/m7-attr-no-ray-shadow-wayland.txt`
   - PASS, same environment qualifier.
   - Key output:
     - `BENCH frame_avg_ms=16.22 fps=61.6`
     - `BENCH p50=16.67 p95=20.72 p99=24.57 max=31.12`
     - `BENCH gpu_raymarch samples=287 p50_ms=5.722 p99_ms=7.154`
     - `BENCH budget_verdict raymarch=WARN ... frame=WARN`

Attribution takeaway:

- The feasible A/B run supports the **shadow rays** hypothesis: disabling `raymarched_sun_shadow` reduced `gpu_raymarch` from `p50 6.324 / p99 7.900` to `p50 5.722 / p99 7.154`.

Concerns / gaps

1. I could not capture the brief’s interactive `F3` + `F12` cost-view screenshot from this shell-only environment.
2. The exact X11 benchmark command is not runnable here because no X11 display is available.
3. I did not produce a meaningful “with and without islands” benchmark pair in Task 2 scope; the new `--effects-off=` hook currently routes through `VoxelWorld.set_effect_enabled()`, which only covers the beauty effect names available on this branch.
4. Godot still reports `WARNING: 2 ObjectDB instances were leaked at exit` at the end of the gdUnit runs; that warning predates this task’s logic and did not cause test failures.

---

## Round 1 fix report

Status: FIXED_WITH_CAPTURE_LIMITATION

Changes:

- `shaders/raymarch.comp.glsl`: changed cost view to an explicit one-step-per-heat-unit grayscale mapping, clamped to 512 units, with black at 0 and white at 512. The final output bypasses AO/sun lighting while the cost flag is enabled, preserving endpoint semantics; the debug material branch follows the same override.
- `demo/benchmark.gd`: `--effects-off=` now accumulates comma-separated names across repeated options.
- `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`: added a real atomic islands render switch. `set_effect_enabled("islands", false)` gates the reported island slot count to zero, which skips island culling and raymarching while leaving island physics active; getter/debug settings report the state.
- `demo/debug_menu.gd`: exposed the islands switch on the existing effect/debug surface.
- `tests/test_benchmark.gd`, `tests/test_debug_menu.gd`: added regression coverage for repeated options and the real islands toggle.
- `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md`: filled Errata entry 2 with fresh A/B data, corrected stale `frame_verdict_qualified` text to the emitted `verdict_qualified` key, and documented screenshot limits.

Fresh verification commands and outputs:

1. `./build.sh -j$(nproc)` — `Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so`.
2. `cd extension && scons test` — `295/295` doctest cases and `3961643/3961643` assertions passed.
3. `./gdunit_tests.sh -c -a res://tests/test_raymarch_cost.gd` — `2/2` passed, exit 0; `./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd` — `5/5` passed, exit 0. These compiled/executed the amended shader.
4. `./gdunit_tests.sh -c -a res://tests/test_benchmark.gd && ./gdunit_tests.sh -c -a res://tests/test_debug_menu.gd` — benchmark `3/3`, debug menu `3/3` passed.

Benchmark commands (all emitted `verdict_qualified=false` because Wayland kept V-Sync enabled):

- `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark`: `BENCH gpu_raymarch samples=287 p50_ms=6.319 p99_ms=7.896`; `BENCH islands=0`.
- `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark --effects-off=raymarched_sun_shadow`: `p50_ms=5.729 p99_ms=7.106`.
- `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark --effects-off=islands`: `p50_ms=6.324 p99_ms=7.982`; `BENCH islands=0` (null no-live-island control).
- `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark --effects-off=raymarched_sun_shadow,islands`: `p50_ms=5.727 p99_ms=7.143`.
- `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-island`: `BENCH gpu_raymarch samples=807 p50_ms=6.665 p99_ms=8.494`; `BENCH islands=2`.
- `WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-island --effects-off=islands`: `p50_ms=6.544 p99_ms=8.087`; `BENCH islands=2`, confirming the control is applied to rendering rather than silently suppressing island simulation.

The steady no-ray-shadow delta (−0.590 ms p50, −0.790 ms p99) supports the **shadow rays** hypothesis. The steady islands pair is not interpreted as an island-cost result because no island was live; the island-leg pair is the available with/without-islands measurement.

Screenshot evidence: the exact X11 command from Step 10 failed with `ERROR: X11 Display is not available`, followed by no usable Wayland fallback. The attempted alternative was
`WAYLAND_DISPLAY=wayland-1 /usr/bin/godot --path . --display-driver wayland --resolution 2560x1440 --disable-vsync demo/main.tscn` plus `WAYLAND_DISPLAY=wayland-1 grim /tmp/m7-task2-cost-view.png`, which returned `grim_exit=0 file=167656 bytes`. No F3 input could be injected, and this branch has no `KEY_F3` handler, so the captured PNG is explicitly ordinary-frame evidence, not a cost-view screenshot.

Report/output correction: the prior report’s recorded `f1be892` SHA did not match the worktree’s actual prior implementation commit (`7936f75`); the header above is corrected. The fresh benchmark values above supersede the earlier stale `6.324/7.900` and `5.722/7.154` figures; the plan’s emitted timing key is also corrected.

Remaining concerns: interactive cost-view screenshot capture is unavailable in this environment; gdUnit continues to emit the pre-existing `WARNING: 2 ObjectDB instances were leaked at exit`. One chained cost-plus-pixel invocation timed out after printing cost `2/2`; both suites passed with exit 0 when rerun independently.
