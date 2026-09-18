// A mirror declares the names it binds; create() resolves them against the manifest. The
// point of the whole mechanism is this file's second case: a mirror that names a channel
// the manifest does not declare must fail LOUDLY at load, where the old extra[] indexing
// silently bound whatever slot happened to sit at that position.
#include <doctest/doctest.h>
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_library.h"
#include <memory>
#include <string>
#include <vector>

namespace {

VE_STAGE_SLOTS(Probe, p, sdf, height);
VE_STAGE_PARAMS(Probe, gain);

void stage_probe(ve::FieldCtx &ctx, const ProbeSlots &s, const ProbeParams &pm,
		const ve::FieldResources &) {
	ctx.f(s.height) = pm.gain * ctx.v(s.p)[0];
	ctx.f(s.sdf) = ctx.v(s.p)[1] - ctx.f(s.height);
}
VE_REGISTER_STAGE("ve::test_stage_probe", Probe, stage_probe);

// Binds a channel no manifest in this file declares.
VE_STAGE_SLOTS(Absent, p, sdf, temperature);
VE_STAGE_PARAMS(Absent);

void stage_absent(ve::FieldCtx &ctx, const AbsentSlots &s, const AbsentParams &,
		const ve::FieldResources &) {
	ctx.f(s.sdf) = ctx.f(s.temperature);
}
VE_REGISTER_STAGE("ve::test_stage_absent", Absent, stage_absent);

ve::StageManifest probe_manifest(const char *cpu) {
	ve::StageManifest m;
	m.name = "probe";
	m.kind = ve::StageKind::kField;
	m.cpu_symbol = cpu;
	m.lipschitz_mode = ve::LipschitzMode::kAdd;
	m.lipschitz = 2.0f;
	m.writes.push_back({"sdf", ve::ChannelType::kFloat});
	m.writes.push_back({"height", ve::ChannelType::kFloat});
	m.params.push_back({"gain", ve::ChannelType::kFloat, 3.0f});
	m.body = "void stage_probe(inout FieldCtx c){}\n";
	return m;
}

ve::ResolvedPipeline resolve_one(const ve::StageManifest &m) {
	ve::PipelineDesc d;
	d.stages.push_back({"s", {}});
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, {m}, &p, &err), err);
	return p;
}

} // namespace

TEST_CASE("a mirror's declared names resolve to the manifest's channels and params") {
	const ve::ResolvedPipeline p = resolve_one(probe_manifest("ve::test_stage_probe"));
	std::string err;
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	REQUIRE_MESSAGE(g != nullptr, err);
	// height = gain * x = 3 * 2 = 6; sdf = y - height = 10 - 6 = 4.
	CHECK(g->eval(2.0f, 10.0f, 0.0f).sdf == doctest::Approx(4.0f));
}

TEST_CASE("a mirror binding a channel the manifest does not declare fails create, by name") {
	ve::StageManifest m = probe_manifest("ve::test_stage_absent");
	const ve::ResolvedPipeline p = resolve_one(m);
	std::string err;
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	CHECK(g == nullptr);
	CHECK(err.find("temperature") != std::string::npos);
	CHECK(err.find("probe") != std::string::npos);
}

TEST_CASE("a mirror binding a param the manifest does not declare fails create, by name") {
	ve::StageManifest m = probe_manifest("ve::test_stage_probe");
	m.params.clear();  // the mirror still declares `gain`
	const ve::ResolvedPipeline p = resolve_one(m);
	std::string err;
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	CHECK(g == nullptr);
	CHECK(err.find("gain") != std::string::npos);
}
