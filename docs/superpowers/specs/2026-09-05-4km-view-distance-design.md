# 4 km View Distance — Design

**Date:** 2026-09-05
**Status:** Approved design, pre-implementation
**Scope:** Sub-project B of three. Raises `stream_radius_m` from 1638.4 m to 4000 m as the
shipped default, splits the sun shadow map into three camera-centred cascades, gives the
demo terrain relief worth looking at at that range, and measures what the radius costs.
**Depends on:** sub-project A (`2026-09-04-unbounded-world-design.md`), merged. A made the
radius expressible and correct; B makes it the default and pays for it.
**Supersedes:** the `stream_radius_m = 1638.4` default and the single-map sun ortho fit
recorded in A §5. A's "known consequence" — one `SunShadowPass::kSize` map stretched over
8 km — is what §2 below closes.

---

## 1. The problem

A shipped a world with no edge and a `stream_radius_m` knob that accepts 4000 and works. It
also recorded, honestly, that the frame budget at that radius was not its subject. Four
things stand between the knob and a 4 km horizon anyone would want to look at.

**The sun map does not scale.** `VoxelWorld::sun_ortho()` fits one 2048² map to a sphere of
`stream_radius_m` around the camera. At 1638.4 m that is a 1.60 m texel. At 4000 m it is
3.91 m, and every shadow in the scene — including the ones ten metres past the near/far
seam — is quantised to it.

**The far field ends abruptly, and the terrain has nothing to show there.** `hills()` is
±10 m of ripple on ~200 m wavelengths. At 4000 m, with the demo's 1592×857 internal buffer
and 75° vertical fov (focal ≈ 558 px), a 10 m hill subtends **1.4 px**. The 1638→4000 m
annulus is a flat sliver on the horizon line: not something a capture can show, and not
something a benchmark can honestly measure.

**Nobody knows what it costs.** The `reports/unbounded-after` legs are the only measurement,
they were taken at 1638.4 m, and their GPU verdicts are all `UNMEASURED` because Metal
returned `samples=0`. Every number below that is not measured is labelled as a projection.

**One budget is a compile-time private constant.** `LodPool::kChunkRecords` is 8192, is not
a parameter of `initialize`, and therefore cannot be driven small by a test. Its exhaustion
path has never executed.

### What is *not* the problem

An early reading of this work projected `chunks_resident` by surface area — 2.44× radius,
5.96× area — and predicted a cliff at `kChunkRecords`. That is wrong, and the correction
shapes the whole sub-project: **LoD chunk count grows roughly logarithmically with radius,
not with area.** Working it through the actual threshold, a chunk descends while its
projected area exceeds `kLodSseAreaThresh` = (32 × 3)² = 9216 px², so level *L* is the
coarsest acceptable level at

```
d >= s_L * f / 96 = (12.8 * 2^L) * 558.4 / 96 = 74.5 * 2^L metres
```

| level | chunk size | used from |
|---:|---:|---:|
| 0 | 12.8 m | 74.5 m |
| 3 | 102.4 m | 595.7 m |
| 4 | 204.8 m | 1191.4 m |
| 5 | 409.6 m | 2382.8 m |
| 6 | 819.2 m | 4765.5 m |

The 1638→4000 m annulus is therefore level 4 out to 2383 m and level 5 beyond it: roughly
**+600 chunks and +2000 pages**, landing near 1900 of 8192 chunk records and 6400 of 32768
pages. Neither budget is the binding constraint. What actually grows is root count (27
candidates → 216, ~113 kept), air-discovery builds, and the frustum-free shadow cut. B is
therefore a cascade, throughput and measurement project, not a budget-raising one — and §5
still measures rather than trusting the projection above.

## 2. Sun shadow cascades

Three 2048² `D32_SFLOAT` layers of one array texture (~50.3 MB), one framebuffer per layer
through `texture_create_shared_from_slice`, and per-layer `dirty_` / `frames_since_` /
`view_proj_` inside a single `SunShadowPass`. One pass keeps the fit written down in one
place, which is the property `VoxelWorld::sun_ortho()` exists to hold: the debug facade
reads the same function the render path does, so what the tests pin is what ships.

### The radii are derived

