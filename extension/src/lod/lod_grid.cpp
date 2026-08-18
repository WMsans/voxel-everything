#include "lod/lod_grid.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {
int clamp_level(int level) { return std::max(0, std::min(level, kLodLevels - 1)); }
} // namespace

float lod_cell_size(int level) {
	return kLodBaseCell * static_cast<float>(1 << clamp_level(level));
}

float lod_chunk_size(int level) {
	return lod_cell_size(level) * static_cast<float>(kLodChunkCells);
}

IVec3 lod_chunk_of_point(int level, float x, float y, float z) {
	const float s = lod_chunk_size(level);
	return IVec3{static_cast<int>(std::floor(x / s)), static_cast<int>(std::floor(y / s)),
			static_cast<int>(std::floor(z / s))};
}

void lod_chunk_origin(int level, IVec3 c, float out[3]) {
	const float s = lod_chunk_size(level);
	out[0] = static_cast<float>(c.x) * s;
	out[1] = static_cast<float>(c.y) * s;
	out[2] = static_cast<float>(c.z) * s;
}

void lod_chunk_aabb(int level, IVec3 c, float lo[3], float hi[3]) {
	lod_chunk_origin(level, c, lo);
	const float s = lod_chunk_size(level);
	for (int a = 0; a < 3; a++) hi[a] = lo[a] + s;
}

IVec3 lod_parent(IVec3 c) {
	return IVec3{floor_div(c.x, 2), floor_div(c.y, 2), floor_div(c.z, 2)};
}

IVec3 lod_child_base(IVec3 c) { return IVec3{c.x * 2, c.y * 2, c.z * 2}; }

float lod_chunk_distance(int level, IVec3 c, const float p[3]) {
	float lo[3], hi[3];
	lod_chunk_aabb(level, c, lo, hi);
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float d = std::max(std::max(lo[a] - p[a], p[a] - hi[a]), 0.0f);
		d2 += d * d;
	}
	return std::sqrt(d2);
}

float lod_chunk_far_distance(int level, IVec3 c, const float p[3]) {
	float lo[3], hi[3];
	lod_chunk_aabb(level, c, lo, hi);
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float d = std::max(std::fabs(lo[a] - p[a]), std::fabs(hi[a] - p[a]));
		d2 += d * d;
	}
	return std::sqrt(d2);
}

bool lod_chunk_in_bounds(const WorldBounds &b, int level, IVec3 c) {
	float wlo[3], whi[3];
	b.aabb(wlo, whi);
	float clo[3], chi[3];
	lod_chunk_aabb(level, c, clo, chi);
	for (int a = 0; a < 3; a++) {
		if (chi[a] <= wlo[a] || clo[a] >= whi[a]) return false;
	}
	return true;
}

void lod_root_range(const WorldBounds &b, IVec3 *lo, IVec3 *hi) {
	float wlo[3], whi[3];
	b.aabb(wlo, whi);
	const int top = kLodLevels - 1;
	*lo = lod_chunk_of_point(top, wlo[0], wlo[1], wlo[2]);
	// The world's maximum corner is exclusive; nudge inside so a world whose extent lands
	// exactly on a chunk boundary does not claim an extra empty root on every axis.
	const float e = lod_chunk_size(top) * 1e-4f;
	*hi = lod_chunk_of_point(top, whi[0] - e, whi[1] - e, whi[2] - e);
}

void op_lod_chunk_range(const EditOp &op, int level, IVec3 *lo, IVec3 *hi) {
	float olo[3], ohi[3];
	op_world_aabb(op, olo, ohi);
	const float pad = 2.0f * lod_cell_size(level);
	const float p0[3] = {olo[0] - pad, olo[1] - pad, olo[2] - pad};
	const float p1[3] = {ohi[0] + pad, ohi[1] + pad, ohi[2] + pad};
	*lo = lod_chunk_of_point(level, p0[0], p0[1], p0[2]);
	*hi = lod_chunk_of_point(level, p1[0], p1[1], p1[2]);
}

void lod_fade_band(float reach_m, float *fade_start, float *fade_end) {
	float end = kLodFadeEndM;
	if (reach_m > 0.0f) {
		const float usable = reach_m * kLodSeamMarginM;
		const float stepped = std::floor(usable / kLodSeamStepM) * kLodSeamStepM;
		end = std::min(end, stepped);
	}
	end = std::max(end, kLodFadeMinEndM);
	// Keep the band's shape: the spec's 120/150 fades over the last fifth of the range.
	// (Widening it instead was measured and did not reduce the residual unclaimed pixels,
	// so the residue is not the two fields' dither thresholds drifting apart.)
	const float start = end * (kLodFadeStartM / kLodFadeEndM);
	if (fade_start) *fade_start = start;
	if (fade_end) *fade_end = end;
}

} // namespace ve
