# Voxel Everything — Fjords Terrain and the Sector Heightmap Tier

**Date:** 2026-10-07
**Status:** approved design (sub-project A); B and C sketched only, each gets its own spec
**Start commit:** `a45f3db` (`main`)
**Prior specs:** `2026-09-03-terrain-pipeline-design.md` (the `sector2d` map tier designed and
not built, §5.2–§7); `2026-09-17-stage-authoring-design.md` (manifests, the Lipschitz rule, CPU
mirrors); `2026-09-18-trees-design.md` (the tree stage and leaf pass B replaces for this world);
`2026-10-06-water-voxels-design.md` (water shading C reuses);
`2026-10-07-title-menu-world-types-design.md` (world types as `.tres` + `.pipeline`).

A new world type replicating three reference screenshots
(`~/Pictures/Screenshots/screenshot_20261007_150634.png`, `…150751.png`, `…150810.png`):
fjord valleys with steep forested walls, a turquoise lake or river on the valley floor, snow-capped
ridged peaks, and grey-blue stone crags. The mountains come from Rune Skovbo Johansen's
Phacelle-noise erosion filter (Shadertoy `wXcfWn`,
https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html). Sky and fog are out of
scope.

---

## 1. Decomposition

The request is three sub-projects. Each gets its own spec, plan and implementation, in this order:

| | Sub-project | Delivers |
|---|---|---|
| **A** | Fjord mountains (this spec) | The sector heightmap tier, the erosion map stage, field and band stages, snow material, `fjords.pipeline`, the Fjords world type |
| **B** | Conifers | A new tree species placed on A's height; dense forest on steep walls; forest coverage at distance |
| **C** | Terrain-generated water | Valley lakes at a fixed level, visible near and far |

A ships on its own: dry fjords with snow. §11 records what is already known about B and C.

## 2. Goals and non-goals (A)

Goals:

- A **Fjords** world type, selectable in Create New World; the seed shifts the domain as for
  every other type.
- About **300 m of relief**: peaks at ~W + 300 m, valleys 200–400 m wide, floors 15–40 m below
  the water level W, walls up to ~68°, erosion gullies on every slope.
- Long winding fjord valleys, not only basins.
- A **snow** material from `terrain_textures_vol2/snow`, placed by height, slope and ridge.
- The **`sector2d` heightmap tier**, so the erosion filter is paid once per texel rather than
  per voxel and per pixel.
- Default, Mesas and Flat unchanged: byte-identical generated `field.glslh`, no golden moves.

Non-goals:

- Trees (B), water (C), sky, fog.
- The rest of the terrain-pipeline spec's §7: a general context scheduler, resource aliasing,
  `//!iterate`, world scope, CPU planner stages, props. The manifest syntax stays as that spec
  designed it, so they can be added later.
- A slope limiter that guarantees the Lipschitz bound by construction (§6.4 says why).

## 3. Why a sector tier

`ErosionFilter` is 3 FBM octaves plus 5 erosion octaves of 16 cells each, with an `exp`, a
`cos` and a `sin` per cell. A field stage runs at every voxel of every brick during streaming and
**7× per near-field pixel every frame** (`terrain_source_normal` → `base_field_gradient`); the
first trees stage took the frame from ~17 ms to 45.5 ms for that reason. B's tree placement also
needs the ground height at the 3×3 neighbouring tree cells for every sample, which would multiply
the filter again. The Shadertoy bakes its heightmap into a buffer for the same reason.

The set-1 seam already reserves this: `binding 1 = SectorMap`, `binding 2..N` for sampled
resources, bound unconditionally by every field consumer (`render/field_context_set.cpp`:
"Plan A ships an empty sector map … Plan B replaces this"). This spec is that Plan B, cut to what
A needs.

## 4. The sector heightmap tier — GPU

### 4.1 Authoring

A pipeline may list `//!kind map` stages ahead of its field stages:

```
//!stage     fjord_height
//!kind      map
//!domain    sector2d 256x256
//!out       sector.terrain : image2d_rg16
//!param     ...
```

