# Forest caching: compute per voxel, not per pixel per frame

## Intent

Walking through the Fjords conifer forest runs at 31 fps on the M1. The target is **60 fps
average while walking there**, at shipped settings (2560x1440, render scale 0.5, near-field
scale 0.40, quality tier 3).

The cause is that every near-field pixel redoes, every frame, work whose answer does not
change between frames: the shading normal re-runs the whole terrain pipeline (7 evaluations
per hit pixel), and the sun-shadow ray re-traces the atlas. The terrain is static between
edits. The fix is the caching approach SDF engines like Dreams and Claybook take: evaluate
once per voxel when a brick is generated or edited, then let rendering read the cache.

The last commit (`1b18a2b perf: tree normals`) does part of this with special cases: bark
takes the R8 lattice normal, and `g_field_skip_detail` drops the tree stages from the
gradient taps. Both are stopgaps. The normal still costs 8.6 ms, and the bark normal speckles.

## Measurements (2026-10-18, M1, fjords-forest leg, one process per variant)

Baseline bracketed 32.40 / 32.25 ms. Reports are in `reports/cache-baseline/` and
`reports/cache-sweep/`; the variant harness was a scratch SceneTree script that overrides
`raymarch.comp.glsl` before `add_child` and lets `demo/scripts/benchmark.gd` time it.

| Variant | avg ms | Saves |
|---|---|---|
| Today | 32.3 | n/a |
| `terrain_source_normal` returns the R8 fallback | 27.1 | 5.2 |
| `terrain_source_normal` returns a constant (free normal, upper bound) | 23.7 | 8.6 |
| `--effects-off=raymarched_sun_shadow` | 28.9 | 3.4 |
| `--effects-off=raymarch` (raster near field) | 18.6 | 13.7 |
| `--quality=0` | 24.5 | 7.8 |
| `--no-physics` | 32.7 | ~0 (GPU-bound) |

Not yet priced: conifer cards and impostors, grass, leaves. `--effects-off=conifers,grass,leaves`
is silently ignored: those are not render or beauty setting names
(`RenderOrchestrator::set_effect_enabled` fails soft). An earlier note put cards and
impostors at about 7 ms.

Other legs at the baseline (avg ms): steady 13.03, ridge 18.90, move 14.09, edit 29.67,
edit-bounded 16.32, island 17.38.

## Goals

1. A new benchmark leg, **fjords-forest-walk**, is the finish line: its average frame time is
   at or under 16.7 ms.
2. Shading normals for terrain come from a per-voxel cache written when bricks are generated,
   not from evaluating the pipeline per pixel.
3. Sun visibility for near-field terrain pixels comes from a per-voxel cache, refreshed as the
   sun moves, not from a full shadow ray per pixel per frame.
4. If 2 and 3 leave a gap, close it with conifer card / impostor reach and density.
5. If that still leaves a gap, speed up the primary march itself with no loss of image quality.
   This is in scope (user, 2026-10-18), even though it is not caching.
6. Each step reports before/after numbers on the walk leg and on the standing fjords-forest
   leg, plus same-pose captures, and names the image-quality cost it took.

## Non-goals

- Other world types and the far field (LoD mesh). They only matter as legs that must not
  regress.
- Reusing last frame's pixels (temporal reprojection). Rejected: walking keeps uncovering
  ground behind trunks, and the user called it a technical black hole.
- Shorter or half-resolution sun rays. Rejected: capping the near sun march at 30 m was
  already measured to save nothing.

## Anti-goals (a failure even if it hits 60)

- **Any other leg regresses meaningfully.** Steady, ridge, move, edit, edit-bounded and island
  may each move by at most about 1 ms or 5% of their average, whichever is larger.
- **A win that only shows while the camera is still.** The walk leg decides; the standing leg
  is only a check.
