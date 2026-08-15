#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "generator/edit_ops.h"
#include "world/brick_mip.h"
#include "world/edit_log.h"
#include "world/region.h"

namespace godot {

struct GpuAtlasConfig {
	ve::IVec3 atlas_bricks{64, 32, 32};
	int max_region_slots = 512;
	int max_brick_jobs = 16384;
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
	RID region_map() const { return region_map_; }
	RID region_tables() const { return region_tables_; }
	RID free_list() const { return free_list_; }
	RID counters() const { return counters_; }
	RID frame_counters() const { return frame_; }
	RID dispatch_args() const { return dispatch_args_; }
	RID jobs() const { return jobs_; }
	RID op_pool() const { return op_pool_; }
	RID op_counts() const { return op_counts_; }

	void reset_frame_counters(RenderingDevice *rd);
	int read_free_count(RenderingDevice *rd) const;
	int read_job_count(RenderingDevice *rd) const;
	uint32_t read_overflow(RenderingDevice *rd) const;

	void upload_region_ops(RenderingDevice *rd, int region_slot, const ve::EditOp *ops,
			int count);
	void set_region_map_entry(RenderingDevice *rd, int region_index, int region_slot);
	void clear_region_map(RenderingDevice *rd);

private:
	RID make_volume(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h, int d);

	RenderingDevice *rd_ = nullptr;
	GpuAtlasConfig cfg_;
	RID sdf_atlas_, mat_atlas_, palette_;
	RID mips_[ve::kMipLevels];
	RID region_map_, region_tables_, free_list_, counters_, frame_, dispatch_args_;
	RID jobs_, op_pool_, op_counts_;
};

} // namespace godot
