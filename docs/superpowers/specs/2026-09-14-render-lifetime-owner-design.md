# Voxel Everything — Render Lifetime Owner (Sub-project 2)

**Date:** 2026-09-14
**Status:** Approved design, pre-implementation
**Roadmap:** `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.1 and the pathway in
`docs/superpowers/plans/2026-09-13-frame-module.md` ("Sub-project 2 — Render lifetime owner").
**Scope:** the unfinished sub-project 1 tasks (10–12), a render lifetime contract suite, roadmap
milestones 1–5 and suspected bug S8. One spec, one plan.

Vocabulary as in the roadmap spec (Ousterhout; `CONTEXT.md` for domain nouns).

---

## 1. Problem

Sub-project 1 gave the frame a module, but render lifetime still has no single owner:

- `RenderOrchestrator::Collaborators` (`orchestrator.h:80-117`) carries 19 fields, most of them
  addresses of other objects' state: VoxelWorld's `island_mutex_`/`island_slots_`/`streamer_`/
  `initialized_`, LodSystem's pool/tree/three page maps, two atomics, a thunk. `teardown_gpu()`
  (`orchestrator.cpp:388-419`) mutates all of it through those addresses.
- `VoxelFrame` depends on `FrameHost` (`frame.h:43-52`), a one-adapter seam accepted in sub-project
  1 as debt named for deletion here.
- `VoxelWorld` (`voxel_world.h`, 535 lines) is ~40 C++-only forwarders to passes, LoD and atlas,
  plus the island handoff queue (`voxel_world.cpp:915-1033`), plus render knobs.
- `island_mutex_` has two unrelated jobs: it guards the main→render upload queue, and it guards
  the `island_manager_` pointer the render thread dereferences in `island_slot_count()`
  (`voxel_world.cpp:750-758`). That second job is why manager create/detach take it
  (`:813`, `:842`) and why a re-entry hang was once possible (`:836`).
- `IslandManager::initialize(VoxelWorld*)` calls 17 world methods.
- `VoxelDebugHooks` is a `friend` of `VoxelWorld` and `LodSystem`, reaching 23 private world
  members (183 `store_`, 63 `mesh_`, 45 `colliders_`, 35 `initialized_`, ...) and 6 LoD privates
  (33 `lod_pool_`), all inside one 5,977-line `hooks.cpp`.

Sub-project 1 was merged with Tasks 10–12 undone: `debug_seam_probe` (`hooks.cpp:1360`) still
rebuilds the frame by hand; `note_lod_cull_debug` and its atomics, `set_beauty_compositor` and
`render_probe_pixel` survive; no acceptance report exists.

## 2. Decided constraints

| Decision | Choice |
|---|---|
| Unfinished sub-project 1 tasks | Folded into this sub-project as its first step; SP1 acceptance items become a section of this sub-project's results report |
| Spec/plan cut | One spec, one plan |
| GDScript API | Byte-identical. `VoxelWorld`'s 62 bound methods and `VoxelDebugHooks`' 175 bound methods keep their names, signatures and Dictionary keys; bodies may become one-liners |
| Pass access | One public `RenderPasses` table on the orchestrator (approach A). Rejected: private passes with friends/views (moves `friend VoxelDebugHooks` rather than deleting it; 80 hook functions drive passes, so a read-only diagnostics view cannot serve them) and frame-brokered access (extra hop, makes the frame an access broker for tests that deliberately do not use it) |
| Locking | `island_mutex_` holds that protected only the manager pointer or the slot high-water int are removed (four sites, §3.2); no lock is added; the handoff mutex is a leaf |
| S8 | In scope: failing test on marker order, then a `fix:` commit with `benchmark.gd` labels |
| Leak measurement | Leak warnings from freeing a local `RenderingDevice`, captured by a `Logger`. The RD allocation counters read 0 and `get_memory_usage` never decreases on this machine (Metal, Apple M1; verified 2026-09-14), so "resource counts before/after" is not measurable. Main-device leaks are unmeasurable here and are not asserted |

## 3. Target design

### 3.1 Ownership map (end state)

**`VoxelWorld` — node façade.** Keeps: the GDScript bindings; CPU-side init ordering
(`ensure_initialized`, `ensure_physics_initialized`, `teardown_physics`); physics objects
(`mesh_`, `chunks_`, `colliders_`, `island_manager_`, `test_bodies_`); the `EditSink` adapter and
`append_edit_locked` (sub-project 5 owns the edit spine); ownership of `store_`, `lod_`, `render_`,
`consolidation_`. Exposes `RenderOrchestrator &render()`, `LodSystem &lod()`,
`WorldStore &store()`, public physics accessors (§3.5) and `WorldStats stats()`.

Loses: the C++-only pass/LoD/atlas forwarders; `FrameHost`; `island_uploads_`,
`pending_normal_releases_`, `island_descs_`, `island_descs_dirty_`, `island_slots_`,
`island_mutex_`; `streamer_`; `initialized_`; `normal_pool_bytes_`; both hiz-readback flags;
`frame_`; the render knobs `sun_state_` + `sun_mutex_`, `near_field_scale_`,
`near_field_enabled_`, `islands_enabled_`, `sun_cascade_min_level_` (bound setters/getters forward);
`beauty_compositor_`/`set_beauty_compositor`; `note_lod_cull_debug` and its three atomics;
`render_probe_pixel`; `analytic_raycast_down`; the six island-queue methods; `friend class
VoxelDebugHooks`. Header ≤ 250 lines (bound bodies move to `.cpp` where needed).

**`RenderOrchestrator` — render lifetime owner.** Existing devices, admission/lifetime, reload
and beauty-settings code is unchanged. Gains:

- `RenderPasses passes_` (§3.3) replacing the 25 pass members and accessors.
- `WorldStreamer *streamer_`, owned outright (today created inside `ensure_gpu_graph` into a
  VoxelWorld slot). `ConsolidationCoordinator::Collaborators::streamer` takes the orchestrator's
  slot, exactly as `atlas` does today; nothing else in consolidation changes.
- `IslandHandoff handoff_` (§3.2) and `drain_island_uploads(rd)` (today's body verbatim).
- `VoxelFrame frame_`, constructed as `VoxelFrame(RenderOrchestrator&, LodSystem&, WorldStore&)`;
  compositors call `world->render().frame()`.
- Frame settings storage: the render knobs listed above, each guarded exactly as today (atomics
  stay atomics, the sun keeps its own mutex, acquired in the same functions). `VoxelFrame` reads
  `render_.frame_settings()`.
- `initialized` flag and the hiz-readback end state (read by hooks through getters).

`Collaborators` shrinks from 19 fields to 5:

```cpp
struct Collaborators {
    const bool *use_local_device;          // stays a VoxelWorld property
    WorldStore *store;
    LodSystem *lod;                        // teardown calls lod->release_gpu()
    Object *callback_owner;                // Callable target for render-thread teardown
    std::function<void()> ensure_initialized; // replaces thunk + self
};
```

`teardown_gpu()` keeps its statement order; the LoD block becomes `handles_.lod->release_gpu()` at
the same position and the island high-water reset becomes `handoff_.reset_debug_slots()`.

**`LodSystem`.** Gains `release_gpu()` — today's five teardown lines verbatim (pool teardown, tree
clear, three map clears), taking no lock, as today. Gains `LodStats stats() const` (§3.5). Its
five slot accessors (`pool_slot()`, `tree_slot()`, `pages_of_slot()`, `page_quads_slot()`,
`overflow_logged_slot()`) are deleted once unused. Reads passes through `render()->passes()`.
Its `near_field_enabled` collaborator re-points at the orchestrator's atomic (the atomic moves;
the read at `lod_system.cpp:64` is unchanged). Loses `friend class VoxelDebugHooks`.

**`WorldStreamer`.** No new method: its drain/delete lines in `teardown_gpu()` already sit in the
owner that now owns it.

### 3.2 `IslandHandoff` (milestone 1)

`extension/src/render/island_handoff.{h,cpp}` — pure (no godot-cpp), in the native test build.
`IslandSlotDesc` moves from `render/island_atlas.h` to a pure header so it compiles natively.

```cpp
class IslandHandoff {
public:
    struct Upload { int atlas_slot = -1; int volume_slot = -1; bool to_island_atlas = false;
                    ve::VolumeData data; };
    struct Batch { std::vector<Upload> uploads; std::vector<int> normal_releases;
                   std::vector<IslandSlotDesc> descs; bool descs_dirty = false; };

