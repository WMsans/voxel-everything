# M5 Far-Field LoD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** See the whole world — eight levels of surface-nets chunks, each 12 bytes per quad with no vertex buffer at all, streamed by a visibility-driven octree walk, culled against a HiZ built from the raymarched near field, drawn as one indirect multi-draw into Godot's scene framebuffer, and shaded through the *same* triplanar material function the raymarcher calls so the 150 m seam cannot show.

**Status (updated after the final review fixes):** Tasks **1–18 are complete** — their steps are ticked and each carries a `**Status: complete**` line naming the commits. The final review's Critical and Important fixes are complete (see `.superpowers/sdd/final-review-fixes-report.md`). Read the Errata at the bottom first: entries record where this plan's text has already met reality. Native suites 262/262; gdUnit 174/174 across 34 suites.

**Architecture:** A LoD chunk is 32³ cells whose lattice is built by evaluating `G + ops` at **half** the level's cell size and tent-reducing 2:1 — a mip cascade computed inside one build job rather than accumulated through stored levels. The mesher is M3's, generalised from a compile-time pitch to an origin + cell size + lattice dimension in the push constant. *(That generalisation landed but the LoD build pass ended up not using it — see errata 7. There are two GPU cell-vertex shaders and two CPU references, each pinned by its own differential test.)* Geometry is a global arena of fixed 512-quad pages; a shared 6 KB index buffer plus `vertexOffset = page · 2048` lets the vertex shader recover `quad = gl_VertexIndex >> 2` and `page = quad >> 9`, which is how one indirect multi-draw addresses thousands of independently placed chunks without `gl_DrawID` or a non-zero `firstInstance` (Godot exposes neither). Selection, streaming and eviction are one pure-C++ octree walk against a projected-area threshold, an interface-injected occlusion test, and a page budget.

**Tech Stack:** Godot 4.7.1 (`/usr/bin/godot`), godot-cpp (pinned master, `api_version = "4.7"`), SCons, C++20, GLSL 460 (Vulkan), Jolt Physics, doctest 2.4.11 (native), gdUnit4 (in-engine), ImageMagick (`convert`, offline asset step only).

**Spec:** `docs/superpowers/specs/2026-08-17-m5-lod-design.md` (commit `1d8d042`), which supersedes parts of `docs/superpowers/specs/2026-08-12-voxel-engine-design.md` §2 and §4 — its §1 is the table of what changed and why. Read both.

**Predecessors:** `docs/superpowers/plans/2026-08-12-m1-walking-skeleton.md`, `2026-08-13-m2-gpu-generation-streaming-edits.md`, `2026-08-14-m3-physics-meshing-colliders.md`, `2026-08-15-m4-connectivity-islands.md` (all complete). **Read all four Errata sections before touching shaders or `MeshPass`.** The ones this plan collides with directly:

- **M1 errata 2** — Godot 4.7.1 Forward+ is reverse-Z (near = 1.0, far = 0.0). Every pipeline writing into the scene depth buffer uses `COMPARE_OP_GREATER_OR_EQUAL`. The HiZ reduction and the occlusion comparison in Task 14 both depend on this sign.
- **M1 errata 3** — tan-half-fov comes from `|1/c00|`, `|1/c11|`, never `Projection::get_fov()`.
- **M2 errata 5** — GLSL reserved words (`active`, `mat2`) are rejected by glslang. Do not name anything `sample`, `filter`, or `output` either.
- **M2 errata 7** — `ivec4` push-constant members need `.xyz` when passed to `ivec3` parameters.
- **M2 errata 9** — `ve::kSurfaceY = 51.2`; the terrain surface sits at `y ≈ 51.2 + hills(x,z)`.
- **M3 errata 1** — this codebase's winding convention has already cost one bug. Task 13 determines the raster front-face empirically rather than assuming it.
- **M3 errata 5** — streaming must fund its own loads. The LoD arena repeats the lesson: a build that cannot get its pages is refused, never partially allocated.
- **M4 errata 1** — `collect_ops_for_aabb` flattens cross-region op lists into one globally-ordered list; it can exceed `kMaxRegionOps` for a large AABB. Task 9 truncates to a chronological **prefix**.

## Milestone Map

| Milestone | Delivers |
|---|---|
| M1 (done) | Toolchain, raymarched terrain, test harnesses |
| M2 (done) | GPU brick generation, region indirection, residency/LRU, min–max mips, destruction edits |
| M3 (done) | Dual-contour collision meshing on the GPU, async readback, collider streaming into Jolt, character controller |
| M4 (done) | Occupancy grid, connectivity, island carve/extract/spawn/re-merge, raymarched island targets, tiled culling, debris |
| **M5 (this plan)** | Eight-level LoD octree, half-cell mip reduction, 12-byte quad arena, triplanar material textures shared with the near field, HiZ occlusion, indirect multi-draw far field, dithered seam |
| M6 | Beautification: cel bands, three-layer shadows, SSGI, SSR, outlines |
| M7 | Benchmark scene, demo polish |

## Global Constraints

- Godot **4.7.1**; godot-cpp pinned to the existing submodule commit, `api_version = "4.7"` — do not bump either.
- Pure C++ cores in `namespace ve` contain **zero Godot types** (engine spec §8). `lod/` is new and **pure**; `render/` is Godot glue. Anything that selects, prioritises, packs, or measures is pure; anything owning a `RID` is glue.
- Shaders: GLSL `#version 460`, loaded **from files** by `ve::load_shader_source`, never inline strings. `#[compute]` / `#[vertex]` / `#[fragment]` are stripped after load by `ve::strip_shader_annotations`.
- **Never put a literal include directive inside a GLSL comment** — the loader matches the token anywhere on a line and self-includes (note at the top of `shaders/common.glslh`).
- `buffer_update`, `buffer_clear`, `texture_update` are device-level commands: record them **before** `compute_list_begin` / `draw_list_begin`, never inside an open list (M2 Task 12's documented ordering).
- **Push constants stay ≤ 128 bytes** (Vulkan's guaranteed minimum). M5's largest is the LoD raster's 80 bytes.
- `ve::EditOp` stays exactly 32 bytes (`static_assert`). M5 adds no op types.
- Reverse-Z everywhere; `COMPARE_OP_GREATER_OR_EQUAL` for anything writing scene depth.
- Error policy (engine spec §8): dev = validation layers + verbose RD checks; release = fail-soft. **A coarser world is always the safe direction** — a refused build leaves the parent drawn, a failed bake leaves the previous pages, a full arena refuses rather than evicting something on screen.
- Commit style: conventional (`feat:`, `test:`, `fix:`, `build:`, `refactor:`, `docs:`).
- RD API reference: `docs/api/renderingdevice.md`. Consult it before inventing a signature.
- Target hardware: RTX 4070 Laptop. Budgets: LoD raster ≤ 2 ms @1440p, CPU walk ≤ 0.3 ms, chunk build ≤ 1.5 ms GPU, frame ≤ 16 ms.
- **Level indices are 0-based and 0 is the FINEST.** Spec §2's L0 is `level == 0`; L7 is the octree root level.
- **LoD chunk coordinates are GLOBAL per level**, like brick, region and collision-chunk coordinates: the world corner of chunk `c` at level `L` is `c · 32 · lod_cell_size(L)`, no origin term. `WorldBounds` only decides membership.

## Conventions Used Throughout

- **Build:** `./build.sh -j$(nproc)` (or `cd extension && scons -j$(nproc)`)
- **Native tests:** `cd extension && scons test`
- **gdUnit tests:** `./gdunit_tests.sh -a res://tests`, or `./gdunit_tests.sh` for everything. Add `-c` to see every failure — gdUnit4 aborts a suite at its FIRST one by default, so a plain run under-reports. Do **not** invoke `addons/gdUnit4/runtest.sh` directly: it resolves `--path` against the caller's directory and reports success when it discovers no tests at all. The repo runner keeps vsync enabled intentionally (see errata 6).
- **Demo:** `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
- gdUnit tests that await must declare the timeout argument: `func test_x(timeout := 10000) -> void:`
- Every gdUnit suite creating a `VoxelWorld` registers it in `_worlds` and frees it in `after_test()` (M3 errata 2).
- Quad corner order is stored **already wound**: `(c0, c1, c2)` and `(c0, c2, c3)` are the two triangles, and the quads pass emits the reversed order for the negative-sign case exactly as `shaders/mesh_quads.comp.glsl:63-70` already does. The vertex shader never branches on winding.

## Fixed Numbers (decided here; every task assumes them)

| Thing | Value | Where it lives |
|---|---|---|
| LoD levels | **8** | `ve::kLodLevels` |
| Cells per chunk axis | **32** | `ve::kLodChunkCells` |
| L0 cell / chunk | **0.4 m / 12.8 m** | `ve::lod_cell_size(0)`, `ve::lod_chunk_size(0)` |
| Level ratio | **2×** linear | implicit in `kLodBaseCell * (1 << level)` |
| Target lattice | **34³** (`kLodChunkCells + 2`) | `ve::kLodChunkLattice` |
| Mesh cells | **33³** | `ve::kLodChunkMeshCells` |
| Supersample lattice | **69³** (`2 · 34 + 1`) | `ve::kLodFineLattice` |
| Reduction filter | separable tent **¼, ½, ¼** | `ve::lod_reduce_lattice` |
| Quad record | **12 bytes** | `ve::LodQuad` |
| Corner offset precision | **5 bits/axis**, `frac = o / 31` | `ve::kLodOffsetBits` |
| Quads per page | **512** (6 KB) | `ve::kLodQuadsPerPage` |
| Verts per page | **2048** | `ve::kLodVertsPerPage` |
| Max pages per chunk | **16** (8192 quads) | `ve::kLodMaxPagesPerChunk` |
| Arena pool | **32768 pages = 192 MB** | `VoxelWorld::max_lod_pages` |
| Target cell error | **3.0 px** | `ve::kLodTargetCellPx` |
| Descend threshold | **(32 · 3)² = 9216 px²** | `ve::kLodSseAreaThresh` |
| Evict age | **300 frames** | `ve::kLodEvictFrames` |
| Occlusion confirmation | **8 frames** | `ve::kLodOccludedFrames` |
| Always-resident levels | **5, 6, 7** | `ve::kLodResidentLevelFrom` |
| Fade band | **120 → 150 m** | `ve::kLodFadeStartM`, `kLodFadeEndM` |
| LoD builds per frame | **8** (exported) | `VoxelWorld::lod_builds_per_frame` |
| HiZ pyramid | **256² R32F**, 9 mips | `godot::HizPass` |
| HiZ CPU readback level | **mip 3 = 32²** (4 KB) | `godot::HizPass` |
| Material array | **512² RGBA8**, 16 layers, full mips | `godot::MaterialAtlas` |
| Material world tile | **2 m** | `MATERIAL_UV_SCALE = 0.5` |

**Memory.** Arena 32768 × 6 KB = 192 MB; page→chunk table 128 KB; chunk records 8192 × 32 B = 256 KB; indirect args 32768 × 20 B = 640 KB; index buffer 6 KB; two material arrays 16 × 1.4 MB × 2 = 45 MB; HiZ 256² × 4 B × 1.33 = 349 KB. **≈ 238 MB**, inside engine spec §4's 300–500 MB with `max_lod_pages` as the dial.

## File Structure

```
extension/src/
  lod/                                                     (pure C++, namespace ve)
    lod_grid.h/.cpp        level table, chunk math, SSE, op ranges       (Task 1)
    lod_quad.h/.cpp        the 12-byte record, pack/unpack, corners      (Task 2)
    lod_reduce.h/.cpp      tent filter + material vote, CPU reference    (Task 3)
    lod_contour.h/.cpp     CPU reference mesher emitting packed quads    (Task 4)
    lod_skirt.h/.cpp       boundary edges, skirt quads                   (Task 5)
    lod_arena.h/.cpp       page allocator, page->chunk table             (Task 6)
    lod_tree.h/.cpp        octree, walk, requests, eviction              (Task 7)
  mesh/
    mesh_chunk.h           MODIFIED: nothing; read for conventions        —
  render/
    mesh_pass.h/.cpp       MODIFIED: origin + cell + lattice push        (Task 8)
    lod_build_pass.h/.cpp  worker-device field/reduce/cells/quads        (Task 9)
    mesh_service.h/.cpp    MODIFIED: LoD build queue                     (Task 10)
    material_atlas.h/.cpp  the two texture arrays                        (Task 11)
    raymarch_pass.h/.cpp   MODIFIED: binds the material arrays           (Task 11)
    lod_pool.h/.cpp        render-device arena, page uploads             (Task 12)
    lod_raster_pass.h/.cpp indirect args + the draw                      (Task 13)
    hiz_pass.h/.cpp        min-pyramid + async coarse readback           (Task 14)
    lod_cull_pass.h/.cpp   GPU frustum ∩ HiZ, cmdgen                     (Task 15)
    composite_pass.h/.cpp  MODIFIED: dithered depth discard              (Task 16)
  raymarch_compositor.cpp  MODIFIED: hiz -> cull -> lod draw             (Task 15,16)
  voxel_world.h/.cpp       MODIFIED: LoD tick, exports, invalidation     (Task 12,17)
  register_types.cpp       unchanged (no new script-visible classes)      —
shaders/
  common.glslh             MODIFIED: material_surface, shade_terrain, bayer4
  mesh_common.glslh        MODIFIED: origin + cell size + lattice dim    (Task 8)
  mesh_field.comp.glsl     MODIFIED: same                                (Task 8)
  mesh_cells.comp.glsl     MODIFIED: same                                (Task 8)
  raymarch.comp.glsl       MODIFIED: ray differentials + shared shading  (Task 11)
  lod_field.comp.glsl      NEW: half-cell field + material               (Task 9)
  lod_reduce.comp.glsl     NEW: tent reduce + material vote              (Task 9)
  lod_quads.comp.glsl      NEW: packed 12-byte quads                     (Task 9)
  lod.vert.glsl            NEW                                           (Task 13)
  lod.frag.glsl            NEW                                           (Task 13)
  hiz.comp.glsl            NEW                                           (Task 14)
  lod_cull.comp.glsl       NEW                                           (Task 15)
  composite.frag.glsl      MODIFIED: dithered depth discard              (Task 16)
extension/tests/           test_lod_grid, test_lod_quad, test_lod_reduce,
                           test_lod_contour, test_lod_skirt, test_lod_arena,
                           test_lod_tree
tests/                     test_lod_mesh_diff.gd, test_lod_build.gd,
                           test_material_atlas.gd, test_lod_pool.gd,
                           test_lod_render.gd, test_hiz.gd, test_lod_cull.gd,
                           test_lod_seam.gd, test_lod_stream.gd
assets/materials/          NEW: 512² PNGs + .gdignore                    (Task 11)
tools/convert_materials.sh NEW                                            (Task 11)
extension/SConstruct       MODIFIED: lod/ joins both builds              (Task 1)
demo/                      hud.gd, benchmark.gd, main.tscn MODIFIED      (Task 18)
```

---

### Task 1: `lod/lod_grid` — the eight-level lattice

**Status: complete** — `7606eb6`. Steps are ticked; read them for context, do not re-run them.

The level table everything else hangs off: chunk size, chunk origin, which chunk contains a point, parent/child, projected-area threshold, which chunks an edit dirties, and the world's root range.

**Files:**
- Create: `extension/src/lod/lod_grid.h`, `extension/src/lod/lod_grid.cpp`
- Create: `extension/tests/test_lod_grid.cpp`
- Modify: `extension/SConstruct` (both the shared-library glob and `pure_sources`)

**Interfaces:**
- Consumes: `ve::IVec3`, `ve::WorldBounds`, `ve::floor_div`, `ve::kBrickSize`, `ve::kRegionSize` (`world/region.h`); `ve::EditOp`, `ve::op_world_aabb` (`generator/edit_ops.h`).
- Produces: `ve::kLodLevels`, `ve::kLodChunkCells`, `ve::kLodChunkLattice`, `ve::kLodChunkMeshCells`, `ve::kLodFineLattice`, `ve::kLodBaseCell`, `ve::kLodTargetCellPx`, `ve::kLodSseAreaThresh`, `ve::kLodFadeStartM`, `ve::kLodFadeEndM`, `ve::kLodResidentLevelFrom`; `float ve::lod_cell_size(int)`, `float ve::lod_chunk_size(int)`, `ve::IVec3 ve::lod_chunk_of_point(int, float, float, float)`, `void ve::lod_chunk_origin(int, IVec3, float[3])`, `void ve::lod_chunk_aabb(int, IVec3, float[3], float[3])`, `ve::IVec3 ve::lod_parent(IVec3)`, `ve::IVec3 ve::lod_child_base(IVec3)`, `float ve::lod_chunk_distance(int, IVec3, const float[3])`, `float ve::lod_chunk_far_distance(int, IVec3, const float[3])`, `bool ve::lod_chunk_in_bounds(const WorldBounds &, int, IVec3)`, `void ve::lod_root_range(const WorldBounds &, IVec3 *, IVec3 *)`, `void ve::op_lod_chunk_range(const EditOp &, int, IVec3 *, IVec3 *)`.

- [x] **Step 1: Write the failing test**

Create `extension/tests/test_lod_grid.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_grid.h"
#include <cmath>

// Spec section 2's table, pinned. If these drift the mesher silently samples the wrong
// lattice and every downstream number (memory, range, page counts) is wrong with it.
TEST_CASE("the level table matches spec section 2") {
	CHECK(ve::kLodLevels == 8);
	CHECK(ve::kLodChunkCells == 32);
	CHECK(ve::kLodChunkLattice == 34);
	CHECK(ve::kLodChunkMeshCells == 33);
	CHECK(ve::kLodFineLattice == 69);
	CHECK(ve::lod_cell_size(0) == doctest::Approx(0.4f));
	CHECK(ve::lod_cell_size(1) == doctest::Approx(0.8f));
	CHECK(ve::lod_cell_size(4) == doctest::Approx(6.4f));
	CHECK(ve::lod_cell_size(7) == doctest::Approx(51.2f));
	CHECK(ve::lod_chunk_size(0) == doctest::Approx(12.8f));
	CHECK(ve::lod_chunk_size(7) == doctest::Approx(1638.4f));
	// Every level is exactly twice the one below.
	for (int l = 1; l < ve::kLodLevels; l++)
		CHECK(ve::lod_cell_size(l) == doctest::Approx(2.0f * ve::lod_cell_size(l - 1)));
}

// The descend threshold is stated once, in terms of the per-cell pixel error, so section 2's
// distance column and section 6.1's walk can never disagree about what "3 px" means.
TEST_CASE("the descend threshold is the per-cell error squared over a chunk") {
	CHECK(ve::kLodTargetCellPx == doctest::Approx(3.0f));
	CHECK(ve::kLodSseAreaThresh ==
			doctest::Approx(float(ve::kLodChunkCells) * ve::kLodTargetCellPx *
					float(ve::kLodChunkCells) * ve::kLodTargetCellPx));
	CHECK(ve::kLodSseAreaThresh == doctest::Approx(9216.0f));
}

// Chunk coordinates are GLOBAL per level: no origin term anywhere.
TEST_CASE("chunk coordinates are global and floor on negatives") {
	CHECK(ve::lod_chunk_of_point(0, 0.0f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::lod_chunk_of_point(0, 12.79f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::lod_chunk_of_point(0, 12.81f, 0.0f, 0.0f) == ve::IVec3{1, 0, 0});
	CHECK(ve::lod_chunk_of_point(0, -0.01f, 0.0f, 0.0f) == ve::IVec3{-1, 0, 0});
	CHECK(ve::lod_chunk_of_point(2, 51.1f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::lod_chunk_of_point(2, 51.3f, 0.0f, 0.0f) == ve::IVec3{1, 0, 0});
	float o[3];
	ve::lod_chunk_origin(3, {2, -1, 0}, o);
	CHECK(o[0] == doctest::Approx(2.0f * 102.4f));
	CHECK(o[1] == doctest::Approx(-102.4f));
	CHECK(o[2] == doctest::Approx(0.0f));
}

// A parent's eight children tile it exactly, and every child maps back to that parent.
TEST_CASE("parent and child tile each other exactly") {
	const ve::IVec3 p{3, -2, 5};
	const ve::IVec3 base = ve::lod_child_base(p);
	CHECK(base == ve::IVec3{6, -4, 10});
	for (int k = 0; k < 8; k++) {
		const ve::IVec3 c{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)};
		CHECK(ve::lod_parent(c) == p);
	}
	CHECK(ve::lod_parent({-1, -1, -1}) == ve::IVec3{-1, -1, -1});
	CHECK(ve::lod_parent({-2, -2, -2}) == ve::IVec3{-1, -1, -1});
}

TEST_CASE("chunk distance is zero inside and grows outside") {
	const float c0[3] = {1.0f, 1.0f, 1.0f};
	CHECK(ve::lod_chunk_distance(0, {0, 0, 0}, c0) == doctest::Approx(0.0f));
	const float c1[3] = {-10.0f, 1.0f, 1.0f};
	CHECK(ve::lod_chunk_distance(0, {0, 0, 0}, c1) == doctest::Approx(10.0f));
	// The far distance is to the FARTHEST corner: it is what decides "entirely inside the
	// near field", where building the chunk would burn pages to draw nothing.
	const float o[3] = {0.0f, 0.0f, 0.0f};
	CHECK(ve::lod_chunk_far_distance(0, {0, 0, 0}, o) ==
			doctest::Approx(std::sqrt(3.0f) * 12.8f));
}

// An op must dirty every chunk whose stored quads it can move. Brute force over a
// neighbourhood: any chunk holding a lattice sample the op's AABB reaches must be inside
// the reported range.
TEST_CASE("op_lod_chunk_range covers every chunk the op can change") {
	ve::EditOp op;
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 20.0f; op.pos[1] = 51.0f; op.pos[2] = -3.0f;
	op.radius = 4.0f;
	for (int level = 0; level < ve::kLodLevels; level++) {
		ve::IVec3 lo{}, hi{};
		ve::op_lod_chunk_range(op, level, &lo, &hi);
		float olo[3], ohi[3];
		ve::op_world_aabb(op, olo, ohi);
		const float cell = ve::lod_cell_size(level);
		// Every sample position within one cell of the op's AABB belongs to a chunk in range.
		for (float z = olo[2] - cell; z <= ohi[2] + cell; z += cell * 0.5f)
			for (float y = olo[1] - cell; y <= ohi[1] + cell; y += cell * 0.5f)
				for (float x = olo[0] - cell; x <= ohi[0] + cell; x += cell * 0.5f) {
					const ve::IVec3 c = ve::lod_chunk_of_point(level, x, y, z);
					CHECK(c.x >= lo.x); CHECK(c.x <= hi.x);
					CHECK(c.y >= lo.y); CHECK(c.y <= hi.y);
					CHECK(c.z >= lo.z); CHECK(c.z <= hi.z);
				}
	}
}

// The demo world is 64x8x64 regions = 1638.4 x 204.8 x 1638.4 m, so one L7 root covers it
// on x/z. The default 4096 m world needs 3. Both must come out of the same function.
TEST_CASE("root range covers the world bounds") {
	ve::WorldBounds b;
	b.origin_bricks = {0, -64, 0};
	b.size_regions = {160, 40, 160}; // 4096 x 1024 x 4096 m
	ve::IVec3 lo{}, hi{};
	ve::lod_root_range(b, &lo, &hi);
	float wlo[3], whi[3];
	b.aabb(wlo, whi);
	float clo[3], chi[3];
	ve::lod_chunk_aabb(ve::kLodLevels - 1, lo, clo, chi);
	CHECK(clo[0] <= wlo[0]);
	CHECK(clo[1] <= wlo[1]);
	CHECK(clo[2] <= wlo[2]);
	ve::lod_chunk_aabb(ve::kLodLevels - 1, hi, clo, chi);
	CHECK(chi[0] >= whi[0]);
	CHECK(chi[1] >= whi[1]);
	CHECK(chi[2] >= whi[2]);
	CHECK(ve::lod_chunk_in_bounds(b, ve::kLodLevels - 1, lo));
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `lod/lod_grid.h: No such file or directory`.

- [x] **Step 3: Write the header**

Create `extension/src/lod/lod_grid.h`:

```cpp
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
```

- [x] **Step 4: Write the implementation**

Create `extension/src/lod/lod_grid.cpp`:

```cpp
#include "lod/lod_grid.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {
int clamp_level(int level) { return std::max(0, std::min(level, kLodLevels - 1)); }
} // namespace

float lod_cell_size(int level) {
	return kLodBaseCell * static_cast<float>(1 << clamp_level(level));
}

float lod_chunk_size(int level) {
	return lod_cell_size(level) * static_cast<float>(kLodChunkCells);
}

IVec3 lod_chunk_of_point(int level, float x, float y, float z) {
	const float s = lod_chunk_size(level);
	return IVec3{static_cast<int>(std::floor(x / s)), static_cast<int>(std::floor(y / s)),
			static_cast<int>(std::floor(z / s))};
}

void lod_chunk_origin(int level, IVec3 c, float out[3]) {
	const float s = lod_chunk_size(level);
	out[0] = static_cast<float>(c.x) * s;
	out[1] = static_cast<float>(c.y) * s;
	out[2] = static_cast<float>(c.z) * s;
}

void lod_chunk_aabb(int level, IVec3 c, float lo[3], float hi[3]) {
	lod_chunk_origin(level, c, lo);
	const float s = lod_chunk_size(level);
	for (int a = 0; a < 3; a++) hi[a] = lo[a] + s;
}

IVec3 lod_parent(IVec3 c) {
	return IVec3{floor_div(c.x, 2), floor_div(c.y, 2), floor_div(c.z, 2)};
}

IVec3 lod_child_base(IVec3 c) { return IVec3{c.x * 2, c.y * 2, c.z * 2}; }

float lod_chunk_distance(int level, IVec3 c, const float p[3]) {
	float lo[3], hi[3];
	lod_chunk_aabb(level, c, lo, hi);
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float d = std::max(std::max(lo[a] - p[a], p[a] - hi[a]), 0.0f);
		d2 += d * d;
	}
	return std::sqrt(d2);
}

float lod_chunk_far_distance(int level, IVec3 c, const float p[3]) {
	float lo[3], hi[3];
	lod_chunk_aabb(level, c, lo, hi);
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float d = std::max(std::fabs(lo[a] - p[a]), std::fabs(hi[a] - p[a]));
		d2 += d * d;
	}
	return std::sqrt(d2);
}

bool lod_chunk_in_bounds(const WorldBounds &b, int level, IVec3 c) {
	float wlo[3], whi[3];
	b.aabb(wlo, whi);
	float clo[3], chi[3];
	lod_chunk_aabb(level, c, clo, chi);
	for (int a = 0; a < 3; a++) {
		if (chi[a] <= wlo[a] || clo[a] >= whi[a]) return false;
	}
	return true;
}

void lod_root_range(const WorldBounds &b, IVec3 *lo, IVec3 *hi) {
	float wlo[3], whi[3];
	b.aabb(wlo, whi);
	const int top = kLodLevels - 1;
	*lo = lod_chunk_of_point(top, wlo[0], wlo[1], wlo[2]);
	// The world's maximum corner is exclusive; nudge inside so a world whose extent lands
	// exactly on a chunk boundary does not claim an extra empty root on every axis.
	const float e = lod_chunk_size(top) * 1e-4f;
	*hi = lod_chunk_of_point(top, whi[0] - e, whi[1] - e, whi[2] - e);
}

void op_lod_chunk_range(const EditOp &op, int level, IVec3 *lo, IVec3 *hi) {
	float olo[3], ohi[3];
	op_world_aabb(op, olo, ohi);
	const float pad = 2.0f * lod_cell_size(level);
	const float p0[3] = {olo[0] - pad, olo[1] - pad, olo[2] - pad};
	const float p1[3] = {ohi[0] + pad, ohi[1] + pad, ohi[2] + pad};
	*lo = lod_chunk_of_point(level, p0[0], p0[1], p0[2]);
	*hi = lod_chunk_of_point(level, p1[0], p1[1], p1[2]);
}

} // namespace ve
```

- [x] **Step 5: Add `lod/` to both builds**

In `extension/SConstruct`, change the shared-library glob (line 7) — it already picks up `src/*/*.cpp`, so no change is needed there. Change `pure_sources` so the native test runner compiles the new directory:

```python
pure_sources = (Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") +
                Glob("src/mesh/*.cpp") + Glob("src/connectivity/*.cpp") +
                Glob("src/lod/*.cpp"))
```

- [x] **Step 6: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS, all `test_lod_grid.cpp` cases green, every previously passing case still green.

- [x] **Step 7: Build the extension to confirm `lod/` links**

Run: `./build.sh -j$(nproc)`
Expected: `==> Build OK`.

- [x] **Step 8: Commit**

```bash
git add extension/src/lod/lod_grid.h extension/src/lod/lod_grid.cpp \
        extension/tests/test_lod_grid.cpp extension/SConstruct
git commit -m "feat: lod level table and chunk math"
```

---

### Task 2: `lod/lod_quad` — the 12-byte record

**Status: complete** — `43d9917`, corrected by `b2fb767`. Steps are ticked; read them for context, do not re-run them.

The whole memory argument lives here. A surface-nets quad's four vertices always lie in the four cells sharing one active lattice edge, and those four cells share their coordinate along the edge axis (`shaders/mesh_quads.comp.glsl:41-50`), so nothing needs an absolute coordinate.

**Files:**
- Create: `extension/src/lod/lod_quad.h`, `extension/src/lod/lod_quad.cpp`
- Create: `extension/tests/test_lod_quad.cpp`

**Interfaces:**
- Consumes: `ve::kLodChunkCells`, `ve::lod_cell_size` (`lod/lod_grid.h`).
- Produces: `ve::LodQuad` (12 bytes, `static_assert`), `ve::kLodQuadBytes`, `ve::kLodOffsetBits`, `ve::kLodOffsetMax`, `ve::kLodQuadCorners` (the `QUAD[4]` table); `void ve::lod_quad_pack(const LodQuadFields &, LodQuad *)`, `void ve::lod_quad_unpack(const LodQuad &, LodQuadFields *)`, `void ve::lod_quad_corner_cell(const LodQuadFields &, int, int m[3])`, `void ve::lod_quad_corner_pos(const LodQuadFields &, int, const float origin[3], float cell, float out[3])`.

- [x] **Step 1: Write the failing test**

Create `extension/tests/test_lod_quad.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_quad.h"
#include "lod/lod_grid.h"
#include <cmath>

TEST_CASE("the record is exactly twelve bytes") {
	CHECK(sizeof(ve::LodQuad) == 12);
	CHECK(ve::kLodQuadBytes == 12);
	CHECK(ve::kLodOffsetBits == 5);
	CHECK(ve::kLodOffsetMax == 31);
}

// Every field survives a round trip at its extremes. The layout spans three uint32s, so the
// fields that straddle a word boundary (the corner offsets, the material) are the ones that
// a naive shift would silently truncate.
TEST_CASE("every field round-trips at its extremes") {
	ve::LodQuadFields f{};
	f.u[0] = 31; f.u[1] = 0; f.u[2] = 17;
	f.axis = 2;
	f.sign = 1;
	f.material = 0xBEEF;
	f.double_sided = 1;
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++) f.offset[k][a] = static_cast<uint8_t>((k * 3 + a) % 32);
	f.offset[0][0] = 0;
	f.offset[3][2] = 31;

	ve::LodQuad q{};
	ve::lod_quad_pack(f, &q);
	ve::LodQuadFields g{};
	ve::lod_quad_unpack(q, &g);

	CHECK(g.u[0] == f.u[0]);
	CHECK(g.u[1] == f.u[1]);
	CHECK(g.u[2] == f.u[2]);
	CHECK(g.axis == f.axis);
	CHECK(g.sign == f.sign);
	CHECK(g.material == f.material);
	CHECK(g.double_sided == f.double_sided);
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++) CHECK(g.offset[k][a] == f.offset[k][a]);
}

// An exhaustive sweep of each field on its own, so a wrong shift cannot hide behind another
// field's bits happening to be zero.
TEST_CASE("fields do not bleed into each other") {
	for (int axis = 0; axis < 3; axis++) {
		for (int v = 0; v < 32; v++) {
			ve::LodQuadFields f{};
			f.axis = static_cast<uint8_t>(axis);
			f.u[0] = static_cast<uint8_t>(v);
			f.offset[2][1] = static_cast<uint8_t>(31 - v);
			f.material = static_cast<uint16_t>(v * 2111u);
			ve::LodQuad q{};
			ve::lod_quad_pack(f, &q);
			ve::LodQuadFields g{};
			ve::lod_quad_unpack(q, &g);
			CHECK(g.axis == f.axis);
			CHECK(g.u[0] == f.u[0]);
			CHECK(g.u[1] == 0);
			CHECK(g.u[2] == 0);
			CHECK(g.offset[2][1] == f.offset[2][1]);
			CHECK(g.offset[2][0] == 0);
			CHECK(g.material == f.material);
			CHECK(g.sign == 0);
			CHECK(g.double_sided == 0);
		}
	}
}

// The four cells around an edge share their coordinate along the edge axis and differ by
// -1/0 in the two perpendicular ones. That is what makes 5-bit offsets sufficient.
TEST_CASE("the four corner cells occupy a 1x2x2 block around the owned edge") {
	for (int axis = 0; axis < 3; axis++) {
		ve::LodQuadFields f{};
		f.axis = static_cast<uint8_t>(axis);
		f.u[0] = 4; f.u[1] = 7; f.u[2] = 9;
		int seen[4][3];
		for (int k = 0; k < 4; k++) ve::lod_quad_corner_cell(f, k, seen[k]);
		const int L[3] = {f.u[0] + 1, f.u[1] + 1, f.u[2] + 1};
		for (int k = 0; k < 4; k++) {
			CHECK(seen[k][axis] == L[axis]);
			for (int a = 0; a < 3; a++) {
				CHECK(seen[k][a] >= L[a] - 1);
				CHECK(seen[k][a] <= L[a]);
			}
		}
		// The four are distinct.
		for (int i = 0; i < 4; i++)
			for (int j = i + 1; j < 4; j++)
				CHECK(!(seen[i][0] == seen[j][0] && seen[i][1] == seen[j][1] &&
						seen[i][2] == seen[j][2]));
	}
}

// Decoded positions must match the mesher's own vertex formula,
// origin + (m - 1 + frac) * cell (shaders/mesh_cells.comp.glsl), to within the quantiser.
TEST_CASE("corner positions match the mesher formula within the quantiser") {
	const float origin[3] = {12.8f, -25.6f, 3.2f};
	const float cell = ve::lod_cell_size(1);
	ve::LodQuadFields f{};
	f.axis = 1;
	f.u[0] = 3; f.u[1] = 4; f.u[2] = 5;
	f.offset[0][0] = 0;  f.offset[0][1] = 16; f.offset[0][2] = 31;
	f.offset[1][0] = 31; f.offset[1][1] = 0;  f.offset[1][2] = 8;
	f.offset[2][0] = 15; f.offset[2][1] = 15; f.offset[2][2] = 15;
	f.offset[3][0] = 7;  f.offset[3][1] = 24; f.offset[3][2] = 1;
	for (int k = 0; k < 4; k++) {
		int m[3];
		ve::lod_quad_corner_cell(f, k, m);
		float p[3];
		ve::lod_quad_corner_pos(f, k, origin, cell, p);
		for (int a = 0; a < 3; a++) {
			const float frac = float(f.offset[k][a]) / float(ve::kLodOffsetMax);
			const float want = origin[a] + (float(m[a]) - 1.0f + frac) * cell;
			CHECK(p[a] == doctest::Approx(want).epsilon(0.0f).scale(1.0f));
		}
	}
}

// The quantiser's worst case is half a step, and a step is 1/31 of a cell. Spec section 3.1
// claims 1/32 of a cell "at every level"; this pins the real bound so the claim cannot rot.
TEST_CASE("quantisation error is under one thirty-second of a cell") {
	const float step = 1.0f / float(ve::kLodOffsetMax);
	CHECK(0.5f * step < 1.0f / 32.0f);
	for (float frac = 0.0f; frac <= 1.0f; frac += 0.001f) {
		const uint8_t o = ve::lod_quantise_offset(frac);
		CHECK(o <= ve::kLodOffsetMax);
		const float back = float(o) / float(ve::kLodOffsetMax);
		CHECK(std::fabs(back - frac) <= 0.5f * step + 1e-6f);
	}
	// The endpoints are exact, so adjacent quads meeting at a cell corner cannot separate.
	CHECK(ve::lod_quantise_offset(0.0f) == 0);
	CHECK(ve::lod_quantise_offset(1.0f) == ve::kLodOffsetMax);
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `lod/lod_quad.h: No such file or directory`.

- [x] **Step 3: Write the header**

Create `extension/src/lod/lod_quad.h`:

```cpp
#pragma once
#include <cstdint>

namespace ve {

inline constexpr int kLodQuadBytes = 12;
inline constexpr int kLodOffsetBits = 5;
inline constexpr int kLodOffsetMax = (1 << kLodOffsetBits) - 1; // 31

// The four cells around a lattice edge, as offsets in the two axes perpendicular to it,
// wound counter-clockwise seen from +axis. Byte-identical to kQuad in dual_contour.cpp and
// QUAD in shaders/mesh_quads.comp.glsl -- there is exactly one winding convention.
inline constexpr int kLodQuadCorners[4][2] = {{-1, -1}, {0, -1}, {0, 0}, {-1, 0}};

// The packed record. Three uint32s rather than a struct with a uvec3 member, because std430
// pads a uvec3 array element to 16 bytes and would silently make every page a third larger.
// The GLSL side reads `uint v[]` and indexes quad * 3 + k for the same reason.
//
//   bits  0..14   owning edge coordinate u    3 x 5 bits   (u in [0, 32))
//   bits 15..16   edge axis                        2 bits
//   bit  17       sign (solid -> air direction)    1 bit
//   bits 18..77   4 corner offsets            4 x 15 bits  (5 bits/axis, frac = o / 31)
//   bits 78..93   material id                     16 bits
//   bit  94       double-sided (skirt)             1 bit
//   bit  95       spare                            1 bit
struct LodQuad {
	uint32_t w[3] = {0u, 0u, 0u};
};
static_assert(sizeof(LodQuad) == kLodQuadBytes);

struct LodQuadFields {
	uint8_t u[3] = {0, 0, 0};          // owned edge coordinate, [0, kLodChunkCells)
	uint8_t axis = 0;                  // 0, 1 or 2
	uint8_t sign = 0;                  // 1 when the sample at the edge's low end is solid
	uint8_t offset[4][3] = {};         // per corner, per axis, [0, kLodOffsetMax]
	uint16_t material = 0;
	uint8_t double_sided = 0;
};

void lod_quad_pack(const LodQuadFields &f, LodQuad *out);
void lod_quad_unpack(const LodQuad &q, LodQuadFields *out);

// frac in [0, 1] -> [0, kLodOffsetMax]. The endpoints are exact, so two quads whose
// vertices sit on the same cell corner decode to the same position and cannot separate.
uint8_t lod_quantise_offset(float frac);

// Mesh-cell array coordinate of corner k. The four differ by -1/0 in the two axes
// perpendicular to `axis` and share their coordinate along it.
void lod_quad_corner_cell(const LodQuadFields &f, int k, int m[3]);

// World position of corner k: origin + (m - 1 + frac) * cell, the mesher's own formula
// (shaders/mesh_cells.comp.glsl and ve::dual_contour agree on it).
void lod_quad_corner_pos(const LodQuadFields &f, int k, const float origin[3], float cell,
		float out[3]);

} // namespace ve
```

- [x] **Step 4: Write the implementation**

Create `extension/src/lod/lod_quad.cpp`:

```cpp
#include "lod/lod_quad.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

// A field can straddle at most two of the three words (the widest is 16 bits), so two
// masked writes always suffice. Doing it this way rather than by hand-written shifts is
// what makes the 78-bit material offset survive; it is the field a naive layout truncates.
void bits_set(uint32_t *w, int lo, int bits, uint32_t v) {
	const uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
	v &= mask;
	const int word = lo >> 5;
	const int shift = lo & 31;
	w[word] |= v << shift;
	const int spill = shift + bits - 32;
	if (spill > 0) w[word + 1] |= v >> (32 - shift);
}

uint32_t bits_get(const uint32_t *w, int lo, int bits) {
	const uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
	const int word = lo >> 5;
	const int shift = lo & 31;
	uint32_t v = w[word] >> shift;
	const int spill = shift + bits - 32;
	if (spill > 0) v |= w[word + 1] << (32 - shift);
	return v & mask;
}

constexpr int kBitU = 0;
constexpr int kBitAxis = 15;
constexpr int kBitSign = 17;
constexpr int kBitOffset = 18;
constexpr int kBitMaterial = 78;
constexpr int kBitDoubleSided = 94;

} // namespace

void lod_quad_pack(const LodQuadFields &f, LodQuad *out) {
	out->w[0] = out->w[1] = out->w[2] = 0u;
	for (int a = 0; a < 3; a++) bits_set(out->w, kBitU + a * 5, 5, f.u[a]);
	bits_set(out->w, kBitAxis, 2, f.axis);
	bits_set(out->w, kBitSign, 1, f.sign);
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++)
			bits_set(out->w, kBitOffset + (k * 3 + a) * 5, 5, f.offset[k][a]);
	bits_set(out->w, kBitMaterial, 16, f.material);
	bits_set(out->w, kBitDoubleSided, 1, f.double_sided);
}

void lod_quad_unpack(const LodQuad &q, LodQuadFields *out) {
	for (int a = 0; a < 3; a++)
		out->u[a] = static_cast<uint8_t>(bits_get(q.w, kBitU + a * 5, 5));
	out->axis = static_cast<uint8_t>(bits_get(q.w, kBitAxis, 2));
	out->sign = static_cast<uint8_t>(bits_get(q.w, kBitSign, 1));
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++)
			out->offset[k][a] =
					static_cast<uint8_t>(bits_get(q.w, kBitOffset + (k * 3 + a) * 5, 5));
	out->material = static_cast<uint16_t>(bits_get(q.w, kBitMaterial, 16));
	out->double_sided = static_cast<uint8_t>(bits_get(q.w, kBitDoubleSided, 1));
}

uint8_t lod_quantise_offset(float frac) {
	const float c = std::max(0.0f, std::min(frac, 1.0f));
	return static_cast<uint8_t>(std::floor(c * float(kLodOffsetMax) + 0.5f));
}

void lod_quad_corner_cell(const LodQuadFields &f, int k, int m[3]) {
	const int axis = f.axis % 3;
	const int b = (axis + 1) % 3;
	const int c = (axis + 2) % 3;
	m[0] = f.u[0] + 1;
	m[1] = f.u[1] + 1;
	m[2] = f.u[2] + 1;
	m[b] += kLodQuadCorners[k & 3][0];
	m[c] += kLodQuadCorners[k & 3][1];
}

void lod_quad_corner_pos(const LodQuadFields &f, int k, const float origin[3], float cell,
		float out[3]) {
	int m[3];
	lod_quad_corner_cell(f, k, m);
	for (int a = 0; a < 3; a++) {
		const float frac = static_cast<float>(f.offset[k & 3][a]) / static_cast<float>(kLodOffsetMax);
		out[a] = origin[a] + (static_cast<float>(m[a]) - 1.0f + frac) * cell;
	}
}

} // namespace ve
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add extension/src/lod/lod_quad.h extension/src/lod/lod_quad.cpp \
        extension/tests/test_lod_quad.cpp
git commit -m "feat: twelve-byte lod quad record"
```

---

### Task 3: `lod/lod_reduce` — the mip cascade

**Status: complete** — `7f00d1b`, test re-baselined in `87c4a87`. Steps are ticked; read them for context, do not re-run them.

Spec §4. This is the task that replaces the engine spec's "no downsampling cascade anywhere". The rules are asymmetric on purpose: the SDF averages, the material votes.

**Files:**
- Create: `extension/src/lod/lod_reduce.h`, `extension/src/lod/lod_reduce.cpp`
- Create: `extension/tests/test_lod_reduce.cpp`

**Interfaces:**
- Consumes: `ve::kLodChunkLattice`, `ve::kLodFineLattice` (`lod/lod_grid.h`); `ve::decode_sdf`, `ve::encode_sdf` (`world/brick.h`).
- Produces: `ve::kLodTentWeights`, `int ve::lod_fine_index(int, int, int)`, `int ve::lod_lattice_index(int, int, int)`, `float ve::lod_fine_local(int j)`, `void ve::lod_reduce_lattice(const uint8_t *fine_sdf, const uint16_t *fine_mat, uint8_t *out_sdf, uint16_t *out_mat)`.

- [x] **Step 1: Write the failing test**

Create `extension/tests/test_lod_reduce.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_reduce.h"
#include "lod/lod_grid.h"
#include "world/brick.h"
#include <cmath>
#include <vector>

namespace {
// Fills a fine lattice from an analytic function of the LOCAL coordinate, so a test can
// state exactly what it expects the reduction to produce.
void fill_fine(std::vector<uint8_t> *sdf, std::vector<uint16_t> *mat,
		float (*fn)(float, float, float), uint16_t solid_mat) {
	const int n = ve::kLodFineLattice;
	sdf->assign(size_t(n) * n * n, 0);
	mat->assign(size_t(n) * n * n, 0);
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				const float d = fn(ve::lod_fine_local(x), ve::lod_fine_local(y),
						ve::lod_fine_local(z));
				const int i = ve::lod_fine_index(x, y, z);
				(*sdf)[i] = ve::encode_sdf(d);
				(*mat)[i] = d <= 0.0f ? solid_mat : uint16_t(0);
			}
}
} // namespace

