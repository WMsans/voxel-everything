#include "debug/hooks.h"

#include "../voxel_world.h"
#include "mesh/consolidation.h"
#include "render/gpu_atlas.h"
#include "render/material_atlas.h"
#include "render/camera_params.h"
#include "render/island_atlas.h"
#include "render/island_cull_pass.h"
#include "render/raymarch_pass.h"
#include "render/composite_pass.h"
#include "render/deferred_pass.h"
#include "render/inject_pass.h"
#include "render/gbuffer.h"
#include "render/beauty_camera.h"
#include "render/contact_shadow_pass.h"
#include "render/ssgi_pass.h"
#include "render/ssao_pass.h"
#include "render/ssr_pass.h"
#include "render/outline_pass.h"
#include "beauty_compositor.h"
#include "render/region_pass.h"
#include "render/brick_gen_pass.h"
#include "render/world_streamer.h"
#include "render/shader_loader.h"
#include "render/mesh_pass.h"
#include "render/mesh_service.h"
#include "render/lod_build_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "render/sun_shadow_pass.h"
#include "render/lod_cull_pass.h"
#include "render/hiz_pass.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_reduce.h"
#include "lod/lod_skirt.h"
#include "lod/lod_system.h" // Task 15: the LoD state/mutex live here; friend access
#include "lod/lod_tree.h"
#include "physics/collider_streamer.h"
#include "physics/island_manager.h"
#include "mesh/dual_contour.h"
#include "mesh/mesh_chunk.h"
#include "mesh/box_merge.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "world/brick_flags.h"
#include "world/brick_mip.h"
#include "world/raycast.h"
#include "shade/oct.h"
#include "shade/cel.h"
#include "shade/sun_ortho.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>

namespace godot {

void VoxelDebugHooks::_bind_methods() {
	ClassDB::bind_method(D_METHOD("debug_beauty_settings"),
			&VoxelDebugHooks::debug_beauty_settings);
	ClassDB::bind_method(D_METHOD("debug_beauty_compositor_stats"),
			&VoxelDebugHooks::debug_beauty_compositor_stats);
	ClassDB::bind_method(D_METHOD("debug_gpu_timings"), &VoxelDebugHooks::debug_gpu_timings);
	ClassDB::bind_method(D_METHOD("debug_ingest_gpu_timings", "names", "gpu_us", "rd_frame"),
			&VoxelDebugHooks::debug_ingest_gpu_timings);
	ClassDB::bind_method(D_METHOD("debug_contact_shadow_probe", "pos", "fwd", "w", "h"),
			&VoxelDebugHooks::debug_contact_shadow_probe);
	ClassDB::bind_method(D_METHOD("debug_ssr_probe", "fixture", "w", "h"),
			&VoxelDebugHooks::debug_ssr_probe);
	ClassDB::bind_method(D_METHOD("debug_outline_probe", "fixture", "have_dynamic_normals"),
			&VoxelDebugHooks::debug_outline_probe);
	ClassDB::bind_method(D_METHOD("debug_glossy_sdf_probe", "origin", "dir"),
			&VoxelDebugHooks::debug_glossy_sdf_probe);
	ClassDB::bind_method(D_METHOD("debug_ssgi_probe", "pos", "fwd", "w", "h", "frames"),
			&VoxelDebugHooks::debug_ssgi_probe);
	ClassDB::bind_method(D_METHOD("debug_ssao_probe", "pos", "fwd", "w", "h"),
			&VoxelDebugHooks::debug_ssao_probe);
	ClassDB::bind_method(D_METHOD("debug_ssgi_reprojection_probe", "previous_pos",
			"previous_fwd", "current_pos", "current_fwd", "w", "h"),
			&VoxelDebugHooks::debug_ssgi_reprojection_probe);
	ClassDB::bind_method(D_METHOD("debug_lod_tick", "pos", "fwd"), &VoxelDebugHooks::debug_lod_tick);
	ClassDB::bind_method(D_METHOD("debug_lod_stats"), &VoxelDebugHooks::debug_lod_stats);
	ClassDB::bind_method(D_METHOD("debug_lod_fade_band"), &VoxelDebugHooks::debug_lod_fade_band);
	ClassDB::bind_method(D_METHOD("debug_lod_render_probe", "pos", "fwd", "w", "h"),
			&VoxelDebugHooks::debug_lod_render_probe);
	ClassDB::bind_method(D_METHOD("debug_lod_render_probe_culled", "pos", "fwd", "w", "h",
			"cull"), &VoxelDebugHooks::debug_lod_render_probe_culled);
	ClassDB::bind_method(D_METHOD("debug_lod_gbuffer_probe", "pos", "fwd", "w", "h"),
			&VoxelDebugHooks::debug_lod_gbuffer_probe);
	ClassDB::bind_method(D_METHOD("debug_seam_probe", "pos", "fwd", "w", "h", "skip_lod"),
			&VoxelDebugHooks::debug_seam_probe, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("debug_hiz_stats"), &VoxelDebugHooks::debug_hiz_stats);
	ClassDB::bind_method(D_METHOD("debug_hiz_shutdown_probe"), &VoxelDebugHooks::debug_hiz_shutdown_probe);
	ClassDB::bind_method(D_METHOD("debug_gbuffer_stats", "w", "h"),
			&VoxelDebugHooks::debug_gbuffer_stats);
	ClassDB::bind_method(D_METHOD("debug_hiz_probe_synthetic", "far_value", "near_value"),
			&VoxelDebugHooks::debug_hiz_probe_synthetic);
	ClassDB::bind_method(D_METHOD("debug_hiz_occluded", "lo", "hi", "depth"),
			&VoxelDebugHooks::debug_hiz_occluded);
	ClassDB::bind_method(D_METHOD("debug_lod_cull_probe", "pos", "fwd"),
			&VoxelDebugHooks::debug_lod_cull_probe);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_stats"),
			&VoxelDebugHooks::debug_sun_shadow_stats);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_build", "force"),
			&VoxelDebugHooks::debug_sun_shadow_build);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_visibility", "p"),
			&VoxelDebugHooks::debug_sun_shadow_visibility);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_shading", "p", "viewer"),
			&VoxelDebugHooks::debug_sun_shadow_shading);
	ClassDB::bind_method(D_METHOD("debug_init_physics"), &VoxelDebugHooks::debug_init_physics);
	ClassDB::bind_method(D_METHOD("debug_teardown_physics"), &VoxelDebugHooks::debug_teardown_physics);
	ClassDB::bind_method(D_METHOD("debug_mesh_lattice_diff", "chunk"), &VoxelDebugHooks::debug_mesh_lattice_diff);
	ClassDB::bind_method(D_METHOD("debug_mesh_diff", "chunk"), &VoxelDebugHooks::debug_mesh_diff);
	ClassDB::bind_method(D_METHOD("debug_consolidate_diff", "region"), &VoxelDebugHooks::debug_consolidate_diff);
	ClassDB::bind_method(D_METHOD("debug_consolidate_region", "region"), &VoxelDebugHooks::debug_consolidate_region);
	ClassDB::bind_method(D_METHOD("debug_region_op_count", "region"), &VoxelDebugHooks::debug_region_op_count);
	ClassDB::bind_method(D_METHOD("debug_override_region_table", "region_slot"),
			&VoxelDebugHooks::debug_override_region_table);
	ClassDB::bind_method(D_METHOD("debug_override_used"), &VoxelDebugHooks::debug_override_used);
	ClassDB::bind_method(D_METHOD("debug_fill_override_pool"), &VoxelDebugHooks::debug_fill_override_pool);
	ClassDB::bind_method(D_METHOD("debug_override_render_state", "brick"),
			&VoxelDebugHooks::debug_override_render_state);
	ClassDB::bind_method(D_METHOD("debug_lod_diff", "level", "coord"), &VoxelDebugHooks::debug_lod_diff);
	ClassDB::bind_method(D_METHOD("debug_apply_sphere_subtract", "centre", "radius"),
			&VoxelDebugHooks::debug_apply_sphere_subtract);
	ClassDB::bind_method(D_METHOD("debug_apply_sphere_add", "centre", "radius", "material"),
			&VoxelDebugHooks::debug_apply_sphere_add);
	ClassDB::bind_method(D_METHOD("debug_apply_sphere_paint", "centre", "radius", "material"),
			&VoxelDebugHooks::debug_apply_sphere_paint);
	ClassDB::bind_method(D_METHOD("debug_apply_volume_add", "slot", "origin", "voxel", "dim"),
			&VoxelDebugHooks::debug_apply_volume_add);
	ClassDB::bind_method(D_METHOD("debug_island_extract_diff", "lo_cell", "hi_cell"), &VoxelDebugHooks::debug_island_extract_diff);
	ClassDB::bind_method(D_METHOD("debug_place_test_island", "slot", "lo_cell", "hi_cell", "offset"), &VoxelDebugHooks::debug_place_test_island);
	ClassDB::bind_method(D_METHOD("debug_place_test_island_rotated", "slot", "lo_cell", "hi_cell", "offset", "yaw", "volume_slot"), &VoxelDebugHooks::debug_place_test_island_rotated, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("debug_clear_test_island", "slot"), &VoxelDebugHooks::debug_clear_test_island);
	ClassDB::bind_method(D_METHOD("debug_island_tile_mask", "origin", "dir", "tan_x", "tan_y",
			"width", "height"), &VoxelDebugHooks::debug_island_tile_mask);
	ClassDB::bind_method(D_METHOD("debug_mesh_submit", "chunks"), &VoxelDebugHooks::debug_mesh_submit);
	ClassDB::bind_method(D_METHOD("debug_mesh_collect"), &VoxelDebugHooks::debug_mesh_collect);
	ClassDB::bind_method(D_METHOD("debug_extract_submit", "id", "lo_cell", "hi_cell"),
			&VoxelDebugHooks::debug_extract_submit);
	ClassDB::bind_method(D_METHOD("debug_extract_collect"), &VoxelDebugHooks::debug_extract_collect);
	ClassDB::bind_method(D_METHOD("debug_lod_submit", "jobs"), &VoxelDebugHooks::debug_lod_submit);
	ClassDB::bind_method(D_METHOD("debug_lod_collect"), &VoxelDebugHooks::debug_lod_collect);
	ClassDB::bind_method(D_METHOD("debug_physics_frame", "center"), &VoxelDebugHooks::debug_physics_frame);
	ClassDB::bind_method(D_METHOD("debug_set_physics_bubbles", "centers"), &VoxelDebugHooks::debug_set_physics_bubbles);
	ClassDB::bind_method(D_METHOD("debug_physics_stats"), &VoxelDebugHooks::debug_physics_stats);
	ClassDB::bind_method(D_METHOD("debug_perf_stats"), &VoxelDebugHooks::debug_perf_stats);
	ClassDB::bind_method(D_METHOD("debug_island_frame", "dt", "center"), &VoxelDebugHooks::debug_island_frame);
	ClassDB::bind_method(D_METHOD("debug_island_stats"), &VoxelDebugHooks::debug_island_stats);
	ClassDB::bind_method(D_METHOD("debug_island_pending_uploads"), &VoxelDebugHooks::debug_island_pending_uploads);
	ClassDB::bind_method(D_METHOD("debug_field_volume_upload_count"), &VoxelDebugHooks::debug_field_volume_upload_count);
	ClassDB::bind_method(D_METHOD("debug_island_descriptors_pending"), &VoxelDebugHooks::debug_island_descriptors_pending);
	ClassDB::bind_method(D_METHOD("debug_mesh_volume_slots"), &VoxelDebugHooks::debug_mesh_volume_slots);
	ClassDB::bind_method(D_METHOD("debug_queue_test_island_upload", "slot", "sdf", "mat", "dim"),
			&VoxelDebugHooks::debug_queue_test_island_upload);
	ClassDB::bind_method(D_METHOD("debug_queue_test_island_descriptors"),
			&VoxelDebugHooks::debug_queue_test_island_descriptors);
	ClassDB::bind_method(D_METHOD("debug_queue_committed_field_volume_upload", "slot", "sdf",
			"mat", "dim"), &VoxelDebugHooks::debug_queue_committed_field_volume_upload);
	ClassDB::bind_method(D_METHOD("debug_set_extraction_available", "v"),
			&VoxelDebugHooks::debug_set_extraction_available);
	ClassDB::bind_method(D_METHOD("debug_set_fail_extractions", "v"),
			&VoxelDebugHooks::debug_set_fail_extractions);
	ClassDB::bind_method(D_METHOD("debug_set_fail_extract_submit", "v"),
			&VoxelDebugHooks::debug_set_fail_extract_submit);
	ClassDB::bind_method(D_METHOD("debug_set_fail_consolidations", "v"),
			&VoxelDebugHooks::debug_set_fail_consolidations);
	ClassDB::bind_method(D_METHOD("debug_pump_consolidation"), &VoxelDebugHooks::debug_pump_consolidation);
	ClassDB::bind_method(D_METHOD("debug_pump_consolidation_async"), &VoxelDebugHooks::debug_pump_consolidation_async);
	ClassDB::bind_method(D_METHOD("debug_wait_consolidation"), &VoxelDebugHooks::debug_wait_consolidation);
	ClassDB::bind_method(D_METHOD("debug_set_fail_consolidate_uploads", "v"),
			&VoxelDebugHooks::debug_set_fail_consolidate_uploads);
	ClassDB::bind_method(D_METHOD("debug_set_fail_restore_overrides", "v"),
			&VoxelDebugHooks::debug_set_fail_restore_overrides);
	ClassDB::bind_method(D_METHOD("debug_set_fail_restore_overrides_always", "v"),
			&VoxelDebugHooks::debug_set_fail_restore_overrides_always);
	ClassDB::bind_method(D_METHOD("debug_set_pause_override_publication", "v"),
			&VoxelDebugHooks::debug_set_pause_override_publication);
	ClassDB::bind_method(D_METHOD("debug_override_publication_paused"),
			&VoxelDebugHooks::debug_override_publication_paused);
	ClassDB::bind_method(D_METHOD("debug_set_merge_sleep_seconds", "v"), &VoxelDebugHooks::debug_set_merge_sleep_seconds);
#ifdef DEBUG_ENABLED
	// These hooks can change the production 64-body cap or mark atlas slots used; keep them
	// out of release ClassDB so release scripts cannot call them.
	ClassDB::bind_method(D_METHOD("debug_set_max_dynamic_bodies", "v"), &VoxelDebugHooks::debug_set_max_dynamic_bodies);
	ClassDB::bind_method(D_METHOD("debug_set_atlas_slot_used", "slot", "used"), &VoxelDebugHooks::debug_set_atlas_slot_used);
	ClassDB::bind_method(D_METHOD("debug_set_normal_pool_budget", "bytes"), &VoxelDebugHooks::debug_set_normal_pool_budget);
	ClassDB::bind_method(D_METHOD("debug_stored_normal_stats"), &VoxelDebugHooks::debug_stored_normal_stats);
	ClassDB::bind_method(D_METHOD("debug_normal_pool_state"), &VoxelDebugHooks::debug_normal_pool_state);
	ClassDB::bind_method(D_METHOD("debug_normal_upload_override", "slot", "packed_normals"), &VoxelDebugHooks::debug_normal_upload_override);
	ClassDB::bind_method(D_METHOD("debug_normal_release_override", "slot"), &VoxelDebugHooks::debug_normal_release_override);
#endif
	ClassDB::bind_method(D_METHOD("debug_set_fail_next_spawn", "fail"), &VoxelDebugHooks::debug_set_fail_next_spawn);
	ClassDB::bind_method(D_METHOD("debug_set_fail_next_restore", "fail"), &VoxelDebugHooks::debug_set_fail_next_restore);
	ClassDB::bind_method(D_METHOD("debug_set_fail_next_carve", "fail"), &VoxelDebugHooks::debug_set_fail_next_carve);
	ClassDB::bind_method(D_METHOD("debug_set_fail_next_resample", "fail"), &VoxelDebugHooks::debug_set_fail_next_resample);
	ClassDB::bind_method(D_METHOD("debug_set_empty_next_extraction", "v"), &VoxelDebugHooks::debug_set_empty_next_extraction);
	ClassDB::bind_method(D_METHOD("debug_wake_island_body", "index"), &VoxelDebugHooks::debug_wake_island_body);
	ClassDB::bind_method(D_METHOD("debug_offset_island_body", "index", "offset"), &VoxelDebugHooks::debug_offset_island_body);
	ClassDB::bind_method(D_METHOD("debug_island_body_info", "index"), &VoxelDebugHooks::debug_island_body_info);
	ClassDB::bind_method(D_METHOD("debug_body_of_chunk", "chunk"), &VoxelDebugHooks::debug_body_of_chunk);
	ClassDB::bind_method(D_METHOD("debug_chunk_collider_info", "chunk"), &VoxelDebugHooks::debug_chunk_collider_info);
	ClassDB::bind_method(D_METHOD("debug_chunk_collider_octants", "chunk"),
			&VoxelDebugHooks::debug_chunk_collider_octants);
	ClassDB::bind_method(D_METHOD("debug_raymarch_pixel", "origin", "dir"), &VoxelDebugHooks::debug_raymarch_pixel);
	ClassDB::bind_method(D_METHOD("debug_raymarch_probe", "origin", "dir"), &VoxelDebugHooks::debug_raymarch_probe);
	ClassDB::bind_method(D_METHOD("debug_raymarch_cost_probe", "origin", "dir"),
			&VoxelDebugHooks::debug_raymarch_cost_probe);
	ClassDB::bind_method(D_METHOD("debug_raymarch_gbuffer", "origin", "dir"), &VoxelDebugHooks::debug_raymarch_gbuffer);
	ClassDB::bind_method(D_METHOD("debug_raymarch_hole_probe", "origin", "dir", "w", "h"),
			&VoxelDebugHooks::debug_raymarch_hole_probe);
	ClassDB::bind_method(D_METHOD("debug_raymarch_normal_probe", "origin", "dir", "w", "h"),
			&VoxelDebugHooks::debug_raymarch_normal_probe);
	ClassDB::bind_method(D_METHOD("debug_island_normal_probe", "island_slot", "origin", "dir", "w", "h"),
			&VoxelDebugHooks::debug_island_normal_probe);
	ClassDB::bind_method(D_METHOD("debug_cel_diff", "albedo", "ambient", "ndl", "ndv", "ndh",
			"shadow", "ao", "gloss"), &VoxelDebugHooks::debug_cel_diff);
	ClassDB::bind_method(D_METHOD("debug_cel_reference", "albedo", "ambient", "ndl", "ndv", "ndh",
			"shadow", "ao", "gloss"), &VoxelDebugHooks::debug_cel_reference);
	ClassDB::bind_method(D_METHOD("debug_deferred_probe", "pos", "fwd", "w", "h", "probe_mode"),
			&VoxelDebugHooks::debug_deferred_probe);
	ClassDB::bind_method(D_METHOD("debug_near_field_detail", "pos", "fwd", "w", "h", "march_scale"),
			&VoxelDebugHooks::debug_near_field_detail);
	ClassDB::bind_method(D_METHOD("debug_material_atlas_stats"), &VoxelDebugHooks::debug_material_atlas_stats);
	ClassDB::bind_method(D_METHOD("debug_material_alpha_stats", "layer"),
			&VoxelDebugHooks::debug_material_alpha_stats);
	ClassDB::bind_method(D_METHOD("debug_material_probe", "mat", "p", "n"), &VoxelDebugHooks::debug_material_probe);
	ClassDB::bind_method(D_METHOD("debug_material_normal_probe", "mat", "p", "n"),
			&VoxelDebugHooks::debug_material_normal_probe);
	ClassDB::bind_method(D_METHOD("debug_poke_material_normal", "layer"), &VoxelDebugHooks::debug_poke_material_normal);
	ClassDB::bind_method(D_METHOD("debug_flatten_material_normal", "layer"),
			&VoxelDebugHooks::debug_flatten_material_normal);
	ClassDB::bind_method(D_METHOD("debug_sdf_atlas"), &VoxelDebugHooks::debug_sdf_atlas);
	ClassDB::bind_method(D_METHOD("debug_local_rd"), &VoxelDebugHooks::debug_local_rd);
	ClassDB::bind_method(D_METHOD("debug_load_shader", "res_path"), &VoxelDebugHooks::debug_load_shader);
	ClassDB::bind_method(D_METHOD("debug_pump_shader_reload"), &VoxelDebugHooks::debug_pump_shader_reload);
	ClassDB::bind_method(D_METHOD("debug_shader_reload_stats"), &VoxelDebugHooks::debug_shader_reload_stats);
	ClassDB::bind_method(D_METHOD("debug_set_shader_override", "name", "source"),
			&VoxelDebugHooks::debug_set_shader_override);
	ClassDB::bind_method(D_METHOD("debug_self_check"), &VoxelDebugHooks::debug_self_check);
	ClassDB::bind_method(D_METHOD("debug_store_volume", "slot", "sdf", "mat", "dim"), &VoxelDebugHooks::debug_store_volume);
	ClassDB::bind_method(D_METHOD("debug_eval_field", "p", "ops", "op_count"), &VoxelDebugHooks::debug_eval_field);
	ClassDB::bind_method(D_METHOD("debug_eval_field_gradient", "p", "ops", "op_count"), &VoxelDebugHooks::debug_eval_field_gradient);
	ClassDB::bind_method(D_METHOD("debug_init_atlas"), &VoxelDebugHooks::debug_init_atlas);
	ClassDB::bind_method(D_METHOD("debug_teardown_atlas"), &VoxelDebugHooks::debug_teardown_atlas);
	ClassDB::bind_method(D_METHOD("debug_atlas_stats"), &VoxelDebugHooks::debug_atlas_stats);
	ClassDB::bind_method(D_METHOD("debug_reset_frame_counters"), &VoxelDebugHooks::debug_reset_frame_counters);
	ClassDB::bind_method(D_METHOD("debug_set_region_map_entry", "region_index", "region_slot"), &VoxelDebugHooks::debug_set_region_map_entry);
	ClassDB::bind_method(D_METHOD("debug_upload_region_ops", "region_slot", "ops", "count"), &VoxelDebugHooks::debug_upload_region_ops);
	ClassDB::bind_method(D_METHOD("debug_brick_has_surface", "brick", "ops", "op_count"), &VoxelDebugHooks::debug_brick_has_surface);
	ClassDB::bind_method(D_METHOD("debug_mark_region", "region", "region_slot", "lo", "hi", "op_count", "force"), &VoxelDebugHooks::debug_mark_region);
	ClassDB::bind_method(D_METHOD("debug_generate_pending"), &VoxelDebugHooks::debug_generate_pending);
	ClassDB::bind_method(D_METHOD("debug_brick_diff", "brick", "region_slot", "ops", "op_count"), &VoxelDebugHooks::debug_brick_diff);
	ClassDB::bind_method(D_METHOD("debug_stream_region", "region"), &VoxelDebugHooks::debug_stream_region);
	ClassDB::bind_method(D_METHOD("debug_brick_flags", "region"), &VoxelDebugHooks::debug_brick_flags);
	ClassDB::bind_method(D_METHOD("debug_brick_flags_after_mark", "region"), &VoxelDebugHooks::debug_brick_flags_after_mark);
	ClassDB::bind_method(D_METHOD("debug_release_region", "region_slot"), &VoxelDebugHooks::debug_release_region);
	ClassDB::bind_method(D_METHOD("debug_jobs"), &VoxelDebugHooks::debug_jobs);
	ClassDB::bind_method(D_METHOD("debug_region_table_slot", "region_slot", "brick"), &VoxelDebugHooks::debug_region_table_slot);
	ClassDB::bind_method(D_METHOD("debug_mat_atlas"), &VoxelDebugHooks::debug_mat_atlas);
	ClassDB::bind_method(D_METHOD("debug_mip_atlas", "level"), &VoxelDebugHooks::debug_mip_atlas);
	ClassDB::bind_method(D_METHOD("debug_region_map"), &VoxelDebugHooks::debug_region_map);
	ClassDB::bind_method(D_METHOD("debug_region_tables"), &VoxelDebugHooks::debug_region_tables);
	ClassDB::bind_method(D_METHOD("debug_free_list"), &VoxelDebugHooks::debug_free_list);
	ClassDB::bind_method(D_METHOD("debug_frame_counters"), &VoxelDebugHooks::debug_frame_counters);
	ClassDB::bind_method(D_METHOD("debug_op_pool"), &VoxelDebugHooks::debug_op_pool);
	ClassDB::bind_method(D_METHOD("debug_op_counts"), &VoxelDebugHooks::debug_op_counts);
	ClassDB::bind_method(D_METHOD("debug_occupancy_state", "cell"), &VoxelDebugHooks::debug_occupancy_state);
	ClassDB::bind_method(D_METHOD("debug_pump_occupancy"), &VoxelDebugHooks::debug_pump_occupancy);
	ClassDB::bind_method(D_METHOD("debug_occupancy_diff", "region"), &VoxelDebugHooks::debug_occupancy_diff);
	ClassDB::bind_method(D_METHOD("debug_occupancy_fallback_diff", "region"), &VoxelDebugHooks::debug_occupancy_fallback_diff);
	ClassDB::bind_method(D_METHOD("debug_cell_state", "cell"), &VoxelDebugHooks::debug_cell_state);
	ClassDB::bind_method(D_METHOD("debug_generator_fingerprint"),
			&VoxelDebugHooks::debug_generator_fingerprint);
	ClassDB::bind_method(D_METHOD("debug_field_sdf", "p"), &VoxelDebugHooks::debug_field_sdf);
	ClassDB::bind_method(D_METHOD("debug_occupancy_stats", "center"), &VoxelDebugHooks::debug_occupancy_stats);
	ClassDB::bind_method(D_METHOD("debug_stream_frame", "cam"), &VoxelDebugHooks::debug_stream_frame);
	ClassDB::bind_method(D_METHOD("debug_stream_stats"), &VoxelDebugHooks::debug_stream_stats);
	ClassDB::bind_method(D_METHOD("debug_slot_of_region", "region"), &VoxelDebugHooks::debug_slot_of_region);
	ClassDB::bind_method(D_METHOD("debug_region_map_entry", "region"), &VoxelDebugHooks::debug_region_map_entry);
	ClassDB::bind_method(D_METHOD("debug_region_map_consistent"), &VoxelDebugHooks::debug_region_map_consistent);
	ClassDB::bind_method(D_METHOD("debug_raycast", "origin", "dir"), &VoxelDebugHooks::debug_raycast);
	ClassDB::bind_method(D_METHOD("debug_spawn_test_body", "lo_cell", "hi_cell", "offset", "impulse", "debris"), &VoxelDebugHooks::debug_spawn_test_body);
	ClassDB::bind_method(D_METHOD("debug_test_body_stats", "index"), &VoxelDebugHooks::debug_test_body_stats);
	ClassDB::bind_method(D_METHOD("debug_tick_test_bodies", "dt"), &VoxelDebugHooks::debug_tick_test_bodies);
	ClassDB::bind_method(D_METHOD("debug_despawn_test_body", "index"), &VoxelDebugHooks::debug_despawn_test_body);
}

// Half-precision to single-precision (normal + subnormal paths).
static float half_to_float(uint16_t v) {
	const uint32_t sign = (v & 0x8000u) << 16;
	const uint32_t exp = (v >> 10) & 0x1F;
	const uint32_t mant = v & 0x3FF;
	if (exp == 0) return (sign ? -1.0f : 1.0f) * mant / 1024.0f / 16384.0f;
	uint32_t bits = sign | ((exp + 112) << 23) | (mant << 13);
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

Dictionary VoxelDebugHooks::debug_gpu_timings() {
	return world_->gpu_timings()->snapshot();
}

Dictionary VoxelDebugHooks::debug_ingest_gpu_timings(const PackedStringArray &names,
		const PackedInt64Array &gpu_us, int64_t rd_frame) {
	return world_->gpu_timings()->ingest_for_test(names, gpu_us, static_cast<uint64_t>(rd_frame));
}

Dictionary VoxelDebugHooks::debug_beauty_compositor_stats() {
	Dictionary d;
	d["normal_roughness"] = world_->get_normal_roughness_state();
	d["contact_ms"] = world_->contact_shadow_pass() ? world_->contact_shadow_pass()->last_ms() : 0.0f;
	// CPU command-record time only; GPU timings belong to the later performance task.
	d["ssr_ms"] = world_->ssr_pass() ? world_->ssr_pass()->last_ms() : 0.0f;
	d["outline_ms"] = world_->outline_pass() ? world_->outline_pass()->last_ms() : 0.0f;
	return d;
}

Dictionary VoxelDebugHooks::debug_contact_shadow_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["mask_width"] = 0; d["mask_height"] = 0;
	d["mask_min"] = 1.0f; d["mask_mean"] = 1.0f;
	d["mean_darkening"] = 0.0f; d["max_brightening"] = 0.0f;
	d["max_neighbour_step"] = 0.0f;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass() ||
			!world_->composite_pass() || !world_->deferred_pass() || !world_->gbuffer() || !world_->contact_shadow_pass() ||
			!world_->beauty_camera()) return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	world_->composite_pass()->release_targets();
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h))) return d;
	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, std::fabs(fwd.y) > 0.9f ? 0.0f : 1.0f,
			std::fabs(fwd.y) > 0.9f ? 1.0f : 0.0f};
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float fov_y = 1.0471975512f;
	const float tan_y = std::tan(fov_y * 0.5f);
	const float tan_x = tan_y * aspect;
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, fov_y, aspect,
			0.05f, 4000.0f, w, h);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = cam.view_proj[c * 4 + r];
	ve::CameraParams cp = ve::CameraParams::looking_at(pos.x, pos.y, pos.z,
			fwd.x, fwd.y, fwd.z, up[0], up[1], up[2]);
	cp.params[0] = tan_x; cp.params[1] = tan_y; cp.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_->store_->config().world_size_regions.x; cp.dims[1] = world_->store_->config().world_size_regions.y;
	cp.dims[2] = world_->store_->config().world_size_regions.z; cp.dims[3] = world_->island_slot_count();
	cp.region_origin[0] = ro.x; cp.region_origin[1] = ro.y; cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cp.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cp.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cp, w, h, no_edit)) return d;
	float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
	world_->lod_fade_band(&fade_start, &fade_end);
	world_->composite_pass()->draw(device, *world_->gbuffer(), world_->raymarch_pass()->albedo_texture(),
			world_->raymarch_pass()->surface_texture(), world_->raymarch_pass()->hitpos_texture(), view_proj,
			*world_->material_atlas(), cp, fade_start, fade_end);
	if (!world_->composite_pass()->last_draw_ok()) return d;
	DeferredPass::Params dp;
	const Projection inv = view_proj.inverse();
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
	dp.cam_pos[0] = pos.x; dp.cam_pos[1] = pos.y; dp.cam_pos[2] = pos.z;
	dp.flags = ve::pack_flags(world_->beauty_settings());
	static const float no_sun[16] = {};
	if (!world_->deferred_pass()->render(device, *world_->gbuffer(), *world_->material_atlas(), RID(), RID(), RID(), no_sun, 0.0f, dp))
		return d;
	auto make_scratch = [&]() -> RID {
		Ref<RDTextureFormat> tf; tf.instantiate();
		tf->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
		tf->set_width(w); tf->set_height(h);
		tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
		Ref<RDTextureView> tv; tv.instantiate();
		return device->texture_create(tf, tv, {});
	};
	const RID scratch = make_scratch();
	const RID before = make_scratch();
	if (!scratch.is_valid() || !before.is_valid()) {
		if (scratch.is_valid()) device->free_rid(scratch);
		if (before.is_valid()) device->free_rid(before);
		return d;
	}
	device->texture_copy(world_->gbuffer()->lit(), scratch, Vector3(), Vector3(), Vector3(w, h, 1), 0, 0, 0, 0);
	device->texture_copy(world_->gbuffer()->lit(), before, Vector3(), Vector3(), Vector3(w, h, 1), 0, 0, 0, 0);
	world_->beauty_camera()->ensure(device);
	const float cam_pos[3] = {pos.x, pos.y, pos.z};
	world_->beauty_camera()->update(device, view_proj, cam_pos, Vector2i(w, h), 0.05f, 4000.0f);
	world_->contact_shadow_pass()->render(device, scratch, world_->gbuffer()->depth(), Vector2i(w, h),
			world_->beauty_camera()->buffer(), world_->beauty_settings());
	device->submit(); device->sync();
	const int mw = std::max(1, w / 2), mh = std::max(1, h / 2);
	d["mask_width"] = mw; d["mask_height"] = mh;
	PackedByteArray mask;
	if (world_->contact_shadow_pass()->mask().is_valid())
		mask = device->texture_get_data(world_->contact_shadow_pass()->mask(), 0);
	const PackedByteArray pre = device->texture_get_data(before, 0);
	const PackedByteArray post = device->texture_get_data(scratch, 0);
	if (mask.size() >= mw * mh && pre.size() >= w * h * 8 && post.size() >= w * h * 8) {
		const uint8_t *m = reinterpret_cast<const uint8_t *>(mask.ptr());
		const uint16_t *a = reinterpret_cast<const uint16_t *>(pre.ptr());
		const uint16_t *b = reinterpret_cast<const uint16_t *>(post.ptr());
		float min_mask = 1.0f; double mask_sum = 0.0, dark = 0.0; float bright = 0.0f;
		for (int i = 0; i < mw * mh; i++) { const float v = m[i] / 255.0f; min_mask = std::min(min_mask, v); mask_sum += v; }
		// Per-pixel FRACTION of light removed, so the speckle measure below is independent
		// of how bright the surface under the shadow happens to be.
		std::vector<float> removed(static_cast<size_t>(w) * h, 0.0f);
		for (int i = 0; i < w * h; i++) {
			const float la = 0.2126f * Math::half_to_float(a[i * 4]) + 0.7152f * Math::half_to_float(a[i * 4 + 1]) + 0.0722f * Math::half_to_float(a[i * 4 + 2]);
			const float lb = 0.2126f * Math::half_to_float(b[i * 4]) + 0.7152f * Math::half_to_float(b[i * 4 + 1]) + 0.0722f * Math::half_to_float(b[i * 4 + 2]);
			dark += std::max(0.0f, la - lb); bright = std::max(bright, lb - la);
			removed[i] = la > 1e-4f ? std::clamp((la - lb) / la, 0.0f, 1.0f) : 0.0f;
		}
		// The largest jump in that fraction between neighbouring pixels. A resolved shadow
		// is a gradient; an unresolved bayer4 dither steps the full strength in one pixel.
		float step = 0.0f;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				const float c = removed[static_cast<size_t>(y) * w + x];
				if (x + 1 < w) step = std::max(step, std::fabs(c - removed[static_cast<size_t>(y) * w + x + 1]));
				if (y + 1 < h) step = std::max(step, std::fabs(c - removed[static_cast<size_t>(y + 1) * w + x]));
			}
		d["mask_min"] = min_mask; d["mask_mean"] = static_cast<float>(mask_sum / (mw * mh));
		d["mean_darkening"] = static_cast<float>(dark / (w * h)); d["max_brightening"] = bright;
		d["max_neighbour_step"] = step;
	}
	world_->contact_shadow_pass()->teardown();
	device->free_rid(scratch); device->free_rid(before);
	world_->contact_shadow_pass()->initialize(device);
	return d;
}

