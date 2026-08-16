#include "connectivity/occupancy.h"
#include "generator/volume_set.h"
#include "world/brick_eval.h"
#include "world/raycast.h"
#include <doctest/doctest.h>
#include <cmath>
#include <vector>

using namespace ve;

namespace {

// A volume holding a sphere of radius r centred in its own lattice, so the tests can check
// the sampler against an analytic answer rather than against itself.
VolumeData ball_volume(int dim, float voxel, float r, uint8_t material) {
	VolumeData v;
	v.dim = dim;
	v.sdf.assign(static_cast<size_t>(dim) * dim * dim, 0);
	v.mat.assign(static_cast<size_t>(dim) * dim * dim, 0);
	const float c = 0.5f * static_cast<float>(dim - 1) * voxel;
	for (int z = 0; z < dim; z++)
		for (int y = 0; y < dim; y++)
			for (int x = 0; x < dim; x++) {
				const float dx = x * voxel - c, dy = y * voxel - c, dz = z * voxel - c;
				const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - r;
				const int i = VolumeSet::voxel_index(dim, x, y, z);
				v.sdf[i] = encode_sdf(d);
				v.mat[i] = d <= 0.0f ? material : 0;
				if (d <= 0.0f) v.solid_voxels++;
			}
	return v;
}

} // namespace

TEST_CASE("a box op round-trips its cell range through 32 bytes") {
	const EditOp op = make_box_subtract({3, -2, 7}, {5, -2, 10});
	CHECK(op.type == kOpBoxSubtract);
	CHECK(sizeof(EditOp) == 32);
	float lo[3], hi[3];
	op_world_aabb(op, lo, hi);
	CHECK(lo[0] == doctest::Approx(3.0f * kOccupancyCellSize));
	CHECK(lo[1] == doctest::Approx(-2.0f * kOccupancyCellSize));
	CHECK(hi[0] == doctest::Approx(6.0f * kOccupancyCellSize));
	CHECK(hi[1] == doctest::Approx(-1.0f * kOccupancyCellSize));
	CHECK(hi[2] == doctest::Approx(11.0f * kOccupancyCellSize));
	int nx = 0, ny = 0, nz = 0;
	unpack_extent3(op.aux[0], &nx, &ny, &nz);
	CHECK(nx == 3);
	CHECK(ny == 1);
	CHECK(nz == 4);
}

TEST_CASE("make_box_subtract normalises an inverted cell range") {
	const EditOp op = make_box_subtract({5, -2, 10}, {3, -2, 7});
	CHECK(op.type == kOpBoxSubtract);
	// The op must name the same cells as the correctly ordered range, not silently
	// collapse into a one-cell box at the original (now meaningless) lo corner.
	CHECK(op.pos[0] == doctest::Approx(3.0f * kOccupancyCellSize));
	CHECK(op.pos[1] == doctest::Approx(-2.0f * kOccupancyCellSize));
	CHECK(op.pos[2] == doctest::Approx(7.0f * kOccupancyCellSize));
	int nx = 0, ny = 0, nz = 0;
	unpack_extent3(op.aux[0], &nx, &ny, &nz);
	CHECK(nx == 3);
	CHECK(ny == 1);
	CHECK(nz == 4);
}

TEST_CASE("extent packing survives every value it must carry") {
	for (int n : {1, 2, 63, 64, 1023}) {
		int a = 0, b = 0, c = 0;
		unpack_extent3(pack_extent3(n, 1, 1023), &a, &b, &c);
		CHECK(a == n);
		CHECK(b == 1);
		CHECK(c == 1023);
	}
}

TEST_CASE("box_sdf is the exact distance to an axis-aligned box") {
	const float lo[3] = {0.0f, 0.0f, 0.0f};
	const float hi[3] = {2.0f, 2.0f, 2.0f};
	CHECK(box_sdf(lo, hi, 1.0f, 1.0f, 1.0f) == doctest::Approx(-1.0f)); // centre
	CHECK(box_sdf(lo, hi, 3.0f, 1.0f, 1.0f) == doctest::Approx(1.0f));  // off one face
	CHECK(box_sdf(lo, hi, 3.0f, 3.0f, 1.0f) == doctest::Approx(std::sqrt(2.0f)));
	CHECK(box_sdf(lo, hi, 2.0f, 1.0f, 1.0f) == doctest::Approx(0.0f));  // on the face
}

