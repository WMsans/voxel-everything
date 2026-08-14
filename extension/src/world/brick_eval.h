#pragma once
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "world/brick.h"
#include "world/brick_mip.h"
#include "world/region.h"

namespace ve {

// Conservative pad for the 3^3 activation probe: the probe samples every 8 voxels, so the
// field can dip across zero between samples. A brick is treated as empty only when all 27
// probes agree AND clear zero by this margin. The GPU mark pass uses the same constant.
inline constexpr float kActivationPad = 0.15f;

struct BrickEval {
	Brick brick;
	BrickMips mips;
};

// The world field: the generator with this point's region ops applied in order (spec §2).
Sample eval_field(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z);

// Coarse residency probe. Mirrored exactly by shaders/brick_mark.comp.glsl — a brick is
// resident iff this returns true, on both sides.
bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick);

// Full brick contents at L0. This is BOTH the path WorldData walks and the CPU reference
// the GPU differential test diffs against (spec §8).
void eval_brick(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		BrickEval *out);

} // namespace ve
