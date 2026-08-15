#include "render/mesh_pass.h"
#include "mesh/mesh_chunk.h"
#include "render/shader_loader.h"
#include "world/edit_log.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
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

// Groups for a dispatch of `n` threads per axis at local size 4.
int groups(int n) { return (n + 3) / 4; }

} // namespace

MeshPass::~MeshPass() {
	teardown();
}

bool MeshPass::build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(String(res_path));
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("MeshPass: ", res_path, " load failed: ", err.c_str());
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
		UtilityFunctions::printerr("MeshPass: ", res_path, ": ", compile_err);
		return false;
	}
	*shader = rd->shader_create_from_spirv(spirv);
	if (!shader->is_valid()) return false;
	*pipeline = rd->compute_pipeline_create(*shader);
	return pipeline->is_valid();
}

bool MeshPass::initialize(RenderingDevice *rd, const MeshPassConfig &cfg) {
	teardown();
	rd_ = rd;
	cfg_ = cfg;
	if (!rd || cfg.max_jobs <= 0 || cfg.max_verts <= 0 || cfg.max_tris <= 0) {
		UtilityFunctions::printerr("MeshPass: degenerate configuration");
		return false;
	}

	{
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
		f->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
		f->set_width(ve::kChunkLattice);
		f->set_height(ve::kChunkLattice);
		f->set_depth(ve::kChunkLattice);
		f->set_mipmaps(1);
		// STORAGE for the field pass to write and the mesher passes to read;
		// CAN_COPY_FROM so the differential test can read the lattice back.
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		lattice_ = rd->texture_create(f, v, TypedArray<PackedByteArray>());
	}
	cells_ = rd->storage_buffer_create(static_cast<uint32_t>(ve::kChunkCellCount) * 4,
			zeroed(static_cast<int64_t>(ve::kChunkCellCount) * 4));
	verts_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_jobs) * cfg_.max_verts * 12,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * cfg_.max_verts * 12));
	tris_ = rd->storage_buffer_create(static_cast<uint32_t>(cfg_.max_jobs) * cfg_.max_tris * 12,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * cfg_.max_tris * 12));
	counts_ = rd->storage_buffer_create(static_cast<uint32_t>(cfg_.max_jobs) * 16,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * 16));
	ops_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_jobs) * ve::kMaxRegionOps * 32,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * ve::kMaxRegionOps * 32));
	if (!lattice_.is_valid() || !cells_.is_valid() || !verts_.is_valid() || !tris_.is_valid() ||
			!counts_.is_valid() || !ops_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: buffer creation failed");
		teardown();
		return false;
	}

	if (!build(rd, "res://shaders/mesh_field.comp.glsl", &field_shader_, &field_pipeline_)) {
		teardown();
		return false;
	}
	field_uset_ = rd->uniform_set_create(Array::make(image(0, lattice_), storage(1, ops_)),
			field_shader_, 0);
	if (!field_uset_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: uniform set creation failed");
		teardown();
		return false;
	}

	if (!build(rd, "res://shaders/mesh_cells.comp.glsl", &cells_shader_, &cells_pipeline_) ||
			!build(rd, "res://shaders/mesh_quads.comp.glsl", &quads_shader_, &quads_pipeline_)) {
		teardown();
		return false;
	}
	cells_uset_ = rd->uniform_set_create(Array::make(image(0, lattice_), storage(1, cells_),
			storage(2, verts_), storage(3, counts_)), cells_shader_, 0);
	quads_uset_ = rd->uniform_set_create(Array::make(image(0, lattice_), storage(1, cells_),
			storage(2, tris_), storage(3, counts_)), quads_shader_, 0);
	if (!cells_uset_.is_valid() || !quads_uset_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void MeshPass::teardown() {
	if (!rd_) return;
	if (in_flight_) {
		rd_->sync();
		in_flight_ = false;
		batch_.clear();
	}
	// Uniform sets first: freeing a shader cascades to its pipelines and referencing sets
	// (M1's documented order).
	free_if_valid(rd_, quads_uset_);
	free_if_valid(rd_, quads_pipeline_);
	free_if_valid(rd_, quads_shader_);
	free_if_valid(rd_, cells_uset_);
	free_if_valid(rd_, cells_pipeline_);
	free_if_valid(rd_, cells_shader_);
	free_if_valid(rd_, field_uset_);
	free_if_valid(rd_, field_pipeline_);
	free_if_valid(rd_, field_shader_);
	free_if_valid(rd_, ops_);
	free_if_valid(rd_, counts_);
	free_if_valid(rd_, tris_);
	free_if_valid(rd_, verts_);
	free_if_valid(rd_, cells_);
	free_if_valid(rd_, lattice_);
	rd_ = nullptr;
}

void MeshPass::upload_ops(const MeshJob &job, int job_index) {
	const int n = job.ops ? std::min(job.op_count, ve::kMaxRegionOps) : 0;
	if (n <= 0) return; // op_count in the push constant is what the shader reads
	PackedByteArray b;
	b.resize(static_cast<int64_t>(n) * 32);
	std::memcpy(b.ptrw(), job.ops, static_cast<size_t>(n) * 32);
	rd_->buffer_update(ops_, static_cast<uint32_t>(job_index) * ve::kMaxRegionOps * 32,
			static_cast<uint32_t>(b.size()), b);
}

// The same 32-byte block for all three passes, so one helper serves them all.
void MeshPass::push(int64_t list, const MeshJob &job, int job_index) {
	PackedByteArray pc;
	pc.resize(32);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = job.chunk.x;
	p[1] = job.chunk.y;
	p[2] = job.chunk.z;
	p[3] = job_index;
	p[4] = std::min(job.op_count, ve::kMaxRegionOps);
	p[5] = cfg_.max_verts;
	p[6] = cfg_.max_tris;
	p[7] = 0;
	rd_->compute_list_set_push_constant(list, pc, pc.size());
}

void MeshPass::record_field(int64_t list, const MeshJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, field_pipeline_);
	rd_->compute_list_bind_uniform_set(list, field_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kChunkLattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

bool MeshPass::run_field_sync(const MeshJob &job, std::vector<uint8_t> *lattice) {
	if (!is_valid()) return false;
	upload_ops(job, 0);
	const int64_t list = rd_->compute_list_begin();
	record_field(list, job, 0);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();
	if (lattice) {
		const PackedByteArray data = rd_->texture_get_data(lattice_, 0);
		if (data.size() < ve::kChunkLatticeCount) return false;
		lattice->assign(data.ptr(), data.ptr() + ve::kChunkLatticeCount);
	}
	return true;
}

void MeshPass::record_cells(int64_t list, const MeshJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, cells_pipeline_);
	rd_->compute_list_bind_uniform_set(list, cells_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kChunkMeshCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

void MeshPass::record_quads(int64_t list, const MeshJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, quads_pipeline_);
	rd_->compute_list_bind_uniform_set(list, quads_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kChunkCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

// The three passes are strictly sequential, and so are the jobs in a batch: they share one
// lattice volume and one cell map. The barriers are what makes that safe — and what makes a
// batch cost three barriers per chunk rather than three buffers per chunk.
void MeshPass::record_job(int64_t list, const MeshJob &job, int job_index) {
	record_field(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_cells(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_quads(list, job, job_index);
	rd_->compute_list_add_barrier(list);
}

void MeshPass::reset_counts() {
	// Device-level, so it must precede compute_list_begin. One update covers the whole batch:
	// every job writes only its own four uints.
	rd_->buffer_update(counts_, 0, static_cast<uint32_t>(cfg_.max_jobs) * 16,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * 16));
}

void MeshPass::read_job(int job_index, ve::IVec3 chunk, MeshResult *out) {
	out->chunk = chunk;
	out->positions.clear();
	out->indices.clear();
	out->overflow = false;
	const PackedByteArray cb =
			rd_->buffer_get_data(counts_, static_cast<uint32_t>(job_index) * 16, 16);
	if (cb.size() < 16) return;
	const uint32_t *c = reinterpret_cast<const uint32_t *>(cb.ptr());
	// The counters are raw atomic totals: they run past the cap when it is hit.
	const int vcount = std::min<int>(static_cast<int>(c[0]), cfg_.max_verts);
	const int tcount = std::min<int>(static_cast<int>(c[1]), cfg_.max_tris);
	out->overflow = c[2] != 0u;
	if (vcount > 0) {
		const PackedByteArray vb = rd_->buffer_get_data(verts_,
				static_cast<uint32_t>(job_index) * cfg_.max_verts * 12,
				static_cast<uint32_t>(vcount) * 12);
		out->positions.resize(static_cast<size_t>(vcount) * 3);
		std::memcpy(out->positions.data(), vb.ptr(), static_cast<size_t>(vcount) * 12);
	}
	if (tcount > 0) {
		const PackedByteArray tb = rd_->buffer_get_data(tris_,
				static_cast<uint32_t>(job_index) * cfg_.max_tris * 12,
				static_cast<uint32_t>(tcount) * 12);
		out->indices.resize(static_cast<size_t>(tcount) * 3);
		std::memcpy(out->indices.data(), tb.ptr(), static_cast<size_t>(tcount) * 12);
	}
}

bool MeshPass::submit(const MeshJob *jobs, int count) {
	if (!is_valid() || in_flight_ || !jobs || count <= 0 || count > cfg_.max_jobs) return false;
	reset_counts();
	for (int j = 0; j < count; j++) upload_ops(jobs[j], j);
	const int64_t list = rd_->compute_list_begin();
	for (int j = 0; j < count; j++) record_job(list, jobs[j], j);
	rd_->compute_list_end();
	rd_->submit();
	in_flight_ = true;
	batch_.clear();
	for (int j = 0; j < count; j++) batch_.push_back(jobs[j].chunk);
	return true;
}

int MeshPass::collect(std::vector<MeshResult> *out) {
	if (!in_flight_) return 0;
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	rd_->sync();
	in_flight_ = false;
	const int n = static_cast<int>(batch_.size());
	for (int j = 0; j < n; j++) {
		MeshResult r;
		read_job(j, batch_[j], &r);
		if (out) out->push_back(std::move(r));
	}
	batch_.clear();
	last_collect_ms_ =
			static_cast<float>(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
	return n;
}

bool MeshPass::mesh_sync(const MeshJob &job, MeshResult *out, std::vector<uint8_t> *lattice,
		std::vector<int32_t> *cell_vertex) {
	if (!is_valid() || in_flight_) return false;
	reset_counts();
	upload_ops(job, 0);
	const int64_t list = rd_->compute_list_begin();
	record_job(list, job, 0);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();
	if (out) read_job(0, job.chunk, out);
	if (lattice) {
		const PackedByteArray data = rd_->texture_get_data(lattice_, 0);
		if (data.size() < ve::kChunkLatticeCount) return false;
		lattice->assign(data.ptr(), data.ptr() + ve::kChunkLatticeCount);
	}
	if (cell_vertex) {
		const PackedByteArray data = rd_->buffer_get_data(cells_, 0,
				static_cast<uint32_t>(ve::kChunkCellCount) * 4);
		if (data.size() < static_cast<int64_t>(ve::kChunkCellCount) * 4) return false;
		cell_vertex->resize(ve::kChunkCellCount);
		std::memcpy(cell_vertex->data(), data.ptr(),
				static_cast<size_t>(ve::kChunkCellCount) * 4);
	}
	return true;
}
