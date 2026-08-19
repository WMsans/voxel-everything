# M7 Task 3 report — per-brick flag word

## Status

Implemented the pure flag-word core, conservative atlas initialization/reset, mark/gen publication, binding 21/22 wiring while preserving Task 2 binding 23, native/GPU diagnostics, and the required benchmark decision. The marcher change was reverted because the measured p50 improvement was below the brief's 3% threshold on every leg; the flag buffer and bindings remain for Task 4.

Commit: `363ed11` (`perf: add conservative per-brick flag words`).

## TDD evidence

### RED

1. Added `extension/tests/test_brick_flags.cpp` from the brief. The first local include spelling was corrected from `"doctest.h"` to the repository convention `<doctest/doctest.h>` because SCons exposes `extension/third_party`, not `extension/third_party/doctest`.
2. `cd extension && scons test` then failed as expected with:

```text
g++ ... tests/test_brick_flags.cpp
fatal error: world/brick_flags.h: No such file or directory
scons: *** [tests/test_brick_flags.o] Error 1
```

3. Added the requested GPU test. Before the hooks existed, `./gdunit_tests.sh -c -a res://tests/test_brick_flags_gpu.gd` failed with missing debug API errors (`debug_stream_region` and `debug_brick_flags_after_mark`).

### GREEN

After adding the pure core:

```text
cd extension && scons test
[doctest] test cases:     299 |     299 passed | 0 failed
[doctest] assertions: 3961657 | 3961657 passed | 0 failed
[doctest] Status: SUCCESS!
```

After adding the GPU implementation and hooks:

```text
./gdunit_tests.sh -c -a res://tests/test_brick_flags_gpu.gd
2 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans
Exit code: 0
```

The final post-revert run reproduced the same GPU result.

## Verification commands and outputs

- `git diff --check`: passed.
- `./build.sh -j$(nproc)`: passed; produced `4.6M libvoxel_everything.linux.template_debug.x86_64.so` and registered `VoxelWorld, RaymarchCompositor`.
- `cd extension && scons test`: passed; 299/299 test cases and 3,961,657/3,961,657 assertions.
- `./gdunit_tests.sh -c -a res://tests/test_brick_flags_gpu.gd`: passed, 2/2.
- `./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd`: passed, 6/6.
- `./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd`: passed, 5/5.
- `./gdunit_tests.sh -c -a res://tests/test_raymarch_mips.gd`: passed, 2/2.
- `./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd`: failed in the pre-existing sphere-add case: 8 cases, 1 error, 2 failures. The failure is `after` missing `pos` after the material-4 sphere add. The same failure reproduced in a control run with the old eight-texel marcher gate restored, so it is not caused by the Task 3 gate change.
- `tools/run_benchmarks.sh m7-task3` initially failed because no display variable was exported; reran with `WAYLAND_DISPLAY=wayland-1` and completed all five legs on Vulkan. Wayland reported V-Sync enabled despite the requested disabled mode, so the timing verdict is qualified by the environment.

## Benchmark / Errata entry 3

Compared with the recorded Task 1 baseline in `reports/m7-baseline`, pre-revert Task 3 `gpu_raymarch` p50/p99 was:

| Leg | Task 1 baseline p50/p99 ms | Task 3 measured p50/p99 ms | p50 delta |
|---|---:|---:|---:|
| steady | 6.294 / 7.910 | 6.307 / 7.935 | +0.21% |
| move | 6.354 / 7.571 | 6.379 / 7.865 | +0.39% |
| ridge | 6.016 / 8.724 | 5.947 / 8.296 | -1.15% |
| edit | 10.533 / 14.263 | 10.815 / 14.365 | +2.68% |
| island | 6.666 / 8.475 | 6.746 / 8.516 | +1.20% |

All five p50 deltas were under 3%. Per the brief, the marcher change was reverted. The flag buffer remains allocated, conservatively initialized/reset, written by mark/gen, and bound at raymarch binding 21; binding 22 remains region slot counts and Task 2 cost binding 23 is preserved.

Representative benchmark output:

```text
steady  BENCH gpu_raymarch samples=287 p50_ms=6.307 p99_ms=7.935
move    BENCH gpu_raymarch samples=287 p50_ms=6.379 p99_ms=7.865
ridge   BENCH gpu_raymarch samples=287 p50_ms=5.947 p99_ms=8.296
edit    BENCH gpu_raymarch samples=287 p50_ms=10.815 p99_ms=14.365
island  BENCH gpu_raymarch samples=807 p50_ms=6.746 p99_ms=8.516
```

## Files touched

- Created `extension/src/world/brick_flags.h` and `.cpp` with the pure `namespace ve` interface/implementation.
- Created `extension/tests/test_brick_flags.cpp`.
- Created `tests/test_brick_flags_gpu.gd`.
- Modified `extension/src/render/gpu_atlas.h/.cpp` for the one-word buffer and conservative reset on clear.
- Modified `extension/src/render/region_pass.cpp` and `brick_gen_pass.cpp` to bind the new storage buffer at binding 10.
- Modified `shaders/brick_mark.comp.glsl`, `brick_gen.comp.glsl`, and `common.glslh` for conservative/publication behavior and mirrored constants.
- Modified `extension/src/render/raymarch_pass.cpp` and `shaders/raymarch.comp.glsl` for binding 21 flags, binding 22 region slot counts, and preserved binding 23 cost buffer.
- Modified `extension/src/voxel_world.h/.cpp` for the fail-soft flag comparison and post-mark diagnostics, plus the region streaming test helper.

