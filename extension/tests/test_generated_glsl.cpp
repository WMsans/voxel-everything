// Every committed file under shaders/generated/ is compared byte for byte with the C++ that
// produces it -- the material_table.glslh pattern. Regenerate after an intentional change:
//   cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests
#include <doctest/doctest.h>
#include "gpu_layout/blocks.h"
#include "gpu_layout/constants.h"
#include "gpu_layout/gbuffer_layout.h"
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
