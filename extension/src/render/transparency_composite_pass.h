#pragma once
#include "render/gpu/gpu.h"
#include "transparency/transparency_settings.h"
#include "water/water_settings.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>

namespace godot {

class GBuffer;
class MaterialAtlas;

// Shades transparent fronts over the lit G-buffer (docs/superpowers/specs/2026-10-01-
// transparent-voxels-design.md §6). Runs after deferred and before inject, rewriting
// gb.lit() in place for the pixels that have a front (or, with the camera inside the medium,
// a thickness), and writing a liquid front's shading normal back into the front target.
class TransparencyCompositePass {
public:
	struct Params {
		float right[3] = {}, up[3] = {};
		float tan_x = 0.0f, tan_y = 0.0f;
		float ambient[3] = {};
		// The two GDD knobs' documented defaults, not repeats of them: a caller that leaves
		// one alone gets whatever TransparencySettings documents.
		float min_transmit = ve::TransparencySettings{}.min_transmit;
		float sky_thickness_m = ve::TransparencySettings{}.sky_thickness_m;
		uint32_t flags = 0;           // beauty flags
		uint32_t inside_material = 0; // material the camera sits inside; 0 = outside
		float time_seconds = 0.0f; // the clock grass wind uses: beauty frames / 60
		ve::WaterSettings water;
	};

	~TransparencyCompositePass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return program_.valid(); }
	// The SunLight UBO is owned by RenderOrchestrator; this pass only mirrors its RID.
	void set_sun_ubo(RID buffer) { sun_light_ubo_ = buffer; }
	// `front` and `thickness` are ShellRasterPass's targets. That pass already reports RID()
	// for both on a frame it drew nothing, so the guard below is the whole staleness gate for
	// every caller: there is no "did the shell draw?" argument to forget to pass.
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials, RID front,
			RID thickness, RID sun_map, RID sun_cascade_ubo, RID beauty_cam_ubo, const Params &p);

private:
	bool ensure_lit_copy(RenderingDevice *rd, Vector2i size);
	RID lit_copy_; // lit as deferred left it: refraction reads neighbours while lit is rewritten
	Vector2i lit_copy_size_{0, 0};
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_, sampler_linear_, dummy_far_;
	RID sun_light_ubo_; // NOT owned
	gpu::SetCache set_;
};

} // namespace godot
