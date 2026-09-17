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

Dictionary VoxelDebugHooks::debug_perf_stats() {
	Dictionary d;
	d["physics_tick_ms"] = world_->stats().last_physics_tick_ms;
	d["phys_collect_ms"] = world_->colliders() ? world_->colliders()->last_collect_ms() : 0.0f;
	d["phys_apply_ms"] = world_->colliders() ? world_->colliders()->last_apply_ms() : 0.0f;
	d["phys_faces_ms"] = world_->colliders() ? world_->colliders()->last_faces_ms() : 0.0f;
	d["phys_setdata_ms"] = world_->colliders() ? world_->colliders()->last_setdata_ms() : 0.0f;
	// `build_ms` is the maximum one octant build call in the measured physics frame, not
	// the sum of all octant calls. This is the value exported into BENCH max_ms.
	d["build_ms"] = world_->colliders() ? world_->colliders()->last_build_ms() : 0.0f;
	d["phys_body_ms"] = world_->colliders() ? world_->colliders()->last_body_ms() : 0.0f;
	d["phys_tris"] = world_->colliders() ? world_->colliders()->last_tris() : 0;
	d["phys_plan_ms"] = world_->colliders() ? world_->colliders()->last_plan_ms() : 0.0f;
	d["phys_submit_ms"] = world_->colliders() ? world_->colliders()->last_submit_ms() : 0.0f;
	d["stream_total_ms"] = world_->context().render->streamer() ? world_->context().render->streamer()->last_total_ms() : 0.0f;
	d["stream_readback_ms"] = world_->context().render->streamer() ? world_->context().render->streamer()->last_readback_ms() : 0.0f;
	d["island_ms"] = world_->island_manager() ? world_->island_manager()->last_ms() : 0.0f;
	// lod_ms is CPU command-record time for the LoD raster + cull passes, not GPU execution
	// time. See LodRasterPass/LodCullPass::last_ms comments.
	d["lod_ms"] = (world_->context().render->passes().lod_raster ? world_->context().render->passes().lod_raster->last_ms() : 0.0f) +
			(world_->context().render->passes().lod_cull ? world_->context().render->passes().lod_cull->last_ms() : 0.0f);
	return d;
}

void VoxelDebugHooks::debug_set_fail_consolidations(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_service()) world_->mesh_service()->debug_set_fail_consolidations(v);
}

void VoxelDebugHooks::debug_set_fail_consolidate_uploads(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_service()) world_->mesh_service()->debug_set_fail_consolidate_uploads(v);
}

void VoxelDebugHooks::debug_set_fail_restore_overrides(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_service()) world_->mesh_service()->debug_set_fail_restore_overrides(v);
}

void VoxelDebugHooks::debug_set_fail_restore_overrides_always(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_service()) world_->mesh_service()->debug_set_fail_restore_overrides_always(v);
}

void VoxelDebugHooks::debug_set_pause_override_publication(bool v) {
	world_->ensure_physics_initialized();
	if (world_->mesh_service()) world_->mesh_service()->debug_set_pause_override_publication(v);
}

bool VoxelDebugHooks::debug_override_publication_paused() const {
	return world_->mesh_service() && world_->mesh_service()->debug_override_publication_paused();
}

void VoxelDebugHooks::debug_apply_sphere_subtract(Vector3 centre, float radius) {
	if (!world_->context().store->edit_log()) world_->ensure_physics_initialized();
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
	if (!world_->context().store->edit_log()) world_->ensure_physics_initialized();
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
	if (!world_->context().store->edit_log()) world_->ensure_physics_initialized();
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
	if (!world_->context().store->edit_log()) world_->ensure_physics_initialized();
	float o[3] = {origin.x, origin.y, origin.z};
	ve::EditOp op = ve::make_volume_add(slot, o, voxel, dim);
	world_->append_edit(op);
}

int VoxelDebugHooks::debug_region_op_count(Vector3i region) {
	if (!world_->context().store->edit_log()) return 0;
	std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
	return world_->context().store->edit_log()->op_count({region.x, region.y, region.z});
}

int VoxelDebugHooks::debug_override_region_table(int region_slot) const {
	return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->overrides().region_table(region_slot) : -1;
}

int VoxelDebugHooks::debug_override_used() const {
	return world_->context().store->overrides() ? world_->context().store->overrides()->used() : 0;
}

