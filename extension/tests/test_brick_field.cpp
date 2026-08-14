// The SDF field the raymarch shader reconstructs must be a faithful, continuous distance
// field over the WHOLE of each brick's [0, BRICK_SIZE) extent -- including the last voxel
// slab, whose trilinear cell needs a lattice point at local coordinate 16 that lives in the
// neighbouring brick. Without that apron the reconstruction clamps, the field goes flat in
// the slab, and calc_normal() produces garbage normals -> dark seams on every brick face.
#include <doctest/doctest.h>
#include "world/world_data.h"
#include "generator/generator.h"
#include <algorithm>
#include <cmath>

namespace {

// Test oracle: mirrors brick_sdf() / world_sdf() in shaders/raymarch.comp.glsl.
struct ShaderField {
	const ve::WorldData &w;

	float brick_sdf(int slot, float lx, float ly, float lz) const {
		const float hi = static_cast<float>(ve::kBrickSdfStride - 1);
		const float px = std::clamp(lx, 0.0f, hi);
		const float py = std::clamp(ly, 0.0f, hi);
		const float pz = std::clamp(lz, 0.0f, hi);
		const int i0x = static_cast<int>(std::floor(px));
		const int i0y = static_cast<int>(std::floor(py));
		const int i0z = static_cast<int>(std::floor(pz));
		const float fx = px - i0x, fy = py - i0y, fz = pz - i0z;
		const int hii = ve::kBrickSdfStride - 1;
		const int i1x = std::min(i0x + 1, hii);
		const int i1y = std::min(i0y + 1, hii);
		const int i1z = std::min(i0z + 1, hii);
		const ve::Brick &b = w.brick(slot);
		auto t = [&](int x, int y, int z) { return ve::decode_sdf(b.sdf[ve::sdf_index(x, y, z)]); };
		auto mix = [](float a, float c, float k) { return a + (c - a) * k; };
		return mix(mix(mix(t(i0x, i0y, i0z), t(i1x, i0y, i0z), fx),
		               mix(t(i0x, i1y, i0z), t(i1x, i1y, i0z), fx), fy),
		           mix(mix(t(i0x, i0y, i1z), t(i1x, i0y, i1z), fx),
		               mix(t(i0x, i1y, i1z), t(i1x, i1y, i1z), fx), fy), fz);
	}

	// Returns false when the point falls in an inactive brick (no atlas slot).
	bool sample(float x, float y, float z, float *out) const {
		const int bx = static_cast<int>(std::floor(x / ve::kBrickSize));
		const int by = static_cast<int>(std::floor(y / ve::kBrickSize));
		const int bz = static_cast<int>(std::floor(z / ve::kBrickSize));
		const int slot = w.brick_slot(bx, by, bz);
		if (slot < 0) return false;
		*out = brick_sdf(slot, (x - bx * ve::kBrickSize) / ve::kVoxelSize,
		                       (y - by * ve::kBrickSize) / ve::kVoxelSize,
		                       (z - bz * ve::kBrickSize) / ve::kVoxelSize);
		return true;
	}
};

// Surface height of the analytic terrain at (x, z), bisected on the generator itself.
float surface_y(const ve::Generator &gen, float x, float z) {
	float lo = 0.0f, hi = 16.0f;
	for (int i = 0; i < 40; i++) {
		const float mid = 0.5f * (lo + hi);
		(gen.sample(x, mid, z).sdf < 0.0f ? lo : hi) = mid;
	}
	return 0.5f * (lo + hi);
}

} // namespace

