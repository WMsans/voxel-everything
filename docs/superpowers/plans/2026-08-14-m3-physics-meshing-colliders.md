# M3 Physics: Meshing, Collider Streaming & Character — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Walk on the GPU-generated terrain: dual-contoured collision meshes are built on the GPU at 0.1 m from the same `G + edit ops` field the bricks come from, read back asynchronously, streamed into `PhysicsServer3D` as Jolt concave shapes in a 64 m ball around the player, rebuilt within two frames of every edit — and a `CharacterBody3D` capsule that walks, jumps and gets blown around by the destruction tools.

**Architecture:** The mesher never touches the brick atlas. A collision chunk is 16 bricks (12.8 m), sampled at 0.1 m into a 130³ lattice by the **same `shaders/field.glsl`** the brick generator uses, then dual-contoured by two more compute passes (one dual vertex per cell, one quad per sign-changing edge). Chunks are 16 bricks so each lies inside exactly one region, which means one region's op list reconstructs it with no neighbour walk. The whole pipeline runs on a **second, local `RenderingDevice`** driven from `VoxelWorld::_process` with one batch in flight, so physics meshing can never lengthen the render thread's frame and the test path and the demo path are the same code. Residency (which chunks have colliders), dirtying and the distance-LRU are a pure C++ core mirroring `ve::RegionResidency`; the Godot glue is one class that turns a finished mesh into a static body.

