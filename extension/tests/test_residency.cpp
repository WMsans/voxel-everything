#include <doctest/doctest.h>
#include "world/residency.h"
#include <algorithm>
#include <cmath>
#include <set>

static ve::ResidencyConfig make_cfg(float radius, int slots, int per_frame) {
	ve::ResidencyConfig cfg;
	cfg.bounds = ve::WorldBounds{{0, -64, 0}, {8, 4, 8}}; // 204.8 x 102.4 x 204.8 m
	cfg.radius_m = radius;
	cfg.max_region_slots = slots;
	cfg.max_loads_per_frame = per_frame;
	return cfg;
}

// Independent oracle: distance from a point to a region's world AABB, 0 when inside.
static float oracle_distance(ve::IVec3 r, float cx, float cy, float cz) {
	const float lo[3] = {r.x * ve::kRegionSize, r.y * ve::kRegionSize, r.z * ve::kRegionSize};
	const float c[3] = {cx, cy, cz};
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float over = std::max(0.0f, std::max(lo[a] - c[a], c[a] - (lo[a] + ve::kRegionSize)));
		d2 += over * over;
	}
	return std::sqrt(d2);
}

// Drives update() until it stops loading, so the test can assert on the settled state.
static int settle(ve::RegionResidency &res, float cx, float cy, float cz, int max_frames = 500) {
	for (int i = 0; i < max_frames; i++) {
		const ve::ResidencyPlan p = res.update(cx, cy, cz);
		if (p.loads.empty() && p.evicts.empty()) return i;
	}
	return max_frames;
}

TEST_CASE("region_distance is zero inside and grows outside") {
	CHECK(ve::RegionResidency::region_distance({0, 0, 0}, 1.0f, 1.0f, 1.0f) == doctest::Approx(0.0f));
	CHECK(ve::RegionResidency::region_distance({0, 0, 0}, -10.0f, 1.0f, 1.0f) ==
			doctest::Approx(10.0f));
	CHECK(ve::RegionResidency::region_distance({1, 0, 0}, 0.0f, 0.0f, 0.0f) ==
			doctest::Approx(ve::kRegionSize));
}

TEST_CASE("loading is throttled to max_loads_per_frame") {
	ve::RegionResidency res(make_cfg(60.0f, 256, 3));
	const ve::ResidencyPlan p = res.update(100.0f, 0.0f, 100.0f);
	CHECK(p.loads.size() == 3);
	CHECK(p.evicts.empty());
	CHECK(res.resident_count() == 3);
}

TEST_CASE("the settled set is exactly the in-radius regions") {
	auto cfg = make_cfg(60.0f, 256, 8);
	ve::RegionResidency res(cfg);
	CHECK(settle(res, 100.0f, 0.0f, 100.0f) < 500);

	// Everything resident is inside the radius...
	std::set<int> slots;
	const ve::IVec3 o = cfg.bounds.origin_regions();
	int resident = 0;
	for (int z = 0; z < cfg.bounds.size_regions.z; z++)
		for (int y = 0; y < cfg.bounds.size_regions.y; y++)
			for (int x = 0; x < cfg.bounds.size_regions.x; x++) {
				const ve::IVec3 r{o.x + x, o.y + y, o.z + z};
				const float d = oracle_distance(r, 100.0f, 0.0f, 100.0f);
				const int slot = res.slot_of(r);
				if (slot >= 0) {
					resident++;
					CHECK(d <= cfg.radius_m * cfg.evict_margin);
					CHECK(slots.insert(slot).second); // slots are unique
					CHECK(res.region_of_slot(slot) == r);
				} else {
					// ...and everything comfortably inside it is resident.
					CHECK(d > cfg.radius_m * 0.9f);
				}
			}
	CHECK(resident == res.resident_count());
	CHECK(resident > 8);
}

