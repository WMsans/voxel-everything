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
			s.ssao_steps = 0;
			s.ssao_directions = 0;
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
			s.ssao_steps = 0;
			s.ssao_directions = 0;
			break;
		case QualityTier::kMedium:
			s.glossy_sdf_rays = false;
			// Half the taps have to cover the same rings, so pull the emissive ring in
			// rather than let it get half as many samples over the same area.
			s.emissive_gi_radius = 24.0f;
			s.ssgi_taps = 4;
			s.ssr_steps = 12;
			s.contact_steps = 8;
			s.ssao_steps = 4;
			s.ssao_directions = 4;
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
	s->ssao_steps = clamp_int(s->ssao_steps, 0, 16);
	s->ssao_directions = clamp_int(s->ssao_directions, 0, 8);
	s->outline_depth_threshold = clamp_float(s->outline_depth_threshold, 0.0f, 1.0f);
	s->outline_normal_threshold = clamp_float(s->outline_normal_threshold, 0.0f, 2.0f);
	// A radius floor of 0.25 m rather than 0: a zero-radius gather still dispatches, still
	// reads the G-buffer, and returns black -- which is the expensive way to spell "off".
	// Turning the effect off is what `ssgi` is for.
	s->ssgi_radius = clamp_float(s->ssgi_radius, 0.25f, 64.0f);
	// Strictly below 1: at 1.0 the accumulator never takes the current frame and the image
	// freezes on whatever it happened to hold.
	s->ssgi_temporal = clamp_float(s->ssgi_temporal, 0.0f, 0.99f);
	s->ssgi_strength = clamp_float(s->ssgi_strength, 0.0f, 8.0f);
	s->emissive_gi_radius = clamp_float(s->emissive_gi_radius, 0.25f, 512.0f);
	s->emissive_gi_strength = clamp_float(s->emissive_gi_strength, 0.0f, 64.0f);
	// Zero work is off. A dispatch that produces nothing still costs a full-screen pass.
	if (s->ssgi_taps == 0) s->ssgi = false;
	if (s->ssr_steps == 0) s->ssr = false;
	if (s->contact_steps == 0) s->contact_shadows = false;
	if (s->ssao_steps == 0 || s->ssao_directions == 0) s->ssao = false;
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
