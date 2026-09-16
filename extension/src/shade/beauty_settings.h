#pragma once
#include <cstdint>
#include "settings/settings_table.h"
#include <span>

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

	// The contact-shadow march (ContactShadowPush.params.xyz). Were literals in
	// contact_shadow_pass.cpp. Metres where named so.
	float contact_reach_m = 0.6f;   // [0.05, 4]  how far the screen-space march reaches
	float contact_strength = 0.85f; // [0, 1]     how dark a fully occluded pixel gets
	float contact_bias_m = 0.05f;   // [0, 0.5]   surface bias and hit thickness

	int ssao_steps = 8;      // [0, 16]  march steps per sweep direction
	int ssao_directions = 6; // [0, 8]   sweep directions per pixel

	// SSAO's gather shape. These were file-scope constants in ssao_pass.cpp, the one place a
	// look constant hid from both the tiers and the menu. Radius is world metres.
	float ssao_radius = 5.0f;   // [0.25, 32]
	float ssao_strength = 1.5f; // [0, 8]

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
	// What an edge pixel's colour is multiplied by (OutlinePush.params.z). Was a literal in
	// outline_pass.cpp.
	float outline_darken = 0.35f; // [0, 1]
	// Sky-fill light on every surface: the deferred pass's ambient term and the ve_ambient global
	// the cel-shaded objects read, one number for both. Was the deferred pass's ambient constant. Linear RGB.
	float ambient[3] = {0.16f, 0.19f, 0.26f}; // [0, 4] per channel
};

// Bit layout; kBeautyFlags below generates BEAUTY_* for the shaders. A bit is only set when the effect is
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

// Every flag bit a shader tests, by GLSL name (BEAUTY_<name>). shaders/generated/constants.glslh
// is emitted from this table, so a new flag is one constant above and one row here.
struct BeautyFlag {
	const char *name;
	uint32_t bit;
};

inline constexpr BeautyFlag kBeautyFlags[] = {
	{"SSGI", kFlagSsgi},
	{"SSR", kFlagSsr},
	{"CONTACT", kFlagContact},
	{"OUTLINES", kFlagOutlines},
	{"SUN_MAP", kFlagSunMap},
	{"GLOSSY_RAYS", kFlagGlossyRays},
	{"RAY_SUN_SHADOW", kFlagRaySunShadow},
	{"SSAO", kFlagSsao},
	{"COST_VIEW", kFlagCostView},
};

BeautySettings settings_for_tier(QualityTier t);
// One row per knob (spec 2026-09-16 §3.3). The store, the settings panel, the config file, the
// inspector and debug_beauty_settings read these rows; clamp_settings clamps through them.
std::span<const SettingRow<BeautySettings>> beauty_rows();
// The rules that span fields: zero work is off. Runs after every clamp and every store resolve.
void normalize_beauty(BeautySettings *s);
void clamp_settings(BeautySettings *s);
uint32_t pack_beauty_flags(const BeautySettings &s);
inline uint32_t pack_flags(const BeautySettings &s) {
	return pack_beauty_flags(s);
}

} // namespace ve
