#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/gpu_atlas.h"

namespace godot {

class FieldContextSet;

// Indirect brick generation: one workgroup per job in GpuAtlas::jobs(), group count read
// from GpuAtlas::dispatch_args().
class BrickGenPass {
public:
	~BrickGenPass();

	bool initialize(RenderingDevice *rd, const GpuAtlas &atlas);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }
	// Task 16: FieldContextSet builds set 1 against this shader's set-1 layout.
	RID shader() const { return shader_; }

	// Records into an OPEN compute list. RegionPass::write_dispatch_args followed by
	// compute_list_add_barrier must already have been recorded. field_context is the
	// orchestrator's set 1 (may be null when its build failed); bound beside set 0.
	void dispatch(RenderingDevice *rd, int64_t list, const GpuAtlas &atlas,
			const FieldContextSet *field_context);

private:
	RenderingDevice *rd_ = nullptr;
	ve::IVec3 atlas_bricks_{};
	RID shader_, pipeline_, uset_;
};

} // namespace godot
