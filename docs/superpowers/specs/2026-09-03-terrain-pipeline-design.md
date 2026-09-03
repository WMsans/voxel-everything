# Voxel Everything — Terrain Pipeline Design

**Date:** 2026-09-03
**Status:** Designed; milestone 1 not yet implemented
**Scope:** Replace the single hardcoded terrain field with a creator-authored pipeline of
stages. Most stages are GPU stages; world-scale planning stages run on the CPU. The context
carried between stages is an open, stage-declared set of channels and resources. The
composed field must terminate in an SDF. No CPU readback ever occurs between GPU stages.
Reserves the hooks the prop system will need.

---

## 1. Problem

The world has exactly one terrain field, written twice.

`ve::AnalyticGenerator::sample()` (`extension/src/generator/generator.cpp`) is thirty lines
of sine hills with one carved sphere. `shaders/field.glslh` is a hand-maintained GLSL mirror
of those same thirty lines, and `tests/test_field_diff.gd` exists to fail when the two drift.
Changing the terrain means editing both files and keeping them in lockstep by hand.

That is the whole of world generation. There is no way to add erosion, biomes, rivers,
caves-as-a-system, or anything that needs more than a closed-form function of `(x, y, z)` —
and no way at all to place a tree, because nothing in the field path has a concept of an
object with a position.

Three properties of the existing engine constrain any replacement, and all three are easy to
break by accident:

1. **The field is a function of an arbitrary point, not a buffer.** `shaders/field.glslh` is
   `#include`d by ten shaders — `brick_gen`, `brick_mark`, `raymarch`, `mesh_field`,
   `lod_field`, `island_extract`, `brick_consolidate` and three probes. `brick_gen` alone
   calls `eval_field()` about 5 000 times per brick, and its material-projection phase
   deliberately samples *outside* the brick, marching along the gradient. A design that
   materialises the field into per-brick buffers cannot serve those callers.
2. **The CPU evaluates the same field, exactly.** Collider streaming, island extraction,
   consolidation baking and raycast all walk `const ve::Generator &`. `test_field_diff.gd`
   enforces agreement with the GPU to within two encoded steps.
3. **The seam is nominal, not enforced.** `ve::FieldGenerator` exists as the documented
   world-generation seam, but `ve::AnalyticGenerator` is constructed *directly* at roughly
   fifteen sites that bypass it: `physics/collider_streamer.h:124`,
   `physics/island_manager.h:183`, `render/island_extract_pass.cpp:206`,
   `render/consolidate_pass.cpp:186`, and nine in `debug/hooks.cpp`. Left alone, those sites
   would keep generating the old world while the GPU generated the new one.

## 2. Decided constraints

| Decision | Choice |
|---|---|
| CPU parity | Mirrored stages by default; a declared GPU-only escape hatch flips the world to GPU-authoritative |
| CPU stage granularity | World scope only — global graph and planning work. No per-brick CPU stages, ever |
| Authoring surface | GLSL stage files with an in-file manifest; a plain-text pipeline resource wires them |
| Field composition | Source composition into one generated `field.glslh`, not an in-shader interpreter |
| Prop placement | A GPU scatter map stage writing an append buffer — not a CPU stage |
| Milestone 1 | Field program, compiler, CPU mirror, and the `sector2d` map tier only. Props designed, not built |

## 3. Architecture

### 3.1 Three scopes

| Scope | Runs | Readback | Produces |
|---|---|---|---|
| **World** | once per world, from the seed | permitted — CPU planner stages live here | world context resources |
| **Sector** | once per coarse tile, on demand, async, cached | permitted, amortised | sector context resources |
| **Field** | per brick in `brick_gen`, and at arbitrary points in nine other shaders | **never** | `sdf` and `material` |

A **sector** is a new coarse tile. Regions are `kRegionSize` = 25.6 m, far too small to carry
a river or a biome. `ve::kSectorRegions = 16` gives a 409.6 m square sector. Sector context is
columnar by default; the resource system permits 3D sector volumes without redesign.

The no-readback rule is structural rather than conventional: **the scheduler exposes no
readback primitive at field or sector-build scope.** The only two readbacks in the system are
terminal — publishing a finished sector's resources to the host cache (§7.3), and feeding
world-scope CPU stages (§6.3).

