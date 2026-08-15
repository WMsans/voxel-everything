#include "world/residency.h"
#include <algorithm>
#include <cmath>

namespace ve {

RegionResidency::RegionResidency(const ResidencyConfig &cfg)
	: cfg_(cfg), slot_region_(static_cast<size_t>(cfg.max_region_slots)),
	  slot_used_(static_cast<size_t>(cfg.max_region_slots), 0) {
	free_slots_.reserve(static_cast<size_t>(cfg.max_region_slots));
	// Descending, so pop_back hands out slot 0 first: stable, readable slot numbers in tests
	// and in the debug HUD.
	for (int s = cfg.max_region_slots - 1; s >= 0; s--) free_slots_.push_back(s);
}

float RegionResidency::region_distance(IVec3 region, float cx, float cy, float cz) {
	const float lo[3] = {region.x * kRegionSize, region.y * kRegionSize, region.z * kRegionSize};
	const float c[3] = {cx, cy, cz};
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float over = std::max(0.0f, std::max(lo[a] - c[a], c[a] - (lo[a] + kRegionSize)));
		d2 += over * over;
	}
	return std::sqrt(d2);
}

int RegionResidency::slot_of(IVec3 region) const {
	const auto it = by_region_.find(key(region));
	return it == by_region_.end() ? -1 : it->second;
}

bool RegionResidency::slot_resident(int slot) const {
	return slot >= 0 && slot < cfg_.max_region_slots && slot_used_[slot] != 0;
}

void RegionResidency::resident_regions(std::vector<IVec3> *out) const {
	out->clear();
	out->reserve(by_region_.size());
	for (const auto &kv : by_region_) out->push_back(IVec3{kv.first.x, kv.first.y, kv.first.z});
}

void RegionResidency::clear() {
	by_region_.clear();
	std::fill(slot_used_.begin(), slot_used_.end(), 0);
	free_slots_.clear();
	for (int s = cfg_.max_region_slots - 1; s >= 0; s--) free_slots_.push_back(s);
}

void RegionResidency::release(IVec3 region, int slot, ResidencyPlan *plan) {
	by_region_.erase(key(region));
	slot_used_[slot] = 0;
	free_slots_.push_back(slot);
	plan->evicts.push_back({region, slot, cfg_.bounds.region_index(region)});
}

int RegionResidency::slot_cost(const AtlasBudget &budget, int slot) const {
	if (!budget.cost_by_slot || slot < 0 || slot >= budget.slot_count) return 0;
	const int c = budget.cost_by_slot[slot];
	return c > 0 ? c : 0;
}

