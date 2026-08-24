#pragma once
#include <cstdint>

namespace ve {

enum class QualityTier { kOff = 0, kLow = 1, kMedium = 2, kHigh = 3 };

// Spec section 7: "Every effect has a quality/off toggle in the debug menu". This is that
// set. No render pass reads a knob that is not here, and no knob is here that no pass reads.
struct BeautySettings {
	bool ssgi = true;
	bool ssr = true;
	bool contact_shadows = false;
	bool outlines = true;
	bool sun_shadow_map = true;
	bool glossy_sdf_rays = true;
	bool raymarched_sun_shadow = true;
	bool cost_view = false;
	bool ssao = true;

	int ssgi_taps = 8;      // [0, 16]
	int ssr_steps = 24;     // [0, 64]
	int contact_steps = 12; // [0, 32]

	float outline_depth_threshold = 0.04f;  // [0, 1], relative to linear depth
	float outline_normal_threshold = 0.25f; // [0, 2], 1 - dot(n0, n1)
};

// Bit layout, mirrored by BEAUTY_* in the shaders. A bit is only set when the effect is
// enabled AND has work to do, so a shader never has to check both.
inline constexpr uint32_t kFlagSsgi = 1u;
inline constexpr uint32_t kFlagSsr = 2u;
inline constexpr uint32_t kFlagContact = 4u;
inline constexpr uint32_t kFlagOutlines = 8u;
inline constexpr uint32_t kFlagSunMap = 16u;
inline constexpr uint32_t kFlagGlossyRays = 32u;
inline constexpr uint32_t kFlagRaySunShadow = 64u;
inline constexpr uint32_t kFlagSsao = 256u;
// A debug view, not an effect: it replaces the albedo channel with marching cost so the
// budget conversation can be about pixels instead of averages. It is never set by a tier.
inline constexpr uint32_t kFlagCostView = 128u;

BeautySettings settings_for_tier(QualityTier t);
void clamp_settings(BeautySettings *s);
uint32_t pack_beauty_flags(const BeautySettings &s);
inline uint32_t pack_flags(const BeautySettings &s) {
	return pack_beauty_flags(s);
}

} // namespace ve