TEST_CASE("a box subtract removes exactly the cells it names") {
	AnalyticGenerator gen;
	// Deep underground, so the base field is solid everywhere in the test region.
	const EditOp op = make_box_subtract({10, 20, 10}, {11, 20, 10});
	CHECK(eval_field(gen, nullptr, 0, 8.4f, 16.4f, 8.4f).sdf < 0.0f);
	// Inside the box: carved.
	CHECK(eval_field(gen, &op, 1, 8.4f, 16.4f, 8.4f).sdf > 0.0f);
	CHECK(eval_field(gen, &op, 1, 8.4f, 16.4f, 8.4f).material == 0);
	// Two cells wide on x (cells 10 and 11 -> world x in [8.0, 9.6)), one on y and z.
	CHECK(eval_field(gen, &op, 1, 9.4f, 16.4f, 8.4f).sdf > 0.0f);
	// Just outside on x: still solid.
	CHECK(eval_field(gen, &op, 1, 10.0f, 16.4f, 8.4f).sdf < 0.0f);
	// Just outside on y: still solid.
	CHECK(eval_field(gen, &op, 1, 8.4f, 15.6f, 8.4f).sdf < 0.0f);
}

TEST_CASE("a box subtract's re-mark ranges cover every cell it can flip") {
	const EditOp op = make_box_subtract({10, 20, 10}, {11, 20, 10});
	IVec3 lo{}, hi{};
	op_brick_range(op, &lo, &hi);
	// The box spans bricks 10..11 on x; the pad (kActivationPad + kVoxelSize = 0.2 m) is
	// under a brick, so the range is one brick out on every side.
	CHECK(lo.x <= 9);
	CHECK(hi.x >= 12);
	CHECK(lo.y <= 19);
	CHECK(hi.y >= 21);
	op_region_range(op, &lo, &hi);
	CHECK(lo.x <= 0);
	CHECK(hi.x >= 0);
}

TEST_CASE("a volume op adds exactly what its lattice holds") {
	AnalyticGenerator gen;
	VolumeSet volumes;
	const int slot = volumes.allocate();
	CHECK(slot == 0);
	const float origin[3] = {8.0f, 64.0f, 8.0f}; // above the terrain: base field is air
	VolumeData v = ball_volume(32, 0.05f, 0.4f, 2);
	CHECK(v.solid_voxels > 0);
	CHECK(volumes.store(slot, std::move(v)));
	const EditOp op = make_volume_add(slot, origin, 0.05f, 32);
	CHECK(op.type == kOpVolumeAdd);
	CHECK(op.radius == doctest::Approx(0.05f));
	CHECK(op.aux[0] == 0u);
	CHECK(op.aux[1] == 32u);

	// The lattice centre: 0.5 * 31 * 0.05 = 0.775 m in from the origin.
	const float cx = 8.0f + 0.775f, cy = 64.0f + 0.775f, cz = 8.0f + 0.775f;
	CHECK(eval_field(gen, nullptr, 0, cx, cy, cz).sdf > 0.0f); // air without the op
	const Sample s = eval_field(gen, &op, 1, cx, cy, cz, &volumes);
	CHECK(s.sdf < 0.0f);
	// The sphere centre sits halfway between lattice nodes 15 and 16; trilinear
	// interpolation there reads the shared node distance sqrt(3 * 0.025^2) - 0.4,
	// quantised to uint8.
	CHECK(s.sdf == doctest::Approx(-0.3589f).epsilon(0.01));
	CHECK(s.material == 2);
	// Outside the ball but inside the lattice: air, and the distance is about right.
	CHECK(eval_field(gen, &op, 1, cx + 0.6f, cy, cz, &volumes).sdf ==
			doctest::Approx(0.2f).epsilon(0.1));
	// Far outside the lattice: the op contributes a positive distance and nothing else.
	CHECK(eval_field(gen, &op, 1, cx + 20.0f, cy, cz, &volumes).sdf > 0.0f);
}