// Fine sample j sits at local coordinate (j - 3) / 2, so j = 3 is the chunk origin and the
// target lattice index i reads fine indices 2i, 2i+1, 2i+2.
TEST_CASE("the fine lattice is addressed in half cells") {
	CHECK(ve::lod_fine_local(3) == doctest::Approx(0.0f));
	CHECK(ve::lod_fine_local(1) == doctest::Approx(-1.0f));
	CHECK(ve::lod_fine_local(5) == doctest::Approx(1.0f));
	CHECK(ve::lod_fine_local(4) == doctest::Approx(0.5f));
	// Target index i's tent is centred on 2i+1, which is local coordinate i - 1 -- the
	// mesher's convention (lattice index i holds the sample at local coordinate i - 1).
	for (int i = 0; i < ve::kLodChunkLattice; i++)
		CHECK(ve::lod_fine_local(2 * i + 1) == doctest::Approx(float(i - 1)));
	// The tent for the last target index must stay inside the fine array.
	CHECK(2 * (ve::kLodChunkLattice - 1) + 2 == ve::kLodFineLattice - 1);
}

TEST_CASE("the tent weights are a quarter, a half, a quarter") {
	CHECK(ve::kLodTentWeights[0] == doctest::Approx(0.25f));
	CHECK(ve::kLodTentWeights[1] == doctest::Approx(0.5f));
	CHECK(ve::kLodTentWeights[2] == doctest::Approx(0.25f));
	CHECK(ve::kLodTentWeights[0] + ve::kLodTentWeights[1] + ve::kLodTentWeights[2] ==
			doctest::Approx(1.0f));
}

// A plane is its own average: a linear field must survive the reduction untouched, which is
// what keeps flat ground flat at every level instead of rippling.
TEST_CASE("a linear field reduces to itself") {
	std::vector<uint8_t> fs;
	std::vector<uint16_t> fm;
	fill_fine(&fs, &fm, [](float x, float y, float z) { (void)x; (void)z; return y * 0.4f; }, 2);
	std::vector<uint8_t> os(size_t(ve::kLodChunkLattice) * ve::kLodChunkLattice *
			ve::kLodChunkLattice);
	std::vector<uint16_t> om(os.size());
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());
	for (int z = 1; z < ve::kLodChunkLattice - 1; z++)
		for (int y = 1; y < ve::kLodChunkLattice - 1; y++)
			for (int x = 1; x < ve::kLodChunkLattice - 1; x++) {
				const float want = float(y - 1) * 0.4f;
				const float got = ve::decode_sdf(os[ve::lod_lattice_index(x, y, z)]);
				// One encoded step of slack: encode/decode is 8-bit.
				CHECK(std::fabs(got - want) <= 2.0f * ve::kSdfRange / 255.0f + 1e-5f);
			}
}

// The reduction is SYMMETRIC. This is the property that makes a crater survive to L4 and is
// exactly where Voxy's solid-preferring Mipper would be wrong for a destruction demo: a min
// reduction erases air pockets, a max reduction erases spires, an average keeps both.
TEST_CASE("a dent and a bump of equal size survive equally") {
	const int n = ve::kLodFineLattice;
	std::vector<uint8_t> fs(size_t(n) * n * n, ve::encode_sdf(0.3f));
	std::vector<uint16_t> fm(fs.size(), 0);
	// A solid slab in the lower half, with one half-cell bump up and one half-cell dent down.
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				float d = ve::lod_fine_local(y);
				if (x == 20 && z == 20) d -= 0.5f; // bump: more solid
				if (x == 40 && z == 40) d += 0.5f; // dent: more air
				fs[ve::lod_fine_index(x, y, z)] = ve::encode_sdf(d * 0.4f);
				fm[ve::lod_fine_index(x, y, z)] = d <= 0.0f ? uint16_t(2) : uint16_t(0);
			}
	std::vector<uint8_t> os(size_t(ve::kLodChunkLattice) * ve::kLodChunkLattice *
			ve::kLodChunkLattice);
	std::vector<uint16_t> om(os.size());
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());

	// Sample the reduced field where the bump and the dent landed, against the undisturbed
	// column between them. The two deviations must have equal magnitude and opposite sign.
	const int by = 10;
	const float base = ve::decode_sdf(os[ve::lod_lattice_index(15, by, 15)]);
	const float bump = ve::decode_sdf(os[ve::lod_lattice_index(10, by, 10)]);
	const float dent = ve::decode_sdf(os[ve::lod_lattice_index(20, by, 20)]);
	CHECK(bump <= base);
	CHECK(dent >= base);
	CHECK(std::fabs((base - bump) - (dent - base)) <= 2.0f * ve::kSdfRange / 255.0f + 1e-5f);
}

// Material is a LABEL: blending two labels is meaningless, so the rule is a vote over the
// solid taps, weighted by the same tent. This is Voxy's Mipper rule (prefer the material
// that actually occupies the volume), and it is what stops distant material boundaries
// dissolving into noise.
TEST_CASE("material is a solidity-weighted majority, never a blend") {
	const int n = ve::kLodFineLattice;
	std::vector<uint8_t> fs(size_t(n) * n * n, ve::encode_sdf(-0.2f)); // all solid
	std::vector<uint16_t> fm(fs.size(), 0);
	// Everything is material 2 except a single tap of material 7 -- one vote cannot win.
	for (size_t i = 0; i < fm.size(); i++) fm[i] = 2;
	fm[ve::lod_fine_index(21, 21, 21)] = 7;
	std::vector<uint8_t> os(size_t(ve::kLodChunkLattice) * ve::kLodChunkLattice *
			ve::kLodChunkLattice);
	std::vector<uint16_t> om(os.size());
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());
	CHECK(om[ve::lod_lattice_index(10, 10, 10)] == 2);
	// Never an average of the two ids.
	for (size_t i = 0; i < om.size(); i++) CHECK((om[i] == 2 || om[i] == 7 || om[i] == 0));

	// A majority of 7 in one neighbourhood does win.
	for (int dz = 0; dz < 3; dz++)
		for (int dy = 0; dy < 3; dy++)
			for (int dx = 0; dx < 3; dx++)
				fm[ve::lod_fine_index(20 + dx, 20 + dy, 20 + dz)] = 7;
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());
	CHECK(om[ve::lod_lattice_index(10, 10, 10)] == 7);
}

// An all-air neighbourhood has no solid taps to vote, and material 0 IS air.
TEST_CASE("air reduces to air") {
	const int n = ve::kLodFineLattice;
	std::vector<uint8_t> fs(size_t(n) * n * n, ve::encode_sdf(0.5f));
	std::vector<uint16_t> fm(fs.size(), 0);
	std::vector<uint8_t> os(size_t(ve::kLodChunkLattice) * ve::kLodChunkLattice *
			ve::kLodChunkLattice);
	std::vector<uint16_t> om(os.size());
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());
	for (size_t i = 0; i < om.size(); i++) CHECK(om[i] == 0);
	for (size_t i = 0; i < os.size(); i++) CHECK(ve::decode_sdf(os[i]) > 0.0f);
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `lod/lod_reduce.h: No such file or directory`.

If `ve::kSdfRange` or `ve::encode_sdf` do not exist under those names, read `extension/src/world/brick.h` and use the names it actually declares; the test's only requirement is "one encoded step of slack".

- [x] **Step 3: Write the header**

Create `extension/src/lod/lod_reduce.h`:

```cpp
#pragma once
#include <cstdint>

namespace ve {

// Separable tent. Spec section 4: the SDF AVERAGES. Voxy's Mipper prefers non-air because
// block data is binary and has no mean; an SDF has one, and an average is symmetric -- it
// preserves craters and spires equally. A solid-preferring min would erase the player's
// craters at distance, which is the wrong failure mode for a destruction demo.
inline constexpr float kLodTentWeights[3] = {0.25f, 0.5f, 0.25f};

// Fine sample j sits at local coordinate (j - 3) / 2 in cells, so j = 3 is the chunk origin,
// and target lattice index i (holding local coordinate i - 1) is centred on fine index
// 2i + 1 with its tent covering 2i, 2i+1, 2i+2.
float lod_fine_local(int j);

int lod_fine_index(int x, int y, int z);     // kLodFineLattice^3, x fastest
int lod_lattice_index(int x, int y, int z);  // kLodChunkLattice^3, x fastest

// fine_sdf/fine_mat are kLodFineLattice^3; out_sdf/out_mat are kLodChunkLattice^3.
// SDF: tent average of the 27 taps. Material: tent-weighted majority over the SOLID taps
// only, ties broken by the centre tap; all-air reduces to material 0.
void lod_reduce_lattice(const uint8_t *fine_sdf, const uint16_t *fine_mat, uint8_t *out_sdf,
		uint16_t *out_mat);

} // namespace ve
```

- [x] **Step 4: Write the implementation**

Create `extension/src/lod/lod_reduce.cpp`:

```cpp
#include "lod/lod_reduce.h"
#include "lod/lod_grid.h"
#include "world/brick.h"
#include <algorithm>
#include <vector>

namespace ve {

float lod_fine_local(int j) { return (static_cast<float>(j) - 3.0f) * 0.5f; }

int lod_fine_index(int x, int y, int z) {
	return x + y * kLodFineLattice + z * kLodFineLattice * kLodFineLattice;
}

int lod_lattice_index(int x, int y, int z) {
	return x + y * kLodChunkLattice + z * kLodChunkLattice * kLodChunkLattice;
}

void lod_reduce_lattice(const uint8_t *fine_sdf, const uint16_t *fine_mat, uint8_t *out_sdf,
		uint16_t *out_mat) {
	// At most 27 distinct ids in one neighbourhood, so a linear scan beats a map.
	uint16_t ids[27];
	float votes[27];
	for (int z = 0; z < kLodChunkLattice; z++)
		for (int y = 0; y < kLodChunkLattice; y++)
			for (int x = 0; x < kLodChunkLattice; x++) {
				float acc = 0.0f;
				int n_ids = 0;
				uint16_t centre_mat = 0;
				for (int dz = 0; dz < 3; dz++)
					for (int dy = 0; dy < 3; dy++)
						for (int dx = 0; dx < 3; dx++) {
							const int fi = lod_fine_index(2 * x + dx, 2 * y + dy, 2 * z + dz);
							const float w = kLodTentWeights[dx] * kLodTentWeights[dy] *
									kLodTentWeights[dz];
							const float d = decode_sdf(fine_sdf[fi]);
							acc += w * d;
							if (dx == 1 && dy == 1 && dz == 1) centre_mat = fine_mat[fi];
							// Only solid taps vote: a material id labels matter, and an air
							// tap has no matter to label.
							if (d > 0.0f) continue;
							const uint16_t m = fine_mat[fi];
							if (m == 0u) continue;
							int slot = -1;
							for (int s = 0; s < n_ids; s++)
								if (ids[s] == m) { slot = s; break; }
							if (slot < 0) {
								slot = n_ids++;
								ids[slot] = m;
								votes[slot] = 0.0f;
							}
							votes[slot] += w;
						}
				const int oi = lod_lattice_index(x, y, z);
				out_sdf[oi] = encode_sdf(acc);
				uint16_t best = 0;
				float best_v = 0.0f;
				for (int s = 0; s < n_ids; s++) {
					// Strict >: the first id to reach a weight wins, and the tie-break below
					// then prefers the centre tap. Deterministic on both CPU and GPU.
					if (votes[s] > best_v) { best_v = votes[s]; best = ids[s]; }
				}
				if (n_ids > 1) {
					for (int s = 0; s < n_ids; s++)
						if (ids[s] == centre_mat && votes[s] >= best_v) { best = centre_mat; break; }
				}
				out_mat[oi] = best;
			}
}

} // namespace ve
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add extension/src/lod/lod_reduce.h extension/src/lod/lod_reduce.cpp \
        extension/tests/test_lod_reduce.cpp
git commit -m "feat: half-cell tent reduction with material vote"
```

---

### Task 4: `lod/lod_contour` — the CPU reference mesher

**Status: complete** — `33fc3bd`. Steps are ticked; read them for context, do not re-run them.

