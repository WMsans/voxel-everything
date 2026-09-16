#include "grass/grass_settings.h"

namespace ve {
namespace {

// Hard ranges are the ones clamp_grass_settings enforced before the table existed. A NaN floors
// to the low end, which is the conservative end for every knob here.
const SettingRow<GrassSettings> kGrassRows[] = {
	bool_row("enabled", "Grass", &GrassSettings::enabled),
	// Stage 1's dispatch width grows with the cube of reach, hence the hard 256 m bound.
	float_row("reach_m", "Reach (m)", &GrassSettings::reach_m, 0.0f, 256.0f, 0.0f, 120.0f, 1.0f),
	float_row("vertical_reach_m", "Vertical reach (m)", &GrassSettings::vertical_reach_m, 0.0f,
			64.0f, 0.0f, 32.0f, 0.5f),
	int_row("blades_per_brick", "Density", &GrassSettings::blades_per_brick, 0, 64, 0, 64),
	int_row("max_blades", "Max blades", &GrassSettings::max_blades, 0, 4000000, 0, 2000000, 10000),
	float_row("blade_width_m", "Blade width (m)", &GrassSettings::blade_width_m, 0.0f, 0.5f, 0.0f,
			0.2f, 0.005f),
	float_row("blade_height_m", "Blade height (m)", &GrassSettings::blade_height_m, 0.0f, 4.0f,
			0.0f, 2.0f, 0.05f),
	float_row("height_jitter", "Height jitter", &GrassSettings::height_jitter, 0.0f, 1.0f, 0.0f,
			1.0f, 0.01f),
	float_row("slope_cos_min", "Min slope (cos)", &GrassSettings::slope_cos_min, -1.0f, 1.0f, 0.0f,
			1.0f, 0.01f),
	float_row("wind_strength", "Wind strength (m)", &GrassSettings::wind_strength, 0.0f, 4.0f, 0.0f,
			1.5f, 0.05f),
	float_row("wind_speed", "Wind speed", &GrassSettings::wind_speed, 0.0f, 8.0f, 0.0f, 3.0f, 0.1f),
	float_row("wind_scale", "Wind scale (1/m)", &GrassSettings::wind_scale, 0.0f, 4.0f, 0.0f, 0.5f,
			0.005f),
	float_row("wind_dir_deg", "Wind direction (deg)", &GrassSettings::wind_dir_deg, 0.0f, 360.0f,
			0.0f, 360.0f, 1.0f),
	// Never past pi: a half-width of pi already covers every azimuth, and anything beyond it is
	// the uniform-random lean this field exists to replace.
	float_row("lean_spread_rad", "Lean spread (rad)", &GrassSettings::lean_spread_rad, 0.0f,
			3.14159265f, 0.0f, 3.14159265f, 0.01f),
	float_row("base_curve", "Blade curve", &GrassSettings::base_curve, 0.0f, 2.0f, 0.0f, 2.0f, 0.01f),
	float_row("camera_tilt", "Camera tilt", &GrassSettings::camera_tilt, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("ring_width_gain", "Far ring width gain", &GrassSettings::ring_width_gain, 0.0f, 6.0f,
			0.0f, 6.0f, 0.1f),
	float_row("flower_chance", "Flower chance", &GrassSettings::flower_chance, 0.0f, 1.0f, 0.0f,
			0.1f, 0.001f),
	float_row("gloss", "Gloss", &GrassSettings::gloss, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("blade_lighting", "Blade lighting", &GrassSettings::blade_lighting, 0.0f, 1.0f, 0.0f,
			1.0f, 0.05f),
};

} // namespace

std::span<const SettingRow<GrassSettings>> grass_rows() {
	return kGrassRows;
}

void clamp_grass_settings(GrassSettings *s) {
	clamp_all(grass_rows(), s);
}

} // namespace ve