    void queue_island(int atlas_slot, int volume_slot, const ve::VolumeData &d);
    void queue_field_volume(int volume_slot, const ve::VolumeData &d);
    void discard_field_volume(int volume_slot);   // only !to_island_atlas uploads for the slot
    void queue_normal_release(int volume_slot);
    void publish_descriptors(std::vector<IslandSlotDesc> d);
    Batch take();                                 // render thread; clears descs_dirty
    // teardown_physics: keep field-volume uploads whose slot is pinned, drop the rest, clear descs.
    void drop_for_physics_teardown(const ve::VolumeSet &volumes);

    // Slot high-water marks. Atomics: no lock on the render thread.
    std::atomic<int> manager_slots{0};  // IslandManager::slot_high_water_ moves here
    void note_debug_slot(int slot);     // max-latch; today's island_slots_ poke
    void reset_debug_slots();           // teardown_gpu
    int slot_count(bool islands_enabled) const; // enabled ? max(debug, manager) : 0

    // Diagnostics for hooks.
    int pending_uploads() const;
    bool descs_dirty() const;
    int field_volume_uploads() const;   // today's debug_field_volume_upload_count_
    void note_field_volume_uploaded();
private:
    mutable std::mutex mutex_;          // LEAF: never held while calling out or locking
    std::vector<Upload> uploads_;               // was VoxelWorld::island_uploads_
    std::vector<int> normal_releases_;          // was pending_normal_releases_
    std::vector<IslandSlotDesc> descs_;         // was island_descs_
    bool descs_dirty_ = false;                  // was island_descs_dirty_
    std::atomic<int> debug_slots_{0};           // was island_slots_
    std::atomic<int> field_volume_uploads_{0};  // was debug_field_volume_upload_count_
};