**Tech Stack:** Godot 4.7.1 (`/usr/bin/godot`), godot-cpp (pinned master, `api_version = "4.7"`), SCons, C++20, GLSL 460 (Vulkan), Jolt Physics (already `project.godot`'s `3d/physics_engine`), doctest 2.4.11 (native), gdUnit4 6.2.1 (in-engine).

**Spec:** `docs/superpowers/specs/2026-08-12-voxel-engine-design.md` — M3 implements §6 in full, plus §8's *"CPU references for … meshing; dev console command runs both and diffs"* bullet and the mesher half of §8's `mesh/` module.

**Predecessors:** `docs/superpowers/plans/2026-08-12-m1-walking-skeleton.md` and `docs/superpowers/plans/2026-08-13-m2-gpu-generation-streaming-edits.md` (both complete). **Read both Errata sections before touching shaders or `VoxelWorld`** — in particular M2 errata 5 (GLSL reserved words), 7 (`ivec4` → `.xyz`), 9 (the `kSurfaceY = 51.2` offset in the field) and 11 (the eviction arm).

## Milestone Map

| Milestone | Delivers |
|---|---|
| M1 (done) | Toolchain + raymarched terrain on screen + test harnesses |
| M2 (done) | GPU brick generation, region indirection, streaming/residency + LRU, min–max mips, destruction edits |
| **M3 (this plan)** | Dual-contour collision meshing on the GPU, async readback, collider streaming into Jolt, character controller |
| M4 | Connectivity + raymarched islands |
| M5 | LoD hierarchy + bakery + depth-injection compositing |
| M6 | Beautification: cel, 3-layer shadows, SSGI, SSR, outlines |
| M7 | Benchmark scene + demo polish |

## Global Constraints

- Godot **4.7.1**; godot-cpp pinned to the existing submodule commit, `api_version = "4.7"` — do not bump either.
- Pure C++ cores in `namespace ve` contain **zero Godot types** (spec §8) — no exceptions. Godot-glue classes live in `namespace godot`. Spec §8's module table is binding: `mesh/` is pure C++, `physics/` is Godot glue. A pure core that "belongs" to physics (chunk residency) therefore lives in `mesh/`.
- Shaders: GLSL `#version 460`, loaded **from files** via `ve::load_shader_source` — never inline strings. `#[compute]` is stripped in C++ after load (M1 errata 6).
- Error policy (spec §8): dev = verbose/validation; release = fail-soft — a readback or shape-build failure logs, keeps the previous collider and retries next frame; a meshing anomaly warns and no-ops. **A stale collider beats a hole the player falls through** (spec §6, *Failure policy*).
- Commit style: conventional (`feat:`, `test:`, `build:`, `fix:`, `refactor:`).
- RD API reference: local copy at `docs/api/renderingdevice.md` — consult it before inventing signatures.
- Target hardware: RTX 4070 Laptop; budgets per spec §7 (raymarch ≤ 6 ms, frame ≤ 16 ms). M3 adds no work to the render thread at all — see *Deliberate decisions* below.
- **Push constants must stay ≤ 128 bytes** (Vulkan's guaranteed minimum). M3's are 32.
- **Never put a literal include directive inside a GLSL comment** — the loader matches the token anywhere on a line (existing note at the top of `shaders/common.glsl`).
- `buffer_update` and `buffer_clear` are device-level commands: they must be recorded **before** `compute_list_begin`, never inside an open list (M2 Task 12's documented ordering).

## Fixed Numbers (decided here; every task assumes them)

| Thing | Value | Where it lives |
|---|---|---|
| Collision chunk | 16 bricks = **12.8 m** | `ve::kChunkBricks`, `ve::kChunkSize` |
| Collision sampling | **0.1 m** (half of L0's 5 cm, spec §6) | `ve::kChunkCellSize` |
| Cells per chunk axis | 128 | `ve::kChunkCells` |
| Cells the mesher evaluates | 129 (one overlap cell below the origin) | `ve::kChunkMeshCells` |
| Lattice samples per axis | **130** (= 2 197 000 per chunk) | `ve::kChunkLattice` |
| Chunk activation probe | 9³ samples, margin `½·√3·spacing·lipschitz()` ≈ 2.77 m | `ve::kChunkProbeSteps` |
| Vertex cap per chunk | 65 536 (a fully covered chunk holds ~16 400) | `MeshPassConfig::max_verts` |
| Triangle cap per chunk | 65 536 (a fully covered chunk holds 32 768) | `MeshPassConfig::max_tris` |
| Mesh jobs per batch | 2 | `MeshPassConfig::max_jobs`, `VoxelWorld::mesh_jobs_per_frame` |
| Shape builds per frame | 2 | `VoxelWorld::shape_builds_per_frame` |
| Collider chunk pool | 160 | `VoxelWorld::max_collider_chunks` |
| Collision radius | **64 m** around the player (spec §6) | `VoxelWorld::physics_radius_m` |
| Chunk probes per frame | 64 | `ChunkResidencyConfig::max_probes_per_frame` |
| Evict hysteresis | ×1.15 of the radius | `ChunkResidencyConfig::evict_margin` |

Why 160 chunks and not spec §6's "~80": a 64 m ball covers π·64²/12.8² ≈ 78 chunk *columns*, and the demo's hills swing ±10 m, so the surface crosses one to three chunks in most columns — ~120–160 in practice. Each chunk is ≤ 32 768 triangles (a fully covered 12.8 m plane at 0.1 m), typically ~15 k, so the pool is ~2 M triangles worst case against spec §6's ~1 M estimate. It is an export; turn it down before the radius.

Mesh-device VRAM: lattice 2.2 MB + cell map 8.6 MB + vertices 1.6 MB + indices 1.6 MB + op pool 16 KB ≈ **14 MB**, independent of the 740 MB brick atlas.

## Deliberate Decisions (recorded, with the spec text they interpret)

- **The mesher reads the analytic field, not the brick atlas.** Spec §6 says "dual-contoured meshes from L0 bricks at 0.1 m". M3 evaluates `G + region ops` at 0.1 m instead of sampling the atlas, for three reasons: (1) spec §6 also requires "small bubbles around active bodies", and a body can fly far outside the 96 m residency ball, where no brick exists to sample; (2) an absent brick in the atlas is *sign-ambiguous* — the mark pass only records "no surface", not solid-vs-air — so a chunk touching one could not be meshed correctly; (3) when the atlas drops bricks under pressure (M2's fail-soft), collision would inherit the holes. The two sources agree to within the uint8 quantisation the bricks themselves store (≤ 2.5 mm), because both go through the same `shaders/field.glsl` and the same `quantise_sdf`. Spec §2's "evaluable at any resolution" is exactly the property being used.
- **A static body per chunk slot, not one body with many shapes.** Spec §6 says "Jolt concave shapes in a static compound". Jolt rebuilds a body's compound shape whenever a sub-shape changes, so one 160-shape body would rebuild the whole compound two or three times a second while streaming; and `body_remove_shape` renumbers the shapes after it, which would need a fix-up map. A pool of single-shape static bodies is operationally identical to Jolt's broadphase and O(1) to evict. Everything else the sentence asks for — server-direct, no scene-tree nodes, streamed — is honoured.
- **The mesher runs on its own local `RenderingDevice`.** Spec §6 asks for "GPU compute meshing + async double-buffered readback". A local device gives explicit `submit`/`sync`, so the batch submitted at the end of frame *N* is read at the top of frame *N+1* and the GPU is never waited on; it also means the gdUnit tests and the running demo take the *same* code path, which the M2 split between `debug_stream_frame` and the compositor did not. If profiling ever shows the sync costing frame time, the three dispatches move into the compositor's compute list and the readback switches to `buffer_get_data_async` — the shaders and buffers do not change.
- **Dual contouring with mass-point vertex placement.** The QEF term that preserves sharp features is not implemented: the field is a smooth SDF whose only creases come from CSG spheres, and spec §4 already calls the same family of mesher "surface nets". Recorded as a deferral, not an omission.

## Deliberate Deferrals (recorded, not forgotten)

- **Vertex-position compression on readback.** Positions go back as three float32 (12 B/vertex); quantising to the chunk AABB would halve it. At two chunks per frame the readback is ~1.2 MB/frame — not the bottleneck.
- **Physics materials per triangle** (footstep surface types). `ConcavePolygonShape3D` has no per-face material channel in Godot, and nothing in M1–M3 consumes it.
- **Bubbles around active bodies are supported but unused.** `ve::ChunkResidency::update` takes an array of centres and radii (spec §6); M3 passes one — the player. M4's islands and debris are the first callers to pass more.
- **Threaded shape building.** `shape_set_data` builds Jolt's BVH on the calling thread and `PhysicsServer3D` is not safe to call from a worker by default, so builds are throttled to two per frame instead. The HUD prints `build_ms` so the cost is visible.

## File Structure

```
extension/src/
  mesh/                                                        (pure C++, namespace ve)
    mesh_chunk.h/.cpp       chunk lattice math + chunk_has_surface        (Task 1)
    dual_contour.h/.cpp     DcGrid, MeshBuffer, ve::dual_contour          (Task 2)
    chunk_residency.h/.cpp  ChunkProbe, ChunkResidency, ChunkPlan         (Task 3)
  render/
    mesh_pass.h/.cpp        local-RD mesher: 3 dispatches, batch, readback (Tasks 4-6)
  physics/                                                     (Godot glue)
    collider_streamer.h/.cpp bodies, shapes, the per-frame tick           (Tasks 7-8)
  voxel_world.h/.cpp        MODIFIED: physics exports, _process, hooks    (Tasks 4-8)
shaders/
  mesh_common.glsl          chunk lattice addressing, shared by all three (Task 4)
  mesh_field.comp.glsl      130^3 lattice from G + region ops             (Task 4)
  mesh_cells.comp.glsl      one dual vertex per cell                      (Task 5)
  mesh_quads.comp.glsl      one quad per sign-changing lattice edge       (Task 5)
extension/tests/            doctest: test_mesh_chunk, test_dual_contour,
                            test_chunk_residency
tests/                      gdUnit: test_mesh_lattice.gd, test_mesh_diff.gd,
                            test_mesh_stream.gd, test_collider_stream.gd
demo/                       player.gd (NEW), fly_camera.gd (DELETED),
                            main.tscn / hud.gd / benchmark.gd / edit_tool.gd (MODIFIED)
extension/SConstruct        MODIFIED: mesh/ joins the native test build   (Task 1)
```

## Conventions Used Throughout

- **Build:** `./build.sh -j$(nproc)` (or `cd extension && scons -j$(nproc)`)
- **Native tests:** `cd extension && scons test`
- **gdUnit tests:** `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
- **Demo:** `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
- **Chunk coordinates are GLOBAL**, like brick coordinates: the world-space origin of chunk `c` is `c * kChunkSize`, no origin term. `WorldBounds` only decides membership.
- **The mesh overlap convention** (used by both the CPU mesher and all three shaders): mesh-cell array index `m` holds the cell whose local coordinate is `m - 1`, and lattice array index `i` holds the sample at local coordinate `i - 1`. Cell `m`'s eight corners are lattice `m` and `m+1` on each axis. A chunk therefore evaluates one cell *below* its own origin, which is what lets it close the quads on its minimum faces without reading a neighbour.
- **Edge ownership:** a chunk emits the quad for lattice edge `(axis, L)` iff `L - 1 ∈ [0, kChunkCells)` on all three axes. Every edge in the world is emitted by exactly one chunk — no cracks between chunks, no duplicated triangles.
- **Winding:** counter-clockwise seen from the air side (positive SDF), i.e. the triangle normal by the right-hand rule points into air. `backface_collision` stays `false`.
- gdUnit tests that await must declare the timeout argument: `func test_x(timeout := 10000) -> void:`.

---

### Task 1: `mesh/mesh_chunk` — the collision chunk lattice

The lattice every later task addresses: how big a chunk is, which region owns it, which chunks an edit touches, and the conservative probe that decides whether a chunk is worth meshing at all.

**Files:**
- Create: `extension/src/mesh/mesh_chunk.h`, `extension/src/mesh/mesh_chunk.cpp`
- Create: `extension/tests/test_mesh_chunk.cpp`
- Modify: `extension/SConstruct:16-19` (pure-source globs)

**Interfaces:**
- Consumes: `ve::IVec3`, `ve::WorldBounds`, `ve::floor_div`, `ve::kBrickSize`, `ve::kRegionBricks` (`world/region.h`); `ve::eval_field` (`world/brick_eval.h`); `ve::EditOp` (`generator/edit_ops.h`); `ve::Generator::lipschitz()` (`generator/generator.h`).
- Produces:
  - `ve::kChunkBricks = 16`, `ve::kChunkSize = 12.8f`, `ve::kChunkCellSize = 0.1f`, `ve::kChunkCells = 128`, `ve::kChunkMeshCells = 129`, `ve::kChunkLattice = 130`, `ve::kChunkLatticeCount`, `ve::kChunkCellCount`, `ve::kChunkProbeSteps = 8`
  - `ve::IVec3 chunk_of_brick(IVec3)`, `ve::IVec3 chunk_of_point(float,float,float)`, `ve::IVec3 region_of_chunk(IVec3)`, `ve::IVec3 chunk_min_brick(IVec3)`
  - `void chunk_world_origin(IVec3, float out[3])`
  - `float chunk_distance(IVec3, float cx, float cy, float cz)`
  - `void op_chunk_range(const EditOp &, IVec3 *lo, IVec3 *hi)`
  - `bool chunk_has_surface(const Generator &, const EditOp *, int, IVec3)`

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_mesh_chunk.cpp`:

```cpp
#include <doctest/doctest.h>
#include "mesh/mesh_chunk.h"
#include "generator/generator.h"
#include "world/brick_eval.h" // ve::eval_field, for the brute-force oracle below
#include <cmath>
#include <vector>

// A chunk must lie inside exactly one region: that is the whole reason the mesher can
// reconstruct it from a single op list (EditLog appends an op to every region it touches).
TEST_CASE("a chunk is 16 bricks and never straddles a region border") {
	CHECK(ve::kChunkBricks == 16);
	CHECK(ve::kChunkSize == doctest::Approx(12.8f));
	CHECK(ve::kChunkCells == 128);
	CHECK(ve::kChunkCellSize == doctest::Approx(0.1f));
	CHECK(ve::kChunkMeshCells == 129);
	CHECK(ve::kChunkLattice == 130);
	for (int cz = -3; cz < 3; cz++)
		for (int cy = -3; cy < 3; cy++)
			for (int cx = -3; cx < 3; cx++) {
				const ve::IVec3 c{cx, cy, cz};
				const ve::IVec3 r = ve::region_of_chunk(c);
				float o[3];
				ve::chunk_world_origin(c, o);
				for (int k = 0; k < 8; k++) {
					const float e = 0.01f, s = ve::kChunkSize - 0.01f;
					const float p[3] = {o[0] + ((k & 1) ? s : e), o[1] + ((k & 2) ? s : e),
							o[2] + ((k & 4) ? s : e)};
					CHECK(ve::WorldBounds::region_of_point(p[0], p[1], p[2]) == r);
				}
			}
}

TEST_CASE("chunk lookups floor on negative coordinates") {
	CHECK(ve::chunk_of_point(0.0f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::chunk_of_point(12.79f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::chunk_of_point(12.81f, 0.0f, 0.0f) == ve::IVec3{1, 0, 0});
	CHECK(ve::chunk_of_point(-0.01f, 0.0f, 0.0f) == ve::IVec3{-1, 0, 0});
	CHECK(ve::chunk_of_brick({-1, 0, 15}) == ve::IVec3{-1, 0, 0});
	CHECK(ve::chunk_of_brick({16, -16, 31}) == ve::IVec3{1, -1, 1});
	CHECK(ve::chunk_min_brick({2, -1, 0}) == ve::IVec3{32, -16, 0});
}

TEST_CASE("chunk_distance is zero inside and grows outside") {
	CHECK(ve::chunk_distance({0, 0, 0}, 1.0f, 1.0f, 1.0f) == doctest::Approx(0.0f));
	CHECK(ve::chunk_distance({0, 0, 0}, -10.0f, 1.0f, 1.0f) == doctest::Approx(10.0f));
	CHECK(ve::chunk_distance({1, 0, 0}, 0.0f, 0.0f, 0.0f) == doctest::Approx(ve::kChunkSize));
}

// An op must dirty every chunk whose STORED TRIANGLES it can move. Brute force: walk the
// chunks around the op and check that any chunk holding a lattice sample the op changes is
// inside the reported range.
TEST_CASE("op_chunk_range covers every chunk whose lattice the op changes") {
	ve::EditOp op;
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 20.0f; op.pos[1] = 51.0f; op.pos[2] = -3.0f;
	op.radius = 4.0f;
	ve::IVec3 lo{}, hi{};
	ve::op_chunk_range(op, &lo, &hi);

	const ve::IVec3 c0 = ve::chunk_of_point(op.pos[0], op.pos[1], op.pos[2]);
	for (int dz = -3; dz <= 3; dz++)
		for (int dy = -3; dy <= 3; dy++)
			for (int dx = -3; dx <= 3; dx++) {
				const ve::IVec3 c{c0.x + dx, c0.y + dy, c0.z + dz};
				float o[3];
				ve::chunk_world_origin(c, o);
				// Does any lattice sample of this chunk fall inside the op's sphere? Sample
				// the lattice coarsely (every 8th) plus its exact extremes: the sphere is
				// far wider than 8 cells, so nothing can hide between the probes.
				bool touched = false;
				for (int i = -1; i <= ve::kChunkCells && !touched; i += 8)
					for (int j = -1; j <= ve::kChunkCells && !touched; j += 8)
						for (int k = -1; k <= ve::kChunkCells && !touched; k += 8) {
							const float p[3] = {o[0] + i * ve::kChunkCellSize,
									o[1] + j * ve::kChunkCellSize, o[2] + k * ve::kChunkCellSize};
							const float d = std::sqrt((p[0] - op.pos[0]) * (p[0] - op.pos[0]) +
									(p[1] - op.pos[1]) * (p[1] - op.pos[1]) +
									(p[2] - op.pos[2]) * (p[2] - op.pos[2]));
							touched = d <= op.radius;
						}
				if (!touched) continue;
				CHECK(c.x >= lo.x); CHECK(c.y >= lo.y); CHECK(c.z >= lo.z);
				CHECK(c.x <= hi.x); CHECK(c.y <= hi.y); CHECK(c.z <= hi.z);
			}
}

// The probe may say "maybe" about an empty chunk (it only costs a wasted mesh job), but it
// may never say "no" about a chunk the mesher would find a surface in.
TEST_CASE("chunk_has_surface never misses a chunk that holds a zero crossing") {
	const ve::AnalyticGenerator gen;
	int surfaced = 0;
	for (int cz = -1; cz <= 2; cz++)
		for (int cy = 2; cy <= 6; cy++)   // world y 25.6 .. 89.6, the surface sits at ~51.2
			for (int cx = -1; cx <= 2; cx++) {
				const ve::IVec3 c{cx, cy, cz};
				float o[3];
				ve::chunk_world_origin(c, o);
				bool pos = false, neg = false;
				for (int i = 0; i <= 32; i++)
					for (int j = 0; j <= 32; j++)
						for (int k = 0; k <= 32; k++) {
							const float s = ve::eval_field(gen, nullptr, 0,
									o[0] + i * (ve::kChunkSize / 32.0f),
									o[1] + j * (ve::kChunkSize / 32.0f),
									o[2] + k * (ve::kChunkSize / 32.0f)).sdf;
							if (s <= 0.0f) neg = true; else pos = true;
						}
				if (neg && pos) {
					surfaced++;
					CHECK(ve::chunk_has_surface(gen, nullptr, 0, c));
				}
			}
	CHECK(surfaced > 4); // the sweep really did cross the surface
}

TEST_CASE("chunk_has_surface rejects open sky and deep rock") {
	const ve::AnalyticGenerator gen;
	CHECK_FALSE(ve::chunk_has_surface(gen, nullptr, 0, {0, 12, 0}));  // y 153.6 .. 166.4
	CHECK_FALSE(ve::chunk_has_surface(gen, nullptr, 0, {0, -2, 0}));  // y -25.6 .. -12.8
}

// A sphere-add in open sky creates surface where the generator has none, so the op list has
// to be part of the verdict.
TEST_CASE("chunk_has_surface sees ops, not just the generator") {
	const ve::AnalyticGenerator gen;
	const ve::IVec3 sky{0, 12, 0};
	CHECK_FALSE(ve::chunk_has_surface(gen, nullptr, 0, sky));
	ve::EditOp add;
	add.type = ve::kOpSphereAdd;
	add.material = 4;
	add.pos[0] = 6.4f; add.pos[1] = 12 * ve::kChunkSize + 6.4f; add.pos[2] = 6.4f;
	add.radius = 3.0f;
	CHECK(ve::chunk_has_surface(gen, &add, 1, sky));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test -j$(nproc)`
Expected: FAIL — `fatal error: mesh/mesh_chunk.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `extension/src/mesh/mesh_chunk.h`:

```cpp
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
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/mesh/mesh_chunk.cpp`:

```cpp
#include "mesh/mesh_chunk.h"
#include "world/brick_eval.h"
#include <algorithm>
#include <cmath>

namespace ve {

IVec3 chunk_of_brick(IVec3 b) {
	return {floor_div(b.x, kChunkBricks), floor_div(b.y, kChunkBricks),
			floor_div(b.z, kChunkBricks)};
}

IVec3 chunk_of_point(float x, float y, float z) {
	return {static_cast<int>(std::floor(x / kChunkSize)),
			static_cast<int>(std::floor(y / kChunkSize)),
			static_cast<int>(std::floor(z / kChunkSize))};
}

IVec3 region_of_chunk(IVec3 c) {
	constexpr int per = kRegionBricks / kChunkBricks; // 2 chunks per region on each axis
	return {floor_div(c.x, per), floor_div(c.y, per), floor_div(c.z, per)};
}

IVec3 chunk_min_brick(IVec3 c) {
	return {c.x * kChunkBricks, c.y * kChunkBricks, c.z * kChunkBricks};
}

void chunk_world_origin(IVec3 c, float out[3]) {
	out[0] = static_cast<float>(c.x) * kChunkSize;
	out[1] = static_cast<float>(c.y) * kChunkSize;
	out[2] = static_cast<float>(c.z) * kChunkSize;
}

float chunk_distance(IVec3 c, float cx, float cy, float cz) {
	float lo[3];
	chunk_world_origin(c, lo);
	const float p[3] = {cx, cy, cz};
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float over = std::max(0.0f, std::max(lo[a] - p[a], p[a] - (lo[a] + kChunkSize)));
		d2 += over * over;
	}
	return std::sqrt(d2);
}

void op_chunk_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	const float r = op.radius + 2.0f * kChunkCellSize;
	const auto cell = [](float v) { return static_cast<int>(std::floor(v / kChunkSize)); };
	*lo = {cell(op.pos[0] - r), cell(op.pos[1] - r), cell(op.pos[2] - r)};
	*hi = {cell(op.pos[0] + r), cell(op.pos[1] + r), cell(op.pos[2] + r)};
}

bool chunk_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 chunk) {
	float o[3];
	chunk_world_origin(chunk, o);
	const float step = kChunkSize / static_cast<float>(kChunkProbeSteps); // 1.6 m
	const float pad = 0.5f * std::sqrt(3.0f) * step * gen.lipschitz();    // ~2.77 m at L = 2
	float mn = 1e30f, mx = -1e30f;
	for (int sz = 0; sz <= kChunkProbeSteps; sz++)
		for (int sy = 0; sy <= kChunkProbeSteps; sy++)
			for (int sx = 0; sx <= kChunkProbeSteps; sx++) {
				const float d = eval_field(gen, ops, op_count, o[0] + sx * step,
						o[1] + sy * step, o[2] + sz * step).sdf;
				mn = std::min(mn, d);
				mx = std::max(mx, d);
			}
	return mn < pad && mx > -pad;
}

} // namespace ve
```

- [ ] **Step 5: Teach SConstruct about `mesh/`**

The native test binary links the pure cores only. Edit `extension/SConstruct` lines 16–19 so `mesh/` joins them (`physics/` never will — it is Godot glue):

```python
pure_sources = Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") + Glob("src/mesh/*.cpp")
for f in ["src/render/shader_loader.cpp", "src/render/camera_params.cpp"]:
    if os.path.exists(f):
        pure_sources.append(f)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd extension && scons test -j$(nproc)`
Expected: PASS — all M1/M2 cases plus the seven new ones.

- [ ] **Step 7: Verify the extension still builds**

Run: `./build.sh -j$(nproc)`
Expected: `==> Build OK`.

- [ ] **Step 8: Commit**

```bash
git add extension/src/mesh/mesh_chunk.h extension/src/mesh/mesh_chunk.cpp \
        extension/tests/test_mesh_chunk.cpp extension/SConstruct
git commit -m "feat: collision chunk lattice and conservative surface probe"
```

---

### Task 2: `mesh/dual_contour` — the CPU mesher and GPU reference

The mesher itself, in pure C++. It is both a usable implementation and the reference the GPU is diffed against in Task 5 (spec §8: "CPU references for brick-eval and meshing").

**Files:**
- Create: `extension/src/mesh/dual_contour.h`, `extension/src/mesh/dual_contour.cpp`
- Create: `extension/tests/test_dual_contour.cpp`

**Interfaces:**
- Consumes: `ve::decode_sdf`, `ve::encode_sdf` (`world/brick.h`); `ve::kChunkLattice`, `ve::kChunkCellSize` (`mesh/mesh_chunk.h`).
- Produces:
  - `struct ve::DcGrid { int lattice; float cell_size; float origin[3]; int cells() const; int owned() const; }`
  - `struct ve::MeshBuffer { std::vector<float> positions; std::vector<uint32_t> indices; std::vector<int32_t> cell_vertex; int vertex_count() const; int triangle_count() const; }`
  - `ve::DcGrid ve::chunk_dc_grid(IVec3 chunk)`
  - `int ve::dc_lattice_index(const DcGrid &, int x, int y, int z)`
  - `int ve::dc_cell_index(const DcGrid &, int x, int y, int z)`
  - `void ve::dual_contour(const uint8_t *lattice, const DcGrid &, MeshBuffer *out)`

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_dual_contour.cpp`:

```cpp
#include <doctest/doctest.h>
#include "mesh/dual_contour.h"
#include "world/brick.h"
#include <cmath>
#include <functional>
#include <vector>

// Small grids keep the native suite in milliseconds (spec §8). The 130^3 chunk grid is
// exercised by the GPU differential test, which has a GPU to do it on.
static ve::DcGrid small_grid(float ox = 0.0f, float oy = 0.0f, float oz = 0.0f) {
	ve::DcGrid g;
	g.lattice = 18;          // 17^3 cells, 16^3 owned edges
	g.cell_size = 0.1f;
	g.origin[0] = ox; g.origin[1] = oy; g.origin[2] = oz;
	return g;
}

// f is evaluated in WORLD space, exactly where the mesher believes the sample sits.
static std::vector<uint8_t> make_lattice(const ve::DcGrid &g,
		const std::function<float(float, float, float)> &f) {
	std::vector<uint8_t> out(static_cast<size_t>(g.lattice) * g.lattice * g.lattice);
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++)
				out[ve::dc_lattice_index(g, x, y, z)] = ve::encode_sdf(
						f(g.origin[0] + (x - 1) * g.cell_size,
						  g.origin[1] + (y - 1) * g.cell_size,
						  g.origin[2] + (z - 1) * g.cell_size));
	return out;
}

static void tri_normal(const ve::MeshBuffer &m, int t, float out[3]) {
	const uint32_t i0 = m.indices[t * 3 + 0], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
	const float *p = m.positions.data();
	const float a[3] = {p[i1 * 3 + 0] - p[i0 * 3 + 0], p[i1 * 3 + 1] - p[i0 * 3 + 1],
			p[i1 * 3 + 2] - p[i0 * 3 + 2]};
	const float b[3] = {p[i2 * 3 + 0] - p[i0 * 3 + 0], p[i2 * 3 + 1] - p[i0 * 3 + 1],
			p[i2 * 3 + 2] - p[i0 * 3 + 2]};
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

TEST_CASE("an all-air lattice and an all-solid lattice produce nothing") {
	const ve::DcGrid g = small_grid();
	ve::MeshBuffer m;
	ve::dual_contour(make_lattice(g, [](float, float, float) { return 1.0f; }).data(), g, &m);
	CHECK(m.vertex_count() == 0);
	CHECK(m.triangle_count() == 0);
	ve::dual_contour(make_lattice(g, [](float, float, float) { return -1.0f; }).data(), g, &m);
	CHECK(m.vertex_count() == 0);
	CHECK(m.triangle_count() == 0);
}

TEST_CASE("a flat plane gives one quad per owned column, wound towards the air") {
	const ve::DcGrid g = small_grid();
	// Solid below y = 0.85, air above. 0.85 is deliberately off-lattice (samples sit at
	// multiples of 0.1 from -0.1), so every crossing is a genuine interpolation.
	const auto lat = make_lattice(g, [](float, float y, float) { return y - 0.85f; });
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);

	CHECK(m.triangle_count() == 2 * g.owned() * g.owned());
	// The crossing sits half way between two lattice samples, so the only error is the
	// uint8 quantisation of those samples: ~2.5 mm, well inside a 0.1 m cell.
	for (int v = 0; v < m.vertex_count(); v++)
		CHECK(m.positions[v * 3 + 1] == doctest::Approx(0.85f).epsilon(0.02f));
	for (int t = 0; t < m.triangle_count(); t++) {
		float n[3];
		tri_normal(m, t, n);
		CHECK(n[1] > 0.0f); // counter-clockwise seen from the air side above
	}
}

TEST_CASE("a sphere meshes onto its own surface with outward winding") {
	const ve::DcGrid g = small_grid();
	const float c[3] = {0.75f, 0.75f, 0.75f};
	const float r = 0.5f;
	const auto lat = make_lattice(g, [&](float x, float y, float z) {
		return std::sqrt((x - c[0]) * (x - c[0]) + (y - c[1]) * (y - c[1]) +
				(z - c[2]) * (z - c[2])) - r;
	});
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);
	CHECK(m.vertex_count() > 100);

	for (int v = 0; v < m.vertex_count(); v++) {
		const float d = std::sqrt(
				(m.positions[v * 3 + 0] - c[0]) * (m.positions[v * 3 + 0] - c[0]) +
				(m.positions[v * 3 + 1] - c[1]) * (m.positions[v * 3 + 1] - c[1]) +
				(m.positions[v * 3 + 2] - c[2]) * (m.positions[v * 3 + 2] - c[2]));
		CHECK(std::fabs(d - r) < g.cell_size); // every vertex within one cell of the surface
	}
	for (int t = 0; t < m.triangle_count(); t++) {
		float n[3];
		tri_normal(m, t, n);
		const uint32_t i0 = m.indices[t * 3];
		const float out[3] = {m.positions[i0 * 3 + 0] - c[0], m.positions[i0 * 3 + 1] - c[1],
				m.positions[i0 * 3 + 2] - c[2]};
		CHECK(n[0] * out[0] + n[1] * out[1] + n[2] * out[2] > 0.0f);
	}
}

TEST_CASE("indices stay inside the vertex array and cell_vertex agrees with them") {
	const ve::DcGrid g = small_grid();
	const auto lat = make_lattice(g, [](float x, float y, float z) {
		return y - 0.85f - 0.2f * x - 0.1f * z; // a tilted plane: crossings on all three axes
	});
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);
	CHECK(m.triangle_count() > 0);
	for (uint32_t i : m.indices) CHECK(static_cast<int>(i) < m.vertex_count());
	CHECK(static_cast<int>(m.cell_vertex.size()) ==
			g.cells() * g.cells() * g.cells());
	int mapped = 0;
	for (int32_t v : m.cell_vertex)
		if (v >= 0) { CHECK(v < m.vertex_count()); mapped++; }
	CHECK(mapped == m.vertex_count()); // exactly one vertex per crossed cell
}

TEST_CASE("the world origin lands in the positions") {
	const ve::DcGrid g = small_grid(100.0f, 50.0f, -20.0f);
	const auto lat = make_lattice(g, [](float, float y, float) { return y - 50.85f; });
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);
	CHECK(m.vertex_count() > 0);
	CHECK(m.positions[0] >= 100.0f - g.cell_size);
	CHECK(m.positions[1] == doctest::Approx(50.85f).epsilon(0.001f));
	CHECK(m.positions[2] >= -20.0f - g.cell_size);
}

TEST_CASE("chunk_dc_grid describes the shipping chunk") {
	const ve::DcGrid g = ve::chunk_dc_grid({2, -1, 3});
	CHECK(g.lattice == ve::kChunkLattice);
	CHECK(g.cells() == ve::kChunkMeshCells);
	CHECK(g.owned() == ve::kChunkCells);
	CHECK(g.cell_size == doctest::Approx(ve::kChunkCellSize));
	CHECK(g.origin[0] == doctest::Approx(2 * ve::kChunkSize));
	CHECK(g.origin[1] == doctest::Approx(-1 * ve::kChunkSize));
	CHECK(g.origin[2] == doctest::Approx(3 * ve::kChunkSize));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test -j$(nproc)`
Expected: FAIL — `fatal error: mesh/dual_contour.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `extension/src/mesh/dual_contour.h`:

```cpp
#pragma once
#include "mesh/mesh_chunk.h"
#include <cstdint>
#include <vector>

namespace ve {

// The lattice a mesher run covers. `lattice` samples per axis means `lattice - 1` mesh cells
// and `lattice - 2` owned edge coordinates: array index m holds the cell at local coordinate
// m - 1, so the grid always carries one overlap cell below its origin (mesh_chunk.h).
// Parameterised rather than fixed at the chunk size so the native tests can run on a 18^3
// grid in microseconds — and so M5's LoD chunks can reuse the mesher at their own pitch.
struct DcGrid {
	int lattice = kChunkLattice;
	float cell_size = kChunkCellSize;
	float origin[3] = {0.0f, 0.0f, 0.0f};
	int cells() const { return lattice - 1; }
	int owned() const { return lattice - 2; }
};

DcGrid chunk_dc_grid(IVec3 chunk);

// x fastest, then y, then z — the layout shaders/mesh_*.comp.glsl writes.
int dc_lattice_index(const DcGrid &g, int x, int y, int z);
int dc_cell_index(const DcGrid &g, int x, int y, int z);

struct MeshBuffer {
	std::vector<float> positions;    // 3 per vertex, WORLD space
	std::vector<uint32_t> indices;   // 3 per triangle
	std::vector<int32_t> cell_vertex; // mesh cell -> vertex index, or -1
	int vertex_count() const { return static_cast<int>(positions.size() / 3); }
	int triangle_count() const { return static_cast<int>(indices.size() / 3); }
};

// Dual contouring with mass-point vertex placement (spec §6). One vertex per cell the surface
// crosses, at the average of that cell's edge crossings; one quad per sign-changing lattice
// edge, from the four cells around it. The QEF sharp-feature term is deliberately absent —
// see the plan's Deliberate Decisions.
//
// `lattice` holds g.lattice^3 ENCODED sdf bytes in dc_lattice_index order. Solid is
// decode_sdf(byte) <= 0, matching the generator's own `if (sdf <= 0) material = ...`.
void dual_contour(const uint8_t *lattice, const DcGrid &g, MeshBuffer *out);

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/mesh/dual_contour.cpp`. **The arithmetic here is mirrored verbatim by `shaders/mesh_cells.comp.glsl` and `shaders/mesh_quads.comp.glsl` in Task 5** — the corner table, the edge table, the accumulation order and the final position expression must all match, or the differential test in Task 5 fails on float drift rather than on a real bug:

```cpp
#include "mesh/dual_contour.h"
#include "world/brick.h"

namespace ve {

namespace {

// Cell corners, indexed by (x | y<<1 | z<<2). Mirrored as CORNER[8] in mesh_cells.comp.glsl.
constexpr int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
		{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};

// The cell's 12 edges as corner pairs: four along x, four along y, four along z. The ORDER
// matters — the vertex is a running sum over crossings, and float addition is not
// associative. Mirrored as EDGE[12] in mesh_cells.comp.glsl.
constexpr int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
		{0, 2}, {1, 3}, {4, 6}, {5, 7},
		{0, 4}, {1, 5}, {2, 6}, {3, 7}};

// The four cells around a lattice edge, as offsets in the two axes perpendicular to it,
// wound counter-clockwise seen from +axis. Mirrored as QUAD[4] in mesh_quads.comp.glsl.
constexpr int kQuad[4][2] = {{-1, -1}, {0, -1}, {0, 0}, {-1, 0}};

} // namespace

DcGrid chunk_dc_grid(IVec3 chunk) {
	DcGrid g;
	g.lattice = kChunkLattice;
	g.cell_size = kChunkCellSize;
	chunk_world_origin(chunk, g.origin);
	return g;
}

int dc_lattice_index(const DcGrid &g, int x, int y, int z) {
	return x + y * g.lattice + z * g.lattice * g.lattice;
}

int dc_cell_index(const DcGrid &g, int x, int y, int z) {
	return x + y * g.cells() + z * g.cells() * g.cells();
}

void dual_contour(const uint8_t *lattice, const DcGrid &g, MeshBuffer *out) {
	const int cells = g.cells();
	out->positions.clear();
	out->indices.clear();
	out->cell_vertex.assign(static_cast<size_t>(cells) * cells * cells, -1);

	// Pass 1: one dual vertex per crossed cell.
	for (int mz = 0; mz < cells; mz++)
		for (int my = 0; my < cells; my++)
			for (int mx = 0; mx < cells; mx++) {
				float d[8];
				for (int k = 0; k < 8; k++)
					d[k] = decode_sdf(lattice[dc_lattice_index(g, mx + kCorner[k][0],
							my + kCorner[k][1], mz + kCorner[k][2])]);
				float acc[3] = {0.0f, 0.0f, 0.0f};
				int n = 0;
				for (int e = 0; e < 12; e++) {
					const float da = d[kEdge[e][0]], db = d[kEdge[e][1]];
					if ((da <= 0.0f) == (db <= 0.0f)) continue;
					// da != db whenever the signs differ, so this never divides by zero.
					const float t = da / (da - db);
					for (int a = 0; a < 3; a++)
						acc[a] += static_cast<float>(kCorner[kEdge[e][0]][a]) +
								t * static_cast<float>(kCorner[kEdge[e][1]][a] -
										kCorner[kEdge[e][0]][a]);
					n++;
				}
				if (n == 0) continue;
				out->cell_vertex[dc_cell_index(g, mx, my, mz)] =
						static_cast<int32_t>(out->positions.size() / 3);
				const int m[3] = {mx, my, mz};
				for (int a = 0; a < 3; a++)
					out->positions.push_back(g.origin[a] +
							(static_cast<float>(m[a] - 1) + acc[a] / static_cast<float>(n)) *
									g.cell_size);
			}

	// Pass 2: one quad per sign-changing lattice edge. The grid owns the edges whose four
	// cells it holds — local edge coordinate u in [0, owned()), lattice index u + 1 — so
	// every edge in the world is emitted by exactly one chunk: no cracks, no duplicates.
	const int owned = g.owned();
	for (int uz = 0; uz < owned; uz++)
		for (int uy = 0; uy < owned; uy++)
			for (int ux = 0; ux < owned; ux++) {
				const int L[3] = {ux + 1, uy + 1, uz + 1};
				const float da = decode_sdf(lattice[dc_lattice_index(g, L[0], L[1], L[2])]);
				for (int axis = 0; axis < 3; axis++) {
					int Lb[3] = {L[0], L[1], L[2]};
					Lb[axis] += 1;
					const float db = decode_sdf(lattice[dc_lattice_index(g, Lb[0], Lb[1], Lb[2])]);
					const bool sa = da <= 0.0f, sb = db <= 0.0f;
					if (sa == sb) continue;
					const int b = (axis + 1) % 3, c = (axis + 2) % 3;
					int32_t q[4];
					bool ok = true;
					for (int k = 0; k < 4; k++) {
						int m[3] = {L[0], L[1], L[2]};
						m[b] += kQuad[k][0];
						m[c] += kQuad[k][1];
						q[k] = out->cell_vertex[dc_cell_index(g, m[0], m[1], m[2])];
						if (q[k] < 0) ok = false;
					}
					// Unreachable on the CPU (a crossed edge crosses all four of its cells),
					// but the GPU can lose a vertex to the per-chunk cap, and both sides run
					// the same rule so the diff stays honest.
					if (!ok) continue;
					// (axis, b, c) is a right-handed cycle, so q0..q3 wind counter-clockwise
					// seen from +axis. Solid -> air along +axis puts the air on the +axis
					// side, which is the side the normal must face.
					const int32_t tri_fwd[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
					const int32_t tri_rev[6] = {q[0], q[2], q[1], q[0], q[3], q[2]};
					const int32_t *tri = sa ? tri_fwd : tri_rev;
					for (int k = 0; k < 6; k++) out->indices.push_back(static_cast<uint32_t>(tri[k]));
				}
			}
}

} // namespace ve
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test -j$(nproc)`
Expected: PASS, including the six new dual-contour cases.

If "a flat plane gives one quad per owned column" fails on the *count*, the ownership rule is wrong; if it fails on `n[1] > 0`, the winding is inverted — fix both the CPU table and the shader in Task 5 together, never one alone.

- [ ] **Step 6: Commit**

```bash
git add extension/src/mesh/dual_contour.h extension/src/mesh/dual_contour.cpp \
        extension/tests/test_dual_contour.cpp
git commit -m "feat: dual contouring mesher for collision chunks"
```

---

### Task 3: `mesh/chunk_residency` — which chunks have colliders

The pure core that decides what the collider streamer does each frame: which chunks are in range, which are worth meshing at all, which need rebuilding after an edit, and what gets dropped when the pool is full. It is `ve::RegionResidency`'s sibling and follows the same distance-LRU rule.

**Files:**
- Create: `extension/src/mesh/chunk_residency.h`, `extension/src/mesh/chunk_residency.cpp`
- Create: `extension/tests/test_chunk_residency.cpp`

**Interfaces:**
- Consumes: `ve::chunk_distance`, `ve::chunk_min_brick`, `ve::kChunkSize` (`mesh/mesh_chunk.h`); `ve::WorldBounds` (`world/region.h`).
- Produces:
  - `struct ve::ChunkProbe { virtual bool chunk_has_surface(IVec3) const = 0; }`
  - `struct ve::ChunkResidencyConfig { WorldBounds bounds; float radius_m; int max_chunks; int max_builds_per_frame; int max_probes_per_frame; float evict_margin; }`
  - `struct ve::ChunkPlan { struct Entry { IVec3 chunk; int slot; }; std::vector<Entry> builds, releases; }`
  - `class ve::ChunkResidency` with `update(const float *centers, const float *radii, int center_count, const ChunkProbe &, int max_builds = -1)`, `mark_dirty(IVec3 lo, IVec3 hi)`, `note_built(IVec3)`, `note_failed(IVec3)`, `int note_empty(IVec3)`, `slot_of`, `chunk_of_slot`, `slot_resident`, `resident_count`, `pending_count`, `probe_cache_size`, `clear`, `config`

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_chunk_residency.cpp`:

```cpp
#include <doctest/doctest.h>
#include "mesh/chunk_residency.h"
#include <algorithm>
#include <vector>

// World: 8 x 4 x 8 regions from brick origin (0, -64, 0) => 204.8 x 102.4 x 204.8 m, so
// chunks run x,z in [0, 16) and y in [-4, 4).
static ve::ChunkResidencyConfig make_cfg(int max_chunks, int builds = 2, int probes = 4096) {
	ve::ChunkResidencyConfig cfg;
	cfg.bounds = ve::WorldBounds{{0, -64, 0}, {8, 4, 8}};
	cfg.radius_m = 64.0f;
	cfg.max_chunks = max_chunks;
	cfg.max_builds_per_frame = builds;
	cfg.max_probes_per_frame = probes;
	return cfg;
}

// Only chunk layer `layer` holds a surface — a flat world, which makes the expected resident
// set something the test can enumerate independently.
struct FakeProbe : ve::ChunkProbe {
	mutable int calls = 0;
	int layer = 2;
	bool everything = false;
	bool chunk_has_surface(ve::IVec3 c) const override {
		calls++;
		return everything || c.y == layer;
	}
};

static void settle(ve::ChunkResidency &r, const ve::ChunkProbe &probe, float x, float y,
		float z, int frames = 400) {
	const float c[3] = {x, y, z};
	for (int i = 0; i < frames; i++) {
		const ve::ChunkPlan p = r.update(c, nullptr, 1, probe);
		for (const auto &b : p.builds) r.note_built(b.chunk);
		if (p.builds.empty() && p.releases.empty()) return;
	}
}

TEST_CASE("the settled set is exactly the in-radius chunks the probe accepted") {
	const auto cfg = make_cfg(256);
	ve::ChunkResidency res(cfg);
	FakeProbe probe;
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	CHECK(res.resident_count() > 20);
	CHECK(res.pending_count() == 0);

	// Everything resident is on the surface layer and inside the radius...
	std::vector<int> slots;
	for (int s = 0; s < cfg.max_chunks; s++) {
		if (!res.slot_resident(s)) continue;
		const ve::IVec3 c = res.chunk_of_slot(s);
		CHECK(c.y == probe.layer);
		CHECK(ve::chunk_distance(c, 100.0f, 30.0f, 100.0f) <= cfg.radius_m);
		slots.push_back(s);
	}
	// ...slots are unique...
	std::sort(slots.begin(), slots.end());
	CHECK(std::adjacent_find(slots.begin(), slots.end()) == slots.end());
	// ...and nothing eligible was left out.
	for (int x = 0; x < 16; x++)
		for (int z = 0; z < 16; z++) {
			const ve::IVec3 c{x, probe.layer, z};
			if (ve::chunk_distance(c, 100.0f, 30.0f, 100.0f) > cfg.radius_m) continue;
			CHECK(res.slot_of(c) >= 0);
		}
}

TEST_CASE("probing is budgeted per frame and cached afterwards") {
	ve::ChunkResidency res(make_cfg(256, 2, 32));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	res.update(c, nullptr, 1, probe);
	CHECK(probe.calls == 32);
	res.update(c, nullptr, 1, probe);
	CHECK(probe.calls == 64);

	// Run until the CACHE stops growing rather than until the plan goes quiet: with the
	// budget this low, the ball still holds unprobed chunks long after the discovered ones
	// have all been built, and settle() would stop at the first such frame.
	int last = -1;
	for (int i = 0; i < 200 && res.probe_cache_size() != last; i++) {
		last = res.probe_cache_size();
		const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
		for (const auto &b : p.builds) res.note_built(b.chunk);
	}
	const int before = probe.calls;
	res.update(c, nullptr, 1, probe);
	CHECK(probe.calls == before); // every chunk in the ball has a cached verdict
	CHECK(res.probe_cache_size() > res.resident_count());
}

TEST_CASE("builds are throttled, nearest first, and stop once ready") {
	ve::ChunkResidency res(make_cfg(256, 2));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	CHECK(p.builds.size() == 2);
	CHECK(ve::chunk_distance(p.builds[0].chunk, 100.0f, 30.0f, 100.0f) <=
			ve::chunk_distance(p.builds[1].chunk, 100.0f, 30.0f, 100.0f));
	for (const auto &b : p.builds) CHECK(res.slot_of(b.chunk) == b.slot);

	// A build that is still in flight is not handed out again.
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe);
	for (const auto &b : p2.builds)
		for (const auto &prev : p.builds) CHECK_FALSE(b.chunk == prev.chunk);

	// max_builds clamps the config downwards (the caller passes 0 while the mesher is busy).
	CHECK(res.update(c, nullptr, 1, probe, 0).builds.empty());
}

TEST_CASE("the pool caps the resident set and keeps the nearest chunks") {
	ve::ChunkResidency res(make_cfg(8));
	FakeProbe probe;
	probe.everything = true;
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	CHECK(res.resident_count() == 8);
	for (int s = 0; s < 8; s++) {
		CHECK(res.slot_resident(s));
		// The eight nearest chunks all touch the cell holding the centre.
		CHECK(ve::chunk_distance(res.chunk_of_slot(s), 100.0f, 30.0f, 100.0f) <=
				ve::kChunkSize + 0.01f);
	}
}

TEST_CASE("moving out of the hysteresis band releases the slot") {
	const auto cfg = make_cfg(256);
	ve::ChunkResidency res(cfg);
	FakeProbe probe;
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	const int before = res.resident_count();

	// A step that leaves everything inside radius * evict_margin releases nothing.
	const float near_c[3] = {100.0f + cfg.radius_m * 0.1f, 30.0f, 100.0f};
	CHECK(res.update(near_c, nullptr, 1, probe).releases.empty());

	// A step far enough out drops what fell behind, and the count settles again.
	settle(res, probe, 100.0f, 30.0f, 200.0f);
	CHECK(res.resident_count() > 0);
	for (int s = 0; s < cfg.max_chunks; s++)
		if (res.slot_resident(s))
			CHECK(ve::chunk_distance(res.chunk_of_slot(s), 100.0f, 30.0f, 200.0f) <=
					cfg.radius_m * cfg.evict_margin);
	CHECK(before > 0);
}

TEST_CASE("mark_dirty re-plans resident chunks and re-probes cached ones") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	const float c[3] = {100.0f, 30.0f, 100.0f};
	CHECK(res.update(c, nullptr, 1, probe).builds.empty()); // settled: nothing to do

	const ve::IVec3 hit = ve::chunk_of_point(100.0f, 30.0f, 100.0f);
	res.mark_dirty(hit, hit);
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(p.builds.size() == 1);
	CHECK(p.builds[0].chunk == hit);

	// A chunk the probe rejected is re-probed after an edit: a sphere-add in open sky makes
	// surface where the generator had none, and a stale "empty" would hide it for ever.
	const ve::IVec3 sky{hit.x, hit.y + 1, hit.z};
	CHECK(res.slot_of(sky) == -1);
	const int calls = probe.calls;
	res.mark_dirty(sky, sky);
	res.update(c, nullptr, 1, probe);
	CHECK(probe.calls > calls);
}

TEST_CASE("an edit during a build survives the build landing") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(p.builds.size() == 2);
	const ve::IVec3 building = p.builds[0].chunk;

	res.mark_dirty(building, building);   // the edit lands while the mesher is running
	res.note_built(building);             // the pre-edit result arrives
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe, 8);
	bool replanned = false;
	for (const auto &b : p2.builds) replanned = replanned || b.chunk == building;
	CHECK(replanned);
}

TEST_CASE("note_empty frees the slot and stops the chunk coming back") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(!p.builds.empty());
	const ve::IVec3 empty = p.builds[0].chunk;
	const int slot = res.note_empty(empty);
	CHECK(slot == p.builds[0].slot);
	CHECK(res.slot_of(empty) == -1);
	for (int i = 0; i < 5; i++) res.update(c, nullptr, 1, probe);
	CHECK(res.slot_of(empty) == -1);

	// ...until something changes the field there.
	res.mark_dirty(empty, empty);
	settle(res, probe, 100.0f, 30.0f, 100.0f);
	CHECK(res.slot_of(empty) >= 0);
}

