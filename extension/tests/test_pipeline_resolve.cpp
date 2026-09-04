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

TEST_CASE("resources sort by name and lipschitz combines multiplicatively") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].samples.push_back({"sector.z", "texture2d_r32f", 0.0f});
	st[0].samples.push_back({"sector.a", "texture2d_r32f", 0.0f});
	st[0].lipschitz = 1.5f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(1), st, &p, &err), err);
	REQUIRE(p.resources.size() == 2);
	CHECK(p.resources[0].name == "sector.a");
	CHECK(p.resources[1].name == "sector.z");
	CHECK(p.lipschitz == doctest::Approx(1.5f));
}

TEST_CASE("param overrides win, and the hash moves when they do") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].params.push_back({"amplitude", ve::ChannelType::kFloat, 6.0f});
	ve::ResolvedPipeline p1, p2;
	std::string err;
	REQUIRE(ve::resolve_pipeline(desc_for(1), st, &p1, &err));
	CHECK(p1.params[0].value == doctest::Approx(6.0f));
	ve::PipelineDesc d = desc_for(1);
	d.stages[0].param_overrides.emplace_back("amplitude", 9.0f);
	REQUIRE(ve::resolve_pipeline(d, st, &p2, &err));
	CHECK(p2.params[0].value == doctest::Approx(9.0f));
	CHECK(p1.hash != p2.hash);
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