bool VoxelDebugHooks::debug_fill_override_pool(Vector3i region_in) {
	world_->ensure_physics_initialized();
	std::unique_lock<std::mutex> edit_lock(world_->context().store->edit_mutex());
	if (!world_->context().render->passes().atlas || !world_->mesh_service() || !world_->context().store->overrides() || world_->context().store->overrides()->used() != 0) return false;
	// The fill publishes through the target region's tenant slot, so the region must be
	// resident -- the caller names the region it streamed. No world box exists to provide
	// a canonical one, and a window-derived one would defeat the offscreen-refusal test
	// (streaming far away recentres the window onto resident ground).
	const ve::IVec3 region{region_in.x, region_in.y, region_in.z};
	const int region_slot = world_->context().store->residency() ? world_->context().store->residency()->slot_of(region) : -1;
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
	slots.reserve(world_->context().store->overrides()->capacity());
	bricks.reserve(world_->context().store->overrides()->capacity());
	entries.reserve(world_->context().store->overrides()->capacity());
	for (int i = 0; i < world_->context().store->overrides()->capacity(); i++) {
		const ve::IVec3 brick{base.x + (i & 31), base.y + ((i >> 5) & 31),
				base.z + ((i >> 10) & 31)};
		const int slot = world_->context().store->overrides()->acquire(brick);
		if (slot < 0) {
			for (const ve::IVec3 acquired : acquired_bricks) world_->context().store->overrides()->release(acquired);
			return false;
		}
		const ve::OverrideBrick *data = world_->context().store->overrides()->data(slot);
		if (!data) {
			for (const ve::IVec3 acquired : acquired_bricks) world_->context().store->overrides()->release(acquired);
			world_->context().store->overrides()->release(brick);
			return false;
		}
		slots.push_back(slot);
		acquired_bricks.push_back(brick);
		bricks.push_back(*data);
		entries.emplace_back(i, slot);
	}
	auto discard = [&]() {
		if (world_->context().render->passes().atlas) {
			world_->context().render->passes().atlas->overrides().clear_table(world_->rd(), 0);
			world_->context().render->passes().atlas->set_override_table(world_->rd(), region_slot, -1, {});
		}
		for (const ve::IVec3 brick : acquired_bricks) world_->context().store->overrides()->release(brick);
	};
	if (world_->context().render->passes().atlas) {
		for (size_t i = 0; i < slots.size(); i++) {
			if (!world_->context().render->passes().atlas->upload_override(world_->rd(), slots[i], bricks[i])) {
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
				world_->context().render->passes().atlas->stored_normals().upload_override(world_->rd(), slots[i],
						brick.normal_oct.data(), ve::kBrickSdfCount);
			else
				world_->context().render->passes().atlas->stored_normals().release_override(world_->rd(), slots[i]);
		}
		world_->context().render->passes().atlas->set_override_table(world_->rd(), region_slot, 0, entries);
	}
	if (!world_->mesh_service()->publish_overrides(slots, bricks, region, region_slot, 0, entries)) {
		// The worker publication is synchronous here, but it can still fail after a
		// partial upload. Replay its empty old transaction before releasing the slots.
		world_->mesh_service()->restore_overrides({}, {}, region, region_slot, 0, -1, {});
		discard();
		return false;
	}
	world_->context().store->override_tables()[std::tuple<int, int, int>{region.x, region.y, region.z}] = 0;
	return true;
}

Dictionary VoxelDebugHooks::debug_override_render_state(Vector3i brick) {
	std::unique_lock<std::mutex> edit_lock(world_->context().store->edit_mutex());
	Dictionary d;
	d["cpu_slot"] = -1;
	d["table"] = -1;
	d["table_slot"] = -1;
	d["sdf_match"] = false;
	d["mat_match"] = false;
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().atlas || !world_->context().store->overrides()) return d;
	const ve::IVec3 b{brick.x, brick.y, brick.z};
	const ve::IVec3 r = ve::region_of_brick(b);
	const int region_slot = world_->context().store->residency() ? world_->context().store->residency()->slot_of(r) : -1;
	if (region_slot < 0) return d;
	const int table = world_->context().render->passes().atlas->overrides().region_table(region_slot);
	int table_slot = -1;
	if (table >= 0) {
		const int bi = ve::brick_index_in_region(b);
		const PackedByteArray entry = device->buffer_get_data(world_->context().render->passes().atlas->overrides().tables(),
				static_cast<uint32_t>((table * ve::kRegionBrickCount + bi) * 4), 4);
		if (entry.size() >= 4) table_slot = *reinterpret_cast<const int32_t *>(entry.ptr());
	}
	const int cpu_slot = world_->context().store->overrides()->slot_of(b);
	d["cpu_slot"] = cpu_slot;
	d["table"] = table;
	d["table_slot"] = table_slot;
	if (table < 0 || table_slot < 0 || table_slot != cpu_slot || cpu_slot < 0) return d;
	const int sdf_stride = ((ve::kBrickSdfCount + 3) / 4) * 4;
	const int mat_stride = ((ve::kBrickVoxelCount + 3) / 4) * 4;
	const PackedByteArray sdf = device->buffer_get_data(world_->context().render->passes().atlas->overrides().sdf_buffer(),
			static_cast<uint32_t>(cpu_slot * sdf_stride), sdf_stride);
	const PackedByteArray mat = device->buffer_get_data(world_->context().render->passes().atlas->overrides().mat_buffer(),
			static_cast<uint32_t>(cpu_slot * mat_stride), mat_stride);
	const ve::OverrideBrick *cpu = world_->context().store->overrides()->data(cpu_slot);
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
	std::unique_lock<std::mutex> edit_lock(world_->context().store->edit_mutex());
	if (!world_->mesh_service() || !world_->context().store->edit_log() || !world_->context().store->overrides() || !world_->context().store->residency()) return d;
	const ve::IVec3 r{region.x, region.y, region.z};
	ve::ConsolidationSnapshot snap;
	const bool sources_ok =
			world_->context().store->field().locked_by_caller().snapshot_region(r, &snap);
	const std::vector<ve::EditOp> &ops = snap.ops;
	const std::vector<ve::IVec3> &bricks = snap.bricks;
	d["bricks"] = static_cast<int>(bricks.size());
	d["sdf_mismatches"] = 0;
	d["mat_mismatches"] = 0;
	if (bricks.empty()) return d;
	ConsolidateJob job;
	job.region = r;
	job.region_slot = world_->context().store->residency()->slot_of(r);
	if (job.region_slot < 0) return d;
	job.bricks = bricks;
	job.ops = ops;
	if (!bricks.empty()) {
		if (!sources_ok) return d;
		job.source = snap.sources;
		job.gen = &world_->context().store->generator()->sampler();
	}
	const int existing_table = world_->context().store->override_table_for_region(r);
	if (existing_table >= 0) {
		std::vector<std::pair<int, int>> existing_entries;
		const ve::IVec3 base{r.x * ve::kRegionBricks, r.y * ve::kRegionBricks,
				r.z * ve::kRegionBricks};
		for (int z = 0; z < ve::kRegionBricks; z++)
			for (int y = 0; y < ve::kRegionBricks; y++)
				for (int x = 0; x < ve::kRegionBricks; x++) {
					const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
					const int slot = world_->context().store->overrides()->slot_of(b);
					if (slot >= 0) existing_entries.emplace_back(
							ve::brick_index_in_region(b), slot);
				}
		if (!world_->mesh_service()->set_override_region(r, job.region_slot, existing_table, existing_entries)) return d;
	}
	if (!world_->mesh_service()->submit_consolidations({job})) return d;
	world_->mesh_service()->run_sync([](MeshPass &) {});
	std::vector<ConsolidateResult> results;
	if (world_->mesh_service()->collect_consolidations(&results) != 1 || results[0].failed) return d;
	const ve::Generator &gen = world_->context().store->generator()->sampler();
	const ve::FieldView view = world_->context().store->field().locked_by_caller();
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
					const ve::Sample s = view.sample(bo[0] + x * ve::kVoxelSize,
							bo[1] + y * ve::kVoxelSize, bo[2] + z * ve::kVoxelSize);
					const uint8_t expected = ve::encode_sdf(s.sdf);
					const uint8_t actual = b.sdf[ve::sdf_index(x, y, z)];
					// CPU/GPU transcendental rounding can cross an R8 quantization boundary.
					// Match the one-step tolerance used by the brick differential probes.
					if (std::abs(int(expected) - int(actual)) > 1) {
						sdf_mismatches++;
						if (first.is_empty()) { first["brick"] = Vector3i(brick.x, brick.y, brick.z); first["lattice"] = Vector3i(x, y, z); first["expected"] = int(expected); first["actual"] = int(actual); }
					}
				}
			for (int z = 0; z < ve::kBrickVoxels; z++)
				for (int y = 0; y < ve::kBrickVoxels; y++)
					for (int x = 0; x < ve::kBrickVoxels; x++) {
						const ve::Sample s = view.sample(bo[0] + x * ve::kVoxelSize,
								bo[1] + y * ve::kVoxelSize, bo[2] + z * ve::kVoxelSize);
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
			ve::FieldSample fs = ve::eval_field_gradient(gen, ops.data(), static_cast<int>(ops.size()), px, py, pz, &world_->context().store->volumes(), world_->context().store->overrides());
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

PackedByteArray VoxelDebugHooks::debug_field_params_bytes() {
	// Same packing as FieldContextSet::initialize; both go through
	// ve::pack_field_params_bytes (see terrain/field_params_pack.h for why both halves
	// of the dense-values-in-16-stride-sized-buffer shape are load-bearing).
	const ve::ResolvedPipeline &p = world_->context().store->terrain_pipeline();
	const std::vector<uint8_t> packed = ve::pack_field_params_bytes(p);
	PackedByteArray bytes;
	bytes.resize(int(packed.size()));
	if (!packed.empty())
		std::memcpy(bytes.ptrw(), packed.data(), packed.size());
	return bytes;
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
	const ve::IVec3 region = ve::region_of_point(center.x, center.y, center.z);
	int brick_mismatches = 0;
	const int rslot = debug_region_map_entry(Vector3i(region.x, region.y, region.z));
	if (rslot >= 0) {
		std::vector<ve::EditOp> ops_vec;
		{
			std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
			if (world_->context().store->edit_log()) ops_vec = world_->context().store->edit_log()->ops(region);
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
					const ve::IVec3 b = ve::brick_of_point(
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
	world_->context().store->volumes().reserve(slot); // no-op when the suite already claimed it
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
	if (world_->context().store->volumes().store(slot, std::move(d)) && world_->mesh_service()) {
		world_->mesh_service()->submit_volume(slot, to_upload);
		world_->mesh_service()->run_sync([](MeshPass &){});
	}
}

Vector2 VoxelDebugHooks::debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) {
	const ve::Generator &gen = world_->context().store->generator()->sampler();
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field: op buffer too small");
			return Vector2();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::Sample s = ve::eval_field(gen, ptr, op_count, p.x, p.y, p.z, &world_->context().store->volumes(), world_->context().store->overrides());
	return Vector2(s.sdf, static_cast<float>(s.material));
}

Dictionary VoxelDebugHooks::debug_eval_field_gradient(Vector3 p, const PackedByteArray &ops, int op_count) {
	const ve::Generator &gen = world_->context().store->generator()->sampler();
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field_gradient: op buffer too small");
			return Dictionary();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::FieldSample s = ve::eval_field_gradient(gen, ptr, op_count, p.x, p.y, p.z, &world_->context().store->volumes(), world_->context().store->overrides());
	Dictionary d;
	d["sdf"] = s.sdf;
	d["material"] = int(s.material);
	d["gradient"] = Vector3(s.gradient[0], s.gradient[1], s.gradient[2]);
	d["exact"] = s.exact_gradient;
	return d;
}

bool VoxelDebugHooks::debug_init_atlas() {
	world_->ensure_initialized();
	return world_->context().render->passes().atlas && world_->context().render->passes().atlas->is_valid();
}

void VoxelDebugHooks::debug_teardown_atlas() {
	world_->teardown_gpu();
}

Dictionary VoxelDebugHooks::debug_atlas_stats() {
	Dictionary d;
	RenderingDevice *device = world_->rd();
	if (!world_->context().render->passes().atlas || !world_->context().render->passes().atlas->is_valid() || !device) return d;
	d["slot_count"] = world_->context().render->passes().atlas->atlas_slot_count();
	d["free_slots"] = world_->context().render->passes().atlas->read_free_count(device);
	d["region_map_entries"] = world_->context().render->passes().atlas->region_map_entries(); // window cell count, not world-box size
	d["job_count"] = world_->context().render->passes().atlas->read_job_count(device);
	d["overflow"] = static_cast<int>(world_->context().render->passes().atlas->read_overflow(device));
	// Memory bounds (Task 6): the R8 atlas byte count is pinned so a regression that
	// resizes it fails loudly next to the normal-pool capacity assertion.
	const ve::IVec3 ab = world_->context().render->passes().atlas->config().atlas_bricks;
	const int64_t sdf_bytes = static_cast<int64_t>(ab.x) * ve::kBrickSdfStride *
			(ab.y * ve::kBrickSdfStride) * (ab.z * ve::kBrickSdfStride);
	d["sdf_atlas_bytes"] = sdf_bytes;
	return d;
}

void VoxelDebugHooks::debug_reset_frame_counters() {
	if (world_->context().render->passes().atlas && world_->rd()) world_->context().render->passes().atlas->reset_frame_counters(world_->rd());
}

void VoxelDebugHooks::debug_set_region_map_entry(int region_index, int region_slot) {
	if (world_->context().render->passes().atlas && world_->rd()) world_->context().render->passes().atlas->set_region_map_entry(world_->rd(), region_index, region_slot);
}

void VoxelDebugHooks::debug_upload_region_ops(int region_slot, const PackedByteArray &ops, int count) {
	if (!world_->context().render->passes().atlas || !world_->rd()) return;
	const ve::EditOp *ptr = nullptr;
	if (count > 0) {
		if (ops.size() < count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_upload_region_ops: op buffer too small");
			return;
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	world_->context().render->passes().atlas->upload_region_ops(world_->rd(), region_slot, ptr, count);
}

bool VoxelDebugHooks::debug_brick_has_surface(Vector3i brick, const PackedByteArray &ops,
		int op_count) const {
	const ve::Generator &gen = world_->context().store->generator()->sampler();
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_brick_has_surface: op buffer too small");
			return false;
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	return ve::brick_has_surface(gen, ptr, op_count, {brick.x, brick.y, brick.z}, &world_->context().store->volumes());
}

void VoxelDebugHooks::debug_mark_region(Vector3i region, int region_slot, Vector3i lo, Vector3i hi,
		int op_count, bool force) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().atlas || !world_->context().render->passes().region) return;
	if (region_slot < 0 || region_slot >= world_->context().store->config().max_region_slots) {
		// The mark shader indexes region_tables with rslot * kRegionBrickCount + bi, so a
		// hostile slot is a GPU-side out-of-bounds write. Refuse before recording.
		UtilityFunctions::printerr("debug_mark_region: region_slot ", region_slot,
				" out of range [0, ", world_->context().store->config().max_region_slots, ")");
		return;
	}
	const int64_t list = device->compute_list_begin();
	world_->context().render->passes().region->mark(device, list, {region.x, region.y, region.z}, region_slot,
			{lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}, op_count, force,
			false, world_->context().render->passes().field_context);
	device->compute_list_end();
	device->submit();
	device->sync();
}

void VoxelDebugHooks::debug_generate_pending() {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().atlas || !world_->context().render->passes().region || !world_->context().render->passes().gen) return;
	const int64_t list = device->compute_list_begin();
	world_->context().render->passes().region->write_dispatch_args(device, list);
	device->compute_list_add_barrier(list);
	world_->context().render->passes().gen->dispatch(device, list, *world_->context().render->passes().atlas, world_->context().render->passes().field_context);
	device->compute_list_end();
	device->submit();
	device->sync();
}

Dictionary VoxelDebugHooks::debug_brick_diff(Vector3i brick, int region_slot,
		const PackedByteArray &ops, int op_count) {
	Dictionary d;
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().atlas) return d;
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

	const ve::Generator &gen = world_->context().store->generator()->sampler();
	ve::BrickEval ref{};
	ve::eval_brick(gen, ptr, op_count, b, &ref, &world_->context().store->volumes(), world_->context().store->overrides());

	const ve::IVec3 ab = world_->context().render->passes().atlas->config().atlas_bricks;
	const ve::IVec3 cell{slot % ab.x, (slot / ab.x) % ab.y, slot / (ab.x * ab.y)};

	// texture_get_data returns the whole volume; tests run a small atlas, so one read each.
	const PackedByteArray sdf = device->texture_get_data(world_->context().render->passes().atlas->sdf_atlas(), 0);
	const PackedByteArray mat = device->texture_get_data(world_->context().render->passes().atlas->mat_atlas(), 0);
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

	const PackedByteArray pal_bytes = device->buffer_get_data(world_->context().render->passes().atlas->palette(),
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
		const PackedByteArray mip = device->texture_get_data(world_->context().render->passes().atlas->mip_atlas(level), 0);
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
	if (!world_->is_initialized() || !world_->rd() || !world_->context().render->streamer()) return;
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
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().store->edit_log()) return d;
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;

	const PackedByteArray table = device->buffer_get_data(world_->context().render->passes().atlas->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	const PackedByteArray flags = device->buffer_get_data(world_->context().render->passes().atlas->brick_flags());
	if (table.size() < ve::kRegionBrickCount * 4 ||
			flags.size() < world_->context().render->passes().atlas->atlas_slot_count() * static_cast<int>(sizeof(uint32_t))) return d;

	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		ops = world_->context().store->edit_log()->ops({region.x, region.y, region.z});
	}
	const ve::Generator &gen = world_->context().store->generator()->sampler();
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
		ve::eval_brick(gen, ops.data(), static_cast<int>(ops.size()), brick, &ref, &world_->context().store->volumes(), world_->context().store->overrides());
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
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().store->edit_log() || !world_->context().render->passes().region) return d;
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;
	int op_count = 0;
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		op_count = static_cast<int>(world_->context().store->edit_log()->ops({region.x, region.y, region.z}).size());
	}
	const ve::IVec3 lo{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	const ve::IVec3 hi{lo.x + ve::kRegionBricks - 1, lo.y + ve::kRegionBricks - 1,
			lo.z + ve::kRegionBricks - 1};
	debug_mark_region(region, rslot, Vector3i(lo.x, lo.y, lo.z), Vector3i(hi.x, hi.y, hi.z),
			op_count, true);
	const PackedByteArray table = device->buffer_get_data(world_->context().render->passes().atlas->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	const PackedByteArray flags = device->buffer_get_data(world_->context().render->passes().atlas->brick_flags());
	if (table.size() < ve::kRegionBrickCount * 4 ||
			flags.size() < world_->context().render->passes().atlas->atlas_slot_count() * static_cast<int>(sizeof(uint32_t))) return d;
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
	if (!device || !world_->context().render->passes().region) return;
	if (region_slot < 0 || region_slot >= world_->context().store->config().max_region_slots) {
		// Same hostile-slot hazard as debug_mark_region: the free shader indexes
		// region_tables with rslot * kRegionBrickCount + bi.
		UtilityFunctions::printerr("debug_release_region: region_slot ", region_slot,
				" out of range [0, ", world_->context().store->config().max_region_slots, ")");
		return;
	}
	const int64_t list = device->compute_list_begin();
	world_->context().render->passes().region->release_region(device, list, region_slot);
	device->compute_list_end();
	device->submit();
	device->sync();
}

PackedInt32Array VoxelDebugHooks::debug_jobs() {
	PackedInt32Array out;
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().atlas) return out;
	const int count = world_->context().render->passes().atlas->read_job_count(device);
	if (count <= 0) return out;
	const PackedByteArray b = device->buffer_get_data(world_->context().render->passes().atlas->jobs(), 0, count * 32);
	out.resize(count * 8);
	memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(count) * 32);
	return out;
}

