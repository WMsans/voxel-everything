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
}

TEST_CASE("2-bit material index roundtrip") {
	ve::Brick b{};
	for (int i = 0; i < ve::kBrickVoxelCount; i++) ve::set_mat_index(b, i, i % 4);
	for (int i = 0; i < ve::kBrickVoxelCount; i++) CHECK(ve::get_mat_index(b, i) == i % 4);
	CHECK(sizeof(b.mat) == 1024);
}
