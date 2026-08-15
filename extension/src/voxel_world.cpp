#include "voxel_world.h"
#include "render/gpu_atlas.h"
#include "render/camera_params.h"
#include "render/raymarch_pass.h"
#include "render/composite_pass.h"
#include "render/region_pass.h"
#include "render/brick_gen_pass.h"
#include "render/world_streamer.h"
#include "render/shader_loader.h"
#include "render/mesh_pass.h"
#include "render/mesh_service.h"
#include "physics/collider_streamer.h"
#include "mesh/dual_contour.h"
#include "mesh/mesh_chunk.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "world/brick_mip.h"
#include "world/raycast.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <iterator>

using namespace godot;

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_use_local_device", "v"), &VoxelWorld::set_use_local_device);
	ClassDB::bind_method(D_METHOD("get_use_local_device"), &VoxelWorld::get_use_local_device);
	ClassDB::bind_method(D_METHOD("set_atlas_bricks", "v"), &VoxelWorld::set_atlas_bricks);
	ClassDB::bind_method(D_METHOD("get_atlas_bricks"), &VoxelWorld::get_atlas_bricks);
	ClassDB::bind_method(D_METHOD("set_max_region_slots", "v"), &VoxelWorld::set_max_region_slots);
	ClassDB::bind_method(D_METHOD("get_max_region_slots"), &VoxelWorld::get_max_region_slots);
	ClassDB::bind_method(D_METHOD("set_max_brick_jobs", "v"), &VoxelWorld::set_max_brick_jobs);
	ClassDB::bind_method(D_METHOD("get_max_brick_jobs"), &VoxelWorld::get_max_brick_jobs);
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
	ClassDB::bind_method(D_METHOD("set_max_collider_chunks", "v"), &VoxelWorld::set_max_collider_chunks);
	ClassDB::bind_method(D_METHOD("get_max_collider_chunks"), &VoxelWorld::get_max_collider_chunks);
	ClassDB::bind_method(D_METHOD("set_mesh_jobs_per_frame", "v"), &VoxelWorld::set_mesh_jobs_per_frame);
	ClassDB::bind_method(D_METHOD("get_mesh_jobs_per_frame"), &VoxelWorld::get_mesh_jobs_per_frame);
	ClassDB::bind_method(D_METHOD("set_shape_builds_per_frame", "v"), &VoxelWorld::set_shape_builds_per_frame);
	ClassDB::bind_method(D_METHOD("get_shape_builds_per_frame"), &VoxelWorld::get_shape_builds_per_frame);
	ClassDB::bind_method(D_METHOD("debug_init_physics"), &VoxelWorld::debug_init_physics);
	ClassDB::bind_method(D_METHOD("debug_teardown_physics"), &VoxelWorld::debug_teardown_physics);
	ClassDB::bind_method(D_METHOD("debug_mesh_lattice_diff", "chunk"), &VoxelWorld::debug_mesh_lattice_diff);
	ClassDB::bind_method(D_METHOD("debug_mesh_diff", "chunk"), &VoxelWorld::debug_mesh_diff);
	ClassDB::bind_method(D_METHOD("debug_mesh_submit", "chunks"), &VoxelWorld::debug_mesh_submit);
	ClassDB::bind_method(D_METHOD("debug_mesh_collect"), &VoxelWorld::debug_mesh_collect);
	ClassDB::bind_method(D_METHOD("debug_physics_frame", "center"), &VoxelWorld::debug_physics_frame);
	ClassDB::bind_method(D_METHOD("debug_physics_stats"), &VoxelWorld::debug_physics_stats);
	ClassDB::bind_method(D_METHOD("debug_perf_stats"), &VoxelWorld::debug_perf_stats);
	ClassDB::bind_method(D_METHOD("debug_body_of_chunk", "chunk"), &VoxelWorld::debug_body_of_chunk);
	ClassDB::bind_method(D_METHOD("ensure_initialized"), &VoxelWorld::ensure_initialized);
	ClassDB::bind_method(D_METHOD("is_initialized"), &VoxelWorld::is_initialized);
	ClassDB::bind_method(D_METHOD("debug_raymarch_pixel", "origin", "dir"), &VoxelWorld::debug_raymarch_pixel);
	ClassDB::bind_method(D_METHOD("debug_raymarch_probe", "origin", "dir"), &VoxelWorld::debug_raymarch_probe);
	ClassDB::bind_method(D_METHOD("debug_sdf_atlas"), &VoxelWorld::debug_sdf_atlas);
	ClassDB::bind_method(D_METHOD("debug_local_rd"), &VoxelWorld::debug_local_rd);
	ClassDB::bind_method(D_METHOD("debug_load_shader", "res_path"), &VoxelWorld::debug_load_shader);
	ClassDB::bind_method(D_METHOD("debug_eval_field", "p", "ops", "op_count"), &VoxelWorld::debug_eval_field);
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
	ClassDB::bind_method(D_METHOD("debug_stream_frame", "cam"), &VoxelWorld::debug_stream_frame);
	ClassDB::bind_method(D_METHOD("debug_stream_stats"), &VoxelWorld::debug_stream_stats);
	ClassDB::bind_method(D_METHOD("debug_slot_of_region", "region"), &VoxelWorld::debug_slot_of_region);
	ClassDB::bind_method(D_METHOD("debug_region_map_entry", "region"), &VoxelWorld::debug_region_map_entry);
	ClassDB::bind_method(D_METHOD("debug_region_map_consistent"), &VoxelWorld::debug_region_map_consistent);
	ClassDB::bind_method(D_METHOD("debug_raycast", "origin", "dir"), &VoxelWorld::debug_raycast);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_local_device"), "set_use_local_device", "get_use_local_device");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "atlas_bricks"), "set_atlas_bricks", "get_atlas_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_region_slots"), "set_max_region_slots", "get_max_region_slots");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_brick_jobs"), "set_max_brick_jobs", "get_max_brick_jobs");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_origin_bricks"), "set_world_origin_bricks", "get_world_origin_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_size_regions"), "set_world_size_regions", "get_world_size_regions");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "residency_radius_m"), "set_residency_radius_m", "get_residency_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "physics_enabled"), "set_physics_enabled", "get_physics_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "physics_center_path"), "set_physics_center_path", "get_physics_center_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "physics_radius_m"), "set_physics_radius_m", "get_physics_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_collider_chunks"), "set_max_collider_chunks", "get_max_collider_chunks");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_jobs_per_frame"), "set_mesh_jobs_per_frame", "get_mesh_jobs_per_frame");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shape_builds_per_frame"), "set_shape_builds_per_frame", "get_shape_builds_per_frame");
}

