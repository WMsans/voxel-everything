#include "debug/hooks.h"

#include "../voxel_world.h"
#include "render/frame.h"
#include "render/frame_params.h"
#include "render/orchestrator.h"
#include "terrain/field_params_pack.h"
#include <cstring>
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
#include "render/field_context_set.h"
#include "render/lod_build_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "render/sun_shadow_pass.h"
#include "render/sun_ubo.h"
#include "render/lod_cull_pass.h"
#include "render/grass_scatter_pass.h"
#include "render/grass_raster_pass.h"
#include "grass/grass_layout.h"
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
#include "shade/sun_cascades.h"
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

#include <algorithm>
#include <vector>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>

#include "debug/hooks_common.h"

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
	ClassDB::bind_method(D_METHOD("debug_ssgi_history_latch_probe", "w", "h", "w2", "h2"),
			&VoxelDebugHooks::debug_ssgi_history_latch_probe);
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
	ClassDB::bind_method(D_METHOD("debug_teardown_trace"), &VoxelDebugHooks::debug_teardown_trace);
	ClassDB::bind_method(D_METHOD("debug_gbuffer_stats", "w", "h"),
			&VoxelDebugHooks::debug_gbuffer_stats);
	ClassDB::bind_method(D_METHOD("debug_hiz_probe_synthetic", "far_value", "near_value"),
			&VoxelDebugHooks::debug_hiz_probe_synthetic);
	ClassDB::bind_method(D_METHOD("debug_hiz_occluded", "lo", "hi", "depth"),
			&VoxelDebugHooks::debug_hiz_occluded);
	ClassDB::bind_method(D_METHOD("debug_lod_cull_probe", "pos", "fwd"),
			&VoxelDebugHooks::debug_lod_cull_probe);
	ClassDB::bind_method(D_METHOD("debug_lod_cull_debug"), &VoxelDebugHooks::debug_lod_cull_debug);
	ClassDB::bind_method(D_METHOD("debug_render_frame", "pos", "fwd", "w", "h"),
			&VoxelDebugHooks::debug_render_frame);
	ClassDB::bind_method(D_METHOD("debug_grass_stats"), &VoxelDebugHooks::debug_grass_stats);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_stats", "cascade"),
			&VoxelDebugHooks::debug_sun_shadow_stats);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_build", "cascade", "force"),
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
	ClassDB::bind_method(D_METHOD("debug_edit_fanout"), &VoxelDebugHooks::debug_edit_fanout);
	ClassDB::bind_method(D_METHOD("debug_drain_invalidations"), &VoxelDebugHooks::debug_drain_invalidations);
	ClassDB::bind_method(D_METHOD("debug_override_region_table", "region_slot"),
			&VoxelDebugHooks::debug_override_region_table);
	ClassDB::bind_method(D_METHOD("debug_override_used"), &VoxelDebugHooks::debug_override_used);
	ClassDB::bind_method(D_METHOD("debug_fill_override_pool", "region"), &VoxelDebugHooks::debug_fill_override_pool);
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
	ClassDB::bind_method(D_METHOD("debug_contact_samples", "cell", "axis"), &VoxelDebugHooks::debug_contact_samples);
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
	ClassDB::bind_method(D_METHOD("debug_hold_consolidation", "held"), &VoxelDebugHooks::debug_hold_consolidation);
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
	ClassDB::bind_method(D_METHOD("debug_request_shader_reload"),
			&VoxelDebugHooks::debug_request_shader_reload);
	ClassDB::bind_method(D_METHOD("debug_clear_shader_source_overrides"),
			&VoxelDebugHooks::debug_clear_shader_source_overrides);
	ClassDB::bind_method(D_METHOD("debug_field_params_bytes"),
			&VoxelDebugHooks::debug_field_params_bytes);
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

Dictionary VoxelDebugHooks::debug_render_frame(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["ok"] = false;
	d["had_history"] = false;
	d["mean_luma"] = 0.0;
	d["lit_checksum"] = 0;
	if (w <= 0 || h <= 0 || !world_->get_use_local_device()) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer) return d;
	d["had_history"] = false;
	FrameInputs in = world_->context().render->frame().prepare_headless(device, VoxelFrame::looking_at(pos, fwd, w, h));
	if (!in.scene_color.is_valid()) return d;
	const bool pre = world_->context().render->frame().render_pre_opaque(device, in);
	d["had_history"] = world_->context().render->has_history();
	const bool post = world_->context().render->frame().render_post_opaque(device, in);
	const bool ok = pre && post;
	device->submit();
	device->sync();
	d["ok"] = ok;
	write_frame_record(d, world_->context().render->frame().last_frame());
	if (world_->context().render->passes().gbuffer->size() != Vector2i(w, h)) return d;
	const PackedByteArray lit = device->texture_get_data(world_->context().render->passes().gbuffer->lit(), 0);
	const int64_t pixels = static_cast<int64_t>(w) * h;
	if (lit.size() < pixels * 8) return d;
	const uint16_t *v = reinterpret_cast<const uint16_t *>(lit.ptr());
	double luma = 0.0;
	int64_t checksum = 0;
	for (int64_t i = 0; i < pixels; i++) {
		luma += 0.2126 * half_to_float(v[i * 4]) + 0.7152 * half_to_float(v[i * 4 + 1]) +
				0.0722 * half_to_float(v[i * 4 + 2]);
		checksum = checksum * 31 + v[i * 4] + 7 * v[i * 4 + 1] + 13 * v[i * 4 + 2];
	}
	d["mean_luma"] = luma / static_cast<double>(pixels);
	d["lit_checksum"] = checksum;
	return d;
}

} // namespace godot
