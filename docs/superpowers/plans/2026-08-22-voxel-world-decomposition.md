# VoxelWorld Decomposition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decompose the `VoxelWorld` god object (7,091-line `.cpp`, 726-line header, ~208 methods) into owned subsystem classes behind a thin node façade, with zero behavior changes.

**Architecture:** Incremental strangler refactor per spec §5: extract debug hooks → WorldStore data plane → ConsolidationCoordinator → RenderOrchestrator → LodSystem, each behind constructor-injected collaborators in a `VoxelContext`. Mutexes move verbatim with their state; documented lock orders are restated at new owners.

**Tech Stack:** C++17 GDExtension (`godot-cpp`), SCons (`extension/SConstruct`), gdUnit4 GDScript test suite (GPU-backed, via `gdunit_tests.sh`).

**Spec:** `docs/superpowers/specs/2026-08-22-voxel-world-decomposition-design.md`

## Global Constraints

- **No behavior changes.** Moved code keeps its semantics verbatim — including quirky failure paths, refusal accounting, and comments.
- **Mutexes move verbatim** with their state: same guard scope, same acquisition sites. No locking "improvements."
- Documented lock order restated at new owner: today's `edit_mutex_` → `lod_mutex_` comment must survive at `WorldStore::mutex()` → `LodSystem::mutex()`.
- Atomics/latches move unchanged: `edit_seq_`, reload latch, compositor admission flags.
- Full suite green after EVERY task: `./build.sh && ./gdunit_tests.sh`. Native tests additionally: `./build.sh --test`.
- Godot-facing API source-compatible EXCEPT `debug_*` methods move off `VoxelWorld` to `VoxelDebugHooks` (tests updated mechanically in Task 2).
- SConstruct auto-globs `src/*.cpp` + `src/*/*.cpp`; native test sources are an explicit list — see Task 1 Step 3.
- Every task plan/commit message notes which mutexes/threads touched code interacts with.
- Work happens in this repo checkout; commit after every task.
- If hidden coupling appears that violates scope guards (spec §8): STOP, report, re-plan. Never silently expand.

## Verification Commands

```bash
./build.sh                          # build extension shared library
./build.sh --test                   # also build & run native C++ tests (ve_tests)
./build.sh --verify                 # headless-load project, check classes register
./gdunit_tests.sh                   # full gdUnit4 suite (real GPU display required)
./gdunit_tests.sh -a res://tests/test_self_check.gd   # single suite
```

Focused suites used below (pick the ones touching what you moved):
- Atlas/stream/regions: `test_gpu_atlas.gd`, `test_region_dda.gd`, `test_brick_flags_gpu.gd`
- Edits/consolidation: `test_field_diff.gd`, `test_field_volume_diff.gd`, `test_deferred.gd`, `test_repro_pillar_debris.gd`
- Occupancy/islands: `test_occupancy.gd`, `test_connectivity.gd`, `test_island_body.gd`, `test_island_extract.gd`, `test_island_render.gd`
- Physics/mesh/colliders: `test_collider_octants.gd`, `test_collider_edits.gd`, `test_mesh_lattice.gd`, `test_lod_mesh_diff.gd`, `test_player_kick.gd`
- Render probes: `test_raymarch_pixel.gd`, `test_hiz.gd`, `test_ssgi.gd`, `test_outline.gd`, `test_contact_shadow.gd`, `test_material_atlas.gd`, `test_stored_normal_pool.gd`
- LoD: `test_lod_build.gd`, `test_lod_pool.gd`, `test_lod_cull.gd`, `test_lod_mesh_diff.gd`
- Lifecycle/shutdown: `test_render_shutdown.gd`, `test_extension_boot.gd`, `test_shader_reload.gd`, `test_demo_shell.gd`

---

## File Structure (target end state)

```
extension/src/
  voxel_world.h/.cpp          Node3D façade: lifecycle, properties, wiring (< ~1000 lines)
  core/context.h              VoxelContext + WorldConfig
  core/world_store.h/.cpp     Authoritative data plane (Phase 2)
  render/orchestrator.h/.cpp  Render pass graph + lifetime + shader reload (Phase 4)
  lod/lod_system.h/.cpp       LoD runtime state (Phase 5)
  mesh/consolidation.h/.cpp   Consolidation coordinator (Phase 3)
  debug/hooks.h/.cpp          VoxelDebugHooks — all 159 debug_* methods (Phase 1)
```

