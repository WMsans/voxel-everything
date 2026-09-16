#include "render/grass_raster_pass.h"
#include "render/gbuffer.h"
#include "render/grass_scatter_pass.h"
#include "gpu_layout/blocks.h"

using namespace godot;

GrassRasterPass::~GrassRasterPass() {
	teardown();
}

void GrassRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "GrassRasterPass", "grass.vert.glsl", "grass.frag.glsl");
	if (!shader_.is_valid()) teardown();
}

void GrassRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = RID();
	framebuffer_ = gpu::FramebufferCache();
	set_ = gpu::SetCache();
	last_vertex_count_ = 0;
	rd_ = nullptr;
}

void GrassRasterPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

bool GrassRasterPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb) {
	if (!shader_.is_valid()) return false;
	// No marker attachment: blades write exactly the two colour channels the far field
	// writes, plus depth.
	if (!framebuffer_.get(rd, group_, {gb.albedo(), gb.surface(), gb.depth()}).is_valid()) return false;
	if (!pipeline_.is_valid()) {
		gpu::RasterState state;
		// Blades are single triangles seen from both sides.
		state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
		state.front = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		// Reverse-Z (M1 errata 2): near = 1, far = 0. GREATER_OR_EQUAL both writes our
		// depth where it is nearer than the current buffer and leaves nearer geometry
		// untouched -- the same compare the far field uses, so blades occlude correctly.
		state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		state.color_attachments = 2;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}

bool GrassRasterPass::ensure_uniform_set(RenderingDevice *rd, GrassScatterPass &scatter) {
	gpu::RdDevice device{rd};
	return set_.get(device, group_, shader_, 0, {
			gpu::storage(0, scatter.instance_buffer()),
			gpu::ubo(1, scatter.params_buffer())}).is_valid();
}

bool GrassRasterPass::draw(RenderingDevice *rd, GrassScatterPass &scatter, GBuffer &gb,
		const Projection &view_proj, const float cam_pos[3]) {
	last_vertex_count_ = 0;
	if (!rd_ || rd != rd_ || !shader_.is_valid() || !gb.is_valid()) return false;
	const RID instances = scatter.instance_buffer();
	const RID args = scatter.draw_args_buffer();
	if (!instances.is_valid() || !args.is_valid()) return true; // nothing placed: not a failure
	if (!ensure_pipeline(rd, gb)) return false;
	if (!ensure_uniform_set(rd, scatter)) return false;

	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set_.id(), 0);
	ve::GrassRasterPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) push.view_proj[c * 4 + r] = view_proj.columns[c][r];
	push.cam[0] = cam_pos[0];
	push.cam[1] = cam_pos[1];
	push.cam[2] = cam_pos[2];
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	// One non-indexed indirect draw; the vertex count is whatever the scatter wrote.
	rd->draw_list_draw_indirect(dl, false, args, 0, 1, 16);
	rd->draw_list_end();
	// Twenty-seven vertices per blade -- must track the atomicMax in grass_scatter.comp.glsl
	// and the corner decode in grass.vert.glsl. This is a report of what the GPU drew, not a
	// command, so a disagreement here is a silent mis-count rather than corruption.
	last_vertex_count_ = scatter.last_blade_count() * 27;
	return true;
}
