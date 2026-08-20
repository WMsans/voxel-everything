#include "render/consolidate_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

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
PackedByteArray zeroed(int64_t n) {
	PackedByteArray b;
	b.resize(n);
	b.fill(0);
	return b;
}
constexpr int kSdfStride = 4916;
constexpr int kMatStride = 4096;
}

ConsolidatePass::~ConsolidatePass() { teardown(); }

bool ConsolidatePass::initialize(RenderingDevice *rd, OverridePool *pool, int max_bricks) {
	teardown();
	if (!rd || !pool || !pool->is_valid() || max_bricks <= 0) return false;
	rd_ = rd;
	pool_ = pool;
	max_bricks_ = max_bricks;
	ops_ = rd_->storage_buffer_create(ve::kMaxRegionOps * 32, zeroed(ve::kMaxRegionOps * 32));
	jobs_ = rd_->storage_buffer_create(static_cast<uint32_t>(max_bricks_) * 32,
			zeroed(static_cast<int64_t>(max_bricks_) * 32));
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/brick_consolidate.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("ConsolidatePass: shader load failed: ", err.c_str());
		teardown();
		return false;
	}
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd_->shader_compile_spirv_from_source(source);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("ConsolidatePass: ", compile_err);
		teardown();
		return false;
	}
	shader_ = rd_->shader_create_from_spirv(spirv);
	pipeline_ = shader_.is_valid() ? rd_->compute_pipeline_create(shader_) : RID();
	if (!pipeline_.is_valid()) {
		teardown();
		return false;
	}
	uset_ = rd_->uniform_set_create(Array::make(
			storage(0, pool_->sdf_buffer()), storage(1, pool_->mat_buffer()), storage(3, pool_->sdf_buffer()),
			storage(4, pool_->mat_buffer()), storage(5, jobs_), storage(6, pool_->tables()),
			storage(7, pool_->region_table_map()), storage(8, ops_)), shader_, 0);
	if (!uset_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void ConsolidatePass::teardown() {
	if (!rd_) return;
	free_if_valid(rd_, uset_);
	free_if_valid(rd_, pipeline_);
	free_if_valid(rd_, shader_);
	free_if_valid(rd_, jobs_);
	free_if_valid(rd_, ops_);
	rd_ = nullptr;
	pool_ = nullptr;
	max_bricks_ = 0;
}

bool ConsolidatePass::run(const ConsolidateJob &job, ConsolidateResult *out) {
	if (out) {
		out->region = job.region;
		out->bricks = job.bricks;
		out->baked.clear();
		out->failed = true;
	}
	if (!is_valid() || !out || job.bricks.empty() || static_cast<int>(job.bricks.size()) > max_bricks_ ||
			static_cast<int>(job.bricks.size()) > pool_->capacity() ||
			static_cast<int>(job.ops.size()) > ve::kMaxRegionOps)
		return false;
	const int n = static_cast<int>(job.bricks.size());
	PackedByteArray jb;
	jb.resize(static_cast<int64_t>(n) * 8 * 4);
	int32_t *j = reinterpret_cast<int32_t *>(jb.ptrw());
	for (int i = 0; i < n; i++) {
		j[i * 8 + 0] = job.bricks[static_cast<size_t>(i)].x;
		j[i * 8 + 1] = job.bricks[static_cast<size_t>(i)].y;
		j[i * 8 + 2] = job.bricks[static_cast<size_t>(i)].z;
		// The staging slots are disjoint from the low slots used by OverrideStore in the
		// normal path. This also leaves the old table intact until the caller publishes it.
		j[i * 8 + 3] = pool_->capacity() - n + i;
	}
	rd_->buffer_update(jobs_, 0, static_cast<uint32_t>(jb.size()), jb);
	if (!job.ops.empty()) {
		PackedByteArray ob;
		ob.resize(static_cast<int64_t>(job.ops.size()) * 32);
		std::memcpy(ob.ptrw(), job.ops.data(), static_cast<size_t>(ob.size()));
		rd_->buffer_update(ops_, 0, static_cast<uint32_t>(ob.size()), ob);
	}
	PackedByteArray pc;
	pc.resize(16);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = n;
	p[1] = job.region_slot;
	p[2] = static_cast<int>(job.ops.size());
	p[3] = pool_->region_table(job.region_slot);
	const int64_t list = rd_->compute_list_begin();
	rd_->compute_list_bind_compute_pipeline(list, pipeline_);
	rd_->compute_list_bind_uniform_set(list, uset_, 0);
	rd_->compute_list_set_push_constant(list, pc, pc.size());
	rd_->compute_list_dispatch(list, static_cast<uint32_t>(n), 1, 1);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();
	out->baked.resize(static_cast<size_t>(n));
	for (int i = 0; i < n; i++) {
		const int slot = pool_->capacity() - n + i;
		const PackedByteArray sb = rd_->buffer_get_data(pool_->sdf_buffer(),
				static_cast<uint32_t>(slot * kSdfStride), kSdfStride);
		const PackedByteArray mb = rd_->buffer_get_data(pool_->mat_buffer(),
				static_cast<uint32_t>(slot * kMatStride), kMatStride);
		if (sb.size() < kSdfStride || mb.size() < kMatStride) {
			out->baked.clear();
			return false;
		}
		std::memcpy(out->baked[static_cast<size_t>(i)].sdf, sb.ptr(), ve::kBrickSdfCount);
		std::memcpy(out->baked[static_cast<size_t>(i)].mat, mb.ptr(), ve::kBrickVoxelCount);
	}
	out->failed = false;
	return true;
}