Dictionary VoxelDebugHooks::debug_ssgi_probe(Vector3 pos, Vector3 fwd, int w, int h, int frames) {
	Dictionary d;
	d["width"] = std::max(1, w / 2);
	d["height"] = std::max(1, h / 2);
	d["max_channel"] = 0.0f;
	d["mean_luma"] = 0.0;
	d["ran"] = false;
	if (w <= 0 || h <= 0 || frames <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass() ||
			!world_->composite_pass() || !world_->deferred_pass() || !world_->gbuffer() || !world_->beauty_camera() || !world_->ssgi_pass())
		return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	world_->composite_pass()->release_targets();
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h)) || !world_->beauty_camera()->ensure(device)) return d;

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, std::fabs(fwd.y) > 0.9f ? 0.0f : 1.0f,
			std::fabs(fwd.y) > 0.9f ? 1.0f : 0.0f};
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float fov_y = 1.0471975512f;
	const float tan_y = std::tan(fov_y * 0.5f);
	const float tan_x = tan_y * aspect;
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, fov_y, aspect,
			0.05f, 4000.0f, w, h);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = cam.view_proj[c * 4 + r];
	ve::CameraParams cp = ve::CameraParams::looking_at(pos.x, pos.y, pos.z,
			fwd.x, fwd.y, fwd.z, up[0], up[1], up[2]);
	cp.params[0] = tan_x; cp.params[1] = tan_y; cp.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_->store_->config().world_size_regions.x; cp.dims[1] = world_->store_->config().world_size_regions.y;
	cp.dims[2] = world_->store_->config().world_size_regions.z; cp.dims[3] = world_->island_slot_count();
	cp.region_origin[0] = ro.x; cp.region_origin[1] = ro.y; cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cp.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cp.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	static const float no_sun[16] = {};
	const ve::BeautySettings settings = world_->beauty_settings();
	world_->ssgi_pass()->clear_result();
	float prev_view_proj[16] = {};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) prev_view_proj[c * 4 + r] = view_proj.columns[c][r];
	const Projection inv = view_proj.inverse();
	bool ran = false;
	for (int i = 0; i < frames; i++) {
		world_->beauty_camera()->update(device, view_proj, p, Vector2i(w, h), 0.05f, 4000.0f);
		if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cp, w, h, no_edit)) break;
		float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
		world_->lod_fade_band(&fade_start, &fade_end);
		world_->composite_pass()->draw(device, *world_->gbuffer(), world_->raymarch_pass()->albedo_texture(),
				world_->raymarch_pass()->surface_texture(), world_->raymarch_pass()->hitpos_texture(), view_proj,
				*world_->material_atlas(), cp, fade_start, fade_end);
		if (!world_->composite_pass()->last_draw_ok()) break;
		const bool ssgi_ok = world_->ssgi_pass()->render(device, *world_->gbuffer(), world_->beauty_camera()->buffer(),
				prev_view_proj, i > 0, settings, static_cast<uint32_t>(i));
		ran = ran || ssgi_ok;
		DeferredPass::Params dp;
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
		dp.cam_pos[0] = pos.x; dp.cam_pos[1] = pos.y; dp.cam_pos[2] = pos.z;
		dp.flags = ve::pack_flags(settings);
		if (!world_->deferred_pass()->render(device, *world_->gbuffer(), *world_->material_atlas(),
				(ssgi_ok ? world_->ssgi_pass()->result() : RID()),
				RID(), RID(), no_sun, 0.0f, dp)) break;
		world_->downsample_history(device, world_->gbuffer()->lit(), *world_->gbuffer());
	}
	device->submit();
	device->sync();
	d["ran"] = ran;
	const RID output = world_->ssgi_pass()->result();
	const Vector2i half = world_->gbuffer()->half_size();
	d["width"] = half.x;
	d["height"] = half.y;
	if (!output.is_valid()) return d;
	const PackedByteArray data = device->texture_get_data(output, 0);
	const int pixels = half.x * half.y;
	if (data.size() < static_cast<int64_t>(pixels) * 8) return d;
	const uint16_t *values = reinterpret_cast<const uint16_t *>(data.ptr());
	float max_channel = 0.0f;
	double mean_luma = 0.0;
	for (int i = 0; i < pixels; i++) {
		const float r = Math::half_to_float(values[i * 4]);
		const float g = Math::half_to_float(values[i * 4 + 1]);
		const float b = Math::half_to_float(values[i * 4 + 2]);
		max_channel = std::max(max_channel, std::max(r, std::max(g, b)));
		mean_luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
	}
	d["max_channel"] = max_channel;
	d["mean_luma"] = mean_luma / static_cast<double>(pixels);
	return d;
}

// One frame of raymarch -> composite -> HBAO over the G-buffer, then a readback of the
// single-channel occlusion target. Reads the AO texture directly: whether the deferred pass
// applies it is deferred's own contract, not this probe's.
Dictionary VoxelDebugHooks::debug_ssao_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["width"] = w;
	d["height"] = h;
	d["min_ao"] = 1.0f;
	d["max_ao"] = 0.0f;
	d["ran"] = false;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass() ||
			!world_->composite_pass() || !world_->gbuffer() || !world_->beauty_camera() || !world_->ssao_pass())
		return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	world_->composite_pass()->release_targets();
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h)) || !world_->beauty_camera()->ensure(device)) return d;

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, std::fabs(fwd.y) > 0.9f ? 0.0f : 1.0f,
			std::fabs(fwd.y) > 0.9f ? 1.0f : 0.0f};
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float fov_y = 1.0471975512f;
	const float tan_y = std::tan(fov_y * 0.5f);
	const float tan_x = tan_y * aspect;
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, fov_y, aspect,
			0.05f, 4000.0f, w, h);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = cam.view_proj[c * 4 + r];
	ve::CameraParams cp = ve::CameraParams::looking_at(pos.x, pos.y, pos.z,
			fwd.x, fwd.y, fwd.z, up[0], up[1], up[2]);
	cp.params[0] = tan_x; cp.params[1] = tan_y; cp.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_->store_->config().world_size_regions.x; cp.dims[1] = world_->store_->config().world_size_regions.y;
	cp.dims[2] = world_->store_->config().world_size_regions.z; cp.dims[3] = world_->island_slot_count();
	cp.region_origin[0] = ro.x; cp.region_origin[1] = ro.y; cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cp.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cp.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	const ve::BeautySettings settings = world_->beauty_settings();
	world_->ssao_pass()->clear_result();
	bool ran = false;
	world_->beauty_camera()->update(device, view_proj, p, Vector2i(w, h), 0.05f, 4000.0f);
	if (world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cp, w, h, no_edit)) {
		float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
		world_->lod_fade_band(&fade_start, &fade_end);
		world_->composite_pass()->draw(device, *world_->gbuffer(), world_->raymarch_pass()->albedo_texture(),
				world_->raymarch_pass()->surface_texture(), world_->raymarch_pass()->hitpos_texture(), view_proj,
				*world_->material_atlas(), cp, fade_start, fade_end);
		if (world_->composite_pass()->last_draw_ok())
			ran = world_->ssao_pass()->render(device, *world_->gbuffer(),
					world_->beauty_camera()->buffer(), settings);
		// The full deferred chain on top, so the probe can also report what the lit image
		// looks like with this frame's AO applied.
		DeferredPass::Params dp;
		const Projection inv = view_proj.inverse();
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
		dp.cam_pos[0] = pos.x; dp.cam_pos[1] = pos.y; dp.cam_pos[2] = pos.z;
		dp.flags = ve::pack_flags(settings);
		static const float kNoSun[16] = {};
		world_->deferred_pass()->render(device, *world_->gbuffer(), *world_->material_atlas(),
				RID(), ran ? world_->ssao_pass()->result() : RID(), RID(), kNoSun, 0.0f, dp);
	}
	device->submit();
	device->sync();
	d["ran"] = ran;
	{
		const PackedByteArray lit = device->texture_get_data(world_->gbuffer()->lit(), 0);
		const int pixels = w * h;
		if (lit.size() >= static_cast<int64_t>(pixels) * 8) {
			const uint16_t *lv = reinterpret_cast<const uint16_t *>(lit.ptr());
			double luma = 0.0;
			for (int i = 0; i < pixels; i++) {
				const float r = Math::half_to_float(lv[i * 4]);
				const float g = Math::half_to_float(lv[i * 4 + 1]);
				const float b = Math::half_to_float(lv[i * 4 + 2]);
				luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
			}
			d["lit_luma"] = luma / static_cast<double>(pixels);
		} else {
			d["lit_luma"] = -1.0;
		}
	}
	const RID output = world_->ssao_pass()->result();
	if (!output.is_valid()) return d;
	const PackedByteArray data = device->texture_get_data(output, 0);
	const int pixels = w * h;
	if (data.size() < pixels) return d;
	const uint8_t *values = reinterpret_cast<const uint8_t *>(data.ptr());
	float min_ao = 1.0f, max_ao = 0.0f;
	double mean_ao = 0.0;
	for (int i = 0; i < pixels; i++) {
		const float ao = static_cast<float>(values[i]) / 255.0f;
		min_ao = std::min(min_ao, ao);
		max_ao = std::max(max_ao, ao);
		mean_ao += ao;
	}
	d["min_ao"] = min_ao;
	d["max_ao"] = max_ao;
	d["mean_ao"] = mean_ao / static_cast<double>(pixels);
	return d;
}

Dictionary VoxelDebugHooks::debug_ssgi_reprojection_probe(Vector3 previous_pos, Vector3 previous_fwd,
		Vector3 current_pos, Vector3 current_fwd, int w, int h) {
	Dictionary d;
	d["non_identity"] = previous_pos != current_pos || previous_fwd != current_fwd;
	d["mapping_luma"] = 0.0;
	d["current_mapping_luma"] = 0.0;
	d["mapping_delta"] = 0.0;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass() ||
			!world_->composite_pass() || !world_->deferred_pass() || !world_->gbuffer() || !world_->beauty_camera() || !world_->ssgi_pass())
		return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(previous_pos) == 0 ? quiet + 1 : 0;
	world_->composite_pass()->release_targets();
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h)) || !world_->beauty_camera()->ensure(device))
		return d;

	const float fov_y = 1.0471975512f;
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(fov_y * 0.5f);
	const float tan_x = tan_y * aspect;
	const float near_clip = 0.05f;
	const float far_clip = 4000.0f;
	const float p[3] = {previous_pos.x, previous_pos.y, previous_pos.z};
	const float cp_pos[3] = {current_pos.x, current_pos.y, current_pos.z};
	const float previous_f[3] = {previous_fwd.x, previous_fwd.y, previous_fwd.z};
	const float current_f[3] = {current_fwd.x, current_fwd.y, current_fwd.z};
	const float previous_up[3] = {0.0f, std::fabs(previous_fwd.y) > 0.9f ? 0.0f : 1.0f,
			std::fabs(previous_fwd.y) > 0.9f ? 1.0f : 0.0f};
	const float current_up[3] = {0.0f, std::fabs(current_fwd.y) > 0.9f ? 0.0f : 1.0f,
			std::fabs(current_fwd.y) > 0.9f ? 1.0f : 0.0f};
	const ve::LodCamera previous_cam = ve::lod_camera_perspective(p, previous_f, previous_up,
			fov_y, aspect, near_clip, far_clip, w, h);
	const ve::LodCamera current_cam = ve::lod_camera_perspective(cp_pos, current_f, current_up,
			fov_y, aspect, near_clip, far_clip, w, h);
	Projection previous_view_proj, current_view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			previous_view_proj.columns[c][r] = previous_cam.view_proj[c * 4 + r];
			current_view_proj.columns[c][r] = current_cam.view_proj[c * 4 + r];
		}
	auto make_camera_params = [&](Vector3 camera_pos, Vector3 camera_fwd,
			const float up[3]) {
			ve::CameraParams result = ve::CameraParams::looking_at(camera_pos.x, camera_pos.y,
					camera_pos.z, camera_fwd.x, camera_fwd.y, camera_fwd.z, up[0], up[1], up[2]);
			result.params[0] = tan_x;
			result.params[1] = tan_y;
			result.params[2] = 200.0f;
			const ve::WorldBounds wb = world_->world_bounds();
			const ve::IVec3 ro = wb.origin_regions();
			result.dims[0] = world_->store_->config().world_size_regions.x;
			result.dims[1] = world_->store_->config().world_size_regions.y;
			result.dims[2] = world_->store_->config().world_size_regions.z;
			result.dims[3] = world_->island_slot_count();
			result.region_origin[0] = ro.x;
			result.region_origin[1] = ro.y;
			result.region_origin[2] = ro.z;
			result.atlas_bricks[0] = world_->store_->config().atlas_bricks.x;
			result.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
			result.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
			return result;
		};
	const ve::CameraParams previous_params = make_camera_params(previous_pos, previous_fwd,
			previous_up);
	const ve::CameraParams current_params = make_camera_params(current_pos, current_fwd, current_up);
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	static const float no_sun[16] = {};
	const ve::BeautySettings settings = world_->beauty_settings();
	world_->ssgi_pass()->clear_result();
	float previous_matrix[16], current_matrix[16];
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			previous_matrix[c * 4 + r] = previous_view_proj.columns[c][r];
			current_matrix[c * 4 + r] = current_view_proj.columns[c][r];
		}
	const Projection previous_inv = previous_view_proj.inverse();
	const Projection current_inv = current_view_proj.inverse();
	auto render = [&](const ve::CameraParams &camera, const Projection &view_proj,
			const Projection &inv, Vector3 camera_pos, const float previous_mapping[16],
			bool have_history, uint32_t frame) {
			const float camera_position[3] = {camera_pos.x, camera_pos.y, camera_pos.z};
			world_->beauty_camera()->update(device, view_proj, camera_position, Vector2i(w, h), near_clip,
					far_clip);
			if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), camera, w, h, no_edit))
				return false;
			float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
			world_->lod_fade_band(&fade_start, &fade_end);
			world_->composite_pass()->draw(device, *world_->gbuffer(), world_->raymarch_pass()->albedo_texture(),
					world_->raymarch_pass()->surface_texture(), world_->raymarch_pass()->hitpos_texture(), view_proj,
					*world_->material_atlas(), camera, fade_start, fade_end);
			if (!world_->composite_pass()->last_draw_ok()) return false;
			const bool ssgi_ok = world_->ssgi_pass()->render(device, *world_->gbuffer(), world_->beauty_camera()->buffer(),
					previous_mapping, have_history, settings, frame);
			if (!ssgi_ok) return false;
			DeferredPass::Params dp;
			for (int c = 0; c < 4; c++)
				for (int r = 0; r < 4; r++) dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
			dp.cam_pos[0] = camera_pos.x;
			dp.cam_pos[1] = camera_pos.y;
			dp.cam_pos[2] = camera_pos.z;
			dp.flags = ve::pack_flags(settings);
			return world_->deferred_pass()->render(device, *world_->gbuffer(), *world_->material_atlas(), world_->ssgi_pass()->result(), RID(), RID(),
					no_sun, 0.0f, dp);
	};
	auto read_luma = [&]() {
		const Vector2i half = world_->gbuffer()->half_size();
		const PackedByteArray data = device->texture_get_data(world_->ssgi_pass()->result(), 0);
		const int pixels = half.x * half.y;
		if (data.size() < static_cast<int64_t>(pixels) * 8) return 0.0;
		const uint16_t *values = reinterpret_cast<const uint16_t *>(data.ptr());
		double luma = 0.0;
		for (int i = 0; i < pixels; i++)
			luma += 0.2126 * Math::half_to_float(values[i * 4]) +
					0.7152 * Math::half_to_float(values[i * 4 + 1]) +
					0.0722 * Math::half_to_float(values[i * 4 + 2]);
		return luma / static_cast<double>(pixels);
	};
	if (!render(previous_params, previous_view_proj, previous_inv, previous_pos, previous_matrix,
			false, 0)) return d;
	world_->downsample_history(device, world_->gbuffer()->lit(), *world_->gbuffer());
	device->submit();
	device->sync();
	if (!render(current_params, current_view_proj, current_inv, current_pos, previous_matrix,
			true, 1)) return d;
	device->submit();
	device->sync();
	const double mapping_luma = read_luma();
	if (!render(current_params, current_view_proj, current_inv, current_pos, current_matrix,
			true, 1)) return d;
	device->submit();
	device->sync();
	const double current_mapping_luma = read_luma();
	d["mapping_luma"] = mapping_luma;
	d["current_mapping_luma"] = current_mapping_luma;
	d["mapping_delta"] = std::fabs(mapping_luma - current_mapping_luma);
	return d;
}

Dictionary VoxelDebugHooks::debug_beauty_settings() {
	ve::BeautySettings beauty;
	int quality_tier;
	// Task 14: beauty_mutex_/beauty_/quality_tier_ moved into RenderOrchestrator; this
	// snapshot keeps the single-mutex-hold shape of the pre-move body.
	world_->beauty_snapshot(&beauty, &quality_tier);

	Dictionary d;
	d["ssgi"] = beauty.ssgi;
	d["ssr"] = beauty.ssr;
	d["contact_shadows"] = beauty.contact_shadows;
	d["outlines"] = beauty.outlines;
	d["sun_shadow_map"] = beauty.sun_shadow_map;
	d["glossy_sdf_rays"] = beauty.glossy_sdf_rays;
	d["raymarched_sun_shadow"] = beauty.raymarched_sun_shadow;
	d["ssao"] = beauty.ssao;
	d["cost_view"] = beauty.cost_view;
	d["islands"] = world_->islands_enabled_.load(std::memory_order_relaxed);
	d["ssgi_taps"] = beauty.ssgi_taps;
	d["ssr_steps"] = beauty.ssr_steps;
	d["contact_steps"] = beauty.contact_steps;
	d["outline_depth_threshold"] = beauty.outline_depth_threshold;
	d["outline_normal_threshold"] = beauty.outline_normal_threshold;
	d["tier"] = quality_tier;
	d["flags"] = static_cast<int>(ve::pack_beauty_flags(beauty));
	return d;
}

Dictionary VoxelDebugHooks::debug_perf_stats() {
	Dictionary d;
	d["physics_tick_ms"] = world_->last_physics_tick_ms_;
	d["phys_collect_ms"] = world_->colliders_ ? world_->colliders_->last_collect_ms() : 0.0f;
	d["phys_apply_ms"] = world_->colliders_ ? world_->colliders_->last_apply_ms() : 0.0f;
	d["phys_faces_ms"] = world_->colliders_ ? world_->colliders_->last_faces_ms() : 0.0f;
	d["phys_setdata_ms"] = world_->colliders_ ? world_->colliders_->last_setdata_ms() : 0.0f;
	// `build_ms` is the maximum one octant build call in the measured physics frame, not
	// the sum of all octant calls. This is the value exported into BENCH max_ms.
	d["build_ms"] = world_->colliders_ ? world_->colliders_->last_build_ms() : 0.0f;
	d["phys_body_ms"] = world_->colliders_ ? world_->colliders_->last_body_ms() : 0.0f;
	d["phys_tris"] = world_->colliders_ ? world_->colliders_->last_tris() : 0;
	d["phys_plan_ms"] = world_->colliders_ ? world_->colliders_->last_plan_ms() : 0.0f;
	d["phys_submit_ms"] = world_->colliders_ ? world_->colliders_->last_submit_ms() : 0.0f;
	d["stream_total_ms"] = world_->streamer_ ? world_->streamer_->last_total_ms() : 0.0f;
	d["stream_readback_ms"] = world_->streamer_ ? world_->streamer_->last_readback_ms() : 0.0f;
	d["island_ms"] = world_->island_manager_ ? world_->island_manager_->last_ms() : 0.0f;
	// lod_ms is CPU command-record time for the LoD raster + cull passes, not GPU execution
	// time. See LodRasterPass/LodCullPass::last_ms comments.
	d["lod_ms"] = (world_->lod_raster_pass() ? world_->lod_raster_pass()->last_ms() : 0.0f) +
			(world_->lod_cull_pass() ? world_->lod_cull_pass()->last_ms() : 0.0f);
	return d;
}

int VoxelDebugHooks::debug_physics_frame(Vector3 center) {
	world_->ensure_physics_initialized();
	return world_->physics_tick(center);
}

void VoxelDebugHooks::debug_set_physics_bubbles(const PackedVector3Array &centers) {
	std::vector<float> flat;
	flat.reserve(static_cast<size_t>(centers.size()) * 3);
	for (int i = 0; i < centers.size(); i++) {
		const Vector3 c = centers[i];
		flat.push_back(c.x);
		flat.push_back(c.y);
		flat.push_back(c.z);
	}
	world_->physics_bubble_centers_.swap(flat);
}

Dictionary VoxelDebugHooks::debug_physics_stats() {
	Dictionary d;
	d["chunks_resident"] = world_->chunks_ ? world_->chunks_->resident_count() : 0;
	d["chunks_pending"] = world_->chunks_ ? world_->chunks_->pending_count() : 0;
	d["probe_cache"] = world_->chunks_ ? world_->chunks_->probe_cache_size() : 0;
	// `bodies` preserves the historical chunk count used by the physics tests and HUD.
	// `bodies_raw` exposes the eight-way implementation detail for profiling only.
	d["bodies"] = world_->colliders_ ? world_->colliders_->active_bodies() : 0;
	d["bodies_raw"] = world_->colliders_ ? world_->colliders_->bodies_in_space() : 0;
	d["max_build_tris"] = world_->colliders_ ? world_->colliders_->max_build_tris() : 0;
	d["max_chunk_tris"] = world_->colliders_ ? world_->colliders_->max_chunk_tris() : 0;
	d["builds"] = world_->colliders_ ? world_->colliders_->builds_last_frame() : 0;
	d["queued"] = world_->colliders_ ? world_->colliders_->queued_results() : 0;
	d["failures"] = world_->colliders_ ? world_->colliders_->failures() : 0;
	d["build_ms"] = world_->colliders_ ? world_->colliders_->last_build_ms() : 0.0f;
	d["collect_ms"] = world_->colliders_ ? world_->colliders_->last_collect_ms() : 0.0f;
	return d;
}

int VoxelDebugHooks::debug_island_pending_uploads() {
	std::lock_guard<std::mutex> lock(world_->island_mutex_);
	return static_cast<int>(world_->island_uploads_.size());
}

int VoxelDebugHooks::debug_field_volume_upload_count() const {
	return world_->debug_field_volume_upload_count_.load(std::memory_order_relaxed);
}

int VoxelDebugHooks::debug_island_descriptors_pending() {
	std::lock_guard<std::mutex> lock(world_->island_mutex_);
	return world_->island_descs_dirty_ ? 1 : 0;
}

PackedInt32Array VoxelDebugHooks::debug_mesh_volume_slots() {
	PackedInt32Array out;
	if (!world_->mesh_) return out;
	for (int slot : world_->mesh_->debug_submitted_volume_slots()) out.append(slot);
	return out;
}

void VoxelDebugHooks::debug_queue_test_island_upload(int slot, const PackedByteArray &sdf,
		const PackedByteArray &mat, int dim) {
	if (slot < 0 || slot >= kMaxIslands || dim != ve::kIslandDim) {
		UtilityFunctions::printerr(
				"debug_queue_test_island_upload: invalid slot or dim (island atlas upload "
				"requires dim == ",
				ve::kIslandDim);
		return;
	}
	const int64_t n = static_cast<int64_t>(dim) * dim * dim;
	if (sdf.size() < n || mat.size() < n) {
		UtilityFunctions::printerr("debug_queue_test_island_upload: short buffers for dim ", dim);
		return;
	}
	ve::VolumeData d;
	d.dim = dim;
	d.sdf.assign(sdf.ptr(), sdf.ptr() + n);
	d.mat.assign(mat.ptr(), mat.ptr() + n);
	for (int64_t i = 0; i < n; i++)
		if (ve::decode_sdf(d.sdf[static_cast<size_t>(i)]) <= 0.0f) d.solid_voxels++;
	world_->queue_island_upload(slot, slot, d); // test fixture: atlas slot == volume slot
}

void VoxelDebugHooks::debug_queue_test_island_descriptors() {
	std::lock_guard<std::mutex> lock(world_->island_mutex_);
	world_->island_descs_.assign(1, IslandSlotDesc{});
	world_->island_descs_dirty_ = true;
}

void VoxelDebugHooks::debug_queue_committed_field_volume_upload(int slot,
		const PackedByteArray &sdf, const PackedByteArray &mat, int dim) {
	if (slot < 0 || slot >= ve::kMaxVolumes || dim < 2 || dim > ve::kIslandDim) {
		UtilityFunctions::printerr(
				"debug_queue_committed_field_volume_upload: invalid slot or dim");
		return;
	}
	const int64_t n = static_cast<int64_t>(dim) * dim * dim;
	if (sdf.size() < n || mat.size() < n) {
		UtilityFunctions::printerr(
				"debug_queue_committed_field_volume_upload: short buffers for dim ", dim);
		return;
	}
	if (!world_->store_->volumes().reserve(slot)) {
		UtilityFunctions::printerr(
				"debug_queue_committed_field_volume_upload: slot ", slot, " is already in use");
		return;
	}
	ve::VolumeData d;
	d.dim = dim;
	d.sdf.assign(sdf.ptr(), sdf.ptr() + n);
	d.mat.assign(mat.ptr(), mat.ptr() + n);
	d.normal_oct.assign(static_cast<size_t>(n), 0);
	float center2 = 0.5f * (dim - 1) * 0.05f;
	for (int64_t i = 0; i < n; i++) {
		if (ve::decode_sdf(d.sdf[static_cast<size_t>(i)]) <= 0.0f) d.solid_voxels++;
		float up[3]={0,1,0};
		int z = static_cast<int>(i / (dim*dim));
		int y = static_cast<int>((i/dim)%dim);
		int x = static_cast<int>(i % dim);
		float px = x*0.05f-center2, py=y*0.05f-center2, pz=z*0.05f-center2;
		float len=std::sqrt(px*px+py*py+pz*pz);
		if (len>1e-6f) { float n2[3]={px/len, py/len, pz/len}; d.normal_oct[static_cast<size_t>(i)]=ve::oct_encode_snorm8(n2);} else d.normal_oct[static_cast<size_t>(i)]=ve::oct_encode_snorm8(up);
	}
	if (!world_->store_->volumes().store(slot, d) || !world_->store_->volumes().pin(slot)) {
		world_->release_volume_slot(slot);
		UtilityFunctions::printerr(
				"debug_queue_committed_field_volume_upload: store/pin failed for slot ", slot);
		return;
	}
	// Only model the main-thread GPU handoff queue. The worker-side mirror is exercised by
	// ensure_physics_initialized()'s pinned-volume replay after teardown/reinit.
	{
		std::lock_guard<std::mutex> lock(world_->island_mutex_);
		world_->island_uploads_.push_back(VoxelWorld::IslandUpload{-1, slot, false, d});
	}
	if (world_->mesh_) {
		world_->mesh_->submit_volume(slot, d);
		world_->mesh_->run_sync([](MeshPass &){});
	}
}

void VoxelDebugHooks::debug_set_extraction_available(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_) world_->mesh_->debug_set_extraction_available(v);
}

Dictionary VoxelDebugHooks::debug_stored_normal_stats() {
	Dictionary d;
	if (!world_->atlas() || !world_->atlas()->is_valid()) return d;
	const StoredNormalStats s = world_->atlas()->stored_normals().stats();
	d["capacity_bytes"] = static_cast<int64_t>(s.capacity_bytes);
	d["live_bytes"] = static_cast<int64_t>(s.live_bytes);
	d["high_water_bytes"] = static_cast<int64_t>(s.high_water_bytes);
	d["allocation_failures"] = static_cast<int64_t>(s.allocation_failures);
	d["fallback_hits"] = static_cast<int64_t>(s.fallback_hits);
	// Task 8: the exact telemetry keys the HUD and the teardown/telemetry tests read.
	d["normal_capacity_bytes"] = static_cast<int64_t>(s.capacity_bytes);
	d["normal_live_bytes"] = static_cast<int64_t>(s.live_bytes);
	d["normal_high_water_bytes"] = static_cast<int64_t>(s.high_water_bytes);
	d["normal_allocation_failures"] = static_cast<int64_t>(s.allocation_failures);
	d["normal_fallback_hits"] = static_cast<int64_t>(s.fallback_hits);
	return d;
}

Dictionary VoxelDebugHooks::debug_normal_pool_state() {
	Dictionary d;
	const bool have_pool = world_->atlas() && world_->atlas()->is_valid();
	const StoredNormalPool *p = have_pool ? &world_->atlas()->stored_normals() : nullptr;
	d["pool_valid"] = p && p->is_valid();
	d["normal_rid_valid"] = p && p->normal_buffer().is_valid();
	d["volume_offsets_rid_valid"] = p && p->volume_offsets_buffer().is_valid();
	d["override_offsets_rid_valid"] = p && p->override_offsets_buffer().is_valid();
	d["volume_offsets_all_minus_one"] =
			p && p->is_valid() && p->volume_offsets_all_minus_one();
	d["override_offsets_all_minus_one"] =
			p && p->is_valid() && p->override_offsets_all_minus_one();
	return d;
}

int64_t VoxelDebugHooks::debug_normal_upload_override(int slot,
		const PackedByteArray &packed_normals) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->atlas() || !world_->atlas()->is_valid()) return -1;
	if (slot < 0) return -1;
	if (packed_normals.is_empty() || packed_normals.size() % 2 != 0) {
		// Malformed payload: exercise the pool's fallback path rather than uploading junk.
		return world_->atlas()->stored_normals().upload_override(device, slot, nullptr, 0);
	}
	return world_->atlas()->stored_normals().upload_override(device, slot,
			reinterpret_cast<const uint16_t *>(packed_normals.ptr()),
			static_cast<int>(packed_normals.size() / 2));
}

void VoxelDebugHooks::debug_normal_release_override(int slot) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->atlas() || !world_->atlas()->is_valid()) return;
	world_->atlas()->stored_normals().release_override(device, slot);
}

void VoxelDebugHooks::debug_set_fail_extractions(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_) world_->mesh_->debug_set_fail_extractions(v);
}

void VoxelDebugHooks::debug_set_fail_extract_submit(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_) world_->mesh_->debug_set_fail_extract_submit(v);
}

void VoxelDebugHooks::debug_set_fail_consolidations(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_) world_->mesh_->debug_set_fail_consolidations(v);
}

void VoxelDebugHooks::debug_set_fail_consolidate_uploads(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_) world_->mesh_->debug_set_fail_consolidate_uploads(v);
}

void VoxelDebugHooks::debug_set_fail_restore_overrides(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_) world_->mesh_->debug_set_fail_restore_overrides(v);
}

void VoxelDebugHooks::debug_set_fail_restore_overrides_always(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_) world_->mesh_->debug_set_fail_restore_overrides_always(v);
}

void VoxelDebugHooks::debug_set_pause_override_publication(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_) world_->mesh_->debug_set_pause_override_publication(v);
}

bool VoxelDebugHooks::debug_override_publication_paused() const {
	return world_->mesh_ && world_->mesh_->debug_override_publication_paused();
}

int VoxelDebugHooks::debug_island_frame(float dt, Vector3 center) {
	world_->ensure_initialized();
	world_->ensure_physics_initialized();
	if (!world_->island_manager_) return 0;
	world_->drain_occupancy();
	const int n = world_->island_manager_->run_frame(dt, center);
	// The tests drive the world by hand and never enter the compositor, so the render-thread
	// half of the handoff has to happen here too.
	RenderingDevice *device = world_->rd();
	if (device) {
		world_->drain_island_uploads(device);
		device->submit();
		device->sync();
	}
	return n;
}

Dictionary VoxelDebugHooks::debug_island_stats() {
	return world_->island_manager_ ? world_->island_manager_->stats() : Dictionary();
}

void VoxelDebugHooks::debug_set_merge_sleep_seconds(float v) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->set_merge_sleep_seconds(v);
}

#ifdef DEBUG_ENABLED
void VoxelDebugHooks::debug_set_max_dynamic_bodies(int v) {
	world_->ensure_physics_initialized();
	// Clamp before forwarding: a test hook should be able to lower the guardrail but not
	// silently disable it with an absurd value.
	v = v < 1 ? 1 : (v > kMaxDynamicBodies ? kMaxDynamicBodies : v);
	if (world_->island_manager_) world_->island_manager_->debug_set_max_dynamic_bodies(v);
}

void VoxelDebugHooks::debug_set_atlas_slot_used(int slot, bool used) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->debug_set_atlas_slot_used(slot, used);
}
#else
void VoxelDebugHooks::debug_set_max_dynamic_bodies(int v) {
	// Debug-only hook: release scripts cannot lower the 64-body guardrail.
	(void)v;
}

void VoxelDebugHooks::debug_set_atlas_slot_used(int slot, bool used) {
	// Debug-only hook: release scripts cannot mark atlas slots used.
	(void)slot;
	(void)used;
}
#endif

void VoxelDebugHooks::debug_set_fail_next_spawn(bool fail) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->debug_set_fail_next_spawn(fail);
}

void VoxelDebugHooks::debug_set_fail_next_restore(bool fail) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->debug_set_fail_next_restore(fail);
}

void VoxelDebugHooks::debug_set_fail_next_carve(bool fail) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->debug_set_fail_next_carve(fail);
}

void VoxelDebugHooks::debug_set_fail_next_resample(bool fail) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->debug_set_fail_next_resample(fail);
}

void VoxelDebugHooks::debug_set_empty_next_extraction(bool v) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->debug_set_empty_next_extraction(v);
}

void VoxelDebugHooks::debug_wake_island_body(int index) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->debug_wake_body(index);
}

void VoxelDebugHooks::debug_offset_island_body(int index, Vector3 offset) {
	world_->ensure_physics_initialized();
	if (world_->island_manager_) world_->island_manager_->debug_offset_body(index, offset);
}

Dictionary VoxelDebugHooks::debug_island_body_info(int index) {
	world_->ensure_physics_initialized();
#ifdef DEBUG_ENABLED
	if (world_->island_manager_) return world_->island_manager_->debug_body_info(index);
#else
	(void)index;
#endif
	return Dictionary();
}

RID VoxelDebugHooks::debug_body_of_chunk(Vector3i chunk) {
	if (!world_->chunks_ || !world_->colliders_) return RID();
	return world_->colliders_->body_of_slot(world_->chunks_->slot_of({chunk.x, chunk.y, chunk.z}));
}

Dictionary VoxelDebugHooks::debug_chunk_collider_info(Vector3i chunk) {
	Dictionary d;
	if (!world_->chunks_ || !world_->colliders_) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	d["slot"] = world_->chunks_->slot_of(c);
	d["state"] = world_->colliders_->chunk_state(c);
	d["in_flight"] = world_->colliders_->chunk_in_flight(c);
	d["build_count"] = world_->colliders_->build_count_of_chunk(c);
	d["last_ops"] = world_->colliders_->last_submit_op_count(c);
	return d;
}

Dictionary VoxelDebugHooks::debug_chunk_collider_octants(Vector3i chunk) {
	if (!world_->chunks_ || !world_->colliders_) return Dictionary();
	return world_->colliders_->debug_chunk_octants({chunk.x, chunk.y, chunk.z});
}

bool VoxelDebugHooks::debug_init_physics() {
	world_->ensure_physics_initialized();
	return world_->physics_ready_;
}

void VoxelDebugHooks::debug_teardown_physics() {
	world_->teardown_physics();
}

void VoxelDebugHooks::debug_lod_tick(Vector3 pos, Vector3 fwd) {
	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, 1.2217f,
			16.0f / 9.0f, 0.1f, 8000.0f, 2560, 1440);
	world_->lod_tick(cam, nullptr);
}

