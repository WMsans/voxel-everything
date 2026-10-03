#pragma once
#include <cstdint>

namespace ve {

// Separable tent. Spec section 4: the SDF AVERAGES. Voxy's Mipper prefers non-air because
// block data is binary and has no mean; an SDF has one, and an average is symmetric -- it
// preserves craters and spires equally. A solid-preferring min would erase the player's
// craters at distance, which is the wrong failure mode for a destruction demo.
inline constexpr float kLodTentWeights[3] = {0.25f, 0.5f, 0.25f};

// Fine sample j sits at local coordinate (j - 3) / 2 in cells, so j = 3 is the chunk origin,
// and target lattice index i (holding local coordinate i - 1) is centred on fine index
// 2i + 1 with its tent covering 2i, 2i+1, 2i+2.
float lod_fine_local(int j);

// Encode physical metres for a LoD fine lattice: L0 retains the brick range, coarser
// levels store +/-2 cells. Mirror lod_field.comp.glsl. decode_sdf on these bytes gives
// scaled distances (NOT metres); the reducer and contourer only need averages/signs/ratios.
uint8_t lod_encode_sdf(float sdf, float cell_size);

int lod_fine_index(int x, int y, int z);     // kLodFineLattice^3, x fastest
int lod_lattice_index(int x, int y, int z);  // kLodChunkLattice^3, x fastest

// fine_sdf/fine_mat are kLodFineLattice^3; out_sdf/out_mat are kLodChunkLattice^3.
// SDF: tent average of the 27 taps, all encoded at the SAME level's scale via lod_encode_sdf.
// Material: tent-weighted majority over the SOLID taps
// only, ties broken by the centre tap; all-air reduces to material 0.
void lod_reduce_lattice(const uint8_t *fine_sdf, const uint16_t *fine_mat, uint8_t *out_sdf,
		uint16_t *out_mat);

// True when any SOLID sample of a reduced lattice (kLodChunkLattice^3) carries a transparent
// material: the per-job bit lod_reduce.comp.glsl raises, and the only chunks that grow a
// shell (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §5).
bool lod_has_transparent(const uint8_t *lattice, const uint16_t *material);

// The encoded "just outside" value a transparent sample becomes in the opaque lattice: half
// a cell, in the scaled-distance space lod_encode_sdf stores at this level. L0 stores metres,
// so half a 0.4 m cell is 0.2; coarser levels store kSdfRange per two cells, so half a cell
// is a quarter of the range. Mirror of the constant in lod_opaque.comp.glsl.
uint8_t lod_outside_byte(float cell_size);

// The OPAQUE lattice the far field's terrain mesh is contoured from: every solid sample whose
// material is transparent becomes lod_outside_byte(cell_size), everything else is copied. The
// identity on a chunk with no transparent label. `out` must not alias `lattice`. Mirror of
// shaders/lod_opaque.comp.glsl.
void lod_opaque_lattice(const uint8_t *lattice, const uint16_t *material, float cell_size,
		uint8_t *out);

} // namespace ve
