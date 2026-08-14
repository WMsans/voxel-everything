#pragma once
#include <cstdint>

namespace ve {

inline constexpr int kBrickVoxels = 16;
inline constexpr float kVoxelSize = 0.05f;
inline constexpr float kBrickSize = kBrickVoxels * kVoxelSize; // 0.8m
inline constexpr int kBrickVoxelCount = kBrickVoxels * kBrickVoxels * kBrickVoxels; // 4096
inline constexpr int kBrickPaletteSize = 4;
inline constexpr float kSdfRange = 0.64f; // uint8 maps to [-0.64, +0.64] meters

uint8_t encode_sdf(float d);
float decode_sdf(uint8_t v);

// x + y*16 + z*256; all coords in [0,16)
int voxel_index(int x, int y, int z);

struct Brick {
	uint8_t sdf[kBrickVoxelCount];          // encoded SDF
	uint8_t mat[kBrickVoxelCount / 4];      // 2-bit palette indices, packed
	uint16_t palette[kBrickPaletteSize];    // global material IDs
	uint32_t flags = 0;
};

uint8_t get_mat_index(const Brick &b, int idx);
void set_mat_index(Brick &b, int idx, uint8_t v);

} // namespace ve
