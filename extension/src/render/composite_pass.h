#pragma once
#include "render/camera_params.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace godot {

class GBuffer;
class MaterialAtlas;

class CompositePass {
public:
	~CompositePass();
	void initialize(RenderingDevice *rd);
	void teardown();
	void release_targets();
	// RaymarchPass may replace its output textures before this pass sees the new RIDs. Drop
	// the dependent set first so a texture replacement cannot leave a stale RID to free.
	void invalidate_uniform_set(RenderingDevice *);
	bool last_draw_ok() const { return last_draw_ok_; }
	// `cam` is the SAME block the raymarch pass was dispatched with. This pass resolves the
	// material per full-resolution pixel, so it must rebuild each pixel's primary ray -- which
	// needs the basis and the half-angle tangents, not just the position. Passing the marcher's
	// own block rather than a copy of the numbers is what keeps the two from disagreeing about
	// where a pixel's ray points.
	void draw(RenderingDevice *rd, GBuffer &gb, RID src_overlay, RID src_surface, RID src_hitpos,
			const Projection &view_proj, const MaterialAtlas &materials, const ve::CameraParams &cam,
			float fade_start, float fade_end, RID marker = RID());

private:
	bool ensure_pipeline(RenderingDevice *rd, RID albedo, RID surface, RID depth, RID marker);

	RenderingDevice *rd_ = nullptr;
	RID shader_, shader_marker_;
	RID pipeline_;
	RID sampler_linear_, sampler_nearest_;
	RID uset_, uset_shader_, uset_src_overlay_, uset_src_surface_, uset_src_hitpos_;
	RID uset_material_albedo_, uset_material_surface_, uset_material_sampler_;
	int64_t fb_format_ = 0;
	bool pipeline_marker_ = false;
	RID framebuffer_, fb_albedo_, fb_surface_, fb_depth_, fb_marker_;
	bool last_draw_ok_ = false;
};

} // namespace godot