int VoxelDebugHooks::debug_region_table_slot(int region_slot, Vector3i brick) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().atlas) return -1;
	const int bi = ve::brick_index_in_region({brick.x, brick.y, brick.z});
	const uint32_t offset =
			(static_cast<uint32_t>(region_slot) * ve::kRegionBrickCount + bi) * 4;
	const PackedByteArray b = device->buffer_get_data(world_->context().render->passes().atlas->region_tables(), offset, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

RID VoxelDebugHooks::debug_sdf_atlas() const { return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->sdf_atlas() : RID(); }

RID VoxelDebugHooks::debug_mat_atlas() const { return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->mat_atlas() : RID(); }

RID VoxelDebugHooks::debug_mip_atlas(int level) const {
	if (!world_->context().render->passes().atlas || level < 0 || level >= ve::kMipLevels) return RID();
	return world_->context().render->passes().atlas->mip_atlas(level);
}

RID VoxelDebugHooks::debug_region_map() const { return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->region_map() : RID(); }

RID VoxelDebugHooks::debug_region_tables() const { return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->region_tables() : RID(); }

RID VoxelDebugHooks::debug_free_list() const { return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->free_list() : RID(); }

RID VoxelDebugHooks::debug_frame_counters() const { return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->frame_counters() : RID(); }

RID VoxelDebugHooks::debug_op_pool() const { return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->op_pool() : RID(); }

RID VoxelDebugHooks::debug_op_counts() const { return world_->context().render->passes().atlas ? world_->context().render->passes().atlas->op_counts() : RID(); }

int VoxelDebugHooks::debug_occupancy_state(Vector3i cell) {
	world_->context().store->drain_occupancy(); // tests step the streamer by hand and never run _process
	return static_cast<int>(world_->context().store->occupancy().state({cell.x, cell.y, cell.z}));
}

void VoxelDebugHooks::debug_pump_occupancy() {
	// Contract: harvest already-issued async GPU readbacks and fold their inbox blocks; this
	// helper does not advance the streamer or issue a mark. Tests must drive frames separately
	// when they need a fresh mark, so harvesting cannot hide which mark branch ran.
	world_->ensure_initialized();
	if (world_->context().render->streamer() && world_->rd()) world_->context().render->streamer()->harvest_occupancy(world_->rd());
	world_->context().store->drain_occupancy();
}

Dictionary VoxelDebugHooks::debug_occupancy_fallback_diff(Vector3i region) {
	Dictionary d;
	d["compared"] = 0;
	d["fallback"] = 0;
	d["mismatches"] = 0;
	d["first_mismatch_brick"] = Vector3i(-1, -1, -1);
	world_->ensure_initialized();
	if (!world_->rd() || !world_->context().render->passes().atlas || !world_->context().store->edit_log() || !world_->context().render->passes().region) return d;
	debug_stream_region(region);
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;

	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		ops = world_->context().store->edit_log()->ops({region.x, region.y, region.z});
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
	const PackedByteArray gpu = world_->rd()->buffer_get_data(world_->context().render->passes().atlas->region_occupancy(),
			static_cast<uint32_t>(rslot) * block_bytes, block_bytes);
	if (gpu.size() < static_cast<int>(block_bytes)) return d;

	const ve::Generator &gen = world_->context().store->generator()->sampler();
	int compared = 0, fallback = 0, mismatches = 0;
	Vector3i first(-1, -1, -1);
	for (int bi = 0; bi < ve::kRegionBrickCount; bi++) {
		const ve::IVec3 brick{
				region.x * ve::kRegionBricks + (bi & (ve::kRegionBricks - 1)),
				region.y * ve::kRegionBricks + ((bi >> 5) & (ve::kRegionBricks - 1)),
				region.z * ve::kRegionBricks + (bi >> 10)};
		if (ve::brick_has_surface(gen, ops.data(), static_cast<int>(ops.size()), brick,
				&world_->context().store->volumes(), world_->context().store->overrides())) continue;
		fallback++;
		const int got = ve::OccupancyGrid::read_packed(
				reinterpret_cast<const uint8_t *>(gpu.ptr()), bi);
		const int want = static_cast<int>(ve::cell_state_probe(gen, ops.data(),
				static_cast<int>(ops.size()), brick, &world_->context().store->volumes(), world_->context().store->overrides()));
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
	if (!world_->rd() || !world_->context().render->passes().atlas || !world_->context().store->edit_log()) return d;
	debug_stream_region(region);
	const int rslot = debug_region_map_entry(region);
	if (rslot < 0) return d;
	const uint32_t block_bytes = GpuAtlas::occupancy_block_bytes();
	const PackedByteArray gpu = world_->rd()->buffer_get_data(world_->context().render->passes().atlas->region_occupancy(),
			static_cast<uint32_t>(rslot) * block_bytes, block_bytes);
	const PackedByteArray table = world_->rd()->buffer_get_data(world_->context().render->passes().atlas->region_tables(),
			static_cast<uint32_t>(rslot) * ve::kRegionBrickCount * 4,
			static_cast<uint32_t>(ve::kRegionBrickCount) * 4);
	if (gpu.size() < static_cast<int>(block_bytes) ||
			table.size() < ve::kRegionBrickCount * 4) return d;
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		ops = world_->context().store->edit_log()->ops({region.x, region.y, region.z});
	}
	const int32_t *slots = reinterpret_cast<const int32_t *>(table.ptr());
	const ve::Generator &gen = world_->context().store->generator()->sampler();
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
				static_cast<int>(ops.size()), brick, &world_->context().store->volumes(), world_->context().store->overrides()));
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
	if (world_ == nullptr || world_->context().store == nullptr ||
			world_->context().store->generator() == nullptr) {
		return out;
	}
	const ve::Generator &gen = world_->context().store->generator()->sampler();
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
	const ve::FieldView view = world_->context().store->field().lock();
	return view.valid() ? view.sample(p.x, p.y, p.z).sdf : 1e30f;
}