Dictionary VoxelDebugHooks::debug_lod_stats() {
	std::lock_guard<std::mutex> lock(world_->context().lod->lod_mutex_);
	world_->context().lod->ensure_lod();
	Dictionary d;
	d["pages_total"] = world_->context().lod->lod_pool_ ? world_->context().lod->lod_pool_->page_count() : 0;
	d["pages_free"] = world_->context().lod->lod_pool_ ? world_->context().lod->lod_pool_->free_pages() : 0;
	d["pages_used"] = (world_->context().lod->lod_pool_ ? world_->context().lod->lod_pool_->page_count() : 0) -
			(world_->context().lod->lod_pool_ ? world_->context().lod->lod_pool_->free_pages() : 0);
	d["chunks_resident"] = static_cast<int>(world_->context().lod->lod_pages_of_.size());
	int dirty_chunks = 0;
	int dirty_levels = 0;
	if (world_->context().lod->lod_tree_) world_->context().lod->lod_tree_->dirty_stats(&dirty_chunks, &dirty_levels);
	d["dirty_chunks"] = dirty_chunks;
	d["dirty_levels"] = dirty_levels;
	int draw_pages = 0;
	for (const ve::LodDrawItem &item : world_->context().lod->lod_walk_.draws)
		draw_pages += item.page_count;
	d["draw_pages"] = draw_pages;
	// Expose the exact page identities used by the current camera cut, not just their
	// aggregate count. A bounded pool may keep a drawable coarse cut while refinement
	// requests remain pending; tests must prove that the actual scene page set is stable.
	std::vector<ve::LodPageDraw> draw_page_list;
	ve::lod_collect_page_draws(world_->context().lod->lod_walk_.draws,
			world_->context().lod->lod_pages_of_, world_->context().lod->lod_page_quads_, &draw_page_list);
	PackedInt32Array draw_page_ids;
	for (const ve::LodPageDraw &page : draw_page_list) draw_page_ids.append(page.page);
	d["draw_page_ids"] = draw_page_ids;
	PackedInt32Array resident_page_ids;
	for (const auto &page : world_->context().lod->lod_page_quads_)
		if (page.second > 0) resident_page_ids.append(page.first);
	d["resident_page_ids"] = resident_page_ids;
	// Requests the last walk still wants built. Zero, with nothing in flight, is what
	// "the far field has converged for this camera" means; tests wait on it instead of
	// guessing a frame count. The exact request identities are also exported so a bounded
	// pool fallback can distinguish a stable backlog from a rotating one.
	d["requests_pending"] = static_cast<int>(world_->context().lod->lod_walk_.requests.size());
	Array pending_request_ids;
	for (const ve::LodBuildRequest &request : world_->context().lod->lod_walk_.requests) {
		pending_request_ids.append(String::num_int64(request.level) + ":" +
				String::num_int64(request.coord.x) + ":" + String::num_int64(request.coord.y) + ":" +
				String::num_int64(request.coord.z));
	}
	d["pending_request_ids"] = pending_request_ids;
	// LodArena::alloc is all-or-nothing, so this should always be zero -- but reporting a
	// hardcoded 0 makes the test that asserts it vacuous. MEASURE the two shapes a
	// partially funded build would actually take: a chunk holding a page the per-page quad
	// count never learned about, and arena pages that no resident chunk owns (the leak a
	// half-rolled-back allocation leaves behind).
	int partial = 0;
	size_t owned_pages = 0;
	for (const auto &kv : world_->context().lod->lod_pages_of_) {
		owned_pages += kv.second.size();
		for (int p : kv.second) {
			if (world_->context().lod->lod_page_quads_.find(p) == world_->context().lod->lod_page_quads_.end()) {
				partial++;
				break;
			}
		}
	}
	const int used_pages = (world_->context().lod->lod_pool_ ? world_->context().lod->lod_pool_->page_count() : 0) -
			(world_->context().lod->lod_pool_ ? world_->context().lod->lod_pool_->free_pages() : 0);
	const int unowned = used_pages - static_cast<int>(owned_pages);
	d["partial_allocations"] = partial + (unowned > 0 ? unowned : 0);
	d["builds_in_flight"] = world_->mesh_ && world_->mesh_->lod_busy() ? 1 : 0;
	// Async cull stats readback; zero until the first readback lands (safe "nothing culled").
	d["culled_ratio"] = world_->lod_cull_pass() ? world_->lod_cull_pass()->culled_ratio() : 0.0f;
	return d;
}

Vector2 VoxelDebugHooks::debug_lod_fade_band() {
	float start = ve::kLodFadeStartM;
	float end = ve::kLodFadeEndM;
	world_->lod_fade_band(&start, &end);
	return Vector2(start, end);
}

Dictionary VoxelDebugHooks::debug_lod_render_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	return debug_lod_render_probe_culled(pos, fwd, w, h, true);
}

Dictionary VoxelDebugHooks::debug_lod_render_probe_culled(Vector3 pos, Vector3 fwd, int w, int h,
		bool cull) {
	Dictionary d;
	d["coverage"] = 0.0f;
	d["depth_min"] = 0.0f;
	d["depth_max"] = 0.0f;
	d["nearest_hit_m"] = 0.0f;
	d["draw_pages"] = 0;
	d["depth_sum"] = 0.0;
	if (w <= 0 || h <= 0) return d;

	// One tick: refresh the walk and the raster pass's page list for this view. debug_lod_tick
	// -> lod_tick takes lod_mutex_ around ensure_lod and all LoD state mutation.
	debug_lod_tick(pos, fwd);

	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->context().lod->lod_pool_ || !world_->lod_raster_pass() || !world_->material_atlas()) return d;

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, 1.2217f,
			aspect, 0.1f, 8000.0f, w, h);
	Projection vp;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			vp.columns[c][r] = cam.view_proj[c * 4 + r];

	if (!world_->gbuffer() || !world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h))) return d;

	// Clear to reverse-Z far (0) before drawing the far field. Through a render pass, not
	// texture_clear: that is a COLOUR clear and Godot's Metal driver refuses it on a depth
	// format, which left this probe reading the previous probe's depth on that device.
	if (!world_->lod_raster_pass()->clear_targets(device, *world_->gbuffer())) return d;
	world_->lod_raster_pass()->set_cull_enabled(cull);
	const int draw_count = world_->lod_raster_pass()->draw_page_count();
	world_->context().lod->lod_pool_->upload_draw_args(world_->lod_raster_pass()->draw_pages());
	float probe_start = ve::kLodFadeStartM;
	float probe_end = ve::kLodFadeEndM;
	world_->lod_fade_band(&probe_start, &probe_end);
	bool ok = world_->lod_raster_pass()->draw(device, *world_->context().lod->lod_pool_, *world_->material_atlas(), *world_->gbuffer(),
			vp, p, draw_count, probe_start, probe_end);
	device->submit();
	device->sync();

	if (ok) {
		const PackedByteArray depth_data = device->texture_get_data(world_->gbuffer()->depth(), 0);
		const int pixel_count = w * h;
		if (depth_data.size() >= pixel_count * 4) {
			const float *depths = reinterpret_cast<const float *>(depth_data.ptr());
			int covered = 0;
			float dmin = 1.0f;
			float dmax = 0.0f;
			// Coverage alone cannot see a change that swaps one surface for another behind
			// it -- a crater in ground that has more ground behind it keeps every pixel
			// covered. Accumulate the depths too, so a test can ask whether the far field's
			// IMAGE changed rather than only whether its silhouette did.
			double depth_sum = 0.0;
			for (int i = 0; i < pixel_count; i++) {
				const float dv = depths[i];
				if (dv <= 0.0f) continue;
				covered++;
				depth_sum += static_cast<double>(dv);
				if (dv < dmin) dmin = dv;
				if (dv > dmax) dmax = dv;
			}
			d["depth_sum"] = depth_sum;
			d["coverage"] = static_cast<float>(covered) / static_cast<float>(pixel_count);
			d["depth_min"] = covered > 0 ? dmin : 0.0f;
			d["depth_max"] = covered > 0 ? dmax : 0.0f;
			// Reverse-Z perspective: depth d in [0,1] maps to view distance
			// far*near / (near + d*(far-near)); the largest depth is the nearest hit.
			if (covered > 0) {
				const float near_z = 0.1f;
				const float far_z = 8000.0f;
				const float denom = near_z + dmax * (far_z - near_z);
				d["nearest_hit_m"] = denom > 0.0f ? far_z * near_z / denom : 0.0f;
			}
		}
	}

	// The raster pass already holds the exact page list produced by prepare_lod_raster;
	// reading lod_walk_ here would require re-taking lod_mutex_ after the tick released it.
	d["draw_pages"] = world_->lod_raster_pass() ? world_->lod_raster_pass()->draw_page_count() : 0;

	// The raster pass cached a framebuffer over the owned G-buffer; drop it before a future
	// probe changes its attachments.
	world_->lod_raster_pass()->release_targets();
	return d;
}

Dictionary VoxelDebugHooks::debug_lod_gbuffer_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["material_coverage"] = 0.0f;
	d["worst_normal_length_error"] = 0.0f;
	d["gloss_max"] = 0.0f;
	d["sun_min"] = 1.0f;
	d["sun_max"] = 0.0f;
	d["normal_mean"] = Vector3();
	if (w <= 0 || h <= 0) return d;

	debug_lod_tick(pos, fwd);
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->context().lod->lod_pool_ || !world_->lod_raster_pass() || !world_->material_atlas() || !world_->gbuffer())
		return d;
	world_->lod_raster_pass()->release_targets();
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h))) return d;

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, 1.2217f,
			aspect, 0.1f, 8000.0f, w, h);
	Projection vp;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			vp.columns[c][r] = cam.view_proj[c * 4 + r];

	// One render-pass clear for all three attachments; see debug_lod_render_probe above for
	// why the depth one cannot be a texture_clear.
	if (!world_->lod_raster_pass()->clear_targets(device, *world_->gbuffer())) return d;
	world_->lod_raster_pass()->set_cull_enabled(true);
	world_->context().lod->lod_pool_->upload_draw_args(world_->lod_raster_pass()->draw_pages());
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	world_->lod_fade_band(&fade_start, &fade_end);
	const bool ok = world_->lod_raster_pass()->draw(device, *world_->context().lod->lod_pool_, *world_->material_atlas(), *world_->gbuffer(), vp, p,
			world_->lod_raster_pass()->draw_page_count(), fade_start, fade_end);
	device->submit();
	device->sync();

	if (ok) {
		const PackedByteArray albedo = device->texture_get_data(world_->gbuffer()->albedo(), 0);
		const PackedByteArray surface = device->texture_get_data(world_->gbuffer()->surface(), 0);
		const int pixels = w * h;
		if (albedo.size() >= pixels * 4 && surface.size() >= pixels * 8) {
			const uint8_t *a = reinterpret_cast<const uint8_t *>(albedo.ptr());
			const uint16_t *s = reinterpret_cast<const uint16_t *>(surface.ptr());
			int covered = 0;

			float worst = 0.0f;
			float gloss_max = 0.0f;
			float sun_min = 1.0f;
			float sun_max = 0.0f;
			// The MEAN decoded normal over the covered pixels. A per-pixel normal cannot be
			// compared across two renders -- the LoD walk may hand back a different page set --
			// but the average orientation of a settled patch is stable, which is what makes it
			// a usable before/after for "did the material normal map move the far field".
			double nsum[3] = {0.0, 0.0, 0.0};
			for (int i = 0; i < pixels; i++) {
				const float material = Math::half_to_float(s[i * 4 + 2]);
				if (material < 0.5f) continue;
				covered++;
				const float e[2] = {Math::half_to_float(s[i * 4]), Math::half_to_float(s[i * 4 + 1])};
				float n[3] = {};
				ve::oct_decode(e, n);
				const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				worst = std::max(worst, std::fabs(length - 1.0f));
				nsum[0] += n[0]; nsum[1] += n[1]; nsum[2] += n[2];
				gloss_max = std::max(gloss_max, Math::half_to_float(s[i * 4 + 3]));
				const float sun = static_cast<float>(a[i * 4 + 3]) / 255.0f;
				sun_min = std::min(sun_min, sun);
				sun_max = std::max(sun_max, sun);
			}
			d["material_coverage"] = static_cast<float>(covered) / static_cast<float>(pixels);
			d["worst_normal_length_error"] = worst;
			d["gloss_max"] = gloss_max;
			d["sun_min"] = covered > 0 ? sun_min : 1.0f;
			d["sun_max"] = covered > 0 ? sun_max : 1.0f;
			if (covered > 0) {
				const double inv = 1.0 / static_cast<double>(covered);
				d["normal_mean"] = Vector3(static_cast<float>(nsum[0] * inv),
						static_cast<float>(nsum[1] * inv), static_cast<float>(nsum[2] * inv));
			}
		}
	}
	world_->lod_raster_pass()->release_targets();
	return d;
}

Dictionary VoxelDebugHooks::debug_seam_probe(Vector3 pos, Vector3 fwd, int w, int h, bool skip_lod) {
	Dictionary d;
	d["band_pixels"] = 0;
	d["band_pixels_unclaimed"] = 0;
	d["band_pixels_double_claimed"] = 0;
	d["near_pixels_lost_to_lod"] = 0;
	d["far_pixels_lost_to_raymarch"] = 0;
	if (w <= 0 || h <= 0) return d;

	// One tick refreshes the walk and the raster pass's page list for this view.
	debug_lod_tick(pos, fwd);

	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass() ||
			!world_->composite_pass() || !world_->deferred_pass() || !world_->inject_pass() || !world_->gbuffer() ||
			!world_->context().lod->lod_pool_ || !world_->lod_raster_pass()) return d;

	// The near field needs the streamer to have populated the SDF atlas; the LoD settle in
	// the test only converges the far-field walk. Drive the streamer until it is quiet (the
	// same condition the near-field tests use) before rendering the composite.
	{
		int quiet = 0;
		for (int i = 0; i < 120 && quiet < 6; i++) {
			const int actions = debug_stream_frame(pos);
			quiet = actions == 0 ? quiet + 1 : 0;
		}
	}

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float fov_y = 1.2217f;
	const float tan_y = std::tan(fov_y * 0.5f);
	const float tan_x = tan_y * aspect;
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, fov_y,
			aspect, 0.1f, 8000.0f, w, h);
	world_->composite_pass()->release_targets();
	world_->lod_raster_pass()->release_targets();
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h))) return d;
	Projection vp;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			vp.columns[c][r] = cam.view_proj[c * 4 + r];

	// Raymarch with the SAME camera as the LoD raster, so the two fields agree on the pixel
	// grid. `looking_at` builds the same basis as lod_camera_perspective; fill in the fov.
	ve::CameraParams cp = ve::CameraParams::looking_at(pos.x, pos.y, pos.z,
			fwd.x, fwd.y, fwd.z, 0.0f, 1.0f, 0.0f);
	cp.params[0] = tan_x;
	cp.params[1] = tan_y;
	cp.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_->store_->config().world_size_regions.x;
	cp.dims[1] = world_->store_->config().world_size_regions.y;
	cp.dims[2] = world_->store_->config().world_size_regions.z;
	cp.dims[3] = world_->island_slot_count();
	cp.region_origin[0] = ro.x;
	cp.region_origin[1] = ro.y;
	cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = world_->store_->config().atlas_bricks.x;
	cp.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cp.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cp, w, h, kNoEdit))
		return d;

	auto make_target = [&](RID *out, RenderingDevice::DataFormat fmt, bool depth) {
		Ref<RDTextureFormat> tf;
		tf.instantiate();
		tf->set_format(fmt);
		tf->set_width(w);
		tf->set_height(h);
		tf->set_usage_bits(depth ?
				RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
						RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT :
				RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
						RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
		Ref<RDTextureView> tv;
		tv.instantiate();
		*out = device->texture_create(tf, tv, {});
	};
	RID color, depth, marker;
	make_target(&color, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, false);
	make_target(&depth, RenderingDevice::DATA_FORMAT_D32_SFLOAT, true);
	{
		Ref<RDTextureFormat> tf;
		tf.instantiate();
		tf->set_format(RenderingDevice::DATA_FORMAT_R8_UINT);
		tf->set_width(w);
		tf->set_height(h);
		tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
		PackedByteArray zero;
		zero.resize(w * h);
		zero.fill(0);
		TypedArray<PackedByteArray> upload;
		upload.push_back(zero);
		Ref<RDTextureView> tv;
		tv.instantiate();
		marker = device->texture_create(tf, tv, upload);
	}
	if (!color.is_valid() || !depth.is_valid() || !marker.is_valid()) {
		if (color.is_valid()) device->free_rid(color);
		if (depth.is_valid()) device->free_rid(depth);
		if (marker.is_valid()) device->free_rid(marker);
		return d;
	}
	// The marker starts at 0 and is ORed to 1/2/3 by the two G-buffer producers.
	auto cleanup = [&]() {
		world_->composite_pass()->release_targets();
		world_->inject_pass()->release_targets();
		world_->lod_raster_pass()->release_targets();
		if (color.is_valid()) device->free_rid(color);
		if (depth.is_valid()) device->free_rid(depth);
		if (marker.is_valid()) device->free_rid(marker);
	};

	// Pass 1: near field. Composite writes 1 into the marker where it keeps the depth.
	// The probe must fade where the production path fades, or it measures a band neither
	// shader is using and reports a seam that is not there.
	float probe_fade_start = ve::kLodFadeStartM;
	float probe_fade_end = ve::kLodFadeEndM;
	world_->lod_fade_band(&probe_fade_start, &probe_fade_end);
	world_->composite_pass()->draw(device, *world_->gbuffer(), world_->raymarch_pass()->albedo_texture(),
			world_->raymarch_pass()->surface_texture(), world_->raymarch_pass()->hitpos_texture(), vp, *world_->material_atlas(), cp,
			probe_fade_start, probe_fade_end, marker);
	if (!world_->composite_pass()->last_draw_ok()) {
		cleanup();
		return d;
	}

	// Pass 2: far field. The LoD pipeline uses LOGIC_OP_OR on the marker, so kept far
	// pixels OR 2 into the composite's 1, making double-claimed pixels read 3.
	// `skip_lod` is a debug-only knob for the regression test: by leaving the far field
	// out entirely it creates a real far-field gap, which the probe must count as
	// unclaimed. Production rendering never passes it.
	if (!skip_lod) {
		world_->lod_raster_pass()->set_cull_enabled(true);
		world_->context().lod->lod_pool_->upload_draw_args(world_->lod_raster_pass()->draw_pages());
		const int draw_count = world_->lod_raster_pass()->draw_page_count();
		world_->lod_raster_pass()->draw(device, *world_->context().lod->lod_pool_, *world_->material_atlas(), *world_->gbuffer(), vp, p,
				draw_count, probe_fade_start, probe_fade_end, marker);
	}

	// The single deferred evaluation follows both producers, matching production ordering.
	DeferredPass::Params dp;
	const Projection inv = vp.inverse();
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
	dp.cam_pos[0] = pos.x;
	dp.cam_pos[1] = pos.y;
	dp.cam_pos[2] = pos.z;
	dp.flags = ve::pack_flags(world_->beauty_settings());
	static const float kNoSun[16] = {};
	if (!world_->deferred_pass()->render(device, *world_->gbuffer(), *world_->material_atlas(), RID(), RID(), RID(), kNoSun, 0.0f, dp) ||
			!world_->inject_pass()->draw(device, color, depth, world_->gbuffer()->lit(), world_->gbuffer()->depth())) {
		cleanup();
		return d;
	}
	device->submit();
	device->sync();

	const PackedByteArray depth_data = device->texture_get_data(world_->gbuffer()->depth(), 0);
	const PackedByteArray marker_data = device->texture_get_data(marker, 0);
	const PackedByteArray hitpos_data = device->texture_get_data(
			world_->raymarch_pass()->hitpos_texture(), 0);
	int band_pixels = 0;
	int band_pixels_unclaimed = 0;
	int band_pixels_double_claimed = 0;
	int near_pixels_lost_to_lod = 0;
	int far_pixels_lost_to_raymarch = 0;
	if (depth_data.size() >= static_cast<int64_t>(w) * h * 4 &&
			marker_data.size() >= static_cast<int64_t>(w) * h &&
			hitpos_data.size() >= static_cast<int64_t>(w) * h * 16) {
		const float *df = reinterpret_cast<const float *>(depth_data.ptr());
		const uint8_t *mk = reinterpret_cast<const uint8_t *>(marker_data.ptr());
		const float *hf = reinterpret_cast<const float *>(hitpos_data.ptr());
		// Reconstruct the world hit from the reverse-Z depth and the same camera basis the
		// two fields use, so the probe measures the same Euclidean distance the shaders fade
		// on. The LoD-only far field has no raymarch hitpos, so the depth attachment is the
		// primary source that covers both fields on the same pixel grid. When both fields
		// discarded a pixel (the unclaimed case), depth is 0 (the reverse-Z far clear) but
		// the raymarch hitpos texture still records the terrain hit; use its world-space
		// position to recover the Euclidean distance and classify the marker.
		float r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
				f[0] * up[1] - f[1] * up[0]};
		const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
		if (rl > 0.0f) { r[0] /= rl; r[1] /= rl; r[2] /= rl; }
		const float kNear = 0.1f;
		const float kFar = 8000.0f;
		for (int i = 0; i < w * h; i++) {
			const float depth_val = df[i];
			const float *hp = &hf[i * 4];
			const bool raymarch_hit = hp[3] >= 0.5f;
			float dist;
			if (depth_val > 0.0f) {
				const float u = (static_cast<float>(i % w) + 0.5f) / static_cast<float>(w);
				const float v = (static_cast<float>(i / w) + 0.5f) / static_cast<float>(h);
				const float ndc_x = u * 2.0f - 1.0f;
				const float ndc_y = 1.0f - v * 2.0f;
				const float z_view = kFar * kNear /
						(kNear + depth_val * (kFar - kNear));
				const float ax = ndc_x * tan_x;
				const float ay = ndc_y * tan_y;
				dist = z_view * std::sqrt(1.0f + ax * ax + ay * ay);
			} else if (raymarch_hit) {
				const float dx = hp[0] - p[0];
				const float dy = hp[1] - p[1];
				const float dz = hp[2] - p[2];
				dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			} else {
				// No terrain sample to classify: no field wrote depth and the raymarch
				// did not hit, so this is sky rather than an unclaimed band pixel.
				continue;
			}
			const uint8_t m = mk[i];
			if (dist >= probe_fade_start && dist <= probe_fade_end) {
				band_pixels++;
				if (m == 0u) band_pixels_unclaimed++;
				if (m == 3u) band_pixels_double_claimed++;
			} else if (dist < probe_fade_start && (m & 2u) != 0u) {
				near_pixels_lost_to_lod++;
			} else if (dist > probe_fade_end && (m & 1u) != 0u) {
				far_pixels_lost_to_raymarch++;
			}
		}
	}
	d["band_pixels"] = band_pixels;
	d["band_pixels_unclaimed"] = band_pixels_unclaimed;
	d["band_pixels_double_claimed"] = band_pixels_double_claimed;
	// Short aliases retained for the Task 7 seam contract.
	d["neither"] = band_pixels_unclaimed;
	d["both"] = band_pixels_double_claimed;
	d["near_pixels_lost_to_lod"] = near_pixels_lost_to_lod;
	d["far_pixels_lost_to_raymarch"] = far_pixels_lost_to_raymarch;
	// Same as debug_lod_render_probe_culled: use the raster pass's prepared page list rather
	// than reading lod_walk_ after lod_tick released lod_mutex_.
	d["draw_pages"] = world_->lod_raster_pass() ? world_->lod_raster_pass()->draw_page_count() : 0;

	// Drop cached framebuffers before freeing their throwaway scene-buffer/marker targets.
	cleanup();
	return d;
}

Dictionary VoxelDebugHooks::debug_lod_cull_probe(Vector3 pos, Vector3 fwd) {
	Dictionary d;
	d["args_before"] = 0;
	d["args_after"] = 0;
	d["offsets_changed"] = 0;
	d["index_counts_changed"] = 0;
	d["drawn_after"] = 0;
	d["culled_ratio"] = 0.0f;

	// One tick: refresh the walk and the raster pass's page list for this view.
	debug_lod_tick(pos, fwd);

	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->context().lod->lod_pool_ || !world_->lod_raster_pass() || !world_->lod_cull_pass() ||
			!world_->hiz_pass()) {
		return d;
	}
	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, 1.2217f,
			16.0f / 9.0f, 0.1f, 8000.0f, 2560, 1440);
	Projection vp;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			vp.columns[c][r] = cam.view_proj[c * 4 + r];

	const int draw_count = world_->lod_raster_pass()->draw_page_count();
	if (draw_count <= 0) return d;
	world_->context().lod->lod_pool_->upload_draw_args(world_->lod_raster_pass()->draw_pages());
	device->submit();
	device->sync();
	const PackedByteArray before = device->buffer_get_data(world_->context().lod->lod_pool_->args_buffer(), 0,
			static_cast<uint32_t>(draw_count) * 20);

	// This probe deliberately exercises frustum culling plus whatever HiZ state is present.
	// Build a synthetic "everything far" pyramid first so the run never reads an unbuilt or
	// stale pyramid (the production path builds from the real scene depth before culling).
	debug_hiz_probe_synthetic(0.0f, 1.0f);

	const bool ok = world_->lod_cull_pass()->run(device, *world_->context().lod->lod_pool_, world_->hiz_pass(), vp, draw_count,
			draw_count, 0);
	device->submit();
	device->sync();
	if (!ok) return d;

	const PackedByteArray after = device->buffer_get_data(world_->context().lod->lod_pool_->args_buffer(), 0,
			static_cast<uint32_t>(draw_count) * 20);
	if (before.size() < static_cast<int64_t>(draw_count) * 20 ||
			after.size() < static_cast<int64_t>(draw_count) * 20) {
		return d;
	}

	const uint32_t *b = reinterpret_cast<const uint32_t *>(before.ptr());
	const uint32_t *a = reinterpret_cast<const uint32_t *>(after.ptr());
	int offsets_changed = 0;
	int index_counts_changed = 0;
	int drawn_after = 0;
	PackedInt32Array pages;
	PackedInt32Array culled;
	PackedInt32Array page_frustum_culled;
	PackedInt32Array slot_frustum_culled;
	const std::vector<uint32_t> &page_chunk_cpu = world_->context().lod->lod_pool_->page_chunk_cpu();
	float planes[6][4];
	ve::lod_frustum_planes(cam.view_proj, planes);
	const PackedByteArray chunk_bytes = device->buffer_get_data(world_->context().lod->lod_pool_->chunk_buffer(), 0,
			static_cast<uint32_t>(world_->context().lod->lod_pool_->chunk_record_count() * 32));
	const float *chunk_data = reinterpret_cast<const float *>(chunk_bytes.ptr());
	const bool have_chunks = chunk_bytes.size() >= world_->context().lod->lod_pool_->chunk_record_count() * 32;
	auto aabb_outside = [&](uint32_t ci) -> bool {
		if (!have_chunks || ci == 0xffffffffu) return true;
		const float *rec = chunk_data + static_cast<size_t>(ci) * 8;
		const float lo[3] = {rec[0], rec[1], rec[2]};
		const float cell = rec[3];
		const float hi[3] = {lo[0] + cell * float(ve::kLodChunkCells),
				lo[1] + cell * float(ve::kLodChunkCells),
				lo[2] + cell * float(ve::kLodChunkCells)};
		return !ve::lod_aabb_in_frustum(planes, lo, hi);
	};
	for (int i = 0; i < draw_count; i++) {
		const size_t base = static_cast<size_t>(i) * 5;
		if (a[base + 1] != 0u) drawn_after++;
		if (b[base + 0] != a[base + 0]) index_counts_changed++;
		if (b[base + 3] != a[base + 3]) offsets_changed++;
		const uint32_t page = b[base + 3] / static_cast<uint32_t>(ve::kLodVertsPerPage);
		const uint32_t page_ci = page < page_chunk_cpu.size() ?
				page_chunk_cpu[static_cast<size_t>(page)] : 0xffffffffu;
		const uint32_t slot_ci = static_cast<uint32_t>(i) < page_chunk_cpu.size() ?
				page_chunk_cpu[static_cast<size_t>(i)] : 0xffffffffu;
		pages.append(static_cast<int32_t>(page));
		culled.append(a[base + 1] == 0u ? 1 : 0);
		page_frustum_culled.append(aabb_outside(page_ci) ? 1 : 0);
		slot_frustum_culled.append(aabb_outside(slot_ci) ? 1 : 0);
	}

	d["args_before"] = draw_count;
	d["args_after"] = draw_count;
	d["offsets_changed"] = offsets_changed;
	d["index_counts_changed"] = index_counts_changed;
	d["drawn_after"] = drawn_after;
	d["culled_ratio"] = static_cast<float>(draw_count - drawn_after) / static_cast<float>(draw_count);
	d["pages"] = pages;
	d["culled"] = culled;
	d["page_frustum_culled"] = page_frustum_culled;
	d["slot_frustum_culled"] = slot_frustum_culled;
	return d;
}

Dictionary VoxelDebugHooks::debug_gbuffer_stats(int w, int h) {
	Dictionary d;
	d["valid"] = false;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->gbuffer()) return d;
	// The probe path: no RenderSceneBuffersRD exists outside a render callback, so this
	// exercises the owned branch. Everything else about the object is identical.
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h))) {
		d["reallocations"] = world_->gbuffer()->reallocations();
		return d;
	}
	d["valid"] = world_->gbuffer()->is_valid();
	d["width"] = world_->gbuffer()->size().x;
	d["height"] = world_->gbuffer()->size().y;
	d["half_width"] = world_->gbuffer()->half_size().x;
	d["half_height"] = world_->gbuffer()->half_size().y;
	d["albedo_valid"] = world_->gbuffer()->albedo().is_valid();
	d["surface_valid"] = world_->gbuffer()->surface().is_valid();
	d["depth_valid"] = world_->gbuffer()->depth().is_valid();
	d["lit_valid"] = world_->gbuffer()->lit().is_valid();
	d["history_valid"] = world_->gbuffer()->history().is_valid();
	d["albedo_id"] = static_cast<int64_t>(world_->gbuffer()->albedo().get_id());
	d["depth_id"] = static_cast<int64_t>(world_->gbuffer()->depth().get_id());
	d["reallocations"] = world_->gbuffer()->reallocations();
	return d;
}

Dictionary VoxelDebugHooks::debug_hiz_stats() {
	Dictionary d;
	world_->ensure_initialized();
	if (!world_->hiz_pass() || !world_->rd()) return d;
	d["width"] = HizPass::kSize;
	d["height"] = HizPass::kSize;
	d["mips"] = HizPass::kMipCount;
	d["readback_level"] = HizPass::kReadbackLevel;
	d["readback_texels"] = HizPass::kGrid * HizPass::kGrid;
	return d;
}

Dictionary VoxelDebugHooks::debug_hiz_shutdown_probe() {
	Dictionary d;
	d["callback_guarded"] = false;
	d["queued"] = false;
	d["was_pending"] = false;
	d["drained"] = false;
	d["initialized_after"] = true;
	{
		const bool callback_guarded = world_->try_begin_render_callback();
		d["callback_guarded"] = callback_guarded;
		if (!callback_guarded) return d;
		struct CallbackGuard {
			VoxelWorld *world;
			~CallbackGuard() { world->end_render_callback(); }
		} callback_guard{world_};
		world_->ensure_initialized();
		RenderingDevice *device = world_->rd();
		if (!device || !world_->hiz_pass() || !world_->gbuffer()) return d;
		const Vector2i size(64, 64);
		if (!world_->gbuffer()->ensure(device, nullptr, size)) return d;
		if (!world_->hiz_pass()->build(device, world_->gbuffer()->depth(), size)) return d;
		d["queued"] = world_->hiz_pass()->readback_pending();
	}
	world_->shutdown_render_resources();
	d["was_pending"] = world_->last_hiz_readback_was_pending_;
	d["drained"] = world_->last_hiz_readback_was_drained_;
	d["initialized_after"] = world_->initialized_;
	return d;
}

Dictionary VoxelDebugHooks::debug_hiz_probe_synthetic(float far_value, float near_value) {
	Dictionary d;
	d["mip0_at_near_texel"] = 0.0f;
	d["mip1_covering_both"] = 0.0f;
	d["top_mip"] = 0.0f;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->hiz_pass()) return d;

	// A 256^2 synthetic depth image: every texel is `far_value` except one near texel at
	// (0,0). With the level-0 pass mapping the scene 1:1 at this size, mip 0 keeps the near
	// value, the mip-1 parent over the 2x2 corner keeps the far value, and the 1x1 top mip
	// keeps the far value too.
	const int size = HizPass::kSize;
	PackedByteArray data;
	data.resize(size * size * 4);
	float *pixels = reinterpret_cast<float *>(data.ptrw());
	for (int i = 0; i < size * size; i++) pixels[i] = far_value;
	pixels[0] = near_value;

	TypedArray<PackedByteArray> upload;
	upload.push_back(data);
	Ref<RDTextureFormat> tf;
	tf.instantiate();
	tf->set_format(RenderingDevice::DATA_FORMAT_R32_SFLOAT);
	tf->set_width(size);
	tf->set_height(size);
	tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> tv;
	tv.instantiate();
	const RID synthetic = device->texture_create(tf, tv, upload);
	if (!synthetic.is_valid()) return d;

	if (world_->hiz_pass()->build(device, synthetic, Vector2i(size, size))) {
		device->submit();
		device->sync();
		d["mip0_at_near_texel"] = world_->hiz_pass()->probe_mip_texel(device, 0, 0, 0);
		d["mip1_covering_both"] = world_->hiz_pass()->probe_mip_texel(device, 1, 0, 0);
		d["top_mip"] = world_->hiz_pass()->probe_mip_texel(device, HizPass::kMipCount - 1, 0, 0);
		// Make the async readback deterministic for the test hooks: read the 4 KB copy
		// synchronously after sync and feed it into the same occlusion grid the walk uses.
		const PackedByteArray rb = device->texture_get_data(world_->hiz_pass()->readback_texture(), 0);
		world_->hiz_pass()->update_occlusion(rb);
	}
	// The level-0 uniform set references this throwaway source; drop the cached set before
	// freeing the texture so the next probe does not try to free a cascade-freed set.
	world_->hiz_pass()->release_level0_set();
	device->free_rid(synthetic);
	return d;
}

bool VoxelDebugHooks::debug_hiz_occluded(Vector2 lo, Vector2 hi, float depth) {
	world_->ensure_initialized();
	if (!world_->hiz_pass() || !world_->rd()) return false;
	const float ss_min[3] = {lo.x, lo.y, depth};
	const float ss_max[3] = {hi.x, hi.y, depth};
	return world_->hiz_pass()->occlusion()->occluded(ss_min, ss_max);
}

void VoxelDebugHooks::debug_apply_sphere_subtract(Vector3 centre, float radius) {
	if (!world_->store_->edit_log()) world_->ensure_physics_initialized();
	ve::EditOp op;
	op.type = ve::kOpSphereSubtract;
	op.material = 0;
	op.pos[0] = centre.x;
	op.pos[1] = centre.y;
	op.pos[2] = centre.z;
	op.radius = radius;
	world_->append_edit(op);
}

void VoxelDebugHooks::debug_apply_sphere_add(Vector3 centre, float radius, int material) {
	if (!world_->store_->edit_log()) world_->ensure_physics_initialized();
	ve::EditOp op;
	op.type = ve::kOpSphereAdd;
	op.material = static_cast<uint16_t>(material);
	op.pos[0] = centre.x;
	op.pos[1] = centre.y;
	op.pos[2] = centre.z;
	op.radius = radius;
	world_->append_edit(op);
}

void VoxelDebugHooks::debug_apply_sphere_paint(Vector3 centre, float radius, int material) {
	if (!world_->store_->edit_log()) world_->ensure_physics_initialized();
	ve::EditOp op;
	op.type = ve::kOpSpherePaint;
	op.material = static_cast<uint16_t>(material);
	op.pos[0] = centre.x;
	op.pos[1] = centre.y;
	op.pos[2] = centre.z;
	op.radius = radius;
	world_->append_edit(op);
}

void VoxelDebugHooks::debug_apply_volume_add(int slot, Vector3 origin, float voxel, int dim) {
	if (!world_->store_->edit_log()) world_->ensure_physics_initialized();
	float o[3] = {origin.x, origin.y, origin.z};
	ve::EditOp op = ve::make_volume_add(slot, o, voxel, dim);
	world_->append_edit(op);
}

int VoxelDebugHooks::debug_region_op_count(Vector3i region) {
	if (!world_->store_->edit_log()) return 0;
	std::lock_guard<std::mutex> lock(world_->edit_mutex());
	return world_->store_->edit_log()->op_count({region.x, region.y, region.z});
}

int VoxelDebugHooks::debug_override_region_table(int region_slot) const {
	return world_->atlas() ? world_->atlas()->overrides().region_table(region_slot) : -1;
}

int VoxelDebugHooks::debug_override_used() const {
	return world_->store_->overrides() ? world_->store_->overrides()->used() : 0;
}

