// The loader against a fake reader: no filesystem, so the failure paths are reachable.
#include <doctest/doctest.h>
#include "terrain/pipeline_load.h"
#include <map>
#include <string>

namespace {

// A reader over an in-memory file table. Returns false for anything not in the table,
// which is how the "cannot read" paths get exercised.
ve::TextReader table_reader(const std::map<std::string, std::string> &files) {
	return [&files](const std::string &path, std::string *out) {
		auto it = files.find(path);
		if (it == files.end()) return false;
		*out = it->second;
		return true;
	};
}

const char *kHills =
		"//!stage     hills\n"
		"//!kind      field\n"
		"//!out       sdf : float\n"
		"//!param     amp : float = 6.0\n"
		"//!lipschitz add 2.0\n"
		"//!cpu       ve::stage_hills\n"
		"void stage_hills(inout FieldCtx ctx) { ctx.sdf = ctx.p.y; }\n";

} // namespace

TEST_CASE("load_pipeline reads the pipeline and every stage it names, then resolves") {
	const std::map<std::string, std::string> files{
		{"pipe/a.pipeline", "seed 7\nstage stages/hills.field.glslh\n"},
		{"root/stages/hills.field.glslh", kHills},
	};
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &p, &err), err);
	REQUIRE(p.stages.size() == 1);
	CHECK(p.stages[0].name == "hills");
	CHECK(p.channel_slot("sdf") == 1);
	REQUIRE(p.params.size() == 1);
	CHECK(p.params[0].name == "hills.amp");
}

TEST_CASE("load_pipeline reports an unreadable pipeline file") {
	ve::ResolvedPipeline p;
	std::string err;
	const std::map<std::string, std::string> files{};
	CHECK_FALSE(ve::load_pipeline(table_reader(files), "pipe/missing.pipeline", "root/", &p, &err));
	CHECK(err.find("pipe/missing.pipeline") != std::string::npos);
}

TEST_CASE("load_pipeline reports an unreadable stage, naming the resolved path") {
	const std::map<std::string, std::string> files{
		{"pipe/a.pipeline", "stage stages/absent.field.glslh\n"},
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &p, &err));
	CHECK(err.find("root/stages/absent.field.glslh") != std::string::npos);
}

TEST_CASE("load_pipeline prefixes a manifest parse error with the stage path") {
	const std::map<std::string, std::string> files{
		{"pipe/a.pipeline", "stage stages/bad.field.glslh\n"},
		{"root/stages/bad.field.glslh", "//!stage bad\n//!kind field\n//!nonsense x\n"},
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &p, &err));
	CHECK(err.find("root/stages/bad.field.glslh") != std::string::npos);
	CHECK(err.find("nonsense") != std::string::npos);
}
