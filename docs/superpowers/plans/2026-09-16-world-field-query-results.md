# World field query (sub-project 5a) — results

Implementation baseline tested through: `c0ac8e0`. Documentation commit: `2947a50`. Recorded 2026-09-17 on Jeremys-Mac-mini.local (arm64, Apple M1 / Metal 4, Godot 4.7.2.stable.official.ed1daf0bf).
Baseline: `docs/superpowers/plans/2026-09-16-world-field-query-baseline.md`. Report: `reports/report_50`.

## Regression
Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` — Build OK.

Native: `[doctest] test cases:     624 |     624 passed | 0 failed | 0 skipped`

gdUnit: 507 declared cases / 1 error / 1 failure in `reports/report_50/results.xml`; the runner executed 503/503 cases across 95/95 suites. Differences from baseline:
- `test_override_region_border`: new suite, 6 cases, all passed.
- `test_world_field_consumers`: new suite, 4 cases, all passed.
- `test_voxel_world_raycast`: new suite, 3 cases, all passed.
- Existing suite declared counts did not drop. The five vanished baseline failure/error lines are:
  - `test_gpu_timing_scopes::test_ssao_scope_closes_before_deferred_opens — ERROR: res://tests/test_gpu_timing_scopes.gd:28`
  - `test_cel_object::test_shaderlanguage_matches_ve_cel_shade — ERROR: res://tests/test_cel_object.gd:35`
  - `test_cel_object::test_object_lighting_follows_the_scene_sun — ERROR: res://tests/test_cel_object.gd:73`
  - `test_frame_shipped_golden::test_the_shipped_frame_matches_the_recorded_golden — ERROR: res://tests/test_frame_shipped_golden.gd:128`
  - `test_render_lifetime_contract::test_the_shipped_path_survives_reload_and_shuts_down — FAILED: res://tests/test_render_lifetime_contract.gd:218`
  The baseline `test_render_lifetime_contract` suite was `tests=6 failures=1 errors=1`: the failure line above vanished, and one separate error child also vanished (`errors=1`); the baseline artifact preserves its count but not a separate error message line.
- Exact remaining final failure/error lines from `reports/report_50/results.xml` are:
  - `test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213` (`error/@message`: `ERROR: res://tests/test_voxel_settings.gd:213`; suite: `tests=17 failures=0 errors=1`)
  - `test_sun_cascades_gpu::test_sub_texel_motion_rebuilds_no_cascade — FAILED: res://tests/test_sun_cascades_gpu.gd:73` (`failure/@message`: `FAILED: res://tests/test_sun_cascades_gpu.gd:73`; suite: `tests=7 failures=1 errors=0`)
- `test_voxel_settings::test_an_ambient_change_reaches_the_object_global` remains the known Godot/Metal `Nil` environment error.
- `test_sun_cascades_gpu::test_sub_texel_motion_rebuilds_no_cascade` remains the baseline failure with the same case and message location; no new failure case/message appeared.
- All other suites kept their declared counts and failure sets. `test_connectivity` and `test_island_body` remained within their known flaky-by-case baseline rule.

## Suspected bugs
| Claim | Result | Test | Commits |
|---|---|---|---|
| S3a brick wrap (region-map shaders) | FIXED | `test_a_brick_beside_a_region_border_reads_no_wrapped_override` | `a799b0c` test, `1bd8e1e` fix |
| S3a brick wrap (single-table jobs) | FIXED | `test_a_collider_chunk_lattice_reads_no_wrapped_override`, `test_an_island_lattice_across_a_region_border_reads_no_wrapped_override` | `a799b0c` test, `1bd8e1e` fix |
| S3b LoD origin table | FIXED | `test_a_lod_chunk_reads_overrides_of_every_region_it_covers` | `a799b0c` test, `1bd8e1e` fix |
| S3c LoD op prefix | FIXED | `test_an_over_cap_lod_chunk_is_refused_not_truncated`, `test_ops_too_small_for_a_lod_level_do_not_count_against_its_cap` | `dd4fff3` test, `d0910b8` fix |

