#include <doctest/doctest.h>
#include "terrain/pipeline.h"

TEST_CASE("parses a pipeline with indented param overrides") {
	const char *src =
		"# a comment\n"
		"seed      1337\n"
		"lipschitz 2.0\n"
		"\n"
		"stage stages/hills.field.glslh\n"
		"  amplitude 6.0\n"
		"  frequency 0.11\n"
		"stage stages/cave.field.glslh\n"
		"  radius 5.0\n";
	ve::PipelineDesc d;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_pipeline_desc(src, &d, &err), err);
	CHECK(d.seed == 1337u);
	CHECK(d.lipschitz_override == doctest::Approx(2.0f));
	REQUIRE(d.stages.size() == 2);
	CHECK(d.stages[0].path == "stages/hills.field.glslh");
	REQUIRE(d.stages[0].param_overrides.size() == 2);
	CHECK(d.stages[0].param_overrides[0].first == "amplitude");
	CHECK(d.stages[0].param_overrides[0].second == doctest::Approx(6.0f));
	CHECK(d.stages[1].path == "stages/cave.field.glslh");
	REQUIRE(d.stages[1].param_overrides.size() == 1);
}

TEST_CASE("allow_gpu_only defaults off and can be set") {
	ve::PipelineDesc d;
	std::string err;
	REQUIRE(ve::parse_pipeline_desc("stage a\n", &d, &err));
	CHECK_FALSE(d.allow_gpu_only);
	REQUIRE(ve::parse_pipeline_desc("allow_gpu_only 1\nstage a\n", &d, &err));
	CHECK(d.allow_gpu_only);
}

TEST_CASE("a param override before any stage is an error") {
	ve::PipelineDesc d;
	std::string err;
	CHECK_FALSE(ve::parse_pipeline_desc("  amplitude 6.0\n", &d, &err));
	CHECK(err.find("amplitude") != std::string::npos);
}

TEST_CASE("an empty pipeline is an error") {
	ve::PipelineDesc d;
	std::string err;
	CHECK_FALSE(ve::parse_pipeline_desc("seed 1\n", &d, &err));
	CHECK(err.find("stage") != std::string::npos);
}
