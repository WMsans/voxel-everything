#include "render/transparency_composite_pass.h"
#include "render/gbuffer.h"
#include "render/material_atlas.h"
#include "gpu_layout/blocks.h"
#include "shade/beauty_settings.h"
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

TransparencyCompositePass::~TransparencyCompositePass() {
	teardown();
}

void TransparencyCompositePass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "TransparencyCompositePass",
			"transparency_composite.comp.glsl");
	if (!program_.valid()) return;
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	// A 1x1 stand-in sun map for frames with none, exactly as DeferredPass keeps one (its
	// make_1x1, verbatim); the shader never reads it because BEAUTY_SUN_MAP is cleared with
	// it. A plain 2D texture DOES satisfy the sampler2DArray descriptor on this backend --
	// DeferredPass binds exactly this one to deferred.comp.glsl's `sampler2DArray sun_map`.
	PackedByteArray far;
	far.resize(4);
	far.fill(0);
	TypedArray<PackedByteArray> data;
	data.push_back(far);
	dummy_far_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32_SFLOAT, Vector2i(1, 1),
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT, data);
}

void TransparencyCompositePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_nearest_ = sampler_linear_ = dummy_far_ = RID();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

bool TransparencyCompositePass::render(RenderingDevice *rd, GBuffer &gb,
		const MaterialAtlas &materials, RID front, RID thickness, RID sun_map,
		RID sun_cascade_ubo, RID beauty_cam_ubo, const Params &p) {
	if (!is_valid() || !gb.is_valid() || !front.is_valid() || !thickness.is_valid() ||
			!sun_cascade_ubo.is_valid() || !beauty_cam_ubo.is_valid() || !sun_light_ubo_.is_valid())
		return false;
	uint32_t flags = p.flags;
	if (!sun_map.is_valid()) flags &= ~ve::kFlagSunMap;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::image(0, gb.lit()),
			gpu::sampled(1, sampler_nearest_, gb.depth()),
			gpu::sampled(2, sampler_nearest_, front),
			gpu::sampled(3, sampler_nearest_, thickness),
			gpu::sampled(4, sampler_linear_, sun_map.is_valid() ? sun_map : dummy_far_),
			gpu::ubo(5, sun_cascade_ubo),
			gpu::sampled(6, materials.sampler(), materials.albedo_array()),
			gpu::sampled(7, materials.sampler(), materials.surface_array()),
			gpu::ubo(8, beauty_cam_ubo),
			gpu::ubo(9, sun_light_ubo_)});
	if (!set.is_valid()) return false;
	const ve::TransparencyCompositePush push{
			{p.right[0], p.right[1], p.right[2], p.tan_x},
			{p.up[0], p.up[1], p.up[2], p.tan_y},
			{p.ambient[0], p.ambient[1], p.ambient[2], 0.0f},
			{p.min_transmit, p.sky_thickness_m, 0.0f, 0.0f},
			{flags, p.inside_material, 0u, 0u}};
	const Vector2i size = gb.size();
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, gpu::push_bytes(push),
			gpu::groups(size.x, 8), gpu::groups(size.y, 8));
}
