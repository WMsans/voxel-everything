#include "mesh/chunk_residency.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

namespace ve {

ChunkResidency::ChunkResidency(const ChunkResidencyConfig &cfg)
	: cfg_(cfg), slot_chunk_(static_cast<size_t>(cfg.max_chunks)),
	  slot_used_(static_cast<size_t>(cfg.max_chunks), 0),
	  slot_state_(static_cast<size_t>(cfg.max_chunks), static_cast<char>(kNeedsBuild)) {
	free_slots_.reserve(static_cast<size_t>(cfg.max_chunks));
	// Descending, so pop_back hands out slot 0 first: stable slot numbers in tests and HUD.
	for (int s = cfg.max_chunks - 1; s >= 0; s--) free_slots_.push_back(s);
}

int ChunkResidency::slot_of(IVec3 c) const {
	const auto it = by_chunk_.find(key(c));
	return it == by_chunk_.end() ? -1 : it->second;
}

IVec3 ChunkResidency::chunk_of_slot(int slot) const {
	return slot >= 0 && slot < cfg_.max_chunks ? slot_chunk_[slot] : IVec3{};
}

bool ChunkResidency::slot_resident(int slot) const {
	return slot >= 0 && slot < cfg_.max_chunks && slot_used_[slot] != 0;
}

int ChunkResidency::pending_count() const {
	int n = 0;
	for (const auto &kv : by_chunk_)
		if (slot_state_[kv.second] != kReady) n++;
	return n;
}

void ChunkResidency::clear() {
	by_chunk_.clear();
	probe_cache_.clear();
	in_flight_.clear();
	std::fill(slot_used_.begin(), slot_used_.end(), 0);
	std::fill(slot_state_.begin(), slot_state_.end(), static_cast<char>(kNeedsBuild));
	free_slots_.clear();
	for (int s = cfg_.max_chunks - 1; s >= 0; s--) free_slots_.push_back(s);
}

void ChunkResidency::release(IVec3 chunk, int slot, ChunkPlan *plan) {
	if (slot_state_[slot] == kBuilding) in_flight_[key(chunk)] = 1;
	by_chunk_.erase(key(chunk));
	slot_used_[slot] = 0;
	slot_state_[slot] = static_cast<char>(kNeedsBuild);
	free_slots_.push_back(slot);
	if (plan) plan->releases.push_back({chunk, slot});
}

void ChunkResidency::mark_dirty(IVec3 lo, IVec3 hi) {
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 c{x, y, z};
				probe_cache_.erase(key(c));
				const int slot = slot_of(c);
				if (slot >= 0) slot_state_[slot] = static_cast<char>(kNeedsBuild);
			}
}

void ChunkResidency::note_built(IVec3 chunk) {
	const int slot = slot_of(chunk);
	in_flight_.erase(key(chunk));
	// Only when this is still the build we asked for. An edit that landed while the mesher
	// was running has already put the chunk back to kNeedsBuild, and the result in hand is
	// of the pre-edit field: promoting it would leave the crater uncollidable.
	if (slot >= 0 && slot_state_[slot] == kBuilding) slot_state_[slot] = static_cast<char>(kReady);
}

void ChunkResidency::note_failed(IVec3 chunk) {
	const int slot = slot_of(chunk);
	in_flight_.erase(key(chunk));
	if (slot >= 0 && slot_state_[slot] == kBuilding)
		slot_state_[slot] = static_cast<char>(kNeedsBuild);
}

int ChunkResidency::note_empty(IVec3 chunk) {
	// The probe is conservative by construction, so a chunk it passed can still hold no
	// triangles. Caching the empty verdict is what stops it being re-planned every frame for
	// ever; mark_dirty drops the entry, so an edit brings it back.
	const int slot = slot_of(chunk);
	if (slot >= 0 && slot_state_[slot] == kBuilding) {
		probe_cache_[key(chunk)] = 0;
		release(chunk, slot, nullptr);
		in_flight_.erase(key(chunk));
		return slot;
	}
	// If the slot is absent or no longer kBuilding, this result belongs to a stale build:
	// mark_dirty already erased the cache, and a cached empty would hide a later edit.
	if (slot >= 0) release(chunk, slot, nullptr);
	in_flight_.erase(key(chunk));
	return slot;
}

