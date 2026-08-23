# Voxel Everything — VoxelWorld Decomposition Design

**Date:** 2026-08-22
**Status:** Approved design, pre-implementation
**Scope:** Decompose the `VoxelWorld` god object (7,091-line `.cpp`, 726-line header,
~208 methods) into owned subsystem classes behind a thin node façade. Refactor only —
no behavior changes.

---

## 1. Problem

The codebase is otherwise well-modularized (`render/`, `lod/`, `mesh/`, `world/`,
`physics/`, `connectivity/`, `generator/`, `shade/`), but every module reaches into a
single hub: `VoxelWorld` simultaneously acts as:

1. Config holder (~20 exposed properties)
2. GPU render orchestrator (~30 raw pass pointers: `RaymarchPass`, `GBuffer`, `HiZ`,
   `SSGI`, `SSR`, …), downsample pipelines, compositor wiring
3. World data owner (`EditLog`, `OverrideStore`, `VolumeSet`, residency, occupancy)
4. Consolidation state machine (~15 `consolidation_*` members)
5. LoD subsystem (own mutex, tree, walk results, page maps)
6. Physics/island coordinator (`IslandManager`, colliders, uploads, test bodies)
7. Render lifetime/teardown manager (condition variables, shutdown latches)
8. Test fixture — **160 `debug_*` methods**, called by 59 gdUnit test files
9. Shader hot-reload, beauty settings, `friend`-coupled compositor free functions

Module boundaries under `src/` are cosmetic: the real dependency graph all flows
through the hub via accessor soup (`world->atlas()`, `world->raymarch_pass()`, …).

## 2. Prior art (researched)

- **Zylann's `godot_voxel`:** feature folders (`storage/`, `streams/`, `generators/`,
  `meshers/`, `modifiers/`, `terrain/`) with thin top-level node classes that delegate.
- **Luanti:** strict separation of map/storage, simulation, networking, rendering;
  coordinator objects delegate rather than implement.
- **Godot best practices:** feature-based organization; scene-node classes are façades.

Consistent pattern: the node class is a thin façade; each subsystem owns its own state,
threads, and mutexes; cross-subsystem communication goes through narrow interfaces.

## 3. Decided constraints

| Decision | Choice |
|---|---|
| Approach | Incremental strangler decomposition (Approach A) |
| Primary goal | Maintainability for continued development |
| Safety net | Full gdUnit suite green after **every task** (suite is green today) |
| Execution | Subagents; phases are task sets, not schedules |
| Next features to serve | Rendering/beauty work; then world generation, edit shape/brush, material definition |
| Debug/test hooks | Move to a separate test-facade class |

## 4. Target architecture

New layout under `extension/src/`:

```
voxel_world.h/.cpp        Node3D façade: lifecycle, properties, wiring (< ~1000 lines)
core/context.h            VoxelContext struct — the only thing subsystems see of each other
core/world_store.h/.cpp   Authoritative data plane
render/orchestrator.*     Render pass graph + lifetime + shader reload
lod/lod_system.*          LoD runtime state
mesh/consolidation.*      Consolidation coordinator
debug/hooks.*             All debug_*/test-fixture hooks
```

### Wiring pattern

Constructor injection with a context struct:

```cpp
// core/context.h — the only thing subsystems see of each other
struct VoxelContext {
    WorldStore *store;
    RenderOrchestrator *render;
    LodSystem *lod;
    ConsolidationCoordinator *consolidation;
    // config snapshot struct instead of reaching back into VoxelWorld
};
```

- Subsystems are constructed by `VoxelWorld::_ready()`/init with the collaborators they
  need. **No subsystem ever holds a `VoxelWorld*`.**
