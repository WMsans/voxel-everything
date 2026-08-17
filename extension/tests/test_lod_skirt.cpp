#include <doctest/doctest.h>
#include "lod/lod_skirt.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_quad.h"
#include "lod/lod_reduce.h"
#include "world/brick.h"
#include <vector>

namespace {
std::vector<uint8_t> plane_lattice(float height_cells) {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n);
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
				l[ve::lod_lattice_index(x, y, z)] =
						ve::encode_sdf((float(y - 1) - height_cells) * 0.4f);
	return l;
}
} // namespace

TEST_CASE("the skirt depth is two cells") { CHECK(ve::kLodSkirtCells == 2); }

// Only quads on the chunk's boundary get a curtain. A plane's boundary quads are the ring
// around its edge: 4 * 32 - 4 of them out of 32 * 32.
TEST_CASE("only boundary quads produce skirts") {
	const std::vector<uint8_t> l = plane_lattice(10.0f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	const size_t surface = r.quads.size();
	const int added = ve::lod_append_skirts(&r.quads);
	// One curtain quad per boundary quad, emitted twice for two-sidedness.
	const int ring = 4 * ve::kLodChunkCells - 4;
	CHECK(added == ring * 2);
	CHECK(r.quads.size() == surface + size_t(added));
}

// Every appended quad carries the double-sided bit, because a crack is looked into from a
// direction nobody can predict.
TEST_CASE("skirt quads are marked double sided and come in opposite-wound pairs") {
	const std::vector<uint8_t> l = plane_lattice(10.0f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	const size_t surface = r.quads.size();
	ve::lod_append_skirts(&r.quads);
	REQUIRE(r.quads.size() > surface);
	for (size_t i = surface; i < r.quads.size(); i += 2) {
		ve::LodQuadFields a{}, b{};
		ve::lod_quad_unpack(r.quads[i], &a);
		ve::lod_quad_unpack(r.quads[i + 1], &b);
		CHECK(a.double_sided == 1);
		CHECK(b.double_sided == 1);
		// The pair is the same geometry wound the other way: corners 1 and 3 swap.
		for (int x = 0; x < 3; x++) {
			CHECK(b.offset[0][x] == a.offset[0][x]);
			CHECK(b.offset[1][x] == a.offset[3][x]);
			CHECK(b.offset[2][x] == a.offset[2][x]);
			CHECK(b.offset[3][x] == a.offset[1][x]);
		}
	}
}

TEST_CASE("skirts respect the per-chunk cap") {
	std::vector<ve::LodQuad> quads(size_t(ve::kLodMaxQuadsPerChunk) - 3);
	// All at u = 0, so every one of them counts as a boundary quad.
	for (ve::LodQuad &q : quads) {
		ve::LodQuadFields f{};
		f.axis = 1;
		f.u[0] = 0; f.u[1] = 5; f.u[2] = 5;
		ve::lod_quad_pack(f, &q);
	}
	const int added = ve::lod_append_skirts(&quads);
	CHECK(added == 3);
	CHECK(int(quads.size()) == ve::kLodMaxQuadsPerChunk);
}

TEST_CASE("an empty chunk produces no skirts") {
	std::vector<ve::LodQuad> quads;
	CHECK(ve::lod_append_skirts(&quads) == 0);
	CHECK(quads.empty());
}
