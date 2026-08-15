#pragma once
#include "world/region.h"
#include <map>
#include <vector>

namespace ve {

struct ResidencyConfig {
	WorldBounds bounds{};
	float radius_m = 96.0f;
	int max_region_slots = 512;
	int max_loads_per_frame = 4;
	// A region is evicted only past radius_m * evict_margin. Without the gap, a camera
	// resting on the radius boundary would load and evict the same region every frame,
	// each cycle costing a full 32^3 mark pass and a few thousand brick generations.
	float evict_margin = 1.15f;
};

struct ResidencyPlan {
	struct Entry {
		IVec3 region;
		int slot = -1;
		int map_index = -1; // WorldBounds::region_index(region)
	};
	std::vector<Entry> loads;
	std::vector<Entry> evicts;
};

// Which regions are resident, and in which region-table slot. Distance-LRU: when the pool is
// full, a closer candidate displaces the furthest resident (spec §2).
class RegionResidency {
public:
	explicit RegionResidency(const ResidencyConfig &cfg);

	ResidencyPlan update(float cx, float cy, float cz);

	int slot_of(IVec3 region) const;
	bool slot_resident(int slot) const;
	IVec3 region_of_slot(int slot) const { return slot_region_[slot]; }
	int resident_count() const { return static_cast<int>(by_region_.size()); }
	void clear();
	const ResidencyConfig &config() const { return cfg_; }

	// Plan-evict the furthest resident region whose coords are not in `exclude`, so an
	// SDF-changing edit can force atlas headroom (spec §8 "evicts or drops": the drop arm
	// alone leaves the edit's new bricks absent and the hole invisible). Returns false when
	// every resident region is excluded — the caller then falls back to the drop.
	bool evict_furthest(float cx, float cy, float cz, ResidencyPlan *plan,
			const std::vector<IVec3> &exclude);

	// Distance from a point to the region's world AABB; 0 inside.
	static float region_distance(IVec3 region, float cx, float cy, float cz);

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	static Key key(IVec3 r) { return Key{r.x, r.y, r.z}; }
	void release(IVec3 region, int slot, ResidencyPlan *plan);

	ResidencyConfig cfg_;
	std::map<Key, int> by_region_;   // region -> slot
	std::vector<IVec3> slot_region_; // slot -> region (valid where slot_used_)
	std::vector<char> slot_used_;
	std::vector<int> free_slots_;
};

} // namespace ve