Each map stage runs as one dispatch per sector, in pipeline order, with a barrier between map
stages. Field stages read a resource with `//!sample sector.terrain : texture2d_rg16`.
`resolve_pipeline` stops rejecting map stages (`pipeline.cpp`'s "Plan A resolves field stages
only"). Map stages carry no `//!cpu`; the CPU reads their output (§5), so a map stage does not
make a pipeline GPU-only.

Validation, in the compiler and naming the stages involved: a map stage after a field stage; a
`//!sample` of a resource no map stage writes; producer/consumer format mismatch; a map stage
with no `//!domain`; a domain other than `sector2d`.

**Pipelines with no map stage generate byte-identical source.** Sample helpers and resource
declarations are emitted only when the pipeline declares a resource.

### 4.2 One sector

- 409.6 m square (`kSectorRegions = 16` regions of 25.6 m).
- 256² texels at **1.6 m**, plus a **2-texel apron** on each side (260² stored) that the bake
  computes itself, so no sector reads a neighbour.
- **RG16 unorm.** R: height normalised over `[W − 64, W + 448]` (7.8 mm steps). G: ridge map,
  −1…1 mapped to 0…1.
- 260² × 4 B ≈ 270 KB per sector.

### 4.3 Residency

- Resident: every sector within `stream_radius_m` (4000 m) plus one sector of the camera, about
  360 sectors, ≈ 100 MB.
- Storage: one `texture2d_array`, one layer per slot, a free list.
- Bake order: nearest first, at most `sector_bakes_per_frame` (default 4) per frame. Estimated
  cost about 1 ms of GPU time per sector.
- Eviction: beyond the radius plus a hysteresis margin of one sector.

### 4.4 Addressing

A camera-centred 24 × 24 toroidal window (24 × 409.6 m = 9.8 km, covering the 8.8 km diameter)
lives in the existing `SectorMap { int slot[]; }` buffer as `(sx, sz, slot)` int triples at
`3 · (mod(sz, 24) · 24 + mod(sx, 24))`. A lookup compares the stored coordinate, so a stale cell
reads as not resident. The binding-1 layout stays as it is today, which is what keeps other
pipelines' generated text unchanged.

### 4.5 Sampling

A uniform cubic **B-spline** over a 4 × 4 neighbourhood with `texelFetch`, returning the height
and its analytic gradient.

- Not bilinear: bilinear is C⁰, and the 5 cm finite-difference normal facets at 1.6 m texels.
- Not Catmull-Rom: it overshoots, and its gradient is not bounded by the texel differences.
- **The B-spline's gradient is a convex combination of per-axis texel differences**, so
  `|∂h/∂x| ≤ max |Δₓh| / 1.6 m` and likewise for z. That turns the field's Lipschitz bound into
  a property of the texels the bake can check exactly (§6.4).
- It does not interpolate; it smooths by about one texel, which at 1.6 m is below the finest
  gully (~19 m).

### 4.6 Ownership

A new `SectorContext` (`extension/src/render/sector_context.{h,cpp}`) owns the texture array, the
window buffer, the bake pipeline per map stage, the stats buffer (§6.4) and the readbacks. It is
created in `RenderOrchestrator::ensure_gpu_graph()` and torn down in the documented order, before
the atlas. `FieldContextSet` stops creating its own placeholder sector map and references the
shared buffers. For a pipeline with no map stage, `SectorContext` holds today's single `-1`
buffer and no array.

The window is rewritten on the frame the camera crosses a sector boundary or a bake completes;
the write is recorded before the frame's first field consumer.

## 5. The sector heightmap tier — CPU

### 5.1 The CPU reads the GPU's bytes

As the terrain-pipeline spec's §6.3 designed: after a bake, the existing `AsyncBufferRead` copies
the layer into a host `SectorCache`, and the stage's C++ mirror runs the same B-spline over the
same 16-bit texels. CPU and GPU never evaluate erosion separately, so the filter's `exp`, `sin`
and `sign()` cannot diverge between them. What remains is float rounding in the B-spline, inside
`test_field_diff.gd`'s existing tolerance.

`FieldResources` (`stage_library.h`, today `struct FieldResources {};`) gains a pointer to the
`SectorCache`. `PipelineFieldGenerator` passes it to every stage, as the signature already does.
The CPU sampler is pure C++ in `extension/src/terrain/` and joins `pure_sources`.

### 5.2 Host cache

- Entries are immutable `std::shared_ptr<const SectorTexels>`, keyed by sector coordinate.
- The map is guarded by a `std::shared_mutex`. A sampling worker holds its pointer for the
  sample, so eviction never frees texels under it.
- Evicted with the GPU slot. ≈ 100 MB at full residency.

### 5.3 Gating

Nothing that bakes field data into a store runs over a sector that is not resident on both
sides. Three producers ask `SectorContext::ready(world_aabb)` before taking a job:

1. Region stream-in in `WorldStreamer::run_frame`. A held region is skipped and retried next
   frame, the shape the existing `repair_queue_` uses.
2. LoD chunk selection in `LodSystem`.
3. Collider and island jobs on the CPU.

Sectors bake one ring past the stream radius, so residency stays ahead of streaming in steady
state. `ready()` is always true for a pipeline with no map stage.

### 5.4 Not resident

Only per-frame consumers (the raymarcher's normal, grass, leaves) and arbitrary CPU queries can
evaluate a non-resident sector. The stage's fallback height is **`W − 64`**, the bottom of the
encoded range: open air above the valley floor, so nothing false is solid. `VoxelWorld.raycast`
far from the player before residency reports no hit; `GroundSpawn` already retries for 10 s.

### 5.5 Failure handling

| Failure | Behaviour |
|---|---|
| Slot pool exhausted | Evict the farthest sector; if one still cannot fit, its regions are held, never streamed on fallback data |
| A map stage fails to compile | The existing pipeline load error and preflight; the world does not initialise and `GroundSpawn` warns |
| A readback never returns | The sector stays not ready and its regions held; logged once per sector |
| Teardown, or a world rebuilt by the title flow | `SectorContext` frees its RIDs and clears the host cache; keys carry the pipeline hash and seed, so a rebuilt world never reads old texels |
| A sector over the slope limit | Logged once, counted in the stats (§6.4) |

**Hooks rule.** gdUnit probes read the texels, window and stats the shipping `SectorContext`
wrote during real frames. No bake is rebuilt inside `hooks_render.cpp`.

## 6. The fjord map stage

### 6.1 Files

- **`shaders/erosion.glslh`** — `PhacelleNoise` and `ErosionFilter`, ported closely from the
  Shadertoy, with Rune Skovbo Johansen's MPL-2.0 header verbatim and the source URLs. MPL is
  file-level copyleft, so isolating the port keeps every other file under the repo's own terms.
  The `Animate*` calls are dropped; the demonstration's `GetTreesAmount`, `pack4` and
  `COMPARISON_SLIDER` are not ported.
- **`shaders/stages/fjord_height.map.glsl`** — the recipe below, plus `noised` and `hash`. The
  Shadertoy's Common tab was not available, so these are our own: iq's gradient noise with
  analytic derivatives, and an integer `pcg2d` hash, which keeps cells stable at the ±8 km seed
  offsets.

### 6.2 Units

Shader space is the Shadertoy's normalised map: `p = (xz + VE_FIELD_OFFSET.xz) / L`. Output
height in metres is `W + V · (h − h_water)`. **L = V = 2000 m.** Equal scales keep the filter's
slope semantics (`assumedSlope`, `onset`, rounding) as the author tuned them. At this scale the
base FBM wavelength is ~670 m, the first gully octave ~300 m and the finest ~19 m (12 texels).

### 6.3 Recipe, in bake order

1. **Base height.** `FractalNoise` with frequency 3, amplitude 0.125, 3 octaves, lacunarity 2,
   gain 0.1 — the Shadertoy's values — carrying height and derivative.
2. **Fjord carving, before erosion.** A domain-warped noise of ~3 km wavelength whose zero set
   traces long winding lines. Height drops by `valley_depth` across `valley_width` (300 m) of
   that line, with an analytic derivative added to the slope. Carving first means erosion's
   `fadeTarget` sees these as valleys, so gullies drain into them.
3. **Erosion.** `ErosionFilter` with the Shadertoy's static defaults:

   | Parameter | Value |
   |---|---|
   | scale | 0.15 |
   | strength | 0.22 |
   | gully weight | 0.5 |
   | detail | 1.5 |
   | rounding | (0.1, 0.0, 0.1, 2.0) |
   | onset | (1.25, 1.25, 2.8, 1.5) |
   | assumed slope | (0.7, 1.0) |
   | cell scale | 0.7 |
   | normalisation | 0.5 |
   | octaves, lacunarity, gain | 5, 2.0, 0.5 |
   | height offset | (−0.65, 0.0) |

   The fade target is `clamp(n.x / (0.125 · 0.6), −1, 1)` and the eroded height
   `n.x + h.x + offset`, as in the Shadertoy's `Heightmap`.
4. **Fjord profile.** A monotone remap of the eroded height around `h_water`. It flattens the
   floor into a broad U 15–40 m below W and steepens the lowest 60 m of wall: flat water between
   sheer sides, as in screenshots 1 and 3.
5. **Encode.** R as in §4.2, clamped; G is the ridge map.

**Every tunable is a `//!param`**: the table above plus `L`, `V`, `W`, `h_water`, `valley_depth`,
`valley_width` and the warp. Look tuning happens in `fjords.pipeline` with no rebuild.

### 6.4 The slope limit and the bound

The bake records each sector's largest per-axis texel difference over 1.6 m, including the apron,
with an `atomicMax` into a per-slot stats word that rides along with the readback.

- **`S_max = 2.5`** (68°). By §4.5, `|∇h| ≤ √2 · S_max`, so the field's bound is
  `sqrt(1 + 2 · S_max²) = 3.67`.
- A sector over `S_max` is logged once and counted. The parameters are tuned to stay under it,
  and §9's slope test holds them there.
- **It is not enforced by construction.** A slope limiter (a min-plus envelope) has to see
  neighbouring sectors, and every texel must stay a pure function of its position.
- **Cost:** 3.67 against Default's 1.99 roughly doubles near-field raymarch steps over this
  terrain. §9 measures it.

## 7. Field stages, materials and the pipeline

### 7.1 `shaders/stages/fjord.field.glslh` (C++ mirror `ve::stage_fjord`)

```
//!stage     fjord
//!kind      field
//!sample    sector.terrain : texture2d_rg16
//!out       sdf : float
//!out       height : float
//!out       slope : float
//!out       ridge : float
//!lipschitz add 3.67
//!cpu       ve::stage_fjord
```

The B-spline (§4.5) gives height and gradient from one set of 16 fetches; `slope = |∇h|` costs
nothing more. `height` keeps its existing meaning, metres above `SURFACE_Y`, and
`sdf = p.y − SURFACE_Y − height`. The bound's derivation from `S_max` is written in the stage's
comment, as `relief.field.glslh` does for its own.

It also defines **`fjord_ground(vec2 xz)`**, returning height, slope and ridge at any XZ, with a
C++ twin. This is the hook B and C use, the way `trees_ground_h` serves the current tree stage.

### 7.2 `shaders/stages/fjord_bands.field.glslh` (C++ mirror `ve::stage_fjord_bands`)

Reads `sdf`, `height`, `slope`, `ridge`; writes `material`. No `//!lipschitz`, because it writes
no `sdf` — the rule `height_bands` follows. Thresholds are on the elevation above water,
`e = SURFACE_Y + height − W`, and every one is a param:

| Condition, in order | Material |
|---|---|
| `sdf > 0` | air |
| `e < 2` | `ground_01` — lake bed and shore |
| `slope > 1.2` (50°) | `breakstone` |
| `e` above the snow line and `slope < 1.0`; `e > 240`, `slope < 1.4` | `snow` |
| otherwise | `grass_01` |

**Snow line:** `e` ≈ 170 m, wavering ±25 m with a hashed value noise. Each step of `ridge` toward
+1 lowers it by up to 40 m, so snow runs down the ridges as in screenshot 1. The relaxed limit
above W + 240 m makes steep peaks read white, not grey.

### 7.3 `assets/pipelines/fjords.pipeline`

```
lipschitz 3.7
stage stages/fjord_height.map.glsl
stage stages/fjord.field.glslh
stage stages/fjord_bands.field.glslh
```

No `cave`, no `trees`: the leaf pass is not created (world-types §10 item 2). B appends its
conifer stage after `fjord_bands`.

`demo/world_types/30_fjords.tres`: `display_name = "Fjords"`,
`pipeline_path = res://assets/pipelines/fjords.pipeline`. `ground_at`'s 600 m start clears the
highest peak (~W + 300 = 351 m).

### 7.4 Snow material

- Atlas layer **`09`**, five maps converted from `terrain_textures_vol2/snow/T_snow_*.tga` to
  512² PNG with the same `convert … -resize 512x512! -strip PNG24:` command
  `convert_materials.sh` uses. A one-off: that script aborts at its bark entry on this machine.
  `snow` is still appended to its `MATERIALS` list, which `test_material_table.cpp` asserts
  equals the table.
- Row, appended after `water`:
  `{"snow", "09", 1.0f, 0.0f, {0,0,0}, {0.86f, 0.88f, 0.92f}}`. Hardness 1.0: soft. Snow takes
  id 10; nothing renumbers.
- `shaders/material_table.glslh` and its byte-exact golden regenerate.
- **Bloom risk.** Snow is the brightest albedo in the game, and spill in this engine is bloom
  past the glow threshold, not GI. If sunlit snow halos in the capture, the fix is snow's albedo,
  not the beauty stack.

## 8. Files

```
extension/src/terrain/sector.{h,cpp}             sector coords, window index, B-spline (pure)
extension/src/terrain/sector_cache.{h,cpp}       host cache (pure)
extension/src/terrain/stage_library.h            FieldResources gains the cache pointer
extension/src/terrain/pipeline.cpp               map stages resolve; validation (§4.1)
extension/src/terrain/field_codegen.cpp          sample helpers and resource declarations
extension/src/terrain/builtin_stages.cpp         stage_fjord, stage_fjord_bands
extension/src/render/sector_context.{h,cpp}      GPU owner, bake, readback, ready()
extension/src/render/field_context_set.{h,cpp}   references the shared sector buffers
extension/src/render/orchestrator.cpp            create / teardown SectorContext
extension/src/render/world_streamer.cpp          gating
extension/src/lod/lod_system.cpp                 gating
extension/src/world/material_table.h             snow row
shaders/erosion.glslh                            MPL-2.0 port
shaders/stages/fjord_height.map.glsl
shaders/stages/fjord.field.glslh
shaders/stages/fjord_bands.field.glslh
shaders/material_table.glslh                     regenerated
assets/materials/09_*.png
assets/pipelines/fjords.pipeline
demo/world_types/30_fjords.tres
tools/convert_materials.sh                       MATERIALS gains snow
tools/fjord_capture.gd
```

The collider-job gate's exact site is found in the plan; it calls the same `ready()`.

## 9. Testing, commit order and measurement

**Baseline first.** Record the gdUnit failure set and run the native suite on clean `a45f3db`.
The set drifts; an older record is stale by default.

1. **Sector maths, native only.** Sector coordinates, the window index, the CPU B-spline, and
   `SectorCache` under concurrent readers during eviction. The key case: on random synthetic
   texel grids, `|∇h| ≤ √2 · max per-axis difference / texel` at dense sample points.
2. **Pipeline resolves map stages.** Compiler tests for every rule in §4.1. **The four existing
   pipelines generate byte-identical `field.glslh`**; the golden does not move.
3. **`SectorContext`**, proven on a tiny fixture map stage under `tests/fixtures/` (not
   `assets/pipelines/`, so neither the world-type nor the pipeline enumerators see it). gdUnit:
   the same sector baked twice gives identical bytes; a sector re-baked after eviction gives
   identical bytes; window lookups reject stale coordinates; the fixture's GPU field equals its
   CPU field.
4. **Gating.** On a cold world, no region marks and no LoD chunk builds over a sector before that
   sector is ready on both sides.
5. **Snow material.** PNGs, row, `MATERIALS`, regenerated `material_table.glslh` and golden.
6. **Fjords.** `erosion.glslh`, the three stages and mirrors, the pipeline and `30_fjords.tres`.
   - `test_field_diff.gd` picks the pipeline up; its harness waits for sector readiness before
     diffing. Seed 0 and a far-offset seed.
   - A slope test reads the shipping bake's stats over a ±3 km area at three seeds and asserts
     zero sectors over `S_max`.
   - `test_lipschitz_sampled.cpp` is native and has no GPU bake, so it skips pipelines with map
     stages; step 1's B-spline case and the slope test cover the bound.
   - World-type discovery now expects Default, Mesas, Flat, Fjords.
7. **Capture and look tuning.** `tools/fjord_capture.gd`, three poses — one per screenshot: the
   valley floor looking down a fjord, a high ridge looking out, an aerial view along a valley.
   Frames go to the user beside the references; tuning happens in `fjords.pipeline` and is
   recorded in §12.

**Measurement**, reported, not gating:

- Frame time, Fjords against Default, interleaved A/B/A, wall percentiles only (GPU timestamps
  are invalid on this machine). Fjords and Default are different terrain, so this prices the
  world type, not the tier alone.
- Cold fill to streaming-quiet at 4 km.
- GPU and host memory at full residency.

**Golden policy.** No existing golden should move. If one does, stop and report rather than
re-record.

## 10. Risks

| Risk | Handling |
|---|---|
| A doubled raymarch step count (bound 3.67) costs too much | Measured in §9. Dials: `S_max` and the profile's wall steepening, both params |
| Sector bake budget lags fast flight, holding regions | Nearest-first order and a one-ring prefetch; `sector_bakes_per_frame` is the dial |
| ~200 MB GPU + host at 4 km | Measured. A coarser far-sector resolution is the upgrade if it bites; not built |
| The slope limit is checked, not guaranteed | Logged and counted per sector; the slope test runs over a wide area at several seeds |
| The fjord carving reads as canals, not valleys | Width, depth and warp are params; the capture decides |
| Snow blooms | Tuned by albedo, never by editing the beauty stack |

## 11. Sketches of B and C

Recorded so A's interfaces serve them. Each gets its own spec.

**B — conifers.** A new tree species: tall trunk plus a conical crown. Placement reads
`fjord_ground` (one B-spline per tree cell instead of an erosion evaluation), so trees can stand
on slopes well past the current `max_slope` 0.6, as the screenshots demand. Open questions for its
brainstorm: crowns as SDF voxels (drawn by LoD everywhere, the blocky look of the references, a
streaming cost) versus leaf cards (the existing technique, ~250 m reach) with far-field forest
coverage; density on a steep slope; whether the leaf pass becomes species-aware or a second module
exists. The current trees stage costs ≈ 3.6 s of cold fill; a forest this dense needs its budget
set up front.

**C — terrain-generated water.** A lake surface at W wherever `fjord_ground` is below it. Known
blockers: the water spec ruled out terrain-generated water, and the transparency shell's
candidate scan (`ve::shell_candidates`, transparency spec §5 ceilings) sees ops, volumes and
overrides but not a transparent material a stage produces. A kilometre-long fjord needs water to
the horizon, beyond the shell fade band. The ridge map (G channel) is the drainage input if rivers
are wanted.

## 12. Deviations

(Recorded during planning and implementation.)