---

## Phase 0 — Scaffolding

### Task 1: Create context header and empty subsystem skeletons

**Files:**
- Create: `extension/src/core/context.h`
- Create: `extension/src/core/world_store.h` (+ empty `.cpp`)
- Create: `extension/src/render/orchestrator.h` (+ empty `.cpp`)
- Create: `extension/src/lod/lod_system.h` (+ empty `.cpp`)
- Create: `extension/src/mesh/consolidation.h` (+ empty `.cpp`)
- Create: `extension/src/debug/hooks.h` (+ empty `.cpp`)
- Modify: `extension/SConstruct:16-18` (pure_sources list)

**Interfaces:**
- Produces: `namespace ve { struct WorldConfig; }` and forward declarations only — later tasks fill these in. Nothing may reference these types from existing code yet.

- [ ] **Step 1: Write the skeleton headers**

`core/context.h`:

```cpp
#pragma once
// VoxelContext — the only thing subsystems see of each other (spec §4).
// Populated incrementally by Phases 2-5; subsystems receive the collaborators
// they need via constructor injection and never hold a VoxelWorld*.
#include "world/region.h"

namespace godot {
class WorldStore;
class RenderOrchestrator;
class LodSystem;
class ConsolidationCoordinator;

struct VoxelContext {
    WorldStore *store = nullptr;
    RenderOrchestrator *render = nullptr;
    LodSystem *lod = nullptr;
    ConsolidationCoordinator *consolidation = nullptr;
};
} // namespace godot
```

Each subsystem header gets a minimal class with a comment naming its phase, e.g. `debug/hooks.h`:

```cpp
#pragma once
#include <godot_cpp/classes/object.hpp>
// VoxelDebugHooks — all debug_*/test-fixture entry points extracted from
// VoxelWorld (spec Phase 1). Registered as a Godot class so GDScript tests
// call world.hooks().debug_x(...). Holds no state of its own beyond a
// back-reference set by its owning VoxelWorld.
namespace godot {
class VoxelWorld;
class VoxelDebugHooks : public Object {
    GDCLASS(VoxelDebugHooks, Object)
public:
    void bind_world(VoxelWorld *w) { world_ = w; }
protected:
    static void _bind_methods();
private:
    VoxelWorld *world_ = nullptr;
};
} // namespace godot
```

Same pattern for `WorldStore`, `RenderOrchestrator`, `LodSystem`, `ConsolidationCoordinator` (plain C++ classes, NOT Godot objects — only `VoxelDebugHooks` extends `Object`). Each `.cpp` contains just its header include so the glob picks it up.

- [ ] **Step 2: Add core to native-test sources**

In `extension/SConstruct`, extend the `pure_sources` glob list (line ~16):

```python
pure_sources = (Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") +
                Glob("src/core/*.cpp") +
                Glob("src/mesh/*.cpp") + Glob("src/connectivity/*.cpp") +
                Glob("src/lod/*.cpp") + Glob("src/shade/*.cpp"))
```

(Do NOT add `src/debug/*.cpp` or all of `src/render/*.cpp` — they need GPU/Godot runtime.)

- [ ] **Step 3: Build and verify**

Run: `./build.sh && ./build.sh --test && ./build.sh --verify`
Expected: all three succeed; no new warnings.

- [ ] **Step 4: Commit**

```bash
git add extension/src/core extension/src/render/orchestrator.* extension/src/lod/lod_system.* extension/src/mesh/consolidation.* extension/src/debug extension/SConstruct
git commit -m "refactor: scaffold subsystem skeletons for VoxelWorld decomposition"
```

---

## Phase 1 — Debug Hooks Extraction

Mechanical forwarding only: while hook-touched state still lives on `VoxelWorld`, hook implementations call back through `world_->`. Failure-injection flags already living in consumers (`IslandManager::debug_fail_next_spawn_`, `MeshService::pause_override_publication_`, …) keep living there; their `debug_set_*` forwards through the same accessor chain VoxelWorld uses today.

### Task 2: VoxelDebugHooks registration + binding switch + mechanical test migration

This task switches ALL debug bindings to the facade at once (one breaking commit), before any implementation moves. Suites stay green because behavior is identical.

