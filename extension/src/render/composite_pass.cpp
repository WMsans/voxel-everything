#include "render/composite_pass.h"
#include "render/gbuffer.h"
#include "render/material_atlas.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

CompositePass::~CompositePass() {
	teardown();
}

void CompositePass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	auto load_stage = [&](const char *file, RenderingDevice::ShaderStage stage,
			Ref<RDShaderSource> &src, bool marker) -> bool {
		std::string err;
		const String path = ProjectSettings::get_singleton()->globalize_path(String("res://shaders/") + file);
		const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
		std::string code = ve::strip_shader_annotations(
				ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
		if (code.empty()) {
			UtilityFunctions::printerr("CompositePass: ", err.c_str());
			return false;
		}
		if (marker) {
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
		if (!load_stage("composite.vert.glsl", RenderingDevice::SHADER_STAGE_VERTEX, src, marker)) return RID();
		if (!load_stage("composite.frag.glsl", RenderingDevice::SHADER_STAGE_FRAGMENT, src, marker)) return RID();
		Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
		const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX) +
				spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
		if (!compile_err.is_empty()) {
			UtilityFunctions::printerr("CompositePass: ", compile_err);
			return RID();
		}
		return rd->shader_create_from_spirv(spirv);
	};
	shader_ = make_shader(false);
	shader_marker_ = make_shader(true);

	Ref<RDSamplerState> sl;
	sl.instantiate();
	sl->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sl->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_linear_ = rd->sampler_create(sl);
	Ref<RDSamplerState> sn;
	sn.instantiate();
	sn->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sn->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_nearest_ = rd->sampler_create(sn);
}

void CompositePass::release_targets() {
	if (rd_ && framebuffer_.is_valid()) rd_->free_rid(framebuffer_);
	framebuffer_ = RID();
	fb_albedo_ = RID();
	fb_surface_ = RID();
	fb_depth_ = RID();
	fb_marker_ = RID();
}

