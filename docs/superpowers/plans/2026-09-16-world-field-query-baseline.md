# World field query — baseline

Commit: `71d58b1`. Recorded 2026-09-16 on Jeremys-Mac-mini (arm64, Apple M1 / Metal 4.0 Forward+, Godot 4.7.2.stable.official.ed1daf0bf).
Report: `reports/report_1`.

## Native
[doctest] test cases:     612 |     612 passed | 0 failed | 0 skipped

## gdUnit per-suite counts
# test_world_store_contract: tests=4 failures=0 errors=0
# test_gpu_timing_scopes: tests=1 failures=0 errors=1
# test_collider_octants: tests=4 failures=0 errors=0
# test_mesh_lattice: tests=3 failures=0 errors=0
# test_outline: tests=9 failures=0 errors=0
# test_capture: tests=3 failures=0 errors=0
# test_self_check: tests=1 failures=0 errors=0
# test_material_registry: tests=3 failures=0 errors=0
# test_demo_shell: tests=5 failures=0 errors=0
# test_render_shutdown: tests=3 failures=0 errors=0
# test_extension_boot: tests=2 failures=0 errors=0
# test_settings_menu: tests=22 failures=0 errors=0
# test_cel_object: tests=3 failures=0 errors=2
# test_field_volume_diff: tests=5 failures=0 errors=0
# test_repro_pillar_debris: tests=1 failures=0 errors=0
# test_frame_contract: tests=6 failures=0 errors=0
# test_lod_mesh_diff: tests=3 failures=0 errors=0
# test_frame_shipped_golden: tests=1 failures=0 errors=1
# test_contact_shadow: tests=7 failures=0 errors=0
# test_stored_normal_pool: tests=2 failures=0 errors=0
# test_raymarch_pixel: tests=5 failures=0 errors=0
# test_benchmark: tests=8 failures=0 errors=0
# test_composite_golden: tests=2 failures=0 errors=0
# test_voxel_settings: tests=17 failures=0 errors=1
# test_hiz: tests=4 failures=0 errors=0
# test_island_body: tests=5 failures=0 errors=0
# test_contact_shadow_golden: tests=1 failures=0 errors=0
# test_op_filter_gpu: tests=4 failures=0 errors=0
# test_render_lifetime_contract: tests=6 failures=1 errors=1
# test_ssgi: tests=7 failures=0 errors=0
# test_lod_cull: tests=4 failures=0 errors=0
# test_occupancy: tests=4 failures=0 errors=0
# test_connectivity: tests=33 failures=0 errors=0
# test_field_diff: tests=6 failures=0 errors=0
# test_shader_reload: tests=2 failures=0 errors=0
# test_raymarch_magenta: tests=1 failures=0 errors=0
# test_material_atlas: tests=8 failures=0 errors=0
# test_lod_pool: tests=4 failures=0 errors=0
# test_deferred: tests=9 failures=0 errors=0
# test_ssao_golden: tests=1 failures=0 errors=0
# test_region_dda: tests=3 failures=0 errors=0
# test_lod_build: tests=5 failures=0 errors=0
# test_grass: tests=19 failures=0 errors=0
# test_material_glow: tests=3 failures=0 errors=0
# test_world_field_overrides: tests=4 failures=0 errors=0
# test_normal_artifact: tests=6 failures=0 errors=0
# test_brick_flags_gpu: tests=2 failures=0 errors=0
# test_island_extract: tests=5 failures=0 errors=0
# test_collider_edits: tests=3 failures=0 errors=0
# test_player_kick: tests=1 failures=0 errors=0
# test_island_render: tests=16 failures=0 errors=0
# test_mesh_stream: tests=5 failures=0 errors=0
# test_emissive_gi: tests=6 failures=0 errors=0
# test_lod_budget: tests=3 failures=0 errors=0
# test_material_picker: tests=5 failures=0 errors=0
# test_field_baseline_gpu: tests=1 failures=0 errors=0
# test_raymarch_cost: tests=2 failures=0 errors=0
# test_lod_stream: tests=3 failures=0 errors=0
# test_pipeline_reload: tests=1 failures=0 errors=0
# test_beauty_settings: tests=11 failures=0 errors=0
# test_ssao: tests=6 failures=0 errors=0
# test_settings_names: tests=5 failures=0 errors=0
# test_lod_seam: tests=3 failures=0 errors=0
# test_material_seam: tests=4 failures=0 errors=0
# test_gpu_smoke: tests=1 failures=0 errors=0
# test_streaming: tests=6 failures=0 errors=0
# test_consolidation: tests=18 failures=0 errors=0
# test_generator_seam: tests=2 failures=0 errors=0
# test_gbuffer: tests=5 failures=0 errors=0
# test_sun_cascades_gpu: tests=7 failures=1 errors=0
# test_field_gradient: tests=7 failures=0 errors=0
# test_lod_gbuffer: tests=4 failures=0 errors=0
# test_repro_thin_sheet: tests=3 failures=0 errors=0
# test_collider_build_timing: tests=1 failures=0 errors=0
# test_near_field_scale: tests=4 failures=0 errors=0
# test_collider_stream: tests=7 failures=0 errors=0
# test_gpu_timings: tests=10 failures=0 errors=0
# test_raymarch_gbuffer: tests=13 failures=0 errors=0
# test_grass_golden: tests=1 failures=0 errors=0
# test_lod_render: tests=4 failures=0 errors=0
# test_sun_shadow: tests=11 failures=0 errors=0
# test_lod_cull_golden: tests=1 failures=0 errors=0
# test_lod_raster_golden: tests=1 failures=0 errors=0
# test_deferred_golden: tests=1 failures=0 errors=0
# test_ssr: tests=7 failures=0 errors=0
# test_gpu_atlas: tests=8 failures=0 errors=0
# test_edit_pipeline: tests=8 failures=0 errors=0
# test_brick_diff: tests=7 failures=0 errors=0
# test_raymarch_mips: tests=2 failures=0 errors=0
# test_occupancy_lattice: tests=3 failures=0 errors=0
# test_region_pass: tests=7 failures=0 errors=0
# test_mesh_diff: tests=4 failures=0 errors=0

## gdUnit failing cases
test_gpu_timing_scopes::test_ssao_scope_closes_before_deferred_opens — ERROR: res://tests/test_gpu_timing_scopes.gd:28
test_cel_object::test_shaderlanguage_matches_ve_cel_shade — ERROR: res://tests/test_cel_object.gd:35
test_cel_object::test_object_lighting_follows_the_scene_sun — ERROR: res://tests/test_cel_object.gd:73
test_frame_shipped_golden::test_the_shipped_frame_matches_the_recorded_golden — ERROR: res://tests/test_frame_shipped_golden.gd:128
test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213
test_render_lifetime_contract::test_the_shipped_path_survives_reload_and_shuts_down — FAILED: res://tests/test_render_lifetime_contract.gd:218
test_sun_cascades_gpu::test_sub_texel_motion_rebuilds_no_cascade — FAILED: res://tests/test_sun_cascades_gpu.gd:73

Known flaky-by-case suites (project memory): test_connectivity, test_island_body — compare the
suite's failure COUNT, not the case name. Known environment error: test_voxel_settings::
test_an_ambient_change_reaches_the_object_global (Godot/Metal returns Nil outside the editor).
