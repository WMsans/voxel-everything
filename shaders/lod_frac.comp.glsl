#[compute]
#version 460

#include "common.glslh"
#include "lod_common.glslh"

// One thread per mesh cell. Identical arithmetic to shaders/mesh_cells.comp.glsl, but it
// stores the vertex as a FRACTION of its own cell (5 bits per axis, packed) rather than a
// world position: that is exactly what the 12-byte record carries, so the quads pass needs
// no division and the GPU and ve::lod_contour quantise the same number the same way.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, std430) writeonly buffer Frac { uint v[]; } frac;

const ivec3 CORNER[8] = ivec3[8](ivec3(0, 0, 0), ivec3(1, 0, 0), ivec3(0, 1, 0), ivec3(1, 1, 0),
		ivec3(0, 0, 1), ivec3(1, 0, 1), ivec3(0, 1, 1), ivec3(1, 1, 1));
const ivec2 EDGE[12] = ivec2[12](ivec2(0, 1), ivec2(2, 3), ivec2(4, 5), ivec2(6, 7),
		ivec2(0, 2), ivec2(1, 3), ivec2(4, 6), ivec2(5, 7),
		ivec2(0, 4), ivec2(1, 5), ivec2(2, 6), ivec2(3, 7));

void main() {
	ivec3 m = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(m, ivec3(LOD_CHUNK_MESH_CELLS)))) return;
	int ci = m.x + m.y * LOD_CHUNK_MESH_CELLS +
			m.z * LOD_CHUNK_MESH_CELLS * LOD_CHUNK_MESH_CELLS;

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
	// Every cell is written every job: the buffer is shared by the batch and never cleared
	// between jobs, so "no vertex" has to be stored, not left behind.
	if (n == 0) { frac.v[ci] = 0xFFFFFFFFu; return; }

	vec3 f = clamp(acc / float(n), vec3(0.0), vec3(1.0));
	uvec3 q = uvec3(floor(f * float(LOD_OFFSET_MAX) + 0.5));
	frac.v[ci] = q.x | (q.y << 5) | (q.z << 10);
}
