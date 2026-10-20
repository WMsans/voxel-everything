# Voxel Everything — Fjords Water

**Date:** 2026-10-20
**Status:** approved
**Start commit:** `22d9193` (`main`)
**Prior specs:** `2026-10-07-fjords-terrain-design.md` (sub-project A; its §11 sketches this
one as sub-project C); `2026-10-06-water-voxels-design.md` (the water material, its shading and
the three views; read §3 first); `2026-10-01-transparent-voxels-design.md` (the near and far
shells, §5; the composite, §6); `2026-09-17-stage-authoring-design.md` (manifests, the
Lipschitz rule, CPU mirrors).

Sub-project C of the Fjords world: a calm sea at the fjords' water level `W`
(`fjord_height.water_y`, 51.2 m) that fills every valley, like Minecraft fills its river
channels to sea level, and is visible from the shore to the horizon.

---

## 1. Intent

The Fjords world was built with water in mind: valley floors sit 15–40 m below `W`, the shore
band paints `ground_01` up to 2 m above it, and trees stop at the shore. Today those floors are
dry. This fills them.

The water is the existing water material (`Liquid::water`), not a new one. It keeps everything
that material already does (waves, refraction, depth absorption, shore foam, the underwater
view) and its rules (a ghost to colliders and connectivity, removable by the edit tool, no flow).

## 2. Goals, non-goals, anti-goals

Goals:

- **Sea level.** Wherever the fjord ground is below `W`, every point between the ground and `W`
  is water. This includes enclosed dips cut off from the main fjord (the user accepted this).
- **To the horizon.** Water is visible near and far, with no visible seam where the near shell
  hands off to the far shell. This was the user's explicit requirement.
- **It is the existing water.** It uses the same shading, the same ghost rule and the same edit
  behaviour as placed water. Digging it out leaves it gone; it does not refill.
- **Ground does not move.** The opaque and solid views of a fjords world with water equal those
  of the same world without water. Collision, the near-field marcher's ground, ground normals,
  conifers, grass and shore bands are unchanged.
- **Fjords only.** Default, Mesas and Flat stay byte-identical: generated `field.glslh` and every
  frame golden.

Non-goals:

