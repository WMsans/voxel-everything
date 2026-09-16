#include "render/consolidate_pass.h"
#include "gpu_layout/blocks.h"
#include "render/field_context_set.h"
#include "render/volume_pool.h"
#include "generator/generator.h"
#include "world/brick.h"
#include "world/brick_eval.h"
#include "shade/oct.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

namespace {
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

bool ConsolidatePass::initialize(RenderingDevice *rd, OverridePool *pool, VolumePool *volumes, int max_bricks) {
	teardown();
	if (!rd || !pool || !volumes || !pool->is_valid() || !volumes->is_valid()) return false;
	max_bricks = max_bricks > 0 ? max_bricks : pool->capacity();
	if (max_bricks <= 0 || max_bricks > pool->capacity()) return false;
	rd_ = rd;
	pool_ = pool;
	max_bricks_ = max_bricks;
	ops_ = group_.add(gpu::Kind::Buffer,
			rd_->storage_buffer_create(ve::kMaxRegionOps * 32, zeroed(ve::kMaxRegionOps * 32)));
	jobs_ = group_.add(gpu::Kind::Buffer, rd_->storage_buffer_create(static_cast<uint32_t>(max_bricks_) * 32,
			zeroed(static_cast<int64_t>(max_bricks_) * 32)));
	// Consolidation reads the currently published override through the pool, then writes the
	// replacement into private transient storage. Writing into the pool's high slots looked
	// like double buffering, but those slots can already hold live bricks after releases and
	// would race the read side of a re-consolidation. The staging buffers are bounded by the
	// pool capacity and are never visible to field consumers.
	staging_sdf_ = group_.add(gpu::Kind::Buffer, rd_->storage_buffer_create(
			static_cast<uint32_t>(max_bricks_) * kSdfStride,
			zeroed(static_cast<int64_t>(max_bricks_) * kSdfStride)));
	staging_mat_ = group_.add(gpu::Kind::Buffer, rd_->storage_buffer_create(
			static_cast<uint32_t>(max_bricks_) * kMatStride,
			zeroed(static_cast<int64_t>(max_bricks_) * kMatStride)));
	program_ = gpu::compile_compute(rd_, group_, "ConsolidatePass", "brick_consolidate.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	set_ = gpu::uniform_set(rd_, group_, program_.shader, 0, {
			gpu::storage(0, pool_->sdf_buffer()),
			gpu::storage(1, pool_->mat_buffer()),
			gpu::storage(3, staging_sdf_),
			gpu::storage(4, staging_mat_),
			gpu::storage(5, jobs_),
			gpu::storage(6, pool_->tables()),
			gpu::storage(7, pool_->region_table_map()),
			gpu::storage(8, ops_),
			gpu::storage(9, volumes->sdf_buffer()),
			gpu::storage(10, volumes->mat_buffer())});
	if (!set_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void ConsolidatePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	set_ = ops_ = jobs_ = staging_sdf_ = staging_mat_ = RID();
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
		// Output slots belong to the private staging buffers, not the published override pool.
		// This remains disjoint even when the CPU store has reused arbitrary live slots.
		j[i * 8 + 3] = i;
	}
	rd_->buffer_update(jobs_, 0, static_cast<uint32_t>(jb.size()), jb);
	if (!job.ops.empty()) {
		PackedByteArray ob;
		ob.resize(static_cast<int64_t>(job.ops.size()) * 32);
		std::memcpy(ob.ptrw(), job.ops.data(), static_cast<size_t>(ob.size()));
		rd_->buffer_update(ops_, 0, static_cast<uint32_t>(ob.size()), ob);
	}
	const ve::ConsolidatePush push{{n, job.region_slot, static_cast<int>(job.ops.size()), pool_->region_table(job.region_slot)}};
	const int64_t list = rd_->compute_list_begin();
	rd_->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, set_, 0);
	if (field_context_ != nullptr) field_context_->bind(rd_, list);
	rd_->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd_->compute_list_dispatch(list, static_cast<uint32_t>(n), 1, 1);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();
	out->baked.resize(static_cast<size_t>(n));
	for (int i = 0; i < n; i++) {
		const int slot = i;
		const PackedByteArray sb = rd_->buffer_get_data(staging_sdf_,
				static_cast<uint32_t>(slot * kSdfStride), kSdfStride);
		const PackedByteArray mb = rd_->buffer_get_data(staging_mat_,
				static_cast<uint32_t>(slot * kMatStride), kMatStride);
		if (sb.size() < kSdfStride || mb.size() < kMatStride) {
			out->baked.clear();
			return false;
		}
		std::memcpy(out->baked[static_cast<size_t>(i)].sdf, sb.ptr(), ve::kBrickSdfCount);
		std::memcpy(out->baked[static_cast<size_t>(i)].mat, mb.ptr(), ve::kBrickVoxelCount);
	}
	// Encode consolidation normals from exact CPU source after readback.
	{
		ve::OverrideStore base_store(static_cast<int>(job.source.overrides.size()));
		ve::VolumeSet volume_set;
		if (!job.source.materialize(&base_store, &volume_set)) {
			for (auto &b : out->baked) b.normal_oct.clear();
		} else if (job.gen == nullptr) {
			for (auto &b : out->baked) b.normal_oct.clear();
		} else {
			const ve::Generator &gen = *job.gen;
				for (size_t bi = 0; bi < out->baked.size(); bi++) {
					const ve::IVec3 brick = job.bricks[bi];
					float bo[3];
					ve::brick_world_origin(brick, bo);
					// Per-brick op filter, but NOT the value bake's kBrickFilterPad: encode_sdf clamps, so an
					// excluded op can win the field without changing any encoded byte while flipping the
					// gradient. Two soundness rules: kOpVolumeAdd is NEVER filtered -- its lattice sampler
					// clamps out-of-box points onto the stored lattice and returns winnable samples in an
					// unbounded region; local sphere/box ops use the documented lattice-consumer pad
					// (kLatticeFilterPad), which covers the representable SDF band plus one pitch.
					float lo[3]={bo[0]-ve::kLatticeFilterPad, bo[1]-ve::kLatticeFilterPad, bo[2]-ve::kLatticeFilterPad};
					float hi[3]={bo[0]+ve::kBrickSize+ve::kLatticeFilterPad, bo[1]+ve::kBrickSize+ve::kLatticeFilterPad, bo[2]+ve::kBrickSize+ve::kLatticeFilterPad};
					std::vector<ve::EditOp> filtered;
					filtered.reserve(job.ops.size());
					for (auto &op : job.ops)
						if (op.type == ve::kOpVolumeAdd || ve::op_touches_aabb(op, lo, hi, 0.0f)) filtered.push_back(op);
					std::vector<uint16_t> normals;
					normals.reserve(ve::kBrickSdfCount);
					bool ok = true;
					// `ok` gates all three loops: a bare `break` on the inner one kept
					// evaluating the rest of the brick only to throw the result away.
					for (int z = 0; z < ve::kBrickSdfStride && ok; z++)
						for (int y = 0; y < ve::kBrickSdfStride && ok; y++)
							for (int x = 0; x < ve::kBrickSdfStride && ok; x++) {
								float px = bo[0] + x * ve::kVoxelSize;
								float py = bo[1] + y * ve::kVoxelSize;
								float pz = bo[2] + z * ve::kVoxelSize;
								// Filtered per brick as above: volume ops always kept, locals at lattice pad.
								ve::FieldSample fs = ve::eval_field_gradient(gen, filtered.data(), static_cast<int>(filtered.size()), px, py, pz, &volume_set, &base_store);
								if (!fs.exact_gradient) { ok = false; break; }
								float len = std::sqrt(fs.gradient[0]*fs.gradient[0] + fs.gradient[1]*fs.gradient[1] + fs.gradient[2]*fs.gradient[2]);
								if (!(len > 1e-8f)) { ok = false; break; }
								float n[3] = {fs.gradient[0]/len, fs.gradient[1]/len, fs.gradient[2]/len};
								normals.push_back(ve::oct_encode_snorm8(n));
							}
					if (!ok) normals.clear();
					if (normals.size() != ve::kBrickSdfCount) normals.clear();
					out->baked[bi].normal_oct = std::move(normals);
				}
			}
		}
	out->failed = false;
	return true;
}
