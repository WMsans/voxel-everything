#include "leaves/leaf_settings.h"

namespace ve {
namespace {

const SettingRow<LeafSettings> kLeafRows[] = {
	bool_row("enabled", "Leaves", &LeafSettings::enabled),
	float_row("reach_m", "Reach (m)", &LeafSettings::reach_m, 0.0f, 400.0f, 0.0f, 400.0f, 5.0f),
	// 128 is the scatter's local_size_x. Raising this needs a second workgroup per tree, not
	// a bigger number here.
	int_row("clumps_per_tree", "Clumps per tree", &LeafSettings::clumps_per_tree, 0, 128, 0, 128),
	int_row("max_clumps", "Max clumps", &LeafSettings::max_clumps, 0, 2000000, 0, 1000000, 10000),
	int_row("max_trees", "Max trees", &LeafSettings::max_trees, 0, 65536, 0, 32768, 256),
	float_row("clump_radius_m", "Clump radius (m)", &LeafSettings::clump_radius_m, 0.0f, 4.0f,
			0.0f, 2.0f, 0.05f),
	float_row("shell_min", "Shell inner (frac)", &LeafSettings::shell_min, 0.0f, 1.0f, 0.0f, 1.0f,
			0.01f),
	float_row("canopy_roundness", "Canopy roundness", &LeafSettings::canopy_roundness, 0.0f, 1.0f,
			0.0f, 1.0f, 0.01f),
	float_row("crown_shade", "Crown self-shade", &LeafSettings::crown_shade, 0.0f, 1.0f, 0.0f,
			1.0f, 0.01f),
	float_row("wind_strength", "Wind strength (m)", &LeafSettings::wind_strength, 0.0f, 3.0f, 0.0f,
			1.5f, 0.05f),
	float_row("wind_speed", "Wind speed", &LeafSettings::wind_speed, 0.0f, 8.0f, 0.0f, 3.0f, 0.1f),
	float_row("wind_scale", "Wind scale (1/m)", &LeafSettings::wind_scale, 0.0f, 4.0f, 0.0f, 0.5f,
			0.005f),
	float_row("gloss", "Gloss", &LeafSettings::gloss, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("hue_jitter", "Hue jitter", &LeafSettings::hue_jitter, 0.0f, 1.0f, 0.0f, 0.5f, 0.01f),
};

} // namespace

std::span<const SettingRow<LeafSettings>> leaf_rows() { return kLeafRows; }

void clamp_leaf_settings(LeafSettings *s) { clamp_all<LeafSettings>(kLeafRows, s); }

} // namespace ve
