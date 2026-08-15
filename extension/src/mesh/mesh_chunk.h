#pragma once
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "world/region.h"

namespace ve {

// A collision chunk is 16 bricks (12.8 m) sampled at 0.1 m — half of L0's 5 cm, because
// "5cm collision is wasted on Jolt; half-res keeps walking smooth and halves triangles"
// (spec §6). 16 divides the region's 32, so a chunk lies inside exactly ONE region: the ops
// that can change any of its samples are exactly that region's op list, since EditLog
// appends an op to every region it touches. That is what lets a chunk be meshed with no
// neighbour walk, on the CPU or the GPU.
inline constexpr int kChunkBricks = 16;
inline constexpr float kChunkSize = kChunkBricks * kBrickSize;  // 12.8 m
inline constexpr float kChunkCellSize = 2.0f * kVoxelSize;      // 0.1 m
inline constexpr int kChunkCells = 128;                         // kChunkSize / kChunkCellSize

// The mesher works one cell BELOW the chunk's own origin on every axis, so that the quads on
// its minimum faces — whose four cells straddle the border — can be built from vertices this
// chunk owns. Mesh-cell array index m holds the cell at local coordinate m - 1; lattice array
// index i holds the sample at local coordinate i - 1; cell m's corners are lattice m and m+1.
inline constexpr int kChunkMeshCells = kChunkCells + 1;         // 129
inline constexpr int kChunkLattice = kChunkCells + 2;           // 130
inline constexpr int kChunkLatticeCount = kChunkLattice * kChunkLattice * kChunkLattice;
inline constexpr int kChunkCellCount = kChunkMeshCells * kChunkMeshCells * kChunkMeshCells;

// The activation probe samples (kChunkProbeSteps + 1)^3 points over the chunk.
inline constexpr int kChunkProbeSteps = 8;                      // 9^3 = 729 samples

IVec3 chunk_of_brick(IVec3 brick);
IVec3 chunk_of_point(float x, float y, float z);
IVec3 region_of_chunk(IVec3 chunk);
IVec3 chunk_min_brick(IVec3 chunk); // the chunk's lowest brick, for WorldBounds membership
void chunk_world_origin(IVec3 chunk, float out[3]);

// Distance from a point to the chunk's world AABB; 0 inside.
float chunk_distance(IVec3 chunk, float cx, float cy, float cz);

// Inclusive chunk range whose stored triangles an op can move. Only the sphere plus two mesh
// cells: a CSG max/min changes the field far outside its sphere, but only INSIDE the sphere
// can it flip a sample's sign, and a sample whose sign it cannot flip only shifts a vertex
// when it is itself within a cell of the surface — i.e. within a cell of the sphere. Two
// cells of pad covers that and the mesh overlap plane below the chunk origin.
void op_chunk_range(const EditOp &op, IVec3 *lo, IVec3 *hi);

// Conservative "this chunk may contain a surface". Unlike brick_has_surface's empirical
// 0.15 m pad, the margin here is DERIVED: with probe spacing s the farthest unsampled point
// is s·√3/2 away, and Generator::lipschitz() bounds how fast the reported distance can
// shrink, so a probe clearing s·√3/2·L on one side proves there is no crossing between
// probes. False positives cost one wasted mesh job; a false negative would leave a hole in
// the collision, so the test only pins the safe direction.
bool chunk_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 chunk);

} // namespace ve
