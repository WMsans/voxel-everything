#include <doctest/doctest.h>
#include "mesh/dual_contour.h"
#include "world/brick.h"
#include <cmath>
#include <functional>
#include <vector>

// Small grids keep the native suite in milliseconds (spec §8). The 130^3 chunk grid is
// exercised by the GPU differential test, which has a GPU to do it on.
static ve::DcGrid small_grid(float ox = 0.0f, float oy = 0.0f, float oz = 0.0f) {
	ve::DcGrid g;
	g.lattice = 18;          // 17^3 cells, 16^3 owned edges
	g.cell_size = 0.1f;
	g.origin[0] = ox; g.origin[1] = oy; g.origin[2] = oz;
	return g;
}

// f is evaluated in WORLD space, exactly where the mesher believes the sample sits.
static std::vector<uint8_t> make_lattice(const ve::DcGrid &g,
		const std::function<float(float, float, float)> &f) {
	std::vector<uint8_t> out(static_cast<size_t>(g.lattice) * g.lattice * g.lattice);
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++)
				out[ve::dc_lattice_index(g, x, y, z)] = ve::encode_sdf(
						f(g.origin[0] + (x - 1) * g.cell_size,
						  g.origin[1] + (y - 1) * g.cell_size,
						  g.origin[2] + (z - 1) * g.cell_size));
	return out;
}

static void tri_normal(const ve::MeshBuffer &m, int t, float out[3]) {
	const uint32_t i0 = m.indices[t * 3 + 0], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
	const float *p = m.positions.data();
	const float a[3] = {p[i1 * 3 + 0] - p[i0 * 3 + 0], p[i1 * 3 + 1] - p[i0 * 3 + 1],
			p[i1 * 3 + 2] - p[i0 * 3 + 2]};
	const float b[3] = {p[i2 * 3 + 0] - p[i0 * 3 + 0], p[i2 * 3 + 1] - p[i0 * 3 + 1],
			p[i2 * 3 + 2] - p[i0 * 3 + 2]};
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

TEST_CASE("an all-air lattice and an all-solid lattice produce nothing") {
	const ve::DcGrid g = small_grid();
	ve::MeshBuffer m;
	ve::dual_contour(make_lattice(g, [](float, float, float) { return 1.0f; }).data(), g, &m);
	CHECK(m.vertex_count() == 0);
	CHECK(m.triangle_count() == 0);
	ve::dual_contour(make_lattice(g, [](float, float, float) { return -1.0f; }).data(), g, &m);
	CHECK(m.vertex_count() == 0);
	CHECK(m.triangle_count() == 0);
}

TEST_CASE("a flat plane gives one quad per owned column, wound towards the air") {
	const ve::DcGrid g = small_grid();
	// Solid below y = 0.85, air above. 0.85 is deliberately off-lattice (samples sit at
	// multiples of 0.1 from -0.1), so every crossing is a genuine interpolation.
	const auto lat = make_lattice(g, [](float, float y, float) { return y - 0.85f; });
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);

	CHECK(m.triangle_count() == 2 * g.owned() * g.owned());
	// The crossing sits half way between two lattice samples, so the only error is the
	// uint8 quantisation of those samples: ~2.5 mm, well inside a 0.1 m cell.
	for (int v = 0; v < m.vertex_count(); v++)
		CHECK(m.positions[v * 3 + 1] == doctest::Approx(0.85f).epsilon(0.02f));
	for (int t = 0; t < m.triangle_count(); t++) {
		float n[3];
		tri_normal(m, t, n);
		CHECK(n[1] > 0.0f); // counter-clockwise seen from the air side above
	}
}

TEST_CASE("a sphere meshes onto its own surface with outward winding") {
	const ve::DcGrid g = small_grid();
	const float c[3] = {0.75f, 0.75f, 0.75f};
	const float r = 0.5f;
	const auto lat = make_lattice(g, [&](float x, float y, float z) {
		return std::sqrt((x - c[0]) * (x - c[0]) + (y - c[1]) * (y - c[1]) +
				(z - c[2]) * (z - c[2])) - r;
	});
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);
	CHECK(m.vertex_count() > 100);

	for (int v = 0; v < m.vertex_count(); v++) {
		const float d = std::sqrt(
				(m.positions[v * 3 + 0] - c[0]) * (m.positions[v * 3 + 0] - c[0]) +
				(m.positions[v * 3 + 1] - c[1]) * (m.positions[v * 3 + 1] - c[1]) +
				(m.positions[v * 3 + 2] - c[2]) * (m.positions[v * 3 + 2] - c[2]));
		CHECK(std::fabs(d - r) < g.cell_size); // every vertex within one cell of the surface
	}
	for (int t = 0; t < m.triangle_count(); t++) {
		float n[3];
		tri_normal(m, t, n);
		const uint32_t i0 = m.indices[t * 3];
		const float out[3] = {m.positions[i0 * 3 + 0] - c[0], m.positions[i0 * 3 + 1] - c[1],
				m.positions[i0 * 3 + 2] - c[2]};
		CHECK(n[0] * out[0] + n[1] * out[1] + n[2] * out[2] > 0.0f);
	}
}

TEST_CASE("indices stay inside the vertex array and cell_vertex agrees with them") {
	const ve::DcGrid g = small_grid();
	const auto lat = make_lattice(g, [](float x, float y, float z) {
		return y - 0.85f - 0.2f * x - 0.1f * z; // a tilted plane: crossings on all three axes
	});
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);
	CHECK(m.triangle_count() > 0);
	for (uint32_t i : m.indices) CHECK(static_cast<int>(i) < m.vertex_count());
	CHECK(static_cast<int>(m.cell_vertex.size()) ==
			g.cells() * g.cells() * g.cells());
	int mapped = 0;
	for (int32_t v : m.cell_vertex)
		if (v >= 0) { CHECK(v < m.vertex_count()); mapped++; }
	CHECK(mapped == m.vertex_count()); // exactly one vertex per crossed cell
}

TEST_CASE("the world origin lands in the positions") {
	const ve::DcGrid g = small_grid(100.0f, 50.0f, -20.0f);
	const auto lat = make_lattice(g, [](float, float y, float) { return y - 50.85f; });
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);
	CHECK(m.vertex_count() > 0);
	CHECK(m.positions[0] >= 100.0f - g.cell_size);
	CHECK(m.positions[1] == doctest::Approx(50.85f).epsilon(0.001f));
	CHECK(m.positions[2] >= -20.0f - g.cell_size);
}

TEST_CASE("chunk_dc_grid describes the shipping chunk") {
	const ve::DcGrid g = ve::chunk_dc_grid({2, -1, 3});
	CHECK(g.lattice == ve::kChunkLattice);
	CHECK(g.cells() == ve::kChunkMeshCells);
	CHECK(g.owned() == ve::kChunkCells);
	CHECK(g.cell_size == doctest::Approx(ve::kChunkCellSize));
	CHECK(g.origin[0] == doctest::Approx(2 * ve::kChunkSize));
	CHECK(g.origin[1] == doctest::Approx(-1 * ve::kChunkSize));
	CHECK(g.origin[2] == doctest::Approx(3 * ve::kChunkSize));
}
