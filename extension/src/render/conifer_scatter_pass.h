#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "conifers/conifer_layout.h"
#include "gpu_layout/blocks.h"
#include "render/async_readback.h"
#include "render/gpu/gpu.h"
#include "render/leaf_raster_pass.h"
#include "world/region_window.h"

namespace godot {

class GpuAtlas;
class FieldContextSet;

// The conifer cull (conifer_cull.comp.glsl) and card scatter (conifer_scatter.comp.glsl),
// modelled on LeafScatterPass: same shader loading, gpu::Group teardown, SetCache, explicit
// counter clearing and async counter readback. It always re-scatters (plan deviation 13).
class ConiferScatterPass {
public:
	~ConiferScatterPass();
	bool initialize(RenderingDevice *rd);
	void teardown();

	// False on any failure; the caller cancels the marker and skips conifers, never the frame.
	// field is the terrain pipeline's set 1 (the cull calls conifers_params()/fjord_ground).
	bool run(RenderingDevice *rd, GpuAtlas &atlas, const ve::ConiferLayout &layout,
			const ve::RegionWindow &region_win, float time_seconds, RID sun_ubo,
			const FieldContextSet *field, const float view_proj[16], RID hiz, int hiz_size,
			int hiz_mips);

	RID card_list_buffer() const { return card_list_; }
	RID impostor_list_buffer() const { return impostor_list_; }
	RID params_buffer() const { return params_ubo_; }
	RID raster_params_buffer() const { return raster_ubo_; }
	RID impostor_draw_args_buffer() const { return impostor_args_; }
	LeafRasterInputs raster_inputs() const { return {instances_, raster_ubo_, draw_args_, last_clumps_}; }

	// Synchronous re-read for the debug hook after its own submit+sync.
	void read_back_counters(RenderingDevice *rd);
	int last_card_trees() const { return last_card_trees_; }
	int last_impostors() const { return last_impostors_; }
	int last_clumps() const { return last_clumps_; }
	int clump_high_water() const { return clump_high_water_; }

private:
	void apply_counters(const PackedByteArray &data);
	bool ensure_buffers(RenderingDevice *rd, const ve::ConiferPassParams &p);
	bool ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo, RID hiz);
	void clear_args(RenderingDevice *rd);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program cull_, scatter_;
	RID params_ubo_, raster_ubo_, region_ubo_, field_ops_;
	RID card_list_, impostor_list_, counters_, dispatch_args_, draw_args_, impostor_args_, instances_;
	RID sampler_linear_, sampler_nearest_;
	RID hiz_dummy_; // bound when there is no pyramid; the push flag keeps it unread
	gpu::SetCache cull_set_, scatter_set_;
	Ref<AsyncBufferRead> counters_read_;
	int max_clumps_ = 0, max_card_trees_ = 0, max_impostors_ = 0;
	int last_card_trees_ = 0, last_impostors_ = 0, last_clumps_ = 0, clump_high_water_ = 0;
	bool overflow_logged_ = false;
};

} // namespace godot
