#include "world/field_source_snapshot.h"

namespace ve {

bool FieldSourceSnapshot::materialize(OverrideStore *override_store, VolumeSet *volume_set) const {
	if (!override_store || !volume_set) return false;
	// Phase 1: validate all entries without mutating.
	std::vector<IVec3> seen_bricks;
	seen_bricks.reserve(overrides.size());
	for (const auto &o : overrides) {
		for (const auto &s : seen_bricks) if (s == o.brick) return false;
		seen_bricks.push_back(o.brick);
		if (!o.data.normal_oct.empty() && o.data.normal_oct.size() != kBrickSdfCount) return false;
	}
	// Insufficient capacity must fail before any mutation: check free slots, not total capacity.
	if (static_cast<int>(overrides.size()) > override_store->capacity() - override_store->used()) return false;
	std::vector<int> seen_slots;
	seen_slots.reserve(volumes.size());
	for (const auto &v : volumes) {
		if (v.slot < 0 || v.slot >= kMaxVolumes) return false;
		for (int s : seen_slots) if (s == v.slot) return false;
		seen_slots.push_back(v.slot);
		if (!v.data.valid()) return false;
	}
	if (static_cast<int>(volumes.size()) > kMaxVolumes - volume_set->live_count()) return false;
	// Phase 2: mutate transactionally with rollback on any failure.
	std::vector<IVec3> acquired_bricks;
	acquired_bricks.reserve(overrides.size());
	std::vector<int> reserved_slots;
	reserved_slots.reserve(volumes.size());
	for (const auto &o : overrides) {
		int slot = override_store->acquire(o.brick);
		if (slot < 0) {
			for (const auto &b : acquired_bricks) override_store->release(b);
			for (int s : reserved_slots) volume_set->release(s);
			return false;
		}
		OverrideBrick *dst = override_store->data(slot);
		if (!dst) {
			for (const auto &b : acquired_bricks) override_store->release(b);
			override_store->release(o.brick);
			for (int s : reserved_slots) volume_set->release(s);
			return false;
		}
		*dst = o.data;
		acquired_bricks.push_back(o.brick);
	}
	for (const auto &v : volumes) {
		if (!volume_set->reserve(v.slot)) {
			for (const auto &b : acquired_bricks) override_store->release(b);
			for (int s : reserved_slots) volume_set->release(s);
			return false;
		}
		reserved_slots.push_back(v.slot);
		if (!volume_set->store(v.slot, v.data)) {
			for (const auto &b : acquired_bricks) override_store->release(b);
			for (int s : reserved_slots) volume_set->release(s);
			return false;
		}
	}
	return true;
}

} // namespace ve
