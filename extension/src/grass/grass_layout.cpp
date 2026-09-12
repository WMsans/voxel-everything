#include "grass/grass_layout.h"
#include "world/brick.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {
namespace {

// Gribb-Hartmann: each plane is a sum or difference of two rows of the view-projection.
// `m` is column-major, so m[col * 4 + row]. Written out rather than looped because the sign
// pattern is the whole content of the function and a loop hides it.
void extract_planes(const float m[16], float out[6][4]) {
	auto row = [&](int r, int c) { return m[c * 4 + r]; };
	const float rows[4][4] = {
		{row(0, 0), row(0, 1), row(0, 2), row(0, 3)},
		{row(1, 0), row(1, 1), row(1, 2), row(1, 3)},
		{row(2, 0), row(2, 1), row(2, 2), row(2, 3)},
		{row(3, 0), row(3, 1), row(3, 2), row(3, 3)},
	};
	auto set = [&](int i, int r, float sign) {
		for (int k = 0; k < 4; k++) out[i][k] = rows[3][k] + sign * rows[r][k];
	};
	set(0, 0, 1.0f);  // left
	set(1, 0, -1.0f); // right
	set(2, 1, 1.0f);  // bottom
	set(3, 1, -1.0f); // top
	set(4, 2, 1.0f);  // near
	set(5, 2, -1.0f); // far
	for (int i = 0; i < 6; i++) {
		const float len = std::sqrt(out[i][0] * out[i][0] + out[i][1] * out[i][1] +
				out[i][2] * out[i][2]);
		if (len > 1e-12f) {
			for (int k = 0; k < 4; k++) out[i][k] /= len;
		} else {
			// Degenerate row: a plane that rejects nothing is the fail-soft answer, exactly
			// as LodCullPass falls back to drawing every page.
			out[i][0] = 0.0f; out[i][1] = 1.0f; out[i][2] = 0.0f; out[i][3] = 1e9f;
		}
	}
}

int floor_div_brick(float world) {
	return static_cast<int>(std::floor(world / kBrickSize));
}

} // namespace

int grass_ring_of(const GrassLayout &l, float distance_m) {
	for (int i = 0; i < l.ring_count; i++) {
		if (distance_m <= l.ring_end_m[i]) return i;
	}
	return -1;
}

GrassLayout grass_layout(const GrassSettings &settings, const float camera[3],
		const float view_proj[16]) {
	GrassSettings s = settings;
	clamp_grass_settings(&s);

	GrassLayout l;
	l.ring_count = kGrassRings;
	for (int i = 0; i < kGrassRings; i++) {
		l.ring_end_m[i] = s.reach_m * static_cast<float>(i + 1) / static_cast<float>(kGrassRings);
		// Halve per ring, with a floor of one so a far ring thins rather than disappears.
		// This used to drop 3 of every 4, which took the default density to one blade per
		// 0.8 m brick by ring 2 and made the field end abruptly instead of fading. The
		// blades the far rings give up are paid back as width in the vertex shader, via
		// shape[3] -- count down, size up, coverage flat.
		l.blades_per_brick[i] = std::max(1, s.blades_per_brick >> i);
	}
	if (s.blades_per_brick == 0) {
		for (int i = 0; i < kGrassRings; i++) l.blades_per_brick[i] = 0;
	}

	const bool live = s.enabled && s.reach_m > 0.0f && s.blades_per_brick > 0 && s.max_blades > 0;
	if (live) {
		l.brick_min = IVec3{floor_div_brick(camera[0] - s.reach_m),
				floor_div_brick(camera[1] - s.vertical_reach_m),
				floor_div_brick(camera[2] - s.reach_m)};
		l.brick_max = IVec3{floor_div_brick(camera[0] + s.reach_m),
				floor_div_brick(camera[1] + s.vertical_reach_m),
				floor_div_brick(camera[2] + s.reach_m)};
	}

	const int dim_x = live ? (l.brick_max.x - l.brick_min.x + 1) : 0;
	const int dim_y = live ? (l.brick_max.y - l.brick_min.y + 1) : 0;
	const int dim_z = live ? (l.brick_max.z - l.brick_min.z + 1) : 0;
	l.max_bricks = dim_x * dim_y * dim_z;

	// Estimate: ground is a surface, so surface bricks in a ring go as its ANNULUS AREA over
	// the brick footprint, times a slack factor for slope (a hillside presents more bricks
	// per square metre of ground plan than a flat field does). Diagnostic only.
	const float kSlopeSlack = 2.0f;
	double blades = 0.0;
	float prev = 0.0f;
	for (int i = 0; i < kGrassRings && live; i++) {
		const float r = l.ring_end_m[i];
		const double area = 3.14159265358979 * (static_cast<double>(r) * r -
				static_cast<double>(prev) * prev);
		const double bricks = area / (kBrickSize * kBrickSize) * kSlopeSlack;
		blades += bricks * l.blades_per_brick[i];
		prev = r;
	}
	l.estimated_blades = static_cast<int>(std::min<double>(blades, s.max_blades));

	GrassParams &p = l.params;
	std::memset(&p, 0, sizeof(p));
	p.cam[0] = camera[0];
	p.cam[1] = camera[1];
	p.cam[2] = camera[2];
	p.cam[3] = s.reach_m;
	extract_planes(view_proj, p.planes);
	p.brick_min[0] = l.brick_min.x;
	p.brick_min[1] = l.brick_min.y;
	p.brick_min[2] = l.brick_min.z;
	p.brick_dim[0] = dim_x;
	p.brick_dim[1] = dim_y;
	p.brick_dim[2] = dim_z;
	p.brick_dim[3] = l.max_bricks;
	for (int i = 0; i < kGrassRings; i++) {
		p.ring_end[i] = l.ring_end_m[i];
		p.ring_blades[i] = l.blades_per_brick[i];
	}
	p.blade[0] = s.blade_width_m;
	p.blade[1] = s.blade_height_m;
	p.blade[2] = s.height_jitter;
	p.blade[3] = s.slope_cos_min;
	p.wind[0] = s.wind_strength;
	p.wind[1] = s.wind_speed;
	p.wind[2] = s.wind_scale;
	p.wind[3] = 0.0f; // the compositor stamps the frame's time before upload
	p.style[0] = s.flower_chance;
	p.style[1] = s.gloss;
	p.shape[0] = s.wind_dir_deg * 3.14159265358979f / 180.0f;
	p.shape[1] = s.lean_spread_rad;
	p.shape[2] = s.base_curve;
	p.shape[3] = s.ring_width_gain;
	// The last free float in the block. Camera tilt is the top-down fix and it needed no new
	// vec4, so the 256-byte layout and its byte-pinned test are untouched.
	p.style[2] = s.camera_tilt;
	p.limits[0] = s.max_blades;
	p.limits[1] = l.max_bricks;
	return l;
}

} // namespace ve
