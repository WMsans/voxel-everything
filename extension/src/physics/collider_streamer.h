#pragma once
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <array>
#include <deque>
#include <mutex>
#include <vector>
#include "generator/generator.h"
#include "mesh/chunk_residency.h"
#include "mesh/octant_split.h"
#include "render/mesh_service.h"
#include "world/edit_log.h"

namespace godot {

// Turns finished chunk meshes into Jolt static bodies and keeps the set in step with the
// player (spec §6: "PhysicsServer3D direct (no scene-tree nodes) … collision streams in a
// ~64 m radius around the player"). Owns the physics RIDs and nothing else; every pointer is
// borrowed from VoxelWorld.
//
// One single-shape static body per populated octant slot rather than one body carrying every
// chunk's shape: empty octants retain only their flat-vector slot, with no PhysicsServer body.
// Jolt rebuilds a body's compound whenever a sub-shape changes, so a 160-shape body
// would rebuild itself two or three times a second while streaming, and body_remove_shape
// renumbers everything after it. See the plan's Deliberate Decisions.
//
// Main thread only, like PhysicsServer3D itself and like the residency it drives. The render
// thread's WorldStreamer touches none of this.
class ColliderStreamer {
public:
	~ColliderStreamer();

	void initialize(ve::ChunkResidency *chunks, ve::EditLog *edit_log, std::mutex *edit_mutex,
			MeshService *mesh, int max_slots);
	void teardown();
	void set_space(RID space);
	void set_shape_builds_per_frame(int v) { max_builds_per_frame_ = v; }
	// Spec §6 asks for "a ~64 m radius around the player + SMALL bubbles around active
	// bodies". The bubble only has to reach the ground a body is about to land on, and its
	// cost is not local: ve::ChunkResidency::update scans one ball per centre and takes the
	// distance to EVERY centre for each chunk it visits, so a full-size bubble per body makes
	// the plan quadratic in the live body count (measured: 0.8 ms at one centre, 37 ms at
	// 64) and fills the chunk pool with rubble's surroundings instead of the player's.
	//
	// The floor on the radius is the lead time a falling body needs: the bubble travels with
	// it, so 12 m at terminal-ish speed is ~0.4 s to mesh the ground below, which the
	// two-chunks-a-frame budget covers several times over.
	void set_body_bubble_radius_m(float v) { bubble_radius_m_ = v; }
	// Wall-clock ceiling for step 2 of run_frame. The first build of a frame always runs, so
	// this bounds the queue drain rate, not one chunk's cost.
	void set_shape_build_budget_ms(float v) { build_budget_ms_ = v; }

	// One frame of collider maintenance around the given centre: land finished meshes, plan,
	// release what left the ball, submit the next batch. Returns the number of actions taken,
	// so a caller (or a test) can tell a settled world from a busy one.
	int run_frame(float cx, float cy, float cz);
	// Same, with additional physics-bubble centres (3 floats each) around active bodies.
	// The player centre is always the first bubble; the extra centres extend the residency.
	int run_frame(float cx, float cy, float cz, const float *extra_centers, int extra_count);

	// A body's public meaning is a resident chunk, not the number of octant bodies used to
	// represent it. The raw count is exposed separately for diagnostics and benchmarks.
	int active_bodies() const { return active_bodies_; }
	int bodies_in_space() const;
	int builds_last_frame() const { return builds_last_frame_; }
	int max_build_tris() const { return max_build_tris_; }
	int max_chunk_tris() const { return max_chunk_tris_; }
	// Diagnostic only: per-slot generation counter bumped for every submitted mesh result, and
	// the residency state of a chunk, so a test can ask whether a chunk's collider ever got
	// rebuilt after a given edit. `state` is the ChunkResidency slot_state char, -1 when the
	// chunk is not resident.
	int build_count_of_chunk(ve::IVec3 c) const;
	int chunk_state(ve::IVec3 c) const;
	bool chunk_in_flight(ve::IVec3 c) const;
	// Diagnostic only: op count of the most recent build SUBMITTED for the chunk's slot, so a
	// stale collider can be tied to the exact op list its mesh was built from.
	int last_submit_op_count(ve::IVec3 c) const;
	int failures() const { return failures_; }
	int queued_results() const { return static_cast<int>(inbox_.size()); }
	// Maximum duration of one octant build call in the last frame; never a frame sum.
	float last_build_ms() const { return last_build_ms_; }
	float last_collect_ms() const;
	RID body_of_slot(int slot) const;
	// Diagnostic-only snapshot for one chunk: the eight raw body slots/RIDs and any staged
	// replacement progress. It has no production-path side effects.
	Dictionary debug_chunk_octants(ve::IVec3 c) const;
	// --- profiling (diagnostic only; see VoxelWorld::debug_perf_stats) ---
	float last_faces_ms() const { return last_faces_ms_; }
	// Maximum duration of one shape_set_data call in the last frame; never an octant sum.
	float last_setdata_ms() const { return last_setdata_ms_; }
	float last_body_ms() const { return last_body_ms_; }
	int last_tris() const { return last_tris_; }
	float last_plan_ms() const { return last_plan_ms_; }
	float last_apply_ms() const { return last_apply_ms_; }
	float last_submit_ms() const { return last_submit_ms_; }
	float last_frame_ms() const { return last_frame_ms_; }

private:
	enum BuildOutcome { kBuilt, kEmpty, kFailed };

	struct PendingBuild {
		int slot = -1;
		MeshResult result;
		std::array<std::vector<uint32_t>, ve::kColliderOctants> bins;
		std::array<RID, ve::kColliderOctants> staged_shapes;
		int next_octant = 0;
		int geometry_octants = 0;
	};

	BuildOutcome build_octant(PendingBuild &pending, int octant);
	bool commit_pending(PendingBuild &pending);
	void discard_pending(PendingBuild &pending);
	void release_slot(int slot);
	void enqueue_result(MeshResult &&r);
	void free_slot_resources(int slot);

	ve::ChunkResidency *chunks_ = nullptr;
	ve::EditLog *edit_log_ = nullptr;
	std::mutex *edit_mutex_ = nullptr;
	MeshService *mesh_ = nullptr;
	// Spec §9 defers a configurable generator; when G becomes one, this moves to VoxelWorld
	// and is handed in, exactly like the edit log.
	ve::AnalyticGenerator gen_;

	RID space_;
	std::vector<RID> bodies_;
	std::vector<RID> shapes_;
	std::vector<char> in_space_;
	std::vector<int> build_counts_;
	std::vector<int> last_submit_ops_;
	std::deque<MeshResult> inbox_; // collected, not yet split into octant builds
	std::deque<PendingBuild> pending_; // staged replacements; old bodies remain live until commit
	int max_builds_per_frame_ = 2;
	float bubble_radius_m_ = 12.0f;
	float build_budget_ms_ = 4.0f;
	int active_bodies_ = 0;
	int builds_last_frame_ = 0;
	int failures_ = 0;
	int overflow_warnings_ = 0;
	int max_build_tris_ = 0;
	int max_chunk_tris_ = 0;
	float last_build_ms_ = 0.0f;
	float last_faces_ms_ = 0.0f;
	float last_setdata_ms_ = 0.0f;
	float last_body_ms_ = 0.0f;
	int last_tris_ = 0;
	float last_plan_ms_ = 0.0f;
	float last_apply_ms_ = 0.0f;
	float last_submit_ms_ = 0.0f;
	float last_frame_ms_ = 0.0f;
};

} // namespace godot
