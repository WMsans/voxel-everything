#pragma once
#include "lod/lod_quad.h"
#include <cstdint>
#include <vector>

namespace ve {

// Spec section 3.3: a chunk is capped at 16 pages of 512 quads. A build that overflows keeps
// its first kLodMaxQuadsPerChunk quads and reports it -- engine spec section 8's fail-soft
// rule. It also bounds the packed-quad plus filtered-normal readback at 160 KiB.
inline constexpr int kLodQuadsPerPage = 512;
inline constexpr int kLodVertsPerPage = kLodQuadsPerPage * 4; // 2048
inline constexpr int kLodMaxPagesPerChunk = 16;
inline constexpr int kLodMaxQuadsPerChunk = kLodQuadsPerPage * kLodMaxPagesPerChunk; // 8192

struct LodContourResult {
	std::vector<LodQuad> quads;
	std::vector<LodQuadNormals> normals;
	bool overflow = false;
};

// Surface nets over a kLodChunkLattice^3 lattice of ENCODED sdf bytes plus a parallel
// material lattice, emitting packed quads plus aligned filtered corner normals. The CPU
// reference shaders/lod_quads.comp.glsl is
// diffed against, and the source of the skirt pass's input. Solid is decode_sdf(byte) <= 0,
// matching the generator's own rule. `shell_only` keeps only the quads whose SOLID endpoint
// is a transparent material -- the far field's transparent shell
// (docs/superpowers/specs/2026-09-29-transparent-materials-design.md §5). The default emits
// every quad, exactly as before.
void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out,
		bool shell_only = false);

// Appends a chunk's shell quads after its opaque quads AND skirts, keeping the total inside
// kLodMaxQuadsPerChunk so one chunk never outgrows its 16 pages. True when shell quads had
// to be dropped (report it as the chunk's overflow).
bool lod_append_shell(std::vector<LodQuad> *quads, std::vector<LodQuadNormals> *normals,
		const std::vector<LodQuad> &shell, const std::vector<LodQuadNormals> &shell_normals);

// Whether any of these quads is a shell quad. After lod_opaque_lattice no opaque-mesh quad
// can carry a transparent material, so the material alone tells the two meshes apart.
bool lod_quads_have_transparent(const LodQuad *quads, int count);

} // namespace ve
