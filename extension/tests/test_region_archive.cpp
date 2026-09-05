#include <doctest/doctest.h>
#include "world/region_archive.h"

static ve::EditOp sphere(float x, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x;
	op.radius = r;
	return op;
}

TEST_CASE("a stored region round-trips") {
	ve::PinnedRegionArchive a;
	ve::RegionSnapshot in;
	in.region = {3, -4, 5};
	in.ops.push_back(sphere(1.0f, 2.0f));
	in.seqs.push_back(7);
	in.override_table = 11;
	a.store(std::move(in));

	ve::RegionSnapshot out;
	REQUIRE(a.load({3, -4, 5}, &out));
	CHECK(out.region == ve::IVec3{3, -4, 5});
	REQUIRE(out.ops.size() == 1);
	CHECK(out.ops[0].radius == doctest::Approx(2.0f));
	REQUIRE(out.seqs.size() == 1);
	CHECK(out.seqs[0] == 7);
	CHECK(out.override_table == 11);
}

TEST_CASE("loading a region that was never stored reports nothing, not empty edits") {
	// The distinction matters: 'never edited' means regenerate from the terrain pipeline,
	// while 'edited, and the edits were an empty list' would mean pristine ground. Confusing
	// them is how a disk-backed archive silently deletes a player's excavation.
	ve::PinnedRegionArchive a;
	ve::RegionSnapshot out;
	CHECK(!a.load({0, 0, 0}, &out));
}

TEST_CASE("storing a region twice keeps the newer snapshot") {
	ve::PinnedRegionArchive a;
	ve::RegionSnapshot first;
	first.region = {1, 1, 1};
	first.ops.push_back(sphere(1.0f, 1.0f));
	a.store(std::move(first));
	ve::RegionSnapshot second;
	second.region = {1, 1, 1};
	second.ops.push_back(sphere(2.0f, 2.0f));
	second.ops.push_back(sphere(3.0f, 3.0f));
	a.store(std::move(second));

	ve::RegionSnapshot out;
	REQUIRE(a.load({1, 1, 1}, &out));
	CHECK(out.ops.size() == 2);
}
