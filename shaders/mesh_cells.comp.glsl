#[compute]
#version 460

#include "mesh_common.glsl"

// One thread per mesh cell. 129 is not a multiple of 4, so the last group of each axis runs
// partly out of bounds and returns.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, std430) writeonly buffer Cells { int v[]; } cells;
layout(set = 0, binding = 2, std430) writeonly buffer Verts { float v[]; } verts;
// vert count, tri count, overflow bits, pad — four uints per job.
layout(set = 0, binding = 3, std430) buffer Counts { uint v[]; } counts;

layout(push_constant, std430) uniform Push {
	ivec4 chunk;  // xyz = chunk coordinates, w = job index in this batch
	ivec4 params; // x = op count, y = max verts per job, z = max tris per job, w = unused
} pc;

// Cell corners indexed by (x | y<<1 | z<<2); mirror of kCorner in dual_contour.cpp.
const ivec3 CORNER[8] = ivec3[8](ivec3(0, 0, 0), ivec3(1, 0, 0), ivec3(0, 1, 0), ivec3(1, 1, 0),
		ivec3(0, 0, 1), ivec3(1, 0, 1), ivec3(0, 1, 1), ivec3(1, 1, 1));
// The 12 edges as corner pairs, in the SAME order as kEdge in dual_contour.cpp.
const ivec2 EDGE[12] = ivec2[12](ivec2(0, 1), ivec2(2, 3), ivec2(4, 5), ivec2(6, 7),
		ivec2(0, 2), ivec2(1, 3), ivec2(4, 6), ivec2(5, 7),
		ivec2(0, 4), ivec2(1, 5), ivec2(2, 6), ivec2(3, 7));

void main() {
	ivec3 m = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(m, ivec3(CHUNK_MESH_CELLS)))) return;
	int ci = mesh_cell_index(m);
	uint job = uint(pc.chunk.w);

	float d[8];
	for (int k = 0; k < 8; k++) d[k] = decode_sdf(imageLoad(lattice, m + CORNER[k]).r);

	vec3 acc = vec3(0.0);
	int n = 0;
	for (int e = 0; e < 12; e++) {
		float da = d[EDGE[e].x], db = d[EDGE[e].y];
		if ((da <= 0.0) == (db <= 0.0)) continue;
		float t = da / (da - db);
		acc += vec3(CORNER[EDGE[e].x]) + t * vec3(CORNER[EDGE[e].y] - CORNER[EDGE[e].x]);
		n++;
	}
	// Every cell is written every job: the cell map is shared by the batch and is never
	// cleared between jobs, so "no vertex" has to be stored, not left behind.
	if (n == 0) { cells.v[ci] = -1; return; }

	uint idx = atomicAdd(counts.v[job * 4u + 0u], 1u);
	if (idx >= uint(pc.params.y)) {
		// Fail-soft (spec §8): the chunk loses this vertex and the quads that needed it, the
		// overflow bit reaches the CPU with the result, and the collider is built from what
		// did fit. A partial collider beats none.
		atomicOr(counts.v[job * 4u + 2u], 1u);
		cells.v[ci] = -1;
		return;
	}
	vec3 p = vec3(pc.chunk.xyz) * CHUNK_SIZE +
			(vec3(m) - 1.0 + acc / float(n)) * CHUNK_CELL_SIZE;
	uint base = (job * uint(pc.params.y) + idx) * 3u;
	verts.v[base + 0u] = p.x;
	verts.v[base + 1u] = p.y;
	verts.v[base + 2u] = p.z;
	cells.v[ci] = int(idx);
}