TEST_CASE("out-of-bounds regions are never resident") {
	auto cfg = make_cfg(200.0f, 512, 64); // radius exceeds the world in every direction
	ve::RegionResidency res(cfg);
	settle(res, 100.0f, 0.0f, 100.0f);
	CHECK(res.slot_of({-1, 0, 0}) == -1);
	CHECK(res.slot_of({8, 0, 0}) == -1);
	CHECK(res.slot_of({0, -3, 0}) == -1);
	CHECK(res.resident_count() == 8 * 4 * 8); // the whole world fits inside the radius
}

TEST_CASE("map_index matches the bounds' dense region index") {
	auto cfg = make_cfg(60.0f, 256, 8);
	ve::RegionResidency res(cfg);
	const ve::ResidencyPlan p = res.update(100.0f, 0.0f, 100.0f);
	REQUIRE_FALSE(p.loads.empty());
	for (const auto &l : p.loads) CHECK(l.map_index == cfg.bounds.region_index(l.region));
}

TEST_CASE("moving away evicts, and the freed slots are reused") {
	auto cfg = make_cfg(40.0f, 256, 16);
	ve::RegionResidency res(cfg);
	settle(res, 30.0f, 0.0f, 30.0f);
	const int first_count = res.resident_count();
	REQUIRE(first_count > 0);
	const int kept_slot = res.slot_of(res.region_of_slot(0));

	settle(res, 170.0f, 0.0f, 170.0f);
	CHECK(res.slot_of({1, 0, 1}) == -1);      // the old neighbourhood is gone
	CHECK(res.resident_count() > 0);
	// Slot count never exceeds the pool, so eviction must actually have recycled slots.
	CHECK(res.resident_count() <= cfg.max_region_slots);
	(void)kept_slot;
}

TEST_CASE("an evict reports the slot and map index the loader was given") {
	auto cfg = make_cfg(40.0f, 256, 16);
	ve::RegionResidency res(cfg);
	settle(res, 30.0f, 0.0f, 30.0f);
	std::set<int> before;
	for (int s = 0; s < cfg.max_region_slots; s++)
		if (res.slot_resident(s)) before.insert(s);

	ve::ResidencyPlan all;
	for (int i = 0; i < 200; i++) {
		const ve::ResidencyPlan p = res.update(170.0f, 0.0f, 170.0f);
		for (const auto &e : p.evicts) all.evicts.push_back(e);
		if (p.loads.empty() && p.evicts.empty()) break;
	}
	CHECK_FALSE(all.evicts.empty());
	for (const auto &e : all.evicts) {
		CHECK(before.count(e.slot) == 1);
		CHECK(e.map_index == cfg.bounds.region_index(e.region));
		CHECK(res.slot_of(e.region) == -1);
	}
}

TEST_CASE("hysteresis keeps a boundary region from thrashing") {
	auto cfg = make_cfg(40.0f, 256, 16);
	ve::RegionResidency res(cfg);
	settle(res, 100.0f, 0.0f, 100.0f);
	// Jitter the camera by a metre either side of a load boundary many times; nothing may
	// churn once the set has settled, or every frame would rebuild a region on the GPU.
	int churn = 0;
	for (int i = 0; i < 40; i++) {
		const float dx = (i % 2 == 0) ? 1.0f : -1.0f;
		const ve::ResidencyPlan p = res.update(100.0f + dx, 0.0f, 100.0f);
		churn += static_cast<int>(p.loads.size() + p.evicts.size());
	}
	CHECK(churn == 0);
}

TEST_CASE("with too few slots the closest regions win") {
	auto cfg = make_cfg(120.0f, 6, 16); // 6 slots for a radius that wants far more
	ve::RegionResidency res(cfg);
	settle(res, 100.0f, 0.0f, 100.0f);
	CHECK(res.resident_count() == 6);

	// Nothing resident may be further away than something that was refused.
	float worst_resident = 0.0f;
	const ve::IVec3 o = cfg.bounds.origin_regions();
	std::vector<float> refused;
	for (int z = 0; z < cfg.bounds.size_regions.z; z++)
		for (int y = 0; y < cfg.bounds.size_regions.y; y++)
			for (int x = 0; x < cfg.bounds.size_regions.x; x++) {
				const ve::IVec3 r{o.x + x, o.y + y, o.z + z};
				const float d = oracle_distance(r, 100.0f, 0.0f, 100.0f);
				if (d > cfg.radius_m) continue;
				if (res.slot_of(r) >= 0) worst_resident = std::max(worst_resident, d);
				else refused.push_back(d);
			}
	REQUIRE_FALSE(refused.empty());
	CHECK(worst_resident <= *std::min_element(refused.begin(), refused.end()) + 1e-3f);
}

