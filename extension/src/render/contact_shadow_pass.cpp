#include "render/contact_shadow_pass.h"
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
#include <algorithm>

using namespace godot;

ContactShadowPass::~ContactShadowPass() {
	teardown();
}

void ContactShadowPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/contact_shadow.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("ContactShadowPass: shader load failed: ", err.c_str());
		teardown();
		return;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("ContactShadowPass: ", compile_err);
		teardown();
		return;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	pipeline_ = rd->compute_pipeline_create(shader_);
	Ref<RDSamplerState> nearest;
	nearest.instantiate();
	nearest->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	nearest->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_nearest_ = rd->sampler_create(nearest);
	Ref<RDSamplerState> linear;
	linear.instantiate();
	linear->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	linear->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_linear_ = rd->sampler_create(linear);
	if (!shader_.is_valid() || !pipeline_.is_valid() || !sampler_nearest_.is_valid() ||
			!sampler_linear_.is_valid()) teardown();
}

void ContactShadowPass::set_sun_ubo(RID buffer) {
	sun_light_ubo_ = buffer;
	// The uniform set caches this RID; drop it so the next render rebuilds.
	if (rd_ && uset_.is_valid()) {
		rd_->free_rid(uset_);
		uset_ = RID();
	}
}

void ContactShadowPass::teardown() {
	if (!rd_) return;
	for (RID *r : {&uset_, &pipeline_, &shader_, &sampler_nearest_, &sampler_linear_, &mask_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	key_color_ = key_depth_ = key_camera_ = RID();
	size_ = Vector2i(0, 0);
	rd_ = nullptr;
}

bool ContactShadowPass::ensure_mask(RenderingDevice *rd, Vector2i size) {
	if (mask_.is_valid() && size == size_) return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	if (mask_.is_valid()) rd->free_rid(mask_);
	mask_ = RID();
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
	f->set_width(std::max(1, size.x / 2));
	f->set_height(std::max(1, size.y / 2));
	f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	Ref<RDTextureView> v;
	v.instantiate();
	mask_ = rd->texture_create(f, v, {});
	if (!mask_.is_valid()) return false;
	size_ = size;
	key_color_ = key_depth_ = key_camera_ = RID();
	return true;
}

bool ContactShadowPass::ensure_uniform_set(RenderingDevice *rd, RID scene_color,
		RID scene_depth, RID camera_ubo) {
	if (uset_.is_valid() && key_color_ == scene_color && key_depth_ == scene_depth &&
			key_camera_ == camera_ubo) return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	Ref<RDUniform> u0, u1, u2, u3, u4, u5;
	for (Ref<RDUniform> *u : {&u0, &u1, &u2, &u3, &u4, &u5}) u->instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u0->set_binding(0); u0->add_id(sampler_nearest_); u0->add_id(scene_depth);
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u1->set_binding(1); u1->add_id(mask_);
	u2->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u2->set_binding(2); u2->add_id(sampler_linear_); u2->add_id(mask_);
	u3->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u3->set_binding(3); u3->add_id(scene_color);
	u4->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u4->set_binding(4); u4->add_id(camera_ubo);
	u5->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u5->set_binding(5); u5->add_id(sun_light_ubo_);
	uset_ = rd->uniform_set_create(Array::make(u0, u1, u2, u3, u4, u5), shader_, 0);
	if (!uset_.is_valid()) return false;
	key_color_ = scene_color;
	key_depth_ = scene_depth;
	key_camera_ = camera_ubo;
	return true;
}

bool ContactShadowPass::render(RenderingDevice *rd, RID scene_color, RID scene_depth,
		Vector2i size, RID camera_ubo, const ve::BeautySettings &s) {
	if (!s.contact_shadows || s.contact_steps <= 0) return false;
	if (!rd_ || rd != rd_ || !pipeline_.is_valid() || !scene_color.is_valid() ||
			!scene_depth.is_valid() || !camera_ubo.is_valid() || size.x <= 0 || size.y <= 0)
		return false;
	if (!ensure_mask(rd, size) || !ensure_uniform_set(rd, scene_color, scene_depth, camera_ubo))
		return false;

	const auto t0 = std::chrono::steady_clock::now();
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	PackedByteArray pc;
	pc.resize(32);
	int32_t *dims = reinterpret_cast<int32_t *>(pc.ptrw());
	dims[0] = std::max(1, size.x / 2);
	dims[1] = std::max(1, size.y / 2);
	dims[2] = 0;
	dims[3] = s.contact_steps;
	float *params = reinterpret_cast<float *>(pc.ptrw()) + 4;
	params[0] = 0.6f;
	params[1] = 0.85f;
	// One voxel: large enough to leave the receiver, but too small to bridge terrain gaps.
	params[2] = 0.05f;
	params[3] = 0.0f;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_add_barrier(list);
	dims[0] = size.x;
	dims[1] = size.y;
	dims[2] = 1;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_end();
	// This is command-record time, not GPU execution time (M5 errata 15).
	last_ms_ = std::chrono::duration<float, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	return true;
}
