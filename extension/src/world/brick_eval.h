#pragma once
#include "connectivity/occupancy.h"
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "world/brick.h"
#include "world/brick_mip.h"
#include "world/region.h"

namespace ve {

// kActivationPad (the 3^3 probe's margin) lives in world/brick.h: ve::op_brick_range needs
// it too, and generator/ may not include world/brick_eval.h.

struct BrickEval {
	Brick brick;
	BrickMips mips;
};

struct OverrideSource;

// The world field: an override replaces the generator base, then the region ops apply in order.
Sample eval_field(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr);

FieldSample eval_field_gradient(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr);

// Coarse residency probe. Mirrored exactly by shaders/brick_mark.comp.glsl — a brick is
// resident iff this returns true, on both sides.
bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr);

// The exact occupancy classification written by brick_gen.comp.glsl: reduce the encoded
// signed-distance lattice produced for this brick. Never returns kCellUnknown -- the field
// always answers; only the GRID has a "nobody looked" state.
CellState cell_state_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr);

// The conservative 3^3 activation-probe classification used by brick_mark for bricks it
// decides not to generate. Kept separate so the two consumers cannot silently diverge.
CellState cell_state_probe(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr);

// Full brick contents at L0. This is BOTH the path WorldData walks and the CPU reference
// the GPU differential test diffs against (spec §8).
void eval_brick(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		BrickEval *out, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr);

} // namespace ve