- Rivers, waterfalls, lakes above `W`, drainage from the ridge map, tides.
- Fluid flow or refilling.
- Swimming or buoyancy. The player falls through to the floor, as with placed water today.
- Retuning the water material's look. If the sea at scale looks wrong, that is a follow-up.
- Fixing the far field's level-transition hairline holes (memory: skirts cannot close them).
- **Correct edits below `W` (the user's decision: a known limit, fixed later).** Override bricks
  store union bytes (`brick_consolidate.comp.glsl` bakes from `eval_field`), so after an edit
  near the sea the ground/water split is lost there. The opaque and solid views fall back to the
  label rule, and the ground in edited bricks can sit up to one voxel (5 cm) off for collision.
  An opaque ADD placed under water shows at first, then disappears once consolidated: the union
  is a plain min, so the water's deeper sdf keeps the union material as water. The later fix is
  a second ground/opaque channel in override bricks. "Ground does not move" (§2, §5) is required
  on unedited terrain only.

Anti-goals: things that make this a failure even if water appears.

- The near-field marcher draws the sea as an opaque solid, or ground normals under water point
  up because the water's plane won the gradient.
- A visible band, a gap or a double-dark line where near water meets far water.
- A near-field hole: the far shell is dithered out but the near shell has no chunk there.
- Dry fjords views (looking at the walls, the forest, the peaks) get measurably slower.

## 3. What makes it hard

Each of these was found in the code and is a constraint, not a choice.

1. **The base field has one `(sdf, mat)`.** `eval_field_pair` (`shaders/field_ops.glslh`) seeds
   the opaque accumulator from the base field's union (`osdf = sdf`), and `eval_field_solid`
   seeds the solid one the same way. They rely on "the terrain base field never produces a
   liquid" (water spec §3). A stage that merges water into `sdf` breaks that rule. The marcher's
   brick lattice would mesh the sea as opaque. `terrain_source_normal` reaches
   `base_field_gradient` (`field_ops.glslh`) through `eval_field_gradient`, so it would return the
   water plane's normal for underwater ground. `solid_view`'s label rule (liquid becomes +½ voxel)
   is only an approximation, so it is not enough. The same assumption hides in a shortcut:
   `brick_mark`, `brick_gen` and `solid_sample` take the label rule whenever `s_liquid == 0`, a
   flag derived only from ops (`op_may_hold_liquid`). With terrain water that shortcut is wrong
   and must be fixed.
1a. **Some union readers want the ground.** `grass_bricks.comp.glsl` bisects and differentiates
   the union `eval_field` to find the ground, so with water in the union it would find the sea
   surface. `leaf_trees.comp.glsl` and `lod_field.comp.glsl` also read the union. Every union
   reader must be audited, and each one either keeps the union or moves to the ground view.
2. **The near shell never sees terrain water.** `LodSystem::refresh_shell_candidates` builds
   candidates from op AABBs, volumes and override bricks (`ve::shell_candidates`). Terrain
   water produces none of these, so near-field water would be missing out to the fade end plus
   one chunk.
3. **The far field already sees it.** `lod_reduce` raises "has transparent" from the sampled
   field, `lod_opaque` applies the label rule at each level's cell size, and the far shell
   meshes transparent-sided quads. Far water should appear once the field produces it. Ground
   under far water sits up to half a cell off; it is under absorbing water, so this is accepted.
4. **Per-pixel cost.** A field stage runs 7× per near-field hit pixel
   (`base_field_gradient`), and the ground-level fjords forest is already 51 ms (memory). A
   water term must cost nothing for a pixel that never needs it.

## 4. Decided

- **Water level is `fjord_height.water_y`**, read by `//!use` like `fjord_bands` and
  `conifers` already do. There is no second knob.
- **The base field carries ground and liquid separately.** The union view (what every existing
  consumer reads today) gains the water. The opaque and solid accumulators start from the
  ground alone, so §2's "ground does not move" holds by construction rather than by the label
  rule. How the pipeline expresses this is §6's first open item.
- **Near-field candidates gain a terrain source.** Shell chunks that straddle `W`, within the
  existing candidate radius, over ground below `W`, become candidates exactly as an op AABB's
  chunks do. Only one layer of chunks holds a water surface (the air/water boundary is the plane
  `y = W`; the water/ground interface is not meshed), so this is at most about 2,000 chunks at
  the default fade band.
  **Correction from review:** `W` = 51.2 m = 16 × 3.2 m lies exactly on a shell-chunk boundary,
  and `shell_candidates` pads each box by one cell, so two layers become candidates: about
  4,200 chunks at the default band (radius 83.2 m). They share the far field's 32,768-page LoD
  pool. `W` is also a lattice plane at 0.1, 0.4 and 0.8 m, so samples on it read exactly
  sdf = 0. The builder checks both the page budget and the exact-zero plane.
- **Not-resident sectors.** A per-frame consumer that falls back to `W − 64` (fjords spec §5.4)
  would read 64 m of water. Water is air in the solid view, so nothing becomes falsely solid.
  Bricks, colliders and shell builds are already gated by `SectorContext::ready`
  (`lod_chunk_sectors_ready`). The gap is candidate selection: `refresh_shell_candidates`
  recomputes only on a chunk crossing, a toggle or an edit. Terrain candidates must also be
  refreshed when a sector lands.
- **Horizon fallback (my call).** Fjords deviation 12.1.10 lets far LoD chunks past the sector
  cache radius (about 4 km) build from the `W − 64` fallback. With water, those chunks become
  flat sea at `W`. This is accepted: a flat sea at the horizon serves "to the horizon". There is
  no dry ring and no fallback change.
- **Transparency off (my call).** `transparency.enabled` is on by default and is only switched
  off by `benchmark.gd --transparency=`; in normal play the sea is fully transparent. With it off the atlas stores the union,
  so the sea is drawn as an opaque `flat_albedo` slab, which is what the water spec says water
  looks like in that mode. The anti-goals apply with transparency on only. The transparency-off
  mode is out of scope and is not verified beyond "does not crash".
- **Own module rule** (memory and water spec): `BeautySettings`, `deferred.comp.glsl`,
  `shade.glslh`, `ssr.comp.glsl` and `outline.comp.glsl` are not edited.
- **CPU mirror.** Whatever produces water on the GPU has a `//!cpu` mirror, so
  `test_field_diff.gd` and the CPU views agree.

## 5. Verification (the done bar)

- **No-water pipelines are byte-identical:** generated `field.glslh` for Default (the existing
  golden) and for Mesas and Flat (baselines recorded at `22d9193` before any change), and every
  existing frame golden. No frame golden uses Fjords. `tests/test_brick_cache.gd` and
  `test_conifers.gd` load `fjords.pipeline`; point them at a no-water fixture, or re-record them
  with a stated reason.
- **Ground does not move:** for the fjords pipeline with and without the water stage
  (`tests/fixtures/` gains a no-water fixture, like `fjords_no_conifers.pipeline`), the brick
  opaque lattice, the collision lattice (`debug_mesh_lattice_diff`) and the ground normal at
  underwater points match.
- **Water exists where it should:** the union view's material is water at points below `W` over
  low ground, and air above `W` and over ground above `W`.
- **Near and far:** the shell candidate count is non-zero at a shore camera. A capture across the
  fade band shows water on both sides with no seam, judged in the same way as the water spec's
  `water_seam.png`.
- **Edit:** removing water near the shore leaves a hole that stays.
- **Captures,** opened and looked at, of: a shore view, an aerial view down a long fjord to the
  horizon, an underwater view, and a fade-band crossing. Note `fjords_capture.gd` already
  exists, and that trees are rare at the default camera (memory).
- **Cost:** interleaved A/B/A frame time (GPU timings are invalid here, memory), water stage on
  vs off, at a dry wall view and a sea view. A dry view must show no quotable regression; the sea
  view's cost is reported, not gated.

## 6. Left to the builder

1. **How the pipeline expresses a liquid.** For example, a stage output such as
   `liquid_sdf`/`liquid_mat` that the generated `eval_base_field` folds into the union only, or a
   separate `eval_base_field_views`. The constraints: §2's byte-identity for no-water
   pipelines; the opaque and solid seeds are the ground; the Lipschitz bound still holds (the
   half-space `p.y − W` has gradient 1, so it is below the fjord's 8.54); and the union-only
   cost is paid by union consumers only.
2. Whether the per-pixel gradient path needs any change once the ground is separated, or is
   already ground-only.
3. How the terrain candidate source asks "is there ground below `W` here": the sector cache's
   heights, or a conservative per-chunk test.
4. Where the stage lives (a new `fjord_water.field.glslh`, or an output of `fjord.field.glslh`)
   and what it is called.
5. Foam: does shore foam come out right where terrain meets sea at a 15–40 m depth slope? Look
   at it. Tune only through existing water knobs.
