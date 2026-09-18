#include <doctest/doctest.h>
#include "terrain/pipeline.h"
#include "terrain/stage_manifest.h"

namespace {

ve::StageManifest field_stage(const char *name, std::vector<const char *> writes,
		std::vector<const char *> reads, const char *cpu = "ve::x") {
	ve::StageManifest m;
	m.name = name;
	m.kind = ve::StageKind::kField;
	m.cpu_symbol = cpu;
	m.lipschitz_mode = ve::LipschitzMode::kAdd;
	m.lipschitz = 1.0f;
	auto type_for = [](const char *n) {
		return std::string(n) == "material" ? ve::ChannelType::kUint : ve::ChannelType::kFloat;
	};
	for (const char *w : writes) m.writes.push_back({w, type_for(w)});
	for (const char *r : reads)  m.reads.push_back({r, type_for(r)});
	m.body = "void s(inout FieldCtx c){}\n";
	return m;
}

ve::PipelineDesc desc_for(size_t n, bool allow_gpu_only = false) {
	ve::PipelineDesc d;
	d.allow_gpu_only = allow_gpu_only;
	for (size_t i = 0; i < n; i++) d.stages.push_back({"s", {}});
	return d;
}

} // namespace

TEST_CASE("built-in channels exist and declared channels get stable slots") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf", "temperature"}, {}),
		field_stage("b", {"material"}, {"temperature"}),
	};
	st[1].lipschitz_mode = ve::LipschitzMode::kNone;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
	CHECK(p.channel_slot("p") == 0);
	CHECK(p.channel_slot("sdf") == 1);
	CHECK(p.channel_slot("material") == 2);
	CHECK(p.channel_slot("temperature") == 3);
	CHECK(p.channel_slot("nope") == -1);
	CHECK(p.cpu_exact);
}

TEST_CASE("a pipeline that never writes sdf is rejected") {
	std::vector<ve::StageManifest> st{field_stage("a", {"temperature"}, {})};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("sdf") != std::string::npos);
}

TEST_CASE("reading a channel no earlier stage wrote is rejected, naming both") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf"}, {}),
		field_stage("b", {"sdf"}, {"moisture"}),
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("moisture") != std::string::npos);
	CHECK(err.find("b") != std::string::npos);
}

TEST_CASE("two stages writing one channel is legal -- ordered override is the model") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf"}, {}),
		field_stage("b", {"sdf"}, {"sdf"}),
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
}

TEST_CASE("a channel type conflict is rejected") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf"}, {}),
		field_stage("b", {"sdf"}, {}),
	};
	st[1].writes[0].type = ve::ChannelType::kVec3;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("sdf") != std::string::npos);
}

TEST_CASE("duplicate stage names are rejected") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf"}, {}), field_stage("a", {"sdf"}, {}),
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("a") != std::string::npos);
}

TEST_CASE("a GPU-only stage needs the opt-in") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {}, "")};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("a") != std::string::npos);
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(1, true), st, &p, &err), err);
	CHECK_FALSE(p.cpu_exact);
}

TEST_CASE("a map stage in a field pipeline is rejected in Plan A") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].kind = ve::StageKind::kMap;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("map") != std::string::npos);
}

TEST_CASE("resources sort by name") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].samples.push_back({"sector.z", "texture2d_r32f", 0.0f});
	st[0].samples.push_back({"sector.a", "texture2d_r32f", 0.0f});
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd;
	st[0].lipschitz = 1.5f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(1), st, &p, &err), err);
	REQUIRE(p.resources.size() == 2);
	CHECK(p.resources[0].name == "sector.a");
	CHECK(p.resources[1].name == "sector.z");
}

