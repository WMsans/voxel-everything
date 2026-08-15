#include <doctest/doctest.h>
#include "mesh/chunk_residency.h"
#include <algorithm>
#include <vector>

// World: 8 x 4 x 8 regions from brick origin (0, -64, 0) => 204.8 x 102.4 x 204.8 m, so
// chunks run x,z in [0, 16) and y in [-4, 4).
static ve::ChunkResidencyConfig make_cfg(int max_chunks, int builds = 2, int probes = 4096) {
	ve::ChunkResidencyConfig cfg;
	cfg.bounds = ve::WorldBounds{{0, -64, 0}, {8, 4, 8}};
	cfg.radius_m = 64.0f;
	cfg.max_chunks = max_chunks;
	cfg.max_builds_per_frame = builds;
	cfg.max_probes_per_frame = probes;
	return cfg;
}

// Only chunk layer `layer` holds a surface — a flat world, which makes the expected resident
// set something the test can enumerate independently.
struct FakeProbe : ve::ChunkProbe {
	mutable int calls = 0;
	mutable std::vector<ve::IVec3> history;
	int layer = 2;
	bool everything = false;
	bool chunk_has_surface(ve::IVec3 c) const override {
		calls++;
		history.push_back(c);
		return everything || c.y == layer;
	}
};

static void settle(ve::ChunkResidency &r, const ve::ChunkProbe &probe, float x, float y,
		float z, int frames = 400) {
	const float c[3] = {x, y, z};
	for (int i = 0; i < frames; i++) {
		const ve::ChunkPlan p = r.update(c, nullptr, 1, probe);
		for (const auto &b : p.builds) r.note_built(b.chunk);
		if (p.builds.empty() && p.releases.empty()) return;
	}
}

TEST_CASE("the settled set is exactly the in-radius chunks the probe accepted") {
	const auto cfg = make_cfg(256);
	ve::ChunkResidency res(cfg);
	FakeProbe probe;
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	CHECK(res.resident_count() > 20);
	CHECK(res.pending_count() == 0);

	// Everything resident is on the surface layer and inside the radius...
	std::vector<int> slots;
	for (int s = 0; s < cfg.max_chunks; s++) {
		if (!res.slot_resident(s)) continue;
		const ve::IVec3 c = res.chunk_of_slot(s);
		CHECK(c.y == probe.layer);
		CHECK(ve::chunk_distance(c, 100.0f, 30.0f, 100.0f) <= cfg.radius_m);
		slots.push_back(s);
	}
	// ...slots are unique...
	std::sort(slots.begin(), slots.end());
	CHECK(std::adjacent_find(slots.begin(), slots.end()) == slots.end());
	// ...and nothing eligible was left out.
	for (int x = 0; x < 16; x++)
		for (int z = 0; z < 16; z++) {
			const ve::IVec3 c{x, probe.layer, z};
			if (ve::chunk_distance(c, 100.0f, 30.0f, 100.0f) > cfg.radius_m) continue;
			CHECK(res.slot_of(c) >= 0);
		}
}

TEST_CASE("probing is budgeted per frame and cached afterwards") {
	ve::ChunkResidency res(make_cfg(256, 2, 32));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	res.update(c, nullptr, 1, probe);
	CHECK(probe.calls == 32);
	res.update(c, nullptr, 1, probe);
	CHECK(probe.calls == 64);

	// Run until the CACHE stops growing rather than until the plan goes quiet: with the
	// budget this low, the ball still holds unprobed chunks long after the discovered ones
	// have all been built, and settle() would stop at the first such frame.
	int last = -1;
	for (int i = 0; i < 200 && res.probe_cache_size() != last; i++) {
		last = res.probe_cache_size();
		const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
		for (const auto &b : p.builds) res.note_built(b.chunk);
	}
	const int before = probe.calls;
	res.update(c, nullptr, 1, probe);
	CHECK(probe.calls == before); // every chunk in the ball has a cached verdict
	CHECK(res.probe_cache_size() > res.resident_count());
}

TEST_CASE("probing is nearest-first within a budgeted frame") {
	ve::ChunkResidency res(make_cfg(256, 2, 1));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::IVec3 nearest = ve::chunk_of_point(c[0], c[1], c[2]);
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(!probe.history.empty());
	CHECK(probe.history.front() == nearest);
	REQUIRE(!p.builds.empty());
	CHECK(p.builds.front().chunk == nearest);
}