## Self-review

- Pure flag code contains no Godot types.
- Constants are exactly `1u`, `2u`, and `3u`; CPU reduction uses the 2^3 mip level and the palette slot-0 material rule.
- Newly allocated and force-regenerated slots are conservative before generation; clear resets every flag word conservatively.
- Generator publication occurs after the shared mip reduction and palette phase.
- GLSL remains `#version 460`; the existing reverse-Z, reserved-word errata, and shader-loader conventions were preserved.
- Task 2 binding 23 cost buffer and cost-view plumbing were not removed or repurposed.
- No plan file was read or modified.

## Concerns

1. `test_edit_pipeline.gd` remains red in the existing material-4 sphere-add scenario; the control with the old marcher gate reproduced it, so it is recorded rather than changed in this task.
2. Benchmark timing is qualified because this environment's Wayland backend enables V-Sync despite `--disable-vsync`; the required five-leg run completed and the measured deltas triggered the specified null-result revert.
3. Godot test runs emit the environment's existing X11 fallback, GTK theme, and ObjectDB leak warnings; all targeted GPU suites otherwise completed with the reported statuses.

## Round 1 fix report

### Finding 1 — SSBO publication ordering

Root cause: the palette SSBO writes in `shaders/brick_gen.comp.glsl` were followed only by
`memoryBarrierShared()` before the workgroup `barrier()`. Shared-memory fencing does not make
SSBO writes visible. The fix adds `memoryBarrierBuffer()` immediately before that existing
rendezvous. The mark/gen dispatch remains ordered by the renderer's existing
`compute_list_add_barrier()`; no allocation, publication, or raymarch logic was broadened.

TDD-style red check, run before the edit:

```text
python3 - <<'PY'
from pathlib import Path
s = Path('shaders/brick_gen.comp.glsl').read_text()
palette = s.index('palette_buf.id[slot * 4] != 0u')
barrier = s.index('memoryBarrierBuffer()', palette) if 'memoryBarrierBuffer()' in s[palette:] else -1
assert barrier >= 0, 'palette SSBO read is not preceded by an explicit buffer barrier'
PY
```

Output:

```text
AssertionError: palette SSBO read is not preceded by an explicit buffer barrier
```

Focused shader validation after the edit:

```text
glslangValidator -V --target-env vulkan1.2 -S comp /tmp/task3-round1-brick_gen.comp.glsl -o /tmp/task3-round1-brick_gen.spv
/tmp/task3-round1-brick_gen.comp.glsl
spirv-val /tmp/task3-round1-brick_gen.spv
glslang_exit=0 spirv_val_exit=0
PASS: palette SSBO write -> memoryBarrierBuffer -> barrier -> flag publication
```

The first direct `glslangValidator` attempt against the raw source was intentionally not used
as evidence: the repository source contains loader-managed `#include` directives. The passing
validation used the repository's include expansion pattern, then `glslangValidator` and
`spirv-val`; the GPU tests below also compiled the shader through the actual Godot loader.

### Focused verification after the amendment

```text
./build.sh -j$(nproc)
==> Build OK: 4.6M libvoxel_everything.linux.template_debug.x86_64.so
EXIT_STATUS=0

cd extension && scons test
[doctest] test cases:     299 |     299 passed | 0 failed | 0 skipped
[doctest] assertions: 3961657 | 3961657 passed | 0 failed |
[doctest] Status: SUCCESS!
EXIT_STATUS=0

./gdunit_tests.sh -c -a res://tests/test_brick_flags_gpu.gd
2 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Exit code: 0
EXIT_STATUS=0

./gdunit_tests.sh -c -a res://tests/test_brick_diff.gd
6 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Exit code: 0
EXIT_STATUS=0

./gdunit_tests.sh -c -a res://tests/test_raymarch_pixel.gd
5 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Exit code: 0
EXIT_STATUS=0

./gdunit_tests.sh -c -a res://tests/test_raymarch_mips.gd
2 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Exit code: 0
EXIT_STATUS=0

./gdunit_tests.sh -c -a res://tests/test_edit_pipeline.gd
8 test cases | 1 errors | 2 failures | 0 flaky | 0 skipped | 0 orphans
Invalid access to property or key 'pos' on a base object of type 'Dictionary'.
 at test_sphere_add_places_material_4_in_open_sky in res://tests/test_edit_pipeline.gd:92
Exit code: 100
EXIT_STATUS=100
```

The edit-pipeline failure is unchanged in shape and scope: the same material-4 sphere-add case
fails while subtract, paint, border, capacity, and hostile-input cases pass. It remains the
pre-existing failure identified in the original report; no edit-pipeline code was changed.

### Finding 2 — committed Errata evidence

Committed `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md` Errata entry 3 now
contains the exact Task 1 baseline and pre-revert Task 3 `gpu_raymarch` p50/p99 values for all
five legs, the computed p50 deltas, the Wayland V-Sync fallback warning and emitted timing line,
and each leg's unaltered `budget_verdict`. Every p50 delta is under 3%:

```text
steady +0.21%   move +0.39%   ridge -1.15%   edit +2.68%   island +1.20%
```

The committed decision is the required null result: revert the marcher gate, retain the
conservative flag buffer/bindings for Task 4, and claim no Task 3 performance PASS. The source
benchmark lines contain `raymarch=WARN` and `frame=WARN`; only process exit status was zero.
