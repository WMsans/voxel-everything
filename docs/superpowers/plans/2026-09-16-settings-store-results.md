# Settings store — results

Commit: `5facda0`. Recorded 2026-09-16 on arm64 macOS 26.4.1 / Apple M1 / Metal 4 / Godot 4.7.2.stable.official.ed1daf0bf.
Baseline: `docs/superpowers/plans/2026-09-16-settings-store-baseline.md`. Report: `reports/report_66`.

## Regression
Native: `[doctest] test cases:     611 |     611 passed | 0 failed | 0 skipped`
gdUnit: 492 test cases / 1 error / 0 failures; differences from baseline:
- `test_settings_names`: new suite, 5 cases, 0 failures; all cases passed.
- `test_world_field_overrides`: new suite, 4 cases, 0 failures; all cases passed.
- `test_voxel_settings`: new suite, 16 cases, 1 error; `test_an_ambient_change_reaches_the_object_global` hit the documented Godot/Metal `Nil` environment limitation below.
- `test_debug_menu`: gone, 8 baseline cases ported into `test_settings_menu`.
- `test_settings_menu`: 15 → 21 cases, +6 ported cases, 0 failures.
- `test_beauty_settings`: 9 → 11 cases, +2 S6 cases, 0 failures.
- All other baseline suites kept their case counts and 0-failure counts. The flaky-by-case suites `test_connectivity` (33 → 33) and `test_island_body` (5 → 5) were compared by count; neither had a recorded failure.
- Baseline total: 467 cases, 0 failures; current total: 492 cases, 0 failures, 1 error. No unrelated test failure was recorded in `report_66`.

The sole error was:
`test_voxel_settings::test_an_ambient_change_reaches_the_object_global` — `ERROR: res://tests/test_voxel_settings.gd:204`; Godot logged `global_shader_parameter_get` as editor-only and returned `Nil`, producing `Trying to assign value of type 'Nil' to a variable of type 'Vector3'.` The test was not removed or relaxed.

## Suspected bugs
| Id | Result | Test | Fix commit |
|---|---|---|---|
| S1 | FIXED | `test_a_consolidated_fill_in_open_sky_still_gets_a_collider`, `test_a_pasted_volume_in_open_sky_gets_a_collider` | `9757819` |
| S2 | FIXED | `test_the_contact_probe_reads_a_consolidated_carve`, `test_the_cpu_island_extract_reads_a_consolidated_carve` | `d9a41ea` |
| S5 | FIXED | `test_every_slider_range_contains_the_shipped_value_and_sits_inside_the_clamp`, native row invariants | `0b8b9a8` |
| S6 | FIXED | `test_a_tweak_survives_a_tier_change`, `test_effects_turned_off_before_a_tier_stay_off` | `aeea5f8` |

## Goldens
- `test_frame_shipped_golden` — unchanged
- `test_ssao_golden` — unchanged
- `test_contact_shadow_golden` — unchanged
- `test_deferred_golden` — unchanged

## Exit checks
```text
$ rg 'if \(name == "' extension/src/render/orchestrator.cpp
<empty output; exit 1>

$ rg 'kSsaoRadius|kSsaoStrength|kAmbient|beauty_value_field|beauty_field' extension/src
<empty output; exit 1>

$ rg 'ConfigFile|KEY_F7|RESOLUTIONS|UPSCALERS|QUALITY_TIERS|GRASS_VALUES' demo
<empty output; exit 1>

$ rg 'min_value|max_value' demo/settings_menu.tscn
<empty output; exit 1>

$ rg 'settings_group|render_settings_|beauty\.' extension/src/render/frame.cpp
	if (ssgi && beauty.ssgi) {
	for (int k = 0; k < 3; k++) dp.ambient[k] = beauty.ambient[k];

$ ls demo/debug_menu.gd
ls: demo/debug_menu.gd: No such file or directory
```

`ls demo/debug_menu.gd` failed as required. The four golden suites above each passed unchanged in `report_66` (`tests=1`, `errors=0`, `failures=0`).

## Change cost (Appendix A re-trace)
| Scenario | Before | After | Files |
|---|---|---|---|
| Beauty float knob | 9 (11 with inspector) | 4 | `extension/src/shade/beauty_settings.h`, `extension/src/shade/beauty_settings.cpp`, `extension/src/render/ssao_pass.cpp`, `shaders/ssao.comp.glsl` |
| Beauty int knob | not settable by name | settable by name | — |
| Display / render dial | 3 hand-written homes (field, menu table, cfg key) | 4 | `extension/src/settings/render_settings.h`, `extension/src/settings/render_settings.cpp`, `extension/src/render/orchestrator.cpp`, `extension/src/render/frame.cpp` (`near_field_scale`) |

The beauty-float trace follows `ssao_radius`/`ssao_strength`: the struct and row are the first two files, the SSAO push is the third, and the shader reader is the fourth. The panel, inspector, persistence and debug dictionary consume the row. The beauty-int trace follows `ssgi_taps` through `set_effect_value` and the row table, and `test_every_beauty_count_is_settable_by_name` passes.

## Open
- The required ambient GPU contract remains environment-blocked on Godot 4.7.2 / Metal 4: `RenderingServer.global_shader_parameter_get("ve_ambient")` returns `Nil` outside the editor. The test remains intact.
- The mandated `frame.cpp` scan is not empty: it reports the existing snapshot reads `beauty.ssgi` and `beauty.ambient`. Task 18 made no production changes, so this was recorded rather than hidden or altered.
- The full run log emitted existing Metal `timeout waiting for fence` diagnostics in `test_connectivity` while those cases were reported `PASSED`; XML records no connectivity or island-body failure.
