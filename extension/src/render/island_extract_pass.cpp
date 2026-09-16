#include "render/island_extract_pass.h"
#include "gpu_layout/blocks.h"
#include "render/field_context_set.h"
#include "render/volume_pool.h"
#include "render/override_pool.h"
#include "world/edit_log.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "shade/oct.h"
#include <cmath>
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

} // namespace

IslandExtractPass::~IslandExtractPass() {
	teardown();
}

bool IslandExtractPass::initialize(RenderingDevice *rd, const VolumePool *volumes) {
	teardown();
	if (!rd || !volumes || !volumes->is_valid()) return false;
	rd_ = rd;
	const int64_t voxels = static_cast<int64_t>(ve::kIslandVoxelCount);
	out_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(voxels * 4), zeroed(voxels * 4)));
	boxes_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(ve::kMaxIslandBoxes * 32, zeroed(ve::kMaxIslandBoxes * 32)));
	counts_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16, zeroed(16)));
	ops_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(ve::kMaxRegionOps * 32, zeroed(ve::kMaxRegionOps * 32)));
	if (!out_.is_valid() || !boxes_.is_valid() || !counts_.is_valid() || !ops_.is_valid()) {
		UtilityFunctions::printerr("IslandExtractPass: buffer creation failed");
		teardown();
		return false;
	}

	program_ = gpu::compile_compute(rd, group_, "IslandExtractPass", "island_extract.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	// The extraction pass shares the worker's override mirror; its owner installs the
	// pointer before initialize so the uniform set never changes identity.
	if (!overrides_) return false;
	set_ = gpu::uniform_set(rd, group_, program_.shader, 0, {
			gpu::storage(0, out_),
			gpu::storage(1, ops_),
			gpu::storage(2, volumes->sdf_buffer()),
			gpu::storage(3, volumes->mat_buffer()),
			gpu::storage(4, boxes_),
			gpu::storage(5, counts_),
			gpu::storage(6, overrides_->sdf_buffer()),
			gpu::storage(7, overrides_->mat_buffer()),
			gpu::storage(8, overrides_->tables()),
			gpu::storage(9, overrides_->region_table_map())});
	if (!set_.is_valid()) {
		UtilityFunctions::printerr("IslandExtractPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void IslandExtractPass::teardown() {
	if (rd_) {
		gpu::RdDevice device{rd_};
		group_.release(device);
	}
	program_ = gpu::Program();
	set_ = out_ = boxes_ = counts_ = ops_ = RID();
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

	const ve::IslandExtractPush push{{job.origin[0], job.origin[1], job.origin[2], job.voxel},
			{job.dim, op_count, box_count, job.override_table}};

	const int64_t list = rd_->compute_list_begin();
	rd_->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, set_, 0);
	if (field_context_ != nullptr) field_context_->bind(rd_, list);
	rd_->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
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
	out->data.normal_oct.resize(static_cast<size_t>(voxels));
	out->data.solid_voxels = 0;
	bool has_sentinel = false;
	for (int64_t i = 0; i < voxels; i++) {
		out->data.sdf[static_cast<size_t>(i)] = static_cast<uint8_t>(w[i] & 0xFFu);
		out->data.mat[static_cast<size_t>(i)] = static_cast<uint8_t>((w[i] >> 8) & 0xFFu);
		uint16_t packed = static_cast<uint16_t>((w[i] >> 16) & 0xFFFFu);
		out->data.normal_oct[static_cast<size_t>(i)] = packed;
		if (packed == 0x8080u) has_sentinel = true;
	}
	if (has_sentinel) {
		ve::OverrideStore base_store(static_cast<int>(job.snapshot.overrides.size()));
		ve::VolumeSet volume_set;
		if (!job.snapshot.materialize(&base_store, &volume_set)) {
			out->data.normal_oct.clear();
		} else if (job.gen == nullptr) {
			out->data.normal_oct.clear();
		} else {
			const ve::Generator &gen = *job.gen;
			bool any_fail = false;
			for (int64_t i = 0; i < voxels; i++) {
				uint16_t packed = out->data.normal_oct[static_cast<size_t>(i)];
				if (packed != 0x8080u) continue;
				int z = static_cast<int>(i / (job.dim * job.dim));
				int y = static_cast<int>((i / job.dim) % job.dim);
				int x = static_cast<int>(i % job.dim);
				float px = job.origin[0] + x * job.voxel;
				float py = job.origin[1] + y * job.voxel;
				float pz = job.origin[2] + z * job.voxel;
				ve::FieldSample fs = ve::eval_field_gradient(gen, job.ops.data(), static_cast<int>(job.ops.size()), px, py, pz, &volume_set, &base_store);
				float bu = 1e30f;
				float bu_grad[3] = {0, 1, 0};
				bool has_bu = false;
				for (const auto &b : job.boxes) {
					float lo[3], hi[3];
					b.world_aabb(lo, hi);
					float d = ve::box_sdf(lo, hi, px, py, pz);
					if (!has_bu || d < bu) {
						bu = d;
						ve::box_sdf_gradient(lo, hi, px, py, pz, bu_grad);
						has_bu = true;
					}
				}
				float grad[3] = {fs.gradient[0], fs.gradient[1], fs.gradient[2]};
				bool exact = fs.exact_gradient;
				if (has_bu && bu > fs.sdf) {
					grad[0] = bu_grad[0]; grad[1] = bu_grad[1]; grad[2] = bu_grad[2];
					exact = true;
				}
				float len = std::sqrt(grad[0]*grad[0] + grad[1]*grad[1] + grad[2]*grad[2]);
				if (!exact || !(len > 1e-8f)) { any_fail = true; break; }
				grad[0] /= len; grad[1] /= len; grad[2] /= len;
				out->data.normal_oct[static_cast<size_t>(i)] = ve::oct_encode_snorm8(grad);
			}
			if (any_fail) out->data.normal_oct.clear();
		}
	}
	// Validate that no sentinel remains; if any sentinel leaked, clear
	for (uint16_t v : out->data.normal_oct) if (v == 0x8080u) { out->data.normal_oct.clear(); break; }
	const PackedByteArray cb = rd_->buffer_get_data(counts_, 0, 16);
	if (cb.size() >= 16)
		out->data.solid_voxels =
				static_cast<int>(reinterpret_cast<const uint32_t *>(cb.ptr())[0]);
	out->failed = false;
	return true;
}
