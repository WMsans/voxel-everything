#include "render/grass_raster_pass.h"
#include "render/gbuffer.h"
#include "render/grass_scatter_pass.h"
#include "render/shader_loader.h"
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

using namespace godot;

GrassRasterPass::~GrassRasterPass() {
	teardown();
}

void GrassRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	auto load_stage = [&](const char *file, RenderingDevice::ShaderStage stage,
			Ref<RDShaderSource> &src) -> bool {
		std::string err;
		const String path = ProjectSettings::get_singleton()->globalize_path(String("res://shaders/") + file);
		const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
		std::string code = ve::strip_shader_annotations(
				ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
		if (code.empty()) {
			UtilityFunctions::printerr("GrassRasterPass: ", err.c_str());
			return false;
		}
		src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		src->set_stage_source(stage, String(code.c_str()));
		return true;
	};
	Ref<RDShaderSource> src;
	src.instantiate();
	if (!load_stage("grass.vert.glsl", RenderingDevice::SHADER_STAGE_VERTEX, src)) {
		teardown();
		return;
	}
	if (!load_stage("grass.frag.glsl", RenderingDevice::SHADER_STAGE_FRAGMENT, src)) {
		teardown();
		return;
	}
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX) +
			spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("GrassRasterPass: ", compile_err);
		teardown();
		return;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	if (!shader_.is_valid()) {
		UtilityFunctions::printerr("GrassRasterPass: shader creation failed");
		teardown();
	}
}

void GrassRasterPass::teardown() {
	if (!rd_) return;
	// Same cascade order as LodRasterPass: the uniform set references the shader, and
	// freeing the shader tears down its pipelines, so free the set first, then
	// pipeline/shader.
	for (RID *r : {&uset_, &pipeline_, &shader_, &framebuffer_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	uset_shader_ = RID();
	uset_instances_ = RID();
	uset_params_ = RID();
	fb_albedo_ = RID();
	fb_surface_ = RID();
	fb_depth_ = RID();
	last_vertex_count_ = 0;
	rd_ = nullptr;
}

void GrassRasterPass::release_targets() {
	if (rd_ && framebuffer_.is_valid()) rd_->free_rid(framebuffer_);
	framebuffer_ = RID();
	fb_albedo_ = RID();
	fb_surface_ = RID();
	fb_depth_ = RID();
}

bool GrassRasterPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb) {
	const RID albedo = gb.albedo();
	const RID surface = gb.surface();
	const RID depth = gb.depth();
	if (!shader_.is_valid()) return false;
	if (pipeline_.is_valid() && framebuffer_.is_valid() &&
			albedo == fb_albedo_ && surface == fb_surface_ && depth == fb_depth_) {
		return true;
	}
	if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
	// No marker attachment: blades write exactly the two colour channels the far field
	// writes, plus depth.
	const Array attachments = Array::make(albedo, surface, depth);
	framebuffer_ = rd->framebuffer_create(attachments);
	fb_albedo_ = albedo;
	fb_surface_ = surface;
	fb_depth_ = depth;
	if (!framebuffer_.is_valid()) return false;
	fb_format_ = rd->framebuffer_get_format(framebuffer_);

	if (!pipeline_.is_valid()) {
		Ref<RDPipelineRasterizationState> rs;
		rs.instantiate();
		// Blades are single triangles seen from both sides.
		rs->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
		rs->set_front_face(RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE);
		Ref<RDPipelineMultisampleState> ms;
		ms.instantiate();
		Ref<RDPipelineDepthStencilState> ds;
		ds.instantiate();
		ds->set_enable_depth_test(true);
		ds->set_enable_depth_write(true);
		// Reverse-Z (M1 errata 2): near = 1, far = 0. GREATER_OR_EQUAL both writes our
		// depth where it is nearer than the current buffer and leaves nearer geometry
		// untouched -- the same compare the far field uses, so blades occlude correctly.
		ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
		Ref<RDPipelineColorBlendStateAttachment> att_albedo, att_surface;
		att_albedo.instantiate();
		att_surface.instantiate();
		att_albedo->set_enable_blend(false);
		att_surface->set_enable_blend(false);
		Ref<RDPipelineColorBlendState> cb;
		cb.instantiate();
		cb->set_attachments(Array::make(att_albedo, att_surface));
		// Pull-only pipeline: no vertex array, so the vertex format must be INVALID_ID
		// (an empty vertex format is valid but expects vertices and ERR_FAILs).
		pipeline_ = rd->render_pipeline_create(shader_, fb_format_, RenderingDevice::INVALID_ID,
				RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
	}
	return pipeline_.is_valid();
}

bool GrassRasterPass::ensure_uniform_set(RenderingDevice *rd, GrassScatterPass &scatter) {
	const RID instances = scatter.instance_buffer();
	const RID params = scatter.params_buffer();
	if (uset_.is_valid() && uset_shader_ == shader_ &&
			instances == uset_instances_ && params == uset_params_) {
		return true;
	}
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u0->set_binding(0);
	u0->add_id(instances);
	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u1->set_binding(1);
	u1->add_id(params);
	uset_ = rd->uniform_set_create(Array::make(u0, u1), shader_, 0);
	if (uset_.is_valid()) {
		uset_shader_ = shader_;
		uset_instances_ = instances;
		uset_params_ = params;
	}
	return uset_.is_valid();
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

	const int64_t dl = rd->draw_list_begin(framebuffer_, RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	PackedByteArray pc;
	pc.resize(80);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) f[c * 4 + r] = view_proj.columns[c][r];
	f[16] = cam_pos[0];
	f[17] = cam_pos[1];
	f[18] = cam_pos[2];
	f[19] = 0.0f;
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	// One non-indexed indirect draw; the vertex count is whatever the scatter wrote.
	rd->draw_list_draw_indirect(dl, false, args, 0, 1, 16);
	rd->draw_list_end();
	last_vertex_count_ = scatter.last_blade_count() * 3;
	return true;
}
