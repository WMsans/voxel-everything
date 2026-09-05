#include "world/region.h"
#include <cmath>

namespace ve {

int floor_div(int a, int b) {
	const int q = a / b;
	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int floor_mod(int a, int b) {
	const int r = a % b;
	return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

void brick_world_origin(IVec3 b, float out[3]) {
	out[0] = static_cast<float>(b.x) * kBrickSize;
	out[1] = static_cast<float>(b.y) * kBrickSize;
	out[2] = static_cast<float>(b.z) * kBrickSize;
}

void brick_world_aabb(IVec3 b, float lo[3], float hi[3]) {
	brick_world_origin(b, lo);
	hi[0] = lo[0] + kBrickSize;
	hi[1] = lo[1] + kBrickSize;
	hi[2] = lo[2] + kBrickSize;
}

IVec3 region_of_brick(IVec3 b) {
	return {floor_div(b.x, kRegionBricks), floor_div(b.y, kRegionBricks),
			floor_div(b.z, kRegionBricks)};
}

IVec3 brick_of_point(float x, float y, float z) {
	return {static_cast<int>(std::floor(x / kBrickSize)),
			static_cast<int>(std::floor(y / kBrickSize)),
			static_cast<int>(std::floor(z / kBrickSize))};
}

IVec3 region_of_point(float x, float y, float z) {
	return region_of_brick(brick_of_point(x, y, z));
}

int brick_index_in_region(IVec3 b) {
	return floor_mod(b.x, kRegionBricks) + floor_mod(b.y, kRegionBricks) * kRegionBricks +
			floor_mod(b.z, kRegionBricks) * kRegionBricks * kRegionBricks;
}

} // namespace ve
