#pragma once
#include "mesh/mesh_chunk.h"
#include <cstdint>
#include <vector>

namespace ve {

// The lattice a mesher run covers. `lattice` samples per axis means `lattice - 1` mesh cells
// and `lattice - 2` owned edge coordinates: array index m holds the cell at local coordinate
// m - 1, so the grid always carries one overlap cell below its origin (mesh_chunk.h).
// Parameterised rather than fixed at the chunk size so the native tests can run on a 18^3
// grid in microseconds — and so M5's LoD chunks can reuse the mesher at their own pitch.
struct DcGrid {
	int lattice = kChunkLattice;
	float cell_size = kChunkCellSize;
	float origin[3] = {0.0f, 0.0f, 0.0f};
	int cells() const { return lattice - 1; }
	int owned() const { return lattice - 2; }
};

DcGrid chunk_dc_grid(IVec3 chunk);

// x fastest, then y, then z — the layout shaders/mesh_*.comp.glsl writes.
int dc_lattice_index(const DcGrid &g, int x, int y, int z);
int dc_cell_index(const DcGrid &g, int x, int y, int z);

struct MeshBuffer {
	std::vector<float> positions;    // 3 per vertex, WORLD space
	std::vector<uint32_t> indices;   // 3 per triangle
	std::vector<int32_t> cell_vertex; // mesh cell -> vertex index, or -1
	int vertex_count() const { return static_cast<int>(positions.size() / 3); }
	int triangle_count() const { return static_cast<int>(indices.size() / 3); }
};

// Dual contouring with mass-point vertex placement (spec §6). One vertex per cell the surface
// crosses, at the average of that cell's edge crossings; one quad per sign-changing lattice
// edge, from the four cells around it. The QEF sharp-feature term is deliberately absent —
// see the plan's Deliberate Decisions.
//
// `lattice` holds g.lattice^3 ENCODED sdf bytes in dc_lattice_index order. Solid is
// decode_sdf(byte) <= 0, matching the generator's own `if (sdf <= 0) material = ...`.
void dual_contour(const uint8_t *lattice, const DcGrid &g, MeshBuffer *out);

} // namespace ve
