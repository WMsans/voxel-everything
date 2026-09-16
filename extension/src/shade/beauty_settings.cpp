#include "shade/beauty_settings.h"

namespace ve {

namespace {

const SettingRow<BeautySettings> kBeautyRows[] = {
	bool_row("ssgi", "SSGI", &BeautySettings::ssgi),
	bool_row("ssr", "SSR", &BeautySettings::ssr),
	bool_row("contact_shadows", "Contact shadows", &BeautySettings::contact_shadows),
	bool_row("outlines", "Outlines", &BeautySettings::outlines),
	bool_row("sun_shadow_map", "Sun shadow map", &BeautySettings::sun_shadow_map),
	bool_row("glossy_sdf_rays", "Glossy SDF rays", &BeautySettings::glossy_sdf_rays),
	bool_row("raymarched_sun_shadow", "Raymarched sun shadow", &BeautySettings::raymarched_sun_shadow),
	bool_row("ssao", "SSAO", &BeautySettings::ssao),
	bool_row("cost_view", "Cost view", &BeautySettings::cost_view,
			"Replaces albedo with raymarch cost. A debug view; no tier sets it."),
	int_row("ssgi_taps", "SSGI taps", &BeautySettings::ssgi_taps, 0, 16, 0, 16),
	int_row("ssr_steps", "SSR steps", &BeautySettings::ssr_steps, 0, 64, 0, 64),
	int_row("contact_steps", "Contact shadow steps", &BeautySettings::contact_steps, 0, 32, 0, 32),
	int_row("ssao_steps", "SSAO steps", &BeautySettings::ssao_steps, 0, 16, 0, 16),
	int_row("ssao_directions", "SSAO directions", &BeautySettings::ssao_directions, 0, 8, 0, 8),
	// A radius floor of 0.25 m rather than 0: a zero-radius gather still dispatches, still reads
	// the G-buffer, and returns black -- the expensive way to spell "off". `ssgi` is the switch.
	float_row("ssgi_radius", "GI reach (m)", &BeautySettings::ssgi_radius, 0.25f, 64.0f, 0.25f,
			64.0f, 0.25f),
	// Strictly below 1: at 1.0 the accumulator never takes the current frame and the image
	// freezes on whatever it happened to hold.
	float_row("ssgi_temporal", "GI history weight", &BeautySettings::ssgi_temporal, 0.0f, 0.99f,
			0.0f, 0.99f, 0.01f),
	float_row("ssgi_strength", "GI bounce", &BeautySettings::ssgi_strength, 0.0f, 8.0f, 0.0f, 8.0f,
			0.05f),
	float_row("emissive_gi_radius", "Emissive reach (m)", &BeautySettings::emissive_gi_radius, 0.25f,
			512.0f, 0.25f, 128.0f, 0.25f),
	float_row("emissive_gi_strength", "Emissive light", &BeautySettings::emissive_gi_strength, 0.0f,
			64.0f, 0.0f, 64.0f, 0.5f),
	float_row("outline_depth_threshold", "Outline depth threshold",
			&BeautySettings::outline_depth_threshold, 0.0f, 1.0f, 0.0f, 0.2f, 0.005f),
	float_row("outline_normal_threshold", "Outline normal threshold",
			&BeautySettings::outline_normal_threshold, 0.0f, 2.0f, 0.0f, 1.0f, 0.01f),
};

} // namespace

std::span<const SettingRow<BeautySettings>> beauty_rows() {
	return kBeautyRows;
}

void normalize_beauty(BeautySettings *s) {
	if (!s) return;
	// Zero work is off. A dispatch that produces nothing still costs a full-screen pass.
	if (s->ssgi_taps == 0) s->ssgi = false;
	if (s->ssr_steps == 0) s->ssr = false;
	if (s->contact_steps == 0) s->contact_shadows = false;
	if (s->ssao_steps == 0 || s->ssao_directions == 0) s->ssao = false;
}

void clamp_settings(BeautySettings *s) {
	clamp_all(beauty_rows(), s);
	normalize_beauty(s);
}

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