void VoxelWorld::_ready() {
	// Godot only calls _process on a GDExtension node that asks for it.
	set_process(true);
}

void VoxelWorld::_process(double) {
	if (!physics_enabled_ || physics_center_path_.is_empty()) return;
	Node3D *anchor = Object::cast_to<Node3D>(get_node_or_null(physics_center_path_));
	if (!anchor) return;
	ensure_physics_initialized();
	physics_tick(anchor->get_global_position());
}

VoxelWorld::~VoxelWorld() {}

void VoxelWorld::teardown_gpu() {
	// Passes before the atlas: their uniform sets reference atlas RIDs, and freeing a
	// texture cascades to referencing sets (M1's documented order).
	if (composite_pass_) { delete composite_pass_; composite_pass_ = nullptr; }
	if (raymarch_pass_) { delete raymarch_pass_; raymarch_pass_ = nullptr; }
	if (gen_pass_) { delete gen_pass_; gen_pass_ = nullptr; }
	if (region_pass_) { delete region_pass_; region_pass_ = nullptr; }
	if (streamer_) { delete streamer_; streamer_ = nullptr; }
	if (residency_) { residency_->clear(); } // slot assignments are meaningless pre-atlas
	if (atlas_) { delete atlas_; atlas_ = nullptr; }
	initialized_ = false;
}