- `VoxelWorld` keeps its Godot-exposed API as one-line delegations.
- Config properties move into an immutable-after-init `WorldConfig` struct; setters on
  `VoxelWorld` write it pre-init and are reflected/rejected after init (same behavior as
  today's never-resizing pools).
- The three compositor entry points (`RaymarchCompositor`, `BeautyCompositor`, free
  callback functions) receive a stable `VoxelContext*`; all `friend` declarations on
  `VoxelWorld` disappear.

### Named seams for near-future features (seams only — not implemented here)

- **World generation** → implements the `FieldGenerator` port extracted in Phase 2
  (today's procedural function becomes one implementation; variants plug in at init).
- **Edit shape/brush** → new `EditOp` types and brush logic live in an `editing/`
  module talking only to `WorldStore::append_edit`; consolidation handles op overflow.
- **Material definition** → a `MaterialLibrary` (id → name/params/palette slots/normal
  layers) owned outside the render orchestrator, registered before init, consumed by
  mesher and raymarcher through `MaterialAtlas`.

## 5. Extraction phases

Each phase is a set of independent subagent tasks; each task ends with the full gdUnit
suite green plus a commit. Dependency order: Phase 1 runs alone after Phase 0 (it
redefines the whole test surface); Phases 2–5 all consume its facade pattern. Phases
3, 4, and 5 each depend on Phase 2's `WorldStore` boundary but are mutually independent
(disjoint state), so they can run in parallel or any order once Phase 2 lands.

| Phase | Extracts | Future feature served |
|---|---|---|
| 0 | Baseline + `core/context.h` scaffolding | all |
| 1 | `debug/hooks.*` (all 160 debug methods + fixture state: `test_bodies_`, failure-injection latches) | all |
| 2 | `WorldStore` data plane **+ ports**: `FieldGenerator`, `EditSink` notification | world generation & edit brushes |
| 3 | `ConsolidationCoordinator` | edit brushes |
| 4 | `RenderOrchestrator` | rendering/beauty work (next focus) |
| 5 | `LodSystem` | — |

Phase details:

**Phase 1 — debug hooks first (mechanical win).** While hook-touched state still lives
on `VoxelWorld`, hooks take `VoxelWorld&` through a temporary narrow accessor surface
(~15 promoted accessors, deleted as later phases move state out). Tests change
mechanically (`world.debug_x(...)` → facade call; a small GDScript helper keeps churn
low). Exit: `voxel_world.h` shrinks to ~350 lines; no debug methods remain on the node.

**Phase 2 — `WorldStore` (highest care).** Moves `edit_log_`, `overrides_`,
`override_tables_`, `volumes_`, `residency_`, `occupancy_` + inbox/mutex,
`pending_edits_`, `edit_seq_`, `append_edit_locked`, `drain_occupancy`. Defines two
notification ports injected at construction instead of reaching back into the world:
`EditSink` (islands) and dirty-mark callbacks. Lock discipline moves verbatim;
documented lock order restated on the new owner.

**Phase 3 — consolidation.** All 15 `consolidation_*` members plus pump/queue/publish
logic. Depends on `WorldStore` only through its public API — proof that Phase 2's
boundary is real.

**Phase 4 — render orchestration.** Moves the ~30 pass pointers, downsample pipeline,
compositor callback admission/lifetime (`render_lifetime_*`, `gpu_teardown_*`), shader
reload machinery, beauty settings snapshot, GPU timings. Teardown ordering (CPU cores
outliving GPU objects) preserved by moving the lifetime code as an unmodified block.

**Phase 5 — LoD.** `lod_mutex_`, tree/walk/pages/pool wiring, `lod_tick`, fade-band
logic. Takes `WorldStore` for op gathering.

Final state: `voxel_world.cpp` ≈ lifecycle + property delegation + construction.

## 6. Threading & locking rules (non-negotiable)

1. Mutexes move with their state, verbatim — same guard scope, same acquisition sites.
   No locking "improvements" during a move.
2. Documented lock order restated at the new owner: `edit_mutex_` → `lod_mutex_`
   becomes `WorldStore::mutex()` → `LodSystem::mutex()`, with the why-comment intact
   (lod_tick never holds lod_mutex_ across gather ops).
3. Atomic/latch semantics unchanged: `edit_seq_`, reload latch, compositor admission
   flags move as-is.
4. Render-thread handoff queues (`island_uploads_`, occupancy inbox, pending normal
   releases) keep their main-thread-produces / render-thread-drains shape; ownership
   transfers to the producing side's subsystem, with a narrow `drain_*` for the reader.
5. Every subagent task plan must list which mutexes/threads its moved code touches.

## 7. Testing strategy

- Full gdUnit suite passes after every task. Canonical commands: `build.sh`,
  `gdunit_tests.sh`.
- Tests migrate mechanically in Phase 1; no behavioral test changes elsewhere.
- One new smoke test per phase asserting the extracted subsystem's public contract
  (e.g., append → occupancy → seq ordering), so regressions localize to the new unit.
- No new tests of existing behavior during the refactor.

## 8. Scope guards (explicitly out of scope)

1. No behavior changes — including quirky failure-injection paths and consolidation
   refusal accounting.
2. No locking redesign; optimizations come later, on stable boundaries.
3. No implementation of worldgen / brushes / material library — only their named seams.
   Each is its own future spec → plan cycle.
4. No GDScript/demo restructuring beyond rerouting migrated debug-hook calls.
5. No pass-graph redesign inside `RenderOrchestrator`; it moves as an owned block.
6. Godot-facing API stays source-compatible except removed `debug_*` methods.

If anything outside these guards proves load-bearing mid-refactor, stop and upgrade
scope explicitly rather than silently expanding.

## 9. Risks

- Threaded code means tests can pass while a subtle lock-order regression lurks; rule
  5 (mutex/thread inventory per task plan) and verbatim moves mitigate this.
- Temporary accessor surface in Phase 1 could ossify; mitigated by explicitly deleting
  each promoted accessor as its state moves in Phases 2–5 (tracked in the plan).
- Hidden coupling discovered mid-extraction upgrades the affected task's scope — stop,
  say so, re-plan; never silently expand.
