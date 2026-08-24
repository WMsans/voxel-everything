#pragma once
#include "generator/edit_ops.h"
#include <cstdint>

namespace ve {

// The gate for clamp_brick_lattice (declared in world/brick.h). The distortion needs BOTH a
// carve to distort and two materials whose hardness differs to distort it. Neither holds for
// unedited terrain, and neither holds anywhere until a material with hardness above 1.0 is
// in play -- so the clamp lands inert and is switched on by the hardness task.
//
// `mat` is the brick's per-cell material array as eval_brick builds it, BEFORE the palette
// exists; `count` is kBrickVoxelCount.
bool lattice_needs_clamp(const uint16_t *mat, int count, const EditOp *ops, int op_count);

} // namespace ve
