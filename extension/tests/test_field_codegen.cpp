#include <doctest/doctest.h>
#include "terrain/field_codegen.h"
#include "terrain/pipeline.h"
#include "terrain/stage_manifest.h"
#include <locale>

namespace {

ve::ResolvedPipeline two_stage() {
	ve::StageManifest a;
	a.name = "hills"; a.kind = ve::StageKind::kField; a.cpu_symbol = "ve::stage_hills";
	a.writes.push_back({"sdf", ve::ChannelType::kFloat});
	a.writes.push_back({"steepness", ve::ChannelType::kFloat});
	a.params.push_back({"amplitude", ve::ChannelType::kFloat, 6.0f});
	a.body = "void stage_hills(inout FieldCtx ctx) { ctx.sdf = ctx.p.y; }\n";

	ve::StageManifest b;
	b.name = "bands"; b.kind = ve::StageKind::kField; b.cpu_symbol = "ve::stage_bands";
	b.reads.push_back({"steepness", ve::ChannelType::kFloat});
	b.writes.push_back({"material", ve::ChannelType::kUint});
	b.body = "void stage_bands(inout FieldCtx ctx) { ctx.material = 1u; }\n";

	ve::PipelineDesc d;
	d.stages.push_back({"a", {}});
	d.stages.push_back({"b", {}});
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE(ve::resolve_pipeline(d, {a, b}, &p, &err));
	return p;
}

} // namespace

TEST_CASE("generated source declares the context, the params and the composition") {
	const std::string g = ve::generate_field_glslh(two_stage(), "// PRELUDE MARKER\n");

	CHECK(g.find("struct FieldCtx") != std::string::npos);
	CHECK(g.find("vec3 p;") != std::string::npos);
	CHECK(g.find("float sdf;") != std::string::npos);
	CHECK(g.find("uint material;") != std::string::npos);
	CHECK(g.find("float steepness;") != std::string::npos);

	// Set 1: params UBO at binding 0, sector map at binding 1.
	CHECK(g.find("set = 1, binding = 0") != std::string::npos);
	CHECK(g.find("set = 1, binding = 1") != std::string::npos);
	CHECK(g.find("hills_amplitude") != std::string::npos);

	// Bodies verbatim, in pipeline order.
	const size_t hills = g.find("void stage_hills(inout FieldCtx ctx)");
	const size_t bands = g.find("void stage_bands(inout FieldCtx ctx)");
	CHECK(hills != std::string::npos);
	CHECK(bands != std::string::npos);
	CHECK(hills < bands);

	// Composition calls them in order and hands back sdf + material.
	const size_t base = g.find("void eval_base_field(");
	CHECK(base != std::string::npos);
	CHECK(g.find("stage_hills(ctx);", base) != std::string::npos);
	CHECK(g.find("stage_bands(ctx);", base) != std::string::npos);
	CHECK(g.find("stage_hills(ctx);", base) < g.find("stage_bands(ctx);", base));

	CHECK(g.find("// PRELUDE MARKER") != std::string::npos);
	CHECK(g.find("void base_field(") == std::string::npos);  // replaced, not kept
}

TEST_CASE("numeric codegen is independent of the process locale") {
	class Punct final : public std::numpunct<char> {
	protected:
		char do_decimal_point() const override { return ','; }
		char do_thousands_sep() const override { return '_'; }
		std::string do_grouping() const override { return "\3"; }
	};
	const std::locale original = std::locale();
	struct LocaleRestore {
		const std::locale &original;
		~LocaleRestore() { std::locale::global(original); }
	} restore{original};
	std::locale::global(std::locale(original, new Punct));

	ve::ResolvedPipeline p = two_stage();
	p.hash = 1234567890;
	p.lipschitz = 1.25f;
	p.resources.push_back({"sector.alpha", "texture2d_r32f", 1.5f});
	const std::string g = ve::generate_field_glslh(p, "");
	CHECK(g.find("// Pipeline hash: 1234567890\n") != std::string::npos);
	CHECK(g.find("// Lipschitz bound: 1.25\n") != std::string::npos);
	CHECK(g.find("const float sector_alpha_FALLBACK = 1.5;\n") != std::string::npos);
}

TEST_CASE("generation is deterministic") {
	const ve::ResolvedPipeline p = two_stage();
	CHECK(ve::generate_field_glslh(p, "x\n") == ve::generate_field_glslh(p, "x\n"));
}

TEST_CASE("resources land in set 1 from binding 2, in sorted order") {
	ve::ResolvedPipeline p = two_stage();
	p.resources.push_back({"sector.alpha", "texture2d_r32f", 0.0f});
	p.resources.push_back({"sector.beta", "texture2d_r32f", 1.5f});
	const std::string g = ve::generate_field_glslh(p, "");
	const size_t a = g.find("sector_alpha");
	const size_t b = g.find("sector_beta");
	CHECK(a != std::string::npos);
	CHECK(b != std::string::npos);
	CHECK(a < b);
	CHECK(g.find("set = 1, binding = 2") != std::string::npos);
	CHECK(g.find("set = 1, binding = 3") != std::string::npos);
}
