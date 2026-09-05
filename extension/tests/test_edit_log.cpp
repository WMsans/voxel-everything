#include <doctest/doctest.h>
#include "world/edit_log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "lod/lod_grid.h"

static ve::EditOp sphere(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

TEST_CASE("an op lands in every region it touches, and only those") {
	ve::EditLog log;
	// Region 0 spans [0, 25.6) m. A 2 m sphere at 25.0 m straddles regions 0 and 1 on x,
	// and its padded range dips below y = 0 and z = 0: with no world edge those negative
	// neighbours get their own lists too (2 x 2 x 2 = 8 regions).
	const auto r = log.append(sphere(25.0f, 1.0f, 1.0f, 2.0f));
	CHECK(r.rejected.empty());
	CHECK(r.touched.size() == 8);
	CHECK(log.op_count({0, 0, 0}) == 1);
	CHECK(log.op_count({1, 0, 0}) == 1);
	CHECK(log.op_count({2, 0, 0}) == 0);
	CHECK(log.op_count({0, -1, -1}) == 1);
	CHECK(log.region_count() == 8);
}

TEST_CASE("collect_ops_for_aabb gathers ops from every region a component straddles") {
	ve::EditLog log;
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
	ve::EditLog log;
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
	ve::EditLog log;
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

TEST_CASE("collect_ops_for_aabb can exceed kMaxRegionOps across a boundary even when each region is under cap") {
	ve::EditLog log;
	// Region 0 ends at x = 25.6. Fill each side with 200 small ops: neither region alone is
	// full, but a component straddling the boundary sees the flattened 400-op list.
	for (int i = 0; i < 200; i++) {
		log.append(sphere(24.8f, 1.0f, 1.0f, 0.1f)); // region 0 only
		log.append(sphere(26.0f, 1.0f, 1.0f, 0.1f)); // region 1 only
	}
	CHECK(log.op_count({0, 0, 0}) == 200);
	CHECK(log.op_count({1, 0, 0}) == 200);
	CHECK(log.op_count({0, 0, 0}) < ve::kMaxRegionOps);
	CHECK(log.op_count({1, 0, 0}) < ve::kMaxRegionOps);
	const float lo[3] = {24.8f, 0.0f, 0.0f};
	const float hi[3] = {26.4f, 2.0f, 2.0f};
	std::vector<ve::EditOp> ops;
	ve::collect_ops_for_aabb(log, lo, hi, &ops);
	REQUIRE(ops.size() > static_cast<size_t>(ve::kMaxRegionOps));
	CHECK(ops.size() == 400);
}

TEST_CASE("an op far from the origin is stored, not dropped") {
	ve::EditLog log;
	const auto r = log.append(sphere(-100.0f, 0.0f, 0.0f, 1.0f));
	CHECK(!r.oversized);
	CHECK(!r.touched.empty());
	CHECK(r.rejected.empty());
	CHECK(log.region_count() >= 1);
}

TEST_CASE("an op straddling the old origin corner keeps its negative regions") {
	ve::EditLog log;
	const auto r = log.append(sphere(0.5f, 0.5f, 0.5f, 2.0f)); // straddles x = 0 corner
	// No world edge now: the op lands in region 0 and its negative neighbours.
	CHECK(!r.touched.empty());
	CHECK(std::any_of(r.touched.begin(), r.touched.end(),
			[](const ve::IVec3 &t) { return t == ve::IVec3{0, 0, 0}; }));
	CHECK(std::any_of(r.touched.begin(), r.touched.end(),
			[](const ve::IVec3 &t) { return t.x < 0 || t.y < 0 || t.z < 0; }));
	CHECK(log.op_count({-1, 0, 0}) >= 1);
}

TEST_CASE("ops are preserved in append order") {
	ve::EditLog log;
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
	ve::EditLog log;
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
	ve::EditLog log;
	for (int i = 0; i < ve::kMaxRegionOps; i++) log.append(sphere(1.0f, 1.0f, 1.0f, 0.5f));
	// The straddler's padded range covers x in {0, 1} x y in {-1, 0} x z in {-1, 0}.
	// Only {0, 0, 0} is full; the other 7 regions accept the op.
	const auto r = log.append(sphere(25.0f, 1.0f, 1.0f, 2.0f));
	CHECK(r.touched.size() == 7);
	CHECK(std::any_of(r.touched.begin(), r.touched.end(),
			[](const ve::IVec3 &t) { return t == ve::IVec3{1, 0, 0}; }));
	CHECK(r.rejected.size() == 1);
	CHECK(r.rejected[0] == ve::IVec3{0, 0, 0});
}

TEST_CASE("clear through a sequence preserves edits appended during the bake") {
	ve::EditLog log;
	log.append(sphere(1.0f, 1.0f, 1.0f, 0.5f));
	const uint64_t through = log.seqs({0, 0, 0}).back();
	log.append(sphere(2.0f, 1.0f, 1.0f, 0.5f));
	log.clear_region_through({0, 0, 0}, through);
	REQUIRE(log.op_count({0, 0, 0}) == 1);
	CHECK(log.ops({0, 0, 0})[0].pos[0] == doctest::Approx(2.0f));
	CHECK(log.seqs({0, 0, 0})[0] > through);
}

TEST_CASE("clear drops everything") {
	ve::EditLog log;
	log.append(sphere(1.0f, 1.0f, 1.0f, 1.0f));
	log.clear();
	CHECK(log.region_count() == 0);
	CHECK(log.op_count({0, 0, 0}) == 0);
	CHECK(log.ops({0, 0, 0}).empty());
	CHECK(log.seqs({0, 0, 0}).empty());
}

TEST_CASE("the padded L7 collector query is not mistaken for a hostile query") {
	ve::EditLog log;
	log.append(sphere(0.0f, 0.0f, 0.0f, 1.0f));
	float lo[3], hi[3];
	ve::lod_chunk_aabb(ve::kLodLevels - 1, {0, 0, 0}, lo, hi);
	const float pad = 2.0f * ve::lod_cell_size(ve::kLodLevels - 1);
	for (int a = 0; a < 3; a++) {
		lo[a] -= pad;
		hi[a] += pad;
	}
	std::vector<ve::EditOp> ops;
	ve::collect_ops_for_aabb(log, lo, hi, &ops);
	REQUIRE(ops.size() == 1);
	CHECK(ops[0].pos[0] == doctest::Approx(0.0f));
}

TEST_CASE("malformed edit ops fail soft without entering the append log") {
	ve::EditLog log;
	ve::EditOp unknown = sphere(0.0f, 0.0f, 0.0f, 1.0f);
	unknown.type = 99;
	const auto unknown_result = log.append(unknown);
	CHECK(unknown_result.touched.empty());
	CHECK(unknown_result.rejected.empty());
	CHECK(!unknown_result.oversized);
	CHECK(log.region_count() == 0);

	ve::EditOp nonfinite = sphere(0.0f, 0.0f, 0.0f, 1.0f);
	nonfinite.pos[0] = std::numeric_limits<float>::quiet_NaN();
	const auto nonfinite_result = log.append(nonfinite);
	CHECK(nonfinite_result.touched.empty());
	CHECK(nonfinite_result.rejected.empty());
	CHECK(!nonfinite_result.oversized);
	CHECK(log.region_count() == 0);
}

TEST_CASE("an op far outside the old world box is accepted") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 50000.0f; op.pos[1] = 0.0f; op.pos[2] = 50000.0f;
	op.radius = 2.0f;
	const auto r = log.append(op);
	CHECK(!r.oversized);
	CHECK(r.rejected.empty());
	CHECK(!r.touched.empty());
	CHECK(log.op_count({1953, 0, 1953}) >= 1); // 50000 / 25.6 = 1953.1
}

TEST_CASE("an oversized op is refused instead of iterated") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.radius = 1.0e5f;
	const auto r = log.append(op);
	CHECK(r.oversized);
	CHECK(r.touched.empty());
	CHECK(r.rejected.empty());
	CHECK(log.region_count() == 0);
}

TEST_CASE("negative region coordinates get their own lists") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = -100.0f; op.pos[1] = -100.0f; op.pos[2] = -100.0f;
	op.radius = 1.0f;
	const auto r = log.append(op);
	CHECK(!r.touched.empty());
	CHECK(log.op_count({-4, -4, -4}) >= 1); // floor(-100 / 25.6) = -4
}
