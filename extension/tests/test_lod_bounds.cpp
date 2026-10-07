#include <doctest/doctest.h>
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_quad.h"
#include "lod/lod_reduce.h"
#include "lod/lod_skirt.h"
#include "world/brick.h"
#include <vector>

namespace {

// A chunk's real mesh: a sloped plane contoured at `cell`, with its boundary ribbons.
std::vector<ve::LodQuad> sloped_chunk(const float origin[3], float cell, int axis, int sign,
		float slope) {
	const int n = ve::kLodChunkLattice;
	const int b = (axis + 1) % 3, c = (axis + 2) % 3;
	std::vector<uint8_t> lattice(size_t(n) * n * n);
	std::vector<uint16_t> material(lattice.size(), 7);
	const float mid = 16.25f * cell;
	for (int z = 0; z < n; ++z) for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
		const float p[3] = {(x - 1) * cell, (y - 1) * cell, (z - 1) * cell};
		const float d = p[axis] - mid - slope * (p[b] - mid) - slope * 0.5f * (p[c] - mid);
		lattice[ve::lod_lattice_index(x, y, z)] =
				ve::encode_sdf(d * (sign ? 1.0f : -1.0f) * 0.2f / cell);
	}
	(void)origin;
	ve::LodContourResult r;
	ve::lod_contour(lattice.data(), material.data(), &r);
	ve::lod_append_skirts(&r.quads, &r.normals);
	return r.quads;
}

} // namespace

TEST_CASE("page bounds contain every decoded corner, ribbons included") {
	const float origins[2][3] = {{0.0f, 0.0f, 0.0f}, {-409.6f, 25.6f, 819.2f}};
	for (const auto &origin : origins)
		for (float cell : {0.4f, 3.2f})
			for (int axis = 0; axis < 3; ++axis)
				for (int sign = 0; sign < 2; ++sign)
					for (float slope : {0.0f, 0.6f}) {
						const std::vector<ve::LodQuad> quads = sloped_chunk(origin, cell, axis, sign, slope);
						REQUIRE(!quads.empty());
						int ribbons = 0;
						float lo[3], hi[3];
						ve::lod_quads_bounds(quads.data(), static_cast<int>(quads.size()), origin, cell, lo, hi);
						for (const ve::LodQuad &q : quads) {
							ve::LodQuadFields f{};
							ve::lod_quad_unpack(q, &f);
							ribbons += f.double_sided;
							for (int k = 0; k < 4; ++k) {
								float p[3];
								ve::lod_quad_corner_pos(f, k, origin, cell, p);
								for (int a = 0; a < 3; ++a) {
									CHECK(p[a] >= lo[a]);
									CHECK(p[a] <= hi[a]);
								}
							}
						}
						CHECK(ribbons > 0); // the fixture must exercise the skirt displacement
					}
}

TEST_CASE("page bounds of flat ground are a slab, not the chunk cube") {
	// The far-field cull used to test a page against its whole 32-cell chunk; a level-4 chunk
	// over flat ground is a 205 m tall box that pokes above any ridge in front of it.
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	const float cell = 6.4f;
	const std::vector<ve::LodQuad> quads = sloped_chunk(origin, cell, 1, 1, 0.0f);
	float lo[3], hi[3];
	ve::lod_quads_bounds(quads.data(), static_cast<int>(quads.size()), origin, cell, lo, hi);
	// The surface sits at 16.25 cells; ribbons may hang kLodSkirtMaxExtensionCells below it.
	CHECK(hi[1] - lo[1] <= cell * float(2 + 2 * ve::kLodSkirtMaxExtensionCells));
	CHECK(hi[1] - lo[1] < 0.5f * cell * float(ve::kLodChunkCells));
}

TEST_CASE("page bounds of nothing are empty") {
	const float origin[3] = {1.0f, 2.0f, 3.0f};
	float lo[3], hi[3];
	ve::lod_quads_bounds(nullptr, 0, origin, 1.0f, lo, hi);
	for (int a = 0; a < 3; ++a) CHECK(lo[a] > hi[a]);
}
