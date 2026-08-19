#include "render/lod_raster_pass.h"
#include "render/gbuffer.h"
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
#include <chrono>
#include <cstring>

using namespace godot;

LodRasterPass::~LodRasterPass() {
	teardown();
}

void LodRasterPass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	auto load_stage = [&](const char *file, RenderingDevice::ShaderStage stage,
			Ref<RDShaderSource> &src, bool marker) -> bool {
		std::string err;
		const String path = ProjectSettings::get_singleton()->globalize_path(String("res://shaders/") + file);
		const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
		std::string code = ve::strip_shader_annotations(
				ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
		if (code.empty()) {
			UtilityFunctions::printerr("LodRasterPass: ", err.c_str());
			return false;
		}
		if (marker) {
			// Same production/debug split as CompositePass: the marker output is compiled
			// only for the seam-probe shader variant, so production pipelines keep exactly
			// one fragment output and match the scene framebuffer's color mask.
			const size_t version_end = code.find('\n', code.find("#version"));
			if (version_end != std::string::npos)
				code.insert(version_end + 1, "#define SEAM_MARKER 1\n");
		}
		src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		src->set_stage_source(stage, String(code.c_str()));
		return true;
	};
	auto make_shader = [&](bool marker) -> RID {
		Ref<RDShaderSource> src;
		src.instantiate();
		if (!load_stage("lod.vert.glsl", RenderingDevice::SHADER_STAGE_VERTEX, src, marker)) return RID();
		if (!load_stage("lod.frag.glsl", RenderingDevice::SHADER_STAGE_FRAGMENT, src, marker)) return RID();
		Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
		const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX) +
				spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
		if (!compile_err.is_empty()) {
			UtilityFunctions::printerr("LodRasterPass: ", compile_err);
			return RID();
		}
		return rd->shader_create_from_spirv(spirv);
	};
	shader_ = make_shader(false);
	shader_marker_ = make_shader(true);
}

