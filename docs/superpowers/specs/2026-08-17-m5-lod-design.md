# M5 — Far-Field LoD Design

**Date:** 2026-08-17
**Status:** Approved design, pre-implementation
**Scope:** The far field: rasterized surface-nets geometry from 150 m to the world edge, its
streaming, its culling, and the material shading both fields share.
**Supersedes:** parts of `2026-08-12-voxel-engine-design.md` §2 and §4 — see §1 below.
**Reference implementation studied:** `/home/jeremy/Development/minecraft/voxy` (Voxy, Minecraft
LoD mod). Cited by file throughout.

---

## 1. What this changes in the engine spec

The engine spec's §4 was written before Voxy's renderer had been read closely. Five of its
decisions are wrong, and this document replaces them. Nothing else in the engine spec changes.

| Engine spec | Replaced by | Why |
|---|---|---|
| §4 "Texture bake (Voxy trick adapted to smooth SDF)" — per-chunk albedo+normal bake | **Deleted.** Detail comes from triplanar material textures sampled at shading time. | Voxy has no bake. `quads.frag` samples the shared block atlas with `textureGrad`; the LoD carries a material id, not pixels. Baking costs 64 B/quad and produces far-field detail the near field would visibly lack. |
| §4 table — 4 levels, 4× linear, 64³ chunks | **8 levels, 2× linear, 32³ chunks** (§2) | Voxy's sections are 32³ at every level with a 2× ratio. A 4× ratio means the screen-space error inside one level's band varies 4:1, so geometry is either over- or under-tessellated almost everywhere, and every level change pops 4× harder. |
| §2 "every LoD level is generated independently and exactly; **no downsampling cascade anywhere in the engine**" | **Half-cell supersample with an averaging reduction** (§4) | Point-sampling `G + ops` at 6.4 m discards every feature below 6.4 m — including the player's craters. Voxy mips (`WorldVoxilizedSectionMipper`) precisely to avoid this. |
| §4 streaming into a distance/SSE-keyed resident shell | **Visibility-driven residency** (§6) | Voxy keeps what the traversal reached, not what is near. A shell pays for the ~95 % of the sphere that is never on screen. |
| §4 "CPU frustum-cull … one indirect multi-draw per level" | **GPU frustum ∩ HiZ cull, one indirect multi-draw total** (§7) | Frustum-only culling draws every ridge-occluded basin. The raymarched near field already writes exact pre-opaque depth, so the occluder is free. |

Two further consequences, recorded so they are not read as scope creep:

- **Material textures land in M5, not M6.** Removing the bake removes the far field's only
  source of surface detail; the replacement has to ship with it. M6 keeps cel banding,
  shadows, SSGI, SSR and outlines.
- **§4's "fade-band quality contingency" (densify L1 to 0.2 m within 300 m) is obsolete.** With
  a 2× ratio the finest level reaches 157 m on its own, and adding a finer level is one more
  octree level rather than a structural change.

---

## 2. Level table

Levels are numbered from the finest. Chunks are **32³ cells** at every level; cell size doubles
per level. Selection is by screen-space error, not by band membership — the ranges below are
what a 3 px error threshold produces at 1440p / 70° vertical FOV (1179 px/rad, so a cell of
size `c` reaches 3 px at `d = 393·c`).

| Level | Cell | Chunk | 3 px range | Role |
|---|---|---|---|---|
| L0 | 0.4 m | 12.8 m | → 157 m | meets the near-field seam |
| L1 | 0.8 m | 25.6 m | → 314 m | |
| L2 | 1.6 m | 51.2 m | → 629 m | |
| L3 | 3.2 m | 102.4 m | → 1.26 km | |
| L4 | 6.4 m | 204.8 m | → 2.5 km | |
| L5 | 12.8 m | 409.6 m | → 5 km (past the world edge) | permanently resident |
| L6 | 25.6 m | 819.2 m | — | permanently resident |
| L7 | 51.2 m | 1638.4 m | — | octree roots, permanently resident |

L0's range landing on 157 m is not tuned; it falls out of the seam sitting at 150 m.

