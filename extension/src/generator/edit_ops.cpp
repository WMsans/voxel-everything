#include "generator/edit_ops.h"
#include <cmath>

namespace ve {

namespace {

float sphere_sdf(const EditOp &op, float x, float y, float z) {
	const float dx = x - op.pos[0], dy = y - op.pos[1], dz = z - op.pos[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz) - op.radius;
}

// Inclusive [lo, hi] cell range of the op's padded AABB on a lattice of the given pitch.
//
// Two margins, and the op's own radius covers neither. kVoxelSize is the brick's apron: its
// SDF lattice reaches one voxel past its own extent (kBrickSdfStride == 17), so an op
// grazing that plane still changes the bytes the brick stores. kActivationPad is the
// activation probe's: a CSG difference is a max and a union is a min, so BOTH move the
// field outside the sphere itself — a point d metres beyond a carved sphere reads -d, and
// once d is under the pad, a brick that was solidly interior starts reporting a surface.
// Those bricks flip active or inactive exactly like the ones inside the sphere, and the
// streamer re-marks nothing but this range, so leaving them out means the GPU and the CPU
// disagree about whether they hold an atlas slot with nothing to ever settle it.
void padded_range(const EditOp &op, float pitch, IVec3 *lo, IVec3 *hi) {
	const float r = op.radius + kActivationPad + kVoxelSize;
	const auto cell = [pitch](float v) { return static_cast<int>(std::floor(v / pitch)); };
	*lo = {cell(op.pos[0] - r), cell(op.pos[1] - r), cell(op.pos[2] - r)};
	*hi = {cell(op.pos[0] + r), cell(op.pos[1] + r), cell(op.pos[2] + r)};
}

} // namespace

Sample apply_op(Sample s, const EditOp &op, float x, float y, float z) {
	const float sp = sphere_sdf(op, x, y, z);
	switch (op.type) {
		case kOpSphereSubtract:
			// CSG subtract: max(s, -sphere). A point that becomes air carries no material,
			// matching the generator's own convention (Sample::material == 0 above ground).
			if (-sp > s.sdf) {
				s.sdf = -sp;
				if (s.sdf > 0.0f) s.material = 0;
			}
			return s;
		case kOpSphereAdd:
			// CSG union: min(s, sphere). The material changes only where the sphere is the
			// winning term and the result is solid — filling air, not recolouring rock.
			if (sp < s.sdf) {
				s.sdf = sp;
				if (s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			}
			return s;
		case kOpSpherePaint:
			if (sp <= 0.0f && s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			return s;
		default:
			return s;
	}
}

Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z) {
	for (int i = 0; i < count; i++) s = apply_op(s, ops[i], x, y, z);
	return s;
}

void op_brick_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	padded_range(op, kBrickSize, lo, hi);
}

void op_region_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	padded_range(op, kRegionSize, lo, hi);
}

} // namespace ve
