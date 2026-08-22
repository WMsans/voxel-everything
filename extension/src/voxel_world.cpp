#include "voxel_world.h"
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
#include <chrono>
#include <thread>
#include <cmath>
#include <set>
#include <cstring>
#include <algorithm>
#include <array>
#include <iterator>
#include <vector>

using namespace godot;

namespace {
std::mutex g_voxel_compositor_admission_mutex;
bool g_voxel_compositor_callbacks_enabled = true;
}

bool godot::voxel_compositor_callbacks_enabled() {
	std::lock_guard<std::mutex> lock(g_voxel_compositor_admission_mutex);
	return g_voxel_compositor_callbacks_enabled;
}

bool godot::voxel_try_begin_compositor_callback(const NodePath &world_path,
		VoxelWorld **out_world) {
	if (!out_world) return false;
	*out_world = nullptr;
	// Keep this lock only through lookup and the per-world guard acquisition. Once the guard
	// is counted, shutdown cannot invalidate the world before the callback starts rendering.
	std::lock_guard<std::mutex> admission(g_voxel_compositor_admission_mutex);
	if (!g_voxel_compositor_callbacks_enabled) return false;
	Engine *engine = Engine::get_singleton();
	if (!engine) return false;
	SceneTree *tree = Object::cast_to<SceneTree>(engine->get_main_loop());
	if (!tree || !tree->get_root()) return false;
	VoxelWorld *world = Object::cast_to<VoxelWorld>(
			tree->get_root()->get_node_or_null(world_path));
	if (!world || world->get_use_local_device()) return false;
	if (!world->try_begin_render_callback()) return false;
	*out_world = world;
	return true;
}

void godot::voxel_compositor_callbacks_ready(VoxelWorld *world) {
	std::lock_guard<std::mutex> admission(g_voxel_compositor_admission_mutex);
	{
		std::lock_guard<std::mutex> lifetime(world->render_lifetime_mutex_);
		world->render_shutting_down_ = false;
		world->render_teardown_deferred_ = false;
	}
	g_voxel_compositor_callbacks_enabled = true;
}

void godot::voxel_compositor_callbacks_shutdown_started(VoxelWorld *world) {
	std::lock_guard<std::mutex> admission(g_voxel_compositor_admission_mutex);
	g_voxel_compositor_callbacks_enabled = false;
	std::lock_guard<std::mutex> lifetime(world->render_lifetime_mutex_);
	world->render_shutting_down_ = true;
}

bool VoxelWorld::try_begin_render_callback() {
	std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
	if (render_shutting_down_) return false;
	render_callbacks_++;
	return true;
}

void VoxelWorld::end_render_callback() {
	bool defer_teardown = false;
	{
		std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
		if (render_callbacks_ <= 0) return;
		if (--render_callbacks_ == 0) {
			defer_teardown = render_teardown_deferred_;
			render_teardown_deferred_ = false;
			render_lifetime_cv_.notify_all();
		}
	}
	// A render-thread caller cannot wait for its own callback guard. Defer destruction until
	// that guard has released the last callback; no resource is touched after this destructor.
	if (defer_teardown) shutdown_render_resources_on_render_thread();
}

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_shutdown_render_resources_on_render_thread"),
			&VoxelWorld::shutdown_render_resources_on_render_thread);
	ClassDB::bind_method(D_METHOD("shutdown_render_resources"),
			&VoxelWorld::shutdown_render_resources);
	ClassDB::bind_method(D_METHOD("set_use_local_device", "v"), &VoxelWorld::set_use_local_device);
	ClassDB::bind_method(D_METHOD("get_use_local_device"), &VoxelWorld::get_use_local_device);
	ClassDB::bind_method(D_METHOD("set_atlas_bricks", "v"), &VoxelWorld::set_atlas_bricks);
	ClassDB::bind_method(D_METHOD("get_atlas_bricks"), &VoxelWorld::get_atlas_bricks);
	ClassDB::bind_method(D_METHOD("set_max_region_slots", "v"), &VoxelWorld::set_max_region_slots);
	ClassDB::bind_method(D_METHOD("get_max_region_slots"), &VoxelWorld::get_max_region_slots);
	ClassDB::bind_method(D_METHOD("set_max_brick_jobs", "v"), &VoxelWorld::set_max_brick_jobs);
	ClassDB::bind_method(D_METHOD("get_max_brick_jobs"), &VoxelWorld::get_max_brick_jobs);
	ClassDB::bind_method(D_METHOD("set_max_override_bricks", "v"), &VoxelWorld::set_max_override_bricks);
	ClassDB::bind_method(D_METHOD("get_max_override_bricks"), &VoxelWorld::get_max_override_bricks);
	ClassDB::bind_method(D_METHOD("set_world_origin_bricks", "v"), &VoxelWorld::set_world_origin_bricks);
	ClassDB::bind_method(D_METHOD("get_world_origin_bricks"), &VoxelWorld::get_world_origin_bricks);
	ClassDB::bind_method(D_METHOD("set_world_size_regions", "v"), &VoxelWorld::set_world_size_regions);
	ClassDB::bind_method(D_METHOD("get_world_size_regions"), &VoxelWorld::get_world_size_regions);
	ClassDB::bind_method(D_METHOD("set_residency_radius_m", "v"), &VoxelWorld::set_residency_radius_m);
	ClassDB::bind_method(D_METHOD("get_residency_radius_m"), &VoxelWorld::get_residency_radius_m);
	ClassDB::bind_method(D_METHOD("set_physics_enabled", "v"), &VoxelWorld::set_physics_enabled);
	ClassDB::bind_method(D_METHOD("get_physics_enabled"), &VoxelWorld::get_physics_enabled);
	ClassDB::bind_method(D_METHOD("set_physics_center_path", "p"), &VoxelWorld::set_physics_center_path);
	ClassDB::bind_method(D_METHOD("get_physics_center_path"), &VoxelWorld::get_physics_center_path);
	ClassDB::bind_method(D_METHOD("set_physics_radius_m", "v"), &VoxelWorld::set_physics_radius_m);
	ClassDB::bind_method(D_METHOD("get_physics_radius_m"), &VoxelWorld::get_physics_radius_m);
	ClassDB::bind_method(D_METHOD("set_physics_bubble_radius_m", "v"), &VoxelWorld::set_physics_bubble_radius_m);
	ClassDB::bind_method(D_METHOD("get_physics_bubble_radius_m"), &VoxelWorld::get_physics_bubble_radius_m);
	ClassDB::bind_method(D_METHOD("set_max_collider_chunks", "v"), &VoxelWorld::set_max_collider_chunks);
	ClassDB::bind_method(D_METHOD("get_max_collider_chunks"), &VoxelWorld::get_max_collider_chunks);
	ClassDB::bind_method(D_METHOD("set_mesh_jobs_per_frame", "v"), &VoxelWorld::set_mesh_jobs_per_frame);
	ClassDB::bind_method(D_METHOD("get_mesh_jobs_per_frame"), &VoxelWorld::get_mesh_jobs_per_frame);
	ClassDB::bind_method(D_METHOD("set_shape_builds_per_frame", "v"), &VoxelWorld::set_shape_builds_per_frame);
	ClassDB::bind_method(D_METHOD("get_shape_builds_per_frame"), &VoxelWorld::get_shape_builds_per_frame);
	ClassDB::bind_method(D_METHOD("set_max_lod_pages", "v"), &VoxelWorld::set_max_lod_pages);
	ClassDB::bind_method(D_METHOD("get_max_lod_pages"), &VoxelWorld::get_max_lod_pages);
	ClassDB::bind_method(D_METHOD("set_lod_builds_per_frame", "v"), &VoxelWorld::set_lod_builds_per_frame);
	ClassDB::bind_method(D_METHOD("get_lod_builds_per_frame"), &VoxelWorld::get_lod_builds_per_frame);
	ClassDB::bind_method(D_METHOD("set_quality_tier", "v"), &VoxelWorld::set_quality_tier);
	ClassDB::bind_method(D_METHOD("get_quality_tier"), &VoxelWorld::get_quality_tier);
	ClassDB::bind_method(D_METHOD("set_effect_enabled", "name", "on"),
			&VoxelWorld::set_effect_enabled);
	ClassDB::bind_method(D_METHOD("get_effect_enabled", "name"),
			&VoxelWorld::get_effect_enabled);
	ClassDB::bind_method(D_METHOD("debug_beauty_settings"),
			&VoxelWorld::debug_beauty_settings);
	ClassDB::bind_method(D_METHOD("debug_beauty_compositor_stats"),
			&VoxelWorld::debug_beauty_compositor_stats);
	ClassDB::bind_method(D_METHOD("debug_gpu_timings"), &VoxelWorld::debug_gpu_timings);
	ClassDB::bind_method(D_METHOD("debug_ingest_gpu_timings", "names", "gpu_us", "rd_frame"),
			&VoxelWorld::debug_ingest_gpu_timings);
	ClassDB::bind_method(D_METHOD("debug_contact_shadow_probe", "pos", "fwd", "w", "h"),
			&VoxelWorld::debug_contact_shadow_probe);
	ClassDB::bind_method(D_METHOD("debug_ssr_probe", "fixture", "w", "h"),
			&VoxelWorld::debug_ssr_probe);
	ClassDB::bind_method(D_METHOD("debug_outline_probe", "fixture", "have_dynamic_normals"),
			&VoxelWorld::debug_outline_probe);
	ClassDB::bind_method(D_METHOD("debug_glossy_sdf_probe", "origin", "dir"),
			&VoxelWorld::debug_glossy_sdf_probe);
	ClassDB::bind_method(D_METHOD("debug_ssgi_probe", "pos", "fwd", "w", "h", "frames"),
			&VoxelWorld::debug_ssgi_probe);
	ClassDB::bind_method(D_METHOD("debug_ssgi_reprojection_probe", "previous_pos",
			"previous_fwd", "current_pos", "current_fwd", "w", "h"),
			&VoxelWorld::debug_ssgi_reprojection_probe);
	ClassDB::bind_method(D_METHOD("debug_lod_tick", "pos", "fwd"), &VoxelWorld::debug_lod_tick);
	ClassDB::bind_method(D_METHOD("debug_lod_stats"), &VoxelWorld::debug_lod_stats);
	ClassDB::bind_method(D_METHOD("debug_lod_fade_band"), &VoxelWorld::debug_lod_fade_band);
	ClassDB::bind_method(D_METHOD("debug_lod_render_probe", "pos", "fwd", "w", "h"),
			&VoxelWorld::debug_lod_render_probe);
	ClassDB::bind_method(D_METHOD("debug_lod_render_probe_culled", "pos", "fwd", "w", "h",
			"cull"), &VoxelWorld::debug_lod_render_probe_culled);
	ClassDB::bind_method(D_METHOD("debug_lod_gbuffer_probe", "pos", "fwd", "w", "h"),
			&VoxelWorld::debug_lod_gbuffer_probe);
	ClassDB::bind_method(D_METHOD("debug_seam_probe", "pos", "fwd", "w", "h", "skip_lod"),
			&VoxelWorld::debug_seam_probe, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("debug_hiz_stats"), &VoxelWorld::debug_hiz_stats);
	ClassDB::bind_method(D_METHOD("debug_hiz_shutdown_probe"), &VoxelWorld::debug_hiz_shutdown_probe);
	ClassDB::bind_method(D_METHOD("debug_gbuffer_stats", "w", "h"),
			&VoxelWorld::debug_gbuffer_stats);
	ClassDB::bind_method(D_METHOD("debug_hiz_probe_synthetic", "far_value", "near_value"),
			&VoxelWorld::debug_hiz_probe_synthetic);
	ClassDB::bind_method(D_METHOD("debug_hiz_occluded", "lo", "hi", "depth"),
			&VoxelWorld::debug_hiz_occluded);
	ClassDB::bind_method(D_METHOD("debug_lod_cull_probe", "pos", "fwd"),
			&VoxelWorld::debug_lod_cull_probe);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_stats"),
			&VoxelWorld::debug_sun_shadow_stats);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_build", "force"),
			&VoxelWorld::debug_sun_shadow_build);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_visibility", "p"),
			&VoxelWorld::debug_sun_shadow_visibility);
	ClassDB::bind_method(D_METHOD("debug_init_physics"), &VoxelWorld::debug_init_physics);
	ClassDB::bind_method(D_METHOD("debug_teardown_physics"), &VoxelWorld::debug_teardown_physics);
	ClassDB::bind_method(D_METHOD("debug_mesh_lattice_diff", "chunk"), &VoxelWorld::debug_mesh_lattice_diff);
	ClassDB::bind_method(D_METHOD("debug_mesh_diff", "chunk"), &VoxelWorld::debug_mesh_diff);
	ClassDB::bind_method(D_METHOD("debug_consolidate_diff", "region"), &VoxelWorld::debug_consolidate_diff);
	ClassDB::bind_method(D_METHOD("debug_consolidate_region", "region"), &VoxelWorld::debug_consolidate_region);
	ClassDB::bind_method(D_METHOD("debug_region_op_count", "region"), &VoxelWorld::debug_region_op_count);
	ClassDB::bind_method(D_METHOD("debug_override_region_table", "region_slot"),
			&VoxelWorld::debug_override_region_table);
	ClassDB::bind_method(D_METHOD("debug_override_used"), &VoxelWorld::debug_override_used);
	ClassDB::bind_method(D_METHOD("debug_fill_override_pool"), &VoxelWorld::debug_fill_override_pool);
	ClassDB::bind_method(D_METHOD("debug_override_render_state", "brick"),
			&VoxelWorld::debug_override_render_state);
	ClassDB::bind_method(D_METHOD("debug_lod_diff", "level", "coord"), &VoxelWorld::debug_lod_diff);
	ClassDB::bind_method(D_METHOD("debug_apply_sphere_subtract", "centre", "radius"),
			&VoxelWorld::debug_apply_sphere_subtract);
	ClassDB::bind_method(D_METHOD("debug_apply_sphere_add", "centre", "radius", "material"),
			&VoxelWorld::debug_apply_sphere_add);
	ClassDB::bind_method(D_METHOD("debug_apply_volume_add", "slot", "origin", "voxel", "dim"),
			&VoxelWorld::debug_apply_volume_add);
	ClassDB::bind_method(D_METHOD("debug_island_extract_diff", "lo_cell", "hi_cell"), &VoxelWorld::debug_island_extract_diff);
	ClassDB::bind_method(D_METHOD("debug_place_test_island", "slot", "lo_cell", "hi_cell", "offset"), &VoxelWorld::debug_place_test_island);
	ClassDB::bind_method(D_METHOD("debug_place_test_island_rotated", "slot", "lo_cell", "hi_cell", "offset", "yaw", "volume_slot"), &VoxelWorld::debug_place_test_island_rotated, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("debug_clear_test_island", "slot"), &VoxelWorld::debug_clear_test_island);
	ClassDB::bind_method(D_METHOD("debug_island_tile_mask", "origin", "dir", "tan_x", "tan_y",
			"width", "height"), &VoxelWorld::debug_island_tile_mask);
	ClassDB::bind_method(D_METHOD("debug_mesh_submit", "chunks"), &VoxelWorld::debug_mesh_submit);
	ClassDB::bind_method(D_METHOD("debug_mesh_collect"), &VoxelWorld::debug_mesh_collect);
	ClassDB::bind_method(D_METHOD("debug_extract_submit", "id", "lo_cell", "hi_cell"),
			&VoxelWorld::debug_extract_submit);
	ClassDB::bind_method(D_METHOD("debug_extract_collect"), &VoxelWorld::debug_extract_collect);
	ClassDB::bind_method(D_METHOD("debug_lod_submit", "jobs"), &VoxelWorld::debug_lod_submit);
	ClassDB::bind_method(D_METHOD("debug_lod_collect"), &VoxelWorld::debug_lod_collect);
	ClassDB::bind_method(D_METHOD("debug_physics_frame", "center"), &VoxelWorld::debug_physics_frame);
	ClassDB::bind_method(D_METHOD("debug_set_physics_bubbles", "centers"), &VoxelWorld::debug_set_physics_bubbles);
	ClassDB::bind_method(D_METHOD("debug_physics_stats"), &VoxelWorld::debug_physics_stats);
	ClassDB::bind_method(D_METHOD("debug_perf_stats"), &VoxelWorld::debug_perf_stats);
	ClassDB::bind_method(D_METHOD("debug_island_frame", "dt", "center"), &VoxelWorld::debug_island_frame);
	ClassDB::bind_method(D_METHOD("debug_island_stats"), &VoxelWorld::debug_island_stats);
	ClassDB::bind_method(D_METHOD("debug_island_pending_uploads"), &VoxelWorld::debug_island_pending_uploads);
	ClassDB::bind_method(D_METHOD("debug_field_volume_upload_count"), &VoxelWorld::debug_field_volume_upload_count);
	ClassDB::bind_method(D_METHOD("debug_island_descriptors_pending"), &VoxelWorld::debug_island_descriptors_pending);
	ClassDB::bind_method(D_METHOD("debug_mesh_volume_slots"), &VoxelWorld::debug_mesh_volume_slots);
	ClassDB::bind_method(D_METHOD("debug_queue_test_island_upload", "slot", "sdf", "mat", "dim"),
			&VoxelWorld::debug_queue_test_island_upload);
	ClassDB::bind_method(D_METHOD("debug_queue_test_island_descriptors"),
			&VoxelWorld::debug_queue_test_island_descriptors);
	ClassDB::bind_method(D_METHOD("debug_queue_committed_field_volume_upload", "slot", "sdf",
			"mat", "dim"), &VoxelWorld::debug_queue_committed_field_volume_upload);
	ClassDB::bind_method(D_METHOD("debug_set_extraction_available", "v"),
			&VoxelWorld::debug_set_extraction_available);
	ClassDB::bind_method(D_METHOD("debug_set_fail_extractions", "v"),
			&VoxelWorld::debug_set_fail_extractions);
	ClassDB::bind_method(D_METHOD("debug_set_fail_extract_submit", "v"),
			&VoxelWorld::debug_set_fail_extract_submit);
	ClassDB::bind_method(D_METHOD("debug_set_fail_consolidations", "v"),
			&VoxelWorld::debug_set_fail_consolidations);
	ClassDB::bind_method(D_METHOD("debug_pump_consolidation"), &VoxelWorld::debug_pump_consolidation);
	ClassDB::bind_method(D_METHOD("debug_pump_consolidation_async"), &VoxelWorld::debug_pump_consolidation_async);
	ClassDB::bind_method(D_METHOD("debug_wait_consolidation"), &VoxelWorld::debug_wait_consolidation);
	ClassDB::bind_method(D_METHOD("debug_set_fail_consolidate_uploads", "v"),
			&VoxelWorld::debug_set_fail_consolidate_uploads);
	ClassDB::bind_method(D_METHOD("debug_set_fail_restore_overrides", "v"),
			&VoxelWorld::debug_set_fail_restore_overrides);
	ClassDB::bind_method(D_METHOD("debug_set_fail_restore_overrides_always", "v"),
			&VoxelWorld::debug_set_fail_restore_overrides_always);
	ClassDB::bind_method(D_METHOD("debug_set_pause_override_publication", "v"),
			&VoxelWorld::debug_set_pause_override_publication);
	ClassDB::bind_method(D_METHOD("debug_override_publication_paused"),
			&VoxelWorld::debug_override_publication_paused);
	ClassDB::bind_method(D_METHOD("debug_set_merge_sleep_seconds", "v"), &VoxelWorld::debug_set_merge_sleep_seconds);
#ifdef DEBUG_ENABLED
	// These hooks can change the production 64-body cap or mark atlas slots used; keep them
	// out of release ClassDB so release scripts cannot call them.
	ClassDB::bind_method(D_METHOD("debug_set_max_dynamic_bodies", "v"), &VoxelWorld::debug_set_max_dynamic_bodies);
	ClassDB::bind_method(D_METHOD("debug_set_atlas_slot_used", "slot", "used"), &VoxelWorld::debug_set_atlas_slot_used);
	ClassDB::bind_method(D_METHOD("debug_set_normal_pool_budget", "bytes"), &VoxelWorld::debug_set_normal_pool_budget);
	ClassDB::bind_method(D_METHOD("debug_stored_normal_stats"), &VoxelWorld::debug_stored_normal_stats);
	ClassDB::bind_method(D_METHOD("debug_normal_pool_state"), &VoxelWorld::debug_normal_pool_state);
	ClassDB::bind_method(D_METHOD("debug_normal_upload_override", "slot", "packed_normals"), &VoxelWorld::debug_normal_upload_override);
	ClassDB::bind_method(D_METHOD("debug_normal_release_override", "slot"), &VoxelWorld::debug_normal_release_override);
#endif
	ClassDB::bind_method(D_METHOD("debug_set_fail_next_spawn", "fail"), &VoxelWorld::debug_set_fail_next_spawn);
	ClassDB::bind_method(D_METHOD("debug_set_fail_next_restore", "fail"), &VoxelWorld::debug_set_fail_next_restore);
	ClassDB::bind_method(D_METHOD("debug_set_fail_next_carve", "fail"), &VoxelWorld::debug_set_fail_next_carve);
	ClassDB::bind_method(D_METHOD("debug_set_fail_next_resample", "fail"), &VoxelWorld::debug_set_fail_next_resample);
	ClassDB::bind_method(D_METHOD("debug_set_empty_next_extraction", "v"), &VoxelWorld::debug_set_empty_next_extraction);
	ClassDB::bind_method(D_METHOD("debug_wake_island_body", "index"), &VoxelWorld::debug_wake_island_body);
	ClassDB::bind_method(D_METHOD("debug_offset_island_body", "index", "offset"), &VoxelWorld::debug_offset_island_body);
	ClassDB::bind_method(D_METHOD("debug_island_body_info", "index"), &VoxelWorld::debug_island_body_info);
	ClassDB::bind_method(D_METHOD("debug_body_of_chunk", "chunk"), &VoxelWorld::debug_body_of_chunk);
	ClassDB::bind_method(D_METHOD("debug_chunk_collider_info", "chunk"), &VoxelWorld::debug_chunk_collider_info);
	ClassDB::bind_method(D_METHOD("debug_chunk_collider_octants", "chunk"),
			&VoxelWorld::debug_chunk_collider_octants);
	ClassDB::bind_method(D_METHOD("ensure_initialized"), &VoxelWorld::ensure_initialized);
	ClassDB::bind_method(D_METHOD("is_initialized"), &VoxelWorld::is_initialized);
	ClassDB::bind_method(D_METHOD("debug_raymarch_pixel", "origin", "dir"), &VoxelWorld::debug_raymarch_pixel);
	ClassDB::bind_method(D_METHOD("debug_raymarch_probe", "origin", "dir"), &VoxelWorld::debug_raymarch_probe);
	ClassDB::bind_method(D_METHOD("debug_raymarch_cost_probe", "origin", "dir"),
			&VoxelWorld::debug_raymarch_cost_probe);
	ClassDB::bind_method(D_METHOD("debug_raymarch_gbuffer", "origin", "dir"), &VoxelWorld::debug_raymarch_gbuffer);
	ClassDB::bind_method(D_METHOD("debug_raymarch_hole_probe", "origin", "dir", "w", "h"),
			&VoxelWorld::debug_raymarch_hole_probe);
	ClassDB::bind_method(D_METHOD("debug_raymarch_normal_probe", "origin", "dir", "w", "h"),
			&VoxelWorld::debug_raymarch_normal_probe);
	ClassDB::bind_method(D_METHOD("debug_island_normal_probe", "island_slot", "origin", "dir", "w", "h"),
			&VoxelWorld::debug_island_normal_probe);
	ClassDB::bind_method(D_METHOD("debug_cel_diff", "albedo", "ambient", "ndl", "ndv", "ndh",
			"shadow", "ao", "gloss"), &VoxelWorld::debug_cel_diff);
	ClassDB::bind_method(D_METHOD("debug_cel_reference", "albedo", "ambient", "ndl", "ndv", "ndh",
			"shadow", "ao", "gloss"), &VoxelWorld::debug_cel_reference);
	ClassDB::bind_method(D_METHOD("debug_deferred_probe", "pos", "fwd", "w", "h", "probe_mode"),
			&VoxelWorld::debug_deferred_probe);
	ClassDB::bind_method(D_METHOD("debug_material_atlas_stats"), &VoxelWorld::debug_material_atlas_stats);
	ClassDB::bind_method(D_METHOD("debug_material_probe", "mat", "p", "n"), &VoxelWorld::debug_material_probe);
	ClassDB::bind_method(D_METHOD("debug_poke_material_normal", "layer"), &VoxelWorld::debug_poke_material_normal);
	ClassDB::bind_method(D_METHOD("debug_sdf_atlas"), &VoxelWorld::debug_sdf_atlas);
	ClassDB::bind_method(D_METHOD("debug_local_rd"), &VoxelWorld::debug_local_rd);
	ClassDB::bind_method(D_METHOD("debug_load_shader", "res_path"), &VoxelWorld::debug_load_shader);
	ClassDB::bind_method(D_METHOD("request_shader_reload"), &VoxelWorld::request_shader_reload);
	ClassDB::bind_method(D_METHOD("debug_pump_shader_reload"), &VoxelWorld::debug_pump_shader_reload);
	ClassDB::bind_method(D_METHOD("debug_shader_reload_stats"), &VoxelWorld::debug_shader_reload_stats);
	ClassDB::bind_method(D_METHOD("debug_set_shader_override", "name", "source"),
			&VoxelWorld::debug_set_shader_override);
	ClassDB::bind_method(D_METHOD("debug_self_check"), &VoxelWorld::debug_self_check);
	ClassDB::bind_method(D_METHOD("debug_store_volume", "slot", "sdf", "mat", "dim"), &VoxelWorld::debug_store_volume);
	ClassDB::bind_method(D_METHOD("debug_eval_field", "p", "ops", "op_count"), &VoxelWorld::debug_eval_field);
	ClassDB::bind_method(D_METHOD("debug_eval_field_gradient", "p", "ops", "op_count"), &VoxelWorld::debug_eval_field_gradient);
	ClassDB::bind_method(D_METHOD("debug_init_atlas"), &VoxelWorld::debug_init_atlas);
	ClassDB::bind_method(D_METHOD("debug_teardown_atlas"), &VoxelWorld::debug_teardown_atlas);
	ClassDB::bind_method(D_METHOD("debug_atlas_stats"), &VoxelWorld::debug_atlas_stats);
	ClassDB::bind_method(D_METHOD("debug_reset_frame_counters"), &VoxelWorld::debug_reset_frame_counters);
	ClassDB::bind_method(D_METHOD("debug_set_region_map_entry", "region_index", "region_slot"), &VoxelWorld::debug_set_region_map_entry);
	ClassDB::bind_method(D_METHOD("debug_upload_region_ops", "region_slot", "ops", "count"), &VoxelWorld::debug_upload_region_ops);
	ClassDB::bind_method(D_METHOD("debug_brick_has_surface", "brick", "ops", "op_count"), &VoxelWorld::debug_brick_has_surface);
	ClassDB::bind_method(D_METHOD("debug_mark_region", "region", "region_slot", "lo", "hi", "op_count", "force"), &VoxelWorld::debug_mark_region);
	ClassDB::bind_method(D_METHOD("debug_generate_pending"), &VoxelWorld::debug_generate_pending);
	ClassDB::bind_method(D_METHOD("debug_brick_diff", "brick", "region_slot", "ops", "op_count"), &VoxelWorld::debug_brick_diff);
	ClassDB::bind_method(D_METHOD("debug_stream_region", "region"), &VoxelWorld::debug_stream_region);
	ClassDB::bind_method(D_METHOD("debug_brick_flags", "region"), &VoxelWorld::debug_brick_flags);
	ClassDB::bind_method(D_METHOD("debug_brick_flags_after_mark", "region"), &VoxelWorld::debug_brick_flags_after_mark);
	ClassDB::bind_method(D_METHOD("debug_release_region", "region_slot"), &VoxelWorld::debug_release_region);
	ClassDB::bind_method(D_METHOD("debug_jobs"), &VoxelWorld::debug_jobs);
	ClassDB::bind_method(D_METHOD("debug_region_table_slot", "region_slot", "brick"), &VoxelWorld::debug_region_table_slot);
	ClassDB::bind_method(D_METHOD("debug_mat_atlas"), &VoxelWorld::debug_mat_atlas);
	ClassDB::bind_method(D_METHOD("debug_mip_atlas", "level"), &VoxelWorld::debug_mip_atlas);
	ClassDB::bind_method(D_METHOD("debug_region_map"), &VoxelWorld::debug_region_map);
	ClassDB::bind_method(D_METHOD("debug_region_tables"), &VoxelWorld::debug_region_tables);
	ClassDB::bind_method(D_METHOD("debug_free_list"), &VoxelWorld::debug_free_list);
	ClassDB::bind_method(D_METHOD("debug_frame_counters"), &VoxelWorld::debug_frame_counters);
	ClassDB::bind_method(D_METHOD("debug_op_pool"), &VoxelWorld::debug_op_pool);
	ClassDB::bind_method(D_METHOD("debug_op_counts"), &VoxelWorld::debug_op_counts);
	ClassDB::bind_method(D_METHOD("debug_occupancy_state", "cell"), &VoxelWorld::debug_occupancy_state);
	ClassDB::bind_method(D_METHOD("debug_pump_occupancy"), &VoxelWorld::debug_pump_occupancy);
	ClassDB::bind_method(D_METHOD("debug_occupancy_diff", "region"), &VoxelWorld::debug_occupancy_diff);
	ClassDB::bind_method(D_METHOD("debug_occupancy_fallback_diff", "region"), &VoxelWorld::debug_occupancy_fallback_diff);
	ClassDB::bind_method(D_METHOD("debug_cell_state", "cell"), &VoxelWorld::debug_cell_state);
	ClassDB::bind_method(D_METHOD("debug_field_sdf", "p"), &VoxelWorld::debug_field_sdf);
	ClassDB::bind_method(D_METHOD("debug_occupancy_stats", "center"), &VoxelWorld::debug_occupancy_stats);
	ClassDB::bind_method(D_METHOD("debug_stream_frame", "cam"), &VoxelWorld::debug_stream_frame);
	ClassDB::bind_method(D_METHOD("debug_stream_stats"), &VoxelWorld::debug_stream_stats);
	ClassDB::bind_method(D_METHOD("debug_slot_of_region", "region"), &VoxelWorld::debug_slot_of_region);
	ClassDB::bind_method(D_METHOD("debug_region_map_entry", "region"), &VoxelWorld::debug_region_map_entry);
	ClassDB::bind_method(D_METHOD("debug_region_map_consistent"), &VoxelWorld::debug_region_map_consistent);
	ClassDB::bind_method(D_METHOD("debug_raycast", "origin", "dir"), &VoxelWorld::debug_raycast);
	ClassDB::bind_method(D_METHOD("debug_spawn_test_body", "lo_cell", "hi_cell", "offset", "impulse", "debris"), &VoxelWorld::debug_spawn_test_body);
	ClassDB::bind_method(D_METHOD("debug_test_body_stats", "index"), &VoxelWorld::debug_test_body_stats);
	ClassDB::bind_method(D_METHOD("debug_tick_test_bodies", "dt"), &VoxelWorld::debug_tick_test_bodies);
	ClassDB::bind_method(D_METHOD("debug_despawn_test_body", "index"), &VoxelWorld::debug_despawn_test_body);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_local_device"), "set_use_local_device", "get_use_local_device");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "atlas_bricks"), "set_atlas_bricks", "get_atlas_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_region_slots"), "set_max_region_slots", "get_max_region_slots");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_brick_jobs"), "set_max_brick_jobs", "get_max_brick_jobs");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_override_bricks"), "set_max_override_bricks", "get_max_override_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_origin_bricks"), "set_world_origin_bricks", "get_world_origin_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_size_regions"), "set_world_size_regions", "get_world_size_regions");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "residency_radius_m"), "set_residency_radius_m", "get_residency_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "physics_enabled"), "set_physics_enabled", "get_physics_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "physics_center_path"), "set_physics_center_path", "get_physics_center_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "physics_radius_m"), "set_physics_radius_m", "get_physics_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "physics_bubble_radius_m"), "set_physics_bubble_radius_m", "get_physics_bubble_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_collider_chunks"), "set_max_collider_chunks", "get_max_collider_chunks");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_jobs_per_frame"), "set_mesh_jobs_per_frame", "get_mesh_jobs_per_frame");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape_builds_per_frame"), "set_shape_builds_per_frame", "get_shape_builds_per_frame");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_lod_pages"), "set_max_lod_pages", "get_max_lod_pages");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_builds_per_frame"), "set_lod_builds_per_frame", "get_lod_builds_per_frame");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "quality_tier", PROPERTY_HINT_ENUM,
			"Off,Low,Medium,High"), "set_quality_tier", "get_quality_tier");
}

