#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "leaves/leaf_layout.h"
#include "render/gpu/gpu.h"
#include "world/region_window.h"

namespace godot {

class GpuAtlas;
class FieldContextSet;

// GPU-driven canopy cull: stage 1 walks the tree lattice over the reach box and compacts the
// trees that survive distance, frustum and the live-atlas chop check into a LeafTree list.
// Stage 2 (Task 11) scatters clumps into instance_buffer(); nothing draws yet. Modelled on
// GrassScatterPass pass-for-pass: same shader loading, gpu::Group teardown, SetCache,
// explicit per-frame counter clearing, readback and overflow-logged-once.
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
	// sun_ubo is the frame's SunUbo buffer: Task 11's clump scatter marches the terrain sun
	// ray once per clump; stage 1 never reads it. field is the terrain pipeline's set-1
	// context -- the leaf_trees shader takes trees_ground_h/trees_ground_slope from the
	// generated field source, so a missing set 1 fails the run, not the frame.
	bool run(RenderingDevice *rd, GpuAtlas &atlas, const ve::LeafLayout &layout,
			const ve::RegionWindow &region_win, float time_seconds, RID sun_ubo,
			const FieldContextSet *field);

	RID tree_list_buffer() const { return tree_list_; }
	RID instance_buffer() const { return instances_; }
	// The 3-word indirect dispatch args stage 1 grows with atomicMax on surviving trees:
	// Task 11 dispatches its clump scatter from this buffer.
	RID draw_args_buffer() const { return dispatch_args_; }
	RID params_buffer() const { return params_ubo_; }

	// Read back after run(); what the SHIPPING pass wrote, which is the only thing
	// debug_leaf_stats() is allowed to report.
	int last_tree_count() const { return last_tree_count_; }
	int last_clump_count() const { return last_clump_count_; }
	int clump_high_water() const { return clump_high_water_; }
	int capacity() const { return capacity_; }

	// Hook re-read: run()'s internal readback lands before the dispatch executes, and the
	// compositor's frame-end submit+sync makes it valid there. The debug hook has no frame,
	// so after its own submit+sync it refreshes the same counters through this entry point
	// before reporting. No logic change -- this is the private read, made callable.
	void read_back_counters(RenderingDevice *rd);

private:
	bool ensure_buffers(RenderingDevice *rd, int max_clumps, int max_trees);
	bool ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program trees_; // stage 1; the clump scatter (Task 11) arrives as a second Program
	RID params_ubo_, tree_list_, counters_, dispatch_args_, instances_;
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
	int capacity_ = 0;      // clump instances the buffer holds
	int tree_capacity_ = 0; // entries in tree_list_
	int last_tree_count_ = 0;
	int last_clump_count_ = 0;
	int clump_high_water_ = 0;
	bool overflow_logged_ = false;
};

} // namespace godot