int VoxelDebugHooks::debug_cell_state(Vector3i cell) {
	if (!world_->context().store->edit_log()) return static_cast<int>(ve::kCellUnknown);
	const ve::IVec3 c{cell.x, cell.y, cell.z};
	const ve::Generator &gen = world_->context().store->generator()->sampler();
	std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
	const std::vector<ve::EditOp> &ops = world_->context().store->edit_log()->ops(ve::region_of_brick(c));
	return static_cast<int>(ve::cell_state_field(gen, ops.data(),
			static_cast<int>(ops.size()), c, &world_->context().store->volumes(), world_->context().store->overrides()));
}

Dictionary VoxelDebugHooks::debug_occupancy_stats(Vector3 center) {
	world_->context().store->drain_occupancy();
	Dictionary d;
	d["regions"] = world_->context().store->occupancy().region_count();
	d["edit_seq"] = static_cast<int64_t>(world_->edit_seq());
	// The block covering the streaming centre, so a test can tell "the grid has been told
	// about this edit" from "some other region's block arrived".
	const ve::IVec3 r = ve::region_of_point(center.x, center.y, center.z);
	d["seq_at_center"] = static_cast<int64_t>(world_->context().store->occupancy().block_seq(r));
	return d;
}

int VoxelDebugHooks::debug_stream_frame(Vector3 cam) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->streamer()) return 0;
	// The retention sweep in drain_occupancy() (called below and from _process) evicts
	// around this centre. Hook-driven tests move the camera without any physics anchor,
	// so the streamer camera is the only correct centre here; without it, observing
	// beyond 256 m of the origin would evict the very blocks under test.
	world_->context().store->set_center(cam.x, cam.y, cam.z);
	const int actions = world_->context().render->streamer()->run_frame(device, cam.x, cam.y, cam.z);
	device->submit();
	device->sync();
	world_->note_overflow(static_cast<int>(world_->context().render->passes().atlas->read_overflow(device)));
	world_->context().store->drain_occupancy();
	return actions;
}