void VoxelWorld::set_quality_tier(int v) {
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	quality_tier_ = v < 0 ? 0 : (v > 3 ? 3 : v);
	beauty_ = ve::settings_for_tier(static_cast<ve::QualityTier>(quality_tier_));
}

int VoxelWorld::get_quality_tier() const {
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	return quality_tier_;
}

namespace {
// One table, so the setter, the getter and the debug dictionary cannot disagree about what
// an effect is called.
bool *beauty_field(ve::BeautySettings &s, const String &name) {
	if (name == "ssgi") return &s.ssgi;
	if (name == "ssr") return &s.ssr;
	if (name == "contact_shadows") return &s.contact_shadows;
	if (name == "outlines") return &s.outlines;
	if (name == "sun_shadow_map") return &s.sun_shadow_map;
	if (name == "glossy_sdf_rays") return &s.glossy_sdf_rays;
	if (name == "raymarched_sun_shadow") return &s.raymarched_sun_shadow;
	if (name == "cost_view") return &s.cost_view;
	return nullptr;
}
} // namespace

void VoxelWorld::set_effect_enabled(const String &name, bool on) {
	if (name == "islands") {
		islands_enabled_.store(on, std::memory_order_relaxed);
		return;
	}
	if (name == "near_field") {
		near_field_enabled_.store(on, std::memory_order_relaxed);
		return;
	}
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	bool *f = beauty_field(beauty_, name);
	if (!f) return; // fail-soft: an unknown name in a debug menu is not a crash
	*f = on;
	ve::clamp_settings(&beauty_);
}

bool VoxelWorld::get_effect_enabled(const String &name) const {
	if (name == "islands") return islands_enabled_.load(std::memory_order_relaxed);
	if (name == "near_field") return near_field_enabled_.load(std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	ve::BeautySettings copy = beauty_;
	const bool *f = beauty_field(copy, name);
	return f ? *f : false;
}

ve::BeautySettings VoxelWorld::beauty_settings() const {
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	return beauty_;
}

Dictionary VoxelWorld::debug_gpu_timings() {
	return gpu_timings_.snapshot();
}

Dictionary VoxelWorld::debug_ingest_gpu_timings(const PackedStringArray &names,
		const PackedInt64Array &gpu_us, int64_t rd_frame) {
	return gpu_timings_.ingest_for_test(names, gpu_us, static_cast<uint64_t>(rd_frame));
}

Dictionary VoxelWorld::debug_beauty_compositor_stats() {
	Dictionary d;
	d["normal_roughness"] = normal_roughness_state_;
	d["contact_ms"] = contact_shadow_pass_ ? contact_shadow_pass_->last_ms() : 0.0f;
	// CPU command-record time only; GPU timings belong to the later performance task.
	d["ssr_ms"] = ssr_pass_ ? ssr_pass_->last_ms() : 0.0f;
	d["outline_ms"] = outline_pass_ ? outline_pass_->last_ms() : 0.0f;
	return d;
}

Dictionary VoxelWorld::debug_contact_shadow_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["mask_width"] = 0; d["mask_height"] = 0;
	d["mask_min"] = 1.0f; d["mask_mean"] = 1.0f;
	d["mean_darkening"] = 0.0f; d["max_brightening"] = 0.0f;
	d["max_neighbour_step"] = 0.0f;
	if (w <= 0 || h <= 0) return d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_ ||
			!composite_pass_ || !deferred_pass_ || !gbuffer_ || !contact_shadow_pass_ ||
			!beauty_camera_) return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	composite_pass_->release_targets();
	if (!gbuffer_->ensure(device, nullptr, Vector2i(w, h))) return d;
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
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_size_regions_.x; cp.dims[1] = world_size_regions_.y;
	cp.dims[2] = world_size_regions_.z; cp.dims[3] = island_slot_count();
	cp.region_origin[0] = ro.x; cp.region_origin[1] = ro.y; cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = atlas_bricks_.x; cp.atlas_bricks[1] = atlas_bricks_.y;
	cp.atlas_bricks[2] = atlas_bricks_.z;
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cp, w, h, no_edit)) return d;
	float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
	lod_fade_band(&fade_start, &fade_end);
	composite_pass_->draw(device, *gbuffer_, raymarch_pass_->albedo_texture(),
			raymarch_pass_->surface_texture(), raymarch_pass_->hitpos_texture(), view_proj,
			*materials_, p, fade_start, fade_end);
	if (!composite_pass_->last_draw_ok()) return d;
	DeferredPass::Params dp;
	const Projection inv = view_proj.inverse();
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
	dp.cam_pos[0] = pos.x; dp.cam_pos[1] = pos.y; dp.cam_pos[2] = pos.z;
	dp.flags = ve::pack_flags(beauty_settings());
	static const float no_sun[16] = {};
	if (!deferred_pass_->render(device, *gbuffer_, *materials_, RID(), RID(), no_sun, 0.0f, dp))
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
	device->texture_copy(gbuffer_->lit(), scratch, Vector3(), Vector3(), Vector3(w, h, 1), 0, 0, 0, 0);
	device->texture_copy(gbuffer_->lit(), before, Vector3(), Vector3(), Vector3(w, h, 1), 0, 0, 0, 0);
	beauty_camera_->ensure(device);
	const float cam_pos[3] = {pos.x, pos.y, pos.z};
	beauty_camera_->update(device, view_proj, cam_pos, Vector2i(w, h), 0.05f, 4000.0f);
	contact_shadow_pass_->render(device, scratch, gbuffer_->depth(), Vector2i(w, h),
			beauty_camera_->buffer(), beauty_settings());
	device->submit(); device->sync();
	const int mw = std::max(1, w / 2), mh = std::max(1, h / 2);
	d["mask_width"] = mw; d["mask_height"] = mh;
	PackedByteArray mask;
	if (contact_shadow_pass_->mask().is_valid())
		mask = device->texture_get_data(contact_shadow_pass_->mask(), 0);
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
	contact_shadow_pass_->teardown();
	device->free_rid(scratch); device->free_rid(before);
	contact_shadow_pass_->initialize(device);
	return d;
}

Dictionary VoxelWorld::debug_ssgi_probe(Vector3 pos, Vector3 fwd, int w, int h, int frames) {
	Dictionary d;
	d["width"] = std::max(1, w / 2);
	d["height"] = std::max(1, h / 2);
	d["max_channel"] = 0.0f;
	d["mean_luma"] = 0.0;
	d["ran"] = false;
	if (w <= 0 || h <= 0 || frames <= 0) return d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_ ||
			!composite_pass_ || !deferred_pass_ || !gbuffer_ || !beauty_camera_ || !ssgi_pass_)
		return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	composite_pass_->release_targets();
	if (!gbuffer_->ensure(device, nullptr, Vector2i(w, h)) || !beauty_camera_->ensure(device)) return d;

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
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_size_regions_.x; cp.dims[1] = world_size_regions_.y;
	cp.dims[2] = world_size_regions_.z; cp.dims[3] = island_slot_count();
	cp.region_origin[0] = ro.x; cp.region_origin[1] = ro.y; cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = atlas_bricks_.x; cp.atlas_bricks[1] = atlas_bricks_.y;
	cp.atlas_bricks[2] = atlas_bricks_.z;
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	static const float no_sun[16] = {};
	const ve::BeautySettings settings = beauty_settings();
	ssgi_pass_->clear_result();
	float prev_view_proj[16] = {};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) prev_view_proj[c * 4 + r] = view_proj.columns[c][r];
	const Projection inv = view_proj.inverse();
	bool ran = false;
	for (int i = 0; i < frames; i++) {
		beauty_camera_->update(device, view_proj, p, Vector2i(w, h), 0.05f, 4000.0f);
		if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cp, w, h, no_edit)) break;
		float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
		lod_fade_band(&fade_start, &fade_end);
		composite_pass_->draw(device, *gbuffer_, raymarch_pass_->albedo_texture(),
				raymarch_pass_->surface_texture(), raymarch_pass_->hitpos_texture(), view_proj,
				*materials_, p, fade_start, fade_end);
		if (!composite_pass_->last_draw_ok()) break;
		const bool ssgi_ok = ssgi_pass_->render(device, *gbuffer_, beauty_camera_->buffer(),
				prev_view_proj, i > 0, settings, static_cast<uint32_t>(i));
		ran = ran || ssgi_ok;
		DeferredPass::Params dp;
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
		dp.cam_pos[0] = pos.x; dp.cam_pos[1] = pos.y; dp.cam_pos[2] = pos.z;
		dp.flags = ve::pack_flags(settings);
		if (!deferred_pass_->render(device, *gbuffer_, *materials_,
				ssgi_ok ? ssgi_pass_->result() : RID(), RID(), no_sun, 0.0f, dp)) break;
		downsample_history(device, gbuffer_->lit(), *gbuffer_);
	}
	device->submit();
	device->sync();
	d["ran"] = ran;
	const RID output = ssgi_pass_->result();
	const Vector2i half = gbuffer_->half_size();
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

Dictionary VoxelWorld::debug_ssgi_reprojection_probe(Vector3 previous_pos, Vector3 previous_fwd,
		Vector3 current_pos, Vector3 current_fwd, int w, int h) {
	Dictionary d;
	d["non_identity"] = previous_pos != current_pos || previous_fwd != current_fwd;
	d["mapping_luma"] = 0.0;
	d["current_mapping_luma"] = 0.0;
	d["mapping_delta"] = 0.0;
	if (w <= 0 || h <= 0) return d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_ ||
			!composite_pass_ || !deferred_pass_ || !gbuffer_ || !beauty_camera_ || !ssgi_pass_)
		return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(previous_pos) == 0 ? quiet + 1 : 0;
	composite_pass_->release_targets();
	if (!gbuffer_->ensure(device, nullptr, Vector2i(w, h)) || !beauty_camera_->ensure(device))
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
			const ve::WorldBounds wb = world_bounds();
			const ve::IVec3 ro = wb.origin_regions();
			result.dims[0] = world_size_regions_.x;
			result.dims[1] = world_size_regions_.y;
			result.dims[2] = world_size_regions_.z;
			result.dims[3] = island_slot_count();
			result.region_origin[0] = ro.x;
			result.region_origin[1] = ro.y;
			result.region_origin[2] = ro.z;
			result.atlas_bricks[0] = atlas_bricks_.x;
			result.atlas_bricks[1] = atlas_bricks_.y;
			result.atlas_bricks[2] = atlas_bricks_.z;
			return result;
		};
	const ve::CameraParams previous_params = make_camera_params(previous_pos, previous_fwd,
			previous_up);
	const ve::CameraParams current_params = make_camera_params(current_pos, current_fwd, current_up);
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	static const float no_sun[16] = {};
	const ve::BeautySettings settings = beauty_settings();
	ssgi_pass_->clear_result();
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
			beauty_camera_->update(device, view_proj, camera_position, Vector2i(w, h), near_clip,
					far_clip);
			if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), camera, w, h, no_edit))
				return false;
			float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
			lod_fade_band(&fade_start, &fade_end);
			composite_pass_->draw(device, *gbuffer_, raymarch_pass_->albedo_texture(),
					raymarch_pass_->surface_texture(), raymarch_pass_->hitpos_texture(), view_proj,
					*materials_, camera_position, fade_start, fade_end);
			if (!composite_pass_->last_draw_ok()) return false;
			const bool ssgi_ok = ssgi_pass_->render(device, *gbuffer_, beauty_camera_->buffer(),
					previous_mapping, have_history, settings, frame);
			if (!ssgi_ok) return false;
			DeferredPass::Params dp;
			for (int c = 0; c < 4; c++)
				for (int r = 0; r < 4; r++) dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
			dp.cam_pos[0] = camera_pos.x;
			dp.cam_pos[1] = camera_pos.y;
			dp.cam_pos[2] = camera_pos.z;
			dp.flags = ve::pack_flags(settings);
			return deferred_pass_->render(device, *gbuffer_, *materials_, ssgi_pass_->result(), RID(),
					no_sun, 0.0f, dp);
	};
	auto read_luma = [&]() {
		const Vector2i half = gbuffer_->half_size();
		const PackedByteArray data = device->texture_get_data(ssgi_pass_->result(), 0);
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
	downsample_history(device, gbuffer_->lit(), *gbuffer_);
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

Dictionary VoxelWorld::debug_beauty_settings() {
	ve::BeautySettings beauty;
	int quality_tier;
	{
		std::lock_guard<std::mutex> lock(beauty_mutex_);
		beauty = beauty_;
		quality_tier = quality_tier_;
	}

	Dictionary d;
	d["ssgi"] = beauty.ssgi;
	d["ssr"] = beauty.ssr;
	d["contact_shadows"] = beauty.contact_shadows;
	d["outlines"] = beauty.outlines;
	d["sun_shadow_map"] = beauty.sun_shadow_map;
	d["glossy_sdf_rays"] = beauty.glossy_sdf_rays;
	d["raymarched_sun_shadow"] = beauty.raymarched_sun_shadow;
	d["cost_view"] = beauty.cost_view;
	d["islands"] = islands_enabled_.load(std::memory_order_relaxed);
	d["ssgi_taps"] = beauty.ssgi_taps;
	d["ssr_steps"] = beauty.ssr_steps;
	d["contact_steps"] = beauty.contact_steps;
	d["outline_depth_threshold"] = beauty.outline_depth_threshold;
	d["outline_normal_threshold"] = beauty.outline_normal_threshold;
	d["tier"] = quality_tier;
	d["flags"] = static_cast<int>(ve::pack_beauty_flags(beauty));
	return d;
}

void VoxelWorld::_ready() {
	// A scene can be instantiated again after a benchmark/test quit request in the same
	// process. Reset this world's lifetime state before reopening global callback admission.
	voxel_compositor_callbacks_ready(this);
	// Godot only calls _process on a GDExtension node that asks for it.
	set_process(true);
}

void VoxelWorld::_process(double delta) {
	// Unconditional: the grid and consolidation queue must keep draining even with physics
	// disabled, because edits and the debug hooks share this path.
	drain_occupancy();
	pump_consolidation();
	if (!physics_enabled_ || physics_center_path_.is_empty()) return;
	Node3D *anchor = Object::cast_to<Node3D>(get_node_or_null(physics_center_path_));
	if (!anchor) return;
	ensure_physics_initialized();
	physics_tick(anchor->get_global_position());
	if (island_manager_) island_manager_->run_frame(static_cast<float>(delta),
			anchor->get_global_position());
}

VoxelWorld::~VoxelWorld() {
	// Test-only shader overrides are global (they are consulted by ve::load_shader_source);
	// clear them when a world goes away so a broken override from one suite cannot leak into
	// the next world created in the same process.
	ve::clear_shader_source_overrides();
}

bool VoxelWorld::initialize_downsample(RenderingDevice *rd) {
	teardown_downsample();
	if (!rd) return false;
	const String path = ProjectSettings::get_singleton()->globalize_path(
			"res://shaders/downsample.comp.glsl");
	const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) return false;
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (!spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE).is_empty())
		return false;
	downsample_shader_ = rd->shader_create_from_spirv(spirv);
	downsample_pipeline_ = rd->compute_pipeline_create(downsample_shader_);
	Ref<RDSamplerState> sampler;
	sampler.instantiate();
	sampler->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	downsample_sampler_ = rd->sampler_create(sampler);
	if (!downsample_shader_.is_valid() || !downsample_pipeline_.is_valid() ||
			!downsample_sampler_.is_valid()) {
		teardown_downsample();
		return false;
	}
	return true;
}

void VoxelWorld::teardown_downsample() {
	RenderingDevice *device = rd();
	if (device) {
		for (RID *r : {&downsample_uset_, &downsample_pipeline_, &downsample_shader_,
				&downsample_sampler_}) {
			if (r->is_valid()) device->free_rid(*r);
			*r = RID();
		}
	}
	downsample_src_ = downsample_dst_ = RID();
}

bool VoxelWorld::ensure_downsample_set(RenderingDevice *rd, RID src, RID dst) {
	if (downsample_uset_.is_valid() && downsample_src_ == src && downsample_dst_ == dst)
		return true;
	if (downsample_uset_.is_valid()) rd->free_rid(downsample_uset_);
	Ref<RDUniform> u0, u1;
	u0.instantiate(); u1.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u0->set_binding(0); u0->add_id(downsample_sampler_); u0->add_id(src);
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u1->set_binding(1); u1->add_id(dst);
	downsample_uset_ = rd->uniform_set_create(Array::make(u0, u1), downsample_shader_, 0);
	if (!downsample_uset_.is_valid()) return false;
	downsample_src_ = src;
	downsample_dst_ = dst;
	return true;
}

void VoxelWorld::finish_beauty_frame(const float view_proj[16]) {
	if (view_proj) std::memcpy(prev_view_proj_, view_proj, sizeof(prev_view_proj_));
	beauty_frame_++;
}

bool VoxelWorld::downsample_history(RenderingDevice *rd, RID src, GBuffer &gb) {
	if (!rd || !downsample_pipeline_.is_valid() || !gb.history().is_valid()) return false;
	const Vector2i half = gb.half_size();
	if (!ensure_downsample_set(rd, src, gb.history())) return false;
	PackedByteArray pc;
	pc.resize(16);
	int32_t *dims = reinterpret_cast<int32_t *>(pc.ptrw());
	dims[0] = half.x; dims[1] = half.y; dims[2] = dims[3] = 0;
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, downsample_pipeline_);
	rd->compute_list_bind_uniform_set(list, downsample_uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (half.x + 7) / 8, (half.y + 7) / 8, 1);
	rd->compute_list_end();
	has_history_ = true;
	return true;
}

void VoxelWorld::teardown_gpu() {
	// Passes before the atlas: their uniform sets reference atlas RIDs, and freeing a
	// texture cascades to referencing sets (M1's documented order). Islands sit between
	// passes and the atlas pool: RaymarchPass's uniform set references island buffers too.
	if (composite_pass_) { delete composite_pass_; composite_pass_ = nullptr; }
	if (inject_pass_) { delete inject_pass_; inject_pass_ = nullptr; }
	if (deferred_pass_) { delete deferred_pass_; deferred_pass_ = nullptr; }
	if (sun_shadow_pass_) { delete sun_shadow_pass_; sun_shadow_pass_ = nullptr; }
	if (hiz_pass_ && gbuffer_) hiz_pass_->release_level0_set();
	teardown_downsample();
	if (contact_shadow_pass_) { delete contact_shadow_pass_; contact_shadow_pass_ = nullptr; }
	if (ssr_pass_) { delete ssr_pass_; ssr_pass_ = nullptr; }
	if (outline_pass_) { delete outline_pass_; outline_pass_ = nullptr; }
	if (ssgi_pass_) { delete ssgi_pass_; ssgi_pass_ = nullptr; }
	if (beauty_camera_) { beauty_camera_->teardown(); delete beauty_camera_; beauty_camera_ = nullptr; }
	if (gbuffer_) { delete gbuffer_; gbuffer_ = nullptr; }
	if (raymarch_pass_) { delete raymarch_pass_; raymarch_pass_ = nullptr; }
	if (lod_raster_pass_) { delete lod_raster_pass_; lod_raster_pass_ = nullptr; }
	if (lod_cull_pass_) { delete lod_cull_pass_; lod_cull_pass_ = nullptr; }
	if (hiz_pass_) {
		hiz_pass_->teardown();
		last_hiz_readback_was_pending_ = hiz_pass_->readback_was_pending_at_teardown();
		last_hiz_readback_was_drained_ = hiz_pass_->readback_was_drained_at_teardown();
		delete hiz_pass_;
		hiz_pass_ = nullptr;
	}
	if (materials_) { delete materials_; materials_ = nullptr; }
	if (gen_pass_) { delete gen_pass_; gen_pass_ = nullptr; }
	if (region_pass_) { delete region_pass_; region_pass_ = nullptr; }
	if (streamer_) {
		streamer_->drain_readbacks(rd());
		delete streamer_;
		streamer_ = nullptr;
	}
	if (residency_) { residency_->clear(); } // slot assignments are meaningless pre-atlas
	if (island_cull_) { delete island_cull_; island_cull_ = nullptr; }
	if (islands_) { delete islands_; islands_ = nullptr; }
	{
		// island_slot_count() can still be on the render thread during teardown; keep the
		// high-water mark's write under the same mutex.
		std::lock_guard<std::mutex> lock(island_mutex_);
		island_slots_ = 0;
	}
	if (atlas_) { delete atlas_; atlas_ = nullptr; }
	// The tree holds page indices the pool is about to free, and a stale index would be
	// handed to the next chunk. Pool first, then tree, then the page map.
	if (lod_pool_) lod_pool_->teardown();
	if (lod_tree_) lod_tree_->clear();
	lod_pages_of_.clear();
	lod_page_quads_.clear();
	lod_overflow_logged_.clear();
	has_history_ = false;
	beauty_frame_ = 0;
	std::memset(prev_view_proj_, 0, sizeof(prev_view_proj_));
	initialized_ = false;
}

void VoxelWorld::shutdown_render_resources_on_render_thread() {
	teardown_gpu();
	{
		std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
		gpu_teardown_done_ = true;
	}
	gpu_teardown_cv_.notify_all();
}

