# Voxel Everything — Engine Design Spec

**Date:** 2026-08-12
**Status:** Approved design, pre-implementation
**Scope:** Tech demo / portfolio piece — destructible smooth-SDF voxel terrain, raymarched near field, Voxy-style LoD far field, cel-styled beautification.

---

## 1. Fixed Constraints (decided)

| Decision | Choice |
|---|---|
| Purpose | Tech demo / portfolio piece; minimal gameplay (fly/walk, destroy) |
| Engine | Godot 4.7, Forward+, Jolt physics, `godot_cpp` GDExtension, RenderingDevice |
| GPU API | Vulkan on Linux (dev); d3d12 on Windows spot-checked only |
| Raymarching | **Compute-only** sphere tracing (no HW RT dependency) |
| Voxel resolution | 5cm (L0) |
| View distance | 2–4km; near field raymarched 0–150m (fade 120–150m), LoD beyond |
| World | Bounded 4096×1024×4096m, procedural base + persisted edits |
| Multiplayer | Single-player, forever. No netcode constraints. |
| Target perf | 60fps @ 1440p on RTX 4070 Laptop |
| Architecture | Approach A: sparse brick atlas + full-screen compute raymarch |

Existing assets: validated GDScript spikes (HW-RT pipeline + SDF sphere tracing) in
`spike/` (throwaway), RenderingDevice API reference in `docs/api/renderingdevice.md`,
gdUnit4 installed.

---

## 2. SDF Data Model

**Voxel format.** L0 = 5cm voxels in **16³-voxel bricks (0.8m)**. Per voxel:

- `uint8` SDF, fixed global encoding −0.64m…+0.64m (~5mm steps = 10% of a voxel;
  sufficient for rendering and dual contouring).
- Material: **2-bit index into a per-brick palette of 4 slots**; palette slots hold
  global `uint16` material IDs. World supports 65,536 materials; a single 0.8m brick
  holds at most 4. Overflow (5th material in one brick): nearest existing palette
  entry wins, one-time warning logged.

Brick size ≈ 4KB SDF + 1KB material + header ≈ **5.5KB**.

**Procedural base + analytic edit overlay.** Dense storage of a 4×4km surface at 5cm
is ~50GB even narrow-banded — never materialized. Instead:

- Base terrain = deterministic **generator function G(position) → (SDF, material)**,
  evaluated on demand. G is evaluated in **GPU compute** (a CPU mirror exists for
  differential testing).
- Destruction = **ordered CSG op list per region** (`{type, position, radius,
  material}`, ~32B/op): sphere-subtract, sphere-add(material), etc.
- Brick contents = G with the region's ops applied in order, **evaluable at any
  resolution** — every LoD level is generated independently and exactly; no
  downsampling cascade anywhere in the engine.
- Saves = generator seed/params + edit log (tiny).
- Op-list overflow (>256 ops/region) consolidates into explicitly stored override
  bricks.

**Residency.** Only L0 bricks near the camera are resident: ~150m working set ≈ 190k
bricks ≈ **0.7–1.0GB** pool (3D-texture atlas + free lists), distance-LRU eviction.
(Residency radius and raymarch max distance are memory-coupled — 150m is the
budget-feasible radius; extending the raymarched field means shrinking something
else.)
Coarse levels exist only transiently while baking LoD chunks, then evaporate; the far
field lives purely as baked meshes/textures.

**Indexing.** Two levels: dense **region map** (regions = 32³ bricks = 25.6m; world
grid 160×40×160 for the 4096×1024×4096m extent, ~4MB) → sparsely allocated GPU
region buffers of brick-slot indices into the atlas. All consumers (raymarcher, meshers, bakery) walk this indirection.

**Per-brick acceleration.** Min–max mip chain (8³/4³/2³, ~0.6KB/brick) for
empty-space skipping during sphere tracing.

---

## 3. Near-Field Raymarcher

**Per frame** (budget 4–6ms @1440p):

1. **Primary visibility** — compute pass at ~0.66× resolution + temporal upsample.
   Three-level traversal per pixel:
   - Region DDA (25.6m) → brick DDA (0.8m) via indirection → in-brick sphere tracing
     accelerated by the per-brick min–max mips.
   - Hit refinement: ~4 secant/bisection steps; normal = central-difference gradient;
     material from palette. Max ray distance 200m (small margin past the fade band).