TEST_CASE("note_failed puts the chunk back in the queue") {
	ve::ChunkResidency res(make_cfg(256));
	FakeProbe probe;
	const float c[3] = {100.0f, 30.0f, 100.0f};
	const ve::ChunkPlan p = res.update(c, nullptr, 1, probe);
	REQUIRE(!p.builds.empty());
	res.note_failed(p.builds[0].chunk);
	const ve::ChunkPlan p2 = res.update(c, nullptr, 1, probe, 8);
	bool requeued = false;
	for (const auto &b : p2.builds) requeued = requeued || b.chunk == p.builds[0].chunk;
	CHECK(requeued);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test -j$(nproc)`
Expected: FAIL — `fatal error: mesh/chunk_residency.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `extension/src/mesh/chunk_residency.h`:

```cpp
#pragma once
#include "mesh/mesh_chunk.h"
#include "world/region.h"
#include <map>
#include <vector>

namespace ve {

// How the residency asks whether a chunk is worth meshing. An interface for the same reason
// ve::Generator is one: the real implementation needs the generator AND the edit log, and the
// edit log's lock lives on the Godot side of the wall.
struct ChunkProbe {
	virtual ~ChunkProbe() = default;
	virtual bool chunk_has_surface(IVec3 chunk) const = 0;
};

struct ChunkResidencyConfig {
	WorldBounds bounds{};
	float radius_m = 64.0f;   // spec §6: collision streams in a ~64 m radius
	int max_chunks = 160;
	int max_builds_per_frame = 2;
	// One probe is 729 field evaluations and a fresh world sees ~1300 unknown chunks on its
	// first frame. Budgeted, nearest first: the ground under the player resolves immediately
	// and the edge of the ball catches up over the next few frames.
	int max_probes_per_frame = 64;
	// A chunk is released only past radius_m * evict_margin. Without the gap a player standing
	// exactly on the boundary would mesh and drop the same chunk every frame.
	float evict_margin = 1.15f;
};

struct ChunkPlan {
	struct Entry {
		IVec3 chunk;
		int slot = -1;
	};
	std::vector<Entry> builds;   // slot reserved; the caller meshes these
	std::vector<Entry> releases; // slot freed; the caller drops the collider
};

// Which collision chunks are resident, in which pool slot, and which of them still owe a
// mesh. Distance-LRU: when the pool is full a closer candidate displaces the furthest
// resident, exactly as ve::RegionResidency does for region slots.
class ChunkResidency {
public:
	enum State { kNeedsBuild = 0, kBuilding = 1, kReady = 2 };

	explicit ChunkResidency(const ChunkResidencyConfig &cfg);

	// centers holds 3 floats per centre; radii holds one per centre, or nullptr to use
	// cfg.radius_m for all of them. Spec §6 wants "a ~64 m radius around the player + small
	// bubbles around active bodies"; M3 passes the player alone, M4's islands pass more.
	// max_builds clamps cfg.max_builds_per_frame downwards for this frame (negative = use
	// the config); the caller passes 0 while the mesher still has a batch in flight.
	ChunkPlan update(const float *centers, const float *radii, int center_count,
			const ChunkProbe &probe, int max_builds = -1);

	// Every chunk in the inclusive range needs its collider rebuilt, and every cached probe
	// verdict inside it is dropped: an op can put a surface into a chunk that had none, and
	// a cached "empty" would hide it for ever.
	void mark_dirty(IVec3 lo, IVec3 hi);

	void note_built(IVec3 chunk);  // a collider now exists for this chunk
	void note_failed(IVec3 chunk); // build failed: keep the slot, retry next frame
	int note_empty(IVec3 chunk);   // no geometry: caches "empty", frees and RETURNS the slot

	int slot_of(IVec3 chunk) const;
	IVec3 chunk_of_slot(int slot) const;
	bool slot_resident(int slot) const;
	int resident_count() const { return static_cast<int>(by_chunk_.size()); }
	int pending_count() const; // resident but not yet built
	int probe_cache_size() const { return static_cast<int>(probe_cache_.size()); }
	void clear();
	const ChunkResidencyConfig &config() const { return cfg_; }

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	static Key key(IVec3 c) { return Key{c.x, c.y, c.z}; }
	void release(IVec3 chunk, int slot, ChunkPlan *plan);

	ChunkResidencyConfig cfg_;
	std::map<Key, int> by_chunk_;    // chunk -> slot
	std::vector<IVec3> slot_chunk_;  // slot -> chunk (valid where slot_used_)
	std::vector<char> slot_used_;
	std::vector<char> slot_state_;   // State
	std::vector<int> free_slots_;
	std::map<Key, char> probe_cache_; // 1 = may hold a surface, 0 = known empty
};

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/mesh/chunk_residency.cpp`:

```cpp
#include "mesh/chunk_residency.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

namespace ve {

ChunkResidency::ChunkResidency(const ChunkResidencyConfig &cfg)
	: cfg_(cfg), slot_chunk_(static_cast<size_t>(cfg.max_chunks)),
	  slot_used_(static_cast<size_t>(cfg.max_chunks), 0),
	  slot_state_(static_cast<size_t>(cfg.max_chunks), static_cast<char>(kNeedsBuild)) {
	free_slots_.reserve(static_cast<size_t>(cfg.max_chunks));
	// Descending, so pop_back hands out slot 0 first: stable slot numbers in tests and HUD.
	for (int s = cfg.max_chunks - 1; s >= 0; s--) free_slots_.push_back(s);
}

int ChunkResidency::slot_of(IVec3 c) const {
	const auto it = by_chunk_.find(key(c));
	return it == by_chunk_.end() ? -1 : it->second;
}

IVec3 ChunkResidency::chunk_of_slot(int slot) const {
	return slot >= 0 && slot < cfg_.max_chunks ? slot_chunk_[slot] : IVec3{};
}

bool ChunkResidency::slot_resident(int slot) const {
	return slot >= 0 && slot < cfg_.max_chunks && slot_used_[slot] != 0;
}

int ChunkResidency::pending_count() const {
	int n = 0;
	for (const auto &kv : by_chunk_)
		if (slot_state_[kv.second] != kReady) n++;
	return n;
}

void ChunkResidency::clear() {
	by_chunk_.clear();
	probe_cache_.clear();
	std::fill(slot_used_.begin(), slot_used_.end(), 0);
	std::fill(slot_state_.begin(), slot_state_.end(), static_cast<char>(kNeedsBuild));
	free_slots_.clear();
	for (int s = cfg_.max_chunks - 1; s >= 0; s--) free_slots_.push_back(s);
}

void ChunkResidency::release(IVec3 chunk, int slot, ChunkPlan *plan) {
	by_chunk_.erase(key(chunk));
	slot_used_[slot] = 0;
	slot_state_[slot] = static_cast<char>(kNeedsBuild);
	free_slots_.push_back(slot);
	if (plan) plan->releases.push_back({chunk, slot});
}

void ChunkResidency::mark_dirty(IVec3 lo, IVec3 hi) {
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 c{x, y, z};
				probe_cache_.erase(key(c));
				const int slot = slot_of(c);
				if (slot >= 0) slot_state_[slot] = static_cast<char>(kNeedsBuild);
			}
}

void ChunkResidency::note_built(IVec3 chunk) {
	const int slot = slot_of(chunk);
	// Only when this is still the build we asked for. An edit that landed while the mesher
	// was running has already put the chunk back to kNeedsBuild, and the result in hand is
	// of the pre-edit field: promoting it would leave the crater uncollidable.
	if (slot >= 0 && slot_state_[slot] == kBuilding) slot_state_[slot] = static_cast<char>(kReady);
}

void ChunkResidency::note_failed(IVec3 chunk) {
	const int slot = slot_of(chunk);
	if (slot >= 0 && slot_state_[slot] == kBuilding)
		slot_state_[slot] = static_cast<char>(kNeedsBuild);
}

int ChunkResidency::note_empty(IVec3 chunk) {
	// The probe is conservative by construction, so a chunk it passed can still hold no
	// triangles. Caching the empty verdict is what stops it being re-planned every frame for
	// ever; mark_dirty drops the entry, so an edit brings it back.
	probe_cache_[key(chunk)] = 0;
	const int slot = slot_of(chunk);
	if (slot < 0) return -1;
	release(chunk, slot, nullptr);
	return slot;
}

ChunkPlan ChunkResidency::update(const float *centers, const float *radii, int center_count,
		const ChunkProbe &probe, int max_builds) {
	ChunkPlan plan;
	if (!centers || center_count <= 0) return plan;

	const auto radius_of = [&](int i) { return radii ? radii[i] : cfg_.radius_m; };
	const auto nearest = [&](IVec3 c) {
		float best = 1e30f;
		for (int i = 0; i < center_count; i++)
			best = std::min(best, chunk_distance(c, centers[i * 3], centers[i * 3 + 1],
					centers[i * 3 + 2]));
		return best;
	};
	const auto inside = [&](IVec3 c, float scale) {
		for (int i = 0; i < center_count; i++)
			if (chunk_distance(c, centers[i * 3], centers[i * 3 + 1], centers[i * 3 + 2]) <=
					radius_of(i) * scale)
				return true;
		return false;
	};

	// 1. Release what has drifted past the hysteresis boundary.
	{
		std::vector<std::pair<IVec3, int>> gone;
		for (const auto &kv : by_chunk_) {
			const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
			if (!inside(c, cfg_.evict_margin)) gone.emplace_back(c, kv.second);
		}
		for (const auto &g : gone) release(g.first, g.second, &plan);
	}

	// 2. Forget probe verdicts well outside the working set, so a long walk cannot grow the
	//    cache without bound.
	for (auto it = probe_cache_.begin(); it != probe_cache_.end();) {
		const IVec3 c{it->first.x, it->first.y, it->first.z};
		it = inside(c, cfg_.evict_margin * 1.5f) ? std::next(it) : probe_cache_.erase(it);
	}

	// 3. Collect in-bounds, in-radius, surface-bearing candidates that are not resident yet.
	struct Cand {
		float dist;
		IVec3 chunk;
	};
	std::vector<Cand> cands;
	std::set<Key> seen;
	int probes = 0;
	for (int i = 0; i < center_count; i++) {
		const float r = radius_of(i);
		const float cx = centers[i * 3], cy = centers[i * 3 + 1], cz = centers[i * 3 + 2];
		const auto span = [](float lo, float hi) {
			return std::make_pair(static_cast<int>(std::floor(lo / kChunkSize)),
					static_cast<int>(std::floor(hi / kChunkSize)));
		};
		const auto rx = span(cx - r, cx + r);
		const auto ry = span(cy - r, cy + r);
		const auto rz = span(cz - r, cz + r);
		for (int z = rz.first; z <= rz.second; z++)
			for (int y = ry.first; y <= ry.second; y++)
				for (int x = rx.first; x <= rx.second; x++) {
					const IVec3 c{x, y, z};
					// A chunk is 16 bricks and the world is region-aligned (32 bricks), so a
					// chunk is either wholly inside or wholly outside: one corner decides.
					if (!cfg_.bounds.contains_brick(chunk_min_brick(c))) continue;
					if (chunk_distance(c, cx, cy, cz) > r) continue;
					if (!seen.insert(key(c)).second) continue;
					if (slot_of(c) >= 0) continue;
					auto pc = probe_cache_.find(key(c));
					if (pc == probe_cache_.end()) {
						if (probes >= cfg_.max_probes_per_frame) continue; // next frame
						probes++;
						pc = probe_cache_.emplace(key(c),
								static_cast<char>(probe.chunk_has_surface(c) ? 1 : 0)).first;
					}
					if (pc->second == 0) continue;
					cands.push_back({nearest(c), c});
				}
	}
	std::sort(cands.begin(), cands.end(),
			[](const Cand &a, const Cand &b) { return a.dist < b.dist; });

	// 4. Make them resident, nearest first. With the pool full, a closer candidate DISPLACES
	//    the furthest resident, so what goes missing under pressure is the far edge of the
	//    ball and never the ground the player is standing on.
	for (const Cand &cand : cands) {
		if (free_slots_.empty()) {
			float worst = cand.dist;
			IVec3 victim{};
			int victim_slot = -1;
			for (const auto &kv : by_chunk_) {
				const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
				const float d = nearest(c);
				if (d > worst) {
					worst = d;
					victim = c;
					victim_slot = kv.second;
				}
			}
			// Candidates are sorted, so if this one cannot displace anything, none can.
			if (victim_slot < 0) break;
			release(victim, victim_slot, &plan);
		}
		const int slot = free_slots_.back();
		free_slots_.pop_back();
		slot_used_[slot] = 1;
		slot_state_[slot] = static_cast<char>(kNeedsBuild);
		slot_chunk_[slot] = cand.chunk;
		by_chunk_[key(cand.chunk)] = slot;
	}

	// 5. Hand out this frame's builds, nearest first. Distance alone is the right priority:
	//    the chunk an edit just dirtied is the one under the player's crosshair.
	const int cap = max_builds < 0 ? cfg_.max_builds_per_frame
								   : std::min(max_builds, cfg_.max_builds_per_frame);
	if (cap > 0) {
		std::vector<Cand> want;
		for (const auto &kv : by_chunk_)
			if (slot_state_[kv.second] == kNeedsBuild) {
				const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
				want.push_back({nearest(c), c});
			}
		std::sort(want.begin(), want.end(),
				[](const Cand &a, const Cand &b) { return a.dist < b.dist; });
		for (int i = 0; i < static_cast<int>(want.size()) && i < cap; i++) {
			const int slot = slot_of(want[i].chunk);
			slot_state_[slot] = static_cast<char>(kBuilding);
			plan.builds.push_back({want[i].chunk, slot});
		}
	}
	return plan;
}

} // namespace ve
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test -j$(nproc)`
Expected: PASS — nine new cases plus everything from Tasks 1–2 and M1/M2.

- [ ] **Step 6: Commit**

```bash
git add extension/src/mesh/chunk_residency.h extension/src/mesh/chunk_residency.cpp \
        extension/tests/test_chunk_residency.cpp
git commit -m "feat: collision chunk residency with probe cache and distance-LRU"
```

---

### Task 4: `render/mesh_pass` + `mesh_field.comp.glsl` — the chunk lattice on the GPU

The mesher's own `RenderingDevice` and its first pass: 130³ samples of `G + region ops` at 0.1 m, written into an R8 volume with the same encoding the brick atlas uses. Nothing here meshes anything yet — the deliverable is a lattice the CPU can prove correct.

**Files:**
- Create: `extension/src/render/mesh_pass.h`, `extension/src/render/mesh_pass.cpp`
- Create: `shaders/mesh_common.glsl`, `shaders/mesh_field.comp.glsl`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_mesh_lattice.gd`

**Interfaces:**
- Consumes: `ve::load_shader_source`, `ve::strip_shader_annotations` (`render/shader_loader.h`); `ve::EditOp`, `ve::kMaxRegionOps`; `ve::kChunkLattice`, `ve::chunk_world_origin`, `ve::region_of_chunk`; `ve::eval_field`, `ve::encode_sdf`. Shader side: `shaders/field.glsl` (`eval_field`, `MAX_REGION_OPS`), `shaders/common.glsl` (`quantise_sdf`, `decode_sdf`).
- Produces:
  - `struct godot::MeshPassConfig { int max_jobs = 2; int max_verts = 65536; int max_tris = 65536; }`
  - `struct godot::MeshJob { ve::IVec3 chunk; const ve::EditOp *ops; int op_count; }`
  - `class godot::MeshPass` with `initialize(RenderingDevice *, const MeshPassConfig &)`, `teardown()`, `is_valid()`, `config()`, `bool run_field_sync(const MeshJob &, std::vector<uint8_t> *lattice)`
  - `VoxelWorld::ensure_physics_initialized()`, `VoxelWorld::teardown_physics()`, `Dictionary VoxelWorld::debug_mesh_lattice_diff(Vector3i chunk)`
  - Shader helpers `lattice_world_pos(ivec3 chunk, ivec3 l)` and `mesh_cell_index(ivec3 m)` in `shaders/mesh_common.glsl`

- [ ] **Step 1: Write the failing test**

Create `tests/test_mesh_lattice.gd`:

```gdscript
extends GdUnitTestSuite

# GPU/CPU differential test for the collision chunk lattice (spec section 8). The mesher does
# not read the brick atlas: it evaluates shaders/field.glsl at 0.1 m, the same field the
# bricks are generated from, so collision cannot inherit a dropped brick's hole and a chunk
# outside the residency ball can still be meshed.
#
# Tolerance: one encoded step, for the same reason test_brick_diff.gd allows one — sin() is
# not bit-identical between glibc and a Vulkan driver, and a uint8 with ~5 mm steps cannot
# show a disagreement smaller than half a step. Two steps would be a real bug.

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false          # the tests drive the tick by hand
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func test_lattice_matches_the_cpu_field() -> void:
	var w := make_world()
	# Chunk (2, 4, 2) spans world y [51.2, 64.0) — the surface (51.2 + hills) crosses it.
	var d: Dictionary = w.debug_mesh_lattice_diff(Vector3i(2, 4, 2))
	assert_int(d["samples"]).is_equal(130 * 130 * 130)
	assert_int(d["max_diff"]).override_failure_message(
		"lattice differs by %d encoded steps" % d["max_diff"]).is_less_equal(1)
	assert_int(d["diff_over_one"]).is_equal(0)
	# The chunk really does straddle the surface, or the comparison proved nothing.
	assert_bool(d["has_surface"]).is_true()

func test_lattice_includes_the_overlap_plane_and_the_edits() -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	# Carve at the low corner of chunk (2, 4, 2) so the crater reaches into the overlap cell
	# below its origin — the plane a chunk needs to close its minimum faces.
	var origin := Vector3(2 * 12.8, 4 * 12.8, 2 * 12.8)
	var r: Dictionary = tool.apply_sphere_subtract(origin + Vector3(0.05, 0.05, 0.05), 3.0)
	assert_array(r["rejected"]).is_empty()
	var d: Dictionary = w.debug_mesh_lattice_diff(Vector3i(2, 4, 2))
	assert_int(d["max_diff"]).is_less_equal(1)
	assert_int(d["diff_over_one"]).is_equal(0)
	assert_int(d["op_count"]).is_greater(0)

func test_a_chunk_far_outside_the_residency_ball_still_meshes() -> void:
	var w := make_world()
	# Nothing is streamed, no atlas is initialised: the mesher is independent of both.
	var d: Dictionary = w.debug_mesh_lattice_diff(Vector3i(7, 4, 7))
	assert_int(d["samples"]).is_equal(130 * 130 * 130)
	assert_int(d["max_diff"]).is_less_equal(1)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_mesh_lattice.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_init_physics'`.

- [ ] **Step 3: Write the shared shader include**

Create `shaders/mesh_common.glsl`:

```glsl
// Chunk lattice addressing, shared by the three collision-meshing passes so they can never
// disagree. Mirror of extension/src/mesh/mesh_chunk.h and ve::dual_contour's conventions:
// lattice array index i holds the sample at local coordinate i - 1, mesh-cell array index m
// holds the cell at local coordinate m - 1, and cell m's corners are lattice m and m + 1.
// The one-cell overlap below the origin is what lets a chunk close the quads on its minimum
// faces without reading a neighbouring chunk's lattice.
#include "common.glsl"

const int CHUNK_CELLS = 128;        // ve::kChunkCells
const int CHUNK_MESH_CELLS = 129;   // ve::kChunkMeshCells
const int CHUNK_LATTICE = 130;      // ve::kChunkLattice
const float CHUNK_CELL_SIZE = 0.1;  // ve::kChunkCellSize
const float CHUNK_SIZE = 12.8;      // ve::kChunkSize

vec3 lattice_world_pos(ivec3 chunk, ivec3 l) {
	return vec3(chunk) * CHUNK_SIZE + (vec3(l) - 1.0) * CHUNK_CELL_SIZE;
}

int mesh_cell_index(ivec3 m) {
	return m.x + m.y * CHUNK_MESH_CELLS + m.z * CHUNK_MESH_CELLS * CHUNK_MESH_CELLS;
}
```

- [ ] **Step 4: Write the field shader**

Create `shaders/mesh_field.comp.glsl`. Note that the op pool is this pass's own small buffer (one region's list per job), not the atlas' — the mesher never touches the atlas:

```glsl
#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 1
#include "field.glsl"
#include "mesh_common.glsl"

// One thread per lattice sample. 130 is not a multiple of 4, so the last group in each axis
// runs partly out of bounds and returns.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) writeonly uniform image3D lattice;
// binding 1 is the field op pool, declared by field.glsl

layout(push_constant, std430) uniform Push {
	ivec4 chunk;  // xyz = chunk coordinates, w = job index in this batch
	ivec4 params; // x = op count, y = max verts per job, z = max tris per job, w = unused
} pc;

void main() {
	ivec3 l = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(l, ivec3(CHUNK_LATTICE)))) return;
	float sdf;
	uint mat; // the mesher has no use for materials; collision carries none
	eval_field(lattice_world_pos(pc.chunk.xyz, l), uint(pc.chunk.w) * MAX_REGION_OPS,
			uint(pc.params.x), sdf, mat);
	imageStore(lattice, l, vec4(quantise_sdf(sdf)));
}
```

- [ ] **Step 5: Write the MeshPass header**

Create `extension/src/render/mesh_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>
#include <vector>
#include "generator/edit_ops.h"
#include "world/region.h"

namespace godot {

struct MeshPassConfig {
	int max_jobs = 2;      // chunks per batch
	int max_verts = 65536; // a fully covered 12.8 m chunk holds ~16 400
	int max_tris = 65536;  // ...and 32 768 triangles
};

struct MeshJob {
	ve::IVec3 chunk{};
	const ve::EditOp *ops = nullptr; // the chunk's region's op list; copied at submit
	int op_count = 0;
};

struct MeshResult {
	ve::IVec3 chunk{};
	std::vector<float> positions;  // 3 per vertex, world space
	std::vector<uint32_t> indices; // 3 per triangle
	bool overflow = false;         // a cap was hit: the mesh is missing pieces
};

// The collision mesher. Owns every GPU resource on ITS OWN local RenderingDevice — the
// mesher never reads the brick atlas (see the plan's Deliberate Decisions), so it shares no
// resource with the renderer and can be submitted and synced without touching the frame.
class MeshPass {
public:
	~MeshPass();

	bool initialize(RenderingDevice *rd, const MeshPassConfig &cfg);
	void teardown();
	bool is_valid() const { return field_pipeline_.is_valid(); }
	const MeshPassConfig &config() const { return cfg_; }

	// Runs the field pass alone for one chunk, inline (record, submit, sync, read back).
	// Diagnostic only — the streaming path never stalls like this.
	bool run_field_sync(const MeshJob &job, std::vector<uint8_t> *lattice);

private:
	bool build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline);
	void record_field(int64_t list, const MeshJob &job, int job_index);
	void upload_ops(const MeshJob &job, int job_index);
	void push(int64_t list, const MeshJob &job, int job_index);

	RenderingDevice *rd_ = nullptr;
	MeshPassConfig cfg_;
	RID lattice_;     // R8_UNORM 3D, 130^3 encoded sdf
	RID cells_;       // int32 per mesh cell: vertex index or -1
	RID verts_;       // float3 per vertex, max_jobs * max_verts
	RID tris_;        // uint3 per triangle, max_jobs * max_tris
	RID counts_;      // 4 uints per job: vert count, tri count, overflow bits, pad
	RID ops_;         // max_jobs * kMaxRegionOps EditOps
	RID field_shader_, field_pipeline_, field_uset_;
};

} // namespace godot
```

- [ ] **Step 6: Write the MeshPass implementation**

Create `extension/src/render/mesh_pass.cpp`:

```cpp
#include "render/mesh_pass.h"
#include "mesh/mesh_chunk.h"
#include "render/shader_loader.h"
#include "world/edit_log.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

namespace {

Ref<RDUniform> storage(int binding, RID rid) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u->set_binding(binding);
	u->add_id(rid);
	return u;
}

Ref<RDUniform> image(int binding, RID rid) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u->set_binding(binding);
	u->add_id(rid);
	return u;
}

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

PackedByteArray zeroed(int64_t bytes) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(0);
	return b;
}

// Groups for a dispatch of `n` threads per axis at local size 4.
int groups(int n) { return (n + 3) / 4; }

} // namespace

MeshPass::~MeshPass() {
	teardown();
}

bool MeshPass::build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(String(res_path));
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("MeshPass: ", res_path, " load failed: ", err.c_str());
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err =
			spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("MeshPass: ", res_path, ": ", compile_err);
		return false;
	}
	*shader = rd->shader_create_from_spirv(spirv);
	if (!shader->is_valid()) return false;
	*pipeline = rd->compute_pipeline_create(*shader);
	return pipeline->is_valid();
}

bool MeshPass::initialize(RenderingDevice *rd, const MeshPassConfig &cfg) {
	teardown();
	rd_ = rd;
	cfg_ = cfg;
	if (!rd || cfg.max_jobs <= 0 || cfg.max_verts <= 0 || cfg.max_tris <= 0) {
		UtilityFunctions::printerr("MeshPass: degenerate configuration");
		return false;
	}

	{
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
		f->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
		f->set_width(ve::kChunkLattice);
		f->set_height(ve::kChunkLattice);
		f->set_depth(ve::kChunkLattice);
		f->set_mipmaps(1);
		// STORAGE for the field pass to write and the mesher passes to read;
		// CAN_COPY_FROM so the differential test can read the lattice back.
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		lattice_ = rd->texture_create(f, v, TypedArray<PackedByteArray>());
	}
	cells_ = rd->storage_buffer_create(static_cast<uint32_t>(ve::kChunkCellCount) * 4,
			zeroed(static_cast<int64_t>(ve::kChunkCellCount) * 4));
	verts_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_jobs) * cfg_.max_verts * 12,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * cfg_.max_verts * 12));
	tris_ = rd->storage_buffer_create(static_cast<uint32_t>(cfg_.max_jobs) * cfg_.max_tris * 12,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * cfg_.max_tris * 12));
	counts_ = rd->storage_buffer_create(static_cast<uint32_t>(cfg_.max_jobs) * 16,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * 16));
	ops_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_jobs) * ve::kMaxRegionOps * 32,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * ve::kMaxRegionOps * 32));
	if (!lattice_.is_valid() || !cells_.is_valid() || !verts_.is_valid() || !tris_.is_valid() ||
			!counts_.is_valid() || !ops_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: buffer creation failed");
		teardown();
		return false;
	}

	if (!build(rd, "res://shaders/mesh_field.comp.glsl", &field_shader_, &field_pipeline_)) {
		teardown();
		return false;
	}
	field_uset_ = rd->uniform_set_create(Array::make(image(0, lattice_), storage(1, ops_)),
			field_shader_, 0);
	if (!field_uset_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void MeshPass::teardown() {
	if (!rd_) return;
	// Uniform sets first: freeing a shader cascades to its pipelines and referencing sets
	// (M1's documented order).
	free_if_valid(rd_, field_uset_);
	free_if_valid(rd_, field_pipeline_);
	free_if_valid(rd_, field_shader_);
	free_if_valid(rd_, ops_);
	free_if_valid(rd_, counts_);
	free_if_valid(rd_, tris_);
	free_if_valid(rd_, verts_);
	free_if_valid(rd_, cells_);
	free_if_valid(rd_, lattice_);
	rd_ = nullptr;
}

void MeshPass::upload_ops(const MeshJob &job, int job_index) {
	const int n = job.ops ? std::min(job.op_count, ve::kMaxRegionOps) : 0;
	if (n <= 0) return; // op_count in the push constant is what the shader reads
	PackedByteArray b;
	b.resize(static_cast<int64_t>(n) * 32);
	std::memcpy(b.ptrw(), job.ops, static_cast<size_t>(n) * 32);
	rd_->buffer_update(ops_, static_cast<uint32_t>(job_index) * ve::kMaxRegionOps * 32,
			static_cast<uint32_t>(b.size()), b);
}

// The same 32-byte block for all three passes, so one helper serves them all.
void MeshPass::push(int64_t list, const MeshJob &job, int job_index) {
	PackedByteArray pc;
	pc.resize(32);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = job.chunk.x;
	p[1] = job.chunk.y;
	p[2] = job.chunk.z;
	p[3] = job_index;
	p[4] = std::min(job.op_count, ve::kMaxRegionOps);
	p[5] = cfg_.max_verts;
	p[6] = cfg_.max_tris;
	p[7] = 0;
	rd_->compute_list_set_push_constant(list, pc, pc.size());
}

void MeshPass::record_field(int64_t list, const MeshJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, field_pipeline_);
	rd_->compute_list_bind_uniform_set(list, field_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kChunkLattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

bool MeshPass::run_field_sync(const MeshJob &job, std::vector<uint8_t> *lattice) {
	if (!is_valid()) return false;
	upload_ops(job, 0);
	const int64_t list = rd_->compute_list_begin();
	record_field(list, job, 0);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();
	if (lattice) {
		const PackedByteArray data = rd_->texture_get_data(lattice_, 0);
		if (data.size() < ve::kChunkLatticeCount) return false;
		lattice->assign(data.ptr(), data.ptr() + ve::kChunkLatticeCount);
	}
	return true;
}
```

`std::min` needs `#include <algorithm>` alongside `<cstring>`.

- [ ] **Step 7: Wire it into VoxelWorld**

Add to `extension/src/voxel_world.h` — the physics half is deliberately independent of
`ensure_initialized()`: the mesher needs no atlas, and a test must be able to mesh without one.

```cpp
// near the other forward declarations
class MeshPass;
class ColliderStreamer;
```

```cpp
// exports, next to residency_radius_m_
bool physics_enabled_ = true;
NodePath physics_center_path_;
float physics_radius_m_ = 64.0f;
int max_collider_chunks_ = 160;
int mesh_jobs_per_frame_ = 2;
int shape_builds_per_frame_ = 2;

// members, next to streamer_
RenderingDevice *mesh_rd_ = nullptr; // owned; ALWAYS local (submit/sync are illegal on main)
MeshPass *mesh_pass_ = nullptr;
ve::ChunkResidency *chunks_ = nullptr;
ColliderStreamer *colliders_ = nullptr;
bool physics_ready_ = false;
std::vector<std::pair<ve::IVec3, ve::IVec3>> pending_dirty_; // guarded by edit_mutex_
```

```cpp
// public
void _process(double delta) override;
void ensure_physics_initialized();
void teardown_physics();
int physics_tick(Vector3 center); // returns actions taken; Task 7 gives it a body
bool is_physics_ready() const { return physics_ready_; }
void set_physics_enabled(bool v) { physics_enabled_ = v; }
bool get_physics_enabled() const { return physics_enabled_; }
void set_physics_center_path(const NodePath &p) { physics_center_path_ = p; }
NodePath get_physics_center_path() const { return physics_center_path_; }
void set_physics_radius_m(float v) { physics_radius_m_ = v; }
float get_physics_radius_m() const { return physics_radius_m_; }
void set_max_collider_chunks(int v) { max_collider_chunks_ = v; }
int get_max_collider_chunks() const { return max_collider_chunks_; }
void set_mesh_jobs_per_frame(int v) { mesh_jobs_per_frame_ = v; }
int get_mesh_jobs_per_frame() const { return mesh_jobs_per_frame_; }
void set_shape_builds_per_frame(int v) { shape_builds_per_frame_ = v; }
int get_shape_builds_per_frame() const { return shape_builds_per_frame_; }

// --- Task 4 hooks ---
bool debug_init_physics();
void debug_teardown_physics();
Dictionary debug_mesh_lattice_diff(Vector3i chunk);
```

Add `#include <godot_cpp/variant/node_path.hpp>`, `#include "mesh/chunk_residency.h"` and `#include <utility>` (for the `std::pair` in `pending_dirty_`) to `voxel_world.h`, and in `voxel_world.cpp`:

```cpp
#include "render/mesh_pass.h"
#include "mesh/dual_contour.h"
#include "mesh/mesh_chunk.h"
#include <godot_cpp/classes/world3d.hpp>
```

```cpp
void VoxelWorld::_ready() {
	// Godot only calls _process on a GDExtension node that asks for it.
	set_process(true);
}

void VoxelWorld::_process(double) {
	if (!physics_enabled_ || physics_center_path_.is_empty()) return;
	Node3D *anchor = Object::cast_to<Node3D>(get_node_or_null(physics_center_path_));
	if (!anchor) return;
	ensure_physics_initialized();
	physics_tick(anchor->get_global_position());
}

void VoxelWorld::ensure_physics_initialized() {
	if (physics_ready_) return;
	if (!mesh_rd_) mesh_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	if (!mesh_rd_) {
		UtilityFunctions::printerr("VoxelWorld: no local RenderingDevice for the mesher");
		return;
	}
	// The CPU cores are shared with the streaming path and outlive both (voxel_world.h).
	if (!edit_log_) edit_log_ = new ve::EditLog(world_bounds());
	mesh_pass_ = new MeshPass();
	MeshPassConfig mcfg;
	mcfg.max_jobs = mesh_jobs_per_frame_;
	if (!mesh_pass_->initialize(mesh_rd_, mcfg)) {
		delete mesh_pass_;
		mesh_pass_ = nullptr;
		return;
	}
	ve::ChunkResidencyConfig ccfg;
	ccfg.bounds = world_bounds();
	ccfg.radius_m = physics_radius_m_;
	ccfg.max_chunks = max_collider_chunks_;
	ccfg.max_builds_per_frame = mesh_jobs_per_frame_;
	chunks_ = new ve::ChunkResidency(ccfg);
	physics_ready_ = true;
}

void VoxelWorld::teardown_physics() {
	physics_ready_ = false;
	if (mesh_pass_) { delete mesh_pass_; mesh_pass_ = nullptr; }
	if (chunks_) { delete chunks_; chunks_ = nullptr; }
	if (mesh_rd_) { memdelete(mesh_rd_); mesh_rd_ = nullptr; }
	pending_dirty_.clear();
}

int VoxelWorld::physics_tick(Vector3) { return 0; } // Task 7 fills this in

bool VoxelWorld::debug_init_physics() {
	ensure_physics_initialized();
	return physics_ready_;
}

void VoxelWorld::debug_teardown_physics() {
	teardown_physics();
}

Dictionary VoxelWorld::debug_mesh_lattice_diff(Vector3i chunk) {
	Dictionary d;
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_pass_) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ops = edit_log_->ops(ve::region_of_chunk(c));
	}
	MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
	std::vector<uint8_t> gpu;
	if (!mesh_pass_->run_field_sync(job, &gpu)) return d;

	ve::AnalyticGenerator gen;
	const ve::DcGrid g = ve::chunk_dc_grid(c);
	int max_diff = 0, over_one = 0;
	bool pos = false, neg = false;
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++) {
				const float p[3] = {g.origin[0] + (x - 1) * g.cell_size,
						g.origin[1] + (y - 1) * g.cell_size,
						g.origin[2] + (z - 1) * g.cell_size};
				const float s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
						p[0], p[1], p[2]).sdf;
				if (s <= 0.0f) neg = true; else pos = true;
				const int want = ve::encode_sdf(s);
				const int got = gpu[ve::dc_lattice_index(g, x, y, z)];
				const int diff = std::abs(got - want);
				max_diff = std::max(max_diff, diff);
				if (diff > 1) over_one++;
			}
	d["samples"] = ve::kChunkLatticeCount;
	d["max_diff"] = max_diff;
	d["diff_over_one"] = over_one;
	d["has_surface"] = pos && neg;
	d["op_count"] = static_cast<int>(ops.size());
	return d;
}
```

Extend `_exit_tree()` to tear physics down **before** the render-side objects (they use different devices, but the ordering keeps the destructor sequence readable):

```cpp
void VoxelWorld::_exit_tree() {
	teardown_physics();
	teardown_gpu();
	if (residency_) { delete residency_; residency_ = nullptr; }
	if (edit_log_) { delete edit_log_; edit_log_ = nullptr; }
	pending_edits_.clear();
	overflow_seen_ = 0;
	if (local_rd_) {
		memdelete(local_rd_);
		local_rd_ = nullptr;
	}
	main_rd_ = nullptr;
}
```

Register the new surface in `_bind_methods()`:

```cpp
ClassDB::bind_method(D_METHOD("set_physics_enabled", "v"), &VoxelWorld::set_physics_enabled);
ClassDB::bind_method(D_METHOD("get_physics_enabled"), &VoxelWorld::get_physics_enabled);
ClassDB::bind_method(D_METHOD("set_physics_center_path", "p"), &VoxelWorld::set_physics_center_path);
ClassDB::bind_method(D_METHOD("get_physics_center_path"), &VoxelWorld::get_physics_center_path);
ClassDB::bind_method(D_METHOD("set_physics_radius_m", "v"), &VoxelWorld::set_physics_radius_m);
ClassDB::bind_method(D_METHOD("get_physics_radius_m"), &VoxelWorld::get_physics_radius_m);
ClassDB::bind_method(D_METHOD("set_max_collider_chunks", "v"), &VoxelWorld::set_max_collider_chunks);
ClassDB::bind_method(D_METHOD("get_max_collider_chunks"), &VoxelWorld::get_max_collider_chunks);
ClassDB::bind_method(D_METHOD("set_mesh_jobs_per_frame", "v"), &VoxelWorld::set_mesh_jobs_per_frame);
ClassDB::bind_method(D_METHOD("get_mesh_jobs_per_frame"), &VoxelWorld::get_mesh_jobs_per_frame);
ClassDB::bind_method(D_METHOD("set_shape_builds_per_frame", "v"), &VoxelWorld::set_shape_builds_per_frame);
ClassDB::bind_method(D_METHOD("get_shape_builds_per_frame"), &VoxelWorld::get_shape_builds_per_frame);
ClassDB::bind_method(D_METHOD("debug_init_physics"), &VoxelWorld::debug_init_physics);
ClassDB::bind_method(D_METHOD("debug_teardown_physics"), &VoxelWorld::debug_teardown_physics);
ClassDB::bind_method(D_METHOD("debug_mesh_lattice_diff", "chunk"), &VoxelWorld::debug_mesh_lattice_diff);
ADD_PROPERTY(PropertyInfo(Variant::BOOL, "physics_enabled"), "set_physics_enabled", "get_physics_enabled");
ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "physics_center_path"), "set_physics_center_path", "get_physics_center_path");
ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "physics_radius_m"), "set_physics_radius_m", "get_physics_radius_m");
ADD_PROPERTY(PropertyInfo(Variant::INT, "max_collider_chunks"), "set_max_collider_chunks", "get_max_collider_chunks");
ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_jobs_per_frame"), "set_mesh_jobs_per_frame", "get_mesh_jobs_per_frame");
ADD_PROPERTY(PropertyInfo(Variant::INT, "shape_builds_per_frame"), "set_shape_builds_per_frame", "get_shape_builds_per_frame");
```

