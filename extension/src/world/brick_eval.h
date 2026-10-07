#pragma once
#include "connectivity/occupancy.h"
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "world/brick.h"
#include "world/brick_mip.h"
#include "world/opaque_view.h"
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

// The same point as eval_field, from ONE pass over the op stack, in BOTH views: `s` is the
// union (bit for bit what eval_field returns) and `o` is the OPAQUE VIEW, in which a
// transparent material is air. Bake sites that store the opaque view (the brick lattice, its
// material projection and the residency probe) read `o`; everything else reads `s`. The
// caller still applies ve::opaque_view to `o` last, for a transparent material the BASE
// field itself named. GLSL mirror: eval_field_pair() in shaders/field_ops.glslh.
void eval_field_pair(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, Sample *s, Sample *opaque,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr);

// The SOLID VIEW of a point (docs/superpowers/specs/2026-10-06-water-voxels-design.md §3):
// the union with every liquid ADD/PAINT op and every liquid volume sample contributing
// nothing, then ve::solid_view for a liquid the base or an override named. Colliders,
// occupancy, contact refinement and island extraction read this; the edit raycast,
// consolidation and the near-shell candidate scan keep the union. With no liquid anywhere it
// IS the union, bit for bit. GLSL mirror: eval_field_solid() in shaders/field_ops.glslh.
Sample eval_field_solid(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr);

FieldSample eval_field_gradient(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr);

// Coarse residency probe. Mirrored exactly by shaders/brick_mark.comp.glsl — a brick is
// resident iff this returns true, on both sides. With `opaque`, a brick is ALSO resident when
// the opaque view (world/opaque_view.h) has a surface in it: ground under a transparent
// material has no union surface and would otherwise never be generated.
bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr,
		bool opaque = true);

// The exact occupancy classification written by brick_gen.comp.glsl: reduce the encoded
// signed-distance lattice produced for this brick. Never returns kCellUnknown -- the field
// always answers; only the GRID has a "nobody looked" state.
// Classified from the SOLID view: liquid is air to connectivity, ice is not.
CellState cell_state_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr);

// The conservative 3^3 activation-probe classification used by brick_mark for bricks it
// decides not to generate. Kept separate so the two consumers cannot silently diverge.
CellState cell_state_probe(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr);

// Full brick contents at L0. This is BOTH the path WorldData walks and the CPU reference
// the GPU differential test diffs against (spec §8). `opaque` stores the opaque view; the
// occupancy classification (cell_state_field) reads the solid view.
void eval_brick(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		BrickEval *out, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr, bool opaque = true);

} // namespace ve
