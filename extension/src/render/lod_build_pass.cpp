#include "render/lod_build_pass.h"
#include "gpu_layout/blocks.h"
#include "render/field_context_set.h"
#include "lod/lod_contour.h"
#include "lod/lod_skirt.h"
#include "world/edit_log.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/time.hpp>
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
		*rid = group_.add(gpu::Kind::Texture,
				rd->texture_create(f, v, TypedArray<PackedByteArray>()));
	};
	make_3d(&fine_sdf_, RenderingDevice::DATA_FORMAT_R8_UNORM, ve::kLodFineLattice);
	make_3d(&fine_mat_, RenderingDevice::DATA_FORMAT_R16_UINT, ve::kLodFineLattice);
	make_3d(&lat_sdf_, RenderingDevice::DATA_FORMAT_R8_UNORM, ve::kLodChunkLattice);
	make_3d(&lat_mat_, RenderingDevice::DATA_FORMAT_R16_UINT, ve::kLodChunkLattice);
	make_3d(&opq_sdf_, RenderingDevice::DATA_FORMAT_R8_UNORM, ve::kLodChunkLattice);

	const int64_t frac_count =
			static_cast<int64_t>(cfg_.max_jobs) * ve::kLodChunkMeshCells *
			ve::kLodChunkMeshCells * ve::kLodChunkMeshCells;
	frac_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(static_cast<uint32_t>(frac_count * 4),
			filled(frac_count * 4, 0xFF)));
	const int64_t quads_bytes =
			static_cast<int64_t>(cfg_.max_jobs) * ve::kLodMaxQuadsPerChunk * 12;
	quads_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(static_cast<uint32_t>(quads_bytes),
			zeroed(quads_bytes)));
	const int64_t normals_bytes =
			static_cast<int64_t>(cfg_.max_jobs) * ve::kLodMaxQuadsPerChunk * sizeof(ve::LodQuadNormals);
	normals_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(static_cast<uint32_t>(normals_bytes),
			zeroed(normals_bytes)));
	shell_quads_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(static_cast<uint32_t>(quads_bytes),
			zeroed(quads_bytes)));
	shell_normals_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(static_cast<uint32_t>(normals_bytes),
			zeroed(normals_bytes)));
	const int64_t counts_bytes = static_cast<int64_t>(cfg_.max_jobs) * 16;
	counts_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(static_cast<uint32_t>(counts_bytes),
			zeroed(counts_bytes)));
	const int64_t ops_bytes =
			static_cast<int64_t>(cfg_.max_jobs) * ve::kMaxRegionOps * 32;
	ops_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(static_cast<uint32_t>(ops_bytes),
			zeroed(ops_bytes)));

	if (!overrides_) overrides_ = &owned_overrides_;
	if (!volumes_.initialize(rd, ve::kMaxVolumes, ve::kIslandDim) ||
			(overrides_ == &owned_overrides_ && !owned_overrides_.initialize(rd, OverridePool::kDefaultCapacity))) {
		UtilityFunctions::printerr("LodBuildPass: field pool creation failed");
		teardown();
		return false;
	}
	if (!fine_sdf_.is_valid() || !fine_mat_.is_valid() || !lat_sdf_.is_valid() ||
			!lat_mat_.is_valid() || !opq_sdf_.is_valid() || !frac_.is_valid() || !quads_.is_valid() ||
			!normals_.is_valid() || !shell_quads_.is_valid() || !shell_normals_.is_valid() ||
			!counts_.is_valid() || !ops_.is_valid() || !volumes_.is_valid() || !overrides_->is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: buffer/texture creation failed");
		teardown();
		return false;
	}

	field_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_field.comp.glsl");
	if (!field_program_.valid()) {
		teardown();
		return false;
	}
	field_set_ = gpu::uniform_set(rd, group_, field_program_.shader, 0, {
			gpu::image(0, fine_sdf_), gpu::image(1, fine_mat_), gpu::storage(2, ops_),
			gpu::storage(3, volumes_.sdf_buffer()), gpu::storage(4, volumes_.mat_buffer()),
			gpu::storage(5, overrides_->sdf_buffer()), gpu::storage(6, overrides_->mat_buffer()),
			gpu::storage(7, overrides_->tables()), gpu::storage(8, overrides_->region_table_map())});
	if (!field_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: field uniform set creation failed");
		teardown();
		return false;
	}

	reduce_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_reduce.comp.glsl");
	if (!reduce_program_.valid()) {
		teardown();
		return false;
	}
	reduce_set_ = gpu::uniform_set(rd, group_, reduce_program_.shader, 0, {
			gpu::image(0, fine_sdf_), gpu::image(1, fine_mat_), gpu::image(2, lat_sdf_),
			gpu::image(3, lat_mat_), gpu::storage(4, counts_)});
	if (!reduce_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: reduce uniform set creation failed");
		teardown();
		return false;
	}

	opaque_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_opaque.comp.glsl");
	if (!opaque_program_.valid()) {
		teardown();
		return false;
	}
	opaque_set_ = gpu::uniform_set(rd, group_, opaque_program_.shader, 0,
			{gpu::image(0, lat_sdf_), gpu::image(1, lat_mat_), gpu::image(2, opq_sdf_)});
	if (!opaque_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: opaque uniform set creation failed");
		teardown();
		return false;
	}

	frac_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_frac.comp.glsl");
	if (!frac_program_.valid()) {
		teardown();
		return false;
	}
	// Both sets bind counts_ because the shader declares it; only the shell set runs at mode
	// 1, and only that mode reads the has-transparent bit.
	frac_set_ = gpu::uniform_set(rd, group_, frac_program_.shader, 0,
			{gpu::image(0, opq_sdf_), gpu::storage(1, frac_), gpu::storage(2, counts_)});
	frac_shell_set_ = gpu::uniform_set(rd, group_, frac_program_.shader, 0,
			{gpu::image(0, lat_sdf_), gpu::storage(1, frac_), gpu::storage(2, counts_)});
	if (!frac_set_.is_valid() || !frac_shell_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: frac uniform set creation failed");
		teardown();
		return false;
	}

	quads_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_quads.comp.glsl");
	if (!quads_program_.valid()) {
		teardown();
		return false;
	}
	quads_set_ = gpu::uniform_set(rd, group_, quads_program_.shader, 0, {
			gpu::image(0, opq_sdf_), gpu::image(1, lat_mat_), gpu::storage(2, frac_),
			gpu::storage(3, quads_), gpu::storage(4, counts_), gpu::storage(5, normals_)});
	quads_shell_set_ = gpu::uniform_set(rd, group_, quads_program_.shader, 0, {
			gpu::image(0, lat_sdf_), gpu::image(1, lat_mat_), gpu::storage(2, frac_),
			gpu::storage(3, shell_quads_), gpu::storage(4, counts_),
			gpu::storage(5, shell_normals_)});
	if (!quads_set_.is_valid() || !quads_shell_set_.is_valid()) {
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
	// Sets, pipelines and shaders first, then this pass's lattices and buffers. The pools'
	// buffers were bound only by field_set_, which is gone by the time they are.
	gpu::RdDevice device{rd_};
	group_.release(device);
	volumes_.teardown();
	if (overrides_ == &owned_overrides_) owned_overrides_.teardown();
	overrides_ = nullptr;
	field_program_ = reduce_program_ = opaque_program_ = frac_program_ = quads_program_ =
			gpu::Program();
	field_set_ = reduce_set_ = opaque_set_ = frac_set_ = frac_shell_set_ = quads_set_ =
			quads_shell_set_ = RID();
	fine_sdf_ = fine_mat_ = lat_sdf_ = lat_mat_ = opq_sdf_ = RID();
	frac_ = quads_ = normals_ = shell_quads_ = shell_normals_ = counts_ = ops_ = RID();
	rd_ = nullptr;
}

