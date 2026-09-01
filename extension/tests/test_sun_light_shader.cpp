#include <doctest/doctest.h>
#include "render/shader_loader.h"
#include <filesystem>

namespace {

std::filesystem::path shader_root() {
	return std::filesystem::absolute(__FILE__).parent_path().parent_path().parent_path() / "shaders";
}

} // namespace

TEST_CASE("raymarch shader consumes the shared SunLight block at binding 24") {
	const auto root = shader_root();
	std::string err;
	const auto source = ve::load_shader_source(
			(root / "raymarch.comp.glsl").string(), root.string(), &err);
	REQUIRE(err.empty());
	CHECK(source.find("#define SUN_LIGHT_BINDING 24") != std::string::npos);
	CHECK(source.find("layout(set = SUN_LIGHT_SET, binding = SUN_LIGHT_BINDING, std140) uniform SunLight") != std::string::npos);
	CHECK(source.find("sun_light.dir.xyz") != std::string::npos);
	CHECK(source.find("SUN_DIR") == std::string::npos);
}
