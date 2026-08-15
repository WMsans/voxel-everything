#include <doctest/doctest.h>
#include "generator/generator.h"
#include <cmath>

static float hills(float x, float z) { // test oracle — mirrors implementation on purpose
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

TEST_CASE("surface point has sdf ~= 0, above is positive, below negative") {
	ve::AnalyticGenerator g;
	float x = 12.3f, z = -7.8f;
	float h = hills(x, z);
	CHECK(g.sample(x, ve::kSurfaceY + h, z).sdf == doctest::Approx(0.0f).epsilon(0.001));
	CHECK(g.sample(x, ve::kSurfaceY + h + 0.5f, z).sdf > 0.0f);
	CHECK(g.sample(x, ve::kSurfaceY + h - 0.5f, z).sdf < 0.0f);
}

TEST_CASE("cave carves empty space inside terrain") {
	ve::AnalyticGenerator g;
	float cy = ve::kSurfaceY + hills(30.0f, 30.0f) - 2.0f;
	CHECK(g.sample(30.0f, cy, 30.0f).sdf > 0.0f);   // cave center: air, though underground
	CHECK(g.sample(-30.0f, ve::kSurfaceY + hills(-30.0f, -30.0f) - 1.0f, -30.0f).sdf < 0.0f); // far from cave: solid
}

TEST_CASE("materials follow height bands; air has material 0") {
	ve::AnalyticGenerator g;
	CHECK(g.sample(0.0f, 100.0f, 0.0f).material == 0);
	float h = hills(1.0f, 1.0f);
	CHECK(g.sample(1.0f, ve::kSurfaceY + h - 0.01f, 1.0f).material != 0);
}

TEST_CASE("determinism: same input, identical output bits") {
	ve::AnalyticGenerator a, b;
	auto sa = a.sample(3.21f, 1.5f, -9.4f);
	auto sb = b.sample(3.21f, 1.5f, -9.4f);
	CHECK(sa.sdf == sb.sdf);
	CHECK(sa.material == sb.material);
}