TEST_CASE("builds are throttled, nearest first, and stop once ready") {
	ve::ChunkResidency res(make_cfg(256, 2));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	CHECK(p.builds.size() == 2);
	CHECK(ve::chunk_distance(p.builds[0].chunk, 100.0f, 30.0f, 100.0f) <=
			ve::chunk_distance(p.builds[1].chunk, 100.0f, 30.0f, 100.0f));
	for (const auto &b : p.builds) CHECK(res.slot_of(b.chunk) == b.slot);

	// A build that is still in flight is not handed out again.
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe);
	for (const auto &b : p2.builds)
		for (const auto &prev : p.builds) CHECK_FALSE(b.chunk == prev.chunk);

	// max_builds clamps the config downwards (the caller passes 0 while the mesher is busy).
	CHECK(res.update(c, nullptr, 1, probe, 0).builds.empty());
}

TEST_CASE("the pool caps the resident set and keeps the nearest chunks") {
	ve::ChunkResidency res(make_cfg(8));
	FakeProbe probe;
	probe.everything = true;
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	CHECK(res.resident_count() == 8);
	for (int s = 0; s < 8; s++) {
		CHECK(res.slot_resident(s));
		// The eight nearest chunks all touch the cell holding the centre.
		CHECK(ve::chunk_distance(res.chunk_of_slot(s), 100.0f, 30.0f, 100.0f) <=
				ve::kChunkSize + 0.01f);
	}
}

TEST_CASE("moving out of the hysteresis band releases the slot") {
	const auto cfg = make_cfg(256);
	ve::ChunkResidency res(cfg);
	FakeProbe probe;
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	const int before = res.resident_count();

	// A step that leaves everything inside radius * evict_margin releases nothing.
	const float near_c[3] = {100.0f + cfg.radius_m * 0.1f, 30.0f, 100.0f};
	CHECK(res.update(near_c, nullptr, 1, probe).releases.empty());

	// A step far enough out drops what fell behind, and the count settles again.
	settle(res, probe, 100.0f, 30.0f, 200.0f);
	CHECK(res.resident_count() > 0);
	for (int s = 0; s < cfg.max_chunks; s++)
		if (res.slot_resident(s))
			CHECK(ve::chunk_distance(res.chunk_of_slot(s), 100.0f, 30.0f, 200.0f) <=
					cfg.radius_m * cfg.evict_margin);
	CHECK(before > 0);
}

TEST_CASE("mark_dirty re-plans resident chunks and re-probes cached ones") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	const float c[3] = {100.0f, 30.0f, 100.0f};
	CHECK(res.update(c, nullptr, 1, probe).builds.empty()); // settled: nothing to do

	const ve::IVec3 hit = ve::chunk_of_point(100.0f, 30.0f, 100.0f);
	res.mark_dirty(hit, hit);
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(p.builds.size() == 1);
	CHECK(p.builds[0].chunk == hit);

	// A chunk the probe rejected is re-probed after an edit: a sphere-add in open sky makes
	// surface where the generator had none, and a stale "empty" would hide it for ever.
	const ve::IVec3 sky{hit.x, hit.y + 1, hit.z};
	CHECK(res.slot_of(sky) == -1);
	const int calls = probe.calls;
	res.mark_dirty(sky, sky);
	res.update(c, nullptr, 1, probe);
	CHECK(probe.calls > calls);
}

TEST_CASE("an edit during a build survives the build landing") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(p.builds.size() == 2);
	const ve::IVec3 building = p.builds[0].chunk;

	res.mark_dirty(building, building);   // the edit lands while the mesher is running
	res.note_built(building);             // the pre-edit result arrives
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe, 8);
	bool replanned = false;
	for (const auto &b : p2.builds) replanned = replanned || b.chunk == building;
	CHECK(replanned);
}

TEST_CASE("dirty resident build is not reissued until the old build lands") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(p.builds.size() == 2);
	const ve::IVec3 building = p.builds[0].chunk;

	res.mark_dirty(building, building);
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe);
	for (const auto &b : p2.builds) CHECK_FALSE(b.chunk == building);

	res.note_built(building); // stale pre-edit result arrives
	const ve::ChunkPlan p3 = res.update(c, nullptr, 1, probe, 8);
	bool replanned = false;
	for (const auto &b : p3.builds) replanned = replanned || b.chunk == building;
	CHECK(replanned);
}

