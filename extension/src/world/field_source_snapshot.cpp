#include "world/field_source_snapshot.h"

namespace ve {

bool FieldSourceSnapshot::materialize(OverrideStore *override_store, VolumeSet *volume_set) const {
	if (!override_store || !volume_set) return false;
	// Validate overrides: duplicate brick, malformed normal payload, insufficient capacity
	std::vector<IVec3> seen_bricks;
	seen_bricks.reserve(overrides.size());
	for (const auto &o : overrides) {
		for (const auto &s : seen_bricks) if (s == o.brick) return false;
		seen_bricks.push_back(o.brick);
		if (!o.data.normal_oct.empty() && o.data.normal_oct.size() != kBrickSdfCount) return false;
	}
	if (static_cast<int>(overrides.size()) > override_store->capacity()) return false;
	// Validate volumes: duplicate slot, invalid slot, malformed payload
	std::vector<int> seen_slots;
	seen_slots.reserve(volumes.size());
	for (const auto &v : volumes) {
		if (v.slot < 0 || v.slot >= kMaxVolumes) return false;
		for (int s : seen_slots) if (s == v.slot) return false;
		seen_slots.push_back(v.slot);
		if (!v.data.valid()) return false;
	}
	// Mutate
	for (const auto &o : overrides) {
		int slot = override_store->acquire(o.brick);
		if (slot < 0) return false;
		OverrideBrick *dst = override_store->data(slot);
		if (!dst) return false;
		*dst = o.data;
	}
	for (const auto &v : volumes) {
		if (!volume_set->reserve(v.slot)) return false;
		if (!volume_set->store(v.slot, v.data)) return false;
	}
	return true;
}

} // namespace ve