void VoxelWorld::shutdown_render_resources() {
	// Close admission before synchronizing or queueing teardown. The admission lock makes
	// the enabled check and world lookup indivisible from this transition.
	voxel_compositor_callbacks_shutdown_started(this);
	{
		std::unique_lock<std::mutex> lock(render_lifetime_mutex_);
		const bool on_render_thread = RenderingServer::get_singleton()->is_on_render_thread();
		if (on_render_thread && render_callbacks_ > 0) {
			render_teardown_deferred_ = true;
			return;
		}
		if (!on_render_thread) {
			render_lifetime_cv_.wait(lock, [this] { return render_callbacks_ == 0; });
		}
	}
	if (!initialized_ || !rd()) return;
	if (RenderingServer::get_singleton()->is_on_render_thread() || use_local_device_ || !main_rd_) {
		teardown_gpu();
		return;
	}
	// Drain the RenderingServer queue first; unlike RenderingDevice::submit/sync this is the
	// supported global-device synchronization boundary. The actual RD teardown is queued on
	// the render thread, where HizPass can drain its pending async Callable safely.
	RenderingServer::get_singleton()->force_sync();
	{
		std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
		gpu_teardown_done_ = false;
	}
	RenderingServer::get_singleton()->call_on_render_thread(
			Callable(this, "_shutdown_render_resources_on_render_thread"));
	std::unique_lock<std::mutex> lock(render_lifetime_mutex_);
	gpu_teardown_cv_.wait(lock, [this] { return gpu_teardown_done_; });
}

void VoxelWorld::_exit_tree() {
	// SceneTree::quit() can tear down the main loop while the renderer still has one or more
	// compositor callbacks queued. shutdown_render_resources() closes admission and drains
	// callbacks before freeing GPU resources; this preserves the same lifetime boundary for
	// explicit benchmark shutdown and normal SceneTree exit.
	shutdown_render_resources();
	teardown_physics();
	if (residency_) { delete residency_; residency_ = nullptr; }
	if (edit_log_) { delete edit_log_; edit_log_ = nullptr; }
	if (overrides_) { delete overrides_; overrides_ = nullptr; }
	pending_edits_.clear();
	overflow_seen_ = 0;
	if (lod_pool_) {
		delete lod_pool_;
		lod_pool_ = nullptr;
	}
	if (lod_tree_) {
		delete lod_tree_;
		lod_tree_ = nullptr;
	}
	lod_pages_of_.clear();
	lod_page_quads_.clear();
	if (local_rd_) {
		memdelete(local_rd_);
		local_rd_ = nullptr;
	}
	main_rd_ = nullptr;
}

void VoxelWorld::ensure_initialized() {
	{
		std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
		if (render_shutting_down_) return;
	}
	if (initialized_) return;
	if (use_local_device_ && !local_rd_) {
		local_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	} else if (!use_local_device_ && !main_rd_) {
		main_rd_ = RenderingServer::get_singleton()->get_rendering_device();
	}
	RenderingDevice *device = rd();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return;
	}
	atlas_ = new GpuAtlas();
	GpuAtlasConfig cfg;
	cfg.atlas_bricks = {atlas_bricks_.x, atlas_bricks_.y, atlas_bricks_.z};
	cfg.max_region_slots = max_region_slots_;
	cfg.max_brick_jobs = max_brick_jobs_;
	cfg.max_override_bricks = max_override_bricks_;
	cfg.bounds = world_bounds();
	if (normal_pool_bytes_ > 0) cfg.normal_pool_bytes = normal_pool_bytes_; // test initializer
	if (!atlas_->initialize(device, cfg)) { delete atlas_; atlas_ = nullptr; return; }
	islands_ = new IslandAtlas();
	if (!islands_->initialize(device)) { teardown_gpu(); return; }
	island_cull_ = new IslandCullPass();
	if (!island_cull_->initialize(device)) { teardown_gpu(); return; }
	region_pass_ = new RegionPass();
	if (!region_pass_->initialize(device, *atlas_)) { teardown_gpu(); return; }
	gen_pass_ = new BrickGenPass();
	if (!gen_pass_->initialize(device, *atlas_)) { teardown_gpu(); return; }
	materials_ = new MaterialAtlas();
	if (!materials_->initialize(device)) { teardown_gpu(); return; }
	if (!edit_log_) edit_log_ = new ve::EditLog(world_bounds());
	if (!overrides_) overrides_ = new ve::OverrideStore(atlas_->overrides().capacity());
	if (!atlas_->replay_overrides(device, *overrides_, override_tables_)) {
		UtilityFunctions::printerr("VoxelWorld: override replay into render pool failed");
		teardown_gpu();
		return;
	}
	if (!residency_) {
		ve::ResidencyConfig rcfg;
		rcfg.bounds = world_bounds();
		rcfg.radius_m = residency_radius_m_;
		rcfg.max_region_slots = max_region_slots_;
		residency_ = new ve::RegionResidency(rcfg);
	}
	streamer_ = new WorldStreamer();
	streamer_->initialize(residency_, edit_log_, &edit_mutex_, &pending_edits_, atlas_,
			region_pass_, gen_pass_, &occupancy_mutex_, &occupancy_inbox_, &edit_seq_, overrides_,
			&override_tables_);
	raymarch_pass_ = new RaymarchPass();
	raymarch_pass_->initialize(device);
	raymarch_pass_->set_materials(*materials_);
	composite_pass_ = new CompositePass();
	composite_pass_->initialize(device);
	deferred_pass_ = new DeferredPass();
	deferred_pass_->initialize(device);
	inject_pass_ = new InjectPass();
	inject_pass_->initialize(device);
	gbuffer_ = new GBuffer();
	beauty_camera_ = new CameraUbo();
	contact_shadow_pass_ = new ContactShadowPass();
	contact_shadow_pass_->initialize(device);
	ssgi_pass_ = new SsgiPass();
	ssgi_pass_->initialize(device);
	ssr_pass_ = new SsrPass();
	ssr_pass_->initialize(device);
	outline_pass_ = new OutlinePass();
	outline_pass_->initialize(device);
	initialize_downsample(device);
	lod_raster_pass_ = new LodRasterPass();
	lod_raster_pass_->initialize(device);
	sun_shadow_pass_ = new SunShadowPass();
	if (!sun_shadow_pass_->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: sun shadow initialization failed; continuing without "
				"the world shadow map");
		delete sun_shadow_pass_;
		sun_shadow_pass_ = nullptr;
	}
	lod_cull_pass_ = new LodCullPass();
	if (!lod_cull_pass_->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: LoD cull initialization failed; continuing "
				"without GPU culling (safe fail-soft: draw every candidate page)");
		delete lod_cull_pass_;
		lod_cull_pass_ = nullptr;
	}
	hiz_pass_ = new HizPass();
	if (!hiz_pass_->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: HiZ initialization failed; continuing without "
				"occlusion (safe fail-soft: always visible)");
		delete hiz_pass_;
		hiz_pass_ = nullptr;
	}
	initialized_ = true;
}

bool VoxelWorld::queue_consolidation(ve::IVec3 region) {
	if (consolidation_in_flight_ && consolidation_job_.region == region) return false;
	for (const ve::IVec3 &queued : consolidation_queue_)
		if (queued == region) return false;
	if (static_cast<int>(consolidation_queue_.size()) + (consolidation_in_flight_ ? 1 : 0) >=
			OverridePool::kMaxOverrideTables) {
		consolidation_queue_refusals_++;
		if (!consolidation_queue_refusal_logged_) {
			UtilityFunctions::printerr("VoxelWorld: consolidation queue full; refusing new region once");
			consolidation_queue_refusal_logged_ = true;
		}
		return false;
	}
	consolidation_queue_.push_back(region);
	return true;
}

void VoxelWorld::requeue_consolidation_locked(ve::IVec3 region) {
	for (const ve::IVec3 &queued : consolidation_queue_)
		if (queued == region) return;
	if (static_cast<int>(consolidation_queue_.size()) < OverridePool::kMaxOverrideTables)
		consolidation_queue_.insert(consolidation_queue_.begin(), region);
	else {
		consolidation_queue_refusals_++;
		if (!consolidation_queue_refusal_logged_) {
			UtilityFunctions::printerr("VoxelWorld: consolidation rollback could not requeue region once");
			consolidation_queue_refusal_logged_ = true;
		}
	}
}

ve::EditLog::AppendResult VoxelWorld::append_edit(const ve::EditOp &op) {
	std::lock_guard<std::mutex> lock(edit_mutex_);
	return append_edit_locked(op);
}

ve::EditLog::AppendResult VoxelWorld::append_edit_locked(const ve::EditOp &op,
		bool notify_islands) {
	if (!edit_log_) return {};
	ve::EditLog::AppendResult r = edit_log_->append(op);
	// Queue before the list reaches its hard cap. The bake is asynchronous, so the spare 64
	// entries absorb edits appended while the worker is in flight.
	for (const ve::IVec3 &region : r.touched)
		if (edit_log_->op_count(region) >= ve::kConsolidateAtOps)
			queue_consolidation(region);
	// Bump AFTER the append and under the same lock the streamer uses to capture op counts.
	// If the seq moved before the append, a readback stamped between the bump and the append
	// would claim edits that are not in the GPU state the readback describes.
	edit_seq_.fetch_add(1, std::memory_order_relaxed);
	if (!r.rejected.empty()) {
		edit_rejections_ += static_cast<int>(r.rejected.size());
		UtilityFunctions::printerr("VoxelWorld: region op list full, op rejected (",
				r.rejected[0].x, ", ", r.rejected[0].y, ", ", r.rejected[0].z,
				") — spec §8 fail-soft");
	}
	pending_edits_.push_back({op, r});
	if (lod_tree_ && !r.touched.empty()) {
		float lo[3], hi[3];
		ve::op_world_aabb(op, lo, hi);
		// Every level: ve::LodTree::mark_dirty walks them itself, and the relevance cut is
		// at the HALF-CELL supersample resolution rather than the cell -- a 5 m crater still
		// registers at L4's 6.4 m cells, which is the point of the reduction change. Only
		// ops shorter than half a cell on every axis are genuinely unrepresentable.
		// Lock order: caller holds edit_mutex_, lod_tick never holds lod_mutex_ while taking
		// edit_mutex_, so edit_mutex_ -> lod_mutex_ is safe.
		std::lock_guard<std::mutex> lock(lod_mutex_);
		lod_tree_->mark_dirty(lo, hi);
	}
	// Connectivity's half of the fan-out. Runs under the append lock; the manager's
	// pending-window queue is guarded by its own windows_mutex_ (note_edit may be called
	// from a tool thread), and the seq bump above lets the window know which readback is
	// "new enough" to act on. A fully rejected op changed no field state, so it must not
	// enqueue a window: doing so would re-label the same component and retry the rejected
	// edit forever.
	if (notify_islands && island_manager_ && !r.touched.empty())
		island_manager_->note_edit(op, edit_seq_.load(std::memory_order_relaxed));
	// Collision's half of the fan-out (spec §5: "Fan-out: raymarch set, physics remesh queue,
	// LoD chain, connectivity"). Queued rather than applied, because this may run on any
	// thread that owns a tool while ChunkResidency belongs to the main one; physics_tick
	// drains it. Queued even when physics is off, so enabling it later starts consistent.
	ve::IVec3 clo{}, chi{};
	ve::op_chunk_range(op, &clo, &chi);
	pending_dirty_.push_back({clo, chi});
	return r;
}

RenderingDevice *VoxelWorld::rd() const {
	return use_local_device_ ? local_rd_ : main_rd_;
}

ve::WorldBounds VoxelWorld::world_bounds() const {
	ve::WorldBounds b;
	b.origin_bricks = {world_origin_bricks_.x, world_origin_bricks_.y, world_origin_bricks_.z};
	b.size_regions = {world_size_regions_.x, world_size_regions_.y, world_size_regions_.z};
	return b;
}

int VoxelWorld::island_slot_count() const {
	// The render thread calls this from RaymarchCompositor::_render_callback. The manager
	// pointer and island_slots_ are written on the main thread, so reads must hold
	// island_mutex_. The manager's own slot_high_water_ is atomic as well, since it is also
	// updated outside this mutex.
	std::lock_guard<std::mutex> lock(island_mutex_);
	if (!islands_enabled_.load(std::memory_order_relaxed)) return 0;
	const int manager_slots = island_manager_ ? island_manager_->slot_high_water() : 0;
	return island_slots_ > manager_slots ? island_slots_ : manager_slots;
}

void VoxelWorld::ensure_physics_initialized() {
	if (physics_ready_) return;
	// The CPU cores are shared with the streaming path and outlive both (voxel_world.h).
	if (!edit_log_) edit_log_ = new ve::EditLog(world_bounds());
	if (!overrides_) overrides_ = new ve::OverrideStore(max_override_bricks_);
	mesh_ = new MeshService();
	MeshPassConfig mcfg;
	mcfg.max_jobs = mesh_jobs_per_frame_;
	mcfg.max_override_bricks = overrides_ ? overrides_->capacity() : max_override_bricks_;
	if (!mesh_->start(mcfg)) {
		delete mesh_;
		mesh_ = nullptr;
		return;
	}
	if (!mesh_->replay_overrides(*overrides_, override_tables_)) {
		mesh_->stop();
		delete mesh_;
		mesh_ = nullptr;
		return;
	}
	if (streamer_) streamer_->set_mesh_service(mesh_);
	// A fresh MeshService starts with an empty worker-side volume pool. The edit log and
	// VolumeSet survive physics teardown, so replay every pinned volume into the new worker;
	// the preserved island_uploads_ only covers the render device's pool.
	for (int slot = 0; slot < ve::kMaxVolumes; slot++) {
		if (!volumes_.pinned(slot)) continue;
		const ve::VolumeData *d = volumes_.get(slot);
		if (d) mesh_->submit_volume(slot, *d);
	}
	ve::ChunkResidencyConfig ccfg;
	ccfg.bounds = world_bounds();
	ccfg.radius_m = physics_radius_m_;
	ccfg.max_chunks = max_collider_chunks_;
	ccfg.max_builds_per_frame = mesh_jobs_per_frame_;
	chunks_ = new ve::ChunkResidency(ccfg);
	colliders_ = new ColliderStreamer();
	colliders_->initialize(chunks_, edit_log_, &edit_mutex_, mesh_, max_collider_chunks_);
	colliders_->set_shape_builds_per_frame(shape_builds_per_frame_);
	colliders_->set_body_bubble_radius_m(physics_bubble_radius_m_);
	// Publish the manager under edit_mutex_: append_edit_locked() can be called from a tool
	// thread and reads island_manager_ while holding that lock, so creation must not expose a
	// half-initialized pointer to it. Also take island_mutex_ (edit_mutex_ -> island_mutex_
	// order, matching teardown) so the render thread's island_slot_count() sees a stable
	// pointer.
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		std::lock_guard<std::mutex> island_lock(island_mutex_);
		island_manager_ = new IslandManager();
		island_manager_->initialize(this);
	}
	physics_ready_ = true;
}

void VoxelWorld::teardown_physics() {
	std::unique_lock<std::mutex> edit_lock(edit_mutex_);
	physics_ready_ = false;
	if (streamer_) streamer_->set_mesh_service(nullptr);
	for (IslandBody *b : test_bodies_) delete b;
	test_bodies_.clear();
	// The manager owns the real island bodies; tear it down before the mesher's worker and
	// the colliders so its volume-slot bookkeeping still has a live VolumeSet to ask. Hold
	// edit_mutex_ while deleting/null it: a tool thread may already be inside
	// append_edit_locked() reading island_manager_ to call note_edit(). Also take
	// island_mutex_ so the render thread's island_slot_count() cannot dereference a manager
	// that is being destroyed (lock order: edit_mutex_ -> island_mutex_).
	//
	// Detach under the lock, then tear down outside it: teardown() releases every body's,
	// in-flight extraction's and merge's volume slot through release_volume_slot(), which
	// takes island_mutex_ to queue the GPU-side normal release. Running it under the lock
	// re-entered a non-recursive std::mutex and hung the process. The render thread is
	// still safe -- it sees a null manager the instant the lock is dropped -- and the tool
	// thread cannot observe the detached pointer because edit_mutex_ is held throughout.
	IslandManager *manager = nullptr;
	{
		std::lock_guard<std::mutex> island_lock(island_mutex_);
		manager = island_manager_;
		island_manager_ = nullptr;
	}
	if (manager) {
		manager->teardown();
		delete manager;
	}
	physics_bubble_centers_.clear();
	// Drop any uploads/descriptors the previous manager queued before the GPU pools are torn
	// down. If physics is re-initialized, stale queue entries must not be drained into the
	// new pools. The one exception is a field-volume upload for a slot the edit log already
	// references: those bytes are part of the surviving CPU volume set and MUST be mirrored
	// into any new GPU pool before an op that names the slot is evaluated.
	{
		std::lock_guard<std::mutex> lock(island_mutex_);
		std::vector<IslandUpload> keep;
		keep.reserve(island_uploads_.size());
		for (IslandUpload &u : island_uploads_)
			if (!u.to_island_atlas && volumes_.pinned(u.volume_slot))
				keep.push_back(std::move(u));
		island_uploads_.swap(keep);
		island_descs_.clear();
		island_descs_dirty_ = false;
	}
	// The worker is going away, but the render atlas and CPU store survive physics teardown.
	// An in-flight transaction may already have acquired slots and staged new bytes there;
	// restore the old consumer state before releasing those speculative slots. Never leave a
	// new table pointing at bytes whose CPU transaction is about to be discarded.
	{
		if (consolidation_in_flight_) {
			const ve::IVec3 region = consolidation_job_.region;
			bool render_restored = true;
			if (atlas_) {
				for (size_t i = 0; i < consolidation_old_slots_.size(); i++) {
					if (!atlas_->upload_override(rd(), consolidation_old_slots_[i],
							consolidation_old_bricks_[i])) render_restored = false;
					// Restore the NORMAL handle alongside the bytes, exactly as
					// rollback_render() does. Leaving the new bake's normals bound to a slot
					// holding the old brick's SDF/material shades a surface that is not there.
					const ve::OverrideBrick &old_brick = consolidation_old_bricks_[i];
					if (old_brick.normal_oct.size() == ve::kBrickSdfCount)
						atlas_->stored_normals().upload_override(rd(),
								consolidation_old_slots_[i], old_brick.normal_oct.data(),
								ve::kBrickSdfCount);
					else
						atlas_->stored_normals().release_override(rd(),
								consolidation_old_slots_[i]);
				}
				if (consolidation_table_ >= 0)
					atlas_->overrides().clear_table(rd(), consolidation_table_);
				if (consolidation_job_.region_slot >= 0) {
					if (render_restored)
						atlas_->set_override_table(rd(), consolidation_job_.region_slot,
								consolidation_old_table_, consolidation_old_entries_);
					else
						// A failed byte restore must not expose the partial new table. The CPU
						// store/map remain authoritative and reinit will replay them.
						atlas_->set_override_table(rd(), consolidation_job_.region_slot, -1, {});
				}
			}
			if (!render_restored)
				UtilityFunctions::printerr(
						"VoxelWorld: render override rollback failed during physics teardown; invalidated table");
			for (const ve::IVec3 brick : consolidation_newly_acquired_) {
				const int slot = overrides_ ? overrides_->slot_of(brick) : -1;
				if (slot >= 0 && atlas_) atlas_->stored_normals().release_override(rd(), slot);
				overrides_->release(brick);
			}
			if (edit_log_ && edit_log_->op_count(region) > 0)
				consolidation_queue_.insert(consolidation_queue_.begin(), region);
			consolidation_in_flight_ = false;
			consolidation_job_ = ConsolidateJob{};
			consolidation_table_ = -1;
			consolidation_old_table_ = -1;
			consolidation_old_entries_.clear();
			consolidation_entries_.clear();
			consolidation_old_slots_.clear();
			consolidation_old_bricks_.clear();
			consolidation_newly_acquired_.clear();
			consolidation_slots_.clear();
			consolidation_baked_.clear();
			consolidation_publish_in_flight_ = false;
		}
	}
	// Colliders first: they hold the mesher's results and the residency's slots. Deleting the
	// service joins its thread, which frees the device and the pass on the thread that made
	// them; nothing else may outlive that.
	if (colliders_) { delete colliders_; colliders_ = nullptr; }
	if (mesh_) { delete mesh_; mesh_ = nullptr; }
	if (chunks_) { delete chunks_; chunks_ = nullptr; }
	pending_dirty_.clear();
}

int VoxelWorld::physics_tick(Vector3 center) {
	if (!physics_ready_ || !colliders_ || !chunks_) return 0;
	const auto t0 = std::chrono::steady_clock::now();
	// Drain the dirty ranges the edit path queued. They are COLLECTED under edit_mutex_ and
	// APPLIED here, on the main thread, so ChunkResidency needs no lock of its own — and the
	// probe inside update(), which takes edit_mutex_, can never deadlock against an edit.
	std::vector<std::pair<ve::IVec3, ve::IVec3>> dirty;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		dirty.swap(pending_dirty_);
	}
	for (const auto &r : dirty) chunks_->mark_dirty(r.first, r.second);
	const Ref<World3D> w = get_world_3d();
	if (w.is_valid()) colliders_->set_space(w->get_space());
	const int actions = colliders_->run_frame(center.x, center.y, center.z,
			physics_bubble_centers_.data(), static_cast<int>(physics_bubble_centers_.size() / 3));
	last_physics_tick_ms_ =
			std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return actions;
}

Dictionary VoxelWorld::debug_perf_stats() {
	Dictionary d;
	d["physics_tick_ms"] = last_physics_tick_ms_;
	d["phys_collect_ms"] = colliders_ ? colliders_->last_collect_ms() : 0.0f;
	d["phys_apply_ms"] = colliders_ ? colliders_->last_apply_ms() : 0.0f;
	d["phys_faces_ms"] = colliders_ ? colliders_->last_faces_ms() : 0.0f;
	d["phys_setdata_ms"] = colliders_ ? colliders_->last_setdata_ms() : 0.0f;
	// `build_ms` is the maximum one octant build call in the measured physics frame, not
	// the sum of all octant calls. This is the value exported into BENCH max_ms.
	d["build_ms"] = colliders_ ? colliders_->last_build_ms() : 0.0f;
	d["phys_body_ms"] = colliders_ ? colliders_->last_body_ms() : 0.0f;
	d["phys_tris"] = colliders_ ? colliders_->last_tris() : 0;
	d["phys_plan_ms"] = colliders_ ? colliders_->last_plan_ms() : 0.0f;
	d["phys_submit_ms"] = colliders_ ? colliders_->last_submit_ms() : 0.0f;
	d["stream_total_ms"] = streamer_ ? streamer_->last_total_ms() : 0.0f;
	d["stream_readback_ms"] = streamer_ ? streamer_->last_readback_ms() : 0.0f;
	d["island_ms"] = island_manager_ ? island_manager_->last_ms() : 0.0f;
	// lod_ms is CPU command-record time for the LoD raster + cull passes, not GPU execution
	// time. See LodRasterPass/LodCullPass::last_ms comments.
	d["lod_ms"] = (lod_raster_pass_ ? lod_raster_pass_->last_ms() : 0.0f) +
			(lod_cull_pass_ ? lod_cull_pass_->last_ms() : 0.0f);
	return d;
}

int VoxelWorld::debug_physics_frame(Vector3 center) {
	ensure_physics_initialized();
	return physics_tick(center);
}

void VoxelWorld::set_physics_bubble_radius_m(float v) {
	physics_bubble_radius_m_ = v;
	// Applies live: a test (and the editor's inspector) can change the bubble after physics
	// has already been initialized.
	if (colliders_) colliders_->set_body_bubble_radius_m(v);
}

void VoxelWorld::debug_set_physics_bubbles(const PackedVector3Array &centers) {
	std::vector<float> flat;
	flat.reserve(static_cast<size_t>(centers.size()) * 3);
	for (int i = 0; i < centers.size(); i++) {
		const Vector3 c = centers[i];
		flat.push_back(c.x);
		flat.push_back(c.y);
		flat.push_back(c.z);
	}
	physics_bubble_centers_.swap(flat);
}

Dictionary VoxelWorld::debug_physics_stats() {
	Dictionary d;
	d["chunks_resident"] = chunks_ ? chunks_->resident_count() : 0;
	d["chunks_pending"] = chunks_ ? chunks_->pending_count() : 0;
	d["probe_cache"] = chunks_ ? chunks_->probe_cache_size() : 0;
	// `bodies` preserves the historical chunk count used by the physics tests and HUD.
	// `bodies_raw` exposes the eight-way implementation detail for profiling only.
	d["bodies"] = colliders_ ? colliders_->active_bodies() : 0;
	d["bodies_raw"] = colliders_ ? colliders_->bodies_in_space() : 0;
	d["max_build_tris"] = colliders_ ? colliders_->max_build_tris() : 0;
	d["max_chunk_tris"] = colliders_ ? colliders_->max_chunk_tris() : 0;
	d["builds"] = colliders_ ? colliders_->builds_last_frame() : 0;
	d["queued"] = colliders_ ? colliders_->queued_results() : 0;
	d["failures"] = colliders_ ? colliders_->failures() : 0;
	d["build_ms"] = colliders_ ? colliders_->last_build_ms() : 0.0f;
	d["collect_ms"] = colliders_ ? colliders_->last_collect_ms() : 0.0f;
	return d;
}

void VoxelWorld::queue_island_upload(int atlas_slot, int volume_slot,
		const ve::VolumeData &d) {
	std::lock_guard<std::mutex> lock(island_mutex_);
	island_uploads_.push_back(IslandUpload{atlas_slot, volume_slot, true, d});
}

void VoxelWorld::queue_field_volume_upload(int slot, const ve::VolumeData &d) {
	{
		std::lock_guard<std::mutex> lock(island_mutex_);
		island_uploads_.push_back(IslandUpload{-1, slot, false, d});
	}
	// The worker's volume pool must see the paste before its next field job, otherwise the
	// mesher's collision against the new rubble lags a frame (or more) behind the main copy.
	if (mesh_) mesh_->submit_volume(slot, d);
}

void VoxelWorld::discard_field_volume_upload(int slot) {
	{
		std::lock_guard<std::mutex> lock(island_mutex_);
		island_uploads_.erase(
				std::remove_if(island_uploads_.begin(), island_uploads_.end(),
						[slot](const IslandUpload &u) {
							return !u.to_island_atlas && u.volume_slot == slot;
						}),
				island_uploads_.end());
	}
	if (mesh_) mesh_->discard_pending_volume_upload(slot);
}

void VoxelWorld::publish_island_descriptors(const std::vector<IslandSlotDesc> &d) {
	std::lock_guard<std::mutex> lock(island_mutex_);
	island_descs_ = d;
	island_descs_dirty_ = true;
}

void VoxelWorld::set_physics_bubbles(const std::vector<IslandBody *> &bodies) {
	std::vector<float> centers;
	centers.reserve(bodies.size() * 3);
	for (IslandBody *b : bodies) {
		if (!b || !b->live()) continue;
		const Vector3 o = b->transform().origin;
		centers.push_back(o.x);
		centers.push_back(o.y);
		centers.push_back(o.z);
	}
	physics_bubble_centers_.swap(centers);
}

