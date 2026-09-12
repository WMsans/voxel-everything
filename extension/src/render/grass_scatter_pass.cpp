#include "render/grass_scatter_pass.h"
#include "render/gpu_atlas.h"
#include "render/shader_loader.h"
#include "shade/oct.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

namespace {
// Loads one compute shader from res://shaders, mirroring SsaoPass's load sequence. Returns
// false without touching `shader`/`pipeline` on any failure.
bool load_compute(RenderingDevice *rd, const char *res, RID *shader, RID *pipeline) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(res);
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("GrassScatterPass: shader load failed: ", err.c_str());
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("GrassScatterPass: ", compile_err);
		return false;
	}
	*shader = rd->shader_create_from_spirv(spirv);
	*pipeline = rd->compute_pipeline_create(*shader);
	if (!shader->is_valid() || !pipeline->is_valid()) {
		UtilityFunctions::printerr("GrassScatterPass: pipeline creation failed for ", res);
		return false;
	}
	return true;
}
} // namespace

GrassScatterPass::~GrassScatterPass() {
	teardown();
}

bool GrassScatterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	if (!load_compute(rd, "res://shaders/grass_bricks.comp.glsl", &bricks_shader_,
			&bricks_pipeline_)) {
		teardown();
		return false;
	}
	// grass_scatter.comp.glsl arrives in Task 6. Until then the scatter stage stays
	// invalid and run() skips stage 2 -- the pass still culls bricks and reports zeros.
	if (!load_compute(rd, "res://shaders/grass_scatter.comp.glsl", &scatter_shader_,
			&scatter_pipeline_)) {
		scatter_shader_ = RID();
		scatter_pipeline_ = RID();
	}
	// Owned sampler pair for the atlas textures stage 1 declares but never samples.
	// Mirrors RaymarchPass: the SDF atlas filters linearly, the integer material atlas
	// must stay nearest.
	{
		Ref<RDSamplerState> ss;
		ss.instantiate();
		ss->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
		ss->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
		sampler_nearest_ = rd->sampler_create(ss);

		Ref<RDSamplerState> ls;
		ls.instantiate();
		ls->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		ls->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
		ls->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
		ls->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
		ls->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
		sampler_linear_ = rd->sampler_create(ls);
	}
	if (!sampler_linear_.is_valid() || !sampler_nearest_.is_valid()) {
		teardown();
		return false;
	}
	// Allocate the default-sized buffers eagerly. Tests (and debug_grass_stats) never drive
	// the compositor, so a capacity that only appears after the first frame would read as
	// zero there; run() still re-sizes to the live layout before every dispatch.
	const ve::GrassSettings defaults;
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	static const float kIdentity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
	const ve::GrassLayout warm = ve::grass_layout(defaults, origin, kIdentity);
	if (!ensure_buffers(rd, warm.params.limits[0], warm.max_bricks)) {
		teardown();
		return false;
	}
	return true;
}

