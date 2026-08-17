#include <doctest/doctest.h>
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_quad.h"
#include "lod/lod_reduce.h"
#include "mesh/dual_contour.h"
#include "world/brick.h"
#include <cmath>
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

TEST_CASE("an all-air lattice produces no quads") {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n, ve::encode_sdf(0.5f));
	std::vector<uint16_t> m(l.size(), 0);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	CHECK(r.quads.empty());
	CHECK(r.overflow == false);
}

// A horizontal plane crosses exactly one lattice edge per column, all along +y, so the quad
// count is the number of owned columns and every quad's axis is 1.
TEST_CASE("a horizontal plane produces one quad per owned column") {
	const std::vector<uint8_t> l = plane_lattice(10.0f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	CHECK(r.quads.size() == size_t(ve::kLodChunkCells) * ve::kLodChunkCells);
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		CHECK(f.axis == 1);
		CHECK(f.material == 2);
	}
}

// The reference must agree with the mesher the collision path already trusts. Run both over
// the SAME lattice and compare the sets of quads, as cell-index quadruples so vertex
// numbering (which ve::dual_contour allocates and lod_contour does not) never enters.
TEST_CASE("lod_contour emits the same surface ve::dual_contour does") {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n);
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				// A lumpy surface, so quads appear on all three axes.
				const float h = 12.0f + 3.0f * std::sin(float(x) * 0.4f) *
						std::cos(float(z) * 0.31f);
				l[ve::lod_lattice_index(x, y, z)] = ve::encode_sdf((float(y - 1) - h) * 0.4f);
			}
	std::vector<uint16_t> m(l.size(), 3);

	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);

	ve::DcGrid g;
	g.lattice = ve::kLodChunkLattice;
	g.cell_size = ve::lod_cell_size(0);
	g.origin[0] = g.origin[1] = g.origin[2] = 0.0f;
	ve::MeshBuffer mb;
	ve::dual_contour(l.data(), g, &mb);

	// ve::dual_contour emits two triangles per quad, so the quad count is half its triangles.
	CHECK(r.quads.size() * 2u == size_t(mb.triangle_count()));

	// Positions must agree to the quantiser: decode every lod_contour corner and find the
	// dual-contour vertex of the same cell.
	const float cell = ve::lod_cell_size(0);
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	const float tol = cell / float(ve::kLodOffsetMax);
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		for (int k = 0; k < 4; k++) {
			int mc[3];
			ve::lod_quad_corner_cell(f, k, mc);
			const int vi = mb.cell_vertex[ve::dc_cell_index(g, mc[0], mc[1], mc[2])];
			REQUIRE(vi >= 0);
			float p[3];
			ve::lod_quad_corner_pos(f, k, origin, cell, p);
			for (int a = 0; a < 3; a++)
				CHECK(std::fabs(p[a] - mb.positions[size_t(vi) * 3 + a]) <= tol);
		}
	}
}

// Corners are stored ALREADY WOUND, so the vertex shader never branches: the first triangle
// of every quad must face away from the solid side.
TEST_CASE("stored corner order already winds toward the air") {
	const std::vector<uint8_t> l = plane_lattice(10.0f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	const float cell = ve::lod_cell_size(0);
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	REQUIRE(!r.quads.empty());
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		float p[3][3];
		for (int k = 0; k < 3; k++) ve::lod_quad_corner_pos(f, k, origin, cell, p[k]);
		const float a[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
		const float b[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
		const float nx = a[1] * b[2] - a[2] * b[1];
		const float ny = a[2] * b[0] - a[0] * b[2];
		const float nz = a[0] * b[1] - a[1] * b[0];
		(void)nx; (void)nz;
		// The plane's solid half is below, so every normal must point up.
		CHECK(ny > 0.0f);
	}
}

TEST_CASE("the cap is reported rather than exceeded") {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n);
	// Alternating slabs: a crossing on nearly every y edge, far past the cap.
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
				l[ve::lod_lattice_index(x, y, z)] = ve::encode_sdf((y & 1) ? 0.2f : -0.2f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	CHECK(r.quads.size() <= size_t(ve::kLodMaxQuadsPerChunk));
	CHECK(r.overflow == true);
}
