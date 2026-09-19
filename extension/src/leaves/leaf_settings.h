#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Canopy knobs. DELIBERATELY not part of BeautySettings and not part of GrassSettings:
// leaves are their own module with their own store, exactly as grass is. The two modules
// share the gust field (shaders/wind.glslh) and nothing else.
struct LeafSettings {
	bool enabled = true;

	// How far canopies are drawn, in metres. Trunks are SDF and are drawn wherever terrain
	// is; only the foliage stops here. Past the reach a clump dithers out with bayer4, the
	// same fade grass uses at its own seam.
	float reach_m = 250.0f;

	// Candidate clumps per tree at the nearest distance. Capped at 128, the scatter's
	// workgroup width -- one thread per candidate, so a tree never needs a second group.
	int clumps_per_tree = 96;
	// Hard cap on the instance buffer. The scatter clamps to it rather than overflowing.
	int max_clumps = 400000;
	// Hard cap on the tree list stage 1 compacts into.
	int max_trees = 8192;

	// Card radius at the nearest distance, metres. Distant trees get FEWER, LARGER clumps
	// (ve::leaf_clump_budget), which holds silhouette coverage at roughly constant instance
	// count -- Ghost of Tsushima's density LOD, the same idea as grass's drop-3-of-4.
	float clump_radius_m = 0.85f;

	// Where in a lobe a clump may sit, as a fraction of the lobe radius. The crown interior
	// is never seen, so filling it is pure overdraw: 0.65 means the inner two thirds of every
	// lobe is left empty.
	float shell_min = 0.65f;

	// How far the per-clump normal leans from the WHOLE crown's sphere normal toward its own
	// lobe's. 0 is one smooth ball, which loses the overlapping masses the reference is built
	// from; 1 shades every lobe as a separate ball, which reads as lumps. This is the leaf
	// module's analogue of GrassSettings::blade_lighting.
	float canopy_roundness = 0.35f;

	// Root-to-canopy self-shade: how dark the base of a crown goes relative to its top.
	// Multiplies the marched sun visibility, so it darkens the crown interior without
	// touching the deferred pass.
	float crown_shade = 0.45f;

	float wind_strength = 0.30f; // metres of clump sway at full gust
	float wind_speed = 0.6f;     // gust scroll rate; matches GrassSettings default so a gust
	float wind_scale = 0.04f;    // frequency, cycles per metre; ditto -- one gust, two modules

	float gloss = 0.15f;
	// Per-clump hue jitter, as a fraction. 0 is a flat green wall.
	float hue_jitter = 0.12f;
};

// One row per knob: name, clamp and slider range. The store, the settings panel, the config
// file and clamp_leaf_settings all read these rows.
std::span<const SettingRow<LeafSettings>> leaf_rows();

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_leaf_settings(LeafSettings *s);

} // namespace ve