Cascade 0's texel is set equal to `kLodBaseCell`. There is no point resolving a shadow finer
than the finest geometry that can cast it. The outermost cascade is the stream radius,
because that is exactly the set the shadow cut draws. The middle is the geometric mean, so
texel size steps by a constant ratio rather than by a chosen constant:

```
r0 = kLodBaseCell * (kSize - 1) / 2   = 0.4 * 2047 / 2 = 409.4 m
rN = stream_radius_m
r1 = sqrt(r0 * rN)
texel_i = 2 * r_i / (kSize - 1)
```

At the new default of 4000 m:

| | radius | texel | cut descends to |
|---|---:|---:|---:|
| cascade 0 | 409.4 m | 0.400 m | level 0 |
| cascade 1 | 1279.7 m | 1.250 m | level 1 |
| cascade 2 | 4000.0 m | 3.908 m | level 3 |

Two properties fall out of that derivation, and they are what make the change safe rather
than merely plausible:

1. **Cascade 2 is, at every radius, exactly what ships today.** It is the same
   `sun_ortho_sphere(dir, cam, stream_radius_m, kSize)` call with the same arguments. At
   R = 1638.4 the table reads 409.4 / 819.0 / 1638.4 m with texels 0.400 / 0.800 / 1.601 m,
   and cascade 2 reproduces today's map bit for bit. This is testable and §8 tests it.
2. **`stream_radius_m <= r0` collapses the set to one cascade**, which is today's behaviour
   with no cascade machinery in the path at all. The degenerate case is the old case.

### Selection is a scalar compare

The fits are camera-centred *spheres*, not frustum slices. A point at distance `d` from the
camera is inside cascade *i* if and only if `d < r_i`, so per-pixel selection is

```glsl
int c = d < sun.splits.x ? 0 : (d < sun.splits.y ? 1 : 2);
```

with no depth-slice arithmetic and no split-plane seam to reconcile against the projection.
This is a direct dividend of A's sphere fit; a cube or frustum fit would not have it.

`SunBlock` grows from `mat4 view_proj; vec4 params;` to `mat4 view_proj[3]; vec4 params[3];
vec4 splits;`, and `sun_map` becomes a `sampler2DArray`. The existing bias in
`deferred.comp.glsl` is already texel-relative (`params.x / params.y` scaled by
`1.5 + 2.0 * slope`), which is the correct form for cascades and becomes per-cascade with no
change of kind. The out-of-map early-out (`uv` outside [0,1] returns 1.0) is what makes
"beyond cascade 2" mean "unshadowed" with no extra branch.

### Cascade boundaries are a hard switch

Shadow sharpness steps visibly at 409 m and 1280 m. Blending the boundary with the existing
`bayer4` dither is roughly four lines and is **deliberately not in scope**; it is recorded in
§9 alongside the horizon fade so that the omission is a decision rather than an oversight.

### Rebuild cost is amortised by the existing rule

`SunShadowPass::build` already refuses work unless `force || projection_moved || (dirty_ &&
frames_since_ >= kMinFrames)`. Because each cascade snaps its light-space origin to its own
texel, cascade 0 re-projects roughly every 0.4 m of camera travel, cascade 1 every 1.25 m and
cascade 2 every 3.9 m. The expensive cascade rebuilds rarely and the cheap one rebuilds often,
with no new throttle invented: the amortisation is the texel snap A already shipped.

## 3. The per-cascade LoD cut

`LodWalkResult::shadow_draws` — one frustum-free cut at `stream_radius_m`, produced as a side
effect of `walk()` — is replaced by an explicit query:

```cpp
void LodTree::shadow_cut(const LodCamera &cam, float radius, int min_level,
                         std::vector<LodDrawItem> *out) const;
```

`walk()` stops producing a shadow cut; `shadow_visit` becomes the private recursion behind
`shadow_cut`. `LodSystem::prepare_shadow_raster(int cascade)` runs one cut per cascade while
holding `lod_mutex_`, and the compositor loops the cascades. Splitting the query out of
`walk()` is what lets a cut be skipped (below) and lets a test ask for one cut in one line.

### `min_level`: the one relaxed invariant

