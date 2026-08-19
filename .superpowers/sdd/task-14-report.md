# Task 14 Report — debug controls, GPU timings, budget verdicts

## Status

`DONE_WITH_CONCERNS`

## RED evidence

Test-first pairing suite was created at `tests/test_gpu_timings.gd` before the production implementation.
After fixing only GDScript type inference in the test, the required focused command failed because the hook was absent:

```text
./gdunit_tests.sh -a res://tests/test_gpu_timings.gd -c
...
SCRIPT ERROR: Invalid call. Nonexistent function 'debug_ingest_gpu_timings' in base 'VoxelWorld'.
...
Statistics: 3 test cases | 3 errors | 0 failures
Exit code: 100
```

## Implementation

Created/modified:

- `extension/src/render/gpu_timings.h/.cpp`: delayed paired timestamp capture, identity parsing, repeated-occurrence summing, microseconds-to-milliseconds conversion (`/1000.0`), `-1.0` missing values, sample de-duplication, and mutex-protected publication.
- `extension/src/raymarch_compositor.cpp`: PRE_OPAQUE frame/pass markers.
- `extension/src/beauty_compositor.cpp`: delayed poll, POST_OPAQUE pass markers, history marker, and frame end marker after the custom POST_OPAQUE work.
- `extension/src/voxel_world.h/.cpp`: GPU timing hooks and ownership; mutex-protected BeautySettings value snapshots; bool history-dispatch result.
- `demo/debug_menu.gd`, `demo/main.tscn`: F1 quality/effect controller in the top-right HUD.
- `demo/hud.gd`: CPU LoD label plus delayed GPU timing line.
- `demo/benchmark.gd`: GPU sample de-duplication, 30-frame drain, fixed p99 budgets, `PASS/WARN/UNMEASURED` verdicts, and explicit CPU LoD source labeling.
- `tests/test_gpu_timings.gd`, `tests/test_debug_menu.gd`.
- `docs/superpowers/plans/2026-08-18-m6-beautification.md`: Task 14 Errata benchmark record.

The global RenderingDevice is never synchronously submitted or synchronized by the new telemetry. `debug_perf_stats()["lod_ms"]` remains CPU command-record time and is not read for GPU verdicts.

## GREEN evidence

Focused verification after implementation:

```text
./build.sh -j$(nproc) && ./gdunit_tests.sh -a res://tests/test_gpu_timings.gd -a res://tests/test_debug_menu.gd -a res://tests/test_beauty_settings.gd -c
Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so
Overall Summary: 9 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
```

Native and full verification:

```text
cd extension && scons test
[doctest] test cases:     294 |     294 passed | 0 failed | 0 skipped
[doctest] assertions: 3961638 | 3961638 passed | 0 failed
[doctest] Status: SUCCESS!

./gdunit_tests.sh -c
Overall Summary: 232 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Executed test suites: (47/47)
Executed test cases : (232/232)
Exit code: 0
```

The complete full-run output was emitted by the runner and retained in the session log (`/tmp/pi-bash-f75e105d98f179a2.log`). The focused run had no signal-connection warnings after moving the OptionButton connection outside its item loop.

## Benchmark verification

All five 2560x1440 High-tier legs printed both required telemetry records. Commands, device details, exact GPU p50/p99 lines, verdict lines, and follow-up are recorded in the plan Errata entry.

Captured stdout files:

- `/tmp/m6-benchmark-steady.txt`
- `/tmp/m6-benchmark-move.txt`
- `/tmp/m6-benchmark-ridge.txt`
- `/tmp/m6-benchmark-edit.txt`
- `/tmp/m6-benchmark-island.txt`

The benchmark command required the detected Wayland environment in this shell (`WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000`); without that environment, the exact bare command could not initialize a display. Device: NVIDIA GeForce RTX 4070 Laptop GPU, driver 610.57.04, Vulkan 1.4.341, Godot 4.7.1.stable.arch_linux.a13da4feb.

## Self-review

- Timestamp marker names are paired by `ve:<serial>:<pass>:<occurrence>:<b|e>`, not by array adjacency.
- Repeated complete occurrences are summed; a bad occurrence is counted in `dropped_pairs` while complete occurrences remain usable, matching the pairing test’s mixed-validity case.
- Missing values remain `-1.0`; benchmark collection ignores them rather than turning them into zero-cost samples.
- GPU publication is mutex-protected because render callbacks and HUD/benchmark reads can occur on different threads.
- `custom_frame_gpu_ms` ends at POST_OPAQUE after history, so the interval includes the engine opaque work between callbacks and stops before tonemap.
- Quality/effect controls call the existing VoxelWorld settings model; they do not create a second settings model.
- `beauty_settings()` returns a locked copy and each compositor takes one frame snapshot.
- `git diff --check` passed before the documentation commit.

## Concerns

