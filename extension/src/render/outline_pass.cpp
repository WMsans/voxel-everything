#include "render/outline_pass.h"
#include "gpu_layout/blocks.h"

using namespace godot;

OutlinePass::~OutlinePass() {
	teardown();
}

bool OutlinePass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "OutlinePass", "outline.comp.glsl");
	nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	dummy_normal_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			Vector2i(1, 1),
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	if (dummy_normal_.is_valid())
		rd->texture_clear(dummy_normal_, Color(0.5f, 0.5f, 1.0f, 1.0f), 0, 1, 0, 1);
	if (!program_.valid() || !nearest_.is_valid() || !dummy_normal_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void OutlinePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	nearest_ = dummy_normal_ = RID();
	set_ = gpu::SetCache();
	last_ms_ = 0.0f;
	rd_ = nullptr;
}

bool OutlinePass::render(RenderingDevice *rd, RID scene_color, RID scene_depth, RID gb_depth,
		RID gb_surface, RID normal_roughness, bool have_normal_roughness, RID camera_ubo,
		Vector2i size, const ve::BeautySettings &s) {
	if (!s.outlines) return false;
	if (!rd_ || rd != rd_ || !program_.valid() || size.x <= 0 || size.y <= 0 ||
			!scene_color.is_valid() || !scene_depth.is_valid() || !gb_depth.is_valid() ||
			!gb_surface.is_valid() || !camera_ubo.is_valid()) return false;
	const RID normal = normal_roughness.is_valid() ? normal_roughness : dummy_normal_;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, nearest_, scene_depth),
			gpu::sampled(1, nearest_, gb_depth),
			gpu::sampled(2, nearest_, gb_surface),
			gpu::sampled(3, nearest_, normal),
			gpu::image(4, scene_color),
			gpu::ubo(6, camera_ubo)});
	if (!set.is_valid()) return false;
	gpu::CpuTimer timer(last_ms_);
	const ve::OutlinePush push{
			{size.x, size.y, have_normal_roughness && normal_roughness.is_valid() ? 1 : 0, 0},
			{s.outline_depth_threshold, s.outline_normal_threshold, 0.35f, 0.0f}};
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, gpu::push_bytes(push),
			gpu::groups(size.x, 8), gpu::groups(size.y, 8));
}
