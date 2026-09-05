#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <map>
#include <vector>

namespace ve {

// Everything about one region that is NOT recomputable from the terrain pipeline. Occupancy is
// absent on purpose: the mark pass regenerates it, so it is dropped rather than archived.
struct RegionSnapshot {
	IVec3 region{};
	std::vector<EditOp> ops;
	std::vector<uint64_t> seqs; // parallel to ops, the global append sequence of each
	int override_table = -1;
};

// Where a region's edits go when they leave RAM.
//
// A pins everything (PinnedRegionArchive), which is exactly the behaviour the bounded world
// had: edits are bounded by how much the player has dug, not by how far they have walked. The
// seam exists so sub-project C can page to disk without touching a caller.
//
// CONSTRAINT FOR C: LodSystem::gather_ops reads a region's ops SYNCHRONOUSLY on the far-field
// build path. Against the pinned archive that is a map lookup. Against a disk archive it is
// asynchronous IO, so a LoD build for a region with archived edits must be able to WAIT rather
// than silently building pre-edit terrain. Design that in before the disk backend, not after.
struct RegionArchive {
	virtual ~RegionArchive() = default;
	virtual void store(RegionSnapshot &&s) = 0;
	// False means the region was never edited -- regenerate it from the pipeline. It does NOT
	// mean "edited, but the edits were empty".
	virtual bool load(IVec3 region, RegionSnapshot *out) = 0;
};

class PinnedRegionArchive : public RegionArchive {
public:
	void store(RegionSnapshot &&s) override;
	bool load(IVec3 region, RegionSnapshot *out) override;
	int size() const { return static_cast<int>(by_region_.size()); }

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	std::map<Key, RegionSnapshot> by_region_;
};

} // namespace ve
