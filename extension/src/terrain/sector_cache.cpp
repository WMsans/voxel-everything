#include "terrain/sector_cache.h"
#include "generator/generator.h" // kSurfaceY
#include <algorithm>
#include <cmath>
#include <mutex>

namespace ve {

SectorCache::SectorCache(float radius_m, float offset_x, float offset_z)
		: radius_m_(radius_m), offset_x_(offset_x), offset_z_(offset_z) {}

SectorCache::Plan SectorCache::plan(float cam_x, float cam_z, int max_bakes) {
	const float fx = cam_x + offset_x_, fz = cam_z + offset_z_;
	std::unique_lock lock(mu_);
	cam_fx_ = fx;
	cam_fz_ = fz;
	centre_ = sector_of(fx, fz);
	Plan p;
	const float keep = radius_m_ + kSectorSizeM;
	for (auto it = resident_.begin(); it != resident_.end();) {
		if (sector_distance(it->first, fx, fz) > keep) {
			p.evicted.push_back(it->first);
			it = resident_.erase(it);
			version_++;
		} else {
			++it;
		}
	}
	struct Want {
		float d;
		SectorCoord c;
	};
	std::vector<Want> want;
	const int reach = int(std::ceil(radius_m_ / kSectorSizeM)) + 1;
	for (int z = centre_.z - reach; z <= centre_.z + reach; z++)
		for (int x = centre_.x - reach; x <= centre_.x + reach; x++) {
			const SectorCoord c{x, z};
			const float d = sector_distance(c, fx, fz);
			if (d > radius_m_ || resident_.count(c) || in_flight_.count(c)) continue;
			want.push_back({d, c});
		}
	std::sort(want.begin(), want.end(), [](const Want &a, const Want &b) {
		return a.d != b.d ? a.d < b.d : a.c < b.c;
	});
	for (int i = 0; i < int(want.size()) && i < max_bakes; i++) {
		in_flight_.insert(want[size_t(i)].c);
		p.bake.push_back(want[size_t(i)].c);
	}
	return p;
}

void SectorCache::insert(SectorCoord c, std::shared_ptr<const SectorTexels> t) {
	if (!t) return;
	float r_min = 1.0f, r_max = 0.0f;
	for (uint32_t v : t->texels) {
		const float r = float(v & 0xffffu) / 65535.0f;
		r_min = std::min(r_min, r);
		r_max = std::max(r_max, r);
	}
	std::unique_lock lock(mu_);
	in_flight_.erase(c);
	resident_[c] = std::move(t);
	const SectorTexels &in = *resident_[c];
	stats_.inserted++;
	stats_.max_slope = std::max(stats_.max_slope, in.max_slope);
	if (in.max_slope > kSectorSlopeLimit) stats_.over_limit++;
	stats_.r_min = std::min(stats_.r_min, r_min);
	stats_.r_max = std::max(stats_.r_max, r_max);
	version_++;
}

void SectorCache::abandon(SectorCoord c) {
	std::unique_lock lock(mu_);
	in_flight_.erase(c);
}

void SectorCache::clear() {
	std::unique_lock lock(mu_);
	resident_.clear();
	in_flight_.clear();
	version_++;
}

std::shared_ptr<const SectorTexels> SectorCache::find(SectorCoord c) const {
	std::shared_lock lock(mu_);
	const auto it = resident_.find(c);
	return it == resident_.end() ? nullptr : it->second;
}

std::vector<SectorCoord> SectorCache::needed_locked(float min_x, float min_z, float max_x,
		float max_z) const {
	std::vector<SectorCoord> out;
	const SectorCoord lo = sector_of(min_x + offset_x_, min_z + offset_z_);
	const SectorCoord hi = sector_of(max_x + offset_x_, max_z + offset_z_);
	for (int z = lo.z; z <= hi.z; z++)
		for (int x = lo.x; x <= hi.x; x++)
			if (sector_distance({x, z}, cam_fx_, cam_fz_) <= radius_m_) out.push_back({x, z});
	return out;
}

std::vector<SectorCoord> SectorCache::needed_world(float min_x, float min_z, float max_x,
		float max_z) const {
	std::shared_lock lock(mu_);
	return needed_locked(min_x, min_z, max_x, max_z);
}

bool SectorCache::ready_world(float min_x, float min_z, float max_x, float max_z) const {
	std::shared_lock lock(mu_);
	for (const SectorCoord &c : needed_locked(min_x, min_z, max_x, max_z))
		if (!resident_.count(c)) return false;
	return true;
}

SectorCoord SectorCache::centre() const {
	std::shared_lock lock(mu_);
	return centre_;
}

uint64_t SectorCache::version() const {
	std::shared_lock lock(mu_);
	return version_;
}

std::vector<std::pair<SectorCoord, std::shared_ptr<const SectorTexels>>> SectorCache::snapshot() const {
	std::shared_lock lock(mu_);
	return {resident_.begin(), resident_.end()};
}

int SectorCache::max_resident() const {
	// Every sector whose distance to the camera is within radius + one sector (plan()'s
	// eviction distance) has its centre within radius + size + half a diagonal of the camera
	// sector's centre. Counting that disc is a bound for any camera position.
	const float reach = (radius_m_ + kSectorSizeM + kSectorSizeM * 0.7072f) / kSectorSizeM + 0.7072f;
	const int r = int(std::ceil(reach));
	int n = 0;
	for (int z = -r; z <= r; z++)
		for (int x = -r; x <= r; x++)
			if (std::sqrt(float(x * x + z * z)) <= reach) n++;
	return n;
}

SectorCache::Stats SectorCache::stats() const {
	std::shared_lock lock(mu_);
	Stats s = stats_;
	s.resident = int(resident_.size());
	s.in_flight = int(in_flight_.size());
	return s;
}

SectorGround sector_ground(const SectorCache *cache, float water_y, float x, float z) {
	SectorGround g;
	const float lo = water_y - kSectorHeightBelowM - kSurfaceY;
	g.height = lo;
	if (cache == nullptr) return g;
	const SectorCoord c = sector_of(x, z);
	const std::shared_ptr<const SectorTexels> t = cache->find(c);
	if (!t) return g;
	const SectorSample s = sector_bspline(*t, c, x, z);
	g.height = lo + s.r * kSectorHeightSpanM;
	g.dhdx = s.drdx * kSectorHeightSpanM;
	g.dhdz = s.drdz * kSectorHeightSpanM;
	g.ridge = s.g * 2.0f - 1.0f;
	return g;
}

} // namespace ve