### 3.2 Two stage kinds

**`MapStage`** — a compute pass over a domain, reading and writing named context resources.
World or sector scope only. Consecutive GPU map stages record into one open compute list,
separated by `compute_list_add_barrier`, exactly as `WorldStreamer::run_frame` already does.
A CPU stage is a scope boundary by definition and may appear only at world scope.

**`FieldStage`** — a GLSL function over a generated `FieldCtx` struct, composed into
`eval_field()`. Field scope only. Never a dispatch of its own.

### 3.3 Containment

The pipeline replaces exactly two things: `base_field()` in `shaders/field.glslh`, and
`ve::AnalyticGenerator::sample()` behind the `ve::FieldGenerator` seam. Everything downstream
is untouched — edit ops still apply in order after the base field, overrides still supersede
it, and palettes, mips, occupancy classification and the volume store never learn the field
changed.

### 3.4 New files

```
extension/src/terrain/stage_manifest.h/.cpp        directive parsing (pure, native-testable)
extension/src/terrain/pipeline.h/.cpp              ve::TerrainPipeline: resolve + validate
extension/src/terrain/field_codegen.h/.cpp         generates field.glslh text
extension/src/terrain/stage_library.h/.cpp         registry of C++ mirror functors
extension/src/terrain/pipeline_field_generator.h/.cpp   ve::PipelineFieldGenerator
extension/src/render/context_scheduler.h/.cpp      DAG, allocation, aliasing, barriers
extension/src/render/sector_context.h/.cpp         sector cache, prefetch, readback publish
shaders/stages/*.field.glslh                       field stage sources
shaders/stages/*.map.glsl                          map stage sources
assets/pipelines/*.pipeline                        pipeline resources
```

Everything under `extension/src/terrain/` is pure C++ with no Godot or GPU dependency, so it
joins `pure_sources` in `SConstruct` and runs in the fast native suite. Everything under
`extension/src/render/` needs the GPU and does not.

## 4. Stage manifest format

One file per stage. The manifest is a block of `//!` directives at the top of the GLSL source,
so manifest and code cannot drift apart.

```glsl
//!stage     hills
//!kind      field
//!out       sdf : float
//!sample    sector.height : texture2d_r32f
//!param     amplitude : float = 6.0
//!lipschitz 2.0
//!cpu       ve::stage_hills

void stage_hills(inout FieldCtx ctx) {
    ctx.sdf = ctx.p.y - SURFACE_Y - sample_sector_r(sector_height, ctx.p) * P.amplitude;
}
```

| Directive | Meaning |
|---|---|
| `//!stage <name>` | Identity. Must be unique within a pipeline. |
| `//!kind field\|map` | Which scope the stage may occupy. |
| `//!in <channel> : <type>` | A `FieldCtx` channel this stage reads. |
| `//!out <channel> : <type>` | A `FieldCtx` channel this stage writes. |
| `//!sample <resource> : <type>` | A context resource read through set 1. |
| `//!param <name> : <type> = <default>` | A per-pipeline tunable, packed into the params UBO. |
| `//!lipschitz <bound>` | This stage's contribution to the field's gradient bound (§10.1). |
| `//!cpu <symbol>` | Names the C++ mirror functor. Absent means GPU-only. |
| `//!domain <domain> [WxH]` | Map stages only: the dispatch grid. |
| `//!iterate <n>` | Map stages only: run `n` times with barriers between. |
| `//!bounds <radius>` | Scatter stages only: maximum world-space influence radius (§9). |

**Channel types** are `float`, `vec2`, `vec3`, `vec4`, `int`, `uint`. Three channels are
built in: `p : vec3` (read-only), `sdf : float`, `material : uint`.

**`out` is the extension mechanism.** A stage declaring `//!out temperature : float` adds
that channel to the context for every later stage in the pipeline. This is what makes the
inter-stage context open-ended without the compiler knowing any domain concepts.

**Resource types** are `texture2d_*` and `texture3d_*` for sampled maps, `image2d_*` and
`image3d_*` for map-stage storage writes, `buffer<T>` for structured data whose GLSL struct
the producing stage declares, and `append_buffer<T>` for scatter output. Resource names are
`<scope>.<name>`; the scope prefix determines the pool the resource is allocated from and its
lifetime. A resource's extent is declared where it is first written.

