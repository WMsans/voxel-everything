#pragma once

namespace ve {

// Stylized grass knobs. DELIBERATELY not part of BeautySettings: grass is its own module
// with its own store, and the beauty stack neither reads these nor needs to know they
// exist (design doc section 3). Every field is clamped by clamp_grass_settings().
struct GrassSettings {
	bool enabled = true;

	// How far blades are placed, in metres. Stage 1's dispatch width grows with the CUBE
	// of this, so it is hard-bounded at 256 m -- past roughly 64 m the flat box wants to
	// become a hierarchy instead (design doc section 11).
	float reach_m = 40.0f;
	// Vertical half-extent of the brick search box, in metres. Grass grows on the ground,
	// so the box is much shorter than it is wide.
	float vertical_reach_m = 10.0f;

	// Candidate blades per brick in the nearest ring. Rings past the first drop 3 of every
	// 4 (Ghost of Tsushima's thinning), so this is the only density number to turn.
	int blades_per_brick = 16;
	// Hard cap on the instance buffer. The scatter clamps to it rather than overflowing.
	int max_blades = 400000;

	float blade_width_m = 0.018f;
	float blade_height_m = 0.55f;
	// Per-blade height jitter as a fraction of blade_height_m.
	float height_jitter = 0.35f;

	// Grass refuses any surface whose normal is flatter than this against +Y, which is what
	// keeps blades off cliff faces.
	float slope_cos_min = 0.55f;

	float wind_strength = 0.35f; // metres of tip displacement at full gust
	float wind_speed = 0.6f;     // gust field scroll rate
	float wind_scale = 0.04f;    // gust field frequency, cycles per metre

	// Fraction of blades that get a flower tint at the tip.
	float flower_chance = 0.012f;

	float gloss = 0.25f;
};

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_grass_settings(GrassSettings *s);

} // namespace ve
