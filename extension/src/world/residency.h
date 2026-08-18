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

// What the atlas can pay for this frame, and what each resident region would give back.
//
// It exists because "which region is furthest" and "which region holds bricks" are almost
// unrelated: only the region layers the surface crosses hold any atlas slots at all, and in a
// 96 m residency ball most residents are pure air. Displacing the furthest resident to fund a
// stream-in therefore used to return nothing at all about 70% of the time, so the free list
// slid to zero and brick_mark.comp.glsl's fail-soft dropped the bricks the load had just
// activated — a hole in freshly streamed ground until the repair sweep came back for it.
struct AtlasBudget {
	// Atlas slots held by each REGION slot (index by RegionResidency::slot_of), as reported
	// by the mark pass. Null means "unknown": update() then falls back to the old behaviour
	// of assuming one eviction funds one load.
	const int *cost_by_slot = nullptr;
	int slot_count = 0;
	// Slots the streamer is willing to spend this frame (free list minus its reserve).
	int available = 0;
	// What one stream-in is assumed to cost. Deliberately the high end of what regions
	// actually cost, so a load is over-funded rather than under-funded.
	int per_load = 3072;
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

	// The region pool and the brick pool are sized independently, and at the shipping radius
	// the brick pool is the binding one: the surface shell of a 96 m ball wants ~140k bricks
	// against a 65536-slot atlas. So a load may only proceed once the atlas can PAY for it:
	// while `budget.available` is short of `budget.per_load`, the furthest residents are
	// released — one after another, crediting what each actually holds — until the load is
	// funded. Air regions cost nothing and so fund nothing; the loop simply walks past them
	// to the furthest region that does hold bricks. Nothing nearer than the candidate is ever
	// released, so when the atlas is full the horizon stops arriving instead of the ground
	// under the player going hollow.
	//
	// `max_loads` overrides ResidencyConfig::max_loads_per_frame downwards for this frame
	// (negative means "use the config").
	// A default budget carries no costs, which means "the atlas is not the binding pool":
	// only a full region-slot pool then forces a displacement, exactly as before.
	ResidencyPlan update(float cx, float cy, float cz, const AtlasBudget &budget = AtlasBudget{},
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
	//
	// With a budget carrying per-slot costs, it keeps evicting until it has actually
	// recovered `want_slots`: the furthest region is usually air and gives back nothing, so
	// evicting exactly one of them is headroom on paper only. Returns true if anything was
	// evicted. Without costs it evicts exactly one region, as it always did.
	bool evict_furthest(float cx, float cy, float cz, ResidencyPlan *plan,
			const std::vector<IVec3> &exclude, const AtlasBudget *budget = nullptr,
			int want_slots = 0);

	// Distance from a point to the region's world AABB; 0 inside.
	static float region_distance(IVec3 region, float cx, float cy, float cz);

	// How far the near field's brick data is actually COMPLETE, as of the last update():
	// the distance to the nearest in-bounds, in-radius region that is not resident, or
	// radius_m when every one of them is. Loads go nearest-first and stop when the atlas
	// cannot pay for the next one, so beyond this radius the raymarcher reads absent bricks
	// as empty space and returns sky. The near/far seam has to sit INSIDE it or the band
	// between belongs to neither field (M5 errata: the LoD design assumed a 0-150 m near
	// field, but the 65536-slot atlas funds barely half of the configured 96 m ball).
	float complete_radius_m() const { return complete_radius_m_; }

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
	int slot_cost(const AtlasBudget &budget, int slot) const;

	ResidencyConfig cfg_;
	std::map<Key, int> by_region_;   // region -> slot
	std::vector<IVec3> slot_region_; // slot -> region (valid where slot_used_)
	std::vector<char> slot_used_;
	std::vector<int> free_slots_;
	float complete_radius_m_ = 0.0f;
};

} // namespace ve
