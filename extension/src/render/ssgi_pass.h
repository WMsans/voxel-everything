#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
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
	bool ensure_targets(RenderingDevice *rd, Vector2i size);
	bool ensure_uniform_set(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
			RID prev_ssgi, RID out_ssgi);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_nearest_, sampler_linear_;
	RID targets_[2], uset_;
	RID key_albedo_, key_surface_, key_depth_, key_history_, key_prev_, key_out_, key_camera_;
	Vector2i size_{0, 0};
	RID output_;
	float last_ms_ = 0.0f;
};

} // namespace godot
