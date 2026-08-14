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

ResidencyPlan RegionResidency::update(float cx, float cy, float cz) {
	ResidencyPlan plan;

	// 1. Evict anything that has drifted past the hysteresis boundary.
	{
		std::vector<std::pair<IVec3, int>> gone;
		for (const auto &kv : by_region_) {
			const IVec3 r{kv.first.x, kv.first.y, kv.first.z};
			if (region_distance(r, cx, cy, cz) > cfg_.radius_m * cfg_.evict_margin)
				gone.emplace_back(r, kv.second);
		}
		for (const auto &g : gone) release(g.first, g.second, &plan);
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

	// 3. Load the nearest candidates, displacing the furthest residents when the pool is full.
	for (const Cand &c : cands) {
		if (static_cast<int>(plan.loads.size()) >= cfg_.max_loads_per_frame) break;
		if (free_slots_.empty()) {
			IVec3 worst{};
			int worst_slot = -1;
			float worst_dist = c.dist;
			for (const auto &kv : by_region_) {
				const IVec3 r{kv.first.x, kv.first.y, kv.first.z};
				const float d = region_distance(r, cx, cy, cz);
				if (d > worst_dist) { worst_dist = d; worst_slot = kv.second; worst = r; }
			}
			if (worst_slot < 0) break; // every resident is closer than every candidate left
			release(worst, worst_slot, &plan);
		}
		const int slot = free_slots_.back();
		free_slots_.pop_back();
		slot_used_[slot] = 1;
		slot_region_[slot] = c.region;
		by_region_[key(c.region)] = slot;
		plan.loads.push_back({c.region, slot, cfg_.bounds.region_index(c.region)});
	}
	return plan;
}

} // namespace ve
