#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "generator/volume_set.h"
#include "render/island_handoff.h"

namespace godot {

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
	// Whether this slot's descriptor currently says live. Read by RenderOrchestrator's
	// island_live_mask(), which must agree with upload_descriptors/clear_slot exactly -- so
	// it is asked of the same array they write rather than tracked a second time.
	bool slot_live(int slot) const {
		return slot >= 0 && slot < kMaxIslands && slot_live_[slot];
	}

	// Device-level: record before compute_list_begin. Builds/uploads ONLY the slot's
	// min-max mip; the SDF/material/normal bytes are uploaded by the caller into the
	// shared GpuAtlas pools exactly once. `opaque` builds the mip of the OPAQUE VIEW, which
	// is what the marcher must skip by -- the surface under a transparent material is a label
	// boundary the union lattice has no min/max for.
	bool upload_mip(RenderingDevice *rd, int slot, const ve::VolumeData &data, bool opaque);
	void upload_descriptors(RenderingDevice *rd, const IslandSlotDesc *descs, int count,
			bool transparency_enabled);
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
