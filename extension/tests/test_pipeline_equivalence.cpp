// THE GATE. The stage pipeline must reproduce ve::AnalyticGenerator exactly -- same sdf
// bits, same materials, same whole bricks. Anything less means the port changed the world.
#include <doctest/doctest.h>
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_manifest.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

namespace {

std::string slurp(const std::string &p) {
	std::ifstream f(p);
	REQUIRE_MESSAGE(f.good(), "cannot open ", p);
	std::ostringstream o;
	o << f.rdbuf();
	return o.str();
}

std::unique_ptr<ve::PipelineFieldGenerator> default_pipeline() {
	const std::string root(VE_REPO_ROOT);
	ve::PipelineDesc d;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_pipeline_desc(
			slurp(root + "/assets/pipelines/default.pipeline"), &d, &err), err);
	std::vector<ve::StageManifest> loaded;
	for (const auto &r : d.stages) {
		ve::StageManifest m;
		REQUIRE_MESSAGE(ve::parse_stage_manifest(slurp(root + "/shaders/" + r.path), &m, &err), err);
		loaded.push_back(m);
	}
	ve::ResolvedPipeline p;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, loaded, &p, &err), err);
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	return std::unique_ptr<ve::PipelineFieldGenerator>(g);
}

uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

} // namespace

TEST_CASE("the default pipeline reproduces the analytic field over the baseline corpus") {
	auto g = default_pipeline();
	ve::AnalyticGenerator ref;
	uint32_t s = 20260903u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	int checked = 0;
	for (int i = 0; i < 512; i++) {
		const float x = next(-20.0f, 60.0f), y = next(21.2f, 81.2f), z = next(-20.0f, 60.0f);
		const ve::Sample a = ref.sample(x, y, z);
		const ve::Sample b = g->eval(x, y, z);
		CHECK(bits(a.sdf) == bits(b.sdf));
		CHECK(a.material == b.material);
		checked++;
	}
	CHECK(checked == 512);
}

TEST_CASE("the default pipeline reproduces whole bricks") {
	auto g = default_pipeline();
	ve::AnalyticGenerator ref;
	const ve::IVec3 bricks[] = {
		{0, 64, 0}, {15, 63, 15}, {37, 63, 37}, {37, 62, 37},
		{-25, 60, -25}, {0, 40, 0}, {0, 90, 0}, {1000, 64, 1000},
	};
	for (const ve::IVec3 &b : bricks) {
		ve::BrickEval want{}, got{};
		ve::eval_brick(ref, nullptr, 0, b, &want);
		ve::eval_brick(g->sampler(), nullptr, 0, b, &got);
		CHECK(std::memcmp(want.brick.sdf, got.brick.sdf, sizeof(want.brick.sdf)) == 0);
		CHECK(std::memcmp(want.brick.mat, got.brick.mat, sizeof(want.brick.mat)) == 0);
		CHECK(std::memcmp(&want.mips, &got.mips, sizeof(want.mips)) == 0);
	}
}

TEST_CASE("the pipeline reports the analytic generator's lipschitz bound") {
	auto g = default_pipeline();
	ve::AnalyticGenerator ref;
	CHECK(g->sampler().lipschitz() == doctest::Approx(ref.lipschitz()));
}
