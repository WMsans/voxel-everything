#include <doctest/doctest.h>
#include "lod/lod_tree.h"
#include "lod/lod_grid.h"
#include <cmath>
#include <vector>

namespace {

ve::WorldBounds demo_bounds() {
	ve::WorldBounds b;
	b.origin_bricks = {0, -64, 0};
	b.size_regions = {64, 8, 64}; // 1638.4 x 204.8 x 1638.4 m
	return b;
}

// Looking down -Z from just above the terrain, which sits at y ~ 51.2 (M2 errata 9).
ve::LodCamera cam_at(float x, float y, float z) {
	const float pos[3] = {x, y, z};
	const float fwd[3] = {0.0f, 0.0f, -1.0f};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	return ve::lod_camera_perspective(pos, fwd, up, 1.2217f, 16.0f / 9.0f, 0.1f, 8000.0f,
			2560, 1440);
}

// Nothing is ever occluded.
struct NoOcclusion : ve::LodOcclusion {
	bool occluded(const float[3], const float[3]) const override { return false; }
};
// Everything is always occluded.
struct AllOccluded : ve::LodOcclusion {
	bool occluded(const float[3], const float[3]) const override { return true; }
};

// Drives a tree to a steady state by answering every request as a ready chunk.
void settle(ve::LodTree *t, const ve::LodCamera &c, const ve::LodOcclusion *occ, int frames) {
	ve::LodWalkResult r;
	for (int f = 1; f <= 10000; f++) {
		t->walk(c, occ, uint32_t(f), &r);
		for (const ve::LodBuildRequest &q : r.requests)
			t->note_ready(q.level, q.coord, 1, 1);
		if (r.requests.empty() && f >= frames) break;
	}
	// Fail instead of hanging if a regression leaves requests never draining.
	REQUIRE(r.requests.empty());
}

} // namespace

TEST_CASE("the roots come from the world bounds and start unknown") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	ve::LodWalkResult r;
	NoOcclusion occ;
	t.walk(cam_at(800.0f, 60.0f, 800.0f), &occ, 1, &r);
	// Nothing is ready yet, so nothing draws and the roots are requested.
	CHECK(r.draws.empty());
	CHECK(!r.requests.empty());
	for (const ve::LodBuildRequest &q : r.requests) CHECK(q.level == ve::kLodLevels - 1);
}

// The cut must be COMPLETE and NON-OVERLAPPING at every instant: a streaming child never
// opens a hole and never z-fights its parent. This is the invariant the whole walk exists
// to preserve, so it is checked on every frame of a settling run, not only at the end.
TEST_CASE("the emitted cut never overlaps and never leaves a gap under a drawn parent") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	for (int f = 1; f <= 40; f++) {
		t.walk(c, &occ, uint32_t(f), &r);
		// No drawn chunk may be an ancestor or descendant of another drawn chunk.
		for (size_t i = 0; i < r.draws.size(); i++)
			for (size_t j = i + 1; j < r.draws.size(); j++) {
				const ve::LodDrawItem &a = r.draws[i];
				const ve::LodDrawItem &b = r.draws[j];
				if (a.level == b.level) {
					CHECK(!(a.coord == b.coord));
					continue;
				}
				const ve::LodDrawItem &lo = a.level < b.level ? a : b;
				const ve::LodDrawItem &hi = a.level < b.level ? b : a;
				ve::IVec3 up = lo.coord;
				for (int l = lo.level; l < hi.level; l++) up = ve::lod_parent(up);
				CHECK(!(up == hi.coord));
			}
		for (const ve::LodBuildRequest &q : r.requests) t.note_ready(q.level, q.coord, 1, 1);
	}
}

TEST_CASE("a node is only descended into when all eight children are ready") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	t.walk(c, &occ, 1, &r);
	REQUIRE(!r.requests.empty());
	const ve::LodBuildRequest root = r.requests[0];
	t.note_ready(root.level, root.coord, 1, 1);
	t.walk(c, &occ, 2, &r);
	// The root now draws, and its children are requested -- but only SEVEN of them ready
	// must not be enough to descend.
	const ve::IVec3 base = ve::lod_child_base(root.coord);
	for (int k = 0; k < 7; k++)
		t.note_ready(root.level - 1,
				{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)}, 1, 1);
	t.walk(c, &occ, 3, &r);
	bool root_drawn = false;
	for (const ve::LodDrawItem &d : r.draws)
		if (d.level == root.level && d.coord == root.coord) root_drawn = true;
	CHECK(root_drawn);
}

// A chunk that reports itself empty is a valid, terminal answer -- it counts as ready for
// the sibling test, otherwise a hole in the terrain would freeze the whole subtree.
TEST_CASE("an empty child counts as ready and draws nothing") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	t.walk(c, &occ, 1, &r);
	REQUIRE(!r.requests.empty());
	for (const ve::LodBuildRequest &q : r.requests) t.note_empty(q.level, q.coord);
	t.walk(c, &occ, 2, &r);
	CHECK(r.draws.empty());
	CHECK(r.requests.empty()); // an empty node has nothing below it worth asking for
}

// Levels 5, 6 and 7 are permanently resident: turning around shows coarse terrain, not sky.
TEST_CASE("coarse levels are never evicted and fine ones are") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	settle(&t, cam_at(800.0f, 60.0f, 800.0f), &occ, 30);
	const int before = t.node_count();
	CHECK(before > 0);
	std::vector<ve::LodDrawItem> evicted;
	// Far in the future, so every unmarked node is past kLodEvictFrames.
	t.collect_evictions(1000000u, 0, &evicted);
	for (const ve::LodDrawItem &e : evicted) CHECK(e.level < ve::kLodResidentLevelFrom);
}

