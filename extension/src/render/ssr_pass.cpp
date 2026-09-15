#include "render/ssr_pass.h"
#include <algorithm>

using namespace godot;

SsrPass::~SsrPass() {
	teardown();
}

bool SsrPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	trace_ = gpu::compile_compute(rd, group_, "SsrPass trace", "ssr.comp.glsl");
	apply_ = gpu::compile_compute(rd, group_, "SsrPass apply", "ssr.comp.glsl", "#define SSR_APPLY 1\n");
	nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	dummy_normal_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			Vector2i(1, 1),
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	if (dummy_normal_.is_valid())
		rd->texture_clear(dummy_normal_, Color(0.0f, 0.0f, 0.0f, 1.0f), 0, 1, 0, 1);
	if (!trace_.valid() || !apply_.valid() || !nearest_.is_valid() || !linear_.is_valid() ||
			!dummy_normal_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void SsrPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	trace_ = apply_ = gpu::Program();
	nearest_ = linear_ = dummy_normal_ = RID();
	reflection_ = gpu::Target();
	trace_set_ = apply_set_ = gpu::SetCache();
	last_ms_ = 0.0f;
	rd_ = nullptr;
}

bool SsrPass::render(RenderingDevice *rd, RID scene_color, RID scene_depth, RID gb_surface,
		RID gb_depth, RID normal_roughness, bool have_normal_roughness, RID camera_ubo,
		Vector2i size, const ve::BeautySettings &s) {
	if (!s.ssr || s.ssr_steps <= 0) return false;
	if (!rd_ || rd != rd_ || !trace_.valid() || !apply_.valid() || !scene_color.is_valid() ||
			!scene_depth.is_valid() || !gb_surface.is_valid() || !gb_depth.is_valid() ||
			!camera_ubo.is_valid()) return false;
	if (size.x <= 0 || size.y <= 0) return false;
	const Vector2i half(std::max(1, size.x / 2), std::max(1, size.y / 2));
	if (!reflection_.ensure(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, half,
				RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
						RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT))
		return false;
	const RID normal = normal_roughness.is_valid() ? normal_roughness : dummy_normal_;
	gpu::RdDevice device{rd};
	const RID trace_set = trace_set_.get(device, group_, trace_.shader, 0, {
			gpu::sampled(0, linear_, scene_color),
			gpu::sampled(1, nearest_, scene_depth),
			gpu::sampled(2, nearest_, gb_surface),
			gpu::sampled(3, nearest_, gb_depth),
			gpu::sampled(4, nearest_, normal),
			gpu::image(5, reflection_.rid()),
			gpu::ubo(6, camera_ubo)});
	const RID apply_set = apply_set_.get(device, group_, apply_.shader, 0, {
			gpu::sampled(0, linear_, reflection_.rid()),
			gpu::image(1, scene_color)});
	if (!trace_set.is_valid() || !apply_set.is_valid()) return false;
	gpu::CpuTimer timer(last_ms_);
	PackedByteArray trace_pc;
	trace_pc.resize(32);
	int32_t *i = reinterpret_cast<int32_t *>(trace_pc.ptrw());
	float *f = reinterpret_cast<float *>(trace_pc.ptrw());
	i[0] = half.x; i[1] = half.y; i[2] = s.ssr_steps;
	i[3] = have_normal_roughness && normal_roughness.is_valid() ? 1 : 0;
	f[4] = kReachM; f[5] = kStartBiasM; f[6] = kThicknessM; f[7] = kStrength;
	if (!gpu::dispatch(rd, trace_.pipeline, {{trace_set, 0}}, trace_pc, gpu::groups(half.x, 8),
				gpu::groups(half.y, 8)))
		return false;
	PackedByteArray apply_pc;
	apply_pc.resize(16);
	int32_t *dims = reinterpret_cast<int32_t *>(apply_pc.ptrw());
	dims[0] = size.x; dims[1] = size.y; dims[2] = dims[3] = 0;
	return gpu::dispatch(rd, apply_.pipeline, {{apply_set, 0}}, apply_pc, gpu::groups(size.x, 8),
			gpu::groups(size.y, 8));
}
