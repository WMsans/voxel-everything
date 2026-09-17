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
	world_->physics_bubble_centers().swap(flat);
}

Dictionary VoxelDebugHooks::debug_physics_stats() {
	Dictionary d;
	d["chunks_resident"] = world_->chunk_residency() ? world_->chunk_residency()->resident_count() : 0;
	d["chunks_pending"] = world_->chunk_residency() ? world_->chunk_residency()->pending_count() : 0;
	d["probe_cache"] = world_->chunk_residency() ? world_->chunk_residency()->probe_cache_size() : 0;
	// `bodies` preserves the historical chunk count used by the physics tests and HUD.
	// `bodies_raw` exposes the eight-way implementation detail for profiling only.
	d["bodies"] = world_->colliders() ? world_->colliders()->active_bodies() : 0;
	d["bodies_raw"] = world_->colliders() ? world_->colliders()->bodies_in_space() : 0;
	d["max_build_tris"] = world_->colliders() ? world_->colliders()->max_build_tris() : 0;
	d["max_chunk_tris"] = world_->colliders() ? world_->colliders()->max_chunk_tris() : 0;
	d["builds"] = world_->colliders() ? world_->colliders()->builds_last_frame() : 0;
	d["queued"] = world_->colliders() ? world_->colliders()->queued_results() : 0;
	d["failures"] = world_->colliders() ? world_->colliders()->failures() : 0;
	d["build_ms"] = world_->colliders() ? world_->colliders()->last_build_ms() : 0.0f;
	d["collect_ms"] = world_->colliders() ? world_->colliders()->last_collect_ms() : 0.0f;
	return d;
}

int VoxelDebugHooks::debug_island_pending_uploads() {
	return world_->context().render->handoff().pending_uploads();
}

int VoxelDebugHooks::debug_field_volume_upload_count() const {
	return world_->context().render->handoff().field_volume_uploads();
}

int VoxelDebugHooks::debug_island_descriptors_pending() {
	return world_->context().render->handoff().descs_dirty() ? 1 : 0;
}

PackedInt32Array VoxelDebugHooks::debug_mesh_volume_slots() {
	PackedInt32Array out;
	if (!world_->mesh_service()) return out;
	for (int slot : world_->mesh_service()->debug_submitted_volume_slots()) out.append(slot);
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
	world_->context().render->handoff().queue_island(slot, slot, d); // test fixture: atlas slot == volume slot
}

void VoxelDebugHooks::debug_queue_test_island_descriptors() {
	world_->context().render->handoff().publish_descriptors(std::vector<IslandSlotDesc>(1));
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
	if (!world_->context().store->volumes().reserve(slot)) {
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
	if (!world_->context().store->volumes().store(slot, d) || !world_->context().store->volumes().pin(slot)) {
		release_volume_slot(world_->context().store->volumes(), world_->context().render->handoff(), slot);
		UtilityFunctions::printerr(
				"debug_queue_committed_field_volume_upload: store/pin failed for slot ", slot);
		return;
	}
	// Only model the main-thread GPU handoff queue. The worker-side mirror is exercised by
	// ensure_physics_initialized()'s pinned-volume replay after teardown/reinit.
	world_->context().render->handoff().queue_field_volume(slot, d);
	if (world_->mesh_service()) {
		world_->mesh_service()->submit_volume(slot, d);
		world_->mesh_service()->run_sync([](MeshPass &){});
	}
}

void VoxelDebugHooks::debug_set_extraction_available(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_service()) world_->mesh_service()->debug_set_extraction_available(v);
}

void VoxelDebugHooks::debug_set_fail_extractions(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_service()) world_->mesh_service()->debug_set_fail_extractions(v);
}

void VoxelDebugHooks::debug_set_fail_extract_submit(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_service()) world_->mesh_service()->debug_set_fail_extract_submit(v);
}

int VoxelDebugHooks::debug_island_frame(float dt, Vector3 center) {
	world_->ensure_initialized();
	world_->ensure_physics_initialized();
	if (!world_->island_manager()) return 0;
	world_->context().store->drain_occupancy();
	const int n = world_->island_manager()->run_frame(dt, center);
	// The tests drive the world by hand and never enter the compositor, so the render-thread
	// half of the handoff has to happen here too.
	RenderingDevice *device = world_->rd();
	if (device) {
		world_->context().render->drain_island_uploads(device);
		device->submit();
		device->sync();
	}
	return n;
}

