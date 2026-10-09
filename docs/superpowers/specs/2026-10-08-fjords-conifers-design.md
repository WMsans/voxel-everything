# Voxel Everything — Fjords Conifers

**Date:** 2026-10-08
**Status:** implemented
**Start commit:** `55bb9ee` (`main`)
**Prior specs:** `2026-10-07-fjords-terrain-design.md` (sub-project A, whose §11 sketches this one;
`fjord_ground`, the sector tier, `fjord_bands`); `2026-09-18-trees-design.md` (the shared-header
pattern, the trunk stage's early-outs, the leaf module whose raster this reuses);
`2026-09-17-stage-authoring-design.md` (manifests, `//!use`, the Lipschitz rule, CPU mirrors).

Sub-project B of the Fjords world: dense conifer forest on the valley walls, visible from the
shore to the horizon, matching the three reference screenshots
(`~/Pictures/Screenshots/screenshot_20261007_150634.png`, `…150751.png`, `…150810.png`). In
shots 1 and 3 every wall below the snow is unbroken forest of narrow, tiered spires, and the
forest still reads as forest kilometres down the fjord.

---

## 1. Goals and non-goals

Goals:

- A conifer species for the Fjords world: trunk as real voxels (choppable, collidable), crown as
  leaf cards.
- Forest on every grass face and on steep faces up to slope 1.7 (~60°), from just above the
  shore to a tree line below the snow, with grove-noise clearings.
- Forest visible at every distance, by three representations that hand off without popping:
  cards near, analytic imposters mid, a forest material far.
- Default, Mesas and Flat unchanged: byte-identical generated `field.glslh`, no frame golden
  moves.

Non-goals:

- Water (sub-project C). Trees stop at the shore band; the fjord floor stays bare ground.
- Conifers in Default or any other world type.
- Making the broadleaf leaf module species-aware. It is untouched apart from §5.4.
- Leaves in the sun cascades, leaves shadowing leaves, or leaves shadowing the ground — the
  trees spec's non-goal, for the same reason.
- Imposters with chop awareness. An edit to a tree past the card reach is seen once it is close.
- Far-field grass colour matching. The same technique applies (§2), but it is its own change.

## 2. Prior art

Researched 2026-10-08. No shipped game draws real trees to the horizon; each hands off between
representations that are cheap in their own distance band.

| Source | Technique | Taken / rejected |
|---|---|---|
| Serious Sam 4, "4 Million Acres" (Croteam, GDC 2019) | Near trees as meshes; **forest imposters** (one camera-facing quad per tree, 8-view texture, 1 km pre-batched blocks, ~8 m jittered grid); **far forest as a terrain material** splatted where the density map has trees; a separate forest-floor material; grass albedo taken from the floor | **The structure of this spec.** Three bands, the 8 m lattice, and a forest material for the far field. |
| Octahedral impostors (Ryan Brucks; Unreal Impostor Baker; Fortnite's upper-hemisphere impostors) | Pre-rendered view atlas, nearest views blended | Rejected for a conifer: it is nearly rotationally symmetric, so only elevation matters, and the profile can be ray-cast analytically with no atlas (§6). |
| Unreal 5.7 Nanite Foliage (The Witcher 4 demo) | Distant foliage as pixel-sized voxels with a normal distribution per voxel | The high end. Equivalent to SDF crowns here, which the brainstorm rejected for the soft card look. |
| Horizon Zero Dawn (GDC 2017) | Deterministic GPU placement evaluated around the player | Confirms hash-and-lattice placement on the GPU at this scale. |
| SpeedTree LOD | Fewer, larger leaf cards with distance; crossfade to billboard | Already the leaf module's density LOD; the crossfade cost informs the 30 m seam (§7). |
| Bruneton & Neyret 2012; Mantler & Jeschke 2006 | Far forest as a terrain shader / vegetation heightfield | Confirms that past resolvability, forest belongs to the terrain's shading. |

## 3. Architecture

**One shared header.** `shaders/conifer.glslh` defines where conifers are and what shape they
have, as pure functions of a cell coordinate plus caller-supplied ground. It knows nothing about
terrain, exactly as `tree.glslh` does, so a native doctest can execute it.

```
ConiferParams                      // from the stage's P.conifers_* params
ivec2 → vec2  conifer_cell_xz      // jittered foot XZ, inside the cell's jitter box
bool  conifer_cell_gate            // hashes + grove noise, cheapest test
bool  conifer_ground_ok(e, slope, treeline)   // shore, tree line, max slope
Conifer conifer_at(cell, ground_y, slope)     // height, crown base, R, tiers, droop, lean, hash
float conifer_profile(Conifer, s)  // crown envelope radius at crown height s, tiers included
float conifer_trunk_sdf(p, Conifer)
float conifer_crown_sdf(p, Conifer)  // 2D distance of revolution; used by the imposter only
```

**Four consumers, three bands.**

| Band | Consumer | Draws |
|---|---|---|
| everywhere | `conifers` field stage | Trunk SDF as `MAT_BARK`; `forest` material in a disc under each crown |
| 0 – `card_reach` (300 m) | conifer cull → conifer card scatter → existing `LeafRasterPass` | Needle cards on tier shells |
| `card_reach` – `impostor_reach` (2500 m) | conifer cull → `ConiferImpostorPass` | One quad per tree, profile sphere-traced in the fragment |
| beyond | none | The far field's own texturing: the `forest` texture's top mip |

Every consumer gets ground from `fjord_ground(xz)` (one B-spline per tree cell, never the
erosion filter) and reads the same `P.conifers_*` params from set 1, so the trunk, the cards,
the imposter and the forest disc cannot disagree about where a tree is.

**Why `card_reach` is 300 m, not the leaf module's 800 m.** Default's groves are sparse, ~1.4k
clumps at 800 m. A forest at 8 m cells and density 0.85 has ~8k trees in an 800 m frustum and
several hundred thousand clumps; at 300 m it has ~1k trees.

**Why imposters need no sun march.** Past `kLodFadeEndM` (150 m) `deferred.comp.glsl`'s
`far_field_owns()` mins the sun map into every pixel. The imposter band starts at 300 m, so
terrain shadow on imposters is free.

### Files

```
shaders/conifer.glslh                              new   shared definition
shaders/stages/conifers.field.glslh                new   trunk SDF + forest disc
extension/src/terrain/builtin_stages.cpp           + ve::stage_conifers mirror
assets/pipelines/fjords.pipeline                   + stage stages/conifers.field.glslh
extension/src/conifers/conifer_settings.{h,cpp}    new   ConiferSettings + rows + clamp
extension/src/conifers/conifer_settings_store.h    new
extension/src/conifers/conifer_layout.{h,cpp}      new   camera-local lattice, budgets (pure)
extension/src/render/conifer_scatter_pass.{h,cpp}  new   cull + card scatter
extension/src/render/conifer_impostor_pass.{h,cpp} new
shaders/conifer_cull.comp.glsl                     new
shaders/conifer_scatter.comp.glsl                  new
shaders/conifer_impostor.vert.glsl                 new
shaders/conifer_impostor.frag.glsl                 new
extension/src/render/leaf_raster_pass.{h,cpp}      draw() takes buffers, not LeafScatterPass&
shaders/leaf.glslh, leaf.frag.glsl                 palette moves into LEAF_PARAMS_FIELDS
extension/src/render/leaf_scatter_pass.cpp         writes today's palette constants
extension/src/gpu_layout/blocks.h                  leaf params block gains the palette
extension/src/render/orchestrator.cpp              create / teardown when a `conifers` stage exists
extension/src/render/frame.cpp (compositor site)   cull, scatter, cards, imposters after leaves
extension/src/world/material_table.h               forest row
shaders/material_table.glslh                       regenerated
assets/materials/10_*.png                          new
tools/convert_materials.sh                         MATERIALS gains forest; recolour step
extension/src/voxel_world.{h,cpp}                  settings store "conifers"
tools/fjord_capture.gd                             conifer stats in its printout
```

`extension/src/conifers/` is godot-cpp-free and joins `pure_sources`, as `leaves/` does. The
compositor's exact file is found in the plan; it is wherever the leaf block lives today.

## 4. Placement and shape

### 4.1 Placement

- An 8 m lattice (`cell`), foot jittered within ±0.35 cell of the centre (the same bound
  `TREE_JITTER` uses), so a tree never leaves its cell.
- **Gate**, cheapest first: the cell hash against `density` (0.85) modulated by grove value
  noise on a 6-cell lattice (~50 m clearings); then the ground tests.
- **Ground tests**, all at the foot's XZ, with `e = SURFACE_Y + h − water_y`:
  - `e > shore + 1` — above `fjord_bands`' shore band, so no trunk stands in the future lake.
  - `e < snow_line − snow_jitter − snow_ridge_drop − treeline_margin` — below the lowest the
    snow line can waver or drop to on a ridge, so no tree stands on snow.
  - `|∇h| < max_slope` (1.7).
- All hashing is integer-only, as in `tree.glslh`: `test_field_diff.gd` requires the CPU mirror
  and GPU stage to agree bit for bit.

### 4.2 Shape

Per tree, from the hash:

- Height `mix(height_min, height_max)` = 18–28 m; trunk radius `trunk_radius` (0.45 m) ±20%.
- Crown from 15–30% of the height to the top; base radius `R = crown_radius` (3.0 m) ±25%.
- Envelope `R · (1 − s)` over crown height `s ∈ [0, 1]`.
- **6–9 tiers.** Within each tier the radius grows from its top to a flared, drooping lower edge,
  so the silhouette is a sawtooth. `conifer_profile` returns the envelope with tiers applied;
  it never exceeds `R · (1 − s) · 1.15`, which bounds every consumer.
- **Slope response.** Lean toward downhill is capped and falls to zero as slope approaches
  `max_slope`; the foot sits at the lowest ground under the trunk's footprint
  (`ground − slope · trunk_radius`), so no foot floats on the downhill side of a 60° wall.

### 4.3 The trunk stage

`shaders/stages/conifers.field.glslh`, after `fjord_bands`:

```
//!stage     conifers
//!kind      field
//!in        sdf : float
//!in        height : float
//!in        slope : float
//!in        material : uint
//!out       sdf : float
//!out       material : uint
//!param     cell : float = 8.0
//!param     density : float = 0.85
//!param     height_min : float = 18.0
//!param     height_max : float = 28.0
//!param     trunk_radius : float = 0.45
//!param     crown_radius : float = 3.0
//!param     max_slope : float = 1.7
//!param     treeline_margin : float = 40.0
//!use       fjord_height.water_y
//!use       fjord_bands.shore
//!use       fjord_bands.snow_line
//!use       fjord_bands.snow_jitter
//!use       fjord_bands.snow_ridge_drop
//!lipschitz mul 1.0
//!cpu       ve::stage_conifers
```

- **Trunk:** one round cone from the foot to the top, through the crown. `min` into `sdf`;
  `MAT_BARK` where it wins at `d ≤ 0`.
- **Bound: `mul 1.0`.** A union of exact 1-Lipschitz round cones; the running bound is 8.54 ≥ 1,
  so the computed bound is unchanged and `lipschitz 8.6` stays. The 3×3 neighbourhood is clamped
  to a `D_safe` derived from `cell`, the jitter and the reach, as `tree_d_safe()` does.
- **Forest disc:** where the final `sdf ≤ 0`, the material is `grass_01` or `breakstone`, and
  the point lies within `conifer_profile(c, 0)` of a present tree's foot in XZ, the material
  becomes `MAT_FOREST`. Same cell walk as the trunk; no second sweep.
- **Early-outs**, after `trees.field.glslh`: `g_field_skip_detail`; the vertical band; the
  hash-only horizontal reach and `sweep` neighbour pruning; the gate; the tight bound. The band
  is `[ground − 2, ground + height_max + max_slope · reach + margin]`; the derivation is written
  in the stage comment and kept identical in the CPU mirror.
- **Not-resident sectors.** `fjord_ground` returns the fallback `W − 64`; the shore test rejects
  it; no tree. Nothing false is solid.

## 5. Cards (0 – `card_reach`)

### 5.1 Cull — `conifer_cull.comp.glsl`

One thread per cell of the camera-local lattice out to `impostor_reach`: ~310 × 310 cells at
2500 m, below grass's ~250k threads. Per thread:

1. Gate and ground tests from `conifer.glslh`, ground from `fjord_ground` (it includes the
   generated `field.glslh`, binding set 1 like every field consumer — the leaf cull's idiom).
2. Distance and frustum cull of the crown's bounding cylinder.
3. **Chop check**, only where the atlas holds the region: one read at a third of trunk height;
   air or not bark means the tree is skipped. This is `leaf_trees.comp.glsl`'s rule verbatim.
4. Append to the **card list** if `dist < card_reach + fade`, and to the **imposter list** if
   `dist > card_reach − fade`. In the crossfade band a tree is in both.
5. Bump the card scatter's indirect dispatch args and the imposter draw args.

### 5.2 Card scatter — `conifer_scatter.comp.glsl`

One workgroup per card tree, `local_size_x = 128`, one thread per candidate clump. Budget
128 per tree at the nearest distance, thinned linearly to 1/8 at `card_reach`, with clump
radius growing so silhouette coverage holds — the leaf module's density LOD, pinned by a CPU
twin in `conifer_layout`.

- A clump picks a tier by hash, weighted by tier area, and sits on that tier's outer shell,
  top-biased within the tier; a minority are flung slightly past it to fray the silhouette.
- **Normal:** `mix(cone normal, tier normal, tier_roundness)`, the analogue of
  `canopy_roundness`. 0 is one smooth cone; 1 shades every tier as its own skirt.
- **Crown depth `t`:** height fraction in the crown, for the base darkening.
- **Sun:** one `terrain_sun_visibility` march per clump, times the `crown_shade` interior factor,
  as `leaf_scatter.comp.glsl` does.
- Writes the existing 32-byte `LeafClump` record, so `leaf.vert` / `leaf.frag` draw it unchanged:
  leaf grain, wind, sun, the dither fade.
- Clumps inside the trunk are pushed out, as the leaf scatter does with `tree_skeleton_sdf`.

### 5.3 Raster

`LeafRasterPass` draws the conifer cards with its existing shaders. Its `draw()` stops taking a
`LeafScatterPass&` and takes the four things it reads:

```cpp
struct LeafRasterInputs { RID instances; RID params; RID draw_args; int clump_count; };
```

`LeafScatterPass` and `ConiferScatterPass` each provide one. The raster's uniform set cache keys
on the instance and params RIDs, so the two draws per frame each get their own set. Two
`LeafRasterPass` instances are not needed.

### 5.4 Palette

`leaf.frag.glsl:100–101` hard-codes `kTop` / `kUnder`. They move into `LEAF_PARAMS_FIELDS` as
`palette_top` / `palette_under`. `LeafScatterPass` writes today's constants, so Default's canopy
is the same at every pixel. `ConiferScatterPass` writes the conifer pair, initially
`(0.20, 0.36, 0.22)` and `(0.05, 0.14, 0.13)`, tuned in the capture. This is the only change to
the leaf module's shaders.

## 6. Imposters (`card_reach` – `impostor_reach`)

`ConiferImpostorPass` draws one non-indexed indirect triangle list: six vertices per tree,
pulled by `gl_VertexIndex / 6` from the imposter list, as the card raster pulls clumps.

- **Quad:** camera-facing, sized to the crown's bounding cylinder from the current view, so the
  tree fits from any elevation.
- **Fragment:** sphere-traces `conifer_crown_sdf` (a 2D distance of revolution of
  `conifer_profile`) in the tree's local frame, at most 16 steps; `discard` on a miss. The trunk
  below the crown is the trunk stage's job — it is in the LoD mesh.
- **Writes**, into the same G-buffer channels the cards write:
  - real depth via `gl_FragDepth`, so imposters sort against terrain and each other;
  - the profile normal blended toward the tier normal by `tier_roundness`, as the cards;
  - albedo from the conifer palette, with the same `t` darkening and per-tree hue jitter;
  - `albedo.a` = the `crown_shade` self-shade only — the sun map supplies terrain shadow (§3);
  - material id `leaf_clump`, so the deferred pass shades imposters as it shades cards.
- **Opaque, alpha-tested.** `gl_FragDepth` costs early-Z on imposter pixels; a far tree covers
  a few pixels, and at the 300 m hand-off a 25 m tree is ~140 px tall, which is why the tiers
  are traced rather than drawn as a flat triangle.
- **No chop check:** the atlas holds regions to ~60 m, inside the card band.

## 7. Seams

- Cards → imposters: `bayer4` crossfade over `crossfade_m` (30 m) centred on `card_reach`; both
  draw in the band.
- Imposters → forest material: imposters dither out over the last 20% of `impostor_reach`. The
  `forest` material has been underneath the whole time, so nothing pops in or out.
- Forest material near → far: no seam; it is one texture whose top mip is the canopy colour.

## 8. The forest material

- Row appended after `snow`:
  `{"forest", "10", 1.2f, 0.0f, {0,0,0}, {<mean of 10_basecolor>}}`. Forest takes id 11; nothing
  renumbers.
- Texture: `terrain_textures_vol2/ground_foliage_01`, recoloured toward dark needle green and
  converted to 512² PNGs with the same command `convert_materials.sh` uses. The recolour is one
  `magick` step recorded in the script beside its `MATERIALS` entry, so it is re-tunable against
  the capture. `test_material_table.cpp` asserts `MATERIALS` equals the table.
- `shaders/material_table.glslh` and its byte-exact golden regenerate, as they did for snow.
- Up close it reads as needle litter and shade under each crown, between the cards; far away its
  top mip is canopy green with the clearings showing through.

## 9. Settings

`ConiferSettings`, its own store, registered as `"conifers"` beside `"leaves"`:

| Knob | Default | |
|---|---|---|
| `enabled` | true | |
| `card_reach_m` | 300 | |
| `impostor_reach_m` | 2500 | |
| `crossfade_m` | 30 | |
| `clumps_per_tree` | 128 | capped at the workgroup width |
| `max_card_trees` | 4096 | buffers clamp, never overflow |
| `max_clumps` | 300000 | |
| `max_impostors` | 120000 | |
| `clump_radius_m` | 1.0 | |
| `tier_roundness` | 0.4 | |
| `crown_shade` | 0.5 | |
| `wind_strength` | 0.12 | conifers are stiff; same gust field as grass and leaves |
| `palette_top`, `palette_under` | §5.4 | |
| `hue_jitter`, `leaf_grain`, `gloss` | as `LeafSettings` | |

Placement and shape are stage params in `fjords.pipeline`, because the field and the module must
agree on them.

## 10. Failure handling

| Failure | Behaviour |
|---|---|
| A conifer shader fails to compile | Fail-soft as the leaf passes: logged once; trunks and forest material still render |
| Pipeline has no `conifers` stage | No conifer passes are created (the orchestrator's `has_trees` idiom) |
| A list or instance buffer fills | Clamp, as the leaf passes do. Which trees drop is arbitrary (append order), so the caps are sized never to bind at default settings, and the high-water mark is reported in the stats |
| A sector not resident | `fjord_ground` returns `W − 64`; the shore gate rejects; no tree, no card, no imposter |
| Settings out of range | `clamp_conifer_settings`, NaN included, idempotent |

**Hooks rule.** gdUnit probes read the lists, counters and G-buffer the shipping passes wrote
during real frames. Nothing is re-scattered inside `hooks_render.cpp`.

## 11. Testing, commit order and measurement

**Baseline first.** Record the gdUnit failure set and run the native suite on clean `55bb9ee`.

1. **`conifer.glslh`, native**, executed as `test_tree_shader.cpp` executes `tree.glslh`:
   gates, jitter bound, `conifer_profile ≤ 1.15 · R(1 − s)`, card shells inside the bound, the
   trunk cone 1-Lipschitz (secants, not gradient norms), the band derivation and `D_safe`.
2. **Forest material.** PNGs, row, `MATERIALS`, regenerated `material_table.glslh` and golden.
3. **The `conifers` stage and mirror**, appended to `fjords.pipeline`.
   - Default, Mesas, Flat and Golden generate byte-identical `field.glslh`.
   - `test_field_diff.gd` diffs Fjords with conifers at seed 0 and a far seed, after sector
     readiness.
   - No trunk below the shore band or above the tree line, sampled over a ±1 km area.
4. **Leaf raster refactor and palette.** Default's frame goldens must not move.
5. **Conifer settings, cull and card scatter.** gdUnit: chopping a near trunk removes its cards
   on the next frame; the cull splits trees into card and imposter lists by distance with the
   crossfade overlap; budgets match `conifer_layout`.
6. **Imposters.** gdUnit: imposters draw at an aerial pose; imposter depth agrees with the trunk
   below it within a tolerance.
7. **Capture and look tuning.** `tools/fjord_capture.gd`'s three poses, frames to the user beside
   the references. Tuning goes in `fjords.pipeline` params and `ConiferSettings`, recorded in §13.

**Budgets**, measured with interleaved A/B/A runs, wall percentiles only (GPU timings are invalid
on this machine), Fjords with conifers against Fjords without:

- Frame p50 ≤ +2.5 ms at 2560×1440, `--disable-vsync`, at the valley pose.
- Cold fill to streaming-quiet ≤ +30%.
- Visible imposters ≤ ~80k at the aerial pose.

Over budget, the dials are `card_reach_m`, `impostor_reach_m`, `density` and `cell`, in that
order.

**Golden policy.** No frame golden should move. `material_table.glslh`'s golden regenerates by
design. If any other golden moves, stop and report rather than re-record.

## 12. Risks

| Risk | Handling |
|---|---|
| The trunk stage's cost: 3×3 B-spline ground reads per in-band sample, ~7× denser trees than Default | Hash-only reach pruning before any ground read; measured against the cold-fill budget; `cell` and `density` are the dials |
| Imposter overdraw and `gl_FragDepth` cost near the hand-off | 16-step cap; `card_reach_m` moves the hand-off; measured |
| Cards and imposters disagree visibly in the crossfade | Both read `conifer_profile` and the same palette; the capture includes the band |
| Forest texture's top mip reads too bright or too olive | The recolour step is re-tunable without code |
| The palette move shifts Default's canopy by rounding | Constants pass through a UBO at full float precision; the frame goldens decide |
| Trees on 60° walls look pasted on | The slope response in §4.2; `max_slope` is a param |

## 13. Deviations

### 13.1 Decided while planning

From `docs/superpowers/plans/2026-10-08-fjords-conifers.md`; 2, 6 and 13.2's performance work
changed below.

1. **The CPU mirror executes `conifer.glslh` itself.** The header is included inside a C++ struct whose member `conifer_ground()` reads the host sector cache, through `extension/src/terrain/glsl_shim.h`. There is no hand-written `conifers_mirror`. As a result, `conifer.glslh` is not terrain-free: its includer defines `vec4 conifer_ground(vec2 xz)` first, and shaders that never walk define a one-line stub.

2. **The trunk stage's vertical band is replaced by a bound that is safe at any slope.** The trees stage's slab argument assumes gentle hills. On a fjord wall, a trunk on a ledge above a point can be closer than `f / L`. `conifer_far_bound` is a ground-free lower bound on the distance to *any* trunk, derived from the field's slope bound of 8.6. Inside the walk, a per-cell vertical bound skips ground reads. Above about 43 m the stage reads no ground and returns `min(f, 8.6 · far_bound)`.

3. **Trees are vertical: no lean.** The spec's slope-dependent lean is dropped. A vertical trunk makes horizontal distance an exact trunk lower bound, which deviation 2 depends on.

4. **The record carries the whole shape** (two `vec4`: foot, hash, height, crown base, `R`, tiers + droop). The card scatter and the imposter therefore need neither stage params nor set 1; only the cull binds set 1.

5. **The crossfade is the leaf raster's existing fade**: cards dither out over the last 20% of `card_reach_m` (240–300 m), and the imposter keeps exactly the complementary pixels. The spec's 30 m centred band and `crossfade_m` are dropped.

6. **The imposter fragment does a 24-step inside test between the entry and exit of the bounding cylinder, then 4 bisection steps.** It does not sphere-trace a `conifer_crown_sdf`: the sawtooth tier profile has a jump at every tier and no useful Lipschitz bound.

7. **Clumps are not pushed out of the trunk.** The trunk is 0.07–0.5 m thick inside a 3 m crown, and the shell sits at 0.8–1.0 of the profile.

8. **Two `LeafRasterPass` instances**: one for leaves, one for conifer cards. Each caches its own uniform set. `draw()` takes a `LeafRasterInputs` instead of a `LeafScatterPass&`.

9. **`LeafParams` grows from 256 to 272 bytes**: `spare` becomes `palette_top`, and `palette_under` is appended.

10. **The forest texture comes from `tools/convert_forest.sh`**, a sibling of `convert_bark.sh` that does the recolour. `MATERIALS` in `convert_materials.sh` still lists `forest` for the table test, and that script skips it in its loop.

11. **`conifer.glslh` reuses `tree.glslh`'s hashes and round cone.**

12. **The cull's body is compiled only where the pipeline has the stage** (`#ifdef CONIFERS_STAGE`, defined by `conifers.field.glslh`). The shader-reload preflight compiles every `*.glsl` against the current world's generated field source, so an unguarded cull would fail reload on a Default world.

13. **The conifer pass always re-scatters.** It has no scatter-reuse epoch.

14. **`debug_conifer_stats` calls the same `VoxelFrame::draw_conifers` the compositor calls.** It has no parallel drive.

### 13.2 Recorded while implementing

**Look tuning: none.** The three capture poses (`tools/fjord_capture.gd -- --at=2000,-1600`)
were taken before and after the performance work below and differ only by per-pixel speckle
(card wind, dither, imposter step count). Nothing in `fjords.pipeline`, `ConiferSettings` or
`convert_forest.sh` changed. The user has not yet given a verdict on the capture, so no look
acceptance is implied.

**First measurement: over budget.** Task 8's A/B (below) on the Task 7 tree: frame p50 16.67 →
26.5 ms (+9.9 ms; budget +2.5) and settle capped at 1500 frames (budget +30%). A split with a
new `--conifer-value=name=value` benchmark flag priced the parts: stage alone (`enabled=0`)
+1.7 ms, cards ≈ 0, imposters ≈ +8 ms. The dial order was not used; four fixes were:

1. **The trunk term is in field units past a 1 m knee** (`conifer_trunk_field`). The walk's trunk
   distance is a true distance, but every other term is ~L × a distance. Fed in raw, `d_safe`
   (8.66) told every point up to ~45 m above forested ground it was within a metre of a
   surface. The collision probe (`chunk_has_surface`, pad 11.9 at L 8.54) then planned 668
   chunks instead of 223 and meshed them through the CPU mirror for the whole run: that was
   the stage's 1.7 ms and the capped settle. Now g(d) = d below 1 m and 1 + 8.5 (d − 1) above.
   g is 8.5-Lipschitz (under the computed 8.54) and g(d) ≤ 8.5 d, so §4.3's bound argument holds;
   1 m is past the atlas's ±0.64 m range, so baked trunk surfaces are unchanged. The early-out
   also returns the cap without a walk whenever the far bound already exceeds a non-negative f.
   This replaces deviation 2's "above about 43 m" threshold. `test_conifer_shader.cpp` now also
   checks the answer is no looser than g(min(truth, d_safe)) (it fails with the raw term).
2. **Imposters keep early depth.** The fragment wrote `gl_FragDepth`, so every quad behind a
   valley wall ran the full ray cast. The quad now stands in front of the crown's bounding
   cylinder and the fragment declares `depth_less` (reverse-Z). ~8 ms → ~2.3 ms.
3. **The imposter ray is clipped to the crown's bounding cone, not its cylinder** (deviation 6
   amended), and takes one step per pixel of footprint (`fwidth`), 4 to 24. The cone is solved
   from the ray's closest approach to the crown, because from 2.5 km the float quadratic cut off
   metres of crown. A new native case marches 400 random rays finely and requires every inside
   sample to fall in the span.
4. **The cull drops occluded imposters against this frame's Hi-Z pyramid** (lod_cull's test, on
   the tree's box; only when `hiz_built` this frame; cards are never occlusion-culled). Valley
   imposters 39 870 → 3 990, aerial 37 599 → 12 928. The cull also tests distance and frustum
   on the jittered cell box before any hash.

**Measurement**, Apple Silicon Mac (macOS, Metal), `--resolution 2560x1440 --disable-vsync`,
internal 1592×857 (render scale 0.65), `--pose=2000,54.2,-1600,200,4`, three interleaved
cycles run sequentially:

```bash
for i in 1 2 3; do
	for p in tests/fixtures/fjords_no_conifers.pipeline assets/pipelines/fjords.pipeline; do
		godot --path . --resolution 2560x1440 --disable-vsync demo/scenes/main.tscn -- --benchmark \
			--pipeline=res://$p --pose=2000,54.2,-1600,200,4 2>&1 | grep -E "p50|p99|settle"
	done
done
```

| | p50 ms (runs 1/2/3) | p99 ms (runs 1/2/3) | settle frames (runs 1/2/3) |
|---|---|---|---|
| Fjords without conifers | 16.67 / 16.67 / 16.67 | 21.38 / 19.02 / 20.42 | 1098 / 1131 / 1124 |
| Fjords with conifers, before | 26.39 / 26.67 / 26.39 | 30.91 / 31.60 / 33.33 | 1500 capped ×3 |
| Fjords with conifers, after | 18.75 / 18.75 / 18.75 | 19.46 / 19.81 / 19.91 | 1207 / 1213 / 1215 |

Median deltas after: **frame p50 +2.08 ms** (budget +2.5) and **cold fill +7.9 %** (budget
+30 %). Visible imposters at the aerial pose: 12 928 (budget ~80k). The baseline p50 sits at
exactly 16.67 ms in every run, which looks like display pacing on macOS, so +2.08 is a lower
bound on the true cost.
