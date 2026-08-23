#pragma once
// WorldStore — the edit log / override data plane extracted from VoxelWorld
// (spec Phase 2). Owns edits, overrides, the edit-append spine, and the
// EditSink/ConsolidationSink ports; consumers talk only to its public API.
//
// Phase 2b scope (Task 8): the edit-append path moved in verbatim behind the
// sink ports, together with edit_mutex_ + pending_edits_. Occupancy/edit_seq
// move next (Task 9). Construction/teardown order is load-bearing near GPU
// setup: VoxelWorld calls ensure_edit_log()/ensure_overrides()/ensure_residency()
// from ensure_initialized() at exactly the points where the moved statements used
// to sit, clear_residency() from teardown_gpu(), and release_cores() from
// _exit_tree() — same allocation/deallocation sequence as before the split.
#include <atomic>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include <godot_cpp/variant/vector3i.hpp>

#include "generator/edit_ops.h"
#include "generator/volume_set.h"
#include "world/edit_log.h"
#include "world/override_store.h"
#include "world/residency.h"

namespace ve {

// Config snapshot for the world's sizing knobs. Property setters on VoxelWorld
// write it pre-init; pools don't resize after creation, so post-init writes are
// reflected/rejected with the same behavior as before the split (spec §5).
struct WorldConfig {
	godot::Vector3i atlas_bricks{64, 32, 32};
	int max_region_slots = 512;
	int max_brick_jobs = 16384;
	int max_override_bricks = 8192;
	godot::Vector3i world_origin_bricks{0, -64, 0};
	godot::Vector3i world_size_regions{64, 8, 64};
	float residency_radius_m = 96.0f;
};

} // namespace ve

namespace godot {

class VoxelWorld;
class VoxelDebugHooks;

// One edit drained by the streamer: the op plus the regions its append touched/rejected.
// (Moved verbatim from voxel_world.h alongside pending_edits_, its only reader.)
struct PendingEdit {
	ve::EditOp op;
	ve::EditLog::AppendResult result;
};

// Notification port injected at construction (spec §5 Phase 2). Implemented today by a
// VoxelWorld adapter that forwards to the island-manager wiring; Phase 3+ IslandManager
// implements it directly and the adapter dies.
struct EditSink {
	virtual ~EditSink() = default;
	// called with edit_mutex() HELD, after append_edit_locked accepts an op.
	// `notify_islands` is false only for the island manager's own crumble carve: the matter
	// it removes was already labelled UNANCHORED, so nothing that was holding on can be
	// loosened by its going, and enqueueing a window would relabel the same neighbourhood
	// every time a speck of sub-voxel dust is swept up. It is ALSO folded false when the
	// accepted op changed no region field state, so implementors never re-label an
	// untouched neighbourhood.
	virtual void on_edit_appended(const ve::EditOp &op, bool notify_islands) = 0;
};

// Consolidation queue port; initially satisfied by VoxelWorld (Task 12 retargets it to
// ConsolidationCoordinator).
struct ConsolidationSink {
	virtual ~ConsolidationSink() = default;
	// edit_mutex() held (the append path calls this while accepting an op).
	virtual bool queue_consolidation(ve::IVec3 region) = 0;
};

class WorldStore {
	// Temporary strangler-phase friendships: VoxelWorld and the debug facade
	// still read the data plane directly while Tasks 8-13 move their logic
	// behind this boundary. Both disappear when the spec §5 goal ("all friend
	// declarations on VoxelWorld disappear") lands.
	friend class VoxelWorld;
	friend class VoxelDebugHooks;

public:
	explicit WorldStore(const ve::WorldConfig &config);
	~WorldStore();

	ve::EditLog *edit_log() { return edit_log_; }
	ve::OverrideStore *overrides() { return overrides_; }
	ve::VolumeSet &volumes() { return volumes_; }
	ve::RegionResidency *residency() { return residency_; }
	const ve::WorldConfig &config() const { return config_; }

	// --- the spine (moved verbatim from VoxelWorld::append_edit/_locked) ---
	// Tool entry point. Main thread; takes edit_mutex().
	ve::EditLog::AppendResult append_edit(const ve::EditOp &op);
	// Low-level append used by callers that hold edit_mutex() across a whole
	// carve/restore sequence (IslandManager). The caller MUST already hold edit_mutex().
	ve::EditLog::AppendResult append_edit_locked(const ve::EditOp &op,
			bool notify_islands = true);
	int override_table_for_region(ve::IVec3 region) const;

