#pragma once
#include "lod/lod_quad.h"
#include <vector>

namespace ve {

// Finite overlap, not transition meshing: two local cells outward and two cells into the
// parent plane cover the regular 2:1 footprint mismatch with margin. Mirror in lod_quad.glslh.
inline constexpr int kLodSkirtCells = 2;
// Per-coordinate safety bound. A near-tangent plane can require arbitrarily large travel
// to satisfy both overlap and inward depth. Reject that edge rather than invent geometry.
inline constexpr int kLodSkirtMaxExtensionCells = 2 * kLodSkirtCells;

struct LodSkirtStatus {
	int unsupported_edges = 0; // no bounded outward/into-solid solution
	int capacity_edges = 0;    // pair would exceed the record cap
};

// Append ribbons only to exposed cell-pair edges on the chunk boundary. Internal edges
// shared by two surface quads are not perimeter, even when every parent has u[axis]==31.
// Each ribbon retains two original endpoints, extends the other two outward AND into solid,
// and keeps the parent's material/shading normal. At a corner the two boundary directions
// extend together. Opposite-wound pairs are atomic at the chunk cap. Repeated calls are safe.
// Returns records added. The finite cap can leave edges uncovered; this is not a guarantee
// for arbitrary field/topology differences between levels or missing neighbouring chunks.
int lod_append_skirts(std::vector<LodQuad> *quads, LodSkirtStatus *status = nullptr);
// Production overload: keeps the separate normal stream exactly aligned with appended
// ribbons. Ribbon corners inherit the attached parent endpoint's filtered normal.
int lod_append_skirts(std::vector<LodQuad> *quads, std::vector<LodQuadNormals> *normals,
		LodSkirtStatus *status = nullptr);

} // namespace ve
