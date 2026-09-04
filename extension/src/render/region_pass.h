#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/gpu_atlas.h"
#include "world/region.h"

namespace godot {

class FieldContextSet;

// The residency machinery: activation probe, atlas-slot allocation and release, and the
// indirect-dispatch argument write that hands the resulting job list to the generator.
class RegionPass {
public:
	~RegionPass();

	bool initialize(RenderingDevice *rd, const GpuAtlas &atlas);
	void teardown();
	bool is_valid() const { return mark_pipeline_.is_valid(); }

	// Records into an OPEN compute list. lo/hi are inclusive GLOBAL brick coordinates and
	// must lie inside `region`; force_regen re-enqueues bricks that are already resident.
	// generate_probe_misses is reserved for edit ranges whose exact lattice must supersede
	// the coarse activation probe. field_context is the orchestrator's set 1 (may be null
	// when its build failed); bound beside the mark pipeline's set 0.
	void mark(RenderingDevice *rd, int64_t list, ve::IVec3 region, int region_slot,
			ve::IVec3 lo, ve::IVec3 hi, int op_count, bool force_regen,
			bool generate_probe_misses, const FieldContextSet *field_context);
	void release_region(RenderingDevice *rd, int64_t list, int region_slot);
	void write_dispatch_args(RenderingDevice *rd, int64_t list);

private:
	bool build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline);

	RenderingDevice *rd_ = nullptr;
	int max_brick_jobs_ = 0;
	RID mark_shader_, mark_pipeline_, mark_uset_;
	RID free_shader_, free_pipeline_, free_uset_;
	RID args_shader_, args_pipeline_, args_uset_;
};

} // namespace godot
