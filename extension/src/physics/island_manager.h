#pragma once
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include "connectivity/components.h"
#include "connectivity/contact_refine.h"
#include "connectivity/flood_fill.h"
#include "generator/generator.h"
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

	int slot_high_water() const {
		return slot_high_water_.load(std::memory_order_relaxed);
	}
	float last_ms() const { return last_ms_; }
	void set_merge_sleep_seconds(float v) { merge_sleep_s_ = v; }
	// Borrowed, not owned; see the member comment.
	void set_generator(const ve::Generator *gen) { gen_ = gen; }
#ifdef DEBUG_ENABLED
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
		if (used) {
			const int high = std::max(slot_high_water_.load(std::memory_order_relaxed), slot + 1);
			slot_high_water_.store(high, std::memory_order_relaxed);
		}
	}
#else
	// Cap/atlas test hooks are debug-only: release builds must not be able to lower the
	// 64-body guardrail or mark atlas slots used.
	void debug_set_max_dynamic_bodies(int v) { (void)v; }
	void debug_set_atlas_slot_used(int slot, bool used) { (void)slot; (void)used; }
#endif
#ifdef DEBUG_ENABLED
	void debug_set_fail_next_spawn(bool fail) { debug_fail_next_spawn_ = fail; }
	// Test hook: make the next carve-rejection restore appear not to cover every carved
	// region, exercising the keep-the-body-alive path without depending on an op-cap race.
	void debug_set_fail_next_restore(bool fail) { debug_fail_next_restore_ = fail; }
	// Test hook: treat the next carve as rejected after at least one box has been accepted,
	// exercising the post-spawn carve-rejection path without depending on an op-cap race.
	void debug_set_fail_next_carve(bool fail) { debug_fail_next_carve_ = fail; }
	// Test hook: make the next re-merge resample fail so the resample backoff path can be
	// exercised without depending on a worker-side failure mode.
	void debug_set_fail_next_resample(bool fail) { debug_fail_next_resample_ = fail; }
	// Test hook: treat the next landed extraction as holding no solid sample, which is what a
	// sheet thinner than the lattice pitch produces. Exercises the crumble path without
	// having to carve a sub-voxel sliver by hand.
	void debug_set_empty_next_extraction(bool v) { debug_empty_next_extraction_ = v; }
	// Test hook: wake an island body after a re-merge resample has been submitted, so the
	// stale-rest-pose guard can be exercised deterministically.
	void debug_wake_body(int index);
	// Diagnostic: the body's full physics-server state plus a downward motion query, for
	// diagnosing islands that do not fall.
	Dictionary debug_body_info(int index);
	// Test hook: offset a live island body and wake it, again for deterministic stale-pose
	// tests. Moving is stronger than waking alone: Jolt may put a motionless body back to
	// sleep before the next poll, but a changed transform always trips the stale guard.
	void debug_offset_body(int index, const Vector3 &offset);
#else
	// Fail-injection hooks are debug-only: release builds must not be able to drive the
	// island manager into the structurally-impossible no-hole restore branch.
	void debug_set_fail_next_spawn(bool fail) { (void)fail; }
	void debug_set_fail_next_restore(bool fail) { (void)fail; }
	void debug_set_fail_next_carve(bool fail) { (void)fail; }
	void debug_set_fail_next_resample(bool fail) { (void)fail; }
	void debug_set_empty_next_extraction(bool v) { (void)v; }
	void debug_wake_body(int index) { (void)index; }
	void debug_offset_body(int index, const Vector3 &offset) { (void)index; (void)offset; }
#endif
	// Not const: the ground probe takes the edit lock.
	Dictionary stats();