TEST_CASE("raycast with volumes misses the lattice apron and only hits solid") {
	AnalyticGenerator gen;
	EditLog log(WorldBounds{{0, -64, 0}, {64, 8, 64}});
	VolumeSet volumes;
	const int slot = volumes.allocate();
	REQUIRE(slot == 0);
	REQUIRE(volumes.store(slot, ball_volume(32, 0.05f, 0.4f, 2)));
	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const EditOp op = make_volume_add(slot, origin, 0.05f, 32);
	REQUIRE_FALSE(log.append(op).touched.empty());

	// The ray passes 5 mm outside the lattice's top corner, far from the ball inside.
	// The volume op unions the lattice's box distance into the field there, so an
	// f < hit_eps test reads empty air as a surface; only a sign crossing is real.
	const float o[3] = {9.555f, 70.0f, 9.555f};
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const RayHit h = raycast(gen, log, o, d, 6.0f, &volumes);
	CHECK_FALSE(h.hit);

	// Control: a ray through the ball's centre still reports the real surface, so the
	// sign-crossing test above is rejecting only the apron, not volume hits in general.
	const float co[3] = {8.775f, 70.0f, 8.775f};
	const RayHit solid = raycast(gen, log, co, d, 20.0f, &volumes);
	REQUIRE(solid.hit);
	CHECK(solid.pos[1] < 66.0f);
	CHECK(solid.pos[1] > 65.0f);
}

TEST_CASE("raycast with an empty VolumeStore keeps the pure-field hit_eps behavior") {
	AnalyticGenerator gen;
	EditLog log(WorldBounds{{0, -64, 0}, {64, 8, 64}});
	VolumeSet volumes; // non-null, but no volume op exists anywhere in the log
	// The ray starts at y = 47.603, where the field is +0.0262, and the tracer's
	// min_step lands it at y = 47.578, where the field is +0.00119 -- positive, but
	// within hit_eps (0.2 * kVoxelSize = 0.01). The hit_eps rule must still fire at
	// that sample; a sign-crossing rule would only hit 2.5 cm further down.
	const float o[3] = {100.0f, 47.603f, 100.0f};
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const RayHit without = raycast(gen, log, o, d, 2.0f);
	const RayHit with_empty = raycast(gen, log, o, d, 2.0f, &volumes);
	REQUIRE(without.hit);
	REQUIRE(with_empty.hit);
	CHECK(with_empty.distance == doctest::Approx(without.distance));
	CHECK(with_empty.pos[1] == doctest::Approx(without.pos[1]));
	const float f = eval_field(gen, nullptr, 0, with_empty.pos[0], with_empty.pos[1],
			with_empty.pos[2]).sdf;
	CHECK(f > 0.0f);
	CHECK(f < 0.2f * kVoxelSize);
}

TEST_CASE("raycast with a released volume slot keeps the pure-field hit_eps behavior") {
	AnalyticGenerator gen;
	const WorldBounds bounds{{0, -64, 0}, {64, 8, 64}};
	EditLog log(bounds);
	VolumeSet volumes;
	const int slot = volumes.allocate();
	REQUIRE(slot == 0);
	// The op still exists in the region's list, but its slot is gone. apply_op
	// fail-softs that to a no-op, so the raycast must not switch to the stricter
	// sign-crossing rule; it still has to hit at the classic f < hit_eps sample.
	REQUIRE(volumes.release(slot));
	const float op_origin[3] = {100.0f, 25.7f, 100.0f}; // region (3,1,3), off the ray's path
	const EditOp op = make_volume_add(slot, op_origin, 0.05f, 32);
	REQUIRE_FALSE(log.append(op).touched.empty());

	const EditLog no_op_log(bounds);
	const float o[3] = {100.0f, 47.603f, 100.0f};
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const RayHit no_op = raycast(gen, no_op_log, o, d, 2.0f);
	const RayHit with_dead = raycast(gen, log, o, d, 2.0f, &volumes);
	REQUIRE(no_op.hit);
	REQUIRE(with_dead.hit);
	CHECK(with_dead.distance == doctest::Approx(no_op.distance));
	CHECK(with_dead.pos[1] == doctest::Approx(no_op.pos[1]));
	const float f = eval_field(gen, nullptr, 0, with_dead.pos[0], with_dead.pos[1],
			with_dead.pos[2]).sdf;
	CHECK(f > 0.0f);
	CHECK(f < 0.2f * kVoxelSize);
}

TEST_CASE("a volume op with no store, or a released slot, is a no-op") {
	AnalyticGenerator gen;
	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const EditOp op = make_volume_add(0, origin, 0.05f, 32);
	const float cx = 8.775f, cy = 64.775f, cz = 8.775f;
	// Fail-soft (spec §8): a missing volume warns nowhere and changes nothing, rather than
	// putting a block of undefined bytes into the terrain.
	CHECK(eval_field(gen, &op, 1, cx, cy, cz, nullptr).sdf > 0.0f);
	VolumeSet volumes;
	CHECK(eval_field(gen, &op, 1, cx, cy, cz, &volumes).sdf > 0.0f);
	// A released slot must be indistinguishable from an empty one: the op names a slot
	// that no longer holds a volume.
	const int slot = volumes.allocate();
	REQUIRE(slot == 0);
	CHECK(volumes.release(slot));
	CHECK(volumes.live_count() == 0);
	CHECK(eval_field(gen, &op, 1, cx, cy, cz, &volumes).sdf > 0.0f);
}

