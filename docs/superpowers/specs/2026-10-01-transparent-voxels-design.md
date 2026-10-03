# Voxel Everything — Transparent Voxels (meshed)

**Date:** 2026-10-01
**Status:** implemented
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

## 11. Deviations and measurements recorded during implementation

### 11.1 The plan's deviations, decided while planning

Copied verbatim from `docs/superpowers/plans/2026-10-01-transparent-voxels.md`, section
"Deviations From The Spec". Task 12's job is to record them where a maintainer will find
them; this is that place.

1. **Brick residency and occupancy.** The spec's §3 table only names the atlas lattice. Two
   more consumers read it and are handled explicitly: a brick is resident when the union
   probe **or** the opaque-view probe finds a surface (otherwise ground under ice is never
   generated), and the occupancy grid is classified from the **union** lattice (otherwise
   connectivity would treat ice as air and ice could never hold anything up or become an
   island).
2. **Near-field normal rule.** `terrain_source_normal` falls back to the stored lattice only
   when the union field at the hit is more than 2 cm inside solid **and** a transparent solid
   sits at the hit or one voxel out along the fallback normal. The second clause is what keeps
   no-ice scenes bit-identical.
3. **Island gating lives in the island descriptor.** A descriptor int lane says "apply the
   opaque view to this island". It is set only for islands whose volume holds a transparent
   label, so islands without ice pay nothing.
4. **Island shells are ordinary LoD pool pages.** Their chunk record carries the island slot,
   and the shell vertex shader applies the island's transform from the descriptor buffer. No
   per-island draw call.
5. **The G-buffer resolve is a fullscreen raster pass**, not part of the compute composite: a
   compute shader cannot write a depth attachment. Resolved gloss is a constant `0.9`.
6. **Camera inside a transparent solid.** Pixels with thickness but no front face get tint
   only (no Fresnel, flat body colour), using the material the CPU sampled at the camera.
7. **Shell preconditions.** The near and far shells need the LoD pool and the `MeshService`,
   exactly as the far field does. The pre-existing lazy-init gap in `frame.cpp` (`lod_.pool()`
   gate) is not fixed here. The demo's HUD, benchmark and capture tool already create the
   pool every frame.
8. **Stage names** `shell` and `transparency` are appended after `history` in `FrameStage`, so
   existing stage bit positions do not move.
9. **`sky_thickness_m` is measured from the nearest front.** An unmatched front against the
   sky exits at `front distance + sky_thickness_m`.

Known ceilings added by these decisions, verbatim:

- On a re-stream (not on a fresh edit, which regenerates every touched brick), a transparent
  intrusion narrower than 0.4 m into an otherwise fully solid brick can be missed by the 3³
  residency probe, leaving a small hole in the ground seen through the ice.
- Candidate detection rescans the material bytes of every nearby override brick each time
  candidates are recomputed.

### 11.2 Deviations the plan did not foresee

Found by the implementation reviews and recorded here because the plan's Global Constraints
and File Structure are both wrong about them.

1. **`field_ops.glslh` and `ve::apply_op` WERE edited**, against an explicit Global
   Constraint. The constraint existed because §3's opaque-view rule was meant to be applied
   *after* the merged op stack, at the consumer. That is not what the rule can do: the union
   merge has already destroyed the information the rule needs, so carving a transparent SOLID
   sample after the merge deletes the GROUND an ice op merely overlaps. Measured: a 1 m ice
   sphere added on a meadow left a 0.5 m deep pit, +0.43 m for `OP_SPHERE_ADD` and +0.87 m
   for `OP_SPHERE_PAINT`, measured against the un-edited tree. The fix threads an **opaque
   accumulator** alongside the union one inside the op loop, so a transparent ADD or PAINT op
   contributes nothing to the opaque bake and only a genuinely empty region becomes ice-like
   air. `shaders/field_ops.glslh` gained 42 lines. **The spec's own stated consequence** —
   "Ground seen through ice can sit up to one voxel (5 cm) off the true label boundary" — was
   falsified by an order of magnitude before this fix; it is only true after it, and only
   because of the accumulator.
