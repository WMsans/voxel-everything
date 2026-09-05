#include "world/override_store.h"
#include "shade/oct.h"
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
	const IVec3 brick = ve::brick_of_point(x, y, z);
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

bool OverrideStore::sample_gradient(float x, float y, float z, FieldSample *out) const {
	if (!out) return false;
	Sample base{};
	if (!sample(x, y, z, &base)) return false;
	const IVec3 brick = ve::brick_of_point(x, y, z);
	const int slot = slot_of(brick);
	if (slot < 0) return false;
	const OverrideBrick &b = bricks_[static_cast<size_t>(slot)];
	if (b.normal_oct.size() != static_cast<size_t>(kBrickSdfCount)) {
		// The bake exists but its normal lattice is missing (normal_oct empty -- the
		// designed fail-soft state from the brief's Step 6). Keep the baked value/material
		// but report an inexact ZERO gradient, mirroring the volume path's no-normals
		// branch, so the caller's terrain_source_normal shades through the wide R8
		// fallback instead of the unedited procedural gradient.
		out->sdf = base.sdf;
		out->material = base.material;
		out->gradient[0] = 0.0f;
		out->gradient[1] = 0.0f;
		out->gradient[2] = 0.0f;
		out->exact_gradient = false;
		return true;
	}
	out->sdf = base.sdf;
	out->material = base.material;
	// trilinear blend of 17^3 normal lattice
	float origin[3];
	brick_world_origin(brick, origin);
	const float local[3] = {
			std::clamp((x - origin[0]) / kVoxelSize, 0.0f, static_cast<float>(kBrickVoxels)),
			std::clamp((y - origin[1]) / kVoxelSize, 0.0f, static_cast<float>(kBrickVoxels)),
			std::clamp((z - origin[2]) / kVoxelSize, 0.0f, static_cast<float>(kBrickVoxels))};
	const int i0[3] = {static_cast<int>(std::floor(local[0])), static_cast<int>(std::floor(local[1])), static_cast<int>(std::floor(local[2]))};
	const int i1[3] = {std::min(i0[0] + 1, kBrickVoxels), std::min(i0[1] + 1, kBrickVoxels), std::min(i0[2] + 1, kBrickVoxels)};
	const float f[3] = {local[0] - i0[0], local[1] - i0[1], local[2] - i0[2]};
	float n000[3], n100[3], n010[3], n110[3], n001[3], n101[3], n011[3], n111[3];
	oct_decode_snorm8(b.normal_oct[static_cast<size_t>(sdf_index(i0[0], i0[1], i0[2]))], n000);
	oct_decode_snorm8(b.normal_oct[static_cast<size_t>(sdf_index(i1[0], i0[1], i0[2]))], n100);
	oct_decode_snorm8(b.normal_oct[static_cast<size_t>(sdf_index(i0[0], i1[1], i0[2]))], n010);
	oct_decode_snorm8(b.normal_oct[static_cast<size_t>(sdf_index(i1[0], i1[1], i0[2]))], n110);
	oct_decode_snorm8(b.normal_oct[static_cast<size_t>(sdf_index(i0[0], i0[1], i1[2]))], n001);
	oct_decode_snorm8(b.normal_oct[static_cast<size_t>(sdf_index(i1[0], i0[1], i1[2]))], n101);
	oct_decode_snorm8(b.normal_oct[static_cast<size_t>(sdf_index(i0[0], i1[1], i1[2]))], n011);
	oct_decode_snorm8(b.normal_oct[static_cast<size_t>(sdf_index(i1[0], i1[1], i1[2]))], n111);
	float nx00[3] = { n000[0]*(1-f[0]) + n100[0]*f[0], n000[1]*(1-f[0]) + n100[1]*f[0], n000[2]*(1-f[0]) + n100[2]*f[0] };
	float nx10[3] = { n010[0]*(1-f[0]) + n110[0]*f[0], n010[1]*(1-f[0]) + n110[1]*f[0], n010[2]*(1-f[0]) + n110[2]*f[0] };
	float nx01[3] = { n001[0]*(1-f[0]) + n101[0]*f[0], n001[1]*(1-f[0]) + n101[1]*f[0], n001[2]*(1-f[0]) + n101[2]*f[0] };
	float nx11[3] = { n011[0]*(1-f[0]) + n111[0]*f[0], n011[1]*(1-f[0]) + n111[1]*f[0], n011[2]*(1-f[0]) + n111[2]*f[0] };
	float nxy0[3] = { nx00[0]*(1-f[1]) + nx10[0]*f[1], nx00[1]*(1-f[1]) + nx10[1]*f[1], nx00[2]*(1-f[1]) + nx10[2]*f[1] };
	float nxy1[3] = { nx01[0]*(1-f[1]) + nx11[0]*f[1], nx01[1]*(1-f[1]) + nx11[1]*f[1], nx01[2]*(1-f[1]) + nx11[2]*f[1] };
	float nxyz[3] = { nxy0[0]*(1-f[2]) + nxy1[0]*f[2], nxy0[1]*(1-f[2]) + nxy1[1]*f[2], nxy0[2]*(1-f[2]) + nxy1[2]*f[2] };
	float len = std::sqrt(nxyz[0]*nxyz[0] + nxyz[1]*nxyz[1] + nxyz[2]*nxyz[2]);
	if (len > 1e-6f) { out->gradient[0]=nxyz[0]/len; out->gradient[1]=nxyz[1]/len; out->gradient[2]=nxyz[2]/len; }
	else { out->gradient[0]=0; out->gradient[1]=1; out->gradient[2]=0; }
	out->exact_gradient = true;
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
