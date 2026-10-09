#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Conifer knobs (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md §9). Their own
// module and store, as leaves and grass are. Placement and shape are NOT here: they are the
// conifers stage's params in fjords.pipeline, because the field and the passes must agree.
struct ConiferSettings {
	bool enabled = true;
	// Cards draw out to here and dither out over its last fifth; imposters take exactly the
	// complementary pixels (plan deviation 5).
	float card_reach_m = 300.0f;
	// Imposters draw out to here and dither out over its last fifth; past it the forest
	// material's top mip is the forest.
	float impostor_reach_m = 2500.0f;
	int clumps_per_tree = 128;       // capped at the scatter's workgroup width
	int max_card_trees = 4096;
	int max_clumps = 300000;
	int max_impostors = 120000;
	float clump_radius_m = 1.0f;     // card radius at the nearest distance
	float shell_min = 0.8f;          // clumps sit between this and 1.0 of the tier profile
	float tier_roundness = 0.4f;     // conifer_normal's blend: 0 one cone, 1 every tier a skirt
	float crown_shade = 0.5f;        // how dark the crown base goes relative to its top
	float wind_strength = 0.12f;     // conifers are stiff
	float wind_speed = 0.6f;
	float wind_scale = 0.04f;
	float gloss = 0.1f;
	float hue_jitter = 0.08f;
	float leaf_grain = 6.0f;
	// Spec §5.4's starting palette; tuned against the capture.
	float palette_top_r = 0.20f, palette_top_g = 0.36f, palette_top_b = 0.22f;
	float palette_under_r = 0.05f, palette_under_g = 0.14f, palette_under_b = 0.13f;
};

std::span<const SettingRow<ConiferSettings>> conifer_rows();
void clamp_conifer_settings(ConiferSettings *s);

} // namespace ve
