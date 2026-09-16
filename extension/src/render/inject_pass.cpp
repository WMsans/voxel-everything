#include "render/inject_pass.h"

using namespace godot;

InjectPass::~InjectPass() {
	teardown();
}

void InjectPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "InjectPass", "inject.vert.glsl", "inject.frag.glsl");
	if (!shader_.is_valid()) return;
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
}

void InjectPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

void InjectPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = sampler_linear_ = sampler_nearest_ = RID();
	framebuffer_ = gpu::FramebufferCache();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

bool InjectPass::ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth) {
	if (!shader_.is_valid()) return false;
	if (!framebuffer_.get(rd, group_, {dst_color, dst_depth}).is_valid()) return false;
	if (!pipeline_.is_valid()) {
		gpu::RasterState state;
		state.color_attachments = 1;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}

bool InjectPass::draw(RenderingDevice *rd, RID dst_color, RID dst_depth, RID lit, RID gb_depth) {
	if (!ensure_pipeline(rd, dst_color, dst_depth)) return false;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, shader_, 0, {
			gpu::sampled(0, sampler_linear_, lit),
			gpu::sampled(1, sampler_nearest_, gb_depth)});
	if (!set.is_valid()) return false;
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	rd->draw_list_draw(dl, false, 1, 3);
	rd->draw_list_end();
	return true;
}
