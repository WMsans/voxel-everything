# Voxel Everything — Trees

**Date:** 2026-09-18
**Status:** approved design, ready for an implementation plan
**Prior specs:** `docs/superpowers/specs/2026-09-11-stylized-grass-design.md` (the foliage
module pattern this follows and deliberately diverges from);
`docs/superpowers/specs/2026-09-17-stage-authoring-design.md` (the 3-file stage contract,
`//!use`, and the computed Lipschitz bound this stage must satisfy).
**Start commit:** `9209d82`.

Fluffy, painterly broadleaf trees: a hash-placed trunk-and-branch skeleton carved into the
voxel field as real destructible geometry, with canopies drawn as scattered leaf-clump
cards shaded from a transferred sphere normal.

Reference image: soft overlapping crown lobes, scalloped clusters of small round leaves
with sky punching through at the edges, dark bare branch spars visibly crossing the
foliage, bright warm yellow-green on each lobe's top falling to deep cool blue-green
underneath, and a crown-wide darkening toward the base.

---

## 1. Goals and non-goals

Goals:

- Trees as a **terrain generation stage**: trunk and primary branches are SDF, so they are
  voxelised at 5 cm, meshed by LoD, collidable, and destructible with the existing edit tools.
- Canopies as a self-contained foliage module, sibling to grass, using a **different**
  technique from it on every axis that matters.
- Chopping a trunk removes its canopy on the next frame, with no invalidation code.
- No edits to the deferred / beauty stack.

Non-goals, decided explicitly:

- **Leaf LoD and far-field coverage.** Canopies reach ~250 m. Trunks, being SDF, are drawn
  everywhere the terrain is. Extending foliage into the meshed and traced far fields is its
  own sub-project, exactly as the horizon meadow problem is for grass.
- **Canopies as islands.** A severed trunk already becomes a Jolt body through the
  connectivity system. Its canopy disappears rather than toppling with it.
- **Leaves in the sun cascades**, leaves shadowing each other, or leaves shadowing the
  ground. Same non-goal grass carries, for the same reason.
- **Multiple species.** One species; the per-tree hash supplies the variety.
- **Fruit and blossom.**
- **A frame budget.** Looks first, tune later — but see §8, because unlike grass this
  feature lands on the streaming path and the risk is not decorative.

## 2. Prior art