### 4.1 The pipeline resource

Plain text, hand-writable, diffable, and hashable:

```
# assets/pipelines/default.pipeline
seed      1337
lipschitz 2.0

stage stages/sector_height.map.glsl
stage stages/hills.field.glslh
  amplitude 6.0
stage stages/cave.field.glslh
  radius 5.0
```

The parser takes a string, not a path, so it runs in the native suite. A thin loader wraps it
in `FileAccess` for `res://`.

## 5. The field program

### 5.1 Generated `field.glslh`

The compiler emits, in this order:

1. `struct FieldCtx` — built-ins plus every resolved channel, ordered by the pipeline position
   of each channel's first write, so the generated text is stable and diffable.
2. Set-1 declarations for the params UBO, the sector map, and every sampled resource, at
   binding indices assigned in sorted-name order (§5.2).
3. Each field stage's body, verbatim, in pipeline order.
4. `eval_base_field(vec3 p, out float sdf, out uint mat)` — constructs the context, calls each
   stage in order, and returns `ctx.sdf` and `ctx.material`.
5. `eval_field()` with its **exact current shape** — base field, then override, then region ops
   in order — with only the call to `base_field` swapped for `eval_base_field`.

Injection is `ve::set_shader_source_override("field.glslh", generated)` followed by
`RenderOrchestrator::request_shader_reload()`. The existing preflight compiles all ten
consumers against the new field *before* tearing anything down, so a broken pipeline cannot
kill a running world: last-known-good pipelines survive and `reload_last_error_` carries the
message. That safety property is already built and is inherited unchanged.

### 5.2 Set 1

All ten field-consuming shaders currently declare only descriptor set 0 — verified across
`brick_gen`, `brick_mark`, `raymarch`, `mesh_field`, `lod_field`, `island_extract`,
`brick_consolidate` and the three probes — so set 1 is free everywhere. The pipeline runtime
owns one uniform set at index 1:

| Binding | Contents |
|---|---|
| 0 | Params UBO — every stage's resolved `//!param` values |
| 1 | `sector_map` — resident sector slot table, indexed by sector coordinate |
| 2..N | Sampled context resources, sorted by name |

Every field-consuming pass binds it at index 1 alongside its own untouched set 0. With no
declared resources the set still holds bindings 0 and 1, so the binding code has no special
case.

### 5.3 Global sector addressing

`eval_field()` is called at arbitrary points, including points outside the brick being
generated and, in the raymarcher, points anywhere in the world. Sector context therefore
cannot be bound per-sector. Sector resources are **texture arrays whose layer is a sector
slot**, reached through the `sector_map` indirection — structurally identical to the existing
`region_map_` → region slot lookup:

```glsl
float sample_sector_r(sampler2DArray res, vec3 p) {
    ivec2 sc   = sector_coord(p);
    int   slot = sector_map_lookup(sc);
    if (slot < 0) return RESOURCE_FALLBACK;   // declared per resource
    return texture(res, vec3(sector_uv(p, sc), float(slot))).r;
}
```

Each resource declares a fallback value for the non-resident case. The gating rule in §7.4
guarantees this is only reached outside the streamed area.

### 5.4 Validation

All of it happens in the compiler, before any GPU work, and every error names the stages
involved:

- Every `in` channel must be written by an earlier stage, or be built in.
- **The pipeline must write `sdf`.** A pipeline that never does is rejected. This is how
  "the final stage has to come up with an SDF" is enforced rather than documented.
- Two stages writing one channel is legal — ordered override is the composition model.
- Channel type conflicts, duplicate stage names, unresolvable resources, and resource
  type mismatches between producer and consumer are errors.
- A `//!kind map` stage in field position, or vice versa, is an error.
- A stage with no `//!cpu` is rejected unless the world sets `allow_gpu_only_field` (§6.2).

## 6. The CPU mirror

### 6.1 Structure

`//!cpu ve::stage_hills` names a functor registered in `ve::StageLibrary`:

