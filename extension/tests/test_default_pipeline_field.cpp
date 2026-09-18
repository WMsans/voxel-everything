// Characterization: pins the CPU field that assets/pipelines/default.pipeline produces.
// test_pipeline_equivalence.cpp pins golden.pipeline against ve::AnalyticGenerator, but
// default.pipeline gained the relief stage and that equivalence no longer holds for it --
// so without this file the shipped terrain has no CPU pin at all.
//
// Every value is compared as raw float BITS, not with a tolerance: a refactor that moves
// the field by one ulp has moved the world.
//
// Regenerate after an INTENTIONAL terrain change:  VE_REGEN_GOLDEN=1 ./build.sh --test
#include <doctest/doctest.h>
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_manifest.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Pt { float x, y, z; };

std::string root() { return std::string(VE_REPO_ROOT); }

std::string slurp(const std::string &p) {
	std::ifstream f(p);
	REQUIRE_MESSAGE(f.good(), "cannot open ", p);
	std::ostringstream o;
	o << f.rdbuf();
	return o.str();
}

std::unique_ptr<ve::PipelineFieldGenerator> default_pipeline() {
	ve::PipelineDesc d;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_pipeline_desc(
			slurp(root() + "/assets/pipelines/default.pipeline"), &d, &err), err);
	std::vector<ve::StageManifest> loaded;
	for (const ve::PipelineStageRef &r : d.stages) {
		ve::StageManifest m;
		REQUIRE_MESSAGE(ve::parse_stage_manifest(slurp(root() + "/shaders/" + r.path), &m, &err), err);
		loaded.push_back(m);
	}
	ve::ResolvedPipeline p;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, loaded, &p, &err), err);
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	return std::unique_ptr<ve::PipelineFieldGenerator>(g);
}

// Self-contained LCG: the corpus must not depend on any std:: RNG implementation. The
// ranges match tests/golden/field_baseline.txt so the two corpora cover the same ground,
// plus a far band where relief dominates and sin() range reduction is hardest.
std::vector<Pt> corpus() {
	std::vector<Pt> pts;
	uint32_t s = 20260917u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	for (int i = 0; i < 512; i++)
		pts.push_back({next(-20.0f, 60.0f), next(21.2f, 81.2f), next(-20.0f, 60.0f)});
	for (int i = 0; i < 256; i++)
		pts.push_back({next(700.0f, 900.0f), next(11.2f, 71.2f), next(700.0f, 900.0f)});
	for (int i = 0; i < 256; i++)
		pts.push_back({next(-3000.0f, 3000.0f), next(-200.0f, 400.0f), next(-3000.0f, 3000.0f)});
	return pts;
}

uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

std::string golden_path() { return root() + "/tests/golden/default_pipeline_field.txt"; }

} // namespace

TEST_CASE("the default pipeline's CPU field matches the committed corpus bit for bit") {
	auto g = default_pipeline();
	const std::vector<Pt> pts = corpus();

	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		FILE *f = std::fopen(golden_path().c_str(), "w");
		REQUIRE(f != nullptr);
		std::fprintf(f, "# assets/pipelines/default.pipeline CPU field. Columns: x y z sdf "
				"(hex float bits), material.\n");
		std::fprintf(f, "# Regenerate: VE_REGEN_GOLDEN=1 ./build.sh --test\n");
		for (const Pt &p : pts) {
			ve::Sample s = g->eval(p.x, p.y, p.z);
			std::fprintf(f, "%08x %08x %08x %08x %u\n", bits(p.x), bits(p.y), bits(p.z),
					bits(s.sdf), unsigned(s.material));
		}
		std::fclose(f);
		MESSAGE("regenerated " << golden_path());
	}

	FILE *f = std::fopen(golden_path().c_str(), "r");
	REQUIRE_MESSAGE(f != nullptr, "missing golden; run VE_REGEN_GOLDEN=1 ./build.sh --test");
	char line[256];
	size_t i = 0;
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		if (line[0] == '#' || line[0] == '\n') continue;
		unsigned bx, by, bz, bsdf, mat;
		REQUIRE(std::sscanf(line, "%x %x %x %x %u", &bx, &by, &bz, &bsdf, &mat) == 5);
		REQUIRE(i < pts.size());
		CHECK(bits(pts[i].x) == bx);
		CHECK(bits(pts[i].y) == by);
		CHECK(bits(pts[i].z) == bz);
		ve::Sample s = g->eval(pts[i].x, pts[i].y, pts[i].z);
		CHECK(bits(s.sdf) == bsdf);
		CHECK(unsigned(s.material) == mat);
		i++;
	}
	std::fclose(f);
	CHECK(i == pts.size());
}
