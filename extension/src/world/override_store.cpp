#include "world/override_store.h"
#include <algorithm>
#include <cmath>

namespace ve {

OverrideStore::OverrideStore(int capacity) : bricks_(static_cast<size_t>(std::max(capacity, 0))) {
	free_.reserve(bricks_.size());
	for (int i = static_cast<int>(bricks_.size()) - 1; i >= 0; i--) free_.push_back(i);
}

int OverrideStore::acquire(IVec3 brick) {
	const Key key{brick.x, brick.y, brick.z};
	const auto found = index_.find(key);
	if (found != index_.end()) return found->second;
	if (free_.empty()) return -1;
	const int slot = free_.back();
	free_.pop_back();
	index_.emplace(key, slot);
	return slot;
}

int OverrideStore::slot_of(IVec3 brick) const {
	const auto found = index_.find(Key{brick.x, brick.y, brick.z});
	return found == index_.end() ? -1 : found->second;
}

void OverrideStore::release(IVec3 brick) {
	const auto found = index_.find(Key{brick.x, brick.y, brick.z});
	if (found == index_.end()) return;
	free_.push_back(found->second);
	index_.erase(found);
}

void OverrideStore::clear() {
	index_.clear();
	free_.clear();
	free_.reserve(bricks_.size());
	for (int i = static_cast<int>(bricks_.size()) - 1; i >= 0; i--) free_.push_back(i);
}

OverrideBrick *OverrideStore::data(int slot) {
	if (slot < 0 || slot >= static_cast<int>(bricks_.size())) return nullptr;
	return &bricks_[static_cast<size_t>(slot)];
}

const OverrideBrick *OverrideStore::data(int slot) const {
	if (slot < 0 || slot >= static_cast<int>(bricks_.size())) return nullptr;
	return &bricks_[static_cast<size_t>(slot)];
}

bool OverrideStore::sample(float x, float y, float z, Sample *out) const {
	if (!out) return false;
	const IVec3 brick = WorldBounds::brick_of_point(x, y, z);
	const int slot = slot_of(brick);
	if (slot < 0) return false;
	const OverrideBrick &b = bricks_[static_cast<size_t>(slot)];

	float origin[3];
	brick_world_origin(brick, origin);
	const float local[3] = {
			std::clamp((x - origin[0]) / kVoxelSize, 0.0f, static_cast<float>(kBrickVoxels)),
			std::clamp((y - origin[1]) / kVoxelSize, 0.0f, static_cast<float>(kBrickVoxels)),
			std::clamp((z - origin[2]) / kVoxelSize, 0.0f, static_cast<float>(kBrickVoxels))};
	const int i0[3] = {static_cast<int>(std::floor(local[0])),
			static_cast<int>(std::floor(local[1])), static_cast<int>(std::floor(local[2]))};
	const int i1[3] = {std::min(i0[0] + 1, kBrickVoxels),
			std::min(i0[1] + 1, kBrickVoxels), std::min(i0[2] + 1, kBrickVoxels)};
	const float f[3] = {local[0] - i0[0], local[1] - i0[1], local[2] - i0[2]};
	auto lattice = [&b](int lx, int ly, int lz) {
		return decode_sdf(b.sdf[sdf_index(lx, ly, lz)]);
	};
	auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
	const float c000 = lattice(i0[0], i0[1], i0[2]);
	const float c100 = lattice(i1[0], i0[1], i0[2]);
	const float c010 = lattice(i0[0], i1[1], i0[2]);
	const float c110 = lattice(i1[0], i1[1], i0[2]);
	const float c001 = lattice(i0[0], i0[1], i1[2]);
	const float c101 = lattice(i1[0], i0[1], i1[2]);
	const float c011 = lattice(i0[0], i1[1], i1[2]);
	const float c111 = lattice(i1[0], i1[1], i1[2]);
	out->sdf = mix(mix(mix(c000, c100, f[0]), mix(c010, c110, f[0]), f[1]),
			mix(mix(c001, c101, f[0]), mix(c011, c111, f[0]), f[1]), f[2]);

	const int cell[3] = {std::min(static_cast<int>(std::floor(local[0])), kBrickVoxels - 1),
			std::min(static_cast<int>(std::floor(local[1])), kBrickVoxels - 1),
			std::min(static_cast<int>(std::floor(local[2])), kBrickVoxels - 1)};
	out->material = b.mat[voxel_index(cell[0], cell[1], cell[2])];
	return true;
}

void plan_consolidation(const EditOp *ops, int op_count, IVec3 region,
		std::vector<IVec3> *bricks) {
	if (!bricks) return;
	bricks->clear();
	if (!ops || op_count <= 0) return;
	const int count = std::max(op_count, 0);
	const float pad = kSdfRange + kVoxelSize;
	const IVec3 base{region.x * kRegionBricks, region.y * kRegionBricks,
			region.z * kRegionBricks};
	for (int z = 0; z < kRegionBricks; z++)
		for (int y = 0; y < kRegionBricks; y++)
			for (int x = 0; x < kRegionBricks; x++) {
				const IVec3 brick{base.x + x, base.y + y, base.z + z};
				float lo[3], hi[3];
				brick_world_aabb(brick, lo, hi);
				for (int i = 0; i < count; i++) {
					if (!op_touches_aabb(ops[i], lo, hi, pad)) continue;
					bricks->push_back(brick);
					break;
				}
			}
}

} // namespace ve
