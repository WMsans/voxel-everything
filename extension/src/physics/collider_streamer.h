#pragma once
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <deque>
#include <mutex>
#include <vector>
#include "generator/generator.h"
#include "mesh/chunk_residency.h"
#include "render/mesh_service.h"
#include "world/edit_log.h"

namespace godot {

// Turns finished chunk meshes into Jolt static bodies and keeps the set in step with the
// player (spec §6: "PhysicsServer3D direct (no scene-tree nodes) … collision streams in a
// ~64 m radius around the player"). Owns the physics RIDs and nothing else; every pointer is
// borrowed from VoxelWorld.
//
// One single-shape static body per pool slot rather than one body carrying every chunk's
// shape: Jolt rebuilds a body's compound whenever a sub-shape changes, so a 160-shape body
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

	int active_bodies() const { return active_bodies_; }
	int builds_last_frame() const { return builds_last_frame_; }
	int failures() const { return failures_; }
	int queued_results() const { return static_cast<int>(inbox_.size()); }
	float last_build_ms() const { return last_build_ms_; }
	float last_collect_ms() const;
	RID body_of_slot(int slot) const;
	// --- profiling (diagnostic only; see VoxelWorld::debug_perf_stats) ---
	float last_faces_ms() const { return last_faces_ms_; }
	float last_setdata_ms() const { return last_setdata_ms_; }
	float last_body_ms() const { return last_body_ms_; }
	int last_tris() const { return last_tris_; }
	float last_plan_ms() const { return last_plan_ms_; }
	float last_apply_ms() const { return last_apply_ms_; }
	float last_submit_ms() const { return last_submit_ms_; }
	float last_frame_ms() const { return last_frame_ms_; }

private:
	enum BuildOutcome { kBuilt, kEmpty, kFailed };

	BuildOutcome build_shape(int slot, const MeshResult &r);
	void release_slot(int slot);
	void apply_result(const MeshResult &r);

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
	std::deque<MeshResult> inbox_; // collected, not yet turned into shapes
	int max_builds_per_frame_ = 2;
	float build_budget_ms_ = 4.0f;
	int active_bodies_ = 0;
	int builds_last_frame_ = 0;
	int failures_ = 0;
	int overflow_warnings_ = 0;
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
