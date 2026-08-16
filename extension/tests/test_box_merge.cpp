#include "mesh/box_merge.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <set>

using namespace ve;

namespace {

std::set<std::tuple<int, int, int>> cover(const std::vector<CellBox> &boxes) {
	std::set<std::tuple<int, int, int>> s;
	for (const CellBox &b : boxes)
		for (int z = b.lo.z; z <= b.hi.z; z++)
			for (int y = b.lo.y; y <= b.hi.y; y++)
				for (int x = b.lo.x; x <= b.hi.x; x++) {
					// A cell must be covered exactly once: overlapping boxes would double
					// an island's mass and subtract the same terrain twice.
					CHECK(s.insert({x, y, z}).second);
				}
	return s;
}

std::set<std::tuple<int, int, int>> as_set(const std::vector<IVec3> &cells) {
	std::set<std::tuple<int, int, int>> s;
	for (const IVec3 &c : cells) s.insert({c.x, c.y, c.z});
	return s;
}

} // namespace

TEST_CASE("a solid block merges to exactly one box") {
	std::vector<IVec3> cells;
	for (int z = 0; z < 3; z++)
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 5; x++) cells.push_back({x + 10, y - 2, z});
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge(cells, kMaxIslandBoxes, &boxes));
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].lo == IVec3{10, -2, 0});
	CHECK(boxes[0].hi == IVec3{14, 1, 2});
	CHECK(boxes[0].cells() == 60);
}

TEST_CASE("the boxes tile the input exactly, whatever its shape") {
	// An L in x/y extruded along z, plus a detached cube: nothing about this is convex.
	std::vector<IVec3> cells;
	for (int z = 0; z < 2; z++) {
		for (int x = 0; x < 6; x++) cells.push_back({x, 0, z});
		for (int y = 1; y < 4; y++) cells.push_back({0, y, z});
	}
	cells.push_back({9, 9, 9});
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge(cells, kMaxIslandBoxes, &boxes));
	CHECK(cover(boxes) == as_set(cells));
	CHECK(boxes.size() <= 4);
}

TEST_CASE("a checkerboard cannot merge and is refused above the cap") {
	std::vector<IVec3> cells;
	for (int z = 0; z < 6; z++)
		for (int y = 0; y < 6; y++)
			for (int x = 0; x < 6; x++)
				if ((x + y + z) % 2 == 0) cells.push_back({x, y, z});
	CHECK(cells.size() == 108);
	std::vector<CellBox> boxes;
	// No two cells share a face, so the merge needs one box each.
	CHECK(greedy_box_merge(cells, 108, &boxes));
	CHECK(boxes.size() == 108);
	CHECK(greedy_box_merge(cells, 64, &boxes) == false);
	CHECK(boxes.empty()); // a refused merge leaves nothing half-built
}

TEST_CASE("an empty input merges to nothing and succeeds") {
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge({}, kMaxIslandBoxes, &boxes));
	CHECK(boxes.empty());
}

TEST_CASE("duplicate input cells are absorbed, not double-counted") {
	std::vector<IVec3> cells{{1, 1, 1}, {1, 1, 1}, {2, 1, 1}};
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge(cells, kMaxIslandBoxes, &boxes));
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].cells() == 2);
}

TEST_CASE("a box's world AABB is its cell range in metres, half-open on the high side") {
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge({{3, -1, 2}}, kMaxIslandBoxes, &boxes));
	REQUIRE(boxes.size() == 1);
	float lo[3], hi[3];
	boxes[0].world_aabb(lo, hi);
	CHECK(lo[0] == doctest::Approx(3.0f * kOccupancyCellSize));
	CHECK(lo[1] == doctest::Approx(-1.0f * kOccupancyCellSize));
	CHECK(hi[0] == doctest::Approx(4.0f * kOccupancyCellSize));
	CHECK(hi[2] == doctest::Approx(3.0f * kOccupancyCellSize));
}