- [ ] **Step 8: Build and run the test**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_mesh_lattice.gd`
Expected: PASS, three cases.

If the shader fails to compile, read the error text: M2 errata 5 and 7 are the two failure modes that have already bitten this codebase (a GLSL reserved word used as an identifier, and an `ivec4` push-constant member passed where an `ivec3` is expected — hence the `.xyz` in `pc.chunk.xyz`).

- [ ] **Step 9: Run the whole suite for regressions**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: everything green — M3 has added a second local device but touched no M2 code path.

- [ ] **Step 10: Commit**

```bash
git add extension/src/render/mesh_pass.h extension/src/render/mesh_pass.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp \
        shaders/mesh_common.glsl shaders/mesh_field.comp.glsl tests/test_mesh_lattice.gd
git commit -m "feat: GPU chunk field lattice on a dedicated mesher device"
```

---

### Task 5: `mesh_cells` + `mesh_quads` — dual contouring on the GPU

The two passes that turn the lattice into triangles, and the differential test that pins them to `ve::dual_contour` (spec §8).

**Files:**
- Create: `shaders/mesh_cells.comp.glsl`, `shaders/mesh_quads.comp.glsl`
- Modify: `extension/src/render/mesh_pass.h`, `extension/src/render/mesh_pass.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_mesh_diff.gd`

**Interfaces:**
- Consumes: Task 4's `MeshPass` internals; `ve::dual_contour`, `ve::MeshBuffer`, `ve::DcGrid`, `ve::chunk_dc_grid`, `ve::dc_lattice_index`, `ve::dc_cell_index`.
- Produces:
  - `MeshPass::mesh_sync(const MeshJob &, MeshResult *out, std::vector<uint8_t> *lattice, std::vector<int32_t> *cell_vertex)`
  - `MeshPass::record_job(int64_t list, const MeshJob &, int job_index)` (private; Task 6's batch uses it)
  - `MeshPass::read_job(int job_index, ve::IVec3 chunk, MeshResult *out)` (private)
  - `MeshPass::reset_counts()` (private)
  - `Dictionary VoxelWorld::debug_mesh_diff(Vector3i chunk)`

- [ ] **Step 1: Write the failing test**

Create `tests/test_mesh_diff.gd`:

```gdscript
extends GdUnitTestSuite

# GPU/CPU differential test for the collision mesher (spec section 8): shaders/mesh_cells and
# shaders/mesh_quads against ve::dual_contour. What is compared, and why each tolerance is
# what it is:
#
#  * The lattice, all 130^3 samples, against ve::eval_field: one encoded step, exactly as
#    test_brick_diff.gd allows (glibc's sin() vs the driver's).
#  * The MESH, against ve::dual_contour run on the GPU'S OWN read-back lattice. Both sides
#    therefore consume identical bytes, so the cell sets and triangle sets must match
#    EXACTLY and positions to 1 mm — the only remaining difference is float rounding inside
#    the interpolation. (This is M2 errata 7's rule for the mip chain applied to the mesher:
#    the property under test is that the algorithm agrees, not that sin() is bit-identical.)
#  * Triangles are compared as cyclically normalised triples of CELL indices, so vertex
#    numbering (the GPU allocates it with atomics, in no fixed order) does not enter, but an
#    inverted winding still shows up as a difference.
#  * Winding is checked independently: the field must be greater on the normal's side of
#    every triangle. That is what keeps a character on top of the ground rather than under it.

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func check_diff(d: Dictionary, label: String) -> void:
	assert_int(d["lattice_max_diff"]).override_failure_message(
		"%s: lattice differs by %d encoded steps" % [label, d["lattice_max_diff"]]).is_less_equal(1)
	assert_int(d["cells_only_cpu"]).override_failure_message(
		"%s: %d cells hold a vertex on the CPU only" % [label, d["cells_only_cpu"]]).is_equal(0)
	assert_int(d["cells_only_gpu"]).override_failure_message(
		"%s: %d cells hold a vertex on the GPU only" % [label, d["cells_only_gpu"]]).is_equal(0)
	assert_float(d["max_pos_diff"]).override_failure_message(
		"%s: vertex positions differ by %f m" % [label, d["max_pos_diff"]]).is_less(0.001)
	assert_int(d["tri_only_cpu"]).override_failure_message(
		"%s: %d triangles are CPU-only" % [label, d["tri_only_cpu"]]).is_equal(0)
	assert_int(d["tri_only_gpu"]).override_failure_message(
		"%s: %d triangles are GPU-only" % [label, d["tri_only_gpu"]]).is_equal(0)
	assert_bool(d["overflow"]).override_failure_message(
		"%s: the mesher hit a per-chunk cap" % label).is_false()

func test_a_surface_chunk_meshes_identically_on_both_sides() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_mesh_diff(Vector3i(2, 4, 2))
	check_diff(d, "plain terrain")
	assert_int(d["cells_both"]).is_greater(1000)
	assert_int(d["tri_gpu"]).is_greater(1000)
	assert_int(d["tri_gpu"]).is_equal(d["tri_cpu"])

func test_every_vertex_sits_on_the_surface_and_faces_the_air() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_mesh_diff(Vector3i(2, 4, 2))
	# The generator reports a distance that exceeds the true one by at most lipschitz() = 2,
	# so "within 0.1 m reported" is "within half a 0.1 m cell of the real surface". The one
	# percent that may miss it are the cells straddling the crease where the cave sphere
	# meets the terrain: a mass point averaged across a kink lands slightly off both sheets.
	assert_int(d["verts_off_10cm"]).override_failure_message(
		"%d of %d vertices sit more than 0.1 m off the surface"
		% [d["verts_off_10cm"], d["cells_gpu"]]).is_less_equal(int(d["cells_gpu"] / 100))
	assert_float(d["max_surface_sdf"]).override_failure_message(
		"a vertex sits %f m (reported) off the surface" % d["max_surface_sdf"]).is_less(0.25)
	# A wholly inverted mesh would put EVERY sampled triangle here; the crease can account
	# for a handful.
	assert_int(d["winding_bad"]).override_failure_message(
		"%d of %d sampled triangles face into the solid"
		% [d["winding_bad"], d["tri_sampled"]]).is_less_equal(int(d["tri_sampled"] / 50))

