#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "shade/beauty_settings.h"

namespace godot {

class ContactShadowPass {
public:
	~ContactShadowPass();
	void set_sun_ubo(RID buffer);
	void initialize(RenderingDevice *rd);
	void teardown();
	bool render(RenderingDevice *rd, RID scene_color, RID scene_depth, Vector2i size,
			RID camera_ubo, const ve::BeautySettings &s);
	RID mask() const { return mask_; }
	float last_ms() const { return last_ms_; }

private:
	bool ensure_mask(RenderingDevice *rd, Vector2i size);
	bool ensure_uniform_set(RenderingDevice *rd, RID scene_color, RID scene_depth,
			RID camera_ubo);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_nearest_, sampler_linear_;
	RID mask_, uset_;
	RID sun_light_ubo_; // NOT owned: RenderOrchestrator frees it
	RID key_color_, key_depth_, key_camera_;
	Vector2i size_{0, 0};
	float last_ms_ = 0.0f;
};

} // namespace godot
