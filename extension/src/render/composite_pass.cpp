#include "render/composite_pass.h"
#include "render/gbuffer.h"
#include "render/material_atlas.h"
#include <godot_cpp/variant/packed_color_array.hpp>
#include <cstring>

using namespace godot;

CompositePass::~CompositePass() {
	teardown();
}

void CompositePass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "CompositePass", "composite.vert.glsl",
			"composite.frag.glsl");
	shader_marker_ = gpu::compile_raster(rd, group_, "CompositePass", "composite.vert.glsl",
			"composite.frag.glsl", "#define SEAM_MARKER 1\n");
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
}

void CompositePass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

void CompositePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = shader_marker_ = pipeline_ = sampler_linear_ = sampler_nearest_ = RID();
	set_ = gpu::SetCache();
	framebuffer_ = gpu::FramebufferCache();
	rd_ = nullptr;
}

bool CompositePass::ensure_pipeline(RenderingDevice *rd, RID albedo, RID surface, RID depth,
		RID marker) {
	const bool want_marker = marker.is_valid();
	const RID shader = want_marker ? shader_marker_ : shader_;
	if (!shader.is_valid()) return false;
	const std::vector<RID> attachments = want_marker
			? std::vector<RID>{albedo, surface, marker, depth}
			: std::vector<RID>{albedo, surface, depth};
	if (!framebuffer_.get(rd, group_, attachments).is_valid()) return false;
	if (!pipeline_.is_valid() || pipeline_marker_ != want_marker) {
		gpu::RdDevice device{rd};
		group_.free(device, pipeline_);
		gpu::RasterState state;
		state.color_attachments = want_marker ? 3 : 2;
		pipeline_marker_ = want_marker;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}

void CompositePass::draw(RenderingDevice *rd, GBuffer &gb, RID src_overlay, RID src_surface,
		RID src_hitpos, const Projection &view_proj, const MaterialAtlas &materials,
		const ve::CameraParams &cam, float fade_start, float fade_end, RID marker) {
	last_draw_ok_ = false;
	const RID shader = marker.is_valid() ? shader_marker_ : shader_;
	if (!shader.is_valid() || !gb.is_valid()) return;
	if (!ensure_pipeline(rd, gb.albedo(), gb.surface(), gb.depth(), marker)) return;

	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, shader, 0, {
			gpu::sampled(0, sampler_linear_, src_overlay),
			gpu::sampled(1, sampler_nearest_, src_hitpos),
			gpu::sampled(2, materials.sampler(), materials.albedo_array()),
			gpu::sampled(3, materials.sampler(), materials.surface_array()),
			gpu::sampled(4, sampler_nearest_, src_surface)});
	if (!set.is_valid()) return;

	// Exactly 128 bytes: Vulkan's guaranteed minimum push-constant size, so still portable.
	// Both stages declare the same five vec4s (Godot rejects differing reflections between
	// stages of one pipeline), and the vertex stage ignores everything but the block's shape.
	PackedByteArray pc;
	pc.resize(128);
	{
		float *f = reinterpret_cast<float *>(pc.ptrw());
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				f[c * 4 + r] = view_proj.columns[c][r];
		f[16] = cam.cam_pos[0];
		f[17] = cam.cam_pos[1];
		f[18] = cam.cam_pos[2];
		// NOT cam.cam_pos[3]: the marcher's block hides the packed beauty flags in that slot.
		f[19] = fade_start;
		f[20] = fade_end;
		f[21] = cam.cam_fwd[0];
		f[22] = cam.cam_fwd[1];
		f[23] = cam.cam_fwd[2];
		f[24] = cam.cam_right[0];
		f[25] = cam.cam_right[1];
		f[26] = cam.cam_right[2];
		f[27] = cam.params[0]; // tan(fov_x / 2)
		f[28] = cam.cam_up[0];
		f[29] = cam.cam_up[1];
		f[30] = cam.cam_up[2];
		f[31] = cam.params[1]; // tan(fov_y / 2)
	}

	PackedColorArray clears;
	clears.push_back(Color(0, 0, 0, 0));
	clears.push_back(Color(0, 0, 0, 0));
	if (marker.is_valid()) clears.push_back(Color(0, 0, 0, 0));
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(),
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH,
			clears, 0.0f);
	if (dl < 0) return;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	rd->draw_list_draw(dl, false, 1, 3);
	rd->draw_list_end();
	last_draw_ok_ = true;
}
