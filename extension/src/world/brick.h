#pragma once
#include <cstdint>

namespace ve {

inline constexpr int kBrickVoxels = 16;
inline constexpr float kVoxelSize = 0.05f;
inline constexpr float kBrickSize = kBrickVoxels * kVoxelSize; // 0.8m
inline constexpr int kBrickVoxelCount = kBrickVoxels * kBrickVoxels * kBrickVoxels; // 4096
inline constexpr int kBrickPaletteSize = 4;
inline constexpr float kSdfRange = 0.64f; // uint8 maps to [-0.64, +0.64] meters

// The SDF is a LATTICE field, not a cell field: sample n sits at local coordinate n, so
// trilinear reconstruction over a brick's full [0, 16) voxel extent needs a lattice point
// at 16 as well -- that point belongs to the +X/+Y/+Z neighbour's origin plane. Bricks
// therefore carry a one-voxel apron on their positive faces (17^3 samples). Without it the
// last slab of every brick clamps to a constant, the gradient collapses, and calc_normal()
// returns garbage -> dark seams along every brick face. The apron is generated directly
// from the generator, so it is correct even when the neighbouring brick is inactive.
inline constexpr int kBrickSdfStride = kBrickVoxels + 1; // 17
inline constexpr int kBrickSdfCount = kBrickSdfStride * kBrickSdfStride * kBrickSdfStride; // 4913

uint8_t encode_sdf(float d);
float decode_sdf(uint8_t v);

// x + y*16 + z*256; all coords in [0,16). Material/cell indexing.
int voxel_index(int x, int y, int z);

// x + y*17 + z*289; all coords in [0,17). SDF lattice indexing (includes the apron).
int sdf_index(int x, int y, int z);

struct Brick {
	uint8_t sdf[kBrickSdfCount];            // encoded SDF lattice, 17^3 with +face apron
	uint8_t mat[kBrickVoxelCount / 4];      // 2-bit palette indices, packed
	uint16_t palette[kBrickPaletteSize];    // global material IDs
	uint32_t flags = 0;
};

uint8_t get_mat_index(const Brick &b, int idx);
void set_mat_index(Brick &b, int idx, uint8_t v);

} // namespace ve
