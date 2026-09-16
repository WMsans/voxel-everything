#include "render/brick_gen_pass.h"
#include "render/field_context_set.h"
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

BrickGenPass::~BrickGenPass() {
	teardown();
}

bool BrickGenPass::initialize(RenderingDevice *rd, const GpuAtlas &atlas) {
	teardown();
	rd_ = rd;
	atlas_bricks_ = atlas.config().atlas_bricks;
	program_ = gpu::compile_compute(rd, group_, "BrickGenPass", "brick_gen.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	// The atlas buffers never change identity, so the uniform set is built once.
	set_ = gpu::uniform_set(rd, group_, program_.shader, 0, {
			gpu::image(0, atlas.sdf_atlas()),
			gpu::image(1, atlas.mat_atlas()),
			gpu::image(2, atlas.mip_atlas(0)),
			gpu::image(3, atlas.mip_atlas(1)),
			gpu::image(4, atlas.mip_atlas(2)),
			gpu::storage(5, atlas.palette()),
			gpu::storage(6, atlas.jobs()),
			gpu::storage(7, atlas.op_pool()),
			gpu::storage(8, atlas.volumes().sdf_buffer()),
			gpu::storage(9, atlas.volumes().mat_buffer()),
			gpu::storage(10, atlas.brick_flags()),
			gpu::storage(11, atlas.overrides().sdf_buffer()),
			gpu::storage(12, atlas.overrides().mat_buffer()),
			gpu::storage(13, atlas.overrides().tables()),
			gpu::storage(14, atlas.overrides().region_table_map()),
			gpu::storage(15, atlas.region_occupancy())});
	if (!set_.is_valid()) {
		UtilityFunctions::printerr("BrickGenPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void BrickGenPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	set_ = RID();
	rd_ = nullptr;
}

void BrickGenPass::dispatch(RenderingDevice *rd, int64_t list, const GpuAtlas &atlas,
		const FieldContextSet *field_context) {
	if (!program_.pipeline.is_valid()) return;
	PackedByteArray pc;
	pc.resize(16);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = atlas_bricks_.x; p[1] = atlas_bricks_.y; p[2] = atlas_bricks_.z; p[3] = 0;
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set_, 0);
	if (field_context != nullptr) field_context->bind(rd, list);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch_indirect(list, atlas.dispatch_args(), 0);
}
