#include "render/deferred_pass.h"
#include "render/gbuffer.h"
#include "render/material_atlas.h"
#include "render/shader_loader.h"
#include "shade/beauty_settings.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <chrono>
#include <cstring>

using namespace godot;

DeferredPass::~DeferredPass() {
	teardown();
}

void DeferredPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	std::string err;
	const String path = ProjectSettings::get_singleton()->globalize_path("res://shaders/deferred.comp.glsl");
	const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("DeferredPass: shader load failed: ", err.c_str());
		return;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("DeferredPass: ", compile_err);
		return;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	pipeline_ = rd->compute_pipeline_create(shader_);

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

void DeferredPass::set_sun_ubo(RID buffer) {
	sun_light_ubo_ = buffer;
	// The uniform set caches this RID; drop it so the next render rebuilds.
	if (rd_ && uset_.is_valid()) {
		rd_->free_rid(uset_);
		uset_ = RID();
	}
}

void DeferredPass::teardown() {
	if (!rd_) return;
	for (RID *r : {&uset_, &pipeline_, &shader_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	for (RID *r : {&dummy_black_, &dummy_far_, &dummy_white_, &sun_ubo_, &sampler_linear_, &sampler_nearest_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	key_albedo_ = RID();
	key_surface_ = RID();
	key_depth_ = RID();
	key_lit_ = RID();
	key_ssgi_ = RID();
	key_ssao_ = RID();
	key_sun_ = RID();
	key_material_albedo_ = RID();
	key_material_surface_ = RID();
	key_material_sampler_ = RID();
	rd_ = nullptr;
}

bool DeferredPass::ensure_dummies(RenderingDevice *rd) {
	if (dummy_black_.is_valid() && dummy_far_.is_valid() && sun_ubo_.is_valid()) return true;
	auto make_1x1 = [&](RenderingDevice::DataFormat fmt, const PackedByteArray &bytes) {
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_format(fmt);
		f->set_width(1);
		f->set_height(1);
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		TypedArray<PackedByteArray> data;
		data.push_back(bytes);
		return rd->texture_create(f, v, data);
	};
	PackedByteArray black;
	black.resize(8);
	black.fill(0);
	dummy_black_ = make_1x1(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, black);
	PackedByteArray white;
	white.resize(8);
	// AO = 1 everywhere: a missing SSAO input must leave the ambient term untouched.
	white.fill(0x3C00); // half float 1.0
	dummy_white_ = make_1x1(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, white);
	PackedByteArray far;
	far.resize(4);
	far.fill(0);
	dummy_far_ = make_1x1(RenderingDevice::DATA_FORMAT_R32_SFLOAT, far);
	PackedByteArray zeros;
	zeros.resize(80);
	zeros.fill(0);
	sun_ubo_ = rd->uniform_buffer_create(80, zeros);
	return dummy_black_.is_valid() && dummy_far_.is_valid() && dummy_white_.is_valid() && sun_ubo_.is_valid();
}

bool DeferredPass::ensure_uniform_set(RenderingDevice *rd, GBuffer &gb,
		const MaterialAtlas &materials, RID ssgi, RID ssao, RID sun_map) {
	const RID material_albedo = materials.albedo_array();
	const RID material_surface = materials.surface_array();
	const RID material_sampler = materials.sampler();
	if (uset_.is_valid() && key_albedo_ == gb.albedo() && key_surface_ == gb.surface() &&
			key_depth_ == gb.depth() && key_lit_ == gb.lit() && key_ssgi_ == ssgi &&
			key_ssao_ == ssao && key_sun_ == sun_map && key_material_albedo_ == material_albedo &&
			key_material_surface_ == material_surface && key_material_sampler_ == material_sampler)
		return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	Ref<RDUniform> u[11];
	for (int i = 0; i < 11; i++) u[i].instantiate();
	const RID textures[6] = {gb.albedo(), gb.surface(), gb.depth(), ssgi, sun_map, ssao};
	for (int i = 0; i < 5; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u[i]->set_binding(i);
		u[i]->add_id(i < 3 ? sampler_nearest_ : sampler_linear_);
		u[i]->add_id(textures[i]);
	}
	// SSAO lives at binding 7, after the material arrays' reserved slots.
	u[7]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[7]->set_binding(7);
	u[7]->add_id(sampler_nearest_);
	u[7]->add_id(ssao);
	u[5]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[5]->set_binding(5);
	u[5]->add_id(gb.lit());
	u[6]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[6]->set_binding(6);
	u[6]->add_id(sun_ubo_);
	u[8]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[8]->set_binding(8);
	u[8]->add_id(materials.sampler());
	u[8]->add_id(materials.albedo_array());
	u[9]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[9]->set_binding(9);
	u[9]->add_id(materials.sampler());
	u[9]->add_id(materials.surface_array());
	u[10]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[10]->set_binding(10);
	u[10]->add_id(sun_light_ubo_);
	uset_ = rd->uniform_set_create(
			Array::make(u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10]),
			shader_, 0);
	if (!uset_.is_valid()) return false;
	key_albedo_ = gb.albedo();
	key_surface_ = gb.surface();
	key_depth_ = gb.depth();
	key_lit_ = gb.lit();
	key_ssgi_ = ssgi;
	key_ssao_ = ssao;
	key_sun_ = sun_map;
	key_material_albedo_ = material_albedo;
	key_material_surface_ = material_surface;
	key_material_sampler_ = material_sampler;
	return true;
}

bool DeferredPass::render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
		RID ssgi, RID ssao, RID sun_map, const float sun_view_proj[16], float shadow_texel,
		const Params &p) {
	if (!is_valid() || !gb.is_valid()) return false;
	if (!ensure_dummies(rd)) return false;
	const auto t0 = std::chrono::steady_clock::now();
	uint32_t flags = p.flags;
	if (!ssgi.is_valid()) flags &= ~ve::kFlagSsgi;
	if (!ssao.is_valid()) flags &= ~ve::kFlagSsao;
	if (!sun_map.is_valid()) flags &= ~ve::kFlagSunMap;
	const RID ssgi_bound = ssgi.is_valid() ? ssgi : dummy_black_;
	const RID ssao_bound = ssao.is_valid() ? ssao : dummy_white_;
	const RID sun_bound = sun_map.is_valid() ? sun_map : dummy_far_;
	if (!ensure_uniform_set(rd, gb, materials, ssgi_bound, ssao_bound, sun_bound)) return false;

	PackedByteArray ub;
	ub.resize(80);
	float *uf = reinterpret_cast<float *>(ub.ptrw());
	for (int i = 0; i < 16; i++) uf[i] = sun_map.is_valid() ? sun_view_proj[i] : 0.0f;
	uf[16] = shadow_texel;
	uf[17] = p.shadow_depth_range;
	uf[18] = p.fade_start;
	uf[19] = p.fade_end;
	rd->buffer_update(sun_ubo_, 0, 80, ub);

	static_assert(sizeof(float) * 28 == 112, "deferred push block");
	PackedByteArray pcb;
	pcb.resize(112);
	float *f = reinterpret_cast<float *>(pcb.ptrw());
	for (int i = 0; i < 16; i++) f[i] = p.inv_view_proj[i];
	f[16] = p.cam_pos[0];
	f[17] = p.cam_pos[1];
	f[18] = p.cam_pos[2];
	f[19] = 0.0f;
	f[20] = p.ambient[0];
	f[21] = p.ambient[1];
	f[22] = p.ambient[2];
	f[23] = 0.0f;
	uint32_t *u = reinterpret_cast<uint32_t *>(pcb.ptrw());
	u[24] = flags;
	u[25] = static_cast<uint32_t>(p.probe_mode);
	u[26] = 0;
	u[27] = 0;

	const Vector2i size = gb.size();
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pcb, pcb.size());
	rd->compute_list_dispatch(list, (size.x + 7) / 8, (size.y + 7) / 8, 1);
	rd->compute_list_end();
	last_ms_ = std::chrono::duration<float, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	return true;
}
