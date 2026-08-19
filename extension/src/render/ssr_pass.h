#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
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
	RID reflection() const { return reflection_; }
	Vector2i half_size() const { return half_size_; }
	float last_ms() const { return last_ms_; }

private:
	bool ensure_targets(RenderingDevice *, Vector2i);
	bool ensure_uniform_sets(RenderingDevice *, RID scene_color, RID scene_depth, RID gb_surface,
			RID gb_depth, RID normal_roughness, RID camera_ubo);

	RenderingDevice *rd_ = nullptr;
	RID trace_shader_, apply_shader_, trace_pipeline_, apply_pipeline_;
	RID nearest_, linear_, reflection_, dummy_normal_, trace_set_, apply_set_;
	RID key_color_, key_depth_, key_surface_, key_gb_depth_, key_normal_, key_camera_;
	Vector2i half_size_{0, 0};
	float last_ms_ = 0.0f;
};

} // namespace godot
