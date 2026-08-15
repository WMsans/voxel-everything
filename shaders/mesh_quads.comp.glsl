#[compute]
#version 460

#include "common.glslh"
#include "mesh_common.glslh"

// One thread per owned edge coordinate; each handles that point's three axis edges.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, std430) readonly buffer Cells { int v[]; } cells;
layout(set = 0, binding = 2, std430) writeonly buffer Tris { uint v[]; } tris;
layout(set = 0, binding = 3, std430) buffer Counts { uint v[]; } counts;

layout(push_constant, std430) uniform Push {
	ivec4 chunk;
	ivec4 params;
} pc;

// The four cells around a lattice edge, as offsets in the two axes perpendicular to it,
// counter-clockwise seen from +axis; mirror of kQuad in dual_contour.cpp.
const ivec2 QUAD[4] = ivec2[4](ivec2(-1, -1), ivec2(0, -1), ivec2(0, 0), ivec2(-1, 0));

void emit(uint job, int a, int b, int c) {
	uint t = atomicAdd(counts.v[job * 4u + 1u], 1u);
	if (t >= uint(pc.params.z)) { atomicOr(counts.v[job * 4u + 2u], 2u); return; }
	uint base = (job * uint(pc.params.z) + t) * 3u;
	tris.v[base + 0u] = uint(a);
	tris.v[base + 1u] = uint(b);
	tris.v[base + 2u] = uint(c);
}

void main() {
	ivec3 u = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(u, ivec3(CHUNK_CELLS)))) return;
	// This chunk owns the edges whose four cells it holds: local coordinate u in
	// [0, CHUNK_CELLS), lattice index u + 1. Every edge in the world is emitted by exactly
	// one chunk, so chunk borders have neither cracks nor duplicated triangles.
	ivec3 L = u + 1;
	uint job = uint(pc.chunk.w);
	float da = decode_sdf(imageLoad(lattice, L).r);

	for (int axis = 0; axis < 3; axis++) {
		ivec3 e = ivec3(0);
		e[axis] = 1;
		float db = decode_sdf(imageLoad(lattice, L + e).r);
		bool sa = da <= 0.0, sb = db <= 0.0;
		if (sa == sb) continue;
		int b = (axis + 1) % 3, c = (axis + 2) % 3;
		int q[4];
		bool ok = true;
		for (int k = 0; k < 4; k++) {
			ivec3 m = L;
			m[b] += QUAD[k].x;
			m[c] += QUAD[k].y;
			q[k] = cells.v[mesh_cell_index(m)];
			if (q[k] < 0) ok = false;
		}
		// Only reachable when a neighbour lost its vertex to the cap: skip the quad rather
		// than emit an index into nothing.
		if (!ok) continue;
		// (axis, b, c) is a right-handed cycle, so q0..q3 wind counter-clockwise seen from
		// +axis. A solid -> air step along +axis puts the air on the +axis side, which is
		// the side the normal must face.
		if (sa) {
			emit(job, q[0], q[1], q[2]);
			emit(job, q[0], q[2], q[3]);
		} else {
			emit(job, q[0], q[2], q[1]);
			emit(job, q[0], q[3], q[2]);
		}
	}
}
