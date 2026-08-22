#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
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

// The render device's per-island state: the min-max chain and the descriptor array the
// shader indexes. Since Task 6 there is NO duplicate VolumePool here -- SDF/material/normal
// bytes live once, in GpuAtlas's authoritative volume pool and stored-normal pool.
//
// Storage buffers rather than 3D textures for exactly one reason: RenderingDevice can only
// texture_update a whole layer, so a per-slot texture upload would need a staging texture
// and a texture_copy, while a buffer takes a plain offset buffer_update -- and the
// raymarcher already reconstructs trilinearly by hand for bricks, so nothing is lost.
class IslandAtlas {
public:
	~IslandAtlas();

	bool initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return mip_.is_valid() && desc_.is_valid(); }

	RID mip_buffer() const { return mip_; }
	RID desc_buffer() const { return desc_; }
	// A one-entry all-ones tile mask, bound whenever the cull pass has not produced one.
	RID fallback_mask() const { return fallback_mask_; }
	// Number of slots whose descriptor currently says live. This is a population count, not
	// a high-water mark; VoxelWorld::island_slot_count() is the latter.
	int live_count() const { return live_count_; }

	// Device-level: record before compute_list_begin. Builds/uploads ONLY the slot's
	// min-max mip; the SDF/material/normal bytes are uploaded by the caller into the
	// shared GpuAtlas pools exactly once.
	bool upload_mip(RenderingDevice *rd, int slot, const ve::VolumeData &data);
	void upload_descriptors(RenderingDevice *rd, const IslandSlotDesc *descs, int count);
	// Marks the slot dead in the descriptor array. The bytes are left as they are: nothing
	// reads a slot whose descriptor says it is not live.
	void clear_slot(RenderingDevice *rd, int slot);

private:
	RenderingDevice *rd_ = nullptr;
	RID mip_, desc_, fallback_mask_;
	int live_count_ = 0;
	bool slot_live_[kMaxIslands] = {};
};

} // namespace godot
