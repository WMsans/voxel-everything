#include "generator/volume_set.h"
#include "mesh/box_merge.h"
#include "world/brick_eval.h"
#include <doctest/doctest.h>
#include <cmath>

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
	volumes.store(slot, std::move(v));
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
	volumes.release(slots[7]);
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
	volumes.store(slot, VolumeData{});
	volumes.pin(slot);
	CHECK(volumes.pinned(slot));
	volumes.release(slot);
	// Still live: an op in the edit log names this slot, and the GPU mirrors have no
	// liveness flag -- reusing it would silently swap one piece of rubble for another.
	CHECK(volumes.live_count() == 1);
	for (int i = 0; i < kMaxVolumes - 1; i++) CHECK(volumes.allocate() != slot);
	CHECK(volumes.allocate() == -1);
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
