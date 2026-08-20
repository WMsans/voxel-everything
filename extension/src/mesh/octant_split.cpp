#include "mesh/octant_split.h"

namespace ve {

int octant_of(const float centroid[3], const float chunk_center[3]) {
	return (centroid[0] >= chunk_center[0] ? 1 : 0) |
			(centroid[1] >= chunk_center[1] ? 2 : 0) |
			(centroid[2] >= chunk_center[2] ? 4 : 0);
}

void split_octants(const float *positions, const uint32_t *indices, int index_count,
		const float chunk_center[3], std::vector<uint32_t> out[kColliderOctants]) {
	for (int i = 0; i < kColliderOctants; i++) out[i].clear();
	if (!positions || !indices || !chunk_center || index_count < 3) return;

	for (int t = 0; t + 2 < index_count; t += 3) {
		const uint32_t a = indices[t];
		const uint32_t b = indices[t + 1];
		const uint32_t c = indices[t + 2];
		const float centroid[3] = {
				(positions[a * 3] + positions[b * 3] + positions[c * 3]) / 3.0f,
				(positions[a * 3 + 1] + positions[b * 3 + 1] + positions[c * 3 + 1]) / 3.0f,
				(positions[a * 3 + 2] + positions[b * 3 + 2] + positions[c * 3 + 2]) / 3.0f,
		};
		const int octant = octant_of(centroid, chunk_center);
		out[octant].push_back(a);
		out[octant].push_back(b);
		out[octant].push_back(c);
	}
}

} // namespace ve
