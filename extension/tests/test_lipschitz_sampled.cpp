// The declared bound versus the actual field. resolve_pipeline can combine stage bounds
// perfectly and still report a lie, because nothing checks that a stage's //!lipschitz
// number describes its own code. This is the terrain design's section 10.1 check, as a
// test: understating the bound makes raycast.cpp tunnel through surfaces, and that failure
// looks like a RENDERING bug to whoever hits it.
//
// Epsilon matches ve::Generator::sample_gradient (generator.cpp:22), so this measures the
// same differences every consumer of the gradient sees.
#include <doctest/doctest.h>
#include "terrain/pipeline_field_generator.h"
#include "terrain/pipeline_load.h"
#include <cmath>
#include <fstream>
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

void check_bound(const char *pipeline) {
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader,
			root + "/assets/pipelines/" + pipeline, root + "/shaders/", &p, nullptr, &err), err);
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	REQUIRE_MESSAGE(g != nullptr, err);

	const float e = 0.01f;
	uint32_t s = 20260917u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};

	float worst = 0.0f;
	float wx = 0.0f, wy = 0.0f, wz = 0.0f;
	for (int i = 0; i < 4096; i++) {
		// Wide enough to cross the relief wavelengths and the carved cave, and to reach
		// where sin() range reduction is hardest.
		const float x = next(-3000.0f, 3000.0f);
		const float y = next(-200.0f, 400.0f);
		const float z = next(-3000.0f, 3000.0f);
		const float dx = (g->sample(x + e, y, z).sdf - g->sample(x - e, y, z).sdf) / (2.0f * e);
		const float dy = (g->sample(x, y + e, z).sdf - g->sample(x, y - e, z).sdf) / (2.0f * e);
		const float dz = (g->sample(x, y, z + e).sdf - g->sample(x, y, z - e).sdf) / (2.0f * e);
		const float mag = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (mag > worst) { worst = mag; wx = x; wy = y; wz = z; }
	}

	CHECK_MESSAGE(worst <= p.lipschitz, pipeline, ": sampled |grad sdf| ", worst,
			" exceeds the reported bound ", p.lipschitz, " at (", wx, ", ", wy, ", ", wz,
			"). A stage's //!lipschitz number is understating its own gradient.");
}

} // namespace

TEST_CASE("default.pipeline never exceeds its reported gradient bound") {
	check_bound("default.pipeline");
}

TEST_CASE("golden.pipeline never exceeds its reported gradient bound") {
	check_bound("golden.pipeline");
}

TEST_CASE("trees.pipeline reports a sound bound") {
	check_bound("trees.pipeline");
}

TEST_CASE("the trees stage does not raise the pipeline bound") {
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline with, without;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root + "/assets/pipelines/trees.pipeline",
			root + "/shaders/", &with, nullptr, &err), err);
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root + "/assets/pipelines/default.pipeline",
			root + "/shaders/", &without, nullptr, &err), err);
	CHECK(with.lipschitz == doctest::Approx(without.lipschitz));
}
