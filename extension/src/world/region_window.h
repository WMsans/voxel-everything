#pragma once
#include "world/region.h"

namespace ve {

// The near-field region map's index space. The near field only reaches as far as the brick
// atlas can pay for (RegionResidency), so the map does not need to span the world -- it needs
// to span residency. Deriving the size from residency is also what makes the no-alias
// invariant hold by construction: two regions that collide in the toroidal grid are `dim`
// regions apart, and two simultaneously RESIDENT regions are at most 2 * radius * margin
// apart. Pick dim so the first distance exceeds the second and a collision between two live
// entries is arithmetically impossible -- which is why the window needs no companion
// coordinate buffer, no stale-entry sweep when it moves, and no re-upload on recentre.
int region_window_dim(float radius_m, float evict_margin);

struct RegionWindow {
	IVec3 origin{0, 0, 0}; // minimum corner, in REGIONS
	int dim = 16;          // power of two, edge length in regions

	// Toroidal, and TOTAL: every region coordinate has a cell. Callers that need to know
	// whether the cell actually belongs to this region must ask contains() first.
	int index(IVec3 r) const;
	bool contains(IVec3 r) const;
	int cell_count() const { return dim * dim * dim; }
};

RegionWindow region_window_centered(float cx, float cy, float cz, int dim);

} // namespace ve