TEST_CASE("dirty resident build is not reissued until stale note_empty lands") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(p.builds.size() == 2);
	const ve::IVec3 building = p.builds[0].chunk;

	res.mark_dirty(building, building);
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe);
	for (const auto &b : p2.builds) CHECK_FALSE(b.chunk == building);

	res.note_empty(building); // stale pre-edit result arrives
	const ve::ChunkPlan p3 = res.update(c, nullptr, 1, probe, 8);
	bool replanned = false;
	for (const auto &b : p3.builds) replanned = replanned || b.chunk == building;
	CHECK(replanned);
}

TEST_CASE("an edit during a build does not let note_empty hide the chunk") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(p.builds.size() == 2);
	const ve::IVec3 building = p.builds[0].chunk;

	res.mark_dirty(building, building);   // the edit lands while the mesher is running
	res.note_empty(building);             // the pre-edit build returns no geometry
	CHECK(res.slot_of(building) == -1);

	const int calls = probe.calls;
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe, 8);
	bool replanned = false;
	for (const auto &b : p2.builds) replanned = replanned || b.chunk == building;
	CHECK(replanned);
	CHECK(probe.calls > calls);
}

TEST_CASE("note_empty frees the slot and stops the chunk coming back") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(!p.builds.empty());
	const ve::IVec3 empty = p.builds[0].chunk;
	const int slot = res.note_empty(empty);
	CHECK(slot == p.builds[0].slot);
	CHECK(res.slot_of(empty) == -1);
	for (int i = 0; i < 5; i++) res.update(c, nullptr, 1, probe);
	CHECK(res.slot_of(empty) == -1);

	// ...until something changes the field there.
	res.mark_dirty(empty, empty);
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	CHECK(res.slot_of(empty) >= 0);
}

TEST_CASE("note_failed puts the chunk back in the queue") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(!p.builds.empty());
	res.note_failed(p.builds[0].chunk);
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe, 8);
	bool requeued = false;
	for (const auto &b : p2.builds) requeued = requeued || b.chunk == p.builds[0].chunk;
	CHECK(requeued);
}

TEST_CASE("evicted in-flight builds are not re-added until the old result lands") {
	ve::ChunkResidency res(make_cfg(1, 1));
	FakeProbe probe;
	probe.everything = true;
	const float at[3] = {100.0f, 30.0f, 100.0f};
	const float away[3] = {300.0f, 30.0f, 300.0f};

	const ve::ChunkPlan p0 = res.update(at, nullptr, 1, probe);
	REQUIRE(p0.builds.size() == 1);
	const ve::IVec3 c = p0.builds[0].chunk;

	// Move far enough that C is released while still building; its result is outstanding.
	res.update(away, nullptr, 1, probe, 0);
	CHECK(res.slot_of(c) == -1);

	// Back in range, C must stay out of the candidate set until the old result arrives.
	const ve::ChunkPlan p1 = res.update(at, nullptr, 1, probe);
	CHECK(res.slot_of(c) == -1);
	for (const auto &b : p1.builds) CHECK_FALSE(b.chunk == c);

	res.note_built(c);
	const ve::ChunkPlan p2 = res.update(at, nullptr, 1, probe);
	bool readded = false;
	for (const auto &b : p2.builds) readded = readded || b.chunk == c;
	CHECK(readded);
}

TEST_CASE("stale note_empty clears in-flight without hiding the re-added chunk") {
	ve::ChunkResidency res(make_cfg(1, 1));
	FakeProbe probe;
	probe.everything = true;
	const float at[3] = {100.0f, 30.0f, 100.0f};
	const float away[3] = {300.0f, 30.0f, 300.0f};

	const ve::ChunkPlan p0 = res.update(at, nullptr, 1, probe);
	REQUIRE(p0.builds.size() == 1);
	const ve::IVec3 c = p0.builds[0].chunk;

	res.update(away, nullptr, 1, probe, 0);
	CHECK(res.slot_of(c) == -1);
	res.update(at, nullptr, 1, probe);
	CHECK(res.slot_of(c) == -1);

	// The old build was empty, but it belongs to the evicted build: clear the marker without
	// caching empty for the current field.
	res.note_empty(c);
	const ve::ChunkPlan p2 = res.update(at, nullptr, 1, probe);
	bool readded = false;
	for (const auto &b : p2.builds) readded = readded || b.chunk == c;
	CHECK(readded);
}
