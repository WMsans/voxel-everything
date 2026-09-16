#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "render/gpu/gpu.h"
#include "shade/beauty_settings.h"

namespace godot {

class GBuffer;

class SsgiPass {
public:
	~SsgiPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
			const float prev_view_proj[16], bool have_history,
			const ve::BeautySettings &s, uint32_t frame);
	RID result() const { return output_; }
	void clear_result() { output_ = RID(); }
	float last_ms() const { return last_ms_; }

private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_, sampler_linear_;
	gpu::Target targets_[2];
	gpu::Target raw_; // the gather before its resolve; see resolve() in shaders/ssgi.comp.glsl
	gpu::SetCache set_;
	RID output_;
	float last_ms_ = 0.0f;
};

} // namespace godot
