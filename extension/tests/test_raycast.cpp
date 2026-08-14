#include <doctest/doctest.h>
#include "world/raycast.h"
#include "world/brick_eval.h"
#include "generator/generator.h"
#include <cmath>

static ve::WorldBounds bounds() { return ve::WorldBounds{{0, -64, 0}, {64, 8, 64}}; }

TEST_CASE("a ray straight down from the sky lands on the surface") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 91.2f, 100.0f}; // 91.2 = 40 + kSurfaceY: well above the surface
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const ve::RayHit h = ve::raycast(gen, log, o, d, 200.0f);
	REQUIRE(h.hit);
	CHECK(h.pos[0] == doctest::Approx(100.0f));
	CHECK(h.pos[2] == doctest::Approx(100.0f));
	// The hit point is on the surface, to within the tracer's own tolerance.
	CHECK(std::fabs(ve::eval_field(gen, nullptr, 0, h.pos[0], h.pos[1], h.pos[2]).sdf) < 0.02f);
	// The surface faces up, so the normal has a strong +y component.
	CHECK(h.normal[1] > 0.5f);
	CHECK(h.distance == doctest::Approx(91.2f - h.pos[1]).epsilon(0.01));
}

TEST_CASE("a ray into the sky misses") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 91.2f, 100.0f}; // 91.2 = 40 + kSurfaceY: well above the surface
	const float d[3] = {0.0f, 1.0f, 0.0f};
	CHECK_FALSE(ve::raycast(gen, log, o, d, 200.0f).hit);
}

TEST_CASE("max_dist bounds the trace") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 91.2f, 100.0f}; // 91.2 = 40 + kSurfaceY: well above the surface
	const float d[3] = {0.0f, -1.0f, 0.0f};
	CHECK_FALSE(ve::raycast(gen, log, o, d, 5.0f).hit);
	CHECK(ve::raycast(gen, log, o, d, 100.0f).hit);
}

TEST_CASE("the trace sees edits: a crater moves the hit point down") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 91.2f, 100.0f}; // 91.2 = 40 + kSurfaceY: well above the surface
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const ve::RayHit before = ve::raycast(gen, log, o, d, 200.0f);
	REQUIRE(before.hit);

	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 100.0f; op.pos[1] = before.pos[1]; op.pos[2] = 100.0f;
	op.radius = 4.0f;
	REQUIRE_FALSE(log.append(op).touched.empty());

	const ve::RayHit after = ve::raycast(gen, log, o, d, 200.0f);
	REQUIRE(after.hit);
	CHECK(after.pos[1] < before.pos[1] - 3.0f); // fell through the crater
}

TEST_CASE("a ray that starts inside solid reports a hit at its origin") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, -30.0f, 100.0f}; // well underground
	const float d[3] = {0.0f, 1.0f, 0.0f};
	const ve::RayHit h = ve::raycast(gen, log, o, d, 100.0f);
	REQUIRE(h.hit);
	CHECK(h.distance == doctest::Approx(0.0f));
}

TEST_CASE("the direction is normalised for the caller") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 91.2f, 100.0f}; // 91.2 = 40 + kSurfaceY: well above the surface
	const float unit[3] = {0.0f, -1.0f, 0.0f};
	const float scaled[3] = {0.0f, -7.5f, 0.0f};
	const ve::RayHit a = ve::raycast(gen, log, o, unit, 200.0f);
	const ve::RayHit b = ve::raycast(gen, log, o, scaled, 200.0f);
	REQUIRE(a.hit);
	REQUIRE(b.hit);
	CHECK(b.pos[1] == doctest::Approx(a.pos[1]).epsilon(0.001));
	CHECK(b.distance == doctest::Approx(a.distance).epsilon(0.001));
}
