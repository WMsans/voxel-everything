#include "render/lod_build_pass.h"
#include "lod/lod_contour.h"
#include "lod/lod_skirt.h"
#include "render/shader_loader.h"
#include "world/edit_log.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/time.hpp>
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

Ref<RDUniform> image(int binding, RID rid) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u->set_binding(binding);
	u->add_id(rid);
	return u;
}

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

PackedByteArray zeroed(int64_t bytes) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(0);
	return b;
}

PackedByteArray filled(int64_t bytes, uint8_t value) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(value);
	return b;
}

// Groups for a dispatch of `n` threads per axis at local size 4.
int groups(int n) { return (n + 3) / 4; }

constexpr int kLodChunkLatticeCount =
		ve::kLodChunkLattice * ve::kLodChunkLattice * ve::kLodChunkLattice;

int sanitized_op_count(const LodBuildJob &job) {
	if (job.ops.empty()) return 0;
	return std::min<int>(static_cast<int>(job.ops.size()), ve::kMaxRegionOps);
}

} // namespace

LodBuildPass::~LodBuildPass() {
	teardown();
}

bool LodBuildPass::build(RenderingDevice *rd, const char *res_path, RID *shader,
		RID *pipeline) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(String(res_path));
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("LodBuildPass: ", res_path, " load failed: ", err.c_str());
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err =
			spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("LodBuildPass: ", res_path, ": ", compile_err);
		return false;
	}
	*shader = rd->shader_create_from_spirv(spirv);
	if (!shader->is_valid()) return false;
	*pipeline = rd->compute_pipeline_create(*shader);
	return pipeline->is_valid();
}

