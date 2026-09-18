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
		// Absolute 10 m steps, with the LAST ring stretched to the reach: the reach follows the
		// raymarcher's seam and moves with streaming, so ring boundaries that were fractions of
		// it would re-thin the grass under the player every time the seam stepped out.
		l.ring_end_m[i] = i == kGrassRings - 1
				? s.reach_m
				: std::min(s.reach_m, kGrassRingStepM * static_cast<float>(i + 1));
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
	// The box is as tall as the reach lets it be: grass has to cover everything the raymarcher
	// draws, which is a SPHERE of resident bricks around the camera -- the valley floor thirty
	// metres below a clifftop is raytraced ground like any other. vertical_reach_m survives as
	// the dial that shortens it back to a slab when the blades under an overhang are not worth
	// their cost.
	const float vertical = std::min(s.vertical_reach_m, s.reach_m);
	if (live) {
		l.brick_min = IVec3{floor_div_brick(camera[0] - s.reach_m),
				floor_div_brick(camera[1] - vertical),
				floor_div_brick(camera[2] - s.reach_m)};
		l.brick_max = IVec3{floor_div_brick(camera[0] + s.reach_m),
				floor_div_brick(camera[1] + vertical),
				floor_div_brick(camera[2] + s.reach_m)};
	}

	const int dim_x = live ? (l.brick_max.x - l.brick_min.x + 1) : 0;
	const int dim_y = live ? (l.brick_max.y - l.brick_min.y + 1) : 0;
	const int dim_z = live ? (l.brick_max.z - l.brick_min.z + 1) : 0;
	// One thread per COLUMN. A thread per brick was affordable while the box was a 20 m-thick
	// slab; over the whole sphere it would be its volume, and the column walk visits the same
	// bricks with a thirtieth of the threads (grass_bricks.comp.glsl).
	l.near_columns = dim_x * dim_z;
	l.near_brick_cap = l.near_columns * kGrassColumnBricks;

	// Far LoD rings. Ring r covers out to kGrassFarBaseM << r in cells of kBrickSize << r, so
	// the cell COUNT is the same for every ring and the dispatch grows linearly in ring count
	// rather than with the cube of the radius. Two cells of slack on each side because the
	// grid is anchored on the floor of the ring's own cell lattice, not on the camera. The
	// schedule is absolute, so a near reach that grows with streaming pushes ring 1's INNER
	// edge out (the rings keep their LoD) instead of coarsening every ring with it.
	l.far_ring_count = live ? s.far_lod_rings : 0;
	if (l.far_ring_count > 0 && s.far_blades_per_cell > 0) {
		l.far_cell_dim = static_cast<int>(std::ceil(2.0f * kGrassFarBaseM / kBrickSize)) + 2;
		l.far_cells = l.far_cell_dim * l.far_cell_dim;
		l.far_reach_m = kGrassFarBaseM * static_cast<float>(1 << l.far_ring_count);
	} else {
		l.far_ring_count = 0;
	}
	l.max_bricks = l.near_brick_cap + l.far_ring_count * l.far_cells;
	l.dispatch_threads = l.near_columns + l.far_ring_count * l.far_cells;

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
	// Far rings: one flat annulus of cells, no slope slack -- a far cell places blades on
	// its own tangent plane, so a hillside gives one cell's worth either way.
	for (int i = 1; i <= l.far_ring_count; i++) {
		const float outer = kGrassFarBaseM * static_cast<float>(1 << i);
		// The near field owns everything inside the reach, so a ring's inner edge is whichever
		// is further out -- which is how extending the near reach pays for itself here. The
		// FIRST ring instead starts exactly at the reach, so the two fields meet however short
		// the reach is (grass_bricks.comp.glsl).
		const float inner = i == 1 ? s.reach_m
				: std::max(kGrassFarBaseM * static_cast<float>(1 << (i - 1)), s.reach_m);
		if (outer <= inner) continue;
		const float cell = kBrickSize * static_cast<float>(1 << i);
		const double area = 3.14159265358979 * (static_cast<double>(outer) * outer -
				static_cast<double>(inner) * inner);
		blades += area / (static_cast<double>(cell) * cell) * s.far_blades_per_cell;
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
	p.brick_dim[3] = l.near_columns;
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
	// And the last one after it: blade lighting, same argument.
	p.style[3] = s.blade_lighting;
	p.limits[0] = s.max_blades;
	p.limits[1] = l.max_bricks;
	p.far[0] = l.far_ring_count;
	p.far[1] = l.far_cell_dim;
	p.far[2] = l.far_cells;
	p.far[3] = l.far_ring_count > 0 ? s.far_blades_per_cell : 0;
	return l;
}

} // namespace ve
