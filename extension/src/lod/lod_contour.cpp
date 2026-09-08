#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_reduce.h"
#include "world/brick.h"
#include "shade/oct.h"
#include <cmath>

namespace ve {

namespace {

// Cell corners indexed by (x | y<<1 | z<<2), and the cell's 12 edges as corner pairs, in the
// SAME order as kCorner/kEdge in dual_contour.cpp. The order matters: the vertex is a running
// sum over crossings and float addition is not associative, so a different order gives a
// different vertex and the GPU diff fails.
constexpr int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
		{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
constexpr int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
		{0, 2}, {1, 3}, {4, 6}, {5, 7},
		{0, 4}, {1, 5}, {2, 6}, {3, 7}};

int mesh_cell_index(int x, int y, int z) {
	return x + y * kLodChunkMeshCells + z * kLodChunkMeshCells * kLodChunkMeshCells;
}

} // namespace

void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out) {
	out->quads.clear();
	out->normals.clear();
	out->overflow = false;

	// Pass 1: the dual vertex of every crossed cell, as a fraction of its own cell. Storing
	// the fraction rather than a world position is what lets the quad record carry it in
	// five bits per axis.
	const size_t cell_count = size_t(kLodChunkMeshCells) * kLodChunkMeshCells * kLodChunkMeshCells;
	std::vector<uint8_t> frac(cell_count * 3, 0);
	std::vector<uint16_t> normal(cell_count, 0);
	std::vector<char> has_vertex(cell_count, 0);
	for (int mz = 0; mz < kLodChunkMeshCells; mz++)
		for (int my = 0; my < kLodChunkMeshCells; my++)
			for (int mx = 0; mx < kLodChunkMeshCells; mx++) {
				float d[8];
				for (int k = 0; k < 8; k++)
					d[k] = decode_sdf(lattice[lod_lattice_index(mx + kCorner[k][0],
							my + kCorner[k][1], mz + kCorner[k][2])]);
				float acc[3] = {0.0f, 0.0f, 0.0f};
				int n = 0;
				for (int e = 0; e < 12; e++) {
					const float da = d[kEdge[e][0]], db = d[kEdge[e][1]];
					if ((da <= 0.0f) == (db <= 0.0f)) continue;
					const float t = da / (da - db);
					for (int a = 0; a < 3; a++)
						acc[a] += float(kCorner[kEdge[e][0]][a]) +
								t * float(kCorner[kEdge[e][1]][a] - kCorner[kEdge[e][0]][a]);
					n++;
				}
				if (n == 0) continue;
				const size_t ci = size_t(mesh_cell_index(mx, my, mz));
				has_vertex[ci] = 1;
				float f[3];
				for (int a = 0; a < 3; a++) {
					f[a] = acc[a] / float(n);
					frac[ci * 3 + a] = lod_quantise_offset(f[a]);
				}
				// Evaluate the trilinear SDF gradient at the dual vertex. This is a
				// filtered lattice normal, not a derivative of the heavily quantised quad.
				const float gx0 = d[1] - d[0] + f[1] * (d[3] - d[2] - d[1] + d[0]);
				const float gx1 = d[5] - d[4] + f[1] * (d[7] - d[6] - d[5] + d[4]);
				const float gy0 = d[2] - d[0] + f[0] * (d[3] - d[1] - d[2] + d[0]);
				const float gy1 = d[6] - d[4] + f[0] * (d[7] - d[5] - d[6] + d[4]);
				const float gz0 = d[4] - d[0] + f[0] * (d[5] - d[1] - d[4] + d[0]);
				const float gz1 = d[6] - d[2] + f[0] * (d[7] - d[3] - d[6] + d[2]);
				float gradient[3] = {gx0 + f[2] * (gx1 - gx0),
						gy0 + f[2] * (gy1 - gy0), gz0 + f[1] * (gz1 - gz0)};
				const float len = std::sqrt(gradient[0] * gradient[0] + gradient[1] * gradient[1] +
						gradient[2] * gradient[2]);
				if (len > 1e-10f) {
					for (float &v : gradient) v /= len;
					normal[ci] = oct_encode_snorm8(gradient);
				}
			}

	// Pass 2: one quad per sign-changing lattice edge this chunk owns -- local edge
	// coordinate u in [0, kLodChunkCells), lattice index u + 1 -- so every edge in the world
	// is emitted by exactly one chunk: no cracks, no duplicates.
	for (int uz = 0; uz < kLodChunkCells; uz++)
		for (int uy = 0; uy < kLodChunkCells; uy++)
			for (int ux = 0; ux < kLodChunkCells; ux++) {
				const int L[3] = {ux + 1, uy + 1, uz + 1};
				const float da = decode_sdf(lattice[lod_lattice_index(L[0], L[1], L[2])]);
				for (int axis = 0; axis < 3; axis++) {
					int Lb[3] = {L[0], L[1], L[2]};
					Lb[axis] += 1;
					const float db = decode_sdf(lattice[lod_lattice_index(Lb[0], Lb[1], Lb[2])]);
					const bool sa = da <= 0.0f, sb = db <= 0.0f;
					if (sa == sb) continue;
					if (int(out->quads.size()) >= kLodMaxQuadsPerChunk) {
						out->overflow = true;
						return;
					}
					const int b = (axis + 1) % 3, c = (axis + 2) % 3;
					size_t ci[4];
					bool ok = true;
					for (int k = 0; k < 4; k++) {
						int m[3] = {L[0], L[1], L[2]};
						m[b] += kLodQuadCorners[k][0];
						m[c] += kLodQuadCorners[k][1];
						ci[k] = size_t(mesh_cell_index(m[0], m[1], m[2]));
						if (!has_vertex[ci[k]]) ok = false;
					}
					// Unreachable on the CPU (a crossed edge crosses all four of its cells),
					// but the GPU can lose a vertex to a cap, and both sides run the same
					// rule so the diff stays honest.
					if (!ok) continue;

					LodQuadFields f{};
					f.u[0] = uint8_t(ux); f.u[1] = uint8_t(uy); f.u[2] = uint8_t(uz);
					f.axis = uint8_t(axis);
					f.sign = sa ? 1 : 0;
					// The material of the SOLID endpoint of the edge: deterministic, and
					// mirrorable in one line of GLSL.
					f.material = material[lod_lattice_index(sa ? L[0] : Lb[0], sa ? L[1] : Lb[1],
							sa ? L[2] : Lb[2])];
					// Store the corners ALREADY WOUND. (axis, b, c) is a right-handed cycle,
					// so c0..c3 wind counter-clockwise seen from +axis; solid -> air along
					// +axis puts the air on the +axis side, which is the side the normal must
					// face. The reversed case stores (c0, c3, c2, c1), whose two triangles
					// are exactly ve::dual_contour's tri_rev pair.
					const int order_fwd[4] = {0, 1, 2, 3};
					const int order_rev[4] = {0, 3, 2, 1};
					const int *order = sa ? order_fwd : order_rev;
					for (int k = 0; k < 4; k++)
						for (int a = 0; a < 3; a++)
							f.offset[k][a] = frac[ci[order[k]] * 3 + a];
					LodQuad q{};
					lod_quad_pack(f, &q);
					out->quads.push_back(q);
					LodQuadNormals qn{};
					for (int k = 0; k < 4; ++k) qn.corner[k] = normal[ci[order[k]]];
					out->normals.push_back(qn);
				}
			}
}

} // namespace ve
