#include <doctest/doctest.h>
#include "world/edit_log.h"
#include <cstring>

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

TEST_CASE("collect_ops_for_aabb gathers ops from every region a component straddles") {
	ve::EditLog log(bounds());
	// The region boundary is at 25.6 m (brick 32). A two-cell component covering bricks
	// 31..32 spans [24.8, 26.4) m: it is smaller than a region, but its op list must come
	// from both regions.
	log.append(sphere(24.8f, 1.0f, 1.0f, 0.5f)); // region 0 only, intersects
	log.append(sphere(26.2f, 1.0f, 1.0f, 0.1f)); // region 1 only, intersects
	log.append(sphere(25.6f, 1.0f, 1.0f, 0.5f)); // straddles: stored in both, one global seq
	log.append(sphere(30.0f, 1.0f, 1.0f, 0.2f)); // region 1 only, does not intersect
	const float lo[3] = {24.8f, 0.0f, 0.0f};
	const float hi[3] = {26.4f, 2.0f, 2.0f};
	std::vector<ve::EditOp> ops;
	ve::collect_ops_for_aabb(log, lo, hi, &ops);
	REQUIRE(ops.size() == 3);
	CHECK(std::any_of(ops.begin(), ops.end(),
			[](const ve::EditOp &op) { return op.pos[0] == doctest::Approx(24.8f); }));
	CHECK(std::any_of(ops.begin(), ops.end(),
			[](const ve::EditOp &op) { return op.pos[0] == doctest::Approx(25.6f); }));
	CHECK(std::any_of(ops.begin(), ops.end(),
			[](const ve::EditOp &op) { return op.pos[0] == doctest::Approx(26.2f); }));
}

TEST_CASE("collect_ops_for_aabb preserves global order across a region boundary") {
	ve::EditLog log(bounds());
	// Region 0 ends at x = 25.6. Append a region-1-only op first, then an op that straddles
	// both regions. Region 0's list is [straddler], region 1's list is [region1-only,
	// straddler]. Iterating regions in z/y/x order without sequence numbers would return
	// [straddler, region1-only]; the true global order for the straddling component is
	// [region1-only, straddler].
	log.append(sphere(26.2f, 1.0f, 1.0f, 0.1f)); // region 1 only
	log.append(sphere(25.6f, 1.0f, 1.0f, 0.5f)); // straddles regions 0 and 1
	const float lo[3] = {24.8f, 0.0f, 0.0f};
	const float hi[3] = {26.4f, 2.0f, 2.0f};
	std::vector<ve::EditOp> ops;
	ve::collect_ops_for_aabb(log, lo, hi, &ops);
	REQUIRE(ops.size() == 2);
	CHECK(ops[0].pos[0] == doctest::Approx(26.2f));
	CHECK(ops[1].pos[0] == doctest::Approx(25.6f));
	CHECK(log.seqs({0, 0, 0}).size() == 1);
	CHECK(log.seqs({1, 0, 0}).size() == 2);
	CHECK(log.seqs({0, 0, 0})[0] == log.seqs({1, 0, 0})[1]); // same append op, same seq
}

TEST_CASE("collect_ops_for_aabb keeps byte-identical edits as separate ops in order") {
	ve::EditLog log(bounds());
	// Two independent edits with identical 32-byte payloads must not be collapsed by the
	// collector. They have distinct sequence numbers, so the output contains both and they
	// appear before a later distinct edit.
	const ve::EditOp a = sphere(1.0f, 1.0f, 1.0f, 0.5f);
	log.append(a);
	log.append(a);
	log.append(sphere(2.0f, 1.0f, 1.0f, 0.5f));
	const float lo[3] = {0.0f, 0.0f, 0.0f};
	const float hi[3] = {3.0f, 2.0f, 2.0f};
	std::vector<ve::EditOp> ops;
	ve::collect_ops_for_aabb(log, lo, hi, &ops);
	REQUIRE(ops.size() == 3);
	CHECK(std::memcmp(&ops[0], &a, sizeof(ve::EditOp)) == 0);
	CHECK(std::memcmp(&ops[1], &a, sizeof(ve::EditOp)) == 0);
	CHECK(ops[2].pos[0] == doctest::Approx(2.0f));
	CHECK(log.seqs({0, 0, 0}).size() == 3);
	CHECK(log.seqs({0, 0, 0})[0] != log.seqs({0, 0, 0})[1]);
	CHECK(log.seqs({0, 0, 0})[0] < log.seqs({0, 0, 0})[1]);
	CHECK(log.seqs({0, 0, 0})[1] < log.seqs({0, 0, 0})[2]);
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
	CHECK(log.seqs({0, 0, 0}).empty());
}