void LodBuildPass::reset_counts() {
	rd_->buffer_update(counts_, 0, static_cast<uint32_t>(cfg_.max_jobs) * 16,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * 16));
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

void LodBuildPass::push(int64_t list, const LodBuildJob &job, int job_index, int mode) {
	float origin[3];
	ve::lod_chunk_origin(job.level, job.coord, origin);
	const ve::LodBuildPush push{{job.coord.x, job.coord.y, job.coord.z, job_index},
			{sanitized_op_count(job), ve::kLodMaxQuadsPerChunk, job.level, mode},
			{origin[0], origin[1], origin[2], ve::lod_cell_size(job.level)},
			{job.override_table, -1, 0, 0}};
	rd_->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
}

void LodBuildPass::record_field(int64_t list, const LodBuildJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, field_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, field_set_, 0);
	if (field_context_ != nullptr) field_context_->bind(rd_, list);
	push(list, job, job_index);
	const int g = groups(ve::kLodFineLattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_reduce(int64_t list, const LodBuildJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, reduce_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, reduce_set_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kLodChunkLattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_opaque(int64_t list, const LodBuildJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, opaque_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, opaque_set_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kLodChunkLattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_frac(int64_t list, const LodBuildJob &job, int job_index, RID set,
		int mode) {
	rd_->compute_list_bind_compute_pipeline(list, frac_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, set, 0);
	push(list, job, job_index, mode);
	const int g = groups(ve::kLodChunkMeshCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_quads(int64_t list, const LodBuildJob &job, int job_index, RID set,
		int mode) {
	rd_->compute_list_bind_compute_pipeline(list, quads_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, set, 0);
	push(list, job, job_index, mode);
	const int g = groups(ve::kLodChunkCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

// Spec §5. The opaque lattice ALWAYS runs: it is the identity on a chunk with no transparent
// label, so the terrain mesh below is the one this chunk always had. The shell pair runs
// first, on the original lattice, and BOTH its passes leave at once when the reduce raised no
// transparent bit. frac_ holds one slice, so the two pairs run strictly in turn.
void LodBuildPass::record_job(int64_t list, const LodBuildJob &job, int job_index) {
	record_field(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_reduce(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_opaque(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_frac(list, job, job_index, frac_shell_set_, 1);
	rd_->compute_list_add_barrier(list);
	record_quads(list, job, job_index, quads_shell_set_, 1);
	rd_->compute_list_add_barrier(list);
	record_frac(list, job, job_index, frac_set_, 0);
	rd_->compute_list_add_barrier(list);
	record_quads(list, job, job_index, quads_set_, 0);
	rd_->compute_list_add_barrier(list);
}

void LodBuildPass::set_override_table(int region_slot, int table,
		const std::vector<std::pair<int, int>> &entries) {
	if (!overrides_ || !overrides_->is_valid()) return;
	overrides_->set_region_table(rd_, region_slot, table);
	for (const auto &entry : entries) overrides_->set_table_entry(rd_, table, entry.first, entry.second);
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
	out->normals.clear();
	out->overflow = false;
	out->failed = false;
	const PackedByteArray cb = rd_->buffer_get_data(counts_,
			static_cast<uint32_t>(job_index) * 16, 16);
	if (cb.size() < 16) {
		out->failed = true;
		return;
	}
	const uint32_t *c = reinterpret_cast<const uint32_t *>(cb.ptr());
	const int qcount = std::min<int>(static_cast<int>(c[0]), ve::kLodMaxQuadsPerChunk);
	const int scount = std::min<int>(static_cast<int>(c[2]), ve::kLodMaxQuadsPerChunk);
	out->overflow = c[1] != 0u || (c[3] & 2u) != 0u;
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
		const PackedByteArray nb = rd_->buffer_get_data(normals_,
				static_cast<uint32_t>(job_index) * ve::kLodMaxQuadsPerChunk * sizeof(ve::LodQuadNormals),
				static_cast<uint32_t>(qcount) * sizeof(ve::LodQuadNormals));
		if (nb.size() < static_cast<int64_t>(qcount) * sizeof(ve::LodQuadNormals)) {
			out->failed = true;
			out->quads.clear();
			return;
		}
		out->normals.resize(static_cast<size_t>(qcount));
		std::memcpy(out->normals.data(), nb.ptr(),
				static_cast<size_t>(qcount) * sizeof(ve::LodQuadNormals));
	}
	ve::lod_append_skirts(&out->quads, &out->normals);
	if (scount > 0) {
		const uint32_t qbytes = static_cast<uint32_t>(scount) * 12;
		const uint32_t nbytes = static_cast<uint32_t>(scount) * sizeof(ve::LodQuadNormals);
		const PackedByteArray sq = rd_->buffer_get_data(shell_quads_,
				static_cast<uint32_t>(job_index) * ve::kLodMaxQuadsPerChunk * 12, qbytes);
		const PackedByteArray sn = rd_->buffer_get_data(shell_normals_,
				static_cast<uint32_t>(job_index) * ve::kLodMaxQuadsPerChunk * sizeof(ve::LodQuadNormals),
				nbytes);
		if (sq.size() < qbytes || sn.size() < nbytes) {
			out->failed = true;
			out->quads.clear();
			out->normals.clear();
			return;
		}
		std::vector<ve::LodQuad> shell(static_cast<size_t>(scount));
		std::vector<ve::LodQuadNormals> shell_normals(static_cast<size_t>(scount));
		std::memcpy(shell.data(), sq.ptr(), qbytes);
		std::memcpy(shell_normals.data(), sn.ptr(), nbytes);
		// Shell after skirts: skirts are boundary ribbons of the TERRAIN mesh, and a shell
		// quad must never become a skirt's parent.
		if (ve::lod_append_shell(&out->quads, &out->normals, shell, shell_normals))
			out->overflow = true;
	}
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
