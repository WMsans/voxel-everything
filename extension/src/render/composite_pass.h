#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace godot {

class MaterialAtlas;

class CompositePass {
public:
	// Calls teardown(): frees RIDs on rd_ (device must be alive). Matches Task 10's
	// ~RaymarchPass() so `delete composite_pass_` in VoxelWorld::_exit_tree() releases
	// GPU resources; the brief's teardown() had no caller otherwise.
	~CompositePass();
	void initialize(RenderingDevice *rd);
	void teardown();
	// Drops the cached framebuffer. Used by the debug seam probe before it frees its
	// throwaway colour/depth/marker targets, so the pass never holds a framebuffer pointing
	// at freed textures.
	void release_targets();
	void draw(RenderingDevice *rd, RID dst_color, RID dst_depth,
			RID src_color, RID src_hitpos, const Projection &view_proj,
			const MaterialAtlas &materials, const float cam_pos[3],
			float fade_start, float fade_end, RID marker = RID());

private:
	bool ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth, RID marker);

	RenderingDevice *rd_ = nullptr;
	RID shader_, shader_marker_;
	RID pipeline_;
	RID sampler_linear_, sampler_nearest_;
	// Uniform set is cached (like RaymarchPass::uset_) and rebuilt only when the
	// bound source textures change; keys remember which RIDs the set references.
	RID uset_, uset_shader_, uset_src_color_, uset_src_hitpos_;
	RID uset_material_albedo_, uset_material_surface_, uset_material_sampler_;
	int64_t fb_format_ = 0;
	bool pipeline_marker_ = false;
	RID framebuffer_, fb_color_, fb_depth_, fb_marker_;
};

} // namespace godot
