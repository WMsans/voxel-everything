# Voxel Everything — Transparent Materials

**Date:** 2026-09-29
**Status:** approved design, ready for an implementation plan
**Prior specs:** `docs/superpowers/specs/2026-09-11-stylized-grass-design.md` (the
own-module pattern this follows); `docs/superpowers/specs/2026-08-17-m5-lod-design.md` (the
far-field surface-nets pipeline §5 extends).
**Start commit:** `1b0e605`.

Clear, tinted, see-through voxel materials. Ice is the first; water and anything else
later is one row in the material table. You see the terrain behind the ice sharply, tinted
and darkened by the thickness of ice the view ray crosses, at every distance: near field
and far field.

---

## 1. Goals and non-goals

Goals:

- **Generic transparency.** It is a property of a material row, never a special case for
  one id. No shader or pass names "ice".
- **Clear, tinted-glass look.** Beer–Lambert absorption by thickness, Fresnel sky
  reflection, and the material's own cel-shaded body colour where the light runs out.
- **Near and far field.** The raymarched near field and the meshed far field both render
  transparency, and meet at the fade band exactly as opaque terrain does.
- **Zero cost without transparency.** Scenes with no transparent material render
  bit-for-bit as today, whether the feature is on or off.
- **No edits to the deferred / beauty stack.** The feature is its own module, and the one
  unavoidable stack edit is the raymarcher (§4).

Non-goals, decided explicitly:

- **Refraction and frosted blur.** Chosen against: refraction is screen-space and breaks at
  screen edges and behind raster foliage; blur costs a filter pass. Both could later build
  on the front layer this design adds.
- **Shading the internal boundary between two transparent materials** (ice in water).
  A ray passing through several transparent materials still absorbs correctly in each.
- **Islands containing transparent materials.** They render opaque until the walker (§4) is
  ported to the island sampler.
- **Glossy reflection rays through transparency.** Reflected rays see transparent materials
  as opaque.
- **Far-field shadows from transparency.** The sun map rasterizes the opaque mesh only.
- **Outlines and SSR on transparent fronts.** Both run after `inject` on the G-buffer, which
  now describes the surface behind (§8).

## 2. Background

Ice is not its own shape. It is a material label on the single union SDF the whole world
is: `OP_SPHERE_ADD` sets `mat`, and `OP_SPHERE_PAINT` relabels solid voxels in place
(`shaders/field_ops.glslh`). Where ice rests on ground the SDF is negative all the way
through; **there is no surface between the ice and what is behind it**, only a change of
label. Terrain generation never places ice today; it exists wherever the player paints or
adds it.

The engine has no TAA. That rules out the Teardown approach (dithered screen-door
transparency in the G-buffer, resolved by TAA). A forward pass after deferred assumes the
transparent surface is raster geometry, and the near field is not. What fits is marching
through the medium, which this design does in the near field, and meshing the medium's
shell, which it does in the far field.

Approaches considered for finding the surface behind:

1. **Label boundary (chosen).** Walk the existing (sdf, label) data and stop at the first
   opaque label. Works unchanged for add, paint, islands' hand-back and consolidated
   override bricks; no new field channel.
2. **Second "opaque SDF" field (ice as air).** Exact seams and cheap thick-ice steps, but
   doubles every field evaluation, adds a byte per voxel to bricks, override bricks and
   volumes, drags consolidation and islands into it, and paint-to-ice has no defined
   "removed" shape.
3. **Dithered G-buffer.** Trivial, but a visible stipple with no TAA.

## 3. Material data

`ve::MaterialDef` (`extension/src/world/material_table.h`) gains:

```cpp
float transmit[3]; // fraction of light per channel left after 1 m; {0,0,0} = opaque
float ior;         // index of refraction, for Fresnel reflectance (unused when opaque)
```

| material    | transmit             | ior  |
|-------------|----------------------|------|
| `ice`       | {0.80, 0.90, 0.95}   | 1.31 |
| `ice_crack` | {0.55, 0.65, 0.70}   | 1.31 |
| all others  | {0, 0, 0}            | 1.0  |

`ve::material_table_glsl()` emits `MAT_TRANSMIT[]`, `MAT_IOR[]` and
`bool mat_transparent(uint id)`. Like every other table lookup, `mat_transparent` is
range-checked, and foliage and air are opaque. The byte-exact generator test is
re-recorded.

