#include "connectivity/occupancy.h"
#include <doctest/doctest.h>
#include <vector>

using namespace ve;

namespace {

// A block whose cells are all `s`, so a test can install a whole region in one line.
std::vector<uint8_t> uniform_block(CellState s) {
	std::vector<uint8_t> b(kOccupancyBlockBytes, 0);
	for (int i = 0; i < kRegionBrickCount; i++)
		OccupancyGrid::write_packed(b.data(), i, static_cast<uint8_t>(s));
	return b;
}

} // namespace

TEST_CASE("packing round-trips every state at every offset in a byte") {
	std::vector<uint8_t> block(kOccupancyBlockBytes, 0);
	const CellState states[4] = {kCellUnknown, kCellAir, kCellSolid, kCellFull};
	for (int i = 0; i < 64; i++)
		OccupancyGrid::write_packed(block.data(), i, static_cast<uint8_t>(states[i % 4]));
	for (int i = 0; i < 64; i++)
		CHECK(OccupancyGrid::read_packed(block.data(), i) == static_cast<uint8_t>(states[i % 4]));
	// Rewriting one cell must not disturb the three sharing its byte.
	OccupancyGrid::write_packed(block.data(), 5, static_cast<uint8_t>(kCellFull));
	CHECK(OccupancyGrid::read_packed(block.data(), 4) == static_cast<uint8_t>(kCellUnknown));
	CHECK(OccupancyGrid::read_packed(block.data(), 5) == static_cast<uint8_t>(kCellFull));
	CHECK(OccupancyGrid::read_packed(block.data(), 6) == static_cast<uint8_t>(kCellSolid));
	CHECK(OccupancyGrid::read_packed(block.data(), 7) == static_cast<uint8_t>(kCellFull));
}

TEST_CASE("cell index inside a region is x-fastest and floor-modulo for negatives") {
	CHECK(OccupancyGrid::cell_index_in_region({0, 0, 0}) == 0);
	CHECK(OccupancyGrid::cell_index_in_region({1, 0, 0}) == 1);
	CHECK(OccupancyGrid::cell_index_in_region({0, 1, 0}) == kRegionBricks);
	CHECK(OccupancyGrid::cell_index_in_region({0, 0, 1}) == kRegionBricks * kRegionBricks);
	// Cell -1 is the LAST cell of the region below, not index -1.
	CHECK(OccupancyGrid::cell_index_in_region({-1, -1, -1}) == kRegionBrickCount - 1);
}

TEST_CASE("an unwritten grid is entirely unknown, and unknown counts as solid") {
	OccupancyGrid g;
	CHECK(g.region_count() == 0);
	CHECK(g.state({7, -3, 11}) == kCellUnknown);
	CHECK(g.is_known({7, -3, 11}) == false);
	// The safe direction (see the plan's Deliberate Decisions): a cell nobody has looked at
	// is treated as ground, so nothing falls because the world had not streamed in yet.
	CHECK(g.is_solid({7, -3, 11}) == true);
	CHECK(g.is_full({7, -3, 11}) == false);
	CHECK(g.block_seq({0, -1, 0}) == -1);
}

TEST_CASE("a stored block answers for every cell of its region and no others") {
	OccupancyGrid g;
	const std::vector<uint8_t> block = uniform_block(kCellSolid);
	g.set_block({0, -1, 0}, block.data(), 42);
	CHECK(g.region_count() == 1);
	CHECK(g.has_region({0, -1, 0}));
	CHECK(g.block_seq({0, -1, 0}) == 42);
	// Region (0,-1,0) owns bricks x,z in [0,32) and y in [-32,0).
	CHECK(g.state({0, -1, 0}) == kCellSolid);
	CHECK(g.state({31, -32, 31}) == kCellSolid);
	CHECK(g.is_known({31, -32, 31}));
	// One cell past the region on any axis is a different region: still unknown.
	CHECK(g.state({32, -1, 0}) == kCellUnknown);
	CHECK(g.state({0, 0, 0}) == kCellUnknown);
}

TEST_CASE("set_cell edits one cell of an existing block and creates a block when absent") {
	OccupancyGrid g;
	g.set_cell({3, 4, 5}, kCellFull, 7);
	CHECK(g.state({3, 4, 5}) == kCellFull);
	CHECK(g.is_full({3, 4, 5}));
	CHECK(g.is_solid({3, 4, 5}));
	CHECK(g.state({4, 4, 5}) == kCellUnknown); // the rest of the fresh block is unknown
	g.set_cell({4, 4, 5}, kCellAir, 8);
	CHECK(g.is_solid({4, 4, 5}) == false);
	CHECK(g.is_known({4, 4, 5}));
	CHECK(g.block_seq({0, 0, 0}) == 8); // a later write advances the block's sequence
}

TEST_CASE("a re-stored block replaces the old contents and never leaks the old sequence") {
	OccupancyGrid g;
	const std::vector<uint8_t> solid = uniform_block(kCellSolid);
	const std::vector<uint8_t> air = uniform_block(kCellAir);
	g.set_block({1, 0, 1}, solid.data(), 5);
	g.set_block({1, 0, 1}, air.data(), 9);
	CHECK(g.region_count() == 1);
	CHECK(g.state({32, 0, 32}) == kCellAir);
	CHECK(g.block_seq({1, 0, 1}) == 9);
	g.clear();
	CHECK(g.region_count() == 0);
	CHECK(g.state({32, 0, 32}) == kCellUnknown);
}
