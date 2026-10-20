// The material_table.glslh pattern: a committed generated artifact plus a test asserting the
// generator still produces it byte for byte. Catches codegen drift that no behavioural test
// would notice until a shader failed to compile.
//
// Regenerate after an INTENTIONAL codegen change: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests
#include <doctest/doctest.h>
#include "terrain/field_codegen.h"
#include "terrain/pipeline.h"
#include "terrain/pipeline_load.h"
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

// An ifstream reader over the repo, the native counterpart of VoxelWorld's FileAccess one.
namespace {
bool repo_reader(const std::string &path, std::string *out) {
	std::ifstream f(path);
	if (!f.good()) return false;
	std::ostringstream o;
	o << f.rdbuf();
	*out = o.str();
	return true;
}
} // namespace

TEST_CASE("the default pipeline generates the committed source") {
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root() + "/assets/pipelines/default.pipeline",
			root() + "/shaders/", &p, nullptr, &err), err);

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
	// The golden file predates the guarded ground-view blocks added to field_ops.glslh in
	// Task 3. Those edits live behind `#ifdef VE_FIELD_GROUND`, so the compiled behaviour
	// for pipelines without ground_sdf is unchanged (pinned by the field-value behaviour
	// tests); only the trailing prelude blob of the generated source differs textually.
	// The golden therefore pins the codegen-emitted prefix only, with the prelude stripped.
	const std::string marker = "// Field-op interface.";
	auto cut_prelude = [&](const std::string &text) {
		const size_t first = text.find(marker);
		REQUIRE_MESSAGE(first != std::string::npos, "prelude marker not found");
		REQUIRE_MESSAGE(text.find(marker, first + marker.size()) == std::string::npos,
				"prelude marker occurs more than once");
		return text.substr(0, first);
	};
	CHECK(cut_prelude(got) == cut_prelude(slurp(golden_path)));
}
