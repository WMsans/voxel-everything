#include "world/brick.h"
#include <algorithm>
#include <cmath>

namespace ve {

uint8_t encode_sdf(float d) {
	float t = std::clamp((d + kSdfRange) / (2.0f * kSdfRange), 0.0f, 1.0f);
	return static_cast<uint8_t>(std::lround(t * 255.0f));
}

float decode_sdf(uint8_t v) {
	return (static_cast<float>(v) / 255.0f) * 2.0f * kSdfRange - kSdfRange;
}

int voxel_index(int x, int y, int z) {
	return x + y * kBrickVoxels + z * kBrickVoxels * kBrickVoxels;
}

int sdf_index(int x, int y, int z) {
	return x + y * kBrickSdfStride + z * kBrickSdfStride * kBrickSdfStride;
}

uint8_t get_mat_index(const Brick &b, int idx) {
	return (b.mat[idx >> 2] >> ((idx & 3) * 2)) & 0x3;
}

void set_mat_index(Brick &b, int idx, uint8_t v) {
	uint8_t &byte = b.mat[idx >> 2];
	const int shift = (idx & 3) * 2;
	byte = static_cast<uint8_t>((byte & ~(0x3 << shift)) | ((v & 0x3) << shift));
}

} // namespace ve
