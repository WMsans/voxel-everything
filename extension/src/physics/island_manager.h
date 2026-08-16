#pragma once
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <deque>
#include <mutex>
#include <vector>
#include "connectivity/components.h"
#include "connectivity/contact_refine.h"
#include "connectivity/flood_fill.h"
#include "physics/island_body.h"
#include "render/island_atlas.h"
#include "render/island_extract_pass.h"
#include "world/edit_log.h"

namespace godot {

// Spec §5's "<=64 total active dynamic bodies". Kept in the header so the test hook can
// expose a lower cap without duplicating the magic number.
inline constexpr int kMaxDynamicBodies = 64;

class VoxelWorld;

// Spec §5, orchestrated. One pass per frame, in this order:
//
//   1. tick the bodies (sleep clocks, debris transforms) and publish their descriptors
//   2. collect finished extractions and resamples -> carve + spawn, or paste + despawn
//   3. if an edit has loosened something and the occupancy grid has caught up, run
//      connectivity ONCE (spec §5: "simultaneous blasts can't race") and submit extractions
//   4. re-merge whatever has slept long enough
//
// run_frame is main-thread only. note_edit may be called from a tool thread while
// VoxelWorld::append_edit holds the edit mutex, so the pending-window queue has its own
// small mutex instead of being touched from two threads unsynchronised.
class IslandManager {
public:
	~IslandManager();

	void initialize(VoxelWorld *world);
	void teardown();

	int run_frame(float dt, const Vector3 &center); // actions taken
	// Called from VoxelWorld::append_edit for every SDF-changing op, including the manager's
	// own carves: removing an island can unsupport the next piece up, and that cascade is
	// the behaviour spec §5 describes, not a bug.
	void note_edit(const ve::EditOp &op, int64_t seq);

	int slot_high_water() const { return slot_high_water_; }
	float last_ms() const { return last_ms_; }
	void set_merge_sleep_seconds(float v) { merge_sleep_s_ = v; }
	void debug_set_max_dynamic_bodies(int v) {
		// Test hook: keep the guardrail sane. Clamping (rather than rejecting) keeps tests
		// that pass an absurd value from silently disabling the body cap.
		max_dynamic_bodies_ = v < 1 ? 1 : (v > kMaxDynamicBodies ? kMaxDynamicBodies : v);
	}
	void debug_set_atlas_slot_used(int slot, bool used) {
		// Test hook for the 32-island atlas ceiling. Out-of-range slots are ignored; used
		// may only be set for slots the manager can actually hand out.
		if (slot < 0 || slot >= kMaxIslands) return;
		atlas_used_[static_cast<size_t>(slot)] = used ? 1 : 0;
		if (used) slot_high_water_ = std::max(slot_high_water_, slot + 1);
	}
	void debug_set_fail_next_spawn(bool fail) { debug_fail_next_spawn_ = fail; }
	// Test hook: make the next re-merge resample fail so the resample backoff path can be
	// exercised without depending on a worker-side failure mode.
	void debug_set_fail_next_resample(bool fail) { debug_fail_next_resample_ = fail; }
	// Not const: the ground probe takes the edit lock.
	Dictionary stats();

private:
	struct PendingWindow {
		ve::IVec3 lo{}, hi{}; // inclusive cell AABB the edit could have loosened
		int64_t seq = 0;
		int waited = 0;
		int retry_cooldown = 0; // frames to skip after a transient full-pool refusal
		float impulse_from[3] = {0, 0, 0}; // the edit's centre, for the radial kick
		float impulse_scale = 0.0f;
	};
	struct InFlight {
		int id = -1;
		std::vector<ve::CellBox> boxes;
		int volume_slot = -1;
		float origin[3] = {0, 0, 0};
		float voxel = 0.0f;
		int dim = 0;
		float impulse[3] = {0, 0, 0};
		// The window this extraction came from, kept so a late refusal (e.g. all island
		// atlas slots full at landing) can re-queue the originating edit.
		PendingWindow window;
	};
	struct Merging {
		int body_index = -1;
		int out_slot = -1;
	};
	struct MergeRetry {
		int body_index = -1;
		int cooldown = 0;
		std::vector<ve::IVec3> blocked_regions; // paste regions still at the op cap
	};

	bool window_is_fresh(const PendingWindow &w) const;
	int run_connectivity(const PendingWindow &w);
	void land_extraction(const IslandExtractResult &r);
	void land_resample(const IslandExtractResult &r);
	void publish_descriptors();
	void start_merges();
	void queue_retry_window(const PendingWindow &w);
	void note_merge_rejected(int body_index, const ve::EditLog::AppendResult &paste);
	bool merge_retry_blocked(int body_index);
	int live_body_count() const;
	int free_atlas_slot() const;
	void despawn(int index);

	VoxelWorld *world_ = nullptr;
	ve::AnalyticGenerator gen_;
	std::mutex windows_mutex_; // guards windows_ against note_edit from tool threads
	std::deque<PendingWindow> windows_;
	std::vector<InFlight> in_flight_;
	std::vector<Merging> merging_;
	std::vector<MergeRetry> merge_retries_;
	// A SLOT POOL, not a list: Merging::body_index outlives a frame, so a despawn nulls its
	// entry and the next spawn reuses it. Erasing would renumber every body after it and
	// silently re-merge the wrong one.
	std::vector<IslandBody *> bodies_;
	std::vector<char> atlas_used_;
	ve::ComponentConfig comp_cfg_;
	ve::ContactRefineConfig refine_cfg_;
	int next_id_ = 1;
	int slot_high_water_ = 0;
	int max_dynamic_bodies_ = kMaxDynamicBodies;
	float merge_sleep_s_ = 2.0f; // spec §5: "Body sleeps ~2s -> re-merge"
	// Counters the HUD, the benchmark and tests/test_connectivity.gd read.
	// Where the last re-merge landed, for stats()'s ground probe.
	float last_merge_xz_[2] = {0.0f, 0.0f};
	int connectivity_runs_ = 0;
	int islands_spawned_ = 0;
	int debris_spawned_ = 0;
	int islands_merged_ = 0;
	int refused_ = 0; // components left attached because a pool was full
	bool debug_fail_next_spawn_ = false;
	bool debug_fail_next_resample_ = false;
	float last_ms_ = 0.0f;
};

} // namespace godot