TEST_CASE("a negative volume slot stays out of range instead of aliasing slot 0") {
	AnalyticGenerator gen;
	VolumeSet volumes;
	const int slot0 = volumes.allocate();
	CHECK(slot0 == 0);
	// Fill the pool so the old clamp-to-zero path would find a live, populated slot 0.
	for (int i = 1; i < kMaxVolumes; i++) CHECK(volumes.allocate() >= 0);
	CHECK(volumes.allocate() == -1);
	CHECK(volumes.store(slot0, ball_volume(32, 0.05f, 0.4f, 2)));

	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const float cx = 8.775f, cy = 64.775f, cz = 8.775f;
	const EditOp op = make_volume_add(-1, origin, 0.05f, 32);
	CHECK(op.aux[0] == 0xFFFFFFFFu);
	// Slot 0 holds a ball that would read solid at this point; the op names slot -1,
	// so fail-soft means it contributes nothing and the air field survives.
	CHECK(eval_field(gen, &op, 1, cx, cy, cz, &volumes).sdf > 0.0f);
}

TEST_CASE("make_volume_add clamps a non-positive voxel pitch") {
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	CHECK(make_volume_add(0, origin, 0.0f, 32).radius == doctest::Approx(kVoxelSize));
	CHECK(make_volume_add(0, origin, -0.1f, 32).radius == doctest::Approx(kVoxelSize));
}

TEST_CASE("volume data is empty unless both lattices are present") {
	VolumeData v;
	v.dim = 2;
	v.sdf.assign(8, 0);
	CHECK(v.empty()); // mat missing
	v.sdf.clear();
	v.mat.assign(8, 0);
	CHECK(v.empty()); // sdf missing
	v.sdf.assign(8, 0);
	CHECK(!v.empty());
}

TEST_CASE("store rejects volumes whose lattices are not exactly dim^3") {
	VolumeSet volumes;
	const int slot = volumes.allocate();
	CHECK(slot == 0);
	const int64_t ver = volumes.version(slot);

	VolumeData short_sdf;
	short_sdf.dim = 4;
	short_sdf.sdf.assign(63, 0);
	short_sdf.mat.assign(64, 0);
	CHECK(!volumes.store(slot, std::move(short_sdf)));
	CHECK(volumes.version(slot) == ver);
	CHECK(volumes.get(slot) == nullptr);

	VolumeData short_mat;
	short_mat.dim = 4;
	short_mat.sdf.assign(64, 0);
	short_mat.mat.assign(63, 0);
	CHECK(!volumes.store(slot, std::move(short_mat)));
	CHECK(volumes.version(slot) == ver);
	CHECK(volumes.get(slot) == nullptr);

	VolumeData bad_dim;
	bad_dim.dim = -1;
	bad_dim.sdf.assign(1, 0);
	bad_dim.mat.assign(1, 0);
	CHECK(!volumes.store(slot, std::move(bad_dim)));
	CHECK(volumes.get(slot) == nullptr);

	VolumeData dim_one;
	dim_one.dim = 1;
	dim_one.sdf.assign(1, 0);
	dim_one.mat.assign(1, 0);
	CHECK(!volumes.store(slot, std::move(dim_one)));
	CHECK(volumes.version(slot) == ver);
	CHECK(volumes.get(slot) == nullptr);

	VolumeData good;
	good.dim = 4;
	good.sdf.assign(64, 0);
	good.mat.assign(64, 0);
	CHECK(volumes.store(slot, std::move(good)));
	CHECK(volumes.version(slot) > ver);
	CHECK(volumes.get(slot) != nullptr);
}

