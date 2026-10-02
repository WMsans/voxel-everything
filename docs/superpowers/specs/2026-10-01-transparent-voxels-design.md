# Voxel Everything — Transparent Voxels (meshed)

**Date:** 2026-10-01
**Status:** design approved in conversation, awaiting written-spec review
**Start commit:** `4211732` (branch `feat/transparent-meshed`, worktree `.worktrees/transparent-meshed`)
**Prior attempt:** `feat/transparent-materials` (worktree `.worktrees/transparent-materials`),
spec `2026-09-29-transparent-materials-design.md` on that branch. It is left untouched.
**Prior specs:** `2026-08-17-m5-lod-design.md` (the far-field pipeline the shell extends);
`2026-09-11-stylized-grass-design.md` (the own-module pattern this follows).

Ice becomes see-through. Transparent materials are never raymarched: they are always meshed
and rasterized, and what lies behind them comes from the opaque render's depth.

---

## 1. Why the prior attempt failed, and what changes

Ice is not its own shape. It is a material label on the single union SDF, so the brick
atlas holds no surface between ice and the ground behind it. The prior attempt walked
through the ice in steps of at least 5 cm, capped at 48 steps (about 2.4 m), and painted
the ice's body colour when the cap was hit. Thick ice therefore looked solid inside.

This design removes the walk. The marcher is given a world in which ice is air, and the ice
itself is drawn as a mesh whose thickness is measured by rasterization.

## 2. Goals and non-goals

Goals:

- **Generic transparency.** A property of a material row. No shader or pass names "ice".
- **Tinted, never opaque.** Absorption deepens with thickness but is floored, so what is
  behind is visible through any amount of ice. Fresnel sky reflection on the surface.
- **Never raymarched.** No step budget depends on ice.
- **Near field, far field and islands** all render transparency.
- **Outlines and SSR** treat the ice front as a surface.
- **Own module.** The deferred / beauty stack is not edited; the G-buffer is the seam.

Non-goals, decided explicitly:

- **Refraction.**
- **Shadows from ice.** Ice casts no sun shadow; ground under it is fully sunlit.
- **Per-layer shading.** Only the nearest front gets Fresnel and body shading; deeper
  layers contribute thickness only.
- **A live on/off toggle.** `enabled` is read when the world streams in (§8).
- **An exact opaque field.** The label rule (§3) is used everywhere. An evaluator that
  carries a second distance through the op list could later replace it behind the same
  function.
- **An in-frame GPU shell.** The near shell is built on the worker thread and lags an edit
  by a few frames (§5).

## 3. The opaque view

One rule, one function: `opaque_view(sdf, mat)` in GLSL (`shaders/opaque_view.glslh`) with
the C++ mirror `ve::opaque_view`. A solid sample (`sdf <= 0`) whose material is transparent
becomes "just outside": `sdf = +½ sample pitch`, `mat = 0`. Every other sample is
unchanged. The field evaluator (`field_ops.glslh`, `ve::apply_op`) is not edited.

| Consumer | How the rule is applied |
|---|---|
| Brick atlas: `brick_gen.comp.glsl`, `ve::eval_brick` | After every `eval_field`: phase 1a, the apron in 1b, and the material projection in phase 2. Mips and brick flags derive from what was written, so a brick with no opaque surface is skipped by the DDA. |
| Far field | `lod_opaque.comp.glsl` and `ve::lod_opaque_lattice`, ported from the prior branch. They are this rule at the level's cell size. |
| Islands | Per sample inside `march_island` (`island_lattice` reads the material byte beside the SDF byte), because island bytes are the shared authoritative volume that physics and merge-back read and cannot be rewritten. The island min-max mip (`IslandAtlas::upload_mip`) is built from the opaque view. |
| Near-field normals | `terrain_source_normal` differentiates the union field, which is wrong under ice. When the union sample at the hit is a transparent solid it returns `terrain_r8_fallback_normal`. The island normal does the same with `island_r8_fallback_normal`. |

Consumers that keep seeing ice as solid, because they evaluate the field and never read the
atlas: colliders (`MeshService`), the CPU edit raycast (`world/raycast.cpp`), connectivity
and island extraction, consolidation.

Consequences:

- Ground seen through ice can sit up to one voxel (5 cm) off the true label boundary, with
  normals from the stored lattice.