The GPU quads pass needs something to be diffed against (engine spec §8's GPU differential testing). This is `ve::dual_contour` with packed quads instead of triangle indices, sharing its tables so the two can never disagree about winding.

**Files:**
- Create: `extension/src/lod/lod_contour.h`, `extension/src/lod/lod_contour.cpp`
- Create: `extension/tests/test_lod_contour.cpp`

**Interfaces:**
- Consumes: `ve::LodQuad`, `ve::LodQuadFields`, `ve::lod_quantise_offset`, `ve::kLodQuadCorners` (`lod/lod_quad.h`); `ve::kLodChunkLattice`, `ve::kLodChunkMeshCells`, `ve::kLodChunkCells` (`lod/lod_grid.h`); `ve::decode_sdf` (`world/brick.h`).
- Produces: `ve::LodContourResult`, `void ve::lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out)`.

- [x] **Step 1: Write the failing test**

Create `extension/tests/test_lod_contour.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_quad.h"
#include "mesh/dual_contour.h"
#include "world/brick.h"
#include <cmath>
#include <vector>

namespace {
std::vector<uint8_t> plane_lattice(float height_cells) {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n);
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
				l[ve::lod_lattice_index(x, y, z)] =
						ve::encode_sdf((float(y - 1) - height_cells) * 0.4f);
	return l;
}
} // namespace

TEST_CASE("an all-air lattice produces no quads") {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n, ve::encode_sdf(0.5f));
	std::vector<uint16_t> m(l.size(), 0);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	CHECK(r.quads.empty());
	CHECK(r.overflow == false);
}

// A horizontal plane crosses exactly one lattice edge per column, all along +y, so the quad
// count is the number of owned columns and every quad's axis is 1.
TEST_CASE("a horizontal plane produces one quad per owned column") {
	const std::vector<uint8_t> l = plane_lattice(10.0f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	CHECK(r.quads.size() == size_t(ve::kLodChunkCells) * ve::kLodChunkCells);
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		CHECK(f.axis == 1);
		CHECK(f.material == 2);
	}
}

// The reference must agree with the mesher the collision path already trusts. Run both over
// the SAME lattice and compare the sets of quads, as cell-index quadruples so vertex
// numbering (which ve::dual_contour allocates and lod_contour does not) never enters.
TEST_CASE("lod_contour emits the same surface ve::dual_contour does") {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n);
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				// A lumpy surface, so quads appear on all three axes.
				const float h = 12.0f + 3.0f * std::sin(float(x) * 0.4f) *
						std::cos(float(z) * 0.31f);
				l[ve::lod_lattice_index(x, y, z)] = ve::encode_sdf((float(y - 1) - h) * 0.4f);
			}
	std::vector<uint16_t> m(l.size(), 3);

	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);

	ve::DcGrid g;
	g.lattice = ve::kLodChunkLattice;
	g.cell_size = ve::lod_cell_size(0);
	g.origin[0] = g.origin[1] = g.origin[2] = 0.0f;
	ve::MeshBuffer mb;
	ve::dual_contour(l.data(), g, &mb);

	// ve::dual_contour emits two triangles per quad, so the quad count is half its triangles.
	CHECK(r.quads.size() * 2u == size_t(mb.triangle_count()));

	// Positions must agree to the quantiser: decode every lod_contour corner and find the
	// dual-contour vertex of the same cell.
	const float cell = ve::lod_cell_size(0);
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	const float tol = cell / float(ve::kLodOffsetMax);
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		for (int k = 0; k < 4; k++) {
			int mc[3];
			ve::lod_quad_corner_cell(f, k, mc);
			const int vi = mb.cell_vertex[ve::dc_cell_index(g, mc[0], mc[1], mc[2])];
			REQUIRE(vi >= 0);
			float p[3];
			ve::lod_quad_corner_pos(f, k, origin, cell, p);
			for (int a = 0; a < 3; a++)
				CHECK(std::fabs(p[a] - mb.positions[size_t(vi) * 3 + a]) <= tol);
		}
	}
}

// Corners are stored ALREADY WOUND, so the vertex shader never branches: the first triangle
// of every quad must face away from the solid side.
TEST_CASE("stored corner order already winds toward the air") {
	const std::vector<uint8_t> l = plane_lattice(10.0f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	const float cell = ve::lod_cell_size(0);
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	REQUIRE(!r.quads.empty());
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		float p[3][3];
		for (int k = 0; k < 3; k++) ve::lod_quad_corner_pos(f, k, origin, cell, p[k]);
		const float a[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
		const float b[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
		const float nx = a[1] * b[2] - a[2] * b[1];
		const float ny = a[2] * b[0] - a[0] * b[2];
		const float nz = a[0] * b[1] - a[1] * b[0];
		(void)nx; (void)nz;
		// The plane's solid half is below, so every normal must point up.
		CHECK(ny > 0.0f);
	}
}

TEST_CASE("the cap is reported rather than exceeded") {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n);
	// Alternating slabs: a crossing on nearly every y edge, far past the cap.
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
				l[ve::lod_lattice_index(x, y, z)] = ve::encode_sdf((y & 1) ? 0.2f : -0.2f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	CHECK(r.quads.size() <= size_t(ve::kLodMaxQuadsPerChunk));
	CHECK(r.overflow == true);
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `lod/lod_contour.h: No such file or directory`.

- [x] **Step 3: Write the header**

Create `extension/src/lod/lod_contour.h`:

```cpp
#pragma once
#include "lod/lod_quad.h"
#include <cstdint>
#include <vector>

namespace ve {

// Spec section 3.3: a chunk is capped at 16 pages of 512 quads. A build that overflows keeps
// its first kLodMaxQuadsPerChunk quads and reports it -- engine spec section 8's fail-soft
// rule. It is also what bounds the per-chunk readback at 96 KB.
inline constexpr int kLodQuadsPerPage = 512;
inline constexpr int kLodVertsPerPage = kLodQuadsPerPage * 4; // 2048
inline constexpr int kLodMaxPagesPerChunk = 16;
inline constexpr int kLodMaxQuadsPerChunk = kLodQuadsPerPage * kLodMaxPagesPerChunk; // 8192

struct LodContourResult {
	std::vector<LodQuad> quads;
	bool overflow = false;
};

// Surface nets over a kLodChunkLattice^3 lattice of ENCODED sdf bytes plus a parallel
// material lattice, emitting packed quads. The CPU reference shaders/lod_quads.comp.glsl is
// diffed against, and the source of the skirt pass's input. Solid is decode_sdf(byte) <= 0,
// matching the generator's own rule.
void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out);

} // namespace ve
```

- [x] **Step 4: Write the implementation**

Create `extension/src/lod/lod_contour.cpp`:

```cpp
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_reduce.h"
#include "world/brick.h"

namespace ve {

namespace {

// Cell corners indexed by (x | y<<1 | z<<2), and the cell's 12 edges as corner pairs, in the
// SAME order as kCorner/kEdge in dual_contour.cpp. The order matters: the vertex is a running
// sum over crossings and float addition is not associative, so a different order gives a
// different vertex and the GPU diff fails.
constexpr int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
		{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
constexpr int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
		{0, 2}, {1, 3}, {4, 6}, {5, 7},
		{0, 4}, {1, 5}, {2, 6}, {3, 7}};

int mesh_cell_index(int x, int y, int z) {
	return x + y * kLodChunkMeshCells + z * kLodChunkMeshCells * kLodChunkMeshCells;
}

} // namespace

void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out) {
	out->quads.clear();
	out->overflow = false;

	// Pass 1: the dual vertex of every crossed cell, as a fraction of its own cell. Storing
	// the fraction rather than a world position is what lets the quad record carry it in
	// five bits per axis.
	const size_t cell_count = size_t(kLodChunkMeshCells) * kLodChunkMeshCells * kLodChunkMeshCells;
	std::vector<uint8_t> frac(cell_count * 3, 0);
	std::vector<char> has_vertex(cell_count, 0);
	for (int mz = 0; mz < kLodChunkMeshCells; mz++)
		for (int my = 0; my < kLodChunkMeshCells; my++)
			for (int mx = 0; mx < kLodChunkMeshCells; mx++) {
				float d[8];
				for (int k = 0; k < 8; k++)
					d[k] = decode_sdf(lattice[lod_lattice_index(mx + kCorner[k][0],
							my + kCorner[k][1], mz + kCorner[k][2])]);
				float acc[3] = {0.0f, 0.0f, 0.0f};
				int n = 0;
				for (int e = 0; e < 12; e++) {
					const float da = d[kEdge[e][0]], db = d[kEdge[e][1]];
					if ((da <= 0.0f) == (db <= 0.0f)) continue;
					const float t = da / (da - db);
					for (int a = 0; a < 3; a++)
						acc[a] += float(kCorner[kEdge[e][0]][a]) +
								t * float(kCorner[kEdge[e][1]][a] - kCorner[kEdge[e][0]][a]);
					n++;
				}
				if (n == 0) continue;
				const size_t ci = size_t(mesh_cell_index(mx, my, mz));
				has_vertex[ci] = 1;
				for (int a = 0; a < 3; a++)
					frac[ci * 3 + a] = lod_quantise_offset(acc[a] / float(n));
			}

	// Pass 2: one quad per sign-changing lattice edge this chunk owns -- local edge
	// coordinate u in [0, kLodChunkCells), lattice index u + 1 -- so every edge in the world
	// is emitted by exactly one chunk: no cracks, no duplicates.
	for (int uz = 0; uz < kLodChunkCells; uz++)
		for (int uy = 0; uy < kLodChunkCells; uy++)
			for (int ux = 0; ux < kLodChunkCells; ux++) {
				const int L[3] = {ux + 1, uy + 1, uz + 1};
				const float da = decode_sdf(lattice[lod_lattice_index(L[0], L[1], L[2])]);
				for (int axis = 0; axis < 3; axis++) {
					int Lb[3] = {L[0], L[1], L[2]};
					Lb[axis] += 1;
					const float db = decode_sdf(lattice[lod_lattice_index(Lb[0], Lb[1], Lb[2])]);
					const bool sa = da <= 0.0f, sb = db <= 0.0f;
					if (sa == sb) continue;
					if (int(out->quads.size()) >= kLodMaxQuadsPerChunk) {
						out->overflow = true;
						return;
					}
					const int b = (axis + 1) % 3, c = (axis + 2) % 3;
					size_t ci[4];
					bool ok = true;
					for (int k = 0; k < 4; k++) {
						int m[3] = {L[0], L[1], L[2]};
						m[b] += kLodQuadCorners[k][0];
						m[c] += kLodQuadCorners[k][1];
						ci[k] = size_t(mesh_cell_index(m[0], m[1], m[2]));
						if (!has_vertex[ci[k]]) ok = false;
					}
					// Unreachable on the CPU (a crossed edge crosses all four of its cells),
					// but the GPU can lose a vertex to a cap, and both sides run the same
					// rule so the diff stays honest.
					if (!ok) continue;

					LodQuadFields f{};
					f.u[0] = uint8_t(ux); f.u[1] = uint8_t(uy); f.u[2] = uint8_t(uz);
					f.axis = uint8_t(axis);
					f.sign = sa ? 1 : 0;
					// The material of the SOLID endpoint of the edge: deterministic, and
					// mirrorable in one line of GLSL.
					f.material = material[lod_lattice_index(sa ? L[0] : Lb[0], sa ? L[1] : Lb[1],
							sa ? L[2] : Lb[2])];
					// Store the corners ALREADY WOUND. (axis, b, c) is a right-handed cycle,
					// so c0..c3 wind counter-clockwise seen from +axis; solid -> air along
					// +axis puts the air on the +axis side, which is the side the normal must
					// face. The reversed case stores (c0, c3, c2, c1), whose two triangles
					// are exactly ve::dual_contour's tri_rev pair.
					const int order_fwd[4] = {0, 1, 2, 3};
					const int order_rev[4] = {0, 3, 2, 1};
					const int *order = sa ? order_fwd : order_rev;
					for (int k = 0; k < 4; k++)
						for (int a = 0; a < 3; a++)
							f.offset[k][a] = frac[ci[order[k]] * 3 + a];
					LodQuad q{};
					lod_quad_pack(f, &q);
					out->quads.push_back(q);
				}
			}
}

} // namespace ve
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

If "stored corner order already winds toward the air" fails, the `order_rev` permutation is wrong — check it against `ve::dual_contour`'s `tri_rev = {q0, q2, q1, q0, q3, q2}`: with the index pattern `{0,1,2, 0,2,3}` the permutation `(q0, q3, q2, q1)` yields `(q0,q3,q2)` and `(q0,q2,q1)`, the same two triangles in the other order. Do not "fix" it by flipping the index buffer — Task 13 depends on that pattern being fixed.

- [x] **Step 6: Commit**

```bash
git add extension/src/lod/lod_contour.h extension/src/lod/lod_contour.cpp \
        extension/tests/test_lod_contour.cpp
git commit -m "feat: cpu reference surface nets emitting packed lod quads"
```

---

### Task 5: `lod/lod_skirt` — the crack curtains

**Status: complete** — `df18839`, corrected by `cc52dcc` and `0f7d7f2`. Steps are ticked; read them for context, do not re-run them.

Engine spec §4: "Skirts on chunk borders hide inter-level cracks; no stitching meshes." A ratio of 2 bounds the mismatch at one coarse cell, so the curtain is two cells deep.

**Files:**
- Create: `extension/src/lod/lod_skirt.h`, `extension/src/lod/lod_skirt.cpp`
- Create: `extension/tests/test_lod_skirt.cpp`

**Interfaces:**
- Consumes: `ve::LodQuad`, `ve::LodQuadFields`, `ve::lod_quad_corner_cell`, `ve::kLodQuadCorners` (`lod/lod_quad.h`); `ve::kLodChunkCells`, `ve::kLodMaxQuadsPerChunk` (`lod/lod_grid.h`, `lod/lod_contour.h`).
- Produces: `ve::kLodSkirtCells`, `int ve::lod_append_skirts(std::vector<LodQuad> *quads)`.

- [x] **Step 1: Write the failing test**

Create `extension/tests/test_lod_skirt.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_skirt.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_quad.h"
#include "world/brick.h"
#include <vector>

namespace {
std::vector<uint8_t> plane_lattice(float height_cells) {
	const int n = ve::kLodChunkLattice;
	std::vector<uint8_t> l(size_t(n) * n * n);
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
				l[ve::lod_lattice_index(x, y, z)] =
						ve::encode_sdf((float(y - 1) - height_cells) * 0.4f);
	return l;
}
} // namespace

TEST_CASE("the skirt depth is two cells") { CHECK(ve::kLodSkirtCells == 2); }

// Only quads on the chunk's boundary get a curtain. A plane's boundary quads are the ring
// around its edge: 4 * 32 - 4 of them out of 32 * 32.
TEST_CASE("only boundary quads produce skirts") {
	const std::vector<uint8_t> l = plane_lattice(10.0f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	const size_t surface = r.quads.size();
	const int added = ve::lod_append_skirts(&r.quads);
	// One curtain quad per boundary quad, emitted twice for two-sidedness.
	const int ring = 4 * ve::kLodChunkCells - 4;
	CHECK(added == ring * 2);
	CHECK(r.quads.size() == surface + size_t(added));
}

// Every appended quad carries the double-sided bit, because a crack is looked into from a
// direction nobody can predict.
TEST_CASE("skirt quads are marked double sided and come in opposite-wound pairs") {
	const std::vector<uint8_t> l = plane_lattice(10.0f);
	std::vector<uint16_t> m(l.size(), 2);
	ve::LodContourResult r;
	ve::lod_contour(l.data(), m.data(), &r);
	const size_t surface = r.quads.size();
	ve::lod_append_skirts(&r.quads);
	REQUIRE(r.quads.size() > surface);
	for (size_t i = surface; i < r.quads.size(); i += 2) {
		ve::LodQuadFields a{}, b{};
		ve::lod_quad_unpack(r.quads[i], &a);
		ve::lod_quad_unpack(r.quads[i + 1], &b);
		CHECK(a.double_sided == 1);
		CHECK(b.double_sided == 1);
		// The pair is the same geometry wound the other way: corners 1 and 3 swap.
		for (int x = 0; x < 3; x++) {
			CHECK(b.offset[0][x] == a.offset[0][x]);
			CHECK(b.offset[1][x] == a.offset[3][x]);
			CHECK(b.offset[2][x] == a.offset[2][x]);
			CHECK(b.offset[3][x] == a.offset[1][x]);
		}
	}
}

TEST_CASE("skirts respect the per-chunk cap") {
	std::vector<ve::LodQuad> quads(size_t(ve::kLodMaxQuadsPerChunk) - 3);
	// All at u = 0, so every one of them counts as a boundary quad.
	for (ve::LodQuad &q : quads) {
		ve::LodQuadFields f{};
		f.axis = 1;
		f.u[0] = 0; f.u[1] = 5; f.u[2] = 5;
		ve::lod_quad_pack(f, &q);
	}
	const int added = ve::lod_append_skirts(&quads);
	CHECK(added == 3);
	CHECK(int(quads.size()) == ve::kLodMaxQuadsPerChunk);
}

TEST_CASE("an empty chunk produces no skirts") {
	std::vector<ve::LodQuad> quads;
	CHECK(ve::lod_append_skirts(&quads) == 0);
	CHECK(quads.empty());
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `lod/lod_skirt.h: No such file or directory`.

- [x] **Step 3: Write the header**

Create `extension/src/lod/lod_skirt.h`:

```cpp
#pragma once
#include "lod/lod_quad.h"
#include <vector>

namespace ve {

// A ratio of 2 bounds the level-boundary mismatch at ONE coarse cell, so two cells of
// curtain covers it with margin. (The engine spec's ratio of 4 would have needed four.)
inline constexpr int kLodSkirtCells = 2;

// Appends a curtain for every quad touching a chunk face, returning how many quads were
// added. A boundary quad is one whose owned edge coordinate sits on the first or last cell
// of any axis -- recoverable from the record alone, so there is no fourth GPU pass and no
// cell-map readback. The curtain hangs kLodSkirtCells along the quad's NEGATIVE normal (into
// the solid), which works on cliffs as well as floors where a straight-down skirt does not,
// and it inherits its parent's material so the crack shows the parent's colour.
//
// Each curtain is emitted TWICE with opposite winding rather than needing a second,
// cull-disabled pipeline: skirts are a small fraction of a chunk's quads.
int lod_append_skirts(std::vector<LodQuad> *quads);

} // namespace ve
```

- [x] **Step 4: Write the implementation**

Create `extension/src/lod/lod_skirt.cpp`:

```cpp
#include "lod/lod_skirt.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include <algorithm>

namespace ve {

namespace {

bool is_boundary(const LodQuadFields &f) {
	for (int a = 0; a < 3; a++)
		if (f.u[a] == 0 || f.u[a] == kLodChunkCells - 1) return true;
	return false;
}

} // namespace

int lod_append_skirts(std::vector<LodQuad> *quads) {
	if (!quads || quads->empty()) return 0;
	const size_t surface = quads->size();
	int added = 0;
	for (size_t i = 0; i < surface; i++) {
		if (int(quads->size()) >= kLodMaxQuadsPerChunk) break;
		LodQuadFields f{};
		lod_quad_unpack((*quads)[i], &f);
		if (f.double_sided) continue; // never skirt a skirt
		if (!is_boundary(f)) continue;

		// The curtain shares the parent's four cells and its material; it is displaced along
		// -normal by pushing every corner offset toward the solid side of the owned edge.
		// Because the offsets are cell-relative, "kLodSkirtCells along -normal" is exactly
		// "clamp the offset on the edge axis to the far end of the solid side".
		LodQuadFields s = f;
		s.double_sided = 1;
		const int axis = f.axis % 3;
		const uint8_t pushed = f.sign ? uint8_t(0) : uint8_t(kLodOffsetMax);
		for (int k = 0; k < 4; k++) s.offset[k][axis] = pushed;

		LodQuad a{};
		lod_quad_pack(s, &a);
		quads->push_back(a);
		added++;
		if (int(quads->size()) >= kLodMaxQuadsPerChunk) break;

		LodQuadFields flipped = s;
		for (int x = 0; x < 3; x++) {
			flipped.offset[1][x] = s.offset[3][x];
			flipped.offset[3][x] = s.offset[1][x];
		}
		LodQuad b{};
		lod_quad_pack(flipped, &b);
		quads->push_back(b);
		added++;
	}
	return added;
}

} // namespace ve
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add extension/src/lod/lod_skirt.h extension/src/lod/lod_skirt.cpp \
        extension/tests/test_lod_skirt.cpp
git commit -m "feat: two-sided boundary skirts for lod chunks"
```

---

### Task 6: `lod/lod_arena` — the page allocator

**Status: complete** — `a94f5f8`. Steps are ticked; read them for context, do not re-run them.

Spec §3.3. Draw granularity is the page, so pages need not be contiguous and this is a plain free list — but it must refuse a build it cannot fully fund (M3 errata 5's lesson: a partially funded load is worse than a refused one).

**Files:**
- Create: `extension/src/lod/lod_arena.h`, `extension/src/lod/lod_arena.cpp`
- Create: `extension/tests/test_lod_arena.cpp`

**Interfaces:**
- Consumes: `ve::kLodQuadsPerPage`, `ve::kLodMaxPagesPerChunk` (`lod/lod_contour.h`).
- Produces: `ve::LodArena` with `explicit LodArena(int page_count)`, `bool alloc(int pages, std::vector<int> *out)`, `void release(const std::vector<int> &pages)`, `int free_pages() const`, `int used_pages() const`, `int capacity() const`, `void clear()`; `int ve::lod_pages_for_quads(int quads)`.

- [x] **Step 1: Write the failing test**

Create `extension/tests/test_lod_arena.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_arena.h"
#include "lod/lod_contour.h"
#include <algorithm>
#include <set>
#include <vector>

TEST_CASE("pages needed rounds up and honours the cap") {
	CHECK(ve::lod_pages_for_quads(0) == 0);
	CHECK(ve::lod_pages_for_quads(1) == 1);
	CHECK(ve::lod_pages_for_quads(ve::kLodQuadsPerPage) == 1);
	CHECK(ve::lod_pages_for_quads(ve::kLodQuadsPerPage + 1) == 2);
	CHECK(ve::lod_pages_for_quads(ve::kLodMaxQuadsPerChunk) == ve::kLodMaxPagesPerChunk);
	CHECK(ve::lod_pages_for_quads(ve::kLodMaxQuadsPerChunk * 4) == ve::kLodMaxPagesPerChunk);
}

TEST_CASE("allocations are distinct and accounted") {
	ve::LodArena a(8);
	CHECK(a.capacity() == 8);
	CHECK(a.free_pages() == 8);
	std::vector<int> p1, p2;
	CHECK(a.alloc(3, &p1));
	CHECK(p1.size() == 3);
	CHECK(a.free_pages() == 5);
	CHECK(a.used_pages() == 3);
	CHECK(a.alloc(5, &p2));
	CHECK(a.free_pages() == 0);
	std::set<int> all(p1.begin(), p1.end());
	all.insert(p2.begin(), p2.end());
	CHECK(all.size() == 8);
	for (int v : all) { CHECK(v >= 0); CHECK(v < 8); }
}

// M3 errata 5's rule, restated for pages: an allocation that cannot be fully funded takes
// NOTHING. A half-allocated chunk would be a hole with pages spent on it.
TEST_CASE("an unfundable allocation takes nothing") {
	ve::LodArena a(4);
	std::vector<int> p;
	CHECK(a.alloc(3, &p));
	CHECK(a.free_pages() == 1);
	std::vector<int> q{99, 98};
	CHECK(a.alloc(2, &q) == false);
	CHECK(q.empty());
	CHECK(a.free_pages() == 1);
	// The one remaining page is still allocatable.
	CHECK(a.alloc(1, &q));
	CHECK(q.size() == 1);
	CHECK(a.free_pages() == 0);
}

TEST_CASE("released pages come back and can be reused") {
	ve::LodArena a(4);
	std::vector<int> p;
	REQUIRE(a.alloc(4, &p));
	CHECK(a.free_pages() == 0);
	a.release(p);
	CHECK(a.free_pages() == 4);
	CHECK(a.used_pages() == 0);
	std::vector<int> q;
	CHECK(a.alloc(4, &q));
	std::sort(p.begin(), p.end());
	std::sort(q.begin(), q.end());
	CHECK(p == q);
}

// A double release would hand the same page to two chunks, which draws one chunk's geometry
// with another's origin. It must be inert, not corrupting.
TEST_CASE("a double release is inert") {
	ve::LodArena a(4);
	std::vector<int> p;
	REQUIRE(a.alloc(2, &p));
	a.release(p);
	CHECK(a.free_pages() == 4);
	a.release(p);
	CHECK(a.free_pages() == 4);
	std::vector<int> q;
	CHECK(a.alloc(4, &q));
	std::set<int> uniq(q.begin(), q.end());
	CHECK(uniq.size() == 4);
}

TEST_CASE("out of range releases are ignored") {
	ve::LodArena a(2);
	std::vector<int> bad{-1, 7, 200};
	a.release(bad);
	CHECK(a.free_pages() == 2);
}

