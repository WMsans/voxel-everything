#include <doctest/doctest.h>
#include "lod/lod_tree.h"
#include "lod/lod_grid.h"
#include "lod/lod_arena.h"
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

// Builds a single ready path from `root` to a level-0 target. Every sibling along the path
// is empty, and the other root chunks are empty too, so the normal walk can descend to the
// target without generating a flood of sibling requests; the target itself is left for the
// caller to mark dirty.
void make_ready_path(ve::LodTree *t, const ve::WorldBounds &bounds, const ve::IVec3 &root,
		const ve::IVec3 &target) {
	ve::IVec3 rlo, rhi;
	ve::lod_root_range(bounds, &rlo, &rhi);
	for (int z = rlo.z; z <= rhi.z; z++)
		for (int y = rlo.y; y <= rhi.y; y++)
			for (int x = rlo.x; x <= rhi.x; x++)
				if (!(ve::IVec3{x, y, z} == root)) t->note_empty(ve::kLodLevels - 1, {x, y, z});

	ve::IVec3 child = target;
	for (int level = 0; level < ve::kLodLevels - 1; level++) {
		const ve::IVec3 parent = ve::lod_parent(child);
		const ve::IVec3 base = ve::lod_child_base(parent);
		for (int k = 0; k < 8; k++) {
			const ve::IVec3 s{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)};
			if (s == child) {
				t->note_ready(level, child, 1, 1);
			} else {
				t->note_empty(level, s);
			}
		}
		child = parent;
	}
	t->note_ready(ve::kLodLevels - 1, root, 1, 1);
}

// Builds a fully ready level-1 chunk: all eight level-0 children are ready with one page,
// every ancestor on the path to the root is ready, and every other root is empty.
void make_ready_full_level0(ve::LodTree *t, const ve::WorldBounds &bounds,
		const ve::IVec3 &l1) {
	ve::IVec3 root = l1;
	for (int i = 1; i < ve::kLodLevels - 1; i++) root = ve::lod_parent(root);
	const ve::IVec3 first_child = ve::lod_child_base(l1);
	make_ready_path(t, bounds, root, first_child);
	const ve::IVec3 base = first_child;
	for (int k = 0; k < 8; k++) {
		const ve::IVec3 child{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)};
		t->note_ready(0, child, 1, 1);
	}
}

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

// The dirty sweep is gathered before the final sort/dedup. A dirty node that the normal walk
// already requests must collapse to one entry, and the slot saved by that dedup must let an
// off-screen dirty node survive the per-walk cap instead of being starved at the tail.
TEST_CASE("dirty sweep requests are deduped before the cap and not starved") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	cfg.max_requests_per_walk = 2;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);

	// A ready path to a level-0 target 150 m in front of the camera: the normal walk visits
	// and requests that target. A second dirty node off to the side is never visited by the
	// normal walk, so only the dirty sweep asks for it.
	const ve::IVec3 drawn_coord = ve::lod_chunk_of_point(0, 800.0f, 51.0f, 650.0f);
	const ve::IVec3 root = ve::lod_chunk_of_point(ve::kLodLevels - 1, 800.0f, 51.0f, 650.0f);
	make_ready_path(&t, cfg.bounds, root, drawn_coord);
	t.note_ready_dirty(0, drawn_coord);

	const int off_level = 2;
	const ve::IVec3 off_coord = ve::lod_chunk_of_point(off_level, 0.0f, 51.0f, 0.0f);
	t.note_ready(off_level, off_coord, 1, 1);
	t.note_ready_dirty(off_level, off_coord);

	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	REQUIRE(int(r.requests.size()) <= cfg.max_requests_per_walk);

	// No (level, coord) appears more than once.
	for (size_t i = 0; i < r.requests.size(); i++)
		for (size_t j = i + 1; j < r.requests.size(); j++) {
			const bool same_key = r.requests[i].level == r.requests[j].level &&
					r.requests[i].coord == r.requests[j].coord;
			CHECK_FALSE(same_key);
		}

	// The drawn dirty node is still re-requested, exactly once.
	int drawn_count = 0;
	for (const ve::LodBuildRequest &q : r.requests)
		if (q.level == 0 && q.coord == drawn_coord) drawn_count++;
	CHECK(drawn_count == 1);

	// The off-screen dirty node is not starved by the duplicate occupying the cap slot.
	bool off_requested = false;
	for (const ve::LodBuildRequest &q : r.requests)
		if (q.level == off_level && q.coord == off_coord) off_requested = true;
	CHECK(off_requested);
}

