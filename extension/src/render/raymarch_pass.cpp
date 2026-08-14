#include "render/raymarch_pass.h"
#include "render/gpu_world.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

RaymarchPass::~RaymarchPass() {
	teardown();
}

void RaymarchPass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	std::string err;
	const String path = ProjectSettings::get_singleton()->globalize_path("res://shaders/raymarch.comp.glsl");
	const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("RaymarchPass: shader load failed: ", err.c_str());
		return;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("RaymarchPass: ", compile_err);
		return;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	pipeline_ = rd->compute_pipeline_create(shader_);

	Ref<RDSamplerState> ss;
	ss.instantiate();
	ss->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	ss->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_ = rd->sampler_create(ss);
}

void RaymarchPass::teardown() {
	if (!rd_) return;
	// Free order matters on Godot 4.7.1's RenderingDevice: freeing a texture (or shader)
	// cascades to referencing uniform sets, and freeing a shader also tears down its
	// pipelines — so uset_ first, then pipeline_ before shader_, then the targets.
	for (RID *r : {&uset_, &pipeline_, &shader_, &color_, &hitpos_, &sampler_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	rd_ = nullptr;
}

RID RaymarchPass::make_target(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h) {
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_format(fmt);
	f->set_width(w);
	f->set_height(h);
	f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	Ref<RDTextureView> v;
	v.instantiate();
	return rd->texture_create(f, v, {});
}

void RaymarchPass::rebuild_targets(RenderingDevice *rd, const GpuWorld &world, int w, int h) {
	// Old uniform set references the old color/hitpos textures: free it before them.
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	if (color_.is_valid()) rd->free_rid(color_);
	if (hitpos_.is_valid()) rd->free_rid(hitpos_);
	color_ = make_target(rd, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, w, h);
	hitpos_ = make_target(rd, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, w, h);
	width_ = w;
	height_ = h;

	Ref<RDUniform> u[6];
	for (int i = 0; i < 6; i++) u[i].instantiate();
	u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[0]->set_binding(0); u[0]->add_id(color_);
	u[1]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[1]->set_binding(1); u[1]->add_id(hitpos_);
	u[2]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[2]->set_binding(2); u[2]->add_id(sampler_); u[2]->add_id(world.sdf_atlas());
	u[3]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[3]->set_binding(3); u[3]->add_id(sampler_); u[3]->add_id(world.mat_atlas());
	u[4]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[4]->set_binding(4); u[4]->add_id(sampler_); u[4]->add_id(world.indirection_tex());
	u[5]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u[5]->set_binding(5); u[5]->add_id(world.palette_buffer());
	uset_ = rd->uniform_set_create(Array::make(u[0], u[1], u[2], u[3], u[4], u[5]), shader_, 0);
}

bool RaymarchPass::render(RenderingDevice *rd, const GpuWorld &world, const ve::CameraParams &cam,
		int width, int height) {
	if (!shader_.is_valid()) return false;
	if (width != width_ || height != height_ || !uset_.is_valid()) {
		rebuild_targets(rd, world, width, height);
	}
	if (!uset_.is_valid() || !color_.is_valid()) return false;

	PackedByteArray pc;
	pc.resize(sizeof(ve::CameraParams));
	std::memcpy(pc.ptrw(), &cam, sizeof(ve::CameraParams));

	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (width + 7) / 8, (height + 7) / 8, 1);
	rd->compute_list_end();
	return true;
}
