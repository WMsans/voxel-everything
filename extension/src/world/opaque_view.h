#pragma once
#include "world/brick.h"
#include "world/material_table.h"
#include <cstdint>

namespace ve {

// The OPAQUE VIEW (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §3): the
// world as the raymarcher sees it, in which a transparent material is air. One rule, applied
// wherever a lattice the marcher reads is baked: a SOLID sample whose material is
// transparent becomes "just outside" with no material. Physics, edits and connectivity keep
// seeing the union: they read eval_field / apply_op, never this.
//
// Half a sample pitch, not a true distance: the label boundary has no distance field. It is
// positive, so the surface the marcher finds sits within one voxel of the true boundary,
// and far below kActivationPad, so a brick holding any such sample stays resident.
inline constexpr float kOpaqueOutside = 0.5f * kVoxelSize;

// Mirror of shaders/opaque_view.glslh.
inline void opaque_view(float *sdf, uint16_t *material, float outside = kOpaqueOutside) {
	if (*sdf <= 0.0f && material_transparent(*material)) {
		*sdf = outside;
		*material = 0;
	}
}

} // namespace ve
