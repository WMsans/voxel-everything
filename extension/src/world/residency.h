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

	// `bricks_scarce` reports that the ATLAS (not the region-slot pool) is nearly spent.
	// The region pool and the brick pool are sized independently, and at the shipping
	// radius the brick pool is the binding one: the surface shell of a 96 m ball wants
	// ~140k bricks against a 65536-slot atlas. Left alone, streaming spends the last slot
	// and every later edit hits the free-list-empty fail-soft in brick_mark.comp.glsl,
	// dropping the bricks it activates for good. Under scarcity a load must therefore
	// DISPLACE the furthest resident instead of taking a fresh region slot, which caps the
	// resident set at what the atlas can hold and keeps a working reserve free. The set
	// stays nearest-first, so what goes missing is the far horizon, never the edit.
	// `max_loads` overrides ResidencyConfig::max_loads_per_frame downwards for this frame
	// (negative means "use the config"); the streamer scales it to the free-slot budget.
	ResidencyPlan update(float cx, float cy, float cz, bool bricks_scarce = false,
			int max_loads = -1);

	int slot_of(IVec3 region) const;
	bool slot_resident(int slot) const;
	IVec3 region_of_slot(int slot) const { return slot_region_[slot]; }
	int resident_count() const { return static_cast<int>(by_region_.size()); }
	void resident_regions(std::vector<IVec3> *out) const;
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
