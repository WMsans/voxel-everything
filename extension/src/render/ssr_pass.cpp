#include "render/ssr_pass.h"
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
#include <algorithm>
#include <chrono>
#include <cstring>

using namespace godot;

namespace {

RID make_texture(RenderingDevice *rd, RenderingDevice::DataFormat format, Vector2i size,
		uint32_t usage) {
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_format(format);
	f->set_width(size.x);
	f->set_height(size.y);
	f->set_usage_bits(usage);
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

} // namespace

SsrPass::~SsrPass() {
	teardown();
}

bool SsrPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/ssr.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("SsrPass: shader load failed: ", err.c_str());
		teardown();
		return false;
	}

	auto compile = [&](const std::string &source, const char *label) -> RID {
		Ref<RDShaderSource> shader_source;
		shader_source.instantiate();
		shader_source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		shader_source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE,
				String(source.c_str()));
		Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(shader_source);
		const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
		if (!compile_err.is_empty()) {
			UtilityFunctions::printerr("SsrPass ", label, ": ", compile_err);
			return RID();
		}
		return rd->shader_create_from_spirv(spirv);
	};
	trace_shader_ = compile(code, "trace");
	const std::string define = "#define SSR_APPLY 1\n";
	const size_t version_end = code.find('\n');
	const std::string apply_code = version_end == std::string::npos ? std::string() :
			code.substr(0, version_end + 1) + define + code.substr(version_end + 1);
	apply_shader_ = apply_code.empty() ? RID() : compile(apply_code, "apply");
	if (!trace_shader_.is_valid() || !apply_shader_.is_valid()) {
		teardown();
		return false;
	}
	trace_pipeline_ = rd->compute_pipeline_create(trace_shader_);
	apply_pipeline_ = rd->compute_pipeline_create(apply_shader_);
	Ref<RDSamplerState> nearest_state;
	nearest_state.instantiate();
	nearest_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	nearest_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	nearest_ = rd->sampler_create(nearest_state);
	Ref<RDSamplerState> linear_state;
	linear_state.instantiate();
	linear_state->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	linear_state->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	linear_ = rd->sampler_create(linear_state);
	const uint32_t sample_usage = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	dummy_normal_ = make_texture(rd, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			Vector2i(1, 1), sample_usage);
	if (dummy_normal_.is_valid()) rd->texture_clear(dummy_normal_, Color(0.0f, 0.0f, 0.0f, 1.0f), 0, 1, 0, 1);
	if (!trace_pipeline_.is_valid() || !apply_pipeline_.is_valid() || !nearest_.is_valid() ||
			!linear_.is_valid() || !dummy_normal_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void SsrPass::teardown() {
	if (!rd_) return;
	for (RID *r : {&trace_set_, &apply_set_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	for (RID *r : {&trace_pipeline_, &apply_pipeline_, &trace_shader_, &apply_shader_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	for (RID *r : {&reflection_, &dummy_normal_, &nearest_, &linear_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	key_color_ = key_depth_ = key_surface_ = key_gb_depth_ = key_normal_ = key_camera_ = RID();
	half_size_ = Vector2i(0, 0);
	last_ms_ = 0.0f;
	rd_ = nullptr;
}

bool SsrPass::ensure_targets(RenderingDevice *rd, Vector2i size) {
	if (size.x <= 0 || size.y <= 0) return false;
	const Vector2i half(std::max(1, size.x / 2), std::max(1, size.y / 2));
	if (reflection_.is_valid() && half == half_size_) return true;
	if (trace_set_.is_valid()) rd->free_rid(trace_set_);
	if (apply_set_.is_valid()) rd->free_rid(apply_set_);
	trace_set_ = apply_set_ = RID();
	if (reflection_.is_valid()) rd->free_rid(reflection_);
	reflection_ = make_texture(rd, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, half,
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	if (!reflection_.is_valid()) return false;
	half_size_ = half;
	key_color_ = key_depth_ = key_surface_ = key_gb_depth_ = key_normal_ = key_camera_ = RID();
	return true;
}

bool SsrPass::ensure_uniform_sets(RenderingDevice *rd, RID scene_color, RID scene_depth,
		RID gb_surface, RID gb_depth, RID normal_roughness, RID camera_ubo) {
	if (trace_set_.is_valid() && apply_set_.is_valid() && key_color_ == scene_color &&
			key_depth_ == scene_depth && key_surface_ == gb_surface && key_gb_depth_ == gb_depth &&
			key_normal_ == normal_roughness && key_camera_ == camera_ubo) return true;
	if (trace_set_.is_valid()) rd->free_rid(trace_set_);
	if (apply_set_.is_valid()) rd->free_rid(apply_set_);
	trace_set_ = apply_set_ = RID();
	const RID normal = normal_roughness.is_valid() ? normal_roughness : dummy_normal_;
	const Array trace_uniforms = Array::make(
			sampler_texture(0, linear_, scene_color),
			sampler_texture(1, nearest_, scene_depth),
			sampler_texture(2, nearest_, gb_surface),
			sampler_texture(3, nearest_, gb_depth),
			sampler_texture(4, nearest_, normal),
			image_texture(5, reflection_));
	Ref<RDUniform> camera;
	camera.instantiate();
	camera->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	camera->set_binding(6);
	camera->add_id(camera_ubo);
	Array trace_with_camera = trace_uniforms;
	trace_with_camera.push_back(camera);
	trace_set_ = rd->uniform_set_create(trace_with_camera, trace_shader_, 0);
	const Array apply_uniforms = Array::make(sampler_texture(0, linear_, reflection_),
			image_texture(1, scene_color));
	apply_set_ = rd->uniform_set_create(apply_uniforms, apply_shader_, 0);
	if (!trace_set_.is_valid() || !apply_set_.is_valid()) {
		if (trace_set_.is_valid()) rd->free_rid(trace_set_);
		if (apply_set_.is_valid()) rd->free_rid(apply_set_);
		trace_set_ = apply_set_ = RID();
		return false;
	}
	key_color_ = scene_color;
	key_depth_ = scene_depth;
	key_surface_ = gb_surface;
	key_gb_depth_ = gb_depth;
	key_normal_ = normal_roughness;
	key_camera_ = camera_ubo;
	return true;
}

bool SsrPass::render(RenderingDevice *rd, RID scene_color, RID scene_depth, RID gb_surface,
		RID gb_depth, RID normal_roughness, bool have_normal_roughness, RID camera_ubo,
		Vector2i size, const ve::BeautySettings &s) {
	if (!s.ssr || s.ssr_steps <= 0) return false;
	if (!rd_ || rd != rd_ || !trace_pipeline_.is_valid() || !apply_pipeline_.is_valid() ||
			!scene_color.is_valid() || !scene_depth.is_valid() || !gb_surface.is_valid() ||
			!gb_depth.is_valid() || !camera_ubo.is_valid()) return false;
	if (!ensure_targets(rd, size) || !ensure_uniform_sets(rd, scene_color, scene_depth,
			gb_surface, gb_depth, normal_roughness, camera_ubo)) return false;
	const auto t0 = std::chrono::steady_clock::now();
	PackedByteArray trace_pc;
	trace_pc.resize(32);
	int32_t *i = reinterpret_cast<int32_t *>(trace_pc.ptrw());
	float *f = reinterpret_cast<float *>(trace_pc.ptrw());
	i[0] = half_size_.x; i[1] = half_size_.y; i[2] = s.ssr_steps;
	i[3] = have_normal_roughness && normal_roughness.is_valid() ? 1 : 0;
	f[4] = kReachM; f[5] = kStartBiasM; f[6] = kThicknessM; f[7] = kStrength;
	const int64_t trace_list = rd->compute_list_begin();
	if (trace_list < 0) return false;
	rd->compute_list_bind_compute_pipeline(trace_list, trace_pipeline_);
	rd->compute_list_bind_uniform_set(trace_list, trace_set_, 0);
	rd->compute_list_set_push_constant(trace_list, trace_pc, trace_pc.size());
	rd->compute_list_dispatch(trace_list, (half_size_.x + 7) / 8, (half_size_.y + 7) / 8, 1);
	rd->compute_list_end();
	PackedByteArray apply_pc;
	apply_pc.resize(16);
	int32_t *dims = reinterpret_cast<int32_t *>(apply_pc.ptrw());
	dims[0] = size.x; dims[1] = size.y; dims[2] = dims[3] = 0;
	const int64_t apply_list = rd->compute_list_begin();
	if (apply_list < 0) return false;
	rd->compute_list_bind_compute_pipeline(apply_list, apply_pipeline_);
	rd->compute_list_bind_uniform_set(apply_list, apply_set_, 0);
	rd->compute_list_set_push_constant(apply_list, apply_pc, apply_pc.size());
	rd->compute_list_dispatch(apply_list, (size.x + 7) / 8, (size.y + 7) / 8, 1);
	rd->compute_list_end();
	last_ms_ = std::chrono::duration<float, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	return true;
}
