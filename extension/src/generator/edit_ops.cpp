#include "generator/edit_ops.h"
#include "connectivity/occupancy.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

float sphere_sdf(const EditOp &op, float x, float y, float z) {
	const float dx = x - op.pos[0], dy = y - op.pos[1], dz = z - op.pos[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz) - op.radius;
}

// Inclusive [lo, hi] cell range of the op's padded AABB on a lattice of the given pitch.
//
// Two margins, and the op's own extent covers neither. kVoxelSize is the brick's apron: its
// SDF lattice reaches one voxel past its own extent (kBrickSdfStride == 17), so an op
// grazing that plane still changes the bytes the brick stores. kActivationPad is the
// activation probe's: a CSG difference is a max and a union is a min, so BOTH move the
// field outside the shape itself -- a point d metres beyond a carved box reads -d, and once
// d is under the pad, a brick that was solidly interior starts reporting a surface. Those
// bricks flip active or inactive exactly like the ones inside the shape, and the streamer
// re-marks nothing but this range, so leaving them out means the GPU and the CPU disagree
// about whether they hold an atlas slot with nothing to ever settle it.
void padded_range(const EditOp &op, float pitch, IVec3 *lo, IVec3 *hi) {
	float a[3], b[3];
	op_world_aabb(op, a, b);
	const float pad = kActivationPad + kVoxelSize;
	const auto cell = [pitch](float v) { return static_cast<int>(std::floor(v / pitch)); };
	*lo = {cell(a[0] - pad), cell(a[1] - pad), cell(a[2] - pad)};
	*hi = {cell(b[0] + pad), cell(b[1] + pad), cell(b[2] + pad)};
}

} // namespace

uint32_t pack_extent3(int nx, int ny, int nz) {
	const auto c = [](int v) { return static_cast<uint32_t>(v < 1 ? 1 : (v > 1023 ? 1023 : v)); };
	return c(nx) | (c(ny) << 10) | (c(nz) << 20);
}

void unpack_extent3(uint32_t v, int *nx, int *ny, int *nz) {
	*nx = static_cast<int>(v & 0x3FFu);
	*ny = static_cast<int>((v >> 10) & 0x3FFu);
	*nz = static_cast<int>((v >> 20) & 0x3FFu);
}

EditOp make_box_subtract(IVec3 lo_cell, IVec3 hi_cell) {
	EditOp op{};
	op.type = kOpBoxSubtract;
	op.pos[0] = static_cast<float>(lo_cell.x) * kOccupancyCellSize;
	op.pos[1] = static_cast<float>(lo_cell.y) * kOccupancyCellSize;
	op.pos[2] = static_cast<float>(lo_cell.z) * kOccupancyCellSize;
	op.aux[0] = pack_extent3(hi_cell.x - lo_cell.x + 1, hi_cell.y - lo_cell.y + 1,
			hi_cell.z - lo_cell.z + 1);
	return op;
}

EditOp make_volume_add(int slot, const float origin[3], float voxel, int dim) {
	EditOp op{};
	op.type = kOpVolumeAdd;
	op.pos[0] = origin[0];
	op.pos[1] = origin[1];
	op.pos[2] = origin[2];
	op.radius = voxel;
	op.aux[0] = static_cast<uint32_t>(slot < 0 ? 0 : slot);
	op.aux[1] = static_cast<uint32_t>(dim < 1 ? 1 : dim);
	return op;
}

void op_world_aabb(const EditOp &op, float lo[3], float hi[3]) {
	switch (op.type) {
		case kOpBoxSubtract: {
			int n[3] = {1, 1, 1};
			unpack_extent3(op.aux[0], &n[0], &n[1], &n[2]);
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a];
				hi[a] = op.pos[a] + static_cast<float>(n[a]) * kOccupancyCellSize;
			}
			return;
		}
		case kOpVolumeAdd: {
			const float span = static_cast<float>(static_cast<int>(op.aux[1]) - 1) * op.radius;
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a];
				hi[a] = op.pos[a] + span;
			}
			return;
		}
		default:
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a] - op.radius;
				hi[a] = op.pos[a] + op.radius;
			}
			return;
	}
}

float box_sdf(const float lo[3], const float hi[3], float x, float y, float z) {
	const float p[3] = {x, y, z};
	float q[3];
	for (int a = 0; a < 3; a++) {
		const float c = 0.5f * (lo[a] + hi[a]);
		const float h = 0.5f * (hi[a] - lo[a]);
		q[a] = std::fabs(p[a] - c) - h;
	}
	const float outside = std::sqrt(std::max(q[0], 0.0f) * std::max(q[0], 0.0f) +
			std::max(q[1], 0.0f) * std::max(q[1], 0.0f) +
			std::max(q[2], 0.0f) * std::max(q[2], 0.0f));
	const float inside = std::min(std::max(q[0], std::max(q[1], q[2])), 0.0f);
	return outside + inside;
}

Sample apply_op(Sample s, const EditOp &op, float x, float y, float z,
		const VolumeStore *volumes) {
	switch (op.type) {
		case kOpSphereSubtract: {
			// CSG subtract: max(s, -sphere). A point that becomes air carries no material,
			// matching the generator's own convention (Sample::material == 0 above ground).
			const float sp = sphere_sdf(op, x, y, z);
			if (-sp > s.sdf) {
				s.sdf = -sp;
				if (s.sdf > 0.0f) s.material = 0;
			}
			return s;
		}
		case kOpSphereAdd: {
			// CSG union: min(s, sphere). The material changes only where the sphere is the
			// winning term and the result is solid — filling air, not recolouring rock.
			const float sp = sphere_sdf(op, x, y, z);
			if (sp < s.sdf) {
				s.sdf = sp;
				if (s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			}
			return s;
		}
		case kOpSpherePaint: {
			const float sp = sphere_sdf(op, x, y, z);
			if (sp <= 0.0f && s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			return s;
		}
		case kOpBoxSubtract: {
			float lo[3], hi[3];
			op_world_aabb(op, lo, hi);
			const float bd = box_sdf(lo, hi, x, y, z);
			if (-bd > s.sdf) {
				s.sdf = -bd;
				if (s.sdf > 0.0f) s.material = 0;
			}
			return s;
		}
		case kOpVolumeAdd: {
			VolumeSample vs{};
			// Fail-soft (spec §8): an op whose volume is gone contributes nothing at all,
			// rather than stamping undefined bytes into the terrain.
			if (!volumes || !volumes->sample(static_cast<int>(op.aux[0]), x, y, z, op, &vs))
				return s;
			if (vs.sdf < s.sdf) {
				s.sdf = vs.sdf;
				if (s.sdf <= 0.0f && vs.material != 0) s.material = vs.material;
			}
			return s;
		}
		default:
			return s;
	}
}

Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z,
		const VolumeStore *volumes) {
	for (int i = 0; i < count; i++) s = apply_op(s, ops[i], x, y, z, volumes);
	return s;
}

void op_brick_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	padded_range(op, kBrickSize, lo, hi);
}

void op_region_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	padded_range(op, kRegionSize, lo, hi);
}

} // namespace ve