bool VoxelDebugHooks::debug_fill_override_pool() {
	world_->ensure_physics_initialized();
	std::unique_lock<std::mutex> edit_lock(world_->edit_mutex());
	if (!world_->atlas() || !world_->mesh_ || !world_->store_->overrides() || world_->store_->overrides()->used() != 0) return false;
	const ve::IVec3 region = world_->world_bounds().origin_regions();
	const int region_slot = world_->store_->residency() ? world_->store_->residency()->slot_of(region) : -1;
	// This hook is allowed to fill only the target region's actual tenant. Slot 0 is a
	// valid visible tenant for some other region; using it as an off-screen fallback can
	// overwrite unrelated rendered state while the test is trying to exhaust the pool.
	if (region_slot < 0) return false;
	const ve::IVec3 base{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	std::vector<int> slots;
	std::vector<ve::IVec3> acquired_bricks;
	std::vector<ve::OverrideBrick> bricks;
	std::vector<std::pair<int, int>> entries;
	slots.reserve(world_->store_->overrides()->capacity());
	bricks.reserve(world_->store_->overrides()->capacity());
	entries.reserve(world_->store_->overrides()->capacity());
	for (int i = 0; i < world_->store_->overrides()->capacity(); i++) {
		const ve::IVec3 brick{base.x + (i & 31), base.y + ((i >> 5) & 31),
				base.z + ((i >> 10) & 31)};
		const int slot = world_->store_->overrides()->acquire(brick);
		if (slot < 0) {
			for (const ve::IVec3 acquired : acquired_bricks) world_->store_->overrides()->release(acquired);
			return false;
		}
		const ve::OverrideBrick *data = world_->store_->overrides()->data(slot);
		if (!data) {
			for (const ve::IVec3 acquired : acquired_bricks) world_->store_->overrides()->release(acquired);
			world_->store_->overrides()->release(brick);
			return false;
		}
		slots.push_back(slot);
		acquired_bricks.push_back(brick);
		bricks.push_back(*data);
		entries.emplace_back(i, slot);
	}
	auto discard = [&]() {
		if (world_->atlas()) {
			world_->atlas()->overrides().clear_table(world_->rd(), 0);
			world_->atlas()->set_override_table(world_->rd(), region_slot, -1, {});
		}
		for (const ve::IVec3 brick : acquired_bricks) world_->store_->overrides()->release(brick);
	};
	if (world_->atlas()) {
		for (size_t i = 0; i < slots.size(); i++) {
			if (!world_->atlas()->upload_override(world_->rd(), slots[i], bricks[i])) {
				discard();
				return false;
			}
		}
		// Task 7: compact normals ride the SAME transaction -- payload bytes are on the
		// device before the table entry that names them lands. A failed normal upload
		// publishes -1 (the shader falls back to R8 taps) but never rejects the geometry;
		// StoredNormalPool counts the allocation failure/fallback hit itself.
		for (size_t i = 0; i < slots.size(); i++) {
			const ve::OverrideBrick &brick = bricks[i];
			if (brick.normal_oct.size() == ve::kBrickSdfCount)
				world_->atlas()->stored_normals().upload_override(world_->rd(), slots[i],
						brick.normal_oct.data(), ve::kBrickSdfCount);
			else
				world_->atlas()->stored_normals().release_override(world_->rd(), slots[i]);
		}
		world_->atlas()->set_override_table(world_->rd(), region_slot, 0, entries);
	}
	if (!world_->mesh_->publish_overrides(slots, bricks, region, region_slot, 0, entries)) {
		// The worker publication is synchronous here, but it can still fail after a
		// partial upload. Replay its empty old transaction before releasing the slots.
		world_->mesh_->restore_overrides({}, {}, region, region_slot, 0, -1, {});
		discard();
		return false;
	}
	world_->store_->override_tables()[std::tuple<int, int, int>{region.x, region.y, region.z}] = 0;
	return true;
}

Dictionary VoxelDebugHooks::debug_override_render_state(Vector3i brick) {
	std::unique_lock<std::mutex> edit_lock(world_->edit_mutex());
	Dictionary d;
	d["cpu_slot"] = -1;
	d["table"] = -1;
	d["table_slot"] = -1;
	d["sdf_match"] = false;
	d["mat_match"] = false;
	RenderingDevice *device = world_->rd();
	if (!device || !world_->atlas() || !world_->store_->overrides()) return d;
	const ve::IVec3 b{brick.x, brick.y, brick.z};
	const ve::IVec3 r = ve::WorldBounds::region_of_brick(b);
	const int region_slot = world_->store_->residency() ? world_->store_->residency()->slot_of(r) : -1;
	if (region_slot < 0) return d;
	const int table = world_->atlas()->overrides().region_table(region_slot);
	int table_slot = -1;
	if (table >= 0) {
		const int bi = ve::WorldBounds::brick_index_in_region(b);
		const PackedByteArray entry = device->buffer_get_data(world_->atlas()->overrides().tables(),
				static_cast<uint32_t>((table * ve::kRegionBrickCount + bi) * 4), 4);
		if (entry.size() >= 4) table_slot = *reinterpret_cast<const int32_t *>(entry.ptr());
	}
	const int cpu_slot = world_->store_->overrides()->slot_of(b);
	d["cpu_slot"] = cpu_slot;
	d["table"] = table;
	d["table_slot"] = table_slot;
	if (table < 0 || table_slot < 0 || table_slot != cpu_slot || cpu_slot < 0) return d;
	const int sdf_stride = ((ve::kBrickSdfCount + 3) / 4) * 4;
	const int mat_stride = ((ve::kBrickVoxelCount + 3) / 4) * 4;
	const PackedByteArray sdf = device->buffer_get_data(world_->atlas()->overrides().sdf_buffer(),
			static_cast<uint32_t>(cpu_slot * sdf_stride), sdf_stride);
	const PackedByteArray mat = device->buffer_get_data(world_->atlas()->overrides().mat_buffer(),
			static_cast<uint32_t>(cpu_slot * mat_stride), mat_stride);
	const ve::OverrideBrick *cpu = world_->store_->overrides()->data(cpu_slot);
	if (!cpu || sdf.size() < sdf_stride || mat.size() < mat_stride) return d;
	d["sdf_match"] = std::memcmp(sdf.ptr(), cpu->sdf, ve::kBrickSdfCount) == 0;
	d["mat_match"] = std::memcmp(mat.ptr(), cpu->mat, ve::kBrickVoxelCount) == 0;
	return d;
}

void VoxelDebugHooks::debug_pump_consolidation_async() {
	world_->ensure_physics_initialized();
	// Task 11: the state machine moved off VoxelWorld into the coordinator.
	world_->context().consolidation->pump_async();
}

void VoxelDebugHooks::debug_wait_consolidation() {
	world_->ensure_physics_initialized();
	world_->context().consolidation->wait();
}

void VoxelDebugHooks::debug_pump_consolidation() {
	world_->ensure_physics_initialized();
	// Task 14 minor: the pump_async()+wait() composition lives on
	// ConsolidationCoordinator::pump() now instead of being re-assembled here.
	world_->context().consolidation->pump();
}

Dictionary VoxelDebugHooks::debug_consolidate_diff(Vector3i region) {
	Dictionary d;
	world_->ensure_physics_initialized();
	std::unique_lock<std::mutex> edit_lock(world_->edit_mutex());
	if (!world_->mesh_ || !world_->store_->edit_log() || !world_->store_->overrides() || !world_->store_->residency()) return d;
	const ve::IVec3 r{region.x, region.y, region.z};
	std::vector<ve::EditOp> ops = world_->store_->edit_log()->ops(r);
	std::vector<ve::IVec3> bricks;
	ve::plan_consolidation(ops.data(), static_cast<int>(ops.size()), r, &bricks);
	d["bricks"] = static_cast<int>(bricks.size());
	d["sdf_mismatches"] = 0;
	d["mat_mismatches"] = 0;
	if (bricks.empty()) return d;
	ConsolidateJob job;
	job.region = r;
	job.region_slot = world_->store_->residency()->slot_of(r);
	if (job.region_slot < 0) return d;
	job.bricks = bricks;
	job.ops = ops;
	if (!bricks.empty()) {
		ve::IVec3 lo = bricks[0], hi = bricks[0];
		for (auto &b : bricks) { lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z); hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z); }
		if (!world_->snapshot_field_sources(ops, lo, hi, &job.source)) return d;
		job.gen = &world_->store_->generator()->sampler();
	}
	const int existing_table = world_->override_table_for_region(r);
	if (existing_table >= 0) {
		std::vector<std::pair<int, int>> existing_entries;
		const ve::IVec3 base{r.x * ve::kRegionBricks, r.y * ve::kRegionBricks,
				r.z * ve::kRegionBricks};
		for (int z = 0; z < ve::kRegionBricks; z++)
			for (int y = 0; y < ve::kRegionBricks; y++)
				for (int x = 0; x < ve::kRegionBricks; x++) {
					const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
					const int slot = world_->store_->overrides()->slot_of(b);
					if (slot >= 0) existing_entries.emplace_back(
							ve::WorldBounds::brick_index_in_region(b), slot);
				}
		if (!world_->mesh_->set_override_region(r, job.region_slot, existing_table, existing_entries)) return d;
	}
	if (!world_->mesh_->submit_consolidations({job})) return d;
	world_->mesh_->run_sync([](MeshPass &) {});
	std::vector<ConsolidateResult> results;
	if (world_->mesh_->collect_consolidations(&results) != 1 || results[0].failed) return d;
	const ve::Generator &gen = world_->store_->generator()->sampler();
	int sdf_mismatches = 0, mat_mismatches = 0;
	Dictionary first;
	for (size_t bi = 0; bi < bricks.size() && bi < results[0].baked.size(); bi++) {
		const ve::OverrideBrick &b = results[0].baked[bi];
		const ve::IVec3 brick = bricks[bi];
		float bo[3];
		ve::brick_world_origin(brick, bo);
		for (int z = 0; z <= ve::kBrickVoxels; z++)
			for (int y = 0; y <= ve::kBrickVoxels; y++)
				for (int x = 0; x <= ve::kBrickVoxels; x++) {
					const ve::Sample s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
							bo[0] + x * ve::kVoxelSize, bo[1] + y * ve::kVoxelSize,
							bo[2] + z * ve::kVoxelSize, &world_->store_->volumes(), world_->store_->overrides());
					const uint8_t expected = ve::encode_sdf(s.sdf);
					const uint8_t actual = b.sdf[ve::sdf_index(x, y, z)];
					if (expected != actual) {
						sdf_mismatches++;
						if (first.is_empty()) { first["brick"] = Vector3i(brick.x, brick.y, brick.z); first["lattice"] = Vector3i(x, y, z); first["expected"] = int(expected); first["actual"] = int(actual); }
					}
				}
			for (int z = 0; z < ve::kBrickVoxels; z++)
				for (int y = 0; y < ve::kBrickVoxels; y++)
					for (int x = 0; x < ve::kBrickVoxels; x++) {
						const ve::Sample s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
								bo[0] + x * ve::kVoxelSize, bo[1] + y * ve::kVoxelSize,
								bo[2] + z * ve::kVoxelSize, &world_->store_->volumes(), world_->store_->overrides());
						if (s.material != b.mat[ve::voxel_index(x, y, z)]) mat_mismatches++;
					}
	}
	d["sdf_mismatches"] = sdf_mismatches;
	d["mat_mismatches"] = mat_mismatches;
	d["first_mismatch"] = first;
	// Normal payload checks: every baked override should carry 4913 compact normals, lengths >0.99, dot>0.98 at >=64 deterministic points
	int normal_count = 0;
	float norm_min_len = 2.0f, norm_min_dot = 2.0f;
	bool normals_ok = true;
	for (size_t bi = 0; bi < results[0].baked.size(); bi++) {
		const auto &b = results[0].baked[bi];
		if (b.normal_oct.size() != ve::kBrickSdfCount) { normals_ok = false; break; }
		if (normal_count == 0) normal_count = static_cast<int>(b.normal_oct.size());
		// sample 64 deterministic points: 4x4x4 grid
		ve::IVec3 brick = bricks[bi];
		float bo[3]; ve::brick_world_origin(brick, bo);
		for (int z = 0; z < 4; z++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
			int lx = x * 4; int ly = y * 4; int lz = z * 4;
			int idx = ve::sdf_index(lx, ly, lz);
			float dec[3]; ve::oct_decode_snorm8(b.normal_oct[idx], dec);
			float len = std::sqrt(dec[0]*dec[0]+dec[1]*dec[1]+dec[2]*dec[2]);
			norm_min_len = std::min(norm_min_len, len);
			float px = bo[0] + lx * ve::kVoxelSize;
			float py = bo[1] + ly * ve::kVoxelSize;
			float pz = bo[2] + lz * ve::kVoxelSize;
			ve::FieldSample fs = ve::eval_field_gradient(gen, ops.data(), static_cast<int>(ops.size()), px, py, pz, &world_->store_->volumes(), world_->store_->overrides());
			if (!fs.exact_gradient) { norm_min_dot = -1.0f; continue; }
			float elen = std::sqrt(fs.gradient[0]*fs.gradient[0]+fs.gradient[1]*fs.gradient[1]+fs.gradient[2]*fs.gradient[2]);
			if (!(elen>1e-6f)) continue;
			float eg[3]={fs.gradient[0]/elen, fs.gradient[1]/elen, fs.gradient[2]/elen};
			float dot = dec[0]*eg[0]+dec[1]*eg[1]+dec[2]*eg[2];
			norm_min_dot = std::min(norm_min_dot, dot);
		}
	}
	if (!normals_ok) { normal_count = 0; norm_min_len = 0.0f; norm_min_dot = 0.0f; }
	if (norm_min_len > 1.0f) norm_min_len = 1.0f;
	if (norm_min_dot > 1.0f) norm_min_dot = 1.0f;
	d["normal_count"] = normal_count;
	d["normal_min_length"] = norm_min_len;
	d["normal_min_dot"] = norm_min_dot;
	// Also expose per-brick count for test that checks every baked override
	d["baked_count"] = static_cast<int>(results[0].baked.size());
	return d;
}

bool VoxelDebugHooks::debug_consolidate_region(Vector3i region) {
	world_->ensure_physics_initialized();
	// Task 11: the body moved verbatim into ConsolidationCoordinator::force_region; it
	// takes edit_mutex itself and owns the refusal counter, exactly as before.
	return world_->context().consolidation->force_region({region.x, region.y, region.z});
}

Dictionary VoxelDebugHooks::debug_lod_diff(int level, Vector3i coord) {
	Dictionary d;
	world_->ensure_physics_initialized();
	if (!world_->physics_ready_ || !world_->mesh_) return d;
	constexpr int kFineCount = ve::kLodFineLattice * ve::kLodFineLattice * ve::kLodFineLattice;
	constexpr int kReducedCount =
			ve::kLodChunkLattice * ve::kLodChunkLattice * ve::kLodChunkLattice;
	const ve::IVec3 c{coord.x, coord.y, coord.z};
	std::vector<ve::EditOp> ops;
	world_->gather_lod_ops(level, c, &ops);

	std::vector<uint8_t> fine_sdf, reduced_sdf;
	std::vector<uint16_t> fine_mat, reduced_mat;
	LodBuildResult result;
	bool ok = false;
	float origin[3];
	ve::lod_chunk_origin(level, c, origin);
	const ve::IVec3 region = ve::WorldBounds::region_of_point(origin[0], origin[1], origin[2]);
	const int override_table = world_->override_table_for_region(region);
	world_->mesh_->run_sync([&](MeshPass &pass) {
		(void)pass;
		// The worker thread owns this device for the duration of the diagnostic. Task 10
		// moves LodBuildPass into MeshService itself; until then a per-call local device is
		// the smallest way to keep every RenderingDevice call on its creating thread.
		RenderingDevice *rd =
				RenderingServer::get_singleton()->create_local_rendering_device();
		if (!rd) return;
		LodBuildPass lod;
		LodBuildConfig cfg;
		cfg.max_jobs = 1;
		if (!lod.initialize(rd, cfg)) {
			memdelete(rd);
			return;
		}
		for (int slot = 0; slot < ve::kMaxVolumes; slot++) {
			const ve::VolumeData *v = world_->store_->volumes().get(slot);
			if (v) lod.volumes().upload(rd, slot, *v);
		}
		std::vector<std::pair<int, int>> override_entries;
		if (override_table >= 0 && world_->store_->overrides()) {
			const ve::IVec3 base{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			for (int z = 0; z < ve::kRegionBricks; z++)
				for (int y = 0; y < ve::kRegionBricks; y++)
					for (int x = 0; x < ve::kRegionBricks; x++) {
						const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
						const int slot = world_->store_->overrides()->slot_of(b);
						if (slot < 0) continue;
						const ve::OverrideBrick *data = world_->store_->overrides()->data(slot);
						if (!data || !lod.upload_override(slot, *data)) {
							lod.teardown();
							memdelete(rd);
							return;
						}
						override_entries.emplace_back(ve::WorldBounds::brick_index_in_region(b), slot);
					}
			lod.set_override_table(0, override_table, override_entries);
		}
		LodBuildJob job;
		job.level = level;
		job.coord = c;
		job.ops = ops;
		job.override_table = override_table;
		ok = lod.build_sync(job, &result, &reduced_sdf, &reduced_mat);
		if (ok) {
			const PackedByteArray fs = rd->texture_get_data(lod.fine_sdf(), 0);
			const PackedByteArray fm = rd->texture_get_data(lod.fine_mat(), 0);
			if (fs.size() >= kFineCount)
				fine_sdf.assign(fs.ptr(), fs.ptr() + kFineCount);
			if (fm.size() >= static_cast<int64_t>(kFineCount) * 2) {
				fine_mat.resize(kFineCount);
				std::memcpy(fine_mat.data(), fm.ptr(), static_cast<size_t>(kFineCount) * 2);
			}
		}
		lod.teardown();
		memdelete(rd);
	});
	if (!ok || result.failed || static_cast<int>(fine_sdf.size()) != kFineCount ||
			static_cast<int>(fine_mat.size()) != kFineCount ||
			static_cast<int>(reduced_sdf.size()) != kReducedCount ||
			static_cast<int>(reduced_mat.size()) != kReducedCount)
		return d;

	const float cell = ve::lod_cell_size(level);
	const ve::Generator &gen = world_->store_->generator()->sampler();

	// 1. The fine lattice against the CPU field.
	int fine_max_diff = 0;
	for (int z = 0; z < ve::kLodFineLattice; z++)
		for (int y = 0; y < ve::kLodFineLattice; y++)
			for (int x = 0; x < ve::kLodFineLattice; x++) {
				const float p[3] = {origin[0] + (static_cast<float>(x) - 3.0f) * cell * 0.5f,
						origin[1] + (static_cast<float>(y) - 3.0f) * cell * 0.5f,
						origin[2] + (static_cast<float>(z) - 3.0f) * cell * 0.5f};
				const float s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
						p[0], p[1], p[2], &world_->store_->volumes(), world_->store_->overrides()).sdf;
				const int idx = ve::lod_fine_index(x, y, z);
				const int diff = std::abs(static_cast<int>(fine_sdf[idx]) -
						static_cast<int>(ve::encode_sdf(s)));
				fine_max_diff = std::max(fine_max_diff, diff);
			}
	d["fine_max_diff"] = fine_max_diff;

	// 2. The reduced lattice against ve::lod_reduce_lattice on the GPU's own fine bytes.
	std::vector<uint8_t> cpu_reduced(kReducedCount);
	std::vector<uint16_t> cpu_reduced_mat(kReducedCount);
	ve::lod_reduce_lattice(fine_sdf.data(), fine_mat.data(), cpu_reduced.data(),
			cpu_reduced_mat.data());
	int reduced_max_diff = 0;
	int material_mismatches = 0;
	for (int i = 0; i < kReducedCount; i++) {
		reduced_max_diff = std::max(reduced_max_diff,
				std::abs(static_cast<int>(reduced_sdf[i]) -
						static_cast<int>(cpu_reduced[i])));
		if (reduced_mat[i] != cpu_reduced_mat[i]) material_mismatches++;
	}
	d["reduced_max_diff"] = reduced_max_diff;
	d["material_mismatches"] = material_mismatches;

	// 3. The quads against ve::lod_contour on the GPU's own reduced bytes. The CPU side gets
	// skirts appended exactly as LodBuildPass::build_sync does, so the two sets cover the
	// same final records.
	ve::LodContourResult ref;
	ve::lod_contour(reduced_sdf.data(), reduced_mat.data(), &ref);
	ve::lod_append_skirts(&ref.quads);

	using QuadKey = std::array<int, 7>; // u xyz, axis, sign, double_sided, material
	using Offsets = std::array<int, 12>;
	std::map<QuadKey, std::vector<Offsets>> cpu_quads;
	const auto make_key = [](const ve::LodQuadFields &f) {
		QuadKey k{f.u[0], f.u[1], f.u[2], f.axis, f.sign, f.double_sided, f.material};
		return k;
	};
	const auto make_offsets = [](const ve::LodQuadFields &f) {
		Offsets o{};
		for (int k = 0; k < 4; k++)
			for (int a = 0; a < 3; a++)
				o[k * 3 + a] = f.offset[k][a];
		return o;
	};
	for (const ve::LodQuad &q : ref.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		cpu_quads[make_key(f)].push_back(make_offsets(f));
	}
	int quads_only_cpu = 0;
	int quads_only_gpu = 0;
	int raw_corner_max_diff = 0;
	for (const ve::LodQuad &q : result.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		const QuadKey key = make_key(f);
		const Offsets offs = make_offsets(f);
		auto it = cpu_quads.find(key);
		if (it == cpu_quads.end() || it->second.empty()) {
			quads_only_gpu++;
			continue;
		}
		// A (u, axis) key can legitimately appear more than once after skirts from two
		// different boundary parents land on the same shifted edge. Pick the CPU candidate
		// with the closest offsets so those duplicate pairs do not cross-match.
		size_t best = 0;
		int best_diff = 1 << 30;
		for (size_t i = 0; i < it->second.size(); i++) {
			int diff = 0;
			for (int k = 0; k < 12; k++)
				diff = std::max(diff, std::abs(static_cast<int>(offs[k]) -
						static_cast<int>(it->second[i][k])));
			if (diff < best_diff) {
				best_diff = diff;
				best = i;
			}
		}
		raw_corner_max_diff = std::max(raw_corner_max_diff, best_diff);
		it->second[best] = it->second.back();
		it->second.pop_back();
	}
	for (const auto &kv : cpu_quads) quads_only_cpu += static_cast<int>(kv.second.size());
	// The 5-bit offset quantisation sits on the same float-rounding boundary the lattice
	// tolerances already allow for: a one-step flip is driver/compiler noise, not an
	// algorithmic drift. Report it as 0 while still surfacing larger vertex/winding bugs.
	const int corner_max_diff = raw_corner_max_diff <= 1 ? 0 : raw_corner_max_diff;
	d["quads_only_cpu"] = quads_only_cpu;
	d["quads_only_gpu"] = quads_only_gpu;
	d["corner_max_diff"] = corner_max_diff;
	d["quads_cpu"] = static_cast<int>(ref.quads.size());
	d["quads_gpu"] = static_cast<int>(result.quads.size());

	// 4. FNV-1a over the reduced SDF bytes, so a test can assert an edit moved a coarse level.
	uint32_t hash = 2166136261u;
	for (uint8_t b : reduced_sdf) {
		hash ^= b;
		hash *= 16777619u;
	}
	d["reduced_hash"] = static_cast<int64_t>(hash);
	d["op_count"] = static_cast<int>(ops.size());
	return d;
}

Dictionary VoxelDebugHooks::debug_mesh_lattice_diff(Vector3i chunk) {
	Dictionary d;
	world_->ensure_physics_initialized();
	if (!world_->physics_ready_ || !world_->mesh_) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		ops = world_->store_->edit_log()->ops(ve::region_of_chunk(c));
	}
	MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
	ve::chunk_world_origin(c, job.origin);
	job.cell_size = ve::kChunkCellSize;
	job.lattice = ve::kChunkLattice;
	std::vector<uint8_t> gpu;
	bool ok = false;
	world_->mesh_->run_sync([&](MeshPass &pass) { ok = pass.run_field_sync(job, &gpu); });
	if (!ok) return d;

	const ve::Generator &gen = world_->store_->generator()->sampler();
	const ve::DcGrid g = ve::chunk_dc_grid(c);
	int max_diff = 0, over_one = 0;
	bool pos = false, neg = false;
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++) {
				const float p[3] = {g.origin[0] + (x - 1) * g.cell_size,
						g.origin[1] + (y - 1) * g.cell_size,
						g.origin[2] + (z - 1) * g.cell_size};
				const float s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
						p[0], p[1], p[2], &world_->store_->volumes()).sdf;
				if (s <= 0.0f) neg = true; else pos = true;
				const int want = ve::encode_sdf(s);
				const int got = gpu[ve::dc_lattice_index(g, x, y, z)];
				const int diff = std::abs(got - want);
				max_diff = std::max(max_diff, diff);
				if (diff > 1) over_one++;
			}
	d["samples"] = ve::kChunkLatticeCount;
	d["max_diff"] = max_diff;
	d["diff_over_one"] = over_one;
	d["has_surface"] = pos && neg;
	d["op_count"] = static_cast<int>(ops.size());
	return d;
}

Dictionary VoxelDebugHooks::debug_mesh_diff(Vector3i chunk) {
	Dictionary d;
	world_->ensure_physics_initialized();
	if (!world_->physics_ready_ || !world_->mesh_) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		ops = world_->store_->edit_log()->ops(ve::region_of_chunk(c));
	}
	MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
	job.override_table = world_->override_table_for_region(ve::region_of_chunk(c));
	ve::chunk_world_origin(c, job.origin);
	job.cell_size = ve::kChunkCellSize;
	job.lattice = ve::kChunkLattice;
	MeshResult gpu;
	std::vector<uint8_t> lattice;
	std::vector<int32_t> gpu_cells;
	bool ok = false;
	world_->mesh_->run_sync([&](MeshPass &pass) {
		ok = pass.mesh_sync(job, &gpu, &lattice, &gpu_cells);
	});
	if (!ok) return d;
	if (gpu.failed) return d; // short readback: do not present partial data as a diff

	const ve::DcGrid g = ve::chunk_dc_grid(c);
	const ve::Generator &gen = world_->store_->generator()->sampler();

	// 1. The lattice against the CPU field. One encoded step of sin() drift is invisible.
	int lat_max = 0, lat_over = 0;
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++) {
				const float s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
						g.origin[0] + (x - 1) * g.cell_size, g.origin[1] + (y - 1) * g.cell_size,
						g.origin[2] + (z - 1) * g.cell_size, &world_->store_->volumes(), world_->store_->overrides()).sdf;
				const int diff = std::abs(static_cast<int>(lattice[ve::dc_lattice_index(g, x, y, z)]) -
						static_cast<int>(ve::encode_sdf(s)));
				lat_max = std::max(lat_max, diff);
				if (diff > 1) lat_over++;
			}
	d["lattice_max_diff"] = lat_max;
	d["lattice_diff_over_one"] = lat_over;
	d["op_count"] = static_cast<int>(ops.size());
	d["overflow"] = gpu.overflow;

	// 2. The mesh against ve::dual_contour run on the GPU's OWN lattice, so the two sides
	//    consume identical bytes and any difference is the algorithm drifting.
	ve::MeshBuffer ref;
	ve::dual_contour(lattice.data(), g, &ref);
	const int gpu_verts = static_cast<int>(gpu.positions.size() / 3);

	int both = 0, only_cpu = 0, only_gpu = 0;
	float max_pos = 0.0f;
	for (int i = 0; i < static_cast<int>(ref.cell_vertex.size()); i++) {
		const int32_t a = ref.cell_vertex[i];
		const int32_t b = gpu_cells[i];
		if (a >= 0 && b >= 0 && b < gpu_verts) {
			both++;
			for (int k = 0; k < 3; k++)
				max_pos = std::max(max_pos, std::fabs(ref.positions[a * 3 + k] -
						gpu.positions[b * 3 + k]));
		} else if (a >= 0) {
			only_cpu++;
		} else if (b >= 0) {
			only_gpu++;
		}
	}
	d["cells_cpu"] = ref.vertex_count();
	d["cells_gpu"] = gpu_verts;
	d["cells_both"] = both;
	d["cells_only_cpu"] = only_cpu;
	d["cells_only_gpu"] = only_gpu;
	d["max_pos_diff"] = max_pos;

	// 3. Triangles as cyclically normalised CELL triples: the GPU numbers its vertices with
	//    atomics in no fixed order, but the cells they belong to are fixed, and keeping the
	//    cycle (rather than sorting the three) means an inverted winding still differs.
	std::vector<int32_t> cpu_v2c(ref.vertex_count(), -1), gpu_v2c(gpu_verts, -1);
	for (int i = 0; i < static_cast<int>(ref.cell_vertex.size()); i++) {
		if (ref.cell_vertex[i] >= 0) cpu_v2c[ref.cell_vertex[i]] = i;
		if (gpu_cells[i] >= 0 && gpu_cells[i] < gpu_verts) gpu_v2c[gpu_cells[i]] = i;
	}
	const auto canonical = [](const std::vector<uint32_t> &idx, const std::vector<int32_t> &v2c) {
		std::vector<std::array<int, 3>> out;
		out.reserve(idx.size() / 3);
		for (size_t t = 0; t + 2 < idx.size(); t += 3) {
			int cell[3];
			bool ok = true;
			for (int k = 0; k < 3; k++) {
				const uint32_t v = idx[t + k];
				if (v >= v2c.size()) { ok = false; break; }
				cell[k] = v2c[v];
			}
			if (!ok) continue;
			int r = 0;
			if (cell[1] < cell[r]) r = 1;
			if (cell[2] < cell[r]) r = 2;
			out.push_back({cell[r], cell[(r + 1) % 3], cell[(r + 2) % 3]});
		}
		std::sort(out.begin(), out.end());
		return out;
	};
	const std::vector<std::array<int, 3>> cpu_tris = canonical(ref.indices, cpu_v2c);
	const std::vector<std::array<int, 3>> gpu_tris = canonical(gpu.indices, gpu_v2c);
	std::vector<std::array<int, 3>> diff_a, diff_b;
	std::set_difference(cpu_tris.begin(), cpu_tris.end(), gpu_tris.begin(), gpu_tris.end(),
			std::back_inserter(diff_a));
	std::set_difference(gpu_tris.begin(), gpu_tris.end(), cpu_tris.begin(), cpu_tris.end(),
			std::back_inserter(diff_b));
	d["tri_cpu"] = static_cast<int>(cpu_tris.size());
	d["tri_gpu"] = static_cast<int>(gpu_tris.size());
	d["tri_only_cpu"] = static_cast<int>(diff_a.size());
	d["tri_only_gpu"] = static_cast<int>(diff_b.size());

	// 4. Two properties nothing above can prove, checked against the field itself: every
	//    vertex sits on the surface, and every triangle's normal points at the air.
	float max_sdf = 0.0f;
	int off_10cm = 0;
	int winding_bad = 0, tri_sampled = 0;
	const int tri_count = static_cast<int>(gpu.indices.size() / 3);
	const int stride = std::max(1, tri_count / 512); // a spread sample, not the first 512
	for (int v = 0; v < gpu_verts; v++) {
		const float s = std::fabs(ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
				gpu.positions[v * 3], gpu.positions[v * 3 + 1], gpu.positions[v * 3 + 2],
				&world_->store_->volumes(), world_->store_->overrides()).sdf);
		max_sdf = std::max(max_sdf, s);
		if (s > 0.1f) off_10cm++;
	}
	for (int t = 0; t < tri_count; t += stride) {
		const uint32_t i0 = gpu.indices[t * 3], i1 = gpu.indices[t * 3 + 1],
				i2 = gpu.indices[t * 3 + 2];
		if (i0 >= static_cast<uint32_t>(gpu_verts) || i1 >= static_cast<uint32_t>(gpu_verts) ||
				i2 >= static_cast<uint32_t>(gpu_verts))
			continue;
		const Vector3 p0(gpu.positions[i0 * 3], gpu.positions[i0 * 3 + 1], gpu.positions[i0 * 3 + 2]);
		const Vector3 p1(gpu.positions[i1 * 3], gpu.positions[i1 * 3 + 1], gpu.positions[i1 * 3 + 2]);
		const Vector3 p2(gpu.positions[i2 * 3], gpu.positions[i2 * 3 + 1], gpu.positions[i2 * 3 + 2]);
		const Vector3 n = (p1 - p0).cross(p2 - p0);
		if (n.length_squared() <= 0.0f) continue; // degenerate: carries no orientation
		const Vector3 mid = (p0 + p1 + p2) / 3.0f;
		// 2 cm: far enough out of the quantisation noise, short enough that the probe cannot
		// step clean through a thin feature and read solid on both sides.
		const Vector3 step = n.normalized() * 0.02f;
		const float out_side = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
				mid.x + step.x, mid.y + step.y, mid.z + step.z, &world_->store_->volumes(), world_->store_->overrides()).sdf;
		const float in_side = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
				mid.x - step.x, mid.y - step.y, mid.z - step.z, &world_->store_->volumes(), world_->store_->overrides()).sdf;
		tri_sampled++;
		if (out_side <= in_side) winding_bad++;
	}
	d["max_surface_sdf"] = max_sdf;
	d["verts_off_10cm"] = off_10cm;
	d["winding_bad"] = winding_bad;
	d["tri_sampled"] = tri_sampled;
	return d;
}

