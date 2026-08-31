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
	// op.radius is the carve's exact geometric reach -- no per-sample scaling -- so the
	// crater floor sits a full radius below its centre whatever bands it crossed.
	CHECK(after.pos[1] < before.pos[1] - 3.5f); // fell through the crater
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

TEST_CASE("the hit reports the material of the surface it struck") {
	// The removal tools resolve hardness from this id, so "air at an ordinary surface" is
	// not an acceptable answer: the tracer's hit tolerance can legitimately stop just
	// OUTSIDE the surface, where the field carries no material, and the probe below the
	// normal is what recovers it.
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 91.2f, 100.0f}; // 91.2 = 40 + kSurfaceY: well above the surface
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const ve::RayHit h = ve::raycast(gen, log, o, d, 200.0f);
	REQUIRE(h.hit);
	CHECK(h.material != 0);
	// It is the material of the solid under the hit, not some neighbouring band.
	const ve::Sample inside = ve::eval_field(gen, nullptr, 0, h.pos[0], h.pos[1] - 0.1f, h.pos[2]);
	REQUIRE(inside.sdf < 0.0f);
	CHECK(h.material == inside.material);
}

TEST_CASE("a thin shell reports the shell's material, not the air behind it") {
	// A single voxel-scale offset below the hit point steps clean through a thin wall and
	// reports the hollow. The probe walks out in quarter-voxel steps and takes the FIRST
	// solid it meets, so an 8 cm shell answers with its own material.
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float c[3] = {100.0f, 81.2f, 100.0f}; // 30 m of clear air above the surface
	ve::EditOp shell{};
	shell.type = ve::kOpSphereAdd;
	shell.material = 4;
	shell.pos[0] = c[0]; shell.pos[1] = c[1]; shell.pos[2] = c[2];
	shell.radius = 1.0f;
	REQUIRE_FALSE(log.append(shell).touched.empty());
	ve::EditOp hollow{};
	hollow.type = ve::kOpSphereSubtract;
	hollow.pos[0] = c[0]; hollow.pos[1] = c[1]; hollow.pos[2] = c[2];
	hollow.radius = 0.92f; // an 8 cm wall
	REQUIRE_FALSE(log.append(hollow).touched.empty());

	const float o[3] = {100.0f, 91.2f, 100.0f};
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const ve::RayHit h = ve::raycast(gen, log, o, d, 200.0f);
	REQUIRE(h.hit);
	// The shell's crown, not the terrain 30 m below it.
	REQUIRE(h.pos[1] == doctest::Approx(c[1] + 1.0f).epsilon(0.02));
	CHECK(h.material == 4);
}
