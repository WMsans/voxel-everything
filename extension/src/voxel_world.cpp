#include "voxel_world.h"
#include "debug/hooks.h"
#include "render/orchestrator.h"
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
#include "mesh/consolidation.h"
#include "generator/generator.h"
#include "generator/field_generator.h"
#include "world/brick_eval.h"
#include "world/brick_flags.h"
#include "world/brick_mip.h"
#include "world/material_table.h"
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

void godot::voxel_compositor_callbacks_ready(RenderOrchestrator *render) {
	std::lock_guard<std::mutex> admission(g_voxel_compositor_admission_mutex);
	// Same lock order as before the Task 13 move: admission -> render_lifetime_mutex_
	// (taken inside reopen_admission()).
	render->reopen_admission();
	g_voxel_compositor_callbacks_enabled = true;
}

void godot::voxel_compositor_callbacks_shutdown_started(RenderOrchestrator *render) {
	std::lock_guard<std::mutex> admission(g_voxel_compositor_admission_mutex);
	g_voxel_compositor_callbacks_enabled = false;
	// Same lock order as before the Task 13 move: admission -> render_lifetime_mutex_
	// (taken inside close_admission()).
	render->close_admission();
}

// One-line delegations into RenderOrchestrator (Task 13), which owns the admission
// counters and the shutdown latches now; bodies moved verbatim.
bool VoxelWorld::try_begin_render_callback() {
	return context_.render->try_begin_render_callback();
}

void VoxelWorld::end_render_callback() {
	context_.render->end_render_callback();
}

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("hooks"), &VoxelWorld::hooks);
	ClassDB::bind_method(D_METHOD("material_table"), &VoxelWorld::material_table);
	// Name shared with orchestrator.cpp's render-thread Callable dispatch
	// (kShutdownRenderResourcesOnRenderThread) so the two sites cannot drift.
	ClassDB::bind_method(D_METHOD(kShutdownRenderResourcesOnRenderThread),
			&VoxelWorld::shutdown_render_resources_on_render_thread);
	ClassDB::bind_method(D_METHOD("shutdown_render_resources"),
			&VoxelWorld::shutdown_render_resources);
	// Task 14 contract smoke test: the admission guard itself, so GDScript can pin the
	// refuse-after-shutdown / re-admit-after-reopen behavior at the orchestrator boundary.
	ClassDB::bind_method(D_METHOD("try_begin_render_callback"),
			&VoxelWorld::try_begin_render_callback);
	ClassDB::bind_method(D_METHOD("end_render_callback"), &VoxelWorld::end_render_callback);
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
	ClassDB::bind_method(D_METHOD("set_near_field_scale", "v"), &VoxelWorld::set_near_field_scale);
	ClassDB::bind_method(D_METHOD("get_near_field_scale"), &VoxelWorld::get_near_field_scale);
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_field_scale", PROPERTY_HINT_RANGE, "0.1,1.0,0.01"),
			"set_near_field_scale", "get_near_field_scale");
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

// Task 14: bodies moved verbatim into RenderOrchestrator (beauty_mutex_, quality_tier_,
// beauty_ and the effect-name table went with them); these one-line delegations keep the
// ClassDB surface and every call site compiling unchanged. Same threads as before the
// move: setters on the main thread, value snapshots wherever beauty_settings() is taken.

void VoxelWorld::set_quality_tier(int v) {
	context_.render->set_quality_tier(v);
}

int VoxelWorld::get_quality_tier() const {
	return context_.render->quality_tier();
}

void VoxelWorld::set_effect_enabled(const String &name, bool on) {
	context_.render->set_effect_enabled(name, on);
}

bool VoxelWorld::get_effect_enabled(const String &name) const {
	return context_.render->get_effect_enabled(name);
}

ve::BeautySettings VoxelWorld::beauty_settings() const {
	return context_.render->beauty_settings();
}








