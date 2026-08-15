#include "connectivity/flood_fill.h"
#include <algorithm>

namespace ve {

namespace {

// The six face neighbours, as (axis, sign) so a step can be turned back into a link name:
// stepping +a from c crosses link (c, a); stepping -a from c crosses link (c - e_a, a).
constexpr int kAxis[6] = {0, 0, 1, 1, 2, 2};
constexpr int kSign[6] = {+1, -1, +1, -1, +1, -1};

IVec3 step(IVec3 c, int d) {
	IVec3 n = c;
	if (kAxis[d] == 0) n.x += kSign[d];
	else if (kAxis[d] == 1) n.y += kSign[d];
	else n.z += kSign[d];
	return n;
}

// The link crossed by stepping direction d out of c, named by its lower cell.
IVec3 link_cell(IVec3 c, int d) {
	return kSign[d] > 0 ? c : step(c, d);
}

} // namespace

bool FloodWindow::contains(IVec3 c) const {
	return c.x >= lo.x && c.x < lo.x + dim && c.y >= lo.y && c.y < lo.y + dim &&
			c.z >= lo.z && c.z < lo.z + dim;
}

int FloodWindow::index(IVec3 c) const {
	if (!contains(c)) return -1;
	return (c.x - lo.x) + (c.y - lo.y) * dim + (c.z - lo.z) * dim * dim;
}

IVec3 FloodWindow::cell_of(int i) const {
	return {lo.x + i % dim, lo.y + (i / dim) % dim, lo.z + i / (dim * dim)};
}

bool FloodWindow::on_boundary(IVec3 c) const {
	if (!contains(c)) return false;
	return c.x == lo.x || c.x == lo.x + dim - 1 || c.y == lo.y || c.y == lo.y + dim - 1 ||
			c.z == lo.z || c.z == lo.z + dim - 1;
}

FloodWindow FloodWindow::around(IVec3 lo_cell, IVec3 hi_cell, int dim) {
	FloodWindow w;
	w.dim = std::max(dim, 3); // a 3-cell window is the smallest with an interior at all
	const auto centre = [](int a, int b) { return a + (b - a) / 2; };
	w.lo = {centre(lo_cell.x, hi_cell.x) - w.dim / 2,
			centre(lo_cell.y, hi_cell.y) - w.dim / 2,
			centre(lo_cell.z, hi_cell.z) - w.dim / 2};
	return w;
}

void LinkCuts::add(IVec3 cell, int axis) {
	set_.insert(Key{cell.x, cell.y, cell.z, axis});
}

bool LinkCuts::cut(IVec3 cell, int axis) const {
	return set_.find(Key{cell.x, cell.y, cell.z, axis}) != set_.end();
}

void flood_anchored(const OccupancyGrid &grid, const FloodWindow &w, const LinkCuts *cuts,
		FloodResult *out) {
	out->window = w;
	const int n = w.cells();
	out->solid.assign(static_cast<size_t>(n), 0);
	out->anchored.assign(static_cast<size_t>(n), 0);
	out->solid_count = 0;
	out->anchored_count = 0;
	out->frontier_reached = false;

	// Pass 1: materialise the window. One grid lookup per cell, and the grid lookup is a map
	// find per cell; hoisting it here means the BFS below never touches the map again.
	for (int i = 0; i < n; i++) {
		if (grid.is_solid(w.cell_of(i))) {
			out->solid[i] = 1;
			out->solid_count++;
		}
	}

	// Pass 2: BFS from every solid shell cell. A plain vector used as a stack -- the order
	// does not matter, only reachability, and a stack keeps the working set small.
	std::vector<int> stack;
	stack.reserve(static_cast<size_t>(out->solid_count));
	for (int i = 0; i < n; i++) {
		if (!out->solid[i]) continue;
		const IVec3 c = w.cell_of(i);
		if (!w.on_boundary(c)) continue;
		out->anchored[i] = 1;
		out->anchored_count++;
		stack.push_back(i);
	}
	while (!stack.empty()) {
		const int i = stack.back();
		stack.pop_back();
		const IVec3 c = w.cell_of(i);
		for (int d = 0; d < 6; d++) {
			const IVec3 nc = step(c, d);
			const int ni = w.index(nc);
			if (ni < 0 || !out->solid[ni] || out->anchored[ni]) continue;
			if (cuts && cuts->cut(link_cell(c, d), kAxis[d])) continue;
			out->anchored[ni] = 1;
			out->anchored_count++;
			stack.push_back(ni);
		}
	}

	// Pass 3: does anything unanchored come close enough to the shell that the window may
	// have cut a real support link out of the picture?
	const int m = kFrontierMarginCells;
	for (int i = 0; i < n && !out->frontier_reached; i++) {
		if (!out->solid[i] || out->anchored[i]) continue;
		const IVec3 c = w.cell_of(i);
		const int dx = std::min(c.x - w.lo.x, w.lo.x + w.dim - 1 - c.x);
		const int dy = std::min(c.y - w.lo.y, w.lo.y + w.dim - 1 - c.y);
		const int dz = std::min(c.z - w.lo.z, w.lo.z + w.dim - 1 - c.z);
		if (std::min(dx, std::min(dy, dz)) <= m) out->frontier_reached = true;
	}
}

} // namespace ve
