#include "world/region_archive.h"

namespace ve {

void PinnedRegionArchive::store(RegionSnapshot &&s) {
	const Key k{s.region.x, s.region.y, s.region.z};
	by_region_[k] = std::move(s);
}

bool PinnedRegionArchive::load(IVec3 region, RegionSnapshot *out) {
	if (!out) return false;
	const auto it = by_region_.find(Key{region.x, region.y, region.z});
	if (it == by_region_.end()) return false;
	*out = it->second;
	return true;
}

} // namespace ve