TEST_CASE("the volume pool hands out every slot once and takes them back") {
	VolumeSet volumes;
	std::vector<int> slots;
	for (int i = 0; i < kMaxVolumes; i++) {
		const int s = volumes.allocate();
		CHECK(s >= 0);
		slots.push_back(s);
	}
	CHECK(volumes.allocate() == -1);
	CHECK(volumes.live_count() == kMaxVolumes);
	CHECK(volumes.release(slots[7]));
	CHECK(volumes.live_count() == kMaxVolumes - 1);
	// Releasing frees the bytes: a 64-slot pool of 64^3 volumes is 33 MB and the manager
	// leans on release() to stay under spec §5's island-texture cap.
	CHECK(volumes.get(slots[7]) == nullptr);
	CHECK(volumes.allocate() == slots[7]);
	CHECK(volumes.live_count() == kMaxVolumes);
}

TEST_CASE("a slot can be claimed by index, once") {
	VolumeSet volumes;
	CHECK(volumes.reserve(3));
	CHECK(volumes.reserve(3) == false);
	CHECK(volumes.live_count() == 1);
	for (int i = 0; i < kMaxVolumes - 1; i++) CHECK(volumes.allocate() != 3);
}

TEST_CASE("a pinned slot can never be released or handed out again") {
	VolumeSet volumes;
	const int slot = volumes.allocate();
	CHECK(volumes.pin(slot));
	CHECK(volumes.pinned(slot));
	CHECK_FALSE(volumes.release(slot));
	// Still live: an op in the edit log names this slot, and the GPU mirrors have no
	// liveness flag -- reusing it would silently swap one piece of rubble for another.
	CHECK(volumes.live_count() == 1);
	for (int i = 0; i < kMaxVolumes - 1; i++) CHECK(volumes.allocate() != slot);
	CHECK(volumes.allocate() == -1);
}

TEST_CASE("pin refuses a free slot") {
	VolumeSet volumes;
	CHECK(!volumes.pin(0));
	CHECK(!volumes.pinned(0));
	const int slot = volumes.allocate();
	CHECK(volumes.pin(slot));
	CHECK(volumes.pinned(slot));
}

TEST_CASE("resample_volume rejects a malformed short-vector VolumeData") {
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	const EditOp src_op = make_volume_add(0, origin, 0.05f, 64);
	const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	const float at[3] = {0.0f, 0.0f, 0.0f};
	const size_t n = static_cast<size_t>(64) * 64 * 64;

	VolumeData bad;
	bad.dim = 64;
	bad.sdf.assign(n - 1, 0);
	bad.mat.assign(n, 0);
	VolumeData out;
	EditOp out_op{};
	CHECK(!resample_volume(bad, src_op, identity, at, 0, kIslandDim, &out, &out_op));

	bad.sdf.assign(n, 0);
	bad.mat.assign(n - 1, 0);
	CHECK(!resample_volume(bad, src_op, identity, at, 0, kIslandDim, &out, &out_op));

	bad.dim = 1;
	bad.sdf.assign(1, 0);
	bad.mat.assign(1, 0);
	CHECK(!resample_volume(bad, src_op, identity, at, 0, kIslandDim, &out, &out_op));
}

TEST_CASE("resample_volume rejects a non-positive source voxel pitch") {
	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const VolumeData src = ball_volume(32, 0.05f, 0.4f, 2);
	// make_volume_add clamps non-positive pitches, so build an op the way a malformed
	// edit log could: a kOpVolumeAdd whose radius is zero or negative.
	EditOp src_op = make_volume_add(0, origin, 0.05f, 32);
	src_op.radius = 0.0f;
	const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	const float at[3] = {0.0f, 0.0f, 0.0f};
	VolumeData out;
	EditOp out_op{};
	CHECK(!resample_volume(src, src_op, identity, at, 0, kIslandDim, &out, &out_op));

	src_op.radius = -0.05f;
	CHECK(!resample_volume(src, src_op, identity, at, 0, kIslandDim, &out, &out_op));
}

TEST_CASE("resample_volume rejects a source op that is not a volume add") {
	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const VolumeData src = ball_volume(32, 0.05f, 0.4f, 2);
	EditOp src_op = make_volume_add(0, origin, 0.05f, 32);
	src_op.type = kOpSphereAdd; // a malformed edit log could hand us any op type
	const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	const float at[3] = {0.0f, 0.0f, 0.0f};
	VolumeData out;
	EditOp out_op{};
	CHECK(!resample_volume(src, src_op, identity, at, 0, kIslandDim, &out, &out_op));
}