2. **Sun shadow rays** — same traversal, one ray/pixel; sphere tracing gives
   contact-hardening soft shadows with no shadow-map acne.
3. **G-buffer output** — albedo, linear depth, oct-encoded normal, material ID.
   Cel shading is deferred (Section 7), not done here.

**Compositing with raster (load-bearing).** Raymarched depth is blitted into Godot's
scene depth buffer **before the opaque pass**; LoD geometry and Godot dynamic objects
depth-test against it normally. Mutual occlusion is exact both ways.

**Godot integration.** All custom passes live in a `CompositorEffect` (available to
godot_cpp) on a Forward+ WorldEnvironment. Terrain raymarch hooks pre-opaque;
beautification post-opaque. Godot keeps swapchain, dynamic objects, UI.

**Near/far seam.** Dithered depth fade over 120–150m, cross-fading into LoD.

**Multi-target raymarching (islands).** Dynamic island bodies are additional march
targets:

- **Tiled target culling:** screen split into 16×16px tiles; island world-AABBs
  projected per tile (tiled-light-culling style) → each pixel marches only the 0–3
  islands overlapping its tile/depth range, never all 32.
- Per pixel: march static terrain + listed islands (ray → island local space via
  inverse body transform → same sphere-trace GLSL); nearest hit wins. Identical
  G-buffer path → islands shade/shadow/reflect exactly like static terrain.
- Island SDF storage: **dense per-island texture** (AABB at 5cm, uint8 + palette +
  own min–max mip), extracted at carve time.

**Known spike items** (budgeted in plan): exact pre-opaque depth-injection ordering
in Godot 4.7; accessibility of Godot's normal-roughness buffer for screen-space
effects on dynamic objects (fallback: tiny normal pre-pass or constant ambient for
dynamic objects); exact compositor hook points for the pass order in Section 7.

---

## 4. LoD System (far field, Voxy-style)

**Hierarchy** — 4 levels, each 4× linear, aligned to the brick/region grid:

| Level | Chunk size | Mesh sampling | Bake texture | Typical use |
|-------|-----------|---------------|--------------|-------------|
| L1 | 25.6m (1 region) | 0.4m (64³) | 128² | 150m–1km |
| L2 | 102.4m | 1.6m | 64² | 1–2km |
| L3 | 409.6m | 6.4m | 32² | 2–4km |
| L4 | 1638.4m | 25.6m | 32² | horizon |

Per frame, a chunk renders at the coarsest level under a screen-space-error
threshold (CDLOD-style). Camera motion re-selects levels; it does not rebuild
anything.

**Geometry.** Per chunk: coarse **surface-nets mesh** evaluated directly from
G + edit ops at that level's resolution (Section-2 any-resolution property; no L0
residency needed to build distant chunks). Imprecise is fine — detail comes from the
bake. **Skirts** on chunk borders hide inter-level cracks; no stitching meshes.

