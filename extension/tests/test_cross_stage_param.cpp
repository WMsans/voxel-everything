// The footgun default.pipeline used to carry as a comment: "overriding hills amp_* also
// requires updating the literals in stage_cave's CPU mirror". The cave's GLSL reads
// P.hills_amp_*, so a pipeline override moved the GPU field and left the CPU field on the
// old constants. The mirror binds the resolved value now; this proves it.
#include <doctest/doctest.h>
#include "terrain/pipeline_field_generator.h"
#include "terrain/pipeline_load.h"
#include <fstream>
#include <map>
#include <memory>
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

// The real stage manifests, with a pipeline built in memory so the override is the only
// thing that differs between the two generators.
std::unique_ptr<ve::PipelineFieldGenerator> build(const std::string &pipeline_text) {
	const std::string root(VE_REPO_ROOT);
	ve::TextReader reader = [&](const std::string &path, std::string *out) {
		if (path == "<memory>") { *out = pipeline_text; return true; }
		return repo_reader(path, out);
	};
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(reader, "<memory>", root + "/shaders/", &p, nullptr, &err), err);
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	return std::unique_ptr<ve::PipelineFieldGenerator>(g);
}

const char *kBase =
		"seed 1337\n"
		"lipschitz 2.0\n"
		"stage stages/hills.field.glslh\n"
		"stage stages/cave.field.glslh\n"
		"stage stages/height_bands.field.glslh\n";

const char *kOverridden =
		"seed 1337\n"
		"lipschitz 2.0\n"
		"stage stages/hills.field.glslh\n"
		"  amp_a 2.0\n"
		"stage stages/cave.field.glslh\n"
		"stage stages/height_bands.field.glslh\n";

} // namespace

TEST_CASE("overriding hills.amp_a moves the cave's CPU mirror too") {
	auto base = build(kBase);
	auto over = build(kOverridden);

	// A point on the cave's rim, where the carve's centre height depends on hills' amp_a.
	// If the mirror still held the old literal, this sample would be identical in both.
	const float x = 30.0f, y = 49.0f, z = 30.0f;
	CHECK(base->sample(x, y, z).sdf != doctest::Approx(over->sample(x, y, z).sdf));
}
