#include "render/mesh_pass.h"
#include "gpu_layout/blocks.h"
#include "render/field_context_set.h"
#include "mesh/mesh_chunk.h"
#include "world/edit_log.h"
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

namespace {

PackedByteArray zeroed(int64_t bytes) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(0);
	return b;
}

// Groups for a dispatch of `n` threads per axis at local size 4.
int groups(int n) { return (n + 3) / 4; }

int sanitized_op_count(const MeshJob &job) {
	if (!job.ops || job.op_count <= 0) return 0;
	return std::min(job.op_count, ve::kMaxRegionOps);
}

} // namespace

MeshPass::~MeshPass() {
	teardown();
}

bool MeshPass::initialize(RenderingDevice *rd, const MeshPassConfig &cfg) {
	teardown();
	rd_ = rd;
	cfg_ = cfg;
	if (!rd || cfg.max_jobs <= 0 || cfg.max_verts <= 0 || cfg.max_tris <= 0 ||
			cfg.max_override_bricks <= 0) {
		UtilityFunctions::printerr("MeshPass: degenerate configuration");
		return false;
	}

	{
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
		f->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
		f->set_width(cfg_.max_lattice);
		f->set_height(cfg_.max_lattice);
		f->set_depth(cfg_.max_lattice);
		f->set_mipmaps(1);
		// STORAGE for the field pass to write and the mesher passes to read;
		// CAN_COPY_FROM so the differential test can read the lattice back.
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		lattice_ = group_.add(gpu::Kind::Texture,
				rd->texture_create(f, v, TypedArray<PackedByteArray>()));
	}
	const int64_t max_cells = static_cast<int64_t>(cfg_.max_lattice - 1) *
			(cfg_.max_lattice - 1) * (cfg_.max_lattice - 1);
	cells_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(
			static_cast<uint32_t>(max_cells) * 4, zeroed(max_cells * 4)));
	verts_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_jobs) * cfg_.max_verts * 12,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * cfg_.max_verts * 12)));
	tris_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_jobs) * cfg_.max_tris * 12,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * cfg_.max_tris * 12)));
	counts_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_jobs) * 16,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * 16)));
	ops_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_jobs) * ve::kMaxRegionOps * 32,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * ve::kMaxRegionOps * 32)));
	if (!volumes_.initialize(rd, ve::kMaxVolumes, ve::kIslandDim) ||
			!overrides_.initialize(rd, cfg_.max_override_bricks)) {
		UtilityFunctions::printerr("MeshPass: field pool creation failed");
		teardown();
		return false;
	}
	if (!lattice_.is_valid() || !cells_.is_valid() || !verts_.is_valid() || !tris_.is_valid() ||
			!counts_.is_valid() || !ops_.is_valid() || !volumes_.is_valid() || !overrides_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: buffer creation failed");
		teardown();
		return false;
	}

	field_program_ = gpu::compile_compute(rd, group_, "MeshPass", "mesh_field.comp.glsl");
	if (!field_program_.valid()) {
		teardown();
		return false;
	}
	field_set_ = gpu::uniform_set(rd, group_, field_program_.shader, 0, {
			gpu::image(0, lattice_),
			gpu::storage(1, ops_),
			gpu::storage(2, volumes_.sdf_buffer()),
			gpu::storage(3, volumes_.mat_buffer()),
			gpu::storage(4, overrides_.sdf_buffer()),
			gpu::storage(5, overrides_.mat_buffer()),
			gpu::storage(6, overrides_.tables()),
			gpu::storage(7, overrides_.region_table_map())});
	if (!field_set_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: uniform set creation failed");
		teardown();
		return false;
	}

	cells_program_ = gpu::compile_compute(rd, group_, "MeshPass", "mesh_cells.comp.glsl");
	quads_program_ = gpu::compile_compute(rd, group_, "MeshPass", "mesh_quads.comp.glsl");
	if (!cells_program_.valid() || !quads_program_.valid()) {
		teardown();
		return false;
	}
	cells_set_ = gpu::uniform_set(rd, group_, cells_program_.shader, 0, {
			gpu::image(0, lattice_), gpu::storage(1, cells_), gpu::storage(2, verts_),
			gpu::storage(3, counts_)});
	quads_set_ = gpu::uniform_set(rd, group_, quads_program_.shader, 0, {
			gpu::image(0, lattice_), gpu::storage(1, cells_), gpu::storage(2, tris_),
			gpu::storage(3, counts_)});
	if (!cells_set_.is_valid() || !quads_set_.is_valid()) {
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
	// Sets, pipelines and shaders first, then this pass's lattice and buffers. The pools'
	// buffers were bound only by field_set_, which is gone by the time they are.
	gpu::RdDevice device{rd_};
	group_.release(device);
	volumes_.teardown();
	overrides_.teardown();
	field_program_ = cells_program_ = quads_program_ = gpu::Program();
	field_set_ = cells_set_ = quads_set_ = RID();
	lattice_ = cells_ = verts_ = tris_ = counts_ = ops_ = RID();
	rd_ = nullptr;
}

