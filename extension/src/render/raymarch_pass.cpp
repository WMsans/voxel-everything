#include "render/raymarch_pass.h"
#include "render/gpu_atlas.h"
#include "render/island_atlas.h"
#include "render/material_atlas.h"
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

	{
		PackedByteArray zero;
		zero.resize(32);
		zero.fill(0);
		edits_ubo_ = rd->uniform_buffer_create(32, zero);
	}
}

void RaymarchPass::set_materials(const MaterialAtlas &materials) {
	material_albedo_ = materials.albedo_array();
	material_surface_ = materials.surface_array();
	material_sampler_ = materials.sampler();
	// The uniform set caches these RIDs; drop it so the next render rebuilds with the new
	// arrays. This is called once before the first render, so the invalidation is a no-op.
	if (uset_.is_valid()) {
		rd_->free_rid(uset_);
		uset_ = RID();
	}
}

void RaymarchPass::teardown() {
	if (!rd_) return;
	// Free order matters on Godot 4.7.1's RenderingDevice: freeing a texture (or shader)
	// cascades to referencing uniform sets, and freeing a shader also tears down its
	// pipelines — so uset_ first, then pipeline_ before shader_, then the targets.
	// uset_mask_ is only a cache key for an externally owned tile-mask RID (usually the
	// IslandAtlas fallback mask); it must not be freed here.
	for (RID *r : {&uset_, &pipeline_, &shader_, &albedo_, &surface_, &hitpos_, &cost_buf_,
			 &sampler_, &edits_ubo_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	uset_mask_ = RID();
	material_albedo_ = RID();
	material_surface_ = RID();
	material_sampler_ = RID();
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

void RaymarchPass::rebuild_targets(RenderingDevice *rd, const GpuAtlas &atlas,
		const IslandAtlas *islands, RID tile_mask, int w, int h) {
	// Old uniform set references the old G-buffer targets: free it before them.
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	if (albedo_.is_valid()) rd->free_rid(albedo_);
	if (surface_.is_valid()) rd->free_rid(surface_);
	if (hitpos_.is_valid()) rd->free_rid(hitpos_);
	if (cost_buf_.is_valid()) rd->free_rid(cost_buf_);
	albedo_ = make_target(rd, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, w, h);
	surface_ = make_target(rd, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, w, h);
	hitpos_ = make_target(rd, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, w, h);
	cost_buf_ = rd->storage_buffer_create(static_cast<uint32_t>(w) * h * 2u * sizeof(uint32_t));
	width_ = w;
	height_ = h;

	Ref<RDUniform> u[24];
	for (int i = 0; i < 24; i++) u[i].instantiate();
	u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[0]->set_binding(0); u[0]->add_id(albedo_);
	u[1]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[1]->set_binding(1); u[1]->add_id(hitpos_);
	const RID sampled[5] = {atlas.sdf_atlas(), atlas.mat_atlas(), atlas.mip_atlas(0),
			atlas.mip_atlas(1), atlas.mip_atlas(2)};
	for (int i = 2; i <= 6; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u[i]->set_binding(i); u[i]->add_id(sampler_); u[i]->add_id(sampled[i - 2]);
	}
	const RID buffers[5] = {atlas.palette(), atlas.region_map(), atlas.region_tables(),
			atlas.op_pool(), atlas.op_counts()};
	for (int i = 7; i <= 11; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u[i]->set_binding(i); u[i]->add_id(buffers[i - 7]);
	}
	u[12]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[12]->set_binding(12); u[12]->add_id(edits_ubo_);
	// 13-17: shared authoritative volume SDF/material (indexed by Island.volume_slot since
	// Task 6), island min-max chain, descriptors, tile mask. Atlas slot still selects the
	// descriptor/mip/tile-mask entries.
	const RID island_bufs[5] = {atlas.volumes().sdf_buffer(), atlas.volumes().mat_buffer(),
			islands->mip_buffer(), islands->desc_buffer(),
			tile_mask.is_valid() ? tile_mask : islands->fallback_mask()};
	for (int i = 13; i <= 17; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u[i]->set_binding(i); u[i]->add_id(island_bufs[i - 13]);
	}
	// 18-19: the shared material arrays (set by set_materials()).
	const RID material_samplers[2] = {material_albedo_, material_surface_};
	for (int i = 18; i <= 19; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u[i]->set_binding(i); u[i]->add_id(material_sampler_);
		u[i]->add_id(material_samplers[i - 18]);
	}
	u[20]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[20]->set_binding(20); u[20]->add_id(surface_);
	u[21]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u[21]->set_binding(21); u[21]->add_id(atlas.brick_flags());
	u[22]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u[22]->set_binding(22); u[22]->add_id(atlas.region_slot_counts());
	u[23]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u[23]->set_binding(23); u[23]->add_id(cost_buf_);
	Array uset_args;
	for (int i = 0; i < 24; i++) uset_args.push_back(u[i]);
	uset_ = rd->uniform_set_create(uset_args, shader_, 0);
}

bool RaymarchPass::targets_need_rebuild(int width, int height, RID mask) const {
	return width != width_ || height != height_ || mask != uset_mask_ || !uset_.is_valid();
}

bool RaymarchPass::render(RenderingDevice *rd, const GpuAtlas &atlas,
		const IslandAtlas *islands, RID tile_mask, const ve::CameraParams &cam,
		int width, int height, const float edit_state[6]) {
	if (!shader_.is_valid()) return false;
	if (!islands || !islands->is_valid()) return false;
	const RID mask = tile_mask.is_valid() ? tile_mask : islands->fallback_mask();
	if (width != width_ || height != height_ || mask != uset_mask_ || !uset_.is_valid()) {
		rebuild_targets(rd, atlas, islands, mask, width, height);
		uset_mask_ = mask;
	}
	if (!uset_.is_valid() || !albedo_.is_valid() || !surface_.is_valid() ||
			!edits_ubo_.is_valid()) return false;

	// Recorded before the compute list: buffer_update errors while a list is open, and the
	// deferred update still lands before the dispatch at submit.
	{
		PackedByteArray eb;
		eb.resize(32);
		float *f = reinterpret_cast<float *>(eb.ptrw());
		for (int i = 0; i < 3; i++) f[i] = edit_state[i];
		f[3] = 0.0f;
		f[4] = edit_state[3]; // radius
		f[5] = edit_state[4]; // type
		f[6] = edit_state[5]; // material
		f[7] = edit_state[3] > 0.0f ? 1.0f : 0.0f;
		rd->buffer_update(edits_ubo_, 0, 32, eb);
	}

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