func test_a_carved_chunk_still_matches() -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	var before: Dictionary = w.debug_mesh_diff(Vector3i(2, 4, 2))
	var hit: Dictionary = w.debug_raycast(Vector3(30.0, 80.0, 30.0), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var r: Dictionary = tool.apply_sphere_subtract(hit["pos"], 3.0)
	assert_array(r["rejected"]).is_empty()

	var chunk := Vector3i(int(floor(30.0 / 12.8)), int(floor(float(hit["pos"].y) / 12.8)),
		int(floor(30.0 / 12.8)))
	var after: Dictionary = w.debug_mesh_diff(chunk)
	check_diff(after, "carved terrain")
	assert_int(after["op_count"]).is_greater(0)
	assert_int(after["tri_gpu"]).is_greater(0)
	assert_int(before["tri_gpu"]).is_greater(0)

func test_open_sky_meshes_to_nothing() -> void:
	var w := make_world()
	# Chunk (2, 5, 2) spans world y [64.0, 76.8); the surface tops out at 51.2 + 10.
	var d: Dictionary = w.debug_mesh_diff(Vector3i(2, 5, 2))
	assert_int(d["tri_gpu"]).is_equal(0)
	assert_int(d["tri_cpu"]).is_equal(0)
	assert_int(d["cells_both"]).is_equal(0)
	assert_int(d["cells_only_gpu"]).is_equal(0)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_mesh_diff.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_mesh_diff'`.

- [ ] **Step 3: Write the cell pass**

Create `shaders/mesh_cells.comp.glsl`. **Every table and every expression here is a mirror of `ve::dual_contour`'s first pass** (`extension/src/mesh/dual_contour.cpp`) — including the order of the 12 edges, because the vertex is a running float sum and float addition is not associative:

```glsl
#[compute]
#version 460

#include "mesh_common.glsl"

// One thread per mesh cell. 129 is not a multiple of 4, so the last group of each axis runs
// partly out of bounds and returns.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, std430) writeonly buffer Cells { int v[]; } cells;
layout(set = 0, binding = 2, std430) writeonly buffer Verts { float v[]; } verts;
// vert count, tri count, overflow bits, pad — four uints per job.
layout(set = 0, binding = 3, std430) buffer Counts { uint v[]; } counts;

layout(push_constant, std430) uniform Push {
	ivec4 chunk;  // xyz = chunk coordinates, w = job index in this batch
	ivec4 params; // x = op count, y = max verts per job, z = max tris per job, w = unused
} pc;

// Cell corners indexed by (x | y<<1 | z<<2); mirror of kCorner in dual_contour.cpp.
const ivec3 CORNER[8] = ivec3[8](ivec3(0, 0, 0), ivec3(1, 0, 0), ivec3(0, 1, 0), ivec3(1, 1, 0),
		ivec3(0, 0, 1), ivec3(1, 0, 1), ivec3(0, 1, 1), ivec3(1, 1, 1));
// The 12 edges as corner pairs, in the SAME order as kEdge in dual_contour.cpp.
const ivec2 EDGE[12] = ivec2[12](ivec2(0, 1), ivec2(2, 3), ivec2(4, 5), ivec2(6, 7),
		ivec2(0, 2), ivec2(1, 3), ivec2(4, 6), ivec2(5, 7),
		ivec2(0, 4), ivec2(1, 5), ivec2(2, 6), ivec2(3, 7));

void main() {
	ivec3 m = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(m, ivec3(CHUNK_MESH_CELLS)))) return;
	int ci = mesh_cell_index(m);
	uint job = uint(pc.chunk.w);

	float d[8];
	for (int k = 0; k < 8; k++) d[k] = decode_sdf(imageLoad(lattice, m + CORNER[k]).r);

	vec3 acc = vec3(0.0);
	int n = 0;
	for (int e = 0; e < 12; e++) {
		float da = d[EDGE[e].x], db = d[EDGE[e].y];
		if ((da <= 0.0) == (db <= 0.0)) continue;
		float t = da / (da - db);
		acc += vec3(CORNER[EDGE[e].x]) + t * vec3(CORNER[EDGE[e].y] - CORNER[EDGE[e].x]);
		n++;
	}
	// Every cell is written every job: the cell map is shared by the batch and is never
	// cleared between jobs, so "no vertex" has to be stored, not left behind.
	if (n == 0) { cells.v[ci] = -1; return; }

	uint idx = atomicAdd(counts.v[job * 4u + 0u], 1u);
	if (idx >= uint(pc.params.y)) {
		// Fail-soft (spec §8): the chunk loses this vertex and the quads that needed it, the
		// overflow bit reaches the CPU with the result, and the collider is built from what
		// did fit. A partial collider beats none.
		atomicOr(counts.v[job * 4u + 2u], 1u);
		cells.v[ci] = -1;
		return;
	}
	vec3 p = vec3(pc.chunk.xyz) * CHUNK_SIZE +
			(vec3(m) - 1.0 + acc / float(n)) * CHUNK_CELL_SIZE;
	uint base = (job * uint(pc.params.y) + idx) * 3u;
	verts.v[base + 0u] = p.x;
	verts.v[base + 1u] = p.y;
	verts.v[base + 2u] = p.z;
	cells.v[ci] = int(idx);
}
```

- [ ] **Step 4: Write the quad pass**

Create `shaders/mesh_quads.comp.glsl` — the mirror of `ve::dual_contour`'s second pass:

```glsl
#[compute]
#version 460

#include "mesh_common.glsl"

// One thread per owned edge coordinate; each handles that point's three axis edges.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, std430) readonly buffer Cells { int v[]; } cells;
layout(set = 0, binding = 2, std430) writeonly buffer Tris { uint v[]; } tris;
layout(set = 0, binding = 3, std430) buffer Counts { uint v[]; } counts;

layout(push_constant, std430) uniform Push {
	ivec4 chunk;
	ivec4 params;
} pc;

// The four cells around a lattice edge, as offsets in the two axes perpendicular to it,
// counter-clockwise seen from +axis; mirror of kQuad in dual_contour.cpp.
const ivec2 QUAD[4] = ivec2[4](ivec2(-1, -1), ivec2(0, -1), ivec2(0, 0), ivec2(-1, 0));

void emit(uint job, int a, int b, int c) {
	uint t = atomicAdd(counts.v[job * 4u + 1u], 1u);
	if (t >= uint(pc.params.z)) { atomicOr(counts.v[job * 4u + 2u], 2u); return; }
	uint base = (job * uint(pc.params.z) + t) * 3u;
	tris.v[base + 0u] = uint(a);
	tris.v[base + 1u] = uint(b);
	tris.v[base + 2u] = uint(c);
}

void main() {
	ivec3 u = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(u, ivec3(CHUNK_CELLS)))) return;
	// This chunk owns the edges whose four cells it holds: local coordinate u in
	// [0, CHUNK_CELLS), lattice index u + 1. Every edge in the world is emitted by exactly
	// one chunk, so chunk borders have neither cracks nor duplicated triangles.
	ivec3 L = u + 1;
	uint job = uint(pc.chunk.w);
	float da = decode_sdf(imageLoad(lattice, L).r);

	for (int axis = 0; axis < 3; axis++) {
		ivec3 e = ivec3(0);
		e[axis] = 1;
		float db = decode_sdf(imageLoad(lattice, L + e).r);
		bool sa = da <= 0.0, sb = db <= 0.0;
		if (sa == sb) continue;
		int b = (axis + 1) % 3, c = (axis + 2) % 3;
		int q[4];
		bool ok = true;
		for (int k = 0; k < 4; k++) {
			ivec3 m = L;
			m[b] += QUAD[k].x;
			m[c] += QUAD[k].y;
			q[k] = cells.v[mesh_cell_index(m)];
			if (q[k] < 0) ok = false;
		}
		// Only reachable when a neighbour lost its vertex to the cap: skip the quad rather
		// than emit an index into nothing.
		if (!ok) continue;
		// (axis, b, c) is a right-handed cycle, so q0..q3 wind counter-clockwise seen from
		// +axis. A solid -> air step along +axis puts the air on the +axis side, which is
		// the side the normal must face.
		if (sa) {
			emit(job, q[0], q[1], q[2]);
			emit(job, q[0], q[2], q[3]);
		} else {
			emit(job, q[0], q[2], q[1]);
			emit(job, q[0], q[3], q[2]);
		}
	}
}
```

- [ ] **Step 5: Extend MeshPass**

Add to `extension/src/render/mesh_pass.h`:

```cpp
	// Meshes one chunk inline (record, submit, sync, read back). Diagnostic only — the
	// streaming path never stalls like this. `lattice` and `cell_vertex` are optional and
	// exist for the differential test.
	bool mesh_sync(const MeshJob &job, MeshResult *out, std::vector<uint8_t> *lattice,
			std::vector<int32_t> *cell_vertex);
```

```cpp
	// private
	void record_job(int64_t list, const MeshJob &job, int job_index);
	void record_cells(int64_t list, const MeshJob &job, int job_index);
	void record_quads(int64_t list, const MeshJob &job, int job_index);
	void read_job(int job_index, ve::IVec3 chunk, MeshResult *out);
	void reset_counts();

	RID cells_shader_, cells_pipeline_, cells_uset_;
	RID quads_shader_, quads_pipeline_, quads_uset_;
```

Add to `initialize()`, after the field pipeline:

```cpp
	if (!build(rd, "res://shaders/mesh_cells.comp.glsl", &cells_shader_, &cells_pipeline_) ||
			!build(rd, "res://shaders/mesh_quads.comp.glsl", &quads_shader_, &quads_pipeline_)) {
		teardown();
		return false;
	}
	cells_uset_ = rd->uniform_set_create(Array::make(image(0, lattice_), storage(1, cells_),
			storage(2, verts_), storage(3, counts_)), cells_shader_, 0);
	quads_uset_ = rd->uniform_set_create(Array::make(image(0, lattice_), storage(1, cells_),
			storage(2, tris_), storage(3, counts_)), quads_shader_, 0);
	if (!cells_uset_.is_valid() || !quads_uset_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: uniform set creation failed");
		teardown();
		return false;
	}
```

and to `teardown()`, before the field RIDs (uniform sets first, then pipelines, then shaders):

```cpp
	free_if_valid(rd_, quads_uset_);
	free_if_valid(rd_, quads_pipeline_);
	free_if_valid(rd_, quads_shader_);
	free_if_valid(rd_, cells_uset_);
	free_if_valid(rd_, cells_pipeline_);
	free_if_valid(rd_, cells_shader_);
```

Then the new methods in `extension/src/render/mesh_pass.cpp`:

```cpp
void MeshPass::record_cells(int64_t list, const MeshJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, cells_pipeline_);
	rd_->compute_list_bind_uniform_set(list, cells_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kChunkMeshCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

void MeshPass::record_quads(int64_t list, const MeshJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, quads_pipeline_);
	rd_->compute_list_bind_uniform_set(list, quads_uset_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kChunkCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

// The three passes are strictly sequential, and so are the jobs in a batch: they share one
// lattice volume and one cell map. The barriers are what makes that safe — and what makes a
// batch cost three barriers per chunk rather than three buffers per chunk.
void MeshPass::record_job(int64_t list, const MeshJob &job, int job_index) {
	record_field(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_cells(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_quads(list, job, job_index);
	rd_->compute_list_add_barrier(list);
}

void MeshPass::reset_counts() {
	// Device-level, so it must precede compute_list_begin. One update covers the whole batch:
	// every job writes only its own four uints.
	rd_->buffer_update(counts_, 0, static_cast<uint32_t>(cfg_.max_jobs) * 16,
			zeroed(static_cast<int64_t>(cfg_.max_jobs) * 16));
}

void MeshPass::read_job(int job_index, ve::IVec3 chunk, MeshResult *out) {
	out->chunk = chunk;
	out->positions.clear();
	out->indices.clear();
	out->overflow = false;
	const PackedByteArray cb =
			rd_->buffer_get_data(counts_, static_cast<uint32_t>(job_index) * 16, 16);
	if (cb.size() < 16) return;
	const uint32_t *c = reinterpret_cast<const uint32_t *>(cb.ptr());
	// The counters are raw atomic totals: they run past the cap when it is hit.
	const int vcount = std::min<int>(static_cast<int>(c[0]), cfg_.max_verts);
	const int tcount = std::min<int>(static_cast<int>(c[1]), cfg_.max_tris);
	out->overflow = c[2] != 0u;
	if (vcount > 0) {
		const PackedByteArray vb = rd_->buffer_get_data(verts_,
				static_cast<uint32_t>(job_index) * cfg_.max_verts * 12,
				static_cast<uint32_t>(vcount) * 12);
		out->positions.resize(static_cast<size_t>(vcount) * 3);
		std::memcpy(out->positions.data(), vb.ptr(), static_cast<size_t>(vcount) * 12);
	}
	if (tcount > 0) {
		const PackedByteArray tb = rd_->buffer_get_data(tris_,
				static_cast<uint32_t>(job_index) * cfg_.max_tris * 12,
				static_cast<uint32_t>(tcount) * 12);
		out->indices.resize(static_cast<size_t>(tcount) * 3);
		std::memcpy(out->indices.data(), tb.ptr(), static_cast<size_t>(tcount) * 12);
	}
}

bool MeshPass::mesh_sync(const MeshJob &job, MeshResult *out, std::vector<uint8_t> *lattice,
		std::vector<int32_t> *cell_vertex) {
	if (!is_valid()) return false;
	reset_counts();
	upload_ops(job, 0);
	const int64_t list = rd_->compute_list_begin();
	record_job(list, job, 0);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();
	if (out) read_job(0, job.chunk, out);
	if (lattice) {
		const PackedByteArray data = rd_->texture_get_data(lattice_, 0);
		if (data.size() < ve::kChunkLatticeCount) return false;
		lattice->assign(data.ptr(), data.ptr() + ve::kChunkLatticeCount);
	}
	if (cell_vertex) {
		const PackedByteArray data = rd_->buffer_get_data(cells_, 0,
				static_cast<uint32_t>(ve::kChunkCellCount) * 4);
		if (data.size() < static_cast<int64_t>(ve::kChunkCellCount) * 4) return false;
		cell_vertex->resize(ve::kChunkCellCount);
		std::memcpy(cell_vertex->data(), data.ptr(),
				static_cast<size_t>(ve::kChunkCellCount) * 4);
	}
	return true;
}
```

`run_field_sync` keeps working unchanged — it stays as the narrower diagnostic that Task 4's test uses.

- [ ] **Step 6: Write the differential hook**

Add `Dictionary debug_mesh_diff(Vector3i chunk);` to `voxel_world.h`, bind it, and implement in `voxel_world.cpp` (add `#include <array>` for the canonical triple and `#include <iterator>` for `std::back_inserter`):

```cpp
Dictionary VoxelWorld::debug_mesh_diff(Vector3i chunk) {
	Dictionary d;
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_pass_) return d;
	const ve::IVec3 c{chunk.x, chunk.y, chunk.z};
	std::vector<ve::EditOp> ops;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ops = edit_log_->ops(ve::region_of_chunk(c));
	}
	const MeshJob job{c, ops.data(), static_cast<int>(ops.size())};
	MeshResult gpu;
	std::vector<uint8_t> lattice;
	std::vector<int32_t> gpu_cells;
	if (!mesh_pass_->mesh_sync(job, &gpu, &lattice, &gpu_cells)) return d;

	const ve::DcGrid g = ve::chunk_dc_grid(c);
	ve::AnalyticGenerator gen;

	// 1. The lattice against the CPU field. One encoded step of sin() drift is invisible.
	int lat_max = 0, lat_over = 0;
	for (int z = 0; z < g.lattice; z++)
		for (int y = 0; y < g.lattice; y++)
			for (int x = 0; x < g.lattice; x++) {
				const float s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
						g.origin[0] + (x - 1) * g.cell_size, g.origin[1] + (y - 1) * g.cell_size,
						g.origin[2] + (z - 1) * g.cell_size).sdf;
				const int diff = std::abs(static_cast<int>(lattice[ve::dc_lattice_index(g, x, y, z)]) -
						static_cast<int>(ve::encode_sdf(s)));
				lat_max = std::max(lat_max, diff);
				if (diff > 1) lat_over++;
			}
	d["lattice_max_diff"] = lat_max;
	d["lattice_diff_over_one"] = lat_over;
	d["op_count"] = static_cast<int>(ops.size());
	d["overflow"] = gpu.overflow;

	// 2. The mesh against ve::dual_contour run on the GPU's OWN lattice, so the two sides
	//    consume identical bytes and any difference is the algorithm drifting.
	ve::MeshBuffer ref;
	ve::dual_contour(lattice.data(), g, &ref);
	const int gpu_verts = static_cast<int>(gpu.positions.size() / 3);

	int both = 0, only_cpu = 0, only_gpu = 0;
	float max_pos = 0.0f;
	for (int i = 0; i < static_cast<int>(ref.cell_vertex.size()); i++) {
		const int32_t a = ref.cell_vertex[i];
		const int32_t b = gpu_cells[i];
		if (a >= 0 && b >= 0 && b < gpu_verts) {
			both++;
			for (int k = 0; k < 3; k++)
				max_pos = std::max(max_pos, std::fabs(ref.positions[a * 3 + k] -
						gpu.positions[b * 3 + k]));
		} else if (a >= 0) {
			only_cpu++;
		} else if (b >= 0) {
			only_gpu++;
		}
	}
	d["cells_cpu"] = ref.vertex_count();
	d["cells_gpu"] = gpu_verts;
	d["cells_both"] = both;
	d["cells_only_cpu"] = only_cpu;
	d["cells_only_gpu"] = only_gpu;
	d["max_pos_diff"] = max_pos;

	// 3. Triangles as cyclically normalised CELL triples: the GPU numbers its vertices with
	//    atomics in no fixed order, but the cells they belong to are fixed, and keeping the
	//    cycle (rather than sorting the three) means an inverted winding still differs.
	std::vector<int32_t> cpu_v2c(ref.vertex_count(), -1), gpu_v2c(gpu_verts, -1);
	for (int i = 0; i < static_cast<int>(ref.cell_vertex.size()); i++) {
		if (ref.cell_vertex[i] >= 0) cpu_v2c[ref.cell_vertex[i]] = i;
		if (gpu_cells[i] >= 0 && gpu_cells[i] < gpu_verts) gpu_v2c[gpu_cells[i]] = i;
	}
	const auto canonical = [](const std::vector<uint32_t> &idx, const std::vector<int32_t> &v2c) {
		std::vector<std::array<int, 3>> out;
		out.reserve(idx.size() / 3);
		for (size_t t = 0; t + 2 < idx.size(); t += 3) {
			int cell[3];
			bool ok = true;
			for (int k = 0; k < 3; k++) {
				const uint32_t v = idx[t + k];
				if (v >= v2c.size()) { ok = false; break; }
				cell[k] = v2c[v];
			}
			if (!ok) continue;
			int r = 0;
			if (cell[1] < cell[r]) r = 1;
			if (cell[2] < cell[r]) r = 2;
			out.push_back({cell[r], cell[(r + 1) % 3], cell[(r + 2) % 3]});
		}
		std::sort(out.begin(), out.end());
		return out;
	};
	const std::vector<std::array<int, 3>> cpu_tris = canonical(ref.indices, cpu_v2c);
	const std::vector<std::array<int, 3>> gpu_tris = canonical(gpu.indices, gpu_v2c);
	std::vector<std::array<int, 3>> diff_a, diff_b;
	std::set_difference(cpu_tris.begin(), cpu_tris.end(), gpu_tris.begin(), gpu_tris.end(),
			std::back_inserter(diff_a));
	std::set_difference(gpu_tris.begin(), gpu_tris.end(), cpu_tris.begin(), cpu_tris.end(),
			std::back_inserter(diff_b));
	d["tri_cpu"] = static_cast<int>(cpu_tris.size());
	d["tri_gpu"] = static_cast<int>(gpu_tris.size());
	d["tri_only_cpu"] = static_cast<int>(diff_a.size());
	d["tri_only_gpu"] = static_cast<int>(diff_b.size());

	// 4. Two properties nothing above can prove, checked against the field itself: every
	//    vertex sits on the surface, and every triangle's normal points at the air.
	float max_sdf = 0.0f;
	int off_10cm = 0;
	int winding_bad = 0, tri_sampled = 0;
	const int tri_count = static_cast<int>(gpu.indices.size() / 3);
	const int stride = std::max(1, tri_count / 512); // a spread sample, not the first 512
	for (int v = 0; v < gpu_verts; v++) {
		const float s = std::fabs(ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
				gpu.positions[v * 3], gpu.positions[v * 3 + 1], gpu.positions[v * 3 + 2]).sdf);
		max_sdf = std::max(max_sdf, s);
		if (s > 0.1f) off_10cm++;
	}
	for (int t = 0; t < tri_count; t += stride) {
		const uint32_t i0 = gpu.indices[t * 3], i1 = gpu.indices[t * 3 + 1],
				i2 = gpu.indices[t * 3 + 2];
		if (i0 >= static_cast<uint32_t>(gpu_verts) || i1 >= static_cast<uint32_t>(gpu_verts) ||
				i2 >= static_cast<uint32_t>(gpu_verts))
			continue;
		const Vector3 p0(gpu.positions[i0 * 3], gpu.positions[i0 * 3 + 1], gpu.positions[i0 * 3 + 2]);
		const Vector3 p1(gpu.positions[i1 * 3], gpu.positions[i1 * 3 + 1], gpu.positions[i1 * 3 + 2]);
		const Vector3 p2(gpu.positions[i2 * 3], gpu.positions[i2 * 3 + 1], gpu.positions[i2 * 3 + 2]);
		const Vector3 n = (p1 - p0).cross(p2 - p0);
		if (n.length_squared() <= 0.0f) continue; // degenerate: carries no orientation
		const Vector3 mid = (p0 + p1 + p2) / 3.0f;
		// 2 cm: far enough out of the quantisation noise, short enough that the probe cannot
		// step clean through a thin feature and read solid on both sides.
		const Vector3 step = n.normalized() * 0.02f;
		const float out_side = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
				mid.x + step.x, mid.y + step.y, mid.z + step.z).sdf;
		const float in_side = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
				mid.x - step.x, mid.y - step.y, mid.z - step.z).sdf;
		tri_sampled++;
		if (out_side <= in_side) winding_bad++;
	}
	d["max_surface_sdf"] = max_sdf;
	d["verts_off_10cm"] = off_10cm;
	d["winding_bad"] = winding_bad;
	d["tri_sampled"] = tri_sampled;
	return d;
}
```

- [ ] **Step 7: Build and run the test**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_mesh_diff.gd`
Expected: PASS, four cases.

Failure map, in the order they are worth checking:
- `cells_only_gpu > 0` with `cells_only_cpu == 0` → the shader lost vertices to the cap; check `overflow`.
- Both cell counts differ everywhere → the lattice indexing or the `- 1.0` overlap offset disagrees between `mesh_common.glsl` and `ve::DcGrid`.
- Cells match but `tri_only_*` are both large and equal → the winding is inverted; the fix is one swap, applied to **both** `mesh_quads.comp.glsl` and `dual_contour.cpp`.
- `winding_bad == tri_count` → same, and `test_dual_contour.cpp`'s sphere case would have caught it first.

- [ ] **Step 8: Run the whole suite**

Run: `cd extension && scons test -j$(nproc)` then
`./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: all green.

- [ ] **Step 9: Commit**

```bash
git add shaders/mesh_cells.comp.glsl shaders/mesh_quads.comp.glsl \
        extension/src/render/mesh_pass.h extension/src/render/mesh_pass.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_mesh_diff.gd
git commit -m "feat: GPU dual contouring with a strict CPU differential test"
```

---

### Task 6: the batch pipeline — submit one frame, collect the next

`mesh_sync` stalls by design. The streaming path must not, so this task adds the one-batch-in-flight pipeline spec §6 calls "async double-buffered readback": the batch submitted at the end of frame *N* is synced and read at the top of frame *N+1*, by which time the GPU has long finished it.

**Files:**
- Modify: `extension/src/render/mesh_pass.h`, `extension/src/render/mesh_pass.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_mesh_stream.gd`

**Interfaces:**
- Produces:
  - `bool MeshPass::submit(const MeshJob *jobs, int count)` — false if a batch is already in flight
  - `bool MeshPass::in_flight() const`
  - `int MeshPass::collect(std::vector<MeshResult> *out)` — syncs, reads back, appends, returns the count
  - `float MeshPass::last_collect_ms() const`
  - `bool VoxelWorld::debug_mesh_submit(Array chunks)`, `Array VoxelWorld::debug_mesh_collect()`

- [ ] **Step 1: Write the failing test**

Create `tests/test_mesh_stream.gd`:

```gdscript
extends GdUnitTestSuite

# The mesher's pipeline contract: one batch in flight, submitted now and collected later, so
# no frame ever waits on the GPU. Everything here runs on the mesher's own local device — the
# renderer's device is not even initialised.

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.mesh_jobs_per_frame = 2
	add_child(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func test_a_batch_is_submitted_once_and_collected_once() -> void:
	var w := make_world()
	assert_bool(w.debug_mesh_submit([Vector3i(2, 4, 2), Vector3i(3, 4, 2)])).is_true()
	# A second batch cannot start while one is in flight.
	assert_bool(w.debug_mesh_submit([Vector3i(4, 4, 2)])).is_false()

	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(2)
	assert_object(got[0]["chunk"]).is_equal(Vector3i(2, 4, 2))
	assert_object(got[1]["chunk"]).is_equal(Vector3i(3, 4, 2))
	for r in got:
		assert_int(r["triangles"]).is_greater(1000)
		assert_int(r["vertices"]).is_greater(500)
		assert_bool(r["overflow"]).is_false()

	# Nothing is left to collect, and the pass is free again.
	assert_int(w.debug_mesh_collect().size()).is_equal(0)
	assert_bool(w.debug_mesh_submit([Vector3i(4, 4, 2)])).is_true()

func test_a_batch_agrees_with_the_synchronous_path() -> void:
	var w := make_world()
	var one: Dictionary = w.debug_mesh_diff(Vector3i(2, 4, 2))
	assert_bool(w.debug_mesh_submit([Vector3i(2, 4, 2)])).is_true()
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(1)
	# The batch path shares every buffer and every dispatch with the inline path; the counts
	# must be identical, or a job's state is leaking between the two.
	assert_int(got[0]["triangles"]).is_equal(one["tri_gpu"])
	assert_int(got[0]["vertices"]).is_equal(one["cells_gpu"])

func test_jobs_in_one_batch_do_not_leak_into_each_other() -> void:
	var w := make_world()
	# Chunk (2, 5, 2) is open sky and meshes to nothing; batching it with a surface chunk
	# must not give it the other job's triangles (they share one lattice and one cell map).
	assert_bool(w.debug_mesh_submit([Vector3i(2, 4, 2), Vector3i(2, 5, 2)])).is_true()
	var got: Array = w.debug_mesh_collect()
	assert_int(got.size()).is_equal(2)
	assert_int(got[0]["triangles"]).is_greater(1000)
	assert_int(got[1]["triangles"]).is_equal(0)
	assert_int(got[1]["vertices"]).is_equal(0)

func test_an_oversized_batch_is_refused() -> void:
	var w := make_world()
	assert_bool(w.debug_mesh_submit(
		[Vector3i(2, 4, 2), Vector3i(3, 4, 2), Vector3i(4, 4, 2)])).is_false()
	assert_int(w.debug_mesh_collect().size()).is_equal(0)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_mesh_stream.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_mesh_submit'`.

- [ ] **Step 3: Add the pipeline to MeshPass**

Header additions:

```cpp
	// Records and submits one batch; false when a batch is still in flight, the count is
	// zero, or it exceeds config().max_jobs.
	bool submit(const MeshJob *jobs, int count);
	bool in_flight() const { return in_flight_; }
	// Syncs the batch in flight, reads it back, appends to `out`, returns how many. Zero
	// when nothing is in flight. The sync is for work submitted a frame ago, so it does not
	// wait on the GPU in practice.
	int collect(std::vector<MeshResult> *out);
	float last_collect_ms() const { return last_collect_ms_; }
```

```cpp
	// private
	bool in_flight_ = false;
	std::vector<ve::IVec3> batch_; // the chunks in flight, in job order
	float last_collect_ms_ = 0.0f;
```

Implementation (add `#include <godot_cpp/classes/time.hpp>`):

```cpp
bool MeshPass::submit(const MeshJob *jobs, int count) {
	if (!is_valid() || in_flight_ || !jobs || count <= 0 || count > cfg_.max_jobs) return false;
	reset_counts();
	for (int j = 0; j < count; j++) upload_ops(jobs[j], j);
	const int64_t list = rd_->compute_list_begin();
	for (int j = 0; j < count; j++) record_job(list, jobs[j], j);
	rd_->compute_list_end();
	rd_->submit();
	in_flight_ = true;
	batch_.clear();
	for (int j = 0; j < count; j++) batch_.push_back(jobs[j].chunk);
	return true;
}

int MeshPass::collect(std::vector<MeshResult> *out) {
	if (!in_flight_) return 0;
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	rd_->sync();
	in_flight_ = false;
	const int n = static_cast<int>(batch_.size());
	for (int j = 0; j < n; j++) {
		MeshResult r;
		read_job(j, batch_[j], &r);
		if (out) out->push_back(std::move(r));
	}
	batch_.clear();
	last_collect_ms_ =
			static_cast<float>(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
	return n;
}
```

`teardown()` gains a first line, because freeing buffers the device is still working through is
undefined:

```cpp
void MeshPass::teardown() {
	if (!rd_) return;
	if (in_flight_) {
		rd_->sync();
		in_flight_ = false;
		batch_.clear();
	}
	// ...then the free_if_valid sequence from Tasks 4 and 5, unchanged: the three uniform
	// sets, the three pipelines, the three shaders, then the six buffers and the volume.
}
```

`mesh_sync()` gains the same guard at the top, since it submits on the same device:

```cpp
	if (!is_valid() || in_flight_) return false;
```

- [ ] **Step 4: Add the debug hooks**

`voxel_world.h`: `bool debug_mesh_submit(Array chunks);` and `Array debug_mesh_collect();`, both bound in `_bind_methods()`.

```cpp
bool VoxelWorld::debug_mesh_submit(Array chunks) {
	ensure_physics_initialized();
	if (!physics_ready_ || !mesh_pass_) return false;
	std::vector<ve::IVec3> coords;
	std::vector<std::vector<ve::EditOp>> ops;
	for (int i = 0; i < chunks.size(); i++) {
		const Vector3i v = chunks[i];
		coords.push_back({v.x, v.y, v.z});
	}
	ops.reserve(coords.size());
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		for (const ve::IVec3 &c : coords) ops.push_back(edit_log_->ops(ve::region_of_chunk(c)));
	}
	std::vector<MeshJob> jobs;
	jobs.reserve(coords.size());
	for (size_t i = 0; i < coords.size(); i++)
		jobs.push_back({coords[i], ops[i].data(), static_cast<int>(ops[i].size())});
	return mesh_pass_->submit(jobs.data(), static_cast<int>(jobs.size()));
}

Array VoxelWorld::debug_mesh_collect() {
	Array out;
	if (!physics_ready_ || !mesh_pass_) return out;
	std::vector<MeshResult> results;
	mesh_pass_->collect(&results);
	for (const MeshResult &r : results) {
		Dictionary d;
		d["chunk"] = Vector3i(r.chunk.x, r.chunk.y, r.chunk.z);
		d["vertices"] = static_cast<int>(r.positions.size() / 3);
		d["triangles"] = static_cast<int>(r.indices.size() / 3);
		d["overflow"] = r.overflow;
		out.push_back(d);
	}
	return out;
}
```

`ops` must be fully populated before any `.data()` pointer is taken — `reserve` plus the
separate loop is what keeps a reallocation from invalidating a job's pointer.

- [ ] **Step 5: Build and run the test**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_mesh_stream.gd`
Expected: PASS, four cases.

`test_jobs_in_one_batch_do_not_leak_into_each_other` is the one that matters most: it is the
only check that the shared lattice and cell map are properly fenced between jobs. If it fails,
a barrier in `record_job` is missing or `mesh_cells.comp.glsl` stopped writing `-1` for empty
cells.

- [ ] **Step 6: Commit**

```bash
git add extension/src/render/mesh_pass.h extension/src/render/mesh_pass.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_mesh_stream.gd
git commit -m "feat: one-batch-in-flight mesh submit and collect"
```

---

### Task 7: `physics/collider_streamer` — meshes become Jolt bodies

The Godot glue: a pool of server-created static bodies, one concave shape each, kept in step with the player by `ve::ChunkResidency` and fed by `MeshPass`. This is the task that first makes the terrain solid.

**Files:**
- Create: `extension/src/physics/collider_streamer.h`, `extension/src/physics/collider_streamer.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_collider_stream.gd`

**Interfaces:**
- Consumes: `ve::ChunkResidency`, `ve::ChunkPlan`, `ve::ChunkProbe`, `ve::chunk_has_surface`, `ve::region_of_chunk`; `godot::MeshPass`, `godot::MeshJob`, `godot::MeshResult`; `ve::EditLog`.
- Produces:
  - `class godot::ColliderStreamer` with `initialize(ve::ChunkResidency *, ve::EditLog *, std::mutex *, MeshPass *, int max_slots)`, `teardown()`, `set_space(RID)`, `set_shape_builds_per_frame(int)`, `int run_frame(float, float, float)`, `active_bodies()`, `builds_last_frame()`, `failures()`, `queued_results()`, `last_build_ms()`, `last_collect_ms()`, `RID body_of_slot(int)`
  - `int VoxelWorld::physics_tick(Vector3 center)`, `int VoxelWorld::debug_physics_frame(Vector3 center)`, `Dictionary VoxelWorld::debug_physics_stats()`, `RID VoxelWorld::debug_body_of_chunk(Vector3i chunk)`

- [ ] **Step 1: Write the failing test**

Create `tests/test_collider_stream.gd`:

```gdscript
extends GdUnitTestSuite

# Collision streaming (spec section 6): dual-contoured chunks become Jolt concave shapes on
# server-created static bodies, in a ball around the player, with no scene-tree nodes.
#
# The physics ray is checked against ve::raycast on the analytic field — the same oracle
# test_edit_pipeline.gd uses for the renderer. If the two agree to a few centimetres, the
# collision the player walks on IS the terrain they can see.

const CENTER := Vector3(60.0, 55.0, 60.0)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false        # no auto tick: these tests step it by hand
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.physics_radius_m = 25.0        # a handful of chunks, not the shipping 160
	w.max_collider_chunks = 64
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	add_child(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

# Settled means several consecutive frames with nothing to do: one quiet frame is only the
# gap between submitting a batch and collecting it.
func settle(w: VoxelWorld, center: Vector3, frames := 400) -> void:
	var quiet := 0
	for i in range(frames):
		quiet = quiet + 1 if w.debug_physics_frame(center) == 0 else 0
		if quiet >= 4:
			return

func ray(from: Vector3, to: Vector3) -> Dictionary:
	var state := get_tree().root.world_3d.direct_space_state
	return state.intersect_ray(PhysicsRayQueryParameters3D.create(from, to))

func test_a_server_built_concave_shape_is_hit_by_a_ray(timeout := 20000) -> void:
	# Pins the shape_set_data contract the streamer depends on. Godot's own
	# ConcavePolygonShape3D sends {"faces": PackedVector3Array, "backface_collision": bool},
	# and the winding decides which side collides — exactly the two things that would make
	# the streamer silently produce nothing.
	var shape := PhysicsServer3D.concave_polygon_shape_create()
	var faces := PackedVector3Array([
		Vector3(-1, 0, -1), Vector3(-1, 0, 1), Vector3(1, 0, 1),
		Vector3(-1, 0, -1), Vector3(1, 0, 1), Vector3(1, 0, -1)])
	PhysicsServer3D.shape_set_data(shape, {"faces": faces, "backface_collision": false})
	var body := PhysicsServer3D.body_create()
	PhysicsServer3D.body_set_mode(body, PhysicsServer3D.BODY_MODE_STATIC)
	PhysicsServer3D.body_add_shape(body, shape)
	PhysicsServer3D.body_set_space(body, get_tree().root.world_3d.space)
	await get_tree().physics_frame
	var hit := ray(Vector3(0, 5, 0), Vector3(0, -5, 0))
	assert_bool(hit.is_empty()).is_false()
	assert_float(hit["position"].y).is_equal_approx(0.0, 0.01)
	PhysicsServer3D.free_rid(body)
	PhysicsServer3D.free_rid(shape)

func test_colliders_appear_around_the_player(timeout := 60000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var st: Dictionary = w.debug_physics_stats()
	assert_int(st["chunks_resident"]).is_greater(3)
	assert_int(st["chunks_pending"]).is_equal(0)
	assert_int(st["bodies"]).is_greater(3)
	assert_int(st["failures"]).is_equal(0)
	# Every resident chunk is inside the radius, so the pool never filled up here.
	assert_int(st["chunks_resident"]).is_less_equal(64)

func test_a_physics_ray_lands_on_the_analytic_surface(timeout := 60000) -> void:
	var w := make_world()
	settle(w, CENTER)
	await get_tree().physics_frame

	var oracle: Dictionary = w.debug_raycast(Vector3(CENTER.x, 80.0, CENTER.z), Vector3(0, -1, 0))
	assert_bool(oracle["hit"]).is_true()
	var hit := ray(Vector3(CENTER.x, 80.0, CENTER.z), Vector3(CENTER.x, 20.0, CENTER.z))
	assert_bool(hit.is_empty()).override_failure_message(
		"the physics ray found no collider under the player").is_false()
	# 0.1 m cells plus the mesher's own error: a few centimetres, never a different surface.
	assert_float(hit["position"].y).is_equal_approx(oracle["pos"].y, 0.15)
	# The surface faces up, or a character would fall through it.
	assert_float(hit["normal"].y).is_greater(0.3)

func test_walking_away_releases_the_far_colliders(timeout := 90000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var before: Dictionary = w.debug_physics_stats()
	assert_int(before["bodies"]).is_greater(3)

	var far := CENTER + Vector3(80.0, 0.0, 0.0)
	settle(w, far)
	await get_tree().physics_frame
	var after: Dictionary = w.debug_physics_stats()
	# The set is bounded by the radius, not by how far the player has walked.
	assert_int(after["bodies"]).is_less_equal(before["bodies"] + 4)
	# ...and the ground the player left is no longer collidable.
	assert_bool(ray(Vector3(CENTER.x, 80.0, CENTER.z),
		Vector3(CENTER.x, 20.0, CENTER.z)).is_empty()).is_true()
	# ...while the ground they arrived on is.
	assert_bool(ray(Vector3(far.x, 80.0, far.z), Vector3(far.x, 20.0, far.z)).is_empty()).is_false()

func test_an_empty_chunk_costs_no_body_and_is_not_retried(timeout := 60000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var st: Dictionary = w.debug_physics_stats()
	# The probe is conservative, so some resident candidates mesh to nothing; those release
	# their slot instead of holding a body, and the cached verdict stops them coming back.
	assert_int(st["bodies"]).is_less_equal(st["chunks_resident"])
	for i in range(20):
		assert_int(w.debug_physics_frame(CENTER)).is_equal(0)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_collider_stream.gd`
Expected: the first case PASSES (it uses only Godot's own API — that is the point of it), the rest FAIL with `Invalid call. Nonexistent function 'debug_physics_frame'`.

- [ ] **Step 3: Write the ColliderStreamer header**

Create `extension/src/physics/collider_streamer.h`:

```cpp
#pragma once
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <mutex>
#include <vector>
#include "generator/generator.h"
#include "mesh/chunk_residency.h"
#include "render/mesh_pass.h"
#include "world/edit_log.h"

namespace godot {

// Turns finished chunk meshes into Jolt static bodies and keeps the set in step with the
// player (spec §6: "PhysicsServer3D direct (no scene-tree nodes) … collision streams in a
// ~64 m radius around the player"). Owns the physics RIDs and nothing else; every pointer is
// borrowed from VoxelWorld.
//
// One single-shape static body per pool slot rather than one body carrying every chunk's
// shape: Jolt rebuilds a body's compound whenever a sub-shape changes, so a 160-shape body
// would rebuild itself two or three times a second while streaming, and body_remove_shape
// renumbers everything after it. See the plan's Deliberate Decisions.
//
// Main thread only, like PhysicsServer3D itself and like the residency it drives. The render
// thread's WorldStreamer touches none of this.
class ColliderStreamer {
public:
	~ColliderStreamer();

	void initialize(ve::ChunkResidency *chunks, ve::EditLog *edit_log, std::mutex *edit_mutex,
			MeshPass *mesh, int max_slots);
	void teardown();
	void set_space(RID space);
	void set_shape_builds_per_frame(int v) { max_builds_per_frame_ = v; }

	// One frame of collider maintenance around the given centre: land finished meshes, plan,
	// release what left the ball, submit the next batch. Returns the number of actions taken,
	// so a caller (or a test) can tell a settled world from a busy one.
	int run_frame(float cx, float cy, float cz);

	int active_bodies() const { return active_bodies_; }
	int builds_last_frame() const { return builds_last_frame_; }
	int failures() const { return failures_; }
	int queued_results() const { return static_cast<int>(inbox_.size()); }
	float last_build_ms() const { return last_build_ms_; }
	float last_collect_ms() const;
	RID body_of_slot(int slot) const;

private:
	enum BuildOutcome { kBuilt, kEmpty, kFailed };

	BuildOutcome build_shape(int slot, const MeshResult &r);
	void release_slot(int slot);
	void apply_result(const MeshResult &r);

	ve::ChunkResidency *chunks_ = nullptr;
	ve::EditLog *edit_log_ = nullptr;
	std::mutex *edit_mutex_ = nullptr;
	MeshPass *mesh_ = nullptr;
	// Spec §9 defers a configurable generator; when G becomes one, this moves to VoxelWorld
	// and is handed in, exactly like the edit log.
	ve::AnalyticGenerator gen_;

	RID space_;
	std::vector<RID> bodies_;
	std::vector<RID> shapes_;
	std::vector<char> in_space_;
	std::vector<MeshResult> inbox_; // collected, not yet turned into shapes
	int max_builds_per_frame_ = 2;
	int active_bodies_ = 0;
	int builds_last_frame_ = 0;
	int failures_ = 0;
	int overflow_warnings_ = 0;
	float last_build_ms_ = 0.0f;
};

} // namespace godot
```

- [ ] **Step 4: Write the ColliderStreamer implementation**

Create `extension/src/physics/collider_streamer.cpp`:

```cpp
#include "physics/collider_streamer.h"
#include "mesh/mesh_chunk.h"
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

using namespace godot;

namespace {

// The residency's view of the world field. NOTE the qualification on ve::chunk_has_surface:
// unqualified, the name would resolve to this override and recurse for ever.
struct LogProbe : ve::ChunkProbe {
	const ve::Generator *gen = nullptr;
	ve::EditLog *log = nullptr;
	std::mutex *mu = nullptr;

	bool chunk_has_surface(ve::IVec3 c) const override {
		std::lock_guard<std::mutex> lock(*mu);
		const std::vector<ve::EditOp> &ops = log->ops(ve::region_of_chunk(c));
		return ve::chunk_has_surface(*gen, ops.data(), static_cast<int>(ops.size()), c);
	}
};

} // namespace

ColliderStreamer::~ColliderStreamer() {
	teardown();
}

void ColliderStreamer::initialize(ve::ChunkResidency *chunks, ve::EditLog *edit_log,
		std::mutex *edit_mutex, MeshPass *mesh, int max_slots) {
	teardown();
	chunks_ = chunks;
	edit_log_ = edit_log;
	edit_mutex_ = edit_mutex;
	mesh_ = mesh;
	bodies_.assign(static_cast<size_t>(std::max(0, max_slots)), RID());
	shapes_.assign(static_cast<size_t>(std::max(0, max_slots)), RID());
	in_space_.assign(static_cast<size_t>(std::max(0, max_slots)), 0);
}

void ColliderStreamer::teardown() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (ps) {
		for (size_t i = 0; i < bodies_.size(); i++) {
			if (bodies_[i].is_valid()) {
				ps->body_set_space(bodies_[i], RID());
				ps->free_rid(bodies_[i]);
			}
			if (shapes_[i].is_valid()) ps->free_rid(shapes_[i]);
		}
	}
	bodies_.clear();
	shapes_.clear();
	in_space_.clear();
	inbox_.clear();
	active_bodies_ = 0;
	chunks_ = nullptr;
	edit_log_ = nullptr;
	edit_mutex_ = nullptr;
	mesh_ = nullptr;
}

void ColliderStreamer::set_space(RID space) {
	if (space == space_) return;
	space_ = space;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps) return;
	// The space arrives once the node is in the tree, which can be after the first bodies
	// exist; re-home whatever is already live.
	for (size_t i = 0; i < bodies_.size(); i++)
		if (in_space_[i] && bodies_[i].is_valid()) ps->body_set_space(bodies_[i], space_);
}

float ColliderStreamer::last_collect_ms() const {
	return mesh_ ? mesh_->last_collect_ms() : 0.0f;
}

RID ColliderStreamer::body_of_slot(int slot) const {
	return slot >= 0 && slot < static_cast<int>(bodies_.size()) ? bodies_[slot] : RID();
}

ColliderStreamer::BuildOutcome ColliderStreamer::build_shape(int slot, const MeshResult &r) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || slot < 0 || slot >= static_cast<int>(bodies_.size())) return kFailed;
	const int verts = static_cast<int>(r.positions.size() / 3);

	// ConcavePolygonShape3D takes a de-indexed triangle soup; Godot's own resource sends
	// exactly this dictionary (scene/resources/3d/concave_polygon_shape_3d.cpp).
	PackedVector3Array faces;
	faces.resize(static_cast<int64_t>(r.indices.size()));
	Vector3 *fw = faces.ptrw();
	int n = 0;
	for (size_t t = 0; t + 2 < r.indices.size(); t += 3) {
		Vector3 v[3];
		bool ok = true;
		for (int k = 0; k < 3; k++) {
			const uint32_t vi = r.indices[t + k];
			if (static_cast<int>(vi) >= verts) { ok = false; break; }
			v[k] = Vector3(r.positions[vi * 3], r.positions[vi * 3 + 1], r.positions[vi * 3 + 2]);
		}
		if (!ok) continue;
		// Two dual vertices can coincide to float precision on a flat run of cells. Jolt
		// warns once per degenerate triangle it is handed, which would drown the log; drop
		// them here, where it costs one cross product.
		if ((v[1] - v[0]).cross(v[2] - v[0]).length_squared() < 1e-12f) continue;
		fw[n++] = v[0];
		fw[n++] = v[1];
		fw[n++] = v[2];
	}
	faces.resize(n);
	if (n < 3) return kEmpty;

	Dictionary data;
	data["faces"] = faces;
	// Left false deliberately: the mesher's winding always faces the air, so one-sided
	// collision is correct everywhere, including inside a carved cave. Jolt also does not
	// implement the two-sided case.
	data["backface_collision"] = false;

	if (!shapes_[slot].is_valid()) shapes_[slot] = ps->concave_polygon_shape_create();
	if (!shapes_[slot].is_valid()) return kFailed;
	ps->shape_set_data(shapes_[slot], data);

	if (!bodies_[slot].is_valid()) {
		bodies_[slot] = ps->body_create();
		if (!bodies_[slot].is_valid()) return kFailed;
		ps->body_set_mode(bodies_[slot], PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_add_shape(bodies_[slot], shapes_[slot]);
		ps->body_set_collision_layer(bodies_[slot], 1);
		ps->body_set_collision_mask(bodies_[slot], 1);
		// Explicit: both backends default a server-created body to ray-pickable, and the
		// tests' intersect_ray depends on it.
		ps->body_set_ray_pickable(bodies_[slot], true);
		// Mesh positions are already world space, so the body never moves.
		ps->body_set_state(bodies_[slot], PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D());
	}
	ps->body_set_shape_disabled(bodies_[slot], 0, false);
	if (!in_space_[slot]) {
		ps->body_set_space(bodies_[slot], space_);
		in_space_[slot] = 1;
		active_bodies_++;
	}
	return kBuilt;
}

void ColliderStreamer::release_slot(int slot) {
	if (slot < 0 || slot >= static_cast<int>(bodies_.size())) return;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || !in_space_[slot]) return;
	// The body and its shape RID are kept for reuse: walking in and out of the radius would
	// otherwise churn server allocations every few seconds, and out of the space they cost
	// Jolt nothing.
	ps->body_set_space(bodies_[slot], RID());
	in_space_[slot] = 0;
	active_bodies_--;
}

void ColliderStreamer::apply_result(const MeshResult &r) {
	const int slot = chunks_->slot_of(r.chunk);
	if (slot < 0) return; // evicted while the mesh was in flight; nothing to attach it to
	if (r.overflow && overflow_warnings_ < 8) {
		overflow_warnings_++;
		UtilityFunctions::push_warning("ColliderStreamer: chunk (", r.chunk.x, ", ", r.chunk.y,
				", ", r.chunk.z, ") hit a mesher cap; collider built from what fit");
	}
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	const BuildOutcome outcome = build_shape(slot, r);
	last_build_ms_ =
			static_cast<float>(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
	switch (outcome) {
		case kBuilt:
			chunks_->note_built(r.chunk);
			break;
		case kEmpty: {
			// The probe is conservative, so a chunk it passed can hold no triangles at all.
			const int freed = chunks_->note_empty(r.chunk);
			release_slot(freed);
			break;
		}
		case kFailed:
			// Spec §6's failure policy: log, keep the previous collider, retry next frame.
			failures_++;
			UtilityFunctions::printerr("ColliderStreamer: shape build failed for chunk (",
					r.chunk.x, ", ", r.chunk.y, ", ", r.chunk.z, ")");
			chunks_->note_failed(r.chunk);
			break;
	}
}

int ColliderStreamer::run_frame(float cx, float cy, float cz) {
	if (!chunks_ || !mesh_ || !mesh_->is_valid()) return 0;
	int actions = 0;

	// 1. Land whatever the GPU finished. The sync inside collect() is for a batch submitted
	//    on an earlier frame, so it does not wait on the GPU.
	if (mesh_->in_flight()) mesh_->collect(&inbox_);

	// 2. Turn results into shapes, throttled: shape_set_data builds Jolt's BVH on this
	//    thread, and a 15k-triangle chunk is a millisecond or two of it.
	builds_last_frame_ = 0;
	while (!inbox_.empty() && builds_last_frame_ < max_builds_per_frame_) {
		MeshResult r = std::move(inbox_.front());
		inbox_.erase(inbox_.begin());
		apply_result(r);
		builds_last_frame_++;
		actions++;
	}

	// 3. Plan. No new work while a batch is in flight or results are still queued — the
	//    mesher holds one batch at a time, and a chunk planned now would only be dropped.
	const float center[3] = {cx, cy, cz};
	LogProbe probe;
	probe.gen = &gen_;
	probe.log = edit_log_;
	probe.mu = edit_mutex_;
	const int build_cap = (mesh_->in_flight() || !inbox_.empty()) ? 0 : -1;
	const ve::ChunkPlan plan = chunks_->update(center, nullptr, 1, probe, build_cap);
	for (const auto &e : plan.releases) {
		release_slot(e.slot);
		actions++;
	}

	// 4. Mesh. Each chunk lies inside exactly one region, so one op list reconstructs it.
	if (!plan.builds.empty()) {
		std::vector<std::vector<ve::EditOp>> ops;
		ops.reserve(plan.builds.size());
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			for (const auto &e : plan.builds)
				ops.push_back(edit_log_->ops(ve::region_of_chunk(e.chunk)));
		}
		std::vector<MeshJob> jobs;
		jobs.reserve(plan.builds.size());
		// Built only after every copy exists: a push_back that reallocated `ops` would
		// leave an earlier job pointing at freed memory.
		for (size_t i = 0; i < plan.builds.size(); i++)
			jobs.push_back({plan.builds[i].chunk, ops[i].data(), static_cast<int>(ops[i].size())});
		if (mesh_->submit(jobs.data(), static_cast<int>(jobs.size()))) {
			actions += static_cast<int>(jobs.size());
		} else {
			for (const auto &e : plan.builds) chunks_->note_failed(e.chunk);
		}
	}
	return actions;
}
```

- [ ] **Step 5: Wire it into VoxelWorld**

`voxel_world.h`: `colliders_` and `physics_tick` are already declared (Task 4); add the hooks:

```cpp
int debug_physics_frame(Vector3 center);
Dictionary debug_physics_stats();
RID debug_body_of_chunk(Vector3i chunk);
```

`voxel_world.cpp` — include `"physics/collider_streamer.h"` and `<godot_cpp/classes/world3d.hpp>`, and finish the functions Task 4 stubbed. `ensure_physics_initialized` in full, with the last three lines new:

```cpp
void VoxelWorld::ensure_physics_initialized() {
	if (physics_ready_) return;
	if (!mesh_rd_) mesh_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	if (!mesh_rd_) {
		UtilityFunctions::printerr("VoxelWorld: no local RenderingDevice for the mesher");
		return;
	}
	if (!edit_log_) edit_log_ = new ve::EditLog(world_bounds());
	mesh_pass_ = new MeshPass();
	MeshPassConfig mcfg;
	mcfg.max_jobs = mesh_jobs_per_frame_;
	if (!mesh_pass_->initialize(mesh_rd_, mcfg)) {
		delete mesh_pass_;
		mesh_pass_ = nullptr;
		return;
	}
	ve::ChunkResidencyConfig ccfg;
	ccfg.bounds = world_bounds();
	ccfg.radius_m = physics_radius_m_;
	ccfg.max_chunks = max_collider_chunks_;
	ccfg.max_builds_per_frame = mesh_jobs_per_frame_;
	chunks_ = new ve::ChunkResidency(ccfg);
	colliders_ = new ColliderStreamer();
	colliders_->initialize(chunks_, edit_log_, &edit_mutex_, mesh_pass_, max_collider_chunks_);
	colliders_->set_shape_builds_per_frame(shape_builds_per_frame_);
	physics_ready_ = true;
}

void VoxelWorld::teardown_physics() {
	physics_ready_ = false;
	// Colliders first: they hold the mesher's results and the residency's slots.
	if (colliders_) { delete colliders_; colliders_ = nullptr; }
	if (mesh_pass_) { delete mesh_pass_; mesh_pass_ = nullptr; }
	if (chunks_) { delete chunks_; chunks_ = nullptr; }
	if (mesh_rd_) { memdelete(mesh_rd_); mesh_rd_ = nullptr; }
	pending_dirty_.clear();
}

int VoxelWorld::physics_tick(Vector3 center) {
	if (!physics_ready_ || !colliders_ || !chunks_) return 0;
	// Drain the dirty ranges the edit path queued. They are COLLECTED under edit_mutex_ and
	// APPLIED here, on the main thread, so ChunkResidency needs no lock of its own — and the
	// probe inside update(), which takes edit_mutex_, can never deadlock against an edit.
	std::vector<std::pair<ve::IVec3, ve::IVec3>> dirty;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		dirty.swap(pending_dirty_);
	}
	for (const auto &r : dirty) chunks_->mark_dirty(r.first, r.second);
	const Ref<World3D> w = get_world_3d();
	if (w.is_valid()) colliders_->set_space(w->get_space());
	return colliders_->run_frame(center.x, center.y, center.z);
}

