#include "connectivity/components.h"
#include <doctest/doctest.h>
#include <algorithm>

using namespace ve;

namespace {

FloodWindow window16() {
	FloodWindow w;
	w.lo = {0, 0, 0};
	w.dim = 16;
	return w;
}

// A hand-built flood result: every listed cell is solid and unanchored, nothing else exists.
FloodResult floating(const FloodWindow &w, const std::vector<IVec3> &cells) {
	FloodResult r;
	r.window = w;
	r.solid.assign(static_cast<size_t>(w.cells()), 0);
	r.anchored.assign(static_cast<size_t>(w.cells()), 0);
	for (const IVec3 &c : cells) {
		const int i = w.index(c);
		REQUIRE(i >= 0);
		r.solid[i] = 1;
		r.solid_count++;
	}
	return r;
}

bool has_cell(const IslandComponent &c, IVec3 v) {
	return std::find(c.cells.begin(), c.cells.end(), v) != c.cells.end();
}

bool component_is_connected(const IslandComponent &c) {
	if (c.cells.empty()) return false;
	std::vector<uint8_t> seen(c.cells.size(), 0);
	std::vector<size_t> stack;
	seen[0] = 1;
	stack.push_back(0);
	size_t visited = 0;
	while (!stack.empty()) {
		const size_t k = stack.back();
		stack.pop_back();
		visited++;
		const IVec3 v = c.cells[k];
		for (int axis = 0; axis < 3; axis++) {
			for (int sign = -1; sign <= 1; sign += 2) {
				IVec3 n = v;
				if (axis == 0) n.x += sign;
				else if (axis == 1) n.y += sign;
				else n.z += sign;
				for (size_t j = 0; j < c.cells.size(); j++) {
					if (!seen[j] && c.cells[j] == n) {
						seen[j] = 1;
						stack.push_back(j);
					}
				}
			}
		}
	}
	return visited == c.cells.size();
}

} // namespace

TEST_CASE("two separated blobs are two components, and each carries its own AABB") {
	const FloodWindow w = window16();
	const FloodResult r = floating(w, {{2, 2, 2}, {3, 2, 2}, {2, 3, 2}, {10, 10, 10}});
	std::vector<IslandComponent> out;
	label_islands(r, ComponentConfig{}, &out);
	REQUIRE(out.size() == 2);
	// Ordered by first cell in window index order, so the test can name them.
	CHECK(out[0].cell_count() == 3);
	CHECK(out[0].lo == IVec3{2, 2, 2});
	CHECK(out[0].hi == IVec3{3, 3, 2});
	CHECK(out[0].extent_cells(0) == 2);
	CHECK(out[0].extent_cells(2) == 1);
	CHECK(out[1].cell_count() == 1);
	CHECK(out[1].lo == IVec3{10, 10, 10});
}

TEST_CASE("cells touching only at a corner are two components") {
	const FloodWindow w = window16();
	const FloodResult r = floating(w, {{4, 4, 4}, {5, 5, 5}});
	std::vector<IslandComponent> out;
	label_islands(r, ComponentConfig{}, &out);
	CHECK(out.size() == 2);
}

TEST_CASE("anchored cells are never labelled") {
	const FloodWindow w = window16();
	FloodResult r = floating(w, {{4, 4, 4}, {5, 4, 4}});
	r.anchored[w.index({5, 4, 4})] = 1; // pretend the fill reached this one
	r.anchored_count = 1;
	std::vector<IslandComponent> out;
	label_islands(r, ComponentConfig{}, &out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].cell_count() == 1);
	CHECK(has_cell(out[0], {4, 4, 4}));
	CHECK(has_cell(out[0], {5, 4, 4}) == false);
}

TEST_CASE("a component wider than the volume can hold is split, losing no cells") {
	const FloodWindow w = window16();
	// A 12-cell-long bar: 9.6 m, past the 6.4 m an island volume can cover at its coarse
	// pitch, so it must come back as pieces that each fit.
	std::vector<IVec3> bar;
	for (int x = 1; x <= 12; x++) bar.push_back({x, 5, 5});
	const FloodResult r = floating(w, bar);

	ComponentConfig cfg;
	cfg.max_extent_cells = 8;
	std::vector<IslandComponent> out;
	label_islands(r, cfg, &out);
	CHECK(out.size() >= 2);
	int total = 0;
	for (const IslandComponent &c : out) {
		total += c.cell_count();
		CHECK(c.extent_cells(0) <= cfg.max_extent_cells);
		CHECK(c.extent_cells(1) <= cfg.max_extent_cells);
		CHECK(c.extent_cells(2) <= cfg.max_extent_cells);
	}
	CHECK(total == 12);
}

