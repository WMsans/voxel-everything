# Voxel Everything — Pass Anatomy and Generated Layouts (Sub-project 4)

**Date:** 2026-09-15
**Status:** Implemented; see `docs/superpowers/plans/2026-09-15-pass-anatomy-results.md`
**Roadmap:** `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.3 and the pathway in
`docs/superpowers/plans/2026-09-13-frame-module.md` ("Sub-project 4 — Pass anatomy and generated
layouts").
**Scope:** roadmap milestones (a) 1–3 and (b) 4–7, suspected bugs S4, S7, S9. One spec, one plan.

Vocabulary as in the roadmap spec (Ousterhout; `CONTEXT.md` for domain nouns).

---

## 1. Problem

Every GPU pass hand-writes the same lifetime machinery, and every C++↔GLSL contract except the
material table is restated by hand.

- **Compile block copied into 23 files.** `shader_compile_spirv_from_source` appears in 22
  `render/*_pass.cpp` files and twice in `orchestrator.cpp` (downsample shader, reload
  pre-flight). Each is the same ~35-line load → strip → compile → printerr → shader → pipeline
  sequence (`ssao_pass.cpp:26-61`, `region_pass.cpp:34-60`).
- **Hand-written RID-keyed uniform-set caches.** One `key_*_` member per bound RID, compared by
  hand (`ssao_pass.cpp:101-127`, `deferred_pass.cpp` 10 keys/11 uniforms); ~92 hand-numbered
  `set_binding` calls.
- **Free order is where the bugs are.** `8da111d` had to change 20 pass files so a uniform set the
  device already cascade-freed (because a texture it referenced was freed) is not freed again.
  Composite's cache depends on raymarch's targets, so the frame reaches across passes to drop it:
  `CompositePass::invalidate_uniform_set` at `frame.cpp:247`, `frame.cpp:604`,
  `hooks_render.cpp:2248`.
- **Push/UBO bytes at hand-counted offsets** (`deferred_pass.cpp:207-251`, `ssao_pass.cpp:142-153`)
  guarded by tautological asserts (`static_assert(sizeof(float) * 28 == 112)`), in 29 shaders with
  push constants plus the SunBlock cascade UBO (`deferred.comp.glsl:23`), `BeautyCam`
  (`beauty_camera.cpp:7`) and `SunLight`.
- **Duplicated constants.** `kFlag*` (`beauty_settings.h:47-57`) vs `BEAUTY_*`
  (`shade.glslh:126-134`); `kMaterialLayers` (`material_atlas.h:12`) vs four
  `#define MATERIAL_LAYERS 16`; `CelParams` defaults (`cel.h`) vs `shade.glslh:46-54` vs
  `cel.gdshaderinc:1-11`; brick/volume sizes in `common.glslh:24-50` guarded by asserts naming the
  wrong files (S9).
- **The G-buffer layout is a comment** (`gbuffer.h:12-17`). Writers restate it
  (`composite.frag.glsl`, `lod.frag.glsl`, `grass.frag.glsl:24`), raster passes hand-build blend
  arrays, readers decode by magic (`g1.z < 0.5` = no surface, `uint(g1.z + 0.5)` = material).
- **Material ids are literals** (`GRASS_MATERIAL = 1u` in `grass.frag.glsl:26` and
  `grass_scatter.comp.glsl:43`, `generator.cpp`, `height_bands.field.glslh`,
  `raymarch.comp.glsl:680`).

## 2. Decided constraints

| Decision | Choice |
|---|---|
| Spec/plan cut | One spec, one plan for all seven milestones. The helper lands before the generators so push packing is rewritten once |
| Migration breadth | All 23 compile sites. Exit criterion holds as the roadmap wrote it: `shader_compile_spirv_from_source` appears only in the helper |
| Helper shape | Composable parts held as pass members; no base class, no descriptor tables, no render graph. Pass public interfaces, `RenderPasses`, `VoxelFrame`, hooks and GDScript bindings are unchanged |
| Native testing of lifetime | A godot-free core, header-only, templated over a `Device` concept on `uint64_t` ids; a `FakeDevice` in `extension/tests/`; a thin `RenderingDevice` adapter in the extension |
| Layout source of truth | The C++ struct, with a field table beside it (`{name, type, offsetof}`); an emitter writes GLSL; a native test checks every offset and `sizeof` coverage |
| Generated files | Committed under `shaders/generated/`, each gated by a byte-exact native test that prints the correct contents on failure (the `material_table_glsl()` pattern); `VE_REGEN_GOLDEN=1` rewrites them |
| S7 / foliage | Foliage gets its own id range beside the terrain materials (`FOLIAGE_BASE + k`) from a generated `kFoliage` table; today one row, `grass_blade`, glow 0. Other grass and leaf types will follow and must cost one row plus their shader |
| Leak measurement | The SP2 `LeakLogger` (`tests/test_render_lifetime_contract.gd`): "RID was leaked" lines when a local `RenderingDevice` is freed. RD allocation counters read 0 on Metal |
| GPU timing | Not measured (`debug_gpu_timings()` is invalid on this machine); CPU `last_ms()` keys are preserved |

### Decisions made during design

1. **No RID generation counter.** Godot never reuses RID ids, so a recreated texture changes the
   cache key by itself.
2. **Field-list macros, not whole blocks.** Generated `#define SSAO_PUSH_FIELDS ...` keeps
   `set`/`binding`/`push_constant` at the use site, following the `GRASS_PARAMS_BLOCK` precedent.
3. **`test_deferred.gd:52` keeps its band-edge literals.** They are probe points for the GPU-vs-CPU
   cel diff, which still measures agreement; generation removes the drift the diff guards. No
   hook is added.
4. **Out of scope from §11 of the roadmap spec:** `field_codegen`, `field_params_pack` and the
   `FieldParams` block are left alone.

## 3. Target design

### 3.1 Pass helper — `extension/src/render/gpu/`

| Unit | Godot-free | Responsibility |
|---|---|---|
| `uniform_set_cache.h` | yes (header-only template) | Key = shader id, set index, ordered `{type, binding, ids…}`. `get(device, shader, set, uniforms)` returns the cached set when the key matches **and** `device.set_valid(id)`; otherwise frees it only if still valid and creates a new one. `release(device)` is idempotent |
| `resource_group.h` | yes (header-only template) | Owns a pass's RIDs by kind and frees them dependents-first: uniform sets → pipelines → shaders → samplers → textures → framebuffers. Every free checks validity. One `release(device)` is a pass's teardown |
| `rd_device.h/.cpp` | no | Adapter: `Device` concept over `RenderingDevice*`; `RID` ↔ `uint64_t` (`get_id()` / `RID::from_uint64`); builds `RDUniform` from the POD descriptors |
| `program.h/.cpp` | no | `compile_compute(rd, group, "ssao.comp.glsl")` and `compile_raster(rd, group, vert, frag, fb_format, RasterState)`: load, strip, compile, printerr, shader, pipeline, registered in the group. `compile_check(rd, path)` compiles and creates nothing (reload pre-flight). The only `shader_compile_spirv_from_source` site |
| `target.h/.cpp` | no | `ensure(rd, group, format, size, usage, clear_color)`; recreates on resize |
| `framebuffer_cache.h/.cpp` | no | Keyed on attachment RIDs; blend-attachment array sized from the colour attachment count (`GB_ATTACHMENTS` after §3.4) |
| `dispatch.h` | no | `dispatch(rd, pipeline, {{set, index}…}, const T &push, groups)`: `static_assert(std::is_trivially_copyable_v<T>)`, `memcpy` into `PackedByteArray`, one compute list. `CpuTimer` scoped timer writing a pass's `last_ms_` |

`FakeDevice` (tests only) records creates and frees and models Godot's cascade: freeing a texture,
buffer or sampler invalidates every uniform set that references it; freeing a shader invalidates
its pipelines and sets.

A simple pass after migration, for scale:

```cpp
void SsaoPass::initialize(RenderingDevice *rd) {
	teardown();
	rd_ = rd;
	prog_ = gpu::compile_compute(rd, group_, "ssao.comp.glsl");
	nearest_ = gpu::sampler_nearest(rd, group_);
}

bool SsaoPass::render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo, const ve::BeautySettings &s) {
	output_ = RID();
	if (!s.ssao || !prog_.valid() || !gb.is_valid() || !camera_ubo.is_valid()) return false;
	const Vector2i half(std::max(1, gb.size().x / 2), std::max(1, gb.size().y / 2));
	if (!target_.ensure(rd, group_, DATA_FORMAT_R8_UNORM, half, kStorageSampled, Color(1, 1, 1, 1))) return false;
	RID set = sets_.get(rd, prog_.shader, 0, {gpu::sampled(0, nearest_, gb.surface()),
			gpu::sampled(1, nearest_, gb.depth()), gpu::image(2, target_.rid()), gpu::ubo(5, camera_ubo)});
	if (!set.is_valid()) return false;
	gpu::CpuTimer timer(last_ms_);
	const ve::SsaoPush push{{half.x, half.y, s.ssao_steps, s.ssao_directions}, {kSsaoRadius, kSsaoStrength, 0, 0}};
	if (!gpu::dispatch(rd, prog_.pipeline, {{set, 0}}, push, gpu::groups(half, 8))) return false;
	output_ = target_.rid();
	return true;
}
```

**Deleted by the helper:** every per-pass compile block, every `key_*_` member,
`CompositePass::invalidate_uniform_set` and its three callers (composite's cache sees raymarch's new
target RIDs), the hand-written free lists in each `teardown()`.

### 3.2 Push and UBO structs — `extension/src/gpu_layout/` (milestone 4)

A new pure module, added to `pure_sources` in `extension/SConstruct`.

- `blocks.h`: every push/UBO layout as a trivially copyable struct (`SsaoPush`, `DeferredPush`,
  `SunCascadeBlock`, `BeautyCam`, `SunLight`, one per push-constant shader — 29 today), each with a
  field table `{name, type, offsetof(S, m)}`.
- Types: `float`, `int`, `uint`, `vec4`, `ivec4`, `uvec4`, `mat4` and fixed arrays of these. No bare
  `vec3`; padding is explicit.
- `layout_emit.h/.cpp`: computes std140 or std430 offsets per table, rejects a table whose offsets
  disagree with `offsetof` or whose fields do not cover `sizeof`, and emits
  `shaders/generated/blocks.glslh` as one field-list macro per struct:
  `#define SSAO_PUSH_FIELDS ivec4 dims; vec4 params;`
- Shaders: `layout(push_constant, std430) uniform Push { SSAO_PUSH_FIELDS } pc;`
- Passes fill the struct and hand it to `dispatch` (or `buffer_update` via the same `memcpy`
  helper). Hand-offset writes and every `static_assert(sizeof(float) * N == M)` are deleted.
- `ve::GrassParams` (`grass_layout.h`, sixteen `vec4`-sized members) gets a field table;
  `GRASS_PARAMS_BLOCK` moves from `grass.glslh` into the generated file.

### 3.3 Constants and material ids (milestone 5)

`shaders/generated/constants.glslh`:

- `BEAUTY_*` from a `kBeautyFlags[] = {{"SSGI", kFlagSsgi}, …}` table beside the existing `kFlag*`
  constants; `shade.glslh`'s copies are deleted.
- `MATERIAL_LAYERS` from `kMaterialLayers`. The four `#define MATERIAL_LAYERS 16` are deleted; the
  gate in `common.glslh` that requires sampler arrays to be declared first becomes
  `#ifdef VE_MATERIAL_ARRAYS`.
- `BRICK_VOXEL_COUNT`, `BRICK_SDF_COUNT`, `VOLUME_VOXELS` from their `ve::` constants, removed from
  `common.glslh`. The asserts in `volume_pool.cpp:12-13` and `override_pool.cpp:14-17` that name the
  wrong files are deleted (S9 closes).

`shaders/material_table.glslh` (existing generator, path unchanged) gains:

- Named ids `const uint MAT_<NAME> = <id>u;` from `kMaterials[].name`.
- The foliage range from §3.5 (landed earlier by the S7 fix).

C++: `constexpr uint8_t ve::mat_id(std::string_view name)` looks a name up in `kMaterials` at
compile time and fails to compile on an unknown name. Literals in `generator.cpp`,
`height_bands.field.glslh`, `grass_scatter.comp.glsl` and `raymarch.comp.glsl:680` become names.
`shaders/generated/field.glslh.golden` changes text but not the float baselines; the commit names
that as the cause.

### 3.4 G-buffer channels (milestone 6)

`gpu_layout/gbuffer_layout.h` (pure) describes attachments and channels and emits
`shaders/generated/gbuffer.glslh`:

- `GB_ATTACHMENTS` (colour attachment count).
- Readers: `GB_ALBEDO(g0)`, `GB_SUN_VIS(g0)`, `GB_NORMAL(g1)`, `GB_MATERIAL_ID(g1)`,
  `GB_IS_SURFACE(g1)`, `GB_GLOSS(g1)`.
- Writer: `GB_PACK_SURFACE(n, mat, gloss)`.

`gbuffer.h`'s comment table points at the layout header; `GBuffer`'s format table stays (roadmap
§11) with a `static_assert` against `kGbAttachments`. `FramebufferCache` sizes blend arrays from it.
Readers and writers switch one shader per commit.

### 3.5 Foliage ids (S7)

`material_table.h` gains `kFoliage[] = {{"grass_blade", glow 0, glow_rgb 0}}` and
`FOLIAGE_BASE` (first id above every terrain id and `MATERIAL_COUNT`). `material_table_glsl()` emits
`MAT_GRASS_BLADE`, the foliage arrays, and a `mat_glow` / `mat_glow_rgb` that look up whichever range
the id is in (fail-soft zero outside both). Foliage ids never index the material atlas; every
atlas lookup is already gated by glow > 0 or by a terrain-range check, and the plan verifies each
reader. `grass.frag.glsl` writes `MAT_GRASS_BLADE`; `grass_scatter.comp.glsl` keeps testing terrain
id 1 (by name after §3.3) to decide where blades grow.

### 3.6 Cel constants (milestone 7)

Emitted from `CelParams` defaults and `kCelBands` into `shaders/generated/cel.glslh` (included by
`shade.glslh`, hand constants deleted) and `shaders/generated/cel_constants.gdshaderinc` (included by
`cel.gdshaderinc`, hand constants deleted). `ve_cel_level`'s literal thresholds read the constants.

### 3.7 Objects follow the scene sun (S4)

`cel_object.gdshader` reads sun direction, sun colour and ambient from global shader uniforms that
`VoxelWorld::update_sun_state` sets on the main thread from `SunState` each frame, alongside
`RenderOrchestrator::set_sun_state`, replacing `VE_SUN_DIR`. `island_body.cpp:194`'s
hard-coded `ambient_linear` is removed in favour of the global.

The hard-coded rock albedo (`island_body.cpp:193`) is **not** fixed here: an `IslandBody` carries no
material data today, so a per-material albedo needs island material extraction, which is outside
this sub-project. The results report records it as an **accepted out-of-scope S4 remainder, not as
a claim that S4 is fully complete**, with that reason.

## 4. Order of work

Each step is one or more commits; nothing in a later step lands before the earlier step's gate.

1. **Baseline.** gdUnit failure set at the start commit (frame-module Task 1 procedure). This run
   closes SP2's open runtime gates (shipped golden, frame contract, lifetime contract, S8 marker
   test). If any fails, SP2 is not accepted: stop.
2. **Coverage audit and bite proofs.** A table mapping each of the 23 sites to its biting test
   (§5.2). A golden is added for any site whose test is invariant-only. Each site gets a deliberate
   break → named failure → revert, recorded in the evidence log. Leak-line baseline recorded.
3. **Failing tests** for S4 and S7.
4. **`fix:` S7** (§3.5) and **`fix:` S4** (§3.7) on today's code, each in its own commit.
5. **Helper core** (native tests first), then the adapter and `program`/`target`/`framebuffer_cache`/
   `dispatch`.
6. **Migrate frame post passes:** ssao → contact_shadow → outline → ssr → ssgi → hiz → deferred.
7. **Migrate raster passes:** inject → grass_raster → lod_raster → sun_shadow → composite (deletes
   `invalidate_uniform_set`).
8. **Migrate frame compute:** island_cull → lod_cull → grass_scatter → raymarch → the two
   `orchestrator.cpp` sites.
9. **Migrate world jobs:** brick_gen → region → consolidate → island_extract → mesh → lod_build.
10. **Generated blocks** (§3.2): emitter + native tests, then one pass per commit.
11. **Generated constants and named ids** (§3.3).
12. **Generated G-buffer channels** (§3.4), one shader per commit.
13. **Generated cel constants** (§3.6).
14. **Results report:** exit criteria, change-cost retraces, pinned-value attribution, S-row closure.

One file per migration commit. Every commit touching a pass runs that pass's biting test plus the
shipped golden, frame contract and lifetime contract.

## 5. Testing

### 5.1 Native (new)

- `test_uniform_set_cache.cpp`: identical key reuses; a changed RID, shader or set index rebuilds;
  a cascade-freed set is rebuilt and never double-freed; `release()` is idempotent.
- `test_resource_group.cpp`: free order is dependents-first; no double free; nothing left in
  `FakeDevice`; teardown → re-init is clean.
- `test_gpu_layout.cpp`: std140/std430 offsets equal `offsetof` for every block; coverage equals
  `sizeof`; a misaligned or incomplete table is rejected; beauty flag bits unique; `mat_id`
  round-trips every name; foliage ids never overlap terrain ids.
- `test_generated_glsl.cpp`: one byte-exact case per generated file (`blocks.glslh`,
  `constants.glslh`, `gbuffer.glslh`, `cel.glslh`, `cel_constants.gdshaderinc`) and one case running
  `load_shader_source` over each (the loader matches include tokens anywhere in a line, so the
  emitter must never write that word). `test_material_glslh.cpp` keeps gating `material_table.glslh`.

### 5.2 GPU (gdUnit)

Coverage candidates for the audit (step 2):

| Group | Sites | Candidate biting tests |
|---|---|---|
| Frame post | ssao, contact_shadow, outline, ssr, ssgi, hiz, deferred | `test_ssao_golden`, `test_contact_shadow`, `test_outline`, `test_ssr`, `test_ssgi` / `test_emissive_gi`, `test_hiz`, `test_deferred` |
| Raster | inject, grass_raster, lod_raster, composite, sun_shadow | `test_gbuffer`, `test_grass_golden`, `test_lod_gbuffer`, `test_raymarch_gbuffer`, `test_sun_shadow` |
| Frame compute | raymarch, island_cull, lod_cull, grass_scatter, orchestrator downsample + pre-flight | `test_raymarch_pixel`, `test_island_render`, `test_lod_cull`, `test_grass`, `test_frame_shipped_golden`, `test_shader_reload` |
| World jobs | brick_gen, region, consolidate, island_extract, mesh, lod_build | `test_brick_diff`, `test_region_pass`, `test_consolidation`, `test_island_extract`, `test_mesh_diff`, `test_lod_build` |

New failing-first tests: S4 (a cel-shaded object's lit colour follows a moved scene sun); S7 (with
`grass_01` made emissive, blade pixels receive no glow).

Every migration commit: that site's biting test unchanged, leak lines not above baseline, shipped
golden, frame contract and lifetime contract unchanged. The gdUnit failure set is compared with the
step-1 baseline, stashing and re-running before attributing a failure to the change.

## 6. Exit criteria

- `rg 'shader_compile_spirv_from_source' extension/src` hits only `render/gpu/gpu.cpp`, the single compile site.
- `rg 'invalidate_uniform_set|static_assert\(sizeof\(float\) \*|#define MATERIAL_LAYERS|GRASS_MATERIAL' extension/src shaders`
  returns nothing.
- `rg 'key_[a-z_]+_ = ' extension/src/render` returns nothing.
- Every file under `shaders/generated/` and `shaders/material_table.glslh` has a byte-exact test.
- Change cost re-traced and recorded in the results report: new material ≤ 4 files (from 7–9); new
  G-buffer channel ≤ 5 (from 14–18); SP2's `FogPass` retrace re-measured (9 today). These are
  aspirational measurement targets; a documented miss is an accepted waiver, not an unresolved
  blocker, and the exact count and rationale remain in the results.
- gdUnit failure set no worse than the step-1 baseline; every moved pinned value names its cause.
- S7 and S9 closed with evidence in the results report; S4's sun/ambient half closed with evidence
  and its rock-albedo remainder recorded as an accepted out-of-scope remainder—not as a claim that
  S4 is fully complete—with its reason (§3.7).

### 6.1 Accepted audit waivers for this sub-project

The exact no-hand-written-key regex and the file-count limits above are audit probes and
aspirational measurement targets, not unresolved blockers. An intentional exception is an
accepted waiver only when the results report preserves the exact probe output or measured count
and its rationale; neither the regex nor an over-target count is hidden. This sub-project records:

- `raymarch_pass.cpp`'s `uset_mask_` as an accepted waiver: it is an externally owned tile-mask
  RID identity tracker, not a uniform-set key cache.
- The new-material, G-buffer-channel, and `FogPass` retraces as accepted measurement-target
  waivers when their measured counts exceed the aspirational limits, with their fan-out or
  bookkeeping rationale recorded in the results.
- S4's island rock albedo as an accepted out-of-scope remainder because `IslandBody` has no
  material data; this is not a claim that S4 is fully complete.

## 7. Stop conditions

Any of these means a move was not verbatim; stop and re-plan:

- A pinned value moves in a migration commit.
- Leak lines rise above the baseline.
- A lock is taken in a new place.
- Stage order or teardown order changes as observed by the frame contract or lifetime contract.

## 8. Out of scope

- Generated binding numbers (`uniform_set_create` already rejects a mismatch).
- A render graph or declarative pass descriptors.
- Per-pass shader reload without GPU teardown (the roadmap's deferred look-dev trigger fires once
  this sub-project is accepted).
- Foliage types beyond `grass_blade`.
- Moving beauty knobs into a settings table (sub-project 3).
- `field_codegen`, `field_params_pack`, the `FieldParams` block.
- Generating `gpu_timings.cpp`'s `kPasses` — unless it is the single file keeping the `FogPass`
  retrace above 8, which the results report records either way.

## 9. Risks

- **Blast radius.** 23 sites, six of them world jobs off the frame path. Mitigation: world jobs
  migrate last, each only after its bite proof.
- **Macro field lists and the include loader.** Continuation-line `#define`s must survive
  `load_shader_source`; covered by the loader case in `test_generated_glsl.cpp`.
- **Noisy GPU pins.** SSGI jitter and grass sway; use the shipped golden's averaging and never
  tighten a tolerance to make a commit pass.
- **Leak detection is weak on Metal.** Only local-device leak lines are observable; main-device
  leaks are not asserted.
- **Foliage ids reaching an atlas lookup.** A reader that indexes the material atlas by id without a
  range check would sample out of range; the S7 task audits every `mat` reader before the fix.

## Decisions made during planning

1. **The lifetime core is templated over `Device::Id`, not `uint64_t`.** godot-cpp's `RID` has no public constructor from an id, so the core never converts: `RdDevice::Id` is `RID`, `FakeDevice::Id` is `uint64_t`.
2. **Two helper files, not seven.** `render/gpu/gpu_core.h` (pure: `Kind`, `Uniform`, `ResourceGroup`, `UniformSetCache`, `uniform_set`) and `render/gpu/gpu.{h,cpp}` (adapter, compile, samplers, textures, `Target`, `FramebufferCache`, `RasterState`, `raster_pipeline`, `push_bytes`, `dispatch`, `CpuTimer`). The exit criterion's single compile site is `render/gpu/gpu.cpp`.
3. **`extension/SConstruct` gains `Glob("src/*/*/*.cpp")`** for the library; `src/gpu_layout/*.cpp` joins `pure_sources`.
4. **Define injection is one pure function,** `ve::insert_after_version` in `render/shader_loader`, replacing the SSR, LoD-raster and composite copies.
5. **Only 16-byte-aligned GLSL types are emitted** (`vec4`, `ivec4`, `uvec4`, `mat4` and arrays of them). Every current block already uses only these, and for them std140 and std430 place each field at the same offset; `check_block` refuses anything else.
6. **`FOLIAGE_BASE` is 200,** a fixed id with `static_assert(kMaterialCount < kFoliageBase)`, not "the first id above the table": adding a terrain material must not renumber foliage, and the GPU test needs a stable expected id.
7. **The S7 test is at id level:** `debug_grass_stats` gains `blade_materials`, the distinct material ids the hooked blade raster wrote. `mat_glow` is id-indexed, so a blade id outside the terrain range cannot pick up `grass_01`'s glow.
8. **S4's globals are declared in `project.godot` `[shader_globals]`** (a `global uniform` must exist before the shader compiles) and set on the main thread in `VoxelWorld::update_sun_state`, next to `set_sun_state` — not in the orchestrator, which has no main-thread hook.
9. **Raymarch's `u[31]`** (an unconfigured `RDUniform` pushed into the set array) is dropped in the migration; Godot matches uniforms by binding and already ignores it.
10. **SSGI's cache keyed on `gb.albedo()` without binding it;** the migrated key is exactly the bound ids.
11. **Sets that never change identity are built once** with `gpu::uniform_set` (world-job passes, HiZ mips 1–8); only sets whose inputs change use `SetCache`.
12. **`CompositePass::release_targets()` stays** where the frame and hooks call it; only `invalidate_uniform_set` is deleted.
13. **`test_sun_light_shader.cpp` pins the text `uniform SunLight`;** the migrated declaration keeps that prefix and moves only the fields into `SUN_LIGHT_FIELDS`.
14. **`ve::material_id`, not the spec's `ve::mat_id`:** a parameter named `mat_id` already exists in `world/palette.h`. An unknown name fails to compile by reaching a non-constexpr call (godot-cpp builds without exceptions, so `throw` is unavailable).
15. **Generated headers are macro-only where they are included right after `#version`** (`blocks.glslh`, `gbuffer.glslh`, so `GB_ATTACHMENTS` is a `#define`); headers with `const` declarations (`constants.glslh`, `cel.glslh`) are included from `common.glslh` / `shade.glslh`.