bool release_volume_slot(ve::VolumeSet &volumes, IslandHandoff &handoff, int slot);
```

**Locking, stated.**

1. `island_slot_count()` reads the handoff's two atomics. The render thread no longer
   dereferences `island_manager_`.
2. `IslandManager` stores into `handoff.manager_slots` where it bumps `slot_high_water_` today
   (`island_manager.h:68`, `island_manager.cpp:641`) and resets it in `initialize`
   (`island_manager.cpp:109`). `teardown_physics` stores 0 at the moment it detaches the manager —
   what the render thread saw from a null pointer before.
3. Four `island_mutex_` holds are deleted: around manager creation (`voxel_world.cpp:813`) and
   detach (`:842`), whose only purpose was the render thread's pointer read; around the
   high-water reset in `teardown_gpu()` (`orchestrator.cpp:403-408`) and the hook's high-water
   poke (`hooks.cpp:2964`), which become atomic stores. `edit_mutex` holds at the create/detach
   sites are unchanged (they protect the tool thread).
4. The handoff mutex is acquired only inside `IslandHandoff` methods, called from today's sites:
   queue/publish/discard/release (main thread), `take` (render thread), physics-teardown filter,
   the hook pokes and diagnostics. The header lists these call sites. Lock order becomes
   `edit_mutex → (handoff leaf)`.

### 3.3 `RenderPasses` (milestone 3)

```cpp
struct RenderPasses {   // pass graph objects; null = not built
    GpuAtlas *atlas; MaterialAtlas *materials; IslandAtlas *islands; IslandCullPass *island_cull;
    RegionPass *region; BrickGenPass *gen; RaymarchPass *raymarch; CompositePass *composite;
    DeferredPass *deferred; SunShadowPass *sun_shadow; SunUbo *sun_ubo;
    FieldContextSet *field_context; InjectPass *inject; LodRasterPass *lod_raster;
    LodCullPass *lod_cull; HizPass *hiz; GBuffer *gbuffer; CameraUbo *beauty_camera;
    ContactShadowPass *contact_shadow; SsgiPass *ssgi; SsaoPass *ssao; SsrPass *ssr;
    OutlinePass *outline; GrassScatterPass *grass_scatter; GrassRasterPass *grass_raster;
};
const RenderPasses &RenderOrchestrator::passes() const;
```

`ensure_gpu_graph()` and the `teardown_*()` halves assign and delete the same objects in the same
statement order. `atlas_slot()` returns `&passes_.atlas`. Consumers: `VoxelFrame`, `LodSystem`,
render hooks, `VoxelWorld::publish_sun_state_to_local_device`.

### 3.4 `IslandManager` collaborators (milestone 5)

`initialize(VoxelWorld*)` is replaced by `initialize(Collaborators)`:

```cpp
struct Collaborators {
    WorldStore *store;       // edit_log, edit_mutex, edit_seq, occupancy, volumes,
                             // override_table_for_region, snapshot_field_sources, raycast_down
    IslandHandoff *handoff;  // queue_island, publish_descriptors, manager_slots, normal releases
    MeshService *mesh;       // plain pointer: created before the manager, deleted after it
    Node3D *scene_node;      // get_world_3d() for body spaces
    std::vector<float> *bubble_centers; // set_physics_bubbles' output, read by physics_tick
    // Named debt: replaced by sub-project 5's EditPipeline.
    std::function<ve::EditLog::AppendResult(const ve::EditOp &, bool)> append_edit_locked;
};
```

- `VoxelWorld::analytic_raycast_down` moves to `WorldStore::raycast_down(const float xz[2])` (its
  body reads only store state and takes `edit_mutex`).
- The field-volume queue/discard helpers (handoff half + `mesh->submit_volume` /
  `discard_pending_volume_upload` half) become private manager methods.
- `release_volume_slot` is the free function in §3.2, shared by the manager and hooks.

### 3.5 Hooks split and friend removal (milestone 4)

`VoxelDebugHooks` stays one bound class: one `hooks.h`, one `_bind_methods` in `hooks.cpp`.

| File | Holds |
|---|---|
| `debug/hooks.cpp` | bindings, `debug_render_frame`, shared helpers (readback, camera) |
| `debug/hooks_render.cpp` | frame probes, isolated pass probes, atlas/material/hiz/sun/grass, GPU timings, reload/beauty |
| `debug/hooks_lod.cpp` | LoD walk/pool/cull/raster/seam probes |
| `debug/hooks_world.cpp` | store, edits, overrides, volumes, residency, streamer, consolidation, field diffs |
| `debug/hooks_physics.cpp` | mesher, colliders, chunk residency, islands, test bodies |

Friend reads are re-homed **in place first** (M4a), then the split is a pure move (M4b) reviewed
with `git diff --color-moved`.

| Private read today | Public home |
|---|---|
| `world_->store_` (183) | `world.store()` (every member read, including `center_`, is already public on `WorldStore`) |
| `initialized_`, `streamer_`, `normal_pool_bytes_`, hiz-readback flags, `islands_enabled_` | orchestrator getters/setters |
| `island_uploads_`, `island_descs_`, `island_descs_dirty_`, `island_slots_`, `island_mutex_` | `render().handoff()` methods |
| `mesh_`, `colliders_`, `chunks_`, `island_manager_`, `physics_ready_`, `test_bodies_` | `mesh_service()` (exists), `colliders()`, `chunk_residency()`, `island_manager()`, `physics_ready()`, `test_bodies()` |
| `overflow_seen_`, `edit_rejections_`, `last_physics_tick_ms_`, `physics_bubble_centers_` | `WorldStats VoxelWorld::stats() const` (POD) |
| `lod->lod_pool_` (33) | `lod.pool()` (exists) |
| `lod->lod_walk_`, `lod_page_quads_`, `lod_pages_of_`, `lod_tree_`, `lod_mutex_` | `LodStats LodSystem::stats() const` (POD copied under its own mutex; the hook no longer locks `lod_mutex_`) |

POD `stats()` exists only where a hook reads private counters. Probes that drive a module call its
public methods; no blanket `stats()` per module.

### 3.6 Sub-project 1 leftovers

Executed as written in `docs/superpowers/plans/2026-09-13-frame-module.md`: Task 10 (migrate
`debug_seam_probe` onto the frame), Task 11 (consolidate pass-probe inputs, delete orphaned frame
plumbing). Task 12's verification and deletion accounting become the "Sub-project 1 acceptance"
section of this sub-project's results report. Sub-project 1's golden policy applies to both tasks.

### 3.7 S8 — SSAO timing nested in "deferred"

Today `timings->begin(rd, "deferred")` (`frame.cpp:454`) precedes the SSAO block, so "deferred"
includes SSAO time.

1. Failing test: after one shipped frame (main device fixture), read the captured timestamp
   *names* and assert `ssao`'s end marker precedes `deferred`'s begin marker. Values are invalid
   on this machine and are not read. **Verify during planning** that Metal captures the names; if
   not, the frame appends its begin/end labels to `FrameRecord` and the test reads that.
2. `fix:` commit: move the `deferred` begin below the SSAO block; update `demo/benchmark.gd`'s
   label/budget tables in the same commit.

## 4. Order of work

Each step is a green commit or commit group.

1. SP1 Task 10, SP1 Task 11.
2. SP2 baseline: re-record the gdUnit failure set at this commit (SP1 Task 1's procedure) into
   `docs/superpowers/plans/2026-09-14-render-lifetime-owner-baseline.md`.
3. Characterization: lifetime contract suite + teardown trace (§5.1), each case proven to bite. No
   other production change.
4. M1: `IslandHandoff` native tests, then the handoff move and locking change.
5. M5: `IslandManager` collaborators, `WorldStore::raycast_down`.
6. M2: `LodSystem::release_gpu()`; `Collaborators` → 5 fields; streamer owned by the orchestrator.
7. M3: `RenderPasses`; frame and frame settings move under the orchestrator; `FrameHost` and the
   VoxelWorld forwarders deleted.
8. S8: failing test, then `fix:`.
9. M4a: friend removal in place. M4b: pure-move split.
10. Results report and roadmap spec amendments.

M5 follows M1 because both rewire island code. M4 is last because it needs every re-homed accessor.

## 5. Testing

### 5.1 Render lifetime contract — `tests/test_render_lifetime_contract.gd`

Fixtures: the local-device world from `tests/test_shader_reload.gd`; the SubViewport + compositors
setup from `tests/test_frame_shipped_golden.gd`. Leak capture: a `Logger` registered with
`OS.add_logger` for the test's duration, collecting lines containing `was leaked`; the local device
is released when the world leaves the tree (`RenderOrchestrator::release_devices`).

| Case | Device | Asserts |
|---|---|---|
| teardown → re-init → frame | local | `debug_render_frame` renders a non-empty frame after `teardown_gpu` + `ensure_initialized`; zero leak lines when the world is freed |
| reload pump → frame | local | same, through `request_shader_reload` + `debug_pump_shader_reload` |
| shutdown → free | local | admission latched shut after `shutdown_render_resources`; zero leak lines |
| uploads survive GPU teardown | local | a field-volume upload queued before `teardown_gpu` lands after re-init (upload counter +1, volume readable in the atlas); `teardown_physics` keeps pinned field-volume uploads and drops island uploads and descriptors |
| teardown order | local | the teardown trace equals the pinned sequence |
| reload and shutdown on the shipped path | main | frames keep rendering after a reload with tile luma within the shipped golden's tolerance; shutdown closes admission. Main-device leaks are not asserted (unmeasurable here) |

**Teardown trace.** The one production addition in step 3: `teardown_gpu()` appends a label per
step (`passes`, `streamer`, `residency`, `island_graph`, `island_slots`, `atlas`, `lod`,
`history`, `initialized`) to a debug-only vector, read through a hook. It makes "teardown order
changed" observable.

**Proving the suite bites** (uncommitted breaks, recorded in the results report): skip one pass
delete → leak case fails; swap two teardown steps → order case fails; drop the pinned-upload keep
→ survival case fails.

### 5.2 Native

`extension/tests/test_island_handoff.cpp`: queue/take ordering; discard removes only field-volume
uploads for that slot; physics-teardown filter keeps pinned field volumes and drops island uploads
and descriptors; `descs_dirty` clears on take; `slot_count` honours the toggle and max rule;
`release_volume_slot` queues a normal release only when the release succeeds; a concurrent
queue/take smoke test. `SConstruct` adds `src/render/island_handoff.cpp` to the native build.

### 5.3 Regression gate (every milestone)

Build; native tests; affected gdUnit suites plus `test_frame_shipped_golden.gd`,
`test_frame_contract.gd`, `test_render_lifetime_contract.gd`. Compared against the SP2 baseline by
case name and message; a suite's case count shrinking is a failure. Full `./gdunit_tests.sh` at
M3, M4b and the end.

## 6. Exit criteria

- `rg 'class FrameHost|friend class VoxelDebugHooks|\*\*lod_pool|island_mutex = ' extension/src`
  returns nothing.
- `rg 'island_mutex_|note_lod_cull_debug|set_beauty_compositor|render_probe_pixel|analytic_raycast_down|initialize\(VoxelWorld' extension/src`
  returns nothing.
- `RenderOrchestrator::Collaborators` has ≤ 5 fields; `voxel_world.h` ≤ 250 lines.
- Appendix A "new render pass" re-traced at ≤ 8 files and recorded: pass `.h`/`.cpp`, shader,
  `orchestrator.h` (table field), `orchestrator.cpp` (build/teardown), `frame.h` (stage bit),
  `frame.cpp` (stage + label), one gdUnit test through `debug_render_frame`.
- Lifetime contract, shipped golden and frame contract pass unchanged; the S8 test passes.
- Every bound method on `VoxelWorld` and `VoxelDebugHooks` keeps its name, signature and
  Dictionary keys (diff of `_bind_methods` bodies shows only moved lines).
- Roadmap spec amended: SP1 status; §4.6/§9.1 record `FrameHost` deleted; sub-project 4's entry
  gate reads "passes reachable only through `RenderPasses`" (pass-level probes stay isolated by
  sub-project 1's decision).
- Results report `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md`: SP1
  acceptance, verification, golden attribution, before/after line counts, retrace, bite proofs.

## 7. Stop conditions

Stop and re-plan; no workaround inside the milestone:

- The teardown trace changes.
- A lock is acquired anywhere other than today's sites (handoff sites listed in its header).
- The shipped golden moves outside an attributed SP1-leftover change.
- A bound method's name, signature or Dictionary keys change.
- A suspected shipped bug appears: failing test and `fix:` commit before the move that touches it.

## 8. Out of scope

Settings-store rework (sub-project 3; the effect/grass/tier API only gets thinner bodies here);
pass internals and shaders (sub-project 4; S8 touches only the frame's timing calls); edit fan-out
and `append_edit_locked` (sub-project 5); `ConsolidationCoordinator` beyond its streamer slot;
compositor admission and lifetime locks; orchestrator construction order.

## 9. Risks

| Risk | Mitigation |
|---|---|
| `manager_slots` publish misses a site, so islands vanish from the march | native `slot_count` tests; island gdUnit suites in the M1 gate; grep for `slot_high_water_` returns only the handoff |
| Moving frame/streamer ownership reorders destruction | `VoxelFrame` stays declared after the members it references; teardown trace; lifetime contract |
| Leak capture misses leaks the device does not report | stated limit; bite proof shows a skipped delete *is* reported |
| M4b move silently edits code | pure-move commit reviewed with `--color-moved`; case counts identical |
| Main-device marker names not captured on Metal (S8 test) | FrameRecord label fallback, decided during planning |
