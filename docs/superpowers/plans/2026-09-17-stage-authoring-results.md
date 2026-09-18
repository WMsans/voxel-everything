# Stage authoring (sub-project 6) — results

**Spec:** `docs/superpowers/specs/2026-09-17-stage-authoring-design.md`
**Baseline:** `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md`
**Range:** `b03e5d6..6598d86` (Task 14 documentation is the closing commit)

## Exit criteria

| Criterion | Check | Result |
|---|---|---|
| No positional slot/param access | `rg 'extra\[[0-9]\]\|p\.at\([0-9]\)' extension/src/terrain` | PASS — no matches; rg exit 1 |
| The generator seam is gone | `rg 'class FieldGenerator\|ProceduralFieldGenerator\|AnalyticGenerator' extension/src` | PASS — no matches; rg exit 1 |
| No generator reached through a view | `rg '\-\>sampler\(\)' extension/src` | PASS — no matches; rg exit 1 |
| New terrain stage ≤ 3 files | Task 13 Step 6 | PASS — 3 paths: `shaders/stages/mesas.field.glslh`, `assets/pipelines/mesas.pipeline`, `extension/src/terrain/builtin_stages.cpp` |
| Material with hardness placed by terrain ≤ 4 files | Task 13 Step 6 | PASS — 3 paths using existing `MAT_ROCK` (hardness 3.0); no new material was added |
| Every shipped pipeline passes the sampled bound check | `test_lipschitz_sampled` | OPEN — `default.pipeline` and `golden.pipeline` passed the 4096-point cases; `mesas.pipeline` resolves to 3.29 but is not included in `test_lipschitz_sampled.cpp` |
| Suites match the baseline | Step 2 | FAIL — native is green (674/674), but gdUnit XML suite-declared cases are 508 versus baseline 513. `test_field_diff` is 6→1 from the required Task 2 parameterisation; `test_generator_seam` has a new failure; the `test_sun_cascades_gpu` failure has a different case/message from baseline. |

## Regression evidence

Native clean build:

```text
[doctest] test cases:     674 |     674 passed | 0 failed | 0 skipped
[doctest] assertions: 9127373 | 9127373 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Full gdUnit run: `reports/report_26/results.xml`, 96/96 suites, 505/505 executed cases, exit code 100. XML suite attributes sum to 508 cases. The baseline runner recorded 509/509 executed cases and its suite attributes sum to 513.

Baseline-matching failure:

```text
test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213
```

New/different final failures:

```text
test_generator_seam::test_generator_fingerprint_is_stable — FAILED: res://tests/test_generator_seam.gd:21
test_sun_cascades_gpu::test_needs_rebuild_agrees_with_what_build_does — FAILED: res://tests/test_sun_cascades_gpu.gd:137
```

The focused generator-seam run reproduced the failure: expected 24 values, got 0. This is an OPEN regression and was not masked or regenerated.

## Baseline and final failure comparison

Baseline failure case/message:

```text
test_sun_cascades_gpu::test_sub_texel_motion_rebuilds_no_cascade — FAILED: res://tests/test_sun_cascades_gpu.gd:73
```

Final changed sun-cascade case/message:

```text
test_sun_cascades_gpu::test_needs_rebuild_agrees_with_what_build_does — FAILED: res://tests/test_sun_cascades_gpu.gd:137
```

## Complete gdUnit per-suite comparison

The committed baseline is `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md`; the final XML was `reports/report_26/results.xml` at the recorded run. Counts below preserve each suite record (`tests`, `failures`, `errors`), including the baseline’s XML error case represented as `failures=0` in its source file.

| Suite | Baseline tests | Final tests | Baseline failures | Final failures | Final errors |
|---|---:|---:|---:|---:|---:|
| `test_world_store_contract` | 4 | 4 | 0 | 0 | 0 |
| `test_gpu_timing_scopes` | 1 | 1 | 0 | 0 | 0 |
| `test_collider_octants` | 4 | 4 | 0 | 0 | 0 |
| `test_mesh_lattice` | 3 | 3 | 0 | 0 | 0 |
| `test_outline` | 9 | 9 | 0 | 0 | 0 |
| `test_edit_fanout` | 4 | 4 | 0 | 0 | 0 |
| `test_capture` | 3 | 3 | 0 | 0 | 0 |
| `test_self_check` | 1 | 1 | 0 | 0 | 0 |
| `test_material_registry` | 3 | 3 | 0 | 0 | 0 |
| `test_demo_shell` | 5 | 5 | 0 | 0 | 0 |
| `test_render_shutdown` | 3 | 3 | 0 | 0 | 0 |
| `test_extension_boot` | 2 | 2 | 0 | 0 | 0 |
| `test_settings_menu` | 22 | 22 | 0 | 0 | 0 |
| `test_cel_object` | 3 | 3 | 0 | 0 | 0 |
| `test_field_volume_diff` | 5 | 5 | 0 | 0 | 0 |
| `test_repro_pillar_debris` | 1 | 1 | 0 | 0 | 0 |
| `test_frame_contract` | 6 | 6 | 0 | 0 | 0 |
| `test_lod_mesh_diff` | 3 | 3 | 0 | 0 | 0 |
| `test_frame_shipped_golden` | 1 | 1 | 0 | 0 | 0 |
| `test_contact_shadow` | 7 | 7 | 0 | 0 | 0 |
| `test_stored_normal_pool` | 2 | 2 | 0 | 0 | 0 |
| `test_raymarch_pixel` | 5 | 5 | 0 | 0 | 0 |
| `test_benchmark` | 8 | 8 | 0 | 0 | 0 |
| `test_composite_golden` | 2 | 2 | 0 | 0 | 0 |
| `test_voxel_settings` | 17 | 17 | 0 | 0 | 1 |
| `test_hiz` | 4 | 4 | 0 | 0 | 0 |
| `test_island_body` | 5 | 5 | 0 | 0 | 0 |
| `test_contact_shadow_golden` | 1 | 1 | 0 | 0 | 0 |
| `test_op_filter_gpu` | 4 | 4 | 0 | 0 | 0 |
| `test_render_lifetime_contract` | 6 | 6 | 0 | 0 | 0 |
| `test_ssgi` | 7 | 7 | 0 | 0 | 0 |
| `test_lod_cull` | 4 | 4 | 0 | 0 | 0 |
| `test_occupancy` | 4 | 4 | 0 | 0 | 0 |
| `test_connectivity` | 33 | 33 | 0 | 0 | 0 |
| `test_field_diff` | 6 | 1 | 0 | 0 | 0 |
| `test_shader_reload` | 2 | 2 | 0 | 0 | 0 |
| `test_raymarch_magenta` | 1 | 1 | 0 | 0 | 0 |
| `test_material_atlas` | 8 | 8 | 0 | 0 | 0 |
| `test_lod_pool` | 4 | 4 | 0 | 0 | 0 |
| `test_deferred` | 9 | 9 | 0 | 0 | 0 |
| `test_ssao_golden` | 1 | 1 | 0 | 0 | 0 |
| `test_region_dda` | 3 | 3 | 0 | 0 | 0 |
| `test_lod_build` | 5 | 5 | 0 | 0 | 0 |
| `test_grass` | 19 | 19 | 0 | 0 | 0 |
| `test_world_field_consumers` | 4 | 4 | 0 | 0 | 0 |
| `test_material_glow` | 3 | 3 | 0 | 0 | 0 |
| `test_world_field_overrides` | 4 | 4 | 0 | 0 | 0 |
| `test_normal_artifact` | 6 | 6 | 0 | 0 | 0 |
| `test_brick_flags_gpu` | 2 | 2 | 0 | 0 | 0 |
| `test_island_extract` | 5 | 5 | 0 | 0 | 0 |
| `test_collider_edits` | 3 | 3 | 0 | 0 | 0 |
| `test_player_kick` | 1 | 1 | 0 | 0 | 0 |
| `test_island_render` | 16 | 16 | 0 | 0 | 0 |
| `test_mesh_stream` | 5 | 5 | 0 | 0 | 0 |
| `test_emissive_gi` | 6 | 6 | 0 | 0 | 0 |
| `test_lod_budget` | 3 | 3 | 0 | 0 | 0 |
| `test_material_picker` | 5 | 5 | 0 | 0 | 0 |
| `test_field_baseline_gpu` | 1 | 1 | 0 | 0 | 0 |
| `test_raymarch_cost` | 2 | 2 | 0 | 0 | 0 |
| `test_lod_stream` | 3 | 3 | 0 | 0 | 0 |
| `test_pipeline_reload` | 1 | 1 | 0 | 0 | 0 |
| `test_beauty_settings` | 11 | 11 | 0 | 0 | 0 |
| `test_ssao` | 6 | 6 | 0 | 0 | 0 |
| `test_settings_names` | 5 | 5 | 0 | 0 | 0 |
| `test_lod_seam` | 3 | 3 | 0 | 0 | 0 |
| `test_material_seam` | 4 | 4 | 0 | 0 | 0 |
| `test_gpu_smoke` | 1 | 1 | 0 | 0 | 0 |
| `test_streaming` | 6 | 6 | 0 | 0 | 0 |
| `test_consolidation` | 20 | 20 | 0 | 0 | 0 |
| `test_generator_seam` | 2 | 2 | 0 | 1 | 0 |
| `test_gbuffer` | 5 | 5 | 0 | 0 | 0 |
| `test_sun_cascades_gpu` | 7 | 7 | 1 | 1 | 0 |
| `test_field_gradient` | 7 | 7 | 0 | 0 | 0 |
| `test_lod_gbuffer` | 4 | 4 | 0 | 0 | 0 |
| `test_repro_thin_sheet` | 3 | 3 | 0 | 0 | 0 |
| `test_collider_build_timing` | 1 | 1 | 0 | 0 | 0 |
| `test_near_field_scale` | 4 | 4 | 0 | 0 | 0 |
| `test_collider_stream` | 7 | 7 | 0 | 0 | 0 |
| `test_override_region_border` | 6 | 6 | 0 | 0 | 0 |
| `test_gpu_timings` | 10 | 10 | 0 | 0 | 0 |
| `test_raymarch_gbuffer` | 13 | 13 | 0 | 0 | 0 |
| `test_grass_golden` | 1 | 1 | 0 | 0 | 0 |
| `test_lod_render` | 4 | 4 | 0 | 0 | 0 |
| `test_sun_shadow` | 11 | 11 | 0 | 0 | 0 |
| `test_lod_cull_golden` | 1 | 1 | 0 | 0 | 0 |
| `test_lod_raster_golden` | 1 | 1 | 0 | 0 | 0 |
| `test_deferred_golden` | 1 | 1 | 0 | 0 | 0 |
| `test_ssr` | 7 | 7 | 0 | 0 | 0 |
| `test_gpu_atlas` | 8 | 8 | 0 | 0 | 0 |
| `test_edit_pipeline` | 8 | 8 | 0 | 0 | 0 |
| `test_brick_diff` | 7 | 7 | 0 | 0 | 0 |
| `test_voxel_world_raycast` | 3 | 3 | 0 | 0 | 0 |
| `test_raymarch_mips` | 2 | 2 | 0 | 0 | 0 |
| `test_occupancy_lattice` | 3 | 3 | 0 | 0 | 0 |
| `test_region_pass` | 7 | 7 | 0 | 0 | 0 |
| `test_mesh_diff` | 4 | 4 | 0 | 0 | 0 |

**Totals:** baseline suite attributes `tests=513`, `failures=1`; final suite attributes `tests=508`, `failures=2`, `errors=1`. Baseline executed 509/509; final executed 505/505.


Native Task 13 Step 5 output:

```text
pipeline gradient bound 3.290000 exceeds the declared ceiling 2.000000; raise the ceiling only if the raymarcher can afford the steps:
  hills: add 1.780000
  relief: add 0.210000
  cave: mul 1.000000
  mesas: add 1.300000