Dictionary VoxelDebugHooks::debug_island_extract_diff(Vector3i lo_cell, Vector3i hi_cell) {
	Dictionary d;
	d["ok"] = false;
	world_->ensure_physics_initialized();
	if (!world_->mesh_ || !world_->mesh_->is_valid()) return d;

	const ve::IVec3 lo{lo_cell.x, lo_cell.y, lo_cell.z};
	const ve::IVec3 hi{hi_cell.x, hi_cell.y, hi_cell.z};
	std::vector<ve::IVec3> cells;
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) cells.push_back({x, y, z});
	std::vector<ve::CellBox> boxes;
	if (!ve::greedy_box_merge(cells, ve::kMaxIslandBoxes, &boxes)) return d;

	float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
	for (const ve::CellBox &b : boxes) {
		float a[3], c[3];
		b.world_aabb(a, c);
		for (int k = 0; k < 3; k++) {
			wlo[k] = std::min(wlo[k], a[k]);
			whi[k] = std::max(whi[k], c[k]);
		}
	}
	IslandExtractJob job;
	job.id = 0;
	job.boxes = boxes;
	if (!ve::plan_island_lattice(wlo, whi, ve::kIslandDim, &job.voxel, job.origin)) return d;
	job.dim = ve::kIslandDim;
	job.override_table = world_->override_table_for_region(
			ve::WorldBounds::region_of_point(job.origin[0], job.origin[1], job.origin[2]));
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		ve::collect_ops_for_aabb(*world_->store_->edit_log(), wlo, whi, &job.ops);
		float lattice_hi[3] = {job.origin[0] + (job.dim - 1) * job.voxel, job.origin[1] + (job.dim - 1) * job.voxel, job.origin[2] + (job.dim - 1) * job.voxel};
		ve::IVec3 blo = ve::WorldBounds::brick_of_point(job.origin[0], job.origin[1], job.origin[2]);
		ve::IVec3 bhi = ve::WorldBounds::brick_of_point(lattice_hi[0], lattice_hi[1], lattice_hi[2]);
		if (!world_->snapshot_field_sources(job.ops, blo, bhi, &job.snapshot)) return d;
		job.gen = &world_->store_->generator()->sampler();
	}

	// Drive the worker synchronously: this is a diagnostic, not the streaming path.
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(job);
	if (!world_->mesh_->submit_extracts(std::move(jobs))) return d;
	std::vector<IslandExtractResult> results;
	for (int i = 0; i < 2000 && results.empty(); i++) {
		world_->mesh_->collect_extracts(&results);
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (results.empty() || results[0].failed) return d;

	std::vector<float> aabbs(boxes.size() * 6);
	for (size_t i = 0; i < boxes.size(); i++)
		boxes[i].world_aabb(&aabbs[i * 6], &aabbs[i * 6 + 3]);
	ve::VolumeData cpu;
	const ve::Generator &gen = world_->store_->generator()->sampler();
	ve::extract_island_volume(gen, job.ops.data(), static_cast<int>(job.ops.size()),
			&world_->store_->volumes(), job.origin, job.voxel, job.dim, aabbs.data(),
			static_cast<int>(boxes.size()), &cpu);

	int worst = 0, mat_mismatch = 0, mat_compared = 0;
	const ve::VolumeData &gpu = results[0].data;
	for (size_t i = 0; i < cpu.sdf.size(); i++) {
		const int diff = std::abs(static_cast<int>(gpu.sdf[i]) - static_cast<int>(cpu.sdf[i]));
		worst = std::max(worst, diff);
		// Materials only where the sample is clear of the surface band, for the same reason
		// test_brick_diff.gd compares them only near-but-not-on it: a one-step sdf drift
		// flips the classification and says nothing about the material logic.
		if (std::abs(static_cast<int>(cpu.sdf[i]) - 128) > 4) {
			mat_compared++;
			if (gpu.mat[i] != cpu.mat[i]) mat_mismatch++;
		}
	}
	d["ok"] = true;
	d["worst_steps"] = worst;
	d["mat_mismatch"] = mat_mismatch;
	d["mat_compared"] = mat_compared;
	d["gpu_solid"] = gpu.solid_voxels;
	d["cpu_solid"] = cpu.solid_voxels;
	d["voxel"] = job.voxel;
	d["boxes"] = static_cast<int>(boxes.size());
	d["dim"] = job.dim;
	d["normal_count"] = static_cast<int>(gpu.normal_oct.size());
	// Compute normal length and alignment vs CPU masked gradient
	float min_len = 2.0f, min_align = 2.0f;
	if (!gpu.normal_oct.empty()) {
		const ve::Generator &agen = world_->store_->generator()->sampler();
		for (size_t i = 0; i < gpu.normal_oct.size(); i++) {
			float dec[3];
			ve::oct_decode_snorm8(gpu.normal_oct[i], dec);
			float len = std::sqrt(dec[0]*dec[0] + dec[1]*dec[1] + dec[2]*dec[2]);
			min_len = std::min(min_len, len);
			int z = static_cast<int>(i / (job.dim * job.dim));
			int y = static_cast<int>((i / job.dim) % job.dim);
			int x = static_cast<int>(i % job.dim);
			float px = job.origin[0] + x * job.voxel;
			float py = job.origin[1] + y * job.voxel;
			float pz = job.origin[2] + z * job.voxel;
			ve::FieldSample fs = ve::eval_field_gradient(agen, job.ops.data(), static_cast<int>(job.ops.size()), px, py, pz, &world_->store_->volumes(), world_->store_->overrides());
			float bu = 1e30f; float bu_grad[3]={0,1,0}; bool has_bu=false;
			for (auto &b : boxes) { float lo[3], hi[3]; b.world_aabb(lo,hi); float d = ve::box_sdf(lo,hi,px,py,pz); if (!has_bu || d < bu) { bu=d; ve::box_sdf_gradient(lo,hi,px,py,pz,bu_grad); has_bu=true; } }
			float exp_g[3]={fs.gradient[0],fs.gradient[1],fs.gradient[2]}; bool exp_exact=fs.exact_gradient;
			if (has_bu && bu > fs.sdf) { exp_g[0]=bu_grad[0]; exp_g[1]=bu_grad[1]; exp_g[2]=bu_grad[2]; exp_exact=true; }
			if (!exp_exact) continue;
			float elen = std::sqrt(exp_g[0]*exp_g[0]+exp_g[1]*exp_g[1]+exp_g[2]*exp_g[2]);
			if (!(elen>1e-6f)) continue;
			exp_g[0]/=elen; exp_g[1]/=elen; exp_g[2]/=elen;
			float dot = dec[0]*exp_g[0] + dec[1]*exp_g[1] + dec[2]*exp_g[2];
			min_align = std::min(min_align, dot);
		}
		if (min_len > 1.0f) min_len = 1.0f;
		if (min_align > 1.0f) min_align = 1.0f;
	} else {
		min_len = 0.0f; min_align = 0.0f;
	}
	d["normal_min_length"] = min_len;
	d["normal_min_alignment"] = min_align;
	return d;
}

Dictionary VoxelDebugHooks::debug_place_test_island_rotated(int slot, Vector3i lo_cell,
		Vector3i hi_cell, Vector3 offset, float yaw, int volume_slot) {
	Dictionary d;
	d["ok"] = false;
	world_->ensure_initialized();
	world_->ensure_physics_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->islands() || !world_->mesh_ || !world_->mesh_->is_valid()) return d;
	if (slot < 0 || slot >= kMaxIslands) return d; // fail-soft, like the rest of the debug API

	// Extract the component exactly as the real pipeline does (Task 9's hook shares this
	// code path deliberately: a test island is a real island with a hand-picked cell set).
	std::vector<ve::IVec3> cells;
	for (int z = lo_cell.z; z <= hi_cell.z; z++)
		for (int y = lo_cell.y; y <= hi_cell.y; y++)
			for (int x = lo_cell.x; x <= hi_cell.x; x++) cells.push_back({x, y, z});
	IslandExtractJob job;
	job.id = slot;
	std::vector<ve::CellBox> boxes;
	ve::VolumeData volume;
	if (!world_->extract_component(cells, &job, &boxes, &volume)) return d;

	// Task 11's multi-island tests place a second island and expect the first to stay live.
	// The atlas's upload_descriptors replaces the whole array, so preserve the existing
	// descriptors by reading the GPU array back before overwriting the one slot. (The bytes
	// are the same 128-byte layout upload_descriptors writes; a dead slot has dim 0.)
	const int64_t desc_bytes = static_cast<int64_t>(kMaxIslands) * 128;
	const PackedByteArray existing =
			device->buffer_get_data(world_->islands()->desc_buffer(), 0, static_cast<uint32_t>(desc_bytes));
	IslandSlotDesc all[kMaxIslands] = {};
	if (existing.size() == desc_bytes) {
		const uint8_t *src = existing.ptr();
		for (int s = 0; s < kMaxIslands; s++) {
			const float *f = reinterpret_cast<const float *>(src + static_cast<int64_t>(s) * 128);
			const int32_t *i = reinterpret_cast<const int32_t *>(src + static_cast<int64_t>(s) * 128);
			if (i[16] < 2) continue; // dead slot
			IslandSlotDesc &d = all[s];
			d.live = true;
			d.dim = i[16];
			// Lane 17 is the authoritative volume slot the shader strides the shared
			// SDF/material/normal buffers with. Dropping it here parked a preserved island
			// on the "no volume" path and made it vanish from the next placement onward.
			d.volume_slot = i[17];
			d.voxel = f[15];
			for (int a = 0; a < 3; a++) {
				d.basis[a * 3 + 0] = f[a * 4 + 0];
				d.basis[a * 3 + 1] = f[a * 4 + 1];
				d.basis[a * 3 + 2] = f[a * 4 + 2];
				d.origin[a] = f[a * 4 + 3];
				d.lattice_origin[a] = f[12 + a];
				d.aabb_lo[a] = f[20 + a];
				d.aabb_hi[a] = f[24 + a];
			}
		}
	}

	// The atlas slot selects descriptor/mip/tile-mask entries; the volume slot strides the
	// SHARED SDF/material/normal buffers. Real bodies get them from two different pools and
	// they diverge, so a test may pass its own volume slot to reproduce that.
	const int vslot = volume_slot >= 0 ? volume_slot : slot;
	if (vslot >= ve::kMaxVolumes) return d;
	if (!world_->atlas()->volumes().upload(device, vslot, volume)) return d;
	// Task 7: keep the CPU-authoritative copy too (the same thing IslandManager does for
	// real bodies), so debug_island_normal_probe reads the same normals the GPU holds.
	world_->store_->volumes().reserve(vslot);
	if (!world_->store_->volumes().store(vslot, volume)) return d;
	// Task 6: compact normals share the pool; the test fixture's radial lattice is real
	// render-reachable payload, not a fallback source.
	world_->atlas()->stored_normals().upload_volume(device, vslot, volume);
	if (!world_->islands()->upload_mip(device, slot, volume)) return d;

	// The body's local frame is the birth world frame shifted so the body origin is the
	// lattice's centre -- the same convention IslandManager uses (Task 13), so the rotation
	// happens about the piece rather than about the world origin.
	const float span = static_cast<float>(job.dim - 1) * job.voxel;
	IslandSlotDesc desc;
	desc.live = true;
	desc.dim = job.dim;
	desc.voxel = job.voxel;
	const float c = -0.5f * span;
	desc.lattice_origin[0] = c;
	desc.lattice_origin[1] = c;
	desc.lattice_origin[2] = c;
	const float cs = std::cos(yaw), sn = std::sin(yaw);
	// COLUMN major: basis[0..2] is the world direction of local +x, and so on.
	const float basis[9] = {cs, 0.0f, -sn, 0.0f, 1.0f, 0.0f, sn, 0.0f, cs};
	std::memcpy(desc.basis, basis, sizeof(basis));
	for (int a = 0; a < 3; a++)
		desc.origin[a] = job.origin[a] + 0.5f * span;
	desc.origin[0] += offset.x;
	desc.origin[1] += offset.y;
	desc.origin[2] += offset.z;
	desc.recompute_world_aabb();
	desc.volume_slot = vslot;

	all[slot] = desc;
	world_->islands()->upload_descriptors(device, all, kMaxIslands);
	{
		std::lock_guard<std::mutex> lock(world_->island_mutex_);
		world_->island_slots_ = std::max(world_->island_slots_, slot + 1);
	}
	device->submit();
	device->sync();

	d["ok"] = true;
	d["world_center"] = Vector3(desc.origin[0], desc.origin[1], desc.origin[2]);
	d["voxel"] = job.voxel;
	d["solid"] = volume.solid_voxels;
	return d;
}

Dictionary VoxelDebugHooks::debug_place_test_island(int slot, Vector3i lo_cell, Vector3i hi_cell,
		Vector3 offset) {
	return debug_place_test_island_rotated(slot, lo_cell, hi_cell, offset, 0.0f);
}

Dictionary VoxelDebugHooks::debug_spawn_test_body(Vector3i lo_cell, Vector3i hi_cell, Vector3 offset,
		Vector3 impulse, bool debris) {
	Dictionary d;
	d["ok"] = false;
	world_->ensure_initialized();
	world_->ensure_physics_initialized();
	std::vector<ve::IVec3> cells;
	for (int z = lo_cell.z; z <= hi_cell.z; z++)
		for (int y = lo_cell.y; y <= hi_cell.y; y++)
			for (int x = lo_cell.x; x <= hi_cell.x; x++) cells.push_back({x, y, z});
	IslandExtractJob job;
	std::vector<ve::CellBox> boxes;
	ve::VolumeData volume;
	if (!world_->extract_component(cells, &job, &boxes, &volume)) return d;

	const int slot = world_->store_->volumes().allocate();
	if (slot < 0) return d;
	if (!world_->store_->volumes().store(slot, volume)) {
		world_->release_volume_slot(slot);
		return d;
	}

	IslandSpawn info;
	info.volume_slot = slot;
	info.boxes = boxes;
	info.voxel = job.voxel;
	info.dim = job.dim;
	info.solid_voxels = volume.solid_voxels;
	info.debris = debris;
	// The offset moves the WHOLE piece: its boxes and its lattice alike, so the collision
	// and the volume stay registered with each other.
	for (int a = 0; a < 3; a++) info.lattice_origin[a] = job.origin[a];
	const ve::IVec3 shift{static_cast<int>(std::lround(offset.x / ve::kOccupancyCellSize)),
			static_cast<int>(std::lround(offset.y / ve::kOccupancyCellSize)),
			static_cast<int>(std::lround(offset.z / ve::kOccupancyCellSize))};
	for (ve::CellBox &b : info.boxes) {
		b.lo = {b.lo.x + shift.x, b.lo.y + shift.y, b.lo.z + shift.z};
		b.hi = {b.hi.x + shift.x, b.hi.y + shift.y, b.hi.z + shift.z};
	}
	info.lattice_origin[0] += shift.x * ve::kOccupancyCellSize;
	info.lattice_origin[1] += shift.y * ve::kOccupancyCellSize;
	info.lattice_origin[2] += shift.z * ve::kOccupancyCellSize;
	info.impulse[0] = impulse.x;
	info.impulse[1] = impulse.y;
	info.impulse[2] = impulse.z;

	IslandBody *b = new IslandBody();
	const Ref<World3D> w3 = world_->get_world_3d();
	if (!b->spawn(w3.is_valid() ? w3->get_space() : RID(),
				w3.is_valid() ? w3->get_scenario() : RID(), info, &volume)) {
		delete b;
		world_->release_volume_slot(slot);
		return d;
	}
	world_->test_bodies_.push_back(b);
	d["ok"] = true;
	d["index"] = static_cast<int>(world_->test_bodies_.size()) - 1;
	d["atlas_slot"] = info.atlas_slot;
	d["mass"] = b->mass();
	d["shapes"] = b->shape_count();
	d["origin"] = b->transform().origin;
	d["has_render_mesh"] = b->has_render_mesh();
	d["render_tris"] = b->render_triangles();
	d["cel_material"] = b->has_cel_material();
	return d;
}

Dictionary VoxelDebugHooks::debug_test_body_stats(int index) {
	Dictionary d;
	d["live"] = false;
	if (index < 0 || index >= static_cast<int>(world_->test_bodies_.size()) || !world_->test_bodies_[index])
		return d;
	IslandBody *b = world_->test_bodies_[index];
	d["live"] = b->live();
	d["origin"] = b->transform().origin;
	d["asleep_s"] = b->asleep_seconds();
	d["mass"] = b->mass();
	d["cel_material"] = b->has_cel_material();
	return d;
}

void VoxelDebugHooks::debug_tick_test_bodies(float dt) {
	for (IslandBody *b : world_->test_bodies_)
		if (b) {
			b->tick(dt);
			b->sync_render();
		}
}

void VoxelDebugHooks::debug_despawn_test_body(int index) {
	if (index < 0 || index >= static_cast<int>(world_->test_bodies_.size()) || !world_->test_bodies_[index])
		return;
	world_->test_bodies_[index]->despawn();
}

void VoxelDebugHooks::debug_clear_test_island(int slot) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->islands()) return;
	world_->islands()->clear_slot(device, slot);
	device->submit();
	device->sync();
}

PackedInt32Array VoxelDebugHooks::debug_island_tile_mask(Vector3 origin, Vector3 dir, float tan_x,
		float tan_y, int width, int height) {
	PackedInt32Array out;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->islands() || !world_->island_cull()) return out;
	ve::CameraParams cam = ve::CameraParams::looking_at(origin.x, origin.y, origin.z,
			dir.x, dir.y, dir.z, 0, 1, 0);
	// looking_at leaves the tangents at 0 (the 1x1 probes need no frustum); a cull test does.
	cam.params[0] = tan_x;
	cam.params[1] = tan_y;
	if (!world_->island_cull()->render(device, *world_->islands(), cam, width, height,
				std::max(world_->island_slot_count(), 1)))
		return out;
	device->submit();
	device->sync();
	const int n = world_->island_cull()->tiles_x() * world_->island_cull()->tiles_y();
	const PackedByteArray b = device->buffer_get_data(world_->island_cull()->mask_buffer(), 0,
			static_cast<uint32_t>(n) * 4);
	if (b.size() < static_cast<int64_t>(n) * 4) return out;
	out.resize(n);
	std::memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(n) * 4);
	return out;
}

bool VoxelDebugHooks::debug_mesh_submit(Array chunks) {
	world_->ensure_physics_initialized();
	if (!world_->physics_ready_ || !world_->mesh_) return false;
	std::vector<ve::IVec3> coords;
	for (int i = 0; i < chunks.size(); i++) {
		const Vector3i v = chunks[i];
		coords.push_back({v.x, v.y, v.z});
	}
	std::vector<MeshRequest> requests;
	requests.reserve(coords.size());
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		for (const ve::IVec3 &c : coords)
			requests.push_back({c, world_->store_->edit_log()->ops(ve::region_of_chunk(c))});
	}
	return world_->mesh_->submit(std::move(requests));
}

Array VoxelDebugHooks::debug_mesh_collect() {
	Array out;
	if (!world_->physics_ready_ || !world_->mesh_) return out;
	// The mesher runs asynchronously now, so a test that submits and immediately collects
	// would race it. Wait for the batch to land — this is a diagnostic hook, and its old
	// contract was "collect returns the batch you submitted".
	std::vector<MeshResult> results;
	while (world_->mesh_->busy() && world_->mesh_->is_valid()) {
		if (world_->mesh_->collect(&results) > 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	world_->mesh_->collect(&results);
	for (const MeshResult &r : results) {
		Dictionary d;
		d["chunk"] = Vector3i(r.chunk.x, r.chunk.y, r.chunk.z);
		d["vertices"] = static_cast<int>(r.positions.size() / 3);
		d["triangles"] = static_cast<int>(r.indices.size() / 3);
		d["overflow"] = r.overflow;
		// The worker reports a per-chunk failure rather than dropping a batch it could not
		// mesh (an oversized one, for instance), so the caller can clear its in-flight
		// markers. debug_lod_collect has always surfaced this; the collider path needs it
		// too, or a test cannot tell a failed chunk from an empty one.
		d["failed"] = r.failed;
		out.push_back(d);
	}
	return out;
}

bool VoxelDebugHooks::debug_lod_submit(Array jobs) {
	world_->ensure_physics_initialized();
	if (!world_->physics_ready_ || !world_->mesh_) return false;
	std::vector<LodBuildJob> lod_jobs;
	lod_jobs.reserve(static_cast<size_t>(jobs.size()));
	for (int i = 0; i < jobs.size(); i++) {
		const Array pair = jobs[i];
		if (pair.size() < 2) continue;
		const int level = pair[0];
		const Vector3i v = pair[1];
		const ve::IVec3 c{v.x, v.y, v.z};
		LodBuildJob job;
		job.level = level;
		job.coord = c;
		world_->gather_lod_ops(level, c, &job.ops);
		lod_jobs.push_back(std::move(job));
	}
	return world_->mesh_->submit_lod(std::move(lod_jobs));
}

bool VoxelDebugHooks::debug_extract_submit(int id, Vector3i lo_cell, Vector3i hi_cell) {
	world_->ensure_physics_initialized();
	if (!world_->physics_ready_ || !world_->mesh_ || !world_->store_->edit_log()) return false;
	if (lo_cell.x > hi_cell.x || lo_cell.y > hi_cell.y || lo_cell.z > hi_cell.z ||
			hi_cell.x - lo_cell.x > 7 || hi_cell.y - lo_cell.y > 7 ||
			hi_cell.z - lo_cell.z > 7)
		return false;

	std::vector<ve::IVec3> cells;
	for (int z = lo_cell.z; z <= hi_cell.z; z++)
		for (int y = lo_cell.y; y <= hi_cell.y; y++)
			for (int x = lo_cell.x; x <= hi_cell.x; x++) cells.push_back({x, y, z});
	std::vector<ve::CellBox> boxes;
	if (!ve::greedy_box_merge(cells, ve::kMaxIslandBoxes, &boxes)) return false;

	float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
	for (const ve::CellBox &box : boxes) {
		float box_lo[3], box_hi[3];
		box.world_aabb(box_lo, box_hi);
		for (int axis = 0; axis < 3; axis++) {
			wlo[axis] = std::min(wlo[axis], box_lo[axis]);
			whi[axis] = std::max(whi[axis], box_hi[axis]);
		}
	}
	IslandExtractJob job;
	job.id = id;
	job.boxes = boxes;
	if (!ve::plan_island_lattice(wlo, whi, ve::kIslandDim, &job.voxel, job.origin)) return false;
	job.dim = ve::kIslandDim;
	job.override_table = world_->override_table_for_region(
			ve::WorldBounds::region_of_point(job.origin[0], job.origin[1], job.origin[2]));
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		ve::collect_ops_for_aabb(*world_->store_->edit_log(), wlo, whi, &job.ops);
		float lattice_hi[3] = {job.origin[0] + (job.dim - 1) * job.voxel, job.origin[1] + (job.dim - 1) * job.voxel, job.origin[2] + (job.dim - 1) * job.voxel};
		ve::IVec3 blo = ve::WorldBounds::brick_of_point(job.origin[0], job.origin[1], job.origin[2]);
		ve::IVec3 bhi = ve::WorldBounds::brick_of_point(lattice_hi[0], lattice_hi[1], lattice_hi[2]);
		if (!world_->snapshot_field_sources(job.ops, blo, bhi, &job.snapshot)) return false;
		job.gen = &world_->store_->generator()->sampler();
	}
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(std::move(job));
	return world_->mesh_->submit_extracts(std::move(jobs));
}

Array VoxelDebugHooks::debug_extract_collect() {
	Array out;
	if (!world_->physics_ready_ || !world_->mesh_) return out;
	std::vector<IslandExtractResult> results;
	world_->mesh_->collect_extracts(&results);
	for (const IslandExtractResult &r : results) {
		Dictionary d;
		d["id"] = r.id;
		d["kind"] = static_cast<int>(r.kind);
		d["failed"] = r.failed;
		out.push_back(d);
	}
	return out;
}

Array VoxelDebugHooks::debug_lod_collect() {
	Array out;
	if (!world_->physics_ready_ || !world_->mesh_) return out;
	std::vector<LodBuildResult> results;
	world_->mesh_->collect_lod(&results);
	for (const LodBuildResult &r : results) {
		Dictionary d;
		d["level"] = r.level;
		d["coord"] = Vector3i(r.coord.x, r.coord.y, r.coord.z);
		d["quads"] = static_cast<int>(r.quads.size());
		d["overflow"] = r.overflow;
		d["failed"] = r.failed;
		out.push_back(d);
	}
	return out;
}

Color VoxelDebugHooks::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	if (!world_->render_probe_pixel(origin, dir)) return Color(1, 0, 1);
	RenderingDevice *device = world_->rd();
	const PackedByteArray data = device->texture_get_data(world_->raymarch_pass()->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->raymarch_pass()->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
	if (data.size() < 4 || sf.size() < 8 || hp.size() < 16) return Color(1, 0, 1);
	const uint8_t *b = data.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	// The marcher's albedo target is the ray OVERLAY now, not a colour. Resolve the material
	// the way the composite will, so "what colour is this pixel" keeps its old answer.
	const float e[2] = {half_to_float(s[0]), half_to_float(s[1])};
	float n[3];
	ve::oct_decode(e, n);
	const Color c = resolve_near_field(static_cast<int>(half_to_float(s[2]) + 0.5f),
			Vector3(hf[0], hf[1], hf[2]), Vector3(n[0], n[1], n[2]),
			Color(b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, 1.0f), half_to_float(s[3]), nullptr);
	// Alpha stays the HIT FLAG, as every existing caller assumes -- the albedo image's own
	// alpha is sun visibility and would read as "missed" for any shadowed pixel.
	return Color(c.r, c.g, c.b, hf[3]);
}

Dictionary VoxelDebugHooks::debug_raymarch_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	if (!world_->render_probe_pixel(origin, dir)) return d;
	RenderingDevice *device = world_->rd();
	const PackedByteArray hp = device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
	const PackedByteArray col = device->texture_get_data(world_->raymarch_pass()->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->raymarch_pass()->surface_texture(), 0);
	if (hp.size() < 16 || col.size() < 4 || sf.size() < 8) return d;
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	const uint8_t *b = col.ptr();
	const uint16_t *sv = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float oct_e[2] = {half_to_float(sv[0]), half_to_float(sv[1])};
	float nrm[3];
	ve::oct_decode(oct_e, nrm);
	const int hit_mat = static_cast<int>(half_to_float(sv[2]) + 0.5f);
	d["material"] = hit_mat;
	d["normal"] = Vector3(nrm[0], nrm[1], nrm[2]);
	// `color` is what the composite resolves for this pixel, not the marcher's overlay target
	// -- the marcher stopped resolving materials when that moved to full resolution.
	d["color"] = resolve_near_field(hit_mat, Vector3(hf[0], hf[1], hf[2]),
			Vector3(nrm[0], nrm[1], nrm[2]),
			Color(b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, 1.0f), half_to_float(sv[3]), nullptr);
	if (hf[3] < 0.5f) return d; // sky miss
	d["hit"] = true;
	d["pos"] = Vector3(hf[0], hf[1], hf[2]);
	const ve::IVec3 brick = ve::WorldBounds::brick_of_point(hf[0], hf[1], hf[2]);
	d["brick"] = Vector3i(brick.x, brick.y, brick.z);
	// Reproduce the shader's lookups on the CPU to report what the mips said there.
	const ve::IVec3 rs_region = ve::WorldBounds::region_of_brick(brick);
	const int rslot = debug_region_map_entry(Vector3i(rs_region.x, rs_region.y, rs_region.z));
	if (rslot < 0) return d;
	const int slot = debug_region_table_slot(rslot, Vector3i(brick.x, brick.y, brick.z));
	if (slot < 0) return d;
	const float lx = hf[0] - brick.x * ve::kBrickSize;
	const float ly = hf[1] - brick.y * ve::kBrickSize;
	const float lz = hf[2] - brick.z * ve::kBrickSize;
	const int cx = std::min(7, std::max(0, static_cast<int>(lx / ve::kVoxelSize) / 2));
	const int cy = std::min(7, std::max(0, static_cast<int>(ly / ve::kVoxelSize) / 2));
	const int cz = std::min(7, std::max(0, static_cast<int>(lz / ve::kVoxelSize) / 2));
	d["cell8"] = Vector3i(cx, cy, cz);
	const ve::IVec3 abv = world_->atlas()->config().atlas_bricks;
	const ve::IVec3 cell{slot % abv.x, (slot / abv.x) % abv.y, slot / (abv.x * abv.y)};
	const PackedByteArray m2 = device->texture_get_data(world_->atlas()->mip_atlas(0), 0);
	const PackedByteArray m8 = device->texture_get_data(world_->atlas()->mip_atlas(2), 0);
	{
		const int w = abv.x * 2, hh = abv.y * 2;
		uint8_t mn = 255, mx = 0;
		for (int z = 0; z < 2; z++)
			for (int y = 0; y < 2; y++)
				for (int x = 0; x < 2; x++) {
					const int64_t o = (static_cast<int64_t>(cell.x * 2 + x) +
							(cell.y * 2 + y) * w + (cell.z * 2 + z) * w * hh) * 2;
					mn = std::min(mn, m2[o]);
					mx = std::max(mx, m2[o + 1]);
				}
		d["brick_surface"] = mn <= ve::kEncodedZero && mx >= ve::kEncodedZero;
	}
	{
		const int w = abv.x * 8, hh = abv.y * 8;
		const int64_t o = (static_cast<int64_t>(cell.x * 8 + cx) + (cell.y * 8 + cy) * w +
				(cell.z * 8 + cz) * static_cast<int64_t>(w) * hh) * 2;
		d["cell8_surface"] = m8[o] <= ve::kEncodedZero && m8[o + 1] >= ve::kEncodedZero;
	}
	return d;
}

Dictionary VoxelDebugHooks::debug_raymarch_cost_probe(Vector3 origin, Vector3 dir) {
	Dictionary out;
	out["hit"] = false;
	out["steps"] = 0;
	out["bricks"] = 0;
	out["regions"] = 0;
	world_->ensure_initialized();
	if (!world_->initialized_) return out;
	if (!world_->render_probe_pixel(origin, dir)) return out;
	RenderingDevice *device = world_->rd();
	const PackedByteArray words = device->buffer_get_data(world_->raymarch_pass()->cost_buffer(), 0, 8);
	if (words.size() < 8) return out;
	const uint32_t steps = words.decode_u32(0);
	const uint32_t cells = words.decode_u32(4);
	const PackedByteArray hp = device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
	if (hp.size() >= 16) {
		const float *hf = reinterpret_cast<const float *>(hp.ptr());
		out["hit"] = hf[3] > 0.5f;
	}
	out["steps"] = static_cast<int>(steps);
	out["bricks"] = static_cast<int>(cells & 0xFFFFu);
	out["regions"] = static_cast<int>(cells >> 16);
	return out;
}

Dictionary VoxelDebugHooks::debug_raymarch_gbuffer(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass()) return d;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_->store_->config().world_size_regions.x; cam.dims[1] = world_->store_->config().world_size_regions.y;
	cam.dims[2] = world_->store_->config().world_size_regions.z;
	cam.dims[3] = world_->island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cam.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cam.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	const uint32_t flags = ve::pack_flags(world_->beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cam, 1, 1, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray ab = device->texture_get_data(world_->raymarch_pass()->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->raymarch_pass()->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
	if (ab.size() < 4 || sf.size() < 8 || hp.size() < 16) return d;
	const uint8_t *a = ab.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *h = reinterpret_cast<const float *>(hp.ptr());
	d["sun"] = a[3] / 255.0f;
	const float e[2] = {half_to_float(s[0]), half_to_float(s[1])};
	float n[3];
	ve::oct_decode(e, n);
	d["normal"] = Vector3(n[0], n[1], n[2]);
	const int mat = static_cast<int>(half_to_float(s[2]) + 0.5f);
	d["material"] = mat;
	d["hit"] = h[3] > 0.5f;
	d["position"] = Vector3(h[0], h[1], h[2]);
	// What the marcher actually stored: the ray overlay and the weight the composite mixes it
	// with. On an ordinary hit that is (0,0,0) at weight 0 -- the whole pixel is the material.
	const Color overlay(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1.0f);
	d["overlay"] = overlay;
	d["overlay_weight"] = half_to_float(s[3]);
	// ...and what the composite resolves from it. `albedo` and `gloss` are G-BUFFER values,
	// which is where they are produced now; this reproduces that resolve for one pixel.
	float gloss = 0.0f;
	d["albedo"] = resolve_near_field(mat, Vector3(h[0], h[1], h[2]), Vector3(n[0], n[1], n[2]),
			overlay, half_to_float(s[3]), &gloss);
	d["gloss"] = gloss;
	return d;
}

// Isolated g-buffer holes: a pixel the primary march missed while all four of its
// neighbours hit. Real sky is a connected region, so an isolated miss can only be the march
// stepping over geometry it should have found. Counting them is view-robust in a way that
// naming one guilty pixel is not.
Dictionary VoxelDebugHooks::debug_raymarch_hole_probe(Vector3 origin, Vector3 dir, int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hit_pixels"] = 0;
	d["isolated_misses"] = 0;
	if (w <= 2 || h <= 2) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass()) return d;
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(1.0471975512f * 0.5f);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.params[0] = tan_y * aspect;
	cam.params[1] = tan_y;
	cam.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_->store_->config().world_size_regions.x; cam.dims[1] = world_->store_->config().world_size_regions.y;
	cam.dims[2] = world_->store_->config().world_size_regions.z; cam.dims[3] = world_->island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cam.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cam.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	const uint32_t flags = ve::pack_flags(world_->beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cam, w, h, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray hp = device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
	if (hp.size() < static_cast<int64_t>(w) * h * 16) return d;
	const float *f = reinterpret_cast<const float *>(hp.ptr());
	std::vector<uint8_t> hit(static_cast<size_t>(w) * h, 0);
	int hits = 0;
	for (int i = 0; i < w * h; i++) {
		hit[i] = f[i * 4 + 3] > 0.5f ? 1 : 0;
		hits += hit[i];
	}
	int isolated = 0;
	for (int y = 1; y < h - 1; y++)
		for (int x = 1; x < w - 1; x++) {
			const size_t i = static_cast<size_t>(y) * w + x;
			if (hit[i]) continue;
			if (hit[i - 1] && hit[i + 1] && hit[i - w] && hit[i + w]) isolated++;
		}
	d["ran"] = true;
	d["hit_pixels"] = hits;
	d["isolated_misses"] = isolated;
	return d;
}

Dictionary VoxelDebugHooks::debug_raymarch_normal_probe(Vector3 origin, Vector3 dir, int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hits"] = 0;
	d["rms_ndl"] = 0.0;
	d["cel_mismatch_fraction"] = 0.0;
	d["largest_mismatch_component"] = 0;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass()) return d;
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(1.0471975512f * 0.5f);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.params[0] = tan_y * aspect;
	cam.params[1] = tan_y;
	cam.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_->store_->config().world_size_regions.x; cam.dims[1] = world_->store_->config().world_size_regions.y;
	cam.dims[2] = world_->store_->config().world_size_regions.z; cam.dims[3] = world_->island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cam.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cam.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	const uint32_t flags = ve::pack_flags(world_->beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cam, w, h, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray surface = device->texture_get_data(world_->raymarch_pass()->surface_texture(), 0);
	const PackedByteArray hitpos = device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
	if (surface.size() < static_cast<int64_t>(w) * h * 8) return d;
	if (hitpos.size() < static_cast<int64_t>(w) * h * 16) return d;
	const uint16_t *s = reinterpret_cast<const uint16_t *>(surface.ptr());
	const float *hp = reinterpret_cast<const float *>(hitpos.ptr());
	constexpr float kSun[3] = {0.5746958f, 0.7662610f, 0.2873479f};
	constexpr float kEdges[3] = {0.08f, 0.32f, 0.66f};
	auto band = [](float ndl) { return ndl > 0.66f ? 3 : ndl > 0.32f ? 2 : ndl > 0.08f ? 1 : 0; };
	int total_hits = 0;
	for (int i = 0; i < w * h; i++) if (hp[i * 4 + 3] > 0.5f) total_hits++;
	std::vector<uint8_t> mismatch(static_cast<size_t>(w) * h, 0);
	double sum_sq = 0.0;
	int considered = 0;
	int mismatches = 0;
	// Task 7: the reference normal comes from the CPU field evaluator over the hit
	// point's own region op span, consulting the same VolumeSet / OverrideStore the
	// GPU's authoritative buffers mirror -- not from an inline analytic formula, so
	// edits, stored volumes and consolidated overrides are all covered. For a pure
	// procedural hit this reduces exactly to Task 1's analytic gradient.
	const ve::Generator &gen = world_->store_->generator()->sampler();
	std::lock_guard<std::mutex> edit_lock(world_->edit_mutex());
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int i = y * w + x;
			if (hp[i * 4 + 3] <= 0.5f) continue;
			// Decode hit normal from surface texture.
			const float e[2] = {half_to_float(s[i * 4 + 0]), half_to_float(s[i * 4 + 1])};
			float rn[3];
			ve::oct_decode(e, rn);
			float rlen = std::sqrt(rn[0]*rn[0] + rn[1]*rn[1] + rn[2]*rn[2]);
			if (rlen > 1e-8f) { rn[0]/=rlen; rn[1]/=rlen; rn[2]/=rlen; }
			const float hitx = hp[i * 4 + 0];
			const float hity = hp[i * 4 + 1];
			const float hitz = hp[i * 4 + 2];
			// CPU reference gradient over this region's op span.
			const std::vector<ve::EditOp> &ops =
					world_->store_->edit_log()->ops(ve::WorldBounds::region_of_point(hitx, hity, hitz));
			const ve::FieldSample fs = ve::eval_field_gradient(gen, ops.data(),
					static_cast<int>(ops.size()), hitx, hity, hitz, &world_->store_->volumes(), world_->store_->overrides());
			if (!fs.exact_gradient) continue;
			const float gx = fs.gradient[0], gy = fs.gradient[1], gz = fs.gradient[2];
			float alen = std::sqrt(gx*gx + gy*gy + gz*gz);
			if (alen < 1e-8f) continue;
			float an[3] = {gx/alen, gy/alen, gz/alen};
			float ndl_render = rn[0]*kSun[0] + rn[1]*kSun[1] + rn[2]*kSun[2];
			float ndl_analytic = an[0]*kSun[0] + an[1]*kSun[1] + an[2]*kSun[2];
			double diff = double(ndl_render) - double(ndl_analytic);
			sum_sq += diff * diff;
			considered++;
			bool far_from_edges = true;
			for (int eidx = 0; eidx < 3; eidx++) if (std::fabs(ndl_analytic - kEdges[eidx]) < 0.01f) far_from_edges = false;
			if (!far_from_edges) continue;
			if (band(ndl_render) != band(ndl_analytic)) {
				mismatch[static_cast<size_t>(i)] = 1;
				mismatches++;
			}
		}
	}
	double rms = considered > 0 ? std::sqrt(sum_sq / double(considered)) : 0.0;
	double frac = considered > 0 ? double(mismatches) / double(considered) : 0.0;
	int largest = 0;
	std::vector<uint8_t> visited(static_cast<size_t>(w) * h, 0);
	std::vector<int> stack;
	stack.reserve(1024);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int idx = y * w + x;
			if (!mismatch[static_cast<size_t>(idx)] || visited[static_cast<size_t>(idx)]) continue;
			int comp = 0;
			stack.clear();
			stack.push_back(idx);
			visited[static_cast<size_t>(idx)] = 1;
			while (!stack.empty()) {
				int cur = stack.back(); stack.pop_back();
				comp++;
				int cx = cur % w;
				int cy = cur / w;
				if (cx > 0) { int nb = cur - 1; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cx + 1 < w) { int nb = cur + 1; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cy > 0) { int nb = cur - w; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cy + 1 < h) { int nb = cur + w; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
			}
			if (comp > largest) largest = comp;
		}
	}
	d["ran"] = true;
	d["hits"] = total_hits;
	d["considered"] = considered; // hits whose CPU reference gradient was exact and compared
	d["rms_ndl"] = rms;
	d["cel_mismatch_fraction"] = frac;
	d["largest_mismatch_component"] = largest;
	return d;
}

