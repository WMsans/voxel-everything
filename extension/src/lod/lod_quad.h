#pragma once
#include <cstdint>

namespace ve {

inline constexpr int kLodQuadBytes = 12;
inline constexpr int kLodOffsetBits = 5;
inline constexpr int kLodOffsetMax = (1 << kLodOffsetBits) - 1; // 31

// The four cells around a lattice edge, as offsets in the two axes perpendicular to it,
// wound counter-clockwise seen from +axis. Byte-identical to kQuad in dual_contour.cpp and
// QUAD in shaders/mesh_quads.comp.glsl -- there is exactly one winding convention.
inline constexpr int kLodQuadCorners[4][2] = {{-1, -1}, {0, -1}, {0, 0}, {-1, 0}};

// The packed record. Three uint32s rather than a struct with a uvec3 member, because std430
// pads a uvec3 array element to 16 bytes and would silently make every page a third larger.
// The GLSL side reads `uint v[]` and indexes quad * 3 + k for the same reason.
//
//   bits  0..14   owning edge coordinate u    3 x 5 bits   (u in [0, 32))
//   bits 15..16   edge axis                        2 bits
//   bit  17       sign (solid -> air direction)    1 bit
//   bits 18..77   4 corner offsets            4 x 15 bits  (5 bits/axis, frac = o / 31)
//   bits 78..93   material id                     16 bits
//   bit  94       double-sided (skirt)             1 bit
//   bit  95       spare                            1 bit
struct LodQuad {
	uint32_t w[3] = {0u, 0u, 0u};
};
static_assert(sizeof(LodQuad) == kLodQuadBytes);

struct LodQuadFields {
	uint8_t u[3] = {0, 0, 0};          // owned edge coordinate, [0, kLodChunkCells)
	uint8_t axis = 0;                  // 0, 1 or 2
	uint8_t sign = 0;                  // 1 when the sample at the edge's low end is solid
	uint8_t offset[4][3] = {};         // per corner, per axis, [0, kLodOffsetMax]
	uint16_t material = 0;
	uint8_t double_sided = 0;
};

void lod_quad_pack(const LodQuadFields &f, LodQuad *out);
void lod_quad_unpack(const LodQuad &q, LodQuadFields *out);

// frac in [0, 1] -> [0, kLodOffsetMax]. The endpoints are exact, so two quads whose
// vertices sit on the same cell corner decode to the same position and cannot separate.
uint8_t lod_quantise_offset(float frac);

// Mesh-cell array coordinate of corner k. The four differ by -1/0 in the two axes
// perpendicular to `axis` and share their coordinate along it.
void lod_quad_corner_cell(const LodQuadFields &f, int k, int m[3]);

// World position of corner k: origin + (m - 1 + frac) * cell, the mesher's own formula
// (shaders/mesh_cells.comp.glsl and ve::dual_contour agree on it).
void lod_quad_corner_pos(const LodQuadFields &f, int k, const float origin[3], float cell,
		float out[3]);

} // namespace ve
