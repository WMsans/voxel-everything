#include "lod/lod_reduce.h"
#include "lod/lod_grid.h"
#include "world/brick.h"

namespace ve {

float lod_fine_local(int j) { return (static_cast<float>(j) - 3.0f) * 0.5f; }

uint8_t lod_encode_sdf(float sdf, float cell_size) {
	// Keep L0 byte-identical. At coarser levels, +/-2 cells contains a crossing
	// edge's endpoints AND their half-cell tent taps: for a unit plane normal n,
	// the largest distance is cell * (abs(n[axis]) + 0.5 * sum(abs(n))), bounded
	// by sqrt(2.75) * cell < 2 * cell. A fixed metre range clips before averaging
	// and moves the zero set as the plane's phase changes.
	// Mirror shaders/lod_field.comp.glsl. Reduction and contouring consume scaled
	// distances, not metres; averaging, signs and crossing ratios retain this scale.
	const float scale = cell_size <= kLodBaseCell ? 1.0f : kSdfRange / (2.0f * cell_size);
	return encode_sdf(sdf * scale);
}

int lod_fine_index(int x, int y, int z) {
	return x + y * kLodFineLattice + z * kLodFineLattice * kLodFineLattice;
}

int lod_lattice_index(int x, int y, int z) {
	return x + y * kLodChunkLattice + z * kLodChunkLattice * kLodChunkLattice;
}

void lod_reduce_lattice(const uint8_t *fine_sdf, const uint16_t *fine_mat, uint8_t *out_sdf,
		uint16_t *out_mat) {
	// At most 27 distinct ids in one neighbourhood, so a linear scan beats a map.
	uint16_t ids[27];
	float votes[27];
	for (int z = 0; z < kLodChunkLattice; z++)
		for (int y = 0; y < kLodChunkLattice; y++)
			for (int x = 0; x < kLodChunkLattice; x++) {
				float acc = 0.0f;
				int n_ids = 0;
				uint16_t centre_mat = 0;
				for (int dz = 0; dz < 3; dz++)
					for (int dy = 0; dy < 3; dy++)
						for (int dx = 0; dx < 3; dx++) {
							const int fi = lod_fine_index(2 * x + dx, 2 * y + dy, 2 * z + dz);
							const float w = kLodTentWeights[dx] * kLodTentWeights[dy] *
									kLodTentWeights[dz];
							const float d = decode_sdf(fine_sdf[fi]);
							acc += w * d;
							if (dx == 1 && dy == 1 && dz == 1) centre_mat = fine_mat[fi];
							// Only solid taps vote: a material id labels matter, and an air
							// tap has no matter to label.
							if (d > 0.0f) continue;
							const uint16_t m = fine_mat[fi];
							if (m == 0u) continue;
							int slot = -1;
							for (int s = 0; s < n_ids; s++)
								if (ids[s] == m) { slot = s; break; }
							if (slot < 0) {
								slot = n_ids++;
								ids[slot] = m;
								votes[slot] = 0.0f;
							}
							votes[slot] += w;
						}
				const int oi = lod_lattice_index(x, y, z);
				out_sdf[oi] = encode_sdf(acc);
				uint16_t best = 0;
				float best_v = 0.0f;
				for (int s = 0; s < n_ids; s++) {
					// Strict >: the first id to reach a weight wins, and the tie-break below
					// then prefers the centre tap. Deterministic on both CPU and GPU.
					if (votes[s] > best_v) { best_v = votes[s]; best = ids[s]; }
				}
				if (n_ids > 1) {
					for (int s = 0; s < n_ids; s++)
						if (ids[s] == centre_mat && votes[s] >= best_v) { best = centre_mat; break; }
				}
				out_mat[oi] = best;
			}
}

} // namespace ve