Dictionary VoxelDebugHooks::debug_island_stats() {
	return world_->island_manager() ? world_->island_manager()->stats() : Dictionary();
}

void VoxelDebugHooks::debug_set_merge_sleep_seconds(float v) {
	world_->ensure_physics_initialized();
	if (world_->island_manager()) world_->island_manager()->set_merge_sleep_seconds(v);
}

#ifdef DEBUG_ENABLED
void VoxelDebugHooks::debug_set_max_dynamic_bodies(int v) {
	world_->ensure_physics_initialized();
	// Clamp before forwarding: a test hook should be able to lower the guardrail but not
	// silently disable it with an absurd value.
	v = v < 1 ? 1 : (v > kMaxDynamicBodies ? kMaxDynamicBodies : v);
	if (world_->island_manager()) world_->island_manager()->debug_set_max_dynamic_bodies(v);
}

void VoxelDebugHooks::debug_set_atlas_slot_used(int slot, bool used) {
	world_->ensure_physics_initialized();
	if (world_->island_manager()) world_->island_manager()->debug_set_atlas_slot_used(slot, used);
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
	if (world_->island_manager()) world_->island_manager()->debug_set_fail_next_spawn(fail);
}

void VoxelDebugHooks::debug_set_fail_next_resample(bool fail) {
	world_->ensure_physics_initialized();
	if (world_->island_manager()) world_->island_manager()->debug_set_fail_next_resample(fail);
}

void VoxelDebugHooks::debug_set_empty_next_extraction(bool v) {
	world_->ensure_physics_initialized();
	if (world_->island_manager()) world_->island_manager()->debug_set_empty_next_extraction(v);
}

void VoxelDebugHooks::debug_wake_island_body(int index) {
	world_->ensure_physics_initialized();
	if (world_->island_manager()) world_->island_manager()->debug_wake_body(index);
}

void VoxelDebugHooks::debug_offset_island_body(int index, Vector3 offset) {
	world_->ensure_physics_initialized();
	if (world_->island_manager()) world_->island_manager()->debug_offset_body(index, offset);
}

Dictionary VoxelDebugHooks::debug_island_body_info(int index) {
	world_->ensure_physics_initialized();
#ifdef DEBUG_ENABLED
	if (world_->island_manager()) return world_->island_manager()->debug_body_info(index);
#else
	(void)index;
#endif
	return Dictionary();
}

RID VoxelDebugHooks::debug_body_of_chunk(Vector3i chunk) {
	if (!world_->chunk_residency() || !world_->colliders()) return RID();
	return world_->colliders()->body_of_slot(world_->chunk_residency()->slot_of({chunk.x, chunk.y, chunk.z}));
}

Dictionary VoxelDebugHooks::debug_chunk_collider_info(Vector3i chunk) {
	Dictionary d;
	if (!world_->chunk_residency() || !world_->colliders()) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	d["slot"] = world_->chunk_residency()->slot_of(c);
	d["state"] = world_->colliders()->chunk_state(c);
	d["in_flight"] = world_->colliders()->chunk_in_flight(c);
	d["build_count"] = world_->colliders()->build_count_of_chunk(c);
	d["last_ops"] = world_->colliders()->last_submit_op_count(c);
	return d;
}

Dictionary VoxelDebugHooks::debug_chunk_collider_octants(Vector3i chunk) {
	if (!world_->chunk_residency() || !world_->colliders()) return Dictionary();
	return world_->colliders()->debug_chunk_octants({chunk.x, chunk.y, chunk.z});
}

bool VoxelDebugHooks::debug_init_physics() {
	world_->ensure_physics_initialized();
	return world_->physics_ready();
}

void VoxelDebugHooks::debug_teardown_physics() {
	world_->teardown_physics();
}