2. **`shaders/raymarch.comp.glsl`'s island `q_mat` heuristic is a known defect.**
   `q_mat = isl.opaque_view ? q - n_local * (0.75 * isl.voxel) : q` pushes the sample one
   notch *out* of the island along the local normal so `island_material_at` reads the rock
   under the cap. When the fallback normal degenerates to `vec3(0,1,0)` that push can land in
   the transparent cap instead and report the transparent label where the rule says rock.
   Marker written at the call site (`raymarch.comp.glsl`, the island branch). Not fixed.
3. **`ShellRasterPass::front()`, `thickness()` and `front_depth()` return `RID()` when the
   pass did not draw.** The freshness invariant ("never sample last frame's target") is
   therefore enforced by the pass, not left to each caller to honour. Four callers were
   audited; the alternative was a convention every future caller would have to know.
4. **`extension/src/physics/island_body.h` is modified** even though the plan's File Structure
   omits it. The island publish path's reachable record is `IslandSpawn` in that header, not
   the extraction result the plan named, and the shell needs the atlas slot from it.
5. **The island shell needs a wait map** — `orchestrator.h`'s
   `std::map<int, std::pair<int, ve::VolumeData>> island_shell_wait_`. `publish_descriptors()`
   runs BEFORE `queue_island`, so an island's first drain carries the previous occupant's
   default lattice frame. That frame passes the plan's size guard (it is a valid 32³ chunk,
   just the wrong one) and the shell contours in the wrong coordinate frame. An entry waits
   only while a descriptor for its slot has not been published.
6. **The G-buffer's depth is never cleared in the shipping path.**
   `LodRasterPass::clear_targets` is called from `hooks_render.cpp` and `hooks_lod.cpp` and
   from nowhere else; no shipping frame clears `gb.depth`. If this is ever fixed, the fix is
   NOT a `DRAW_CLEAR_DEPTH` in the resolve's draw list — §11.4 ceiling 1 records why that is
   wrong and what the correct pass is. Consequence measured during
   implementation: with SSAO on, two identically built and identically streamed worlds flip
   between exactly two `lit_checksums` (~50% of pairs), and the flip survives with grass
   disabled and with the canopy removed entirely, so grass scatter is not the cause. The
   input is still unidentified. No existing suite is exposed to it because every transparency
   fixture turns SSAO off — which is why the defect is *latent* rather than fixed. It is a
   real defect in the shipping renderer and is filed here, not fixed here: the fix changes
   every production frame and belongs in its own commit.
7. **`demo/benchmark.gd`'s `--transparency=` had to be applied before the world's first
   streamed frame.** `enabled` is read where lattices are baked, so it is set in the arg loop
   in `_ready`, which runs before `VoxelWorld._process` streams a single region. Setting it
   after the first frame would A/B a world that had already baked with the other value.
8. **The gdUnit transparency fixture disables SSAO** (`w.set_effect_enabled("ssao", false)`),
   because of (6). The comment in the fixture records why: `ssao.comp.glsl` is a pure
   function of the G-buffer (fixed `bayer4(px)` rotation, no frame counter, no history), so
   the non-determinism is in the WORLD, not the pass — which is what pointed at the uncleared
   depth.

Deviations made while executing the plan's final task, on top of the eight above:

9. **`tools/transparency_capture.gd` gained three cases.** The plan asked for `seam` and
   `foliage` only; `inside`, `sky` and `linger` were added because §6 step 5, §3's "a camera
   inside ice" and the never-cleared depth are exactly the claims a still *can* falsify, and
   neither of the two planned frames puts a camera inside ice, puts ice against bare sky, or
   changes the world between frames. `linger` is two frames of one pose with and without the
   ice — the fixed-camera exposure the moving-camera case would otherwise need a human for.
