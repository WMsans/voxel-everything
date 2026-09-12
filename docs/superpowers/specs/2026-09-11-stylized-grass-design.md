# Stylized Grass — Design

Date: 2026-09-11
Status: approved design, ready for an implementation plan

Dense, stylized grass blades growing on `grass_01` voxels, in the visual register of
*Breath of the Wild*: a field of thin bright blades with a real lean direction, gust waves
crossing it, and a dark-root-to-bright-tip gradient.

## 1. Goals and non-goals

Goals:

- Blade geometry on near-field `grass_01` surfaces, dense enough to read as a meadow.
- Wind: gust waves across the field, per-blade phase, tip jitter.
- Edits destroy grass: dig a grass voxel away, or paint it to another material, and the
  blades there are gone on the next frame.
- A self-contained module. No edits to the deferred/beauty stack.

Non-goals, decided explicitly:

- **Player interaction.** Blades do not part around the player or any other body.
- **Cutting.** No tool interaction, no clippings.
- **Sun shadow map.** Blades are not rendered into the sun cascades. They receive sun
  shadow from the deferred pass and self-shade through AO and backlight, which is what both
  Ghost of Tsushima and the Godot full-geometry approaches do.
- **Far-distance meadow coverage.** Making the horizon read as grass means changing how
  far-field grass pixels shade, which is the one part that cannot stay inside this module.
  It is deferred to its own scope. Blades plus the distance-thinning tail may carry the
  image far enough that it is not needed.
- **A frame budget.** The posture for this work is looks first, tune later (section 9).

## 2. Prior art

