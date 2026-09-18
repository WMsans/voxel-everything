#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "grass/grass_layout.h"
#include "render/gpu/gpu.h"
#include "world/region_window.h"

namespace godot {

class GpuAtlas;
class FieldContextSet;

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
	// region_win is the LIVE residency-backed window (WorldStore::region_window()),
	// supplied caller-side: run() has no world handle and atlas.config().region_window
	// is init-centred/stale. atlas_bricks still comes from the atlas (static grid).
	// sun_ubo is the frame's SunUbo buffer: stage 2 marches the terrain sun ray once per blade.
	// field is the terrain pipeline's set-1 context; stage 1 evaluates the field directly for
	// the far LoD rings, which live past the brick atlas's residency radius. A null `field`
	// (no pipeline) keeps the near rings and skips the far ones.
	bool run(RenderingDevice *rd, GpuAtlas &atlas, const ve::GrassLayout &layout,
			const ve::RegionWindow &region_win, float time_seconds, RID sun_ubo,
			const FieldContextSet *field);

	RID instance_buffer() const { return instances_; }
	RID draw_args_buffer() const { return draw_args_; }
	// The GrassParams UBO run() refreshes every dispatch. GrassRasterPass binds the same
	// RID at its set-0 binding 1 so the raster reads the blade width and gloss the
	// scatter ran with; read-only, the stage-1/2 uniform sets are untouched.
	RID params_buffer() const { return params_ubo_; }

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

	// Placement-contract sample: reduces at most the first 4096 instances on the CPU to
	// the minimum ground-normal Y, the maximum blade height and the min/max/mean terrain sun
	// visibility the scatter marched, which the hook reports beside the counters. Call after
	// submit+sync, like read_back_counters.
	void read_back_sample(RenderingDevice *rd);
	int sample_count() const { return sample_count_; }
	float sample_min_normal_y() const { return sample_min_normal_y_; }
	float sample_max_height() const { return sample_max_height_; }
	float sample_min_sun() const { return sample_min_sun_; }
	float sample_max_sun() const { return sample_max_sun_; }
	float sample_mean_sun() const { return sample_mean_sun_; }

private:
	bool ensure_buffers(RenderingDevice *rd, int max_blades, int max_bricks);
	bool ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program bricks_;
	gpu::Program scatter_; // invalid when grass_scatter.comp.glsl failed: cull-only
	RID params_ubo_, brick_list_, counters_, dispatch_args_, instances_, draw_args_;
	// The field op pool stage 1's `field.glslh` declares. The far rings evaluate the field
	// with an op count of ZERO -- edits belong to the near field, which reads the brick atlas
	// -- so the buffer is never indexed and one empty op's worth of bytes is the whole need.
	RID field_ops_;
	// Pass-owned region-window block (binding 10): created in ensure_buffers, refreshed
	// from the caller-supplied live window on every run(), Task 6 reuses it for stage 2's set.
	RID region_ubo_;
	// Owned samplers for the atlas textures stage 1 declares but never samples (bindings
	// 7-8); mirrors RaymarchPass's sampler pair, linear for the SDF, nearest for ints.
	RID sampler_linear_, sampler_nearest_;
	gpu::SetCache bricks_set_, scatter_set_;
	int capacity_ = 0;
	int brick_capacity_ = 0;
	int last_brick_count_ = 0;
	int last_blade_count_ = 0;
	int blade_high_water_ = 0;
	bool overflow_logged_ = false;
	int sample_count_ = 0;
	float sample_min_normal_y_ = 1.0f;
	float sample_max_height_ = 0.0f;
	float sample_min_sun_ = 1.0f;
	float sample_max_sun_ = 0.0f;
	float sample_mean_sun_ = 0.0f;
};

} // namespace godot