**The rule every consumer follows.** A solid voxel whose material is transparent counts as
clear for visibility. Along a path through transparent solids the transmittance is
`T *= transmit[mat]^length` for each segment. A path ends at the first opaque solid or on
exit into air.

## 4. Near field — the walker

`raymarch.comp.glsl` gains `walk_transparent`, entered when the primary march's hit is a
transparent material.

- **Step:** `max(min_step_m, pixel footprint at t)`, where the footprint is the ray's world
  width at distance t, the same derivation the glossy path uses. It is 5 cm near the
  camera and grows with the pixel cone.
- **Sampling:** the brick atlas the marcher already reads, trilinear SDF and nearest label.
- **Accumulation:** `T *= transmit[mat]^step`, per channel.
- **Termination, first match wins:**
  1. **An opaque label (SDF ≤ 0).** This is the surface behind. Its position is refined by
     bisection on the label within the last step. Its normal is the gradient of the
     trilinear opaque occupancy (1 for opaque solid, 0 otherwise) over the eight
     neighbouring voxels.
  2. **Air (SDF > 0).** Hand back to `march_terrain` from here. The next hit may be opaque,
     or another transparent front, which is walked the same way but is not shaded as a
     front (§1).
  3. **`max(T.r, T.g, T.b) < min_transmit`.** The medium is opaque from here on. Nothing
     behind is marched. The G-buffer gets the front surface itself and `trans` is written
     as T = 0, so the composite shows body only (§7).
  4. **`max_steps` reached.** A safety net only. Treated as case 3.

**Sun.** `terrain_sun_visibility` treats transparent solids through the same walker: sun
visibility at any surface is multiplied by the transmittance of the ice between it and the
sun, so ground seen through ice is lit through a tinted shadow, not black. For the scalar
sun term the tint collapses to `max(T)`.

**Outputs.**

- The existing G-buffer (`out_albedo`, `out_surface`, `out_hitpos`) receives the **surface
  behind**: its position, normal, material and sun visibility. SSAO, SSGI, deferred, contact
  shadows, outlines and the raster depth test for grass and leaves all see an ordinary
  surface and need no change.
- Two new march-resolution targets owned by the transparency module, bound to the raymarch
  pass:
  - `front` (rgba16f): xy octahedral normal, z front distance t, w material id (0 = no
    transparent front).
  - `trans` (rgba16f): rgb transmittance T, a front sun visibility.

The raymarch pass's bindings, and this shader, are the only beauty-stack files edited.

## 5. Far field — one extra mesh per chunk that has transparency

**Detection.** `lod_reduce.comp.glsl` sets a per-job "has transparent label" bit when any
solid target sample carries a transparent material. Jobs without it take exactly today's
path, and pay nothing.

**Opaque mesh.** For flagged jobs, a new `lod_opaque.comp.glsl` writes a derived lattice:
each solid sample with a transparent label becomes `+½ cell` (just outside); every other
sample is copied. The existing `lod_frac` / `lod_quads` then run on the derived lattice with
no changes. Air-to-opaque edges keep exact SDF crossings; transparent-to-opaque edges gain a
crossing inside the cell, and normals come from the existing trilinear gradient.

**Shell.** A second `lod_quads` run, on the original lattice with a `shell_only` push flag,
keeps only quads whose solid side is a transparent material. Its `lod_frac` is the one the
original lattice already needs.

**CPU mirrors.** `ve::lod_reduce_lattice` reports the flag, a new `ve::lod_opaque_lattice`
mirrors the derived lattice, and `ve::lod_contour` gains the shell filter. The existing
GPU/CPU parity tests cover all three.

**Storage and draw.** Each flagged chunk gets a second arena range for its shell quads, and
the LoD pool carries a second indirect draw list. That list is filled by the same cull
pass, from the same per-chunk frustum and HiZ verdict as the opaque list.

**Raster.** A new `TransparentRasterPass` in the transparency module runs after the LoD
raster.

- It draws the shell's front faces into the full-resolution front layer (§6).
- It depth-tests against the G-buffer depth (read-only) and against its own depth
  attachment, so the nearest front wins and fronts hidden by opaque terrain are dropped.
- It applies the fade-band `bayer4` complement exactly as `lod.frag.glsl` does, so near
  and far fronts meet like terrain does.
- It writes `T = transmit^thickness`, where thickness = G-buffer linear depth − front
  linear depth.

**Known ceiling.** An air gap behind far-field ice is counted as ice. Past ~120 m that
should not be noticeable. A later back-face pass would fix it.

