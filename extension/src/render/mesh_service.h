#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <utility>
#include <map>
#include <tuple>
#include "generator/edit_ops.h"
#include "render/island_extract_pass.h"
#include "render/consolidate_pass.h"
#include "render/lod_build_pass.h"
#include "render/mesh_pass.h"
#include "world/region.h"

namespace godot {

// One chunk of work, owning its op list. MeshJob points at a caller-owned array, which is
// fine inside one call but not across a thread hand-off, so the queue carries the ops.
struct MeshRequest {
	ve::IVec3 chunk{};
	std::vector<ve::EditOp> ops;
};

// The collision mesher, moved off the main thread.
//
// MeshPass drives its own local RenderingDevice: submit(), sync() and buffer_get_data() are
// all blocking, and a batch of two chunks cost 14-17 ms of main-thread stall every frame it
// landed one — measured, and the second largest item in the frame after Jolt's shape build.
// None of that stall is work the main thread has to witness: it is waiting for a GPU that is
// not the renderer's.
//
// A RenderingDevice belongs to the thread that CREATES it (Godot's render-thread guard keys
// off that thread id, which is also why creating one on the main thread while the renderer
// runs threaded yields a device the main thread may not touch). So the device and the
// MeshPass are created, used, and destroyed entirely inside run(), and the main thread only
// ever exchanges plain data with it through the two queues below.
class MeshService {
public:
	~MeshService();

	// Spawns the thread and builds the pass on it. Blocks until that has succeeded or failed,
	// so a caller can report the failure exactly where it used to.
	bool start(const MeshPassConfig &cfg);
	void stop();
	bool is_valid() const { return ready_.load(std::memory_order_acquire); }

	// A batch is accepted only when nothing is queued or running, preserving MeshPass's
	// one-batch-at-a-time contract (the residency's in-flight bookkeeping relies on it).
	bool submit(std::vector<MeshRequest> requests);
	// True while a batch is queued or being meshed.
	bool busy() const { return busy_.load(std::memory_order_acquire); }
	// Moves whatever has finished into `out`. Never blocks on the GPU.
	int collect(std::vector<MeshResult> *out);

	// Extraction shares the worker thread with meshing, in its own queue: an island is a
	// player-visible event and must not wait behind a collider batch, but it is also rare
	// enough that a dedicated thread would idle.
	bool submit_extracts(std::vector<IslandExtractJob> jobs);
	bool extracts_busy() const { return extract_busy_.load(std::memory_order_acquire); }
	int collect_extracts(std::vector<IslandExtractResult> *out);
	// True when the worker has a live IslandExtractPass. Field extractions (island carving)
	// can never succeed without one, so the island manager treats this as a permanent
	// no-progress condition rather than re-queueing work that can only fail.
	bool extraction_available() const {
		return extract_available_.load(std::memory_order_acquire);
	}

	// LoD builds share the worker thread in a third queue, behind colliders: a missing far
	// chunk is a coarse horizon, a missing collider is a hole the player falls through.
	bool submit_lod(std::vector<LodBuildJob> jobs);
	// True while a LoD batch is queued or being built.
	bool lod_busy() const { return lod_busy_.load(std::memory_order_acquire); }
	int collect_lod(std::vector<LodBuildResult> *out);

	bool submit_consolidations(std::vector<ConsolidateJob> jobs);
	int collect_consolidations(std::vector<ConsolidateResult> *out);
	// Publishes all bricks for a region in one worker-thread transaction. The table is
	// repointed only after every upload succeeds, so a partial bake remains invisible.
	bool publish_overrides(const std::vector<int> &slots,
			const std::vector<ve::OverrideBrick> &bricks, ve::IVec3 region, int region_slot,
			int table, const std::vector<std::pair<int, int>> &entries);
	bool publish_override(int slot, const ve::OverrideBrick &brick, ve::IVec3 region,
			int region_slot, int table, const std::vector<std::pair<int, int>> &entries);
	// True when the worker has a live LodBuildPass.
	bool lod_available() const {
		return lod_available_.load(std::memory_order_acquire);
	}
#ifdef DEBUG_ENABLED
	// Test hook: force the availability flag so the engine suite can simulate a permanently
	// unavailable extraction pass.
	void debug_set_extraction_available(bool v) {
		extract_available_.store(v, std::memory_order_release);
	}
	// Test hook: force every field extraction to report failure, simulating a worker pass that
	// exists but cannot extract (as opposed to no pass at all).
	void debug_set_fail_extractions(bool v) {
		fail_extracts_.store(v, std::memory_order_release);
	}
	// Test hook: make submit_extracts() reject a batch even when the worker is idle, so the
	// island manager's rollback path can be exercised deterministically.
	void debug_set_fail_extract_submit(bool v) {
		fail_extract_submit_.store(v, std::memory_order_release);
	}
#else
	// Fail-injection hooks are debug-only: release builds must not be able to drive the
	// mesh service into artificial failure states.
	void debug_set_extraction_available(bool v) { (void)v; }
	void debug_set_fail_extractions(bool v) { (void)v; }
	void debug_set_fail_extract_submit(bool v) { (void)v; }
#endif