```cpp
void stage_hills(ve::FieldCtx &ctx, const ve::StageParams &params,
                 const ve::FieldResources &res);
```

`ve::PipelineFieldGenerator` implements `ve::FieldGenerator` and holds the ordered vector of
functors plus a `FieldCtx` whose channel indices **the same compiler assigned**. CPU and GPU
therefore agree by construction, not by inspection — a channel cannot be at index 3 on one
side and index 4 on the other.

`ve::FieldGenerator` requires `sampler()` to hand out a `const ve::Generator &`, because
`brick_eval`, `raycast` and `extract_island_volume` all consume that view. The pipeline
generator holds an inner `ve::Generator` implementation that walks the same functor list, so
those call sites keep their current signatures unchanged. Its `lipschitz()` returns the
compiler-combined bound from §10.1 rather than a hardcoded 2.0.

The ~15 direct `ve::AnalyticGenerator` construction sites listed in §1 are routed through the
seam as part of this milestone. This is not optional cleanup: left in place they would
generate the old world on the CPU while the GPU generated the new one.

### 6.2 The escape hatch

A stage with no `//!cpu` marks the pipeline GPU-only. The compiler rejects it unless the world
opts in via `allow_gpu_only_field`, in which case `ve::PipelineFieldGenerator::is_cpu_exact()`
returns false and `WorldStore` exposes that to consumers. The GPU-probe-driven fallbacks for
collider streaming and island extraction are designed for here — `shaders/field_probe.comp.glsl`
and `field_gradient_probe.comp.glsl` already evaluate the field at arbitrary point lists on the
GPU — but are built in a later milestone.

### 6.3 Map-derived data on the CPU

The CPU mirror does not recompute map stages. **Sector context resources are read back once
per sector into a host-side cache**, which sector scope explicitly permits. The CPU mirror then
samples the identical bytes the GPU used, not an approximation of them, which removes
map-derived data from the parity problem entirely.

World-scope CPU planner stages read their inputs back from earlier world-scope GPU stages by
the same mechanism, once per world.

## 7. Map stages and the context scheduler

### 7.1 A map stage

```glsl
//!stage   erosion
//!kind    map
//!domain  sector2d 256x256
//!in      sector.height : image2d_r32f
//!out     sector.height : image2d_r32f
//!out     sector.flow   : image2d_rg16f
//!iterate 64
```

`//!iterate` covers the iterative case honestly: hydraulic erosion is 64 dispatches with
barriers between them, and the scheduler ping-pongs the in-place resource rather than making
the author manage two names. `//!domain` defaults to the extent of the stage's first output.

### 7.2 The scheduler

`godot::ContextScheduler` is a small render graph. It builds a DAG from declared reads and writes,
topologically orders it, allocates physical resources, **aliases** those whose lifetimes do not
overlap, and inserts `compute_list_add_barrier` between every producer and consumer. A whole
sector build records into one compute list and one submit.

### 7.3 CPU planner stages

World scope only:

```glsl
//!stage  rivers
//!kind   map
//!domain world
//!cpu    ve::plan_rivers
//!in     world.elevation   : buffer<float>
//!out    world.river_nodes : buffer<RiverNode>
//!out    world.river_edges : buffer<RiverEdge>
```

One run at world init on a worker thread. Inputs arrive by readback from earlier world-scope
GPU stages; outputs upload as buffers into set 1. Deterministic from the seed, so results
serialise and are skipped on reload.

### 7.4 Caching, gating and budget

Everything is a pure function of `(seed, pipeline hash, tile coordinate)`. The **pipeline hash**
is a content hash over every stage source plus resolved params, and it is what makes a
hot-reloaded stage file invalidate stale sector maps *and* the generated shader together
instead of leaving the two disagreeing.

- **World context**: computed once, held in memory, optionally serialised.
- **Sector context**: LRU keyed by `(sector coordinate, pipeline hash)`, evicted alongside the
  regions it serves, recomputed bit-identically on re-entry.
- **Gating**: a region cannot be marked until its sector context is resident on both the GPU
  (set 1) and the host cache. `WorldStreamer` gains a sector-prefetch step at one sector beyond
  the region streaming radius, funded the way its stream-ins already are.
