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

void lod_roots_in_radius(const float cam_pos[3], float radius_m, std::vector<IVec3> *out) {
	out->clear();
	if (!cam_pos || !out || radius_m <= 0.0f) return;
	const int top = kLodLevels - 1;
	const float s = lod_chunk_size(top);
	const IVec3 lo{static_cast<int>(std::floor((cam_pos[0] - radius_m) / s)),
			static_cast<int>(std::floor((cam_pos[1] - radius_m) / s)),
			static_cast<int>(std::floor((cam_pos[2] - radius_m) / s))};
	const IVec3 hi{static_cast<int>(std::floor((cam_pos[0] + radius_m) / s)),
			static_cast<int>(std::floor((cam_pos[1] + radius_m) / s)),
			static_cast<int>(std::floor((cam_pos[2] + radius_m) / s))};
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 c{x, y, z};
				// The sphere test, not the AABB: it trims roughly half the corner candidates.
				if (lod_chunk_distance(top, c, cam_pos) > radius_m) continue;
				out->push_back(c);
			}
}

void op_lod_chunk_range(const EditOp &op, int level, IVec3 *lo, IVec3 *hi) {
	float olo[3], ohi[3];
	op_world_aabb(op, olo, ohi);
	const float pad = std::max(2.0f * lod_cell_size(level), kLatticeFilterPad);
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
