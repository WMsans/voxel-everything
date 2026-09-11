#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "grass/grass_layout.h"
#include "world/region_window.h"

namespace godot {

class GpuAtlas;

// GPU-driven grass placement: stage 1 culls resident grass-surface bricks into a compacted
// list, stage 2 places blades inside them. Produces an instance buffer and the indirect
// draw args GrassRasterPass consumes. Modelled on SsaoPass for shader loading/teardown and
// on LodCullPass for the two-stage compute shape.
class GrassScatterPass {
public:
	~GrassScatterPass();
	bool initialize(RenderingDevice *rd);
	void teardown();

	// Returns false on any failure; the caller cancels the timing marker and skips grass.
	// Never aborts the frame -- grass is decorative (design doc section 8).
	// region_win is the LIVE residency-backed window (VoxelWorld::region_window()),
	// supplied caller-side: run() has no world handle and atlas.config().region_window
	// is init-centred/stale. atlas_bricks still comes from the atlas (static grid).
	bool run(RenderingDevice *rd, GpuAtlas &atlas, const ve::GrassLayout &layout,
			const ve::RegionWindow &region_win, float time_seconds);

	RID instance_buffer() const { return instances_; }
	RID draw_args_buffer() const { return draw_args_; }

	// Read back after run(); all three are what the SHIPPING pass wrote, which is the only
	// thing debug_grass_stats() is allowed to report.
	int last_brick_count() const { return last_brick_count_; }
	int last_blade_count() const { return last_blade_count_; }
	int blade_high_water() const { return blade_high_water_; }
	int capacity() const { return capacity_; }

	// Hook re-read: run()'s internal readback lands before the dispatch executes, and the
	// compositor's frame-end submit+sync makes it valid there. The debug hook has no frame,
	// so after its own submit+sync it refreshes the same counters through this entry point
	// before reporting. No logic change -- this is the private read, made callable.
	void read_back_counters(RenderingDevice *rd);

private:
	bool ensure_buffers(RenderingDevice *rd, int max_blades, int max_bricks);
	bool ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas);

	RenderingDevice *rd_ = nullptr;
	RID bricks_shader_, bricks_pipeline_;
	RID scatter_shader_, scatter_pipeline_;
	RID params_ubo_, brick_list_, counters_, dispatch_args_, instances_, draw_args_;
	// Pass-owned region-window block (binding 10): created in ensure_buffers, refreshed
	// from the caller-supplied live window on every run(), Task 6 reuses it for stage 2's set.
	RID region_ubo_;
	// Owned samplers for the atlas textures stage 1 declares but never samples (bindings
	// 7-8); mirrors RaymarchPass's sampler pair, linear for the SDF, nearest for ints.
	RID sampler_linear_, sampler_nearest_;
	RID bricks_uset_, scatter_uset_;
	RID key_params_, key_bricks_, key_counters_, key_dispatch_;
	RID key_rmap_, key_rtables_, key_bflags_, key_sdf_, key_mat_, key_palette_, key_region_;
	RID key_sparams_, key_sbricks_, key_scounters_, key_sdispatch_;
	int capacity_ = 0;
	int brick_capacity_ = 0;
	int last_brick_count_ = 0;
	int last_blade_count_ = 0;
	int blade_high_water_ = 0;
};

} // namespace godot
