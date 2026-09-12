#pragma once
#include "grass/grass_settings.h"
#include "world/region.h" // ve::IVec3

namespace ve {

inline constexpr int kGrassRings = 4;

// Uploaded to a uniform buffer and mirrored by `GrassParams` in shaders/grass.glslh. Laid
// out as sixteen vec4 (256 bytes) so std140 padding cannot disagree with the C++ struct;
// test_grass_layout pins both the size and the first three floats.
//
// Order matters and is asserted: cam_pos first, so a shader reading params.cam.xyz gets the
// camera without an offset table.
struct GrassParams {
	float cam[4];          // xyz camera position, w reach_m
	float planes[6][4];    // frustum planes, inward, normalised: xyz normal, w distance
	int32_t brick_min[4];  // xyz inclusive brick coordinate, w unused
	int32_t brick_dim[4];  // xyz brick counts, w total brick count
	float ring_end[4];     // ring outer radius in metres
	int32_t ring_blades[4]; // candidate blades per brick, per ring
	float blade[4];        // width, height, height_jitter, slope_cos_min
	float wind[4];         // strength, speed, scale, time_seconds
	float style[4];        // flower_chance, gloss, camera_tilt, blade_lighting
	float shape[4];        // wind_dir_rad, lean_spread_rad, base_curve, ring_width_gain
	int32_t limits[4];     // max_blades, max_bricks, unused, unused
};

struct GrassLayout {
	IVec3 brick_min{};
	IVec3 brick_max{};
	int ring_count = kGrassRings;
	float ring_end_m[kGrassRings] = {0, 0, 0, 0};
	int blades_per_brick[kGrassRings] = {0, 0, 0, 0};
	// Threads stage 1 dispatches: the full brick box, one thread per brick.
	int max_bricks = 0;
	// Diagnostic upper bound on blades this layout can produce, already capped by
	// GrassSettings::max_blades. The instance buffer is sized from settings.max_blades,
	// not from this -- see the design doc's failure-modes section.
	int estimated_blades = 0;
	GrassParams params{};
};

// `settings` is clamped internally, so a caller may pass an unclamped snapshot.
// `view_proj` is column-major, the same order the compositor builds for the raymarcher.
GrassLayout grass_layout(const GrassSettings &settings, const float camera[3],
		const float view_proj[16]);

// Ring index for a distance, or -1 past the reach.
int grass_ring_of(const GrassLayout &l, float distance_m);

} // namespace ve