10. **The plan's Step 3 command line is under-specified and was corrected before it was run.**
    It reads `tools/run_benchmarks.sh <label> --ice=3 --transparency=N`, but
    `near_field_scale=0.40` — which the prior branch's spec §11 items 7 and 10 record for
    their legs, and which the run log prints — is NOT a shipped default
    (`render_settings.h`: `0.66`; the command adds `--near-scale=0.40`). `render_scale=0.65`
    needed no flag: it is `project.godot`'s `scaling_3d/scale`. Every leg below therefore ran
    `--near-scale=0.40`.
11. **The plan's instruction to strip `min_step_m` / `max_steps` and rename `center_trans` →
    `center_thick` was a no-op.** The checked-out tool is the branch-tip version of the prior
    branch's file, which never had the walker's knobs and never read a centre probe. Nothing
    was deleted; the grep is in the task report.

### 11.3 Cost: interleaved A/B/A, with and without ice

Six `tools/run_benchmarks.sh` runs, sequential (two Godot processes sharing one GPU measure
each other), each contributing its steady leg:

```bash
tools/run_benchmarks.sh trans-ice-A1   --near-scale=0.40 --ice=3 --transparency=0
tools/run_benchmarks.sh trans-ice-B    --near-scale=0.40 --ice=3 --transparency=1
tools/run_benchmarks.sh trans-ice-A2   --near-scale=0.40 --ice=3 --transparency=0
tools/run_benchmarks.sh trans-noice-A1 --near-scale=0.40 --transparency=0
tools/run_benchmarks.sh trans-noice-B  --near-scale=0.40 --transparency=1
tools/run_benchmarks.sh trans-noice-A2 --near-scale=0.40 --transparency=0
```

`run_benchmarks.sh` supplies `--resolution 2560x1440 --disable-vsync` and 300 sampled frames
after a settle cap. Every leg printed `BENCH camera ... render_scale=0.65
near_field_scale=0.40 quality_tier=3 fade_band_start=38.40 fade_band_end=48.00` — the same
band the prior branch's spec records for its benchmark legs, so the two sets are comparable
in setup — and `BENCH timing_condition ... vsync_actual=disabled`. The three ice legs each
printed `benchmark: ice r=3.0 at (28.51491, 56.37516, 28.51491)`; the three no-ice legs
printed none. All six report the same world: `draw_pages_p50=341`,
`chunks_resident_p50=709`, `culled_ratio_p50=0.314` in every leg.

**With a transparent patch (`--ice=3` on all three legs, an r=3 m ball ~9 m ahead of the
lens):**

| leg | `--transparency` | p50 ms | p95 ms | p99 ms | frame_avg ms | max ms |
|---|---|---|---|---|---|---|
| A1 | 0 | 21.67 | 22.22 | 22.96 | 21.59 | 26.20 |
| B  | 1 | 21.43 | 22.22 | 22.75 | 21.54 | 23.40 |
| A2 | 0 | 21.67 | 22.22 | 22.22 | 21.67 | 22.29 |

**B − mean(A1, A2) = −0.24 ms p50** (mean A = 21.67, B = 21.43), on a 21.7 ms frame. **The
A/A spread is 0.00 ms** — the two A legs report the same p50, p95 and frame_avg to the
printed precision. So the feature measured *negative*, i.e. faster, which is not a result;
it is a delta smaller than two legs can establish. The p95 is an exact tie (all three legs
22.22). The p99 delta is **+0.16 ms** with an A/A spread of **0.74 ms**, so the p99 is **not
quotable**: the two A legs disagree by more than the effect.

**No transparent material in the scene (no `--ice` on any leg):**

| leg | `--transparency` | p50 ms | p95 ms | p99 ms | frame_avg ms | max ms |
|---|---|---|---|---|---|---|
| A1 | 0 | 20.37 | 20.83 | 21.20 | 20.54 | 22.03 |
| B  | 1 | 20.37 | 20.83 | 21.31 | 20.42 | 22.22 |
| A2 | 0 | 20.37 | 20.83 | 20.83 | 20.45 | 21.84 |

**B − mean(A1, A2) = 0.00 ms p50**, A/A spread **0.00 ms** — a tie on p50 and on p95, three
legs landing on the same number. The p99 delta is **+0.30 ms** against an A/A spread of
**0.37 ms**, so the p99 is **not quotable** here either. The legs' maximums differ (22.03 /
22.22 / 21.84), so they did not land on the same frame.