TEST_CASE("additive stages add to the bound and composing stages multiply it") {
	std::vector<ve::StageManifest> st{
		field_stage("base", {"sdf"}, {}),
		field_stage("relief", {"sdf"}, {"sdf"}),
		field_stage("warp", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[1].lipschitz_mode = ve::LipschitzMode::kAdd; st[1].lipschitz = 0.21f;
	st[2].lipschitz_mode = ve::LipschitzMode::kMul; st[2].lipschitz = 2.0f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(3), st, &p, &err), err);
	CHECK(p.lipschitz == doctest::Approx(3.98f));  // (1.78 + 0.21) * 2.0
}

TEST_CASE("a stage that writes no sdf contributes nothing and must declare nothing") {
	std::vector<ve::StageManifest> st{
		field_stage("base", {"sdf"}, {}),
		field_stage("bands", {"material"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[1].lipschitz_mode = ve::LipschitzMode::kNone;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
	CHECK(p.lipschitz == doctest::Approx(1.78f));

	st[1].lipschitz_mode = ve::LipschitzMode::kMul; st[1].lipschitz = 3.0f;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("bands") != std::string::npos);
	CHECK(err.find("writes no sdf") != std::string::npos);
}

TEST_CASE("an sdf writer with no declared bound is rejected by name") {
	std::vector<ve::StageManifest> st{field_stage("base", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kNone;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("base") != std::string::npos);
	CHECK(err.find("//!lipschitz") != std::string::npos);
}

TEST_CASE("the first sdf writer must be additive, because a multiplied zero is not a bound") {
	std::vector<ve::StageManifest> st{field_stage("base", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kMul; st[0].lipschitz = 2.0f;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("base") != std::string::npos);
	CHECK(err.find("add") != std::string::npos);
}

TEST_CASE("the pipeline's lipschitz line is a ceiling, and exceeding it fails the load") {
	std::vector<ve::StageManifest> st{
		field_stage("base", {"sdf"}, {}),
		field_stage("steep", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[1].lipschitz_mode = ve::LipschitzMode::kAdd; st[1].lipschitz = 8.0f;

	ve::PipelineDesc d = desc_for(2);
	d.lipschitz_ceiling = 2.0f;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(d, st, &p, &err));
	// The message must name every contributor, so the artist can see which stage to budget.
	CHECK(err.find("base") != std::string::npos);
	CHECK(err.find("steep") != std::string::npos);

	// No ceiling declared: the computed bound is reported and the load succeeds.
	d.lipschitz_ceiling = 0.0f;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, st, &p, &err), err);
	CHECK(p.lipschitz == doctest::Approx(9.78f));
}

TEST_CASE("a ceiling the stages fit under is accepted and does not replace the bound") {
	std::vector<ve::StageManifest> st{field_stage("base", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	ve::PipelineDesc d = desc_for(1);
	d.lipschitz_ceiling = 2.0f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, st, &p, &err), err);
	CHECK(p.lipschitz == doctest::Approx(1.78f));  // NOT 2.0
}

TEST_CASE("safe param overrides win, and the hash moves when they do") {
	std::vector<ve::StageManifest> st{
			field_stage("a", {"sdf"}, {}),
			field_stage("bands", {"material"}, {"sdf"})};
	st[1].lipschitz_mode = ve::LipschitzMode::kNone;
	st[1].params.push_back({"amplitude", ve::ChannelType::kFloat, 6.0f});
	ve::ResolvedPipeline p1, p2;
	std::string err;
	REQUIRE(ve::resolve_pipeline(desc_for(2), st, &p1, &err));
	CHECK(p1.params[0].value == doctest::Approx(6.0f));
	ve::PipelineDesc d = desc_for(2);
	d.stages[1].param_overrides.emplace_back("amplitude", 9.0f);
	REQUIRE(ve::resolve_pipeline(d, st, &p2, &err));
	CHECK(p2.params[0].value == doctest::Approx(9.0f));
	CHECK(p1.hash != p2.hash);
}

TEST_CASE("overrides on sdf stages are rejected until their gradient bound is recomputed") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].params.push_back({"amplitude", ve::ChannelType::kFloat, 6.0f});
	ve::PipelineDesc d = desc_for(1);
	d.stages[0].param_overrides.emplace_back("amplitude", 9.0f);
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(d, st, &p, &err));
	CHECK(err.find("a") != std::string::npos);
	CHECK(err.find("gradient bound") != std::string::npos);
}