Dictionary VoxelDebugHooks::debug_stream_stats() {
	Dictionary d;
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().store->residency() || !world_->context().render->streamer()) return d;
	d["resident_regions"] = world_->context().store->residency()->resident_count();
	d["frame_edits"] = world_->context().render->streamer()->last_frame_edits();
	d["overflow"] = static_cast<int>(world_->context().render->passes().atlas->read_overflow(device));
	// Either path may be the one running: debug_stream_frame drives the world in tests, the
	// compositor's render callback drives it in the demo, and only the streamer sees the
	// latter's frames. The HUD reads this, so it has to cover both.
	d["overflow_ever"] =
			world_->stats().overflow_seen | static_cast<int>(world_->context().render->streamer()->overflow_seen());
	{
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		d["override_bricks"] = world_->context().store->overrides() ? world_->context().store->overrides()->used() : 0;
		d["override_capacity"] = world_->context().store->overrides() ? world_->context().store->overrides()->capacity() : world_->context().store->config().max_override_bricks;
		d["consolidations"] = world_->context().consolidation->consolidated_count();
		d["consolidation_refusals"] = world_->context().consolidation->refusals();
		d["consolidation_queue_refusals"] = world_->context().consolidation->queue_refusals();
		d["edit_rejections"] = world_->stats().edit_rejections;
	}
	return d;
}

