#include "render/ssao_pass.h"
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
#include <chrono>

using namespace godot;

// Must match the Push block in ssao.comp.glsl.
static const float kSsaoRadius = 5.0f;
static const int kSsaoSteps = 8;
static const float kSsaoStrength = 1.5f;

SsaoPass::~SsaoPass() {
	teardown();
}

void SsaoPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/ssao.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("SsaoPass: shader load failed: ", err.c_str());
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
		UtilityFunctions::printerr("SsaoPass: ", compile_err);
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
	if (!shader_.is_valid() || !pipeline_.is_valid() || !sampler_nearest_.is_valid()) teardown();
}

void SsaoPass::teardown() {
	if (!rd_) return;
	for (RID *r : {&uset_, &pipeline_, &shader_, &target_, &sampler_nearest_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	output_ = RID();
	key_surface_ = key_depth_ = key_out_ = key_camera_ = RID();
	size_ = Vector2i(0, 0);
	rd_ = nullptr;
}

bool SsaoPass::ensure_target(RenderingDevice *rd, Vector2i size) {
	if (size.x <= 0 || size.y <= 0) return false;
	if (target_.is_valid() && size == size_) return true;
	if (target_.is_valid()) rd->free_rid(target_);
	target_ = RID();
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
	f->set_width(size.x);
	f->set_height(size.y);
	f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	Ref<RDTextureView> v;
	v.instantiate();
	target_ = rd->texture_create(f, v, {});
	if (!target_.is_valid()) return false;
	// Defined before the first read even if the pass is skipped this frame.
	rd->texture_clear(target_, Color(1.0f, 1.0f, 1.0f, 1.0f), 0, 1, 0, 1);
	size_ = size;
	key_surface_ = key_depth_ = key_out_ = key_camera_ = RID();
	return true;
}

bool SsaoPass::ensure_uniform_set(RenderingDevice *rd, GBuffer &gb, RID camera_ubo) {
	if (uset_.is_valid() && key_surface_ == gb.surface() && key_depth_ == gb.depth() &&
			key_out_ == target_ && key_camera_ == camera_ubo) return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	Ref<RDUniform> u[4];
	for (Ref<RDUniform> &item : u) item.instantiate();
	const RID textures[2] = {gb.surface(), gb.depth()};
	for (int i = 0; i < 2; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u[i]->set_binding(i);
		u[i]->add_id(sampler_nearest_);
		u[i]->add_id(textures[i]);
	}
	u[2]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[2]->set_binding(2);
	u[2]->add_id(target_);
	u[3]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[3]->set_binding(5);
	u[3]->add_id(camera_ubo);
	uset_ = rd->uniform_set_create(Array::make(u[0], u[1], u[2], u[3]), shader_, 0);
	if (!uset_.is_valid()) return false;
	key_surface_ = gb.surface();
	key_depth_ = gb.depth();
	key_out_ = target_;
	key_camera_ = camera_ubo;
	return true;
}

bool SsaoPass::render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
		const ve::BeautySettings &s) {
	output_ = RID();
	if (!s.ssao) return false;
	if (!rd_ || rd != rd_ || !pipeline_.is_valid() || !gb.is_valid() ||
			!camera_ubo.is_valid()) return false;
	if (!ensure_target(rd, gb.size())) return false;
	if (!ensure_uniform_set(rd, gb, camera_ubo)) return false;

	const auto t0 = std::chrono::steady_clock::now();
	static_assert(sizeof(float) * 8 == 32, "ssao push block");
	PackedByteArray pc;
	pc.resize(32);
	int32_t *dims = reinterpret_cast<int32_t *>(pc.ptrw());
	dims[0] = gb.size().x;
	dims[1] = gb.size().y;
	dims[2] = kSsaoSteps;
	dims[3] = 0;
	float *f = reinterpret_cast<float *>(pc.ptrw());
	f[4] = kSsaoRadius;
	f[5] = kSsaoStrength;
	f[6] = f[7] = 0.0f;

	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_end();
	output_ = target_;
	last_ms_ = std::chrono::duration<float, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	return true;
}