- **Budget**: at most one sector build per frame, with a bounded dispatch count, so a
  64-iteration erosion never lands as a frame spike. The publishing readback uses the existing
  `AsyncBufferRead` machinery that already brings occupancy back eight reads in flight.

## 8. Prior art

| System | What it contributes |
|---|---|
| [godot_voxel `VoxelGeneratorGraph`](https://voxel-tools.readthedocs.io/en/latest/generators/) | Node graph compiled to a compute shader per usage, dispatched on a separate `RenderingDevice` in a background thread; a VM on the CPU. Its documented wall — *"graph generators only work per voxel"*, hence no trees or villages — is the limitation §9 is designed to beat. Also: [GPU normalmaps do not support edited voxels](https://voxel-tools.readthedocs.io/en/latest/performance/), a reminder to keep edits downstream of the base field. |
| [Unreal PCG GPU processing](https://dev.epicgames.com/documentation/unreal-engine/using-pcg-with-gpu-processing-in-unreal-engine) | Contiguous GPU nodes fuse into one "Compute Graph"; the editor badges every CPU↔GPU crossing; GPU buffers are sized upfront; `Custom HLSL` is the escape hatch. It is also Unreal's props system — the closest whole-system match. |
| [Houdini heightfields](https://www.sidefx.com/docs/houdini/model/terrain_workflow.html) | Terrain as a stack of *named layers*, every node reading and writing layers by name, and [`HeightField Scatter`](https://www.sidefx.com/docs/houdini/nodes/sop/heightfield_scatter.html) consuming a mask to place trees. The proven form of §4's open channel set. |
| [MapMagic 2](https://assetstore.unity.com/packages/tools/terrain/mapmagic-2-165180), [TerraForge3D](https://github.com/Jaysmito101/TerraForge3D) | Typed multi-port products (matrix / objects / splines) between nodes; separate object and biome graphs. TerraForge3D is open source, GPU-backed, with node *and* layer workflows. |

## 9. Space for the prop system

Nothing prop-specific is built in milestone 1. Four things exist in the type system from day
one because each is painful to retrofit:

1. **Placement is a GPU scatter map stage.** It reads masks already in sector context — slope,
   moisture, biome — and appends to `append_buffer<PropInstance>`. Deterministic from seed and
   sector coordinate, so it caches and replays like everything else. The instance struct is
   whatever the stage declares; nothing is fixed here.
2. **`append_buffer<T>` is a first-class resource type**, counter included, so a later stage can
   `dispatch_indirect` over the instance count **without reading it back**. This is the
   load-bearing no-readback piece for props, and the engine already does exactly this:
   `shaders/dispatch_args.comp.glsl` and `RegionPass::write_dispatch_args` derive an indirect
   dispatch from a GPU-written job count for bricks.
3. **Field stages can sample buffers, not only textures.** Props that *are* terrain — a tree
   trunk, a rock, a foundation — reach the SDF through a field stage that reads the instance
   buffer and unions primitives into `ctx.sdf`. Tractability needs a binning stage producing
   `sector.prop_grid` so the field stage examines only its cell neighbourhood; set 1 supports
   SSBOs from the start, and the binning stage itself is future work.
4. **Scatter stages declare `//!bounds <radius>`** — the maximum distance a prop influences the
   field. It sets how many neighbouring grid cells a field stage queries and the required
   sector overlap. Without it, a tree straddling a sector boundary is clipped by whichever
   sector did not scatter it.

Two consequences fall out at no cost. Structures larger than a brick reuse the **existing volume-op
machinery**: `OP_VOLUME_ADD` and `VolumePool` already stamp stored voxel volumes into the field,
so a placement stage emitting volume ops needs no new stamping path. And because props are part
of the *base* field, player edits carve them exactly like terrain, since ops apply after the base
field — felling a tree permanently is an edit op, which the override and consolidation paths
already persist.

CPU parity for props is free: the prop field stage needs a `//!cpu` mirror, and its instance
buffer rides the sector host-cache readback of §6.3.

## 10. Failure modes

| Failure | Handling |
|---|---|
| Bad stage GLSL | Preflight compiles all ten consumers before teardown; last-known-good pipelines survive, `reload_last_error_` carries the message |
| Unwritten `in`, no `sdf` write, type conflict, unresolved resource, kind mismatch | Compiler, before any GPU work, naming the stages involved |
| GPU-only stage without `allow_gpu_only_field` | Compiler rejects |
| Sector context pool exhausted | LRU evict; if a sector still cannot build, its regions hold rather than stream in — the shape the existing free-list-dry `repair_queue_` already uses |
| Stale sector cache after a stage edit | Pipeline hash is part of the cache key |
| Teardown / device loss | The pipeline runtime owns its RIDs; set 1 is released inside `RenderOrchestrator`'s existing verbatim teardown order, before the atlas |

Two hazards fail *silently*, and both are new surface that arbitrary creator fields introduce.

### 10.1 Lipschitz violation

`ve::Generator::lipschitz()` is what stops the sphere tracer tunnelling through overhangs;
`AnalyticGenerator` returns a hand-derived 2.0. An arbitrary composed field has no known bound.
Each stage declares `//!lipschitz`, the compiler combines them, and the pipeline resource may
override the result. A debug mode samples the field and warns when the declared bound is
violated. Without this, a creator's first steep field looks like a *rendering* bug and they go
hunting in the raymarcher.

### 10.2 SDF range

The atlas stores `r8`-quantised SDF over `SDF_RANGE`. Distances beyond that clamp. This is
existing behaviour, but it becomes a constraint stages must respect and is documented as such.

### 10.3 CPU/GPU divergence

The other silent hazard, and what §11 is mostly built around.

## 11. Testing

1. **`test_field_diff.gd` becomes pipeline-parameterised.** It already loads
   `field_probe.comp.glsl` at runtime and diffs CPU against GPU at sample points; the new
   version runs that diff for several test pipelines — the ported field, a multi-channel one,
   and one sampling a sector resource. This is the most important test in the design.
2. **Port equivalence.** The milestone's proof: a stage-pipeline port of today's hills-and-cave
   field produces *identical bricks* to `ve::AnalyticGenerator`. `test_brick_diff.gd` already
   diffs bricks; it is pointed at both generators. The port is pure field stages with no sector
   resource, so the equivalence is exact rather than approximate.
3. **Golden generated source.** The ported pipeline compiles to a committed `field.glslh`
   byte-for-byte, exactly the `material_table.glslh` pattern already in the repo.
4. **Compiler unit tests** in the native `pure_sources` suite — every validation rule in §5.4,
   stable channel-index assignment, deterministic generated text, manifest parse errors. No
   GPU, so they run in the fast suite.
5. **Scheduler tests** — DAG ordering, aliasing correctness (no two live resources sharing
   memory), barrier insertion, `//!iterate` ping-pong.
6. **Sector cache tests** — the same sector built twice yields identical bytes, pipeline-hash
   invalidation, LRU eviction under budget.
7. **Gating test** — no region marks before its sector context is resident on both sides, and
   sector prefetch stays ahead of region streaming.
8. **Reload safety** — a broken stage injected through `set_shader_source_override` leaves the
   world rendering and the error reported.

A separate demo pipeline exercising the `sector2d` map tier proves the map path independently
of the equivalence port, so a failure in one does not mask the other.

**Baseline note for the implementation plan:** this repository has five known-failing assertions
across four suites on a clean `main`. Capture that baseline before attributing any failure to
this work.

## 12. Milestone 1 scope

**Built:** manifest parser, pipeline resolution and validation, field codegen, set 1, the
`sector2d` map tier, the context scheduler, the sector cache with gating and prefetch,
`ve::PipelineFieldGenerator` with the stage library, routing the ~15 direct
`AnalyticGenerator` sites through the seam, the equivalence port, and the tests in §11.

**Specified and supported by the scheduler, but no stage ships:** the `world` domain. The
scheduler resolves and orders world-scope resources so the sector tier can depend on them, and
§7.3's CPU planner contract is fixed, but milestone 1 ships no world-scope stage and no CPU
planner implementation.

**Designed, not built:** CPU planner stage implementations (§7.3), `region3d` as a domain, the
prop scatter and binning stages (§9), the GPU-authoritative fallbacks for collider streaming
and island extraction (§6.2), and any node-graph authoring UI.
