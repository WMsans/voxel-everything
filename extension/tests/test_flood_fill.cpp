#include "connectivity/flood_fill.h"
#include <doctest/doctest.h>

using namespace ve;

namespace {

// A grid whose every cell is explicitly air, so a test can then carve solids into it and
// know that nothing is left unknown (unknown would anchor everything and prove nothing).
OccupancyGrid air_grid(IVec3 lo, IVec3 hi) {
	OccupancyGrid g;
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) g.set_cell({x, y, z}, kCellAir, 1);
	return g;
}

void fill(OccupancyGrid *g, IVec3 lo, IVec3 hi, CellState s) {
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) g->set_cell({x, y, z}, s, 2);
}

// A 16-cell window at the origin: small enough to read in a debugger, big enough to have an
// interior. Every test here uses it so the boundary shell is at 0 and 15 on each axis.
FloodWindow small_window() {
	FloodWindow w;
	w.lo = {0, 0, 0};
	w.dim = 16;
	return w;
}

} // namespace

TEST_CASE("a window addresses its cells x-fastest and rejects the outside") {
	const FloodWindow w = small_window();
	CHECK(w.cells() == 16 * 16 * 16);
	CHECK(w.index({0, 0, 0}) == 0);
	CHECK(w.index({1, 0, 0}) == 1);
	CHECK(w.index({0, 1, 0}) == 16);
	CHECK(w.index({0, 0, 1}) == 256);
	CHECK(w.index({16, 0, 0}) == -1);
	CHECK(w.index({-1, 0, 0}) == -1);
	CHECK(w.cell_of(256 + 16 + 1) == IVec3{1, 1, 1});
	CHECK(w.on_boundary({0, 5, 5}));
	CHECK(w.on_boundary({15, 5, 5}));
	CHECK(w.on_boundary({5, 5, 15}));
	CHECK(w.on_boundary({1, 5, 5}) == false);
}

TEST_CASE("FloodWindow::around centres a window on a cell AABB") {
	// The AABB spans cells x in [10, 13]; a dim-16 window centred on it starts 6 below the
	// centre 11 (integer midpoint), so the AABB sits comfortably inside the shell.
	const FloodWindow w = FloodWindow::around({10, 20, 30}, {13, 21, 30}, 16);
	CHECK(w.dim == 16);
	CHECK(w.contains({10, 20, 30}));
	CHECK(w.contains({13, 21, 30}));
	// ...and with at least the frontier margin of clearance on every side.
	CHECK(w.index({10 - kFrontierMarginCells, 20 - kFrontierMarginCells,
			30 - kFrontierMarginCells}) >= 0);
	CHECK(w.index({13 + kFrontierMarginCells, 21 + kFrontierMarginCells,
			30 + kFrontierMarginCells}) >= 0);
}

TEST_CASE("a solid slab reaching the shell is entirely anchored") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 3, 15}, kCellFull); // a floor spanning the whole window
	FloodResult r;
	flood_anchored(g, small_window(), nullptr, &r);
	CHECK(r.solid_count == 16 * 4 * 16);
	CHECK(r.anchored_count == r.solid_count);
	CHECK(r.frontier_reached == false);
	// The spec's invariant, stated as an assertion: an anchored cell never becomes an island.
	for (int i = 0; i < r.cells(); i++)
		CHECK((r.solid[i] && !r.anchored[i]) == false);
}

TEST_CASE("a floating cube in the middle of the window is not anchored") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull); // floor
	fill(&g, {7, 8, 7}, {8, 9, 8}, kCellFull);   // a 2x2x2 cube floating above it
	const FloodWindow w = small_window();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	CHECK(r.anchored[w.index({7, 0, 7})] == 1);
	CHECK(r.solid[w.index({7, 8, 7})] == 1);
	CHECK(r.anchored[w.index({7, 8, 7})] == 0);
	CHECK(r.solid_count - r.anchored_count == 8);
	CHECK(r.frontier_reached == false); // the cube is far from the shell
}

TEST_CASE("corner and edge contact do not carry support") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 4, 15}, kCellFull);
	// One cell diagonally above the floor's top layer: it shares only an EDGE with the
	// floor's corner cell, and spec §5 says edge/corner contact never counts.
	g.set_cell({5, 6, 5}, kCellFull, 2);
	g.set_cell({6, 5, 5}, kCellAir, 2); // make sure no face neighbour exists
	g.set_cell({5, 5, 5}, kCellAir, 2);
	const FloodWindow w = small_window();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	CHECK(r.solid[w.index({5, 6, 5})] == 1);
	CHECK(r.anchored[w.index({5, 6, 5})] == 0);
}

TEST_CASE("unknown cells seed the anchor set and conduct support") {
	// Nothing is set at all: the whole window is unknown, which is "not known to be air".
	OccupancyGrid g;
	const FloodWindow w = small_window();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	CHECK(r.solid_count == w.cells());
	CHECK(r.anchored_count == w.cells());
	CHECK(r.frontier_reached == false);
}

TEST_CASE("an unanchored cell near the shell raises the frontier flag") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull);
	// A floating cell one inside the shell -- within kFrontierMarginCells of the boundary,
	// so its true extent may continue outside the window and the caller must expand.
	g.set_cell({1, 8, 8}, kCellFull, 2);
	FloodResult r;
	flood_anchored(g, small_window(), nullptr, &r);
	CHECK(r.frontier_reached == true);
}

TEST_CASE("a cut link stops support crossing it") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull); // floor
	fill(&g, {8, 2, 8}, {8, 6, 8}, kCellFull);   // a one-cell-thick pillar on it
	const FloodWindow w = small_window();

	FloodResult before;
	flood_anchored(g, w, nullptr, &before);
	CHECK(before.anchored[w.index({8, 6, 8})] == 1);

	// Cut the link between the floor's top cell (8,1,8) and the pillar's foot (8,2,8).
	LinkCuts cuts;
	cuts.add({8, 1, 8}, 1);
	CHECK(cuts.size() == 1);
	CHECK(cuts.cut({8, 1, 8}, 1));
	CHECK(cuts.cut({8, 1, 8}, 0) == false);
	CHECK(cuts.cut({8, 2, 8}, 1) == false); // a link is named by its LOWER cell

	FloodResult after;
	flood_anchored(g, w, &cuts, &after);
	CHECK(after.anchored[w.index({8, 6, 8})] == 0);
	CHECK(after.anchored[w.index({8, 2, 8})] == 0);
	CHECK(after.anchored[w.index({8, 1, 8})] == 1);
	CHECK(before.solid_count == after.solid_count);
	CHECK(after.anchored_count == before.anchored_count - 5);
}