**Files:**
- Modify: `extension/src/debug/hooks.h/.cpp`
- Modify: `extension/src/voxel_world.h/.cpp` (`hooks()` accessor; `_bind_methods` loses debug registrations)
- Modify: `extension/src/register_types.cpp` (register `VoxelDebugHooks`)
- Modify: all files under `tests/` and `demo/` calling `debug_` methods

**Interfaces:**
- Produces: `VoxelWorld::hooks() -> VoxelDebugHooks*` (Godot-bound property getter). GDScript call pattern changes from `w.debug_x(args)` to `w.hooks().debug_x(args)` — uniform, sed-able.
- Consumes: nothing new.

- [ ] **Step 1: Register the class**

In `register_types.cpp`, next to `VoxelWorld`'s registration: `ClassDB::register_class<VoxelDebugHooks>();`

- [ ] **Step 2: Expose hooks() on VoxelWorld**

In `voxel_world.h` public section:

```cpp
VoxelDebugHooks *hooks();
```

In `voxel_world.cpp`: lazily construct a `VoxelDebugHooks` member `debug_hooks_` in `_ready()`/first access, call `debug_hooks_->bind_world(this)`, bind `"hooks"` via `ClassDB::bind_method(D_METHOD("hooks"), &VoxelWorld::hooks)`.

- [ ] **Step 3: Move all debug_* declarations, definitions, and bindings at once**

For each of the ~160 `debug_*` methods:
1. Cut the declaration from `voxel_world.h`, paste into `debug/hooks.h` under `VoxelDebugHooks`, signature verbatim.
2. Cut the definition from `voxel_world.cpp`, paste into `debug/hooks.cpp` as `VoxelDebugHooks::name`, body verbatim except member accesses rewritten `x` → `world_->x`.
3. Move its `ClassDB::bind_method(D_METHOD("debug_..."))` line into `VoxelDebugHooks::_bind_methods()`, retargeted `&VoxelWorld::debug_x` → `&VoxelDebugHooks::debug_x`.

This is large but purely mechanical (cut-paste + receiver rewrite); script-assisted is fine as long as every moved body is diff-reviewed against the original.

- [ ] **Step 4: Migrate callers mechanically**

```bash
# tests and demo scripts: w.debug_x( -> w.hooks().debug_x(
grep -rl "\.debug_" tests demo | xargs sed -i 's/\.debug_/.hooks().debug_/g'
grep -rl "\.debug_" tests demo   # verify none left
```

Hand-fix any non-receiver calls or multi-line wraps the sed misses (compiler errors will list them). Typed locals `var w: VoxelWorld` still work — `hooks()` returns the bound Object.

- [ ] **Step 5: Build, run full suite**

Run: `./build.sh && ./gdunit_tests.sh`
Expected: PASS — identical results to baseline.

- [ ] **Step 6: Commit**

```bash
git add -A extension/src tests demo
git commit -m "refactor: move all debug_* hooks onto VoxelDebugHooks facade (Phase 1)
Threads: unchanged; hooks run on caller threads exactly as before."
```

### Tasks 3–6: Move remaining debug state + shrink voxel_world.h

After Task 2 every debug method already lives in `debug/hooks.cpp`. What remains in these tasks: members only hooks used (e.g. `test_bodies_` if unused elsewhere — verify with grep before moving; leave anything engine code reads where it is), temporary promoted accessors audit, and header cleanup.

**Files:** `extension/src/debug/hooks.*`, `extension/src/voxel_world.h`

**Interfaces:**
- Produces: `voxel_world.h` at ~350 lines with ZERO `debug_` declarations; the temporary accessor surface is an explicit, commented block listing each promoted accessor and which phase deletes it.

- [ ] **Step 1 (Task 3): Audit member usage**

```bash
for m in $(grep -oE "\b[a-z_0-9]+_\b" extension/src/debug/hooks.cpp | sort -u); do
  echo "$m: $(grep -c "$m" extension/src/debug/hooks.cpp) hook / $(grep -rc "$m" extension/src/voxel_world.cpp) world"
done
```

Members referenced ONLY from `debug/hooks.cpp`: move the declaration into `VoxelDebugHooks` (private) and update references from `world_->x` to plain `x`. Expected candidates: `test_bodies_`, `overflow_seen_`, `last_physics_tick_ms_`, `last_hiz_readback_was_pending_/drained_` — VERIFY each with grep first; if any non-hook code reads it, leave it and note it for its phase.