TEST_CASE("resample_volume rejects a non-orthonormal basis") {
	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const VolumeData src = ball_volume(32, 0.05f, 0.4f, 2);
	const EditOp src_op = make_volume_add(0, origin, 0.05f, 32);
	const float at[3] = {0.0f, 0.0f, 0.0f};
	VolumeData out;
	EditOp out_op{};

	float scaled[9] = {2, 0, 0, 0, 1, 0, 0, 0, 1};
	CHECK(!resample_volume(src, src_op, scaled, at, 0, kIslandDim, &out, &out_op));

	float skew[9] = {1, 0, 0, 0, 1, 0, 0.5f, 0, 1};
	CHECK(!resample_volume(src, src_op, skew, at, 0, kIslandDim, &out, &out_op));
}

TEST_CASE("eval_brick threads a volume op through a VolumeStore") {
	AnalyticGenerator gen;
	VolumeSet volumes;
	const int slot = volumes.allocate();
	CHECK(volumes.store(slot, ball_volume(32, 0.05f, 0.4f, 2)));

	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const EditOp op = make_volume_add(slot, origin, 0.05f, 32);
	// Brick (10, 80, 10) has its origin at (8, 64, 8), so its lattice contains the
	// ball centre at local (0.775, 0.775, 0.775).
	const IVec3 brick{10, 80, 10};
	BrickEval air{}, filled{};
	eval_brick(gen, &op, 1, brick, &air, nullptr);
	eval_brick(gen, &op, 1, brick, &filled, &volumes);

	const int sdf_at = sdf_index(15, 15, 15); // local (0.75, 0.75, 0.75): inside the ball
	CHECK(decode_sdf(air.brick.sdf[sdf_at]) > 0.0f);
	CHECK(decode_sdf(filled.brick.sdf[sdf_at]) < 0.0f);

	const int mat_at = voxel_index(15, 15, 15);
	CHECK(filled.brick.palette[get_mat_index(filled.brick, mat_at)] == 2);
}

TEST_CASE("a volume resampled through the identity transform reproduces itself") {
	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const VolumeData src = ball_volume(32, 0.05f, 0.4f, 2);
	// The body's local frame IS the birth world frame shifted to the lattice origin.
	const EditOp src_op = make_volume_add(0, origin, 0.05f, 32);
	const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	const float at[3] = {0.0f, 0.0f, 0.0f};

	VolumeData out;
	EditOp out_op{};
	CHECK(resample_volume(src, src_op, identity, at, 3, kIslandDim, &out, &out_op));
	CHECK(out_op.aux[0] == 3u);
	CHECK(out_op.radius == doctest::Approx(kIslandVoxelFine));
	CHECK(out.solid_voxels > 0);
	// The ball is still where it was, to within a voxel.
	const float cx = 8.775f, cy = 64.775f, cz = 8.775f;
	VolumeSample a{}, b{};
	CHECK(sample_volume_lattice(src.sdf.data(), src.mat.data(), src.dim, src_op.pos,
			src_op.radius, cx, cy, cz, &a));
	CHECK(sample_volume_lattice(out.sdf.data(), out.mat.data(), out.dim, out_op.pos,
			out_op.radius, cx, cy, cz, &b));
	CHECK(b.sdf == doctest::Approx(a.sdf).epsilon(0.15));
	CHECK(b.material == a.material);
}

TEST_CASE("a volume resampled through a rotation lands where the transform puts it") {
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	const VolumeData src = ball_volume(32, 0.05f, 0.3f, 2);
	const EditOp src_op = make_volume_add(0, origin, 0.05f, 32);
	// 90 degrees about y, then translated 100 m up. Row-major: local (x,y,z) -> world
	// (z, y, -x) + t.
	const float basis[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
	const float at[3] = {5.0f, 100.0f, 5.0f};

	VolumeData out;
	EditOp out_op{};
	CHECK(resample_volume(src, src_op, basis, at, 1, kIslandDim, &out, &out_op));
	// The ball's local centre (0.775, 0.775, 0.775) maps to world
	// (0.775 + 5, 0.775 + 100, -0.775 + 5).
	VolumeSample s{};
	CHECK(sample_volume_lattice(out.sdf.data(), out.mat.data(), out.dim, out_op.pos,
			out_op.radius, 5.775f, 100.775f, 4.225f, &s));
	CHECK(s.sdf == doctest::Approx(-0.3f).epsilon(0.2));
	CHECK(s.material == 2);
	// ...and the ball is NOT at the untransformed place any more.
	VolumeSample t{};
	CHECK(sample_volume_lattice(out.sdf.data(), out.mat.data(), out.dim, out_op.pos,
			out_op.radius, 0.775f, 0.775f, 0.775f, &t));
	CHECK(t.sdf > 0.0f);
}
