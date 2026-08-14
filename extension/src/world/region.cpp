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

IVec3 WorldBounds::size_bricks() const {
	return {size_regions.x * kRegionBricks, size_regions.y * kRegionBricks,
			size_regions.z * kRegionBricks};
}

IVec3 WorldBounds::origin_regions() const {
	return {floor_div(origin_bricks.x, kRegionBricks), floor_div(origin_bricks.y, kRegionBricks),
			floor_div(origin_bricks.z, kRegionBricks)};
}

IVec3 WorldBounds::region_of_brick(IVec3 b) {
	return {floor_div(b.x, kRegionBricks), floor_div(b.y, kRegionBricks),
			floor_div(b.z, kRegionBricks)};
}

IVec3 WorldBounds::brick_of_point(float x, float y, float z) {
	return {static_cast<int>(std::floor(x / kBrickSize)),
			static_cast<int>(std::floor(y / kBrickSize)),
			static_cast<int>(std::floor(z / kBrickSize))};
}

IVec3 WorldBounds::region_of_point(float x, float y, float z) {
	return region_of_brick(brick_of_point(x, y, z));
}

int WorldBounds::brick_index_in_region(IVec3 b) {
	return floor_mod(b.x, kRegionBricks) + floor_mod(b.y, kRegionBricks) * kRegionBricks +
			floor_mod(b.z, kRegionBricks) * kRegionBricks * kRegionBricks;
}

bool WorldBounds::contains_region(IVec3 r) const {
	const IVec3 o = origin_regions();
	return r.x >= o.x && r.y >= o.y && r.z >= o.z && r.x < o.x + size_regions.x &&
			r.y < o.y + size_regions.y && r.z < o.z + size_regions.z;
}

bool WorldBounds::contains_brick(IVec3 b) const {
	const IVec3 s = size_bricks();
	return b.x >= origin_bricks.x && b.y >= origin_bricks.y && b.z >= origin_bricks.z &&
			b.x < origin_bricks.x + s.x && b.y < origin_bricks.y + s.y &&
			b.z < origin_bricks.z + s.z;
}

int WorldBounds::region_index(IVec3 r) const {
	if (!contains_region(r)) return -1;
	const IVec3 o = origin_regions();
	return (r.x - o.x) + (r.y - o.y) * size_regions.x +
			(r.z - o.z) * size_regions.x * size_regions.y;
}

void WorldBounds::aabb(float lo[3], float hi[3]) const {
	const IVec3 s = size_bricks();
	brick_world_origin(origin_bricks, lo);
	brick_world_origin({origin_bricks.x + s.x, origin_bricks.y + s.y, origin_bricks.z + s.z}, hi);
}

} // namespace ve
