#include <doctest/doctest.h>
#include "world/material_table.h"
#include <cstring>
#include <set>
#include <string>

TEST_CASE("every material is at least as hard as the baseline") {
	// hardness < 1.0 would make a carve reach outside op_world_aabb's pos +/- radius,
	// and an op that reaches outside its declared AABB is dropped at region boundaries.
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(ve::kMaterials[i].hardness >= 1.0f);
}

TEST_CASE("glow strengths are non-negative") {
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(ve::kMaterials[i].glow >= 0.0f);
}

TEST_CASE("material names and assets are unique and non-empty") {
	std::set<std::string> names, assets;
	for (int i = 0; i < ve::kMaterialCount; i++) {
		REQUIRE(ve::kMaterials[i].name != nullptr);
		REQUIRE(ve::kMaterials[i].asset != nullptr);
		CHECK(std::strlen(ve::kMaterials[i].name) > 0);
		CHECK(names.insert(ve::kMaterials[i].name).second);
		CHECK(assets.insert(ve::kMaterials[i].asset).second);
	}
}

TEST_CASE("lookups map id i+1 to table entry i and fail soft out of range") {
	CHECK(ve::material_hardness(1) == doctest::Approx(ve::kMaterials[0].hardness));
	CHECK(ve::material_glow(1) == doctest::Approx(ve::kMaterials[0].glow));
	// Air and out-of-range ids must carve at full radius and emit nothing.
	CHECK(ve::material_hardness(0) == doctest::Approx(1.0f));
	CHECK(ve::material_glow(0) == doctest::Approx(0.0f));
	CHECK(ve::material_hardness(9999) == doctest::Approx(1.0f));
	CHECK(ve::material_glow(9999) == doctest::Approx(0.0f));
}
