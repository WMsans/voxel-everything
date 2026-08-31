#include <doctest/doctest.h>
#include "world/material_table.h"
#include <fstream>
#include <iterator>
#include <string>

// The committed shaders/material_table.glslh is the GPU-shading mirror of ve::kMaterials.
// This test keeps glow and fallback albedo synchronized with the authoritative CPU table.
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
}