	// Sinks are injected at construction and never re-pointed: append_edit_locked()
	// dereferences both without a guard, exactly as the pre-split body dereferenced its
	// collaborators. WorldStore is only constructed by VoxelWorld, which calls this from
	// its own constructor before any edit can exist.
	void set_sinks(EditSink *edits, ConsolidationSink *consolidation) {
		edit_sink_ = edits;
		consolidation_sink_ = consolidation;
	}
	// Borrowed until Task 9 moves edit_seq into WorldStore; must outlive the store
	// (VoxelWorld owns the atomic and constructs the store first).
	void set_edit_seq(std::atomic<int64_t> *seq) { edit_seq_ = seq; }

	// THE edit mutex; guards the edit log, override tables' append path, pending_edits_,
	// and everything the fan-out touches while an op is accepted.
	// Lock order restated at the new owner (spec §6): Lock order is edit_mutex() ->
	// LodSystem::mutex(): lod_tick never holds LodSystem::mutex() while it calls
	// gather_lod_ops (which takes edit_mutex()), so append_edit_locked can safely take
	// LodSystem::mutex() while already holding edit_mutex(). (LodSystem arrives in Phase 5.)
	std::mutex &edit_mutex() { return edit_mutex_; }

	// --- lazy core creation; call order near GPU setup is load-bearing ---
	// Each body is the verbatim statement pair moved out of
	// VoxelWorld::ensure_initialized() / ensure_physics_initialized().
	void ensure_edit_log(const ve::WorldBounds &bounds) {
		if (!edit_log_)
			edit_log_ = new ve::EditLog(bounds);
	}
	void ensure_overrides(int capacity) {
		if (!overrides_)
			overrides_ = new ve::OverrideStore(capacity);
	}
	void ensure_residency(const ve::WorldBounds &bounds) {
		if (!residency_) {
			ve::ResidencyConfig rcfg;
			rcfg.bounds = bounds;
			rcfg.radius_m = config_.residency_radius_m;
			rcfg.max_region_slots = config_.max_region_slots;
			residency_ = new ve::RegionResidency(rcfg);
		}
	}
	// Slot assignments are meaningless pre-atlas. Called by teardown_gpu()
	// between streamer deletion and island-cull deletion, exactly where
	// `if (residency_) { residency_->clear(); }` used to sit.
	void clear_residency() {
		if (residency_) {
			residency_->clear();
		}
	}
	// CPU cores survive GPU teardown and are deleted together, in the same
	// order _exit_tree() used (residency_ -> edit_log_ -> overrides_).
	void release_cores() {
		if (residency_) {
			delete residency_;
			residency_ = nullptr;
		}
		if (edit_log_) {
			delete edit_log_;
			edit_log_ = nullptr;
		}
		if (overrides_) {
			delete overrides_;
			overrides_ = nullptr;
		}
	}

private:
	ve::WorldConfig config_;
	// The CPU cores are shared with the streaming path and outlive both GPU
	// objects and physics teardown: a re-init re-streams the same world, edits
	// included. This is also what a future save/reload will do (saves ARE the
	// edit log).
	ve::EditLog *edit_log_ = nullptr;
	ve::OverrideStore *overrides_ = nullptr;
	std::map<std::tuple<int, int, int>, int> override_tables_;
	// The authoritative copy of every stored volume. Owned here because it outlives the GPU
	// objects exactly as the edit log does: a re-init re-uploads the same rubble.
	ve::VolumeSet volumes_;
	ve::RegionResidency *residency_ = nullptr;

	// --- moved verbatim from VoxelWorld (Task 8) ---
	std::mutex edit_mutex_;                  // guards the data plane + pending_edits_
	std::vector<PendingEdit> pending_edits_; // appended by tools, drained by the streamer
	std::atomic<int64_t> *edit_seq_ = nullptr; // borrowed from VoxelWorld until Task 9
	EditSink *edit_sink_ = nullptr;
	ConsolidationSink *consolidation_sink_ = nullptr;
};

} // namespace godot
