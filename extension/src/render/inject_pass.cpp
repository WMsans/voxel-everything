#include "render/inject_pass.h"
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
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

InjectPass::~InjectPass() {
	teardown();
}

void InjectPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	Ref<RDShaderSource> src;
	src.instantiate();
	auto load_stage = [&](const char *file, RenderingDevice::ShaderStage stage) -> bool {
		std::string err;
		const String path = ProjectSettings::get_singleton()->globalize_path(String("res://shaders/") + file);
		const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
		const std::string code = ve::strip_shader_annotations(
				ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
		if (code.empty()) {
			UtilityFunctions::printerr("InjectPass: shader load failed: ", err.c_str());
			return false;
		}
		src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		src->set_stage_source(stage, String(code.c_str()));
		return true;
	};
	if (!load_stage("inject.vert.glsl", RenderingDevice::SHADER_STAGE_VERTEX) ||
			!load_stage("inject.frag.glsl", RenderingDevice::SHADER_STAGE_FRAGMENT)) return;
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX) +
			spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("InjectPass: ", compile_err);
		return;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
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

void InjectPass::release_targets() {
	if (rd_ && framebuffer_.is_valid()) rd_->free_rid(framebuffer_);
	framebuffer_ = RID();
	fb_color_ = RID();
	fb_depth_ = RID();
}

void InjectPass::teardown() {
	if (!rd_) return;
	for (RID *r : {&uset_, &pipeline_, &shader_, &framebuffer_, &sampler_linear_, &sampler_nearest_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	fb_color_ = RID();
	fb_depth_ = RID();
	uset_lit_ = RID();
	uset_depth_ = RID();
	rd_ = nullptr;
}

bool InjectPass::ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth) {
	if (!shader_.is_valid()) return false;
	if (pipeline_.is_valid() && framebuffer_.is_valid() && dst_color == fb_color_ &&
			dst_depth == fb_depth_) return true;
	if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
	framebuffer_ = rd->framebuffer_create(Array::make(dst_color, dst_depth));
	fb_color_ = dst_color;
	fb_depth_ = dst_depth;
	if (!framebuffer_.is_valid()) return false;
	fb_format_ = rd->framebuffer_get_format(framebuffer_);
	if (!pipeline_.is_valid()) {
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
		Ref<RDPipelineColorBlendStateAttachment> att;
		att.instantiate();
		att->set_enable_blend(false);
		Ref<RDPipelineColorBlendState> cb;
		cb.instantiate();
		cb->set_attachments(Array::make(att));
		pipeline_ = rd->render_pipeline_create(shader_, fb_format_, RenderingDevice::INVALID_ID,
				RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
	}
	return pipeline_.is_valid() && framebuffer_.is_valid();
}

bool InjectPass::draw(RenderingDevice *rd, RID dst_color, RID dst_depth, RID lit, RID gb_depth) {
	if (!ensure_pipeline(rd, dst_color, dst_depth)) return false;
	if (!(uset_.is_valid() && uset_lit_ == lit && uset_depth_ == gb_depth)) {
		if (uset_.is_valid()) rd->free_rid(uset_);
		Ref<RDUniform> u0, u1;
		u0.instantiate();
		u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u0->set_binding(0);
		u0->add_id(sampler_linear_);
		u0->add_id(lit);
		u1.instantiate();
		u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u1->set_binding(1);
		u1->add_id(sampler_nearest_);
		u1->add_id(gb_depth);
		uset_ = rd->uniform_set_create(Array::make(u0, u1), shader_, 0);
		uset_lit_ = lit;
		uset_depth_ = gb_depth;
	}
	if (!uset_.is_valid()) return false;
	const int64_t dl = rd->draw_list_begin(framebuffer_, RenderingDevice::DRAW_DEFAULT_ALL);
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	rd->draw_list_draw(dl, false, 1, 3);
	rd->draw_list_end();
	return true;
}