**Octree.** The 4096×1024×4096 m world is covered by a **3×1×3 grid of L7 roots**. A node at
level `L` has eight children at `L-1`; L7→L0 is seven subdivisions (2⁷ × 12.8 m = 1638.4 m).
Chunk coordinates are global per level: chunk `c` at level `L` has its minimum corner at
`c · 32 · cell(L)`. Only nodes that contain surface are ever instantiated.

L5–L7 hold roughly 190 surface-intersecting chunks in total (~6 MB) and are **never evicted**,
so turning the camera can reveal coarse terrain but never sky.

---

## 3. Quad encoding and the geometry arena

### 3.1 The record

A surface-nets quad's four vertices always lie in the four cells sharing one active lattice
edge. Those four cells occupy a 1×2×2 block (they share their coordinate along the edge axis —
see `shaders/mesh_quads.comp.glsl:41-50`), so no vertex needs an absolute coordinate:

```
bits  0..14   owning edge coordinate u     3 × 5 bits   (u ∈ [0,32))
bits 15..16   edge axis                         2 bits
bit  17       sign (solid → air direction)      1 bit
bits 18..77   4 corner offsets             4 × 15 bits  (5 bits/axis, 1/32 cell)
bits 78..93   material id                      16 bits
bit  94       double-sided (skirt)              1 bit
bit  95       spare                             1 bit
                                           ─────────────
                                                96 bits = 12 bytes
```

`u` is the coordinate of the edge the chunk **owns**, exactly as in
`shaders/mesh_quads.comp.glsl:36-38`: local coordinate `u ∈ [0, 32)`, lattice index `u + 1`.
Every edge in the world is owned by exactly one chunk, so borders have neither cracks nor
duplicated quads. The four corner cells follow from `u`, the axis and the corner index via the
same `QUAD` table the mesher already uses, and each offset is relative to its own corner's
cell. Precision is 1/32 of a cell at every level — 12.5 mm at L0, 1.6 m at L7.

Against the reverted M5 plan's 32 B of vertices plus 64 B of bake, this is **8× more resident
geometry per byte**, which is the entire reason that plan concluded the engine spec's view
distance was unreachable.

### 3.2 No vertex buffer

Geometry is pulled, not fetched. One shared **6 KB index buffer** holds
`{4q, 4q+1, 4q+2, 4q, 4q+2, 4q+3}` for `q ∈ [0, 512)` as `uint16` (max index 2047). Every draw
binds it and sets `vertexOffset = page · 2048`, so the vertex shader recovers

```glsl
uint quad = gl_VertexIndex >> 2;         // global arena quad index
uint page = quad >> 9;                   // 512 quads per page
ChunkRecord ch = chunks[pageToChunk[page]];
```

This is Voxy's `gl_VertexID>>2` trick (`quads3.vert:56`) and it routes around Godot exposing
neither `gl_DrawID` (`VK_KHR_shader_draw_parameters`) nor a non-zero `firstInstance`
(`drawIndirectFirstInstance`) — Voxy uses `gl_BaseInstance` for its per-section lookup
(`cmdgen.comp:writeCmd`) and we cannot.

### 3.3 Arena

One global quad arena of fixed **512-quad (6 KB) pages**. Draw granularity is the page, so a
chunk's pages need not be contiguous and there is no suballocator or fragmentation. Default
pool **32768 pages = 192 MB** (`VoxelWorld::max_lod_pages`, exported); at ~2500 quads/chunk
that is ~6500 resident chunks. Raising it extends range linearly.

A chunk is capped at **16 pages (8192 quads)**; a build that overflows keeps its first 8192
quads and logs once, per the engine spec's fail-soft policy. The cap is what bounds the
per-chunk readback in §6.4 at 96 KB.

Side tables: `pageToChunk` 128 KB, chunk records 8192 × 32 B = 256 KB, indirect args
32768 × 20 B = 640 KB, index buffer 6 KB.

**M5 GPU memory: 192 MB arena + 45 MB material arrays + ~1 MB tables ≈ 238 MB**, inside the
engine spec §4's 300–500 MB with the arena as the dial.

### 3.4 Normals

