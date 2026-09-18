# Stage authoring (sub-project 6) — results

**Spec:** `docs/superpowers/specs/2026-09-17-stage-authoring-design.md`
**Baseline:** `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md`
**Range:** `b03e5d6..4685a1d` (Task 14 documentation is the closing commit)

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

## The refusal message

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
