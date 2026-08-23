#pragma once
// WorldStore — the edit log / override data plane extracted from VoxelWorld
// (spec Phase 2). Owns edits, overrides, and the FieldGenerator / EditSink
// ports; consumers talk only to its public API.
//
// Phase 2a scope (Task 7): this is the INERT data plane only — the config
// struct plus the five members that outlive GPU teardown exactly as they did
// on VoxelWorld. No mutexes move yet (Task 8), no append path, no occupancy.
// Construction/teardown order is load-bearing near GPU setup: VoxelWorld calls
// ensure_edit_log()/ensure_overrides()/ensure_residency() from
// ensure_initialized() at exactly the points where the moved statements used
// to sit, clear_residency() from teardown_gpu(), and release_cores() from
// _exit_tree() — same allocation/deallocation sequence as before the split.
#include <map>
#include <tuple>

#include <godot_cpp/variant/vector3i.hpp>

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
};

} // namespace godot