TEST_CASE("reconstructed gradient stays unit-length across every slab of a brick") {
	ve::WorldData w(64, 24, 64);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	const ShaderField field{w};

	// Bucket samples by which voxel slab of the brick they land in. The last slab (15) is
	// the one that needs the apron; before the fix its gradient collapses toward zero.
	double worst[ve::kBrickVoxels];
	int count[ve::kBrickVoxels] = {};
	for (int i = 0; i < ve::kBrickVoxels; i++) worst[i] = 1e30;

	for (int i = 0; i < 24; i++)
		for (int j = 0; j < 24; j++) {
			const float x = 4.0f + i * 0.37f, z = 4.0f + j * 0.41f;
			const int by = static_cast<int>(std::floor(surface_y(gen, x, z) / ve::kBrickSize));
			for (int slab = 0; slab < ve::kBrickVoxels; slab++) {
				const float y = by * ve::kBrickSize + (slab + 0.5f) * ve::kVoxelSize;
				if (std::fabs(gen.sample(x, y, z).sdf) > 0.25f) continue; // stay in the band
				const float e = 0.01f;
				float gx0, gx1, gy0, gy1, gz0, gz1;
				if (!field.sample(x - e, y, z, &gx0) || !field.sample(x + e, y, z, &gx1)) continue;
				if (!field.sample(x, y - e, z, &gy0) || !field.sample(x, y + e, z, &gy1)) continue;
				if (!field.sample(x, y, z - e, &gz0) || !field.sample(x, y, z + e, &gz1)) continue;
				const float dx = (gx1 - gx0) / (2 * e);
				const float dy = (gy1 - gy0) / (2 * e);
				const float dz = (gz1 - gz0) / (2 * e);
				const double g = std::sqrt(dx * dx + dy * dy + dz * dz);
				worst[slab] = std::min(worst[slab], g);
				count[slab]++;
			}
		}

	for (int slab = 0; slab < ve::kBrickVoxels; slab++) {
		CAPTURE(slab);
		CAPTURE(count[slab]);
		CAPTURE(worst[slab]);
		REQUIRE(count[slab] > 0);
		// An exact SDF has |grad| == 1; quantisation and trilinear reconstruction keep it
		// near that. A collapsed (clamped) axis drops it well below 0.8.
		CHECK(worst[slab] > 0.8);
	}
}

TEST_CASE("field is continuous across a brick face") {
	ve::WorldData w(64, 24, 64);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	const ShaderField field{w};

	const float eps = 1e-4f;
	int checked = 0;
	for (int i = 0; i < 20; i++)
		for (int j = 0; j < 20; j++) {
			const float x = 4.0f + i * 0.43f, z = 4.0f + j * 0.47f;
			const int by = static_cast<int>(std::floor(surface_y(gen, x, z) / ve::kBrickSize));
			const float face = (by + 1) * ve::kBrickSize; // +Y face of the surface brick
			float below, above;
			if (!field.sample(x, face - eps, z, &below)) continue;
			if (!field.sample(x, face + eps, z, &above)) continue;
			CAPTURE(x);
			CAPTURE(z);
			CAPTURE(face);
			CAPTURE(below);
			CAPTURE(above);
			// Both sides must agree: the apron plane of the lower brick IS the lattice
			// plane of the upper one. Tolerance covers uint8 SDF quantisation (2*0.64/255).
			CHECK(std::fabs(above - below) < 0.01f);
			checked++;
		}
	REQUIRE(checked > 100);
}

TEST_CASE("brick apron holds the neighbour's origin sample") {
	ve::WorldData w(64, 24, 64);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	const float x = 8.0f, z = 8.0f;
	const int bx = static_cast<int>(x / ve::kBrickSize), bz = static_cast<int>(z / ve::kBrickSize);
	const int by = static_cast<int>(std::floor(surface_y(gen, x, z) / ve::kBrickSize));
	const int slot = w.brick_slot(bx, by, bz);
	REQUIRE(slot >= 0);
	const ve::Brick &b = w.brick(slot);
	for (int a = 0; a < ve::kBrickSdfStride; a++)
		for (int c = 0; c < ve::kBrickSdfStride; c++) {
			// Top apron plane (local y == 16) must equal the generator at that world height.
			const float wx = (bx * ve::kBrickVoxels + a) * ve::kVoxelSize;
			const float wy = (by * ve::kBrickVoxels + ve::kBrickVoxels) * ve::kVoxelSize;
			const float wz = (bz * ve::kBrickVoxels + c) * ve::kVoxelSize;
			const float want = ve::decode_sdf(ve::encode_sdf(gen.sample(wx, wy, wz).sdf));
			const float got = ve::decode_sdf(b.sdf[ve::sdf_index(a, ve::kBrickVoxels, c)]);
			REQUIRE(got == doctest::Approx(want));
		}
}
