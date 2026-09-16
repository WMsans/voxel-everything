#include "render/ssao_pass.h"
#include "render/gbuffer.h"
#include "gpu_layout/blocks.h"
#include "render/gpu/gpu.h"
#include <algorithm>

using namespace godot;

// Must match the Push block in ssao.comp.glsl.
static const float kSsaoRadius = 5.0f;
static const float kSsaoStrength = 1.5f;

SsaoPass::~SsaoPass() {
	teardown();
}

void SsaoPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "SsaoPass", "ssao.comp.glsl");
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!program_.valid() || !sampler_nearest_.is_valid()) teardown();
}

void SsaoPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_nearest_ = RID();
	target_ = gpu::Target();
	set_ = gpu::SetCache();
	output_ = RID();
	rd_ = nullptr;
}

bool SsaoPass::render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
		const ve::BeautySettings &s) {
	output_ = RID();
	if (!s.ssao) return false;
	if (!rd_ || rd != rd_ || !program_.valid() || !gb.is_valid() || !camera_ubo.is_valid())
		return false;
	// Half-res, matching the SSGI and SSR chains. AO modulates only the ambient term and
	// is upsampled bilinearly by the deferred pass, so the quarter-cost target costs the
	// image far less than it costs the frame. See ssao.comp.glsl.
	const Vector2i half(std::max(1, gb.size().x / 2), std::max(1, gb.size().y / 2));
	// Defined before the first read even if the pass is skipped this frame.
	const Color unoccluded(1.0f, 1.0f, 1.0f, 1.0f);
	if (!target_.ensure(rd, group_, RenderingDevice::DATA_FORMAT_R8_UNORM, half,
				RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
						RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT,
				&unoccluded))
		return false;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, sampler_nearest_, gb.surface()),
			gpu::sampled(1, sampler_nearest_, gb.depth()),
			gpu::image(2, target_.rid()),
			gpu::ubo(5, camera_ubo)});
	if (!set.is_valid()) return false;

	gpu::CpuTimer timer(last_ms_);
	const ve::SsaoPush push{{half.x, half.y, s.ssao_steps, s.ssao_directions},
			{kSsaoRadius, kSsaoStrength, 0.0f, 0.0f}};
	if (!gpu::dispatch(rd, program_.pipeline, {{set, 0}}, gpu::push_bytes(push),
				gpu::groups(half.x, 8), gpu::groups(half.y, 8)))
		return false;
	output_ = target_.rid();
	return true;
}
