#include <doctest/doctest.h>
#include "generator/generator.h" // kSurfaceY
#include "terrain/field_codegen.h"
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_manifest.h"
#include <memory>

namespace {
const char *kMap = R"(//!stage m
//!kind map
//!domain sector2d 256x256
//!out sector.terrain : image2d_rg16
//!param amp : float = 2.0
vec2 stage_m(vec2 xz) { return vec2(0.5 + 0.0 * P.m_amp, 0.5); }
)";
// Borrows ve::stage_flat as its CPU mirror: this test is about resolution, not shape.
const char *kField = R"(//!stage flat
//!kind field
//!sample sector.terrain : texture2d_rg16
//!out sdf : float
//!out height : float
//!param level : float = 2.0
//!lipschitz add 1.0
//!cpu ve::stage_flat
void stage_flat(inout FieldCtx ctx) { ctx.height = P.flat_level; ctx.sdf = ctx.p.y - SURFACE_Y - ctx.height; }
)";

ve::StageManifest parse(const std::string &src) {
	ve::StageManifest m;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_stage_manifest(src, &m, &err), err);
	return m;
}

bool resolve(std::vector<ve::StageManifest> st, ve::ResolvedPipeline *out, std::string *err) {
	ve::PipelineDesc d;
	for (size_t i = 0; i < st.size(); i++) d.stages.push_back({"s" + std::to_string(i), {}});
	return ve::resolve_pipeline(d, st, out, err);
}

std::string with(const std::string &src, const std::string &from, const std::string &to) {
	std::string s = src;
	s.replace(s.find(from), from.size(), to);
	return s;
}
} // namespace

TEST_CASE("a dotted //!out is recorded as a map write") {
	const ve::StageManifest m = parse(kMap);
	REQUIRE(m.map_writes.size() == 1);
	CHECK(m.map_writes[0].name == "sector.terrain");
	CHECK(m.map_writes[0].type == "image2d_rg16");
	CHECK(m.writes.empty());
}

TEST_CASE("a map stage ahead of a sampling field stage resolves") {
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(resolve({parse(kMap), parse(kField)}, &p, &err), err);
	CHECK(p.map_stage == 0);
	CHECK(p.cpu_exact); // a map stage has no //!cpu and needs none
	REQUIRE(p.resources.size() == 1);
	CHECK(p.resources[0].name == "sector.terrain");
	bool has_amp = false;
	for (const ve::ParamDecl &pd : p.params) has_amp |= pd.name == "m.amp";
	CHECK(has_amp);
	CHECK(p.lipschitz == doctest::Approx(1.0f));
}

TEST_CASE("map-stage rules are enforced with the stage named") {
	ve::ResolvedPipeline p;
	std::string err;
	SUBCASE("after a field stage") {
		CHECK_FALSE(resolve({parse(kField), parse(kMap)}, &p, &err));
		CHECK(err.find("'m'") != std::string::npos);
	}
	SUBCASE("a second map stage") {
		CHECK_FALSE(resolve({parse(kMap), parse(with(kMap, "//!stage m", "//!stage m2")), parse(kField)}, &p, &err));
		CHECK(err.find("m2") != std::string::npos);
	}
	SUBCASE("a domain other than sector2d 256x256") {
		CHECK_FALSE(resolve({parse(with(kMap, "256x256", "128x128")), parse(kField)}, &p, &err));
	}
	SUBCASE("no sector write") {
		CHECK_FALSE(resolve({parse(with(kMap, "//!out sector.terrain : image2d_rg16\n", "")), parse(kField)}, &p, &err));
	}
	SUBCASE("a field channel on a map stage") {
		CHECK_FALSE(resolve({parse(with(kMap, "//!param", "//!out height : float\n//!param")), parse(kField)}, &p, &err));
	}
	SUBCASE("a sampled resource no map stage writes") {
		CHECK_FALSE(resolve({parse(kField)}, &p, &err));
		CHECK(err.find("sector.terrain") != std::string::npos);
	}
	SUBCASE("a format mismatch") {
		CHECK_FALSE(resolve({parse(kMap), parse(with(kField, "texture2d_rg16", "texture2d_r32f"))}, &p, &err));
	}
	SUBCASE("an invalid resource type") {
		CHECK_FALSE(resolve({parse(with(kMap, "image2d_rg16", "bogus2d_rg16")), parse(kField)}, &p, &err));
	}
	SUBCASE("a 3D resource type") {
		CHECK_FALSE(resolve({parse(with(kMap, "image2d_rg16", "texture3d_rg16")), parse(kField)}, &p, &err));
	}
	SUBCASE("a field stage dotted output") {
		CHECK_FALSE(resolve({parse(kMap), parse(with(kField, "//!out sdf : float", "//!out sector.extra : image2d_rg16\n//!out sdf : float"))}, &p, &err));
	}
	SUBCASE("a map output nothing samples") {
		CHECK_FALSE(resolve({parse(kMap), parse(with(kField, "//!sample sector.terrain : texture2d_rg16\n", ""))}, &p, &err));
	}
}

TEST_CASE("codegen calls only field stages and exposes the map stage to the bake") {
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE(resolve({parse(kMap), parse(kField)}, &p, &err));
	const std::string src = ve::generate_field_glslh(p, "");
	CHECK(src.find("\tstage_m(ctx);") == std::string::npos);
	CHECK(src.find("\tstage_flat(ctx);") != std::string::npos);
	CHECK(src.find("#define VE_SECTOR_MAP 1") != std::string::npos);
	CHECK(src.find("vec2 ve_sector_map(vec2 xz) { return stage_m(xz); }") != std::string::npos);
}

TEST_CASE("the CPU generator skips the map stage and carries the cache") {
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE(resolve({parse(kMap), parse(kField)}, &p, &err));
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	REQUIRE_MESSAGE(g != nullptr, err);
	g->set_sector_cache(std::make_shared<ve::SectorCache>(1000.0f, 0.0f, 0.0f));
	CHECK(g->sample(0.0f, 100.0f, 0.0f).sdf == doctest::Approx(100.0f - ve::kSurfaceY - 2.0f));
}
