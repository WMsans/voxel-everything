#include "render/contact_shadow_pass.h"
#include <algorithm>

using namespace godot;

ContactShadowPass::~ContactShadowPass() {
	teardown();
}

void ContactShadowPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "ContactShadowPass", "contact_shadow.comp.glsl");
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	if (!program_.valid() || !sampler_nearest_.is_valid() || !sampler_linear_.is_valid()) teardown();
}

void ContactShadowPass::set_sun_ubo(RID buffer) {
	// The uniform set keys on this RID, so the next render rebuilds it.
	sun_light_ubo_ = buffer;
}

void ContactShadowPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_nearest_ = sampler_linear_ = RID();
	mask_ = gpu::Target();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

bool ContactShadowPass::render(RenderingDevice *rd, RID scene_color, RID scene_depth,
		Vector2i size, RID camera_ubo, const ve::BeautySettings &s) {
	if (!s.contact_shadows || s.contact_steps <= 0) return false;
	if (!rd_ || rd != rd_ || !program_.valid() || !scene_color.is_valid() ||
			!scene_depth.is_valid() || !camera_ubo.is_valid() || size.x <= 0 || size.y <= 0)
		return false;
	const Vector2i half(std::max(1, size.x / 2), std::max(1, size.y / 2));
	if (!mask_.ensure(rd, group_, RenderingDevice::DATA_FORMAT_R8_UNORM, half,
				RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
						RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT))
		return false;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, sampler_nearest_, scene_depth),
			gpu::image(1, mask_.rid()),
			gpu::sampled(2, sampler_linear_, mask_.rid()),
			gpu::image(3, scene_color),
			gpu::ubo(4, camera_ubo),
			gpu::ubo(5, sun_light_ubo_)});
	if (!set.is_valid()) return false;

	// This is command-record time, not GPU execution time (M5 errata 15).
	gpu::CpuTimer timer(last_ms_);
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	PackedByteArray pc;
	pc.resize(32);
	int32_t *dims = reinterpret_cast<int32_t *>(pc.ptrw());
	dims[0] = half.x;
	dims[1] = half.y;
	dims[2] = 0;
	dims[3] = s.contact_steps;
	float *params = reinterpret_cast<float *>(pc.ptrw()) + 4;
	params[0] = 0.6f;
	params[1] = 0.85f;
	// One voxel: large enough to leave the receiver, but too small to bridge terrain gaps.
	params[2] = 0.05f;
	params[3] = 0.0f;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_add_barrier(list);
	dims[0] = size.x;
	dims[1] = size.y;
	dims[2] = 1;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_end();
	return true;
}
