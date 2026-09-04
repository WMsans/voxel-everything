// The material_table.glslh pattern: a committed generated artifact plus a test asserting the
// generator still produces it byte for byte. Catches codegen drift that no behavioural test
// would notice until a shader failed to compile.
//
// Regenerate after an INTENTIONAL codegen change: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests
#include <doctest/doctest.h>
#include "terrain/field_codegen.h"
#include "terrain/pipeline.h"
#include "terrain/stage_manifest.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

} // namespace

TEST_CASE("the default pipeline generates the committed source") {
	ve::PipelineDesc d;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_pipeline_desc(
			slurp(root() + "/assets/pipelines/default.pipeline"), &d, &err), err);

	std::vector<ve::StageManifest> loaded;
	for (const ve::PipelineStageRef &r : d.stages) {
		ve::StageManifest m;
		REQUIRE_MESSAGE(ve::parse_stage_manifest(
				slurp(root() + "/shaders/" + r.path), &m, &err), err);
		loaded.push_back(m);
	}

	ve::ResolvedPipeline p;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, loaded, &p, &err), err);

	const std::string prelude = slurp(root() + "/shaders/field_ops.glslh");
	const std::string got = ve::generate_field_glslh(p, prelude);
	const std::string golden_path = root() + "/shaders/generated/field.glslh.golden";

	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		std::ofstream out(golden_path);
		REQUIRE(out.good());
		out << got;
		out.close();
		MESSAGE("regenerated " << golden_path);
	}
	CHECK(got == slurp(golden_path));
}
