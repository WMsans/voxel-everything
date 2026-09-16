#include "render/deferred_pass.h"
#include "render/gbuffer.h"
#include "render/material_atlas.h"
#include "shade/beauty_settings.h"
#include <godot_cpp/variant/typed_array.hpp>
#include <cstring>

using namespace godot;

DeferredPass::~DeferredPass() {
	teardown();
}

void DeferredPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "DeferredPass", "deferred.comp.glsl");
	if (!program_.valid()) return;
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
}

void DeferredPass::set_sun_ubo(RID buffer) {
	// The uniform set keys on this RID, so the next render rebuilds it.
	sun_light_ubo_ = buffer;
}

void DeferredPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_linear_ = sampler_nearest_ = RID();
	dummy_black_ = dummy_far_ = dummy_white_ = sun_ubo_ = RID();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

bool DeferredPass::ensure_dummies(RenderingDevice *rd) {
	if (dummy_black_.is_valid() && dummy_far_.is_valid() && sun_ubo_.is_valid()) return true;
	auto make_1x1 = [&](RenderingDevice::DataFormat fmt, const PackedByteArray &bytes) {
		TypedArray<PackedByteArray> data;
		data.push_back(bytes);
		return gpu::texture(rd, group_, fmt, Vector2i(1, 1),
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT,
				data);
	};
	PackedByteArray black;
	black.resize(8);
	black.fill(0);
	dummy_black_ = make_1x1(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, black);
	PackedByteArray white;
	white.resize(8);
	// AO = 1 everywhere: a missing SSAO input must leave the ambient term untouched.
	white.fill(0x3C00); // half float 1.0
	dummy_white_ = make_1x1(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, white);
	PackedByteArray far;
	far.resize(4);
	far.fill(0);
	dummy_far_ = make_1x1(RenderingDevice::DATA_FORMAT_R32_SFLOAT, far);
	PackedByteArray zeros;
	zeros.resize(256);
	zeros.fill(0);
	sun_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(256, zeros));
	return dummy_black_.is_valid() && dummy_far_.is_valid() && dummy_white_.is_valid() && sun_ubo_.is_valid();
}

bool DeferredPass::render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
		RID ssgi, RID ssao, RID sun_map, const Params &p) {
	if (!is_valid() || !gb.is_valid()) return false;
	if (!ensure_dummies(rd)) return false;
	gpu::CpuTimer timer(last_ms_);
	uint32_t flags = p.flags;
	if (!ssgi.is_valid()) flags &= ~ve::kFlagSsgi;
	if (!ssao.is_valid()) flags &= ~ve::kFlagSsao;
	if (!sun_map.is_valid()) flags &= ~ve::kFlagSunMap;
	const RID ssgi_bound = ssgi.is_valid() ? ssgi : dummy_black_;
	const RID ssao_bound = ssao.is_valid() ? ssao : dummy_white_;
	const RID sun_bound = sun_map.is_valid() ? sun_map : dummy_far_;
	gpu::RdDevice device{rd};
	// SSAO lives at binding 7, after the material arrays' reserved slots. Linear, not
	// nearest: the pass renders at half the G-buffer size, so this sampler is what
	// upsamples it. Nearest here would show the half-res grid as 2x2 blocks.
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, sampler_nearest_, gb.albedo()),
			gpu::sampled(1, sampler_nearest_, gb.surface()),
			gpu::sampled(2, sampler_nearest_, gb.depth()),
			gpu::sampled(3, sampler_linear_, ssgi_bound),
			gpu::sampled(4, sampler_linear_, sun_bound),
			gpu::image(5, gb.lit()),
			gpu::ubo(6, sun_ubo_),
			gpu::sampled(7, sampler_linear_, ssao_bound),
			gpu::sampled(8, materials.sampler(), materials.albedo_array()),
			gpu::sampled(9, materials.sampler(), materials.surface_array()),
			gpu::ubo(10, sun_light_ubo_)});
	if (!set.is_valid()) return false;

	// std140: mat4[3] = 192 B, then vec4[3] = 48 B, then vec4 splits = 16 B. 256 total.
	PackedByteArray ub;
	ub.resize(256);
	ub.fill(0);
	float *uf = reinterpret_cast<float *>(ub.ptrw());
	const int n = sun_map.is_valid() ? p.cascade_count : 0;
	for (int c = 0; c < ve::kSunCascades; c++)
		for (int i = 0; i < 16; i++)
			uf[c * 16 + i] = c < n ? p.sun_view_proj[c][i] : 0.0f;
	for (int c = 0; c < ve::kSunCascades; c++) {
		uf[48 + c * 4 + 0] = c < n ? p.shadow_texel[c] : 0.0f;
		uf[48 + c * 4 + 1] = c < n ? p.shadow_depth_range_c[c] : 0.0f;
	}
	uf[48 + 0*4 + 2] = n > 0 ? p.fade_start : 0.0f;
	uf[48 + 0*4 + 3] = n > 0 ? p.fade_end : 0.0f;
	for (int c = 0; c < ve::kSunCascades; c++)
		uf[60 + c] = c < n ? p.cascade_split[c] : 0.0f;
	uf[63] = float(n);
	rd->buffer_update(sun_ubo_, 0, 256, ub);

	static_assert(sizeof(float) * 28 == 112, "deferred push block");
	PackedByteArray pcb;
	pcb.resize(112);
	float *f = reinterpret_cast<float *>(pcb.ptrw());
	for (int i = 0; i < 16; i++) f[i] = p.inv_view_proj[i];
	f[16] = p.cam_pos[0];
	f[17] = p.cam_pos[1];
	f[18] = p.cam_pos[2];
	f[19] = 0.0f;
	f[20] = p.ambient[0];
	f[21] = p.ambient[1];
	f[22] = p.ambient[2];
	f[23] = 0.0f;
	uint32_t *u = reinterpret_cast<uint32_t *>(pcb.ptrw());
	u[24] = flags;
	u[25] = static_cast<uint32_t>(p.probe_mode);
	u[26] = 0;
	u[27] = 0;

	const Vector2i size = gb.size();
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, pcb, gpu::groups(size.x, 8),
			gpu::groups(size.y, 8));
}
