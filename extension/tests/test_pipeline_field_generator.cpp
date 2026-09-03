#include <doctest/doctest.h>
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_library.h"
#include "terrain/pipeline.h"

namespace {

// A two-stage pipeline whose result is checkable by hand: sdf = y - amplitude, then a
// second stage that sets material from the sign of sdf.
void tp_plane(ve::FieldCtx &ctx, const ve::StageSlots &s, const ve::StageParams &p,
		const ve::FieldResources &) {
	ctx.f(s.sdf) = ctx.v(s.p)[1] - p.at(0);
}
void tp_mat(ve::FieldCtx &ctx, const ve::StageSlots &s, const ve::StageParams &,
		const ve::FieldResources &) {
	ctx.f(s.material) = ctx.f(s.sdf) <= 0.0f ? 3.0f : 0.0f;
}

ve::ResolvedPipeline build(bool with_cpu = true) {
	ve::StageManifest a;
	a.name = "plane"; a.kind = ve::StageKind::kField;
	a.cpu_symbol = with_cpu ? "ve::tp_plane" : "";
	a.writes.push_back({"sdf", ve::ChannelType::kFloat});
	a.params.push_back({"height", ve::ChannelType::kFloat, 51.2f});
	a.lipschitz = 1.0f;
	a.body = "void stage_plane(inout FieldCtx c){}\n";

	ve::StageManifest b;
	b.name = "mat"; b.kind = ve::StageKind::kField; b.cpu_symbol = "ve::tp_mat";
	b.reads.push_back({"sdf", ve::ChannelType::kFloat});
	b.writes.push_back({"material", ve::ChannelType::kUint});
	b.lipschitz = 1.0f;
	b.body = "void stage_mat(inout FieldCtx c){}\n";

	ve::PipelineDesc d;
	d.allow_gpu_only = !with_cpu;
	d.stages.push_back({"a", {}});
	d.stages.push_back({"b", {}});
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, {a, b}, &p, &err), err);
	return p;
}

} // namespace

VE_REGISTER_STAGE("ve::tp_plane", tp_plane);
VE_REGISTER_STAGE("ve::tp_mat", tp_mat);

TEST_CASE("stages run in order and the last sdf write wins") {
	std::string err;
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(build(), &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	CHECK(g->eval(0.0f, 61.2f, 0.0f).sdf == doctest::Approx(10.0f));
	CHECK(g->eval(0.0f, 41.2f, 0.0f).sdf == doctest::Approx(-10.0f));
	CHECK(g->eval(0.0f, 41.2f, 0.0f).material == 3);
	CHECK(g->eval(0.0f, 61.2f, 0.0f).material == 0);
	CHECK(g->is_cpu_exact());
	delete g;
}

TEST_CASE("sampler() hands out a Generator view with the pipeline's lipschitz bound") {
	std::string err;
	ve::ResolvedPipeline p = build();
	p.lipschitz = 3.5f;
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	const ve::Generator &view = g->sampler();
	CHECK(view.sample(0.0f, 61.2f, 0.0f).sdf == doctest::Approx(10.0f));
	CHECK(view.lipschitz() == doctest::Approx(3.5f));
	delete g;
}

TEST_CASE("an unregistered cpu symbol fails create with a named error") {
	ve::ResolvedPipeline p = build();
	p.stages[0].cpu_symbol = "ve::not_registered";
	std::string err;
	CHECK(ve::PipelineFieldGenerator::create(p, &err) == nullptr);
	CHECK(err.find("ve::not_registered") != std::string::npos);
}

TEST_CASE("a GPU-only pipeline creates but reports itself inexact") {
	std::string err;
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(build(false), &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	CHECK_FALSE(g->is_cpu_exact());
	delete g;
}
