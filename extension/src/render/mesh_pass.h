#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>
#include <vector>
#include <utility>
#include "generator/edit_ops.h"
#include "mesh/mesh_chunk.h"
#include "render/volume_pool.h"
#include "render/override_pool.h"
#include "world/region.h"

namespace godot {

struct MeshPassConfig {
	int max_jobs = 2;      // chunks per batch
	int max_verts = 16384; // a fully covered 6.4 m chunk holds ~4 100
	int max_tris = 32768;  // ...and ~8 200 triangles; edits can carve well past that
	int max_override_bricks = 8192;
	// Buffers are sized for the largest lattice any consumer will ask for, so one pass can
	// serve both the collision chunk and (Task 9) a LoD chunk without reallocating.
	int max_lattice = ve::kChunkLattice;
};

struct MeshJob {
	ve::IVec3 chunk{};
	const ve::EditOp *ops = nullptr; // the chunk's region's op list; copied at submit
	int op_count = 0;
	// Where and how finely to sample. Defaulted to the collision chunk so every existing
	// caller is unchanged; MeshService::submit fills them from ve::chunk_world_origin.
	float origin[3] = {0.0f, 0.0f, 0.0f};
	float cell_size = ve::kChunkCellSize;
	int lattice = ve::kChunkLattice;
	int override_table = -1;
};

struct MeshResult {
	ve::IVec3 chunk{};
	std::vector<float> positions;  // 3 per vertex, world space
	std::vector<uint32_t> indices; // 3 per triangle
	bool overflow = false;         // a cap was hit: the mesh is missing pieces
	bool failed = false;           // readback was short/invalid; treat as a failed build
};

// The collision mesher. Owns every GPU resource on ITS OWN local RenderingDevice — the
// mesher never reads the brick atlas (see the plan's Deliberate Decisions), so it shares no
// resource with the renderer and can be submitted and synced without touching the frame.
class MeshPass {
public:
	~MeshPass();

	bool initialize(RenderingDevice *rd, const MeshPassConfig &cfg);
	void teardown();
	bool is_valid() const { return field_pipeline_.is_valid(); }
	const MeshPassConfig &config() const { return cfg_; }
	VolumePool &volumes() { return volumes_; }
	OverridePool &overrides() { return overrides_; }
	bool upload_override(int slot, const ve::OverrideBrick &brick) { return overrides_.upload(slot, brick); }
	void set_override_table(int region_slot, int table, const std::vector<std::pair<int, int>> &entries);
	void clear_override_table(int table) { overrides_.clear_table(rd_, table); }
	void clear_override_region(int region_slot) { overrides_.set_region_table(rd_, region_slot, -1); }
	void set_override_entry(int table, int brick_index, int slot) {
		overrides_.set_table_entry(rd_, table, brick_index, slot);
	}

	// Uploads one stored volume to THIS device. Called on the worker thread only (the device
	// belongs to it); MeshService::submit_volume is the main thread's way in.
	bool upload_volume(int slot, const ve::VolumeData &data);

	// Runs the field pass alone for one chunk, inline (record, submit, sync, read back).
	// Diagnostic only — the streaming path never stalls like this.
	bool run_field_sync(const MeshJob &job, std::vector<uint8_t> *lattice);

	// Meshes one chunk inline (record, submit, sync, read back). Diagnostic only — the
	// streaming path never stalls like this. `lattice` and `cell_vertex` are optional and
	// exist for the differential test.
	bool mesh_sync(const MeshJob &job, MeshResult *out, std::vector<uint8_t> *lattice,
			std::vector<int32_t> *cell_vertex);

	// Records and submits one batch; false when a batch is still in flight, the count is
	// zero, or it exceeds config().max_jobs.
	bool submit(const MeshJob *jobs, int count);
	bool in_flight() const { return in_flight_; }
	// Syncs the batch in flight, reads it back, appends to `out`, returns how many. Zero
	// when nothing is in flight. The sync is for work submitted a frame ago, so it does not
	// wait on the GPU in practice.
	int collect(std::vector<MeshResult> *out);
	float last_collect_ms() const { return last_collect_ms_; }

private:
	bool build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline);
	void record_field(int64_t list, const MeshJob &job, int job_index);
	void upload_ops(const MeshJob &job, int job_index);
	void push(int64_t list, const MeshJob &job, int job_index);

	// private
	void record_job(int64_t list, const MeshJob &job, int job_index);
	void record_cells(int64_t list, const MeshJob &job, int job_index);
	void record_quads(int64_t list, const MeshJob &job, int job_index);
	void read_job(int job_index, ve::IVec3 chunk, MeshResult *out);
	void reset_counts();

	RenderingDevice *rd_ = nullptr;
	MeshPassConfig cfg_;
	RID lattice_;     // R8_UNORM 3D, 130^3 encoded sdf
	RID cells_;       // int32 per mesh cell: vertex index or -1
	RID verts_;       // float3 per vertex, max_jobs * max_verts
	RID tris_;        // uint3 per triangle, max_jobs * max_tris
	RID counts_;      // 4 uints per job: vert count, tri count, overflow bits, pad
	RID ops_;         // max_jobs * kMaxRegionOps EditOps
	VolumePool volumes_;
	RID field_shader_, field_pipeline_, field_uset_;
	OverridePool overrides_;
	RID cells_shader_, cells_pipeline_, cells_uset_;
	RID quads_shader_, quads_pipeline_, quads_uset_;

	bool in_flight_ = false;
	std::vector<ve::IVec3> batch_; // the chunks in flight, in job order
	float last_collect_ms_ = 0.0f;
};

} // namespace godot