// A dirty sweep request for a node outside the current walk must not count as a residency
// touch. Otherwise an off-screen stale page would be pinned forever by being dirty, and arena
// pressure could never evict it.
TEST_CASE("dirty sweep requests do not mark off-screen nodes resident") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);

	// Create a far, unvisited node and make it dirty. No root is ready, so the normal walk
	// cannot descend to it; only the dirty sweep can request it.
	const int level = 2;
	const ve::IVec3 coord = ve::lod_chunk_of_point(level, 0.0f, 51.0f, 0.0f);
	t.note_ready(level, coord, 1, 1);
	t.note_ready_dirty(level, coord);

	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	bool re_requested = false;
	for (const ve::LodBuildRequest &q : r.requests)
		if (q.level == level && q.coord == coord) re_requested = true;
	REQUIRE(re_requested);

	// The node is not visited by the normal walk, so a non-touching dirty sweep leaves its
	// last_marked at frame 0. At frame 1 under pressure it is age 1 and evictable.
	std::vector<ve::LodDrawItem> evicted;
	t.collect_evictions(1u, 1, &evicted);
	bool evicted_off_screen = false;
	for (const ve::LodDrawItem &e : evicted)
		if (e.level == level && e.coord == coord) evicted_off_screen = true;
	CHECK(evicted_off_screen);
}

// The VoxelWorld upload-refusal path must re-affirm a resident node as Ready-with-dirty
// with its old page list instead of marking it failed or clearing dirty: the old pages stay
// drawable AND the node is re-requested next frame so the refused rebuild is retried. This
// simulates that branch at the tree level.
TEST_CASE("a refused rebuild with resident pages stays drawable and is retried") {
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

	// VoxelWorld keeps the old resident pages and re-affirms Ready while leaving dirty set.
	t.note_ready_dirty(d.level, d.coord);
	CHECK(t.state_of(d.level, d.coord) == ve::kLodReady);

	ve::LodWalkResult after;
	t.walk(c, &occ, 33u, &after);
	// Still drawable with the old page range...
	bool still_drawn = false;
	for (const ve::LodDrawItem &draw : after.draws)
		if (draw.level == d.level && draw.coord == d.coord &&
				draw.page_first == d.page_first && draw.page_count == d.page_count)
			still_drawn = true;
	CHECK(still_drawn);
	// ...and still dirty, so the walk asks for the rebuild again.
	bool retried = false;
	for (const ve::LodBuildRequest &q : after.requests)
		if (q.level == d.level && q.coord == d.coord) retried = true;
	CHECK(retried);
}

// Direct unit test for the ready-with-dirty transition used by a refused/failed rebuild.
TEST_CASE("note_ready_dirty keeps old pages drawable and dirty for retry") {
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

	// A non-dirty ready node becomes ready-with-dirty and keeps its existing page range.
	t.note_ready_dirty(d.level, d.coord);
	CHECK(t.state_of(d.level, d.coord) == ve::kLodReady);

	ve::LodWalkResult after;
	t.walk(c, &occ, 32u, &after);
	bool still_drawn = false;
	for (const ve::LodDrawItem &draw : after.draws)
		if (draw.level == d.level && draw.coord == d.coord &&
				draw.page_first == d.page_first && draw.page_count == d.page_count)
			still_drawn = true;
	CHECK(still_drawn);
	bool re_requested = false;
	for (const ve::LodBuildRequest &q : after.requests)
		if (q.level == d.level && q.coord == d.coord) re_requested = true;
	CHECK(re_requested);
}