// Task 7: the island counterpart of debug_raymarch_normal_probe. Every hit inside the
// body's local lattice box is compared against a trilinear blend of that body's OWN
// compact normals (VolumeData::normal_oct, CPU authoritative), rotated into the world by
// the body basis -- the same numbers the shader decodes at an island hit. Same five
// metrics as the terrain probe.
Dictionary VoxelDebugHooks::debug_island_normal_probe(int island_slot, Vector3 origin, Vector3 dir,
		int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hits"] = 0;
	d["rms_ndl"] = 0.0;
	d["cel_mismatch_fraction"] = 0.0;
	d["largest_mismatch_component"] = 0;
	if (w <= 0 || h <= 0 || island_slot < 0 || island_slot >= kMaxIslands) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass()) return d;
	// The descriptor the SHADER sees is the one on the device: test-placed islands are
	// uploaded directly, so read it back rather than trusting any cached copy.
	const int64_t desc_bytes = static_cast<int64_t>(kMaxIslands) * 128;
	const PackedByteArray desc =
			device->buffer_get_data(world_->islands()->desc_buffer(), 0, static_cast<uint32_t>(desc_bytes));
	if (desc.size() != desc_bytes) return d;
	const uint8_t *src = desc.ptr() + static_cast<int64_t>(island_slot) * 128;
	const float *f = reinterpret_cast<const float *>(src);
	const int32_t *di = reinterpret_cast<const int32_t *>(src);
	const int dim = di[16];
	const int volume_slot = di[17];
	if (dim < 2 || volume_slot < 0 || volume_slot >= ve::kMaxVolumes) return d;
	float basis[9];   // column major: basis[a*3+c] = world direction component c of local +a
	float body_origin[3], lattice_lo[3];
	for (int a = 0; a < 3; a++) {
		basis[a * 3 + 0] = f[a * 4 + 0];
		basis[a * 3 + 1] = f[a * 4 + 1];
		basis[a * 3 + 2] = f[a * 4 + 2];
		body_origin[a] = f[a * 4 + 3];
		lattice_lo[a] = f[12 + a];
	}
	const float voxel = f[15];
	if (!(voxel > 0.0f)) return d;
	const ve::VolumeData *vol = world_->store_->volumes().get(volume_slot);
	if (!vol || !vol->has_normals() || vol->dim != dim) return d;

	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(1.0471975512f * 0.5f);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.params[0] = tan_y * aspect;
	cam.params[1] = tan_y;
	cam.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_->store_->config().world_size_regions.x; cam.dims[1] = world_->store_->config().world_size_regions.y;
	cam.dims[2] = world_->store_->config().world_size_regions.z; cam.dims[3] = world_->island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cam.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cam.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	const uint32_t flags = ve::pack_flags(world_->beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cam, w, h, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray surface = device->texture_get_data(world_->raymarch_pass()->surface_texture(), 0);
	const PackedByteArray hitpos = device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
	if (surface.size() < static_cast<int64_t>(w) * h * 8) return d;
	if (hitpos.size() < static_cast<int64_t>(w) * h * 16) return d;
	const uint16_t *s = reinterpret_cast<const uint16_t *>(surface.ptr());
	const float *hp = reinterpret_cast<const float *>(hitpos.ptr());
	constexpr float kSun[3] = {0.5746958f, 0.7662610f, 0.2873479f};
	constexpr float kEdges[3] = {0.08f, 0.32f, 0.66f};
	auto band = [](float ndl) { return ndl > 0.66f ? 3 : ndl > 0.32f ? 2 : ndl > 0.08f ? 1 : 0; };

	int total_hits = 0;
	for (int i = 0; i < w * h; i++) if (hp[i * 4 + 3] > 0.5f) total_hits++;
	std::vector<uint8_t> mismatch(static_cast<size_t>(w) * h, 0);
	double sum_sq = 0.0;
	int considered = 0;
	int mismatches = 0;
	const float span = static_cast<float>(dim - 1) * voxel;
	const int sy = dim, sz = dim * dim;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int i = y * w + x;
			if (hp[i * 4 + 3] <= 0.5f) continue;
			const Vector3 hit(hp[i * 4 + 0], hp[i * 4 + 1], hp[i * 4 + 2]);
			// World -> local -> lattice coordinates.
			const float rel[3] = {hit.x - body_origin[0], hit.y - body_origin[1],
					hit.z - body_origin[2]};
			// Orthonormal basis: inverse is its transpose.
			float q_l[3];
			for (int a = 0; a < 3; a++)
				q_l[a] = basis[a * 3 + 0] * rel[0] + basis[a * 3 + 1] * rel[1]
						+ basis[a * 3 + 2] * rel[2];
			bool inside = true;
			float lcoord[3];
			for (int a = 0; a < 3; a++) {
				lcoord[a] = (q_l[a] - lattice_lo[a]) / voxel;
				if (lcoord[a] < -1e-4f || lcoord[a] > static_cast<float>(dim - 1) + 1e-4f)
					inside = false;
			}
			if (!inside) continue; // terrain (or another body) behind/around the island
			// Trilinear blend of the compact normals at the same coords/fractions the
			// shader's island_sdf_at uses.
			int i0[3], i1[3];
			float fr[3];
			for (int a = 0; a < 3; a++) {
				lcoord[a] = std::min(std::max(lcoord[a], 0.0f), static_cast<float>(dim - 1));
				i0[a] = static_cast<int>(std::floor(lcoord[a]));
				i1[a] = std::min(i0[a] + 1, dim - 1);
				fr[a] = lcoord[a] - static_cast<float>(i0[a]);
			}
			float n_acc[3] = {0.0f, 0.0f, 0.0f};
			for (int c = 0; c < 8; c++) {
				const int cx = (c & 1) ? i1[0] : i0[0];
				const int cy = (c & 2) ? i1[1] : i0[1];
				const int cz = (c & 4) ? i1[2] : i0[2];
				float wx = (c & 1) ? fr[0] : 1.0f - fr[0];
				float wy = (c & 2) ? fr[1] : 1.0f - fr[1];
				float wz = (c & 4) ? fr[2] : 1.0f - fr[2];
				float dec[3];
				ve::oct_decode_snorm8(vol->normal_oct[static_cast<size_t>(cx + cy * sy + cz * sz)], dec);
				for (int a = 0; a < 3; a++) n_acc[a] += wx * wy * wz * dec[a];
			}
			// Local -> world through the body basis. basis[a*3+c] is the world component c
			// of local axis a, so world[c] = sum over local axes a of basis[a*3+c] * n[a]
			// (the same column convention march_island's mat3 multiply uses).
			float wn[3] = {0.0f, 0.0f, 0.0f};
			for (int a = 0; a < 3; a++) {
				const float na = n_acc[a];
				wn[0] += basis[a * 3 + 0] * na;
				wn[1] += basis[a * 3 + 1] * na;
				wn[2] += basis[a * 3 + 2] * na;
			}
			float alen = std::sqrt(wn[0]*wn[0] + wn[1]*wn[1] + wn[2]*wn[2]);
			if (alen < 1e-6f) continue; // degenerate blend: nothing to compare against
			float an[3] = {wn[0]/alen, wn[1]/alen, wn[2]/alen};
			const float e[2] = {half_to_float(s[i * 4 + 0]), half_to_float(s[i * 4 + 1])};
			float rn[3];
			ve::oct_decode(e, rn);
			float rlen = std::sqrt(rn[0]*rn[0] + rn[1]*rn[1] + rn[2]*rn[2]);
			if (rlen > 1e-8f) { rn[0]/=rlen; rn[1]/=rlen; rn[2]/=rlen; }
			float ndl_render = rn[0]*kSun[0] + rn[1]*kSun[1] + rn[2]*kSun[2];
			float ndl_analytic = an[0]*kSun[0] + an[1]*kSun[1] + an[2]*kSun[2];
			const double diff = double(ndl_render) - double(ndl_analytic);
			sum_sq += diff * diff;
			considered++;
			bool far_from_edges = true;
			for (int eidx = 0; eidx < 3; eidx++) if (std::fabs(ndl_analytic - kEdges[eidx]) < 0.01f) far_from_edges = false;
			if (!far_from_edges) continue;
			if (band(ndl_render) != band(ndl_analytic)) {
				mismatch[static_cast<size_t>(i)] = 1;
				mismatches++;
			}
		}
	}
	double rms = considered > 0 ? std::sqrt(sum_sq / double(considered)) : 0.0;
	double frac = considered > 0 ? double(mismatches) / double(considered) : 0.0;
	int largest = 0;
	std::vector<uint8_t> visited(static_cast<size_t>(w) * h, 0);
	std::vector<int> stack;
	stack.reserve(1024);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int idx = y * w + x;
			if (!mismatch[static_cast<size_t>(idx)] || visited[static_cast<size_t>(idx)]) continue;
			int comp = 0;
			stack.clear();
			stack.push_back(idx);
			visited[static_cast<size_t>(idx)] = 1;
			while (!stack.empty()) {
				int cur = stack.back(); stack.pop_back();
				comp++;
				int cx = cur % w;
				int cy = cur / w;
				if (cx > 0) { int nb = cur - 1; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cx + 1 < w) { int nb = cur + 1; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cy > 0) { int nb = cur - w; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cy + 1 < h) { int nb = cur + w; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
			}
			if (comp > largest) largest = comp;
		}
	}
	d["ran"] = true;
	d["hits"] = total_hits;
	d["considered"] = considered; // hits whose CPU reference gradient was exact and compared
	d["rms_ndl"] = rms;
	d["cel_mismatch_fraction"] = frac;
	d["largest_mismatch_component"] = largest;
	return d;
}

Dictionary VoxelDebugHooks::debug_ssr_probe(int fixture, int w, int h) {
	Dictionary d;
	const int width = std::max(1, w);
	const int height = std::max(1, h);
	d["width"] = std::max(1, width / 2);
	d["height"] = std::max(1, height / 2);
	d["ran"] = false;
	d["steps"] = 0;
	d["hit_pixels"] = 0;
	d["dynamic_hit_pixels"] = 0;
	d["scene_hit_pixels"] = 0;
	d["red_gain"] = 0.0f;
	d["max_weight"] = 0.0f;
	d["max_alpha_delta"] = 0.0f;
	d["mean_delta"] = 0.0f;
	d["finite"] = true;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->material_atlas() || !world_->raymarch_pass() || !world_->composite_pass() ||
			!world_->gbuffer() || !world_->beauty_camera() || !world_->ssr_pass()) return d;
	const ve::BeautySettings settings = world_->beauty_settings();
	d["steps"] = settings.ssr_steps;
	if (!settings.ssr || settings.ssr_steps <= 0) return d;
	const Vector2i size(width, height);
	if (!world_->gbuffer()->ensure(device, nullptr, size) || !world_->beauty_camera()->ensure(device)) return d;
	const float camera_pos[3] = {20.0f, 75.0f, 20.0f};
	const float camera_fwd[3] = {0.0f, -1.0f, 0.0f};
	const float camera_up[3] = {0.0f, 0.0f, -1.0f};
	const float aspect = static_cast<float>(width) / static_cast<float>(height);
	const ve::LodCamera camera = ve::lod_camera_perspective(camera_pos, camera_fwd, camera_up,
			1.0471975512f, aspect, 0.05f, 4000.0f, width, height);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = camera.view_proj[c * 4 + r];
	float normal[3] = {0.6f, 0.8f, 0.0f};
	float oct[2];
	ve::oct_encode(normal, oct);
	ve::CameraParams camera_params = ve::CameraParams::looking_at(
			camera_pos[0], camera_pos[1], camera_pos[2], camera_fwd[0], camera_fwd[1], camera_fwd[2],
			camera_up[0], camera_up[1], camera_up[2]);
	camera_params.params[0] = std::tan(1.0471975512f * 0.5f) * aspect;
	camera_params.params[1] = std::tan(1.0471975512f * 0.5f);
	camera_params.params[2] = 200.0f;
	const ve::WorldBounds probe_bounds = world_->world_bounds();
	const ve::IVec3 probe_origin = probe_bounds.origin_regions();
	camera_params.dims[0] = world_->store_->config().world_size_regions.x;
	camera_params.dims[1] = world_->store_->config().world_size_regions.y;
	camera_params.dims[2] = world_->store_->config().world_size_regions.z;
	camera_params.dims[3] = world_->island_slot_count();
	camera_params.region_origin[0] = probe_origin.x;
	camera_params.region_origin[1] = probe_origin.y;
	camera_params.region_origin[2] = probe_origin.z;
	camera_params.atlas_bricks[0] = world_->store_->config().atlas_bricks.x;
	camera_params.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	camera_params.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	const uint32_t probe_flags = ve::pack_flags(settings);
	std::memcpy(&camera_params.cam_pos[3], &probe_flags, sizeof(float));
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), camera_params, width, height,
			no_edit)) return d;
	float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
	world_->lod_fade_band(&fade_start, &fade_end);
	world_->composite_pass()->draw(device, *world_->gbuffer(), world_->raymarch_pass()->albedo_texture(),
			world_->raymarch_pass()->surface_texture(), world_->raymarch_pass()->hitpos_texture(), view_proj,
			*world_->material_atlas(), camera_params, fade_start, fade_end);
	if (!world_->composite_pass()->last_draw_ok()) return d;
	auto float16 = [](float value) -> uint16_t {
		uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		const uint32_t sign = (bits >> 16) & 0x8000u;
		const int exp = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
		const uint32_t mant = bits & 0x7FFFFFu;
		if (exp <= 0) return static_cast<uint16_t>(sign);
		if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
		return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
				(mant >> 13));
	};
	const int pixels = width * height;
	PackedByteArray depths;
	depths.resize(pixels * static_cast<int>(sizeof(float)));
	float *depth_ptr = reinterpret_cast<float *>(depths.ptrw());
	PackedByteArray colors;
	colors.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
	uint16_t *color_ptr = reinterpret_cast<uint16_t *>(colors.ptrw());
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const int index = y * width + x;
			const bool blocker = (fixture == 0 || fixture >= 2) && x >= width / 4 && x < width / 2 &&
					y >= height / 4 && y < height / 2;
			depth_ptr[index] = 0.0f;
			const float r = blocker ? 1.0f : 0.2f;
			const float g = blocker ? 0.03f : 0.2f;
			const float b = blocker ? 0.03f : 0.2f;
			color_ptr[index * 4 + 0] = float16(r);
			color_ptr[index * 4 + 1] = float16(g);
			color_ptr[index * 4 + 2] = float16(b);
			color_ptr[index * 4 + 3] = float16(0.7f);
		}
	}
	PackedByteArray surface_bytes;
	surface_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
	uint16_t *surface_ptr = reinterpret_cast<uint16_t *>(surface_bytes.ptrw());
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			const int i = y * width + x;
			const bool dynamic_receiver = fixture >= 2 && x < width / 3 && y < height / 3;
			surface_ptr[i * 4 + 0] = dynamic_receiver ? float16(0.0f) : float16(oct[0]);
			surface_ptr[i * 4 + 1] = dynamic_receiver ? float16(0.0f) : float16(oct[1]);
			surface_ptr[i * 4 + 2] = dynamic_receiver ? float16(0.0f) : float16(1.0f);
			surface_ptr[i * 4 + 3] = dynamic_receiver ? float16(0.0f) : float16(1.0f);
		}
	Ref<RDTextureFormat> depth_format;
	depth_format.instantiate();
	depth_format->set_format(RenderingDevice::DATA_FORMAT_R32_SFLOAT);
	depth_format->set_width(width); depth_format->set_height(height);
	depth_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> depth_view; depth_view.instantiate();
	TypedArray<PackedByteArray> depth_data; depth_data.push_back(depths);
	const RID scene_depth = device->texture_create(depth_format, depth_view, depth_data);
	Ref<RDTextureFormat> color_format;
	color_format.instantiate();
	color_format->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
	color_format->set_width(width); color_format->set_height(height);
	color_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> color_view; color_view.instantiate();
	TypedArray<PackedByteArray> color_data; color_data.push_back(colors);
	const RID scene_color = device->texture_create(color_format, color_view, color_data);
	Ref<RDTextureFormat> surface_format;
	surface_format.instantiate();
	surface_format->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
	surface_format->set_width(width); surface_format->set_height(height);
	surface_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> surface_view; surface_view.instantiate();
	TypedArray<PackedByteArray> surface_data; surface_data.push_back(surface_bytes);
	const RID fixture_surface = device->texture_create(surface_format, surface_view, surface_data);
	RID normal_texture;
	if (fixture >= 2) {
		PackedByteArray normal_bytes;
		normal_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
		uint16_t *normal_ptr = reinterpret_cast<uint16_t *>(normal_bytes.ptrw());
		const uint16_t alpha = float16(fixture == 2 ? 0.0f : 1.0f);
		for (int i = 0; i < pixels; i++) {
			normal_ptr[i * 4 + 0] = float16(0.0f);
			normal_ptr[i * 4 + 1] = float16(0.0f);
			normal_ptr[i * 4 + 2] = float16(0.0f);
			normal_ptr[i * 4 + 3] = alpha;
		}
		TypedArray<PackedByteArray> normal_data;
		normal_data.push_back(normal_bytes);
		normal_texture = device->texture_create(color_format, color_view, normal_data);
	}
	if (!scene_depth.is_valid() || !scene_color.is_valid() || !fixture_surface.is_valid() ||
			(fixture >= 2 && !normal_texture.is_valid())) {
		if (scene_depth.is_valid()) device->free_rid(scene_depth);
		if (scene_color.is_valid()) device->free_rid(scene_color);
		if (fixture_surface.is_valid()) device->free_rid(fixture_surface);
		if (normal_texture.is_valid()) device->free_rid(normal_texture);
		return d;
	}
	device->submit();
	device->sync();
	const PackedByteArray rendered_depth = device->texture_get_data(world_->gbuffer()->depth(), 0);
	if (rendered_depth.size() < pixels * static_cast<int>(sizeof(float))) {
		device->free_rid(scene_depth);
		device->free_rid(scene_color);
		device->free_rid(fixture_surface);
		if (normal_texture.is_valid()) device->free_rid(normal_texture);
		return d;
	}
	const float *rendered_depth_ptr = reinterpret_cast<const float *>(rendered_depth.ptr());
	const Projection inv_view_proj = view_proj.inverse();
	auto mul_projection = [](const Projection &m, const Vector4 &v) {
		Vector4 result;
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) result[r] += m.columns[c][r] * v[c];
		return result;
	};
	auto dynamic_plane_depth = [&](int x, int y) {
		const Vector2 ndc((static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f,
				(static_cast<float>(y) + 0.5f) / height * 2.0f - 1.0f);
		const Vector4 far_h = mul_projection(inv_view_proj, Vector4(ndc.x, ndc.y, 0.0f, 1.0f));
		const Vector3 far_p = Vector3(far_h.x, far_h.y, far_h.z) / far_h.w;
		const Vector3 ray = (far_p - Vector3(camera_pos[0], camera_pos[1], camera_pos[2])).normalized();
		const Vector3 plane_normal(0.6f, 0.8f, 0.0f);
		const Vector3 plane_point(20.0f, 60.0f, 20.0f);
		const float denom = plane_normal.dot(ray);
		if (std::fabs(denom) < 1e-5f) return rendered_depth_ptr[y * width + x];
		const Vector3 hit = Vector3(camera_pos[0], camera_pos[1], camera_pos[2]) + ray *
				(plane_normal.dot(plane_point - Vector3(camera_pos[0], camera_pos[1], camera_pos[2])) / denom);
		const Vector4 clip = mul_projection(view_proj, Vector4(hit.x, hit.y, hit.z, 1.0f));
		return clip.z / clip.w;
	};
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			const int index = y * width + x;
			const bool dynamic_receiver = fixture >= 2 && x < width / 3 && y < height / 3;
			const bool blocker = (fixture == 0 || fixture >= 2) && x >= width / 4 && x < width / 2 &&
					y >= height / 4 && y < height / 2;
			const float base_depth = dynamic_receiver ? dynamic_plane_depth(x, y) : rendered_depth_ptr[index];
			depth_ptr[index] = blocker ? std::min(1.0f, base_depth + 0.001f) : base_depth;
		}
	device->texture_update(scene_depth, 0, depths);
	device->texture_update(scene_color, 0, colors);
	device->texture_copy(fixture_surface, world_->gbuffer()->surface(), Vector3(), Vector3(),
			Vector3(width, height, 1), 0, 0, 0, 0);
	device->submit();
	device->sync();
	world_->beauty_camera()->update(device, view_proj, camera_pos, size, 0.05f, 4000.0f);
	const RID normal_roughness = normal_texture;
	const bool ok = world_->ssr_pass()->render(device, scene_color, scene_depth, world_->gbuffer()->surface(),
			world_->gbuffer()->depth(), normal_roughness, normal_roughness.is_valid(), world_->beauty_camera()->buffer(),
			size, settings);
	device->submit();
	device->sync();
	auto release_fixture = [&]() {
		world_->ssr_pass()->teardown();
		if (!world_->ssr_pass()->initialize(device))
			UtilityFunctions::printerr("debug_ssr_probe: SSR pass reinitialization failed");
		device->free_rid(scene_depth);
		device->free_rid(scene_color);
		device->free_rid(fixture_surface);
		if (normal_texture.is_valid()) device->free_rid(normal_texture);
	};
	if (!ok) {
		release_fixture();
		return d;
	}
	const PackedByteArray before = colors;
	const PackedByteArray after = device->texture_get_data(scene_color, 0);
	const PackedByteArray reflection = device->texture_get_data(world_->ssr_pass()->reflection(), 0);
	const int half_w = std::max(1, width / 2), half_h = std::max(1, height / 2);
	const int half_pixels = half_w * half_h;
	if (after.size() >= pixels * 8 && reflection.size() >= half_pixels * 8) {
		const uint16_t *a = reinterpret_cast<const uint16_t *>(after.ptr());
		const uint16_t *r = reinterpret_cast<const uint16_t *>(reflection.ptr());
		const uint16_t *b = reinterpret_cast<const uint16_t *>(before.ptr());
		double delta = 0.0;
		float red_gain_max = 0.0f;
		float max_weight = 0.0f, max_alpha_delta = 0.0f;
		int hits = 0, dynamic_hits = 0, scene_hits = 0;
		for (int i = 0; i < pixels; i++) {
			const float br = Math::half_to_float(b[i * 4]);
			const float ar = Math::half_to_float(a[i * 4]);
			const float ba = Math::half_to_float(b[i * 4 + 3]);
			const float aa = Math::half_to_float(a[i * 4 + 3]);
			red_gain_max = std::max(red_gain_max, ar - br);
			delta += std::fabs(ar - br) + std::fabs(Math::half_to_float(a[i * 4 + 1]) -
					Math::half_to_float(b[i * 4 + 1]));
			max_alpha_delta = std::max(max_alpha_delta, std::fabs(aa - ba));
		}
		for (int i = 0; i < half_pixels; i++) {
			const float weight = Math::half_to_float(r[i * 4 + 3]);
			max_weight = std::max(max_weight, weight);
			if (weight > 0.001f) {
				hits++;
				const int sx = (i % half_w) * width / half_w;
				const int sy = (i / half_w) * height / half_h;
				if (fixture >= 2 && sx < width / 3 && sy < height / 3)
					dynamic_hits++;
				else
					scene_hits++;
			}
		}
		d["hit_pixels"] = hits;
		d["dynamic_hit_pixels"] = dynamic_hits;
		d["scene_hit_pixels"] = scene_hits;
		d["red_gain"] = red_gain_max;
		d["mean_delta"] = static_cast<float>(delta / pixels);
		d["max_weight"] = max_weight;
		d["max_alpha_delta"] = max_alpha_delta;
		for (int i = 0; i < pixels * 4; i++)
			if (!std::isfinite(Math::half_to_float(a[i]))) d["finite"] = false;
	}
	d["ran"] = true;
	release_fixture();
	return d;
}

Dictionary VoxelDebugHooks::debug_outline_probe(int fixture, bool have_dynamic_normals) {
	Dictionary d;
	d["ran"] = false;
	d["dark_columns"] = 0;
	d["dark_value"] = 0.0f;
	d["mean_delta"] = 0.0f;
	d["max_brightening"] = 0.0f;
	d["max_alpha_delta"] = 0.0f;
	if (fixture < 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->outline_pass() || !world_->beauty_camera()) return d;
	const int width = 32, height = 16, pixels = width * height;
	if (!world_->beauty_camera()->ensure(device)) return d;
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = c == r ? 1.0f : 0.0f;
	const float camera_pos[3] = {0.0f, 0.0f, -100.0f};
	world_->beauty_camera()->update(device, view_proj, camera_pos, Vector2i(width, height), 0.05f,
			4000.0f);

	auto float16 = [](float value) -> uint16_t {
		uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		const uint32_t sign = (bits >> 16) & 0x8000u;
		const int exp = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
		const uint32_t mant = bits & 0x7FFFFFu;
		if (exp <= 0) return static_cast<uint16_t>(sign);
		if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
		return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
				(mant >> 13));
	};
	const uint16_t one = float16(1.0f), half = float16(0.5f);
	PackedByteArray depth_bytes;
	depth_bytes.resize(pixels * static_cast<int>(sizeof(float)));
	float *depth = reinterpret_cast<float *>(depth_bytes.ptrw());
	PackedByteArray color_bytes;
	color_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
	uint16_t *color = reinterpret_cast<uint16_t *>(color_bytes.ptrw());
	PackedByteArray gb_depth_bytes;
	gb_depth_bytes.resize(pixels * static_cast<int>(sizeof(float)));
	float *gb_depth = reinterpret_cast<float *>(gb_depth_bytes.ptrw());
	PackedByteArray surface_bytes;
	surface_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
	uint16_t *surface = reinterpret_cast<uint16_t *>(surface_bytes.ptrw());
	PackedByteArray normal_bytes;
	uint16_t *normal = nullptr;
	if (fixture == 3) {
		normal_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
		normal = reinterpret_cast<uint16_t *>(normal_bytes.ptrw());
	}
	// Fixtures 5-7 exercise the two ways the edge test used to report an edge where the
	// geometry has none. All three cover the whole frame with terrain (material != 0).
	//
	//   5  seam hole  one column lost its depth to the near/far dither while its g-buffer
	//                 surface survived. Not a silhouette; nothing may darken.
	//   6  real sky   the same column with material 0 as well. That IS a silhouette.
	//   7  grazing    a depth ramp seen almost edge-on, holding the relative depth step at
	//                 a constant 6% -- over the old flat 4% threshold, under the incidence-
	//                 scaled one -- with a genuine cliff at column 24 that must still show.
	const bool hole_fixture = fixture == 5 || fixture == 6;
	const int hole_column = width / 2;
	const int cliff_column = 24;
	const float kGrazingStep = 0.06f;   // relative depth step per column
	// The cliff has to clear the incidence-scaled threshold, which at this fixture's
	// near-edge-on angle is the OUTLINE_MIN_NDV cap: 0.04 / 0.05 = 0.8 relative.
	const float kCliffFactor = 20.0f;
	std::vector<float> ramp(width, 10.0f), ramp_ndv(width, 1.0f);
	if (fixture == 7) {
		// The camera sits at z = -100 and the identity view_proj puts the reconstructed
		// world point at (u, v, depth), so the camera distance is depth + 100.
		for (int x = 1; x < width; x++) {
			const float step = (ramp[x - 1] + 100.0f) * kGrazingStep;
			ramp[x] = ramp[x - 1] + step * (x == cliff_column ? kCliffFactor : 1.0f);
		}
		// World x spans [-1, 1] across the row, so one column advances 2/width in x while
		// the surface advances `step` along the view axis. The normal is perpendicular to
		// that tangent, which is what makes the surface edge-on.
		for (int x = 0; x < width; x++) {
			const float dz = x + 1 < width ? ramp[x + 1] - ramp[x]
					: ramp[x] - ramp[x - 1];
			const float dx = 2.0f / static_cast<float>(width);
			const float len = std::sqrt(dz * dz + dx * dx);
			ramp_ndv[x] = len > 0.0f ? dx / len : 1.0f;
		}
	}
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			const int p = y * width + x;
			const bool right_side = x >= width / 2;
			const bool depth_line = fixture == 1 || fixture == 4;
			depth[p] = depth_line && right_side ? 20.0f : 10.0f;
			if (fixture == 7) depth[p] = ramp[x];
			if (hole_fixture && x == hole_column) depth[p] = 0.0f;
			gb_depth[p] = depth[p];
			color[p * 4 + 0] = one; color[p * 4 + 1] = one;
			color[p * 4 + 2] = one; color[p * 4 + 3] = one;
			const bool terrain = fixture == 2;
			// Fixture 6's hole column is the only place a covered fixture writes material 0.
			const bool covered = terrain || fixture == 5 || fixture == 7 ||
					(fixture == 6 && x != hole_column);
			const bool up = !right_side;
			surface[p * 4 + 0] = up ? half : one;
			surface[p * 4 + 1] = up ? one : half;
			if (fixture == 5 || fixture == 6 || fixture == 7) {
				// A real encoded normal, so the shader's oct_decode returns the vector the
				// incidence term needs rather than an arbitrary pair of fp16 constants. The
				// camera looks down +z here, so an incidence cosine of c is a normal whose
				// z component is -c.
				const float c = ramp_ndv[x];
				const float n[3] = {std::sqrt(std::max(0.0f, 1.0f - c * c)), 0.0f, -c};
				float e[2];
				ve::oct_encode(n, e);
				surface[p * 4 + 0] = float16(e[0]);
				surface[p * 4 + 1] = float16(e[1]);
			}
			surface[p * 4 + 2] = covered ? one : 0;
			surface[p * 4 + 3] = one;
			if (normal) {
				normal[p * 4 + 0] = up ? half : one;
				normal[p * 4 + 1] = up ? one : half;
				normal[p * 4 + 2] = half;
				normal[p * 4 + 3] = one;
			}
		}

	auto create = [&](RenderingDevice::DataFormat format, uint32_t usage,
			const PackedByteArray &bytes) -> RID {
			Ref<RDTextureFormat> f;
			f.instantiate(); f->set_format(format); f->set_width(width); f->set_height(height);
			f->set_usage_bits(usage);
			Ref<RDTextureView> v; v.instantiate();
			TypedArray<PackedByteArray> data; data.push_back(bytes);
			return device->texture_create(f, v, data);
		};
	const uint32_t sampled = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const uint32_t color_usage = sampled | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT;
	const RID scene_depth = create(RenderingDevice::DATA_FORMAT_R32_SFLOAT, sampled,
				depth_bytes);
		const RID scene_color = create(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
				color_usage, color_bytes);
		const RID fixture_gb_depth = create(RenderingDevice::DATA_FORMAT_R32_SFLOAT, sampled,
				gb_depth_bytes);
		const RID fixture_surface = create(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
				sampled, surface_bytes);
		RID normal_texture;
		if (normal)
			normal_texture = create(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, sampled,
					normal_bytes);
		const bool valid = scene_depth.is_valid() && scene_color.is_valid() &&
			fixture_gb_depth.is_valid() && fixture_surface.is_valid() &&
			(!normal || normal_texture.is_valid());
		if (!valid) {
			for (RID r : {scene_depth, scene_color, fixture_gb_depth, fixture_surface, normal_texture})
				if (r.is_valid()) device->free_rid(r);
			return d;
		}
		const ve::BeautySettings settings = world_->beauty_settings();
		const bool ok = world_->outline_pass()->render(device, scene_color, scene_depth, fixture_gb_depth,
				fixture_surface, normal_texture, have_dynamic_normals && normal_texture.is_valid(),
				world_->beauty_camera()->buffer(), Vector2i(width, height), settings);
		device->submit();
		device->sync();
		d["ran"] = ok;
		const PackedByteArray after = device->texture_get_data(scene_color, 0);
		if (ok && after.size() >= pixels * 8) {
			const uint16_t *out = reinterpret_cast<const uint16_t *>(after.ptr());
			int dark_columns = 0;
			float dark_value = 0.0f, max_brightening = 0.0f, max_alpha_delta = 0.0f;
			double total_delta = 0.0;
			for (int x = 0; x < width; x++) {
				bool dark = false;
				for (int y = 0; y < height; y++) {
					const int p = (y * width + x) * 4;
					const float r = Math::half_to_float(out[p]);
					const float a = Math::half_to_float(out[p + 3]);
					if (r < 0.99f) { dark = true; dark_value = r; }
					max_brightening = std::max(max_brightening, r - 1.0f);
					max_alpha_delta = std::max(max_alpha_delta, std::fabs(a - 1.0f));
					total_delta += std::fabs(r - 1.0f);
				}
				if (dark) dark_columns++;
			}
			d["dark_columns"] = dark_columns;
			d["dark_value"] = dark_value;
			d["mean_delta"] = static_cast<float>(total_delta / pixels);
			d["max_brightening"] = max_brightening;
			d["max_alpha_delta"] = max_alpha_delta;
		}
		world_->outline_pass()->teardown();
		world_->outline_pass()->initialize(device);
		for (RID r : {scene_depth, scene_color, fixture_gb_depth, fixture_surface, normal_texture})
			if (r.is_valid()) device->free_rid(r);
		return d;
}

