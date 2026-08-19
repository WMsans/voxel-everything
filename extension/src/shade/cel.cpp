#include "shade/cel.h"
#include <cmath>

namespace {

inline float clamp01(float v) {
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

} // namespace

namespace ve {

int cel_band(const CelParams &p, float ndl) {
	const float v = clamp01(ndl);
	int band = 0;
	for (int i = 0; i < kCelBands - 1; i++)
		if (v > p.band_edge[i]) band = i + 1;
	return band;
}

float cel_level(const CelParams &p, float ndl) {
	return p.band_level[cel_band(p, ndl)];
}

void rgb_to_hsv(const float rgb[3], float hsv[3]) {
	const float r = rgb[0], g = rgb[1], b = rgb[2];
	const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	const float d = mx - mn;
	float h = 0.0f;
	if (d > 0.0f) {
		if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
		else if (mx == g) h = (b - r) / d + 2.0f;
		else h = (r - g) / d + 4.0f;
		h /= 6.0f;
	}
	hsv[0] = h;
	hsv[1] = mx > 0.0f ? d / mx : 0.0f;
	hsv[2] = mx;
}

void hsv_to_rgb(const float hsv[3], float rgb[3]) {
	const float h = (hsv[0] - std::floor(hsv[0])) * 6.0f;
	const float s = clamp01(hsv[1]);
	const float v = hsv[2];
	const int i = static_cast<int>(std::floor(h)) % 6;
	const float f = h - std::floor(h);
	const float p = v * (1.0f - s);
	const float q = v * (1.0f - s * f);
	const float t = v * (1.0f - s * (1.0f - f));
	switch (i) {
		case 0: rgb[0] = v; rgb[1] = t; rgb[2] = p; break;
		case 1: rgb[0] = q; rgb[1] = v; rgb[2] = p; break;
		case 2: rgb[0] = p; rgb[1] = v; rgb[2] = t; break;
		case 3: rgb[0] = p; rgb[1] = q; rgb[2] = v; break;
		case 4: rgb[0] = t; rgb[1] = p; rgb[2] = v; break;
		default: rgb[0] = v; rgb[1] = p; rgb[2] = q; break;
	}
}

void cel_shadow_tint(const CelParams &p, const float albedo[3], float t, float out[3]) {
	const float k = clamp01(t);
	float hsv[3];
	rgb_to_hsv(albedo, hsv);
	// A grey surface has no hue to shift, and multiplying a zero saturation keeps it zero,
	// so the identity for grey falls out of the maths rather than needing a branch.
	hsv[0] = hsv[0] + p.shadow_hue_shift * k;
	hsv[0] -= std::floor(hsv[0]);
	hsv[1] = clamp01(hsv[1] * (1.0f + (p.shadow_saturation - 1.0f) * k));
	hsv_to_rgb(hsv, out);
}

void cel_shade(const CelParams &p, const CelInput &in, float out[3]) {
	const float lit = cel_level(p, in.ndl) * clamp01(in.shadow);
	float tint[3];
	cel_shadow_tint(p, in.albedo, 1.0f - lit, tint);
	// A hard step, not a falloff: the band edge IS the highlight's silhouette.
	const float spec = (in.gloss > 0.0f && in.ndh >= p.spec_edge)
			? in.gloss * p.spec_strength * clamp01(in.shadow)
			: 0.0f;
	const float rim = p.rim_strength * std::pow(1.0f - clamp01(in.ndv), p.rim_power);
	const float ao = clamp01(in.ao);
	for (int c = 0; c < 3; c++)
		out[c] = tint[c] * lit + tint[c] * in.ambient[c] * ao + spec + rim;
}

} // namespace ve
