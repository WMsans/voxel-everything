#pragma once
#include "world/brick.h"

namespace ve {

inline constexpr int kRegionBricks = 32;                        // 32^3 bricks per region
inline constexpr float kRegionSize = kRegionBricks * kBrickSize; // 25.6 m
inline constexpr int kRegionBrickCount =
		kRegionBricks * kRegionBricks * kRegionBricks;           // 32768

struct IVec3 {
	int x = 0, y = 0, z = 0;
	bool operator==(const IVec3 &o) const { return x == o.x && y == o.y && z == o.z; }
	bool operator!=(const IVec3 &o) const { return !(*this == o); }
};

// C++ integer division truncates toward zero; the brick lattice extends below y = 0 (the
// world origin sits under the terrain), so every lattice quotient must FLOOR instead.
int floor_div(int a, int b);
int floor_mod(int a, int b);

// Brick coordinates are GLOBAL: the world-space corner of brick b is b * kBrickSize, with
// no origin term. WorldBounds::origin_bricks only decides membership and map indexing.
void brick_world_origin(IVec3 b, float out[3]);

// A bounded, region-aligned world placed on the global brick lattice.
struct WorldBounds {
	IVec3 origin_bricks{0, 0, 0};  // multiple of kRegionBricks on every axis
	IVec3 size_regions{1, 1, 1};

	IVec3 size_bricks() const;
	IVec3 origin_regions() const;

	static IVec3 region_of_brick(IVec3 b);
	static IVec3 brick_of_point(float x, float y, float z);
	static IVec3 region_of_point(float x, float y, float z);
	// 0..kRegionBrickCount-1, x fastest, y, then z.
	static int brick_index_in_region(IVec3 b);

	bool contains_region(IVec3 r) const;
	bool contains_brick(IVec3 b) const;
	// Dense index into the region map (x fastest), or -1 when outside.
	int region_index(IVec3 r) const;
	void aabb(float lo[3], float hi[3]) const;
};

} // namespace ve