None are stored. At a 3 px screen-space error a quad is smaller than any shading gradient, so
the flat geometric normal computed from the four corners in the vertex shader and passed `flat`
is sufficient and free — Voxy's choice for the same reason. **Measured trigger:** if M6's
quantized cel bands make faceting visible in the fade band, add a parallel 8 B/quad
corner-normal array enabled for the two finest levels only (20 B/quad, still 5× better than a
bake).

---

## 4. Field evaluation and the mip reduction

Each chunk build evaluates `G + ops` at **half its cell size** and reduces to its own lattice.

1. **Field pass** — evaluate at `cell/2` over a **70³** lattice (the 34³ target lattice at
   indices `[-1, 32]` needs half-cell samples over `[-3, 65]`, i.e. 69 per axis; dispatched at
   70³). Produces SDF and material per sample.
2. **Reduce pass** — separable **tent filter** (¼, ½, ¼ per axis) over the 27-tap neighbourhood
   → the 34³ lattice at the level's own cell size.
3. **Cells pass** — surface-nets vertex per cell → 33³ cell array. This is M3's
   `mesh_cells.comp.glsl` generalized from a compile-time pitch to an origin + cell size in the
   push constant.
4. **Quads pass** — emit packed 12-byte quads. New; M3's `mesh_quads.comp.glsl` emits triangle
   indices instead.

**Reduction rules.**

- **SDF: average** (the tent filter above). Voxy's `Mipper` prefers non-air because block data
  is binary and has no mean; an SDF has one, and averaging is symmetric — it preserves craters
  and spires equally. A solid-preferring `min` would erase the player's craters at distance,
  which is the wrong failure mode for a destruction demo.
- **Material: solidity-weighted majority vote** over the same 27 taps, ties broken by the
  nearest sample. This one *is* Voxy's `Mipper` rule, and it is what stops distant material
  boundaries dissolving into noise.
- **Quad material** = the material of the solid endpoint of its active edge. Deterministic and
  mirrorable on the CPU.

**Transitivity, stated honestly.** An L4 chunk sees L3-scale features, not 5 cm ones — the
cascade is computed on demand inside one build job, not accumulated through stored levels. A
fully transitive cascade needs Voxy's persistent world database, which engine spec §2 rules
out. This is a deliberate accuracy/storage trade, not an oversight.

**Skirts** are generated on the CPU from the readback quads: a boundary edge is one whose
endpoints both lie within 1.5 cells of the same chunk face plane, and the curtain hangs 2 cells
along the quad's negative normal — enough for the ≤1 coarse-cell mismatch a single-level jump
can produce, which a 2× ratio bounds much more tightly than a 4× one. Skirt quads carry the
double-sided bit and are emitted twice with opposite winding.

---

## 5. Material textures and shading

**Source.** `/home/jeremy/Development/Unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2`
— 40 PBR sets, 2048² TGA (basecolor / normal / roughness / AO / height / smoothness). A
committed script (`tools/convert_materials.sh`) converts the subset in use to
`assets/materials/*.png` at 512², so the build never depends on a path outside the repo.

**Two `RGBA8` texture arrays with full mip chains, 512² per layer:**

| Array | Contents |
|---|---|
| `albedo` | basecolor RGB, height in A |
| `surface` | normal XY, roughness, AO |

512² is set by memory: 1.4 MB per layer per array, so 16 materials ≈ **45 MB**; 1024² would be
4× that and competes with the 0.7–1.0 GB brick atlas. Starting subset maps the four existing
material ids (`common.glslh:39`) — `grass_01`, `rock`, `ground_01`, `breakstone` — with room to
16.

Mips do the distance work for free: at 2 km a 2 m tile is sub-pixel and resolves to the top
mip's average. That is exactly what `quads.frag`'s `textureGrad` does in Voxy, and it is why
the far field does not need a bake to look right.

**One shared entry point, in `shaders/common.glslh`:**

```glsl
vec4 material_surface(uint mat, vec3 wpos, vec3 n, vec3 ddx, vec3 ddy);
vec3 shade_terrain(vec4 surf, vec3 n, vec3 wpos);
```

`material_surface` triplanar-blends three `textureGrad` samples weighted by `|n|^k`, each
projection deriving its own 2D gradient from `ddx`/`ddy`.