	// Copies one stored volume into THIS device's pool, on the worker thread. The main
	// thread's ve::VolumeSet is authoritative; this keeps the mesher's field evaluation --
	// and therefore collision against pasted rubble -- in step with it.
	bool submit_volume(int slot, ve::VolumeData data);
	// Removes any still-pending worker upload for `slot`. Used when a re-merge paste is
	// fully rejected and the out-slot is released/reused before the worker has drained the
	// stale bytes; already-applied uploads are harmless because a later upload for a reused
	// field-referenced slot is always queued before the op that names it reaches the log.
	void discard_pending_volume_upload(int slot);
	// Test hook: report the slots this MeshService accepted through submit_volume since it
	// was started. Used by teardown/reinit tests to verify pinned volumes are replayed into
	// the new worker. Debug-only: release builds do not accumulate the backing vector.
	std::vector<int> debug_submitted_volume_slots() const;

	// Runs `fn` on the worker thread and waits for it. The diagnostic entry points
	// (debug_mesh_diff and friends) are inherently synchronous and must touch the pass on the
	// thread that owns its device; this is how they still can.
	bool run_sync(const std::function<void(MeshPass &)> &fn);

	float last_collect_ms() const;

private:
	void run();

	std::thread thread_;
	mutable std::mutex mu_;
	std::condition_variable cv_;      // signals the worker: work, sync task, or stop
	std::condition_variable done_cv_; // signals the caller: startup settled, sync task done

	MeshPassConfig cfg_;
	std::vector<MeshRequest> pending_;  // submitted, not yet picked up
	std::vector<MeshResult> results_;   // meshed, not yet collected
	struct VolumeUpload {
		int slot = -1;
		ve::VolumeData data;
	};
	IslandExtractPass *extract_ = nullptr;         // worker thread only
	std::vector<IslandExtractJob> pending_extract_;
	std::vector<IslandExtractResult> extract_results_;
	LodBuildPass *lod_ = nullptr;                  // worker thread only
	ConsolidatePass *consolidate_ = nullptr;       // worker thread only
	std::vector<LodBuildJob> pending_lod_;
	std::vector<LodBuildResult> lod_results_;
	std::vector<ConsolidateJob> pending_consolidations_;
	std::vector<ConsolidateResult> consolidate_results_;
	std::vector<VolumeUpload> pending_volumes_;
#ifdef DEBUG_ENABLED
	// Debug-only: accepted volume upload slots, used by debug_submitted_volume_slots().
	// Guarded so release builds do not grow this vector forever across re-merges/restores.
	std::vector<int> submitted_volume_slots_;
#endif
	std::atomic<bool> extract_busy_{false};
	std::atomic<bool> extract_available_{false};
	std::atomic<bool> fail_extracts_{false};
	std::atomic<bool> fail_extract_submit_{false};
	std::atomic<bool> lod_busy_{false};
	std::atomic<bool> lod_available_{false};
	std::atomic<bool> consolidate_busy_{false};
	std::map<std::tuple<int, int, int>, int> override_tables_;
	const std::function<void(MeshPass &)> *sync_fn_ = nullptr;
	bool sync_pending_ = false;
	bool started_ = false;   // startup attempt has settled (ready_ is then meaningful)
	bool stopping_ = false;
	std::atomic<bool> ready_{false};
	std::atomic<bool> busy_{false};
	std::atomic<float> last_collect_ms_{0.0f};
};

} // namespace godot
