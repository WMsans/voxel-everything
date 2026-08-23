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
//	Lock order is WorldStore::edit_mutex() -> LodSystem::mutex(): tick never holds
//	lod_mutex_ across gather_ops (which takes edit_mutex()), so append_edit_locked can
//	take LodSystem::mutex() while already holding edit_mutex().
//
// NOTE: this translation unit is explicitly EXCLUDED from the zero-godot-cpp native test
// build's pure_sources in SConstruct: ensure_lod()/tick() drive GPU pools and raster
// passes (godot-cpp + RenderingDevice headers), so it cannot compile without godot-cpp.

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "lod/lod_tree.h" // ve::LodKey / ve::LodWalkResult / ve::LodCamera / ve::LodOcclusion
#include "world/edit_log.h"

namespace godot {

class MeshService;
class RenderOrchestrator;
class RenderingDevice;
class VoxelDebugHooks;
class WorldStore;

class LodSystem {
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
		const std::atomic<bool> *near_field_enabled = nullptr;
		// ensure_lod()'s lazy-init arm: VoxelWorld::ensure_initialized() via a captureless
		// thunk -- the same pattern RenderOrchestrator uses; no VoxelWorld* is stored.
		void (*ensure_initialized_thunk)(void *) = nullptr;
		void *ensure_initialized_self = nullptr;
	};

	explicit LodSystem(Collaborators handles);

	// THE lod mutex; guards lod_tree_, lod_walk_, lod_pages_of_, lod_page_quads_,
	// lod_overflow_logged_ and lod_pool_ state between the render thread (tick) and
	// main/tool threads (mark-dirty fan-out, debug stats). Lock order versus the edit
	// path is restated at WorldStore::edit_mutex(): edit_mutex -> LodSystem::mutex()
	// (tick never holds mutex() across gather_ops, so append_edit_locked can take it
	// while holding edit_mutex).
	std::mutex &mutex() { return lod_mutex_; }

	// Address-of slots consumed by RenderOrchestrator's teardown interleaving and
	// ConsolidationCoordinator's dirty-marking handles. The pool/tree are created lazily
	// and destroyed across teardown cycles, so collaborators hold these addresses and
	// re-read them at every use instead of caching stranded pointers.
	std::mutex *mutex_slot() { return &lod_mutex_; }
	ve::LodTree **tree_slot() { return &lod_tree_; }
	class LodPool **pool_slot() { return &lod_pool_; }
	std::map<ve::LodKey, std::vector<int>> *pages_of_slot() { return &lod_pages_of_; }
	std::map<int, int> *page_quads_slot() { return &lod_page_quads_; }
	std::set<ve::LodKey> *overflow_logged_slot() { return &lod_overflow_logged_; }

	// Was VoxelWorld::lod_tick; render thread (compositor callback).
	void tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ);
	// Push the current walk's page list (with per-page quad counts) into the raster pass.
	void prepare_raster();
	void prepare_shadow_raster();
	// The near/far seam for this frame, derived from how far the near field's brick data is
	// actually complete. One source of truth: the composite, the LoD raster and the LoD
	// build gate must all fade at the same two distances or the band belongs to no field.
	void fade_band(float *fade_start, float *fade_end) const;
	// Gathers the ops that can affect a LoD chunk: its AABB padded by two cells, flattened
	// across regions in global append order, truncated to a chronological prefix (M4 errata 1).
	// Reads region edits through WorldStore's public API; takes edit_mutex(). MUST NOT be
	// called with mutex() held (lock order -- see class comment).
	void gather_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out);
	// Append fan-out (was the LoD tail of VoxelWorld::append_edit_locked, which runs with
	// edit_mutex() held): marks the touched world AABB dirty at every level, taking
	// mutex() while edit_mutex() is held (safe per the lock order above).
	void note_edit(const ve::EditOp &op);
	// The _exit_tree() LoD half, verbatim statement-for-statement: pool -> tree ->
	// page maps, exactly where VoxelWorld used to run it (after CPU-core release).
	void teardown();

	LodPool *pool() const { return lod_pool_; }

	// User-facing budgets (VoxelWorld ClassDB properties delegate to these). max_lod_pages_
	// is read once at LodPool::initialize time; lod_builds_per_frame_ clamps each frame's
	// submission batch.
	void set_max_lod_pages(int v) { max_lod_pages_ = v; }
	int max_lod_pages() const { return max_lod_pages_; }
	void set_lod_builds_per_frame(int v) { lod_builds_per_frame_ = v; }
	int lod_builds_per_frame() const { return lod_builds_per_frame_; }

private:
	// Temporary Task-15 surface: the debug facade pokes the moved members directly today,
	// exactly as it poked VoxelWorld's before the move. EXPIRY: dies with the facade's
	// world_ back-reference in the plan's final cleanup task (Task 16), alongside the
	// friend declaration on VoxelWorld -- see task-13-report's friend table.
	friend class VoxelDebugHooks;

	// lazy: creates/initializes lod_tree_ + lod_pool_ on first use
	void ensure_lod();
	// Assumes lod_mutex_ is held; emits the real page list for the current lod_walk_.
	void prepare_raster_locked();

	WorldStore *store() const { return handles_.store; }
	RenderOrchestrator *render() const { return *handles_.render; }
	MeshService *mesh() const { return *handles_.mesh; }
	ve::WorldBounds world_bounds() const; // store-config projection, as VoxelWorld's

	Collaborators handles_;

	// Member ORDER mirrors the pre-split block in voxel_world.h.
	int max_lod_pages_ = 32768;
	int lod_builds_per_frame_ = 8;
	std::mutex lod_mutex_;
	ve::LodTree *lod_tree_ = nullptr;
	class LodPool *lod_pool_ = nullptr;
	uint32_t lod_frame_ = 0;
	ve::LodWalkResult lod_walk_;
	std::map<ve::LodKey, std::vector<int>> lod_pages_of_;
	std::map<int, int> lod_page_quads_; // page -> number of quads stored in that page
	std::set<ve::LodKey> lod_overflow_logged_; // once-per-chunk overflow diagnostics
	int lod_pressure_ = 0;
};

} // namespace godot