## 6. The front layer

The transparency module owns a full-resolution front layer, `front_full` / `trans_full`,
laid out as §4 describes, plus a depth attachment.

- The near field writes its march-resolution targets. `composite.frag.glsl` upsamples them
  into the full-resolution layer with the nearest sampler, the same way it already resolves
  geometry.
- The far-field shell raster (§5) writes the same layer.

The shading pass reads one layer, whichever field produced a pixel.

## 7. Shading — `transparency_composite`

A compute pass after `deferred` and before `inject`, timed as its own stage, rewriting the
lit colour in place.

Per pixel:

1. No front (id 0), or G-buffer depth < front depth (raster grass or leaves in front of the
   ice): leave the pixel untouched.
2. **Body:** `cel_shade` on `material_surface(mat, front_pos, n)` albedo, with the front
   normal, sky ambient and front sun visibility. Far-field fronts take
   `sun_map_visibility`, as `deferred` does for far-field pixels. Ice keeps the art style
   and its texture.
3. **Fresnel:** `F0 = ((ior−1)/(ior+1))²`, `F = F0 + (1−F0)(1−n·v)^5`; the reflection is
   `sky_color(reflect(−v, n))`.
4. **Result:** `F·sky + (1−F)·(T·behind + (1−T)·body)`, per channel. The walker's
   termination cases 3 and 4 arrive with T = 0, which gives body only.

## 8. Settings, module and failure handling

- **Module:** `extension/src/transparency/`: `TransparencySettings`, its settings store,
  and the front-layer resources.
- **Passes:** `render/transparent_raster_pass.*` and `render/transparency_composite_pass.*`.
- **Integration:** a handful of lines in `VoxelFrame::render_pre_opaque` and the pass
  registry. `BeautySettings`, `deferred.comp.glsl` and `shade.glslh` are not edited.

| setting        | default | meaning |
|----------------|---------|---------|
| `enabled`      | true    | Off: the walker is skipped (transparent materials hit as opaque) and T is forced to 0, which reproduces today's opaque render with no LoD rebuild. Also the benchmark A/B toggle. |
| `min_step_m`   | 0.05    | The walker's smallest step. |
| `max_steps`    | 48      | The walker's safety cap. |
| `min_transmit` | 0.01    | The walker's ε cutoff. |

**Failure handling.** If a transparency pass fails to render, its stage is cancelled and
the frame continues. The G-buffer then shows the surface behind with no ice over it for
that frame. The frame is never aborted for a transparency failure.

**Known ceilings, deferred.**

- Outlines and SSR read the G-buffer after `inject`, so outlines of rocks under ice draw
  over the ice, and the ice's own silhouette gets no outline.
- Everything in the §1 non-goals.

## 9. Testing

Characterization first:

1. **Pins before any edit.** The existing near and far goldens are the baseline. Every task
   keeps scenes with no transparent material bit-for-bit identical, with `enabled` on and
   off.
2. **C++ unit tests:**
   - Material table generator, byte-exact.
   - `lod_opaque_lattice` is the identity on a lattice with no transparent labels.
   - The shell filter emits only transparent-sided quads.
   - The Beer–Lambert, ε-cut and step-cap maths.
   - GPU/CPU LoD parity extended to a chunk containing ice.
3. **gdUnit, through the shipping capture path, not `hooks.cpp`:** an ice slab painted over
   lit ground, streamed at (20,60,30).
   - The pixel through the ice matches the tinted ground behind it.
   - A thicker slab is darker.
   - A slab past the ε distance shows body colour only, and its step count stays under the
     cap.
   - Ground under the ice is not black (tinted sun).
   - Toggling `enabled` off reproduces the opaque render.
4. **Seam:** far-field ice at ~140 m, checked with the existing seam probe.
5. **Cost:** the benchmark leg with an ice patch, interleaved A/B/A with `enabled` on and
   off. The `transparency_composite` stage and raymarch deltas are reported as measured.

## 10. References

- Teardown's deferred screen-door transparency, and why it needs TAA:
  https://acko.net/blog/teardown-frame-teardown/
- Weighted-blended OIT and the deferred + forward hybrid:
  https://learnopengl.com/Guest-Articles/2020/OIT/Weighted-Blended
- Stochastic transparency: https://research.nvidia.com/publication/stochastic-transparency
- Transmittance through voxel media (ESVO):
  https://users.aalto.fi/~laines9/publications/laine2010i3d_paper.pdf
