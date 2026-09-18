#pragma once
// LodSystem — the LoD runtime extracted from VoxelWorld (spec Phase 5): tree/walk/
// pages/pool wiring plus tick() (was VoxelWorld::lod_tick), the fade band and LoD op
// gathering. Takes WorldStore for op gathering through its public API; never holds a
// VoxelWorld*.
//
// Threading (spec §6): tick()/prepare_raster()/prepare_shadow_raster() run on the render
// thread inside the compositor callback; edits mark the tree dirty from main/tool threads
// while holding edit_mutex(); debug hooks read stats from tool threads.
//
// THE lod mutex moved here verbatim from VoxelWorld (Task 15) -- same guard scopes, same
// acquisition sites:
//
// THE lod mutex lives here (Task 15 of the frame-module plan). Acquisition rules for it and
// for WorldStore::edit_mutex(): core/edit_pipeline.h. Nothing takes this mutex while holding
// the edit lock -- record() queues, drain_invalidations() applies.
//
// NOTE: this translation unit is explicitly EXCLUDED from the zero-godot-cpp native test
// build's pure_sources in SConstruct: ensure_lod()/tick() drive GPU pools and raster
// passes (godot-cpp + RenderingDevice headers), so it cannot compile without godot-cpp.

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "core/edit_pipeline.h"
#include "lod/lod_tree.h" // ve::LodKey / ve::LodWalkResult / ve::LodCamera / ve::LodOcclusion
#include "world/edit_log.h"

namespace godot {

class MeshService;
class LodPool;
class RenderOrchestrator;
class RenderingDevice;
class WorldStore;

// What the debug facade reports about the LoD runtime, copied in ONE hold of the lod mutex
// (the hold debug_lod_stats used to take itself, through friendship). Plain data.
struct LodStats {
	int pages_total = 0;
	int pages_free = 0;
	int pages_high_water = 0;
	int chunk_records = 0;
	int chunk_records_used = 0;
	int chunk_records_high_water = 0;
	const char *budget_bound = "none";
	int chunks_resident = 0;
	int dirty_chunks = 0;
	int dirty_levels = 0;
	int draw_pages = 0;
	std::vector<int> draw_page_ids;     // the current cut's page identities, in draw order
	std::vector<int> resident_page_ids; // pages holding at least one quad
	std::vector<ve::LodBuildRequest> requests; // what the last walk still wants built
	int partial_allocations = 0;
	int op_overflow = 0; // LoD builds refused because their visible ops exceed the cap
};

class LodSystem : public ve::InvalidationSink {
public:
	struct Collaborators {
		WorldStore *store = nullptr;
		// Address-of slot into VoxelContext: this system is constructed BEFORE the
		// orchestrator (the orchestrator's teardown interleaves with the pool/tree/page
		// maps below, so it consumes those slots at its construction), meaning *render
		// is null until the world finishes constructing. Re-read at every use; pass
		// pointers are re-fetched per call because teardown/reload recreates them.
		RenderOrchestrator **render = nullptr;
		// Created/destroyed across physics init/teardown cycles; re-read at every use.
		MeshService **mesh = nullptr;
		// ensure_lod()'s lazy-init arm: VoxelWorld::ensure_initialized() via a captureless
		// thunk -- the same pattern RenderOrchestrator uses; no VoxelWorld* is stored.
		void (*ensure_initialized_thunk)(void *) = nullptr;
		void *ensure_initialized_self = nullptr;
	};

	explicit LodSystem(Collaborators handles);

	// THE lod mutex; guards lod_tree_, lod_walk_, lod_pages_of_, lod_page_quads_,
	// lod_overflow_logged_ and lod_pool_ state between the render thread (tick) and
	// main/tool threads (mark-dirty fan-out, debug stats). See core/edit_pipeline.h.
	// (tick never holds mutex() across gather_ops; deferred edit marks are applied after
	// releasing edit_mutex()).
	std::mutex &mutex() { return lod_mutex_; }

