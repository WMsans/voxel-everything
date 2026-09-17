#include <doctest/doctest.h>
#include "core/edit_pipeline.h"
#include <atomic>
#include <cmath>
#include <vector>

namespace {

ve::EditOp sphere(float x, float y, float z, float r, uint32_t type = ve::kOpSphereSubtract) {
	ve::EditOp op{};
	op.type = type;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

// Region (0, 0, 0) spans [0, 25.6) m; a small op at its centre touches only that region.
ve::EditOp in_r0() { return sphere(12.8f, 12.8f, 12.8f, 0.5f); }
// Region (2, 0, 0)'s centre, a region away from r0.
ve::EditOp in_r2() { return sphere(64.0f, 12.8f, 12.8f, 0.5f); }
// The boundary at x = 25.6 m: this one lands in regions (0, 0, 0) and (1, 0, 0).
ve::EditOp across() { return sphere(25.6f, 12.8f, 12.8f, 1.0f); }

const ve::IVec3 kR0{0, 0, 0};
const ve::IVec3 kR1{1, 0, 0};

struct Recorder : ve::InvalidationSink {
	std::vector<ve::InvalidationReason> reasons;
	std::vector<int64_t> seqs;
	std::vector<bool> notify;
	std::vector<ve::IVec3> regions;
	void record(const ve::Invalidation &inv) override {
		reasons.push_back(inv.reason);
		seqs.push_back(inv.seq);
		notify.push_back(inv.notify_islands);
		regions.push_back(inv.region);
	}
};

struct Fixture {
	ve::EditLog storage;
	ve::EditLog *log = &storage;
	std::atomic<int64_t> seq{0};
	ve::EditPipeline pipeline{&log, &seq};
	Recorder sink;
	Fixture() { pipeline.add_sink(&sink); }
	void fill(ve::EditOp op, int n) {
		for (int i = 0; i < n; i++) storage.append(op);
	}
};

} // namespace

TEST_CASE("an accepted op appends, bumps the seq once and tells the sinks") {
	Fixture f;
	const ve::EditOp op = in_r0();
	const ve::BatchResult r = f.pipeline.apply({&op, 1}, {});
	CHECK(r.ops.size() == 1);
	CHECK(r.ops[0].touched.size() == 1);
	CHECK(f.seq.load() == 1);
	REQUIRE(f.sink.reasons.size() == 1);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kEdit);
	CHECK(f.sink.seqs[0] == 1);
	CHECK(f.sink.notify[0] == true);
}

TEST_CASE("an op that touches nothing moves no seq and tells no sink") {
	Fixture f;
	const ve::EditOp bad = sphere(1.0f, 1.0f, 1.0f, std::nanf("")); // not well-formed
	const ve::BatchResult r = f.pipeline.apply({&bad, 1}, {});
	CHECK(r.ops[0].touched.empty());
	CHECK(f.seq.load() == 0);
	CHECK(f.sink.reasons.empty());
}

TEST_CASE("a fully rejected op reports kRejected and nothing else") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps);
	const ve::EditOp op = in_r0();
	const ve::BatchResult r = f.pipeline.apply({&op, 1}, {});
	CHECK(r.ops[0].rejected.size() == 1);
	CHECK(r.ops[0].touched.empty());
	CHECK(f.seq.load() == 0);
	REQUIRE(f.sink.reasons.size() == 1);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kRejected);
}

TEST_CASE("an oversized op reports kRejected") {
	Fixture f;
	const ve::EditOp huge = sphere(0.0f, 0.0f, 0.0f, 5000.0f);
	const ve::BatchResult r = f.pipeline.apply({&huge, 1}, {});
	CHECK(r.ops[0].oversized);
	REQUIRE(f.sink.reasons.size() == 1);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kRejected);
}

TEST_CASE("a non-atomic op is partially accepted") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps);
	const ve::EditOp op = across();
	const ve::BatchResult r = f.pipeline.apply({&op, 1}, {});
	CHECK(r.ops[0].rejected.size() == 1);
	CHECK(r.ops[0].rejected[0] == kR0);
	CHECK(f.storage.op_count(kR1) == 1);
	CHECK(f.seq.load() == 1);
	REQUIRE(f.sink.reasons.size() == 2);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kRejected);
	CHECK(f.sink.reasons[1] == ve::InvalidationReason::kEdit);
}

TEST_CASE("an atomic batch is all or nothing") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps - 1);
	const ve::EditOp ops[2] = {in_r0(), in_r0()};
	const ve::BatchResult r = f.pipeline.apply({ops, 2}, {.atomic = true});
	CHECK(r.refused);
	REQUIRE(r.full.size() == 1);
	CHECK(r.full[0] == kR0);
	CHECK(f.storage.op_count(kR0) == ve::kMaxRegionOps - 1);
	CHECK(f.seq.load() == 0);
	CHECK(f.sink.reasons.empty());
	// One op still fits, and then the same batch does not.
	const ve::BatchResult one = f.pipeline.apply({ops, 1}, {.atomic = true});
	CHECK_FALSE(one.refused);
	CHECK(f.storage.op_count(kR0) == ve::kMaxRegionOps);
}

TEST_CASE("an atomic batch counts its own ops against the cap, region by region") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps - 2);
	// Each op fits on its own; two of them in the same region do not.
	std::vector<ve::EditOp> ops = {in_r0(), in_r2(), in_r0(), in_r0()};
	const ve::BatchResult r = f.pipeline.apply(ops, {.atomic = true});
	CHECK(r.refused);
	REQUIRE(r.full.size() == 1);
	CHECK(r.full[0] == kR0);
	CHECK(f.storage.op_count(kR0) == ve::kMaxRegionOps - 2);
	CHECK(f.storage.op_count({2, 0, 0}) == 0);
}