**Explicit gradients are load-bearing.** The raymarcher is a *compute* shader and has no
`dFdx`/`dFdy`, so it supplies gradients from ray differentials (pixel cone footprint × hit
distance) while `lod.frag.glsl` supplies `dFdx(wpos)`/`dFdy(wpos)`. Without this the near field
either aliases or over-blurs and the seam becomes a visible sharpness step — the one artefact
this whole approach exists to avoid.

`shade_terrain` is the lighting currently inlined at `shaders/raymarch.comp.glsl:415`, factored
out. Both fields call it. M6 then replaces the lighting in one place instead of two, which is
what engine spec §8's single `common.glslh` is for.

---

## 6. Residency, traversal, and streaming

### 6.1 The walk

`ve::LodTree` is pure C++ with no Godot types, ~5–20k nodes for the whole bounded world. It
runs **every frame against the current camera** (~0.2–0.3 ms):

```
walk(node):
  if !frustum(node):             mark coarse-only; return
  if area(node) <= sse_thresh:   emit_draw(node); return
  if !all_children_ready(node):  emit_draw(node); request(children); return
  for c in children: walk(c)
```

`area(node)` is the projected screen area of the node AABB in px², matching Voxy's
`screenspace.glsl:shouldDecend`. The threshold is stated once, in terms of §2's per-cell error:

```
kLodTargetCellPx   = 3.0
kLodSseAreaThresh  = (32 · kLodTargetCellPx)²   // ≈ 9216 px²
```

A node whose projection is larger than that would render cells coarser than 3 px, so it
descends. Because projected area scales with resolution, the threshold is expressed in absolute
px² and needs no per-resolution tuning; §2's distance columns are this rule evaluated at
1440p.

Three outputs: the **draw list** (page indices), the **build queue** (keyed by projected area,
largest first), and **keep-alive marks**.

Descending only into a **fully ready sibling set** — all eight children built, or probed empty
— guarantees the emitted cut is complete and non-overlapping at every instant: a streaming
child never opens a hole and never z-fights its parent. This is the one idea worth keeping from
the reverted M5 plan.

### 6.2 Residency

Residency is what the walk touched. Unmarked nodes age out after
**`kLodEvictFrames` = 300 consecutive unmarked frames**, or immediately under arena pressure
(evict largest projected-area-deficit first). L5–L7 are exempt (§2).

### 6.3 Occlusion has two jobs, deliberately split

| | Source | Latency | Failure mode |
|---|---|---|---|
| **Hiding** geometry | GPU HiZ, full res, this frame | exact | none |
| **Streaming** decisions | async readback of a coarse HiZ level (~2 KB) | 1–2 frames | 1–2 frame pop-in |

Stale occlusion can only ever cause a late build, never a wrongly hidden chunk. Because the
readback is stale, occlusion alone never evicts or skips on a single frame: a node must test
occluded on **`kLodOccludedFrames` = 8 consecutive frames** before the walk stops requesting
it. This is a separate constant from `kLodEvictFrames` (§6.2) and much smaller — occlusion is a
reason to stop *building*, ageing out is a reason to *free*.

### 6.4 Builds

Builds reuse M3's worker-device pattern: gather ops for the chunk AABB → field → reduce →
cells → quads → async readback (≤ 96 KB/chunk) → page upload on the render device.
`shaders/field_probe.comp.glsl` already provides the cheap empty-chunk probe. Budget **8 builds
per frame** (exported), priority by projected area.

Chunks whose farthest corner is nearer than the fade start (120 m) are never built — the
fragment shader would discard every pixel.

### 6.5 Invalidation

An edit dirties every level whose chunks its world AABB touches; dirty chunks rebuild in
priority order, distant ones lazily. The relevance cut is at the **half-cell** supersample
resolution rather than the cell: a 5 m crater still registers at L4's 6.4 m cells, which is the
point of §4's reduction change. Only ops whose AABB is shorter than half a cell on every axis
are genuinely unrepresentable and dropped.

Op lists exceeding `ve::kMaxRegionOps` truncate to a **chronological prefix**, never a suffix —
a prefix of an ordered CSG list is a valid world state; a suffix can apply an add without the
subtract that made room for it.

---

## 7. Rendering

### 7.1 Frame order

All four new passes are pre-opaque, inside the existing `CompositorEffect`:

```
raymarch.comp        near field 0–150 m → color + depth              (M1, unchanged)
composite.frag       inject color + depth into scene buffers          (M1, + dither)
  ↓ scene depth now holds exact near-field occluders
hiz.comp             min-pyramid (reverse-Z) from scene depth         NEW
lod_cull.comp        per page: frustum ∩ HiZ → instanceCount          NEW
lod_cmdgen.comp      pack indirect args                               NEW
lod.vert/.frag       one indirect draw into the scene framebuffer     NEW
  ↓
Godot opaque pass    dynamic objects depth-test against both fields
```

### 7.2 The draw

Godot's `draw_list_draw_indirect(draw_list, use_indices, buffer, offset, draw_count, stride)`
takes `draw_count` as a **CPU integer** (`docs/api/renderingdevice.md:2971`); there is no
count-buffer variant, so a purely GPU-decided cut would have to issue tens of thousands of
empty draws. The split in §6.3 solves this: **the CPU walk is the candidate list**, giving an
exact `draw_count`, and `lod_cull.comp` only ever *removes* by zeroing `instanceCount`. Upload
is ~16 KB/frame for ~4000 pages.

Depth state matches `CompositePass`: reverse-Z, `COMPARE_OP_GREATER_OR_EQUAL`, depth write on.

**Backface culling is enabled.** The reverted plan disabled it out of caution over M3 errata
1's winding bug, but `shaders/mesh_quads.comp.glsl:63-70` already resolves winding correctly
for Jolt, and culling halves fragment work on geometry that is half backfaces. Skirts are the
only two-sided geometry and are duplicated rather than needing a second pipeline.

### 7.3 Fragment path

```glsl
vec4 surf = material_surface(ch.mat, wpos, n, dFdx(wpos), dFdy(wpos));
outColor  = shade_terrain(surf, n, wpos);
```

Identical calls to the raymarcher's, so the two fields cannot drift.

### 7.4 Seam

Complementary 4×4 Bayer dither over 120–150 m, kept from the reverted plan: `composite.frag`
discards its *depth* where `bayer(px) < fade(d)` (keeping colour, so a missing LoD chunk shows
near-field terrain rather than sky); `lod.frag` discards entirely where `bayer(px) >= fade(d)`.
Both run at full scene resolution on the same pixel grid, so the masks are exact complements
and every pixel in the band belongs to exactly one field. `bayer4()` lives in `common.glslh`.

### 7.5 Deferred: the temporal second phase

This HiZ is built from near-field depth only, so it catches *near occludes far* — standing in a
valley — but not *far occludes far*, a ridge hiding the basin behind it at 1 km. Voxy's answer
is the temporal two-pass (`renderTemporally` / `TEMPORAL_OFFSET` in `cmdgen.comp`): draw last
frame's visible set, rebuild HiZ, then cull and draw the remainder. That is **additive** to
this structure, not a restructuring of it. M5 ships one pass and the benchmark reports the
culled-quad ratio. **Trigger:** if far-occludes-far waste exceeds 30 %, the second phase goes
in.

---

## 8. Module structure

Engine spec §8's boundary holds: pure C++ cores in `ve::` with zero Godot types; anything
owning a `RID` is glue.