TEST_CASE("clear returns every page") {
	ve::LodArena a(16);
	std::vector<int> p;
	REQUIRE(a.alloc(10, &p));
	a.clear();
	CHECK(a.free_pages() == 16);
	CHECK(a.used_pages() == 0);
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `lod/lod_arena.h: No such file or directory`.

- [x] **Step 3: Write the header**

Create `extension/src/lod/lod_arena.h`:

```cpp
#pragma once
#include <vector>

namespace ve {

// Quads -> pages, rounded up and clamped to kLodMaxPagesPerChunk.
int lod_pages_for_quads(int quads);

// The geometry arena's page allocator. Draw granularity is the page (spec section 3.3), so a
// chunk's pages need not be contiguous and there is no suballocator, no fragmentation, and
// no compaction pass.
class LodArena {
public:
	explicit LodArena(int page_count);

	// All-or-nothing: an allocation that cannot be fully funded takes nothing and leaves
	// `out` empty (M3 errata 5 -- a partially funded load is worse than a refused one).
	bool alloc(int pages, std::vector<int> *out);
	void release(const std::vector<int> &pages);

	int free_pages() const { return static_cast<int>(free_.size()); }
	int used_pages() const { return capacity_ - free_pages(); }
	int capacity() const { return capacity_; }
	void clear();

private:
	int capacity_ = 0;
	std::vector<int> free_;
	std::vector<char> used_; // guards double release, which would alias two chunks' geometry
};

} // namespace ve
```

- [x] **Step 4: Write the implementation**

Create `extension/src/lod/lod_arena.cpp`:

```cpp
#include "lod/lod_arena.h"
#include "lod/lod_contour.h"
#include <algorithm>

namespace ve {

int lod_pages_for_quads(int quads) {
	if (quads <= 0) return 0;
	const int pages = (quads + kLodQuadsPerPage - 1) / kLodQuadsPerPage;
	return std::min(pages, kLodMaxPagesPerChunk);
}

LodArena::LodArena(int page_count) : capacity_(std::max(0, page_count)) {
	clear();
}

void LodArena::clear() {
	free_.clear();
	free_.reserve(size_t(capacity_));
	// Descending, so pop_back hands out page 0 first: a fresh world's first chunks land at
	// the front of the buffer, which makes a hex dump of the arena readable.
	for (int i = capacity_ - 1; i >= 0; i--) free_.push_back(i);
	used_.assign(size_t(capacity_), 0);
}

bool LodArena::alloc(int pages, std::vector<int> *out) {
	if (!out) return false;
	out->clear();
	if (pages <= 0) return true;
	if (pages > free_pages()) return false;
	out->reserve(size_t(pages));
	for (int i = 0; i < pages; i++) {
		const int p = free_.back();
		free_.pop_back();
		used_[size_t(p)] = 1;
		out->push_back(p);
	}
	return true;
}

void LodArena::release(const std::vector<int> &pages) {
	for (int p : pages) {
		if (p < 0 || p >= capacity_) continue;
		if (!used_[size_t(p)]) continue; // already free: inert, never a second free-list entry
		used_[size_t(p)] = 0;
		free_.push_back(p);
	}
}

} // namespace ve
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add extension/src/lod/lod_arena.h extension/src/lod/lod_arena.cpp \
        extension/tests/test_lod_arena.cpp
git commit -m "feat: all-or-nothing lod page arena"
```

---

### Task 7: `lod/lod_tree` — the octree walk

**Status: complete** — `f982210`, corrected by `e5e7224`. Steps are ticked; read them for context, do not re-run them.

Spec §6. The largest pure task and the one that replaces distance-shell residency. Everything it needs from the GPU arrives through two injected interfaces, so the whole thing is unit-testable with a synthetic camera and a fake depth pyramid.

**Files:**
- Create: `extension/src/lod/lod_tree.h`, `extension/src/lod/lod_tree.cpp`
- Create: `extension/tests/test_lod_tree.cpp`

**Interfaces:**
- Consumes: everything from `lod/lod_grid.h`; `ve::WorldBounds`, `ve::IVec3` (`world/region.h`).
- Produces: `ve::LodCamera`, `ve::lod_camera_perspective(...)`, `ve::LodOcclusion`, `ve::LodNodeState`, `ve::LodDrawItem`, `ve::LodBuildRequest`, `ve::LodWalkResult`, `ve::LodTreeConfig`, `ve::LodTree` with `walk`, `note_building`, `note_ready`, `note_empty`, `note_failed`, `mark_dirty`, `collect_evictions`, `state_of`, `node_count`, `clear`.

- [x] **Step 1: Write the failing test**

Create `extension/tests/test_lod_tree.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_tree.h"
#include "lod/lod_grid.h"
#include <cmath>
#include <vector>

namespace {

ve::WorldBounds demo_bounds() {
	ve::WorldBounds b;
	b.origin_bricks = {0, -64, 0};
	b.size_regions = {64, 8, 64}; // 1638.4 x 204.8 x 1638.4 m
	return b;
}

// Looking down -Z from just above the terrain, which sits at y ~ 51.2 (M2 errata 9).
ve::LodCamera cam_at(float x, float y, float z) {
	const float pos[3] = {x, y, z};
	const float fwd[3] = {0.0f, 0.0f, -1.0f};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	return ve::lod_camera_perspective(pos, fwd, up, 1.2217f, 16.0f / 9.0f, 0.1f, 8000.0f,
			2560, 1440);
}

// Nothing is ever occluded.
struct NoOcclusion : ve::LodOcclusion {
	bool occluded(const float[3], const float[3]) const override { return false; }
};
// Everything is always occluded.
struct AllOccluded : ve::LodOcclusion {
	bool occluded(const float[3], const float[3]) const override { return true; }
};

// Drives a tree to a steady state by answering every request as a ready chunk.
void settle(ve::LodTree *t, const ve::LodCamera &c, const ve::LodOcclusion *occ, int frames) {
	ve::LodWalkResult r;
	for (int f = 1; f <= frames; f++) {
		t->walk(c, occ, uint32_t(f), &r);
		for (const ve::LodBuildRequest &q : r.requests)
			t->note_ready(q.level, q.coord, 1, 1);
	}
}

} // namespace

TEST_CASE("the roots come from the world bounds and start unknown") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	ve::LodWalkResult r;
	NoOcclusion occ;
	t.walk(cam_at(800.0f, 60.0f, 800.0f), &occ, 1, &r);
	// Nothing is ready yet, so nothing draws and the roots are requested.
	CHECK(r.draws.empty());
	CHECK(!r.requests.empty());
	for (const ve::LodBuildRequest &q : r.requests) CHECK(q.level == ve::kLodLevels - 1);
}

// The cut must be COMPLETE and NON-OVERLAPPING at every instant: a streaming child never
// opens a hole and never z-fights its parent. This is the invariant the whole walk exists
// to preserve, so it is checked on every frame of a settling run, not only at the end.
TEST_CASE("the emitted cut never overlaps and never leaves a gap under a drawn parent") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	for (int f = 1; f <= 40; f++) {
		t.walk(c, &occ, uint32_t(f), &r);
		// No drawn chunk may be an ancestor or descendant of another drawn chunk.
		for (size_t i = 0; i < r.draws.size(); i++)
			for (size_t j = i + 1; j < r.draws.size(); j++) {
				const ve::LodDrawItem &a = r.draws[i];
				const ve::LodDrawItem &b = r.draws[j];
				if (a.level == b.level) {
					CHECK(!(a.coord == b.coord));
					continue;
				}
				const ve::LodDrawItem &lo = a.level < b.level ? a : b;
				const ve::LodDrawItem &hi = a.level < b.level ? b : a;
				ve::IVec3 up = lo.coord;
				for (int l = lo.level; l < hi.level; l++) up = ve::lod_parent(up);
				CHECK(!(up == hi.coord));
			}
		for (const ve::LodBuildRequest &q : r.requests) t.note_ready(q.level, q.coord, 1, 1);
	}
}

TEST_CASE("a node is only descended into when all eight children are ready") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	t.walk(c, &occ, 1, &r);
	REQUIRE(!r.requests.empty());
	const ve::LodBuildRequest root = r.requests[0];
	t.note_ready(root.level, root.coord, 1, 1);
	t.walk(c, &occ, 2, &r);
	// The root now draws, and its children are requested -- but only SEVEN of them ready
	// must not be enough to descend.
	const ve::IVec3 base = ve::lod_child_base(root.coord);
	for (int k = 0; k < 7; k++)
		t.note_ready(root.level - 1,
				{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)}, 1, 1);
	t.walk(c, &occ, 3, &r);
	bool root_drawn = false;
	for (const ve::LodDrawItem &d : r.draws)
		if (d.level == root.level && d.coord == root.coord) root_drawn = true;
	CHECK(root_drawn);
}

// A chunk that reports itself empty is a valid, terminal answer -- it counts as ready for
// the sibling test, otherwise a hole in the terrain would freeze the whole subtree.
TEST_CASE("an empty child counts as ready and draws nothing") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	t.walk(c, &occ, 1, &r);
	REQUIRE(!r.requests.empty());
	for (const ve::LodBuildRequest &q : r.requests) t.note_empty(q.level, q.coord);
	t.walk(c, &occ, 2, &r);
	CHECK(r.draws.empty());
	CHECK(r.requests.empty()); // an empty node has nothing below it worth asking for
}

// Levels 5, 6 and 7 are permanently resident: turning around shows coarse terrain, not sky.
TEST_CASE("coarse levels are never evicted and fine ones are") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	settle(&t, cam_at(800.0f, 60.0f, 800.0f), &occ, 30);
	const int before = t.node_count();
	CHECK(before > 0);
	std::vector<ve::LodDrawItem> evicted;
	// Far in the future, so every unmarked node is past kLodEvictFrames.
	t.collect_evictions(1000000u, 0, &evicted);
	for (const ve::LodDrawItem &e : evicted) CHECK(e.level < ve::kLodResidentLevelFrom);
}

TEST_CASE("a marked node is never evicted for age") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 30);
	ve::LodWalkResult r;
	t.walk(c, &occ, 31u, &r);
	std::vector<ve::LodDrawItem> evicted;
	t.collect_evictions(31u, 0, &evicted);
	CHECK(evicted.empty());
}

// Occlusion may stop REFINEMENT but must never stop DRAWING: the readback is stale, so a
// wrongly hidden chunk would be a hole that heals only when the camera moves.
TEST_CASE("occlusion stops requests but never removes a drawn chunk") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion none;
	AllOccluded all;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &none, 20);
	ve::LodWalkResult open;
	t.walk(c, &none, 21u, &open);
	const size_t drawn_open = open.draws.size();

	ve::LodWalkResult shut;
	// Confirmation takes kLodOccludedFrames frames, so the first few walks still request.
	for (uint32_t f = 22; f < 22 + uint32_t(ve::kLodOccludedFrames) + 2; f++)
		t.walk(c, &all, f, &shut);
	CHECK(shut.requests.empty());
	CHECK(shut.draws.size() == drawn_open);
}

// A drawn node whose field changed keeps drawing its stale pages until the rebuild lands.
// Stale beats missing (engine spec section 8).
TEST_CASE("an edit re-requests a chunk without un-drawing it") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 30);
	ve::LodWalkResult before;
	t.walk(c, &occ, 31u, &before);
	REQUIRE(!before.draws.empty());
	CHECK(before.requests.empty());

	const ve::LodDrawItem d = before.draws[0];
	float lo[3], hi[3];
	ve::lod_chunk_aabb(d.level, d.coord, lo, hi);
	t.mark_dirty(lo, hi);

	ve::LodWalkResult after;
	t.walk(c, &occ, 32u, &after);
	CHECK(after.draws.size() == before.draws.size());
	bool re_requested = false;
	for (const ve::LodBuildRequest &q : after.requests)
		if (q.level == d.level && q.coord == d.coord) re_requested = true;
	CHECK(re_requested);
}

TEST_CASE("requests are capped and ordered largest first") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	cfg.max_requests_per_walk = 4;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	for (int f = 1; f <= 10; f++) {
		t.walk(c, &occ, uint32_t(f), &r);
		CHECK(int(r.requests.size()) <= 4);
		for (size_t i = 1; i < r.requests.size(); i++)
			CHECK(r.requests[i - 1].priority >= r.requests[i].priority);
		for (const ve::LodBuildRequest &q : r.requests) t.note_ready(q.level, q.coord, 1, 1);
	}
}

// A chunk entirely inside the near field is discarded by the fragment shader on every pixel,
// so building it burns pages to draw nothing (spec section 6.4).
TEST_CASE("chunks entirely inside the fade start are never requested") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	for (int f = 1; f <= 60; f++) {
		t.walk(c, &occ, uint32_t(f), &r);
		for (const ve::LodBuildRequest &q : r.requests) {
			const float pos[3] = {800.0f, 60.0f, 800.0f};
			CHECK(ve::lod_chunk_far_distance(q.level, q.coord, pos) >= ve::kLodFadeStartM);
			t.note_ready(q.level, q.coord, 1, 1);
		}
	}
}

// A failed build must be retried, not cached as done -- otherwise one transient GPU hiccup
// leaves a permanent hole.
TEST_CASE("a failed build is retried") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	REQUIRE(!r.requests.empty());
	const ve::LodBuildRequest q = r.requests[0];
	t.note_building(q.level, q.coord);
	t.note_failed(q.level, q.coord);
	t.walk(c, &occ, 2u, &r);
	bool again = false;
	for (const ve::LodBuildRequest &s : r.requests)
		if (s.level == q.level && s.coord == q.coord) again = true;
	CHECK(again);
}

// A node with a build in flight must not be requested again every frame, or the queue fills
// with duplicates and nothing else is ever built.
TEST_CASE("a building node is not re-requested") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	REQUIRE(!r.requests.empty());
	for (const ve::LodBuildRequest &q : r.requests) t.note_building(q.level, q.coord);
	t.walk(c, &occ, 2u, &r);
	CHECK(r.requests.empty());
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `lod/lod_tree.h: No such file or directory`.

- [x] **Step 3: Write the header**

Create `extension/src/lod/lod_tree.h`:

```cpp
#pragma once
#include "lod/lod_grid.h"
#include "world/region.h"
#include <cstdint>
#include <map>
#include <vector>

namespace ve {

// Everything the walk needs to know about the view. view_proj is COLUMN-MAJOR in GLSL order
// (index = column * 4 + row), which is what godot::Projection hands over and what the raster
// pass pushes, so there is exactly one convention.
struct LodCamera {
	float view_proj[16] = {};
	float pos[3] = {};
	int viewport[2] = {1, 1};
};

// A REVERSE-Z perspective camera (near -> 1, far -> 0), matching Godot 4.7.1's depth-corrected
// scene projection (M1 errata 2). Production passes Godot's own matrix; this exists so tests
// can state a camera in one line and still exercise the same depth convention.
LodCamera lod_camera_perspective(const float pos[3], const float fwd[3], const float up[3],
		float fov_y_rad, float aspect, float z_near, float z_far, int vw, int vh);

// How the walk asks whether something is hidden. An interface for the same reason
// ve::ChunkProbe is one: the real answer lives in a GPU readback on the Godot side of the
// wall, and a test must be able to answer it in two lines.
struct LodOcclusion {
	virtual ~LodOcclusion() = default;
	// Screen-space AABB: xy in [0, 1], z the REVERSE-Z depth (larger = nearer). Returns true
	// when everything in that box is behind an already-drawn surface.
	virtual bool occluded(const float ss_min[3], const float ss_max[3]) const = 0;
};

enum LodNodeState : uint8_t {
	kLodUnknown = 0,
	kLodBuilding = 1,
	kLodReady = 2,
	kLodEmpty = 3,
	kLodFailed = 4,
};

struct LodDrawItem {
	int level = 0;
	IVec3 coord{};
	int page_first = -1;
	int page_count = 0;
};

struct LodBuildRequest {
	int level = 0;
	IVec3 coord{};
	float priority = 0.0f; // projected screen area in px^2; larger is built first
};

struct LodWalkResult {
	std::vector<LodDrawItem> draws;
	std::vector<LodBuildRequest> requests;
};

struct LodTreeConfig {
	WorldBounds bounds{};
	float sse_area_thresh = kLodSseAreaThresh;
	int resident_level_from = kLodResidentLevelFrom;
	uint32_t evict_frames = 300;
	uint32_t occluded_frames = 8;
	int max_requests_per_walk = 32;
	float fade_start_m = kLodFadeStartM;
};

// Spec section 6. Residency is what the walk touched, not what is near.
class LodTree {
public:
	explicit LodTree(const LodTreeConfig &cfg);

	// One walk per frame against the CURRENT camera. `occ` may be null (no readback yet).
	void walk(const LodCamera &cam, const LodOcclusion *occ, uint32_t frame, LodWalkResult *out);

	void note_building(int level, IVec3 c);
	void note_ready(int level, IVec3 c, int page_first, int page_count);
	void note_empty(int level, IVec3 c);
	void note_failed(int level, IVec3 c);

	// Every level whose chunks the world AABB touches is re-requested. A drawn node keeps
	// drawing its stale pages until the rebuild lands -- stale beats missing.
	void mark_dirty(const float lo[3], const float hi[3]);

	// Nodes to free. Age-based when want_pages == 0; under arena pressure it additionally
	// evicts least-recently-marked first until want_pages have been recovered. Levels at or
	// above resident_level_from are exempt.
	void collect_evictions(uint32_t frame, int want_pages, std::vector<LodDrawItem> *out);

	int state_of(int level, IVec3 c) const;
	int node_count() const { return static_cast<int>(nodes_.size()); }
	void clear();
	const LodTreeConfig &config() const { return cfg_; }

private:
	struct Key {
		int level, x, y, z;
		bool operator<(const Key &o) const {
			if (level != o.level) return level < o.level;
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	struct Node {
		uint8_t state = kLodUnknown;
		bool dirty = false;
		int page_first = -1;
		int page_count = 0;
		uint32_t last_marked = 0;
		uint32_t occluded_since = 0; // 0 = not currently occluded
	};
	static Key key(int level, IVec3 c) { return Key{level, c.x, c.y, c.z}; }

	void visit(int level, IVec3 c, const LodCamera &cam, const LodOcclusion *occ,
			uint32_t frame, LodWalkResult *out);
	bool children_ready(int level, IVec3 c) const;
	void request(int level, IVec3 c, float area, LodWalkResult *out);

	LodTreeConfig cfg_;
	std::map<Key, Node> nodes_;
	float planes_[6][4] = {};  // scratch, rebuilt per walk
};

// Exposed for testing: the six frustum planes of a view-projection, and Voxy's exact
// projected-area measure (screenspace.glsl's shouldDecend), which is the silhouette area
// rather than the screen AABB -- an AABB over-estimates a diagonal box by 2-3x and would
// over-tessellate everywhere.
void lod_frustum_planes(const float view_proj[16], float out[6][4]);
bool lod_aabb_in_frustum(const float planes[6][4], const float lo[3], const float hi[3]);
// Returns the area in px^2, or a very large value when the box straddles the near plane
// (where the perspective divide is meaningless and the only safe answer is "descend").
float lod_projected_area(const LodCamera &cam, const float lo[3], const float hi[3],
		float ss_min[3], float ss_max[3]);

} // namespace ve
```

- [x] **Step 4: Write the implementation**

Create `extension/src/lod/lod_tree.cpp`:

```cpp
#include "lod/lod_tree.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

void mat_mul_vec(const float m[16], const float v[4], float out[4]) {
	for (int r = 0; r < 4; r++)
		out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2] +
				m[3 * 4 + r] * v[3];
}

float cross_mag(const float a[2], const float b[2]) {
	return std::fabs(a[0] * b[1] - b[0] * a[1]);
}

void normalize3(float v[3]) {
	const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

} // namespace

LodCamera lod_camera_perspective(const float pos[3], const float fwd[3], const float up[3],
		float fov_y_rad, float aspect, float z_near, float z_far, int vw, int vh) {
	LodCamera c;
	c.pos[0] = pos[0]; c.pos[1] = pos[1]; c.pos[2] = pos[2];
	c.viewport[0] = vw;
	c.viewport[1] = vh;

	float f[3] = {fwd[0], fwd[1], fwd[2]};
	normalize3(f);
	float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
			f[0] * up[1] - f[1] * up[0]};
	normalize3(s);
	const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
			s[0] * f[1] - s[1] * f[0]};

	// View matrix (column-major).
	float v[16] = {};
	v[0] = s[0]; v[4] = s[1]; v[8] = s[2];
	v[1] = u[0]; v[5] = u[1]; v[9] = u[2];
	v[2] = -f[0]; v[6] = -f[1]; v[10] = -f[2];
	v[12] = -(s[0] * pos[0] + s[1] * pos[1] + s[2] * pos[2]);
	v[13] = -(u[0] * pos[0] + u[1] * pos[1] + u[2] * pos[2]);
	v[14] = f[0] * pos[0] + f[1] * pos[1] + f[2] * pos[2];
	v[15] = 1.0f;

	// REVERSE-Z perspective: z maps near -> 1, far -> 0 (M1 errata 2).
	const float t = 1.0f / std::tan(fov_y_rad * 0.5f);
	float p[16] = {};
	p[0] = t / aspect;
	p[5] = t;
	p[10] = z_near / (z_far - z_near);
	p[11] = -1.0f;
	p[14] = (z_far * z_near) / (z_far - z_near);

	for (int col = 0; col < 4; col++)
		for (int row = 0; row < 4; row++) {
			float acc = 0.0f;
			for (int k = 0; k < 4; k++) acc += p[k * 4 + row] * v[col * 4 + k];
			c.view_proj[col * 4 + row] = acc;
		}
	return c;
}

void lod_frustum_planes(const float m[16], float out[6][4]) {
	// Gribb-Hartmann on a column-major matrix: row r of the matrix is m[c*4 + r].
	const auto row = [&](int r, int c) { return m[c * 4 + r]; };
	for (int i = 0; i < 6; i++) {
		const int r = i >> 1;
		const float sgn = (i & 1) ? -1.0f : 1.0f;
		for (int c = 0; c < 4; c++) out[i][c] = row(3, c) + sgn * row(r, c);
	}
	for (int i = 0; i < 6; i++) {
		const float l = std::sqrt(out[i][0] * out[i][0] + out[i][1] * out[i][1] +
				out[i][2] * out[i][2]);
		if (l > 0.0f)
			for (int c = 0; c < 4; c++) out[i][c] /= l;
	}
}

bool lod_aabb_in_frustum(const float planes[6][4], const float lo[3], const float hi[3]) {
	for (int i = 0; i < 6; i++) {
		// The AABB corner farthest along the plane normal. If even that is behind the plane,
		// the whole box is.
		const float px = planes[i][0] >= 0.0f ? hi[0] : lo[0];
		const float py = planes[i][1] >= 0.0f ? hi[1] : lo[1];
		const float pz = planes[i][2] >= 0.0f ? hi[2] : lo[2];
		if (planes[i][0] * px + planes[i][1] * py + planes[i][2] * pz + planes[i][3] < 0.0f)
			return false;
	}
	return true;
}

float lod_projected_area(const LodCamera &cam, const float lo[3], const float hi[3],
		float ss_min[3], float ss_max[3]) {
	float ss[8][3];
	for (int k = 0; k < 8; k++) {
		const float p[4] = {(k & 1) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1],
				(k & 4) ? hi[2] : lo[2], 1.0f};
		float clip[4];
		mat_mul_vec(cam.view_proj, p, clip);
		// Straddling the near plane makes the perspective divide meaningless. The only safe
		// answer is "this is enormous, descend" -- and no occlusion claim can be made.
		if (clip[3] <= 1e-4f) {
			ss_min[0] = ss_min[1] = ss_min[2] = 0.0f;
			ss_max[0] = ss_max[1] = ss_max[2] = 1.0f;
			return 3.4e38f;
		}
		const float inv = 1.0f / clip[3];
		ss[k][0] = (clip[0] * inv * 0.5f + 0.5f) * float(cam.viewport[0]);
		ss[k][1] = (clip[1] * inv * 0.5f + 0.5f) * float(cam.viewport[1]);
		ss[k][2] = clip[2] * inv;
	}
	for (int a = 0; a < 3; a++) {
		ss_min[a] = ss[0][a];
		ss_max[a] = ss[0][a];
		for (int k = 1; k < 8; k++) {
			ss_min[a] = std::min(ss_min[a], ss[k][a]);
			ss_max[a] = std::max(ss_max[a], ss[k][a]);
		}
	}
	// Voxy's exact silhouette measure: the three faces meeting at corner 000 plus the three
	// meeting at 111, halved because that counts front and back.
	const auto edge = [&](int from, int to, float d[2]) {
		d[0] = ss[to][0] - ss[from][0];
		d[1] = ss[to][1] - ss[from][1];
	};
	float A[2], B[2], C[2];
	float area = 0.0f;
	edge(0, 1, A); edge(0, 2, B); edge(0, 4, C);
	area += cross_mag(A, B) + cross_mag(A, C) + cross_mag(C, B);
	edge(7, 6, A); edge(7, 5, B); edge(7, 3, C);
	area += cross_mag(A, B) + cross_mag(A, C) + cross_mag(C, B);
	area *= 0.5f;

	// Normalise the screen box to [0, 1] for the occlusion interface.
	ss_min[0] /= float(cam.viewport[0]); ss_max[0] /= float(cam.viewport[0]);
	ss_min[1] /= float(cam.viewport[1]); ss_max[1] /= float(cam.viewport[1]);
	for (int a = 0; a < 2; a++) {
		ss_min[a] = std::max(0.0f, std::min(ss_min[a], 1.0f));
		ss_max[a] = std::max(0.0f, std::min(ss_max[a], 1.0f));
	}
	return area;
}

LodTree::LodTree(const LodTreeConfig &cfg) : cfg_(cfg) {}

void LodTree::clear() { nodes_.clear(); }

int LodTree::state_of(int level, IVec3 c) const {
	const auto it = nodes_.find(key(level, c));
	return it == nodes_.end() ? -1 : int(it->second.state);
}

void LodTree::note_building(int level, IVec3 c) {
	nodes_[key(level, c)].state = kLodBuilding;
}

void LodTree::note_ready(int level, IVec3 c, int page_first, int page_count) {
	Node &n = nodes_[key(level, c)];
	n.state = kLodReady;
	n.dirty = false;
	n.page_first = page_first;
	n.page_count = page_count;
}

void LodTree::note_empty(int level, IVec3 c) {
	Node &n = nodes_[key(level, c)];
	n.state = kLodEmpty;
	n.dirty = false;
	n.page_first = -1;
	n.page_count = 0;
}

void LodTree::note_failed(int level, IVec3 c) {
	nodes_[key(level, c)].state = kLodFailed;
}

bool LodTree::children_ready(int level, IVec3 c) const {
	if (level <= 0) return false;
	const IVec3 base = lod_child_base(c);
	for (int k = 0; k < 8; k++) {
		const IVec3 ch{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)};
		if (!lod_chunk_in_bounds(cfg_.bounds, level - 1, ch)) continue; // outside is "done"
		const auto it = nodes_.find(key(level - 1, ch));
		if (it == nodes_.end()) return false;
		if (it->second.state != kLodReady && it->second.state != kLodEmpty) return false;
	}
	return true;
}

void LodTree::request(int level, IVec3 c, float area, LodWalkResult *out) {
	if (!lod_chunk_in_bounds(cfg_.bounds, level, c)) return;
	// Never build what the fragment shader would discard on every pixel (spec section 6.4).
	if (lod_chunk_far_distance(level, c, last_cam_pos_) < cfg_.fade_start_m) return;
	Node &n = nodes_[key(level, c)];
	if (n.state == kLodBuilding) return;
	if (n.state == kLodEmpty) return;
	if (n.state == kLodReady && !n.dirty) return;
	out->requests.push_back(LodBuildRequest{level, c, area});
}

void LodTree::visit(int level, IVec3 c, const LodCamera &cam, const LodOcclusion *occ,
		uint32_t frame, LodWalkResult *out) {
	if (!lod_chunk_in_bounds(cfg_.bounds, level, c)) return;
	float lo[3], hi[3];
	lod_chunk_aabb(level, c, lo, hi);

	Node &n = nodes_[key(level, c)];
	n.last_marked = frame; // touched, therefore resident: this is the whole eviction rule

	if (!lod_aabb_in_frustum(planes_, lo, hi)) return;

	float ss_min[3], ss_max[3];
	const float area = lod_projected_area(cam, lo, hi, ss_min, ss_max);

	if (occ && area < 3.0e38f && occ->occluded(ss_min, ss_max)) {
		if (n.occluded_since == 0) n.occluded_since = frame;
	} else {
		n.occluded_since = 0;
	}
	const bool refine_blocked = n.occluded_since != 0 &&
			(frame - n.occluded_since) >= cfg_.occluded_frames;

	if (n.state != kLodReady) {
		// Not drawable. Ask for it (unless occlusion says nobody would see it) and stop:
		// there is nothing below a node we do not have.
		if (n.state != kLodEmpty && !refine_blocked) request(level, c, area, out);
		return;
	}

	const bool want_finer = level > 0 && area > cfg_.sse_area_thresh;
	if (want_finer && children_ready(level, c)) {
		const IVec3 base = lod_child_base(c);
		for (int k = 0; k < 8; k++)
			visit(level - 1, {base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)},
					cam, occ, frame, out);
		return;
	}

	out->draws.push_back(LodDrawItem{level, c, n.page_first, n.page_count});
	if (n.dirty && !refine_blocked) request(level, c, area, out);
	if (want_finer && !refine_blocked) {
		const IVec3 base = lod_child_base(c);
		for (int k = 0; k < 8; k++) {
			const IVec3 ch{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)};
			// The child inherits its parent's area as its priority: the parent is what the
			// viewer is actually looking at, and eight children of one parent should arrive
			// together or the sibling gate never opens.
			request(level - 1, ch, area, out);
		}
	}
}

void LodTree::walk(const LodCamera &cam, const LodOcclusion *occ, uint32_t frame,
		LodWalkResult *out) {
	out->draws.clear();
	out->requests.clear();
	lod_frustum_planes(cam.view_proj, planes_);
	last_cam_pos_[0] = cam.pos[0];
	last_cam_pos_[1] = cam.pos[1];
	last_cam_pos_[2] = cam.pos[2];

	IVec3 lo{}, hi{};
	lod_root_range(cfg_.bounds, &lo, &hi);
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++)
				visit(kLodLevels - 1, {x, y, z}, cam, occ, frame, out);

	std::sort(out->requests.begin(), out->requests.end(),
			[](const LodBuildRequest &a, const LodBuildRequest &b) {
				if (a.priority != b.priority) return a.priority > b.priority;
				if (a.level != b.level) return a.level > b.level; // coarse first: the gate
				if (a.coord.z != b.coord.z) return a.coord.z < b.coord.z;
				if (a.coord.y != b.coord.y) return a.coord.y < b.coord.y;
				return a.coord.x < b.coord.x;
			});
	// De-duplicate: a node can be requested both as a dirty draw and as a parent's child.
	auto last = std::unique(out->requests.begin(), out->requests.end(),
			[](const LodBuildRequest &a, const LodBuildRequest &b) {
				return a.level == b.level && a.coord == b.coord;
			});
	out->requests.erase(last, out->requests.end());
	if (int(out->requests.size()) > cfg_.max_requests_per_walk)
		out->requests.resize(size_t(cfg_.max_requests_per_walk));
}

void LodTree::mark_dirty(const float lo[3], const float hi[3]) {
	EditOp probe;
	probe.type = kOpSphereSubtract;
	for (int a = 0; a < 3; a++) probe.pos[a] = 0.5f * (lo[a] + hi[a]);
	probe.radius = 0.5f * std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]);
	for (int level = 0; level < kLodLevels; level++) {
		IVec3 clo{}, chi{};
		op_lod_chunk_range(probe, level, &clo, &chi);
		for (int z = clo.z; z <= chi.z; z++)
			for (int y = clo.y; y <= chi.y; y++)
				for (int x = clo.x; x <= chi.x; x++) {
					const auto it = nodes_.find(key(level, {x, y, z}));
					if (it == nodes_.end()) continue;
					// A cached "empty" would hide a surface an add-op just put there.
					if (it->second.state == kLodEmpty) it->second.state = kLodUnknown;
					it->second.dirty = true;
				}
	}
}

void LodTree::collect_evictions(uint32_t frame, int want_pages, std::vector<LodDrawItem> *out) {
	out->clear();
	struct Cand {
		Key k;
		uint32_t age;
		int pages;
		int page_first;
	};
	std::vector<Cand> cands;
	for (const auto &kv : nodes_) {
		if (kv.first.level >= cfg_.resident_level_from) continue;
		if (kv.second.state == kLodBuilding) continue;
		const uint32_t age = frame >= kv.second.last_marked ? frame - kv.second.last_marked : 0u;
		cands.push_back(Cand{kv.first, age, kv.second.page_count, kv.second.page_first});
	}
	std::sort(cands.begin(), cands.end(),
			[](const Cand &a, const Cand &b) { return a.age > b.age; });

	int recovered = 0;
	for (const Cand &c : cands) {
		const bool too_old = c.age > cfg_.evict_frames;
		const bool pressure = want_pages > 0 && recovered < want_pages && c.age > 0;
		if (!too_old && !pressure) continue;
		out->push_back(LodDrawItem{c.k.level, IVec3{c.k.x, c.k.y, c.k.z}, c.page_first, c.pages});
		recovered += c.pages;
	}
	for (const LodDrawItem &d : *out) nodes_.erase(key(d.level, d.coord));
}

} // namespace ve
```

Add the private member the code above uses to `lod_tree.h`, next to `planes_`:

```cpp
	float last_cam_pos_[3] = {}; // scratch, rebuilt per walk; read by request()
```

- [x] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

If "the emitted cut never overlaps" fails with an ancestor and descendant both drawn, the bug is in `visit`: the descend branch must `return` after recursing, never fall through to `out->draws.push_back`.

- [x] **Step 6: Commit**

```bash
git add extension/src/lod/lod_tree.h extension/src/lod/lod_tree.cpp \
        extension/tests/test_lod_tree.cpp
