// Characterization: pins ve::AnalyticGenerator's exact output before the terrain pipeline
// replaces it. Every value is compared as raw float BITS, not with a tolerance -- the point
// is that the ported pipeline reproduces today's terrain exactly, not approximately.
//
// Regenerate after an INTENTIONAL terrain change:  VE_REGEN_GOLDEN=1 ./build.sh --test
#include <doctest/doctest.h>
#include "generator/generator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Pt { float x, y, z; };

// Self-contained LCG: the corpus must not depend on any std:: RNG implementation.
std::vector<Pt> corpus() {
	std::vector<Pt> pts;
	uint32_t s = 20260903u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	for (int i = 0; i < 512; i++)
		pts.push_back({next(-20.0f, 60.0f), next(21.2f, 81.2f), next(-20.0f, 60.0f)});
	for (int i = 0; i < 128; i++)
		pts.push_back({next(700.0f, 900.0f), next(11.2f, 71.2f), next(700.0f, 900.0f)});
	return pts;
}

uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

std::string golden_path() { return std::string(VE_REPO_ROOT) + "/tests/golden/field_baseline.txt"; }

} // namespace

TEST_CASE("analytic field matches the committed baseline bit for bit") {
	ve::AnalyticGenerator g;
	const std::vector<Pt> pts = corpus();

	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		FILE *f = std::fopen(golden_path().c_str(), "w");
		REQUIRE(f != nullptr);
		std::fprintf(f, "# ve::AnalyticGenerator baseline. Columns: x y z sdf (hex float bits), material.\n");
		std::fprintf(f, "# Regenerate: VE_REGEN_GOLDEN=1 ./build.sh --test\n");
		for (const Pt &p : pts) {
			ve::Sample s = g.sample(p.x, p.y, p.z);
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
		ve::Sample s = g.sample(pts[i].x, pts[i].y, pts[i].z);
		CHECK(bits(s.sdf) == bsdf);
		CHECK(unsigned(s.material) == mat);
		i++;
	}
	std::fclose(f);
	CHECK(i == pts.size());
}