Dictionary VoxelDebugHooks::debug_mesh_lattice_diff(Vector3i chunk) {
	Dictionary d;
	world_->ensure_physics_initialized();
	if (!world_->physics_ready() || !world_->mesh_service()) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	std::vector<ve::EditOp> ops;
	ve::FieldSnapshot snap;
	{
		const ve::FieldView view = world_->context().store->field().lock();
		if (!view.valid()) return d;
		ops = world_->context().store->edit_log()->ops(ve::region_of_chunk(c));
		// The oracle's sources: everything the chunk lattice (one cell below the origin to
		// the far face) can read, copied under the lock instead of read live afterwards.
		float o[3];
		ve::chunk_world_origin(c, o);
		const float lattice_origin[3] = {o[0] - ve::kChunkCellSize, o[1] - ve::kChunkCellSize,
				o[2] - ve::kChunkCellSize};
		const float span = static_cast<float>(ve::kChunkLattice - 1) * ve::kChunkCellSize;
		const float lattice_hi[3] = {lattice_origin[0] + span, lattice_origin[1] + span,
				lattice_origin[2] + span};
		if (!view.snapshot_lattice(lattice_origin, lattice_hi, lattice_origin, ve::kChunkCellSize,
				ve::kChunkLattice, &snap))
			return d;
	}
	const ve::SnapshotSources sources(snap.sources);
	if (!sources.ok) return d;
	MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
	job.override_table = snap.override_table;
	ve::chunk_world_origin(c, job.origin);
	job.cell_size = ve::kChunkCellSize;
	job.lattice = ve::kChunkLattice;
	std::vector<uint8_t> gpu;
	bool ok = false;
	world_->mesh_service()->run_sync([&](MeshPass &pass) { ok = pass.run_field_sync(job, &gpu); });
	if (!ok) return d;

	const ve::Generator &gen = world_->context().store->generator()->sampler();
	const ve::DcGrid g = ve::chunk_dc_grid(c);
	int max_diff = 0, over_one = 0;
	bool pos = false, neg = false;
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++) {
				const float p[3] = {g.origin[0] + (x - 1) * g.cell_size,
						g.origin[1] + (y - 1) * g.cell_size,
						g.origin[2] + (z - 1) * g.cell_size};
				const float s = ve::eval_field(gen, snap.ops.data(), static_cast<int>(snap.ops.size()),
						p[0], p[1], p[2], &sources.volumes, &sources.overrides).sdf;
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
	if (!world_->physics_ready() || !world_->mesh_service()) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	std::vector<ve::EditOp> ops;
	ve::FieldSnapshot snap;
	{
		const ve::FieldView view = world_->context().store->field().lock();
		if (!view.valid()) return d;
		ops = world_->context().store->edit_log()->ops(ve::region_of_chunk(c));
		// The oracle's sources: everything the chunk lattice (one cell below the origin to
		// the far face) can read, copied under the lock instead of read live afterwards.
		float o[3];
		ve::chunk_world_origin(c, o);
		const float lattice_origin[3] = {o[0] - ve::kChunkCellSize, o[1] - ve::kChunkCellSize,
				o[2] - ve::kChunkCellSize};
		const float span = static_cast<float>(ve::kChunkLattice - 1) * ve::kChunkCellSize;
		const float lattice_hi[3] = {lattice_origin[0] + span, lattice_origin[1] + span,
				lattice_origin[2] + span};
		if (!view.snapshot_lattice(lattice_origin, lattice_hi, lattice_origin, ve::kChunkCellSize,
				ve::kChunkLattice, &snap))
			return d;
	}
	const ve::SnapshotSources sources(snap.sources);
	if (!sources.ok) return d;
	MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
	job.override_table = snap.override_table;
	ve::chunk_world_origin(c, job.origin);
	job.cell_size = ve::kChunkCellSize;
	job.lattice = ve::kChunkLattice;
	MeshResult gpu;
	std::vector<uint8_t> lattice;
	std::vector<int32_t> gpu_cells;
	bool ok = false;
	world_->mesh_service()->run_sync([&](MeshPass &pass) {
		ok = pass.mesh_sync(job, &gpu, &lattice, &gpu_cells);
	});
	if (!ok) return d;
	if (gpu.failed) return d; // short readback: do not present partial data as a diff

	const ve::DcGrid g = ve::chunk_dc_grid(c);
	const ve::Generator &gen = world_->context().store->generator()->sampler();

	// 1. The lattice against the CPU field. One encoded step of sin() drift is invisible.
	int lat_max = 0, lat_over = 0;
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++) {
				const float s = ve::eval_field(gen, snap.ops.data(), static_cast<int>(snap.ops.size()),
						g.origin[0] + (x - 1) * g.cell_size, g.origin[1] + (y - 1) * g.cell_size,
						g.origin[2] + (z - 1) * g.cell_size, &sources.volumes, &sources.overrides).sdf;
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
		const float s = std::fabs(ve::eval_field(gen, snap.ops.data(), static_cast<int>(snap.ops.size()),
				gpu.positions[v * 3], gpu.positions[v * 3 + 1], gpu.positions[v * 3 + 2],
				&sources.volumes, &sources.overrides).sdf);
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
		const float out_side = ve::eval_field(gen, snap.ops.data(), static_cast<int>(snap.ops.size()),
				mid.x + step.x, mid.y + step.y, mid.z + step.z, &sources.volumes, &sources.overrides).sdf;
		const float in_side = ve::eval_field(gen, snap.ops.data(), static_cast<int>(snap.ops.size()),
				mid.x - step.x, mid.y - step.y, mid.z - step.z, &sources.volumes, &sources.overrides).sdf;
		tri_sampled++;
		if (out_side <= in_side) winding_bad++;
	}
	d["max_surface_sdf"] = max_sdf;
	d["verts_off_10cm"] = off_10cm;
	d["winding_bad"] = winding_bad;
	d["tri_sampled"] = tri_sampled;
	return d;
}