git commit -m "feat: visibility-driven lod octree walk"
```

---

### Task 8: generalise the mesher to an origin, a cell size, and a lattice dimension

**Status: complete** — `fbcb87f`, corrected by `2066abb`. Steps are ticked; read them for context, do not re-run them.

M3 wrote `ve::DcGrid` parameterised "so M5's LoD chunks can reuse the mesher at their own pitch" (`dual_contour.h:14`). The GPU side never was. This task moves the three compile-time constants in `shaders/mesh_common.glslh` into the push constant, which turns the M3 mesher into the M5 mesher. **The whole task is a refactor: M3's collision behaviour must be byte-identical afterwards, and its tests are the gate.**

**Files:**
- Modify: `shaders/mesh_common.glslh`, `shaders/mesh_field.comp.glsl`, `shaders/mesh_cells.comp.glsl`, `shaders/mesh_quads.comp.glsl`
- Modify: `extension/src/render/mesh_pass.h`, `extension/src/render/mesh_pass.cpp`

**Interfaces:**
- Consumes: `ve::kChunkLattice`, `ve::kChunkCellSize`, `ve::chunk_world_origin` (`mesh/mesh_chunk.h`).
- Produces: `godot::MeshJob` gains `float origin[3]`, `float cell_size`, `int lattice`; `MeshPassConfig` gains `int max_lattice = ve::kChunkLattice` so buffers are sized for the largest consumer.

- [x] **Step 1: Record the baseline the refactor must preserve**

Run: `./gdunit_tests.sh -a res://tests/test_mesh_diff.gd`
Run: `./gdunit_tests.sh -a res://tests/test_mesh_lattice.gd`
Expected: both PASS. Note the pass counts; they must be identical at Step 6.

- [x] **Step 2: Move the geometry into the push constant**

Replace the whole of `shaders/mesh_common.glslh` with:

```glsl
// Chunk lattice addressing, shared by the meshing passes so they can never disagree.
// Mirror of extension/src/mesh/mesh_chunk.h and ve::dual_contour's conventions: lattice
// array index i holds the sample at local coordinate i - 1, mesh-cell array index m holds
// the cell at local coordinate m - 1, and cell m's corners are lattice m and m + 1. The
// one-cell overlap below the origin is what lets a chunk close the quads on its minimum
// faces without reading a neighbouring chunk's lattice. Include common.glslh first.
//
// The lattice size, cell pitch and world origin arrive in the PUSH CONSTANT rather than as
// constants, so one mesher serves the 0.1 m collision lattice and every LoD level. The block
// is declared here, not in each shader, so there is exactly one layout.
layout(push_constant, std430) uniform Push {
	ivec4 chunk;  // xyz = chunk coordinates (job identity only), w = job index in this batch
	ivec4 params; // x = op count, y = max verts per job, z = max tris per job, w = lattice dim
	vec4 grid;    // xyz = the chunk's world origin, w = cell size in metres
} pc;

int chunk_lattice() { return pc.params.w; }
int chunk_mesh_cells() { return pc.params.w - 1; }
int chunk_cells() { return pc.params.w - 2; }
float chunk_cell_size() { return pc.grid.w; }

vec3 lattice_world_pos(ivec3 l) {
	return pc.grid.xyz + (vec3(l) - 1.0) * pc.grid.w;
}

int mesh_cell_index(ivec3 m) {
	int n = chunk_mesh_cells();
	return m.x + m.y * n + m.z * n * n;
}
```

- [x] **Step 3: Update the three shaders**

In `shaders/mesh_field.comp.glsl`, delete its own `layout(push_constant …) uniform Push { … } pc;` block and change the body to:

```glsl
void main() {
	ivec3 l = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(l, ivec3(chunk_lattice())))) return;
	float sdf;
	uint mat; // the mesher has no use for materials; collision carries none
	eval_field(lattice_world_pos(l), uint(pc.chunk.w) * MAX_REGION_OPS,
			uint(pc.params.x), sdf, mat);
	imageStore(lattice, l, vec4(quantise_sdf(sdf)));
}
```

In `shaders/mesh_cells.comp.glsl`, delete its `Push` block and change the two places that used the constants:

```glsl
	if (any(greaterThanEqual(m, ivec3(chunk_mesh_cells())))) return;
```

```glsl
	vec3 p = pc.grid.xyz + (vec3(m) - 1.0 + acc / float(n)) * pc.grid.w;
```

In `shaders/mesh_quads.comp.glsl`, delete its `Push` block and change the bounds test:

```glsl
	if (any(greaterThanEqual(u, ivec3(chunk_cells())))) return;
```

**Do not touch anything else in these files.** In particular leave the `QUAD`, `CORNER` and `EDGE` tables and the winding branch exactly as they are — M3 errata 1 was expensive.

- [x] **Step 4: Widen the push constant and carry the geometry in `MeshJob`**

In `extension/src/render/mesh_pass.h`, add to `MeshJob`:

```cpp
struct MeshJob {
	ve::IVec3 chunk{};
	const ve::EditOp *ops = nullptr; // the chunk's region's op list; copied at submit
	int op_count = 0;
	// Where and how finely to sample. Defaulted to the collision chunk so every existing
	// caller is unchanged; MeshService::submit fills them from ve::chunk_world_origin.
	float origin[3] = {0.0f, 0.0f, 0.0f};
	float cell_size = ve::kChunkCellSize;
	int lattice = ve::kChunkLattice;
};
```

and to `MeshPassConfig`:

```cpp
	// Buffers are sized for the largest lattice any consumer will ask for, so one pass can
	// serve both the collision chunk and (Task 9) a LoD chunk without reallocating.
	int max_lattice = ve::kChunkLattice;
```

In `extension/src/render/mesh_pass.cpp`, replace `MeshPass::push` and the three `record_*` dispatch sizes:

```cpp
void MeshPass::push(int64_t list, const MeshJob &job, int job_index) {
	PackedByteArray pc;
	pc.resize(48);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = job.chunk.x;
	p[1] = job.chunk.y;
	p[2] = job.chunk.z;
	p[3] = job_index;
	p[4] = sanitized_op_count(job);
	p[5] = cfg_.max_verts;
	p[6] = cfg_.max_tris;
	p[7] = job.lattice;
	float *f = reinterpret_cast<float *>(pc.ptrw());
	f[8] = job.origin[0];
	f[9] = job.origin[1];
	f[10] = job.origin[2];
	f[11] = job.cell_size;
	rd_->compute_list_set_push_constant(list, pc, pc.size());
}
```

and in the three recorders replace `groups(ve::kChunkLattice)` / `groups(ve::kChunkMeshCells)` / `groups(ve::kChunkCells)` with `groups(job.lattice)`, `groups(job.lattice - 1)` and `groups(job.lattice - 2)` respectively.

Size `lattice_` and `cells_` from `cfg_.max_lattice` rather than `ve::kChunkLattice` wherever `MeshPass::initialize` allocates them, and default any `MeshJob` the pass builds internally by filling `origin` with `ve::chunk_world_origin(job.chunk, job.origin)`.

- [x] **Step 5: Fill the new fields at every submit site**

In `extension/src/render/mesh_service.cpp`, wherever a `MeshRequest` becomes a `MeshJob`, add:

```cpp
	ve::chunk_world_origin(req.chunk, job.origin);
	job.cell_size = ve::kChunkCellSize;
	job.lattice = ve::kChunkLattice;
```

Grep for every other construction site and do the same: `grep -rn "MeshJob" extension/src`.

- [x] **Step 6: Run the regression gate**

Run: `./build.sh -j$(nproc) --test`
Run: `./gdunit_tests.sh -a res://tests`
Expected: identical results to Step 1 — in particular `test_mesh_diff.gd`, `test_mesh_lattice.gd`, `test_collider_stream.gd` and `test_collider_edits.gd` all still PASS. This task adds no new behaviour; if any of them changed, the refactor is wrong.

- [x] **Step 7: Commit**

```bash
git add shaders/mesh_common.glslh shaders/mesh_field.comp.glsl shaders/mesh_cells.comp.glsl \
        shaders/mesh_quads.comp.glsl extension/src/render/mesh_pass.h \
        extension/src/render/mesh_pass.cpp extension/src/render/mesh_service.cpp
git commit -m "refactor: mesher takes origin, cell size and lattice from the push constant"
```

---

### Task 9: `LodBuildPass` — field, reduce, cells, quads on the worker device

**Status: complete** — `5407c39`. Steps are ticked; read them for context, do not re-run them.

Spec §4 and §6.4. Four dispatches on M3's worker `RenderingDevice`, and a GPU/CPU differential test against Tasks 3 and 4.

**Files:**
- Create: `shaders/lod_field.comp.glsl`, `shaders/lod_reduce.comp.glsl`, `shaders/lod_quads.comp.glsl`
- Create: `extension/src/render/lod_build_pass.h`, `extension/src/render/lod_build_pass.cpp`
- Create: `tests/test_lod_mesh_diff.gd`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (the `debug_lod_diff` hook)

**Interfaces:**
- Consumes: `ve::lod_chunk_origin`, `ve::lod_cell_size`, `ve::kLodFineLattice`, `ve::kLodChunkLattice` (`lod/lod_grid.h`); `ve::collect_ops_for_aabb`, `ve::kMaxRegionOps` (`world/edit_log.h`); `ve::lod_reduce_lattice` (`lod/lod_reduce.h`); `ve::lod_contour` (`lod/lod_contour.h`); `ve::lod_append_skirts` (`lod/lod_skirt.h`); `godot::MeshPass` internals for the shared cells pass.
- Produces: `godot::LodBuildJob { int level; ve::IVec3 coord; std::vector<ve::EditOp> ops; }`, `godot::LodBuildResult { int level; ve::IVec3 coord; std::vector<ve::LodQuad> quads; bool overflow; bool failed; }`, `godot::LodBuildPass` with `initialize`, `teardown`, `is_valid`, `build_sync(const LodBuildJob &, LodBuildResult *, std::vector<uint8_t> *lattice, std::vector<uint16_t> *material)`, `submit`, `collect`.

- [x] **Step 1: Write the failing gdUnit differential test**

Create `tests/test_lod_mesh_diff.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# GPU/CPU differential test for the LoD build (engine spec section 8), the M5 counterpart of
# test_mesh_diff.gd. Three things are compared and each tolerance is what it is for a reason:
#
#  * The FINE lattice (69^3) against ve::eval_field: one encoded step, exactly as
#    test_brick_diff.gd allows (glibc's sin() against the driver's).
#  * The REDUCED lattice (34^3) against ve::lod_reduce_lattice run on the GPU's own fine
#    lattice. Both sides consume identical bytes, so this must agree to one encoded step and
#    the material lattice must agree EXACTLY -- a vote has no rounding.
#  * The QUADS against ve::lod_contour run on the GPU's own reduced lattice, compared as sets
#    of (u, axis) with their four corner offsets, so quad emission order (the GPU allocates
#    with atomics, in no fixed order) does not enter but a wrong winding still shows up.

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	return w

func check_diff(d: Dictionary, label: String) -> void:
	assert_bool(d.has("fine_max_diff")).override_failure_message(
		"%s: debug_lod_diff returned %s" % [label, d]).is_true()
	assert_int(d["fine_max_diff"]).override_failure_message(
		"%s: fine lattice differs by %d encoded steps" % [label, d["fine_max_diff"]]
		).is_less_equal(1)
	assert_int(d["reduced_max_diff"]).override_failure_message(
		"%s: reduced lattice differs by %d encoded steps" % [label, d["reduced_max_diff"]]
		).is_less_equal(1)
	assert_int(d["material_mismatches"]).override_failure_message(
		"%s: %d reduced material samples disagree" % [label, d["material_mismatches"]]
		).is_equal(0)
	assert_int(d["quads_only_cpu"]).override_failure_message(
		"%s: %d quads exist on the CPU only" % [label, d["quads_only_cpu"]]).is_equal(0)
	assert_int(d["quads_only_gpu"]).override_failure_message(
		"%s: %d quads exist on the GPU only" % [label, d["quads_only_gpu"]]).is_equal(0)
	assert_int(d["corner_max_diff"]).override_failure_message(
		"%s: a corner offset differs by %d steps" % [label, d["corner_max_diff"]]).is_equal(0)

func test_level_zero_over_the_surface() -> void:
	var w := make_world()
	# The terrain surface sits at y ~ 51.2 + hills (M2 errata 9), so an L0 chunk (12.8 m)
	# whose y index is 4 straddles it.
	check_diff(w.debug_lod_diff(0, Vector3i(2, 4, 2)), "L0 surface")

func test_every_level_agrees() -> void:
	var w := make_world()
	for level in range(0, 8):
		# The chunk containing (25.6, 51.2, 25.6) at each level.
		var s := 12.8 * pow(2.0, float(level))
		var c := Vector3i(int(floor(25.6 / s)), int(floor(51.2 / s)), int(floor(25.6 / s)))
		check_diff(w.debug_lod_diff(level, c), "level %d" % level)

func test_an_edit_reaches_the_coarse_levels() -> void:
	var w := make_world()
	# A 5 m crater. At L4 the cell is 6.4 m, so the crater is under one cell -- and the whole
	# point of the half-cell supersample is that it still moves samples there. Point sampling
	# at 6.4 m would leave the coarse lattice bit-identical, which this asserts against.
	var before := w.debug_lod_diff(4, Vector3i(0, 1, 0))
	assert_bool(before.has("reduced_hash")).is_true()
	var hash_before: int = before["reduced_hash"]
	w.debug_apply_sphere_subtract(Vector3(25.6, 51.2, 25.6), 5.0)
	var after := w.debug_lod_diff(4, Vector3i(0, 1, 0))
	check_diff(after, "L4 after a 5 m crater")
	assert_int(after["reduced_hash"]).override_failure_message(
		"a 5 m crater left the 6.4 m lattice bit-identical: the reduction is point sampling"
		).is_not_equal(hash_before)
```

- [x] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_lod_mesh_diff.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_lod_diff'`.

- [x] **Step 3: Write `shaders/lod_field.comp.glsl`**

```glsl
#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 2
#define FIELD_VOLUME_SDF_BINDING 3
#define FIELD_VOLUME_MAT_BINDING 4
#include "common.glslh"
#include "field.glslh"
#include "lod_common.glslh"

// One thread per HALF-CELL sample. Spec section 4: the target lattice is built from samples
// at half the level's cell size and tent-reduced, which is the mip cascade computed inside
// one build job. 69 is not a multiple of 4, so the last group of each axis runs partly out
// of bounds and returns.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) writeonly uniform image3D fine_sdf;
layout(set = 0, binding = 1, r16ui) writeonly uniform uimage3D fine_mat;

void main() {
	ivec3 j = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(j, ivec3(LOD_FINE_LATTICE)))) return;
	float sdf;
	uint mat;
	eval_field(lod_fine_world_pos(j), uint(lpc.job.w) * MAX_REGION_OPS, uint(lpc.params.x),
			sdf, mat);
	imageStore(fine_sdf, j, vec4(quantise_sdf(sdf)));
	imageStore(fine_mat, j, uvec4(mat, 0u, 0u, 0u));
}
```

- [x] **Step 4: Write `shaders/lod_common.glslh`**

```glsl
// Shared addressing for the LoD build passes. Mirror of extension/src/lod/lod_grid.h and
// lod/lod_reduce.cpp; the differential test in tests/test_lod_mesh_diff.gd fails when the
// two drift. Include common.glslh first.
const int LOD_CHUNK_CELLS = 32;       // ve::kLodChunkCells
const int LOD_CHUNK_MESH_CELLS = 33;  // ve::kLodChunkMeshCells
const int LOD_CHUNK_LATTICE = 34;     // ve::kLodChunkLattice
const int LOD_FINE_LATTICE = 69;      // ve::kLodFineLattice
const int LOD_QUADS_PER_PAGE = 512;   // ve::kLodQuadsPerPage
const int LOD_MAX_QUADS = 8192;       // ve::kLodMaxQuadsPerChunk
const int LOD_OFFSET_MAX = 31;        // ve::kLodOffsetMax

layout(push_constant, std430) uniform LodPush {
	ivec4 job;    // xyz = chunk coordinates, w = job index in this batch
	ivec4 params; // x = op count, y = max quads per job, z = level, w = unused
	vec4 grid;    // xyz = the chunk's world origin, w = the level's cell size
} lpc;

// Fine sample j sits at local coordinate (j - 3) / 2 cells, so j = 3 is the chunk origin and
// target lattice index i is centred on fine index 2i + 1. ve::lod_fine_local mirrors this.
vec3 lod_fine_world_pos(ivec3 j) {
	return lpc.grid.xyz + (vec3(j) - 3.0) * (lpc.grid.w * 0.5);
}

// The four cells around a lattice edge, wound counter-clockwise seen from +axis. Byte-
// identical to ve::kLodQuadCorners and to QUAD in shaders/mesh_quads.comp.glsl.
const ivec2 LOD_QUAD[4] = ivec2[4](ivec2(-1, -1), ivec2(0, -1), ivec2(0, 0), ivec2(-1, 0));

// Separable tent, ve::kLodTentWeights.
const float LOD_TENT[3] = float[3](0.25, 0.5, 0.25);
```

- [x] **Step 5: Write `shaders/lod_reduce.comp.glsl`**

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "lod_common.glslh"

// One thread per TARGET lattice sample. The SDF averages (symmetric: a crater and a spire
// survive equally, which a solid-preferring min would not); the material is a tent-weighted
// majority over the SOLID taps only, ties broken by the centre tap. Mirror of
// ve::lod_reduce_lattice.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D fine_sdf;
layout(set = 0, binding = 1, r16ui) readonly uniform uimage3D fine_mat;
layout(set = 0, binding = 2, r8) writeonly uniform image3D out_sdf;
layout(set = 0, binding = 3, r16ui) writeonly uniform uimage3D out_mat;

void main() {
	ivec3 i = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(i, ivec3(LOD_CHUNK_LATTICE)))) return;

	float acc = 0.0;
	uint ids[27];
	float votes[27];
	int n_ids = 0;
	uint centre_mat = 0u;
	for (int dz = 0; dz < 3; dz++)
		for (int dy = 0; dy < 3; dy++)
			for (int dx = 0; dx < 3; dx++) {
				ivec3 j = ivec3(2 * i.x + dx, 2 * i.y + dy, 2 * i.z + dz);
				float w = LOD_TENT[dx] * LOD_TENT[dy] * LOD_TENT[dz];
				float d = decode_sdf(imageLoad(fine_sdf, j).r);
				acc += w * d;
				uint m = imageLoad(fine_mat, j).r;
				if (dx == 1 && dy == 1 && dz == 1) centre_mat = m;
				if (d > 0.0) continue;
				if (m == 0u) continue;
				int slot = -1;
				for (int s = 0; s < n_ids; s++) { if (ids[s] == m) { slot = s; break; } }
				if (slot < 0) { slot = n_ids++; ids[slot] = m; votes[slot] = 0.0; }
				votes[slot] += w;
			}

	uint best = 0u;
	float best_v = 0.0;
	for (int s = 0; s < n_ids; s++) {
		if (votes[s] > best_v) { best_v = votes[s]; best = ids[s]; }
	}
	if (n_ids > 1) {
		for (int s = 0; s < n_ids; s++) {
			if (ids[s] == centre_mat && votes[s] >= best_v) { best = centre_mat; break; }
		}
	}
	imageStore(out_sdf, i, vec4(quantise_sdf(acc)));
	imageStore(out_mat, i, uvec4(best, 0u, 0u, 0u));
}
```

- [x] **Step 6: Write `shaders/lod_quads.comp.glsl`**

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "lod_common.glslh"

// One thread per owned edge coordinate; each handles that point's three axis edges. Mirror
// of ve::lod_contour's second pass, emitting the packed 12-byte record instead of triangle
// indices. Corners are written ALREADY WOUND so the vertex shader never branches.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, r16ui) readonly uniform uimage3D material;
// Fractional vertex position per mesh cell, quantised to 5 bits per axis and packed into one
// uint, or 0xFFFFFFFF when the cell holds no vertex.
layout(set = 0, binding = 2, std430) readonly buffer Frac { uint v[]; } frac;
// Three uints per quad: ve::LodQuad.
layout(set = 0, binding = 3, std430) writeonly buffer Quads { uint v[]; } quads;
// Two uints per job: quad count, overflow flag.
layout(set = 0, binding = 4, std430) buffer Counts { uint v[]; } counts;

void bits_set(inout uvec3 w, int lo, int bits, uint v) {
	uint mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << uint(bits)) - 1u);
	v &= mask;
	int word = lo >> 5;
	int shift = lo & 31;
	if (word == 0) w.x |= v << uint(shift); else if (word == 1) w.y |= v << uint(shift);
	else w.z |= v << uint(shift);
	int spill = shift + bits - 32;
	if (spill > 0) {
		uint hi = v >> uint(32 - shift);
		if (word == 0) w.y |= hi; else w.z |= hi;
	}
}

int cell_index(ivec3 m) {
	return m.x + m.y * LOD_CHUNK_MESH_CELLS + m.z * LOD_CHUNK_MESH_CELLS * LOD_CHUNK_MESH_CELLS;
}

void main() {
	ivec3 u = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(u, ivec3(LOD_CHUNK_CELLS)))) return;
	ivec3 L = u + 1;
	uint job = uint(lpc.job.w);
	float da = decode_sdf(imageLoad(lattice, L).r);

	for (int axis = 0; axis < 3; axis++) {
		ivec3 e = ivec3(0);
		e[axis] = 1;
		float db = decode_sdf(imageLoad(lattice, L + e).r);
		bool sa = da <= 0.0, sb = db <= 0.0;
		if (sa == sb) continue;
		int b = (axis + 1) % 3, c = (axis + 2) % 3;
		uint f[4];
		bool ok = true;
		for (int k = 0; k < 4; k++) {
			ivec3 m = L;
			m[b] += LOD_QUAD[k].x;
			m[c] += LOD_QUAD[k].y;
			f[k] = frac.v[cell_index(m)];
			if (f[k] == 0xFFFFFFFFu) ok = false;
		}
		if (!ok) continue;

		uint t = atomicAdd(counts.v[job * 2u + 0u], 1u);
		if (t >= uint(lpc.params.y)) { atomicOr(counts.v[job * 2u + 1u], 1u); return; }

		uvec3 w = uvec3(0u);
		bits_set(w, 0, 5, uint(u.x));
		bits_set(w, 5, 5, uint(u.y));
		bits_set(w, 10, 5, uint(u.z));
		bits_set(w, 15, 2, uint(axis));
		bits_set(w, 17, 1, sa ? 1u : 0u);
		// Already wound: (0,1,2,3) when the low end is solid, (0,3,2,1) otherwise -- the two
		// triangles of the reversed order are exactly ve::dual_contour's tri_rev pair.
		int order[4] = int[4](0, 1, 2, 3);
		if (!sa) { order[1] = 3; order[3] = 1; }
		for (int k = 0; k < 4; k++) {
			uint p = f[order[k]];
			for (int a = 0; a < 3; a++)
				bits_set(w, 18 + (k * 3 + a) * 5, 5, (p >> uint(a * 5)) & 31u);
		}
		ivec3 ms = sa ? L : (L + e);
		bits_set(w, 78, 16, imageLoad(material, ms).r);
		// bit 94 (double-sided) stays 0: skirts are appended on the CPU.

		uint base = (job * uint(lpc.params.y) + t) * 3u;
		quads.v[base + 0u] = w.x;
		quads.v[base + 1u] = w.y;
		quads.v[base + 2u] = w.z;
	}
}
```

- [x] **Step 7: Write `LodBuildPass`**

Create `extension/src/render/lod_build_pass.h` and `.cpp`. The pass owns, on the worker `RenderingDevice`:

- `fine_sdf_` — `R8_UNORM` 3D image, `kLodFineLattice³`
- `fine_mat_` — `R16_UINT` 3D image, same dims
- `lat_sdf_` — `R8_UNORM` 3D image, `kLodChunkLattice³`
- `lat_mat_` — `R16_UINT` 3D image, same dims
- `frac_` — `uint` per mesh cell, `max_jobs · kLodChunkMeshCells³`
- `quads_` — `3 · uint` per quad, `max_jobs · kLodMaxQuadsPerChunk`
- `counts_` — 2 `uint` per job
- `ops_` — `max_jobs · kMaxRegionOps` `EditOp`s, plus the shared `VolumePool`

and records four dispatches per job: `lod_field` at `groups(69)`, `lod_reduce` at `groups(34)`, a **fraction** pass, and `lod_quads` at `groups(32)`.

The fraction pass is `shaders/lod_frac.comp.glsl` — the cell pass, writing the quantised fraction directly rather than a world position, because that is what the record stores and what `ve::lod_contour` computes:

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "lod_common.glslh"

// One thread per mesh cell. Identical arithmetic to shaders/mesh_cells.comp.glsl, but it
// stores the vertex as a FRACTION of its own cell (5 bits per axis, packed) rather than a
// world position: that is exactly what the 12-byte record carries, so the quads pass needs
// no division and the GPU and ve::lod_contour quantise the same number the same way.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, std430) writeonly buffer Frac { uint v[]; } frac;

const ivec3 CORNER[8] = ivec3[8](ivec3(0, 0, 0), ivec3(1, 0, 0), ivec3(0, 1, 0), ivec3(1, 1, 0),
		ivec3(0, 0, 1), ivec3(1, 0, 1), ivec3(0, 1, 1), ivec3(1, 1, 1));
const ivec2 EDGE[12] = ivec2[12](ivec2(0, 1), ivec2(2, 3), ivec2(4, 5), ivec2(6, 7),
		ivec2(0, 2), ivec2(1, 3), ivec2(4, 6), ivec2(5, 7),
		ivec2(0, 4), ivec2(1, 5), ivec2(2, 6), ivec2(3, 7));

void main() {
	ivec3 m = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(m, ivec3(LOD_CHUNK_MESH_CELLS)))) return;
	int ci = m.x + m.y * LOD_CHUNK_MESH_CELLS +
			m.z * LOD_CHUNK_MESH_CELLS * LOD_CHUNK_MESH_CELLS;

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
	// Every cell is written every job: the buffer is shared by the batch and never cleared
	// between jobs, so "no vertex" has to be stored, not left behind.
	if (n == 0) { frac.v[ci] = 0xFFFFFFFFu; return; }

	vec3 f = clamp(acc / float(n), vec3(0.0), vec3(1.0));
	uvec3 q = uvec3(floor(f * float(LOD_OFFSET_MAX) + 0.5));
	frac.v[ci] = q.x | (q.y << 5) | (q.z << 10);
}
```

`LodBuildPass::build_sync` records the four dispatches, submits, syncs, reads back `counts_` and `quads_`, and appends `ve::lod_append_skirts` on the CPU. `submit`/`collect` mirror `MeshPass`'s one-batch-at-a-time contract.

- [x] **Step 8: Add the `debug_lod_diff` hook**

In `VoxelWorld`, add `Dictionary debug_lod_diff(int level, Vector3i coord)` and `void debug_apply_sphere_subtract(Vector3 centre, float radius)`. `debug_lod_diff` runs on the worker thread through `MeshService::run_sync`, reads back the fine lattice, the reduced lattice and the quads, then on the CPU:

1. evaluates `ve::eval_field` at every fine sample (using `ve::collect_ops_for_aabb` over the chunk's padded AABB, truncated to a chronological **prefix** of `ve::kMaxRegionOps` — M4 errata 1) and reports `fine_max_diff` as the largest absolute difference in encoded steps;
2. runs `ve::lod_reduce_lattice` on the **GPU's own** fine lattice and reports `reduced_max_diff` and `material_mismatches`;
3. runs `ve::lod_contour` on the **GPU's own** reduced lattice and compares the quad sets keyed by `(u, axis)`, reporting `quads_only_cpu`, `quads_only_gpu` and `corner_max_diff`;
4. reports `reduced_hash`, an FNV-1a over the reduced SDF bytes, so a test can assert that an edit changed a coarse level at all.

Bind both to `_bind_methods`.

- [x] **Step 9: Run the differential test**

Run: `./build.sh -j$(nproc)`
Run: `./gdunit_tests.sh -a res://tests/test_lod_mesh_diff.gd`
Expected: PASS.

If `quads_only_gpu` is non-zero and equals the count of quads whose `sign` is 0, the `order` permutation in `lod_quads.comp.glsl` disagrees with `ve::lod_contour`'s — they must both be `(0, 3, 2, 1)`.

- [x] **Step 10: Commit**

```bash
git add shaders/lod_common.glslh shaders/lod_field.comp.glsl shaders/lod_reduce.comp.glsl \
        shaders/lod_frac.comp.glsl shaders/lod_quads.comp.glsl \
        extension/src/render/lod_build_pass.h extension/src/render/lod_build_pass.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_mesh_diff.gd
git commit -m "feat: gpu lod chunk build with half-cell reduction"
```

---

### Task 10: `MeshService` grows a LoD queue

**Status: complete** — `1745ffc`, corrected by `a9d999a` and `b2c50fc`. Steps are ticked; read them for context, do not re-run them.

