// Every committed file under shaders/generated/ is compared byte for byte with the C++ that
// produces it -- the material_table.glslh pattern. Regenerate after an intentional change:
//   cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests
#include <doctest/doctest.h>
#include "gpu_layout/blocks.h"
#include "gpu_layout/cel_emit.h"
#include "gpu_layout/constants.h"
#include "gpu_layout/gbuffer_layout.h"
#include "shade/cel.h"
#include "render/shader_loader.h"
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string root() { return std::string(VE_REPO_ROOT); }

void check_generated(const std::string &rel, const std::string &expected) {
	const std::string path = root() + "/" + rel;
	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		std::ofstream out(path, std::ios::binary);
		REQUIRE(out.good());
		out << expected;
	}
	std::ifstream f(path, std::ios::binary);
	REQUIRE_MESSAGE(f.good(), "cannot open ", path);
	const std::string on_disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	CHECK_MESSAGE(on_disk == expected, rel,
			" is stale. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests");
	// The shader loader expands include directives anywhere in a line, so generated text must
	// hold none, end in a newline, and come back from the loader unchanged.
	CHECK(expected.find("#include \"") == std::string::npos);
	REQUIRE(!expected.empty());
	CHECK(expected.back() == '\n');
	std::string err;
	CHECK(ve::load_shader_source(path, root() + "/shaders", &err) == expected);
	CHECK(err.empty());
}

} // namespace

TEST_CASE("generated: shaders/generated/blocks.glslh") {
	check_generated("shaders/generated/blocks.glslh", ve::layout::blocks_glsl());
}

TEST_CASE("generated: shaders/generated/constants.glslh") {
	check_generated("shaders/generated/constants.glslh", ve::layout::constants_glsl());
}

TEST_CASE("generated: shaders/generated/gbuffer.glslh") {
	check_generated("shaders/generated/gbuffer.glslh", ve::layout::gbuffer_glsl());
	CHECK(ve::layout::kGbColorAttachments == 2);
}

TEST_CASE("glsl_float round-trips and always reads as a float literal") {
	const ve::CelParams p;
	const float values[] = {p.band_edge[0], p.band_edge[1], p.band_edge[2], p.band_level[0],
		p.band_level[3], p.shadow_hue_shift, p.shadow_saturation, p.spec_edge, p.spec_strength,
		p.rim_strength, p.rim_power, 1.0f, 3.0f};
	for (float v : values) {
		const std::string s = ve::layout::glsl_float(v);
		CHECK_MESSAGE(std::strtof(s.c_str(), nullptr) == v, s);
		CHECK_MESSAGE(s.find_first_of(".eE") != std::string::npos, s);
	}
	CHECK(ve::layout::glsl_float(0.08f) == "0.08");
	CHECK(ve::layout::glsl_float(1.0f) == "1.0");
}

TEST_CASE("generated: shaders/generated/cel.glslh") {
	check_generated("shaders/generated/cel.glslh", ve::layout::cel_glsl());
}

TEST_CASE("generated: shaders/generated/cel_constants.gdshaderinc") {
	check_generated("shaders/generated/cel_constants.gdshaderinc", ve::layout::cel_gdshaderinc());
}
