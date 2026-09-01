#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>

namespace godot {

class GBuffer;
class MaterialAtlas;

class DeferredPass {
public:
	static constexpr float kAmbient[3] = {0.16f, 0.19f, 0.26f};

	struct Params {
		float inv_view_proj[16] = {};
		float cam_pos[3] = {};
		float ambient[3] = {kAmbient[0], kAmbient[1], kAmbient[2]};
		// Light-space depth extent of the sun ortho, in world metres. Only read when a sun
		// map is bound; `render()` clears kFlagSunMap when it is not, so the default 0 is
		// never divided by.
		float shadow_depth_range = 0.0f;
		// The LoD hand-over band, in metres from the camera. The sun map is rasterized from
		// the LoD mesh alone, so it may only shade the pixels that mesh drew; these two
		// distances are how the shader recovers which field owns a pixel.
		float fade_start = 0.0f;
		float fade_end = 0.0f;
		uint32_t flags = 0;
		int probe_mode = 0;
	};

	~DeferredPass();
	// The sun UBO is owned by RenderOrchestrator; this pass only mirrors its RID.
	void set_sun_ubo(RID buffer);
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return shader_.is_valid() && pipeline_.is_valid(); }
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID ssgi, RID ssao, RID sun_map, const float sun_view_proj[16],
			float shadow_texel, const Params &p);
	float last_ms() const { return last_ms_; }

private:
	bool ensure_dummies(RenderingDevice *rd);
	bool ensure_uniform_set(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID ssgi, RID ssao, RID sun_map);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_linear_, sampler_nearest_;
	RID dummy_black_, dummy_far_, dummy_white_, sun_ubo_;
	RID sun_light_ubo_; // NOT owned: RenderOrchestrator frees it
	RID uset_;
	RID key_albedo_, key_surface_, key_depth_, key_lit_, key_ssgi_, key_ssao_, key_sun_;
	RID key_material_albedo_, key_material_surface_, key_material_sampler_;
	float last_ms_ = 0.0f;
};

} // namespace godot
