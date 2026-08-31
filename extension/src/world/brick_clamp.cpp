#include "world/brick.h"
#include "world/brick_clamp.h"
#include <cmath>
#include <cstring>

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

// One snapshot-Jacobi sweep: every sample's new value is computed from the PREVIOUS
// iteration's frozen state, so the visiting order cannot influence the result. This is what
// lets shaders/brick_gen.comp.glsl agree byte for byte with 256 threads racing ahead of each
// other -- a Gauss-Seidel sweep here would be one arbitrary interleaving the GPU cannot
// reproduce (Task 7 review: cross-sign coupling is anti-monotone, forward vs reverse sweeps
// reach genuinely different fixed points on realistic seam fields).
void jacobi_sweep(uint8_t *sdf, const uint8_t *snap) {
	const int s = kBrickSdfStride;
	for (int z = 0; z < s; z++)
		for (int y = 0; y < s; y++)
			for (int x = 0; x < s; x++) {
				const int i = sdf_index(x, y, z);
				uint8_t v = snap[i];
				if (x > 0)     v = bound(v, snap[sdf_index(x - 1, y, z)]);
				if (x + 1 < s) v = bound(v, snap[sdf_index(x + 1, y, z)]);
				if (y > 0)     v = bound(v, snap[sdf_index(x, y - 1, z)]);
				if (y + 1 < s) v = bound(v, snap[sdf_index(x, y + 1, z)]);
				if (z > 0)     v = bound(v, snap[sdf_index(x, y, z - 1)]);
				if (z + 1 < s) v = bound(v, snap[sdf_index(x, y, z + 1)]);
				sdf[i] = v;
			}
}

} // namespace

void clamp_brick_lattice(uint8_t *sdf) {
	uint8_t snap[kBrickSdfCount];
	for (int iter = 0; iter < kClampIterations; iter++) {
		std::memcpy(snap, sdf, kBrickSdfCount);
		jacobi_sweep(sdf, snap);
	}
}

bool lattice_needs_clamp(float min_hardness, float max_hardness,
		const EditOp *ops, int op_count) {
	if (min_hardness == max_hardness) return false;
	for (int i = 0; i < op_count; i++)
		if (ops[i].type == kOpSphereSubtract) return true;
	return false;
}

} // namespace ve
