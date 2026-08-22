#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include <utility>
#include <map>
#include <tuple>
#include "connectivity/occupancy.h"
#include "generator/edit_ops.h"
#include "render/volume_pool.h"
#include "render/override_pool.h"
#include "render/stored_normal_pool.h"
#include "world/brick_flags.h"
#include "world/edit_log.h"
#include "world/region.h"
#include "world/override_store.h"

namespace godot {

struct GpuAtlasConfig {
	ve::IVec3 atlas_bricks{64, 32, 32};
	int max_region_slots = 512;
	int max_brick_jobs = 16384;
	int max_override_bricks = OverridePool::kDefaultCapacity;
	// Hard total for the compact-normal pool (packed payload + both offset tables).
	uint32_t normal_pool_bytes = StoredNormalPool::kDefaultBudgetBytes;
	ve::WorldBounds bounds{};
};

// Owns every GPU resource the voxel world needs. Nothing else creates or frees RIDs in the
// world path, so teardown order lives in exactly one place.
class GpuAtlas {
public:
	~GpuAtlas();

	bool initialize(RenderingDevice *rd, const GpuAtlasConfig &cfg);
	void teardown();
	bool is_valid() const { return sdf_atlas_.is_valid(); }

	const GpuAtlasConfig &config() const { return cfg_; }
	int atlas_slot_count() const {
		return cfg_.atlas_bricks.x * cfg_.atlas_bricks.y * cfg_.atlas_bricks.z;
	}
	int region_map_entries() const {
		return cfg_.bounds.size_regions.x * cfg_.bounds.size_regions.y *
				cfg_.bounds.size_regions.z;
	}

	RID sdf_atlas() const { return sdf_atlas_; }
	RID mat_atlas() const { return mat_atlas_; }
	RID mip_atlas(int level) const {
		// Defensive: mips_[level] is a native array OOB for hostile levels; the
		// GDScript-reachable surface (VoxelWorld::debug_mip_atlas) clamps too.
		return level >= 0 && level < ve::kMipLevels ? mips_[level] : RID();
	}
	RID palette() const { return palette_; }
	RID brick_flags() const { return brick_flags_; }
	RID region_map() const { return region_map_; }
	RID region_tables() const { return region_tables_; }
	RID free_list() const { return free_list_; }
	RID counters() const { return counters_; }
	RID frame_counters() const { return frame_; }
	RID dispatch_args() const { return dispatch_args_; }
	RID jobs() const { return jobs_; }
	RID op_pool() const { return op_pool_; }
	RID op_counts() const { return op_counts_; }
	RID region_slot_counts() const { return region_slot_counts_; }
	RID region_occupancy() const { return region_occupancy_; }
	// Bytes per region slot, and the offset of one slot's block. Mirrors
	// ve::kOccupancyBlockBytes; the static_assert in gpu_atlas.cpp pins them together.
	static uint32_t occupancy_block_bytes() {
		return static_cast<uint32_t>(ve::kOccupancyBlockBytes);
	}
	VolumePool &volumes() { return volumes_; }
	const VolumePool &volumes() const { return volumes_; }
	OverridePool &overrides() { return overrides_; }
	const OverridePool &overrides() const { return overrides_; }
	// The render device's compact-normal store. Owned here so teardown order lives in
	// exactly one place, like every other RID in the world path.
	StoredNormalPool &stored_normals() { return stored_normals_; }
	const StoredNormalPool &stored_normals() const { return stored_normals_; }
	bool upload_override(RenderingDevice *rd, int slot, const ve::OverrideBrick &brick) {
		return overrides_.is_valid() && overrides_.upload(slot, brick);
	}
	void set_override_table(RenderingDevice *rd, int region_slot, int table, const std::vector<std::pair<int, int>> &entries);
	// Replays the CPU-authoritative override bytes and all table entries after this device's
	// resources have been recreated. Region-slot bindings are restored by WorldStreamer when
	// the regions stream back in.
	bool replay_overrides(RenderingDevice *rd, const ve::OverrideStore &store,
			const std::map<std::tuple<int, int, int>, int> &tables);

	void reset_frame_counters(RenderingDevice *rd);
	void clear_overflow(RenderingDevice *rd);
	int read_free_count(RenderingDevice *rd) const;
	int read_job_count(RenderingDevice *rd) const;
	uint32_t read_overflow(RenderingDevice *rd) const;
	// How many atlas slots each region slot currently holds, written by the mark and free
	// passes. One small readback (max_region_slots ints) per frame; it is what lets the
	// streamer fund a stream-in out of evictions that actually return bricks.
	void read_region_slot_counts(RenderingDevice *rd, std::vector<int> *out) const;

	void upload_region_ops(RenderingDevice *rd, int region_slot, const ve::EditOp *ops,
			int count);
	void set_region_map_entry(RenderingDevice *rd, int region_index, int region_slot);
	void clear_region_map(RenderingDevice *rd);

private:
	RID make_volume(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h, int d);

	RenderingDevice *rd_ = nullptr;
	GpuAtlasConfig cfg_;
	RID sdf_atlas_, mat_atlas_, palette_, brick_flags_;
	RID mips_[ve::kMipLevels];
	RID region_map_, region_tables_, free_list_, counters_, frame_, dispatch_args_;
	RID jobs_, op_pool_, op_counts_, region_slot_counts_;
	RID region_occupancy_;
	VolumePool volumes_;
	OverridePool overrides_;
	StoredNormalPool stored_normals_;
};

} // namespace godot