- [ ] **Step 2 (Task 3): Build + focused suites + commit**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_island_body.gd,res://tests/test_hiz.gd,res://tests/test_physics_stats.gd` (whichever exist; else full suite).
Commit: `refactor: move hook-only state into VoxelDebugHooks (Phase 1)`

- [ ] **Step 3 (Task 4): Header sectioning**

Delete every moved `debug_*` declaration block from `voxel_world.h` (the `--- debug/test hooks ---` sections). Verify zero remain:

```bash
! grep -n "debug_" extension/src/voxel_world.h
wc -l extension/src/voxel_world.h   # expect ~350-420 lines
```

- [ ] **Step 4 (Task 4): Full suite + commit**

Run: `./build.sh && ./gdunit_tests.sh` → PASS.
Commit: `refactor: drop debug declarations from voxel_world.h (Phase 1 done)`

(Tasks 5–6 are spare capacity: if Steps above surface stragglers — e.g. `demo/dev_tools.gd` or `tools/` calling removed API — fix them as their own green commits.)

**Phase 1 exit criteria:** `voxel_world.h` ≤ ~420 lines, zero `debug_` symbols; full suite green.

---

## Phase 2 — WorldStore (data plane)

### Task 7: WorldStore construction + config + inert members

**Files:**
- Modify: `extension/src/core/world_store.h/.cpp`, `extension/src/core/context.h`
- Modify: `extension/src/voxel_world.h/.cpp`

**Interfaces:**
- Produces (later tasks rely on exact names):

```cpp
namespace ve {
struct WorldConfig {
    Vector3i atlas_bricks{64, 32, 32};
    int max_region_slots = 512;
    int max_brick_jobs = 16384;
    int max_override_bricks = 8192;
    Vector3i world_origin_bricks{0, -64, 0};
    Vector3i world_size_regions{64, 8, 64};
    float residency_radius_m = 96.0f;
};
}
class WorldStore {  // namespace godot
public:
    WorldStore(const ve::WorldConfig &config);
    ve::EditLog *edit_log();
    ve::OverrideStore *overrides();
    ve::VolumeSet &volumes();
    ve::RegionResidency *residency();
    const ve::WorldConfig &config() const;
    std::mutex &edit_mutex();   // THE edit mutex; lock order doc lives here (Task 8)
private:
    ve::WorldConfig config_;
    ve::EditLog *edit_log_ = nullptr;
    ve::OverrideStore *overrides_ = nullptr;
    std::map<std::tuple<int,int,int>,int> override_tables_;
    ve::VolumeSet volumes_;
    ve::RegionResidency *residency_ = nullptr;
};
```

- [ ] **Step 1: Implement WorldStore shell**

Move construction/teardown of the five inert members out of `VoxelWorld::ensure_initialized()/teardown_gpu()` VERBATIM (same allocation order — init order is load-bearing near GPU setup). `VoxelWorld` creates `WorldStore` first inside its own init and stores it in a `std::unique_ptr<WorldStore> store_` member plus `VoxelContext context_; context_.store = store_.get();`.