```
extension/src/
  lod/                      (pure C++, ve::)
    lod_grid.h/.cpp         level table, chunk math, coords, op ranges
    lod_tree.h/.cpp         octree, walk, requests, eviction
    lod_quad.h/.cpp         12-byte pack/unpack + CPU mesher reference
    lod_arena.h/.cpp        page allocator, page→chunk table
    lod_reduce.h/.cpp       tent filter + material vote, CPU reference
    lod_skirt.h/.cpp        boundary-edge detection, skirt emission
  render/                   (Godot glue)
    material_atlas.h/.cpp   the two texture arrays, load + mips
    lod_build_pass.h/.cpp   worker-device field/reduce/cells/quads
    lod_pool.h/.cpp         render-device arena, page uploads
    hiz_pass.h/.cpp         min-pyramid + async coarse readback
    lod_cull_pass.h/.cpp    frustum ∩ HiZ → instanceCount
    lod_cmdgen_pass.h/.cpp  indirect args
    lod_raster_pass.h/.cpp  the draw
    mesh_pass.h/.cpp        MODIFIED: origin + cell size in the push constant
    composite_pass.h/.cpp   MODIFIED: dithered depth discard
shaders/
  common.glslh              MODIFIED: material_surface, shade_terrain, bayer4
  raymarch.comp.glsl        MODIFIED: calls both; ray-differential gradients
  mesh_common.glslh         MODIFIED: origin + cell size from the push constant
  mesh_field.comp.glsl      MODIFIED: generalized pitch + lattice dimension
  mesh_cells.comp.glsl      MODIFIED: same
  lod_reduce.comp.glsl      NEW
  lod_quads.comp.glsl       NEW
  hiz.comp.glsl             NEW
  lod_cull.comp.glsl        NEW
  lod_cmdgen.comp.glsl      NEW
  lod.vert.glsl             NEW
  lod.frag.glsl             NEW
assets/materials/           NEW: 512² PNGs
tools/convert_materials.sh  NEW: TGA → 512² PNG
```

---

## 9. Testing

Per engine spec §8, implementation follows TDD.

**Native (doctest + CTest), milliseconds, no Godot and no GPU:**

- `lod_grid` — level table matches §2, chunk coords, parent/child, op → dirty-chunk ranges
- `lod_quad` — pack/unpack round-trip, precision bounds at every level, corner-cell derivation
- `lod_tree` — walk against a synthetic camera and a fake depth pyramid: the emitted cut is
  complete and non-overlapping, budget is respected, eviction honours K and the L5–L7 exemption
- `lod_arena` — page alloc/free, no leak, no double-free, pressure eviction
- `lod_reduce` — tent filter, material vote, tie-breaking
- `lod_skirt` — boundary-edge detection, winding of the duplicated pair

**GPU differential:** `mesh_field` / `lod_reduce` / `lod_quads` against their CPU references via
the existing dev-console diff command.

**gdUnit4:** build lifecycle, page upload, draw-list count, edit invalidation across levels,
seam continuity.

**Benchmark scene:** adds culled-quad ratio, resident chunk count, builds/second and LoD
milliseconds to the existing overlay.

---

## 10. Budgets and measured triggers

| Quantity | Budget |
|---|---|
| LoD raster | ≤ 2 ms @1440p |
| CPU octree walk | ≤ 0.3 ms |
| Chunk build | ≤ 1.5 ms GPU, 8 builds/frame |
| Quad arena | 32768 pages = 192 MB (exported) |
| Material arrays | 45 MB (16 materials @ 512²) |
| Frame total, with M1–M4 | ≤ 16 ms |

**Triggers, decided by measurement rather than pre-emptively:**

1. Far-occludes-far waste > 30 % → add the temporal second phase (§7.5).
2. Faceting visible in the fade band → +8 B/quad corner normals on the two finest levels (§3.4).
3. Material textures blurry at arm's length → 1024² for the most-used materials (§5).
4. Range short of the world edge at the default pool → raise `max_lod_pages` (linear).

---

## 11. Risks and spikes

- **Indexed indirect draw with `vertex_format = INVALID_ID`.** `CompositePass` proves the
  vertexless half (`extension/src/render/composite_pass.cpp:123-127`); the indexed half is
  unverified on Godot 4.7.1 and the whole no-vertex-buffer design rests on it. Fallback:
  non-indexed with `firstVertex = quad·6` and `quad = gl_VertexIndex/6`, costing 6 vertices per
  quad instead of 4 through a shader that does no vertex fetch anyway.
- **A second draw into the scene framebuffer after the composite.** M1 proved one; ordering and
  framebuffer-format reuse for a second is new.
- **Ray-differential gradients in the compute raymarcher** (§5) — correctness is judged by the
  seam being invisible, which the benchmark flythrough must exercise.
- **Async HiZ readback cost** — 2 KB, but the round trip must not stall the render thread; reuse
  M3's `async_readback` double-buffering.

---

## 12. Out of scope

Cel banding, three-layer shadows, SSGI, SSR and outlines stay in M6. The far-field ortho shadow
map (engine spec §7) will reuse `lod_raster_pass`, which is designed to be callable with a
different view-projection, but M5 does not build it.