TEST_CASE("a component with more cells than the cap is split too") {
	const FloodWindow w = window16();
	std::vector<IVec3> block;
	for (int z = 1; z <= 6; z++)
		for (int y = 1; y <= 6; y++)
			for (int x = 1; x <= 6; x++) block.push_back({x, y, z});
	const FloodResult r = floating(w, block); // 216 cells inside an 8-cell extent

	ComponentConfig cfg;
	cfg.max_extent_cells = 8;
	cfg.max_cells = 64;
	std::vector<IslandComponent> out;
	label_islands(r, cfg, &out);
	CHECK(out.size() >= 4);
	int total = 0;
	for (const IslandComponent &c : out) {
		total += c.cell_count();
		CHECK(c.cell_count() <= cfg.max_cells);
	}
	CHECK(total == 216);
}

TEST_CASE("the split plane is the weakest seam, not the midpoint") {
	const FloodWindow w = window16();
	// A dumbbell along x: two 3x3x3 blobs joined by a one-cell neck at x = 6. Splitting at
	// the neck costs one crossing face; splitting anywhere inside a blob costs nine.
	std::vector<IVec3> cells;
	for (int z = 4; z <= 6; z++)
		for (int y = 4; y <= 6; y++) {
			for (int x = 3; x <= 5; x++) cells.push_back({x, y, z});
			for (int x = 7; x <= 9; x++) cells.push_back({x, y, z});
		}
	cells.push_back({6, 5, 5});
	const FloodResult r = floating(w, cells);

	ComponentConfig cfg;
	cfg.max_extent_cells = 6; // the dumbbell is 7 cells long, so it must split once
	std::vector<IslandComponent> out;
	label_islands(r, cfg, &out);
	REQUIRE(out.size() == 2);
	// Each side keeps its whole blob: a midpoint split would have cut a blob in half.
	CHECK(std::max(out[0].cell_count(), out[1].cell_count()) == 28);
	CHECK(std::min(out[0].cell_count(), out[1].cell_count()) == 27);
}

TEST_CASE("a split half that is internally disconnected is re-labelled into connected pieces") {
	const FloodWindow w = window16();
	// Two x-bars separated in z touch each other only through the x = 4 bridge. The whole
	// component is 6-connected, but when the longest axis (x, extent 5) is split at its
	// weakest seam, the x < 2 half holds two bars that are not face-adjacent to each other.
	std::vector<IVec3> cells;
	for (int x = 0; x <= 3; x++) {
		cells.push_back({x, 0, 0});
		cells.push_back({x, 0, 2});
	}
	for (int z = 0; z <= 2; z++) cells.push_back({4, 0, z});
	const FloodResult r = floating(w, cells);

	ComponentConfig cfg;
	cfg.max_extent_cells = 4;
	std::vector<IslandComponent> out;
	label_islands(r, cfg, &out);
	REQUIRE(out.size() == 3);

	int total = 0;
	for (const IslandComponent &c : out) {
		total += c.cell_count();
		CHECK(component_is_connected(c));
	}
	CHECK(total == 11);
}

TEST_CASE("world AABB is the cell AABB in metres, half-open on the high side") {
	const FloodWindow w = window16();
	const FloodResult r = floating(w, {{2, 3, 4}});
	std::vector<IslandComponent> out;
	label_islands(r, ComponentConfig{}, &out);
	REQUIRE(out.size() == 1);
	float lo[3], hi[3];
	out[0].world_aabb(lo, hi);
	CHECK(lo[0] == doctest::Approx(2.0f * kOccupancyCellSize));
	CHECK(lo[1] == doctest::Approx(3.0f * kOccupancyCellSize));
	CHECK(hi[2] == doctest::Approx(5.0f * kOccupancyCellSize));
}