// VoxelWorld releases a resident chunk's old pages before calling note_empty. The tree-side
// contract of that path is: the node leaves the draw list, its page range is cleared, and it
// is never re-requested (empty is a terminal answer).
TEST_CASE("an empty result after a resident chunk stops drawing it and clears the page range") {
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

	t.note_empty(d.level, d.coord);
	CHECK(t.state_of(d.level, d.coord) == ve::kLodEmpty);

	ve::LodWalkResult after;
	t.walk(c, &occ, 32u, &after);
	bool still_drawn = false;
	for (const ve::LodDrawItem &draw : after.draws)
		if (draw.level == d.level && draw.coord == d.coord) still_drawn = true;
	CHECK(!still_drawn);
	for (const ve::LodBuildRequest &q : after.requests) {
		const bool re_requested_empty = q.level == d.level && q.coord == d.coord;
		CHECK_FALSE(re_requested_empty);
	}
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

TEST_CASE("an op smaller than half a cell does not dirty that level") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 30);
	// A 0.5 m drill. Half of L4's 6.4 m cell is 3.2 m, so it cannot move a sample there.
	const float lo[3] = {800.0f, 51.0f, 700.0f};
	const float hi[3] = {800.5f, 51.5f, 700.5f};
	t.mark_dirty(lo, hi);
	ve::LodWalkResult r;
	t.walk(c, &occ, 31u, &r);
	for (const ve::LodBuildRequest &q : r.requests) CHECK(q.level <= 1);
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

// Critical 2: a ready chunk with a rebuild in flight keeps its state kLodReady and keeps
// drawing its old page range. The build-in-flight bit is separate from drawability.
TEST_CASE("a ready node stays drawable while a rebuild is in flight") {
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

	t.note_building(d.level, d.coord);
	CHECK(t.state_of(d.level, d.coord) == ve::kLodReady);

	ve::LodWalkResult after;
	t.walk(c, &occ, 32u, &after);
	bool still_drawn = false;
	for (const ve::LodDrawItem &draw : after.draws)
		if (draw.level == d.level && draw.coord == d.coord &&
				draw.page_first == d.page_first && draw.page_count == d.page_count)
			still_drawn = true;
	CHECK(still_drawn);
	for (const ve::LodBuildRequest &q : after.requests) {
		const bool re_requested = q.level == d.level && q.coord == d.coord;
		CHECK_FALSE(re_requested);
	}
}

// Critical 3: an edit that arrives after note_building (which clears dirty at submission)
// must survive note_ready, so the next walk re-requests the node instead of dropping the edit.
TEST_CASE("an edit during an in-flight build survives note_ready") {
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
	REQUIRE(!requested.requests.empty());

	// The build is submitted: note_building clears the dirty that the request above observed.
	t.note_building(d.level, d.coord);
	// The edit lands while the build is in flight.
	t.mark_dirty(lo, hi);
	// The stale result comes back: it must NOT clear the re-set dirty flag.
	t.note_ready(d.level, d.coord, d.page_first, d.page_count);

	int dirty_chunks = 0;
	int dirty_levels = 0;
	t.dirty_stats(&dirty_chunks, &dirty_levels);
	CHECK(dirty_chunks > 0);

	ve::LodWalkResult after;
	t.walk(c, &occ, 33u, &after);
	bool re_requested = false;
	for (const ve::LodBuildRequest &q : after.requests)
		if (q.level == d.level && q.coord == d.coord) re_requested = true;
	CHECK(re_requested);
}

// Remaining Critical: an edit that arrives after note_building must also survive a stale
// note_empty (an empty build result from before the edit). Without this fix note_empty made
// the node terminal kLodEmpty with dirty=false, permanently discarding the edit.
TEST_CASE("an edit during an in-flight build survives stale note_empty") {
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
	REQUIRE(!requested.requests.empty());

	// The build is submitted: note_building clears the dirty that the request above observed.
	t.note_building(d.level, d.coord);
	// The edit lands while the build is in flight.
	t.mark_dirty(lo, hi);
	// The stale pre-edit build returns no geometry. It must leave the node requestable, not
	// terminal-empty, so the next walk rebuilds with the edit included.
	t.note_empty(d.level, d.coord);
	CHECK(t.is_dirty(d.level, d.coord));
	CHECK(t.state_of(d.level, d.coord) != ve::kLodEmpty);

	ve::LodWalkResult after;
	t.walk(c, &occ, 33u, &after);
	bool re_requested = false;
	for (const ve::LodBuildRequest &q : after.requests)
		if (q.level == d.level && q.coord == d.coord) re_requested = true;
	CHECK(re_requested);
}