bool LodBuildPass::initialize(RenderingDevice *rd, const LodBuildConfig &cfg) {
	teardown();
	rd_ = rd;
	cfg_ = cfg;
	if (!rd || cfg.max_jobs <= 0) {
		UtilityFunctions::printerr("LodBuildPass: degenerate configuration");
		return false;
	}

	auto make_3d = [&](RID *rid, RenderingDevice::DataFormat format, int dim) {
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
		f->set_format(format);
		f->set_width(dim);
		f->set_height(dim);
		f->set_depth(dim);
		f->set_mipmaps(1);
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		*rid = rd->texture_create(f, v, TypedArray<PackedByteArray>());
	};
	make_3d(&fine_sdf_, RenderingDevice::DATA_FORMAT_R8_UNORM, ve::kLodFineLattice);
	make_3d(&fine_mat_, RenderingDevice::DATA_FORMAT_R16_UINT, ve::kLodFineLattice);
	make_3d(&lat_sdf_, RenderingDevice::DATA_FORMAT_R8_UNORM, ve::kLodChunkLattice);
	make_3d(&lat_mat_, RenderingDevice::DATA_FORMAT_R16_UINT, ve::kLodChunkLattice);

	const int64_t frac_count =
			static_cast<int64_t>(cfg_.max_jobs) * ve::kLodChunkMeshCells *
			ve::kLodChunkMeshCells * ve::kLodChunkMeshCells;
	frac_ = rd->storage_buffer_create(static_cast<uint32_t>(frac_count * 4),
			filled(frac_count * 4, 0xFF));
	const int64_t quads_bytes =
			static_cast<int64_t>(cfg_.max_jobs) * ve::kLodMaxQuadsPerChunk * 12;
	quads_ = rd->storage_buffer_create(static_cast<uint32_t>(quads_bytes),
			zeroed(quads_bytes));
	const int64_t counts_bytes = static_cast<int64_t>(cfg_.max_jobs) * 8;
	counts_ = rd->storage_buffer_create(static_cast<uint32_t>(counts_bytes),
			zeroed(counts_bytes));
	const int64_t ops_bytes =
			static_cast<int64_t>(cfg_.max_jobs) * ve::kMaxRegionOps * 32;
	ops_ = rd->storage_buffer_create(static_cast<uint32_t>(ops_bytes),
			zeroed(ops_bytes));

	if (!volumes_.initialize(rd, ve::kMaxVolumes, ve::kIslandDim)) {
		UtilityFunctions::printerr("LodBuildPass: volume pool creation failed");
		teardown();
		return false;
	}
	if (!fine_sdf_.is_valid() || !fine_mat_.is_valid() || !lat_sdf_.is_valid() ||
			!lat_mat_.is_valid() || !frac_.is_valid() || !quads_.is_valid() ||
			!counts_.is_valid() || !ops_.is_valid() || !volumes_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: buffer/texture creation failed");
		teardown();
		return false;
	}

	if (!build(rd, "res://shaders/lod_field.comp.glsl", &field_shader_, &field_pipeline_)) {
		teardown();
		return false;
	}
	field_uset_ = rd->uniform_set_create(Array::make(image(0, fine_sdf_), image(1, fine_mat_),
			storage(2, ops_), storage(3, volumes_.sdf_buffer()),
			storage(4, volumes_.mat_buffer())),
			field_shader_, 0);
	if (!field_uset_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: field uniform set creation failed");
		teardown();
		return false;
	}

	if (!build(rd, "res://shaders/lod_reduce.comp.glsl", &reduce_shader_, &reduce_pipeline_)) {
		teardown();
		return false;
	}
	reduce_uset_ = rd->uniform_set_create(Array::make(image(0, fine_sdf_), image(1, fine_mat_),
			image(2, lat_sdf_), image(3, lat_mat_)),
			reduce_shader_, 0);
	if (!reduce_uset_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: reduce uniform set creation failed");
		teardown();
		return false;
	}

	if (!build(rd, "res://shaders/lod_frac.comp.glsl", &frac_shader_, &frac_pipeline_)) {
		teardown();
		return false;
	}
	frac_uset_ = rd->uniform_set_create(Array::make(image(0, lat_sdf_), storage(1, frac_)),
			frac_shader_, 0);
	if (!frac_uset_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: frac uniform set creation failed");
		teardown();
		return false;
	}

	if (!build(rd, "res://shaders/lod_quads.comp.glsl", &quads_shader_, &quads_pipeline_)) {
		teardown();
		return false;
	}
	quads_uset_ = rd->uniform_set_create(Array::make(image(0, lat_sdf_), image(1, lat_mat_),
			storage(2, frac_), storage(3, quads_), storage(4, counts_)),
			quads_shader_, 0);
	if (!quads_uset_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: quads uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void LodBuildPass::teardown() {
	if (!rd_) return;
	if (in_flight_) {
		rd_->sync();
		in_flight_ = false;
		batch_.clear();
	}
	free_if_valid(rd_, quads_uset_);
	free_if_valid(rd_, quads_pipeline_);
	free_if_valid(rd_, quads_shader_);
	free_if_valid(rd_, frac_uset_);
	free_if_valid(rd_, frac_pipeline_);
	free_if_valid(rd_, frac_shader_);
	free_if_valid(rd_, reduce_uset_);
	free_if_valid(rd_, reduce_pipeline_);
	free_if_valid(rd_, reduce_shader_);
	free_if_valid(rd_, field_uset_);
	volumes_.teardown();
	free_if_valid(rd_, field_pipeline_);
	free_if_valid(rd_, field_shader_);
	free_if_valid(rd_, ops_);
	free_if_valid(rd_, counts_);
	free_if_valid(rd_, quads_);
	free_if_valid(rd_, frac_);
	free_if_valid(rd_, lat_mat_);
	free_if_valid(rd_, lat_sdf_);
	free_if_valid(rd_, fine_mat_);
	free_if_valid(rd_, fine_sdf_);
	rd_ = nullptr;
}

void LodBuildPass::reset_counts() {
	rd_->buffer_update(counts_, 0, static_cast<uint32_t>(cfg_.max_jobs) * 8,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * 8));
}

void LodBuildPass::upload_ops(const LodBuildJob &job, int job_index) {
	const int n = sanitized_op_count(job);
	if (n <= 0) return; // op_count in the push constant is what the shader reads
	PackedByteArray b;
	b.resize(static_cast<int64_t>(n) * 32);
	std::memcpy(b.ptrw(), job.ops.data(), static_cast<size_t>(n) * 32);
	rd_->buffer_update(ops_, static_cast<uint32_t>(job_index) * ve::kMaxRegionOps * 32,
			static_cast<uint32_t>(b.size()), b);
}

void LodBuildPass::push(int64_t list, const LodBuildJob &job, int job_index) {
	float origin[3];
	ve::lod_chunk_origin(job.level, job.coord, origin);
	const float cell = ve::lod_cell_size(job.level);
	PackedByteArray pc;
	pc.resize(48);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = job.coord.x;
	p[1] = job.coord.y;
	p[2] = job.coord.z;
	p[3] = job_index;
	p[4] = sanitized_op_count(job);
	p[5] = ve::kLodMaxQuadsPerChunk;
	p[6] = job.level;
	p[7] = 0;
	float *f = reinterpret_cast<float *>(pc.ptrw());
	f[8] = origin[0];
	f[9] = origin[1];
	f[10] = origin[2];
	f[11] = cell;
	rd_->compute_list_set_push_constant(list, pc, pc.size());
}

void LodBuildPass::record_field(int64_t list, const LodBuildJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, field_pipeline_);
	rd_->compute_list_bind_uniform_set(list, field_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kLodFineLattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_reduce(int64_t list, const LodBuildJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, reduce_pipeline_);
	rd_->compute_list_bind_uniform_set(list, reduce_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kLodChunkLattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_frac(int64_t list, const LodBuildJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, frac_pipeline_);
	rd_->compute_list_bind_uniform_set(list, frac_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kLodChunkMeshCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_quads(int64_t list, const LodBuildJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, quads_pipeline_);
	rd_->compute_list_bind_uniform_set(list, quads_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kLodChunkCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_job(int64_t list, const LodBuildJob &job, int job_index) {
	record_field(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_reduce(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_frac(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_quads(list, job, job_index);
	rd_->compute_list_add_barrier(list);
}

bool LodBuildPass::submit(const LodBuildJob *jobs, int count) {
	if (!is_valid() || in_flight_ || !jobs || count <= 0 || count > cfg_.max_jobs)
		return false;
	reset_counts();
	for (int j = 0; j < count; j++) upload_ops(jobs[j], j);
	const int64_t list = rd_->compute_list_begin();
	for (int j = 0; j < count; j++) record_job(list, jobs[j], j);
	rd_->compute_list_end();
	rd_->submit();
	in_flight_ = true;
	batch_.clear();
	for (int j = 0; j < count; j++) batch_.push_back(jobs[j]);
	return true;
}

void LodBuildPass::read_job(int job_index, LodBuildResult *out) {
	const LodBuildJob &job = batch_[job_index];
	out->level = job.level;
	out->coord = job.coord;
	out->quads.clear();
	out->overflow = false;
	out->failed = false;
	const PackedByteArray cb = rd_->buffer_get_data(counts_,
			static_cast<uint32_t>(job_index) * 8, 8);
	if (cb.size() < 8) {
		out->failed = true;
		return;
	}
	const uint32_t *c = reinterpret_cast<const uint32_t *>(cb.ptr());
	const int qcount = std::min<int>(static_cast<int>(c[0]), ve::kLodMaxQuadsPerChunk);
	out->overflow = c[1] != 0u;
	if (qcount > 0) {
		const PackedByteArray qb = rd_->buffer_get_data(quads_,
				static_cast<uint32_t>(job_index) * ve::kLodMaxQuadsPerChunk * 12,
				static_cast<uint32_t>(qcount) * 12);
		if (qb.size() < static_cast<int64_t>(qcount) * 12) {
			out->failed = true;
			out->quads.clear();
			return;
		}
		out->quads.resize(static_cast<size_t>(qcount));
		std::memcpy(out->quads.data(), qb.ptr(), static_cast<size_t>(qcount) * 12);
	}
	ve::lod_append_skirts(&out->quads);
}

int LodBuildPass::collect(std::vector<LodBuildResult> *out) {
	if (!in_flight_) return 0;
	rd_->sync();
	in_flight_ = false;
	const int n = static_cast<int>(batch_.size());
	for (int j = 0; j < n; j++) {
		LodBuildResult r;
		read_job(j, &r);
		if (out) out->push_back(std::move(r));
	}
	batch_.clear();
	return n;
}

bool LodBuildPass::build_sync(const LodBuildJob &job, LodBuildResult *out,
		std::vector<uint8_t> *lattice, std::vector<uint16_t> *material) {
	if (!is_valid() || in_flight_) return false;
	std::vector<LodBuildResult> results;
	if (!submit(&job, 1)) return false;
	if (collect(&results) != 1) return false;
	if (out) *out = std::move(results[0]);
	if (lattice) {
		const PackedByteArray data = rd_->texture_get_data(lat_sdf_, 0);
		if (data.size() < kLodChunkLatticeCount) return false;
		lattice->assign(data.ptr(), data.ptr() + kLodChunkLatticeCount);
	}
	if (material) {
		const PackedByteArray data = rd_->texture_get_data(lat_mat_, 0);
		if (data.size() < static_cast<int64_t>(kLodChunkLatticeCount) * 2) return false;
		material->resize(kLodChunkLatticeCount);
		std::memcpy(material->data(), data.ptr(),
				static_cast<size_t>(kLodChunkLatticeCount) * 2);
	}
	return true;
}
