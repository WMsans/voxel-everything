#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "render/gpu/gpu.h"
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
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID nearest_, dummy_normal_;
	gpu::SetCache set_;
	float last_ms_ = 0.0f;
};

} // namespace godot
