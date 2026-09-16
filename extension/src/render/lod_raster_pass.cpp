#include "render/lod_raster_pass.h"
#include "render/gbuffer.h"
#include "render/lod_pool.h"
#include "render/material_atlas.h"
#include "lod/lod_contour.h"
#include "gpu_layout/blocks.h"
#include <godot_cpp/variant/packed_color_array.hpp>
#include <chrono>
#include <cstring>

using namespace godot;

LodRasterPass::~LodRasterPass() {
	teardown();
}

void LodRasterPass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "LodRasterPass", "lod.vert.glsl", "lod.frag.glsl");
	// Same production/debug split as CompositePass: the marker output is compiled only for
	// the seam-probe shader variant, so production pipelines keep exactly one fragment output
	// and match the scene framebuffer's color mask.
	shader_marker_ = gpu::compile_raster(rd, group_, "LodRasterPass", "lod.vert.glsl",
			"lod.frag.glsl", "#define SEAM_MARKER 1\n");
}

void LodRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = shader_marker_ = RID();
	pipeline_cull_off_ = pipeline_cull_ccw_ = pipeline_cull_cw_ = RID();
	index_array_ = index_array_buffer_ = RID();
	set_ = gpu::SetCache();
	framebuffer_ = gpu::FramebufferCache();
	draw_pages_.clear();
	rd_ = nullptr;
}

void LodRasterPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

void LodRasterPass::set_draw_pages(const std::vector<PageDraw> &pages) {
	draw_pages_ = pages;
}

bool LodRasterPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb, RID marker) {
	const bool want_marker = marker.is_valid();
	const RID shader = want_marker ? shader_marker_ : shader_;
	if (!shader.is_valid()) return false;
	const std::vector<RID> attachments = want_marker
			? std::vector<RID>{gb.albedo(), gb.surface(), marker, gb.depth()}
			: std::vector<RID>{gb.albedo(), gb.surface(), gb.depth()};
	if (!framebuffer_.get(rd, group_, attachments).is_valid()) return false;
	if (pipeline_cull_off_.is_valid() && pipeline_cull_ccw_.is_valid() &&
			pipeline_cull_cw_.is_valid() && pipeline_marker_ == want_marker)
		return true;
	gpu::RdDevice device{rd};
	for (RID *p : {&pipeline_cull_off_, &pipeline_cull_ccw_, &pipeline_cull_cw_}) {
		group_.free(device, *p);
		*p = RID();
	}
	gpu::RasterState state;
	// Reverse-Z (M1 errata 2): near = 1, far = 0. GREATER_OR_EQUAL both writes our depth
	// where it is nearer than the current buffer and leaves nearer geometry untouched.
	state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	state.color_attachments = want_marker ? 3 : 2;
	// Debug seam probe: the LoD marker (2) must OR into the composite marker (1) so
	// double-claimed band pixels read 3. Production pipelines have no marker attachment.
	state.logic_or = want_marker;
	state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
	state.front = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
	pipeline_cull_off_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	state.cull = RenderingDevice::POLYGON_CULL_BACK;
	pipeline_cull_ccw_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	state.front = RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE;
	pipeline_cull_cw_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	pipeline_marker_ = want_marker;
	return pipeline_cull_off_.is_valid() && pipeline_cull_ccw_.is_valid() &&
			pipeline_cull_cw_.is_valid();
}

bool LodRasterPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials,
		RID shader) {
	gpu::RdDevice device{rd};
	return set_.get(device, group_, shader, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::sampled(3, materials.sampler(), materials.albedo_array()),
			gpu::sampled(4, materials.sampler(), materials.surface_array()),
			gpu::storage(5, pool.normal_buffer())}).is_valid();
}

RID LodRasterPass::active_pipeline() const {
	if (!cull_enabled_) return pipeline_cull_off_;
	return front_face_clockwise_ ? pipeline_cull_cw_ : pipeline_cull_ccw_;
}

bool LodRasterPass::ensure_index_array(RenderingDevice *rd, LodPool &pool) {
	const RID index_buffer = pool.index_buffer();
	if (index_array_.is_valid() && index_buffer == index_array_buffer_) return true;
	gpu::RdDevice device{rd};
	group_.free(device, index_array_);
	index_array_ = group_.add(gpu::Kind::IndexArray,
			rd->index_array_create(index_buffer, 0, ve::kLodQuadsPerPage * 6));
	index_array_buffer_ = index_buffer;
	return index_array_.is_valid();
}

bool LodRasterPass::prepare_index_array(RenderingDevice *rd, LodPool &pool) {
	return ensure_index_array(rd, pool);
}

bool LodRasterPass::clear_targets(RenderingDevice *rd, GBuffer &gb, RID marker) {
	if (!gb.is_valid()) return false;
	if (!ensure_pipeline(rd, gb, marker)) return false;
	PackedColorArray clears;
	clears.push_back(Color(0, 0, 0, 0)); // albedo
	clears.push_back(Color(0, 0, 0, 0)); // surface
	if (marker.is_valid()) clears.push_back(Color(0, 0, 0, 0));
	// 0.0 is the reverse-Z far plane, the same value CompositePass clears depth to.
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(),
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH,
			clears, 0.0f);
	if (dl < 0) return false;
	rd->draw_list_end();
	return true;
}

bool LodRasterPass::draw(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials,
		GBuffer &gb, const Projection &view_proj, const float cam_pos[3],
		int draw_count, float fade_start, float fade_end, RID marker) {
	const auto t0 = std::chrono::steady_clock::now();
	const RID shader = marker.is_valid() ? shader_marker_ : shader_;
	if (!shader.is_valid() || !gb.is_valid()) return false;
	if (draw_count <= 0 || draw_count > static_cast<int>(draw_pages_.size())) return false;
	if (!ensure_pipeline(rd, gb, marker)) return false;
	if (!ensure_uniform_set(rd, pool, materials, shader)) return false;
	if (!ensure_index_array(rd, pool)) return false;

	// The indirect args were uploaded by LodPool::upload_draw_args before the cull pass ran;
	// draw() only opens the draw list and issues the indirect draw.
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	// RenderingDevice returns an invalid list when the framebuffer or device is no longer
	// recordable. Do not issue bindings against it: the compositor treats false as a
	// fail-soft skipped LoD pass and leaves the existing G-buffer intact.
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, active_pipeline());
	rd->draw_list_bind_uniform_set(dl, set_.id(), 0);
	rd->draw_list_bind_index_array(dl, index_array_);
	ve::LodRasterPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			push.view_proj[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
	push.cam[0] = cam_pos[0];
	push.cam[1] = cam_pos[1];
	push.cam[2] = cam_pos[2];
	push.cam[3] = fade_start;
	push.fade[0] = fade_end;
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	rd->draw_list_draw_indirect(dl, true, pool.args_buffer(), 0, draw_count, 20);
	rd->draw_list_end();
	last_ms_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return true;
}
