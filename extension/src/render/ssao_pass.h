#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "render/gpu/gpu.h"
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
	// Target resolution; half the G-buffer since the half-res chain landed.
	Vector2i size() const { return target_.size(); }

private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_;
	gpu::Target target_;
	gpu::SetCache set_;
	RID output_;
	float last_ms_ = 0.0f;
};

} // namespace godot
