#include <doctest/doctest.h>
#include "override_bake.h"
#include "connectivity/contact_refine.h"
#include "mesh/chunk_residency.h"
#include "mesh/mesh_chunk.h"
#include "world/world_field.h"
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

namespace {

// One encoded SDF step is 1.28 m / 255 ~= 5 mm; trilinear reconstruction between lattice
// points of a smooth carve adds at most about one more step.
constexpr float kQuantumTolerance = 2.0f * 1.28f / 255.0f;

ve::EditOp sphere_sub(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

// A world whose region (0, 2, 0) holds one carve, and its consolidated twin: the same carve
// baked into override bricks with the region's op list cleared.
struct Worlds {
	ve::AnalyticGenerator gen;
	ve::EditLog live_log, baked_log;
	ve::VolumeSet volumes;
	ve::OverrideStore no_overrides{1};
	ve::OverrideStore overrides{512};
	ve::OverrideTableMap tables;
	std::mutex live_mu, baked_mu;
	std::atomic<int64_t> seq{7};
	const ve::EditOp carve = sphere_sub(12.4f, 54.4f, 12.4f, 1.5f);

	Worlds() {
		live_log.append(carve);
		std::vector<ve::IVec3> bricks;
		ve::plan_consolidation(&carve, 1, {0, 2, 0}, &bricks);
		for (const ve::IVec3 &b : bricks)
			ve_test::bake_override(gen, &carve, 1, b, overrides.data(overrides.acquire(b)));
		tables[{0, 2, 0}] = 3;
	}
	ve::WorldField live() {
		return ve::WorldField(&gen, &live_log, &volumes, &no_overrides, &tables, &live_mu, &seq);
	}
	ve::WorldField baked() {
		return ve::WorldField(&gen, &baked_log, &volumes, &overrides, &tables, &baked_mu, &seq);
	}
};

} // namespace

TEST_CASE("a consolidated region answers every query as its unconsolidated twin") {
	Worlds w;
	const ve::FieldView live = w.live().lock();
	const ve::FieldView baked = w.baked().lock();
	for (float dx = -2.0f; dx <= 2.0f; dx += 0.35f)
		for (float dy = -2.0f; dy <= 2.0f; dy += 0.35f) {
			const float x = 12.4f + dx, y = 54.4f + dy, z = 12.4f + 0.2f;
			const float want = ve::decode_sdf(ve::encode_sdf(live.sample(x, y, z).sdf));
			CHECK(std::fabs(baked.sample(x, y, z).sdf - want) <= kQuantumTolerance);
		}
	const ve::IVec3 chunk = ve::chunk_of_point(12.4f, 54.4f, 12.4f);
	CHECK(baked.has_surface(chunk) == live.has_surface(chunk));
	// The occupancy cell holding the carve's centre: its +y face is inside the pocket.
	const ve::IVec3 cell = ve::brick_of_point(12.4f, 54.4f, 12.4f);
	for (int axis = 0; axis < 3; axis++)
		CHECK(std::abs(baked.contact_samples(cell, axis, 9) - live.contact_samples(cell, axis, 9)) <= 2);
	const float o[3] = {12.4f, 70.0f, 12.4f};
	const float down[3] = {0.0f, -1.0f, 0.0f};
	const ve::RayHit a = live.raycast(o, down, 200.0f);
	const ve::RayHit b = baked.raycast(o, down, 200.0f);
	REQUIRE(a.hit);
	REQUIRE(b.hit);
	CHECK(std::fabs(a.pos[1] - b.pos[1]) <= ve::kVoxelSize);
}

TEST_CASE("a point query evaluates the op list of the region containing it") {
	ve::AnalyticGenerator gen;
	ve::EditLog log;
	ve::VolumeSet volumes;
	ve::OverrideTableMap tables;
	std::mutex mu;
	// A carve deep inside region (1, 2, 0), far from every border and every pad: only that
	// region's list holds it.
	const ve::EditOp far = sphere_sub(38.4f, 51.2f, 12.8f, 1.0f);
	log.append(far);
	REQUIRE(log.op_count({1, 2, 0}) == 1);
	REQUIRE(log.op_count({0, 2, 0}) == 0);
	const ve::FieldView v = ve::WorldField(&gen, &log, &volumes, nullptr, &tables, &mu, nullptr).lock();
	CHECK(v.sample(38.4f, 51.2f, 12.8f).sdf ==
			doctest::Approx(ve::eval_field(gen, &far, 1, 38.4f, 51.2f, 12.8f).sdf));
	CHECK(v.sample(12.8f, 51.2f, 12.8f).sdf == doctest::Approx(gen.sample(12.8f, 51.2f, 12.8f).sdf));
}

TEST_CASE("a lattice snapshot copies ops, overrides, seq and table") {
	Worlds w;
	ve::FieldSnapshot s;
	const float lo[3] = {12.0f, 54.0f, 12.0f}, hi[3] = {12.8f, 54.8f, 12.8f};
	const float origin[3] = {11.6f, 53.6f, 11.6f};
	{
		const ve::FieldView v = w.live().lock();
		REQUIRE(v.snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	}
	CHECK(s.ops.size() == 1);
	CHECK_FALSE(s.over_cap);
	CHECK(s.edit_seq == 7);
	CHECK(s.override_table == 3);
	CHECK(s.sources.overrides.empty()); // the live twin has no overrides
	{
		const ve::FieldView v = w.baked().lock();
		REQUIRE(v.snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	}
	CHECK(s.ops.empty());
	CHECK_FALSE(s.sources.overrides.empty());
}

TEST_CASE("a lattice snapshot across two regions reports the op cap and never truncates") {
	ve::AnalyticGenerator gen;
	ve::EditLog log;
	ve::VolumeSet volumes;
	ve::OverrideStore overrides(1);
	ve::OverrideTableMap tables;
	std::mutex mu;
	// A region holds at most kMaxRegionOps, so the cap is only reachable across a border:
	// 200 ops at x = 23.6 live in region 0 only, the rest at x = 27.6 in region 1 only.
	for (int i = 0; i < 200; i++) log.append(sphere_sub(23.6f, 51.2f + 0.01f * i, 12.8f, 0.1f));
	for (int i = 0; i < ve::kMaxRegionOps - 200; i++)
		log.append(sphere_sub(27.6f, 51.2f + 0.01f * i, 12.8f, 0.1f));
	const float lo[3] = {23.0f, 51.0f, 12.4f}, hi[3] = {28.2f, 54.0f, 13.2f};
	const float origin[3] = {23.0f, 51.0f, 12.4f};
	const ve::WorldField field(&gen, &log, &volumes, &overrides, &tables, &mu, nullptr);
	ve::FieldSnapshot s;
	REQUIRE(field.lock().snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	CHECK(s.ops.size() == static_cast<size_t>(ve::kMaxRegionOps));
	CHECK_FALSE(s.over_cap);
	log.append(sphere_sub(27.6f, 53.9f, 12.8f, 0.1f));
	REQUIRE(field.lock().snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	CHECK(s.ops.size() == static_cast<size_t>(ve::kMaxRegionOps + 1));
	CHECK(s.over_cap);
}

TEST_CASE("a region snapshot is the consolidation job's inputs") {
	Worlds w;
	ve::ConsolidationSnapshot s;
	{
		const ve::FieldView v = w.live().lock();
		REQUIRE(v.snapshot_region({0, 2, 0}, &s));
	}
	REQUIRE(s.ops.size() == 1);
	CHECK(s.through_seq == w.live_log.seqs({0, 2, 0}).back());
	std::vector<ve::IVec3> planned;
	ve::plan_consolidation(&w.carve, 1, {0, 2, 0}, &planned);
	CHECK(s.bricks.size() == planned.size());
}

TEST_CASE("snapshot sources evaluate exactly as the live store they were copied from") {
	Worlds w;
	ve::FieldSnapshot s;
	const float lo[3] = {12.0f, 54.0f, 12.0f}, hi[3] = {12.8f, 54.8f, 12.8f};
	const float origin[3] = {11.6f, 53.6f, 11.6f};
	const ve::FieldView v = w.baked().lock();
	REQUIRE(v.snapshot_lattice(lo, hi, origin, 0.05f, 64, &s));
	const ve::SnapshotSources sources(s.sources);
	REQUIRE(sources.ok);
	const ve::Sample a = ve::eval_field(w.gen, s.ops.data(), static_cast<int>(s.ops.size()),
			12.4f, 54.4f, 12.4f, &sources.volumes, &sources.overrides);
	CHECK(a.sdf == doctest::Approx(v.sample(12.4f, 54.4f, 12.4f).sdf));
}

TEST_CASE("an invalid field answers every query with its empty value") {
	const ve::WorldField none;
	CHECK_FALSE(none.valid());
	const ve::FieldView v = none.lock();
	CHECK_FALSE(v.valid());
	CHECK(v.sample(0.0f, 0.0f, 0.0f).sdf == 0.0f);
	CHECK_FALSE(v.has_surface({0, 0, 0}));
	CHECK(v.contact_samples({0, 0, 0}, 1, 9) == 0);
	const float o[3] = {0.0f, 0.0f, 0.0f}, d[3] = {0.0f, -1.0f, 0.0f};
	CHECK_FALSE(v.raycast(o, d, 10.0f).hit);
	ve::FieldSnapshot fs;
	CHECK_FALSE(v.snapshot_lattice(o, o, o, 0.05f, 2, &fs));
	ve::ConsolidationSnapshot rs;
	CHECK_FALSE(v.snapshot_region({0, 0, 0}, &rs));
	CHECK_FALSE(none.chunk_has_surface({0, 0, 0}));
	CHECK(none.contact_samples({0, 0, 0}, 1, 9) == 0);
}

TEST_CASE("the probe interfaces answer as the free functions over the same sources") {
	Worlds w;
	const ve::WorldField field = w.baked();
	const ve::IVec3 chunk = ve::chunk_of_point(12.4f, 54.4f, 12.4f);
	const std::vector<ve::EditOp> &ops = w.baked_log.ops(ve::region_of_chunk(chunk));
	CHECK(static_cast<const ve::ChunkProbe &>(field).chunk_has_surface(chunk) ==
			ve::chunk_has_surface(w.gen, ops.data(), static_cast<int>(ops.size()), chunk,
					&w.volumes, &w.overrides));
	const ve::IVec3 cell = ve::brick_of_point(12.4f, 54.4f, 12.4f);
	CHECK(static_cast<const ve::ContactProbe &>(field).contact_samples(cell, 1, 9) ==
			ve::contact_samples_field(w.gen, ops.data(), static_cast<int>(ops.size()), cell, 1, 9,
					&w.volumes, &w.overrides));
}