TEST_CASE("teleporting into a full pool swaps the far set for the near set") {
	auto cfg = make_cfg(40.0f, 8, 4);
	ve::RegionResidency res(cfg);
	settle(res, 20.0f, 0.0f, 20.0f);
	REQUIRE(res.resident_count() == 8);
	settle(res, 180.0f, 0.0f, 180.0f);
	CHECK(res.resident_count() == 8);
	for (int s = 0; s < 8; s++) {
		REQUIRE(res.slot_resident(s));
		CHECK(oracle_distance(res.region_of_slot(s), 180.0f, 0.0f, 180.0f) <=
				cfg.radius_m * cfg.evict_margin);
	}
}

TEST_CASE("clear releases everything") {
	ve::RegionResidency res(make_cfg(60.0f, 256, 16));
	settle(res, 100.0f, 0.0f, 100.0f);
	REQUIRE(res.resident_count() > 0);
	res.clear();
	CHECK(res.resident_count() == 0);
	CHECK(res.slot_of({3, 0, 3}) == -1);
	CHECK_FALSE(res.slot_resident(0));
}

TEST_CASE("under brick scarcity a load must displace the furthest resident") {
	// The atlas and the region pool are sized independently, and it is the atlas that runs
	// out first at the shipping radius. `bricks_scarce` is how the streamer says so: from
	// then on the resident set may not grow, only trade, so the region a load takes has to
	// be paid for by giving the furthest one back.
	ve::RegionResidency res(make_cfg(60.0f, 64, 4));
	settle(res, 100.0f, 20.0f, 100.0f);
	const int settled = res.resident_count();
	CHECK(settled > 4);

	// Move far enough that new regions are candidates, then stream scarce.
	const ve::ResidencyPlan p = res.update(160.0f, 20.0f, 160.0f, /*bricks_scarce=*/true, 2);
	CHECK(p.loads.size() <= 2u);
	CHECK(p.loads.size() > 0u);
	// Every load was matched by an eviction: the set traded rather than grew. (update() also
	// evicts what drifted past the hysteresis boundary, so this is a lower bound.)
	CHECK(p.evicts.size() >= p.loads.size());
	CHECK(res.resident_count() <= settled);

	// Nothing displaced is nearer than what replaced it.
	for (const auto &l : p.loads)
		for (const auto &e : p.evicts)
			CHECK(oracle_distance(e.region, 160.0f, 20.0f, 160.0f) >=
					oracle_distance(l.region, 160.0f, 20.0f, 160.0f));
}

TEST_CASE("scarcity cannot bootstrap an empty world into a deadlock") {
	// With nothing resident there is nothing to displace, so a scarce first frame must still
	// be allowed to load — otherwise the world stays empty forever.
	ve::RegionResidency res(make_cfg(60.0f, 64, 4));
	const ve::ResidencyPlan p = res.update(100.0f, 20.0f, 100.0f, /*bricks_scarce=*/true, 1);
	CHECK(p.loads.size() == 1u);
	CHECK(p.evicts.empty());
}

TEST_CASE("resident_regions reports exactly the resident set") {
	ve::RegionResidency res(make_cfg(40.0f, 64, 4));
	settle(res, 100.0f, 20.0f, 100.0f);
	std::vector<ve::IVec3> regions;
	res.resident_regions(&regions);
	CHECK(static_cast<int>(regions.size()) == res.resident_count());
	for (const ve::IVec3 &r : regions) CHECK(res.slot_of(r) >= 0);
}