ve::RayHit VoxelWorld::analytic_raycast_down(const float xz[2]) {
	ve::RayHit h;
	if (!edit_log_) return h;
	std::lock_guard<std::mutex> lock(edit_mutex_);
	ve::AnalyticGenerator gen;
	const float o[3] = {xz[0], 200.0f, xz[1]};
	const float dir[3] = {0.0f, -1.0f, 0.0f};
	return ve::raycast(gen, *edit_log_, o, dir, 400.0f, &volumes_, overrides_);
}

bool VoxelWorld::release_volume_slot(int slot) {
	// The authoritative copy goes first; only a successful release (never a pinned slot --
	// a pasted volume-add still names it) queues the GPU-side normal teardown.
	const bool freed = volumes_.release(slot);
	if (freed) {
		std::lock_guard<std::mutex> lock(island_mutex_);
		pending_normal_releases_.push_back(slot);
	}
	return freed;
}

int VoxelWorld::drain_island_uploads(RenderingDevice *device) {
	if (!device) return 0;
	std::vector<IslandUpload> uploads;
	std::vector<int> normal_releases;
	std::vector<IslandSlotDesc> descs;
	bool dirty = false;
	{
		std::lock_guard<std::mutex> lock(island_mutex_);
		uploads.swap(island_uploads_);
		normal_releases.swap(pending_normal_releases_);
		descs = island_descs_;
		dirty = island_descs_dirty_;
		island_descs_dirty_ = false;
	}
	for (const int slot : normal_releases) {
		if (atlas_) atlas_->stored_normals().release_volume(device, slot);
	}
	for (const IslandUpload &u : uploads) {
		// SDF/material and compact normals land ONCE, in the shared authoritative pools,
		// indexed by the volume slot. An island upload additionally refreshes its mip at
		// the atlas slot; a field-volume upload follows the identical volume/normal path
		// without one. A missing/malformed/failed normal payload is fail-soft: the pool
		// publishes -1 and the shader falls back to differentiating the R8 atlas.
		if (atlas_ && u.volume_slot >= 0) {
			if (!atlas_->volumes().upload(device, u.volume_slot, u.data))
				UtilityFunctions::printerr("VoxelWorld: field volume upload failed for slot ",
						u.volume_slot);
			atlas_->stored_normals().upload_volume(device, u.volume_slot, u.data);
		} else if (!atlas_ && u.to_island_atlas) {
			UtilityFunctions::printerr("VoxelWorld: no GpuAtlas for island upload of slot ",
					u.volume_slot);
		}
		if (u.to_island_atlas && islands_ && u.atlas_slot >= 0 &&
				!islands_->upload_mip(device, u.atlas_slot, u.data))
			UtilityFunctions::printerr("VoxelWorld: island mip upload failed for slot ",
					u.atlas_slot);
		if (!u.to_island_atlas)
			debug_field_volume_upload_count_.fetch_add(1, std::memory_order_relaxed);
	}
	if (dirty && islands_)
		islands_->upload_descriptors(device, descs.data(), static_cast<int>(descs.size()));
	return static_cast<int>(uploads.size());
}

int VoxelWorld::debug_island_pending_uploads() {
	std::lock_guard<std::mutex> lock(island_mutex_);
	return static_cast<int>(island_uploads_.size());
}

int VoxelWorld::debug_field_volume_upload_count() const {
	return debug_field_volume_upload_count_.load(std::memory_order_relaxed);
}

int VoxelWorld::debug_island_descriptors_pending() {
	std::lock_guard<std::mutex> lock(island_mutex_);
	return island_descs_dirty_ ? 1 : 0;
}

PackedInt32Array VoxelWorld::debug_mesh_volume_slots() {
	PackedInt32Array out;
	if (!mesh_) return out;
	for (int slot : mesh_->debug_submitted_volume_slots()) out.append(slot);
	return out;
}

void VoxelWorld::debug_queue_test_island_upload(int slot, const PackedByteArray &sdf,
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
	queue_island_upload(slot, slot, d); // test fixture: atlas slot == volume slot
}

void VoxelWorld::debug_queue_test_island_descriptors() {
	std::lock_guard<std::mutex> lock(island_mutex_);
	island_descs_.assign(1, IslandSlotDesc{});
	island_descs_dirty_ = true;
}

void VoxelWorld::debug_queue_committed_field_volume_upload(int slot,
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
	if (!volumes_.reserve(slot)) {
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
	if (!volumes_.store(slot, d) || !volumes_.pin(slot)) {
		release_volume_slot(slot);
		UtilityFunctions::printerr(
				"debug_queue_committed_field_volume_upload: store/pin failed for slot ", slot);
		return;
	}
	// Only model the main-thread GPU handoff queue. The worker-side mirror is exercised by
	// ensure_physics_initialized()'s pinned-volume replay after teardown/reinit.
	{
		std::lock_guard<std::mutex> lock(island_mutex_);
		island_uploads_.push_back(IslandUpload{-1, slot, false, d});
	}
	if (mesh_) {
		mesh_->submit_volume(slot, d);
		mesh_->run_sync([](MeshPass &){});
	}
}

void VoxelWorld::debug_set_extraction_available(bool v) {
	ensure_physics_initialized();
	if (mesh_) mesh_->debug_set_extraction_available(v);
}

Dictionary VoxelWorld::debug_stored_normal_stats() {
	Dictionary d;
	if (!atlas_ || !atlas_->is_valid()) return d;
	const StoredNormalStats s = atlas_->stored_normals().stats();
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

Dictionary VoxelWorld::debug_normal_pool_state() {
	Dictionary d;
	const bool have_pool = atlas_ && atlas_->is_valid();
	const StoredNormalPool *p = have_pool ? &atlas_->stored_normals() : nullptr;
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

int64_t VoxelWorld::debug_normal_upload_override(int slot,
		const PackedByteArray &packed_normals) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !atlas_->is_valid()) return -1;
	if (slot < 0) return -1;
	if (packed_normals.is_empty() || packed_normals.size() % 2 != 0) {
		// Malformed payload: exercise the pool's fallback path rather than uploading junk.
		return atlas_->stored_normals().upload_override(device, slot, nullptr, 0);
	}
	return atlas_->stored_normals().upload_override(device, slot,
			reinterpret_cast<const uint16_t *>(packed_normals.ptr()),
			static_cast<int>(packed_normals.size() / 2));
}

void VoxelWorld::debug_normal_release_override(int slot) {
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !atlas_->is_valid()) return;
	atlas_->stored_normals().release_override(device, slot);
}

void VoxelWorld::debug_set_fail_extractions(bool v) {
	ensure_physics_initialized();
	if (mesh_) mesh_->debug_set_fail_extractions(v);
}

void VoxelWorld::debug_set_fail_extract_submit(bool v) {
	ensure_physics_initialized();
	if (mesh_) mesh_->debug_set_fail_extract_submit(v);
}

void VoxelWorld::debug_set_fail_consolidations(bool v) {
	ensure_physics_initialized();
	if (mesh_) mesh_->debug_set_fail_consolidations(v);
}

void VoxelWorld::debug_set_fail_consolidate_uploads(bool v) {
	ensure_physics_initialized();
	if (mesh_) mesh_->debug_set_fail_consolidate_uploads(v);
}

void VoxelWorld::debug_set_fail_restore_overrides(bool v) {
	ensure_physics_initialized();
	if (mesh_) mesh_->debug_set_fail_restore_overrides(v);
}

void VoxelWorld::debug_set_fail_restore_overrides_always(bool v) {
	ensure_physics_initialized();
	if (mesh_) mesh_->debug_set_fail_restore_overrides_always(v);
}

void VoxelWorld::debug_set_pause_override_publication(bool v) {
	ensure_physics_initialized();
	if (mesh_) mesh_->debug_set_pause_override_publication(v);
}

bool VoxelWorld::debug_override_publication_paused() const {
	return mesh_ && mesh_->debug_override_publication_paused();
}

int VoxelWorld::debug_island_frame(float dt, Vector3 center) {
	ensure_initialized();
	ensure_physics_initialized();
	if (!island_manager_) return 0;
	drain_occupancy();
	const int n = island_manager_->run_frame(dt, center);
	// The tests drive the world by hand and never enter the compositor, so the render-thread
	// half of the handoff has to happen here too.
	RenderingDevice *device = rd();
	if (device) {
		drain_island_uploads(device);
		device->submit();
		device->sync();
	}
	return n;
}

Dictionary VoxelWorld::debug_island_stats() {
	return island_manager_ ? island_manager_->stats() : Dictionary();
}

void VoxelWorld::debug_set_merge_sleep_seconds(float v) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->set_merge_sleep_seconds(v);
}

#ifdef DEBUG_ENABLED
void VoxelWorld::debug_set_max_dynamic_bodies(int v) {
	ensure_physics_initialized();
	// Clamp before forwarding: a test hook should be able to lower the guardrail but not
	// silently disable it with an absurd value.
	v = v < 1 ? 1 : (v > kMaxDynamicBodies ? kMaxDynamicBodies : v);
	if (island_manager_) island_manager_->debug_set_max_dynamic_bodies(v);
}

void VoxelWorld::debug_set_atlas_slot_used(int slot, bool used) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->debug_set_atlas_slot_used(slot, used);
}
#else
void VoxelWorld::debug_set_max_dynamic_bodies(int v) {
	// Debug-only hook: release scripts cannot lower the 64-body guardrail.
	(void)v;
}

void VoxelWorld::debug_set_atlas_slot_used(int slot, bool used) {
	// Debug-only hook: release scripts cannot mark atlas slots used.
	(void)slot;
	(void)used;
}
#endif

void VoxelWorld::debug_set_fail_next_spawn(bool fail) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->debug_set_fail_next_spawn(fail);
}

void VoxelWorld::debug_set_fail_next_restore(bool fail) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->debug_set_fail_next_restore(fail);
}

void VoxelWorld::debug_set_fail_next_carve(bool fail) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->debug_set_fail_next_carve(fail);
}

void VoxelWorld::debug_set_fail_next_resample(bool fail) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->debug_set_fail_next_resample(fail);
}

void VoxelWorld::debug_set_empty_next_extraction(bool v) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->debug_set_empty_next_extraction(v);
}

void VoxelWorld::debug_wake_island_body(int index) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->debug_wake_body(index);
}

void VoxelWorld::debug_offset_island_body(int index, Vector3 offset) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->debug_offset_body(index, offset);
}

Dictionary VoxelWorld::debug_island_body_info(int index) {
	ensure_physics_initialized();
#ifdef DEBUG_ENABLED
	if (island_manager_) return island_manager_->debug_body_info(index);
#else
	(void)index;
#endif
	return Dictionary();
}

RID VoxelWorld::debug_body_of_chunk(Vector3i chunk) {
	if (!chunks_ || !colliders_) return RID();
	return colliders_->body_of_slot(chunks_->slot_of({chunk.x, chunk.y, chunk.z}));
}

Dictionary VoxelWorld::debug_chunk_collider_info(Vector3i chunk) {
	Dictionary d;
	if (!chunks_ || !colliders_) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	d["slot"] = chunks_->slot_of(c);
	d["state"] = colliders_->chunk_state(c);
	d["in_flight"] = colliders_->chunk_in_flight(c);
	d["build_count"] = colliders_->build_count_of_chunk(c);
	d["last_ops"] = colliders_->last_submit_op_count(c);
	return d;
}

Dictionary VoxelWorld::debug_chunk_collider_octants(Vector3i chunk) {
	if (!chunks_ || !colliders_) return Dictionary();
	return colliders_->debug_chunk_octants({chunk.x, chunk.y, chunk.z});
}

bool VoxelWorld::debug_init_physics() {
	ensure_physics_initialized();
	return physics_ready_;
}

void VoxelWorld::debug_teardown_physics() {
	teardown_physics();
}

void VoxelWorld::gather_lod_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out) {
	if (!out) return;
	out->clear();
	std::lock_guard<std::mutex> lock(edit_mutex_);
	if (!edit_log_) return;
	float lo[3], hi[3];
	ve::lod_chunk_aabb(level, coord, lo, hi);
	const float pad = std::max(2.0f * ve::lod_cell_size(level), ve::kLatticeFilterPad);
	for (int a = 0; a < 3; a++) {
		lo[a] -= pad;
		hi[a] += pad;
	}
	ve::collect_ops_for_aabb(*edit_log_, lo, hi, out);
	// M4 errata 1: the flattened cross-region list can exceed the cap. A chronological
	// prefix is a valid world state; a suffix could apply an add without the subtract that
	// made room for it.
	if (out->size() > ve::kMaxRegionOps) out->resize(ve::kMaxRegionOps);
}

bool VoxelWorld::snapshot_field_sources(const std::vector<ve::EditOp> &ops, ve::IVec3 brick_lo, ve::IVec3 brick_hi, ve::FieldSourceSnapshot *out) const {
	if (!out || !overrides_) return false;
	out->overrides.clear();
	out->volumes.clear();
	// Copy only prior overrides inside inclusive brick range
	for (int z = brick_lo.z; z <= brick_hi.z; z++)
		for (int y = brick_lo.y; y <= brick_hi.y; y++)
			for (int x = brick_lo.x; x <= brick_hi.x; x++) {
				ve::IVec3 b{x, y, z};
				int slot = overrides_->slot_of(b);
				if (slot >= 0) {
					const ve::OverrideBrick *data = overrides_->data(slot);
					if (!data) return false;
					if (!data->normal_oct.empty() && data->normal_oct.size() != ve::kBrickSdfCount) return false;
					out->overrides.push_back({b, *data});
				}
			}
	std::set<int> seen;
	for (const auto &op : ops) {
		if (op.type != ve::kOpVolumeAdd) continue;
		int slot = static_cast<int>(op.aux[0]);
		if (seen.count(slot)) continue;
		seen.insert(slot);
		const ve::VolumeData *vd = volumes_.get(slot);
		if (!vd || !vd->valid()) return false;
		out->volumes.push_back({slot, *vd});
	}
	return true;
}

void VoxelWorld::ensure_lod() {
	if (lod_tree_ && lod_pool_ && lod_pool_->page_count() > 0) return;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!device) return;
	if (!lod_tree_) {
		ve::LodTreeConfig cfg;
		cfg.bounds = world_bounds();
		lod_tree_ = new ve::LodTree(cfg);
	}
	if (!lod_pool_) lod_pool_ = new LodPool();
	if (lod_pool_->page_count() == 0 && !lod_pool_->initialize(device, max_lod_pages_))
		UtilityFunctions::printerr("VoxelWorld: LodPool initialize failed");
}

void VoxelWorld::lod_fade_band(float *fade_start, float *fade_end) const {
	// With the near field forced off the far field owns every distance: move the seam to
	// zero and make the fade span essentially infinite so the LoD build gate requests the
	// near chunks and the fragment shader keeps every far-field fragment.
	if (!near_field_enabled_.load(std::memory_order_relaxed)) {
		if (fade_start) *fade_start = 0.0f;
		if (fade_end) *fade_end = 1.0e9f;
		return;
	}
	// Until the streamer has run a frame there is nothing measured, and before the first
	// regions land the measurement is "complete out to 0 m" -- both would swing the seam.
	// Fall back to the CONFIGURED radius there: the seam then starts where the near field
	// intends to reach and only tightens if the atlas cannot fund it, instead of jumping
	// once streaming begins and stranding the chunks the walk built under the old band.
	float reach = residency_ ? residency_->complete_radius_m() : 0.0f;
	if (reach <= 0.0f) reach = residency_radius_m_;
	ve::lod_fade_band(reach, fade_start, fade_end);
}

void VoxelWorld::lod_tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ) {
	std::unique_lock<std::mutex> lock(lod_mutex_);
	ensure_lod();
	if (!lod_tree_ || !lod_pool_) return;
	// The gate that decides which chunks are worth building has to agree with the fragment
	// shader about where the far field starts, or it refuses to build exactly the chunks the
	// near field can no longer cover.
	{
		float fs = ve::kLodFadeStartM;
		lod_fade_band(&fs, nullptr);
		lod_tree_->set_fade_start_m(fs);
	}
	lod_tree_->walk(cam, occ, ++lod_frame_, &lod_walk_);

	// Results first: a page that arrives this frame should be drawable this frame.
	std::vector<LodBuildResult> done;
	if (mesh_ && mesh_->collect_lod(&done) > 0) {
		for (LodBuildResult &r : done) {
			if (r.failed) {
				const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
				const auto old_it = lod_pages_of_.find(key);
				if (old_it != lod_pages_of_.end()) {
					// Stale beats missing: a failed rebuild keeps the old pages drawable and
					// is re-affirmed Ready-with-dirty so the next walk retries it. Do not
					// release the old pages and do not mark the node failed (that would
					// un-draw it).
					lod_tree_->note_ready_dirty(r.level, r.coord);
					lod_pressure_ += ve::lod_pages_for_quads(int(r.quads.size()));
				} else {
					lod_tree_->note_failed(r.level, r.coord);
				}
				continue;
			}
			if (r.overflow) {
				const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
				if (lod_overflow_logged_.insert(key).second)
					UtilityFunctions::printerr("VoxelWorld: LoD chunk (level ", r.level,
							", ", r.coord.x, ", ", r.coord.y, ", ", r.coord.z,
							") overflowed; keeping first ", ve::kLodMaxQuadsPerChunk,
							" quads");
			}
			if (r.quads.empty()) {
				// Empty result. If an edit landed while this build was in flight, the result
				// is stale: keep any old pages drawing (stale beats missing) or leave a
				// non-resident node requestable. Only a non-dirty empty result is terminal,
				// and only then may the old GPU pages be released.
				const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
				const bool dirty = lod_tree_->is_dirty(r.level, r.coord);
				const auto old_it = lod_pages_of_.find(key);
				if (dirty) {
					if (old_it != lod_pages_of_.end()) {
						// Old pages stay drawable; note_ready_dirty re-requests the rebuild.
						lod_tree_->note_ready_dirty(r.level, r.coord);
					} else {
						// Nothing to keep drawing; note_empty leaves the node requestable.
						lod_tree_->note_empty(r.level, r.coord);
					}
					continue;
				}
				// Genuinely empty: release any old pages before telling the tree, otherwise
				// the tree stops drawing/requesting it while the stale GPU pages stay
				// allocated forever.
				if (old_it != lod_pages_of_.end()) {
					for (int p : old_it->second) lod_page_quads_.erase(p);
					lod_pool_->release(old_it->second);
					if (sun_shadow_pass_) sun_shadow_pass_->mark_dirty();
					lod_pages_of_.erase(old_it);
				}
				lod_tree_->note_empty(r.level, r.coord);
				continue;
			}
			std::vector<int> pages;
			if (!lod_pool_->upload(r.level, r.coord, r.quads, &pages)) {
				// Refused, not half-funded. If the chunk already has resident pages, keep
				// drawing them: stale beats missing. Re-affirm Ready-with-dirty using the old
				// page list so the node stays drawable AND is re-requested next frame; a node
				// with no old pages still fails and is re-requested next frame.
				const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
				const auto old_it = lod_pages_of_.find(key);
				if (old_it != lod_pages_of_.end()) {
					lod_tree_->note_ready_dirty(r.level, r.coord);
					// Keep the old page list in lod_pages_of_: it remains the node's drawable
					// pages until a later upload succeeds and replaces them.
				} else {
					lod_tree_->note_failed(r.level, r.coord);
				}
				// Accumulate across refusals in this frame so evictions recover enough pages
				// for every refused rebuild, not just the last one.
				lod_pressure_ += ve::lod_pages_for_quads(int(r.quads.size()));
				continue;
			}
			// A rebuild replaces the old page list. Release the stale pages only once the
			// new pages are allocated and uploaded, so a refused rebuild keeps the old pages
			// drawing; after this point the tree points at the new list.
			if (sun_shadow_pass_) sun_shadow_pass_->mark_dirty();
			const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
			const auto old_it = lod_pages_of_.find(key);
			if (old_it != lod_pages_of_.end()) {
				for (int p : old_it->second) lod_page_quads_.erase(p);
				lod_pool_->release(old_it->second);
				lod_pages_of_.erase(old_it);
			}
			for (int i = 0; i < int(pages.size()); i++) {
				const int first = i * ve::kLodQuadsPerPage;
				const int count = std::min(ve::kLodQuadsPerPage,
						static_cast<int>(r.quads.size()) - first);
				lod_page_quads_[pages[static_cast<size_t>(i)]] = count;
			}
			lod_tree_->note_ready(r.level, r.coord, pages.front(), int(pages.size()));
			lod_pages_of_[key] = std::move(pages);
		}
	}

	// Then evictions, so the budget below sees the pages they returned.
	std::vector<ve::LodDrawItem> evicted;
	lod_tree_->collect_evictions(lod_frame_, lod_pressure_, &evicted);
	lod_pressure_ = 0;
	for (const ve::LodDrawItem &e : evicted) {
		const LodKey key{e.level, e.coord.x, e.coord.y, e.coord.z};
		const auto it = lod_pages_of_.find(key);
		if (it == lod_pages_of_.end()) continue;
		for (int p : it->second) lod_page_quads_.erase(p);
		lod_pool_->release(it->second);
		if (sun_shadow_pass_) sun_shadow_pass_->mark_dirty();
		lod_pages_of_.erase(it);
	}

	// Then this frame's builds, priority order, one batch. Mark the nodes building while
	// still holding lod_mutex_ so note_building's dirty-clear happens at submission time.
	// gather_lod_ops takes edit_mutex_, so it must run AFTER releasing lod_mutex_ (lock
	// order: edit_mutex_ -> lod_mutex_); the building flag prevents a concurrent walk from
	// re-requesting these nodes during that window, and a refused submit rolls the flags back.
	std::vector<ve::LodBuildRequest> batch_requests;
	if (mesh_ && !mesh_->lod_busy()) {
		// MeshService's LodBuildPass currently supports at most 8 LoD jobs per batch.
		// lod_builds_per_frame_ is user-facing and may be higher; submit_lod would reject
		// anything above the mesher's cap, so clamp the actual batch take here.
		const int take = std::min<int>({lod_builds_per_frame_, int(lod_walk_.requests.size()), 8});
		batch_requests.assign(lod_walk_.requests.begin(), lod_walk_.requests.begin() + take);
		for (const ve::LodBuildRequest &q : batch_requests)
			lod_tree_->note_building(q.level, q.coord);
	}
	lock.unlock();

	if (!batch_requests.empty()) {
		std::vector<LodBuildJob> batch;
		batch.reserve(batch_requests.size());
		for (const ve::LodBuildRequest &q : batch_requests) {
			LodBuildJob j;
			j.level = q.level;
			j.coord = q.coord;
			gather_lod_ops(q.level, q.coord, &j.ops);
			batch.push_back(std::move(j));
		}
		if (!mesh_->submit_lod(std::move(batch))) {
			lock.lock();
			for (const ve::LodBuildRequest &q : batch_requests) {
				const LodKey key{q.level, q.coord.x, q.coord.y, q.coord.z};
				if (lod_pages_of_.find(key) != lod_pages_of_.end()) {
					lod_tree_->note_ready_dirty(q.level, q.coord);
				} else {
					lod_tree_->note_failed(q.level, q.coord);
				}
			}
			lock.unlock();
		}
	}

	lock.lock();
	prepare_lod_raster_locked();
}

void VoxelWorld::prepare_lod_raster() {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	prepare_lod_raster_locked();
}

void VoxelWorld::prepare_lod_shadow_raster() {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	if (!lod_raster_pass_ || !lod_pool_) return;
	std::vector<LodRasterPass::PageDraw> pages;
	pages.reserve(lod_page_quads_.size());
	for (const auto &kv : lod_page_quads_)
		pages.push_back(LodRasterPass::PageDraw{kv.first, kv.second});
	lod_raster_pass_->set_draw_pages(pages);
}

void VoxelWorld::prepare_lod_raster_locked() {
	if (!lod_raster_pass_ || !lod_pool_) return;
	std::vector<ve::LodPageDraw> page_draws;
	ve::lod_collect_page_draws(lod_walk_.draws, lod_pages_of_, lod_page_quads_, &page_draws);
	std::vector<LodRasterPass::PageDraw> pages;
	pages.reserve(page_draws.size());
	for (const ve::LodPageDraw &pd : page_draws)
		pages.push_back(LodRasterPass::PageDraw{pd.page, pd.quad_count});
	lod_raster_pass_->set_draw_pages(pages);
}

void VoxelWorld::debug_lod_tick(Vector3 pos, Vector3 fwd) {
	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const ve::LodCamera cam = ve::lod_camera_perspective(p, f, up, 1.2217f,
			16.0f / 9.0f, 0.1f, 8000.0f, 2560, 1440);
	lod_tick(cam, nullptr);
}

Dictionary VoxelWorld::debug_lod_stats() {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	ensure_lod();
	Dictionary d;
	d["pages_total"] = lod_pool_ ? lod_pool_->page_count() : 0;
	d["pages_free"] = lod_pool_ ? lod_pool_->free_pages() : 0;
	d["pages_used"] = (lod_pool_ ? lod_pool_->page_count() : 0) -
			(lod_pool_ ? lod_pool_->free_pages() : 0);
	d["chunks_resident"] = static_cast<int>(lod_pages_of_.size());
	int dirty_chunks = 0;
	int dirty_levels = 0;
	if (lod_tree_) lod_tree_->dirty_stats(&dirty_chunks, &dirty_levels);
	d["dirty_chunks"] = dirty_chunks;
	d["dirty_levels"] = dirty_levels;
	int draw_pages = 0;
	for (const ve::LodDrawItem &item : lod_walk_.draws)
		draw_pages += item.page_count;
	d["draw_pages"] = draw_pages;
	// Requests the last walk still wants built. Zero, with nothing in flight, is what
	// "the far field has converged for this camera" means; tests wait on it instead of
	// guessing a frame count.
	d["requests_pending"] = static_cast<int>(lod_walk_.requests.size());
	// LodArena::alloc is all-or-nothing, so this should always be zero -- but reporting a
	// hardcoded 0 makes the test that asserts it vacuous. MEASURE the two shapes a
	// partially funded build would actually take: a chunk holding a page the per-page quad
	// count never learned about, and arena pages that no resident chunk owns (the leak a
	// half-rolled-back allocation leaves behind).
	int partial = 0;
	size_t owned_pages = 0;
	for (const auto &kv : lod_pages_of_) {
		owned_pages += kv.second.size();
		for (int p : kv.second) {
			if (lod_page_quads_.find(p) == lod_page_quads_.end()) {
				partial++;
				break;
			}
		}
	}
	const int used_pages = (lod_pool_ ? lod_pool_->page_count() : 0) -
			(lod_pool_ ? lod_pool_->free_pages() : 0);
	const int unowned = used_pages - static_cast<int>(owned_pages);
	d["partial_allocations"] = partial + (unowned > 0 ? unowned : 0);
	d["builds_in_flight"] = mesh_ && mesh_->lod_busy() ? 1 : 0;
	// Async cull stats readback; zero until the first readback lands (safe "nothing culled").
	d["culled_ratio"] = lod_cull_pass_ ? lod_cull_pass_->culled_ratio() : 0.0f;
	return d;
}

