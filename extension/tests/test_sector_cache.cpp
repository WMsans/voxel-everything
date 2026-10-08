#include <doctest/doctest.h>
#include "generator/generator.h"
#include "terrain/sector_cache.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace {
std::shared_ptr<const ve::SectorTexels> flat(float r, float slope = 0.0f) {
	auto t = std::make_shared<ve::SectorTexels>();
	const uint32_t v = uint32_t(std::lround(r * 65535.0f)) | (32768u << 16);
	t->texels.assign(size_t(ve::kSectorStride * ve::kSectorStride), v);
	t->max_slope = slope;
	return t;
}
} // namespace

TEST_CASE("plan asks for the nearest sectors first, once each") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	const ve::SectorCache::Plan a = c.plan(10.0f, 10.0f, 3);
	REQUIRE(a.bake.size() == 3);
	CHECK(a.bake[0] == ve::SectorCoord{0, 0}); // the camera's own sector, distance 0
	const ve::SectorCache::Plan b = c.plan(10.0f, 10.0f, 100);
	for (const ve::SectorCoord &s : b.bake)
		for (const ve::SectorCoord &t : a.bake) CHECK(s != t); // in flight: not asked twice
	CHECK(c.stats().in_flight == int(a.bake.size() + b.bake.size()));
}

TEST_CASE("an inserted sector is found, and readiness covers every overlapped sector") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	c.plan(10.0f, 10.0f, 0);
	c.insert({0, 0}, flat(0.5f));
	CHECK(c.find({0, 0}) != nullptr);
	CHECK(c.ready_world(1.0f, 1.0f, 400.0f, 400.0f));
	CHECK_FALSE(c.ready_world(1.0f, 1.0f, 420.0f, 400.0f)); // reaches into (1, 0)
	CHECK(c.stats().resident == 1);
}

TEST_CASE("readiness ignores sectors beyond the radius") {
	ve::SectorCache c(500.0f, 0.0f, 0.0f);
	c.plan(10.0f, 10.0f, 0);
	c.insert({0, 0}, flat(0.5f));
	// The rectangle spans sectors x = 0..5. (1, 0) starts 399.6 m away and is wanted; (2, 0)
	// onwards start past 500 m, are never baked, and so are never waited on (deviation 10).
	CHECK_FALSE(c.ready_world(100.0f, 100.0f, 2300.0f, 300.0f));
	c.insert({1, 0}, flat(0.5f));
	CHECK(c.ready_world(100.0f, 100.0f, 2300.0f, 300.0f));
	CHECK(c.find({2, 0}) == nullptr);
}

TEST_CASE("moving away evicts what left the radius plus one sector, and bumps the version") {
	ve::SectorCache c(500.0f, 0.0f, 0.0f);
	c.plan(10.0f, 10.0f, 0);
	c.insert({0, 0}, flat(0.5f));
	const uint64_t v = c.version();
	const ve::SectorCache::Plan p = c.plan(5000.0f, 10.0f, 0);
	REQUIRE(p.evicted.size() == 1);
	CHECK(p.evicted[0] == ve::SectorCoord{0, 0});
	CHECK(c.find({0, 0}) == nullptr);
	CHECK(c.version() > v);
}

TEST_CASE("the seed offset moves the wanted sectors into field space") {
	ve::SectorCache c(100.0f, 1000.0f, 0.0f);
	const ve::SectorCache::Plan p = c.plan(10.0f, 10.0f, 1);
	REQUIRE(p.bake.size() == 1);
	CHECK(p.bake[0] == ve::sector_of(1010.0f, 10.0f));
	CHECK(c.centre() == ve::sector_of(1010.0f, 10.0f));
}

TEST_CASE("sector_ground decodes height and falls back to the bottom of the range") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	const float water = 51.2f;
	const ve::SectorGround none = ve::sector_ground(&c, water, 5.0f, 5.0f);
	CHECK(none.height == doctest::Approx(water - ve::kSectorHeightBelowM - ve::kSurfaceY));
	c.insert({0, 0}, flat(0.5f));
	const ve::SectorGround g = ve::sector_ground(&c, water, 5.0f, 5.0f);
	CHECK(g.height == doctest::Approx(water - ve::kSectorHeightBelowM + 0.5f * ve::kSectorHeightSpanM - ve::kSurfaceY).epsilon(1e-4));
	CHECK(g.dhdx == doctest::Approx(0.0f));
	CHECK(g.ridge == doctest::Approx(0.0f).epsilon(1e-3));
	CHECK(ve::sector_ground(nullptr, water, 5.0f, 5.0f).height == doctest::Approx(none.height));
}

TEST_CASE("stats count sectors over the slope limit and track the encoded range") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	c.insert({0, 0}, flat(0.25f, 1.0f));
	c.insert({1, 0}, flat(0.75f, ve::kSectorSlopeLimit + 0.1f));
	const ve::SectorCache::Stats s = c.stats();
	CHECK(s.over_limit == 1);
	CHECK(s.max_slope == doctest::Approx(ve::kSectorSlopeLimit + 0.1f));
	CHECK(s.r_min == doctest::Approx(0.25f).epsilon(1e-4));
	CHECK(s.r_max == doctest::Approx(0.75f).epsilon(1e-4));
	c.clear();
	CHECK(c.stats().resident == 0);
}

TEST_CASE("max_resident bounds every resident set plan can leave behind") {
	ve::SectorCache c(4000.0f, 0.0f, 0.0f);
	// Bake everything wanted from one spot, then step the camera across a sector: what plan()
	// keeps (radius + one sector) must still fit in max_resident() layers.
	for (float x : {0.0f, 204.8f, 409.5f})
		for (float z : {0.0f, 100.0f, 409.5f}) {
			c.clear();
			for (float step : {0.0f, 300.0f, 600.0f}) {
				const ve::SectorCache::Plan p = c.plan(x + step, z, 100000);
				for (const ve::SectorCoord &s : p.bake) c.insert(s, flat(0.5f));
				c.plan(x + step, z, 0);
				CHECK(c.stats().resident <= c.max_resident());
			}
		}
}

TEST_CASE("readers holding a sector survive its eviction") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	std::atomic<bool> stop{false};
	std::atomic<int> bad{0};
	std::vector<std::thread> readers;
	for (int i = 0; i < 4; i++)
		readers.emplace_back([&] {
			while (!stop.load()) {
				const auto t = c.find({0, 0});
				if (t && t->texels.size() != size_t(ve::kSectorStride * ve::kSectorStride)) bad++;
				if (t) (void)ve::sector_bspline(*t, {0, 0}, 100.0f, 100.0f);
			}
		});
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
	while (std::chrono::steady_clock::now() < end) {
		c.insert({0, 0}, flat(0.5f));
		c.clear();
	}
	stop = true;
	for (std::thread &t : readers) t.join();
	CHECK(bad.load() == 0);
}
