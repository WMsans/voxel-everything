#[compute]
#version 460

#include "common.glslh"
#include "lod_common.glslh"

// One thread per owned edge coordinate; each handles that point's three axis edges. Mirror
// of ve::lod_contour's second pass, emitting the packed 12-byte record instead of triangle
// indices. Corners are written ALREADY WOUND so the vertex shader never branches.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, r16ui) readonly uniform uimage3D material;
// Fractional vertex position per mesh cell, quantised to 5 bits per axis and packed into one
// uint, or 0xFFFFFFFF when the cell holds no vertex.
layout(set = 0, binding = 2, std430) readonly buffer Frac { uint v[]; } frac;
// Three uints per quad: ve::LodQuad.
layout(set = 0, binding = 3, std430) writeonly buffer Quads { uint v[]; } quads;
// Two uints per job: quad count, overflow flag.
layout(set = 0, binding = 4, std430) buffer Counts { uint v[]; } counts;

void bits_set(inout uvec3 w, int lo, int bits, uint v) {
	uint mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << uint(bits)) - 1u);
	v &= mask;
	int word = lo >> 5;
	int shift = lo & 31;
	if (word == 0) w.x |= v << uint(shift); else if (word == 1) w.y |= v << uint(shift);
	else w.z |= v << uint(shift);
	int spill = shift + bits - 32;
	if (spill > 0) {
		uint hi = v >> uint(32 - shift);
		if (word == 0) w.y |= hi; else w.z |= hi;
	}
}

int cell_index(ivec3 m) {
	return m.x + m.y * LOD_CHUNK_MESH_CELLS + m.z * LOD_CHUNK_MESH_CELLS * LOD_CHUNK_MESH_CELLS;
}

void main() {
	ivec3 u = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(u, ivec3(LOD_CHUNK_CELLS)))) return;
	ivec3 L = u + 1;
	uint job = uint(lpc.job.w);
	float da = decode_sdf(imageLoad(lattice, L).r);

	for (int axis = 0; axis < 3; axis++) {
		ivec3 e = ivec3(0);
		e[axis] = 1;
		float db = decode_sdf(imageLoad(lattice, L + e).r);
		bool sa = da <= 0.0, sb = db <= 0.0;
		if (sa == sb) continue;
		int b = (axis + 1) % 3, c = (axis + 2) % 3;
		uint f[4];
		bool ok = true;
		for (int k = 0; k < 4; k++) {
			ivec3 m = L;
			m[b] += LOD_QUAD[k].x;
			m[c] += LOD_QUAD[k].y;
			f[k] = frac.v[cell_index(m)];
			if (f[k] == 0xFFFFFFFFu) ok = false;
		}
		if (!ok) continue;

		uint t = atomicAdd(counts.v[job * 2u + 0u], 1u);
		if (t >= uint(lpc.params.y)) { atomicOr(counts.v[job * 2u + 1u], 1u); return; }

		uvec3 w = uvec3(0u);
		bits_set(w, 0, 5, uint(u.x));
		bits_set(w, 5, 5, uint(u.y));
		bits_set(w, 10, 5, uint(u.z));
		bits_set(w, 15, 2, uint(axis));
		bits_set(w, 17, 1, sa ? 1u : 0u);
		// Already wound: (0,1,2,3) when the low end is solid, (0,3,2,1) otherwise -- the two
		// triangles of the reversed order are exactly ve::dual_contour's tri_rev pair.
		int order[4] = int[4](0, 1, 2, 3);
		if (!sa) { order[1] = 3; order[3] = 1; }
		for (int k = 0; k < 4; k++) {
			uint p = f[order[k]];
			for (int a = 0; a < 3; a++)
				bits_set(w, 18 + (k * 3 + a) * 5, 5, (p >> uint(a * 5)) & 31u);
		}
		ivec3 ms = sa ? L : (L + e);
		bits_set(w, 78, 16, imageLoad(material, ms).r);
		// bit 94 (double-sided) stays 0: skirts are appended on the CPU.

		uint base = (job * uint(lpc.params.y) + t) * 3u;
		quads.v[base + 0u] = w.x;
		quads.v[base + 1u] = w.y;
		quads.v[base + 2u] = w.z;
	}
}
