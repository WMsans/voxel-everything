# M4 Connectivity & Raymarched Islands — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Blow a chunk of terrain loose and watch it *fall*: a persistent 0.8 m occupancy grid, a localised flood fill that decides what is still attached to the world, connected-component labelling that turns the rest into islands, each island carved out of the static field, extracted into a dense 5 cm SDF volume, raymarched as an additional march target at full terrain fidelity, dropped into Jolt as a box compound — and, two seconds after it stops moving, stamped back into the terrain as permanent rubble.

**Architecture:** Occupancy is two bits per 0.8 m cell (`solid`, `full`), computed for free by the mark pass that already probes every brick and read back asynchronously into a persistent CPU grid. Connectivity is four pure C++ cores — grid, flood fill, component labelling, thin-contact refinement — with no Godot and no GPU, so the spec's "anchored cells never become islands" invariant is a doctest. An island is *exactly* the solid field intersected with a union of 0.8 m cells, which makes both halves of its lifecycle expressible as CSG: the carve is a handful of **box-subtract ops**, and the re-merge is one **volume-add op** referencing a dense volume the CPU owns. Extraction runs on the collision mesher's existing worker device (it already compiles `field.glslh` and already reads back); the render device gets an island atlas of ≤32 live volumes, a per-tile 32-bit visibility mask, and a raymarcher restructured to resolve the nearest of terrain and islands before shading.

**Tech Stack:** Godot 4.7.1 (`/usr/bin/godot`), godot-cpp (pinned master, `api_version = "4.7"`), SCons, C++20, GLSL 460 (Vulkan), Jolt Physics (`project.godot`'s `3d/physics_engine`), doctest 2.4.11 (native), gdUnit4 6.2.1 (in-engine).

**Spec:** `docs/superpowers/specs/2026-08-12-voxel-engine-design.md` — M4 implements §5 in full, §3's *Multi-target raymarching (islands)* block, §6's "Islands = box compounds", "small debris", "Jolt sleep events drive the re-merge hook" and "CCD on fast debris", and §8's `connectivity/` module plus the box-merging half of `mesh/`.

**Predecessors:** `docs/superpowers/plans/2026-08-12-m1-walking-skeleton.md`, `docs/superpowers/plans/2026-08-13-m2-gpu-generation-streaming-edits.md`, `docs/superpowers/plans/2026-08-14-m3-physics-meshing-colliders.md` (all complete). **Read all three Errata sections before touching shaders, `VoxelWorld` or the streamer** — in particular M2 errata 5 (GLSL reserved words), 7 (`ivec4` → `.xyz`), 9 (the `kSurfaceY = 51.2` offset), 10 (the mip-skip corrections), 11 (the eviction arm) and M3 errata 1 (Jolt's winding convention) and 5 (streaming funds its own loads).

## Milestone Map

| Milestone | Delivers |
|---|---|
| M1 (done) | Toolchain + raymarched terrain on screen + test harnesses |
| M2 (done) | GPU brick generation, region indirection, streaming/residency + LRU, min–max mips, destruction edits |
| M3 (done) | Dual-contour collision meshing on the GPU, async readback, collider streaming into Jolt, character controller |
| **M4 (this plan)** | Occupancy grid, connectivity, island carve/extract/spawn/re-merge, raymarched island targets, tiled culling, debris |
| M5 | LoD hierarchy + bakery + depth-injection compositing |
| M6 | Beautification: cel, 3-layer shadows, SSGI, SSR, outlines |
| M7 | Benchmark scene + demo polish |

## Global Constraints

- Godot **4.7.1**; godot-cpp pinned to the existing submodule commit, `api_version = "4.7"` — do not bump either.
- Pure C++ cores in `namespace ve` contain **zero Godot types** (spec §8) — no exceptions. Godot-glue classes live in `namespace godot`. Spec §8's module table is binding: `connectivity/`, `generator/`, `mesh/`, `world/` are pure C++; `physics/` and `render/` are Godot glue. The island *manager* orchestrates physics bodies, so it lives in `physics/`, not `connectivity/`.
- Shaders: GLSL `#version 460`, loaded **from files** via `ve::load_shader_source` — never inline strings. `#[compute]` is stripped in C++ after load (M1 errata 6).
- Error policy (spec §8): dev = verbose/validation; release = fail-soft — a connectivity or meshing anomaly **warns and no-ops**, a pool exhaustion evicts rather than crashing, a failed extraction drops the island and leaves the terrain intact. **Leaving a piece attached is always the safe direction**; a wrongly-dropped island is a hole in the world, a wrongly-kept one is a rock that did not fall.
- Commit style: conventional (`feat:`, `test:`, `build:`, `fix:`, `refactor:`).
- RD API reference: local copy at `docs/api/renderingdevice.md` — consult it before inventing signatures.
- Target hardware: RTX 4070 Laptop; budgets per spec §7 (raymarch ≤ 6 ms, frame ≤ 16 ms). M4's only new render-thread work is the tile-cull dispatch (~0.05 ms), the island descriptor upload (4 KB) and the island march itself, which the tile mask keeps to 0–3 volumes per pixel.
- **Push constants must stay ≤ 128 bytes** (Vulkan's guaranteed minimum). M4's largest is the tile-cull pass's 96 bytes (a `mat4` plus two `ivec4`).
- **Never put a literal include directive inside a GLSL comment** — the loader matches the token anywhere on a line (note at the top of `shaders/common.glslh`).
- `buffer_update` and `buffer_clear` are device-level commands: they must be recorded **before** `compute_list_begin`, never inside an open list (M2 Task 12's documented ordering).
- **`ve::EditOp` stays exactly 32 bytes** (`static_assert`) and its GLSL mirror stays exactly two `uvec4`. New op types reinterpret existing fields; they never widen the struct.

## Fixed Numbers (decided here; every task assumes them)

| Thing | Value | Where it lives |
|---|---|---|
| Occupancy cell | **0.8 m** = one brick | `ve::kOccupancyCellSize` (`== kBrickSize`) |
| Bits per cell | **2** (`unknown`/`air`/`solid`/`full`) | `ve::CellState` |
| Occupancy block per region | 32768 cells → **8192 bytes** | `ve::kOccupancyBlockBytes` |
| Occupancy GPU buffer | `max_region_slots × 2048` uints = **4 MB** at 512 slots | `GpuAtlas::region_occupancy()` |
| Occupancy reads in flight | **8** | `WorldStreamer::kOccupancyReads` |
| Flood window | **64³ cells = 51.2 m** (spec §5) | `ve::kFloodWindowCells` |
| Window expansions | **1**, doubling to 128³ = 102.4 m | `ve::kMaxWindowExpansions` |
| Frontier margin | **2 cells** | `ve::kFrontierMarginCells` |
| Island volume | **64³ voxels** | `ve::kIslandDim` |
| Island voxel pitch | **0.05 m**, or **0.10 m** above 2.95 m of extent | `ve::kIslandVoxelFine`, `kIslandVoxelCoarse` |
| Lattice margin | **2 voxels** at each end | `ve::kIslandMarginVoxels` |
| Max component extent | **5.6 m** (7 cells); wider components split | `ve::kMaxIslandExtentCells` |
| Live island slots (raymarched) | **32** (spec §5) | `godot::kMaxIslands` |
| Stored volume slots (rubble) | **64** | `ve::kMaxVolumes` |
| Island density | **500 kg/m³** (a fifth of rock; see `island_body.cpp`) | `kIslandDensity` |
| Boxes per island | **64** | `ve::kMaxIslandBoxes` |
| Debris threshold | **0.2 m³** of solid (spec §5) | `kDebrisVolumeM3` (island_manager.cpp) |
| Max dynamic bodies | **64** (spec §5) | `kMaxDynamicBodies` (island_manager.cpp) |
| Sleep → re-merge | **2.0 s** (spec §5) | `IslandManager::set_merge_sleep_seconds` |
| Island cull tile | **16 × 16 px** (spec §3) | `godot::kIslandTileSize` |
| Volume min–max cell | **8 voxels** → 8³ chain, 1 KB per island | `ve::kVolumeMipStride` |
| Contact-refine face samples | **9 × 9 at 0.1 m**, ≥ 8 solid to keep the link | `ContactRefineConfig` |
| Refine iterations | **3** | `ContactRefineConfig::max_iterations` |

**Memory.** Island atlas on the render device: 32 slots × 64³ SDF (R8) = 8.4 MB, + 8.4 MB material (R8UI), + 32 KB min–max mip → **~17 MB**. Stored-volume pool: 64 slots × 64³ × 2 bytes = **33.5 MB**, held once on the CPU and mirrored on both the render device and the mesher's worker device → ~100 MB total. Against spec §5's ~512 MB island-texture cap this is comfortable, which is the point: the cap is what forces early merges, and M4 never gets near it.

## Deliberate Decisions (recorded, with the spec text they interpret)

- **An island is a box union intersected with the field, so the carve is exact in CSG.** Spec §5 step 1 says "Carve out of the static SDF (automatic subtract op → correct crater)". Occupancy is per-0.8 m-cell, so a component *is* a set of whole cells: the material that leaves is `solid_field ∩ union(cells)` and the material that stays is `solid_field \ union(cells)`. Subtracting the (greedy-merged, ≤ 64) boxes is therefore not an approximation of the carve — it *is* the carve, exactly, with no new machinery in the field evaluator. The fracture face is 0.8 m-blocky, but that is a property of the spec's cell-granular connectivity, not of the op encoding: no representation could place the cut anywhere else.
- **The re-merge needs a stored volume, and that is the only place the field evaluator grows.** Spec §5 step 4: "island SDF sampled at rest pose and stamped back as a CSG paste-op". A rested island is rotated, so its shape is not a union of world-axis-aligned boxes and cannot be pasted as boxes without turning smooth rubble into a pile of cubes. `kOpVolumeAdd` therefore references a dense volume; `field.glslh` gains exactly **one** new binding (a byte-packed SSBO holding every stored volume back to back at a constant stride), and the CPU mirror gains a `ve::VolumeStore` interface argument that defaults to `nullptr`.
- **Volumes are always world-axis-aligned; rotation is removed by resampling, not carried in the op.** A volume op carries origin, pitch and dim in the 32 bytes it already has and needs no matrix. The rotation is spent once, on the worker thread, when the sleeping island is resampled into a fresh world-aligned volume — which is exactly what "sampled at rest pose" asks for.
- **The CPU owns the authoritative copy of every volume.** Both GPUs hold mirrors. This is forced: the collision mesher's field evaluation runs on the worker device, brick generation runs on the render device, and `ve::raycast` (the edit tool's aim) runs on the main thread — three consumers of one field. Extraction reads back to the CPU precisely so the other two can be fed from it.
- **Extraction runs synchronously on the mesher's worker thread.** `MeshService` already owns a `RenderingDevice` on a thread nothing else touches, already uploads op lists, and already reads results back. An extraction is ~1 ms of GPU work and a 512 KB readback; running it inline between mesh batches costs one deferred collision chunk and saves a second device, a second pipeline and a second in-flight protocol.
- **Occupancy is a by-product of the mark pass, not a new pass.** `brick_mark.comp.glsl` already probes every brick it scans on a 3³ lattice and reduces min/max — `solid` is `mn ≤ 0` and `full` is `mx ≤ 0`, both already in registers. Spec §5's "updated incrementally from brick-regen readback" is honoured by reading those two bits back; the deviation is that the mark pass, not the generation pass, is what produces them, because generation only ever runs on *surface* bricks and a fully-solid interior brick holds no atlas slot at all.
- **Unknown cells count as solid and anchored.** A cell in a region the mark pass has never scanned has no occupancy. Treating it as air would let pieces at the edge of the streamed world fall for no reason; treating it as solid-and-anchored can only ever *keep* a piece attached, which is the safe direction under spec §8's fail-soft rule, and self-heals the moment the region streams in.
- **The thin-contact check runs on the CPU.** Spec §5 calls for "a tiny GPU check [that] samples the true 5 cm SDF along the contact plane". The check as specified is 81 field evaluations per candidate link and the candidates are the *bridges* of the cell graph — a handful per edit. That is ~50 µs of CPU against a GPU round trip measured in frames (M2 errata: a synchronous read costs 1.6–39.6 ms). Same samples, same verdict, no latency.
- **Sleep is polled, not signalled.** Spec §6 says "Jolt sleep events drive the re-merge hook". `PhysicsServer3D` exposes `body_get_state(body, BODY_STATE_SLEEPING)`, which is the same bit the event would carry; the manager already runs every frame, so polling 32 bodies costs nothing and needs no signal plumbing through `PhysicsServer3D`.
- **Debris is a body like any other; only its rendering differs.** Spec §6 asks for "small debris = single-box bodies + cheap DC render meshes". Both islands and debris are `PhysicsServer3D` rigid bodies carrying a box compound — one physics path, one sleep clock, one re-merge. What separates them is that an island gets an atlas slot and is raymarched, while a crumb gets a `ve::dual_contour` mesh drawn through a `RenderingServer` instance. Server-direct on both sides, so spec §6's "no scene-tree nodes" holds for the whole of M4.

## Deliberate Deferrals (recorded, not forgotten)

- **Island fracture on impact.** Spec §5 already defers it: "islands land whole; a second explosion breaks them". A second explosion does break them, because the paste-op puts them back in the field first.
- **Structural stress.** Support stays binary (spec §5, *Deliberately not simulated*).
- **Simultaneous 2-face cuts.** The thin-contact refinement finds *bridges* and iterates, so it catches 1-face contacts and any chain of them. A piece held by exactly two faces that are not individually bridges stays anchored. That is the safe direction, and spec §5's own framing ("Cell grid decides the common case; true SDF arbitrates border cases") does not promise a complete cut enumeration.
- **Volume-pool consolidation into override bricks.** Spec §2 offers override bricks as the escape hatch when a region exceeds 256 ops. M4 keeps M2's behaviour — the append is rejected and logged — and adds one fail-soft arm: when the 64-slot volume pool (or the region's 256-op budget) cannot take another paste, the sleeping body **stays a body**. It is still collidable, still drawn, still there; it simply never becomes terrain. One warning, no crash, and nothing in the world is wrong — only unconsolidated.
- **Per-island material palettes.** An island volume stores one **byte of global material id per voxel** rather than a 2-bit palette index plus a palette (spec §3's "uint8 + palette"). At 64³ that is 256 KB against 64 KB + 8 bytes, on a pool that is nowhere near its cap, and it removes the palette-packing step from the extract shader and its CPU reference entirely.
- **BC compression of island volumes.** Not needed at this pool size; spec §4 only raises it for the LoD bakery.

## File Structure

```
extension/src/
  connectivity/                                              (pure C++, namespace ve)
    occupancy.h/.cpp        OccupancyGrid, CellState, block packing        (Task 1)
    flood_fill.h/.cpp       FloodWindow, LinkCuts, flood_anchored          (Task 2)
    components.h/.cpp       IslandComponent, label_islands, splitting      (Task 3)
    contact_refine.h/.cpp   ContactProbe, bridges, refine_anchoring        (Task 4)
  mesh/
    box_merge.h/.cpp        CellBox, greedy_box_merge                      (Task 5)
  generator/
    edit_ops.h/.cpp         MODIFIED: box + volume ops, op_world_aabb      (Task 6)
    volume_set.h/.cpp       VolumeStore, VolumeData, VolumeSet, resample   (Task 6)
  world/
    brick_eval.h/.cpp       MODIFIED: volumes threaded through eval_field  (Task 6)
    raycast.h/.cpp          MODIFIED: volumes threaded through             (Task 6)
  render/
    volume_pool.h/.cpp      the byte-packed volume SSBO on one device      (Task 7)
    island_extract_pass.h/.cpp   64^3 extraction on the worker device      (Task 9)
    mesh_service.h/.cpp     MODIFIED: extract queue + volume uploads       (Task 9)
    island_atlas.h/.cpp     render-device island textures + descriptors    (Task 10)
    island_cull_pass.h/.cpp per-tile 32-bit island mask                    (Task 11)
    raymarch_pass.h/.cpp    MODIFIED: island bindings, tile mask           (Tasks 10, 11)
    gpu_atlas.h/.cpp        MODIFIED: region_occupancy buffer              (Task 8)
    world_streamer.h/.cpp   MODIFIED: occupancy readback ring              (Task 8)
  physics/
    island_body.h/.cpp      one island's Jolt body, shapes, sleep clock    (Task 12)
    island_manager.h/.cpp   the per-frame orchestration                    (Task 13)
  voxel_world.h/.cpp        MODIFIED: grid, manager, hooks, seq counter    (Tasks 8, 13)
shaders/
  field.glslh               MODIFIED: box + volume ops, FIELD_VOLUME_*     (Task 7)
  brick_mark.comp.glsl      MODIFIED: 2-bit occupancy output               (Task 8)
  region_free.comp.glsl     MODIFIED: clears the region's occupancy block  (Task 8)
  brick_gen.comp.glsl       MODIFIED: binds the volume pool                (Task 7)
  mesh_field.comp.glsl      MODIFIED: binds the volume pool                (Task 7)
  field_probe.comp.glsl     MODIFIED: binds the volume pool                (Task 7)
  island_extract.comp.glsl  NEW: field ∩ box union → dense volume          (Task 9)
  island_cull.comp.glsl     NEW: island AABBs → per-tile bitmask           (Task 11)
  raymarch.comp.glsl        MODIFIED: multi-target resolve + island march  (Task 10)
extension/tests/            doctest: test_occupancy, test_flood_fill,
                            test_components, test_contact_refine,
                            test_box_merge, test_volume_ops
tests/                      gdUnit: test_field_volume_diff.gd, test_occupancy.gd,
                            test_island_extract.gd, test_island_render.gd,
                            test_island_body.gd, test_connectivity.gd
demo/                       edit_tool.gd, hud.gd, benchmark.gd (MODIFIED)
extension/SConstruct        MODIFIED: connectivity/ joins the native build (Task 1)
```

## Conventions Used Throughout

- **Build:** `./build.sh -j$(nproc)` (or `cd extension && scons -j$(nproc)`)
- **Native tests:** `cd extension && scons test`
- **gdUnit tests:** `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
- **Demo:** `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
- **A cell coordinate IS a brick coordinate.** Occupancy cells are 0.8 m and global, exactly like bricks and chunks: the world-space corner of cell `c` is `c * kBrickSize`, no origin term.
- **Window-local indices are `x` fastest, then `y`, then `z`** — the same order as every other lattice in this codebase (`voxel_index`, `dc_lattice_index`, `brick_index_in_region`).
- **A link is named by its lower cell and an axis**: link `(c, a)` joins `c` and `c + e_a`. Every face in the grid has exactly one name, so a cut set is a plain sorted vector with no aliasing.
- **`is_solid` is true for `kCellUnknown`.** Read every `is_solid` in this plan as "not known to be air". The one place the distinction matters is seeding, where unknown cells are also anchors.
- **Volumes are lattices, not cell grids**: sample `(x, y, z)` of a volume sits at `origin + (x, y, z) * voxel`, and the volume's world AABB for op-range purposes is `[origin, origin + (dim - 1) * voxel]`.
- gdUnit tests that await must declare the timeout argument: `func test_x(timeout := 10000) -> void:`.
- Every gdUnit suite that creates a `VoxelWorld` registers it in `_worlds` and frees it in `after_test()` (M3 errata 2).

---
### Task 1: `connectivity/occupancy` — the persistent 0.8 m grid

Spec §5's "Global persistent occupancy grid, 0.8 m cells = one bit per brick". It is two bits, not one: the flood fill needs "is there any solid voxel here" *and* the thin-contact refinement needs to know which links are worth checking at all, and both fall out of the min/max the mark pass already computes.

**Files:**
- Create: `extension/src/connectivity/occupancy.h`, `extension/src/connectivity/occupancy.cpp`
- Test: `extension/tests/test_occupancy.cpp`
- Modify: `extension/SConstruct:17`

**Interfaces:**
- Consumes: `ve::IVec3`, `ve::kRegionBrickCount`, `ve::WorldBounds::region_of_brick` (from `world/region.h`).
- Produces: `ve::CellState`, `ve::kOccupancyCellSize`, `ve::kOccupancyBlockBytes`, `ve::OccupancyGrid` with `set_block`, `set_cell`, `state`, `is_solid`, `is_full`, `is_known`, `has_region`, `block_seq`, `region_count`, `clear`, and the statics `cell_index_in_region`, `read_packed`, `write_packed`. Task 2 floods it, Task 4 refines against it, Task 8 fills it from the GPU, Task 13 owns it.

- [ ] **Step 1: Let the native build see `connectivity/`**

`extension/SConstruct` line 17 currently reads:

```python
pure_sources = Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") + Glob("src/mesh/*.cpp")
```

Change it to:

```python
pure_sources = (Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") +
                Glob("src/mesh/*.cpp") + Glob("src/connectivity/*.cpp"))
```

The GDExtension `sources` glob (`Glob("src/*/*.cpp")`, line 8) already picks the directory up; only the native test runner needed telling.

- [ ] **Step 2: Write the failing test**

Create `extension/tests/test_occupancy.cpp`:

```cpp
#include "connectivity/occupancy.h"
#include "doctest.h"
#include <vector>

using namespace ve;

namespace {

// A block whose cells are all `s`, so a test can install a whole region in one line.
std::vector<uint8_t> uniform_block(CellState s) {
	std::vector<uint8_t> b(kOccupancyBlockBytes, 0);
	for (int i = 0; i < kRegionBrickCount; i++)
		OccupancyGrid::write_packed(b.data(), i, static_cast<uint8_t>(s));
	return b;
}

} // namespace

TEST_CASE("packing round-trips every state at every offset in a byte") {
	std::vector<uint8_t> block(kOccupancyBlockBytes, 0);
	const CellState states[4] = {kCellUnknown, kCellAir, kCellSolid, kCellFull};
	for (int i = 0; i < 64; i++)
		OccupancyGrid::write_packed(block.data(), i, static_cast<uint8_t>(states[i % 4]));
	for (int i = 0; i < 64; i++)
		CHECK(OccupancyGrid::read_packed(block.data(), i) == static_cast<uint8_t>(states[i % 4]));
	// Rewriting one cell must not disturb the three sharing its byte.
	OccupancyGrid::write_packed(block.data(), 5, static_cast<uint8_t>(kCellFull));
	CHECK(OccupancyGrid::read_packed(block.data(), 4) == static_cast<uint8_t>(kCellUnknown));
	CHECK(OccupancyGrid::read_packed(block.data(), 5) == static_cast<uint8_t>(kCellFull));
	CHECK(OccupancyGrid::read_packed(block.data(), 6) == static_cast<uint8_t>(kCellSolid));
	CHECK(OccupancyGrid::read_packed(block.data(), 7) == static_cast<uint8_t>(kCellFull));
}

TEST_CASE("cell index inside a region is x-fastest and floor-modulo for negatives") {
	CHECK(OccupancyGrid::cell_index_in_region({0, 0, 0}) == 0);
	CHECK(OccupancyGrid::cell_index_in_region({1, 0, 0}) == 1);
	CHECK(OccupancyGrid::cell_index_in_region({0, 1, 0}) == kRegionBricks);
	CHECK(OccupancyGrid::cell_index_in_region({0, 0, 1}) == kRegionBricks * kRegionBricks);
	// Cell -1 is the LAST cell of the region below, not index -1.
	CHECK(OccupancyGrid::cell_index_in_region({-1, -1, -1}) == kRegionBrickCount - 1);
}

TEST_CASE("an unwritten grid is entirely unknown, and unknown counts as solid") {
	OccupancyGrid g;
	CHECK(g.region_count() == 0);
	CHECK(g.state({7, -3, 11}) == kCellUnknown);
	CHECK(g.is_known({7, -3, 11}) == false);
	// The safe direction (see the plan's Deliberate Decisions): a cell nobody has looked at
	// is treated as ground, so nothing falls because the world had not streamed in yet.
	CHECK(g.is_solid({7, -3, 11}) == true);
	CHECK(g.is_full({7, -3, 11}) == false);
	CHECK(g.block_seq({0, -1, 0}) == -1);
}

TEST_CASE("a stored block answers for every cell of its region and no others") {
	OccupancyGrid g;
	const std::vector<uint8_t> block = uniform_block(kCellSolid);
	g.set_block({0, -1, 0}, block.data(), 42);
	CHECK(g.region_count() == 1);
	CHECK(g.has_region({0, -1, 0}));
	CHECK(g.block_seq({0, -1, 0}) == 42);
	// Region (0,-1,0) owns bricks x,z in [0,32) and y in [-32,0).
	CHECK(g.state({0, -1, 0}) == kCellSolid);
	CHECK(g.state({31, -32, 31}) == kCellSolid);
	CHECK(g.is_known({31, -32, 31}));
	// One cell past the region on any axis is a different region: still unknown.
	CHECK(g.state({32, -1, 0}) == kCellUnknown);
	CHECK(g.state({0, 0, 0}) == kCellUnknown);
}

TEST_CASE("set_cell edits one cell of an existing block and creates a block when absent") {
	OccupancyGrid g;
	g.set_cell({3, 4, 5}, kCellFull, 7);
	CHECK(g.state({3, 4, 5}) == kCellFull);
	CHECK(g.is_full({3, 4, 5}));
	CHECK(g.is_solid({3, 4, 5}));
	CHECK(g.state({4, 4, 5}) == kCellUnknown); // the rest of the fresh block is unknown
	g.set_cell({4, 4, 5}, kCellAir, 8);
	CHECK(g.is_solid({4, 4, 5}) == false);
	CHECK(g.is_known({4, 4, 5}));
	CHECK(g.block_seq({0, 0, 0}) == 8); // a later write advances the block's sequence
}

TEST_CASE("a re-stored block replaces the old contents and never leaks the old sequence") {
	OccupancyGrid g;
	const std::vector<uint8_t> solid = uniform_block(kCellSolid);
	const std::vector<uint8_t> air = uniform_block(kCellAir);
	g.set_block({1, 0, 1}, solid.data(), 5);
	g.set_block({1, 0, 1}, air.data(), 9);
	CHECK(g.region_count() == 1);
	CHECK(g.state({32, 0, 32}) == kCellAir);
	CHECK(g.block_seq({1, 0, 1}) == 9);
	g.clear();
	CHECK(g.region_count() == 0);
	CHECK(g.state({32, 0, 32}) == kCellUnknown);
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: connectivity/occupancy.h: No such file or directory`

- [ ] **Step 4: Write the header**

Create `extension/src/connectivity/occupancy.h`:

```cpp
#pragma once
#include "world/region.h"
#include <cstdint>
#include <map>
#include <vector>

namespace ve {

// Spec §5's "global persistent occupancy grid, 0.8 m cells = one bit per brick". A cell and
// a brick are the same lattice: cell c spans world [c * kBrickSize, (c + 1) * kBrickSize).
inline constexpr float kOccupancyCellSize = kBrickSize; // 0.8 m
// Two bits per cell, four cells per byte: 32768 cells per region -> 8192 bytes.
inline constexpr int kOccupancyBlockBytes = kRegionBrickCount / 4;

// Two bits, because the flood fill and the thin-contact refinement want different questions
// answered and the mark pass already has both answers in registers (its 3^3 probe reduces a
// min and a max over the brick). kCellUnknown is the ZERO state on purpose: a freshly
// allocated or freshly released block reads as "nobody has looked", which is the state the
// grid must never confuse with "air".
enum CellState : uint8_t {
	kCellUnknown = 0, // never probed
	kCellAir = 1,     // probe found no solid sample
	kCellSolid = 2,   // some solid, some air -- a surface crosses the cell
	kCellFull = 3,    // no air sample at all
};

// Which cells hold matter, kept for the whole world and for as long as the world lives, in
// sparse per-region blocks. Regions arrive from the GPU mark pass (Task 8) and are never
// evicted from here even when the region leaves the atlas: 8 KB per region is cheap next to
// re-probing, and connectivity windows routinely reach past the residency ball.
//
// Not thread-safe. It is written and read on the main thread only; the render thread hands
// blocks over through VoxelWorld's occupancy inbox (Task 8).
class OccupancyGrid {
public:
	// `bytes` must hold kOccupancyBlockBytes. `seq` is the world's edit sequence number as
	// of the mark that produced it; IslandManager waits on it before trusting a window.
	void set_block(IVec3 region, const uint8_t *bytes, int64_t seq);
	// One cell, creating an all-unknown block if the region has none. The GPU never uses
	// this; it exists for tests and for the manager's own bookkeeping after a carve.
	void set_cell(IVec3 cell, CellState s, int64_t seq);

	CellState state(IVec3 cell) const;
	// NOT KNOWN TO BE AIR. Unknown counts as solid -- see the plan's Deliberate Decisions.
	bool is_solid(IVec3 cell) const { return state(cell) != kCellAir; }
	bool is_full(IVec3 cell) const { return state(cell) == kCellFull; }
	bool is_known(IVec3 cell) const { return state(cell) != kCellUnknown; }

	bool has_region(IVec3 region) const;
	int64_t block_seq(IVec3 region) const; // -1 when the region has no block
	int region_count() const { return static_cast<int>(blocks_.size()); }
	void clear();

	// 0..kRegionBrickCount-1, x fastest then y then z, floor-modulo so negative cells land
	// on the last cell of the region below rather than off the front of the block.
	static int cell_index_in_region(IVec3 cell);
	static uint8_t read_packed(const uint8_t *block, int index);
	static void write_packed(uint8_t *block, int index, uint8_t value);

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	static Key key(IVec3 r) { return Key{r.x, r.y, r.z}; }
	struct Block {
		std::vector<uint8_t> bytes;
		int64_t seq = -1;
	};
	Block *ensure(IVec3 region);

	std::map<Key, Block> blocks_;
};

} // namespace ve
```

- [ ] **Step 5: Write the implementation**

Create `extension/src/connectivity/occupancy.cpp`:

```cpp
#include "connectivity/occupancy.h"

namespace ve {

int OccupancyGrid::cell_index_in_region(IVec3 cell) {
	// floor_mod, not %, for the same reason brick_mark.comp.glsl uses `& 31`: the brick
	// lattice extends below y = 0 and C++'s % would give -1 for -1.
	const int x = floor_mod(cell.x, kRegionBricks);
	const int y = floor_mod(cell.y, kRegionBricks);
	const int z = floor_mod(cell.z, kRegionBricks);
	return x + y * kRegionBricks + z * kRegionBricks * kRegionBricks;
}

uint8_t OccupancyGrid::read_packed(const uint8_t *block, int index) {
	return static_cast<uint8_t>((block[index >> 2] >> ((index & 3) * 2)) & 0x3);
}

void OccupancyGrid::write_packed(uint8_t *block, int index, uint8_t value) {
	const int shift = (index & 3) * 2;
	uint8_t &b = block[index >> 2];
	b = static_cast<uint8_t>((b & ~(0x3 << shift)) | ((value & 0x3) << shift));
}

OccupancyGrid::Block *OccupancyGrid::ensure(IVec3 region) {
	Block &b = blocks_[key(region)];
	if (b.bytes.empty()) b.bytes.assign(kOccupancyBlockBytes, 0);
	return &b;
}

void OccupancyGrid::set_block(IVec3 region, const uint8_t *bytes, int64_t seq) {
	if (!bytes) return;
	Block *b = ensure(region);
	b->bytes.assign(bytes, bytes + kOccupancyBlockBytes);
	b->seq = seq;
}

void OccupancyGrid::set_cell(IVec3 cell, CellState s, int64_t seq) {
	Block *b = ensure(WorldBounds::region_of_brick(cell));
	write_packed(b->bytes.data(), cell_index_in_region(cell), static_cast<uint8_t>(s));
	if (seq > b->seq) b->seq = seq;
}

CellState OccupancyGrid::state(IVec3 cell) const {
	const auto it = blocks_.find(key(WorldBounds::region_of_brick(cell)));
	if (it == blocks_.end()) return kCellUnknown;
	return static_cast<CellState>(read_packed(it->second.bytes.data(),
			cell_index_in_region(cell)));
}

bool OccupancyGrid::has_region(IVec3 region) const {
	return blocks_.find(key(region)) != blocks_.end();
}

int64_t OccupancyGrid::block_seq(IVec3 region) const {
	const auto it = blocks_.find(key(region));
	return it == blocks_.end() ? -1 : it->second.seq;
}

void OccupancyGrid::clear() {
	blocks_.clear();
}

} // namespace ve
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd extension && scons test`
Expected: PASS — all six new cases plus every M1–M3 case.

- [ ] **Step 7: Commit**

```bash
git add extension/src/connectivity/occupancy.h extension/src/connectivity/occupancy.cpp \
        extension/tests/test_occupancy.cpp extension/SConstruct
git commit -m "feat: persistent 0.8 m occupancy grid"
```

---

### Task 2: `connectivity/flood_fill` — what is still attached to the world

Spec §5's "localized flood fill from the window boundary … boundary = anchored to static world" and "**6-connectivity (face-only)** defines support — edge/corner contact never counts, so pieces touching only through cell corners fall rather than wrongly hang."

**Files:**
- Create: `extension/src/connectivity/flood_fill.h`, `extension/src/connectivity/flood_fill.cpp`
- Test: `extension/tests/test_flood_fill.cpp`

**Interfaces:**
- Consumes: `ve::OccupancyGrid`, `ve::CellState` (Task 1).
- Produces: `ve::kFloodWindowCells`, `ve::kMaxWindowExpansions`, `ve::kFrontierMarginCells`, `ve::FloodWindow` (`around`, `contains`, `index`, `cell_of`, `cells`, `on_boundary`), `ve::LinkCuts` (`add`, `cut`, `size`, `clear`), `ve::FloodResult`, `ve::flood_anchored`. Task 3 labels its islands, Task 4 cuts its links, Task 13 drives it.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_flood_fill.cpp`:

```cpp
#include "connectivity/flood_fill.h"
#include "doctest.h"

using namespace ve;

namespace {

// A grid whose every cell is explicitly air, so a test can then carve solids into it and
// know that nothing is left unknown (unknown would anchor everything and prove nothing).
OccupancyGrid air_grid(IVec3 lo, IVec3 hi) {
	OccupancyGrid g;
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) g.set_cell({x, y, z}, kCellAir, 1);
	return g;
}

void fill(OccupancyGrid *g, IVec3 lo, IVec3 hi, CellState s) {
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) g->set_cell({x, y, z}, s, 2);
}

// A 16-cell window at the origin: small enough to read in a debugger, big enough to have an
// interior. Every test here uses it so the boundary shell is at 0 and 15 on each axis.
FloodWindow small_window() {
	FloodWindow w;
	w.lo = {0, 0, 0};
	w.dim = 16;
	return w;
}

} // namespace

TEST_CASE("a window addresses its cells x-fastest and rejects the outside") {
	const FloodWindow w = small_window();
	CHECK(w.cells() == 16 * 16 * 16);
	CHECK(w.index({0, 0, 0}) == 0);
	CHECK(w.index({1, 0, 0}) == 1);
	CHECK(w.index({0, 1, 0}) == 16);
	CHECK(w.index({0, 0, 1}) == 256);
	CHECK(w.index({16, 0, 0}) == -1);
	CHECK(w.index({-1, 0, 0}) == -1);
	CHECK(w.cell_of(256 + 16 + 1) == IVec3{1, 1, 1});
	CHECK(w.on_boundary({0, 5, 5}));
	CHECK(w.on_boundary({15, 5, 5}));
	CHECK(w.on_boundary({5, 5, 15}));
	CHECK(w.on_boundary({1, 5, 5}) == false);
}

TEST_CASE("FloodWindow::around centres a window on a cell AABB") {
	// The AABB spans cells x in [10, 13]; a dim-16 window centred on it starts 6 below the
	// centre 11 (integer midpoint), so the AABB sits comfortably inside the shell.
	const FloodWindow w = FloodWindow::around({10, 20, 30}, {13, 21, 30}, 16);
	CHECK(w.dim == 16);
	CHECK(w.contains({10, 20, 30}));
	CHECK(w.contains({13, 21, 30}));
	// ...and with at least the frontier margin of clearance on every side.
	CHECK(w.index({10 - kFrontierMarginCells, 20 - kFrontierMarginCells,
			30 - kFrontierMarginCells}) >= 0);
	CHECK(w.index({13 + kFrontierMarginCells, 21 + kFrontierMarginCells,
			30 + kFrontierMarginCells}) >= 0);
}

TEST_CASE("a solid slab reaching the shell is entirely anchored") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 3, 15}, kCellFull); // a floor spanning the whole window
	FloodResult r;
	flood_anchored(g, small_window(), nullptr, &r);
	CHECK(r.solid_count == 16 * 4 * 16);
	CHECK(r.anchored_count == r.solid_count);
	CHECK(r.frontier_reached == false);
	// The spec's invariant, stated as an assertion: an anchored cell never becomes an island.
	for (int i = 0; i < r.cells(); i++)
		CHECK((r.solid[i] && !r.anchored[i]) == false);
}

TEST_CASE("a floating cube in the middle of the window is not anchored") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull); // floor
	fill(&g, {7, 8, 7}, {8, 9, 8}, kCellFull);   // a 2x2x2 cube floating above it
	const FloodWindow w = small_window();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	CHECK(r.anchored[w.index({7, 0, 7})] == 1);
	CHECK(r.solid[w.index({7, 8, 7})] == 1);
	CHECK(r.anchored[w.index({7, 8, 7})] == 0);
	CHECK(r.solid_count - r.anchored_count == 8);
	CHECK(r.frontier_reached == false); // the cube is far from the shell
}

TEST_CASE("corner and edge contact do not carry support") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 4, 15}, kCellFull);
	// One cell diagonally above the floor's top layer: it shares only an EDGE with the
	// floor's corner cell, and spec §5 says edge/corner contact never counts.
	g.set_cell({5, 6, 5}, kCellFull, 2);
	g.set_cell({6, 5, 5}, kCellAir, 2); // make sure no face neighbour exists
	g.set_cell({5, 5, 5}, kCellAir, 2);
	const FloodWindow w = small_window();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	CHECK(r.solid[w.index({5, 6, 5})] == 1);
	CHECK(r.anchored[w.index({5, 6, 5})] == 0);
}

TEST_CASE("unknown cells seed the anchor set and conduct support") {
	// Nothing is set at all: the whole window is unknown, which is "not known to be air".
	OccupancyGrid g;
	const FloodWindow w = small_window();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	CHECK(r.solid_count == w.cells());
	CHECK(r.anchored_count == w.cells());
	CHECK(r.frontier_reached == false);
}

TEST_CASE("an unanchored cell near the shell raises the frontier flag") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull);
	// A floating cell one inside the shell -- within kFrontierMarginCells of the boundary,
	// so its true extent may continue outside the window and the caller must expand.
	g.set_cell({1, 8, 8}, kCellFull, 2);
	FloodResult r;
	flood_anchored(g, small_window(), nullptr, &r);
	CHECK(r.frontier_reached == true);
}

TEST_CASE("a cut link stops support crossing it") {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull); // floor
	fill(&g, {8, 2, 8}, {8, 6, 8}, kCellFull);   // a one-cell-thick pillar on it
	const FloodWindow w = small_window();

	FloodResult before;
	flood_anchored(g, w, nullptr, &before);
	CHECK(before.anchored[w.index({8, 6, 8})] == 1);

	// Cut the link between the floor's top cell (8,1,8) and the pillar's foot (8,2,8).
	LinkCuts cuts;
	cuts.add({8, 1, 8}, 1);
	CHECK(cuts.size() == 1);
	CHECK(cuts.cut({8, 1, 8}, 1));
	CHECK(cuts.cut({8, 1, 8}, 0) == false);
	CHECK(cuts.cut({8, 2, 8}, 1) == false); // a link is named by its LOWER cell

	FloodResult after;
	flood_anchored(g, w, &cuts, &after);
	CHECK(after.anchored[w.index({8, 6, 8})] == 0);
	CHECK(after.anchored[w.index({8, 2, 8})] == 0);
	CHECK(after.anchored[w.index({8, 1, 8})] == 1);
	CHECK(before.solid_count == after.solid_count);
	CHECK(after.anchored_count == before.anchored_count - 5);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: connectivity/flood_fill.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `extension/src/connectivity/flood_fill.h`:

```cpp
#pragma once
#include "connectivity/occupancy.h"
#include <cstdint>
#include <set>
#include <vector>

namespace ve {

// Spec §5: "localized flood fill from the window boundary (64^3 cells ~ 51 m, expanding if
// the frontier is reached)".
inline constexpr int kFloodWindowCells = 64;
// One expansion, doubling to 128^3 = 102.4 m. Two would be 204.8 m and 16.7 M cells, which
// is a 16 MB working set for a case the demo cannot produce: the guardrails in Task 3 split
// any component wider than 6.4 m long before a window that size could be justified.
inline constexpr int kMaxWindowExpansions = 1;
// An unanchored cell this close to the shell means the piece may continue past the window,
// where cells we never looked at could be holding it up. Two cells is 1.6 m.
inline constexpr int kFrontierMarginCells = 2;

// A cube of cells the fill works over. Cells are GLOBAL (a cell coordinate is a brick
// coordinate); `lo` is the minimum corner, inclusive, and the window spans `dim` cells on
// each axis. Window-local indices run x fastest, then y, then z.
struct FloodWindow {
	IVec3 lo{};
	int dim = kFloodWindowCells;

	int cells() const { return dim * dim * dim; }
	bool contains(IVec3 c) const;
	int index(IVec3 c) const;    // -1 when outside
	IVec3 cell_of(int index) const;
	bool on_boundary(IVec3 c) const; // in the outermost cell layer (the anchor shell)

	// The smallest `dim`-wide window centred on the inclusive cell AABB [lo_cell, hi_cell].
	// The caller picks dim; if the AABB does not fit, the window is still centred and the
	// fill will raise frontier_reached.
	static FloodWindow around(IVec3 lo_cell, IVec3 hi_cell, int dim);
};

// Face links the thin-contact refinement (Task 4) has severed. A link is named by its LOWER
// cell and an axis (0 = x, 1 = y, 2 = z): link (c, a) joins c and c + e_a, so every face in
// the grid has exactly one name and a cut set cannot alias.
class LinkCuts {
public:
	void add(IVec3 cell, int axis);
	bool cut(IVec3 cell, int axis) const;
	int size() const { return static_cast<int>(set_.size()); }
	void clear() { set_.clear(); }

private:
	struct Key {
		int x, y, z, a;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			if (x != o.x) return x < o.x;
			return a < o.a;
		}
	};
	std::set<Key> set_;
};

struct FloodResult {
	FloodWindow window{};
	std::vector<uint8_t> solid;    // 1 = not known to be air
	std::vector<uint8_t> anchored; // 1 = reachable from the shell through solid faces
	int solid_count = 0;
	int anchored_count = 0;
	// An unanchored solid cell sits within kFrontierMarginCells of the shell: the piece may
	// continue outside, so the caller should widen the window and re-run (Task 13).
	bool frontier_reached = false;

	int cells() const { return window.cells(); }
};

// Six-connected BFS over solid cells, seeded from every solid cell of the window's outermost
// layer. FACE connectivity only: spec §5 is explicit that "edge/corner contact never counts,
// so pieces touching only through cell corners fall rather than wrongly hang".
//
// `cuts` may be null. Cells outside the window are never visited, which is what makes the
// shell the definition of "anchored to the static world".
void flood_anchored(const OccupancyGrid &grid, const FloodWindow &w, const LinkCuts *cuts,
		FloodResult *out);

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/connectivity/flood_fill.cpp`:

```cpp
#include "connectivity/flood_fill.h"
#include <algorithm>

namespace ve {

namespace {

// The six face neighbours, as (axis, sign) so a step can be turned back into a link name:
// stepping +a from c crosses link (c, a); stepping -a from c crosses link (c - e_a, a).
constexpr int kAxis[6] = {0, 0, 1, 1, 2, 2};
constexpr int kSign[6] = {+1, -1, +1, -1, +1, -1};

IVec3 step(IVec3 c, int d) {
	IVec3 n = c;
	if (kAxis[d] == 0) n.x += kSign[d];
	else if (kAxis[d] == 1) n.y += kSign[d];
	else n.z += kSign[d];
	return n;
}

// The link crossed by stepping direction d out of c, named by its lower cell.
IVec3 link_cell(IVec3 c, int d) {
	return kSign[d] > 0 ? c : step(c, d);
}

} // namespace

bool FloodWindow::contains(IVec3 c) const {
	return c.x >= lo.x && c.x < lo.x + dim && c.y >= lo.y && c.y < lo.y + dim &&
			c.z >= lo.z && c.z < lo.z + dim;
}

int FloodWindow::index(IVec3 c) const {
	if (!contains(c)) return -1;
	return (c.x - lo.x) + (c.y - lo.y) * dim + (c.z - lo.z) * dim * dim;
}

IVec3 FloodWindow::cell_of(int i) const {
	return {lo.x + i % dim, lo.y + (i / dim) % dim, lo.z + i / (dim * dim)};
}

bool FloodWindow::on_boundary(IVec3 c) const {
	if (!contains(c)) return false;
	return c.x == lo.x || c.x == lo.x + dim - 1 || c.y == lo.y || c.y == lo.y + dim - 1 ||
			c.z == lo.z || c.z == lo.z + dim - 1;
}

FloodWindow FloodWindow::around(IVec3 lo_cell, IVec3 hi_cell, int dim) {
	FloodWindow w;
	w.dim = std::max(dim, 3); // a 3-cell window is the smallest with an interior at all
	const auto centre = [](int a, int b) { return a + (b - a) / 2; };
	w.lo = {centre(lo_cell.x, hi_cell.x) - w.dim / 2,
			centre(lo_cell.y, hi_cell.y) - w.dim / 2,
			centre(lo_cell.z, hi_cell.z) - w.dim / 2};
	return w;
}

void LinkCuts::add(IVec3 cell, int axis) {
	set_.insert(Key{cell.x, cell.y, cell.z, axis});
}

bool LinkCuts::cut(IVec3 cell, int axis) const {
	return set_.find(Key{cell.x, cell.y, cell.z, axis}) != set_.end();
}

void flood_anchored(const OccupancyGrid &grid, const FloodWindow &w, const LinkCuts *cuts,
		FloodResult *out) {
	out->window = w;
	const int n = w.cells();
	out->solid.assign(static_cast<size_t>(n), 0);
	out->anchored.assign(static_cast<size_t>(n), 0);
	out->solid_count = 0;
	out->anchored_count = 0;
	out->frontier_reached = false;

	// Pass 1: materialise the window. One grid lookup per cell, and the grid lookup is a map
	// find per cell; hoisting it here means the BFS below never touches the map again.
	for (int i = 0; i < n; i++) {
		if (grid.is_solid(w.cell_of(i))) {
			out->solid[i] = 1;
			out->solid_count++;
		}
	}

	// Pass 2: BFS from every solid shell cell. A plain vector used as a stack -- the order
	// does not matter, only reachability, and a stack keeps the working set small.
	std::vector<int> stack;
	stack.reserve(static_cast<size_t>(out->solid_count));
	for (int i = 0; i < n; i++) {
		if (!out->solid[i]) continue;
		const IVec3 c = w.cell_of(i);
		if (!w.on_boundary(c)) continue;
		out->anchored[i] = 1;
		out->anchored_count++;
		stack.push_back(i);
	}
	while (!stack.empty()) {
		const int i = stack.back();
		stack.pop_back();
		const IVec3 c = w.cell_of(i);
		for (int d = 0; d < 6; d++) {
			const IVec3 nc = step(c, d);
			const int ni = w.index(nc);
			if (ni < 0 || !out->solid[ni] || out->anchored[ni]) continue;
			if (cuts && cuts->cut(link_cell(c, d), kAxis[d])) continue;
			out->anchored[ni] = 1;
			out->anchored_count++;
			stack.push_back(ni);
		}
	}

	// Pass 3: does anything unanchored come close enough to the shell that the window may
	// have cut a real support link out of the picture?
	const int m = kFrontierMarginCells;
	for (int i = 0; i < n && !out->frontier_reached; i++) {
		if (!out->solid[i] || out->anchored[i]) continue;
		const IVec3 c = w.cell_of(i);
		const int dx = std::min(c.x - w.lo.x, w.lo.x + w.dim - 1 - c.x);
		const int dy = std::min(c.y - w.lo.y, w.lo.y + w.dim - 1 - c.y);
		const int dz = std::min(c.z - w.lo.z, w.lo.z + w.dim - 1 - c.z);
		if (std::min(dx, std::min(dy, dz)) <= m) out->frontier_reached = true;
	}
}

} // namespace ve
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd extension && scons test`
Expected: PASS — eight new cases.

- [ ] **Step 6: Commit**

```bash
git add extension/src/connectivity/flood_fill.h extension/src/connectivity/flood_fill.cpp \
        extension/tests/test_flood_fill.cpp
git commit -m "feat: six-connected flood fill for island anchoring"
```

---

### Task 3: `connectivity/components` — labelling the pieces that fall

Spec §5's "Solid cells unreachable from the boundary → connected-component labelling → each group = one island", plus two of its guardrails: "oversized components split along weakest box seams" and "components <~0.2 m³ become plain mesh debris".

**Files:**
- Create: `extension/src/connectivity/components.h`, `extension/src/connectivity/components.cpp`
- Test: `extension/tests/test_components.cpp`

**Interfaces:**
- Consumes: `ve::FloodResult`, `ve::FloodWindow` (Task 2).
- Produces: `ve::IslandComponent` (`cells`, `lo`, `hi`, `cell_count`, `extent_cells`, `world_aabb`), `ve::ComponentConfig`, `ve::kMaxIslandExtentCells`, `ve::label_islands`. Task 5 merges each component's cells into boxes, Task 13 turns each into an island.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_components.cpp`:

```cpp
#include "connectivity/components.h"
#include "doctest.h"
#include <algorithm>

using namespace ve;

namespace {

FloodWindow window16() {
	FloodWindow w;
	w.lo = {0, 0, 0};
	w.dim = 16;
	return w;
}

// A hand-built flood result: every listed cell is solid and unanchored, nothing else exists.
FloodResult floating(const FloodWindow &w, const std::vector<IVec3> &cells) {
	FloodResult r;
	r.window = w;
	r.solid.assign(static_cast<size_t>(w.cells()), 0);
	r.anchored.assign(static_cast<size_t>(w.cells()), 0);
	for (const IVec3 &c : cells) {
		const int i = w.index(c);
		REQUIRE(i >= 0);
		r.solid[i] = 1;
		r.solid_count++;
	}
	return r;
}

bool has_cell(const IslandComponent &c, IVec3 v) {
	return std::find(c.cells.begin(), c.cells.end(), v) != c.cells.end();
}

} // namespace

TEST_CASE("two separated blobs are two components, and each carries its own AABB") {
	const FloodWindow w = window16();
	const FloodResult r = floating(w, {{2, 2, 2}, {3, 2, 2}, {2, 3, 2}, {10, 10, 10}});
	std::vector<IslandComponent> out;
	label_islands(r, ComponentConfig{}, &out);
	REQUIRE(out.size() == 2);
	// Ordered by first cell in window index order, so the test can name them.
	CHECK(out[0].cell_count() == 3);
	CHECK(out[0].lo == IVec3{2, 2, 2});
	CHECK(out[0].hi == IVec3{3, 3, 2});
	CHECK(out[0].extent_cells(0) == 2);
	CHECK(out[0].extent_cells(2) == 1);
	CHECK(out[1].cell_count() == 1);
	CHECK(out[1].lo == IVec3{10, 10, 10});
}

TEST_CASE("cells touching only at a corner are two components") {
	const FloodWindow w = window16();
	const FloodResult r = floating(w, {{4, 4, 4}, {5, 5, 5}});
	std::vector<IslandComponent> out;
	label_islands(r, ComponentConfig{}, &out);
	CHECK(out.size() == 2);
}

TEST_CASE("anchored cells are never labelled") {
	const FloodWindow w = window16();
	FloodResult r = floating(w, {{4, 4, 4}, {5, 4, 4}});
	r.anchored[w.index({5, 4, 4})] = 1; // pretend the fill reached this one
	r.anchored_count = 1;
	std::vector<IslandComponent> out;
	label_islands(r, ComponentConfig{}, &out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].cell_count() == 1);
	CHECK(has_cell(out[0], {4, 4, 4}));
	CHECK(has_cell(out[0], {5, 4, 4}) == false);
}

TEST_CASE("a component wider than the volume can hold is split, losing no cells") {
	const FloodWindow w = window16();
	// A 12-cell-long bar: 9.6 m, past the 6.4 m an island volume can cover at its coarse
	// pitch, so it must come back as pieces that each fit.
	std::vector<IVec3> bar;
	for (int x = 1; x <= 12; x++) bar.push_back({x, 5, 5});
	const FloodResult r = floating(w, bar);

	ComponentConfig cfg;
	cfg.max_extent_cells = 8;
	std::vector<IslandComponent> out;
	label_islands(r, cfg, &out);
	CHECK(out.size() >= 2);
	int total = 0;
	for (const IslandComponent &c : out) {
		total += c.cell_count();
		CHECK(c.extent_cells(0) <= cfg.max_extent_cells);
		CHECK(c.extent_cells(1) <= cfg.max_extent_cells);
		CHECK(c.extent_cells(2) <= cfg.max_extent_cells);
	}
	CHECK(total == 12);
}

TEST_CASE("a component with more cells than the cap is split too") {
	const FloodWindow w = window16();
	std::vector<IVec3> block;
	for (int z = 1; z <= 6; z++)
		for (int y = 1; y <= 6; y++)
			for (int x = 1; x <= 6; x++) block.push_back({x, y, z});
	const FloodResult r = floating(w, block); // 216 cells inside an 8-cell extent

	ComponentConfig cfg;
	cfg.max_extent_cells = 8;
	cfg.max_cells = 64;
	std::vector<IslandComponent> out;
	label_islands(r, cfg, &out);
	CHECK(out.size() >= 4);
	int total = 0;
	for (const IslandComponent &c : out) {
		total += c.cell_count();
		CHECK(c.cell_count() <= cfg.max_cells);
	}
	CHECK(total == 216);
}

TEST_CASE("the split plane is the weakest seam, not the midpoint") {
	const FloodWindow w = window16();
	// A dumbbell along x: two 3x3x3 blobs joined by a one-cell neck at x = 6. Splitting at
	// the neck costs one crossing face; splitting anywhere inside a blob costs nine.
	std::vector<IVec3> cells;
	for (int z = 4; z <= 6; z++)
		for (int y = 4; y <= 6; y++) {
			for (int x = 3; x <= 5; x++) cells.push_back({x, y, z});
			for (int x = 7; x <= 9; x++) cells.push_back({x, y, z});
		}
	cells.push_back({6, 5, 5});
	const FloodResult r = floating(w, cells);

	ComponentConfig cfg;
	cfg.max_extent_cells = 6; // the dumbbell is 7 cells long, so it must split once
	std::vector<IslandComponent> out;
	label_islands(r, cfg, &out);
	REQUIRE(out.size() == 2);
	// Each side keeps its whole blob: a midpoint split would have cut a blob in half.
	CHECK(std::max(out[0].cell_count(), out[1].cell_count()) == 28);
	CHECK(std::min(out[0].cell_count(), out[1].cell_count()) == 27);
}

TEST_CASE("world AABB is the cell AABB in metres, half-open on the high side") {
	const FloodWindow w = window16();
	const FloodResult r = floating(w, {{2, 3, 4}});
	std::vector<IslandComponent> out;
	label_islands(r, ComponentConfig{}, &out);
	REQUIRE(out.size() == 1);
	float lo[3], hi[3];
	out[0].world_aabb(lo, hi);
	CHECK(lo[0] == doctest::Approx(2.0f * kOccupancyCellSize));
	CHECK(lo[1] == doctest::Approx(3.0f * kOccupancyCellSize));
	CHECK(hi[2] == doctest::Approx(5.0f * kOccupancyCellSize));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: connectivity/components.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `extension/src/connectivity/components.h`:

```cpp
#pragma once
#include "connectivity/flood_fill.h"
#include <vector>

namespace ve {

// A component may be no wider than the island volume that will hold it. An island volume is
// kIslandDim (64) samples with kIslandMarginVoxels (2) of clearance at each end, so its
// usable reach at the coarse 0.10 m pitch is (64 - 1 - 4) * 0.10 = 5.9 m -- seven 0.8 m
// cells. Wider components are split, which is spec §5's "oversized components split along
// weakest box seams". generator/volume_set.cpp static_asserts the relationship, so the two
// constants cannot drift apart silently.
inline constexpr int kMaxIslandExtentCells = 7;

struct ComponentConfig {
	int max_extent_cells = kMaxIslandExtentCells;
	// Also a guardrail on the box merge: kMaxIslandBoxes boxes cannot cover an arbitrary
	// 512-cell blob, and a component that needs more boxes than that is better split than
	// dropped. 512 cells is 262 m^3 of bounding volume -- far past anything the demo tools
	// can free in one shot.
	int max_cells = 512;
};

// One connected group of solid, unanchored cells: spec §5's "each group = one island".
struct IslandComponent {
	std::vector<IVec3> cells; // window index order; every cell is 0.8 m
	IVec3 lo{}, hi{};         // inclusive cell AABB

	int cell_count() const { return static_cast<int>(cells.size()); }
	int extent_cells(int axis) const;
	// The component's bounding box in metres. Half-open on the high side, because cell c
	// spans [c * 0.8, (c + 1) * 0.8): hi[a] is the far FACE of cell hi, not its origin.
	void world_aabb(float lo_m[3], float hi_m[3]) const;
};

// Six-connected labelling of every solid cell the flood left unanchored, then splitting.
// Output order is stable: components come out ordered by their lowest window index, and a
// split component's pieces immediately follow each other, so a test can name them.
//
// Splitting recursively cuts the offending component with an axis-aligned plane, choosing
// the axis by longest extent and the plane by FEWEST CROSSING FACES among the candidate
// positions -- the "weakest box seam". Recursion stops when both halves fit, and a piece
// that cannot be reduced further (a single cell) is emitted as it is.
void label_islands(const FloodResult &r, const ComponentConfig &cfg,
		std::vector<IslandComponent> *out);

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/connectivity/components.cpp`:

```cpp
#include "connectivity/components.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <tuple>

namespace ve {

namespace {

constexpr int kAxis[6] = {0, 0, 1, 1, 2, 2};
constexpr int kSign[6] = {+1, -1, +1, -1, +1, -1};

IVec3 step(IVec3 c, int d) {
	IVec3 n = c;
	if (kAxis[d] == 0) n.x += kSign[d];
	else if (kAxis[d] == 1) n.y += kSign[d];
	else n.z += kSign[d];
	return n;
}

int coord(IVec3 c, int axis) { return axis == 0 ? c.x : (axis == 1 ? c.y : c.z); }

void recompute_bounds(IslandComponent *c) {
	c->lo = c->cells.front();
	c->hi = c->cells.front();
	for (const IVec3 &v : c->cells) {
		c->lo = {std::min(c->lo.x, v.x), std::min(c->lo.y, v.y), std::min(c->lo.z, v.z)};
		c->hi = {std::max(c->hi.x, v.x), std::max(c->hi.y, v.y), std::max(c->hi.z, v.z)};
	}
}

bool fits(const IslandComponent &c, const ComponentConfig &cfg) {
	return c.cell_count() <= cfg.max_cells && c.extent_cells(0) <= cfg.max_extent_cells &&
			c.extent_cells(1) <= cfg.max_extent_cells &&
			c.extent_cells(2) <= cfg.max_extent_cells;
}

// The number of face links the plane "axis coordinate < p" would sever. Cheap: one hash
// probe per cell, no adjacency structure.
int seam_cost(const IslandComponent &c, int axis, int p,
		const std::map<std::tuple<int, int, int>, char> &present) {
	int cost = 0;
	for (const IVec3 &v : c.cells) {
		if (coord(v, axis) != p - 1) continue;
		IVec3 n = v;
		if (axis == 0) n.x++;
		else if (axis == 1) n.y++;
		else n.z++;
		if (present.count({n.x, n.y, n.z})) cost++;
	}
	return cost;
}

// Split `c` into two halves along its longest axis at the cheapest seam, appending both to
// `work`. A component of one cell cannot be split and is emitted as it stands.
void split(const IslandComponent &c, std::vector<IslandComponent> *work) {
	int axis = 0;
	for (int a = 1; a < 3; a++)
		if (c.extent_cells(a) > c.extent_cells(axis)) axis = a;
	const int lo = coord(c.lo, axis), hi = coord(c.hi, axis);
	if (hi == lo) { work->push_back(c); return; } // one cell thick everywhere: cannot split

	std::map<std::tuple<int, int, int>, char> present;
	for (const IVec3 &v : c.cells) present[{v.x, v.y, v.z}] = 1;

	// Candidate planes sit between lo and hi. Ties break towards the middle, so a uniform
	// blob (every seam equally costly) still halves instead of shaving one cell off an end.
	int best = lo + 1, best_cost = -1, best_bias = 0;
	const int mid = lo + (hi - lo + 1) / 2;
	for (int p = lo + 1; p <= hi; p++) {
		const int cost = seam_cost(c, axis, p, present);
		const int bias = std::abs(p - mid);
		if (best_cost < 0 || cost < best_cost || (cost == best_cost && bias < best_bias)) {
			best = p;
			best_cost = cost;
			best_bias = bias;
		}
	}

	IslandComponent a, b;
	for (const IVec3 &v : c.cells) (coord(v, axis) < best ? a : b).cells.push_back(v);
	if (a.cells.empty() || b.cells.empty()) { work->push_back(c); return; }
	recompute_bounds(&a);
	recompute_bounds(&b);
	work->push_back(a);
	work->push_back(b);
}

} // namespace

int IslandComponent::extent_cells(int axis) const {
	return coord(hi, axis) - coord(lo, axis) + 1;
}

void IslandComponent::world_aabb(float lo_m[3], float hi_m[3]) const {
	lo_m[0] = static_cast<float>(lo.x) * kOccupancyCellSize;
	lo_m[1] = static_cast<float>(lo.y) * kOccupancyCellSize;
	lo_m[2] = static_cast<float>(lo.z) * kOccupancyCellSize;
	hi_m[0] = static_cast<float>(hi.x + 1) * kOccupancyCellSize;
	hi_m[1] = static_cast<float>(hi.y + 1) * kOccupancyCellSize;
	hi_m[2] = static_cast<float>(hi.z + 1) * kOccupancyCellSize;
}

void label_islands(const FloodResult &r, const ComponentConfig &cfg,
		std::vector<IslandComponent> *out) {
	out->clear();
	const FloodWindow &w = r.window;
	const int n = w.cells();
	std::vector<uint8_t> seen(static_cast<size_t>(n), 0);
	std::vector<int> stack;

	for (int i = 0; i < n; i++) {
		if (seen[i] || !r.solid[i] || r.anchored[i]) continue;
		IslandComponent c;
		seen[i] = 1;
		stack.push_back(i);
		while (!stack.empty()) {
			const int j = stack.back();
			stack.pop_back();
			const IVec3 cell = w.cell_of(j);
			c.cells.push_back(cell);
			for (int d = 0; d < 6; d++) {
				const int nj = w.index(step(cell, d));
				if (nj < 0 || seen[nj] || !r.solid[nj] || r.anchored[nj]) continue;
				seen[nj] = 1;
				stack.push_back(nj);
			}
		}
		recompute_bounds(&c);

		// Split until every piece fits. Depth is bounded: each split strictly reduces the
		// longest extent or the cell count of both halves.
		std::vector<IslandComponent> work{c};
		while (!work.empty()) {
			const IslandComponent piece = work.back();
			work.pop_back();
			if (fits(piece, cfg)) { out->push_back(piece); continue; }
			const size_t before = work.size();
			split(piece, &work);
			// split() pushes the piece back unchanged when it cannot divide it; emit it
			// rather than looping for ever.
			if (work.size() == before + 1) {
				out->push_back(work.back());
				work.pop_back();
			}
		}
	}
}

} // namespace ve
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd extension && scons test`
Expected: PASS — seven new cases.

- [ ] **Step 6: Commit**

```bash
git add extension/src/connectivity/components.h extension/src/connectivity/components.cpp \
        extension/tests/test_components.cpp
git commit -m "feat: island component labelling with weakest-seam splitting"
```

---
### Task 4: `connectivity/contact_refine` — the true SDF arbitrates thin necks

Spec §5's *Marginal-contact refinement*: "when a component's only anchor links are thin (<~2 cell faces of contact), a tiny GPU check samples the true 5 cm SDF along the contact plane before declaring support. Cell grid decides the common case; true SDF arbitrates border cases — including contacts spanning chunk borders."

The candidates are exactly the **bridges** of the anchored cell graph: a link whose removal separates a seedless piece is, by definition, that piece's only anchor. Tarjan finds them all in one O(V + E) sweep, and only they are worth 81 field evaluations.

**Files:**
- Create: `extension/src/connectivity/contact_refine.h`, `extension/src/connectivity/contact_refine.cpp`
- Test: `extension/tests/test_contact_refine.cpp`

**Interfaces:**
- Consumes: `ve::FloodResult`, `ve::LinkCuts`, `ve::flood_anchored` (Task 2); `ve::eval_field` (`world/brick_eval.h`).
- Produces: `ve::ContactProbe` (abstract), `ve::ContactRefineConfig`, `ve::BridgeLink`, `ve::find_anchor_bridges`, `ve::refine_anchoring`, `ve::contact_samples_field`. Task 6 threads a `VolumeStore *` through `contact_samples_field`; Task 13 implements the probe against the edit log.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_contact_refine.cpp`:

```cpp
#include "connectivity/contact_refine.h"
#include "doctest.h"
#include <map>

using namespace ve;

namespace {

FloodWindow window16() {
	FloodWindow w;
	w.lo = {0, 0, 0};
	w.dim = 16;
	return w;
}

OccupancyGrid air_grid(IVec3 lo, IVec3 hi) {
	OccupancyGrid g;
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) g.set_cell({x, y, z}, kCellAir, 1);
	return g;
}

void fill(OccupancyGrid *g, IVec3 lo, IVec3 hi, CellState s) {
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) g->set_cell({x, y, z}, s, 2);
}

// A probe the test scripts: every link is a solid contact unless it appears in `thin`.
struct ScriptedProbe : ContactProbe {
	std::map<std::tuple<int, int, int, int>, int> thin;
	int fat = 81;
	mutable int calls = 0;

	int contact_samples(IVec3 c, int axis) const override {
		calls++;
		const auto it = thin.find({c.x, c.y, c.z, axis});
		return it == thin.end() ? fat : it->second;
	}
};

// A floor with a one-cell-thick pillar standing on it and a slab on top of the pillar.
// The link (8,1,8)+y is the pillar's only anchor and therefore the graph's bridge.
OccupancyGrid mushroom() {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull); // floor, touching the shell
	fill(&g, {8, 2, 8}, {8, 4, 8}, kCellFull);   // stalk
	fill(&g, {6, 5, 6}, {10, 5, 10}, kCellFull); // cap
	return g;
}

} // namespace

TEST_CASE("the only anchor link of a mushroom is found as a bridge") {
	const FloodWindow w = window16();
	const OccupancyGrid g = mushroom();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	CHECK(r.anchored[w.index({8, 5, 8})] == 1); // the cap is currently supported

	std::vector<BridgeLink> bridges;
	find_anchor_bridges(r, ContactRefineConfig{}, &bridges);
	bool found = false;
	for (const BridgeLink &b : bridges)
		if (b.cell == IVec3{8, 1, 8} && b.axis == 1) {
			found = true;
			// Everything above the floor hangs off it: stalk (3) + cap (25).
			CHECK(b.piece_cells == 28);
		}
	CHECK(found);
}

TEST_CASE("a solidly attached slab has no bridge at its base") {
	const FloodWindow w = window16();
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull);
	fill(&g, {5, 2, 5}, {9, 3, 9}, kCellFull); // a block sitting on 25 faces
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	std::vector<BridgeLink> bridges;
	find_anchor_bridges(r, ContactRefineConfig{}, &bridges);
	for (const BridgeLink &b : bridges) CHECK(b.axis != 1);
}

TEST_CASE("a thin contact is cut and the piece above becomes an island") {
	const FloodWindow w = window16();
	const OccupancyGrid g = mushroom();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);

	ScriptedProbe probe;
	probe.thin[{8, 1, 8, 1}] = 2; // only two of 81 samples are solid across that face
	LinkCuts cuts;
	const int made = refine_anchoring(g, probe, ContactRefineConfig{}, &cuts, &r);
	CHECK(made == 1);
	CHECK(cuts.cut({8, 1, 8}, 1));
	CHECK(r.anchored[w.index({8, 2, 8})] == 0);
	CHECK(r.anchored[w.index({8, 5, 8})] == 0);
	CHECK(r.anchored[w.index({8, 1, 8})] == 1); // the floor is untouched
}

TEST_CASE("a fat contact is left alone and the probe is asked once") {
	const FloodWindow w = window16();
	const OccupancyGrid g = mushroom();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	const int anchored_before = r.anchored_count;

	ScriptedProbe probe; // everything reads 81 solid samples
	LinkCuts cuts;
	CHECK(refine_anchoring(g, probe, ContactRefineConfig{}, &cuts, &r) == 0);
	CHECK(cuts.size() == 0);
	CHECK(r.anchored_count == anchored_before);
	CHECK(probe.calls >= 1);
}

TEST_CASE("cutting one neck exposes the next one, within the iteration budget") {
	const FloodWindow w = window16();
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull);
	fill(&g, {8, 2, 8}, {8, 8, 8}, kCellFull); // a tall stalk: every link on it is a bridge
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);

	ScriptedProbe probe;
	probe.thin[{8, 1, 8, 1}] = 0; // the foot is hollow
	probe.thin[{8, 4, 8, 1}] = 1; // ...and so is a joint further up
	LinkCuts cuts;
	ContactRefineConfig cfg;
	const int made = refine_anchoring(g, probe, cfg, &cuts, &r);
	// Both cuts land: the foot in the first pass, and the joint either in the same pass
	// (it is also a bridge before anything is cut) or in the next.
	CHECK(made == 2);
	CHECK(r.anchored[w.index({8, 2, 8})] == 0);
}

TEST_CASE("a bridge separating most of the window is not a candidate") {
	const FloodWindow w = window16();
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	// Two full halves joined by one cell. Whichever side the DFS roots in, the other is
	// huge, and a piece that big is the world, not a rock.
	fill(&g, {0, 0, 0}, {6, 15, 15}, kCellFull);
	fill(&g, {8, 0, 0}, {15, 15, 15}, kCellFull);
	g.set_cell({7, 8, 8}, kCellFull, 2);
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);

	ContactRefineConfig cfg;
	cfg.max_piece_cells = 512;
	std::vector<BridgeLink> bridges;
	find_anchor_bridges(r, cfg, &bridges);
	for (const BridgeLink &b : bridges) CHECK(b.piece_cells <= cfg.max_piece_cells);
}

TEST_CASE("contact_samples_field counts solid samples on the shared face") {
	AnalyticGenerator gen;
	// The face between cells (10,79,10) and (10,80,10) is the plane y = 80 * 0.8 = 64.0 m,
	// which is above the terrain everywhere (the surface is 51.2 +- 10 m), so the only solid
	// on it is what an op puts there.
	CHECK(contact_samples_field(gen, nullptr, 0, {10, 79, 10}, 1, 9) == 0);
	// ...and deep underground every sample is solid.
	CHECK(contact_samples_field(gen, nullptr, 0, {10, 20, 10}, 1, 9) == 81);

	// A ball of fill centred on that face. The face spans x, z in [8.0, 8.8]; a 0.2 m radius
	// meets the plane in a disc of 0.126 m^2 against the face's 0.64 m^2, so a fifth or so
	// of the 81 samples land inside it -- a partial contact, which is the interesting case.
	EditOp add{};
	add.type = kOpSphereAdd;
	add.material = 4;
	add.pos[0] = 8.4f;
	add.pos[1] = 64.0f;
	add.pos[2] = 8.4f;
	add.radius = 0.2f;
	const int n = contact_samples_field(gen, &add, 1, {10, 79, 10}, 1, 9);
	CHECK(n > 0);
	CHECK(n < 81);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: connectivity/contact_refine.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `extension/src/connectivity/contact_refine.h`:

```cpp
#pragma once
#include "connectivity/flood_fill.h"
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include <vector>

namespace ve {

// How the refinement asks whether two cells really touch. An interface for the same reason
// ve::ChunkProbe is one: the real implementation needs the generator, the edit log and the
// volume store, and the edit log's lock lives on the Godot side of the wall.
struct ContactProbe {
	virtual ~ContactProbe() = default;
	// Solid samples on the 0.8 m face between `cell` and `cell + e_axis`, of face_samples^2.
	virtual int contact_samples(IVec3 cell, int axis) const = 0;
};

struct ContactRefineConfig {
	// 9x9 over a 0.8 m face is an 8.9 cm pitch -- spec §5's "true 5 cm SDF along the contact
	// plane" at a density that costs 81 evaluations instead of 256.
	int face_samples = 9;
	// Below this many solid samples the link is severed. 8 of 81 is ~10% of the face, about
	// 0.064 m^2: a hand-sized bridge of rock, which is what "thin" has to mean if a stone
	// arch is to survive and a shattered ledge is not.
	int min_contact_samples = 8;
	// Cutting a link can expose the next one up a chain (a stalk of single cells is a chain
	// of bridges). Three passes covers the shapes the demo tools produce.
	int max_iterations = 3;
	// A bridge that separates more cells than this is the world hanging off the piece, not
	// the piece hanging off the world: never a candidate.
	int max_piece_cells = 512;
	// Field evaluations are the expensive part; cap how many links one pass may test.
	// Candidates are sorted by piece size ascending, so the most island-like go first.
	int max_candidates = 64;
};

// A link whose removal would separate `piece_cells` cells from every shell seed.
struct BridgeLink {
	IVec3 cell{}; // the LOWER cell of the link
	int axis = 0;
	int piece_cells = 0;
};

// Bridges of the anchored subgraph, DFS-rooted at the shell seeds, filtered by
// cfg.max_piece_cells and capped at cfg.max_candidates, smallest piece first.
// Iterative Tarjan: the window holds up to 2 M cells and recursion would overflow the stack.
void find_anchor_bridges(const FloodResult &r, const ContactRefineConfig &cfg,
		std::vector<BridgeLink> *out);

// Finds bridges, asks the probe about each, severs the thin ones and re-floods; repeats
// while cuts are still being made, up to cfg.max_iterations. Returns the number of links
// cut, and leaves `r` re-flooded so the caller can label islands from it directly.
int refine_anchoring(const OccupancyGrid &grid, const ContactProbe &probe,
		const ContactRefineConfig &cfg, LinkCuts *cuts, FloodResult *r);

// The probe's arithmetic, as a pure function: samples the field on the shared face between
// `cell` and `cell + e_axis` on a face_samples^2 lattice inset half a step from the edges,
// and counts how many are solid. This is what the Godot-side probe calls once it has the
// region's op list under the edit lock.
int contact_samples_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		int axis, int face_samples);

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/connectivity/contact_refine.cpp`:

```cpp
#include "connectivity/contact_refine.h"
#include "world/brick_eval.h"
#include <algorithm>

namespace ve {

namespace {

// Direction d is (axis d/2, sign +/-); d ^ 1 is its reverse, which is how the DFS skips the
// tree edge back to its parent without storing the parent node.
constexpr int kAxis[6] = {0, 0, 1, 1, 2, 2};
constexpr int kSign[6] = {+1, -1, +1, -1, +1, -1};

IVec3 step(IVec3 c, int d) {
	IVec3 n = c;
	if (kAxis[d] == 0) n.x += kSign[d];
	else if (kAxis[d] == 1) n.y += kSign[d];
	else n.z += kSign[d];
	return n;
}

IVec3 link_cell(IVec3 c, int d) { return kSign[d] > 0 ? c : step(c, d); }

struct Frame {
	int node = -1;
	int from = -1;  // the direction this node was entered by, or -1 at a root
	int next = 0;   // the next direction to try
};

} // namespace

void find_anchor_bridges(const FloodResult &r, const ContactRefineConfig &cfg,
		std::vector<BridgeLink> *out) {
	out->clear();
	const FloodWindow &w = r.window;
	const int n = w.cells();
	std::vector<int> disc(static_cast<size_t>(n), -1);
	std::vector<int> low(static_cast<size_t>(n), 0);
	std::vector<int> sub(static_cast<size_t>(n), 0);   // subtree cell count
	std::vector<int> seeds(static_cast<size_t>(n), 0); // shell cells in the subtree
	int timer = 0;

	std::vector<Frame> stack;
	std::vector<BridgeLink> found;
	for (int s = 0; s < n; s++) {
		if (disc[s] >= 0 || !r.solid[s] || !r.anchored[s]) continue;
		if (!w.on_boundary(w.cell_of(s))) continue; // roots are shell seeds only
		disc[s] = low[s] = timer++;
		sub[s] = 1;
		seeds[s] = 1;
		stack.push_back(Frame{s, -1, 0});
		while (!stack.empty()) {
			Frame &f = stack.back();
			if (f.next < 6) {
				const int d = f.next++;
				if (d == (f.from ^ 1)) continue; // the edge back to the parent
				const IVec3 c = w.cell_of(f.node);
				const int ni = w.index(step(c, d));
				if (ni < 0 || !r.solid[ni] || !r.anchored[ni]) continue;
				if (disc[ni] >= 0) {
					low[f.node] = std::min(low[f.node], disc[ni]);
					continue;
				}
				disc[ni] = low[ni] = timer++;
				sub[ni] = 1;
				seeds[ni] = w.on_boundary(w.cell_of(ni)) ? 1 : 0;
				stack.push_back(Frame{ni, d, 0});
				continue;
			}
			const Frame child = stack.back();
			stack.pop_back();
			if (stack.empty()) break;
			const int p = stack.back().node;
			low[p] = std::min(low[p], low[child.node]);
			sub[p] += sub[child.node];
			seeds[p] += seeds[child.node];
			if (low[child.node] > disc[p] && seeds[child.node] == 0) {
				// Removing this edge separates child's subtree from every shell seed.
				const IVec3 pc = w.cell_of(p);
				found.push_back(BridgeLink{link_cell(pc, child.from), kAxis[child.from],
						sub[child.node]});
			}
		}
	}

	found.erase(std::remove_if(found.begin(), found.end(),
						[&cfg](const BridgeLink &b) { return b.piece_cells > cfg.max_piece_cells; }),
			found.end());
	std::sort(found.begin(), found.end(), [](const BridgeLink &a, const BridgeLink &b) {
		return a.piece_cells < b.piece_cells;
	});
	if (static_cast<int>(found.size()) > cfg.max_candidates)
		found.resize(static_cast<size_t>(cfg.max_candidates));
	*out = std::move(found);
}

int refine_anchoring(const OccupancyGrid &grid, const ContactProbe &probe,
		const ContactRefineConfig &cfg, LinkCuts *cuts, FloodResult *r) {
	int total = 0;
	std::vector<BridgeLink> bridges;
	for (int iter = 0; iter < cfg.max_iterations; iter++) {
		find_anchor_bridges(*r, cfg, &bridges);
		int made = 0;
		for (const BridgeLink &b : bridges) {
			if (cuts->cut(b.cell, b.axis)) continue;
			if (probe.contact_samples(b.cell, b.axis) >= cfg.min_contact_samples) continue;
			cuts->add(b.cell, b.axis);
			made++;
		}
		if (made == 0) break;
		total += made;
		flood_anchored(grid, r->window, cuts, r);
	}
	return total;
}

int contact_samples_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		int axis, int face_samples) {
	if (face_samples < 1) return 0;
	// The face is the plane at the far side of `cell` along `axis`; the other two axes span
	// the cell's own extent. Samples are inset half a step so none lands on a corner shared
	// with three other faces, where a hairline of rock would read as contact on all of them.
	const int u = (axis + 1) % 3, v = (axis + 2) % 3;
	const int c[3] = {cell.x, cell.y, cell.z};
	float base[3];
	base[axis] = static_cast<float>(c[axis] + 1) * kOccupancyCellSize;
	base[u] = static_cast<float>(c[u]) * kOccupancyCellSize;
	base[v] = static_cast<float>(c[v]) * kOccupancyCellSize;
	const float step_m = kOccupancyCellSize / static_cast<float>(face_samples);

	int solid = 0;
	for (int j = 0; j < face_samples; j++)
		for (int i = 0; i < face_samples; i++) {
			float p[3] = {base[0], base[1], base[2]};
			p[u] += (static_cast<float>(i) + 0.5f) * step_m;
			p[v] += (static_cast<float>(j) + 0.5f) * step_m;
			if (eval_field(gen, ops, op_count, p[0], p[1], p[2]).sdf <= 0.0f) solid++;
		}
	return solid;
}

} // namespace ve
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd extension && scons test`
Expected: PASS — seven new cases.

- [ ] **Step 6: Commit**

```bash
git add extension/src/connectivity/contact_refine.h extension/src/connectivity/contact_refine.cpp \
        extension/tests/test_contact_refine.cpp
git commit -m "feat: thin-contact refinement arbitrates support with the true SDF"
```

---

### Task 5: `mesh/box_merge` — cells become a collision compound

Spec §5's "collision = **greedy box-merged compound** from 0.8 m occupancy (≤256 boxes)" and spec §8's `mesh/` responsibility "dual contouring, surface nets, **box merging**". The same box set does double duty: it is the island's Jolt compound *and* the CSG carve that removes the island from the terrain, which is why it has to be exact.

**Files:**
- Create: `extension/src/mesh/box_merge.h`, `extension/src/mesh/box_merge.cpp`
- Test: `extension/tests/test_box_merge.cpp`

**Interfaces:**
- Consumes: `ve::IVec3`, `ve::kOccupancyCellSize`.
- Produces: `ve::CellBox` (`lo`, `hi`, `cells`, `world_aabb`), `ve::kMaxIslandBoxes`, `ve::greedy_box_merge`. Task 6 turns each box into a `kOpBoxSubtract`, Task 9 uploads them as the extraction mask, Task 12 turns each into a Jolt box shape.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_box_merge.cpp`:

```cpp
#include "mesh/box_merge.h"
#include "doctest.h"
#include <algorithm>
#include <set>

using namespace ve;

namespace {

std::set<std::tuple<int, int, int>> cover(const std::vector<CellBox> &boxes) {
	std::set<std::tuple<int, int, int>> s;
	for (const CellBox &b : boxes)
		for (int z = b.lo.z; z <= b.hi.z; z++)
			for (int y = b.lo.y; y <= b.hi.y; y++)
				for (int x = b.lo.x; x <= b.hi.x; x++) {
					// A cell must be covered exactly once: overlapping boxes would double
					// an island's mass and subtract the same terrain twice.
					CHECK(s.insert({x, y, z}).second);
				}
	return s;
}

std::set<std::tuple<int, int, int>> as_set(const std::vector<IVec3> &cells) {
	std::set<std::tuple<int, int, int>> s;
	for (const IVec3 &c : cells) s.insert({c.x, c.y, c.z});
	return s;
}

} // namespace

TEST_CASE("a solid block merges to exactly one box") {
	std::vector<IVec3> cells;
	for (int z = 0; z < 3; z++)
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 5; x++) cells.push_back({x + 10, y - 2, z});
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge(cells, kMaxIslandBoxes, &boxes));
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].lo == IVec3{10, -2, 0});
	CHECK(boxes[0].hi == IVec3{14, 1, 2});
	CHECK(boxes[0].cells() == 60);
}

TEST_CASE("the boxes tile the input exactly, whatever its shape") {
	// An L in x/y extruded along z, plus a detached cube: nothing about this is convex.
	std::vector<IVec3> cells;
	for (int z = 0; z < 2; z++) {
		for (int x = 0; x < 6; x++) cells.push_back({x, 0, z});
		for (int y = 1; y < 4; y++) cells.push_back({0, y, z});
	}
	cells.push_back({9, 9, 9});
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge(cells, kMaxIslandBoxes, &boxes));
	CHECK(cover(boxes) == as_set(cells));
	CHECK(boxes.size() <= 4);
}

TEST_CASE("a checkerboard cannot merge and is refused above the cap") {
	std::vector<IVec3> cells;
	for (int z = 0; z < 6; z++)
		for (int y = 0; y < 6; y++)
			for (int x = 0; x < 6; x++)
				if ((x + y + z) % 2 == 0) cells.push_back({x, y, z});
	CHECK(cells.size() == 108);
	std::vector<CellBox> boxes;
	// No two cells share a face, so the merge needs one box each.
	CHECK(greedy_box_merge(cells, 108, &boxes));
	CHECK(boxes.size() == 108);
	CHECK(greedy_box_merge(cells, 64, &boxes) == false);
	CHECK(boxes.empty()); // a refused merge leaves nothing half-built
}

TEST_CASE("an empty input merges to nothing and succeeds") {
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge({}, kMaxIslandBoxes, &boxes));
	CHECK(boxes.empty());
}

TEST_CASE("duplicate input cells are absorbed, not double-counted") {
	std::vector<IVec3> cells{{1, 1, 1}, {1, 1, 1}, {2, 1, 1}};
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge(cells, kMaxIslandBoxes, &boxes));
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].cells() == 2);
}

TEST_CASE("a box's world AABB is its cell range in metres, half-open on the high side") {
	std::vector<CellBox> boxes;
	CHECK(greedy_box_merge({{3, -1, 2}}, kMaxIslandBoxes, &boxes));
	REQUIRE(boxes.size() == 1);
	float lo[3], hi[3];
	boxes[0].world_aabb(lo, hi);
	CHECK(lo[0] == doctest::Approx(3.0f * kOccupancyCellSize));
	CHECK(lo[1] == doctest::Approx(-1.0f * kOccupancyCellSize));
	CHECK(hi[0] == doctest::Approx(4.0f * kOccupancyCellSize));
	CHECK(hi[2] == doctest::Approx(3.0f * kOccupancyCellSize));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: mesh/box_merge.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `extension/src/mesh/box_merge.h`:

```cpp
#pragma once
#include "connectivity/occupancy.h"
#include "world/region.h"
#include <vector>

namespace ve {

// Spec §5 caps an island's collision compound at 256 boxes. M4 uses 64, because the SAME box
// set is also appended to the edit log as one kOpBoxSubtract each: 256 boxes would consume a
// region's entire 256-op budget on a single island, and every op is evaluated by every brick
// and every collision-mesh sample in that region for the rest of the session. 64 is more
// than the demo's tools free in one shot; components that need more are split (Task 3).
inline constexpr int kMaxIslandBoxes = 64;

// An inclusive range of 0.8 m occupancy cells.
struct CellBox {
	IVec3 lo{}, hi{};

	int cells() const {
		return (hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1);
	}
	// Half-open on the high side: cell hi spans up to (hi + 1) * 0.8.
	void world_aabb(float lo_m[3], float hi_m[3]) const;
};

// Greedy box merging: cells are visited in z, y, x order; each unconsumed cell grows as far
// as it can along +x, then the whole row grows along +y, then the whole slab along +z. The
// result TILES the input -- every cell is covered exactly once and nothing outside it is --
// which is what lets one box set serve as both the Jolt compound and the CSG carve.
//
// Returns false (and leaves `out` empty) when the merge would need more than max_boxes; the
// caller splits the component and tries again rather than shipping a partial carve.
bool greedy_box_merge(const std::vector<IVec3> &cells, int max_boxes,
		std::vector<CellBox> *out);

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/mesh/box_merge.cpp`:

```cpp
#include "mesh/box_merge.h"
#include <algorithm>
#include <map>
#include <tuple>

namespace ve {

void CellBox::world_aabb(float lo_m[3], float hi_m[3]) const {
	lo_m[0] = static_cast<float>(lo.x) * kOccupancyCellSize;
	lo_m[1] = static_cast<float>(lo.y) * kOccupancyCellSize;
	lo_m[2] = static_cast<float>(lo.z) * kOccupancyCellSize;
	hi_m[0] = static_cast<float>(hi.x + 1) * kOccupancyCellSize;
	hi_m[1] = static_cast<float>(hi.y + 1) * kOccupancyCellSize;
	hi_m[2] = static_cast<float>(hi.z + 1) * kOccupancyCellSize;
}

bool greedy_box_merge(const std::vector<IVec3> &cells, int max_boxes,
		std::vector<CellBox> *out) {
	out->clear();
	if (cells.empty()) return true;

	// A map rather than a dense grid: a component is at most a few hundred cells and may sit
	// anywhere in a 4 km world, so the dense array would be the expensive representation.
	// The value is the "consumed" flag.
	std::map<std::tuple<int, int, int>, char> live;
	for (const IVec3 &c : cells) live[{c.x, c.y, c.z}] = 0;

	const auto free_at = [&live](int x, int y, int z) {
		const auto it = live.find({x, y, z});
		return it != live.end() && it->second == 0;
	};

	// std::map's ordering on the tuple is (x, y, z) lexicographic; iterate explicitly in
	// z, y, x so growth along +x is always into cells the sweep has not reached yet.
	std::vector<IVec3> order;
	order.reserve(live.size());
	for (const auto &kv : live)
		order.push_back({std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first)});
	std::sort(order.begin(), order.end(), [](const IVec3 &a, const IVec3 &b) {
		if (a.z != b.z) return a.z < b.z;
		if (a.y != b.y) return a.y < b.y;
		return a.x < b.x;
	});

	for (const IVec3 &seed : order) {
		if (!free_at(seed.x, seed.y, seed.z)) continue;

		int x1 = seed.x;
		while (free_at(x1 + 1, seed.y, seed.z)) x1++;

		int y1 = seed.y;
		for (;;) {
			bool row = true;
			for (int x = seed.x; x <= x1 && row; x++) row = free_at(x, y1 + 1, seed.z);
			if (!row) break;
			y1++;
		}

		int z1 = seed.z;
		for (;;) {
			bool slab = true;
			for (int y = seed.y; y <= y1 && slab; y++)
				for (int x = seed.x; x <= x1 && slab; x++) slab = free_at(x, y, z1 + 1);
			if (!slab) break;
			z1++;
		}

		for (int z = seed.z; z <= z1; z++)
			for (int y = seed.y; y <= y1; y++)
				for (int x = seed.x; x <= x1; x++) live[{x, y, z}] = 1;

		if (static_cast<int>(out->size()) >= max_boxes) {
			out->clear();
			return false;
		}
		out->push_back(CellBox{seed, {x1, y1, z1}});
	}
	return true;
}

} // namespace ve
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd extension && scons test`
Expected: PASS — six new cases.

- [ ] **Step 6: Commit**

```bash
git add extension/src/mesh/box_merge.h extension/src/mesh/box_merge.cpp \
        extension/tests/test_box_merge.cpp
git commit -m "feat: greedy box merging of occupancy cells"
```

---
### Task 6: box and volume ops — the two halves of an island's lifecycle, in CSG

Spec §5 step 1 ("Carve out of the static SDF (automatic subtract op → correct crater)") and step 4 ("island SDF sampled at rest pose and stamped back as a CSG paste-op"). Both become ops on the same ordered per-region list M2 built, so every existing consumer — brick generation, the collision mesher, the edit-tool raycast, the LoD bakery in M5 — inherits them for free.

**Files:**
- Create: `extension/src/generator/volume_set.h`, `extension/src/generator/volume_set.cpp`
- Modify: `extension/src/generator/edit_ops.h`, `extension/src/generator/edit_ops.cpp`
- Modify: `extension/src/world/brick_eval.h`, `extension/src/world/brick_eval.cpp`
- Modify: `extension/src/world/raycast.h`, `extension/src/world/raycast.cpp`
- Modify: `extension/src/mesh/mesh_chunk.h`, `extension/src/mesh/mesh_chunk.cpp`
- Modify: `extension/src/connectivity/contact_refine.h`, `extension/src/connectivity/contact_refine.cpp`
- Test: `extension/tests/test_volume_ops.cpp`

**Interfaces:**
- Consumes: `ve::CellBox` (Task 5).
- Produces: `ve::kOpBoxSubtract`, `ve::kOpVolumeAdd`, `EditOp::aux`, `ve::pack_extent3`, `ve::unpack_extent3`, `ve::make_box_subtract`, `ve::make_volume_add`, `ve::op_world_aabb`, `ve::box_sdf`, `ve::VolumeSample`, `ve::VolumeStore`, `ve::kIslandDim`, `ve::kIslandVoxelCount`, `ve::kIslandVoxelFine`, `ve::kIslandVoxelCoarse`, `ve::kMaxVolumes`, `ve::VolumeData`, `ve::VolumeSet`, `ve::sample_volume_lattice`, `ve::resample_volume`, and the trailing `const VolumeStore *volumes = nullptr` parameter on `apply_op`, `apply_ops`, `eval_field`, `brick_has_surface`, `eval_brick`, `raycast`, `chunk_has_surface`, `contact_samples_field`. Task 7 mirrors all of it in GLSL; Tasks 9, 12 and 13 produce and consume the volumes.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_volume_ops.cpp`:

```cpp
#include "generator/volume_set.h"
#include "mesh/box_merge.h"
#include "world/brick_eval.h"
#include "doctest.h"
#include <cmath>

using namespace ve;

namespace {

// A volume holding a sphere of radius r centred in its own lattice, so the tests can check
// the sampler against an analytic answer rather than against itself.
VolumeData ball_volume(int dim, float voxel, float r, uint8_t material) {
	VolumeData v;
	v.dim = dim;
	v.sdf.assign(static_cast<size_t>(dim) * dim * dim, 0);
	v.mat.assign(static_cast<size_t>(dim) * dim * dim, 0);
	const float c = 0.5f * static_cast<float>(dim - 1) * voxel;
	for (int z = 0; z < dim; z++)
		for (int y = 0; y < dim; y++)
			for (int x = 0; x < dim; x++) {
				const float dx = x * voxel - c, dy = y * voxel - c, dz = z * voxel - c;
				const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - r;
				const int i = VolumeSet::voxel_index(dim, x, y, z);
				v.sdf[i] = encode_sdf(d);
				v.mat[i] = d <= 0.0f ? material : 0;
				if (d <= 0.0f) v.solid_voxels++;
			}
	return v;
}

} // namespace

TEST_CASE("a box op round-trips its cell range through 32 bytes") {
	const EditOp op = make_box_subtract({3, -2, 7}, {5, -2, 10});
	CHECK(op.type == kOpBoxSubtract);
	CHECK(sizeof(EditOp) == 32);
	float lo[3], hi[3];
	op_world_aabb(op, lo, hi);
	CHECK(lo[0] == doctest::Approx(3.0f * kOccupancyCellSize));
	CHECK(lo[1] == doctest::Approx(-2.0f * kOccupancyCellSize));
	CHECK(hi[0] == doctest::Approx(6.0f * kOccupancyCellSize));
	CHECK(hi[1] == doctest::Approx(-1.0f * kOccupancyCellSize));
	CHECK(hi[2] == doctest::Approx(11.0f * kOccupancyCellSize));
	int nx = 0, ny = 0, nz = 0;
	unpack_extent3(op.aux[0], &nx, &ny, &nz);
	CHECK(nx == 3);
	CHECK(ny == 1);
	CHECK(nz == 4);
}

TEST_CASE("extent packing survives every value it must carry") {
	for (int n : {1, 2, 63, 64, 1023}) {
		int a = 0, b = 0, c = 0;
		unpack_extent3(pack_extent3(n, 1, 1023), &a, &b, &c);
		CHECK(a == n);
		CHECK(b == 1);
		CHECK(c == 1023);
	}
}

TEST_CASE("box_sdf is the exact distance to an axis-aligned box") {
	const float lo[3] = {0.0f, 0.0f, 0.0f};
	const float hi[3] = {2.0f, 2.0f, 2.0f};
	CHECK(box_sdf(lo, hi, 1.0f, 1.0f, 1.0f) == doctest::Approx(-1.0f)); // centre
	CHECK(box_sdf(lo, hi, 3.0f, 1.0f, 1.0f) == doctest::Approx(1.0f));  // off one face
	CHECK(box_sdf(lo, hi, 3.0f, 3.0f, 1.0f) == doctest::Approx(std::sqrt(2.0f)));
	CHECK(box_sdf(lo, hi, 2.0f, 1.0f, 1.0f) == doctest::Approx(0.0f));  // on the face
}

TEST_CASE("a box subtract removes exactly the cells it names") {
	AnalyticGenerator gen;
	// Deep underground, so the base field is solid everywhere in the test region.
	const EditOp op = make_box_subtract({10, 20, 10}, {11, 20, 10});
	CHECK(eval_field(gen, nullptr, 0, 8.4f, 16.4f, 8.4f).sdf < 0.0f);
	// Inside the box: carved.
	CHECK(eval_field(gen, &op, 1, 8.4f, 16.4f, 8.4f).sdf > 0.0f);
	CHECK(eval_field(gen, &op, 1, 8.4f, 16.4f, 8.4f).material == 0);
	// Two cells wide on x (cells 10 and 11 -> world x in [8.0, 9.6)), one on y and z.
	CHECK(eval_field(gen, &op, 1, 9.4f, 16.4f, 8.4f).sdf > 0.0f);
	// Just outside on x: still solid.
	CHECK(eval_field(gen, &op, 1, 10.0f, 16.4f, 8.4f).sdf < 0.0f);
	// Just outside on y: still solid.
	CHECK(eval_field(gen, &op, 1, 8.4f, 15.6f, 8.4f).sdf < 0.0f);
}

TEST_CASE("a box subtract's re-mark ranges cover every cell it can flip") {
	const EditOp op = make_box_subtract({10, 20, 10}, {11, 20, 10});
	IVec3 lo{}, hi{};
	op_brick_range(op, &lo, &hi);
	// The box spans bricks 10..11 on x; the pad (kActivationPad + kVoxelSize = 0.2 m) is
	// under a brick, so the range is one brick out on every side.
	CHECK(lo.x <= 9);
	CHECK(hi.x >= 12);
	CHECK(lo.y <= 19);
	CHECK(hi.y >= 21);
	op_region_range(op, &lo, &hi);
	CHECK(lo.x <= 0);
	CHECK(hi.x >= 0);
}

TEST_CASE("a volume op adds exactly what its lattice holds") {
	AnalyticGenerator gen;
	VolumeSet volumes;
	const int slot = volumes.allocate();
	CHECK(slot == 0);
	const float origin[3] = {8.0f, 64.0f, 8.0f}; // above the terrain: base field is air
	VolumeData v = ball_volume(32, 0.05f, 0.4f, 2);
	CHECK(v.solid_voxels > 0);
	volumes.store(slot, std::move(v));
	const EditOp op = make_volume_add(slot, origin, 0.05f, 32);
	CHECK(op.type == kOpVolumeAdd);
	CHECK(op.radius == doctest::Approx(0.05f));
	CHECK(op.aux[0] == 0u);
	CHECK(op.aux[1] == 32u);

	// The lattice centre: 0.5 * 31 * 0.05 = 0.775 m in from the origin.
	const float cx = 8.0f + 0.775f, cy = 64.0f + 0.775f, cz = 8.0f + 0.775f;
	CHECK(eval_field(gen, nullptr, 0, cx, cy, cz).sdf > 0.0f); // air without the op
	const Sample s = eval_field(gen, &op, 1, cx, cy, cz, &volumes);
	CHECK(s.sdf < 0.0f);
	CHECK(s.sdf == doctest::Approx(-0.4f).epsilon(0.02));
	CHECK(s.material == 2);
	// Outside the ball but inside the lattice: air, and the distance is about right.
	CHECK(eval_field(gen, &op, 1, cx + 0.6f, cy, cz, &volumes).sdf ==
			doctest::Approx(0.2f).epsilon(0.1));
	// Far outside the lattice: the op contributes a positive distance and nothing else.
	CHECK(eval_field(gen, &op, 1, cx + 20.0f, cy, cz, &volumes).sdf > 0.0f);
}

TEST_CASE("a volume op with no store, or a released slot, is a no-op") {
	AnalyticGenerator gen;
	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const EditOp op = make_volume_add(0, origin, 0.05f, 32);
	const float cx = 8.775f, cy = 64.775f, cz = 8.775f;
	// Fail-soft (spec §8): a missing volume warns nowhere and changes nothing, rather than
	// putting a block of undefined bytes into the terrain.
	CHECK(eval_field(gen, &op, 1, cx, cy, cz, nullptr).sdf > 0.0f);
	VolumeSet volumes;
	CHECK(eval_field(gen, &op, 1, cx, cy, cz, &volumes).sdf > 0.0f);
}

TEST_CASE("the volume pool hands out every slot once and takes them back") {
	VolumeSet volumes;
	std::vector<int> slots;
	for (int i = 0; i < kMaxVolumes; i++) {
		const int s = volumes.allocate();
		CHECK(s >= 0);
		slots.push_back(s);
	}
	CHECK(volumes.allocate() == -1);
	CHECK(volumes.live_count() == kMaxVolumes);
	volumes.release(slots[7]);
	CHECK(volumes.live_count() == kMaxVolumes - 1);
	// Releasing frees the bytes: a 64-slot pool of 64^3 volumes is 33 MB and the manager
	// leans on release() to stay under spec §5's island-texture cap.
	CHECK(volumes.get(slots[7]) == nullptr);
	CHECK(volumes.allocate() == slots[7]);
	CHECK(volumes.live_count() == kMaxVolumes);
}

TEST_CASE("a slot can be claimed by index, once") {
	VolumeSet volumes;
	CHECK(volumes.reserve(3));
	CHECK(volumes.reserve(3) == false);
	CHECK(volumes.live_count() == 1);
	for (int i = 0; i < kMaxVolumes - 1; i++) CHECK(volumes.allocate() != 3);
}

TEST_CASE("a pinned slot can never be released or handed out again") {
	VolumeSet volumes;
	const int slot = volumes.allocate();
	volumes.store(slot, VolumeData{});
	volumes.pin(slot);
	CHECK(volumes.pinned(slot));
	volumes.release(slot);
	// Still live: an op in the edit log names this slot, and the GPU mirrors have no
	// liveness flag -- reusing it would silently swap one piece of rubble for another.
	CHECK(volumes.live_count() == 1);
	for (int i = 0; i < kMaxVolumes - 1; i++) CHECK(volumes.allocate() != slot);
	CHECK(volumes.allocate() == -1);
}

TEST_CASE("a volume resampled through the identity transform reproduces itself") {
	const float origin[3] = {8.0f, 64.0f, 8.0f};
	const VolumeData src = ball_volume(32, 0.05f, 0.4f, 2);
	// The body's local frame IS the birth world frame shifted to the lattice origin.
	const EditOp src_op = make_volume_add(0, origin, 0.05f, 32);
	const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	const float at[3] = {0.0f, 0.0f, 0.0f};

	VolumeData out;
	EditOp out_op{};
	CHECK(resample_volume(src, src_op, identity, at, 3, kIslandDim, &out, &out_op));
	CHECK(out_op.aux[0] == 3u);
	CHECK(out_op.radius == doctest::Approx(kIslandVoxelFine));
	CHECK(out.solid_voxels > 0);
	// The ball is still where it was, to within a voxel.
	const float cx = 8.775f, cy = 64.775f, cz = 8.775f;
	VolumeSample a{}, b{};
	CHECK(sample_volume_lattice(src.sdf.data(), src.mat.data(), src.dim, src_op.pos,
			src_op.radius, cx, cy, cz, &a));
	CHECK(sample_volume_lattice(out.sdf.data(), out.mat.data(), out.dim, out_op.pos,
			out_op.radius, cx, cy, cz, &b));
	CHECK(b.sdf == doctest::Approx(a.sdf).epsilon(0.15));
	CHECK(b.material == a.material);
}

TEST_CASE("a volume resampled through a rotation lands where the transform puts it") {
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	const VolumeData src = ball_volume(32, 0.05f, 0.3f, 2);
	const EditOp src_op = make_volume_add(0, origin, 0.05f, 32);
	// 90 degrees about y, then translated 100 m up. Row-major: local (x,y,z) -> world
	// (z, y, -x) + t.
	const float basis[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
	const float at[3] = {5.0f, 100.0f, 5.0f};

	VolumeData out;
	EditOp out_op{};
	CHECK(resample_volume(src, src_op, basis, at, 1, kIslandDim, &out, &out_op));
	// The ball's local centre (0.775, 0.775, 0.775) maps to world
	// (0.775 + 5, 0.775 + 100, -0.775 + 5).
	VolumeSample s{};
	CHECK(sample_volume_lattice(out.sdf.data(), out.mat.data(), out.dim, out_op.pos,
			out_op.radius, 5.775f, 100.775f, 4.225f, &s));
	CHECK(s.sdf == doctest::Approx(-0.3f).epsilon(0.2));
	CHECK(s.material == 2);
	// ...and the ball is NOT at the untransformed place any more.
	VolumeSample t{};
	CHECK(sample_volume_lattice(out.sdf.data(), out.mat.data(), out.dim, out_op.pos,
			out_op.radius, 0.775f, 0.775f, 0.775f, &t));
	CHECK(t.sdf > 0.0f);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: generator/volume_set.h: No such file or directory`

- [ ] **Step 3: Extend `generator/edit_ops.h`**

Replace the top of `extension/src/generator/edit_ops.h` (everything from `enum EditOpType` down to the `static_assert`) with:

```cpp
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
//   kOpBoxSubtract: pos = the box's minimum corner (always cell-aligned), radius unused,
//                   aux[0] = pack_extent3(cells on x, y, z), aux[1] unused
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
};
```

Then replace the function declarations at the bottom with:

```cpp
Sample apply_op(Sample s, const EditOp &op, float x, float y, float z,
		const VolumeStore *volumes = nullptr);
Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z,
		const VolumeStore *volumes = nullptr);

// 3 x 10 bits, values 1..1023 (0 is never a legal extent). An island cell box is at most 8
// cells on a side, so the range is enormous headroom; it exists because M5's LoD bakery will
// want box ops in world-scale units too.
uint32_t pack_extent3(int nx, int ny, int nz);
void unpack_extent3(uint32_t v, int *nx, int *ny, int *nz);

EditOp make_box_subtract(IVec3 lo_cell, IVec3 hi_cell); // inclusive 0.8 m cells
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
```

- [ ] **Step 4: Extend `generator/edit_ops.cpp`**

Replace `padded_range` and `apply_op`/`apply_ops` with:

```cpp
namespace {

float sphere_sdf(const EditOp &op, float x, float y, float z) {
	const float dx = x - op.pos[0], dy = y - op.pos[1], dz = z - op.pos[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz) - op.radius;
}

// Inclusive [lo, hi] cell range of the op's padded AABB on a lattice of the given pitch.
//
// Two margins, and the op's own extent covers neither. kVoxelSize is the brick's apron: its
// SDF lattice reaches one voxel past its own extent (kBrickSdfStride == 17), so an op
// grazing that plane still changes the bytes the brick stores. kActivationPad is the
// activation probe's: a CSG difference is a max and a union is a min, so BOTH move the
// field outside the shape itself -- a point d metres beyond a carved box reads -d, and once
// d is under the pad, a brick that was solidly interior starts reporting a surface. Those
// bricks flip active or inactive exactly like the ones inside the shape, and the streamer
// re-marks nothing but this range, so leaving them out means the GPU and the CPU disagree
// about whether they hold an atlas slot with nothing to ever settle it.
void padded_range(const EditOp &op, float pitch, IVec3 *lo, IVec3 *hi) {
	float a[3], b[3];
	op_world_aabb(op, a, b);
	const float pad = kActivationPad + kVoxelSize;
	const auto cell = [pitch](float v) { return static_cast<int>(std::floor(v / pitch)); };
	*lo = {cell(a[0] - pad), cell(a[1] - pad), cell(a[2] - pad)};
	*hi = {cell(b[0] + pad), cell(b[1] + pad), cell(b[2] + pad)};
}

} // namespace

uint32_t pack_extent3(int nx, int ny, int nz) {
	const auto c = [](int v) { return static_cast<uint32_t>(v < 1 ? 1 : (v > 1023 ? 1023 : v)); };
	return c(nx) | (c(ny) << 10) | (c(nz) << 20);
}

void unpack_extent3(uint32_t v, int *nx, int *ny, int *nz) {
	*nx = static_cast<int>(v & 0x3FFu);
	*ny = static_cast<int>((v >> 10) & 0x3FFu);
	*nz = static_cast<int>((v >> 20) & 0x3FFu);
}

EditOp make_box_subtract(IVec3 lo_cell, IVec3 hi_cell) {
	EditOp op{};
	op.type = kOpBoxSubtract;
	op.pos[0] = static_cast<float>(lo_cell.x) * kOccupancyCellSize;
	op.pos[1] = static_cast<float>(lo_cell.y) * kOccupancyCellSize;
	op.pos[2] = static_cast<float>(lo_cell.z) * kOccupancyCellSize;
	op.aux[0] = pack_extent3(hi_cell.x - lo_cell.x + 1, hi_cell.y - lo_cell.y + 1,
			hi_cell.z - lo_cell.z + 1);
	return op;
}

EditOp make_volume_add(int slot, const float origin[3], float voxel, int dim) {
	EditOp op{};
	op.type = kOpVolumeAdd;
	op.pos[0] = origin[0];
	op.pos[1] = origin[1];
	op.pos[2] = origin[2];
	op.radius = voxel;
	op.aux[0] = static_cast<uint32_t>(slot < 0 ? 0 : slot);
	op.aux[1] = static_cast<uint32_t>(dim < 1 ? 1 : dim);
	return op;
}

void op_world_aabb(const EditOp &op, float lo[3], float hi[3]) {
	switch (op.type) {
		case kOpBoxSubtract: {
			int n[3] = {1, 1, 1};
			unpack_extent3(op.aux[0], &n[0], &n[1], &n[2]);
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a];
				hi[a] = op.pos[a] + static_cast<float>(n[a]) * kOccupancyCellSize;
			}
			return;
		}
		case kOpVolumeAdd: {
			const float span = static_cast<float>(static_cast<int>(op.aux[1]) - 1) * op.radius;
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a];
				hi[a] = op.pos[a] + span;
			}
			return;
		}
		default:
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a] - op.radius;
				hi[a] = op.pos[a] + op.radius;
			}
			return;
	}
}

float box_sdf(const float lo[3], const float hi[3], float x, float y, float z) {
	const float p[3] = {x, y, z};
	float q[3];
	for (int a = 0; a < 3; a++) {
		const float c = 0.5f * (lo[a] + hi[a]);
		const float h = 0.5f * (hi[a] - lo[a]);
		q[a] = std::fabs(p[a] - c) - h;
	}
	const float outside = std::sqrt(std::max(q[0], 0.0f) * std::max(q[0], 0.0f) +
			std::max(q[1], 0.0f) * std::max(q[1], 0.0f) +
			std::max(q[2], 0.0f) * std::max(q[2], 0.0f));
	const float inside = std::min(std::max(q[0], std::max(q[1], q[2])), 0.0f);
	return outside + inside;
}

Sample apply_op(Sample s, const EditOp &op, float x, float y, float z,
		const VolumeStore *volumes) {
	switch (op.type) {
		case kOpSphereSubtract: {
			// CSG subtract: max(s, -sphere). A point that becomes air carries no material,
			// matching the generator's own convention (Sample::material == 0 above ground).
			const float sp = sphere_sdf(op, x, y, z);
			if (-sp > s.sdf) {
				s.sdf = -sp;
				if (s.sdf > 0.0f) s.material = 0;
			}
			return s;
		}
		case kOpSphereAdd: {
			// CSG union: min(s, sphere). The material changes only where the sphere is the
			// winning term and the result is solid — filling air, not recolouring rock.
			const float sp = sphere_sdf(op, x, y, z);
			if (sp < s.sdf) {
				s.sdf = sp;
				if (s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			}
			return s;
		}
		case kOpSpherePaint: {
			const float sp = sphere_sdf(op, x, y, z);
			if (sp <= 0.0f && s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			return s;
		}
		case kOpBoxSubtract: {
			float lo[3], hi[3];
			op_world_aabb(op, lo, hi);
			const float bd = box_sdf(lo, hi, x, y, z);
			if (-bd > s.sdf) {
				s.sdf = -bd;
				if (s.sdf > 0.0f) s.material = 0;
			}
			return s;
		}
		case kOpVolumeAdd: {
			VolumeSample vs{};
			// Fail-soft (spec §8): an op whose volume is gone contributes nothing at all,
			// rather than stamping undefined bytes into the terrain.
			if (!volumes || !volumes->sample(static_cast<int>(op.aux[0]), x, y, z, op, &vs))
				return s;
			if (vs.sdf < s.sdf) {
				s.sdf = vs.sdf;
				if (s.sdf <= 0.0f && vs.material != 0) s.material = vs.material;
			}
			return s;
		}
		default:
			return s;
	}
}

Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z,
		const VolumeStore *volumes) {
	for (int i = 0; i < count; i++) s = apply_op(s, ops[i], x, y, z, volumes);
	return s;
}
```

`edit_ops.cpp` now needs `#include "connectivity/occupancy.h"` for `kOccupancyCellSize`, and `<algorithm>` for `std::min`/`std::max`.

> **Dependency note:** `generator/` now includes one header from `connectivity/`. That is the correct direction — `connectivity/occupancy.h` includes only `world/region.h` — and it is why `kOccupancyCellSize` lives there rather than being redefined here.

- [ ] **Step 5: Write `generator/volume_set.h`**

Create `extension/src/generator/volume_set.h`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include "world/brick.h"
#include <cstdint>
#include <vector>

namespace ve {

// Spec §3 stores an island as a "dense per-island texture (AABB at 5 cm, uint8 + palette +
// own min-max mip)". M4 fixes the lattice at 64^3 and picks the pitch from the island's
// extent, which bounds every pool in the engine with one constant instead of a size class
// per island: 3.15 m of reach at 5 cm, 6.3 m at 10 cm, and components wider than that are
// split before they ever get here (ve::kMaxIslandExtentCells).
inline constexpr int kIslandDim = 64;
inline constexpr int kIslandVoxelCount = kIslandDim * kIslandDim * kIslandDim; // 262144
inline constexpr float kIslandVoxelFine = 0.05f;   // = kVoxelSize, spec §3's 5 cm
inline constexpr float kIslandVoxelCoarse = 0.10f; // spec §5's "halved ... for large AABBs"
// 64 slots x 256 KB of SDF + 256 KB of material = 33.5 MB, held on the CPU and mirrored on
// both devices. Spec §5 caps the island texture pool at ~512 MB; this is the number that
// forces an early merge long before that cap could bite.
inline constexpr int kMaxVolumes = 64;

// One dense volume: a lattice of encoded SDF plus one global material id per sample.
//
// A byte of material rather than spec §3's 2-bit palette index plus a palette: at 64^3 the
// difference is 192 KB on a pool that is nowhere near its ceiling, and it removes the whole
// palette-packing step from the extract shader and from its CPU reference.
struct VolumeData {
	int dim = kIslandDim;
	std::vector<uint8_t> sdf; // dim^3, ve::encode_sdf
	std::vector<uint8_t> mat; // dim^3, 0 = air
	int solid_voxels = 0;     // how many samples read solid; the island's mass comes from it

	bool empty() const { return sdf.empty(); }
	int voxel_count() const { return dim * dim * dim; }
};

// Trilinear SDF and nearest material from a dim^3 lattice placed at `origin` with pitch
// `voxel`. Mirrored exactly by sample_field_volume() in shaders/field.glslh.
//
// OUTSIDE the lattice's own box the function returns the distance TO that box, which is a
// sound lower bound on the distance to anything the volume contains (the contents are inside
// the box). For the union the op performs that can only ever tighten a positive distance,
// never add material, and it keeps sphere tracing conservative. The extraction pads the
// island so its outermost lattice shell is already positive, so the two branches agree at
// the seam.
bool sample_volume_lattice(const uint8_t *sdf, const uint8_t *mat, int dim,
		const float origin[3], float voxel, float x, float y, float z, VolumeSample *out);

// The pool of stored volumes, and the VolumeStore every field evaluation consults. The CPU
// copy is AUTHORITATIVE: the render device, the mesher's worker device and ve::raycast all
// read the same field, and only a CPU-side original can feed all three.
class VolumeSet : public VolumeStore {
public:
	int allocate();         // -1 when the pool is full
	// Claims one SPECIFIC free slot; false when it is already in use. It exists because a
	// volume's slot index is baked into the op that names it, so reloading a saved edit log
	// has to put each volume back where it was rather than wherever allocate() felt like.
	bool reserve(int slot);
	void release(int slot); // frees the bytes; refused on a pinned slot
	void store(int slot, VolumeData data);
	const VolumeData *get(int slot) const;
	int live_count() const { return live_; }
	// Bumped on every store(); the GPU mirrors re-upload a slot when their copy is behind.
	int64_t version(int slot) const;

	// Called the moment an EditOp referencing this slot enters the edit log. A pinned slot
	// can never be released or reused, which is what makes the GPU mirrors safe: the shader
	// has no liveness flag and reads whatever bytes the slot holds, so a slot that an op
	// still names must never come back as a different volume. Live island volumes are NOT
	// pinned -- nothing in the field references them until they are pasted at rest.
	void pin(int slot);
	bool pinned(int slot) const;

	bool sample(int slot, float x, float y, float z, const EditOp &op,
			VolumeSample *out) const override;

	static int voxel_index(int dim, int x, int y, int z) {
		return x + y * dim + z * dim * dim; // x fastest, as everywhere else
	}

private:
	struct Slot {
		VolumeData data;
		bool used = false;
		bool pinned = false;
		int64_t version = 0;
	};
	Slot slots_[kMaxVolumes];
	int live_ = 0;
	int64_t next_version_ = 1;
};

// Resample `src` -- stored in the body's LOCAL frame at src_op's origin/pitch/dim -- through
// the rigid transform (`basis` row-major 3x3 orthonormal, `origin` translation) into a fresh
// WORLD-AXIS-ALIGNED volume of `dim` samples, and fill in `out_op` as a kOpVolumeAdd for
// `slot`. This is spec §5's "island SDF sampled at rest pose".
//
// The pitch is the finest of kIslandVoxelFine / kIslandVoxelCoarse whose (dim - 1) * pitch
// covers the rotated AABB. Returns false when even the coarse pitch cannot -- the caller
// then falls back to box-add ops and logs (see the plan's Deliberate Deferrals).
bool resample_volume(const VolumeData &src, const EditOp &src_op, const float basis[9],
		const float origin[3], int slot, int dim, VolumeData *out, EditOp *out_op);

} // namespace ve
```

- [ ] **Step 6: Write `generator/volume_set.cpp`**

Create `extension/src/generator/volume_set.cpp`:

```cpp
#include "generator/volume_set.h"
#include <algorithm>
#include <cmath>

namespace ve {

bool sample_volume_lattice(const uint8_t *sdf, const uint8_t *mat, int dim,
		const float origin[3], float voxel, float x, float y, float z, VolumeSample *out) {
	if (!sdf || !mat || dim < 2 || voxel <= 0.0f) return false;
	const float span = static_cast<float>(dim - 1) * voxel;
	float lo[3] = {origin[0], origin[1], origin[2]};
	float hi[3] = {origin[0] + span, origin[1] + span, origin[2] + span};
	const float p[3] = {x, y, z};
	const float outside = box_sdf(lo, hi, x, y, z);
	if (outside > 0.0f) {
		out->sdf = outside;
		out->material = 0;
		return true;
	}

	float l[3];
	for (int a = 0; a < 3; a++)
		l[a] = std::min(std::max((p[a] - lo[a]) / voxel, 0.0f),
				static_cast<float>(dim - 1));
	const int i0[3] = {static_cast<int>(l[0]), static_cast<int>(l[1]), static_cast<int>(l[2])};
	const int i1[3] = {std::min(i0[0] + 1, dim - 1), std::min(i0[1] + 1, dim - 1),
			std::min(i0[2] + 1, dim - 1)};
	const float f[3] = {l[0] - i0[0], l[1] - i0[1], l[2] - i0[2]};
	const auto at = [&](int ax, int ay, int az) {
		return decode_sdf(sdf[VolumeSet::voxel_index(dim, ax, ay, az)]);
	};
	const float c00 = at(i0[0], i0[1], i0[2]) * (1 - f[0]) + at(i1[0], i0[1], i0[2]) * f[0];
	const float c10 = at(i0[0], i1[1], i0[2]) * (1 - f[0]) + at(i1[0], i1[1], i0[2]) * f[0];
	const float c01 = at(i0[0], i0[1], i1[2]) * (1 - f[0]) + at(i1[0], i0[1], i1[2]) * f[0];
	const float c11 = at(i0[0], i1[1], i1[2]) * (1 - f[0]) + at(i1[0], i1[1], i1[2]) * f[0];
	const float c0 = c00 * (1 - f[1]) + c10 * f[1];
	const float c1 = c01 * (1 - f[1]) + c11 * f[1];
	out->sdf = c0 * (1 - f[2]) + c1 * f[2];

	// Nearest, not interpolated: a material id is a label, and blending two labels is
	// meaningless. Same rule the brick atlas uses (mat_atlas is NEAREST filtered).
	const int m[3] = {static_cast<int>(l[0] + 0.5f), static_cast<int>(l[1] + 0.5f),
			static_cast<int>(l[2] + 0.5f)};
	out->material = mat[VolumeSet::voxel_index(dim, std::min(m[0], dim - 1),
			std::min(m[1], dim - 1), std::min(m[2], dim - 1))];
	return true;
}

int VolumeSet::allocate() {
	for (int i = 0; i < kMaxVolumes; i++) {
		if (slots_[i].used) continue;
		slots_[i].used = true;
		slots_[i].data = VolumeData{};
		slots_[i].version = next_version_++;
		live_++;
		return i;
	}
	return -1;
}

bool VolumeSet::reserve(int slot) {
	if (slot < 0 || slot >= kMaxVolumes || slots_[slot].used) return false;
	slots_[slot].used = true;
	slots_[slot].data = VolumeData{};
	slots_[slot].version = next_version_++;
	live_++;
	return true;
}

void VolumeSet::release(int slot) {
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return;
	if (slots_[slot].pinned) return; // an op still names it; see pin()
	slots_[slot].used = false;
	slots_[slot].data = VolumeData{};
	slots_[slot].data.sdf.shrink_to_fit();
	slots_[slot].data.mat.shrink_to_fit();
	slots_[slot].version = next_version_++;
	live_--;
}

void VolumeSet::store(int slot, VolumeData data) {
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return;
	slots_[slot].data = std::move(data);
	slots_[slot].version = next_version_++;
}

const VolumeData *VolumeSet::get(int slot) const {
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return nullptr;
	return slots_[slot].data.empty() ? nullptr : &slots_[slot].data;
}

int64_t VolumeSet::version(int slot) const {
	return slot >= 0 && slot < kMaxVolumes ? slots_[slot].version : 0;
}

void VolumeSet::pin(int slot) {
	if (slot >= 0 && slot < kMaxVolumes) slots_[slot].pinned = true;
}

bool VolumeSet::pinned(int slot) const {
	return slot >= 0 && slot < kMaxVolumes && slots_[slot].pinned;
}

bool VolumeSet::sample(int slot, float x, float y, float z, const EditOp &op,
		VolumeSample *out) const {
	const VolumeData *v = get(slot);
	if (!v) return false;
	return sample_volume_lattice(v->sdf.data(), v->mat.data(), v->dim, op.pos, op.radius,
			x, y, z, out);
}

bool resample_volume(const VolumeData &src, const EditOp &src_op, const float basis[9],
		const float origin[3], int slot, int dim, VolumeData *out, EditOp *out_op) {
	if (src.empty() || dim < 2) return false;

	// World AABB of the rotated source box: transform its eight corners.
	const float span = static_cast<float>(src.dim - 1) * src_op.radius;
	float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
	for (int c = 0; c < 8; c++) {
		const float q[3] = {src_op.pos[0] + ((c & 1) ? span : 0.0f),
				src_op.pos[1] + ((c & 2) ? span : 0.0f),
				src_op.pos[2] + ((c & 4) ? span : 0.0f)};
		for (int a = 0; a < 3; a++) {
			const float w = basis[a * 3 + 0] * q[0] + basis[a * 3 + 1] * q[1] +
					basis[a * 3 + 2] * q[2] + origin[a];
			wlo[a] = std::min(wlo[a], w);
			whi[a] = std::max(whi[a], w);
		}
	}
	float extent = 0.0f;
	for (int a = 0; a < 3; a++) extent = std::max(extent, whi[a] - wlo[a]);

	float pitch = kIslandVoxelFine;
	if (extent > static_cast<float>(dim - 1) * kIslandVoxelFine) pitch = kIslandVoxelCoarse;
	if (extent > static_cast<float>(dim - 1) * pitch) return false;

	// Centre the new lattice on the rotated AABB so the margin is shared on both sides,
	// which is what keeps the outermost shell positive (see sample_volume_lattice).
	float o[3];
	for (int a = 0; a < 3; a++) {
		const float slack = static_cast<float>(dim - 1) * pitch - (whi[a] - wlo[a]);
		o[a] = wlo[a] - 0.5f * slack;
	}

	out->dim = dim;
	out->sdf.assign(static_cast<size_t>(dim) * dim * dim, encode_sdf(kSdfRange));
	out->mat.assign(static_cast<size_t>(dim) * dim * dim, 0);
	out->solid_voxels = 0;
	for (int z = 0; z < dim; z++)
		for (int y = 0; y < dim; y++)
			for (int x = 0; x < dim; x++) {
				const float p[3] = {o[0] + x * pitch, o[1] + y * pitch, o[2] + z * pitch};
				// world -> local: basis is orthonormal, so its inverse is its transpose.
				const float d[3] = {p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
				float q[3];
				for (int a = 0; a < 3; a++)
					q[a] = basis[0 * 3 + a] * d[0] + basis[1 * 3 + a] * d[1] +
							basis[2 * 3 + a] * d[2];
				VolumeSample s{};
				if (!sample_volume_lattice(src.sdf.data(), src.mat.data(), src.dim,
							src_op.pos, src_op.radius, q[0], q[1], q[2], &s))
					continue;
				const int i = VolumeSet::voxel_index(dim, x, y, z);
				out->sdf[i] = encode_sdf(s.sdf);
				out->mat[i] = static_cast<uint8_t>(s.material);
				if (s.sdf <= 0.0f) out->solid_voxels++;
			}

	*out_op = make_volume_add(slot, o, pitch, dim);
	return true;
}

} // namespace ve
```

- [ ] **Step 7: Thread `volumes` through the four evaluators**

Each is a defaulted trailing parameter, so no existing call site changes.

`extension/src/world/brick_eval.h` — add `const VolumeStore *volumes = nullptr` to `eval_field`, `brick_has_surface` and `eval_brick`. `extension/src/world/brick_eval.cpp` — forward it in each body, including the two `eval_field` calls inside `spread_materials` (add a `const VolumeStore *volumes` parameter to that static helper and pass it from `eval_brick`):

```cpp
Sample eval_field(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, const VolumeStore *volumes) {
	return apply_ops(gen.sample(x, y, z), ops, op_count, x, y, z, volumes);
}
```

`extension/src/world/raycast.h` / `.cpp` — add the parameter to `raycast` and pass it to every `eval_field` inside.

`extension/src/mesh/mesh_chunk.h` / `.cpp` — add the parameter to `chunk_has_surface` and pass it to the probe's `eval_field`. Also replace `op_chunk_range`'s sphere assumption with the shared AABB:

```cpp
void op_chunk_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	float a[3], b[3];
	op_world_aabb(op, a, b);
	// Two mesh cells of pad: a CSG max/min changes the field far outside its own shape, but
	// only INSIDE it can it flip a sample's sign, and a sample whose sign it cannot flip only
	// shifts a vertex when it is itself within a cell of the surface. Two cells covers that
	// and the mesh overlap plane below the chunk origin.
	const float pad = 2.0f * kChunkCellSize;
	const auto cell = [](float v) { return static_cast<int>(std::floor(v / kChunkSize)); };
	*lo = {cell(a[0] - pad), cell(a[1] - pad), cell(a[2] - pad)};
	*hi = {cell(b[0] + pad), cell(b[1] + pad), cell(b[2] + pad)};
}
```

`extension/src/connectivity/contact_refine.h` / `.cpp` — add `const VolumeStore *volumes = nullptr` to `contact_samples_field` and forward it to `eval_field`.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS — twelve new cases in `test_volume_ops.cpp`, and every M1–M3 case unchanged (the defaulted parameter means nothing else had to move).

- [ ] **Step 9: Verify the extension still builds and the engine tests still pass**

Run: `./build.sh -j$(nproc)`
Expected: builds clean.

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: green — no shader has changed yet, so the field differential must be exactly as it was.

- [ ] **Step 10: Commit**

```bash
git add extension/src/generator extension/src/world/brick_eval.h extension/src/world/brick_eval.cpp \
        extension/src/world/raycast.h extension/src/world/raycast.cpp \
        extension/src/mesh/mesh_chunk.h extension/src/mesh/mesh_chunk.cpp \
        extension/src/connectivity/contact_refine.h extension/src/connectivity/contact_refine.cpp \
        extension/tests/test_volume_ops.cpp
git commit -m "feat: box-subtract and volume-add CSG ops with a CPU volume pool"
```

---
### Task 7: the GLSL mirror — box and volume ops on every device that evaluates the field

Spec §8: "One shared `common.glsl` for SDF/cel/lighting so raymarcher, LoD, and deferred pass can never drift apart", and the §8 testing bullet "GPU differential testing: CPU references for brick-eval and meshing; dev console command runs both and diffs — catches shader/reference drift." Two new op types means two new mirrors and one more differential case each.

**Files:**
- Create: `extension/src/render/volume_pool.h`, `extension/src/render/volume_pool.cpp`
- Modify: `shaders/field.glslh`
- Modify: `shaders/brick_gen.comp.glsl:1-25`, `shaders/brick_mark.comp.glsl:1-25`, `shaders/mesh_field.comp.glsl:1-20`, `shaders/field_probe.comp.glsl:1-15`
- Modify: `extension/src/render/gpu_atlas.h`, `extension/src/render/gpu_atlas.cpp`
- Modify: `extension/src/render/region_pass.cpp`, `extension/src/render/brick_gen_pass.cpp`
- Modify: `extension/src/render/mesh_pass.h`, `extension/src/render/mesh_pass.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Test: `tests/test_field_volume_diff.gd`

**Interfaces:**
- Consumes: `ve::VolumeData`, `ve::kIslandDim`, `ve::kIslandVoxelCount`, `ve::kMaxVolumes` (Task 6).
- Produces: `godot::VolumePool` (`initialize`, `teardown`, `is_valid`, `sdf_buffer`, `mat_buffer`, `upload`, `slots`, `dim`); `GpuAtlas::volumes()`; `MeshPass::upload_volume`; `VoxelWorld::debug_store_volume`; and `VoxelWorld::debug_eval_field` consulting a `ve::VolumeSet`. Task 9's extract shader includes the same `field.glslh`; Task 13 drives the uploads.

- [ ] **Step 1: Write the failing test**

Create `tests/test_field_volume_diff.gd`:

```gdscript
extends GdUnitTestSuite

# GPU/CPU differential test for M4's two new op types (spec section 8). shaders/field.glslh
# must agree with ve::apply_op or a carve leaves a ghost of the island behind and a paste
# stamps a different rock than the one the physics dropped.
#
# Structure and tolerances are lifted from tests/test_field_diff.gd: sin() is not
# bit-identical between glibc and a Vulkan driver, and the stored SDF is a uint8 with ~5 mm
# steps, so the gate is expressed in encoded steps rather than metres. Box and volume ops
# involve no transcendentals at all, so in practice they agree far more tightly than the
# sphere ops do -- the gate stays where it is because the BASE field is still in the sum.
const SDF_STEP := 1.28 / 255.0
const MAX_STEPS := 2.0
const TIGHT_FRACTION := 0.99

const OP_SUBTRACT := 0
const OP_BOX_SUBTRACT := 3
const OP_VOLUME_ADD := 4

# The volume the tests paste. 16^3 keeps the GDScript that builds it instant; the shader
# reads the dimension out of the op, and slot 0's byte offset is zero whatever the pool's
# per-slot stride is, so a 16^3 buffer is a legal pool of one.
const VDIM := 16
const VVOXEL := 0.05
const VORIGIN := Vector3(10.0, 64.0, 10.0) # above the terrain everywhere
const VRADIUS := 0.25
const VMATERIAL := 2

var _world: VoxelWorld
var _rd: RenderingDevice
var _worlds: Array = []

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)
	_worlds.append(_world)
	_rd = RenderingServer.create_local_rendering_device()

func after_test() -> void:
	if _rd != null:
		_rd.free()
		_rd = null
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func encode_sdf(d: float) -> int:
	# ve::encode_sdf: clamp to +-0.64 m, then 255 even steps.
	var t := clampf((d + 0.64) / 1.28, 0.0, 1.0)
	return int(floor(t * 255.0 + 0.5))

# A ball centred in its own lattice, as raw bytes for both sides.
func ball_volume() -> Array:
	var sdf := PackedByteArray()
	var mat := PackedByteArray()
	sdf.resize(VDIM * VDIM * VDIM)
	mat.resize(VDIM * VDIM * VDIM)
	var c := 0.5 * float(VDIM - 1) * VVOXEL
	for z in range(VDIM):
		for y in range(VDIM):
			for x in range(VDIM):
				var p := Vector3(x, y, z) * VVOXEL - Vector3(c, c, c)
				var d := p.length() - VRADIUS
				var i := x + y * VDIM + z * VDIM * VDIM
				sdf[i] = encode_sdf(d)
				mat[i] = VMATERIAL if d <= 0.0 else 0
	return [sdf, mat]

func make_op(type: int, material: int, pos: Vector3, radius: float,
		aux0: int = 0, aux1: int = 0) -> PackedByteArray:
	# Byte-identical to ve::EditOp: type, material, pos[3], radius, aux[2] — 32 bytes.
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type)
	b.put_u32(material)
	b.put_float(pos.x)
	b.put_float(pos.y)
	b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(aux0)
	b.put_u32(aux1)
	return b.data_array

func pack_extent3(nx: int, ny: int, nz: int) -> int:
	return nx | (ny << 10) | (nz << 20)

func box_op(lo: Vector3i, hi: Vector3i) -> PackedByteArray:
	# ve::make_box_subtract: pos is the minimum corner in metres, aux[0] the cell extent.
	return make_op(OP_BOX_SUBTRACT, 0, Vector3(lo) * 0.8, 0.0,
		pack_extent3(hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1), 0)

func volume_op(slot: int) -> PackedByteArray:
	return make_op(OP_VOLUME_ADD, 0, VORIGIN, VVOXEL, slot, VDIM)

func sample_points() -> PackedVector3Array:
	var pts := PackedVector3Array()
	var rng := RandomNumberGenerator.new()
	rng.seed = 20260815
	# A dense cloud over the pasted volume's own extent, where the interesting disagreements
	# would be, ...
	for i in range(384):
		pts.append(VORIGIN + Vector3(rng.randf_range(-0.4, 1.2), rng.randf_range(-0.4, 1.2),
			rng.randf_range(-0.4, 1.2)))
	# ...and a spread over the terrain the box ops carve.
	for i in range(384):
		pts.append(Vector3(rng.randf_range(-4.0, 20.0), rng.randf_range(38.0, 62.0),
			rng.randf_range(-4.0, 20.0)))
	return pts

func run_gpu(pts: PackedVector3Array, ops: PackedByteArray, op_count: int,
		vol: Array) -> PackedFloat32Array:
	var code: String = _world.debug_load_shader("res://shaders/field_probe.comp.glsl")
	assert_str(code).is_not_empty()
	code = code.replace("#[compute]\n", "")

	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = code
	var spirv := _rd.shader_compile_spirv_from_source(src)
	assert_str(spirv.compile_error_compute).is_empty()
	var shader := _rd.shader_create_from_spirv(spirv)
	assert_bool(shader.is_valid()).is_true()

	var op_bytes := ops.duplicate()
	if op_bytes.size() < 32:
		op_bytes.resize(32)
	var op_buf := _rd.storage_buffer_create(op_bytes.size(), op_bytes)

	var pt_bytes := PackedFloat32Array()
	for p in pts:
		pt_bytes.append_array(PackedFloat32Array([p.x, p.y, p.z, 0.0]))
	var pt_buf := _rd.storage_buffer_create(pt_bytes.size() * 4, pt_bytes.to_byte_array())
	var out_buf := _rd.storage_buffer_create(pts.size() * 16)
	var vsdf_buf := _rd.storage_buffer_create(vol[0].size(), vol[0])
	var vmat_buf := _rd.storage_buffer_create(vol[1].size(), vol[1])

	var uniforms := []
	for pair in [[0, op_buf], [1, pt_buf], [2, out_buf], [3, vsdf_buf], [4, vmat_buf]]:
		var u := RDUniform.new()
		u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		u.binding = pair[0]
		u.add_id(pair[1])
		uniforms.append(u)
	var uset := _rd.uniform_set_create(uniforms, shader, 0)
	var pipeline := _rd.compute_pipeline_create(shader)

	var push := PackedInt32Array([pts.size(), op_count, 0, 0]).to_byte_array()
	var list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(list, pipeline)
	_rd.compute_list_bind_uniform_set(list, uset, 0)
	_rd.compute_list_set_push_constant(list, push, push.size())
	_rd.compute_list_dispatch(list, (pts.size() + 63) / 64, 1, 1)
	_rd.compute_list_end()
	_rd.submit()
	_rd.sync()

	var out := _rd.buffer_get_data(out_buf).to_float32_array()
	for rid in [uset, pipeline, shader, op_buf, pt_buf, out_buf, vsdf_buf, vmat_buf]:
		_rd.free_rid(rid)
	return out

func compare(ops: PackedByteArray, op_count: int, label: String) -> void:
	var vol := ball_volume()
	# The CPU side reads the same bytes through VoxelWorld's own ve::VolumeSet.
	_world.debug_store_volume(0, vol[0], vol[1], VDIM)
	var pts := sample_points()
	var gpu := run_gpu(pts, ops, op_count, vol)
	assert_int(gpu.size()).is_equal(pts.size() * 4)

	var worst := 0.0
	var within_one := 0
	var mat_mismatch := 0
	var mat_compared := 0
	for i in range(pts.size()):
		var cpu: Vector2 = _world.debug_eval_field(pts[i], ops, op_count)
		var diff: float = absf(gpu[i * 4] - cpu.x) / SDF_STEP
		worst = maxf(worst, diff)
		if diff <= 1.0:
			within_one += 1
		if absf(cpu.x) > 4.0 * SDF_STEP:
			mat_compared += 1
			if int(gpu[i * 4 + 1]) != int(cpu.y):
				mat_mismatch += 1

	assert_float(worst).override_failure_message(
		"%s: worst sdf disagreement %.2f encoded steps" % [label, worst]).is_less(MAX_STEPS)
	assert_float(float(within_one) / float(pts.size())).override_failure_message(
		"%s: only %d/%d samples within one encoded step" % [label, within_one, pts.size()]
		).is_greater(TIGHT_FRACTION)
	assert_int(mat_compared).is_greater(pts.size() / 2)
	assert_int(mat_mismatch).override_failure_message(
		"%s: %d material mismatches" % [label, mat_mismatch]).is_equal(0)

func test_a_box_subtract_matches() -> void:
	# Cells 5..8 on x, 60..62 on y, 5..7 on z -> world [4.0, 7.2) x [48.0, 50.4) x [4.0, 6.4).
	compare(box_op(Vector3i(5, 60, 5), Vector3i(8, 62, 7)), 1, "box")

func test_a_chain_of_box_subtracts_matches() -> void:
	# What an island carve actually looks like: several boxes tiling one component.
	var ops := box_op(Vector3i(5, 60, 5), Vector3i(8, 62, 7))
	ops.append_array(box_op(Vector3i(9, 60, 5), Vector3i(9, 61, 7)))
	ops.append_array(box_op(Vector3i(5, 63, 5), Vector3i(6, 63, 6)))
	compare(ops, 3, "box chain")

func test_a_volume_add_matches() -> void:
	compare(volume_op(0), 1, "volume")

func test_a_carve_then_paste_chain_matches() -> void:
	# The full island lifecycle in one op list: the terrain is carved, and rubble is pasted
	# back somewhere else. Order matters on both sides.
	var ops := make_op(OP_SUBTRACT, 0, Vector3(8.0, 51.2, 8.0), 4.0)
	ops.append_array(box_op(Vector3i(5, 60, 5), Vector3i(8, 62, 7)))
	ops.append_array(volume_op(0))
	compare(ops, 3, "carve+paste")

func test_a_volume_op_naming_an_empty_slot_changes_nothing() -> void:
	# Fail-soft (spec section 8), and the one place the two sides could legitimately differ:
	# the CPU skips the op because the slot is empty, so the GPU must too. It does because
	# the manager never lets an op reach the log without its slot pinned -- this test pins
	# the CONTRACT by giving both sides an all-air volume and requiring they agree.
	var sdf := PackedByteArray()
	var mat := PackedByteArray()
	sdf.resize(VDIM * VDIM * VDIM)
	mat.resize(VDIM * VDIM * VDIM)
	sdf.fill(255) # encode_sdf(+0.64): solidly outside anything
	mat.fill(0)
	_world.debug_store_volume(0, sdf, mat, VDIM)
	var pts := sample_points()
	var gpu := run_gpu(pts, volume_op(0), 1, [sdf, mat])
	for i in range(pts.size()):
		var cpu: Vector2 = _world.debug_eval_field(pts[i], volume_op(0), 1)
		assert_float(absf(gpu[i * 4] - cpu.x) / SDF_STEP).is_less(MAX_STEPS)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_field_volume_diff.gd`
Expected: FAIL — `debug_store_volume` does not exist, and the probe shader has no volume bindings.

- [ ] **Step 3: Extend `shaders/field.glslh`**

Append the two op codes and the volume constants next to the existing ones:

```glsl
const uint OP_SPHERE_SUBTRACT = 0u;
const uint OP_SPHERE_ADD = 1u;
const uint OP_SPHERE_PAINT = 2u;
const uint OP_BOX_SUBTRACT = 3u;
const uint OP_VOLUME_ADD = 4u;
const uint MAX_REGION_OPS = 256u;
// ve::kOccupancyCellSize -- a box op's extent is counted in 0.8 m occupancy cells.
const float OCCUPANCY_CELL_SIZE = 0.8;
// ve::kIslandDim ^ 3: the per-slot stride of both volume buffers. render/volume_pool.cpp
// static_asserts that this matches, so changing the C++ constant breaks the BUILD.
const int VOLUME_VOXELS = 262144;
```

Add, after `apply_field_op`'s current neighbours and before it:

```glsl
// Mirror of ve::box_sdf (extension/src/generator/edit_ops.cpp).
float op_box_sdf(vec3 lo, vec3 hi, vec3 p) {
	vec3 c = 0.5 * (lo + hi);
	vec3 h = 0.5 * (hi - lo);
	vec3 q = abs(p - c) - h;
	return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

// The stored-volume pool. Two buffers rather than one so a slot's byte offset is
// slot * VOLUME_VOXELS in BOTH of them: a test can then stand up a one-slot pool without
// knowing how many slots the shipping pool has. Bytes are unpacked by hand out of uint
// words for the same reason ve::EditOp is unpacked by hand -- there is exactly one layout
// rule, and it is written here.
#ifdef FIELD_VOLUME_SDF_BINDING
layout(set = 0, binding = FIELD_VOLUME_SDF_BINDING, std430) readonly buffer FieldVolumeSdf {
	uint w[];
} field_volume_sdf;
layout(set = 0, binding = FIELD_VOLUME_MAT_BINDING, std430) readonly buffer FieldVolumeMat {
	uint w[];
} field_volume_mat;

uint volume_sdf_byte(int i) {
	return (field_volume_sdf.w[i >> 2] >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}
uint volume_mat_byte(int i) {
	return (field_volume_mat.w[i >> 2] >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}

// Mirror of ve::sample_volume_lattice (extension/src/generator/volume_set.cpp), including
// its outside-the-box rule: beyond the lattice the value is the distance TO the lattice box,
// which is a sound lower bound on the distance to anything inside it and therefore cannot
// add material through the union the op performs.
void sample_field_volume(int slot, vec3 origin, float voxel, int dim, vec3 p,
		out float sdf, out uint mat) {
	float span = float(dim - 1) * voxel;
	vec3 lo = origin, hi = origin + vec3(span);
	float outside = op_box_sdf(lo, hi, p);
	if (outside > 0.0) { sdf = outside; mat = 0u; return; }

	vec3 l = clamp((p - lo) / voxel, vec3(0.0), vec3(float(dim - 1)));
	ivec3 i0 = ivec3(l);
	ivec3 i1 = min(i0 + 1, ivec3(dim - 1));
	vec3 f = l - vec3(i0);
	int base = slot * VOLUME_VOXELS;
	int sy = dim, sz = dim * dim;
	float c000 = decode_sdf(float(volume_sdf_byte(base + i0.x + i0.y * sy + i0.z * sz)) / 255.0);
	float c100 = decode_sdf(float(volume_sdf_byte(base + i1.x + i0.y * sy + i0.z * sz)) / 255.0);
	float c010 = decode_sdf(float(volume_sdf_byte(base + i0.x + i1.y * sy + i0.z * sz)) / 255.0);
	float c110 = decode_sdf(float(volume_sdf_byte(base + i1.x + i1.y * sy + i0.z * sz)) / 255.0);
	float c001 = decode_sdf(float(volume_sdf_byte(base + i0.x + i0.y * sy + i1.z * sz)) / 255.0);
	float c101 = decode_sdf(float(volume_sdf_byte(base + i1.x + i0.y * sy + i1.z * sz)) / 255.0);
	float c011 = decode_sdf(float(volume_sdf_byte(base + i0.x + i1.y * sy + i1.z * sz)) / 255.0);
	float c111 = decode_sdf(float(volume_sdf_byte(base + i1.x + i1.y * sy + i1.z * sz)) / 255.0);
	sdf = mix(mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y),
	          mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y), f.z);

	// Nearest, not interpolated: a material id is a label and blending two labels is
	// meaningless. Same rule the brick material atlas uses.
	ivec3 m = min(ivec3(l + 0.5), ivec3(dim - 1));
	mat = volume_mat_byte(base + m.x + m.y * sy + m.z * sz);
}
#endif
```

Then replace `apply_field_op`'s body with the five-way form:

```glsl
void apply_field_op(uint index, vec3 p, inout float sdf, inout uint mat) {
	uvec4 a = field_op_pool.v[index * 2u + 0u];
	uvec4 b = field_op_pool.v[index * 2u + 1u];
	uint type = a.x;
	uint material = a.y;
	vec3 c = vec3(uintBitsToFloat(a.z), uintBitsToFloat(a.w), uintBitsToFloat(b.x));
	float radius = uintBitsToFloat(b.y);

	if (type == OP_BOX_SUBTRACT) {
		// aux[0] packs the cell extent as 3 x 10 bits (ve::pack_extent3).
		vec3 n = vec3(float(b.z & 0x3FFu), float((b.z >> 10) & 0x3FFu),
		              float((b.z >> 20) & 0x3FFu));
		float bd = op_box_sdf(c, c + n * OCCUPANCY_CELL_SIZE, p);
		if (-bd > sdf) { sdf = -bd; if (sdf > 0.0) mat = 0u; }
		return;
	}
	if (type == OP_VOLUME_ADD) {
#ifdef FIELD_VOLUME_SDF_BINDING
		// aux[0] = slot, aux[1] = lattice dimension; radius carries the voxel pitch.
		float vd;
		uint vm;
		sample_field_volume(int(b.z), c, radius, int(b.w), p, vd, vm);
		if (vd < sdf) { sdf = vd; if (sdf <= 0.0 && vm != 0u) mat = vm; }
#endif
		return;
	}

	float sp = length(p - c) - radius;
	if (type == OP_SPHERE_SUBTRACT) {
		if (-sp > sdf) { sdf = -sp; if (sdf > 0.0) mat = 0u; }
	} else if (type == OP_SPHERE_ADD) {
		if (sp < sdf) { sdf = sp; if (sdf <= 0.0) mat = material; }
	} else if (type == OP_SPHERE_PAINT) {
		if (sp <= 0.0 && sdf <= 0.0) mat = material;
	}
}
```

> **Why the `#ifdef` around the volume arm and not the box arm:** a box op needs no extra resource, so every field consumer can honour it unconditionally. A volume op needs two bindings, and a shader that has no use for volumes (there are none in M4, but M5's LoD probes may be one) must still compile. A consumer that omits the bindings silently ignores volume ops, which is the same fail-soft the CPU takes for an absent store — and `tests/test_field_volume_diff.gd` is what stops that silence from becoming a lie for the four consumers that DO define them.

- [ ] **Step 4: Bind the pool in the four field consumers**

Each gets two `#define`s above its includes and two `layout` lines it does not have to write (they come from `field.glslh`). Only the binding numbers differ.

`shaders/field_probe.comp.glsl` — header becomes:

```glsl
#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 0
#define FIELD_VOLUME_SDF_BINDING 3
#define FIELD_VOLUME_MAT_BINDING 4
#include "common.glslh"
#include "field.glslh"
```

`shaders/mesh_field.comp.glsl`:

```glsl
#define FIELD_OP_POOL_BINDING 1
#define FIELD_VOLUME_SDF_BINDING 2
#define FIELD_VOLUME_MAT_BINDING 3
```

`shaders/brick_gen.comp.glsl`:

```glsl
#define FIELD_OP_POOL_BINDING 7
#define FIELD_VOLUME_SDF_BINDING 8
#define FIELD_VOLUME_MAT_BINDING 9
```

`shaders/brick_mark.comp.glsl`:

```glsl
#define FIELD_OP_POOL_BINDING 4
#define FIELD_VOLUME_SDF_BINDING 7
#define FIELD_VOLUME_MAT_BINDING 8
```

(Bindings 5 and 6 in the mark pass are already the job list and the region slot tally; Task 8 takes binding 9 for occupancy.)

- [ ] **Step 5: Write `render/volume_pool`**

Create `extension/src/render/volume_pool.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "generator/volume_set.h"

namespace godot {

// One device's mirror of ve::VolumeSet. Two storage buffers -- SDF bytes and material bytes
// -- each holding `slots` lattices of `dim`^3 back to back, so a slot's offset is
// slot * dim^3 in both. shaders/field.glslh reads them through FIELD_VOLUME_SDF_BINDING and
// FIELD_VOLUME_MAT_BINDING.
//
// The CPU's ve::VolumeSet is authoritative (see the plan's Deliberate Decisions); this class
// only ever receives uploads. There is one instance per device: GpuAtlas owns the render
// device's, MeshPass owns the mesher worker's.
class VolumePool {
public:
	~VolumePool();

	bool initialize(RenderingDevice *rd, int slots, int dim);
	void teardown();
	bool is_valid() const { return sdf_.is_valid() && mat_.is_valid(); }

	RID sdf_buffer() const { return sdf_; }
	RID mat_buffer() const { return mat_; }
	int slots() const { return slots_; }
	int dim() const { return dim_; }

	// Copies one slot's bytes to the device. A device-level command: record it BEFORE
	// compute_list_begin, never inside an open list (M2 Task 12's ordering rule).
	// Rejects a mismatched dim rather than writing a torn lattice.
	bool upload(RenderingDevice *rd, int slot, const ve::VolumeData &data);

private:
	RenderingDevice *rd_ = nullptr;
	RID sdf_, mat_;
	int slots_ = 0, dim_ = 0;
};

} // namespace godot
```

Create `extension/src/render/volume_pool.cpp`:

```cpp
#include "render/volume_pool.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

// shaders/field.glslh hard-codes VOLUME_VOXELS (GLSL cannot include the header), and a
// mismatch would not fail anywhere -- it would silently read a neighbouring slot's bytes.
// Pin it here so changing the C++ constant breaks the BUILD, with the file that must follow
// named. (Same guard MeshPass uses for the chunk lattice.)
static_assert(ve::kIslandVoxelCount == 262144, "update VOLUME_VOXELS in shaders/field.glslh");
static_assert(ve::kIslandDim == 64, "update VOLUME_VOXELS in shaders/field.glslh");

VolumePool::~VolumePool() {
	teardown();
}

bool VolumePool::initialize(RenderingDevice *rd, int slots, int dim) {
	teardown();
	if (!rd || slots <= 0 || dim < 2) return false;
	rd_ = rd;
	slots_ = slots;
	dim_ = dim;
	const int64_t per_slot = static_cast<int64_t>(dim) * dim * dim;
	const int64_t bytes = per_slot * slots;
	PackedByteArray zero;
	zero.resize(bytes);
	// 255 is ve::encode_sdf(+0.64): an un-uploaded slot reads as solidly OUTSIDE everything,
	// so a stray reference to it can never add material. Zero would decode to -0.64 and
	// stamp a block of rock into the world.
	zero.fill(255);
	sdf_ = rd->storage_buffer_create(static_cast<uint32_t>(bytes), zero);
	zero.fill(0); // material 0 = air
	mat_ = rd->storage_buffer_create(static_cast<uint32_t>(bytes), zero);
	if (!is_valid()) {
		UtilityFunctions::printerr("VolumePool: buffer creation failed");
		teardown();
		return false;
	}
	return true;
}

void VolumePool::teardown() {
	if (rd_) {
		if (sdf_.is_valid()) rd_->free_rid(sdf_);
		if (mat_.is_valid()) rd_->free_rid(mat_);
	}
	sdf_ = RID();
	mat_ = RID();
	rd_ = nullptr;
	slots_ = 0;
	dim_ = 0;
}

bool VolumePool::upload(RenderingDevice *rd, int slot, const ve::VolumeData &data) {
	if (!rd || !is_valid() || slot < 0 || slot >= slots_) return false;
	if (data.dim != dim_ || data.empty()) return false;
	const int64_t per_slot = static_cast<int64_t>(dim_) * dim_ * dim_;
	if (static_cast<int64_t>(data.sdf.size()) != per_slot ||
			static_cast<int64_t>(data.mat.size()) != per_slot)
		return false;
	PackedByteArray b;
	b.resize(per_slot);
	std::memcpy(b.ptrw(), data.sdf.data(), static_cast<size_t>(per_slot));
	rd->buffer_update(sdf_, static_cast<uint32_t>(slot * per_slot),
			static_cast<uint32_t>(per_slot), b);
	std::memcpy(b.ptrw(), data.mat.data(), static_cast<size_t>(per_slot));
	rd->buffer_update(mat_, static_cast<uint32_t>(slot * per_slot),
			static_cast<uint32_t>(per_slot), b);
	return true;
}
```

- [ ] **Step 6: Wire the pool into the render device**

`extension/src/render/gpu_atlas.h`: `#include "render/volume_pool.h"`, add `VolumePool volumes_;` to the private section and

```cpp
	VolumePool &volumes() { return volumes_; }
	const VolumePool &volumes() const { return volumes_; }
```

to the public one. In `GpuAtlas::initialize`, after the existing buffers succeed:

```cpp
	if (!volumes_.initialize(rd, ve::kMaxVolumes, ve::kIslandDim)) {
		teardown();
		return false;
	}
```

and in `GpuAtlas::teardown`, call `volumes_.teardown()` **before** the RID frees (it owns its own device pointer and frees its own two buffers).

`extension/src/render/region_pass.cpp` — `mark_uset_` gains two entries:

```cpp
	storage(7, atlas.volumes().sdf_buffer()),
	storage(8, atlas.volumes().mat_buffer()),
```

`extension/src/render/brick_gen_pass.cpp` — `uset_` gains:

```cpp
	storage(8, atlas.volumes().sdf_buffer()),
	storage(9, atlas.volumes().mat_buffer()),
```

- [ ] **Step 7: Wire the pool into the mesher's worker device**

`extension/src/render/mesh_pass.h`: `#include "render/volume_pool.h"`, add `VolumePool volumes_;` and

```cpp
	// Uploads one stored volume to THIS device. Called on the worker thread only (the device
	// belongs to it); MeshService::submit_volume is the main thread's way in.
	bool upload_volume(int slot, const ve::VolumeData &data);
```

`extension/src/render/mesh_pass.cpp`: initialise `volumes_` alongside the other buffers, extend `field_uset_` to

```cpp
	field_uset_ = rd->uniform_set_create(Array::make(image(0, lattice_), storage(1, ops_),
			storage(2, volumes_.sdf_buffer()), storage(3, volumes_.mat_buffer())),
			field_shader_, 0);
```

tear `volumes_` down in `teardown()`, and add

```cpp
bool MeshPass::upload_volume(int slot, const ve::VolumeData &data) {
	// buffer_update is device-level and must not land inside an open compute list; the
	// worker only ever calls this between jobs, which is where that is guaranteed.
	return rd_ && !in_flight_ && volumes_.upload(rd_, slot, data);
}
```

- [ ] **Step 8: Give `VoxelWorld` a volume set and the debug hook**

`extension/src/voxel_world.h`: `#include "generator/volume_set.h"`, and in the private section

```cpp
	// The authoritative copy of every stored volume. Owned here because it outlives the GPU
	// objects exactly as the edit log does: a re-init re-uploads the same rubble.
	ve::VolumeSet volumes_;
```

with `ve::VolumeSet &volumes() { return volumes_; }` public, plus the hook

```cpp
	void debug_store_volume(int slot, const PackedByteArray &sdf, const PackedByteArray &mat,
			int dim);
```

`extension/src/voxel_world.cpp` — bind it in `_bind_methods` and implement:

```cpp
void VoxelWorld::debug_store_volume(int slot, const PackedByteArray &sdf,
		const PackedByteArray &mat, int dim) {
	const int64_t n = static_cast<int64_t>(dim) * dim * dim;
	if (dim < 2 || sdf.size() < n || mat.size() < n) {
		UtilityFunctions::printerr("debug_store_volume: short buffers for dim ", dim);
		return;
	}
	volumes_.reserve(slot); // no-op when the suite already claimed it
	ve::VolumeData d;
	d.dim = dim;
	d.sdf.assign(sdf.ptr(), sdf.ptr() + n);
	d.mat.assign(mat.ptr(), mat.ptr() + n);
	for (int64_t i = 0; i < n; i++)
		if (ve::decode_sdf(d.sdf[static_cast<size_t>(i)]) <= 0.0f) d.solid_voxels++;
	volumes_.store(slot, std::move(d));
}
```

Finally, `debug_eval_field` passes the store:

```cpp
Vector2 VoxelWorld::debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) const {
	std::vector<ve::EditOp> v(static_cast<size_t>(std::max(0, op_count)));
	if (!v.empty()) std::memcpy(v.data(), ops.ptr(), v.size() * sizeof(ve::EditOp));
	ve::AnalyticGenerator gen;
	const ve::Sample s = ve::eval_field(gen, v.data(), static_cast<int>(v.size()),
			p.x, p.y, p.z, &volumes_);
	return Vector2(s.sdf, static_cast<float>(s.material));
}
```

(keep whatever the existing body already does for op unpacking; the only change is the trailing `&volumes_` and making the method non-`const` or `volumes_` `mutable` — prefer dropping `const` from the method and updating the binding).

- [ ] **Step 9: Run the differential test**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_field_volume_diff.gd`
Expected: PASS — five cases.

- [ ] **Step 10: Run the whole suite — nothing that worked may have stopped**

Run: `cd extension && scons test`
Expected: PASS.

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: green. `test_field_diff.gd`, `test_brick_diff.gd`, `test_mesh_diff.gd` and `test_edit_pipeline.gd` all exercise the shaders whose bindings just moved; if any of them fails, the uniform set and the `#define` disagree.

- [ ] **Step 11: Commit**

```bash
git add shaders/field.glslh shaders/field_probe.comp.glsl shaders/mesh_field.comp.glsl \
        shaders/brick_gen.comp.glsl shaders/brick_mark.comp.glsl \
        extension/src/render/volume_pool.h extension/src/render/volume_pool.cpp \
        extension/src/render/gpu_atlas.h extension/src/render/gpu_atlas.cpp \
        extension/src/render/region_pass.cpp extension/src/render/brick_gen_pass.cpp \
        extension/src/render/mesh_pass.h extension/src/render/mesh_pass.cpp \
        extension/src/generator/volume_set.h extension/src/generator/volume_set.cpp \
        extension/tests/test_volume_ops.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_field_volume_diff.gd
git commit -m "feat: GPU mirror of box and volume ops with a per-device volume pool"
```

---
### Task 8: occupancy from the mark pass — filling the grid without a new pass

Spec §5: "Global persistent occupancy grid, 0.8 m cells = one bit per brick ('any solid voxel'), updated incrementally from brick-regen readback (1-frame latency — imperceptible)."

`brick_mark.comp.glsl` already probes every brick it scans on a 3³ lattice and reduces a min and a max. `solid` is `mn ≤ 0` and `full` is `mx ≤ 0`: both are already in registers, and both are answers the *generation* pass could never give, because generation only ever runs on surface bricks and a fully-solid interior brick holds no atlas slot at all.

**Files:**
- Modify: `shaders/brick_mark.comp.glsl`, `shaders/region_free.comp.glsl`
- Modify: `extension/src/world/brick_eval.h`, `extension/src/world/brick_eval.cpp`
- Modify: `extension/src/render/gpu_atlas.h`, `extension/src/render/gpu_atlas.cpp`
- Modify: `extension/src/render/region_pass.cpp`
- Modify: `extension/src/render/world_streamer.h`, `extension/src/render/world_streamer.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Test: `extension/tests/test_occupancy.cpp` (added cases), `tests/test_occupancy.gd`

**Interfaces:**
- Consumes: `ve::OccupancyGrid`, `ve::CellState`, `ve::kOccupancyBlockBytes` (Task 1).
- Produces: `ve::cell_state_field`; `GpuAtlas::region_occupancy()`; `godot::OccupancyBlock`; `WorldStreamer::initialize`'s three new arguments; `VoxelWorld::occupancy()`, `VoxelWorld::edit_seq()`, `debug_occupancy_state`, `debug_cell_state`, `debug_occupancy_stats`. Task 13 floods the grid this fills.

- [ ] **Step 1: Write the failing native test cases**

Add `#include "world/brick_eval.h"` to the include block at the top of
`extension/tests/test_occupancy.cpp`, then append:

```cpp
TEST_CASE("cell_state_field classifies air, surface and interior") {
	AnalyticGenerator gen;
	// The surface at x, z ~ 8 m sits near y = 54 m, i.e. cell y ~ 67.
	CHECK(cell_state_field(gen, nullptr, 0, {10, 100, 10}) == kCellAir);   // y = 80 m
	CHECK(cell_state_field(gen, nullptr, 0, {10, 20, 10}) == kCellFull);   // y = 16 m
	// Somewhere in between there is a cell the surface crosses.
	bool saw_surface = false;
	for (int y = 55; y < 80 && !saw_surface; y++)
		saw_surface = cell_state_field(gen, nullptr, 0, {10, y, 10}) == kCellSolid;
	CHECK(saw_surface);
	// Never kCellUnknown: the field always has an answer, and only the GRID has "nobody
	// looked". Anything else would let a probed cell masquerade as an anchor.
	for (int y = 0; y < 120; y++)
		CHECK(cell_state_field(gen, nullptr, 0, {10, y, 10}) != kCellUnknown);
}

TEST_CASE("an op that empties a cell moves it from full to air") {
	AnalyticGenerator gen;
	const IVec3 cell{10, 20, 10};
	CHECK(cell_state_field(gen, nullptr, 0, cell) == kCellFull);
	EditOp cut{};
	cut.type = kOpSphereSubtract;
	cut.pos[0] = 8.4f;
	cut.pos[1] = 16.4f;
	cut.pos[2] = 8.4f;
	cut.radius = 3.0f; // comfortably swallows the 0.8 m cell and its probe lattice
	CHECK(cell_state_field(gen, &cut, 1, cell) == kCellAir);
}
```

- [ ] **Step 2: Run the native tests to verify they fail**

Run: `cd extension && scons test`
Expected: FAIL — `'cell_state_field' was not declared in this scope`

- [ ] **Step 3: Add the CPU mirror**

`extension/src/world/brick_eval.h` — add `#include "connectivity/occupancy.h"` and, next to `brick_has_surface`:

```cpp
// The occupancy classification shaders/brick_mark.comp.glsl writes, as a pure function: the
// same 3^3 probe brick_has_surface uses, reduced to spec §5's two bits. Never returns
// kCellUnknown -- the field always answers; only the GRID has a "nobody looked" state.
CellState cell_state_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		const VolumeStore *volumes = nullptr);
```

`extension/src/world/brick_eval.cpp` — factor the probe out of `brick_has_surface` so the two callers cannot drift:

```cpp
namespace {

// The 3^3 activation probe, reduced. Both brick_has_surface and cell_state_field read it,
// and shaders/brick_mark.comp.glsl computes exactly this once per brick and uses it twice.
void brick_probe(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		const VolumeStore *volumes, float *mn, float *mx) {
	float bo[3];
	brick_world_origin(brick, bo);
	*mn = 1e30f;
	*mx = -1e30f;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				const float d = eval_field(gen, ops, op_count,
						bo[0] + sx * (kBrickVoxels / 2) * kVoxelSize,
						bo[1] + sy * (kBrickVoxels / 2) * kVoxelSize,
						bo[2] + sz * (kBrickVoxels / 2) * kVoxelSize, volumes).sdf;
				*mn = std::min(*mn, d);
				*mx = std::max(*mx, d);
			}
}

} // namespace

bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		const VolumeStore *volumes) {
	float mn = 0.0f, mx = 0.0f;
	brick_probe(gen, ops, op_count, brick, volumes, &mn, &mx);
	return mn < kActivationPad && mx > -kActivationPad;
}

CellState cell_state_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		const VolumeStore *volumes) {
	float mn = 0.0f, mx = 0.0f;
	brick_probe(gen, ops, op_count, cell, volumes, &mn, &mx);
	if (mn > 0.0f) return kCellAir;
	return mx <= 0.0f ? kCellFull : kCellSolid;
}
```

- [ ] **Step 4: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

- [ ] **Step 5: Write the failing engine test**

Create `tests/test_occupancy.gd`:

```gdscript
extends GdUnitTestSuite

# Spec section 5's occupancy grid, filled from the mark pass. The grid is what the flood fill
# reads, so if it disagrees with the field the wrong rocks fall — this test pins the two
# together on a streamed world and across an edit.
#
# CellState: 0 unknown, 1 air, 2 solid, 3 full (ve::CellState).
const UNKNOWN := 0
const AIR := 1
const SOLID := 2
const FULL := 3

const CENTER := Vector3(20.0, 56.0, 20.0)

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.residency_radius_m = 30.0
	# Sizing rule of thumb (see test_streaming.gd): only regions CROSSING the surface hold
	# bricks, ~1500 each for the demo hills, and a 30 m ball holds a handful of them.
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w

# The readback ring carries at most eight regions at a time and the request goes out one
# frame after the mark, so a freshly streamed ball takes a few dozen frames to be fully
# described. Settle until the block count stops growing.
func settle(w: VoxelWorld, center: Vector3, frames := 300) -> void:
	var last := -1
	var quiet := 0
	for i in range(frames):
		w.debug_stream_frame(center)
		var n: int = w.debug_occupancy_stats(center)["regions"]
		quiet = quiet + 1 if n == last else 0
		last = n
		if quiet >= 20 and n > 0:
			return

func test_the_grid_fills_in_around_the_camera(timeout := 60000) -> void:
	var w := make_world()
	assert_int(w.debug_occupancy_stats(CENTER)["regions"]).is_equal(0)
	settle(w, CENTER)
	assert_int(w.debug_occupancy_stats(CENTER)["regions"]).is_greater(2)

func test_every_described_cell_agrees_with_the_field(timeout := 60000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var rng := RandomNumberGenerator.new()
	rng.seed = 20260815
	var compared := 0
	var mismatched := 0
	var saw_air := 0
	var saw_full := 0
	var saw_solid := 0
	for i in range(600):
		var cell := Vector3i(rng.randi_range(10, 40), rng.randi_range(50, 80),
			rng.randi_range(10, 40))
		var gpu: int = w.debug_occupancy_state(cell)
		if gpu == UNKNOWN:
			continue # outside the streamed ball: nobody has looked yet, by design
		compared += 1
		if gpu != w.debug_cell_state(cell):
			mismatched += 1
		if gpu == AIR: saw_air += 1
		elif gpu == FULL: saw_full += 1
		else: saw_solid += 1
	assert_int(compared).is_greater(50)
	assert_int(mismatched).override_failure_message(
		"%d of %d described cells disagree with ve::cell_state_field" % [mismatched, compared]
		).is_equal(0)
	# All three states must actually occur, or the comparison proves nothing.
	assert_int(saw_air).is_greater(0)
	assert_int(saw_full).is_greater(0)
	assert_int(saw_solid).is_greater(0)

func test_an_edit_empties_the_cells_it_carves(timeout := 90000) -> void:
	var w := make_world()
	settle(w, CENTER)
	# A cell that is solidly underground before the edit.
	var cell := Vector3i(25, 62, 25)
	var before: int = w.debug_occupancy_state(cell)
	assert_int(before).is_not_equal(UNKNOWN)
	assert_int(before).is_not_equal(AIR)

	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	# Centre the blast on the cell's own centre, radius 4 m: the cell and its 3^3 probe are
	# entirely inside it, so the only correct answer afterwards is AIR.
	tool.apply_sphere_subtract(Vector3(cell) * 0.8 + Vector3(0.4, 0.4, 0.4), 4.0)
	for i in range(120):
		w.debug_stream_frame(CENTER)
	assert_int(w.debug_occupancy_state(cell)).override_failure_message(
		"the carved cell is still reported as occupied").is_equal(AIR)
	assert_int(w.debug_occupancy_state(cell)).is_equal(w.debug_cell_state(cell))
	# ...and the block carries a sequence number at least as new as the edit.
	assert_int(w.debug_occupancy_stats(CENTER)["seq_at_center"]).is_greater_equal(1)

func test_occupancy_survives_a_region_being_evicted_and_reloaded(timeout := 90000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var cell := Vector3i(25, 62, 25)
	var state: int = w.debug_occupancy_state(cell)
	assert_int(state).is_not_equal(UNKNOWN)
	# Walk far enough that the region is evicted, then come back.
	settle(w, CENTER + Vector3(200.0, 0.0, 0.0))
	# The grid is persistent (spec section 5): an evicted region keeps its block.
	assert_int(w.debug_occupancy_state(cell)).is_equal(state)
	settle(w, CENTER)
	assert_int(w.debug_occupancy_state(cell)).is_equal(state)
```

- [ ] **Step 6: Run the engine test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_occupancy.gd`
Expected: FAIL — `debug_occupancy_state` does not exist.

- [ ] **Step 7: Write the occupancy bits in `brick_mark.comp.glsl`**

Add the binding and the helper:

```glsl
// Spec §5's occupancy grid, two bits per brick, indexed by REGION SLOT exactly as the region
// tables are. The streamer reads a region's 8 KB block back asynchronously and folds it into
// ve::OccupancyGrid at the region's COORDINATE, so eviction can recycle the slot freely.
//
// Byte layout: cell i occupies bits (i & 15) * 2 of word (i >> 4), which on little-endian
// memory is exactly ve::OccupancyGrid's "cell i in byte i >> 2, shift (i & 3) * 2".
layout(set = 0, binding = 9, std430) buffer RegionOccupancy { uint w[]; } occupancy;
const int OCC_WORDS_PER_REGION = REGION_BRICK_COUNT / 16; // 2048

const uint CELL_AIR = 1u;
const uint CELL_SOLID = 2u;
const uint CELL_FULL = 3u;

void write_occupancy(int rslot, int bi, uint state) {
	int word = rslot * OCC_WORDS_PER_REGION + (bi >> 4);
	uint shift = (uint(bi) & 15u) * 2u;
	// Two atomics rather than one CAS loop: nothing on the GPU ever READS this buffer, and
	// no two threads in a dispatch touch the same cell, so the only requirement is that the
	// other fifteen cells sharing the word survive.
	atomicAnd(occupancy.w[word], ~(3u << shift));
	atomicOr(occupancy.w[word], (state & 3u) << shift);
}
```

Split the probe so both answers come from one evaluation, mirroring `ve::brick_probe`:

```glsl
// Mirror of ve::brick_probe (extension/src/world/brick_eval.cpp).
void brick_probe(ivec3 brick, uint op_base, uint op_count, out float mn, out float mx) {
	vec3 bo = vec3(brick) * BRICK_SIZE;
	mn = 1e30;
	mx = -1e30;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				float sdf;
				uint mat;
				eval_field(bo + vec3(sx, sy, sz) * (float(BRICK_VOXELS) * 0.5 * VOXEL_SIZE),
						op_base, op_count, sdf, mat);
				mn = min(mn, sdf);
				mx = max(mx, sdf);
			}
}
```

and in `main()`, replace the `bool has_surface = brick_has_surface(...)` line with:

```glsl
	float probe_mn, probe_mx;
	brick_probe(brick, op_base, op_count, probe_mn, probe_mx);
	// `active` is a GLSL reserved word (M2 errata 5); this local is has_surface.
	bool has_surface = probe_mn < ACTIVATION_PAD && probe_mx > -ACTIVATION_PAD;
	// Occupancy is written in the ALLOCATE phase only: both phases scan the same range, so
	// one write per brick per mark is enough and the release phase returns early for most.
	if (pc.cfg.y == 1)
		write_occupancy(rslot, bi,
				probe_mn > 0.0 ? CELL_AIR : (probe_mx <= 0.0 ? CELL_FULL : CELL_SOLID));
```

(delete the old `brick_has_surface` function; nothing else calls it).

- [ ] **Step 8: Clear the block when a region is released**

`shaders/region_free.comp.glsl` — add the binding and the clear:

```glsl
layout(set = 0, binding = 4, std430) buffer RegionOccupancy { uint w[]; } occupancy;
const int OCC_WORDS_PER_REGION = REGION_BRICK_COUNT / 16; // 2048
```

and at the top of `main()`, alongside the existing `region_counts` reset:

```glsl
	// Back to kCellUnknown. The slot is about to describe a different region, and a leftover
	// bit would answer for a cell in a place it was never probed. The CPU-side grid keeps
	// the OLD region's block -- it is persistent by coordinate, not by slot.
	if (i < OCC_WORDS_PER_REGION) occupancy.w[pc.cfg.x * OCC_WORDS_PER_REGION + i] = 0u;
```

- [ ] **Step 9: Allocate the buffer and bind it**

`extension/src/render/gpu_atlas.h` — add `RID region_occupancy_;` and

```cpp
	RID region_occupancy() const { return region_occupancy_; }
	// Bytes per region slot, and the offset of one slot's block. Mirrors
	// ve::kOccupancyBlockBytes; the static_assert in gpu_atlas.cpp pins them together.
	static uint32_t occupancy_block_bytes() {
		return static_cast<uint32_t>(ve::kOccupancyBlockBytes);
	}
```

`extension/src/render/gpu_atlas.cpp` — create it next to `region_slot_counts_`:

```cpp
static_assert(ve::kOccupancyBlockBytes == ve::kRegionBrickCount / 4,
		"update OCC_WORDS_PER_REGION in shaders/brick_mark.comp.glsl and region_free.comp.glsl");

	// 8 KB per region slot: 4 MB at the shipping 512 slots.
	region_occupancy_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg.max_region_slots) * ve::kOccupancyBlockBytes,
			zeroed(static_cast<int64_t>(cfg.max_region_slots) * ve::kOccupancyBlockBytes));
```

and free it in `teardown()`.

`extension/src/render/region_pass.cpp` — `mark_uset_` gains `storage(9, atlas.region_occupancy())`, `free_uset_` gains `storage(4, atlas.region_occupancy())`.

- [ ] **Step 10: Read the blocks back asynchronously**

`extension/src/voxel_world.h` — next to `PendingEdit`:

```cpp
// One region's occupancy block on its way from the render thread to the main thread's grid.
struct OccupancyBlock {
	ve::IVec3 region{};
	int64_t seq = 0; // the world's edit sequence as of the mark that produced it
	std::vector<uint8_t> bytes; // ve::kOccupancyBlockBytes
};
```

`extension/src/render/world_streamer.h` — forward-declare `struct OccupancyBlock;` beside `PendingEdit`, extend `initialize`, and add:

```cpp
	// Eight reads in flight. buffer_get_data_async costs the frame that asks nothing, but
	// the bytes turn up a few frames later, so the only way to shorten the delay between an
	// edit and its occupancy is to have several outstanding at once. Eight covers the region
	// fan-out of the demo's largest blast with one request each.
	static constexpr int kOccupancyReads = 8;

private:
	struct OccupancyRead {
		Ref<AsyncBufferRead> read;
		ve::IVec3 region{};
		int64_t seq = 0;
		bool active = false;
	};
	// Regions marked LAST frame, waiting for a free ring slot. Requests are issued at the
	// top of run_frame, before any compute list opens: buffer_get_data_async is a
	// device-level command under the same ordering rule as buffer_update (M2 Task 12), so a
	// request issued now returns the state as of the previous frame's mark -- which is
	// exactly what these entries describe.
	std::vector<OccupancyBlock> occ_pending_;  // region + seq only; bytes filled on arrival
	OccupancyRead occ_reads_[kOccupancyReads];
	std::mutex *occ_mutex_ = nullptr;
	std::vector<OccupancyBlock> *occ_inbox_ = nullptr;
	std::atomic<int64_t> *edit_seq_ = nullptr;

	void note_marked(ve::IVec3 region);
	void pump_occupancy(RenderingDevice *rd);
```

`extension/src/render/world_streamer.cpp`:

```cpp
void WorldStreamer::note_marked(ve::IVec3 region) {
	for (const OccupancyBlock &b : occ_pending_)
		if (b.region == region) return; // one request per region per frame is enough
	OccupancyBlock b;
	b.region = region;
	b.seq = edit_seq_ ? edit_seq_->load(std::memory_order_relaxed) : 0;
	occ_pending_.push_back(std::move(b));
}

void WorldStreamer::pump_occupancy(RenderingDevice *rd) {
	if (!atlas_ || !occ_inbox_ || !occ_mutex_) return;
	// 1. Harvest whatever landed. take_fresh() is true exactly once per arrival.
	for (OccupancyRead &r : occ_reads_) {
		if (!r.active || !r.read->take_fresh()) continue;
		r.active = false;
		if (r.read->data().size() < ve::kOccupancyBlockBytes) continue; // short read: drop it
		OccupancyBlock b;
		b.region = r.region;
		b.seq = r.seq;
		b.bytes.assign(r.read->data().ptr(),
				r.read->data().ptr() + ve::kOccupancyBlockBytes);
		std::lock_guard<std::mutex> lock(*occ_mutex_);
		occ_inbox_->push_back(std::move(b));
	}
	// 2. Issue what fits. A region that has since been evicted is skipped: its slot now
	//    describes somewhere else, and region_free has already cleared it.
	for (OccupancyRead &r : occ_reads_) {
		if (r.active || occ_pending_.empty()) continue;
		const OccupancyBlock want = occ_pending_.front();
		occ_pending_.erase(occ_pending_.begin());
		const int slot = residency_->slot_of(want.region);
		if (slot < 0) continue;
		r.region = want.region;
		r.seq = want.seq;
		r.active = r.read->request(rd, atlas_->region_occupancy(),
				static_cast<uint32_t>(slot) * GpuAtlas::occupancy_block_bytes(),
				GpuAtlas::occupancy_block_bytes());
	}
}
```

Call `pump_occupancy(rd)` as the **first** statement of `run_frame` after the timing setup, instantiate the eight `Ref<AsyncBufferRead>`s in `initialize`, and call `note_marked(region)` immediately after every `region_pass_->mark(...)` call in `run_frame` (there are three arms: fresh loads, edit re-marks and the repair sweep).

- [ ] **Step 11: Own the grid in `VoxelWorld`**

`extension/src/voxel_world.h` — `#include "connectivity/occupancy.h"`, `#include <atomic>`, and in the private section:

```cpp
	ve::OccupancyGrid occupancy_;              // main thread only
	std::mutex occupancy_mutex_;               // guards occupancy_inbox_
	std::vector<OccupancyBlock> occupancy_inbox_;
	// Monotonic; bumped by every accepted edit. The streamer stamps each occupancy readback
	// with it so IslandManager (Task 13) can tell whether a window's cells are new enough to
	// act on, rather than running connectivity against a picture of the world from before
	// the blast.
	std::atomic<int64_t> edit_seq_{0};
	void drain_occupancy();                    // inbox -> grid
```

public:

```cpp
	ve::OccupancyGrid &occupancy() { return occupancy_; }
	int64_t edit_seq() const { return edit_seq_.load(std::memory_order_relaxed); }
	int debug_occupancy_state(Vector3i cell);
	int debug_cell_state(Vector3i cell);
	Dictionary debug_occupancy_stats(Vector3 center);
```

`extension/src/voxel_world.cpp`:

```cpp
void VoxelWorld::drain_occupancy() {
	std::vector<OccupancyBlock> blocks;
	{
		std::lock_guard<std::mutex> lock(occupancy_mutex_);
		blocks.swap(occupancy_inbox_);
	}
	for (const OccupancyBlock &b : blocks)
		occupancy_.set_block(b.region, b.bytes.data(), b.seq);
}

void VoxelWorld::_process(double) {
	// Unconditional: the grid must keep filling even with physics disabled, because the
	// island manager (Task 13) and the debug hooks both read it.
	drain_occupancy();
	if (!physics_enabled_ || physics_center_path_.is_empty()) return;
	Node3D *anchor = Object::cast_to<Node3D>(get_node_or_null(physics_center_path_));
	if (!anchor) return;
	ensure_physics_initialized();
	physics_tick(anchor->get_global_position());
}

int VoxelWorld::debug_occupancy_state(Vector3i cell) {
	drain_occupancy(); // tests step the streamer by hand and never run _process
	return static_cast<int>(occupancy_.state({cell.x, cell.y, cell.z}));
}

int VoxelWorld::debug_cell_state(Vector3i cell) {
	if (!edit_log_) return static_cast<int>(ve::kCellUnknown);
	const ve::IVec3 c{cell.x, cell.y, cell.z};
	ve::AnalyticGenerator gen;
	std::lock_guard<std::mutex> lock(edit_mutex_);
	const std::vector<ve::EditOp> &ops = edit_log_->ops(ve::WorldBounds::region_of_brick(c));
	return static_cast<int>(ve::cell_state_field(gen, ops.data(),
			static_cast<int>(ops.size()), c, &volumes_));
}

Dictionary VoxelWorld::debug_occupancy_stats(Vector3 center) {
	drain_occupancy();
	Dictionary d;
	d["regions"] = occupancy_.region_count();
	d["edit_seq"] = static_cast<int64_t>(edit_seq());
	// The block covering the streaming centre, so a test can tell "the grid has been told
	// about this edit" from "some other region's block arrived".
	const ve::IVec3 r = ve::WorldBounds::region_of_point(center.x, center.y, center.z);
	d["seq_at_center"] = static_cast<int64_t>(occupancy_.block_seq(r));
	return d;
}
```

`append_edit` bumps the counter as its first act:

```cpp
	edit_seq_.fetch_add(1, std::memory_order_relaxed);
```

and `ensure_initialized` passes the three new arguments to the streamer:

```cpp
	streamer_->initialize(residency_, edit_log_, &edit_mutex_, &pending_edits_, atlas_,
			region_pass_, gen_pass_, &occupancy_mutex_, &occupancy_inbox_, &edit_seq_);
```

Finally, `debug_stream_frame` drains after syncing, so a test that never runs `_process`
still sees the grid fill:

```cpp
	overflow_seen_ |= static_cast<int>(atlas_->read_overflow(device));
	drain_occupancy();
	return actions;
```

- [ ] **Step 12: Run both suites**

Run: `cd extension && scons test`
Expected: PASS.

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: green, including the four new `test_occupancy.gd` cases. `test_streaming.gd` and `test_edit_pipeline.gd` exercise the mark and free passes whose bindings just moved.

- [ ] **Step 13: Commit**

```bash
git add shaders/brick_mark.comp.glsl shaders/region_free.comp.glsl \
        extension/src/world/brick_eval.h extension/src/world/brick_eval.cpp \
        extension/src/render/gpu_atlas.h extension/src/render/gpu_atlas.cpp \
        extension/src/render/region_pass.cpp \
        extension/src/render/world_streamer.h extension/src/render/world_streamer.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp \
        extension/tests/test_occupancy.cpp tests/test_occupancy.gd
git commit -m "feat: occupancy grid filled from the mark pass"
```

---
### Task 9: `render/island_extract_pass` — a component becomes a dense volume

Spec §5 step 2, "Extract dense island SDF texture", and spec §3's "Island SDF storage: dense per-island texture (AABB at 5 cm, uint8 + palette + own min–max mip), extracted at carve time."

The extraction runs on the collision mesher's worker device: it already compiles `field.glslh`, already uploads op lists, already reads results back, and already lives on a thread the frame never waits for.

**Files:**
- Create: `shaders/island_extract.comp.glsl`, `extension/src/render/island_extract_pass.h`, `extension/src/render/island_extract_pass.cpp`
- Modify: `shaders/common.glslh`
- Modify: `extension/src/generator/volume_set.h`, `extension/src/generator/volume_set.cpp`
- Modify: `extension/src/render/mesh_service.h`, `extension/src/render/mesh_service.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Test: `extension/tests/test_volume_ops.cpp` (added cases), `tests/test_island_extract.gd`

**Interfaces:**
- Consumes: `ve::CellBox` (Task 5), `ve::VolumeData`, `ve::plan_island_lattice` (Task 6), `field.glslh`'s volume bindings (Task 7).
- Produces: `ve::extract_island_volume`, `ve::build_volume_mip`; `godot::IslandExtractJob`, `godot::IslandExtractResult`, `godot::IslandExtractPass`; `MeshService::submit_extracts`, `collect_extracts`, `extracts_busy`, `submit_volume`; `VoxelWorld::debug_island_extract_diff`. Task 10 uploads what this produces; Tasks 12 and 13 consume its mass and its bytes.

- [ ] **Step 1: Write the failing native test cases**

Add `#include "connectivity/components.h"` (for `kMaxIslandExtentCells`) and `#include <algorithm>`
to the include block at the top of `extension/tests/test_volume_ops.cpp`, then append:

```cpp
namespace {

// extract_island_volume takes 6 world-space floats per box; ve::CellBox knows how to produce
// them, so this is the one place the two representations meet in a test.
std::vector<float> flat_aabbs(const std::vector<CellBox> &boxes) {
	std::vector<float> v(boxes.size() * 6);
	for (size_t i = 0; i < boxes.size(); i++) boxes[i].world_aabb(&v[i * 6], &v[i * 6 + 3]);
	return v;
}

} // namespace

TEST_CASE("plan_island_lattice picks the finest pitch that fits with its margin") {
	float voxel = 0.0f;
	float origin[3] = {0, 0, 0};

	// A 1 m box: comfortably inside the fine pitch's (64 - 1 - 4) * 0.05 = 2.95 m reach.
	const float small_lo[3] = {10.0f, 20.0f, 30.0f};
	const float small_hi[3] = {11.0f, 21.0f, 31.0f};
	CHECK(plan_island_lattice(small_lo, small_hi, kIslandDim, &voxel, origin));
	CHECK(voxel == doctest::Approx(kIslandVoxelFine));
	// The box is centred in the lattice, so both margins are equal and at least two voxels.
	const float span = static_cast<float>(kIslandDim - 1) * voxel;
	CHECK(origin[0] == doctest::Approx(10.0f - 0.5f * (span - 1.0f)));
	CHECK(small_lo[0] - origin[0] >= kIslandMarginVoxels * voxel);
	CHECK((origin[0] + span) - small_hi[0] >= kIslandMarginVoxels * voxel);

	// A 4 m box needs the coarse pitch.
	const float mid_hi[3] = {14.0f, 24.0f, 34.0f};
	CHECK(plan_island_lattice(small_lo, mid_hi, kIslandDim, &voxel, origin));
	CHECK(voxel == doctest::Approx(kIslandVoxelCoarse));

	// The largest component the labeller can emit still fits at the coarse pitch. This is
	// the invariant that lets Task 3 split on extent alone.
	const float big_hi[3] = {
		10.0f + kMaxIslandExtentCells * kOccupancyCellSize,
		20.0f + kMaxIslandExtentCells * kOccupancyCellSize,
		30.0f + kMaxIslandExtentCells * kOccupancyCellSize};
	CHECK(plan_island_lattice(small_lo, big_hi, kIslandDim, &voxel, origin));

	// ...and anything bigger is refused rather than silently clipped.
	const float huge_hi[3] = {30.0f, 40.0f, 50.0f};
	CHECK(plan_island_lattice(small_lo, huge_hi, kIslandDim, &voxel, origin) == false);
}

TEST_CASE("extract_island_volume is the field intersected with the component's boxes") {
	AnalyticGenerator gen;
	// Cells 10..11 on x, 20 on y and z: solid rock, deep underground.
	const std::vector<CellBox> boxes{CellBox{{10, 20, 20}, {11, 20, 20}}};
	const std::vector<float> aabbs = flat_aabbs(boxes);
	float lo[3], hi[3];
	boxes[0].world_aabb(lo, hi);
	float voxel = 0.0f;
	float origin[3] = {0, 0, 0};
	REQUIRE(plan_island_lattice(lo, hi, kIslandDim, &voxel, origin));

	VolumeData v;
	extract_island_volume(gen, nullptr, 0, nullptr, origin, voxel, kIslandDim, aabbs.data(),
			1, &v);
	CHECK(v.dim == kIslandDim);
	CHECK(v.solid_voxels > 0);

	VolumeSample s{};
	// The middle of the box: solid rock, so the island holds it.
	REQUIRE(sample_volume_lattice(v.sdf.data(), v.mat.data(), v.dim, origin, voxel,
			lo[0] + 0.4f, lo[1] + 0.4f, lo[2] + 0.4f, &s));
	CHECK(s.sdf < 0.0f);
	CHECK(s.material != 0);
	// Outside the box but inside the lattice: the mask cut it away even though the terrain
	// there is solid.
	REQUIRE(sample_volume_lattice(v.sdf.data(), v.mat.data(), v.dim, origin, voxel,
			hi[0] + 0.3f, lo[1] + 0.4f, lo[2] + 0.4f, &s));
	CHECK(s.sdf > 0.0f);
	// The outermost lattice shell is positive on every face, which is what makes
	// sample_volume_lattice's outside-the-box branch agree with its inside branch.
	const int d = v.dim;
	for (int z = 0; z < d; z += d - 1)
		for (int y = 0; y < d; y++)
			for (int x = 0; x < d; x++)
				CHECK(decode_sdf(v.sdf[VolumeSet::voxel_index(d, x, y, z)]) > 0.0f);
}

TEST_CASE("an extraction whose boxes hold no solid comes back empty") {
	AnalyticGenerator gen;
	// High above the terrain: the field is air, so the intersection is nothing.
	const std::vector<CellBox> boxes{CellBox{{10, 100, 20}, {10, 100, 20}}};
	const std::vector<float> aabbs = flat_aabbs(boxes);
	float lo[3], hi[3];
	boxes[0].world_aabb(lo, hi);
	float voxel = 0.0f;
	float origin[3] = {0, 0, 0};
	REQUIRE(plan_island_lattice(lo, hi, kIslandDim, &voxel, origin));
	VolumeData v;
	extract_island_volume(gen, nullptr, 0, nullptr, origin, voxel, kIslandDim, aabbs.data(),
			1, &v);
	CHECK(v.solid_voxels == 0);
}

TEST_CASE("the volume min-max mip bounds every sample in its cell") {
	AnalyticGenerator gen;
	const std::vector<CellBox> boxes{CellBox{{10, 20, 20}, {12, 21, 21}}};
	const std::vector<float> aabbs = flat_aabbs(boxes);
	float lo[3], hi[3];
	boxes[0].world_aabb(lo, hi);
	float voxel = 0.0f;
	float origin[3] = {0, 0, 0};
	REQUIRE(plan_island_lattice(lo, hi, kIslandDim, &voxel, origin));
	VolumeData v;
	extract_island_volume(gen, nullptr, 0, nullptr, origin, voxel, kIslandDim, aabbs.data(),
			1, &v);

	std::vector<uint8_t> mip;
	build_volume_mip(v, &mip);
	const int cells = kIslandDim / kVolumeMipStride;
	CHECK(static_cast<int>(mip.size()) == cells * cells * cells * 2);
	// Inclusive over the cell's CORNER range, exactly as ve::build_brick_mips is, so a
	// "no surface" verdict is a sound skip for the trilinear reconstruction inside it.
	for (int cz = 0; cz < cells; cz++)
		for (int cy = 0; cy < cells; cy++)
			for (int cx = 0; cx < cells; cx++) {
				const int ci = (cx + cy * cells + cz * cells * cells) * 2;
				uint8_t mn = 255, mx = 0;
				for (int z = 0; z <= kVolumeMipStride; z++)
					for (int y = 0; y <= kVolumeMipStride; y++)
						for (int x = 0; x <= kVolumeMipStride; x++) {
							const int sx = std::min(cx * kVolumeMipStride + x, kIslandDim - 1);
							const int sy = std::min(cy * kVolumeMipStride + y, kIslandDim - 1);
							const int sz = std::min(cz * kVolumeMipStride + z, kIslandDim - 1);
							const uint8_t s =
									v.sdf[VolumeSet::voxel_index(kIslandDim, sx, sy, sz)];
							mn = std::min(mn, s);
							mx = std::max(mx, s);
						}
				CHECK(mip[ci + 0] == mn);
				CHECK(mip[ci + 1] == mx);
			}
}
```

- [ ] **Step 2: Run the native tests to verify they fail**

Run: `cd extension && scons test`
Expected: FAIL — `'plan_island_lattice' was not declared in this scope`

- [ ] **Step 3: Add the CPU reference and the mip builder**

`extension/src/generator/volume_set.h` — add near the other constants:

```cpp
// Margin between the component's AABB and the lattice's outer shell, in voxels. It exists so
// the outermost shell is strictly outside every mask box and therefore reads POSITIVE, which
// is what makes sample_volume_lattice's inside and outside branches agree at the seam.
inline constexpr int kIslandMarginVoxels = 2;
// Voxels per min-max cell along each axis. 8 gives an 8^3 chain per 64^3 volume: 1 KB per
// island, the same shape (and the same inclusive-corner rule) as ve::build_brick_mips.
inline constexpr int kVolumeMipStride = 8;
```

and the three functions:

```cpp
// Where to put a component's lattice: the finest of kIslandVoxelFine / kIslandVoxelCoarse
// whose usable reach ((dim - 1 - 2 * kIslandMarginVoxels) * pitch) covers the AABB, and an
// origin that centres the AABB inside it. False when even the coarse pitch cannot -- the
// caller splits the component (ve::kMaxIslandExtentCells is chosen so that never happens for
// a labelled component, and volume_set.cpp static_asserts the relationship).
bool plan_island_lattice(const float lo[3], const float hi[3], int dim, float *voxel,
		float origin[3]);

// The CPU reference for shaders/island_extract.comp.glsl (spec §8's differential testing):
// the world field intersected with the union of the component's boxes. The intersection is
// what makes the island exactly the material that left, and the carve exactly those boxes.
//
// `box_aabbs` holds 6 floats per box -- min xyz then max xyz, in world space. Flat floats
// rather than ve::CellBox so generator/ need not depend on mesh/: the extractor does not
// care that the boxes came from occupancy cells, only where they are.
void extract_island_volume(const Generator &gen, const EditOp *ops, int op_count,
		const VolumeStore *volumes, const float origin[3], float voxel, int dim,
		const float *box_aabbs, int box_count, VolumeData *out);

// Spec §3's "own min-max mip". Two bytes (min, max) per kVolumeMipStride^3 cell, INCLUSIVE
// over the cell's corner range so a "no surface" verdict is a sound skip for the trilinear
// reconstruction inside it -- the same soundness argument ve::build_brick_mips rests on.
void build_volume_mip(const VolumeData &v, std::vector<uint8_t> *out);
```

`extension/src/generator/volume_set.cpp` — add `#include "connectivity/components.h"`, `#include "world/brick_eval.h"`, and:

```cpp
// The labeller splits on extent alone, so its bound must be one the lattice can always hold.
static_assert(static_cast<double>(kMaxIslandExtentCells) * kOccupancyCellSize <=
				static_cast<double>(kIslandDim - 1 - 2 * kIslandMarginVoxels) *
						kIslandVoxelCoarse,
		"kMaxIslandExtentCells is larger than an island volume can cover");

bool plan_island_lattice(const float lo[3], const float hi[3], int dim, float *voxel,
		float origin[3]) {
	if (dim < 2 + 2 * kIslandMarginVoxels) return false;
	float extent = 0.0f;
	for (int a = 0; a < 3; a++) extent = std::max(extent, hi[a] - lo[a]);
	const float usable = static_cast<float>(dim - 1 - 2 * kIslandMarginVoxels);
	float pitch = kIslandVoxelFine;
	if (extent > usable * pitch) pitch = kIslandVoxelCoarse;
	if (extent > usable * pitch) return false;
	const float span = static_cast<float>(dim - 1) * pitch;
	for (int a = 0; a < 3; a++) origin[a] = lo[a] - 0.5f * (span - (hi[a] - lo[a]));
	*voxel = pitch;
	return true;
}

void extract_island_volume(const Generator &gen, const EditOp *ops, int op_count,
		const VolumeStore *volumes, const float origin[3], float voxel, int dim,
		const float *box_aabbs, int box_count, VolumeData *out) {
	out->dim = dim;
	out->sdf.assign(static_cast<size_t>(dim) * dim * dim, 0);
	out->mat.assign(static_cast<size_t>(dim) * dim * dim, 0);
	out->solid_voxels = 0;

	for (int z = 0; z < dim; z++)
		for (int y = 0; y < dim; y++)
			for (int x = 0; x < dim; x++) {
				const float p[3] = {origin[0] + x * voxel, origin[1] + y * voxel,
						origin[2] + z * voxel};
				Sample s = eval_field(gen, ops, op_count, p[0], p[1], p[2], volumes);
				// The island IS the solid field intersected with the union of its cells, so
				// the mask is a CSG intersection: max(field, min over boxes).
				float bu = 1e30f;
				for (int b = 0; b < box_count; b++)
					bu = std::min(bu, box_sdf(&box_aabbs[static_cast<size_t>(b) * 6 + 0],
										 &box_aabbs[static_cast<size_t>(b) * 6 + 3],
										 p[0], p[1], p[2]));
				s.sdf = std::max(s.sdf, bu);
				if (s.sdf > 0.0f) s.material = 0;
				const int i = VolumeSet::voxel_index(dim, x, y, z);
				out->sdf[i] = encode_sdf(s.sdf);
				out->mat[i] = static_cast<uint8_t>(s.material > 255 ? 255 : s.material);
				if (s.sdf <= 0.0f) out->solid_voxels++;
			}
}

void build_volume_mip(const VolumeData &v, std::vector<uint8_t> *out) {
	const int dim = v.dim;
	const int cells = dim / kVolumeMipStride;
	out->assign(static_cast<size_t>(cells) * cells * cells * 2, 0);
	for (int cz = 0; cz < cells; cz++)
		for (int cy = 0; cy < cells; cy++)
			for (int cx = 0; cx < cells; cx++) {
				uint8_t mn = 255, mx = 0;
				// INCLUSIVE corner range: the trilinear interpolant inside a cell is a
				// multilinear combination of its corner samples and therefore never leaves
				// their convex hull, so an inclusive bound is sound and an exclusive one is
				// not (the argument in world/brick_mip.h, applied to a 64^3 lattice).
				for (int z = 0; z <= kVolumeMipStride; z++)
					for (int y = 0; y <= kVolumeMipStride; y++)
						for (int x = 0; x <= kVolumeMipStride; x++) {
							const int sx = std::min(cx * kVolumeMipStride + x, dim - 1);
							const int sy = std::min(cy * kVolumeMipStride + y, dim - 1);
							const int sz = std::min(cz * kVolumeMipStride + z, dim - 1);
							const uint8_t s = v.sdf[VolumeSet::voxel_index(dim, sx, sy, sz)];
							mn = std::min(mn, s);
							mx = std::max(mx, s);
						}
				const int ci = (cx + cy * cells + cz * cells * cells) * 2;
				(*out)[static_cast<size_t>(ci) + 0] = mn;
				(*out)[static_cast<size_t>(ci) + 1] = mx;
			}
}
```

Replace the pitch-and-origin block inside `resample_volume` with a call to the shared planner, so the two placements cannot drift:

```cpp
	float pitch = 0.0f;
	float o[3] = {0, 0, 0};
	if (!plan_island_lattice(wlo, whi, dim, &pitch, o)) return false;
```

- [ ] **Step 4: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS — four new cases.

- [ ] **Step 5: Write the failing engine test**

Create `tests/test_island_extract.gd`:

```gdscript
extends GdUnitTestSuite

# GPU/CPU differential test for island extraction (spec section 8): the world field
# intersected with a component's 0.8 m cell boxes, at 5 or 10 cm, on the mesher's worker
# device against ve::extract_island_volume.
#
# Tolerances follow tests/test_brick_diff.gd, and for the same reason: sin() is not
# bit-identical between glibc and a Vulkan driver, and a uint8 with ~5 mm steps cannot show a
# disagreement smaller than half a step. The MASK contributes no transcendentals at all, so
# a disagreement bigger than that is a real bug in the box arithmetic, not in libm.
const MAX_STEPS := 2

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true() # starts the mesher's worker + its device
	return w

func check_extract(w: VoxelWorld, lo: Vector3i, hi: Vector3i, label: String) -> Dictionary:
	var d: Dictionary = w.debug_island_extract_diff(lo, hi)
	assert_bool(d.get("ok", false)).override_failure_message(
		"%s: extraction failed: %s" % [label, d]).is_true()
	assert_int(d["worst_steps"]).override_failure_message(
		"%s: worst sdf disagreement %d encoded steps" % [label, d["worst_steps"]]
		).is_less(MAX_STEPS)
	assert_int(d["mat_mismatch"]).override_failure_message(
		"%s: %d material mismatches" % [label, d["mat_mismatch"]]).is_equal(0)
	return d

func test_a_single_cell_extracts_to_the_same_volume_on_both_sides(timeout := 60000) -> void:
	# Deep underground: the box is entirely full, so the island is the box.
	var d := check_extract(make_world(), Vector3i(10, 20, 20), Vector3i(10, 20, 20), "cell")
	assert_int(d["gpu_solid"]).is_greater(0)
	assert_float(float(d["gpu_solid"]) / float(d["cpu_solid"])).is_between(0.99, 1.01)
	assert_float(d["voxel"]).is_equal_approx(0.05, 0.001)

func test_a_multi_cell_component_across_the_surface_matches(timeout := 60000) -> void:
	# Cells straddling the terrain surface near (20, 20): the interesting case, because the
	# mask and the field both have something to say about the same voxels.
	var d := check_extract(make_world(), Vector3i(24, 64, 24), Vector3i(26, 67, 26), "slab")
	assert_int(d["gpu_solid"]).is_greater(0)
	assert_float(float(d["gpu_solid"]) / float(d["cpu_solid"])).is_between(0.99, 1.01)
	# Four cells across is 3.2 m, past the fine pitch's reach: the planner drops to 10 cm.
	assert_float(d["voxel"]).is_equal_approx(0.10, 0.001)

func test_the_mask_cuts_the_terrain_at_the_box_faces(timeout := 60000) -> void:
	var w := make_world()
	var d := check_extract(w, Vector3i(10, 20, 20), Vector3i(11, 20, 20), "mask")
	# The extraction is an intersection, so the solid count can never exceed the boxes'
	# volume: two 0.8 m cells at 5 cm is 2 * 16^3 = 8192 voxels.
	assert_int(d["gpu_solid"]).is_less_equal(8192)
	# ...and here, where the terrain is solid throughout, it should very nearly reach it.
	assert_int(d["gpu_solid"]).is_greater(7000)

func test_an_edit_inside_the_component_reaches_the_extraction(timeout := 60000) -> void:
	var w := make_world()
	var before := check_extract(w, Vector3i(10, 20, 20), Vector3i(11, 20, 20), "before")
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	# Carve half the box away. The extraction reads the region's op list, so both sides must
	# see it -- this is what catches an op pool that was uploaded to only one of them.
	tool.apply_sphere_subtract(Vector3(8.4, 16.4, 16.4), 0.6)
	var after := check_extract(w, Vector3i(10, 20, 20), Vector3i(11, 20, 20), "after")
	assert_int(after["gpu_solid"]).is_less(before["gpu_solid"])
	assert_int(after["gpu_solid"]).is_greater(0)
```

- [ ] **Step 6: Run the engine test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_island_extract.gd`
Expected: FAIL — `debug_island_extract_diff` does not exist.

- [ ] **Step 7: Add `encode_sdf_byte` to `shaders/common.glslh`**

Replace `quantise_sdf` with the pair, so the byte form and the unorm form can never disagree:

```glsl
// The BYTE ve::encode_sdf produces. Quantising here rather than leaning on the hardware's
// float->unorm conversion removes the only rounding-mode difference between CPU and GPU.
uint encode_sdf_byte(float d) {
	float t = clamp((d + SDF_RANGE) / (2.0 * SDF_RANGE), 0.0, 1.0);
	return uint(floor(t * 255.0 + 0.5));
}

// The float an R8_UNORM imageStore must receive for the written byte to equal
// ve::encode_sdf(d).
float quantise_sdf(float d) { return float(encode_sdf_byte(d)) / 255.0; }
```

- [ ] **Step 8: Write `shaders/island_extract.comp.glsl`**

```glsl
#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 1
#define FIELD_VOLUME_SDF_BINDING 2
#define FIELD_VOLUME_MAT_BINDING 3
#include "common.glslh"
#include "field.glslh"

// One thread per lattice sample. 64 is a multiple of 4, so no group runs out of bounds --
// the guard is kept anyway because the dim comes from a push constant.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

// One uint per voxel: (material << 8) | encoded sdf. Packing here rather than writing two
// byte arrays means one buffer, one readback, and no sub-word atomics; the CPU splits the
// two halves apart as it copies them into ve::VolumeData.
layout(set = 0, binding = 0, std430) writeonly buffer Out { uint v[]; } out_vol;
// binding 1 is the field op pool and 2/3 the volume pool, declared by field.glslh
// Two vec4 per box: the world AABB's min and max corners.
layout(set = 0, binding = 4, std430) readonly buffer Boxes { vec4 v[]; } boxes;
layout(set = 0, binding = 5, std430) buffer Counts { uint solid; uint pad0, pad1, pad2; } counts;

layout(push_constant, std430) uniform Push {
	vec4 origin_voxel; // xyz = lattice world origin, w = voxel pitch
	ivec4 params;      // x = dim, y = op count, z = box count, w = unused
} pc;

void main() {
	ivec3 l = ivec3(gl_GlobalInvocationID);
	int dim = pc.params.x;
	if (any(greaterThanEqual(l, ivec3(dim)))) return;
	vec3 p = pc.origin_voxel.xyz + vec3(l) * pc.origin_voxel.w;

	float sdf;
	uint mat;
	eval_field(p, 0u, uint(pc.params.y), sdf, mat);

	// Mirror of ve::extract_island_volume: the island IS the solid field intersected with
	// the union of its 0.8 m cells, which is max(field, min over boxes). A component with no
	// boxes extracts to nothing, which is the correct answer and not a special case.
	float bu = 1e30;
	for (int i = 0; i < pc.params.z; i++)
		bu = min(bu, op_box_sdf(boxes.v[i * 2 + 0].xyz, boxes.v[i * 2 + 1].xyz, p));
	sdf = max(sdf, bu);
	if (sdf > 0.0) mat = 0u;
	if (sdf <= 0.0) atomicAdd(counts.solid, 1u);

	out_vol.v[l.x + l.y * dim + l.z * dim * dim] =
			(min(mat, 255u) << 8) | encode_sdf_byte(sdf);
}
```

- [ ] **Step 9: Write `render/island_extract_pass`**

Create `extension/src/render/island_extract_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "connectivity/occupancy.h" // ve::CellBox
#include "generator/edit_ops.h"
#include "generator/volume_set.h"
#include "mesh/box_merge.h"         // ve::kMaxIslandBoxes

namespace godot {

class VolumePool;

// One component's worth of work. `ops` and `boxes` are owned, because the job crosses onto
// the mesher's worker thread.
struct IslandExtractJob {
	int id = -1; // the caller's handle, echoed back untouched
	float origin[3] = {0.0f, 0.0f, 0.0f};
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	std::vector<ve::EditOp> ops;   // the component's region's op list at extraction time
	std::vector<ve::CellBox> boxes;
};

struct IslandExtractResult {
	int id = -1;
	ve::VolumeData data;
	bool failed = false;
};

// Spec §5 step 2 and §3's dense per-island volume. Synchronous by design: it runs on the
// mesher's worker thread, where a submit/sync costs nothing the frame can see, and one
// extraction (262 144 field evaluations plus a 1 MB readback) is ~1-2 ms.
class IslandExtractPass {
public:
	~IslandExtractPass();

	// `volumes` is the same pool the mesher's field pass binds, so an extraction sees the
	// rubble already pasted into the world.
	bool initialize(RenderingDevice *rd, const VolumePool *volumes);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }

	bool extract(const IslandExtractJob &job, IslandExtractResult *out);

private:
	RenderingDevice *rd_ = nullptr;
	RID out_, boxes_, counts_, ops_;
	RID shader_, pipeline_, uset_;
};

} // namespace godot
```

Create `extension/src/render/island_extract_pass.cpp` — the shape follows `MeshPass` exactly (same `build()` helper, same `storage()`/`zeroed()` locals, same teardown order):

```cpp
#include "render/island_extract_pass.h"
#include "render/shader_loader.h"
#include "render/volume_pool.h"
#include "world/edit_log.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
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

PackedByteArray zeroed(int64_t bytes) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(0);
	return b;
}

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

} // namespace

IslandExtractPass::~IslandExtractPass() {
	teardown();
}

bool IslandExtractPass::initialize(RenderingDevice *rd, const VolumePool *volumes) {
	teardown();
	if (!rd || !volumes || !volumes->is_valid()) return false;
	rd_ = rd;
	const int64_t voxels = static_cast<int64_t>(ve::kIslandVoxelCount);
	out_ = rd->storage_buffer_create(static_cast<uint32_t>(voxels * 4), zeroed(voxels * 4));
	boxes_ = rd->storage_buffer_create(ve::kMaxIslandBoxes * 32,
			zeroed(ve::kMaxIslandBoxes * 32));
	counts_ = rd->storage_buffer_create(16, zeroed(16));
	ops_ = rd->storage_buffer_create(ve::kMaxRegionOps * 32, zeroed(ve::kMaxRegionOps * 32));
	if (!out_.is_valid() || !boxes_.is_valid() || !counts_.is_valid() || !ops_.is_valid()) {
		UtilityFunctions::printerr("IslandExtractPass: buffer creation failed");
		teardown();
		return false;
	}

	// Same loader path as MeshPass::build: read the file, strip #[compute], compile, report.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/island_extract.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("IslandExtractPass: load failed: ", err.c_str());
		teardown();
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String cerr = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!cerr.is_empty()) {
		UtilityFunctions::printerr("IslandExtractPass: ", cerr);
		teardown();
		return false;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	pipeline_ = shader_.is_valid() ? rd->compute_pipeline_create(shader_) : RID();
	if (!pipeline_.is_valid()) {
		teardown();
		return false;
	}
	uset_ = rd->uniform_set_create(Array::make(storage(0, out_), storage(1, ops_),
			storage(2, volumes->sdf_buffer()), storage(3, volumes->mat_buffer()),
			storage(4, boxes_), storage(5, counts_)), shader_, 0);
	if (!uset_.is_valid()) {
		UtilityFunctions::printerr("IslandExtractPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void IslandExtractPass::teardown() {
	// Uniform sets first: freeing a shader cascades to its pipelines and referencing sets.
	free_if_valid(rd_, uset_);
	free_if_valid(rd_, pipeline_);
	free_if_valid(rd_, shader_);
	free_if_valid(rd_, ops_);
	free_if_valid(rd_, counts_);
	free_if_valid(rd_, boxes_);
	free_if_valid(rd_, out_);
	rd_ = nullptr;
}

bool IslandExtractPass::extract(const IslandExtractJob &job, IslandExtractResult *out) {
	out->id = job.id;
	out->failed = true;
	out->data = ve::VolumeData{};
	if (!is_valid() || job.dim < 2 || job.dim > ve::kIslandDim || job.voxel <= 0.0f)
		return false;
	const int box_count = std::min(static_cast<int>(job.boxes.size()), ve::kMaxIslandBoxes);
	const int op_count = std::min(static_cast<int>(job.ops.size()), ve::kMaxRegionOps);

	// Device-level commands, all before compute_list_begin (M2 Task 12's ordering rule).
	rd_->buffer_update(counts_, 0, 16, zeroed(16));
	if (op_count > 0) {
		PackedByteArray b;
		b.resize(static_cast<int64_t>(op_count) * 32);
		std::memcpy(b.ptrw(), job.ops.data(), static_cast<size_t>(op_count) * 32);
		rd_->buffer_update(ops_, 0, static_cast<uint32_t>(b.size()), b);
	}
	if (box_count > 0) {
		PackedByteArray b;
		b.resize(static_cast<int64_t>(box_count) * 32);
		float *f = reinterpret_cast<float *>(b.ptrw());
		for (int i = 0; i < box_count; i++) {
			float lo[3], hi[3];
			job.boxes[static_cast<size_t>(i)].world_aabb(lo, hi);
			f[i * 8 + 0] = lo[0]; f[i * 8 + 1] = lo[1]; f[i * 8 + 2] = lo[2]; f[i * 8 + 3] = 0.0f;
			f[i * 8 + 4] = hi[0]; f[i * 8 + 5] = hi[1]; f[i * 8 + 6] = hi[2]; f[i * 8 + 7] = 0.0f;
		}
		rd_->buffer_update(boxes_, 0, static_cast<uint32_t>(b.size()), b);
	}

	PackedByteArray pc;
	pc.resize(32);
	float *pf = reinterpret_cast<float *>(pc.ptrw());
	int32_t *pi = reinterpret_cast<int32_t *>(pc.ptrw());
	pf[0] = job.origin[0];
	pf[1] = job.origin[1];
	pf[2] = job.origin[2];
	pf[3] = job.voxel;
	pi[4] = job.dim;
	pi[5] = op_count;
	pi[6] = box_count;
	pi[7] = 0;

	const int64_t list = rd_->compute_list_begin();
	rd_->compute_list_bind_compute_pipeline(list, pipeline_);
	rd_->compute_list_bind_uniform_set(list, uset_, 0);
	rd_->compute_list_set_push_constant(list, pc, pc.size());
	const int g = (job.dim + 3) / 4;
	rd_->compute_list_dispatch(list, g, g, g);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();

	const int64_t voxels = static_cast<int64_t>(job.dim) * job.dim * job.dim;
	const PackedByteArray data =
			rd_->buffer_get_data(out_, 0, static_cast<uint32_t>(voxels * 4));
	if (data.size() < voxels * 4) {
		UtilityFunctions::printerr("IslandExtractPass: short readback");
		return false;
	}
	const uint32_t *w = reinterpret_cast<const uint32_t *>(data.ptr());
	out->data.dim = job.dim;
	out->data.sdf.resize(static_cast<size_t>(voxels));
	out->data.mat.resize(static_cast<size_t>(voxels));
	out->data.solid_voxels = 0;
	for (int64_t i = 0; i < voxels; i++) {
		out->data.sdf[static_cast<size_t>(i)] = static_cast<uint8_t>(w[i] & 0xFFu);
		out->data.mat[static_cast<size_t>(i)] = static_cast<uint8_t>((w[i] >> 8) & 0xFFu);
	}
	const PackedByteArray cb = rd_->buffer_get_data(counts_, 0, 16);
	if (cb.size() >= 16)
		out->data.solid_voxels =
				static_cast<int>(reinterpret_cast<const uint32_t *>(cb.ptr())[0]);
	out->failed = false;
	return true;
}
```

- [ ] **Step 10: Give `MeshService` the extract queue and the volume uploads**

`extension/src/render/mesh_service.h` — add `#include "render/island_extract_pass.h"` and:

```cpp
	// Extraction shares the worker thread with meshing, in its own queue: an island is a
	// player-visible event and must not wait behind a collider batch, but it is also rare
	// enough that a dedicated thread would idle.
	bool submit_extracts(std::vector<IslandExtractJob> jobs);
	bool extracts_busy() const { return extract_busy_.load(std::memory_order_acquire); }
	int collect_extracts(std::vector<IslandExtractResult> *out);

	// Copies one stored volume into THIS device's pool, on the worker thread. The main
	// thread's ve::VolumeSet is authoritative; this keeps the mesher's field evaluation --
	// and therefore collision against pasted rubble -- in step with it.
	bool submit_volume(int slot, ve::VolumeData data);
```

with the matching private state:

```cpp
	struct VolumeUpload {
		int slot = -1;
		ve::VolumeData data;
	};
	IslandExtractPass *extract_ = nullptr;         // worker thread only
	std::vector<IslandExtractJob> pending_extract_;
	std::vector<IslandExtractResult> extract_results_;
	std::vector<VolumeUpload> pending_volumes_;
	std::atomic<bool> extract_busy_{false};
```

`extension/src/render/mesh_service.cpp` — in `run()`, after the `MeshPass` is built, also build the extract pass on the same device and pool:

```cpp
	extract_ = new IslandExtractPass();
	if (!extract_->initialize(&rd_device, &pass.volumes())) {
		UtilityFunctions::printerr("MeshService: island extraction unavailable");
		delete extract_;
		extract_ = nullptr;
		// Not fatal: collision meshing still works, and IslandManager reports the loss once.
	}
```

The worker's wait predicate gains the two new queues, and its work loop drains them in this order — **volume uploads first**, because a mesh or an extraction submitted afterwards must see the rubble:

```cpp
	// 1. volume uploads (device-level buffer_update; no list may be open)
	// 2. extraction jobs
	// 3. mesh batches
```

`MeshPass` needs `VolumePool &volumes()` exposed for the extract pass to share (add the accessor next to `config()`).

`stop()` deletes `extract_` on the worker thread, before the device goes away, exactly as it already does for the `MeshPass`.

- [ ] **Step 11: Add the differential hook**

`extension/src/voxel_world.h`:

```cpp
	Dictionary debug_island_extract_diff(Vector3i lo_cell, Vector3i hi_cell);
```

`extension/src/voxel_world.cpp`:

```cpp
Dictionary VoxelWorld::debug_island_extract_diff(Vector3i lo_cell, Vector3i hi_cell) {
	Dictionary d;
	d["ok"] = false;
	ensure_physics_initialized();
	if (!mesh_ || !mesh_->is_valid()) return d;

	const ve::IVec3 lo{lo_cell.x, lo_cell.y, lo_cell.z};
	const ve::IVec3 hi{hi_cell.x, hi_cell.y, hi_cell.z};
	std::vector<ve::IVec3> cells;
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) cells.push_back({x, y, z});
	std::vector<ve::CellBox> boxes;
	if (!ve::greedy_box_merge(cells, ve::kMaxIslandBoxes, &boxes)) return d;

	float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
	for (const ve::CellBox &b : boxes) {
		float a[3], c[3];
		b.world_aabb(a, c);
		for (int k = 0; k < 3; k++) {
			wlo[k] = std::min(wlo[k], a[k]);
			whi[k] = std::max(whi[k], c[k]);
		}
	}
	IslandExtractJob job;
	job.id = 0;
	job.boxes = boxes;
	if (!ve::plan_island_lattice(wlo, whi, ve::kIslandDim, &job.voxel, job.origin)) return d;
	job.dim = ve::kIslandDim;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		// One region's list: a component never spans regions, because Task 3's extent bound
		// (5.6 m) is a fifth of a region and the manager splits any that would.
		job.ops = edit_log_->ops(ve::WorldBounds::region_of_brick(lo));
	}

	// Drive the worker synchronously: this is a diagnostic, not the streaming path.
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(job);
	if (!mesh_->submit_extracts(std::move(jobs))) return d;
	std::vector<IslandExtractResult> results;
	for (int i = 0; i < 2000 && results.empty(); i++) {
		mesh_->collect_extracts(&results);
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (results.empty() || results[0].failed) return d;

	std::vector<float> aabbs(boxes.size() * 6);
	for (size_t i = 0; i < boxes.size(); i++)
		boxes[i].world_aabb(&aabbs[i * 6], &aabbs[i * 6 + 3]);
	ve::VolumeData cpu;
	ve::AnalyticGenerator gen;
	ve::extract_island_volume(gen, job.ops.data(), static_cast<int>(job.ops.size()),
			&volumes_, job.origin, job.voxel, job.dim, aabbs.data(),
			static_cast<int>(boxes.size()), &cpu);

	int worst = 0, mat_mismatch = 0, mat_compared = 0;
	const ve::VolumeData &gpu = results[0].data;
	for (size_t i = 0; i < cpu.sdf.size(); i++) {
		const int diff = std::abs(static_cast<int>(gpu.sdf[i]) - static_cast<int>(cpu.sdf[i]));
		worst = std::max(worst, diff);
		// Materials only where the sample is clear of the surface band, for the same reason
		// test_brick_diff.gd compares them only near-but-not-on it: a one-step sdf drift
		// flips the classification and says nothing about the material logic.
		if (std::abs(static_cast<int>(cpu.sdf[i]) - 128) > 4) {
			mat_compared++;
			if (gpu.mat[i] != cpu.mat[i]) mat_mismatch++;
		}
	}
	d["ok"] = true;
	d["worst_steps"] = worst;
	d["mat_mismatch"] = mat_mismatch;
	d["mat_compared"] = mat_compared;
	d["gpu_solid"] = gpu.solid_voxels;
	d["cpu_solid"] = cpu.solid_voxels;
	d["voxel"] = job.voxel;
	d["boxes"] = static_cast<int>(boxes.size());
	return d;
}
```

- [ ] **Step 12: Run both suites**

Run: `cd extension && scons test`
Expected: PASS.

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: green, including the four new `test_island_extract.gd` cases.

- [ ] **Step 13: Commit**

```bash
git add shaders/island_extract.comp.glsl shaders/common.glslh \
        extension/src/render/island_extract_pass.h extension/src/render/island_extract_pass.cpp \
        extension/src/render/mesh_service.h extension/src/render/mesh_service.cpp \
        extension/src/render/mesh_pass.h \
        extension/src/generator/volume_set.h extension/src/generator/volume_set.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp \
        extension/tests/test_volume_ops.cpp tests/test_island_extract.gd
git commit -m "feat: GPU island extraction into dense volumes"
```

---
### Task 10: `render/island_atlas` + the raymarcher resolves multiple targets

Spec §3's *Multi-target raymarching (islands)*: "Per pixel: march static terrain + listed islands (ray → island local space via inverse body transform → same sphere-trace GLSL); nearest hit wins. Identical G-buffer path → islands shade/shadow/reflect exactly like static terrain."

This task marches **every** live island. Task 11 adds the tile mask that keeps it to 0–3.

**Files:**
- Create: `extension/src/render/island_atlas.h`, `extension/src/render/island_atlas.cpp`
- Modify: `shaders/raymarch.comp.glsl` (substantial restructure)
- Modify: `extension/src/render/camera_params.h`
- Modify: `extension/src/render/raymarch_pass.h`, `extension/src/render/raymarch_pass.cpp`
- Modify: `extension/src/raymarch_compositor.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Test: `tests/test_island_render.gd`

**Interfaces:**
- Consumes: `godot::VolumePool` (Task 7), `ve::VolumeData`, `ve::build_volume_mip` (Task 9).
- Produces: `godot::IslandSlotDesc`, `godot::IslandAtlas` (`initialize`, `teardown`, `is_valid`, `sdf_buffer`, `mat_buffer`, `mip_buffer`, `desc_buffer`, `fallback_mask`, `upload`, `clear_slot`, `upload_descriptors`, `live_count`); `RaymarchPass::render`'s two new arguments; `VoxelWorld::islands()`, `debug_place_test_island`; `CameraParams`'s three repurposed trailing ints. Task 11 replaces the fallback mask; Task 13 fills the descriptors from real bodies.

- [ ] **Step 1: Write the failing test**

Create `tests/test_island_render.gd`:

```gdscript
extends GdUnitTestSuite

# Spec section 3's multi-target raymarching: an island is an ADDITIONAL march target whose
# hit competes with the terrain's on distance alone, so the two shade identically and
# occlude each other exactly.
#
# The island here is extracted from the terrain itself and then placed in the air above it,
# which makes the assertions unambiguous: everything the camera can see at that height is
# either the island or the sky.

const SKY_UP := Color(0.25, 0.45, 0.85) # common.glslh's sky_color for dir.y = +1

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	for i in range(60):
		w.debug_stream_frame(Vector3(20.0, 56.0, 20.0))
	return w

func is_sky(c: Color) -> bool:
	# sky_color is a two-stop gradient; nothing the terrain or an island shades to sits on it.
	return absf(c.r - c.b) > 0.05 and c.b > c.r

func test_an_island_placed_in_the_air_is_hit_by_a_ray(timeout := 60000) -> void:
	var w := make_world()
	# Lift a 2x2x2-cell lump of rock 30 m above where it came from.
	var lift := Vector3(0.0, 30.0, 0.0)
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		lift)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var centre: Vector3 = d["world_center"]

	# Straight down through the island's centre from above it.
	var probe: Dictionary = w.debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	assert_bool(probe["hit"]).override_failure_message(
		"the ray passed through the island").is_true()
	var pos: Vector3 = probe["pos"]
	# It hit the island, not the terrain 30 m below it.
	assert_float(pos.y).is_greater(centre.y - 2.0)
	assert_bool(is_sky(probe["color"])).is_false()

func test_a_ray_beside_the_island_still_sees_the_sky(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]
	# Ten metres to the side of a 1.6 m lump, pointing up: nothing but sky.
	var c: Color = w.debug_raymarch_pixel(centre + Vector3(10, 0, 0), Vector3(0, 1, 0))
	assert_bool(is_sky(c)).override_failure_message(
		"a ray well clear of the island did not see the sky: %s" % c).is_true()

func test_the_island_occludes_the_terrain_behind_it(timeout := 60000) -> void:
	var w := make_world()
	var origin := Vector3(20.4, 90.0, 20.4)
	var before: Dictionary = w.debug_raymarch_probe(origin, Vector3(0, -1, 0))
	assert_bool(before["hit"]).is_true()
	var terrain_y: float = before["pos"].y

	# Place it once to learn where the lattice lands, then again to put it exactly between
	# the camera and the ground it just hit. (The hook's offset is a delta, because the
	# lattice origin is chosen by ve::plan_island_lattice and the caller cannot predict it.)
	var probe_place: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25),
		Vector3i(26, 63, 26), Vector3.ZERO)
	assert_bool(probe_place.get("ok", false)).is_true()
	var want := Vector3(20.4, 80.0, 20.4)
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		want - (probe_place["world_center"] as Vector3))
	assert_bool(d.get("ok", false)).is_true()
	assert_float((d["world_center"] as Vector3).distance_to(want)).is_less(0.01)

	var after: Dictionary = w.debug_raymarch_probe(origin, Vector3(0, -1, 0))
	assert_bool(after["hit"]).is_true()
	assert_float(after["pos"].y).override_failure_message(
		"the island did not occlude the terrain").is_greater(terrain_y + 5.0)

func test_a_rotated_island_is_hit_where_the_transform_puts_it(timeout := 60000) -> void:
	var w := make_world()
	# The same lump, rotated 90 degrees about y and moved. A rotation about the body origin
	# moves the lattice, so a ray that hit before must miss and one aimed at the new place
	# must hit -- which is what proves the inverse transform is applied and not skipped.
	var d: Dictionary = w.debug_place_test_island_rotated(0, Vector3i(25, 62, 25),
		Vector3i(28, 63, 26), Vector3(0.0, 30.0, 0.0), PI * 0.5)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var centre: Vector3 = d["world_center"]
	var probe: Dictionary = w.debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	assert_bool(probe["hit"]).is_true()
	assert_float(probe["pos"].y).is_greater(centre.y - 3.0)
	# The lump is 3 cells long on x and 2 on z; after the rotation its long axis is z, so a
	# ray 1.6 m out along x -- inside the UNROTATED extent, outside the rotated one -- misses.
	var side: Dictionary = w.debug_raymarch_probe(centre + Vector3(1.6, 6, 0), Vector3(0, -1, 0))
	assert_float(side["pos"].y if side["hit"] else -1000.0).is_less(centre.y - 5.0)

func test_a_cleared_slot_stops_being_marched(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]
	assert_bool(w.debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))["hit"]
		).is_true()
	w.debug_clear_test_island(0)
	var probe: Dictionary = w.debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	# The terrain 30 m below is still there, so this hits -- just not up here.
	assert_float(probe["pos"].y if probe["hit"] else -1000.0).is_less(centre.y - 20.0)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_island_render.gd`
Expected: FAIL — `debug_place_test_island` does not exist.

- [ ] **Step 3: Repurpose the three unused trailing ints in `CameraParams`**

`extension/src/render/camera_params.h` — the struct's size does not change; only three `w` components that were documented "unused" acquire meaning:

```cpp
	float params[4];          // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	int32_t dims[4];          // world size in REGIONS (xyz), w = live island count
	int32_t region_origin[4]; // world origin in REGIONS, w = island cull tiles per row
	int32_t atlas_bricks[4];  // atlas grid in bricks, w = island cull tile rows
```

with the note:

```cpp
	// The island cull grid rides in this struct rather than a second push constant so the
	// cull pass and the raymarcher project world points with the SAME camera arithmetic --
	// ndc = (dot(v, right) / (z * tan_x), dot(v, up) / (z * tan_y)) -- and can never disagree
	// about which tile a pixel is in. tiles-per-row 0 means "no mask": march every live
	// island, which is what the 1x1 debug probes (tan fov 0) do.
```

- [ ] **Step 4: Write `render/island_atlas`**

Create `extension/src/render/island_atlas.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "generator/volume_set.h"
#include "render/volume_pool.h"

namespace godot {

// Spec §5's guardrail: "<=32 island bodies".
inline constexpr int kMaxIslands = 32;

// One live island as the raymarcher needs to see it. Written every frame from the body's
// transform, which is why nothing here is a Godot type: IslandManager fills it on the main
// thread and IslandAtlas uploads it on the render thread.
struct IslandSlotDesc {
	bool live = false;
	// Local -> world rotation, COLUMN major: basis[a] is the world direction of local +a.
	// (ve::resample_volume takes the same rotation ROW major; the two conversions are
	// spelled out where they happen so the transpose is never implicit.)
	float basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	float origin[3] = {0, 0, 0};         // body translation, world
	float lattice_origin[3] = {0, 0, 0}; // the lattice's minimum corner in LOCAL space
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	float aabb_lo[3] = {0, 0, 0}; // world AABB of the rotated lattice box (Task 11's cull)
	float aabb_hi[3] = {0, 0, 0};

	// Fills aabb_lo/hi from the other fields. Called by whoever writes the descriptor.
	void recompute_world_aabb();
};

// The render device's copy of every live island: SDF and material bytes (a VolumePool of 32
// slots), the per-island min-max chain, and the descriptor array the shader indexes.
//
// Storage buffers rather than 3D textures for exactly one reason: RenderingDevice can only
// texture_update a whole layer, so a per-slot texture upload would need a staging texture
// and a texture_copy, while a buffer takes a plain offset buffer_update -- and the
// raymarcher already reconstructs trilinearly by hand for bricks, so nothing is lost.
class IslandAtlas {
public:
	~IslandAtlas();

	bool initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return volumes_.is_valid() && desc_.is_valid(); }

	RID sdf_buffer() const { return volumes_.sdf_buffer(); }
	RID mat_buffer() const { return volumes_.mat_buffer(); }
	RID mip_buffer() const { return mip_; }
	RID desc_buffer() const { return desc_; }
	// A one-entry all-ones tile mask, bound whenever the cull pass has not produced one.
	RID fallback_mask() const { return fallback_mask_; }

	// Device-level: record before compute_list_begin.
	bool upload(RenderingDevice *rd, int slot, const ve::VolumeData &data);
	void upload_descriptors(RenderingDevice *rd, const IslandSlotDesc *descs, int count);
	// Marks the slot dead in the descriptor array. The bytes are left as they are: nothing
	// reads a slot whose descriptor says it is not live.
	void clear_slot(RenderingDevice *rd, int slot);

private:
	RenderingDevice *rd_ = nullptr;
	VolumePool volumes_;
	RID mip_, desc_, fallback_mask_;
};

} // namespace godot
```

Create `extension/src/render/island_atlas.cpp`:

```cpp
#include "render/island_atlas.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

// shaders/raymarch.comp.glsl hard-codes these (GLSL cannot include the header). A mismatch
// would read a neighbouring island's bytes rather than failing, so pin them here.
static_assert(ve::kIslandDim == 64, "update ISLAND_DIM in shaders/raymarch.comp.glsl");
static_assert(ve::kIslandVoxelCount == 262144, "update ISLAND_VOXELS in raymarch.comp.glsl");
static_assert(ve::kVolumeMipStride == 8, "update ISLAND_MIP_STRIDE in raymarch.comp.glsl");
static_assert(kMaxIslands == 32, "the tile mask is one uint per tile: 32 bits, 32 islands");

namespace {
constexpr int kMipCells = ve::kIslandDim / ve::kVolumeMipStride;             // 8
constexpr int kMipPerSlot = kMipCells * kMipCells * kMipCells;               // 512
constexpr int64_t kDescBytes = 128;                                          // 8 vec4
} // namespace

void IslandSlotDesc::recompute_world_aabb() {
	const float span = static_cast<float>(dim - 1) * voxel;
	for (int a = 0; a < 3; a++) {
		aabb_lo[a] = 1e30f;
		aabb_hi[a] = -1e30f;
	}
	for (int c = 0; c < 8; c++) {
		const float q[3] = {lattice_origin[0] + ((c & 1) ? span : 0.0f),
				lattice_origin[1] + ((c & 2) ? span : 0.0f),
				lattice_origin[2] + ((c & 4) ? span : 0.0f)};
		for (int a = 0; a < 3; a++) {
			// basis is COLUMN major: world_a = sum_k basis[k * 3 + a] * q[k].
			const float w = basis[0 * 3 + a] * q[0] + basis[1 * 3 + a] * q[1] +
					basis[2 * 3 + a] * q[2] + origin[a];
			aabb_lo[a] = std::min(aabb_lo[a], w);
			aabb_hi[a] = std::max(aabb_hi[a], w);
		}
	}
}

IslandAtlas::~IslandAtlas() {
	teardown();
}

bool IslandAtlas::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	if (!volumes_.initialize(rd, kMaxIslands, ve::kIslandDim)) {
		teardown();
		return false;
	}
	PackedByteArray zero;
	zero.resize(static_cast<int64_t>(kMaxIslands) * kMipPerSlot * 2);
	zero.fill(0);
	mip_ = rd->storage_buffer_create(static_cast<uint32_t>(zero.size()), zero);
	PackedByteArray descs;
	descs.resize(kMaxIslands * kDescBytes);
	descs.fill(0); // every slot dead: dim 0 is what the shader tests
	desc_ = rd->storage_buffer_create(static_cast<uint32_t>(descs.size()), descs);
	PackedByteArray ones;
	ones.resize(4);
	ones.fill(0xFF);
	fallback_mask_ = rd->storage_buffer_create(4, ones);
	if (!mip_.is_valid() || !desc_.is_valid() || !fallback_mask_.is_valid()) {
		UtilityFunctions::printerr("IslandAtlas: buffer creation failed");
		teardown();
		return false;
	}
	return true;
}

void IslandAtlas::teardown() {
	volumes_.teardown();
	if (rd_) {
		for (RID *r : {&mip_, &desc_, &fallback_mask_})
			if (r->is_valid()) rd_->free_rid(*r);
	}
	mip_ = RID();
	desc_ = RID();
	fallback_mask_ = RID();
	rd_ = nullptr;
}

bool IslandAtlas::upload(RenderingDevice *rd, int slot, const ve::VolumeData &data) {
	if (!rd || !is_valid() || slot < 0 || slot >= kMaxIslands) return false;
	if (!volumes_.upload(rd, slot, data)) return false;
	std::vector<uint8_t> mip;
	ve::build_volume_mip(data, &mip);
	if (static_cast<int>(mip.size()) != kMipPerSlot * 2) return false;
	PackedByteArray b;
	b.resize(static_cast<int64_t>(mip.size()));
	std::memcpy(b.ptrw(), mip.data(), mip.size());
	rd->buffer_update(mip_, static_cast<uint32_t>(slot * kMipPerSlot * 2),
			static_cast<uint32_t>(b.size()), b);
	return true;
}

void IslandAtlas::upload_descriptors(RenderingDevice *rd, const IslandSlotDesc *descs,
		int count) {
	if (!rd || !is_valid() || !descs) return;
	PackedByteArray b;
	b.resize(kMaxIslands * kDescBytes);
	b.fill(0);
	float *f = reinterpret_cast<float *>(b.ptrw());
	int32_t *i = reinterpret_cast<int32_t *>(b.ptrw());
	for (int s = 0; s < std::min(count, kMaxIslands); s++) {
		const IslandSlotDesc &d = descs[s];
		const int base = s * 32; // 32 floats per descriptor
		// Rows 0-2: the local->world basis columns, with the body translation in .w.
		for (int a = 0; a < 3; a++) {
			f[base + a * 4 + 0] = d.basis[a * 3 + 0];
			f[base + a * 4 + 1] = d.basis[a * 3 + 1];
			f[base + a * 4 + 2] = d.basis[a * 3 + 2];
			f[base + a * 4 + 3] = d.origin[a];
		}
		f[base + 12] = d.lattice_origin[0];
		f[base + 13] = d.lattice_origin[1];
		f[base + 14] = d.lattice_origin[2];
		f[base + 15] = d.voxel;
		i[base + 16] = d.live ? d.dim : 0; // dim 0 == dead, tested by the shader
		i[base + 17] = 0;
		i[base + 18] = 0;
		i[base + 19] = 0;
		for (int a = 0; a < 3; a++) {
			f[base + 20 + a] = d.aabb_lo[a];
			f[base + 24 + a] = d.aabb_hi[a];
		}
	}
	rd->buffer_update(desc_, 0, static_cast<uint32_t>(b.size()), b);
}

void IslandAtlas::clear_slot(RenderingDevice *rd, int slot) {
	if (!rd || !is_valid() || slot < 0 || slot >= kMaxIslands) return;
	PackedByteArray b;
	b.resize(kDescBytes);
	b.fill(0);
	rd->buffer_update(desc_, static_cast<uint32_t>(slot * kDescBytes),
			static_cast<uint32_t>(kDescBytes), b);
}
```

- [ ] **Step 5: Restructure `shaders/raymarch.comp.glsl`**

The terrain march moves into a function returning a hit record, the island march joins it, and shading happens once on the winner. Everything above `main()` in the existing file is unchanged except for the new bindings; replace from `bool cell8_may_have_surface` onwards with:

```glsl
bool cell8_may_have_surface(int slot, ivec3 cell) { // cell in [0,8)^3, 2 voxels per cell
	uvec2 mm = texelFetch(mip8_atlas, atlas_base(slot, pc.atlas_bricks.xyz, 8) + cell, 0).xy;
	return mm.x <= ENCODED_ZERO && mm.y >= ENCODED_ZERO;
}

// ---------------------------------------------------------------------------------------
// Islands (spec §3, "Multi-target raymarching"). Each is a dense 64^3 volume in a pool of
// byte-packed storage buffers, placed by a rigid transform. Its hit competes with the
// terrain's on distance alone, which is what makes the two shade identically.
// ---------------------------------------------------------------------------------------
const int ISLAND_DIM = 64;          // ve::kIslandDim
const int ISLAND_VOXELS = 262144;   // 64^3
const int ISLAND_MIP_STRIDE = 8;    // ve::kVolumeMipStride
const int ISLAND_MIP_CELLS = 8;     // ISLAND_DIM / ISLAND_MIP_STRIDE
const int ISLAND_MIP_PER_SLOT = 512;

layout(set = 0, binding = 13, std430) readonly buffer IslandSdf { uint w[]; } island_sdf;
layout(set = 0, binding = 14, std430) readonly buffer IslandMat { uint w[]; } island_mat;
layout(set = 0, binding = 15, std430) readonly buffer IslandMip { uint w[]; } island_mip;
// Eight vec4 per island: basis columns 0-2 with the body translation in .w, then
// (lattice origin.xyz, voxel), then (dim, 0, 0, 0) as ints, then the world AABB.
layout(set = 0, binding = 16, std430) readonly buffer IslandDesc { vec4 v[]; } island_desc;
// One uint per 16x16 screen tile, bit i = "island i may be visible here" (Task 11). When
// pc.region_origin.w is 0 the buffer is a single all-ones entry and every island is marched.
layout(set = 0, binding = 17, std430) readonly buffer TileMask { uint v[]; } tile_mask;

struct Island {
	mat3 basis;    // local -> world
	vec3 pos;      // body translation, world
	vec3 lo;       // lattice minimum corner, LOCAL
	float voxel;
	int dim;
};

bool island_load(int i, out Island isl) {
	vec4 r0 = island_desc.v[i * 8 + 0];
	vec4 r1 = island_desc.v[i * 8 + 1];
	vec4 r2 = island_desc.v[i * 8 + 2];
	vec4 lv = island_desc.v[i * 8 + 3];
	int dim = floatBitsToInt(island_desc.v[i * 8 + 4].x);
	if (dim < 2) return false; // dead slot
	isl.basis = mat3(r0.xyz, r1.xyz, r2.xyz); // mat3(c0, c1, c2): columns, as written
	isl.pos = vec3(r0.w, r1.w, r2.w);
	isl.lo = lv.xyz;
	isl.voxel = lv.w;
	isl.dim = dim;
	return true;
}

uint island_byte_sdf(int i) {
	return (island_sdf.w[i >> 2] >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}
uint island_byte_mat(int i) {
	return (island_mat.w[i >> 2] >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}
uint island_byte_mip(int i) {
	return (island_mip.w[i >> 2] >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}

float island_lattice(int slot, int dim, ivec3 v) {
	int i = slot * ISLAND_VOXELS + v.x + v.y * dim + v.z * dim * dim;
	return decode_sdf(float(island_byte_sdf(i)) / 255.0);
}

// Trilinear reconstruction in LOCAL space, mirroring ve::sample_volume_lattice's inside
// branch. Callers clamp q to the lattice box first, so no outside branch is needed here.
float island_sdf_at(int slot, Island isl, vec3 q) {
	vec3 l = clamp((q - isl.lo) / isl.voxel, vec3(0.0), vec3(float(isl.dim - 1)));
	ivec3 i0 = ivec3(l);
	ivec3 i1 = min(i0 + 1, ivec3(isl.dim - 1));
	vec3 f = l - vec3(i0);
	float c000 = island_lattice(slot, isl.dim, ivec3(i0.x, i0.y, i0.z));
	float c100 = island_lattice(slot, isl.dim, ivec3(i1.x, i0.y, i0.z));
	float c010 = island_lattice(slot, isl.dim, ivec3(i0.x, i1.y, i0.z));
	float c110 = island_lattice(slot, isl.dim, ivec3(i1.x, i1.y, i0.z));
	float c001 = island_lattice(slot, isl.dim, ivec3(i0.x, i0.y, i1.z));
	float c101 = island_lattice(slot, isl.dim, ivec3(i1.x, i0.y, i1.z));
	float c011 = island_lattice(slot, isl.dim, ivec3(i0.x, i1.y, i1.z));
	float c111 = island_lattice(slot, isl.dim, ivec3(i1.x, i1.y, i1.z));
	return mix(mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y),
	           mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y), f.z);
}

uint island_material_at(int slot, Island isl, vec3 q) {
	vec3 l = clamp((q - isl.lo) / isl.voxel, vec3(0.0), vec3(float(isl.dim - 1)));
	ivec3 m = min(ivec3(l + 0.5), ivec3(isl.dim - 1));
	int i = slot * ISLAND_VOXELS + m.x + m.y * isl.dim + m.z * isl.dim * isl.dim;
	return island_byte_mat(i);
}

// Spec §3's "own min-max mip": the same inclusive-corner soundness argument the brick chain
// rests on, so a "no surface" verdict is a skip that cannot tunnel.
bool island_cell_has_surface(int slot, ivec3 c) {
	int i = (slot * ISLAND_MIP_PER_SLOT + c.x + c.y * ISLAND_MIP_CELLS +
			c.z * ISLAND_MIP_CELLS * ISLAND_MIP_CELLS) * 2;
	uint mn = island_byte_mip(i);
	uint mx = island_byte_mip(i + 1);
	return mn <= ENCODED_ZERO && mx >= ENCODED_ZERO;
}

// Slab test. rd components are nudged off zero rather than divided by it: 0 * inf is NaN,
// and a NaN here would silently drop the island for that pixel.
bool ray_box(vec3 ro, vec3 rd, vec3 lo, vec3 hi, out float t0, out float t1) {
	vec3 srd = vec3(abs(rd.x) < 1e-8 ? 1e-8 : rd.x, abs(rd.y) < 1e-8 ? 1e-8 : rd.y,
			abs(rd.z) < 1e-8 ? 1e-8 : rd.z);
	vec3 a = (lo - ro) / srd;
	vec3 b = (hi - ro) / srd;
	vec3 tmn = min(a, b), tmx = max(a, b);
	t0 = max(max(tmn.x, tmn.y), tmn.z);
	t1 = min(min(tmx.x, tmx.y), tmx.z);
	return t1 >= max(t0, 0.0);
}

struct Hit {
	bool hit;
	float t;
	vec3 p;
	vec3 n;
	uint mat;
};

// Sphere-traces one island. `best` is updated only when this island is nearer, so calling it
// for every island in the tile's mask resolves "nearest hit wins" with no sorting.
void march_island(int slot, vec3 ro, vec3 rd, inout Hit best) {
	Island isl;
	if (!island_load(slot, isl)) return;

	// World -> local. The basis is orthonormal, so its inverse is its transpose and the ray
	// stays unit length: t values are directly comparable with the terrain's.
	mat3 inv = transpose(isl.basis);
	vec3 ro_l = inv * (ro - isl.pos);
	vec3 rd_l = inv * rd;
	vec3 span = vec3(float(isl.dim - 1) * isl.voxel);

	float t0, t1;
	if (!ray_box(ro_l, rd_l, isl.lo, isl.lo + span, t0, t1)) return;
	t0 = max(t0, 0.0);
	t1 = min(t1, best.t);
	if (t0 > t1) return;

	float t = t0;
	float cell_m = float(ISLAND_MIP_STRIDE) * isl.voxel;
	for (int k = 0; k < 192; k++) {
		if (t > t1) return;
		vec3 q = ro_l + rd_l * t;
		ivec3 c = clamp(ivec3(floor((q - isl.lo) / cell_m)), ivec3(0),
				ivec3(ISLAND_MIP_CELLS - 1));
		if (!island_cell_has_surface(slot, c)) {
			// Jump to the cell's exit face, exactly as the brick march does, with a floor on
			// the step so a ray grazing a face still makes progress.
			vec3 clo = isl.lo + vec3(c) * cell_m;
			vec3 chi = clo + cell_m;
			vec3 far = mix(clo, chi, step(0.0, rd_l));
			vec3 tf = (far - q) / vec3(abs(rd_l.x) < 1e-8 ? 1e-8 : rd_l.x,
					abs(rd_l.y) < 1e-8 ? 1e-8 : rd_l.y,
					abs(rd_l.z) < 1e-8 ? 1e-8 : rd_l.z);
			t += max(min(tf.x, min(tf.y, tf.z)), 0.002);
			continue;
		}
		float d = island_sdf_at(slot, isl, q);
		if (d < 0.002) {
			for (int r = 0; r < 4; r++) { // secant refinement, as the terrain march does
				q = ro_l + rd_l * t;
				t += island_sdf_at(slot, isl, q) * 0.5;
			}
			if (t > best.t) return; // refinement pushed it behind the current winner
			q = ro_l + rd_l * t;
			const float e = 0.5 * 0.05;
			vec3 n_l = normalize(vec3(
				island_sdf_at(slot, isl, q + vec3(e, 0, 0)) -
					island_sdf_at(slot, isl, q - vec3(e, 0, 0)),
				island_sdf_at(slot, isl, q + vec3(0, e, 0)) -
					island_sdf_at(slot, isl, q - vec3(0, e, 0)),
				island_sdf_at(slot, isl, q + vec3(0, 0, e)) -
					island_sdf_at(slot, isl, q - vec3(0, 0, e))));
			best.hit = true;
			best.t = t;
			best.p = ro + rd * t;
			best.n = normalize(isl.basis * n_l);
			best.mat = island_material_at(slot, isl, q);
			return;
		}
		t += max(d * 0.9, 0.005);
	}
}

// The M1/M2 terrain march, unchanged in behaviour, returning a hit record instead of a
// colour so an island can outrank it.
Hit march_terrain(vec3 ro, vec3 rd, float max_dist) {
	Hit h;
	h.hit = false;
	h.t = max_dist;
	h.p = vec3(0.0);
	h.n = vec3(0.0, 1.0, 0.0);
	h.mat = 0u;

	ivec3 map = ivec3(floor(ro / BRICK_SIZE));
	vec3 delta = abs(vec3(BRICK_SIZE) / rd);
	ivec3 st = ivec3(sign(rd));
	vec3 side = (vec3(map) * BRICK_SIZE - ro + (vec3(st) * 0.5 + 0.5) * BRICK_SIZE) / rd;
	if (st.x == 0) side.x = 1.0 / 0.0;
	if (st.y == 0) side.y = 1.0 / 0.0;
	if (st.z == 0) side.z = 1.0 / 0.0;
	float t_prev = 0.0;

	for (int i = 0; i < 1024; i++) {
		float t_exit = min(side.x, min(side.y, side.z));
		if (t_exit > max_dist) break;

		int slot = slot_at(map);
		if (slot >= 0 && brick_may_have_surface(slot)) {
			bool has_material = palette_buf.ids[slot * 4] != 0u;
			float t = t_prev;
			for (int j = 0; j < 64; j++) {
				if (t > t_exit) break;
				vec3 p = ro + rd * t;
				vec3 vox = (p - vec3(map) * BRICK_SIZE) / VOXEL_SIZE;
				ivec3 cell8 = clamp(ivec3(floor(vox * 0.5)), ivec3(0), ivec3(7));
				if (!cell8_may_have_surface(slot, cell8)) {
					vec3 cell_lo = vec3(map) * BRICK_SIZE + vec3(cell8 * 2) * VOXEL_SIZE;
					vec3 cell_hi = cell_lo + 2.0 * VOXEL_SIZE;
					vec3 far = mix(cell_lo, cell_hi, step(0.0, rd));
					vec3 tf = (far - p) / rd;
					if (st.x == 0) tf.x = 1.0 / 0.0;
					if (st.y == 0) tf.y = 1.0 / 0.0;
					if (st.z == 0) tf.z = 1.0 / 0.0;
					t = min(t + max(min(tf.x, min(tf.y, tf.z)), 0.002), t_exit);
					continue;
				}
				float d = world_sdf(p);
				if (d < 0.002 && has_material) {
					for (int k = 0; k < 4; k++) {
						float dk = world_sdf(p);
						t += dk * 0.5;
						p = ro + rd * t;
					}
					h.hit = true;
					h.t = t;
					h.p = p;
					h.n = calc_normal(p, map, slot);
					h.mat = material_at(p, map, slot);
					return h;
				}
				t += max(d * 0.9, 0.005);
			}
		}

		if (side.x < side.y && side.x < side.z) { t_prev = side.x; side.x += delta.x; map.x += st.x; }
		else if (side.y < side.z)               { t_prev = side.y; side.y += delta.y; map.y += st.y; }
		else                                    { t_prev = side.z; side.z += delta.z; map.z += st.z; }
	}
	return h;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_color);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

	vec3 ro = pc.cam_pos.xyz;
	vec3 rd = normalize(pc.cam_fwd.xyz
		+ pc.cam_right.xyz * ndc.x * pc.params.x
		+ pc.cam_up.xyz * ndc.y * pc.params.y);
	float max_dist = pc.params.z;

	Hit best = march_terrain(ro, rd, max_dist);

	// Which islands could be here? pc.region_origin.w is the cull grid's tiles-per-row, and
	// 0 means "no mask" -- the 1x1 debug probes and any frame before the cull pass has run.
	int island_count = min(pc.dims.w, 32);
	uint mask = island_count > 0 ? 0xFFFFFFFFu : 0u;
	int tiles_x = pc.region_origin.w;
	if (tiles_x > 0) {
		ivec2 tile = px / 16;
		mask = tile_mask.v[tile.y * tiles_x + tile.x];
	}
	if (island_count < 32) mask &= (1u << uint(island_count)) - 1u;
	while (mask != 0u) {
		int i = findLSB(mask);
		mask &= mask - 1u;
		march_island(i, ro, rd, best);
	}

	// One shading path for both (spec §3: "islands shade/shadow/reflect exactly like static
	// terrain"). M6 replaces this with the deferred cel stack; the point is that there is
	// exactly one of it.
	vec3 color = sky_color(rd);
	vec4 hitpos = vec4(0.0);
	if (best.hit) {
		vec3 alb = material_albedo(best.mat);
		vec3 sun = normalize(vec3(0.6, 0.8, 0.3));
		float lam = max(dot(best.n, sun), 0.0);
		color = alb * (0.25 + 0.75 * lam);
		hitpos = vec4(best.p, 1.0);
	}

	if (best.hit && edits.params.x > 0.0 &&
			length(best.p - edits.center.xyz) < edits.params.x) {
		uint t = uint(edits.params.y);
		vec3 tint = t == 0u ? vec3(1.0, 0.55, 0.1)
		          : t == 1u ? material_albedo(4u)
		          : material_albedo(uint(edits.params.z));
		color = mix(color, tint, 0.45);
	}

	imageStore(out_color, px, vec4(color, 1.0));
	imageStore(out_hitpos, px, hitpos);
}
```

> **`findLSB` returns `int` for a `uint` argument in GLSL 4.60** — that is the signature used above. If glslang complains, the equivalent is a plain `for (int i = 0; i < island_count; i++) if ((mask & (1u << uint(i))) != 0u)`, which costs a branch per slot instead of per set bit; record the swap as errata if it happens.

- [ ] **Step 6: Bind the atlas in `RaymarchPass`**

`extension/src/render/raymarch_pass.h` — forward-declare `class IslandAtlas;` and change the signature:

```cpp
	// `islands` may be null (no island support yet initialised) and `tile_mask` invalid (no
	// cull pass has run); both fall back to the atlas's own all-ones single-entry mask.
	bool render(RenderingDevice *rd, const GpuAtlas &atlas, const IslandAtlas *islands,
			RID tile_mask, const ve::CameraParams &cam, int width, int height,
			const float edit_state[6]);
```

with `RID uset_mask_;` added next to the other uniform-set keys.

`extension/src/render/raymarch_pass.cpp` — `rebuild_targets` gains the five entries and takes the atlas plus the mask RID; the set is rebuilt when the size, the mask RID, or the island atlas changes:

```cpp
	// 13-17: island sdf, material, min-max chain, descriptors, tile mask.
	const RID island_bufs[5] = {islands->sdf_buffer(), islands->mat_buffer(),
			islands->mip_buffer(), islands->desc_buffer(),
			tile_mask.is_valid() ? tile_mask : islands->fallback_mask()};
	for (int i = 13; i <= 17; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u[i]->set_binding(i);
		u[i]->add_id(island_bufs[i - 13]);
	}
```

(grow the `u[13]` array to `u[18]` and both loops accordingly), and in `render`:

```cpp
	if (!islands || !islands->is_valid()) return false;
	const RID mask = tile_mask.is_valid() ? tile_mask : islands->fallback_mask();
	if (width != width_ || height != height_ || mask != uset_mask_ || !uset_.is_valid()) {
		rebuild_targets(rd, atlas, islands, mask, width, height);
		uset_mask_ = mask;
	}
```

- [ ] **Step 7: Own the atlas in `VoxelWorld` and pass it through**

`extension/src/voxel_world.h` — `IslandAtlas *islands_ = nullptr;` with `IslandAtlas *islands() { return islands_; }`, created in `ensure_initialized` right after the atlas and deleted in `teardown_gpu` **before** the atlas (its uniform sets reference nothing else, but the ordering rule is uniform: passes before pools):

```cpp
	islands_ = new IslandAtlas();
	if (!islands_->initialize(device)) { teardown_gpu(); return; }
```

Both debug probes and the compositor gain the two arguments:

```cpp
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, 1, 1, kNoEdit))
		return Color(1, 0, 1);
```

`debug_raymarch_probe` also starts reporting the hit position, which the island tests need:

```cpp
	d["pos"] = Vector3(hf[0], hf[1], hf[2]);
```

placed immediately after the `d["hit"] = true;` line.

`extension/src/raymarch_compositor.cpp` — set the island count and pass the atlas:

```cpp
	cp.dims[3] = world->island_slot_count();
	cp.region_origin[3] = 0; // Task 11 sets the cull grid
	...
	if (!rmp->render(rd, *atlas, world->islands(), RID(), cp, rw, rh, edit_state)) return;
```

with `int VoxelWorld::island_slot_count() const` returning 0 until Task 13 fills it (declare it now, `return island_slots_;` over a plain `int island_slots_ = 0;`).

> `island_slot_count()` is a **high-water mark, not a population**: the shader masks off bits at or above it and then tests each remaining slot's descriptor for `dim >= 2`, so a dead slot below the mark costs one branch and nothing else. Calling it a count would invite someone to decrement it when an island despawns, which would silently stop marching every slot above the gap.

- [ ] **Step 8: Add the test-island hooks**

`extension/src/voxel_world.h`:

```cpp
	Dictionary debug_place_test_island(int slot, Vector3i lo_cell, Vector3i hi_cell,
			Vector3 offset);
	Dictionary debug_place_test_island_rotated(int slot, Vector3i lo_cell, Vector3i hi_cell,
			Vector3 offset, float yaw);
	void debug_clear_test_island(int slot);
```

`extension/src/voxel_world.cpp` — one shared body; the plain version is the rotated one with `yaw = 0`:

```cpp
Dictionary VoxelWorld::debug_place_test_island_rotated(int slot, Vector3i lo_cell,
		Vector3i hi_cell, Vector3 offset, float yaw) {
	Dictionary d;
	d["ok"] = false;
	ensure_initialized();
	ensure_physics_initialized();
	RenderingDevice *device = rd();
	if (!device || !islands_ || !mesh_ || !mesh_->is_valid()) return d;

	// Extract the component exactly as the real pipeline does (Task 9's hook shares this
	// code path deliberately: a test island is a real island with a hand-picked cell set).
	std::vector<ve::IVec3> cells;
	for (int z = lo_cell.z; z <= hi_cell.z; z++)
		for (int y = lo_cell.y; y <= hi_cell.y; y++)
			for (int x = lo_cell.x; x <= hi_cell.x; x++) cells.push_back({x, y, z});
	std::vector<ve::CellBox> boxes;
	if (!ve::greedy_box_merge(cells, ve::kMaxIslandBoxes, &boxes)) return d;
	float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
	for (const ve::CellBox &b : boxes) {
		float a[3], c[3];
		b.world_aabb(a, c);
		for (int k = 0; k < 3; k++) {
			wlo[k] = std::min(wlo[k], a[k]);
			whi[k] = std::max(whi[k], c[k]);
		}
	}
	IslandExtractJob job;
	job.id = slot;
	job.boxes = boxes;
	job.dim = ve::kIslandDim;
	if (!ve::plan_island_lattice(wlo, whi, job.dim, &job.voxel, job.origin)) return d;
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		job.ops = edit_log_->ops(ve::WorldBounds::region_of_brick(
				{lo_cell.x, lo_cell.y, lo_cell.z}));
	}
	std::vector<IslandExtractJob> jobs;
	jobs.push_back(job);
	if (!mesh_->submit_extracts(std::move(jobs))) return d;
	std::vector<IslandExtractResult> results;
	for (int i = 0; i < 2000 && results.empty(); i++) {
		mesh_->collect_extracts(&results);
		if (results.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (results.empty() || results[0].failed) return d;
	if (!islands_->upload(device, slot, results[0].data)) return d;

	// The body's local frame is the birth world frame shifted so the body origin is the
	// lattice's centre -- the same convention IslandManager uses (Task 13), so the rotation
	// happens about the piece rather than about the world origin.
	const float span = static_cast<float>(job.dim - 1) * job.voxel;
	IslandSlotDesc desc;
	desc.live = true;
	desc.dim = job.dim;
	desc.voxel = job.voxel;
	const float c = -0.5f * span;
	desc.lattice_origin[0] = c;
	desc.lattice_origin[1] = c;
	desc.lattice_origin[2] = c;
	const float cs = std::cos(yaw), sn = std::sin(yaw);
	// COLUMN major: basis[0..2] is the world direction of local +x, and so on.
	const float basis[9] = {cs, 0.0f, -sn, 0.0f, 1.0f, 0.0f, sn, 0.0f, cs};
	std::memcpy(desc.basis, basis, sizeof(basis));
	for (int a = 0; a < 3; a++)
		desc.origin[a] = job.origin[a] + 0.5f * span;
	desc.origin[0] += offset.x;
	desc.origin[1] += offset.y;
	desc.origin[2] += offset.z;
	desc.recompute_world_aabb();

	IslandSlotDesc all[kMaxIslands];
	all[slot] = desc;
	islands_->upload_descriptors(device, all, kMaxIslands);
	island_slots_ = std::max(island_slots_, slot + 1);
	device->submit();
	device->sync();

	d["ok"] = true;
	d["world_center"] = Vector3(desc.origin[0], desc.origin[1], desc.origin[2]);
	d["voxel"] = job.voxel;
	d["solid"] = results[0].data.solid_voxels;
	return d;
}

Dictionary VoxelWorld::debug_place_test_island(int slot, Vector3i lo_cell, Vector3i hi_cell,
		Vector3 offset) {
	return debug_place_test_island_rotated(slot, lo_cell, hi_cell, offset, 0.0f);
}

void VoxelWorld::debug_clear_test_island(int slot) {
	RenderingDevice *device = rd();
	if (!device || !islands_) return;
	islands_->clear_slot(device, slot);
	device->submit();
	device->sync();
}
```

- [ ] **Step 9: Run the tests**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_island_render.gd`
Expected: PASS — five cases.

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: green. `test_raymarch_pixel.gd`, `test_raymarch_magenta.gd`, `test_raymarch_mips.gd` and `test_edit_pipeline.gd` all go through the shader that was just restructured — if any regresses, the terrain march's behaviour changed, and the only correct fix is to make `march_terrain` byte-for-byte the old loop.

- [ ] **Step 10: Commit**

```bash
git add shaders/raymarch.comp.glsl extension/src/render/island_atlas.h \
        extension/src/render/island_atlas.cpp extension/src/render/camera_params.h \
        extension/src/render/raymarch_pass.h extension/src/render/raymarch_pass.cpp \
        extension/src/raymarch_compositor.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_island_render.gd
git commit -m "feat: islands as additional raymarch targets"
```

---

### Task 11: `render/island_cull_pass` — 16×16 tiles decide which islands a pixel marches

Spec §3: "**Tiled target culling:** screen split into 16×16px tiles; island world-AABBs projected per tile (tiled-light-culling style) → each pixel marches only the 0–3 islands overlapping its tile/depth range, never all 32."

**Files:**
- Create: `shaders/island_cull.comp.glsl`, `extension/src/render/island_cull_pass.h`, `extension/src/render/island_cull_pass.cpp`
- Modify: `extension/src/raymarch_compositor.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Test: `tests/test_island_render.gd` (added cases)

**Interfaces:**
- Consumes: `IslandAtlas::desc_buffer()` (Task 10), `ve::CameraParams`.
- Produces: `kIslandTileSize`, `godot::IslandCullPass` (`initialize`, `teardown`, `is_valid`, `render`, `mask_buffer`, `tiles_x`, `tiles_y`); `VoxelWorld::debug_island_tile_mask`.

- [ ] **Step 1: Write the failing test cases**

Append to `tests/test_island_render.gd`:

```gdscript
# Spec section 3's tiled target culling. The mask is one uint per 16x16 tile, bit i set when
# island i's world AABB may cover that tile. Correctness has one direction that matters: a
# tile that CAN see the island must have the bit, or the island vanishes for those pixels.
# An extra bit only costs a march.
const TAN_X := 0.6
const TAN_Y := 0.4

func test_the_tile_mask_marks_the_tiles_the_island_covers(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]

	# Look straight at it from 20 m away, at 128x128 -> an 8x8 tile grid.
	var eye := centre + Vector3(0.0, 0.0, 20.0)
	var mask: PackedInt32Array = w.debug_island_tile_mask(eye, Vector3(0, 0, -1),
		TAN_X, TAN_Y, 128, 128)
	assert_int(mask.size()).is_equal(64)
	var set_tiles := 0
	for m in mask:
		if (m & 1) != 0:
			set_tiles += 1
	# A 1.6 m lump 20 m away subtends a small part of the view: some tiles, not all of them.
	assert_int(set_tiles).is_greater(0)
	assert_int(set_tiles).is_less(64)
	# The island sits at NDC (0, 0), which is the shared corner of the four middle tiles;
	# whichever of them the inclusive bounds hand it to, at least one must be marked.
	var middle: int = mask[3 * 8 + 3] | mask[3 * 8 + 4] | mask[4 * 8 + 3] | mask[4 * 8 + 4]
	assert_int(middle & 1).is_not_equal(0)

func test_an_island_behind_the_camera_marks_nothing(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]
	var mask: PackedInt32Array = w.debug_island_tile_mask(centre + Vector3(0, 0, 20),
		Vector3(0, 0, 1), TAN_X, TAN_Y, 128, 128)
	for m in mask:
		assert_int(m & 1).is_equal(0)

func test_an_island_the_camera_is_inside_marks_every_tile(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	# Inside the lattice box, where the projection of its corners says nothing useful: the
	# pass must fail SAFE and mark everything rather than culling the island away.
	var mask: PackedInt32Array = w.debug_island_tile_mask(d["world_center"], Vector3(0, 0, -1),
		TAN_X, TAN_Y, 128, 128)
	for m in mask:
		assert_int(m & 1).is_not_equal(0)

func test_a_cleared_slot_is_never_marked(timeout := 60000) -> void:
	var w := make_world()
	var a: Dictionary = w.debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(a.get("ok", false)).is_true()
	# A second island in slot 3, in the same place, then killed. Its BYTES stay in the atlas
	# by design (clear_slot only zeroes the descriptor), so this is the test that the
	# descriptor -- not the bytes -- is what decides whether a slot is marched.
	var b: Dictionary = w.debug_place_test_island(3, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(b.get("ok", false)).is_true()
	var eye: Vector3 = (a["world_center"] as Vector3) + Vector3(0, 0, 20)
	var both: PackedInt32Array = w.debug_island_tile_mask(eye, Vector3(0, 0, -1), TAN_X,
		TAN_Y, 128, 128)
	var saw_three := false
	for m in both:
		if (m & 8) != 0:
			saw_three = true
	assert_bool(saw_three).is_true()

	w.debug_clear_test_island(3)
	var after: PackedInt32Array = w.debug_island_tile_mask(eye, Vector3(0, 0, -1), TAN_X,
		TAN_Y, 128, 128)
	var live_zero := false
	for m in after:
		assert_int(m & 8).is_equal(0)
		if (m & 1) != 0:
			live_zero = true
	assert_bool(live_zero).is_true()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_island_render.gd`
Expected: FAIL — `debug_island_tile_mask` does not exist.

- [ ] **Step 3: Write `shaders/island_cull.comp.glsl`**

```glsl
#[compute]
#version 460

#include "common.glslh"

// One thread per 16x16 screen tile (spec §3). The tile grid is small -- 1440p at 0.66x is
// 95 x 54 tiles -- so a thread per tile testing 32 AABBs is ~160k corner projections, well
// under 0.05 ms.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer IslandDesc { vec4 v[]; } island_desc;
layout(set = 0, binding = 1, std430) writeonly buffer TileMask { uint v[]; } tile_mask;

// ve::CameraParams, byte for byte -- the SAME struct the raymarcher takes. Sharing it is
// what guarantees the two agree about where a world point lands on screen: there is one
// projection, written once, below.
layout(push_constant, std430) uniform Push {
	vec4 cam_pos;
	vec4 cam_right;
	vec4 cam_up;
	vec4 cam_fwd;
	vec4 params;          // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	ivec4 dims;           // w = live island count
	ivec4 region_origin;  // w = tiles per row
	ivec4 atlas_bricks;   // w = tile rows
} pc;

const int TILE = 16; // kIslandTileSize

void main() {
	ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
	int tiles_x = pc.region_origin.w;
	int tiles_y = pc.atlas_bricks.w;
	if (tile.x >= tiles_x || tile.y >= tiles_y) return;
	int count = min(pc.dims.w, 32);

	// The tile's NDC rectangle. The raymarcher builds a pixel's ray as
	// normalize(fwd + right * ndc.x * tan_x + up * ndc.y * tan_y) with
	// ndc = ((px + 0.5) / size * 2 - 1, 1 - (py + 0.5) / size * 2), so a world point at
	// (dot(v, right), dot(v, up), dot(v, fwd)) = (x, y, z) lands at ndc = (x / (z * tan_x),
	// y / (z * tan_y)). The tile spans whole pixels, so its NDC bounds come from its corner
	// pixel edges -- inclusive on both sides, which is the conservative direction.
	vec2 size = vec2(float(tiles_x * TILE), float(tiles_y * TILE));
	vec2 lo_px = vec2(tile) * float(TILE);
	vec2 hi_px = lo_px + float(TILE);
	vec2 ndc_lo = vec2(lo_px.x / size.x * 2.0 - 1.0, 1.0 - hi_px.y / size.y * 2.0);
	vec2 ndc_hi = vec2(hi_px.x / size.x * 2.0 - 1.0, 1.0 - lo_px.y / size.y * 2.0);

	uint mask = 0u;
	for (int i = 0; i < count; i++) {
		int dim = floatBitsToInt(island_desc.v[i * 8 + 4].x);
		if (dim < 2) continue; // dead slot
		vec3 lo = island_desc.v[i * 8 + 5].xyz;
		vec3 hi = island_desc.v[i * 8 + 6].xyz;

		vec2 smin = vec2(1e30), smax = vec2(-1e30);
		bool near_clip = false;
		float zmin = 1e30;
		for (int c = 0; c < 8; c++) {
			vec3 p = vec3((c & 1) != 0 ? hi.x : lo.x, (c & 2) != 0 ? hi.y : lo.y,
					(c & 4) != 0 ? hi.z : lo.z);
			vec3 v = p - pc.cam_pos.xyz;
			float z = dot(v, pc.cam_fwd.xyz);
			zmin = min(zmin, z);
			// A corner at or behind the eye plane projects to nonsense. Fail SAFE: the
			// island is marked everywhere rather than culled away, which costs a march and
			// cannot make it disappear. Degenerate fov (the 1x1 debug probes) does the same.
			if (z < 0.01 || pc.params.x <= 0.0 || pc.params.y <= 0.0) {
				near_clip = true;
				break;
			}
			vec2 s = vec2(dot(v, pc.cam_right.xyz) / (z * pc.params.x),
					dot(v, pc.cam_up.xyz) / (z * pc.params.y));
			smin = min(smin, s);
			smax = max(smax, s);
		}
		if (near_clip) {
			mask |= 1u << uint(i);
			continue;
		}
		if (zmin > pc.params.z) continue; // entirely past the march's reach
		if (smax.x < ndc_lo.x || smin.x > ndc_hi.x) continue;
		if (smax.y < ndc_lo.y || smin.y > ndc_hi.y) continue;
		mask |= 1u << uint(i);
	}
	tile_mask.v[tile.y * tiles_x + tile.x] = mask;
}
```

- [ ] **Step 4: Write `render/island_cull_pass`**

Create `extension/src/render/island_cull_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/camera_params.h"

namespace godot {

class IslandAtlas;

// Spec §3's 16 x 16 px tiles.
inline constexpr int kIslandTileSize = 16;

// Writes one uint per screen tile: bit i means "island i may be visible in this tile". The
// raymarcher then marches only those bits, which is what keeps the per-pixel cost at spec
// §3's "0-3 islands" instead of all 32.
class IslandCullPass {
public:
	~IslandCullPass();

	bool initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }

	// Sizes the mask to the raymarch target, records its own compute list and dispatches.
	// Returns false when nothing was recorded (in which case the caller passes RID() to the
	// raymarcher and every live island is marched -- correct, just slower).
	bool render(RenderingDevice *rd, const IslandAtlas &atlas, const ve::CameraParams &cam,
			int width, int height, int island_count);

	RID mask_buffer() const { return mask_; }
	int tiles_x() const { return tiles_x_; }
	int tiles_y() const { return tiles_y_; }

private:
	void rebuild(RenderingDevice *rd, const IslandAtlas &atlas, int tx, int ty);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, uset_, mask_;
	int tiles_x_ = 0, tiles_y_ = 0;
};

} // namespace godot
```

Create `extension/src/render/island_cull_pass.cpp` — the loader and teardown ordering follow `IslandExtractPass` exactly; the only interesting parts are:

```cpp
void IslandCullPass::rebuild(RenderingDevice *rd, const IslandAtlas &atlas, int tx, int ty) {
	// Uniform set first: it references the mask buffer about to be freed.
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	if (mask_.is_valid()) rd->free_rid(mask_);
	PackedByteArray zero;
	zero.resize(static_cast<int64_t>(tx) * ty * 4);
	zero.fill(0);
	mask_ = rd->storage_buffer_create(static_cast<uint32_t>(zero.size()), zero);
	tiles_x_ = tx;
	tiles_y_ = ty;
	uset_ = rd->uniform_set_create(
			Array::make(storage(0, atlas.desc_buffer()), storage(1, mask_)), shader_, 0);
}

bool IslandCullPass::render(RenderingDevice *rd, const IslandAtlas &atlas,
		const ve::CameraParams &cam, int width, int height, int island_count) {
	if (!rd || !is_valid() || width <= 0 || height <= 0 || island_count <= 0) return false;
	const int tx = (width + kIslandTileSize - 1) / kIslandTileSize;
	const int ty = (height + kIslandTileSize - 1) / kIslandTileSize;
	if (tx != tiles_x_ || ty != tiles_y_ || !uset_.is_valid()) rebuild(rd, atlas, tx, ty);
	if (!uset_.is_valid()) return false;

	// The push constant IS ve::CameraParams, with the cull grid in the three trailing ints.
	// Copying rather than re-deriving is the point: the raymarcher gets the same bytes.
	ve::CameraParams pc = cam;
	pc.dims[3] = island_count;
	pc.region_origin[3] = tx;
	pc.atlas_bricks[3] = ty;
	PackedByteArray b;
	b.resize(sizeof(ve::CameraParams));
	std::memcpy(b.ptrw(), &pc, sizeof(ve::CameraParams));

	// Its own compute list. Godot's RenderingDevice ends a compute list with a full barrier
	// unless told otherwise, so the raymarch list that follows sees the finished mask.
	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, b, b.size());
	rd->compute_list_dispatch(list, (tx + 7) / 8, (ty + 7) / 8, 1);
	rd->compute_list_end();
	return true;
}
```

- [ ] **Step 5: Run it before the raymarch**

`extension/src/voxel_world.h` — `IslandCullPass *island_cull_ = nullptr;` with an accessor, created after `islands_` in `ensure_initialized` and deleted before it in `teardown_gpu`.

`extension/src/raymarch_compositor.cpp` — between the streamer and the raymarch:

```cpp
	const int islands = world->island_slot_count();
	IslandCullPass *cull = world->island_cull();
	RID mask;
	if (cull && islands > 0 && cull->render(rd, *world->islands(), cp, rw, rh, islands)) {
		mask = cull->mask_buffer();
		cp.region_origin[3] = cull->tiles_x();
		cp.atlas_bricks[3] = cull->tiles_y();
	}
	cp.dims[3] = islands;
	if (!rmp->render(rd, *atlas, world->islands(), mask, cp, rw, rh, edit_state)) return;
```

(`rw`/`rh` must be computed before this block; move the two lines that derive them above it.)

- [ ] **Step 6: Add the mask hook**

`extension/src/voxel_world.h`:

```cpp
	PackedInt32Array debug_island_tile_mask(Vector3 origin, Vector3 dir, float tan_x,
			float tan_y, int width, int height);
```

`extension/src/voxel_world.cpp`:

```cpp
PackedInt32Array VoxelWorld::debug_island_tile_mask(Vector3 origin, Vector3 dir, float tan_x,
		float tan_y, int width, int height) {
	PackedInt32Array out;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!device || !islands_ || !island_cull_) return out;
	ve::CameraParams cam = ve::CameraParams::looking_at(origin.x, origin.y, origin.z,
			dir.x, dir.y, dir.z, 0, 1, 0);
	// looking_at leaves the tangents at 0 (the 1x1 probes need no frustum); a cull test does.
	cam.params[0] = tan_x;
	cam.params[1] = tan_y;
	if (!island_cull_->render(device, *islands_, cam, width, height,
				std::max(island_slots_, 1)))
		return out;
	device->submit();
	device->sync();
	const int n = island_cull_->tiles_x() * island_cull_->tiles_y();
	const PackedByteArray b = device->buffer_get_data(island_cull_->mask_buffer(), 0,
			static_cast<uint32_t>(n) * 4);
	if (b.size() < static_cast<int64_t>(n) * 4) return out;
	out.resize(n);
	std::memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(n) * 4);
	return out;
}
```

- [ ] **Step 7: Run the tests**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: green, including the four new `test_island_render.gd` cases.

- [ ] **Step 8: Commit**

```bash
git add shaders/island_cull.comp.glsl extension/src/render/island_cull_pass.h \
        extension/src/render/island_cull_pass.cpp extension/src/raymarch_compositor.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_island_render.gd
git commit -m "feat: tiled island culling for the raymarcher"
```

---
### Task 12: `physics/island_body` — a component becomes a rigid body

Spec §5 step 3: "Spawn **Jolt rigid body**: collision = greedy box-merged compound from 0.8 m occupancy (≤256 boxes), mass/inertia from solid volume", plus spec §6's "Small debris = single-box bodies + cheap DC render meshes", "Explosions apply radial impulses", "Jolt sleep events drive the re-merge hook" and "CCD on fast debris".

**Files:**
- Modify: `extension/src/mesh/box_merge.h`, `extension/src/mesh/box_merge.cpp`
- Create: `extension/src/physics/island_body.h`, `extension/src/physics/island_body.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Test: `extension/tests/test_box_merge.cpp` (added cases), `tests/test_island_body.gd`

**Interfaces:**
- Consumes: `ve::CellBox`, `ve::greedy_box_merge` (Task 5); `ve::VolumeData` (Task 6); `ve::dual_contour`, `ve::DcGrid` (M3); `IslandAtlas` (Task 10).
- Produces: `ve::box_compound_mass`; `godot::IslandSpawn`, `godot::IslandBody` (`spawn`, `despawn`, `live`, `body`, `info`, `transform`, `tick`, `asleep_seconds`, `sync_render`); `VoxelWorld::debug_spawn_test_body`, `debug_test_body_stats`, `debug_despawn_test_body`. Task 13 owns a pool of these.

- [ ] **Step 1: Write the failing native test cases**

Append to `extension/tests/test_box_merge.cpp`:

```cpp
TEST_CASE("a box compound's mass properties are its boxes' volume-weighted centre") {
	// Two 1-cell boxes two cells apart on x: the centre of mass is exactly between them.
	const std::vector<CellBox> pair{CellBox{{0, 0, 0}, {0, 0, 0}}, CellBox{{2, 0, 0}, {2, 0, 0}}};
	float com[3] = {0, 0, 0};
	float vol = 0.0f;
	box_compound_mass(pair.data(), 2, com, &vol);
	const float c = kOccupancyCellSize;
	CHECK(com[0] == doctest::Approx(1.5f * c));  // centres at 0.5c and 2.5c
	CHECK(com[1] == doctest::Approx(0.5f * c));
	CHECK(com[2] == doctest::Approx(0.5f * c));
	CHECK(vol == doctest::Approx(2.0f * c * c * c));

	// A big box and a small one: the big one dominates.
	const std::vector<CellBox> uneven{CellBox{{0, 0, 0}, {3, 3, 3}}, CellBox{{10, 0, 0}, {10, 0, 0}}};
	box_compound_mass(uneven.data(), 2, com, &vol);
	CHECK(vol == doctest::Approx(65.0f * c * c * c));
	CHECK(com[0] < 1.5f * c); // 64 cells at x = 2c against 1 cell at x = 10.5c

	// Degenerate input is answered, not crashed on (spec §8's fail-soft).
	box_compound_mass(nullptr, 0, com, &vol);
	CHECK(vol == doctest::Approx(0.0f));
}
```

- [ ] **Step 2: Run the native tests to verify they fail**

Run: `cd extension && scons test`
Expected: FAIL — `'box_compound_mass' was not declared in this scope`

- [ ] **Step 3: Add the pure mass helper**

`extension/src/mesh/box_merge.h`:

```cpp
// Volume-weighted centre of the boxes and their total volume in m^3. Spec §5's "mass/inertia
// from solid volume" -- Jolt derives the inertia tensor from the compound's own shapes, so
// only the mass and the origin the shapes hang off come from here.
//
// The BOX volume, not the solid volume inside it: a body's origin has to be the centre of
// the shapes Jolt is given, or the compound tumbles about a point outside itself. Mass uses
// the true solid count instead (see IslandSpawn::solid_voxels).
void box_compound_mass(const CellBox *boxes, int count, float *out_com, float *out_volume_m3);
```

`extension/src/mesh/box_merge.cpp`:

```cpp
void box_compound_mass(const CellBox *boxes, int count, float *out_com, float *out_volume_m3) {
	double acc[3] = {0.0, 0.0, 0.0};
	double total = 0.0;
	for (int i = 0; i < count; i++) {
		float lo[3], hi[3];
		boxes[i].world_aabb(lo, hi);
		const double v = static_cast<double>(hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
		total += v;
		for (int a = 0; a < 3; a++) acc[a] += v * 0.5 * (lo[a] + hi[a]);
	}
	for (int a = 0; a < 3; a++)
		out_com[a] = total > 0.0 ? static_cast<float>(acc[a] / total) : 0.0f;
	*out_volume_m3 = static_cast<float>(total);
}
```

- [ ] **Step 4: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

- [ ] **Step 5: Write the failing engine test**

Create `tests/test_island_body.gd`:

```gdscript
extends GdUnitTestSuite

# Spec section 5 step 3 and section 6's dynamic-body rules: a component becomes a Jolt rigid
# body carrying a box compound, with mass from its solid volume, an explosion's impulse
# already applied, and a sleep clock the re-merge hook (Task 13) reads.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

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
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func settle_colliders(w: VoxelWorld, center: Vector3, frames := 400) -> void:
	var quiet := 0
	for i in range(frames):
		quiet = quiet + 1 if w.debug_physics_frame(center) == 0 else 0
		if quiet >= 4:
			return

func test_a_spawned_body_has_mass_and_falls(timeout := 90000) -> void:
	var w := make_world()
	var centre := Vector3(20.0, 56.0, 20.0)
	settle_colliders(w, centre)
	# A lump of rock lifted 20 m into the air, with no kick: it should just drop.
	var d: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 20.0, 0.0), Vector3.ZERO, false)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	assert_float(d["mass"]).is_greater(0.0)
	assert_int(d["shapes"]).is_greater(0)
	assert_int(d["shapes"]).is_less_equal(64)
	var start: Vector3 = d["origin"]

	for i in range(30):
		await get_tree().physics_frame
	var s: Dictionary = w.debug_test_body_stats(d["index"])
	assert_bool(s["live"]).is_true()
	assert_float((s["origin"] as Vector3).y).override_failure_message(
		"the body did not fall").is_less(start.y - 0.5)

func test_a_body_lands_on_the_streamed_collider_and_sleeps(timeout := 120000) -> void:
	var w := make_world()
	var centre := Vector3(20.0, 56.0, 20.0)
	settle_colliders(w, centre)
	var d: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 8.0, 0.0), Vector3.ZERO, false)
	assert_bool(d.get("ok", false)).is_true()
	for i in range(400):
		await get_tree().physics_frame
		w.debug_tick_test_bodies(1.0 / 60.0)
		if w.debug_test_body_stats(d["index"])["asleep_s"] > 0.5:
			break
	var s: Dictionary = w.debug_test_body_stats(d["index"])
	assert_float(s["asleep_s"]).override_failure_message(
		"the body never came to rest on the terrain").is_greater(0.5)
	# It stopped ON the ground, not below it.
	var oracle: Dictionary = w.debug_raycast(Vector3(centre.x, 90.0, centre.z), Vector3(0, -1, 0))
	assert_float((s["origin"] as Vector3).y).is_greater((oracle["pos"] as Vector3).y - 2.0)

func test_an_impulse_throws_the_body_sideways(timeout := 90000) -> void:
	var w := make_world()
	settle_colliders(w, Vector3(20.0, 56.0, 20.0))
	var d: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 20.0, 0.0), Vector3(400.0, 0.0, 0.0), false)
	assert_bool(d.get("ok", false)).is_true()
	var start: Vector3 = d["origin"]
	for i in range(20):
		await get_tree().physics_frame
	var s: Dictionary = w.debug_test_body_stats(d["index"])
	assert_float((s["origin"] as Vector3).x).override_failure_message(
		"the explosion impulse did not reach the body").is_greater(start.x + 0.5)

func test_debris_gets_a_render_instance_and_an_island_does_not(timeout := 90000) -> void:
	var w := make_world()
	settle_colliders(w, Vector3(20.0, 56.0, 20.0))
	var rock: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 20.0, 0.0), Vector3.ZERO, false)
	var crumb: Dictionary = w.debug_spawn_test_body(Vector3i(28, 62, 28), Vector3i(28, 62, 28),
		Vector3(0.0, 20.0, 0.0), Vector3.ZERO, true)
	assert_bool(rock.get("ok", false)).is_true()
	assert_bool(crumb.get("ok", false)).is_true()
	assert_bool(rock["has_render_mesh"]).is_false() # raymarched from the island atlas
	assert_bool(crumb["has_render_mesh"]).is_true() # dual-contoured, drawn by RenderingServer
	assert_int(crumb["render_tris"]).is_greater(0)

func test_despawn_removes_the_body(timeout := 90000) -> void:
	var w := make_world()
	settle_colliders(w, Vector3(20.0, 56.0, 20.0))
	var d: Dictionary = w.debug_spawn_test_body(Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 20.0, 0.0), Vector3.ZERO, false)
	assert_bool(d.get("ok", false)).is_true()
	w.debug_despawn_test_body(d["index"])
	assert_bool(w.debug_test_body_stats(d["index"])["live"]).is_false()
```

- [ ] **Step 6: Run the engine test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_island_body.gd`
Expected: FAIL — `debug_spawn_test_body` does not exist.

- [ ] **Step 7: Write `physics/island_body.h`**

```cpp
#pragma once
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <vector>
#include "generator/volume_set.h"
#include "mesh/box_merge.h"

namespace godot {

// Everything a component needs to become a body. Filled by IslandManager (Task 13) from the
// component, its extraction and the edit that freed it.
struct IslandSpawn {
	int volume_slot = -1; // ve::VolumeSet slot holding the CPU copy (mass, resample, mesh)
	int atlas_slot = -1;  // IslandAtlas slot; -1 for debris, which is not raymarched
	std::vector<ve::CellBox> boxes;
	float lattice_origin[3] = {0, 0, 0}; // world, at birth
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	int solid_voxels = 0;
	float impulse[3] = {0, 0, 0}; // spec §6's "explosions apply radial impulses"
	bool debris = false;
};

// One dynamic piece of terrain: a Jolt rigid body carrying a box compound, its sleep clock,
// and (for debris) the mesh and RenderingServer instance that draw it.
//
// Server-direct, like ColliderStreamer and for the same reasons; main thread only, like
// PhysicsServer3D itself.
class IslandBody {
public:
	~IslandBody();

	// `scenario` is the World3D scenario debris is drawn in; ignored for islands.
	// The body's ORIGIN is the compound's centre, so it tumbles about itself, and
	// `local_lattice_origin()` is where the volume sits relative to that.
	bool spawn(RID space, RID scenario, const IslandSpawn &info, const ve::VolumeData *volume);
	void despawn();

	bool live() const { return body_.is_valid(); }
	RID body() const { return body_; }
	const IslandSpawn &info() const { return info_; }
	float mass() const { return mass_; }
	int shape_count() const { return static_cast<int>(shapes_.size()); }
	bool has_render_mesh() const { return mesh_.is_valid(); }
	int render_triangles() const { return render_tris_; }
	const float *local_lattice_origin() const { return local_origin_; }

	Transform3D transform() const;
	// Polls PhysicsServer3D's BODY_STATE_SLEEPING and accumulates. Spec §6 says "Jolt sleep
	// events drive the re-merge hook"; this is that bit, read once a frame instead of
	// signalled, because the manager already runs every frame.
	void tick(float dt);
	float asleep_seconds() const { return asleep_; }
	// Pushes the body transform to the debris instance. No-op for a raymarched island: its
	// descriptor carries the transform instead.
	void sync_render();

private:
	void build_render_mesh(RID scenario, const ve::VolumeData &volume);

	IslandSpawn info_;
	RID body_;
	std::vector<RID> shapes_;
	Ref<ArrayMesh> mesh_;
	RID instance_;
	float mass_ = 0.0f;
	float asleep_ = 0.0f;
	int render_tris_ = 0;
	float local_origin_[3] = {0, 0, 0};
};

} // namespace godot
```

- [ ] **Step 8: Write `physics/island_body.cpp`**

```cpp
#include "physics/island_body.h"
#include "mesh/dual_contour.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>

using namespace godot;

namespace {

// Rock is ~2600 kg/m^3. The demo uses a fifth of that: at true density a 3 m slab weighs
// twenty tonnes, lands like a dropped anvil and shrugs off the explosion impulse that freed
// it. Lighter reads as rubble. One constant, tuned by eye, spec §5's "one-constant tunables".
constexpr float kIslandDensity = 500.0f;

} // namespace

IslandBody::~IslandBody() {
	despawn();
}

bool IslandBody::spawn(RID space, RID scenario, const IslandSpawn &info,
		const ve::VolumeData *volume) {
	despawn();
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || info.boxes.empty()) return false;
	info_ = info;

	float com[3] = {0, 0, 0};
	float box_volume = 0.0f;
	ve::box_compound_mass(info.boxes.data(), static_cast<int>(info.boxes.size()), com,
			&box_volume);
	// Mass from the SOLID volume, not the boxes': a component's cells are only partly full
	// where the surface crosses them, and a hollow shell should not weigh like a brick.
	const float solid_m3 = static_cast<float>(info.solid_voxels) * info.voxel * info.voxel *
			info.voxel;
	mass_ = std::max(solid_m3 * kIslandDensity, 1.0f);
	for (int a = 0; a < 3; a++) local_origin_[a] = info.lattice_origin[a] - com[a];

	body_ = ps->body_create();
	ps->body_set_mode(body_, PhysicsServer3D::BODY_MODE_RIGID);
	for (const ve::CellBox &b : info.boxes) {
		float lo[3], hi[3];
		b.world_aabb(lo, hi);
		RID shape = ps->box_shape_create();
		ps->shape_set_data(shape, Vector3(0.5f * (hi[0] - lo[0]), 0.5f * (hi[1] - lo[1]),
									 0.5f * (hi[2] - lo[2])));
		Transform3D xf;
		xf.origin = Vector3(0.5f * (lo[0] + hi[0]) - com[0], 0.5f * (lo[1] + hi[1]) - com[1],
				0.5f * (lo[2] + hi[2]) - com[2]);
		ps->body_add_shape(body_, shape, xf);
		shapes_.push_back(shape);
	}
	ps->body_set_param(body_, PhysicsServer3D::BODY_PARAM_MASS, mass_);
	// Inertia is left at zero, which is Godot's "derive it from the shapes" sentinel: the
	// compound already has the right distribution and Jolt computes a better tensor from it
	// than any closed form over the cell set would.
	Transform3D at;
	at.origin = Vector3(com[0], com[1], com[2]);
	ps->body_set_state(body_, PhysicsServer3D::BODY_STATE_TRANSFORM, at);
	// Spec §6: "CCD on fast debris". A crumb is small and gets thrown hard; a slab is neither.
	ps->body_set_enable_continuous_collision_detection(body_, info.debris);
	ps->body_set_space(body_, space);

	const Vector3 imp(info.impulse[0], info.impulse[1], info.impulse[2]);
	if (imp.length_squared() > 0.0f) ps->body_apply_impulse(body_, imp);

	if (info.debris && volume && !volume->empty()) build_render_mesh(scenario, *volume);
	return true;
}

void IslandBody::despawn() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (ps) {
		if (body_.is_valid()) {
			ps->body_set_space(body_, RID());
			ps->free_rid(body_);
		}
		for (RID s : shapes_)
			if (s.is_valid()) ps->free_rid(s);
	}
	body_ = RID();
	shapes_.clear();
	if (instance_.is_valid()) {
		RenderingServer::get_singleton()->free_rid(instance_);
		instance_ = RID();
	}
	mesh_.unref();
	render_tris_ = 0;
	asleep_ = 0.0f;
	mass_ = 0.0f;
}

Transform3D IslandBody::transform() const {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || !body_.is_valid()) return Transform3D();
	return ps->body_get_state(body_, PhysicsServer3D::BODY_STATE_TRANSFORM);
}

void IslandBody::tick(float dt) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || !body_.is_valid()) return;
	const bool sleeping = ps->body_get_state(body_, PhysicsServer3D::BODY_STATE_SLEEPING);
	asleep_ = sleeping ? asleep_ + dt : 0.0f;
}

void IslandBody::sync_render() {
	if (!instance_.is_valid()) return;
	RenderingServer::get_singleton()->instance_set_transform(instance_, transform());
}

void IslandBody::build_render_mesh(RID scenario, const ve::VolumeData &volume) {
	// ve::dual_contour's convention: lattice index i holds the sample at local coordinate
	// i - 1, and a vertex lands at g.origin + (i - 1 + frac) * cell_size. Offsetting the
	// grid origin by one voxel therefore puts lattice index i at the volume's own sample i,
	// and subtracting the body's centre puts the whole mesh in BODY-LOCAL space.
	ve::DcGrid g;
	g.lattice = volume.dim;
	g.cell_size = info_.voxel;
	for (int a = 0; a < 3; a++) g.origin[a] = local_origin_[a] + info_.voxel;

	ve::MeshBuffer mb;
	ve::dual_contour(volume.sdf.data(), g, &mb);
	if (mb.triangle_count() == 0) return;

	PackedVector3Array verts;
	verts.resize(mb.vertex_count());
	Vector3 *vw = verts.ptrw();
	for (int i = 0; i < mb.vertex_count(); i++)
		vw[i] = Vector3(mb.positions[i * 3 + 0], mb.positions[i * 3 + 1],
				mb.positions[i * 3 + 2]);

	PackedInt32Array idx;
	idx.resize(static_cast<int64_t>(mb.indices.size()));
	int32_t *iw = idx.ptrw();
	for (size_t i = 0; i < mb.indices.size(); i++)
		iw[i] = static_cast<int32_t>(mb.indices[i]);

	// Area-weighted vertex normals from the faces. Without them the mesh renders unlit-black,
	// and the SDF gradient is not available on this side of the readback.
	PackedVector3Array normals;
	normals.resize(mb.vertex_count());
	Vector3 *nw = normals.ptrw();
	for (int i = 0; i < mb.vertex_count(); i++) nw[i] = Vector3();
	for (size_t t = 0; t + 2 < mb.indices.size(); t += 3) {
		const Vector3 &a = vw[mb.indices[t + 0]];
		const Vector3 &b = vw[mb.indices[t + 1]];
		const Vector3 &c = vw[mb.indices[t + 2]];
		const Vector3 fn = (b - a).cross(c - a); // unnormalised: area weighting for free
		nw[mb.indices[t + 0]] += fn;
		nw[mb.indices[t + 1]] += fn;
		nw[mb.indices[t + 2]] += fn;
	}
	for (int i = 0; i < mb.vertex_count(); i++)
		nw[i] = nw[i].length_squared() > 0.0f ? nw[i].normalized() : Vector3(0, 1, 0);

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_INDEX] = idx;
	mesh_.instantiate();
	mesh_->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	// A plain material for M4; M6's cel pass replaces it with the shared ShaderMaterial so
	// debris is shaded by the same GLSL as everything else (spec §7).
	Ref<StandardMaterial3D> mat;
	mat.instantiate();
	mat->set_albedo(Color(0.45f, 0.42f, 0.40f));
	mesh_->surface_set_material(0, mat);
	render_tris_ = mb.triangle_count();

	RenderingServer *rs = RenderingServer::get_singleton();
	instance_ = rs->instance_create2(mesh_->get_rid(), scenario);
	rs->instance_set_transform(instance_, transform());
}
```

> **Winding.** `ve::dual_contour` emits counter-clockwise as seen from the air side, which is Godot's front face for rendering — the *opposite* of the swap M3 errata 1 requires for Jolt collision. Debris meshes are render-only, so no swap here.

- [ ] **Step 9: Add the test hooks**

`extension/src/voxel_world.h` — `#include "physics/island_body.h"`, and:

```cpp
	// A hand-driven body pool for tests. Task 13's IslandManager owns the real one and
	// takes these over; until then this is what proves the body path works.
	std::vector<IslandBody *> test_bodies_;

public:
	Dictionary debug_spawn_test_body(Vector3i lo_cell, Vector3i hi_cell, Vector3 offset,
			Vector3 impulse, bool debris);
	Dictionary debug_test_body_stats(int index);
	void debug_tick_test_bodies(float dt);
	void debug_despawn_test_body(int index);
```

`extension/src/voxel_world.cpp` — `debug_spawn_test_body` shares the extraction path of `debug_place_test_island_rotated` (factor the "cells → boxes → extract → CPU volume" half into a private helper `bool extract_component(const std::vector<ve::IVec3> &cells, IslandExtractJob *job, std::vector<ve::CellBox> *boxes, ve::VolumeData *out)` and call it from both), then:

```cpp
Dictionary VoxelWorld::debug_spawn_test_body(Vector3i lo_cell, Vector3i hi_cell, Vector3 offset,
		Vector3 impulse, bool debris) {
	Dictionary d;
	d["ok"] = false;
	ensure_initialized();
	ensure_physics_initialized();
	std::vector<ve::IVec3> cells;
	for (int z = lo_cell.z; z <= hi_cell.z; z++)
		for (int y = lo_cell.y; y <= hi_cell.y; y++)
			for (int x = lo_cell.x; x <= hi_cell.x; x++) cells.push_back({x, y, z});
	IslandExtractJob job;
	std::vector<ve::CellBox> boxes;
	ve::VolumeData volume;
	if (!extract_component(cells, &job, &boxes, &volume)) return d;

	const int slot = volumes_.allocate();
	if (slot < 0) return d;
	volumes_.store(slot, volume);

	IslandSpawn info;
	info.volume_slot = slot;
	info.boxes = boxes;
	info.voxel = job.voxel;
	info.dim = job.dim;
	info.solid_voxels = volume.solid_voxels;
	info.debris = debris;
	// The offset moves the WHOLE piece: its boxes and its lattice alike, so the collision
	// and the volume stay registered with each other.
	for (int a = 0; a < 3; a++) info.lattice_origin[a] = job.origin[a];
	const ve::IVec3 shift{static_cast<int>(std::lround(offset.x / ve::kOccupancyCellSize)),
			static_cast<int>(std::lround(offset.y / ve::kOccupancyCellSize)),
			static_cast<int>(std::lround(offset.z / ve::kOccupancyCellSize))};
	for (ve::CellBox &b : info.boxes) {
		b.lo = {b.lo.x + shift.x, b.lo.y + shift.y, b.lo.z + shift.z};
		b.hi = {b.hi.x + shift.x, b.hi.y + shift.y, b.hi.z + shift.z};
	}
	info.lattice_origin[0] += shift.x * ve::kOccupancyCellSize;
	info.lattice_origin[1] += shift.y * ve::kOccupancyCellSize;
	info.lattice_origin[2] += shift.z * ve::kOccupancyCellSize;
	info.impulse[0] = impulse.x;
	info.impulse[1] = impulse.y;
	info.impulse[2] = impulse.z;

	IslandBody *b = new IslandBody();
	const Ref<World3D> w3 = get_world_3d();
	if (!b->spawn(w3.is_valid() ? w3->get_space() : RID(),
				w3.is_valid() ? w3->get_scenario() : RID(), info, &volume)) {
		delete b;
		volumes_.release(slot);
		return d;
	}
	test_bodies_.push_back(b);
	d["ok"] = true;
	d["index"] = static_cast<int>(test_bodies_.size()) - 1;
	d["mass"] = b->mass();
	d["shapes"] = b->shape_count();
	d["origin"] = b->transform().origin;
	d["has_render_mesh"] = b->has_render_mesh();
	d["render_tris"] = b->render_triangles();
	return d;
}

Dictionary VoxelWorld::debug_test_body_stats(int index) {
	Dictionary d;
	d["live"] = false;
	if (index < 0 || index >= static_cast<int>(test_bodies_.size()) || !test_bodies_[index])
		return d;
	IslandBody *b = test_bodies_[index];
	d["live"] = b->live();
	d["origin"] = b->transform().origin;
	d["asleep_s"] = b->asleep_seconds();
	d["mass"] = b->mass();
	return d;
}

void VoxelWorld::debug_tick_test_bodies(float dt) {
	for (IslandBody *b : test_bodies_)
		if (b) {
			b->tick(dt);
			b->sync_render();
		}
}

void VoxelWorld::debug_despawn_test_body(int index) {
	if (index < 0 || index >= static_cast<int>(test_bodies_.size()) || !test_bodies_[index])
		return;
	test_bodies_[index]->despawn();
}
```

with the pool freed in `teardown_physics()`:

```cpp
	for (IslandBody *b : test_bodies_) delete b;
	test_bodies_.clear();
```

- [ ] **Step 10: Run the tests**

Run: `cd extension && scons test`
Expected: PASS.

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_island_body.gd`
Expected: PASS — five cases.

- [ ] **Step 11: Commit**

```bash
git add extension/src/mesh/box_merge.h extension/src/mesh/box_merge.cpp \
        extension/src/physics/island_body.h extension/src/physics/island_body.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp \
        extension/tests/test_box_merge.cpp tests/test_island_body.gd
git commit -m "feat: islands and debris as Jolt rigid bodies"
```

---
### Task 13: `physics/island_manager` — the lifecycle, end to end

Spec §5's whole *Island lifecycle* section, its *Guardrails*, and the sentence that ties them together: "Runs once per frame after all that frame's edits — simultaneous blasts can't race."

**Files:**
- Create: `extension/src/physics/island_manager.h`, `extension/src/physics/island_manager.cpp`
- Modify: `extension/src/render/island_extract_pass.h`, `extension/src/render/mesh_service.cpp` (a second job kind)
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/raymarch_compositor.cpp`
- Test: `tests/test_connectivity.gd`

**Interfaces:**
- Consumes: everything from Tasks 1–12.
- Produces: `godot::IslandManager` (`initialize`, `teardown`, `run_frame`, `note_edit`, `stats`, `last_ms`, `slot_high_water`, `set_merge_sleep_seconds`); `godot::IslandUpload`; `VoxelWorld::debug_island_frame`, `debug_island_stats`, `debug_set_merge_sleep_seconds`. Task 14 puts its numbers on the HUD.

- [ ] **Step 1: Write the failing test**

Create `tests/test_connectivity.gd`:

```gdscript
extends GdUnitTestSuite

# Spec section 5, end to end. Blow the middle out of a pillar and the top must:
#   1. stop being part of the terrain (the carve),
#   2. become a rigid body that falls (the spawn),
#   3. come back as terrain where it lands (the re-merge).
#
# Every assertion is against the FIELD, through ve::raycast, because that is what the
# renderer, the collision mesher and the LoD bakery all read: if the field says the pillar
# top is gone, every consumer agrees it is gone.

const CENTER := Vector3(20.0, 56.0, 20.0)
# A pillar built above the terrain with sphere-adds, so the test does not depend on where
# the analytic hills happen to put a cliff.
const PILLAR_X := 20.4
const PILLAR_Z := 20.4
const PILLAR_BASE := 62.0

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(48, 24, 48)
	w.max_region_slots = 64
	w.physics_radius_m = 30.0
	w.max_collider_chunks = 128
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

func tool_of(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

# One frame of everything: streaming (which fills the occupancy grid), collider maintenance
# and the island manager, in the order VoxelWorld::_process runs them.
func step(w: VoxelWorld, frames: int, center: Vector3 = CENTER) -> void:
	for i in range(frames):
		w.debug_stream_frame(center)
		w.debug_physics_frame(center)
		w.debug_island_frame(1.0 / 60.0, center)

func solid_at(w: VoxelWorld, p: Vector3) -> bool:
	# A downward ray from just above the point: it hits at p if there is matter there.
	var hit: Dictionary = w.debug_raycast(p + Vector3(0, 0.6, 0), Vector3(0, -1, 0))
	return hit["hit"] and absf((hit["pos"] as Vector3).y - p.y) < 0.5

func build_pillar(w: VoxelWorld, t: VoxelEditTool) -> void:
	# Five overlapping 1.2 m balls stacked into a 6 m column standing on the terrain.
	for i in range(5):
		t.apply_sphere_add(Vector3(PILLAR_X, PILLAR_BASE + 1.0 * i, PILLAR_Z), 1.2, 4)
	step(w, 90)

func test_the_grid_and_the_flood_find_a_severed_pillar_top(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	var top := Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z)
	assert_bool(solid_at(w, top)).override_failure_message(
		"the pillar was never built").is_true()

	# Cut the pillar in half.
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 240)

	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_spawned"]).override_failure_message(
		"nothing came loose: %s" % st).is_greater(0)
	# The top is no longer part of the static field: it is a body now.
	assert_bool(solid_at(w, top)).override_failure_message(
		"the severed top is still in the terrain (the carve did not happen)").is_false()
	# ...and the stump below the cut is untouched.
	assert_bool(solid_at(w, Vector3(PILLAR_X, PILLAR_BASE, PILLAR_Z))).is_true()

func test_the_island_body_falls_and_the_bodies_are_capped(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["live_bodies"]).is_greater(0)
	var y0: float = st["lowest_body_y"]
	for i in range(60):
		await get_tree().physics_frame
		w.debug_island_frame(1.0 / 60.0, CENTER)
	assert_float(w.debug_island_stats()["lowest_body_y"]).override_failure_message(
		"the island did not fall").is_less(y0 - 0.2)
	# Spec section 5's guardrails hold whatever happens.
	assert_int(st["live_bodies"]).is_less_equal(64)
	assert_int(st["live_islands"]).is_less_equal(32)

func test_a_rested_island_merges_back_into_the_terrain(timeout := 180000) -> void:
	var w := make_world()
	w.debug_set_merge_sleep_seconds(0.2) # the demo waits 2 s; a test should not
	var t := tool_of(w)
	build_pillar(w, t)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	step(w, 120)
	assert_int(w.debug_island_stats()["islands_spawned"]).is_greater(0)

	for i in range(600):
		await get_tree().physics_frame
		w.debug_stream_frame(CENTER)
		w.debug_island_frame(1.0 / 60.0, CENTER)
		if w.debug_island_stats()["islands_merged"] > 0:
			break
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["islands_merged"]).override_failure_message(
		"the island never merged back: %s" % st).is_greater(0)
	assert_int(st["live_bodies"]).is_equal(0)
	# Spec section 5: "Rubble permanently accumulates as terrain." The rock is somewhere on
	# the ground under where it fell, and the FIELD knows about it.
	var down: Dictionary = w.debug_raycast(Vector3(PILLAR_X, 90.0, PILLAR_Z), Vector3(0, -1, 0))
	assert_bool(down["hit"]).is_true()
	assert_float((down["pos"] as Vector3).y).override_failure_message(
		"the merged rubble is not standing on the ground").is_greater(
		(st["ground_y"] as float) - 0.1)

func test_an_anchored_overhang_is_left_alone(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	# Undercut the pillar without severing it: a 0.5 m bite out of one side.
	t.apply_sphere_subtract(Vector3(PILLAR_X + 1.0, PILLAR_BASE + 2.0, PILLAR_Z), 0.7)
	step(w, 180)
	assert_int(w.debug_island_stats()["islands_spawned"]).override_failure_message(
		"a still-attached pillar was declared an island").is_equal(0)
	assert_bool(solid_at(w, Vector3(PILLAR_X, PILLAR_BASE + 4.0, PILLAR_Z))).is_true()

func test_connectivity_runs_once_per_frame_however_many_edits_land(timeout := 120000) -> void:
	var w := make_world()
	var t := tool_of(w)
	build_pillar(w, t)
	# Three blasts in one frame (spec section 5: "simultaneous blasts can't race").
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 2.0, PILLAR_Z), 1.6)
	t.apply_sphere_subtract(Vector3(PILLAR_X + 0.4, PILLAR_BASE + 2.2, PILLAR_Z), 1.2)
	t.apply_sphere_subtract(Vector3(PILLAR_X - 0.4, PILLAR_BASE + 2.2, PILLAR_Z), 1.2)
	step(w, 240)
	var st: Dictionary = w.debug_island_stats()
	assert_int(st["connectivity_runs"]).is_greater(0)
	# One window covered all three, so the pillar top came off exactly once.
	assert_int(st["islands_spawned"]).is_between(1, 3)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_connectivity.gd`
Expected: FAIL — `debug_island_frame` does not exist.

- [ ] **Step 3: Give the worker a second job kind**

`extension/src/render/island_extract_pass.h` — extend the job so a re-merge's resample rides the same queue:

```cpp
// A component's extraction, or a sleeping island's rest-pose resample. One queue, because
// the second is pure CPU (ve::resample_volume) and belongs on the same off-frame thread the
// first already owns -- 262 144 trilinear samples is ~5 ms, which is a hitch on the main
// thread and nothing at all on the worker.
enum IslandJobKind {
	kExtractField = 0,   // evaluate G + ops, masked by `boxes`
	kResampleVolume = 1, // transform `source` by (basis, rest_origin) into a world-aligned volume
};

struct IslandExtractJob {
	IslandJobKind kind = kExtractField;
	int id = -1;
	float origin[3] = {0.0f, 0.0f, 0.0f};
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	std::vector<ve::EditOp> ops;
	std::vector<ve::CellBox> boxes;

	// kResampleVolume only.
	ve::VolumeData source;
	ve::EditOp source_op{};
	float basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // ROW major (ve::resample_volume's form)
	float rest_origin[3] = {0.0f, 0.0f, 0.0f};
	int out_slot = -1;
};

struct IslandExtractResult {
	int id = -1;
	IslandJobKind kind = kExtractField;
	ve::VolumeData data;
	ve::EditOp op{}; // kResampleVolume: the kOpVolumeAdd the manager appends
	bool failed = false;
};
```

`extension/src/render/mesh_service.cpp` — the worker branches on the kind:

```cpp
			if (job.kind == kResampleVolume) {
				IslandExtractResult r;
				r.id = job.id;
				r.kind = job.kind;
				r.failed = !ve::resample_volume(job.source, job.source_op, job.basis,
						job.rest_origin, job.out_slot, job.dim, &r.data, &r.op);
				results.push_back(std::move(r));
			} else if (extract_) {
				IslandExtractResult r;
				extract_->extract(job, &r);
				r.kind = job.kind;
				results.push_back(std::move(r));
			}
```

(`kResampleVolume` works even when `extract_` failed to build, which is deliberate: a world whose GPU extraction is unavailable can still finish the islands it already has.)

- [ ] **Step 4: Write `physics/island_manager.h`**

```cpp
#pragma once
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <deque>
#include <vector>
#include "connectivity/components.h"
#include "connectivity/contact_refine.h"
#include "connectivity/flood_fill.h"
#include "physics/island_body.h"
#include "render/island_atlas.h"

namespace godot {

class VoxelWorld;

// Spec §5, orchestrated. One pass per frame, in this order:
//
//   1. tick the bodies (sleep clocks, debris transforms) and publish their descriptors
//   2. collect finished extractions and resamples -> carve + spawn, or paste + despawn
//   3. if an edit has loosened something and the occupancy grid has caught up, run
//      connectivity ONCE (spec §5: "simultaneous blasts can't race") and submit extractions
//   4. re-merge whatever has slept long enough
//
// Main thread only. Everything it hands to the render thread goes through VoxelWorld's
// mutex-guarded island queues; everything it hands to the mesher goes through MeshService.
class IslandManager {
public:
	~IslandManager();

	void initialize(VoxelWorld *world);
	void teardown();

	int run_frame(float dt, const Vector3 &center); // actions taken
	// Called from VoxelWorld::append_edit for every SDF-changing op, including the manager's
	// own carves: removing an island can unsupport the next piece up, and that cascade is
	// the behaviour spec §5 describes, not a bug.
	void note_edit(const ve::EditOp &op, int64_t seq);

	int slot_high_water() const { return slot_high_water_; }
	float last_ms() const { return last_ms_; }
	void set_merge_sleep_seconds(float v) { merge_sleep_s_ = v; }
	// Not const: the ground probe takes the edit lock.
	Dictionary stats();

private:
	struct PendingWindow {
		ve::IVec3 lo{}, hi{}; // inclusive cell AABB the edit could have loosened
		int64_t seq = 0;
		int waited = 0;
		float impulse_from[3] = {0, 0, 0}; // the edit's centre, for the radial kick
		float impulse_scale = 0.0f;
	};
	struct InFlight {
		int id = -1;
		std::vector<ve::CellBox> boxes;
		int volume_slot = -1;
		float origin[3] = {0, 0, 0};
		float voxel = 0.0f;
		int dim = 0;
		float impulse[3] = {0, 0, 0};
	};
	struct Merging {
		int body_index = -1;
		int out_slot = -1;
	};

	bool window_is_fresh(const PendingWindow &w) const;
	int run_connectivity(const PendingWindow &w);
	void land_extraction(const IslandExtractResult &r);
	void land_resample(const IslandExtractResult &r);
	void publish_descriptors();
	void start_merges();
	int free_atlas_slot() const;
	void despawn(int index);

	VoxelWorld *world_ = nullptr;
	ve::AnalyticGenerator gen_;
	std::deque<PendingWindow> windows_;
	std::vector<InFlight> in_flight_;
	std::vector<Merging> merging_;
	// A SLOT POOL, not a list: Merging::body_index outlives a frame, so a despawn nulls its
	// entry and the next spawn reuses it. Erasing would renumber every body after it and
	// silently re-merge the wrong one.
	std::vector<IslandBody *> bodies_;
	std::vector<char> atlas_used_;
	ve::ComponentConfig comp_cfg_;
	ve::ContactRefineConfig refine_cfg_;
	int next_id_ = 1;
	int slot_high_water_ = 0;
	float merge_sleep_s_ = 2.0f; // spec §5: "Body sleeps ~2s -> re-merge"
	// Counters the HUD, the benchmark and tests/test_connectivity.gd read.
	// Where the last re-merge landed, for stats()'s ground probe.
	float last_merge_xz_[2] = {0.0f, 0.0f};
	int connectivity_runs_ = 0;
	int islands_spawned_ = 0;
	int debris_spawned_ = 0;
	int islands_merged_ = 0;
	int refused_ = 0; // components left attached because a pool was full
	float last_ms_ = 0.0f;
};

} // namespace godot
```

- [ ] **Step 5: Write `physics/island_manager.cpp`**

The parts that carry the design; the rest is bookkeeping in the same shape as `ColliderStreamer::run_frame`.

```cpp
#include "physics/island_manager.h"
#include "voxel_world.h"
#include "mesh/box_merge.h"
#include "render/mesh_service.h"
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace godot;

namespace {

using Clock = std::chrono::steady_clock;

// How long a window may wait for the occupancy grid to catch up before the manager gives up
// and runs anyway. The readback ring is eight deep and a request goes out the frame after
// the mark, so a large blast's fan-out lands within a handful of frames; thirty is a
// generous ceiling that keeps a stalled readback from silently disabling destruction.
constexpr int kMaxWindowWaitFrames = 30;
// Extractions submitted per frame. Each is ~1-2 ms on the worker; two keeps a big collapse
// resolving inside a few frames without starving collision meshing.
constexpr int kExtractsPerFrame = 2;
// Spec §5: "components <~0.2 m^3 become plain mesh debris (not raymarch targets)".
constexpr float kDebrisVolumeM3 = 0.2f;
// Spec §5's "<=64 total active dynamic bodies".
constexpr int kMaxDynamicBodies = 64;

// The residency's view of the world field, for ve::refine_anchoring. The lock is taken per
// call rather than held, exactly as ColliderStreamer::LogProbe does, so an edit landing
// mid-refinement waits rather than deadlocks.
struct LogContactProbe : ve::ContactProbe {
	const ve::Generator *gen = nullptr;
	ve::EditLog *log = nullptr;
	std::mutex *mu = nullptr;
	const ve::VolumeStore *volumes = nullptr;
	int face_samples = 9;

	int contact_samples(ve::IVec3 cell, int axis) const override {
		std::lock_guard<std::mutex> lock(*mu);
		const std::vector<ve::EditOp> &ops = log->ops(ve::WorldBounds::region_of_brick(cell));
		return ve::contact_samples_field(*gen, ops.data(), static_cast<int>(ops.size()), cell,
				axis, face_samples, volumes);
	}
};

} // namespace

void IslandManager::note_edit(const ve::EditOp &op, int64_t seq) {
	if (op.type == ve::kOpSpherePaint) return; // paint moves no matter
	float lo[3], hi[3];
	ve::op_world_aabb(op, lo, hi);
	PendingWindow w;
	const auto cell = [](float v) {
		return static_cast<int>(std::floor(v / ve::kOccupancyCellSize));
	};
	w.lo = {cell(lo[0]), cell(lo[1]), cell(lo[2])};
	w.hi = {cell(hi[0]), cell(hi[1]), cell(hi[2])};
	w.seq = seq;
	// Spec §5's "explosion + radial impulse": a piece freed by a blast is thrown away from
	// it, which is the difference between rubble falling and rubble erupting.
	if (op.type == ve::kOpSphereSubtract) {
		for (int a = 0; a < 3; a++) w.impulse_from[a] = op.pos[a];
		w.impulse_scale = op.radius;
	}

	// Merge into an overlapping window rather than queueing a second one: spec §5 wants ONE
	// connectivity run per frame however many blasts landed, and two windows over the same
	// rubble would label the same component twice.
	for (PendingWindow &e : windows_) {
		const bool overlap = e.lo.x <= w.hi.x && e.hi.x >= w.lo.x && e.lo.y <= w.hi.y &&
				e.hi.y >= w.lo.y && e.lo.z <= w.hi.z && e.hi.z >= w.lo.z;
		if (!overlap) continue;
		e.lo = {std::min(e.lo.x, w.lo.x), std::min(e.lo.y, w.lo.y), std::min(e.lo.z, w.lo.z)};
		e.hi = {std::max(e.hi.x, w.hi.x), std::max(e.hi.y, w.hi.y), std::max(e.hi.z, w.hi.z)};
		e.seq = std::max(e.seq, w.seq);
		if (w.impulse_scale > e.impulse_scale) {
			e.impulse_scale = w.impulse_scale;
			for (int a = 0; a < 3; a++) e.impulse_from[a] = w.impulse_from[a];
		}
		return;
	}
	windows_.push_back(w);
}

bool IslandManager::window_is_fresh(const PendingWindow &w) const {
	// Every region the window touches must have been re-probed since the edit. A region with
	// no block at all is "unknown", which the flood treats as anchored ground -- it cannot
	// hide an island, so it does not hold the window up either.
	const ve::OccupancyGrid &grid = world_->occupancy();
	const ve::IVec3 rlo = ve::WorldBounds::region_of_brick(w.lo);
	const ve::IVec3 rhi = ve::WorldBounds::region_of_brick(w.hi);
	for (int z = rlo.z; z <= rhi.z; z++)
		for (int y = rlo.y; y <= rhi.y; y++)
			for (int x = rlo.x; x <= rhi.x; x++) {
				const int64_t s = grid.block_seq({x, y, z});
				if (s >= 0 && s < w.seq) return false;
			}
	return true;
}

int IslandManager::run_connectivity(const PendingWindow &pw) {
	connectivity_runs_++;
	ve::FloodWindow w = ve::FloodWindow::around(pw.lo, pw.hi, ve::kFloodWindowCells);
	ve::LinkCuts cuts;
	ve::FloodResult r;
	LogContactProbe probe;
	probe.gen = &gen_;
	probe.log = world_->edit_log();
	probe.mu = &world_->edit_mutex();
	probe.volumes = &world_->volumes();
	probe.face_samples = refine_cfg_.face_samples;

	for (int expand = 0;; expand++) {
		ve::flood_anchored(world_->occupancy(), w, &cuts, &r);
		// Spec §5's marginal-contact refinement, before labelling: a piece held by one thin
		// neck must be cut loose BEFORE the labeller decides it is anchored.
		ve::refine_anchoring(world_->occupancy(), probe, refine_cfg_, &cuts, &r);
		if (!r.frontier_reached || expand >= ve::kMaxWindowExpansions) break;
		// Spec §5: "expanding if the frontier is reached".
		w = ve::FloodWindow::around(pw.lo, pw.hi, w.dim * 2);
		cuts.clear();
	}
	if (r.frontier_reached)
		UtilityFunctions::print_verbose(
				"IslandManager: a loose piece reaches the widest window; treating it as anchored");

	std::vector<ve::IslandComponent> comps;
	ve::label_islands(r, comp_cfg_, &comps);

	int submitted = 0;
	std::vector<IslandExtractJob> jobs;
	for (const ve::IslandComponent &c : comps) {
		if (submitted >= kExtractsPerFrame) break;
		if (static_cast<int>(bodies_.size()) >= kMaxDynamicBodies) { refused_++; break; }

		std::vector<ve::CellBox> boxes;
		if (!ve::greedy_box_merge(c.cells, ve::kMaxIslandBoxes, &boxes)) {
			// Fail-soft (spec §8): a shape too fragmented for 64 boxes stays attached. The
			// labeller's extent bound does not bound box COUNT, and a partial carve would
			// leave matter in two places at once.
			refused_++;
			continue;
		}
		float wlo[3], whi[3];
		c.world_aabb(wlo, whi);
		IslandExtractJob job;
		job.kind = kExtractField;
		job.id = next_id_++;
		job.boxes = boxes;
		job.dim = ve::kIslandDim;
		if (!ve::plan_island_lattice(wlo, whi, job.dim, &job.voxel, job.origin)) {
			refused_++;
			continue;
		}
		const int slot = world_->volumes().allocate();
		if (slot < 0) { refused_++; continue; } // pool full: leave it attached
		{
			std::lock_guard<std::mutex> lock(world_->edit_mutex());
			job.ops = world_->edit_log()->ops(ve::WorldBounds::region_of_brick(c.lo));
		}

		InFlight f;
		f.id = job.id;
		f.boxes = boxes;
		f.volume_slot = slot;
		f.voxel = job.voxel;
		f.dim = job.dim;
		for (int a = 0; a < 3; a++) f.origin[a] = job.origin[a];
		if (pw.impulse_scale > 0.0f) {
			// Away from the blast, falling off with distance, scaled by the blast radius.
			const float cx = 0.5f * (wlo[0] + whi[0]);
			const float cy = 0.5f * (wlo[1] + whi[1]);
			const float cz = 0.5f * (wlo[2] + whi[2]);
			float v[3] = {cx - pw.impulse_from[0], cy - pw.impulse_from[1],
					cz - pw.impulse_from[2]};
			const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (len > 0.001f) {
				const float mag = 60.0f * pw.impulse_scale /
						std::max(len, pw.impulse_scale);
				for (int a = 0; a < 3; a++) f.impulse[a] = v[a] / len * mag;
			}
		}
		in_flight_.push_back(std::move(f));
		jobs.push_back(std::move(job));
		submitted++;
	}
	if (!jobs.empty()) world_->mesh_service()->submit_extracts(std::move(jobs));
	return submitted;
}

void IslandManager::land_extraction(const IslandExtractResult &r) {
	auto it = std::find_if(in_flight_.begin(), in_flight_.end(),
			[&r](const InFlight &f) { return f.id == r.id; });
	if (it == in_flight_.end()) return;
	const InFlight f = *it;
	in_flight_.erase(it);
	if (r.failed || r.data.solid_voxels == 0) {
		world_->volumes().release(f.volume_slot);
		return; // nothing there after all: the terrain keeps whatever the boxes covered
	}

	const float solid_m3 = static_cast<float>(r.data.solid_voxels) * f.voxel * f.voxel * f.voxel;
	const bool debris = solid_m3 < kDebrisVolumeM3;
	int atlas_slot = -1;
	if (!debris) {
		atlas_slot = free_atlas_slot();
		if (atlas_slot < 0) {
			// Spec §5's "<=32 island bodies (oldest sleepers merge early)". Nothing is
			// carved yet, so refusing costs only that this piece stays put for now.
			refused_++;
			world_->volumes().release(f.volume_slot);
			return;
		}
		atlas_used_[static_cast<size_t>(atlas_slot)] = 1;
		slot_high_water_ = std::max(slot_high_water_, atlas_slot + 1);
	}

	world_->volumes().store(f.volume_slot, r.data);

	// 1. Carve (spec §5 step 1). The boxes tile the component exactly, so this removes the
	//    material that just became a body and nothing else. Ordered AFTER the extraction so
	//    the volume holds the rock rather than the hole.
	for (const ve::CellBox &b : f.boxes)
		world_->append_edit(ve::make_box_subtract(b.lo, b.hi));
	// 2. Tell the occupancy grid straight away. The GPU readback that would say the same
	//    thing is several frames out, and until it lands the next connectivity run would
	//    find this component all over again and carve it twice.
	for (const ve::CellBox &b : f.boxes)
		for (int z = b.lo.z; z <= b.hi.z; z++)
			for (int y = b.lo.y; y <= b.hi.y; y++)
				for (int x = b.lo.x; x <= b.hi.x; x++)
					world_->occupancy().set_cell({x, y, z}, ve::kCellAir, world_->edit_seq());

	// 3. Spawn (spec §5 step 3).
	IslandSpawn info;
	info.volume_slot = f.volume_slot;
	info.atlas_slot = atlas_slot;
	info.boxes = f.boxes;
	for (int a = 0; a < 3; a++) info.lattice_origin[a] = f.origin[a];
	info.voxel = f.voxel;
	info.dim = f.dim;
	info.solid_voxels = r.data.solid_voxels;
	for (int a = 0; a < 3; a++) info.impulse[a] = f.impulse[a];
	info.debris = debris;

	IslandBody *b = new IslandBody();
	const Ref<World3D> w3 = world_->get_world_3d();
	if (!b->spawn(w3.is_valid() ? w3->get_space() : RID(),
				w3.is_valid() ? w3->get_scenario() : RID(), info, &r.data)) {
		delete b;
		if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
		world_->volumes().release(f.volume_slot);
		refused_++;
		return; // the carve stands; the piece is simply gone, which is better than doubled
	}
	// Reuse a hole left by a despawn; append only when there is none.
	{
		size_t k = 0;
		while (k < bodies_.size() && bodies_[k] != nullptr) k++;
		if (k < bodies_.size()) bodies_[k] = b;
		else bodies_.push_back(b);
	}
	if (debris) debris_spawned_++;
	else islands_spawned_++;

	// 4. The raymarcher needs the bytes (spec §3's dense per-island texture).
	if (atlas_slot >= 0) world_->queue_island_upload(atlas_slot, r.data);
}

void IslandManager::start_merges() {
	std::vector<IslandExtractJob> jobs;
	for (size_t i = 0; i < bodies_.size(); i++) {
		IslandBody *b = bodies_[i];
		if (!b || !b->live() || b->asleep_seconds() < merge_sleep_s_) continue;
		if (std::any_of(merging_.begin(), merging_.end(),
					[&](const Merging &m) { return m.body_index == static_cast<int>(i); }))
			continue;
		const ve::VolumeData *src = world_->volumes().get(b->info().volume_slot);
		if (!src) continue;
		const int out = world_->volumes().allocate();
		if (out < 0) {
			// Fail-soft (see the plan's Deliberate Deferrals): the body stays a body. It is
			// still collidable and still drawn; it just never becomes terrain.
			if (refused_++ == 0)
				UtilityFunctions::printerr(
						"IslandManager: volume pool full; sleeping islands stay as bodies");
			continue;
		}

		const Transform3D xf = b->transform();
		IslandExtractJob job;
		job.kind = kResampleVolume;
		job.id = next_id_++;
		job.dim = b->info().dim;
		job.source = *src;
		// The birth placement, in the body's LOCAL frame: that is what the volume's samples
		// are indexed against, and it is exactly what IslandBody handed the raymarcher.
		job.source_op = ve::make_volume_add(b->info().volume_slot, b->local_lattice_origin(),
				b->info().voxel, b->info().dim);
		// ve::resample_volume takes the rotation ROW major, and Godot's Basis indexes rows.
		for (int a = 0; a < 3; a++)
			for (int k = 0; k < 3; k++) job.basis[a * 3 + k] = xf.basis[a][k];
		job.rest_origin[0] = xf.origin.x;
		job.rest_origin[1] = xf.origin.y;
		job.rest_origin[2] = xf.origin.z;
		job.out_slot = out;
		merging_.push_back(Merging{static_cast<int>(i), out});
		jobs.push_back(std::move(job));
		break; // one re-merge in flight at a time: the paste changes the field under the rest
	}
	if (!jobs.empty()) world_->mesh_service()->submit_extracts(std::move(jobs));
}

void IslandManager::land_resample(const IslandExtractResult &r) {
	if (merging_.empty()) return;
	const Merging m = merging_.front();
	merging_.erase(merging_.begin());
	if (r.failed || m.body_index < 0 || m.body_index >= static_cast<int>(bodies_.size())) {
		world_->volumes().release(m.out_slot);
		return;
	}
	// Store, PIN and upload BEFORE the op reaches the log: once an op names a slot the
	// slot can never be reused, and the two GPU mirrors must already hold the bytes or a
	// brick regenerated this frame would read an empty slot.
	world_->volumes().store(m.out_slot, r.data);
	world_->volumes().pin(m.out_slot);
	world_->queue_field_volume_upload(m.out_slot, r.data);

	// Spec §5 step 4: "stamped back as a CSG paste-op ... Rubble permanently accumulates".
	const Transform3D rest = bodies_[m.body_index]->transform();
	last_merge_xz_[0] = rest.origin.x;
	last_merge_xz_[1] = rest.origin.z;
	world_->append_edit(r.op);
	despawn(m.body_index);
	islands_merged_++;
}

void IslandManager::publish_descriptors() {
	std::vector<IslandSlotDesc> descs(kMaxIslands);
	for (IslandBody *b : bodies_) {
		if (!b || !b->live() || b->info().atlas_slot < 0) continue;
		const Transform3D xf = b->transform();
		IslandSlotDesc &d = descs[static_cast<size_t>(b->info().atlas_slot)];
		d.live = true;
		d.dim = b->info().dim;
		d.voxel = b->info().voxel;
		// COLUMN major: basis[a] is the world direction of local +a, which is what
		// Basis::get_column returns and what the shader's mat3(c0, c1, c2) expects.
		for (int a = 0; a < 3; a++) {
			const Vector3 c = xf.basis.get_column(a);
			d.basis[a * 3 + 0] = c.x;
			d.basis[a * 3 + 1] = c.y;
			d.basis[a * 3 + 2] = c.z;
		}
		d.origin[0] = xf.origin.x;
		d.origin[1] = xf.origin.y;
		d.origin[2] = xf.origin.z;
		for (int a = 0; a < 3; a++) d.lattice_origin[a] = b->local_lattice_origin()[a];
		d.recompute_world_aabb();
	}
	world_->publish_island_descriptors(descs);
}

int IslandManager::run_frame(float dt, const Vector3 &center) {
	if (!world_ || !world_->mesh_service()) return 0;
	const Clock::time_point t0 = Clock::now();
	int actions = 0;

	// 1. Bodies.
	for (IslandBody *b : bodies_)
		if (b && b->live()) {
			b->tick(dt);
			b->sync_render();
		}
	publish_descriptors();

	// 2. Results.
	std::vector<IslandExtractResult> results;
	world_->mesh_service()->collect_extracts(&results);
	for (const IslandExtractResult &r : results) {
		actions++;
		if (r.kind == kResampleVolume) land_resample(r);
		else land_extraction(r);
	}

	// 3. Connectivity, ONCE (spec §5). Held back while extractions are outstanding so a
	//    component cannot be labelled twice before its carve lands.
	if (!windows_.empty() && in_flight_.empty() && !world_->mesh_service()->extracts_busy()) {
		PendingWindow w = windows_.front();
		if (window_is_fresh(w) || w.waited >= kMaxWindowWaitFrames) {
			windows_.pop_front();
			if (!window_is_fresh(w))
				UtilityFunctions::print_verbose(
						"IslandManager: occupancy readback is behind; running anyway");
			actions += run_connectivity(w);
		} else {
			windows_.front().waited++;
		}
	}

	// 4. Re-merge.
	if (merging_.empty()) start_merges();

	// Spec §6's "small bubbles around active bodies": the collider streamer already accepts
	// N centres, and the bodies are the other N - 1.
	world_->set_physics_bubbles(bodies_);

	last_ms_ = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
	(void)center;
	return actions;
}
```

And the three small members the rest leans on:

```cpp
int IslandManager::free_atlas_slot() const {
	for (int i = 0; i < kMaxIslands; i++)
		if (!atlas_used_[static_cast<size_t>(i)]) return i;
	return -1;
}

void IslandManager::despawn(int index) {
	if (index < 0 || index >= static_cast<int>(bodies_.size()) || !bodies_[index]) return;
	IslandBody *b = bodies_[index];
	if (b->info().atlas_slot >= 0) {
		atlas_used_[static_cast<size_t>(b->info().atlas_slot)] = 0;
		// The descriptor is republished next frame with this slot dead, which is what stops
		// the raymarcher reading the bytes; they are left in place because nothing reads a
		// slot whose descriptor says dim 0.
	}
	// The BIRTH volume is never pinned -- no op in the edit log ever named it -- so releasing
	// it is safe. The RESTED volume the paste created is pinned and stays for ever.
	world_->volumes().release(b->info().volume_slot);
	delete b;
	bodies_[index] = nullptr; // a hole, not an erase: Merging::body_index must stay valid
}

Dictionary IslandManager::stats() {
	Dictionary d;
	int live_bodies = 0, live_islands = 0, live_debris = 0;
	float lowest = 1e30f;
	for (IslandBody *b : bodies_) {
		if (!b || !b->live()) continue;
		live_bodies++;
		if (b->info().debris) live_debris++;
		else live_islands++;
		lowest = std::min(lowest, static_cast<float>(b->transform().origin.y));
	}
	d["live_bodies"] = live_bodies;
	d["live_islands"] = live_islands;
	d["live_debris"] = live_debris;
	d["lowest_body_y"] = live_bodies > 0 ? lowest : 0.0f;
	d["islands_spawned"] = islands_spawned_;
	d["debris_spawned"] = debris_spawned_;
	d["islands_merged"] = islands_merged_;
	d["connectivity_runs"] = connectivity_runs_;
	d["refused"] = refused_;
	d["pending_windows"] = static_cast<int>(windows_.size());
	d["in_flight"] = static_cast<int>(in_flight_.size());
	d["manager_ms"] = last_ms_;
	// Where the ground is under the last body to fall, so a test can say "the rubble is
	// standing on it" without knowing the terrain's shape. ve::raycast reads the same field
	// the paste went into, which is the point of asking it rather than the physics.
	float ground = 0.0f;
	if (world_) {
		const ve::RayHit h = world_->analytic_raycast_down(last_merge_xz_);
		if (h.hit) ground = h.pos[1];
	}
	d["ground_y"] = ground;
	return d;
}
```

`last_merge_xz_` is a `float[2]` the manager records in `land_resample` from the rested body's
transform, and `VoxelWorld::analytic_raycast_down(const float xz[2])` is a two-line wrapper
over the `ve::raycast` call `debug_raycast` already makes — the manager may not build its own
`EditLog` view, and this is the one number it needs from the field.

- [ ] **Step 6: Wire it into `VoxelWorld`**

`extension/src/voxel_world.h` — the manager, the two render-thread queues, and the accessors the manager needs:

```cpp
	// Bytes on their way to a GPU pool. Filled on the main thread, drained on the render
	// thread by the compositor before it runs the streamer -- an op that names a volume must
	// never be evaluated before the volume is there.
	struct IslandUpload {
		int slot = -1;
		bool to_island_atlas = false; // false = the field volume pool
		ve::VolumeData data;
	};

	IslandManager *island_manager_ = nullptr;
	std::mutex island_mutex_;
	std::vector<IslandUpload> island_uploads_;
	std::vector<IslandSlotDesc> island_descs_;
	bool island_descs_dirty_ = false;

public:
	ve::VolumeSet &volumes() { return volumes_; }
	MeshService *mesh_service() { return mesh_; }
	IslandAtlas *islands() { return islands_; }
	IslandCullPass *island_cull() { return island_cull_; }
	void queue_island_upload(int slot, const ve::VolumeData &d);
	void queue_field_volume_upload(int slot, const ve::VolumeData &d);
	void publish_island_descriptors(const std::vector<IslandSlotDesc> &d);
	void set_physics_bubbles(const std::vector<IslandBody *> &bodies);
	// A downward ve::raycast at (xz[0], xz[1]) from above the world, on the analytic field
	// plus its region ops and volumes -- the same call debug_raycast makes. The manager may
	// not build its own EditLog view, and this is the one field query it needs.
	ve::RayHit analytic_raycast_down(const float xz[2]);
	// Drained by RaymarchCompositor on the render thread; returns how many landed.
	int drain_island_uploads(RenderingDevice *device);

	int debug_island_frame(float dt, Vector3 center);
	Dictionary debug_island_stats();
	void debug_set_merge_sleep_seconds(float v);
```

`queue_field_volume_upload` does two things: it queues the render-device copy, **and** it calls `mesh_->submit_volume(slot, d)` straight away so the worker's pool is updated before its next job.

`ensure_physics_initialized` creates the manager after the collider streamer; `teardown_physics` deletes it first. `append_edit` calls `island_manager_->note_edit(op, edit_seq_)` after the append, under no lock (the manager is main-thread and so is `append_edit`'s caller).

`_process` gains the third tick:

```cpp
	if (island_manager_) island_manager_->run_frame(static_cast<float>(delta),
			anchor->get_global_position());
```

`island_slot_count()` returns `island_manager_ ? island_manager_->slot_high_water() : 0`.

`set_physics_bubbles` stores the live bodies' positions in a `std::vector<float>` the collider streamer reads: `ColliderStreamer::run_frame` gains an overload taking extra centres, and passes them to `ve::ChunkResidency::update`, which has accepted N centres since M3 (spec §6's "small bubbles around active bodies", the deferral M3 recorded).

`extension/src/raymarch_compositor.cpp` — drain first, in this order:

```cpp
	// Volumes before anything that evaluates the field: an op naming a slot may already be
	// in the edit log, and the streamer is about to regenerate the bricks that read it.
	world->drain_island_uploads(rd);
	WorldStreamer *st = world->streamer();
	if (st) st->run_frame(rd, cam.origin.x, cam.origin.y, cam.origin.z);
```

`drain_island_uploads` swaps the queue under `island_mutex_`, calls `islands_->upload` or `atlas_->volumes().upload` per entry, then uploads the descriptor array if `island_descs_dirty_`.

- [ ] **Step 7: Add the test hooks**

```cpp
int VoxelWorld::debug_island_frame(float dt, Vector3 center) {
	ensure_initialized();
	ensure_physics_initialized();
	if (!island_manager_) return 0;
	drain_occupancy();
	const int n = island_manager_->run_frame(dt, center);
	// The tests drive the world by hand and never enter the compositor, so the render-thread
	// half of the handoff has to happen here too.
	RenderingDevice *device = rd();
	if (device) {
		drain_island_uploads(device);
		device->submit();
		device->sync();
	}
	return n;
}

Dictionary VoxelWorld::debug_island_stats() {
	return island_manager_ ? island_manager_->stats() : Dictionary();
}

void VoxelWorld::debug_set_merge_sleep_seconds(float v) {
	ensure_physics_initialized();
	if (island_manager_) island_manager_->set_merge_sleep_seconds(v);
}
```

- [ ] **Step 8: Run everything**

Run: `cd extension && scons test`
Expected: PASS.

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: green, including the six new `test_connectivity.gd` cases.

- [ ] **Step 9: Commit**

```bash
git add extension/src/physics/island_manager.h extension/src/physics/island_manager.cpp \
        extension/src/render/island_extract_pass.h extension/src/render/mesh_service.cpp \
        extension/src/physics/collider_streamer.h extension/src/physics/collider_streamer.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp \
        extension/src/raymarch_compositor.cpp tests/test_connectivity.gd
git commit -m "feat: island lifecycle from connectivity to re-merge"
```

---
### Task 14: the demo — a line drill, islands on the HUD, and a benchmark that breaks things

Spec §5's *Demo edit tools*: "sphere-subtract (explosion + radial impulse), sphere-add, material paint brush, **line drill**. All are just op emitters." Three of the four shipped with M2; the drill is the one that reliably severs an overhang, which makes it the tool this milestone is demonstrated with.

**Files:**
- Modify: `demo/edit_tool.gd`, `demo/hud.gd`, `demo/benchmark.gd`
- Modify: `extension/src/voxel_world.cpp` (`debug_perf_stats`)

**Interfaces:**
- Consumes: `VoxelWorld::debug_island_stats` (Task 13), `VoxelEditTool::apply_sphere_subtract` (M2).
- Produces: nothing further tasks depend on.

- [ ] **Step 1: Add the line drill**

Append to `demo/edit_tool.gd`'s exports:

```gdscript
@export var drill_radius := 0.6
@export var drill_length := 6.0
@export var drill_steps := 10
```

and handle the key in `_unhandled_input`, before the mouse-button block:

```gdscript
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_R:
		_drill()
		return
```

with:

```gdscript
func _drill() -> void:
	# Spec section 5's line drill: a row of small subtracts along the aim ray, starting at
	# the surface and boring inward. It is the tool that actually severs an overhang, because
	# a narrow bore cuts a support without swallowing the piece it was holding up.
	var hit: Dictionary = _world.debug_raycast(
		_cam.global_position, -_cam.global_transform.basis.z)
	if not hit["hit"]:
		return
	var dir := -_cam.global_transform.basis.z.normalized()
	var start: Vector3 = hit["pos"]
	var step := drill_length / float(maxi(drill_steps - 1, 1))
	for i in range(drill_steps):
		_tool.apply_sphere_subtract(start + dir * (step * i), drill_radius)
	_kick(start)
```

- [ ] **Step 2: Put the islands on the HUD**

`demo/hud.gd` — extend the physics line:

```gdscript
	var isl := ""
	if _world:
		var st: Dictionary = _world.debug_island_stats()
		# islands/debris are what is in the air right now; spawned/merged are the running
		# totals, so a demo recording can be checked afterwards for whether the loop closed.
		isl = "  |  isl %d dbr %d (+%d/-%d)  cx %d  %.1fms" % [
			st.get("live_islands", 0), st.get("live_debris", 0),
			st.get("islands_spawned", 0), st.get("islands_merged", 0),
			st.get("connectivity_runs", 0), st.get("manager_ms", 0.0)]
	text = "%d fps  (%.1f ms)  |  %s%s%s" % [fps, ms, s, p, isl]
```

- [ ] **Step 3: Report the manager's cost**

Add `float last_ms() const { return last_ms_; }` to `IslandManager`'s public section (the counter is already there), and in `VoxelWorld::debug_perf_stats`:

```cpp
	d["island_ms"] = island_manager_ ? island_manager_->last_ms() : 0.0f;
```

The plain float rather than `stats()["manager_ms"]`: `debug_perf_stats` runs every frame in the benchmark, and building a `Dictionary` to read one number out of it is the kind of cost that shows up in the thing it is measuring.

- [ ] **Step 4: Add a benchmark mode that makes islands**

`demo/benchmark.gd` — a fourth mode. In the header comment:

```gdscript
#   --benchmark-island the player is frozen and the drill severs an overhang every second,
#                      so connectivity, extraction, spawning and re-merging all run under
#                      the frame timer.
```

in `_ready`'s mode list, `"--benchmark-island"` before `"--benchmark"`, and alongside the `--benchmark-edit` tool creation:

```gdscript
	if _mode == "--benchmark-edit" or _mode == "--benchmark-island":
		_tool = ClassDB.instantiate("VoxelEditTool")
		_world.add_child(_tool)
```

in `_process`:

```gdscript
	elif _mode == "--benchmark-island" and _frames > WARMUP:
		_island_cycle(delta)
```

and:

```gdscript
var _island_timer := 0.0
var _island_built := false

func _island_cycle(delta: float) -> void:
	# Build a pillar, wait for it to stream in, drill through its middle, repeat. Each cycle
	# puts one connectivity run, one extraction, one spawn and (a couple of seconds later)
	# one re-merge inside the sampled window.
	_island_timer += delta
	var base := _player.global_position + Vector3(6.0, -4.0, 6.0)
	if not _island_built:
		for i in range(5):
			_tool.apply_sphere_add(base + Vector3(0, 1.0 * i, 0), 1.2, 4)
		_island_built = true
		_island_timer = 0.0
		return
	if _island_timer < 1.0:
		return
	_tool.apply_sphere_subtract(base + Vector3(0, 2.0, 0), 1.6)
	_island_built = false
	_island_timer = 0.0
```

and in `_report()`, after the physics line:

```gdscript
	var isl: Dictionary = _world.debug_island_stats()
	print("BENCH islands=%d debris=%d spawned=%d merged=%d refused=%d cx_runs=%d" % [
		isl.get("live_islands", -1), isl.get("live_debris", -1),
		isl.get("islands_spawned", -1), isl.get("islands_merged", -1),
		isl.get("refused", -1), isl.get("connectivity_runs", -1)])
```

- [ ] **Step 5: Play it**

Run: `godot --path . demo/main.tscn`

Walk to a hillside, press `R` to drill through a lip of rock, and watch the piece above the bore drop, tumble, settle and — two seconds later — become terrain you can stand on. Check:
- the HUD's `isl` count rises when a piece comes loose and returns to 0 after it merges;
- `+n/-n` end up equal once everything has settled;
- `failures` on the physics readout stays 0;
- the fallen rock is walkable *before* it merges (its box compound) and *after* (the collision mesher re-meshed the pasted volume);
- shooting the fallen rock a second time breaks it up again — the paste put it in the field, so a subtract now applies to it (spec §5's "a second explosion breaks them").

- [ ] **Step 6: Benchmark it**

Run: `godot --path . demo/main.tscn -- --benchmark-island --disable-vsync`
Expected: `BENCH` lines printed, `frame_avg_ms` under 16.6, `spawned` and `merged` both greater than zero, `refused=0`, `failures=0`.

Run the other three modes as well and confirm none regressed:

```bash
for m in --benchmark --benchmark-move --benchmark-edit --benchmark-island; do
  godot --path . demo/main.tscn -- $m --disable-vsync 2>&1 | grep '^BENCH'
done
```

- [ ] **Step 7: Commit**

```bash
git add demo/edit_tool.gd demo/hud.gd demo/benchmark.gd extension/src/voxel_world.cpp
git commit -m "feat: line drill, island HUD and an island benchmark mode"
```

---

## M4 Acceptance Checklist

- `cd extension && scons test` — native suite green: occupancy, flood fill, components, contact refinement, box merging, volume ops and lattice planning, plus every M1–M3 case
- `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests` — green, including the six new suites: **`test_field_volume_diff.gd`**, **`test_occupancy.gd`**, **`test_island_extract.gd`**, **`test_island_render.gd`**, **`test_island_body.gd`**, **`test_connectivity.gd`**
- `godot --path . demo/main.tscn` — press `R` to drill through an overhang: the piece falls, tumbles, settles, and becomes walkable terrain about two seconds later. `isl` returns to 0, `+n` equals `-n`, `failures` stays 0
- `godot --path . demo/main.tscn -- --benchmark-island --disable-vsync` — `frame_avg_ms` < 16.6, `spawned > 0`, `merged > 0`, `refused=0`, `failures=0`
- The three older benchmark modes are unchanged within noise: M4 adds one compute dispatch (~0.05 ms) and a 4 KB descriptor upload to the render thread, and nothing else
- The CPU references still hold: `ve::extract_island_volume` against `island_extract.comp.glsl`, `ve::apply_op` against `field.glslh`, `ve::cell_state_field` against `brick_mark.comp.glsl` — each guarded by its own differential test
- **The safe direction is preserved**: no test, and no minute of play, produces a hole in the terrain where an island used to be but no body exists. Every fail-soft arm in M4 leaves the piece attached

## Spec Coverage

| Spec sentence | Where |
|---|---|
| §5 "Global persistent occupancy grid, 0.8 m cells = one bit per brick" | Tasks 1, 8 — two bits, because the mark pass already has both (Deliberate Decisions) |
| §5 "updated incrementally from brick-regen readback (1-frame latency)" | Task 8 — from the **mark** pass, asynchronously, ~4 frames (Deliberate Decisions) |
| §5 "localized flood fill from the window boundary (64³ cells), expanding if the frontier is reached" | Task 2 (`flood_anchored`, `frontier_reached`), Task 13 (the expansion loop) |
| §5 "6-connectivity (face-only) defines support" | Task 2 — a doctest for corner and edge contact |
| §5 "Marginal-contact refinement … a tiny GPU check samples the true 5 cm SDF" | Task 4 — bridges + an 81-sample face test, on the CPU (Deliberate Decisions) |
| §5 "Solid cells unreachable from the boundary → CCL → each group = one island" | Task 3 |
| §5 "Runs once per frame after all that frame's edits" | Task 13 — windows merge, and one runs per frame |
| §5 step 1 "Carve out of the static SDF (automatic subtract op → correct crater)" | Task 6 (`kOpBoxSubtract`), Task 13 (`land_extraction`) |
| §5 step 2 "Extract dense island SDF texture" | Task 9 |
| §5 step 3 "Jolt rigid body: greedy box-merged compound, mass/inertia from solid volume" | Tasks 5, 12 |
| §5 step 4 "sleeps ~2 s → re-merge … stamped back as a CSG paste-op; body despawned" | Task 6 (`kOpVolumeAdd`, `resample_volume`), Task 13 (`start_merges`, `land_resample`) |
| §5 "Rubble permanently accumulates as terrain" | Task 13 — the paste op is in the edit log, which is the save format |
| §5 guardrails: ≤32 islands, ≤64 bodies, oversized split, <0.2 m³ debris | Task 3 (splitting), Task 13 (`kMaxIslands`, `kMaxDynamicBodies`, `kDebrisVolumeM3`) |
| §5 "Island texture memory: 5 cm, halved to 10 cm for large AABBs, pool capped" | Task 6 (`plan_island_lattice`), Fixed Numbers — the threshold is ~3 m, not 8 m, because the lattice is fixed at 64³ |
| §5 "Demo edit tools … line drill" | Task 14 |
| §3 "Tiled target culling: 16×16 px tiles, island world-AABBs projected per tile" | Task 11 — screen-space overlap plus a max-distance reject; the *depth-range* half of spec §3's "tile/depth range" waits for M5's depth injection, since M4 has no scene depth to test a tile against before the opaque pass |
| §3 "ray → island local space via inverse body transform → same sphere-trace GLSL; nearest hit wins" | Task 10 |
| §3 "Island SDF storage: dense per-island texture (AABB at 5 cm, uint8 + palette + own min–max mip)" | Tasks 9, 10 — a byte of material id instead of a palette (Deliberate Deferrals) |
| §6 "Islands = box compounds", "CCD on fast debris", "Jolt sleep events drive the re-merge hook" | Task 12 |
| §6 "Small debris = single-box bodies + cheap DC render meshes" | Task 12 — a box **compound** and a `ve::dual_contour` mesh through `RenderingServer` |
| §6 "small bubbles around active bodies" | Task 13 (`set_physics_bubbles`) — the M3 deferral, now used |
| §8 `connectivity/` module, `mesh/` box merging | Tasks 1–5 |
| §8 "GPU differential testing: CPU references … dev console command runs both and diffs" | `debug_island_extract_diff` (Task 9), `test_field_volume_diff.gd` (Task 7), `test_occupancy.gd` (Task 8) |
| §5 "islands land whole; a second explosion breaks them" | Deferred by the spec itself; works out of the box once the paste is in the field |

## Errata (recorded during M4 implementation — corrections to the task text)

<!-- Append numbered entries here as the plan meets reality, in the style of M1/M2/M3. -->

1. **Cross-region extraction uses a flattened, globally-ordered op list as a deliberate approximation.** `collect_ops_for_aabb()` gathers every op list overlapping a component's world AABB and applies the combined list to every sample, rather than evaluating per-region op lists and stitching the results at region boundaries. This can over-apply ops across region boundaries and can refuse a cross-region component whose *combined* op count exceeds `kMaxRegionOps` even when each individual region is under cap. The production carve path is protected by the safe-direction invariant: `has_restore_headroom()` refuses to carve any component that touches a full (or near-full) region, and `submit_extracts()` additionally refuses before allocating/submitting when the flattened list already exceeds `kMaxRegionOps`. Therefore a cross-region component whose combined op count exceeds the cap is left attached in the field rather than producing a wrong carve; a wrongly-kept rock is always the safe failure mode under spec §8.