Frame time A/B/A for the S3a/S3b shader fix (avg/p99, milliseconds):
- steady: `30.37/31.99` / `30.41/30.56` / `30.28/30.58`
- ridge: `28.19/38.62` / `29.08/39.60` / `28.16/37.12`
- edit-bounded: `31.10/36.95` / `31.41/35.56` / `31.10/37.22`

The B run was outside the A1–A2 spread for ridge average (+3.2%) and edit-bounded average (+1.0%); the fix commit records the controller ruling accepting that 1–3% cost.

## Goldens
- `test_frame_shipped_golden` — unchanged; passed in `report_50` (its baseline error did not recur).
- `test_lod_raster_golden` — unchanged; passed.
- `test_world_field_consumers` — unchanged since Task 5; passed.
- No production, golden, or pin files were changed by Task 15.

## Exit checks
Command 1:
```text
rg 'struct LogProbe|struct LogContactProbe|snapshot_field_sources|raycast_down' extension/src
```
Output: empty.

Command 2:
```text
rg 'debug_raycast' demo/edit_tool.gd demo/hud.gd
```
Output: empty.

Command 3:
```text
rg 'edit_log\(\)->ops\(|collect_ops_for_aabb' extension/src/physics extension/src/mesh extension/src/debug extension/src/voxel_world.cpp
```
Output and owner:
- `extension/src/physics/island_manager.cpp:771` — 5b `land_extraction` staleness check.
- `extension/src/debug/hooks_physics.cpp:965` — 5b collider mesh request op copy.
- `extension/src/debug/hooks_render.cpp:1088` — diagnostic raymarch-normal CPU oracle; it needs the hit point's region op list.
- `extension/src/debug/hooks_physics.cpp:377,440` — diagnostic mesh-diff GPU job op copies.
- `extension/src/debug/hooks_world.cpp:535` — diagnostic `self_check` brick-diff GPU job op copy.
- `extension/src/debug/hooks_world.cpp:922,964` — diagnostic brick-flags GPU job op copies.
- `extension/src/debug/hooks_world.cpp:1082,1146` — diagnostic occupancy GPU job op copies.
- `extension/src/debug/hooks_world.cpp:1206` — diagnostic cell-state query; it needs the brick's region op list.

The first two matches in command 1 are comments, and command 2 is clean. The remaining command-3 sites are the 5b-owned sites or diagnostics that deliberately assemble a region-specific GPU/CPU diagnostic input.

The focused consumer list in the earlier plan names `res://tests/test_stored_normals.gd`, but that path is absent in this branch. The native `extension/tests/test_stored_normals.cpp` and gdUnit `test_stored_normal_pool` are present and included by the full run; no missing test file was created.

## Change cost (spec §7)
| Scenario | Before | After | Files after |
|---|---|---|---|
| New downward ground query in a consumer that holds the store | 3 | 1 | the consumer (`store->field().lock().raycast(...)`) |

## Handed to 5b
`EditPipeline::apply(ops[], policy)` and `invalidate(aabb, reason)` fan-out; edit fan-out characterization (which consumer hears which AABB); removing `max_override_bricks = 1` from `test_connectivity.gd` behind a test proving it is still needed; lock order stated in exactly one header; `IslandManager::land_extraction` op headroom and its staleness check (`island_manager.cpp:781`); consolidation's hand-repeated invalidation (`consolidation.cpp:157-171`, `554-564`); deleting `WorldStore::append_edit` and `VoxelWorld::append_edit_locked`'s tail; the exit check `rg 'WorldStore::append_edit\('`.

Additional 5b-owned sites retained by the exit scan: `extension/src/physics/island_manager.cpp:771` and `extension/src/debug/hooks_physics.cpp:965`.

## Open
- Known baseline/environment failures remain: the Metal `Nil` error in `test_voxel_settings` and the `test_sun_cascades_gpu` failure.
- `test_stored_normals.gd` is still absent; the existing native stored-normal test and `test_stored_normal_pool` were not changed.
- The two stale `snapshot_field_sources` mentions were removed from comments in `world_field.{h,cpp}` to satisfy the explicit empty exit scan; behavior is unchanged.
- The consolidation snapshot is `ve::ConsolidationSnapshot`; `ve::RegionSnapshot` remains the archive interface.
