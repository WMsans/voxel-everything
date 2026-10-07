# Final whole-branch review — fix wave report

Branch: `feat/world-types`, base for this wave `0da57c6`.
Worktree: `/Users/jeremyzhao/Development/godot/voxel-everything/.worktrees/world-types`.

Fixes the final-review findings. Finding 1 (required) is the shipped Mesas world type
rendering error magenta; Findings 2–4 are the recommended minor/hardening items.

## Commits

| SHA | Subject |
|---|---|
| `49c011a` | `fix: give the lifted mesas flanks a material so they stop shading magenta` |
| `b84c6ab` | `chore: require explicit leaf_layout field offsets` |
| this commit | `docs: correct stale world-types paths and record the shipped mechanisms` |

## Finding 1 (Important) — Mesas rendered error magenta — FIXED (TDD)

Root cause: `assets/pipelines/mesas.pipeline` runs `height_bands` *before* `mesas`.
`stage_height_bands` writes `material = 0` on every voxel it still reads as air; `stage_mesas`
then lifts those voxels (`ctx.sdf -= P.mesas_lift * mask`) but only assigned `MAT_ROCK` when
`mask > P.mesas_plateau` (0.5), so the lifted flanks (`0 < mask <= 0.5`) kept id 0, which
`shaders/common.glslh` shades magenta.

Test added to `extension/tests/test_pipeline_field_generator.cpp`:
`every solid sample of the mesas pipeline has a material` — loads the shipped
`mesas.pipeline` through `shipped_reader`, samples a 48×48 grid at 59 m pitch over
y = 20…130 m in 1 m steps, and requires every `sdf <= 0` sample to carry `material != 0`.

### RED (test written first, source unfixed)

`cd extension && scons -Q test`

```
tests/test_pipeline_field_generator.cpp:151:
TEST CASE:  every solid sample of the mesas pipeline has a material
[doctest] test cases:      807 |      806 passed |    1 failed | 0 skipped
[doctest] assertions: 11625975 | 11623996 passed | 1979 failed |
[doctest] Status: FAILURE!
```

All 1979 failures are `ERROR: CHECK( s.material != 0 ) is NOT correct!` at
`tests/test_pipeline_field_generator.cpp:165`. Output was usable (bounded at 1979 lines), so
no checks were removed.

### Fix (both mirrors, no `sdf` touched)

- `shaders/stages/mesas.field.glslh`, `stage_mesas`:
  ```glsl
	else if (ctx.sdf <= 0.0 && ctx.material == 0u) ctx.material = MAT_ROCK;
  ```
- `extension/src/terrain/builtin_stages.cpp`, `stage_mesas` (mirror):
  ```cpp
	else if (ctx.f(s.sdf) <= 0.0f && ctx.f(s.material) == 0.0f)
		ctx.f(s.material) = float(kBandRock);
  ```

The `material == 0` guard means a voxel already banded by `height_bands` is never
re-materialed, and no `sdf` is written by the new branch, so no field, bound or golden moves.
`height_bands`, the pipeline stage order, and every golden are unchanged.

### GREEN

`cd extension && scons -Q test`

```
[doctest] test cases:      807 |      807 passed | 0 failed | 0 skipped
[doctest] assertions: 11625975 | 11625975 passed | 0 failed |
[doctest] Status: SUCCESS!
```

### GPU/CPU field agreement

`./gdunit_tests.sh -a res://tests/test_field_diff.gd`

```
res://tests/test_field_diff.gd > test_every_pipeline_agrees_between_cpu_and_gpu PASSED 5s 543ms
res://tests/test_field_diff.gd > test_a_far_seeded_world_agrees_between_cpu_and_gpu PASSED 558ms
Statistics: 2 test cases | 0 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Exit code: 0
```

The field diff picks up `assets/pipelines/mesas.pipeline`, so this run also proves the GLSL
stage and the C++ mirror still agree after the change.

### Goldens / generated

```
$ git status --short shaders/generated tests/golden
(empty)
```

### Visual confirmation (Mesas no longer magenta)

```
$ godot --path . --resolution 1280x720 -s res://tools/world_type_capture.gd \
    -- --out=$TMPDIR/world-types-fix --type=Mesas --seed=0
WORLD_TYPE_CAPTURE saved .../world-types-fix/title.png
WORLD_TYPE_CAPTURE saved .../world-types-fix/create_world.png
WORLD_TYPE_CAPTURE saved .../world-types-fix/world_mesas.png
WORLD_TYPE_CAPTURE Mesas seed 0 player (8.0, 58.55438, 8.0)
EXIT=0
```

