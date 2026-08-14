#include <doctest/doctest.h>
#include "world/brick.h"
#include <cmath>
#include <set>

TEST_CASE("sdf codec roundtrips within quantization step") {
	for (float d : {-0.64f, -0.1f, 0.0f, 0.05f, 0.33f, 0.64f}) {
		float rt = ve::decode_sdf(ve::encode_sdf(d));
		CHECK(std::abs(rt - d) <= (1.28f / 255.0f) + 1e-6f);
	}
}

TEST_CASE("sdf codec clamps out of range") {
	CHECK(ve::encode_sdf(1.0f) == 255);
	CHECK(ve::encode_sdf(-1.0f) == 0);
	CHECK(ve::decode_sdf(0) == doctest::Approx(-0.64f));
	CHECK(ve::decode_sdf(255) == doctest::Approx(0.64f));
}

TEST_CASE("voxel_index is a bijection over 4096 cells") {
	std::set<int> seen;
	for (int z = 0; z < 16; z++)
		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++) {
				int idx = ve::voxel_index(x, y, z);
				CHECK(idx >= 0);
				CHECK(idx < ve::kBrickVoxelCount);
				seen.insert(idx);
			}
	CHECK(seen.size() == ve::kBrickVoxelCount);
	// Pin the exact layout (x + y*16 + z*256) so a silent axis swap fails the suite.
	CHECK(ve::voxel_index(1, 0, 0) == 1);
	CHECK(ve::voxel_index(0, 1, 0) == 16);
}

TEST_CASE("sdf_index is a bijection over the 17^3 apron lattice") {
	std::set<int> seen;
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				int idx = ve::sdf_index(x, y, z);
				CHECK(idx >= 0);
				CHECK(idx < ve::kBrickSdfCount);
				seen.insert(idx);
			}
	CHECK(seen.size() == ve::kBrickSdfCount);
	// Pin the exact layout (x + y*17 + z*289): the GPU atlas upload mirrors this stride.
	CHECK(ve::kBrickSdfStride == 17);
	CHECK(ve::kBrickSdfCount == 4913);
	CHECK(ve::sdf_index(1, 0, 0) == 1);
	CHECK(ve::sdf_index(0, 1, 0) == 17);
	CHECK(ve::sdf_index(0, 0, 1) == 289);
}

TEST_CASE("2-bit material index roundtrip") {
	ve::Brick b{};
	for (int i = 0; i < ve::kBrickVoxelCount; i++) ve::set_mat_index(b, i, i % 4);
	for (int i = 0; i < ve::kBrickVoxelCount; i++) CHECK(ve::get_mat_index(b, i) == i % 4);
	CHECK(sizeof(b.mat) == 1024);
}
