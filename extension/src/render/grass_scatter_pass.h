#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "grass/grass_layout.h"

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
	bool run(RenderingDevice *rd, GpuAtlas &atlas, const ve::GrassLayout &layout,
			float time_seconds);

	RID instance_buffer() const { return instances_; }
	RID draw_args_buffer() const { return draw_args_; }

	// Read back after run(); all three are what the SHIPPING pass wrote, which is the only
	// thing debug_grass_stats() is allowed to report.
	int last_brick_count() const { return last_brick_count_; }
	int last_blade_count() const { return last_blade_count_; }
	int blade_high_water() const { return blade_high_water_; }
	int capacity() const { return capacity_; }

private:
	bool ensure_buffers(RenderingDevice *rd, int max_blades, int max_bricks);
	bool ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas);
	void read_back_counters(RenderingDevice *rd);

	RenderingDevice *rd_ = nullptr;
	RID bricks_shader_, bricks_pipeline_;
	RID scatter_shader_, scatter_pipeline_;
	RID params_ubo_, brick_list_, counters_, dispatch_args_, instances_, draw_args_;
	RID bricks_uset_, scatter_uset_;
	RID key_params_, key_bricks_, key_counters_, key_dispatch_;
	RID key_sparams_, key_sbricks_, key_scounters_, key_sdispatch_;
	int capacity_ = 0;
	int brick_capacity_ = 0;
	int last_brick_count_ = 0;
	int last_blade_count_ = 0;
	int blade_high_water_ = 0;
};

} // namespace godot