The worker thread already owns a `RenderingDevice`, a `MeshPass` and an `IslandExtractPass`. LoD builds join them in a third queue: frequent, interruptible, and never allowed to starve a collider batch the player is about to walk on.

**Files:**
- Modify: `extension/src/render/mesh_service.h`, `extension/src/render/mesh_service.cpp`
- Create: `tests/test_lod_build.gd`

**Interfaces:**
- Produces: `bool MeshService::submit_lod(std::vector<LodBuildJob>)`, `bool MeshService::lod_busy() const`, `int MeshService::collect_lod(std::vector<LodBuildResult> *)`, `bool MeshService::lod_available() const`.

- [x] **Step 1: Write the failing test**

Create `tests/test_lod_build.gd`:

```gdscript
extends GdUnitTestSuite

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
	assert_bool(w.debug_init_physics()).is_true()
	return w

func test_a_submitted_chunk_comes_back_with_quads(timeout := 20000) -> void:
	var w := make_world()
	# L0 chunk (2, 4, 2) straddles the surface at y ~ 51.2.
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 4, 2)]])).is_true()
	var got: Array = []
	for i in range(300):
		got = w.debug_lod_collect()
		if got.size() > 0:
			break
		await get_tree().process_frame
	assert_int(got.size()).override_failure_message(
		"the LoD build never came back").is_equal(1)
	assert_int(got[0]["level"]).is_equal(0)
	assert_vector(got[0]["coord"]).is_equal(Vector3i(2, 4, 2))
	assert_int(got[0]["quads"]).override_failure_message(
		"a chunk straddling the surface produced no quads").is_greater(0)
	assert_bool(got[0]["failed"]).is_false()

func test_an_air_chunk_comes_back_empty(timeout := 20000) -> void:
	var w := make_world()
	# High above the terrain: no surface, so no quads and no wasted pages.
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 12, 2)]])).is_true()
	var got: Array = []
	for i in range(300):
		got = w.debug_lod_collect()
		if got.size() > 0:
			break
		await get_tree().process_frame
	assert_int(got.size()).is_equal(1)
	assert_int(got[0]["quads"]).is_equal(0)

func test_a_batch_is_refused_while_one_is_in_flight() -> void:
	var w := make_world()
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 4, 2)]])).is_true()
	# MeshPass's one-batch-at-a-time contract: the residency bookkeeping relies on it.
	assert_bool(w.debug_lod_submit([[0, Vector3i(3, 4, 2)]])).is_false()

func test_collider_meshing_still_works_alongside(timeout := 20000) -> void:
	var w := make_world()
	assert_bool(w.debug_lod_submit([[0, Vector3i(2, 4, 2)]])).is_true()
	assert_bool(w.debug_mesh_submit([Vector3i(4, 8, 4)])).is_true()
	var lod_done := false
	var mesh_done := false
	for i in range(300):
		if w.debug_lod_collect().size() > 0:
			lod_done = true
		if w.debug_mesh_collect().size() > 0:
			mesh_done = true
		if lod_done and mesh_done:
			break
		await get_tree().process_frame
	assert_bool(lod_done).override_failure_message("the LoD queue starved").is_true()
	assert_bool(mesh_done).override_failure_message("the collider queue starved").is_true()
```

- [x] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_lod_build.gd`
Expected: FAIL — `Nonexistent function 'debug_lod_submit'`.

- [x] **Step 3: Add the queue to `MeshService`**

Mirror the extraction queue exactly (`mesh_service.h:66-80`): `pending_lod_`, `lod_results_`, `lod_busy_`, `lod_available_`, and a `LodBuildPass *lod_ = nullptr` created and destroyed inside `run()` on the worker thread — a `RenderingDevice` belongs to the thread that creates it. In `run()`'s loop, drain in this order: **volumes → extracts → colliders → LoD**. LoD is last because it is the only one whose staleness is invisible: a missing far chunk is a coarse horizon, a missing collider is a hole the player falls through.

- [x] **Step 4: Add the `VoxelWorld` hooks**

`bool debug_lod_submit(Array jobs)` takes an array of `[level, Vector3i]` pairs, gathers each chunk's ops with `ve::collect_ops_for_aabb` over its padded AABB (truncated to a chronological prefix of `ve::kMaxRegionOps`), and forwards to `MeshService::submit_lod`. `Array debug_lod_collect()` returns one dictionary per result with `level`, `coord`, `quads` (count), `overflow`, `failed`.

- [x] **Step 5: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then the gdUnit command from Step 2.
Expected: PASS. Also re-run the full suite — the worker thread now has a fourth responsibility and `test_mesh_stream.gd` / `test_island_extract.gd` are the regression gate.

- [x] **Step 6: Commit**

```bash
git add extension/src/render/mesh_service.h extension/src/render/mesh_service.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_build.gd
git commit -m "feat: lod build queue on the mesher worker thread"
```

---

### Task 11: material textures, shared by both fields

**Status: complete** — `122988c`, test re-baselined in `7ec4528`. Steps are ticked; read them for context, do not re-run them.

Spec §5. This is what replaces the deleted bakery, and it lands in **both** fields at once through one function in `common.glslh` — which is the entire reason that file exists (engine spec §8).

**Files:**
- Create: `tools/convert_materials.sh`, `assets/materials/.gdignore`, `assets/materials/*.png` (generated)
- Create: `extension/src/render/material_atlas.h`, `extension/src/render/material_atlas.cpp`
- Create: `tests/test_material_atlas.gd`
- Modify: `shaders/common.glslh`, `shaders/raymarch.comp.glsl`, `extension/src/render/raymarch_pass.h`, `.cpp`, `extension/src/voxel_world.h`, `.cpp`

**Interfaces:**
- Produces: `godot::MaterialAtlas` with `bool initialize(RenderingDevice *)`, `void teardown()`, `RID albedo_array() const`, `RID surface_array() const`, `RID sampler() const`, `int layer_count() const`; GLSL `vec4 material_surface(uint mat, vec3 wpos, vec3 n, vec3 ddx, vec3 ddy)` and `vec3 shade_terrain(vec4 surf, vec3 n, vec3 wpos)`.

- [x] **Step 1: Write the conversion script**

Create `tools/convert_materials.sh`:

```bash
#!/usr/bin/env bash
# Converts the subset of terrain_textures_vol2 the demo uses into 512^2 PNGs under
# assets/materials/, so the build never depends on a path outside the repo. Run once; the
# outputs are committed. 512^2 is set by memory: 1.4 MB per layer per array with mips, so 16
# materials is ~45 MB, and 1024^2 would be 4x that against the 0.7-1.0 GB brick atlas.
set -euo pipefail

SRC="${1:-/home/jeremy/Development/Unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2}"
DST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/assets/materials"

# Index order IS the texture-array layer order and must match kMaterialNames in
# extension/src/render/material_atlas.cpp. Layer i serves ve material id i + 1; material 0 is
# air and has no layer.
MATERIALS=(grass_01 rock ground_01 breakstone)
MAPS=(basecolor normal roughness ambientOcclusion height)

command -v convert >/dev/null || { echo "need ImageMagick 'convert'" >&2; exit 1; }
[ -d "$SRC" ] || { echo "source not found: $SRC" >&2; exit 1; }
mkdir -p "$DST"

for i in "${!MATERIALS[@]}"; do
	m="${MATERIALS[$i]}"
	for map in "${MAPS[@]}"; do
		in="$SRC/$m/T_${m}_${map}.tga"
		out="$DST/$(printf '%02d' "$i")_${map}.png"
		if [ ! -f "$in" ]; then
			echo "missing $in" >&2
			exit 1
		fi
		convert "$in" -resize 512x512! -strip "PNG24:$out"
		echo "  $out"
	done
done
echo "wrote ${#MATERIALS[@]} materials to $DST"
```

Create `assets/materials/.gdignore` (empty file). It stops Godot's importer touching the PNGs; `MaterialAtlas` reads them with `FileAccess` and decodes with `Image::load_png_from_buffer`, so there is no `.import` sidecar and no `.ctex` in the way.

Run: `chmod +x tools/convert_materials.sh && ./tools/convert_materials.sh`
Expected: 20 PNGs under `assets/materials/`.

- [x] **Step 2: Write the failing test**

Create `tests/test_material_atlas.gd`:

```gdscript
extends GdUnitTestSuite

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
	assert_bool(w.debug_init_atlas()).is_true()
	return w

func test_the_arrays_load_with_mips() -> void:
	var w := make_world()
	var d := w.debug_material_atlas_stats()
	assert_int(d["layers"]).is_greater_equal(4)
	assert_int(d["width"]).is_equal(512)
	assert_int(d["height"]).is_equal(512)
	# Mips are what make the far field resolve to an average instead of aliasing; without
	# them a 2 m tile at 2 km sparkles.
	assert_int(d["mipmaps"]).is_greater_equal(9)
	assert_bool(d["albedo_valid"]).is_true()
	assert_bool(d["surface_valid"]).is_true()

# The near field must now vary WITHIN one material. Before this task every grass pixel was
# exactly material_albedo(1); after it, two points 1 m apart on the same material differ.
func test_the_near_field_gains_texture_detail() -> void:
	var w := make_world()
	var origin := Vector3(20.0, 70.0, 20.0)
	var a: Color = w.debug_raymarch_pixel(origin, Vector3(0.05, -1.0, 0.0).normalized())
	var b: Color = w.debug_raymarch_pixel(origin, Vector3(-0.05, -1.0, 0.0).normalized())
	assert_bool(a.a > 0.0 or b.a > 0.0).override_failure_message(
		"neither probe hit the terrain").is_true()
	var diff := absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)
	assert_float(diff).override_failure_message(
		"two points on the same material shaded identically: no texture is being sampled"
		).is_greater(0.002)

# Sampling must fall back rather than read garbage when a material has no layer.
func test_an_unknown_material_falls_back_to_flat_albedo() -> void:
	var w := make_world()
	var c: Color = w.debug_material_probe(9999, Vector3(10.0, 51.0, 10.0), Vector3(0, 1, 0))
	assert_bool(c.r > 0.9 and c.g < 0.1 and c.b > 0.9).override_failure_message(
		"an out-of-range material id should shade error magenta, got %s" % c).is_true()
```

- [x] **Step 3: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_material_atlas.gd`
Expected: FAIL — `Nonexistent function 'debug_material_atlas_stats'`.

- [x] **Step 4: Write `MaterialAtlas`**

**The layer count is fixed at 16.** `godot::kMaterialLayers = 16`, and every shader that samples the arrays writes `#define MATERIAL_LAYERS 16` before including `common.glslh`. A `#define` rather than a push-constant parameter is deliberate — it is a compile-time bound on an array index, which is what makes an out-of-range material id a cheap branch instead of undefined behaviour. `MaterialAtlas` therefore always allocates 16 layers and fills the ones with no source material with flat error magenta, so the constant and the texture can never disagree. Adding a fifth material means dropping it into `MATERIALS` in the script and bumping nothing.

`initialize` builds two `TEXTURE_TYPE_2D_ARRAY` `R8G8B8A8_UNORM` textures, 512 × 512, `array_layers = kMaterialLayers`, `mipmaps = 10`, usage `SAMPLING_BIT | CAN_UPDATE_BIT`. For each layer it reads the five PNGs, packs

- `albedo` ← basecolor RGB, height in A
- `surface` ← normal XY, roughness in B, AO in A

into a `godot::Image`, calls `Image::generate_mipmaps()`, and appends the resulting `PackedByteArray` (all mips concatenated) as one element of the `data` array — `texture_create(format, view, data)` takes one `PackedByteArray` per layer for `_ARRAY` types (`docs/api/renderingdevice.md:3702`), which is the whole array built in one call with no per-mip `texture_update` loop.

The sampler is `SAMPLER_FILTER_LINEAR` with `mip_filter = LINEAR` and `repeat_u/v = REPEAT` — tiling is what makes a 2 m texture cover a 25 m quad.

`kMaterialNames` in the `.cpp` carries the same order the script uses, and a comment says so on both sides.

- [x] **Step 5: Add the shared shading functions**

Append to `shaders/common.glslh`:

```glsl
// One world tile every 2 m. Mips then do the distance work for free: at 2 km a 2 m tile is
// sub-pixel and resolves to the top mip's average, which is what makes the far field look
// right with no bake at all (spec section 5).
const float MATERIAL_UV_SCALE = 0.5;

// The includer must declare these before including, at bindings it owns:
//   layout(set = ?, binding = ?) uniform sampler2DArray material_albedo;
//   layout(set = ?, binding = ?) uniform sampler2DArray material_surface_tex;
// and define MATERIAL_LAYERS to the array's layer count.
#ifdef MATERIAL_LAYERS

// Triplanar with EXPLICIT gradients. The raymarcher is a compute shader and has no
// dFdx/dFdy, so it supplies gradients from ray differentials while the LoD fragment shader
// supplies dFdx(wpos)/dFdy(wpos). Without this the near field either aliases or over-blurs
// and the seam becomes a visible sharpness step -- the one artefact this whole approach
// exists to avoid.
//
// Returns albedo in rgb and height in a. Material 0 is air; anything without a layer shades
// error magenta, matching material_albedo()'s default.
vec4 material_surface(uint mat, vec3 p, vec3 n, vec3 ddx, vec3 ddy) {
	int layer = int(mat) - 1;
	if (layer < 0 || layer >= MATERIAL_LAYERS) return vec4(1.0, 0.0, 1.0, 0.0);
	vec3 an = abs(n);
	vec3 w = an / max(an.x + an.y + an.z, 1e-5);
	float s = MATERIAL_UV_SCALE;
	vec4 cx = textureGrad(material_albedo, vec3(p.zy * s, float(layer)), ddx.zy * s, ddy.zy * s);
	vec4 cy = textureGrad(material_albedo, vec3(p.xz * s, float(layer)), ddx.xz * s, ddy.xz * s);
	vec4 cz = textureGrad(material_albedo, vec3(p.xy * s, float(layer)), ddx.xy * s, ddy.xy * s);
	return cx * w.x + cy * w.y + cz * w.z;
}

// The lighting that was inlined in raymarch.comp.glsl until M5. Both fields call it, so M6
// replaces the lighting in ONE place instead of two.
vec3 shade_terrain(vec4 surf, vec3 n, vec3 wpos) {
	vec3 sun = normalize(vec3(0.6, 0.8, 0.3));
	float lam = max(dot(n, sun), 0.0);
	return surf.rgb * (0.25 + 0.75 * lam);
}

// A 4x4 Bayer threshold in [0, 1). Task 16 uses it in opposite directions on the two fields,
// so the two masks are exact complements. It lives here for the same reason the shading
// does: there must be exactly one of it.
float bayer4(ivec2 px) {
	const int M[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
	return float(M[(px.y & 3) * 4 + (px.x & 3)]) / 16.0;
}
#endif
```

- [x] **Step 6: Give the raymarcher ray differentials**

In `shaders/raymarch.comp.glsl`, declare the two array samplers and `MATERIAL_LAYERS` before the `common.glslh` include, then replace the shading block at the end of `main()`:

```glsl
	vec3 color = sky_color(rd);
	vec4 hitpos = vec4(0.0);
	if (best.hit) {
		// The pixel's world footprint at the hit: the ray direction's screen derivative
		// scaled by distance. tan_x/tan_y and the target size are already in the push
		// constant, so this costs two multiplies and no extra state.
		vec3 ddx = pc.cam_right.xyz * (2.0 * pc.params.x / float(size.x)) * best.t;
		vec3 ddy = pc.cam_up.xyz * (2.0 * pc.params.y / float(size.y)) * best.t;
		vec4 surf = material_surface(best.mat, best.p, best.n, ddx, ddy);
		color = shade_terrain(surf, best.n, best.p);
		hitpos = vec4(best.p, 1.0);
	}
```

`RaymarchPass::render` gains the two array textures plus the sampler in its uniform set, and `VoxelWorld` owns the `MaterialAtlas` and passes it down. Add `Dictionary debug_material_atlas_stats()` and `Color debug_material_probe(int mat, Vector3 p, Vector3 n)` (a 1×1 dispatch that calls `material_surface` with zero gradients) for the test.

- [x] **Step 7: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then
`./gdunit_tests.sh -a res://tests`
Expected: PASS. `test_raymarch_pixel.gd` and `test_raymarch_magenta.gd` assert specific colours and **will** need re-baselining — the near field is textured now. Re-baseline them by asserting the *hit/miss* structure and the magenta fallback rather than exact albedo, and note the change in the commit message.

- [x] **Step 8: Look at it**

Run: `godot --path . demo/main.tscn`
Expected: the near-field terrain is textured rock/grass/dirt instead of flat colours, with no visible tiling seams and no shimmer when the camera moves. This is the first visible M5 change.

- [x] **Step 9: Commit**

```bash
git add tools/convert_materials.sh assets/materials extension/src/render/material_atlas.h \
        extension/src/render/material_atlas.cpp shaders/common.glslh shaders/raymarch.comp.glsl \
        extension/src/render/raymarch_pass.h extension/src/render/raymarch_pass.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp \
        tests/test_material_atlas.gd tests/test_raymarch_pixel.gd tests/test_raymarch_magenta.gd
git commit -m "feat: triplanar material textures shared by both fields"
```

---

### Task 12: `LodPool` and the `VoxelWorld` LoD tick

**Status: complete** — `ababe6d`, corrected by `be88f82`, `2c3585f`, `d12086e`. Steps are ticked; read them for context, do not re-run them.

The render-device side of the arena, and the loop that turns the walk's requests into builds and its results into pages.

**Files:**
- Create: `extension/src/render/lod_pool.h`, `extension/src/render/lod_pool.cpp`
- Create: `tests/test_lod_pool.gd`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`

**Interfaces:**
- Produces: `godot::LodPool` with `bool initialize(RenderingDevice *, int max_pages)`, `void teardown()`, `bool upload(int level, ve::IVec3 coord, const std::vector<ve::LodQuad> &, std::vector<int> *pages_out)`, `void release(const std::vector<int> &pages)`, `RID quad_buffer()`, `RID index_buffer()`, `RID page_chunk_buffer()`, `RID chunk_buffer()`, `RID args_buffer()`, `int page_count() const`, `int free_pages() const`; `VoxelWorld` gains `set_max_lod_pages`, `set_lod_builds_per_frame`, `lod_tick(const ve::LodCamera &, const ve::LodOcclusion *)`, `Dictionary debug_lod_stats()`.

- [x] **Step 1: Write the failing test**

Create `tests/test_lod_pool.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(pages: int = 256) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.max_lod_pages = pages
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

func test_the_pool_starts_empty_and_sized() -> void:
	var w := make_world(256)
	var d := w.debug_lod_stats()
	assert_int(d["pages_total"]).is_equal(256)
	assert_int(d["pages_free"]).is_equal(256)
	assert_int(d["chunks_resident"]).is_equal(0)

func test_ticking_streams_chunks_in(timeout := 30000) -> void:
	var w := make_world(2048)
	for i in range(200):
		w.debug_lod_tick(Vector3(400.0, 70.0, 400.0), Vector3(0, -0.2, -1))
		await get_tree().process_frame
	var d := w.debug_lod_stats()
	assert_int(d["chunks_resident"]).override_failure_message(
		"200 ticks produced no resident chunks: %s" % d).is_greater(0)
	assert_int(d["pages_free"]).is_less(2048)
	assert_int(d["draw_pages"]).override_failure_message(
		"chunks are resident but nothing is in the draw list").is_greater(0)

# M3 errata 5's lesson, restated for pages: a build that cannot get all its pages must be
# refused, never half-allocated. A tiny pool must degrade to a coarse world, not a broken one.
func test_a_tiny_pool_degrades_to_coarse_instead_of_breaking(timeout := 30000) -> void:
	var w := make_world(24)
	for i in range(200):
		w.debug_lod_tick(Vector3(400.0, 70.0, 400.0), Vector3(0, -0.2, -1))
		await get_tree().process_frame
	var d := w.debug_lod_stats()
	assert_int(d["pages_free"]).is_greater_equal(0)
	assert_int(d["pages_used"] as int + d["pages_free"] as int).is_equal(24)
	assert_bool(d["partial_allocations"] as int == 0).override_failure_message(
		"a build was partially funded").is_true()
	# Something is still drawn: the coarse levels are exempt from eviction.
	assert_int(d["draw_pages"]).is_greater(0)

func test_pages_come_back_when_chunks_are_evicted(timeout := 30000) -> void:
	var w := make_world(2048)
	for i in range(120):
		w.debug_lod_tick(Vector3(400.0, 70.0, 400.0), Vector3(0, -0.2, -1))
		await get_tree().process_frame
	var used_near: int = w.debug_lod_stats()["pages_used"]
	assert_int(used_near).is_greater(0)
	# Jump far away and let the eviction age expire.
	for i in range(400):
		w.debug_lod_tick(Vector3(1500.0, 400.0, 1500.0), Vector3(0, -1, 0))
		await get_tree().process_frame
	var d := w.debug_lod_stats()
	assert_int(d["pages_used"]).override_failure_message(
		"nothing was ever evicted: pages_used %d -> %d" % [used_near, d["pages_used"]]
		).is_less(used_near)
```

- [x] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_lod_pool.gd`
Expected: FAIL — `Invalid assignment of property 'max_lod_pages'`.

- [x] **Step 3: Write `LodPool`**

Buffers, all on the render device:

| Buffer | Size | Contents |
|---|---|---|
| `quads_` | `max_pages · 512 · 12` B | the arena |
| `index_` | 6 KB, `INDEX_BUFFER_FORMAT_UINT16` | `{4q, 4q+1, 4q+2, 4q, 4q+2, 4q+3}` for q in [0, 512), built once at init |
| `page_chunk_` | `max_pages · 4` B | page → chunk-record index |
| `page_quads_` | `max_pages · 4` B | quads actually stored in that page |
| `chunks_` | `8192 · 32` B | `{vec3 origin, float cell, uint level, uint flags, uint pad[2]}` |
| `args_` | `max_pages · 20` B | indirect args, `STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT` |

`upload` calls `ve::LodArena::alloc`, refuses on failure (returning false with `pages_out` untouched), then `buffer_update`s each page's slice of `quads_` plus its `page_chunk_`/`page_quads_` entries and the chunk record. All of it is `buffer_update`, so it must be recorded **before** any list is opened (M2 Task 12's ordering).

- [x] **Step 4: Wire the tick into `VoxelWorld`**

Add the exported properties `max_lod_pages` (default 32768) and `lod_builds_per_frame` (default 8), the `ve::LodTree *lod_tree_`, the `LodPool *lod_pool_`, and:

```cpp
void VoxelWorld::lod_tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ) {
	if (!lod_tree_ || !lod_pool_) return;
	lod_tree_->walk(cam, occ, ++lod_frame_, &lod_walk_);

	// Results first: a page that arrives this frame should be drawable this frame.
	std::vector<LodBuildResult> done;
	if (mesh_ && mesh_->collect_lod(&done) > 0) {
		for (LodBuildResult &r : done) {
			if (r.failed) { lod_tree_->note_failed(r.level, r.coord); continue; }
			if (r.quads.empty()) { lod_tree_->note_empty(r.level, r.coord); continue; }
			std::vector<int> pages;
			if (!lod_pool_->upload(r.level, r.coord, r.quads, &pages)) {
				// Refused, not half-funded: ask the tree for pages and retry next frame.
				lod_tree_->note_failed(r.level, r.coord);
				lod_pressure_ = ve::lod_pages_for_quads(int(r.quads.size()));
				continue;
			}
			lod_tree_->note_ready(r.level, r.coord, pages.front(), int(pages.size()));
			lod_pages_of_[{r.level, r.coord}] = pages;
		}
	}

	// Then evictions, so the budget below sees the pages they returned.
	std::vector<ve::LodDrawItem> evicted;
	lod_tree_->collect_evictions(lod_frame_, lod_pressure_, &evicted);
	lod_pressure_ = 0;
	for (const ve::LodDrawItem &e : evicted) {
		const auto it = lod_pages_of_.find({e.level, e.coord});
		if (it == lod_pages_of_.end()) continue;
		lod_pool_->release(it->second);
		lod_pages_of_.erase(it);
	}

	// Then this frame's builds, priority order, one batch.
	if (mesh_ && !mesh_->lod_busy()) {
		const int take = std::min<int>(lod_builds_per_frame_, int(lod_walk_.requests.size()));
		std::vector<LodBuildJob> batch;
		batch.reserve(size_t(take));
		for (int i = 0; i < take; i++) {
			const ve::LodBuildRequest &q = lod_walk_.requests[size_t(i)];
			LodBuildJob j;
			j.level = q.level;
			j.coord = q.coord;
			gather_lod_ops(q.level, q.coord, &j.ops);
			batch.push_back(std::move(j));
		}
		// Mark building only for the jobs that were actually ACCEPTED, and only the ones in
		// the batch: marking a request the submit refused would leave a node permanently in
		// kLodBuilding, and request() skips those, so it would never be built again.
		if (!batch.empty() && mesh_->submit_lod(std::move(batch)))
			for (int i = 0; i < take; i++)
				lod_tree_->note_building(lod_walk_.requests[size_t(i)].level,
						lod_walk_.requests[size_t(i)].coord);
	}
}
```

`gather_lod_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out)` is a private `VoxelWorld` helper shared with Task 10's `debug_lod_submit`: it takes `edit_mutex_`, calls `ve::lod_chunk_aabb` padded by two cells, runs `ve::collect_ops_for_aabb`, and truncates to a chronological **prefix** of `ve::kMaxRegionOps` (M4 errata 1 — the flattened cross-region list can exceed the cap, and a prefix of an ordered CSG list is a valid world state where a suffix can apply an add without the subtract that made room for it).

Add `debug_lod_tick(Vector3 pos, Vector3 fwd)` (builds a `ve::LodCamera` with `lod_camera_perspective` and calls `lod_tick`) and `debug_lod_stats()` returning `pages_total`, `pages_free`, `pages_used`, `chunks_resident`, `draw_pages`, `partial_allocations`, `builds_in_flight`.

- [x] **Step 5: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then the gdUnit command from Step 2.
Expected: PASS.

- [x] **Step 6: Commit**

```bash
git add extension/src/render/lod_pool.h extension/src/render/lod_pool.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_pool.gd
git commit -m "feat: lod page pool and the world's lod tick"
```

---

### Task 13: `LodRasterPass` — the far field on screen

**Status: complete** — `52c9fb3`, corrected by `a2e0a63` and `c15a1ae`. Steps are ticked; read them for context, do not re-run them.

Spec §7.1–7.3. One indirect multi-draw into Godot's scene framebuffer, pre-opaque, after the composite. Indirect args are built on the CPU here; Task 15 replaces that with a GPU pass that additionally culls.

**Files:**
- Create: `shaders/lod_quad.glslh`, `shaders/lod.vert.glsl`, `shaders/lod.frag.glsl`
- Create: `extension/src/render/lod_raster_pass.h`, `extension/src/render/lod_raster_pass.cpp`
- Create: `tests/test_lod_render.gd`
- Modify: `shaders/lod_common.glslh` (now includes `lod_quad.glslh`), `extension/src/raymarch_compositor.cpp`, `extension/src/voxel_world.h`, `.cpp`

**Interfaces:**
- Produces: `godot::LodRasterPass` with `void initialize(RenderingDevice *)`, `void teardown()`, `bool draw(RenderingDevice *, LodPool &, MaterialAtlas &, RID dst_color, RID dst_depth, const Projection &view_proj, const float cam_pos[3], int draw_count)`; `VoxelWorld::debug_lod_render_probe(Vector3 pos, Vector3 fwd, int w, int h)`.

- [x] **Step 1: Split the shared GLSL out of `lod_common.glslh`**

Create `shaders/lod_quad.glslh` holding everything the *raster* also needs, and delete those lines from `lod_common.glslh`, which now begins with an include of this file:

```glsl
// The packed 12-byte quad record and its geometry, shared by the build passes and the
// raster. Mirror of extension/src/lod/lod_quad.h and lod/lod_grid.h; the differential test
// in tests/test_lod_mesh_diff.gd fails when the two drift. Include common.glslh first.
const int LOD_CHUNK_CELLS = 32;       // ve::kLodChunkCells
const int LOD_CHUNK_MESH_CELLS = 33;  // ve::kLodChunkMeshCells
const int LOD_CHUNK_LATTICE = 34;     // ve::kLodChunkLattice
const int LOD_FINE_LATTICE = 69;      // ve::kLodFineLattice
const int LOD_QUADS_PER_PAGE = 512;   // ve::kLodQuadsPerPage
const int LOD_PAGE_SHIFT = 9;         // log2(LOD_QUADS_PER_PAGE); page = quad >> this
const int LOD_MAX_QUADS = 8192;       // ve::kLodMaxQuadsPerChunk
const int LOD_OFFSET_MAX = 31;        // ve::kLodOffsetMax

// The four cells around a lattice edge, wound counter-clockwise seen from +axis. Byte-
// identical to ve::kLodQuadCorners and to QUAD in shaders/mesh_quads.comp.glsl.
const ivec2 LOD_QUAD[4] = ivec2[4](ivec2(-1, -1), ivec2(0, -1), ivec2(0, 0), ivec2(-1, 0));

// A field can straddle at most two of the three words (the widest is 16 bits), so two masked
// reads always suffice. Mirror of bits_get in extension/src/lod/lod_quad.cpp.
uint lod_bits_get(uvec3 w, int lo, int bits) {
	uint mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << uint(bits)) - 1u);
	int word = lo >> 5;
	int shift = lo & 31;
	uint v = (word == 0 ? w.x : (word == 1 ? w.y : w.z)) >> uint(shift);
	int spill = shift + bits - 32;
	if (spill > 0) {
		uint hi = (word == 0 ? w.y : w.z);
		v |= hi << uint(32 - shift);
	}
	return v & mask;
}

// World position of corner k: origin + (m - 1 + frac) * cell -- the mesher's own formula,
// which ve::lod_quad_corner_pos and shaders/mesh_cells.comp.glsl both use.
vec3 lod_corner_pos(uvec3 w, int k, vec3 origin, float cell) {
	int axis = int(lod_bits_get(w, 15, 2));
	int b = (axis + 1) % 3;
	int c = (axis + 2) % 3;
	ivec3 m = ivec3(int(lod_bits_get(w, 0, 5)), int(lod_bits_get(w, 5, 5)),
			int(lod_bits_get(w, 10, 5))) + 1;
	m[b] += LOD_QUAD[k].x;
	m[c] += LOD_QUAD[k].y;
	vec3 frac = vec3(lod_bits_get(w, 18 + (k * 3 + 0) * 5, 5),
			lod_bits_get(w, 18 + (k * 3 + 1) * 5, 5),
			lod_bits_get(w, 18 + (k * 3 + 2) * 5, 5)) / float(LOD_OFFSET_MAX);
	return origin + (vec3(m) - 1.0 + frac) * cell;
}
```

- [x] **Step 2: Write the failing test**

Create `tests/test_lod_render.gd`:

```gdscript
extends GdUnitTestSuite

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
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

# The walk descends only into a node whose eight children are all resident, so the far field
# converges over HUNDREDS of ticks and at a rate set by build throughput, not by frame count.
# Wait on the condition (errata 6): the fixed counts this plan first used were tuned to a
# fixed frame counts were tuned to the old occluded 590 ms vsync frame and settle nothing on the current runner, which keeps vsync enabled intentionally (gdunit_tests.sh) at a normal display rate.
# requests_pending comes from the walk that ran BEFORE this tick collected its results, so it
# dips to zero for a tick or two while a batch lands -- the streak is what makes it mean
# "converged" rather than "between batches". Measured: ~350-400 ticks, so the budget is margin.
const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

func test_the_far_field_covers_the_ground(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var r := w.debug_lod_render_probe(pos, fwd, 256, 144)
	assert_int(r["draw_pages"]).override_failure_message(
		"nothing was submitted to the draw: %s" % r).is_greater(0)
	# Looking down at terrain from 90 m: the lower half of the frame must be covered.
	assert_float(r["coverage"]).override_failure_message(
		"the far field covered %.3f of the frame" % r["coverage"]).is_greater(0.25)

func test_depth_is_written_so_the_near_field_can_occlude(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var r := w.debug_lod_render_probe(pos, fwd, 256, 144)
	# Reverse-Z: every covered pixel must hold a depth strictly between far (0) and near (1).
	assert_float(r["depth_min"]).is_greater(0.0)
	assert_float(r["depth_max"]).is_less(1.0)
	assert_float(r["depth_max"]).is_greater(r["depth_min"])

func test_nothing_is_drawn_inside_the_near_field(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var r := w.debug_lod_render_probe(pos, fwd, 256, 144)
	# Spec section 6.4: a chunk entirely inside the fade start is never even built.
	assert_float(r["nearest_hit_m"]).override_failure_message(
		"the far field drew geometry at %.1f m, inside the 120 m fade start" % r["nearest_hit_m"]
		).is_greater_equal(100.0)

func test_backface_culling_does_not_remove_visible_ground(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var off := w.debug_lod_render_probe_culled(pos, fwd, 256, 144, false)
	var on := w.debug_lod_render_probe_culled(pos, fwd, 256, 144, true)
	# M3 errata 1: this codebase's winding convention has already cost one bug, so the
	# front-face setting is MEASURED, not assumed. Culling backfaces must not lose coverage.
	assert_float(on["coverage"]).override_failure_message(
		"backface culling dropped coverage from %.3f to %.3f: the front-face setting is wrong"
		% [off["coverage"], on["coverage"]]).is_greater(off["coverage"] * 0.95)
```

- [x] **Step 3: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_lod_render.gd`
Expected: FAIL — `Nonexistent function 'debug_lod_render_probe'`.

- [x] **Step 4: Write `shaders/lod.vert.glsl`**

```glsl
#[vertex]
#version 460

#include "common.glslh"
#include "lod_quad.glslh"

// No vertex buffer and no vertex attributes: geometry is PULLED. The shared index buffer
// supplies {4q, 4q+1, 4q+2, 4q, 4q+2, 4q+3} for q in [0, 512) and each page's draw sets
// vertexOffset = page * 2048, so gl_VertexIndex recovers both the global quad index and the
// page. This is Voxy's gl_VertexID>>2 trick, and it routes around Godot exposing neither
// gl_DrawID nor a non-zero firstInstance.
layout(set = 0, binding = 0, std430) readonly buffer Quads { uint v[]; } quads;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
// Two vec4 per chunk: (origin.xyz, cell size), (level, flags, pad, pad).
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam; // xyz = camera position, w = unused
} pc;

layout(location = 0) out vec3 v_wpos;
layout(location = 1) out flat vec3 v_normal;
layout(location = 2) out flat uint v_material;

void main() {
	uint vi = uint(gl_VertexIndex);
	uint quad = vi >> 2;
	uint corner = vi & 3u;
	uint page = quad >> uint(LOD_PAGE_SHIFT);
	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];

	uvec3 w = uvec3(quads.v[quad * 3u + 0u], quads.v[quad * 3u + 1u], quads.v[quad * 3u + 2u]);

	// All four corners, because the flat normal comes from the geometry rather than storage:
	// at a 3 px screen-space error a quad is smaller than any shading gradient, so a stored
	// per-corner normal would cost 8 bytes a quad to change nothing (spec section 3.4).
	vec3 p0 = lod_corner_pos(w, 0, c0.xyz, c0.w);
	vec3 p1 = lod_corner_pos(w, 1, c0.xyz, c0.w);
	vec3 p2 = lod_corner_pos(w, 2, c0.xyz, c0.w);
	vec3 p3 = lod_corner_pos(w, 3, c0.xyz, c0.w);

	v_wpos = corner == 0u ? p0 : (corner == 1u ? p1 : (corner == 2u ? p2 : p3));
	// Corners are stored ALREADY WOUND, so this never branches on the sign bit.
	v_normal = normalize(cross(p1 - p0, p2 - p0));
	v_material = lod_bits_get(w, 78, 16);
	gl_Position = pc.view_proj * vec4(v_wpos, 1.0);
}
```

- [x] **Step 5: Write `shaders/lod.frag.glsl`**

```glsl
#[fragment]
#version 460

#define MATERIAL_LAYERS 16
layout(set = 0, binding = 3) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 4) uniform sampler2DArray material_surface_tex;
#include "common.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in flat vec3 v_normal;
layout(location = 2) in flat uint v_material;

layout(location = 0) out vec4 frag_color;

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam; // xyz = camera position, w = unused
} pc;

void main() {
	// Explicit gradients are not needed here -- a fragment shader has them -- but the SAME
	// function the raymarcher calls is, so the two fields cannot drift (spec section 5).
	vec4 surf = material_surface(v_material, v_wpos, v_normal, dFdx(v_wpos), dFdy(v_wpos));
	frag_color = vec4(shade_terrain(surf, v_normal, v_wpos), 1.0);
}
```

- [x] **Step 6: Write `LodRasterPass`**

The pipeline mirrors `CompositePass::ensure_pipeline` (`extension/src/render/composite_pass.cpp:79-129`) with three deviations, each load-bearing:

- **Vertex format `RenderingDevice::INVALID_ID`.** An *empty* vertex format is a valid format that expects vertices and makes the draw `ERR_FAIL` with "No vertex array was bound" — `CompositePass`'s comment records this. `INVALID_ID` is what a pull-only pipeline needs. **This is spec §11's first spike:** `CompositePass` proves the vertexless half, but *indexed* drawing with `INVALID_ID` is unverified on 4.7.1. If `draw_list_bind_index_array` rejects it, fall back to the non-indexed form — `draw_list_draw_indirect(dl, false, args, 0, count, 20)` with `firstVertex = quad · 6` and `quad = gl_VertexIndex / 6`, which needs no index buffer at all and costs six vertices per quad through a shader that does no vertex fetch. Record whichever path works in the Errata.
- **Depth state:** `enable_depth_test`, `enable_depth_write`, `COMPARE_OP_GREATER_OR_EQUAL` (reverse-Z, M1 errata 2). Identical to `CompositePass`.
- **Cull mode:** `POLYGON_CULL_BACK`. Build **both** front-face variants at init and let `draw` pick; the probe hook below is what decides which.

`draw` records `buffer_update` for the args (before opening the draw list — M2's ordering), then `draw_list_begin(framebuffer, DRAW_DEFAULT_ALL)`, binds the pipeline, uniform set and index array, sets the 80-byte push constant, and issues **one** `draw_list_draw_indirect(dl, true, args, 0, draw_count, 20)`.

- [x] **Step 7: Add the probe hooks and determine the front face**

`VoxelWorld::debug_lod_render_probe(pos, fwd, w, h)` creates a throwaway colour+depth target, runs `lod_tick` once, calls `LodRasterPass::draw`, reads both back, and returns `coverage` (fraction of pixels whose depth ≠ the cleared value), `depth_min`, `depth_max`, `nearest_hit_m` (from the largest reverse-Z depth) and `draw_pages`. `debug_lod_render_probe_culled(pos, fwd, w, h, cull)` is the same with culling forced off or on.

Run the front-face experiment:

```bash
./gdunit_tests.sh -a res://tests/test_lod_render.gd
```

If `test_backface_culling_does_not_remove_visible_ground` fails, flip the pipeline's
`set_front_face` between `POLYGON_FRONT_FACE_CLOCKWISE` and `POLYGON_FRONT_FACE_COUNTER_CLOCKWISE`, re-run, and **record the answer in this plan's Errata**. Do not "fix" it by changing the winding in `lod_contour.cpp`, `lod_quads.comp.glsl` or the index buffer — those three agree with `ve::dual_contour` and with Jolt, and Task 9's differential test is what holds them together.

- [x] **Step 8: Call it from the compositor**

In `RaymarchCompositor::_render_callback`, after `cmp->draw(...)`, build a `ve::LodCamera` from the scene data (`view_proj` from `proj * Projection(cam.affine_inverse())`, exactly as the composite already computes it), call `world->lod_tick(lod_cam, nullptr)` — the occlusion argument stays null until Task 14 — and then `lod_raster->draw(...)` into the same `rsb->get_color_texture()` / `rsb->get_depth_texture()` the composite just wrote.

- [x] **Step 9: Run the tests and look at it**

Run: `./build.sh -j$(nproc)` then the gdUnit command from Step 3, then the full suite.
Run: `godot --path . demo/main.tscn`
Expected: terrain now extends past 150 m to the horizon instead of ending in sky. The seam at 150 m is a hard edge — Task 16 fixes that. Level changes pop — that is expected and is what the ratio of 2 makes tolerable.

- [x] **Step 10: Commit**

```bash
git add shaders/lod_quad.glslh shaders/lod_common.glslh shaders/lod.vert.glsl \
        shaders/lod.frag.glsl extension/src/render/lod_raster_pass.h \
        extension/src/render/lod_raster_pass.cpp extension/src/raymarch_compositor.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_render.gd
git commit -m "feat: rasterize the far field with one indirect multi-draw"
```

---

### Task 14: `HizPass` — the depth pyramid and the streaming readback

Spec §6.3 and §7.1. The near field already writes exact pre-opaque depth, so the occluder is free; this task turns it into a pyramid the GPU can test against and a 4 KB grid the CPU walk can.

**Files:**
- Create: `shaders/hiz.comp.glsl`, `extension/src/render/hiz_pass.h`, `.cpp`
- Create: `tests/test_hiz.gd`
- Modify: `extension/src/render/async_readback.h`, `.cpp` (add `AsyncTextureRead`), `extension/src/register_types.cpp`, `extension/src/raymarch_compositor.cpp`, `extension/src/voxel_world.h`, `.cpp`

**Interfaces:**
- Produces: `godot::HizPass` with `bool initialize(RenderingDevice *)`, `void teardown()`, `bool build(RenderingDevice *, RID scene_depth, Vector2i scene_size)`, `RID pyramid() const`, `int mip_count() const`, `const ve::LodOcclusion *occlusion() const`; an internal `HizOcclusion : ve::LodOcclusion` reading the async 32² readback.

- [x] **Step 1: Write the failing test**

Create `tests/test_hiz.gd`:

```gdscript
extends GdUnitTestSuite

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
	assert_bool(w.debug_init_atlas()).is_true()
	return w

func test_the_pyramid_is_fixed_size_and_fully_mipped() -> void:
	var w := make_world()
	var d := w.debug_hiz_stats()
	assert_int(d["width"]).is_equal(256)
	assert_int(d["height"]).is_equal(256)
	assert_int(d["mips"]).is_equal(9)
	assert_int(d["readback_level"]).is_equal(3)
	assert_int(d["readback_texels"]).is_equal(32 * 32)

# Reverse-Z: nearer is LARGER, so the conservative reduction is a MIN -- it keeps the
# FARTHEST of the nearest surfaces, which is the only value a whole footprint can be tested
# against without wrongly hiding something.
func test_the_reduction_is_a_min_in_reverse_z() -> void:
	var w := make_world()
	# A synthetic depth image: one near texel (0.9) in a far field (0.1).
	var d := w.debug_hiz_probe_synthetic(0.1, 0.9)
	assert_float(d["mip0_at_near_texel"]).is_equal_approx(0.9, 0.001)
	# The parent covering both must keep the FAR value.
	assert_float(d["mip1_covering_both"]).is_equal_approx(0.1, 0.001)
	assert_float(d["top_mip"]).is_equal_approx(0.1, 0.001)

# The whole point of the split in spec section 6.3: stale occlusion may delay a build, never
# hide a chunk. A box in front of every occluder must never test occluded.
func test_a_box_in_front_of_everything_is_never_occluded() -> void:
	var w := make_world()
	w.debug_hiz_probe_synthetic(0.1, 0.1) # everything far
	# ss box covering the whole screen, nearest depth 0.9 (well in front).
	assert_bool(w.debug_hiz_occluded(Vector2(0.0, 0.0), Vector2(1.0, 1.0), 0.9)).is_false()
	# ...and the same box behind everything is.
	w.debug_hiz_probe_synthetic(0.9, 0.9) # everything near
	assert_bool(w.debug_hiz_occluded(Vector2(0.0, 0.0), Vector2(1.0, 1.0), 0.1)).is_true()

func test_an_absent_readback_never_occludes() -> void:
	var w := make_world()
	# Before any build has landed there is no data; the safe answer is "visible".
	assert_bool(w.debug_hiz_occluded(Vector2(0.2, 0.2), Vector2(0.3, 0.3), 0.01)).is_false()
```

- [x] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_hiz.gd`
Expected: FAIL — `Nonexistent function 'debug_hiz_stats'`.

- [x] **Step 3: Write `shaders/hiz.comp.glsl`**

```glsl
#[compute]
#version 460

// Two entry paths behind one shader: level 0 reduces the scene depth into a fixed 256^2
// pyramid (so the CPU readback is resolution-independent), and every level after that
// reduces 2x2 from the level above.
//
// REVERSE-Z (M1 errata 2): near = 1.0, far = 0.0, so the conservative reduction is a MIN.
// It keeps the FARTHEST of the nearest surfaces over a footprint, which is the only value a
// whole node can be tested against without ever wrongly hiding it.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, r32f) writeonly uniform image2D dst;

layout(push_constant, std430) uniform Push {
	ivec4 dims;  // xy = destination size, zw = source size
	ivec4 flags; // x = 1 when the source is the scene depth (level 0), else 0
} pc;

void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(p, pc.dims.xy))) return;

	float m = 1.0;
	if (pc.flags.x == 1) {
		// The scene is not 256^2 and not a power of two, so each destination texel owns a
		// rectangle of source texels. Reduce all of them: sampling one would let a thin
		// near sliver claim the whole texel and hide what is behind it.
		ivec2 lo = (p * pc.dims.zw) / pc.dims.xy;
		ivec2 hi = ((p + 1) * pc.dims.zw + pc.dims.xy - 1) / pc.dims.xy;
		hi = min(hi, pc.dims.zw);
		for (int y = lo.y; y < hi.y; y++)
			for (int x = lo.x; x < hi.x; x++)
				m = min(m, texelFetch(src, ivec2(x, y), 0).r);
	} else {
		ivec2 s = p * 2;
		m = min(min(texelFetch(src, s, 0).r, texelFetch(src, s + ivec2(1, 0), 0).r),
				min(texelFetch(src, s + ivec2(0, 1), 0).r, texelFetch(src, s + ivec2(1, 1), 0).r));
	}
	imageStore(dst, p, vec4(m));
}
```

- [x] **Step 4: Write `HizPass`**

A single `R32_SFLOAT` 256×256 texture with 9 mips and per-mip texture *slices* to bind as the destination. `build` records nine dispatches; a mip's dispatch reads the previous mip through a `sampler2D` view of that slice.

**The 4 KB readback needs a copy first.** `RenderingDevice::texture_get_data_async(texture, layer, callback)` takes a **layer, not a mip** (`docs/api/renderingdevice.md:3744`), so asking it for the pyramid downloads the whole layer — ~349 KB with its mip chain, against spec §6.3's ~2 KB. Allocate a second, single-mip 32² `R32_SFLOAT` texture with `TEXTURE_USAGE_CAN_COPY_TO_BIT | TEXTURE_USAGE_CAN_COPY_FROM_BIT`, give the pyramid `TEXTURE_USAGE_CAN_COPY_FROM_BIT`, and after the ninth dispatch record

```cpp
rd->texture_copy(pyramid_, readback_, Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(32, 32, 1),
        /*src_mipmap*/ 3, /*dst_mipmap*/ 0, /*src_layer*/ 0, /*dst_layer*/ 0);