void VoxelWorld::_exit_tree() {
	teardown_physics();
	teardown_gpu();
	if (residency_) { delete residency_; residency_ = nullptr; }
	if (edit_log_) { delete edit_log_; edit_log_ = nullptr; }
	pending_edits_.clear();
	overflow_seen_ = 0;
	if (local_rd_) {
		memdelete(local_rd_);
		local_rd_ = nullptr;
	}
	main_rd_ = nullptr;
}

void VoxelWorld::ensure_initialized() {
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
	cfg.bounds = world_bounds();
	if (!atlas_->initialize(device, cfg)) { delete atlas_; atlas_ = nullptr; return; }
	region_pass_ = new RegionPass();
	if (!region_pass_->initialize(device, *atlas_)) { teardown_gpu(); return; }
	gen_pass_ = new BrickGenPass();
	if (!gen_pass_->initialize(device, *atlas_)) { teardown_gpu(); return; }
	if (!edit_log_) edit_log_ = new ve::EditLog(world_bounds());
	if (!residency_) {
		ve::ResidencyConfig rcfg;
		rcfg.bounds = world_bounds();
		rcfg.radius_m = residency_radius_m_;
		rcfg.max_region_slots = max_region_slots_;
		residency_ = new ve::RegionResidency(rcfg);
	}
	streamer_ = new WorldStreamer();
	streamer_->initialize(residency_, edit_log_, &edit_mutex_, &pending_edits_, atlas_,
			region_pass_, gen_pass_);
	raymarch_pass_ = new RaymarchPass();
	raymarch_pass_->initialize(device);
	composite_pass_ = new CompositePass();
	composite_pass_->initialize(device);
	initialized_ = true;
}

ve::EditLog::AppendResult VoxelWorld::append_edit(const ve::EditOp &op) {
	std::lock_guard<std::mutex> lock(edit_mutex_);
	if (!edit_log_) return {};
	ve::EditLog::AppendResult r = edit_log_->append(op);
	if (!r.rejected.empty()) {
		UtilityFunctions::printerr("VoxelWorld: region op list full, op rejected (",
				r.rejected[0].x, ", ", r.rejected[0].y, ", ", r.rejected[0].z,
				") — spec §8 fail-soft");
	}
	pending_edits_.push_back({op, r});
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

void VoxelWorld::ensure_physics_initialized() {
	if (physics_ready_) return;
	// The CPU cores are shared with the streaming path and outlive both (voxel_world.h).
	if (!edit_log_) edit_log_ = new ve::EditLog(world_bounds());
	mesh_ = new MeshService();
	MeshPassConfig mcfg;
	mcfg.max_jobs = mesh_jobs_per_frame_;
	if (!mesh_->start(mcfg)) {
		delete mesh_;
		mesh_ = nullptr;
		return;
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
	physics_ready_ = true;
}

void VoxelWorld::teardown_physics() {
	physics_ready_ = false;
	// Colliders first: they hold the mesher's results and the residency's slots. Deleting the
	// service joins its thread, which frees the device and the pass on the thread that made
	// them; nothing else may outlive that.
	if (colliders_) { delete colliders_; colliders_ = nullptr; }
	if (mesh_) { delete mesh_; mesh_ = nullptr; }
	if (chunks_) { delete chunks_; chunks_ = nullptr; }
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		pending_dirty_.clear();
	}
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
	const int actions = colliders_->run_frame(center.x, center.y, center.z);
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
	d["phys_body_ms"] = colliders_ ? colliders_->last_body_ms() : 0.0f;
	d["phys_tris"] = colliders_ ? colliders_->last_tris() : 0;
	d["phys_plan_ms"] = colliders_ ? colliders_->last_plan_ms() : 0.0f;
	d["phys_submit_ms"] = colliders_ ? colliders_->last_submit_ms() : 0.0f;
	d["stream_total_ms"] = streamer_ ? streamer_->last_total_ms() : 0.0f;
	d["stream_readback_ms"] = streamer_ ? streamer_->last_readback_ms() : 0.0f;
	return d;
}

int VoxelWorld::debug_physics_frame(Vector3 center) {
	ensure_physics_initialized();
	return physics_tick(center);
}

Dictionary VoxelWorld::debug_physics_stats() {
	Dictionary d;
	d["chunks_resident"] = chunks_ ? chunks_->resident_count() : 0;
	d["chunks_pending"] = chunks_ ? chunks_->pending_count() : 0;
	d["probe_cache"] = chunks_ ? chunks_->probe_cache_size() : 0;
	d["bodies"] = colliders_ ? colliders_->active_bodies() : 0;
	d["builds"] = colliders_ ? colliders_->builds_last_frame() : 0;
	d["queued"] = colliders_ ? colliders_->queued_results() : 0;
	d["failures"] = colliders_ ? colliders_->failures() : 0;
	d["build_ms"] = colliders_ ? colliders_->last_build_ms() : 0.0f;
	d["collect_ms"] = colliders_ ? colliders_->last_collect_ms() : 0.0f;
	return d;
}

RID VoxelWorld::debug_body_of_chunk(Vector3i chunk) {
	if (!chunks_ || !colliders_) return RID();
	return colliders_->body_of_slot(chunks_->slot_of({chunk.x, chunk.y, chunk.z}));
}

bool VoxelWorld::debug_init_physics() {
	ensure_physics_initialized();
	return physics_ready_;
}

void VoxelWorld::debug_teardown_physics() {
	teardown_physics();
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
						p[0], p[1], p[2]).sdf;
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
	const MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
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
						g.origin[2] + (z - 1) * g.cell_size).sdf;
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
				gpu.positions[v * 3], gpu.positions[v * 3 + 1], gpu.positions[v * 3 + 2]).sdf);
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
				mid.x + step.x, mid.y + step.y, mid.z + step.z).sdf;
		const float in_side = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
				mid.x - step.x, mid.y - step.y, mid.z - step.z).sdf;
		tri_sampled++;
		if (out_side <= in_side) winding_bad++;
	}
	d["max_surface_sdf"] = max_sdf;
	d["verts_off_10cm"] = off_10cm;
	d["winding_bad"] = winding_bad;
	d["tri_sampled"] = tri_sampled;
	return d;
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