int VoxelDebugHooks::debug_slot_of_region(Vector3i region) const {
	if (!world_->context().store->residency()) return -1;
	return world_->context().store->residency()->slot_of({region.x, region.y, region.z});
}

int VoxelDebugHooks::debug_region_map_entry(Vector3i region) {
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas) return -1;
	// Toroidal and total: every region has a cell, so there is no out-of-world -1 anymore.
	// An unstreamed region reads the -1 the evict path (or the init fill) wrote to its cell.
	const int idx = world_->context().store->region_window().index({region.x, region.y, region.z});
	const PackedByteArray b = device->buffer_get_data(world_->context().render->passes().atlas->region_map(), idx * 4, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

bool VoxelDebugHooks::debug_region_map_consistent() {
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().store->residency()) return false;
	const ve::RegionWindow win = world_->context().store->region_window();
	const PackedByteArray b = device->buffer_get_data(world_->context().render->passes().atlas->region_map());
	const int32_t *map = reinterpret_cast<const int32_t *>(b.ptr());
	// The index is toroidal and origin-independent, so the whole window is checked cell
	// for cell: a live entry sits exactly where residency says it should, and anything
	// else must hold the -1 the evict path (or init fill) wrote.
	for (int z = 0; z < win.dim; z++)
		for (int y = 0; y < win.dim; y++)
			for (int x = 0; x < win.dim; x++) {
				const ve::IVec3 r{win.origin.x + x, win.origin.y + y, win.origin.z + z};
				const int gpu_slot = map[win.index(r)];
				const int cpu_slot = world_->context().store->residency()->slot_of(r);
				if (gpu_slot != cpu_slot) return false;
				if (gpu_slot >= 0 && !(world_->context().store->residency()->region_of_slot(gpu_slot) == r)) return false;
			}
	return true;
}

Dictionary VoxelDebugHooks::debug_raycast(Vector3 origin, Vector3 dir) {
	// Kept for the test suites; gameplay calls VoxelWorld.raycast.
	return world_->raycast(origin, dir, 200.0f);
}
} // namespace godot
