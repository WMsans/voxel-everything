#pragma once
#include "render/gpu/gpu.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>

namespace godot {

class GBuffer;
class MaterialAtlas;

// Shades transparent fronts over the lit G-buffer (docs/superpowers/specs/2026-09-29-
// transparent-materials-design.md §7). Runs after deferred and before inject, rewriting
// gb.lit() in place for the pixels that have a front and leaving every other pixel alone.
class TransparencyCompositePass {
public:
	struct Params {
		float right[3] = {}, up[3] = {};
		float tan_x = 0.0f, tan_y = 0.0f;
		float ambient[3] = {};
		float fade_start = 0.0f, fade_end = 0.0f;
		uint32_t flags = 0; // beauty flags
	};

	~TransparencyCompositePass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return program_.valid(); }
	// The SunLight UBO is owned by RenderOrchestrator; this pass only mirrors its RID.
	void set_sun_ubo(RID buffer) { sun_light_ubo_ = buffer; }
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID near_front, RID near_trans, RID far_front, RID far_trans, bool far_drawn,
			RID sun_map, RID sun_cascade_ubo, RID beauty_cam_ubo, const Params &p);

private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_, sampler_linear_, dummy_far_;
	RID sun_light_ubo_; // NOT owned
	gpu::SetCache set_;
};

} // namespace godot
