#include "render/island_extract_pass.h"
#include "render/shader_loader.h"
#include "render/volume_pool.h"
#include "world/edit_log.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
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

PackedByteArray zeroed(int64_t bytes) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(0);
	return b;
}

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

} // namespace

IslandExtractPass::~IslandExtractPass() {
	teardown();
}

bool IslandExtractPass::initialize(RenderingDevice *rd, const VolumePool *volumes) {
	teardown();
	if (!rd || !volumes || !volumes->is_valid()) return false;
	rd_ = rd;
	const int64_t voxels = static_cast<int64_t>(ve::kIslandVoxelCount);
	out_ = rd->storage_buffer_create(static_cast<uint32_t>(voxels * 4), zeroed(voxels * 4));
	boxes_ = rd->storage_buffer_create(ve::kMaxIslandBoxes * 32,
			zeroed(ve::kMaxIslandBoxes * 32));
	counts_ = rd->storage_buffer_create(16, zeroed(16));
	ops_ = rd->storage_buffer_create(ve::kMaxRegionOps * 32, zeroed(ve::kMaxRegionOps * 32));
	if (!out_.is_valid() || !boxes_.is_valid() || !counts_.is_valid() || !ops_.is_valid()) {
		UtilityFunctions::printerr("IslandExtractPass: buffer creation failed");
		teardown();
		return false;
	}

	// Same loader path as MeshPass::build: read the file, strip #[compute], compile, report.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/island_extract.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("IslandExtractPass: load failed: ", err.c_str());
		teardown();
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String cerr = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!cerr.is_empty()) {
		UtilityFunctions::printerr("IslandExtractPass: ", cerr);
		teardown();
		return false;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	pipeline_ = shader_.is_valid() ? rd->compute_pipeline_create(shader_) : RID();
	if (!pipeline_.is_valid()) {
		teardown();
		return false;
	}
	uset_ = rd->uniform_set_create(Array::make(storage(0, out_), storage(1, ops_),
			storage(2, volumes->sdf_buffer()), storage(3, volumes->mat_buffer()),
			storage(4, boxes_), storage(5, counts_)), shader_, 0);
	if (!uset_.is_valid()) {
		UtilityFunctions::printerr("IslandExtractPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void IslandExtractPass::teardown() {
	// Uniform sets first: freeing a shader cascades to its pipelines and referencing sets.
	free_if_valid(rd_, uset_);
	free_if_valid(rd_, pipeline_);
	free_if_valid(rd_, shader_);
	free_if_valid(rd_, ops_);
	free_if_valid(rd_, counts_);
	free_if_valid(rd_, boxes_);
	free_if_valid(rd_, out_);
	rd_ = nullptr;
}

bool IslandExtractPass::extract(const IslandExtractJob &job, IslandExtractResult *out) {
	out->id = job.id;
	out->failed = true;
	out->data = ve::VolumeData{};
	if (!is_valid() || job.dim < 2 || job.dim > ve::kIslandDim || job.voxel <= 0.0f)
		return false;
	const int box_count = std::min(static_cast<int>(job.boxes.size()), ve::kMaxIslandBoxes);
	// A partial op list would evaluate the wrong field and could carve a shape that does not
	// match the component. Fail the extraction rather than silently clamping to the GPU pool
	// capacity.
	const int op_count = static_cast<int>(job.ops.size());
	if (op_count > ve::kMaxRegionOps) return false;

	// Device-level commands, all before compute_list_begin (M2 Task 12's ordering rule).
	rd_->buffer_update(counts_, 0, 16, zeroed(16));
	if (op_count > 0) {
		PackedByteArray b;
		b.resize(static_cast<int64_t>(op_count) * 32);
		std::memcpy(b.ptrw(), job.ops.data(), static_cast<size_t>(op_count) * 32);
		rd_->buffer_update(ops_, 0, static_cast<uint32_t>(b.size()), b);
	}
	if (box_count > 0) {
		PackedByteArray b;
		b.resize(static_cast<int64_t>(box_count) * 32);
		float *f = reinterpret_cast<float *>(b.ptrw());
		for (int i = 0; i < box_count; i++) {
			float lo[3], hi[3];
			job.boxes[static_cast<size_t>(i)].world_aabb(lo, hi);
			f[i * 8 + 0] = lo[0]; f[i * 8 + 1] = lo[1]; f[i * 8 + 2] = lo[2]; f[i * 8 + 3] = 0.0f;
			f[i * 8 + 4] = hi[0]; f[i * 8 + 5] = hi[1]; f[i * 8 + 6] = hi[2]; f[i * 8 + 7] = 0.0f;
		}
		rd_->buffer_update(boxes_, 0, static_cast<uint32_t>(b.size()), b);
	}

	PackedByteArray pc;
	pc.resize(32);
	float *pf = reinterpret_cast<float *>(pc.ptrw());
	int32_t *pi = reinterpret_cast<int32_t *>(pc.ptrw());
	pf[0] = job.origin[0];
	pf[1] = job.origin[1];
	pf[2] = job.origin[2];
	pf[3] = job.voxel;
	pi[4] = job.dim;
	pi[5] = op_count;
	pi[6] = box_count;
	pi[7] = 0;

	const int64_t list = rd_->compute_list_begin();
	rd_->compute_list_bind_compute_pipeline(list, pipeline_);
	rd_->compute_list_bind_uniform_set(list, uset_, 0);
	rd_->compute_list_set_push_constant(list, pc, pc.size());
	const int g = (job.dim + 3) / 4;
	rd_->compute_list_dispatch(list, g, g, g);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();

	const int64_t voxels = static_cast<int64_t>(job.dim) * job.dim * job.dim;
	const PackedByteArray data =
			rd_->buffer_get_data(out_, 0, static_cast<uint32_t>(voxels * 4));
	if (data.size() < voxels * 4) {
		UtilityFunctions::printerr("IslandExtractPass: short readback");
		return false;
	}
	const uint32_t *w = reinterpret_cast<const uint32_t *>(data.ptr());
	out->data.dim = job.dim;
	out->data.sdf.resize(static_cast<size_t>(voxels));
	out->data.mat.resize(static_cast<size_t>(voxels));
	out->data.solid_voxels = 0;
	for (int64_t i = 0; i < voxels; i++) {
		out->data.sdf[static_cast<size_t>(i)] = static_cast<uint8_t>(w[i] & 0xFFu);
		out->data.mat[static_cast<size_t>(i)] = static_cast<uint8_t>((w[i] >> 8) & 0xFFu);
	}
	const PackedByteArray cb = rd_->buffer_get_data(counts_, 0, 16);
	if (cb.size() >= 16)
		out->data.solid_voxels =
				static_cast<int>(reinterpret_cast<const uint32_t *>(cb.ptr())[0]);
	out->failed = false;
	return true;
}
