#pragma once
#include "generator/generator.h"
#include "world/region.h"
#include <cstdint>

namespace ve {

enum EditOpType : uint32_t {
	kOpSphereSubtract = 0,
	kOpSphereAdd = 1,
	kOpSpherePaint = 2,
};

// Exactly 32 bytes (spec §2: "~32B/op"). The GPU op pool stores two uvec4 per op and
// unpacks by hand, so no std430 struct-layout rule can silently disagree with this.
struct EditOp {
	uint32_t type = kOpSphereSubtract;
	uint32_t material = 0;
	float pos[3] = {0.0f, 0.0f, 0.0f};
	float radius = 0.0f;
	uint32_t pad[2] = {0, 0};
};
static_assert(sizeof(EditOp) == 32);

Sample apply_op(Sample s, const EditOp &op, float x, float y, float z);
Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z);

// Inclusive lattice ranges an op can change, padded by one voxel: a brick's SDF lattice
// carries a one-voxel apron on its positive faces, so an op that only reaches the apron
// plane still alters that brick's stored bytes.
void op_brick_range(const EditOp &op, IVec3 *lo, IVec3 *hi);
void op_region_range(const EditOp &op, IVec3 *lo, IVec3 *hi);

} // namespace ve