void CompositePass::invalidate_uniform_set(RenderingDevice *rd) {
	if (rd && uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	uset_shader_ = RID();
	uset_src_albedo_ = RID();
	uset_src_surface_ = RID();
	uset_src_hitpos_ = RID();
	uset_material_albedo_ = RID();
	uset_material_surface_ = RID();
	uset_material_sampler_ = RID();
}

void CompositePass::teardown() {
	if (!rd_) return;
	for (RID *r : {&uset_, &pipeline_, &shader_, &shader_marker_,
			&sampler_linear_, &sampler_nearest_, &framebuffer_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	uset_shader_ = RID();
	uset_src_albedo_ = RID();
	uset_src_surface_ = RID();
	uset_src_hitpos_ = RID();
	uset_material_albedo_ = RID();
	uset_material_surface_ = RID();
	uset_material_sampler_ = RID();
	fb_albedo_ = RID();
	fb_surface_ = RID();
	fb_depth_ = RID();
	fb_marker_ = RID();
	rd_ = nullptr;
}

bool CompositePass::ensure_pipeline(RenderingDevice *rd, RID albedo, RID surface, RID depth,
		RID marker) {
	const bool want_marker = marker.is_valid();
	const RID shader = want_marker ? shader_marker_ : shader_;
	if (!shader.is_valid()) return false;
	if (pipeline_.is_valid() && framebuffer_.is_valid() &&
			albedo == fb_albedo_ && surface == fb_surface_ && depth == fb_depth_ &&
			marker == fb_marker_ && pipeline_marker_ == want_marker) {
		return true;
	}
	if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
	const Array attachments = want_marker
			? Array::make(albedo, surface, marker, depth)
			: Array::make(albedo, surface, depth);
	framebuffer_ = rd->framebuffer_create(attachments);
	fb_albedo_ = albedo;
	fb_surface_ = surface;
	fb_depth_ = depth;
	fb_marker_ = marker;
	if (!framebuffer_.is_valid()) return false;
	fb_format_ = rd->framebuffer_get_format(framebuffer_);

	if (!pipeline_.is_valid() || pipeline_marker_ != want_marker) {
		if (pipeline_.is_valid()) rd->free_rid(pipeline_);
		pipeline_ = RID();
		Ref<RDPipelineRasterizationState> rs;
		rs.instantiate();
		rs->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
		Ref<RDPipelineMultisampleState> ms;
		ms.instantiate();
		Ref<RDPipelineDepthStencilState> ds;
		ds.instantiate();
		ds->set_enable_depth_test(true);
		ds->set_enable_depth_write(true);
		ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
		Ref<RDPipelineColorBlendStateAttachment> att_a, att_s;
		att_a.instantiate();
		att_s.instantiate();
		att_a->set_enable_blend(false);
		att_s->set_enable_blend(false);
		Ref<RDPipelineColorBlendState> cb;
		cb.instantiate();
		if (want_marker) {
			Ref<RDPipelineColorBlendStateAttachment> att_m;
			att_m.instantiate();
			att_m->set_enable_blend(false);
			cb->set_attachments(Array::make(att_a, att_s, att_m));
		} else {
			cb->set_attachments(Array::make(att_a, att_s));
		}
		pipeline_marker_ = want_marker;
		pipeline_ = rd->render_pipeline_create(shader, fb_format_, RenderingDevice::INVALID_ID,
				RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
	}
	return pipeline_.is_valid() && framebuffer_.is_valid();
}

void CompositePass::draw(RenderingDevice *rd, GBuffer &gb, RID src_albedo, RID src_surface,
		RID src_hitpos, const Projection &view_proj, const MaterialAtlas &materials,
		const float cam_pos[3], float fade_start, float fade_end, RID marker) {
	last_draw_ok_ = false;
	const RID shader = marker.is_valid() ? shader_marker_ : shader_;
	if (!shader.is_valid() || !gb.is_valid()) return;
	if (!ensure_pipeline(rd, gb.albedo(), gb.surface(), gb.depth(), marker)) return;

	Ref<RDUniform> u0, u1, u2, u3, u4;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u0->set_binding(0);
	u0->add_id(sampler_linear_);
	u0->add_id(src_albedo);
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u1->set_binding(1);
	u1->add_id(sampler_nearest_);
	u1->add_id(src_hitpos);
	u2.instantiate();
	u2->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u2->set_binding(2);
	u2->add_id(materials.sampler());
	u2->add_id(materials.albedo_array());
	u3.instantiate();
	u3->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u3->set_binding(3);
	u3->add_id(materials.sampler());
	u3->add_id(materials.surface_array());
	u4.instantiate();
	u4->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u4->set_binding(4);
	u4->add_id(sampler_nearest_);
	u4->add_id(src_surface);

	if (!(uset_.is_valid() && uset_shader_ == shader && src_albedo == uset_src_albedo_ &&
			src_surface == uset_src_surface_ && src_hitpos == uset_src_hitpos_ &&
			materials.albedo_array() == uset_material_albedo_ &&
			materials.surface_array() == uset_material_surface_ &&
			materials.sampler() == uset_material_sampler_)) {
		if (uset_.is_valid()) rd->free_rid(uset_);
		uset_ = rd->uniform_set_create(Array::make(u0, u1, u2, u3, u4), shader, 0);
		uset_shader_ = shader;
		uset_src_albedo_ = src_albedo;
		uset_src_surface_ = src_surface;
		uset_src_hitpos_ = src_hitpos;
		uset_material_albedo_ = materials.albedo_array();
		uset_material_surface_ = materials.surface_array();
		uset_material_sampler_ = materials.sampler();
	}
	if (!uset_.is_valid()) return;

	PackedByteArray pc;
	pc.resize(96);
	{
		float *f = reinterpret_cast<float *>(pc.ptrw());
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				f[c * 4 + r] = view_proj.columns[c][r];
		f[16] = cam_pos[0];
		f[17] = cam_pos[1];
		f[18] = cam_pos[2];
		f[19] = fade_start;
		f[20] = fade_end;
	}

	PackedColorArray clears;
	clears.push_back(Color(0, 0, 0, 0));
	clears.push_back(Color(0, 0, 0, 0));
	if (marker.is_valid()) clears.push_back(Color(0, 0, 0, 0));
	const int64_t dl = rd->draw_list_begin(framebuffer_,
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH,
			clears, 0.0f);
	if (dl < 0) return;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	rd->draw_list_draw(dl, false, 1, 3);
	rd->draw_list_end();
	last_draw_ok_ = true;
}