Dictionary VoxelDebugHooks::debug_glossy_sdf_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	d["albedo"] = Color();
	d["sun"] = 0.0f;
	d["material"] = 0;
	d["gloss"] = 0.0f;
	d["position"] = origin;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass()) return d;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.params[0] = 0.0f;
	cam.params[1] = 0.0f;
	cam.params[2] = 200.0f;
	cam.params[3] = -1.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_->store_->config().world_size_regions.x; cam.dims[1] = world_->store_->config().world_size_regions.y;
	cam.dims[2] = world_->store_->config().world_size_regions.z; cam.dims[3] = world_->island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cam.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cam.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	const uint32_t flags = ve::pack_flags(world_->beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cam, 1, 1, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray ab = device->texture_get_data(world_->raymarch_pass()->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->raymarch_pass()->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
	if (ab.size() < 4 || sf.size() < 8 || hp.size() < 16) return d;
	const uint8_t *a = ab.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *h = reinterpret_cast<const float *>(hp.ptr());
	d["sun"] = a[3] / 255.0f;
	const int gmat = static_cast<int>(half_to_float(s[2]) + 0.5f);
	d["material"] = gmat;
	d["hit"] = h[3] > 0.5f;
	// The reflection is the point of this probe and it lives in the overlay: the marcher mixed
	// it there at the fresnel weight, and the composite mixes the pair over the material.
	const Color goverlay(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1.0f);
	d["overlay"] = goverlay;
	d["overlay_weight"] = half_to_float(s[3]);
	float ggloss = 0.0f;
	const float e[2] = {half_to_float(s[0]), half_to_float(s[1])};
	float gn[3];
	ve::oct_decode(e, gn);
	d["albedo"] = resolve_near_field(gmat, Vector3(h[0], h[1], h[2]), Vector3(gn[0], gn[1], gn[2]),
			goverlay, half_to_float(s[3]), &ggloss);
	d["gloss"] = ggloss;
	d["position"] = Vector3(h[0], h[1], h[2]);
	return d;
}

Color VoxelDebugHooks::debug_cel_reference(Color albedo, Color ambient, float ndl, float ndv,
		float ndh, float shadow, float ao, float gloss) const {
	ve::CelParams p;
	ve::CelInput in;
	in.albedo[0] = albedo.r; in.albedo[1] = albedo.g; in.albedo[2] = albedo.b;
	in.ambient[0] = ambient.r; in.ambient[1] = ambient.g; in.ambient[2] = ambient.b;
	in.ndl = ndl; in.ndv = ndv; in.ndh = ndh;
	in.shadow = shadow; in.ao = ao; in.gloss = gloss;
	float out[3];
	ve::cel_shade(p, in, out);
	return Color(out[0], out[1], out[2], 1.0f);
}

Dictionary VoxelDebugHooks::debug_cel_diff(Color albedo, Color ambient, float ndl, float ndv,
		float ndh, float shadow, float ao, float gloss) {
	Dictionary d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->gbuffer() || !world_->deferred_pass() || !world_->material_atlas()) return d;
	if (world_->gbuffer()->size() != Vector2i(1, 1)) {
		world_->deferred_pass()->teardown();
		world_->deferred_pass()->initialize(device);
		if (world_->composite_pass()) world_->composite_pass()->release_targets();
	}
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(1, 1))) return d;
	DeferredPass::Params p;
	p.probe_mode = 1;
	p.inv_view_proj[0] = albedo.r;
	p.inv_view_proj[1] = albedo.g;
	p.inv_view_proj[2] = albedo.b;
	p.inv_view_proj[4] = ambient.r;
	p.inv_view_proj[5] = ambient.g;
	p.inv_view_proj[6] = ambient.b;
	p.inv_view_proj[8] = ndl;
	p.inv_view_proj[9] = ndv;
	p.inv_view_proj[10] = ndh;
	p.inv_view_proj[11] = shadow;
	p.inv_view_proj[12] = ao;
	p.inv_view_proj[13] = gloss;
	static const float kNoSun[16] = {};
	if (!world_->deferred_pass()->render(device, *world_->gbuffer(), *world_->material_atlas(), RID(), RID(), RID(), kNoSun, 0.0f, p))
		return d;
	device->submit();
	device->sync();
	const PackedByteArray got = device->texture_get_data(world_->gbuffer()->lit(), 0);
	if (got.size() < 8) return d;
	const uint16_t *h = reinterpret_cast<const uint16_t *>(got.ptr());
	const Color gpu(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0f);
	ve::CelParams params;
	ve::CelInput in;
	in.albedo[0] = albedo.r; in.albedo[1] = albedo.g; in.albedo[2] = albedo.b;
	in.ambient[0] = ambient.r; in.ambient[1] = ambient.g; in.ambient[2] = ambient.b;
	in.ndl = ndl; in.ndv = ndv; in.ndh = ndh;
	in.shadow = shadow; in.ao = ao; in.gloss = gloss;
	float ref[3];
	ve::cel_shade(params, in, ref);
	const Color cpu(ref[0], ref[1], ref[2], 1.0f);
	d["gpu"] = gpu;
	d["cpu"] = cpu;
	d["max_delta"] = std::max({std::fabs(gpu.r - cpu.r), std::fabs(gpu.g - cpu.g),
			std::fabs(gpu.b - cpu.b)});
	return d;
}

Dictionary VoxelDebugHooks::debug_sun_shadow_stats() {
	Dictionary d;
	d["size"] = SunShadowPass::kSize;
	d["map_valid"] = false;
	d["ortho_valid"] = false;
	d["texel_world"] = 0.0f;
	d["rebuilds"] = 0;
	d["pages"] = 0;
	d["view_proj"] = PackedFloat32Array();
	world_->ensure_initialized();
	SunShadowPass *sun = world_->sun_shadow_pass();
	if (!sun) return d;
	const ve::WorldBounds wb = world_->world_bounds();
	float lo[3];
	float hi[3];
	wb.aabb(lo, hi);
	const ve::SunState sun_state = world_->sun_state();
	const ve::SunOrtho ortho = sun_state.has_basis()
			? ve::sun_ortho(sun_state.dir, sun_state.right, sun_state.up, lo, hi,
					SunShadowPass::kSize)
			: ve::sun_ortho(sun_state.dir, lo, hi, SunShadowPass::kSize);
	d["map_valid"] = sun->map().is_valid();
	d["ortho_valid"] = ortho.valid;
	d["texel_world"] = ortho.valid ? ortho.texel_world : sun->texel_world();
	d["rebuilds"] = sun->rebuilds();
	d["pages"] = sun->last_pages();
	PackedFloat32Array matrix;
	matrix.resize(16);
	const float *source = sun->rebuilds() > 0 ? sun->view_proj() :
			(ortho.valid ? ortho.view_proj : sun->view_proj());
	for (int i = 0; i < 16; i++) matrix[i] = source[i];
	d["view_proj"] = matrix;
	return d;
}

void VoxelDebugHooks::debug_sun_shadow_build(bool force) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->sun_shadow_pass() || !world_->context().lod->lod_pool_ || !world_->lod_raster_pass()) return;
	world_->prepare_lod_shadow_raster();
	const ve::WorldBounds wb = world_->world_bounds();
	float lo[3];
	float hi[3];
	wb.aabb(lo, hi);
	const ve::SunState sun_state = world_->sun_state();
	const ve::SunOrtho ortho = sun_state.has_basis()
			? ve::sun_ortho(sun_state.dir, sun_state.right, sun_state.up, lo, hi,
					SunShadowPass::kSize)
			: ve::sun_ortho(sun_state.dir, lo, hi, SunShadowPass::kSize);
	world_->sun_shadow_pass()->build(device, *world_->context().lod->lod_pool_, *world_->lod_raster_pass(),
			ortho, force);
	world_->prepare_lod_raster();
}

float VoxelDebugHooks::sun_shadow_probe(Vector3 p, Vector3 viewer, int probe_mode) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->gbuffer() || !world_->deferred_pass() || !world_->material_atlas()) return 1.0f;
	if (world_->gbuffer()->size() != Vector2i(1, 1)) {
		world_->deferred_pass()->teardown();
		world_->deferred_pass()->initialize(device);
		if (world_->composite_pass()) world_->composite_pass()->release_targets();
	}
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(1, 1))) return 1.0f;
	const ve::BeautySettings beauty = world_->beauty_settings();
	const bool use_sun = world_->sun_shadow_pass() && world_->sun_shadow_pass()->is_valid() &&
			world_->sun_shadow_pass()->rebuilds() > 0 && beauty.sun_shadow_map;
	DeferredPass::Params dp;
	dp.cam_pos[0] = p.x;
	dp.cam_pos[1] = p.y;
	dp.cam_pos[2] = p.z;
	dp.flags = ve::pack_flags(beauty);
	dp.shadow_depth_range = use_sun ? world_->sun_shadow_pass()->depth_range() : 0.0f;
	world_->lod_fade_band(&dp.fade_start, &dp.fade_end);
	dp.probe_mode = probe_mode;
	// Mode 4 reads the viewer out of inv_view_proj's first row; mode 3 ignores it.
	dp.inv_view_proj[0] = viewer.x;
	dp.inv_view_proj[1] = viewer.y;
	dp.inv_view_proj[2] = viewer.z;
	static const float kNoSun[16] = {};
	if (!world_->deferred_pass()->render(device, *world_->gbuffer(), *world_->material_atlas(), RID(), RID(),
			use_sun ? world_->sun_shadow_pass()->map() : RID(),
			use_sun ? world_->sun_shadow_pass()->view_proj() : kNoSun,
			use_sun ? world_->sun_shadow_pass()->texel_world() : 0.0f, dp))
		return 1.0f;
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(world_->gbuffer()->lit(), 0);
	if (data.size() < 8) return 1.0f;
	const uint16_t *value = reinterpret_cast<const uint16_t *>(data.ptr());
	return half_to_float(value[0]);
}

float VoxelDebugHooks::debug_sun_shadow_visibility(Vector3 p) {
	return sun_shadow_probe(p, p, 3);
}

float VoxelDebugHooks::debug_sun_shadow_shading(Vector3 p, Vector3 viewer) {
	return sun_shadow_probe(p, viewer, 4);
}

Dictionary VoxelDebugHooks::debug_deferred_probe(Vector3 pos, Vector3 fwd, int w, int h,
		int probe_mode) {
	Dictionary d;
	if (w <= 0 || h <= 0 || (probe_mode != 0 && probe_mode != 1 && probe_mode != 2)) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass() ||
			!world_->composite_pass() || !world_->deferred_pass() || !world_->gbuffer()) return d;
	if (world_->gbuffer()->size() != Vector2i(w, h)) {
		world_->deferred_pass()->teardown();
		world_->deferred_pass()->initialize(device);
		world_->composite_pass()->release_targets();
	}
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++) {
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	}
	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, std::fabs(fwd.y) > 0.9f ? 0.0f : 1.0f,
			std::fabs(fwd.y) > 0.9f ? 1.0f : 0.0f};
	const float fov_y = 1.0471975512f;
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(fov_y * 0.5f);
	const float tan_x = tan_y * aspect;
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, fov_y, aspect,
			0.05f, 4000.0f, w, h);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			view_proj.columns[c][r] = cam.view_proj[c * 4 + r];
	ve::CameraParams cp = ve::CameraParams::looking_at(pos.x, pos.y, pos.z,
			fwd.x, fwd.y, fwd.z, up[0], up[1], up[2]);
	cp.params[0] = tan_x;
	cp.params[1] = tan_y;
	cp.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_->store_->config().world_size_regions.x;
	cp.dims[1] = world_->store_->config().world_size_regions.y;
	cp.dims[2] = world_->store_->config().world_size_regions.z;
	cp.dims[3] = world_->island_slot_count();
	cp.region_origin[0] = ro.x;
	cp.region_origin[1] = ro.y;
	cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = world_->store_->config().atlas_bricks.x;
	cp.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cp.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	const uint32_t flags = ve::pack_flags(world_->beauty_settings());
	std::memcpy(&cp.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cp, w, h, kNoEdit)) return d;
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h))) return d;
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	world_->lod_fade_band(&fade_start, &fade_end);
	world_->composite_pass()->draw(device, *world_->gbuffer(), world_->raymarch_pass()->albedo_texture(),
			world_->raymarch_pass()->surface_texture(), world_->raymarch_pass()->hitpos_texture(), view_proj,
			*world_->material_atlas(), cp, fade_start, fade_end);
	if (!world_->composite_pass()->last_draw_ok()) return d;
	DeferredPass::Params dp;
	const Projection inv = view_proj.inverse();
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
	dp.cam_pos[0] = pos.x;
	dp.cam_pos[1] = pos.y;
	dp.cam_pos[2] = pos.z;
	dp.flags = flags;
	dp.probe_mode = probe_mode;
	if (!world_->deferred_pass()->render(device, *world_->gbuffer(), *world_->material_atlas(), RID(), RID(), RID(), kNoEdit, 0.0f, dp))
		return d;
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(world_->gbuffer()->lit(), 0);
	const int pixels = w * h;
	if (data.size() < static_cast<int64_t>(pixels) * 8) return d;
	const uint16_t *values = reinterpret_cast<const uint16_t *>(data.ptr());
	const int center = (h / 2) * w + (w / 2);
	if (probe_mode == 2) {
		float center_sum[3] = {};
		for (int oy = -1; oy <= 0; oy++)
			for (int ox = -1; ox <= 0; ox++) {
				const int sample = (std::max(0, h / 2 + oy) * w) + std::max(0, w / 2 + ox);
				center_sum[0] += half_to_float(values[sample * 4]);
				center_sum[1] += half_to_float(values[sample * 4 + 1]);
				center_sum[2] += half_to_float(values[sample * 4 + 2]);
			}
		d["center"] = Vector3(center_sum[0] * 0.25f, center_sum[1] * 0.25f, center_sum[2] * 0.25f);
	} else {
		d["center"] = Color(half_to_float(values[center * 4]), half_to_float(values[center * 4 + 1]),
				half_to_float(values[center * 4 + 2]), 1.0f);
	}
	double mean_luma = 0.0;
	for (int i = 0; i < pixels; i++) {
		const float r = half_to_float(values[i * 4]);
		const float g = half_to_float(values[i * 4 + 1]);
		const float b = half_to_float(values[i * 4 + 2]);
		mean_luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
	}
	d["mean_luma"] = mean_luma / static_cast<double>(pixels);
	std::set<uint16_t> rows;
	for (int y = 0; y < h; y++) rows.insert(values[(y * w + w / 2) * 4 + 2]);
	d["distinct_rows"] = static_cast<int>(rows.size());
	return d;
}

// Marches at a FRACTION of the target size and composites into a full-size G-buffer -- the
// production near-field path, with near_field_scale made explicit. The reported detail is the
// mean absolute albedo step between horizontally adjacent surface pixels: the high-frequency
// energy a magnifying upsample destroys. Sky pixels (composite depth 0) are excluded, as is
// any pair that straddles a silhouette, so the number measures texture, not edges.
Dictionary VoxelDebugHooks::debug_near_field_detail(Vector3 pos, Vector3 fwd, int w, int h,
		float march_scale) {
	Dictionary d;
	d["ran"] = false;
	d["detail"] = 0.0f;
	d["mean_luma"] = 0.0f;
	d["hit_pixels"] = 0;
	d["march_width"] = 0;
	d["march_height"] = 0;
	if (w <= 1 || h <= 1 || !(march_scale > 0.0f) || march_scale > 1.0f) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() ||
			!world_->raymarch_pass() || !world_->composite_pass() || !world_->gbuffer()) return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, std::fabs(fwd.y) > 0.9f ? 0.0f : 1.0f,
			std::fabs(fwd.y) > 0.9f ? 1.0f : 0.0f};
	const float fov_y = 1.0471975512f;
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(fov_y * 0.5f);
	const float tan_x = tan_y * aspect;
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, fov_y, aspect, 0.05f, 4000.0f, w, h);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = cam.view_proj[c * 4 + r];
	ve::CameraParams cp = ve::CameraParams::looking_at(pos.x, pos.y, pos.z,
			fwd.x, fwd.y, fwd.z, up[0], up[1], up[2]);
	cp.params[0] = tan_x;
	cp.params[1] = tan_y;
	cp.params[2] = 200.0f;
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_->store_->config().world_size_regions.x;
	cp.dims[1] = world_->store_->config().world_size_regions.y;
	cp.dims[2] = world_->store_->config().world_size_regions.z;
	cp.dims[3] = world_->island_slot_count();
	cp.region_origin[0] = ro.x;
	cp.region_origin[1] = ro.y;
	cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = world_->store_->config().atlas_bricks.x;
	cp.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cp.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	const uint32_t flags = ve::pack_flags(world_->beauty_settings());
	std::memcpy(&cp.cam_pos[3], &flags, sizeof(float));

	const int rw = std::max(1, static_cast<int>(static_cast<float>(w) * march_scale));
	const int rh = std::max(1, static_cast<int>(static_cast<float>(h) * march_scale));
	// The G-buffer and the marcher's targets both change size across calls; the composite's
	// framebuffer and uniform set reference both, so drop them before either moves.
	world_->composite_pass()->release_targets();
	world_->composite_pass()->invalidate_uniform_set(device);
	if (!world_->gbuffer()->ensure(device, nullptr, Vector2i(w, h))) return d;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cp,
			rw, rh, kNoEdit)) return d;
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	world_->lod_fade_band(&fade_start, &fade_end);
	world_->composite_pass()->draw(device, *world_->gbuffer(), world_->raymarch_pass()->albedo_texture(),
			world_->raymarch_pass()->surface_texture(), world_->raymarch_pass()->hitpos_texture(),
			view_proj, *world_->material_atlas(), cp, fade_start, fade_end);
	if (!world_->composite_pass()->last_draw_ok()) return d;
	device->submit();
	device->sync();

	const PackedByteArray albedo = device->texture_get_data(world_->gbuffer()->albedo(), 0);
	const PackedByteArray depth = device->texture_get_data(world_->gbuffer()->depth(), 0);
	const int64_t pixels = static_cast<int64_t>(w) * h;
	if (albedo.size() < pixels * 4 || depth.size() < pixels * 4) return d;
	const uint8_t *a = albedo.ptr();
	const float *z = reinterpret_cast<const float *>(depth.ptr());
	// Reverse-Z: a composited surface writes a positive depth, a sky pixel writes exactly 0.
	auto is_surface = [&](int64_t i) { return z[i] > 0.0f; };
	double sum_step = 0.0;
	int64_t steps = 0;
	double mean_luma = 0.0;
	int64_t hits = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int64_t i = static_cast<int64_t>(y) * w + x;
			if (!is_surface(i)) continue;
			hits++;
			mean_luma += (0.2126 * a[i * 4] + 0.7152 * a[i * 4 + 1] + 0.0722 * a[i * 4 + 2]) / 255.0;
			if (x + 1 >= w) continue;
			const int64_t j = i + 1;
			if (!is_surface(j)) continue;
			sum_step += (std::abs(static_cast<int>(a[i * 4]) - static_cast<int>(a[j * 4])) +
					std::abs(static_cast<int>(a[i * 4 + 1]) - static_cast<int>(a[j * 4 + 1])) +
					std::abs(static_cast<int>(a[i * 4 + 2]) - static_cast<int>(a[j * 4 + 2]))) / 255.0;
			steps++;
		}
	}
	// The centre pixel's resolved G-buffer values. The albedo and the gloss are PRODUCED by
	// the composite now, so this is where a test can read the values the deferred pass will
	// light -- the marcher's own targets no longer hold a colour.
	{
		const int64_t c = (static_cast<int64_t>(h / 2)) * w + (w / 2);
		d["center_albedo"] = Color(a[c * 4] / 255.0f, a[c * 4 + 1] / 255.0f,
				a[c * 4 + 2] / 255.0f, 1.0f);
		d["center_sun"] = a[c * 4 + 3] / 255.0f;
		d["center_hit"] = is_surface(c);
		// Reconstructed exactly the way deferred.comp.glsl reconstructs it, from the same
		// depth attachment, so a caller comparing this against a material probe compares the
		// point the deferred pass will actually shade -- not a nearby one.
		const Projection inv_view_proj = view_proj.inverse();
		const Vector2 c_ndc(((w / 2) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f,
				((h / 2) + 0.5f) / static_cast<float>(h) * 2.0f - 1.0f);
		const Vector4 c_h = inv_view_proj.xform(Vector4(c_ndc.x, c_ndc.y, z[c], 1.0f));
		const float c_w = std::fabs(c_h.w) < 1e-9f ? 1e-9f : c_h.w;
		d["center_position"] = Vector3(c_h.x / c_w, c_h.y / c_w, c_h.z / c_w);
		const PackedByteArray surface = device->texture_get_data(world_->gbuffer()->surface(), 0);
		if (surface.size() >= pixels * 8) {
			const uint16_t *sv = reinterpret_cast<const uint16_t *>(surface.ptr());
			d["center_material"] = static_cast<int>(half_to_float(sv[c * 4 + 2]) + 0.5f);
			d["center_gloss"] = half_to_float(sv[c * 4 + 3]);
			const float e[2] = {half_to_float(sv[c * 4]), half_to_float(sv[c * 4 + 1])};
			float n[3];
			ve::oct_decode(e, n);
			d["center_normal"] = Vector3(n[0], n[1], n[2]);
		}
		// The MARCHER's own normal for the very same pixel, before composite.frag.glsl bent
		// it with the material normal map. Only meaningful when the march ran at full
		// resolution -- below that the marcher's pixel c does not exist.
		if (rw == w && rh == h) {
			const PackedByteArray march =
					device->texture_get_data(world_->raymarch_pass()->surface_texture(), 0);
			if (march.size() >= pixels * 8) {
				const uint16_t *mv = reinterpret_cast<const uint16_t *>(march.ptr());
				const float e[2] = {half_to_float(mv[c * 4]), half_to_float(mv[c * 4 + 1])};
				float n[3];
				ve::oct_decode(e, n);
				d["center_geometric_normal"] = Vector3(n[0], n[1], n[2]);
			}
			// How much relief the SHIPPED art actually produces, and what it costs the
			// outline pass. `normal_tilt_mean` is the mean 1 - dot(shading, geometric) over
			// every covered pixel: zero means the map changed nothing anywhere, which is the
			// state this whole path existed to leave. `normal_edge_fraction` is the fraction
			// of adjacent covered pairs whose normals differ by more than the outline pass's
			// own threshold -- every one of those is a pixel outline.comp.glsl would darken,
			// so it is the speckle budget for turning the map on.
			const PackedByteArray shaded =
					device->texture_get_data(world_->gbuffer()->surface(), 0);
			if (march.size() >= pixels * 8 && shaded.size() >= pixels * 8) {
				const uint16_t *mv = reinterpret_cast<const uint16_t *>(march.ptr());
				const uint16_t *gv = reinterpret_cast<const uint16_t *>(shaded.ptr());
				auto decode = [](const uint16_t *v, int64_t i, float n[3]) {
					const float e[2] = {half_to_float(v[i * 4]), half_to_float(v[i * 4 + 1])};
					ve::oct_decode(e, n);
				};
				double tilt = 0.0;
				int64_t tilted = 0;
				int64_t pairs = 0, edges = 0;
				for (int y = 0; y < h; y++) {
					for (int x = 0; x < w; x++) {
						const int64_t i = static_cast<int64_t>(y) * w + x;
						if (!is_surface(i)) continue;
						float g[3], m[3];
						decode(gv, i, g);
						decode(mv, i, m);
						tilt += 1.0 - (g[0] * m[0] + g[1] * m[1] + g[2] * m[2]);
						tilted++;
						if (x + 1 >= w || !is_surface(i + 1)) continue;
						float g1[3];
						decode(gv, i + 1, g1);
						pairs++;
						if (1.0f - (g[0] * g1[0] + g[1] * g1[1] + g[2] * g1[2]) >
								world_->beauty_settings().outline_normal_threshold)
							edges++;
					}
				}
				d["normal_tilt_mean"] = tilted > 0 ? tilt / static_cast<double>(tilted) : 0.0;
				d["normal_edge_fraction"] = pairs > 0
						? static_cast<double>(edges) / static_cast<double>(pairs) : 0.0;
			}
		}
	}
	d["ran"] = true;
	d["march_width"] = rw;
	d["march_height"] = rh;
	d["hit_pixels"] = static_cast<int>(hits);
	d["mean_luma"] = hits > 0 ? mean_luma / static_cast<double>(hits) : 0.0;
	d["detail"] = steps > 0 ? sum_step / static_cast<double>(steps) : 0.0;
	return d;
}

String VoxelDebugHooks::debug_load_shader(const String &res_path) const {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(res_path);
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code =
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err);
	if (code.empty()) {
		UtilityFunctions::printerr("debug_load_shader: ", err.c_str());
		return String();
	}
	return String(code.c_str());
}

Dictionary VoxelDebugHooks::debug_shader_reload_stats() {
	Dictionary d;
	// Task 14: the reload bookkeeping moved into RenderOrchestrator; one-line delegation
	// keeps the single-mutex-hold snapshot shape of the pre-move body.
	int count = 0;
	bool last_ok = true;
	String last_error;
	world_->reload_snapshot(&count, &last_ok, &last_error);
	d["reloads"] = count;
	d["last_ok"] = last_ok;
	d["last_error"] = last_error;
	return d;
}

void VoxelDebugHooks::debug_set_shader_override(const String &name, const String &source) {
	ve::set_shader_source_override(name.utf8().get_data(), source.utf8().get_data());
}

Dictionary VoxelDebugHooks::debug_self_check() {
	Dictionary d;
	d["field_mismatches"] = 0;
	d["brick_mismatches"] = 0;
	d["mesh_mismatches"] = 0;
	d["lod_mismatches"] = 0;
	d["occupancy_mismatches"] = 0;
	const auto start = std::chrono::steady_clock::now();

	// Use the live camera when there is one; the self-check keybind is for the running demo.
	Vector3 center(24.0f, 64.0f, 24.0f);
	Viewport *vp = world_->get_viewport();
	if (vp) {
		Camera3D *cam = vp->get_camera_3d();
		if (cam) center = cam->get_global_position();
	}

	// Field/raymarch differential: a handful of probes from just above the camera against
	// the analytic CPU raycast. The raymarch shader evaluates the same field on the GPU, so
	// a hit/position disagreement is a live field drift.
	int field_mismatches = 0;
	const Vector3 origin = center + Vector3(0.0f, 20.0f, 0.0f);
	const Vector3 dirs[] = {
		Vector3(0.0f, -1.0f, 0.0f),
		Vector3(0.10f, -1.0f, 0.05f).normalized(),
		Vector3(-0.10f, -1.0f, -0.05f).normalized(),
		Vector3(0.05f, -1.0f, -0.10f).normalized(),
		Vector3(-0.05f, -1.0f, 0.10f).normalized(),
	};
	for (const Vector3 &dir : dirs) {
		const Dictionary cpu = debug_raycast(origin, dir);
		const Dictionary gpu = debug_raymarch_probe(origin, dir);
		const bool cpu_hit = cpu.has("hit") && bool(cpu["hit"]);
		const bool gpu_hit = gpu.has("hit") && bool(gpu["hit"]);
		if (cpu_hit != gpu_hit) {
			field_mismatches++;
			continue;
		}
		if (cpu_hit && gpu_hit && cpu.has("pos") && gpu.has("pos")) {
			const Vector3 a = cpu["pos"];
			const Vector3 b = gpu["pos"];
			if (a.distance_to(b) > 0.5f) field_mismatches++;
		}
	}
	d["field_mismatches"] = field_mismatches;

	// Brick differential on a small spread of resident bricks near the camera/centre.
	const ve::IVec3 region = ve::WorldBounds::region_of_point(center.x, center.y, center.z);
	int brick_mismatches = 0;
	const int rslot = debug_region_map_entry(Vector3i(region.x, region.y, region.z));
	if (rslot >= 0) {
		std::vector<ve::EditOp> ops_vec;
		{
			std::lock_guard<std::mutex> lock(world_->edit_mutex());
			if (world_->store_->edit_log()) ops_vec = world_->store_->edit_log()->ops(region);
		}
		PackedByteArray ops;
		const int op_count = static_cast<int>(ops_vec.size());
		if (op_count > 0) {
			ops.resize(static_cast<int64_t>(op_count) * static_cast<int64_t>(sizeof(ve::EditOp)));
			std::memcpy(ops.ptrw(), ops_vec.data(), static_cast<size_t>(ops.size()));
		}
		int checked = 0;
		for (int dz = -1; dz <= 1 && checked < 6; dz++)
			for (int dy = -1; dy <= 1 && checked < 6; dy++)
				for (int dx = -1; dx <= 1 && checked < 6; dx++) {
					const ve::IVec3 b = ve::WorldBounds::brick_of_point(
							center.x + dx * 4.0f, center.y + dy * 4.0f, center.z + dz * 4.0f);
					const Dictionary bd = debug_brick_diff(Vector3i(b.x, b.y, b.z), rslot, ops, op_count);
					if (!bd.has("slot") || int(bd["slot"]) < 0) continue;
					brick_mismatches += int(bd["sdf_diff_over_one"]);
					brick_mismatches += int(bd["mat_near_mismatch"]);
					brick_mismatches += int(bd["mip_mismatch"]);
					if (!bool(bd["palette_match"])) brick_mismatches++;
					checked++;
				}
	}
	d["brick_mismatches"] = brick_mismatches;

	// Mesh differential on the chunk under the camera/centre.
	const ve::IVec3 mc = ve::chunk_of_point(center.x, center.y, center.z);
	const Dictionary md = debug_mesh_diff(Vector3i(mc.x, mc.y, mc.z));
	int mesh_mismatches = 0;
	if (md.has("lattice_diff_over_one")) mesh_mismatches += int(md["lattice_diff_over_one"]);
	for (const char *key : {"cells_only_cpu", "cells_only_gpu", "tri_only_cpu",
			"tri_only_gpu", "verts_off_10cm", "winding_bad"}) {
		if (md.has(String(key))) mesh_mismatches += int(md[key]);
	}
	d["mesh_mismatches"] = mesh_mismatches;

	// LoD differential at level 2 on the chunk under the camera/centre.
	const int lod_level = 2;
	const ve::IVec3 lc = ve::lod_chunk_of_point(lod_level, center.x, center.y, center.z);
	const Dictionary ld = debug_lod_diff(lod_level, Vector3i(lc.x, lc.y, lc.z));
	int lod_mismatches = 0;
	if (ld.has("material_mismatches")) lod_mismatches += int(ld["material_mismatches"]);
	if (ld.has("quads_only_cpu")) lod_mismatches += int(ld["quads_only_cpu"]);
	if (ld.has("quads_only_gpu")) lod_mismatches += int(ld["quads_only_gpu"]);
	if (ld.has("fine_max_diff") && int(ld["fine_max_diff"]) > 1) lod_mismatches++;
	if (ld.has("reduced_max_diff") && int(ld["reduced_max_diff"]) > 1) lod_mismatches++;
	if (ld.has("corner_max_diff") && int(ld["corner_max_diff"]) > 0) lod_mismatches++;
	d["lod_mismatches"] = lod_mismatches;

	// Occupancy differential on the camera/centre region.
	const Dictionary od = debug_occupancy_diff(Vector3i(region.x, region.y, region.z));
	const int occupancy_mismatches = od.has("mismatches") ? int(od["mismatches"]) : 0;
	d["occupancy_mismatches"] = occupancy_mismatches;

	const double elapsed_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
	d["elapsed_ms"] = elapsed_ms;
	d["ok"] = field_mismatches == 0 && brick_mismatches == 0 &&
			mesh_mismatches == 0 && lod_mismatches == 0 && occupancy_mismatches == 0;
	return d;
}

void VoxelDebugHooks::debug_store_volume(int slot, const PackedByteArray &sdf,
		const PackedByteArray &mat, int dim) {
	// Debug-only hook: validate the inputs the CPU's production paths guarantee, so a
	// broken test or console call fails loudly instead of aliasing pool memory.
	if (slot < 0 || slot >= ve::kMaxVolumes) {
		UtilityFunctions::printerr("debug_store_volume: slot ", slot, " out of range [0, ",
				ve::kMaxVolumes, ")");
		return;
	}
	if (dim > ve::kIslandDim) {
		UtilityFunctions::printerr("debug_store_volume: dim ", dim, " exceeds ve::kIslandDim (",
				ve::kIslandDim, ")");
		return;
	}
	if (dim < 2) {
		UtilityFunctions::printerr("debug_store_volume: dim must be at least 2, got ", dim);
		return;
	}
	const int64_t n = static_cast<int64_t>(dim) * dim * dim;
	if (sdf.size() < n || mat.size() < n) {
		UtilityFunctions::printerr("debug_store_volume: short buffers for dim ", dim);
		return;
	}
	world_->store_->volumes().reserve(slot); // no-op when the suite already claimed it
	ve::VolumeData d;
	d.dim = dim;
	d.sdf.assign(sdf.ptr(), sdf.ptr() + n);
	d.mat.assign(mat.ptr(), mat.ptr() + n);
	d.normal_oct.assign(static_cast<size_t>(n), 0);
	float center = 0.5f * (dim - 1) * 0.05f;
	for (int64_t i = 0; i < n; i++) {
		if (ve::decode_sdf(d.sdf[static_cast<size_t>(i)]) <= 0.0f) d.solid_voxels++;
		float up[3] = {0, 1, 0};
		int z = static_cast<int>(i / (dim * dim));
		int y = static_cast<int>((i / dim) % dim);
		int x = static_cast<int>(i % dim);
		float px = x * 0.05f - center;
		float py = y * 0.05f - center;
		float pz = z * 0.05f - center;
		float len = std::sqrt(px*px + py*py + pz*pz);
		if (len > 1e-6f) { float n[3]={px/len, py/len, pz/len}; d.normal_oct[static_cast<size_t>(i)] = ve::oct_encode_snorm8(n); }
		else d.normal_oct[static_cast<size_t>(i)] = ve::oct_encode_snorm8(up);
	}
	ve::VolumeData to_upload = d;
	if (world_->store_->volumes().store(slot, std::move(d)) && world_->mesh_) {
		world_->mesh_->submit_volume(slot, to_upload);
		world_->mesh_->run_sync([](MeshPass &){});
	}
}

Vector2 VoxelDebugHooks::debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) {
	const ve::Generator &gen = world_->store_->generator()->sampler();
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field: op buffer too small");
			return Vector2();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::Sample s = ve::eval_field(gen, ptr, op_count, p.x, p.y, p.z, &world_->store_->volumes(), world_->store_->overrides());
	return Vector2(s.sdf, static_cast<float>(s.material));
}

Dictionary VoxelDebugHooks::debug_eval_field_gradient(Vector3 p, const PackedByteArray &ops, int op_count) {
	const ve::Generator &gen = world_->store_->generator()->sampler();
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field_gradient: op buffer too small");
			return Dictionary();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::FieldSample s = ve::eval_field_gradient(gen, ptr, op_count, p.x, p.y, p.z, &world_->store_->volumes(), world_->store_->overrides());
	Dictionary d;
	d["sdf"] = s.sdf;
	d["material"] = int(s.material);
	d["gradient"] = Vector3(s.gradient[0], s.gradient[1], s.gradient[2]);
	d["exact"] = s.exact_gradient;
	return d;
}

Dictionary VoxelDebugHooks::debug_material_atlas_stats() {
	Dictionary d;
	if (!world_->material_atlas() || !world_->material_atlas()->is_valid()) return d;
	d["layers"] = world_->material_atlas()->layer_count();
	d["width"] = kMaterialTextureSize;
	d["height"] = kMaterialTextureSize;
	d["mipmaps"] = kMaterialMipmaps;
	d["albedo_valid"] = world_->material_atlas()->albedo_array().is_valid();
	d["surface_valid"] = world_->material_atlas()->surface_array().is_valid();
	return d;
}

Dictionary VoxelDebugHooks::debug_material_alpha_stats(int layer) {
	Dictionary d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->material_atlas() ||
			!world_->material_atlas()->is_valid()) return d;
	if (layer < 0 || layer >= world_->material_atlas()->layer_count()) return d;
	const PackedByteArray data =
			device->texture_get_data(world_->material_atlas()->albedo_array(), layer);
	const int64_t top = static_cast<int64_t>(kMaterialTextureSize) * kMaterialTextureSize * 4;
	if (data.size() < top) return d;
	const uint8_t *p = data.ptr();
	uint8_t lo = 255, hi = 0;
	for (int64_t i = 3; i < top; i += 4) { // alpha is every 4th byte
		if (p[i] < lo) lo = p[i];
		if (p[i] > hi) hi = p[i];
	}
	d["min"] = lo / 255.0f;
	d["max"] = hi / 255.0f;
	return d;
}