// The shipped marginal-contact query: refine_anchoring asks WorldStore::field() with the
// manager's face sample count, and so does this.
int VoxelDebugHooks::debug_contact_samples(Vector3i cell, int axis) {
	world_->ensure_physics_initialized();
	if (!world_->island_manager()) return -1;
	return world_->context().store->field().contact_samples({cell.x, cell.y, cell.z}, axis,
			world_->island_manager()->refine_config().face_samples);
}

Dictionary VoxelDebugHooks::debug_island_extract_diff(Vector3i lo_cell, Vector3i hi_cell) {
	Dictionary d;
	d["ok"] = false;
	world_->ensure_physics_initialized();
	if (!world_->mesh_service() || !world_->mesh_service()->is_valid()) return d;

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
	ve::FieldSnapshot snap;
	{
		const ve::FieldView view = world_->context().store->field().lock();
		if (!view.valid() || !view.snapshot_lattice(wlo, whi, job.origin, job.voxel, job.dim, &snap))
			return d;
	}
	job.ops = std::move(snap.ops);
	job.snapshot = std::move(snap.sources);
	job.override_table = snap.override_table;
	job.gen = &world_->context().store->generator()->sampler();

	// Drive the worker synchronously: this is a diagnostic, not the streaming path.
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(job);
	if (!world_->mesh_service()->submit_extracts(std::move(jobs))) return d;
	std::vector<IslandExtractResult> results;
	for (int i = 0; i < 2000 && results.empty(); i++) {
		world_->mesh_service()->collect_extracts(&results);
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (results.empty() || results[0].failed) return d;

	const ve::SnapshotSources sources(job.snapshot);
	if (!sources.ok) return d;

	std::vector<float> aabbs(boxes.size() * 6);
	for (size_t i = 0; i < boxes.size(); i++)
		boxes[i].world_aabb(&aabbs[i * 6], &aabbs[i * 6 + 3]);
	ve::VolumeData cpu;
	const ve::Generator &gen = world_->context().store->generator()->sampler();
	ve::extract_island_volume(gen, job.ops.data(), static_cast<int>(job.ops.size()),
			&sources.volumes, &sources.overrides, job.origin,
			job.voxel, job.dim, aabbs.data(), static_cast<int>(boxes.size()), &cpu);

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
		const ve::Generator &agen = world_->context().store->generator()->sampler();
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
			ve::FieldSample fs = ve::eval_field_gradient(agen, job.ops.data(), static_cast<int>(job.ops.size()), px, py, pz, &sources.volumes, &sources.overrides);
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
	if (!device || !world_->context().render->passes().islands || !world_->mesh_service() || !world_->mesh_service()->is_valid()) return d;
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
			device->buffer_get_data(world_->context().render->passes().islands->desc_buffer(), 0, static_cast<uint32_t>(desc_bytes));
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
	if (!world_->context().render->passes().atlas->volumes().upload(device, vslot, volume)) return d;
	// Task 7: keep the CPU-authoritative copy too (the same thing IslandManager does for
	// real bodies), so debug_island_normal_probe reads the same normals the GPU holds.
	world_->context().store->volumes().reserve(vslot);
	if (!world_->context().store->volumes().store(vslot, volume)) return d;
	// Task 6: compact normals share the pool; the test fixture's radial lattice is real
	// render-reachable payload, not a fallback source.
	world_->context().render->passes().atlas->stored_normals().upload_volume(device, vslot, volume);
	if (!world_->context().render->passes().islands->upload_mip(device, slot, volume)) return d;

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
	world_->context().render->passes().islands->upload_descriptors(device, all, kMaxIslands);
	world_->context().render->handoff().note_debug_slot(slot);
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

	const int slot = world_->context().store->volumes().allocate();
	if (slot < 0) return d;
	if (!world_->context().store->volumes().store(slot, volume)) {
		release_volume_slot(world_->context().store->volumes(), world_->context().render->handoff(), slot);
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
		release_volume_slot(world_->context().store->volumes(), world_->context().render->handoff(), slot);
		return d;
	}
	world_->test_bodies().push_back(b);
	d["ok"] = true;
	d["index"] = static_cast<int>(world_->test_bodies().size()) - 1;
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
	if (index < 0 || index >= static_cast<int>(world_->test_bodies().size()) || !world_->test_bodies()[index])
		return d;
	IslandBody *b = world_->test_bodies()[index];
	d["live"] = b->live();
	d["origin"] = b->transform().origin;
	d["asleep_s"] = b->asleep_seconds();
	d["mass"] = b->mass();
	d["cel_material"] = b->has_cel_material();
	return d;
}

void VoxelDebugHooks::debug_tick_test_bodies(float dt) {
	for (IslandBody *b : world_->test_bodies())
		if (b) {
			b->tick(dt);
			b->sync_render();
		}
}

void VoxelDebugHooks::debug_despawn_test_body(int index) {
	if (index < 0 || index >= static_cast<int>(world_->test_bodies().size()) || !world_->test_bodies()[index])
		return;
	world_->test_bodies()[index]->despawn();
}

void VoxelDebugHooks::debug_clear_test_island(int slot) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().islands) return;
	world_->context().render->passes().islands->clear_slot(device, slot);
	device->submit();
	device->sync();
}

