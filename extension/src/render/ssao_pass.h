#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "shade/beauty_settings.h"

namespace godot {

class GBuffer;

// Full-resolution HBAO over the G-buffer. Single target, no history, no ping-pong:
// the pass is stateless by design (see ssao.comp.glsl).
class SsaoPass {
public:
	~SsaoPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
			const ve::BeautySettings &s);
	RID result() const { return output_; }
	void clear_result() { output_ = RID(); }
	float last_ms() const { return last_ms_; }

private:
	bool ensure_target(RenderingDevice *rd, Vector2i size);
	bool ensure_uniform_set(RenderingDevice *rd, GBuffer &gb, RID camera_ubo);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_nearest_;
	RID target_, uset_;
	RID key_surface_, key_depth_, key_out_, key_camera_;
	Vector2i size_{0, 0};
	RID output_;
	float last_ms_ = 0.0f;
};

} // namespace godot
