#include "conifers/conifer_layout.h"
#include <algorithm>
#include <cmath>

namespace ve {
namespace {

// Gribb-Hartmann, normalised inward. Kept local, as leaf_layout.cpp and grass_layout.cpp do,
// so the modules move independently.
void extract_planes(const float m[16], float out[6][4]) {
	auto row = [&m](int r, int c) { return m[c * 4 + r]; };
	const int sign[6] = {1, -1, 1, -1, 1, -1};
	const int axis[6] = {0, 0, 1, 1, 2, 2};
	for (int i = 0; i < 6; i++) {
		float p[4];
		for (int c = 0; c < 4; c++) p[c] = row(3, c) + float(sign[i]) * row(axis[i], c);
		const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		const float inv = len > 1e-8f ? 1.0f / len : 0.0f;
		for (int c = 0; c < 4; c++) out[i][c] = p[c] * inv;
	}
}

} // namespace

int conifer_clump_budget(int clumps_per_tree, float card_reach_m, float distance_m) {
	if (clumps_per_tree <= 0) return 0;
	const float t = card_reach_m > 0.0f ? std::clamp(distance_m / card_reach_m, 0.0f, 1.0f) : 1.0f;
	return std::max(1, int(std::lround(float(clumps_per_tree) * (1.0f - 0.875f * t))));
}

ConiferLayout conifer_layout(const ConiferSettings &settings, float cell_m, const float camera[3],
		const float view_proj[16], float field_offset_x, float field_offset_z) {
	ConiferSettings s = settings;
	clamp_conifer_settings(&s);
	ConiferLayout l;
	l.cell_size_m = cell_m;
	const float card = s.enabled ? s.card_reach_m : 0.0f;
	const float far = s.enabled ? std::max(s.impostor_reach_m, card) : 0.0f;

	ConiferPassParams &p = l.params;
	p.cam[0] = camera[0];
	p.cam[1] = camera[1];
	p.cam[2] = camera[2];
	p.cam[3] = card;
	extract_planes(view_proj, p.planes);
	if (far > 0.0f && cell_m > 0.0f) {
		const float sx = camera[0] + field_offset_x, sz = camera[2] + field_offset_z;
		const int lo_x = int(std::floor((sx - far) / cell_m));
		const int lo_z = int(std::floor((sz - far) / cell_m));
		const int hi_x = int(std::floor((sx + far) / cell_m));
		const int hi_z = int(std::floor((sz + far) / cell_m));
		p.cell_min[0] = lo_x;
		p.cell_min[2] = lo_z;
		p.cell_dim[0] = hi_x - lo_x + 1;
		p.cell_dim[2] = hi_z - lo_z + 1;
		l.dispatch_threads = p.cell_dim[0] * p.cell_dim[2];
		p.cell_min[3] = l.dispatch_threads;
	}
	p.reach[0] = far;
	p.reach[1] = s.clump_radius_m;
	p.reach[2] = s.tier_roundness;
	p.reach[3] = s.crown_shade;
	p.look[0] = s.shell_min;
	p.limits[0] = s.max_clumps;
	p.limits[1] = s.max_card_trees;
	p.limits[2] = s.clumps_per_tree;
	p.limits[3] = s.max_impostors;

	LeafParams &r = l.raster;
	r.cam[0] = camera[0];
	r.cam[1] = camera[1];
	r.cam[2] = camera[2];
	r.cam[3] = card; // leaf.frag.glsl fades cards over the last fifth of this
	r.wind[0] = s.wind_strength;
	r.wind[1] = s.wind_speed;
	r.wind[2] = s.wind_scale;
	r.style[0] = s.gloss;
	r.style[1] = s.hue_jitter;
	r.style[2] = s.leaf_grain;
	r.palette_top[0] = s.palette_top_r;
	r.palette_top[1] = s.palette_top_g;
	r.palette_top[2] = s.palette_top_b;
	r.palette_under[0] = s.palette_under_r;
	r.palette_under[1] = s.palette_under_g;
	r.palette_under[2] = s.palette_under_b;
	return l;
}

} // namespace ve
