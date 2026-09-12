#include "grass/grass_settings.h"
#include <algorithm>
#include <cmath>

namespace ve {
namespace {
// NaN-safe: the comparison chain below is written so a NaN falls through to `lo`, which is
// the conservative end for every knob here.
float clampf(float v, float lo, float hi) {
	if (!(v > lo)) return lo;
	if (!(v < hi)) return hi;
	return v;
}
int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
} // namespace

void clamp_grass_settings(GrassSettings *s) {
	if (!s) return;
	s->reach_m = clampf(s->reach_m, 0.0f, 256.0f);
	s->vertical_reach_m = clampf(s->vertical_reach_m, 0.0f, 64.0f);
	s->blades_per_brick = clampi(s->blades_per_brick, 0, 64);
	s->max_blades = clampi(s->max_blades, 0, 4000000);
	s->blade_width_m = clampf(s->blade_width_m, 0.0f, 0.5f);
	s->blade_height_m = clampf(s->blade_height_m, 0.0f, 4.0f);
	s->height_jitter = clampf(s->height_jitter, 0.0f, 1.0f);
	s->slope_cos_min = clampf(s->slope_cos_min, -1.0f, 1.0f);
	s->wind_strength = clampf(s->wind_strength, 0.0f, 4.0f);
	s->wind_speed = clampf(s->wind_speed, 0.0f, 8.0f);
	s->wind_scale = clampf(s->wind_scale, 0.0f, 4.0f);
	s->flower_chance = clampf(s->flower_chance, 0.0f, 1.0f);
	s->gloss = clampf(s->gloss, 0.0f, 1.0f);
}

} // namespace ve
