#pragma once
#include "generator/edit_ops.h"
#include <cstdint>

namespace ve {

// The gate for clamp_brick_lattice (declared in world/brick.h). The distortion needs BOTH a
// carve to distort and two samples whose hardness differs to distort it. Air is a sample at
// baseline hardness 1.0, so a hard solid/air surface qualifies. Neither condition holds for
// unedited terrain, and the gate stays inert until a harder material is carved.
//
// The hardness range must cover the full 17^3 SDF lattice, including its positive apron;
// the 16^3 cell-material array alone cannot see a surface that crosses only on that apron.
bool lattice_needs_clamp(float min_hardness, float max_hardness,
		const EditOp *ops, int op_count);

} // namespace ve
