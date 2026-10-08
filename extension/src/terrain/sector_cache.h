#pragma once
// The host copy of every resident sector (spec §5). The render device's bake ring inserts
// read-back bytes; every SectorMirror uploads from here; the CPU field samples here. One
// shared_mutex guards the maps, and every entry is an immutable shared_ptr, so a reader that
// holds one is never left with freed texels by an eviction.
//
// World-space in (cameras, rectangles), field-space out (SectorCoord): the cache adds the
// world seed's offset itself, so callers never mix the two spaces.
#include "terrain/sector.h"
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace ve {

class SectorCache {
public:
	// radius_m: sectors within this distance of the camera are wanted. One more sector of
	// distance is kept before eviction, so a camera on a boundary does not thrash.
	SectorCache(float radius_m, float offset_x, float offset_z);

	struct Plan {
		std::vector<SectorCoord> bake;    // in flight from now on, nearest first
		std::vector<SectorCoord> evicted; // dropped this call
	};
	// Render thread, once per frame, before anything evaluates the field.
	Plan plan(float cam_x, float cam_z, int max_bakes);
	void insert(SectorCoord c, std::shared_ptr<const SectorTexels> t);
	void abandon(SectorCoord c); // a bake that will not arrive; plan() may ask again
	void clear();                // drop everything, resident and in flight

	std::shared_ptr<const SectorTexels> find(SectorCoord c) const;
	// The wanted sectors (within the radius of the last plan's camera) that overlap the
	// world-space rectangle. Sectors beyond the radius are never baked, so nothing waits on
	// them (plan deviation 10).
	std::vector<SectorCoord> needed_world(float min_x, float min_z, float max_x, float max_z) const;
	bool ready_world(float min_x, float min_z, float max_x, float max_z) const;

	SectorCoord centre() const;
	uint64_t version() const; // bumps on every insert, eviction and clear
	std::vector<std::pair<SectorCoord, std::shared_ptr<const SectorTexels>>> snapshot() const;
	int max_resident() const;
	float radius_m() const { return radius_m_; } // fixed at construction (plan deviation 14)
	float offset_x() const { return offset_x_; }
	float offset_z() const { return offset_z_; }

	struct Stats {
		int resident = 0;
		int in_flight = 0;
		int64_t inserted = 0;
		float max_slope = 0.0f;
		int over_limit = 0;   // inserted sectors steeper than kSectorSlopeLimit
		float r_min = 1.0f;   // the encoded range actually used, over every insert
		float r_max = 0.0f;
	};
	Stats stats() const;

private:
	std::vector<SectorCoord> needed_locked(float min_x, float min_z, float max_x, float max_z) const;

	float radius_m_, offset_x_, offset_z_;
	mutable std::shared_mutex mu_;
	std::map<SectorCoord, std::shared_ptr<const SectorTexels>> resident_;
	std::set<SectorCoord> in_flight_;
	float cam_fx_ = 0.0f, cam_fz_ = 0.0f; // field-space camera of the last plan
	SectorCoord centre_{};
	uint64_t version_ = 0;
	Stats stats_{};
};

// Mirror of sector_ground() in shaders/sector.glslh. Field-space x, z. Height is metres above
// kSurfaceY, gradient in metres per metre, ridge in -1..1. Where no sector is resident (or
// there is no cache) the height is the bottom of the encoded range: open air above the
// valley floor, never a false solid (spec §5.4).
struct SectorGround {
	float height = 0.0f, dhdx = 0.0f, dhdz = 0.0f, ridge = 0.0f;
};
SectorGround sector_ground(const SectorCache *cache, float water_y, float x, float z);

} // namespace ve
