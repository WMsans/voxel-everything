#include "render/outline_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <chrono>

using namespace godot;

namespace {

RID make_texture(RenderingDevice *rd, RenderingDevice::DataFormat format) {
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_format(format);
	f->set_width(1);
	f->set_height(1);
	f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	Ref<RDTextureView> v;
	v.instantiate();
	return rd->texture_create(f, v, {});
}

Ref<RDUniform> sampler_texture(int binding, RID sampler, RID texture) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u->set_binding(binding);
	u->add_id(sampler);
	u->add_id(texture);
	return u;
}

Ref<RDUniform> image_texture(int binding, RID texture) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u->set_binding(binding);
	u->add_id(texture);
	return u;
}

Ref<RDUniform> uniform_buffer(int binding, RID buffer) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u->set_binding(binding);
	u->add_id(buffer);
	return u;
}

} // namespace

OutlinePass::~OutlinePass() {
	teardown();
}

bool OutlinePass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/outline.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("OutlinePass: shader load failed: ", err.c_str());
		teardown();
		return false;
	}
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("OutlinePass: ", compile_err);
		teardown();
		return false;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	pipeline_ = rd->compute_pipeline_create(shader_);
	Ref<RDSamplerState> nearest_state;
	nearest_state.instantiate();
	nearest_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	nearest_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	nearest_ = rd->sampler_create(nearest_state);
	dummy_normal_ = make_texture(rd, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
	if (dummy_normal_.is_valid())
		rd->texture_clear(dummy_normal_, Color(0.5f, 0.5f, 1.0f, 1.0f), 0, 1, 0, 1);
	if (!shader_.is_valid() || !pipeline_.is_valid() || !nearest_.is_valid() ||
			!dummy_normal_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void OutlinePass::teardown() {
	if (!rd_) return;
	if (uset_.is_valid()) rd_->free_rid(uset_);
	uset_ = RID();
	for (RID *r : {&pipeline_, &shader_, &dummy_normal_, &nearest_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	key_color_ = key_depth_ = key_gb_depth_ = key_surface_ = key_normal_ = key_camera_ = RID();
	last_ms_ = 0.0f;
	rd_ = nullptr;
}

bool OutlinePass::ensure_uniform_set(RenderingDevice *rd, RID scene_color, RID scene_depth,
		RID gb_depth, RID gb_surface, RID normal_roughness, RID camera_ubo) {
	if (uset_.is_valid() && key_color_ == scene_color && key_depth_ == scene_depth &&
			key_gb_depth_ == gb_depth && key_surface_ == gb_surface &&
			key_normal_ == normal_roughness && key_camera_ == camera_ubo)
		return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	const RID normal = normal_roughness.is_valid() ? normal_roughness : dummy_normal_;
	uset_ = rd->uniform_set_create(Array::make(
			sampler_texture(0, nearest_, scene_depth),
			sampler_texture(1, nearest_, gb_depth),
			sampler_texture(2, nearest_, gb_surface),
			sampler_texture(3, nearest_, normal),
			image_texture(4, scene_color),
			uniform_buffer(6, camera_ubo)), shader_, 0);
	if (!uset_.is_valid()) return false;
	key_color_ = scene_color;
	key_depth_ = scene_depth;
	key_gb_depth_ = gb_depth;
	key_surface_ = gb_surface;
	key_normal_ = normal_roughness;
	key_camera_ = camera_ubo;
	return true;
}

bool OutlinePass::render(RenderingDevice *rd, RID scene_color, RID scene_depth, RID gb_depth,
		RID gb_surface, RID normal_roughness, bool have_normal_roughness, RID camera_ubo,
		Vector2i size, const ve::BeautySettings &s) {
	if (!s.outlines) return false;
	if (!rd_ || rd != rd_ || !pipeline_.is_valid() || size.x <= 0 || size.y <= 0 ||
			!scene_color.is_valid() || !scene_depth.is_valid() || !gb_depth.is_valid() ||
			!gb_surface.is_valid() || !camera_ubo.is_valid()) return false;
	if (!ensure_uniform_set(rd, scene_color, scene_depth, gb_depth, gb_surface,
			normal_roughness, camera_ubo)) return false;
	const auto t0 = std::chrono::steady_clock::now();
	PackedByteArray pc;
	pc.resize(32);
	int32_t *i = reinterpret_cast<int32_t *>(pc.ptrw());
	float *f = reinterpret_cast<float *>(pc.ptrw());
	i[0] = size.x; i[1] = size.y;
	i[2] = have_normal_roughness && normal_roughness.is_valid() ? 1 : 0; i[3] = 0;
	f[4] = s.outline_depth_threshold; f[5] = s.outline_normal_threshold;
	f[6] = 0.35f; f[7] = 0.0f;
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (size.x + 7) / 8, (size.y + 7) / 8, 1);
	rd->compute_list_end();
	last_ms_ = std::chrono::duration<float, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	return true;
}
