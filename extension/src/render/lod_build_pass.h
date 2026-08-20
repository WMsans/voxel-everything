#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>
#include <vector>
#include <utility>
#include "generator/edit_ops.h"
#include "lod/lod_grid.h"
#include "lod/lod_quad.h"
#include "render/volume_pool.h"
#include "render/override_pool.h"

namespace godot {

struct LodBuildConfig {
	int max_jobs = 8; // chunks per batch; default allows multi-job batches without trapping callers
};

struct LodBuildJob {
	int level = 0;
	ve::IVec3 coord{};
	std::vector<ve::EditOp> ops;
	int override_table = -1;
};

struct LodBuildResult {
	int level = 0;
	ve::IVec3 coord{};
	std::vector<ve::LodQuad> quads;
	bool overflow = false;
	bool failed = false; // readback was short/invalid; treat as a failed build
};

// LoD chunk builder on the worker RenderingDevice. Four dispatches per chunk:
// field (half-cell samples), tent reduce, cell fractions, packed quads.
class LodBuildPass {
public:
	~LodBuildPass();

	bool initialize(RenderingDevice *rd, const LodBuildConfig &cfg);
	void teardown();
	bool is_valid() const { return field_pipeline_.is_valid(); }
	const LodBuildConfig &config() const { return cfg_; }
	VolumePool &volumes() { return volumes_; }
	OverridePool &overrides() { return *overrides_; }
	void set_override_pool(OverridePool *pool) { overrides_ = pool; }
	bool upload_override(int slot, const ve::OverrideBrick &brick) { return overrides_ && overrides_->upload(slot, brick); }
	void set_override_table(int region_slot, int table, const std::vector<std::pair<int, int>> &entries);

	// Diagnostics: the worker owns the textures, and the differential hook reads them back
	// after build_sync.
	RID fine_sdf() const { return fine_sdf_; }
	RID fine_mat() const { return fine_mat_; }
	RID lat_sdf() const { return lat_sdf_; }
	RID lat_mat() const { return lat_mat_; }

	// Runs one chunk inline (record, submit, sync, read back). Diagnostic only — the
	// streaming path never stalls like this. `lattice`/`material`, when non-null, receive
	// the REDUCED lattice/material (kLodChunkLattice^3) used by the contour pass.
	bool build_sync(const LodBuildJob &job, LodBuildResult *out,
			std::vector<uint8_t> *lattice, std::vector<uint16_t> *material);

	// Records and submits one batch; false when a batch is still in flight, the count is
	// zero, or it exceeds config().max_jobs.
	bool submit(const LodBuildJob *jobs, int count);
	bool in_flight() const { return in_flight_; }
	// Syncs the batch in flight, reads it back, appends skirts, returns how many. Zero
	// when nothing is in flight.
	int collect(std::vector<LodBuildResult> *out);

private:
	bool build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline);
	void reset_counts();
	void upload_ops(const LodBuildJob &job, int job_index);
	void push(int64_t list, const LodBuildJob &job, int job_index);
	void record_field(int64_t list, const LodBuildJob &job, int job_index);
	void record_reduce(int64_t list, const LodBuildJob &job, int job_index);
	void record_frac(int64_t list, const LodBuildJob &job, int job_index);
	void record_quads(int64_t list, const LodBuildJob &job, int job_index);
	void record_job(int64_t list, const LodBuildJob &job, int job_index);
	void read_job(int job_index, LodBuildResult *out);

	RenderingDevice *rd_ = nullptr;
	LodBuildConfig cfg_;
	RID fine_sdf_;     // R8_UNORM 3D, 69^3 encoded sdf
	RID fine_mat_;     // R16_UINT 3D, 69^3 material
	RID lat_sdf_;      // R8_UNORM 3D, 34^3 encoded sdf
	RID lat_mat_;      // R16_UINT 3D, 34^3 material
	RID frac_;         // uint per mesh cell, max_jobs * 33^3 (first slice is the live one)
	RID quads_;        // 3 uint per quad, max_jobs * kLodMaxQuadsPerChunk
	RID counts_;       // 2 uint per job: quad count, overflow flag
	RID ops_;          // max_jobs * kMaxRegionOps EditOps
	VolumePool volumes_;
	OverridePool owned_overrides_;
	OverridePool *overrides_ = nullptr;
	RID field_shader_, field_pipeline_, field_uset_;
	RID reduce_shader_, reduce_pipeline_, reduce_uset_;
	RID frac_shader_, frac_pipeline_, frac_uset_;
	RID quads_shader_, quads_pipeline_, quads_uset_;

	bool in_flight_ = false;
	std::vector<LodBuildJob> batch_; // the jobs in flight, in job order
};

} // namespace godot
