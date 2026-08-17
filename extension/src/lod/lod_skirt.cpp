#include "lod/lod_skirt.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include <algorithm>

namespace ve {

namespace {

bool is_boundary(const LodQuadFields &f) {
	for (int a = 0; a < 3; a++)
		if (f.u[a] == 0 || f.u[a] == kLodChunkCells - 1) return true;
	return false;
}

} // namespace

int lod_append_skirts(std::vector<LodQuad> *quads) {
	if (!quads || quads->empty()) return 0;
	const size_t surface = quads->size();
	int added = 0;
	for (size_t i = 0; i < surface; i++) {
		if (int(quads->size()) >= kLodMaxQuadsPerChunk) break;
		LodQuadFields f{};
		lod_quad_unpack((*quads)[i], &f);
		if (f.double_sided) continue; // never skirt a skirt
		if (!is_boundary(f)) continue;

		// The curtain shares the parent's four cells and its material; it is displaced along
		// -normal by shifting the owned edge coordinate by kLodSkirtCells. Perpendicular
		// offsets are unchanged, so the curtain is exactly kLodSkirtCells deep.
		LodQuadFields s = f;
		s.double_sided = 1;
		const int axis = f.axis % 3;
		const int delta = f.sign ? -kLodSkirtCells : kLodSkirtCells;
		s.u[axis] = static_cast<uint8_t>(static_cast<int>(f.u[axis]) + delta);

		LodQuad a{};
		lod_quad_pack(s, &a);
		quads->push_back(a);
		added++;
		if (int(quads->size()) >= kLodMaxQuadsPerChunk) break;

		LodQuadFields flipped = s;
		flipped.sign = static_cast<uint8_t>(s.sign ^ 1);
		for (int x = 0; x < 3; x++) {
			flipped.offset[1][x] = s.offset[3][x];
			flipped.offset[3][x] = s.offset[1][x];
		}
		LodQuad b{};
		lod_quad_pack(flipped, &b);
		quads->push_back(b);
		added++;
	}
	return added;
}

} // namespace ve
