#include "world/brick_mip.h"
#include <algorithm>

namespace ve {

namespace {

void reduce(const uint8_t *cmn, const uint8_t *cmx, int cd, uint8_t *pmn, uint8_t *pmx) {
	const int pd = cd / 2;
	for (int z = 0; z < pd; z++)
		for (int y = 0; y < pd; y++)
			for (int x = 0; x < pd; x++) {
				uint8_t mn = 255, mx = 0;
				for (int dz = 0; dz < 2; dz++)
					for (int dy = 0; dy < 2; dy++)
						for (int dx = 0; dx < 2; dx++) {
							const int c = (2 * x + dx) + (2 * y + dy) * cd +
									(2 * z + dz) * cd * cd;
							mn = std::min(mn, cmn[c]);
							mx = std::max(mx, cmx[c]);
						}
				const int p = x + y * pd + z * pd * pd;
				pmn[p] = mn;
				pmx[p] = mx;
			}
}

} // namespace

void build_brick_mips(const uint8_t *sdf_lattice, BrickMips *out) {
	// Finest level straight off the lattice: cell (i,j,k) covers voxels [2i, 2i+2), whose
	// trilinear corners are lattice samples [2i, 2i+2] inclusive -> a 3^3 block.
	for (int k = 0; k < 8; k++)
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++) {
				uint8_t mn = 255, mx = 0;
				for (int dz = 0; dz <= 2; dz++)
					for (int dy = 0; dy <= 2; dy++)
						for (int dx = 0; dx <= 2; dx++) {
							const uint8_t v =
									sdf_lattice[sdf_index(2 * i + dx, 2 * j + dy, 2 * k + dz)];
							mn = std::min(mn, v);
							mx = std::max(mx, v);
						}
				const int idx = i + j * 8 + k * 64;
				out->mn8[idx] = mn;
				out->mx8[idx] = mx;
			}
	reduce(out->mn8, out->mx8, 8, out->mn4, out->mx4);
	reduce(out->mn4, out->mx4, 4, out->mn2, out->mx2);
}

const uint8_t *mip_min(const BrickMips &m, int level) {
	return level == 0 ? m.mn2 : (level == 1 ? m.mn4 : m.mn8);
}

const uint8_t *mip_max(const BrickMips &m, int level) {
	return level == 0 ? m.mx2 : (level == 1 ? m.mx4 : m.mx8);
}

} // namespace ve
