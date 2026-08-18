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
	const int order_rev[4] = {0, 3, 2, 1};
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	const float cell = ve::lod_cell_size(0);
	for (size_t i = surface; i < r.quads.size(); i += 2) {
		ve::LodQuadFields a{}, b{};
		ve::lod_quad_unpack(r.quads[i], &a);
		ve::lod_quad_unpack(r.quads[i + 1], &b);
		CHECK(a.double_sided == 1);
		CHECK(b.double_sided == 1);
		CHECK(b.sign == (a.sign ^ 1));
		// The pair is the same geometry wound the other way: corners 1 and 3 swap.
		for (int x = 0; x < 3; x++) {
			CHECK(b.offset[0][x] == a.offset[0][x]);
			CHECK(b.offset[1][x] == a.offset[3][x]);
			CHECK(b.offset[2][x] == a.offset[2][x]);
			CHECK(b.offset[3][x] == a.offset[1][x]);
		}
		// Decode both quads: under the sign-aware decoder the reversed copy lands on the
		// same four world positions in the opposite order.
		for (int k = 0; k < 4; k++) {
			float pa[3], pb[3];
			ve::lod_quad_corner_pos(a, order_rev[k], origin, cell, pa);
			ve::lod_quad_corner_pos(b, k, origin, cell, pb);
			for (int x = 0; x < 3; x++) CHECK(pb[x] == doctest::Approx(pa[x]));
		}
	}
}

TEST_CASE("skirt quads are two cells deep along the negative normal") {
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	const float cell = ve::lod_cell_size(0);
	for (int sign = 0; sign <= 1; sign++) {
		for (int axis = 0; axis < 3; axis++) {
			ve::LodQuadFields f{};
			f.u[0] = 10; f.u[1] = 10; f.u[2] = 10;
			f.u[(axis + 1) % 3] = 0; // boundary on a perpendicular axis, normal axis interior
			f.axis = static_cast<uint8_t>(axis);
			f.sign = static_cast<uint8_t>(sign);
			f.material = 7;
			for (int k = 0; k < 4; k++)
				for (int a = 0; a < 3; a++)
					f.offset[k][a] = static_cast<uint8_t>((k * 3 + a) * 3 % (ve::kLodOffsetMax + 1));
			ve::LodQuad q{};
			ve::lod_quad_pack(f, &q);
			std::vector<ve::LodQuad> quads{q};
			ve::lod_append_skirts(&quads);
			REQUIRE(quads.size() == 3);
			ve::LodQuadFields s{};
			ve::lod_quad_unpack(quads[1], &s);
			CHECK(s.double_sided == 1);
			CHECK(s.material == f.material);
			const int delta = sign ? -ve::kLodSkirtCells : ve::kLodSkirtCells;
			for (int k = 0; k < 4; k++) {
				float parent[3], skirt[3];
				ve::lod_quad_corner_pos(f, k, origin, cell, parent);
				ve::lod_quad_corner_pos(s, k, origin, cell, skirt);
				CHECK(skirt[axis] == doctest::Approx(parent[axis] + delta * cell));
				for (int a = 0; a < 3; a++)
					if (a != axis) CHECK(skirt[a] == doctest::Approx(parent[a]));
			}
		}
	}
}

// At the two extreme chunk faces the full two-cell shift would leave the 5-bit u field.
// The skirt is clamped to the chunk edge (a partial-depth curtain) instead of wrapping to
// the opposite side of the chunk, and the opposite-wound pair is still emitted.
TEST_CASE("extreme boundary skirt u clamps instead of wrapping") {
	const int cases[2][2] = {
			{1, 0}, // sign==1 at u==0: -2 would leave the range below 0
			{0, ve::kLodChunkCells - 1}, // sign==0 at u==31: +2 would leave the range above 31
	};
	for (int axis = 0; axis < 3; axis++) {
		for (int c = 0; c < 2; c++) {
			const int sign = cases[c][0];
			const int u = cases[c][1];
			const int expected = sign ? 0 : ve::kLodChunkCells - 1;

			ve::LodQuadFields f{};
			f.u[0] = 10; f.u[1] = 10; f.u[2] = 10;
			f.u[axis] = static_cast<uint8_t>(u);
			f.u[(axis + 1) % 3] = 0; // boundary on a perpendicular axis
			f.axis = static_cast<uint8_t>(axis);
			f.sign = static_cast<uint8_t>(sign);
			f.material = 7;
			for (int k = 0; k < 4; k++)
				for (int a = 0; a < 3; a++)
					f.offset[k][a] = static_cast<uint8_t>((k * 3 + a) * 3 % (ve::kLodOffsetMax + 1));
			ve::LodQuad q{};
			ve::lod_quad_pack(f, &q);
			std::vector<ve::LodQuad> quads{q};
			ve::lod_append_skirts(&quads);

			REQUIRE(quads.size() == 3);
			ve::LodQuadFields a{}, b{};
			ve::lod_quad_unpack(quads[1], &a);
			ve::lod_quad_unpack(quads[2], &b);
			CHECK(a.double_sided == 1);
			CHECK(b.double_sided == 1);
			CHECK(b.sign == (a.sign ^ 1));
			CHECK(a.u[axis] == expected);
			// The opposite-wound copy must keep the same clamped edge coordinate.
			CHECK(b.u[axis] == expected);
			for (int x = 0; x < 3; x++) {
				CHECK(b.offset[0][x] == a.offset[0][x]);
				CHECK(b.offset[1][x] == a.offset[3][x]);
				CHECK(b.offset[2][x] == a.offset[2][x]);
				CHECK(b.offset[3][x] == a.offset[1][x]);
			}
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