ChunkPlan ChunkResidency::update(const float *centers, const float *radii, int center_count,
		const ChunkProbe &probe, int max_builds) {
	ChunkPlan plan;
	if (!centers || center_count <= 0) return plan;

	const auto radius_of = [&](int i) { return radii ? radii[i] : cfg_.radius_m; };
	const auto nearest = [&](IVec3 c) {
		float best = 1e30f;
		for (int i = 0; i < center_count; i++)
			best = std::min(best, chunk_distance(c, centers[i * 3], centers[i * 3 + 1],
					centers[i * 3 + 2]));
		return best;
	};
	const auto inside = [&](IVec3 c, float scale) {
		for (int i = 0; i < center_count; i++)
			if (chunk_distance(c, centers[i * 3], centers[i * 3 + 1], centers[i * 3 + 2]) <=
					radius_of(i) * scale)
				return true;
		return false;
	};

	// 1. Release what has drifted past the hysteresis boundary.
	{
		std::vector<std::pair<IVec3, int>> gone;
		for (const auto &kv : by_chunk_) {
			const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
			if (!inside(c, cfg_.evict_margin)) gone.emplace_back(c, kv.second);
		}
		for (const auto &g : gone) release(g.first, g.second, &plan);
	}

	// 2. Forget probe verdicts well outside the working set, so a long walk cannot grow the
	//    cache without bound.
	for (auto it = probe_cache_.begin(); it != probe_cache_.end();) {
		const IVec3 c{it->first.x, it->first.y, it->first.z};
		it = inside(c, cfg_.evict_margin * 1.5f) ? std::next(it) : probe_cache_.erase(it);
	}

	// 3. Collect every in-bounds, in-radius, non-resident chunk first, then probe them in
	//    nearest-first order so the per-frame budget always spends itself on the closest chunks.
	struct Cand {
		float dist;
		IVec3 chunk;
	};
	std::vector<Cand> candidates;
	std::set<Key> seen;
	for (int i = 0; i < center_count; i++) {
		const float r = radius_of(i);
		const float cx = centers[i * 3], cy = centers[i * 3 + 1], cz = centers[i * 3 + 2];
		const auto span = [](float lo, float hi) {
			return std::make_pair(static_cast<int>(std::floor(lo / kChunkSize)),
					static_cast<int>(std::floor(hi / kChunkSize)));
		};
		const auto rx = span(cx - r, cx + r);
		const auto ry = span(cy - r, cy + r);
		const auto rz = span(cz - r, cz + r);
		for (int z = rz.first; z <= rz.second; z++)
			for (int y = ry.first; y <= ry.second; y++)
				for (int x = rx.first; x <= rx.second; x++) {
					const IVec3 c{x, y, z};
					// A chunk is 16 bricks and the world is region-aligned (32 bricks), so a
					// chunk is either wholly inside or wholly outside: one corner decides.
					if (!cfg_.bounds.contains_brick(chunk_min_brick(c))) continue;
					if (chunk_distance(c, cx, cy, cz) > r) continue;
					if (!seen.insert(key(c)).second) continue;
					if (slot_of(c) >= 0) continue;
					// A chunk evicted/displaced while its build was in flight must stay out
					// until that result lands; otherwise a stale result can promote the new
					// build or cache empty for a re-added chunk.
					if (in_flight_.count(key(c))) continue;
					candidates.push_back({nearest(c), c});
				}
	}
	std::sort(candidates.begin(), candidates.end(),
			[](const Cand &a, const Cand &b) { return a.dist < b.dist; });

	std::vector<Cand> cands;
	int probes = 0;
	for (const Cand &cand : candidates) {
		auto pc = probe_cache_.find(key(cand.chunk));
		if (pc == probe_cache_.end()) {
			if (probes >= cfg_.max_probes_per_frame) continue; // next frame
			probes++;
			pc = probe_cache_.emplace(key(cand.chunk),
					static_cast<char>(probe.chunk_has_surface(cand.chunk) ? 1 : 0)).first;
		}
		if (pc->second == 0) continue;
		cands.push_back(cand);
	}

	// 4. Make them resident, nearest first. With the pool full, a closer candidate DISPLACES
	//    the furthest resident, so what goes missing under pressure is the far edge of the
	//    ball and never the ground the player is standing on.
	for (const Cand &cand : cands) {
		if (free_slots_.empty()) {
			float worst = cand.dist;
			IVec3 victim{};
			int victim_slot = -1;
			for (const auto &kv : by_chunk_) {
				const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
				const float d = nearest(c);
				if (d > worst) {
					worst = d;
					victim = c;
					victim_slot = kv.second;
				}
			}
			// Candidates are sorted, so if this one cannot displace anything, none can.
			if (victim_slot < 0) break;
			release(victim, victim_slot, &plan);
		}
		const int slot = free_slots_.back();
		free_slots_.pop_back();
		slot_used_[slot] = 1;
		slot_state_[slot] = static_cast<char>(kNeedsBuild);
		slot_chunk_[slot] = cand.chunk;
		by_chunk_[key(cand.chunk)] = slot;
	}

	// 5. Hand out this frame's builds, nearest first. Distance alone is the right priority:
	//    the chunk an edit just dirtied is the one under the player's crosshair.
	const int cap = max_builds < 0 ? cfg_.max_builds_per_frame
								   : std::min(max_builds, cfg_.max_builds_per_frame);
	if (cap > 0) {
		std::vector<Cand> want;
		for (const auto &kv : by_chunk_)
			if (slot_state_[kv.second] == kNeedsBuild) {
				const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
				want.push_back({nearest(c), c});
			}
		std::sort(want.begin(), want.end(),
				[](const Cand &a, const Cand &b) { return a.dist < b.dist; });
		for (int i = 0; i < static_cast<int>(want.size()) && i < cap; i++) {
			const int slot = slot_of(want[i].chunk);
			slot_state_[slot] = static_cast<char>(kBuilding);
			plan.builds.push_back({want[i].chunk, slot});
		}
	}
	return plan;
}

} // namespace ve
