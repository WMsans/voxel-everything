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
#include "render/gpu/gpu.h"

namespace godot {

class FieldContextSet;

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
	std::vector<ve::LodQuadNormals> normals;
	bool overflow = false;
	bool failed = false; // readback was short/invalid; treat as a failed build
};

// LoD chunk builder on the worker RenderingDevice. Per chunk: field, tent reduce, opaque
// lattice, then cell fractions + packed quads twice -- the shell (original lattice,
// transparent quads only) and the terrain mesh (opaque lattice).
class LodBuildPass {
public:
	~LodBuildPass();

	bool initialize(RenderingDevice *rd, const LodBuildConfig &cfg);
	void teardown();
	bool is_valid() const { return field_program_.pipeline.is_valid(); }
	const LodBuildConfig &config() const { return cfg_; }
	// The pass's field shader, for building a set 1 against it on diagnostics' own
	// devices (debug_lod_diff runs its own LodBuildPass on a per-call device).
	RID field_shader() const { return field_program_.shader; }
	// The worker device's set 1, owned by MeshService and valid for the worker's whole
	// run. Borrowed, never freed here; bound beside the field pass's set 0.
	void set_field_context(const FieldContextSet *fc) { field_context_ = fc; }
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
	void reset_counts();
	void upload_ops(const LodBuildJob &job, int job_index);
	void push(int64_t list, const LodBuildJob &job, int job_index, int mode = 0);
	void record_field(int64_t list, const LodBuildJob &job, int job_index);
	void record_reduce(int64_t list, const LodBuildJob &job, int job_index);
	void record_opaque(int64_t list, const LodBuildJob &job, int job_index);
	void record_frac(int64_t list, const LodBuildJob &job, int job_index, RID set, int mode);
	void record_quads(int64_t list, const LodBuildJob &job, int job_index, RID set, int mode);
	void record_job(int64_t list, const LodBuildJob &job, int job_index);
	void read_job(int job_index, LodBuildResult *out);

	RenderingDevice *rd_ = nullptr;
	const FieldContextSet *field_context_ = nullptr;
	LodBuildConfig cfg_;
	RID fine_sdf_;     // R8_UNORM 3D, 69^3 encoded sdf
	RID fine_mat_;     // R16_UINT 3D, 69^3 material
	RID lat_sdf_;      // R8_UNORM 3D, 34^3 encoded sdf
	RID lat_mat_;      // R16_UINT 3D, 34^3 material
	RID opq_sdf_;      // R8_UNORM 3D, 34^3: the opaque lattice (spec §5)
	RID frac_;         // uint per mesh cell, max_jobs * 33^3 (the terrain write is the live one)
	RID quads_;        // 3 uint per quad, max_jobs * kLodMaxQuadsPerChunk
	RID normals_;      // 2 uint per quad, aligned with quads_
	RID shell_quads_;   // 3 uint per quad, max_jobs * kLodMaxQuadsPerChunk
	RID shell_normals_; // 2 uint per quad, aligned with shell_quads_
	RID counts_;       // 4 uint per job: quad count, overflow, shell count, flags
	RID ops_;          // max_jobs * kMaxRegionOps EditOps
	VolumePool volumes_;
	OverridePool owned_overrides_;
	OverridePool *overrides_ = nullptr;
	gpu::Group group_;
	gpu::Program field_program_, reduce_program_, opaque_program_, frac_program_, quads_program_;
	RID field_set_, reduce_set_, opaque_set_, frac_set_, frac_shell_set_, quads_set_,
			quads_shell_set_;

	bool in_flight_ = false;
	std::vector<LodBuildJob> batch_; // the jobs in flight, in job order
};

} // namespace godot
