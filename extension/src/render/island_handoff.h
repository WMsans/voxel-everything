#pragma once
// IslandHandoff -- island and field-volume bytes, normal releases and slot descriptors on their
// way from the main thread to the render device (spec 2026-09-14 §3.2). Pure: no godot-cpp, in
// the native test build. Owned by RenderOrchestrator and drained by
// RenderOrchestrator::drain_island_uploads on the render thread, before the streamer runs: an
// op that names a volume must never be evaluated before the volume is there.
//
// Locking: mutex_ is a LEAF. It is taken only inside these methods, never while calling out and
// never around another acquisition; callers may already hold WorldStore::edit_mutex(), so the
// order is edit_mutex -> (handoff leaf). Call sites: IslandManager (queue_*, discard_, publish_,
// release_volume_slot), VoxelWorld::teardown_physics (drop_for_physics_teardown),
// RenderOrchestrator::drain_island_uploads (take), RenderOrchestrator::teardown_gpu
// (reset_debug_slots), and the debug facade's island fixtures and diagnostics.
// The slot high-water marks are atomics so the render thread reads them without a lock.
#include <atomic>
#include <mutex>
#include <vector>
#include "generator/volume_set.h"

namespace godot {

// Spec §5's guardrail: "<=32 island bodies".
inline constexpr int kMaxIslands = 32;

// One live island as the raymarcher needs to see it. Written every frame from the body's
// transform, which is why nothing here is a Godot type: IslandManager fills it on the main
// thread and IslandAtlas uploads it on the render thread.
struct IslandSlotDesc {
	bool live = false;
	// The AUTHORITATIVE ve::VolumeSet slot whose bytes this island renders from. Task 6
	// removed IslandAtlas's duplicate SDF/material buffers: the raymarcher reads shared
	// atlas.volumes() buffers indexed by THIS slot (the descriptor's unused integer lane),
	// while the atlas slot keeps selecting descriptor/mip/tile-mask entries.
	int volume_slot = -1;
	// Local -> world rotation, COLUMN major: basis[a] is the world direction of local +a.
	// (ve::resample_volume takes the same rotation ROW major; the two conversions are
	// spelled out where they happen so the transpose is never implicit.)
	float basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	float origin[3] = {0, 0, 0};         // body translation, world
	float lattice_origin[3] = {0, 0, 0}; // the lattice's minimum corner in LOCAL space
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	float aabb_lo[3] = {0, 0, 0}; // world AABB of the rotated lattice box (Task 11's cull)
	float aabb_hi[3] = {0, 0, 0};

	// Fills aabb_lo/hi from the other fields. Called by whoever writes the descriptor.
	void recompute_world_aabb();
};

class IslandHandoff {
public:
	struct Upload {
		int atlas_slot = -1;          // island mip/descriptor entry; -1 = field-volume only
		int volume_slot = -1;         // authoritative ve::VolumeSet slot
		bool to_island_atlas = false; // true = also upload the island min-max mip
		ve::VolumeData data;
	};
	// Everything one drain hands to the render device.
	struct Batch {
		std::vector<Upload> uploads;
		std::vector<int> normal_releases;
		std::vector<IslandSlotDesc> descs; // the last published set, every drain
		bool descs_dirty = false;          // true once per publish
	};

	void queue_island(int atlas_slot, int volume_slot, const ve::VolumeData &data);
	void queue_field_volume(int volume_slot, const ve::VolumeData &data);
	// Removes queued field-volume uploads for the slot (never island uploads).
	void discard_field_volume(int volume_slot);
	void queue_normal_release(int volume_slot);
	void publish_descriptors(std::vector<IslandSlotDesc> descs);
	Batch take();
	// Physics teardown: stale island uploads and descriptors must not reach the next pool,
	// but a field-volume upload whose slot an op already pins is part of the surviving CPU
	// world and must be mirrored into any new GPU pool.
	void drop_for_physics_teardown(const ve::VolumeSet &volumes);

	// IslandManager's slot high-water mark (it stores here; 0 while no manager exists).
	std::atomic<int> manager_slots{0};
	// The debug facade's placed-island latch: raises the mark to slot + 1.
	void note_debug_slot(int slot);
	void reset_debug_slots();
	// High-water mark, not a population: max(debug, manager), or 0 with islands disabled.
	int slot_count(bool islands_enabled) const;

	int pending_uploads() const;
	bool descs_dirty() const;
	int field_volume_uploads() const;
	void note_field_volume_uploaded();

private:
	mutable std::mutex mutex_;
	std::vector<Upload> uploads_;
	std::vector<int> normal_releases_;
	std::vector<IslandSlotDesc> descs_;
	bool descs_dirty_ = false;
	std::atomic<int> debug_slots_{0};
	std::atomic<int> field_volume_uploads_{0};
};

// Releases an authoritative volume slot AND queues the render-thread teardown of its compact
// normals. Pinned slots are refused by VolumeSet::release() and keep their normals (a pasted
// volume-add still names it). Returns release()'s result.
bool release_volume_slot(ve::VolumeSet &volumes, IslandHandoff &handoff, int slot);

} // namespace godot
