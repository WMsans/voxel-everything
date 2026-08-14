#include "world/world_data.h"
#include "world/palette.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

// Give every cell of a brick a material, not just the solid ones.
//
// A ray's hit point routinely lands on the AIR side of the surface: the march stops at
// d < 0.002 and the secant refinement can leave p just outside, so material_at() rounds to
// an air cell. The packed 2-bit index of an unassigned cell is 0, which is indistinguishable
// from palette slot 0 -- so an air cell silently renders as the brick's FIRST material.
// Where a brick holds one material that is accidentally right; where it straddles a band
// boundary it is wrong, and the two answers interleave into a dithered fringe.
//
// Giving every cell the material of the surface nearest to it removes the failure mode: an
// air-side lookup now resolves to the surface the ray actually hit.
void spread_materials(uint16_t *mat, const Brick &b, const Generator &gen, int bx, int by, int bz) {
	// Pass 1: project each near-surface air cell onto the surface along -grad(SDF) and ask
	// the GENERATOR for the material there. Two reasons not to copy from a neighbouring cell
	// instead: the closest surface point is what a ray hitting near this cell would shade,
	// which on a slope is not the L1-nearest solid cell; and that surface often lies in the
	// next brick along -- a cell on this brick's bottom plane belongs to the surface below
	// it, whose material may appear nowhere in this brick at all. The generator has no such
	// boundary, so it answers correctly in both cases.
	auto lat = [&b](int x, int y, int z) {
		return decode_sdf(b.sdf[sdf_index(x, y, z)]);
	};
	// Central difference along one axis, divided by the span actually sampled. On a brick's
	// outer planes the lattice has no neighbour on one side, so the difference is one-sided
	// over a single voxel; dividing every axis by a fixed 2 would then halve that component
	// and tilt the "normal" towards the horizontal, sending the projection below sideways
	// across a material band instead of straight down onto the surface underneath.
	auto slope = [&lat](int x, int y, int z, int axis) {
		int lo[3] = {x, y, z}, hi[3] = {x, y, z};
		lo[axis] = std::max(lo[axis] - 1, 0);
		hi[axis] = std::min(hi[axis] + 1, kBrickVoxels);
		const float span = static_cast<float>(hi[axis] - lo[axis]) * kVoxelSize;
		return (lat(hi[0], hi[1], hi[2]) - lat(lo[0], lo[1], lo[2])) / span;
	};
	// A hit point rounds to the cell containing it, so only cells within about a voxel of
	// the surface are ever read; project a little beyond that for margin.
	const float project_range = 2.0f * kVoxelSize;
	for (int z = 0; z < kBrickVoxels; z++)
		for (int y = 0; y < kBrickVoxels; y++)
			for (int x = 0; x < kBrickVoxels; x++) {
				const int i = voxel_index(x, y, z);
				if (mat[i] != 0) continue;
				const float d = lat(x, y, z);
				if (d > project_range) continue; // too far from any surface to be shaded
				const float gx = slope(x, y, z, 0);
				const float gy = slope(x, y, z, 1);
				const float gz = slope(x, y, z, 2);
				const float len = std::sqrt(gx * gx + gy * gy + gz * gz);
				if (len <= 0.0f) continue;
				// Step the cell's own distance to the surface, plus half a voxel to land
				// inside the solid, where a material was sampled. The stored SDF is
				// uint8-quantised and the gradient is a difference of those, so a single step
				// can still fall short of the surface; lengthen it and retry rather than let
				// the cell fall through to pass 2, which would answer from a lateral neighbour.
				for (float over = 0.5f; over <= 2.5f && mat[i] == 0; over += 1.0f) {
					const float t = d + over * kVoxelSize;
					mat[i] = gen.sample((bx * kBrickVoxels + x) * kVoxelSize - gx / len * t,
					                    (by * kBrickVoxels + y) * kVoxelSize - gy / len * t,
					                    (bz * kBrickVoxels + z) * kVoxelSize - gz / len * t).material;
				}
			}

	// Pass 2: cells too deep in air to be shaded still must not read as palette slot 0, so
	// give them the nearest seeded material. Multi-source BFS visits each cell once.
	int queue[kBrickVoxelCount];
	int head = 0, tail = 0;
	for (int i = 0; i < kBrickVoxelCount; i++)
		if (mat[i] != 0) queue[tail++] = i;
	if (tail == 0) return; // no solid anywhere in this brick: nothing to spread
	static constexpr int kStep[6][3] = {{0,-1,0},{0,1,0},{-1,0,0},{1,0,0},{0,0,-1},{0,0,1}};
	while (head < tail) {
		const int i = queue[head++];
		const int x = i % kBrickVoxels;
		const int y = (i / kBrickVoxels) % kBrickVoxels;
		const int z = i / (kBrickVoxels * kBrickVoxels);
		for (const auto &s : kStep) {
			const int nx = x + s[0], ny = y + s[1], nz = z + s[2];
			if (nx < 0 || ny < 0 || nz < 0 ||
			    nx >= kBrickVoxels || ny >= kBrickVoxels || nz >= kBrickVoxels) continue;
			const int ni = voxel_index(nx, ny, nz);
			if (mat[ni] != 0) continue;
			mat[ni] = mat[i];
			queue[tail++] = ni;
		}
	}
}

} // namespace

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
				uint16_t mat[kBrickVoxelCount] = {};
				for (int vz = 0; vz < kBrickSdfStride; vz++)
					for (int vy = 0; vy < kBrickSdfStride; vy++)
						for (int vx = 0; vx < kBrickSdfStride; vx++) {
							const float wx = (bx * kBrickVoxels + vx) * kVoxelSize;
							const float wy = (by * kBrickVoxels + vy) * kVoxelSize;
							const float wz = (bz * kBrickVoxels + vz) * kVoxelSize;
							const Sample s = gen.sample(wx, wy, wz);
							b.sdf[sdf_index(vx, vy, vz)] = encode_sdf(s.sdf);
							if (s.material == 0) continue;
							// An apron sample seeds the cell the shader's clamp folds it into: a
							// brick whose surface crosses only inside its last slab has no solid
							// cell of its own, and would otherwise hold no material at all.
							const bool apron = vx == kBrickVoxels || vy == kBrickVoxels || vz == kBrickVoxels;
							const int ci = voxel_index(std::min(vx, kBrickVoxels - 1),
							                           std::min(vy, kBrickVoxels - 1),
							                           std::min(vz, kBrickVoxels - 1));
							if (!apron || mat[ci] == 0) mat[ci] = s.material; // a cell's own sample wins
						}
				spread_materials(mat, b, gen, bx, by, bz);
				for (int i = 0; i < kBrickVoxelCount; i++) {
					if (mat[i] == 0) continue;
					const int pslot = palette_slot(b.palette, mat[i], &overflow);
					set_mat_index(b, i, static_cast<uint8_t>(pslot));
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
