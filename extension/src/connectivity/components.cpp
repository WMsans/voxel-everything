#include "connectivity/components.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <map>
#include <tuple>

namespace ve {

namespace {

constexpr int kAxis[6] = {0, 0, 1, 1, 2, 2};
constexpr int kSign[6] = {+1, -1, +1, -1, +1, -1};

IVec3 step(IVec3 c, int d) {
	IVec3 n = c;
	if (kAxis[d] == 0) n.x += kSign[d];
	else if (kAxis[d] == 1) n.y += kSign[d];
	else n.z += kSign[d];
	return n;
}

int coord(IVec3 c, int axis) { return axis == 0 ? c.x : (axis == 1 ? c.y : c.z); }

using CellKey = std::tuple<int, int, int>;
using CellSet = std::map<CellKey, char>;

CellKey cell_key(IVec3 v) { return {v.x, v.y, v.z}; }
IVec3 cell_from_key(const CellKey &k) { return {std::get<0>(k), std::get<1>(k), std::get<2>(k)}; }

void recompute_bounds(IslandComponent *c) {
	assert(!c->cells.empty());
	c->lo = c->cells.front();
	c->hi = c->cells.front();
	for (const IVec3 &v : c->cells) {
		c->lo = {std::min(c->lo.x, v.x), std::min(c->lo.y, v.y), std::min(c->lo.z, v.z)};
		c->hi = {std::max(c->hi.x, v.x), std::max(c->hi.y, v.y), std::max(c->hi.z, v.z)};
	}
}

void sort_by_window_index(IslandComponent *c, const FloodWindow &w) {
	std::sort(c->cells.begin(), c->cells.end(), [&](const IVec3 &a, const IVec3 &b) {
		return w.index(a) < w.index(b);
	});
}

// Six-connected labelling of `piece`'s cells. A partition half is not guaranteed to be
// connected on its own, so every connected subgroup is bounded and appended as its own
// IslandComponent.
void push_connected_pieces(const IslandComponent &piece, std::vector<IslandComponent> *work) {
	CellSet remaining;
	for (const IVec3 &v : piece.cells) remaining[cell_key(v)] = 1;

	while (!remaining.empty()) {
		IslandComponent sub;
		const IVec3 start = cell_from_key(remaining.begin()->first);
		std::vector<IVec3> stack;
		remaining.erase(cell_key(start));
		stack.push_back(start);

		while (!stack.empty()) {
			const IVec3 v = stack.back();
			stack.pop_back();
			sub.cells.push_back(v);
			for (int d = 0; d < 6; d++) {
				const IVec3 n = step(v, d);
				const auto it = remaining.find(cell_key(n));
				if (it == remaining.end()) continue;
				remaining.erase(it);
				stack.push_back(n);
			}
		}
		recompute_bounds(&sub);
		work->push_back(sub);
	}
}

bool fits(const IslandComponent &c, const ComponentConfig &cfg) {
	return c.cell_count() <= cfg.max_cells && c.extent_cells(0) <= cfg.max_extent_cells &&
			c.extent_cells(1) <= cfg.max_extent_cells &&
			c.extent_cells(2) <= cfg.max_extent_cells;
}

// The number of face links the plane "axis coordinate < p" would sever. Cheap: one map
// lookup per cell, no adjacency structure.
int seam_cost(const IslandComponent &c, int axis, int p, const CellSet &present) {
	int cost = 0;
	for (const IVec3 &v : c.cells) {
		if (coord(v, axis) != p - 1) continue;
		IVec3 n = v;
		if (axis == 0) n.x++;
		else if (axis == 1) n.y++;
		else n.z++;
		if (present.count({n.x, n.y, n.z})) cost++;
	}
	return cost;
}

// Split `c` into two halves along its longest axis at the cheapest seam. Each half is then
// 6-connectivity labelled and every connected subgroup is appended to `work`, so a half that
// only touched through the seam is not emitted as one disconnected component. A component of
// one cell cannot be split and is emitted as it stands.
void split(const IslandComponent &c, std::vector<IslandComponent> *work) {
	int axis = 0;
	for (int a = 1; a < 3; a++)
		if (c.extent_cells(a) > c.extent_cells(axis)) axis = a;
	const int lo = coord(c.lo, axis), hi = coord(c.hi, axis);
	// Defensive: hi == lo means the piece is one cell thick on every axis, i.e. a single
	// cell, which cannot be split. split() is only called on pieces that fail to fit, so
	// this is reachable only with a zero-sized cap; emit the piece unchanged.
	if (hi == lo) { work->push_back(c); return; }

	CellSet present;
	for (const IVec3 &v : c.cells) present[cell_key(v)] = 1;

	// Candidate planes sit between lo and hi. Ties break towards the middle, so a uniform
	// blob (every seam equally costly) still halves instead of shaving one cell off an end.
	int best = lo + 1, best_cost = -1, best_bias = 0;
	const int mid = lo + (hi - lo + 1) / 2;
	for (int p = lo + 1; p <= hi; p++) {
		const int cost = seam_cost(c, axis, p, present);
		const int bias = std::abs(p - mid);
		if (best_cost < 0 || cost < best_cost || (cost == best_cost && bias < best_bias)) {
			best = p;
			best_cost = cost;
			best_bias = bias;
		}
	}

	IslandComponent a, b;
	for (const IVec3 &v : c.cells) (coord(v, axis) < best ? a : b).cells.push_back(v);
	// Defensive: p is chosen strictly between lo and hi, and c has at least one cell at
	// each of those coordinates, so neither half can be empty; guard anyway.
	if (a.cells.empty() || b.cells.empty()) { work->push_back(c); return; }
	push_connected_pieces(a, work);
	push_connected_pieces(b, work);
}

} // namespace

int IslandComponent::extent_cells(int axis) const {
	return coord(hi, axis) - coord(lo, axis) + 1;
}

void IslandComponent::world_aabb(float lo_m[3], float hi_m[3]) const {
	lo_m[0] = static_cast<float>(lo.x) * kOccupancyCellSize;
	lo_m[1] = static_cast<float>(lo.y) * kOccupancyCellSize;
	lo_m[2] = static_cast<float>(lo.z) * kOccupancyCellSize;
	hi_m[0] = static_cast<float>(hi.x + 1) * kOccupancyCellSize;
	hi_m[1] = static_cast<float>(hi.y + 1) * kOccupancyCellSize;
	hi_m[2] = static_cast<float>(hi.z + 1) * kOccupancyCellSize;
}

void label_islands(const FloodResult &r, const ComponentConfig &cfg,
		std::vector<IslandComponent> *out) {
	out->clear();
	const FloodWindow &w = r.window;
	const int n = w.cells();
	std::vector<uint8_t> seen(static_cast<size_t>(n), 0);
	std::vector<int> stack;

	for (int i = 0; i < n; i++) {
		if (seen[i] || !r.solid[i] || r.anchored[i]) continue;
		IslandComponent c;
		seen[i] = 1;
		stack.push_back(i);
		while (!stack.empty()) {
			const int j = stack.back();
			stack.pop_back();
			const IVec3 cell = w.cell_of(j);
			c.cells.push_back(cell);
			for (int d = 0; d < 6; d++) {
				const int nj = w.index(step(cell, d));
				if (nj < 0 || seen[nj] || !r.solid[nj] || r.anchored[nj]) continue;
				seen[nj] = 1;
				stack.push_back(nj);
			}
		}
		recompute_bounds(&c);

		// Split until every piece fits. Depth is bounded: each split strictly reduces the
		// longest extent or the cell count of both halves.
		std::vector<IslandComponent> work{c};
		std::vector<IslandComponent> pieces;
		while (!work.empty()) {
			IslandComponent piece = work.back();
			work.pop_back();
			if (fits(piece, cfg)) {
				sort_by_window_index(&piece, w);
				pieces.push_back(piece);
				continue;
			}
			const size_t before = work.size();
			split(piece, &work);
			// split() pushes the piece back unchanged when it cannot divide it; emit it
			// rather than looping for ever.
			if (work.size() == before + 1) {
				IslandComponent last = work.back();
				work.pop_back();
				sort_by_window_index(&last, w);
				pieces.push_back(last);
			}
		}
		// The work stack is LIFO and split() stacks the low-coordinate half before the
		// high-coordinate half, so the pieces above come out in reverse window order.
		// Reorder them here to honour the documented output contract.
		std::sort(pieces.begin(), pieces.end(), [&](const IslandComponent &a, const IslandComponent &b) {
			return w.index(a.cells.front()) < w.index(b.cells.front());
		});
		for (const IslandComponent &piece : pieces) out->push_back(piece);
	}
}

} // namespace ve
