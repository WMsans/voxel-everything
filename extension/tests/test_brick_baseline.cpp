// Characterization: pins whole-brick output (SDF lattice, materials, palette, mips) for a
// fixed set of bricks. Task 1 pins the field as a function; this pins everything eval_brick
// derives from it, which is what actually reaches the atlas.
//
// Regenerate after an INTENTIONAL terrain change:  VE_REGEN_GOLDEN=1 ./build.sh --test
#include <doctest/doctest.h>
#include "world/brick_eval.h"
#include "generator/generator.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Bricks are 0.8 m; these straddle the surface at y = 51.2 m, the carved cave at (30,~50,30),
// deep solid ground, and open sky -- one brick per distinct field regime.
const ve::IVec3 kBricks[] = {
	{0, 64, 0}, {15, 63, 15}, {37, 63, 37}, {37, 62, 37},
	{-25, 60, -25}, {0, 40, 0}, {0, 90, 0}, {1000, 64, 1000},
};

// FNV-1a over every byte a brick contributes to the atlas.
uint64_t brick_hash(const ve::BrickEval &e) {
	uint64_t h = 1469598103934665603ull;
	auto feed = [&h](const void *p, size_t n) {
		const unsigned char *b = static_cast<const unsigned char *>(p);
		for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
	};
	feed(e.brick.sdf, sizeof(e.brick.sdf));
	feed(e.brick.mat, sizeof(e.brick.mat));
	feed(e.brick.palette, sizeof(e.brick.palette));
	feed(&e.mips, sizeof(e.mips));
	return h;
}

std::string golden_path() { return std::string(VE_REPO_ROOT) + "/tests/golden/brick_baseline.txt"; }

} // namespace

TEST_CASE("eval_brick output matches the committed baseline") {
	ve::AnalyticGenerator g;
	const int n = int(sizeof(kBricks) / sizeof(kBricks[0]));
	std::vector<uint64_t> got;
	for (int i = 0; i < n; i++) {
		ve::BrickEval e{};
		ve::eval_brick(g, nullptr, 0, kBricks[i], &e);
		got.push_back(brick_hash(e));
	}

	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		FILE *f = std::fopen(golden_path().c_str(), "w");
		REQUIRE(f != nullptr);
		std::fprintf(f, "# ve::eval_brick baseline. Columns: bx by bz fnv1a-64.\n");
		std::fprintf(f, "# Regenerate: VE_REGEN_GOLDEN=1 ./build.sh --test\n");
		for (int i = 0; i < n; i++)
			std::fprintf(f, "%d %d %d %016llx\n", kBricks[i].x, kBricks[i].y, kBricks[i].z,
					(unsigned long long)got[size_t(i)]);
		std::fclose(f);
		MESSAGE("regenerated " << golden_path());
	}

	FILE *f = std::fopen(golden_path().c_str(), "r");
	REQUIRE_MESSAGE(f != nullptr, "missing golden; run VE_REGEN_GOLDEN=1 ./build.sh --test");
	char line[256];
	int i = 0;
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		if (line[0] == '#' || line[0] == '\n') continue;
		int bx, by, bz;
		unsigned long long h;
		REQUIRE(std::sscanf(line, "%d %d %d %llx", &bx, &by, &bz, &h) == 4);
		REQUIRE(i < n);
		CHECK(bx == kBricks[i].x);
		CHECK(by == kBricks[i].y);
		CHECK(bz == kBricks[i].z);
		CHECK(got[size_t(i)] == uint64_t(h));
		i++;
	}
	std::fclose(f);
	CHECK(i == n);
}
