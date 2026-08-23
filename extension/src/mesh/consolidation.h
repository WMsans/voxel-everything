#pragma once
// ConsolidationCoordinator — all consolidation_* members plus pump/queue/publish
// logic extracted from VoxelWorld (spec Phase 3). Depends on WorldStore through its
// public API and on the atlas/mesher via injected handles; it never holds a VoxelWorld*.
//
// Threading: consolidation runs across the main thread (frame pump, teardown) and tool
// threads (debug hooks), while the mesher's worker owns the bake itself. Lock order is
// verbatim from VoxelWorld: render lifetime mutex -> WorldStore::edit_mutex() ->
// lod mutex (VoxelWorld::lod_mutex_ today; LodSystem::mutex() after Phase 5).
//
// NOTE: this translation unit is explicitly EXCLUDED from the zero-godot-cpp native test
// build's pure_sources in SConstruct: the moved state machine publishes transactions to
// GpuAtlas and MeshService by design (plan Task 11 takes "mesher/atlas collaborators" in
// its constructor), so it cannot compile without godot-cpp.
#include <functional>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

#include "core/world_store.h"        // ConsolidationSink port + WorldStore public API
#include "render/consolidate_pass.h" // ConsolidateJob member
#include "world/override_store.h"    // ve::OverrideBrick members
#include "world/region.h"

namespace ve {
class LodTree; // forward: the coordinator only ever holds a pointer to it
}

namespace godot {

class GpuAtlas;
class MeshService;
class RenderingDevice;
class WorldStreamer;
class WorldStore;

class ConsolidationCoordinator : public ConsolidationSink { // satisfies the port from Task 8
public:
	// Handles, not ownership. The atlas/mesher/streamer/lod-tree are created lazily and
	// destroyed across ensure_initialized()/ensure_physics_initialized()/teardown cycles,
	// so the coordinator receives ADDRESSES of VoxelWorld's fields and re-reads them at
	// every use instead of caching pointers that a teardown would strand.
	struct Collaborators {
		GpuAtlas **atlas = nullptr;
		MeshService **mesh = nullptr;
		WorldStreamer **streamer = nullptr;
		ve::LodTree **lod_tree = nullptr;
		std::mutex *lod_mutex = nullptr;
		// Collider remesh queue guarded by edit_mutex(); drained by physics_tick().
		std::vector<std::pair<ve::IVec3, ve::IVec3>> *pending_dirty = nullptr;
		// Device selection seam (use_local_device_/main_rd_/local_rd_ stay on the world).
		const bool *use_local_device = nullptr;
		RenderingDevice **main_rd = nullptr;
		RenderingDevice **local_rd = nullptr;
		// Frame-pump lifetime guard: prevents teardown from racing the transaction.
		std::mutex *render_lifetime_mutex = nullptr;
		const bool *render_shutting_down = nullptr;
	};

	ConsolidationCoordinator(WorldStore *store, Collaborators handles);

	// One non-blocking frame-pump step; was VoxelWorld::pump_consolidation (called from
	// _process every frame, unconditionally).
	void pump_async();
	// Spin until no transaction is in flight (or a 2 s deadline expires), pumping between
	// sleeps; was debug_wait_consolidation's loop.
	void wait();
	// pump_async + wait; was debug_pump_consolidation ("what _process does, to completion").
	void pump();
	// Synchronous single-region consolidate for diagnostics; was
	// VoxelDebugHooks::debug_consolidate_region's body.
	bool force_region(ve::IVec3 region);
	// Worker-teardown rollback: restore old consumer state for an in-flight transaction
	// before the mesher's worker goes away. Caller MUST hold edit_mutex(); called from
	// VoxelWorld::teardown_physics exactly where the block used to sit.
	void rollback_in_flight_for_worker_teardown();

	// Stats accessors (HUD/benchmark read them through the debug facade). Written only
	// under edit_mutex(); readers hold it exactly as the pre-split field readers did.
	int consolidated_count() const { return consolidation_count_; }
	int refusals() const { return consolidation_refusals_; }
	int queue_refusals() const { return consolidation_queue_refusals_; }

private:
	GpuAtlas *atlas() const { return *handles_.atlas; }
	MeshService *mesh() const { return *handles_.mesh; }
	WorldStreamer *streamer() const { return *handles_.streamer; }
	ve::LodTree *lod_tree() const { return *handles_.lod_tree; }
	std::mutex &lod_mutex() const { return *handles_.lod_mutex; }
	std::vector<std::pair<ve::IVec3, ve::IVec3>> &pending_dirty() const {
		return *handles_.pending_dirty;
	}
	RenderingDevice *device() const {
		return *handles_.use_local_device ? *handles_.local_rd : *handles_.main_rd;
	}

	// edit_mutex must be held (ConsolidationSink port satisfied for WorldStore's spine).
	bool queue_consolidation(ve::IVec3 region) override;
	void requeue_consolidation_locked(ve::IVec3 region);

	// Consolidation is deliberately one-region-at-a-time. The worker owns the bake; the main
	// thread owns this queue and publishes the completed transaction between frames.
	std::vector<ve::IVec3> consolidation_queue_;
	bool consolidation_in_flight_ = false;
	ConsolidateJob consolidation_job_;
	int consolidation_table_ = -1;
	int consolidation_old_table_ = -1;
	std::vector<std::pair<int, int>> consolidation_old_entries_;
	std::vector<std::pair<int, int>> consolidation_entries_;
	std::vector<int> consolidation_old_slots_;
	std::vector<ve::OverrideBrick> consolidation_old_bricks_;
	std::vector<ve::IVec3> consolidation_newly_acquired_;
	std::vector<int> consolidation_slots_;
	std::vector<ve::OverrideBrick> consolidation_baked_;
	bool consolidation_publish_in_flight_ = false;
	int consolidation_count_ = 0;
	int consolidation_refusals_ = 0;
	int consolidation_queue_refusals_ = 0;
	bool consolidation_queue_refusal_logged_ = false;

	WorldStore *store_;
	Collaborators handles_;
};

} // namespace godot
