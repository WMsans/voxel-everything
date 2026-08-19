# Task 2 report — M7 budget demo capture

Status: DONE_WITH_CONCERNS

Date: 2026-08-19

Implementation commit SHA before report self-reference amend: f1be892

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
