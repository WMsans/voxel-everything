#include "render/conifer_impostor_pass.h"
#include "render/conifer_scatter_pass.h"
#include "render/gbuffer.h"
#include "gpu_layout/gbuffer_layout.h"

using namespace godot;

namespace {
// Mirrors the vertex and fragment Push block: mat4 view_proj; vec4 cam.
struct ImpostorPush {
	float view_proj[16];
	float cam[4];
};
} // namespace

ConiferImpostorPass::~ConiferImpostorPass() { teardown(); }

void ConiferImpostorPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "ConiferImpostorPass", "conifer_impostor.vert.glsl",
			"conifer_impostor.frag.glsl");
	if (!shader_.is_valid()) teardown();
}

void ConiferImpostorPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = RID();
	framebuffer_ = gpu::FramebufferCache();
	set_ = gpu::SetCache();
	last_vertex_count_ = 0;
	rd_ = nullptr;
}

void ConiferImpostorPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

bool ConiferImpostorPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb) {
	if (!shader_.is_valid()) return false;
	if (!framebuffer_.get(rd, group_, {gb.albedo(), gb.surface(), gb.depth()}).is_valid()) return false;
	if (!pipeline_.is_valid()) {
		gpu::RasterState state;
		// The quad faces the camera; cull nothing so a winding flip can never drop a tree.
		state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
		state.front = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		// Reverse-Z, the far field's compare: the fragment writes the crown's real depth.
		state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		state.color_attachments = ve::layout::kGbColorAttachments;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}

bool ConiferImpostorPass::draw(RenderingDevice *rd, ConiferScatterPass &scatter, GBuffer &gb,
		const Projection &view_proj, const float cam_pos[3]) {
	last_vertex_count_ = 0;
	if (!rd_ || rd != rd_ || !shader_.is_valid() || !gb.is_valid()) return false;
	const RID list = scatter.impostor_list_buffer();
	const RID args = scatter.impostor_draw_args_buffer();
	if (!list.is_valid() || !args.is_valid()) return true;
	if (!ensure_pipeline(rd, gb)) return false;
	gpu::RdDevice device{rd};
	if (!set_.get(device, group_, shader_, 0, {
			gpu::storage(0, list),
			gpu::ubo(1, scatter.params_buffer()),
			gpu::ubo(2, scatter.raster_params_buffer())}).is_valid())
		return false;
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set_.id(), 0);
	ImpostorPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) push.view_proj[c * 4 + r] = view_proj.columns[c][r];
	push.cam[0] = cam_pos[0];
	push.cam[1] = cam_pos[1];
	push.cam[2] = cam_pos[2];
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	rd->draw_list_draw_indirect(dl, false, args, 0, 1, 16);
	rd->draw_list_end();
	last_vertex_count_ = scatter.last_impostors() * 6;
	return true;
}