int VoxelWorld::debug_physics_frame(Vector3 center) {
	ensure_physics_initialized();
	return physics_tick(center);
}

Dictionary VoxelWorld::debug_physics_stats() {
	Dictionary d;
	d["chunks_resident"] = chunks_ ? chunks_->resident_count() : 0;
	d["chunks_pending"] = chunks_ ? chunks_->pending_count() : 0;
	d["probe_cache"] = chunks_ ? chunks_->probe_cache_size() : 0;
	d["bodies"] = colliders_ ? colliders_->active_bodies() : 0;
	d["builds"] = colliders_ ? colliders_->builds_last_frame() : 0;
	d["queued"] = colliders_ ? colliders_->queued_results() : 0;
	d["failures"] = colliders_ ? colliders_->failures() : 0;
	d["build_ms"] = colliders_ ? colliders_->last_build_ms() : 0.0f;
	d["collect_ms"] = colliders_ ? colliders_->last_collect_ms() : 0.0f;
	return d;
}

RID VoxelWorld::debug_body_of_chunk(Vector3i chunk) {
	if (!chunks_ || !colliders_) return RID();
	return colliders_->body_of_slot(chunks_->slot_of({chunk.x, chunk.y, chunk.z}));
}
```

Update `_process` to use the return value's absence gracefully (it already ignores it) and bind
the three hooks in `_bind_methods()`:

```cpp
ClassDB::bind_method(D_METHOD("debug_physics_frame", "center"), &VoxelWorld::debug_physics_frame);
ClassDB::bind_method(D_METHOD("debug_physics_stats"), &VoxelWorld::debug_physics_stats);
ClassDB::bind_method(D_METHOD("debug_body_of_chunk", "chunk"), &VoxelWorld::debug_body_of_chunk);
```

- [ ] **Step 6: Build and run the test**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_collider_stream.gd`
Expected: PASS, five cases.

If `test_a_physics_ray_lands_on_the_analytic_surface` finds no collider while
`test_colliders_appear_around_the_player` passes, the geometry exists but faces the wrong way:
check `winding_bad` in `debug_mesh_diff` first, and only then the shape dictionary.

- [ ] **Step 7: Run the whole suite**

Run: `cd extension && scons test -j$(nproc)` then
`./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: all green.

- [ ] **Step 8: Commit**

```bash
git add extension/src/physics/collider_streamer.h extension/src/physics/collider_streamer.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_collider_stream.gd
git commit -m "feat: stream dual-contoured colliders into Jolt static bodies"
```

---

### Task 8: edits dirty the colliders

Spec §6: "Edit → remesh → collidable again in 1–2 frames." The edit path already fans out to the raymarch set and the atlas; this task adds the collision fan-out.

**Files:**
- Modify: `extension/src/voxel_world.cpp` (`append_edit`)
- Create: `tests/test_collider_edits.gd`

**Interfaces:**
- Consumes: `ve::op_chunk_range` (Task 1), `VoxelWorld::pending_dirty_` (Task 4), `ve::ChunkResidency::mark_dirty` (Task 3).
- Produces: no new API — `VoxelWorld::append_edit` gains the fan-out.

- [ ] **Step 1: Write the failing test**

Create `tests/test_collider_edits.gd`:

```gdscript
extends GdUnitTestSuite

# Spec section 6: "Edit -> remesh -> collidable again in 1-2 frames." The collider is
# rebuilt from the same op list the renderer uses, so what you shot through is what you fall
# through.

const CENTER := Vector3(60.0, 55.0, 60.0)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 64
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	add_child(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, center: Vector3, frames := 400) -> void:
	var quiet := 0
	for i in range(frames):
		quiet = quiet + 1 if w.debug_physics_frame(center) == 0 else 0
		if quiet >= 4:
			return

func ray(from: Vector3, to: Vector3) -> Dictionary:
	var state := get_tree().root.world_3d.direct_space_state
	return state.intersect_ray(PhysicsRayQueryParameters3D.create(from, to))

func test_carving_makes_the_ground_fall_away(timeout := 90000) -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	settle(w, CENTER)
	await get_tree().physics_frame

	var from := Vector3(CENTER.x, 80.0, CENTER.z)
	var to := Vector3(CENTER.x, 20.0, CENTER.z)
	var before := ray(from, to)
	assert_bool(before.is_empty()).is_false()
	var surface: float = before["position"].y

	var r: Dictionary = tool.apply_sphere_subtract(Vector3(CENTER.x, surface + 0.5, CENTER.z), 3.0)
	assert_array(r["rejected"]).is_empty()
	# A blast dirties a handful of chunks; at two jobs a frame the one under the player
	# rebuilds first, so a few frames is generous for "1-2".
	for i in range(30):
		w.debug_physics_frame(CENTER)
	await get_tree().physics_frame

	var after := ray(from, to)
	assert_bool(after.is_empty()).override_failure_message(
		"the crater floor has no collider at all").is_false()
	assert_float(after["position"].y).override_failure_message(
		"collision did not follow the crater: %f vs %f" % [after["position"].y, surface]
		).is_less(surface - 1.0)
	# ...and it still agrees with the field the renderer draws.
	var oracle: Dictionary = w.debug_raycast(from, Vector3(0, -1, 0))
	assert_float(after["position"].y).is_equal_approx(oracle["pos"].y, 0.15)