TEST_CASE("an atomic batch holding an oversized op is refused whole") {
	Fixture f;
	std::vector<ve::EditOp> ops = {in_r0(), sphere(0.0f, 0.0f, 0.0f, 5000.0f)};
	const ve::BatchResult r = f.pipeline.apply(ops, {.atomic = true});
	CHECK(r.refused);
	CHECK(r.full.empty()); // no region is at the cap; the op itself is the refusal
	CHECK(f.storage.op_count(kR0) == 0);
	CHECK(f.sink.reasons.empty());
}

TEST_CASE("preflight answers without appending") {
	Fixture f;
	f.fill(in_r0(), ve::kMaxRegionOps);
	const ve::EditOp op = in_r0();
	std::vector<ve::IVec3> full;
	CHECK_FALSE(f.pipeline.preflight({&op, 1}, &full));
	REQUIRE(full.size() == 1);
	CHECK(full[0] == kR0);
	CHECK(f.storage.op_count(kR0) == ve::kMaxRegionOps);
	CHECK(f.sink.reasons.empty());
}

TEST_CASE("notify_islands travels with the invalidation") {
	Fixture f;
	const ve::EditOp op = in_r0();
	f.pipeline.apply({&op, 1}, {.atomic = false, .notify_islands = false});
	REQUIRE(f.sink.notify.size() == 1);
	CHECK(f.sink.notify[0] == false);
}

TEST_CASE("a removed sink hears nothing") {
	Fixture f;
	f.pipeline.remove_sink(&f.sink);
	const ve::EditOp op = in_r0();
	f.pipeline.apply({&op, 1}, {});
	CHECK(f.sink.reasons.empty());
	CHECK(f.seq.load() == 1); // the edit still happened
}

TEST_CASE("invalidate carries the region and its world box") {
	Fixture f;
	const ve::Invalidation inv = ve::Invalidation::consolidated({1, 2, 3});
	CHECK(inv.reason == ve::InvalidationReason::kConsolidated);
	CHECK(inv.region == ve::IVec3{1, 2, 3});
	// One region is 32 bricks of 0.8 m.
	CHECK(inv.lo[0] == doctest::Approx(25.6f));
	CHECK(inv.lo[1] == doctest::Approx(51.2f));
	CHECK(inv.lo[2] == doctest::Approx(76.8f));
	CHECK(inv.hi[0] == doctest::Approx(51.2f));
	CHECK(inv.hi[1] == doctest::Approx(76.8f));
	CHECK(inv.hi[2] == doctest::Approx(102.4f));
	f.pipeline.invalidate(inv);
	REQUIRE(f.sink.reasons.size() == 1);
	CHECK(f.sink.reasons[0] == ve::InvalidationReason::kConsolidated);
	CHECK(f.sink.regions[0] == ve::IVec3{1, 2, 3});
	CHECK(f.seq.load() == 0);
}

TEST_CASE("with no edit log an apply is a no-op with one empty result per op") {
	ve::EditLog *none = nullptr;
	std::atomic<int64_t> seq{0};
	ve::EditPipeline pipeline(&none, &seq);
	Recorder sink;
	pipeline.add_sink(&sink);
	std::vector<ve::EditOp> ops = {in_r0(), in_r2()};
	const ve::BatchResult r = pipeline.apply(ops, {});
	CHECK(r.ops.size() == 2);
	CHECK(r.ops[0].touched.empty());
	CHECK_FALSE(r.refused);
	CHECK(sink.reasons.empty());
	CHECK(seq.load() == 0);
}

TEST_CASE("merge_or_cap merges what overlaps and keeps what does not") {
	std::vector<ve::Box3<int>> q;
	ve::merge_or_cap(&q, ve::Box3<int>{{0, 0, 0}, {1, 1, 1}});
	ve::merge_or_cap(&q, ve::Box3<int>{{1, 0, 0}, {2, 1, 1}});
	REQUIRE(q.size() == 1);
	CHECK(q[0].lo[0] == 0);
	CHECK(q[0].hi[0] == 2);
	ve::merge_or_cap(&q, ve::Box3<int>{{10, 10, 10}, {11, 11, 11}});
	CHECK(q.size() == 2);
}

TEST_CASE("merge_or_cap absorbs every box a bridging box reaches") {
	std::vector<ve::Box3<int>> q;
	ve::merge_or_cap(&q, ve::Box3<int>{{0, 0, 0}, {1, 1, 1}});
	ve::merge_or_cap(&q, ve::Box3<int>{{5, 0, 0}, {6, 1, 1}});
	REQUIRE(q.size() == 2);
	ve::merge_or_cap(&q, ve::Box3<int>{{1, 0, 0}, {5, 1, 1}});
	REQUIRE(q.size() == 1);
	CHECK(q[0].lo[0] == 0);
	CHECK(q[0].hi[0] == 6);
}

TEST_CASE("past the cap merge_or_cap folds the queue into one box") {
	std::vector<ve::Box3<float>> q;
	for (int i = 0; i < 5; i++) {
		const float x = static_cast<float>(i) * 10.0f;
		ve::merge_or_cap(&q, ve::Box3<float>{{x, 0.0f, 0.0f}, {x + 1.0f, 1.0f, 1.0f}}, 4);
	}
	REQUIRE(q.size() == 1);
	CHECK(q[0].lo[0] == doctest::Approx(0.0f));
	CHECK(q[0].hi[0] == doctest::Approx(41.0f));
}