	// Was VoxelWorld::lod_tick; render thread (compositor callback).
	void tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ);
	// Where the last walk was run from. The shadow cut is a distance test against exactly
	// this point (LodTree::shadow_visit), so it is also the centre the sun ortho must fit --
	// and having ONE place to read it from is what stops the render path and the debug path
	// fitting two different boxes, which is how the camera-follow shimmer shipped unseen.
	// Written under lod_mutex_ by tick(); false until the first tick.
	bool last_camera(float out[3]) const;
	// Push the current walk's page list (with per-page quad counts) into the raster pass.
	void prepare_raster();
	// One cascade's shadow cut, pushed into the raster pass. Radius and min_level come from
	// ve::sun_cascades(); the caller skips this entirely for a cascade that will not
	// rebuild, which for cascade 2 is most frames.
	void prepare_shadow_raster(float radius, int min_level);
	// The near/far seam for this frame, derived from how far the near field's brick data is
	// actually complete. One source of truth: the composite, the LoD raster and the LoD
	// build gate must all fade at the same two distances or the band belongs to no field.
	void fade_band(float *fade_start, float *fade_end) const;
	// False when the chunk's visible ops exceed kMaxRegionOps (S3c): the caller must refuse
	// the build rather than submit a truncated list.
	bool gather_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out);
	// InvalidationSink: an edit or a consolidation marks the tree dirty. Edit lock held
	// (core/edit_pipeline.h). Task 11 makes this a queue-and-drain.
	void record(const ve::Invalidation &inv) override;
	// Swap the queued marks out under the edit lock, then apply them under mutex(). Called at
	// the top of tick() and by the debug drain. Takes the edit lock: never call it while
	// holding mutex() or the edit lock.
	void drain_invalidations();
	// The _exit_tree() LoD half, verbatim statement-for-statement: pool -> tree ->
	// page maps, exactly where VoxelWorld used to run it (after CPU-core release).
	void teardown();
	// RenderOrchestrator::teardown_gpu()'s LoD step, verbatim: pool, then tree, then the page
	// maps (the tree holds page indices the pool is about to free, and a stale index would be
	// handed to the next chunk). Drops pending edit marks under edit_mutex(), without taking
	// lod_mutex_.
	void release_gpu();

	LodPool *pool() const { return lod_pool_; }

	// Runs ensure_lod() and copies LodStats under mutex(). Tool/main thread.
	LodStats stats();

	// User-facing budgets (VoxelWorld ClassDB properties delegate to these). max_lod_pages_
	// is read once at LodPool::initialize time; lod_builds_per_frame_ clamps each frame's
	// submission batch.
	void set_max_lod_pages(int v) { max_lod_pages_ = v; }
	int max_lod_pages() const { return max_lod_pages_; }
	void set_lod_builds_per_frame(int v) { lod_builds_per_frame_ = v; }
	int lod_builds_per_frame() const { return lod_builds_per_frame_; }
	void set_max_lod_chunk_records(int v) { max_lod_chunk_records_ = v; }
	int max_lod_chunk_records() const { return max_lod_chunk_records_; }

private:
	// lazy: creates/initializes lod_tree_ + lod_pool_ on first use
	void ensure_lod();
	// Assumes lod_mutex_ is held; emits the real page list for the current lod_walk_.
	void prepare_raster_locked();

	WorldStore *store() const { return handles_.store; }
	RenderOrchestrator *render() const { return *handles_.render; }
	MeshService *mesh() const { return *handles_.mesh; }

	Collaborators handles_;

	// Member ORDER mirrors the pre-split block in voxel_world.h.
	int max_lod_pages_ = 32768;
	int max_lod_chunk_records_ = 8192;
	int lod_builds_per_frame_ = 8;
	mutable std::mutex lod_mutex_;
	ve::LodTree *lod_tree_ = nullptr;
	class LodPool *lod_pool_ = nullptr;
	uint32_t lod_frame_ = 0;
	ve::LodWalkResult lod_walk_;
	std::map<ve::LodKey, std::vector<int>> lod_pages_of_;
	std::map<int, int> lod_page_quads_; // page -> number of quads stored in that page
	std::set<ve::LodKey> lod_overflow_logged_; // once-per-chunk overflow diagnostics
	int lod_op_overflow_ = 0; // guarded by lod_mutex_
	// Marks queued by record(), guarded by WorldStore::edit_mutex(); drained by
	// drain_invalidations(). Bounded by ve::merge_or_cap.
	std::vector<ve::Box3<float>> pending_marks_;
	int lod_pressure_ = 0;
	float last_cam_[3] = {};
	bool has_last_cam_ = false;
	// The camera the last walk ran with. shadow_cut() needs the whole LodCamera (it
	// projects chunk AABBs), not just the position, and it must be the SAME camera the walk
	// used or the two cuts choose different levels for the same ground.
	ve::LodCamera lod_shadow_cam_;
};

} // namespace godot