Property setters/getters on `VoxelWorld` (`set_atlas_bricks`, etc.) now write `store_->config_` via a pre-init-only path: keep the existing setter bodies but target the config struct; add the same post-init rejection behavior they have today (pools don't resize after creation).

- [ ] **Step 2: Rewire internal readers**

All `voxel_world.cpp` reads of `edit_log_/overrides_/volumes_/residency_` become `store_->...`. Public accessors (`edit_log()`, `volumes()`) become one-line delegations — external callers (`world_streamer.cpp`, `island_manager.cpp`, compositor files) compile UNCHANGED.

- [ ] **Step 3: Build + focused suites**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_gpu_atlas.gd,res://tests/test_field_diff.gd,res://tests/test_render_shutdown.gd` then full suite.
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git commit -am "refactor: extract WorldStore shell owning config+inert data-plane members (Phase 2a)
Mutexes: none moved yet."
```

### Task 8: Edit append path + ConsolidationSink port

**Files:** `extension/src/core/world_store.*`, `extension/src/core/context.h`, `extension/src/voxel_world.*`

**Interfaces:**
- Produces:

```cpp
// In world_store.h — notification port injected at construction (spec §5 Phase 2).
struct EditSink {                       // implemented by IslandManager wiring in Phase 3+
    virtual ~EditSink() = default;
    // called with edit_mutex_ HELD, after append_edit_locked accepts an op
    virtual void on_edit_appended(const ve::EditOp &op, bool notify_islands) = 0;
};
struct ConsolidationSink {              // initially satisfied by VoxelWorld (Task 12 retargets)
    virtual ~ConsolidationSink() = default;
    virtual bool queue_consolidation(ve::IVec3 region) = 0;   // edit_mutex_ held
};

class WorldStore {
public:
    // The spine (moved verbatim from VoxelWorld::append_edit/_locked).
    ve::EditLog::AppendResult append_edit(const ve::EditOp &op);
    ve::EditLog::AppendResult append_edit_locked(const ve::EditOp &op, bool notify_islands = true);
    int override_table_for_region(ve::IVec3 region) const;
    void set_sinks(EditSink *edits, ConsolidationSink *consolidation);
    std::mutex &edit_mutex();
private:
    std::mutex edit_mutex_;
    std::vector<PendingEdit> pending_edits_;
    EditSink *edit_sink_ = nullptr;
    ConsolidationSink *consolidation_sink_ = nullptr;
};
```

- [ ] **Step 1: Move append path verbatim**

Move `append_edit`, `append_edit_locked`, `override_table_for_region`, `pending_edits_`, `edit_mutex_` from `voxel_world.cpp` into `WorldStore`, bodies unchanged except receiver rewrites. The island-notification body inside `append_edit_locked` calls `edit_sink_->on_edit_appended(...)` instead of reaching into `island_manager_`; `VoxelWorld` provides a tiny adapter object implementing both sinks that forwards to today's logic (adapter dies in Phase 3 when IslandManager implements `EditSink` directly). `queue_consolidation`/`requeue_consolidation_locked` stay on `VoxelWorld` (they are consolidation machinery) and are reached via `ConsolidationSink`.

`PendingEdit` (struct) moves from `voxel_world.h` to `world_store.h` alongside `pending_edits_`.

Restate the lock-order comment verbatim at `WorldStore::edit_mutex()`: currently documents `edit_mutex_ -> lod_mutex_`; update the sibling name to `LodSystem::mutex()` with a TODO-free note "(LodSystem arrives in Phase 5)".

- [ ] **Step 2: Rewire callers**

`VoxelEditTool`, `IslandManager`, `WorldStreamer` call sites switch from `world->append_edit(...)` to `world->store()->append_edit(...)` OR keep the one-line delegation on VoxelWorld — prefer keeping delegations so external code compiles untouched.

- [ ] **Step 3: Build + focused suites**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_field_diff.gd,res://tests/test_repro_pillar_debris.gd,res://tests/test_collider_edits.gd` then full suite.
Expected: PASS, identical refusal counts in stats dictionaries.

- [ ] **Step 4: Commit**

```bash
git commit -am "refactor: move edit-append path into WorldStore behind EditSink/ConsolidationSink ports (Phase 2b)
Mutexes: edit_mutex_ moved verbatim; lock-order comment restated."
```

### Task 9: Occupancy cluster + edit sequence

**Files:** `extension/src/core/world_store.*`, `extension/src/voxel_world.*`

**Interfaces:**
- Produces:

```cpp
struct OccupancyBlock {                 // moved verbatim from voxel_world.h
    ve::IVec3 region{};
    int64_t seq = 0;
    std::vector<uint8_t> bytes;
};
class WorldStore {
public:
    ve::OccupancyGrid &occupancy();
    int64_t edit_seq() const;                       // atomic, memory_order_relaxed
    int64_t bump_edit_seq();                        // called by append_edit_locked
    void enqueue_occupancy_block(OccupancyBlock b); // main thread produces
    int drain_occupancy();                          // inbox -> grid, was VoxelWorld::drain_occupancy
private:
    ve::OccupancyGrid occupancy_;
    std::mutex occupancy_mutex_;
    std::vector<OccupancyBlock> occupancy_inbox_;
    std::atomic<int64_t> edit_seq_{0};
};
```

- [ ] **Step 1: Move occupancy cluster verbatim** — grid, mutex, inbox, `drain_occupancy`, `edit_seq_` atomic; the streamer's readback stamping calls `store_->enqueue_occupancy_block(...)`. Keep the long `edit_seq_` explanatory comment verbatim on the atomic.
- [ ] **Step 2: Build + focused suites** — `./build.sh && ./gdunit_tests.sh -a res://tests/test_occupancy.gd,res://tests/test_connectivity.gd` then full suite. PASS.
- [ ] **Step 3: Commit** — `git commit -am "refactor: move occupancy grid+inbox and edit_seq into WorldStore (Phase 2c)\nMutexes: occupancy_mutex_ moved verbatim."`

### Task 10: FieldGenerator port + Phase-2 smoke test

**Files:**
- Modify: `extension/src/core/world_store.*`, `extension/src/generator/*` (read-mostly), `extension/src/voxel_world.*`
- Test: `tests/test_world_store_contract.gd` (new)

**Interfaces:**
- Produces (the seam future worldgen plugs into — spec §4):

```cpp
// generator/field_generator.h
namespace ve {
class FieldGenerator {                  // abstract; procedural G() is the first impl
public:
    virtual ~FieldGenerator() = default;
    virtual void eval(/* same signature as the current generator entry point */) = 0;
};
}
```

Wrap the existing generator call sites (CPU mirror + GPU dispatch setup) behind this interface without changing either implementation. `WorldStore` holds `FieldGenerator *generator_` injected at construction; `VoxelWorld::set_generator(...)` allows swapping pre-init (used by future worldgen features).

- [ ] **Step 1: Extract interface, adapt existing generator** (no behavior change; the concrete class delegates to today's functions).
- [ ] **Step 2: Write the contract smoke test**

```gdscript
extends GdUnitTestSuite
## Contract: accepted edits bump edit_seq monotonically across the WorldStore boundary.

func test_append_bumps_edit_seq_monotonically() -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	add_child(w)
	w.ensure_initialized()
	var before: int = w.edit_seq()
	var op := sphere_subtract_op()   # copy the op-construction helper verbatim
	                                  # from an existing suite (test_field_diff.gd)
	var result: ve.EditLog.AppendResult = w.append_edit(op)
	assert_that(result).is_not_null()
	assert_int(w.edit_seq()).is_greater(before)
	w.queue_free()
```

Expose `int64_t VoxelWorld::edit_seq()` as a one-line delegation to `store_` (Godot-bound) for this test. Copy the op-construction helper verbatim from whichever existing suite builds ops (e.g. `test_field_diff.gd`).

- [ ] **Step 3: Run new test (expect PASS) + full suite**
- [ ] **Step 4: Commit** — `git commit -am "feat: FieldGenerator port + WorldStore contract smoke test (Phase 2 done)"`

**Phase 2 exit criteria:** edit/occupancy/config state owned by `WorldStore`; `FieldGenerator` seam exists; full suite green.

Phases 3, 4, 5 each depend on Phase 2 and are mutually independent — safe to run in parallel or any order.

---

## Phase 3 — ConsolidationCoordinator

### Task 11: Extract consolidation machinery

**Files:**
- Create/modify: `extension/src/mesh/consolidation.h/.cpp`, `extension/src/voxel_world.*`, `extension/src/core/context.h`

**Interfaces:**
- Produces:

```cpp
class ConsolidationCoordinator : public ConsolidationSink {   // satisfies the port from Task 8
public:
    explicit ConsolidationCoordinator(WorldStore *store, /* mesher/atlas collaborators */);
    void pump();                    // was VoxelWorld::pump_consolidation
    void pump_async();              // was debug_pump_consolidation_async path
    bool force_region(ve::IVec3);   // was debug_consolidate_region
    void wait();                    // was debug_wait_consolidation
    // stats accessors used by HUD/benchmark via world->consolidation_stats()
private:
    // ALL 15 consolidation_* members moved verbatim:
    // consolidation_queue_, consolidation_in_flight_, consolidation_job_,
    // consolidation_table_, consolidation_old_table_, consolidation_old_entries_,
    // consolidation_entries_, consolidation_old_slots_, consolidation_old_bricks_,
    // consolidation_newly_acquired_, consolidation_slots_, consolidation_baked_,
    // consolidation_publish_in_flight_, consolidation_count_, consolidation_refusals_,
    // consolidation_queue_refusals_, consolidation_queue_refusal_logged_
};
```

- [ ] **Step 1: Move class + members verbatim.** Worker-thread ownership rule preserved (comment travels): worker owns the bake; coordinator owns the queue and publishes transactions between frames. `WorldStore::set_sinks(edits, coordinator)` replaces the VoxelWorld adapter's consolidation half; delete the adapter's `ConsolidationSink` impl.
- [ ] **Step 2: Rewire debug_pump_consolidation*/wait/force** — these became hooks→coordinator calls in Phase 1 via world; now they go `hooks->world->context().consolidation`.
- [ ] **Step 3: Build + focused suites** — `-a res://tests/test_deferred.gd,res://tests/test_field_volume_diff.gd` then full suite. PASS; `debug_perf_stats` consolidation timings unchanged.
- [ ] **Step 3b (spec §7): Phase-3 contract smoke test** — extend `tests/test_world_store_contract.gd` with `test_consolidation_refusal_accounting()`: force a consolidation on a region (via the hooks facade used by `test_deferred.gd`), assert refusal/success counters surface through stats with unchanged semantics. Run it + full suite.
- [ ] **Step 4: Commit** — `git commit -am "refactor: extract ConsolidationCoordinator satisfying ConsolidationSink (Phase 3)"`

---

## Phase 4 — RenderOrchestrator

### Task 12: Pass graph + device ownership

**Files:**
- Modify: `extension/src/render/orchestrator.*`, `extension/src/voxel_world.*`, `extension/src/raymarch_compositor.*`, `extension/src/beauty_compositor.*`

**Interfaces:**
- Produces:

```cpp
class RenderOrchestrator {
public:
    RenderingDevice *rd() const;             // local or main per use_local_device_
    GpuAtlas *atlas(); MaterialAtlas *materials(); IslandAtlas *islands();
    // one accessor per moved pass pointer (30): raymarch_pass(), gbuffer(), hiz_pass(),
    // ssgi_pass(), ssr_pass(), outline_pass(), composite_pass(), deferred_pass(),
    // sun_shadow_pass(), inject_pass(), lod_raster_pass(), lod_cull_pass(),
    // beauty_camera(), contact_shadow_pass(), island_cull(), gpu_timings(), ...
    bool try_begin_render_callback();
    void end_render_callback();
private:
    // moved verbatim: atlas_, materials_, islands_, all pass pointers, downsample RIDs,
    // prev_view_proj_[16], has_history_, beauty_frame_, normal_roughness_state_,
    // main_rd_, local_rd_
};
```

- [ ] **Step 1: Move pass pointers + rd() + atlas accessors verbatim**, including `ensure_initialized` GPU-half ordering. External callers (`world_streamer.cpp`, passes, compositor files) switch from `world->atlas()` to whatever VoxelWorld one-line delegations provide — external code compiles unchanged.
- [ ] **Step 2: Build + focused suites** — render probe suites + `test_extension_boot.gd` then full suite. PASS.
- [ ] **Step 3: Commit** — `git commit -am "refactor: move GPU pass graph into RenderOrchestrator (Phase 4a)"`

### Task 13: Lifetime/admission + teardown block

Move AS ONE UNMODIFIED BLOCK (spec §5 Phase 4): `render_lifetime_mutex_`, `render_lifetime_cv_`, `render_shutting_down_`, `render_teardown_deferred_`, `render_callbacks_`, `gpu_teardown_cv_`, `gpu_teardown_done_`, `try_begin_render_callback/end_render_callback`, `shutdown_render_resources[_on_render_thread]`, `teardown_gpu`, `teardown_downsample`, `ensure_downsample_set`, `downsample_history`, `initialize_downsample`, `preflight_shaders`. The CPU-outlives-GPU invariant comment travels with it.

Then remove ALL `friend` declarations from `voxel_world.h`; `RaymarchCompositor`/`BeautyCompositor` and the free admission functions take `RenderOrchestrator*` (or `VoxelContext*`) obtained via the world instead of friendship.

- [ ] **Step 1: Move lifetime block; rewire compositor entry points to context**
- [ ] **Step 2: Build + suites** — `test_render_shutdown.gd`, `test_shader_reload.gd`, `test_demo_shell.gd` then full suite. PASS (shutdown/reinit cycles are the risk here — run shutdown suite twice if flaky suspicion arises, but do NOT modify timing code).
- [ ] **Step 3: Commit** — `git commit -am "refactor: move render lifetime/admission into orchestrator; drop friend couplings (Phase 4b)"`

### Task 14: Shader reload + beauty snapshot + timings

Move: `reload_requested_`, `reload_mutex_`, `reload_count_`, `reload_last_ok_`, `reload_last_error_`, `request_shader_reload/pump_shader_reload`, `beauty_mutex_`, `quality_tier_`, `beauty_`, `beauty_settings()` snapshot pattern (comment travels: setters main-thread; callbacks take value copies), `gpu_timings_`, `set_effect_enabled/get_effect_enabled/set_quality_tier/get_quality_tier`. Debug variants (`debug_shader_reload_stats`, `debug_beauty_*`) were already hooks; repoint them at the orchestrator.

- [ ] **Step 3b (spec §7): Phase-4 contract smoke test** — extend `tests/test_world_store_contract.gd` with `test_render_callback_admission_shuts_down_cleanly()`: begin a render callback, run `shutdown_render_resources()`, assert admission refuses afterward and re-init re-admits (mirrors what `test_render_shutdown.gd` proves end-to-end, now pinned to the orchestrator boundary). Run it + full suite.
- [ ] **Step 4: Commit** — `refactor: move shader-reload + beauty settings + timings into orchestrator (Phase 4 done)`

---

## Phase 5 — LodSystem

### Task 15: Extract LoD runtime

**Files:**
- Modify: `extension/src/lod/lod_system.*`, `extension/src/voxel_world.*`

**Interfaces:**
- Produces:

```cpp
class LodSystem {
public:
    explicit LodSystem(WorldStore *store, /* pool/raster collaborators */);
    std::mutex &mutex();      // THE lod mutex — carries the verbatim lock-order comment:
                              // "Lock order is WorldStore::edit_mutex() -> LodSystem::mutex():
                              //  lod_tick never holds lod_mutex_ across gather_lod_ops..."
    void tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ);   // was lod_tick
    void prepare_raster(); void prepare_shadow_raster();
    void fade_band(float *start, float *end) const;
    void gather_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out); // uses store_
private:
    // moved verbatim: lod_tree_, lod_pool_, lod_frame_, lod_walk_, lod_pages_of_,
    // lod_page_quads_, lod_overflow_logged_, lod_pressure_, ensure_lod()
};
```

- [ ] **Step 1: Move state + tick/fade/gather verbatim.** `gather_lod_ops` reads region edits through `WorldStore`'s public API (it took `edit_mutex_` internally — now takes `store_->edit_mutex()`; verify order comment matches reality: tick does NOT hold lod mutex during gather).
- [ ] **Step 2: Build + suites** — all four LoD suites + `test_seam_probe` users, then full suite. PASS.
- [ ] **Step 3b (spec §7): Phase-5 contract smoke test** — extend `tests/test_world_store_contract.gd` with `test_lod_fade_band_is_single_source_of_truth()`: after streaming settles, assert `debug_lod_fade_band()` equals `LodSystem::fade_band()` output and both endpoints satisfy `start < end` (the invariant the M5 seam tests depend on).
- [ ] **Step 4: Commit** — `git commit -am "refactor: extract LodSystem carrying lod mutex + walk state (Phase 5 done)"`

---

## Final Task 16: Cleanup + verification

- [ ] **Step 1:** Delete the temporary promoted-accessor block from `voxel_world.h` (every accessor whose only remaining callers were moved code). `! grep -c friend extension/src/voxel_world.h` → 0 friends.
- [ ] **Step 2:** Line counts vs baseline: `wc -l extension/src/voxel_world.{h,cpp}` — expect h ≈ 300–400, cpp ≈ 800–1200. Record in commit message.
- [ ] **Step 3:** `./build.sh && ./build.sh --test && ./build.sh --verify && ./gdunit_tests.sh` — everything green.
- [ ] **Step 4:** Update `docs/PORTFOLIO.md` architecture blurb if it references monolithic VoxelWorld.
- [ ] **Step 5:** Commit — `refactor: decomposition complete — VoxelWorld is a thin façade`
