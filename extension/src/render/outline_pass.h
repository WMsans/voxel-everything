#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "shade/beauty_settings.h"

namespace godot {

class OutlinePass {
public:
	~OutlinePass();
	bool initialize(RenderingDevice *);
	void teardown();
	bool render(RenderingDevice *, RID scene_color, RID scene_depth, RID gb_depth,
			RID gb_surface, RID normal_roughness, bool have_normal_roughness, RID camera_ubo,
			Vector2i size, const ve::BeautySettings &);
	float last_ms() const { return last_ms_; }

private:
	bool ensure_uniform_set(RenderingDevice *, RID scene_color, RID scene_depth, RID gb_depth,
			RID gb_surface, RID normal_roughness, RID camera_ubo);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, nearest_, dummy_normal_, uset_;
	RID key_color_, key_depth_, key_gb_depth_, key_surface_, key_normal_, key_camera_;
	float last_ms_ = 0.0f;
};

} // namespace godot
