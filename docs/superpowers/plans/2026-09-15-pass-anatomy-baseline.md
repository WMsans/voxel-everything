# Pass anatomy — baseline

Commit: `27bc434`. Recorded 2026-09-15 on Darwin arm64 (Mac mini, Apple M1; Godot v4.7.2.stable.official.ed1daf0bf, Metal 4.0 Forward+).
Report: `reports/report_1` (total tests=459 failures=0 errors=0; `Run tests ends with 0`).

## Native
[doctest] test cases:     551 |     551 passed | 0 failed | 0 skipped

## gdUnit per-suite counts
# test_world_store_contract: tests=4 failures=0
# test_gpu_timing_scopes: tests=1 failures=0
# test_collider_octants: tests=4 failures=0
# test_mesh_lattice: tests=3 failures=0
# test_outline: tests=9 failures=0
# test_capture: tests=3 failures=0
# test_self_check: tests=1 failures=0
# test_material_registry: tests=3 failures=0
# test_demo_shell: tests=5 failures=0
# test_render_shutdown: tests=3 failures=0
# test_extension_boot: tests=2 failures=0
# test_settings_menu: tests=15 failures=0
# test_cel_object: tests=2 failures=0
# test_field_volume_diff: tests=5 failures=0
# test_repro_pillar_debris: tests=1 failures=0
# test_frame_contract: tests=6 failures=0
# test_lod_mesh_diff: tests=3 failures=0
# test_frame_shipped_golden: tests=1 failures=0
# test_contact_shadow: tests=7 failures=0
# test_stored_normal_pool: tests=2 failures=0
# test_raymarch_pixel: tests=5 failures=0
# test_benchmark: tests=8 failures=0
# test_debug_menu: tests=8 failures=0
# test_hiz: tests=4 failures=0
# test_island_body: tests=5 failures=0
# test_op_filter_gpu: tests=4 failures=0
# test_render_lifetime_contract: tests=6 failures=0
# test_ssgi: tests=7 failures=0
# test_lod_cull: tests=4 failures=0
# test_occupancy: tests=4 failures=0
# test_connectivity: tests=33 failures=0
# test_field_diff: tests=6 failures=0
# test_shader_reload: tests=2 failures=0
# test_raymarch_magenta: tests=1 failures=0
# test_material_atlas: tests=8 failures=0
# test_lod_pool: tests=4 failures=0
# test_deferred: tests=9 failures=0
# test_ssao_golden: tests=1 failures=0
# test_region_dda: tests=3 failures=0
# test_lod_build: tests=5 failures=0
# test_grass: tests=18 failures=0
# test_material_glow: tests=3 failures=0
# test_normal_artifact: tests=6 failures=0
# test_brick_flags_gpu: tests=2 failures=0
# test_island_extract: tests=5 failures=0
# test_collider_edits: tests=3 failures=0
# test_player_kick: tests=1 failures=0
# test_island_render: tests=16 failures=0
# test_mesh_stream: tests=5 failures=0
# test_emissive_gi: tests=6 failures=0
# test_lod_budget: tests=3 failures=0
# test_material_picker: tests=5 failures=0
# test_field_baseline_gpu: tests=1 failures=0
# test_raymarch_cost: tests=2 failures=0
# test_lod_stream: tests=3 failures=0
# test_pipeline_reload: tests=1 failures=0
# test_beauty_settings: tests=9 failures=0
# test_ssao: tests=6 failures=0
# test_lod_seam: tests=3 failures=0
# test_material_seam: tests=4 failures=0
# test_gpu_smoke: tests=1 failures=0
# test_streaming: tests=6 failures=0
# test_consolidation: tests=18 failures=0
# test_generator_seam: tests=2 failures=0
# test_gbuffer: tests=5 failures=0
# test_sun_cascades_gpu: tests=7 failures=0
# test_field_gradient: tests=7 failures=0
# test_lod_gbuffer: tests=4 failures=0
# test_repro_thin_sheet: tests=3 failures=0
# test_collider_build_timing: tests=1 failures=0
# test_near_field_scale: tests=4 failures=0
# test_collider_stream: tests=7 failures=0
# test_gpu_timings: tests=10 failures=0
# test_raymarch_gbuffer: tests=13 failures=0
# test_grass_golden: tests=1 failures=0
# test_lod_render: tests=4 failures=0
# test_sun_shadow: tests=11 failures=0
# test_ssr: tests=7 failures=0
# test_gpu_atlas: tests=8 failures=0
# test_edit_pipeline: tests=8 failures=0
# test_brick_diff: tests=7 failures=0
# test_raymarch_mips: tests=2 failures=0
# test_occupancy_lattice: tests=3 failures=0
# test_region_pass: tests=7 failures=0
# test_mesh_diff: tests=4 failures=0

## gdUnit failing cases
none

Leak lines in the full run: 0.
Flaky by case (compare failure COUNT): test_connectivity, test_island_body.

## SP2 runtime gates
- test_render_lifetime_contract: pass (tests=6 failures=0)
- test_frame_shipped_golden: pass (tests=1 failures=0)
- test_frame_contract: pass (tests=6 failures=0)
- test_gpu_timing_scopes: pass (tests=1 failures=0)

## Site rows
Filled by Tasks 2–4: per site, the failing cases and leak count of its Pass gate suite set on this commit.

## Evidence log
Appended by later tasks.
