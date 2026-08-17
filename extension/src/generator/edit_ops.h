#pragma once
#include "generator/generator.h"
#include "world/region.h"
#include <cstdint>

namespace ve {

enum EditOpType : uint32_t {
	kOpSphereSubtract = 0,
	kOpSphereAdd = 1,
	kOpSpherePaint = 2,
	// An island's carve: subtract one axis-aligned box of 0.8 m occupancy cells. An island
	// IS a union of whole cells intersected with the solid field (see the plan's Deliberate
	// Decisions), so a handful of these removes exactly the material that fell away.
	kOpBoxSubtract = 3,
	// Rubble coming back: CSG-union a dense stored volume. This is spec §5's "stamped back
	// as a CSG paste-op". Volumes are always world-axis-aligned; the body's rotation is
	// spent once, resampling at rest, rather than carried in the op.
	kOpVolumeAdd = 4,
};

// Exactly 32 bytes (spec §2: "~32B/op"). The GPU op pool stores two uvec4 per op and unpacks
// by hand, so no std430 struct-layout rule can silently disagree with this.
//
// Field meanings are per type. `aux` is the two words that used to be padding:
//   sphere*:       pos = centre, radius = radius, material = material, aux unused
//   kOpBoxSubtract: pos = the box's minimum corner (always cell-aligned), aux[0] =
//                   pack_extent3(cells on x, y, z), aux[1] unused; radius carries an
//                   optional CLEARANCE MARGIN (0 for a plain op): the field evaluator
//                   expands the carved box by it, so the cell-aligned faces of an island
//                   carve read as air instead of the exact SDF == 0 a cell-boundary CSG
//                   difference otherwise leaves behind. See make_box_subtract.
//   kOpVolumeAdd:  pos = the lattice's world origin, radius = the voxel pitch,
//                  aux[0] = volume slot, aux[1] = lattice dimension, material unused
struct EditOp {
	uint32_t type = kOpSphereSubtract;
	uint32_t material = 0;
	float pos[3] = {0.0f, 0.0f, 0.0f};
	float radius = 0.0f;
	uint32_t aux[2] = {0, 0};
};
static_assert(sizeof(EditOp) == 32);

// One sample of a stored volume.
struct VolumeSample {
	float sdf = 0.0f;
	uint16_t material = 0;
};

// How the field evaluator reaches a stored volume. An interface, not a concrete type, so
// generator/ stays free of ownership questions and a test can pass a two-line stub.
// Implemented by ve::VolumeSet (generator/volume_set.h).
struct VolumeStore {
	virtual ~VolumeStore() = default;
	// False when the slot holds nothing: the op is then skipped entirely (fail-soft).
	virtual bool sample(int slot, float x, float y, float z, const EditOp &op,
			VolumeSample *out) const = 0;
	// True when the slot currently holds a live, sampleable volume. Raycast uses this
	// to decide whether a region's kOpVolumeAdd can actually contribute a field value,
	// because apply_op fail-softs a missing slot and leaves the field identical.
	virtual bool has(int slot) const = 0;
};

Sample apply_op(Sample s, const EditOp &op, float x, float y, float z,
		const VolumeStore *volumes = nullptr);
Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z,
		const VolumeStore *volumes = nullptr);

// 3 x 10 bits, values 1..1023 (0 is never a legal extent). An island cell box is at most 8
// cells on a side, so the range is enormous headroom; it exists because M5's LoD bakery will
// want box ops in world-scale units too.
uint32_t pack_extent3(int nx, int ny, int nz);
void unpack_extent3(uint32_t v, int *nx, int *ny, int *nz);

// Inclusive 0.8 m cells. `margin` (metres) expands the carved box ONLY inside the field
// evaluator: a cell-aligned CSG difference can force the field no further than SDF == 0
// at its faces, and every cell-aligned sampler (the 0.1 m chunk-mesh lattice, the 5 cm
// brick lattice, the 0.4 m occupancy probe) contains those planes and reads the exact 0
// as solid -- a razor-thin phantom wall standing inside the carved region, which a freshly
// freed island then wedges against. A small positive margin turns those faces into air.
// The op's world AABB deliberately does NOT see the margin: connectivity's cell-exact
// bookkeeping (component freshness, region ranges, re-mark coverage) is unchanged, and the
// existing pads (kActivationPad + kVoxelSize, or 2 mesh cells) already cover it.
EditOp make_box_subtract(IVec3 lo_cell, IVec3 hi_cell, float margin = 0.0f);
EditOp make_volume_add(int slot, const float origin[3], float voxel, int dim);

// The op's own world AABB, before any padding. For a sphere this is centre +/- radius; for
// a box the box; for a volume the lattice's extent, [origin, origin + (dim - 1) * voxel].
void op_world_aabb(const EditOp &op, float lo[3], float hi[3]);

// Exact signed distance to an axis-aligned box. Mirrored in shaders/field.glslh.
float box_sdf(const float lo[3], const float hi[3], float x, float y, float z);

// Inclusive lattice ranges an op can change, padded by one voxel: a brick's SDF lattice
// carries a one-voxel apron on its positive faces, so an op that only reaches the apron
// plane still alters that brick's stored bytes.
void op_brick_range(const EditOp &op, IVec3 *lo, IVec3 *hi);
void op_region_range(const EditOp &op, IVec3 *lo, IVec3 *hi);

} // namespace ve
