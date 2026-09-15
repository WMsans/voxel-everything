# Voxel Everything — Frame Module & Architecture Deepening Roadmap

**Date:** 2026-09-13
**Status:** Implemented sub-project 1 (Tasks 10–12 completed within sub-project 2); roadmap continues
**Scope:** (1) Sub-project 1 in full: extract a `VoxelFrame` module that owns the frame, make
the compositors and the frame-rebuilding debug probes call it, and delete the probe copies.
(2) Every other deepening pathway found during exploration, recorded with evidence so each can
become its own spec → plan → implementation cycle without re-exploring.

Vocabulary: *module, interface, depth, seam, adapter, locality, leverage* in Ousterhout's sense
(A Philosophy of Software Design). Domain nouns: see `CONTEXT.md` at the repo root.

---

## 1. Problem

The 2026-08-22 decomposition (`2026-08-22-voxel-world-decomposition-design.md`) split the
7k-line `VoxelWorld` into `WorldStore`, `RenderOrchestrator`, `LodSystem`,
`ConsolidationCoordinator` and `debug/hooks`. It moved state; it did not deepen modules. The
hot spots in the last 200 commits are still the hub and its satellites:

| File | Touches (last 200 commits) | Size |
|---|---|---|
| `voxel_world.{h,cpp}` | 160 | 526 + 1,301 lines |
| `debug/hooks.{h,cpp}` | 70 | 517 + 6,197 lines |
| `raymarch_compositor.cpp` | 35 | 464 lines |
| `render/orchestrator.{h,cpp}` | 30 | 356 + 668 lines |

The costliest symptom: **the frame has no module.** The real frame is the 415-line
`RaymarchCompositor::_render_callback` (`raymarch_compositor.cpp:49-464`) plus
`BeautyCompositor::_render_callback` (`beauty_compositor.cpp:26-100`). `debug/hooks.cpp`
rebuilds partial copies of it in roughly 15 hooks (15 hand-built `CameraParams::looking_at`,
8 hand-built `DeferredPass::Params`), and 71 of 81 gdUnit suites test through those hooks on a
local device. The copies have drifted from what ships:

- `debug_ssao_probe` (`hooks.cpp:601-710`) runs SSAO regardless of `kFlagSsao` (shipped gate:
  `raymarch_compositor.cpp:437`), marches a fixed 200 m instead of the fade-band end
  (`:199`), and ignores `near_field_scale`.
- `debug_deferred_probe` (`hooks.cpp:4768-4838`) sets no cascades and a zero fade band.
- `debug_grass_stats` (`hooks.cpp:~1994`) skips the reach clamp (`raymarch_compositor.cpp:385-386`).
- `debug_contact_shadow_probe` feeds G-buffer lit/depth; shipped feeds scene colour/depth
  (`beauty_compositor.cpp:70`).