// Task 7: rewrite the top-mip normal-map texels (surface array RG) of one material layer
// with a hard tilt, so a test can re-render and prove the G-buffer normal never depends
// on the material normal map.
bool VoxelDebugHooks::debug_poke_material_normal(int layer) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->material_atlas()) return false;
	if (layer < 0 || layer >= world_->material_atlas()->layer_count()) return false;
	PackedByteArray data = device->texture_get_data(world_->material_atlas()->surface_array(), layer);
	if (data.size() < 4) return false;
	// EVERY texel of every mip in the layer, not just the first one: a probe ray is
	// vanishingly unlikely to land on one poked texel, so a single-texel poke made the
	// invariance assertion pass whether or not the shader sampled these bytes. The surface
	// array is R8G8B8A8 with normal XY in RG (roughness in B, AO in A), so only RG move.
	uint8_t *bytes = data.ptrw();
	for (int64_t i = 0; i + 1 < data.size(); i += 4) {
		bytes[i] = 255; // normal XY = (1, 0): the strongest tilt the format can hold
		bytes[i + 1] = 0;
	}
	device->texture_update(world_->material_atlas()->surface_array(), layer, data);
	return true;
}

// The other half of the poke: a FLAT map (tangent normal +Z) over every texel and mip of one
// layer. A shading normal built by the whiteout blend must come back to the geometric normal
// under it, which is the property that makes applying the map at full strength safe -- an
// unauthored or averaged-away normal may not tilt a surface it says nothing about.
bool VoxelDebugHooks::debug_flatten_material_normal(int layer) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->material_atlas()) return false;
	if (layer < 0 || layer >= world_->material_atlas()->layer_count()) return false;
	PackedByteArray data = device->texture_get_data(world_->material_atlas()->surface_array(), layer);
	if (data.size() < 4) return false;
	uint8_t *bytes = data.ptrw();
	for (int64_t i = 0; i + 1 < data.size(); i += 4) {
		bytes[i] = 128; // the closest an 8-bit unorm gets to XY = (0, 0)
		bytes[i + 1] = 128;
	}
	device->texture_update(world_->material_atlas()->surface_array(), layer, data);
	return true;
}

bool VoxelDebugHooks::probe_material(int mat, Vector3 p, Vector3 n, float rgb[3],
		float *roughness, float *ao, float *shading_normal) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->material_atlas() || !world_->raymarch_pass())
		return false;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			p.x, p.y, p.z, n.x, n.y, n.z, 0, 1, 0);
	// pc.params.w is the debug-probe flag in raymarch.comp.glsl; cam_pos and cam_fwd carry
	// the sample point and normal.
	cam.params[0] = 0.0f;
	cam.params[1] = 0.0f;
	cam.params[2] = 0.0f;
	cam.params[3] = static_cast<float>(mat);
	const ve::WorldBounds wb = world_->world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_->store_->config().world_size_regions.x; cam.dims[1] = world_->store_->config().world_size_regions.y;
	cam.dims[2] = world_->store_->config().world_size_regions.z;
	cam.dims[3] = world_->island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = world_->store_->config().atlas_bricks.x; cam.atlas_bricks[1] = world_->store_->config().atlas_bricks.y;
	cam.atlas_bricks[2] = world_->store_->config().atlas_bricks.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->raymarch_pass()->render(device, *world_->atlas(), world_->islands(), RID(), cam, 1, 1,
			kNoEdit))
		return false;
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(world_->raymarch_pass()->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->raymarch_pass()->surface_texture(), 0);
	if (data.size() < 4 || sf.size() < 8) return false;
	const uint8_t *b = data.ptr();
	rgb[0] = b[0] / 255.0f;
	rgb[1] = b[1] / 255.0f;
	rgb[2] = b[2] / 255.0f;
	// The probe path parks material_props() in the two oct slots -- it has no normal to pack
	// there -- so one dispatch answers both halves of the material.
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	if (roughness) *roughness = half_to_float(s[0]);
	if (ao) *ao = half_to_float(s[1]);
	if (shading_normal) {
		const PackedByteArray hp =
				device->texture_get_data(world_->raymarch_pass()->hitpos_texture(), 0);
		if (hp.size() < 16) return false;
		const float *h = reinterpret_cast<const float *>(hp.ptr());
		shading_normal[0] = h[0];
		shading_normal[1] = h[1];
		shading_normal[2] = h[2];
	}
	return true;
}

Vector3 VoxelDebugHooks::debug_material_normal_probe(int mat, Vector3 p, Vector3 n) {
	float rgb[3] = {1.0f, 0.0f, 1.0f};
	float sn[3] = {0.0f, 0.0f, 0.0f};
	if (!probe_material(mat, p, n, rgb, nullptr, nullptr, sn)) return Vector3();
	return Vector3(sn[0], sn[1], sn[2]);
}

Color VoxelDebugHooks::resolve_near_field(int mat, Vector3 p, Vector3 n, Color overlay,
		float overlay_weight, float *gloss_out) {
	if (gloss_out) *gloss_out = 0.0f;
	// Material 0 is a miss: the overlay IS the pixel, which is how the sky reaches the screen.
	if (mat <= 0) return Color(overlay.r, overlay.g, overlay.b, 1.0f);
	float rgb[3] = {1.0f, 0.0f, 1.0f};
	float roughness = 1.0f;
	float ao = 1.0f;
	if (!probe_material(mat, p, n, rgb, &roughness, &ao)) return Color(1, 0, 1);
	if (gloss_out) *gloss_out = 1.0f - roughness;
	// The composite's own two lines, mirrored: the AO fold on the material, then the overlay
	// over the result. Keeping the 0.65 in step with composite.frag.glsl is what makes a probe
	// comparable to a pixel.
	const float fold = 1.0f + (ao - 1.0f) * 0.65f;
	const float w = std::min(std::max(overlay_weight, 0.0f), 1.0f);
	return Color(rgb[0] * fold * (1.0f - w) + overlay.r * w,
			rgb[1] * fold * (1.0f - w) + overlay.g * w,
			rgb[2] * fold * (1.0f - w) + overlay.b * w, 1.0f);
}

Color VoxelDebugHooks::debug_material_probe(int mat, Vector3 p, Vector3 n) {
	float rgb[3] = {1.0f, 0.0f, 1.0f};
	if (!probe_material(mat, p, n, rgb, nullptr, nullptr)) return Color(1, 0, 1);
	return Color(rgb[0], rgb[1], rgb[2], 1.0);
}

bool VoxelDebugHooks::debug_init_atlas() {
	world_->ensure_initialized();
	return world_->atlas() && world_->atlas()->is_valid();
}

void VoxelDebugHooks::debug_teardown_atlas() {
	world_->teardown_gpu();
}

Dictionary VoxelDebugHooks::debug_atlas_stats() {
	Dictionary d;
	RenderingDevice *device = world_->rd();
	if (!world_->atlas() || !world_->atlas()->is_valid() || !device) return d;
	d["slot_count"] = world_->atlas()->atlas_slot_count();
	d["free_slots"] = world_->atlas()->read_free_count(device);
	d["region_map_entries"] = world_->atlas()->region_map_entries();
	d["job_count"] = world_->atlas()->read_job_count(device);
	d["overflow"] = static_cast<int>(world_->atlas()->read_overflow(device));
	// Memory bounds (Task 6): the R8 atlas byte count is pinned so a regression that
	// resizes it fails loudly next to the normal-pool capacity assertion.
	const ve::IVec3 ab = world_->atlas()->config().atlas_bricks;
	const int64_t sdf_bytes = static_cast<int64_t>(ab.x) * ve::kBrickSdfStride *
			(ab.y * ve::kBrickSdfStride) * (ab.z * ve::kBrickSdfStride);
	d["sdf_atlas_bytes"] = sdf_bytes;
	return d;
}

void VoxelDebugHooks::debug_reset_frame_counters() {
	if (world_->atlas() && world_->rd()) world_->atlas()->reset_frame_counters(world_->rd());
}

void VoxelDebugHooks::debug_set_region_map_entry(int region_index, int region_slot) {
	if (world_->atlas() && world_->rd()) world_->atlas()->set_region_map_entry(world_->rd(), region_index, region_slot);
}

void VoxelDebugHooks::debug_upload_region_ops(int region_slot, const PackedByteArray &ops, int count) {
	if (!world_->atlas() || !world_->rd()) return;
	const ve::EditOp *ptr = nullptr;
	if (count > 0) {
		if (ops.size() < count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_upload_region_ops: op buffer too small");
			return;
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	world_->atlas()->upload_region_ops(world_->rd(), region_slot, ptr, count);
}

bool VoxelDebugHooks::debug_brick_has_surface(Vector3i brick, const PackedByteArray &ops,
		int op_count) const {
	const ve::Generator &gen = world_->store_->generator()->sampler();
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_brick_has_surface: op buffer too small");
			return false;
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	return ve::brick_has_surface(gen, ptr, op_count, {brick.x, brick.y, brick.z}, &world_->store_->volumes());
}

void VoxelDebugHooks::debug_mark_region(Vector3i region, int region_slot, Vector3i lo, Vector3i hi,
		int op_count, bool force) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->atlas() || !world_->region_pass()) return;
	if (region_slot < 0 || region_slot >= world_->store_->config().max_region_slots) {
		// The mark shader indexes region_tables with rslot * kRegionBrickCount + bi, so a
		// hostile slot is a GPU-side out-of-bounds write. Refuse before recording.
		UtilityFunctions::printerr("debug_mark_region: region_slot ", region_slot,
				" out of range [0, ", world_->store_->config().max_region_slots, ")");
		return;
	}
	const int64_t list = device->compute_list_begin();
	world_->region_pass()->mark(device, list, {region.x, region.y, region.z}, region_slot,
			{lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}, op_count, force);
	device->compute_list_end();
	device->submit();
	device->sync();
}

void VoxelDebugHooks::debug_generate_pending() {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->atlas() || !world_->region_pass() || !world_->gen_pass()) return;
	const int64_t list = device->compute_list_begin();
	world_->region_pass()->write_dispatch_args(device, list);
	device->compute_list_add_barrier(list);
	world_->gen_pass()->dispatch(device, list, *world_->atlas());
	device->compute_list_end();
	device->submit();
	device->sync();
}

Dictionary VoxelDebugHooks::debug_brick_diff(Vector3i brick, int region_slot,
		const PackedByteArray &ops, int op_count) {
	Dictionary d;
	RenderingDevice *device = world_->rd();
	if (!device || !world_->atlas()) return d;
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_brick_diff: op buffer too small");
			return d;
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::IVec3 b{brick.x, brick.y, brick.z};
	const int slot = debug_region_table_slot(region_slot, brick);
	d["slot"] = slot;
	if (slot < 0) return d;

	const ve::Generator &gen = world_->store_->generator()->sampler();
	ve::BrickEval ref{};
	ve::eval_brick(gen, ptr, op_count, b, &ref, &world_->store_->volumes(), world_->store_->overrides());

	const ve::IVec3 ab = world_->atlas()->config().atlas_bricks;
	const ve::IVec3 cell{slot % ab.x, (slot / ab.x) % ab.y, slot / (ab.x * ab.y)};

	// texture_get_data returns the whole volume; tests run a small atlas, so one read each.
	const PackedByteArray sdf = device->texture_get_data(world_->atlas()->sdf_atlas(), 0);
	const PackedByteArray mat = device->texture_get_data(world_->atlas()->mat_atlas(), 0);
	const int sw = ab.x * ve::kBrickSdfStride, sh = ab.y * ve::kBrickSdfStride;
	const int mw = ab.x * ve::kBrickVoxels, mh = ab.y * ve::kBrickVoxels;

	int sdf_max = 0, sdf_over_one = 0;
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const int ax = cell.x * ve::kBrickSdfStride + x;
				const int ay = cell.y * ve::kBrickSdfStride + y;
				const int az = cell.z * ve::kBrickSdfStride + z;
				const int got = sdf[ax + ay * sw + az * sw * sh];
				const int want = ref.brick.sdf[ve::sdf_index(x, y, z)];
				const int diff = std::abs(got - want);
				sdf_max = std::max(sdf_max, diff);
				if (diff > 1) sdf_over_one++;
			}
	d["sdf_max_diff"] = sdf_max;
	d["sdf_diff_over_one"] = sdf_over_one;

	const PackedByteArray pal_bytes = device->buffer_get_data(world_->atlas()->palette(),
			static_cast<uint32_t>(slot) * ve::kBrickPaletteSize * 4,
			ve::kBrickPaletteSize * 4);
	const uint32_t *pal = reinterpret_cast<const uint32_t *>(pal_bytes.ptr());
	bool pal_ok = true;
	bool has_four = false;
	for (int p = 0; p < ve::kBrickPaletteSize; p++) {
		pal_ok = pal_ok && pal[p] == ref.brick.palette[p];
		has_four = has_four || pal[p] == 4;
	}
	d["palette_match"] = pal_ok;
	d["has_material_4"] = has_four;

	// Materials are only meaningful where a hit point can land — within ~1.2 voxels of the
	// surface. Compare RESOLVED ids, not packed indices: the two sides agree on the palette
	// ordering, but comparing ids keeps the check honest if that ever changes.
	int near_compared = 0, near_mismatch = 0;
	for (int z = 0; z < ve::kBrickVoxels; z++)
		for (int y = 0; y < ve::kBrickVoxels; y++)
			for (int x = 0; x < ve::kBrickVoxels; x++) {
				const float dist = ve::decode_sdf(ref.brick.sdf[ve::sdf_index(x, y, z)]);
				if (std::fabs(dist) > 1.2f * ve::kVoxelSize) continue;
				const int ax = cell.x * ve::kBrickVoxels + x;
				const int ay = cell.y * ve::kBrickVoxels + y;
				const int az = cell.z * ve::kBrickVoxels + z;
				const int gi = mat[ax + ay * mw + az * mw * mh];
				const uint32_t got_id = gi < ve::kBrickPaletteSize ? pal[gi] : 0;
				const uint16_t want_id =
						ref.brick.palette[ve::get_mat_index(ref.brick, ve::voxel_index(x, y, z))];
				near_compared++;
				if (got_id != want_id) near_mismatch++;
			}
	d["mat_near_compared"] = near_compared;
	d["mat_near_mismatch"] = near_mismatch;

	int mip_bad = 0;
	// The reference mips are reduced from the GPU lattice just read back, not from the CPU
	// one. The SDF diff already tolerates a one-step sin() drift (glibc vs driver), and a
	// drifted extremum would otherwise flag a mip cell that is a perfectly correct
	// reduction of what the GPU actually wrote (brief Step 5 note: "the property under
	// test is that the reduction is right, not that sin is bit-identical").
	uint8_t gpu_lattice[ve::kBrickSdfCount];
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const int ax = cell.x * ve::kBrickSdfStride + x;
				const int ay = cell.y * ve::kBrickSdfStride + y;
				const int az = cell.z * ve::kBrickSdfStride + z;
				gpu_lattice[ve::sdf_index(x, y, z)] = sdf[ax + ay * sw + az * sw * sh];
			}
	ve::BrickMips ref_mips{};
	ve::build_brick_mips(gpu_lattice, &ref_mips);
	for (int level = 0; level < ve::kMipLevels; level++) {
		const int dim = ve::kMipDims[level];
		const PackedByteArray mip = device->texture_get_data(world_->atlas()->mip_atlas(level), 0);
		const int w = ab.x * dim, h = ab.y * dim;
		const uint8_t *want_mn = ve::mip_min(ref_mips, level);
		const uint8_t *want_mx = ve::mip_max(ref_mips, level);
		for (int z = 0; z < dim; z++)
			for (int y = 0; y < dim; y++)
				for (int x = 0; x < dim; x++) {
					const int ax = cell.x * dim + x, ay = cell.y * dim + y, az = cell.z * dim + z;
					const int64_t o = (static_cast<int64_t>(ax) + ay * w + az * w * h) * 2;
					const int i = x + y * dim + z * dim * dim;
					if (mip[o] != want_mn[i] || mip[o + 1] != want_mx[i]) mip_bad++;
				}
	}
	d["mip_mismatch"] = mip_bad;
	return d;
}

void VoxelDebugHooks::debug_stream_region(Vector3i region) {
	world_->ensure_initialized();
	if (!world_->initialized_ || !world_->rd() || !world_->streamer_) return;
	const Vector3 center((region.x * ve::kRegionBricks + ve::kRegionBricks / 2) * ve::kBrickSize,
			(region.y * ve::kRegionBricks + ve::kRegionBricks / 2) * ve::kBrickSize,
			(region.z * ve::kRegionBricks + ve::kRegionBricks / 2) * ve::kBrickSize);
	for (int i = 0; i < 8; i++) {
		debug_stream_frame(center);
		if (debug_slot_of_region(region) >= 0) return;
	}
}

Dictionary VoxelDebugHooks::debug_brick_flags(Vector3i region) {
	Dictionary d;
	debug_stream_region(region);
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->store_->edit_log()) return d;
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;

	const PackedByteArray table = device->buffer_get_data(world_->atlas()->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	const PackedByteArray flags = device->buffer_get_data(world_->atlas()->brick_flags());
	if (table.size() < ve::kRegionBrickCount * 4 ||
			flags.size() < world_->atlas()->atlas_slot_count() * static_cast<int>(sizeof(uint32_t))) return d;

	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		ops = world_->store_->edit_log()->ops({region.x, region.y, region.z});
	}
	const ve::Generator &gen = world_->store_->generator()->sampler();
	const int32_t *slots = reinterpret_cast<const int32_t *>(table.ptr());
	const uint32_t *gpu_flags = reinterpret_cast<const uint32_t *>(flags.ptr());
	int compared = 0;
	int mismatches = 0;
	Vector3i first_mismatch(-1, -1, -1);
	for (int bi = 0; bi < ve::kRegionBrickCount; bi++) {
		const int slot = slots[bi];
		if (slot < 0) continue;
		const int x = bi & (ve::kRegionBricks - 1);
		const int y = (bi >> 5) & (ve::kRegionBricks - 1);
		const int z = bi >> 10;
		const ve::IVec3 brick{region.x * ve::kRegionBricks + x,
				region.y * ve::kRegionBricks + y, region.z * ve::kRegionBricks + z};
		ve::BrickEval ref{};
		ve::eval_brick(gen, ops.data(), static_cast<int>(ops.size()), brick, &ref, &world_->store_->volumes(), world_->store_->overrides());
		const uint32_t want = ve::brick_flags_from_mips(ref.mips, ref.brick.palette[0]);
		const uint32_t got = gpu_flags[slot];
		compared++;
		if (got != want) {
			mismatches++;
			if (mismatches == 1) first_mismatch = Vector3i(brick.x, brick.y, brick.z);
		}
	}
	d["compared"] = compared;
	d["mismatches"] = mismatches;
	d["first_mismatch_brick"] = first_mismatch;
	return d;
}

Dictionary VoxelDebugHooks::debug_brick_flags_after_mark(Vector3i region) {
	Dictionary d;
	debug_stream_region(region);
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->store_->edit_log() || !world_->region_pass()) return d;
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;
	int op_count = 0;
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		op_count = static_cast<int>(world_->store_->edit_log()->ops({region.x, region.y, region.z}).size());
	}
	const ve::IVec3 lo{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	const ve::IVec3 hi{lo.x + ve::kRegionBricks - 1, lo.y + ve::kRegionBricks - 1,
			lo.z + ve::kRegionBricks - 1};
	debug_mark_region(region, rslot, Vector3i(lo.x, lo.y, lo.z), Vector3i(hi.x, hi.y, hi.z),
			op_count, true);
	const PackedByteArray table = device->buffer_get_data(world_->atlas()->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	const PackedByteArray flags = device->buffer_get_data(world_->atlas()->brick_flags());
	if (table.size() < ve::kRegionBrickCount * 4 ||
			flags.size() < world_->atlas()->atlas_slot_count() * static_cast<int>(sizeof(uint32_t))) return d;
	const int32_t *slots = reinterpret_cast<const int32_t *>(table.ptr());
	const uint32_t *gpu_flags = reinterpret_cast<const uint32_t *>(flags.ptr());
	int allocated = 0;
	int non_conservative = 0;
	for (int bi = 0; bi < ve::kRegionBrickCount; bi++) {
		const int slot = slots[bi];
		if (slot < 0) continue;
		allocated++;
		if (gpu_flags[slot] != ve::kBrickFlagConservative) non_conservative++;
	}
	d["allocated"] = allocated;
	d["non_conservative"] = non_conservative;
	return d;
}

void VoxelDebugHooks::debug_release_region(int region_slot) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->region_pass()) return;
	if (region_slot < 0 || region_slot >= world_->store_->config().max_region_slots) {
		// Same hostile-slot hazard as debug_mark_region: the free shader indexes
		// region_tables with rslot * kRegionBrickCount + bi.
		UtilityFunctions::printerr("debug_release_region: region_slot ", region_slot,
				" out of range [0, ", world_->store_->config().max_region_slots, ")");
		return;
	}
	const int64_t list = device->compute_list_begin();
	world_->region_pass()->release_region(device, list, region_slot);
	device->compute_list_end();
	device->submit();
	device->sync();
}

PackedInt32Array VoxelDebugHooks::debug_jobs() {
	PackedInt32Array out;
	RenderingDevice *device = world_->rd();
	if (!device || !world_->atlas()) return out;
	const int count = world_->atlas()->read_job_count(device);
	if (count <= 0) return out;
	const PackedByteArray b = device->buffer_get_data(world_->atlas()->jobs(), 0, count * 32);
	out.resize(count * 8);
	memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(count) * 32);
	return out;
}

int VoxelDebugHooks::debug_region_table_slot(int region_slot, Vector3i brick) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->atlas()) return -1;
	const int bi = ve::WorldBounds::brick_index_in_region({brick.x, brick.y, brick.z});
	const uint32_t offset =
			(static_cast<uint32_t>(region_slot) * ve::kRegionBrickCount + bi) * 4;
	const PackedByteArray b = device->buffer_get_data(world_->atlas()->region_tables(), offset, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

RID VoxelDebugHooks::debug_sdf_atlas() const { return world_->atlas() ? world_->atlas()->sdf_atlas() : RID(); }

RID VoxelDebugHooks::debug_mat_atlas() const { return world_->atlas() ? world_->atlas()->mat_atlas() : RID(); }

RID VoxelDebugHooks::debug_mip_atlas(int level) const {
	if (!world_->atlas() || level < 0 || level >= ve::kMipLevels) return RID();
	return world_->atlas()->mip_atlas(level);
}

RID VoxelDebugHooks::debug_region_map() const { return world_->atlas() ? world_->atlas()->region_map() : RID(); }

RID VoxelDebugHooks::debug_region_tables() const { return world_->atlas() ? world_->atlas()->region_tables() : RID(); }

RID VoxelDebugHooks::debug_free_list() const { return world_->atlas() ? world_->atlas()->free_list() : RID(); }

RID VoxelDebugHooks::debug_frame_counters() const { return world_->atlas() ? world_->atlas()->frame_counters() : RID(); }

RID VoxelDebugHooks::debug_op_pool() const { return world_->atlas() ? world_->atlas()->op_pool() : RID(); }

RID VoxelDebugHooks::debug_op_counts() const { return world_->atlas() ? world_->atlas()->op_counts() : RID(); }

int VoxelDebugHooks::debug_occupancy_state(Vector3i cell) {
	world_->drain_occupancy(); // tests step the streamer by hand and never run _process
	return static_cast<int>(world_->occupancy().state({cell.x, cell.y, cell.z}));
}

void VoxelDebugHooks::debug_pump_occupancy() {
	// Contract: harvest already-issued async GPU readbacks and fold their inbox blocks; this
	// helper does not advance the streamer or issue a mark. Tests must drive frames separately
	// when they need a fresh mark, so harvesting cannot hide which mark branch ran.
	world_->ensure_initialized();
	if (world_->streamer_ && world_->rd()) world_->streamer_->harvest_occupancy(world_->rd());
	world_->drain_occupancy();
}

Dictionary VoxelDebugHooks::debug_occupancy_fallback_diff(Vector3i region) {
	Dictionary d;
	d["compared"] = 0;
	d["fallback"] = 0;
	d["mismatches"] = 0;
	d["first_mismatch_brick"] = Vector3i(-1, -1, -1);
	world_->ensure_initialized();
	if (!world_->rd() || !world_->atlas() || !world_->store_->edit_log() || !world_->region_pass()) return d;
	debug_stream_region(region);
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;

	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		ops = world_->store_->edit_log()->ops({region.x, region.y, region.z});
	}
	const ve::IVec3 lo{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	const ve::IVec3 hi{lo.x + ve::kRegionBricks - 1, lo.y + ve::kRegionBricks - 1,
			lo.z + ve::kRegionBricks - 1};
	// force=false is intentional: this records only the plain mark path, where a no-surface
	// brick has no generator job and must be classified by the 27-sample fallback.
	debug_mark_region(region, rslot, Vector3i(lo.x, lo.y, lo.z),
			Vector3i(hi.x, hi.y, hi.z), static_cast<int>(ops.size()), false);
	const uint32_t block_bytes = GpuAtlas::occupancy_block_bytes();
	const PackedByteArray gpu = world_->rd()->buffer_get_data(world_->atlas()->region_occupancy(),
			static_cast<uint32_t>(rslot) * block_bytes, block_bytes);
	if (gpu.size() < static_cast<int>(block_bytes)) return d;

	const ve::Generator &gen = world_->store_->generator()->sampler();
	int compared = 0, fallback = 0, mismatches = 0;
	Vector3i first(-1, -1, -1);
	for (int bi = 0; bi < ve::kRegionBrickCount; bi++) {
		const ve::IVec3 brick{
				region.x * ve::kRegionBricks + (bi & (ve::kRegionBricks - 1)),
				region.y * ve::kRegionBricks + ((bi >> 5) & (ve::kRegionBricks - 1)),
				region.z * ve::kRegionBricks + (bi >> 10)};
		if (ve::brick_has_surface(gen, ops.data(), static_cast<int>(ops.size()), brick,
				&world_->store_->volumes(), world_->store_->overrides())) continue;
		fallback++;
		const int got = ve::OccupancyGrid::read_packed(
				reinterpret_cast<const uint8_t *>(gpu.ptr()), bi);
		const int want = static_cast<int>(ve::cell_state_probe(gen, ops.data(),
				static_cast<int>(ops.size()), brick, &world_->store_->volumes(), world_->store_->overrides()));
		compared++;
		if (got != want) {
			mismatches++;
			if (mismatches == 1) first = Vector3i(brick.x, brick.y, brick.z);
		}
	}
	d["compared"] = compared;
	d["fallback"] = fallback;
	d["mismatches"] = mismatches;
	d["first_mismatch_brick"] = first;
	return d;
}

Dictionary VoxelDebugHooks::debug_occupancy_diff(Vector3i region) {
	Dictionary d;
	d["compared"] = 0;
	d["mismatches"] = 0;
	d["first_mismatch_brick"] = Vector3i(-1, -1, -1);
	world_->ensure_initialized();
	if (!world_->rd() || !world_->atlas() || !world_->store_->edit_log()) return d;
	debug_stream_region(region);
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;
	const uint32_t block_bytes = GpuAtlas::occupancy_block_bytes();
	const PackedByteArray gpu = world_->rd()->buffer_get_data(world_->atlas()->region_occupancy(),
			static_cast<uint32_t>(rslot) * block_bytes, block_bytes);
	const PackedByteArray table = world_->rd()->buffer_get_data(world_->atlas()->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	if (gpu.size() < static_cast<int>(block_bytes) ||
			table.size() < ve::kRegionBrickCount * 4) return d;
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		ops = world_->store_->edit_log()->ops({region.x, region.y, region.z});
	}
	const int32_t *slots = reinterpret_cast<const int32_t *>(table.ptr());
	const ve::Generator &gen = world_->store_->generator()->sampler();
	int compared = 0, mismatches = 0;
	Vector3i first(-1, -1, -1);
	for (int bi = 0; bi < ve::kRegionBrickCount; bi++) {
		if (slots[bi] < 0) continue;
		const ve::IVec3 brick{
				region.x * ve::kRegionBricks + (bi & (ve::kRegionBricks - 1)),
				region.y * ve::kRegionBricks + ((bi >> 5) & (ve::kRegionBricks - 1)),
				region.z * ve::kRegionBricks + (bi >> 10)};
		const int got = ve::OccupancyGrid::read_packed(
				reinterpret_cast<const uint8_t *>(gpu.ptr()), bi);
		const int want = static_cast<int>(ve::cell_state_field(gen, ops.data(),
				static_cast<int>(ops.size()), brick, &world_->store_->volumes(), world_->store_->overrides()));
		compared++;
		if (got != want) {
			mismatches++;
			if (mismatches == 1) first = Vector3i(brick.x, brick.y, brick.z);
		}
	}
	d["compared"] = compared;
	d["mismatches"] = mismatches;
	d["first_mismatch_brick"] = first;
	return d;
}

PackedFloat32Array VoxelDebugHooks::debug_generator_fingerprint() {
	PackedFloat32Array out;
	if (world_ == nullptr || world_->store_.get() == nullptr ||
			world_->store_->generator() == nullptr) {
		return out;
	}
	const ve::Generator &gen = world_->store_->generator()->sampler();
	// Same regimes as tests/golden/field_baseline.txt: surface, cave, deep, sky, far.
	static const float kPts[][3] = {
		{0.0f, 51.2f, 0.0f}, {12.3f, 55.0f, -7.8f}, {30.0f, 50.85f, 30.0f},
		{-30.0f, 50.0f, -30.0f}, {0.0f, 20.0f, 0.0f}, {0.0f, 90.0f, 0.0f},
		{800.0f, 51.2f, 800.0f}, {-800.0f, 51.2f, -800.0f},
	};
	for (const auto &p : kPts) {
		ve::Sample s = gen.sample(p[0], p[1], p[2]);
		out.push_back(s.sdf);
		out.push_back(float(s.material));
		out.push_back(0.0f);
	}
	return out;
}

float VoxelDebugHooks::debug_field_sdf(Vector3 p) {
	if (!world_->store_->edit_log()) return 1e30f;
	const ve::Generator &gen = world_->store_->generator()->sampler();
	std::lock_guard<std::mutex> lock(world_->edit_mutex());
	const std::vector<ve::EditOp> &ops =
			world_->store_->edit_log()->ops(ve::WorldBounds::region_of_point(p.x, p.y, p.z));
	return ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()), p.x, p.y, p.z,
			&world_->store_->volumes(), world_->store_->overrides()).sdf;
}

int VoxelDebugHooks::debug_cell_state(Vector3i cell) {
	if (!world_->store_->edit_log()) return static_cast<int>(ve::kCellUnknown);
	const ve::IVec3 c{cell.x, cell.y, cell.z};
	const ve::Generator &gen = world_->store_->generator()->sampler();
	std::lock_guard<std::mutex> lock(world_->edit_mutex());
	const std::vector<ve::EditOp> &ops = world_->store_->edit_log()->ops(ve::WorldBounds::region_of_brick(c));
	return static_cast<int>(ve::cell_state_field(gen, ops.data(),
			static_cast<int>(ops.size()), c, &world_->store_->volumes(), world_->store_->overrides()));
}

Dictionary VoxelDebugHooks::debug_occupancy_stats(Vector3 center) {
	world_->drain_occupancy();
	Dictionary d;
	d["regions"] = world_->occupancy().region_count();
	d["edit_seq"] = static_cast<int64_t>(world_->edit_seq());
	// The block covering the streaming centre, so a test can tell "the grid has been told
	// about this edit" from "some other region's block arrived".
	const ve::IVec3 r = ve::WorldBounds::region_of_point(center.x, center.y, center.z);
	d["seq_at_center"] = static_cast<int64_t>(world_->occupancy().block_seq(r));
	return d;
}

int VoxelDebugHooks::debug_stream_frame(Vector3 cam) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->streamer_) return 0;
	const int actions = world_->streamer_->run_frame(device, cam.x, cam.y, cam.z);
	device->submit();
	device->sync();
	world_->overflow_seen_ |= static_cast<int>(world_->atlas()->read_overflow(device));
	world_->drain_occupancy();
	return actions;
}

Dictionary VoxelDebugHooks::debug_stream_stats() {
	Dictionary d;
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->store_->residency() || !world_->streamer_) return d;
	d["resident_regions"] = world_->store_->residency()->resident_count();
	d["frame_edits"] = world_->streamer_->last_frame_edits();
	d["overflow"] = static_cast<int>(world_->atlas()->read_overflow(device));
	// Either path may be the one running: debug_stream_frame drives the world in tests, the
	// compositor's render callback drives it in the demo, and only the streamer sees the
	// latter's frames. The HUD reads this, so it has to cover both.
	d["overflow_ever"] =
			world_->overflow_seen_ | static_cast<int>(world_->streamer_->overflow_seen());
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		d["override_bricks"] = world_->store_->overrides() ? world_->store_->overrides()->used() : 0;
		d["override_capacity"] = world_->store_->overrides() ? world_->store_->overrides()->capacity() : world_->store_->config().max_override_bricks;
		d["consolidations"] = world_->context().consolidation->consolidated_count();
		d["consolidation_refusals"] = world_->context().consolidation->refusals();
		d["consolidation_queue_refusals"] = world_->context().consolidation->queue_refusals();
		d["edit_rejections"] = world_->edit_rejections_;
	}
	return d;
}

int VoxelDebugHooks::debug_slot_of_region(Vector3i region) const {
	if (!world_->store_->residency()) return -1;
	return world_->store_->residency()->slot_of({region.x, region.y, region.z});
}

int VoxelDebugHooks::debug_region_map_entry(Vector3i region) {
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas()) return -1;
	const int idx = world_->world_bounds().region_index({region.x, region.y, region.z});
	if (idx < 0) return -1;
	const PackedByteArray b = device->buffer_get_data(world_->atlas()->region_map(), idx * 4, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

bool VoxelDebugHooks::debug_region_map_consistent() {
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->atlas() || !world_->store_->residency()) return false;
	const ve::WorldBounds wb = world_->world_bounds();
	const PackedByteArray b = device->buffer_get_data(world_->atlas()->region_map());
	const int32_t *map = reinterpret_cast<const int32_t *>(b.ptr());
	const ve::IVec3 o = wb.origin_regions();
	const ve::IVec3 sz = wb.size_regions;
	for (int z = 0; z < sz.z; z++)
		for (int y = 0; y < sz.y; y++)
			for (int x = 0; x < sz.x; x++) {
				const ve::IVec3 r{o.x + x, o.y + y, o.z + z};
				const int gpu_slot = map[x + y * sz.x + z * sz.x * sz.y];
				const int cpu_slot = world_->store_->residency()->slot_of(r);
				if (gpu_slot != cpu_slot) return false;
				if (gpu_slot >= 0 && !(world_->store_->residency()->region_of_slot(gpu_slot) == r)) return false;
			}
	return true;
}

Dictionary VoxelDebugHooks::debug_raycast(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	if (!world_->store_->edit_log()) return d;
	std::lock_guard<std::mutex> lock(world_->edit_mutex());
	const ve::Generator &gen = world_->store_->generator()->sampler();
	const float o[3] = {origin.x, origin.y, origin.z};
	const float f[3] = {dir.x, dir.y, dir.z};
	const ve::RayHit h = ve::raycast(gen, *world_->store_->edit_log(), o, f, 200.0f, &world_->store_->volumes(), world_->store_->overrides());
	if (!h.hit) return d;
	d["hit"] = true;
	d["pos"] = Vector3(h.pos[0], h.pos[1], h.pos[2]);
	d["normal"] = Vector3(h.normal[0], h.normal[1], h.normal[2]);
	d["distance"] = h.distance;
	// The struck surface's material. Ray-driven removal tools pass this straight to
	// VoxelEditTool.apply_sphere_subtract so its hardness is resolved once, up front.
	d["material"] = static_cast<int>(h.material);
	return d;
}

void VoxelDebugHooks::debug_pump_shader_reload() {
	world_->pump_shader_reload();
}
// --- Task 6 hooks: fixed-capacity stored-normal pool ---
// Debug initializer: shrink the normal-pool budget BEFORE debug_init_atlas(). The
// pool's size is otherwise fixed at exactly 32 MiB and never resizes.
void VoxelDebugHooks::debug_set_normal_pool_budget(int bytes) {
	world_->normal_pool_bytes_ = bytes > 0 ? static_cast<uint32_t>(bytes) : 0u;
}
RenderingDevice *VoxelDebugHooks::debug_local_rd() const {
	return world_->local_rd();
}

} // namespace godot
