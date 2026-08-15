#include "mesh/dual_contour.h"
#include "world/brick.h"

namespace ve {

namespace {

// Cell corners, indexed by (x | y<<1 | z<<2). Mirrored as CORNER[8] in mesh_cells.comp.glsl.
constexpr int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
		{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};

// The cell's 12 edges as corner pairs: four along x, four along y, four along z. The ORDER
// matters — the vertex is a running sum over crossings, and float addition is not
// associative. Mirrored as EDGE[12] in mesh_cells.comp.glsl.
constexpr int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
		{0, 2}, {1, 3}, {4, 6}, {5, 7},
		{0, 4}, {1, 5}, {2, 6}, {3, 7}};

// The four cells around a lattice edge, as offsets in the two axes perpendicular to it,
// wound counter-clockwise seen from +axis. Mirrored as QUAD[4] in mesh_quads.comp.glsl.
constexpr int kQuad[4][2] = {{-1, -1}, {0, -1}, {0, 0}, {-1, 0}};

} // namespace

DcGrid chunk_dc_grid(IVec3 chunk) {
	DcGrid g;
	g.lattice = kChunkLattice;
	g.cell_size = kChunkCellSize;
	chunk_world_origin(chunk, g.origin);
	return g;
}

int dc_lattice_index(const DcGrid &g, int x, int y, int z) {
	return x + y * g.lattice + z * g.lattice * g.lattice;
}

int dc_cell_index(const DcGrid &g, int x, int y, int z) {
	return x + y * g.cells() + z * g.cells() * g.cells();
}

void dual_contour(const uint8_t *lattice, const DcGrid &g, MeshBuffer *out) {
	const int cells = g.cells();
	out->positions.clear();
	out->indices.clear();
	out->cell_vertex.assign(static_cast<size_t>(cells) * cells * cells, -1);

	// Pass 1: one dual vertex per crossed cell.
	for (int mz = 0; mz < cells; mz++)
		for (int my = 0; my < cells; my++)
			for (int mx = 0; mx < cells; mx++) {
				float d[8];
				for (int k = 0; k < 8; k++)
					d[k] = decode_sdf(lattice[dc_lattice_index(g, mx + kCorner[k][0],
							my + kCorner[k][1], mz + kCorner[k][2])]);
				float acc[3] = {0.0f, 0.0f, 0.0f};
				int n = 0;
				for (int e = 0; e < 12; e++) {
					const float da = d[kEdge[e][0]], db = d[kEdge[e][1]];
					if ((da <= 0.0f) == (db <= 0.0f)) continue;
					// da != db whenever the signs differ, so this never divides by zero.
					const float t = da / (da - db);
					for (int a = 0; a < 3; a++)
						acc[a] += static_cast<float>(kCorner[kEdge[e][0]][a]) +
								t * static_cast<float>(kCorner[kEdge[e][1]][a] -
										kCorner[kEdge[e][0]][a]);
					n++;
				}
				if (n == 0) continue;
				out->cell_vertex[dc_cell_index(g, mx, my, mz)] =
						static_cast<int32_t>(out->positions.size() / 3);
				const int m[3] = {mx, my, mz};
				for (int a = 0; a < 3; a++)
					out->positions.push_back(g.origin[a] +
							(static_cast<float>(m[a] - 1) + acc[a] / static_cast<float>(n)) *
									g.cell_size);
			}

	// Pass 2: one quad per sign-changing lattice edge. The grid owns the edges whose four
	// cells it holds — local edge coordinate u in [0, owned()), lattice index u + 1 — so
	// every edge in the world is emitted by exactly one chunk: no cracks, no duplicates.
	const int owned = g.owned();
	for (int uz = 0; uz < owned; uz++)
		for (int uy = 0; uy < owned; uy++)
			for (int ux = 0; ux < owned; ux++) {
				const int L[3] = {ux + 1, uy + 1, uz + 1};
				const float da = decode_sdf(lattice[dc_lattice_index(g, L[0], L[1], L[2])]);
				for (int axis = 0; axis < 3; axis++) {
					int Lb[3] = {L[0], L[1], L[2]};
					Lb[axis] += 1;
					const float db = decode_sdf(lattice[dc_lattice_index(g, Lb[0], Lb[1], Lb[2])]);
					const bool sa = da <= 0.0f, sb = db <= 0.0f;
					if (sa == sb) continue;
					const int b = (axis + 1) % 3, c = (axis + 2) % 3;
					int32_t q[4];
					bool ok = true;
					for (int k = 0; k < 4; k++) {
						int m[3] = {L[0], L[1], L[2]};
						m[b] += kQuad[k][0];
						m[c] += kQuad[k][1];
						q[k] = out->cell_vertex[dc_cell_index(g, m[0], m[1], m[2])];
						if (q[k] < 0) ok = false;
					}
					// Unreachable on the CPU (a crossed edge crosses all four of its cells),
					// but the GPU can lose a vertex to the per-chunk cap, and both sides run
					// the same rule so the diff stays honest.
					if (!ok) continue;
					// (axis, b, c) is a right-handed cycle, so q0..q3 wind counter-clockwise
					// seen from +axis. Solid -> air along +axis puts the air on the +axis
					// side, which is the side the normal must face.
					const int32_t tri_fwd[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
					const int32_t tri_rev[6] = {q[0], q[2], q[1], q[0], q[3], q[2]};
					const int32_t *tri = sa ? tri_fwd : tri_rev;
					for (int k = 0; k < 6; k++) out->indices.push_back(static_cast<uint32_t>(tri[k]));
				}
			}
}

} // namespace ve
