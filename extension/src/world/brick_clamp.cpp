#include "world/brick.h"
#include "world/brick_clamp.h"
#include "world/material_table.h"
#include <cmath>

namespace ve {

namespace {

// The SIGNED eikonal band: |self - neighbour| may not exceed one voxel pitch, in either
// direction. Sign is preserved because the correction only ever pulls self toward zero,
// and floors at the smallest same-sign magnitude the lattice can represent (half a
// quantisation step on either side of encoded mid-scale), so a sample can be drawn to the
// brink but never flipped across it. Note the bound must be signed: a magnitude-only
// limit |d| <= |neighbour| + kVoxelSize is trivially satisfied by a step discontinuity
// whose two sides carry equal magnitudes, and would leave the seam untouched.
uint8_t bound(uint8_t self, uint8_t neighbour) {
	constexpr float kMinMagnitude = kSdfRange / 255.0f;
	const float d = decode_sdf(self);
	const float n = decode_sdf(neighbour);
	if (d > 0.0f) {
		const float hi = std::max(n + kVoxelSize, kMinMagnitude);
		return d <= hi ? self : encode_sdf(hi);
	}
	const float lo = std::min(n - kVoxelSize, -kMinMagnitude);
	return d >= lo ? self : encode_sdf(lo);
}

} // namespace

void clamp_brick_lattice(uint8_t *sdf) {
	const int s = kBrickSdfStride;
	for (int iter = 0; iter < kClampIterations; iter++) {
		for (int z = 0; z < s; z++)
			for (int y = 0; y < s; y++)
				for (int x = 0; x < s; x++) {
					const int i = sdf_index(x, y, z);
					uint8_t v = sdf[i];
					if (x > 0)     v = bound(v, sdf[sdf_index(x - 1, y, z)]);
					if (x + 1 < s) v = bound(v, sdf[sdf_index(x + 1, y, z)]);
					if (y > 0)     v = bound(v, sdf[sdf_index(x, y - 1, z)]);
					if (y + 1 < s) v = bound(v, sdf[sdf_index(x, y + 1, z)]);
					if (z > 0)     v = bound(v, sdf[sdf_index(x, y, z - 1)]);
					if (z + 1 < s) v = bound(v, sdf[sdf_index(x, y, z + 1)]);
					sdf[i] = v;
				}
	}
}

bool lattice_needs_clamp(const uint16_t *mat, int count, const EditOp *ops, int op_count) {
	bool has_subtract = false;
	for (int i = 0; i < op_count && !has_subtract; i++)
		has_subtract = ops[i].type == kOpSphereSubtract;
	if (!has_subtract) return false;

	// Two materials whose hardness DIFFERS. Two different materials that carve identically
	// produce no discontinuity, so comparing ids rather than hardness would clamp bricks
	// that never needed it.
	bool seen = false;
	float first = 0.0f;
	for (int i = 0; i < count; i++) {
		if (mat[i] == 0) continue;
		const float h = material_hardness(mat[i]);
		if (!seen) { first = h; seen = true; continue; }
		if (h != first) return true;
	}
	return false;
}

} // namespace ve
