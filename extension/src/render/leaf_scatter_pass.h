#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "leaves/leaf_layout.h"
#include "render/async_readback.h"
#include "render/gpu/gpu.h"
#include "gpu_layout/blocks.h"
#include "render/leaf_raster_pass.h"
#include "render/scatter_reuse.h"
#include "world/region_window.h"

namespace godot {

class GpuAtlas;
class FieldContextSet;

// GPU-driven canopy pipeline: stage 1 walks the tree lattice over the reach box and compacts
// the trees that survive distance, frustum and the live-atlas chop check into a LeafTree list;
// stage 2 (leaf_scatter.comp.glsl) is dispatched INDIRECT from that list's args and scatters
// the clump instances into instance_buffer(). Nothing draws yet -- Task 12's raster consumes
// raster_draw_args_buffer(). Modelled on GrassScatterPass pass-for-pass: same shader loading,
// gpu::Group teardown, SetCache, explicit per-frame counter clearing, readback and
// overflow-logged-once.
class LeafScatterPass {
public:
	~LeafScatterPass();
	bool initialize(RenderingDevice *rd);
	void teardown();

	// Returns false on any failure; the caller cancels the timing marker and skips leaves.
	// Never aborts the frame -- canopies are decorative (design doc section 8).
	// region_win is the LIVE residency-backed window (WorldStore::region_window()), supplied
	// caller-side: run() has no world handle and atlas.config().region_window is
	// init-centred/stale. atlas_bricks still comes from the atlas (static grid).
	// sun_ubo is the frame's SunUbo buffer: stage 2's clump scatter marches the terrain sun
	// ray once per clump; stage 1 never reads it, and an absent sun skips stage 2, not the
	// cull (the frame driver's contract). field is the terrain pipeline's set-1
	// context -- the leaf_trees shader takes trees_ground_h/trees_ground_slope from the
	// generated field source, so a missing set 1 fails the run, not the frame.
	bool run(RenderingDevice *rd, GpuAtlas &atlas, const ve::LeafLayout &layout,
			const ve::RegionWindow &region_win, float time_seconds, RID sun_ubo,
			const FieldContextSet *field, uint64_t world_epoch = ve::kAlwaysScatter);
	// Full scatters run since initialize(); reused frames do not count. world_epoch and the
	// reuse rule are GrassScatterPass::run's.
	int64_t scatter_runs() const { return scatter_runs_; }

	RID tree_list_buffer() const { return tree_list_; }
	RID instance_buffer() const { return instances_; }
	// The 3-word indirect dispatch args stage 1 grows with atomicMax on surviving trees:
	// stage 2's dispatch reads this (compute_list_dispatch_indirect, exactly as grass).
	RID draw_args_buffer() const { return dispatch_args_; }
	// The 4-word indirect DRAW args stage 2 grows with atomicMax on emitted clumps: Task 12's
	// raster reads this (six vertices per clump, instance_count 1).
	RID raster_draw_args_buffer() const { return draw_args_; }
	RID params_buffer() const { return params_ubo_; }
	LeafRasterInputs raster_inputs() const {
		return {instances_, params_ubo_, draw_args_, last_clump_count_};
	}

	// Read back after run(); what the SHIPPING pass wrote, which is the only thing
	// debug_leaf_stats() is allowed to report.
	int last_tree_count() const { return last_tree_count_; }
	int last_clump_count() const { return last_clump_count_; }
	int clump_high_water() const { return clump_high_water_; }
	int capacity() const { return capacity_; }

	// Synchronous re-read for the debug hook, after its own submit+sync. run() never calls
	// this -- see GrassScatterPass::read_back_counters: the shipping path reads the counters
	// asynchronously, so the getters above lag the GPU by the frame queue. Drains that read
	// first, so a late arrival can never overwrite what this returns.
	void read_back_counters(RenderingDevice *rd);

	// Sample readback, mirroring the grass pass's contract (read after read_back_counters):
	// sample_count() is how many instances the reduction covers (at most the first 4096),
	// and sample_max_crown_offset() reports the GPU-side containment reduction -- stage 2
	// atomicMaxes the shell reach as a fixed-point fraction of the crown radius into
	// counters.pad, so the number is what the SHIPPING pass computed, not a CPU re-derivation.
	void read_back_sample(RenderingDevice *rd);
	int sample_count() const { return sample_count_; }
	float sample_max_crown_offset() const { return sample_max_crown_offset_; }

	// The params block the last run() uploaded — the exact dispatch grid, lattice pitch and
	// reach the SHIPPING pass walked. debug_leaf_stats() reports it for the spec §9 contract
	// test so GDScript can name cells INSIDE the grid that ran; presence is still read from
	// the tree list, never re-derived here.
	const ve::LeafParams &last_params() const { return last_params_; }

private:
	void apply_counters(const PackedByteArray &data);
	bool ensure_buffers(RenderingDevice *rd, int max_clumps, int max_trees);
	bool ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program trees_;   // stage 1: the cull
	gpu::Program scatter_; // stage 2: the clump scatter
	RID params_ubo_, tree_list_, counters_, dispatch_args_, draw_args_, instances_;
	// Pass-owned region-window block (binding 9): created in ensure_buffers, refreshed from
	// the caller-supplied live window on every run().
	RID region_ubo_;
	// One EditOp (32 bytes) of zeroes: declared by field_ops.glslh at binding 12, never
	// indexed (the chop check reads the atlas, the ground reads the analytic field).
	RID field_ops_;
	// Owned samplers for the atlas textures (bindings 7-8); mirrors RaymarchPass's sampler
	// pair, linear for the SDF, nearest for ints.
	RID sampler_linear_, sampler_nearest_;
	gpu::SetCache trees_set_;
	gpu::SetCache scatter_set_;
	// The counters run() asks for, arriving a few frames later (see AsyncBufferRead).
	Ref<AsyncBufferRead> counters_read_;
	// What the instances in instances_ were scattered from; see GrassScatterPass::run().
	bool scattered_ = false;
	uint64_t scattered_epoch_ = 0;
	ve::LeafParams scattered_params_{};
	ve::GrassRegionBlock scattered_region_{};
	RID scattered_sets_[2];
	int64_t scatter_runs_ = 0;
	int capacity_ = 0;      // clump instances the buffer holds
	int tree_capacity_ = 0; // entries in tree_list_
	int last_tree_count_ = 0;
	int last_clump_count_ = 0;
	int clump_high_water_ = 0;
	int sample_count_ = 0;
	float sample_max_crown_offset_ = 0.0f;
	ve::LeafParams last_params_{};
	bool overflow_logged_ = false;
};

} // namespace godot
