#pragma once
#include "lod/lod_quad.h"
#include <cstdint>
#include <vector>

namespace ve {

// Spec section 3.3: a chunk is capped at 16 pages of 512 quads. A build that overflows keeps
// its first kLodMaxQuadsPerChunk quads and reports it -- engine spec section 8's fail-soft
// rule. It is also what bounds the per-chunk readback at 96 KB.
inline constexpr int kLodQuadsPerPage = 512;
inline constexpr int kLodVertsPerPage = kLodQuadsPerPage * 4; // 2048
inline constexpr int kLodMaxPagesPerChunk = 16;
inline constexpr int kLodMaxQuadsPerChunk = kLodQuadsPerPage * kLodMaxPagesPerChunk; // 8192

struct LodContourResult {
	std::vector<LodQuad> quads;
	bool overflow = false;
};

// Surface nets over a kLodChunkLattice^3 lattice of ENCODED sdf bytes plus a parallel
// material lattice, emitting packed quads. The CPU reference shaders/lod_quads.comp.glsl is
// diffed against, and the source of the skirt pass's input. Solid is decode_sdf(byte) <= 0,
// matching the generator's own rule.
void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out);

} // namespace ve