**What the feature costs when there is nothing to see: nothing measurable, and this run
shows it on the page budget rather than only on the clock.** `pages_used_p50` is
**2287 / 2324 / 2287** across the three ice legs and **2286 / 2286 / 2286** across the three
no-ice legs. In the no-ice world the feature on and the feature off allocate *the same number
of pool pages to the last page*, which is §1's "zero cost without transparency" measured
directly: the shell build finds no candidates, the shell raster's `drew()` gate keeps it out,
and the composite returns without a front. In the ice world B holds exactly **37 more** pages
than either A leg — the near-shell chunks for one r=3 m ball — which is also the cleanest
available proof that `--transparency=0` really did disable the feature on the B leg's A legs
and that nothing else differed between them.

**Caveats, stated plainly.**

- **No pass-level GPU numbers exist.** Every leg printed
  `BENCH gpu_timing valid_samples=0 dropped_pairs=0` and every `BENCH gpu_*` stage reported
  `samples=0`. The `shell` and `transparency` stages were never timed
  (memory: `gpu-timings-invalid-on-this-machine`). Cost here is frame time only.
- **n = 2 establishes no noise floor however well the two A legs agree.** A 0.00 ms A/A
  spread is this run's resolution, not a demonstrated bound on run-to-run variation. Both
  deltas (−0.24 ms, 0.00 ms) should be read as "a few tenths of a millisecond at most, if
  anything".
- The two sets are not comparable to each other as a cost of the ice itself: the ice legs run
  ~1.3 ms slower than the no-ice legs because the ball replaces terrain colliders and adds
  shell pages. Only the within-set A/B/A deltas are the feature's cost.
- The world is not perfectly identical between the three ice legs: `BENCH max_ms` reports
  `phys_tris` 5277 on A1 against 4257 on B and A2, so A1's worst frame landed on a different
  collider mesh. Every other reported statistic matches exactly.

### 11.4 What the visual check actually inspected

`tools/transparency_capture.gd` renders the SHIPPING scene (`demo/main.tscn`) and writes PNGs
to `reports/transparency-B/`, which is git-ignored; the tool regenerates them. All six PNGs
were opened and looked at, at 1× and at 5× nearest-neighbour crops. Every claim below is a
statement about a still, at 1600×900, with a fixed camera.

