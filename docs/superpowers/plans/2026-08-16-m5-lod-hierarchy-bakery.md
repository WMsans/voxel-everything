# M5 LoD Hierarchy, Bakery & Far-Field Compositing — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** See the whole world. Four levels of surface-nets chunks, each evaluated directly from `G + edit ops` at its own pitch and each carrying a baked albedo/normal texture produced by refining the analytic field at 4× the mesh sampling, streamed by a screen-space-error priority queue into a fixed page pool, drawn as one indirect multi-draw straight into Godot's scene framebuffer — depth-testing against the raymarched near field that M1's composite already injects — and cross-fading into it over a dithered 120–150 m band so the seam is invisible.

**Architecture:** A LoD chunk is **the collision mesher's lattice at a different pitch**: 64³ cells, 65³ mesh cells, 66³ samples, exactly the numbers `shaders/mesh_common.glslh` already uses. Generalising the three mesh shaders from "chunk coordinate" to "world origin + cell size" therefore turns the M3 mesher into the M5 mesher for free, and `ve::DcGrid` (already parameterised — M3 wrote it that way on purpose) stays its CPU reference. The bakery is a fourth compute pass on the same worker device: one thread per texel of a per-quad 4×4 **tile**, positioned by bilinear interpolation across the quad and refined onto the analytic surface by one Newton step, so overhangs, cave mouths and cliff undersides each bake their own neighbourhood with no projection collisions. The render device holds a **page pool** — one 512×256 texture-array layer plus one 40 960-vertex slab per page — and the whole visible cut is one `draw_list_draw_indirect` over a shared index buffer, with the page index recovered in the vertex shader from `gl_VertexIndex / kLodVertsPerPage`. Selection, streaming and eviction are one pure C++ CDLOD walk with a page budget, so quality degrades gracefully when the pool is small and range grows linearly when it is raised.

**Tech Stack:** Godot 4.7.1 (`/usr/bin/godot`), godot-cpp (pinned master, `api_version = "4.7"`), SCons, C++20, GLSL 460 (Vulkan), Jolt Physics, doctest 2.4.11 (native), gdUnit4 6.2.1 (in-engine).

**Spec:** `docs/superpowers/specs/2026-08-12-voxel-engine-design.md` — M5 implements §4 in full, §3's *Compositing with raster* and *Near/far seam* paragraphs, and the "LoD raster into the shared buffer" half of §7's frame order. Cel shading, shadows, SSGI, SSR and outlines stay in M6; M5 deliberately shares the near field's **existing** lighting GLSL so that when M6 replaces it there is exactly one place to change.

