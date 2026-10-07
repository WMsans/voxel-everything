#include <doctest/doctest.h>
#include "terrain/seed_offset.h"
#include <cstdint>
#include <cstdlib>
#include <set>
#include <utility>

TEST_CASE("seed 0 is the unshifted terrain every golden was recorded on") {
	const ve::SeedOffset o = ve::seed_offset(0);
	CHECK(o.x == 0);
	CHECK(o.z == 0);
}

TEST_CASE("the offset is a pure function of the seed") {
	for (uint32_t s : {1u, 42u, 1337u, 0xffffffffu}) {
		CHECK(ve::seed_offset(s).x == ve::seed_offset(s).x);
		CHECK(ve::seed_offset(s).z == ve::seed_offset(s).z);
	}
}

TEST_CASE("every offset stays inside +-8192 m on both axes") {
	for (uint32_t s = 1; s <= 20000; s++) {
		const ve::SeedOffset o = ve::seed_offset(s);
		REQUIRE(o.x >= -8192);
		REQUIRE(o.x < 8192);
		REQUIRE(o.z >= -8192);
		REQUIRE(o.z < 8192);
	}
	const ve::SeedOffset top = ve::seed_offset(0xffffffffu);
	CHECK(top.x >= -8192);
	CHECK(top.x < 8192);
}

TEST_CASE("distinct seeds land in distinct places") {
	std::set<std::pair<int32_t, int32_t>> seen;
	for (uint32_t s = 1; s <= 1000; s++) {
		const ve::SeedOffset o = ve::seed_offset(s);
		seen.insert({o.x, o.z});
	}
	CHECK(seen.size() == 1000);
}

TEST_CASE("neighbouring seeds are not neighbouring places") {
	for (uint32_t s = 1; s <= 200; s++) {
		const ve::SeedOffset a = ve::seed_offset(s), b = ve::seed_offset(s + 1);
		CHECK(std::abs(a.x - b.x) + std::abs(a.z - b.z) > 64);
	}
}