```

Godot Task 13 Step 5 output:

```text
ERROR: terrain pipeline: pipeline gradient bound 3.290000 exceeds the declared ceiling 2.000000; raise the ceiling only if the raymarcher can afford the steps:
  hills: add 1.780000
  relief: add 0.210000
  cave: mul 1.000000
  mesas: add 1.300000
```

## Goldens

| Golden | Moved? | Cause |
|---|---|---|
| `tests/golden/default_pipeline_field.txt` | y (created) | G3's new CPU pin for `default.pipeline`; field data is pinned bit-for-bit |
| `tests/golden/field_baseline.txt` | n | unchanged |
| `tests/golden/brick_baseline.txt` | n | unchanged |
| `shaders/generated/field.glslh.golden` | y | generated output records the stage Lipschitz declarations and the computed default bound comment (`2` → `1.99`), plus the corresponding generated metadata |

## Reported Lipschitz bounds

| Pipeline | Before | After | Ceiling |
|---|---|---|---|
| `default.pipeline` | 2.0 (declared) | 1.99 | 2.0 |
| `golden.pipeline` | 2.0 (declared) | 1.78 | 2.0 |
| `mesas.pipeline` | — | 3.29 | none |

## gdUnit suites that moved

- `test_world_field_consumers::test_raycasts_are_pinned`: the golden moved because `golden.pipeline` now reports 1.78 instead of the old declared 2.0. Hit/miss pattern, materials, and normals remained identical; positions/distances moved by at most 0.02 and the golden was re-recorded.
- `test_field_diff`: case count changed from six functions to one parameterised function in Task 2 so all pipelines and six scenarios run in one test; this is intentional, but it is a strict Task 1 suite-count drop.
- No other value golden moved. The final `test_generator_seam` and `test_sun_cascades_gpu` failures are regression-gate findings, not accepted golden movements.

## Deviations from the spec

1. All four CPU mirrors migrated in one commit rather than one per commit: `StageFn`'s
   signature change is atomic.
2. `VE_STAGE_SLOTS` takes the PascalCase struct prefix explicitly, because a macro cannot
   capitalise its argument.
3. The spec's `test_lipschitz_rule.cpp` is split in two: the combination-rule and ceiling
   cases live in `test_pipeline_resolve.cpp` beside the other resolver cases, and only the
   sampled check got its own file, `test_lipschitz_sampled.cpp`.

## Open findings

- The final regression gate is OPEN: `test_generator_seam::test_generator_fingerprint_is_stable` reproducibly returns an empty fingerprint before world initialization, and the full-run sun-cascade failure no longer matches the baseline case/message.
- The strict suite-count rule is OPEN because Task 2 intentionally reduced `test_field_diff` from six cases to one; the baseline was not updated.
- `mesas.pipeline` has the declared 3.29 bound and passes the CPU/GPU field diff, but the sampled 4096-point native check does not enumerate it.
