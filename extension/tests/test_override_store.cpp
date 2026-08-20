#include <doctest/doctest.h>
#include "world/override_store.h"
#include "world/brick_eval.h"
#include "generator/generator.h"
#include <cmath>

namespace {

ve::EditOp sphere_sub(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

// Bake one brick's lattice out of the generator plus ops, exactly as the GPU pass will.
void bake(const ve::Generator &gen, const ve::EditOp *ops, int n, ve::IVec3 brick,
		ve::OverrideBrick *out) {
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const float wx = (static_cast<float>(brick.x) * ve::kBrickVoxels + x) * ve::kVoxelSize;
				const float wy = (static_cast<float>(brick.y) * ve::kBrickVoxels + y) * ve::kVoxelSize;
				const float wz = (static_cast<float>(brick.z) * ve::kBrickVoxels + z) * ve::kVoxelSize;
				const ve::Sample s = ve::eval_field(gen, ops, n, wx, wy, wz);
				out->sdf[ve::sdf_index(x, y, z)] = ve::encode_sdf(s.sdf);
			}
	for (int z = 0; z < ve::kBrickVoxels; z++)
		for (int y = 0; y < ve::kBrickVoxels; y++)
			for (int x = 0; x < ve::kBrickVoxels; x++) {
				const float wx = (static_cast<float>(brick.x) * ve::kBrickVoxels + x + 0.5f) * ve::kVoxelSize;
				const float wy = (static_cast<float>(brick.y) * ve::kBrickVoxels + y + 0.5f) * ve::kVoxelSize;
				const float wz = (static_cast<float>(brick.z) * ve::kBrickVoxels + z + 0.5f) * ve::kVoxelSize;
				const ve::Sample s = ve::eval_field(gen, ops, n, wx, wy, wz);
				out->mat[x + y * ve::kBrickVoxels + z * ve::kBrickVoxels * ve::kBrickVoxels] =
						static_cast<uint8_t>(s.material & 0xFFu);
			}
}

} // namespace

TEST_CASE("a slot is handed out once and found again") {
	ve::OverrideStore store(4);
	const ve::IVec3 b{30, 64, 30};
	const int slot = store.acquire(b);
	CHECK(slot >= 0);
	CHECK(store.acquire(b) == slot); // idempotent: a re-consolidation reuses the brick's slot
	CHECK(store.slot_of(b) == slot);
	CHECK(store.slot_of({0, 0, 0}) == -1);
	CHECK(store.used() == 1);
	store.release(b);
	CHECK(store.slot_of(b) == -1);
	CHECK(store.used() == 0);
}

TEST_CASE("a full pool refuses rather than evicting") {
	// Fail-soft (spec §8): a refused consolidation leaves the op list exactly as it was, and
	// the region keeps working. Evicting somebody else's override would corrupt the world.
	ve::OverrideStore store(2);
	CHECK(store.acquire({0, 0, 0}) >= 0);
	CHECK(store.acquire({1, 0, 0}) >= 0);
	CHECK(store.acquire({2, 0, 0}) == -1);
	CHECK(store.used() == 2);
}

TEST_CASE("sampling an override reproduces the field it baked") {
	ve::AnalyticGenerator gen;
	const ve::EditOp ops[1] = {sphere_sub(24.4f, 51.4f, 24.4f, 1.5f)};
	const ve::IVec3 brick{30, 64, 30};
	ve::OverrideStore store(4);
	const int slot = store.acquire(brick);
	bake(gen, ops, 1, brick, store.data(slot));

	// At a lattice point the stored value is exact to the encoding's step (~5 mm).
	const float px = static_cast<float>(brick.x) * ve::kBrickSize + 4 * ve::kVoxelSize;
	const float py = static_cast<float>(brick.y) * ve::kBrickSize + 4 * ve::kVoxelSize;
	const float pz = static_cast<float>(brick.z) * ve::kBrickSize + 4 * ve::kVoxelSize;
	ve::Sample got{};
	CHECK(store.sample(px, py, pz, &got));
	const ve::Sample want = ve::eval_field(gen, ops, 1, px, py, pz);
	// OverrideBrick stores the same encoded narrow-band SDF as the atlas; values outside
	// kSdfRange are necessarily clamped before sampling.
	CHECK(got.sdf == doctest::Approx(ve::decode_sdf(ve::encode_sdf(want.sdf))).epsilon(0.02));
}

