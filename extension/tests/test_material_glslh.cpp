#include <doctest/doctest.h>
#include "world/material_table.h"
#include <fstream>
#include <iterator>
#include <string>

// The committed shaders/material_table.glslh is a MIRROR of ve::kMaterials. If they drift,
// materials glow and shade differently on CPU and GPU and the tests that notice fail in a
// place that gives no hint why. This test is the gate; its failure message is the fix.
// Hardness is deliberately absent from the mirror: ve::removal_radius consumes it once, on
// the CPU, so no shader ever looks it up.
TEST_CASE("the committed GLSL mirror matches the C++ table") {
	// The native test binary runs from extension/, so the repo root is one level up.
	std::ifstream f("../shaders/material_table.glslh");
	REQUIRE_MESSAGE(f.good(), "cannot open ../shaders/material_table.glslh");
	const std::string on_disk((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());
	const std::string expected = ve::material_table_glsl();
	CHECK_MESSAGE(on_disk == expected,
			"shaders/material_table.glslh is stale. Replace its entire contents with:\n"
			<< expected);
}

TEST_CASE("the emitter covers every material") {
	const std::string s = ve::material_table_glsl();
	CHECK(s.find("MATERIAL_COUNT = " + std::to_string(ve::kMaterialCount)) != std::string::npos);
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(s.find(ve::kMaterials[i].name) != std::string::npos);
	// Hardness never reaches a shader as a SYMBOL (the prose explaining why does mention
	// it). If the table or the lookup reappears, a field evaluator is about to start
	// scaling per sample again and the seam artifact comes back with it.
	CHECK(s.find("MAT_HARDNESS") == std::string::npos);
	CHECK(s.find("mat_hardness(") == std::string::npos);
}