private:
	struct PendingWindow {
		// Stable across overlapping-edit merges. note_edit() may expand an existing window
		// (mutating lo/hi/seq), but InFlight and retry/failure bookkeeping copy the window
		// before that happens, so they must match by this id rather than by the mutable AABB.
		int64_t id = 0;
		ve::IVec3 lo{}, hi{}; // inclusive cell AABB the edit could have loosened
		int64_t seq = 0;
		int waited = 0;
		int retry_cooldown = 0; // frames to skip after a transient full-pool refusal
		int extract_failures = 0; // consecutive failed field extractions from this window
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
		// The ops captured for this component at submit time, and the world AABB they were
		// collected from. land_extraction() recomputes the current ops for the same AABB; if
		// a newer edit changed them, the extraction is stale and must not be carved.
		std::vector<ve::EditOp> ops;
		float aabb_lo[3] = {0, 0, 0};
		float aabb_hi[3] = {0, 0, 0};
		// The window this extraction came from, kept so a late refusal (e.g. all island
		// atlas slots full at landing) can re-queue the originating edit.
		PendingWindow window;
	};
	struct Merging {
		int body_index = -1;
		int out_slot = -1;
		// Original birth volume when re-merge reuses the body's own slot. If the paste is
		// fully rejected the slot must be restored to these bytes so the live body keeps its
		// volume; otherwise the slot can be released as before.
		ve::VolumeData source;
		// The rest pose the resample was submitted from. land_resample() refuses to paste if
		// the body has woken or moved since, so a stale paste can never land at the old pose
		// while the live body disappears from the new one.
		Transform3D submitted_transform;
	};
	struct MergeRetry {
		int body_index = -1;
		int cooldown = 0;
		std::vector<ve::IVec3> blocked_regions; // paste regions still at the op cap
		bool permanent = false; // defensive: never re-merge this body again
	};

	bool window_is_fresh(const PendingWindow &w) const;
	int run_connectivity(const PendingWindow &w);
	bool crumble_component(const InFlight &f);
	void land_extraction(const IslandExtractResult &r);
	void land_resample(const IslandExtractResult &r);
	void publish_descriptors();
	void start_merges();
	void queue_retry_window(const PendingWindow &w);
	void note_extract_failure(const PendingWindow &w);
	void note_extract_success(const PendingWindow &w);
	void note_merge_rejected(int body_index, const ve::EditLog::AppendResult &paste);
	void block_merge_permanently(int body_index);
	bool merge_retry_blocked(int body_index);
	int live_body_count() const;
	int free_atlas_slot() const;
	void despawn(int index);

	VoxelWorld *world_ = nullptr;
	// Borrowed from WorldStore via VoxelWorld, exactly like edit_log_. Never owned: the
	// terrain pipeline can swap the world's generator, and a copy here would silently keep
	// generating the old world for collision while the GPU generated the new one.
	const ve::Generator *gen_ = nullptr;
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
	int64_t next_window_id_ = 1;
	// Read by the render thread through VoxelWorld::island_slot_count(), so it is atomic.
	std::atomic<int> slot_high_water_{0};
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
	// Why they were left attached, so a stall can name its own cause instead of being
	// diagnosed by inspection.
	int refused_box_merge_ = 0;
	int refused_lattice_ = 0;
	int refused_op_cap_ = 0;
	int refused_body_cap_ = 0;
	int refused_pool_full_ = 0;
	int refused_unavailable_ = 0;
	int refused_empty_ = 0;
	int components_labelled_ = 0;
	// Loose components the extractor could not represent, carved away rather than left
	// standing as an invisible wedge (see crumble_component).
	int crumbled_ = 0;
#ifdef DEBUG_ENABLED
	// Diagnostic only: every cell box this manager has committed a carve for, so a test can
	// ask whether leftover matter sits inside a carved box or outside every one of them.
	std::vector<ve::CellBox> debug_carved_boxes_;
	// Diagnostic only: "refused_landing" split by the branch that took the decision, so a
	// stall after an extraction comes back can name itself.
	struct LandRefusals {
		int atlas_full = 0, store_failed = 0, no_edit_log = 0, preflight = 0, stale = 0,
			pin_failed = 0, spawn_failed = 0, carve_nothing = 0, carve_restored = 0;
	} debug_land_;
#endif
	bool debug_fail_next_spawn_ = false;
	bool debug_fail_next_restore_ = false;
	bool debug_fail_next_carve_ = false;
	bool debug_fail_next_resample_ = false;
	bool debug_empty_next_extraction_ = false;
	float last_ms_ = 0.0f;
};

} // namespace godot