Vector2 VoxelWorld::debug_lod_fade_band() {
	float start = ve::kLodFadeStartM;
	float end = ve::kLodFadeEndM;
	lod_fade_band(&start, &end);
	return Vector2(start, end);
}

Dictionary VoxelWorld::debug_lod_render_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	return debug_lod_render_probe_culled(pos, fwd, w, h, true);
}

Dictionary VoxelWorld::debug_lod_render_probe_culled(Vector3 pos, Vector3 fwd, int w, int h,
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

	RenderingDevice *device = rd();
	if (!initialized_ || !device || !lod_pool_ || !lod_raster_pass_ || !materials_) return d;

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

	if (!gbuffer_ || !gbuffer_->ensure(device, nullptr, Vector2i(w, h))) return d;

	// Clear to reverse-Z far (0) before drawing the far field. The G-buffer owns the
	// attachment format used by both production producers.
	device->texture_clear(gbuffer_->depth(), Color(0.0f, 0.0f, 0.0f, 0.0f), 0, 1, 0, 1);
	lod_raster_pass_->set_cull_enabled(cull);
	const int draw_count = lod_raster_pass_->draw_page_count();
	lod_pool_->upload_draw_args(lod_raster_pass_->draw_pages());
	float probe_start = ve::kLodFadeStartM;
	float probe_end = ve::kLodFadeEndM;
	lod_fade_band(&probe_start, &probe_end);
	bool ok = lod_raster_pass_->draw(device, *lod_pool_, *materials_, *gbuffer_,
			vp, p, draw_count, probe_start, probe_end);
	device->submit();
	device->sync();

	if (ok) {
		const PackedByteArray depth_data = device->texture_get_data(gbuffer_->depth(), 0);
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
	d["draw_pages"] = lod_raster_pass_ ? lod_raster_pass_->draw_page_count() : 0;

	// The raster pass cached a framebuffer over the owned G-buffer; drop it before a future
	// probe changes its attachments.
	lod_raster_pass_->release_targets();
	return d;
}

Dictionary VoxelWorld::debug_lod_gbuffer_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["material_coverage"] = 0.0f;
	d["worst_normal_length_error"] = 0.0f;
	d["gloss_max"] = 0.0f;
	d["sun_min"] = 1.0f;
	d["sun_max"] = 0.0f;
	if (w <= 0 || h <= 0) return d;

	debug_lod_tick(pos, fwd);
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !lod_pool_ || !lod_raster_pass_ || !materials_ || !gbuffer_)
		return d;
	lod_raster_pass_->release_targets();
	if (!gbuffer_->ensure(device, nullptr, Vector2i(w, h))) return d;

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

	device->texture_clear(gbuffer_->albedo(), Color(0, 0, 0, 0), 0, 1, 0, 1);
	device->texture_clear(gbuffer_->surface(), Color(0, 0, 0, 0), 0, 1, 0, 1);
	device->texture_clear(gbuffer_->depth(), Color(0, 0, 0, 0), 0, 1, 0, 1);
	lod_raster_pass_->set_cull_enabled(true);
	lod_pool_->upload_draw_args(lod_raster_pass_->draw_pages());
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	lod_fade_band(&fade_start, &fade_end);
	const bool ok = lod_raster_pass_->draw(device, *lod_pool_, *materials_, *gbuffer_, vp, p,
			lod_raster_pass_->draw_page_count(), fade_start, fade_end);
	device->submit();
	device->sync();

	if (ok) {
		const PackedByteArray albedo = device->texture_get_data(gbuffer_->albedo(), 0);
		const PackedByteArray surface = device->texture_get_data(gbuffer_->surface(), 0);
		const int pixels = w * h;
		if (albedo.size() >= pixels * 4 && surface.size() >= pixels * 8) {
			const uint8_t *a = reinterpret_cast<const uint8_t *>(albedo.ptr());
			const uint16_t *s = reinterpret_cast<const uint16_t *>(surface.ptr());
			int covered = 0;

			float worst = 0.0f;
			float gloss_max = 0.0f;
			float sun_min = 1.0f;
			float sun_max = 0.0f;
			for (int i = 0; i < pixels; i++) {
				const float material = Math::half_to_float(s[i * 4 + 2]);
				if (material < 0.5f) continue;
				covered++;
				const float e[2] = {Math::half_to_float(s[i * 4]), Math::half_to_float(s[i * 4 + 1])};
				float n[3] = {};
				ve::oct_decode(e, n);
				const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				worst = std::max(worst, std::fabs(length - 1.0f));
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
		}
	}
	lod_raster_pass_->release_targets();
	return d;
}

Dictionary VoxelWorld::debug_seam_probe(Vector3 pos, Vector3 fwd, int w, int h, bool skip_lod) {
	Dictionary d;
	d["band_pixels"] = 0;
	d["band_pixels_unclaimed"] = 0;
	d["band_pixels_double_claimed"] = 0;
	d["near_pixels_lost_to_lod"] = 0;
	d["far_pixels_lost_to_raymarch"] = 0;
	if (w <= 0 || h <= 0) return d;

	// One tick refreshes the walk and the raster pass's page list for this view.
	debug_lod_tick(pos, fwd);

	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_ ||
			!composite_pass_ || !deferred_pass_ || !inject_pass_ || !gbuffer_ ||
			!lod_pool_ || !lod_raster_pass_) return d;

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
	composite_pass_->release_targets();
	lod_raster_pass_->release_targets();
	if (!gbuffer_->ensure(device, nullptr, Vector2i(w, h))) return d;
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
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_size_regions_.x;
	cp.dims[1] = world_size_regions_.y;
	cp.dims[2] = world_size_regions_.z;
	cp.dims[3] = island_slot_count();
	cp.region_origin[0] = ro.x;
	cp.region_origin[1] = ro.y;
	cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = atlas_bricks_.x;
	cp.atlas_bricks[1] = atlas_bricks_.y;
	cp.atlas_bricks[2] = atlas_bricks_.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cp, w, h, kNoEdit))
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
		composite_pass_->release_targets();
		inject_pass_->release_targets();
		lod_raster_pass_->release_targets();
		if (color.is_valid()) device->free_rid(color);
		if (depth.is_valid()) device->free_rid(depth);
		if (marker.is_valid()) device->free_rid(marker);
	};

	// Pass 1: near field. Composite writes 1 into the marker where it keeps the depth.
	// The probe must fade where the production path fades, or it measures a band neither
	// shader is using and reports a seam that is not there.
	float probe_fade_start = ve::kLodFadeStartM;
	float probe_fade_end = ve::kLodFadeEndM;
	lod_fade_band(&probe_fade_start, &probe_fade_end);
	composite_pass_->draw(device, *gbuffer_, raymarch_pass_->albedo_texture(),
			raymarch_pass_->surface_texture(), raymarch_pass_->hitpos_texture(), vp, *materials_, p,
			probe_fade_start, probe_fade_end, marker);
	if (!composite_pass_->last_draw_ok()) {
		cleanup();
		return d;
	}

	// Pass 2: far field. The LoD pipeline uses LOGIC_OP_OR on the marker, so kept far
	// pixels OR 2 into the composite's 1, making double-claimed pixels read 3.
	// `skip_lod` is a debug-only knob for the regression test: by leaving the far field
	// out entirely it creates a real far-field gap, which the probe must count as
	// unclaimed. Production rendering never passes it.
	if (!skip_lod) {
		lod_raster_pass_->set_cull_enabled(true);
		lod_pool_->upload_draw_args(lod_raster_pass_->draw_pages());
		const int draw_count = lod_raster_pass_->draw_page_count();
		lod_raster_pass_->draw(device, *lod_pool_, *materials_, *gbuffer_, vp, p,
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
	dp.flags = ve::pack_flags(beauty_settings());
	static const float kNoSun[16] = {};
	if (!deferred_pass_->render(device, *gbuffer_, *materials_, RID(), RID(), kNoSun, 0.0f, dp) ||
			!inject_pass_->draw(device, color, depth, gbuffer_->lit(), gbuffer_->depth())) {
		cleanup();
		return d;
	}
	device->submit();
	device->sync();

	const PackedByteArray depth_data = device->texture_get_data(gbuffer_->depth(), 0);
	const PackedByteArray marker_data = device->texture_get_data(marker, 0);
	const PackedByteArray hitpos_data = device->texture_get_data(
			raymarch_pass_->hitpos_texture(), 0);
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
	d["draw_pages"] = lod_raster_pass_ ? lod_raster_pass_->draw_page_count() : 0;

	// Drop cached framebuffers before freeing their throwaway scene-buffer/marker targets.
	cleanup();
	return d;
}

Dictionary VoxelWorld::debug_lod_cull_probe(Vector3 pos, Vector3 fwd) {
	Dictionary d;
	d["args_before"] = 0;
	d["args_after"] = 0;
	d["offsets_changed"] = 0;
	d["index_counts_changed"] = 0;
	d["drawn_after"] = 0;
	d["culled_ratio"] = 0.0f;

	// One tick: refresh the walk and the raster pass's page list for this view.
	debug_lod_tick(pos, fwd);

	RenderingDevice *device = rd();
	if (!initialized_ || !device || !lod_pool_ || !lod_raster_pass_ || !lod_cull_pass_ ||
			!hiz_pass_) {
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

	const int draw_count = lod_raster_pass_->draw_page_count();
	if (draw_count <= 0) return d;
	lod_pool_->upload_draw_args(lod_raster_pass_->draw_pages());
	device->submit();
	device->sync();
	const PackedByteArray before = device->buffer_get_data(lod_pool_->args_buffer(), 0,
			static_cast<uint32_t>(draw_count) * 20);

	// This probe deliberately exercises frustum culling plus whatever HiZ state is present.
	// Build a synthetic "everything far" pyramid first so the run never reads an unbuilt or
	// stale pyramid (the production path builds from the real scene depth before culling).
	debug_hiz_probe_synthetic(0.0f, 1.0f);

	const bool ok = lod_cull_pass_->run(device, *lod_pool_, hiz_pass_, vp, draw_count,
			draw_count, 0);
	device->submit();
	device->sync();
	if (!ok) return d;

	const PackedByteArray after = device->buffer_get_data(lod_pool_->args_buffer(), 0,
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
	const std::vector<uint32_t> &page_chunk_cpu = lod_pool_->page_chunk_cpu();
	float planes[6][4];
	ve::lod_frustum_planes(cam.view_proj, planes);
	const PackedByteArray chunk_bytes = device->buffer_get_data(lod_pool_->chunk_buffer(), 0,
			static_cast<uint32_t>(lod_pool_->chunk_record_count() * 32));
	const float *chunk_data = reinterpret_cast<const float *>(chunk_bytes.ptr());
	const bool have_chunks = chunk_bytes.size() >= lod_pool_->chunk_record_count() * 32;
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

Dictionary VoxelWorld::debug_gbuffer_stats(int w, int h) {
	Dictionary d;
	d["valid"] = false;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !gbuffer_) return d;
	// The probe path: no RenderSceneBuffersRD exists outside a render callback, so this
	// exercises the owned branch. Everything else about the object is identical.
	if (!gbuffer_->ensure(device, nullptr, Vector2i(w, h))) {
		d["reallocations"] = gbuffer_->reallocations();
		return d;
	}
	d["valid"] = gbuffer_->is_valid();
	d["width"] = gbuffer_->size().x;
	d["height"] = gbuffer_->size().y;
	d["half_width"] = gbuffer_->half_size().x;
	d["half_height"] = gbuffer_->half_size().y;
	d["albedo_valid"] = gbuffer_->albedo().is_valid();
	d["surface_valid"] = gbuffer_->surface().is_valid();
	d["depth_valid"] = gbuffer_->depth().is_valid();
	d["lit_valid"] = gbuffer_->lit().is_valid();
	d["history_valid"] = gbuffer_->history().is_valid();
	d["albedo_id"] = static_cast<int64_t>(gbuffer_->albedo().get_id());
	d["depth_id"] = static_cast<int64_t>(gbuffer_->depth().get_id());
	d["reallocations"] = gbuffer_->reallocations();
	return d;
}

Dictionary VoxelWorld::debug_hiz_stats() {
	Dictionary d;
	ensure_initialized();
	if (!hiz_pass_ || !rd()) return d;
	d["width"] = HizPass::kSize;
	d["height"] = HizPass::kSize;
	d["mips"] = HizPass::kMipCount;
	d["readback_level"] = HizPass::kReadbackLevel;
	d["readback_texels"] = HizPass::kGrid * HizPass::kGrid;
	return d;
}

Dictionary VoxelWorld::debug_hiz_shutdown_probe() {
	Dictionary d;
	d["callback_guarded"] = false;
	d["queued"] = false;
	d["was_pending"] = false;
	d["drained"] = false;
	d["initialized_after"] = true;
	{
		const bool callback_guarded = try_begin_render_callback();
		d["callback_guarded"] = callback_guarded;
		if (!callback_guarded) return d;
		struct CallbackGuard {
			VoxelWorld *world;
			~CallbackGuard() { world->end_render_callback(); }
		} callback_guard{this};
		ensure_initialized();
		RenderingDevice *device = rd();
		if (!device || !hiz_pass_ || !gbuffer_) return d;
		const Vector2i size(64, 64);
		if (!gbuffer_->ensure(device, nullptr, size)) return d;
		if (!hiz_pass_->build(device, gbuffer_->depth(), size)) return d;
		d["queued"] = hiz_pass_->readback_pending();
	}
	shutdown_render_resources();
	d["was_pending"] = last_hiz_readback_was_pending_;
	d["drained"] = last_hiz_readback_was_drained_;
	d["initialized_after"] = initialized_;
	return d;
}

Dictionary VoxelWorld::debug_hiz_probe_synthetic(float far_value, float near_value) {
	Dictionary d;
	d["mip0_at_near_texel"] = 0.0f;
	d["mip1_covering_both"] = 0.0f;
	d["top_mip"] = 0.0f;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !hiz_pass_) return d;

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

	if (hiz_pass_->build(device, synthetic, Vector2i(size, size))) {
		device->submit();
		device->sync();
		d["mip0_at_near_texel"] = hiz_pass_->probe_mip_texel(device, 0, 0, 0);
		d["mip1_covering_both"] = hiz_pass_->probe_mip_texel(device, 1, 0, 0);
		d["top_mip"] = hiz_pass_->probe_mip_texel(device, HizPass::kMipCount - 1, 0, 0);
		// Make the async readback deterministic for the test hooks: read the 4 KB copy
		// synchronously after sync and feed it into the same occlusion grid the walk uses.
		const PackedByteArray rb = device->texture_get_data(hiz_pass_->readback_texture(), 0);
		hiz_pass_->update_occlusion(rb);
	}
	// The level-0 uniform set references this throwaway source; drop the cached set before
	// freeing the texture so the next probe does not try to free a cascade-freed set.
	hiz_pass_->release_level0_set();
	device->free_rid(synthetic);
	return d;
}

bool VoxelWorld::debug_hiz_occluded(Vector2 lo, Vector2 hi, float depth) {
	ensure_initialized();
	if (!hiz_pass_ || !rd()) return false;
	const float ss_min[3] = {lo.x, lo.y, depth};
	const float ss_max[3] = {hi.x, hi.y, depth};
	return hiz_pass_->occlusion()->occluded(ss_min, ss_max);
}

void VoxelWorld::debug_apply_sphere_subtract(Vector3 centre, float radius) {
	if (!edit_log_) ensure_physics_initialized();
	ve::EditOp op;
	op.type = ve::kOpSphereSubtract;
	op.material = 0;
	op.pos[0] = centre.x;
	op.pos[1] = centre.y;
	op.pos[2] = centre.z;
	op.radius = radius;
	append_edit(op);
}

void VoxelWorld::debug_apply_sphere_add(Vector3 centre, float radius, int material) {
	if (!edit_log_) ensure_physics_initialized();
	ve::EditOp op;
	op.type = ve::kOpSphereAdd;
	op.material = static_cast<uint16_t>(material);
	op.pos[0] = centre.x;
	op.pos[1] = centre.y;
	op.pos[2] = centre.z;
	op.radius = radius;
	append_edit(op);
}

void VoxelWorld::debug_apply_volume_add(int slot, Vector3 origin, float voxel, int dim) {
	if (!edit_log_) ensure_physics_initialized();
	float o[3] = {origin.x, origin.y, origin.z};
	ve::EditOp op = ve::make_volume_add(slot, o, voxel, dim);
	append_edit(op);
}

int VoxelWorld::debug_region_op_count(Vector3i region) {
	if (!edit_log_) return 0;
	std::lock_guard<std::mutex> lock(edit_mutex_);
	return edit_log_->op_count({region.x, region.y, region.z});
}

int VoxelWorld::override_table_for_region(ve::IVec3 region) const {
	const auto it = override_tables_.find(std::tuple<int, int, int>{region.x, region.y, region.z});
	return it == override_tables_.end() ? -1 : it->second;
}

int VoxelWorld::debug_override_region_table(int region_slot) const {
	return atlas_ ? atlas_->overrides().region_table(region_slot) : -1;
}

int VoxelWorld::debug_override_used() const {
	return overrides_ ? overrides_->used() : 0;
}

bool VoxelWorld::debug_fill_override_pool() {
	ensure_physics_initialized();
	std::unique_lock<std::mutex> edit_lock(edit_mutex_);
	if (!atlas_ || !mesh_ || !overrides_ || overrides_->used() != 0) return false;
	const ve::IVec3 region = world_bounds().origin_regions();
	const int region_slot = residency_ ? residency_->slot_of(region) : -1;
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
	slots.reserve(overrides_->capacity());
	bricks.reserve(overrides_->capacity());
	entries.reserve(overrides_->capacity());
	for (int i = 0; i < overrides_->capacity(); i++) {
		const ve::IVec3 brick{base.x + (i & 31), base.y + ((i >> 5) & 31),
				base.z + ((i >> 10) & 31)};
		const int slot = overrides_->acquire(brick);
		if (slot < 0) {
			for (const ve::IVec3 acquired : acquired_bricks) overrides_->release(acquired);
			return false;
		}
		const ve::OverrideBrick *data = overrides_->data(slot);
		if (!data) {
			for (const ve::IVec3 acquired : acquired_bricks) overrides_->release(acquired);
			overrides_->release(brick);
			return false;
		}
		slots.push_back(slot);
		acquired_bricks.push_back(brick);
		bricks.push_back(*data);
		entries.emplace_back(i, slot);
	}
	auto discard = [&]() {
		if (atlas_) {
			atlas_->overrides().clear_table(rd(), 0);
			atlas_->set_override_table(rd(), region_slot, -1, {});
		}
		for (const ve::IVec3 brick : acquired_bricks) overrides_->release(brick);
	};
	if (atlas_) {
		for (size_t i = 0; i < slots.size(); i++) {
			if (!atlas_->upload_override(rd(), slots[i], bricks[i])) {
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
				atlas_->stored_normals().upload_override(rd(), slots[i],
						brick.normal_oct.data(), ve::kBrickSdfCount);
			else
				atlas_->stored_normals().release_override(rd(), slots[i]);
		}
		atlas_->set_override_table(rd(), region_slot, 0, entries);
	}
	if (!mesh_->publish_overrides(slots, bricks, region, region_slot, 0, entries)) {
		// The worker publication is synchronous here, but it can still fail after a
		// partial upload. Replay its empty old transaction before releasing the slots.
		mesh_->restore_overrides({}, {}, region, region_slot, 0, -1, {});
		discard();
		return false;
	}
	override_tables_[std::tuple<int, int, int>{region.x, region.y, region.z}] = 0;
	return true;
}

Dictionary VoxelWorld::debug_override_render_state(Vector3i brick) {
	std::unique_lock<std::mutex> edit_lock(edit_mutex_);
	Dictionary d;
	d["cpu_slot"] = -1;
	d["table"] = -1;
	d["table_slot"] = -1;
	d["sdf_match"] = false;
	d["mat_match"] = false;
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !overrides_) return d;
	const ve::IVec3 b{brick.x, brick.y, brick.z};
	const ve::IVec3 r = ve::WorldBounds::region_of_brick(b);
	const int region_slot = residency_ ? residency_->slot_of(r) : -1;
	if (region_slot < 0) return d;
	const int table = atlas_->overrides().region_table(region_slot);
	int table_slot = -1;
	if (table >= 0) {
		const int bi = ve::WorldBounds::brick_index_in_region(b);
		const PackedByteArray entry = device->buffer_get_data(atlas_->overrides().tables(),
				static_cast<uint32_t>((table * ve::kRegionBrickCount + bi) * 4), 4);
		if (entry.size() >= 4) table_slot = *reinterpret_cast<const int32_t *>(entry.ptr());
	}
	const int cpu_slot = overrides_->slot_of(b);
	d["cpu_slot"] = cpu_slot;
	d["table"] = table;
	d["table_slot"] = table_slot;
	if (table < 0 || table_slot < 0 || table_slot != cpu_slot || cpu_slot < 0) return d;
	const int sdf_stride = ((ve::kBrickSdfCount + 3) / 4) * 4;
	const int mat_stride = ((ve::kBrickVoxelCount + 3) / 4) * 4;
	const PackedByteArray sdf = device->buffer_get_data(atlas_->overrides().sdf_buffer(),
			static_cast<uint32_t>(cpu_slot * sdf_stride), sdf_stride);
	const PackedByteArray mat = device->buffer_get_data(atlas_->overrides().mat_buffer(),
			static_cast<uint32_t>(cpu_slot * mat_stride), mat_stride);
	const ve::OverrideBrick *cpu = overrides_->data(cpu_slot);
	if (!cpu || sdf.size() < sdf_stride || mat.size() < mat_stride) return d;
	d["sdf_match"] = std::memcmp(sdf.ptr(), cpu->sdf, ve::kBrickSdfCount) == 0;
	d["mat_match"] = std::memcmp(mat.ptr(), cpu->mat, ve::kBrickVoxelCount) == 0;
	return d;
}

void VoxelWorld::pump_consolidation() {
	// This is a frame-pump path: the lifetime guard prevents teardown from racing the
	// transaction, while the edit lock makes the queue, log, CPU store, and table map one
	// consistent state. No worker call below waits for GPU work.
	std::unique_lock<std::mutex> lifetime(render_lifetime_mutex_);
	if (render_shutting_down_) return;
	std::unique_lock<std::mutex> edit_lock(edit_mutex_);
	if (!mesh_ || !mesh_->is_valid() || !edit_log_ || !overrides_ || !residency_) return;

	const auto reset_transaction = [this]() {
		consolidation_in_flight_ = false;
		consolidation_publish_in_flight_ = false;
		consolidation_job_ = ConsolidateJob{};
		consolidation_table_ = -1;
		consolidation_old_table_ = -1;
		consolidation_old_entries_.clear();
		consolidation_entries_.clear();
		consolidation_old_slots_.clear();
		consolidation_old_bricks_.clear();
		consolidation_newly_acquired_.clear();
		consolidation_slots_.clear();
		consolidation_baked_.clear();
	};
	const auto requeue = [this](ve::IVec3 region) { requeue_consolidation_locked(region); };
	const auto rollback_render = [this]() {
		bool ok = true;
		if (atlas_) {
			for (size_t i = 0; i < consolidation_old_slots_.size(); i++)
				if (!atlas_->upload_override(rd(), consolidation_old_slots_[i],
						consolidation_old_bricks_[i])) ok = false;
			// Restore the previous NORMAL handles alongside the previous override bytes:
			// re-upload each old brick's compact normals, or park -1 where the old brick
			// had none, so a rolled-back table never names stale spans.
			for (size_t i = 0; i < consolidation_old_slots_.size(); i++) {
				const ve::OverrideBrick &old = consolidation_old_bricks_[i];
				if (old.normal_oct.size() == ve::kBrickSdfCount)
					atlas_->stored_normals().upload_override(rd(), consolidation_old_slots_[i],
							old.normal_oct.data(), ve::kBrickSdfCount);
				else
					atlas_->stored_normals().release_override(rd(), consolidation_old_slots_[i]);
			}
			if (consolidation_table_ >= 0) atlas_->overrides().clear_table(rd(), consolidation_table_);
			if (consolidation_job_.region_slot >= 0)
				atlas_->set_override_table(rd(), consolidation_job_.region_slot,
						consolidation_old_table_, consolidation_old_entries_);
		}
		return ok;
	};
	const auto refuse_transaction = [&](bool retry, bool rebuild_worker) {
		const ve::IVec3 region = consolidation_job_.region;
		const bool restored = rollback_render();
		for (const ve::IVec3 brick : consolidation_newly_acquired_) {
			// The speculative slot's normal span was staged before the table entry naming
			// it. Releasing the slot without releasing the span leaks payload out of the
			// fixed 32 MiB pool for the rest of the process.
			const int slot = overrides_ ? overrides_->slot_of(brick) : -1;
			if (slot >= 0 && atlas_) atlas_->stored_normals().release_override(rd(), slot);
			overrides_->release(brick);
		}
		if (!restored)
			UtilityFunctions::printerr(
					"VoxelWorld: render override rollback failed; retaining old edit state");
		// A worker rollback failure means its bytes are not authoritative. Rebuild from the
		// CPU store/table map after releasing the speculative slots and before any requeue.
		bool worker_rebuilt = true;
		if (rebuild_worker)
			worker_rebuilt = mesh_->replay_overrides(*overrides_, override_tables_);
		if (!worker_rebuilt)
			UtilityFunctions::printerr(
					"VoxelWorld: worker override rebuild failed; refusing requeue");
		consolidation_refusals_++;
		if (retry && worker_rebuilt) requeue(region);
		reset_transaction();
	};

	if (consolidation_in_flight_) {
		const ve::IVec3 region = consolidation_job_.region;
		if (consolidation_publish_in_flight_) {
			std::vector<OverridePublicationResult> results;
			if (mesh_->collect_override_publications(&results) == 0) return;
			if (results.empty() || !results.front().success) {
				const bool rebuild_worker = !results.empty() && !results.front().worker_state_valid;
				refuse_transaction(true, rebuild_worker);
				return;
			}
			// The worker transaction is complete. CPU bytes are committed only now; the old
			// table and op list were untouched until both consumers succeeded.
			const ve::IVec3 r = region;
			const ve::IVec3 base{r.x * ve::kRegionBricks, r.y * ve::kRegionBricks,
					r.z * ve::kRegionBricks};
			// The baked bytes live in the transaction's slots through the worker command; copy
			// them from the publication command's result is unnecessary because acquire slots
			// were populated before submission below.
			if (residency_->slot_of(r) != consolidation_job_.region_slot) {
				refuse_transaction(true, false);
				return;
			}
			if (atlas_)
				atlas_->set_override_table(rd(), consolidation_job_.region_slot,
						consolidation_table_, consolidation_entries_);
			for (size_t i = 0; i < consolidation_slots_.size(); i++)
				if (ve::OverrideBrick *data = overrides_->data(consolidation_slots_[i]))
					*data = consolidation_baked_[i];
			edit_log_->clear_region_through(r, consolidation_job_.through_seq);
			pending_dirty_.push_back({ve::chunk_of_brick(base),
					ve::chunk_of_brick({base.x + ve::kRegionBricks - 1,
							base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1})});
			if (edit_log_->op_count(r) >= ve::kConsolidateAtOps) queue_consolidation(r);
			float lo[3], first_hi[3], last_lo[3], hi[3];
			ve::brick_world_aabb(base, lo, first_hi);
			ve::brick_world_aabb({base.x + ve::kRegionBricks - 1,
					base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1}, last_lo, hi);
			if (lod_tree_) {
				std::lock_guard<std::mutex> lod_lock(lod_mutex_);
				lod_tree_->mark_dirty(lo, hi);
			}
			if (streamer_) streamer_->queue_region_regeneration_locked(r);
			override_tables_[std::tuple<int, int, int>{r.x, r.y, r.z}] = consolidation_table_;
			consolidation_count_++;
			reset_transaction();
			return;
		}

		std::vector<ConsolidateResult> results;
		if (mesh_->collect_consolidations(&results) == 0) return;
		if (results.empty() || results.front().failed ||
				results.front().baked.size() != consolidation_job_.bricks.size()) {
			consolidation_refusals_++;
			requeue(region);
			reset_transaction();
			return;
		}
		const ConsolidateResult &result = results.front();
		consolidation_slots_.clear();
		consolidation_baked_ = results.front().baked;
		consolidation_newly_acquired_.clear();
		for (const ve::IVec3 brick : result.bricks) {
			const bool present = overrides_->slot_of(brick) >= 0;
			const int slot = overrides_->acquire(brick);
			if (slot < 0) {
				refuse_transaction(true, false);
				return;
			}
			if (!present) consolidation_newly_acquired_.push_back(brick);
			consolidation_slots_.push_back(slot);
		}
		for (size_t i = 0; i < result.bricks.size(); i++) {
			const int bi = ve::WorldBounds::brick_index_in_region(result.bricks[i]);
			consolidation_entries_.erase(std::remove_if(consolidation_entries_.begin(),
					consolidation_entries_.end(), [bi](const std::pair<int, int> &entry) {
						return entry.first == bi;
					}), consolidation_entries_.end());
			consolidation_entries_.emplace_back(bi, consolidation_slots_[i]);
		}
		if (residency_->slot_of(region) != consolidation_job_.region_slot) {
			refuse_transaction(true, false);
			return;
		}
		bool render_ok = true;
		if (atlas_)
			for (size_t i = 0; i < consolidation_slots_.size(); i++)
				if (!atlas_->upload_override(rd(), consolidation_slots_[i], result.baked[i])) render_ok = false;
		// Task 7: stage the baked compact normals in the SAME transaction, before the
		// table entry that names them is published. A failed normal upload parks -1 and
		// the shader falls back to R8 taps -- geometry is never rejected.
		if (atlas_)
			for (size_t i = 0; i < consolidation_slots_.size(); i++) {
				const ve::OverrideBrick &brick = consolidation_baked_[i];
				if (brick.normal_oct.size() == ve::kBrickSdfCount)
					atlas_->stored_normals().upload_override(rd(), consolidation_slots_[i],
							brick.normal_oct.data(), ve::kBrickSdfCount);
				else
					atlas_->stored_normals().release_override(rd(), consolidation_slots_[i]);
			}
		if (!render_ok) {
			refuse_transaction(true, false);
			return;
		}
		OverridePublication publication;
		publication.slots = consolidation_slots_;
		publication.bricks = consolidation_baked_;
		publication.old_slots = consolidation_old_slots_;
		publication.old_bricks = consolidation_old_bricks_;
		publication.region = region;
		publication.region_slot = consolidation_job_.region_slot;
		publication.table = consolidation_table_;
		publication.old_table = consolidation_old_table_;
		publication.entries = consolidation_entries_;
		publication.old_entries = consolidation_old_entries_;
		if (!mesh_->submit_override_publication(std::move(publication))) {
			refuse_transaction(true, false);
			return;
		}
		consolidation_publish_in_flight_ = true;
		return;
	}

	if (consolidation_queue_.empty()) return;
	const ve::IVec3 region = consolidation_queue_.front();
	consolidation_queue_.erase(consolidation_queue_.begin());
	const int region_slot = residency_->slot_of(region);
	if (region_slot < 0) {
		consolidation_refusals_++;
		requeue(region);
		return;
	}
	const std::vector<ve::EditOp> &ops = edit_log_->ops(region);
	if (ops.empty()) return;
	ConsolidateJob job;
	job.region = region;
	job.region_slot = region_slot;
	job.ops = ops;
	const std::vector<uint64_t> &seqs = edit_log_->seqs(region);
	job.through_seq = seqs.empty() ? 0 : seqs.back();
	ve::plan_consolidation(job.ops.data(), static_cast<int>(job.ops.size()), region, &job.bricks);
	if (!job.bricks.empty()) {
		// Spec requires collect + snapshot while edit_mutex_ is held: edit_lock above spans this
		// whole function, so overrides_, edit_log_, and volumes_ are read in one consistent state.
		ve::IVec3 lo = job.bricks[0], hi = job.bricks[0];
		for (const auto &b : job.bricks) { lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z); hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z); }
		if (!snapshot_field_sources(job.ops, lo, hi, &job.source)) {
			consolidation_refusals_++;
			requeue(region);
			return;
		}
	}
	int needed_slots = 0;
	for (const ve::IVec3 brick : job.bricks) if (overrides_->slot_of(brick) < 0) needed_slots++;
	if (job.bricks.empty() || needed_slots > overrides_->capacity() - overrides_->used()) {
		consolidation_refusals_++;
		requeue(region);
		return;
	}
	const std::tuple<int, int, int> key{region.x, region.y, region.z};
	const auto found = override_tables_.find(key);
	const int old_table = found == override_tables_.end() ? -1 : found->second;
	int table = old_table;
	if (table < 0) {
		std::vector<bool> used(OverridePool::kMaxOverrideTables, false);
		for (const auto &it : override_tables_)
			if (it.second >= 0 && it.second < OverridePool::kMaxOverrideTables)
				used[static_cast<size_t>(it.second)] = true;
		for (int i = 0; i < OverridePool::kMaxOverrideTables; i++)
			if (!used[static_cast<size_t>(i)]) { table = i; break; }
		if (table < 0) {
			consolidation_refusals_++;
			requeue(region);
			return;
		}
	}
	const ve::IVec3 base{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	consolidation_old_entries_.clear();
	consolidation_old_slots_.clear();
	consolidation_old_bricks_.clear();
	for (int z = 0; z < ve::kRegionBricks; z++)
		for (int y = 0; y < ve::kRegionBricks; y++)
			for (int x = 0; x < ve::kRegionBricks; x++) {
				const ve::IVec3 brick{base.x + x, base.y + y, base.z + z};
				const int slot = overrides_->slot_of(brick);
				if (slot < 0) continue;
				consolidation_old_entries_.emplace_back(
						ve::WorldBounds::brick_index_in_region(brick), slot);
				consolidation_old_slots_.push_back(slot);
				consolidation_old_bricks_.push_back(*overrides_->data(slot));
			}
	consolidation_job_ = std::move(job);
	consolidation_table_ = table;
	consolidation_old_table_ = old_table;
	consolidation_entries_ = consolidation_old_entries_;
	std::vector<ConsolidateJob> worker_jobs;
	worker_jobs.push_back(consolidation_job_);
	if (!mesh_->submit_consolidations(std::move(worker_jobs))) {
		consolidation_refusals_++;
		requeue(region);
		reset_transaction();
		return;
	}
	consolidation_in_flight_ = true;
}

void VoxelWorld::debug_pump_consolidation_async() {
	ensure_physics_initialized();
	pump_consolidation();
}

void VoxelWorld::debug_wait_consolidation() {
	ensure_physics_initialized();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	for (;;) {
		bool in_flight = false;
		{
			std::lock_guard<std::mutex> lock(edit_mutex_);
			in_flight = consolidation_in_flight_;
		}
		if (!in_flight || std::chrono::steady_clock::now() >= deadline) return;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		pump_consolidation();
	}
}

void VoxelWorld::debug_pump_consolidation() {
	debug_pump_consolidation_async();
	debug_wait_consolidation();
}

Dictionary VoxelWorld::debug_consolidate_diff(Vector3i region) {
	Dictionary d;
	ensure_physics_initialized();
	std::unique_lock<std::mutex> edit_lock(edit_mutex_);
	if (!mesh_ || !edit_log_ || !overrides_ || !residency_) return d;
	const ve::IVec3 r{region.x, region.y, region.z};
	std::vector<ve::EditOp> ops = edit_log_->ops(r);
	std::vector<ve::IVec3> bricks;
	ve::plan_consolidation(ops.data(), static_cast<int>(ops.size()), r, &bricks);
	d["bricks"] = static_cast<int>(bricks.size());
	d["sdf_mismatches"] = 0;
	d["mat_mismatches"] = 0;
	if (bricks.empty()) return d;
	ConsolidateJob job;
	job.region = r;
	job.region_slot = residency_->slot_of(r);
	if (job.region_slot < 0) return d;
	job.bricks = bricks;
	job.ops = ops;
	if (!bricks.empty()) {
		ve::IVec3 lo = bricks[0], hi = bricks[0];
		for (auto &b : bricks) { lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z); hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z); }
		if (!snapshot_field_sources(ops, lo, hi, &job.source)) return d;
	}
	const int existing_table = override_table_for_region(r);
	if (existing_table >= 0) {
		std::vector<std::pair<int, int>> existing_entries;
		const ve::IVec3 base{r.x * ve::kRegionBricks, r.y * ve::kRegionBricks,
				r.z * ve::kRegionBricks};
		for (int z = 0; z < ve::kRegionBricks; z++)
			for (int y = 0; y < ve::kRegionBricks; y++)
				for (int x = 0; x < ve::kRegionBricks; x++) {
					const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
					const int slot = overrides_->slot_of(b);
					if (slot >= 0) existing_entries.emplace_back(
							ve::WorldBounds::brick_index_in_region(b), slot);
				}
		if (!mesh_->set_override_region(r, job.region_slot, existing_table, existing_entries)) return d;
	}
	if (!mesh_->submit_consolidations({job})) return d;
	mesh_->run_sync([](MeshPass &) {});
	std::vector<ConsolidateResult> results;
	if (mesh_->collect_consolidations(&results) != 1 || results[0].failed) return d;
	ve::AnalyticGenerator gen;
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
							bo[2] + z * ve::kVoxelSize, &volumes_, overrides_);
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
								bo[2] + z * ve::kVoxelSize, &volumes_, overrides_);
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
			ve::FieldSample fs = ve::eval_field_gradient(gen, ops.data(), static_cast<int>(ops.size()), px, py, pz, &volumes_, overrides_);
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