ResidencyPlan RegionResidency::update(float cx, float cy, float cz, const AtlasBudget &budget,
		int max_loads) {
	ResidencyPlan plan;
	const int load_cap = max_loads < 0 ? cfg_.max_loads_per_frame
									   : std::min(max_loads, cfg_.max_loads_per_frame);
	// Slots this frame may still spend. Evictions credit it back what they actually held.
	int available = budget.available;

	// 1. Evict anything that has drifted past the hysteresis boundary. What they held is
	//    credited to this frame's budget: a moving camera is funded mostly by the ground it
	//    is leaving behind, and these releases are in the same compute list, ahead of the
	//    marks that will claim the slots back.
	{
		std::vector<std::pair<IVec3, int>> gone;
		for (const auto &kv : by_region_) {
			const IVec3 r{kv.first.x, kv.first.y, kv.first.z};
			if (region_distance(r, cx, cy, cz) > cfg_.radius_m * cfg_.evict_margin)
				gone.emplace_back(r, kv.second);
		}
		for (const auto &g : gone) {
			available += slot_cost(budget, g.second);
			release(g.first, g.second, &plan);
		}
	}

	// 2. Collect in-bounds, in-radius candidates that are not resident yet. The scan is over
	//    the radius' region AABB, not the whole world: at the shipping radius that is ~500
	//    cells, and the world holds a million.
	const IVec3 o = cfg_.bounds.origin_regions();
	const IVec3 sz = cfg_.bounds.size_regions;
	const auto span = [](float lo, float hi) {
		return std::make_pair(static_cast<int>(std::floor(lo / kRegionSize)),
				static_cast<int>(std::floor(hi / kRegionSize)));
	};
	const auto rx = span(cx - cfg_.radius_m, cx + cfg_.radius_m);
	const auto ry = span(cy - cfg_.radius_m, cy + cfg_.radius_m);
	const auto rz = span(cz - cfg_.radius_m, cz + cfg_.radius_m);

	struct Cand { float dist; IVec3 region; };
	std::vector<Cand> cands;
	for (int z = std::max(rz.first, o.z); z <= std::min(rz.second, o.z + sz.z - 1); z++)
		for (int y = std::max(ry.first, o.y); y <= std::min(ry.second, o.y + sz.y - 1); y++)
			for (int x = std::max(rx.first, o.x); x <= std::min(rx.second, o.x + sz.x - 1); x++) {
				const IVec3 r{x, y, z};
				const float d = region_distance(r, cx, cy, cz);
				if (d > cfg_.radius_m) continue;
				if (slot_of(r) >= 0) continue;
				cands.push_back({d, r});
			}
	// Nearest first; the coordinate tie-break keeps the plan deterministic frame to frame.
	std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
		if (a.dist != b.dist) return a.dist < b.dist;
		if (a.region.z != b.region.z) return a.region.z < b.region.z;
		if (a.region.y != b.region.y) return a.region.y < b.region.y;
		return a.region.x < b.region.x;
	});

	// 3. Load the nearest candidates, releasing the furthest residents until the atlas can
	//    pay for each load. The releases go into the same frame's compute list BEFORE the
	//    load marks that claim slots, so what a release gives back is available to the load
	//    immediately. Bootstrapping is exempt: with nothing resident there is nothing to
	//    release, so honouring the budget on the first frame would leave the world empty.
	// Without per-slot costs there is nothing to fund a load out of, so the atlas half of the
	// gate is skipped entirely and only the region-slot pool forces a displacement — exactly
	// what this loop did before the mark pass began reporting what each region holds.
	const bool priced = budget.cost_by_slot != nullptr;
	for (const Cand &c : cands) {
		if (static_cast<int>(plan.loads.size()) >= load_cap) break;
		// Re-tested per candidate, so the exemption covers exactly the one load that gets the
		// world started. Granting it to the whole frame let four regions into an atlas that
		// could hold two, and the mark pass dropped the difference on frame zero.
		const bool bootstrap = by_region_.empty();
		while (!bootstrap) {
			const bool need_region_slot = free_slots_.empty();
			const bool need_slots = priced && available < budget.per_load;
			if (!need_region_slot && !need_slots) break;
			// When only the ATLAS is short, only a region that holds bricks can pay for the
			// load. Air regions cost the atlas nothing to keep, so releasing them recovers
			// nothing — walking the whole air tail would give a load no more to spend and
			// would shorten the horizon for free. A region slot, on the other hand, any
			// resident can give back.
			const bool payers_only = need_slots && !need_region_slot;
			IVec3 worst{};
			int worst_slot = -1;
			float worst_dist = c.dist;
			for (const auto &kv : by_region_) {
				if (payers_only && slot_cost(budget, kv.second) <= 0) continue;
				const IVec3 r{kv.first.x, kv.first.y, kv.first.z};
				const float d = region_distance(r, cx, cy, cz);
				if (d > worst_dist) { worst_dist = d; worst_slot = kv.second; worst = r; }
			}
			if (worst_slot < 0) break; // nothing further away is able to pay
			available += slot_cost(budget, worst_slot);
			release(worst, worst_slot, &plan);
			if (!priced) break; // one displacement per load, as before
		}
		if (free_slots_.empty()) break; // no region slot left to load into
		// Unfunded: everything further away has already been released and the atlas still
		// cannot pay. Stop — the horizon stays short, which is the whole point: loading
		// anyway is what made the mark pass drop bricks out of freshly streamed ground.
		if (priced && !bootstrap && available < budget.per_load) break;
		const int slot = free_slots_.back();
		free_slots_.pop_back();
		slot_used_[slot] = 1;
		slot_region_[slot] = c.region;
		by_region_[key(c.region)] = slot;
		plan.loads.push_back({c.region, slot, cfg_.bounds.region_index(c.region)});
		available -= budget.per_load;
	}
	return plan;
}

bool RegionResidency::evict_furthest(float cx, float cy, float cz, ResidencyPlan *plan,
		const std::vector<IVec3> &exclude, const AtlasBudget *budget, int want_slots) {
	// Same rule as update()'s funding loop: when the caller wants SLOTS back, only a region
	// holding some can provide them, so the air regions are passed over rather than evicted
	// for nothing.
	const bool payers_only = budget && budget->cost_by_slot && want_slots > 0;
	int recovered = 0;
	bool any = false;
	do {
		IVec3 worst{};
		int worst_slot = -1;
		float worst_d = -1.0f;
		for (const auto &kv : by_region_) {
			if (payers_only && slot_cost(*budget, kv.second) <= 0) continue;
			const IVec3 r{kv.first.x, kv.first.y, kv.first.z};
			bool skip = false;
			for (const IVec3 &e : exclude)
				if (e == r) { skip = true; break; }
			if (skip) continue;
			const float d = region_distance(r, cx, cy, cz);
			if (d > worst_d) { worst_d = d; worst = r; worst_slot = kv.second; }
		}
		if (worst_slot < 0) return any;
		if (budget) recovered += slot_cost(*budget, worst_slot);
		release(worst, worst_slot, plan);
		any = true;
	} while (budget && budget->cost_by_slot && recovered < want_slots);
	return any;
}

} // namespace ve