void GrassScatterPass::teardown() {
	if (!rd_) return;
	for (RID *r : {&bricks_uset_, &scatter_uset_, &bricks_pipeline_, &bricks_shader_,
			&scatter_pipeline_, &scatter_shader_, &params_ubo_, &brick_list_, &counters_,
			&dispatch_args_, &instances_, &draw_args_, &region_ubo_, &sampler_linear_,
			&sampler_nearest_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	key_params_ = key_bricks_ = key_counters_ = key_dispatch_ = RID();
	key_rmap_ = key_rtables_ = key_bflags_ = RID();
	key_sdf_ = key_mat_ = key_palette_ = key_region_ = RID();
	key_sparams_ = key_sbricks_ = key_scounters_ = key_sdraw_ = RID();
	key_srmap_ = key_srtables_ = key_sbflags_ = RID();
	key_spalette_ = key_ssdf_ = key_smat_ = RID();
	key_sregion_ = key_sinstances_ = key_sslot_counts_ = key_ssun_ = RID();
	sample_count_ = 0;
	sample_min_normal_y_ = 1.0f;
	sample_max_height_ = 0.0f;
	sample_min_sun_ = 1.0f;
	sample_max_sun_ = 0.0f;
	sample_mean_sun_ = 0.0f;
	capacity_ = 0;
	brick_capacity_ = 0;
	last_brick_count_ = 0;
	last_blade_count_ = 0;
	overflow_logged_ = false;
	rd_ = nullptr;
}

bool GrassScatterPass::ensure_buffers(RenderingDevice *rd, int max_blades, int max_bricks) {
	if (max_blades <= 0 || max_bricks <= 0) return false;
	if (instances_.is_valid() && capacity_ == max_blades && brick_capacity_ >= max_bricks)
		return true;
	for (RID *r : {&instances_, &brick_list_, &counters_, &dispatch_args_, &draw_args_,
			&params_ubo_, &region_ubo_, &bricks_uset_, &scatter_uset_}) {
		if (r->is_valid()) rd->free_rid(*r);
		*r = RID();
	}
	// 32 bytes per blade: two vec4 (design doc section 5).
	instances_ = rd->storage_buffer_create(static_cast<uint32_t>(max_blades) * 32u);
	brick_list_ = rd->storage_buffer_create(static_cast<uint32_t>(max_bricks) * 4u);
	counters_ = rd->storage_buffer_create(16u);
	dispatch_args_ = rd->storage_buffer_create(12u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	// Non-indexed indirect draw args: vertexCount, instanceCount, firstVertex, firstInstance.
	draw_args_ = rd->storage_buffer_create(16u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	params_ubo_ = rd->uniform_buffer_create(sizeof(ve::GrassParams));
	// Region-window block for stage 1's binding 10 (three ivec4: dims, region_origin,
	// atlas_bricks). Contents refresh every run(); the RID is stable so the uniform set
	// survives across frames -- cached against the RID below, never rebuilt per frame.
	region_ubo_ = rd->uniform_buffer_create(48u);
	capacity_ = max_blades;
	brick_capacity_ = max_bricks;
	return instances_.is_valid() && brick_list_.is_valid() && counters_.is_valid() &&
			dispatch_args_.is_valid() && draw_args_.is_valid() && params_ubo_.is_valid() &&
			region_ubo_.is_valid();
}

bool GrassScatterPass::ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo) {
	// Stage-1 set: 0 grass-params, 1 brick_list, 2 counters, 3 dispatch_args, 4 region_map,
	// 5 region_tables, 6 brick_flags, 7 sdf_atlas, 8 mat_atlas, 9 palette_buf, 10 region UBO.
	// Texture/sampler RIDs come from GpuAtlas through the same accessors RaymarchPass uses;
	// the set is cached against every RID, SsaoPass-style, so it rebuilds only when a
	// backing resource is recreated. (The owned samplers never change after initialize.)
	if (bricks_uset_.is_valid() && key_params_ == params_ubo_ &&
			key_bricks_ == brick_list_ && key_counters_ == counters_ &&
			key_dispatch_ == dispatch_args_ && key_rmap_ == atlas.region_map() &&
			key_rtables_ == atlas.region_tables() && key_bflags_ == atlas.brick_flags() &&
			key_sdf_ == atlas.sdf_atlas() && key_mat_ == atlas.mat_atlas() &&
			key_palette_ == atlas.palette() && key_region_ == region_ubo_) {
		// Bricks set is current; fall through to check the scatter set below.
	} else {
		if (bricks_uset_.is_valid()) rd->free_rid(bricks_uset_);
		bricks_uset_ = RID();
		Ref<RDUniform> u[11];
		for (Ref<RDUniform> &item : u) item.instantiate();
		u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
		u[0]->set_binding(0);
		u[0]->add_id(params_ubo_);
		const RID stage1_buffers[5] = {brick_list_, counters_, dispatch_args_,
				atlas.region_map(), atlas.region_tables()};
		for (int i = 1; i <= 5; i++) {
			u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
			u[i]->set_binding(i);
			u[i]->add_id(stage1_buffers[i - 1]);
		}
		u[6]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u[6]->set_binding(6);
		u[6]->add_id(atlas.brick_flags());
		// Bindings 7-8 are the atlas textures the sampling helpers declare but stage 1
		// never fetches; a declared binding still has to be provided.
		u[7]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u[7]->set_binding(7);
		u[7]->add_id(sampler_linear_);
		u[7]->add_id(atlas.sdf_atlas());
		u[8]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u[8]->set_binding(8);
		u[8]->add_id(sampler_nearest_);
		u[8]->add_id(atlas.mat_atlas());
		u[9]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u[9]->set_binding(9);
		u[9]->add_id(atlas.palette());
		u[10]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
		u[10]->set_binding(10);
		u[10]->add_id(region_ubo_);
		Array uset_args;
		for (int i = 0; i < 11; i++) uset_args.push_back(u[i]);
		bricks_uset_ = rd->uniform_set_create(uset_args, bricks_shader_, 0);
		if (!bricks_uset_.is_valid()) return false;
		key_params_ = params_ubo_;
		key_bricks_ = brick_list_;
		key_counters_ = counters_;
		key_dispatch_ = dispatch_args_;
		key_rmap_ = atlas.region_map();
		key_rtables_ = atlas.region_tables();
		key_bflags_ = atlas.brick_flags();
		key_sdf_ = atlas.sdf_atlas();
		key_mat_ = atlas.mat_atlas();
		key_palette_ = atlas.palette();
		key_region_ = region_ubo_;
	}
	// Stage-2 set mirrors grass_scatter.comp.glsl bindings 0-13: 0 grass-params, 1
	// brick_list, 2 counters, 3 draw_args, 4 region_map, 5 region_tables, 6 brick_flags,
	// 7 palette_buf, 8 sdf_atlas, 9 mat_atlas, 10 region UBO, 11 instances, 12
	// region_slot_counts, 13 the frame's SunUbo -- the last two feed the sun march. Binding 3 is
	// draw_args_ here, NOT dispatch_args_ (that is stage 1's binding 3): separate sets
	// against separate shaders, so the locals are named after the buffers.
	if (!scatter_shader_.is_valid()) return true; // scatter stage absent: cull only.
	if (!sun_ubo.is_valid() || !atlas.region_slot_counts().is_valid()) return false;
	if (scatter_uset_.is_valid() && key_sparams_ == params_ubo_ &&
			key_sbricks_ == brick_list_ && key_scounters_ == counters_ &&
			key_sdraw_ == draw_args_ && key_srmap_ == atlas.region_map() &&
			key_srtables_ == atlas.region_tables() && key_sbflags_ == atlas.brick_flags() &&
			key_spalette_ == atlas.palette() && key_ssdf_ == atlas.sdf_atlas() &&
			key_smat_ == atlas.mat_atlas() && key_sregion_ == region_ubo_ &&
			key_sinstances_ == instances_ &&
			key_sslot_counts_ == atlas.region_slot_counts() && key_ssun_ == sun_ubo)
		return true;
	if (scatter_uset_.is_valid()) rd->free_rid(scatter_uset_);
	scatter_uset_ = RID();
	Ref<RDUniform> su[14];
	for (Ref<RDUniform> &item : su) item.instantiate();
	su[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	su[0]->set_binding(0);
	su[0]->add_id(params_ubo_);
	const RID scatter_buffers[6] = {brick_list_, counters_, draw_args_,
			atlas.region_map(), atlas.region_tables(), atlas.brick_flags()};
	for (int i = 1; i <= 6; i++) {
		su[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		su[i]->set_binding(i);
		su[i]->add_id(scatter_buffers[i - 1]);
	}
	su[7]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	su[7]->set_binding(7);
	su[7]->add_id(atlas.palette());
	su[8]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	su[8]->set_binding(8);
	su[8]->add_id(sampler_linear_);
	su[8]->add_id(atlas.sdf_atlas());
	su[9]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	su[9]->set_binding(9);
	su[9]->add_id(sampler_nearest_);
	su[9]->add_id(atlas.mat_atlas());
	su[10]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	su[10]->set_binding(10);
	su[10]->add_id(region_ubo_);
	su[11]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	su[11]->set_binding(11);
	su[11]->add_id(instances_);
	su[12]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	su[12]->set_binding(12);
	su[12]->add_id(atlas.region_slot_counts());
	su[13]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	su[13]->set_binding(13);
	su[13]->add_id(sun_ubo);
	Array scatter_args;
	for (int i = 0; i < 14; i++) scatter_args.push_back(su[i]);
	scatter_uset_ = rd->uniform_set_create(scatter_args, scatter_shader_, 0);
	if (!scatter_uset_.is_valid()) return false;
	key_sparams_ = params_ubo_;
	key_sbricks_ = brick_list_;
	key_scounters_ = counters_;
	key_sdraw_ = draw_args_;
	key_srmap_ = atlas.region_map();
	key_srtables_ = atlas.region_tables();
	key_sbflags_ = atlas.brick_flags();
	key_spalette_ = atlas.palette();
	key_ssdf_ = atlas.sdf_atlas();
	key_smat_ = atlas.mat_atlas();
	key_sregion_ = region_ubo_;
	key_sinstances_ = instances_;
	key_sslot_counts_ = atlas.region_slot_counts();
	key_ssun_ = sun_ubo;
	return true;
}

bool GrassScatterPass::run(RenderingDevice *rd, GpuAtlas &atlas,
		const ve::GrassLayout &layout, const ve::RegionWindow &region_win,
		float time_seconds, RID sun_ubo) {
	last_brick_count_ = 0;
	last_blade_count_ = 0;
	if (!rd_ || rd != rd_ || !bricks_pipeline_.is_valid()) return false;
	if (layout.max_bricks <= 0 || layout.params.limits[0] <= 0) {
		// Disabled: clear the GPU counters AND the indirect draw args, not just the CPU
		// mirrors zeroed above. debug_grass_stats() re-reads the GPU counters after its own
		// submit+sync, so stale counters would report the previous frame's counts;
		// stale draw args are worse -- the raster would re-draw the previous frame's
		// frozen blades while the CPU reports zero. An indirect draw with vertex_count
		// 0 is a no-op, so the stale params-UBO time needs no clearing.
		// A successful no-op, not a failure. The caller still ends its timing marker.
		PackedByteArray zero_gpu;
		zero_gpu.resize(16);
		zero_gpu.fill(0);
		if (counters_.is_valid()) rd->buffer_update(counters_, 0, 16, zero_gpu);
		if (draw_args_.is_valid()) rd->buffer_update(draw_args_, 0, 16, zero_gpu);
		return true;
	}
	if (!ensure_buffers(rd, layout.params.limits[0], layout.max_bricks)) return false;
	if (!ensure_uniform_sets(rd, atlas, sun_ubo)) return false;

	ve::GrassParams params = layout.params;
	params.wind[3] = time_seconds;
	PackedByteArray ubo;
	ubo.resize(sizeof(ve::GrassParams));
	std::memcpy(ubo.ptrw(), &params, sizeof(ve::GrassParams));
	rd->buffer_update(params_ubo_, 0, ubo.size(), ubo);

	// Refresh the pass-owned region window from the LIVE residency-backed window the
	// caller threads through (world_->region_window(), same source debug_ssao_probe
	// uses): atlas.config().region_window is init-centred/stale, so grass would vanish
	// away from the origin. The UBO RID is stable, so the cached uniform set survives;
	// only contents change.
	{
		const ve::RegionWindow &win = region_win;
		const ve::IVec3 ab = atlas.config().atlas_bricks;
		PackedByteArray rb;
		rb.resize(48);
		int32_t *w = reinterpret_cast<int32_t *>(rb.ptrw());
		w[0] = win.dim; w[1] = win.dim; w[2] = win.dim; w[3] = 0;
		w[4] = win.origin.x; w[5] = win.origin.y; w[6] = win.origin.z; w[7] = 0;
		w[8] = ab.x; w[9] = ab.y; w[10] = ab.z; w[11] = 0;
		rd->buffer_update(region_ubo_, 0, 48, rb);
	}

	// Clear the counters explicitly. A fresh RD buffer reads back as zero on this machine,
	// so "the count was zero" must mean the pass wrote zero -- never that nobody wrote.
	PackedByteArray zero;
	zero.resize(16);
	zero.fill(0);
	rd->buffer_update(counters_, 0, 16, zero);
	// Same for the indirect dispatch args: stage 1 grows dispatch_args.x with atomicMax,
	// which assumes a zero base. Thread 0 seeds y/z in-shader; x is seeded here because a
	// store would race the atomics.
	PackedByteArray zero_dispatch;
	zero_dispatch.resize(12);
	zero_dispatch.fill(0);
	rd->buffer_update(dispatch_args_, 0, 12, zero_dispatch);
	// Same for the indirect draw args: stage 2 grows vertex_count with atomicMax, which
	// assumes a zero base, and the raster would otherwise draw last frame's blades.
	PackedByteArray zero_args;
	zero_args.resize(16);
	zero_args.fill(0);
	rd->buffer_update(draw_args_, 0, 16, zero_args);

	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, bricks_pipeline_);
	rd->compute_list_bind_uniform_set(list, bricks_uset_, 0);
	rd->compute_list_dispatch(list, (layout.max_bricks + 63) / 64, 1, 1);
	if (scatter_pipeline_.is_valid()) {
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, scatter_pipeline_);
		rd->compute_list_bind_uniform_set(list, scatter_uset_, 0);
		rd->compute_list_dispatch_indirect(list, dispatch_args_, 0);
	}
	rd->compute_list_end();
	read_back_counters(rd);
	return true;
}

void GrassScatterPass::read_back_counters(RenderingDevice *rd) {
	const PackedByteArray data = rd->buffer_get_data(counters_, 0, 16);
	if (data.size() < 16) return;
	const uint32_t *c = reinterpret_cast<const uint32_t *>(data.ptr());
	last_brick_count_ = static_cast<int>(c[0]);
	last_blade_count_ = static_cast<int>(std::min<uint32_t>(c[1], static_cast<uint32_t>(capacity_)));
	blade_high_water_ = std::max(blade_high_water_, static_cast<int>(c[1]));
	if (static_cast<int>(c[1]) > capacity_ && !overflow_logged_) {
		overflow_logged_ = true;
		UtilityFunctions::printerr("GrassScatterPass: blade buffer overflow, wanted ",
				static_cast<int>(c[1]), " of ", capacity_,
				"; blades were dropped. Lower blades_per_brick or raise max_blades.");
	}
}

void GrassScatterPass::read_back_sample(RenderingDevice *rd) {
	sample_min_normal_y_ = 1.0f;
	sample_max_height_ = 0.0f;
	sample_min_sun_ = 1.0f;
	sample_max_sun_ = 0.0f;
	sample_mean_sun_ = 0.0f;
	sample_count_ = std::min(last_blade_count_, 4096);
	if (sample_count_ <= 0) return;
	const PackedByteArray data = rd->buffer_get_data(instances_, 0,
			static_cast<uint32_t>(sample_count_) * 32u);
	if (data.size() < sample_count_ * 32) { sample_count_ = 0; return; }
	const float *f = reinterpret_cast<const float *>(data.ptr());
	for (int i = 0; i < sample_count_; i++) {
		const float *a = f + i * 8;      // xyz position, w height
		// b.x is grass_pack_ground(): the oct normal in the low 16 bits, the terrain sun
		// visibility byte in bits 16-23 (shaders/grass_blade.glslh).
		const uint32_t packed = static_cast<uint32_t>(a[4]);
		// oct_decode_snorm8's inverse for the y component only: the pass stores the ground
		// normal, and all this assertion needs is which way it points.
		const float ny = ve::oct_decode_y_snorm8(static_cast<uint16_t>(packed & 0xFFFFu));
		sample_min_normal_y_ = std::min(sample_min_normal_y_, ny);
		sample_max_height_ = std::max(sample_max_height_, a[3]);
		const float sun = static_cast<float>((packed >> 16) & 0xFFu) / 255.0f;
		sample_min_sun_ = std::min(sample_min_sun_, sun);
		sample_max_sun_ = std::max(sample_max_sun_, sun);
		sample_mean_sun_ += sun / static_cast<float>(sample_count_);
	}
}