Cascade 2's texel is 3.908 m. Descending its cut to level 0 — 12.8 m chunks, three texels
across — is work whose result cannot be resolved, and `kLodNearDenseRadiusM` (300 m) *forces*
exactly that inside the camera's near band. `min_level` stops descent once
`lod_cell_size(level) <= texel_world`, which gives level 0 / 1 / 3 for the three cascades.

**Cascade 0 is unclamped by construction, not by evaluating that rule at its boundary.** Its
texel is `2 * (kLodBaseCell * (kSize - 1) / 2) / (kSize - 1)`, which is `kLodBaseCell`
exactly in real arithmetic and therefore lands on `lod_cell_size(0) <= texel_world` as an
equality that float rounding can decide either way. `min_level` for cascade 0 is hard-wired
to 0; the rule is evaluated only for cascades 1 and above.

This deliberately relaxes the warning at `lod_tree.cpp:302`, and the relaxation must be
understood before it is implemented. That comment says the camera walk and the shadow cut
must share one descend rule so the two "can never choose different levels for the same
ground". The failure it prevents is **two descriptions of one piece of ground inside one
shadow map**, which is what feeding every resident page produced: coarse ancestors and their
own finer children rasterised together, disagreeing by metres, with whichever survived the
depth test shadowing open sunlit ground. A cut clamped at `min_level` is still a *cut* — one
description per piece of ground — so that failure cannot return.

What the clamp does risk is different: the camera's fine surface and the shadow map's coarse
surface disagree, which shows as peter-panning on the far field. The disagreement is bounded
by the level's own tent-filtered deviation, which at level 3 is a few metres against a
3.908 m texel — roughly one texel, which the existing texel-relative bias already spans.

The decision: **implemented, enabled for cascades 1 and 2, cascade 0 unclamped**, behind the
`sun_cascade_min_level` knob so the A/B is measurable, with a peter-panning test (§8) that
fails if the argument above is wrong.

### Only cut for cascades that will rebuild

`SunShadowPass`'s early-out condition is split into a query:

```cpp
bool SunShadowPass::needs_rebuild(int cascade, const ve::SunOrtho &ortho) const;
```

The compositor calls it before building the cut, so a cascade that will not rasterise this
frame does not pay for its cut either. Cascade 2 — the large one — is skipped on most frames.
`build()` keeps the same test internally; the query must not be a second copy of the rule.

## 4. Build throughput and horizon fill

A 4 km horizon that takes a minute to arrive is not a 4 km view distance. Today's ceiling is
structural rather than tuned: `MeshService::submit_lod` refuses while `lod_busy_`, so at most
one batch is in flight, and `LodBuildConfig::max_jobs` caps a batch at 8.

**Measure first.** `demo/benchmark.gd`'s existing `settle` metric counts physics
`chunks_pending` only and is capped at 1500 frames (`capped=true` in every filed steady leg),
so it says nothing about the far field. B adds two lines: `lod_pending`, the current LoD
request-queue depth, and `frames_to_horizon`, the frame index at which `lod_pending` first
reads zero for 30 consecutive frames (half a second at 60 fps, matched to the existing
`settle` idiom and capped like it, so a world that never quiesces reports `capped=true`
rather than hanging the leg). Cold fill is measured at 4 km against the relief terrain of §6.

**Then decide.** If fill time is unacceptable, raise `LodBuildConfig::max_jobs` past 8 and/or
allow a second batch in flight. Both are `MeshService` changes and both add concurrency; they
stay gated on the measurement rather than being taken on suspicion.

**Fix regardless of the measurement.** `lod_system.cpp:221` clamps the batch with

```cpp
const int take = std::min<int>({lod_builds_per_frame_, int(lod_walk_.requests.size()), 8});
```

That literal `8` duplicates `LodBuildConfig::max_jobs`. It must read the mesher's actual cap,
or raising `max_jobs` silently changes nothing — a latent trap regardless of which way the
throughput measurement goes.

## 5. Budgets

The §1 correction says the budgets are probably adequate. B verifies rather than trusts.

- **Diagnostics.** High-water marks for chunk records used, pages used,
  `LodTree::node_count()`, and per-cascade shadow page counts, surfaced through the debug
  facade and the benchmark line.