void LodRasterPass::teardown() {
	if (!rd_) return;
	// Same cascade order as CompositePass: uniform set references the shader, and freeing
	// the shader tears down its pipelines, so free the set first, then pipelines/shader.
	for (RID *r : {&uset_, &index_array_, &pipeline_cull_off_, &pipeline_cull_ccw_,
				&pipeline_cull_cw_, &shader_, &shader_marker_, &framebuffer_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	uset_shader_ = RID();
	uset_quads_ = RID();
	uset_page_chunk_ = RID();
	uset_chunks_ = RID();
	uset_albedo_ = RID();
	uset_surface_ = RID();
	uset_sampler_ = RID();
	index_array_buffer_ = RID();
	fb_albedo_ = RID();
	fb_surface_ = RID();
	fb_depth_ = RID();
	fb_marker_ = RID();
	draw_pages_.clear();
	rd_ = nullptr;
}

void LodRasterPass::release_targets() {
	if (rd_ && framebuffer_.is_valid()) rd_->free_rid(framebuffer_);
	framebuffer_ = RID();
	fb_albedo_ = RID();
	fb_surface_ = RID();
	fb_depth_ = RID();
	fb_marker_ = RID();
}

void LodRasterPass::set_draw_pages(const std::vector<PageDraw> &pages) {
	draw_pages_ = pages;
}

bool LodRasterPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb, RID marker) {
	const RID albedo = gb.albedo();
	const RID surface = gb.surface();
	const RID depth = gb.depth();
	const bool want_marker = marker.is_valid();
	const RID shader = want_marker ? shader_marker_ : shader_;
	if (!shader.is_valid()) return false;
	if (pipeline_cull_off_.is_valid() && pipeline_cull_ccw_.is_valid() &&
			pipeline_cull_cw_.is_valid() && framebuffer_.is_valid() &&
			albedo == fb_albedo_ && surface == fb_surface_ && depth == fb_depth_ &&
			marker == fb_marker_ && pipeline_marker_ == want_marker) {
		return true;
	}
	if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
	const Array attachments = want_marker ?
			Array::make(albedo, surface, marker, depth) : Array::make(albedo, surface, depth);
	framebuffer_ = rd->framebuffer_create(attachments);
	fb_albedo_ = albedo;
	fb_surface_ = surface;
	fb_depth_ = depth;
	fb_marker_ = marker;
	if (!framebuffer_.is_valid()) return false;
	fb_format_ = rd->framebuffer_get_format(framebuffer_);

	if (!pipeline_cull_off_.is_valid() || !pipeline_cull_ccw_.is_valid() ||
			!pipeline_cull_cw_.is_valid() || pipeline_marker_ != want_marker) {
		if (pipeline_cull_off_.is_valid()) rd->free_rid(pipeline_cull_off_);
		if (pipeline_cull_ccw_.is_valid()) rd->free_rid(pipeline_cull_ccw_);
		if (pipeline_cull_cw_.is_valid()) rd->free_rid(pipeline_cull_cw_);
		pipeline_cull_off_ = RID();
		pipeline_cull_ccw_ = RID();
		pipeline_cull_cw_ = RID();
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
			Ref<RDPipelineColorBlendStateAttachment> att_albedo, att_surface;
			att_albedo.instantiate();
			att_surface.instantiate();
			att_albedo->set_enable_blend(false);
			att_surface->set_enable_blend(false);
			Ref<RDPipelineColorBlendState> cb;
			cb.instantiate();
			if (want_marker) {
				Ref<RDPipelineColorBlendStateAttachment> att_marker;
				att_marker.instantiate();
				att_marker->set_enable_blend(false);
				cb->set_attachments(Array::make(att_albedo, att_surface, att_marker));
				// Debug seam probe: the LoD marker (2) must OR into the composite marker
				// (1) so double-claimed band pixels read 3. This is debug-only; production
				// pipelines have no marker attachment and keep blending disabled.
				cb->set_enable_logic_op(true);
				cb->set_logic_op(RenderingDevice::LOGIC_OP_OR);
			} else {
				cb->set_attachments(Array::make(att_albedo, att_surface));
			}
			// Pull-only pipeline: no vertex array, so the vertex format must be INVALID_ID
			// (an empty vertex format is valid but expects vertices and ERR_FAILs).
			return rd->render_pipeline_create(shader, fb_format_, RenderingDevice::INVALID_ID,
					RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
		};
		pipeline_cull_off_ = make_pipeline(RenderingDevice::POLYGON_CULL_DISABLED,
				RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE);
		pipeline_cull_ccw_ = make_pipeline(RenderingDevice::POLYGON_CULL_BACK,
				RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE);
		pipeline_cull_cw_ = make_pipeline(RenderingDevice::POLYGON_CULL_BACK,
				RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE);
		pipeline_marker_ = want_marker;
	}
	return pipeline_cull_off_.is_valid() && pipeline_cull_ccw_.is_valid() &&
			pipeline_cull_cw_.is_valid();
}

bool LodRasterPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials,
		RID shader) {
	const RID quads = pool.quad_buffer();
	const RID page_chunk = pool.page_chunk_buffer();
	const RID chunks = pool.chunk_buffer();
	const RID albedo = materials.albedo_array();
	const RID surface = materials.surface_array();
	const RID sampler = materials.sampler();
	if (uset_.is_valid() && uset_shader_ == shader &&
			quads == uset_quads_ && page_chunk == uset_page_chunk_ &&
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
	uset_ = rd->uniform_set_create(Array::make(u0, u1, u2, u3, u4), shader, 0);
	if (uset_.is_valid()) {
		uset_shader_ = shader;
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
	const int64_t dl = rd->draw_list_begin(framebuffer_, RenderingDevice::DRAW_DEFAULT_ALL);
	rd->draw_list_bind_render_pipeline(dl, active_pipeline());
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	rd->draw_list_bind_index_array(dl, index_array_);
	PackedByteArray pc;
	pc.resize(96);
	{
		float *f = reinterpret_cast<float *>(pc.ptrw());
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				f[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
		// std430 push block: mat4 view_proj occupies floats 0..15 (bytes 0..63), so the
		// vec4 cam that follows starts at float 16 and vec4 fade at float 20. Index by
		// float, not byte (plan errata 3): byte indexing wrote past the end of the array
		// and corrupted the heap in earlier tasks.
		f[16] = cam_pos[0];
		f[17] = cam_pos[1];
		f[18] = cam_pos[2];
		f[19] = fade_start;
		f[20] = fade_end;
	}
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	rd->draw_list_draw_indirect(dl, true, pool.args_buffer(), 0, draw_count, 20);
	rd->draw_list_end();
	last_ms_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return true;
}