bool VoxelDebugHooks::debug_mesh_submit(Array chunks) {
	world_->ensure_physics_initialized();
	if (!world_->physics_ready() || !world_->mesh_service()) return false;
	std::vector<ve::IVec3> coords;
	for (int i = 0; i < chunks.size(); i++) {
		const Vector3i v = chunks[i];
		coords.push_back({v.x, v.y, v.z});
	}
	std::vector<MeshRequest> requests;
	requests.reserve(coords.size());
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		for (const ve::IVec3 &c : coords)
			requests.push_back({c, world_->context().store->edit_log()->ops(ve::region_of_chunk(c))});
	}
	return world_->mesh_service()->submit(std::move(requests));
}

Array VoxelDebugHooks::debug_mesh_collect() {
	Array out;
	if (!world_->physics_ready() || !world_->mesh_service()) return out;
	// The mesher runs asynchronously now, so a test that submits and immediately collects
	// would race it. Wait for the batch to land — this is a diagnostic hook, and its old
	// contract was "collect returns the batch you submitted".
	std::vector<MeshResult> results;
	while (world_->mesh_service()->busy() && world_->mesh_service()->is_valid()) {
		if (world_->mesh_service()->collect(&results) > 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	world_->mesh_service()->collect(&results);
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

bool VoxelDebugHooks::debug_extract_submit(int id, Vector3i lo_cell, Vector3i hi_cell) {
	world_->ensure_physics_initialized();
	if (!world_->physics_ready() || !world_->mesh_service() || !world_->context().store->edit_log()) return false;
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
	ve::FieldSnapshot snap;
	{
		const ve::FieldView view = world_->context().store->field().lock();
		if (!view.valid() || !view.snapshot_lattice(wlo, whi, job.origin, job.voxel, job.dim, &snap))
			return false;
	}
	job.ops = std::move(snap.ops);
	job.snapshot = std::move(snap.sources);
	job.override_table = snap.override_table;
	job.gen = &world_->context().store->generator()->sampler();
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(std::move(job));
	return world_->mesh_service()->submit_extracts(std::move(jobs));
}

Array VoxelDebugHooks::debug_extract_collect() {
	Array out;
	if (!world_->physics_ready() || !world_->mesh_service()) return out;
	std::vector<IslandExtractResult> results;
	world_->mesh_service()->collect_extracts(&results);
	for (const IslandExtractResult &r : results) {
		Dictionary d;
		d["id"] = r.id;
		d["kind"] = static_cast<int>(r.kind);
		d["failed"] = r.failed;
		out.push_back(d);
	}
	return out;
}
} // namespace godot