- **`kChunkRecords` becomes a parameter of `LodPool::initialize`**, as `max_pages` already is,
  exposed as `max_lod_chunk_records`. The point is not tuning — 8192 is very likely right —
  but that the exhaustion path cannot be tested while the constant is private and
  compile-time. A test drives it to a small value and asserts the degradation below.
- **Degradation is already correct and stays as it is.** `LodPool::upload` returns false when
  either the chunk-slot free list or the arena cannot fund a chunk; `LodSystem` then calls
  `note_ready_dirty` (keeping stale pages drawable) or `note_failed`, accumulates
  `lod_pressure_`, and the next `collect_evictions` recovers pages. The horizon stops
  arriving instead of the ground disappearing, which is the same contract `RegionResidency`
  states for the brick atlas.
- **Add a once-per-run warning naming which pool bound.** Today both exhaustions take the
  same silent path, so a budget cliff is indistinguishable from a slow build queue.
- Raise the constants only if the measurement at 4 km with relief terrain demands it.

## 6. Terrain relief

A new stage in the terrain pipeline milestone 1 already shipped:
`shaders/stages/relief.field.glslh`, its `ve::stage_relief` CPU mirror in
`terrain/builtin_stages.cpp`, a `VE_REGISTER_STAGE` line, and one `stage` line in
`assets/pipelines/default.pipeline`.

It runs **after** `hills` and is purely additive:

```glsl
ctx.height += r;
ctx.sdf    -= r;
```

so `hills` is untouched, `height_bands` keeps banding on the same channel, and a pipeline
that omits the stage is byte-for-byte the pipeline that exists today.

### The Lipschitz bound is the binding constraint, and it is satisfiable

`Generator::lipschitz()` bounds how fast the reported distance may change. Understating it
lets `raycast.cpp`'s `1 / lipschitz()` step overshoot a surface; overstating it costs CPU
raycast steps and widens `mesh_chunk.cpp`'s conservative padding. A relief stage must
therefore be budgeted against it, not bolted on.

`hills()` contributes per-axis amplitude×frequency sums of

```
d/dx: 6(0.11) + 3(0.031) + 1(0.23) = 0.983
d/dz: 6(0.13) + 3(0.043) + 1(0.19) = 1.099
```

so `|grad h| <= 1.474` and `|grad(y - h)| = sqrt(1 + 1.474^2) = 1.782` against the declared
bound of 2.0. Holding 2.0 leaves a relief budget of about **0.15 per axis** in
amplitude×frequency: at 0.15 the bound reaches 1.961, still under 2.0.

```
octave 1:  amplitude 250 m,  wavelength 15.7 km   (amp * freq = 0.10)
octave 2:  amplitude  60 m,  wavelength  7.5 km   (amp * freq = 0.05)
```

Long wavelengths, which is the right shape at this range: across the visible 4 km that is
roughly **250–350 m of height change**, or about 42 px of vertical relief at the horizon
against today's 1.4 px. Phases are chosen so `relief(0, 0) = 0`, leaving `kSurfaceY` and the
demo player spawn undisturbed. A future terrain wanting sharper relief raises `lipschitz`
explicitly and pays the two costs named above; it does not get it by accident.

### The golden corpora must be repointed, not regenerated

`tests/golden/field_baseline.txt` and `brick_baseline.txt` are generated against
`res://assets/pipelines/default.pipeline`, whose path is hardcoded at `voxel_world.cpp:529`
and consumed by `tests/test_field_baseline_gpu.gd`. Changing the default terrain invalidates
both.

Regenerating them would discard exactly the proof they exist to provide — that the generator
did not move. Instead: add a `terrain_pipeline_path` property defaulting to today's path,
freeze the current three stages as `assets/pipelines/golden.pipeline`, and point
`test_field_baseline_gpu.gd` at that. The corpora stay valid and keep proving what they were
written to prove, while the demo's terrain is free to change.

## 7. Configuration surface

| Property | Today | B |
|---|---:|---:|
| `stream_radius_m` | 1638.4 | **4000.0** |
| `max_lod_chunk_records` | — (private `kChunkRecords` = 8192) | 8192, measurement-reviewed |
| `sun_cascade_min_level` | — | enabled (A/B knob for §3) |
| `terrain_pipeline_path` | — (hardcoded at `voxel_world.cpp:529`) | `res://assets/pipelines/default.pipeline` |

