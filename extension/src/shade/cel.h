#pragma once

namespace ve {

inline constexpr int kCelBands = 4;

// normalize(0.6, 0.8, 0.3) -- the direction common.glslh's shade_terrain() has used since
// M1, written out so the CPU, the shaders and the shadow map cannot disagree about where
// the sun is. shaders/shade.glslh mirrors it as SUN_DIR.
inline constexpr float kSunDir[3] = {0.5746958f, 0.7662610f, 0.2873479f};

// The paintable ramp of spec section 7. Every field is a knob a artist-facing debug menu
// could move; none of them is read anywhere except through cel_shade().
struct CelParams {
	float band_edge[kCelBands - 1] = {0.08f, 0.32f, 0.66f};
	float band_level[kCelBands] = {0.18f, 0.45f, 0.75f, 1.00f};
	// Turns of hue, applied in proportion to how deep into shadow the pixel is.
	float shadow_hue_shift = 0.055f;
	float shadow_saturation = 1.35f;
	float spec_edge = 0.72f;
	float spec_strength = 0.45f;
	float rim_strength = 0.35f;
	float rim_power = 3.0f;
};

// Everything the ramp needs, precomputed by the caller. Scalars rather than vectors so the
// GLSL mirror computes the same numbers from the same inputs instead of re-deriving them
// from its own view/light vectors -- that re-derivation is exactly how two shading paths
// drift apart.
struct CelInput {
	float albedo[3] = {1, 1, 1};
	float ambient[3] = {0, 0, 0};
	float ndl = 1.0f;    // dot(normal, sun), unclamped
	float ndv = 1.0f;    // dot(normal, view), clamped by cel_shade
	float ndh = 0.0f;    // dot(normal, halfway) for the specular band
	float shadow = 1.0f; // 1 = fully lit
	float ao = 1.0f;
	float gloss = 0.0f;
	float sun[3] = {1, 1, 1}; // linear sun colour * energy; white reproduces the old output exactly
};

int cel_band(const CelParams &p, float ndl);
float cel_level(const CelParams &p, float ndl);

void rgb_to_hsv(const float rgb[3], float hsv[3]);
void hsv_to_rgb(const float hsv[3], float rgb[3]);

// `t` is how far into shadow the pixel is: 0 = fully lit (identity), 1 = darkest band.
void cel_shadow_tint(const CelParams &p, const float albedo[3], float t, float out[3]);

void cel_shade(const CelParams &p, const CelInput &in, float out[3]);

} // namespace ve
