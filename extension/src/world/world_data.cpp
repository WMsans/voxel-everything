#include "world/world_data.h"
#include "world/palette.h"
#include <algorithm>

namespace ve {

WorldData::WorldData(int bx, int by, int bz)
	: dims_{bx, by, bz}, indirection_(static_cast<size_t>(bx) * by * bz, -1) {}

void WorldData::generate(const Generator &gen) {
	// Activation probe: 27 sample points per brick; active if the SDF brackets 0.
	// Conservative pad accounts for undersampling between probe points.
	const float pad = 0.15f;
	for (int bz = 0; bz < dims_.z; bz++)
		for (int by = 0; by < dims_.y; by++)
			for (int bx = 0; bx < dims_.x; bx++) {
				float mn = 1e30f, mx = -1e30f;
				for (int sz = 0; sz < 3; sz++)
					for (int sy = 0; sy < 3; sy++)
						for (int sx = 0; sx < 3; sx++) {
							const float wx = (bx * kBrickVoxels + sx * kBrickVoxels / 2) * kVoxelSize;
							const float wy = (by * kBrickVoxels + sy * kBrickVoxels / 2) * kVoxelSize;
							const float wz = (bz * kBrickVoxels + sz * kBrickVoxels / 2) * kVoxelSize;
							const float d = gen.sample(wx, wy, wz).sdf;
							mn = std::min(mn, d);
							mx = std::max(mx, d);
						}
				if (mn >= pad || mx <= -pad) continue; // fully air or fully solid

				const int slot = static_cast<int>(bricks_.size());
				indirection_[static_cast<size_t>(bx) + by * dims_.x + bz * dims_.x * dims_.y] = slot;
				bricks_.emplace_back();
				Brick &b = bricks_.back();
				bool overflow = false;
				// The SDF runs over the 17^3 lattice (see kBrickSdfStride): the extra plane at
				// local 16 on each axis is the apron the shader's trilinear filter needs in order
				// to cover the brick's last voxel slab. Materials stay on the 16^3 cell grid.
				for (int vz = 0; vz < kBrickSdfStride; vz++)
					for (int vy = 0; vy < kBrickSdfStride; vy++)
						for (int vx = 0; vx < kBrickSdfStride; vx++) {
							const float wx = (bx * kBrickVoxels + vx) * kVoxelSize;
							const float wy = (by * kBrickVoxels + vy) * kVoxelSize;
							const float wz = (bz * kBrickVoxels + vz) * kVoxelSize;
							const Sample s = gen.sample(wx, wy, wz);
							b.sdf[sdf_index(vx, vy, vz)] = encode_sdf(s.sdf);
							const bool apron = vx == kBrickVoxels || vy == kBrickVoxels || vz == kBrickVoxels;
							if (!apron && s.material != 0) {
								const int pslot = palette_slot(b.palette, s.material, &overflow);
								set_mat_index(b, voxel_index(vx, vy, vz), static_cast<uint8_t>(pslot));
							}
						}
			}
}

bool WorldData::brick_active(int bx, int by, int bz) const {
	return brick_slot(bx, by, bz) >= 0;
}

int WorldData::brick_slot(int bx, int by, int bz) const {
	if (bx < 0 || by < 0 || bz < 0 || bx >= dims_.x || by >= dims_.y || bz >= dims_.z) return -1;
	return indirection_[static_cast<size_t>(bx) + by * dims_.x + bz * dims_.x * dims_.y];
}

} // namespace ve
