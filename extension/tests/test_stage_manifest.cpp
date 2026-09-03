#include <doctest/doctest.h>
#include "terrain/stage_manifest.h"

static const char *kHills =
	"//!stage     hills\n"
	"//!kind      field\n"
	"//!out       sdf : float\n"
	"//!param     amplitude : float = 6.0\n"
	"//!lipschitz 2.0\n"
	"//!cpu       ve::stage_hills\n"
	"\n"
	"void stage_hills(inout FieldCtx ctx) {\n"
	"\tctx.sdf = ctx.p.y;\n"
	"}\n";

TEST_CASE("parses every directive and keeps the body verbatim") {
	ve::StageManifest m;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_stage_manifest(kHills, &m, &err), err);
	CHECK(m.name == "hills");
	CHECK(m.kind == ve::StageKind::kField);
	REQUIRE(m.writes.size() == 1);
	CHECK(m.writes[0].name == "sdf");
	CHECK(m.writes[0].type == ve::ChannelType::kFloat);
	REQUIRE(m.params.size() == 1);
	CHECK(m.params[0].name == "amplitude");
	CHECK(m.params[0].value == doctest::Approx(6.0f));
	CHECK(m.lipschitz == doctest::Approx(2.0f));
	CHECK(m.cpu_symbol == "ve::stage_hills");
	CHECK(m.body.find("void stage_hills(inout FieldCtx ctx) {") != std::string::npos);
	CHECK(m.body.find("//!") == std::string::npos);
}

TEST_CASE("a stage with no //!cpu is GPU-only, not an error") {
	ve::StageManifest m;
	std::string err;
	std::string src = "//!stage s\n//!kind field\n//!out sdf : float\nvoid s(inout FieldCtx c){}\n";
	REQUIRE(ve::parse_stage_manifest(src, &m, &err));
	CHECK(m.cpu_symbol.empty());
}

TEST_CASE("reads, samples and map directives parse") {
	ve::StageManifest m;
	std::string err;
	std::string src =
		"//!stage   erosion\n"
		"//!kind    map\n"
		"//!domain  sector2d 256x256\n"
		"//!in      sector.height : image2d_r32f\n"
		"//!out     sector.flow : image2d_rg16f\n"
		"//!sample  world.mask : texture2d_r32f\n"
		"//!iterate 64\n"
		"//!bounds  12.5\n"
		"void erosion(){}\n";
	REQUIRE_MESSAGE(ve::parse_stage_manifest(src, &m, &err), err);
	CHECK(m.kind == ve::StageKind::kMap);
	CHECK(m.domain == "sector2d");
	CHECK(m.domain_w == 256);
	CHECK(m.domain_h == 256);
	CHECK(m.iterate == 64);
	CHECK(m.bounds == doctest::Approx(12.5f));
	REQUIRE(m.samples.size() == 1);
	CHECK(m.samples[0].name == "world.mask");
}

TEST_CASE("errors name the problem") {
	ve::StageManifest m;
	std::string err;
	CHECK_FALSE(ve::parse_stage_manifest("//!kind field\nvoid f(){}\n", &m, &err));
	CHECK(err.find("stage") != std::string::npos);

	err.clear();
	CHECK_FALSE(ve::parse_stage_manifest("//!stage s\n//!kind wat\n", &m, &err));
	CHECK(err.find("wat") != std::string::npos);

	err.clear();
	CHECK_FALSE(ve::parse_stage_manifest("//!stage s\n//!kind field\n//!out sdf : mat4\n", &m, &err));
	CHECK(err.find("mat4") != std::string::npos);

	err.clear();
	CHECK_FALSE(ve::parse_stage_manifest("//!stage s\n//!kind field\n//!bogus x\n", &m, &err));
	CHECK(err.find("bogus") != std::string::npos);
}
