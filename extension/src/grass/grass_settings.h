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

	// Candidate blades per brick in the nearest ring. Capped at 64, the scatter's workgroup
	// width -- one thread per candidate, so a brick never needs a second group.
	int blades_per_brick = 48;
	// Hard cap on the instance buffer. The scatter clamps to it rather than overflowing.
	int max_blades = 600000;

	// 0.045, not the 0.030 this used to be. grass.vert.glsl used to widen a blade by up to
	// 2.5x as it turned edge-on; billboarding the width axis retired that term, and since
	// projected width is width * sin(view angle), the factor it was really contributing
	// peaked around 1.55x and averaged near 1.5x across the field. Holding 0.030 after the
	// deletion measurably thinned the canopy, so the compensation is carried here -- as a
	// tunable blade width -- rather than as an invisible constant in the shader.
	float blade_width_m = 0.08f;
	float blade_height_m = 0.95f;
	// Per-blade height jitter as a fraction of blade_height_m.
	float height_jitter = 0.35f;

	// Grass refuses any surface whose normal is flatter than this against +Y, which is what
	// keeps blades off cliff faces.
	float slope_cos_min = 0.55f;

	float wind_strength = 0.35f; // metres of tip displacement at full gust
	float wind_speed = 0.6f;     // gust field scroll rate
	float wind_scale = 0.04f;    // gust field frequency, cycles per metre

	// Compass direction the field lies in. Blades lean around THIS angle rather than around
	// a uniform-random azimuth: coherent direction is what makes a meadow read as a meadow
	// instead of a pincushion, and it was the single worst thing about the old look.
	float wind_dir_deg = 35.0f;
	// Half-width of the per-blade lean scatter about wind_dir_deg, in radians. 0 is a lawn
	// of clones; pi is the old uniform-random azimuth under a new name.
	float lean_spread_rad = 0.55f;
	// How far the tip travels horizontally, as a fraction of blade height. This is what
	// arcs the blade over instead of standing it up like a spike.
	float base_curve = 0.45f;
	// How far a blade leans away from the viewer as the camera pitches down, as a FRACTION
	// of the camera's elevation above the blade. A blade is a card whose width axis is
	// billboarded, so its projected area goes as cos(elevation): full at eye level, half at
	// 60 degrees, a sub-pixel sliver straight overhead, where the gaps between blades open
	// into bare ground. Leaning the growth axis away from the camera by the camera's own
	// elevation turns the card's normal from horizontal up to point straight at the camera,
	// and the projected area becomes cos((1 - tilt) * elevation) -- flat in elevation, not
	// falling with it. 0 is the untilted blade; 1 is the exact fit, a full billboard when
	// the camera is overhead. The default holds .95 of the level-view area at the zenith
	// while the blade still visibly grows rather than lying flat like a lawn of leaves.
	float camera_tilt = 0.85f;
	// Far rings halve their blade count; they buy the coverage back with width, scaled
	// exp2(ring_fraction * this) so the gain is continuous and rings do not band.
	float ring_width_gain = 3.0f;

	// Fraction of blades that get a flower tint at the tip.
	float flower_chance = 0.012f;

	float gloss = 0.25f;

	// How much of a blade's own rounded normal survives against the ground normal it grows
	// from, near the camera; grass.vert.glsl fades it to zero by the reach. 0 is the flat
	// meadow every blade of which lands in the same cel band and reads as unlit paint; 1 is
	// fully per-blade lighting, which starts to read as noise.
	float blade_lighting = 0.6f;
};

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_grass_settings(GrassSettings *s);

} // namespace ve
