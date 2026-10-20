#include <doctest/doctest.h>
#include "terrain/field_codegen.h"
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_manifest.h"
#include "terrain/pipeline_load.h"
#include "world/material_table.h"
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace {

std::string root() { return std::string(VE_REPO_ROOT); }

std::string slurp(const std::string &path) {
	std::ifstream f(path);
	REQUIRE_MESSAGE(f.good(), "cannot open ", path);
	std::ostringstream o;
	o << f.rdbuf();
	return o.str();
}

bool repo_reader(const std::string &path, std::string *out) {
	std::ifstream f(path);
	if (!f.good()) return false;
	std::ostringstream o;
	o << f.rdbuf();
	*out = o.str();
	return true;
}

static bool g_sloped = false;

VE_STAGE_SLOTS(Ground, p, sdf, material);
VE_STAGE_PARAMS(Ground);

void stage_ground(ve::FieldCtx &ctx, const GroundSlots &s, const GroundParams &,
		const ve::FieldResources &) {
	float ground_y = 40.0f + (g_sloped ? 0.5f * ctx.v(s.p)[0] : 0.0f);
	ctx.f(s.sdf) = ctx.v(s.p)[1] - ground_y;
	ctx.f(s.material) = 1.0f;
}

VE_STAGE_SLOTS(Water, p, sdf, material, ground_sdf, ground_material, liquid_level);
VE_STAGE_PARAMS(Water);

void stage_water(ve::FieldCtx &ctx, const WaterSlots &s, const WaterParams &,
		const ve::FieldResources &) {
	ctx.f(s.ground_sdf) = ctx.f(s.sdf);
	ctx.f(s.ground_material) = ctx.f(s.material);
	ctx.f(s.liquid_level) = 50.0f;
	float w = ctx.v(s.p)[1] - 50.0f;
	if (w < ctx.f(s.sdf)) {
		ctx.f(s.sdf) = w;
		if (ctx.f(s.ground_sdf) > 0.0f)
			ctx.f(s.material) = float(ve::material_id("water"));
	}
}

ve::ResolvedPipeline build_pipeline(bool sloped = false) {
	g_sloped = sloped;

	ve::StageManifest g;
	g.name = "ground"; g.kind = ve::StageKind::kField; g.cpu_symbol = "ve::stage_ground";
	g.writes.push_back({"sdf", ve::ChannelType::kFloat});
	g.writes.push_back({"material", ve::ChannelType::kUint});
	g.lipschitz_mode = ve::LipschitzMode::kAdd;
	g.lipschitz = 1.0f;
	g.body = "void stage_ground(inout FieldCtx ctx) { ctx.sdf = ctx.p.y - 40.0; ctx.material = 1u; }\n";

	ve::StageManifest w;
	w.name = "water"; w.kind = ve::StageKind::kField; w.cpu_symbol = "ve::stage_water";
	w.reads.push_back({"sdf", ve::ChannelType::kFloat});
	w.reads.push_back({"material", ve::ChannelType::kUint});
	w.writes.push_back({"sdf", ve::ChannelType::kFloat});
	w.writes.push_back({"material", ve::ChannelType::kUint});
	w.writes.push_back({"ground_sdf", ve::ChannelType::kFloat});
	w.writes.push_back({"ground_material", ve::ChannelType::kUint});
	w.writes.push_back({"liquid_level", ve::ChannelType::kFloat});
	w.lipschitz_mode = ve::LipschitzMode::kMul;
	w.lipschitz = 1.0f;
	w.body = "void stage_water(inout FieldCtx ctx) { ctx.ground_sdf = ctx.sdf; ctx.ground_material = ctx.material; ctx.liquid_level = 50.0; float ww = ctx.p.y - 50.0; if (ww < ctx.sdf) { ctx.sdf = ww; if (ctx.ground_sdf > 0.0) ctx.material = 9u; } }\n";

	ve::PipelineDesc d;
	d.stages.push_back({"a", {}});
	d.stages.push_back({"b", {}});
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, {g, w}, &p, &err), err);
	return p;
}

} // namespace

VE_REGISTER_STAGE("ve::stage_ground", Ground, stage_ground);
VE_REGISTER_STAGE("ve::stage_water", Water, stage_water);

TEST_CASE("a pipeline without ground_sdf generates no views") {
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root() + "/assets/pipelines/default.pipeline",
			root() + "/shaders/", &p, nullptr, &err), err);
	const std::string prelude = slurp(root() + "/shaders/field_ops.glslh");
	const std::string got = ve::generate_field_glslh(p, prelude);
	CHECK(got.find("VE_FIELD_GROUND") == std::string::npos);
	CHECK(got == slurp(root() + "/shaders/generated/field.glslh.golden"));
}

TEST_CASE("a pipeline with ground_sdf emits eval_base_field_views") {
	ve::ResolvedPipeline p = build_pipeline(false);
	const std::string src = ve::generate_field_glslh(p, "");
	CHECK(src.find("#define VE_FIELD_GROUND 1") != std::string::npos);
	CHECK(src.find("void eval_base_field_views(") != std::string::npos);
}

TEST_CASE("sample_ground and liquid_level") {
	g_sloped = false;
	std::string err;
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(build_pipeline(false), &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	CHECK(g->sample(0.0f, 45.0f, 0.0f).material == ve::material_id("water"));
	CHECK(g->sample_ground(0.0f, 45.0f, 0.0f).sdf == doctest::Approx(5.0f));
	float y = 0.0f;
	CHECK(g->liquid_level(0.0f, 0.0f, &y));
	CHECK(y == doctest::Approx(50.0f));
	ve::FieldSample fs = g->sample_gradient(0.0f, 45.0f, 0.0f);
	CHECK(fs.gradient[0] == doctest::Approx(0.0f).epsilon(0.01));
	CHECK(fs.gradient[1] == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(fs.gradient[2] == doctest::Approx(0.0f).epsilon(0.01));
	CHECK(fs.sdf == doctest::Approx(5.0f));
	delete g;

	g_sloped = true;
	ve::PipelineFieldGenerator *g2 = ve::PipelineFieldGenerator::create(build_pipeline(true), &err);
	REQUIRE_MESSAGE(g2 != nullptr, err);
	ve::FieldSample fs2 = g2->sample_gradient(0.0f, 45.0f, 0.0f);
	CHECK(fs2.gradient[0] == doctest::Approx(-0.5f).epsilon(0.01));
	CHECK(fs2.gradient[1] == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(fs2.gradient[2] == doctest::Approx(0.0f).epsilon(0.01));
	delete g2;
}