`demo/main.tscn`, all six benchmark legs and the capture run at the new default. Cascade
count is fixed at three and is not a property: the radii derive from `stream_radius_m` and
`kLodBaseCell`, and a fourth cascade would need a reason the derivation does not supply.

## 8. Testing

**Phase 0 — baseline.** Full gdUnit on clean `main`, recorded before anything moves. Five
assertions across four suites already fail there; without the baseline every post-change
failure is unattributable.

**Phase 1 — characterization, before code moves.**

- The shadow cut's level-and-coordinate list for a fixed camera at `stream_radius_m = 1638.4`.
- The sun ortho matrix at 1638.4 m. Together these are what prove the cascade rewrite is
  neutral at the old radius.
- `tests/golden` field and brick corpora, repointed per §6 and otherwise untouched.

**Phase 2 — new invariants** (each fails before the change and passes after).

- **Derivation.** Across a radius sweep, `r0`, `r1`, `rN` and their texels match §2's formula.
- **Cascade 2 is today's map.** For any radius R, cascade 2's matrix equals
  `sun_ortho_sphere(dir, cam, R, kSize)` exactly.
- **Collapse.** `stream_radius_m <= r0` yields one cascade whose matrix is today's.
- **Snap, per cascade.** Sub-texel camera motion leaves each cascade's matrix bit-identical;
  motion of one texel translates it by exactly one texel. This is A's shimmer guarantee,
  re-asserted three times over.
- **Cut clamping.** `shadow_cut(radius, min_level)` emits no item below `min_level`, and no
  two emitted items overlap. The cut is not required to be *complete* — a node absent from
  the tree is skipped, exactly as today — but it must never describe one piece of ground
  twice, which is the property the map depends on.
- **Peter-panning.** With `sun_cascade_min_level` on, far-field shadow offset stays within the
  bias the texel already spans. This is the test that falsifies §3's argument if it is wrong.
- **`needs_rebuild` agrees with `build`.** The query and the internal early-out never disagree
  for the same inputs; a cut skipped is a rasterise that would not have happened.
- **Budget exhaustion.** With `max_lod_chunk_records` driven small, the horizon stops growing
  and no geometry is corrupted, dropped, or aliased between chunks.
- **Lipschitz.** The relief pipeline's combined bound is under its declared value, and the
  CPU mirror matches the GLSL through the existing `test_field_diff.gd` path.

**Phase 3 — evidence.** Six benchmark legs at 4000 m with relief terrain, plus a legacy leg
at 1638.4 m so the delta is attributable, filed under `reports/`. A capture at 4 km.

**A measurement honesty constraint carried from A:** GPU timestamps return `samples=0` on this
M1/Metal setup, so every `budget_verdict` GPU entry will read `UNMEASURED`. B reports wall
frame time and says so. It does not present an unavailable GPU number as a zero, and it does
not compare M1/Metal wall times against the RTX 4070/Vulkan figures in `docs/PORTFOLIO.md` as
though they were the same measurement.

## 9. Deliberate deferrals and non-goals

- **Horizon fade. Explicitly descoped, with its consequence recorded.** A §2 listed horizon
  fade as part of B; it is not built. The far field ends abruptly at 4000 m against the
  camera's 4000 m far plane. Chunks will visibly pop at the horizon as the camera moves and
  as builds land, and the LoD level-transition hairline cracks stay unmasked at distance.
  `sky_color(dir)` in `common.glslh` is the hook a later fade would use.
- **Cascade-boundary blending.** Shadow sharpness steps at 409 m and 1280 m (§2).
- **A frame-rate target.** B measures; it does not promise 16.6 ms. The raymarch and frame
  WARNs that `docs/todo/opti.md` leaves open are a renderer budget project of their own, and
  B neither closes them nor is blocked by them.
- **A fourth cascade, or a configurable cascade count.** The derivation gives three; a fourth
  needs a reason it does not supply.
- **Disk persistence.** Sub-project C, unchanged.