void VoxelWorld::_ready() {
	// Debug/test facade lives as long as the world; tests reach it through hooks().
	hooks();
	// A scene can be instantiated again after a benchmark/test quit request in the same
	// process. Reset this world's lifetime state before reopening global callback admission.
	voxel_compositor_callbacks_ready(context_.render);
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

Array VoxelWorld::material_table() const {
	Array out;
	for (int i = 0; i < ve::kMaterialCount; i++) {
		const ve::MaterialDef &m = ve::kMaterials[i];
		Dictionary d;
		d["id"] = i + 1; // material 0 is air; layer i serves id i + 1
		d["name"] = String(m.name);
		d["asset"] = String(m.asset);
		d["hardness"] = m.hardness;
		d["glow"] = m.glow;
		d["glow_color"] = Color(m.glow_rgb[0], m.glow_rgb[1], m.glow_rgb[2]);
		d["albedo"] = Color(m.flat_albedo[0], m.flat_albedo[1], m.flat_albedo[2]);
		out.push_back(d);
	}
	return out;
}

void VoxelWorld::_process(double delta) {
	// Unconditional: the grid and consolidation queue must keep draining even with physics
	// disabled, because edits and the debug hooks share this path.
	drain_occupancy();
	consolidation_->pump_async();
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
	// Task 15: the LoD runtime moves off this class into LodSystem (the lod mutex travels
	// with it). Created BEFORE RenderOrchestrator: the orchestrator's teardown interleaves
	// with the LoD pool/tree/page maps, so it takes this system's state slots below. The
	// render collaborator is the ADDRESS of context_.render precisely because the
	// orchestrator does not exist yet at this point; LodSystem re-reads it at every use.
	lod_ = std::make_unique<LodSystem>(LodSystem::Collaborators{
			.store = store_.get(),
			.render = &context_.render,
			.mesh = &mesh_,
			.near_field_enabled = &near_field_enabled_,
			.ensure_initialized_thunk = [](void *self) {
				static_cast<VoxelWorld *>(self)->ensure_initialized();
			},
			.ensure_initialized_self = this,
	});
	context_.lod = lod_.get();
	// Task 12: the GPU pass graph + device ownership move into RenderOrchestrator. Created
	// BEFORE the consolidation coordinator, whose collaborators take addresses of the
	// orchestrator's atlas/device slots (handles-only; re-read at every use).
	render_ = std::make_unique<RenderOrchestrator>(RenderOrchestrator::Collaborators{
			.use_local_device = &use_local_device_,
			.store = store_.get(),
			.streamer = &streamer_,
			.normal_pool_bytes = &normal_pool_bytes_,
			.last_hiz_readback_was_pending = &last_hiz_readback_was_pending_,
			.last_hiz_readback_was_drained = &last_hiz_readback_was_drained_,
			// Task 13: teardown interleaving + admission/lifetime handles. callback_owner
			// is this node as an Object, solely the Callable target for the queued
			// render-thread teardown; render_ dies with this node, so it cannot dangle.
			.initialized = &initialized_,
			.island_mutex = &island_mutex_,
			.island_slots = &island_slots_,
			// Task 15: the LoD state slots live on LodSystem now.
			.lod_pool = context_.lod->pool_slot(),
			.lod_tree = context_.lod->tree_slot(),
			.lod_pages_of = context_.lod->pages_of_slot(),
			.lod_page_quads = context_.lod->page_quads_slot(),
			.lod_overflow_logged = context_.lod->overflow_logged_slot(),
			.callback_owner = this,
			// Task 14: effect toggles stay world properties (they gate non-beauty behavior
			// too); reload's re-init arm stays VoxelWorld::ensure_initialized() via a
			// captureless thunk -- no VoxelWorld* is handed to the orchestrator.
			.islands_enabled = &islands_enabled_,
			.near_field_enabled = &near_field_enabled_,
			.ensure_initialized_thunk = [](void *self) {
				static_cast<VoxelWorld *>(self)->ensure_initialized();
			},
			.ensure_initialized_self = this,
	});
	context_.render = render_.get();
	// Task 11: the consolidation state machine moves off this class into the coordinator.
	// It receives ADDRESSES of the fields below (they are created lazily and destroyed
	// across teardown cycles, so it re-reads them at every use). The store's ConsolidationSink
	// port is satisfied by the coordinator directly now; VoxelWorld keeps only its EditSink
	// adapter half until IslandManager implements that port itself.
	consolidation_ = std::make_unique<ConsolidationCoordinator>(store_.get(),
			ConsolidationCoordinator::Collaborators{
					.atlas = context_.render->atlas_slot(),
					.mesh = &mesh_,
					.streamer = &streamer_,
					// Task 15: tree/mutex handles now address LodSystem's state.
					.lod_tree = context_.lod->tree_slot(),
					.lod_mutex = context_.lod->mutex_slot(),
					.pending_dirty = &pending_dirty_,
					.use_local_device = &use_local_device_,
					.main_rd = context_.render->main_rd_slot(),
					.local_rd = context_.render->local_rd_slot(),
					// Task 13: the lifetime mutex/flag moved into RenderOrchestrator; the
					// coordinator keeps its handles-only view of them.
					.render_lifetime_mutex = context_.render->render_lifetime_mutex_slot(),
					.render_shutting_down = context_.render->render_shutting_down_slot(),
			});
	context_.consolidation = consolidation_.get();
	// Task 8: the edit-append spine lives in WorldStore now. Inject its notification ports
	// (the edit sink forwards to today's island logic; consolidation is the coordinator).
	// Since Task 9 the store also owns the occupancy grid/inbox and the edit_seq_ atomic.
	// Sinks are never null from this point on, matching append_edit_locked's unguarded
	// expectations.
	store_->set_sinks(this, consolidation_.get());
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

// Moved verbatim into RenderOrchestrator (Task 12); one-line delegations so the
// compositor's world->downsample_history() and world->finish_beauty_frame() compile
// unchanged.
bool VoxelWorld::downsample_history(RenderingDevice *rd, RID src, GBuffer &gb) {
	return context_.render->downsample_history(rd, src, gb);
}

void VoxelWorld::finish_beauty_frame(const float view_proj[16]) {
	context_.render->finish_beauty_frame(view_proj);
}

void VoxelWorld::teardown_gpu() {
	// Whole method lives in RenderOrchestrator now (Task 13): the three teardown halves
	// and the interleaved world-owned statements (streamer drain/delete, residency clear,
	// island high-water mark, LoD pool/tree/page maps) run there via Collaborator
	// addresses, so the deallocation order is statement-for-statement identical.
	context_.render->teardown_gpu();
}

void VoxelWorld::shutdown_render_resources_on_render_thread() {
	// ClassDB-bound (see _bind_methods): the render-thread Callable targets this node.
	context_.render->shutdown_render_resources_on_render_thread();
}

void VoxelWorld::shutdown_render_resources() {
	// Body moved verbatim into RenderOrchestrator (Task 13); admission close, callback
	// drain, and the render-thread Callable dispatch sequence are unchanged.
	context_.render->shutdown_render_resources();
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
	store_->pending_edits()->clear();
	overflow_seen_ = 0;
	// Task 15: pool -> tree -> page maps moved verbatim into LodSystem::teardown(); run at
	// exactly the position where the statements used to sit here.
	lod_->teardown();
	// Device drop moved verbatim into RenderOrchestrator (Task 12): the owned local
	// device is deleted, the borrowed main pointer merely forgotten.
	context_.render->release_devices();
}

void VoxelWorld::ensure_initialized() {
	// Admission gate moved with the lifetime state (Task 13); same mutex-guarded check.
	if (context_.render->shutdown_in_progress()) return;
	if (initialized_) return;
	// Device acquisition + the whole GPU graph construction live in RenderOrchestrator
	// (Task 12), in the exact allocation order this body used. Only the lifetime flag
	// and the failure routing remain here.
	RenderingDevice *device = context_.render->acquire_device();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return;
	}
	switch (context_.render->ensure_gpu_graph(device)) {
	case RenderOrchestrator::GpuInitResult::kOk:
		initialized_ = true;
		break;
	case RenderOrchestrator::GpuInitResult::kAtlasFailed:
		// Only the half-built atlas existed; it deleted itself, exactly as before.
		break;
	case RenderOrchestrator::GpuInitResult::kFailed:
		// A later stage refused; a partial graph exists. Same recovery as the pre-split
		// body's per-stage teardown_gpu() + return.
		teardown_gpu();
		break;
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
	// The LoD half of the fan-out moved verbatim into LodSystem::note_edit (Task 15): it
	// checks its tree, takes lod_mutex under edit_mutex exactly as this block did.
	if (!r.touched.empty()) context_.lod->note_edit(op);
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
	return context_.render->rd(); // one-line delegation (Task 12)
}

ve::WorldBounds VoxelWorld::world_bounds() const {
	return ve::world_bounds(store_->config());
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
	store_->ensure_overrides(store_->config().max_override_bricks);
	mesh_ = new MeshService();
	MeshPassConfig mcfg;
	mcfg.max_jobs = mesh_jobs_per_frame_;
	mcfg.max_override_bricks = store_->overrides() ? store_->overrides()->capacity() : store_->config().max_override_bricks;
	if (!mesh_->start(mcfg)) {
		delete mesh_;
		mesh_ = nullptr;
		return;
	}
	if (!mesh_->replay_overrides(*store_->overrides(), store_->override_tables())) {
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
		if (!store_->volumes().pinned(slot)) continue;
		const ve::VolumeData *d = store_->volumes().get(slot);
		if (d) mesh_->submit_volume(slot, *d);
	}
	ve::ChunkResidencyConfig ccfg;
	ccfg.bounds = world_bounds();
	ccfg.radius_m = physics_radius_m_;
	ccfg.max_chunks = max_collider_chunks_;
	ccfg.max_builds_per_frame = mesh_jobs_per_frame_;
	chunks_ = new ve::ChunkResidency(ccfg);
	colliders_ = new ColliderStreamer();
	colliders_->initialize(chunks_, store_->edit_log(), &store_->edit_mutex(), mesh_, max_collider_chunks_);
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
			if (!u.to_island_atlas && store_->volumes().pinned(u.volume_slot))
				keep.push_back(std::move(u));
		island_uploads_.swap(keep);
		island_descs_.clear();
		island_descs_dirty_ = false;
	}
	// The worker is going away, but the render atlas and CPU store survive physics teardown.
	// An in-flight transaction may already have acquired slots and staged new bytes there;
	// restore the old consumer state before releasing those speculative slots. Never leave a
	// new table pointing at bytes whose CPU transaction is about to be discarded. The block
	// moved verbatim into ConsolidationCoordinator (Task 11); edit_lock is held exactly as
	// it was when this state lived here.
	consolidation_->rollback_in_flight_for_worker_teardown();
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
	if (!store_->edit_log()) return h;
	std::lock_guard<std::mutex> lock(store_->edit_mutex());
	// Task 10: through the FieldGenerator seam -- same analytic field, no behavior change.
	const ve::Generator &gen = store_->generator()->sampler();
	const float o[3] = {xz[0], 200.0f, xz[1]};
	const float dir[3] = {0.0f, -1.0f, 0.0f};
	return ve::raycast(gen, *store_->edit_log(), o, dir, 400.0f, &store_->volumes(), store_->overrides());
}

bool VoxelWorld::release_volume_slot(int slot) {
	// The authoritative copy goes first; only a successful release (never a pinned slot --
	// a pasted volume-add still names it) queues the GPU-side normal teardown.
	const bool freed = store_->volumes().release(slot);
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
		if (atlas()) atlas()->stored_normals().release_volume(device, slot);
	}
	for (const IslandUpload &u : uploads) {
		// SDF/material and compact normals land ONCE, in the shared authoritative pools,
		// indexed by the volume slot. An island upload additionally refreshes its mip at
		// the atlas slot; a field-volume upload follows the identical volume/normal path
		// without one. A missing/malformed/failed normal payload is fail-soft: the pool
		// publishes -1 and the shader falls back to differentiating the R8 atlas.
		if (atlas() && u.volume_slot >= 0) {
			if (!atlas()->volumes().upload(device, u.volume_slot, u.data))
				UtilityFunctions::printerr("VoxelWorld: field volume upload failed for slot ",
						u.volume_slot);
			atlas()->stored_normals().upload_volume(device, u.volume_slot, u.data);
		} else if (!atlas() && u.to_island_atlas) {
			UtilityFunctions::printerr("VoxelWorld: no GpuAtlas for island upload of slot ",
					u.volume_slot);
		}
		if (u.to_island_atlas && islands() && u.atlas_slot >= 0 &&
				!islands()->upload_mip(device, u.atlas_slot, u.data))
			UtilityFunctions::printerr("VoxelWorld: island mip upload failed for slot ",
					u.atlas_slot);
		if (!u.to_island_atlas)
			debug_field_volume_upload_count_.fetch_add(1, std::memory_order_relaxed);
	}
	if (dirty && islands())
		islands()->upload_descriptors(device, descs.data(), static_cast<int>(descs.size()));
	return static_cast<int>(uploads.size());
}






































