# Frame module — baseline (clean branch at 72eae3c)

Recorded 2026-09-13, Mac mini (Apple M1 CPU, Apple M1 GPU).

## Native
[doctest] test cases:     538 |     538 passed | 0 failed | 0 skipped

## gdUnit per-suite counts
# test_world_store_contract: tests=4 failures=0
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
# test_lod_mesh_diff: tests=3 failures=0
# test_contact_shadow: tests=7 failures=1
# test_stored_normal_pool: tests=2 failures=0
# test_raymarch_pixel: tests=5 failures=0
# test_benchmark: tests=8 failures=0
# test_debug_menu: tests=8 failures=0
# test_hiz: tests=4 failures=0
# test_island_body: tests=5 failures=2
# test_op_filter_gpu: tests=4 failures=0
# test_ssgi: tests=7 failures=1
# test_lod_cull: tests=4 failures=1
# test_occupancy: tests=4 failures=0
# test_connectivity: tests=33 failures=2
# test_field_diff: tests=6 failures=0
# test_shader_reload: tests=2 failures=0
# test_raymarch_magenta: tests=1 failures=0
# test_material_atlas: tests=8 failures=1
# test_lod_pool: tests=4 failures=1
# test_deferred: tests=9 failures=0
# test_ssao_golden: tests=1 failures=0
# test_region_dda: tests=3 failures=0
# test_lod_build: tests=5 failures=0
# test_grass: tests=18 failures=0
# test_material_glow: tests=3 failures=0
# test_normal_artifact: tests=6 failures=0
# test_brick_flags_gpu: tests=2 failures=0
# test_island_extract: tests=5 failures=0
# test_collider_edits: tests=3 failures=1
# test_player_kick: tests=1 failures=0
# test_island_render: tests=16 failures=0
# test_mesh_stream: tests=5 failures=2
# test_emissive_gi: tests=6 failures=0
# test_lod_budget: tests=3 failures=0
# test_material_picker: tests=5 failures=0
# test_field_baseline_gpu: tests=1 failures=0
# test_raymarch_cost: tests=2 failures=0
# test_lod_stream: tests=3 failures=1
# test_pipeline_reload: tests=1 failures=0
# test_beauty_settings: tests=9 failures=0
# test_ssao: tests=6 failures=0
# test_lod_seam: tests=3 failures=0
# test_material_seam: tests=4 failures=1
# test_gpu_smoke: tests=1 failures=0
# test_streaming: tests=6 failures=2
# test_consolidation: tests=18 failures=4
# test_generator_seam: tests=2 failures=0
# test_gbuffer: tests=5 failures=0
# test_sun_cascades_gpu: tests=7 failures=0
# test_field_gradient: tests=7 failures=0
# test_lod_gbuffer: tests=4 failures=1
# test_repro_thin_sheet: tests=3 failures=0
# test_collider_build_timing: tests=1 failures=0
# test_near_field_scale: tests=4 failures=0
# test_collider_stream: tests=7 failures=1
# test_gpu_timings: tests=10 failures=0
# test_raymarch_gbuffer: tests=13 failures=0
# test_grass_golden: tests=1 failures=0
# test_lod_render: tests=4 failures=0
# test_sun_shadow: tests=11 failures=1
# test_ssr: tests=7 failures=1
# test_gpu_atlas: tests=8 failures=0
# test_edit_pipeline: tests=8 failures=1
# test_brick_diff: tests=7 failures=0
# test_raymarch_mips: tests=2 failures=0
# test_occupancy_lattice: tests=3 failures=0
# test_region_pass: tests=7 failures=0
# test_mesh_diff: tests=4 failures=0

## gdUnit failing cases
test_contact_shadow::test_a_crater_darkens_its_own_floor — FAILED: res://tests/test_contact_shadow.gd:39
test_island_body::test_a_body_lands_on_the_streamed_collider_and_sleeps — FAILED: res://tests/test_island_body.gd:67
test_ssgi::test_light_bounces_once_the_history_exists — FAILED: res://tests/test_ssgi.gd:46
test_lod_cull::test_facing_away_culls_almost_everything — FAILED: res://tests/test_lod_cull.gd:71
test_connectivity::test_the_grid_and_the_flood_find_a_severed_pillar_top — FAILED: res://tests/test_connectivity.gd:125
test_material_atlas::test_every_material_normal_map_carries_real_relief — FAILED: res://tests/test_material_atlas.gd:157
test_lod_pool::test_ticking_streams_chunks_in — FAILED: res://tests/test_lod_pool.gd:70
test_collider_edits::test_carving_makes_the_ground_fall_away — FAILED: res://tests/test_collider_edits.gd:75
test_mesh_stream::test_a_batch_is_submitted_once_and_collected_once — FAILED: res://tests/test_mesh_stream.gd:46
test_lod_stream::test_an_edit_rebuilds_every_level_it_touches — FAILED: res://tests/test_lod_stream.gd:63
test_material_seam::test_rays_do_not_leak_beside_a_hard_material_crater — FAILED: res://tests/test_material_seam.gd:213
test_streaming::test_a_starved_atlas_heals_instead_of_keeping_the_holes — FAILED: res://tests/test_streaming.gd:279
test_consolidation::test_bake_reproduces_the_field — FAILED: res://tests/test_consolidation.gd:30
test_lod_gbuffer::test_far_field_pixels_carry_a_material_and_a_unit_normal — FAILED: res://tests/test_lod_gbuffer.gd:49
test_collider_stream::test_colliders_appear_around_the_player — FAILED: res://tests/test_collider_stream.gd:87
test_sun_shadow::test_moving_the_sun_moves_the_shadow — FAILED: res://tests/test_sun_shadow.gd:270
test_ssr::test_removing_the_scene_only_blocker_removes_its_hits — FAILED: res://tests/test_ssr.gd:44
test_edit_pipeline::test_paint_recolours_grass_to_rock_without_moving_the_surface — FAILED: res://tests/test_edit_pipeline.gd:149

Known flaky-by-case suites (memory): test_connectivity, test_island_body — a different case may
fail each run; compare the suite's failure COUNT, not the case name.
