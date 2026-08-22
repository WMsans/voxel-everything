#pragma once
#include "generator/volume_set.h"
#include "world/override_store.h"
#include "world/region.h"
#include <vector>

namespace ve {

struct LocatedOverride {
	IVec3 brick{};
	OverrideBrick data{};
};

struct SlottedVolume {
	int slot = -1;
	VolumeData data{};
};

struct FieldSourceSnapshot {
	std::vector<LocatedOverride> overrides;
	std::vector<SlottedVolume> volumes;
	bool materialize(OverrideStore *override_store, VolumeSet *volume_set) const;
};

} // namespace ve
