#pragma once
#include "render/gpu/gpu.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>

namespace godot {

class GBuffer;
class MaterialAtlas;

// Shades transparent fronts over the lit G-buffer (docs/superpowers/specs/2026-10-01-
// transparent-voxels-design.md §6). Runs after deferred and before inject, rewriting
// gb.lit() in place for the pixels that have a front (or, with the camera inside the medium,
// a thickness) and leaving every other pixel alone.
class TransparencyCompositePass {
public:
	struct Params {
		float right[3] = {}, up[3] = {};
		float tan_x = 0.0f, tan_y = 0.0f;
		float ambient[3] = {};
		float min_transmit = 0.35f;
		float sky_thickness_m = 4.0f;
		uint32_t flags = 0;           // beauty flags
		uint32_t inside_material = 0; // material the camera sits inside; 0 = outside
	};

	~TransparencyCompositePass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return program_.valid(); }
	// The SunLight UBO is owned by RenderOrchestrator; this pass only mirrors its RID.
	void set_sun_ubo(RID buffer) { sun_light_ubo_ = buffer; }
	// `front` and `thickness` are the shell raster's targets, which are deliberately left
	// STALE on frames it drew nothing. The caller must pass RID() for either on such a frame:
	// the guard below is what keeps last frame's medium out of this frame's image.
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials, RID front,
			RID thickness, RID sun_map, RID sun_cascade_ubo, RID beauty_cam_ubo, const Params &p);

private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_, sampler_linear_, dummy_far_;
	RID sun_light_ubo_; // NOT owned
	gpu::SetCache set_;
};

} // namespace godot
