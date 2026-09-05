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
// no origin term: brick coordinates name world space directly.
void brick_world_origin(IVec3 b, float out[3]);
void brick_world_aabb(IVec3 b, float lo[3], float hi[3]);

// The brick and region lattices are GLOBAL and unbounded: brick b's world corner is
// b * kBrickSize with no origin term, and every integer coordinate names a real region. There
// is no world extent -- see docs/superpowers/specs/2026-09-04-unbounded-world-design.md.
IVec3 region_of_brick(IVec3 b);
IVec3 brick_of_point(float x, float y, float z);
IVec3 region_of_point(float x, float y, float z);
// 0..kRegionBrickCount-1, x fastest, y, then z.
int brick_index_in_region(IVec3 b);

} // namespace ve
