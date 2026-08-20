#pragma once

#include <cstdint>
#include <vector>

namespace ve {

inline constexpr int kColliderOctants = 8;

// Returns an octant index whose bits identify the positive x/y/z side of the centre.
int octant_of(const float centroid[3], const float chunk_center[3]);

// Bins whole triangles by centroid. The output indices continue to refer to the original
// position array and retain the source order, so splitting cannot change winding or create a
// seam between bodies.
void split_octants(const float *positions, const uint32_t *indices, int index_count,
		const float chunk_center[3], std::vector<uint32_t> out[kColliderOctants]);

} // namespace ve