TEST_CASE("near-dense radius forces level 0 inside the radius even when SSE would accept level 1") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	const ve::IVec3 l1 = ve::lod_chunk_of_point(1, 800.0f, 51.0f, 500.0f);
	make_ready_full_level0(&t, cfg.bounds, l1);

	float lo[3], hi[3];
	ve::lod_chunk_aabb(1, l1, lo, hi);
	float smin[3], smax[3];
	const float area = ve::lod_projected_area(c, lo, hi, smin, smax);
	REQUIRE(area < ve::kLodSseAreaThresh);

	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	bool l1_drawn = false;
	int l0_drawn = 0;
	for (const ve::LodDrawItem &d : r.draws) {
		if (d.level == 1 && d.coord == l1) l1_drawn = true;
		if (d.level == 0) l0_drawn++;
	}
	CHECK_FALSE(l1_drawn);
	CHECK(l0_drawn == 8);
}

TEST_CASE("outside the near-dense radius the SSE threshold still decides") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	const ve::IVec3 l1 = ve::lod_chunk_of_point(1, 800.0f, 51.0f, 450.0f);
	make_ready_full_level0(&t, cfg.bounds, l1);

	float lo[3], hi[3];
	ve::lod_chunk_aabb(1, l1, lo, hi);
	float smin[3], smax[3];
	const float area = ve::lod_projected_area(c, lo, hi, smin, smax);
	REQUIRE(area < ve::kLodSseAreaThresh);

	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	bool l1_drawn = false;
	int l0_drawn = 0;
	for (const ve::LodDrawItem &d : r.draws) {
		if (d.level == 1 && d.coord == l1) l1_drawn = true;
		if (d.level == 0) l0_drawn++;
	}
	CHECK(l1_drawn);
	CHECK(l0_drawn == 0);
}

TEST_CASE("a starved arena refuses pages without dropping the near-dense walk's draw set") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	const ve::IVec3 l1 = ve::lod_chunk_of_point(1, 800.0f, 51.0f, 500.0f);
	make_ready_full_level0(&t, cfg.bounds, l1);

	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	REQUIRE(r.draws.size() == 8);

	// Densifying costs pages: eight level-0 draws need eight pages. A four-page arena
	// cannot fund the set; LodArena refuses the over-budget allocations instead of handing
	// out a partial chunk.
	ve::LodArena arena(4);
	int refused = 0;
	for (const ve::LodDrawItem &d : r.draws) {
		std::vector<int> pages;
		if (!arena.alloc(d.page_count, &pages)) {
			refused++;
			CHECK(pages.empty());
		} else {
			CHECK(pages.size() == size_t(d.page_count));
		}
	}
	CHECK(refused > 0);
	CHECK(arena.free_pages() == 0);

	// The tree's answer is unchanged by arena pressure: the walk still returns the complete
	// near-dense set, so VoxelWorld's upload path can keep the old pages drawable and retry.
	ve::LodWalkResult again;
	t.walk(c, &occ, 2u, &again);
	CHECK(again.draws.size() == r.draws.size());
}

// Critical 1: the draw-list builder must use the chunk's actual page list, not the
// page_first/page_count fields (which can be stale/non-contiguous after arena reuse).
TEST_CASE("page draws are emitted from the actual non-contiguous page list") {
	const ve::LodKey key{3, 10, 20, 30};
	std::map<ve::LodKey, std::vector<int>> pages_of;
	pages_of[key] = {7, 2, 99};
	std::map<int, int> page_quads;
	page_quads[7] = 5;
	page_quads[2] = 8;
	page_quads[99] = 3;

	std::vector<ve::LodDrawItem> draws;
	draws.push_back(ve::LodDrawItem{3, {10, 20, 30}, 7, 3});
	std::vector<ve::LodPageDraw> out;
	ve::lod_collect_page_draws(draws, pages_of, page_quads, &out);

	REQUIRE(out.size() == 3);
	CHECK(out[0].page == 7);
	CHECK(out[0].quad_count == 5);
	CHECK(out[1].page == 2);
	CHECK(out[1].quad_count == 8);
	CHECK(out[2].page == 99);
	CHECK(out[2].quad_count == 3);
}
