#include "render/region_pass.h"
#include "gpu_layout/blocks.h"
#include "render/field_context_set.h"
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

RegionPass::~RegionPass() {
	teardown();
}

bool RegionPass::initialize(RenderingDevice *rd, const GpuAtlas &atlas) {
	teardown();
	rd_ = rd;
	max_brick_jobs_ = atlas.config().max_brick_jobs;
	mark_ = gpu::compile_compute(rd, group_, "RegionPass", "brick_mark.comp.glsl");
	free_ = gpu::compile_compute(rd, group_, "RegionPass", "region_free.comp.glsl");
	args_ = gpu::compile_compute(rd, group_, "RegionPass", "dispatch_args.comp.glsl");
	if (!mark_.valid() || !free_.valid() || !args_.valid()) {
		teardown();
		return false;
	}
	// The atlas buffers never change identity, so the uniform sets are built once.
	mark_set_ = gpu::uniform_set(rd, group_, mark_.shader, 0, {
			gpu::storage(0, atlas.region_tables()),
			gpu::storage(1, atlas.free_list()),
			gpu::storage(2, atlas.counters()),
			gpu::storage(3, atlas.frame_counters()),
			gpu::storage(4, atlas.op_pool()),
			gpu::storage(5, atlas.jobs()),
			gpu::storage(6, atlas.region_slot_counts()),
			gpu::storage(7, atlas.volumes().sdf_buffer()),
			gpu::storage(8, atlas.volumes().mat_buffer()),
			gpu::storage(9, atlas.region_occupancy()),
			gpu::storage(10, atlas.brick_flags()),
			gpu::storage(11, atlas.overrides().sdf_buffer()),
			gpu::storage(12, atlas.overrides().mat_buffer()),
			gpu::storage(13, atlas.overrides().tables()),
			gpu::storage(14, atlas.overrides().region_table_map())});
	free_set_ = gpu::uniform_set(rd, group_, free_.shader, 0, {
			gpu::storage(0, atlas.region_tables()),
			gpu::storage(1, atlas.free_list()),
			gpu::storage(2, atlas.counters()),
			gpu::storage(3, atlas.region_slot_counts()),
			gpu::storage(4, atlas.region_occupancy())});
	args_set_ = gpu::uniform_set(rd, group_, args_.shader, 0, {
			gpu::storage(0, atlas.frame_counters()),
			gpu::storage(1, atlas.dispatch_args())});
	if (!mark_set_.is_valid() || !free_set_.is_valid() || !args_set_.is_valid()) {
		UtilityFunctions::printerr("RegionPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void RegionPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	mark_ = free_ = args_ = gpu::Program();
	mark_set_ = free_set_ = args_set_ = RID();
	rd_ = nullptr;
}

void RegionPass::mark(RenderingDevice *rd, int64_t list, ve::IVec3 region, int region_slot,
		ve::IVec3 lo, ve::IVec3 hi, int op_count, bool force_regen,
		bool generate_probe_misses, const FieldContextSet *field_context) {
	if (!mark_.pipeline.is_valid()) return;
	const int64_t total = static_cast<int64_t>(hi.x - lo.x + 1) * (hi.y - lo.y + 1) *
			(hi.z - lo.z + 1);
	if (total <= 0) return;
	const uint32_t groups = static_cast<uint32_t>((total + 255) / 256);

	ve::BrickMarkPush push{};
	push.region[0] = region.x; push.region[1] = region.y; push.region[2] = region.z;
	push.region[3] = region_slot;
	push.lo[0] = lo.x; push.lo[1] = lo.y; push.lo[2] = lo.z;
	push.hi[0] = hi.x; push.hi[1] = hi.y; push.hi[2] = hi.z;
	push.cfg[0] = op_count;
	push.cfg[2] = max_brick_jobs_;
	// 0 = plain stream-in, 1 = force resident regeneration, 2 = edit: generate every
	// touched brick so the exact lattice, rather than the activation probe, owns occupancy.
	push.cfg[3] = generate_probe_misses ? 2 : (force_regen ? 1 : 0);

	rd->compute_list_bind_compute_pipeline(list, mark_.pipeline);
	rd->compute_list_bind_uniform_set(list, mark_set_, 0);
	if (field_context != nullptr) field_context->bind(rd, list);
	// Phase 0 (release) is only meaningful when bricks may have gone inactive, which only
	// an edit can cause. A plain stream-in scans a region whose table is entirely absent.
	if (force_regen) {
		push.cfg[1] = 0;
		rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
		rd->compute_list_dispatch(list, groups, 1, 1);
		rd->compute_list_add_barrier(list);
	}
	push.cfg[1] = 1;
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, groups, 1, 1);
}

void RegionPass::release_region(RenderingDevice *rd, int64_t list, int region_slot) {
	if (!free_.pipeline.is_valid()) return;
	const ve::RegionFreePush push{{region_slot, 0, 0, 0}};
	rd->compute_list_bind_compute_pipeline(list, free_.pipeline);
	rd->compute_list_bind_uniform_set(list, free_set_, 0);
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, (ve::kRegionBrickCount + 255) / 256, 1, 1);
}

void RegionPass::write_dispatch_args(RenderingDevice *rd, int64_t list) {
	if (!args_.pipeline.is_valid()) return;
	// Godot's compute_list_add_barrier() restarts the list and REPLAYS the last set push
	// constant against the last bound pipeline. The streamer barriers right after this
	// call, so this pipeline declares a 16-byte push constant (dispatch_args.comp.glsl)
	// and we set it here — otherwise the barrier's replay would push whatever the
	// previous dispatch set (the mark pass's 64 bytes) into a mismatched pipeline and
	// Godot errors. The shader ignores the value.
	const ve::DispatchArgsPush push{};
	rd->compute_list_bind_compute_pipeline(list, args_.pipeline);
	rd->compute_list_bind_uniform_set(list, args_set_, 0);
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, 1, 1, 1);
}
