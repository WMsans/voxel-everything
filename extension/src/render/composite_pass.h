#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace godot {

class CompositePass {
public:
	// Calls teardown(): frees RIDs on rd_ (device must be alive). Matches Task 10's
	// ~RaymarchPass() so `delete composite_pass_` in VoxelWorld::_exit_tree() releases
	// GPU resources; the brief's teardown() had no caller otherwise.
	~CompositePass();
	void initialize(RenderingDevice *rd);
	void teardown();
	void draw(RenderingDevice *rd, RID dst_color, RID dst_depth,
			RID src_color, RID src_hitpos, const Projection &view_proj);

private:
	bool ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth);

	RenderingDevice *rd_ = nullptr;
	RID shader_;
	RID pipeline_;
	RID sampler_linear_, sampler_nearest_;
	// Uniform set is cached (like RaymarchPass::uset_) and rebuilt only when the
	// bound source textures change; keys remember which RIDs the set references.
	RID uset_, uset_src_color_, uset_src_hitpos_;
	int64_t fb_format_ = 0;
	RID framebuffer_, fb_color_, fb_depth_;
};

} // namespace godot
