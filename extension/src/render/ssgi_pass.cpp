#include "render/ssgi_pass.h"
#include "render/gbuffer.h"
#include <algorithm>
#include <cstring>

using namespace godot;

SsgiPass::~SsgiPass() {
	teardown();
}

void SsgiPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "SsgiPass", "ssgi.comp.glsl");
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	if (!program_.valid() || !sampler_nearest_.is_valid() || !sampler_linear_.is_valid()) teardown();
}

void SsgiPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_nearest_ = sampler_linear_ = RID();
	targets_[0] = targets_[1] = raw_ = gpu::Target();
	set_ = gpu::SetCache();
	output_ = RID();
	rd_ = nullptr;
}

bool SsgiPass::render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
		const float prev_view_proj[16], bool have_history, const ve::BeautySettings &s,
		uint32_t frame) {
	output_ = RID();
	if (!s.ssgi || s.ssgi_taps <= 0) return false;
	if (!rd_ || rd != rd_ || !program_.valid() || !gb.is_valid() ||
			!camera_ubo.is_valid() || !prev_view_proj) return false;
	const Vector2i size = gb.size();
	if (size.x <= 0 || size.y <= 0) return false;
	const Vector2i half(std::max(1, size.x / 2), std::max(1, size.y / 2));
	const uint32_t usage = RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	// Defined before the first ping-pong read, including a first frame whose history is absent.
	const Color cleared(0.0f, 0.0f, 0.0f, 0.0f);
	for (gpu::Target *t : {&targets_[0], &targets_[1], &raw_})
		if (!t->ensure(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, half, usage, &cleared))
			return false;
	const uint32_t out_index = frame & 1u;
	const uint32_t prev_index = (frame + 1u) & 1u;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, sampler_nearest_, gb.surface()),
			gpu::sampled(1, sampler_nearest_, gb.depth()),
			gpu::sampled(2, sampler_linear_, gb.history()),
			gpu::sampled(3, sampler_linear_, targets_[prev_index].rid()),
			gpu::image(4, targets_[out_index].rid()),
			gpu::ubo(5, camera_ubo),
			gpu::image(6, raw_.rid()),
			gpu::sampled(7, sampler_nearest_, raw_.rid())});
	if (!set.is_valid()) return false;

	gpu::CpuTimer timer(last_ms_);
	static_assert(sizeof(float) * 32 == 128, "ssgi push block");
	PackedByteArray pc;
	pc.resize(128);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	std::memcpy(f, prev_view_proj, sizeof(float) * 16);
	int32_t *dims = reinterpret_cast<int32_t *>(f + 16);
	dims[0] = half.x;
	dims[1] = half.y;
	dims[2] = s.ssgi_taps;
	dims[3] = have_history ? 1 : 0;
	// These were literals here until the emissive work: 6 m, 0.90, 1.0. They are knobs in
	// ve::BeautySettings now, which is where that struct always said every knob a pass reads
	// has to live -- and which is what lets a tier move the emissive ring.
	f[20] = s.ssgi_radius;
	f[21] = s.ssgi_temporal;
	f[22] = s.ssgi_strength;
	f[23] = 0.0f;
	f[24] = s.emissive_gi_radius;
	f[25] = s.emissive_gi_strength;
	f[26] = 0.0f;
	f[27] = 0.0f;
	int32_t *stage = reinterpret_cast<int32_t *>(f + 28);
	stage[0] = 0;
	stage[1] = stage[2] = stage[3] = 0;
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	// The gather rotates its taps by a bayer4 phase that never changes, so without this second
	// dispatch that phase reaches the screen as a lattice of dots. It averages one full 4x4
	// period back out before the temporal blend, which cannot.
	rd->compute_list_add_barrier(list);
	stage[0] = 1;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_end();
	output_ = targets_[out_index].rid();
	return true;
}
