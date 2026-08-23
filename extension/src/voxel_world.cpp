#include "voxel_world.h"
#include "debug/hooks.h"
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
#include "generator/field_generator.h"
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
	ClassDB::bind_method(D_METHOD("hooks"), &VoxelWorld::hooks);
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
	ClassDB::bind_method(D_METHOD("ensure_initialized"), &VoxelWorld::ensure_initialized);
	// Task 10 contract smoke test: the WorldStore spine's edit sequence, and an
	// AppendResult-free way to push one encoded op through the spine from GDScript.
	ClassDB::bind_method(D_METHOD("edit_seq"), &VoxelWorld::edit_seq);
	ClassDB::bind_method(D_METHOD("append_edit", "op"), &VoxelWorld::append_edit_op);
	ClassDB::bind_method(D_METHOD("is_initialized"), &VoxelWorld::is_initialized);
	ClassDB::bind_method(D_METHOD("request_shader_reload"), &VoxelWorld::request_shader_reload);
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








void VoxelWorld::_ready() {
	// Debug/test facade lives as long as the world; tests reach it through hooks().
	hooks();
	// A scene can be instantiated again after a benchmark/test quit request in the same
	// process. Reset this world's lifetime state before reopening global callback admission.
	voxel_compositor_callbacks_ready(this);
	// Godot only calls _process on a GDExtension node that asks for it.
	set_process(true);
}