Read `$TMPDIR/world-types-fix/world_mesas.png`:

- HUD first line `seed 0 · Mesas`; the capture settled (spawn `(8.0, 58.55438, 8.0)` matches
  the §10 item 12 record for seed 0), so this is the real Mesas world, not a fallback.
- Lifted mesa plateaus and their flanks fill the mid and far terrain, all shaded grey rock
  with soft banding and normal shading. A green grass patch with grass blades sits on the
  lower-left slope; blue sky above.
- **No magenta anywhere in the frame.** The mid/far flanks that §10 item 12 recorded as
  error magenta are now rock-coloured.

Full extension build after the change (catches the `leaf_scatter_pass.cpp` call site below):
`cd extension && scons -Q -j$(sysctl -n hw.ncpu)` → all sources compiled,
`Linking Shared Library bin/libvoxel_everything.macos.template_debug.universal.dylib ...`, no
errors.

## Finding 2 (Minor) — stale `demo/main.tscn` paths in PORTFOLIO.md — FIXED

```
$ grep -n "demo/main.tscn" docs/PORTFOLIO.md
(no output)
$ grep -n "demo/scenes/main.tscn" docs/PORTFOLIO.md
86:| Near-field scale | `demo/scenes/main.tscn` `VoxelWorld.near_field_scale` | 0.40 |
190:godot --path . demo/scenes/main.tscn
```

Only the two live-doc occurrences changed; `docs/superpowers/plans/*` untouched.

## Finding 3 (Minor, hardening) — `leaf_layout` offset defaults removed — FIXED

`extension/src/leaves/leaf_layout.h`:

```cpp
 LeafLayout leaf_layout(const LeafSettings &settings, const float camera[3],
-		const float view_proj[16], float field_offset_x = 0.0f, float field_offset_z = 0.0f);
+		const float view_proj[16], float field_offset_x, float field_offset_z);
```

`$ grep -n "float field_offset" extension/src/leaves/leaf_layout.h`
→ `81:		const float view_proj[16], float field_offset_x, float field_offset_z);`

The definition in `leaf_layout.cpp` is unchanged. Every call site now states its offsets:

- `extension/src/render/frame.cpp:114` — already passed `tp.field_offset_x/z` (the shipping
  call), unchanged.
- `extension/src/render/leaf_scatter_pass.cpp:51` (the warm-up capacity layout) — passes
  `0.0f, 0.0f`.
- `extension/tests/test_leaf_layout.cpp` — 9 call sites intending the unshifted box now pass
  `0.0f, 0.0f` (the one offset test, line 305, already passed `1234.0f, -4321.0f`).

`grep -rn "leaf_layout(" extension/src extension/tests` shows no remaining caller with three
arguments; omitting the offsets is now a compile error. Native suite re-run after:
`Status: SUCCESS!` (counts above). Full extension build links clean.

## Finding 4 (Minor, doc) — spec mechanism text — FIXED

`docs/superpowers/specs/2026-10-07-title-menu-world-types-design.md`:

- §9 "Back to title" bullet now ends: *"The single-process teardown → re-create evidence is
  recorded in §10 item 12."*
- §10 item 13 records the §4.1 erratum: the spec claims
  `resource_local_to_scene = true` on the Compositor/effects, but the shipped mechanism is
  `duplicate()` + a fresh `Compositor` in `VoxelWorldScene._enter_tree`
  (`demo/scripts/voxel_world_scene.gd`) — functionally equivalent, pinned by
  `tests/test_world_scene.gd` (`test_two_instances_never_share_a_compositor`,
  `test_compositors_follow_the_instance_wherever_it_is_placed`).
- §10 item 14 records the §4.2 erratum: the spec claims `_enter_tree` sets `sun_light_path`,
  but `_enter_tree` sets only `world_path`; `sun_light_path` is serialized in
  `demo/scenes/voxel_world.tscn` (`sun_light_path = NodePath("Sun")`), pinned by
  `tests/test_world_scene.gd` (`test_sun_and_settings_resolve_inside_the_instance`).

No other spec text rewritten. Verified both claims against the shipped files before writing.

## Concerns

- None blocking. Finding 1's fix matches the reviewer's sketch exactly; the only judgement
  call was the `material == 0` guard, which keeps the change `sdf`-neutral and cannot
  re-material a voxel `height_bands` already banded.