| Source | Approach | Taken / rejected |
|---|---|---|
| [2Retr0/GodotGrass](https://github.com/2Retr0/GodotGrass) (MIT) | MultiMeshInstance, CPU placement, tile LOD by mesh swap | Rejected. Author documents both constraints: placement must happen on the CPU, and mesh-swap LOD pops visibly at tile boundaries. |
| [hexaquo full-geometry grass](https://hexaquo.at/pages/grass-rendering-series-part-2-full-geometry-grass-in-godot/) | MultiMesh + stylized vertex/fragment shader | Shading tricks taken: normals interpolated toward vertical for roundness, clump noise sampled at world XZ, backlight translucency, no backface culling. |
| [Leon Stansfield, dense foliage in Godot](https://leonstansfield.github.io/Posts/rendering_dense_foliage_in_godot/rendering_dense_foliage_in_godot.html) | MultiMesh chunks, timer-based distance culling | Rejected. Documents the bottleneck: frustum culling is per-patch, so one visible blade draws the whole chunk. |
| [Procedural Grass in Ghost of Tsushima (GDC)](https://gdcvault.com/play/1027033/Advanced-Graphics-Summit-Procedural-Grass), [writeup](https://tigerabrodi.blog/grass-in-ghost-of-tsushima) | Compute scatter per tile, frustum/distance cull, indirect draw, 100k+ blades, no CPU involvement | The architecture. Also taken: density thinning for LOD instead of mesh swap, view-space thickening, per-blade hash driving height/lean/colour/phase, clumping. Occlusion culling rejected — the talk measures it as marginal for grass. |
| [BotW technical analysis](https://www.resetera.com/threads/zelda-breath-of-the-wild-the-technical-analysis.8197/) | One triangle per blade at close range, vertex-colour gradient, wind displaces the top vertex only | The blade itself. This is the reference image's actual recipe. |

**Why the Godot ecosystem's constraint does not apply here.** Every Godot solution above is
built around `MultiMeshInstance`, which requires blade positions to be computed on the CPU
or survive a GPU→CPU→GPU round trip. This engine never uses Godot's scene renderer for
terrain. It owns its `RenderingDevice`, `lod_raster_pass.cpp:332` already issues
`draw_list_draw_indirect` against GPU-written args, and `lod.vert.glsl` pulls all geometry
from storage buffers with no vertex format at all. The Ghost of Tsushima pipeline is
directly available, and none of the MultiMesh workarounds are relevant.

## 3. Architecture

New module `extension/src/grass/` — CPU-side, GPU-free, built into the native
`pure_sources` test target:

- `grass_settings.h` — `ve::GrassSettings` plus `ve::clamp_grass_settings`. Follows the
  `BeautySettings` convention without joining it.
- `grass_layout.h` / `.cpp` — `ve::GrassLayout` and the free function
  `ve::grass_layout(const GrassSettings &, const float camera[3], const float view_proj[16])`
  that produces it. Pure arithmetic: the brick-space search box, the per-distance blade budget
  per brick, the frustum planes, and the total instance capacity. The drop-3-of-4 thinning
  lives here so it is testable without a GPU. There is no `GrassField` object; the layout is a
  value recomputed each frame. Signatures take plain `float` arrays and `ve::IVec3` rather
  than `Vector3` / `Projection`, because this module is in the native test target and must not
  depend on godot-cpp.
- `grass_settings_store.h` / `.cpp` — `ve::GrassSettingsStore`. Owns the mutex and the
  render-thread snapshot, mirroring the shape of `beauty_mutex_` / `beauty_snapshot()`
  without joining it.

Two new passes in `extension/src/render/`, following the `LodCullPass` → `LodRasterPass`
idiom already in the frame graph:

- `grass_scatter_pass.h` / `.cpp` — `GrassScatterPass`, two compute dispatches.
- `grass_raster_pass.h` / `.cpp` — `GrassRasterPass`, one indirect draw.

New shaders:

- `shaders/brick_atlas.glslh` — extracted, see section 4.
- `shaders/grass_bricks.comp.glsl` — stage 1, brick cull and compaction.
- `shaders/grass_scatter.comp.glsl` — stage 2, blade placement.
- `shaders/grass.vert.glsl`, `shaders/grass.frag.glsl`, `shaders/grass.glslh`.

Per-frame flow, inserted between the LoD raster and SSGI:

```
ve::grass_layout(settings, camera)   CPU, a few dozen ops
  -> grass_bricks.comp    brick cull      -> compacted brick list + dispatch args
  -> grass_scatter.comp   blade placement -> instance SSBO + draw args
  -> GrassRasterPass::draw                -> GBuffer albedo / surface / depth
  -> existing SSGI / SSAO / deferred / outline shade it, unmodified
```

## 4. Shared-header extraction

`region_slot_of`, `slot_in_region`, `slot_at`, `brick_sdf`, `world_sdf` and `material_at`
currently live inside `shaders/raymarch.comp.glsl` (lines 104–206). The scatter pass needs
all of them. A second copy of the atlas addressing is exactly how two implementations of one
path drift apart, so they move verbatim to `shaders/brick_atlas.glslh`, included after the
bindings are declared — the convention `common.glslh` already uses ("the material arrays must
be declared before common.glslh so `material_surface()` can see them").

This is a cut-and-paste with no behaviour change. It is pinned by running the existing
raymarch tests before and after, and the move is its own commit so a regression bisects
cleanly.

## 5. Placement

**Why brick-driven rather than column-driven.** Finding the ground at a candidate XZ by
marching down costs tens of SDF samples over a 40 m range. Starting from bricks that already
straddle the surface bounds the search to one brick's 0.8 m — about ten samples per blade —
and handles caves and overhangs correctly for free.

### Stage 1 — `grass_bricks.comp.glsl`

One thread per brick over a camera-local brick grid. At the default `reach_m` of 40 m that
box is roughly 100 × 25 × 100 bricks ≈ 250k threads; the vertical extent is much smaller than the
horizontal because grass grows on the ground. Each thread:

1. `slot_at(brick)` — two dependent loads. Reject if not resident.
2. `brick_flag_word(slot)` — one buffer load. Reject unless the brick straddles the surface.
3. Frustum-reject the brick's AABB against the six planes.
4. Append the survivor to a compacted list and bump the stage-2 indirect dispatch args,
   following the existing `dispatch_args.comp.glsl` convention.

### Stage 2 — `grass_scatter.comp.glsl`

One thread per candidate blade over the compacted list. The per-brick blade budget comes
from `ve::GrassLayout` and falls with distance — the drop-3-of-4 thinning, which is Ghost of
Tsushima's LOD and avoids the mesh-swap popping GodotGrass documents. Each thread:

1. Hash the brick coordinate and the candidate index into a jittered XZ inside the brick.
2. Find the SDF zero crossing along the brick's 0.8 m vertical span: a short stepped search
   plus one bisection refine.
3. Reject if the surface normal is not up-facing (no grass on cliff faces) or if
   `material_at()` is not `grass_01`.
4. Append a blade instance through an atomic counter and bump the draw args.

Edit-awareness falls out of this: the scatter reads the live atlas, and an edit regenerates
those bricks, so blades vanish the frame after the voxels do. No invalidation code exists
because none is needed.

Reach is bounded by brick residency, which is the same seam the near field already honours.

### Instance record

32 bytes, two `vec4`:

- `a` — world position `xyz`, blade height `w`
- `b` — oct-packed ground normal, hash, lean angle, clump weight

## 6. Blade geometry, wind, shading

### What the GBuffer needs

`lod.frag.glsl` writes `out_albedo = vec4(resolved_rgb, 1.0)` (alpha is sun visibility) and
`out_surface = vec4(oct_encode(n), float(material), gloss)`. `deferred.comp.glsl` reads the
material id **only** for the emissive lookup, and `mat_glow(grass_01)` is zero. So a blade
writes:

```glsl
out_albedo  = vec4(blade_colour, 1.0);
out_surface = vec4(oct_encode(n), 1.0, gloss);   // material id 1 = grass_01
```

and is correctly cel-shaded, sun-shadowed, SSAO'd, SSGI-lit and outlined with **no new
material id**. `material_table.h`, its byte-exact `.glslh` mirror, and
`tools/convert_materials.sh` are untouched.

### Geometry

Three vertices per blade, per the BotW analysis. `gl_VertexIndex / 3` is the blade,
`% 3` is the corner: two at the base offset ±width along the blade's side vector, one at the
tip. Geometry is pulled from the instance SSBO with no vertex format, as `lod.vert.glsl`
does — which also routes around the `gl_DrawID` / non-zero `firstInstance` gap that file
documents.

Each blade has a **real lean direction** from its hash rather than being a camera billboard;
that directionality is most of what makes a field read as a field. Ghost of Tsushima's
**view-space thickening** keeps edge-on blades from vanishing: shift vertices toward the
camera as the blade turns side-on, rather than rotating it to face the camera.

### Wind

Only the tip vertex moves, per BotW. Three layers:

- A scrolling procedural noise sampled at world XZ gives the gust field — the light and dark
  waves crossing the meadow in the reference.
- Per-blade `sin(time * rate + hash)` keeps neighbours out of phase.
- A high-frequency term jitters the tip.

Bend goes as `pow(t, 2.0)` along the blade so the base stays planted.

### Normals

With three vertices, roundness comes from per-vertex splay: base normals fan outward ±, the
tip's leans toward vertical. Interpolation then shades a flat triangle as a rounded blade —
hexaquo's `mix(n, vec3(0,1,0), t)` generalised across the width. Backfaces are not culled,
and the normal flips for back-facing fragments so a blade seen from behind is not black.

### Colour

- Vertex gradient, dark cool root to bright warm tip. This is the BotW vertex-colour trick
  and it does most of the work.
- Per-blade hue and value jitter from the hash.
- Clump noise sampled at world XZ modulating height and colour, which produces the patchiness
  in the reference rather than a uniform lawn.
- A small hash fraction gets a flower colour at the tip, for the yellow specks. One knob,
  near-free.

### Fade

Blades dither out at the reach limit using the same `bayer4` discard `lod.frag.glsl` uses at
the LoD seam, so the density tail does not pop.

Overdraw, not vertex count, is the cost that will bite. Blade width and reach are the dials.

## 7. Integration

The whole edit to `raymarch_compositor.cpp`, after the LoD raster block and before SSGI:

```cpp
if (GrassScatterPass *gs = world->grass_scatter_pass()) {
    timings->begin(rd, "grass");
    const ve::GrassLayout gl = ve::grass_layout(world->grass_settings(), cam.origin, view_proj);
    if (gs->run(rd, *atlas, gl) && grass_raster->draw(rd, *gs, *gb, view_proj, cam_pos))
        timings->end(rd, "grass");
    else
        timings->cancel("grass");
}
```

**Lifetime.** Both passes are constructed in `RenderOrchestrator::ensure_gpu_graph()` beside
`lod_raster_pass_` and destroyed in `teardown_render_passes()`, preserving the documented
CPU-outlives-GPU ordering.

**Hot reload.** No registration needed: `preflight_shaders()` enumerates
`res://shaders/*.glsl` and infers the stage from the filename suffix, so the grass shaders
join shader reload automatically.

**Settings.** `VoxelWorld` gains one `GrassSettingsStore` member and two one-line
delegations (`grass_settings()`, `set_grass_value()`). `BeautySettings`, its tier table,
`pack_flags` and `test_beauty_settings` are untouched.

**Demo knobs.** A grass section in the settings/debug menu — on/off, density, reach, blade
width, blade height, wind strength, wind speed — following the existing effect-row pattern.

## 8. Failure modes

- **Instance buffer overflow.** The buffer is fixed-size, allocated from the capacity
  `ve::grass_layout()` computes for the maximum reach. The scatter appends through an atomic
  counter and clamps: an overflowing frame drops blades rather than scribbling, and the pass
  records a high-water mark so the overflow is visible rather than silent. Same treatment `lod_overflow_logged` gives page
  overflow.
- **Zero blades.** Draw args carry zero vertices and the indirect draw is a no-op. A camera
  in the air with no resident bricks takes the same path. No special-casing.
- **Pass failure.** `timings->cancel("grass")` and skip; the frame continues without grass.
  Deliberately unlike the near field's `abort_frame()` — grass is decorative and must never
  take a frame down with it.
- **Counter initialisation.** The append counter is cleared explicitly every frame. Stated
  because on this machine a fresh RD buffer reads back as zero, so "the count came back zero"
  is a real zero and never an uninitialised-memory story.

## 9. Performance posture

Looks first, tune later, by decision. Density defaults are chosen for the image, not for a
budget number. A `grass` timing label and an on/off toggle ship from the start anyway,
because they cost nothing and they keep the existing benchmark legs comparable with grass
off.

Any cost claim must come from **interleaved A/B/A wall-frame runs** with grass on and off.
GPU timestamps read back invalid on this machine, so per-pass timings cannot support a claim
here.

## 10. Testing

**Native, doctest, `pure_sources`** — `extension/tests/test_grass_field.cpp`: ring layout and
drop-3-of-4 thinning arithmetic, capacity computation, brick search-box bounds, settings
clamping. No GPU. The density-versus-distance contract is pinned here, where it is cheap.

**GPU, gdUnit** — `tests/test_grass.gd`, every assertion reading back buffers the real pass
wrote:

- no blades over a rock-only region; blades over a grass region
- blade count falls monotonically as the camera retreats
- digging grass voxels away drops the blade count on the next frame
- the count clamps at capacity instead of overflowing
- every emitted blade sits inside its source brick, on an up-facing surface

**Golden capture** — one deterministic `--capture` frame into `tests/golden`, so wind and
colour changes have to be deliberate.

**The `hooks.cpp` rule.** That file is 6063 lines of probes that re-assemble render inputs,
which means a green test there can be exercising a path that does not ship. Grass gets
exactly one hook, `debug_grass_stats()`, and it only reads back counters and instances the
real scatter pass wrote during a real compositor frame. No probe that rebuilds a parallel
scatter. A behaviour that cannot be tested against shipping output does not get a test.

## 11. Risks

- **Overdraw** is the cost, and the reference look is dense. Expect blade width and reach to
  be the knobs that matter, and expect the M1 to feel it.
- **Far field does not write depth on macOS/Metal** (pre-existing, `docs/PORTFOLIO.md`).
  Blades write depth, so they will occlude each other and the near field correctly while far
  terrain continues to ghost through. This may look worse next to correct grass than it does
  today. It is not caused by this work and is not fixed by it.
- **Stage 1 dispatch width** scales with the cube of `reach_m`. The 40 m default is
  affordable; a much larger reach needs a hierarchy rather than a bigger box.
- **The header extraction touches the hottest shader in the engine.** Separate commit, tests
  run either side.

## 12. What shipped

Measured cost (Apple M1 Mac mini 8 GB, shipped defaults): grass costs **+3.79 ms wall p50**
(+3.25 p99) on the steady leg and **+3.21 ms p50** (+1.09 p99) on ridge, by interleaved A/B/A
runs (`tools/run_benchmarks.sh grass-off-a --grass=0`, `grass-on --grass=1`,
`grass-off-b --grass=0`; delta is on − mean(off-a, off-b); full tables in
`docs/PORTFOLIO.md`). The off-a/off-b bracket is tight (steady p50 identical at 25.00 ms),
so the delta is grass, not drift. Per-pass GPU attribution was impossible — timestamps read
back invalid on this machine — so the `grass` timing label stays a toggle for keeping legs
comparable, not a claim.

Deviations from the design:

- The edit-contract test paints rather than digs. Digging the brief's crater at the camera
  spot grows NEW blades on the fresh grass-band crater floor (74 → 409 blades measured),
  which confounds removal with exposure; painting the whole reach to rock moves only the
  material layer, so the measured 74 → 0 blades proves the scatter reads the live atlas.
  Same contract (edits destroy grass, no invalidation code anywhere), sharper instrument.
  No production change was needed — the paint variant passed on its first run.
- The brief's `debug_apply_edit(centre, radius, material)` hook does not exist; the test
  uses the verbatim `debug_apply_sphere_paint(centre, radius, material)` from `hooks.cpp`
  (rock is material id 2). No new hook was added.
- Overflow reporting logs once per pass lifetime (flag reset in `teardown()`), mirroring how
  `lod_overflow_logged` treats page overflow: a per-frame error line would hide the next
  real one. The high-water mark stays sticky, as designed — it is the visible record.

Far-distance coverage: still deferred and still open, as designed. The blades and the
thinning tail now exist to judge against, and the A/B/A harness (`--grass=`) gives any
future far-field experiment its control leg for free.

## 13. Lighting revision (2026-09-12)

The grass read as stiff, unshadowed and unlit. Two claims in this doc were wrong and caused
the last two:

- **Section 1, "receive sun shadow from the deferred pass".** They did not. `deferred.comp.glsl`
  takes G-buffer albedo alpha as sun visibility and only mins in the sun map where
  `far_field_owns(px)`. Near-field shadow comes from the raymarcher's own sun march written
  into that channel, so blades writing `1.0` were never shadowed near the camera.
- **Section 6, ground normal for every vertex.** It lit every blade identically, so the field
  landed in one cel band and read as flat paint.

What changed, all still inside the grass module:

- **Shadow.** `terrain_sun_visibility` moved verbatim from `raymarch.comp.glsl` to
  `shaders/sun_march.glslh` (proved byte-identical once inlined). The scatter marches it once
  per blade and packs the result as a byte into bits 16–23 of `b.x` beside the 16-bit oct
  normal. The record stays 32 bytes. The fragment writes it, scaled by a root-to-tip canopy
  self-shade (`GRASS_ROOT_SUN = 0.35`), as albedo alpha.
- **Sun response.** Each blade writes its Bezier face normal, rounded across the width,
  blended against the ground normal by the new `blade_lighting` knob (default 0.6, carried in
  `style[3]`), fading to the pure ground normal by the reach. Floored at 0.25 against the
  ground normal so the black-grass bug cannot return.
- **Wind.** Blades are 27-vertex cubic Bezier profiles (was 9). Wind changes the bend angle,
  the profile keeps its arc length (Gravesen's estimate, measured exact to 4 digits), the gust
  is re-centred so blades swing back past rest, bob rate varies per blade, and gusts swing the
  lie direction a little.

The blade maths lives in `shaders/grass_blade.glslh` and is executed natively by
`extension/tests/test_grass_blade_shader.cpp`. `test_grass.gd` pins that open grass is sunlit
and that a rock placed up-sun shadows it; both stream from (20, 60, 30), because the usual
(30, 56.2, 30) hook view sees only cave-floor blades, which are correctly in full shadow.

Blades still do not cast into the sun map or onto the ground (non-goal unchanged).

Measured cost (Apple M1, 2560x1440, vsync off), interleaved main / this change / main, 300
frames per leg: steady p50 29.63 / 30.00 / 29.63 ms (**+0.37 ms**), frame avg 29.41 / 30.05
/ 29.46 (+0.62); ridge p50 27.78 / 28.33 / 27.78 ms (**+0.55 ms**), frame avg 28.26 / 28.26
/ 28.22 (flat). The two main legs agree exactly on p50, so the delta is this change rather
than drift. It covers the per-blade sun march and the tripled vertex count together; the
split between them is not measurable here, because GPU timestamps are invalid on this
machine.
