#include "render/region_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

Ref<RDUniform> storage(int binding, RID rid) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u->set_binding(binding);
	u->add_id(rid);
	return u;
}

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

} // namespace

RegionPass::~RegionPass() {
	teardown();
}

bool RegionPass::build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(String(res_path));
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("RegionPass: ", res_path, " load failed: ", err.c_str());
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("RegionPass: ", res_path, ": ", compile_err);
		return false;
	}
	*shader = rd->shader_create_from_spirv(spirv);
	if (!shader->is_valid()) return false;
	*pipeline = rd->compute_pipeline_create(*shader);
	return pipeline->is_valid();
}

bool RegionPass::initialize(RenderingDevice *rd, const GpuAtlas &atlas) {
	teardown();
	rd_ = rd;
	max_brick_jobs_ = atlas.config().max_brick_jobs;
	if (!build(rd, "res://shaders/brick_mark.comp.glsl", &mark_shader_, &mark_pipeline_) ||
			!build(rd, "res://shaders/region_free.comp.glsl", &free_shader_, &free_pipeline_) ||
			!build(rd, "res://shaders/dispatch_args.comp.glsl", &args_shader_, &args_pipeline_)) {
		teardown();
		return false;
	}
	// The atlas buffers never change identity, so the uniform sets are built once.
	mark_uset_ = rd->uniform_set_create(
			Array::make(storage(0, atlas.region_tables()), storage(1, atlas.free_list()),
					storage(2, atlas.counters()), storage(3, atlas.frame_counters()),
					storage(4, atlas.op_pool()), storage(5, atlas.jobs())),
			mark_shader_, 0);
	free_uset_ = rd->uniform_set_create(
			Array::make(storage(0, atlas.region_tables()), storage(1, atlas.free_list()),
					storage(2, atlas.counters())),
			free_shader_, 0);
	args_uset_ = rd->uniform_set_create(
			Array::make(storage(0, atlas.frame_counters()), storage(1, atlas.dispatch_args())),
			args_shader_, 0);
	if (!mark_uset_.is_valid() || !free_uset_.is_valid() || !args_uset_.is_valid()) {
		UtilityFunctions::printerr("RegionPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void RegionPass::teardown() {
	if (!rd_) return;
	// Uniform sets first: freeing a shader cascades to its pipelines and referencing sets.
	free_if_valid(rd_, mark_uset_);
	free_if_valid(rd_, free_uset_);
	free_if_valid(rd_, args_uset_);
	free_if_valid(rd_, mark_pipeline_);
	free_if_valid(rd_, free_pipeline_);
	free_if_valid(rd_, args_pipeline_);
	free_if_valid(rd_, mark_shader_);
	free_if_valid(rd_, free_shader_);
	free_if_valid(rd_, args_shader_);
	rd_ = nullptr;
}

void RegionPass::mark(RenderingDevice *rd, int64_t list, ve::IVec3 region, int region_slot,
		ve::IVec3 lo, ve::IVec3 hi, int op_count, bool force_regen) {
	if (!mark_pipeline_.is_valid()) return;
	const int64_t total = static_cast<int64_t>(hi.x - lo.x + 1) * (hi.y - lo.y + 1) *
			(hi.z - lo.z + 1);
	if (total <= 0) return;
	const uint32_t groups = static_cast<uint32_t>((total + 255) / 256);

	PackedByteArray pc;
	pc.resize(64);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = region.x; p[1] = region.y; p[2] = region.z; p[3] = region_slot;
	p[4] = lo.x; p[5] = lo.y; p[6] = lo.z; p[7] = 0;
	p[8] = hi.x; p[9] = hi.y; p[10] = hi.z; p[11] = 0;
	p[12] = op_count; p[13] = 0; p[14] = max_brick_jobs_; p[15] = force_regen ? 1 : 0;

	rd->compute_list_bind_compute_pipeline(list, mark_pipeline_);
	rd->compute_list_bind_uniform_set(list, mark_uset_, 0);
	// Phase 0 (release) is only meaningful when bricks may have gone inactive, which only
	// an edit can cause. A plain stream-in scans a region whose table is entirely absent.
	if (force_regen) {
		p[13] = 0;
		rd->compute_list_set_push_constant(list, pc, pc.size());
		rd->compute_list_dispatch(list, groups, 1, 1);
		rd->compute_list_add_barrier(list);
	}
	p[13] = 1;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, groups, 1, 1);
}

void RegionPass::release_region(RenderingDevice *rd, int64_t list, int region_slot) {
	if (!free_pipeline_.is_valid()) return;
	PackedByteArray pc;
	pc.resize(16);
	reinterpret_cast<int32_t *>(pc.ptrw())[0] = region_slot;
	rd->compute_list_bind_compute_pipeline(list, free_pipeline_);
	rd->compute_list_bind_uniform_set(list, free_uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (ve::kRegionBrickCount + 255) / 256, 1, 1);
}

void RegionPass::write_dispatch_args(RenderingDevice *rd, int64_t list) {
	if (!args_pipeline_.is_valid()) return;
	rd->compute_list_bind_compute_pipeline(list, args_pipeline_);
	rd->compute_list_bind_uniform_set(list, args_uset_, 0);
	rd->compute_list_dispatch(list, 1, 1, 1);
}