TEST_CASE("a point outside every override brick is not claimed") {
	ve::OverrideStore store(4);
	const ve::IVec3 brick{30, 64, 30};
	store.acquire(brick);
	ve::Sample got{};
	// One brick over: the store must say "not mine" so the caller falls back to G, or the
	// world would gain a 0.8 m box of zeroes wherever a consolidation stopped.
	CHECK_FALSE(store.sample(static_cast<float>(brick.x + 2) * ve::kBrickSize,
			static_cast<float>(brick.y) * ve::kBrickSize,
			static_cast<float>(brick.z) * ve::kBrickSize, &got));
}

TEST_CASE("the plan covers every brick an op can reach and no more") {
	std::vector<ve::EditOp> ops;
	ops.push_back(sphere_sub(24.4f, 51.4f, 24.4f, 1.5f));
	std::vector<ve::IVec3> bricks;
	ve::plan_consolidation(ops.data(), 1, {0, 2, 0}, &bricks);
	CHECK(!bricks.empty());
	// Every planned brick is inside the region...
	for (const ve::IVec3 &b : bricks) {
		CHECK((b.x >> 5) == 0);
		CHECK((b.y >> 5) == 2);
		CHECK((b.z >> 5) == 0);
	}
	// ...and every brick the op reaches is planned. The check that matters is the second
	// direction: a missed brick is an edit that silently un-happens at consolidation time.
	const float pad = ve::kSdfRange + ve::kVoxelSize;
	for (int bz = 0; bz < 32; bz++)
		for (int by = 0; by < 32; by++)
			for (int bx = 0; bx < 32; bx++) {
				const ve::IVec3 b{bx, 64 + by, bz};
				float lo[3], hi[3];
				ve::brick_world_aabb(b, lo, hi);
				if (!ve::op_touches_aabb(ops[0], lo, hi, pad)) continue;
				bool found = false;
				for (const ve::IVec3 &p : bricks)
					if (p.x == b.x && p.y == b.y && p.z == b.z) { found = true; break; }
				CHECK(found);
			}
}

TEST_CASE("eval_field prefers an override over the generator") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 brick{30, 64, 30};
	ve::OverrideStore store(4);
	const int slot = store.acquire(brick);
	// Bake "solid rock everywhere" into the brick, which the generator would never produce
	// there, so the preference is unmistakable.
	for (int i = 0; i < ve::kBrickSdfCount; i++) store.data(slot)->sdf[i] = ve::encode_sdf(-0.5f);
	for (int i = 0; i < ve::kBrickVoxelCount; i++) store.data(slot)->mat[i] = 2;

	const float px = static_cast<float>(brick.x) * ve::kBrickSize + 0.4f;
	const float py = static_cast<float>(brick.y) * ve::kBrickSize + 0.4f;
	const float pz = static_cast<float>(brick.z) * ve::kBrickSize + 0.4f;
	const ve::Sample s = ve::eval_field(gen, nullptr, 0, px, py, pz, nullptr, &store);
	CHECK(s.sdf == doctest::Approx(-0.5f).epsilon(0.02));
	CHECK(s.material == 2u);
}

TEST_CASE("ops still apply on top of an override") {
	// Consolidation clears the list, but the NEXT edit lands on the baked base. If ops were
	// applied to G instead, every edit after a consolidation would undo it.
	ve::AnalyticGenerator gen;
	const ve::IVec3 brick{30, 64, 30};
	ve::OverrideStore store(4);
	const int slot = store.acquire(brick);
	for (int i = 0; i < ve::kBrickSdfCount; i++) store.data(slot)->sdf[i] = ve::encode_sdf(-0.5f);
	for (int i = 0; i < ve::kBrickVoxelCount; i++) store.data(slot)->mat[i] = 2;

	const float px = static_cast<float>(brick.x) * ve::kBrickSize + 0.4f;
	const float py = static_cast<float>(brick.y) * ve::kBrickSize + 0.4f;
	const float pz = static_cast<float>(brick.z) * ve::kBrickSize + 0.4f;
	const ve::EditOp cut[1] = {sphere_sub(px, py, pz, 1.0f)};
	const ve::Sample s = ve::eval_field(gen, cut, 1, px, py, pz, nullptr, &store);
	CHECK(s.sdf > 0.0f); // the sphere carved the baked rock away
}
