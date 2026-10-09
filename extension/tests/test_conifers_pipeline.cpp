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
} // namespace

TEST_CASE("fjords resolves with the conifers stage after the bands, at an unchanged bound") {
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root + "/assets/pipelines/fjords.pipeline",
			root + "/shaders/", &p, nullptr, &err), err);
	REQUIRE(p.stages.size() == 4);
	CHECK(p.stages[2].name == "fjord_bands");
	CHECK(p.stages[3].name == "conifers");
	// `mul 1.0`: the union of 1-Lipschitz trunks leaves the fjord field's 8.54 alone, and
	// conifer.glslh's CONIFER_FIELD_L (8.6) must not be below it.
	CHECK(p.lipschitz == doctest::Approx(8.54f).epsilon(0.002));
	CHECK(p.lipschitz <= 8.6f);
	bool cell = false;
	for (const ve::ParamDecl &d : p.params)
		if (d.name == "conifers.cell") cell = d.value == 8.0f;
	CHECK(cell);
}
