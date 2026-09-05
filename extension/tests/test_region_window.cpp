#include <doctest/doctest.h>
#include "world/region_window.h"
#include <set>

TEST_CASE("window dim is the next power of two covering twice the evict radius") {
	// 96 m radius x 1.15 margin = 110.4 m; resident span 220.8 m; 220.8 / 25.6 = 8.63;
	// ceil + 1 = 10; next_pow2 = 16.
	CHECK(ve::region_window_dim(96.0f, 1.15f) == 16);
	// A tiny radius still yields a usable window.
	CHECK(ve::region_window_dim(10.0f, 1.15f) >= 4);
	// The result is always a power of two.
	for (float r = 8.0f; r < 400.0f; r += 7.0f) {
		const int d = ve::region_window_dim(r, 1.15f);
		CHECK((d & (d - 1)) == 0);
	}
}

TEST_CASE("the window spans more than twice the evict radius") {
	// This is invariant 3: two regions that alias are `dim` regions apart, and two
	// simultaneously resident regions are at most 2 * radius * margin apart.
	for (float r = 8.0f; r < 400.0f; r += 7.0f) {
		const int d = ve::region_window_dim(r, 1.15f);
		CHECK(float(d) * ve::kRegionSize > 2.0f * r * 1.15f);
	}
}

TEST_CASE("index is toroidal and total") {
	const ve::RegionWindow w = ve::region_window_centered(0.0f, 0.0f, 0.0f, 16);
	// Every index is in range, including for regions far outside the window.
	for (int x = -1000; x <= 1000; x += 37) {
		const int i = w.index({x, 0, 0});
		CHECK(i >= 0);
		CHECK(i < w.cell_count());
	}
	// Regions `dim` apart alias onto the same cell; that is the property `contains` guards.
	CHECK(w.index({0, 0, 0}) == w.index({16, 0, 0}));
	CHECK(w.index({0, 0, 0}) != w.index({1, 0, 0}));
}

TEST_CASE("contains is the window AABB, and every contained region has a unique cell") {
	const ve::RegionWindow w = ve::region_window_centered(400.0f, 0.0f, 400.0f, 16);
	std::set<int> cells;
	int contained = 0;
	for (int z = w.origin.z; z < w.origin.z + w.dim; z++)
		for (int y = w.origin.y; y < w.origin.y + w.dim; y++)
			for (int x = w.origin.x; x < w.origin.x + w.dim; x++) {
				const ve::IVec3 r{x, y, z};
				CHECK(w.contains(r));
				cells.insert(w.index(r));
				contained++;
			}
	CHECK(contained == w.cell_count());
	CHECK(int(cells.size()) == w.cell_count()); // a bijection, so no in-window aliasing
	CHECK(!w.contains({w.origin.x - 1, w.origin.y, w.origin.z}));
	CHECK(!w.contains({w.origin.x + w.dim, w.origin.y, w.origin.z}));
}

TEST_CASE("centering floors toward negative coordinates") {
	// -1.0 m is region -1, not region 0: the window must not truncate toward zero.
	const ve::RegionWindow w = ve::region_window_centered(-1.0f, -1.0f, -1.0f, 16);
	CHECK(w.contains({-1, -1, -1}));
	CHECK(w.origin.x == -1 - 8);
}