```

then issue `texture_get_data_async(readback_, 0, ...)`, which is exactly 4 KB. Keeping ONE mipped pyramid for the GPU is what lets Task 15's cull do `texelFetch(hiz, p, ml)` at a level it picks per node; the 32² copy exists only for the CPU.

**The holder is a new class, not `AsyncBufferRead`.** That one wraps `buffer_get_data_async` and takes a buffer RID. Add `AsyncTextureRead` beside it in `async_readback.h/.cpp` with the same shape — a `RefCounted` `GDCLASS` whose `_on_data` is named by `callable_mp`, plus `request()` / `take_fresh()` / `data()` — and register it with `GDREGISTER_INTERNAL_CLASS(AsyncTextureRead)` in `register_types.cpp`. That is the one file the File Structure above calls unchanged; this task is the exception (errata 8).

`HizOcclusion::occluded(ss_min, ss_max)` implements the CPU test:

```cpp
bool HizOcclusion::occluded(const float ss_min[3], const float ss_max[3]) const {
	if (!have_data_) return false; // no readback yet: the safe answer is always "visible"
	const int lo_x = std::max(0, int(std::floor(ss_min[0] * kGrid)));
	const int hi_x = std::min(kGrid - 1, int(std::ceil(ss_max[0] * kGrid)) - 1);
	const int lo_y = std::max(0, int(std::floor(ss_min[1] * kGrid)));
	const int hi_y = std::min(kGrid - 1, int(std::ceil(ss_max[1] * kGrid)) - 1);
	if (lo_x > hi_x || lo_y > hi_y) return false;
	float occluder = 1.0f;
	for (int y = lo_y; y <= hi_y; y++)
		for (int x = lo_x; x <= hi_x; x++)
			occluder = std::min(occluder, grid_[y * kGrid + x]);
	// Reverse-Z: the node's NEAREST point is its largest depth. If even that is behind the
	// farthest occluder over its footprint, everything in the node is behind everything
	// drawn there.
	return ss_max[2] < occluder;
}
```

- [x] **Step 5: Wire it in and hand it to the walk**

In `RaymarchCompositor::_render_callback`, between `cmp->draw(...)` and the LoD draw, call `hiz->build(rd, rsb->get_depth_texture(), size)`. Change the `lod_tick` call to pass `hiz->occlusion()` instead of `nullptr`.

Add `debug_hiz_stats()`, `debug_hiz_probe_synthetic(float far_value, float near_value)` (uploads a synthetic depth image, runs the pyramid, reads three specific texels back) and `debug_hiz_occluded(Vector2 lo, Vector2 hi, float depth)`.

- [x] **Step 6: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then the gdUnit command from Step 2, then the full suite.
Expected: PASS. `test_lod_pool.gd::test_ticking_streams_chunks_in` must still pass — occlusion may only delay builds, and its "no readback yet" answer is `false`.

- [x] **Step 7: Commit**

```bash
git add shaders/hiz.comp.glsl extension/src/render/hiz_pass.h \
        extension/src/render/hiz_pass.cpp extension/src/raymarch_compositor.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_hiz.gd
git commit -m "feat: hiz pyramid from near-field depth, driving lod streaming"
```

---

### Task 15: `LodCullPass` — GPU cull and command generation

Spec §7.2. The CPU walk is already the candidate list, so this pass only ever *removes* — which is what lets `draw_count` stay an exact CPU integer despite Godot exposing no count buffer.

**Files:**
- Create: `shaders/lod_cull.comp.glsl`, `extension/src/render/lod_cull_pass.h`, `.cpp`
- Create: `tests/test_lod_cull.gd`
- Modify: `extension/src/render/lod_raster_pass.cpp`, `extension/src/raymarch_compositor.cpp`

- [x] **Step 1: Write the failing test**

Create `tests/test_lod_cull.gd`:

```gdscript
extends GdUnitTestSuite

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
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

