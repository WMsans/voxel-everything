#include "render/ssgi_pass.h"
#include "render/gbuffer.h"
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

SsgiPass::~SsgiPass() {
	teardown();
}

void SsgiPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/ssgi.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("SsgiPass: shader load failed: ", err.c_str());
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
		UtilityFunctions::printerr("SsgiPass: ", compile_err);
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

void SsgiPass::teardown() {
	if (!rd_) return;
	for (RID *r : {&uset_, &pipeline_, &shader_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	for (RID *r : {&targets_[0], &targets_[1], &sampler_nearest_, &sampler_linear_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	output_ = RID();
	key_albedo_ = key_surface_ = key_depth_ = key_history_ = key_prev_ = key_out_ = key_camera_ = RID();
	size_ = Vector2i(0, 0);
	rd_ = nullptr;
}

bool SsgiPass::ensure_targets(RenderingDevice *rd, Vector2i size) {
	if (size.x <= 0 || size.y <= 0) return false;
	if (targets_[0].is_valid() && targets_[1].is_valid() && size == size_) return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	for (RID *r : {&targets_[0], &targets_[1]}) {
		if (r->is_valid()) rd->free_rid(*r);
		*r = RID();
	}
	const Vector2i half(std::max(1, size.x / 2), std::max(1, size.y / 2));
	for (RID *target : {&targets_[0], &targets_[1]}) {
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
		f->set_width(half.x);
		f->set_height(half.y);
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		*target = rd->texture_create(f, v, {});
		if (!target->is_valid()) return false;
		// Defined before the first ping-pong read, including a first frame whose history is absent.
		rd->texture_clear(*target, Color(0.0f, 0.0f, 0.0f, 0.0f), 0, 1, 0, 1);
	}
	size_ = size;
	key_albedo_ = key_surface_ = key_depth_ = key_history_ = key_prev_ = key_out_ = key_camera_ = RID();
	return true;
}

bool SsgiPass::ensure_uniform_set(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
		RID prev_ssgi, RID out_ssgi) {
	if (uset_.is_valid() && key_albedo_ == gb.albedo() && key_surface_ == gb.surface() &&
			key_depth_ == gb.depth() && key_history_ == gb.history() && key_prev_ == prev_ssgi &&
			key_out_ == out_ssgi && key_camera_ == camera_ubo) return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	Ref<RDUniform> u[6];
	for (Ref<RDUniform> &item : u) item.instantiate();
	const RID textures[4] = {gb.surface(), gb.depth(), gb.history(), prev_ssgi};
	for (int i = 0; i < 4; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u[i]->set_binding(i);
		u[i]->add_id(i < 2 ? sampler_nearest_ : sampler_linear_);
		u[i]->add_id(textures[i]);
	}
	u[4]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[4]->set_binding(4);
	u[4]->add_id(out_ssgi);
	u[5]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[5]->set_binding(5);
	u[5]->add_id(camera_ubo);
	uset_ = rd->uniform_set_create(Array::make(u[0], u[1], u[2], u[3], u[4], u[5]), shader_, 0);
	if (!uset_.is_valid()) return false;
	key_albedo_ = gb.albedo();
	key_surface_ = gb.surface();
	key_depth_ = gb.depth();
	key_history_ = gb.history();
	key_prev_ = prev_ssgi;
	key_out_ = out_ssgi;
	key_camera_ = camera_ubo;
	return true;
}

bool SsgiPass::render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
		const float prev_view_proj[16], bool have_history, const ve::BeautySettings &s,
		uint32_t frame) {
	output_ = RID();
	if (!s.ssgi || s.ssgi_taps <= 0) return false;
	if (!rd_ || rd != rd_ || !pipeline_.is_valid() || !gb.is_valid() ||
			!camera_ubo.is_valid() || !prev_view_proj) return false;
	if (!ensure_targets(rd, gb.size())) return false;
	const uint32_t out_index = frame & 1u;
	const uint32_t prev_index = (frame + 1u) & 1u;
	if (!ensure_uniform_set(rd, gb, camera_ubo, targets_[prev_index], targets_[out_index])) return false;

	const auto t0 = std::chrono::steady_clock::now();
	static_assert(sizeof(float) * 24 == 96, "ssgi push block");
	PackedByteArray pc;
	pc.resize(96);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	std::memcpy(f, prev_view_proj, sizeof(float) * 16);
	int32_t *dims = reinterpret_cast<int32_t *>(f + 16);
	dims[0] = std::max(1, gb.size().x / 2);
	dims[1] = std::max(1, gb.size().y / 2);
	dims[2] = s.ssgi_taps;
	dims[3] = have_history ? 1 : 0;
	f[20] = 6.0f;
	f[21] = 0.90f;
	f[22] = 1.0f;
	f[23] = 0.0f;
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_end();
	output_ = targets_[out_index];
	last_ms_ = std::chrono::duration<float, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	return true;
}
