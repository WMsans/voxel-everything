#include "conifers/conifer_settings.h"

namespace ve {
namespace {

const SettingRow<ConiferSettings> kConiferRows[] = {
	bool_row("enabled", "Conifers", &ConiferSettings::enabled),
	float_row("card_reach_m", "Card reach (m)", &ConiferSettings::card_reach_m, 0.0f, 1000.0f, 0.0f, 800.0f, 10.0f),
	float_row("impostor_reach_m", "Imposter reach (m)", &ConiferSettings::impostor_reach_m, 0.0f, 4000.0f, 0.0f, 4000.0f, 50.0f),
	int_row("clumps_per_tree", "Clumps per tree", &ConiferSettings::clumps_per_tree, 0, 128, 0, 128),
	int_row("max_card_trees", "Max card trees", &ConiferSettings::max_card_trees, 0, 65536, 0, 16384, 256),
	int_row("max_clumps", "Max clumps", &ConiferSettings::max_clumps, 0, 2000000, 0, 1000000, 10000),
	int_row("max_impostors", "Max imposters", &ConiferSettings::max_impostors, 0, 1000000, 0, 400000, 10000),
	float_row("clump_radius_m", "Clump radius (m)", &ConiferSettings::clump_radius_m, 0.0f, 4.0f, 0.0f, 2.0f, 0.05f),
	float_row("shell_min", "Shell inner (frac)", &ConiferSettings::shell_min, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("tier_roundness", "Tier roundness", &ConiferSettings::tier_roundness, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("crown_shade", "Crown self-shade", &ConiferSettings::crown_shade, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("wind_strength", "Wind strength (m)", &ConiferSettings::wind_strength, 0.0f, 3.0f, 0.0f, 1.5f, 0.05f),
	float_row("wind_speed", "Wind speed", &ConiferSettings::wind_speed, 0.0f, 8.0f, 0.0f, 3.0f, 0.1f),
	float_row("wind_scale", "Wind scale (1/m)", &ConiferSettings::wind_scale, 0.0f, 4.0f, 0.0f, 0.5f, 0.005f),
	float_row("gloss", "Gloss", &ConiferSettings::gloss, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("hue_jitter", "Hue jitter", &ConiferSettings::hue_jitter, 0.0f, 1.0f, 0.0f, 0.5f, 0.01f),
	float_row("leaf_grain", "Needle grain", &ConiferSettings::leaf_grain, 0.5f, 12.0f, 0.5f, 12.0f, 0.25f),
	float_row("palette_top_r", "Top R", &ConiferSettings::palette_top_r, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_top_g", "Top G", &ConiferSettings::palette_top_g, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_top_b", "Top B", &ConiferSettings::palette_top_b, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_under_r", "Under R", &ConiferSettings::palette_under_r, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_under_g", "Under G", &ConiferSettings::palette_under_g, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_under_b", "Under B", &ConiferSettings::palette_under_b, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
};

} // namespace

std::span<const SettingRow<ConiferSettings>> conifer_rows() { return kConiferRows; }

void clamp_conifer_settings(ConiferSettings *s) { clamp_all<ConiferSettings>(kConiferRows, s); }

} // namespace ve