func test_filling_makes_new_ground_collidable(timeout := 90000) -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	settle(w, CENTER)

	# Well above the terrain (which tops out at 51.2 + 10) and still inside the world, whose
	# y span here is [-51.2, 76.8): nothing to stand on, and the chunk's probe has said so.
	var from := Vector3(CENTER.x, 78.0, CENTER.z)
	var to := Vector3(CENTER.x, 66.0, CENTER.z)
	assert_bool(ray(from, to).is_empty()).is_true()

	var r: Dictionary = tool.apply_sphere_add(Vector3(CENTER.x, 70.0, CENTER.z), 4.0, 4)
	assert_array(r["rejected"]).is_empty()
	settle(w, CENTER)
	await get_tree().physics_frame

	var hit := ray(from, to)
	assert_bool(hit.is_empty()).override_failure_message(
		"the added blob never became collidable — the cached empty verdict was not cleared"
		).is_false()
	assert_float(hit["position"].y).is_equal_approx(74.0, 0.3)

func test_a_paint_edit_rebuilds_nothing_it_does_not_have_to(timeout := 90000) -> void:
	var w := make_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	settle(w, CENTER)
	var hit: Dictionary = w.debug_raycast(Vector3(CENTER.x, 80.0, CENTER.z), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()

	# Paint changes no SDF, but it is still an op in the region's list, so the chunks it
	# touches are re-meshed once. The point of the test is that it CONVERGES — a dirty mark
	# that never clears would keep the mesher busy for ever.
	tool.apply_sphere_paint(hit["pos"], 3.0, 1)
	settle(w, CENTER)
	for i in range(20):
		assert_int(w.debug_physics_frame(CENTER)).is_equal(0)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_collider_edits.gd`
Expected: FAIL — `test_carving_makes_the_ground_fall_away` finds the *old* surface, because no
chunk was ever marked dirty.

- [ ] **Step 3: Fan the edit out to the colliders**

In `extension/src/voxel_world.cpp`, `append_edit` (which already holds `edit_mutex_`):

```cpp
ve::EditLog::AppendResult VoxelWorld::append_edit(const ve::EditOp &op) {
	std::lock_guard<std::mutex> lock(edit_mutex_);
	if (!edit_log_) return {};
	ve::EditLog::AppendResult r = edit_log_->append(op);
	if (!r.rejected.empty()) {
		UtilityFunctions::printerr("VoxelWorld: region op list full, op rejected (",
				r.rejected[0].x, ", ", r.rejected[0].y, ", ", r.rejected[0].z,
				") — spec §8 fail-soft");
	}
	pending_edits_.push_back({op, r});
	// Collision's half of the fan-out (spec §5: "Fan-out: raymarch set, physics remesh queue,
	// LoD chain, connectivity"). Queued rather than applied, because this may run on any
	// thread that owns a tool while ChunkResidency belongs to the main one; physics_tick
	// drains it. Queued even when physics is off, so enabling it later starts consistent.
	ve::IVec3 clo{}, chi{};
	ve::op_chunk_range(op, &clo, &chi);
	pending_dirty_.push_back({clo, chi});
	return r;
}
```

`voxel_world.cpp` already includes `mesh/mesh_chunk.h` from Task 4.

- [ ] **Step 4: Build and run the test**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_collider_edits.gd`
Expected: PASS, three cases.

`test_filling_makes_new_ground_collidable` is the one that proves `mark_dirty` clears the probe
cache; `test_a_paint_edit_rebuilds_nothing_it_does_not_have_to` is the one that proves the dirty
state actually clears.

- [ ] **Step 5: Run the whole suite**

Run: `cd extension && scons test -j$(nproc)` then
`./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: all green.

- [ ] **Step 6: Commit**

```bash
git add extension/src/voxel_world.cpp tests/test_collider_edits.gd
git commit -m "feat: edits dirty the collision chunks they touch"
```

---

### Task 9: the demo — a character that walks on it

Spec §6: "Character = standard `CharacterBody3D` capsule." The scene grows a player, the camera moves onto it, the HUD reports the collider streamer, and the benchmark prints its numbers.

**Files:**
- Create: `demo/player.gd`
- Delete: `demo/fly_camera.gd`, `demo/fly_camera.gd.uid`
- Modify: `demo/main.tscn`, `demo/hud.gd`, `demo/benchmark.gd`, `demo/edit_tool.gd`

**Interfaces:**
- Consumes: `VoxelWorld::physics_center_path` (Task 4), `VoxelWorld::debug_physics_stats` (Task 7), `VoxelEditTool::apply_sphere_*` (M2).
- Produces: `Player` (`CharacterBody3D`) at `/root/Main/Player` with `flying: bool` and `velocity`, its `Camera3D` child at `/root/Main/Player/Camera3D`.

- [ ] **Step 1: Write the player**

Create `demo/player.gd`:

```gdscript
extends CharacterBody3D
# Demo character (spec section 6: "Character = standard CharacterBody3D capsule").
#
# Two modes, F toggles: FLY (no gravity, no collision, the M2 fly camera's controls) and
# WALK (gravity, move_and_slide against the streamed colliders). It starts in FLY on purpose
# — the collider streamer needs a second or so to build the first chunks, and a walking body
# dropped into a world whose colliders do not exist yet would fall straight through.

@export var walk_speed := 6.0
@export var fly_speed := 25.0
@export var jump_velocity := 5.5
@export var look_sensitivity := 0.0025
@export var gravity := 24.0

var flying := true

@onready var _cam: Camera3D = $Camera3D

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		rotate_y(-event.relative.x * look_sensitivity)
		_cam.rotate_x(-event.relative.y * look_sensitivity)
		_cam.rotation.x = clampf(_cam.rotation.x, -1.45, 1.45)
	if event.is_action_pressed("ui_cancel"):
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED else Input.MOUSE_MODE_CAPTURED
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F:
		flying = not flying
		velocity = Vector3.ZERO

func _physics_process(delta: float) -> void:
	var dir := Vector3.ZERO
	# Flying steers with the camera (pitch included); walking steers with the body.
	var basis := _cam.global_transform.basis if flying else global_transform.basis
	if Input.is_key_pressed(KEY_W): dir -= basis.z
	if Input.is_key_pressed(KEY_S): dir += basis.z
	if Input.is_key_pressed(KEY_A): dir -= basis.x
	if Input.is_key_pressed(KEY_D): dir += basis.x

	if flying:
		var lift := 0.0
		if Input.is_key_pressed(KEY_E): lift += 1.0
		if Input.is_key_pressed(KEY_Q): lift -= 1.0
		var boost := 4.0 if Input.is_key_pressed(KEY_SHIFT) else 1.0
		velocity = (dir.normalized() + Vector3.UP * lift) * fly_speed * boost
		global_position += velocity * delta # no collision in fly mode
		return

	dir.y = 0.0
	dir = dir.normalized()
	velocity.x = dir.x * walk_speed
	velocity.z = dir.z * walk_speed
	if is_on_floor():
		velocity.y = jump_velocity if Input.is_key_pressed(KEY_SPACE) else 0.0
	else:
		velocity.y -= gravity * delta
	move_and_slide()
```

- [ ] **Step 2: Rewrite the scene**

Replace `demo/main.tscn` (the camera moves onto the player, and the world learns where to
stream colliders):

```
[gd_scene load_steps=10 format=3 uid="uid://dx2akrpw2s62k"]

[ext_resource type="Script" path="res://demo/player.gd" id="1"]
[ext_resource type="Script" path="res://demo/hud.gd" id="2"]
[ext_resource type="Script" path="res://demo/benchmark.gd" id="3"]
[ext_resource type="Script" path="res://demo/edit_tool.gd" id="4"]

[sub_resource type="RaymarchCompositor" id="1"]
world_path = NodePath("/root/Main/VoxelWorld")

[sub_resource type="Compositor" id="2"]
compositor_effects = [SubResource(1)]

[sub_resource type="Environment" id="3"]
background_mode = 4
ambient_light_source = 3
ambient_light_energy = 0.4

[sub_resource type="BoxMesh" id="4"]
size = Vector3(2, 2, 2)

[sub_resource type="CapsuleShape3D" id="5"]
radius = 0.4
height = 1.8

[node name="Main" type="Node3D"]

[node name="VoxelWorld" type="VoxelWorld" parent="."]
world_origin_bricks = Vector3i(0, -64, 0)
world_size_regions = Vector3i(64, 8, 64)
physics_center_path = NodePath("/root/Main/Player")

[node name="WorldEnvironment" type="WorldEnvironment" parent="."]
environment = SubResource(3)
compositor = SubResource(2)

[node name="Player" type="CharacterBody3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 8, 62, 8)
script = ExtResource(1)

[node name="Collider" type="CollisionShape3D" parent="Player"]
shape = SubResource(5)

[node name="Camera3D" type="Camera3D" parent="Player"]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0.7, 0)

[node name="DirectionalLight3D" type="DirectionalLight3D" parent="."]
transform = Transform3D(0.6, -0.48, 0.64, 0, 0.8, 0.6, -0.8, -0.36, 0.48, 0, 20, 0)

[node name="TestCube" type="MeshInstance3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 24, 57.2, 24)
mesh = SubResource(4)

[node name="HUD" type="CanvasLayer" parent="."]

[node name="Label" type="Label" parent="HUD"]
offset_left = 10.0
offset_top = 10.0
script = ExtResource(2)
world_path = NodePath("/root/Main/VoxelWorld")

[node name="Benchmark" type="Node" parent="."]
script = ExtResource(3)

[node name="EditTool" type="Node" parent="."]
script = ExtResource(4)
world_path = NodePath("/root/Main/VoxelWorld")
camera_path = NodePath("/root/Main/Player/Camera3D")
player_path = NodePath("/root/Main/Player")
```

Then remove the node the player replaced:

```bash
git rm demo/fly_camera.gd demo/fly_camera.gd.uid
```

- [ ] **Step 3: Give the edit tool a camera on the player and an explosion kick**

`demo/edit_tool.gd`: add the export, resolve it, and push the player away from a carve —
spec §5's "sphere-subtract (explosion + radial impulse)". M3's only dynamic body is the
character; M4's islands are what this will really throw.

```gdscript
@export var player_path: NodePath
```

```gdscript
var _player: CharacterBody3D
```

```gdscript
func _ready() -> void:
	_world = get_node(world_path)
	_cam = get_node(camera_path)
	if not player_path.is_empty():
		_player = get_node(player_path)
	_tool = ClassDB.instantiate("VoxelEditTool")
	_world.add_child(_tool) # VoxelEditTool resolves the world through its parent
```

```gdscript
		MOUSE_BUTTON_LEFT:
			_tool.apply_sphere_subtract(pos, radius)
			_kick(pos)
```

```gdscript
func _kick(pos: Vector3) -> void:
	if _player == null or _player.flying:
		return
	var away: Vector3 = _player.global_position - pos
	var d := away.length()
	var reach := radius * 3.0
	if d > reach or d < 0.001:
		return
	_player.velocity += away.normalized() * (1.0 - d / reach) * 14.0
```

- [ ] **Step 4: Report the collider streamer in the HUD**

`demo/hud.gd`, inside the `_frames % 15` block:

```gdscript
	var s := "world: booting"
	if _world and _world.is_initialized():
		var st: Dictionary = _world.debug_stream_stats()
		s = "regions %d  edits %d  ovf %d" % [
			st.get("resident_regions", 0), st.get("frame_edits", 0),
			st.get("overflow_ever", 0)]
	var p := ""
	if _world:
		var ph: Dictionary = _world.debug_physics_stats()
		# build_ms is the Jolt BVH build for the last chunk; it is the one physics number
		# that can show up in the frame time (spec section 6 budgets nothing for it, and the
		# streamer throttles it to shape_builds_per_frame).
		p = "  |  chunks %d (+%d)  bodies %d  build %.1fms" % [
			ph.get("chunks_resident", 0), ph.get("chunks_pending", 0),
			ph.get("bodies", 0), ph.get("build_ms", 0.0)]
	text = "%d fps  (%.1f ms)  |  %s%s" % [fps, ms, s, p]
```

- [ ] **Step 5: Teach the benchmark about the player**

`demo/benchmark.gd`:

```gdscript
func _ready() -> void:
	if "--benchmark" in OS.get_cmdline_user_args():
		_active = true
		var player: CharacterBody3D = get_parent().get_node("Player")
		# Freeze the character rather than the camera: the world streams colliders around it,
		# so it has to stay put for the run to measure a steady state.
		player.set_physics_process(false)
		player.set_process_unhandled_input(false)
		player.global_transform = Transform3D(Basis.IDENTITY, Vector3(24, 63.2, 24))
		var cam: Camera3D = player.get_node("Camera3D")
		cam.transform = Transform3D(Basis.looking_at(Vector3(6, -10, 6).normalized()),
			Vector3(0, 0.7, 0))
```

```gdscript
	if _frames >= FRAMES:
		var avg := _accum_ms / FRAMES
		print("BENCH frame_avg_ms=%.2f fps=%.1f" % [avg, 1000.0 / avg])
		var world: VoxelWorld = get_parent().get_node("VoxelWorld")
		var st: Dictionary = world.debug_stream_stats()
		print("BENCH regions=%d overflow=%d" % [st.get("resident_regions", -1), st.get("overflow_ever", -1)])
		var ph: Dictionary = world.debug_physics_stats()
		print("BENCH chunks=%d pending=%d bodies=%d failures=%d build_ms=%.2f collect_ms=%.2f" % [
			ph.get("chunks_resident", -1), ph.get("chunks_pending", -1), ph.get("bodies", -1),
			ph.get("failures", -1), ph.get("build_ms", 0.0), ph.get("collect_ms", 0.0)])
		if avg > 16.6:
			push_warning("BENCH: frame budget exceeded (target 16.6ms)")
		get_tree().quit()
```

- [ ] **Step 6: Run the demo and walk on it**

Run: `./build.sh -j$(nproc) && godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`

Check, in order:
1. The HUD's `chunks` climbs to a plateau within a couple of seconds and `pending` settles at 0.
2. Press **F**: the capsule lands on the terrain instead of falling through it, and WASD walks.
3. Walk 100 m: `chunks` stays near its plateau (the far ones are released), `bodies` too.
4. Left-click the ground next to you: a crater appears, you are pushed away from it, and
   walking into the crater goes *down* rather than across an invisible floor.
5. Right-click in the air: a blob appears and can be stood on within a second.
6. Press **F** again and fly to the cave at (30, ~50, 30); the cave mouth is walkable.
7. No `ERROR`/`Invalid shape data` lines in the console.

- [ ] **Step 7: Run the benchmark**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn -- --benchmark`
Expected: the `BENCH` lines print, `frame_avg_ms` stays under 16.6, `failures=0`, and
`pending=0` by the end of the 300 frames.

If `frame_avg_ms` is over budget, look at `build_ms` first: the Jolt BVH build is the only
new main-thread cost, and `shape_builds_per_frame` is the knob (a chunk of ~15 k triangles
runs a millisecond or two). `collect_ms` covers the sync plus readback; if that is what is
large, the mesher is not keeping a whole frame ahead and `mesh_jobs_per_frame` should come
down.

- [ ] **Step 8: Full suite**

Run: `cd extension && scons test -j$(nproc)` then
`./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: all green.

- [ ] **Step 9: Commit**

```bash
git add demo/player.gd demo/main.tscn demo/hud.gd demo/benchmark.gd demo/edit_tool.gd
git rm --cached demo/fly_camera.gd demo/fly_camera.gd.uid 2>/dev/null || true
git commit -m "feat: character controller walking on streamed colliders"
```

---

## M3 Acceptance Checklist

- `cd extension && scons test` — native suite green (chunk lattice, dual contouring, chunk residency, + all M1/M2 cases)
- `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests` — green: boot, GPU smoke, field differential, atlas, region pass, brick differential, streaming, mips, edit pipeline, pixel + magenta, **chunk lattice differential**, **mesher differential**, **mesh batch pipeline**, **collider streaming**, **collider edits**
- `godot --path . demo/main.tscn` — press F and walk on the terrain; carve a crater and walk into it; fill a blob and stand on it; the HUD's `pending` settles at 0 and `failures` stays 0
- `godot --path . demo/main.tscn -- --benchmark` — BENCH lines printed, `frame_avg_ms` < 16.6, `failures=0`
- `ve::dual_contour` remains the CPU reference; `test_mesh_diff.gd` guards drift, on the GPU's own lattice
- No physics work runs on the render thread: `RaymarchCompositor::_render_callback` is untouched by this milestone

## Spec §6 Coverage

| Spec §6 sentence | Where |
|---|---|
| "Dual-contoured meshes from L0 bricks at 0.1 m (half-res)" | Tasks 2, 4, 5 — at 0.1 m from the same field the L0 bricks are generated from (see Deliberate Decisions) |
| "Chunked at 12.8 m (16 bricks); ~5–20 k tris/chunk" | Task 1 (`kChunkBricks`), measured in Task 5's test |
| "GPU compute meshing + async double-buffered readback (~120 KB/chunk)" | Tasks 4–6; the real readback is ~590 KB/chunk (indexed), recorded in Fixed Numbers |
| "`PhysicsServer3D` direct (no scene-tree nodes), Jolt concave shapes in a static compound" | Task 7 (`ColliderStreamer`; one body per pool slot — see Deliberate Decisions) |
| "Collision streams in a ~64 m radius around the player + small bubbles around active bodies" | Task 3 (`ChunkResidency::update` takes N centres), Task 7 passes the player; bubbles deferred to M4's bodies |
| "Edit → remesh → collidable again in 1–2 frames" | Task 8 |
| "Character = standard `CharacterBody3D` capsule" | Task 9 |
| "Explosions apply radial impulses" | Task 9 (`_kick`); `PhysicsDirectSpaceState` sweeps arrive with M4's bodies |
| "Physics mesh pool ~80 chunks (~1 M tris worst case)" | Fixed Numbers — 160 chunks, with the arithmetic for why |
| "Failure policy: readback/shape-build failure → log, keep previous collider, retry next frame" | Task 7 (`apply_result`'s three outcomes) |
| "Islands = box compounds", "small debris", "Jolt sleep events drive the re-merge", "CCD on fast debris" | M4 — they need the connectivity pass and the bodies it produces |
| §8 "CPU references for … meshing; dev console command runs both and diffs" | Task 5 (`debug_mesh_diff`, `test_mesh_diff.gd`) |

## Errata (recorded during M3 implementation — corrections to the task text)

<!-- Append numbered entries here as the plan meets reality, in the style of M1/M2. -->

1. **Task 7 — Jolt winding convention.** In the target Godot 4.7.1 + Jolt build, the triangle winding supplied by the brief's `ConcavePolygonShape3D` contract test and `ColliderStreamer::build_shape` is treated as the back face when `backface_collision=false`; a downward ray misses. `ColliderStreamer::build_shape` and `tests/test_collider_stream.gd` therefore swap the last two vertices of each triangle so the terrain's upward-facing surfaces collide from above. The one-sided collision behavior is unchanged.
2. **Task 7 — gdUnit world cleanup.** The brief's collider tests did not free their `VoxelWorld` children. gdUnit does not clear root children between test cases in a suite, so later tests saw earlier physics bodies. Every world-creating suite (`test_collider_stream.gd`, `test_collider_edits.gd`, `test_mesh_lattice.gd`, `test_mesh_diff.gd`, `test_mesh_stream.gd`) now registers each created world in `_worlds` and frees it in `after_test()`, so cleanup also runs when a test fails before its final `w.free()`.
3. **Task 9 — player kick regression test.** To guard the explosion-kick fix, the demo task adds `tests/test_player_kick.gd`, which is outside the original task file list. This is an approved scope addition recorded as errata.
4. **Task 9 — benchmark invocation.** The exact interactive benchmark command can stall on vsync/display frame throttling in a headless/CI-like environment. Running with `--disable-vsync` is an acceptable verification substitute; the demo's own BENCH output and acceptance criteria remain unchanged.
5. **Post-M3 — streaming must fund its own loads (the transient-hole bug).** Freshly streamed terrain showed sky through it for a few frames before healing. Measured at the demo's configuration over a 200-frame flight: the free list sat at zero on 58 frames and the mark pass reported dropped bricks on 14, so the bricks a load had just activated were fail-softly dropped and every ray through them passed into the (hollow) terrain shell and out to the sky.

   Two assumptions in `WorldStreamer::run_frame` were wrong in the same direction. A load was priced at a flat `kSlotsPerRegionEstimate = 3072` (real regions cost ~300-2500, and air regions nothing), and it was funded by displacing the single furthest resident — but distance and cost are nearly unrelated: only the region layers the surface crosses hold atlas slots, so **53 of 75 evictions during that flight returned zero slots**. The free list could not hold its reserve and settled on the floor, where every subsequent stream-in dropped bricks.

   The fix gives the streamer the number it was guessing at. `brick_mark.comp.glsl` and `region_free.comp.glsl` now keep a per-region-slot tally of allocated bricks (new binding, `GpuAtlas::region_slot_counts`, one `max_region_slots`-int readback per frame). `RegionResidency::update` takes an `AtlasBudget` and releases furthest-first — crediting what each release actually held, skipping air regions when it is slots it needs, and crediting the hysteresis evictions a moving camera generates — until the load is paid for; an unfundable load is refused, so the horizon stops arriving instead of the ground going hollow. The bootstrap exemption is now re-tested per candidate (granting it to a whole frame let four regions into an atlas that held two, dropping the difference on frame zero). `evict_furthest` gained the same "evict until the slots are really recovered" rule for the edit-headroom and post-drop shrink arms.

   Result at the same demo configuration and flight: **0 starved frames, 0 dropped bricks, 1 of 39 evictions wasted**, free slots steady at 7-9k, and a *longer* horizon (166 resident regions against 137) because air regions are no longer evicted for nothing. Contract change: plain streaming may no longer use the drop arm, so `test_a_starved_atlas_heals_instead_of_keeping_the_holes` starves the atlas with edits — the path spec §8 still allows to drop — instead of with a flight. New coverage: `tests/test_streaming.gd::test_flight_never_drops_the_bricks_it_is_streaming_in` and two `extension/tests/test_residency.cpp` cases.

