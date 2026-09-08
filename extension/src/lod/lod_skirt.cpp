#include "lod/lod_skirt.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace ve {
namespace {

uint64_t edge_key(const LodQuadFields &f, int edge) {
	int m[2][3];
	lod_quad_corner_cell(f, edge, m[0]);
	lod_quad_corner_cell(f, (edge + 1) & 3, m[1]);
	uint32_t v[2];
	for (int i = 0; i < 2; i++)
		v[i] = uint32_t(m[i][0] + kLodChunkMeshCells *
				(m[i][1] + kLodChunkMeshCells * m[i][2]));
	return (uint64_t(std::min(v[0], v[1])) << 32) | std::max(v[0], v[1]);
}

int boundary_face(const LodQuadFields &f, int edge) {
	int a[3], b[3];
	lod_quad_corner_cell(f, edge, a);
	lod_quad_corner_cell(f, (edge + 1) & 3, b);
	// Prefer a perpendicular face when a plane lies in the last normal-axis cell.
	for (int i = 1; i <= 3; i++) {
		const int axis = (f.axis + i) % 3;
		if (a[axis] != b[axis]) continue;
		if (a[axis] == 0) return 2 * axis;
		if (a[axis] == kLodChunkCells) return 2 * axis + 1;
	}
	return -1;
}

} // namespace

int lod_append_skirts(std::vector<LodQuad> *quads, LodSkirtStatus *status) {
	return lod_append_skirts(quads, nullptr, status);
}

int lod_append_skirts(std::vector<LodQuad> *quads, std::vector<LodQuadNormals> *normals,
		LodSkirtStatus *status) {
	if (status) *status = {};
	if (!quads || quads->empty()) return 0;
	if (normals && normals->size() != quads->size()) return 0;
	const size_t surface = quads->size();
	std::unordered_map<uint64_t, int> incidence;
	std::unordered_set<uint64_t> covered;
	for (const LodQuad &q : *quads) {
		LodQuadFields f{};
		lod_quad_unpack(q, &f);
		if (f.double_sided) {
			covered.insert(edge_key(f, f.skirt_edge));
		} else {
			for (int k = 0; k < 4; k++) ++incidence[edge_key(f, k)];
		}
	}
	int added = 0;
	for (size_t i = 0; i < surface; i++) {
		LodQuadFields f{};
		lod_quad_unpack((*quads)[i], &f);
		if (f.double_sided) continue;
		for (int k = 0; k < 4; k++) {
			const uint64_t key = edge_key(f, k);
			if (incidence[key] != 1 || covered.count(key)) continue;
			const int face = boundary_face(f, k);
			if (face < 0) continue;
			LodQuadFields skirt = f;
			skirt.double_sided = 1;
			skirt.skirt_face = uint8_t(face);
			skirt.skirt_edge = uint8_t(k);
			if (!lod_quad_skirt_supported(skirt)) {
				if (status) status->unsupported_edges++;
				continue;
			}
			if (quads->size() + 2 > size_t(kLodMaxQuadsPerChunk)) {
				if (status) status->capacity_edges++;
				continue;
			}
			for (int reverse = 0; reverse < 2; reverse++) {
				skirt.reverse_winding = uint8_t(reverse);
				LodQuad q;
				lod_quad_pack(skirt, &q);
				quads->push_back(q);
				if (normals) {
					LodQuadNormals ribbon{};
					const int order_rev[4] = {0, 3, 2, 1};
					for (int corner = 0; corner < 4; ++corner) {
						const int kk = reverse ? order_rev[corner] : corner;
						const int endpoint = (k + ((kk == 0 || kk == 3) ? 1 : 0)) & 3;
						ribbon.corner[corner] = (*normals)[i].corner[endpoint];
					}
					normals->push_back(ribbon);
				}
			}
			covered.insert(key);
			added += 2;
		}
	}
	return added;
}

} // namespace ve
