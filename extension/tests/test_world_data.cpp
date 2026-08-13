#include <doctest/doctest.h>
#include "world/world_data.h"
#include "generator/generator.h"
#include <cmath>

static float hills(float x, float z) { // test oracle
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

TEST_CASE("generation activates a bounded subset of bricks") {
	ve::WorldData w(20, 12, 20); // 16m x 9.6m x 16m
	ve::AnalyticGenerator gen;
	w.generate(gen);
	CHECK(w.active_brick_count() > 0);
	CHECK(w.active_brick_count() < 20 * 12 * 20 / 4); // narrow band: well under 25%
}

TEST_CASE("brick at the terrain surface is active; sky brick is not") {
	ve::WorldData w(20, 12, 20);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	const float h = hills(8.0f, 8.0f); // ~3.07m
	CHECK(w.brick_active(static_cast<int>(8.0f / 0.8f),
	                     static_cast<int>(h / 0.8f),
	                     static_cast<int>(8.0f / 0.8f)));
	CHECK_FALSE(w.brick_active(10, 11, 10)); // y >= 8.8m: above all hills here
}

TEST_CASE("active brick contains a sign change and a non-empty palette") {
	ve::WorldData w(20, 12, 20);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	const float h = hills(8.0f, 8.0f);
	const int slot = w.brick_slot(static_cast<int>(8.0f / 0.8f),
	                              static_cast<int>(h / 0.8f),
	                              static_cast<int>(8.0f / 0.8f));
	REQUIRE(slot >= 0);
	const ve::Brick &b = w.brick(slot);
	bool has_pos = false, has_neg = false;
	for (int i = 0; i < ve::kBrickVoxelCount; i++) {
		const float d = ve::decode_sdf(b.sdf[i]);
		has_pos = has_pos || d > 0.0f;
		has_neg = has_neg || d < 0.0f;
	}
	CHECK(has_pos);
	CHECK(has_neg);
	bool any_material = false;
	for (uint16_t m : b.palette) any_material = any_material || m != 0;
	CHECK(any_material);
}

TEST_CASE("indirection round-trips slot for active bricks, -1 for inactive") {
	ve::WorldData w(20, 12, 20);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	CHECK(static_cast<int>(w.indirection().size()) == 20 * 12 * 20);
	for (int z = 0; z < 20; z++)
		for (int y = 0; y < 12; y++)
			for (int x = 0; x < 20; x++)
				CHECK((w.brick_slot(x, y, z) >= 0) == w.brick_active(x, y, z));
}