- **A cache that goes stale after an edit.** An edited brick must shade with its new normals
  and shadows the frame its regenerated SDF lands. Showing the old surface's normal on the new
  geometry is a bug, not a quality trade. *(Claude's call)* Shadows the edit casts onto, or
  removes from, *unedited* bricks up to `RAY_SHADOW_DIST` down-sun may refresh over a few frames
  through a budgeted queue, so the edit legs don't pay for a synchronous 60 m invalidation.
- **Unbounded memory.** This machine has 8 GB of unified memory and the SDF atlas alone is
  about 320 MB (64x32x32 bricks x 17^3 bytes). New caches get a fixed budget and degrade
  (fall back to today's path) when full; they never grow.

## Accepted quality trades

The user approved these. Each step must say which ones it used.

1. **Cached normals at reduced resolution, ground and trunks alike.** The fjords ground is a
   B-spline over 1.6 m texels, so a 10–20 cm normal lattice should be indistinguishable on the
   ground. The user accepted the same resolution on trunks (radius 0.45 m), including where a
   trunk meets the ground inside one brick; trunks do not need their own 5 cm path. Small
   edited features soften slightly.
2. **8-bit normals** (about 1–2° quantisation). Possible faint banding on large smooth slopes
   under a low sun.
3. **Cached sun visibility.** Shadow edges get blockier at the cache's voxel scale, and a
   moving sun's shadows trail it by a few frames.
4. **Conifer cards / impostors.** Fewer cards, or an earlier hand-off to impostors, so the
   canopy looks thinner at mid-distance.

Not accepted: reprojection smear, shorter or half-res sun rays.

## Constraints and existing pieces to build on

- **Brick generation** (`shaders/brick_gen.comp.glsl`): one workgroup per brick, 256 threads
  over 16^3 cells plus the 17^3 apron, already evaluating `eval_field_pair` at every lattice
  point. This is where cached normals are written. Bricks stream in over many frames
  (`2d050cc perf: block generation multi framed`), so per-brick cost hurts streaming, not the
  steady frame. It must not make the move leg regress.
- **Stored-normal infrastructure already exists.** `StoredNormalPool`
  (`extension/src/render/stored_normal_pool.h`) holds packed uint16 octahedral normals for
  island volumes and override (edited) bricks. `sample_field_override_gradient` and
  `island_source_normal` read them with `pool_normal16` / `oct_decode_snorm8` and blend
  trilinearly. Reuse the encoding and decoding; whether terrain bricks share that pool or get
  their own is the builder's call.
- **Normal consumer:** `terrain_source_normal` in `shaders/raymarch.comp.glsl`, called once per
  primary hit (and by the glossy reflection ray). Its transparent-material ("opaque view")
  branch must keep working.
- **Sun consumer:** `terrain_sun_visibility` (`shaders/sun_march.glslh`), called per hit pixel
  from raymarch's `main()`. The result goes in `out_albedo.a`. Per project notes, deferred
  only sun-maps far-field pixels, so for a near-field pixel this march is the only shadow
  term. The cache has to cover occluders up to `RAY_SHADOW_DIST` (60 m) up-sun, and island
  shadows (`island_sun_visibility`) stay as they are.
- **Edits** go through brick regeneration and override bricks; both paths must refresh or
  invalidate the caches.
- **Measuring on this Mac:** GPU timestamps are invalid (they return -1), so only wall-clock
  frame time is available. Bracket every comparison (A/B/A), use one process per variant or
  interleave inside one process, and check that a shader override actually compiled
  (`debug_shader_reload_stats().last_ok`).
- **Commits** in this repo for plan and spec docs carry no Co-Authored-By lines.

## Decided

- The walk leg exists before any optimisation lands and is the number every step is judged
  on: same pipeline and seed as fjords-forest
  (`--pipeline=res://assets/pipelines/fjords.pipeline --seed=880510467`), starting at the
  fjords-forest pose (-98.05, 186.0, 417.40, yaw 59, pitch -5), moving at walking speed
  through the trees. It goes into `NAMES` in `tools/run_benchmarks.sh`.
- Order: walk leg, then normal cache, then sun cache, then cards, then the primary march,
  measuring after each. Stop as soon as the walk leg reaches 16.7 ms.
- *(Claude's call)* **The sun is static in the shipped game**: one fixed `DirectionalLight3D` in
  `demo/scenes/voxel_world.tscn`, read each frame by `VoxelWorld::update_sun_state()`, and
  nothing rotates it. So the sun cache is baked per brick with streaming and edits and costs
  nothing per frame. A change in sun direction invalidates it all and re-bakes over a few
  frames (trade 3's lag). No leg moves the sun.
- *(Claude's call)* **Grass blades, leaf clumps and conifer cards keep their own sun march**
  (`terrain_sun_visibility` in `grass_scatter`, `leaf_scatter` and `conifer_scatter`). The
  cache covers near-field terrain pixels only. Price those scatter marches with the cards in
  goal 4. If the ground's blockier cached shadow visibly stops matching the blades standing in
  it, report it rather than making the cache volumetric.
- The `fjords-forest` leg in `tools/run_benchmarks.sh` is still uncommitted in the working tree;
  commit it along with the walk leg.
- The bark special case and the `g_field_skip_detail` gradient shortcut from `1b18a2b` should
  be removed once the normal cache replaces them, if the numbers allow.

## Left to the builder

- Normal cache resolution (5, 10 or 20 cm), layout (texture atlas vs. buffer pool), whether
  to cover only surface-straddling bricks, and the budget. Measure the resident surface-brick
  count in the forest first (`debug_atlas_stats`).
- How the normal is computed at bake time: a gradient of the field at each lattice point,
  differences over the float lattice in shared memory, or a cheaper analytic route. The
  fjords heightfield sample (`sector_sample`) already returns its exact slope.
- What happens on a cache miss: the R8 fallback normal, or today's analytic path.
- Sun cache shape (per voxel, per brick, or per region), refresh policy as the sun moves, and
  how it handles brick edits and streaming.
- Walk-leg details: speed (about human walking pace), duration, and whether it runs
  physics-on like the other legs.
- Card/impostor knobs (`card_reach_m`, `impostor_reach_m`, clump budget in
  `extension/src/conifers/`), if needed.

## Known risk

Caching normals and sun visibility, at their measured upper bounds, takes the standing view
from 32.3 to about 20 ms, not 16.7. The rest has to come from cards and impostors (still
unpriced) and the primary march (goal 5). If all of that still falls short, stop and report
before reaching for anything outside the accepted trades.