| Source | Technique | Taken / rejected |
|---|---|---|
| [Smooth foliage like Breath of the Wild / Europa](https://polycount.com/discussion/209623/smooth-foliage-like-in-breath-of-the-wild-europa-by-helder-pinto-mini-tutorial) | Transfer vertex normals from a sphere placed over the foliage clump | **The** technique. Becomes `n = normalize(p − crown_centre)` at scatter time. It is what makes a canopy read as one soft volume instead of a pile of cards. |
| [Building a Fancy Stylized Tree Shader (Blender)](https://blenderartists.org/t/building-a-fancy-stylized-tree-shader/1395761), [aVersion of Reality writeup](http://www.aversionofreality.com/blog/2022/8/7/stylized-tree-shader) | Spherical crown with custom radial normals, mixed colours | Confirms the radial normal and the top-bright / underside-dark colour ramp driven by that normal rather than by real lighting. |
| [Anime foliage pipeline](https://trungduyng.substack.com/p/tutorial-blender-anime-foliage-pipeline) | Camera-facing leaf planes, normals transferred from a sphere to the bush | Camera-facing cards taken, with a per-clump roll and Y-locked tilt; full spherical billboarding rejected because a canopy swims when the camera strafes. |
| [BotW palm tree deep dive](https://www.artstation.com/blogs/alexanderzawadski/44EaO/breath-of-the-wild-palm-tree-deep-dive), [BotW technical analysis](https://www.resetera.com/threads/zelda-breath-of-the-wild-the-technical-analysis.8197/) | Simplified branch geometry, billboarded leaf clusters, thousands of trees at near-zero cost | Leaf **clusters**, never individual leaves. Branch geometry kept deliberately coarse. |
| [Stylized fluffy trees](https://www.artstation.com/artwork/lR4JJO), [Lush Tree](https://gamesartist.co.uk/lush-tree/) | Crown built from several overlapping rounded lobes | Crowns are multi-lobed, not one sphere. Lobes come from branch tips, so foliage sits where branches end. |
| [Modeling Trees with a Space Colonization Algorithm](https://www.researchgate.net/publication/221314843_Modeling_Trees_with_a_Space_Colonization_Algorithm), [Rule-based procedural tree modeling](https://arxiv.org/pdf/2204.03237) | Space colonization / L-systems for branch skeletons | **Rejected**, and the reason is architectural rather than aesthetic — see below. |
| [iq, raymarching distance fields](https://iquilezles.org/articles/raymarchingdf/) | Exact primitive SDFs; the tapered capsule (round cone) | The branch primitive. Exactly 1-Lipschitz, which a smooth-min blend is not. |
| [Procedural Grass in Ghost of Tsushima (GDC)](https://gdcvault.com/play/1027033/Advanced-Graphics-Summit-Procedural-Grass) | Compute scatter, indirect draw, density thinning for LOD | The pass architecture, already in this engine for grass. Density thinning is the canopy LOD. |

**Why not space colonization or an L-system.** A terrain stage is a per-sample `sdf(p)`
function that must evaluate identically on CPU and GPU with no side resources
(`FieldResources` is empty, and CPU-side resource sampling is out of scope for the terrain
pipeline). Both algorithms are iterative builders producing a data structure; they need a
build step and a buffer to read. A fixed-topology recursion evaluated analytically from the
cell hash is the only family that fits the seam, and at the branch counts a 5 cm voxel grid
can actually resolve, the realism difference does not survive voxelisation.

## 3. Architecture

**One shared header is the whole idea.** `shaders/tree.glslh` defines where trees are and
what shape they have, as pure functions of a cell coordinate:

```
bool  tree_cell_has_tree(ivec2 cell)   // forest gate + slope + height band
Tree  tree_skeleton(ivec2 cell)        // trunk + branch capsules, AND branch-tip positions
```

The terrain stage consumes the **capsules**; the leaf scatter consumes the **branch tips**,
each of which is one canopy lobe centre. Same functions, same `P.tree_*` params from the
same UBO, so the trunk you chop and the canopy you see cannot disagree. This follows
`shaders/grass_blade.glslh` — a shared header executed natively by a doctest — applied to
the case that genuinely has two consumers.

Params reach both sides through the existing seam: `field.glslh` establishes a **set 1**
that "every field-consuming pass binds unconditionally", params UBO at binding 0. The leaf
scatter binds set 1 like any other field consumer and reads the resolved pipeline's
`P.tree_*` directly. There is no second copy of the tree parameters anywhere.

Rejected alternatives:

- **A CPU-side tree list uploaded per frame.** Adds a fourth copy of the terrain definition,
  which is the exact hazard the frame-module spec's Appendix A exists to eliminate.
- **Brick-driven discovery, as grass does it.** The canopy volume is air. There are no bricks
  to iterate. Iterating bark-material bricks to find trunk tops reconstructs, badly, something
  the hash already knows exactly.

### Files

Terrain stage — 3 files, per the stage-authoring exit criterion:

```
shaders/stages/trees.field.glslh              new
extension/src/terrain/builtin_stages.cpp      + CPU mirror
assets/pipelines/trees.pipeline               new  (then default.pipeline, M3)
```

plus `shaders/tree.glslh`, which is shared infrastructure rather than stage authoring, and
one `MAT_BARK` row in `extension/src/world/material_table.h`.

Leaf module, mirroring `extension/src/grass/` and its two render passes:

```
extension/src/leaves/leaf_settings.{h,cpp}
extension/src/leaves/leaf_settings_store.h
extension/src/leaves/leaf_layout.{h,cpp}
extension/src/render/leaf_scatter_pass.{h,cpp}
extension/src/render/leaf_raster_pass.{h,cpp}
shaders/leaf_clump.glslh
shaders/leaf_scatter.comp.glsl
shaders/leaf.vert.glsl
shaders/leaf.frag.glsl
```

plus one `kFoliage` row, `{"leaf_clump", 0.0f, {0,0,0}}` — the table's own comment already
anticipates this: *"A new grass or leaf type is one row here plus its shader."*

`extension/src/leaves/` is CPU-side and godot-cpp-free so it builds into the native
`pure_sources` test target, exactly as `extension/src/grass/` does.

### Per-frame flow

Inserted after the grass raster and before SSGI:

```
ve::leaf_layout(settings, camera, view_proj)   CPU, a few dozen ops
  -> leaf_scatter.comp   stage 1: tree cull    -> compacted tree list + dispatch args
  -> leaf_scatter.comp   stage 2: clump scatter-> instance SSBO + draw args
  -> LeafRasterPass::draw                      -> GBuffer albedo / surface / depth
  -> existing SSGI / SSAO / deferred / outline shade it, unmodified
```

## 4. The trees stage

### Placement

A jittered grid over XZ, cell ≈ 14 m. Per cell the hash yields the jittered position, trunk
height and radius, branch angles, crown size and a rotation. A low-frequency forest noise
gates the cell, so the result is groves and clearings rather than an orchard.

**Base height.** The stage needs the terrain height at the *tree's* XZ, not at `ctx.p`'s, so
it recomputes the hills and relief terms there. This is exactly what `cave.field.glslh`
already does at `P.cave_cx, P.cave_cz`, and it is what `//!use` was built for:

```
//!use hills.amp_a
//!use hills.amp_b
//!use hills.amp_c
//!use relief.amp_a
//!use relief.freq_a
//!use relief.amp_b
//!use relief.freq_b
```

No literals. Overriding `hills.amp_a` in a pipeline file moves the trees with the ground, on
both CPU and GPU, which is precisely the divergence M4 of the stage-authoring work removed.

**Rejection.** A cell drops its tree when the analytic slope at the tree's XZ exceeds a
threshold (trees do not grow on cliffs) or when the terrain height there falls outside the
grass band. Both come from the closed-form derivative of the same hills and relief terms —
no marching, no sampling.

### Skeleton

Trunk plus 4–5 primary branches, each splitting once, as **tapered capsules** (iq's round
cone). Roughly 13–16 primitives. Branch tips fall out of the same recursion and become the
canopy lobe centres, which is why the reference's dark spars read as *holding* the foliage
rather than merely crossing it.

Plain `min` for the union, not a smooth minimum: a tapered capsule is exactly 1-Lipschitz
and `min` of 1-Lipschitz fields is 1-Lipschitz, while a polynomial smooth-min is not, and an
overstated field here tunnels the raymarcher.

**All hashing is integer-only** (`uint` mixing, no float trig inside the hash). `test_field_diff.gd`
requires the CPU mirror and the GPU stage to agree bit-for-bit, and float transcendentals do
not agree across the two. This is a constraint on the implementation, not a preference.

### Cost, and the three early-outs

The field is evaluated at every voxel of every brick, so a naive 3×3 cells × 16 capsules =
144 distance evaluations per sample is not shippable. In order:

1. **Vertical band.** `ctx.height` already carries the terrain height at `p`'s XZ. Outside
   `[ground − 2, ground + max_tree_height]` the stage returns immediately, having done one
   compare. This removes the large majority of samples in a 40 m-tall brick column.
2. **Forest gate** — one integer hash per cell.
3. **Bounding capsule** per surviving cell — one cheap test before touching the 16.

Typical in-band cost is therefore ~9 bounding tests; the full capsule set is paid only
adjacent to an actual trunk.

### Lipschitz: `mul 1.0`

`ctx.sdf = min(ctx.sdf, tree_d)` where `tree_d` is an exact 1-Lipschitz union. The running
bound at this point in the pipeline is 1.99 ≥ 1, so a union cannot raise it — the same
argument `cave.field.glslh` makes for its `max`.

**One correctness detail.** Evaluating only the 3×3 cell neighbourhood *overestimates* the
distance when the nearest tree is two cells out, and an overestimate is the direction that
tunnels. `tree_d` is therefore clamped to the provably safe

```
D_safe = 1.5 · cell_size − jitter_max − tree_bound_radius ≈ 12 m
```

Clamping down is conservative. Inside the vertical band the terrain distance is already
smaller than `D_safe`, so the clamp costs essentially nothing, and outside the band the stage
has already returned.

### Material

The stage runs **after** `height_bands` and writes `MAT_BARK` wherever the tree won the
`min`. Running it before would let height-banding paint trunks as grass or rock according to
the ground height at their base.

Manifest shape:

```
//!stage     trees
//!kind      field
//!in        sdf : float
//!in        height : float
//!in        material : uint
//!out       sdf : float
//!out       material : uint
//!param     cell : float = 14.0
//!param     density : float = 0.55
//!param     trunk_height : float = 9.0
//!param     trunk_radius : float = 0.30
//!param     branch_radius_min : float = 0.10
//!param     crown_radius : float = 4.0
//!param     max_slope : float = 0.6
//!lipschitz mul 1.0
//!cpu       ve::stage_trees
```

`material` is declared **in** as well as out: the stage preserves whatever `height_bands`
wrote wherever the tree did not win the `min`, so it is a read-modify, not a write.

`crown_radius` is also the `tree_bound_radius` that `D_safe` is derived from, so raising it
tightens the clamp rather than silently invalidating it; `tree.glslh` computes `D_safe` from
the params rather than from a constant.

## 5. The leaf module

### Stage 1 — tree cull

One thread per cell in a camera-local 2D grid: 250 m reach ÷ 14 m cell ≈ 36 × 36 ≈ 1300
threads, trivial beside grass's ~250k. Each thread:

1. Forest gate and rejection tests from `tree.glslh` — the same functions the stage used.
2. Reconstruct the skeleton; frustum- and distance-cull the crown's bounding sphere.
3. **Sample the live SDF at the trunk attachment point.** If the trunk is not there, the tree
   never enters the list.
4. Append to a compacted list and bump the stage-2 indirect dispatch args, following
   `dispatch_args.comp.glsl`.

Step 3 is the whole chopping answer: one atlas read per *tree*, rather than one per
instance as grass pays per blade, and it reuses the same live-atlas seam that makes grass
edit-aware. No invalidation code exists because none is needed.

### Stage 2 — clump scatter

One workgroup per surviving tree, `local_size_x = 128`, each thread one candidate clump. The
per-tree budget comes from `ve::leaf_layout` and falls with distance while clump radius
grows, holding silhouette coverage at roughly constant instance count — Ghost of Tsushima's
density LOD, which grass already uses as drop-3-of-4.

Clumps are placed **on the shell of each lobe**:

```
dir = hash_unit_sphere();  r = lobe_radius * mix(0.65, 1.0, hash);
```

not through its volume. The crown interior is never seen and filling it is pure overdraw.

### Instance record

32 bytes, two `vec4`, the same budget as `GrassBlade`:

- `a` — clump centre `xyz`, clump radius `w`
- `b.x` — 16-bit oct-packed sphere normal, sun-visibility byte in bits 16–23 (grass's exact
  packing)
- `b.y` — per-clump hash (colour jitter, wind phase, roll)
- `b.z` — crown-depth `t` and the curvature ratio `clump_radius / crown_radius`, 16 bits each
- `b.w` — roll and sway phase

The curvature ratio is packed rather than recomputed because `crown_radius` is a per-*tree*
value and the raster pass sees only clumps. It is the one scalar §6's fragment-side normal
perturbation needs, and packing it is what keeps the record at 32 bytes.

## 6. Card, shading, wind

### Silhouette

A plain camera-facing quad reads as a rectangle; the reference's edge leaves punch out as
individual round dots against the sky. The fragment builds the scallop procedurally — radial
falloff plus a value-noise term, thresholded to alpha — so the edge is lobed and a few holes
open through to sky. No texture, the same posture grass takes.

`discard` costs early-Z. If overdraw proves to be the binding constraint, the upgrade is a
fan of sub-quads that builds the scallop in geometry instead; that is a change inside
`leaf.vert.glsl` and `leaf.frag.glsl` with no effect on anything else.

Orientation is camera-facing with a fixed per-clump roll from the hash and Y-locked tilt.

### Normals — the technique

```glsl
n = normalize(p_world - crown_centre);
```

transferred from a sphere over the whole crown, not the card's real facing. This is the axis
on which leaves differ most from grass, which uses its own Bezier face normal.

A pure crown-centre normal loses the lobes — the crown shades as one smooth ball and the
overlapping rounded masses in the reference disappear. The scatter therefore blends toward
the clump's own lobe centre:

```glsl
n = normalize(mix(normalize(p - crown_centre), normalize(p - lobe_centre), canopy_roundness));
```

`canopy_roundness` is the settings knob named in §7 — 0 is one smooth ball, 1 is a cluster of
separately-shaded balls. Default 0.35, which keeps the crown reading as one volume while the
lobes stay legible. This mirrors grass's `blade_lighting`, which blends the blade's own
normal against the ground normal for the same reason.

The scatter packs the resulting normal per clump; the fragment perturbs it across the card by
`uv * (clump_radius / crown_radius)`, using the ratio packed in `b.z`. That keeps the record
at 32 bytes and is visually indistinguishable from a true per-fragment transfer at these card
sizes.

### Colour

Two gradients baked into albedo before cel shading sees it, matching the two gradients
visible in the reference:

- `n.y` — each lobe's top goes bright warm yellow-green, its underside deep cool blue-green.
- crown-depth `t` — the whole crown darkens toward its base. This is the ambient-occlusion
  read in the reference, and it is a per-clump constant, so it is free.

Plus per-clump hue and value jitter from the hash. Cel shading and the outline pass then
apply on top, unmodified. This is the same arrangement grass uses for its root-to-tip
gradient, and it keeps the module clear of the beauty stack.

### Sun

One `terrain_sun_visibility` march per clump from `shaders/sun_march.glslh`, verbatim as
grass does it, written to albedo alpha — because `deferred.comp.glsl` takes G-buffer albedo
alpha as sun visibility and only mins in the sun map where `far_field_owns(px)`. A clump
writing `1.0` would be unshadowed near the camera, which is exactly the bug the grass
lighting revision had to come back and fix.

Multiplied by a crown-interior factor from `t` and the shell parameter, so the inside of a
crown is darker than its lit face.

**Honest limitation.** That march sees terrain and trunks, not other leaves, since foliage is
not in the SDF. Canopies shade the ground beneath them only insofar as their trunks do.

### Wind

Whole-clump sway with amplitude `∝ t²`, so the crown base stays planted. Driven by the
**same** scrolling gust noise grass samples at world XZ, so a gust crosses the meadow and the
canopies together; the shared function moves to `shaders/wind.glslh` in the same commit that
first needs it in two places. Per-clump phase on top.

The trunk is voxels and cannot move. Sway amplitude is capped before the canopy visibly
detaches from it, and that cap is a settings knob rather than a constant.

## 7. Integration, settings, failure modes

**Integration.** In `raymarch_compositor.cpp`, after the grass raster block and before SSGI,
following the grass idiom exactly:

```cpp
if (LeafScatterPass *ls = world->leaf_scatter_pass()) {
    timings->begin(rd, "leaves");
    const ve::LeafLayout ll = ve::leaf_layout(world->leaf_settings(), cam.origin, view_proj);
    if (ls->run(rd, *atlas, ll) && leaf_raster->draw(rd, *ls, *gb, view_proj, cam_pos))
        timings->end(rd, "leaves");
    else
        timings->cancel("leaves");
}
```

Both passes are constructed in `RenderOrchestrator::ensure_gpu_graph()` and destroyed in
`teardown_render_passes()`, preserving the documented CPU-outlives-GPU ordering. Shader hot
reload needs no registration: `preflight_shaders()` enumerates `res://shaders/*.glsl`.

**Settings.** `VoxelWorld` gains one `LeafSettingsStore` member and two one-line delegations,
mirroring `GrassSettingsStore` without joining it. Demo knobs: on/off, reach, clumps per
tree, clump size, canopy roundness, wind strength, wind speed. Tree *shape* knobs are
pipeline `//!param`s, edited in the `.pipeline` file and applied on fresh world init.

A `--leaves=` benchmark flag ships from the start, so every existing leg keeps its control,
exactly as `--grass=` does.

**Failure modes**, all following the grass precedent:

- **Instance buffer overflow.** Fixed-size buffer sized from `ve::leaf_layout`'s capacity for
  the maximum reach; the scatter appends through an atomic counter and clamps, so an
  overflowing frame drops clumps rather than scribbling. A sticky high-water mark records it,
  logged once per pass lifetime rather than per frame.
- **Zero clumps.** Draw args carry zero vertices; the indirect draw is a no-op. No special case.
- **Pass failure.** `timings->cancel("leaves")` and skip. Foliage is decorative and must never
  take a frame down with it.
- **Counter initialisation.** Cleared explicitly every frame. Stated because a fresh RD buffer
  reads back as zero on this machine, so "the count came back zero" is always a real zero and
  never an uninitialised-memory story.

## 8. Sequencing

Ordered so golden churn never lands inside a feature commit.

| | |
|---|---|
| **M1** | `shaders/tree.glslh` + `extension/tests/test_tree_shader.cpp` executing it natively, as `test_grass_blade_shader.cpp` does for the blade maths. No pipeline change; nothing ships. |
| **M2** | `trees.field.glslh`, its CPU mirror, and **its own `assets/pipelines/trees.pipeline`** — 3 files, zero golden churn. `test_field_diff.gd` and `test_lipschitz_sampled.cpp` enumerate `assets/pipelines/*`, so both pick the stage up with no test edit. |
| **M3** | `MAT_BARK` row, and flip `default.pipeline`, in its own commit. Demo terrain changes here; `tests/golden/default_pipeline_field.txt` is re-recorded with the cause named in the message, and any moved raycast / mesh / collider number is attributed in the same commit — the golden policy from §9 of the stage-authoring spec. |
| **M4** | Leaf scatter and raster passes, `LeafSettings` and its store, demo knobs, the `--leaves=` flag. |
| **M5** | Shading and wind polish, golden capture, A/B/A measurement. |

A baseline gate precedes M1: re-record the gdUnit failure set at `9209d82` into
`docs/superpowers/plans/2026-09-18-trees-baseline.md`. That set drifts, so an older baseline
is stale by default and every "did I break this" question is unanswerable without it.

## 9. Testing

**Native, doctest, `pure_sources`:**

- `extension/tests/test_leaf_layout.cpp` — per-tree clump budget against distance, total
  capacity, the cell search box, settings clamping. No GPU. The density-versus-distance
  contract is pinned here, where it is cheap.
- `extension/tests/test_tree_shader.cpp` — `tree.glslh` executed directly: skeleton
  determinism for a given cell, every capsule inside the declared bounding sphere, branch tips
  inside the crown, and **the `D_safe` clamp never exceeding the true distance to a tree two
  cells away**. That last case is the one protecting the raymarcher.

**Free, from tests that already enumerate every pipeline:**

- `test_field_diff.gd` pins CPU ≡ GPU for the tree stage across all six op scenarios, the
  moment `trees.pipeline` exists. This is what catches a hash that diverges.
- `test_lipschitz_sampled.cpp` takes central differences over the CPU mirror and asserts
  `|grad| ≤` the reported bound. This is the instrument that catches the `D_safe` clamp being
  wrong, and it is the reason M2 lands before anything renders.

**GPU, gdUnit** — `tests/test_trees.gd` and `tests/test_leaves.gd`, every assertion reading
back buffers the real passes wrote:

- bark voxels exist at a known tree cell and none in a known clearing
- clumps exist over a tree cell, none over a clearing
- clump count falls monotonically as the camera retreats
- **painting the trunk to rock drops that tree's clumps to zero on the next frame** — paint,
  not dig, because digging a trunk exposes fresh ground and confounds removal with exposure.
  This is the correction the grass edit-contract test already had to make; the sharper
  instrument is adopted here from the start.
- the clump count clamps at capacity instead of overflowing
- every emitted clump lies within its tree's crown bounding sphere

**Golden capture** — one deterministic `--capture` frame into `tests/golden`, so wind and
colour changes have to be deliberate.

**The `hooks.cpp` rule.** Trees and leaves get exactly one hook, `debug_leaf_stats()`, and it
only reads back counters and instances the shipping scatter pass wrote during a real
compositor frame. That file re-assembles render inputs, so a green test there can be
exercising a path that does not ship. No probe that rebuilds a parallel scatter. A behaviour
that cannot be tested against shipping output does not get a test.

## 10. Risks

| Risk | Handling |
|---|---|
| **Field evaluation cost.** The stage runs at every voxel of every brick, so it lands on the *streaming* path, not only the frame. This is the risk that decides the feature. | The three early-outs of §4, of which the vertical band does nearly all the work. Measured by interleaved A/B/A wall-frame runs **and** stream throughput, because a streaming regression shows as hitching rather than as frame time. GPU timestamps read back invalid on this machine, so per-pass attribution is not available and will not be claimed. |
| **CPU/GPU hash divergence.** `test_field_diff.gd` demands bit-identical fields. | Integer-only hashing, no float trig inside the hash. A constraint on the implementation, checked by a test that already exists. |
| **Branches at 5 cm.** A 20 cm branch is four voxels; the surface-nets far field will thin and eventually drop them, and this engine already has hairline artifacts at LoD level boundaries. | Minimum branch radius is a param. Distant trees losing their spars is accepted, and named here so it is not later mistaken for a regression. |
| **Canopy overdraw**, strictly worse than grass: large alpha-discarded cards, layered, and the pathological case is a near tree filling the screen. | Shell-only placement is the structural mitigation. Clump budget, clump size and reach are the dials. The sub-quad fan is the documented upgrade if `discard` proves to be the cost. |
| **Trees over carved caves** can hang, since placement uses the analytic height while `cave` carves afterward. | Nearly unreachable in the shipped pipeline, which carves one hand-placed sphere. Known artifact, not designed around. If a later pipeline carves broadly, the tree stage gains a `//!use cave.*` rejection the same way it gets its base height. |
| **M3 moves demo terrain**, and with it any raycast, mesh and collider number that depends on the field. | Golden policy: attribute every moved number to this cause, re-record in the same commit, name the cause in the message. If a diff looks like a shipped bug rather than a moved baseline, stop and report. |

## 11. What the leaf module does *not* share with grass

Recorded because "similar but different" was the brief, and because a later reader will
reasonably ask why two foliage modules exist rather than one parameterised module.

| | Grass | Leaves |
|---|---|---|
| Discovery | resident bricks straddling the surface | tree cells from a hash grid |
| Scatter domain | a surface | a volume shell |
| Primitive | 27-vertex cubic Bezier blade | camera-facing scalloped card |
| Normal | the blade's own rounded face normal | transferred from the crown sphere |
| Colour drive | root-to-tip along the blade | lobe normal `n.y` plus crown depth |
| Edit awareness | per-blade atlas read | one atlas read per tree |
| LOD | drop 3 of 4 | fewer clumps, larger radius |

What they do share — and should share rather than duplicate — is the gust field, the
`sun_march.glslh` visibility march and its albedo-alpha packing, the bayer4 reach dither, the
indirect-draw and overflow-clamp bookkeeping, and the settings-store shape.

---

## 12. What shipped (2026-09-19)

The feature landed as designed across Tasks 0–15 of `docs/superpowers/plans/2026-09-18-trees.md`.
`docs/superpowers/plans/2026-09-18-trees-results.md` is the long form: both measured costs with
their methods and brackets, and the complete deviation record (ledger rulings R1–R13). This is
the summary so the spec and the code do not drift apart.

**Measured, Apple M1, 2560x1440, vsync genuinely disabled; GPU timestamps invalid, so wall
percentiles only and no per-pass attribution:**

- Leaf passes (frame path), interleaved A/B/A `--leaves=0|1`: **≤ +1.8 ms p50** on every
  leg (steady +0.40, move −0.09, ridge +0.11, edit +0.24, edit-bounded +1.76, island +0.90);
  p99 deltas −1.35…+1.88, all inside off-leg tail noise. Off brackets ≤ 1.03 ms p50.
- Trees stage (streaming path), cold-atlas `debug_init_atlas()` + pump-to-quiet medians of 3:
  golden 774 ms, default-minus-trees 856 ms, **default 4463 ms — the stage costs ≈ 3.6 s
  (≈ 5.2×) of cold time-to-quiet** at camera (20, 60, 30); reps within ~50 ms. It also keeps
  in-game streaming nonstop (§10's "hitching", and the reason the A/B/A absolute level is
  85.7 ms where the Task-0 baseline was 23.8 ms — the deltas above sit on that saturated
  base and isolate the leaf passes only, which is what the dial gates).

**Claims this doc got wrong, in §13-of-grass fashion — named plainly:**

- **§4 early-out 1** ("outside [ground − 2, ground + max_tree_height] the stage returns")
  was unsound at the band top: the returned terrain distance lets a sphere-trace step jump
  over a crown top just under the plane. Shipped band top:
  `ground_y + 2.05 * (trunk_height * 1.25 + crown_radius)` (the 1.99 slab factor rounded up),
  identical in the GPU stage and the CPU mirror.
- **§7 integration** names `raymarch_compositor.cpp`; the block lives in
  `VoxelFrame::render_pre_opaque` (`render/frame.cpp`) since the frame-module move — the
  grass raster line it says to follow there has been there since then.
- **§9's byte-identical capture golden** is unachievable through the shipping path on this
  machine: raster draw-order depth ties move ~0.06% of pixels run-to-run — proven non-leaf
  (249 px with grass, leaves and SSGI all off). Shipped instead: the tolerance-golden route —
  a fourth `grove` camera in `tests/test_frame_shipped_golden.gd` (TOL_TILE 0.004; worst
  measured per-tile margin 3.8e-4 in the Task-15 re-measurement, >10x inside it) plus
  `tests/golden/leaf.png` as the human reference; `tools/leaf_capture.gd`
  remains the deterministic-by-construction look tool.

**Plan-level corrections worth remembering:** bark layer 07 comes from the owner's
`bark_willow_1k` set via `tools/convert_bark.sh` — the vol2 pack the plan cited has no bark
folder, and the missing map took every GPU suite down before the fix (placeholder look,
reconvert trivially; `convert_materials.sh` still aborts at its bark entry on this machine).
`allow_gpu_only` needs a value (`atoi`); the `//!cpu` directive must arrive in the same
commit as its registration. The leaf compute shaders carry two forced extra set-0 bindings
each (`palette_buf`, `brick_flags` — GLSL analyses the whole TU), and a custom pipeline
without the trees stage fails the leaf compile and fail-softs the pass by design.

**Accepted, stated rather than hidden:** the seam probes' third ownership state for
`MAT_LEAF_CLUMP` pixels and the band bar at 6.25% vs the 7.3% stall reference (3 px teeth);
the combined-card containment envelope ≤1.75 (R10); `kLeafCellM`'s deliberate duplication
with a `ponytail:` upgrade path; the crown_radius ≥ branch_radius_min/0.38 authoring
coupling with **no** runtime clamp (pipeline-authored param, defaults satisfy, symptom loud);
and the funding-frontier headroom figures §4 of the results doc hands to the next feature.

**Final review wave (same day; ledger R13; results doc §3.5).** The whole-branch review
returned "with fixes"; three Important findings were deviation records this section lacked,
and each now says plainly what was true before:

- **§4's height-band rejection was specified, not implemented — and unrecorded** until the
  review. R13 probed first: over ±3000 m of the shipped default pipeline, 64,552 cells placed
  a tree and 63,681 of them (32,474 dirt + 31,207 rock — 98.65 %) stood **outside** the grass
  band; the spec's sentence was aspiration, not artifact. Fixed by implementing, not by
  recording: `tree_cell_present(cell, tp, h, slope)` in `shaders/tree.glslh` (shared by GPU
  stage and leaf scatter) rejects `h <= 1.0 || h > 4.0` right after the slope gate, and the
  CPU mirror in
  `builtin_stages.cpp` carries it identically. The band literals are a mirror of
  `stage_height_bands` (`height_bands.field.glslh`: rock above 4, grass above 1 — stage text,
  not pipeline params, so the copy cannot diverge from an author's edit); GPU≡CPU is pinned
  by `test_field_diff.gd`, re-verified to actually cross the band boundaries. The field
  goldens moved and were re-recorded in the causing commit — together with the two
  machine-checked numeric look-goldens (the frame suite's tile means, the SSAO horizon
  `lit_luma`; R12 keeps committed PNGs human-reference only, and `leaf.png` was not
  re-captured in this wave); the look changes (no treeline-
  top or valley-floor trees). Two consumer-characterization suites moved at the final gate,
  re-recorded with the cause named: `test_world_field_consumers`'s collider golden (one of
  its three resident chunks was a band-rejected tree's trunk) and `test_material_glow`'s
  paint sync (its quiet-window sync stopped landing paints once the band removed the tree
  work that kept the uploader busy; results doc §3.5 holds the full record). §2's streaming
  figures pre-date the band; the band rejects
  cells the old gate built full skeletons for, so it is not a cost regression.
- **§9's first GPU bullet — "bark voxels exist at a known tree cell and none in a known
  clearing" — shipped at this fix wave, never as `tests/test_trees.gd`.** The plan never
  scheduled it: that is a plan gap, not a design change, and §9 stays the authority the code
  now meets. It lives in `tests/test_leaves.gd`, where the streaming harness and the tree
  hook already are, and reads shipping output only (§9's hooks rule): the tree list from
  `debug_leaf_stats()` (now also reporting the dispatch grid, lattice pitch and reach the
  last real `LeafScatterPass::run()` uploaded) names the cell, and `debug_raymarch_gbuffer()`
  drives the real marcher to demand `MAT_BARK` (8) on the trunk axis; the clearing is the
  first lattice cell of that shipped grid near the view centre carrying no listing and no
  overhanging crown sphere, and its march must return a non-bark surface. Presence is read
  from the pass's output, never re-derived from the hash in GDScript.
- **§5's scatter shape values are duplicated pipeline defaults, not live UBO reads.**
  `leaf_layout.cpp` carried a comment claiming the scatter "reads the LIVE values from the
  pipeline's set-1 UBO"; it does not and cannot from there — the literals it packs *are*
  what `leaf_trees.comp.glsl` sees. The comment now says so and names
  `assets/pipelines/trees.pipeline` as the source of truth, and a native pin in
  `test_leaf_layout.cpp` asserts every tree-shape literal against the shipped pipeline
  resolved through the engine's own loader (exact float equality): edit a tree param there
  without moving `leaf_layout.cpp` and a test fails, instead of canopies silently floating
  off their new trunks.

The wave also closed two §3.4 parked minors (hole-probe exemption ordering + per-class teeth;
leaf settings per-row clamp sweep + non-vacuous idempotence) — see results doc §3.5.