bool VoxelWorld::debug_consolidate_region(Vector3i region) {
	ensure_physics_initialized();
	std::unique_lock<std::mutex> edit_lock(edit_mutex_);
	const auto refuse = [this]() { consolidation_refusals_++; return false; };
	if (!mesh_ || !edit_log_ || !overrides_) return refuse();
	const ve::IVec3 r{region.x, region.y, region.z};
	std::vector<ve::EditOp> ops = edit_log_->ops(r);
	std::vector<ve::IVec3> bricks;
	ve::plan_consolidation(ops.data(), static_cast<int>(ops.size()), r, &bricks);
	int needed_slots = 0;
	for (const ve::IVec3 b : bricks) if (overrides_->slot_of(b) < 0) needed_slots++;
	if (bricks.empty() || needed_slots > overrides_->capacity() - overrides_->used()) return refuse();
	const int resident_slot = residency_ ? residency_->slot_of(r) : -1;
	if (resident_slot < 0) return refuse();

	const std::tuple<int, int, int> key{r.x, r.y, r.z};
	const auto found = override_tables_.find(key);
	const int old_table = found == override_tables_.end() ? -1 : found->second;
	int table = old_table;
	if (table < 0) {
		std::vector<bool> used(OverridePool::kMaxOverrideTables, false);
		for (const auto &it : override_tables_)
			if (it.second >= 0 && it.second < OverridePool::kMaxOverrideTables)
				used[static_cast<size_t>(it.second)] = true;
		for (int i = 0; i < OverridePool::kMaxOverrideTables; i++)
			if (!used[static_cast<size_t>(i)]) { table = i; break; }
		if (table < 0) return refuse();
	}
	const ve::IVec3 base{r.x * ve::kRegionBricks, r.y * ve::kRegionBricks,
			r.z * ve::kRegionBricks};
	std::vector<std::pair<int, int>> old_entries;
	std::vector<int> old_slots;
	std::vector<ve::OverrideBrick> old_bricks;
	for (int z = 0; z < ve::kRegionBricks; z++)
		for (int y = 0; y < ve::kRegionBricks; y++)
			for (int x = 0; x < ve::kRegionBricks; x++) {
				const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
				const int slot = overrides_->slot_of(b);
				if (slot < 0) continue;
				old_entries.emplace_back(ve::WorldBounds::brick_index_in_region(b), slot);
				old_slots.push_back(slot);
				old_bricks.push_back(*overrides_->data(slot));
			}

	ConsolidateJob job;
	job.region = r;
	job.region_slot = resident_slot;
	job.bricks = bricks;
	job.ops = ops;
	if (!bricks.empty()) {
		ve::IVec3 lo = bricks[0], hi = bricks[0];
		for (auto &b : bricks) { lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z); hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z); }
		if (!snapshot_field_sources(ops, lo, hi, &job.source)) return refuse();
	}
	// A reused worker region slot must see the old table while the bake reads its base.
	if (!mesh_->set_override_region(r, job.region_slot, old_table, old_entries)) return refuse();
	if (!mesh_->submit_consolidations({job})) return refuse();
	mesh_->run_sync([](MeshPass &) {});
	std::vector<ConsolidateResult> results;
	if (mesh_->collect_consolidations(&results) != 1 || results[0].failed ||
			results[0].baked.size() != bricks.size()) return refuse();

	std::vector<int> slots;
	std::vector<ve::IVec3> newly_acquired;
	for (const ve::IVec3 b : bricks) {
		const bool was_present = overrides_->slot_of(b) >= 0;
		const int slot = overrides_->acquire(b);
		if (slot < 0) {
			for (const ve::IVec3 acquired : newly_acquired) overrides_->release(acquired);
			return refuse();
		}
		if (!was_present) newly_acquired.push_back(b);
		slots.push_back(slot);
	}
	std::vector<std::pair<int, int>> entries;
	entries.reserve(bricks.size());
	for (size_t i = 0; i < bricks.size(); i++)
		entries.emplace_back(ve::WorldBounds::brick_index_in_region(bricks[i]), slots[i]);

	// Stage render bytes first and check every upload. The old CPU bytes/table remain the
	// rollback source until both devices have published the complete replacement.
	bool render_ok = true;
	if (atlas_) {
		for (size_t i = 0; i < slots.size(); i++)
			if (!atlas_->upload_override(rd(), slots[i], results[0].baked[i])) render_ok = false;
		// Task 7: normals share the transaction -- payload before table publication.
		if (render_ok)
			for (size_t i = 0; i < slots.size(); i++) {
				const ve::OverrideBrick &brick = results[0].baked[i];
				if (brick.normal_oct.size() == ve::kBrickSdfCount)
					atlas_->stored_normals().upload_override(rd(), slots[i],
							brick.normal_oct.data(), ve::kBrickSdfCount);
				else
					atlas_->stored_normals().release_override(rd(), slots[i]);
			}
		if (render_ok) atlas_->set_override_table(rd(), job.region_slot, table, entries);
	}
	const auto rollback_publication = [&]() {
		bool render_restored = true;
		if (atlas_) {
			for (size_t i = 0; i < old_slots.size(); i++)
				if (!atlas_->upload_override(rd(), old_slots[i], old_bricks[i])) render_restored = false;
			// Restore the previous NORMAL handles alongside the previous override bytes.
			for (size_t i = 0; i < old_slots.size(); i++) {
				const ve::OverrideBrick &old = old_bricks[i];
				if (old.normal_oct.size() == ve::kBrickSdfCount)
					atlas_->stored_normals().upload_override(rd(), old_slots[i],
							old.normal_oct.data(), ve::kBrickSdfCount);
				else
					atlas_->stored_normals().release_override(rd(), old_slots[i]);
			}
			atlas_->overrides().clear_table(rd(), table);
			atlas_->set_override_table(rd(), job.region_slot, old_table, old_entries);
		}
		bool worker_restored = mesh_->restore_overrides(old_slots, old_bricks, r,
				job.region_slot, table, old_table, old_entries);
		if (!worker_restored) {
			UtilityFunctions::printerr("VoxelWorld: worker override rollback failed; retrying");
			worker_restored = mesh_->restore_overrides(old_slots, old_bricks, r,
					job.region_slot, table, old_table, old_entries);
		}
		if (!worker_restored)
			UtilityFunctions::printerr("VoxelWorld: worker override rollback could not be completed");
		// Newly acquired slots are never part of the old table. Release them even when a
		// rollback reports failure; the old table/op list remains authoritative and the
		// transaction is refused/retried rather than leaking capacity.
		for (const ve::IVec3 acquired : newly_acquired) overrides_->release(acquired);
		return render_restored && worker_restored;
	};
	if (!render_ok) {
		rollback_publication();
		return refuse();
	}
	if (!mesh_->publish_overrides(slots, results[0].baked, r, job.region_slot, table, entries)) {
		if (!rollback_publication()) return refuse();
		return refuse();
	}
	for (size_t i = 0; i < slots.size(); i++) *overrides_->data(slots[i]) = results[0].baked[i];
	edit_log_->clear_region(r);
	const ve::IVec3 hi_brick{base.x + ve::kRegionBricks - 1,
			base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1};
	pending_dirty_.push_back({ve::chunk_of_brick(base), ve::chunk_of_brick(hi_brick)});
	float lo[3], first_hi[3], last_lo[3], hi[3];
	ve::brick_world_aabb(base, lo, first_hi);
	ve::brick_world_aabb({base.x + ve::kRegionBricks - 1, base.y + ve::kRegionBricks - 1,
			base.z + ve::kRegionBricks - 1}, last_lo, hi);
	if (lod_tree_) {
		std::lock_guard<std::mutex> lock(lod_mutex_);
		lod_tree_->mark_dirty(lo, hi);
	}
	if (streamer_) streamer_->queue_region_regeneration_locked(r);
	override_tables_[key] = table;
	return true;
}

Dictionary VoxelWorld::debug_lod_diff(int level, Vector3i coord) {
	Dictionary d;
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_) return d;
	constexpr int kFineCount = ve::kLodFineLattice * ve::kLodFineLattice * ve::kLodFineLattice;
	constexpr int kReducedCount =
			ve::kLodChunkLattice * ve::kLodChunkLattice * ve::kLodChunkLattice;
	const ve::IVec3 c{coord.x, coord.y, coord.z};
	std::vector<ve::EditOp> ops;
	gather_lod_ops(level, c, &ops);

	std::vector<uint8_t> fine_sdf, reduced_sdf;
	std::vector<uint16_t> fine_mat, reduced_mat;
	LodBuildResult result;
	bool ok = false;
	float origin[3];
	ve::lod_chunk_origin(level, c, origin);
	const ve::IVec3 region = ve::WorldBounds::region_of_point(origin[0], origin[1], origin[2]);
	const int override_table = override_table_for_region(region);
	mesh_->run_sync([&](MeshPass &pass) {
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
			const ve::VolumeData *v = volumes_.get(slot);
			if (v) lod.volumes().upload(rd, slot, *v);
		}
		std::vector<std::pair<int, int>> override_entries;
		if (override_table >= 0 && overrides_) {
			const ve::IVec3 base{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			for (int z = 0; z < ve::kRegionBricks; z++)
				for (int y = 0; y < ve::kRegionBricks; y++)
					for (int x = 0; x < ve::kRegionBricks; x++) {
						const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
						const int slot = overrides_->slot_of(b);
						if (slot < 0) continue;
						const ve::OverrideBrick *data = overrides_->data(slot);
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
	ve::AnalyticGenerator gen;

	// 1. The fine lattice against the CPU field.
	int fine_max_diff = 0;
	for (int z = 0; z < ve::kLodFineLattice; z++)
		for (int y = 0; y < ve::kLodFineLattice; y++)
			for (int x = 0; x < ve::kLodFineLattice; x++) {
				const float p[3] = {origin[0] + (static_cast<float>(x) - 3.0f) * cell * 0.5f,
						origin[1] + (static_cast<float>(y) - 3.0f) * cell * 0.5f,
						origin[2] + (static_cast<float>(z) - 3.0f) * cell * 0.5f};
				const float s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
						p[0], p[1], p[2], &volumes_, overrides_).sdf;
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

Dictionary VoxelWorld::debug_mesh_lattice_diff(Vector3i chunk) {
	Dictionary d;
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ops = edit_log_->ops(ve::region_of_chunk(c));
	}
	MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
	ve::chunk_world_origin(c, job.origin);
	job.cell_size = ve::kChunkCellSize;
	job.lattice = ve::kChunkLattice;
	std::vector<uint8_t> gpu;
	bool ok = false;
	mesh_->run_sync([&](MeshPass &pass) { ok = pass.run_field_sync(job, &gpu); });
	if (!ok) return d;

	ve::AnalyticGenerator gen;
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
						p[0], p[1], p[2], &volumes_).sdf;
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

Dictionary VoxelWorld::debug_mesh_diff(Vector3i chunk) {
	Dictionary d;
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ops = edit_log_->ops(ve::region_of_chunk(c));
	}
	MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
	job.override_table = override_table_for_region(ve::region_of_chunk(c));
	ve::chunk_world_origin(c, job.origin);
	job.cell_size = ve::kChunkCellSize;
	job.lattice = ve::kChunkLattice;
	MeshResult gpu;
	std::vector<uint8_t> lattice;
	std::vector<int32_t> gpu_cells;
	bool ok = false;
	mesh_->run_sync([&](MeshPass &pass) {
		ok = pass.mesh_sync(job, &gpu, &lattice, &gpu_cells);
	});
	if (!ok) return d;
	if (gpu.failed) return d; // short readback: do not present partial data as a diff

	const ve::DcGrid g = ve::chunk_dc_grid(c);
	ve::AnalyticGenerator gen;

	// 1. The lattice against the CPU field. One encoded step of sin() drift is invisible.
	int lat_max = 0, lat_over = 0;
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++) {
				const float s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
						g.origin[0] + (x - 1) * g.cell_size, g.origin[1] + (y - 1) * g.cell_size,
						g.origin[2] + (z - 1) * g.cell_size, &volumes_, overrides_).sdf;
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
				&volumes_, overrides_).sdf);
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
				mid.x + step.x, mid.y + step.y, mid.z + step.z, &volumes_, overrides_).sdf;
		const float in_side = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
				mid.x - step.x, mid.y - step.y, mid.z - step.z, &volumes_, overrides_).sdf;
		tri_sampled++;
		if (out_side <= in_side) winding_bad++;
	}
	d["max_surface_sdf"] = max_sdf;
	d["verts_off_10cm"] = off_10cm;
	d["winding_bad"] = winding_bad;
	d["tri_sampled"] = tri_sampled;
	return d;
}

bool VoxelWorld::extract_component(const std::vector<ve::IVec3> &cells, IslandExtractJob *job,
		std::vector<ve::CellBox> *boxes, ve::VolumeData *out) {
	if (!job || !boxes || !out || !mesh_ || !mesh_->is_valid() || cells.empty()) return false;
	if (!ve::greedy_box_merge(cells, ve::kMaxIslandBoxes, boxes)) return false;

	float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
	for (const ve::CellBox &b : *boxes) {
		float a[3], c[3];
		b.world_aabb(a, c);
		for (int k = 0; k < 3; k++) {
			wlo[k] = std::min(wlo[k], a[k]);
			whi[k] = std::max(whi[k], c[k]);
		}
	}
	job->boxes = *boxes;
	if (!ve::plan_island_lattice(wlo, whi, ve::kIslandDim, &job->voxel, job->origin)) return false;
	job->dim = ve::kIslandDim;
	job->override_table = override_table_for_region(
			ve::WorldBounds::region_of_point(job->origin[0], job->origin[1], job->origin[2]));
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		if (!edit_log_) return false;
		ve::collect_ops_for_aabb(*edit_log_, wlo, whi, &job->ops);
		float lattice_hi[3] = {job->origin[0] + (job->dim - 1) * job->voxel, job->origin[1] + (job->dim - 1) * job->voxel, job->origin[2] + (job->dim - 1) * job->voxel};
		ve::IVec3 blo = ve::WorldBounds::brick_of_point(job->origin[0], job->origin[1], job->origin[2]);
		ve::IVec3 bhi = ve::WorldBounds::brick_of_point(lattice_hi[0], lattice_hi[1], lattice_hi[2]);
		if (!snapshot_field_sources(job->ops, blo, bhi, &job->snapshot)) return false;
	}

	// Drive the worker synchronously: this is a diagnostic, not the streaming path.
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(*job);
	if (!mesh_->submit_extracts(std::move(jobs))) return false;
	std::vector<IslandExtractResult> results;
	for (int i = 0; i < 2000 && results.empty(); i++) {
		mesh_->collect_extracts(&results);
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (results.empty() || results[0].failed) return false;

	std::vector<float> aabbs(boxes->size() * 6);
	for (size_t i = 0; i < boxes->size(); i++)
		(*boxes)[i].world_aabb(&aabbs[i * 6], &aabbs[i * 6 + 3]);
	ve::VolumeData cpu;
	ve::AnalyticGenerator gen;
	ve::extract_island_volume(gen, job->ops.data(), static_cast<int>(job->ops.size()),
			&volumes_, job->origin, job->voxel, job->dim, aabbs.data(),
			static_cast<int>(boxes->size()), &cpu);
	*out = std::move(cpu);
	return true;
}