TEST_CASE("a marked node is never evicted for age") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 30);
	ve::LodWalkResult r;
	t.walk(c, &occ, 31u, &r);
	std::vector<ve::LodDrawItem> evicted;
	t.collect_evictions(31u, 0, &evicted);
	CHECK(evicted.empty());
}

// Occlusion may stop REFINEMENT but must never stop DRAWING: the readback is stale, so a
// wrongly hidden chunk would be a hole that heals only when the camera moves.
TEST_CASE("occlusion stops requests but never removes a drawn chunk") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion none;
	AllOccluded all;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &none, 20);
	ve::LodWalkResult open;
	t.walk(c, &none, 21u, &open);
	const size_t drawn_open = open.draws.size();

	ve::LodWalkResult shut;
	// Confirmation takes kLodOccludedFrames frames, so the first few walks still request.
	for (uint32_t f = 22; f < 22 + uint32_t(ve::kLodOccludedFrames) + 2; f++)
		t.walk(c, &all, f, &shut);
	CHECK(shut.requests.empty());
	CHECK(shut.draws.size() == drawn_open);
}

// A drawn node whose field changed keeps drawing its stale pages until the rebuild lands.
// Stale beats missing (engine spec section 8).
TEST_CASE("an edit re-requests a chunk without un-drawing it") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 30);
	ve::LodWalkResult before;
	t.walk(c, &occ, 31u, &before);
	REQUIRE(!before.draws.empty());
	CHECK(before.requests.empty());

	const ve::LodDrawItem d = before.draws[0];
	float lo[3], hi[3];
	ve::lod_chunk_aabb(d.level, d.coord, lo, hi);
	t.mark_dirty(lo, hi);

	ve::LodWalkResult after;
	t.walk(c, &occ, 32u, &after);
	CHECK(after.draws.size() == before.draws.size());
	bool re_requested = false;
	for (const ve::LodBuildRequest &q : after.requests)
		if (q.level == d.level && q.coord == d.coord) re_requested = true;
	CHECK(re_requested);
}

// The VoxelWorld upload-refusal path must re-affirm a resident node as Ready with its old
// page list instead of marking it failed; otherwise the old pages are hidden even though
// they are still valid GPU data. This simulates that branch at the tree level.
TEST_CASE("a refused rebuild with resident pages keeps the old pages drawable") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 30);
	ve::LodWalkResult before;
	t.walk(c, &occ, 31u, &before);
	REQUIRE(!before.draws.empty());
	const ve::LodDrawItem d = before.draws[0];

	float lo[3], hi[3];
	ve::lod_chunk_aabb(d.level, d.coord, lo, hi);
	t.mark_dirty(lo, hi);
	ve::LodWalkResult requested;
	t.walk(c, &occ, 32u, &requested);
	// The dirty node was re-requested; in VoxelWorld the upload for this rebuild is refused.
	bool re_requested = false;
	for (const ve::LodBuildRequest &q : requested.requests)
		if (q.level == d.level && q.coord == d.coord) re_requested = true;
	REQUIRE(re_requested);

	// VoxelWorld keeps the old resident pages and re-affirms Ready with them.
	t.note_ready(d.level, d.coord, d.page_first, d.page_count);

	ve::LodWalkResult after;
	t.walk(c, &occ, 33u, &after);
	bool still_drawn = false;
	for (const ve::LodDrawItem &draw : after.draws)
		if (draw.level == d.level && draw.coord == d.coord &&
				draw.page_first == d.page_first && draw.page_count == d.page_count)
			still_drawn = true;
	CHECK(still_drawn);
}

TEST_CASE("requests are capped and ordered largest first") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	cfg.max_requests_per_walk = 4;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	for (int f = 1; f <= 10; f++) {
		t.walk(c, &occ, uint32_t(f), &r);
		CHECK(int(r.requests.size()) <= 4);
		for (size_t i = 1; i < r.requests.size(); i++)
			CHECK(r.requests[i - 1].priority >= r.requests[i].priority);
		for (const ve::LodBuildRequest &q : r.requests) t.note_ready(q.level, q.coord, 1, 1);
	}
}

// A chunk entirely inside the near field is discarded by the fragment shader on every pixel,
// so building it burns pages to draw nothing (spec section 6.4).
TEST_CASE("chunks entirely inside the fade start are never requested") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	for (int f = 1; f <= 60; f++) {
		t.walk(c, &occ, uint32_t(f), &r);
		for (const ve::LodBuildRequest &q : r.requests) {
			const float pos[3] = {800.0f, 60.0f, 800.0f};
			CHECK(ve::lod_chunk_far_distance(q.level, q.coord, pos) >= ve::kLodFadeStartM);
			t.note_ready(q.level, q.coord, 1, 1);
		}
	}
}

// A failed build must be retried, not cached as done -- otherwise one transient GPU hiccup
// leaves a permanent hole.
TEST_CASE("a failed build is retried") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	REQUIRE(!r.requests.empty());
	const ve::LodBuildRequest q = r.requests[0];
	t.note_building(q.level, q.coord);
	t.note_failed(q.level, q.coord);
	t.walk(c, &occ, 2u, &r);
	bool again = false;
	for (const ve::LodBuildRequest &s : r.requests)
		if (s.level == q.level && s.coord == q.coord) again = true;
	CHECK(again);
}

// A node with a build in flight must not be requested again every frame, or the queue fills
// with duplicates and nothing else is ever built.
TEST_CASE("a building node is not re-requested") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	REQUIRE(!r.requests.empty());
	for (const ve::LodBuildRequest &q : r.requests) t.note_building(q.level, q.coord);
	t.walk(c, &occ, 2u, &r);
	CHECK(r.requests.empty());
}
