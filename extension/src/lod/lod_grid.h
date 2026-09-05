#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <vector>

namespace ve {

// Spec section 2: eight levels, ratio 2, 32 cells per chunk at EVERY level. Level 0 is the
// finest (the spec's L0); level 7 holds the octree roots. A ratio of 2 rather than the
// engine spec's 4 is the whole point: the screen-space error inside one level's band then
// varies 2:1 instead of 4:1, so geometry is close to the target error almost everywhere and
// every level change pops half as hard.
inline constexpr int kLodLevels = 8;
inline constexpr int kLodChunkCells = 32;
inline constexpr float kLodBaseCell = 0.4f;

// M3's mesher convention, at LoD dimensions: mesh-cell array index m holds the cell at local
// coordinate m - 1, lattice array index i holds the sample at local coordinate i - 1, and
// cell m's corners are lattice m and m + 1. The one-cell overlap below the origin lets a
// chunk close the quads on its minimum faces without reading a neighbour.
inline constexpr int kLodChunkMeshCells = kLodChunkCells + 1; // 33
inline constexpr int kLodChunkLattice = kLodChunkCells + 2;   // 34

// Spec section 4: the target lattice is built from HALF-cell samples reduced by a separable
// tent filter, so target index i needs fine indices 2i, 2i+1, 2i+2 and the fine array spans
// [0, 2 * kLodChunkLattice + 1). Fine sample j sits at local coordinate (j - 3) * cell/2,
// which puts j = 3 exactly on the chunk origin and j = 1 on lattice index 0.
inline constexpr int kLodFineLattice = 2 * kLodChunkLattice + 1; // 69

// The one statement of "how coarse is too coarse". A chunk is 32 cells across, so a chunk
// projecting to more than (32 * kLodTargetCellPx)^2 px^2 would render cells coarser than
// kLodTargetCellPx and must descend. Absolute px^2, so it needs no per-resolution tuning.
inline constexpr float kLodTargetCellPx = 3.0f;
inline constexpr float kLodSseAreaThresh =
		float(kLodChunkCells) * kLodTargetCellPx * float(kLodChunkCells) * kLodTargetCellPx;

// Spec §4's fade-band contingency: within this radius the walk forces level 0 even when the
// SSE test would accept a coarser level. The original spec chose 300 m for a 0.2 m denser
// band; with M5's 2x level table this is the existing finest level (0.4 m) kept dense
// through the measured near-field handover. 0 disables the override.
inline constexpr float kLodNearDenseRadiusM = 300.0f;

// Engine spec section 3's near/far band. A chunk whose FARTHEST corner is nearer than the
// fade start is discarded by the fragment shader on every pixel, so building it burns pages
// to draw nothing.
inline constexpr float kLodFadeStartM = 120.0f;
inline constexpr float kLodFadeEndM = 150.0f;

// ...but only when the near field can actually reach that far, which it usually cannot: the
// brick atlas, not the residency radius, is the binding pool (ve::RegionResidency), so the
// raymarcher's data commonly stops around 60 m. Every metre between where the near field
// runs out and where the far field is allowed to draw belongs to NEITHER field: the ray
// reads absent bricks as empty and returns sky, and lod.frag discards every fragment inside
// the fade start. lod_fade_band() moves the seam in to sit inside the measured radius so
// that gap cannot open. The quantisation is what stops a streaming wobble from sliding the
// seam -- and the LoD build gate with it -- every single frame.
inline constexpr float kLodSeamMarginM = 0.9f; // keep the whole band inside the complete radius
inline constexpr float kLodSeamStepM = 8.0f;   // seam granularity
inline constexpr float kLodFadeMinEndM = 32.0f; // never collapse the near field to nothing

// The seam for a near field whose data is complete out to `reach_m`. Returns the spec band
// unchanged once the near field can pay for it.
void lod_fade_band(float reach_m, float *fade_start, float *fade_end);

// Levels 5, 6 and 7 are never evicted: roughly 190 surface-intersecting chunks over the
// whole world, a few MB, and they are what makes turning the camera reveal coarse terrain
// instead of sky.
inline constexpr int kLodResidentLevelFrom = 5;

float lod_cell_size(int level);
float lod_chunk_size(int level);

IVec3 lod_chunk_of_point(int level, float x, float y, float z);
void lod_chunk_origin(int level, IVec3 c, float out[3]);
void lod_chunk_aabb(int level, IVec3 c, float lo[3], float hi[3]);

// The chunk at level+1 containing c, and the lowest of the eight children at level-1.
IVec3 lod_parent(IVec3 c);
IVec3 lod_child_base(IVec3 c);

// Distance from a point to the chunk's AABB (0 inside), and to its farthest corner.
float lod_chunk_distance(int level, IVec3 c, const float p[3]);
float lod_chunk_far_distance(int level, IVec3 c, const float p[3]);

// The root-level chunks intersecting a sphere of `radius_m` around the camera. This is what
// replaced the world AABB: an unbounded world has no edge to enumerate from, so the forest of
// octree roots follows the camera. Air roots cost one build each to discover and then prune
// their whole subtree (LodTree::visit treats kLodEmpty as terminal), so a generous radius is
// paid for in one-time builds, not per frame.
void lod_roots_in_radius(const float cam_pos[3], float radius_m, std::vector<IVec3> *out);

// Inclusive chunk range whose stored quads an op can move. LoD field consumers use the
// shared lattice pad (or the larger two-cell LoD overlap pad) so narrow-band influence is not
// lost at a chunk boundary. (Same conservative argument as ve::op_chunk_range.)
void op_lod_chunk_range(const EditOp &op, int level, IVec3 *lo, IVec3 *hi);

} // namespace ve