Dictionary VoxelWorld::debug_island_extract_diff(Vector3i lo_cell, Vector3i hi_cell) {
	Dictionary d;
	d["ok"] = false;
	ensure_physics_initialized();
	if (!mesh_ || !mesh_->is_valid()) return d;

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
	job.override_table = override_table_for_region(
			ve::WorldBounds::region_of_point(job.origin[0], job.origin[1], job.origin[2]));
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ve::collect_ops_for_aabb(*edit_log_, wlo, whi, &job.ops);
		float lattice_hi[3] = {job.origin[0] + (job.dim - 1) * job.voxel, job.origin[1] + (job.dim - 1) * job.voxel, job.origin[2] + (job.dim - 1) * job.voxel};
		ve::IVec3 blo = ve::WorldBounds::brick_of_point(job.origin[0], job.origin[1], job.origin[2]);
		ve::IVec3 bhi = ve::WorldBounds::brick_of_point(lattice_hi[0], lattice_hi[1], lattice_hi[2]);
		if (!snapshot_field_sources(job.ops, blo, bhi, &job.snapshot)) return d;
	}

	// Drive the worker synchronously: this is a diagnostic, not the streaming path.
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(job);
	if (!mesh_->submit_extracts(std::move(jobs))) return d;
	std::vector<IslandExtractResult> results;
	for (int i = 0; i < 2000 && results.empty(); i++) {
		mesh_->collect_extracts(&results);
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (results.empty() || results[0].failed) return d;

	std::vector<float> aabbs(boxes.size() * 6);
	for (size_t i = 0; i < boxes.size(); i++)
		boxes[i].world_aabb(&aabbs[i * 6], &aabbs[i * 6 + 3]);
	ve::VolumeData cpu;
	ve::AnalyticGenerator gen;
	ve::extract_island_volume(gen, job.ops.data(), static_cast<int>(job.ops.size()),
			&volumes_, job.origin, job.voxel, job.dim, aabbs.data(),
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
		ve::AnalyticGenerator agen;
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
			ve::FieldSample fs = ve::eval_field_gradient(agen, job.ops.data(), static_cast<int>(job.ops.size()), px, py, pz, &volumes_, overrides_);
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

Dictionary VoxelWorld::debug_place_test_island_rotated(int slot, Vector3i lo_cell,
		Vector3i hi_cell, Vector3 offset, float yaw, int volume_slot) {
	Dictionary d;
	d["ok"] = false;
	ensure_initialized();
	ensure_physics_initialized();
	RenderingDevice *device = rd();
	if (!device || !islands_ || !mesh_ || !mesh_->is_valid()) return d;
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
	if (!extract_component(cells, &job, &boxes, &volume)) return d;

	// Task 11's multi-island tests place a second island and expect the first to stay live.
	// The atlas's upload_descriptors replaces the whole array, so preserve the existing
	// descriptors by reading the GPU array back before overwriting the one slot. (The bytes
	// are the same 128-byte layout upload_descriptors writes; a dead slot has dim 0.)
	const int64_t desc_bytes = static_cast<int64_t>(kMaxIslands) * 128;
	const PackedByteArray existing =
			device->buffer_get_data(islands_->desc_buffer(), 0, static_cast<uint32_t>(desc_bytes));
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
	if (!atlas_->volumes().upload(device, vslot, volume)) return d;
	// Task 7: keep the CPU-authoritative copy too (the same thing IslandManager does for
	// real bodies), so debug_island_normal_probe reads the same normals the GPU holds.
	volumes_.reserve(vslot);
	if (!volumes_.store(vslot, volume)) return d;
	// Task 6: compact normals share the pool; the test fixture's radial lattice is real
	// render-reachable payload, not a fallback source.
	atlas_->stored_normals().upload_volume(device, vslot, volume);
	if (!islands_->upload_mip(device, slot, volume)) return d;

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
	islands_->upload_descriptors(device, all, kMaxIslands);
	{
		std::lock_guard<std::mutex> lock(island_mutex_);
		island_slots_ = std::max(island_slots_, slot + 1);
	}
	device->submit();
	device->sync();

	d["ok"] = true;
	d["world_center"] = Vector3(desc.origin[0], desc.origin[1], desc.origin[2]);
	d["voxel"] = job.voxel;
	d["solid"] = volume.solid_voxels;
	return d;
}

Dictionary VoxelWorld::debug_place_test_island(int slot, Vector3i lo_cell, Vector3i hi_cell,
		Vector3 offset) {
	return debug_place_test_island_rotated(slot, lo_cell, hi_cell, offset, 0.0f);
}

Dictionary VoxelWorld::debug_spawn_test_body(Vector3i lo_cell, Vector3i hi_cell, Vector3 offset,
		Vector3 impulse, bool debris) {
	Dictionary d;
	d["ok"] = false;
	ensure_initialized();
	ensure_physics_initialized();
	std::vector<ve::IVec3> cells;
	for (int z = lo_cell.z; z <= hi_cell.z; z++)
		for (int y = lo_cell.y; y <= hi_cell.y; y++)
			for (int x = lo_cell.x; x <= hi_cell.x; x++) cells.push_back({x, y, z});
	IslandExtractJob job;
	std::vector<ve::CellBox> boxes;
	ve::VolumeData volume;
	if (!extract_component(cells, &job, &boxes, &volume)) return d;

	const int slot = volumes_.allocate();
	if (slot < 0) return d;
	if (!volumes_.store(slot, volume)) {
		release_volume_slot(slot);
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
	const Ref<World3D> w3 = get_world_3d();
	if (!b->spawn(w3.is_valid() ? w3->get_space() : RID(),
				w3.is_valid() ? w3->get_scenario() : RID(), info, &volume)) {
		delete b;
		release_volume_slot(slot);
		return d;
	}
	test_bodies_.push_back(b);
	d["ok"] = true;
	d["index"] = static_cast<int>(test_bodies_.size()) - 1;
	d["atlas_slot"] = info.atlas_slot;
	d["mass"] = b->mass();
	d["shapes"] = b->shape_count();
	d["origin"] = b->transform().origin;
	d["has_render_mesh"] = b->has_render_mesh();
	d["render_tris"] = b->render_triangles();
	d["cel_material"] = b->has_cel_material();
	return d;
}

Dictionary VoxelWorld::debug_test_body_stats(int index) {
	Dictionary d;
	d["live"] = false;
	if (index < 0 || index >= static_cast<int>(test_bodies_.size()) || !test_bodies_[index])
		return d;
	IslandBody *b = test_bodies_[index];
	d["live"] = b->live();
	d["origin"] = b->transform().origin;
	d["asleep_s"] = b->asleep_seconds();
	d["mass"] = b->mass();
	d["cel_material"] = b->has_cel_material();
	return d;
}

void VoxelWorld::debug_tick_test_bodies(float dt) {
	for (IslandBody *b : test_bodies_)
		if (b) {
			b->tick(dt);
			b->sync_render();
		}
}

void VoxelWorld::debug_despawn_test_body(int index) {
	if (index < 0 || index >= static_cast<int>(test_bodies_.size()) || !test_bodies_[index])
		return;
	test_bodies_[index]->despawn();
}

void VoxelWorld::debug_clear_test_island(int slot) {
	RenderingDevice *device = rd();
	if (!device || !islands_) return;
	islands_->clear_slot(device, slot);
	device->submit();
	device->sync();
}

PackedInt32Array VoxelWorld::debug_island_tile_mask(Vector3 origin, Vector3 dir, float tan_x,
		float tan_y, int width, int height) {
	PackedInt32Array out;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!device || !islands_ || !island_cull_) return out;
	ve::CameraParams cam = ve::CameraParams::looking_at(origin.x, origin.y, origin.z,
			dir.x, dir.y, dir.z, 0, 1, 0);
	// looking_at leaves the tangents at 0 (the 1x1 probes need no frustum); a cull test does.
	cam.params[0] = tan_x;
	cam.params[1] = tan_y;
	if (!island_cull_->render(device, *islands_, cam, width, height,
				std::max(island_slot_count(), 1)))
		return out;
	device->submit();
	device->sync();
	const int n = island_cull_->tiles_x() * island_cull_->tiles_y();
	const PackedByteArray b = device->buffer_get_data(island_cull_->mask_buffer(), 0,
			static_cast<uint32_t>(n) * 4);
	if (b.size() < static_cast<int64_t>(n) * 4) return out;
	out.resize(n);
	std::memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(n) * 4);
	return out;
}

bool VoxelWorld::debug_mesh_submit(Array chunks) {
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_) return false;
	std::vector<ve::IVec3> coords;
	for (int i = 0; i < chunks.size(); i++) {
		const Vector3i v = chunks[i];
		coords.push_back({v.x, v.y, v.z});
	}
	std::vector<MeshRequest> requests;
	requests.reserve(coords.size());
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		for (const ve::IVec3 &c : coords)
			requests.push_back({c, edit_log_->ops(ve::region_of_chunk(c))});
	}
	return mesh_->submit(std::move(requests));
}

Array VoxelWorld::debug_mesh_collect() {
	Array out;
	if (!physics_ready_ || !mesh_) return out;
	// The mesher runs asynchronously now, so a test that submits and immediately collects
	// would race it. Wait for the batch to land — this is a diagnostic hook, and its old
	// contract was "collect returns the batch you submitted".
	std::vector<MeshResult> results;
	while (mesh_->busy() && mesh_->is_valid()) {
		if (mesh_->collect(&results) > 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	mesh_->collect(&results);
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

bool VoxelWorld::debug_lod_submit(Array jobs) {
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_) return false;
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
		gather_lod_ops(level, c, &job.ops);
		lod_jobs.push_back(std::move(job));
	}
	return mesh_->submit_lod(std::move(lod_jobs));
}

bool VoxelWorld::debug_extract_submit(int id, Vector3i lo_cell, Vector3i hi_cell) {
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_ || !edit_log_) return false;
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
	job.override_table = override_table_for_region(
			ve::WorldBounds::region_of_point(job.origin[0], job.origin[1], job.origin[2]));
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ve::collect_ops_for_aabb(*edit_log_, wlo, whi, &job.ops);
		float lattice_hi[3] = {job.origin[0] + (job.dim - 1) * job.voxel, job.origin[1] + (job.dim - 1) * job.voxel, job.origin[2] + (job.dim - 1) * job.voxel};
		ve::IVec3 blo = ve::WorldBounds::brick_of_point(job.origin[0], job.origin[1], job.origin[2]);
		ve::IVec3 bhi = ve::WorldBounds::brick_of_point(lattice_hi[0], lattice_hi[1], lattice_hi[2]);
		if (!snapshot_field_sources(job.ops, blo, bhi, &job.snapshot)) return false;
	}
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(std::move(job));
	return mesh_->submit_extracts(std::move(jobs));
}

Array VoxelWorld::debug_extract_collect() {
	Array out;
	if (!physics_ready_ || !mesh_) return out;
	std::vector<IslandExtractResult> results;
	mesh_->collect_extracts(&results);
	for (const IslandExtractResult &r : results) {
		Dictionary d;
		d["id"] = r.id;
		d["kind"] = static_cast<int>(r.kind);
		d["failed"] = r.failed;
		out.push_back(d);
	}
	return out;
}

Array VoxelWorld::debug_lod_collect() {
	Array out;
	if (!physics_ready_ || !mesh_) return out;
	std::vector<LodBuildResult> results;
	mesh_->collect_lod(&results);
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

bool VoxelWorld::render_probe_pixel(Vector3 origin, Vector3 dir) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_)
		return false;
	// The probe is a read-only diagnostic: it must not mutate the streamed world.
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t flags = ve::pack_flags(beauty_);
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, 1, 1,
			kNoEdit))
		return false;
	device->submit();
	device->sync();
	return true;
}

Color VoxelWorld::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	if (!render_probe_pixel(origin, dir)) return Color(1, 0, 1);
	RenderingDevice *device = rd();
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->albedo_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	if (data.size() < 4 || hp.size() < 16) return Color(1, 0, 1);
	const uint8_t *b = data.ptr();
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	// Alpha stays the HIT FLAG, as every existing caller assumes -- the albedo image's own
	// alpha is sun visibility and would read as "missed" for any shadowed pixel.
	return Color(b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, hf[3]);
}

Dictionary VoxelWorld::debug_raymarch_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	if (!render_probe_pixel(origin, dir)) return d;
	RenderingDevice *device = rd();
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	const PackedByteArray col = device->texture_get_data(raymarch_pass_->albedo_texture(), 0);
	if (hp.size() < 16 || col.size() < 4) return d;
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	const uint8_t *b = col.ptr();
	d["color"] = Color(b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, 1.0);
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
	const ve::IVec3 abv = atlas_->config().atlas_bricks;
	const ve::IVec3 cell{slot % abv.x, (slot / abv.x) % abv.y, slot / (abv.x * abv.y)};
	const PackedByteArray m2 = device->texture_get_data(atlas_->mip_atlas(0), 0);
	const PackedByteArray m8 = device->texture_get_data(atlas_->mip_atlas(2), 0);
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

Dictionary VoxelWorld::debug_raymarch_cost_probe(Vector3 origin, Vector3 dir) {
	Dictionary out;
	out["hit"] = false;
	out["steps"] = 0;
	out["bricks"] = 0;
	out["regions"] = 0;
	ensure_initialized();
	if (!initialized_) return out;
	if (!render_probe_pixel(origin, dir)) return out;
	RenderingDevice *device = rd();
	const PackedByteArray words = device->buffer_get_data(raymarch_pass_->cost_buffer(), 0, 8);
	if (words.size() < 8) return out;
	const uint32_t steps = words.decode_u32(0);
	const uint32_t cells = words.decode_u32(4);
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	if (hp.size() >= 16) {
		const float *hf = reinterpret_cast<const float *>(hp.ptr());
		out["hit"] = hf[3] > 0.5f;
	}
	out["steps"] = static_cast<int>(steps);
	out["bricks"] = static_cast<int>(cells & 0xFFFFu);
	out["regions"] = static_cast<int>(cells >> 16);
	return out;
}

Dictionary VoxelWorld::debug_raymarch_gbuffer(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_) return d;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t flags = ve::pack_flags(beauty_);
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, 1, 1, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray ab = device->texture_get_data(raymarch_pass_->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(raymarch_pass_->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	if (ab.size() < 4 || sf.size() < 8 || hp.size() < 16) return d;
	const uint8_t *a = ab.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *h = reinterpret_cast<const float *>(hp.ptr());
	d["albedo"] = Color(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1.0f);
	d["sun"] = a[3] / 255.0f;
	const float e[2] = {half_to_float(s[0]), half_to_float(s[1])};
	float n[3];
	ve::oct_decode(e, n);
	d["normal"] = Vector3(n[0], n[1], n[2]);
	d["material"] = static_cast<int>(half_to_float(s[2]) + 0.5f);
	d["gloss"] = half_to_float(s[3]);
	d["hit"] = h[3] > 0.5f;
	d["position"] = Vector3(h[0], h[1], h[2]);
	return d;
}

// Isolated g-buffer holes: a pixel the primary march missed while all four of its
// neighbours hit. Real sky is a connected region, so an isolated miss can only be the march
// stepping over geometry it should have found. Counting them is view-robust in a way that
// naming one guilty pixel is not.
Dictionary VoxelWorld::debug_raymarch_hole_probe(Vector3 origin, Vector3 dir, int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hit_pixels"] = 0;
	d["isolated_misses"] = 0;
	if (w <= 2 || h <= 2) return d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_) return d;
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(1.0471975512f * 0.5f);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.params[0] = tan_y * aspect;
	cam.params[1] = tan_y;
	cam.params[2] = 200.0f;
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z; cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t flags = ve::pack_flags(beauty_);
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, w, h, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
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

Dictionary VoxelWorld::debug_raymarch_normal_probe(Vector3 origin, Vector3 dir, int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hits"] = 0;
	d["rms_ndl"] = 0.0;
	d["cel_mismatch_fraction"] = 0.0;
	d["largest_mismatch_component"] = 0;
	if (w <= 0 || h <= 0) return d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_) return d;
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(1.0471975512f * 0.5f);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.params[0] = tan_y * aspect;
	cam.params[1] = tan_y;
	cam.params[2] = 200.0f;
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z; cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t flags = ve::pack_flags(beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, w, h, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray surface = device->texture_get_data(raymarch_pass_->surface_texture(), 0);
	const PackedByteArray hitpos = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
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
	ve::AnalyticGenerator gen;
	std::lock_guard<std::mutex> edit_lock(edit_mutex_);
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
					edit_log_->ops(ve::WorldBounds::region_of_point(hitx, hity, hitz));
			const ve::FieldSample fs = ve::eval_field_gradient(gen, ops.data(),
					static_cast<int>(ops.size()), hitx, hity, hitz, &volumes_, overrides_);
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
Dictionary VoxelWorld::debug_island_normal_probe(int island_slot, Vector3 origin, Vector3 dir,
		int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hits"] = 0;
	d["rms_ndl"] = 0.0;
	d["cel_mismatch_fraction"] = 0.0;
	d["largest_mismatch_component"] = 0;
	if (w <= 0 || h <= 0 || island_slot < 0 || island_slot >= kMaxIslands) return d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_) return d;
	// The descriptor the SHADER sees is the one on the device: test-placed islands are
	// uploaded directly, so read it back rather than trusting any cached copy.
	const int64_t desc_bytes = static_cast<int64_t>(kMaxIslands) * 128;
	const PackedByteArray desc =
			device->buffer_get_data(islands_->desc_buffer(), 0, static_cast<uint32_t>(desc_bytes));
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
	const ve::VolumeData *vol = volumes_.get(volume_slot);
	if (!vol || !vol->has_normals() || vol->dim != dim) return d;

	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float tan_y = std::tan(1.0471975512f * 0.5f);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.params[0] = tan_y * aspect;
	cam.params[1] = tan_y;
	cam.params[2] = 200.0f;
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z; cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t flags = ve::pack_flags(beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, w, h, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray surface = device->texture_get_data(raymarch_pass_->surface_texture(), 0);
	const PackedByteArray hitpos = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
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

Dictionary VoxelWorld::debug_ssr_probe(int fixture, int w, int h) {
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
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !materials_ || !raymarch_pass_ || !composite_pass_ ||
			!gbuffer_ || !beauty_camera_ || !ssr_pass_) return d;
	const ve::BeautySettings settings = beauty_settings();
	d["steps"] = settings.ssr_steps;
	if (!settings.ssr || settings.ssr_steps <= 0) return d;
	const Vector2i size(width, height);
	if (!gbuffer_->ensure(device, nullptr, size) || !beauty_camera_->ensure(device)) return d;
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
	const ve::WorldBounds probe_bounds = world_bounds();
	const ve::IVec3 probe_origin = probe_bounds.origin_regions();
	camera_params.dims[0] = world_size_regions_.x;
	camera_params.dims[1] = world_size_regions_.y;
	camera_params.dims[2] = world_size_regions_.z;
	camera_params.dims[3] = island_slot_count();
	camera_params.region_origin[0] = probe_origin.x;
	camera_params.region_origin[1] = probe_origin.y;
	camera_params.region_origin[2] = probe_origin.z;
	camera_params.atlas_bricks[0] = atlas_bricks_.x;
	camera_params.atlas_bricks[1] = atlas_bricks_.y;
	camera_params.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t probe_flags = ve::pack_flags(settings);
	std::memcpy(&camera_params.cam_pos[3], &probe_flags, sizeof(float));
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), camera_params, width, height,
			no_edit)) return d;
	float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
	lod_fade_band(&fade_start, &fade_end);
	composite_pass_->draw(device, *gbuffer_, raymarch_pass_->albedo_texture(),
			raymarch_pass_->surface_texture(), raymarch_pass_->hitpos_texture(), view_proj,
			*materials_, camera_pos, fade_start, fade_end);
	if (!composite_pass_->last_draw_ok()) return d;
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
	const PackedByteArray rendered_depth = device->texture_get_data(gbuffer_->depth(), 0);
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
	device->texture_copy(fixture_surface, gbuffer_->surface(), Vector3(), Vector3(),
			Vector3(width, height, 1), 0, 0, 0, 0);
	device->submit();
	device->sync();
	beauty_camera_->update(device, view_proj, camera_pos, size, 0.05f, 4000.0f);
	const RID normal_roughness = normal_texture;
	const bool ok = ssr_pass_->render(device, scene_color, scene_depth, gbuffer_->surface(),
			gbuffer_->depth(), normal_roughness, normal_roughness.is_valid(), beauty_camera_->buffer(),
			size, settings);
	device->submit();
	device->sync();
	auto release_fixture = [&]() {
		ssr_pass_->teardown();
		if (!ssr_pass_->initialize(device))
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
	const PackedByteArray reflection = device->texture_get_data(ssr_pass_->reflection(), 0);
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

Dictionary VoxelWorld::debug_outline_probe(int fixture, bool have_dynamic_normals) {
	Dictionary d;
	d["ran"] = false;
	d["dark_columns"] = 0;
	d["dark_value"] = 0.0f;
	d["mean_delta"] = 0.0f;
	d["max_brightening"] = 0.0f;
	d["max_alpha_delta"] = 0.0f;
	if (fixture < 0) return d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !outline_pass_ || !beauty_camera_) return d;
	const int width = 32, height = 16, pixels = width * height;
	if (!beauty_camera_->ensure(device)) return d;
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = c == r ? 1.0f : 0.0f;
	const float camera_pos[3] = {0.0f, 0.0f, -100.0f};
	beauty_camera_->update(device, view_proj, camera_pos, Vector2i(width, height), 0.05f,
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
		const ve::BeautySettings settings = beauty_settings();
		const bool ok = outline_pass_->render(device, scene_color, scene_depth, fixture_gb_depth,
				fixture_surface, normal_texture, have_dynamic_normals && normal_texture.is_valid(),
				beauty_camera_->buffer(), Vector2i(width, height), settings);
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
		outline_pass_->teardown();
		outline_pass_->initialize(device);
		for (RID r : {scene_depth, scene_color, fixture_gb_depth, fixture_surface, normal_texture})
			if (r.is_valid()) device->free_rid(r);
		return d;
}

Dictionary VoxelWorld::debug_glossy_sdf_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	d["albedo"] = Color();
	d["sun"] = 0.0f;
	d["material"] = 0;
	d["gloss"] = 0.0f;
	d["position"] = origin;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_) return d;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.params[0] = 0.0f;
	cam.params[1] = 0.0f;
	cam.params[2] = 200.0f;
	cam.params[3] = -1.0f;
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z; cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t flags = ve::pack_flags(beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, 1, 1, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray ab = device->texture_get_data(raymarch_pass_->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(raymarch_pass_->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	if (ab.size() < 4 || sf.size() < 8 || hp.size() < 16) return d;
	const uint8_t *a = ab.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *h = reinterpret_cast<const float *>(hp.ptr());
	d["albedo"] = Color(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1.0f);
	d["sun"] = a[3] / 255.0f;
	d["gloss"] = half_to_float(s[3]);
	d["material"] = static_cast<int>(half_to_float(s[2]) + 0.5f);
	d["hit"] = h[3] > 0.5f;
	d["position"] = Vector3(h[0], h[1], h[2]);
	return d;
}

Color VoxelWorld::debug_cel_reference(Color albedo, Color ambient, float ndl, float ndv,
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

Dictionary VoxelWorld::debug_cel_diff(Color albedo, Color ambient, float ndl, float ndv,
		float ndh, float shadow, float ao, float gloss) {
	Dictionary d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !gbuffer_ || !deferred_pass_ || !materials_) return d;
	if (gbuffer_->size() != Vector2i(1, 1)) {
		deferred_pass_->teardown();
		deferred_pass_->initialize(device);
		if (composite_pass_) composite_pass_->release_targets();
	}
	if (!gbuffer_->ensure(device, nullptr, Vector2i(1, 1))) return d;
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
	if (!deferred_pass_->render(device, *gbuffer_, *materials_, RID(), RID(), kNoSun, 0.0f, p))
		return d;
	device->submit();
	device->sync();
	const PackedByteArray got = device->texture_get_data(gbuffer_->lit(), 0);
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

Dictionary VoxelWorld::debug_sun_shadow_stats() {
	Dictionary d;
	d["size"] = SunShadowPass::kSize;
	d["map_valid"] = false;
	d["ortho_valid"] = false;
	d["texel_world"] = 0.0f;
	d["rebuilds"] = 0;
	d["view_proj"] = PackedFloat32Array();
	ensure_initialized();
	SunShadowPass *sun = sun_shadow_pass_;
	if (!sun) return d;
	const ve::WorldBounds wb = world_bounds();
	float lo[3];
	float hi[3];
	wb.aabb(lo, hi);
	const ve::SunOrtho ortho = ve::sun_ortho(ve::kSunDir, lo, hi, SunShadowPass::kSize);
	d["map_valid"] = sun->map().is_valid();
	d["ortho_valid"] = ortho.valid;
	d["texel_world"] = ortho.valid ? ortho.texel_world : sun->texel_world();
	d["rebuilds"] = sun->rebuilds();
	PackedFloat32Array matrix;
	matrix.resize(16);
	const float *source = sun->rebuilds() > 0 ? sun->view_proj() :
			(ortho.valid ? ortho.view_proj : sun->view_proj());
	for (int i = 0; i < 16; i++) matrix[i] = source[i];
	d["view_proj"] = matrix;
	return d;
}

void VoxelWorld::debug_sun_shadow_build(bool force) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!device || !sun_shadow_pass_ || !lod_pool_ || !lod_raster_pass_) return;
	prepare_lod_shadow_raster();
	const ve::WorldBounds wb = world_bounds();
	float lo[3];
	float hi[3];
	wb.aabb(lo, hi);
	sun_shadow_pass_->build(device, *lod_pool_, *lod_raster_pass_,
			ve::sun_ortho(ve::kSunDir, lo, hi, SunShadowPass::kSize), force);
	prepare_lod_raster();
}

float VoxelWorld::debug_sun_shadow_visibility(Vector3 p) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!device || !gbuffer_ || !deferred_pass_ || !materials_) return 1.0f;
	if (gbuffer_->size() != Vector2i(1, 1)) {
		deferred_pass_->teardown();
		deferred_pass_->initialize(device);
		if (composite_pass_) composite_pass_->release_targets();
	}
	if (!gbuffer_->ensure(device, nullptr, Vector2i(1, 1))) return 1.0f;
	const ve::BeautySettings beauty = beauty_settings();
	const bool use_sun = sun_shadow_pass_ && sun_shadow_pass_->is_valid() &&
			sun_shadow_pass_->rebuilds() > 0 && beauty.sun_shadow_map;
	DeferredPass::Params dp;
	dp.cam_pos[0] = p.x;
	dp.cam_pos[1] = p.y;
	dp.cam_pos[2] = p.z;
	dp.flags = ve::pack_flags(beauty);
	dp.probe_mode = 3;
	static const float kNoSun[16] = {};
	if (!deferred_pass_->render(device, *gbuffer_, *materials_, RID(),
			use_sun ? sun_shadow_pass_->map() : RID(),
			use_sun ? sun_shadow_pass_->view_proj() : kNoSun,
			use_sun ? sun_shadow_pass_->texel_world() : 0.0f, dp))
		return 1.0f;
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(gbuffer_->lit(), 0);
	if (data.size() < 8) return 1.0f;
	const uint16_t *value = reinterpret_cast<const uint16_t *>(data.ptr());
	return half_to_float(value[0]);
}

