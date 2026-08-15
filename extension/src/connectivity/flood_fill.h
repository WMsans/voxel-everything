#pragma once
#include "connectivity/occupancy.h"
#include <cstdint>
#include <set>
#include <vector>

namespace ve {

// Spec §5: "localized flood fill from the window boundary (64^3 cells ~ 51 m, expanding if
// the frontier is reached)".
inline constexpr int kFloodWindowCells = 64;
// One expansion, doubling to 128^3 = 102.4 m. Two would be 204.8 m and 16.7 M cells, which
// is a 16 MB working set for a case the demo cannot produce: the guardrails in Task 3 split
// any component wider than 6.4 m long before a window that size could be justified.
inline constexpr int kMaxWindowExpansions = 1;
// An unanchored cell this close to the shell means the piece may continue past the window,
// where cells we never looked at could be holding it up. Two cells is 1.6 m.
inline constexpr int kFrontierMarginCells = 2;

// A cube of cells the fill works over. Cells are GLOBAL (a cell coordinate is a brick
// coordinate); `lo` is the minimum corner, inclusive, and the window spans `dim` cells on
// each axis. Window-local indices run x fastest, then y, then z.
struct FloodWindow {
	IVec3 lo{};
	int dim = kFloodWindowCells;

	int cells() const { return dim * dim * dim; }
	bool contains(IVec3 c) const;
	int index(IVec3 c) const;    // -1 when outside
	IVec3 cell_of(int index) const;
	bool on_boundary(IVec3 c) const; // in the outermost cell layer (the anchor shell)

	// The smallest `dim`-wide window centred on the inclusive cell AABB [lo_cell, hi_cell].
	// The caller picks dim; if the AABB does not fit, the window is still centred and the
	// fill will raise frontier_reached.
	static FloodWindow around(IVec3 lo_cell, IVec3 hi_cell, int dim);
};

// Face links the thin-contact refinement (Task 4) has severed. A link is named by its LOWER
// cell and an axis (0 = x, 1 = y, 2 = z): link (c, a) joins c and c + e_a, so every face in
// the grid has exactly one name and a cut set cannot alias.
class LinkCuts {
public:
	void add(IVec3 cell, int axis);
	bool cut(IVec3 cell, int axis) const;
	int size() const { return static_cast<int>(set_.size()); }
	void clear() { set_.clear(); }

private:
	struct Key {
		int x, y, z, a;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			if (x != o.x) return x < o.x;
			return a < o.a;
		}
	};
	std::set<Key> set_;
};

struct FloodResult {
	FloodWindow window{};
	std::vector<uint8_t> solid;    // 1 = not known to be air
	std::vector<uint8_t> anchored; // 1 = reachable from the shell through solid faces
	int solid_count = 0;
	int anchored_count = 0;
	// An unanchored solid cell sits within kFrontierMarginCells of the shell: the piece may
	// continue outside, so the caller should widen the window and re-run (Task 13).
	bool frontier_reached = false;

	int cells() const { return window.cells(); }
};

// Six-connected BFS over solid cells, seeded from every solid cell of the window's outermost
// layer. FACE connectivity only: spec §5 is explicit that "edge/corner contact never counts,
// so pieces touching only through cell corners fall rather than wrongly hang".
//
// `cuts` may be null. Cells outside the window are never visited, which is what makes the
// shell the definition of "anchored to the static world".
void flood_anchored(const OccupancyGrid &grid, const FloodWindow &w, const LinkCuts *cuts,
		FloodResult *out);

} // namespace ve
