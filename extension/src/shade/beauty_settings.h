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
	int ssao_steps = 8;      // [0, 16]  march steps per sweep direction
	int ssao_directions = 6; // [0, 8]   sweep directions per pixel

	// SSGI's gather shape. These lived as literals in SsgiPass::render, which broke this
	// struct's own contract that no pass reads a knob that is not here -- and made the one
	// effect that carries emissive light around the scene the only effect a tier could not
	// move. Radii are world metres.
	float ssgi_radius = 6.0f;    // [0.25, 64]  how far a bounce tap may reach
	float ssgi_temporal = 0.9f;  // [0, 0.99]   history weight; higher is smoother and later
	float ssgi_strength = 1.0f;  // [0, 8]      multiplier on the gathered bounce

	// Emissive light transport, gathered on its own ring so lava can spill much further than
	// a diffuse bounce without dragging the bounce radius (and its noise) out with it. Costs
	// a second tap loop in shaders/ssgi.comp.glsl; ssgi_taps sizes both.
	float emissive_gi_radius = 16.0f;  // [0.25, 512]
	float emissive_gi_strength = 6.0f; // [0, 64]

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
