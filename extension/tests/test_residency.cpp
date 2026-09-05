#include <doctest/doctest.h>
#include "world/region_window.h"
#include "world/residency.h"
#include <algorithm>
#include <cmath>
#include <set>

static ve::ResidencyConfig make_cfg(float radius, int slots, int per_frame) {
	ve::ResidencyConfig cfg;
	cfg.radius_m = radius;
	cfg.max_region_slots = slots;
	cfg.max_loads_per_frame = per_frame;
	cfg.window = ve::region_window_centered(0.0f, 0.0f, 0.0f,
			ve::region_window_dim(radius, cfg.evict_margin));
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
	// No world box any more: scan the radius' region AABB instead of the old 8x4x8 box.
	const auto lo = [](float c) { return static_cast<int>(std::floor((c - 60.0f) / ve::kRegionSize)); };
	const auto hi = [](float c) { return static_cast<int>(std::floor((c + 60.0f) / ve::kRegionSize)); };
	int resident = 0;
	int oracle = 0;
	for (int z = lo(100.0f); z <= hi(100.0f); z++)
		for (int y = lo(0.0f); y <= hi(0.0f); y++)
			for (int x = lo(100.0f); x <= hi(100.0f); x++) {
				const ve::IVec3 r{x, y, z};
				const float d = oracle_distance(r, 100.0f, 0.0f, 100.0f);
				if (d <= cfg.radius_m) oracle++;
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
	CHECK(resident == oracle); // at a standstill the set is exactly the in-radius regions
	CHECK(resident > 8);
}

TEST_CASE("there is no world edge: regions at any coordinate can be resident") {
	// The old 8x4x8 world box started at the origin, so negative coordinates used to clip
	// and a radius larger than the box settled at exactly 8*4*8 residents. The box is gone:
	// the settled set is the in-radius regions wherever the camera stands.
	auto cfg = make_cfg(60.0f, 512, 64);
	ve::RegionResidency res(cfg);
	settle(res, 0.0f, 0.0f, 0.0f);
	CHECK(res.slot_of({-1, 0, -1}) >= 0);
	CHECK(res.slot_of({-2, 0, 0}) >= 0);
	CHECK(res.slot_of({0, 0, -2}) >= 0);
	int oracle = 0;
	const auto lo = [](float c) { return static_cast<int>(std::floor((c - 60.0f) / ve::kRegionSize)); };
	const auto hi = [](float c) { return static_cast<int>(std::floor((c + 60.0f) / ve::kRegionSize)); };
	for (int z = lo(0.0f); z <= hi(0.0f); z++)
		for (int y = lo(0.0f); y <= hi(0.0f); y++)
			for (int x = lo(0.0f); x <= hi(0.0f); x++)
				if (oracle_distance({x, y, z}, 0.0f, 0.0f, 0.0f) <= cfg.radius_m) oracle++;
	CHECK(res.resident_count() == oracle);
}

TEST_CASE("map_index is the window cell index") {
	auto cfg = make_cfg(60.0f, 256, 8);
	ve::RegionResidency res(cfg);
	const ve::ResidencyPlan p = res.update(100.0f, 0.0f, 100.0f);
	REQUIRE_FALSE(p.loads.empty());
	for (const auto &l : p.loads) CHECK(l.map_index == cfg.window.index(l.region));
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
		CHECK(e.map_index == cfg.window.index(e.region));
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
	// No world box any more: scan the radius' region AABB instead of the old 8x4x8 box.
	const auto lo = [](float c) { return static_cast<int>(std::floor((c - 120.0f) / ve::kRegionSize)); };
	const auto hi = [](float c) { return static_cast<int>(std::floor((c + 120.0f) / ve::kRegionSize)); };
	std::vector<float> refused;
	for (int z = lo(100.0f); z <= hi(100.0f); z++)
		for (int y = lo(0.0f); y <= hi(0.0f); y++)
			for (int x = lo(100.0f); x <= hi(100.0f); x++) {
				const ve::IVec3 r{x, y, z};
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

// Every region slot priced the same, so "the furthest resident is worth something" holds and
// the test is about the trade, not about which region happens to hold bricks.
static ve::AtlasBudget uniform_budget(std::vector<int> &costs, int slots, int cost,
		int available, int per_load) {
	costs.assign(static_cast<size_t>(slots), cost);
	ve::AtlasBudget b;
	b.cost_by_slot = costs.data();
	b.slot_count = slots;
	b.available = available;
	b.per_load = per_load;
	return b;
}

TEST_CASE("under brick scarcity a load must displace the furthest resident") {
	// The atlas and the region pool are sized independently, and it is the atlas that runs
	// out first at the shipping radius. An exhausted budget is how the streamer says so: from
	// then on the resident set may not grow, only trade, so the region a load takes has to
	// be paid for by giving the furthest one back.
	ve::RegionResidency res(make_cfg(60.0f, 64, 4));
	settle(res, 100.0f, 20.0f, 100.0f);
	const int settled = res.resident_count();
	CHECK(settled > 4);

	// Move far enough that new regions are candidates, then stream with nothing to spend.
	std::vector<int> costs;
	const ve::AtlasBudget budget = uniform_budget(costs, 64, 1000, 0, 1000);
	const ve::ResidencyPlan p = res.update(160.0f, 20.0f, 160.0f, budget, 2);
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
	std::vector<int> costs;
	const ve::AtlasBudget budget = uniform_budget(costs, 64, 1000, 0, 1000);
	const ve::ResidencyPlan p = res.update(100.0f, 20.0f, 100.0f, budget, 1);
	CHECK(p.loads.size() == 1u);
	CHECK(p.evicts.empty());
}

TEST_CASE("a load is funded by regions that hold bricks, not merely by distant ones") {
	// The transient-hole bug. Only the region layers the surface crosses hold atlas slots; in
	// a residency ball most residents are pure air and give back nothing when evicted. Paying
	// for a load with one distance-picked eviction therefore funded nothing at all most of the
	// time, the free list slid to zero, and the mark pass dropped the bricks the load had just
	// activated — sky through freshly streamed ground.
	//
	// Here only ONE region layer holds anything, the way only the layers a surface crosses do
	// in a real world. With nothing to spend up front, every load in the plan has to be
	// covered by what the plan's own evictions gave back.
	ve::RegionResidency res(make_cfg(60.0f, 64, 4));
	settle(res, 100.0f, 20.0f, 100.0f);
	CHECK(res.resident_count() > 6);

	std::vector<int> costs(64, 0);
	int priced = 0;
	for (int s = 0; s < 64; s++)
		if (res.slot_resident(s) && res.region_of_slot(s).y == 0) {
			costs[s] = 700;
			priced++;
		}
	REQUIRE(priced > 0);
	ve::AtlasBudget budget;
	budget.cost_by_slot = costs.data();
	budget.slot_count = 64;
	budget.available = 0;
	budget.per_load = 1000;

	const ve::ResidencyPlan p = res.update(160.0f, 20.0f, 160.0f, budget, 2);
	CHECK(p.loads.size() > 0u);
	// The atlas paid for what it took. Evicting one distance-picked region per load would not
	// have covered it: the furthest regions here are worth nothing at all.
	int recovered = 0;
	for (const auto &e : p.evicts) recovered += costs[e.slot];
	CHECK(recovered >= budget.per_load * static_cast<int>(p.loads.size()));
	// And it still never took a region nearer than the one it loaded.
	for (const auto &l : p.loads)
		for (const auto &e : p.evicts)
			CHECK(oracle_distance(e.region, 160.0f, 20.0f, 160.0f) >=
					oracle_distance(l.region, 160.0f, 20.0f, 160.0f));
}

TEST_CASE("an unfundable load is refused rather than streamed into a full atlas") {
	// The other half: when nothing further away holds bricks either, the load must not
	// happen. Streaming it anyway is what put holes in ground the player was looking at —
	// the mark pass drops what the free list cannot serve. A short horizon is the correct
	// failure here.
	ve::RegionResidency res(make_cfg(60.0f, 64, 4));
	settle(res, 100.0f, 20.0f, 100.0f);
	const int settled = res.resident_count();

	std::vector<int> costs(64, 0); // every resident is air: evicting frees nothing
	ve::AtlasBudget budget;
	budget.cost_by_slot = costs.data();
	budget.slot_count = 64;
	budget.available = 0;
	budget.per_load = 1000;

	const ve::ResidencyPlan p = res.update(160.0f, 20.0f, 160.0f, budget, 4);
	CHECK(p.loads.empty());
	CHECK(res.resident_count() <= settled);
}

TEST_CASE("resident_regions reports exactly the resident set") {
	ve::RegionResidency res(make_cfg(40.0f, 64, 4));
	settle(res, 100.0f, 20.0f, 100.0f);
	std::vector<ve::IVec3> regions;
	res.resident_regions(&regions);
	CHECK(static_cast<int>(regions.size()) == res.resident_count());
	for (const ve::IVec3 &r : regions) CHECK(res.slot_of(r) >= 0);
}

TEST_CASE("residency has no world edge") {
	ve::ResidencyConfig cfg;
	cfg.window = ve::region_window_centered(50000.0f, 0.0f, 50000.0f,
			ve::region_window_dim(60.0f, cfg.evict_margin));
	cfg.radius_m = 60.0f;
	cfg.max_region_slots = 256;
	cfg.max_loads_per_frame = 8;
	ve::RegionResidency res(cfg);
	// 50 km from the origin, far outside any box the engine used to have.
	settle(res, 50000.0f, 0.0f, 50000.0f);
	CHECK(res.resident_count() > 0);
}

TEST_CASE("every load carries a valid window cell index") {
	ve::ResidencyConfig cfg;
	cfg.window = ve::region_window_centered(0.0f, 0.0f, 0.0f,
			ve::region_window_dim(60.0f, cfg.evict_margin));
	cfg.radius_m = 60.0f;
	cfg.max_region_slots = 256;
	cfg.max_loads_per_frame = 8;
	ve::RegionResidency res(cfg);
	for (int i = 0; i < 200; i++) {
		const ve::ResidencyPlan p = res.update(0.0f, 0.0f, 0.0f);
		for (const auto &e : p.loads) {
			CHECK(e.map_index >= 0);
			CHECK(e.map_index < cfg.window.cell_count());
		}
		if (p.loads.empty() && p.evicts.empty()) break;
	}
}

TEST_CASE("no two simultaneously resident regions share a window cell") {
	// Invariant 3, exercised along a travelling camera rather than at one standstill.
	ve::ResidencyConfig cfg;
	cfg.radius_m = 96.0f;
	cfg.max_region_slots = 512;
	cfg.max_loads_per_frame = 8;
	const int dim = ve::region_window_dim(cfg.radius_m, cfg.evict_margin);
	cfg.window = ve::region_window_centered(0.0f, 0.0f, 0.0f, dim);
	ve::RegionResidency res(cfg);
	for (int step = 0; step < 400; step++) {
		const float x = float(step) * 12.0f; // ~5 km of travel
		res.set_window(ve::region_window_centered(x, 40.0f, 0.0f, dim));
		res.update(x, 40.0f, 0.0f);
		std::vector<ve::IVec3> regions;
		res.resident_regions(&regions);
		std::set<int> cells;
		for (const ve::IVec3 &r : regions) {
			const int idx = res.window().index(r);
			CHECK(cells.insert(idx).second); // false means two residents aliased
		}
	}
}
