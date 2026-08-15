#include <doctest/doctest.h>
#include "world/brick_mip.h"
#include "world/brick.h"
#include <algorithm>
#include <vector>

// Fills a 17^3 lattice from a callable over lattice coordinates.
template <typename F>
static std::vector<uint8_t> lattice(F f) {
	std::vector<uint8_t> l(ve::kBrickSdfCount, 0);
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++)
				l[ve::sdf_index(x, y, z)] = ve::encode_sdf(f(x, y, z));
	return l;
}

TEST_CASE("kEncodedZero is exactly what encode_sdf(0) produces") {
	CHECK(ve::encode_sdf(0.0f) == ve::kEncodedZero);
}

TEST_CASE("an all-air brick reports no surface at any level") {
	const auto l = lattice([](int, int, int) { return 0.5f; });
	ve::BrickMips m{};
	ve::build_brick_mips(l.data(), &m);
	for (int level = 0; level < ve::kMipLevels; level++) {
		const int n = ve::kMipDims[level] * ve::kMipDims[level] * ve::kMipDims[level];
		for (int i = 0; i < n; i++)
			CHECK_FALSE(ve::mip_cell_has_surface(ve::mip_min(m, level)[i],
			                                     ve::mip_max(m, level)[i]));
	}
}

TEST_CASE("a plane through the brick marks exactly the cells it crosses") {
	// Surface at local y = 4 voxels: sdf = (y - 4) * kVoxelSize.
	const auto l = lattice([](int, int y, int) { return (y - 4) * ve::kVoxelSize; });
	ve::BrickMips m{};
	ve::build_brick_mips(l.data(), &m);

	// 8^3 level: each cell spans 2 voxels, inclusive of its far lattice plane. Cell row
	// j covers lattice y in [2j, 2j+2], so j = 1 (y 2..4) and j = 2 (y 4..6) both touch 0.
	for (int k = 0; k < 8; k++)
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++) {
				const int idx = i + j * 8 + k * 64;
				const bool expect = (2 * j <= 4 && 4 <= 2 * j + 2);
				CHECK(ve::mip_cell_has_surface(m.mn8[idx], m.mx8[idx]) == expect);
			}
	// 2^3 level: cell row j covers lattice y in [8j, 8j+8]; only j = 0 contains y = 4.
	for (int j = 0; j < 2; j++)
		CHECK(ve::mip_cell_has_surface(m.mn2[j * 2], m.mx2[j * 2]) == (j == 0));
}

TEST_CASE("coarse levels bound the fine levels they summarise") {
	// A wobbly field so every cell has a distinct range.
	const auto l = lattice([](int x, int y, int z) {
		return 0.01f * static_cast<float>((x * 7 + y * 13 + z * 3) % 41) - 0.2f;
	});
	ve::BrickMips m{};
	ve::build_brick_mips(l.data(), &m);
	// Each 4^3 cell must enclose its eight 8^3 children, and each 2^3 cell its 4^3 children.
	const auto check_parent = [](const uint8_t *pmn, const uint8_t *pmx, int pd,
			const uint8_t *cmn, const uint8_t *cmx, int cd) {
		for (int z = 0; z < pd; z++)
			for (int y = 0; y < pd; y++)
				for (int x = 0; x < pd; x++) {
					const int p = x + y * pd + z * pd * pd;
					uint8_t mn = 255, mx = 0;
					for (int dz = 0; dz < 2; dz++)
						for (int dy = 0; dy < 2; dy++)
							for (int dx = 0; dx < 2; dx++) {
								const int c = (2 * x + dx) + (2 * y + dy) * cd +
										(2 * z + dz) * cd * cd;
								mn = std::min(mn, cmn[c]);
								mx = std::max(mx, cmx[c]);
							}
					CHECK(static_cast<int>(pmn[p]) == static_cast<int>(mn));
					CHECK(static_cast<int>(pmx[p]) == static_cast<int>(mx));
				}
	};
	check_parent(m.mn4, m.mx4, 4, m.mn8, m.mx8, 8);
	check_parent(m.mn2, m.mx2, 2, m.mn4, m.mx4, 4);
}

TEST_CASE("the 8^3 level is the exact min/max of its inclusive 3^3 lattice block") {
	const auto l = lattice([](int x, int y, int z) {
		return 0.01f * static_cast<float>((x * 5 + y * 11 + z * 17) % 53) - 0.25f;
	});
	ve::BrickMips m{};
	ve::build_brick_mips(l.data(), &m);
	for (int k = 0; k < 8; k++)
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++) {
				uint8_t mn = 255, mx = 0;
				for (int dz = 0; dz <= 2; dz++)
					for (int dy = 0; dy <= 2; dy++)
						for (int dx = 0; dx <= 2; dx++) {
							const uint8_t v = l[ve::sdf_index(2 * i + dx, 2 * j + dy, 2 * k + dz)];
							mn = std::min(mn, v);
							mx = std::max(mx, v);
						}
				const int idx = i + j * 8 + k * 64;
				CHECK(static_cast<int>(m.mn8[idx]) == static_cast<int>(mn));
				CHECK(static_cast<int>(m.mx8[idx]) == static_cast<int>(mx));
			}
}