A green probe test therefore does not prove the shipped frame works (project memory: "debug
hooks rebuild render inputs"). Until that is fixed, no later refactor has a trustworthy net.

Change cost today (traced, see Appendix A): a new render pass touches 12–19 files (SSAO commit
`6aadf43`: 19; grass skeleton `c531f9d`: 15), a quarter of which are frame wiring and
hand-copied probe rebuilds.

## 2. How this was explored

- Hot spots from `git log` churn over 200 commits; prior specs read (no `CONTEXT.md` or ADRs
  existed).
- Three read-only walks: (a) world hub, hooks, compositors, orchestrator; (b) render passes,
  shading, materials, look-dev; (c) terrain pipeline, world data, LoD, mesh, physics. Each traced
  concrete artist/developer change scenarios end-to-end and applied the deletion test.
- Key claims spot-checked by hand (compositor bodies, two probes, collider probe).
- Research (Appendix B): deep modules and the deletion test; render/frame graphs and when they
  are overkill; Godot's Compositor model; godot_voxel's data-driven libraries.

## 3. Decided constraints

| Decision | Choice |
|---|---|
| First sub-project | Frame module |
| Reach | Frame extraction **and** migration of frame-rebuilding probes; orchestrator lifetime cleanup is sub-project 2 |
| Shape | `VoxelFrame`: one module, explicit ordered stage methods (not a stage list, not a free function) |
| Golden diffs when a probe starts running the shipped frame | Shipped frame is truth: explain each diff by cause, re-record the golden in the same commit and say so; a diff that reveals a shipped bug stops the task and is reported — no fixes inside the refactor |
| Render graph | Rejected for this engine size (~25 passes, one queue); ordering stays explicit code |

## 4. Target design: `VoxelFrame`

### 4.1 Location and ownership

`extension/src/render/frame.{h,cpp}`. Owned by `RenderOrchestrator`, which constructs it after
`LodSystem` and `WorldStore`. Not in the native test build (needs RenderingDevice), like the rest
of `src/render/`.

### 4.2 Interface

```cpp
// Debug knobs that already exist on probes. Defaults == the shipped frame.
struct FrameDebug {
    int deferred_view = 0;        // DeferredPass::Params::probe_mode
    bool skip_far_field = false;  // debug_seam_probe(skip_lod)
    RID marker;                   // optional seam-probe ownership marker
    Vector2i lod_viewport;        // settled LoD viewport; zero means frame size
};

struct FrameInputs {
    Transform3D cam;
    Projection proj;              // engine projection (y-flipped) or synthetic perspective
    Vector2i size;                // internal render size
    RID scene_color, scene_depth; // engine buffers; invalid => frame-owned headless targets
    RID normal_roughness;         // optional (post-opaque)
    RenderSceneBuffersRD *rsb = nullptr; // GBuffer::ensure context; null when headless
    FrameDebug debug;
};

struct FrameSettings {            // per-frame values RenderOrchestrator owns, sampled once
    ve::SunState sun;
    float near_field_scale;
    bool near_field_enabled;
    bool sun_cascade_min_level;
};

struct FrameRecord {              // returned by value under a leaf mutex
    float fade_start, fade_end;
    bool lod_two_phase, hiz_built;
    int lod_first_pass_count;
    uint32_t stages_ok, stages_cancelled; // bit per stage label
};

// Historical SP1 interface: FrameHost was deleted by sub-project 2 in commit 6b595c1.
class VoxelFrame {
public:
    VoxelFrame(RenderOrchestrator &, LodSystem &, WorldStore &);
    bool render_pre_opaque(RenderingDevice *, const FrameInputs &);
    bool render_post_opaque(RenderingDevice *, const FrameInputs &);
    bool render_headless(RenderingDevice *, const FrameInputs &); // both halves, one call
    FrameRecord last_frame() const;
    // The one synthetic probe camera (fov_y 60°, near 0.05, far 4000, up-vector rule).
    static FrameInputs looking_at(Vector3 pos, Vector3 fwd, int w, int h);
};
```

### 4.3 What the frame hides

Stage methods, in the shipped order, moved verbatim:

- **Pre-opaque:** camera/UBO/sun-UBO update → `stream` (island upload drain, streamer
  `run_frame`, window refresh) → `raymarch` (island cull, reach clamp to fade end, target
  rebuild ⇒ composite uniform-set invalidation) → `composite` → HiZ → `lod` two-phase with
  `sun_shadow` interleaved, cascade fitting → `grass` (reach clamp) → `ssgi` → `deferred`
  (containing `ssao`) → `inject` → finish beauty frame.
- **Post-opaque:** camera UBO → `contact` → `ssr` → `outlines` → `history` → timings
  `end_frame`.

Also hidden: `CameraParams` / `DeferredPass::Params` / `LodCamera` packing, fail-soft
`abort_frame` paths, the GPU-timings begin/cancel/abort protocol that spans both callbacks, and
the normal-roughness state probe.

`render_headless` allocates colour and depth targets in the same formats `InjectPass` writes
into (verified during planning) when `FrameInputs` carries none, and runs both halves against
them. Headless has no engine opaque objects; that difference is documented, not bridged.

### 4.4 What shrinks

- **Compositors** become ~20 lines each: callback admission guard, `pump_shader_reload` /
  `ensure_initialized` (lifetime concerns stay outside the frame), `RenderData` → `FrameInputs`,
  call `render_pre_opaque` / `render_post_opaque`.
- **`VoxelWorld`** loses the accessors only the frame used: `lod_tick`, `prepare_lod_raster`,
  `prepare_lod_shadow_raster`, `sun_ortho`, `lod_fade_band`, `note_lod_cull_debug` and its three
  atomics + `lod_cull_debug()`, `finish_beauty_frame`, `set_beauty_compositor` /
  `beauty_compositor_` (written, never read), and `render_probe_pixel` (moved to
  `VoxelDebugHooks`). `downsample_history` stays for the isolated SSGI history-latch probe;
  any accessor a surviving caller still needs stays. The temporary `FrameHost` seam was deleted
  by sub-project 2 in commit `6b595c1`.
- **`hooks.cpp`** loses every migrated rebuild (§5). Before/after line counts are reported.

### 4.5 Unchanged

Compositor admission and lifetime locks, the shader-reload pump, orchestrator construction and
teardown order, every pass's internals, lock order (`edit_mutex → lod_mutex`), and the handoff
leaf's atomic threading of `island_slot_count`.

### 4.6 Resolved named debt

`FrameHost` was the one-adapter seam accepted for SP1 so the island handoff queue and its mutex
could remain in `VoxelWorld`. Sub-project 2 moved that queue into the render lifetime owner and
deleted `FrameHost` by name in commit `6b595c1`; it is no longer part of the implementation.

## 5. Probe migration

Provisional classification; the plan verifies each hook by reading it.

| Migrate to `render_headless` (rebuilds a multi-pass chain today) | Stay a pass test (fixture / single pass) |
|---|---|
| `debug_ssao_probe` | `debug_raymarch_probe`, `debug_raymarch_pixel`, `debug_raymarch_cost_probe` |
| `debug_deferred_probe` (`debug.deferred_view`) | `debug_raymarch_gbuffer`, `debug_raymarch_hole_probe`, `debug_raymarch_normal_probe` |
| `debug_ssgi_probe` (N calls for history) | `debug_island_normal_probe`, `probe_material`, `debug_glossy_sdf_probe` |
| `debug_ssgi_reprojection_probe` | `debug_ssr_probe` (fixture), `debug_outline_probe` (fixture) |
| `debug_contact_shadow_probe` | `debug_hiz_probe_synthetic`, `debug_hiz_shutdown_probe` |
| `debug_seam_probe` (`debug.skip_far_field`) | `debug_island_tile_mask`, `debug_lod_cull_probe` |
| | `debug_near_field_detail`, `debug_sun_shadow_build`, `debug_lod_tick` |
| | `debug_grass_stats`, `debug_lod_gbuffer_probe`, `debug_lod_render_probe(_culled)` |
| | `debug_cel_diff` (synthetic albedo/ndl fixture), `sun_shadow_probe` (single-point deferred sample) |

The six migrated hooks are `debug_ssao_probe`, `debug_deferred_probe`, `debug_ssgi_probe`,
`debug_ssgi_reprojection_probe`, `debug_contact_shadow_probe` and `debug_seam_probe`; the
reclassified pass probes are `debug_lod_render_probe(_culled)`, `debug_lod_gbuffer_probe`,
`debug_grass_stats` and `debug_near_field_detail`. Pass tests keep their isolation but build
perspective fixtures through `ve::probe_camera`, single-ray fixtures through `ve::probe_up_hint`
and the existing pure basis primitive, and terrain fixtures through the shared world/flag
packing. Probes keep their GDScript signatures and Dictionary keys; only numbers may move under
§3's golden policy. The single-ray render helper belongs to `VoxelDebugHooks`; retained world
forwarders and concrete callers are `downsample_history` (SSGI history-latch probe),
`lod_tick` (LoD probes), `prepare_lod_raster` (LoD render/gbuffer and shadow probes),
`prepare_lod_shadow_raster` (shadow probes), `sun_ortho` (shadow probes), and `lod_fade_band`
(LoD, near-field, SSR and shadow probes).

The settle loop (`debug_stream_frame` until quiet) stays in the hooks as world preparation; the
frame's own `stream` stage is idempotent once quiet.

## 6. Testing strategy and task order

**Step 0 — baselines (no production change).**
1. Record the gdUnit failure set on clean `main` (project memory: a standing set fails and it
   drifts; stash and re-run before blaming a change).
2. Add `tests/test_frame_shipped_golden.gd`: a minimal scene with the real `RaymarchCompositor`
   and `BeautyCompositor`, three cameras (close ground, oblique, horizon) at a lit spot such as
   `(20, 60, 30)` — not the `(30, 56.2, 30)` hook spot, which sees buried cave grass. Pin per
   camera: mean luma of each 8×8 viewport tile, the LoD cull record, and the **set** of timing
   labels (values are invalid on this machine). Tolerances tight, recorded with date and reason.

**Step 1 — extract.** Move both bodies into `VoxelFrame` verbatim; compositors call it. Exit:
Step 0 golden unchanged; failure set ⊆ baseline.

**Step 2 — frame contract tests (new, through `render_headless`).**
- Two renders of the same inputs produce identical lit output.
- `FrameRecord` is populated (fade band, LoD record and stage bits); grass counters stay covered by the grass suites.
- A failing stage cancels its own label and aborts the frame.
- `near_field` off ⇒ HiZ not built ⇒ every LoD page drawn.
- SSGI history survives N consecutive calls and falls on a size change.

**Step 3 — migrate probes, one probe per commit.** Each commit: run the probe's suites, diff,
attribute every moved number to a cause (e.g. "reach now clamps to fade_end"), re-record in the
same commit with the cause in the message. A diff that looks like a shipped bug stops the task.
Deterministic output comparisons disable temporal SSGI and wind; timing values remain unpinned.

**Step 4 — delete.** Remove orphaned copies and frame-only `VoxelWorld` accessors; rebuild; full
suite; report line counts for `hooks.cpp`, `voxel_world.h`, `voxel_world.cpp`, both compositors.

Canonical commands: `./build.sh`, `./gdunit_tests.sh`.

## 7. Scope guards (out of scope for sub-project 1)

1. No pass internals, no pass helper, no generated layouts (sub-project 4).
2. No locking or lifetime changes; `Collaborators` slots stay (sub-project 2).
3. No settings-store changes (sub-project 3).
4. No bug fixes, including the suspected bugs in §9 — they are recorded only.
5. No GDScript API changes beyond what a migrated hook needs; Dictionary keys preserved.
6. No reordering of frame stages.

Hidden coupling that proves load-bearing mid-task upgrades that task's scope explicitly: stop,
say so, re-plan.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Headless frame differs from engine frame (no engine opaque objects; contact shadows read injected lit) | Documented in `frame.h`; contract tests compare headless to headless only; Step 0 golden covers the engine path |
| Timings protocol spans two callbacks; post-opaque may not fire | Moved verbatim; contract test for abort/cancel; headless runs both halves |
| Migrated goldens move for many reasons at once | One probe per commit; every moved number attributed to a cause |
| `FrameHost` ossifies | Resolved: sub-project 2 deleted it in commit `6b595c1` |
| Verbatim move silently changes behaviour through object lifetimes (uniform-set cascades) | Step 0 golden on the real compositor; no stage reordering |

## 9. Pathway roadmap (all deepening candidates)

Order reflects dependency and leverage. Each is its own spec → plan cycle. Strength: **Strong**,
**Worth exploring**, **Speculative**.

### 9.1 Sub-project 2 — Render lifetime owner (Strong)

**Problem.** `RenderOrchestrator::Collaborators` (`orchestrator.h:80-117`) takes 17 pointer and
pointer-to-pointer handles, including VoxelWorld's `island_mutex`, `island_slots` and LoD page
maps; its teardown clears LoD state (`orchestrator.cpp:412-416`) and locks the world's island
mutex (`:406`). `ConsolidationCoordinator` reads the orchestrator's lifetime mutex and shutdown
flag (`consolidation.h:44-60`). `VoxelWorld` is ~60 one-line pass-throughs
(`voxel_world.h:390-461`, `voxel_world.cpp:271-317, 1064-1110`); ~216 of its 1,301 lines are
blank move debris. Hooks reach private state via `friend` (198 `store_->`, 33 `lod_pool_` reads;
`lod_system.h:130`). `IslandManager::initialize(VoxelWorld*)` calls 17 world methods.
**Deletion test:** delete the slots and teardown ordering concentrates in one owner.

**Proposal.** The orchestrator becomes the render lifetime owner: `ensure(device)`,
`teardown()`, `shutdown()`, `begin/end_callback()`, `request_reload()`, `frame()`. It owns the
island upload queue (`island_uploads_`, `pending_normal_releases_`, `island_descs_`, their mutex)
and calls `release_gpu()` on the streamer and LoD pool in order. Delete: the 25 public pass
accessors (only `VoxelFrame` and a narrow diagnostics view see passes), `Collaborators` slots,
the VoxelWorld forwarding block and the former `FrameHost` seam (deleted in commit `6b595c1`).
Then split `hooks.cpp` by module: each module
exposes a POD `stats()`, the GDScript facade formats; both `friend` declarations go.
**Tests:** a reload/teardown contract suite against the single owner.

Implemented; results in docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md.

### 9.2 Sub-project 3 — One settings store (Strong; artist-facing)

**Problem.** Beauty knobs are `if (name == ...)` chains in an anonymous namespace
(`orchestrator.cpp:516-541`), float/bool only — `ssao_steps`, `ssgi_taps`, `ssao_directions`
cannot be set by name. Grass has a proper member-pointer table with its own lock
(`grass_settings_store.cpp:9-32`). Menu ranges disagree with C++ clamps (`debug_menu.gd:38`
caps blade width 0.06, default 0.08 at `grass_settings.h:31`; `debug_menu.gd:22` caps emissive
radius 128, clamp allows 512). The menu reads beauty values through a debug hook
(`debug_menu.gd:157`). `set_quality_tier` discards every tweak (`orchestrator.cpp:605-609`).
Only tier and `near_field` persist (`settings_menu.gd:169`); nothing but `quality_tier` is in the
inspector. Look constants live outside both stores: SSAO radius/strength (`ssao_pass.cpp:19-20`),
outline 0.35 (`outline_pass.cpp:166`), contact shadow 0.6/0.85/0.05
(`contact_shadow_pass.cpp:155-158`), `DeferredPass::kAmbient` (`deferred_pass.h:14`, copied in
`island_body.cpp:194`, `main.tscn:47`, tests), sky gradient (`common.glslh:77`).

**Proposal.** A generic field table `{name, type, member, min, max, default, per-tier}` over any
settings struct; supplies `set/get(name)` for all types, `describe()` so `debug_menu.gd` builds
its rows from it (ranges cannot drift), `ConfigFile` save/load, and an optional `BeautyProfile`
Resource (`.tres`) for the inspector. Tier sets defaults, tweaks layer on top. Stray constants
move into the table. Keep beauty and grass as separate stores and structs (project memory: keep
new features out of the beauty stack) — share the mechanism, not the struct. A knob then costs:
struct field, table row, pass push, shader — ~4 files instead of 9–11.
**Tests:** native tests over the generic store (the grass tests generalised).

### 9.3 Sub-project 4 — Pass anatomy and generated layouts (Strong)

**Problem.** The shader load/compile/pipeline block (~35 lines, e.g. `ssao_pass.cpp:26-61`) is
copied into 23 pass files (`grass_scatter_pass.cpp:19`: "mirroring SsaoPass's load sequence");
~25 hand-written RID-keyed uniform-set caches (`deferred_pass.cpp:131-188`: 10 keys, 11
uniforms); ~90 hand-numbered `set_binding` calls; push constants written at hand-counted offsets
(`ssao_pass.cpp:144-153`, `ssgi_pass.cpp:170-189`) guarded by tautological asserts
(`static_assert(sizeof(float) * 28 == 112)` at `deferred_pass.cpp:226`, `ssao_pass.cpp:142`,
`ssgi_pass.cpp:169`, `beauty_camera.cpp:7`). RID caching leaks across passes
(`raymarch_compositor.cpp:225-228`).
C++↔GLSL contracts are unchecked except `material_table.glslh` (byte test) and the cel ramp
(GPU diff): bindings, push layouts, SunBlock, the 256 B cascade block declared inline
(`deferred.comp.glsl:22-31`) and built in `deferred_pass.cpp:207-223`, `MATERIAL_LAYERS` (4
`#define` copies of `kMaterialLayers`, `material_atlas.h:12`), beauty flag bits, cel constants
(`cel.h:15`, `shade.glslh:46-54`, `cel.gdshaderinc:1-11`, `test_deferred.gd:52`). The G-buffer
layout exists only as a comment (`gbuffer.h:12-17`): writers restate it (`composite.frag.glsl:10-13`,
`lod.frag.glsl:14-17`, `grass.frag.glsl:23-24`), blend arrays are hand-built in three raster
passes, readers decode by magic (`g1.z < 0.5` = surface in `ssao.comp.glsl:45`,
`ssgi.comp.glsl:133,179,198`; `uint(g1.z+0.5)` = material in `deferred.comp.glsl:165`). Material
ids are literals (`height_bands.field.glslh:13`, `generator.cpp:41,57`, `GRASS_MATERIAL = 1u` in
`grass.frag.glsl:26` / `grass_scatter.comp.glsl:43`, `flat_material_albedo(4u)` at
`raymarch.comp.glsl:680`).

**Proposal.**
(a) `ComputePass` / `RasterPass` helper: `init(rd, "ssao.comp.glsl")`, `bind(slot, rid, kind)`,
`target(slot, format, size)`, `dispatch(push, groups)`. Hides compile/pipeline, the uniform-set
cache and its free-cascade rule (retiring `invalidate_uniform_set`), teardown, timing. Target
~40 lines per simple pass. No render graph.
(b) Generated layouts from one description, extending the proven `material_table_glsl()` +
byte-exact golden pattern: push/UBO structs (with real `offsetof` asserts, packing via
`memcpy`), `BEAUTY_*` bits, `MATERIAL_LAYERS`, named `MAT_*` ids, cel constants (emitted into
both `shade.glslh` and `cel.gdshaderinc`), G-buffer channel accessors
(`GB_MATERIAL_ID(g1)`, `GB_IS_SURFACE(g1)`) and attachment count.
**Tests:** fake-RD tests for rebuild-on-RID-change and free order; golden-file tests per
generated header.
**Entry gate.** Sub-project 2 accepted (passes reachable only through `RenderPasses` (pass-level probes stay isolated by sub-project 1's classification)).
**Change cost after:** new material ~4 files (from 7–9); new G-buffer channel ~5 (from 14–18).

### 9.4 Sub-project 5 — World field query and edit spine (Strong; characterization first)

**Problem.** No module answers "the world field at a point / AABB". `eval_field(gen, ops, n, x,
y, z, volumes = nullptr, overrides = nullptr)` has ~25 call sites each assembling inputs; some
omit pieces (§10, S1–S3). The edit fan-out is split: `VoxelWorld::append_edit_locked`
(`voxel_world.cpp:694-723`) wraps `WorldStore::append_edit_locked` (`world_store.cpp:31`) and adds
LoD and collider tails; `WorldStore::append_edit` is public, unused, and carries a "tools must
not call this" warning. Consolidation repeats invalidation by hand (`consolidation.cpp:157-171`,
`554-564`); `WorldStreamer::run_frame` repeats lock/read/capture-seq/upload five times. Lock
order is restated in four headers; one re-entrancy hang is on record
(`voxel_world.cpp:836-842`). Gameplay raycast exists only as a debug hook (`hooks.cpp:6164`,
used by `demo/edit_tool.gd`). `IslandManager::land_extraction` (~380 lines,
`island_manager.cpp:577-956`) re-derives op-cap headroom because `EditLog` has no atomic batch
append. **Deletion test:** delete the probe adapters (`LogProbe`, `LogContactProbe`) and the
split `append_edit` spines — complexity concentrates in one query and one spine.

**Proposal.** `WorldField::snapshot(aabb) → FieldView` taken under the lock, with
`sample / gradient / has_surface(chunk | brick) / contact(cell, axis) / raycast(ray)`; hides pad
choice, region wrap, overrides, volumes, sequence stamp, op truncation; also produces the
snapshots GPU jobs need (`FieldSourceSnapshot`, island extract job inputs). `EditPipeline`:
`apply(ops[], policy) → Result` as an atomic batch plus `invalidate(aabb, reason)` fanned out to
registered sinks (islands, LoD, colliders, stats). Public `VoxelWorld.raycast` for gameplay.
**Tests:** native invariant "consolidated region ≡ unconsolidated region" for every query; fake
sinks recording which consumer heard which AABB.
**Precondition:** characterization tests for S1–S3 before any refactor.

### 9.5 Sub-project 6 — Stage authoring (Worth exploring; terrain artists)

**Problem.** The pipeline core (manifest parse, resolve, codegen) is deep and stays. Friction is
at its edges: C++ stage mirrors address channels by position (`extra[]`) and params by index
(`p.at(0)`) while GLSL uses names; `cave.field.glslh:15` reads `P.hills_amp_*` but its C++ mirror
hardcodes literals (warned in `default.pipeline:6`). Lipschitz bounds combine multiplicatively
(`pipeline.cpp:143`, test-locked) — wrong for additive stages, overridden by hand. The violation
check promised in the terrain spec §10.1 does not exist; `test_field_diff.gd` was never
pipeline-parameterised (§11.1). `allow_gpu_only` silently diverges CPU raycast/colliders/islands
from what renders. The four-step load sequence is duplicated (`voxel_world.cpp:579` and two
tests). `FieldGenerator` + its `View` is shallow (deletion test: delete; `PipelineFieldGenerator`
can be a `Generator`).

**Proposal.** Generate a C++ header per stage (`MesasSlots { int sdf, height; }`,
`MesasParams { float amp_a; }`); forbid or formalise cross-stage params; fix the Lipschitz
combination rule and add the sampled violation check; pipeline-parameterised field diff test plus
a native CPU test over `default.pipeline`; one `load_pipeline(reader)`; `MAT_*` constants shared
with 9.3(b). Move `AnalyticGenerator` to test-only as the equivalence oracle.

### 9.6 Deferred (Speculative — YAGNI until a second need appears)

- **Op-type registry.** One descriptor per `EditOp` type (AABB, well-formed, `changes_sdf`,
  impulse, hardness scaling, pack/unpack, GLSL constants, `debug_pack_op` for the 9 GDScript
  suites that hand-pack the 32-byte op). A new op today costs six hand-written implementations
  across two languages; a capsule brush is also blocked by `EditOp` having no second endpoint.
  Revisit when the next brush is scheduled.
- **`StreamingBudget`.** Streaming knobs live in three homes (`WorldConfig`; VoxelWorld's
  `physics_radius_m_`, `max_collider_chunks_`; `LodSystem::max_lod_pages_`); the region window
  dim is computed twice (`world_store.h:239`, `orchestrator.cpp:178`); `LodTree` copies
  `stream_radius_m` once while sun cascades read it live (`voxel_world.cpp:1090`). Revisit with
  the next view-distance change.
- **Sun consolidation.** The sun spans ~8 modules; `DeferredPass` has members `sun_ubo_` and
  `sun_light_ubo_` meaning different buffers. Largely absorbed by 9.3(b); the pure `sun_ortho` /
  `sun_cascades` stay.
- **Look-dev iteration speed.** Every shader reload tears down GPU state and re-streams the world
  (`orchestrator.cpp:587`). Revisit after 9.3, when pass rebuild is uniform.

## 10. Suspected bugs register (recorded, not fixed; each needs a failing test first)

| Id | Suspicion | Evidence |
|---|---|---|
| S1 | Collider residency probe ignores volumes and overrides; after consolidation clears a region's ops, a filled/pasted chunk can probe as surface-free and get no collider | `collider_streamer.cpp:32-43` (verified: generator + ops only); `consolidation.cpp:157` `clear_region_through`; `test_connectivity.gd:34-38` sets `max_override_bricks = 1` |
| S2 | Island contact probe omits overrides; CPU `extract_island_volume` has no override input while its GPU counterpart uses them | `island_manager.cpp:82-95`; `volume_set.cpp:382,425,431` |
| S3 | LoD chunks spanning several regions use one region's override table; `& 31` brick wrap may alias edits every 25.6 m; `gather_ops` truncates at 256 | `mesh_service.cpp:826-833`; `field_ops.glslh:249,294`; `lod_system.cpp:41` |
| S4 | Physics objects ignore the scene sun (fixed `VE_SUN_DIR`, no colour); island rock albedo hard-coded | `cel_object.gdshader:28`; `island_body.cpp:193` |
| S5 | Menu ranges contradict C++ clamps | `debug_menu.gd:22,38` vs `grass_settings.h:31` and the emissive clamp |
| S6 | `set_quality_tier` discards per-knob tweaks | `orchestrator.cpp:605-609` |
| S7 | Grass writes material 1, so making `grass_01` emissive makes blades glow | `grass.frag.glsl:26` |
| S8 | `ssao` timing scope nested inside `deferred`, so "deferred" includes SSAO time | `raymarch_compositor.cpp:433-451` |
| S9 | `static_assert` messages name the wrong files | `volume_pool.cpp:12`, `override_pool.cpp:14` |

## 11. Leave alone

Terrain `parse_stage_manifest` / `resolve_pipeline` / `field_codegen`; `field_params_pack`;
`material_table` codegen and table; `EditLog` storage; `op_world_aabb` and edit AABB/range math;
pure `sun_ortho`, `sun_cascades`, `cel`, `grass_layout`, `pack_flags`; `GBuffer`'s spec table;
`FieldContextSet`; compositor admission and lifetime locks (relocate at most); shader reload
pre-flight; `VoxelEditTool`; `register_types.cpp`; the GPU-vs-CPU cel diff tests.

---

## Appendix A — Change pathways today (traced)

File counts are what a change touches now; they are the baseline the roadmap is judged against.

| Scenario | Files | Path |
|---|---|---|
| New render pass | 12–19 | pass `.h/.cpp` + shader; `orchestrator.h/.cpp` (member, ctor slot, teardown slot); `voxel_world.h` forwarder; compositor wiring + timing; `deferred_pass.h/.cpp` + `deferred.comp.glsl` if lighting consumes it (binding + dummy fallback); `beauty_settings.h/.cpp` + orchestrator name table + `shade.glslh` flag; `hooks.h/.cpp` probe rebuilding upstream; `debug_menu.gd`; tests; `gpu_timings.cpp` + `benchmark.gd` labels |
| Beauty float knob | 9 (11 with inspector) | `beauty_settings.h/.cpp`; `orchestrator.cpp` name table; pass push packing; shader Push block; `hooks.cpp` `debug_beauty_settings`; `debug_menu.gd`; native + gdUnit tests; (+ `voxel_world.h/.cpp` for a property) |
| Beauty int knob | not settable by name | — |
| Cel / ambient constant | 7+ | `cel.h`, `shade.glslh`, `cel.gdshaderinc`, `deferred_pass.h`, `island_body.cpp`, `main.tscn`, tests |
| New material ("moss") | 7–9 (+4 shaders past 16 layers) | 512² PNGs in `assets/materials/NN_*`; `tools/convert_materials.sh`; `material_table.h`; paste regenerated `material_table.glslh`; stage GLSL + `generator.cpp` to place procedurally; past 16 layers `material_atlas.h` + 4 shaders + test. Per-material roughness/normal-strength/tint unsupported |
| New G-buffer channel | 14–18 | `gbuffer.h/.cpp`; `composite.frag.glsl` + `composite_pass.cpp` (framebuffer, 2 blend arrays, clears, marker location); `lod.frag.glsl` + `lod_raster_pass.cpp`; `grass.frag.glsl` + `grass_raster_pass.cpp`; `raymarch.comp.glsl` + `raymarch_pass.cpp`; `deferred.comp.glsl` + `deferred_pass.cpp`; post readers; `hooks.cpp` |
| New terrain stage ("mesas") | 5–7 | `shaders/stages/mesas.field.glslh`; `terrain/builtin_stages.cpp` mirror + register (C++ rebuild); `assets/pipelines/default.pipeline` (hand-budgeted Lipschitz); `shaders/generated/field.glslh.golden` (`VE_REGEN_GOLDEN`); `test_field_diff.gd`; other stages' C++ literals if params are shared; `height_bands` if it feeds material banding |
| Material with hardness, placed by terrain | ~9 | `material_table.h`; regenerated `material_table.glslh`; `convert_materials.sh`; PNGs; stage GLSL literal + C++ mirror literal; golden; grass literal. Silent risks: reordering materials breaks terrain; hardness ignores box/drill carves |
| New edit brush (capsule carve) | 12+ | blocked first by `EditOp` layout (no second endpoint); `edit_ops.h/.cpp` (enum, well-formed, AABB, apply, gradient); `common.glslh`; `field_ops.glslh` (3 functions); `voxel_edit_tool.h/.cpp`; `demo/edit_tool.gd`; `island_manager.cpp:155` impulse; sphere-only edit preview (`raymarch_compositor.cpp:172-180`); `test_edit_ops.cpp`; three GDScript op packers |
| View distance change | 2–8 | `WorldConfig` / `main.tscn`; relief stage amplitude + Lipschitz; `max_lod_pages` / `max_lod_chunk_records`; `atlas_bricks` and `max_region_slots` vs residency radius; physics radius / chunk count; tests that shrink radii |
| New debug hook | 2 (5 if it needs frame state; renders ⇒ copies the frame) | `hooks.h/.cpp`; + `voxel_world.h` + compositor (e.g. `f3bda89`) |

Edit flow today: `demo/edit_tool.gd` (target via `hooks().debug_raycast`) → `VoxelEditTool::apply`
→ `VoxelWorld::append_edit_locked` → `WorldStore::append_edit_locked` (log, consolidation queue at
192 ops, seq, `EditSink` → islands, `pending_edits_`) → back in VoxelWorld: `LodSystem::note_edit`,
collider `pending_dirty_` → render thread `WorldStreamer::run_frame` (region op upload, mark/gen) →
physics tick → `ColliderStreamer` → `MeshService` worker; islands extract and write box carves back
through `append_edit_locked`; consolidation bakes overrides into `OverrideStore`, `GpuAtlas` and
the MeshService pool.

Generation flow today: `load_terrain_pipeline` (`voxel_world.cpp:579`) reads `default.pipeline` →
stage manifests → resolve → CPU `PipelineFieldGenerator` (C++ mirrors from `builtin_stages.cpp`)
and GPU `generate_field_glslh` installed as the `field.glslh` source override, prepended to
`field_ops.glslh`. Four terrain copies exist: stage GLSL, stage C++, the fallback `field.glslh`
stub, `AnalyticGenerator`.

## Appendix B — Research

- Ousterhout, *A Philosophy of Software Design*, deep vs shallow modules —
  https://dev.to/gosukiwi/software-design-deep-modules-2on9,
  https://www.sandordargo.com/blog/2023/01/25/deep-vs-shallow-modules
- Deletion test, seams/adapters ("one adapter = hypothetical, two = real"), replace-don't-layer
  testing — https://github.com/mattpocock/skills/blob/main/skills/engineering/improve-codebase-architecture/SKILL.md
- Render graphs: setup/compile/execute and the explicit caveat that small engines rarely recoup
  the machinery — https://logins.github.io/graphics/2021/05/31/RenderGraphs.html;
  Frostbite FrameGraph — https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in
- Godot Compositor / CompositorEffect callback model —
  https://docs.godotengine.org/en/stable/tutorials/rendering/compositor.html
- godot_voxel: thin node façades over feature modules; data-driven libraries as Resources —
  https://github.com/Zylann/godot_voxel/blob/master/doc/source/overview.md