void MeshPass::upload_ops(const MeshJob &job, int job_index) {
	const int n = sanitized_op_count(job);
	if (n <= 0) return; // op_count in the push constant is what the shader reads
	PackedByteArray b;
	b.resize(static_cast<int64_t>(n) * 32);
	std::memcpy(b.ptrw(), job.ops, static_cast<size_t>(n) * 32);
	rd_->buffer_update(ops_, static_cast<uint32_t>(job_index) * ve::kMaxRegionOps * 32,
			static_cast<uint32_t>(b.size()), b);
}

void MeshPass::set_override_table(int region_slot, int table,
		const std::vector<std::pair<int, int>> &entries) {
	if (!overrides_.is_valid()) return;
	overrides_.set_region_table(rd_, region_slot, table);
	for (const auto &entry : entries) overrides_.set_table_entry(rd_, table, entry.first, entry.second);
}

bool MeshPass::upload_volume(int slot, const ve::VolumeData &data) {
	// buffer_update is device-level and must not land inside an open compute list; the
	// worker only ever calls this between jobs, which is where that is guaranteed.
	return rd_ && !in_flight_ && volumes_.upload(rd_, slot, data);
}

// The same 48-byte block for all three passes, so one helper serves them all.
void MeshPass::push(int64_t list, const MeshJob &job, int job_index) {
	const ve::MeshPush push{{job.chunk.x, job.chunk.y, job.chunk.z, job_index},
			{sanitized_op_count(job), cfg_.max_verts, cfg_.max_tris, job.lattice},
			{job.origin[0], job.origin[1], job.origin[2], job.cell_size},
			{job.override_table, -1, 0, 0}};
	rd_->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
}

void MeshPass::record_field(int64_t list, const MeshJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, field_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, field_set_, 0);
	if (field_context_ != nullptr) field_context_->bind(rd_, list);
	push(list, job, job_index);
	const int g = groups(job.lattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

bool MeshPass::run_field_sync(const MeshJob &job, std::vector<uint8_t> *lattice) {
	if (!is_valid() || in_flight_) return false;
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
	rd_->compute_list_bind_compute_pipeline(list, cells_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, cells_set_, 0);
	push(list, job, job_index);
	const int g = groups(job.lattice - 1);
	rd_->compute_list_dispatch(list, g, g, g);
}

void MeshPass::record_quads(int64_t list, const MeshJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, quads_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, quads_set_, 0);
	push(list, job, job_index);
	const int g = groups(job.lattice - 2);
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
	out->failed = false;
	const PackedByteArray cb =
			rd_->buffer_get_data(counts_, static_cast<uint32_t>(job_index) * 16, 16);
	if (cb.size() < 16) {
		out->failed = true;
		return;
	}
	const uint32_t *c = reinterpret_cast<const uint32_t *>(cb.ptr());
	// The counters are raw atomic totals: they run past the cap when it is hit.
	const int vcount = std::min<int>(static_cast<int>(c[0]), cfg_.max_verts);
	const int tcount = std::min<int>(static_cast<int>(c[1]), cfg_.max_tris);
	out->overflow = c[2] != 0u;
	if (vcount > 0) {
		const PackedByteArray vb = rd_->buffer_get_data(verts_,
				static_cast<uint32_t>(job_index) * cfg_.max_verts * 12,
				static_cast<uint32_t>(vcount) * 12);
		if (vb.size() < static_cast<int64_t>(vcount) * 12) {
			out->failed = true;
			out->positions.clear();
			out->indices.clear();
			return;
		}
		out->positions.resize(static_cast<size_t>(vcount) * 3);
		std::memcpy(out->positions.data(), vb.ptr(), static_cast<size_t>(vcount) * 12);
	}
	if (tcount > 0) {
		const PackedByteArray tb = rd_->buffer_get_data(tris_,
				static_cast<uint32_t>(job_index) * cfg_.max_tris * 12,
				static_cast<uint32_t>(tcount) * 12);
		if (tb.size() < static_cast<int64_t>(tcount) * 12) {
			out->failed = true;
			out->positions.clear();
			out->indices.clear();
			return;
		}
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
