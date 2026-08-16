#pragma once
#include "connectivity/occupancy.h"
#include "world/region.h"
#include <vector>

namespace ve {

// Spec §5 caps an island's collision compound at 256 boxes. M4 uses 64, because the SAME box
// set is also appended to the edit log as one kOpBoxSubtract each: 256 boxes would consume a
// region's entire 256-op budget on a single island, and every op is evaluated by every brick
// and every collision-mesh sample in that region for the rest of the session. 64 is more
// than the demo's tools free in one shot; components that need more are split (Task 3).
inline constexpr int kMaxIslandBoxes = 64;

// An inclusive range of 0.8 m occupancy cells.
struct CellBox {
	IVec3 lo{}, hi{};

	int cells() const {
		return (hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1);
	}
	// Half-open on the high side: cell hi spans up to (hi + 1) * 0.8.
	void world_aabb(float lo_m[3], float hi_m[3]) const;
};

// Greedy box merging: cells are visited in z, y, x order; each unconsumed cell grows as far
// as it can along +x, then the whole row grows along +y, then the whole slab along +z. The
// result TILES the input -- every cell is covered exactly once and nothing outside it is --
// which is what lets one box set serve as both the Jolt compound and the CSG carve.
//
// Returns false (and leaves `out` empty) when the merge would need more than max_boxes; the
// caller splits the component and tries again rather than shipping a partial carve.
bool greedy_box_merge(const std::vector<IVec3> &cells, int max_boxes,
		std::vector<CellBox> *out);

} // namespace ve
