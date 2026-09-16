#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>
#include "render/gpu/gpu.h"
#include "shade/sun_cascades.h"
#include "shade/beauty_settings.h"

namespace godot {

class GBuffer;
class MaterialAtlas;

class DeferredPass {
public:
	struct Params {
		float inv_view_proj[16] = {};
		float cam_pos[3] = {};
		// The frame and the probes fill this from BeautySettings::ambient; a Params nobody fills
		// carries the shipped default.
		float ambient[3] = {ve::BeautySettings{}.ambient[0], ve::BeautySettings{}.ambient[1],
				ve::BeautySettings{}.ambient[2]};
		// Per-cascade sun state. `cascade_count` is what ve::sun_cascades() returned: 1 when
		// the stream radius collapsed the set, which is exactly the pre-cascade behaviour.
		float sun_view_proj[ve::kSunCascades][16] = {};
		float shadow_texel[ve::kSunCascades] = {};
		// Light-space depth extent of each cascade's ortho, in world metres. Only read when
		// a sun map is bound; render() clears kFlagSunMap when it is not, so the default 0
		// is never divided by.
		float shadow_depth_range_c[ve::kSunCascades] = {};
		float cascade_split[ve::kSunCascades] = {};
		int cascade_count = 0;
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
	bool is_valid() const { return program_.valid(); }
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID ssgi, RID ssao, RID sun_map, const Params &p);
	float last_ms() const { return last_ms_; }

private:
	bool ensure_dummies(RenderingDevice *rd);
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_linear_, sampler_nearest_;
	RID dummy_black_, dummy_far_, dummy_white_, sun_ubo_;
	RID sun_light_ubo_; // NOT owned: RenderOrchestrator frees it
	gpu::SetCache set_;
	float last_ms_ = 0.0f;
};

} // namespace godot