# The walk descends only into a node whose eight children are all resident, so the far field
# converges over HUNDREDS of ticks and at a rate set by build throughput, not by frame count.
# Wait on the condition (errata 6): the fixed counts this plan first used were tuned to a
# fixed frame counts were tuned to the old occluded 590 ms vsync frame and settle nothing on the current runner, which keeps vsync enabled intentionally (gdunit_tests.sh) at a normal display rate.
# requests_pending comes from the walk that ran BEFORE this tick collected its results, so it
# dips to zero for a tick or two while a batch lands -- the streak is what makes it mean
# "converged" rather than "between batches". Measured: ~350-400 ticks, so the budget is margin.
const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# The cull only ever ZEROES instanceCount. It must never change a page's vertexOffset or its
# index count, because those are what address the arena -- a cull that rewrote them would
# draw one chunk's geometry at another chunk's origin.
func test_the_cull_only_removes(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.debug_lod_cull_probe(pos, fwd)
	assert_int(d["args_before"]).is_greater(0)
	assert_int(d["args_after"]).is_equal(d["args_before"])
	assert_int(d["offsets_changed"]).override_failure_message(
		"the cull rewrote %d vertex offsets" % d["offsets_changed"]).is_equal(0)
	assert_int(d["index_counts_changed"]).is_equal(0)
	assert_int(d["drawn_after"]).is_less_equal(d["args_before"])

func test_facing_away_culls_almost_everything(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var facing := w.debug_lod_cull_probe(pos, fwd)
	# Same resident set, camera spun to face straight up into empty sky.
	var away := w.debug_lod_cull_probe(pos, Vector3(0.0, 1.0, 0.0))
	assert_int(away["drawn_after"]).override_failure_message(
		"looking at the sky still drew %d pages" % away["drawn_after"]
		).is_less(facing["drawn_after"] / 2)

func test_the_reported_ratio_is_sane(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.debug_lod_cull_probe(pos, fwd)
	assert_float(d["culled_ratio"]).is_between(0.0, 1.0)
```

- [x] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_lod_cull.gd`
Expected: FAIL — `Nonexistent function 'debug_lod_cull_probe'`.

- [x] **Step 3: Write `shaders/lod_cull.comp.glsl`**

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "lod_quad.glslh"

// One thread per candidate page. The CPU walk already decided WHAT could be drawn, so this
// pass only ever removes -- it zeroes instanceCount and touches nothing else. That is what
// keeps draw_count an exact CPU integer: Godot's draw_list_draw_indirect takes the count as
// a parameter and exposes no count buffer, so a GPU-decided cut would have to issue tens of
// thousands of empty draws.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) buffer Args { uint v[]; } args;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;
layout(set = 0, binding = 3) uniform sampler2D hiz;
layout(set = 0, binding = 4, std430) buffer Stats { uint v[]; } stats; // [0] = drawn

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 planes[6];
	ivec4 params; // x = page count, y = hiz size, z = hiz mips, w = unused
} pc;

bool outside_frustum(vec3 lo, vec3 hi) {
	for (int i = 0; i < 6; i++) {
		vec3 p = mix(lo, hi, step(vec3(0.0), pc.planes[i].xyz));
		if (dot(pc.planes[i].xyz, p) + pc.planes[i].w < 0.0) return true;
	}
	return false;
}

void main() {
	uint page = gl_GlobalInvocationID.x;
	if (page >= uint(pc.params.x)) return;
	uint base = page * 5u;
	if (args.v[base + 1u] == 0u) return; // already empty

	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];
	vec3 lo = c0.xyz;
	vec3 hi = lo + vec3(c0.w * float(LOD_CHUNK_CELLS));

	if (outside_frustum(lo, hi)) { args.v[base + 1u] = 0u; return; }

	// Screen-space box, and the node's NEAREST reverse-Z depth.
	vec2 mn = vec2(1e30), mx = vec2(-1e30);
	float near_z = 0.0;
	for (int k = 0; k < 8; k++) {
		vec3 p = vec3((k & 1) != 0 ? hi.x : lo.x, (k & 2) != 0 ? hi.y : lo.y,
				(k & 4) != 0 ? hi.z : lo.z);
		vec4 clip = pc.view_proj * vec4(p, 1.0);
		// Straddling the near plane makes the divide meaningless; the only safe answer is
		// "keep it".
		if (clip.w <= 1e-4) { atomicAdd(stats.v[0], 1u); return; }
		vec3 ndc = clip.xyz / clip.w;
		mn = min(mn, ndc.xy * 0.5 + 0.5);
		mx = max(mx, ndc.xy * 0.5 + 0.5);
		near_z = max(near_z, ndc.z);
	}
	mn = clamp(mn, vec2(0.0), vec2(1.0));
	mx = clamp(mx, vec2(0.0), vec2(1.0));

	// Pick the mip whose texels are at least as large as the box, so the loop below is a
	// handful of fetches whatever the box's size.
	vec2 span = (mx - mn) * float(pc.params.y);
	int ml = clamp(int(floor(log2(max(max(span.x, span.y), 1.0)))), 0, pc.params.z - 1);
	int size = max(1, pc.params.y >> ml);
	ivec2 a = ivec2(floor(mn * float(size)));
	ivec2 b = min(ivec2(ceil(mx * float(size))), ivec2(size - 1));

	float occluder = 1.0;
	for (int y = a.y; y <= b.y; y++)
		for (int x = a.x; x <= b.x; x++)
			occluder = min(occluder, texelFetch(hiz, ivec2(x, y), ml).r);

	// Reverse-Z: if the node's nearest point is behind the farthest occluder over its whole
	// footprint, nothing in it can be seen.
	if (near_z < occluder) { args.v[base + 1u] = 0u; return; }
	atomicAdd(stats.v[0], 1u);
}
```

- [x] **Step 4: Move the indirect args out of the raster, then write `LodCullPass`**

**Do this first or the cull is a no-op.** Task 13 built the indirect args *inside* `LodRasterPass::draw` — the `buffer_update(pool.args_buffer(), ...)` recorded just before `draw_list_begin`. The cull runs BEFORE the draw, so leaving it there means the raster rewrites every `instanceCount` the cull just zeroed and nothing is ever culled, with both tests in Step 1 passing vacuously because `args_after == args_before` either way. Move the args build to `LodPool::upload_draw_args(const std::vector<LodRasterPass::PageDraw> &)`, called from the compositor before the cull; `LodRasterPass::draw` then keeps only the `draw_list_*` calls and the `draw_count` it already takes.

`initialize` builds the pipeline; `run(rd, pool, hiz, view_proj, page_count)` clears `stats`, records the dispatch at `ceil(page_count / 64)`, and leaves the args buffer ready for the raster's `draw_list_draw_indirect`. The stats buffer is read back asynchronously for the HUD's culled ratio — never synchronously.

In `RaymarchCompositor`, insert `cull->run(...)` between `hiz->build(...)` and `lod_raster->draw(...)`. **Ordering:** the args `buffer_update` must be recorded before the cull's `compute_list_begin`, and the cull's compute list must be ended before the raster's `draw_list_begin`.

Add `debug_lod_cull_probe(pos, fwd)`, which snapshots the args buffer before and after the cull and reports `args_before`, `args_after`, `offsets_changed`, `index_counts_changed`, `drawn_after`, `culled_ratio`.

- [x] **Step 5: Give the far field a measured cost**

Nothing times the LoD passes yet, and Task 18's HUD and benchmark both read one. Add CPU
command-record timing to `LodRasterPass` and `LodCullPass` in the style the other passes
already use (`WorldStreamer::last_total_ms`, `ColliderStreamer::last_build_ms`), and surface
it from `VoxelWorld::debug_perf_stats()` as `lod_ms` (raster + cull) alongside the existing
`stream_total_ms` / `island_ms`. This is NOT GPU execution time; it is the std::chrono cost of
recording the command lists (see errata 15). Report `culled_ratio` from the cull's async stats
readback in `debug_lod_stats()` so the HUD reads it from the same place as the rest of the LoD
numbers.

- [x] **Step 6: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then the gdUnit command from Step 2, then the full suite.
Run: `godot --path . demo/main.tscn`
Expected: PASS, and the frame time drops when facing terrain with a ridge in front of it.

- [x] **Step 7: Commit**

```bash
git add shaders/lod_cull.comp.glsl extension/src/render/lod_cull_pass.h \
        extension/src/render/lod_cull_pass.cpp extension/src/render/lod_raster_pass.cpp \
        extension/src/raymarch_compositor.cpp extension/src/voxel_world.h \
        extension/src/voxel_world.cpp tests/test_lod_cull.gd
git commit -m "feat: gpu frustum and hiz culling for the far field"
```

---

### Task 16: the seam

Spec §7.4. Two complementary halves of one Bayer threshold, so every pixel in the 120–150 m band belongs to exactly one field.

**Files:**
- Modify: `shaders/composite.frag.glsl`, `shaders/lod.frag.glsl`, `extension/src/render/composite_pass.cpp`, `extension/src/render/lod_raster_pass.cpp`
- Create: `tests/test_lod_seam.gd`

- [x] **Step 1: Write the failing test**

Create `tests/test_lod_seam.gd`:

```gdscript
extends GdUnitTestSuite

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
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

# The walk descends only into a node whose eight children are all resident, so the far field
# converges over HUNDREDS of ticks and at a rate set by build throughput, not by frame count.
# Wait on the condition (errata 6): the fixed counts this plan first used were tuned to a
# fixed frame counts were tuned to the old occluded 590 ms vsync frame and settle nothing on the current runner, which keeps vsync enabled intentionally (gdunit_tests.sh) at a normal display rate.
# requests_pending comes from the walk that ran BEFORE this tick collected its results, so it
# dips to zero for a tick or two while a batch lands -- the streak is what makes it mean
# "converged" rather than "between batches". Measured: ~350-400 ticks, so the budget is margin.
const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# The two masks are exact complements on the same pixel grid, so no pixel in the band may be
# claimed by both fields and none may be claimed by neither. A gap shows as sky through the
# ground; an overlap shows as z-fighting.
func test_the_band_is_covered_exactly_once(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 60.0, 400.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.debug_seam_probe(pos, fwd, 256, 144)
	assert_int(d["band_pixels"]).override_failure_message(
		"the probe camera saw no pixels in the 120-150 m band").is_greater(200)
	assert_int(d["band_pixels_unclaimed"]).override_failure_message(
		"%d band pixels were claimed by neither field" % d["band_pixels_unclaimed"]).is_equal(0)
	assert_int(d["band_pixels_double_claimed"]).override_failure_message(
		"%d band pixels were claimed by both fields" % d["band_pixels_double_claimed"]
		).is_equal(0)

func test_the_near_field_owns_everything_before_the_band(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 60.0, 400.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.debug_seam_probe(pos, fwd, 256, 144)
	assert_int(d["near_pixels_lost_to_lod"]).is_equal(0)
	assert_int(d["far_pixels_lost_to_raymarch"]).is_equal(0)
```

- [x] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_lod_seam.gd`
Expected: FAIL — `Nonexistent function 'debug_seam_probe'`.

- [x] **Step 3: Add the fade to `composite.frag.glsl`**

```glsl
#[fragment]
#version 460
#define MATERIAL_LAYERS 16
layout(set = 0, binding = 2) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 3) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0) uniform sampler2D src_color;  // linear (0.66x upscale)
layout(set = 0, binding = 1) uniform sampler2D src_hitpos; // nearest (no silhouette smear)
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;  // xyz = camera position, w = fade start
	vec4 fade; // x = fade end, yzw unused
} pc;
void main() {
	vec4 hp = texture(src_hitpos, uv_in);
	frag_color = texture(src_color, uv_in);
	if (hp.w < 0.5) {
		gl_FragDepth = 0.0;
		return;
	}
	// Spec section 7.4: discard the DEPTH, keeping the colour, over the band. Keeping the
	// colour is what makes a missing LoD chunk show near-field terrain rather than sky.
	// lod.frag discards on the complementary side of the same threshold at the same
	// resolution on the same pixel grid, so every band pixel belongs to exactly one field.
	float d = distance(hp.xyz, pc.cam.xyz);
	float t = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	if (bayer4(ivec2(gl_FragCoord.xy)) < t) {
		gl_FragDepth = 0.0;
		return;
	}
	vec4 clip = pc.view_proj * vec4(hp.xyz, 1.0);
	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
```

`CompositePass::draw` grows its push constant from 64 to 96 bytes to carry the camera position and the two band distances, and binds the material arrays (it now includes `common.glslh`, which needs them declared).

- [x] **Step 4: Add the complementary half to `lod.frag.glsl`**

```glsl
	float d = distance(v_wpos, pc.cam.xyz);
	float t = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	// The exact complement of composite.frag's test: >= where it uses <.
	if (bayer4(ivec2(gl_FragCoord.xy)) >= t) discard;
```

placed **before** the material sample, so a discarded fragment costs no texture work. `LodRasterPass`'s push constant already carries `cam`; add `fade`.

- [x] **Step 5: Add `debug_seam_probe`**

Renders both fields into one target with a stencil-free two-pass trick: run the composite writing a marker into an auxiliary `R8_UINT` target (1 = near field kept the pixel), then the LoD pass ORing 2 into the same target, then read it back. `band_pixels` counts texels whose distance is in [120, 150], `band_pixels_unclaimed` counts those still 0, `band_pixels_double_claimed` those equal to 3.

- [x] **Step 6: Run the tests and look at it**

Run: `./build.sh -j$(nproc)` then the gdUnit command from Step 2, then the full suite.
Run: `godot --path . demo/main.tscn`
Expected: the 150 m edge is gone; the two fields dissolve into each other with no line, no sky gap and no z-fighting shimmer.

- [x] **Step 7: Commit**

```bash
git add shaders/composite.frag.glsl shaders/lod.frag.glsl \
        extension/src/render/composite_pass.cpp extension/src/render/lod_raster_pass.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_seam.gd
git commit -m "feat: dithered near/far cross-fade over the 120-150 m band"
```

---

### Task 17: edits reach the far field

Spec §6.5. Every level whose chunks an edit touches is re-requested; the drawn pages stay drawn until the rebuild lands.

**Files:**
- Modify: `extension/src/voxel_world.cpp` (`append_edit_locked`, `teardown_gpu`), `extension/src/render/lod_pool.cpp`
- Create: `tests/test_lod_stream.gd`

- [x] **Step 1: Write the failing test**

Create `tests/test_lod_stream.gd`:

```gdscript
extends GdUnitTestSuite

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
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

# The walk descends only into a node whose eight children are all resident, so the far field
# converges over HUNDREDS of ticks and at a rate set by build throughput, not by frame count.
# Wait on the condition (errata 6): the fixed counts this plan first used were tuned to a
# fixed frame counts were tuned to the old occluded 590 ms vsync frame and settle nothing on the current runner, which keeps vsync enabled intentionally (gdunit_tests.sh) at a normal display rate.
# requests_pending comes from the walk that ran BEFORE this tick collected its results, so it
# dips to zero for a tick or two while a batch lands -- the streak is what makes it mean
# "converged" rather than "between batches". Measured: ~350-400 ticks, so the budget is margin.
const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

func test_an_edit_rebuilds_every_level_it_touches(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var before := w.debug_lod_stats()
	w.debug_apply_sphere_subtract(Vector3(380.0, 55.0, 300.0), 8.0)
	var d := w.debug_lod_stats()
	assert_int(d["dirty_chunks"]).override_failure_message(
		"an 8 m crater dirtied no LoD chunks").is_greater(0)
	assert_int(d["dirty_levels"]).override_failure_message(
		"an 8 m crater dirtied %d levels, expected every level it reaches" % d["dirty_levels"]
		).is_greater_equal(4)
	# Stale beats missing: nothing is un-drawn while the rebuild is queued.
	assert_int(d["draw_pages"]).is_greater_equal(before["draw_pages"] * 0.9)
	await settle(w, pos, fwd)
	assert_int(w.debug_lod_stats()["dirty_chunks"]).override_failure_message(
		"the dirty chunks never finished rebuilding").is_equal(0)

func test_a_far_edit_is_visible_in_the_far_field(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	var before := w.debug_lod_render_probe(pos, fwd, 256, 144)
	w.debug_apply_sphere_subtract(Vector3(400.0, 55.0, 250.0), 20.0)
	await settle(w, pos, fwd)
	var after := w.debug_lod_render_probe(pos, fwd, 256, 144)
	# A 20 m crater 150 m away must change what the far field draws.
	assert_float(absf(after["coverage"] - before["coverage"])).override_failure_message(
		"a 20 m crater at 150 m changed nothing in the far field").is_greater(0.001)

func test_teardown_and_reinit_leave_no_pages_behind(timeout := 40000) -> void:
	var w := make_world()
	var pos := Vector3(400.0, 90.0, 400.0)
	var fwd := Vector3(0.0, -0.35, -1.0).normalized()
	await settle(w, pos, fwd)
	assert_int(w.debug_lod_stats()["pages_used"]).is_greater(0)
	w.debug_teardown_atlas()
	assert_bool(w.debug_init_atlas()).is_true()
	var d := w.debug_lod_stats()
	assert_int(d["pages_used"]).override_failure_message(
		"%d pages survived a teardown" % d["pages_used"]).is_equal(0)
	assert_int(d["chunks_resident"]).is_equal(0)
	# And it still streams afterwards.
	await settle(w, pos, fwd)
	assert_int(w.debug_lod_stats()["pages_used"]).is_greater(0)
```

- [x] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_lod_stream.gd`
Expected: FAIL — `dirty_chunks` missing from `debug_lod_stats`.

- [x] **Step 3: Dirty the tree on every accepted edit**

In `VoxelWorld::append_edit_locked`, after the edit log accepts the op:

```cpp
	if (lod_tree_) {
		float lo[3], hi[3];
		ve::op_world_aabb(op, lo, hi);
		// Every level: ve::LodTree::mark_dirty walks them itself, and the relevance cut is
		// at the HALF-CELL supersample resolution rather than the cell -- a 5 m crater still
		// registers at L4's 6.4 m cells, which is the point of the reduction change. Only
		// ops shorter than half a cell on every axis are genuinely unrepresentable.
		lod_tree_->mark_dirty(lo, hi);
	}
```

Add the relevance cut inside `ve::LodTree::mark_dirty`: skip a level when the AABB's longest edge is below `0.5f * lod_cell_size(level)`. Add a `test_lod_tree.cpp` case pinning it:

```cpp
TEST_CASE("an op smaller than half a cell does not dirty that level") {
	ve::LodTreeConfig cfg;
	cfg.bounds = demo_bounds();
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 30);
	// A 0.5 m drill. Half of L4's 6.4 m cell is 3.2 m, so it cannot move a sample there.
	const float lo[3] = {800.0f, 51.0f, 700.0f};
	const float hi[3] = {800.5f, 51.5f, 700.5f};
	t.mark_dirty(lo, hi);
	ve::LodWalkResult r;
	t.walk(c, &occ, 31u, &r);
	for (const ve::LodBuildRequest &q : r.requests) CHECK(q.level <= 1);
}
```

- [x] **Step 4: Close the page-accounting gaps**

`VoxelWorld::teardown_gpu` must `lod_pool_->teardown()`, `lod_tree_->clear()` and clear `lod_pages_of_`, in that order — the tree holds page indices the pool is about to free, and a stale index would be handed to the next chunk. Follow `CompositePass::teardown`'s documented free order (uniform set → pipeline → shader → buffers): freeing a shader cascades to its pipelines, so a uniform set referencing it must go first. **Task 12 already did this**; verify rather than rewrite, and make the third test in Step 1 the proof.

Two invariants spec §3.3 states are not actually held by the code Task 9 and Task 12 shipped (errata 9), and this is the task that owns page accounting:

- **The 16-page cap is unenforced.** `LodBuildPass::read_job` clamps the readback to `kLodMaxQuadsPerChunk`, then appends skirts *on top of the clamp*, and `LodPool::upload` only tests `pages_needed > arena_.free_pages()`. Add the `pages_needed > ve::kLodMaxPagesPerChunk` refusal, and cover it with a `test_lod_arena.cpp` case.
- **An overflowing build never logs.** `LodBuildResult::overflow` is set and read by nobody. Log it once per chunk (not per frame — a chunk that overflows overflows every rebuild), per the engine spec's fail-soft policy.

- [x] **Step 5: Report the dirty counters**

Extend `debug_lod_stats()` with `dirty_chunks` (nodes with `dirty == true`) and `dirty_levels` (distinct levels among them). Add `LodTree::dirty_stats(int *chunks, int *levels) const`.

- [x] **Step 6: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc) --test` then the full gdUnit suite.
Expected: PASS, including `test_edit_pipeline.gd` and `test_collider_edits.gd` — an edit now does strictly more work and neither of those may regress.

- [x] **Step 7: Commit**

```bash
git add extension/src/lod/lod_tree.h extension/src/lod/lod_tree.cpp \
        extension/tests/test_lod_tree.cpp extension/src/voxel_world.cpp \
        extension/src/render/lod_pool.cpp tests/test_lod_stream.gd
git commit -m "feat: edits invalidate every lod level they can reach"
```

---

### Task 18: demo, HUD, benchmark, and the four verdicts

Spec §10. The plan's triggers are decided by measurement, not pre-emptively, and this task is where the measuring happens and the answers get written down.

**Files:**
- Modify: `demo/hud.gd`, `demo/benchmark.gd`, `demo/main.tscn`
- Modify: this plan (the Errata section)

- [x] **Step 1: Add the LoD line to the HUD**

`demo/hud.gd` reads `debug_lod_stats()` each frame and shows: resident chunks, pages used/total, draw pages, culled %, builds in flight, dirty chunks, and the LoD pass's CPU command-record milliseconds from `debug_perf_stats()`. Every one of those exists by the time this task runs: Task 12 gives the page and residency counters, Task 15 Step 5 adds `lod_ms` and `culled_ratio`, Task 17 Step 5 adds `dirty_chunks`.

- [x] **Step 2: Extend the benchmark**

`demo/benchmark.gd`'s scripted flythrough gains, per frame: `lod_ms`, `draw_pages`, `culled_ratio`, `chunks_resident`, `pages_used`, and a `BENCH` summary line reporting p50/p99 for each. Add a **second flythrough leg** that flies low along a valley floor with a ridge between the camera and the far basin — that is the case the near-field-only HiZ cannot cull, and it is what trigger 1 is measured on.

- [x] **Step 3: Measure**

Run: `godot --path . demo/main.tscn --disable-vsync -- --benchmark`
Record: LoD ms p50/p99, frame ms p50/p99, culled ratio on both legs, pages used at the default 32768, and the farthest level actually drawn.

- [x] **Step 4: Decide the four triggers and record the verdicts**

| Trigger | Threshold | Action if tripped |
|---|---|---|
| 1. Far-occludes-far waste | culled ratio on the ridge leg is more than 30 % below the open leg | add Voxy's temporal second phase (spec §7.5) — draw last frame's visible set, rebuild HiZ, cull and draw the remainder |
| 2. Faceting in the fade band | visible banding on smooth ground between 120 and 300 m | add an 8 B/quad corner-normal array on levels 0 and 1 only (spec §3.4) |
| 3. Material blur at arm's length | 512² reads soft on the ground under the player | 1024² for the most-used materials (spec §5) |
| 4. Range short of the world edge | the horizon ends before 4 km at the default pool | raise `max_lod_pages` (range grows linearly) |

Write each verdict — tripped or not, with the number that decided it — into this plan's **Errata**, and implement only the ones that tripped. Each implemented trigger gets its own commit.

- [x] **Step 5: Full-suite regression**

Run: `./build.sh -j$(nproc) --test --verify`
Run: `./gdunit_tests.sh -a res://tests`
Expected: everything green.

- [x] **Step 6: Commit**

```bash
git add demo/hud.gd demo/benchmark.gd demo/main.tscn \
        docs/superpowers/plans/2026-08-17-m5-far-field-lod.md
git commit -m "feat: lod hud, benchmark legs, and the measured trigger verdicts"
```

---

## Acceptance

M5 is done when all of the following hold:

1. `cd extension && scons test` — every native suite green, including the seven new `test_lod_*` suites.
2. `./gdunit_tests.sh -c` — every suite green, including the nine new ones, with no M1–M4 suite regressed. Baseline at the end of Task 13: **160/160 across 30 suites**. Use `-c`: without it gdUnit4 stops each suite at its first failure and reports a fraction of the truth (errata 5).
3. `godot --path . demo/main.tscn` shows terrain to the world edge with **no visible seam** at 150 m, textured consistently near and far.
4. Flying does not pop holes: turning the camera reveals coarse terrain, never sky.
5. A crater blasted at 300 m appears in the far field within a second, and one blasted at 2 km appears eventually.
6. Benchmark: LoD raster ≤ 2 ms p50 at 1440p, whole frame ≤ 16 ms p99 on the flythrough.
7. The four triggers in Task 18 have recorded verdicts.

---

## Errata (recorded during M5 implementation — corrections to the task text)

<!-- Append numbered entries here as the plan meets reality, in the style of M1/M2/M3/M4.
     Task 18's four trigger verdicts are still expected. -->

1. **Task 13 Step 6's indexed indirect draw works — spec §11's first risk is retired.** A pipeline created with `vertex_format = RenderingDevice::INVALID_ID`, a `draw_list_bind_index_array` over the shared 6 KB `uint16` buffer, and `draw_list_draw_indirect(dl, /*use_indices*/ true, args, 0, draw_count, 20)` all behave on Godot 4.7.1 against the NVIDIA Vulkan driver. The whole no-vertex-buffer design rested on this and it holds. The non-indexed fallback (`firstVertex = quad · 6`, `quad = gl_VertexIndex / 6`) was never needed and no longer has to be carried as a contingency.

2. **Task 13 Step 7's front face is CLOCKWISE.** `POLYGON_CULL_BACK` with `POLYGON_FRONT_FACE_CLOCKWISE` is what keeps the ground visible; the counter-clockwise pipeline culls it away entirely. `LodRasterPass` builds all three pipelines (cull off, cull-back CCW, cull-back CW) and selects with `front_face_clockwise_ = true`, measured by `test_backface_culling_does_not_remove_visible_ground` rather than assumed. Per Step 7's instruction nothing in `lod_contour.cpp`, `lod_quads.comp.glsl` or the index buffer was touched: those three agree with `ve::dual_contour` and with Jolt, and Task 9's differential test is what holds them together.

3. **The LoD raster push constant is 20 floats, and `cam` starts at float 16 — not float 64.** The std430 block is `mat4 view_proj` (floats 0–15, bytes 0–63) followed by `vec4 cam` (floats 16–19), 80 bytes total. `LodRasterPass::draw` first wrote the camera position at `f[64..67]`, i.e. byte offsets 256–271, **176 bytes past the end of the 80-byte `PackedByteArray`**. It corrupted the heap on every single draw: `test_lod_render.gd` reported all four tests PASSED and then aborted four runs in five, either as glibc `corrupted size vs. prev_size` or as a SIGSEGV inside `vkDestroyDevice` while the local `RenderingDevice` was being torn down — so the suite looked green while exiting 134. Fixed in `c15a1ae`. **Task 16 Step 4 adds `fade` to this same block**: index it by float, keep the array and `draw_list_set_push_constant`'s size argument in step, and stay under the 128-byte guarantee.

4. **Task 13's shipped `tests/test_lod_render.gd` differs from Step 2's snippet.** The camera is `(100, 155, 204)` looking down at `(0, -0.35, -1)`, not `(400, 90, 400)`; one world is built and settled ONCE for the whole suite through `after()` instead of one per test; and the settle is condition-based (errata 6). The plan's version settled the same shared world four times over identical camera state, and its camera/coverage pair was not satisfiable as written. Coverage at the shipped camera is ~0.317 against the `> 0.25` threshold, `nearest_hit_m` ~101.7 m against `>= 100`.

5. **The gdUnit runner is `./gdunit_tests.sh`, and it had four bugs of its own.** Every "Run:" line in this plan used `addons/gdUnit4/runtest.sh` directly. Do not: that wrapper runs `godot --path .` relative to the **caller's** directory, so it only worked from the repo root, and it passes `-d --remote-debug tcp://127.0.0.1:0`, whose invalid port printed two `ERROR` lines on every run. The wrapper script also (a) documented a comma-separated `-a` that gdUnit4 does not support — it takes one path and may be repeated, so `-a a.gd,b.gd` matched nothing and **exited 0 having run zero tests**; (b) swallowed a missing `-a` value, silently widening to the whole suite; (c) launched a second headless Godot for `GdUnitCopyLog.gd`, which only ever wrote a "No logging available!" placeholder. All fixed in `73a2522`. Also note **gdUnit4 aborts a suite at its first failure by default** — pass `-c` when you want the true failure count; a plain run under-reports it badly (121 tests reported versus 160 actually present).

6. **A fixed frame count settles nothing — wait on the condition.** Every settle in this plan was written as `for i in range(250)`, which was only ever long enough because the suite's window is normally unfocused and occluded and vsync made a single `await get_tree().process_frame` cost **~590 ms**: 60 empty frames that did no work at all took 35.4 s, and `test_lod_pool.gd` took 8m36s of which the LoD work itself was under a second. The repo runner deliberately **keeps vsync enabled** (user requirement, `gdunit_tests.sh`; `73a2522` had briefly disabled it, making a frame cost ~0.5 ms), so the same 250 frames settle almost nothing. Suites now tick until `debug_lod_stats()` reports `requests_pending == 0 and builds_in_flight == 0` for a streak of 8 ticks, with a wide tick budget as the ceiling. The streak is required: `requests_pending` reflects the walk that ran BEFORE this tick collected its results, so a single sample reads zero while a batch is landing and "converges" with zero chunks resident. Measured convergence is ~350–400 ticks; eviction fires between far-tick 300 and 400, matching `kLodEvictFrames`. `debug_lod_stats()` gained `requests_pending` for this, and `partial_allocations` stopped being a hardcoded `0` — it now measures the two shapes a half-funded build would leave, so the assertion on it can actually fail. All four LoD suites: ~9 minutes and a crash, down to 13 s and 16/16 stable.

7. **Task 8's generalisation is not used by the LoD path — there are two meshers, not one.** The plan's architecture paragraph says the mesher is "M3's, generalised … so there is one mesher and one CPU reference". Task 8 did land: `mesh_common.glslh` takes origin, cell size and lattice dimension from the push constant. But Task 9 then wrote its own `lod_field.comp.glsl` **and** `lod_frac.comp.glsl`, and every `MeshJob` in the GPU path still passes `ve::kChunkCellSize` / `ve::kChunkLattice` — so the generalisation is never exercised at LoD scale. `lod_frac` is individually justified (it emits packed 5-bit cell fractions rather than world positions, which is what the 12-byte record carries, so GPU and `ve::lod_contour` quantise the same number the same way), but the consequence is two GPU cell-vertex shaders with duplicated `CORNER`/`EDGE` tables and two CPU references. Each side is pinned by its own differential test and nothing is currently wrong; the tables can nonetheless drift, and no test would catch it because no chunk is ever meshed both ways.

8. **`register_types.cpp` is unchanged for every task except 14.** The File Structure says "unchanged (no new script-visible classes)". Task 14's `AsyncTextureRead` is not script-visible either, but it is a `RefCounted` `GDCLASS` — `callable_mp` cannot name a handler on an unregistered class — so it needs `GDREGISTER_INTERNAL_CLASS`, exactly as `AsyncBufferRead` already does.

9. **Two of spec §3.3's invariants are not held by the shipped code.** `LodBuildPass::read_job` clamps the readback to `ve::kLodMaxQuadsPerChunk` and then appends skirts *on top of the clamp*, while `LodPool::upload` tests only `pages_needed > arena_.free_pages()` — so nothing enforces the stated 16-page-per-chunk cap. And an overflowing build sets `LodBuildResult::overflow`, which no caller reads, so the "logs once" half of the fail-soft policy never happens. Neither is currently harmful (arena pages need not be contiguous), but both are stated invariants with nothing holding them. Folded into Task 17 Step 4, which owns page accounting.

10. **The collider and mesh gdUnit suites were already red when M5 started, from `65486a0` on `main`.** Anyone running Task 8 Step 6's regression gate, or the full suite at any point in Tasks 9–13, would have seen seven suites failing for reasons that have nothing to do with this plan. That commit halved the collision chunk from 16 bricks (12.8 m) to 8 (6.4 m) — `kChunkCells` 128 → 64, lattice 130³ → 66³ — moved meshing onto a worker thread, and made the atlas counter readback asynchronous. It updated the C++ unit tests and missed the GDScript ones, which hardcoded the old chunk coordinates (so the mesher was handed solid rock and correctly returned nothing), counted "frames with no action" as settled (`ColliderStreamer::run_frame` returns 0 on every frame it spends *waiting*, so `settle()` gave up after five frames with 163 chunks pending and zero bodies), and assumed overflow recovery happened on the very next frame. Repaired in `99849bb`, `2d3e391`, `6ba311a`, `068a3dd`; the full suite is 160/160. Two contracts in `test_mesh_stream.gd` had genuinely changed with the async refactor and now pin the real ones: the inline diagnostic path no longer *refuses* while a batch is in flight (`MeshService::run_sync` waits its turn), and an oversized batch is not refused at submit but reported as a failure per chunk so the caller can clear its in-flight markers.



11. **Task 18 Trigger 1 tripped: far-occludes-far waste is real on the ridge leg.** The open flythrough leg (`--benchmark-move`, camera at (24, 63.2, 24) looking toward (6, -10, 6)) had `culled_ratio` p50 = 0.382 (p99 = 0.854). The new ridge leg (`--benchmark-ridge`, low valley at (480, 340) flying along (2, 1), with a ridge ahead near 460 m and the basin behind it) had `culled_ratio` p50 = 0.220 (p99 = 0.925) — 42 % below the open leg, over the 30 % threshold. The action (Voxy's temporal second phase, spec §7.5) is therefore indicated. Task 18's file scope is demo/HUD/benchmark/plan only, so this verdict is recorded here and the implementation is left as a follow-up rather than smuggled into the final demo commit.

    **Implemented in `8cc6a01`** (`feat: temporal second-phase lod culling for far occludes far`). Post-implementation benchmark on the same Wayland/NVIDIA session: `--benchmark-move` culled_ratio p50 = 0.226 (p99 = 0.702), `--benchmark-ridge` culled_ratio p50 = 0.053 (p99 = 0.508); both runs print their BENCH lines and then still abort at teardown with the pre-existing core dump. The ratio now counts first-pass pages as drawn by design (whole candidate set), so it is not directly comparable to the pre-trigger single-pass ratio; the ridge leg's near-zero value reflects the temporal visible set being drawn first rather than a regression in culling.

12. **Task 18 Trigger 2 not measurable: faceting in the fade band is a visual judgement and the interactive demo cannot run to completion on this session.** The demo still aborts with `corrupted size vs. prev_size` during teardown on this Wayland/NVIDIA session (pre-existing at baseline, Task 15). No interactive visual inspection of the 120–300 m band was possible, so no banding verdict is recorded.

13. **Task 18 Trigger 3 not measurable: material blur at arm's length is a visual judgement and the same demo crash blocks it.** The 512²-read softness under the player cannot be assessed without an interactive view, so no blur verdict is recorded.

14. **Task 18 Trigger 4 not tripped: the default LoD pool reaches the world edge.** The demo world is 64×8×64 regions = 1638.4 m on x/z; the LoD level table has 8 levels (0–7) and the root level L7 is drawn by design (pinned by `extension/tests/test_lod_tree.cpp` `root_drawn`). Benchmarks used only p50 ≈ 2.9k / p99 ≈ 4.9k of the default 32768 pages, so `max_lod_pages` is nowhere near exhausted. A 4 km horizon is not a property of this 1.64 km demo world, so the "range short of the world edge" condition does not arise. Farthest level actually drawn: L7 (`kLodLevels - 1`), from the level table/native test; `debug_lod_stats` does not expose a per-level draw counter.

15. **`lod_ms` is CPU command-record time, not GPU execution time.** `LodRasterPass::last_ms()` and `LodCullPass::last_ms()` wrap command recording in `std::chrono::steady_clock`; they do not include GPU dispatch/execution or readback latency. `VoxelWorld::debug_perf_stats()` reports their sum as `lod_ms`, so the benchmark's `lod_ms_p50/p99` must be read as "CPU time spent recording LoD commands", not as GPU frame cost. The code comments in both passes state this explicitly.