VoxelDebugHooks *VoxelWorld::hooks() {
	if (!debug_hooks_) {
		debug_hooks_ = memnew(VoxelDebugHooks);
		debug_hooks_->bind_world(this);
	}
	return debug_hooks_;
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

VoxelWorld::VoxelWorld() {
	// WorldStore is created FIRST so the property setters always have a config
	// to write -- pre-init setter semantics are identical to the plain fields
	// they replace, and context wiring publishes the store from birth.
	store_ = std::make_unique<WorldStore>(ve::WorldConfig{}, new ve::ProceduralFieldGenerator());
	context_.store = store_.get();
	// Task 8: the edit-append spine lives in WorldStore now. Inject its notification ports
	// (this adapter forwards to today's island/consolidation logic). Since Task 9 the store
	// also owns the occupancy grid/inbox and the edit_seq_ atomic. Sinks are never null
	// from this point on, matching append_edit_locked's unguarded expectations.
	store_->set_sinks(this, this);
}

VoxelWorld::~VoxelWorld() {
	if (debug_hooks_) {
		memdelete(debug_hooks_);
		debug_hooks_ = nullptr;
	}
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
	store_->clear_residency(); // slot assignments are meaningless pre-atlas
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
	// CPU cores survive GPU teardown; deleted here exactly where they were
	// before the split, in the same residency -> edit log -> overrides order.
	store_->release_cores();
	store_->pending_edits_.clear();
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
	cfg.atlas_bricks = {store_->config_.atlas_bricks.x, store_->config_.atlas_bricks.y, store_->config_.atlas_bricks.z};
	cfg.max_region_slots = store_->config_.max_region_slots;
	cfg.max_brick_jobs = store_->config_.max_brick_jobs;
	cfg.max_override_bricks = store_->config_.max_override_bricks;
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
	// The four blocks below are the verbatim construction sequence moved into
	// WorldStore; their call positions relative to the GPU setup are load-bearing.
	store_->ensure_edit_log(world_bounds());
	store_->ensure_overrides(atlas_->overrides().capacity());
	if (!atlas_->replay_overrides(device, *store_->overrides_, store_->override_tables_)) {
		UtilityFunctions::printerr("VoxelWorld: override replay into render pool failed");
		teardown_gpu();
		return;
	}
	store_->ensure_residency(world_bounds());
	streamer_ = new WorldStreamer();
	streamer_->initialize(store_->residency_, store_->edit_log_, &store_->edit_mutex(),
			&store_->pending_edits_, atlas_,
			region_pass_, gen_pass_, store_.get(), store_->overrides_,
			&store_->override_tables_);
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
	std::lock_guard<std::mutex> lock(store_->edit_mutex());
	return append_edit_locked(op);
}

Dictionary VoxelWorld::append_edit_op(const PackedByteArray &op_bytes) {
	// Same {touched, rejected} shape VoxelEditTool::apply reports, so suites inspect the
	// result exactly as they inspect tool results (r["rejected"] and friends).
	Dictionary out;
	Array touched, rejected;
	out["touched"] = touched;
	out["rejected"] = rejected;
	if (op_bytes.size() < static_cast<int>(sizeof(ve::EditOp))) {
		UtilityFunctions::printerr("VoxelWorld: append_edit op must be ",
				static_cast<int>(sizeof(ve::EditOp)), " bytes (the ve::EditOp encoding)");
		return out;
	}
	ve::EditOp op{};
	std::memcpy(&op, op_bytes.ptr(), sizeof(ve::EditOp));
	const ve::EditLog::AppendResult r = append_edit(op);
	for (const ve::IVec3 &v : r.touched) touched.push_back(Vector3i(v.x, v.y, v.z));
	for (const ve::IVec3 &v : r.rejected) rejected.push_back(Vector3i(v.x, v.y, v.z));
	return out;
}

ve::EditLog::AppendResult VoxelWorld::append_edit_locked(const ve::EditOp &op,
		bool notify_islands) {
	// The spine (log append, consolidation queueing, seq bump, island notification via the
	// EditSink port, pending_edits_) runs in WorldStore; the VoxelWorld-owned fan-out below
	// stays here under the SAME single lock hold, in the same relative order as before the
	// split.
	if (!store_->edit_log()) return {};
	ve::EditLog::AppendResult r = store_->append_edit_locked(op, notify_islands);
	if (!r.rejected.empty()) {
		edit_rejections_ += static_cast<int>(r.rejected.size());
		UtilityFunctions::printerr("VoxelWorld: region op list full, op rejected (",
				r.rejected[0].x, ", ", r.rejected[0].y, ", ", r.rejected[0].z,
				") — spec §8 fail-soft");
	}
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
	b.origin_bricks = {store_->config_.world_origin_bricks.x, store_->config_.world_origin_bricks.y, store_->config_.world_origin_bricks.z};
	b.size_regions = {store_->config_.world_size_regions.x, store_->config_.world_size_regions.y, store_->config_.world_size_regions.z};
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
	// The CPU cores are shared with the streaming path and outlive both
	// (voxel_world.h); created through the same WorldStore lazy paths as the
	// streaming init, so physics-first worlds get identical objects.
	store_->ensure_edit_log(world_bounds());
	store_->ensure_overrides(store_->config_.max_override_bricks);
	mesh_ = new MeshService();
	MeshPassConfig mcfg;
	mcfg.max_jobs = mesh_jobs_per_frame_;
	mcfg.max_override_bricks = store_->overrides_ ? store_->overrides_->capacity() : store_->config_.max_override_bricks;
	if (!mesh_->start(mcfg)) {
		delete mesh_;
		mesh_ = nullptr;
		return;
	}
	if (!mesh_->replay_overrides(*store_->overrides_, store_->override_tables_)) {
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
		if (!store_->volumes_.pinned(slot)) continue;
		const ve::VolumeData *d = store_->volumes_.get(slot);
		if (d) mesh_->submit_volume(slot, *d);
	}
	ve::ChunkResidencyConfig ccfg;
	ccfg.bounds = world_bounds();
	ccfg.radius_m = physics_radius_m_;
	ccfg.max_chunks = max_collider_chunks_;
	ccfg.max_builds_per_frame = mesh_jobs_per_frame_;
	chunks_ = new ve::ChunkResidency(ccfg);
	colliders_ = new ColliderStreamer();
	colliders_->initialize(chunks_, store_->edit_log_, &store_->edit_mutex(), mesh_, max_collider_chunks_);
	colliders_->set_shape_builds_per_frame(shape_builds_per_frame_);
	colliders_->set_body_bubble_radius_m(physics_bubble_radius_m_);
	// Publish the manager under edit_mutex_: append_edit_locked() can be called from a tool
	// thread and reads island_manager_ while holding that lock, so creation must not expose a
	// half-initialized pointer to it. Also take island_mutex_ (edit_mutex_ -> island_mutex_
	// order, matching teardown) so the render thread's island_slot_count() sees a stable
	// pointer.
	{
		std::lock_guard<std::mutex> lock(store_->edit_mutex());
		std::lock_guard<std::mutex> island_lock(island_mutex_);
		island_manager_ = new IslandManager();
		island_manager_->initialize(this);
	}
	physics_ready_ = true;
}

void VoxelWorld::teardown_physics() {
	std::unique_lock<std::mutex> edit_lock(store_->edit_mutex());
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
			if (!u.to_island_atlas && store_->volumes_.pinned(u.volume_slot))
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
				const int slot = store_->overrides_ ? store_->overrides_->slot_of(brick) : -1;
				if (slot >= 0 && atlas_) atlas_->stored_normals().release_override(rd(), slot);
				store_->overrides_->release(brick);
			}
			if (store_->edit_log_ && store_->edit_log_->op_count(region) > 0)
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
		std::lock_guard<std::mutex> lock(store_->edit_mutex());
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



void VoxelWorld::set_physics_bubble_radius_m(float v) {
	physics_bubble_radius_m_ = v;
	// Applies live: a test (and the editor's inspector) can change the bubble after physics
	// has already been initialized.
	if (colliders_) colliders_->set_body_bubble_radius_m(v);
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
	if (!store_->edit_log_) return h;
	std::lock_guard<std::mutex> lock(store_->edit_mutex());
	// Task 10: through the FieldGenerator seam -- same analytic field, no behavior change.
	const ve::Generator &gen = store_->generator()->sampler();
	const float o[3] = {xz[0], 200.0f, xz[1]};
	const float dir[3] = {0.0f, -1.0f, 0.0f};
	return ve::raycast(gen, *store_->edit_log_, o, dir, 400.0f, &store_->volumes_, store_->overrides_);
}

bool VoxelWorld::release_volume_slot(int slot) {
	// The authoritative copy goes first; only a successful release (never a pinned slot --
	// a pasted volume-add still names it) queues the GPU-side normal teardown.
	const bool freed = store_->volumes_.release(slot);
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






































void VoxelWorld::gather_lod_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out) {
	if (!out) return;
	out->clear();
	std::lock_guard<std::mutex> lock(store_->edit_mutex());
	if (!store_->edit_log_) return;
	float lo[3], hi[3];
	ve::lod_chunk_aabb(level, coord, lo, hi);
	const float pad = std::max(2.0f * ve::lod_cell_size(level), ve::kLatticeFilterPad);
	for (int a = 0; a < 3; a++) {
		lo[a] -= pad;
		hi[a] += pad;
	}
	ve::collect_ops_for_aabb(*store_->edit_log_, lo, hi, out);
	// M4 errata 1: the flattened cross-region list can exceed the cap. A chronological
	// prefix is a valid world state; a suffix could apply an add without the subtract that
	// made room for it.
	if (out->size() > ve::kMaxRegionOps) out->resize(ve::kMaxRegionOps);
}

bool VoxelWorld::snapshot_field_sources(const std::vector<ve::EditOp> &ops, ve::IVec3 brick_lo, ve::IVec3 brick_hi, ve::FieldSourceSnapshot *out) const {
	if (!out || !store_->overrides_) return false;
	out->overrides.clear();
	out->volumes.clear();
	// Copy only prior overrides inside inclusive brick range
	for (int z = brick_lo.z; z <= brick_hi.z; z++)
		for (int y = brick_lo.y; y <= brick_hi.y; y++)
			for (int x = brick_lo.x; x <= brick_hi.x; x++) {
				ve::IVec3 b{x, y, z};
				int slot = store_->overrides_->slot_of(b);
				if (slot >= 0) {
					const ve::OverrideBrick *data = store_->overrides_->data(slot);
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
		const ve::VolumeData *vd = store_->volumes_.get(slot);
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
	float reach = store_->residency_ ? store_->residency_->complete_radius_m() : 0.0f;
	if (reach <= 0.0f) reach = store_->config_.residency_radius_m;
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


















int VoxelWorld::override_table_for_region(ve::IVec3 region) const {
	return store_->override_table_for_region(region);
}

void VoxelWorld::on_edit_appended(const ve::EditOp &op, bool notify_islands) {
	// EditSink adapter (Task 8): called by WorldStore::append_edit_locked with edit_mutex()
	// held, at exactly the point where this logic used to sit inside append_edit_locked.
	// WorldStore has already gated on `notify_islands` and on the op changing region field
	// state; only the manager-presence check remains here. Dies in Phase 3 when
	// IslandManager implements EditSink directly.
	if (notify_islands && island_manager_)
		island_manager_->note_edit(op, store_->edit_seq());
}





void VoxelWorld::pump_consolidation() {
	// This is a frame-pump path: the lifetime guard prevents teardown from racing the
	// transaction, while the edit lock makes the queue, log, CPU store, and table map one
	// consistent state. No worker call below waits for GPU work.
	std::unique_lock<std::mutex> lifetime(render_lifetime_mutex_);
	if (render_shutting_down_) return;
	std::unique_lock<std::mutex> edit_lock(store_->edit_mutex());
	if (!mesh_ || !mesh_->is_valid() || !store_->edit_log_ || !store_->overrides_ || !store_->residency_) return;

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
			const int slot = store_->overrides_ ? store_->overrides_->slot_of(brick) : -1;
			if (slot >= 0 && atlas_) atlas_->stored_normals().release_override(rd(), slot);
			store_->overrides_->release(brick);
		}
		if (!restored)
			UtilityFunctions::printerr(
					"VoxelWorld: render override rollback failed; retaining old edit state");
		// A worker rollback failure means its bytes are not authoritative. Rebuild from the
		// CPU store/table map after releasing the speculative slots and before any requeue.
		bool worker_rebuilt = true;
		if (rebuild_worker)
			worker_rebuilt = mesh_->replay_overrides(*store_->overrides_, store_->override_tables_);
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
			if (store_->residency_->slot_of(r) != consolidation_job_.region_slot) {
				refuse_transaction(true, false);
				return;
			}
			if (atlas_)
				atlas_->set_override_table(rd(), consolidation_job_.region_slot,
						consolidation_table_, consolidation_entries_);
			for (size_t i = 0; i < consolidation_slots_.size(); i++)
				if (ve::OverrideBrick *data = store_->overrides_->data(consolidation_slots_[i]))
					*data = consolidation_baked_[i];
			store_->edit_log_->clear_region_through(r, consolidation_job_.through_seq);
			pending_dirty_.push_back({ve::chunk_of_brick(base),
					ve::chunk_of_brick({base.x + ve::kRegionBricks - 1,
							base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1})});
			if (store_->edit_log_->op_count(r) >= ve::kConsolidateAtOps) queue_consolidation(r);
			float lo[3], first_hi[3], last_lo[3], hi[3];
			ve::brick_world_aabb(base, lo, first_hi);
			ve::brick_world_aabb({base.x + ve::kRegionBricks - 1,
					base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1}, last_lo, hi);
			if (lod_tree_) {
				std::lock_guard<std::mutex> lod_lock(lod_mutex_);
				lod_tree_->mark_dirty(lo, hi);
			}
			if (streamer_) streamer_->queue_region_regeneration_locked(r);
			store_->override_tables_[std::tuple<int, int, int>{r.x, r.y, r.z}] = consolidation_table_;
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
			const bool present = store_->overrides_->slot_of(brick) >= 0;
			const int slot = store_->overrides_->acquire(brick);
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
		if (store_->residency_->slot_of(region) != consolidation_job_.region_slot) {
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
	const int region_slot = store_->residency_->slot_of(region);
	if (region_slot < 0) {
		consolidation_refusals_++;
		requeue(region);
		return;
	}
	const std::vector<ve::EditOp> &ops = store_->edit_log_->ops(region);
	if (ops.empty()) return;
	ConsolidateJob job;
	job.region = region;
	job.region_slot = region_slot;
	job.ops = ops;
	const std::vector<uint64_t> &seqs = store_->edit_log_->seqs(region);
	job.through_seq = seqs.empty() ? 0 : seqs.back();
	ve::plan_consolidation(job.ops.data(), static_cast<int>(job.ops.size()), region, &job.bricks);
	if (!job.bricks.empty()) {
		// Spec requires collect + snapshot while edit_mutex_ is held: edit_lock above spans this
		// whole function, so the overrides, edit log, and volumes are read in one consistent state.
		ve::IVec3 lo = job.bricks[0], hi = job.bricks[0];
		for (const auto &b : job.bricks) { lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z); hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z); }
		if (!snapshot_field_sources(job.ops, lo, hi, &job.source)) {
			consolidation_refusals_++;
			requeue(region);
			return;
		}
	}
	int needed_slots = 0;
	for (const ve::IVec3 brick : job.bricks) if (store_->overrides_->slot_of(brick) < 0) needed_slots++;
	if (job.bricks.empty() || needed_slots > store_->overrides_->capacity() - store_->overrides_->used()) {
		consolidation_refusals_++;
		requeue(region);
		return;
	}
	const std::tuple<int, int, int> key{region.x, region.y, region.z};
	const auto found = store_->override_tables_.find(key);
	const int old_table = found == store_->override_tables_.end() ? -1 : found->second;
	int table = old_table;
	if (table < 0) {
		std::vector<bool> used(OverridePool::kMaxOverrideTables, false);
		for (const auto &it : store_->override_tables_)
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
				const int slot = store_->overrides_->slot_of(brick);
				if (slot < 0) continue;
				consolidation_old_entries_.emplace_back(
						ve::WorldBounds::brick_index_in_region(brick), slot);
				consolidation_old_slots_.push_back(slot);
				consolidation_old_bricks_.push_back(*store_->overrides_->data(slot));
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
		std::lock_guard<std::mutex> lock(store_->edit_mutex());
		if (!store_->edit_log_) return false;
		ve::collect_ops_for_aabb(*store_->edit_log_, wlo, whi, &job->ops);
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
	// Task 10: through the FieldGenerator seam -- same analytic field, no behavior change.
	const ve::Generator &gen = store_->generator()->sampler();
	ve::extract_island_volume(gen, job->ops.data(), static_cast<int>(job->ops.size()),
			&store_->volumes_, job->origin, job->voxel, job->dim, aabbs.data(),
			static_cast<int>(boxes->size()), &cpu);
	*out = std::move(cpu);
	return true;
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
	cam.dims[0] = store_->config_.world_size_regions.x; cam.dims[1] = store_->config_.world_size_regions.y;
	cam.dims[2] = store_->config_.world_size_regions.z;
	cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = store_->config_.atlas_bricks.x; cam.atlas_bricks[1] = store_->config_.atlas_bricks.y;
	cam.atlas_bricks[2] = store_->config_.atlas_bricks.z;
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








