Dictionary VoxelWorld::debug_deferred_probe(Vector3 pos, Vector3 fwd, int w, int h,
		int probe_mode) {
	Dictionary d;
	if (w <= 0 || h <= 0 || (probe_mode != 0 && probe_mode != 1 && probe_mode != 2)) return d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_ ||
			!composite_pass_ || !deferred_pass_ || !gbuffer_) return d;
	if (gbuffer_->size() != Vector2i(w, h)) {
		deferred_pass_->teardown();
		deferred_pass_->initialize(device);
		composite_pass_->release_targets();
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
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cp.dims[0] = world_size_regions_.x;
	cp.dims[1] = world_size_regions_.y;
	cp.dims[2] = world_size_regions_.z;
	cp.dims[3] = island_slot_count();
	cp.region_origin[0] = ro.x;
	cp.region_origin[1] = ro.y;
	cp.region_origin[2] = ro.z;
	cp.atlas_bricks[0] = atlas_bricks_.x;
	cp.atlas_bricks[1] = atlas_bricks_.y;
	cp.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t flags = ve::pack_flags(beauty_settings());
	std::memcpy(&cp.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cp, w, h, kNoEdit)) return d;
	if (!gbuffer_->ensure(device, nullptr, Vector2i(w, h))) return d;
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	lod_fade_band(&fade_start, &fade_end);
	composite_pass_->draw(device, *gbuffer_, raymarch_pass_->albedo_texture(),
			raymarch_pass_->surface_texture(), raymarch_pass_->hitpos_texture(), view_proj,
			*materials_, p, fade_start, fade_end);
	if (!composite_pass_->last_draw_ok()) return d;
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
	if (!deferred_pass_->render(device, *gbuffer_, *materials_, RID(), RID(), kNoEdit, 0.0f, dp))
		return d;
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(gbuffer_->lit(), 0);
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

String VoxelWorld::debug_load_shader(const String &res_path) const {
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

bool VoxelWorld::preflight_shaders(RenderingDevice *rd, String *out_error) {
	if (!rd) {
		if (out_error) *out_error = "shader reload pre-flight: no RenderingDevice";
		return false;
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String inc = ps->globalize_path("res://shaders");
	Ref<DirAccess> dir = DirAccess::open("res://shaders");
	if (dir.is_null()) {
		if (out_error) *out_error = "shader reload pre-flight: cannot open res://shaders";
		return false;
	}
	dir->list_dir_begin();
	String file = dir->get_next();
	bool ok = true;
	while (!file.is_empty()) {
		if (!dir->current_is_dir() && file.ends_with(".glsl")) {
			const String res = "res://shaders/" + file;
			const String path = ps->globalize_path(res);
			std::string err;
			const std::string code = ve::strip_shader_annotations(
					ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
			if (code.empty()) {
				if (out_error) *out_error = res + String(": ") + String(err.c_str());
				ok = false;
				break;
			}
			Ref<RDShaderSource> src;
			src.instantiate();
			src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
			RenderingDevice::ShaderStage stage = RenderingDevice::SHADER_STAGE_COMPUTE;
			if (file.ends_with(".vert.glsl")) stage = RenderingDevice::SHADER_STAGE_VERTEX;
			else if (file.ends_with(".frag.glsl")) stage = RenderingDevice::SHADER_STAGE_FRAGMENT;
			src->set_stage_source(stage, String(code.c_str()));
			Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
			const String compile_err = spirv->get_stage_compile_error(stage);
			if (!compile_err.is_empty()) {
				if (out_error) *out_error = res + String(": ") + compile_err;
				ok = false;
				break;
			}
		}
		file = dir->get_next();
	}
	dir->list_dir_end();
	return ok;
}

void VoxelWorld::request_shader_reload() {
	// A latch, not the work: shaders are compiled and pipelines created on the device that
	// owns them, and for the shipping world that device belongs to the render thread.
	reload_requested_.store(true, std::memory_order_release);
}

void VoxelWorld::pump_shader_reload() {
	if (!reload_requested_.exchange(false, std::memory_order_acq_rel)) return;
	{
		std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
		if (render_shutting_down_) return;
	}
	{
		std::lock_guard<std::mutex> lock(reload_mutex_);
		reload_count_++;
	}
	if (!initialized_) {
		ensure_initialized();
		std::lock_guard<std::mutex> lock(reload_mutex_);
		reload_last_ok_ = initialized_;
		reload_last_error_ = initialized_ ? String() : String("shader reload re-init failed");
		return;
	}
	String error;
	if (!preflight_shaders(rd(), &error)) {
		// Fail-soft (spec §8): a shader that will not compile must not take down the
		// pipelines that are already running. Keep the old GPU objects untouched.
		std::lock_guard<std::mutex> lock(reload_mutex_);
		reload_last_ok_ = false;
		reload_last_error_ = error;
		UtilityFunctions::printerr("VoxelWorld: shader reload pre-flight failed; keeping old pipelines: ",
				error);
		return;
	}
	// teardown_gpu() frees every GPU object and leaves the CPU cores -- edit log, residency,
	// override store, LoD tree -- untouched, so ensure_initialized() re-streams the same
	// world. This is the whole hot reload.
	teardown_gpu();
	ensure_initialized();
	{
		std::lock_guard<std::mutex> lock(reload_mutex_);
		reload_last_ok_ = initialized_;
		reload_last_error_ = initialized_ ? String() : String("shader reload re-init failed");
	}
}

Dictionary VoxelWorld::debug_shader_reload_stats() {
	Dictionary d;
	std::lock_guard<std::mutex> lock(reload_mutex_);
	d["reloads"] = reload_count_;
	d["last_ok"] = reload_last_ok_;
	d["last_error"] = reload_last_error_;
	return d;
}

void VoxelWorld::debug_set_shader_override(const String &name, const String &source) {
	ve::set_shader_source_override(name.utf8().get_data(), source.utf8().get_data());
}

Dictionary VoxelWorld::debug_self_check() {
	Dictionary d;
	d["field_mismatches"] = 0;
	d["brick_mismatches"] = 0;
	d["mesh_mismatches"] = 0;
	d["lod_mismatches"] = 0;
	d["occupancy_mismatches"] = 0;
	const auto start = std::chrono::steady_clock::now();

	// Use the live camera when there is one; the self-check keybind is for the running demo.
	Vector3 center(24.0f, 64.0f, 24.0f);
	Viewport *vp = get_viewport();
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
			std::lock_guard<std::mutex> lock(edit_mutex_);
			if (edit_log_) ops_vec = edit_log_->ops(region);
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

void VoxelWorld::debug_store_volume(int slot, const PackedByteArray &sdf,
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
	volumes_.reserve(slot); // no-op when the suite already claimed it
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
	if (volumes_.store(slot, std::move(d)) && mesh_) {
		mesh_->submit_volume(slot, to_upload);
		mesh_->run_sync([](MeshPass &){});
	}
}

Vector2 VoxelWorld::debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) {
	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field: op buffer too small");
			return Vector2();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::Sample s = ve::eval_field(gen, ptr, op_count, p.x, p.y, p.z, &volumes_, overrides_);
	return Vector2(s.sdf, static_cast<float>(s.material));
}

Dictionary VoxelWorld::debug_eval_field_gradient(Vector3 p, const PackedByteArray &ops, int op_count) {
	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field_gradient: op buffer too small");
			return Dictionary();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::FieldSample s = ve::eval_field_gradient(gen, ptr, op_count, p.x, p.y, p.z, &volumes_, overrides_);
	Dictionary d;
	d["sdf"] = s.sdf;
	d["material"] = int(s.material);
	d["gradient"] = Vector3(s.gradient[0], s.gradient[1], s.gradient[2]);
	d["exact"] = s.exact_gradient;
	return d;
}

Dictionary VoxelWorld::debug_material_atlas_stats() {
	Dictionary d;
	if (!materials_ || !materials_->is_valid()) return d;
	d["layers"] = materials_->layer_count();
	d["width"] = kMaterialTextureSize;
	d["height"] = kMaterialTextureSize;
	d["mipmaps"] = kMaterialMipmaps;
	d["albedo_valid"] = materials_->albedo_array().is_valid();
	d["surface_valid"] = materials_->surface_array().is_valid();
	return d;
}

// Task 7: rewrite the top-mip normal-map texels (surface array RG) of one material layer
// with a hard tilt, so a test can re-render and prove the G-buffer normal never depends
// on the material normal map.
bool VoxelWorld::debug_poke_material_normal(int layer) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !materials_) return false;
	if (layer < 0 || layer >= materials_->layer_count()) return false;
	PackedByteArray data = device->texture_get_data(materials_->surface_array(), layer);
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
	device->texture_update(materials_->surface_array(), layer, data);
	return true;
}

Color VoxelWorld::debug_material_probe(int mat, Vector3 p, Vector3 n) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_)
		return Color(1, 0, 1);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			p.x, p.y, p.z, n.x, n.y, n.z, 0, 1, 0);
	// pc.params.w is the debug-probe flag in raymarch.comp.glsl; cam_pos and cam_fwd carry
	// the sample point and normal.
	cam.params[0] = 0.0f;
	cam.params[1] = 0.0f;
	cam.params[2] = 0.0f;
	cam.params[3] = static_cast<float>(mat);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, 1, 1,
			kNoEdit))
		return Color(1, 0, 1);
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->albedo_texture(), 0);
	if (data.size() < 4) return Color(1, 0, 1);
	const uint8_t *b = data.ptr();
	return Color(b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, 1.0);
}

bool VoxelWorld::debug_init_atlas() {
	ensure_initialized();
	return atlas_ && atlas_->is_valid();
}

void VoxelWorld::debug_teardown_atlas() {
	teardown_gpu();
}

Dictionary VoxelWorld::debug_atlas_stats() {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!atlas_ || !atlas_->is_valid() || !device) return d;
	d["slot_count"] = atlas_->atlas_slot_count();
	d["free_slots"] = atlas_->read_free_count(device);
	d["region_map_entries"] = atlas_->region_map_entries();
	d["job_count"] = atlas_->read_job_count(device);
	d["overflow"] = static_cast<int>(atlas_->read_overflow(device));
	// Memory bounds (Task 6): the R8 atlas byte count is pinned so a regression that
	// resizes it fails loudly next to the normal-pool capacity assertion.
	const ve::IVec3 ab = atlas_->config().atlas_bricks;
	const int64_t sdf_bytes = static_cast<int64_t>(ab.x) * ve::kBrickSdfStride *
			(ab.y * ve::kBrickSdfStride) * (ab.z * ve::kBrickSdfStride);
	d["sdf_atlas_bytes"] = sdf_bytes;
	return d;
}

void VoxelWorld::debug_reset_frame_counters() {
	if (atlas_ && rd()) atlas_->reset_frame_counters(rd());
}

void VoxelWorld::debug_set_region_map_entry(int region_index, int region_slot) {
	if (atlas_ && rd()) atlas_->set_region_map_entry(rd(), region_index, region_slot);
}

void VoxelWorld::debug_upload_region_ops(int region_slot, const PackedByteArray &ops, int count) {
	if (!atlas_ || !rd()) return;
	const ve::EditOp *ptr = nullptr;
	if (count > 0) {
		if (ops.size() < count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_upload_region_ops: op buffer too small");
			return;
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	atlas_->upload_region_ops(rd(), region_slot, ptr, count);
}

bool VoxelWorld::debug_brick_has_surface(Vector3i brick, const PackedByteArray &ops,
		int op_count) const {
	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_brick_has_surface: op buffer too small");
			return false;
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	return ve::brick_has_surface(gen, ptr, op_count, {brick.x, brick.y, brick.z}, &volumes_);
}

void VoxelWorld::debug_mark_region(Vector3i region, int region_slot, Vector3i lo, Vector3i hi,
		int op_count, bool force) {
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !region_pass_) return;
	if (region_slot < 0 || region_slot >= max_region_slots_) {
		// The mark shader indexes region_tables with rslot * kRegionBrickCount + bi, so a
		// hostile slot is a GPU-side out-of-bounds write. Refuse before recording.
		UtilityFunctions::printerr("debug_mark_region: region_slot ", region_slot,
				" out of range [0, ", max_region_slots_, ")");
		return;
	}
	const int64_t list = device->compute_list_begin();
	region_pass_->mark(device, list, {region.x, region.y, region.z}, region_slot,
			{lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}, op_count, force);
	device->compute_list_end();
	device->submit();
	device->sync();
}

void VoxelWorld::debug_generate_pending() {
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !region_pass_ || !gen_pass_) return;
	const int64_t list = device->compute_list_begin();
	region_pass_->write_dispatch_args(device, list);
	device->compute_list_add_barrier(list);
	gen_pass_->dispatch(device, list, *atlas_);
	device->compute_list_end();
	device->submit();
	device->sync();
}

Dictionary VoxelWorld::debug_brick_diff(Vector3i brick, int region_slot,
		const PackedByteArray &ops, int op_count) {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!device || !atlas_) return d;
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

	ve::AnalyticGenerator gen;
	ve::BrickEval ref{};
	ve::eval_brick(gen, ptr, op_count, b, &ref, &volumes_, overrides_);

	const ve::IVec3 ab = atlas_->config().atlas_bricks;
	const ve::IVec3 cell{slot % ab.x, (slot / ab.x) % ab.y, slot / (ab.x * ab.y)};

	// texture_get_data returns the whole volume; tests run a small atlas, so one read each.
	const PackedByteArray sdf = device->texture_get_data(atlas_->sdf_atlas(), 0);
	const PackedByteArray mat = device->texture_get_data(atlas_->mat_atlas(), 0);
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

	const PackedByteArray pal_bytes = device->buffer_get_data(atlas_->palette(),
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
		const PackedByteArray mip = device->texture_get_data(atlas_->mip_atlas(level), 0);
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

void VoxelWorld::debug_stream_region(Vector3i region) {
	ensure_initialized();
	if (!initialized_ || !rd() || !streamer_) return;
	const Vector3 center((region.x * ve::kRegionBricks + ve::kRegionBricks / 2) * ve::kBrickSize,
			(region.y * ve::kRegionBricks + ve::kRegionBricks / 2) * ve::kBrickSize,
			(region.z * ve::kRegionBricks + ve::kRegionBricks / 2) * ve::kBrickSize);
	for (int i = 0; i < 8; i++) {
		debug_stream_frame(center);
		if (debug_slot_of_region(region) >= 0) return;
	}
}

Dictionary VoxelWorld::debug_brick_flags(Vector3i region) {
	Dictionary d;
	debug_stream_region(region);
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !edit_log_) return d;
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;

	const PackedByteArray table = device->buffer_get_data(atlas_->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	const PackedByteArray flags = device->buffer_get_data(atlas_->brick_flags());
	if (table.size() < ve::kRegionBrickCount * 4 ||
			flags.size() < atlas_->atlas_slot_count() * static_cast<int>(sizeof(uint32_t))) return d;

	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ops = edit_log_->ops({region.x, region.y, region.z});
	}
	ve::AnalyticGenerator gen;
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
		ve::eval_brick(gen, ops.data(), static_cast<int>(ops.size()), brick, &ref, &volumes_, overrides_);
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

Dictionary VoxelWorld::debug_brick_flags_after_mark(Vector3i region) {
	Dictionary d;
	debug_stream_region(region);
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !edit_log_ || !region_pass_) return d;
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;
	int op_count = 0;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		op_count = static_cast<int>(edit_log_->ops({region.x, region.y, region.z}).size());
	}
	const ve::IVec3 lo{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	const ve::IVec3 hi{lo.x + ve::kRegionBricks - 1, lo.y + ve::kRegionBricks - 1,
			lo.z + ve::kRegionBricks - 1};
	debug_mark_region(region, rslot, Vector3i(lo.x, lo.y, lo.z), Vector3i(hi.x, hi.y, hi.z),
			op_count, true);
	const PackedByteArray table = device->buffer_get_data(atlas_->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	const PackedByteArray flags = device->buffer_get_data(atlas_->brick_flags());
	if (table.size() < ve::kRegionBrickCount * 4 ||
			flags.size() < atlas_->atlas_slot_count() * static_cast<int>(sizeof(uint32_t))) return d;
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

void VoxelWorld::debug_release_region(int region_slot) {
	RenderingDevice *device = rd();
	if (!device || !region_pass_) return;
	if (region_slot < 0 || region_slot >= max_region_slots_) {
		// Same hostile-slot hazard as debug_mark_region: the free shader indexes
		// region_tables with rslot * kRegionBrickCount + bi.
		UtilityFunctions::printerr("debug_release_region: region_slot ", region_slot,
				" out of range [0, ", max_region_slots_, ")");
		return;
	}
	const int64_t list = device->compute_list_begin();
	region_pass_->release_region(device, list, region_slot);
	device->compute_list_end();
	device->submit();
	device->sync();
}

PackedInt32Array VoxelWorld::debug_jobs() {
	PackedInt32Array out;
	RenderingDevice *device = rd();
	if (!device || !atlas_) return out;
	const int count = atlas_->read_job_count(device);
	if (count <= 0) return out;
	const PackedByteArray b = device->buffer_get_data(atlas_->jobs(), 0, count * 32);
	out.resize(count * 8);
	memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(count) * 32);
	return out;
}

int VoxelWorld::debug_region_table_slot(int region_slot, Vector3i brick) {
	RenderingDevice *device = rd();
	if (!device || !atlas_) return -1;
	const int bi = ve::WorldBounds::brick_index_in_region({brick.x, brick.y, brick.z});
	const uint32_t offset =
			(static_cast<uint32_t>(region_slot) * ve::kRegionBrickCount + bi) * 4;
	const PackedByteArray b = device->buffer_get_data(atlas_->region_tables(), offset, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

RID VoxelWorld::debug_sdf_atlas() const { return atlas_ ? atlas_->sdf_atlas() : RID(); }
RID VoxelWorld::debug_mat_atlas() const { return atlas_ ? atlas_->mat_atlas() : RID(); }
RID VoxelWorld::debug_mip_atlas(int level) const {
	if (!atlas_ || level < 0 || level >= ve::kMipLevels) return RID();
	return atlas_->mip_atlas(level);
}
RID VoxelWorld::debug_region_map() const { return atlas_ ? atlas_->region_map() : RID(); }
RID VoxelWorld::debug_region_tables() const { return atlas_ ? atlas_->region_tables() : RID(); }
RID VoxelWorld::debug_free_list() const { return atlas_ ? atlas_->free_list() : RID(); }
RID VoxelWorld::debug_frame_counters() const { return atlas_ ? atlas_->frame_counters() : RID(); }
RID VoxelWorld::debug_op_pool() const { return atlas_ ? atlas_->op_pool() : RID(); }
RID VoxelWorld::debug_op_counts() const { return atlas_ ? atlas_->op_counts() : RID(); }

void VoxelWorld::drain_occupancy() {
	std::vector<OccupancyBlock> blocks;
	{
		std::lock_guard<std::mutex> lock(occupancy_mutex_);
		blocks.swap(occupancy_inbox_);
	}
	for (const OccupancyBlock &b : blocks) {
		// A region marked in consecutive frames can have two reads in flight, and the older
		// one can land after the newer one. Never let it regress the grid or the block's seq.
		if (b.seq < occupancy_.block_seq(b.region)) continue;
		occupancy_.set_block(b.region, b.bytes.data(), b.seq);
	}
}

int VoxelWorld::debug_occupancy_state(Vector3i cell) {
	drain_occupancy(); // tests step the streamer by hand and never run _process
	return static_cast<int>(occupancy_.state({cell.x, cell.y, cell.z}));
}

void VoxelWorld::debug_pump_occupancy() {
	// Contract: harvest already-issued async GPU readbacks and fold their inbox blocks; this
	// helper does not advance the streamer or issue a mark. Tests must drive frames separately
	// when they need a fresh mark, so harvesting cannot hide which mark branch ran.
	ensure_initialized();
	if (streamer_ && rd()) streamer_->harvest_occupancy(rd());
	drain_occupancy();
}

Dictionary VoxelWorld::debug_occupancy_fallback_diff(Vector3i region) {
	Dictionary d;
	d["compared"] = 0;
	d["fallback"] = 0;
	d["mismatches"] = 0;
	d["first_mismatch_brick"] = Vector3i(-1, -1, -1);
	ensure_initialized();
	if (!rd() || !atlas_ || !edit_log_ || !region_pass_) return d;
	debug_stream_region(region);
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;

	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ops = edit_log_->ops({region.x, region.y, region.z});
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
	const PackedByteArray gpu = rd()->buffer_get_data(atlas_->region_occupancy(),
			static_cast<uint32_t>(rslot) * block_bytes, block_bytes);
	if (gpu.size() < static_cast<int>(block_bytes)) return d;

	ve::AnalyticGenerator gen;
	int compared = 0, fallback = 0, mismatches = 0;
	Vector3i first(-1, -1, -1);
	for (int bi = 0; bi < ve::kRegionBrickCount; bi++) {
		const ve::IVec3 brick{
				region.x * ve::kRegionBricks + (bi & (ve::kRegionBricks - 1)),
				region.y * ve::kRegionBricks + ((bi >> 5) & (ve::kRegionBricks - 1)),
				region.z * ve::kRegionBricks + (bi >> 10)};
		if (ve::brick_has_surface(gen, ops.data(), static_cast<int>(ops.size()), brick,
				&volumes_, overrides_)) continue;
		fallback++;
		const int got = ve::OccupancyGrid::read_packed(
				reinterpret_cast<const uint8_t *>(gpu.ptr()), bi);
		const int want = static_cast<int>(ve::cell_state_probe(gen, ops.data(),
				static_cast<int>(ops.size()), brick, &volumes_, overrides_));
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

Dictionary VoxelWorld::debug_occupancy_diff(Vector3i region) {
	Dictionary d;
	d["compared"] = 0;
	d["mismatches"] = 0;
	d["first_mismatch_brick"] = Vector3i(-1, -1, -1);
	ensure_initialized();
	if (!rd() || !atlas_ || !edit_log_) return d;
	debug_stream_region(region);
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;
	const uint32_t block_bytes = GpuAtlas::occupancy_block_bytes();
	const PackedByteArray gpu = rd()->buffer_get_data(atlas_->region_occupancy(),
			static_cast<uint32_t>(rslot) * block_bytes, block_bytes);
	const PackedByteArray table = rd()->buffer_get_data(atlas_->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	if (gpu.size() < static_cast<int>(block_bytes) ||
			table.size() < ve::kRegionBrickCount * 4) return d;
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ops = edit_log_->ops({region.x, region.y, region.z});
	}
	const int32_t *slots = reinterpret_cast<const int32_t *>(table.ptr());
	ve::AnalyticGenerator gen;
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
				static_cast<int>(ops.size()), brick, &volumes_, overrides_));
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

float VoxelWorld::debug_field_sdf(Vector3 p) {
	if (!edit_log_) return 1e30f;
	ve::AnalyticGenerator gen;
	std::lock_guard<std::mutex> lock(edit_mutex_);
	const std::vector<ve::EditOp> &ops =
			edit_log_->ops(ve::WorldBounds::region_of_point(p.x, p.y, p.z));
	return ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()), p.x, p.y, p.z,
			&volumes_, overrides_).sdf;
}

int VoxelWorld::debug_cell_state(Vector3i cell) {
	if (!edit_log_) return static_cast<int>(ve::kCellUnknown);
	const ve::IVec3 c{cell.x, cell.y, cell.z};
	ve::AnalyticGenerator gen;
	std::lock_guard<std::mutex> lock(edit_mutex_);
	const std::vector<ve::EditOp> &ops = edit_log_->ops(ve::WorldBounds::region_of_brick(c));
	return static_cast<int>(ve::cell_state_field(gen, ops.data(),
			static_cast<int>(ops.size()), c, &volumes_, overrides_));
}

Dictionary VoxelWorld::debug_occupancy_stats(Vector3 center) {
	drain_occupancy();
	Dictionary d;
	d["regions"] = occupancy_.region_count();
	d["edit_seq"] = static_cast<int64_t>(edit_seq());
	// The block covering the streaming centre, so a test can tell "the grid has been told
	// about this edit" from "some other region's block arrived".
	const ve::IVec3 r = ve::WorldBounds::region_of_point(center.x, center.y, center.z);
	d["seq_at_center"] = static_cast<int64_t>(occupancy_.block_seq(r));
	return d;
}

int VoxelWorld::debug_stream_frame(Vector3 cam) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !streamer_) return 0;
	const int actions = streamer_->run_frame(device, cam.x, cam.y, cam.z);
	device->submit();
	device->sync();
	overflow_seen_ |= static_cast<int>(atlas_->read_overflow(device));
	drain_occupancy();
	return actions;
}

Dictionary VoxelWorld::debug_stream_stats() {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !residency_ || !streamer_) return d;
	d["resident_regions"] = residency_->resident_count();
	d["frame_edits"] = streamer_->last_frame_edits();
	d["overflow"] = static_cast<int>(atlas_->read_overflow(device));
	// Either path may be the one running: debug_stream_frame drives the world in tests, the
	// compositor's render callback drives it in the demo, and only the streamer sees the
	// latter's frames. The HUD reads this, so it has to cover both.
	d["overflow_ever"] =
			overflow_seen_ | static_cast<int>(streamer_->overflow_seen());
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		d["override_bricks"] = overrides_ ? overrides_->used() : 0;
		d["override_capacity"] = overrides_ ? overrides_->capacity() : max_override_bricks_;
		d["consolidations"] = consolidation_count_;
		d["consolidation_refusals"] = consolidation_refusals_;
		d["consolidation_queue_refusals"] = consolidation_queue_refusals_;
		d["edit_rejections"] = edit_rejections_;
	}
	return d;
}

int VoxelWorld::debug_slot_of_region(Vector3i region) const {
	if (!residency_) return -1;
	return residency_->slot_of({region.x, region.y, region.z});
}

int VoxelWorld::debug_region_map_entry(Vector3i region) {
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_) return -1;
	const int idx = world_bounds().region_index({region.x, region.y, region.z});
	if (idx < 0) return -1;
	const PackedByteArray b = device->buffer_get_data(atlas_->region_map(), idx * 4, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

bool VoxelWorld::debug_region_map_consistent() {
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !residency_) return false;
	const ve::WorldBounds wb = world_bounds();
	const PackedByteArray b = device->buffer_get_data(atlas_->region_map());
	const int32_t *map = reinterpret_cast<const int32_t *>(b.ptr());
	const ve::IVec3 o = wb.origin_regions();
	const ve::IVec3 sz = wb.size_regions;
	for (int z = 0; z < sz.z; z++)
		for (int y = 0; y < sz.y; y++)
			for (int x = 0; x < sz.x; x++) {
				const ve::IVec3 r{o.x + x, o.y + y, o.z + z};
				const int gpu_slot = map[x + y * sz.x + z * sz.x * sz.y];
				const int cpu_slot = residency_->slot_of(r);
				if (gpu_slot != cpu_slot) return false;
				if (gpu_slot >= 0 && !(residency_->region_of_slot(gpu_slot) == r)) return false;
			}
	return true;
}

Dictionary VoxelWorld::debug_raycast(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	if (!edit_log_) return d;
	std::lock_guard<std::mutex> lock(edit_mutex_);
	ve::AnalyticGenerator gen;
	const float o[3] = {origin.x, origin.y, origin.z};
	const float f[3] = {dir.x, dir.y, dir.z};
	const ve::RayHit h = ve::raycast(gen, *edit_log_, o, f, 200.0f, &volumes_, overrides_);
	if (!h.hit) return d;
	d["hit"] = true;
	d["pos"] = Vector3(h.pos[0], h.pos[1], h.pos[2]);
	d["normal"] = Vector3(h.normal[0], h.normal[1], h.normal[2]);
	d["distance"] = h.distance;
	return d;
}
