#pragma once
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>

namespace godot {

class BeautyCompositor : public CompositorEffect {
	GDCLASS(BeautyCompositor, CompositorEffect)

	NodePath world_path_;
	int normal_roughness_state_ = -1;

protected:
	static void _bind_methods();

public:
	BeautyCompositor();
	void set_world_path(const NodePath &p) { world_path_ = p; }
	NodePath get_world_path() const { return world_path_; }
	int normal_roughness_state() const { return normal_roughness_state_; }
	void _render_callback(int p_effect_callback_type, RenderData *p_render_data) override;
};

} // namespace godot