TEST_CASE("an override naming an unknown param is rejected") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	ve::PipelineDesc d = desc_for(1);
	d.stages[0].param_overrides.emplace_back("nope", 1.0f);
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(d, st, &p, &err));
	CHECK(err.find("nope") != std::string::npos);
}

TEST_CASE("a body reading another stage's param without //!use is rejected, naming the token") {
	std::vector<ve::StageManifest> st{
		field_stage("hills", {"sdf"}, {}),
		field_stage("cave", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[0].params.push_back({"amp_a", ve::ChannelType::kFloat, 6.0f});
	st[1].lipschitz_mode = ve::LipschitzMode::kMul; st[1].lipschitz = 1.0f;
	st[1].body = "void stage_cave(inout FieldCtx c){ c.sdf = P.hills_amp_a; }\n";

	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("hills_amp_a") != std::string::npos);
	CHECK(err.find("cave") != std::string::npos);
	CHECK(err.find("//!use") != std::string::npos);
}

TEST_CASE("a declared //!use accepts the same body") {
	std::vector<ve::StageManifest> st{
		field_stage("hills", {"sdf"}, {}),
		field_stage("cave", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[0].params.push_back({"amp_a", ve::ChannelType::kFloat, 6.0f});
	st[1].lipschitz_mode = ve::LipschitzMode::kMul; st[1].lipschitz = 1.0f;
	st[1].body = "void stage_cave(inout FieldCtx c){ c.sdf = P.hills_amp_a; }\n";
	st[1].uses.push_back("hills.amp_a");

	ve::ResolvedPipeline p;
	std::string err;
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
}

TEST_CASE("a //!use naming a param no stage declares is rejected") {
	std::vector<ve::StageManifest> st{field_stage("cave", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.0f;
	st[0].uses.push_back("hills.amp_a");
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("hills.amp_a") != std::string::npos);
}

TEST_CASE("a stage reads its own params without declaring anything") {
	std::vector<ve::StageManifest> st{field_stage("hills", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[0].params.push_back({"amp_a", ve::ChannelType::kFloat, 6.0f});
	st[0].body = "void stage_hills(inout FieldCtx c){ c.sdf = P.hills_amp_a; }\n";
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(1), st, &p, &err), err);
}

TEST_CASE("a param name inside a comment is not a read") {
	std::vector<ve::StageManifest> st{
		field_stage("hills", {"sdf"}, {}),
		field_stage("cave", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[0].params.push_back({"amp_a", ve::ChannelType::kFloat, 6.0f});
	st[1].lipschitz_mode = ve::LipschitzMode::kMul; st[1].lipschitz = 1.0f;
	st[1].body = "// once read P.hills_amp_a; it does not any more\n"
			"void stage_cave(inout FieldCtx c){ c.sdf = 1.0; }\n";
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
}

TEST_CASE("a GPU-only stage warns, naming the CPU consumers that will diverge") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {}, "")};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd;
	st[0].lipschitz = 1.0f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(1, true), st, &p, &err), err);
	CHECK_FALSE(p.cpu_exact);
	REQUIRE(p.warnings.size() == 1);
	CHECK(p.warnings[0].find("a") != std::string::npos);
	CHECK(p.warnings[0].find("collider") != std::string::npos);
	CHECK(p.warnings[0].find("island") != std::string::npos);
	CHECK(p.warnings[0].find("raycast") != std::string::npos);
}

TEST_CASE("a pipeline whose stages all have mirrors warns about nothing") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd;
	st[0].lipschitz = 1.0f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(1), st, &p, &err), err);
	CHECK(p.warnings.empty());
}
