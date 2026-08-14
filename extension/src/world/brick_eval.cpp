#include "world/brick_eval.h"
#include "world/palette.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

// Give every cell within reach of a surface hit the material of that surface.
//
// A ray's hit point routinely lands on the AIR side of the surface: the march stops at
// d < 0.002 and the secant refinement can leave p just outside, so the material lookup
// rounds to an air cell. Projecting each near-surface air cell onto the surface along
// -grad(SDF) and asking the FIELD for the material there removes the failure mode.
//
// Two reasons not to copy from a neighbouring cell instead: the closest surface point is
// what a ray hitting near this cell would shade, which on a slope is not the L1-nearest
// solid cell; and that surface often lies in the next brick along -- a cell on this brick's
// bottom plane belongs to the surface below it, whose material may appear nowhere in this
// brick at all. The field has no such boundary, so it answers correctly in both cases.
//
// Cells further than project_range from any surface are left at 0 and therefore resolve to
// palette slot 0, which palette_occupancy_order() guarantees is the brick's dominant
// material. No flood fill is needed, and the GPU can run this pass thread-per-cell.
void spread_materials(uint16_t *mat, const Brick &b, const Generator &gen,
		const EditOp *ops, int op_count, const float bo[3]) {
	auto lat = [&b](int x, int y, int z) { return decode_sdf(b.sdf[sdf_index(x, y, z)]); };
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
				// uint8-quantised and the gradient is a difference of those, so a single
				// step can still fall short; lengthen it and retry.
				for (float over = 0.5f; over <= 2.5f && mat[i] == 0; over += 1.0f) {
					const float t = d + over * kVoxelSize;
					mat[i] = eval_field(gen, ops, op_count,
							bo[0] + x * kVoxelSize - gx / len * t,
							bo[1] + y * kVoxelSize - gy / len * t,
							bo[2] + z * kVoxelSize - gz / len * t).material;
				}
			}
}

} // namespace

Sample eval_field(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z) {
	return apply_ops(gen.sample(x, y, z), ops, op_count, x, y, z);
}

bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick) {
	float bo[3];
	brick_world_origin(brick, bo);
	float mn = 1e30f, mx = -1e30f;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				const float d = eval_field(gen, ops, op_count,
						bo[0] + sx * (kBrickVoxels / 2) * kVoxelSize,
						bo[1] + sy * (kBrickVoxels / 2) * kVoxelSize,
						bo[2] + sz * (kBrickVoxels / 2) * kVoxelSize).sdf;
				mn = std::min(mn, d);
				mx = std::max(mx, d);
			}
	return mn < kActivationPad && mx > -kActivationPad;
}

void eval_brick(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		BrickEval *out) {
	*out = BrickEval{};
	Brick &b = out->brick;
	float bo[3];
	brick_world_origin(brick, bo);

	// The SDF runs over the 17^3 lattice (see kBrickSdfStride): the extra plane at local 16
	// on each axis is the apron the shader's trilinear filter needs to cover the brick's
	// last voxel slab. Materials stay on the 16^3 cell grid.
	uint16_t mat[kBrickVoxelCount] = {};
	for (int vz = 0; vz < kBrickSdfStride; vz++)
		for (int vy = 0; vy < kBrickSdfStride; vy++)
			for (int vx = 0; vx < kBrickSdfStride; vx++) {
				const Sample s = eval_field(gen, ops, op_count, bo[0] + vx * kVoxelSize,
						bo[1] + vy * kVoxelSize, bo[2] + vz * kVoxelSize);
				b.sdf[sdf_index(vx, vy, vz)] = encode_sdf(s.sdf);
				if (s.material == 0) continue;
				// An apron sample seeds the cell the shader's clamp folds it into: a brick
				// whose surface crosses only inside its last slab has no solid cell of its
				// own, and would otherwise hold no material at all.
				const bool apron =
						vx == kBrickVoxels || vy == kBrickVoxels || vz == kBrickVoxels;
				const int ci = voxel_index(std::min(vx, kBrickVoxels - 1),
						std::min(vy, kBrickVoxels - 1), std::min(vz, kBrickVoxels - 1));
				if (!apron || mat[ci] == 0) mat[ci] = s.material; // a cell's own sample wins
			}

	spread_materials(mat, b, gen, ops, op_count, bo);

	uint16_t pal[kBrickPaletteSize] = {};
	int counts[kBrickPaletteSize] = {};
	uint8_t slot_of[kBrickVoxelCount];
	bool overflow = false;
	for (int i = 0; i < kBrickVoxelCount; i++) {
		if (mat[i] == 0) { slot_of[i] = 0xFF; continue; }
		const int s = palette_slot(pal, mat[i], &overflow);
		slot_of[i] = static_cast<uint8_t>(s);
		counts[s]++;
	}
	int order[kBrickPaletteSize] = {};
	palette_occupancy_order(pal, counts, order);
	int inverse[kBrickPaletteSize] = {};
	for (int k = 0; k < kBrickPaletteSize; k++) inverse[order[k]] = k;
	for (int k = 0; k < kBrickPaletteSize; k++) b.palette[k] = pal[order[k]];
	for (int i = 0; i < kBrickVoxelCount; i++)
		if (slot_of[i] != 0xFF)
			set_mat_index(b, i, static_cast<uint8_t>(inverse[slot_of[i]]));

	build_brick_mips(b.sdf, &out->mips);
}

} // namespace ve