- The author's surface-nets terrain this far-field approach extends:
  https://github.com/WMsans/Unity_SDF_Terrain

## 11. Deviations recorded during planning

1. `composite.frag.glsl` is not edited: `transparency_composite` samples the marcher's
   march-resolution front targets directly, nearest-sampled like composite.frag's geometry.
2. The opaque-lattice pass always runs (identity without transparent labels); the shell
   passes early-out on the job's bit instead of the CPU skipping dispatches.
3. Shell quads share their chunk's pages after the skirts and are told apart by material;
   the opaque and shadow vertex shaders collapse them. No new arena range or quad bit.
4. The shell list is the CPU walk's pages holding shell quads, not HiZ-culled.
5. Grass and leaf sun marches always see through transparency; only the raymarcher's march
   honours `enabled`.
6. With `enabled` off, far-field ice still casts no sun-map shadow.
7. With a front present the composite fetches three texels per pixel — `near_front`,
   `far_front` (conditional, on the far field's bit), and `gb_depth`; it returns without
   shading when the G-buffer depth is nearer than the front. Its measured cost is the
   Step 2 steady-leg delta, "B − mean(A1, A2) = +0.65 ms p50" on a 52 ms frame
   (interleaved A/B/A, `--ice=3` on every leg, 300 sampled frames each, V-Sync disabled,
   `render_scale=0.65`, `near_field_scale=0.40`, and this world's fade band
   `38.40–48.00 m`; p50 ms: A1 52.38, B 53.03, A2 52.38). Two A legs agree exactly
   (0.00 ms p50), but n=2 establishes no noise floor however well the two agree, so the
   +0.65 ms is one measurement plus a replicate, not a demonstrated separation. A prior
   run of the same three legs put the p50 delta at +0.67 ms — consistent, but that run
   is unsourced for a reason: it was invalidated by a bug that orphaned the edit tool
   (`_tool == null`) and so broke the edit / edit-bounded / island legs. Its *steady* leg
   never touches `_tool` and was genuinely unaffected, so its numbers stand for this leg;
   they are named here so a reader does not assume two fully clean runs. The p99 delta
   is **not** a measurement worth quoting: B's p99 is 62.50 ms against a mean A of
   58.38 ms, i.e. +4.13 ms, but the two A legs already differ by 1.89 ms p99 and that
   prior run put the same figure at +0.55 ms. No pass-level GPU numbers exist at all:
   this machine reports `valid_samples=0`, so the `transparency` and
   `transparent_raster` stages were never timed (memory:
   gpu-timings-invalid-on-this-machine).
8. §9's automated seam-probe check became a rendered-frame check, not a hook
   (`tools/transparency_capture.gd`): the existing seam probe marks terrain ownership, not
   front ownership, and a hook that re-derived front ownership would test a copy of the
   logic rather than the shipping pass. What was actually inspected, through
   `demo/main.tscn`:
   - `seam` — a chain of 22 overlapping r=5 m ice balls placed every 6 m along the ground
     from 20 m to 144 m out (about 45 m to 150 m of true range from a lens 45 m up), plus
     three separate r=7 m probe balls at roughly 0.7×, the midpoint and 1.6× the band
     (printed ranges 45 m / 72 m / 128 m). The chain is what crosses the band — marched
     ice in front of it, shell ice behind. The capture world's fade band is
     **64–80 m, not the 38–48 m** band of item 7's benchmark leg (different display
     settings), so "the band" in this frame is not the band that was measured for cost.
     Read at 1× and at 5×. The ice reads as ice on both sides of the band, the ground
     shows through it, the dither rim is the ordinary near/far cross-fade, and no
     double-dark or missing-pixel line was visible.
   - `foliage` — a 4 m ice ball 14 m ahead of a low camera with the shipped grass scatter
     (18 096 blades). The blades draw over the ice untinted, which is §7 step 1's
     G-buffer-depth rule. That rule has **no automated coverage**: the 15 cases in
     `tests/test_transparency.gd` never place ice in front of grass or leaves, and the
     fixture only zeroes grass wind. This frame is the only evidence for it.
   Both PNGs are under `reports/transparency-B/`, which is git-ignored, so they are not in
   the tree; the tool regenerates them. What was **not** done: nobody played the demo with
   the mouse-driven edit tool, and no frame was judged at native resolution in a live
   window. These are rendered stills, inspected by eye at 1× and magnified, and a still
   cannot show a seam that only appears while the camera moves through the band.
