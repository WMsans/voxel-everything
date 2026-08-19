#pragma once
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
	bool last_draw_ok() const { return last_draw_ok_; }
	void draw(RenderingDevice *rd, GBuffer &gb, RID src_albedo, RID src_surface, RID src_hitpos,
			const Projection &view_proj, const MaterialAtlas &materials, const float cam_pos[3],
			float fade_start, float fade_end, RID marker = RID());

private:
	bool ensure_pipeline(RenderingDevice *rd, RID albedo, RID surface, RID depth, RID marker);

	RenderingDevice *rd_ = nullptr;
	RID shader_, shader_marker_;
	RID pipeline_;
	RID sampler_linear_, sampler_nearest_;
	RID uset_, uset_shader_, uset_src_albedo_, uset_src_surface_, uset_src_hitpos_;
	RID uset_material_albedo_, uset_material_surface_, uset_material_sampler_;
	int64_t fb_format_ = 0;
	bool pipeline_marker_ = false;
	RID framebuffer_, fb_albedo_, fb_surface_, fb_depth_, fb_marker_;
	bool last_draw_ok_ = false;
};

} // namespace godot
