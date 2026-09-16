#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "render/gpu/gpu.h"
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
	RID mask() const { return mask_.rid(); }
	float last_ms() const { return last_ms_; }

private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_, sampler_linear_;
	gpu::Target mask_;
	gpu::SetCache set_;
	RID sun_light_ubo_; // NOT owned: RenderOrchestrator frees it
	float last_ms_ = 0.0f;
};

} // namespace godot
