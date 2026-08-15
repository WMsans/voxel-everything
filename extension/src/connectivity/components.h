#pragma once
#include "connectivity/flood_fill.h"
#include <vector>

namespace ve {

// A component may be no wider than the island volume that will hold it. An island volume is
// kIslandDim (64) samples with kIslandMarginVoxels (2) of clearance at each end, so its
// usable reach at the coarse 0.10 m pitch is (64 - 1 - 4) * 0.10 = 5.9 m -- seven 0.8 m
// cells. Wider components are split, which is spec §5's "oversized components split along
// weakest box seams". generator/volume_set.cpp static_asserts the relationship, so the two
// constants cannot drift apart silently.
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
// Output order is stable: components come out ordered by their lowest window index, and a
// split component's pieces immediately follow each other, so a test can name them.
//
// Splitting recursively cuts the offending component with an axis-aligned plane, choosing
// the axis by longest extent and the plane by FEWEST CROSSING FACES among the candidate
// positions -- the "weakest box seam". Recursion stops when both halves fit, and a piece
// that cannot be reduced further (a single cell) is emitted as it is.
void label_islands(const FloodResult &r, const ComponentConfig &cfg,
		std::vector<IslandComponent> *out);

} // namespace ve