**Predecessors:** `docs/superpowers/plans/2026-08-12-m1-walking-skeleton.md`, `docs/superpowers/plans/2026-08-13-m2-gpu-generation-streaming-edits.md`, `docs/superpowers/plans/2026-08-14-m3-physics-meshing-colliders.md`, `docs/superpowers/plans/2026-08-15-m4-connectivity-islands.md` (all complete). **Read all four Errata sections before touching shaders, `MeshPass` or `VoxelWorld`** — in particular M1 errata 2 (reverse-Z depth and `COMPARE_OP_GREATER_OR_EQUAL`) and 3 (tan-half-fov from the projection diagonals), M2 errata 5 (GLSL reserved words), 7 (`ivec4` → `.xyz`), 9 (`kSurfaceY = 51.2`) and 11 (the eviction arm), M3 errata 1 (Jolt's winding convention) and 5 (streaming must fund its own loads), and M4 errata 1 (the flattened cross-region op list).

## Milestone Map

| Milestone | Delivers |
|---|---|
| M1 (done) | Toolchain + raymarched terrain on screen + test harnesses |
| M2 (done) | GPU brick generation, region indirection, streaming/residency + LRU, min–max mips, destruction edits |
| M3 (done) | Dual-contour collision meshing on the GPU, async readback, collider streaming into Jolt, character controller |
| M4 (done) | Occupancy grid, connectivity, island carve/extract/spawn/re-merge, raymarched island targets, tiled culling, debris |
| **M5 (this plan)** | LoD hierarchy, surface-nets chunks at four pitches, per-quad texture bakery, page pool, indirect multi-draw, dithered near/far cross-fade |
| M6 | Beautification: cel, 3-layer shadows, SSGI, SSR, outlines |
| M7 | Benchmark scene + demo polish |

## Global Constraints

- Godot **4.7.1**; godot-cpp pinned to the existing submodule commit, `api_version = "4.7"` — do not bump either.
- Pure C++ cores in `namespace ve` contain **zero Godot types** (spec §8) — no exceptions. Godot-glue classes live in `namespace godot`. Spec §8's module table is binding: `lod/` is new and **pure C++**; `render/` is Godot glue. Anything that selects, prioritises, packs or measures is pure; anything that owns a `RID` is glue.
- Shaders: GLSL `#version 460`, loaded **from files** via `ve::load_shader_source` — never inline strings. `#[compute]` / `#[vertex]` / `#[fragment]` are stripped in C++ after load (M1 errata 6; `ve::strip_shader_annotations`).
- Error policy (spec §8): dev = verbose/validation; release = fail-soft — a LoD build failure keeps the previous pages and retries, a full page pool refuses the build rather than evicting something on screen, a bake failure leaves the chunk with a flat-material page. **A coarser world is always the safe direction**; a missing far chunk is a hole in the horizon, a stale one is only slightly out of date.
- Commit style: conventional (`feat:`, `test:`, `build:`, `fix:`, `refactor:`).
- RD API reference: local copy at `docs/api/renderingdevice.md` — consult it before inventing signatures.
- Target hardware: RTX 4070 Laptop; budgets per spec §7 (raymarch ≤ 6 ms, **LoD ≈ 2 ms**, frame ≤ 16 ms). M5's render-thread work is one `buffer_update` of the indirect args (≤ 20 KB), one page-record `buffer_update` (≤ 16 KB), the page uploads a finished build queues (≤ 832 KB), and one indirect draw.
- **Push constants must stay ≤ 128 bytes** (Vulkan's guaranteed minimum). M5's largest is the LoD raster's 80 bytes (a `mat4` plus one `vec4`).
- **Never put a literal include directive inside a GLSL comment** — the loader matches the token anywhere on a line (note at the top of `shaders/common.glslh`).
- `buffer_update`, `buffer_clear` and `texture_update` are device-level commands: they must be recorded **before** `compute_list_begin` / `draw_list_begin`, never inside an open list (M2 Task 12's documented ordering).
- **`ve::EditOp` stays exactly 32 bytes** (`static_assert`) and its GLSL mirror stays exactly two `uvec4`. M5 adds no op types.
- Reverse-Z everywhere: Godot 4.7.1's scene projection is depth-corrected with near = 1.0, far = 0.0. Every pipeline that writes into the scene depth buffer uses `COMPARE_OP_GREATER_OR_EQUAL` (M1 errata 2).

## Fixed Numbers (decided here; every task assumes them)

| Thing | Value | Where it lives |
|---|---|---|
| LoD levels | **4** | `ve::kLodLevels` |
| Cells per chunk axis | **64** (identical to the collision lattice) | `ve::kLodChunkCells == ve::kChunkCells` |
| L1 cell / chunk | **0.4 m / 25.6 m** (= one region) | `ve::kLodCellSize[0]`, `ve::kLodChunkSize[0]` |
| L2 cell / chunk | **1.6 m / 102.4 m** | `[1]` |
| L3 cell / chunk | **6.4 m / 409.6 m** | `[2]` |
| L4 cell / chunk | **25.6 m / 1638.4 m** | `[3]` |
| Level ratio | **4×** linear (spec §4) | `ve::kLodRatio` |
| Bake tile | **4 × 4 texels per quad** (= "~4× the chunk's mesh sampling") | `ve::kLodTileTexels` |
| Page texture | **512 × 256 RGBA8** = 128 × 64 tiles = 8192 tiles, 512 KB | `ve::kLodPageTexW/H`, `kLodTilesPerPage` |
| Page quads | **10 240** (≤ 8192 tiled surface quads + skirts) | `ve::kLodQuadsPerPage` |
| Page vertices | **40 960** (4 per quad), 8 bytes each = 320 KB | `ve::kLodVertsPerPage`, `kLodVertexBytes` |
| Page total | **832 KB** | — |
| Pages per chunk | **≤ 2** | `ve::kLodMaxPagesPerChunk` |
| Page pool | **512** ≈ **426 MB** (export) | `VoxelWorld::max_lod_pages` |
| Vertex quantisation pad | **8 cells** each side | `ve::kLodQuantPadCells` |
| Skirt depth | **4 cells**, along −gradient | `ve::kLodSkirtCells` |
| Screen-space-error threshold | **4.0 px** | `ve::kLodSseThreshold` |
| Near/far fade band | **120 → 150 m** (spec §3) | `ve::kLodFadeStartM`, `kLodFadeEndM` |
| LoD builds per frame | **1** (export, ≤ 4) | `VoxelWorld::lod_builds_per_frame` |
| LoD chunk probes per frame | **128** | `LodResidencyConfig::max_probes_per_frame` |
| Bake field evaluations per texel | **9** (1 value + 6 gradient + 1 refined material + 1 AO) | `lod_bake.comp.glsl` |
| Ops per LoD job | **≤ 256** (`ve::kMaxRegionOps`), chronological prefix | `ve::lod_ops_for_chunk` |
| Op relevance cut | op world-AABB longest edge **< the level's cell size** → dropped | `ve::lod_ops_for_chunk` |

**Memory.** One page is 512 KB of texture-array layer plus 320 KB of vertex slab = **832 KB**; 512 pages is **426 MB**, inside spec §4's "300–500 MB". The shared index buffer is 61 440 `uint16` = 120 KB, the page-record SSBO 512 × 32 B = 16 KB, the indirect-args buffer 512 × 20 B = 10 KB. Nothing else is resident.

**What that buys.** A page holds ~8 000 quads; L1 terrain produces ~11–12 quads per m² of ground (one y-edge crossing per 0.4 m cell column plus the x/z crossings a 0.4 slope adds), so one L1 chunk is ~7 400 quads ≈ one page and covers 655 m². Sharing 512 pages across four levels by screen-space error puts, on the demo world, **L1 out to ≈ 200 m, L2 to ≈ 700 m, L3 to ≈ 2 km, L4 to the world edge**. That is short of spec §4's "L1 150 m – 1 km", and it has to be: 1 km of L1 is π·10⁶ m² × 1.5 (vertical faces) × 11 quads/m² ≈ 52 M quads, and at the **cheapest imaginable** encoding (one shared vertex per cell at 8 bytes plus a `uint16` index triple) that is still 1.2 GB of geometry alone, before any bake. The spec's own memory line is the binding constraint, `max_lod_pages` is the dial, and raising it buys range linearly.

## Deliberate Decisions (recorded, with the spec text they interpret)

- **A LoD chunk is the collision chunk's lattice at another pitch, so there is one mesher.** Spec §4 asks for "coarse surface-nets mesh evaluated directly from G + edit ops at that level's resolution" and spec §6 already built exactly that machine at 0.1 m. `kChunkCells` is 64 and every level of §4's table is 64 cells across (25.6/0.4, 102.4/1.6, 409.6/6.4, 1638.4/25.6), so the only thing that differs is *where the lattice starts and how far apart its samples are*. Task 4 moves those two facts from compile-time constants into the push constant; `ve::DcGrid` already carries them (M3 wrote it "so M5's LoD chunks can reuse the mesher at their own pitch"). No second mesher, no second CPU reference, no second differential test harness.
- **The bake is a per-quad tile atlas, not a per-chunk planar projection.** Spec §4 says "along each triangle's **dominant axis** — triplanar-style projection, so cliffs/overhangs/cave mouths bake correctly". A single planar image per chunk cannot do that: two surfaces that overlap in the projection (the roof and floor of a cave mouth, the top and underside of an overhang) collide in the same texels, which is precisely the case the sentence names. Giving each quad its own 4×4 tile makes the projection *per quad* — the strongest possible reading of "dominant axis" — and removes the collision case entirely. It costs 64 bytes of texture per quad against a planar bake's ~4, which is why the range arithmetic above lands where it does.
- **Tile corners sit on texel centres, so bilinear filtering never leaves the tile.** With a 4×4 tile, the quad's four corners map to the centres of texels (0,0), (3,0), (3,3), (0,3). Every interior point of the quad interpolates to a UV inside `[0.5, 3.5]` texels, whose bilinear footprint touches only that tile's 16 texels. No borders, no padding, no bleeding, and adjacent quads meet at corners that sampled the *same world position*, so there is no seam either. A 2×2 tile would put every texel on a corner and the bake would degenerate into per-vertex attributes stored four times over — that is why the tile is 4×4 and not 2×2.
- **The page index is recovered from `gl_VertexIndex`, not from a draw parameter.** `draw_list_draw_indirect` with indices takes `{index count, instance count, first index, vertex offset, first instance}`. Setting `vertex offset = page * kLodVertsPerPage` makes `gl_VertexIndex / kLodVertsPerPage` the page slot in the vertex shader, which is how one indirect multi-draw over a shared index buffer can address 512 independently-placed chunks without `gl_DrawID` (needs `VK_KHR_shader_draw_parameters` plumbing Godot does not expose) and without a non-zero `firstInstance` (needs the `drawIndirectFirstInstance` device feature Godot does not request).
- **LoD rasterises into Godot's scene framebuffer, after the composite, still pre-opaque.** Spec §3 says "Raymarched depth is blitted into Godot's scene depth buffer before the opaque pass; LoD geometry and Godot dynamic objects depth-test against it normally", and spec §7's frame order puts the LoD raster next to the raymarch. M1's `CompositePass` already proves that a `CompositorEffect` can build a framebuffer over `RenderSceneBuffersRD::get_color_texture()/get_depth_texture()` and write `gl_FragDepth` into it, so M5 adds a second draw into the same framebuffer with the same `COMPARE_OP_GREATER_OR_EQUAL` depth state. Mutual occlusion with the near field is then the depth test itself, not a compositing rule, and Godot's opaque pass sees one merged depth buffer.
- **The cross-fade is one 4×4 Bayer threshold used in opposite directions.** Spec §3's "Dithered depth fade over 120–150 m, cross-fading into LoD". The composite discards its *depth* (keeping its colour, so a missing LoD chunk shows terrain rather than sky) where `bayer(pixel) < fade(d)`; the LoD fragment shader discards entirely where `bayer(pixel) >= fade(d)`. Both run at full scene resolution on the same pixel grid, so the two masks are exact complements and every pixel in the band is owned by exactly one of them. The same `bayer4()` lives in `common.glslh`, which is the file spec §8 exists to keep them from drifting apart.
- **Skirts are built on the CPU from the mesh, and reuse their parent quad's tile.** Spec §4: "Skirts on chunk borders hide inter-level cracks; no stitching meshes." A boundary edge is one whose two endpoints both lie within 1.5 cells of the same face plane of the chunk box — recoverable from positions and quads alone, which is all the readback already carries, so no fourth GPU pass and no cell-map readback. The curtain hangs 4 cells along the quad's **negative** normal (into the solid), which covers the ≤ 1 coarse-cell mismatch a one-level jump can produce and works on cliffs as well as floors, where a straight-down skirt does not. Giving the skirt its parent's tile costs nothing, needs no extra bake, and smears the parent's edge colours down the curtain — which is exactly what should be visible in a crack.
- **Distant edits are dropped when they are smaller than the level's cell.** Spec §4: "Edit op → dirty intersecting chunks at all 4 levels → rebuild in priority order". M5 dirties all four levels, but `lod_ops_for_chunk` then discards ops whose world AABB is shorter than the level's cell size on every axis: at 6.4 m sampling a 5 m crater cannot move a single sample, so applying it costs op-pool space and changes nothing. This is a resolution argument, not a distance one, and it is what keeps an L3 chunk's flattened op list (M4 errata 1's `collect_ops_for_aabb` over 16³ regions) inside `kMaxRegionOps`.
- **An over-full op list is truncated to a chronological prefix, never a suffix.** When a chunk still gathers more than `kMaxRegionOps` relevant ops, the *first* 256 are kept. A prefix of an ordered CSG list is a valid world state — the world as of edit 256 — whereas a suffix can apply an add without the subtract that made room for it. Stale beats wrong, which is spec §8's rule stated for the op list.
- **Selection and streaming are one CDLOD walk with a page budget, and the walk only descends into a fully-ready sibling set.** Spec §4: "Per frame, a chunk renders at the coarsest level under a screen-space-error threshold (CDLOD-style)." Descending only when all eight children are ready (built, or probed empty) guarantees the emitted cut is complete and non-overlapping at every instant — no holes while a child streams in, no double-drawn ancestor z-fighting a resident sibling. The nodes the walk *wanted* to descend into become the build queue, keyed by distance, which is spec §4's "background priority queue keyed by screen-space error".
- **Chunks entirely inside the near field are never built.** A chunk whose farthest corner is nearer than `kLodFadeStartM` is discarded by the fragment shader on every pixel, so building it burns a page to draw nothing. Skipping them frees the whole 120 m ball — on the demo world, over a thousand L1 chunks — for the shell where LoD is actually visible.
- **Backface culling stays disabled for the LoD pass.** M3 errata 1 showed that this codebase's winding convention is subtle enough to have already cost one bug, and skirts are two-sided by construction. `POLYGON_CULL_DISABLED` (what `CompositePass` already uses) removes the whole class of failure for a fill cost that is invisible on geometry averaging 3–5 px per quad. Recorded as a deferral, not an omission.

## Deliberate Deferrals (recorded, not forgotten)

- **BC compression of bake pages.** Spec §4 offers it "if the budget demands". It would quarter the 512 KB layer and roughly double the range, but it needs a GPU block compressor and a second texture format path. `max_lod_pages` is the dial M5 ships instead.
- **Temporal upsampling of the raymarched near field.** Spec §3 mentions "~0.66× resolution + temporal upsample"; M1 shipped the 0.66× half and M5 does not change it. The cross-fade dither is a separate mechanism and does not depend on it.
- **The depth-range half of tiled island culling.** M4 deferred it for want of a scene depth buffer before the opaque pass. M5 injects one — but the island cull pass runs *before* the composite, so the depth is still not there when it would be read. Moving the cull after the composite is an M6 concern, when the whole pass order is rewritten around the merged G-buffer.
- **Spec §4's "Fade-band quality contingency" (densify L1 to 0.2 m within 300 m).** Task 12 measures the band and records the verdict; the change would be a fifth level (12.8 m chunks at 0.2 m) and is structural only in that `kLodLevels` becomes 5. Not taken pre-emptively.
- **Per-level texture arrays.** Spec §4 says "one indirect multi-draw per level … textures as arrays". M5 uses **one** array and **one** multi-draw for all four levels, because every page has identical dimensions and the level only enters through the per-page record. Strictly fewer draws than the spec asks for.
- **Vertex-position compression below 16 bits per axis.** 3 × `uint16` over a chunk-plus-pad box is 0.44 mm at L1 and 28 mm at L4; 8 bits would be a quarter of a cell and visibly faceted.

## File Structure

```
extension/src/
  lod/                                                       (pure C++, namespace ve)
    lod_grid.h/.cpp        level table, chunk math, op ranges, probe      (Task 1)
    lod_page.h/.cpp        tiles, vertex packing, page building, skirts   (Task 2)
    lod_residency.h/.cpp   CDLOD walk, page budget, build queue, dirty    (Task 3)
    lod_bake.h/.cpp        the bake's CPU reference (one texel)           (Task 5)
    lod_cull.h/.cpp        frustum planes + the AABB test the draw uses   (Task 8)
  mesh/
    dual_contour.h/.cpp    MODIFIED: quad output alongside triangles      (Task 4)
  render/
    mesh_pass.h/.cpp       MODIFIED: origin+pitch push, quad buffer       (Task 4)
    lod_bake_pass.h/.cpp   the 4th worker-device dispatch                 (Task 5)
    mesh_service.h/.cpp    MODIFIED: LoD build queue                      (Task 6)
    lod_pool.h/.cpp        render-device pages: verts, indices, array     (Task 7)
    lod_raster_pass.h/.cpp frustum cull, indirect args, the draw          (Task 8)
    composite_pass.h/.cpp  MODIFIED: dithered depth fade                  (Task 9)
  raymarch_compositor.cpp  MODIFIED: LoD draw + camera publication        (Task 9)
  voxel_world.h/.cpp       MODIFIED: LoD tick, exports, invalidation      (Task 10)
shaders/
  common.glslh             MODIFIED: bayer4, oct encode/decode            (Task 2)
                           MODIFIED: shade_terrain, lod_fade              (Task 8)
  raymarch.comp.glsl       MODIFIED: calls the shared shade_terrain       (Task 8)
  mesh_common.glslh        MODIFIED: origin+pitch from the push constant  (Task 4)
  mesh_field.comp.glsl     MODIFIED: same                                 (Task 4)
  mesh_cells.comp.glsl     MODIFIED: same                                 (Task 4)
  mesh_quads.comp.glsl     MODIFIED: writes quads, one atomic per quad    (Task 4)
  lod_bake.comp.glsl       NEW: one thread per bake texel                 (Task 5)
  lod.vert.glsl            NEW: unpack page + vertex, project             (Task 8)
  lod.frag.glsl            NEW: sample the page, light it, dither-fade    (Task 8)
  composite.frag.glsl      MODIFIED: dithered depth fade                  (Task 9)
extension/tests/           doctest: test_lod_grid, test_lod_page,
                           test_lod_residency, test_lod_bake, test_lod_cull
tests/                     gdUnit: test_lod_mesh.gd, test_lod_bake.gd,
                           test_lod_build.gd, test_lod_pool.gd,
                           test_lod_render.gd, test_lod_stream.gd
demo/                      hud.gd, benchmark.gd, main.tscn (MODIFIED)
extension/SConstruct       MODIFIED: lod/ joins the native build (Task 1)
```

## Conventions Used Throughout

- **Build:** `./build.sh -j$(nproc)` (or `cd extension && scons -j$(nproc)`)
- **Native tests:** `cd extension && scons test`
- **gdUnit tests:** `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
- **Demo:** `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
- **Level indices are 0-based; the spec's L1 is `level == 0`.** Every signature takes `int level` in `[0, kLodLevels)`. Where prose says "L1" it means level 0.
- **LoD chunk coordinates are GLOBAL per level**, like brick and collision-chunk coordinates: the world-space corner of chunk `c` at level `L` is `c * kLodChunkSize[L]`, no origin term. `WorldBounds` only decides membership.
- **A chunk's mesh box is not its nominal box.** The mesher owns cells at local coordinates `-1 … 63`, so vertices lie in `[origin - cell, origin + 64·cell]`. Skirts add up to 4 more cells in any direction. Quantisation therefore uses `[origin - 8·cell, origin + (64 + 8)·cell]` (`kLodQuantPadCells`).
- **Quad corner order is `(c0, c1, c2, c3)` = tile-space `(0,0), (1,0), (1,1), (0,1)`,** already wound so that `(c0,c1,c2)` and `(c0,c2,c3)` are the two triangles. `mesh_quads.comp.glsl` reverses the order for the flipped sign case rather than making the CPU guess.
- **Bake texel `(i, j)` of a quad is bilinear parameter `(i/3, j/3)`** across that quad — so texel 0 sits exactly on corner 0 and texel 3 on the opposite corner, which is what makes the corner UVs texel centres.
- gdUnit tests that await must declare the timeout argument: `func test_x(timeout := 10000) -> void:`.
- Every gdUnit suite that creates a `VoxelWorld` registers it in `_worlds` and frees it in `after_test()` (M3 errata 2).

---

### Task 1: `lod/lod_grid` — the four-level lattice

The level table every later task hangs off: how big a chunk is, where it sits, which chunk contains a point, which chunks an edit dirties, which ops a chunk actually needs, and the conservative probe that decides whether a chunk is worth building at all.

**Files:**
- Create: `extension/src/lod/lod_grid.h`, `extension/src/lod/lod_grid.cpp`
- Create: `extension/tests/test_lod_grid.cpp`
- Modify: `extension/SConstruct:16-17` (pure-source globs)

**Interfaces:**
- Consumes: `ve::IVec3`, `ve::WorldBounds`, `ve::floor_div`, `ve::kBrickSize`, `ve::kRegionSize` (`world/region.h`); `ve::kChunkCells` (`mesh/mesh_chunk.h`); `ve::EditOp`, `ve::op_world_aabb`, `ve::VolumeStore` (`generator/edit_ops.h`); `ve::eval_field` (`world/brick_eval.h`); `ve::Generator` (`generator/generator.h`); `ve::EditLog`, `ve::collect_ops_for_aabb`, `ve::kMaxRegionOps` (`world/edit_log.h`).
- Produces:
  - `ve::kLodLevels = 4`, `ve::kLodRatio = 4`, `ve::kLodChunkCells`, `ve::kLodCellSize[4]`, `ve::kLodChunkSize[4]`, `ve::kLodSseThreshold`, `ve::kLodFadeStartM`, `ve::kLodFadeEndM`
  - `float ve::lod_cell_size(int)`, `float ve::lod_chunk_size(int)`
  - `ve::IVec3 ve::lod_chunk_of_point(int, float, float, float)`
  - `void ve::lod_chunk_origin(int, IVec3, float[3])`, `void ve::lod_chunk_aabb(int, IVec3, float[3], float[3])`
  - `ve::IVec3 ve::lod_parent(IVec3)`, `ve::IVec3 ve::lod_child_base(IVec3)`
  - `float ve::lod_chunk_distance(int, IVec3, const float[3])`, `float ve::lod_chunk_far_distance(int, IVec3, const float[3])`
  - `float ve::lod_screen_error(int, float, float)`
  - `bool ve::lod_chunk_in_bounds(const WorldBounds &, int, IVec3)`
  - `void ve::lod_root_range(const WorldBounds &, IVec3 *, IVec3 *)`
  - `void ve::op_lod_chunk_range(const EditOp &, int, IVec3 *, IVec3 *)`
  - `int ve::lod_ops_for_chunk(const EditLog &, int, IVec3, std::vector<EditOp> *)`
  - `bool ve::lod_chunk_has_surface(const Generator &, const EditOp *, int, int, IVec3, const VolumeStore *)`

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_lod_grid.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_grid.h"
#include "generator/generator.h"
#include "world/edit_log.h"
#include <cmath>
#include <vector>

// The spec's own table (section 4): every level is 64 cells across, each level 4x the
// linear size of the one below. If these ever disagree the mesher silently samples the
// wrong lattice, so they are pinned here rather than trusted.
TEST_CASE("the level table matches spec section 4") {
	CHECK(ve::kLodLevels == 4);
	CHECK(ve::kLodChunkCells == 64);
	CHECK(ve::lod_cell_size(0) == doctest::Approx(0.4f));
	CHECK(ve::lod_cell_size(1) == doctest::Approx(1.6f));
	CHECK(ve::lod_cell_size(2) == doctest::Approx(6.4f));
	CHECK(ve::lod_cell_size(3) == doctest::Approx(25.6f));
	for (int l = 0; l < ve::kLodLevels; l++) {
		CHECK(ve::lod_chunk_size(l) ==
				doctest::Approx(ve::lod_cell_size(l) * ve::kLodChunkCells));
	}
	// L1 is exactly one region, which is what lets an L1 chunk's op list be one region's.
	CHECK(ve::lod_chunk_size(0) == doctest::Approx(ve::kRegionSize));
	// Out-of-range levels clamp rather than read past the table.
	CHECK(ve::lod_cell_size(-1) == doctest::Approx(ve::lod_cell_size(0)));
	CHECK(ve::lod_cell_size(99) == doctest::Approx(ve::lod_cell_size(3)));
}

TEST_CASE("chunk coordinates are global and floor toward negative infinity") {
	CHECK(ve::lod_chunk_of_point(0, 0.0f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::lod_chunk_of_point(0, 25.5f, 0.1f, 51.3f) == ve::IVec3{0, 0, 2});
	// Below the origin the quotient must FLOOR: -0.1 m is chunk -1, not chunk 0.
	CHECK(ve::lod_chunk_of_point(0, -0.1f, -25.7f, -51.2f) == ve::IVec3{-1, -2, -2});
	float o[3];
	ve::lod_chunk_origin(1, ve::IVec3{2, -1, 0}, o);
	CHECK(o[0] == doctest::Approx(204.8f));
	CHECK(o[1] == doctest::Approx(-102.4f));
	CHECK(o[2] == doctest::Approx(0.0f));
}

TEST_CASE("a chunk's aabb covers the cells its mesher owns") {
	float lo[3], hi[3];
	ve::lod_chunk_aabb(0, ve::IVec3{1, 0, 0}, lo, hi);
	// One overlap cell below the origin (the mesh convention), 64 cells above it.
	CHECK(lo[0] == doctest::Approx(25.6f - 0.4f));
	CHECK(hi[0] == doctest::Approx(25.6f + 25.6f));
	CHECK(lo[1] == doctest::Approx(-0.4f));
	CHECK(hi[1] == doctest::Approx(25.6f));
}

TEST_CASE("parent and child indices floor consistently across the origin") {
	CHECK(ve::lod_parent(ve::IVec3{0, 0, 0}) == ve::IVec3{0, 0, 0});
	CHECK(ve::lod_parent(ve::IVec3{3, 4, -1}) == ve::IVec3{0, 1, -1});
	CHECK(ve::lod_parent(ve::IVec3{-4, -5, 7}) == ve::IVec3{-1, -2, 1});
	CHECK(ve::lod_child_base(ve::IVec3{-1, 2, 0}) == ve::IVec3{-4, 8, 0});
	// Every child of a node has that node as its parent -- the property the walk relies on.
	const ve::IVec3 base = ve::lod_child_base(ve::IVec3{-1, 2, 0});
	for (int i = 0; i < ve::kLodRatio; i++)
		for (int j = 0; j < ve::kLodRatio; j++)
			for (int k = 0; k < ve::kLodRatio; k++)
				CHECK(ve::lod_parent(ve::IVec3{base.x + i, base.y + j, base.z + k}) ==
						ve::IVec3{-1, 2, 0});
}

TEST_CASE("distance is to the box, zero inside, and the far distance bounds it") {
	const float cam[3] = {12.8f, 12.8f, 12.8f};
	CHECK(ve::lod_chunk_distance(0, ve::IVec3{0, 0, 0}, cam) == doctest::Approx(0.0f));
	const float near_d = ve::lod_chunk_distance(0, ve::IVec3{4, 0, 0}, cam);
	const float far_d = ve::lod_chunk_far_distance(0, ve::IVec3{4, 0, 0}, cam);
	CHECK(near_d > 0.0f);
	CHECK(far_d > near_d);
	// The near-field skip test uses the far distance, so it must be a true upper bound.
	CHECK(far_d >= near_d);
}

TEST_CASE("screen error falls with distance and rises with cell size") {
	const float ppr = 1756.0f; // 1440 px over a 0.82 rad vertical fov
	const float a = ve::lod_screen_error(0, 200.0f, ppr);
	const float b = ve::lod_screen_error(0, 400.0f, ppr);
	const float c = ve::lod_screen_error(1, 200.0f, ppr);
	CHECK(a == doctest::Approx(0.4f * ppr / 200.0f));
	CHECK(b < a);
	CHECK(c == doctest::Approx(4.0f * a));
	// A camera inside the chunk must not divide by zero.
	CHECK(std::isfinite(ve::lod_screen_error(0, 0.0f, ppr)));
}

TEST_CASE("an op dirties chunks at every level, padded by a cell") {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 30.0f; op.pos[1] = 5.0f; op.pos[2] = 30.0f;
	op.radius = 5.0f;
	for (int l = 0; l < ve::kLodLevels; l++) {
		ve::IVec3 lo{}, hi{};
		ve::op_lod_chunk_range(op, l, &lo, &hi);
		CHECK(lo.x <= hi.x);
		const ve::IVec3 c = ve::lod_chunk_of_point(l, op.pos[0], op.pos[1], op.pos[2]);
		CHECK(lo.x <= c.x); CHECK(hi.x >= c.x);
		CHECK(lo.y <= c.y); CHECK(hi.y >= c.y);
		CHECK(lo.z <= c.z); CHECK(hi.z >= c.z);
	}
	// At L1 a 5 m sphere at 30 m straddles the 25.6 m boundary in x and z.
	ve::IVec3 lo{}, hi{};
	ve::op_lod_chunk_range(op, 0, &lo, &hi);
	CHECK(lo.x == 0);
	CHECK(hi.x == 1);
}

TEST_CASE("ops too small to move a sample at this level are dropped") {
	ve::WorldBounds b{{0, 0, 0}, {4, 4, 4}};
	ve::EditLog log(b);
	ve::EditOp big{};
	big.type = ve::kOpSphereSubtract;
	big.pos[0] = 12.0f; big.pos[1] = 12.0f; big.pos[2] = 12.0f;
	big.radius = 10.0f;
	ve::EditOp small = big;
	small.radius = 0.3f; // 0.6 m across: smaller than L2's 1.6 m cell
	log.append(big);
	log.append(small);

	std::vector<ve::EditOp> ops;
	CHECK(ve::lod_ops_for_chunk(log, 0, ve::IVec3{0, 0, 0}, &ops) == 2); // L1 keeps both
	CHECK(ve::lod_ops_for_chunk(log, 1, ve::IVec3{0, 0, 0}, &ops) == 1); // L2 drops the drill
	CHECK(ops[0].radius == doctest::Approx(10.0f));
}

TEST_CASE("an over-full op list keeps the chronological PREFIX") {
	ve::WorldBounds b{{0, 0, 0}, {4, 4, 4}};
	ve::EditLog log(b);
	// kMaxRegionOps + 8 ops, all big enough to survive the relevance cut, all over the
	// same chunk. Their radii number them so the survivors can be identified.
	for (int i = 0; i < ve::kMaxRegionOps + 8; i++) {
		ve::EditOp op{};
		op.type = ve::kOpSphereSubtract;
		op.pos[0] = 12.0f; op.pos[1] = 12.0f; op.pos[2] = 12.0f;
		op.radius = 5.0f + static_cast<float>(i) * 0.001f;
		log.append(op);
	}
	std::vector<ve::EditOp> ops;
	const int n = ve::lod_ops_for_chunk(log, 0, ve::IVec3{0, 0, 0}, &ops);
	CHECK(n == ve::kMaxRegionOps);
	// The FIRST 256, not the last: a prefix of an ordered CSG list is a valid world state.
	CHECK(ops.front().radius == doctest::Approx(5.0f));
	CHECK(ops.back().radius == doctest::Approx(5.0f + 255.0f * 0.001f));
}

TEST_CASE("the surface probe never says empty where a surface is") {
	ve::AnalyticGenerator gen;
	// The demo terrain's surface sits at y = kSurfaceY + hills in [41.2, 61.2].
	const ve::IVec3 on_surface = ve::lod_chunk_of_point(0, 12.0f, ve::kSurfaceY, 12.0f);
	CHECK(ve::lod_chunk_has_surface(gen, nullptr, 0, 0, on_surface, nullptr));
	// A chunk far above the terrain is empty at every level.
	const ve::IVec3 sky{0, 40, 0};
	CHECK(!ve::lod_chunk_has_surface(gen, nullptr, 0, 0, sky, nullptr));
	// Brute force at 5 cm over one L1 chunk: wherever a sign change really exists, the
	// probe must agree. A false positive costs one wasted build; a false negative is a
	// hole in the horizon, so only the safe direction is pinned.
	for (int cz = 0; cz < 3; cz++)
		for (int cy = 60; cy < 68; cy++)
			for (int cx = 0; cx < 3; cx++) {
				const ve::IVec3 c{cx, cy, cz};
				float lo[3], hi[3];
				ve::lod_chunk_aabb(0, c, lo, hi);
				bool pos = false, neg = false;
				for (int k = 0; k <= 8 && !(pos && neg); k++)
					for (int j = 0; j <= 8 && !(pos && neg); j++)
						for (int i = 0; i <= 8; i++) {
							const float p[3] = {lo[0] + (hi[0] - lo[0]) * i / 8.0f,
									lo[1] + (hi[1] - lo[1]) * j / 8.0f,
									lo[2] + (hi[2] - lo[2]) * k / 8.0f};
							const float d = ve::eval_field(gen, nullptr, 0, p[0], p[1], p[2]).sdf;
							if (d <= 0.0f) neg = true; else pos = true;
						}
				if (pos && neg) {
					CHECK(ve::lod_chunk_has_surface(gen, nullptr, 0, 0, c, nullptr));
				}
			}
}

TEST_CASE("root range covers the world and nothing else") {
	ve::WorldBounds b{{0, -64, 0}, {64, 8, 64}};
	ve::IVec3 lo{}, hi{};
	ve::lod_root_range(b, &lo, &hi);
	float wlo[3], whi[3];
	b.aabb(wlo, whi);
	// Every corner of the world's aabb lies in some root chunk.
	for (int a = 0; a < 3; a++) {
		const ve::IVec3 c0 = ve::lod_chunk_of_point(ve::kLodLevels - 1, wlo[0], wlo[1], wlo[2]);
		const ve::IVec3 c1 = ve::lod_chunk_of_point(ve::kLodLevels - 1,
				whi[0] - 0.01f, whi[1] - 0.01f, whi[2] - 0.01f);
		const int l = a == 0 ? lo.x : (a == 1 ? lo.y : lo.z);
		const int h = a == 0 ? hi.x : (a == 1 ? hi.y : hi.z);
		CHECK(l <= (a == 0 ? c0.x : (a == 1 ? c0.y : c0.z)));
		CHECK(h >= (a == 0 ? c1.x : (a == 1 ? c1.y : c1.z)));
	}
	CHECK(ve::lod_chunk_in_bounds(b, 0, ve::IVec3{0, 0, 0}));
	CHECK(!ve::lod_chunk_in_bounds(b, 0, ve::IVec3{64, 0, 0}));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: FAIL — `fatal error: lod/lod_grid.h: No such file or directory`.

- [ ] **Step 3: Add `lod/` to the native build**

Modify `extension/SConstruct`, replacing the `pure_sources` assignment:

```python
pure_sources = (Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") +
                Glob("src/mesh/*.cpp") + Glob("src/connectivity/*.cpp") +
                Glob("src/lod/*.cpp"))
```

(The GDExtension build already globs `src/*/*.cpp`, so `lod/` joins it with no change.)

- [ ] **Step 4: Write the header**

Create `extension/src/lod/lod_grid.h`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "mesh/mesh_chunk.h"
#include "world/edit_log.h"
#include "world/region.h"
#include <vector>

namespace ve {

// Spec section 4's hierarchy: four levels, each 4x the linear size of the one below, every
// one of them 64 cells across -- which is exactly the collision mesher's lattice, so the
// same three shaders and the same ve::DcGrid serve all of them (see the plan's Deliberate
// Decisions). Level index 0 is the spec's L1.
inline constexpr int kLodLevels = 4;
inline constexpr int kLodRatio = 4;
inline constexpr int kLodChunkCells = kChunkCells; // 64
inline constexpr float kLodCellSize[kLodLevels] = {0.4f, 1.6f, 6.4f, 25.6f};
inline constexpr float kLodChunkSize[kLodLevels] = {25.6f, 102.4f, 409.6f, 1638.4f};

// A chunk renders at the coarsest level whose cells project to fewer than this many pixels.
inline constexpr float kLodSseThreshold = 4.0f;
// Spec section 3's near/far seam. Chunks entirely nearer than the start are never built.
inline constexpr float kLodFadeStartM = 120.0f;
inline constexpr float kLodFadeEndM = 150.0f;

// Clamping accessors: a level index out of range is a bug, but reading past the table is a
// crash, and spec section 8 prefers the warn-and-carry-on failure.
float lod_cell_size(int level);
float lod_chunk_size(int level);

IVec3 lod_chunk_of_point(int level, float x, float y, float z);
void lod_chunk_origin(int level, IVec3 chunk, float out[3]);
// The box the MESH occupies: one overlap cell below the origin (mesh_chunk.h's convention)
// and 64 cells above it. Used for distance, frustum culling and dirty ranges alike.
void lod_chunk_aabb(int level, IVec3 chunk, float lo[3], float hi[3]);

IVec3 lod_parent(IVec3 chunk);     // floor(chunk / 4) on every axis
IVec3 lod_child_base(IVec3 chunk); // chunk * 4: the lowest of its 64 children

float lod_chunk_distance(int level, IVec3 chunk, const float cam[3]);     // 0 inside
float lod_chunk_far_distance(int level, IVec3 chunk, const float cam[3]); // farthest corner

// Pixels one cell of this level subtends at that distance. px_per_radian is
// (viewport_height / 2) / tan(fov_y / 2).
float lod_screen_error(int level, float distance_m, float px_per_radian);

bool lod_chunk_in_bounds(const WorldBounds &bounds, int level, IVec3 chunk);
// Inclusive coarsest-level chunk range covering the world. The CDLOD walk's roots.
void lod_root_range(const WorldBounds &bounds, IVec3 *lo, IVec3 *hi);

// Inclusive chunk range whose stored geometry an op can move, at one level. Same argument
// as ve::op_chunk_range: the op's own shape plus two cells covers both the sign flips and
// the vertices a changed sample can drag, and the mesh box already carries its overlap cell.
void op_lod_chunk_range(const EditOp &op, int level, IVec3 *lo, IVec3 *hi);

// The ops a chunk at this level actually needs, in global append order. Three filters, in
// this order (see the plan's Deliberate Decisions):
//   1. ve::collect_ops_for_aabb over the chunk's mesh box -- a coarse chunk spans many
//      regions, so one region's list is not enough (M4 errata 1's helper, same caveats).
//   2. ops whose world aabb is shorter than this level's cell on EVERY axis are dropped:
//      they cannot move a sample at this pitch, and they crowd out ops that can.
//   3. the surviving list is truncated to its first kMaxRegionOps entries -- a PREFIX, so
//      the result is the world as of some earlier edit rather than an incoherent mixture.
// Returns the number written to `out`.
int lod_ops_for_chunk(const EditLog &log, int level, IVec3 chunk, std::vector<EditOp> *out);

// Conservative "this chunk may contain a surface", with the same derived margin
// ve::chunk_has_surface uses: with probe spacing s the farthest unsampled point is
// s*sqrt(3)/2 away and Generator::lipschitz() bounds how fast the reported distance can
// shrink, so a probe clearing s*sqrt(3)/2*L on one side proves no crossing between probes.
// False positives cost one wasted build; a false negative is a hole in the horizon.
bool lod_chunk_has_surface(const Generator &gen, const EditOp *ops, int op_count, int level,
		IVec3 chunk, const VolumeStore *volumes = nullptr);

} // namespace ve
```

- [ ] **Step 5: Write the implementation**

Create `extension/src/lod/lod_grid.cpp`:

```cpp
#include "lod/lod_grid.h"
#include "world/brick_eval.h"
#include <algorithm>
#include <cmath>

namespace ve {
namespace {

int clamp_level(int level) {
	return level < 0 ? 0 : (level >= kLodLevels ? kLodLevels - 1 : level);
}

// The probe grid is the same shape as the collision chunk's: (steps + 1)^3 samples over the
// chunk box. Four steps is 125 evaluations, which is what makes probing thousands of
// candidate chunks per second affordable.
constexpr int kProbeSteps = 4;

} // namespace

float lod_cell_size(int level) { return kLodCellSize[clamp_level(level)]; }
float lod_chunk_size(int level) { return kLodChunkSize[clamp_level(level)]; }

IVec3 lod_chunk_of_point(int level, float x, float y, float z) {
	const float s = lod_chunk_size(level);
	return IVec3{static_cast<int>(std::floor(x / s)), static_cast<int>(std::floor(y / s)),
			static_cast<int>(std::floor(z / s))};
}

void lod_chunk_origin(int level, IVec3 chunk, float out[3]) {
	const float s = lod_chunk_size(level);
	out[0] = static_cast<float>(chunk.x) * s;
	out[1] = static_cast<float>(chunk.y) * s;
	out[2] = static_cast<float>(chunk.z) * s;
}

void lod_chunk_aabb(int level, IVec3 chunk, float lo[3], float hi[3]) {
	float o[3];
	lod_chunk_origin(level, chunk, o);
	const float c = lod_cell_size(level);
	const float s = lod_chunk_size(level);
	for (int a = 0; a < 3; a++) {
		lo[a] = o[a] - c; // the mesher's overlap cell below the origin
		hi[a] = o[a] + s;
	}
}

IVec3 lod_parent(IVec3 chunk) {
	return IVec3{floor_div(chunk.x, kLodRatio), floor_div(chunk.y, kLodRatio),
			floor_div(chunk.z, kLodRatio)};
}

IVec3 lod_child_base(IVec3 chunk) {
	return IVec3{chunk.x * kLodRatio, chunk.y * kLodRatio, chunk.z * kLodRatio};
}

float lod_chunk_distance(int level, IVec3 chunk, const float cam[3]) {
	float lo[3], hi[3];
	lod_chunk_aabb(level, chunk, lo, hi);
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float v = cam[a] < lo[a] ? lo[a] - cam[a] : (cam[a] > hi[a] ? cam[a] - hi[a] : 0.0f);
		d2 += v * v;
	}
	return std::sqrt(d2);
}

float lod_chunk_far_distance(int level, IVec3 chunk, const float cam[3]) {
	float lo[3], hi[3];
	lod_chunk_aabb(level, chunk, lo, hi);
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float v = std::max(std::fabs(cam[a] - lo[a]), std::fabs(cam[a] - hi[a]));
		d2 += v * v;
	}
	return std::sqrt(d2);
}

float lod_screen_error(int level, float distance_m, float px_per_radian) {
	// A camera inside the chunk reports distance 0; 1 m is the floor, which makes the error
	// large (so the walk descends) without producing an infinity the sort would choke on.
	return lod_cell_size(level) * px_per_radian / std::max(distance_m, 1.0f);
}

bool lod_chunk_in_bounds(const WorldBounds &bounds, int level, IVec3 chunk) {
	float lo[3], hi[3];
	lod_chunk_aabb(level, chunk, lo, hi);
	float wlo[3], whi[3];
	bounds.aabb(wlo, whi);
	for (int a = 0; a < 3; a++) {
		if (hi[a] <= wlo[a] || lo[a] >= whi[a]) return false;
	}
	return true;
}

void lod_root_range(const WorldBounds &bounds, IVec3 *lo, IVec3 *hi) {
	float wlo[3], whi[3];
	bounds.aabb(wlo, whi);
	const float s = lod_chunk_size(kLodLevels - 1);
	int l[3], h[3];
	for (int a = 0; a < 3; a++) {
		l[a] = static_cast<int>(std::floor(wlo[a] / s));
		h[a] = static_cast<int>(std::floor((whi[a] - 1e-3f) / s));
		if (h[a] < l[a]) h[a] = l[a];
	}
	*lo = IVec3{l[0], l[1], l[2]};
	*hi = IVec3{h[0], h[1], h[2]};
}

void op_lod_chunk_range(const EditOp &op, int level, IVec3 *lo, IVec3 *hi) {
	float olo[3], ohi[3];
	op_world_aabb(op, olo, ohi);
	const float pad = 2.0f * lod_cell_size(level);
	const float s = lod_chunk_size(level);
	const float c = lod_cell_size(level);
	int l[3], h[3];
	for (int a = 0; a < 3; a++) {
		// The chunk's mesh box starts one cell BELOW its origin, so a chunk whose origin is
		// just above the op can still hold a sample the op moves; adding that cell to the
		// low side of the query is what covers it.
		l[a] = static_cast<int>(std::floor((olo[a] - pad) / s));
		h[a] = static_cast<int>(std::floor((ohi[a] + pad + c) / s));
	}
	*lo = IVec3{l[0], l[1], l[2]};
	*hi = IVec3{h[0], h[1], h[2]};
}

int lod_ops_for_chunk(const EditLog &log, int level, IVec3 chunk, std::vector<EditOp> *out) {
	out->clear();
	float lo[3], hi[3];
	lod_chunk_aabb(level, chunk, lo, hi);
	std::vector<EditOp> all;
	collect_ops_for_aabb(log, lo, hi, &all);

	const float cell = lod_cell_size(level);
	for (const EditOp &op : all) {
		float olo[3], ohi[3];
		op_world_aabb(op, olo, ohi);
		// Relevance: an op whose whole extent fits inside one cell on every axis cannot flip
		// a sample's sign at this pitch, whatever its position -- two adjacent samples are a
		// cell apart and the op reaches neither farther than that.
		if (ohi[0] - olo[0] < cell && ohi[1] - olo[1] < cell && ohi[2] - olo[2] < cell) continue;
		out->push_back(op);
		if (static_cast<int>(out->size()) >= kMaxRegionOps) break; // chronological PREFIX
	}
	return static_cast<int>(out->size());
}

bool lod_chunk_has_surface(const Generator &gen, const EditOp *ops, int op_count, int level,
		IVec3 chunk, const VolumeStore *volumes) {
	float lo[3], hi[3];
	lod_chunk_aabb(level, chunk, lo, hi);
	const float span[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
	const float spacing = span[0] / static_cast<float>(kProbeSteps);
	const float margin = 0.5f * 1.7320508f * spacing * gen.lipschitz();

	bool any_near = false;
	bool pos = false, neg = false;
	for (int k = 0; k <= kProbeSteps; k++)
		for (int j = 0; j <= kProbeSteps; j++)
			for (int i = 0; i <= kProbeSteps; i++) {
				const float p[3] = {lo[0] + span[0] * i / kProbeSteps,
						lo[1] + span[1] * j / kProbeSteps,
						lo[2] + span[2] * k / kProbeSteps};
				const float d = eval_field(gen, ops, op_count, p[0], p[1], p[2], volumes).sdf;
				if (d <= 0.0f) neg = true; else pos = true;
				if (std::fabs(d) <= margin) any_near = true;
			}
	// A sign change between probes proves a crossing; a probe within the margin cannot be
	// ruled out. Either is enough to build.
	return (pos && neg) || any_near;
}

} // namespace ve
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: PASS — all `test_lod_grid.cpp` cases green, every earlier suite unchanged.

- [ ] **Step 7: Commit**

```bash
git add extension/src/lod/lod_grid.h extension/src/lod/lod_grid.cpp \
        extension/tests/test_lod_grid.cpp extension/SConstruct
git commit -m "feat(lod): four-level chunk lattice, dirty ranges and op relevance"
```

---

### Task 2: `lod/lod_page` — quads become a drawable, tiled page

Everything about how a meshed chunk turns into bytes: where a quad's tile lives in the page texture, how a vertex is squeezed into eight bytes, how skirts are grown from the mesh alone, and how the result is split across at most two pages. All of it is arithmetic with no I/O, so all of it is a doctest.

**Files:**
- Create: `extension/src/lod/lod_page.h`, `extension/src/lod/lod_page.cpp`
- Create: `extension/tests/test_lod_page.cpp`
- Modify: `shaders/common.glslh` (add `bayer4`, `oct_encode`, `oct_decode`)

**Interfaces:**
- Consumes: `ve::IVec3`, `ve::lod_cell_size`, `ve::lod_chunk_origin`, `ve::lod_chunk_size`, `ve::kLodChunkCells` (`lod/lod_grid.h`).
- Produces:
  - `ve::kLodTileTexels = 4`, `kLodPageTexW = 512`, `kLodPageTexH = 256`, `kLodTilesX = 128`, `kLodTilesY = 64`, `kLodTilesPerPage = 8192`, `kLodQuadsPerPage = 10240`, `kLodVertsPerPage = 40960`, `kLodIndicesPerPage = 61440`, `kLodVertexBytes = 8`, `kLodPageTexels = 131072`, `kLodPageTexBytes = 524288`, `kLodMaxPagesPerChunk = 2`, `kLodQuantPadCells = 8`, `kLodSkirtCells = 4`
  - `struct ve::LodQuad { uint32_t v[4]; }`
  - `struct ve::LodPageBuild { std::vector<uint8_t> vertices; int quad_count; int surface_quads; }`
  - `struct ve::LodChunkBuild { float quant_lo[3]; float quant_size; std::vector<LodPageBuild> pages; bool overflow; }`
  - `void ve::lod_tile_origin(int tile, int *tx, int *ty)`
  - `void ve::lod_tile_corner_uv(int tile, int corner, float uv[2])`
  - `uint16_t ve::lod_pack_axis(float v, float lo, float size)`, `float ve::lod_unpack_axis(uint16_t, float lo, float size)`
  - `uint16_t ve::lod_pack_attr(int tile, int corner)`, `void ve::lod_unpack_attr(uint16_t, int *tile, int *corner)`
  - `void ve::lod_quant_box(int level, IVec3 chunk, float lo[3], float *size)`
  - `void ve::lod_shared_indices(std::vector<uint16_t> *out)`
  - `void ve::build_lod_pages(const float *positions, int vertex_count, const LodQuad *quads, int quad_count, int level, IVec3 chunk, LodChunkBuild *out)`
  - `void ve::oct_encode(const float n[3], uint8_t out[2])`, `void ve::oct_decode(const uint8_t in[2], float out[3])`

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_lod_page.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_page.h"
#include "lod/lod_grid.h"
#include <cmath>
#include <vector>

TEST_CASE("page geometry is self-consistent") {
	CHECK(ve::kLodTilesX * ve::kLodTileTexels == ve::kLodPageTexW);
	CHECK(ve::kLodTilesY * ve::kLodTileTexels == ve::kLodPageTexH);
	CHECK(ve::kLodTilesX * ve::kLodTilesY == ve::kLodTilesPerPage);
	CHECK(ve::kLodVertsPerPage == 4 * ve::kLodQuadsPerPage);
	CHECK(ve::kLodIndicesPerPage == 6 * ve::kLodQuadsPerPage);
	// uint16 indices address a page's vertices; 40960 < 65536 is what makes that legal.
	CHECK(ve::kLodVertsPerPage < 65536);
	// A page holds at least as many quads as it has tiles, plus room for skirts.
	CHECK(ve::kLodQuadsPerPage > ve::kLodTilesPerPage);
	CHECK(ve::kLodPageTexels == ve::kLodPageTexW * ve::kLodPageTexH);
	CHECK(ve::kLodPageTexBytes == ve::kLodPageTexels * 4);
}

TEST_CASE("tiles tile the page, and corner uvs are texel centres inside their own tile") {
	int tx = -1, ty = -1;
	ve::lod_tile_origin(0, &tx, &ty);
	CHECK(tx == 0);
	CHECK(ty == 0);
	ve::lod_tile_origin(ve::kLodTilesX, &tx, &ty);
	CHECK(tx == 0);
	CHECK(ty == ve::kLodTileTexels);
	ve::lod_tile_origin(ve::kLodTilesPerPage - 1, &tx, &ty);
	CHECK(tx == ve::kLodPageTexW - ve::kLodTileTexels);
	CHECK(ty == ve::kLodPageTexH - ve::kLodTileTexels);

	// Corner order: 0 = (0,0), 1 = (1,0), 2 = (1,1), 3 = (0,1) in tile-normalised space.
	const int tile = 5 + 3 * ve::kLodTilesX;
	ve::lod_tile_origin(tile, &tx, &ty);
	const float t = static_cast<float>(ve::kLodTileTexels);
	for (int c = 0; c < 4; c++) {
		float uv[2];
		ve::lod_tile_corner_uv(tile, c, uv);
		const float px = uv[0] * ve::kLodPageTexW;
		const float py = uv[1] * ve::kLodPageTexH;
		// Inside this tile...
		CHECK(px > tx);
		CHECK(px < tx + t);
		CHECK(py > ty);
		CHECK(py < ty + t);
		// ...and exactly on a texel centre at the tile's edge.
		const float ex = (c == 1 || c == 2) ? tx + t - 0.5f : tx + 0.5f;
		const float ey = (c >= 2) ? ty + t - 0.5f : ty + 0.5f;
		CHECK(px == doctest::Approx(ex));
		CHECK(py == doctest::Approx(ey));
	}
	// Bilinear across the quad can never leave the tile: the extreme uvs are the corners.
	float a[2], b[2];
	ve::lod_tile_corner_uv(tile, 0, a);
	ve::lod_tile_corner_uv(tile, 2, b);
	CHECK(a[0] * ve::kLodPageTexW >= tx + 0.5f);
	CHECK(b[0] * ve::kLodPageTexW <= tx + t - 0.5f);
}

TEST_CASE("attribute packing round-trips every legal tile and corner") {
	for (int tile : {0, 1, 4095, 8191}) {
		for (int corner = 0; corner < 4; corner++) {
			int t = -1, c = -1;
			ve::lod_unpack_attr(ve::lod_pack_attr(tile, corner), &t, &c);
			CHECK(t == tile);
			CHECK(c == corner);
		}
	}
}

TEST_CASE("position quantisation covers the mesh box plus skirts, to under a millimetre") {
	float lo[3];
	float size = 0.0f;
	ve::lod_quant_box(0, ve::IVec3{2, 0, -1}, lo, &size);
	float o[3];
	ve::lod_chunk_origin(0, ve::IVec3{2, 0, -1}, o);
	const float cell = ve::lod_cell_size(0);
	// Eight cells of pad each side covers the overlap cell (1) and the skirt (4).
	CHECK(lo[0] == doctest::Approx(o[0] - ve::kLodQuantPadCells * cell));
	CHECK(size == doctest::Approx(ve::lod_chunk_size(0) + 2.0f * ve::kLodQuantPadCells * cell));
	CHECK(size / 65535.0f < 0.001f); // sub-millimetre at L1

	for (float f : {0.0f, 0.25f, 0.5f, 1.0f}) {
		const float v = lo[0] + size * f;
		const float back = ve::lod_unpack_axis(ve::lod_pack_axis(v, lo[0], size), lo[0], size);
		CHECK(back == doctest::Approx(v).epsilon(0.0f).scale(1.0f));
		CHECK(std::fabs(back - v) < 0.001f);
	}
	// Out of range clamps rather than wrapping: a wrapped vertex would fly across the chunk.
	CHECK(ve::lod_pack_axis(lo[0] - 100.0f, lo[0], size) == 0);
	CHECK(ve::lod_pack_axis(lo[0] + size + 100.0f, lo[0], size) == 65535);
}

TEST_CASE("the shared index buffer is the same quad pattern repeated") {
	std::vector<uint16_t> idx;
	ve::lod_shared_indices(&idx);
	CHECK(static_cast<int>(idx.size()) == ve::kLodIndicesPerPage);
	CHECK(idx[0] == 0); CHECK(idx[1] == 1); CHECK(idx[2] == 2);
	CHECK(idx[3] == 0); CHECK(idx[4] == 2); CHECK(idx[5] == 3);
	const int q = 1000;
	CHECK(idx[q * 6 + 0] == q * 4 + 0);
	CHECK(idx[q * 6 + 4] == q * 4 + 2);
	CHECK(idx.back() == ve::kLodVertsPerPage - 1);
}

// One quad, dead centre of an L1 chunk, far from every face: no skirt, one page, four
// vertices carrying tile 0 and corners 0..3.
TEST_CASE("a single interior quad becomes one page with four vertices") {
	const ve::IVec3 chunk{0, 0, 0};
	float o[3];
	ve::lod_chunk_origin(0, chunk, o);
	const float c = ve::lod_cell_size(0);
	const float m = o[0] + 32.0f * c; // middle of the chunk on every axis
	const float pos[12] = {m, m, m, m + c, m, m, m + c, m, m + c, m, m, m + c};
	const ve::LodQuad quads[1] = {{{0, 1, 2, 3}}};

	ve::LodChunkBuild b;
	ve::build_lod_pages(pos, 4, quads, 1, 0, chunk, &b);
	REQUIRE(b.pages.size() == 1);
	CHECK(b.pages[0].surface_quads == 1);
	CHECK(b.pages[0].quad_count == 1);
	CHECK(!b.overflow);
	CHECK(static_cast<int>(b.pages[0].vertices.size()) == 4 * ve::kLodVertexBytes);

	const uint16_t *v = reinterpret_cast<const uint16_t *>(b.pages[0].vertices.data());
	for (int i = 0; i < 4; i++) {
		int tile = -1, corner = -1;
		ve::lod_unpack_attr(v[i * 4 + 3], &tile, &corner);
		CHECK(tile == 0);
		CHECK(corner == i);
		const float x = ve::lod_unpack_axis(v[i * 4 + 0], b.quant_lo[0], b.quant_size);
		CHECK(std::fabs(x - pos[i * 3 + 0]) < 0.001f);
	}
}

TEST_CASE("a quad on a chunk face grows a skirt into the solid, sharing its parent's tile") {
	const ve::IVec3 chunk{0, 0, 0};
	float o[3];
	ve::lod_chunk_origin(0, chunk, o);
	const float c = ve::lod_cell_size(0);
	const float m = o[1] + 32.0f * c;
	// A horizontal quad whose x = the chunk's MAXIMUM face (o[0] + 64c). Its normal is +Y,
	// so the skirt hangs straight down.
	const float top = o[0] + 64.0f * c;
	const float pos[12] = {top - c, m, o[2] + 10.0f * c, top, m, o[2] + 10.0f * c,
			top, m, o[2] + 11.0f * c, top - c, m, o[2] + 11.0f * c};
	const ve::LodQuad quads[1] = {{{0, 1, 2, 3}}};

	ve::LodChunkBuild b;
	ve::build_lod_pages(pos, 4, quads, 1, 0, chunk, &b);
	REQUIRE(b.pages.size() == 1);
	CHECK(b.pages[0].surface_quads == 1);
	// Exactly one edge (v1 -> v2) has both endpoints on the +x face.
	CHECK(b.pages[0].quad_count == 2);

	const uint16_t *v = reinterpret_cast<const uint16_t *>(b.pages[0].vertices.data());
	// The skirt's four vertices are quad 1: verts 4..7.
	for (int i = 4; i < 8; i++) {
		int tile = -1, corner = -1;
		ve::lod_unpack_attr(v[i * 4 + 3], &tile, &corner);
		CHECK(tile == 0); // the parent's tile, not a new one
	}
	const float y_top = ve::lod_unpack_axis(v[4 * 4 + 1], b.quant_lo[1], b.quant_size);
	const float y_bot = ve::lod_unpack_axis(v[6 * 4 + 1], b.quant_lo[1], b.quant_size);
	CHECK(y_top > y_bot);
	CHECK(std::fabs((y_top - y_bot) - ve::kLodSkirtCells * c) < 0.01f);
}

TEST_CASE("surface quads past a page's tiles spill into the next page") {
	// kLodTilesPerPage + 5 degenerate-but-legal quads, all interior so none skirts.
	const int n = ve::kLodTilesPerPage + 5;
	std::vector<float> pos;
	std::vector<ve::LodQuad> quads;
	float o[3];
	ve::lod_chunk_origin(0, ve::IVec3{0, 0, 0}, o);
	const float c = ve::lod_cell_size(0);
	for (int q = 0; q < n; q++) {
		const uint32_t base = static_cast<uint32_t>(q * 4);
		const float x = o[0] + (20.0f + (q % 8) * 0.01f) * c;
		const float y = o[1] + 32.0f * c;
		const float z = o[2] + 30.0f * c;
		for (int k = 0; k < 4; k++) {
			pos.push_back(x + (k == 1 || k == 2 ? c * 0.5f : 0.0f));
			pos.push_back(y);
			pos.push_back(z + (k >= 2 ? c * 0.5f : 0.0f));
		}
		quads.push_back({{base, base + 1, base + 2, base + 3}});
	}
	ve::LodChunkBuild b;
	ve::build_lod_pages(pos.data(), n * 4, quads.data(), n, 0, ve::IVec3{0, 0, 0}, &b);
	REQUIRE(b.pages.size() == 2);
	CHECK(b.pages[0].surface_quads == ve::kLodTilesPerPage);
	CHECK(b.pages[1].surface_quads == 5);
	CHECK(!b.overflow);

	// Page 1's first quad must use tile 0 of ITS OWN page, not tile 8192 of page 0.
	const uint16_t *v = reinterpret_cast<const uint16_t *>(b.pages[1].vertices.data());
	int tile = -1, corner = -1;
	ve::lod_unpack_attr(v[3], &tile, &corner);
	CHECK(tile == 0);
}

TEST_CASE("more surface quads than the page cap is fail-soft, not a buffer overrun") {
	const int n = ve::kLodTilesPerPage * ve::kLodMaxPagesPerChunk + 100;
	std::vector<float> pos(static_cast<size_t>(n) * 12, 0.0f);
	std::vector<ve::LodQuad> quads;
	for (int q = 0; q < n; q++) {
		const uint32_t base = static_cast<uint32_t>(q * 4);
		quads.push_back({{base, base + 1, base + 2, base + 3}});
	}
	ve::LodChunkBuild b;
	ve::build_lod_pages(pos.data(), n * 4, quads.data(), n, 0, ve::IVec3{0, 0, 0}, &b);
	CHECK(static_cast<int>(b.pages.size()) == ve::kLodMaxPagesPerChunk);
	CHECK(b.overflow);
	for (const ve::LodPageBuild &p : b.pages) {
		CHECK(p.quad_count <= ve::kLodQuadsPerPage);
		CHECK(static_cast<int>(p.vertices.size()) == p.quad_count * 4 * ve::kLodVertexBytes);
	}
}

TEST_CASE("octahedral normal encoding round-trips to under two degrees") {
	const float dirs[7][3] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0, 0, -1},
			{0.577f, 0.577f, 0.577f}, {-0.577f, -0.577f, 0.577f}, {0.1f, -0.99f, 0.05f}};
	for (const auto &d : dirs) {
		float n[3] = {d[0], d[1], d[2]};
		const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		for (int a = 0; a < 3; a++) n[a] /= len;
		uint8_t e[2];
		ve::oct_encode(n, e);
		float back[3];
		ve::oct_decode(e, back);
		const float dot = n[0] * back[0] + n[1] * back[1] + n[2] * back[2];
		CHECK(dot > 0.9994f); // ~2 degrees
	}
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: FAIL — `fatal error: lod/lod_page.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `extension/src/lod/lod_page.h`:

```cpp
#pragma once
#include "lod/lod_grid.h"
#include <cstdint>
#include <vector>

namespace ve {

// Spec section 4's "texture bake ... at ~4x the chunk's mesh sampling", read per quad: each
// quad owns a 4x4 tile whose corner texels are the quad's corners, so its three interior
// spans carry field detail the mesh cannot. See the plan's Deliberate Decisions for why the
// tile is per quad rather than a per-chunk plane, and why it is 4x4 rather than 2x2.
inline constexpr int kLodTileTexels = 4;
inline constexpr int kLodPageTexW = 512;
inline constexpr int kLodPageTexH = 256;
inline constexpr int kLodTilesX = kLodPageTexW / kLodTileTexels;   // 128
inline constexpr int kLodTilesY = kLodPageTexH / kLodTileTexels;   // 64
inline constexpr int kLodTilesPerPage = kLodTilesX * kLodTilesY;   // 8192
inline constexpr int kLodPageTexels = kLodPageTexW * kLodPageTexH; // 131072
inline constexpr int kLodPageTexBytes = kLodPageTexels * 4;        // RGBA8, 512 KB

// Skirts share their parent's tile, so a page may hold more quads than it holds tiles. The
// margin is 2048 quads, against a boundary ring of a few hundred on a real chunk.
inline constexpr int kLodQuadsPerPage = 10240;
inline constexpr int kLodVertsPerPage = 4 * kLodQuadsPerPage;   // 40960, addressable by uint16
inline constexpr int kLodIndicesPerPage = 6 * kLodQuadsPerPage; // 61440
inline constexpr int kLodVertexBytes = 8;                       // 3 x uint16 pos + 1 x uint16 attr
inline constexpr int kLodMaxPagesPerChunk = 2;

// The quantisation box is the chunk padded by this many cells each side: one for the
// mesher's overlap cell, four for the skirt, three of headroom.
inline constexpr int kLodQuantPadCells = 8;
// How far a boundary curtain hangs, in cells of ITS OWN level. A one-level jump mismatches
// by at most one coarse cell = four fine ones, so four covers it (spec section 4's skirts).
inline constexpr int kLodSkirtCells = 4;

// Four corner vertex indices, already wound: (v0,v1,v2) and (v0,v2,v3) are the triangles.
struct LodQuad {
	uint32_t v[4] = {0, 0, 0, 0};
};

struct LodPageBuild {
	std::vector<uint8_t> vertices; // kLodVertexBytes * 4 * quad_count
	int quad_count = 0;            // surface quads + skirt quads
	int surface_quads = 0;         // the first this many are tiled; the rest are skirts
};

struct LodChunkBuild {
	float quant_lo[3] = {0.0f, 0.0f, 0.0f};
	float quant_size = 0.0f;
	std::vector<LodPageBuild> pages;
	bool overflow = false; // a cap was hit: the chunk is missing quads
};

void lod_tile_origin(int tile, int *tx, int *ty);
// uv in [0,1]^2 of the PAGE texture, at the centre of the tile's corner texel.
void lod_tile_corner_uv(int tile, int corner, float uv[2]);

uint16_t lod_pack_axis(float v, float lo, float size);
float lod_unpack_axis(uint16_t q, float lo, float size);
uint16_t lod_pack_attr(int tile, int corner); // tile in bits 0-12, corner in bits 13-14
void lod_unpack_attr(uint16_t a, int *tile, int *corner);

void lod_quant_box(int level, IVec3 chunk, float lo[3], float *size);

// The one index buffer every page shares: quad q is vertices 4q..4q+3 as (0,1,2)(0,2,3).
void lod_shared_indices(std::vector<uint16_t> *out);

// Splits a meshed chunk into pages: tiles the surface quads, grows skirts from the boundary
// edges, packs every vertex. `positions` is 3 floats per vertex in WORLD space, exactly as
// ve::dual_contour and shaders/mesh_cells.comp.glsl produce it.
void build_lod_pages(const float *positions, int vertex_count, const LodQuad *quads,
		int quad_count, int level, IVec3 chunk, LodChunkBuild *out);

// Signed octahedral encoding, mirrored in shaders/common.glslh. Two bytes per normal.
void oct_encode(const float n[3], uint8_t out[2]);
void oct_decode(const uint8_t in[2], float out[3]);

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/lod/lod_page.cpp`:

```cpp
#include "lod/lod_page.h"
#include <algorithm>
#include <cmath>

namespace ve {
namespace {

void write_u16(uint8_t *dst, uint16_t v) {
	dst[0] = static_cast<uint8_t>(v & 0xFFu);
	dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void quad_normal(const float *positions, const LodQuad &q, float out[3]) {
	const float *a = positions + q.v[0] * 3;
	const float *b = positions + q.v[1] * 3;
	const float *c = positions + q.v[2] * 3;
	const float *d = positions + q.v[3] * 3;
	// Cross of the diagonals: robust on the slivers a surface-nets mesh routinely produces,
	// where one triangle can be degenerate while the quad is not.
	const float u[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	const float v[3] = {d[0] - b[0], d[1] - b[1], d[2] - b[2]};
	float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
	const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len < 1e-8f) {
		out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f;
		return;
	}
	for (int i = 0; i < 3; i++) out[i] = n[i] / len;
}

} // namespace

void lod_tile_origin(int tile, int *tx, int *ty) {
	const int t = tile < 0 ? 0 : (tile >= kLodTilesPerPage ? kLodTilesPerPage - 1 : tile);
	*tx = (t % kLodTilesX) * kLodTileTexels;
	*ty = (t / kLodTilesX) * kLodTileTexels;
}

void lod_tile_corner_uv(int tile, int corner, float uv[2]) {
	int tx = 0, ty = 0;
	lod_tile_origin(tile, &tx, &ty);
	const float edge = static_cast<float>(kLodTileTexels) - 0.5f;
	const float px = static_cast<float>(tx) + ((corner == 1 || corner == 2) ? edge : 0.5f);
	const float py = static_cast<float>(ty) + ((corner >= 2) ? edge : 0.5f);
	uv[0] = px / static_cast<float>(kLodPageTexW);
	uv[1] = py / static_cast<float>(kLodPageTexH);
}

uint16_t lod_pack_axis(float v, float lo, float size) {
	if (!(size > 0.0f)) return 0;
	const float t = (v - lo) / size;
	const float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return static_cast<uint16_t>(std::lround(c * 65535.0f));
}

float lod_unpack_axis(uint16_t q, float lo, float size) {
	return lo + (static_cast<float>(q) / 65535.0f) * size;
}

uint16_t lod_pack_attr(int tile, int corner) {
	const uint32_t t = static_cast<uint32_t>(tile) & 0x1FFFu;
	const uint32_t c = static_cast<uint32_t>(corner) & 0x3u;
	return static_cast<uint16_t>(t | (c << 13));
}

void lod_unpack_attr(uint16_t a, int *tile, int *corner) {
	*tile = static_cast<int>(a & 0x1FFFu);
	*corner = static_cast<int>((a >> 13) & 0x3u);
}

void lod_quant_box(int level, IVec3 chunk, float lo[3], float *size) {
	float o[3];
	lod_chunk_origin(level, chunk, o);
	const float c = lod_cell_size(level);
	for (int a = 0; a < 3; a++) lo[a] = o[a] - kLodQuantPadCells * c;
	*size = lod_chunk_size(level) + 2.0f * kLodQuantPadCells * c;
}

void lod_shared_indices(std::vector<uint16_t> *out) {
	out->clear();
	out->reserve(kLodIndicesPerPage);
	for (int q = 0; q < kLodQuadsPerPage; q++) {
		const uint16_t b = static_cast<uint16_t>(q * 4);
		out->push_back(b);
		out->push_back(static_cast<uint16_t>(b + 1));
		out->push_back(static_cast<uint16_t>(b + 2));
		out->push_back(b);
		out->push_back(static_cast<uint16_t>(b + 2));
		out->push_back(static_cast<uint16_t>(b + 3));
	}
}

void build_lod_pages(const float *positions, int vertex_count, const LodQuad *quads,
		int quad_count, int level, IVec3 chunk, LodChunkBuild *out) {
	out->pages.clear();
	out->overflow = false;
	lod_quant_box(level, chunk, out->quant_lo, &out->quant_size);
	if (!positions || !quads || quad_count <= 0 || vertex_count <= 0) return;

	const float cell = lod_cell_size(level);
	const float skirt = kLodSkirtCells * cell;
	float cbox_lo[3], cbox_hi[3];
	lod_chunk_aabb(level, chunk, cbox_lo, cbox_hi);
	// A vertex is "on" a face plane when it is within 1.5 cells of it: the dual vertex of a
	// boundary cell sits somewhere inside that cell, so one cell of slack is structural and
	// the extra half absorbs the interpolation.
	const float face_eps = 1.5f * cell;

	const int max_surface = kLodTilesPerPage * kLodMaxPagesPerChunk;
	const int surface_total = std::min(quad_count, max_surface);
	if (quad_count > max_surface) out->overflow = true;

	const int page_count = (surface_total + kLodTilesPerPage - 1) / kLodTilesPerPage;
	out->pages.resize(std::max(page_count, 1));

	auto emit = [&](LodPageBuild &page, const float *p0, const float *p1, const float *p2,
					const float *p3, int tile, const int corners[4]) {
		if (page.quad_count >= kLodQuadsPerPage) {
			out->overflow = true;
			return;
		}
		const float *src[4] = {p0, p1, p2, p3};
		const size_t base = page.vertices.size();
		page.vertices.resize(base + 4 * kLodVertexBytes);
		uint8_t *dst = page.vertices.data() + base;
		for (int k = 0; k < 4; k++) {
			for (int a = 0; a < 3; a++)
				write_u16(dst + k * kLodVertexBytes + a * 2,
						lod_pack_axis(src[k][a], out->quant_lo[a], out->quant_size));
			write_u16(dst + k * kLodVertexBytes + 6, lod_pack_attr(tile, corners[k]));
		}
		page.quad_count++;
	};

	for (int q = 0; q < surface_total; q++) {
		const LodQuad &quad = quads[q];
		bool ok = true;
		for (int k = 0; k < 4; k++)
			if (quad.v[k] >= static_cast<uint32_t>(vertex_count)) ok = false;
		if (!ok) {
			// Fail-soft (spec section 8): a stray index is a meshing anomaly, not a crash.
			out->overflow = true;
			continue;
		}
		LodPageBuild &page = out->pages[q / kLodTilesPerPage];
		const int tile = q % kLodTilesPerPage;
		const float *p[4] = {positions + quad.v[0] * 3, positions + quad.v[1] * 3,
				positions + quad.v[2] * 3, positions + quad.v[3] * 3};
		const int corners[4] = {0, 1, 2, 3};
		emit(page, p[0], p[1], p[2], p[3], tile, corners);
		page.surface_quads++;
	}

	// Skirts, in a second sweep so that every page's surface quads are contiguous and its
	// tile indices are exactly 0..surface_quads-1 -- which is the mapping the bake shader
	// derives independently on the GPU, with no CPU list to agree with.
	for (int q = 0; q < surface_total; q++) {
		const LodQuad &quad = quads[q];
		bool ok = true;
		for (int k = 0; k < 4; k++)
			if (quad.v[k] >= static_cast<uint32_t>(vertex_count)) ok = false;
		if (!ok) continue;
		LodPageBuild &page = out->pages[q / kLodTilesPerPage];
		const int tile = q % kLodTilesPerPage;
		float n[3];
		quad_normal(positions, quad, n);
		const float s[3] = {-n[0] * skirt, -n[1] * skirt, -n[2] * skirt};

		for (int e = 0; e < 4; e++) {
			const int ia = e, ib = (e + 1) & 3;
			const float *a = positions + quad.v[ia] * 3;
			const float *b = positions + quad.v[ib] * 3;
			bool on_face = false;
			for (int axis = 0; axis < 3 && !on_face; axis++) {
				if (std::fabs(a[axis] - cbox_lo[axis]) < face_eps &&
						std::fabs(b[axis] - cbox_lo[axis]) < face_eps)
					on_face = true;
				if (std::fabs(a[axis] - cbox_hi[axis]) < face_eps &&
						std::fabs(b[axis] - cbox_hi[axis]) < face_eps)
					on_face = true;
			}
			if (!on_face) continue;
			const float lo_a[3] = {a[0] + s[0], a[1] + s[1], a[2] + s[2]};
			const float lo_b[3] = {b[0] + s[0], b[1] + s[1], b[2] + s[2]};
			// The curtain reuses the parent's tile; its two lower corners repeat the upper
			// ones' uvs, so the parent's edge colours smear straight down it.
			const int corners[4] = {ia, ib, ib, ia};
			emit(page, a, b, lo_b, lo_a, tile, corners);
		}
	}
}

void oct_encode(const float n[3], uint8_t out[2]) {
	const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
	const float inv = l1 > 1e-8f ? 1.0f / l1 : 0.0f;
	float px = n[0] * inv;
	float pz = n[2] * inv;
	if (n[1] < 0.0f) {
		const float ox = (1.0f - std::fabs(pz)) * (px >= 0.0f ? 1.0f : -1.0f);
		const float oz = (1.0f - std::fabs(px)) * (pz >= 0.0f ? 1.0f : -1.0f);
		px = ox;
		pz = oz;
	}
	auto q = [](float v) {
		const float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
		return static_cast<uint8_t>(std::lround((c * 0.5f + 0.5f) * 255.0f));
	};
	out[0] = q(px);
	out[1] = q(pz);
}

void oct_decode(const uint8_t in[2], float out[3]) {
	const float px = static_cast<float>(in[0]) / 255.0f * 2.0f - 1.0f;
	const float pz = static_cast<float>(in[1]) / 255.0f * 2.0f - 1.0f;
	float x = px, z = pz;
	float y = 1.0f - std::fabs(px) - std::fabs(pz);
	if (y < 0.0f) {
		const float ox = (1.0f - std::fabs(z)) * (x >= 0.0f ? 1.0f : -1.0f);
		const float oz = (1.0f - std::fabs(x)) * (z >= 0.0f ? 1.0f : -1.0f);
		x = ox;
		z = oz;
	}
	const float len = std::sqrt(x * x + y * y + z * z);
	const float inv = len > 1e-8f ? 1.0f / len : 0.0f;
	out[0] = x * inv;
	out[1] = y * inv;
	out[2] = z * inv;
}

} // namespace ve
```

- [ ] **Step 5: Mirror the two helpers in GLSL**

Append to `shaders/common.glslh`:

```glsl
// Mirror of ve::oct_encode / ve::oct_decode (extension/src/lod/lod_page.cpp). The Y axis is
// the pole because terrain normals cluster there, which puts the encoding's coarsest region
// where the fewest normals live. The bake writes the two bytes; the LoD fragment shader and
// (from M6) the deferred pass read them.
vec2 oct_encode(vec3 n) {
	vec3 a = n / (abs(n.x) + abs(n.y) + abs(n.z));
	vec2 p = a.xz;
	if (a.y < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return clamp(p, vec2(-1.0), vec2(1.0)) * 0.5 + 0.5;
}

vec3 oct_decode(vec2 e) {
	vec2 p = e * 2.0 - 1.0;
	float y = 1.0 - abs(p.x) - abs(p.y);
	if (y < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
	return normalize(vec3(p.x, y, p.y));
}

// The 4x4 ordered dither the near/far cross-fade is built on (spec section 3). The composite
// drops its DEPTH where bayer4 < fade and the LoD raster discards where bayer4 >= fade, so
// the two masks are exact complements on the same full-resolution pixel grid: every pixel in
// the 120-150 m band belongs to exactly one of them.
float bayer4(ivec2 p) {
	const int M[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
	ivec2 q = p & 3;
	return (float(M[q.y * 4 + q.x]) + 0.5) / 16.0;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: PASS — every `test_lod_page.cpp` case green.

- [ ] **Step 7: Verify the GLSL still loads**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_gpu_smoke.gd`
Expected: PASS — `common.glslh` is included by every shader in the project, so a syntax error here breaks all of them. Green means the two new functions compile everywhere.

- [ ] **Step 8: Commit**

```bash
git add extension/src/lod/lod_page.h extension/src/lod/lod_page.cpp \
        extension/tests/test_lod_page.cpp shaders/common.glslh
git commit -m "feat(lod): per-quad tile pages, vertex packing, skirts and oct normals"
```

---

### Task 3: `lod/lod_residency` — the CDLOD walk with a page budget

One walk per frame produces three things at once: the cut of chunks to draw, the queue of chunks to build, and the pages to hand back. Spec §4's "coarsest level under a screen-space-error threshold" and "background priority queue keyed by screen-space error", with the page pool as the hard constraint that decides how far each level actually reaches.

**Files:**
- Create: `extension/src/lod/lod_residency.h`, `extension/src/lod/lod_residency.cpp`
- Create: `extension/tests/test_lod_residency.cpp`

**Interfaces:**
- Consumes: everything from `lod/lod_grid.h` and `lod/lod_page.h`.
- Produces:
  - `struct ve::LodProbe { virtual bool lod_chunk_has_surface(int level, IVec3) const = 0; }`
  - `struct ve::LodResidencyConfig { WorldBounds bounds; int max_pages; int max_builds_per_frame; int max_probes_per_frame; float sse_threshold; float fade_start_m; }`
  - `struct ve::LodBuildRequest { int level; IVec3 chunk; }`
  - `struct ve::LodDrawPage { int page; int level; IVec3 chunk; float lo[3]; float hi[3]; float quant_lo[3]; float quant_size; int quad_count; }`
  - `struct ve::LodFrame { std::vector<LodBuildRequest> builds; std::vector<LodDrawPage> draws; std::vector<int> released; int wanted; int resident; int pages_used; }`
  - `class ve::LodResidency` with `update`, `note_built`, `note_empty`, `note_failed`, `mark_dirty`, `pages_of`, `state_of`, `resident_count`, `pages_used`, `clear`, `config`

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_lod_residency.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_residency.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace {

// A probe that calls every chunk crossing a horizontal slab a surface chunk. Terrain-shaped
// enough that the walk behaves as it will in the demo, and completely deterministic.
struct SlabProbe : ve::LodProbe {
	float y_lo = 40.0f, y_hi = 62.0f;
	mutable int calls = 0;
	bool lod_chunk_has_surface(int level, ve::IVec3 chunk) const override {
		calls++;
		float lo[3], hi[3];
		ve::lod_chunk_aabb(level, chunk, lo, hi);
		return hi[1] > y_lo && lo[1] < y_hi;
	}
};

ve::LodResidencyConfig cfg(int pages) {
	ve::LodResidencyConfig c;
	c.bounds = ve::WorldBounds{{0, -64, 0}, {64, 8, 64}};
	c.max_pages = pages;
	c.max_builds_per_frame = 8;
	c.max_probes_per_frame = 100000; // the tests want the probe budget out of the way
	return c;
}

// Runs frames until nothing more is requested, satisfying every build with one page.
int settle(ve::LodResidency &r, const float cam[3], float ppr, const ve::LodProbe &probe,
		int max_frames = 200) {
	int frames = 0;
	for (; frames < max_frames; frames++) {
		ve::LodFrame f = r.update(cam, ppr, probe);
		if (f.builds.empty()) return frames;
		for (const ve::LodBuildRequest &b : f.builds) {
			if (!probe.lod_chunk_has_surface(b.level, b.chunk)) {
				r.note_empty(b.level, b.chunk);
				continue;
			}
			const int quads[1] = {4000};
			r.note_built(b.level, b.chunk, 1, quads);
		}
	}
	return frames;
}

} // namespace

TEST_CASE("the cut is complete and non-overlapping once everything has streamed in") {
	SlabProbe probe;
	ve::LodResidency r(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	settle(r, cam, 1756.0f, probe);
	ve::LodFrame f = r.update(cam, 1756.0f, probe);

	// No chunk is drawn together with any ancestor or descendant of itself: that is what
	// "non-overlapping" means on a nested grid, and it is what stops z-fighting.
	std::set<std::pair<int, std::tuple<int, int, int>>> drawn;
	for (const ve::LodDrawPage &d : f.draws)
		drawn.insert({d.level, {d.chunk.x, d.chunk.y, d.chunk.z}});
	for (const ve::LodDrawPage &d : f.draws) {
		ve::IVec3 c = d.chunk;
		for (int l = d.level + 1; l < ve::kLodLevels; l++) {
			c = ve::lod_parent(c);
			CHECK(drawn.count({l, {c.x, c.y, c.z}}) == 0);
		}
	}
	CHECK(!f.draws.empty());
}

TEST_CASE("a nearer chunk is finer than a farther one") {
	SlabProbe probe;
	ve::LodResidency r(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	settle(r, cam, 1756.0f, probe);
	ve::LodFrame f = r.update(cam, 1756.0f, probe);

	float near_worst = 1e9f, far_best = 0.0f;
	for (const ve::LodDrawPage &d : f.draws) {
		const float dist = ve::lod_chunk_distance(d.level, d.chunk, cam);
		if (dist < 300.0f) near_worst = std::min(near_worst, (float)d.level);
		if (dist > 1200.0f) far_best = std::max(far_best, (float)d.level);
	}
	CHECK(near_worst <= far_best);
}

TEST_CASE("chunks entirely inside the near field are never built") {
	SlabProbe probe;
	ve::LodResidency r(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	settle(r, cam, 1756.0f, probe);
	ve::LodFrame f = r.update(cam, 1756.0f, probe);
	for (const ve::LodDrawPage &d : f.draws) {
		// Every drawn chunk must reach at least to the fade start; one entirely inside it
		// would be discarded on every pixel.
		CHECK(ve::lod_chunk_far_distance(d.level, d.chunk, cam) >= ve::kLodFadeStartM);
	}
}

TEST_CASE("a small pool still produces a complete cut, just a coarser one") {
	SlabProbe probe;
	ve::LodResidency small(cfg(24));
	ve::LodResidency big(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	settle(small, cam, 1756.0f, probe);
	settle(big, cam, 1756.0f, probe);
	ve::LodFrame fs = small.update(cam, 1756.0f, probe);
	ve::LodFrame fb = big.update(cam, 1756.0f, probe);

	CHECK(!fs.draws.empty());
	CHECK(fs.pages_used <= 24);
	// "Coarser" = a higher mean level index.
	auto mean_level = [](const ve::LodFrame &f) {
		double s = 0.0;
		for (const ve::LodDrawPage &d : f.draws) s += d.level;
		return f.draws.empty() ? 0.0 : s / (double)f.draws.size();
	};
	CHECK(mean_level(fs) >= mean_level(fb));
}

TEST_CASE("the walk never descends into a half-resident sibling set") {
	SlabProbe probe;
	ve::LodResidency r(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	// One frame only: almost nothing is resident yet.
	ve::LodFrame f = r.update(cam, 1756.0f, probe);
	for (const ve::LodDrawPage &d : f.draws) {
		CHECK(r.state_of(d.level, d.chunk) == ve::LodResidency::kReady);
	}
	// Everything it could not draw, it asked for.
	CHECK(!f.builds.empty());
}

TEST_CASE("builds come out nearest-first") {
	SlabProbe probe;
	ve::LodResidency r(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	ve::LodFrame f = r.update(cam, 1756.0f, probe);
	float prev = -1.0f;
	for (const ve::LodBuildRequest &b : f.builds) {
		const float d = ve::lod_chunk_distance(b.level, b.chunk, cam);
		CHECK(d >= prev - 0.001f);
		prev = d;
	}
}

TEST_CASE("a page is never evicted while it is being drawn") {
	SlabProbe probe;
	ve::LodResidency r(cfg(40));
	float cam[3] = {800.0f, 55.0f, 800.0f};
	settle(r, cam, 1756.0f, probe);
	ve::LodFrame before = r.update(cam, 1756.0f, probe);
	std::set<int> drawn;
	for (const ve::LodDrawPage &d : before.draws) drawn.insert(d.page);

	// Walk away: new chunks are wanted, the pool is full, something must be released -- but
	// never one of the pages the SAME frame is drawing.
	cam[0] += 900.0f;
	ve::LodFrame after = r.update(cam, 1756.0f, probe);
	std::set<int> now;
	for (const ve::LodDrawPage &d : after.draws) now.insert(d.page);
	for (int p : after.released) CHECK(now.count(p) == 0);
}

TEST_CASE("marking dirty forces a rebuild and drops the cached emptiness") {
	SlabProbe probe;
	ve::LodResidency r(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	settle(r, cam, 1756.0f, probe);
	CHECK(r.update(cam, 1756.0f, probe).builds.empty());

	// An op somewhere in the air: at every level the chunks over it must come back.
	const ve::IVec3 lo{30, 0, 30}, hi{31, 1, 31};
	for (int l = 0; l < ve::kLodLevels; l++) {
		ve::IVec3 llo{ve::floor_div(lo.x, 1), ve::floor_div(lo.y, 1), ve::floor_div(lo.z, 1)};
		(void)llo;
	}
	r.mark_dirty(0, lo, hi);
	ve::LodFrame f = r.update(cam, 1756.0f, probe);
	bool asked = false;
	for (const ve::LodBuildRequest &b : f.builds)
		if (b.level == 0 && b.chunk.x >= lo.x && b.chunk.x <= hi.x) asked = true;
	CHECK(asked);
	// The probe cache for those chunks is gone: an op can put a surface where there was none.
	CHECK(r.probe_cache_size() < 100000);
}

TEST_CASE("a failed build keeps its pages and is retried") {
	SlabProbe probe;
	ve::LodResidency r(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	ve::LodFrame f = r.update(cam, 1756.0f, probe);
	REQUIRE(!f.builds.empty());
	const ve::LodBuildRequest b = f.builds.front();
	r.note_failed(b.level, b.chunk);
	CHECK(r.state_of(b.level, b.chunk) == ve::LodResidency::kNeedsBuild);
	ve::LodFrame g = r.update(cam, 1756.0f, probe);
	bool again = false;
	for (const ve::LodBuildRequest &q : g.builds)
		if (q.level == b.level && q.chunk == b.chunk) again = true;
	CHECK(again);
}

TEST_CASE("an empty chunk costs no page and is not asked for twice") {
	SlabProbe probe;
	ve::LodResidency r(cfg(512));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	settle(r, cam, 1756.0f, probe);
	const int used = r.pages_used();
	// The slab probe rejects everything above y = 62 m, so the sky chunks resolved as empty.
	CHECK(used > 0);
	CHECK(used <= 512);
	CHECK(r.update(cam, 1756.0f, probe).builds.empty());
}

TEST_CASE("clear releases every page") {
	SlabProbe probe;
	ve::LodResidency r(cfg(64));
	const float cam[3] = {800.0f, 55.0f, 800.0f};
	settle(r, cam, 1756.0f, probe);
	CHECK(r.pages_used() > 0);
	r.clear();
	CHECK(r.pages_used() == 0);
	CHECK(r.resident_count() == 0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: FAIL — `fatal error: lod/lod_residency.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `extension/src/lod/lod_residency.h`:

```cpp
#pragma once
#include "lod/lod_grid.h"
#include "lod/lod_page.h"
#include <map>
#include <vector>

namespace ve {

// How the residency asks whether a chunk is worth building. An interface for the same reason
// ve::ChunkProbe is one: the real implementation needs the generator, the edit log and the
// volume store, and the edit log's lock lives on the Godot side of the wall.
struct LodProbe {
	virtual ~LodProbe() = default;
	virtual bool lod_chunk_has_surface(int level, IVec3 chunk) const = 0;
};

struct LodResidencyConfig {
	WorldBounds bounds{};
	// Spec section 4's memory line, expressed as the only number that matters. 512 pages is
	// ~426 MB; raising it buys range linearly (see the plan's memory arithmetic).
	int max_pages = 512;
	int max_builds_per_frame = 1;
	// A probe is 125 field evaluations; a fresh world sees thousands of unknown chunks on
	// its first frame. Budgeted, nearest first, exactly as ve::ChunkResidency does.
	int max_probes_per_frame = 128;
	float sse_threshold = kLodSseThreshold;
	float fade_start_m = kLodFadeStartM;
};

struct LodBuildRequest {
	int level = 0;
	IVec3 chunk{};
};

struct LodDrawPage {
	int page = -1;
	int level = 0;
	IVec3 chunk{};
	float lo[3] = {0, 0, 0}; // world aabb, for the render thread's frustum cull
	float hi[3] = {0, 0, 0};
	float quant_lo[3] = {0, 0, 0};
	float quant_size = 0.0f;
	int quad_count = 0;
};

struct LodFrame {
	std::vector<LodBuildRequest> builds; // nearest first
	std::vector<LodDrawPage> draws;      // the complete, non-overlapping cut
	std::vector<int> released;           // pages the caller may reuse
	int wanted = 0;
	int resident = 0;
	int pages_used = 0;
};

// Spec section 4's level selection and streaming, in one walk. Descending into a node only
// when ALL its in-bounds children are ready (built or known empty) is what makes the emitted
// cut complete and non-overlapping at every instant -- see the plan's Deliberate Decisions.
class LodResidency {
public:
	enum State { kUnknown = 0, kNeedsBuild = 1, kBuilding = 2, kReady = 3, kEmpty = 4 };

	explicit LodResidency(const LodResidencyConfig &cfg);

	// One frame. `cam` is the world-space camera position; `px_per_radian` is
	// (viewport_height / 2) / tan(fov_y / 2).
	LodFrame update(const float cam[3], float px_per_radian, const LodProbe &probe);

	// A build finished. `pages` is how many pages it needs; `quads` holds that many quad
	// counts. Returns the pages assigned, or an empty vector when the pool refused (in which
	// case the chunk stays kNeedsBuild and will be asked for again).
	std::vector<int> note_built(int level, IVec3 chunk, int page_count, const int *quads);
	void note_empty(int level, IVec3 chunk);  // no geometry: caches the verdict, costs no page
	void note_failed(int level, IVec3 chunk); // retry next frame, keep any pages it had

	// Every chunk in the inclusive range at this level needs rebuilding, and every cached
	// probe verdict inside it is dropped: an op can put a surface into a chunk that had none.
	void mark_dirty(int level, IVec3 lo, IVec3 hi);

	int state_of(int level, IVec3 chunk) const;
	const std::vector<int> &pages_of(int level, IVec3 chunk) const;
	int resident_count() const { return static_cast<int>(nodes_.size()); }
	int pages_used() const { return cfg_.max_pages - static_cast<int>(free_pages_.size()); }
	int probe_cache_size() const { return static_cast<int>(probe_cache_.size()); }
	void clear();
	const LodResidencyConfig &config() const { return cfg_; }

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
	static Key key(int level, IVec3 c) { return Key{level, c.x, c.y, c.z}; }

	struct Node {
		char state = kUnknown;
		std::vector<int> pages;
		std::vector<int> quads;
		int64_t last_wanted = -1;
		float last_distance = 0.0f;
	};

	struct WalkCtx {
		const float *cam = nullptr;
		float ppr = 0.0f;
		const LodProbe *probe = nullptr;
		LodFrame *frame = nullptr;
		int probes_left = 0;
	};

	// Returns true when the subtree emitted something (or is legitimately empty).
	void walk(WalkCtx &ctx, int level, IVec3 chunk);
	bool ready_or_empty(WalkCtx &ctx, int level, IVec3 chunk);
	bool probe(WalkCtx &ctx, int level, IVec3 chunk, bool *known);
	void request(WalkCtx &ctx, int level, IVec3 chunk, float distance);
	void emit(WalkCtx &ctx, int level, IVec3 chunk);
	bool take_pages(int count, std::vector<int> *out);
	void release_node(const Key &k, LodFrame *frame);

	LodResidencyConfig cfg_;
	std::map<Key, Node> nodes_;
	std::map<Key, char> probe_cache_; // 1 = may hold a surface, 0 = known empty
	std::vector<int> free_pages_;
	int64_t frame_ = 0;
	static const std::vector<int> kNoPages;
};

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/lod/lod_residency.cpp`:

```cpp
#include "lod/lod_residency.h"
#include <algorithm>
#include <cmath>

namespace ve {

const std::vector<int> LodResidency::kNoPages;

LodResidency::LodResidency(const LodResidencyConfig &cfg) : cfg_(cfg) {
	free_pages_.reserve(cfg_.max_pages);
	for (int i = cfg_.max_pages - 1; i >= 0; i--) free_pages_.push_back(i);
}

void LodResidency::clear() {
	nodes_.clear();
	probe_cache_.clear();
	free_pages_.clear();
	for (int i = cfg_.max_pages - 1; i >= 0; i--) free_pages_.push_back(i);
	frame_ = 0;
}

int LodResidency::state_of(int level, IVec3 chunk) const {
	const auto it = nodes_.find(key(level, chunk));
	return it == nodes_.end() ? static_cast<int>(kUnknown) : static_cast<int>(it->second.state);
}

const std::vector<int> &LodResidency::pages_of(int level, IVec3 chunk) const {
	const auto it = nodes_.find(key(level, chunk));
	return it == nodes_.end() ? kNoPages : it->second.pages;
}

bool LodResidency::probe(WalkCtx &ctx, int level, IVec3 chunk, bool *known) {
	const Key k = key(level, chunk);
	const auto it = probe_cache_.find(k);
	if (it != probe_cache_.end()) {
		*known = true;
		return it->second != 0;
	}
	if (ctx.probes_left <= 0) {
		*known = false;
		return true; // unknown counts as "may have a surface": the safe direction
	}
	ctx.probes_left--;
	const bool has = ctx.probe->lod_chunk_has_surface(level, chunk);
	probe_cache_[k] = has ? 1 : 0;
	*known = true;
	return has;
}

void LodResidency::request(WalkCtx &ctx, int level, IVec3 chunk, float distance) {
	const Key k = key(level, chunk);
	Node &n = nodes_[k];
	if (n.state == kUnknown) n.state = kNeedsBuild;
	n.last_distance = distance;
	n.last_wanted = frame_;
	if (n.state != kNeedsBuild) return;
	ctx.frame->builds.push_back(LodBuildRequest{level, chunk});
}

void LodResidency::emit(WalkCtx &ctx, int level, IVec3 chunk) {
	const Key k = key(level, chunk);
	const auto it = nodes_.find(k);
	if (it == nodes_.end() || it->second.state != kReady) return;
	it->second.last_wanted = frame_;
	float lo[3], hi[3], qlo[3];
	float qsize = 0.0f;
	lod_chunk_aabb(level, chunk, lo, hi);
	lod_quant_box(level, chunk, qlo, &qsize);
	for (size_t p = 0; p < it->second.pages.size(); p++) {
		LodDrawPage d;
		d.page = it->second.pages[p];
		d.level = level;
		d.chunk = chunk;
		for (int a = 0; a < 3; a++) {
			d.lo[a] = lo[a];
			d.hi[a] = hi[a];
			d.quant_lo[a] = qlo[a];
		}
		d.quant_size = qsize;
		d.quad_count = p < it->second.quads.size() ? it->second.quads[p] : 0;
		if (d.quad_count > 0) ctx.frame->draws.push_back(d);
	}
}

bool LodResidency::ready_or_empty(WalkCtx &ctx, int level, IVec3 chunk) {
	if (!lod_chunk_in_bounds(cfg_.bounds, level, chunk)) return true;
	const int s = state_of(level, chunk);
	if (s == kReady || s == kEmpty) return true;
	// An unbuilt node whose probe already says "no surface" is empty without a build.
	bool known = false;
	if (!probe(ctx, level, chunk, &known) && known) {
		nodes_[key(level, chunk)].state = kEmpty;
		return true;
	}
	return false;
}

void LodResidency::walk(WalkCtx &ctx, int level, IVec3 chunk) {
	if (!lod_chunk_in_bounds(cfg_.bounds, level, chunk)) return;
	// Spec section 3's near field owns everything nearer than the fade: a chunk entirely
	// inside it is discarded on every pixel, so building it would burn a page for nothing.
	if (lod_chunk_far_distance(level, chunk, ctx.cam) < cfg_.fade_start_m) return;

	bool known = false;
	if (!probe(ctx, level, chunk, &known) && known) {
		nodes_[key(level, chunk)].state = kEmpty;
		return;
	}
	ctx.frame->wanted++;

	const float dist = lod_chunk_distance(level, chunk, ctx.cam);
	const float sse = lod_screen_error(level, dist, ctx.ppr);
	if (level > 0 && sse > cfg_.sse_threshold) {
		const IVec3 base = lod_child_base(chunk);
		bool all_ready = true;
		for (int i = 0; i < kLodRatio && all_ready; i++)
			for (int j = 0; j < kLodRatio && all_ready; j++)
				for (int k = 0; k < kLodRatio; k++) {
					const IVec3 c{base.x + k, base.y + j, base.z + i};
					if (!ready_or_empty(ctx, level - 1, c)) {
						all_ready = false;
						break;
					}
				}
		if (all_ready) {
			for (int i = 0; i < kLodRatio; i++)
				for (int j = 0; j < kLodRatio; j++)
					for (int k = 0; k < kLodRatio; k++)
						walk(ctx, level - 1, IVec3{base.x + k, base.y + j, base.z + i});
			return;
		}
		// Not ready: ask for the missing children, then draw THIS node meanwhile.
		for (int i = 0; i < kLodRatio; i++)
			for (int j = 0; j < kLodRatio; j++)
				for (int k = 0; k < kLodRatio; k++) {
					const IVec3 c{base.x + k, base.y + j, base.z + i};
					if (!lod_chunk_in_bounds(cfg_.bounds, level - 1, c)) continue;
					if (lod_chunk_far_distance(level - 1, c, ctx.cam) < cfg_.fade_start_m)
						continue;
					const int s = state_of(level - 1, c);
					if (s == kReady || s == kEmpty) continue;
					request(ctx, level - 1, c, lod_chunk_distance(level - 1, c, ctx.cam));
				}
	}

	if (state_of(level, chunk) == kReady) {
		emit(ctx, level, chunk);
	} else {
		request(ctx, level, chunk, dist);
	}
}

LodFrame LodResidency::update(const float cam[3], float px_per_radian, const LodProbe &probe_in) {
	frame_++;
	LodFrame frame;
	WalkCtx ctx;
	ctx.cam = cam;
	ctx.ppr = px_per_radian;
	ctx.probe = &probe_in;
	ctx.frame = &frame;
	ctx.probes_left = cfg_.max_probes_per_frame;

	IVec3 rlo{}, rhi{};
	lod_root_range(cfg_.bounds, &rlo, &rhi);
	for (int z = rlo.z; z <= rhi.z; z++)
		for (int y = rlo.y; y <= rhi.y; y++)
			for (int x = rlo.x; x <= rhi.x; x++)
				walk(ctx, kLodLevels - 1, IVec3{x, y, z});

	// Nearest first (spec section 4's priority queue; distance is monotone in screen error
	// within a level and near-monotone across them), then truncated to the frame's budget.
	std::sort(frame.builds.begin(), frame.builds.end(),
			[&](const LodBuildRequest &a, const LodBuildRequest &b) {
				return lod_chunk_distance(a.level, a.chunk, cam) <
						lod_chunk_distance(b.level, b.chunk, cam);
			});
	if (static_cast<int>(frame.builds.size()) > cfg_.max_builds_per_frame)
		frame.builds.resize(cfg_.max_builds_per_frame);
	for (const LodBuildRequest &b : frame.builds) nodes_[key(b.level, b.chunk)].state = kBuilding;

	// Evict what the pool cannot afford: never a node this frame wanted, furthest first.
	// Refusing to evict a wanted node is what keeps the cut from tearing under pressure.
	if (free_pages_.empty()) {
		std::vector<std::pair<float, Key>> victims;
		for (const auto &kv : nodes_) {
			if (kv.second.pages.empty()) continue;
			if (kv.second.last_wanted == frame_) continue;
			victims.push_back({kv.second.last_distance, kv.first});
		}
		std::sort(victims.begin(), victims.end(),
				[](const auto &a, const auto &b) { return a.first > b.first; });
		const int need = std::max(1, static_cast<int>(frame.builds.size()) * kLodMaxPagesPerChunk);
		for (const auto &v : victims) {
			if (static_cast<int>(free_pages_.size()) >= need) break;
			release_node(v.second, &frame);
		}
	}

	frame.resident = resident_count();
	frame.pages_used = pages_used();
	return frame;
}

void LodResidency::release_node(const Key &k, LodFrame *frame) {
	const auto it = nodes_.find(k);
	if (it == nodes_.end()) return;
	for (int p : it->second.pages) {
		free_pages_.push_back(p);
		if (frame) frame->released.push_back(p);
	}
	nodes_.erase(it);
}

bool LodResidency::take_pages(int count, std::vector<int> *out) {
	out->clear();
	if (count <= 0 || static_cast<int>(free_pages_.size()) < count) return false;
	for (int i = 0; i < count; i++) {
		out->push_back(free_pages_.back());
		free_pages_.pop_back();
	}
	return true;
}

std::vector<int> LodResidency::note_built(int level, IVec3 chunk, int page_count,
		const int *quads) {
	const Key k = key(level, chunk);
	Node &n = nodes_[k];
	// Reuse what it already holds; take or hand back the difference.
	while (static_cast<int>(n.pages.size()) > page_count) {
		free_pages_.push_back(n.pages.back());
		n.pages.pop_back();
	}
	std::vector<int> extra;
	if (static_cast<int>(n.pages.size()) < page_count) {
		if (!take_pages(page_count - static_cast<int>(n.pages.size()), &extra)) {
			// Fail-soft (spec section 8): the pool is full of chunks this frame is drawing.
			// Stay kNeedsBuild and ask again once the camera has moved something out.
			n.state = kNeedsBuild;
			return {};
		}
		n.pages.insert(n.pages.end(), extra.begin(), extra.end());
	}
	n.quads.assign(quads, quads + page_count);
	n.state = kReady;
	n.last_wanted = frame_;
	return n.pages;
}

void LodResidency::note_empty(int level, IVec3 chunk) {
	const Key k = key(level, chunk);
	Node &n = nodes_[k];
	for (int p : n.pages) free_pages_.push_back(p);
	n.pages.clear();
	n.quads.clear();
	n.state = kEmpty;
	probe_cache_[k] = 0;
}

void LodResidency::note_failed(int level, IVec3 chunk) {
	const auto it = nodes_.find(key(level, chunk));
	if (it == nodes_.end()) return;
	// Keep whatever pages it had: a stale far chunk beats a hole in the horizon.
	it->second.state = it->second.pages.empty() ? kNeedsBuild : kReady;
	if (it->second.pages.empty()) it->second.state = kNeedsBuild;
}

void LodResidency::mark_dirty(int level, IVec3 lo, IVec3 hi) {
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const Key k = key(level, IVec3{x, y, z});
				probe_cache_.erase(k);
				const auto it = nodes_.find(k);
				if (it == nodes_.end()) continue;
				// Keep the pages: they hold the pre-edit chunk, which is what is drawn until
				// the rebuild lands. kBuilding stays kBuilding -- the in-flight result is
				// stale, and note_built will be followed by another dirty pass anyway.
				if (it->second.state != kBuilding) it->second.state = kNeedsBuild;
			}
}

} // namespace ve
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: PASS — all `test_lod_residency.cpp` cases green.

- [ ] **Step 6: Commit**

```bash
git add extension/src/lod/lod_residency.h extension/src/lod/lod_residency.cpp \
        extension/tests/test_lod_residency.cpp
git commit -m "feat(lod): CDLOD walk, page budget and screen-error build queue"
```

---

### Task 4: one mesher, two pitches — generalise `mesh_*` to origin + cell size

The collision mesher becomes the LoD mesher. Two changes, both behaviour-preserving for M3's collision path: the three shaders take the lattice's world origin and cell size from the push constant instead of deriving them from a chunk coordinate, and `mesh_quads` reserves both of a quad's triangles with **one** atomic and writes the quad's four corners, so the CPU can recover quads without guessing. Every M3 and M4 test must still pass, byte for byte, at the end of this task.

**Files:**
- Modify: `shaders/mesh_common.glslh`, `shaders/mesh_field.comp.glsl`, `shaders/mesh_cells.comp.glsl`, `shaders/mesh_quads.comp.glsl`
- Modify: `extension/src/render/mesh_pass.h`, `extension/src/render/mesh_pass.cpp`
- Modify: `extension/src/mesh/dual_contour.h`, `extension/src/mesh/dual_contour.cpp`
- Modify: `extension/tests/test_dual_contour.cpp`
- Create: `tests/test_lod_mesh.gd`

**Interfaces:**
- Consumes: `ve::DcGrid`, `ve::MeshBuffer` (`mesh/dual_contour.h`); `ve::LodQuad` (`lod/lod_page.h`); `ve::lod_chunk_origin`, `ve::lod_cell_size` (`lod/lod_grid.h`).
- Produces:
  - `ve::MeshBuffer::quads` — `std::vector<LodQuad>`, parallel to `indices` (two triangles per quad, in order)
  - `godot::MeshJob` gains `float origin[3]`, `float cell_size`, and `bool want_quads`
  - `godot::MeshResult` gains `std::vector<ve::LodQuad> quads`
  - `MeshPassConfig::max_tris` becomes `MeshPassConfig::max_quads` (16384)

- [ ] **Step 1: Write the failing test — the CPU mesher must emit quads**

Add to `extension/tests/test_dual_contour.cpp`:

```cpp
// M5 needs the quads, not just the triangles: a LoD page gives each quad its own bake tile,
// so the four corners have to survive the mesher in a known order. The triangles stay
// exactly what they were -- they are what Jolt is fed -- and are now derived from the quads.
TEST_CASE("dual_contour emits quads whose two triangles are its own") {
	const ve::DcGrid g = small_grid();
	const std::vector<uint8_t> lat = make_lattice(g, [](float, float y, float) {
		return y - 0.75f;
	});
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);
	REQUIRE(m.triangle_count() > 0);
	CHECK(static_cast<int>(m.quads.size()) * 2 == m.triangle_count());
	for (size_t q = 0; q < m.quads.size(); q++) {
		const ve::LodQuad &qd = m.quads[q];
		CHECK(m.indices[q * 6 + 0] == qd.v[0]);
		CHECK(m.indices[q * 6 + 1] == qd.v[1]);
		CHECK(m.indices[q * 6 + 2] == qd.v[2]);
		CHECK(m.indices[q * 6 + 3] == qd.v[0]);
		CHECK(m.indices[q * 6 + 4] == qd.v[2]);
		CHECK(m.indices[q * 6 + 5] == qd.v[3]);
	}
}

// The whole point of the generalisation: the same mesher at a LoD pitch produces a surface
// in the same place, only coarser. A plane at y = 3 m meshed at 0.4 m must land within half
// a cell of y = 3 m everywhere.
TEST_CASE("the mesher is correct at a LoD pitch, not just the collision one") {
	ve::DcGrid g;
	g.lattice = 18;
	g.cell_size = 0.4f;
	g.origin[0] = 25.6f; g.origin[1] = 0.0f; g.origin[2] = 0.0f;
	const std::vector<uint8_t> lat = make_lattice(g, [](float, float y, float) {
		return y - 3.0f;
	});
	ve::MeshBuffer m;
	ve::dual_contour(lat.data(), g, &m);
	REQUIRE(m.vertex_count() > 0);
	for (int v = 0; v < m.vertex_count(); v++) {
		CHECK(std::fabs(m.positions[v * 3 + 1] - 3.0f) < 0.5f * g.cell_size);
		CHECK(m.positions[v * 3 + 0] >= g.origin[0] - g.cell_size - 1e-4f);
	}
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: FAIL — `'struct ve::MeshBuffer' has no member named 'quads'`.

- [ ] **Step 3: Give the CPU mesher a quad list**

Modify `extension/src/mesh/dual_contour.h` — add the include and the member:

```cpp
#include "lod/lod_page.h" // ve::LodQuad
```

and inside `struct MeshBuffer`, after `cell_vertex`:

```cpp
	// Parallel to `indices`: quad q is triangles 2q and 2q+1, wound (v0,v1,v2)(v0,v2,v3).
	// M5's bakery gives each quad its own texture tile, so the four corners must survive the
	// mesher as a unit; the triangle list is derived from this and is unchanged.
	std::vector<LodQuad> quads;
```

In `extension/src/mesh/dual_contour.cpp`, wherever the two triangles of a quad are pushed, push the quad first and derive the triangles from it. Replace the emit block (the one that appends six indices per sign-changing edge) with:

```cpp
			// One quad, already wound: the flipped case reverses the corner order rather
			// than reordering the triangles, so (v0,v1,v2) and (v0,v2,v3) are always right.
			// This is the exact convention shaders/mesh_quads.comp.glsl writes.
			LodQuad quad{};
			if (solid_a) {
				quad.v[0] = static_cast<uint32_t>(q[0]);
				quad.v[1] = static_cast<uint32_t>(q[1]);
				quad.v[2] = static_cast<uint32_t>(q[2]);
				quad.v[3] = static_cast<uint32_t>(q[3]);
			} else {
				quad.v[0] = static_cast<uint32_t>(q[0]);
				quad.v[1] = static_cast<uint32_t>(q[3]);
				quad.v[2] = static_cast<uint32_t>(q[2]);
				quad.v[3] = static_cast<uint32_t>(q[1]);
			}
			out->quads.push_back(quad);
			out->indices.push_back(quad.v[0]);
			out->indices.push_back(quad.v[1]);
			out->indices.push_back(quad.v[2]);
			out->indices.push_back(quad.v[0]);
			out->indices.push_back(quad.v[2]);
			out->indices.push_back(quad.v[3]);
```

and clear `out->quads` alongside the other outputs at the top of `dual_contour`.

- [ ] **Step 4: Run the CPU test to verify it passes**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: PASS — the new cases green **and** every pre-existing `test_dual_contour` winding case unchanged (the triangle stream is byte-identical to what it was).

- [ ] **Step 5: Commit the CPU half**

```bash
git add extension/src/mesh/dual_contour.h extension/src/mesh/dual_contour.cpp \
        extension/tests/test_dual_contour.cpp
git commit -m "refactor(mesh): dual_contour emits quads; triangles derive from them"
```

- [ ] **Step 6: Generalise the shared GLSL header**

Replace `shaders/mesh_common.glslh` entirely:

```glsl
// Chunk lattice addressing, shared by the three meshing passes so they can never disagree.
// Mirror of extension/src/mesh/mesh_chunk.h and ve::dual_contour's conventions: lattice
// array index i holds the sample at local coordinate i - 1, mesh-cell array index m holds
// the cell at local coordinate m - 1, and cell m's corners are lattice m and m + 1. The
// one-cell overlap below the origin is what lets a chunk close the quads on its minimum
// faces without reading a neighbouring chunk's lattice. Include common.glslh first.
//
// The cell COUNTS are the same at every pitch this codebase meshes at: a 6.4 m collision
// chunk at 0.1 m and every one of spec section 4's four LoD levels are all 64 cells across.
// Only the origin and the spacing differ, and both arrive in the push constant -- which is
// what makes one mesher serve both (see the M5 plan's Deliberate Decisions).
const int CHUNK_CELLS = 64;         // ve::kChunkCells == ve::kLodChunkCells
const int CHUNK_MESH_CELLS = 65;    // ve::kChunkMeshCells
const int CHUNK_LATTICE = 66;       // ve::kChunkLattice

layout(push_constant, std430) uniform Push {
	vec4 origin;  // xyz = the lattice's world origin, w = cell size
	ivec4 params; // x = op count, y = max verts per job, z = max quads per job, w = job index
} pc;

vec3 lattice_world_pos(ivec3 l) {
	return pc.origin.xyz + (vec3(l) - 1.0) * pc.origin.w;
}

int mesh_cell_index(ivec3 m) {
	return m.x + m.y * CHUNK_MESH_CELLS + m.z * CHUNK_MESH_CELLS * CHUNK_MESH_CELLS;
}
```

- [ ] **Step 7: Update the three shaders**

In `shaders/mesh_field.comp.glsl`, delete its own `layout(push_constant...)` block (it now comes from `mesh_common.glslh`) and rewrite `main`:

```glsl
void main() {
	ivec3 l = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(l, ivec3(CHUNK_LATTICE)))) return;
	float sdf;
	uint mat; // the mesher has no use for materials; collision carries none
	eval_field(lattice_world_pos(l), uint(pc.params.w) * MAX_REGION_OPS,
			uint(pc.params.x), sdf, mat);
	imageStore(lattice, l, vec4(quantise_sdf(sdf)));
}
```

In `shaders/mesh_cells.comp.glsl`, delete its push-constant block and change the two places that used `pc.chunk`:

```glsl
	uint job = uint(pc.params.w);
```

and the vertex position:

```glsl
	vec3 p = pc.origin.xyz + (vec3(m) - 1.0 + acc / float(n)) * pc.origin.w;
```

In `shaders/mesh_quads.comp.glsl`, delete its push-constant block, add the quad output buffer, and replace `emit` and the emit calls:

```glsl
layout(set = 0, binding = 2, std430) writeonly buffer Quads { uvec4 v[]; } quads;

// One atomic per QUAD, not per triangle: the two triangles of a quad have to be adjacent in
// the output for the CPU to pair them, and separate atomics let another thread interleave.
// The quad's four corners go out too, already wound, so the CPU never has to infer the
// winding case (M5 gives each quad its own bake tile and needs the corners in order).
void emit_quad(uint job, int a, int b, int c, int d) {
	uint q = atomicAdd(counts.v[job * 4u + 1u], 1u);
	if (q >= uint(pc.params.z)) { atomicOr(counts.v[job * 4u + 2u], 2u); return; }
	quads.v[job * uint(pc.params.z) + q] = uvec4(uint(a), uint(b), uint(c), uint(d));
}
```

and the winding branch at the bottom of `main`:

```glsl
		// (axis, b, c) is a right-handed cycle, so q0..q3 wind counter-clockwise seen from
		// +axis. A solid -> air step along +axis puts the air on the +axis side, which is
		// the side the normal must face; the flipped case reverses the corner ORDER so that
		// (v0,v1,v2) and (v0,v2,v3) stay the two correctly wound triangles either way.
		if (sa) emit_quad(job, q[0], q[1], q[2], q[3]);
		else    emit_quad(job, q[0], q[3], q[2], q[1]);
```

Also change `main`'s guard to use `pc.params.w` for the job index (`uint job = uint(pc.params.w);`) and delete the now-unused `Tris` binding.

- [ ] **Step 8: Update `MeshPass`**

In `extension/src/render/mesh_pass.h`:

```cpp
struct MeshPassConfig {
	int max_jobs = 2;       // chunks per batch
	int max_verts = 16384;  // a fully covered 6.4 m collision chunk holds ~4 100
	// Quads, not triangles: 16 384 is both the old 32 768-triangle cap and exactly
	// ve::kLodTilesPerPage * ve::kLodMaxPagesPerChunk, so a LoD job can fill two pages.
	int max_quads = 16384;
};

struct MeshJob {
	ve::IVec3 chunk{};               // identity only; the lattice comes from origin/cell_size
	float origin[3] = {0, 0, 0};     // the lattice's world origin
	float cell_size = ve::kChunkCellSize;
	const ve::EditOp *ops = nullptr; // copied at submit
	int op_count = 0;
};

struct MeshResult {
	ve::IVec3 chunk{};
	std::vector<float> positions;    // 3 per vertex, world space
	std::vector<ve::LodQuad> quads;  // 4 corner indices, wound
	std::vector<uint32_t> indices;   // 3 per triangle, derived from quads
	bool overflow = false;
	bool failed = false;
};
```

Rename the `tris_` RID to `quads_`, and add a helper that fills `origin`/`cell_size` for the collision path so every existing caller keeps working:

```cpp
// A collision chunk's job: the M3 lattice, expressed in the generalised form.
MeshJob collision_mesh_job(ve::IVec3 chunk, const ve::EditOp *ops, int op_count);
```

In `extension/src/render/mesh_pass.cpp`:

```cpp
MeshJob godot::collision_mesh_job(ve::IVec3 chunk, const ve::EditOp *ops, int op_count) {
	MeshJob j;
	j.chunk = chunk;
	ve::chunk_world_origin(chunk, j.origin);
	j.cell_size = ve::kChunkCellSize;
	j.ops = ops;
	j.op_count = op_count;
	return j;
}
```

Replace `MeshPass::push` with the generalised push constant:

```cpp
void MeshPass::push(int64_t list, const MeshJob &job, int job_index) {
	PackedByteArray pc;
	pc.resize(32);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	int32_t *i = reinterpret_cast<int32_t *>(pc.ptrw());
	f[0] = job.origin[0];
	f[1] = job.origin[1];
	f[2] = job.origin[2];
	f[3] = job.cell_size;
	i[4] = job.op_count;
	i[5] = cfg_.max_verts;
	i[6] = cfg_.max_quads;
	i[7] = job_index;
	rd_->compute_list_set_push_constant(list, pc, pc.size());
}
```

Size the quad buffer `cfg_.max_jobs * cfg_.max_quads * 16` bytes, and rewrite the index derivation in `read_job`:

```cpp
	// Quads come back as uvec4; the triangle stream Jolt eats is derived here, in exactly
	// the order ve::dual_contour produces it, so the differential test still compares like
	// with like.
	out->quads.resize(quad_count);
	out->indices.clear();
	out->indices.reserve(static_cast<size_t>(quad_count) * 6);
	for (int q = 0; q < quad_count; q++) {
		const uint32_t *v = raw + q * 4;
		out->quads[q] = ve::LodQuad{{v[0], v[1], v[2], v[3]}};
		out->indices.push_back(v[0]);
		out->indices.push_back(v[1]);
		out->indices.push_back(v[2]);
		out->indices.push_back(v[0]);
		out->indices.push_back(v[2]);
		out->indices.push_back(v[3]);
	}
```

Update every `cfg_.max_tris` reference to `cfg_.max_quads`, and every construction of a `MeshJob` in `mesh_service.cpp` / `voxel_world.cpp` to go through `collision_mesh_job`.

- [ ] **Step 9: Verify the collision path is unchanged**

Run:
```bash
./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot \
  -a res://tests/test_mesh_lattice.gd -a res://tests/test_mesh_diff.gd \
  -a res://tests/test_mesh_stream.gd -a res://tests/test_collider_stream.gd \
  -a res://tests/test_collider_edits.gd
```
Expected: PASS, all five suites. This is the acceptance gate for the refactor: M3's mesher must behave identically after being generalised.

- [ ] **Step 10: Write the LoD-pitch differential test**

Create `tests/test_lod_mesh.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# The generalised mesher, driven at a LoD pitch instead of the collision one. Same shaders,
# same ve::dual_contour reference, same tolerances as tests/test_mesh_diff.gd -- the property
# under test is that changing the origin and the spacing changes nothing else.

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
	assert_int(d["lattice_max_diff"]).override_failure_message(
		"%s: lattice differs by %d encoded steps" % [label, d["lattice_max_diff"]]).is_less_equal(1)
	assert_int(d["cells_only_cpu"]).override_failure_message(
		"%s: %d cells hold a vertex on the CPU only" % [label, d["cells_only_cpu"]]).is_equal(0)
	assert_int(d["cells_only_gpu"]).override_failure_message(
		"%s: %d cells hold a vertex on the GPU only" % [label, d["cells_only_gpu"]]).is_equal(0)
	assert_int(d["quads_only_cpu"]).is_equal(0)
	assert_int(d["quads_only_gpu"]).is_equal(0)
	assert_float(d["max_position_diff"]).is_less_equal(0.001)

func test_gpu_and_cpu_agree_at_every_lod_pitch() -> void:
	var w := make_world()
	# One chunk over the terrain at each level. The surface sits at y = 51.2 + hills.
	for level in range(4):
		var d: Dictionary = w.debug_lod_mesh_diff(level, Vector3i(0, 0, 0))
		check_diff(d, "level %d" % level)
		assert_int(d["quad_count"]).override_failure_message(
			"level %d meshed nothing at all" % level).is_greater(0)

func test_a_lod_chunk_covers_its_own_box_and_one_cell_below_it() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_lod_mesh_diff(0, Vector3i(1, 2, 1))
	assert_int(d["quad_count"]).is_greater(0)
	# Every vertex lies inside [origin - cell, origin + 64*cell] on every axis: the mesh box.
	assert_bool(d["inside_mesh_box"]).is_true()
```

Add `VoxelWorld::debug_lod_mesh_diff(int level, Vector3i chunk)` in Task 6's hook block (it needs the LoD job path); until then this test is red — that is expected and is what Task 6 closes.

- [ ] **Step 11: Commit**

```bash
git add shaders/mesh_common.glslh shaders/mesh_field.comp.glsl shaders/mesh_cells.comp.glsl \
        shaders/mesh_quads.comp.glsl extension/src/render/mesh_pass.h \
        extension/src/render/mesh_pass.cpp extension/src/render/mesh_service.cpp \
        extension/src/voxel_world.cpp tests/test_lod_mesh.gd
git commit -m "refactor(mesh): lattice origin and pitch from the push constant; quad output"
```

---

### Task 5: the bakery — one thread per texel, refined onto the analytic surface

Spec §4's "bake albedo + normal by raymarching the higher-fidelity representation (G+edits at ~4× the chunk's mesh sampling) along each triangle's dominant axis". A quad's 4×4 tile is baked by walking its bilinear interior, taking one Newton step onto the true surface, and writing the analytic gradient, the material found there, and a one-tap ambient estimate. The CPU mirror of exactly that lands in `lod/lod_bake.cpp`, so spec §8's differential test has something to diff against.

**Files:**
- Create: `shaders/lod_bake.comp.glsl`
- Create: `extension/src/lod/lod_bake.h`, `extension/src/lod/lod_bake.cpp`
- Create: `extension/src/render/lod_bake_pass.h`, `extension/src/render/lod_bake_pass.cpp`
- Create: `extension/tests/test_lod_bake.cpp`
- Create: `tests/test_lod_bake.gd`

**Interfaces:**
- Consumes: `ve::eval_field` (`world/brick_eval.h`); `ve::Generator`, `ve::EditOp`, `ve::VolumeStore`; `ve::oct_encode`, `ve::kLodTileTexels`, `ve::kLodTilesPerPage`, `ve::kLodPageTexW/H` (`lod/lod_page.h`); `godot::VolumePool` (`render/volume_pool.h`).
- Produces:
  - `struct ve::LodBakeTexel { uint8_t rgba[4]; }`
  - `ve::LodBakeTexel ve::bake_lod_texel(const Generator &, const EditOp *, int, const VolumeStore *, const float corners[12], int i, int j, float cell_size)`
  - `void ve::lod_bilinear_corner(const float corners[12], float s, float t, float out[3])`
  - `int ve::lod_bake_texel_index(int quad, int i, int j)` — page-local texel index
  - `class godot::LodBakePass` with `initialize(RenderingDevice *, const VolumePool *)`, `teardown()`, `is_valid()`, `record(int64_t list, RID verts, RID quads, RID counts, RID ops, RID out, int job_index, const MeshJob &)`, `output_bytes_per_job()`

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_lod_bake.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_bake.h"
#include "lod/lod_page.h"
#include "generator/generator.h"
#include <cmath>

namespace {

// A quad lying flat at y = kSurfaceY on the demo terrain's plateau, one L1 cell across.
void flat_quad(float c[12], float x, float z, float y, float cell) {
	const float p[4][3] = {{x, y, z}, {x + cell, y, z}, {x + cell, y, z + cell}, {x, y, z + cell}};
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++) c[k * 3 + a] = p[k][a];
}

} // namespace

TEST_CASE("bilinear corner interpolation puts texel 0 on corner 0 and texel 3 opposite it") {
	float c[12];
	flat_quad(c, 10.0f, 20.0f, 5.0f, 0.4f);
	float p[3];
	ve::lod_bilinear_corner(c, 0.0f, 0.0f, p);
	CHECK(p[0] == doctest::Approx(c[0]));
	CHECK(p[2] == doctest::Approx(c[2]));
	ve::lod_bilinear_corner(c, 1.0f, 1.0f, p);
	CHECK(p[0] == doctest::Approx(c[6]));
	CHECK(p[2] == doctest::Approx(c[8]));
	ve::lod_bilinear_corner(c, 1.0f, 0.0f, p);
	CHECK(p[0] == doctest::Approx(c[3]));
	ve::lod_bilinear_corner(c, 0.0f, 1.0f, p);
	CHECK(p[2] == doctest::Approx(c[11]));
	// The tile's texel (i, j) is parameter (i/3, j/3): texel 0 IS the corner.
	CHECK(ve::kLodTileTexels == 4);
}

TEST_CASE("a baked texel carries the analytic normal, not the quad's") {
	ve::AnalyticGenerator gen;
	// A quad deliberately offset from the true surface by a quarter cell and tilted flat:
	// the bake must still report the terrain's real normal, which is what the mesh cannot.
	const float cell = 0.4f;
	const float x = 12.0f, z = 34.0f;
	// Find the surface height under (x, z) by bisection on the analytic field.
	float ylo = 0.0f, yhi = 120.0f;
	for (int k = 0; k < 60; k++) {
		const float ym = 0.5f * (ylo + yhi);
		if (ve::eval_field(gen, nullptr, 0, x, ym, z).sdf <= 0.0f) ylo = ym; else yhi = ym;
	}
	float c[12];
	flat_quad(c, x, z, 0.5f * (ylo + yhi) + 0.1f * cell, cell);

	const ve::LodBakeTexel t = ve::bake_lod_texel(gen, nullptr, 0, nullptr, c, 1, 2, cell);
	uint8_t oct[2] = {t.rgba[0], t.rgba[1]};
	float n[3];
	ve::oct_decode(oct, n);
	CHECK(std::fabs(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] - 1.0f) < 0.01f);
	CHECK(n[1] > 0.5f); // the demo terrain faces up
	// The material is the one the field reports at the refined point, never 0 (air).
	CHECK(t.rgba[2] != 0);
	// The ambient byte is a fraction, so it must be in range and not saturated black.
	CHECK(t.rgba[3] > 0);
}

TEST_CASE("every texel of a tile bakes a distinct position, which is what 4x4 buys") {
	ve::AnalyticGenerator gen;
	const float cell = 0.4f;
	float c[12];
	flat_quad(c, 12.0f, 34.0f, ve::kSurfaceY, cell);
	// Corner texels agree with their corners; interior texels do not, or the tile would be
	// carrying nothing the vertices did not already carry (the 2x2 degeneracy).
	float p00[3], p11[3], p22[3];
	ve::lod_bilinear_corner(c, 0.0f, 0.0f, p00);
	ve::lod_bilinear_corner(c, 1.0f / 3.0f, 1.0f / 3.0f, p11);
	ve::lod_bilinear_corner(c, 2.0f / 3.0f, 2.0f / 3.0f, p22);
	CHECK(std::fabs(p11[0] - p00[0]) > 1e-4f);
	CHECK(std::fabs(p22[0] - p11[0]) > 1e-4f);
}

TEST_CASE("texel indices tile the page without collisions") {
	// Two different quads never write the same texel, and one quad's 16 texels are distinct.
	int seen[32];
	int n = 0;
	for (int q : {0, 1, ve::kLodTilesX, ve::kLodTilesPerPage - 1}) {
		for (int j = 0; j < ve::kLodTileTexels; j++)
			for (int i = 0; i < ve::kLodTileTexels; i++) {
				const int idx = ve::lod_bake_texel_index(q, i, j);
				CHECK(idx >= 0);
				CHECK(idx < ve::kLodPageTexels);
				if (n < 32) seen[n++] = idx;
			}
	}
	for (int a = 0; a < n; a++)
		for (int b = a + 1; b < n; b++) CHECK(seen[a] != seen[b]);

	// Texel (i, j) of quad q must land in q's own tile.
	int tx = 0, ty = 0;
	ve::lod_tile_origin(7, &tx, &ty);
	CHECK(ve::lod_bake_texel_index(7, 0, 0) == ty * ve::kLodPageTexW + tx);
	CHECK(ve::lod_bake_texel_index(7, 3, 3) ==
			(ty + 3) * ve::kLodPageTexW + tx + 3);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: FAIL — `fatal error: lod/lod_bake.h: No such file or directory`.

- [ ] **Step 3: Write the CPU reference header**

Create `extension/src/lod/lod_bake.h`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "lod/lod_page.h"
#include <cstdint>

namespace ve {

// One baked texel: RG = octahedral normal, B = material id, A = ambient. The channel layout
// is mirrored byte for byte in shaders/lod_bake.comp.glsl and read by shaders/lod.frag.glsl;
// M6's deferred pass consumes the same four bytes, which is why the material is an ID rather
// than a colour.
struct LodBakeTexel {
	uint8_t rgba[4] = {0, 0, 0, 0};
};

// Bilinear across the quad's four corners in the plan's corner order:
// (0,0) = c0, (1,0) = c1, (1,1) = c2, (0,1) = c3.
void lod_bilinear_corner(const float corners[12], float s, float t, float out[3]);

// Page-local texel index of texel (i, j) of quad `quad` (whose tile is quad % kLodTilesPerPage).
int lod_bake_texel_index(int quad, int i, int j);

// Bake one texel. Nine field evaluations, and every one of them is mirrored in the shader:
//   1  the value at the bilinear point, which drives the Newton step
//   6  central differences for the gradient -> the analytic normal
//   1  the value (and material) at the refined point
//   1  one ambient tap two cells along the normal
// Refining first and shading second is what makes the tile carry detail the mesh does not:
// the mesh vertex is on the coarse surface, the texel is on the real one.
LodBakeTexel bake_lod_texel(const Generator &gen, const EditOp *ops, int op_count,
		const VolumeStore *volumes, const float corners[12], int i, int j, float cell_size);

} // namespace ve
```

- [ ] **Step 4: Write the CPU reference**

Create `extension/src/lod/lod_bake.cpp`:

```cpp
#include "lod/lod_bake.h"
#include "world/brick_eval.h"
#include <algorithm>
#include <cmath>

namespace ve {

void lod_bilinear_corner(const float corners[12], float s, float t, float out[3]) {
	for (int a = 0; a < 3; a++) {
		const float bottom = corners[0 * 3 + a] + (corners[1 * 3 + a] - corners[0 * 3 + a]) * s;
		const float top = corners[3 * 3 + a] + (corners[2 * 3 + a] - corners[3 * 3 + a]) * s;
		out[a] = bottom + (top - bottom) * t;
	}
}

int lod_bake_texel_index(int quad, int i, int j) {
	int tx = 0, ty = 0;
	lod_tile_origin(quad % kLodTilesPerPage, &tx, &ty);
	return (ty + j) * kLodPageTexW + (tx + i);
}

LodBakeTexel bake_lod_texel(const Generator &gen, const EditOp *ops, int op_count,
		const VolumeStore *volumes, const float corners[12], int i, int j, float cell_size) {
	LodBakeTexel out;
	const float denom = static_cast<float>(kLodTileTexels - 1);
	float p[3];
	lod_bilinear_corner(corners, static_cast<float>(i) / denom, static_cast<float>(j) / denom, p);

	const float e = 0.25f * cell_size;
	auto f = [&](float x, float y, float z) {
		return eval_field(gen, ops, op_count, x, y, z, volumes).sdf;
	};

	const float d0 = f(p[0], p[1], p[2]);
	float g[3] = {f(p[0] + e, p[1], p[2]) - f(p[0] - e, p[1], p[2]),
			f(p[0], p[1] + e, p[2]) - f(p[0], p[1] - e, p[2]),
			f(p[0], p[1], p[2] + e) - f(p[0], p[1], p[2] - e)};
	const float glen = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
	if (glen < 1e-8f) {
		g[0] = 0.0f; g[1] = 1.0f; g[2] = 0.0f;
	} else {
		for (int a = 0; a < 3; a++) g[a] /= glen;
	}

	// One Newton step onto the analytic surface. The field is a near-SDF with a bounded
	// Lipschitz constant, so a single step from within half a cell lands within the
	// quantisation of the bake itself; more steps buy nothing a texel can show.
	float q[3];
	const float step = std::max(std::min(d0, 2.0f * cell_size), -2.0f * cell_size);
	for (int a = 0; a < 3; a++) q[a] = p[a] - step * g[a];

	const Sample s = eval_field(gen, ops, op_count, q[0], q[1], q[2], volumes);
	uint16_t mat = s.material;
	if (mat == 0) {
		// The refined point landed just outside the solid: step a hair further in rather
		// than writing air, which would read as magenta (common.glslh's error colour).
		const Sample inside = eval_field(gen, ops, op_count, q[0] - 0.25f * cell_size * g[0],
				q[1] - 0.25f * cell_size * g[1], q[2] - 0.25f * cell_size * g[2], volumes);
		mat = inside.material;
	}

	// One ambient tap: how much room there is above the surface. Free geometry darkens
	// creases and cave mouths, which is the far field's cheapest read of shape, and M6's
	// SSGI will multiply into the same byte rather than replace it.
	const float r = 2.0f * cell_size;
	const float open = f(q[0] + g[0] * r, q[1] + g[1] * r, q[2] + g[2] * r);
	const float ao = std::clamp(open / r, 0.0f, 1.0f);

	uint8_t oct[2];
	oct_encode(g, oct);
	out.rgba[0] = oct[0];
	out.rgba[1] = oct[1];
	out.rgba[2] = static_cast<uint8_t>(mat > 255 ? 255 : mat);
	out.rgba[3] = static_cast<uint8_t>(std::lround(std::max(ao, 0.15f) * 255.0f));
	return out;
}

} // namespace ve
```

- [ ] **Step 5: Run the native test to verify it passes**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: PASS — every `test_lod_bake.cpp` case green.

- [ ] **Step 6: Write the shader**

Create `shaders/lod_bake.comp.glsl`:

```glsl
#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 4
#define FIELD_VOLUME_SDF_BINDING 5
#define FIELD_VOLUME_MAT_BINDING 6
#include "common.glslh"
#include "field.glslh"

// One thread per bake texel: 16 per quad. Mirror of ve::bake_lod_texel -- same nine field
// evaluations, same order, same channel layout, so the differential test in
// tests/test_lod_bake.gd can hold both to one encoded step.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) readonly buffer Verts { float v[]; } verts;
layout(set = 0, binding = 1, std430) readonly buffer Quads { uvec4 v[]; } quads;
layout(set = 0, binding = 2, std430) readonly buffer Counts { uint v[]; } counts;
// RGBA8 packed into one uint per texel: two pages of 512x256 per job.
layout(set = 0, binding = 3, std430) writeonly buffer Bake { uint v[]; } bake;
// bindings 4-6 are the field op pool and the volume pool, declared by field.glslh

const int TILE_TEXELS = 4;      // ve::kLodTileTexels
const int TILES_X = 128;        // ve::kLodTilesX
const int TILES_PER_PAGE = 8192;// ve::kLodTilesPerPage
const int PAGE_W = 512;         // ve::kLodPageTexW
const int PAGE_TEXELS = 131072; // ve::kLodPageTexels

layout(push_constant, std430) uniform Push {
	vec4 origin;  // xyz unused here, w = cell size
	ivec4 params; // x = op count, y = max verts, z = max quads, w = job index
} pc;

vec3 vert_at(uint job, uint idx) {
	uint base = (job * uint(pc.params.y) + idx) * 3u;
	return vec3(verts.v[base + 0u], verts.v[base + 1u], verts.v[base + 2u]);
}

// Mirror of ve::lod_bilinear_corner: (0,0)=c0, (1,0)=c1, (1,1)=c2, (0,1)=c3.
vec3 bilinear_corner(vec3 c0, vec3 c1, vec3 c2, vec3 c3, float s, float t) {
	return mix(mix(c0, c1, s), mix(c3, c2, s), t);
}

void main() {
	uint job = uint(pc.params.w);
	uint thread = gl_GlobalInvocationID.x;
	uint quad = thread >> 4u;          // 16 texels per quad
	uint texel = thread & 15u;
	uint quad_count = min(counts.v[job * 4u + 1u], uint(pc.params.z));
	// Only TILED quads are baked: a page's tiles are its first kLodTilesPerPage quads, and
	// skirts (added on the CPU) share their parent's tile rather than owning one.
	if (quad >= quad_count) return;
	if (quad >= uint(TILES_PER_PAGE * 2)) return;

	uvec4 q = quads.v[job * uint(pc.params.z) + quad];
	vec3 c0 = vert_at(job, q.x);
	vec3 c1 = vert_at(job, q.y);
	vec3 c2 = vert_at(job, q.z);
	vec3 c3 = vert_at(job, q.w);

	int i = int(texel & 3u);
	int j = int(texel >> 2u);
	float denom = float(TILE_TEXELS - 1);
	vec3 p = bilinear_corner(c0, c1, c2, c3, float(i) / denom, float(j) / denom);

	float cell = pc.origin.w;
	float e = 0.25 * cell;
	uint op_base = job * MAX_REGION_OPS;
	uint op_count = uint(pc.params.x);

	float d0; uint m0;
	eval_field(p, op_base, op_count, d0, m0);
	float gx0, gx1, gy0, gy1, gz0, gz1; uint mtmp;
	eval_field(p + vec3(e, 0, 0), op_base, op_count, gx1, mtmp);
	eval_field(p - vec3(e, 0, 0), op_base, op_count, gx0, mtmp);
	eval_field(p + vec3(0, e, 0), op_base, op_count, gy1, mtmp);
	eval_field(p - vec3(0, e, 0), op_base, op_count, gy0, mtmp);
	eval_field(p + vec3(0, 0, e), op_base, op_count, gz1, mtmp);
	eval_field(p - vec3(0, 0, e), op_base, op_count, gz0, mtmp);
	vec3 g = vec3(gx1 - gx0, gy1 - gy0, gz1 - gz0);
	g = dot(g, g) < 1e-16 ? vec3(0.0, 1.0, 0.0) : normalize(g);

	float step_d = clamp(d0, -2.0 * cell, 2.0 * cell);
	vec3 qp = p - step_d * g;
	float d1; uint mat;
	eval_field(qp, op_base, op_count, d1, mat);
	if (mat == 0u) {
		float dtmp;
		eval_field(qp - 0.25 * cell * g, op_base, op_count, dtmp, mat);
	}

	float r = 2.0 * cell;
	float open; uint mopen;
	eval_field(qp + g * r, op_base, op_count, open, mopen);
	float ao = clamp(open / r, 0.0, 1.0);

	vec2 oct = oct_encode(g);
	uvec4 rgba = uvec4(uint(floor(oct.x * 255.0 + 0.5)), uint(floor(oct.y * 255.0 + 0.5)),
			min(mat, 255u), uint(floor(max(ao, 0.15) * 255.0 + 0.5)));

	int tile = int(quad) % TILES_PER_PAGE;
	int page = int(quad) / TILES_PER_PAGE;
	int tx = (tile % TILES_X) * TILE_TEXELS;
	int ty = (tile / TILES_X) * TILE_TEXELS;
	int idx = int(job) * (2 * PAGE_TEXELS) + page * PAGE_TEXELS + (ty + j) * PAGE_W + (tx + i);
	bake.v[idx] = rgba.x | (rgba.y << 8) | (rgba.z << 16) | (rgba.w << 24);
}
```

- [ ] **Step 7: Write the pass**

Create `extension/src/render/lod_bake_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "lod/lod_page.h"
#include "render/mesh_pass.h"

namespace godot {

class VolumePool;

// Spec section 4's bakery, as a fourth dispatch on the mesher's worker device. It reads the
// vertex and quad buffers the two preceding passes just wrote and produces one RGBA8 word
// per texel of up to kLodMaxPagesPerChunk pages per job, so the whole chunk -- lattice,
// mesh, bake -- is one compute list, one submit and one sync.
class LodBakePass {
public:
	~LodBakePass();
	bool initialize(RenderingDevice *rd, const VolumePool *volumes, const MeshPassConfig &cfg);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }

	RID output_buffer() const { return out_; }
	int64_t output_bytes_per_job() const {
		return static_cast<int64_t>(ve::kLodMaxPagesPerChunk) * ve::kLodPageTexels * 4;
	}
	int64_t output_offset(int job_index) const { return job_index * output_bytes_per_job(); }

	// Records the dispatch into an OPEN compute list. `verts`, `quads`, `counts` and `ops`
	// are MeshPass's own buffers; the caller has already recorded the three mesh passes.
	void record(int64_t list, RID verts, RID quads, RID counts, RID ops, int job_index,
			const MeshJob &job);

private:
	RenderingDevice *rd_ = nullptr;
	MeshPassConfig cfg_;
	RID shader_, pipeline_, uset_, out_;
	RID bound_verts_, bound_quads_, bound_counts_, bound_ops_;
};

} // namespace godot
```

Create `extension/src/render/lod_bake_pass.cpp`, following `IslandExtractPass`'s shape exactly — load `res://shaders/lod_bake.comp.glsl` through `ve::load_shader_source` + `ve::strip_shader_annotations`, compile, build the pipeline, allocate `out_` as a storage buffer of `cfg.max_jobs * output_bytes_per_job()` bytes, and cache one uniform set keyed on the four bound buffer RIDs plus the volume pool's two. `record` is:

```cpp
void LodBakePass::record(int64_t list, RID verts, RID quads, RID counts, RID ops,
		int job_index, const MeshJob &job) {
	if (!pipeline_.is_valid()) return;
	ensure_uset(verts, quads, counts, ops);
	if (!uset_.is_valid()) return;
	rd_->compute_list_bind_compute_pipeline(list, pipeline_);
	rd_->compute_list_bind_uniform_set(list, uset_, 0);
	// The same 32-byte push constant the three mesh passes take, so a job description is
	// one struct for the whole chain. Only .w (cell size) and params.x/z/w are read here.
	PackedByteArray pc;
	pc.resize(32);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	int32_t *i = reinterpret_cast<int32_t *>(pc.ptrw());
	f[0] = job.origin[0];
	f[1] = job.origin[1];
	f[2] = job.origin[2];
	f[3] = job.cell_size;
	i[4] = job.op_count;
	i[5] = cfg_.max_verts;
	i[6] = cfg_.max_quads;
	i[7] = job_index;
	rd_->compute_list_set_push_constant(list, pc, pc.size());
	// Worst case: every tiled quad of every page. The shader's own count guard retires the
	// excess in one instruction, which is cheaper than a readback to size the dispatch.
	const int threads = ve::kLodTilesPerPage * ve::kLodMaxPagesPerChunk * 16;
	rd_->compute_list_dispatch(list, (threads + 63) / 64, 1, 1);
}
```

- [ ] **Step 8: Write the GPU/CPU differential test**

Create `tests/test_lod_bake.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# GPU/CPU differential test for the bakery (spec section 8). shaders/lod_bake.comp.glsl must
# agree with ve::bake_lod_texel, or the far field is lit by a different surface than the one
# the mesh describes.
#
# Tolerances, and why:
#  * NORMAL: compared as a direction, not as bytes. Octahedral encoding is one part in 255
#    per axis and the gradient is a difference of two sin()-bearing evaluations, so the two
#    sides can legitimately land on adjacent codes. The gate is 3 degrees, which is four
#    times the encoding's own resolution and still an order of magnitude tighter than any
#    visible difference.
#  * MATERIAL: exact. It is an integer decision from the same field, and a disagreement
#    means one side refined to the other side of the surface -- a real bug.
#  * AMBIENT: 2 bytes. One field evaluation each, same glibc-vs-driver sin() drift as the
#    lattice diff, scaled by 255 over a 2-cell radius.

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

func test_gpu_and_cpu_bake_the_same_texels() -> void:
	var w := make_world()
	for level in range(4):
		var d: Dictionary = w.debug_lod_bake_diff(level, Vector3i(0, 0, 0))
		assert_int(d["texels_compared"]).override_failure_message(
			"level %d baked nothing" % level).is_greater(256)
		assert_float(d["max_normal_degrees"]).override_failure_message(
			"level %d: normals differ by %.2f degrees" % [level, d["max_normal_degrees"]]
			).is_less_equal(3.0)
		assert_int(d["material_mismatches"]).override_failure_message(
			"level %d: %d texels disagree on material" % [level, d["material_mismatches"]]
			).is_equal(0)
		assert_int(d["max_ambient_diff"]).is_less_equal(2)

func test_a_baked_tile_varies_across_its_interior() -> void:
	# The reason the tile is 4x4 and not 2x2: if interior texels only ever repeated their
	# corners, the whole bake would be per-vertex data stored four times over.
	var w := make_world()
	var d: Dictionary = w.debug_lod_bake_diff(0, Vector3i(0, 0, 0))
	assert_int(d["interior_texels_differing_from_corners"]).is_greater(0)

func test_no_baked_texel_is_air() -> void:
	# Material 0 renders as common.glslh's magenta. A texel that refined the wrong way would
	# put a magenta speck on the horizon, which is exactly the M1 bug this guards against.
	var w := make_world()
	for level in range(4):
		var d: Dictionary = w.debug_lod_bake_diff(level, Vector3i(0, 0, 0))
		assert_int(d["air_texels"]).override_failure_message(
			"level %d baked %d air texels" % [level, d["air_texels"]]).is_equal(0)
```

`VoxelWorld::debug_lod_bake_diff` is added with the other LoD hooks in Task 6; until then this suite is red.

- [ ] **Step 9: Commit**

```bash
git add shaders/lod_bake.comp.glsl extension/src/lod/lod_bake.h extension/src/lod/lod_bake.cpp \
        extension/src/render/lod_bake_pass.h extension/src/render/lod_bake_pass.cpp \
        extension/tests/test_lod_bake.cpp tests/test_lod_bake.gd
git commit -m "feat(lod): per-quad texture bakery with a CPU reference"
```

---

### Task 6: the LoD build queue on the mesher's worker thread

`MeshService` gains a third queue. Collision batches, island extractions and LoD builds all live on the one worker device that already compiles `field.glslh`, already holds the volume pool and already reads back — spec §4's "~1–4 ms GPU each; a few per frame" fits between collision batches without a second device or a second in-flight protocol. This task also lands every `debug_lod_*` hook the two previous tasks' gdUnit suites are waiting on.

**Files:**
- Modify: `extension/src/render/mesh_pass.h`, `extension/src/render/mesh_pass.cpp`
- Modify: `extension/src/render/mesh_service.h`, `extension/src/render/mesh_service.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_lod_build.gd`

**Interfaces:**
- Consumes: `godot::LodBakePass`, `godot::MeshJob`, `godot::MeshResult`; `ve::build_lod_pages`, `ve::LodChunkBuild` (`lod/lod_page.h`); `ve::lod_ops_for_chunk` (`lod/lod_grid.h`).
- Produces:
  - `struct godot::LodBuildJob { int level; ve::IVec3 chunk; std::vector<ve::EditOp> ops; }`
  - `struct godot::LodBuildResult { int level; ve::IVec3 chunk; ve::LodChunkBuild build; std::vector<uint8_t> page_texels; bool empty; bool failed; }` — `page_texels` is `pages.size() * kLodPageTexBytes` bytes, page-major
  - `MeshService::submit_lod(std::vector<LodBuildJob>)`, `lod_busy()`, `collect_lod(std::vector<LodBuildResult> *)`, `lod_available()`
  - `MeshPass::lod_build_sync(const LodBuildJob &, LodBuildResult *)`
  - `VoxelWorld::debug_lod_mesh_diff`, `debug_lod_bake_diff`, `debug_lod_build`

- [ ] **Step 1: Write the failing test**

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

func test_a_surface_chunk_builds_pages_at_every_level() -> void:
	var w := make_world()
	for level in range(4):
		var d: Dictionary = w.debug_lod_build(level, Vector3i(0, 0, 0))
		assert_bool(d["failed"]).override_failure_message(
			"level %d build failed" % level).is_false()
		assert_bool(d["empty"]).override_failure_message(
			"level %d found no surface where the terrain is" % level).is_false()
		assert_int(d["pages"]).is_between(1, 2)
		assert_int(d["quad_count"]).is_greater(0)
		# Bytes: one full page texture per page, no more and no less.
		assert_int(d["texel_bytes"]).is_equal(d["pages"] * 512 * 256 * 4)
		# Vertices: four per quad, eight bytes each.
		assert_int(d["vertex_bytes"]).is_equal(d["quad_count"] * 4 * 8)

func test_a_sky_chunk_builds_nothing_and_costs_nothing() -> void:
	var w := make_world()
	# The demo terrain tops out near y = 61 m; chunk y = 8 at L1 starts at 204.8 m.
	var d: Dictionary = w.debug_lod_build(0, Vector3i(0, 8, 0))
	assert_bool(d["empty"]).is_true()
	assert_int(d["pages"]).is_equal(0)
	assert_int(d["texel_bytes"]).is_equal(0)

func test_a_boundary_chunk_grows_skirts() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_lod_build(0, Vector3i(0, 2, 0))
	assert_int(d["quad_count"]).is_greater(d["surface_quads"])
	assert_int(d["skirt_quads"]).override_failure_message(
		"a chunk the surface crosses must have a boundary ring").is_greater(0)

func test_an_edit_changes_what_the_chunk_builds() -> void:
	var w := make_world()
	var before: Dictionary = w.debug_lod_build(0, Vector3i(1, 2, 1))
	# A 6 m crater, comfortably larger than L1's 0.4 m cell, so it survives the relevance cut.
	var hit: Dictionary = w.debug_raycast(Vector3(38.0, 120.0, 38.0), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var p: Vector3 = hit["position"]
	w.debug_apply_sphere_subtract(p, 6.0)
	var after: Dictionary = w.debug_lod_build(0, Vector3i(1, 2, 1))
	assert_int(after["quad_count"]).override_failure_message(
		"the crater did not reach the LoD mesh").is_not_equal(before["quad_count"])

func test_an_edit_smaller_than_the_cell_is_dropped_at_coarse_levels() -> void:
	var w := make_world()
	var before: Dictionary = w.debug_lod_build(2, Vector3i(0, 0, 0))
	var hit: Dictionary = w.debug_raycast(Vector3(38.0, 120.0, 38.0), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	# 0.5 m across, against L3's 6.4 m cell: it cannot move a sample, so it must not enter
	# the op list at all (and therefore cannot change the mesh).
	w.debug_apply_sphere_subtract(hit["position"], 0.25)
	var after: Dictionary = w.debug_lod_build(2, Vector3i(0, 0, 0))
	assert_int(after["op_count"]).is_equal(before["op_count"])
	assert_int(after["quad_count"]).is_equal(before["quad_count"])

func test_lod_builds_do_not_disturb_the_collision_mesher(timeout := 20000) -> void:
	var w := make_world()
	# Interleave: a LoD build between two collision batches must leave the collision result
	# byte-identical, since they share one device, one op pool and one set of buffers.
	var a: Dictionary = w.debug_mesh_diff(Vector3i(1, 6, 1))
	w.debug_lod_build(1, Vector3i(0, 0, 0))
	var b: Dictionary = w.debug_mesh_diff(Vector3i(1, 6, 1))
	assert_int(b["cells_only_gpu"]).is_equal(a["cells_only_gpu"])
	assert_int(b["quads_only_gpu"]).is_equal(a["quads_only_gpu"])
	assert_float(b["max_position_diff"]).is_equal_approx(a["max_position_diff"], 0.0001)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_build.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_lod_build'`.

- [ ] **Step 3: Add the synchronous build to `MeshPass`**

In `extension/src/render/mesh_pass.h`, add the job/result types and the entry point:

```cpp
struct LodBuildJob {
	int level = 0;
	ve::IVec3 chunk{};
	std::vector<ve::EditOp> ops;
};

struct LodBuildResult {
	int level = 0;
	ve::IVec3 chunk{};
	ve::LodChunkBuild build;           // page vertex bytes, quad counts, quantisation box
	std::vector<uint8_t> page_texels;  // build.pages.size() * ve::kLodPageTexBytes, page-major
	int op_count = 0;                  // diagnostic: what survived the relevance cut
	bool empty = false;
	bool failed = false;
};
```

and to `class MeshPass`:

```cpp
	// One LoD chunk, inline (record the four passes, submit, sync, read back). LoD builds
	// are rare enough -- one or two a frame -- that the in-flight machinery the collision
	// batch needs would only add a protocol; the worker thread absorbs the sync.
	bool lod_build_sync(const LodBuildJob &job, LodBuildResult *out);
	LodBakePass &bake() { return bake_; }
```

In `extension/src/render/mesh_pass.cpp`:

```cpp
bool MeshPass::lod_build_sync(const LodBuildJob &job, LodBuildResult *out) {
	if (!is_valid() || !out) return false;
	out->level = job.level;
	out->chunk = job.chunk;
	out->op_count = static_cast<int>(job.ops.size());
	out->empty = false;
	out->failed = false;

	MeshJob mj;
	mj.chunk = job.chunk;
	ve::lod_chunk_origin(job.level, job.chunk, mj.origin);
	mj.cell_size = ve::lod_cell_size(job.level);
	mj.ops = job.ops.empty() ? nullptr : job.ops.data();
	mj.op_count = static_cast<int>(job.ops.size());

	reset_counts();
	upload_ops(mj, 0); // buffer_update before the list opens (M2 Task 12's ordering)
	const int64_t list = rd_->compute_list_begin();
	record_field(list, mj, 0);
	rd_->compute_list_add_barrier(list);
	record_cells(list, mj, 0);
	rd_->compute_list_add_barrier(list);
	record_quads(list, mj, 0);
	rd_->compute_list_add_barrier(list);
	bake_.record(list, verts_, quads_, counts_, ops_, 0, mj);
	rd_->compute_list_end();
	rd_->submit();
	rd_->sync();

	const PackedByteArray cbytes = rd_->buffer_get_data(counts_, 0, 16);
	if (cbytes.size() < 16) {
		out->failed = true;
		return false;
	}
	const uint32_t *c = reinterpret_cast<const uint32_t *>(cbytes.ptr());
	const int vert_count = static_cast<int>(std::min(c[0], static_cast<uint32_t>(cfg_.max_verts)));
	const int quad_count = static_cast<int>(std::min(c[1], static_cast<uint32_t>(cfg_.max_quads)));
	if (quad_count == 0) {
		out->empty = true;
		return true;
	}

	const PackedByteArray vbytes = rd_->buffer_get_data(verts_, 0, vert_count * 12);
	const PackedByteArray qbytes = rd_->buffer_get_data(quads_, 0, quad_count * 16);
	if (vbytes.size() < vert_count * 12 || qbytes.size() < quad_count * 16) {
		out->failed = true;
		return false;
	}
	std::vector<ve::LodQuad> quads(quad_count);
	{
		const uint32_t *q = reinterpret_cast<const uint32_t *>(qbytes.ptr());
		for (int i = 0; i < quad_count; i++)
			quads[i] = ve::LodQuad{{q[i * 4 + 0], q[i * 4 + 1], q[i * 4 + 2], q[i * 4 + 3]}};
	}
	ve::build_lod_pages(reinterpret_cast<const float *>(vbytes.ptr()), vert_count, quads.data(),
			quad_count, job.level, job.chunk, &out->build);
	if (out->build.pages.empty()) {
		out->empty = true;
		return true;
	}

	// Only the pages that exist are read back: a typical chunk uses one of the two, and the
	// other half megabyte is not worth moving across the bus to throw away.
	const int64_t page_bytes = ve::kLodPageTexBytes;
	const int64_t want = static_cast<int64_t>(out->build.pages.size()) * page_bytes;
	const PackedByteArray tex = rd_->buffer_get_data(bake_.output_buffer(), 0, want);
	if (tex.size() < want) {
		out->failed = true;
		return false;
	}
	out->page_texels.assign(tex.ptr(), tex.ptr() + want);
	if (c[2] != 0) out->build.overflow = true;
	return true;
}
```

`record_field`, `record_cells` and `record_quads` are M3's, unchanged apart from the push constant Task 4 generalised.

- [ ] **Step 4: Add the third queue to `MeshService`**

In `extension/src/render/mesh_service.h`, mirroring the extraction queue exactly:

```cpp
	// LoD builds share the worker thread with meshing and extraction, in their own queue.
	// Priority order on the worker is extraction > collision > LoD: an island is a
	// player-visible event, a collider is something the player can fall through, and a
	// horizon chunk being one frame late is invisible.
	bool submit_lod(std::vector<LodBuildJob> jobs);
	bool lod_busy() const { return lod_busy_.load(std::memory_order_acquire); }
	int collect_lod(std::vector<LodBuildResult> *out);
	bool lod_available() const { return lod_available_.load(std::memory_order_acquire); }
```

with `std::vector<LodBuildJob> pending_lod_;`, `std::vector<LodBuildResult> lod_results_;`, `std::atomic<bool> lod_busy_{false};`, `std::atomic<bool> lod_available_{false};`. In `run()`, after the extraction and collision arms, add:

```cpp
		// LoD last: a collision chunk the player is about to walk on outranks the horizon.
		if (!lod.empty()) {
			for (const LodBuildJob &j : lod) {
				LodBuildResult r;
				if (!pass->lod_build_sync(j, &r)) {
					r.level = j.level;
					r.chunk = j.chunk;
					r.failed = true;
				}
				std::lock_guard<std::mutex> g(mu_);
				lod_results_.push_back(std::move(r));
			}
			lod_busy_.store(false, std::memory_order_release);
		}
```

and set `lod_available_` from `pass->bake().is_valid()` at startup.

- [ ] **Step 5: Add the `VoxelWorld` hooks**

In `extension/src/voxel_world.h`, in the debug block:

```cpp
	// --- M5 Task 6 hooks ---
	// GPU vs ve::dual_contour at a LoD pitch, the LoD counterpart of debug_mesh_diff.
	Dictionary debug_lod_mesh_diff(int level, Vector3i chunk);
	// GPU vs ve::bake_lod_texel over one chunk's tiled quads (spec section 8).
	Dictionary debug_lod_bake_diff(int level, Vector3i chunk);
	// One synchronous LoD build, reporting what came out. The streaming path never stalls
	// like this; the tests and the console command do.
	Dictionary debug_lod_build(int level, Vector3i chunk);
	// A sphere-subtract straight into the edit log, for tests that need an edit without a
	// tool. Returns the number of regions the append touched.
	int debug_apply_sphere_subtract(Vector3 center, float radius);
```

`debug_lod_build` collects ops with `ve::lod_ops_for_chunk`, runs `MeshService::run_sync` so the call lands on the thread that owns the device, and reports `pages`, `quad_count`, `surface_quads`, `skirt_quads`, `vertex_bytes`, `texel_bytes`, `op_count`, `empty`, `failed`, `overflow`. `debug_lod_mesh_diff` additionally re-runs `ve::dual_contour` on the GPU's own read-back lattice (as `debug_mesh_diff` does) and reports `lattice_max_diff`, `cells_only_cpu`, `cells_only_gpu`, `quads_only_cpu`, `quads_only_gpu`, `max_position_diff`, `quad_count`, `inside_mesh_box`. `debug_lod_bake_diff` re-bakes every tiled texel with `ve::bake_lod_texel` and reports `texels_compared`, `max_normal_degrees`, `material_mismatches`, `max_ambient_diff`, `air_texels`, `interior_texels_differing_from_corners`.

- [ ] **Step 6: Run the three gdUnit suites to verify they pass**

Run:
```bash
./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot \
  -a res://tests/test_lod_mesh.gd -a res://tests/test_lod_bake.gd -a res://tests/test_lod_build.gd
```
Expected: PASS — all three, including Task 4's and Task 5's suites which were red until now.

- [ ] **Step 7: Verify nothing else regressed**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: PASS — the whole engine suite. The worker device is now shared three ways, so this is the gate that says the collision and island paths still get their turn.

- [ ] **Step 8: Commit**

```bash
git add extension/src/render/mesh_pass.h extension/src/render/mesh_pass.cpp \
        extension/src/render/mesh_service.h extension/src/render/mesh_service.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_build.gd
git commit -m "feat(lod): LoD build queue on the mesher worker, with diff hooks"
```

---

### Task 7: `render/lod_pool` — pages on the render device

One vertex buffer, one shared index buffer, one texture array, one page-record SSBO. A page is 320 KB of vertex slab plus one 512×256 layer, and its slot number is the only handle anything outside this class ever holds.

**Files:**
- Create: `extension/src/render/lod_pool.h`, `extension/src/render/lod_pool.cpp`
- Create: `tests/test_lod_pool.gd`

**Interfaces:**
- Consumes: `ve::kLodVertsPerPage`, `kLodVertexBytes`, `kLodIndicesPerPage`, `kLodPageTexW/H`, `kLodPageTexBytes`, `ve::lod_shared_indices` (`lod/lod_page.h`); `ve::LodDrawPage` (`lod/lod_residency.h`).
- Produces:
  - `struct godot::LodPoolConfig { int max_pages = 512; }`
  - `class godot::LodPool` with `initialize(RenderingDevice *, const LodPoolConfig &)`, `teardown()`, `is_valid()`, `upload_page(RenderingDevice *, int slot, const uint8_t *verts, int64_t vert_bytes, const uint8_t *texels)`, `clear_page(RenderingDevice *, int slot)`, `write_records(RenderingDevice *, const std::vector<ve::LodDrawPage> &)`, `vertex_array()`, `index_array()`, `records_buffer()`, `texture()`, `vertex_format()`, `page_count()`, `bytes_resident()`

- [ ] **Step 1: Write the failing test**

Create `tests/test_lod_pool.gd`:

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
	w.max_lod_pages = 8
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_lod()).is_true()
	return w

func test_the_pool_reports_the_memory_it_actually_holds() -> void:
	var w := make_world()
	var s: Dictionary = w.debug_lod_pool_stats()
	assert_int(s["pages"]).is_equal(8)
	# 320 KB of vertex slab + 512 KB of texture layer per page.
	assert_int(s["bytes"]).is_equal(8 * (40960 * 8 + 512 * 256 * 4))
	assert_bool(s["valid"]).is_true()

func test_an_uploaded_page_comes_back_byte_for_byte() -> void:
	var w := make_world()
	# A recognisable pattern in both halves, so a wrong offset shows up as a shifted readback
	# rather than as zeroes (which a cleared buffer would also produce).
	assert_bool(w.debug_lod_upload_test_page(3, 1234)).is_true()
	var d: Dictionary = w.debug_lod_read_test_page(3, 1234)
	assert_int(d["vertex_mismatches"]).is_equal(0)
	assert_int(d["texel_mismatches"]).is_equal(0)

func test_pages_do_not_overlap() -> void:
	var w := make_world()
	assert_bool(w.debug_lod_upload_test_page(0, 111)).is_true()
	assert_bool(w.debug_lod_upload_test_page(1, 222)).is_true()
	assert_int(w.debug_lod_read_test_page(0, 111)["vertex_mismatches"]).is_equal(0)
	assert_int(w.debug_lod_read_test_page(1, 222)["vertex_mismatches"]).is_equal(0)

func test_clearing_a_page_leaves_its_neighbours_alone() -> void:
	var w := make_world()
	assert_bool(w.debug_lod_upload_test_page(4, 777)).is_true()
	assert_bool(w.debug_lod_upload_test_page(5, 888)).is_true()
	w.debug_lod_clear_page(4)
	assert_int(w.debug_lod_read_test_page(5, 888)["vertex_mismatches"]).is_equal(0)

func test_the_shared_index_buffer_is_the_quad_pattern() -> void:
	var w := make_world()
	var idx: PackedInt32Array = w.debug_lod_indices(0, 12)
	assert_array(Array(idx)).is_equal([0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7])

func test_out_of_range_slots_are_refused_rather_than_scribbling() -> void:
	var w := make_world()
	assert_bool(w.debug_lod_upload_test_page(-1, 1)).is_false()
	assert_bool(w.debug_lod_upload_test_page(8, 1)).is_false()
	# The pool survives and still works.
	assert_bool(w.debug_lod_upload_test_page(7, 99)).is_true()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_pool.gd`
Expected: FAIL — `Invalid assignment of property 'max_lod_pages'`.

- [ ] **Step 3: Write the header**

Create `extension/src/render/lod_pool.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>
#include <vector>
#include "lod/lod_residency.h"

namespace godot {

struct LodPoolConfig {
	int max_pages = 512; // ~426 MB; see the M5 plan's memory arithmetic
};

// Spec section 4's resident chunk memory, as one flat pool on the render device. Every page
// is identical -- kLodVertsPerPage vertex slots and one texture-array layer -- so a page slot
// is the whole handle, the vertex offset is slot * kLodVertsPerPage (which is how the vertex
// shader recovers the slot from gl_VertexIndex), and the texture layer is the slot itself.
class LodPool {
public:
	~LodPool(); // calls teardown(): frees RIDs on rd_ (device must be alive)
	bool initialize(RenderingDevice *rd, const LodPoolConfig &cfg);
	void teardown();
	bool is_valid() const { return vertex_array_.is_valid() && texture_.is_valid(); }

	// `vert_bytes` may be shorter than a full slab: only what a page actually holds is
	// moved. `texels` is exactly ve::kLodPageTexBytes.
	bool upload_page(RenderingDevice *rd, int slot, const uint8_t *verts, int64_t vert_bytes,
			const uint8_t *texels);
	// Zeroes a page's quad count in the record buffer; the bytes are left alone because the
	// next upload overwrites them and nothing reads a page with no draw pointing at it.
	void clear_page(RenderingDevice *rd, int slot);

	// One buffer_update of the whole record table. Must be recorded BEFORE draw_list_begin.
	void write_records(RenderingDevice *rd, const std::vector<ve::LodDrawPage> &draws);

	RID vertex_array() const { return vertex_array_; }
	RID index_array() const { return index_array_; }
	RID records_buffer() const { return records_; }
	RID texture() const { return texture_; }
	// Task 8's pipeline must be created with the EXACT vertex format its vertex array was
	// created from, exactly as CompositePass must use the framebuffer's own format ID.
	int64_t vertex_format() const { return vertex_format_; }
	int page_count() const { return cfg_.max_pages; }
	int64_t bytes_resident() const {
		return static_cast<int64_t>(cfg_.max_pages) *
				(static_cast<int64_t>(ve::kLodVertsPerPage) * ve::kLodVertexBytes +
						ve::kLodPageTexBytes);
	}

private:
	RenderingDevice *rd_ = nullptr;
	LodPoolConfig cfg_;
	RID vertex_buffer_, vertex_array_;
	RID index_buffer_, index_array_;
	RID records_;  // one 32-byte LodPageRecord per page
	RID texture_;  // TEXTURE_TYPE_2D_ARRAY, RGBA8_UNORM, max_pages layers
	int64_t vertex_format_ = 0;
	std::vector<uint8_t> record_scratch_;
};

} // namespace godot
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/render/lod_pool.cpp`:

```cpp
#include "render/lod_pool.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

LodPool::~LodPool() {
	teardown();
}

bool LodPool::initialize(RenderingDevice *rd, const LodPoolConfig &cfg) {
	teardown();
	if (!rd || cfg.max_pages <= 0) return false;
	rd_ = rd;
	cfg_ = cfg;

	const int64_t vbytes =
			static_cast<int64_t>(cfg_.max_pages) * ve::kLodVertsPerPage * ve::kLodVertexBytes;
	vertex_buffer_ = rd->vertex_buffer_create(vbytes);
	if (!vertex_buffer_.is_valid()) return false;

	// One attribute, one buffer, 8-byte stride: three quantised position axes and the
	// packed tile/corner word, as a single R16G16B16A16_UINT. Splitting them would need a
	// 4-byte-aligned second offset the 8-byte vertex does not have room for.
	Ref<RDVertexAttribute> attr;
	attr.instantiate();
	attr->set_location(0);
	attr->set_offset(0);
	attr->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_UINT);
	attr->set_stride(ve::kLodVertexBytes);
	attr->set_frequency(RenderingDevice::VERTEX_FREQUENCY_VERTEX);
	vertex_format_ = rd->vertex_format_create(Array::make(attr));

	const int64_t total_verts = static_cast<int64_t>(cfg_.max_pages) * ve::kLodVertsPerPage;
	vertex_array_ = rd->vertex_array_create(total_verts, vertex_format_,
			Array::make(vertex_buffer_));
	if (!vertex_array_.is_valid()) return false;

	// Every page draws the same indices; only the indirect args' vertex offset differs.
	std::vector<uint16_t> idx;
	ve::lod_shared_indices(&idx);
	PackedByteArray ib;
	ib.resize(static_cast<int64_t>(idx.size()) * 2);
	std::memcpy(ib.ptrw(), idx.data(), idx.size() * 2);
	index_buffer_ = rd->index_buffer_create(static_cast<int64_t>(idx.size()),
			RenderingDevice::INDEX_BUFFER_FORMAT_UINT16, ib);
	if (!index_buffer_.is_valid()) return false;
	index_array_ = rd->index_array_create(index_buffer_, 0, static_cast<int64_t>(idx.size()));
	if (!index_array_.is_valid()) return false;

	records_ = rd->storage_buffer_create(static_cast<int64_t>(cfg_.max_pages) * 32);
	if (!records_.is_valid()) return false;
	record_scratch_.assign(static_cast<size_t>(cfg_.max_pages) * 32, 0);

	Ref<RDTextureFormat> tf;
	tf.instantiate();
	tf->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
	tf->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	tf->set_width(ve::kLodPageTexW);
	tf->set_height(ve::kLodPageTexH);
	tf->set_depth(1);
	tf->set_array_layers(cfg_.max_pages);
	tf->set_mipmaps(1);
	tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> tv;
	tv.instantiate();
	texture_ = rd->texture_create(tf, tv, TypedArray<PackedByteArray>());
	if (!texture_.is_valid()) {
		UtilityFunctions::printerr("LodPool: could not create a ", cfg_.max_pages,
				"-layer page array; lower max_lod_pages");
		return false;
	}
	return true;
}

void LodPool::teardown() {
	if (!rd_) return;
	// Uniform sets referencing these live in LodRasterPass and are freed first by its own
	// teardown; here the order is arrays before their backing buffers, texture last.
	for (RID *r : {&index_array_, &index_buffer_, &vertex_array_, &vertex_buffer_, &records_,
				 &texture_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	record_scratch_.clear();
	rd_ = nullptr;
}

bool LodPool::upload_page(RenderingDevice *rd, int slot, const uint8_t *verts,
		int64_t vert_bytes, const uint8_t *texels) {
	if (!rd || !is_valid() || slot < 0 || slot >= cfg_.max_pages) return false;
	const int64_t slab = static_cast<int64_t>(ve::kLodVertsPerPage) * ve::kLodVertexBytes;
	if (vert_bytes < 0 || vert_bytes > slab) return false;

	if (verts && vert_bytes > 0) {
		PackedByteArray vb;
		vb.resize(vert_bytes);
		std::memcpy(vb.ptrw(), verts, static_cast<size_t>(vert_bytes));
		if (rd->buffer_update(vertex_buffer_, slot * slab, vert_bytes, vb) != OK) return false;
	}
	if (texels) {
		PackedByteArray tb;
		tb.resize(ve::kLodPageTexBytes);
		std::memcpy(tb.ptrw(), texels, ve::kLodPageTexBytes);
		if (rd->texture_update(texture_, slot, tb) != OK) return false;
	}
	return true;
}

void LodPool::clear_page(RenderingDevice *rd, int slot) {
	if (!rd || !is_valid() || slot < 0 || slot >= cfg_.max_pages) return;
	int32_t *meta = reinterpret_cast<int32_t *>(record_scratch_.data() + slot * 32 + 16);
	meta[2] = 0; // quad count: nothing points at this page any more
}

void LodPool::write_records(RenderingDevice *rd, const std::vector<ve::LodDrawPage> &draws) {
	if (!rd || !is_valid()) return;
	// Quad counts are zeroed wholesale and re-stamped: a page dropped this frame must not
	// keep the count it had last frame, and one memset is cheaper than tracking the delta.
	for (int i = 0; i < cfg_.max_pages; i++)
		reinterpret_cast<int32_t *>(record_scratch_.data() + i * 32 + 16)[2] = 0;

	for (const ve::LodDrawPage &d : draws) {
		if (d.page < 0 || d.page >= cfg_.max_pages) continue;
		uint8_t *rec = record_scratch_.data() + static_cast<size_t>(d.page) * 32;
		float *f = reinterpret_cast<float *>(rec);
		int32_t *i = reinterpret_cast<int32_t *>(rec + 16);
		f[0] = d.quant_lo[0];
		f[1] = d.quant_lo[1];
		f[2] = d.quant_lo[2];
		f[3] = d.quant_size;
		i[0] = d.page; // texture layer == page slot
		i[1] = d.level;
		i[2] = d.quad_count;
		i[3] = 0;
	}
	PackedByteArray pb;
	pb.resize(static_cast<int64_t>(record_scratch_.size()));
	std::memcpy(pb.ptrw(), record_scratch_.data(), record_scratch_.size());
	rd->buffer_update(records_, 0, pb.size(), pb);
}
```

- [ ] **Step 5: Add the `VoxelWorld` exports and test hooks**

In `extension/src/voxel_world.h`: the member `int max_lod_pages_ = 512;` with `set_max_lod_pages` / `get_max_lod_pages` bound as a property, `LodPool *lod_pool_ = nullptr;`, and:

```cpp
	// --- M5 Task 7 hooks ---
	bool debug_init_lod();
	void debug_teardown_lod();
	Dictionary debug_lod_pool_stats();
	// Fills page `slot` with a deterministic pattern derived from `seed`, both halves.
	bool debug_lod_upload_test_page(int slot, int seed);
	// Reads it back and reports how many bytes disagree with the pattern.
	Dictionary debug_lod_read_test_page(int slot, int seed);
	void debug_lod_clear_page(int slot);
	PackedInt32Array debug_lod_indices(int first, int count);
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_pool.gd`
Expected: PASS — all six cases.

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/lod_pool.h extension/src/render/lod_pool.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_pool.gd
git commit -m "feat(lod): render-device page pool (verts, indices, texture array, records)"
```

---

### Task 8: `render/lod_raster_pass` — one indirect multi-draw, four levels

Spec §4's *Drawing* paragraph, entire: "CPU frustum-cull (few thousand AABBs), one indirect multi-draw per level via RenderingDevice, textures as arrays. All LoD shares the near field's cel lighting GLSL." The cull is a pure C++ function with a doctest, the multi-draw is one `draw_list_draw_indirect` for **all four levels** (the level only enters through the per-page record), and "shares the near field's lighting GLSL" is made literally true by moving the raymarcher's three shading lines into `common.glslh` and calling them from both.

**Files:**
- Create: `extension/src/lod/lod_cull.h`, `extension/src/lod/lod_cull.cpp`
- Create: `extension/tests/test_lod_cull.cpp`
- Create: `shaders/lod.vert.glsl`, `shaders/lod.frag.glsl`
- Create: `extension/src/render/lod_raster_pass.h`, `extension/src/render/lod_raster_pass.cpp`
- Create: `tests/test_lod_render.gd`
- Modify: `shaders/common.glslh` (add `kSunDir`, `shade_terrain`, `lod_fade`)
- Modify: `shaders/raymarch.comp.glsl` (call `shade_terrain`)
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (`lod_draws_`, `debug_lod_stage`, `debug_lod_render`)

**Interfaces:**
- Consumes: `ve::LodDrawPage` (`lod/lod_residency.h`); `ve::lod_cell_size`, `ve::kLodQuantPadCells` (`lod/lod_grid.h`, `lod/lod_page.h`); `godot::LodPool::vertex_array/index_array/records_buffer/texture/vertex_format/page_count` (`render/lod_pool.h`); `bayer4`, `oct_decode`, `material_albedo` (`shaders/common.glslh`).
- Produces:
  - `struct ve::LodFrustum { float p[6][4]; }`
  - `void ve::lod_frustum_from_view_proj(const float m[16], LodFrustum *out)` — `m` is **column-major**, `m[c * 4 + r]`
  - `bool ve::lod_aabb_visible(const LodFrustum &, const float lo[3], const float hi[3])`
  - `int ve::lod_cull_pages(const LodFrustum &, const LodDrawPage *, int count, int *out_indices)`
  - `class godot::LodRasterPass` with `initialize(RenderingDevice *)`, `teardown()`, `is_valid()`, `draw(RenderingDevice *, RID dst_color, RID dst_depth, LodPool &, const std::vector<ve::LodDrawPage> &, const Projection &view_proj, const Vector3 &cam_pos, float fade_scale)`, `last_drawn()`, `last_culled()`, `last_cpu_ms()`
  - `vec3 shade_terrain(vec3 albedo, vec3 n, float ao)` and `float lod_fade(float dist)` in `shaders/common.glslh`
  - `VoxelWorld::debug_lod_stage`, `VoxelWorld::debug_lod_render`

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_lod_cull.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_cull.h"
#include <cmath>
#include <vector>

namespace {

// The shape of matrix Godot 4.7 hands the compositor: a right-handed perspective, Vulkan
// clip z in [0, w], REVERSED so near maps to w and far to 0 (M1 errata 2). Column-major,
// m[c * 4 + r], exactly as godot::Projection::columns stores it.
void reverse_z_perspective(float fov_y_rad, float aspect, float znear, float zfar,
		float m[16]) {
	for (int i = 0; i < 16; i++) m[i] = 0.0f;
	const float f = 1.0f / std::tan(fov_y_rad * 0.5f);
	m[0 * 4 + 0] = f / aspect;
	m[1 * 4 + 1] = f;
	m[2 * 4 + 2] = znear / (zfar - znear);
	m[3 * 4 + 2] = znear * zfar / (zfar - znear);
	m[2 * 4 + 3] = -1.0f;
}

// A point AABB, so "is this box visible" reduces to "is this point inside".
bool point_visible(const ve::LodFrustum &fr, float x, float y, float z) {
	const float p[3] = {x, y, z};
	return ve::lod_aabb_visible(fr, p, p);
}

} // namespace

TEST_CASE("the reverse-Z frustum still bounds the same volume") {
	float m[16];
	reverse_z_perspective(1.0f, 1.0f, 1.0f, 100.0f, m);
	ve::LodFrustum fr{};
	ve::lod_frustum_from_view_proj(m, &fr);

	// Camera at the origin looking down -z. Reversing z swaps which ROW carries the near
	// plane and which carries the far plane, but the six planes bound the same frustum --
	// which is the whole reason the Gribb-Hartmann extraction needs no reverse-Z special case.
	CHECK(point_visible(fr, 0.0f, 0.0f, -50.0f));
	CHECK_FALSE(point_visible(fr, 0.0f, 0.0f, -0.5f));   // in front of near
	CHECK_FALSE(point_visible(fr, 0.0f, 0.0f, -150.0f)); // past far
	CHECK_FALSE(point_visible(fr, 0.0f, 0.0f, 50.0f));   // behind the camera
}

TEST_CASE("the side planes reject what is off screen") {
	float m[16];
	reverse_z_perspective(1.0f, 1.0f, 1.0f, 100.0f, m);
	ve::LodFrustum fr{};
	ve::lod_frustum_from_view_proj(m, &fr);

	// At z = -10 the half-extent is 10 * tan(0.5) ~= 5.46 m on both axes.
	CHECK(point_visible(fr, 5.0f, 0.0f, -10.0f));
	CHECK_FALSE(point_visible(fr, 6.0f, 0.0f, -10.0f));
	CHECK(point_visible(fr, 0.0f, -5.0f, -10.0f));
	CHECK_FALSE(point_visible(fr, 0.0f, 6.0f, -10.0f));
}

TEST_CASE("a box straddling a plane is kept") {
	float m[16];
	reverse_z_perspective(1.0f, 1.0f, 1.0f, 100.0f, m);
	ve::LodFrustum fr{};
	ve::lod_frustum_from_view_proj(m, &fr);

	// The safe direction for a culler is to keep: a chunk half on screen must still draw.
	const float lo[3] = {4.0f, -1.0f, -12.0f};
	const float hi[3] = {40.0f, 1.0f, -8.0f};
	CHECK(ve::lod_aabb_visible(fr, lo, hi));

	const float far_lo[3] = {40.0f, -1.0f, -12.0f};
	const float far_hi[3] = {80.0f, 1.0f, -8.0f};
	CHECK_FALSE(ve::lod_aabb_visible(fr, far_lo, far_hi));
}

TEST_CASE("culling a page list keeps order and reports the survivors") {
	float m[16];
	reverse_z_perspective(1.0f, 1.0f, 1.0f, 1000.0f, m);
	ve::LodFrustum fr{};
	ve::lod_frustum_from_view_proj(m, &fr);

	std::vector<ve::LodDrawPage> pages(4);
	auto box = [](ve::LodDrawPage &p, int level, float x, float z) {
		p.level = level;
		p.quad_count = 100;
		p.lo[0] = x; p.lo[1] = -5.0f; p.lo[2] = z;
		p.hi[0] = x + 10.0f; p.hi[1] = 5.0f; p.hi[2] = z + 10.0f;
	};
	box(pages[0], 0, -5.0f, -40.0f);   // dead ahead
	box(pages[1], 0, 400.0f, -40.0f);  // far to the right
	box(pages[2], 1, -5.0f, 30.0f);    // behind the camera
	box(pages[3], 1, -5.0f, -80.0f);   // dead ahead, further out
	for (int i = 0; i < 4; i++) pages[i].page = i;

	std::vector<int> visible(4, -1);
	const int n = ve::lod_cull_pages(fr, pages.data(), 4, visible.data());
	REQUIRE(n == 2);
	CHECK(visible[0] == 0);
	CHECK(visible[1] == 3);
}

TEST_CASE("an empty page is culled whatever the frustum says") {
	float m[16];
	reverse_z_perspective(1.0f, 1.0f, 1.0f, 1000.0f, m);
	ve::LodFrustum fr{};
	ve::lod_frustum_from_view_proj(m, &fr);

	// A page with no quads would issue a zero-index draw: the GPU cost is small but the
	// arithmetic is pointless, and the indirect args buffer is the scarcer resource.
	std::vector<ve::LodDrawPage> pages(1);
	pages[0].page = 7;
	pages[0].level = 0;
	pages[0].quad_count = 0;
	pages[0].lo[0] = -5.0f; pages[0].lo[1] = -5.0f; pages[0].lo[2] = -40.0f;
	pages[0].hi[0] = 5.0f;  pages[0].hi[1] = 5.0f;  pages[0].hi[2] = -30.0f;

	int visible = -1;
	CHECK(ve::lod_cull_pages(fr, pages.data(), 1, &visible) == 0);
}
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: FAIL — `lod/lod_cull.h: No such file or directory`.

- [ ] **Step 3: Write the culler**

Create `extension/src/lod/lod_cull.h`:

```cpp
#pragma once
#include "lod/lod_residency.h"

namespace ve {

// Six clip planes in world space, stored as {nx, ny, nz, d}. A point is inside the frustum
// when n . p + d >= 0 for every plane (planes point INWARD).
struct LodFrustum {
	float p[6][4] = {};
};

// Gribb-Hartmann extraction from a combined view-projection matrix. `m` is COLUMN-major --
// m[c * 4 + r] -- which is both what GLSL's mat4 wants and what godot::Projection::columns
// already holds, so no transpose happens anywhere in M5.
//
// The clip volume assumed here is Vulkan's 0 <= z_clip <= w_clip, NOT OpenGL's -w <= z <= w.
// Godot 4.7's reverse-Z only decides WHICH end of that range is near; it does not change the
// range, so the same six rows extract the same six planes (test_lod_cull.cpp proves it).
void lod_frustum_from_view_proj(const float m[16], LodFrustum *out);

// Conservative: a box that straddles any plane is kept. The safe direction for a culler is
// to draw something invisible, never to drop something visible (spec section 8).
bool lod_aabb_visible(const LodFrustum &f, const float lo[3], const float hi[3]);

// Fills `out_indices` with the indices into `pages` that survive, in input order, and
// returns how many. `out_indices` must have room for `count`. Pages with no quads are
// dropped here rather than in the pass. The AABB tested is the page's box grown by the
// level's quantisation pad, because skirts and the mesher's overlap cell both put geometry
// outside the nominal chunk box (Conventions Used Throughout).
int lod_cull_pages(const LodFrustum &f, const LodDrawPage *pages, int count,
		int *out_indices);

} // namespace ve
```

Create `extension/src/lod/lod_cull.cpp`:

```cpp
#include "lod/lod_cull.h"
#include "lod/lod_page.h"
#include <cmath>

namespace ve {
namespace {

// Row r of a column-major matrix.
inline void row(const float m[16], int r, float out[4]) {
	for (int c = 0; c < 4; c++) out[c] = m[c * 4 + r];
}

inline void plane(const float a[4], const float b[4], int sign, float out[4]) {
	for (int i = 0; i < 4; i++) out[i] = a[i] + sign * b[i];
	const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	if (len > 1e-12f)
		for (int i = 0; i < 4; i++) out[i] /= len;
}

} // namespace

void lod_frustum_from_view_proj(const float m[16], LodFrustum *out) {
	float r0[4], r1[4], r2[4], r3[4];
	row(m, 0, r0);
	row(m, 1, r1);
	row(m, 2, r2);
	row(m, 3, r3);
	plane(r3, r0, +1, out->p[0]); // left:   w + x >= 0
	plane(r3, r0, -1, out->p[1]); // right:  w - x >= 0
	plane(r3, r1, +1, out->p[2]); // bottom: w + y >= 0
	plane(r3, r1, -1, out->p[3]); // top:    w - y >= 0
	// The Vulkan depth pair: z >= 0 and z <= w. Under reverse-Z the first row is the FAR
	// plane and the second the NEAR one; the volume is identical either way.
	float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	plane(r2, zero, +1, out->p[4]);
	plane(r3, r2, -1, out->p[5]);
}

bool lod_aabb_visible(const LodFrustum &f, const float lo[3], const float hi[3]) {
	for (int i = 0; i < 6; i++) {
		const float *p = f.p[i];
		// The "positive vertex": the corner furthest along the plane normal. If even that
		// corner is outside, every corner is.
		const float x = p[0] >= 0.0f ? hi[0] : lo[0];
		const float y = p[1] >= 0.0f ? hi[1] : lo[1];
		const float z = p[2] >= 0.0f ? hi[2] : lo[2];
		if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) return false;
	}
	return true;
}

int lod_cull_pages(const LodFrustum &f, const LodDrawPage *pages, int count,
		int *out_indices) {
	int n = 0;
	for (int i = 0; i < count; i++) {
		const LodDrawPage &d = pages[i];
		if (d.quad_count <= 0) continue;
		const float pad = kLodQuantPadCells * lod_cell_size(d.level);
		const float lo[3] = {d.lo[0] - pad, d.lo[1] - pad, d.lo[2] - pad};
		const float hi[3] = {d.hi[0] + pad, d.hi[1] + pad, d.hi[2] + pad};
		if (!lod_aabb_visible(f, lo, hi)) continue;
		out_indices[n++] = i;
	}
	return n;
}

} // namespace ve
```

- [ ] **Step 4: Run the native test to verify it passes**

Run: `cd extension && scons test 2>&1 | tail -20`
Expected: PASS — five new `test_lod_cull.cpp` cases green, every earlier case still green. `src/lod/*.cpp` is already in `pure_sources` from Task 1, so no build change is needed.

- [ ] **Step 5: Commit the culler**

```bash
git add extension/src/lod/lod_cull.h extension/src/lod/lod_cull.cpp \
        extension/tests/test_lod_cull.cpp
git commit -m "feat(lod): frustum extraction and conservative AABB culling"
```

- [ ] **Step 6: Move the near field's shading into `common.glslh`**

Spec §4 says "All LoD shares the near field's cel lighting GLSL — the 150 m seam is a fade, not a style change." Make that literally true rather than a copied-and-pasted approximation, so M6's deferred cel stack has exactly one function to replace.

Append to `shaders/common.glslh`:

```glsl
// The sun the near field marches its shadow rays towards (M1). One declaration, so the LoD
// raster cannot drift a degree away from it.
const vec3 kSunDir = vec3(0.6, 0.8, 0.3);

// The whole of M5's lighting, shared by shaders/raymarch.comp.glsl and shaders/lod.frag.glsl
// (spec section 4: "All LoD shares the near field's cel lighting GLSL"). `ao` is 1.0 where
// nothing measured it -- the raymarcher -- and the bake's ambient byte where something did,
// so it multiplies the AMBIENT term only and the two paths agree exactly wherever ao == 1.
// M6 replaces this body with the deferred cel stack and both callers follow for free.
vec3 shade_terrain(vec3 albedo, vec3 n, float ao) {
	vec3 sun = normalize(kSunDir);
	float lam = max(dot(n, sun), 0.0);
	return albedo * (0.25 * ao + 0.75 * lam);
}

// The near/far cross-fade's ramp (spec section 3: "Dithered depth fade over 120-150 m").
// 0 = the raymarched near field owns this pixel, 1 = the LoD raster does. Both sides call
// THIS function, so the two dither masks are exact complements.
const float kLodFadeStartM = 120.0; // ve::kLodFadeStartM
const float kLodFadeEndM = 150.0;   // ve::kLodFadeEndM

float lod_fade(float dist) {
	return smoothstep(kLodFadeStartM, kLodFadeEndM, dist);
}
```

In `shaders/raymarch.comp.glsl`, replace the three shading lines inside `if (best.hit)`:

```glsl
	if (best.hit) {
		vec3 alb = material_albedo(best.mat);
		vec3 sun = normalize(vec3(0.6, 0.8, 0.3));
		float lam = max(dot(best.n, sun), 0.0);
		color = alb * (0.25 + 0.75 * lam);
		hitpos = vec4(best.p, 1.0);
	}
```

with:

```glsl
	if (best.hit) {
		// ao = 1.0: the raymarcher measures no ambient occlusion, so this is arithmetically
		// identical to the 0.25 + 0.75 * lambert it computed inline before Task 8.
		color = shade_terrain(material_albedo(best.mat), best.n, 1.0);
		hitpos = vec4(best.p, 1.0);
	}
```

- [ ] **Step 7: Prove the refactor changed no pixel**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_raymarch_pixel.gd -a res://tests/test_island_render.gd`
Expected: PASS — both suites assert on shaded colours, so a changed constant or a lost term fails them immediately. Green means `shade_terrain(alb, n, 1.0)` is the same expression it replaced.

- [ ] **Step 8: Write the two LoD shaders**

Create `shaders/lod.vert.glsl`:

```glsl
#[vertex]
#version 460
#include "common.glslh"

// One attribute, 8 bytes: three quantised position axes and the packed tile/corner word
// (ve::kLodVertexBytes). DATA_FORMAT_R16G16B16A16_UINT arrives as a uvec4.
layout(location = 0) in uvec4 v_packed;

struct PageRecord {
	vec4 quant; // xyz = ve::LodChunkBuild::quant_lo, w = quant_size
	ivec4 meta; // x = texture layer, y = level, z = quad count, w = flags
};
layout(set = 0, binding = 0, std430) readonly buffer Records { PageRecord v[]; } records;

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam_pos; // xyz = camera world position, w = fade scale (1 = fade on, 0 = fade off)
} pc;

layout(location = 0) out vec2 uv_out;
layout(location = 1) flat out float layer_out;
layout(location = 2) out vec3 world_out;

const int VERTS_PER_PAGE = 40960; // ve::kLodVertsPerPage
const int TILES_X = 128;          // ve::kLodTilesX
const int TILE_TEXELS = 4;        // ve::kLodTileTexels
const float PAGE_W = 512.0;       // ve::kLodPageTexW
const float PAGE_H = 256.0;       // ve::kLodPageTexH

void main() {
	// The indirect args set vertexOffset = page * VERTS_PER_PAGE, so this division is the
	// page slot -- the only per-draw parameter the shader gets. See the plan's Deliberate
	// Decisions for why this and not gl_DrawID or a non-zero firstInstance.
	int page = gl_VertexIndex / VERTS_PER_PAGE;
	PageRecord r = records.v[page];

	vec3 p = r.quant.xyz + (vec3(v_packed.xyz) / 65535.0) * r.quant.w;

	// Mirror of ve::lod_unpack_attr / ve::lod_tile_corner_uv. The corners land on the CORNER
	// TEXEL CENTRES of the quad's 4x4 tile, so every interior fragment's bilinear footprint
	// stays inside those 16 texels: no borders, no bleed, no seam between adjacent quads.
	int attr = int(v_packed.w);
	int tile = attr & 0x1FFF;
	int corner = (attr >> 13) & 3;
	float tx = float((tile % TILES_X) * TILE_TEXELS);
	float ty = float((tile / TILES_X) * TILE_TEXELS);
	float edge = float(TILE_TEXELS) - 0.5;
	float px = tx + ((corner == 1 || corner == 2) ? edge : 0.5);
	float py = ty + ((corner >= 2) ? edge : 0.5);

	uv_out = vec2(px / PAGE_W, py / PAGE_H);
	layer_out = float(r.meta.x);
	world_out = p;
	// view_proj is Godot's own depth-corrected scene projection, so the depth this writes is
	// reverse-Z already and matches to the bit what composite.frag.glsl computes by hand.
	gl_Position = pc.view_proj * vec4(p, 1.0);
}
```

Create `shaders/lod.frag.glsl`:

```glsl
#[fragment]
#version 460
#include "common.glslh"

layout(location = 0) in vec2 uv_in;
layout(location = 1) flat in float layer_in;
layout(location = 2) in vec3 world_in;
layout(location = 0) out vec4 frag_color;

// LINEAR filtering is the point of the tile layout: the corner texels ARE the quad corners,
// so interpolating between them is what turns 4x4 texels into a smooth patch.
layout(set = 0, binding = 1) uniform sampler2DArray pages;

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam_pos;
} pc;

void main() {
	// Spec section 3's near/far seam. shaders/composite.frag.glsl drops its DEPTH on exactly
	// the complementary set of pixels (bayer4 < f), so every pixel in the 120-150 m band is
	// owned by exactly one of the two. fade scale 0 disables the band entirely, which is what
	// the debug toggle and the offscreen render test use to see LoD at close range.
	float f = mix(1.0, lod_fade(distance(world_in, pc.cam_pos.xyz)), pc.cam_pos.w);
	if (bayer4(ivec2(gl_FragCoord.xy)) >= f) discard;

	vec4 t = texture(pages, vec3(uv_in, layer_in));
	vec3 n = oct_decode(t.rg);
	uint mat = uint(t.b * 255.0 + 0.5);
	// Depth comes from gl_Position through the fixed-function path; writing gl_FragDepth here
	// would only cost early-z for a value the rasteriser already has right.
	frag_color = vec4(shade_terrain(material_albedo(mat), n, t.a), 1.0);
}
```

- [ ] **Step 9: Write the failing render test**

Create `tests/test_lod_render.gd`:

```gdscript
extends GdUnitTestSuite

# Spec section 4's Drawing paragraph, end to end on a local device: build a real LoD chunk,
# upload its pages, point a camera at it and read the framebuffer back. Everything here is
# offscreen, so it runs headless and does not need the compositor (that is Task 9).

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
	w.max_lod_pages = 8
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_lod()).is_true()
	return w

# The demo generator's terrain sits around y = 51.2 (kSurfaceY) and the hills dip to about
# 49.4 m, so the surface STRADDLES the L1 boundary at y = 51.2. Stage both chunks either
# side of it -- (1,1,1) spans y 25.6-51.2 and (1,2,1) spans 51.2-76.8 -- so the patch of
# ground under x,z in 25.6-51.2 is covered whichever way the terrain went.
const CHUNKS := [Vector3i(1, 1, 1), Vector3i(1, 2, 1)]

func stage(w: VoxelWorld) -> int:
	var quads := 0
	for c in CHUNKS:
		var d: Dictionary = w.debug_lod_stage(0, c)
		assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
		quads += int(d.get("quads", 0))
	# If this is zero, the generator's surface is not where this suite thinks it is: run
	# w.debug_field_sdf(Vector3(38.4, y, 38.4)) over y and move CHUNKS to the pair that
	# brackets the sign change.
	assert_int(quads).is_greater(0)
	return quads

func pixel(img: PackedByteArray, width: int, x: int, y: int) -> Color:
	var i := (y * width + x) * 4
	return Color(img[i] / 255.0, img[i + 1] / 255.0, img[i + 2] / 255.0, img[i + 3] / 255.0)

func count_lit(img: PackedByteArray) -> int:
	var n := 0
	var i := 0
	while i < img.size():
		if img[i] > 0 or img[i + 1] > 0 or img[i + 2] > 0:
			n += 1
		i += 4
	return n

func test_a_staged_chunk_covers_pixels(timeout := 60000) -> void:
	var w := make_world()
	stage(w)
	# Looking straight down at the chunk's top face from 40 m above its centre, with the fade
	# off so distance does not enter into it.
	var r: Dictionary = w.debug_lod_render(Vector3(38.4, 91.2, 38.4), Vector3(0, -1, 0),
		64, 64, false)
	assert_bool(r.get("ok", false)).override_failure_message(str(r)).is_true()
	assert_int(r["drawn"]).is_greater(0)
	# A 25.6 m box seen from 40 m up subtends 2*atan(12.8/40) = 0.62 rad of a 1.047 rad
	# vertical fov, so it covers roughly 38 of 64 pixels per side: ~1400 px if it filled the
	# square. 800 leaves room for the terrain not filling the chunk's whole footprint.
	assert_int(count_lit(r["image"])).is_greater(800)

func test_nothing_is_drawn_when_the_chunk_is_behind_the_camera(timeout := 60000) -> void:
	var w := make_world()
	stage(w)
	var r: Dictionary = w.debug_lod_render(Vector3(38.4, 91.2, 38.4), Vector3(0, 1, 0),
		64, 64, false)
	assert_bool(r["ok"]).is_true()
	assert_int(r["drawn"]).is_equal(0)
	assert_int(r["culled"]).is_greater(0)
	assert_int(count_lit(r["image"])).is_equal(0)

func test_the_image_is_the_right_way_up(timeout := 60000) -> void:
	var w := make_world()
	stage(w)
	# From the side, level with the chunk's top face: ground below the horizon, sky above.
	# A y-flip in the projection would put the terrain in the top half instead, and NOTHING
	# else in this milestone would notice.
	var r: Dictionary = w.debug_lod_render(Vector3(38.4, 51.2, 90.0), Vector3(0, -0.35, -1),
		64, 64, false)
	assert_bool(r["ok"]).is_true()
	assert_int(r["drawn"]).is_greater(0)
	var img: PackedByteArray = r["image"]
	var top := 0
	var bottom := 0
	for y in range(64):
		for x in range(64):
			var c := pixel(img, 64, x, y)
			if c.r > 0.0 or c.g > 0.0 or c.b > 0.0:
				if y < 32:
					top += 1
				else:
					bottom += 1
	assert_int(bottom).is_greater(top * 2)

func test_the_bake_is_what_is_being_sampled(timeout := 60000) -> void:
	var w := make_world()
	stage(w)
	var r: Dictionary = w.debug_lod_render(Vector3(38.4, 91.2, 38.4), Vector3(0, -1, 0),
		64, 64, false)
	var c := pixel(r["image"], 64, 32, 32)
	# common.glslh's material 1 (rock) shaded by shade_terrain against a mostly-up normal:
	# grey-brown and clearly lit, never black (a missing texture) and never magenta.
	assert_float(c.r).is_greater(0.05)
	assert_float(c.r).is_less(0.95)
	assert_float(absf(c.r - c.b)).is_less(0.35)

func test_the_fade_band_dithers_instead_of_switching(timeout := 60000) -> void:
	var w := make_world()
	stage(w)
	# 135 m out, the middle of the 120-150 m band: with the fade ON, roughly half the covered
	# pixels survive the dither. All or none would mean the two masks are not complementary.
	var eye := Vector3(38.4, 51.2, 38.4) + Vector3(0, 0.35, 1).normalized() * 135.0
	var off: Dictionary = w.debug_lod_render(eye, Vector3(0, -0.35, -1), 128, 128, false)
	var on: Dictionary = w.debug_lod_render(eye, Vector3(0, -0.35, -1), 128, 128, true)
	var n_off := count_lit(off["image"])
	var n_on := count_lit(on["image"])
	assert_int(n_off).is_greater(50)
	assert_int(n_on).is_greater(0)
	assert_int(n_on).is_less(n_off)
	assert_float(float(n_on) / float(n_off)).is_between(0.15, 0.85)
```

- [ ] **Step 10: Run it to make sure it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_render.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_lod_stage'`.

- [ ] **Step 11: Write the pass header**

Create `extension/src/render/lod_raster_pass.h`:

```cpp
#pragma once
#include "lod/lod_cull.h"
#include "render/lod_pool.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <cstdint>
#include <vector>

namespace godot {

// Spec section 4's Drawing paragraph: CPU frustum cull, then ONE indirect multi-draw for all
// four levels into whatever colour/depth pair it is handed. The level is not a draw
// parameter here -- every page has identical dimensions and carries its level in its record
// -- so the spec's "one indirect multi-draw per level" collapses to strictly fewer draws.
class LodRasterPass {
public:
	~LodRasterPass(); // calls teardown()
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return shader_.is_valid(); }

	// Culls `draws`, writes the page records and the indirect args, and issues the draw.
	// Returns the number of pages actually drawn. `fade_scale` is 1.0 for the 120-150 m
	// cross-fade and 0.0 to draw the LoD everywhere (debug toggle, offscreen tests).
	// Both buffer_updates happen BEFORE draw_list_begin -- they are device-level commands
	// and are illegal inside an open list (Global Constraints).
	int draw(RenderingDevice *rd, RID dst_color, RID dst_depth, LodPool &pool,
			const std::vector<ve::LodDrawPage> &draws, const Projection &view_proj,
			const Vector3 &cam_pos, float fade_scale);

	int last_drawn() const { return last_drawn_; }
	int last_culled() const { return last_culled_; }
	float last_cpu_ms() const { return last_cpu_ms_; }

private:
	bool ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth,
			int64_t vertex_format);
	bool ensure_args(RenderingDevice *rd, int capacity);
	bool ensure_uniform_set(RenderingDevice *rd, RID records, RID texture);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_, args_, uset_;
	RID uset_records_, uset_texture_; // what uset_ currently references
	int64_t fb_format_ = 0;
	int64_t pipeline_vertex_format_ = 0;
	RID framebuffer_, fb_color_, fb_depth_;
	int args_capacity_ = 0;
	std::vector<int> visible_;           // indices into the caller's draw list
	std::vector<uint32_t> args_scratch_; // 5 uint32 per visible page, staged for the upload
	int last_drawn_ = 0;
	int last_culled_ = 0;
	float last_cpu_ms_ = 0.0f;
};

} // namespace godot
```

- [ ] **Step 12: Write the pass implementation**

Create `extension/src/render/lod_raster_pass.cpp`:

```cpp
#include "render/lod_raster_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

namespace {
// {index count, instance count, first index, vertex offset, first instance} -- the layout
// docs/api/renderingdevice.md gives for draw_list_draw_indirect with use_indices = true.
constexpr int kIndirectStride = 20;
} // namespace

LodRasterPass::~LodRasterPass() {
	teardown();
}

void LodRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	auto load_stage = [&](const char *file, RenderingDevice::ShaderStage stage,
							  Ref<RDShaderSource> &src) -> bool {
		std::string err;
		const String path = ProjectSettings::get_singleton()->globalize_path(
				String("res://shaders/") + file);
		const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
		const std::string code = ve::strip_shader_annotations(
				ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
		if (code.empty()) {
			UtilityFunctions::printerr("LodRasterPass: ", err.c_str());
			return false;
		}
		src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		src->set_stage_source(stage, String(code.c_str()));
		return true;
	};
	Ref<RDShaderSource> src;
	src.instantiate();
	if (!load_stage("lod.vert.glsl", RenderingDevice::SHADER_STAGE_VERTEX, src)) return;
	if (!load_stage("lod.frag.glsl", RenderingDevice::SHADER_STAGE_FRAGMENT, src)) return;
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err =
			spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX) +
			spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("LodRasterPass: ", compile_err);
		return;
	}
	shader_ = rd->shader_create_from_spirv(spirv);

	// The tile layout is built for bilinear: the corner texels ARE the quad corners.
	// CLAMP_TO_EDGE is belt-and-braces -- no UV this shader produces leaves [0,1].
	Ref<RDSamplerState> ss;
	ss.instantiate();
	ss->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	ss->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	ss->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	ss->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	ss->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_ = rd->sampler_create(ss);
}

void LodRasterPass::teardown() {
	if (!rd_) return;
	// Uniform set first: it references the shader, and freeing the shader cascades to the
	// pipeline (M1's documented free order, CompositePass::teardown).
	for (RID *r : {&uset_, &pipeline_, &shader_, &sampler_, &args_, &framebuffer_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	uset_records_ = RID();
	uset_texture_ = RID();
	fb_color_ = RID();
	fb_depth_ = RID();
	args_capacity_ = 0;
	visible_.clear();
	args_scratch_.clear();
	rd_ = nullptr;
}

bool LodRasterPass::ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth,
		int64_t vertex_format) {
	if (framebuffer_.is_valid() && dst_color == fb_color_ && dst_depth == fb_depth_ &&
			pipeline_.is_valid() && pipeline_vertex_format_ == vertex_format) {
		return true;
	}
	if (dst_color != fb_color_ || dst_depth != fb_depth_ || !framebuffer_.is_valid()) {
		if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
		framebuffer_ = rd->framebuffer_create(Array::make(dst_color, dst_depth));
		fb_color_ = dst_color;
		fb_depth_ = dst_depth;
		if (!framebuffer_.is_valid()) return false;
		// The pipeline's framebuffer format must be the EXACT ID the framebuffer derives
		// from these textures -- usage bits are part of the format cache key and the draw
		// path ERR_FAILs on any mismatch (CompositePass::ensure_pipeline's note).
		const int64_t fmt = rd->framebuffer_get_format(framebuffer_);
		if (fmt != fb_format_ && pipeline_.is_valid()) {
			rd->free_rid(pipeline_);
			pipeline_ = RID();
		}
		fb_format_ = fmt;
	}
	if (pipeline_.is_valid() && pipeline_vertex_format_ == vertex_format) return true;
	if (pipeline_.is_valid()) {
		rd->free_rid(pipeline_);
		pipeline_ = RID();
	}

	Ref<RDPipelineRasterizationState> rs;
	rs.instantiate();
	// Deliberate: skirts are two-sided by construction and M3 errata 1 already showed this
	// codebase's winding convention is subtle. Recorded as a Deliberate Decision.
	rs->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
	Ref<RDPipelineMultisampleState> ms;
	ms.instantiate();
	Ref<RDPipelineDepthStencilState> ds;
	ds.instantiate();
	ds->set_enable_depth_test(true);
	ds->set_enable_depth_write(true);
	// Reverse-Z (M1 errata 2): near = 1.0, far = 0.0, so "nearer" is "greater". This is the
	// same state CompositePass uses, which is exactly why the raymarched near field and the
	// LoD far field occlude each other correctly with no compositing rule at all.
	ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
	Ref<RDPipelineColorBlendStateAttachment> att;
	att.instantiate();
	att->set_enable_blend(false);
	Ref<RDPipelineColorBlendState> cb;
	cb.instantiate();
	cb->set_attachments(Array::make(att));

	pipeline_ = rd->render_pipeline_create(shader_, fb_format_, vertex_format,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
	pipeline_vertex_format_ = vertex_format;
	return pipeline_.is_valid();
}

bool LodRasterPass::ensure_args(RenderingDevice *rd, int capacity) {
	if (args_.is_valid() && args_capacity_ >= capacity) return true;
	if (args_.is_valid()) rd->free_rid(args_);
	args_ = RID();
	// draw_list_draw_indirect requires the DISPATCH_INDIRECT usage bit on its argument
	// buffer (docs/api/renderingdevice.md) -- the same flag compute indirect uses.
	args_ = rd->storage_buffer_create(static_cast<int64_t>(capacity) * kIndirectStride,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	args_capacity_ = args_.is_valid() ? capacity : 0;
	return args_.is_valid();
}

bool LodRasterPass::ensure_uniform_set(RenderingDevice *rd, RID records, RID texture) {
	if (uset_.is_valid() && records == uset_records_ && texture == uset_texture_) return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u0->set_binding(0);
	u0->add_id(records);
	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u1->set_binding(1);
	u1->add_id(sampler_);
	u1->add_id(texture);
	// Binding 0 is declared only by the vertex stage and binding 1 only by the fragment
	// stage; shader_create_from_spirv unions the two stages' reflection into one set 0.
	uset_ = rd->uniform_set_create(Array::make(u0, u1), shader_, 0);
	uset_records_ = records;
	uset_texture_ = texture;
	return uset_.is_valid();
}

int LodRasterPass::draw(RenderingDevice *rd, RID dst_color, RID dst_depth, LodPool &pool,
		const std::vector<ve::LodDrawPage> &draws, const Projection &view_proj,
		const Vector3 &cam_pos, float fade_scale) {
	last_drawn_ = 0;
	last_culled_ = 0;
	last_cpu_ms_ = 0.0f;
	if (!rd || !shader_.is_valid() || !pool.is_valid() || draws.empty()) return 0;
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();

	float m[16];
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) m[c * 4 + r] = static_cast<float>(view_proj.columns[c][r]);
	ve::LodFrustum frustum{};
	ve::lod_frustum_from_view_proj(m, &frustum);

	visible_.assign(draws.size(), 0);
	const int n = ve::lod_cull_pages(frustum, draws.data(),
			static_cast<int>(draws.size()), visible_.data());
	last_culled_ = static_cast<int>(draws.size()) - n;
	if (n == 0) {
		last_cpu_ms_ = (Time::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
		return 0;
	}

	if (!ensure_pipeline(rd, dst_color, dst_depth, pool.vertex_format())) return 0;
	if (!ensure_args(rd, pool.page_count())) return 0;
	if (!ensure_uniform_set(rd, pool.records_buffer(), pool.texture())) return 0;

	args_scratch_.assign(static_cast<size_t>(n) * 5, 0u);
	for (int i = 0; i < n; i++) {
		const ve::LodDrawPage &d = draws[visible_[i]];
		uint32_t *a = args_scratch_.data() + static_cast<size_t>(i) * 5;
		a[0] = static_cast<uint32_t>(d.quad_count) * 6u;           // index count
		a[1] = 1u;                                                 // instance count
		a[2] = 0u;                                                 // first index
		a[3] = static_cast<uint32_t>(d.page) * ve::kLodVertsPerPage; // vertex offset
		a[4] = 0u;                                                 // first instance
	}
	PackedByteArray ab;
	ab.resize(static_cast<int64_t>(n) * kIndirectStride);
	std::memcpy(ab.ptrw(), args_scratch_.data(), static_cast<size_t>(n) * kIndirectStride);

	// Device-level commands: both of these MUST precede draw_list_begin.
	pool.write_records(rd, draws);
	rd->buffer_update(args_, 0, ab.size(), ab);

	PackedByteArray pc;
	pc.resize(80);
	{
		float *f = reinterpret_cast<float *>(pc.ptrw());
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				f[c * 4 + r] = static_cast<float>(view_proj.columns[c][r]);
		f[16] = static_cast<float>(cam_pos.x);
		f[17] = static_cast<float>(cam_pos.y);
		f[18] = static_cast<float>(cam_pos.z);
		f[19] = fade_scale;
	}

	// DRAW_DEFAULT_ALL = load colour, store colour, load depth, store depth: the scene's
	// contents (and the composite's depth, written moments earlier) must survive.
	const int64_t dl = rd->draw_list_begin(framebuffer_, RenderingDevice::DRAW_DEFAULT_ALL);
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	rd->draw_list_bind_vertex_array(dl, pool.vertex_array());
	rd->draw_list_bind_index_array(dl, pool.index_array());
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	// One call, every level. The page slot reaches the vertex shader through the args'
	// vertex offset alone (see lod.vert.glsl).
	rd->draw_list_draw_indirect(dl, true, args_, 0, n, kIndirectStride);
	rd->draw_list_end();

	last_drawn_ = n;
	last_cpu_ms_ = (Time::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
	return n;
}
```

- [ ] **Step 13: Wire the two `VoxelWorld` hooks**

In `extension/src/voxel_world.h`, next to `lod_pool_`:

```cpp
	LodRasterPass *lod_raster_ = nullptr;
	// The cut the LoD tick published for the render thread to draw. Written on the main
	// thread under lod_mutex_, copied on the render thread; Task 10 fills it every frame,
	// Task 8's debug_lod_stage fills it by hand.
	std::vector<ve::LodDrawPage> lod_draws_;
	mutable std::mutex lod_mutex_;
	// Offscreen targets for debug_lod_render only; never allocated in a real frame.
	RID lod_test_color_, lod_test_depth_;
	int lod_test_w_ = 0, lod_test_h_ = 0;
```

and in the public section:

```cpp
	LodRasterPass *lod_raster() { return lod_raster_; }
	// Copies the published cut for the render thread. Returns how many pages it wrote.
	int copy_lod_draws(std::vector<ve::LodDrawPage> *out) const;

	// --- M5 Task 8 hooks ---
	// Builds one LoD chunk synchronously, uploads its pages into the pool and appends the
	// resulting draw pages to lod_draws_. Returns {ok, pages, quads, empty}.
	Dictionary debug_lod_stage(int level, Vector3i chunk);
	// Renders the published cut into an offscreen RGBA8 target and reads it back. Returns
	// {ok, image, width, height, drawn, culled, cpu_ms}.
	Dictionary debug_lod_render(Vector3 cam_pos, Vector3 look_dir, int width, int height,
			bool fade);
```

`debug_lod_stage` reuses Task 6's synchronous path exactly:

```cpp
Dictionary VoxelWorld::debug_lod_stage(int level, Vector3i chunk) {
	Dictionary d;
	d["ok"] = false;
	if (!lod_pool_ || !lod_pool_->is_valid() || !mesh_) return d;
	LodBuildJob job;
	job.level = level;
	job.chunk = ve::IVec3{chunk.x, chunk.y, chunk.z};
	{
		std::lock_guard<std::mutex> lock(edit_mutex_);
		ve::lod_ops_for_chunk(*edit_log_, level, job.chunk, &job.ops);
	}
	LodBuildResult res;
	bool built = false;
	// run_sync is the established way to reach the worker's device from the main thread
	// (MeshService, M3); debug_lod_build in Task 6 uses exactly this call.
	if (!mesh_->run_sync([&](MeshPass &pass) { built = pass.lod_build_sync(job, &res); }))
		return d;
	if (!built || res.failed) return d;
	d["empty"] = res.empty;
	if (res.empty) {
		d["ok"] = true;
		d["pages"] = 0;
		d["quads"] = 0;
		return d;
	}
	int quads = 0;
	std::lock_guard<std::mutex> lock(lod_mutex_);
	for (size_t i = 0; i < res.build.pages.size(); i++) {
		const int slot = static_cast<int>(lod_draws_.size());
		if (slot >= lod_pool_->page_count()) break;
		const ve::LodPageBuild &pg = res.build.pages[i];
		if (!lod_pool_->upload_page(rd(), slot, pg.vertices.data(),
					static_cast<int64_t>(pg.vertices.size()),
					res.page_texels.data() + i * ve::kLodPageTexBytes)) {
			return d;
		}
		ve::LodDrawPage dp{};
		dp.page = slot;
		dp.level = level;
		dp.chunk = job.chunk;
		ve::lod_chunk_aabb(level, job.chunk, dp.lo, dp.hi);
		for (int a = 0; a < 3; a++) dp.quant_lo[a] = res.build.quant_lo[a];
		dp.quant_size = res.build.quant_size;
		dp.quad_count = pg.quad_count;
		lod_draws_.push_back(dp);
		quads += pg.quad_count;
	}
	d["ok"] = true;
	d["pages"] = static_cast<int>(res.build.pages.size());
	d["quads"] = quads;
	return d;
}
```

`debug_lod_render` builds the same shape of matrix the compositor is handed, so what the test sees is what a frame will see:

```cpp
Dictionary VoxelWorld::debug_lod_render(Vector3 cam_pos, Vector3 look_dir, int width,
		int height, bool fade) {
	Dictionary d;
	d["ok"] = false;
	RenderingDevice *device = rd();
	if (!device || !lod_raster_ || !lod_pool_ || width <= 0 || height <= 0) return d;

	if (width != lod_test_w_ || height != lod_test_h_ || !lod_test_color_.is_valid()) {
		for (RID *r : {&lod_test_color_, &lod_test_depth_}) {
			if (r->is_valid()) device->free_rid(*r);
			*r = RID();
		}
		Ref<RDTextureFormat> cf;
		cf.instantiate();
		cf->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
		cf->set_width(width);
		cf->set_height(height);
		// CAN_COPY_FROM for texture_get_data, CAN_COPY_TO because texture_clear requires it.
		cf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
		Ref<RDTextureFormat> df;
		df.instantiate();
		df->set_format(RenderingDevice::DATA_FORMAT_D32_SFLOAT);
		df->set_width(width);
		df->set_height(height);
		df->set_usage_bits(RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
		Ref<RDTextureView> tv;
		tv.instantiate();
		lod_test_color_ = device->texture_create(cf, tv, TypedArray<PackedByteArray>());
		lod_test_depth_ = device->texture_create(df, tv, TypedArray<PackedByteArray>());
		lod_test_w_ = width;
		lod_test_h_ = height;
	}
	if (!lod_test_color_.is_valid() || !lod_test_depth_.is_valid()) return d;
	// Black colour, depth 0.0 = the reverse-Z FAR value, so any LoD fragment passes
	// GREATER_OR_EQUAL exactly as it does against a cleared scene depth buffer.
	// (If texture_clear refuses the D32_SFLOAT target on this driver, clear instead by
	// opening a throwaway draw list on a framebuffer over the same pair with
	// DRAW_CLEAR_ALL and a clear_depth_value of 0.0, then draw_list_end. Same result,
	// one more RID to free.)
	device->texture_clear(lod_test_color_, Color(0, 0, 0, 1), 0, 1, 0, 1);
	device->texture_clear(lod_test_depth_, Color(0, 0, 0, 0), 0, 1, 0, 1);

	const Vector3 fwd = look_dir.normalized();
	const Vector3 up = std::fabs(fwd.y) > 0.99f ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
	const Transform3D cam(Basis::looking_at(fwd, up), cam_pos);
	const float aspect = static_cast<float>(width) / static_cast<float>(height);
	// Godot's scene projection is create_depth_correction(true) * perspective: flip-y,
	// remap z to [0,1] and REVERSE it (near = 1, far = 0). Reproducing it here is what makes
	// this offscreen render answer questions about the real frame (M1 errata 2).
	const Projection proj = Projection::create_depth_correction(true) *
			Projection::create_perspective(60.0, aspect, 0.1, 4000.0);
	const Projection view_proj = proj * Projection(cam.affine_inverse());

	std::vector<ve::LodDrawPage> draws;
	copy_lod_draws(&draws);
	const int drawn = lod_raster_->draw(device, lod_test_color_, lod_test_depth_, *lod_pool_,
			draws, view_proj, cam_pos, fade ? 1.0f : 0.0f);
	// A local device only runs what has been submitted; the main device is already pumped by
	// the frame. Both are safe here because nothing else holds a list open.
	if (local_rd_) {
		local_rd_->submit();
		local_rd_->sync();
	}
	d["ok"] = true;
	d["image"] = device->texture_get_data(lod_test_color_, 0);
	d["width"] = width;
	d["height"] = height;
	d["drawn"] = drawn;
	d["culled"] = lod_raster_->last_culled();
	d["cpu_ms"] = lod_raster_->last_cpu_ms();
	return d;
}
```

Task 7's `debug_init_lod` gains `lod_raster_ = new LodRasterPass(); lod_raster_->initialize(rd());` after the pool, and `debug_teardown_lod` deletes it **before** the pool (its uniform set and framebuffer reference the pool's RIDs). `debug_init_lod` also calls `ensure_initialized()` first and returns false if that fails — the LoD needs the world's device, and from Task 9 it also needs `composite_pass_`, both of which `ensure_initialized()` is what creates. `debug_lod_stage` and `debug_lod_render` are bound in `_bind_methods` alongside the Task 7 hooks.

- [ ] **Step 14: Run the render test to verify it passes**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_render.gd`
Expected: PASS — all five cases. If `test_the_image_is_the_right_way_up` fails with the terrain in the *top* half, the depth correction's y-flip has been applied twice: `Projection::create_depth_correction(true)` already flips, so the view matrix must not.

- [ ] **Step 15: Run the whole suite**

Run: `cd extension && scons test && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: PASS — in particular `test_raymarch_pixel.gd` and `test_island_render.gd`, which are what prove Step 6's shading refactor was a refactor.

- [ ] **Step 16: Commit**

```bash
git add shaders/common.glslh shaders/raymarch.comp.glsl shaders/lod.vert.glsl \
        shaders/lod.frag.glsl extension/src/render/lod_raster_pass.h \
        extension/src/render/lod_raster_pass.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_render.gd
git commit -m "feat(lod): indirect multi-draw raster pass with shared terrain shading"
```

---

### Task 9: the dithered near/far seam, and the LoD draw inside the frame

Spec §3's *Near/far seam*: "Dithered depth fade over 120–150 m, cross-fading into LoD." Two shaders, one threshold, opposite directions. This task also puts the LoD draw into the real frame — after the composite, still pre-opaque — and opens the two handoffs the orchestration in Task 10 will fill: pages from the main thread to the render thread, and the camera from the render thread back.

**Files:**
- Modify: `shaders/composite.frag.glsl` (the dithered depth drop)
- Modify: `extension/src/render/composite_pass.h`, `extension/src/render/composite_pass.cpp` (camera + fade in the push constant)
- Modify: `extension/src/raymarch_compositor.cpp` (page uploads, camera publication, the LoD draw)
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (`note_camera`, `publish_lod_draws`, `queue_lod_upload`, `drain_lod_uploads`, `lod_fade_scale`, `debug_composite_probe`)
- Create: `tests/test_lod_fade.gd`

**Interfaces:**
- Consumes: `bayer4`, `lod_fade` (`shaders/common.glslh`); `godot::LodRasterPass::draw`, `godot::LodPool` (Tasks 7–8).
- Produces:
  - `CompositePass::draw(RenderingDevice *, RID dst_color, RID dst_depth, RID src_color, RID src_hitpos, const Projection &view_proj, const Vector3 &cam_pos, float fade_scale)` — **the signature grows by two arguments**
  - `struct godot::LodUpload { int slot; std::vector<uint8_t> vertices; std::vector<uint8_t> texels; }`
  - `void VoxelWorld::note_camera(const Vector3 &pos, float px_per_radian)`
  - `bool VoxelWorld::camera_known() const`, `Vector3 VoxelWorld::camera_pos() const`, `float VoxelWorld::camera_px_per_radian() const`
  - `void VoxelWorld::publish_lod_draws(std::vector<ve::LodDrawPage> draws)`
  - `void VoxelWorld::queue_lod_upload(int slot, const uint8_t *verts, int64_t vert_bytes, const uint8_t *texels)`
  - `int VoxelWorld::drain_lod_uploads(RenderingDevice *)`
  - `float VoxelWorld::lod_fade_scale() const`
  - `VoxelWorld::debug_composite_probe`

- [ ] **Step 1: Write the failing test**

Create `tests/test_lod_fade.gd`:

```gdscript
extends GdUnitTestSuite

# Spec section 3's load-bearing sentence: "Dithered depth fade over 120-150 m, cross-fading
# into LoD." The property that makes it work is not that each side fades -- it is that the
# two masks are EXACT COMPLEMENTS, so in the band every pixel is owned once: never twice
# (which would z-fight) and never zero times (which would be a hole).
#
# The near field is stood in for by a synthetic hitpos texture holding one constant world
# point, i.e. a flat wall square-on to the camera at a chosen distance, coloured pure blue.
# Nothing the LoD shading can produce is pure blue, so counting pixels by colour separates
# the two owners exactly.

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
	w.max_lod_pages = 8
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_lod()).is_true()
	# Both chunks either side of the y = 51.2 surface, exactly as tests/test_lod_render.gd
	# stages them: the terrain straddles that L1 boundary.
	var quads := 0
	for c in [Vector3i(1, 1, 1), Vector3i(1, 2, 1)]:
		var d: Dictionary = w.debug_lod_stage(0, c)
		assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
		quads += int(d.get("quads", 0))
	assert_int(quads).is_greater(0)
	return w

const W := 128
const H := 128

# The chunk at (1,1,1) spans (25.6, 25.6, 25.6) to (51.2, 51.2, 51.2); its top face is the
# terrain surface. Look at its centre from a chosen distance along +z, tilted down.
func eye_at(dist: float) -> Vector3:
	return Vector3(38.4, 51.2, 38.4) + Vector3(0.0, 0.35, 1.0).normalized() * dist

const DIR := Vector3(0.0, -0.35, -1.0)

func classify(img: PackedByteArray) -> Dictionary:
	var near := 0
	var lod := 0
	var empty := 0
	var i := 0
	while i < img.size():
		var r := img[i]
		var g := img[i + 1]
		var b := img[i + 2]
		if r == 0 and g == 0 and b == 0:
			empty += 1
		elif r < 8 and g < 8 and b > 200:
			near += 1
		else:
			lod += 1
		i += 4
	return {"near": near, "lod": lod, "empty": empty}

func test_before_the_band_the_near_field_owns_every_pixel(timeout := 60000) -> void:
	var w := make_world()
	var r: Dictionary = w.debug_composite_probe(eye_at(100.0), DIR, W, H, 100.0)
	assert_bool(r.get("ok", false)).override_failure_message(str(r)).is_true()
	var c := classify(r["image"])
	# 100 m is inside kLodFadeStartM, so lod_fade is 0 and the LoD discards on every pixel.
	assert_int(c["lod"]).is_equal(0)
	assert_int(c["near"]).is_equal(W * H)

func test_past_the_band_the_lod_owns_every_pixel_it_covers(timeout := 60000) -> void:
	var w := make_world()
	var r: Dictionary = w.debug_composite_probe(eye_at(200.0), DIR, W, H, 200.0)
	assert_bool(r["ok"]).is_true()
	var c := classify(r["image"])
	# Past kLodFadeEndM the composite drops its depth everywhere, so the LoD wins on every
	# pixel it covers and the near field keeps only the rest. A 25.6 m chunk at 200 m
	# subtends 0.128 rad of a 1.047 rad fov: ~16 of 128 pixels per side, so a few dozen
	# covered pixels, not hundreds.
	assert_int(c["lod"]).is_greater(30)
	assert_int(c["lod"] + c["near"]).is_equal(W * H)

func test_inside_the_band_every_covered_pixel_is_owned_exactly_once(timeout := 60000) -> void:
	var w := make_world()
	var eye := eye_at(135.0)
	# The set of pixels the chunk covers at all: same camera, fade forced off.
	var full: Dictionary = w.debug_lod_render(eye, DIR, W, H, false)
	assert_bool(full["ok"]).is_true()
	var covered: PackedByteArray = full["image"]

	var r: Dictionary = w.debug_composite_probe(eye, DIR, W, H, 135.0)
	assert_bool(r["ok"]).is_true()
	var img: PackedByteArray = r["image"]

	var covered_n := 0
	var lod_n := 0
	var near_n := 0
	var black_n := 0
	var i := 0
	while i < covered.size():
		if covered[i] > 0 or covered[i + 1] > 0 or covered[i + 2] > 0:
			covered_n += 1
			var rr := img[i]
			var gg := img[i + 1]
			var bb := img[i + 2]
			if rr == 0 and gg == 0 and bb == 0:
				black_n += 1
			elif rr < 8 and gg < 8 and bb > 200:
				near_n += 1
			else:
				lod_n += 1
		i += 4
	assert_int(covered_n).is_greater(60)
	# The whole point: no pixel in the band is left to nobody.
	assert_int(black_n).is_equal(0)
	assert_int(lod_n + near_n).is_equal(covered_n)
	# And it really is a dither, not a switch.
	assert_int(lod_n).is_greater(0)
	assert_int(near_n).is_greater(0)

func test_a_disabled_fade_leaves_the_depth_test_to_arbitrate(timeout := 60000) -> void:
	var w := make_world()
	w.lod_fade_enabled = false
	var eye := eye_at(200.0)
	# fade scale 0 means the composite keeps ALL its depth and the LoD discards nothing, so
	# the two meet on depth alone. A wall 50 m in front of the chunk must win everywhere...
	var infront: Dictionary = w.debug_composite_probe(eye, DIR, W, H, 150.0)
	assert_bool(infront["ok"]).is_true()
	var a := classify(infront["image"])
	assert_int(a["near"]).is_equal(W * H)
	assert_int(a["lod"]).is_equal(0)
	# ...and a wall 200 m BEHIND it must lose wherever the chunk covers a pixel. Same frame,
	# same shaders, opposite outcome: that is the depth test doing the work, not the fade.
	var behind: Dictionary = w.debug_composite_probe(eye, DIR, W, H, 400.0)
	var b := classify(behind["image"])
	assert_int(b["lod"]).is_greater(30)
	assert_int(b["lod"] + b["near"]).is_equal(W * H)
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_fade.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_composite_probe'`.

- [ ] **Step 3: Add the dithered depth drop to the composite shader**

Rewrite `shaders/composite.frag.glsl`:

```glsl
#[fragment]
#version 460
#include "common.glslh"
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0) uniform sampler2D src_color;  // linear (0.66x upscale)
layout(set = 0, binding = 1) uniform sampler2D src_hitpos; // nearest (no silhouette smear)
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam_pos; // xyz = camera world position, w = fade scale (1 = fade on, 0 = off)
} pc;
void main() {
	vec4 hp = texture(src_hitpos, uv_in);
	frag_color = texture(src_color, uv_in);
	if (hp.w < 0.5) {
		// Sky: farthest. Godot 4.7 renders with reverse-Z (near=1.0, far=0.0 — the scene
		// projection is depth-corrected with remap_z+reverse_z), so the farthest depth is
		// 0.0, and the composite pipeline tests GREATER_OR_EQUAL so nearer scene geometry
		// (opaque objects' pre-pass depth) is never overwritten.
		gl_FragDepth = 0.0;
		return;
	}
	// Spec section 3's near/far seam. shaders/lod.frag.glsl discards where
	// bayer4 >= f; this drops its DEPTH where bayer4 < f. Exact complements on one
	// full-resolution pixel grid, from one shared lod_fade().
	//
	// The fade scale enters the two shaders DIFFERENTLY, because "band off" means opposite
	// things to them: this side wants to keep all its depth (f -> 0, multiply), the LoD side
	// wants to draw every pixel (f -> 1, mix towards 1). Getting this backwards is the one
	// way to make the seam fail silently, so the table in the plan's Task 9 spells it out.
	//
	// The COLOUR is written either way, deliberately: where the LoD has not streamed in yet
	// the pixel then shows terrain at a stale depth rather than sky at the right one, which
	// is the safe direction (spec section 8).
	float f = lod_fade(distance(hp.xyz, pc.cam_pos.xyz)) * pc.cam_pos.w;
	if (bayer4(ivec2(gl_FragCoord.xy)) < f) {
		gl_FragDepth = 0.0;
		return;
	}
	vec4 clip = pc.view_proj * vec4(hp.xyz, 1.0);
	// The scene projection already outputs NDC z in [0,1] with near->1, far->0 (reverse-Z),
	// so no *0.5+0.5 remap: that would both compress the range to [0.5,1.0] and invert the
	// convention (brief's GL-style [-1,1] assumption does not hold on Godot 4.7.1).
	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
```

- [ ] **Step 4: Grow the composite's push constant**

In `extension/src/render/composite_pass.h`, add `#include <godot_cpp/variant/vector3.hpp>` and change the declaration to:

```cpp
	// cam_pos and fade_scale are M5's: the fragment shader needs the camera to measure the
	// 120-150 m band, and the scale is the shared on/off that keeps this mask and
	// shaders/lod.frag.glsl's mask exact complements.
	void draw(RenderingDevice *rd, RID dst_color, RID dst_depth,
			RID src_color, RID src_hitpos, const Projection &view_proj,
			const Vector3 &cam_pos, float fade_scale);
```

In `extension/src/render/composite_pass.cpp`, change the signature to match and replace the push-constant block:

```cpp
	PackedByteArray pc;
	pc.resize(80);
	{
		float *f = reinterpret_cast<float *>(pc.ptrw());
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				f[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
		f[16] = static_cast<float>(cam_pos.x);
		f[17] = static_cast<float>(cam_pos.y);
		f[18] = static_cast<float>(cam_pos.z);
		f[19] = fade_scale;
	}
```

- [ ] **Step 5: Add the two handoffs and the fade gate to `VoxelWorld`**

In `extension/src/voxel_world.h`, beside the LoD members Task 8 added:

```cpp
	bool lod_fade_enabled_ = true;
	// Bytes on their way to the page pool. Filled by the main thread's LoD tick, drained on
	// the render thread before anything opens a list -- the same shape as island_uploads_.
	struct LodUpload {
		int slot = -1;
		std::vector<uint8_t> vertices;
		std::vector<uint8_t> texels;
	};
	std::vector<LodUpload> lod_uploads_; // guarded by lod_mutex_
	// Published by the render thread, read by the main thread's LoD tick. Atomics rather
	// than a mutex because a frame of latency in LEVEL SELECTION is invisible, and because
	// the render thread must never block on the main thread's tick.
	std::atomic<float> cam_x_{0.0f}, cam_y_{0.0f}, cam_z_{0.0f};
	std::atomic<float> cam_px_per_radian_{0.0f};
	std::atomic<bool> cam_known_{false};
	// debug_composite_probe's synthetic near field. Allocated on first use, freed by
	// debug_teardown_lod; never touched in a real frame.
	RID lod_probe_color_, lod_probe_hitpos_;
	Vector3 lod_probe_wall_;
	bool ensure_lod_test_targets(RenderingDevice *device, int width, int height);
	bool ensure_lod_probe_source(RenderingDevice *device, int width, int height,
			const Vector3 &wall);
```

`lod_fade_enabled` is bound as a **property**, not just a method pair, because `tests/test_lod_fade.gd` assigns it directly:

```cpp
	ClassDB::bind_method(D_METHOD("set_lod_fade_enabled", "v"), &VoxelWorld::set_lod_fade_enabled);
	ClassDB::bind_method(D_METHOD("get_lod_fade_enabled"), &VoxelWorld::get_lod_fade_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lod_fade_enabled"), "set_lod_fade_enabled",
			"get_lod_fade_enabled");
```

public:

```cpp
	void set_lod_fade_enabled(bool v) { lod_fade_enabled_ = v; }
	bool get_lod_fade_enabled() const { return lod_fade_enabled_; }
	// 1.0 when the cross-fade should run, 0.0 when it must not. Both the composite and the
	// LoD raster take THIS value, so they cannot disagree about who owns a pixel. It is 0
	// until the LoD has something to show: fading the near field out into an empty pool
	// would be a hole, and a hole is never the safe direction.
	float lod_fade_scale() const;

	void note_camera(const Vector3 &pos, float px_per_radian);
	bool camera_known() const { return cam_known_.load(std::memory_order_relaxed); }
	Vector3 camera_pos() const;
	float camera_px_per_radian() const {
		return cam_px_per_radian_.load(std::memory_order_relaxed);
	}

	void publish_lod_draws(std::vector<ve::LodDrawPage> draws);
	void queue_lod_upload(int slot, const uint8_t *verts, int64_t vert_bytes,
			const uint8_t *texels);
	// Render thread. Returns how many pages landed.
	int drain_lod_uploads(RenderingDevice *device);
	LodPool *lod_pool() { return lod_pool_; }

	// --- M5 Task 9 hook ---
	// Renders a synthetic near field (a flat wall of pure blue at `hit_distance`) through
	// CompositePass and then the published LoD cut through LodRasterPass, into the same
	// offscreen pair debug_lod_render uses. Returns {ok, image, width, height}.
	Dictionary debug_composite_probe(Vector3 cam_pos, Vector3 look_dir, int width,
			int height, float hit_distance);
```

In `extension/src/voxel_world.cpp`:

```cpp
float VoxelWorld::lod_fade_scale() const {
	if (!lod_fade_enabled_) return 0.0f;
	if (!lod_pool_ || !lod_pool_->is_valid()) return 0.0f;
	std::lock_guard<std::mutex> lock(lod_mutex_);
	return lod_draws_.empty() ? 0.0f : 1.0f;
}

void VoxelWorld::note_camera(const Vector3 &pos, float px_per_radian) {
	cam_x_.store(static_cast<float>(pos.x), std::memory_order_relaxed);
	cam_y_.store(static_cast<float>(pos.y), std::memory_order_relaxed);
	cam_z_.store(static_cast<float>(pos.z), std::memory_order_relaxed);
	cam_px_per_radian_.store(px_per_radian, std::memory_order_relaxed);
	cam_known_.store(true, std::memory_order_release);
}

Vector3 VoxelWorld::camera_pos() const {
	return Vector3(cam_x_.load(std::memory_order_relaxed),
			cam_y_.load(std::memory_order_relaxed),
			cam_z_.load(std::memory_order_relaxed));
}

void VoxelWorld::publish_lod_draws(std::vector<ve::LodDrawPage> draws) {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	lod_draws_ = std::move(draws);
}

int VoxelWorld::copy_lod_draws(std::vector<ve::LodDrawPage> *out) const {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	*out = lod_draws_;
	return static_cast<int>(out->size());
}

void VoxelWorld::queue_lod_upload(int slot, const uint8_t *verts, int64_t vert_bytes,
		const uint8_t *texels) {
	if (slot < 0) return;
	LodUpload u;
	u.slot = slot;
	if (verts && vert_bytes > 0) u.vertices.assign(verts, verts + vert_bytes);
	if (texels) u.texels.assign(texels, texels + ve::kLodPageTexBytes);
	std::lock_guard<std::mutex> lock(lod_mutex_);
	lod_uploads_.push_back(std::move(u));
}

int VoxelWorld::drain_lod_uploads(RenderingDevice *device) {
	if (!device || !lod_pool_ || !lod_pool_->is_valid()) return 0;
	std::vector<LodUpload> batch;
	{
		std::lock_guard<std::mutex> lock(lod_mutex_);
		batch.swap(lod_uploads_);
	}
	int landed = 0;
	for (const LodUpload &u : batch) {
		// Fail-soft: a refused page leaves the pool holding whatever it held, and the
		// residency still believes the page is resident. The next rebuild overwrites it.
		if (lod_pool_->upload_page(device, u.slot, u.vertices.data(),
					static_cast<int64_t>(u.vertices.size()),
					u.texels.empty() ? nullptr : u.texels.data())) {
			landed++;
		}
	}
	return landed;
}
```

`debug_composite_probe` reuses `debug_lod_render`'s offscreen pair and its projection, and adds the synthetic near field:

```cpp
Dictionary VoxelWorld::debug_composite_probe(Vector3 cam_pos, Vector3 look_dir, int width,
		int height, float hit_distance) {
	Dictionary d;
	d["ok"] = false;
	RenderingDevice *device = rd();
	if (!device || !composite_pass_ || !lod_raster_ || !lod_pool_) return d;
	if (!ensure_lod_test_targets(device, width, height)) return d;

	// One constant world point for every pixel: a wall square-on to the camera at exactly
	// hit_distance, which is what makes the band assertions one number instead of a range.
	const Vector3 fwd = look_dir.normalized();
	const Vector3 wall = cam_pos + fwd * hit_distance;
	if (!ensure_lod_probe_source(device, width, height, wall)) return d;

	device->texture_clear(lod_test_color_, Color(0, 0, 0, 1), 0, 1, 0, 1);
	device->texture_clear(lod_test_depth_, Color(0, 0, 0, 0), 0, 1, 0, 1);

	const Vector3 up = std::fabs(fwd.y) > 0.99f ? Vector3(0, 0, -1) : Vector3(0, 1, 0);
	const Transform3D cam(Basis::looking_at(fwd, up), cam_pos);
	const float aspect = static_cast<float>(width) / static_cast<float>(height);
	const Projection proj = Projection::create_depth_correction(true) *
			Projection::create_perspective(60.0, aspect, 0.1, 4000.0);
	const Projection view_proj = proj * Projection(cam.affine_inverse());
	const float fade = lod_fade_scale();

	// The real frame order, in miniature: composite first, LoD second, one framebuffer.
	composite_pass_->draw(device, lod_test_color_, lod_test_depth_, lod_probe_color_,
			lod_probe_hitpos_, view_proj, cam_pos, fade);
	std::vector<ve::LodDrawPage> draws;
	copy_lod_draws(&draws);
	lod_raster_->draw(device, lod_test_color_, lod_test_depth_, *lod_pool_, draws, view_proj,
			cam_pos, fade);
	if (local_rd_) {
		local_rd_->submit();
		local_rd_->sync();
	}
	d["ok"] = true;
	d["image"] = device->texture_get_data(lod_test_color_, 0);
	d["width"] = width;
	d["height"] = height;
	return d;
}
```

`ensure_lod_test_targets` is `debug_lod_render`'s allocation block lifted into a private helper (Task 8 wrote it inline; move it, do not copy it). `ensure_lod_probe_source` creates two `TEXTURE_USAGE_SAMPLING_BIT | TEXTURE_USAGE_CAN_UPDATE_BIT` textures the first time: `lod_probe_color_` (`DATA_FORMAT_R16G16B16A16_SFLOAT`, every texel pure blue) and `lod_probe_hitpos_` (`DATA_FORMAT_R32G32B32A32_SFLOAT`, every texel `(wall.x, wall.y, wall.z, 1.0)`), re-uploading the hitpos whenever `wall` changes. Both are freed in `debug_teardown_lod` alongside the offscreen pair.

- [ ] **Step 6: Wire the frame**

In `extension/src/raymarch_compositor.cpp`, add the includes for `render/lod_pool.h` and `render/lod_raster_pass.h`, then change two places.

Beside the existing volume drain, so every device-level upload happens before any list opens:

```cpp
	// Volumes before anything that evaluates the field: an op naming a slot may already be
	// in the edit log, and the streamer is about to regenerate the bricks that read it.
	world->drain_island_uploads(rd);
	// LoD pages likewise: buffer_update and texture_update are device-level commands and
	// are illegal inside an open list (Global Constraints).
	world->drain_lod_uploads(rd);
```

And replace the final composite call with the composite plus the LoD draw:

```cpp
	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	// Publish the camera for the main thread's LoD tick. px_per_radian is what turns a world
	// error into a screen error: a feature of size e at distance d subtends e/d radians, and
	// half the viewport height spans tan_y radians of tangent (M1 errata 3).
	world->note_camera(cam.origin, (size.y * 0.5f) / tan_y);

	const float fade = world->lod_fade_scale();
	// Master-API note: rsb->get_color_texture()/get_depth_texture() exist on godot-cpp master
	// (render_scene_buffers_rd.hpp) and return the non-MSAA internal color/depth textures —
	// the same RIDs the engine's own framebuffers use when MSAA is disabled (verified against
	// render_forward_clustered.cpp), so the composite writes into the actual scene buffers.
	cmp->draw(rd, rsb->get_color_texture(), rsb->get_depth_texture(),
			rmp->color_texture(), rmp->hitpos_texture(), view_proj, cam.origin, fade);

	// Spec section 7's frame order: the LoD raster goes into the same buffer, straight after
	// the near field and still pre-opaque, so Godot's opaque pass sees one merged depth
	// buffer and mutual occlusion is the depth test itself.
	LodRasterPass *lrp = world->lod_raster();
	LodPool *pool = world->lod_pool();
	if (lrp && lrp->is_valid() && pool && pool->is_valid()) {
		std::vector<ve::LodDrawPage> draws;
		world->copy_lod_draws(&draws);
		lrp->draw(rd, rsb->get_color_texture(), rsb->get_depth_texture(), *pool, draws,
				view_proj, cam.origin, fade);
	}
```

- [ ] **Step 7: Run the fade test to verify it passes**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_fade.gd`
Expected: PASS — all four cases. `test_inside_the_band_every_covered_pixel_is_owned_exactly_once` is the one that matters: if it reports `black_n > 0`, the two `lod_fade` calls disagree (check that both shaders take the distance to the same point — the LoD's interpolated world position and the composite's hit position are both world space); if it reports `lod_n + near_n > covered_n`, they are not complements and one comparison is `<=` where it should be `<`.

- [ ] **Step 8: Verify the near field is unchanged with LoD off**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_raymarch_pixel.gd -a res://tests/test_raymarch_magenta.gd -a res://tests/test_gpu_smoke.gd`
Expected: PASS — the three near-field suites are untouched by M5. `lod_fade_scale()` returns 0 for every world that never initialised a LoD pool, and this is the case the asymmetry in Step 3 exists for:

| `cam_pos.w` | `composite.frag.glsl` keeps its depth where | `lod.frag.glsl` keeps its pixel where |
|---|---|---|
| 1 (band on) | `bayer4 >= lod_fade(d)` | `bayer4 < lod_fade(d)` |
| 0 (band off) | everywhere (`f = lod_fade * 0 = 0`) | everywhere (`f = mix(1, lod_fade, 0) = 1`) |

The two rows are complements of each other in the first case and both permissive in the second, which is why the scale is a **multiply** on the composite side and a **mix towards 1** on the LoD side. If they were written the same way, turning the band off would blank the near field's depth on every pixel and nothing in the near-field suites would say why.

- [ ] **Step 9: Run the whole suite**

Run: `cd extension && scons test && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: PASS — every suite, including `test_lod_render.gd` from Task 8, which calls the same `lod_fade_scale`-independent path with an explicit boolean.

- [ ] **Step 10: Commit**

```bash
git add shaders/composite.frag.glsl extension/src/render/composite_pass.h \
        extension/src/render/composite_pass.cpp extension/src/raymarch_compositor.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_fade.gd
git commit -m "feat(lod): dithered 120-150 m cross-fade and the pre-opaque LoD draw"
```

---

### Task 10: `VoxelWorld` runs the LoD every frame

Everything built so far is inert until something calls it once per frame in the right order. This task adds the four exports, the real (non-debug) construction of the pool, the raster pass and the residency, the tick that turns a camera position into builds and draws, and spec §4's "Edit op → dirty intersecting chunks at all 4 levels".

**Files:**
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_lod_stream.gd`

**Interfaces:**
- Consumes: `ve::LodResidency`, `ve::LodFrame`, `ve::LodBuildRequest`, `ve::LodDrawPage` (`lod/lod_residency.h`); `ve::lod_ops_for_chunk`, `ve::op_lod_chunk_range`, `ve::lod_chunk_has_surface`, `ve::lod_chunk_aabb`, `ve::kLodLevels` (`lod/lod_grid.h`); `godot::LodBuildJob`, `LodBuildResult`, `MeshService::submit_lod/collect_lod/lod_busy/lod_available` (Task 6); `queue_lod_upload`, `publish_lod_draws`, `camera_pos`, `camera_px_per_radian` (Task 9).
- Produces:
  - Exports `lod_enabled` (bool, default `true`), `max_lod_pages` (int, default 512), `lod_builds_per_frame` (int, default 1), `lod_sse_threshold` (float, default 4.0)
  - `void VoxelWorld::lod_tick(const Vector3 &cam_pos, float px_per_radian)`
  - `Dictionary VoxelWorld::debug_lod_stats()`
  - `int VoxelWorld::debug_lod_tick(Vector3 cam_pos, float px_per_radian)`

- [ ] **Step 1: Write the failing test**

Create `tests/test_lod_stream.gd`:

```gdscript
extends GdUnitTestSuite

# Spec section 4's Streaming & invalidation paragraph, driven by hand so the assertions are
# about the POLICY rather than about how fast a GPU happens to be: debug_lod_tick runs one
# whole tick synchronously (collect, walk, submit, publish) and returns the pages published.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(pages := 64, builds := 4) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(16, 5, 16)
	w.lod_enabled = true
	w.max_lod_pages = pages
	w.lod_builds_per_frame = builds
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	assert_bool(w.is_initialized()).is_true()
	return w

# Half the world, 100 m up, looking down the diagonal: everything the walk can reach is in
# front of the camera, so the cut is not trimmed by a frustum it does not know about.
const EYE := Vector3(204.8, 150.0, 204.8)
const PPR := 540.0 # (1080 / 2) / tan(30 deg) — a 1080p 60-degree camera

func settle(w: VoxelWorld, ticks := 80) -> void:
	for i in range(ticks):
		w.debug_lod_tick(EYE, PPR)

func test_a_settled_camera_publishes_a_cut(timeout := 120000) -> void:
	var w := make_world()
	settle(w)
	var s: Dictionary = w.debug_lod_stats()
	assert_int(s["draws"]).is_greater(0)
	assert_int(s["pages_used"]).is_greater(0)
	assert_int(s["pages_used"]).is_less_equal(64)

func test_the_build_budget_is_respected(timeout := 120000) -> void:
	var w := make_world(64, 1)
	# One build per tick means the pages resident after N ticks can never exceed N (a chunk
	# may need two pages, so this is an upper bound on CHUNKS, checked through builds_done).
	for i in range(10):
		w.debug_lod_tick(EYE, PPR)
	var s: Dictionary = w.debug_lod_stats()
	assert_int(s["builds_done"]).is_less_equal(10)
	assert_int(s["builds_done"]).is_greater(0)

func test_the_page_pool_is_never_overdrawn(timeout := 120000) -> void:
	# Four pages is far less than the walk wants, which is the interesting case: spec
	# section 8's fail-soft says a coarser world, never a corrupt one.
	var w := make_world(4, 4)
	settle(w, 60)
	var s: Dictionary = w.debug_lod_stats()
	assert_int(s["pages_used"]).is_less_equal(4)
	assert_int(s["draws"]).is_greater(0)
	assert_int(s["draws"]).is_less_equal(4)

func test_the_coarsest_level_under_the_threshold_wins(timeout := 120000) -> void:
	var w := make_world()
	settle(w)
	var levels: PackedInt32Array = w.debug_lod_draw_levels()
	assert_int(levels.size()).is_greater(0)
	# Nothing is drawn at a level finer than L1 or coarser than L4, and a settled far camera
	# must be using more than one level or the CDLOD walk is not descending at all.
	var seen := {}
	for l in levels:
		assert_int(l).is_between(0, 3)
		seen[l] = true
	assert_int(seen.size()).is_greater(1)

func test_an_edit_dirties_the_chunk_at_every_level(timeout := 120000) -> void:
	var w := make_world()
	settle(w)
	var before: Dictionary = w.debug_lod_stats()
	# A 6 m sphere: big enough that L1, L2 and L3 all resolve it, small enough that L4's
	# 25.6 m cells cannot (lod_ops_for_chunk drops it there — a resolution argument, not a
	# distance one). The chunk is still marked dirty at all four levels.
	var centre := Vector3(204.8, 51.2, 204.8)
	# Returns the number of regions the append touched, so >0 means the edit landed.
	assert_int(w.debug_apply_sphere_subtract(centre, 6.0)).is_greater(0)
	var d: Dictionary = w.debug_lod_dirty_counts()
	for l in range(4):
		assert_int(d[str(l)]).is_greater(0)
	# And the tick actually rebuilds them rather than leaving them stale for ever.
	settle(w, 40)
	var after: Dictionary = w.debug_lod_stats()
	assert_int(after["builds_done"]).is_greater(before["builds_done"])

func test_disabling_lod_publishes_nothing(timeout := 120000) -> void:
	var w := make_world()
	settle(w, 20)
	assert_int(w.debug_lod_stats()["draws"]).is_greater(0)
	w.lod_enabled = false
	w.debug_lod_tick(EYE, PPR)
	var s: Dictionary = w.debug_lod_stats()
	assert_int(s["draws"]).is_equal(0)
	# Turning it back on must not need a re-init: the residency and the pool survived.
	w.lod_enabled = true
	settle(w, 20)
	assert_int(w.debug_lod_stats()["draws"]).is_greater(0)
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_stream.gd`
Expected: FAIL — `Invalid assignment of property 'lod_enabled'`.

- [ ] **Step 3: Add the exports and the members**

In `extension/src/voxel_world.h`:

```cpp
	bool lod_enabled_ = true;
	// One build per frame is the default because a build is spec section 4's "~1-4 ms GPU"
	// and it shares the worker device with collision meshing, which the player feels first.
	int lod_builds_per_frame_ = 1;
	float lod_sse_threshold_ = 4.0f; // ve::kLodSseThreshold
	ve::LodResidency *lod_residency_ = nullptr;
	// Ranges to mark dirty at each level, filled by append_edit under edit_mutex_ and drained
	// by the tick. Same shape as pending_dirty_ (M3): the residency is main-thread-only and
	// an edit can arrive from any tool.
	struct LodDirty {
		int level = 0;
		ve::IVec3 lo{}, hi{};
	};
	std::vector<LodDirty> pending_lod_dirty_; // guarded by edit_mutex_
	int64_t lod_builds_done_ = 0;
	int64_t lod_dirty_counts_[ve::kLodLevels] = {0, 0, 0, 0};
	std::vector<ve::EditOp> lod_op_scratch_;
```

with the usual setter/getter pairs and `ADD_PROPERTY` entries for `lod_enabled`, `max_lod_pages`, `lod_builds_per_frame` and `lod_sse_threshold`. `set_max_lod_pages` and `set_lod_sse_threshold` take effect at the next `ensure_initialized()`; the other two are read every tick.

Public:

```cpp
	// One frame of spec section 4's streaming: collect finished builds, walk the hierarchy,
	// submit what the walk wants, publish what it can draw.
	void lod_tick(const Vector3 &cam_pos, float px_per_radian);

	// --- M5 Task 10 hooks ---
	int debug_lod_tick(Vector3 cam_pos, float px_per_radian); // returns pages published
	Dictionary debug_lod_stats();
	PackedInt32Array debug_lod_draw_levels();
	Dictionary debug_lod_dirty_counts();
```

- [ ] **Step 4: Construct the LoD in `ensure_initialized` and tear it down in order**

In `ensure_initialized()`, after the atlas and the passes exist:

```cpp
	if (lod_enabled_ && !lod_pool_) {
		LodPoolConfig pc;
		pc.max_pages = max_lod_pages_;
		lod_pool_ = new LodPool();
		if (!lod_pool_->initialize(device, pc)) {
			// Fail-soft (spec section 8): a world with no far field still plays. The warning
			// names the dial, because "lower max_lod_pages" is the actual fix.
			UtilityFunctions::printerr("VoxelWorld: LoD page pool refused ", max_lod_pages_,
					" pages; far field disabled. Lower max_lod_pages.");
			delete lod_pool_;
			lod_pool_ = nullptr;
		}
	}
	if (lod_pool_ && !lod_raster_) {
		lod_raster_ = new LodRasterPass();
		lod_raster_->initialize(device);
	}
	if (lod_pool_ && !lod_residency_) {
		ve::LodResidencyConfig rc;
		rc.bounds = world_bounds();
		rc.max_pages = lod_pool_->page_count();
		rc.max_builds_per_frame = lod_builds_per_frame_;
		rc.max_probes_per_frame = 128;
		rc.sse_threshold = lod_sse_threshold_;
		rc.fade_start_m = ve::kLodFadeStartM;
		lod_residency_ = new ve::LodResidency(rc);
	}
```

In `teardown_gpu()`, **before** `composite_pass_` (the raster pass's uniform set and framebuffer reference the pool's RIDs, and the pool's RIDs live on the same device as the atlas):

```cpp
	if (lod_raster_) { delete lod_raster_; lod_raster_ = nullptr; }
	if (lod_pool_) { delete lod_pool_; lod_pool_ = nullptr; }
	// The residency's page slots name a pool that no longer exists. The CPU core survives a
	// re-init exactly as ve::RegionResidency does, but its slot assignments must not.
	if (lod_residency_) lod_residency_->clear();
	{
		std::lock_guard<std::mutex> lock(lod_mutex_);
		lod_draws_.clear();
		lod_uploads_.clear();
	}
```

and in `_exit_tree()`, after `teardown_gpu()`: `if (lod_residency_) { delete lod_residency_; lod_residency_ = nullptr; }`.

- [ ] **Step 5: Write the tick**

In `extension/src/voxel_world.cpp`:

```cpp
namespace {

// The residency's view of the world field at a LoD level. Same shape as
// collider_streamer.cpp's LogProbe, and the same warning applies: ve::lod_chunk_has_surface
// must be qualified or the name resolves to this override and recurses.
struct LodLogProbe : ve::LodProbe {
	// Owned, not borrowed: ve::AnalyticGenerator is stateless and every other call site in
	// voxel_world.cpp constructs one on the spot.
	ve::AnalyticGenerator gen;
	ve::EditLog *log = nullptr;
	std::mutex *mu = nullptr;
	const ve::VolumeStore *volumes = nullptr;
	mutable std::vector<ve::EditOp> scratch;

	bool lod_chunk_has_surface(int level, ve::IVec3 c) const override {
		int n = 0;
		{
			std::lock_guard<std::mutex> lock(*mu);
			n = ve::lod_ops_for_chunk(*log, level, c, &scratch);
		}
		return ve::lod_chunk_has_surface(gen, scratch.data(), n, level, c, volumes);
	}
};

} // namespace

void VoxelWorld::lod_tick(const Vector3 &cam_pos, float px_per_radian) {
	if (!lod_enabled_ || !lod_pool_ || !lod_residency_ || !mesh_) return;
	if (!(px_per_radian > 0.0f)) return;

	// 1. Land what the worker finished. Before the walk, so a chunk that arrived this frame
	//    is drawable this frame rather than next.
	std::vector<LodBuildResult> done;
	mesh_->collect_lod(&done);
	for (LodBuildResult &r : done) {
		if (r.failed) {
			lod_residency_->note_failed(r.level, r.chunk);
			continue;
		}
		if (r.empty || r.build.pages.empty()) {
			lod_residency_->note_empty(r.level, r.chunk);
			continue;
		}
		std::vector<int> quads;
		quads.reserve(r.build.pages.size());
		for (const ve::LodPageBuild &p : r.build.pages) quads.push_back(p.quad_count);
		const std::vector<int> slots = lod_residency_->note_built(r.level, r.chunk,
				static_cast<int>(r.build.pages.size()), quads.data());
		if (slots.empty()) continue; // pool refused; stays kNeedsBuild, asked for again
		for (size_t i = 0; i < slots.size() && i < r.build.pages.size(); i++) {
			const ve::LodPageBuild &p = r.build.pages[i];
			queue_lod_upload(slots[i], p.vertices.data(),
					static_cast<int64_t>(p.vertices.size()),
					r.page_texels.data() + i * ve::kLodPageTexBytes);
		}
		lod_builds_done_++;
	}

	// 2. Apply invalidation before the walk, so an edited chunk is re-requested this frame.
	{
		std::vector<LodDirty> dirty;
		{
			std::lock_guard<std::mutex> lock(edit_mutex_);
			dirty.swap(pending_lod_dirty_);
		}
		for (const LodDirty &d : dirty) lod_residency_->mark_dirty(d.level, d.lo, d.hi);
	}

	// 3. The walk.
	LodLogProbe probe;
	probe.log = edit_log_;
	probe.mu = &edit_mutex_;
	probe.volumes = &volumes_;
	const float cam[3] = {static_cast<float>(cam_pos.x), static_cast<float>(cam_pos.y),
			static_cast<float>(cam_pos.z)};
	ve::LodFrame frame = lod_residency_->update(cam, px_per_radian, probe);

	// 4. Submit. update() has already moved every emitted request to kBuilding, so each one
	//    is either submitted or immediately failed back -- there is no third option, and a
	//    request quietly dropped here would be a chunk that never builds again.
	if (!frame.builds.empty()) {
		std::vector<LodBuildJob> jobs;
		if (!mesh_->lod_busy() && mesh_->lod_available()) {
			jobs.reserve(frame.builds.size());
			std::lock_guard<std::mutex> lock(edit_mutex_);
			for (const ve::LodBuildRequest &b : frame.builds) {
				LodBuildJob j;
				j.level = b.level;
				j.chunk = b.chunk;
				ve::lod_ops_for_chunk(*edit_log_, b.level, b.chunk, &lod_op_scratch_);
				j.ops = lod_op_scratch_;
				jobs.push_back(std::move(j));
			}
		}
		if (jobs.empty() || !mesh_->submit_lod(std::move(jobs))) {
			for (const ve::LodBuildRequest &b : frame.builds)
				lod_residency_->note_failed(b.level, b.chunk);
		}
	}

	// 5. Publish the cut for the render thread.
	publish_lod_draws(std::move(frame.draws));
}
```

and in `_process`, before the physics block, so the LoD keeps streaming in a world with physics disabled:

```cpp
	if (lod_enabled_ && camera_known()) lod_tick(camera_pos(), camera_px_per_radian());
```

- [ ] **Step 6: Dirty all four levels on every edit**

In `append_edit_locked` — the caller already holds `edit_mutex_`, which is also what guards `pending_lod_dirty_` — beside the existing `pending_dirty_` push:

```cpp
	// Spec section 4: "Edit op -> dirty intersecting chunks at all 4 levels". All four,
	// unconditionally: whether the op is big enough to MOVE a sample at a level is
	// ve::lod_ops_for_chunk's judgement at build time, not this call site's. Marking a chunk
	// that then rebuilds identically costs one build; failing to mark one that needed it
	// leaves a crater visible in the near field and absent on the horizon for ever.
	for (int level = 0; level < ve::kLodLevels; level++) {
		LodDirty d;
		d.level = level;
		ve::op_lod_chunk_range(op, level, &d.lo, &d.hi);
		pending_lod_dirty_.push_back(d);
		lod_dirty_counts_[level] +=
				static_cast<int64_t>(d.hi.x - d.lo.x + 1) * (d.hi.y - d.lo.y + 1) *
				(d.hi.z - d.lo.z + 1);
	}
```

- [ ] **Step 7: Write the four debug hooks**

```cpp
int VoxelWorld::debug_lod_tick(Vector3 cam_pos, float px_per_radian) {
	note_camera(cam_pos, px_per_radian);
	lod_tick(cam_pos, px_per_radian);
	// A local device runs nothing until it is told to; the page uploads the tick queued must
	// land before the next tick asks the pool for anything.
	RenderingDevice *device = rd();
	drain_lod_uploads(device);
	if (local_rd_) {
		local_rd_->submit();
		local_rd_->sync();
	}
	std::lock_guard<std::mutex> lock(lod_mutex_);
	return static_cast<int>(lod_draws_.size());
}

Dictionary VoxelWorld::debug_lod_stats() {
	Dictionary d;
	std::lock_guard<std::mutex> lock(lod_mutex_);
	d["draws"] = static_cast<int>(lod_draws_.size());
	d["pending_uploads"] = static_cast<int>(lod_uploads_.size());
	d["builds_done"] = static_cast<int64_t>(lod_builds_done_);
	d["resident"] = lod_residency_ ? lod_residency_->resident_count() : 0;
	d["pages_used"] = lod_residency_ ? lod_residency_->pages_used() : 0;
	d["pages"] = lod_pool_ ? lod_pool_->page_count() : 0;
	d["bytes"] = lod_pool_ ? lod_pool_->bytes_resident() : 0;
	d["drawn"] = lod_raster_ ? lod_raster_->last_drawn() : 0;
	d["culled"] = lod_raster_ ? lod_raster_->last_culled() : 0;
	d["cull_ms"] = lod_raster_ ? lod_raster_->last_cpu_ms() : 0.0f;
	d["busy"] = mesh_ ? mesh_->lod_busy() : false;
	return d;
}

PackedInt32Array VoxelWorld::debug_lod_draw_levels() {
	PackedInt32Array out;
	std::lock_guard<std::mutex> lock(lod_mutex_);
	for (const ve::LodDrawPage &d : lod_draws_) out.push_back(d.level);
	return out;
}

Dictionary VoxelWorld::debug_lod_dirty_counts() {
	Dictionary d;
	for (int l = 0; l < ve::kLodLevels; l++)
		d[String::num_int64(l)] = static_cast<int64_t>(lod_dirty_counts_[l]);
	return d;
}
```

- [ ] **Step 8: Run the streaming test to verify it passes**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_stream.gd`
Expected: PASS — all six cases. If `test_the_page_pool_is_never_overdrawn` hangs rather than fails, the four-page walk is thrashing: `update()` is emitting builds for chunks it cannot page, which means the budget check in Task 3 runs after the emit instead of before it.

- [ ] **Step 9: Run the whole suite**

Run: `cd extension && scons test && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: PASS — every suite. `test_collider_stream.gd` and `test_mesh_stream.gd` matter most here: the LoD queue now shares the worker device with collision meshing, and their timings are what would show a starved collision path (Task 6 gives collision strict priority; this is the test that says so).

- [ ] **Step 10: Commit**

```bash
git add extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_lod_stream.gd
git commit -m "feat(lod): per-frame LoD tick, exports and edit invalidation at all 4 levels"
```

---

### Task 11: the far field, end to end

Every previous task tested one mechanism. This one asks the only question that matters: from a camera above the world, is there a horizon there, is it whole, and does it change when the world does? It uses only machinery Tasks 8–10 already proved — the offscreen render and the hand-driven tick — so it runs headless and deterministically.

**Files:**
- Create: `tests/test_lod_world.gd`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (`debug_lod_settle`)

**Interfaces:**
- Consumes: `debug_lod_tick`, `debug_lod_stats`, `debug_lod_render`, `debug_apply_sphere_subtract` (Tasks 6, 8, 10).
- Produces: `int VoxelWorld::debug_lod_settle(Vector3 cam_pos, float px_per_radian, int max_ticks)` — ticks until the walk stops asking for builds or `max_ticks` runs out; returns the ticks used.

- [ ] **Step 1: Write the failing test**

Create `tests/test_lod_world.gd`:

```gdscript
extends GdUnitTestSuite

# The M5 acceptance question, asked four ways: is the horizon THERE, is it WHOLE, does it
# FOLLOW the world, and does it degrade to something coarser rather than to nothing when the
# page pool is too small.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(pages := 256) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(16, 5, 16)
	w.lod_enabled = true
	w.max_lod_pages = pages
	w.lod_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	assert_bool(w.is_initialized()).is_true()
	return w

# The world spans 0..409.6 m on x and z. Stand at its centre, 300 m up, looking down 45
# degrees: everything in the lower half of the frame is ground, out to hundreds of metres.
const EYE := Vector3(204.8, 300.0, 204.8)
const DIR := Vector3(0.0, -1.0, -1.0)
const PPR := 540.0

func settle(w: VoxelWorld) -> void:
	w.debug_lod_settle(EYE, PPR, 400)

func black_count(img: PackedByteArray, w: int, x0: int, y0: int, x1: int, y1: int) -> int:
	var n := 0
	for y in range(y0, y1):
		for x in range(x0, x1):
			var i := (y * w + x) * 4
			if img[i] == 0 and img[i + 1] == 0 and img[i + 2] == 0:
				n += 1
	return n

func test_there_is_a_horizon(timeout := 180000) -> void:
	var w := make_world()
	settle(w)
	var s: Dictionary = w.debug_lod_stats()
	assert_int(s["draws"]).is_greater(20)
	var r: Dictionary = w.debug_lod_render(EYE, DIR, 128, 128, false)
	assert_bool(r["ok"]).is_true()
	assert_int(r["drawn"]).is_greater(20)
	# Something was culled too, or the frustum test is not running at all.
	assert_int(r["culled"]).is_greater(0)
	# The lower half of the frame is ground.
	assert_int(black_count(r["image"], 128, 0, 64, 128, 128)).is_less(128 * 64 / 10)

func test_the_horizon_has_no_holes_where_the_levels_meet(timeout := 180000) -> void:
	var w := make_world()
	settle(w)
	var r: Dictionary = w.debug_lod_render(EYE, DIR, 128, 128, false)
	# A window well inside the ground, spanning the distance range where the CDLOD walk
	# switches level. Spec section 4's skirts exist for exactly these pixels: an unskirted
	# boundary shows as a line of sky one or two pixels wide, and this window would find it.
	assert_int(black_count(r["image"], 128, 32, 72, 96, 120)).is_equal(0)

func test_an_edit_reaches_the_far_field(timeout := 180000) -> void:
	var w := make_world()
	settle(w)
	var before: PackedByteArray = w.debug_lod_render(EYE, DIR, 128, 128, false)["image"]
	# A 20 m bite out of the ground ~150 m in front of the camera: far enough to be LoD, big
	# enough that even L2's 1.6 m sampling resolves it.
	assert_int(w.debug_apply_sphere_subtract(Vector3(204.8, 51.2, 100.0), 20.0)).is_greater(0)
	settle(w)
	var after: PackedByteArray = w.debug_lod_render(EYE, DIR, 128, 128, false)["image"]
	var diff := 0
	for i in range(0, before.size(), 4):
		if absi(int(before[i]) - int(after[i])) > 8:
			diff += 1
	assert_int(diff).is_greater(50)

func test_a_starved_pool_gives_a_coarser_world_not_an_empty_one(timeout := 180000) -> void:
	# Spec section 8's fail-soft direction, as an assertion: 16 pages cannot hold the cut the
	# walk wants, so the walk must stop descending and draw coarse chunks instead of holes.
	var w := make_world(16)
	settle(w)
	var s: Dictionary = w.debug_lod_stats()
	assert_int(s["pages_used"]).is_less_equal(16)
	var r: Dictionary = w.debug_lod_render(EYE, DIR, 128, 128, false)
	assert_int(r["drawn"]).is_greater(0)
	assert_int(black_count(r["image"], 128, 0, 64, 128, 128)).is_less(128 * 64 / 2)
	var levels: PackedInt32Array = w.debug_lod_draw_levels()
	var coarsest := 0
	for l in levels:
		coarsest = maxi(coarsest, l)
	# With sixteen pages the cut has to include a level coarser than L1 somewhere.
	assert_int(coarsest).is_greater(0)

func test_the_resident_memory_matches_the_budget(timeout := 180000) -> void:
	var w := make_world()
	settle(w)
	var s: Dictionary = w.debug_lod_stats()
	# 832 KB per page, 256 pages: the pool is allocated up front, so this is the ceiling and
	# the number the spec's 300-500 MB line is measured against.
	assert_int(s["bytes"]).is_equal(256 * (40960 * 8 + 512 * 256 * 4))
	assert_int(s["pages_used"]).is_less_equal(256)
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_world.gd`
Expected: FAIL — `Invalid call. Nonexistent function 'debug_lod_settle'`.

- [ ] **Step 3: Write `debug_lod_settle`**

```cpp
int VoxelWorld::debug_lod_settle(Vector3 cam_pos, float px_per_radian, int max_ticks) {
	int t = 0;
	for (; t < max_ticks; t++) {
		const int64_t before = lod_builds_done_;
		debug_lod_tick(cam_pos, px_per_radian);
		// Settled means: the walk asked for nothing this tick and nothing is outstanding.
		// Two consecutive quiet ticks, because a build submitted on tick N lands on N+1.
		if (lod_builds_done_ == before && mesh_ && !mesh_->lod_busy()) {
			debug_lod_tick(cam_pos, px_per_radian);
			t++;
			if (lod_builds_done_ == before) break;
		}
	}
	return t;
}
```

Bind it in `_bind_methods` beside the other Task 10 hooks.

- [ ] **Step 4: Run the acceptance test to verify it passes**

Run: `./build.sh -j$(nproc) && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_lod_world.gd`
Expected: PASS — all five cases.

If `test_the_horizon_has_no_holes_where_the_levels_meet` finds black pixels, work the diagnosis in this order, because each step rules out the next: (a) render the same frame at 512×512 and look at whether the holes form **lines** (a level boundary, so skirts) or **speckle** (the dither, so a fade scale leaked into a test that passed `false`); (b) if lines, check `ve::kLodSkirtCells` against the level ratio — a one-level jump mismatches by one coarse cell, which is four fine ones, and a skirt shorter than that cannot cover it; (c) if the lines follow *chunk* boundaries at the *same* level, the mesher's overlap cell is not being written and the crack is inside one level, which `test_lod_mesh.gd` (Task 4) would also be failing.

- [ ] **Step 5: Commit**

```bash
git add tests/test_lod_world.gd extension/src/voxel_world.h extension/src/voxel_world.cpp
git commit -m "test(lod): far-field acceptance — horizon present, whole, and edit-following"
```

---

### Task 12: the demo, the numbers, and the fade-band verdict

M5 is finished when someone can stand in the demo world, look at the horizon, and see it. This task puts the LoD in the scene, on the HUD and in the benchmark, gives it the quality toggles spec §7 asks every effect to have, and closes out spec §4's *Fade-band quality contingency* with a measured decision rather than a guess.

**Files:**
- Modify: `demo/main.tscn`, `demo/hud.gd`, `demo/benchmark.gd`
- Modify: `extension/src/voxel_world.cpp` (`debug_perf_stats` gains the LoD phases)
- Modify: `docs/superpowers/plans/2026-08-16-m5-lod-hierarchy-bakery.md` (the verdict, in Deliberate Deferrals)

**Interfaces:**
- Consumes: `debug_lod_stats`, `debug_perf_stats`, `lod_enabled`, `lod_fade_enabled`.
- Produces: `--benchmark-lod` mode; HUD LoD line; `debug_perf_stats` keys `lod_tick_ms`, `lod_cull_ms`, `lod_build_ms`, `lod_draws`, `lod_pages_used`, `lod_builds_done`.

- [ ] **Step 1: Add the LoD phases to `debug_perf_stats`**

In `extension/src/voxel_world.cpp`, beside the existing streaming and physics keys:

```cpp
	d["lod_tick_ms"] = last_lod_tick_ms_;
	d["lod_cull_ms"] = lod_raster_ ? lod_raster_->last_cpu_ms() : 0.0f;
	d["lod_build_ms"] = mesh_ ? mesh_->last_lod_build_ms() : 0.0f;
	d["lod_draws"] = lod_raster_ ? lod_raster_->last_drawn() : 0;
	d["lod_pages_used"] = lod_residency_ ? lod_residency_->pages_used() : 0;
	d["lod_builds_done"] = static_cast<int64_t>(lod_builds_done_);
```

`last_lod_tick_ms_` is a `float` member timed around the body of `lod_tick` with `Time::get_singleton()->get_ticks_usec()`, exactly as `last_physics_tick_ms_` is. `MeshService::last_lod_build_ms()` returns the worker's wall time for the most recent `lod_build_sync`, stored as a `std::atomic<float>` because it is written on the worker thread and read on the main one.

- [ ] **Step 2: Put the LoD on the HUD**

In `demo/hud.gd`, extend `_process` with a fourth segment:

```gdscript
	var lod := ""
	if _world and _world.is_initialized():
		var l: Dictionary = _world.debug_lod_stats()
		var p: Dictionary = _world.debug_perf_stats()
		# draws/pages is what the frustum kept over what the pool holds; the two ms figures
		# are the only LoD costs that can land IN a frame (the build is on the worker).
		lod = "  |  lod %d/%d pg %d/%d  b%d  %.2f+%.2fms" % [
			l.get("drawn", 0), l.get("draws", 0),
			l.get("pages_used", 0), l.get("pages", 0),
			l.get("builds_done", 0),
			p.get("lod_tick_ms", 0.0), p.get("lod_cull_ms", 0.0)]
		if not _world.lod_enabled:
			lod = "  |  lod OFF"
		elif not _world.lod_fade_enabled:
			lod += " (nofade)"
	text = "%d fps  (%.1f ms)  |  %s%s%s%s" % [fps, ms, s, p, isl, lod]
```

and add the two quality toggles spec §7 asks for ("Every effect has a quality/off toggle in the debug menu"):

```gdscript
func _unhandled_input(event: InputEvent) -> void:
	if not _world or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	match event.keycode:
		KEY_F3:
			_world.lod_enabled = not _world.lod_enabled
		KEY_F4:
			# Off, the seam becomes a hard edge at 120 m — which is how you SEE what the
			# dither is doing, and the fastest way to tell a fade bug from a build bug.
			_world.lod_fade_enabled = not _world.lod_fade_enabled
```

In `demo/main.tscn`, the `VoxelWorld` node gains `lod_enabled = true`, `max_lod_pages = 512`, `lod_builds_per_frame = 1`, `lod_sse_threshold = 4.0`. Nothing else in the scene changes: the compositor already publishes the camera and issues the draw.

- [ ] **Step 3: Add the `--benchmark-lod` mode**

In `demo/benchmark.gd`, extend the header comment and the mode list with:

```
#   --benchmark-lod    the player flies a wide circle at altitude, so the LoD hierarchy
#                      re-selects levels and streams new chunks for the whole run. This is
#                      the mode spec section 7's "LoD ~2 ms" budget is measured in.
```

```gdscript
const LOD_FRAMES := 600

	for m in ["--benchmark-move", "--benchmark-edit", "--benchmark-island",
			"--benchmark-lod", "--benchmark"]:
```

```gdscript
	if _mode == "--benchmark-lod":
		_target_frames = LOD_FRAMES
		# 300 m up so the whole hierarchy is in play at once; a circle rather than a line so
		# the run neither leaves the world nor settles into a cut it can reuse.
		_player.global_transform = Transform3D(Basis.IDENTITY, Vector3(204.8, 300.0, 204.8))
		_cam.transform = Transform3D(Basis.looking_at(Vector3(1, -1, 0).normalized()),
			Vector3(0, 0.7, 0))
```

in `_process`:

```gdscript
	elif _mode == "--benchmark-lod":
		_lod_phase += delta * 0.25
		var c := Vector3(204.8, 300.0, 204.8)
		_player.global_position = c + Vector3(cos(_lod_phase), 0.0, sin(_lod_phase)) * 120.0
		_cam.look_at(c + Vector3(0.0, -80.0, 0.0), Vector3.UP)
```

and in `_report`, after the existing percentile block:

```gdscript
	if _mode == "--benchmark-lod":
		var tick: float = float(_perf_max.get("lod_tick_ms", 0.0))
		var cull: float = float(_perf_max.get("lod_cull_ms", 0.0))
		print("lod_tick_max_ms=%.2f lod_cull_max_ms=%.2f lod_build_max_ms=%.2f" % [
			tick, cull, float(_perf_max.get("lod_build_ms", 0.0))])
		print("lod_pages_used=%d lod_builds_done=%d" % [
			int(_prev_perf.get("lod_pages_used", 0)),
			int(_prev_perf.get("lod_builds_done", 0))])
		# Spec section 7 budgets ~2 ms for LoD. Warn rather than fail: the number that
		# matters is the frame percentile above, and a warning is what makes someone look.
		if tick + cull > 2.0:
			print("WARNING: LoD main-thread cost %.2f ms exceeds the 2 ms budget" %
				[tick + cull])
```

`_lod_phase` is a new `var _lod_phase := 0.0`.

- [ ] **Step 4: Run the benchmarks**

```bash
godot --path . demo/main.tscn -- --benchmark-lod --disable-vsync
godot --path . demo/main.tscn -- --benchmark-move --disable-vsync
godot --path . demo/main.tscn -- --benchmark --disable-vsync
```
Expected: `frame_avg_ms` < 16.6 in all three; `lod_builds_done > 0` and `lod_pages_used > 0` in the first; no `WARNING` line. The two older modes must be unchanged within noise — M5 adds one indirect draw and two small `buffer_update`s to the render thread and one tick to the main thread, and nothing else.

- [ ] **Step 5: Play it**

```bash
godot --path . demo/main.tscn
```
Expected: standing on the ground and looking at the horizon, there is terrain all the way out instead of the 200 m cliff-edge into sky that M1–M4 had. Walking changes which chunks are resident without visible popping. Pressing `F3` makes the far field vanish and the horizon go back to sky; pressing it again brings it back within a second or two. Pressing `F4` turns the dither off and makes the 120 m seam a hard, visible line — which is what confirms the dither is the thing hiding it. Drilling a large hole and then flying 300 m away shows the hole still there, at the coarser resolution.

- [ ] **Step 6: Decide spec §4's fade-band contingency, and record the decision**

Spec §4: "L1 starts at 150 m; 0.4 m mesh features there are ~3–5 px … if benchmarking shows the fade band too coarse, densify L1 mesh sampling to 0.2 m within 300 m — decided during implementation, no structural change."

Measure, do not guess. At 1440p with a 60° vertical fov, `px_per_radian` ≈ 1247, so a 0.4 m feature at 135 m subtends 0.4/135 × 1247 ≈ **3.7 px** — the spec's own estimate, confirmed. The question the benchmark answers is whether the *bake* carries the detail those 3.7 px lose:

1. Run the demo, stand still at ground level, press `F4` to make the seam hard.
2. Look at the band edge. If the LoD side reads as a different **material or brightness** from the near field, the bake is wrong and densifying the mesh will not help — fix the bake.
3. If it reads as the same surface but visibly **smoother in silhouette**, that is the case the contingency is for.
4. Press `F4` again. If the dither hides it at normal head-turn speeds, the contingency is **not needed** and M5 ships as planned.

Record the outcome in this plan's **Deliberate Deferrals** section, replacing the "Task 12 measures the band and records the verdict" sentence with the verdict and the two numbers behind it (measured px per feature, and whether the difference survived the dither). If the contingency *is* needed, the change is a fifth level — 12.8 m chunks at 0.2 m — which means `ve::kLodLevels` becomes 5 and the four `kLodCellSize` / `kLodChunkSize` entries gain a leading pair; every other file in M5 is written against `kLodLevels` and needs no edit. Do not make that change inside M5: record it as the first entry of the M6 plan's inputs.

- [ ] **Step 7: Run everything one last time**

Run: `cd extension && scons test && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: PASS — the native suite (including `test_lod_grid`, `test_lod_page`, `test_lod_residency`, `test_lod_bake`, `test_lod_cull`) and every gdUnit suite (including `test_lod_mesh`, `test_lod_bake`, `test_lod_build`, `test_lod_pool`, `test_lod_render`, `test_lod_fade`, `test_lod_stream`, `test_lod_world`).

- [ ] **Step 8: Commit**

```bash
git add demo/main.tscn demo/hud.gd demo/benchmark.gd extension/src/voxel_world.h \
        extension/src/voxel_world.cpp extension/src/render/mesh_service.h \
        extension/src/render/mesh_service.cpp \
        docs/superpowers/plans/2026-08-16-m5-lod-hierarchy-bakery.md
git commit -m "feat(demo): LoD on the HUD, a LoD benchmark mode, and the fade-band verdict"
```

---

## M5 Acceptance Checklist

- `cd extension && scons test` — native suite green: the level lattice, page packing and skirts, the CDLOD walk with a page budget, the bake's one-texel reference and the frustum culler, plus every M1–M4 case
- `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests` — green, including the eight new suites: **`test_lod_mesh.gd`**, **`test_lod_bake.gd`**, **`test_lod_build.gd`**, **`test_lod_pool.gd`**, **`test_lod_render.gd`**, **`test_lod_fade.gd`**, **`test_lod_stream.gd`**, **`test_lod_world.gd`**
- `godot --path . demo/main.tscn` — the horizon is terrain, not sky, in every direction; walking re-selects levels with no popping; `F3` toggles the far field; `F4` exposes the seam the dither hides; a crater drilled up close is still there from 300 m away
- `godot --path . demo/main.tscn -- --benchmark-lod --disable-vsync` — `frame_avg_ms` < 16.6, `lod_builds_done > 0`, no budget `WARNING`
- The three older benchmark modes are unchanged within noise
- **The GPU/CPU pairs still agree**: `ve::dual_contour` against `mesh_*.comp.glsl` at *both* pitches (`debug_lod_mesh_diff`), and `ve::bake_lod_texel` against `lod_bake.comp.glsl` (`debug_lod_bake_diff`) — spec §8's differential testing rule, now covering the two new shaders
- **The safe direction is preserved**: a starved page pool draws a coarser world, never an empty one; a refused build retries; a failed bake leaves a flat page; a stale far chunk outlives a missing one. No test, and no minute of play, produces a hole in the horizon

## Spec Coverage

| Spec sentence | Where |
|---|---|
| §4 "4 levels, each 4× linear, aligned to the brick/region grid" | Task 1 — `kLodLevels`, `kLodRatio`, L1 = one region |
| §4 the level table's chunk sizes (25.6 / 102.4 / 409.6 / 1638.4 m) | Task 1 — `kLodChunkSize[]`, verbatim |
| §4 the level table's mesh sampling (0.4 / 1.6 / 6.4 / 25.6 m, 64³) | Tasks 1, 4 — one mesher at four pitches, because 64³ is already `kChunkCells` |
| §4 the level table's bake texture (128² / 64² / 32² / 32²) | Task 2 — **per-quad 4×4 tiles in a 512×256 page** instead, because a per-chunk plane cannot bake an overhang (Deliberate Decisions); a page carries ~64× the texels of a 128² chunk image |
| §4 "L1 150 m – 1 km", "L2 1–2 km", "L3 2–4 km", "L4 horizon" | Tasks 3, 10 — selection is by screen-space error against a page budget; the memory arithmetic above shows 1 km of L1 is infeasible inside spec §4's own 300–500 MB, and names `max_lod_pages` as the dial |
| §4 "a chunk renders at the coarsest level under a screen-space-error threshold (CDLOD-style)" | Task 3 — `lod_screen_error`, `kLodSseThreshold`, the walk |
| §4 "Camera motion re-selects levels; it does not rebuild anything" | Task 3 — `update()` never touches a built page; only `mark_dirty` does |
| §4 "coarse surface-nets mesh evaluated directly from G + edit ops at that level's resolution" | Task 4 — the M3 mesher with origin and pitch in the push constant |
| §4 "no L0 residency needed to build distant chunks" | Task 4 — `mesh_field.comp.glsl` evaluates the analytic field; the brick atlas is not read |
| §4 "Imprecise is fine — detail comes from the bake" | Task 5 — 4×4 texels per quad, refined onto the analytic surface |
| §4 "Skirts on chunk borders hide inter-level cracks; no stitching meshes" | Task 2 (`build_lod_pages` grows them from the mesh alone), Task 11 (the no-holes assertion) |
| §4 "bake albedo + normal by raymarching the higher-fidelity representation … at ~4× the chunk's mesh sampling" | Task 5 — one Newton step onto the true surface plus the analytic gradient, at 4× |
| §4 "along each triangle's dominant axis — triplanar-style projection, so cliffs/overhangs/cave mouths bake correctly" | Task 5 — **per-quad** projection, the strongest reading: no two surfaces can collide in one tile (Deliberate Decisions) |
| §4 "GPU compute into texture arrays" | Tasks 5, 7 — `lod_bake.comp.glsl` into a `TEXTURE_TYPE_2D_ARRAY` page pool |
| §4 "BC compression if the budget demands" | Deliberate Deferrals — `max_lod_pages` is the dial M5 ships instead |
| §4 "Background priority queue keyed by screen-space error" | Task 3 — `LodFrame::builds`, ordered by the walk's distance |
| §4 "~1–4 ms GPU each; a few per frame" | Task 6 (the worker queue), Task 10 (`lod_builds_per_frame`, default 1) |
| §4 "Edit op → dirty intersecting chunks at all 4 levels" | Task 10 — all four, unconditionally, via `op_lod_chunk_range` |
| §4 "distant edits rebuild lazily" | Task 3 — a dirty chunk re-enters the queue at its own screen-space priority |
| §4 "Fade-band quality contingency … decided during implementation" | Task 12 Step 6 — measured, decided, recorded |
| §4 "CPU frustum-cull (few thousand AABBs)" | Task 8 — `ve::lod_cull_pages`, with a doctest |
| §4 "one indirect multi-draw per level via RenderingDevice, textures as arrays" | Task 8 — **one** multi-draw for all four levels (Deliberate Deferrals: strictly fewer draws) |
| §4 "All LoD shares the near field's cel lighting GLSL" | Task 8 — `shade_terrain()` in `common.glslh`, called by both shaders |
| §4 "Memory: ~5–8k resident chunks ≈ 300–500 MB textures + trivial geometry" | Fixed Numbers — 512 pages ≈ 426 MB, and the geometry is **not** trivial at this tile density, which is why the range lands where it does |
| §3 "Raymarched depth is blitted into Godot's scene depth buffer before the opaque pass" | M1's `CompositePass`, unchanged; Task 9 only adds the dither |
| §3 "LoD geometry and Godot dynamic objects depth-test against it normally. Mutual occlusion is exact both ways" | Task 8 — the same framebuffer, `COMPARE_OP_GREATER_OR_EQUAL`, no compositing rule |
| §3 "Dithered depth fade over 120–150 m, cross-fading into LoD" | Task 9 — one `lod_fade`, one `bayer4`, two opposite comparisons; `test_lod_fade.gd` proves the masks are complements |
| §7 "Frame order: Raymarch G-buffer → LoD raster into G-buffer → … → inject color+depth → Godot opaque" | Task 9 — raymarch, composite, LoD raster, all pre-opaque. M5 injects colour+depth *before* the LoD raster rather than after, because there is no merged G-buffer until M6 builds one; the resulting image is identical and the ordering moves in M6 |
| §7 "LoD ~2 ms" | Task 12 — measured per phase, warned on in `--benchmark-lod` |
| §7 "Every effect has a quality/off toggle in the debug menu" | Task 12 — `F3` (LoD), `F4` (fade band) |
| §8 `lod/` module, pure C++ | Tasks 1, 2, 3, 5, 8 — five pure files, five doctest suites, zero Godot types |
| §8 "GPU differential testing: CPU references … dev console command runs both and diffs" | `debug_lod_mesh_diff` (Task 4), `debug_lod_bake_diff` (Task 5) |
| §7 shadow map from LoD geometry, SSGI, SSR, outlines, cel bands | M6 — M5 deliberately shares the *existing* lighting so there is one place to change |

## Errata (recorded during M5 implementation — corrections to the task text)

<!-- Append numbered entries here as the plan meets reality, in the style of M1/M2/M3/M4. -->

## Execution Handoff

The plan is complete and self-contained: twelve tasks, each ending in a committed, independently testable deliverable, with every file path, signature, shader and test written out.

Two ways to run it:

1. **Subagent-Driven Development** *(recommended)* — `superpowers:subagent-driven-development`. Each task goes to a fresh subagent with only this plan as context, and the task's own test is the acceptance gate before the next one starts. It suits M5 particularly well: the tasks are strictly ordered by their interfaces, and Tasks 1–3 and 5 are pure C++ with doctests that need no GPU at all.
2. **Inline Execution** — `superpowers:executing-plans`, working through the checkboxes in this session with review checkpoints at each commit.

**Which would you like?**
