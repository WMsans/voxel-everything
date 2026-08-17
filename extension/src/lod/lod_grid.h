#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"

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

// Engine spec section 3's near/far band. A chunk whose FARTHEST corner is nearer than the
// fade start is discarded by the fragment shader on every pixel, so building it burns pages
// to draw nothing.
inline constexpr float kLodFadeStartM = 120.0f;
inline constexpr float kLodFadeEndM = 150.0f;

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

bool lod_chunk_in_bounds(const WorldBounds &b, int level, IVec3 c);
// Inclusive root-level chunk range covering the whole world.
void lod_root_range(const WorldBounds &b, IVec3 *lo, IVec3 *hi);

// Inclusive chunk range whose stored quads an op can move: the op's own world AABB plus two
// cells. A CSG max/min changes the field far outside the shape, but only inside it can it
// flip a sample's sign, and a sample whose sign it cannot flip only shifts a vertex when it
// is itself within a cell of the surface. Two cells covers that and the mesh overlap plane
// below the chunk origin. (Same argument as ve::op_chunk_range at 0.1 m.)
void op_lod_chunk_range(const EditOp &op, int level, IVec3 *lo, IVec3 *hi);

} // namespace ve
