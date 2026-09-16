#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "render/gpu/gpu.h"
#include "shade/beauty_settings.h"

namespace godot {

class SsrPass {
public:
	static constexpr float kReachM = 40.0f;
	static constexpr float kStartBiasM = 0.08f;
	static constexpr float kThicknessM = 1.5f;
	static constexpr float kStrength = 0.80f;
	~SsrPass();
	bool initialize(RenderingDevice *);
	void teardown();
	bool render(RenderingDevice *, RID scene_color, RID scene_depth, RID gb_surface,
			RID gb_depth, RID normal_roughness, bool have_normal_roughness, RID camera_ubo,
			Vector2i size, const ve::BeautySettings &);
	RID reflection() const { return reflection_.rid(); }
	Vector2i half_size() const { return reflection_.size(); }
	float last_ms() const { return last_ms_; }

private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program trace_, apply_;
	RID nearest_, linear_, dummy_normal_;
	gpu::Target reflection_;
	gpu::SetCache trace_set_, apply_set_;
	float last_ms_ = 0.0f;
};

} // namespace godot