// Task 15: gather_lod_ops/ensure_lod/lod_fade_band/lod_tick/prepare_lod_raster[_locked]/
// prepare_lod_shadow_raster moved verbatim into LodSystem; these one-line delegations keep
// the compositor, the debug facade and ClassDB compiling unchanged.
void VoxelWorld::gather_lod_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out) {
	context_.lod->gather_ops(level, coord, out);
}

// Moved verbatim into WorldStore (Task 11); one-line delegation so hooks and
// IslandManager compile unchanged.
bool VoxelWorld::snapshot_field_sources(const std::vector<ve::EditOp> &ops, ve::IVec3 brick_lo, ve::IVec3 brick_hi, ve::FieldSourceSnapshot *out) const {
	return store_->snapshot_field_sources(ops, brick_lo, brick_hi, out);
}

// Task 15 one-line delegations into LodSystem, where tick/fade-band/raster-prep moved
// verbatim; the compositor, the debug facade and ClassDB compile unchanged.
void VoxelWorld::lod_tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ) {
	context_.lod->tick(cam, occ);
}

void VoxelWorld::prepare_lod_raster() {
	context_.lod->prepare_raster();
}

void VoxelWorld::prepare_lod_shadow_raster() {
	context_.lod->prepare_shadow_raster();
}

void VoxelWorld::lod_fade_band(float *fade_start, float *fade_end) const {
	context_.lod->fade_band(fade_start, fade_end);
}

