#include "render/lod_raster_pass.h"
#include "render/lod_pool.h"
#include "render/material_atlas.h"
#include "render/shader_loader.h"
#include "lod/lod_contour.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

LodRasterPass::~LodRasterPass() {
	teardown();
}

void LodRasterPass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	auto load_stage = [&](const char *file, RenderingDevice::ShaderStage stage, Ref<RDShaderSource> &src) -> bool {
		std::string err;
		const String path = ProjectSettings::get_singleton()->globalize_path(String("res://shaders/") + file);
		const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
		const std::string code = ve::strip_shader_annotations(
				ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
		if (code.empty()) {
			UtilityFunctions::printerr("LodRasterPass: ", err.c_str());
			return false;
		}
		src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		src->set_stage_source(stage, String(code.c_str()));
		return true;
	};
	Ref<RDShaderSource> src;
	src.instantiate();
	if (!load_stage("lod.vert.glsl", RenderingDevice::SHADER_STAGE_VERTEX, src)) return;
	if (!load_stage("lod.frag.glsl", RenderingDevice::SHADER_STAGE_FRAGMENT, src)) return;
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX) +
			spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("LodRasterPass: ", compile_err);
		return;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
}

void LodRasterPass::teardown() {
	if (!rd_) return;
	// Same cascade order as CompositePass: uniform set references the shader, and freeing
	// the shader tears down its pipelines, so free the set first, then pipelines/shader.
	for (RID *r : {&uset_, &index_array_, &pipeline_cull_off_, &pipeline_cull_ccw_,
				&pipeline_cull_cw_, &shader_, &framebuffer_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	uset_quads_ = RID();
	uset_page_chunk_ = RID();
	uset_chunks_ = RID();
	uset_albedo_ = RID();
	uset_surface_ = RID();
	uset_sampler_ = RID();
	index_array_buffer_ = RID();
	draw_pages_.clear();
	rd_ = nullptr;
}

void LodRasterPass::release_targets() {
	if (rd_ && framebuffer_.is_valid()) rd_->free_rid(framebuffer_);
	framebuffer_ = RID();
	fb_color_ = RID();
	fb_depth_ = RID();
}

void LodRasterPass::set_draw_pages(const std::vector<PageDraw> &pages) {
	draw_pages_ = pages;
}

bool LodRasterPass::ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth) {
	if (pipeline_cull_off_.is_valid() && pipeline_cull_ccw_.is_valid() &&
			pipeline_cull_cw_.is_valid() && framebuffer_.is_valid() &&
			dst_color == fb_color_ && dst_depth == fb_depth_) {
		return true;
	}
	if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
	framebuffer_ = rd->framebuffer_create(Array::make(dst_color, dst_depth));
	fb_color_ = dst_color;
	fb_depth_ = dst_depth;
	if (!framebuffer_.is_valid()) return false;
	fb_format_ = rd->framebuffer_get_format(framebuffer_);

	if (!pipeline_cull_off_.is_valid() || !pipeline_cull_ccw_.is_valid() ||
			!pipeline_cull_cw_.is_valid()) {
		auto make_pipeline = [&](RenderingDevice::PolygonCullMode cull,
				RenderingDevice::PolygonFrontFace front) -> RID {
			Ref<RDPipelineRasterizationState> rs;
			rs.instantiate();
			rs->set_cull_mode(cull);
			rs->set_front_face(front);
			Ref<RDPipelineMultisampleState> ms;
			ms.instantiate();
			Ref<RDPipelineDepthStencilState> ds;
			ds.instantiate();
			ds->set_enable_depth_test(true);
			ds->set_enable_depth_write(true);
			// Reverse-Z (M1 errata 2): near = 1, far = 0. GREATER_OR_EQUAL both writes our
			// depth where it is nearer than the current buffer and leaves nearer geometry
			// untouched.
			ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
			Ref<RDPipelineColorBlendStateAttachment> att;
			att.instantiate();
			att->set_enable_blend(false);
			Ref<RDPipelineColorBlendState> cb;
			cb.instantiate();
			cb->set_attachments(Array::make(att));
			// Pull-only pipeline: no vertex array, so the vertex format must be INVALID_ID
			// (an empty vertex format is valid but expects vertices and ERR_FAILs).
			return rd->render_pipeline_create(shader_, fb_format_, RenderingDevice::INVALID_ID,
					RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
		};
		pipeline_cull_off_ = make_pipeline(RenderingDevice::POLYGON_CULL_DISABLED,
				RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE);
		pipeline_cull_ccw_ = make_pipeline(RenderingDevice::POLYGON_CULL_BACK,
				RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE);
		pipeline_cull_cw_ = make_pipeline(RenderingDevice::POLYGON_CULL_BACK,
				RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE);
	}
	return pipeline_cull_off_.is_valid() && pipeline_cull_ccw_.is_valid() &&
			pipeline_cull_cw_.is_valid();
}

bool LodRasterPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials) {
	const RID quads = pool.quad_buffer();
	const RID page_chunk = pool.page_chunk_buffer();
	const RID chunks = pool.chunk_buffer();
	const RID albedo = materials.albedo_array();
	const RID surface = materials.surface_array();
	const RID sampler = materials.sampler();
	if (uset_.is_valid() && quads == uset_quads_ && page_chunk == uset_page_chunk_ &&
			chunks == uset_chunks_ && albedo == uset_albedo_ && surface == uset_surface_ &&
			sampler == uset_sampler_) {
		return true;
	}
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u0->set_binding(0);
	u0->add_id(quads);
	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u1->set_binding(1);
	u1->add_id(page_chunk);
	Ref<RDUniform> u2;
	u2.instantiate();
	u2->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u2->set_binding(2);
	u2->add_id(chunks);
	Ref<RDUniform> u3;
	u3.instantiate();
	u3->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u3->set_binding(3);
	u3->add_id(sampler);
	u3->add_id(albedo);
	Ref<RDUniform> u4;
	u4.instantiate();
	u4->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u4->set_binding(4);
	u4->add_id(sampler);
	u4->add_id(surface);
	uset_ = rd->uniform_set_create(Array::make(u0, u1, u2, u3, u4), shader_, 0);
	if (uset_.is_valid()) {
		uset_quads_ = quads;
		uset_page_chunk_ = page_chunk;
		uset_chunks_ = chunks;
		uset_albedo_ = albedo;
		uset_surface_ = surface;
		uset_sampler_ = sampler;
	}
	return uset_.is_valid();
}

RID LodRasterPass::active_pipeline() const {
	if (!cull_enabled_) return pipeline_cull_off_;
	return front_face_clockwise_ ? pipeline_cull_cw_ : pipeline_cull_ccw_;
}

bool LodRasterPass::ensure_index_array(RenderingDevice *rd, LodPool &pool) {
	const RID index_buffer = pool.index_buffer();
	if (index_array_.is_valid() && index_buffer == index_array_buffer_) return true;
	if (index_array_.is_valid()) rd->free_rid(index_array_);
	index_array_ = RID();
	index_array_ = rd->index_array_create(index_buffer, 0, ve::kLodQuadsPerPage * 6);
	index_array_buffer_ = index_buffer;
	return index_array_.is_valid();
}

bool LodRasterPass::draw(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials,
		RID dst_color, RID dst_depth, const Projection &view_proj, const float cam_pos[3],
		int draw_count) {
	if (!shader_.is_valid()) return false;
	if (draw_count <= 0 || draw_count > static_cast<int>(draw_pages_.size())) return false;
	if (!ensure_pipeline(rd, dst_color, dst_depth)) return false;
	if (!ensure_uniform_set(rd, pool, materials)) return false;
	if (!ensure_index_array(rd, pool)) return false;

	// Indirect args are built on the CPU (Task 15's cull will only zero instanceCount).
	// Record before the draw list opens: buffer_update is a device-level command.
	PackedByteArray args;
	args.resize(static_cast<int64_t>(draw_count) * 20);
	uint32_t *a = reinterpret_cast<uint32_t *>(args.ptrw());
	for (int i = 0; i < draw_count; i++) {
		const PageDraw &pd = draw_pages_[static_cast<size_t>(i)];
		a[i * 5 + 0] = static_cast<uint32_t>(pd.quad_count * 6);
		a[i * 5 + 1] = 1u; // instanceCount; the cull pass zeroes this to remove
		a[i * 5 + 2] = 0u; // firstIndex: each page starts at index 0 in the shared buffer
		a[i * 5 + 3] = static_cast<uint32_t>(pd.page * ve::kLodVertsPerPage);
		a[i * 5 + 4] = 0u; // firstInstance
	}
	rd->buffer_update(pool.args_buffer(), 0, args.size(), args);

	const int64_t dl = rd->draw_list_begin(framebuffer_, RenderingDevice::DRAW_DEFAULT_ALL);
	rd->draw_list_bind_render_pipeline(dl, active_pipeline());
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	rd->draw_list_bind_index_array(dl, index_array_);
	PackedByteArray pc;
	pc.resize(80);
	{
		float *f = reinterpret_cast<float *>(pc.ptrw());
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				f[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
		// std430 push block: mat4 view_proj occupies floats 0..15 (bytes 0..63), so the
		// vec4 cam that follows starts at float 16, NOT float 64. Indexing by the byte
		// offset wrote 176 bytes past the end of this 80-byte array and corrupted the
		// heap, which surfaced as an abort in a later free (glibc "corrupted size vs.
		// prev_size") or a segfault inside vkDestroyDevice at teardown.
		f[16] = cam_pos[0];
		f[17] = cam_pos[1];
		f[18] = cam_pos[2];
		f[19] = 0.0f;
	}
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	rd->draw_list_draw_indirect(dl, true, pool.args_buffer(), 0, draw_count, 20);
	rd->draw_list_end();
	return true;
}
