#include "render/leaf_raster_pass.h"
#include "render/gbuffer.h"
#include "render/leaf_scatter_pass.h"
#include "gpu_layout/blocks.h"
#include "gpu_layout/gbuffer_layout.h"

using namespace godot;

namespace {
// Mirrors leaf.vert.glsl's Push block (mat4 view_proj; vec4 cam), the same way
// ve::GrassRasterPush mirrors grass's. 80 bytes; the cam slot's w is unused.
struct LeafRasterPush {
	float view_proj[16];
	float cam[4];
};
} // namespace

LeafRasterPass::~LeafRasterPass() {
	teardown();
}

void LeafRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "LeafRasterPass", "leaf.vert.glsl", "leaf.frag.glsl");
	if (!shader_.is_valid()) teardown();
}

void LeafRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = RID();
	framebuffer_ = gpu::FramebufferCache();
	set_ = gpu::SetCache();
	last_vertex_count_ = 0;
	rd_ = nullptr;
}

void LeafRasterPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

bool LeafRasterPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb) {
	if (!shader_.is_valid()) return false;
	// No marker attachment: cards write exactly the two colour channels the far field
	// writes, plus depth.
	if (!framebuffer_.get(rd, group_, {gb.albedo(), gb.surface(), gb.depth()}).is_valid()) return false;
	if (!pipeline_.is_valid()) {
		gpu::RasterState state;
		// Cards are billboards seen from both sides; the vertex turns their normals to
		// the viewer (leaf.vert.glsl), so backfaces must draw, not vanish.
		state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
		state.front = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		// Reverse-Z (M1 errata 2): near = 1, far = 0. GREATER_OR_EQUAL both writes our
		// depth where it is nearer than the current buffer and leaves nearer geometry
		// untouched -- the same compare the far field and grass use, so cards occlude
		// each other and are occluded correctly with no sort.
		state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		state.color_attachments = ve::layout::kGbColorAttachments;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}

bool LeafRasterPass::ensure_uniform_set(RenderingDevice *rd, LeafScatterPass &scatter) {
	gpu::RdDevice device{rd};
	return set_.get(device, group_, shader_, 0, {
			gpu::storage(0, scatter.instance_buffer()),
			gpu::ubo(1, scatter.params_buffer())}).is_valid();
}

bool LeafRasterPass::draw(RenderingDevice *rd, LeafScatterPass &scatter, GBuffer &gb,
		const Projection &view_proj, const float cam_pos[3]) {
	last_vertex_count_ = 0;
	if (!rd_ || rd != rd_ || !shader_.is_valid() || !gb.is_valid()) return false;
	const RID instances = scatter.instance_buffer();
	// Task 11's naming ruling: draw_args_buffer() is the 12-byte DISPATCH args; the real
	// 16-byte DRAW args (vertex_count, instance_count, first_vertex, first_instance) that
	// stage 2 grows with atomicMax on emitted clumps live in raster_draw_args_buffer().
	const RID args = scatter.raster_draw_args_buffer();
	if (!instances.is_valid() || !args.is_valid()) return true; // nothing placed: not a failure
	if (!ensure_pipeline(rd, gb)) return false;
	if (!ensure_uniform_set(rd, scatter)) return false;

	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set_.id(), 0);
	LeafRasterPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) push.view_proj[c * 4 + r] = view_proj.columns[c][r];
	push.cam[0] = cam_pos[0];
	push.cam[1] = cam_pos[1];
	push.cam[2] = cam_pos[2];
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	// One non-indexed indirect draw; the vertex count is whatever the scatter wrote.
	rd->draw_list_draw_indirect(dl, false, args, 0, 1, 16);
	rd->draw_list_end();
	// Six vertices per clump: two triangles per card -- must track the atomicMax in
	// leaf_scatter.comp.glsl and the corner decode in leaf.vert.glsl. This is a report of
	// what the GPU drew, not a command, so a disagreement here is a silent mis-count
	// rather than corruption.
	last_vertex_count_ = scatter.last_clump_count() * 6;
	return true;
}
