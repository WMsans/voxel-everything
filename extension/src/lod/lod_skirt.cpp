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
		// -normal by pushing every corner offset toward the solid side of the owned edge.
		// Because the offsets are cell-relative, "kLodSkirtCells along -normal" is exactly
		// "clamp the offset on the edge axis to the far end of the solid side".
		LodQuadFields s = f;
		s.double_sided = 1;
		const int axis = f.axis % 3;
		const uint8_t pushed = f.sign ? uint8_t(0) : uint8_t(kLodOffsetMax);
		for (int k = 0; k < 4; k++) s.offset[k][axis] = pushed;

		LodQuad a{};
		lod_quad_pack(s, &a);
		quads->push_back(a);
		added++;
		if (int(quads->size()) >= kLodMaxQuadsPerChunk) break;

		LodQuadFields flipped = s;
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
