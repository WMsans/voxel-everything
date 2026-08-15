#pragma once
#include "connectivity/flood_fill.h"
#include <vector>

namespace ve {

// A component may be no wider than the island volume that will hold it. An island volume is
// kIslandDim (64) samples with kIslandMarginVoxels (2) of clearance at each end, so its
// usable reach at the coarse cell pitch is 7 × 0.8 m = 5.6 m -- seven 0.8 m cells. Wider
// components are split, which is spec §5's "oversized components split along weakest box
// seams". Keep this in sync with the island volume dimensions used by the generator.
inline constexpr int kMaxIslandExtentCells = 7;

struct ComponentConfig {
	int max_extent_cells = kMaxIslandExtentCells;
	// Also a guardrail on the box merge: kMaxIslandBoxes boxes cannot cover an arbitrary
	// 512-cell blob, and a component that needs more boxes than that is better split than
	// dropped. 512 cells is 262 m^3 of bounding volume -- far past anything the demo tools
	// can free in one shot.
	int max_cells = 512;
};

// One connected group of solid, unanchored cells: spec §5's "each group = one island".
struct IslandComponent {
	std::vector<IVec3> cells; // window index order; every cell is 0.8 m
	IVec3 lo{}, hi{};         // inclusive cell AABB

	int cell_count() const { return static_cast<int>(cells.size()); }
	int extent_cells(int axis) const;
	// The component's bounding box in metres. Half-open on the high side, because cell c
	// spans [c * 0.8, (c + 1) * 0.8): hi[a] is the far FACE of cell hi, not its origin.
	void world_aabb(float lo_m[3], float hi_m[3]) const;
};

// Six-connected labelling of every solid cell the flood left unanchored, then splitting.
// Output order is stable: components come out globally ordered by their lowest window index
// (the first cell in `cells`, which is kept in window-index order). A split component's
// pieces appear in the order of their own lowest indices, but later components with smaller
// lowest indices may interleave between them.
//
// Splitting recursively cuts the offending component with an axis-aligned plane, choosing
// the axis by longest extent and the plane by FEWEST CROSSING FACES among the candidate
// positions -- the "weakest box seam". Each half is then 6-connectivity labelled and each
// connected subgroup is emitted as its own component, so a partition never emits a
// disconnected piece. Recursion stops when a piece fits, and a piece that cannot be reduced
// further (a single cell) is emitted as it is.
void label_islands(const FloodResult &r, const ComponentConfig &cfg,
		std::vector<IslandComponent> *out);

} // namespace ve
