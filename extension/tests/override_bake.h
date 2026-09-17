#pragma once
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "world/override_store.h"

namespace ve_test {

// Bake one brick's lattice out of the generator plus ops, exactly as the GPU pass will.
inline void bake_override(const ve::Generator &gen, const ve::EditOp *ops, int n, ve::IVec3 brick,
		ve::OverrideBrick *out) {
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const float wx = (static_cast<float>(brick.x) * ve::kBrickVoxels + x) * ve::kVoxelSize;
				const float wy = (static_cast<float>(brick.y) * ve::kBrickVoxels + y) * ve::kVoxelSize;
				const float wz = (static_cast<float>(brick.z) * ve::kBrickVoxels + z) * ve::kVoxelSize;
				const ve::Sample s = ve::eval_field(gen, ops, n, wx, wy, wz);
				out->sdf[ve::sdf_index(x, y, z)] = ve::encode_sdf(s.sdf);
			}
	for (int z = 0; z < ve::kBrickVoxels; z++)
		for (int y = 0; y < ve::kBrickVoxels; y++)
			for (int x = 0; x < ve::kBrickVoxels; x++) {
				const float wx = (static_cast<float>(brick.x) * ve::kBrickVoxels + x + 0.5f) * ve::kVoxelSize;
				const float wy = (static_cast<float>(brick.y) * ve::kBrickVoxels + y + 0.5f) * ve::kVoxelSize;
				const float wz = (static_cast<float>(brick.z) * ve::kBrickVoxels + z + 0.5f) * ve::kVoxelSize;
				const ve::Sample s = ve::eval_field(gen, ops, n, wx, wy, wz);
				out->mat[x + y * ve::kBrickVoxels + z * ve::kBrickVoxels * ve::kBrickVoxels] =
						static_cast<uint8_t>(s.material & 0xFFu);
			}
}

} // namespace ve_test
