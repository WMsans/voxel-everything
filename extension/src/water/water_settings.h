#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Liquid knobs (docs/superpowers/specs/2026-10-06-water-voxels-design.md §8). DELIBERATELY
// its own module and store, like transparency, grass and leaves: nothing here joins
// BeautySettings or TransparencySettings. What a liquid lets through is a material-table
// column; these are the global look knobs while water is the only liquid. All are read every
// frame.
struct WaterSettings {
	float wave_strength = 1.0f;       // tilt multiplier on the wave normal; 0 = mirror-flat
	float flow_speed = 0.6f;          // m/s down sloped and vertical faces; the top drift is fixed
	float refraction_strength = 0.03f; // screen-uv offset at full tilt; 0 = no refraction
	float foam_width_m = 0.3f;        // water depth where shore foam fades out; 0 = no foam
};

std::span<const SettingRow<WaterSettings>> water_rows();

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_water_settings(WaterSettings *s);

} // namespace ve
