#pragma once
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>

namespace godot {

class RaymarchCompositor : public CompositorEffect {
	GDCLASS(RaymarchCompositor, CompositorEffect)

	NodePath world_path_;

protected:
	static void _bind_methods();

public:
	RaymarchCompositor();
	void set_world_path(const NodePath &p) { world_path_ = p; }
	NodePath get_world_path() const { return world_path_; }
	void _render_callback(int p_effect_callback_type, RenderData *p_render_data) override;
};

} // namespace godot
