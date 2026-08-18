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

int lod_fine_index(int x, int y, int z);     // kLodFineLattice^3, x fastest
int lod_lattice_index(int x, int y, int z);  // kLodChunkLattice^3, x fastest

// fine_sdf/fine_mat are kLodFineLattice^3; out_sdf/out_mat are kLodChunkLattice^3.
// SDF: tent average of the 27 taps. Material: tent-weighted majority over the SOLID taps
// only, ties broken by the centre tap; all-air reduces to material 0.
void lod_reduce_lattice(const uint8_t *fine_sdf, const uint16_t *fine_mat, uint8_t *out_sdf,
		uint16_t *out_mat);

} // namespace ve
