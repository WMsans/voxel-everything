#include "shade/beauty_settings.h"

namespace {

inline int clamp_int(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

inline float clamp_float(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

namespace ve {

BeautySettings settings_for_tier(QualityTier t) {
	BeautySettings s;
	switch (t) {
		case QualityTier::kOff:
			s.ssgi = s.ssr = s.contact_shadows = s.outlines = false;
			s.sun_shadow_map = s.glossy_sdf_rays = s.raymarched_sun_shadow = false;
			s.ssao = false;
			s.ssgi_taps = 0;
			s.ssr_steps = 0;
			s.contact_steps = 0;
			break;
		case QualityTier::kLow:
			// Outlines and the raymarched sun shadow survive: they are what makes the image
			// read as this engine's image at all, and together they cost under 1 ms.
			s.ssgi = s.ssr = s.contact_shadows = false;
			s.glossy_sdf_rays = false;
			s.ssao = false;
			s.ssgi_taps = 0;
			s.ssr_steps = 0;
			s.contact_steps = 0;
			break;
		case QualityTier::kMedium:
			s.glossy_sdf_rays = false;
			s.ssgi_taps = 4;
			s.ssr_steps = 12;
			s.contact_steps = 8;
			break;
		case QualityTier::kHigh:
		default:
			break; // the struct's defaults ARE High
	}
	clamp_settings(&s);
	return s;
}

void clamp_settings(BeautySettings *s) {
	if (!s) return;
	s->ssgi_taps = clamp_int(s->ssgi_taps, 0, 16);
	s->ssr_steps = clamp_int(s->ssr_steps, 0, 64);
	s->contact_steps = clamp_int(s->contact_steps, 0, 32);
	s->outline_depth_threshold = clamp_float(s->outline_depth_threshold, 0.0f, 1.0f);
	s->outline_normal_threshold = clamp_float(s->outline_normal_threshold, 0.0f, 2.0f);
	// Zero work is off. A dispatch that produces nothing still costs a full-screen pass.
	if (s->ssgi_taps == 0) s->ssgi = false;
	if (s->ssr_steps == 0) s->ssr = false;
	if (s->contact_steps == 0) s->contact_shadows = false;
}

uint32_t pack_beauty_flags(const BeautySettings &s) {
	uint32_t f = 0;
	if (s.ssgi && s.ssgi_taps > 0) f |= kFlagSsgi;
	if (s.ssr && s.ssr_steps > 0) f |= kFlagSsr;
	if (s.contact_shadows && s.contact_steps > 0) f |= kFlagContact;
	if (s.outlines) f |= kFlagOutlines;
	if (s.sun_shadow_map) f |= kFlagSunMap;
	if (s.glossy_sdf_rays) f |= kFlagGlossyRays;
	if (s.raymarched_sun_shadow) f |= kFlagRaySunShadow;
	if (s.ssao) f |= kFlagSsao;
	if (s.cost_view) f |= kFlagCostView;
	return f;
}

} // namespace ve
