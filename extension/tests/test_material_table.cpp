#include <doctest/doctest.h>
#include "world/material_table.h"
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

// tools/convert_materials.sh cannot include a C++ header, so its MATERIALS array is a
// second list of the same names. That is exactly the drift the old kMaterialNames had and
// nothing checked. This checks it.
TEST_CASE("the converter script's material list matches the table") {
	std::ifstream f("../tools/convert_materials.sh");
	REQUIRE_MESSAGE(f.good(), "cannot open ../tools/convert_materials.sh");
	std::string line, found;
	while (std::getline(f, line))
		if (line.rfind("MATERIALS=(", 0) == 0) { found = line; break; }
	REQUIRE_MESSAGE(!found.empty(), "no MATERIALS=( line in convert_materials.sh");

	const size_t open = found.find('('), close = found.rfind(')');
	REQUIRE(open != std::string::npos);
	REQUIRE(close != std::string::npos);
	std::istringstream items(found.substr(open + 1, close - open - 1));
	std::vector<std::string> names;
	for (std::string n; items >> n; ) names.push_back(n);

	REQUIRE(static_cast<int>(names.size()) == ve::kMaterialCount);
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(names[i] == ve::kMaterials[i].name);
}

TEST_CASE("the table fits inside the atlas layer bound") {
	// kMaterialLayers = 16 (extension/src/render/material_atlas.h). source_count clamps
	// silently, so an overflowing table would render flat magenta while hardness and glow
	// stayed live -- silent drift, caught here instead. A static_assert would drag
	// godot-cpp into the pure native test binary.
	CHECK(ve::kMaterialCount <= 16);
}