1. The five legs all recorded `WARN` for every fixed budget. The reported GPU values are approximately 1000x larger than wall-clock frame timing on this Godot/Vulkan/NVIDIA stack (for example, steady raymarch p50 `6312.416 ms` versus frame p50 `16.67 ms`). I followed the brief and Godot’s documented microsecond API contract exactly (`delta_us / 1000.0`) and did not retune or silently divide by another factor. The plan Errata records this as a follow-up to reconcile the engine/backend timestamp conversion before treating these numeric verdicts as performance claims.
2. The benchmark process printed the required completed leg output but aborted during shutdown with `corrupted size vs. prev_size`; island also printed an invalid RID free warning. This is retained as a benchmark/demo shutdown concern, not converted into a pass.
3. The benchmark environment fell back from requested disabled V-Sync because the active Wayland compositor did not expose that mode.
4. Full gdUnit finished with the pre-existing-looking `2 ObjectDB instances were leaked at exit` warning despite 232/232 passing.

## Commits

- `722ab906a6870f65e9dc48599ca78c9c8dcb99f4` — `feat: beauty controls and per-pass gpu budget telemetry`
- `fe0897f` — `docs: record M6 GPU budget verdicts`

## Task 14 correction-wave root-cause analysis and fixes

Investigation was performed before editing. A fresh 2560x1440 steady leg reproduced the shutdown abort; a physics-disabled diagnostic still aborted, proving the first failure was not island simulation. The island leg separately reproduced `Attempted to free invalid ID: 5892695130146`. Instrumenting teardown and per-pass frees traced that RID to `CompositePass::uset_`: `RaymarchPass::rebuild_targets()` freed/replaced its output textures while CompositePass still held a dependent uniform set and framebuffer. Godot cascaded those resources, leaving a stale RID that CompositePass later freed. The fix is `RaymarchPass::targets_need_rebuild()` plus pre-rebuild `CompositePass::release_targets()` and `invalidate_uniform_set()`. This removed the island invalid-RID diagnostic in fresh 640x360 and 2560x1440 island runs.

The remaining shutdown heap abort was traced to resource/lifetime sequencing rather than output suppression: main-device RIDs were destroyed during SceneTree quit while renderer work and compositor callbacks were still in flight. The correction wave adds a callback lifetime guard, stops callbacks before stale `Engine::get_main_loop()` access, and gives the benchmark a real `shutdown_render_resources()` path that runs teardown on Godot’s render thread, after `RenderingServer::force_sync()`, followed by a five-second renderer observation window before `SceneTree::quit()`. No GPU cleanup was removed and no test budget was weakened. This stabilizes steady/edit and low-resolution island legs, but the fresh high-resolution move/ridge/island legs still show a `corrupted size vs. prev_size` shutdown abort in this Wayland/NVIDIA environment; they remain explicitly unqualified below rather than being claimed clean.

Live timestamp evidence: the Godot 4.7.1 Vulkan/NVIDIA backend returned raw GPU deltas approximately 1000x the paired CPU-marker scale (observed raw/CPU ratios 3,722.530–9,789.078). `GpuTimings::poll()` calibrates live values as nanoseconds and converts to microseconds once with `raw * 0.001`; the common parser still divides documented microseconds by `1000.0` once for milliseconds. `ingest_for_test()` remains unscaled microseconds. The benchmark now publishes `gpu_timestamp_calibration`; the focused synthetic test asserts scale 1.0 and 16.0 ms for a 16,000-us frame.

Marker correction: LoD occurrence 0 now encloses the first actual draw, sun-shadow is built/captured after that occurrence, and LoD occurrence 1 encloses the following draw. Failure paths cancel the pass marker and abort/reset the frame; optional failed passes cancel their active marker; POST_OPAQUE contact/SSR/outlines/history paths do the same; `end_frame()` clears any forgotten active state.

## Fresh verification after correction edits

```text
./build.sh -j$(nproc)
Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so

cd extension && scons test
[doctest] test cases: 294 | 294 passed | 0 failed
[doctest] assertions: 3961638 | 3961638 passed | 0 failed

./gdunit_tests.sh -a res://tests/test_gpu_timings.gd -a res://tests/test_debug_menu.gd -a res://tests/test_beauty_settings.gd -c
Overall Summary: 10 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans

./gdunit_tests.sh -c
Overall Summary: 233 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Executed test suites: (47/47)
Executed test cases : (233/233)
Exit code: 0
```

Required benchmark commands and exact current evidence are in Errata entry 3 of `docs/superpowers/plans/2026-08-18-m6-beautification.md`. Captured stdout files include `/tmp/m6-benchmark-steady.txt` and the per-leg rerun files under `/tmp/m6-final2-*`; the current worktree has not yet been committed, so correction commit: `f888110` (`fix: stabilize beauty timing markers and resource lifetimes`). The environment qualification remains: Wayland prints `The requested V-Sync mode Disabled is not available. Falling back to V-Sync mode Enabled.` Steady and edit exited 0; move, ridge, and island still aborted during shutdown, and are not clean benchmark passes.
