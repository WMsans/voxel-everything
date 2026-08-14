#include <doctest/doctest.h>
#include "world/edit_log.h"

static ve::EditOp sphere(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

static ve::WorldBounds bounds() { return ve::WorldBounds{{0, 0, 0}, {64, 8, 64}}; }

TEST_CASE("an op lands in every region it touches, and only those") {
	ve::EditLog log(bounds());
	// Region 0 spans [0, 25.6) m. A 2 m sphere at 25.0 m straddles regions 0 and 1 on x.
	const auto r = log.append(sphere(25.0f, 1.0f, 1.0f, 2.0f));
	CHECK(r.rejected.empty());
	CHECK(r.touched.size() == 2);
	CHECK(log.op_count({0, 0, 0}) == 1);
	CHECK(log.op_count({1, 0, 0}) == 1);
	CHECK(log.op_count({2, 0, 0}) == 0);
	CHECK(log.region_count() == 2);
}

TEST_CASE("ops outside the world bounds are dropped, not stored") {
	ve::EditLog log(bounds());
	const auto r = log.append(sphere(-100.0f, 0.0f, 0.0f, 1.0f));
	CHECK(r.touched.empty());
	CHECK(r.rejected.empty());
	CHECK(log.region_count() == 0);
}

TEST_CASE("a partially out-of-bounds op keeps only its in-bounds regions") {
	ve::EditLog log(bounds());
	const auto r = log.append(sphere(0.5f, 0.5f, 0.5f, 2.0f)); // straddles x = 0 corner
	CHECK(r.touched.size() == 1);
	CHECK(r.touched[0] == ve::IVec3{0, 0, 0});
	CHECK(log.op_count({-1, 0, 0}) == 0);
}

TEST_CASE("ops are preserved in append order") {
	ve::EditLog log(bounds());
	for (int i = 0; i < 5; i++) {
		ve::EditOp op = sphere(1.0f, 1.0f, 1.0f, 1.0f);
		op.material = static_cast<uint32_t>(i);
		log.append(op);
	}
	const auto &ops = log.ops({0, 0, 0});
	REQUIRE(ops.size() == 5);
	for (int i = 0; i < 5; i++) CHECK(ops[i].material == static_cast<uint32_t>(i));
}

TEST_CASE("a full region rejects further ops and reports which region overflowed") {
	ve::EditLog log(bounds());
	for (int i = 0; i < ve::kMaxRegionOps; i++) {
		const auto r = log.append(sphere(1.0f, 1.0f, 1.0f, 0.5f));
		CHECK(r.rejected.empty());
	}
	CHECK(log.op_count({0, 0, 0}) == ve::kMaxRegionOps);
	const auto r = log.append(sphere(1.0f, 1.0f, 1.0f, 0.5f));
	CHECK(r.touched.empty());
	REQUIRE(r.rejected.size() == 1);
	CHECK(r.rejected[0] == ve::IVec3{0, 0, 0});
	CHECK(log.op_count({0, 0, 0}) == ve::kMaxRegionOps); // unchanged: fail-soft, no eviction
}

TEST_CASE("overflow in one region does not block the op's other regions") {
	ve::EditLog log(bounds());
	for (int i = 0; i < ve::kMaxRegionOps; i++) log.append(sphere(1.0f, 1.0f, 1.0f, 0.5f));
	const auto r = log.append(sphere(25.0f, 1.0f, 1.0f, 2.0f)); // regions 0 and 1
	CHECK(r.touched.size() == 1);
	CHECK(r.touched[0] == ve::IVec3{1, 0, 0});
	CHECK(r.rejected.size() == 1);
	CHECK(r.rejected[0] == ve::IVec3{0, 0, 0});
}

TEST_CASE("clear drops everything") {
	ve::EditLog log(bounds());
	log.append(sphere(1.0f, 1.0f, 1.0f, 1.0f));
	log.clear();
	CHECK(log.region_count() == 0);
	CHECK(log.op_count({0, 0, 0}) == 0);
	CHECK(log.ops({0, 0, 0}).empty());
}
