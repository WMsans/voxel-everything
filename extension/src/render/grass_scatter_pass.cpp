#include "render/grass_scatter_pass.h"
#include "render/gpu_atlas.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
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
			&dispatch_args_, &instances_, &draw_args_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	key_params_ = key_bricks_ = key_counters_ = key_dispatch_ = RID();
	key_sparams_ = key_sbricks_ = key_scounters_ = key_sdispatch_ = RID();
	capacity_ = 0;
	brick_capacity_ = 0;
	last_brick_count_ = 0;
	last_blade_count_ = 0;
	rd_ = nullptr;
}

bool GrassScatterPass::ensure_buffers(RenderingDevice *rd, int max_blades, int max_bricks) {
	if (max_blades <= 0 || max_bricks <= 0) return false;
	if (instances_.is_valid() && capacity_ == max_blades && brick_capacity_ >= max_bricks)
		return true;
	for (RID *r : {&instances_, &brick_list_, &counters_, &dispatch_args_, &draw_args_,
			&params_ubo_, &bricks_uset_, &scatter_uset_}) {
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
	capacity_ = max_blades;
	brick_capacity_ = max_bricks;
	return instances_.is_valid() && brick_list_.is_valid() && counters_.is_valid() &&
			dispatch_args_.is_valid() && draw_args_.is_valid() && params_ubo_.is_valid();
}

bool GrassScatterPass::ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas) {
	(void)atlas; // Stage 1 reads no atlas buffers yet; Task 5 binds the brick tables here.
	if (bricks_uset_.is_valid() && key_params_ == params_ubo_ &&
			key_bricks_ == brick_list_ && key_counters_ == counters_ &&
			key_dispatch_ == dispatch_args_) {
		// Bricks set is current; fall through to check the scatter set below.
	} else {
		if (bricks_uset_.is_valid()) rd->free_rid(bricks_uset_);
		bricks_uset_ = RID();
		Ref<RDUniform> u[4];
		for (Ref<RDUniform> &item : u) item.instantiate();
		u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
		u[0]->set_binding(0);
		u[0]->add_id(params_ubo_);
		for (int i = 1; i < 4; i++) {
			u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
			u[i]->set_binding(i);
		}
		u[1]->add_id(brick_list_);
		u[2]->add_id(counters_);
		u[3]->add_id(dispatch_args_);
		bricks_uset_ = rd->uniform_set_create(
				Array::make(u[0], u[1], u[2], u[3]), bricks_shader_, 0);
		if (!bricks_uset_.is_valid()) return false;
		key_params_ = params_ubo_;
		key_bricks_ = brick_list_;
		key_counters_ = counters_;
		key_dispatch_ = dispatch_args_;
	}
	if (!scatter_shader_.is_valid()) return true; // Task 6 owns the scatter set shape.
	if (scatter_uset_.is_valid() && key_sparams_ == params_ubo_ &&
			key_sbricks_ == brick_list_ && key_scounters_ == counters_ &&
			key_sdispatch_ == dispatch_args_)
		return true;
	if (scatter_uset_.is_valid()) rd->free_rid(scatter_uset_);
	scatter_uset_ = RID();
	Ref<RDUniform> u[4];
	for (Ref<RDUniform> &item : u) item.instantiate();
	u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[0]->set_binding(0);
	u[0]->add_id(params_ubo_);
	for (int i = 1; i < 4; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u[i]->set_binding(i);
	}
	u[1]->add_id(brick_list_);
	u[2]->add_id(counters_);
	u[3]->add_id(dispatch_args_);
	scatter_uset_ = rd->uniform_set_create(
			Array::make(u[0], u[1], u[2], u[3]), scatter_shader_, 0);
	if (!scatter_uset_.is_valid()) return false;
	key_sparams_ = params_ubo_;
	key_sbricks_ = brick_list_;
	key_scounters_ = counters_;
	key_sdispatch_ = dispatch_args_;
	return true;
}

bool GrassScatterPass::run(RenderingDevice *rd, GpuAtlas &atlas,
		const ve::GrassLayout &layout, float time_seconds) {
	last_brick_count_ = 0;
	last_blade_count_ = 0;
	if (!rd_ || rd != rd_ || !bricks_pipeline_.is_valid()) return false;
	if (layout.max_bricks <= 0 || layout.params.limits[0] <= 0) return true; // disabled: a
	// successful no-op, not a failure. The caller still ends its timing marker.
	if (!ensure_buffers(rd, layout.params.limits[0], layout.max_bricks)) return false;
	if (!ensure_uniform_sets(rd, atlas)) return false;

	ve::GrassParams params = layout.params;
	params.wind[3] = time_seconds;
	PackedByteArray ubo;
	ubo.resize(sizeof(ve::GrassParams));
	std::memcpy(ubo.ptrw(), &params, sizeof(ve::GrassParams));
	rd->buffer_update(params_ubo_, 0, ubo.size(), ubo);

	// Clear the counters explicitly. A fresh RD buffer reads back as zero on this machine,
	// so "the count was zero" must mean the pass wrote zero -- never that nobody wrote.
	PackedByteArray zero;
	zero.resize(16);
	zero.fill(0);
	rd->buffer_update(counters_, 0, 16, zero);

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
}
