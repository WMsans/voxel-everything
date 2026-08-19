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
		uint32_t flags = 0;
		int probe_mode = 0;
	};

	~DeferredPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return shader_.is_valid() && pipeline_.is_valid(); }
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID ssgi, RID sun_map, const float sun_view_proj[16], float shadow_texel,
			const Params &p);
	float last_ms() const { return last_ms_; }

private:
	bool ensure_dummies(RenderingDevice *rd);
	bool ensure_uniform_set(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID ssgi, RID sun_map);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_linear_, sampler_nearest_;
	RID dummy_black_, dummy_far_, sun_ubo_;
	RID uset_;
	RID key_albedo_, key_surface_, key_depth_, key_lit_, key_ssgi_, key_sun_;
	RID key_material_albedo_, key_material_surface_, key_material_sampler_;
	float last_ms_ = 0.0f;
};

} // namespace godot