- Grass and leaf scatter read the atlas, so grass grows on ground under ice and shows
  through it; blades taller than thin ice poke out.

## 4. Material data

Ported from the prior branch (`b02c407`). `ve::MaterialDef` gains:

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
`bool mat_transparent(uint id)`, range-checked; foliage and air are opaque. The byte-exact
generator test is re-recorded.

## 5. The shell meshes

The shell is the set of surface quads whose solid side is transparent: the air↔ice
boundary only. The ice↔opaque interface is not meshed; opaque depth supplies it. All three
sources emit `ve::LodQuad`, so one raster shader draws them.

**Far field** (ported: `caa2a5c`, `d75c0fb`, `744b7e3`, `aa53882`, `ff1c2c7`).
`lod_reduce` raises a per-job "has transparent" bit. A second `lod_quads` run in shell mode
keeps only transparent-sided quads. Shell quads share their chunk's pages after the skirts
and are told apart by material; the opaque and shadow vertex shaders collapse them.

**Near field** (new).

- **Grid.** Fixed-level shell chunks of 32 cells at 0.1 m, 3.2 m per chunk. The builder's
  half-cell fine samples land on the 5 cm voxel pitch.
- **Builder.** `LodBuildPass` in shell-only mode at that cell size, on the worker thread.
  No new contour code.
- **Candidates.** `ve::shell_candidates` returns the chunks, within the fade band's end
  plus one chunk, that overlap any of: the AABB of an op with a transparent material; a
  stored volume whose material bytes hold a transparent id; an override brick whose
  material bytes hold a transparent id. A candidate that builds zero quads is recorded
  empty and holds no page.
- **Invalidation.** Edits dirty shell chunks through `ve::InvalidationSink`, as they dirty
  LoD chunks. Shell jobs queue behind colliders and ahead of far LoD builds.
- **Storage.** Pages in the existing LoD pool with their own draw list. Chunks are evicted
  when they leave the radius.
- **Seam.** Over the fade band a far shell fragment survives where `lod.frag.glsl` keeps a
  terrain fragment (`bayer4 < t_fade`), and a near shell fragment survives on the
  complement, where `composite.frag.glsl` keeps the near field. Each face therefore
  survives in exactly one shell, which keeps §6's face counts exact.

**Islands** (new). At extraction the CPU holds the island's `VolumeData`. It contours the
shell once with `ve::lod_contour` plus the shell filter, split into 32-cell blocks. The
quads are uploaded once, drawn each frame with the body's transform as a push constant, and
freed with the island. On merge-back the island becomes a volume op, which dirties the
terrain shell chunks.

Known ceilings:

- Candidate detection covers ops, volumes and overrides. A terrain stage that generated a
  transparent material would be missed in the near field. No stage does today.
- The shell lags an edit by a few frames. Painting ground into ice shows a hole for those
  frames, because the atlas updates in-frame.

## 6. Rendering

```
raymarch → composite → LoD → grass → leaves
  → shell thickness → shell front        (new, raster)
  → ssgi → ssao → deferred
  → transparency composite               (new, compute; also resolves the G-buffer)
  → inject → contact → ssr → outlines
```

**Shell thickness** (`render/shell_thickness_pass.*`). Every shell face, no culling,
depth-tested against G-buffer depth, no depth write, additive blend into an `RG32F` target.
A front face adds `(−z, +1)`; a back face adds `(+z, −1)`, where `z` is view distance.

```
thickness = max(R + G · z_opaque, 0)
```

`G` counts entries with no matching exit: rays that end on opaque ground inside the ice.
The result is exact for any thickness and any number of layers and is order-independent.
`ve::shell_thickness(R, G, z_opaque, camera_inside)` is the CPU reference.

- Camera inside ice: the CPU evaluates the field at the camera each frame; when it is a
  transparent solid the pass's clear value is `(0, +1)`, a virtual front at `z = 0`.
- Sky behind (`z_opaque` absent) with `G > 0`: thickness is capped at `sky_thickness_m`.

**Shell front** (`render/shell_front_pass.*`, from the prior `TransparentRasterPass`).
Front faces only, own depth attachment, discards fragments behind G-buffer depth. The
nearest front writes `front` (xy octahedral normal, z distance, w material id; id 0 = none).