Color VoxelWorld::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !raymarch_pass_) return Color(1, 0, 1);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, cam, 1, 1, kNoEdit)) return Color(1, 0, 1);
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (data.size() < 8) return Color(1, 0, 1);
	const uint16_t *h = reinterpret_cast<const uint16_t *>(data.ptr());
	return Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
}

Dictionary VoxelWorld::debug_raymarch_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !raymarch_pass_) return d;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 rorig = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.region_origin[0] = rorig.x; cam.region_origin[1] = rorig.y; cam.region_origin[2] = rorig.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, cam, 1, 1, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	const PackedByteArray col = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (hp.size() < 16 || col.size() < 8) return d;
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	const uint16_t *h = reinterpret_cast<const uint16_t *>(col.ptr());
	d["color"] = Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
	if (hf[3] < 0.5f) return d; // sky miss
	d["hit"] = true;
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

Vector2 VoxelWorld::debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) const {
	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field: op buffer too small");
			return Vector2();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::Sample s = ve::eval_field(gen, ptr, op_count, p.x, p.y, p.z);
	return Vector2(s.sdf, static_cast<float>(s.material));
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
	return ve::brick_has_surface(gen, ptr, op_count, {brick.x, brick.y, brick.z});
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
	ve::eval_brick(gen, ptr, op_count, b, &ref);

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

int VoxelWorld::debug_stream_frame(Vector3 cam) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !streamer_) return 0;
	const int actions = streamer_->run_frame(device, cam.x, cam.y, cam.z);
	device->submit();
	device->sync();
	overflow_seen_ |= static_cast<int>(atlas_->read_overflow(device));
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
	const ve::RayHit h = ve::raycast(gen, *edit_log_, o, f, 200.0f);
	if (!h.hit) return d;
	d["hit"] = true;
	d["pos"] = Vector3(h.pos[0], h.pos[1], h.pos[2]);
	d["normal"] = Vector3(h.normal[0], h.normal[1], h.normal[2]);
	d["distance"] = h.distance;
	return d;
}