**Texture bake (Voxy trick adapted to smooth SDF).** Per chunk, bake **albedo +
normal** by raymarching the higher-fidelity representation (G+edits at ~4× the
chunk's mesh sampling) along each triangle's **dominant axis** — triplanar-style
projection, so cliffs/overhangs/cave mouths bake correctly. GPU compute into texture
arrays; BC compression if the budget demands.

**Streaming & invalidation.** Background priority queue keyed by screen-space error.
Camera moves → build missing chunks (~1–4ms GPU each: mesh sub-ms, bake the rest; a
few per frame). **Edit op → dirty intersecting chunks at all 4 levels** → rebuild in
priority order; distant edits rebuild lazily.

**Fade-band quality contingency.** L1 starts at 150m; 0.4m mesh features there are
~3–5px. The bake carries interior detail and cel outlines forgive silhouettes, but
if benchmarking shows the fade band too coarse, densify L1 mesh sampling to 0.2m
within 300m — decided during implementation, no structural change.

**Drawing.** CPU frustum-cull (few thousand AABBs), one **indirect multi-draw per
level** via RenderingDevice, textures as arrays. All LoD shares the near field's cel
lighting GLSL — the 150m seam is a fade, not a style change.

Memory: ~5–8k resident chunks ≈ **300–500MB** textures + trivial geometry.

---

## 5. Destruction & Connectivity

**Edit pipeline.** An edit appends a CSG op to every touched region. Per frame, all
pending edits batch into one GPU pass: dirty bricks re-evaluate `G + ops` in place
(a 5m blast ≈ 2k bricks ≈ 8M voxel evals — sub-ms). Visible latency: same or next
frame. Fan-out: raymarch set (implicit), physics remesh queue, LoD chain (all
levels), connectivity.

**Connectivity analysis (floating islands).**

- **Global persistent occupancy grid**, 0.8m cells = one bit per brick ("any solid
  voxel"), updated incrementally from brick-regen readback (1-frame latency —
  imperceptible).
- On edit: localized **flood fill from the window boundary** (64³ cells ≈ 51m,
  expanding if the frontier is reached); boundary = anchored to static world.
- **6-connectivity (face-only)** defines support — edge/corner contact never counts,
  so pieces touching only through cell corners fall rather than wrongly hang.
- **Marginal-contact refinement:** when a component's only anchor links are thin
  (<~2 cell faces of contact), a tiny GPU check samples the true 5cm SDF along the
  contact plane before declaring support. Cell grid decides the common case; true
  SDF arbitrates border cases — including contacts spanning chunk borders.
- Solid cells unreachable from the boundary → connected-component labeling → each
  group = one island.
- Runs once per frame after all that frame's edits — simultaneous blasts can't race.

**Island lifecycle.**

1. **Carve out** of the static SDF (automatic subtract op → correct crater).
2. Extract dense island SDF texture (raymarched render, Section 3).
3. Spawn **Jolt rigid body**: collision = **greedy box-merged compound** from 0.8m
   occupancy (≤256 boxes), mass/inertia from solid volume.
4. Body **sleeps ~2s → re-merge**: island SDF sampled at rest pose and stamped back
   as a CSG paste-op; body despawned. Rubble permanently accumulates as terrain.

**Guardrails (one-constant tunables).** ≤32 island bodies (oldest sleepers merge
early), ≤64 total active dynamic bodies, oversized components split along weakest
box seams, components <~0.2m³ become plain mesh debris (not raymarch targets).
Island texture memory: 5cm sampling, halved to 10cm for islands with AABB >8m,
total island-texture pool capped ~512MB (largest mergers forced to sleep early).

**Demo edit tools** (minimal): sphere-subtract (explosion + radial impulse), sphere-add,
material paint brush, line drill. All are just op emitters.

---

## 6. Physics

**Static terrain collision.** Dual-contoured meshes from L0 bricks at **0.1m
(half-res)** — 5cm collision is wasted on Jolt; half-res keeps walking smooth and
halves triangles. Chunked at 12.8m (16 bricks); ~5–20k tris/chunk. GPU compute
meshing + async double-buffered readback (~120KB/chunk) → `PhysicsServer3D` direct
(no scene-tree nodes), Jolt concave shapes in a static compound. Collision streams
in a **~64m radius around the player + small bubbles around active bodies**. Edit →
remesh → collidable again in 1–2 frames.

**Dynamic bodies.** Character = standard `CharacterBody3D` capsule. Islands =
box compounds (Section 5). Small debris = single-box bodies + cheap DC render meshes.
Explosions apply radial impulses via `PhysicsDirectSpaceState`. Jolt sleep events
drive the re-merge hook. CCD on fast debris. Physics mesh pool ~80 chunks (~1M tris
worst case) — comfortable for Jolt broadphase.

**Failure policy.** Readback/shape-build failure → log, keep previous collider,
retry next frame. Stale collision for a frame beats a hole players fall through.

**Deliberately not simulated:** structural stress (support is binary), island
fracture on impact (islands land whole; a second explosion breaks them), fluids /
granular flow. All future-work notes.

---

## 7. Beautification & Shading

**Core principle: one merged G-buffer, one deferred lighting stack.** Raymarched
terrain + islands AND rasterized LoD write albedo / oct-normal / linear-depth /
material-ID into the same offscreen G-buffer (LoD from its bakes). One deferred pass
shades everything identically — the near/far seam is mathematically invisible.

**Cel shading** (shared GLSL; mirrored in `ShaderMaterial`s on dynamic objects):
3–4 band quantized diffuse with paintable ramp, hue-shifted shadow tint, specular
band on glossy materials, rim light. **Outlines:** full-screen depth+normal
discontinuity detection, pixel-thin darkened-albedo lines, late in the frame —
unifies raymarched terrain, LoD, and debris into one image.

**Shadows, three layers:**

1. Near field: raymarched sun rays (Section 3).
2. Far field: one world-covering **ortho shadow map from LoD geometry** (2048²,
   ~2m/texel), redrawn lazily on LoD rebuilds.
3. Screen-space **contact shadows** (short depth march toward sun, half-res,
   dithered, bilateral upsample) — grounds dynamic objects, sharpens LoD contacts.

**SSGI.** Half-res, 6–8 horizon taps/pixel from this frame's depth/normals +
reprojected previous frame's lit color for one bounce; temporal accumulation with
neighborhood clamping. Feeds the ambient term of the cel bands. Dynamic objects
included only if the normal-buffer spike pans out, else constant ambient.

**SSR.** Half-res march against the merged depth buffer — reflects LoD, terrain,
objects alike. Quality tier: **materials flagged glossy (water, ore) optionally cast
true SDF reflection rays** in the raymarcher (short min-max-accelerated rays).
Default on for the demo GPU, toggleable.

**Frame order.** Raymarch G-buffer → LoD raster into G-buffer → SSGI → deferred cel
lighting → inject color+depth → Godot opaque (cel objects) → contact shadows → SSR →
outlines → Godot glow/tonemap. (Compositor hook-point spike noted in Section 3.)

**Budget @1440p:** raymarch 4–6ms, LoD ~2ms, SSGI ~1.5ms, SSR ~1.5ms, shadows ~1ms,
outlines ~0.3ms → ~12ms worst case in a 16ms budget. Every effect has a quality/off
toggle in the debug menu.

---

## 8. Project Structure, Error Handling, Testing

```
extension/src/
  world/        brick atlas, region indirection, residency/LRU        (pure C++)
  generator/    G(pos) eval, edit op lists, consolidation             (pure C++)
  connectivity/ occupancy grid, flood fill, island components         (pure C++)
  mesh/         dual contouring, surface nets, box merging            (pure C++)
  physics/      PhysicsServer3D glue, collider streaming              (Godot glue)
  render/       raymarch pass, tiled island culling, LoD raster/bakery,
                beauty passes, compositor wiring                      (Godot glue)
  demo/         player, edit tools, debug menu
shaders/        .glsl files, shared includes (concatenated at load),
                hot-reload keybind in dev builds
```

Hard boundary: **pure C++ cores with zero Godot types** behind a thin Godot-facing
layer (~5 exposed classes: `VoxelWorld`, `VoxelEditTool`, `VoxelSettings`, the
`CompositorEffect`, debug HUD). One shared `common.glsl` for SDF/cel/lighting so
raymarcher, LoD, and deferred pass can never drift apart.

**Error handling.** Dev: Vulkan validation layers + verbose RD checks. Release:
fail-soft — shader compile failure → magenta fallback + log; pool exhaustion → LRU
evict, never crash; GPU job/readback failure → keep last frame's resource, retry;
connectivity/meshing anomaly → warn + no-op. A stale frame beats a crash during a
demo recording.

**Testing.**

- **Native unit tests (doctest + CTest)** for every pure C++ core: flood-fill
  invariants (anchored cells never become islands), occupancy roundtrips, palette
  packing, op-list ordering/consolidation, SDF paste/unpaste, mesher tables. No
  Godot, no GPU, milliseconds. Implementation follows TDD.
- **GPU differential testing:** CPU references for brick-eval and meshing; dev
  console command runs both and diffs — catches shader/reference drift.
- **gdUnit4** for the Godot-facing layer (world load, edit-tool → op emission,
  collider lifecycle).
- **Benchmark scene:** scripted flythrough + canned destruction, per-pass GPU timing
  overlay, budget warnings (>6ms raymarch, >16ms frame). Doubles as the portfolio
  capture rig.

---

## 9. Deferred / Out of Scope

- **Terrain generator G content** (noise, biomes, caves) — separate design session;
  the engine only depends on G being deterministic, GPU-evaluable, and
  resolution-independent.
- Infinite/streaming worlds, structural stress sim, impact fracturing, fluids,
  multiplayer (closed forever).
- HW-RT raytracing acceleration — upgrade path preserved (brick BVH derivable from
  the same tree), not planned.
