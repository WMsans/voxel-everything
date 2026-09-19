#include "leaves/leaf_layout.h"
#include <algorithm>
#include <cmath>

namespace ve {
namespace {

// Gribb-Hartmann plane extraction from a column-major view-projection, normalised inward.
// Same derivation grass_layout.cpp uses; kept local rather than shared because the two
// modules must be free to move independently.
void extract_planes(const float m[16], float out[6][4]) {
	auto row = [&m](int r, int c) { return m[c * 4 + r]; };
	const int sign[6] = {1, -1, 1, -1, 1, -1};
	const int axis[6] = {0, 0, 1, 1, 2, 2};
	for (int i = 0; i < 6; i++) {
		float p[4];
		for (int c = 0; c < 4; c++)
			p[c] = row(3, c) + float(sign[i]) * row(axis[i], c);
		const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		const float inv = len > 1e-8f ? 1.0f / len : 0.0f;
		for (int c = 0; c < 4; c++) out[i][c] = p[c] * inv;
	}
}

// Fraction of the way to the reach, 0..1.
float reach_t(const LeafLayout &l, float distance_m) {
	if (!(l.reach_m > 0.0f)) return 1.0f;
	return std::clamp(distance_m / l.reach_m, 0.0f, 1.0f);
}

} // namespace

int leaf_clump_budget(const LeafLayout &l, float distance_m) {
	if (l.clumps_per_tree <= 0) return 0;
	if (distance_m > l.reach_m) return 0;
	// Linear in the reach fraction down to 1/8 of the near count. Linear rather than stepped
	// so no ring edge can band, which is the mistake GodotGrass's mesh-swap LOD makes.
	const float f = 1.0f - 0.875f * reach_t(l, distance_m);
	return std::max(1, int(std::lround(float(l.clumps_per_tree) * f)));
}

float leaf_clump_radius(const LeafLayout &l, float distance_m) {
	const int n = leaf_clump_budget(l, distance_m);
	if (n <= 0) return 0.0f;
	// budget * r^2 == clumps_per_tree * r_near^2  =>  r = r_near * sqrt(near / n).
	return l.clump_radius_m * std::sqrt(float(l.clumps_per_tree) / float(n));
}

LeafLayout leaf_layout(const LeafSettings &settings, const float camera[3],
		const float view_proj[16]) {
	LeafSettings s = settings;
	clamp_leaf_settings(&s);

	LeafLayout l;
	l.cell_size_m = kLeafCellM;
	l.reach_m = s.enabled ? s.reach_m : 0.0f;
	l.clumps_per_tree = s.clumps_per_tree;
	l.clump_radius_m = s.clump_radius_m;
	l.max_trees = s.max_trees;
	l.max_clumps = s.max_clumps;

	if (l.reach_m > 0.0f && l.clumps_per_tree > 0) {
		const float c = l.cell_size_m;
		const int lo_x = int(std::floor((camera[0] - l.reach_m) / c));
		const int lo_z = int(std::floor((camera[2] - l.reach_m) / c));
		const int hi_x = int(std::floor((camera[0] + l.reach_m) / c));
		const int hi_z = int(std::floor((camera[2] + l.reach_m) / c));
		l.cell_min = IVec3{lo_x, 0, lo_z};
		l.cell_dim = IVec3{hi_x - lo_x + 1, 0, hi_z - lo_z + 1};
		l.dispatch_threads = l.cell_dim.x * l.cell_dim.z;
		// Trees the box can hold, capped: density is a fraction, and a tree occupies one cell.
		const long long cells = static_cast<long long>(l.dispatch_threads);
		l.max_trees = int(std::min<long long>(l.max_trees, std::max<long long>(cells, 1)));
		l.estimated_clumps = int(std::min<long long>(
				l.max_clumps,
				static_cast<long long>(l.max_trees) * l.clumps_per_tree));
	}

	LeafParams &p = l.params;
	p.cam[0] = camera[0];
	p.cam[1] = camera[1];
	p.cam[2] = camera[2];
	p.cam[3] = l.reach_m;
	extract_planes(view_proj, p.planes);
	p.cell_min[0] = l.cell_min.x;
	p.cell_min[2] = l.cell_min.z;
	p.cell_min[3] = l.dispatch_threads;
	p.cell_dim[0] = l.cell_dim.x;
	p.cell_dim[2] = l.cell_dim.z;
	// The tree-shape values below are the shipped pipeline's tree-stage defaults DUPLICATED
	// here, not live reads: nothing in this function touches the field pipeline's set-1 UBO,
	// and these literals are exactly what the scatter shader sees through params.tree/
	// params.shape (leaf_trees.comp.glsl's leaf_tree_params() reads only this block). Source
	// of truth: assets/pipelines/trees.pipeline, i.e. the //!param defaults of
	// shaders/stages/trees.field.glslh (cell, density, crown_radius, trunk_radius,
	// trunk_height, branch_radius_min, max_slope; surface_y is ve::kSurfaceY). If an author
	// edits those params (or overrides them in a pipeline file), the trunks move and the
	// canopies do not — so that edit must come here too. test_leaf_layout.cpp pins these
	// values against the resolved shipped pipeline and fails on drift.
	p.tree[0] = l.cell_size_m;
	p.tree[1] = 0.55f;
	p.tree[2] = 4.0f;
	p.tree[3] = 0.30f;
	p.shape[0] = 9.0f;
	p.shape[1] = 0.10f;
	p.shape[2] = 0.6f;
	p.shape[3] = 51.2f; // ve::kSurfaceY
	p.clump[0] = l.clump_radius_m;
	p.clump[1] = s.shell_min;
	p.clump[2] = s.canopy_roundness;
	p.clump[3] = s.crown_shade;
	p.wind[0] = s.wind_strength;
	p.wind[1] = s.wind_speed;
	p.wind[2] = s.wind_scale;
	p.style[0] = s.gloss;
	p.style[1] = s.hue_jitter;
	p.limits[0] = l.max_clumps;
	p.limits[1] = l.max_trees;
	p.limits[2] = l.clumps_per_tree;
	return l;
}

} // namespace ve