| case | what it is for | what was seen |
|---|---|---|
| `seam` | spec §9 item 4: one ice ridge crossing the fade band | 22 overlapping r=5 m balls every 6 m from 20 m to 146 m plus three r=7 m probes at 45 m / 72 m / 128 m, camera 45 m up, fade band **64–80 m** in this world (it is the streaming reach at this camera pose, not the benchmark world's 38–48 m). Ice reads as ice on both sides of the band: on the near side the ground and grass show through the balls tinted blue-green; past the band the same chain continues to the horizon with a cooler blue-grey cast and a visibly coarser silhouette (far-field cells). `shell_pages=577`, so the near shell is live. **No double-dark band and no missing-pixel line** were visible at 1× or 5×. The only discontinuity visible in the frame is the ordinary 4×4 bayer dither on the ice surface and the far field's own LOD stair-stepping, both present with and without the feature. |
| `foliage` | grass in front of ice must draw over it untinted | r=4 m ball 14 m ahead, 19 384 blades. **The rule holds and is visible at 1×**: blades standing between the lens and the ice are fully saturated green and un-tinted, while blades inside the ice's silhouette are visibly paler and greyer. The ice against the sky is a lighter, hazier blue than the sky around it — tinted, not a hole. A thin dark outline rings the silhouette, which is §6 step 5's resolve reaching the outline pass. This rule has **no automated coverage**: the 25 cases in `tests/test_transparency.gd` never place ice in front of foliage. |
| `inside` | §6's camera-inside case | lens 1.5 m above the centre of an r=6 m ball, so most pixels have thickness and no front. The world renders, finite, **not black**, and measurably tinted: sky reads `(119,148,196)` through the ice against `(152,179,224)` untinted in the same world at the same pose — per-channel ratios 0.78 / 0.83 / 0.88, a Beer–Lambert tint of a few metres of ice. |
| `sky` | ice against bare sky, past the band | one r=7 m ball floating 45 m up at 119 m, `shell_pages=0` so the **far** shell owns it and it is genuinely drawn. Inside the silhouette the sky is `(132,146,183)` at mid-chord (darker and bluer than the surrounding `(159,179,216)`) and `(188,199,219)` at the grazing top (the Fresnel cap). Tinted sky, not a hole, at distance. |
| `linger` / `linger_after` | the never-cleared `gb.depth` | one frame with a chain of r=6 m balls from 14 m to 102 m, then every ball subtracted and the world re-settled, same camera. **No ghost.** Measured, not eyeballed: at screen row y=200 the ice reads `(200..207, 201..217, 222..234)` over x = 680..920 and `linger_after` reads `(158,179,217..218)` there — the same value as x=560 and x=1000, which were never inside the ice. No residual tint, no residual Fresnel cap, no residual outline, including across the part of the ball that stood against bare sky, which is the case where nothing re-writes `gb.depth`. `shell_pages` went 621 → 0, so the ice really was gone. |

**Known ceilings, visited one by one.**

1. **The resolve is a writer on a never-cleared `gb.depth` (§11.2 item 6).** Code-verified:
   `LodRasterPass::clear_targets` is called from `hooks_render.cpp` and `hooks_lod.cpp` only.
   Exposure taken: `linger` / `linger_after`, the fixed-camera form of the moving-camera case.
   **Not visible.** No phantom ice silhouette, tint, reflection or outline survived a settled
   frame with the ice removed. The reading is that the ceiling is real for the G-buffer's
   *contents* but not, in this scene, for the final image: the composite contributes nothing
   without a front, and the terrain that was behind the ice re-writes `gb.depth` every frame
   anyway. What this does **not** establish: whether a *moving* camera, a silhouette that
   sweeps across previously empty sky, or a scene where the only geometry behind the ice was
   also removed, leaves one. That needs frames this tool does not take, and it is still a
   latent defect — the SSAO two-checksum flip in §11.2 item 6 is the same missing clear seen
   from the other side.

   **The mitigation recorded for this ceiling earlier in this section was WRONG, and is
   retracted: do NOT put a `DRAW_CLEAR_DEPTH` in the resolve's own draw list.** That draw
   list's depth attachment is `gb.depth` itself, so the clear would set EVERY pixel to
   reverse-Z far before the resolve wrote back the handful of ice fronts. `inject`, contact
   shadows and SSR all read `gb.depth()` / `gb.surface()` after the resolve, so every
   Godot-drawn scene object, outline and reflection would draw over the ice, SSR and outlines
   would lose all opaque depth, and contact shadows would disappear entirely. It trades one
   latent one-pixel staleness for a total loss of the G-buffer's depth. The correct shape, if
   this is ever picked up, is a SECOND fullscreen pass that writes far depth ONLY where last
   frame's `front.w > 0.5` and this frame's is `< 0.5` — `COMPARE_OP_ALWAYS`,
   `gl_FragDepth = 0.0`, colour writes off — so the pixels that lost their ice are returned
   to far and every other pixel is left exactly as the frame left it. That pass is **not**
   implemented here; it is separate work.
2. **Hi-Z never sees the resolved depth, so gloss-0.9 ice is SSR-eligible against a depth
   that has no ice in it.** **Not visible in any capture, and honestly not decidable by one.**
   The only bright reflection in any frame is the Fresnel cap on the `sky` ball, and that ball
   is ringed by sky in every direction, so "the ray passed through the ice" and "the ray hit
   the sky behind it" produce the same pixels. Separating them needs a reflective surface
   placed *behind* the ice, which is a scene this tool does not build.
3. **Island shell pages sat unconditionally on the NEAR side of the fade dither, so an island
   beyond the shell fade distance rendered with no medium at all.** **Fixed.** The ceiling was
   recorded as unverified because there is no island in any of the six captured worlds; it is
   now decided by code review plus an automated case.
   `test_a_floating_island_with_an_ice_cap_is_see_through_and_its_shell_draws_the_ice` uses
   the camera pose every test camera uses — close — so the far case was uncovered. The defect:
   `island_shell_flags` sets chunk flag bit 0, `shell.vert.glsl` turned that into `v_near = 1`,
   and both shell fragment shaders then kept the island page only where `bayer4 >= t_fade`.
   Past `fade_end` `t_fade` is 1 and `bayer4` tops out at 15/16, so the cap discarded
   everywhere. It is a HOLE, not a missing tint: `march_island` is bounded by its own AABB and
   `best.t`, never by the camera's `max_dist`, and `island_lattice` applies the opaque view
   unconditionally, so the marcher still drew the lump's opaque part at full opacity and saw
   air where the ice is — nothing was left to fill the gap. Fix: an island page gets its own
   partition, keeping every fragment at `d >= fade_end` (`shell_front.frag.glsl`,
   `shell_thickness.frag.glsl`, via a `v_island` varying). Inside the band it is unchanged, so
   the complementary partition with the far field still holds and the counts stay exact.
   `test_an_island_past_the_fade_band_still_draws_its_ice_cap` places the same fixture 38 m
   from the camera in a world whose fade band ends at 32 m.
4. **The seam.** See the `seam` row: reads as ice on both sides of the band, no double-dark
   band, no missing-pixel line, at 1× and 5×. **A still cannot show a seam that only appears
   in motion**, and this branch's seam is a dither cross-fade between two fragments of the
   same surface, which is exactly the kind of thing that can read clean in one frame and
   flicker as the camera moves through it. Nobody drove the demo with the mouse; no frame was
   judged in a live window at native resolution. Every camera here is fixed.
5. **Foliage in front of ice.** See the `foliage` row: holds, and visible at 1×. Grass
   *behind* the ice is visibly tinted. The reverse case — blades poking out through *thin*
   ice, which §3 lists as a consequence — is in the `foliage` frame (blades at the top of the
   ball are greener than the ones inside it) and reads as intended.
6. **Camera inside ice, and ice against sky.** Both demonstrated and measured: `inside` and
   `sky` above. Ten metres of ice darker than one metre is not directly demonstrated by a
   still — but it is the automated `test_transparency.gd` thickness pair, and the `sky` ball's
   own profile (Fresnel-bright at the grazing top, Beer–Lambert-dark at mid-chord, sky visible
   at both) is the same statement in one frame.

**Not done, and said so.** No island capture. No moving-camera capture. No live-window
inspection at native resolution. No per-pass GPU numbers, because the machine reports
`valid_samples=0`. No golden was re-recorded.

### 11.5 Verification at the end of the branch

`./build.sh --test`: `Status: SUCCESS!` — 755 test cases, 9 319 632 assertions, 0 failed.

gdUnit, `./gdunit_tests.sh`, 98/98 suites, 22min30s: `547 test cases | 1 errors | 5
failures`. The failing set is **equal to** the baseline Task 0 recorded on clean `main` and
nothing new: `test_edit_pipeline.gd`, `test_ssao_golden.gd`, `test_lod_seam.gd`,
`test_sun_cascades_gpu.gd`, `test_voxel_settings.gd`. `test_frame_shipped_golden.gd` passed
and was not re-recorded.

One suite, `test_island_body.gd`, failed in an EARLIER full run of this same tree and not in
the final one. Investigated rather than assumed: it passes on the stashed clean tree, four
times in isolation with the change applied, and 5/5 in the second full run. The branch's only
change under `extension/src/physics/` is an inert `bool transparent = false;` in
`IslandSpawn`; the suite never renders a frame, so nothing in this branch executes inside it;
and with no transparent label in the world the opaque-view rule and the new opaque accumulator
are both identities. It is a load-sensitive suite, recorded here as such rather than written
off.