int VoxelWorld::override_table_for_region(ve::IVec3 region) const {
	return store_->override_table_for_region(region);
}

void VoxelWorld::on_edit_appended(const ve::EditOp &op, bool notify_islands) {
	// EditSink adapter (Task 8): called by WorldStore::append_edit_locked with edit_mutex()
	// held, at exactly the point where this logic used to sit inside append_edit_locked.
	// WorldStore has already gated on `notify_islands` and on the op changing region field
	// state; only the manager-presence check remains here. Permanent by ruling: IslandManager
	// keeps its own notification path; VoxelWorld remains the EditSink.
	if (notify_islands && island_manager_)
		island_manager_->note_edit(op, store_->edit_seq());
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
		if (!store_->edit_log()) return false;
		ve::collect_ops_for_aabb(*store_->edit_log(), wlo, whi, &job->ops);
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
			&store_->volumes(), job->origin, job->voxel, job->dim, aabbs.data(),
			static_cast<int>(boxes->size()), &cpu);
	*out = std::move(cpu);
	return true;
}

















bool VoxelWorld::render_probe_pixel(Vector3 origin, Vector3 dir) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas() || !material_atlas() || !raymarch_pass())
		return false;
	// The probe is a read-only diagnostic: it must not mutate the streamed world.
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = store_->config().world_size_regions.x; cam.dims[1] = store_->config().world_size_regions.y;
	cam.dims[2] = store_->config().world_size_regions.z;
	cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = store_->config().atlas_bricks.x; cam.atlas_bricks[1] = store_->config().atlas_bricks.y;
	cam.atlas_bricks[2] = store_->config().atlas_bricks.z;
	const uint32_t flags = ve::pack_flags(beauty_settings());
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass()->render(device, *atlas(), islands(), RID(), cam, 1, 1,
			kNoEdit))
		return false;
	device->submit();
	device->sync();
	return true;
}


















// Preflight_shaders moved verbatim into RenderOrchestrator (Task 13); the reload latch,
// pump machinery and bookkeeping moved verbatim into RenderOrchestrator (Task 14).

void VoxelWorld::request_shader_reload() {
	context_.render->request_shader_reload();
}

void VoxelWorld::pump_shader_reload() {
	context_.render->pump_shader_reload();
}








