**Transparency composite** (`render/transparency_composite_pass.*`, from the prior branch).
A compute pass rewriting `lit` in place. Per pixel with a front:

1. `T = max(pow(transmit, thickness), min_transmit)` per channel.
2. Body: `cel_shade` on `material_surface(mat, front_pos, n)` with sky ambient and
   `sun_map_visibility` (`sun_map.glslh`, ported: `bd035a5`).
3. Fresnel: `F0 = ((ior−1)/(ior+1))²`, Schlick; reflection is `sky_color(reflect(−v, n))`.
4. `lit = F·sky + (1−F)·(T·behind + (1−T)·body)`.
5. **Resolve:** write the front's normal, material id and gloss into `gb.surface` and the
   front's depth into `gb.depth`.

Step 5 is what makes outlines, SSR and contact shadows treat ice as an ordinary glossy
surface with no edits to their shaders. `inject` then copies the ice depth into the scene
depth, so Godot-drawn scene objects behind ice are hidden by it.

Known ceilings:

- Ice fronts take sun from the sun map even near the camera, which is coarser than the
  marched shadows on terrain beside them.
- Two raster passes over the shell; one needs depth write and the other additive blending.
- SSGI and SSAO run before the composite and see the surface behind.
- Godot-drawn objects behind ice are hidden rather than seen through it.

## 7. Islands in the marcher

Covered by §3: the island march applies the rule per sample, which costs one extra byte
read per lattice corner on island pixels only. The island's opaque part renders as today;
its shell renders through §5 and §6.

## 8. Module, settings, failure handling

- **Module:** `extension/src/transparency/` — `TransparencySettings`, its store (ported:
  `07122ab`, walker fields removed), `shell_candidates`, the shell targets.
- **Integration:** a few lines in `VoxelFrame::render_pre_opaque` and the pass registry.
  `BeautySettings`, `deferred.comp.glsl`, `shade.glslh`, `outline.comp.glsl` and
  `ssr.comp.glsl` are not edited.

| setting | default | meaning |
|---|---|---|
| `enabled` | true | Read when the world streams in. Off: `opaque_view` is the identity, no shell is built or drawn, and ice renders opaque as on `main`. The benchmark A/B switch. |
| `min_transmit` | 0.35 | The transmittance floor. |
| `sky_thickness_m` | 4.0 | Thickness assumed for an unmatched front against the sky. |

**Failure handling.** A failed shell or composite pass cancels its stage and the frame
continues with no ice that frame. A failed shell chunk build is retried like a LoD build.
The frame is never aborted for transparency.

## 9. Testing

Characterization first.

1. **Pins before any edit.** Existing near and far goldens stay bit-identical for scenes
   with no transparent material, with `enabled` on and off. `test_frame_shipped_golden.gd`
   is known flaky and stays informational.
2. **C++ unit tests:**
   - Material table generator, byte-exact.
   - `opaque_view`; `ve::eval_brick` against `brick_gen` on a brick containing ice.
   - `lod_opaque_lattice` is the identity without transparent labels; the shell filter
     emits only transparent-sided quads.
   - `shell_thickness`: single slab, two layers, a ray ending on ground, camera inside,
     sky cap.
   - `shell_candidates` from ops, volumes and overrides.
3. **gdUnit, through the shipping capture path, not `hooks.cpp`,** streamed at (20,60,30):
   - The pixel through a slab is the tinted ground behind it.
   - A thicker slab is darker but never below `min_transmit`.
   - A 10 m ice block still shows the ground behind it. This is the regression test for
     the prior attempt's failure.
   - Camera inside ice.
   - A falling island containing ice shows what is behind it.
   - The ice silhouette gets an outline.
   - `enabled` off reproduces the opaque render.
4. **Seam:** a rendered-frame check of ice crossing the fade band, as the prior branch's
   `tools/transparency_capture.gd` did.
5. **Cost:** interleaved A/B/A frame time with and without ice. GPU pass timings are
   invalid on this machine, so only frame time is reported.

## 10. Plan phasing

1. **Core:** material data, opaque view (atlas and far field), far and near shells,
   thickness, front, composite without resolve.
2. **Islands:** per-sample rule, island mip, island shell.
3. **Post passes:** the G-buffer resolve; outline and SSR tests.
