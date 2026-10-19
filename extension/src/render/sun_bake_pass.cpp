#include "render/sun_bake_pass.h"
#include "gpu_layout/blocks.h"
#include <algorithm>
#include <cstring>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

// Vulkan guarantees at least 65535 workgroups per dimension. The frame's sweep is far below
// that, but the debug probe sweeps all 65 536 slots at once, so chunk it here.
constexpr int kMaxGroupsPerDispatch = 65535;

ve::SunBakePush make_push(const ve::CameraParams &cam, int mode, int first, int count) {
	ve::SunBakePush push{};
	std::memcpy(push.dims, cam.dims, sizeof(push.dims));
	std::memcpy(push.region_origin, cam.region_origin, sizeof(push.region_origin));
	std::memcpy(push.atlas_bricks, cam.atlas_bricks, sizeof(push.atlas_bricks));
	push.cfg[0] = mode;
	push.cfg[1] = first;
	push.cfg[2] = count;
	return push;
}

} // namespace

SunBakePass::~SunBakePass() {
	teardown();
}

bool SunBakePass::initialize(RenderingDevice *rd, const GpuAtlas &atlas, RID sun_ubo) {
	teardown();
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "SunBakePass", "sun_bake.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	const RID linear = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR, true);
	const RID nearest = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	// The atlas buffers never change identity, so both sets are built once.
	set_ = gpu::uniform_set(rd, group_, program_.shader, 0, {
			gpu::sampled(0, linear, atlas.sdf_atlas()),
			gpu::sampled(1, nearest, atlas.mat_atlas()),
			gpu::storage(2, atlas.palette()),
			gpu::storage(3, atlas.region_map()),
			gpu::storage(4, atlas.region_tables()),
			gpu::storage(5, atlas.brick_flags()),
			gpu::storage(6, atlas.region_slot_counts()),
			gpu::storage(7, atlas.region_bounds()),
			gpu::image(8, atlas.brick_cache()),
			gpu::storage(9, atlas.cache_normal_ok()),
			gpu::storage(10, atlas.cache_sun_ok()),
			gpu::storage(11, atlas.jobs()),
			gpu::storage(12, atlas.cache_brick())});
	sun_set_ = gpu::uniform_set(rd, group_, program_.shader, 2, {gpu::ubo(24, sun_ubo)});
	if (!set_.is_valid() || !sun_set_.is_valid()) {
		UtilityFunctions::printerr("SunBakePass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void SunBakePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	set_ = sun_set_ = RID();
	sun_dir_[0] = sun_dir_[1] = sun_dir_[2] = 0.0f;
	rd_ = nullptr;
}

void SunBakePass::note_sun(RenderingDevice *rd, const GpuAtlas &atlas, const float dir[3]) {
	if (!ve::sun_dir_changed(sun_dir_, dir)) return;
	std::memcpy(sun_dir_, dir, sizeof(sun_dir_));
	// Device operation: the caller records this before opening its compute list. Every entry
	// was baked under the old direction, so the sweep re-bakes them all over the next seconds.
	rd->buffer_clear(atlas.cache_sun_ok(), 0,
			static_cast<uint32_t>(atlas.atlas_slot_count()) * sizeof(uint32_t));
}

void SunBakePass::bake_jobs(RenderingDevice *rd, const GpuAtlas &atlas,
		const ve::CameraParams &cam) {
	if (!program_.pipeline.is_valid()) return;
	// dispatch_args is this frame's job count: one workgroup per generated brick.
	record(rd, make_push(cam, 0, 0, 0), 0, atlas.dispatch_args());
}

void SunBakePass::bake_sweep(RenderingDevice *rd, const GpuAtlas &atlas,
		const ve::CameraParams &cam, int first_slot, int count) {
	if (!program_.pipeline.is_valid()) return;
	const int slots = atlas.atlas_slot_count();
	count = std::min(count, slots - first_slot);
	for (int done = 0; done < count; done += kMaxGroupsPerDispatch) {
		const int groups = std::min(count - done, kMaxGroupsPerDispatch);
		record(rd, make_push(cam, 1, first_slot + done, groups), groups, RID());
	}
}

void SunBakePass::record(RenderingDevice *rd, const ve::SunBakePush &push, int groups,
		RID indirect) {
	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set_, 0);
	rd->compute_list_bind_uniform_set(list, sun_set_, 2);
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	if (indirect.is_valid()) rd->compute_list_dispatch_indirect(list, indirect, 0);
	else rd->compute_list_dispatch(list, groups, 1, 1);
	rd->compute_list_end();
}
