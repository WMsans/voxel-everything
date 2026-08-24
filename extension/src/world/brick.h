#pragma once
#include <cstdint>

namespace ve {

inline constexpr int kBrickVoxels = 16;
inline constexpr float kVoxelSize = 0.05f;
inline constexpr float kBrickSize = kBrickVoxels * kVoxelSize; // 0.8m
inline constexpr int kBrickVoxelCount = kBrickVoxels * kBrickVoxels * kBrickVoxels; // 4096
inline constexpr int kBrickPaletteSize = 4;
inline constexpr float kSdfRange = 0.64f; // uint8 maps to [-0.64, +0.64] meters

// The SDF is a LATTICE field, not a cell field: sample n sits at local coordinate n, so
// trilinear reconstruction over a brick's full [0, 16) voxel extent needs a lattice point
// at 16 as well -- that point belongs to the +X/+Y/+Z neighbour's origin plane. Bricks
// therefore carry a one-voxel apron on their positive faces (17^3 samples). Without it the
// last slab of every brick clamps to a constant, the gradient collapses, and calc_normal()
// returns garbage -> dark seams along every brick face. The apron is generated directly
// from the generator, so it is correct even when the neighbouring brick is inactive.
inline constexpr int kBrickSdfStride = kBrickVoxels + 1; // 17
inline constexpr int kBrickSdfCount = kBrickSdfStride * kBrickSdfStride * kBrickSdfStride; // 4913

// Conservative pad for the 3^3 activation probe: the probe samples every 8 voxels, so the
// field can dip across zero between samples. A brick is treated as empty only when all 27
// probes agree AND clear zero by this margin. shaders/brick_mark.comp.glsl mirrors it as
// ACTIVATION_PAD, and ve::op_brick_range widens an edit's re-mark range by it — a CSG
// union or difference moves the field by up to the pad well outside the sphere it carves,
// so bricks that far out can flip active or inactive and must be re-marked with the rest.
// It lives here, at the bottom of the include graph, because the generator side (edit_ops)
// and the world side (brick_eval) both need it and neither may include the other.
inline constexpr float kActivationPad = 0.15f;
// Brick residency and generation use the probe's activation margin plus the positive lattice
// apron. This is the only consumer pad that is 0.20 m.
inline constexpr float kBrickFilterPad = kActivationPad + kVoxelSize;
// Mesh, LoD, and stored-volume consumers evaluate a lattice whose representable narrow band
// reaches kSdfRange beyond an op's own AABB. Include one 5 cm lattice pitch as well.
inline constexpr float kLatticeFilterPad = kVoxelSize + kSdfRange; // 0.69 m

uint8_t encode_sdf(float d);
float decode_sdf(uint8_t v);

// x + y*16 + z*256; all coords in [0,16). Material/cell indexing.
int voxel_index(int x, int y, int z);

// x + y*17 + z*289; all coords in [0,17). SDF lattice indexing (includes the apron).
int sdf_index(int x, int y, int z);

struct Brick {
	uint8_t sdf[kBrickSdfCount];            // encoded SDF lattice, 17^3 with +face apron
	uint8_t mat[kBrickVoxelCount / 4];      // 2-bit palette indices, packed
	uint16_t palette[kBrickPaletteSize];    // global material IDs
	uint32_t flags = 0;
};

uint8_t get_mat_index(const Brick &b, int idx);
void set_mat_index(Brick &b, int idx, uint8_t v);

// Per-point hardness makes a carve's radius vary with the material at the sample point.
// That keeps the field's SIGN exactly right, so meshing, occupancy and collision are
// unaffected -- but it destroys the MAGNITUDE as a distance bound at a material seam: the
// field reports the distance to the soft material's crater wall while the barely-carved
// hard lip stands much closer, and shaders/raymarch.comp.glsl steps t += max(d * 0.9, ...)
// straight through it.
//
// clamp_brick_lattice restores the property the tracer needs: after it, no two adjacent
// lattice samples differ by more than one voxel pitch. It only ever shrinks magnitudes and
// never flips a sign, so occupancy classification (which compares against encoded zero) is
// untouched. It works in the ENCODED uint8 space because shaders/brick_gen.comp.glsl
// mirrors it against an r8 image, and the two must agree byte for byte.
//
// The relaxation runs as snapshot-Jacobi: every pass computes each sample's new value from
// the previous pass's frozen state, so the result does not depend on the visiting order.
// This matters because shaders/brick_gen.comp.glsl mirrors the clamp with 256 threads per
// brick -- a Gauss-Seidel sweep here would be one arbitrary interleaving that no GPU
// schedule can reproduce (cross-sign coupling makes the fixed point genuinely
// order-dependent; see the Task 7 review). It still converges within kClampIterations
// passes for any input: magnitudes only ever shrink, and Jacobi merely delays each
// sample's pull by one pass compared to Gauss-Seidel.
void clamp_brick_lattice(uint8_t *sdf);

// Iterations to convergence. A magnitude can travel at most kSdfRange before it saturates,
// in steps of kVoxelSize, so ceil(0.64 / 0.05) = 13 passes suffice from any input -- but
// Jacobi reaches each sample one pass later than an in-place sweep would, and the Task 8
// probe measured 13 passes to a fixed point on the worst inputs it could construct
// (too-steep ramps, a quantised carve). The shipped count is that observed worst case
// doubled. shaders/common.glslh mirrors this as CLAMP_ITERATIONS; the two must stay equal.
inline constexpr int kClampIterations = 26;

} // namespace ve
