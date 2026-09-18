// Gradient-sensitive pipeline overrides are rejected because the static stage budget is
// not recomputed. This keeps a CPU/GPU parameter divergence from becoming an unsafe bound.
#include <doctest/doctest.h>
#include "terrain/pipeline_load.h"
#include <fstream>
#include <sstream>
#include <string>

namespace {

bool repo_reader(const std::string &path, std::string *out) {
	std::ifstream f(path);
	if (!f.good()) return false;
	std::ostringstream o;
	o << f.rdbuf();
	*out = o.str();
	return true;
}

const char *kOverridden =
		"seed 1337\n"
		"lipschitz 2.0\n"
		"stage stages/hills.field.glslh\n"
		"  amp_a 2.0\n"
		"stage stages/cave.field.glslh\n"
		"stage stages/height_bands.field.glslh\n";

} // namespace

TEST_CASE("overriding a gradient-sensitive stage is rejected before CPU/GPU can diverge") {
	std::string err;
	const std::string root(VE_REPO_ROOT);
	ve::TextReader reader = [&](const std::string &path, std::string *out) {
		if (path == "<memory>") { *out = kOverridden; return true; }
		return repo_reader(path, out);
	};
	ve::ResolvedPipeline p;
	CHECK_FALSE(ve::load_pipeline(reader, "<memory>", root + "/shaders/", &p, nullptr, &err));
	CHECK(err.find("hills") != std::string::npos);
	CHECK(err.find("gradient bound") != std::string::npos);
}
