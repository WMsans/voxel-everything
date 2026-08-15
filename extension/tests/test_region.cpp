#include <doctest/doctest.h>
#include <initializer_list>
#include "world/region.h"

TEST_CASE("floor_div and floor_mod are exact for negative numerators") {
	CHECK(ve::floor_div(-1, 32) == -1);
	CHECK(ve::floor_div(-32, 32) == -1);
	CHECK(ve::floor_div(-33, 32) == -2);
	CHECK(ve::floor_div(31, 32) == 0);
	CHECK(ve::floor_mod(-1, 32) == 31);
	CHECK(ve::floor_mod(-32, 32) == 0);
	CHECK(ve::floor_mod(-33, 32) == 31);
	for (int a = -100; a <= 100; a++)
		CHECK(ve::floor_div(a, 32) * 32 + ve::floor_mod(a, 32) == a);
}

TEST_CASE("region_of_brick partitions the brick lattice, negatives included") {
	CHECK(ve::WorldBounds::region_of_brick({0, 0, 0}) == ve::IVec3{0, 0, 0});
	CHECK(ve::WorldBounds::region_of_brick({31, 31, 31}) == ve::IVec3{0, 0, 0});
	CHECK(ve::WorldBounds::region_of_brick({32, 0, 0}) == ve::IVec3{1, 0, 0});
	CHECK(ve::WorldBounds::region_of_brick({0, -1, 0}) == ve::IVec3{0, -1, 0});
	CHECK(ve::WorldBounds::region_of_brick({0, -64, 0}) == ve::IVec3{0, -2, 0});
}

TEST_CASE("brick_index_in_region is a bijection over one region") {
	// Offsetting the region by a negative multiple must not change the index pattern.
	for (int ry : {0, -2}) {
		bool seen[ve::kRegionBrickCount] = {};
		for (int z = 0; z < ve::kRegionBricks; z++)
			for (int y = 0; y < ve::kRegionBricks; y++)
				for (int x = 0; x < ve::kRegionBricks; x++) {
					const ve::IVec3 b{x, ry * ve::kRegionBricks + y, z};
					const int i = ve::WorldBounds::brick_index_in_region(b);
					REQUIRE(i >= 0);
					REQUIRE(i < ve::kRegionBrickCount);
					CHECK_FALSE(seen[i]);
					seen[i] = true;
				}
		for (bool s : seen) CHECK(s);
	}
}

TEST_CASE("brick_of_point floors, including below the origin plane") {
	CHECK(ve::WorldBounds::brick_of_point(0.0f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::WorldBounds::brick_of_point(0.79f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::WorldBounds::brick_of_point(0.81f, 0.0f, 0.0f) == ve::IVec3{1, 0, 0});
	CHECK(ve::WorldBounds::brick_of_point(-0.01f, 0.0f, 0.0f) == ve::IVec3{-1, 0, 0});
	CHECK(ve::WorldBounds::region_of_point(0.0f, -0.01f, 0.0f) == ve::IVec3{0, -1, 0});
}

TEST_CASE("bounds containment and dense region indexing") {
	ve::WorldBounds b{{0, -64, 0}, {64, 8, 64}};
	CHECK(b.size_bricks() == ve::IVec3{2048, 256, 2048});
	CHECK(b.origin_regions() == ve::IVec3{0, -2, 0});
	CHECK(b.contains_region({0, -2, 0}));
	CHECK(b.contains_region({63, 5, 63}));
	CHECK_FALSE(b.contains_region({64, 0, 0}));
	CHECK_FALSE(b.contains_region({0, -3, 0}));
	CHECK(b.region_index({0, -2, 0}) == 0);
	CHECK(b.region_index({1, -2, 0}) == 1);
	CHECK(b.region_index({0, -1, 0}) == 64);
	CHECK(b.region_index({0, -2, 1}) == 64 * 8);
	CHECK(b.region_index({64, 0, 0}) == -1);
	CHECK(b.contains_brick({0, -64, 0}));
	CHECK_FALSE(b.contains_brick({0, -65, 0}));
	CHECK(b.contains_brick({2047, 191, 2047}));
	CHECK_FALSE(b.contains_brick({2048, 0, 0}));
}

TEST_CASE("world aabb spans the bricks the bounds contain") {
	ve::WorldBounds b{{0, -64, 0}, {64, 8, 64}};
	float lo[3], hi[3];
	b.aabb(lo, hi);
	CHECK(lo[0] == doctest::Approx(0.0f));
	CHECK(lo[1] == doctest::Approx(-51.2f));
	CHECK(hi[0] == doctest::Approx(1638.4f));
	CHECK(hi[1] == doctest::Approx(153.6f));
}

TEST_CASE("brick_world_origin ignores the bounds origin (brick coords are global)") {
	float p[3];
	ve::brick_world_origin({-64, -64, 3}, p);
	CHECK(p[0] == doctest::Approx(-51.2f));
	CHECK(p[2] == doctest::Approx(2.4f));
}
