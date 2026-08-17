#[compute]
#version 460

#include "common.glslh"
#include "lod_common.glslh"

// One thread per TARGET lattice sample. The SDF averages (symmetric: a crater and a spire
// survive equally, which a solid-preferring min would not); the material is a tent-weighted
// majority over the SOLID taps only, ties broken by the centre tap. Mirror of
// ve::lod_reduce_lattice.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D fine_sdf;
layout(set = 0, binding = 1, r16ui) readonly uniform uimage3D fine_mat;
layout(set = 0, binding = 2, r8) writeonly uniform image3D out_sdf;
layout(set = 0, binding = 3, r16ui) writeonly uniform uimage3D out_mat;

void main() {
	ivec3 i = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(i, ivec3(LOD_CHUNK_LATTICE)))) return;

	float acc = 0.0;
	uint ids[27];
	float votes[27];
	int n_ids = 0;
	uint centre_mat = 0u;
	for (int dz = 0; dz < 3; dz++)
		for (int dy = 0; dy < 3; dy++)
			for (int dx = 0; dx < 3; dx++) {
				ivec3 j = ivec3(2 * i.x + dx, 2 * i.y + dy, 2 * i.z + dz);
				float w = LOD_TENT[dx] * LOD_TENT[dy] * LOD_TENT[dz];
				float d = decode_sdf(imageLoad(fine_sdf, j).r);
				acc += w * d;
				uint m = imageLoad(fine_mat, j).r;
				if (dx == 1 && dy == 1 && dz == 1) centre_mat = m;
				if (d > 0.0) continue;
				if (m == 0u) continue;
				int slot = -1;
				for (int s = 0; s < n_ids; s++) { if (ids[s] == m) { slot = s; break; } }
				if (slot < 0) { slot = n_ids++; ids[slot] = m; votes[slot] = 0.0; }
				votes[slot] += w;
			}

	uint best = 0u;
	float best_v = 0.0;
	for (int s = 0; s < n_ids; s++) {
		if (votes[s] > best_v) { best_v = votes[s]; best = ids[s]; }
	}
	if (n_ids > 1) {
		for (int s = 0; s < n_ids; s++) {
			if (ids[s] == centre_mat && votes[s] >= best_v) { best = centre_mat; break; }
		}
	}
	imageStore(out_sdf, i, vec4(quantise_sdf(acc)));
	imageStore(out_mat, i, uvec4(best, 0u, 0u, 0u));
}
