# Pass Anatomy and Generated Layouts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every GPU pass is built from one shared helper (compile, uniform-set cache, targets, framebuffers, pipelines, dispatch, teardown), and every C++↔GLSL contract that can be generated — push/UBO layouts, beauty flags, material layers and ids, brick/volume sizes, G-buffer channels, cel constants — is generated and byte-checked.

**Architecture:** Characterize first (baseline, per-site bite proofs, failing tests for S7 and S4, then their fixes on today's code). A godot-free lifetime core (`ResourceGroup`, `UniformSetCache`) is written test-first against a fake device, bound to `RenderingDevice` by a thin adapter, and the 23 shader-compile sites migrate onto it one file per commit. Then a pure `gpu_layout` module emits committed GLSL headers from C++ tables, and shaders and passes switch over one per commit.

**Tech Stack:** C++20, godot-cpp (Godot 4.7.2), GLSL via `RenderingDevice`, doctest (native), gdUnit4 (GPU), SCons.

**Spec:** `docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md` (committed `13a1161`). Roadmap: `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.3, §10; pathway in `docs/superpowers/plans/2026-09-13-frame-module.md` ("Sub-project 4").

## Global Constraints

- Branch: `refactor/pass-anatomy` (already checked out; spec at `13a1161`).
- **GDScript API.** Every existing `ClassDB::bind_method` name and signature and every existing Dictionary key is unchanged. The only addition is the `blade_materials` key of `debug_grass_stats` (Task 5).
- **No stage reordering** in `VoxelFrame`; **no lock acquired at a new call site**.
- **Never add a call to `teardown()` or `initialize()` on `DeferredPass` or `ContactShadowPass`.** Their bodies change in Tasks 10 and 15; their callers do not. Both keep their `sun_light_ubo_` mirror across `teardown()` (it is not owned).
- **Migration commits (Tasks 9–31) are moves.** A pinned value (golden, probe number, frame-contract assertion) that changes in a migration commit is a stop condition, not something to re-record.
- **Generation commits (Tasks 32–40)** may change the *text* of `shaders/generated/field.glslh.golden` only where the task says so; the float baselines (`extension/tests/golden/*`) never change.
- **Stop conditions (spec §7):** a pinned value moves in a migration commit; leak lines rise above the recorded baseline; a lock at a new site; stage order or teardown order changes as seen by `test_frame_contract.gd` or `test_render_lifetime_contract.gd`. Stop, report, do not work around.
- **Baseline comparison:** compare gdUnit results against `docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md` by case name *and* message. A suite's case **count** dropping is a failure (a failing case aborts the rest of its suite). `test_connectivity` and `test_island_body` are flaky by case: compare their failure count. The set drifts: before blaming a change, `git stash`, rebuild, re-run the same suites.
- **GPU timing values are invalid on this machine;** never pin them. Leaks are measured only as `" leaked"` lines when a local `RenderingDevice` is freed (RD allocation counters read 0 on Metal).
- **Pure code stays pure.** `extension/src/render/gpu/gpu_core.h` and everything under `extension/src/gpu_layout/` include no godot-cpp header; the native build (`extension/SConstruct` `test_env`) compiles them.
- **Commit messages** carry no attribution trailer.
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` from the repo root (a clean rebuild can take ~20 min). Native: `(cd extension && scons -Q test)`. GPU: `./gdunit_tests.sh -a res://tests/<a>.gd,res://tests/<b>.gd`; full run `./gdunit_tests.sh`. Reports: `reports/report_N/results.xml`. Logs go to `.superpowers/sdd/2026-09-15-pass-anatomy/` (git-ignored); create it with `mkdir -p` on first use.
- All commands run from the repo root unless a step says otherwise.

### Procedure: Pass gate

Every task that changes a pass or a shader ends with this gate. `<site suites>` is the row for the site in the Site table below.

```bash
L=.superpowers/sdd/2026-09-15-pass-anatomy; mkdir -p $L
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -3
(cd extension && scons -Q test) 2>&1 | tail -3
./gdunit_tests.sh -a <site suites>,res://tests/test_frame_shipped_golden.gd,res://tests/test_frame_contract.gd,res://tests/test_render_lifetime_contract.gd 2>&1 | tee $L/gate-<site>.log
grep -c " leaked" $L/gate-<site>.log
latest=$(ls -d reports/report_* | sort -V | tail -1); python3 .superpowers/sdd/2026-09-15-pass-anatomy/failures.py "$latest/results.xml"
```

Pass criteria: build OK; native all pass; the failing gdUnit cases (name + message) equal the site's recorded row in the baseline evidence log; the `leaked` count is ≤ the recorded count. Append one line to the evidence log: `- <site> <task>: build OK, native <N>/<N>, gdUnit matches row, leaked <n> (row <m>)`.

`failures.py` is written in Task 1 Step 3.

### Site table

| Site | File | Site suites (`res://tests/…`) |
|---|---|---|
| ssao | `render/ssao_pass.cpp` | `test_ssao.gd,test_ssao_golden.gd` |
| contact_shadow | `render/contact_shadow_pass.cpp` | `test_contact_shadow.gd` |
| outline | `render/outline_pass.cpp` | `test_outline.gd` |
| ssr | `render/ssr_pass.cpp` | `test_ssr.gd` |
| ssgi | `render/ssgi_pass.cpp` | `test_ssgi.gd,test_emissive_gi.gd` |
| hiz | `render/hiz_pass.cpp` | `test_hiz.gd` |
| deferred | `render/deferred_pass.cpp` | `test_deferred.gd` |
| inject | `render/inject_pass.cpp` | `test_gbuffer.gd` |
| grass_raster | `render/grass_raster_pass.cpp` | `test_grass.gd,test_grass_golden.gd` |
| lod_raster | `render/lod_raster_pass.cpp` | `test_lod_gbuffer.gd,test_lod_render.gd,test_lod_seam.gd` |
| sun_shadow | `render/sun_shadow_pass.cpp` | `test_sun_shadow.gd,test_sun_cascades_gpu.gd` |
| composite | `render/composite_pass.cpp` | `test_raymarch_gbuffer.gd,test_lod_seam.gd` |
| island_cull | `render/island_cull_pass.cpp` | `test_island_render.gd` |
| lod_cull | `render/lod_cull_pass.cpp` | `test_lod_cull.gd` |
| grass_scatter | `render/grass_scatter_pass.cpp` | `test_grass.gd,test_grass_golden.gd` |
| raymarch | `render/raymarch_pass.cpp` | `test_raymarch_pixel.gd,test_raymarch_gbuffer.gd,test_raymarch_mips.gd,test_raymarch_magenta.gd` |
| orchestrator | `render/orchestrator.cpp` (downsample, pre-flight) | `test_gbuffer.gd,test_ssgi.gd,test_shader_reload.gd,test_pipeline_reload.gd` |
| brick_gen | `render/brick_gen_pass.cpp` | `test_brick_diff.gd,test_field_diff.gd` |
| region | `render/region_pass.cpp` | `test_region_pass.gd,test_streaming.gd` |
| consolidate | `render/consolidate_pass.cpp` | `test_consolidation.gd` |
| island_extract | `render/island_extract_pass.cpp` | `test_island_extract.gd` |
| mesh | `render/mesh_pass.cpp` | `test_mesh_diff.gd,test_mesh_stream.gd` |
| lod_build | `render/lod_build_pass.cpp` | `test_lod_build.gd,test_lod_mesh_diff.gd` |

Paths in the File column are relative to `extension/src/`.

## Decisions made during planning (amend the spec; Task 41 writes them into it)

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

## File Map

| File | Status | Responsibility |
|---|---|---|
| `extension/src/render/gpu/gpu_core.h` | Create | Pure lifetime core |
| `extension/src/render/gpu/gpu.h`, `gpu.cpp` | Create | RenderingDevice adapter and pass helper |
| `extension/tests/test_gpu_core.cpp` | Create | Fake-device tests |
| `extension/src/render/shader_loader.{h,cpp}` | Modify | `insert_after_version` |
| `extension/tests/test_shader_loader.cpp` | Modify | Its test |
| `extension/SConstruct` | Modify | Library glob; `gpu_layout` pure |
| `extension/src/render/*_pass.{h,cpp}` (22 passes), `render/orchestrator.{h,cpp}`, `render/frame.cpp`, `debug/hooks_render.cpp` | Modify | Migration |
| `extension/src/gpu_layout/layout.{h,cpp}` | Create | Block checker and emitter |
| `extension/src/gpu_layout/blocks.{h,cpp}` | Create | Every push/UBO struct and table; `blocks_glsl()` |
| `extension/src/gpu_layout/constants.{h,cpp}` | Create | `constants_glsl()` |
| `extension/src/gpu_layout/gbuffer_layout.{h,cpp}` | Create | `gbuffer_glsl()` |
| `extension/src/gpu_layout/cel_emit.{h,cpp}` | Create | `cel_glsl()`, `cel_gdshaderinc()` |
| `extension/tests/test_gpu_layout.cpp`, `test_generated_glsl.cpp` | Create | Table checks; byte-exact goldens |
| `shaders/generated/{blocks,constants,gbuffer,cel}.glslh`, `shaders/generated/cel_constants.gdshaderinc` | Create (generated) | Committed outputs |
| `extension/src/world/material_table.{h,cpp}` | Modify | Foliage range, `mat_id`, named ids, `kMaterialLayers` |
| `extension/src/shade/beauty_settings.h` | Modify | `kBeautyFlags` table |
| `shaders/*.glsl`, `shaders/*.glslh`, `shaders/stages/height_bands.field.glslh`, `shaders/cel.gdshaderinc`, `shaders/cel_object.gdshader` | Modify | Consumers |
| `extension/src/physics/island_body.cpp`, `extension/src/voxel_world.cpp`, `project.godot` | Modify | S4 |
| `tests/test_grass.gd`, `tests/test_cel_object.gd` | Modify | S7, S4 |
| `docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md` | Create | Baseline and evidence log |
| `docs/superpowers/plans/2026-09-15-pass-anatomy-results.md` | Create | Results report |

---

### Task 1: Baseline and SP2's open runtime gates

No production change.

**Files:**
- Create: `docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md`
- Create (ignored): `.superpowers/sdd/2026-09-15-pass-anatomy/failures.py`

**Interfaces:**
- Consumes: HEAD of `refactor/pass-anatomy`.
- Produces: the baseline file, whose "Evidence log" section every later task appends to; `failures.py`.

- [ ] **Step 1: Build and run the native suite**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -3; echo "build=$?"
(cd extension && scons -Q test) 2>&1 | tail -3; echo "native=$?"
```
Record the doctest `test cases:` line.

- [ ] **Step 2: Run the full gdUnit suite**

```bash
L=.superpowers/sdd/2026-09-15-pass-anatomy; mkdir -p $L
./gdunit_tests.sh 2>&1 | tee $L/baseline-full.log
grep -c " leaked" $L/baseline-full.log
```
Failing cases are normal (the exit code is not recorded: `tee` masks it). If the log shows `Could not find type "GdUnitTestCIRunner"` (the launcher failure SP2 recorded), **stop**: sub-project 2's runtime gates cannot be closed and the entry gate is not met.

- [ ] **Step 3: Write the extraction script**

`.superpowers/sdd/2026-09-15-pass-anatomy/failures.py`:

```python
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for suite in root.iter('testsuite'):
    print(f"# {suite.get('name')}: tests={suite.get('tests')} failures={suite.get('failures')}")
for tc in root.iter('testcase'):
    bad = tc.find('failure') if tc.find('failure') is not None else tc.find('error')
    if bad is not None:
        msg = (bad.get('message') or bad.text or '').strip().splitlines()
        print(f"{tc.get('classname')}::{tc.get('name')} — {msg[0] if msg else ''}")
```

```bash
latest=$(ls -d reports/report_* | sort -V | tail -1); echo "$latest"
python3 .superpowers/sdd/2026-09-15-pass-anatomy/failures.py "$latest/results.xml"
```

- [ ] **Step 4: Check SP2's open gates**

From Step 3's output, confirm that none of these suites has a failing case: `test_render_lifetime_contract`, `test_frame_shipped_golden`, `test_frame_contract`, `test_gpu_timing_scopes`. If any fails, **stop** and report: SP2 is not accepted (spec §4 step 1).

- [ ] **Step 5: Write the baseline file**

```markdown
# Pass anatomy — baseline

Commit: <`git rev-parse --short HEAD`>. Recorded <YYYY-MM-DD> on <OS / GPU>.
Report: <reports/report_N path>.

## Native
<doctest summary line>

## gdUnit per-suite counts
<every "# suite: tests=… failures=…" line>

## gdUnit failing cases
<every "suite::case — message" line, or "none">

Leak lines in the full run: <count>.
Flaky by case (compare failure COUNT): test_connectivity, test_island_body.

## SP2 runtime gates
- test_render_lifetime_contract: <pass / failing cases>
- test_frame_shipped_golden: <…>
- test_frame_contract: <…>
- test_gpu_timing_scopes: <…>

## Site rows
Filled by Tasks 2–4: per site, the failing cases and leak count of its Pass gate suite set on this commit.

## Evidence log
Appended by later tasks.
```
Paste real output into every angle-bracket field.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "docs: pass anatomy baseline and SP2 runtime gates"
```

---

### Task 2: Site rows and bite proofs — frame post passes

No production change is committed. Every break below is reverted before the next.

**Files:**
- Modify: `docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md` (Site rows, Evidence log)

**Interfaces:**
- Consumes: Task 1's baseline and `failures.py`.
- Produces: a Site row and a bite proof for ssao, contact_shadow, outline, ssr, ssgi, hiz, deferred.

- [ ] **Step 1: Record the Site rows**

For each site in this task, run its Pass gate suite set on the unmodified commit:

```bash
L=.superpowers/sdd/2026-09-15-pass-anatomy
./gdunit_tests.sh -a <site suites>,res://tests/test_frame_shipped_golden.gd,res://tests/test_frame_contract.gd,res://tests/test_render_lifetime_contract.gd 2>&1 | tee $L/row-<site>.log
grep -c " leaked" $L/row-<site>.log
latest=$(ls -d reports/report_* | sort -V | tail -1); python3 $L/failures.py "$latest/results.xml"
```
Append under "Site rows": `### <site>` then the failing-case lines (or `none`) and `leaked: <n>`.

- [ ] **Step 2: Bite proofs**

For each row: apply the break, rebuild, run only the named suite, confirm a failing case, `git checkout -- <file>`, rebuild.

| Site | Break (in `extension/src/render/`) | Must fail |
|---|---|---|
| ssao | `ssao_pass.cpp` `render()`: `f[5] = kSsaoStrength;` → `f[5] = 0.0f;` | `test_ssao_golden.gd` |
| contact_shadow | `contact_shadow_pass.cpp` `render()`: `params[1] = 0.85f;` → `params[1] = 0.0f;` | `test_contact_shadow.gd` |
| outline | `outline_pass.cpp` `render()`: `f[6] = 0.35f;` → `f[6] = 0.0f;` | `test_outline.gd` |
| ssr | `ssr_pass.cpp` `ensure_uniform_sets()`: `sampler_texture(2, nearest_, gb_surface)` ↔ `sampler_texture(3, nearest_, gb_depth)` resources swapped (binding 2 gets `gb_depth`, 3 gets `gb_surface`) | `test_ssr.gd` |
| ssgi | `ssgi_pass.cpp` `render()`: `f[22] = s.ssgi_strength;` → `f[22] = 0.0f;` | `test_ssgi.gd` or `test_emissive_gi.gd` |
| hiz | `hiz_pass.cpp` `build()`: `p[4] = m == 0 ? 1 : 0;` → `p[4] = 0;` | `test_hiz.gd` |
| deferred | `deferred_pass.cpp` `render()`: `u[24] = flags;` → `u[24] = 0;` | `test_deferred.gd` |

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -1
./gdunit_tests.sh -a res://tests/<suite>.gd 2>&1 | tail -20
git checkout -- extension/src/render/<file>
```

If the named suite does **not** fail, the site has no biting test. Add one before continuing: copy the pattern of `tests/test_grass_golden.gd` (a `const GOLDEN := {…}` of the numbers the suite's existing hook already returns, recorded on the unmodified commit, compared with `is_equal_approx` and a tolerance twice the spread of three repeated runs), commit it as `test: golden for <site> (characterization)`, and repeat the break.

- [ ] **Step 3: Record the proofs**

Append under "Evidence log":

```markdown
### Task 2 bite proofs
| Site | Break | Failing case — message | Reverted |
|---|---|---|---|
| ssao | f[5] = 0 | <suite::case — message> | yes |
```
One row per site, with real output.

- [ ] **Step 4: Commit**

```bash
git status --short extension/src   # must print nothing
git add docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md tests/
git commit -m "docs: frame post pass site rows and bite proofs"
```

---

### Task 3: Site rows and bite proofs — raster and frame compute

Same procedure as Task 2 Steps 1–4, for these sites (Site rows run first, then breaks):

**Files:**
- Modify: `docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md`

**Interfaces:**
- Consumes: Task 1's `failures.py`.
- Produces: Site rows and bite proofs for inject, grass_raster, lod_raster, sun_shadow, composite, island_cull, lod_cull, grass_scatter, raymarch, orchestrator.

- [ ] **Step 1: Record the Site rows** (Task 2 Step 1's commands, for the ten sites above)

- [ ] **Step 2: Bite proofs**

| Site | Break (in `extension/src/render/`) | Must fail |
|---|---|---|
| inject | `inject_pass.cpp` `draw()`: delete `rd->draw_list_draw(dl, false, 1, 3);` | `test_gbuffer.gd` or `test_frame_shipped_golden.gd` |
| grass_raster | `grass_raster_pass.cpp` `draw()`: `f[16] = cam_pos[0];` → `f[16] = 0.0f;` | `test_grass_golden.gd` |
| lod_raster | `lod_raster_pass.cpp` `draw()`: `f[20] = fade_end;` → `f[20] = fade_start;` | `test_lod_gbuffer.gd` or `test_lod_seam.gd` |
| sun_shadow | `sun_shadow_pass.cpp` `build()`: `f[i] = ortho.view_proj[i];` → `f[i] = 0.0f;` | `test_sun_shadow.gd` |
| composite | `composite_pass.cpp` `draw()`: `u0->add_id(src_overlay);` → `u0->add_id(src_surface);` | `test_raymarch_gbuffer.gd` |
| island_cull | `island_cull_pass.cpp` `render()`: `pc.dims[3] = island_count;` → `pc.dims[3] = 0;` | `test_island_render.gd` |
| lod_cull | `lod_cull_pass.cpp` `run()`: `ip[0] = page_count;` → `ip[0] = 0;` | `test_lod_cull.gd` |
| grass_scatter | `grass_scatter_pass.cpp` `run()`: `if (scatter_pipeline_.is_valid()) {` → `if (false) {` | `test_grass.gd` |
| raymarch | `raymarch_pass.cpp` `render()`: `(width + kRaymarchGroupX - 1) / kRaymarchGroupX` → `(width + kRaymarchGroupX - 1) / (2 * kRaymarchGroupX)` | `test_raymarch_pixel.gd` or `test_raymarch_gbuffer.gd` |
| orchestrator (downsample) | `orchestrator.cpp` `downsample_history()`: `dims[0] = half.x;` → `dims[0] = 1;` | `test_ssgi.gd` or `test_gbuffer.gd` |
| orchestrator (pre-flight) | `orchestrator.cpp` `preflight_shaders()`: delete both `stage = …` reassignments (every file compiles as compute) | `test_shader_reload.gd` or `test_pipeline_reload.gd` |

The missing-test rule of Task 2 Step 2 applies.

- [ ] **Step 3: Record the proofs** under `### Task 3 bite proofs` (Task 2 Step 3's table format).

- [ ] **Step 4: Commit**

```bash
git status --short extension/src   # must print nothing
git add docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md tests/
git commit -m "docs: raster and frame compute site rows and bite proofs"
```

---

### Task 4: Site rows and bite proofs — world jobs

Same procedure as Task 2 Steps 1–4.

**Files:**
- Modify: `docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md`

**Interfaces:**
- Consumes: Task 1's `failures.py`.
- Produces: Site rows and bite proofs for brick_gen, region, consolidate, island_extract, mesh, lod_build.

- [ ] **Step 1: Record the Site rows** (Task 2 Step 1's commands, for the six sites)

- [ ] **Step 2: Bite proofs**

Each break deletes one `rd->compute_list_dispatch(` statement (the whole statement, which may span lines) inside the named function.

| Site | Function (in `extension/src/render/`) | Must fail |
|---|---|---|
| brick_gen | `brick_gen_pass.cpp` `BrickGenPass::dispatch` | `test_brick_diff.gd` |
| region | `region_pass.cpp` `RegionPass::mark` | `test_region_pass.gd` or `test_streaming.gd` |
| consolidate | `consolidate_pass.cpp` `ConsolidatePass::run` | `test_consolidation.gd` |
| island_extract | `island_extract_pass.cpp` `IslandExtractPass::extract` | `test_island_extract.gd` |
| mesh | `mesh_pass.cpp` `MeshPass::record_quads` | `test_mesh_diff.gd` |
| lod_build | `lod_build_pass.cpp` `LodBuildPass::record_quads` | `test_lod_build.gd` or `test_lod_mesh_diff.gd` |

The missing-test rule of Task 2 Step 2 applies.

- [ ] **Step 3: Record the proofs** under `### Task 4 bite proofs`.

- [ ] **Step 4: Commit**

```bash
git status --short extension/src   # must print nothing
git add docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md tests/
git commit -m "docs: world job site rows and bite proofs"
```

---

### Task 5: S7 — grass blades write a foliage id

Two commits: the failing test, then the fix on today's code.

**Files:**
- Modify: `extension/src/debug/hooks_render.cpp` (`debug_grass_stats`: `blade_materials` key)
- Modify: `tests/test_grass.gd`
- Modify: `extension/src/world/material_table.h`, `extension/src/world/material_table.cpp`
- Modify: `shaders/material_table.glslh` (regenerated)
- Modify: `shaders/grass.frag.glsl`
- Test: `extension/tests/test_material_table.cpp`, `extension/tests/test_material_glslh.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `ve::FoliageDef`, `ve::kFoliage`, `ve::kFoliageCount`, `ve::kFoliageBase` (= 200) in `world/material_table.h`; `material_glow` covering both ranges; GLSL `FOLIAGE_BASE`, `FOLIAGE_COUNT`, `MAT_GRASS_BLADE`, `FOLIAGE_GLOW`, `FOLIAGE_GLOW_RGB` in `shaders/material_table.glslh`. Task 38 adds `MAT_<terrain>` names beside `MAT_GRASS_BLADE`.

- [ ] **Step 1: Report the ids the blade raster writes**

In `extension/src/debug/hooks_render.cpp`, `VoxelDebugHooks::debug_grass_stats()`:

After `d["mean_luma"] = -1.0;` add:

```cpp
	// The material ids the hooked blade raster wrote into surface.z, ascending. Material 0 is
	// the cleared background, so only covered pixels report. S7 pins this: a blade must write
	// its own foliage id, never the terrain material it grows on.
	d["blade_materials"] = PackedInt32Array();
```

Inside the drive, directly after the closing brace of `if (alb.size() >= pixels * 4) { … }`, add:

```cpp
			const PackedByteArray surf = device->texture_get_data(
					w->context().render->passes().gbuffer->surface(), 0);
			if (surf.size() >= pixels * 8) {
				const uint16_t *s = reinterpret_cast<const uint16_t *>(surf.ptr());
				std::set<int> seen;
				for (int i = 0; i < pixels; i++) {
					const float z = half_to_float(s[i * 4 + 2]);
					if (z >= 0.5f) seen.insert(static_cast<int>(z + 0.5f));
				}
				PackedInt32Array ids;
				for (int id : seen) ids.push_back(id);
				d["blade_materials"] = ids;
			}
```

Add `#include <set>` and `#include <godot_cpp/variant/packed_int32_array.hpp>` to the file's includes if absent.

- [ ] **Step 2: Write the failing GPU test**

Append to `tests/test_grass.gd`:

```gdscript
# S7 (docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md §3.5). Blades
# used to write material 1 -- grass_01, the ground they grow on -- so an emissive grass_01
# would light every blade. They write their own foliage id instead. 200 is ve::kFoliageBase
# (extension/src/world/material_table.h) and grass_blade is its first row.
const MAT_GRASS_BLADE := 200

func test_blades_write_the_grass_blade_material_not_the_terrain_they_grow_on() -> void:
	var w := make_world()
	stream_to(w, OPEN_GRASS)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["drawn"]).is_true()
	var ids: PackedInt32Array = d["blade_materials"]
	assert_int(ids.size()).override_failure_message(
		"the hooked raster covered no pixels").is_greater(0)
	assert_array(Array(ids)).override_failure_message(
		"blade material ids: %s" % [ids]).is_equal([MAT_GRASS_BLADE])
```

- [ ] **Step 3: Run it and confirm it fails for the right reason**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -1
./gdunit_tests.sh -a res://tests/test_grass.gd 2>&1 | grep -A3 "blade_material"
```
Expected: `test_blades_write_the_grass_blade_material_not_the_terrain_they_grow_on` FAILS with `blade material ids: [1]`. Every other `test_grass.gd` case matches the grass_raster Site row.

- [ ] **Step 4: Commit the failing test**

```bash
git add extension/src/debug/hooks_render.cpp tests/test_grass.gd
git commit -m "test: blades must write their own foliage id (S7, failing)"
```

- [ ] **Step 5: Write the failing native tests**

Append to `extension/tests/test_material_table.cpp`:

```cpp
TEST_CASE("foliage ids sit above every terrain id and look up their own glow") {
	CHECK(ve::kFoliageBase == 200);
	CHECK(ve::kMaterialCount < ve::kFoliageBase);
	REQUIRE(ve::kFoliageCount >= 1);
	CHECK(std::string(ve::kFoliage[0].name) == "grass_blade");
	CHECK(ve::material_glow(ve::kFoliageBase) == doctest::Approx(ve::kFoliage[0].glow));
	CHECK(ve::material_glow(static_cast<uint16_t>(ve::kFoliageBase + ve::kFoliageCount)) ==
			doctest::Approx(0.0f));
	CHECK(ve::material_glow(static_cast<uint16_t>(ve::kFoliageBase - 1)) == doctest::Approx(0.0f));
}
```
(add `#include <string>` if absent).

Append to `extension/tests/test_material_glslh.cpp`:

```cpp
TEST_CASE("the emitter names every foliage row and gives mat_glow both ranges") {
	const std::string s = ve::material_table_glsl();
	CHECK(s.find("const uint FOLIAGE_BASE = 200u;") != std::string::npos);
	CHECK(s.find("const uint MAT_GRASS_BLADE = 200u;") != std::string::npos);
	CHECK(s.find("FOLIAGE_GLOW[j]") != std::string::npos);
	CHECK(s.find("FOLIAGE_GLOW_RGB[j]") != std::string::npos);
}
```

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: build error `'kFoliageBase' is not a member of 've'` (or equivalent).

- [ ] **Step 6: Add the foliage table**

In `extension/src/world/material_table.h`, after the `kMaterialCount` definition, add:

```cpp
// Foliage draws its own albedo and grows ON terrain rather than being terrain, so it has no
// atlas layer, no flat albedo and no hardness. Its ids start at kFoliageBase, far above every
// terrain id: adding a terrain material never renumbers foliage, and no foliage id can reach
// an atlas lookup (every GLSL table lookup is range-checked). A new grass or leaf type is one
// row here plus its shader.
struct FoliageDef {
	const char *name;
	float glow; // emissive strength; 0.0 = not emissive
	float glow_rgb[3];
};

inline constexpr uint16_t kFoliageBase = 200;

inline constexpr FoliageDef kFoliage[] = {
	// name          glow  glow_rgb
	{"grass_blade",  0.0f, {0.0f, 0.0f, 0.0f}},
};

inline constexpr int kFoliageCount = static_cast<int>(sizeof(kFoliage) / sizeof(kFoliage[0]));
static_assert(kMaterialCount < kFoliageBase, "terrain material ids would reach the foliage range");
```

Change the comment above `material_hardness`/`material_glow` to:

```cpp
// Fail soft for air (0) and any id with no table entry: full-size removal, no emission.
// material_glow also answers for foliage ids (kFoliageBase + k).
```

In `extension/src/world/material_table.cpp`, replace `material_glow`:

```cpp
float material_glow(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i >= 0 && i < kMaterialCount) return kMaterials[i].glow;
	const int j = static_cast<int>(id) - static_cast<int>(kFoliageBase);
	return (j >= 0 && j < kFoliageCount) ? kFoliage[j].glow : 0.0f;
}
```

In `extension/src/world/material_table.cpp`, add to the anonymous namespace:

```cpp
std::string upper(const char *name) {
	std::string s(name);
	for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return s;
}
```
(add `#include <cctype>`).

In `material_table_glsl()`, insert immediately before the line `o << "// Mirror of ve::material_glow, including its fail-soft rule: air and any id with\n"`:

```cpp
	o << "// Foliage (ve::kFoliage): ids FOLIAGE_BASE + k. No atlas layer and no flat albedo -- a\n"
	     "// foliage raster writes its own colour -- so only the emission tables exist.\n"
	     "const uint FOLIAGE_BASE = " << kFoliageBase << "u;\n"
	     "const int FOLIAGE_COUNT = " << kFoliageCount << ";\n";
	for (int k = 0; k < kFoliageCount; k++)
		o << "const uint MAT_" << upper(kFoliage[k].name) << " = " << (kFoliageBase + k) << "u;\n";
	o << "\n";

	o << "const float FOLIAGE_GLOW[FOLIAGE_COUNT] = float[FOLIAGE_COUNT](\n";
	for (int k = 0; k < kFoliageCount; k++)
		o << "\t" << f(kFoliage[k].glow) << (k + 1 < kFoliageCount ? "," : "")
		  << " // " << kFoliage[k].name << "\n";
	o << ");\n\n";

	o << "const vec3 FOLIAGE_GLOW_RGB[FOLIAGE_COUNT] = vec3[FOLIAGE_COUNT](\n";
	for (int k = 0; k < kFoliageCount; k++)
		o << "\t" << vec3(kFoliage[k].glow_rgb) << (k + 1 < kFoliageCount ? "," : "")
		  << " // " << kFoliage[k].name << "\n";
	o << ");\n\n";
```

Replace the two emitted lookup functions (from `"float mat_glow(uint id) {\n"` to the final `"}\n";`) with:

```cpp
	     "float mat_glow(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\tif (i >= 0 && i < MATERIAL_COUNT) return MAT_GLOW[i];\n"
	     "\tint j = int(id) - int(FOLIAGE_BASE);\n"
	     "\treturn (j >= 0 && j < FOLIAGE_COUNT) ? FOLIAGE_GLOW[j] : 0.0;\n"
	     "}\n\n"
	     "vec3 mat_glow_rgb(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\tif (i >= 0 && i < MATERIAL_COUNT) return MAT_GLOW_RGB[i];\n"
	     "\tint j = int(id) - int(FOLIAGE_BASE);\n"
	     "\treturn (j >= 0 && j < FOLIAGE_COUNT) ? FOLIAGE_GLOW_RGB[j] : vec3(0.0);\n"
	     "}\n";
```

- [ ] **Step 7: Regenerate the committed mirror**

```bash
(cd extension && scons -Q test) 2>&1 | grep -A200 "is stale. Replace its entire contents with:" | head -120
```
Replace the whole of `shaders/material_table.glslh` with the printed text (everything after the `with:` line, up to the doctest location line). Re-run: `(cd extension && scons -Q test) 2>&1 | tail -3` — all pass.

- [ ] **Step 8: Blades write their own id**

In `shaders/grass.frag.glsl`, delete the line `const uint GRASS_MATERIAL = 1u;` and change

```glsl
	out_surface = vec4(oct_encode(n), float(GRASS_MATERIAL), pc.style.y);
```
to
```glsl
	out_surface = vec4(oct_encode(n), float(MAT_GRASS_BLADE), pc.style.y);
```

`shaders/grass_scatter.comp.glsl`'s `GRASS_MATERIAL` (where blades grow) is unchanged here; Task 38 renames it.

- [ ] **Step 9: Confirm no reader indexes a table with a foliage id unguarded**

```bash
rg -n "mat_glow|mat_glow_rgb|flat_material_albedo|material_surface\(|material_props" shaders --glob '*.glsl'
```
Expected hits and why each is safe (record in the commit message): `deferred.comp.glsl` `mat_glow`/`mat_glow_rgb` (both ranges now), `material_surface` there runs only when glow > 0 and range-checks the layer; `ssgi.comp.glsl` `mat_glow`/`mat_glow_rgb`; `composite.frag.glsl` reads the raymarch's own intermediate surface, never the G-buffer; `lod.frag.glsl` and `raymarch.comp.glsl` look up terrain ids they computed. Any other hit: stop and report.

- [ ] **Step 10: Pass gate**

Run the Pass gate for **grass_raster**, adding `res://tests/test_deferred.gd,res://tests/test_ssgi.gd,res://tests/test_emissive_gi.gd,res://tests/test_material_glow.gd` to the suite list. Expected: `test_blades_write_the_grass_blade_material_not_the_terrain_they_grow_on` now PASSES; `test_grass_golden.gd` unchanged (luma reads albedo only); everything else matches the rows.

- [ ] **Step 11: Commit the fix**

```bash
git add extension/src/world/material_table.h extension/src/world/material_table.cpp \
	shaders/material_table.glslh shaders/grass.frag.glsl \
	extension/tests/test_material_table.cpp extension/tests/test_material_glslh.cpp \
	docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "fix: grass blades write the grass_blade foliage id, not grass_01 (S7)

Foliage ids start at ve::kFoliageBase = 200; mat_glow and mat_glow_rgb look up both ranges.
Readers audited: <paste Step 9's list>."
```

---

### Task 6: S4 — cel-shaded objects follow the scene sun

Two commits: the failing test, then the fix.

**Files:**
- Modify: `tests/test_cel_object.gd`
- Modify: `project.godot`
- Modify: `shaders/cel_object.gdshader`, `shaders/cel.gdshaderinc`
- Modify: `extension/src/voxel_world.cpp` (`update_sun_state`)
- Modify: `extension/src/physics/island_body.cpp`
- Modify: `demo/main.tscn`

**Interfaces:**
- Consumes: `VoxelWorld.sun_light_path` (bound property), `ve::SunState`, `DeferredPass::kAmbient`.
- Produces: global shader uniforms `ve_sun_dir`, `ve_sun_rgb`, `ve_ambient`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_cel_object.gd`:

```gdscript
# S4 (docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md §3.7). The quad
# faces +Z, toward the camera. A DirectionalLight3D emits along its local -Z, so a light
# looking down -Z puts the sun behind the camera (N.L = 1, brightest band) and one looking
# down +Z puts it behind the quad (N.L = -1, darkest band). Objects used to shade against a
# fixed VE_SUN_DIR and ignore the light entirely.
func render_once(probe: Dictionary) -> Color:
	var vp: SubViewport = probe["viewport"]
	vp.render_target_update_mode = SubViewport.UPDATE_ONCE
	await RenderingServer.frame_post_draw
	return vp.get_texture().get_image().get_pixel(8, 8)

func test_object_lighting_follows_the_scene_sun() -> void:
	var w := make_world()
	var probe := make_probe()
	var m: ShaderMaterial = probe["material"]
	m.set_shader_parameter("probe_mode", false)
	var light := DirectionalLight3D.new()
	add_child(light); _nodes.append(light)
	w.sun_light_path = w.get_path_to(light)
	light.look_at_from_position(Vector3.ZERO, Vector3(0, 0, -1), Vector3.UP)
	await get_tree().process_frame
	await get_tree().process_frame
	var toward: Color = await render_once(probe)
	light.look_at_from_position(Vector3.ZERO, Vector3(0, 0, 1), Vector3.UP)
	await get_tree().process_frame
	await get_tree().process_frame
	var away: Color = await render_once(probe)
	assert_float(toward.g - away.g).override_failure_message(
		"sun toward the camera %s, behind the quad %s" % [toward, away]).is_greater(0.2)
```

- [ ] **Step 2: Run it and confirm it fails**

```bash
./gdunit_tests.sh -a res://tests/test_cel_object.gd 2>&1 | grep -B2 -A4 "follows_the_scene_sun"
```
Expected: FAIL, the two colours equal (the shader ignores the light).

- [ ] **Step 3: Commit the failing test**

```bash
git add tests/test_cel_object.gd
git commit -m "test: cel objects must follow the scene sun (S4, failing)"
```

- [ ] **Step 4: Declare the globals**

Append to `project.godot` (a new section; if `[shader_globals]` already exists, add the three entries to it):

```ini
[shader_globals]

ve_sun_dir={
"type": "vec3",
"value": Vector3(0.574696, 0.766261, 0.287348)
}
ve_sun_rgb={
"type": "vec3",
"value": Vector3(1, 1, 1)
}
ve_ambient={
"type": "vec3",
"value": Vector3(0.16, 0.19, 0.26)
}
```

- [ ] **Step 5: The shader reads them**

`shaders/cel_object.gdshader`: delete the line `uniform vec3 ambient_linear=vec3(.16,.19,.26);`, add after the `#include` line:

```glsl
// Set every frame from the scene's DirectionalLight3D by VoxelWorld::update_sun_state (S4);
// declared in project.godot [shader_globals]. The terrain reads the same sun from its UBO.
global uniform vec3 ve_sun_dir;
global uniform vec3 ve_sun_rgb;
global uniform vec3 ve_ambient;
```

and replace the non-probe branch of `fragment()`:

```glsl
	}else{
		vec3 n=normalize(ve_world_n),v=normalize(INV_VIEW_MATRIX[3].xyz-ve_world_pos);
		vec3 l=normalize(ve_sun_dir);
		shaded=ve_cel_shade(base_color_linear,ve_ambient,dot(n,l),dot(n,v),
				dot(n,normalize(l+v)),shadow_visibility,ambient_occlusion,gloss,ve_sun_rgb);
	}
```

`shaders/cel.gdshaderinc`: delete line 1, `const vec3 VE_SUN_DIR = vec3(0.5746958, 0.7662610, 0.2873479);`.

- [ ] **Step 6: Publish the sun on the main thread**

`extension/src/voxel_world.cpp`, in `VoxelWorld::update_sun_state()`, after `context_.render->set_sun_state(s);`:

```cpp
	// S4: cel-shaded objects (shaders/cel_object.gdshader) read the same sun through global
	// shader uniforms declared in project.godot, set here on the main thread beside the UBO
	// publish the terrain reads. Ambient is DeferredPass's, so both paths share one number.
	if (RenderingServer *server = RenderingServer::get_singleton()) {
		server->global_shader_parameter_set("ve_sun_dir", Vector3(s.dir[0], s.dir[1], s.dir[2]));
		server->global_shader_parameter_set("ve_sun_rgb", Vector3(s.rgb[0], s.rgb[1], s.rgb[2]));
		server->global_shader_parameter_set("ve_ambient", Vector3(DeferredPass::kAmbient[0],
				DeferredPass::kAmbient[1], DeferredPass::kAmbient[2]));
	}
```
Add `#include <godot_cpp/classes/rendering_server.hpp>` if absent (`render/deferred_pass.h` is already included).

`extension/src/physics/island_body.cpp`: delete `cel->set_shader_parameter("ambient_linear", Vector3(0.16f, 0.19f, 0.26f));`.

`demo/main.tscn`: delete the line `shader_parameter/ambient_linear = Vector3(0.16, 0.19, 0.26)`.

```bash
rg -n "ambient_linear|VE_SUN_DIR" shaders demo extension/src tests
```
Expected: no output.

- [ ] **Step 7: Verify**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -1
./gdunit_tests.sh -a res://tests/test_cel_object.gd,res://tests/test_island_body.gd,res://tests/test_deferred.gd,res://tests/test_demo_shell.gd 2>&1 | tee .superpowers/sdd/2026-09-15-pass-anatomy/s4.log | tail -20
```
Expected: `test_object_lighting_follows_the_scene_sun` PASSES; `test_shaderlanguage_matches_ve_cel_shade` still passes (probe mode is unchanged); the other suites match Task 1's failing set.

- [ ] **Step 8: Commit the fix**

```bash
git add project.godot shaders/cel_object.gdshader shaders/cel.gdshaderinc \
	extension/src/voxel_world.cpp extension/src/physics/island_body.cpp demo/main.tscn
git commit -m "fix: cel objects light from the scene sun through global shader uniforms (S4)

The hard-coded island rock albedo stays: IslandBody carries no material data (spec §3.7)."
```

---

### Task 7: The lifetime core — `gpu_core.h`, test-first

Pure code; no pass changes.

**Files:**
- Create: `extension/src/render/gpu/gpu_core.h`
- Create: `extension/tests/test_gpu_core.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (namespace `ve::gpu`):
  - `enum class UniformType : uint8_t { SamplerWithTexture = 1, Image = 3, UniformBuffer = 7, StorageBuffer = 8 };`
  - `enum class Kind : uint8_t { UniformSet, IndexArray, Framebuffer, Pipeline, Shader, Sampler, Texture, Buffer };` (declaration order = free order)
  - `template <class Id> struct Uniform { UniformType type; uint32_t binding; Id ids[2]; uint8_t count; };` with `operator==`
  - `sampled(binding, sampler, texture)`, `image(binding, texture)`, `ubo(binding, buffer)`, `storage(binding, buffer)` → `Uniform<Id>`
  - `template <class Device> class ResourceGroup { Id add(Kind, const Id &); void free(Device &, const Id &); void release(Device &); size_t size() const; };`
  - `template <class Device> class UniformSetCache { Id get(Device &, ResourceGroup<Device> &, const Id &shader, uint32_t set, std::vector<Uniform<Id>>); void drop(Device &, ResourceGroup<Device> &); const Id &id() const; };`
  - `template <class Device> Id uniform_set(Device &, ResourceGroup<Device> &, const Id &shader, uint32_t set, const std::vector<Uniform<Id>> &)`
  - A `Device` supplies `using Id`, `bool alive(Kind, const Id &)`, `void free(const Id &)`, `Id create_uniform_set(const Id &shader, uint32_t set, const std::vector<Uniform<Id>> &)`.

- [ ] **Step 1: Write the failing tests**

`extension/tests/test_gpu_core.cpp`:

```cpp
#include <doctest/doctest.h>
#include "render/gpu/gpu_core.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

using ve::gpu::Kind;
using U = ve::gpu::Uniform<uint64_t>;

namespace {

// The two RenderingDevice rules gpu_core.h is written against: freeing a resource kills
// everything that references it (a uniform set dies with its shader and with any id it binds;
// a pipeline with its shader; a framebuffer with its attachments), and freeing a dead id is an
// error -- counted here, so a test can assert it never happens.
struct FakeDevice {
	using Id = uint64_t;
	struct Res {
		Kind kind;
		std::vector<uint64_t> refs;
		bool alive = true;
	};
	std::map<uint64_t, Res> res;
	std::vector<uint64_t> freed;
	uint64_t next = 1;
	int bad_frees = 0;
	int sets_created = 0;

	uint64_t make(Kind kind, std::vector<uint64_t> refs = {}) {
		res[next] = Res{kind, std::move(refs)};
		return next++;
	}
	bool alive(Kind, const uint64_t &id) {
		const auto it = res.find(id);
		return it != res.end() && it->second.alive;
	}
	void kill(uint64_t id) {
		res[id].alive = false;
		for (auto &[other, r] : res)
			if (r.alive && std::find(r.refs.begin(), r.refs.end(), id) != r.refs.end()) kill(other);
	}
	void free(const uint64_t &id) {
		if (!alive(Kind::Buffer, id)) {
			bad_frees++;
			return;
		}
		freed.push_back(id);
		kill(id);
	}
	uint64_t create_uniform_set(const uint64_t &shader, uint32_t, const std::vector<U> &uniforms) {
		std::vector<uint64_t> refs{shader};
		for (const U &u : uniforms)
			for (uint8_t i = 0; i < u.count; i++) refs.push_back(u.ids[i]);
		for (uint64_t r : refs)
			if (!alive(Kind::Buffer, r)) return 0;
		sets_created++;
		return make(Kind::UniformSet, refs);
	}
	int live() const {
		int n = 0;
		for (const auto &[id, r] : res) n += r.alive ? 1 : 0;
		return n;
	}
};

using Group = ve::gpu::ResourceGroup<FakeDevice>;
using Cache = ve::gpu::UniformSetCache<FakeDevice>;

U img(uint32_t binding, uint64_t texture) { return ve::gpu::image<uint64_t>(binding, texture); }

} // namespace

TEST_CASE("a uniform set is reused while its shader, set index and every bound id match") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t a = c.get(d, g, shader, 0, {img(0, tex)});
	const uint64_t b = c.get(d, g, shader, 0, {img(0, tex)});
	CHECK(a != 0);
	CHECK(a == b);
	CHECK(c.id() == a);
	CHECK(d.sets_created == 1);
}

TEST_CASE("a changed id, binding, shader or set index rebuilds and frees the old set") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t s1 = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t s2 = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t t1 = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t t2 = g.add(Kind::Texture, d.make(Kind::Texture));
	uint64_t prev = c.get(d, g, s1, 0, {img(0, t1)});
	auto rebuilt = [&](uint64_t now) {
		CHECK(now != 0);
		CHECK(now != prev);
		CHECK_FALSE(d.alive(Kind::UniformSet, prev));
		prev = now;
	};
	rebuilt(c.get(d, g, s1, 0, {img(0, t2)}));
	rebuilt(c.get(d, g, s1, 0, {img(1, t2)}));
	rebuilt(c.get(d, g, s2, 0, {img(1, t2)}));
	rebuilt(c.get(d, g, s2, 2, {img(1, t2)}));
	CHECK(d.bad_frees == 0);
	CHECK(g.size() == 5); // two shaders, two textures, the one live set
}

TEST_CASE("a set its texture took down is rebuilt, and nothing is freed twice") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t first = c.get(d, g, shader, 0, {img(0, tex)});
	g.free(d, tex); // a resize: the old target goes and takes the set with it
	CHECK_FALSE(d.alive(Kind::UniformSet, first));
	tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t second = c.get(d, g, shader, 0, {img(0, tex)});
	CHECK(second != first);
	CHECK(d.alive(Kind::UniformSet, second));
	CHECK(d.bad_frees == 0);
}

TEST_CASE("a set that died while its key still matches is rebuilt, not reused") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t first = c.get(d, g, shader, 0, {img(0, tex)});
	d.kill(first); // the device dropped it for a reason this pass cannot see
	const uint64_t second = c.get(d, g, shader, 0, {img(0, tex)});
	CHECK(second != first);
	CHECK(d.alive(Kind::UniformSet, second));
	CHECK(d.bad_frees == 0);
}

TEST_CASE("a failed create returns none, keeps no key and retries on the next call") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t dead = d.make(Kind::Texture);
	d.free(dead);
	CHECK(c.get(d, g, shader, 0, {img(0, dead)}) == 0);
	CHECK(c.id() == 0);
	CHECK(g.size() == 1);
	const uint64_t live = g.add(Kind::Texture, d.make(Kind::Texture));
	CHECK(c.get(d, g, shader, 0, {img(0, live)}) != 0);
}

TEST_CASE("drop is idempotent, safe after a cascade and a no-op after release") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	c.get(d, g, shader, 0, {img(0, tex)});
	g.free(d, tex);
	c.drop(d, g);
	c.drop(d, g);
	CHECK(c.id() == 0);
	CHECK(d.bad_frees == 0);
	const uint64_t tex2 = g.add(Kind::Texture, d.make(Kind::Texture));
	c.get(d, g, shader, 0, {img(0, tex2)});
	g.release(d);
	c.drop(d, g);
	CHECK(d.bad_frees == 0);
}

TEST_CASE("release frees dependents first whatever the registration order, and leaves nothing") {
	FakeDevice d;
	Group g;
	Cache c;
	// Registered dependencies-first on purpose: release must not rely on registration order.
	const uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t buf = g.add(Kind::Buffer, d.make(Kind::Buffer));
	const uint64_t smp = g.add(Kind::Sampler, d.make(Kind::Sampler));
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t pipeline = g.add(Kind::Pipeline, d.make(Kind::Pipeline, {shader}));
	const uint64_t fb = g.add(Kind::Framebuffer, d.make(Kind::Framebuffer, {tex}));
	const uint64_t set = c.get(d, g, shader, 0,
			{ve::gpu::sampled<uint64_t>(0, smp, tex), ve::gpu::storage<uint64_t>(1, buf)});
	g.release(d);
	CHECK(d.bad_frees == 0);
	CHECK(d.live() == 0);
	CHECK(g.size() == 0);
	CHECK(d.freed == std::vector<uint64_t>{set, fb, pipeline, shader, smp, tex, buf});
}

TEST_CASE("within one kind, release frees the newest first") {
	FakeDevice d;
	Group g;
	const uint64_t parent = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t view = g.add(Kind::Texture, d.make(Kind::Texture, {parent}));
	g.release(d);
	CHECK(d.freed == std::vector<uint64_t>{view, parent});
	CHECK(d.bad_frees == 0);
}

TEST_CASE("release skips what a cascade already took, and a group is reusable after it") {
	FakeDevice d;
	Group g;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	g.add(Kind::Pipeline, d.make(Kind::Pipeline, {shader}));
	d.free(shader); // the pipeline dies with it
	g.release(d);
	CHECK(d.bad_frees == 0);
	CHECK(g.size() == 0);
	const uint64_t again = g.add(Kind::Texture, d.make(Kind::Texture));
	g.release(d);
	CHECK_FALSE(d.alive(Kind::Texture, again));
	CHECK(d.live() == 0);
}

TEST_CASE("add ignores none, free ignores ids the group does not own") {
	FakeDevice d;
	Group g;
	CHECK(g.add(Kind::Texture, 0) == 0);
	CHECK(g.size() == 0);
	const uint64_t foreign = d.make(Kind::Buffer);
	g.free(d, foreign);
	CHECK(d.alive(Kind::Buffer, foreign));
	CHECK(d.bad_frees == 0);
}

TEST_CASE("uniform_set builds a set once and registers it for release") {
	FakeDevice d;
	Group g;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t buf = g.add(Kind::Buffer, d.make(Kind::Buffer));
	const uint64_t set = ve::gpu::uniform_set(d, g, shader, 0, {ve::gpu::storage<uint64_t>(0, buf)});
	CHECK(set != 0);
	CHECK(g.size() == 3);
	g.release(d);
	CHECK(d.live() == 0);
}
```

- [ ] **Step 2: Run and confirm the build fails**

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: `fatal error: render/gpu/gpu_core.h: No such file or directory`.

- [ ] **Step 3: Write the core**

`extension/src/render/gpu/gpu_core.h`:

```cpp
#pragma once
// Lifetime rules every GPU pass shares, with no godot-cpp dependency so the native suite can
// drive them against a fake device (extension/tests/test_gpu_core.cpp). The RenderingDevice
// binding is render/gpu/gpu.h.
//
// A Device supplies:
//   using Id = ...;                  // copyable; == and !=; Id{} means "none"
//   bool alive(Kind, const Id &);    // false once freed, directly or by cascade
//   void free(const Id &);
//   Id create_uniform_set(const Id &shader, uint32_t set, const std::vector<Uniform<Id>> &);
//
// RenderingDevice frees dependents with their dependency: a uniform set dies with its shader
// and with any texture, buffer or sampler it binds; a pipeline with its shader; a framebuffer
// with any attachment. Freeing an id that already died is an error. So every free below asks
// alive() first, and release() frees dependents before what they reference.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ve::gpu {

// Values are RenderingDevice::UniformType's; render/gpu/gpu.cpp static_asserts each.
enum class UniformType : uint8_t {
	SamplerWithTexture = 1,
	Image = 3,
	UniformBuffer = 7,
	StorageBuffer = 8,
};

// Declaration order is free order: nothing here references a kind declared after it.
enum class Kind : uint8_t { UniformSet, IndexArray, Framebuffer, Pipeline, Shader, Sampler, Texture, Buffer };
inline constexpr int kKindCount = 8;

template <class Id>
struct Uniform {
	UniformType type = UniformType::Image;
	uint32_t binding = 0;
	Id ids[2] = {};
	uint8_t count = 0;
	bool operator==(const Uniform &) const = default;
};

template <class Id>
Uniform<Id> sampled(uint32_t binding, const Id &sampler, const Id &texture) {
	return {UniformType::SamplerWithTexture, binding, {sampler, texture}, 2};
}

template <class Id>
Uniform<Id> image(uint32_t binding, const Id &texture) {
	return {UniformType::Image, binding, {texture, Id{}}, 1};
}

template <class Id>
Uniform<Id> ubo(uint32_t binding, const Id &buffer) {
	return {UniformType::UniformBuffer, binding, {buffer, Id{}}, 1};
}

template <class Id>
Uniform<Id> storage(uint32_t binding, const Id &buffer) {
	return {UniformType::StorageBuffer, binding, {buffer, Id{}}, 1};
}

// Everything one pass owns on one device. A pass registers each RID as it creates it; its
// teardown is one release().
template <class Device>
class ResourceGroup {
public:
	using Id = typename Device::Id;

	Id add(Kind kind, const Id &id) {
		if (id != Id{}) entries_.push_back({kind, id});
		return id;
	}

	// Frees one owned id now (if the device still has it) and forgets it. Ids the group does
	// not own are ignored.
	void free(Device &device, const Id &id) {
		for (size_t i = entries_.size(); i-- > 0;) {
			if (entries_[i].id != id) continue;
			if (device.alive(entries_[i].kind, id)) device.free(id);
			entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
			return;
		}
	}

	// Kind by kind in declaration order; within a kind, newest first, so a texture view
	// registered after its parent goes before it.
	void release(Device &device) {
		for (int k = 0; k < kKindCount; k++)
			for (size_t i = entries_.size(); i-- > 0;) {
				const Entry &e = entries_[i];
				if (static_cast<int>(e.kind) == k && device.alive(e.kind, e.id)) device.free(e.id);
			}
		entries_.clear();
	}

	size_t size() const { return entries_.size(); }

private:
	struct Entry {
		Kind kind;
		Id id;
	};
	std::vector<Entry> entries_;
};

// One uniform set whose inputs can change. get() returns the cached set while the shader, the
// set index and every bound id match and the device still has it; otherwise it frees the old
// set (if alive) and builds a new one. Godot never reuses an RID, so a recreated texture is a
// changed key by itself.
template <class Device>
class UniformSetCache {
public:
	using Id = typename Device::Id;

	Id get(Device &device, ResourceGroup<Device> &group, const Id &shader, uint32_t set,
			std::vector<Uniform<Id>> uniforms) {
		if (set_ != Id{} && shader == shader_ && set == index_ && uniforms == key_ &&
				device.alive(Kind::UniformSet, set_))
			return set_;
		drop(device, group);
		set_ = group.add(Kind::UniformSet, device.create_uniform_set(shader, set, uniforms));
		if (set_ != Id{}) {
			shader_ = shader;
			index_ = set;
			key_ = std::move(uniforms);
		}
		return set_;
	}

	void drop(Device &device, ResourceGroup<Device> &group) {
		if (set_ != Id{}) group.free(device, set_);
		set_ = Id{};
		shader_ = Id{};
		index_ = 0;
		key_.clear();
	}

	const Id &id() const { return set_; }

private:
	Id set_{};
	Id shader_{};
	uint32_t index_ = 0;
	std::vector<Uniform<Id>> key_;
};

// A set whose inputs never change identity: built once, released with the group.
template <class Device>
typename Device::Id uniform_set(Device &device, ResourceGroup<Device> &group,
		const typename Device::Id &shader, uint32_t set,
		const std::vector<Uniform<typename Device::Id>> &uniforms) {
	return group.add(Kind::UniformSet, device.create_uniform_set(shader, set, uniforms));
}

} // namespace ve::gpu
```

- [ ] **Step 4: Run the tests**

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: all pass, with 11 more test cases than Task 1's native line (plus Task 5's two).

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/gpu/gpu_core.h extension/tests/test_gpu_core.cpp
git commit -m "feat: pure GPU lifetime core (ResourceGroup, UniformSetCache) with fake-device tests"
```

---

### Task 8: The RenderingDevice adapter and pass helper — `gpu.{h,cpp}`

No pass uses it yet; this task only has to build and pass its native test.

**Files:**
- Create: `extension/src/render/gpu/gpu.h`, `extension/src/render/gpu/gpu.cpp`
- Modify: `extension/src/render/shader_loader.h`, `extension/src/render/shader_loader.cpp`
- Modify: `extension/SConstruct`
- Test: `extension/tests/test_shader_loader.cpp`

**Interfaces:**
- Consumes: Task 7's `ve::gpu` core; `ve::load_shader_source`, `ve::strip_shader_annotations`.
- Produces (namespace `godot::gpu`, all used by Tasks 9–31):
  - `struct RdDevice { using Id = RID; RenderingDevice *rd; bool alive(Kind, const RID &); void free(const RID &); RID create_uniform_set(const RID &, uint32_t, const std::vector<Uniform> &); };`
  - `using Uniform = ve::gpu::Uniform<RID>; using Group = ve::gpu::ResourceGroup<RdDevice>; using SetCache = ve::gpu::UniformSetCache<RdDevice>;` and `using ve::gpu::{Kind, sampled, image, ubo, storage};`
  - `RID uniform_set(RenderingDevice *, Group &, const RID &shader, uint32_t set, const std::vector<Uniform> &);`
  - `struct Program { RID shader, pipeline; bool valid() const; };`
  - `Program compile_compute(RenderingDevice *, Group &, const char *label, const char *file, const char *defines = "");`
  - `RID compile_raster(RenderingDevice *, Group &, const char *label, const char *vertex_file, const char *fragment_file, const char *defines = "");`
  - `bool compile_check(RenderingDevice *, const String &res_path, RenderingDevice::ShaderStage, String *out_error);`
  - `RID sampler(RenderingDevice *, Group &, RenderingDevice::SamplerFilter, bool clamp_to_edge = false);`
  - `RID texture(RenderingDevice *, Group &, RenderingDevice::DataFormat, Vector2i size, uint32_t usage, const TypedArray<PackedByteArray> &data = {});`
  - `class Target { bool ensure(RenderingDevice *, Group &, RenderingDevice::DataFormat, Vector2i, uint32_t usage, const Color *clear = nullptr); RID rid() const; Vector2i size() const; };`
  - `class FramebufferCache { RID get(RenderingDevice *, Group &, const std::vector<RID> &attachments); void release(RenderingDevice *, Group &); RID rid() const; int64_t format() const; };`
  - `struct RasterState { PolygonCullMode cull; PolygonFrontFace front; bool depth_test, depth_write; CompareOperator compare; int color_attachments; bool logic_or; };`
  - `RID raster_pipeline(RenderingDevice *, Group &, const RID &shader, int64_t fb_format, const RasterState &);`
  - `template <class T> PackedByteArray push_bytes(const T &);`
  - `struct SetSlot { RID set; uint32_t index; };` `bool dispatch(RenderingDevice *, const RID &pipeline, std::initializer_list<SetSlot>, const PackedByteArray &push, uint32_t x, uint32_t y, uint32_t z = 1);`
  - `inline uint32_t groups(int n, int local);` `class CpuTimer { explicit CpuTimer(float &out_ms); };`
  - Pure: `std::string ve::insert_after_version(const std::string &src, const std::string &text);`

- [ ] **Step 1: Failing test for define insertion**

Append to `extension/tests/test_shader_loader.cpp`:

```cpp
TEST_CASE("insert_after_version places text on the line after #version") {
	CHECK(ve::insert_after_version("#version 450\nvoid main() {}\n", "#define A 1\n") ==
			"#version 450\n#define A 1\nvoid main() {}\n");
	CHECK(ve::insert_after_version("// lead\n#version 450\nx\n", "#define A 1\n") ==
			"// lead\n#version 450\n#define A 1\nx\n");
	CHECK(ve::insert_after_version("no version line\n", "#define A 1\n") == "no version line\n");
	CHECK(ve::insert_after_version("#version 450", "#define A 1\n") == "#version 450");
}
```

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: compile error, `insert_after_version` is not a member of `ve`.

- [ ] **Step 2: Implement it**

`extension/src/render/shader_loader.h`, before `} // namespace ve`:

```cpp
// Returns `src` with `text` inserted at the start of the line after the first `#version`
// line; unchanged when there is no such line or it has no newline. The one way a pass
// compiles a variant of a shared source (SSR's apply stage, the seam-marker rasters).
std::string insert_after_version(const std::string &src, const std::string &text);
```

`extension/src/render/shader_loader.cpp`, before the closing `} // namespace ve`:

```cpp
std::string insert_after_version(const std::string &src, const std::string &text) {
	const size_t version = src.find("#version");
	if (version == std::string::npos) return src;
	const size_t eol = src.find('\n', version);
	if (eol == std::string::npos) return src;
	std::string out = src;
	out.insert(eol + 1, text);
	return out;
}
```

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: all pass.

- [ ] **Step 3: Let the library build `render/gpu/`**

`extension/SConstruct` line 9:

```python
sources = Glob("src/*.cpp") + Glob("src/*/*.cpp") + Glob("src/*/*/*.cpp")
```

- [ ] **Step 4: Write the helper header**

`extension/src/render/gpu/gpu.h`:

```cpp
#pragma once
// The pass helper (docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md
// §3.1). Lifetime rules live in the godot-free gpu_core.h; this file binds them to
// RenderingDevice and holds the steps every pass used to copy: compile, samplers, textures,
// sized targets, framebuffers, raster pipelines, push bytes and dispatch.
#include "render/gpu/gpu_core.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <vector>

namespace godot::gpu {

using ve::gpu::image;
using ve::gpu::Kind;
using ve::gpu::sampled;
using ve::gpu::storage;
using ve::gpu::ubo;

// ve::gpu's Device concept over a RenderingDevice. A pointer, not an owner: build one on the
// stack where a call needs it.
struct RdDevice {
	using Id = RID;
	RenderingDevice *rd = nullptr;
	bool alive(Kind kind, const RID &id);
	void free(const RID &id);
	RID create_uniform_set(const RID &shader, uint32_t set,
			const std::vector<ve::gpu::Uniform<RID>> &uniforms);
};

using Uniform = ve::gpu::Uniform<RID>;
using Group = ve::gpu::ResourceGroup<RdDevice>;
using SetCache = ve::gpu::UniformSetCache<RdDevice>;

RID uniform_set(RenderingDevice *rd, Group &group, const RID &shader, uint32_t set,
		const std::vector<Uniform> &uniforms);

// ---- compile -------------------------------------------------------------------------------

struct Program {
	RID shader;
	RID pipeline; // compute only: a raster pipeline depends on a framebuffer format
	bool valid() const { return shader.is_valid() && pipeline.is_valid(); }
};

// res://shaders/<file>, expanded, annotation-stripped, `defines` inserted after #version,
// compiled; shader and pipeline registered in `group`. Prints "<label>: <file>: <why>" and
// returns an invalid Program on failure.
Program compile_compute(RenderingDevice *rd, Group &group, const char *label, const char *file,
		const char *defines = "");

// Vertex + fragment, `defines` in both stages. Returns the shader (registered), or RID().
RID compile_raster(RenderingDevice *rd, Group &group, const char *label, const char *vertex_file,
		const char *fragment_file, const char *defines = "");

// Compiles one stage of `res_path` and creates nothing: the shader-reload pre-flight. On
// failure sets *out_error to "<res_path>: <why>".
bool compile_check(RenderingDevice *rd, const String &res_path, RenderingDevice::ShaderStage stage,
		String *out_error);

// ---- resources -----------------------------------------------------------------------------

RID sampler(RenderingDevice *rd, Group &group, RenderingDevice::SamplerFilter filter,
		bool clamp_to_edge = false);

// A 2D texture that never resizes, registered in `group`.
RID texture(RenderingDevice *rd, Group &group, RenderingDevice::DataFormat format, Vector2i size,
		uint32_t usage, const TypedArray<PackedByteArray> &data = TypedArray<PackedByteArray>());

// A 2D texture recreated whenever the requested size changes. The old one is freed through the
// group, so the uniform sets that bound it die with it and their caches rebuild.
class Target {
public:
	// `clear`, when given, is applied once to each newly created texture.
	bool ensure(RenderingDevice *rd, Group &group, RenderingDevice::DataFormat format, Vector2i size,
			uint32_t usage, const Color *clear = nullptr);
	RID rid() const { return rid_; }
	Vector2i size() const { return size_; }

private:
	RID rid_;
	Vector2i size_{0, 0};
};

// A framebuffer over a list of attachments, reused while the attachment RIDs match and the
// device still has it (a framebuffer dies with any attachment).
class FramebufferCache {
public:
	RID get(RenderingDevice *rd, Group &group, const std::vector<RID> &attachments);
	void release(RenderingDevice *rd, Group &group);
	RID rid() const { return rid_; }
	int64_t format() const { return format_; }

private:
	RID rid_;
	std::vector<RID> attachments_;
	int64_t format_ = 0;
};

// The raster state this engine's passes vary. Defaults are RenderingDevice's own for cull and
// front face, and this engine's reverse-Z depth test (near = 1, far = 0).
struct RasterState {
	RenderingDevice::PolygonCullMode cull = RenderingDevice::POLYGON_CULL_DISABLED;
	RenderingDevice::PolygonFrontFace front = RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE;
	bool depth_test = true;
	bool depth_write = true;
	RenderingDevice::CompareOperator compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	int color_attachments = 0; // each opaque (blend disabled)
	bool logic_or = false;     // colour logic op OR (the seam-marker probe)
};

// Pull-only triangles: no vertex format.
RID raster_pipeline(RenderingDevice *rd, Group &group, const RID &shader, int64_t fb_format,
		const RasterState &state);

// ---- dispatch ------------------------------------------------------------------------------

template <class T>
PackedByteArray push_bytes(const T &block) {
	static_assert(std::is_trivially_copyable_v<T>, "a push block is copied as bytes");
	PackedByteArray bytes;
	bytes.resize(sizeof(T));
	std::memcpy(bytes.ptrw(), &block, sizeof(T));
	return bytes;
}

struct SetSlot {
	RID set;
	uint32_t index;
};

// One compute list: bind, push (skipped when empty), dispatch, end. False when the list could
// not open.
bool dispatch(RenderingDevice *rd, const RID &pipeline, std::initializer_list<SetSlot> sets,
		const PackedByteArray &push, uint32_t x, uint32_t y, uint32_t z = 1);

inline uint32_t groups(int n, int local) {
	return static_cast<uint32_t>((n + local - 1) / local);
}

// Writes command-record time (not GPU time) to `out_ms` when it leaves scope.
class CpuTimer {
public:
	explicit CpuTimer(float &out_ms) : out_(out_ms), t0_(std::chrono::steady_clock::now()) {}
	~CpuTimer() {
		out_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0_).count();
	}

private:
	float &out_;
	std::chrono::steady_clock::time_point t0_;
};

} // namespace godot::gpu
```

- [ ] **Step 5: Write the helper implementation**

`extension/src/render/gpu/gpu.cpp`:

```cpp
#include "render/gpu/gpu.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <string>
#include <utility>

namespace godot::gpu {

static_assert(static_cast<int>(ve::gpu::UniformType::SamplerWithTexture) ==
		RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
static_assert(static_cast<int>(ve::gpu::UniformType::Image) == RenderingDevice::UNIFORM_TYPE_IMAGE);
static_assert(static_cast<int>(ve::gpu::UniformType::UniformBuffer) ==
		RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
static_assert(static_cast<int>(ve::gpu::UniformType::StorageBuffer) ==
		RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);

bool RdDevice::alive(Kind kind, const RID &id) {
	if (!rd || !id.is_valid()) return false;
	switch (kind) {
		case Kind::UniformSet: return rd->uniform_set_is_valid(id);
		case Kind::Framebuffer: return rd->framebuffer_is_valid(id);
		case Kind::Texture: return rd->texture_is_valid(id);
		case Kind::Pipeline: return rd->compute_pipeline_is_valid(id) || rd->render_pipeline_is_valid(id);
		// No query exists for these, and nothing in this engine frees them by cascade.
		case Kind::IndexArray:
		case Kind::Shader:
		case Kind::Sampler:
		case Kind::Buffer: return true;
	}
	return true;
}

void RdDevice::free(const RID &id) {
	rd->free_rid(id);
}

RID RdDevice::create_uniform_set(const RID &shader, uint32_t set,
		const std::vector<ve::gpu::Uniform<RID>> &uniforms) {
	Array array;
	for (const ve::gpu::Uniform<RID> &u : uniforms) {
		Ref<RDUniform> r;
		r.instantiate();
		r->set_uniform_type(static_cast<RenderingDevice::UniformType>(u.type));
		r->set_binding(static_cast<int32_t>(u.binding));
		for (uint8_t i = 0; i < u.count; i++) r->add_id(u.ids[i]);
		array.push_back(r);
	}
	return rd->uniform_set_create(array, shader, set);
}

RID uniform_set(RenderingDevice *rd, Group &group, const RID &shader, uint32_t set,
		const std::vector<Uniform> &uniforms) {
	RdDevice device{rd};
	return ve::gpu::uniform_set(device, group, shader, set, uniforms);
}

namespace {

// "" with *error set on failure.
std::string load_stage(const String &res_path, const char *defines, std::string *error) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(res_path);
	const String inc = ps->globalize_path("res://shaders");
	std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), error));
	if (!code.empty() && defines && *defines) code = ve::insert_after_version(code, defines);
	return code;
}

using StageSource = std::pair<RenderingDevice::ShaderStage, const std::string *>;

// Every stage's compile error concatenated into *error ("" on success).
Ref<RDShaderSPIRV> compile_stages(RenderingDevice *rd, std::initializer_list<StageSource> stages,
		String *error) {
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	for (const StageSource &s : stages) src->set_stage_source(s.first, String(s.second->c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	*error = String();
	for (const StageSource &s : stages) *error += spirv->get_stage_compile_error(s.first);
	return spirv;
}

} // namespace

Program compile_compute(RenderingDevice *rd, Group &group, const char *label, const char *file,
		const char *defines) {
	Program p;
	if (!rd) return p;
	std::string err;
	const std::string code = load_stage(String("res://shaders/") + file, defines, &err);
	if (code.empty()) {
		UtilityFunctions::printerr(label, ": ", file, " load failed: ", err.c_str());
		return p;
	}
	String compile_err;
	const Ref<RDShaderSPIRV> spirv =
			compile_stages(rd, {{RenderingDevice::SHADER_STAGE_COMPUTE, &code}}, &compile_err);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr(label, ": ", file, ": ", compile_err);
		return p;
	}
	p.shader = group.add(Kind::Shader, rd->shader_create_from_spirv(spirv));
	if (p.shader.is_valid()) p.pipeline = group.add(Kind::Pipeline, rd->compute_pipeline_create(p.shader));
	if (!p.valid()) UtilityFunctions::printerr(label, ": ", file, ": pipeline creation failed");
	return p;
}

RID compile_raster(RenderingDevice *rd, Group &group, const char *label, const char *vertex_file,
		const char *fragment_file, const char *defines) {
	if (!rd) return RID();
	std::string err;
	const std::string vertex = load_stage(String("res://shaders/") + vertex_file, defines, &err);
	if (vertex.empty()) {
		UtilityFunctions::printerr(label, ": ", vertex_file, " load failed: ", err.c_str());
		return RID();
	}
	const std::string fragment = load_stage(String("res://shaders/") + fragment_file, defines, &err);
	if (fragment.empty()) {
		UtilityFunctions::printerr(label, ": ", fragment_file, " load failed: ", err.c_str());
		return RID();
	}
	String compile_err;
	const Ref<RDShaderSPIRV> spirv = compile_stages(rd,
			{{RenderingDevice::SHADER_STAGE_VERTEX, &vertex},
					{RenderingDevice::SHADER_STAGE_FRAGMENT, &fragment}},
			&compile_err);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr(label, ": ", compile_err);
		return RID();
	}
	const RID shader = group.add(Kind::Shader, rd->shader_create_from_spirv(spirv));
	if (!shader.is_valid()) UtilityFunctions::printerr(label, ": shader creation failed");
	return shader;
}

bool compile_check(RenderingDevice *rd, const String &res_path, RenderingDevice::ShaderStage stage,
		String *out_error) {
	std::string err;
	const std::string code = load_stage(res_path, "", &err);
	if (code.empty()) {
		if (out_error) *out_error = res_path + String(": ") + String(err.c_str());
		return false;
	}
	String compile_err;
	compile_stages(rd, {{stage, &code}}, &compile_err);
	if (!compile_err.is_empty()) {
		if (out_error) *out_error = res_path + String(": ") + compile_err;
		return false;
	}
	return true;
}

RID sampler(RenderingDevice *rd, Group &group, RenderingDevice::SamplerFilter filter,
		bool clamp_to_edge) {
	Ref<RDSamplerState> s;
	s.instantiate();
	s->set_min_filter(filter);
	s->set_mag_filter(filter);
	if (clamp_to_edge) {
		s->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
		s->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
		s->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	}
	return group.add(Kind::Sampler, rd->sampler_create(s));
}

RID texture(RenderingDevice *rd, Group &group, RenderingDevice::DataFormat format, Vector2i size,
		uint32_t usage, const TypedArray<PackedByteArray> &data) {
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_format(format);
	f->set_width(size.x);
	f->set_height(size.y);
	f->set_usage_bits(usage);
	Ref<RDTextureView> v;
	v.instantiate();
	return group.add(Kind::Texture, rd->texture_create(f, v, data));
}

bool Target::ensure(RenderingDevice *rd, Group &group, RenderingDevice::DataFormat format,
		Vector2i size, uint32_t usage, const Color *clear) {
	if (!rd || size.x <= 0 || size.y <= 0) return false;
	if (rid_.is_valid() && size == size_) return true;
	if (rid_.is_valid()) {
		RdDevice device{rd};
		group.free(device, rid_);
	}
	size_ = Vector2i(0, 0);
	rid_ = texture(rd, group, format, size, usage);
	if (!rid_.is_valid()) return false;
	if (clear) rd->texture_clear(rid_, *clear, 0, 1, 0, 1);
	size_ = size;
	return true;
}

RID FramebufferCache::get(RenderingDevice *rd, Group &group, const std::vector<RID> &attachments) {
	if (rid_.is_valid() && attachments == attachments_ && rd->framebuffer_is_valid(rid_)) return rid_;
	release(rd, group);
	Array array;
	for (const RID &a : attachments) array.push_back(a);
	const RID fb = group.add(Kind::Framebuffer, rd->framebuffer_create(array));
	if (!rd->framebuffer_is_valid(fb)) return RID();
	rid_ = fb;
	attachments_ = attachments;
	format_ = rd->framebuffer_get_format(fb);
	return rid_;
}

void FramebufferCache::release(RenderingDevice *rd, Group &group) {
	if (rid_.is_valid()) {
		RdDevice device{rd};
		group.free(device, rid_);
	}
	rid_ = RID();
	attachments_.clear();
}

RID raster_pipeline(RenderingDevice *rd, Group &group, const RID &shader, int64_t fb_format,
		const RasterState &state) {
	Ref<RDPipelineRasterizationState> rs;
	rs.instantiate();
	rs->set_cull_mode(state.cull);
	rs->set_front_face(state.front);
	Ref<RDPipelineMultisampleState> ms;
	ms.instantiate();
	Ref<RDPipelineDepthStencilState> ds;
	ds.instantiate();
	ds->set_enable_depth_test(state.depth_test);
	ds->set_enable_depth_write(state.depth_write);
	ds->set_depth_compare_operator(state.compare);
	Array attachments;
	for (int i = 0; i < state.color_attachments; i++) {
		Ref<RDPipelineColorBlendStateAttachment> a;
		a.instantiate();
		a->set_enable_blend(false);
		attachments.push_back(a);
	}
	Ref<RDPipelineColorBlendState> cb;
	cb.instantiate();
	cb->set_attachments(attachments);
	if (state.logic_or) {
		cb->set_enable_logic_op(true);
		cb->set_logic_op(RenderingDevice::LOGIC_OP_OR);
	}
	return group.add(Kind::Pipeline, rd->render_pipeline_create(shader, fb_format,
			RenderingDevice::INVALID_ID, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb));
}

bool dispatch(RenderingDevice *rd, const RID &pipeline, std::initializer_list<SetSlot> sets,
		const PackedByteArray &push, uint32_t x, uint32_t y, uint32_t z) {
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	for (const SetSlot &s : sets) rd->compute_list_bind_uniform_set(list, s.set, s.index);
	if (!push.is_empty()) rd->compute_list_set_push_constant(list, push, push.size());
	rd->compute_list_dispatch(list, x, y, z);
	rd->compute_list_end();
	return true;
}

} // namespace godot::gpu
```

- [ ] **Step 6: Build**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -3
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: `Build OK`; native all pass. A compile error in `gpu.cpp` is fixed here (godot-cpp signature mismatches are expected to surface now, not in a pass task).

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/gpu/gpu.h extension/src/render/gpu/gpu.cpp \
	extension/src/render/shader_loader.h extension/src/render/shader_loader.cpp \
	extension/tests/test_shader_loader.cpp extension/SConstruct
git commit -m "feat: RenderingDevice adapter and pass helper over the lifetime core"
```

---

### Migration tasks 9–31: what every one of them does

Each migration task rewrites one site's lifetime code onto `render/gpu/gpu.h` and nothing else:

- Push-constant packing (`PackedByteArray` plus hand-offset writes) is kept byte for byte; Tasks 33–36 replace it.
- Every uniform keeps its binding, type, sampler and resource. The migrated list is written in binding order.
- Public methods, their signatures and what they return are unchanged.
- The pass's includes lose the `rd_*`/`project_settings`/`shader_loader` headers it no longer uses and gain `#include "render/gpu/gpu.h"` (in the header, since members use its types).
- Step "no leftovers" greps the file for the members the task deleted. Step "Pass gate" runs the Procedure for the site. The commit message is `refactor(<site>): lifetime through the pass helper` with any rows-with-differences noted.

---

### Task 9: Migrate `SsaoPass`

**Files:**
- Modify: `extension/src/render/ssao_pass.h`, `extension/src/render/ssao_pass.cpp`

**Interfaces:**
- Consumes: Task 8's `gpu::Group`, `gpu::Program`, `gpu::compile_compute`, `gpu::sampler`, `gpu::Target`, `gpu::SetCache`, `gpu::RdDevice`, `gpu::dispatch`, `gpu::groups`, `gpu::CpuTimer`.
- Produces: unchanged public API (`initialize`, `teardown`, `render`, `result`, `clear_result`, `last_ms`, `size`).

- [ ] **Step 1: Header**

In `extension/src/render/ssao_pass.h`, add `#include "render/gpu/gpu.h"` after the godot includes, change `Vector2i size() const { return size_; }` to `Vector2i size() const { return target_.size(); }`, and replace everything from `private:` to the closing `};` of the class with:

```cpp
private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_;
	gpu::Target target_;
	gpu::SetCache set_;
	RID output_;
	float last_ms_ = 0.0f;
};
```

- [ ] **Step 2: Implementation**

Replace `extension/src/render/ssao_pass.cpp` with:

```cpp
#include "render/ssao_pass.h"
#include "render/gbuffer.h"
#include <algorithm>

using namespace godot;

// Must match the Push block in ssao.comp.glsl.
static const float kSsaoRadius = 5.0f;
static const float kSsaoStrength = 1.5f;

SsaoPass::~SsaoPass() {
	teardown();
}

void SsaoPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "SsaoPass", "ssao.comp.glsl");
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!program_.valid() || !sampler_nearest_.is_valid()) teardown();
}

void SsaoPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_nearest_ = RID();
	target_ = gpu::Target();
	set_ = gpu::SetCache();
	output_ = RID();
	rd_ = nullptr;
}

bool SsaoPass::render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
		const ve::BeautySettings &s) {
	output_ = RID();
	if (!s.ssao) return false;
	if (!rd_ || rd != rd_ || !program_.valid() || !gb.is_valid() || !camera_ubo.is_valid())
		return false;
	// Half-res, matching the SSGI and SSR chains. AO modulates only the ambient term and
	// is upsampled bilinearly by the deferred pass, so the quarter-cost target costs the
	// image far less than it costs the frame. See ssao.comp.glsl.
	const Vector2i half(std::max(1, gb.size().x / 2), std::max(1, gb.size().y / 2));
	// Defined before the first read even if the pass is skipped this frame.
	const Color unoccluded(1.0f, 1.0f, 1.0f, 1.0f);
	if (!target_.ensure(rd, group_, RenderingDevice::DATA_FORMAT_R8_UNORM, half,
				RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
						RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT,
				&unoccluded))
		return false;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, sampler_nearest_, gb.surface()),
			gpu::sampled(1, sampler_nearest_, gb.depth()),
			gpu::image(2, target_.rid()),
			gpu::ubo(5, camera_ubo)});
	if (!set.is_valid()) return false;

	gpu::CpuTimer timer(last_ms_);
	static_assert(sizeof(float) * 8 == 32, "ssao push block");
	PackedByteArray pc;
	pc.resize(32);
	int32_t *dims = reinterpret_cast<int32_t *>(pc.ptrw());
	dims[0] = half.x;
	dims[1] = half.y;
	dims[2] = s.ssao_steps;
	dims[3] = s.ssao_directions;
	float *f = reinterpret_cast<float *>(pc.ptrw());
	f[4] = kSsaoRadius;
	f[5] = kSsaoStrength;
	f[6] = f[7] = 0.0f;
	if (!gpu::dispatch(rd, program_.pipeline, {{set, 0}}, pc, gpu::groups(half.x, 8),
				gpu::groups(half.y, 8)))
		return false;
	output_ = target_.rid();
	return true;
}
```

- [ ] **Step 3: No leftovers**

```bash
rg -n "key_|uset_|shader_compile|ensure_target|ensure_uniform_set|free_rid" extension/src/render/ssao_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **ssao**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/ssao_pass.h extension/src/render/ssao_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(ssao): lifetime through the pass helper"
```

---

### Task 10: Migrate `ContactShadowPass`

`teardown()` keeps the `sun_light_ubo_` mirror (not owned; see Global Constraints).

**Files:**
- Modify: `extension/src/render/contact_shadow_pass.h`, `extension/src/render/contact_shadow_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged public API (`set_sun_ubo`, `initialize`, `teardown`, `render`, `mask`, `last_ms`).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; change `RID mask() const { return mask_; }` to `RID mask() const { return mask_.rid(); }`; replace the private section with:

```cpp
private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_, sampler_linear_;
	gpu::Target mask_;
	gpu::SetCache set_;
	RID sun_light_ubo_; // NOT owned: RenderOrchestrator frees it
	float last_ms_ = 0.0f;
};
```

- [ ] **Step 2: Implementation**

Replace `extension/src/render/contact_shadow_pass.cpp` with:

```cpp
#include "render/contact_shadow_pass.h"
#include <algorithm>

using namespace godot;

ContactShadowPass::~ContactShadowPass() {
	teardown();
}

void ContactShadowPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "ContactShadowPass", "contact_shadow.comp.glsl");
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	if (!program_.valid() || !sampler_nearest_.is_valid() || !sampler_linear_.is_valid()) teardown();
}

void ContactShadowPass::set_sun_ubo(RID buffer) {
	// The uniform set keys on this RID, so the next render rebuilds it.
	sun_light_ubo_ = buffer;
}

void ContactShadowPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_nearest_ = sampler_linear_ = RID();
	mask_ = gpu::Target();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

bool ContactShadowPass::render(RenderingDevice *rd, RID scene_color, RID scene_depth,
		Vector2i size, RID camera_ubo, const ve::BeautySettings &s) {
	if (!s.contact_shadows || s.contact_steps <= 0) return false;
	if (!rd_ || rd != rd_ || !program_.valid() || !scene_color.is_valid() ||
			!scene_depth.is_valid() || !camera_ubo.is_valid() || size.x <= 0 || size.y <= 0)
		return false;
	const Vector2i half(std::max(1, size.x / 2), std::max(1, size.y / 2));
	if (!mask_.ensure(rd, group_, RenderingDevice::DATA_FORMAT_R8_UNORM, half,
				RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
						RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT))
		return false;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, sampler_nearest_, scene_depth),
			gpu::image(1, mask_.rid()),
			gpu::sampled(2, sampler_linear_, mask_.rid()),
			gpu::image(3, scene_color),
			gpu::ubo(4, camera_ubo),
			gpu::ubo(5, sun_light_ubo_)});
	if (!set.is_valid()) return false;

	// This is command-record time, not GPU execution time (M5 errata 15).
	gpu::CpuTimer timer(last_ms_);
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	PackedByteArray pc;
	pc.resize(32);
	int32_t *dims = reinterpret_cast<int32_t *>(pc.ptrw());
	dims[0] = half.x;
	dims[1] = half.y;
	dims[2] = 0;
	dims[3] = s.contact_steps;
	float *params = reinterpret_cast<float *>(pc.ptrw()) + 4;
	params[0] = 0.6f;
	params[1] = 0.85f;
	// One voxel: large enough to leave the receiver, but too small to bridge terrain gaps.
	params[2] = 0.05f;
	params[3] = 0.0f;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_add_barrier(list);
	dims[0] = size.x;
	dims[1] = size.y;
	dims[2] = 1;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_end();
	return true;
}
```

The mask is now keyed on its half size rather than the full size; two full sizes with the same half (e.g. 101 and 100 wide) reuse one mask, which the shader never distinguishes (`dims.xy` is the half size either way).

- [ ] **Step 3: No leftovers**

```bash
rg -n "key_|uset_|shader_compile|ensure_mask|ensure_uniform_set|free_rid" extension/src/render/contact_shadow_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **contact_shadow**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/contact_shadow_pass.h extension/src/render/contact_shadow_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(contact_shadow): lifetime through the pass helper"
```

---

### Task 11: Migrate `OutlinePass`

**Files:**
- Modify: `extension/src/render/outline_pass.h`, `extension/src/render/outline_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged public API.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; replace the private section with:

```cpp
private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID nearest_, dummy_normal_;
	gpu::SetCache set_;
	float last_ms_ = 0.0f;
};
```

- [ ] **Step 2: Implementation**

Replace `extension/src/render/outline_pass.cpp` with:

```cpp
#include "render/outline_pass.h"

using namespace godot;

OutlinePass::~OutlinePass() {
	teardown();
}

bool OutlinePass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "OutlinePass", "outline.comp.glsl");
	nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	dummy_normal_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			Vector2i(1, 1),
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	if (dummy_normal_.is_valid())
		rd->texture_clear(dummy_normal_, Color(0.5f, 0.5f, 1.0f, 1.0f), 0, 1, 0, 1);
	if (!program_.valid() || !nearest_.is_valid() || !dummy_normal_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void OutlinePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	nearest_ = dummy_normal_ = RID();
	set_ = gpu::SetCache();
	last_ms_ = 0.0f;
	rd_ = nullptr;
}

bool OutlinePass::render(RenderingDevice *rd, RID scene_color, RID scene_depth, RID gb_depth,
		RID gb_surface, RID normal_roughness, bool have_normal_roughness, RID camera_ubo,
		Vector2i size, const ve::BeautySettings &s) {
	if (!s.outlines) return false;
	if (!rd_ || rd != rd_ || !program_.valid() || size.x <= 0 || size.y <= 0 ||
			!scene_color.is_valid() || !scene_depth.is_valid() || !gb_depth.is_valid() ||
			!gb_surface.is_valid() || !camera_ubo.is_valid()) return false;
	const RID normal = normal_roughness.is_valid() ? normal_roughness : dummy_normal_;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, nearest_, scene_depth),
			gpu::sampled(1, nearest_, gb_depth),
			gpu::sampled(2, nearest_, gb_surface),
			gpu::sampled(3, nearest_, normal),
			gpu::image(4, scene_color),
			gpu::ubo(6, camera_ubo)});
	if (!set.is_valid()) return false;
	gpu::CpuTimer timer(last_ms_);
	PackedByteArray pc;
	pc.resize(32);
	int32_t *i = reinterpret_cast<int32_t *>(pc.ptrw());
	float *f = reinterpret_cast<float *>(pc.ptrw());
	i[0] = size.x; i[1] = size.y;
	i[2] = have_normal_roughness && normal_roughness.is_valid() ? 1 : 0; i[3] = 0;
	f[4] = s.outline_depth_threshold; f[5] = s.outline_normal_threshold;
	f[6] = 0.35f; f[7] = 0.0f;
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, pc, gpu::groups(size.x, 8),
			gpu::groups(size.y, 8));
}
```

- [ ] **Step 3: No leftovers**

```bash
rg -n "key_|uset_|shader_compile|ensure_uniform_set|make_texture|sampler_texture|image_texture|uniform_buffer\(|free_rid" extension/src/render/outline_pass.{h,cpp}
```
Expected: no output. (Remove `ensure_uniform_set` from the header if Step 1 left it.)

- [ ] **Step 4: Pass gate** for **outline**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/outline_pass.h extension/src/render/outline_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(outline): lifetime through the pass helper"
```

---

### Task 12: Migrate `SsrPass`

The apply stage was compiled by inserting `#define SSR_APPLY 1` after the first line of the stripped source; `compile_compute`'s `defines` inserts after the `#version` line. Step 1 confirms those are the same line.

**Files:**
- Modify: `extension/src/render/ssr_pass.h`, `extension/src/render/ssr_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged public API (`reflection()`, `half_size()`, `last_ms()` …).

- [ ] **Step 1: Confirm `#version` is the first line after stripping**

```bash
head -3 shaders/ssr.comp.glsl
```
Expected: line 1 is `#[compute]` and line 2 is `#version …`. `strip_shader_annotations` removes line 1, so `#version` is first. If line 2 is not `#version`, stop and report.

- [ ] **Step 2: Header**

Add `#include "render/gpu/gpu.h"`; change `RID reflection() const { return reflection_; }` → `return reflection_.rid();` and `Vector2i half_size() const { return half_size_; }` → `return reflection_.size();`; replace the private section with:

```cpp
private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program trace_, apply_;
	RID nearest_, linear_, dummy_normal_;
	gpu::Target reflection_;
	gpu::SetCache trace_set_, apply_set_;
	float last_ms_ = 0.0f;
};
```

- [ ] **Step 3: Implementation**

Replace `extension/src/render/ssr_pass.cpp` with:

```cpp
#include "render/ssr_pass.h"
#include <algorithm>

using namespace godot;

SsrPass::~SsrPass() {
	teardown();
}

bool SsrPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	trace_ = gpu::compile_compute(rd, group_, "SsrPass trace", "ssr.comp.glsl");
	apply_ = gpu::compile_compute(rd, group_, "SsrPass apply", "ssr.comp.glsl", "#define SSR_APPLY 1\n");
	nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	dummy_normal_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			Vector2i(1, 1),
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	if (dummy_normal_.is_valid())
		rd->texture_clear(dummy_normal_, Color(0.0f, 0.0f, 0.0f, 1.0f), 0, 1, 0, 1);
	if (!trace_.valid() || !apply_.valid() || !nearest_.is_valid() || !linear_.is_valid() ||
			!dummy_normal_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void SsrPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	trace_ = apply_ = gpu::Program();
	nearest_ = linear_ = dummy_normal_ = RID();
	reflection_ = gpu::Target();
	trace_set_ = apply_set_ = gpu::SetCache();
	last_ms_ = 0.0f;
	rd_ = nullptr;
}

bool SsrPass::render(RenderingDevice *rd, RID scene_color, RID scene_depth, RID gb_surface,
		RID gb_depth, RID normal_roughness, bool have_normal_roughness, RID camera_ubo,
		Vector2i size, const ve::BeautySettings &s) {
	if (!s.ssr || s.ssr_steps <= 0) return false;
	if (!rd_ || rd != rd_ || !trace_.valid() || !apply_.valid() || !scene_color.is_valid() ||
			!scene_depth.is_valid() || !gb_surface.is_valid() || !gb_depth.is_valid() ||
			!camera_ubo.is_valid()) return false;
	if (size.x <= 0 || size.y <= 0) return false;
	const Vector2i half(std::max(1, size.x / 2), std::max(1, size.y / 2));
	if (!reflection_.ensure(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, half,
				RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
						RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
						RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT))
		return false;
	const RID normal = normal_roughness.is_valid() ? normal_roughness : dummy_normal_;
	gpu::RdDevice device{rd};
	const RID trace_set = trace_set_.get(device, group_, trace_.shader, 0, {
			gpu::sampled(0, linear_, scene_color),
			gpu::sampled(1, nearest_, scene_depth),
			gpu::sampled(2, nearest_, gb_surface),
			gpu::sampled(3, nearest_, gb_depth),
			gpu::sampled(4, nearest_, normal),
			gpu::image(5, reflection_.rid()),
			gpu::ubo(6, camera_ubo)});
	const RID apply_set = apply_set_.get(device, group_, apply_.shader, 0, {
			gpu::sampled(0, linear_, reflection_.rid()),
			gpu::image(1, scene_color)});
	if (!trace_set.is_valid() || !apply_set.is_valid()) return false;
	gpu::CpuTimer timer(last_ms_);
	PackedByteArray trace_pc;
	trace_pc.resize(32);
	int32_t *i = reinterpret_cast<int32_t *>(trace_pc.ptrw());
	float *f = reinterpret_cast<float *>(trace_pc.ptrw());
	i[0] = half.x; i[1] = half.y; i[2] = s.ssr_steps;
	i[3] = have_normal_roughness && normal_roughness.is_valid() ? 1 : 0;
	f[4] = kReachM; f[5] = kStartBiasM; f[6] = kThicknessM; f[7] = kStrength;
	if (!gpu::dispatch(rd, trace_.pipeline, {{trace_set, 0}}, trace_pc, gpu::groups(half.x, 8),
				gpu::groups(half.y, 8)))
		return false;
	PackedByteArray apply_pc;
	apply_pc.resize(16);
	int32_t *dims = reinterpret_cast<int32_t *>(apply_pc.ptrw());
	dims[0] = size.x; dims[1] = size.y; dims[2] = dims[3] = 0;
	return gpu::dispatch(rd, apply_.pipeline, {{apply_set, 0}}, apply_pc, gpu::groups(size.x, 8),
			gpu::groups(size.y, 8));
}
```

- [ ] **Step 4: No leftovers**

```bash
rg -n "key_|trace_pipeline_|apply_pipeline_|trace_shader_|apply_shader_|shader_compile|ensure_targets|ensure_uniform_sets|make_texture|free_rid" extension/src/render/ssr_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 5: Pass gate** for **ssr**.

- [ ] **Step 6: Commit**

```bash
git add extension/src/render/ssr_pass.h extension/src/render/ssr_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(ssr): lifetime through the pass helper"
```

---

### Task 13: Migrate `SsgiPass`

**Files:**
- Modify: `extension/src/render/ssgi_pass.h`, `extension/src/render/ssgi_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged public API.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; replace the private section with:

```cpp
private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_, sampler_linear_;
	gpu::Target targets_[2];
	gpu::Target raw_; // the gather before its resolve; see resolve() in shaders/ssgi.comp.glsl
	gpu::SetCache set_;
	RID output_;
	float last_ms_ = 0.0f;
};
```

- [ ] **Step 2: Implementation**

Replace `extension/src/render/ssgi_pass.cpp` with:

```cpp
#include "render/ssgi_pass.h"
#include "render/gbuffer.h"
#include <algorithm>
#include <cstring>

using namespace godot;

SsgiPass::~SsgiPass() {
	teardown();
}

void SsgiPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "SsgiPass", "ssgi.comp.glsl");
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	if (!program_.valid() || !sampler_nearest_.is_valid() || !sampler_linear_.is_valid()) teardown();
}

void SsgiPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_nearest_ = sampler_linear_ = RID();
	targets_[0] = targets_[1] = raw_ = gpu::Target();
	set_ = gpu::SetCache();
	output_ = RID();
	rd_ = nullptr;
}

bool SsgiPass::render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo,
		const float prev_view_proj[16], bool have_history, const ve::BeautySettings &s,
		uint32_t frame) {
	output_ = RID();
	if (!s.ssgi || s.ssgi_taps <= 0) return false;
	if (!rd_ || rd != rd_ || !program_.valid() || !gb.is_valid() ||
			!camera_ubo.is_valid() || !prev_view_proj) return false;
	const Vector2i size = gb.size();
	if (size.x <= 0 || size.y <= 0) return false;
	const Vector2i half(std::max(1, size.x / 2), std::max(1, size.y / 2));
	const uint32_t usage = RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	// Defined before the first ping-pong read, including a first frame whose history is absent.
	const Color cleared(0.0f, 0.0f, 0.0f, 0.0f);
	for (gpu::Target *t : {&targets_[0], &targets_[1], &raw_})
		if (!t->ensure(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, half, usage, &cleared))
			return false;
	const uint32_t out_index = frame & 1u;
	const uint32_t prev_index = (frame + 1u) & 1u;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, sampler_nearest_, gb.surface()),
			gpu::sampled(1, sampler_nearest_, gb.depth()),
			gpu::sampled(2, sampler_linear_, gb.history()),
			gpu::sampled(3, sampler_linear_, targets_[prev_index].rid()),
			gpu::image(4, targets_[out_index].rid()),
			gpu::ubo(5, camera_ubo),
			gpu::image(6, raw_.rid()),
			gpu::sampled(7, sampler_nearest_, raw_.rid())});
	if (!set.is_valid()) return false;

	gpu::CpuTimer timer(last_ms_);
	static_assert(sizeof(float) * 32 == 128, "ssgi push block");
	PackedByteArray pc;
	pc.resize(128);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	std::memcpy(f, prev_view_proj, sizeof(float) * 16);
	int32_t *dims = reinterpret_cast<int32_t *>(f + 16);
	dims[0] = half.x;
	dims[1] = half.y;
	dims[2] = s.ssgi_taps;
	dims[3] = have_history ? 1 : 0;
	// These were literals here until the emissive work: 6 m, 0.90, 1.0. They are knobs in
	// ve::BeautySettings now, which is where that struct always said every knob a pass reads
	// has to live -- and which is what lets a tier move the emissive ring.
	f[20] = s.ssgi_radius;
	f[21] = s.ssgi_temporal;
	f[22] = s.ssgi_strength;
	f[23] = 0.0f;
	f[24] = s.emissive_gi_radius;
	f[25] = s.emissive_gi_strength;
	f[26] = 0.0f;
	f[27] = 0.0f;
	int32_t *stage = reinterpret_cast<int32_t *>(f + 28);
	stage[0] = 0;
	stage[1] = stage[2] = stage[3] = 0;
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	// The gather rotates its taps by a bayer4 phase that never changes, so without this second
	// dispatch that phase reaches the screen as a lattice of dots. It averages one full 4x4
	// period back out before the temporal blend, which cannot.
	rd->compute_list_add_barrier(list);
	stage[0] = 1;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (dims[0] + 7) / 8, (dims[1] + 7) / 8, 1);
	rd->compute_list_end();
	output_ = targets_[out_index].rid();
	return true;
}
```

- [ ] **Step 3: No leftovers**

```bash
rg -n "key_|uset_|shader_compile|ensure_targets|ensure_uniform_set|free_rid" extension/src/render/ssgi_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **ssgi**. `test_frame_contract.gd::test_ssgi_history_carries_across_frames_and_falls_on_resize` is the case to watch.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/ssgi_pass.h extension/src/render/ssgi_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(ssgi): lifetime through the pass helper

The set was keyed on gb.albedo() without binding it; the key is now exactly the bound ids."
```

---

### Task 14: Migrate `HizPass`

Mips 1–8 have fixed sets built once; mip 0's source changes and uses a `SetCache`. The shared slice views are not registered: they die with `pyramid_`.

**Files:**
- Modify: `extension/src/render/hiz_pass.h`, `extension/src/render/hiz_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged public API (`release_level0_set` kept for its callers in `orchestrator.cpp` and `hooks_render.cpp`).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`. Delete the private `bool ensure_uniform_set(RenderingDevice *rd, RID src, int dst_mip);`. Replace the member block from `RenderingDevice *rd_ = nullptr;` through `RID uset0_src_;` with:

```cpp
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_;
	RID pyramid_, readback_tex_;
	std::array<RID, kMipCount> slices_{};
	std::array<RID, kMipCount> usets_{}; // [0] mirrors level0_; [1..] are built once
	gpu::SetCache level0_;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/hiz_pass.cpp`:

Replace the includes of `project_settings`, `rd_sampler_state`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `utility_functions` and `render/shader_loader.h` with nothing (keep `rd_texture_format`, `rd_texture_view`, `array`, `<cmath>`, `<cstring>`).

Replace `HizPass::initialize` with:

```cpp
bool HizPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;

	program_ = gpu::compile_compute(rd, group_, "HizPass", "hiz.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!sampler_.is_valid()) {
		teardown();
		return false;
	}

	{
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_format(RenderingDevice::DATA_FORMAT_R32_SFLOAT);
		f->set_width(kSize);
		f->set_height(kSize);
		f->set_mipmaps(kMipCount);
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		pyramid_ = group_.add(gpu::Kind::Texture, rd->texture_create(f, v, {}));
		if (!pyramid_.is_valid()) {
			teardown();
			return false;
		}
	}
	readback_tex_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32_SFLOAT,
			Vector2i(kGrid, kGrid),
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	if (!readback_tex_.is_valid()) {
		teardown();
		return false;
	}

	// Views of pyramid_: freeing pyramid_ frees them, so they are not registered.
	for (int m = 0; m < kMipCount; m++) {
		Ref<RDTextureView> v;
		v.instantiate();
		slices_[m] = rd->texture_create_shared_from_slice(v, pyramid_, 0, m, 1,
				RenderingDevice::TEXTURE_SLICE_2D);
		if (!slices_[m].is_valid()) {
			teardown();
			return false;
		}
	}

	readback_.instantiate();
	if (readback_.is_null()) {
		teardown();
		return false;
	}

	// Mips 1..8 have fixed source/destination slices, so their uniform sets are built once.
	// Mip 0's source is the frame's scene depth and is cached in build().
	for (int m = 1; m < kMipCount; m++) {
		usets_[m] = gpu::uniform_set(rd, group_, program_.shader, 0,
				{gpu::sampled(0, sampler_, slices_[m - 1]), gpu::image(1, slices_[m])});
		if (!usets_[m].is_valid()) {
			teardown();
			return false;
		}
	}
	return true;
}
```

Replace `HizPass::teardown` with:

```cpp
void HizPass::teardown() {
	if (!rd_) return;
	// RenderingDevice retains the Callable for an async readback but not this RefCounted target.
	// Drain before freeing the source texture or releasing readback_, otherwise the deferred
	// callback can validate a freed ObjectDB entry during allocator cleanup.
	readback_was_pending_at_teardown_ = readback_.is_valid() && readback_->pending();
	readback_was_drained_at_teardown_ = !readback_was_pending_at_teardown_ || readback_->drain(rd_);
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_ = pyramid_ = readback_tex_ = RID();
	for (RID &r : slices_) r = RID();
	for (RID &r : usets_) r = RID();
	level0_ = gpu::SetCache();
	readback_ = Ref<AsyncTextureRead>();
	occlusion_ = HizOcclusion();
	rd_ = nullptr;
}
```

Delete `HizPass::ensure_uniform_set`.

In `HizPass::build`, change `if (!rd_ || !pipeline_.is_valid() || !readback_.is_valid()) return false;` to `if (!rd_ || !program_.valid() || !readback_.is_valid()) return false;`, replace `if (!ensure_uniform_set(rd, scene_depth, 0)) return false;` with

```cpp
	gpu::RdDevice device{rd};
	usets_[0] = level0_.get(device, group_, program_.shader, 0,
			{gpu::sampled(0, sampler_, scene_depth), gpu::image(1, slices_[0])});
	if (!usets_[0].is_valid()) return false;
```

and change `rd->compute_list_bind_compute_pipeline(list, pipeline_);` to `rd->compute_list_bind_compute_pipeline(list, program_.pipeline);`.

Replace `HizPass::release_level0_set` with:

```cpp
void HizPass::release_level0_set() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	level0_.drop(device, group_);
	usets_[0] = RID();
}
```

- [ ] **Step 3: No leftovers**

```bash
rg -n "uset0_src_|pipeline_|shader_\b|shader_compile|ensure_uniform_set|free_rid" extension/src/render/hiz_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **hiz**; also run `res://tests/test_render_shutdown.gd`.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/hiz_pass.h extension/src/render/hiz_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(hiz): lifetime through the pass helper"
```

---

### Task 15: Migrate `DeferredPass`

`teardown()` frees this pass's own cascade UBO (`sun_ubo_`) and keeps the `sun_light_ubo_` mirror. No new caller of `teardown()`/`initialize()`.

**Files:**
- Modify: `extension/src/render/deferred_pass.h`, `extension/src/render/deferred_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged public API (`Params`, `kAmbient`, `set_sun_ubo`, `initialize`, `teardown`, `is_valid`, `render`, `last_ms`).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; change `bool is_valid() const { return shader_.is_valid() && pipeline_.is_valid(); }` to `bool is_valid() const { return program_.valid(); }`; delete the private `ensure_uniform_set` declaration; replace the member block (from `RenderingDevice *rd_ = nullptr;` to `float last_ms_ = 0.0f;`) with:

```cpp
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_linear_, sampler_nearest_;
	RID dummy_black_, dummy_far_, dummy_white_, sun_ubo_;
	RID sun_light_ubo_; // NOT owned: RenderOrchestrator frees it
	gpu::SetCache set_;
	float last_ms_ = 0.0f;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/deferred_pass.cpp`, remove the includes of `project_settings`, `rd_sampler_state`, `rd_shader_source`, `rd_shader_spirv`, `rd_texture_format`, `rd_texture_view`, `rd_uniform`, `utility_functions`, `render/shader_loader.h` and `<chrono>`.

Replace `initialize`, `set_sun_ubo`, `teardown`, `ensure_dummies` and `ensure_uniform_set` (everything from `void DeferredPass::initialize` to the end of `ensure_uniform_set`) with:

```cpp
void DeferredPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "DeferredPass", "deferred.comp.glsl");
	if (!program_.valid()) return;
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
}

void DeferredPass::set_sun_ubo(RID buffer) {
	// The uniform set keys on this RID, so the next render rebuilds it.
	sun_light_ubo_ = buffer;
}

void DeferredPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_linear_ = sampler_nearest_ = RID();
	dummy_black_ = dummy_far_ = dummy_white_ = sun_ubo_ = RID();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

bool DeferredPass::ensure_dummies(RenderingDevice *rd) {
	if (dummy_black_.is_valid() && dummy_far_.is_valid() && sun_ubo_.is_valid()) return true;
	auto make_1x1 = [&](RenderingDevice::DataFormat fmt, const PackedByteArray &bytes) {
		TypedArray<PackedByteArray> data;
		data.push_back(bytes);
		return gpu::texture(rd, group_, fmt, Vector2i(1, 1),
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT,
				data);
	};
	PackedByteArray black;
	black.resize(8);
	black.fill(0);
	dummy_black_ = make_1x1(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, black);
	PackedByteArray white;
	white.resize(8);
	// AO = 1 everywhere: a missing SSAO input must leave the ambient term untouched.
	white.fill(0x3C00); // half float 1.0
	dummy_white_ = make_1x1(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, white);
	PackedByteArray far;
	far.resize(4);
	far.fill(0);
	dummy_far_ = make_1x1(RenderingDevice::DATA_FORMAT_R32_SFLOAT, far);
	PackedByteArray zeros;
	zeros.resize(256);
	zeros.fill(0);
	sun_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(256, zeros));
	return dummy_black_.is_valid() && dummy_far_.is_valid() && dummy_white_.is_valid() && sun_ubo_.is_valid();
}
```

In `DeferredPass::render`, replace from `const auto t0 = std::chrono::steady_clock::now();` through `if (!ensure_uniform_set(rd, gb, materials, ssgi_bound, ssao_bound, sun_bound)) return false;` with:

```cpp
	gpu::CpuTimer timer(last_ms_);
	uint32_t flags = p.flags;
	if (!ssgi.is_valid()) flags &= ~ve::kFlagSsgi;
	if (!ssao.is_valid()) flags &= ~ve::kFlagSsao;
	if (!sun_map.is_valid()) flags &= ~ve::kFlagSunMap;
	const RID ssgi_bound = ssgi.is_valid() ? ssgi : dummy_black_;
	const RID ssao_bound = ssao.is_valid() ? ssao : dummy_white_;
	const RID sun_bound = sun_map.is_valid() ? sun_map : dummy_far_;
	gpu::RdDevice device{rd};
	// SSAO lives at binding 7, after the material arrays' reserved slots. Linear, not
	// nearest: the pass renders at half the G-buffer size, so this sampler is what
	// upsamples it. Nearest here would show the half-res grid as 2x2 blocks.
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::sampled(0, sampler_nearest_, gb.albedo()),
			gpu::sampled(1, sampler_nearest_, gb.surface()),
			gpu::sampled(2, sampler_nearest_, gb.depth()),
			gpu::sampled(3, sampler_linear_, ssgi_bound),
			gpu::sampled(4, sampler_linear_, sun_bound),
			gpu::image(5, gb.lit()),
			gpu::ubo(6, sun_ubo_),
			gpu::sampled(7, sampler_linear_, ssao_bound),
			gpu::sampled(8, materials.sampler(), materials.albedo_array()),
			gpu::sampled(9, materials.sampler(), materials.surface_array()),
			gpu::ubo(10, sun_light_ubo_)});
	if (!set.is_valid()) return false;
```

Replace the end of `render`, from `const Vector2i size = gb.size();` to the closing `return true;`, with:

```cpp
	const Vector2i size = gb.size();
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, pcb, gpu::groups(size.x, 8),
			gpu::groups(size.y, 8));
```
(The cascade-UBO packing and push packing between them stay exactly as they are.)

- [ ] **Step 3: No leftovers**

```bash
rg -n "key_|uset_|shader_\b|pipeline_|shader_compile|ensure_uniform_set|free_rid|chrono" extension/src/render/deferred_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **deferred**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/deferred_pass.h extension/src/render/deferred_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(deferred): lifetime through the pass helper"
```

---

### Task 16: Migrate `InjectPass`

**Files:**
- Modify: `extension/src/render/inject_pass.h`, `extension/src/render/inject_pass.cpp`

**Interfaces:**
- Consumes: Task 8's `compile_raster`, `sampler`, `FramebufferCache`, `RasterState`, `raster_pipeline`, `SetCache`.
- Produces: unchanged public API (`initialize`, `teardown`, `release_targets`, `draw`).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; replace the private section with:

```cpp
private:
	bool ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_, pipeline_, sampler_linear_, sampler_nearest_;
	gpu::FramebufferCache framebuffer_;
	gpu::SetCache set_;
};
```

- [ ] **Step 2: Implementation**

Replace `extension/src/render/inject_pass.cpp` with:

```cpp
#include "render/inject_pass.h"

using namespace godot;

InjectPass::~InjectPass() {
	teardown();
}

void InjectPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "InjectPass", "inject.vert.glsl", "inject.frag.glsl");
	if (!shader_.is_valid()) return;
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
}

void InjectPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

void InjectPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = sampler_linear_ = sampler_nearest_ = RID();
	framebuffer_ = gpu::FramebufferCache();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

bool InjectPass::ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth) {
	if (!shader_.is_valid()) return false;
	if (!framebuffer_.get(rd, group_, {dst_color, dst_depth}).is_valid()) return false;
	if (!pipeline_.is_valid()) {
		gpu::RasterState state;
		state.color_attachments = 1;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}

bool InjectPass::draw(RenderingDevice *rd, RID dst_color, RID dst_depth, RID lit, RID gb_depth) {
	if (!ensure_pipeline(rd, dst_color, dst_depth)) return false;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, shader_, 0, {
			gpu::sampled(0, sampler_linear_, lit),
			gpu::sampled(1, sampler_nearest_, gb_depth)});
	if (!set.is_valid()) return false;
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	rd->draw_list_draw(dl, false, 1, 3);
	rd->draw_list_end();
	return true;
}
```

- [ ] **Step 3: No leftovers**

```bash
rg -n "fb_|uset_|shader_compile|free_rid|Ref<RD" extension/src/render/inject_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **inject**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/inject_pass.h extension/src/render/inject_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(inject): lifetime through the pass helper"
```

---

### Task 17: Migrate `GrassRasterPass`

**Files:**
- Modify: `extension/src/render/grass_raster_pass.h`, `extension/src/render/grass_raster_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper; `GrassScatterPass::instance_buffer()`, `params_buffer()`, `draw_args_buffer()`, `last_blade_count()`.
- Produces: unchanged public API.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; replace the member lines from `RenderingDevice *rd_ = nullptr;` through `RID framebuffer_, fb_albedo_, fb_surface_, fb_depth_;` with:

```cpp
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_;
	RID pipeline_;
	gpu::FramebufferCache framebuffer_;
	gpu::SetCache set_;
```
(keep `last_vertex_count_` and the two private method declarations).

- [ ] **Step 2: Implementation**

In `extension/src/render/grass_raster_pass.cpp`, remove the includes of `project_settings`, `rd_pipeline_*`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `utility_functions` and `render/shader_loader.h`. Replace `initialize`, `teardown`, `release_targets`, `ensure_pipeline` and `ensure_uniform_set` with:

```cpp
void GrassRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "GrassRasterPass", "grass.vert.glsl", "grass.frag.glsl");
	if (!shader_.is_valid()) teardown();
}

void GrassRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = RID();
	framebuffer_ = gpu::FramebufferCache();
	set_ = gpu::SetCache();
	last_vertex_count_ = 0;
	rd_ = nullptr;
}

void GrassRasterPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

bool GrassRasterPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb) {
	if (!shader_.is_valid()) return false;
	// No marker attachment: blades write exactly the two colour channels the far field
	// writes, plus depth.
	if (!framebuffer_.get(rd, group_, {gb.albedo(), gb.surface(), gb.depth()}).is_valid()) return false;
	if (!pipeline_.is_valid()) {
		gpu::RasterState state;
		// Blades are single triangles seen from both sides.
		state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
		state.front = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		// Reverse-Z (M1 errata 2): near = 1, far = 0. GREATER_OR_EQUAL both writes our
		// depth where it is nearer than the current buffer and leaves nearer geometry
		// untouched -- the same compare the far field uses, so blades occlude correctly.
		state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		state.color_attachments = 2;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}

bool GrassRasterPass::ensure_uniform_set(RenderingDevice *rd, GrassScatterPass &scatter) {
	gpu::RdDevice device{rd};
	return set_.get(device, group_, shader_, 0, {
			gpu::storage(0, scatter.instance_buffer()),
			gpu::ubo(1, scatter.params_buffer())}).is_valid();
}
```

In `draw()`, change `rd->draw_list_begin(framebuffer_, …)` to `rd->draw_list_begin(framebuffer_.rid(), …)` and `rd->draw_list_bind_uniform_set(dl, uset_, 0);` to `rd->draw_list_bind_uniform_set(dl, set_.id(), 0);`.

- [ ] **Step 3: No leftovers**

```bash
rg -n "fb_|uset_|shader_compile|free_rid|Ref<RD" extension/src/render/grass_raster_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **grass_raster**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/grass_raster_pass.h extension/src/render/grass_raster_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(grass_raster): lifetime through the pass helper"
```

---

### Task 18: Migrate `LodRasterPass`

`initialize()` does not call `teardown()` first today; that stays.

**Files:**
- Modify: `extension/src/render/lod_raster_pass.h`, `extension/src/render/lod_raster_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper; `LodPool` buffer accessors; `MaterialAtlas` accessors.
- Produces: unchanged public API (`front_face_clockwise`, `prepare_index_array`, `index_array`, `clear_targets`, `release_targets`, `draw` …).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; replace the member lines from `RenderingDevice *rd_ = nullptr;` through `RID framebuffer_, fb_albedo_, fb_surface_, fb_depth_, fb_marker_;` with:

```cpp
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_, shader_marker_;
	RID pipeline_cull_off_;
	RID pipeline_cull_ccw_;
	RID pipeline_cull_cw_;
	gpu::SetCache set_;
	RID index_array_, index_array_buffer_;
	bool pipeline_marker_ = false;
	gpu::FramebufferCache framebuffer_;
```
(keep `draw_pages_`, `last_ms_`, `cull_enabled_`, `front_face_clockwise_` and their comments).

- [ ] **Step 2: Implementation**

In `extension/src/render/lod_raster_pass.cpp`, remove the includes of `project_settings`, `rd_pipeline_*`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `utility_functions`, `render/shader_loader.h`. Replace `initialize`, `teardown`, `release_targets`, `ensure_pipeline`, `ensure_uniform_set` and `ensure_index_array` with:

```cpp
void LodRasterPass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "LodRasterPass", "lod.vert.glsl", "lod.frag.glsl");
	// Same production/debug split as CompositePass: the marker output is compiled only for
	// the seam-probe shader variant, so production pipelines keep exactly one fragment output
	// and match the scene framebuffer's color mask.
	shader_marker_ = gpu::compile_raster(rd, group_, "LodRasterPass", "lod.vert.glsl",
			"lod.frag.glsl", "#define SEAM_MARKER 1\n");
}

void LodRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = shader_marker_ = RID();
	pipeline_cull_off_ = pipeline_cull_ccw_ = pipeline_cull_cw_ = RID();
	index_array_ = index_array_buffer_ = RID();
	set_ = gpu::SetCache();
	framebuffer_ = gpu::FramebufferCache();
	draw_pages_.clear();
	rd_ = nullptr;
}

void LodRasterPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

bool LodRasterPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb, RID marker) {
	const bool want_marker = marker.is_valid();
	const RID shader = want_marker ? shader_marker_ : shader_;
	if (!shader.is_valid()) return false;
	const std::vector<RID> attachments = want_marker
			? std::vector<RID>{gb.albedo(), gb.surface(), marker, gb.depth()}
			: std::vector<RID>{gb.albedo(), gb.surface(), gb.depth()};
	if (!framebuffer_.get(rd, group_, attachments).is_valid()) return false;
	if (pipeline_cull_off_.is_valid() && pipeline_cull_ccw_.is_valid() &&
			pipeline_cull_cw_.is_valid() && pipeline_marker_ == want_marker)
		return true;
	gpu::RdDevice device{rd};
	for (RID *p : {&pipeline_cull_off_, &pipeline_cull_ccw_, &pipeline_cull_cw_}) {
		group_.free(device, *p);
		*p = RID();
	}
	gpu::RasterState state;
	// Reverse-Z (M1 errata 2): near = 1, far = 0. GREATER_OR_EQUAL both writes our depth
	// where it is nearer than the current buffer and leaves nearer geometry untouched.
	state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	state.color_attachments = want_marker ? 3 : 2;
	// Debug seam probe: the LoD marker (2) must OR into the composite marker (1) so
	// double-claimed band pixels read 3. Production pipelines have no marker attachment.
	state.logic_or = want_marker;
	state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
	state.front = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
	pipeline_cull_off_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	state.cull = RenderingDevice::POLYGON_CULL_BACK;
	pipeline_cull_ccw_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	state.front = RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE;
	pipeline_cull_cw_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	pipeline_marker_ = want_marker;
	return pipeline_cull_off_.is_valid() && pipeline_cull_ccw_.is_valid() &&
			pipeline_cull_cw_.is_valid();
}

bool LodRasterPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials,
		RID shader) {
	gpu::RdDevice device{rd};
	return set_.get(device, group_, shader, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::sampled(3, materials.sampler(), materials.albedo_array()),
			gpu::sampled(4, materials.sampler(), materials.surface_array()),
			gpu::storage(5, pool.normal_buffer())}).is_valid();
}

bool LodRasterPass::ensure_index_array(RenderingDevice *rd, LodPool &pool) {
	const RID index_buffer = pool.index_buffer();
	if (index_array_.is_valid() && index_buffer == index_array_buffer_) return true;
	gpu::RdDevice device{rd};
	group_.free(device, index_array_);
	index_array_ = group_.add(gpu::Kind::IndexArray,
			rd->index_array_create(index_buffer, 0, ve::kLodQuadsPerPage * 6));
	index_array_buffer_ = index_buffer;
	return index_array_.is_valid();
}
```

In `clear_targets()` and `draw()`, change `framebuffer_` (as a `draw_list_begin` argument) to `framebuffer_.rid()`, and in `draw()` change `rd->draw_list_bind_uniform_set(dl, uset_, 0);` to `rd->draw_list_bind_uniform_set(dl, set_.id(), 0);`.

- [ ] **Step 3: No leftovers**

```bash
rg -n "fb_|uset_|shader_compile|free_rid|Ref<RD" extension/src/render/lod_raster_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **lod_raster**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/lod_raster_pass.h extension/src/render/lod_raster_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(lod_raster): lifetime through the pass helper"
```

---

### Task 19: Migrate `SunShadowPass`

The map array and its per-cascade slices are registered (slices after the map, so they are freed first); framebuffers too.

**Files:**
- Modify: `extension/src/render/sun_shadow_pass.h`, `extension/src/render/sun_shadow_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper; `LodRasterPass::prepare_index_array`, `index_array`, `draw_pages`.
- Produces: unchanged public API.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; replace the member lines from `RenderingDevice *rd_ = nullptr;` through `RID key_chunks_;` with:

```cpp
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID map_;
	RID shader_;
	RID pipeline_;
	gpu::SetCache set_;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/sun_shadow_pass.cpp`, remove the includes of `project_settings`, `rd_pipeline_*`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform` and `render/shader_loader.h`. Keep `utility_functions` (the slice-failure messages in `initialize` still print).

In `initialize`, change `map_ = rd_->texture_create(tf, tv, {});` to `map_ = group_.add(gpu::Kind::Texture, rd_->texture_create(tf, tv, {}));` and `c_[i].slice = rd_->texture_create_shared_from_slice(tv, map_, i, 0);` to `c_[i].slice = group_.add(gpu::Kind::Texture, rd_->texture_create_shared_from_slice(tv, map_, i, 0));`. Replace everything from `std::string err;` to the end of `initialize` with:

```cpp
	shader_ = gpu::compile_raster(rd_, group_, "SunShadowPass", "lod_shadow.vert.glsl",
			"lod_shadow.frag.glsl");
	if (!shader_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}
```

Replace `teardown` with:

```cpp
void SunShadowPass::teardown() {
	if (rd_) {
		gpu::RdDevice device{rd_};
		group_.release(device);
	}
	for (int i = 0; i < kCascades; i++) c_[i] = Cascade{};
	map_ = shader_ = pipeline_ = RID();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}
```

In `ensure_pipeline`, replace from its first line through `const int64_t format = rd->framebuffer_get_format(c_[0].framebuffer);` with:

```cpp
bool SunShadowPass::ensure_pipeline(RenderingDevice *rd) {
	if (rd->framebuffer_is_valid(c_[0].framebuffer) && pipeline_.is_valid()) return true;
	gpu::RdDevice device{rd};
	for (int i = 0; i < kCascades; i++) {
		group_.free(device, c_[i].framebuffer);
		c_[i].framebuffer = group_.add(gpu::Kind::Framebuffer,
				rd->framebuffer_create(Array::make(c_[i].slice)));
		if (!rd->framebuffer_is_valid(c_[i].framebuffer)) return false;
	}
	const int64_t format = rd->framebuffer_get_format(c_[0].framebuffer);
	group_.free(device, pipeline_);
```

and replace the rest of `ensure_pipeline`, from `Ref<RDPipelineRasterizationState> rs;` to its end, with (the two long comments are moved verbatim):

```cpp
	gpu::RasterState state;
	// NO CULLING, and no borrowed front-face convention.
	// <move the whole existing comment block that precedes rs->set_cull_mode here, unchanged>
	state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
	// <move the existing "Reverse-Z (near the sun = 1)..." comment block here, unchanged>
	state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	state.color_attachments = 0;
	pipeline_ = gpu::raster_pipeline(rd, group_, shader_, format, state);
	return pipeline_.is_valid();
}
```
The two `<move …>` markers are instructions to move the existing comment text, not text to type.

Replace `ensure_uniform_set` with:

```cpp
bool SunShadowPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool) {
	gpu::RdDevice device{rd};
	return set_.get(device, group_, shader_, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer())}).is_valid();
}
```

In `build()`, change `rd->draw_list_bind_uniform_set(dl, uset_, 0);` to `rd->draw_list_bind_uniform_set(dl, set_.id(), 0);`.

Before the edit, `rs` set only the cull mode, so its front face was RenderingDevice's default (clockwise) — the `RasterState` default. Confirm with `rg -n "set_front_face" extension/src/render/sun_shadow_pass.cpp` before replacing: expected no output; any hit means copy that value into `state.front`.

- [ ] **Step 3: No leftovers**

```bash
rg -n "key_|uset_|shader_compile|Ref<RDPipeline|free_rid" extension/src/render/sun_shadow_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **sun_shadow**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/sun_shadow_pass.h extension/src/render/sun_shadow_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(sun_shadow): lifetime through the pass helper

A rebuilt framebuffer set now frees the pipeline it replaces instead of leaking it."
```

---

### Task 20: Migrate `CompositePass`; delete `invalidate_uniform_set`

The composite set binds the raymarcher's textures. When those are recreated they take the set with them (device cascade) and the new RIDs change the cache key, so the manual invalidation and its three callers go.

**Files:**
- Modify: `extension/src/render/composite_pass.h`, `extension/src/render/composite_pass.cpp`
- Modify: `extension/src/render/frame.cpp` (two call sites)
- Modify: `extension/src/debug/hooks_render.cpp` (one call site)

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: `CompositePass` public API minus `invalidate_uniform_set`.

- [ ] **Step 1: Header**

In `extension/src/render/composite_pass.h`: add `#include "render/gpu/gpu.h"`; delete the `invalidate_uniform_set` declaration and its two-line comment; replace the member lines from `RenderingDevice *rd_ = nullptr;` through `RID framebuffer_, fb_albedo_, fb_surface_, fb_depth_, fb_marker_;` with:

```cpp
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_, shader_marker_;
	RID pipeline_;
	RID sampler_linear_, sampler_nearest_;
	gpu::SetCache set_;
	bool pipeline_marker_ = false;
	gpu::FramebufferCache framebuffer_;
```
(keep `last_draw_ok_`).

- [ ] **Step 2: Implementation**

In `extension/src/render/composite_pass.cpp`, remove the includes of `project_settings`, `rd_pipeline_*`, `rd_sampler_state`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `utility_functions`, `render/shader_loader.h`. Replace `initialize`, `release_targets`, `invalidate_uniform_set`, `teardown` and `ensure_pipeline` with:

```cpp
void CompositePass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "CompositePass", "composite.vert.glsl",
			"composite.frag.glsl");
	shader_marker_ = gpu::compile_raster(rd, group_, "CompositePass", "composite.vert.glsl",
			"composite.frag.glsl", "#define SEAM_MARKER 1\n");
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
}

void CompositePass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

void CompositePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = shader_marker_ = pipeline_ = sampler_linear_ = sampler_nearest_ = RID();
	set_ = gpu::SetCache();
	framebuffer_ = gpu::FramebufferCache();
	rd_ = nullptr;
}

bool CompositePass::ensure_pipeline(RenderingDevice *rd, RID albedo, RID surface, RID depth,
		RID marker) {
	const bool want_marker = marker.is_valid();
	const RID shader = want_marker ? shader_marker_ : shader_;
	if (!shader.is_valid()) return false;
	const std::vector<RID> attachments = want_marker
			? std::vector<RID>{albedo, surface, marker, depth}
			: std::vector<RID>{albedo, surface, depth};
	if (!framebuffer_.get(rd, group_, attachments).is_valid()) return false;
	if (!pipeline_.is_valid() || pipeline_marker_ != want_marker) {
		gpu::RdDevice device{rd};
		group_.free(device, pipeline_);
		gpu::RasterState state;
		state.color_attachments = want_marker ? 3 : 2;
		pipeline_marker_ = want_marker;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}
```

Before replacing `ensure_pipeline`, confirm its state matches `RasterState`'s defaults plus `color_attachments`: `rg -n "set_front_face|set_logic_op|set_cull_mode|set_depth_compare" extension/src/render/composite_pass.cpp` — expected exactly `set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED)` and `set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL)`.

In `draw()`, replace everything from `Ref<RDUniform> u0, u1, u2, u3, u4;` through `if (!uset_.is_valid()) return;` with:

```cpp
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, shader, 0, {
			gpu::sampled(0, sampler_linear_, src_overlay),
			gpu::sampled(1, sampler_nearest_, src_hitpos),
			gpu::sampled(2, materials.sampler(), materials.albedo_array()),
			gpu::sampled(3, materials.sampler(), materials.surface_array()),
			gpu::sampled(4, sampler_nearest_, src_surface)});
	if (!set.is_valid()) return;
```
and change `rd->draw_list_begin(framebuffer_,` to `rd->draw_list_begin(framebuffer_.rid(),` and `rd->draw_list_bind_uniform_set(dl, uset_, 0);` to `rd->draw_list_bind_uniform_set(dl, set, 0);`.

- [ ] **Step 3: Remove the callers**

`extension/src/render/frame.cpp`, in the raymarch stage — replace

```cpp
	// If the island cull mask/target size changes, RaymarchPass releases its old target
	// textures. CompositePass owns a uniform set that references those textures, so drop that
	// dependent set first rather than later attempting to free a cascade-invalid RID.
	if (rmp->targets_need_rebuild(rw, rh, effective_mask)) {
		cmp->release_targets();
		cmp->invalidate_uniform_set(rd);
	}
```
with
```cpp
	// If the island cull mask/target size changes, RaymarchPass releases its old target
	// textures. CompositePass's uniform set binds them and rebuilds itself on the new RIDs;
	// its framebuffer is dropped here as it always was.
	if (rmp->targets_need_rebuild(rw, rh, effective_mask)) cmp->release_targets();
```

`extension/src/render/frame.cpp`, in the headless-targets block — replace

```cpp
		if (CompositePass *composite = render_.passes().composite) {
			composite->release_targets();
			composite->invalidate_uniform_set(rd);
		}
```
with
```cpp
		if (CompositePass *composite = render_.passes().composite) composite->release_targets();
```

`extension/src/debug/hooks_render.cpp` — delete the line `world_->context().render->passes().composite->invalidate_uniform_set(device);` and change the comment above it from "the composite's framebuffer and uniform set reference both, so drop them before either moves." to "the composite's framebuffer references the G-buffer, so drop it before it moves; its uniform set rebuilds on the marcher's new RIDs."

- [ ] **Step 4: No leftovers**

```bash
rg -n "invalidate_uniform_set" extension/src
rg -n "fb_|uset_|shader_compile|free_rid|Ref<RD" extension/src/render/composite_pass.{h,cpp}
```
Expected: no output from either.

- [ ] **Step 5: Pass gate** for **composite**; also run `res://tests/test_near_field_scale.gd`.

- [ ] **Step 6: Commit**

```bash
git add extension/src/render/composite_pass.h extension/src/render/composite_pass.cpp \
	extension/src/render/frame.cpp extension/src/debug/hooks_render.cpp \
	docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(composite): lifetime through the pass helper; invalidate_uniform_set is gone"
```

---

### Task 21: Migrate `IslandCullPass`

**Files:**
- Modify: `extension/src/render/island_cull_pass.h`, `extension/src/render/island_cull_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper; `IslandAtlas::desc_buffer()`.
- Produces: unchanged public API (`initialize`, `teardown`, `is_valid`, `render`, `mask_buffer`, `tiles_x`, `tiles_y`).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; change `bool is_valid() const { return pipeline_.is_valid(); }` to `bool is_valid() const { return program_.pipeline.is_valid(); }`; replace the private section with:

```cpp
private:
	void rebuild_mask(RenderingDevice *rd, int tx, int ty);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID mask_;
	gpu::SetCache set_;
	int tiles_x_ = 0, tiles_y_ = 0;
};
```

- [ ] **Step 2: Implementation**

Replace everything in `extension/src/render/island_cull_pass.cpp` above `bool IslandCullPass::render` (includes, anonymous namespace, destructor, `initialize`, `teardown`, `rebuild`) with:

```cpp
#include "render/island_cull_pass.h"
#include "render/island_atlas.h"
#include <cstring>

using namespace godot;

IslandCullPass::~IslandCullPass() {
	teardown();
}

bool IslandCullPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "IslandCullPass", "island_cull.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	return true;
}

void IslandCullPass::teardown() {
	if (rd_) {
		gpu::RdDevice device{rd_};
		group_.release(device);
	}
	program_ = gpu::Program();
	mask_ = RID();
	set_ = gpu::SetCache();
	tiles_x_ = 0;
	tiles_y_ = 0;
	rd_ = nullptr;
}

void IslandCullPass::rebuild_mask(RenderingDevice *rd, int tx, int ty) {
	// Freeing the old mask takes its uniform set with it; the cache rebuilds on the new RID.
	gpu::RdDevice device{rd};
	group_.free(device, mask_);
	PackedByteArray zero;
	zero.resize(static_cast<int64_t>(tx) * ty * 4);
	zero.fill(0);
	mask_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(zero.size()), zero));
	tiles_x_ = tx;
	tiles_y_ = ty;
}
```

In `render()`, replace

```cpp
	if (tx != tiles_x_ || ty != tiles_y_ || !uset_.is_valid()) rebuild(rd, atlas, tx, ty);
	if (!uset_.is_valid()) return false;
```
with
```cpp
	if (tx != tiles_x_ || ty != tiles_y_ || !mask_.is_valid()) rebuild_mask(rd, tx, ty);
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0,
			{gpu::storage(0, atlas.desc_buffer()), gpu::storage(1, mask_)});
	if (!set.is_valid()) return false;
```
and replace the compute list at the end (from `const int64_t list = rd->compute_list_begin();` through `return true;`) with:

```cpp
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, b, gpu::groups(tx, 8), gpu::groups(ty, 8));
```
Keep the comment above the old list ("Its own compute list. Godot's RenderingDevice ends a compute list with a full barrier…") above the `return`.

- [ ] **Step 3: No leftovers**

```bash
rg -n "uset_|free_if_valid|storage\(int|shader_compile|rebuild\(" extension/src/render/island_cull_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **island_cull**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/island_cull_pass.h extension/src/render/island_cull_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(island_cull): lifetime through the pass helper"
```

---

### Task 22: Migrate `LodCullPass`

**Files:**
- Modify: `extension/src/render/lod_cull_pass.h`, `extension/src/render/lod_cull_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper; `HizPass::pyramid()`; `LodPool` buffers.
- Produces: unchanged public API.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; change `bool is_valid() const { return pipeline_.is_valid(); }` to `return program_.pipeline.is_valid();`; replace

```cpp
	RID shader_, pipeline_, sampler_, stats_, uset_;
	RID uset_args_, uset_page_chunk_, uset_chunks_, uset_hiz_, uset_stats_;
```
with
```cpp
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_, stats_;
	gpu::SetCache set_;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/lod_cull_pass.cpp`, remove the includes of `project_settings`, `rd_sampler_state`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `render/shader_loader.h`. In `initialize`, replace from `ProjectSettings *ps = …` through the sampler block's closing `}` (the one after `sampler_ = rd->sampler_create(ss);`) with:

```cpp
	program_ = gpu::compile_compute(rd, group_, "LodCullPass", "lod_cull.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!sampler_.is_valid()) {
		teardown();
		return false;
	}
```
and change `stats_ = rd->storage_buffer_create(4, zero);` to `stats_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(4, zero));`.

In `teardown`, replace from `// Uniform set first: it references the shader, stats, and pool buffers.` through `uset_stats_ = RID();` with:

```cpp
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_ = stats_ = RID();
	set_ = gpu::SetCache();
```

Replace `ensure_uniform_set` with:

```cpp
bool LodCullPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool, HizPass *hiz) {
	if (!hiz || !hiz->pyramid().is_valid()) return false;
	gpu::RdDevice device{rd};
	return set_.get(device, group_, program_.shader, 0, {
			gpu::storage(0, pool.args_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::sampled(3, sampler_, hiz->pyramid()),
			gpu::storage(4, stats_)}).is_valid();
}
```

In `run()`, change `!pipeline_.is_valid()` to `!program_.valid()`, `rd->compute_list_bind_compute_pipeline(list, pipeline_);` to `…(list, program_.pipeline);` and `rd->compute_list_bind_uniform_set(list, uset_, 0);` to `…(list, set_.id(), 0);`. The rest of `run()` is unchanged.

- [ ] **Step 3: No leftovers**

```bash
rg -n "uset_|pipeline_|shader_\b|shader_compile|Ref<RDUniform>|free_rid" extension/src/render/lod_cull_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **lod_cull**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/lod_cull_pass.h extension/src/render/lod_cull_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(lod_cull): lifetime through the pass helper"
```

---

### Task 23: Migrate `GrassScatterPass`

**Files:**
- Modify: `extension/src/render/grass_scatter_pass.h`, `extension/src/render/grass_scatter_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper; `GpuAtlas` accessors.
- Produces: unchanged public API (`instance_buffer`, `draw_args_buffer`, `params_buffer`, counters …).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`. Replace the member lines from `RID bricks_shader_, bricks_pipeline_;` through `RID key_sregion_, key_sinstances_, key_sslot_counts_, key_ssun_;` with:

```cpp
	gpu::Group group_;
	gpu::Program bricks_;
	gpu::Program scatter_; // invalid when grass_scatter.comp.glsl failed: cull-only
	RID params_ubo_, brick_list_, counters_, dispatch_args_, instances_, draw_args_;
	RID region_ubo_;
	RID sampler_linear_, sampler_nearest_;
	gpu::SetCache bricks_set_, scatter_set_;
```
Keep any comment lines that sat between those members (for example the one above `region_ubo_`) above the corresponding new line.

- [ ] **Step 2: Implementation**

In `extension/src/render/grass_scatter_pass.cpp`: remove the includes of `project_settings`, `rd_sampler_state`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `render/shader_loader.h`; delete the anonymous-namespace `load_compute`.

In `initialize`, replace from `if (!load_compute(rd, "res://shaders/grass_bricks.comp.glsl", …` through the closing `}` of `if (!sampler_linear_.is_valid() || !sampler_nearest_.is_valid()) { … }` with:

```cpp
	bricks_ = gpu::compile_compute(rd, group_, "GrassScatterPass", "grass_bricks.comp.glsl");
	if (!bricks_.valid()) {
		teardown();
		return false;
	}
	// grass_scatter.comp.glsl arrives in Task 6. Until then the scatter stage stays
	// invalid and run() skips stage 2 -- the pass still culls bricks and reports zeros.
	scatter_ = gpu::compile_compute(rd, group_, "GrassScatterPass", "grass_scatter.comp.glsl");
	if (!scatter_.valid()) scatter_ = gpu::Program();
	// Owned sampler pair for the atlas textures stage 1 declares but never samples.
	// Mirrors RaymarchPass: the SDF atlas filters linearly, the integer material atlas
	// must stay nearest.
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR, true);
	if (!sampler_linear_.is_valid() || !sampler_nearest_.is_valid()) {
		teardown();
		return false;
	}
```

In `teardown`, replace the `for (RID *r : {…}) { … }` loop and the seven `key_… = RID();` lines with:

```cpp
	gpu::RdDevice device{rd_};
	group_.release(device);
	bricks_ = scatter_ = gpu::Program();
	params_ubo_ = brick_list_ = counters_ = dispatch_args_ = instances_ = draw_args_ = RID();
	region_ubo_ = sampler_linear_ = sampler_nearest_ = RID();
	bricks_set_ = scatter_set_ = gpu::SetCache();
```

In `ensure_buffers`, replace the `for (RID *r : {&instances_, …, &scatter_uset_}) { … }` loop and the seven `… = rd->…_create(…)` assignments with:

```cpp
	// Freeing a buffer takes the uniform sets that bind it; both caches rebuild on new RIDs.
	gpu::RdDevice device{rd};
	for (RID *r : {&instances_, &brick_list_, &counters_, &dispatch_args_, &draw_args_,
			&params_ubo_, &region_ubo_}) {
		group_.free(device, *r);
		*r = RID();
	}
	// 32 bytes per blade: two vec4 (design doc section 5).
	instances_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(max_blades) * 32u));
	brick_list_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(max_bricks) * 4u));
	counters_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u));
	dispatch_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(12u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT));
	// Non-indexed indirect draw args: vertexCount, instanceCount, firstVertex, firstInstance.
	draw_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT));
	params_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::GrassParams)));
	// Region-window block for stage 1's binding 10 (three ivec4: dims, region_origin,
	// atlas_bricks). Contents refresh every run(); the RID is stable so the uniform set
	// survives across frames -- cached against the RID, never rebuilt per frame.
	region_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(48u));
```

Replace `ensure_uniform_sets` with:

```cpp
bool GrassScatterPass::ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo) {
	gpu::RdDevice device{rd};
	// Stage-1 set: 0 grass-params, 1 brick_list, 2 counters, 3 dispatch_args, 4 region_map,
	// 5 region_tables, 6 brick_flags, 7 sdf_atlas, 8 mat_atlas, 9 palette_buf, 10 region UBO.
	// Texture/sampler RIDs come from GpuAtlas through the same accessors RaymarchPass uses.
	// Bindings 7-8 are the atlas textures the sampling helpers declare but stage 1 never
	// fetches; a declared binding still has to be provided.
	const RID bricks = bricks_set_.get(device, group_, bricks_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, brick_list_),
			gpu::storage(2, counters_),
			gpu::storage(3, dispatch_args_),
			gpu::storage(4, atlas.region_map()),
			gpu::storage(5, atlas.region_tables()),
			gpu::storage(6, atlas.brick_flags()),
			gpu::sampled(7, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(8, sampler_nearest_, atlas.mat_atlas()),
			gpu::storage(9, atlas.palette()),
			gpu::ubo(10, region_ubo_)});
	if (!bricks.is_valid()) return false;
	// Stage-2 set mirrors grass_scatter.comp.glsl bindings 0-13: 0 grass-params, 1
	// brick_list, 2 counters, 3 draw_args, 4 region_map, 5 region_tables, 6 brick_flags,
	// 7 palette_buf, 8 sdf_atlas, 9 mat_atlas, 10 region UBO, 11 instances, 12
	// region_slot_counts, 13 the frame's SunUbo -- the last two feed the sun march. Binding 3 is
	// draw_args_ here, NOT dispatch_args_ (that is stage 1's binding 3).
	if (!scatter_.valid()) return true; // scatter stage absent: cull only.
	if (!sun_ubo.is_valid() || !atlas.region_slot_counts().is_valid()) return false;
	return scatter_set_.get(device, group_, scatter_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, brick_list_),
			gpu::storage(2, counters_),
			gpu::storage(3, draw_args_),
			gpu::storage(4, atlas.region_map()),
			gpu::storage(5, atlas.region_tables()),
			gpu::storage(6, atlas.brick_flags()),
			gpu::storage(7, atlas.palette()),
			gpu::sampled(8, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(9, sampler_nearest_, atlas.mat_atlas()),
			gpu::ubo(10, region_ubo_),
			gpu::storage(11, instances_),
			gpu::storage(12, atlas.region_slot_counts()),
			gpu::ubo(13, sun_ubo)}).is_valid();
}
```

In `run()`: `!bricks_pipeline_.is_valid()` → `!bricks_.valid()`; `bind_compute_pipeline(list, bricks_pipeline_)` → `(list, bricks_.pipeline)`; `bind_uniform_set(list, bricks_uset_, 0)` → `(list, bricks_set_.id(), 0)`; `if (scatter_pipeline_.is_valid())` → `if (scatter_.valid())`; `bind_compute_pipeline(list, scatter_pipeline_)` → `(list, scatter_.pipeline)`; `bind_uniform_set(list, scatter_uset_, 0)` → `(list, scatter_set_.id(), 0)`. Then `rg -n "bricks_shader_|scatter_shader_" extension/src/render/grass_scatter_pass.cpp` and replace any remaining use with `bricks_.shader` / `scatter_.shader`.

- [ ] **Step 3: No leftovers**

```bash
rg -n "key_|_uset_|_pipeline_|_shader_|load_compute|Ref<RDUniform>|free_rid" extension/src/render/grass_scatter_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **grass_scatter**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/grass_scatter_pass.h extension/src/render/grass_scatter_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(grass_scatter): lifetime through the pass helper"
```

---

### Task 24: Migrate `RaymarchPass`

Targets and the cost buffer are still recreated together whenever size, mask or set validity change (`targets_need_rebuild` is what the frame asks before releasing composite targets, so its rule is unchanged). The two uniform sets become caches.

**Files:**
- Modify: `extension/src/render/raymarch_pass.h`, `extension/src/render/raymarch_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper; `GpuAtlas`, `IslandAtlas`, `MaterialAtlas`, `FieldContextSet::bind`.
- Produces: unchanged public API (`set_materials`, `set_sun_ubo`, `render`, `targets_need_rebuild`, `albedo_texture`, `surface_texture`, `hitpos_texture`, `cost_buffer`).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"` and `#include <vector>`. Replace the private section (from `private:` to the class's closing `};`) with:

```cpp
private:
	void rebuild_targets(RenderingDevice *rd, int w, int h);
	std::vector<gpu::Uniform> uniforms(const GpuAtlas &atlas, const IslandAtlas &islands,
			RID mask) const;

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_;     // shared NEAREST sampler, created once
	// The SDF atlas is the one target sampled with hardware trilinear (binding 2): its
	// 17-voxel apron keeps a filtered fetch inside the brick's own block, so one fetch
	// replaces the eight brick_sdf() used to issue. The material and min-max atlases stay
	// on sampler_ -- they are integer textures and cannot be filtered at all.
	RID sampler_linear_;
	RID edits_ubo_;   // 32-byte uniform buffer, updated every render
	RID sun_ubo_; // NOT owned: RenderOrchestrator frees it
	RID material_albedo_, material_surface_, material_sampler_;
	RID albedo_, surface_, hitpos_, cost_buf_;
	gpu::SetCache set_, sun_set_; // set 0; set 2 (SunLight)
	RID uset_mask_;
	int width_ = 0, height_ = 0;
};
```

- [ ] **Step 2: Implementation**

In `extension/src/render/raymarch_pass.cpp`, remove the includes of `project_settings`, `rd_sampler_state`, `rd_shader_source`, `rd_shader_spirv`, `rd_texture_format`, `rd_texture_view`, `rd_uniform`, `utility_functions`, `render/shader_loader.h`. Replace everything from `void RaymarchPass::initialize` through the end of `RaymarchPass::targets_need_rebuild` with:

```cpp
void RaymarchPass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "RaymarchPass", "raymarch.comp.glsl");
	if (!program_.valid()) return;
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	// Explicit clamp: brick_sdf() only ever asks for coordinates inside the brick's own
	// 17-voxel block, but an edge brick must not wrap to the far side of the atlas if a
	// coordinate lands exactly on the boundary.
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR, true);
	PackedByteArray zero;
	zero.resize(32);
	zero.fill(0);
	edits_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(32, zero));
}

void RaymarchPass::set_materials(const MaterialAtlas &materials) {
	// The set-0 cache keys on these RIDs, so the next render rebuilds with the new arrays.
	material_albedo_ = materials.albedo_array();
	material_surface_ = materials.surface_array();
	material_sampler_ = materials.sampler();
}

void RaymarchPass::set_sun_ubo(RID buffer) {
	// The set-2 cache keys on this RID.
	sun_ubo_ = buffer;
}

void RaymarchPass::teardown() {
	if (!rd_) return;
	// uset_mask_ is only a cache key for an externally owned tile-mask RID (usually the
	// IslandAtlas fallback mask); it is not registered and never freed here.
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_ = sampler_linear_ = edits_ubo_ = RID();
	albedo_ = surface_ = hitpos_ = cost_buf_ = RID();
	set_ = sun_set_ = gpu::SetCache();
	uset_mask_ = RID();
	sun_ubo_ = RID();
	material_albedo_ = RID();
	material_surface_ = RID();
	material_sampler_ = RID();
	rd_ = nullptr;
}

void RaymarchPass::rebuild_targets(RenderingDevice *rd, int w, int h) {
	// The old targets and cost buffer take set 0 with them (device cascade); its cache
	// rebuilds on the new RIDs.
	gpu::RdDevice device{rd};
	for (RID *r : {&albedo_, &surface_, &hitpos_, &cost_buf_}) {
		group_.free(device, *r);
		*r = RID();
	}
	const uint32_t usage = RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	albedo_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, Vector2i(w, h), usage);
	surface_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, Vector2i(w, h), usage);
	hitpos_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, Vector2i(w, h), usage);
	cost_buf_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(w) * h * 2u * sizeof(uint32_t)));
	width_ = w;
	height_ = h;
}

std::vector<gpu::Uniform> RaymarchPass::uniforms(const GpuAtlas &atlas, const IslandAtlas &islands,
		RID mask) const {
	return {
		gpu::image(0, albedo_),
		gpu::image(1, hitpos_),
		// Binding 2 is the R8_UNORM SDF atlas and is the only one filtered in hardware.
		gpu::sampled(2, sampler_linear_, atlas.sdf_atlas()),
		gpu::sampled(3, sampler_, atlas.mat_atlas()),
		gpu::sampled(4, sampler_, atlas.mip_atlas(0)),
		gpu::sampled(5, sampler_, atlas.mip_atlas(1)),
		gpu::sampled(6, sampler_, atlas.mip_atlas(2)),
		gpu::storage(7, atlas.palette()),
		gpu::storage(8, atlas.region_map()),
		gpu::storage(9, atlas.region_tables()),
		gpu::storage(10, atlas.op_pool()),
		gpu::storage(11, atlas.op_counts()),
		gpu::ubo(12, edits_ubo_),
		// 13-17: shared authoritative volume SDF/material (indexed by Island.volume_slot since
		// Task 6), island min-max chain, descriptors, tile mask. Atlas slot still selects the
		// descriptor/mip/tile-mask entries.
		gpu::storage(13, atlas.volumes().sdf_buffer()),
		gpu::storage(14, atlas.volumes().mat_buffer()),
		gpu::storage(15, islands.mip_buffer()),
		gpu::storage(16, islands.desc_buffer()),
		gpu::storage(17, mask),
		// 18-19: the shared material arrays (set by set_materials()).
		gpu::sampled(18, material_sampler_, material_albedo_),
		gpu::sampled(19, material_sampler_, material_surface_),
		gpu::image(20, surface_),
		gpu::storage(21, atlas.brick_flags()),
		gpu::storage(22, atlas.region_slot_counts()),
		gpu::storage(23, cost_buf_),
		// 24-26: the compact-normal pool -- packed payload plus BOTH offset tables (per volume
		// slot, per override-brick slot). -1 in a table row means "no normals bound".
		gpu::storage(24, atlas.stored_normals().normal_buffer()),
		gpu::storage(25, atlas.stored_normals().volume_offsets_buffer()),
		gpu::storage(26, atlas.stored_normals().override_offsets_buffer()),
		// 27-30: the shared authoritative override pool (SDF bytes, material bytes, brick
		// tables, region-to-table map) the field evaluator consults for shading normals.
		gpu::storage(27, atlas.overrides().sdf_buffer()),
		gpu::storage(28, atlas.overrides().mat_buffer()),
		gpu::storage(29, atlas.overrides().tables()),
		gpu::storage(30, atlas.overrides().region_table_map()),
	};
}

bool RaymarchPass::targets_need_rebuild(int width, int height, RID mask) const {
	return width != width_ || height != height_ || mask != uset_mask_ ||
			(rd_ && !rd_->uniform_set_is_valid(set_.id()));
}
```

In `render()`, replace from `if (!shader_.is_valid()) return false;` through `!albedo_.is_valid() || !surface_.is_valid() || !edits_ubo_.is_valid()) return false;` with:

```cpp
	if (!program_.valid()) return false;
	if (!islands || !islands->is_valid()) return false;
	const RID mask = tile_mask.is_valid() ? tile_mask : islands->fallback_mask();
	if (width != width_ || height != height_ || mask != uset_mask_ ||
			!rd->uniform_set_is_valid(set_.id())) {
		rebuild_targets(rd, width, height);
		uset_mask_ = mask;
	}
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, uniforms(atlas, *islands, mask));
	const RID sun_set = sun_set_.get(device, group_, program_.shader, 2, {gpu::ubo(24, sun_ubo_)});
	if (!set.is_valid() || !sun_set.is_valid() || !albedo_.is_valid() || !surface_.is_valid() ||
			!edits_ubo_.is_valid()) return false;
```
and in the compute list change `pipeline_` → `program_.pipeline`, `uset_` → `set`, `sun_uset_` → `sun_set`. Everything else in `render()` is unchanged.

The old set array pushed a 32nd, unconfigured `RDUniform` (`u[31]`); Godot matches uniforms by binding and ignored it. It is not carried over (planning decision 9).

- [ ] **Step 3: No leftovers**

```bash
rg -n "sun_uset_|uset_\b|pipeline_|shader_\b|make_target|shader_compile|Ref<RDUniform>|free_rid" extension/src/render/raymarch_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **raymarch**; also run `res://tests/test_island_render.gd,res://tests/test_raymarch_cost.gd`.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/raymarch_pass.h extension/src/render/raymarch_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(raymarch): lifetime through the pass helper

Drops the unconfigured 32nd RDUniform the set array carried; Godot ignored it."
```

---

### Task 25: Migrate the orchestrator's downsample and shader pre-flight

**Files:**
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`

**Interfaces:**
- Consumes: Task 8's `compile_compute`, `compile_check`, `sampler`, `SetCache`, `dispatch`.
- Produces: unchanged `initialize_downsample`, `teardown_downsample`, `downsample_history`, `preflight_shaders`; `ensure_downsample_set` deleted.

- [ ] **Step 1: Header**

`extension/src/render/orchestrator.h`: add `#include "render/gpu/gpu.h"`; delete `bool ensure_downsample_set(RenderingDevice *device, RID src, RID dst);`; replace

```cpp
	RID downsample_shader_, downsample_pipeline_, downsample_sampler_, downsample_uset_;
	RID downsample_src_, downsample_dst_;
```
with
```cpp
	gpu::Group downsample_group_;
	gpu::Program downsample_;
	RID downsample_sampler_;
	gpu::SetCache downsample_set_;
```

- [ ] **Step 2: Implementation**

`extension/src/render/orchestrator.cpp`: replace `initialize_downsample`, `teardown_downsample` and `ensure_downsample_set` with:

```cpp
bool RenderOrchestrator::initialize_downsample(RenderingDevice *rd) {
	teardown_downsample();
	if (!rd) return false;
	downsample_ = gpu::compile_compute(rd, downsample_group_, "RenderOrchestrator", "downsample.comp.glsl");
	downsample_sampler_ = gpu::sampler(rd, downsample_group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	if (!downsample_.valid() || !downsample_sampler_.is_valid()) {
		teardown_downsample();
		return false;
	}
	return true;
}

void RenderOrchestrator::teardown_downsample() {
	if (RenderingDevice *device = rd()) {
		gpu::RdDevice d{device};
		downsample_group_.release(d);
	} else {
		// No device to free on: its RIDs died with it.
		downsample_group_ = gpu::Group();
	}
	downsample_ = gpu::Program();
	downsample_sampler_ = RID();
	downsample_set_ = gpu::SetCache();
}
```

In `downsample_history`, replace

```cpp
	if (!rd || !downsample_pipeline_.is_valid() || !gb.history().is_valid()) return false;
	const Vector2i half = gb.half_size();
	if (!ensure_downsample_set(rd, src, gb.history())) return false;
```
with
```cpp
	if (!rd || !downsample_.valid() || !gb.history().is_valid()) return false;
	const Vector2i half = gb.half_size();
	gpu::RdDevice device{rd};
	const RID set = downsample_set_.get(device, downsample_group_, downsample_.shader, 0,
			{gpu::sampled(0, downsample_sampler_, src), gpu::image(1, gb.history())});
	if (!set.is_valid()) return false;
```
and replace the compute list (from `const int64_t list = rd->compute_list_begin();` through `rd->compute_list_end();`) with:

```cpp
	if (!gpu::dispatch(rd, downsample_.pipeline, {{set, 0}}, pc, gpu::groups(half.x, 8),
				gpu::groups(half.y, 8)))
		return false;
```

In `preflight_shaders`, replace the block from `const String path = ps->globalize_path(res);` through the closing `}` of `if (!compile_err.is_empty()) { … }` with:

```cpp
			RenderingDevice::ShaderStage stage = RenderingDevice::SHADER_STAGE_COMPUTE;
			if (file.ends_with(".vert.glsl")) stage = RenderingDevice::SHADER_STAGE_VERTEX;
			else if (file.ends_with(".frag.glsl")) stage = RenderingDevice::SHADER_STAGE_FRAGMENT;
			if (!gpu::compile_check(rd, res, stage, out_error)) {
				ok = false;
				break;
			}
```
and delete `const String inc = ps->globalize_path("res://shaders");` and `ProjectSettings *ps = …;` if nothing else in the function uses them (the build warns on unused locals). Remove `rd_shader_source`, `rd_shader_spirv`, `rd_uniform` and `render/shader_loader.h` includes from the file only if `rg` shows no remaining use.

- [ ] **Step 3: No leftovers**

```bash
rg -n "downsample_(shader|pipeline|uset|src|dst)_|ensure_downsample_set|shader_compile_spirv_from_source" extension/src/render/orchestrator.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **orchestrator**. `test_shader_reload.gd` checks the pre-flight error text: it must still read `res://shaders/<file>: <compiler message>`.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(orchestrator): downsample and shader pre-flight through the pass helper"
```

---

### Tasks 26–31: world-job passes

These passes build their uniform sets once in `initialize()` against buffers whose identity never changes, so they use `gpu::uniform_set`, not `SetCache`. Each task states the member renames; after editing, the rename table's left column must not appear in the two files.

---

### Task 26: Migrate `BrickGenPass`

The field context's set-1 sets are created against this pass's shader (`shader()`), so releasing the group takes them too — the same order as today.

**Files:**
- Modify: `extension/src/render/brick_gen_pass.h`, `extension/src/render/brick_gen_pass.cpp`

**Interfaces:**
- Consumes: Task 8's `compile_compute`, `uniform_set`; `GpuAtlas` accessors.
- Produces: unchanged `initialize`, `teardown`, `is_valid`, `shader`, `dispatch`.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; `bool is_valid() const { return program_.pipeline.is_valid(); }`; `RID shader() const { return program_.shader; }`; replace `RID shader_, pipeline_, uset_;` with:

```cpp
	gpu::Group group_;
	gpu::Program program_;
	RID set_;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/brick_gen_pass.cpp`, remove the includes of `project_settings`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `render/shader_loader.h` and the anonymous-namespace `storage` and `free_if_valid` helpers. Replace `initialize` and `teardown` with:

```cpp
bool BrickGenPass::initialize(RenderingDevice *rd, const GpuAtlas &atlas) {
	teardown();
	rd_ = rd;
	atlas_bricks_ = atlas.config().atlas_bricks;
	program_ = gpu::compile_compute(rd, group_, "BrickGenPass", "brick_gen.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	// The atlas buffers never change identity, so the uniform set is built once.
	set_ = gpu::uniform_set(rd, group_, program_.shader, 0, {
			gpu::image(0, atlas.sdf_atlas()),
			gpu::image(1, atlas.mat_atlas()),
			gpu::image(2, atlas.mip_atlas(0)),
			gpu::image(3, atlas.mip_atlas(1)),
			gpu::image(4, atlas.mip_atlas(2)),
			gpu::storage(5, atlas.palette()),
			gpu::storage(6, atlas.jobs()),
			gpu::storage(7, atlas.op_pool()),
			gpu::storage(8, atlas.volumes().sdf_buffer()),
			gpu::storage(9, atlas.volumes().mat_buffer()),
			gpu::storage(10, atlas.brick_flags()),
			gpu::storage(11, atlas.overrides().sdf_buffer()),
			gpu::storage(12, atlas.overrides().mat_buffer()),
			gpu::storage(13, atlas.overrides().tables()),
			gpu::storage(14, atlas.overrides().region_table_map()),
			gpu::storage(15, atlas.region_occupancy())});
	if (!set_.is_valid()) {
		UtilityFunctions::printerr("BrickGenPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void BrickGenPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	set_ = RID();
	rd_ = nullptr;
}
```

Renames in `dispatch()`: `pipeline_` → `program_.pipeline`, `uset_` → `set_`.

- [ ] **Step 3: No leftovers**

```bash
rg -n "pipeline_|uset_|shader_\b|free_if_valid|Ref<RDUniform>|shader_compile" extension/src/render/brick_gen_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **brick_gen**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/brick_gen_pass.h extension/src/render/brick_gen_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(brick_gen): lifetime through the pass helper"
```

---

### Task 27: Migrate `RegionPass`

**Files:**
- Modify: `extension/src/render/region_pass.h`, `extension/src/render/region_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged `initialize`, `teardown`, `is_valid`, `mark`, `release_region`, `write_dispatch_args`; private `build` deleted.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; `bool is_valid() const { return mark_.pipeline.is_valid(); }`; delete `bool build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline);`; replace the three `RID mark_shader_…`, `RID free_shader_…`, `RID args_shader_…` lines with:

```cpp
	gpu::Group group_;
	gpu::Program mark_, free_, args_;
	RID mark_set_, free_set_, args_set_;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/region_pass.cpp`, remove the includes of `project_settings`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `render/shader_loader.h`, the anonymous-namespace `storage`/`free_if_valid`, and `RegionPass::build`. Replace `initialize` and `teardown` with:

```cpp
bool RegionPass::initialize(RenderingDevice *rd, const GpuAtlas &atlas) {
	teardown();
	rd_ = rd;
	max_brick_jobs_ = atlas.config().max_brick_jobs;
	mark_ = gpu::compile_compute(rd, group_, "RegionPass", "brick_mark.comp.glsl");
	free_ = gpu::compile_compute(rd, group_, "RegionPass", "region_free.comp.glsl");
	args_ = gpu::compile_compute(rd, group_, "RegionPass", "dispatch_args.comp.glsl");
	if (!mark_.valid() || !free_.valid() || !args_.valid()) {
		teardown();
		return false;
	}
	// The atlas buffers never change identity, so the uniform sets are built once.
	mark_set_ = gpu::uniform_set(rd, group_, mark_.shader, 0, {
			gpu::storage(0, atlas.region_tables()),
			gpu::storage(1, atlas.free_list()),
			gpu::storage(2, atlas.counters()),
			gpu::storage(3, atlas.frame_counters()),
			gpu::storage(4, atlas.op_pool()),
			gpu::storage(5, atlas.jobs()),
			gpu::storage(6, atlas.region_slot_counts()),
			gpu::storage(7, atlas.volumes().sdf_buffer()),
			gpu::storage(8, atlas.volumes().mat_buffer()),
			gpu::storage(9, atlas.region_occupancy()),
			gpu::storage(10, atlas.brick_flags()),
			gpu::storage(11, atlas.overrides().sdf_buffer()),
			gpu::storage(12, atlas.overrides().mat_buffer()),
			gpu::storage(13, atlas.overrides().tables()),
			gpu::storage(14, atlas.overrides().region_table_map())});
	free_set_ = gpu::uniform_set(rd, group_, free_.shader, 0, {
			gpu::storage(0, atlas.region_tables()),
			gpu::storage(1, atlas.free_list()),
			gpu::storage(2, atlas.counters()),
			gpu::storage(3, atlas.region_slot_counts()),
			gpu::storage(4, atlas.region_occupancy())});
	args_set_ = gpu::uniform_set(rd, group_, args_.shader, 0, {
			gpu::storage(0, atlas.frame_counters()),
			gpu::storage(1, atlas.dispatch_args())});
	if (!mark_set_.is_valid() || !free_set_.is_valid() || !args_set_.is_valid()) {
		UtilityFunctions::printerr("RegionPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void RegionPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	mark_ = free_ = args_ = gpu::Program();
	mark_set_ = free_set_ = args_set_ = RID();
	rd_ = nullptr;
}
```

Renames in `mark`, `release_region`, `write_dispatch_args`:

| Old | New |
|---|---|
| `mark_pipeline_` | `mark_.pipeline` |
| `mark_uset_` | `mark_set_` |
| `free_pipeline_` | `free_.pipeline` |
| `free_uset_` | `free_set_` |
| `args_pipeline_` | `args_.pipeline` |
| `args_uset_` | `args_set_` |

- [ ] **Step 3: No leftovers**

```bash
rg -n "_pipeline_|_uset_|_shader_|free_if_valid|::build|Ref<RDUniform>" extension/src/render/region_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **region**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/region_pass.h extension/src/render/region_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(region): lifetime through the pass helper"
```

---

### Task 28: Migrate `ConsolidatePass`

**Files:**
- Modify: `extension/src/render/consolidate_pass.h`, `extension/src/render/consolidate_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper; `OverridePool`, `VolumePool` buffer accessors.
- Produces: unchanged `initialize`, `teardown`, `is_valid`, `set_field_context`, `run`.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; `bool is_valid() const { return program_.pipeline.is_valid(); }`; replace `RID shader_, pipeline_, uset_, ops_, jobs_;` with:

```cpp
	gpu::Group group_;
	gpu::Program program_;
	RID set_, ops_, jobs_;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/consolidate_pass.cpp`, remove the includes of `project_settings`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `render/shader_loader.h`, and the anonymous-namespace `storage`/`free_if_valid` (keep `zeroed` and the stride constants). In `initialize`, wrap the four buffer creations in `group_.add(gpu::Kind::Buffer, …)`:

```cpp
	ops_ = group_.add(gpu::Kind::Buffer,
			rd_->storage_buffer_create(ve::kMaxRegionOps * 32, zeroed(ve::kMaxRegionOps * 32)));
	jobs_ = group_.add(gpu::Kind::Buffer, rd_->storage_buffer_create(static_cast<uint32_t>(max_bricks_) * 32,
			zeroed(static_cast<int64_t>(max_bricks_) * 32)));
```
and the same for `staging_sdf_` and `staging_mat_` (keep the comment above them). Replace everything from `ProjectSettings *ps = …` to the end of `initialize` with:

```cpp
	program_ = gpu::compile_compute(rd_, group_, "ConsolidatePass", "brick_consolidate.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	set_ = gpu::uniform_set(rd_, group_, program_.shader, 0, {
			gpu::storage(0, pool_->sdf_buffer()),
			gpu::storage(1, pool_->mat_buffer()),
			gpu::storage(3, staging_sdf_),
			gpu::storage(4, staging_mat_),
			gpu::storage(5, jobs_),
			gpu::storage(6, pool_->tables()),
			gpu::storage(7, pool_->region_table_map()),
			gpu::storage(8, ops_),
			gpu::storage(9, volumes->sdf_buffer()),
			gpu::storage(10, volumes->mat_buffer())});
	if (!set_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}
```

Replace `teardown` with:

```cpp
void ConsolidatePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	set_ = ops_ = jobs_ = staging_sdf_ = staging_mat_ = RID();
	rd_ = nullptr;
	pool_ = nullptr;
	max_bricks_ = 0;
}
```

Renames in `run()`: `pipeline_` → `program_.pipeline`, `uset_` → `set_`.

- [ ] **Step 3: No leftovers**

```bash
rg -n "pipeline_|uset_|shader_\b|free_if_valid|Ref<RDUniform>|shader_compile" extension/src/render/consolidate_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **consolidate**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/consolidate_pass.h extension/src/render/consolidate_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(consolidate): lifetime through the pass helper"
```

---

### Task 29: Migrate `IslandExtractPass`

`initialize()` returns `false` without tearing down when no override pool was installed; that stays.

**Files:**
- Modify: `extension/src/render/island_extract_pass.h`, `extension/src/render/island_extract_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged `initialize`, `teardown`, `is_valid`, `overrides`, `set_override_pool`, `set_field_context`, `extract`.

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; `bool is_valid() const { return program_.pipeline.is_valid(); }`; replace `RID shader_, pipeline_, uset_;` with:

```cpp
	gpu::Group group_;
	gpu::Program program_;
	RID set_;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/island_extract_pass.cpp`, remove the includes of `project_settings`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `render/shader_loader.h` and the anonymous-namespace `storage`/`free_if_valid` (keep `zeroed`). In `initialize`, wrap the four buffer creations (`out_`, `boxes_`, `counts_`, `ops_`) in `group_.add(gpu::Kind::Buffer, …)`. Replace everything from `// Same loader path as MeshPass::build…` to the end of `initialize` with:

```cpp
	program_ = gpu::compile_compute(rd, group_, "IslandExtractPass", "island_extract.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	// The extraction pass shares the worker's override mirror; its owner installs the
	// pointer before initialize so the uniform set never changes identity.
	if (!overrides_) return false;
	set_ = gpu::uniform_set(rd, group_, program_.shader, 0, {
			gpu::storage(0, out_),
			gpu::storage(1, ops_),
			gpu::storage(2, volumes->sdf_buffer()),
			gpu::storage(3, volumes->mat_buffer()),
			gpu::storage(4, boxes_),
			gpu::storage(5, counts_),
			gpu::storage(6, overrides_->sdf_buffer()),
			gpu::storage(7, overrides_->mat_buffer()),
			gpu::storage(8, overrides_->tables()),
			gpu::storage(9, overrides_->region_table_map())});
	if (!set_.is_valid()) {
		UtilityFunctions::printerr("IslandExtractPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}
```

Replace `teardown` with:

```cpp
void IslandExtractPass::teardown() {
	if (rd_) {
		gpu::RdDevice device{rd_};
		group_.release(device);
	}
	program_ = gpu::Program();
	set_ = out_ = boxes_ = counts_ = ops_ = RID();
	rd_ = nullptr;
}
```

Renames in `extract()`: `pipeline_` → `program_.pipeline`, `uset_` → `set_`.

- [ ] **Step 3: No leftovers**

```bash
rg -n "pipeline_|uset_|shader_\b|free_if_valid|Ref<RDUniform>|shader_compile" extension/src/render/island_extract_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **island_extract**.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/island_extract_pass.h extension/src/render/island_extract_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(island_extract): lifetime through the pass helper"
```

---

### Task 30: Migrate `MeshPass`

The volume and override pools are torn down after the group: their buffers were bound only by `field_set_`, which the group has already freed.

**Files:**
- Modify: `extension/src/render/mesh_pass.h`, `extension/src/render/mesh_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged public API including `field_shader()` (used by `mesh_service.cpp:537` to build the worker field context).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; `bool is_valid() const { return field_program_.pipeline.is_valid(); }`; `RID field_shader() const { return field_program_.shader; }`; delete the private `bool build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline);`; replace

```cpp
	RID field_shader_, field_pipeline_, field_uset_;
```
and the two `RID cells_shader_…` / `RID quads_shader_…` lines with:

```cpp
	gpu::Group group_;
	gpu::Program field_program_, cells_program_, quads_program_;
	RID field_set_, cells_set_, quads_set_;
```
(keep any comment line that sat between them).

- [ ] **Step 2: Implementation**

In `extension/src/render/mesh_pass.cpp`, remove the includes of `project_settings`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `render/shader_loader.h`, the anonymous-namespace `storage`, `image`, `free_if_valid` helpers, and `MeshPass::build`. In `initialize`:

- `lattice_ = rd->texture_create(f, v, TypedArray<PackedByteArray>());` → `lattice_ = group_.add(gpu::Kind::Texture, rd->texture_create(f, v, TypedArray<PackedByteArray>()));`
- wrap each of `cells_`, `verts_`, `tris_`, `counts_`, `ops_` creations in `group_.add(gpu::Kind::Buffer, …)`.
- replace from `if (!build(rd, "res://shaders/mesh_field.comp.glsl", …` to the end of `initialize` with:

```cpp
	field_program_ = gpu::compile_compute(rd, group_, "MeshPass", "mesh_field.comp.glsl");
	if (!field_program_.valid()) {
		teardown();
		return false;
	}
	field_set_ = gpu::uniform_set(rd, group_, field_program_.shader, 0, {
			gpu::image(0, lattice_),
			gpu::storage(1, ops_),
			gpu::storage(2, volumes_.sdf_buffer()),
			gpu::storage(3, volumes_.mat_buffer()),
			gpu::storage(4, overrides_.sdf_buffer()),
			gpu::storage(5, overrides_.mat_buffer()),
			gpu::storage(6, overrides_.tables()),
			gpu::storage(7, overrides_.region_table_map())});
	if (!field_set_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: uniform set creation failed");
		teardown();
		return false;
	}

	cells_program_ = gpu::compile_compute(rd, group_, "MeshPass", "mesh_cells.comp.glsl");
	quads_program_ = gpu::compile_compute(rd, group_, "MeshPass", "mesh_quads.comp.glsl");
	if (!cells_program_.valid() || !quads_program_.valid()) {
		teardown();
		return false;
	}
	cells_set_ = gpu::uniform_set(rd, group_, cells_program_.shader, 0, {
			gpu::image(0, lattice_), gpu::storage(1, cells_), gpu::storage(2, verts_),
			gpu::storage(3, counts_)});
	quads_set_ = gpu::uniform_set(rd, group_, quads_program_.shader, 0, {
			gpu::image(0, lattice_), gpu::storage(1, cells_), gpu::storage(2, tris_),
			gpu::storage(3, counts_)});
	if (!cells_set_.is_valid() || !quads_set_.is_valid()) {
		UtilityFunctions::printerr("MeshPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}
```

Replace `teardown` with:

```cpp
void MeshPass::teardown() {
	if (!rd_) return;
	if (in_flight_) {
		rd_->sync();
		in_flight_ = false;
		batch_.clear();
	}
	// Sets, pipelines and shaders first, then this pass's lattice and buffers. The pools'
	// buffers were bound only by field_set_, which is gone by the time they are.
	gpu::RdDevice device{rd_};
	group_.release(device);
	volumes_.teardown();
	overrides_.teardown();
	field_program_ = cells_program_ = quads_program_ = gpu::Program();
	field_set_ = cells_set_ = quads_set_ = RID();
	lattice_ = cells_ = verts_ = tris_ = counts_ = ops_ = RID();
	rd_ = nullptr;
}
```

Renames in the `record_*`/`run_field_sync`/`push` functions:

| Old | New |
|---|---|
| `field_pipeline_` | `field_program_.pipeline` |
| `field_uset_` | `field_set_` |
| `field_shader_` | `field_program_.shader` |
| `cells_pipeline_` | `cells_program_.pipeline` |
| `cells_uset_` | `cells_set_` |
| `quads_pipeline_` | `quads_program_.pipeline` |
| `quads_uset_` | `quads_set_` |

- [ ] **Step 3: No leftovers**

```bash
rg -n "_pipeline_|_uset_|_shader_|free_if_valid|MeshPass::build|Ref<RDUniform>" extension/src/render/mesh_pass.{h,cpp}
```
Expected: no output.

- [ ] **Step 4: Pass gate** for **mesh**; also run `res://tests/test_consolidation.gd,res://tests/test_collider_stream.gd`.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/mesh_pass.h extension/src/render/mesh_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(mesh): lifetime through the pass helper"
```

---

### Task 31: Migrate `LodBuildPass`

**Files:**
- Modify: `extension/src/render/lod_build_pass.h`, `extension/src/render/lod_build_pass.cpp`

**Interfaces:**
- Consumes: Task 8's helper.
- Produces: unchanged public API including `field_shader()` (used by `debug/hooks_lod.cpp:646`).

- [ ] **Step 1: Header**

Add `#include "render/gpu/gpu.h"`; `bool is_valid() const { return field_program_.pipeline.is_valid(); }`; `RID field_shader() const { return field_program_.shader; }`; delete `bool build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline);`; replace the four `RID field_shader_…`, `RID reduce_shader_…`, `RID frac_shader_…`, `RID quads_shader_…` lines with:

```cpp
	gpu::Group group_;
	gpu::Program field_program_, reduce_program_, frac_program_, quads_program_;
	RID field_set_, reduce_set_, frac_set_, quads_set_;
```

- [ ] **Step 2: Implementation**

In `extension/src/render/lod_build_pass.cpp`, remove the includes of `project_settings`, `rd_shader_source`, `rd_shader_spirv`, `rd_uniform`, `render/shader_loader.h`, the anonymous-namespace `storage`, `image`, `free_if_valid` helpers, and `LodBuildPass::build`. In `initialize`:

- in the `make_3d` lambda, `*rid = rd->texture_create(f, v, TypedArray<PackedByteArray>());` → `*rid = group_.add(gpu::Kind::Texture, rd->texture_create(f, v, TypedArray<PackedByteArray>()));`
- wrap each of `frac_`, `quads_`, `normals_`, `counts_`, `ops_` creations in `group_.add(gpu::Kind::Buffer, …)`.
- replace from `if (!build(rd, "res://shaders/lod_field.comp.glsl", …` to the end of `initialize` with:

```cpp
	field_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_field.comp.glsl");
	if (!field_program_.valid()) {
		teardown();
		return false;
	}
	field_set_ = gpu::uniform_set(rd, group_, field_program_.shader, 0, {
			gpu::image(0, fine_sdf_), gpu::image(1, fine_mat_), gpu::storage(2, ops_),
			gpu::storage(3, volumes_.sdf_buffer()), gpu::storage(4, volumes_.mat_buffer()),
			gpu::storage(5, overrides_->sdf_buffer()), gpu::storage(6, overrides_->mat_buffer()),
			gpu::storage(7, overrides_->tables()), gpu::storage(8, overrides_->region_table_map())});
	if (!field_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: field uniform set creation failed");
		teardown();
		return false;
	}

	reduce_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_reduce.comp.glsl");
	if (!reduce_program_.valid()) {
		teardown();
		return false;
	}
	reduce_set_ = gpu::uniform_set(rd, group_, reduce_program_.shader, 0, {
			gpu::image(0, fine_sdf_), gpu::image(1, fine_mat_), gpu::image(2, lat_sdf_),
			gpu::image(3, lat_mat_)});
	if (!reduce_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: reduce uniform set creation failed");
		teardown();
		return false;
	}

	frac_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_frac.comp.glsl");
	if (!frac_program_.valid()) {
		teardown();
		return false;
	}
	frac_set_ = gpu::uniform_set(rd, group_, frac_program_.shader, 0,
			{gpu::image(0, lat_sdf_), gpu::storage(1, frac_)});
	if (!frac_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: frac uniform set creation failed");
		teardown();
		return false;
	}

	quads_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_quads.comp.glsl");
	if (!quads_program_.valid()) {
		teardown();
		return false;
	}
	quads_set_ = gpu::uniform_set(rd, group_, quads_program_.shader, 0, {
			gpu::image(0, lat_sdf_), gpu::image(1, lat_mat_), gpu::storage(2, frac_),
			gpu::storage(3, quads_), gpu::storage(4, counts_), gpu::storage(5, normals_)});
	if (!quads_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: quads uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}
```

Replace `teardown` with:

```cpp
void LodBuildPass::teardown() {
	if (!rd_) return;
	if (in_flight_) {
		rd_->sync();
		in_flight_ = false;
		batch_.clear();
	}
	// Sets, pipelines and shaders first, then this pass's lattices and buffers. The pools'
	// buffers were bound only by field_set_, which is gone by the time they are.
	gpu::RdDevice device{rd_};
	group_.release(device);
	volumes_.teardown();
	if (overrides_ == &owned_overrides_) owned_overrides_.teardown();
	overrides_ = nullptr;
	field_program_ = reduce_program_ = frac_program_ = quads_program_ = gpu::Program();
	field_set_ = reduce_set_ = frac_set_ = quads_set_ = RID();
	fine_sdf_ = fine_mat_ = lat_sdf_ = lat_mat_ = RID();
	frac_ = quads_ = normals_ = counts_ = ops_ = RID();
	rd_ = nullptr;
}
```

Renames in the `record_*` functions:

| Old | New |
|---|---|
| `field_pipeline_` / `field_uset_` / `field_shader_` | `field_program_.pipeline` / `field_set_` / `field_program_.shader` |
| `reduce_pipeline_` / `reduce_uset_` | `reduce_program_.pipeline` / `reduce_set_` |
| `frac_pipeline_` / `frac_uset_` | `frac_program_.pipeline` / `frac_set_` |
| `quads_pipeline_` / `quads_uset_` | `quads_program_.pipeline` / `quads_set_` |

- [ ] **Step 3: No leftovers**

```bash
rg -n "_pipeline_|_uset_|_shader_|free_if_valid|LodBuildPass::build|Ref<RDUniform>" extension/src/render/lod_build_pass.{h,cpp}
rg -n "shader_compile_spirv_from_source" extension/src -g '*.cpp'
```
Expected: the first prints nothing; the second prints exactly one line, in `extension/src/render/gpu/gpu.cpp`.

- [ ] **Step 4: Pass gate** for **lod_build**; also run `res://tests/test_lod_stream.gd,res://tests/test_world_store_contract.gd`.

- [ ] **Step 5: Milestone (a) regression check**

Run the full suite and compare with the baseline:

```bash
./gdunit_tests.sh 2>&1 | tee .superpowers/sdd/2026-09-15-pass-anatomy/milestone-a.log | tail -5
grep -c " leaked" .superpowers/sdd/2026-09-15-pass-anatomy/milestone-a.log
latest=$(ls -d reports/report_* | sort -V | tail -1); python3 .superpowers/sdd/2026-09-15-pass-anatomy/failures.py "$latest/results.xml"
```
Expected: the failing set equals Task 1's (plus S7/S4 cases now passing); leak count ≤ Task 1's. Record under `### Milestone (a)` in the evidence log.

- [ ] **Step 6: Commit**

```bash
git add extension/src/render/lod_build_pass.h extension/src/render/lod_build_pass.cpp docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "refactor(lod_build): lifetime through the pass helper; one shader compile site remains"
```

---

### Task 32: The layout emitter, every block table, and `shaders/generated/blocks.glslh`

Pure code plus one committed generated file that nothing includes yet.

**Files:**
- Create: `extension/src/gpu_layout/layout.h`, `extension/src/gpu_layout/layout.cpp`
- Create: `extension/src/gpu_layout/blocks.h`, `extension/src/gpu_layout/blocks.cpp`
- Create: `shaders/generated/blocks.glslh` (generated)
- Create: `extension/tests/test_gpu_layout.cpp`, `extension/tests/test_generated_glsl.cpp`
- Modify: `extension/SConstruct` (`pure_sources`)

**Interfaces:**
- Consumes: `ve::CameraParams` (`render/camera_params.h`), `ve::GrassParams` (`grass/grass_layout.h`), `ve::kSunCascades` (`shade/sun_cascades.h`).
- Produces:
  - `ve::layout::FieldType { Vec4, IVec4, UVec4, Mat4 }`, `ve::layout::Field { const char *name; FieldType type; int count; size_t offset; }`, `ve::layout::Block { const char *macro; size_t size; const Field *fields; int field_count; }`
  - `std::string ve::layout::check_block(const Block &)` ("" when valid), `std::string ve::layout::emit_block(const Block &)`
  - Macros `VE_LAYOUT_FIELD(S, member, type, count)`, `VE_LAYOUT_BLOCK(S, macro, table)`
  - Structs in `ve`: `SsaoPush`, `ContactShadowPush`, `OutlinePush`, `SsrTracePush`, `SsrApplyPush`, `SsgiPush`, `HizPush`, `DeferredPush`, `SunCascadeBlock`, `BeautyCamBlock`, `SunLightBlock`, `LodRasterPush`, `CompositePush`, `GrassRasterPush`, `SunShadowPush`, `EditsBlock`, `LodCullPush`, `GrassRegionBlock`, `DownsamplePush`, `BrickGenPush`, `ConsolidatePush`, `BrickMarkPush`, `RegionFreePush`, `DispatchArgsPush`, `IslandExtractPush`, `MeshPush`, `LodBuildPush` (field names exactly as below)
  - `ve::layout::kBlocks[]` (29 entries, including `CameraParams` → `CAMERA_PARAMS_FIELDS` and `GrassParams` → `GRASS_PARAMS_FIELDS`), `std::string ve::layout::blocks_glsl()`
  - Test helper `check_generated(rel_path, expected)` in `test_generated_glsl.cpp`, reused by Tasks 37, 39, 40.

- [ ] **Step 1: Failing tests**

`extension/tests/test_gpu_layout.cpp`:

```cpp
#include <doctest/doctest.h>
#include "gpu_layout/blocks.h"
#include <set>
#include <string>

using ve::layout::Block;
using ve::layout::Field;
using ve::layout::FieldType;

TEST_CASE("every block table matches its C++ struct") {
	for (const Block &b : ve::layout::kBlocks) {
		const std::string why = ve::layout::check_block(b);
		CHECK_MESSAGE(why.empty(), why);
	}
}

TEST_CASE("a table that misplaces or omits a field is refused") {
	const Field gap[] = {{"a", FieldType::Vec4, 0, 0}, {"b", FieldType::Vec4, 0, 20}};
	const std::string moved = ve::layout::check_block({"GAP", 36, gap, 2});
	CHECK(moved.find("GAP: b") != std::string::npos);
	const Field short_table[] = {{"a", FieldType::Vec4, 0, 0}};
	const std::string missing = ve::layout::check_block({"SHORT", 32, short_table, 1});
	CHECK(missing.find("cover 16 bytes") != std::string::npos);
	const Field arrays[] = {{"m", FieldType::Mat4, 3, 0}, {"v", FieldType::Vec4, 2, 192}};
	CHECK(ve::layout::check_block({"ARRAYS", 224, arrays, 2}).empty());
}

TEST_CASE("block macros are unique and emit one declaration per field") {
	std::set<std::string> names;
	for (const Block &b : ve::layout::kBlocks) CHECK_MESSAGE(names.insert(b.macro).second, b.macro);
	CHECK(names.size() == 29);
	const Field f[] = {{"view_proj", FieldType::Mat4, 3, 0}, {"splits", FieldType::UVec4, 0, 192}};
	CHECK(ve::layout::emit_block({"X_FIELDS", 208, f, 2}) ==
			"#define X_FIELDS \\\n\tmat4 view_proj[3]; \\\n\tuvec4 splits;\n");
}
```

`extension/tests/test_generated_glsl.cpp`:

```cpp
// Every committed file under shaders/generated/ is compared byte for byte with the C++ that
// produces it -- the material_table.glslh pattern. Regenerate after an intentional change:
//   cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests
#include <doctest/doctest.h>
#include "gpu_layout/blocks.h"
#include "render/shader_loader.h"
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string root() { return std::string(VE_REPO_ROOT); }

void check_generated(const std::string &rel, const std::string &expected) {
	const std::string path = root() + "/" + rel;
	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		std::ofstream out(path, std::ios::binary);
		REQUIRE(out.good());
		out << expected;
	}
	std::ifstream f(path, std::ios::binary);
	REQUIRE_MESSAGE(f.good(), "cannot open ", path);
	const std::string on_disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	CHECK_MESSAGE(on_disk == expected, rel,
			" is stale. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests");
	// The shader loader expands include directives anywhere in a line, so generated text must
	// hold none, end in a newline, and come back from the loader unchanged.
	CHECK(expected.find("#include \"") == std::string::npos);
	REQUIRE(!expected.empty());
	CHECK(expected.back() == '\n');
	std::string err;
	CHECK(ve::load_shader_source(path, root() + "/shaders", &err) == expected);
	CHECK(err.empty());
}

} // namespace

TEST_CASE("generated: shaders/generated/blocks.glslh") {
	check_generated("shaders/generated/blocks.glslh", ve::layout::blocks_glsl());
}
```

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: `fatal error: gpu_layout/blocks.h: No such file or directory`.

- [ ] **Step 2: Add the module to the native build**

`extension/SConstruct`, the `pure_sources = (…)` expression: append `+ Glob("src/gpu_layout/*.cpp")` inside the parentheses after `Glob("src/grass/*.cpp")`.

- [ ] **Step 3: The emitter**

`extension/src/gpu_layout/layout.h`:

```cpp
#pragma once
// Describes a push-constant or uniform block once, in C++, and writes its GLSL field list.
// Only 16-byte-aligned GLSL types are allowed (vec4, ivec4, uvec4, mat4 and arrays of them):
// for those, std140 and std430 put every field at the same offset, so one rule covers push
// constants and uniform buffers and check_block can prove a table against offsetof.
#include <cstddef>
#include <string>

namespace ve::layout {

enum class FieldType { Vec4, IVec4, UVec4, Mat4 };

struct Field {
	const char *name;
	FieldType type;
	int count;     // 0 = one value; N = an array of N
	size_t offset; // offsetof in the C++ struct
};

struct Block {
	const char *macro; // GLSL field-list macro, e.g. "SSAO_PUSH_FIELDS"
	size_t size;       // sizeof the C++ struct
	const Field *fields;
	int field_count;
};

// "" when each field starts where the previous one ends and the fields cover `size` exactly;
// otherwise one sentence naming the block and the first disagreement.
std::string check_block(const Block &block);

// "#define <macro> \\\n\t<type> <name>[N]; \\\n ... \t<type> <name>;\n"
std::string emit_block(const Block &block);

} // namespace ve::layout

#define VE_LAYOUT_FIELD(S, member, type, count) \
	::ve::layout::Field{#member, ::ve::layout::FieldType::type, count, offsetof(S, member)}

#define VE_LAYOUT_BLOCK(S, macro, table) \
	::ve::layout::Block{macro, sizeof(S), table, static_cast<int>(sizeof(table) / sizeof(table[0]))}
```

`extension/src/gpu_layout/layout.cpp`:

```cpp
#include "gpu_layout/layout.h"

namespace ve::layout {

namespace {

size_t bytes(FieldType type) {
	return type == FieldType::Mat4 ? 64 : 16;
}

const char *glsl_type(FieldType type) {
	switch (type) {
		case FieldType::Vec4: return "vec4";
		case FieldType::IVec4: return "ivec4";
		case FieldType::UVec4: return "uvec4";
		case FieldType::Mat4: return "mat4";
	}
	return "vec4";
}

} // namespace

std::string check_block(const Block &block) {
	size_t at = 0;
	for (int i = 0; i < block.field_count; i++) {
		const Field &f = block.fields[i];
		if (f.offset != at)
			return std::string(block.macro) + ": " + f.name + " is at offset " +
					std::to_string(f.offset) + " in C++ but at " + std::to_string(at) + " in GLSL";
		at += bytes(f.type) * static_cast<size_t>(f.count > 0 ? f.count : 1);
	}
	if (at != block.size)
		return std::string(block.macro) + ": the fields cover " + std::to_string(at) +
				" bytes but sizeof is " + std::to_string(block.size);
	return "";
}

std::string emit_block(const Block &block) {
	std::string out = std::string("#define ") + block.macro + " \\\n";
	for (int i = 0; i < block.field_count; i++) {
		const Field &f = block.fields[i];
		out += std::string("\t") + glsl_type(f.type) + " " + f.name;
		if (f.count > 0) out += "[" + std::to_string(f.count) + "]";
		out += i + 1 < block.field_count ? "; \\\n" : ";\n";
	}
	return out;
}

} // namespace ve::layout
```

- [ ] **Step 4: The blocks**

`extension/src/gpu_layout/blocks.h`:

```cpp
#pragma once
// Every push-constant and uniform-buffer layout a C++ pass fills and a shader reads: one
// trivially copyable struct per block and a field table beside it. blocks_glsl() writes
// shaders/generated/blocks.glslh from kBlocks; extension/tests/test_gpu_layout.cpp proves each
// table against offsetof and sizeof, so a struct and its shader cannot disagree.
//
// Comments on fields are the only documentation of what a slot carries; the generated GLSL
// has no comments of its own.
#include "gpu_layout/layout.h"
#include "grass/grass_layout.h"
#include "render/camera_params.h"
#include "shade/sun_cascades.h"
#include <cstdint>
#include <string>

namespace ve {

struct SsaoPush {
	int32_t dims[4];  // xy = target size, z = march steps per direction, w = sweep directions
	float params[4];  // x = world-space radius, y = strength, zw = unused
};

struct ContactShadowPush {
	int32_t dims[4];  // xy = target size, z = mode (0 march, 1 apply), w = steps
	float params[4];  // x = reach, y = strength, z = surface bias/hit thickness (metres)
};

struct OutlinePush {
	int32_t dims[4];  // xy = full size, z = have normal-roughness
	float params[4];  // relative depth threshold, normal threshold, darken, unused
};

struct SsrTracePush {
	int32_t dims[4];  // xy = half size, z = steps, w = have normal-roughness
	float params[4];  // reach, start bias, thickness (metres), strength
};

struct SsrApplyPush {
	int32_t dims[4];  // xy = full size
};

struct SsgiPush {
	float prev_view_proj[16];
	int32_t dims[4];    // xy = target size, z = taps, w = have history
	float params[4];    // x = bounce radius (m), y = temporal history weight, z = bounce strength
	float emissive[4];  // x = emissive radius (m), y = emissive strength, zw unused
	int32_t stage[4];   // x = 0 gather into out_raw, 1 resolve out_raw into out_ssgi
};

struct HizPush {
	int32_t dims[4];   // xy = destination size, zw = source size
	int32_t flags[4];  // x = 1 when the source is the scene depth (level 0), else 0
};

struct DeferredPush {
	float inv_view_proj[16];
	float cam[4];       // xyz = camera position
	float sky[4];       // xyz = ambient
	uint32_t flags[4];  // x = beauty flags, y = probe mode
};

struct SunCascadeBlock {
	float view_proj[kSunCascades][16];
	// Per cascade: x = one shadow texel in world metres, y = light-space depth range in the same
	// metres; zw on cascade 0 carries the LoD fade band (fade_start, fade_end).
	float params[kSunCascades][4];
	float splits[4];  // xyz = the cascade radii; w = the count in use (1 when the radius collapsed)
};

struct BeautyCamBlock {
	float view_proj[16];
	float inv_view_proj[16];
	float cam[4];     // xyz = camera position, w = unused
	float screen[4];  // xy = full-resolution size, zw = 1 / size
};

struct SunLightBlock {
	float dir[4];  // xyz = normalized, TOWARD the sun; w unused
	float rgb[4];  // xyz = linear colour * energy; w unused
};

struct LodRasterPush {
	float view_proj[16];
	float cam[4];   // xyz = camera position, w = fade start
	float fade[4];  // x = fade end, yzw unused
};

struct CompositePush {
	float view_proj[16];
	float cam[4];         // xyz = camera position, w = fade start
	float fade[4];        // x = fade end, yzw = camera forward
	float right_tanx[4];  // xyz = camera right, w = tan(fov_x / 2)
	float up_tany[4];     // xyz = camera up,    w = tan(fov_y / 2)
};

struct GrassRasterPush {
	float view_proj[16];
	float cam[4];  // xyz = camera position, w unused
};

struct SunShadowPush {
	float sun_view_proj[16];
};

struct EditsBlock {
	float center[4];  // xyz = brush centre
	float params[4];  // x = radius, y = type, z = material, w = 1 while a brush is shown
};

struct LodCullPush {
	float view_proj[16];
	int32_t params[4];  // x = page count, y = hiz size, z = hiz mips, w = unused
};

struct GrassRegionBlock {
	int32_t dims[4];
	int32_t region_origin[4];
	int32_t atlas_bricks[4];
};

struct DownsamplePush {
	int32_t dims[4];  // xy = destination (half) size
};

struct BrickGenPush {
	int32_t atlas_bricks[4];
};

struct ConsolidatePush {
	int32_t params[4];  // x = brick count, y = region slot, z = op count, w = region table
};

struct BrickMarkPush {
	int32_t region[4];  // xyz = global region coord (may be negative), w = region slot
	int32_t lo[4];      // inclusive global brick coord of the range to scan
	int32_t hi[4];      // inclusive
	int32_t cfg[4];     // x = op count, y = phase (0 release, 1 allocate), z = max jobs, w = force
};

struct RegionFreePush {
	int32_t cfg[4];  // x = region slot
};

struct DispatchArgsPush {
	int32_t pad[4];
};

struct IslandExtractPush {
	float origin_voxel[4];  // xyz = origin, w = voxel size
	int32_t params[4];      // x = dim, y = op count, z = box count, w = override table
};

struct MeshPush {
	int32_t chunk[4];          // xyz = chunk coordinates (job identity only), w = job index
	int32_t params[4];         // x = op count, y = max verts per job, z = max tris per job, w = lattice dim
	float grid[4];             // xyz = the chunk's world origin, w = cell size in metres
	int32_t override_data[4];  // x = override table, y = region slot
};

struct LodBuildPush {
	int32_t job[4];            // xyz = chunk coordinates, w = job index in this batch
	int32_t params[4];         // x = op count, y = max quads per job, z = level, w = unused
	float grid[4];             // xyz = the chunk's world origin, w = the level's cell size
	int32_t override_data[4];  // x = override table, y = region slot
};

} // namespace ve

namespace ve::layout {

inline constexpr Field kSsaoPushFields[] = {
	VE_LAYOUT_FIELD(SsaoPush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(SsaoPush, params, Vec4, 0),
};
inline constexpr Field kContactShadowPushFields[] = {
	VE_LAYOUT_FIELD(ContactShadowPush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(ContactShadowPush, params, Vec4, 0),
};
inline constexpr Field kOutlinePushFields[] = {
	VE_LAYOUT_FIELD(OutlinePush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(OutlinePush, params, Vec4, 0),
};
inline constexpr Field kSsrTracePushFields[] = {
	VE_LAYOUT_FIELD(SsrTracePush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(SsrTracePush, params, Vec4, 0),
};
inline constexpr Field kSsrApplyPushFields[] = {
	VE_LAYOUT_FIELD(SsrApplyPush, dims, IVec4, 0),
};
inline constexpr Field kSsgiPushFields[] = {
	VE_LAYOUT_FIELD(SsgiPush, prev_view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(SsgiPush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(SsgiPush, params, Vec4, 0),
	VE_LAYOUT_FIELD(SsgiPush, emissive, Vec4, 0),
	VE_LAYOUT_FIELD(SsgiPush, stage, IVec4, 0),
};
inline constexpr Field kHizPushFields[] = {
	VE_LAYOUT_FIELD(HizPush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(HizPush, flags, IVec4, 0),
};
inline constexpr Field kDeferredPushFields[] = {
	VE_LAYOUT_FIELD(DeferredPush, inv_view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(DeferredPush, cam, Vec4, 0),
	VE_LAYOUT_FIELD(DeferredPush, sky, Vec4, 0),
	VE_LAYOUT_FIELD(DeferredPush, flags, UVec4, 0),
};
inline constexpr Field kSunCascadeBlockFields[] = {
	VE_LAYOUT_FIELD(SunCascadeBlock, view_proj, Mat4, kSunCascades),
	VE_LAYOUT_FIELD(SunCascadeBlock, params, Vec4, kSunCascades),
	VE_LAYOUT_FIELD(SunCascadeBlock, splits, Vec4, 0),
};
inline constexpr Field kBeautyCamBlockFields[] = {
	VE_LAYOUT_FIELD(BeautyCamBlock, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(BeautyCamBlock, inv_view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(BeautyCamBlock, cam, Vec4, 0),
	VE_LAYOUT_FIELD(BeautyCamBlock, screen, Vec4, 0),
};
inline constexpr Field kSunLightBlockFields[] = {
	VE_LAYOUT_FIELD(SunLightBlock, dir, Vec4, 0),
	VE_LAYOUT_FIELD(SunLightBlock, rgb, Vec4, 0),
};
inline constexpr Field kLodRasterPushFields[] = {
	VE_LAYOUT_FIELD(LodRasterPush, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(LodRasterPush, cam, Vec4, 0),
	VE_LAYOUT_FIELD(LodRasterPush, fade, Vec4, 0),
};
inline constexpr Field kCompositePushFields[] = {
	VE_LAYOUT_FIELD(CompositePush, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(CompositePush, cam, Vec4, 0),
	VE_LAYOUT_FIELD(CompositePush, fade, Vec4, 0),
	VE_LAYOUT_FIELD(CompositePush, right_tanx, Vec4, 0),
	VE_LAYOUT_FIELD(CompositePush, up_tany, Vec4, 0),
};
inline constexpr Field kGrassRasterPushFields[] = {
	VE_LAYOUT_FIELD(GrassRasterPush, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(GrassRasterPush, cam, Vec4, 0),
};
inline constexpr Field kSunShadowPushFields[] = {
	VE_LAYOUT_FIELD(SunShadowPush, sun_view_proj, Mat4, 0),
};
inline constexpr Field kCameraParamsFields[] = {
	VE_LAYOUT_FIELD(CameraParams, cam_pos, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, cam_right, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, cam_up, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, cam_fwd, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, params, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, dims, IVec4, 0),
	VE_LAYOUT_FIELD(CameraParams, region_origin, IVec4, 0),
	VE_LAYOUT_FIELD(CameraParams, atlas_bricks, IVec4, 0),
};
inline constexpr Field kEditsBlockFields[] = {
	VE_LAYOUT_FIELD(EditsBlock, center, Vec4, 0),
	VE_LAYOUT_FIELD(EditsBlock, params, Vec4, 0),
};
inline constexpr Field kLodCullPushFields[] = {
	VE_LAYOUT_FIELD(LodCullPush, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(LodCullPush, params, IVec4, 0),
};
inline constexpr Field kGrassParamsFields[] = {
	VE_LAYOUT_FIELD(GrassParams, cam, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, planes, Vec4, 6),
	VE_LAYOUT_FIELD(GrassParams, brick_min, IVec4, 0),
	VE_LAYOUT_FIELD(GrassParams, brick_dim, IVec4, 0),
	VE_LAYOUT_FIELD(GrassParams, ring_end, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, ring_blades, IVec4, 0),
	VE_LAYOUT_FIELD(GrassParams, blade, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, wind, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, style, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, shape, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, limits, IVec4, 0),
};
inline constexpr Field kGrassRegionBlockFields[] = {
	VE_LAYOUT_FIELD(GrassRegionBlock, dims, IVec4, 0),
	VE_LAYOUT_FIELD(GrassRegionBlock, region_origin, IVec4, 0),
	VE_LAYOUT_FIELD(GrassRegionBlock, atlas_bricks, IVec4, 0),
};
inline constexpr Field kDownsamplePushFields[] = {
	VE_LAYOUT_FIELD(DownsamplePush, dims, IVec4, 0),
};
inline constexpr Field kBrickGenPushFields[] = {
	VE_LAYOUT_FIELD(BrickGenPush, atlas_bricks, IVec4, 0),
};
inline constexpr Field kConsolidatePushFields[] = {
	VE_LAYOUT_FIELD(ConsolidatePush, params, IVec4, 0),
};
inline constexpr Field kBrickMarkPushFields[] = {
	VE_LAYOUT_FIELD(BrickMarkPush, region, IVec4, 0),
	VE_LAYOUT_FIELD(BrickMarkPush, lo, IVec4, 0),
	VE_LAYOUT_FIELD(BrickMarkPush, hi, IVec4, 0),
	VE_LAYOUT_FIELD(BrickMarkPush, cfg, IVec4, 0),
};
inline constexpr Field kRegionFreePushFields[] = {
	VE_LAYOUT_FIELD(RegionFreePush, cfg, IVec4, 0),
};
inline constexpr Field kDispatchArgsPushFields[] = {
	VE_LAYOUT_FIELD(DispatchArgsPush, pad, IVec4, 0),
};
inline constexpr Field kIslandExtractPushFields[] = {
	VE_LAYOUT_FIELD(IslandExtractPush, origin_voxel, Vec4, 0),
	VE_LAYOUT_FIELD(IslandExtractPush, params, IVec4, 0),
};
inline constexpr Field kMeshPushFields[] = {
	VE_LAYOUT_FIELD(MeshPush, chunk, IVec4, 0),
	VE_LAYOUT_FIELD(MeshPush, params, IVec4, 0),
	VE_LAYOUT_FIELD(MeshPush, grid, Vec4, 0),
	VE_LAYOUT_FIELD(MeshPush, override_data, IVec4, 0),
};
inline constexpr Field kLodBuildPushFields[] = {
	VE_LAYOUT_FIELD(LodBuildPush, job, IVec4, 0),
	VE_LAYOUT_FIELD(LodBuildPush, params, IVec4, 0),
	VE_LAYOUT_FIELD(LodBuildPush, grid, Vec4, 0),
	VE_LAYOUT_FIELD(LodBuildPush, override_data, IVec4, 0),
};

inline constexpr Block kBlocks[] = {
	VE_LAYOUT_BLOCK(SsaoPush, "SSAO_PUSH_FIELDS", kSsaoPushFields),
	VE_LAYOUT_BLOCK(ContactShadowPush, "CONTACT_SHADOW_PUSH_FIELDS", kContactShadowPushFields),
	VE_LAYOUT_BLOCK(OutlinePush, "OUTLINE_PUSH_FIELDS", kOutlinePushFields),
	VE_LAYOUT_BLOCK(SsrTracePush, "SSR_TRACE_PUSH_FIELDS", kSsrTracePushFields),
	VE_LAYOUT_BLOCK(SsrApplyPush, "SSR_APPLY_PUSH_FIELDS", kSsrApplyPushFields),
	VE_LAYOUT_BLOCK(SsgiPush, "SSGI_PUSH_FIELDS", kSsgiPushFields),
	VE_LAYOUT_BLOCK(HizPush, "HIZ_PUSH_FIELDS", kHizPushFields),
	VE_LAYOUT_BLOCK(DeferredPush, "DEFERRED_PUSH_FIELDS", kDeferredPushFields),
	VE_LAYOUT_BLOCK(SunCascadeBlock, "SUN_CASCADE_BLOCK_FIELDS", kSunCascadeBlockFields),
	VE_LAYOUT_BLOCK(BeautyCamBlock, "BEAUTY_CAM_FIELDS", kBeautyCamBlockFields),
	VE_LAYOUT_BLOCK(SunLightBlock, "SUN_LIGHT_FIELDS", kSunLightBlockFields),
	VE_LAYOUT_BLOCK(LodRasterPush, "LOD_RASTER_PUSH_FIELDS", kLodRasterPushFields),
	VE_LAYOUT_BLOCK(CompositePush, "COMPOSITE_PUSH_FIELDS", kCompositePushFields),
	VE_LAYOUT_BLOCK(GrassRasterPush, "GRASS_RASTER_PUSH_FIELDS", kGrassRasterPushFields),
	VE_LAYOUT_BLOCK(SunShadowPush, "SUN_SHADOW_PUSH_FIELDS", kSunShadowPushFields),
	VE_LAYOUT_BLOCK(CameraParams, "CAMERA_PARAMS_FIELDS", kCameraParamsFields),
	VE_LAYOUT_BLOCK(EditsBlock, "EDITS_BLOCK_FIELDS", kEditsBlockFields),
	VE_LAYOUT_BLOCK(LodCullPush, "LOD_CULL_PUSH_FIELDS", kLodCullPushFields),
	VE_LAYOUT_BLOCK(GrassParams, "GRASS_PARAMS_FIELDS", kGrassParamsFields),
	VE_LAYOUT_BLOCK(GrassRegionBlock, "GRASS_REGION_FIELDS", kGrassRegionBlockFields),
	VE_LAYOUT_BLOCK(DownsamplePush, "DOWNSAMPLE_PUSH_FIELDS", kDownsamplePushFields),
	VE_LAYOUT_BLOCK(BrickGenPush, "BRICK_GEN_PUSH_FIELDS", kBrickGenPushFields),
	VE_LAYOUT_BLOCK(ConsolidatePush, "CONSOLIDATE_PUSH_FIELDS", kConsolidatePushFields),
	VE_LAYOUT_BLOCK(BrickMarkPush, "BRICK_MARK_PUSH_FIELDS", kBrickMarkPushFields),
	VE_LAYOUT_BLOCK(RegionFreePush, "REGION_FREE_PUSH_FIELDS", kRegionFreePushFields),
	VE_LAYOUT_BLOCK(DispatchArgsPush, "DISPATCH_ARGS_PUSH_FIELDS", kDispatchArgsPushFields),
	VE_LAYOUT_BLOCK(IslandExtractPush, "ISLAND_EXTRACT_PUSH_FIELDS", kIslandExtractPushFields),
	VE_LAYOUT_BLOCK(MeshPush, "MESH_PUSH_FIELDS", kMeshPushFields),
	VE_LAYOUT_BLOCK(LodBuildPush, "LOD_BUILD_PUSH_FIELDS", kLodBuildPushFields),
};

// The exact contents of shaders/generated/blocks.glslh.
std::string blocks_glsl();

} // namespace ve::layout
```

`extension/src/gpu_layout/blocks.cpp`:

```cpp
#include "gpu_layout/blocks.h"

namespace ve::layout {

std::string blocks_glsl() {
	std::string out =
			"// GENERATED from extension/src/gpu_layout/blocks.h by ve::layout::blocks_glsl().\n"
			"// Do not edit by hand: extension/tests/test_generated_glsl.cpp asserts this file byte\n"
			"// for byte. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests\n"
			"//\n"
			"// Each macro is the field list of one push-constant or uniform block. The shader keeps\n"
			"// layout(), set, binding and instance name around it; the C++ struct carries the\n"
			"// comments on what each field holds.\n";
	for (const Block &b : kBlocks) out += "\n" + emit_block(b);
	return out;
}

} // namespace ve::layout
```

- [ ] **Step 5: Generate and run**

```bash
mkdir -p shaders/generated
(cd extension && scons -Q test) 2>&1 | tail -3               # builds; the golden case fails: file missing
(cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests) 2>&1 | tail -3
(cd extension && scons -Q test) 2>&1 | tail -3
head -20 shaders/generated/blocks.glslh
```
Expected: the final run passes every case. `blocks.glslh` starts with the header comment then `#define SSAO_PUSH_FIELDS \`, `\tivec4 dims; \`, `\tvec4 params;`.

- [ ] **Step 6: Commit**

```bash
git add extension/src/gpu_layout/ extension/tests/test_gpu_layout.cpp extension/tests/test_generated_glsl.cpp \
	extension/SConstruct shaders/generated/blocks.glslh
git commit -m "feat: generated push/UBO field lists from C++ block tables, checked against offsetof"
```

---

### Tasks 33–36: switching blocks over

Each switch is one commit per site and changes two things together: the shader's block body becomes the macro, and the C++ fills the struct and hands `gpu::push_bytes(block)` (or its bytes to `buffer_update`) instead of indexing a `PackedByteArray`. The bytes on the wire must be identical, so the site's pinned values are unchanged.

**GLSL rule for every shader in these tasks:** insert `#include "generated/blocks.glslh"` as the line immediately after the `#version` line (for a `.glslh` header, as its first non-comment line). Never inside `#if`/`#ifdef`: the loader expands each file once, so an include swallowed by an inactive branch would hide the macros from the active one. Then replace the block body `{ … }` with `{ <MACRO> }`, keeping `layout(...)`, the block name and the instance name.

**C++ rule:** add `#include "gpu_layout/blocks.h"`; fill a value-initialised struct (`ve::X push{};`), so every slot the old code left unwritten is zero; delete the `static_assert(sizeof(float) * N == M, …)` lines.

---

### Task 33: Blocks for the frame post passes, the beauty camera and the sun light

One commit per numbered step group. Each group ends with the Pass gate for the named site and a commit `refactor(<site>): push block from the generated layout`.

**Files:**
- Modify: `extension/src/render/{ssao,contact_shadow,outline,ssr,ssgi,hiz,deferred}_pass.cpp`, `extension/src/render/beauty_camera.cpp`, `extension/src/render/sun_ubo.cpp`
- Modify: `shaders/{ssao,contact_shadow,outline,ssr,ssgi,hiz,deferred}.comp.glsl`, `shaders/beauty_camera.glslh`, `shaders/sun_light.glslh`

**Interfaces:**
- Consumes: Task 32's structs and macros; Task 8's `gpu::push_bytes`.
- Produces: no hand-offset packing left in these files.

- [ ] **Step 1: ssao**

`shaders/ssao.comp.glsl`: include line after `#version`; `layout(push_constant, std430) uniform Push { SSAO_PUSH_FIELDS } pc;`.

`extension/src/render/ssao_pass.cpp`, replace from `static_assert(sizeof(float) * 8 == 32, "ssao push block");` through the `if (!gpu::dispatch(…)) return false;` statement with:

```cpp
	const ve::SsaoPush push{{half.x, half.y, s.ssao_steps, s.ssao_directions},
			{kSsaoRadius, kSsaoStrength, 0.0f, 0.0f}};
	if (!gpu::dispatch(rd, program_.pipeline, {{set, 0}}, gpu::push_bytes(push),
				gpu::groups(half.x, 8), gpu::groups(half.y, 8)))
		return false;
```
Pass gate **ssao**; commit.

- [ ] **Step 2: contact_shadow**

`shaders/contact_shadow.comp.glsl`: include; `{ CONTACT_SHADOW_PUSH_FIELDS }`.

`extension/src/render/contact_shadow_pass.cpp`, replace from `PackedByteArray pc;` through the second `rd->compute_list_dispatch(…)` with:

```cpp
	// One voxel of surface bias: large enough to leave the receiver, but too small to bridge
	// terrain gaps.
	ve::ContactShadowPush push{{half.x, half.y, 0, s.contact_steps}, {0.6f, 0.85f, 0.05f, 0.0f}};
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, gpu::groups(half.x, 8), gpu::groups(half.y, 8), 1);
	rd->compute_list_add_barrier(list);
	push.dims[0] = size.x;
	push.dims[1] = size.y;
	push.dims[2] = 1;
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, gpu::groups(size.x, 8), gpu::groups(size.y, 8), 1);
```
Pass gate **contact_shadow**; commit.

- [ ] **Step 3: outline**

`shaders/outline.comp.glsl`: include; `{ OUTLINE_PUSH_FIELDS }`.

`extension/src/render/outline_pass.cpp`, replace from `PackedByteArray pc;` through the `return gpu::dispatch(…);` with:

```cpp
	const ve::OutlinePush push{
			{size.x, size.y, have_normal_roughness && normal_roughness.is_valid() ? 1 : 0, 0},
			{s.outline_depth_threshold, s.outline_normal_threshold, 0.35f, 0.0f}};
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, gpu::push_bytes(push),
			gpu::groups(size.x, 8), gpu::groups(size.y, 8));
```
Pass gate **outline**; commit.

- [ ] **Step 4: ssr**

`shaders/ssr.comp.glsl`: include after `#version`; replace the trace block body with `{ SSR_TRACE_PUSH_FIELDS }` and the apply block (`uniform Push { ivec4 dims; } pc;`) with `uniform Push { SSR_APPLY_PUSH_FIELDS } pc;`.

`extension/src/render/ssr_pass.cpp`, replace from `PackedByteArray trace_pc;` to the end of `render` with:

```cpp
	const ve::SsrTracePush trace{
			{half.x, half.y, s.ssr_steps, have_normal_roughness && normal_roughness.is_valid() ? 1 : 0},
			{kReachM, kStartBiasM, kThicknessM, kStrength}};
	if (!gpu::dispatch(rd, trace_.pipeline, {{trace_set, 0}}, gpu::push_bytes(trace),
				gpu::groups(half.x, 8), gpu::groups(half.y, 8)))
		return false;
	const ve::SsrApplyPush apply{{size.x, size.y, 0, 0}};
	return gpu::dispatch(rd, apply_.pipeline, {{apply_set, 0}}, gpu::push_bytes(apply),
			gpu::groups(size.x, 8), gpu::groups(size.y, 8));
}
```
Pass gate **ssr**; commit.

- [ ] **Step 5: ssgi**

`shaders/ssgi.comp.glsl`: include; `{ SSGI_PUSH_FIELDS }`.

`extension/src/render/ssgi_pass.cpp`, replace from `static_assert(sizeof(float) * 32 == 128, "ssgi push block");` through `rd->compute_list_end();` with:

```cpp
	ve::SsgiPush push{};
	std::memcpy(push.prev_view_proj, prev_view_proj, sizeof(push.prev_view_proj));
	push.dims[0] = half.x;
	push.dims[1] = half.y;
	push.dims[2] = s.ssgi_taps;
	push.dims[3] = have_history ? 1 : 0;
	// These were literals here until the emissive work: 6 m, 0.90, 1.0. They are knobs in
	// ve::BeautySettings now, which is where that struct always said every knob a pass reads
	// has to live -- and which is what lets a tier move the emissive ring.
	push.params[0] = s.ssgi_radius;
	push.params[1] = s.ssgi_temporal;
	push.params[2] = s.ssgi_strength;
	push.emissive[0] = s.emissive_gi_radius;
	push.emissive[1] = s.emissive_gi_strength;
	push.stage[0] = 0;
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, gpu::groups(half.x, 8), gpu::groups(half.y, 8), 1);
	// The gather rotates its taps by a bayer4 phase that never changes, so without this second
	// dispatch that phase reaches the screen as a lattice of dots. It averages one full 4x4
	// period back out before the temporal blend, which cannot.
	rd->compute_list_add_barrier(list);
	push.stage[0] = 1;
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, gpu::groups(half.x, 8), gpu::groups(half.y, 8), 1);
	rd->compute_list_end();
```
Pass gate **ssgi**; commit.

- [ ] **Step 6: hiz**

`shaders/hiz.comp.glsl`: include; `{ HIZ_PUSH_FIELDS }`.

`extension/src/render/hiz_pass.cpp`, in `build()`, replace from `PackedByteArray pc;` through `rd->compute_list_set_push_constant(list, pc, pc.size());` with:

```cpp
		const ve::HizPush push{{dw, dh, sw, sh}, {m == 0 ? 1 : 0, 0, 0, 0}};
		rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
```
Pass gate **hiz**; commit.

- [ ] **Step 7: deferred (push and cascade block)**

`shaders/deferred.comp.glsl`: include after `#version`; replace the `SunBlock` body with `{ SUN_CASCADE_BLOCK_FIELDS }` (keep `layout(set = 0, binding = 6, std140) uniform SunBlock` and `sun;`, and keep `#define SUN_CASCADES 3` if anything else in the file uses it — `rg -n SUN_CASCADES shaders/deferred.comp.glsl`; delete it if not); replace the push body with `{ DEFERRED_PUSH_FIELDS }`.

`extension/src/render/deferred_pass.cpp`, in `render()`, replace from `// std140: mat4[3] = 192 B, then vec4[3] = 48 B, then vec4 splits = 16 B. 256 total.` through the last `u[27] = 0;` with:

```cpp
	ve::SunCascadeBlock sun{};
	const int n = sun_map.is_valid() ? p.cascade_count : 0;
	for (int c = 0; c < ve::kSunCascades && c < n; c++) {
		std::memcpy(sun.view_proj[c], p.sun_view_proj[c], sizeof(sun.view_proj[c]));
		sun.params[c][0] = p.shadow_texel[c];
		sun.params[c][1] = p.shadow_depth_range_c[c];
		sun.splits[c] = p.cascade_split[c];
	}
	if (n > 0) {
		sun.params[0][2] = p.fade_start;
		sun.params[0][3] = p.fade_end;
	}
	sun.splits[3] = static_cast<float>(n);
	rd->buffer_update(sun_ubo_, 0, sizeof(sun), gpu::push_bytes(sun));

	ve::DeferredPush push{};
	std::memcpy(push.inv_view_proj, p.inv_view_proj, sizeof(push.inv_view_proj));
	push.cam[0] = p.cam_pos[0];
	push.cam[1] = p.cam_pos[1];
	push.cam[2] = p.cam_pos[2];
	push.sky[0] = p.ambient[0];
	push.sky[1] = p.ambient[1];
	push.sky[2] = p.ambient[2];
	push.flags[0] = flags;
	push.flags[1] = static_cast<uint32_t>(p.probe_mode);
```
and change the final `gpu::dispatch(…, pcb, …)` argument to `gpu::push_bytes(push)`. In `ensure_dummies`, `rd->uniform_buffer_create(256, zeros)` → `rd->uniform_buffer_create(sizeof(ve::SunCascadeBlock), zeros)` with `zeros.resize(sizeof(ve::SunCascadeBlock))`.
Pass gate **deferred**; commit.

- [ ] **Step 8: beauty camera**

`shaders/beauty_camera.glslh`: `#include "generated/blocks.glslh"` as the first line after its two header comment lines; `uniform BeautyCam { BEAUTY_CAM_FIELDS } bcam;`.

`extension/src/render/beauty_camera.cpp`: delete the anonymous namespace holding `static_assert(sizeof(float) * 40 == 160, …)`; in `ensure`, `zero.resize(160)` → `zero.resize(sizeof(ve::BeautyCamBlock))` and `uniform_buffer_create(160, zero)` → `uniform_buffer_create(sizeof(ve::BeautyCamBlock), zero)`; in `update`, replace from `PackedByteArray bytes;` through `rd->buffer_update(buffer_, 0, 160, bytes);` with:

```cpp
	ve::BeautyCamBlock block{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			block.view_proj[c * 4 + r] = view_proj.columns[c][r];
			block.inv_view_proj[c * 4 + r] = inv.columns[c][r];
		}
	block.cam[0] = cam_pos[0];
	block.cam[1] = cam_pos[1];
	block.cam[2] = cam_pos[2];
	block.screen[0] = static_cast<float>(size.x);
	block.screen[1] = static_cast<float>(size.y);
	block.screen[2] = 1.0f / static_cast<float>(size.x);
	block.screen[3] = 1.0f / static_cast<float>(size.y);
	// Device-level operation: callers must perform this before opening a list.
	rd->buffer_update(buffer_, 0, sizeof(block), gpu::push_bytes(block));
```
Add `#include "render/gpu/gpu.h"` and `#include "gpu_layout/blocks.h"`. Pass gate **outline** (the heaviest `BeautyCam` reader) plus `res://tests/test_ssr.gd,res://tests/test_contact_shadow.gd`; commit as `refactor(beauty_camera): block from the generated layout`.

- [ ] **Step 9: sun light**

`shaders/sun_light.glslh`: `#include "generated/blocks.glslh"` as the first line after its header comments; `uniform SunLight { SUN_LIGHT_FIELDS } sun_light;` — the text before `{` stays exactly `layout(set = SUN_LIGHT_SET, binding = SUN_LIGHT_BINDING, std140) uniform SunLight` (pinned by `test_sun_light_shader.cpp`).

`extension/src/render/sun_ubo.cpp`: sizes `32` → `sizeof(ve::SunLightBlock)`; `update` body:

```cpp
	if (!rd || !buffer_.is_valid()) return;
	const ve::SunLightBlock block{{s.dir[0], s.dir[1], s.dir[2], 0.0f}, {s.rgb[0], s.rgb[1], s.rgb[2], 0.0f}};
	rd->buffer_update(buffer_, 0, sizeof(block), gpu::push_bytes(block));
```
Add the two includes. Native (`test_sun_light_shader.cpp`) and Pass gate **deferred** plus `res://tests/test_sun_shadow.gd,res://tests/test_grass.gd`; commit as `refactor(sun_ubo): block from the generated layout`.

- [ ] **Step 10: No tautological asserts left in this group**

```bash
rg -n "static_assert\(sizeof\(float\) \*" extension/src
```
Expected: no output.

---

### Task 34: Blocks for the raster passes

**Files:**
- Modify: `extension/src/render/{lod_raster,composite,grass_raster,sun_shadow}_pass.cpp`
- Modify: `shaders/lod.vert.glsl`, `shaders/lod.frag.glsl`, `shaders/composite.vert.glsl`, `shaders/composite.frag.glsl`, `shaders/grass.vert.glsl`, `shaders/grass.frag.glsl`, `shaders/lod_shadow.vert.glsl`

**Interfaces:**
- Consumes: Task 32's `LodRasterPush`, `CompositePush`, `GrassRasterPush`, `SunShadowPush`.
- Produces: no hand-offset packing left in these passes. Both stages of each raster pipeline declare the same macro (Godot rejects differing push reflections between stages).

- [ ] **Step 1: lod_raster**

Both `shaders/lod.vert.glsl` and `shaders/lod.frag.glsl`: include; `{ LOD_RASTER_PUSH_FIELDS }`.

`extension/src/render/lod_raster_pass.cpp`, in `draw()`, replace from `PackedByteArray pc;` through `rd->draw_list_set_push_constant(dl, pc, pc.size());` with:

```cpp
	ve::LodRasterPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			push.view_proj[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
	push.cam[0] = cam_pos[0];
	push.cam[1] = cam_pos[1];
	push.cam[2] = cam_pos[2];
	push.cam[3] = fade_start;
	push.fade[0] = fade_end;
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
```
Pass gate **lod_raster**; commit.

- [ ] **Step 2: composite**

Both composite stages: include; `{ COMPOSITE_PUSH_FIELDS }`.

`extension/src/render/composite_pass.cpp`, in `draw()`, replace from `// Exactly 128 bytes: …` through the closing `}` of the packing block with:

```cpp
	// Both stages declare the same block (Godot rejects differing reflections between stages
	// of one pipeline); the vertex stage ignores everything but its shape.
	ve::CompositePush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			push.view_proj[c * 4 + r] = view_proj.columns[c][r];
	push.cam[0] = cam.cam_pos[0];
	push.cam[1] = cam.cam_pos[1];
	push.cam[2] = cam.cam_pos[2];
	// NOT cam.cam_pos[3]: the marcher's block hides the packed beauty flags in that slot.
	push.cam[3] = fade_start;
	push.fade[0] = fade_end;
	push.fade[1] = cam.cam_fwd[0];
	push.fade[2] = cam.cam_fwd[1];
	push.fade[3] = cam.cam_fwd[2];
	push.right_tanx[0] = cam.cam_right[0];
	push.right_tanx[1] = cam.cam_right[1];
	push.right_tanx[2] = cam.cam_right[2];
	push.right_tanx[3] = cam.params[0]; // tan(fov_x / 2)
	push.up_tany[0] = cam.cam_up[0];
	push.up_tany[1] = cam.cam_up[1];
	push.up_tany[2] = cam.cam_up[2];
	push.up_tany[3] = cam.params[1]; // tan(fov_y / 2)
```
and change `rd->draw_list_set_push_constant(dl, pc, pc.size());` to `rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));`.
Pass gate **composite**; commit.

- [ ] **Step 3: grass_raster**

`shaders/grass.vert.glsl` and `shaders/grass.frag.glsl` (block instance name is `push`): include; `{ GRASS_RASTER_PUSH_FIELDS }`.

`extension/src/render/grass_raster_pass.cpp`, in `draw()`, replace from `PackedByteArray pc;` through `rd->draw_list_set_push_constant(dl, pc, pc.size());` with:

```cpp
	ve::GrassRasterPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) push.view_proj[c * 4 + r] = view_proj.columns[c][r];
	push.cam[0] = cam_pos[0];
	push.cam[1] = cam_pos[1];
	push.cam[2] = cam_pos[2];
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
```
Pass gate **grass_raster**; commit.

- [ ] **Step 4: sun_shadow**

`shaders/lod_shadow.vert.glsl`: include; `{ SUN_SHADOW_PUSH_FIELDS }`. Check `rg -n push_constant shaders/lod_shadow.frag.glsl`: if the fragment stage declares the block too, give it the same macro.

`extension/src/render/sun_shadow_pass.cpp`, in `build()`, replace from `PackedByteArray pc;` through `rd->draw_list_set_push_constant(dl, pc, pc.size());` with:

```cpp
	ve::SunShadowPush push{};
	std::memcpy(push.sun_view_proj, ortho.view_proj, sizeof(push.sun_view_proj));
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
```
Pass gate **sun_shadow**; commit.

---

### Task 35: Blocks for raymarch, island cull, LoD cull, grass scatter and downsample

**Files:**
- Modify: `extension/src/render/{raymarch,island_cull,lod_cull,grass_scatter}_pass.cpp`, `extension/src/render/orchestrator.cpp`
- Modify: `shaders/raymarch.comp.glsl`, `shaders/island_cull.comp.glsl`, `shaders/lod_cull.comp.glsl`, `shaders/grass.glslh`, `shaders/grass.vert.glsl`, `shaders/grass.frag.glsl`, `shaders/grass_bricks.comp.glsl`, `shaders/grass_scatter.comp.glsl`, `shaders/downsample.comp.glsl`
- Modify: `extension/src/render/camera_params.h` (comment only)

**Interfaces:**
- Consumes: Task 32's `CameraParams` table, `EditsBlock`, `LodCullPush`, `GrassParams` table, `GrassRegionBlock`, `DownsamplePush`.
- Produces: `GRASS_PARAMS_BLOCK` no longer exists; `GRASS_PARAMS_FIELDS` replaces it.

- [ ] **Step 1: raymarch and island cull (`CameraParams`, `EditsBlock`)**

`shaders/raymarch.comp.glsl`: include after `#version`; push body → `{ CAMERA_PARAMS_FIELDS }`; `uniform Edits { vec4 center; vec4 params; } edits;` → `uniform Edits { EDITS_BLOCK_FIELDS } edits;`.
`shaders/island_cull.comp.glsl`: include; push body → `{ CAMERA_PARAMS_FIELDS }`.
`extension/src/render/camera_params.h`: change the comment "The shader declares the same eight vec4s" to "shaders/generated/blocks.glslh declares them from gpu_layout/blocks.h's table (CAMERA_PARAMS_FIELDS)".

`extension/src/render/raymarch_pass.cpp`, in `render()`: replace the edits block (from `PackedByteArray eb;` through `rd->buffer_update(edits_ubo_, 0, 32, eb);`) with

```cpp
		const ve::EditsBlock edits{{edit_state[0], edit_state[1], edit_state[2], 0.0f},
				{edit_state[3] /* radius */, edit_state[4] /* type */, edit_state[5] /* material */,
						edit_state[3] > 0.0f ? 1.0f : 0.0f}};
		rd->buffer_update(edits_ubo_, 0, sizeof(edits), gpu::push_bytes(edits));
```
and replace `PackedByteArray pc; pc.resize(sizeof(ve::CameraParams)); std::memcpy(pc.ptrw(), &cam, sizeof(ve::CameraParams));` with `const PackedByteArray pc = gpu::push_bytes(cam);`. In `initialize`, `zero.resize(32)` / `uniform_buffer_create(32, zero)` → `sizeof(ve::EditsBlock)`.

`extension/src/render/island_cull_pass.cpp`, in `render()`: replace `PackedByteArray b; b.resize(sizeof(ve::CameraParams)); std::memcpy(b.ptrw(), &pc, sizeof(ve::CameraParams));` with `const PackedByteArray b = gpu::push_bytes(pc);`.

Pass gate **raymarch** plus `res://tests/test_island_render.gd`; commit `refactor(raymarch): camera and edits blocks from the generated layout`.

- [ ] **Step 2: lod_cull**

`shaders/lod_cull.comp.glsl`: include; `{ LOD_CULL_PUSH_FIELDS }`.

`extension/src/render/lod_cull_pass.cpp`, in `run()`, replace from `PackedByteArray pc;` through `ip[3] = 0;` with

```cpp
	// The shader derives frustum planes from view_proj.
	ve::LodCullPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			push.view_proj[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
	push.params[0] = page_count;
	push.params[1] = HizPass::kSize;
	push.params[2] = hiz->mip_count();
```
and `rd->compute_list_set_push_constant(list, pc, pc.size());` → `rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));`.
Pass gate **lod_cull**; commit.

- [ ] **Step 3: grass params and region**

```bash
rg -n "GRASS_PARAMS_BLOCK" shaders extension tests
```
Expected hits: the `#define` in `shaders/grass.glslh` and one use each in `grass.vert.glsl`, `grass.frag.glsl`, `grass_bricks.comp.glsl`, `grass_scatter.comp.glsl`. Any other hit: stop and report.

`shaders/grass.glslh`: delete the `#define GRASS_PARAMS_BLOCK \ … ivec4 limits;` block; add `#include "generated/blocks.glslh"` as the first line after the header comments; change its comment "Mirrors ve::GrassParams in extension/src/grass/grass_layout.h, which test_grass_layout pins at 256 bytes with cam.xyz first." to "The GrassParams field list is GRASS_PARAMS_FIELDS in shaders/generated/blocks.glslh, generated from ve::GrassParams."
The four users: `{ GRASS_PARAMS_BLOCK }` → `{ GRASS_PARAMS_FIELDS }`.
`shaders/grass_bricks.comp.glsl` and `shaders/grass_scatter.comp.glsl`: `uniform Region { ivec4 dims; ivec4 region_origin;` / `ivec4 atlas_bricks; } pc;` → `uniform Region { GRASS_REGION_FIELDS } pc;` (one line, keeping `layout(set = 0, binding = 10, std140)`).

`extension/src/render/grass_scatter_pass.cpp`, in `run()`: replace `PackedByteArray ubo; ubo.resize(sizeof(ve::GrassParams)); std::memcpy(ubo.ptrw(), &params, sizeof(ve::GrassParams)); rd->buffer_update(params_ubo_, 0, ubo.size(), ubo);` with `rd->buffer_update(params_ubo_, 0, sizeof(params), gpu::push_bytes(params));`, and the region block's `PackedByteArray rb; …; rd->buffer_update(region_ubo_, 0, 48, rb);` with

```cpp
		const ve::GrassRegionBlock region{{win.dim, win.dim, win.dim, 0},
				{win.origin.x, win.origin.y, win.origin.z, 0}, {ab.x, ab.y, ab.z, 0}};
		rd->buffer_update(region_ubo_, 0, sizeof(region), gpu::push_bytes(region));
```
In `ensure_buffers`, `rd->uniform_buffer_create(48u)` → `rd->uniform_buffer_create(sizeof(ve::GrassRegionBlock))`.
Native (`test_grass_layout.cpp`, `test_grass_blade_shader.cpp`, `test_grass_tilt_shader.cpp`) and Pass gate **grass_scatter**; commit `refactor(grass): params and region blocks from the generated layout`.

- [ ] **Step 4: downsample**

`shaders/downsample.comp.glsl`: include; `uniform Push { DOWNSAMPLE_PUSH_FIELDS } pc;`.

`extension/src/render/orchestrator.cpp`, `downsample_history`: replace `PackedByteArray pc; pc.resize(16); int32_t *dims = …; dims[0] = half.x; dims[1] = half.y; dims[2] = dims[3] = 0;` with `const ve::DownsamplePush push{{half.x, half.y, 0, 0}};` and the `pc` argument of `gpu::dispatch` with `gpu::push_bytes(push)`.
Pass gate **orchestrator**; commit.

---

### Task 36: Blocks for the world-job passes

**Files:**
- Modify: `extension/src/render/{brick_gen,consolidate,region,island_extract,mesh,lod_build}_pass.cpp`
- Modify: `shaders/brick_gen.comp.glsl`, `shaders/brick_consolidate.comp.glsl`, `shaders/brick_mark.comp.glsl`, `shaders/region_free.comp.glsl`, `shaders/dispatch_args.comp.glsl`, `shaders/island_extract.comp.glsl`, `shaders/mesh_common.glslh`, `shaders/lod_common.glslh`

**Interfaces:**
- Consumes: Task 32's world-job structs.
- Produces: `rg 'reinterpret_cast<(int32_t|float|uint32_t) \*>\((pc|trace_pc|apply_pc|pcb)\.ptrw' extension/src/render` returns nothing.

- [ ] **Step 1: brick_gen**

`shaders/brick_gen.comp.glsl`: include; `{ BRICK_GEN_PUSH_FIELDS }`. `BrickGenPass::dispatch`: replace the four `pc` lines with `const ve::BrickGenPush push{{atlas_bricks_.x, atlas_bricks_.y, atlas_bricks_.z, 0}};` and `rd->compute_list_set_push_constant(list, pc, pc.size());` with `rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));`. Pass gate **brick_gen**; commit.

- [ ] **Step 2: region (mark, free, args)**

Shaders: `brick_mark.comp.glsl` → `{ BRICK_MARK_PUSH_FIELDS }`; `region_free.comp.glsl` → `{ REGION_FREE_PUSH_FIELDS }`; `dispatch_args.comp.glsl` → `{ DISPATCH_ARGS_PUSH_FIELDS }`; include in each.

`RegionPass::mark`: replace from `PackedByteArray pc;` through `p[15] = …;` with

```cpp
	ve::BrickMarkPush push{};
	push.region[0] = region.x; push.region[1] = region.y; push.region[2] = region.z;
	push.region[3] = region_slot;
	push.lo[0] = lo.x; push.lo[1] = lo.y; push.lo[2] = lo.z;
	push.hi[0] = hi.x; push.hi[1] = hi.y; push.hi[2] = hi.z;
	push.cfg[0] = op_count;
	push.cfg[2] = max_brick_jobs_;
	// 0 = plain stream-in, 1 = force resident regeneration, 2 = edit: generate every
	// touched brick so the exact lattice, rather than the activation probe, owns occupancy.
	push.cfg[3] = generate_probe_misses ? 2 : (force_regen ? 1 : 0);
```
and in the two phases `p[13] = 0;` → `push.cfg[1] = 0;`, `p[13] = 1;` → `push.cfg[1] = 1;`, each `rd->compute_list_set_push_constant(list, pc, pc.size());` → `rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));`.

`RegionPass::release_region`: `PackedByteArray pc; pc.resize(16); reinterpret_cast<int32_t *>(pc.ptrw())[0] = region_slot;` → `const ve::RegionFreePush push{{region_slot, 0, 0, 0}};`, and the push call as above.

`RegionPass::write_dispatch_args`: keep its comment; `PackedByteArray pc; pc.resize(16); pc.fill(0);` → `const ve::DispatchArgsPush push{};`, and the push call as above.

Pass gate **region**; commit.

- [ ] **Step 3: consolidate**

`shaders/brick_consolidate.comp.glsl`: include; `{ CONSOLIDATE_PUSH_FIELDS }`. `ConsolidatePass::run`: replace the six `pc`/`p` lines with `const ve::ConsolidatePush push{{n, job.region_slot, static_cast<int>(job.ops.size()), pool_->region_table(job.region_slot)}};` and the push call. Pass gate **consolidate**; commit.

- [ ] **Step 4: island_extract**

`shaders/island_extract.comp.glsl`: include; `{ ISLAND_EXTRACT_PUSH_FIELDS }`. `IslandExtractPass::extract`: replace from `PackedByteArray pc;` through `pi[7] = job.override_table;` with

```cpp
	const ve::IslandExtractPush push{{job.origin[0], job.origin[1], job.origin[2], job.voxel},
			{job.dim, op_count, box_count, job.override_table}};
```
and the push call. Pass gate **island_extract**; commit.

- [ ] **Step 5: mesh**

`shaders/mesh_common.glslh`: include as its first non-comment line; `{ MESH_PUSH_FIELDS }`. `MeshPass::push` body:

```cpp
void MeshPass::push(int64_t list, const MeshJob &job, int job_index) {
	const ve::MeshPush push{{job.chunk.x, job.chunk.y, job.chunk.z, job_index},
			{sanitized_op_count(job), cfg_.max_verts, cfg_.max_tris, job.lattice},
			{job.origin[0], job.origin[1], job.origin[2], job.cell_size},
			{job.override_table, -1, 0, 0}};
	rd_->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
}
```
Pass gate **mesh**; commit.

- [ ] **Step 6: lod_build**

`shaders/lod_common.glslh`: include as its first non-comment line; `uniform LodPush { LOD_BUILD_PUSH_FIELDS } lpc;`. `LodBuildPass::push` body:

```cpp
void LodBuildPass::push(int64_t list, const LodBuildJob &job, int job_index) {
	float origin[3];
	ve::lod_chunk_origin(job.level, job.coord, origin);
	const ve::LodBuildPush push{{job.coord.x, job.coord.y, job.coord.z, job_index},
			{sanitized_op_count(job), ve::kLodMaxQuadsPerChunk, job.level, 0},
			{origin[0], origin[1], origin[2], ve::lod_cell_size(job.level)},
			{job.override_table, -1, 0, 0}};
	rd_->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
}
```
Pass gate **lod_build**; commit.

- [ ] **Step 7: Nothing hand-packed remains**

```bash
rg -n 'reinterpret_cast<(int32_t|float|uint32_t) \*>\((pc|trace_pc|apply_pc|pcb|ub|eb|rb|ubo|bytes)\.ptrw' extension/src/render
rg -n 'resize\(sizeof\(ve::CameraParams\)\)' extension/src/render
```
Expected: no output from either. (Data buffers such as island extract's box list `b` and consolidation's job list `jb` are not blocks and stay as they are.) A hit is a block this plan missed: stop and report rather than inventing a struct.

---

### Task 37: Generated constants — beauty flags, material layers, strides (closes S9)

**Files:**
- Create: `extension/src/gpu_layout/constants.h`, `extension/src/gpu_layout/constants.cpp`
- Create: `shaders/generated/constants.glslh` (generated)
- Modify: `extension/src/shade/beauty_settings.h` (`kBeautyFlags`)
- Modify: `extension/src/world/material_table.h` (`kMaterialLayers`), `extension/src/render/material_atlas.h`
- Modify: `extension/src/world/brick.h` (override strides), `extension/src/render/override_pool.cpp`, `extension/src/render/volume_pool.cpp`
- Modify: `shaders/common.glslh`, `shaders/shade.glslh`, `shaders/composite.frag.glsl`, `shaders/raymarch.comp.glsl`, `shaders/lod.frag.glsl`, `shaders/deferred.comp.glsl`, `shaders/field_ops.glslh`, `shaders/brick_consolidate.comp.glsl`
- Modify: `shaders/generated/field.glslh.golden` (regenerated: text only)
- Test: `extension/tests/test_gpu_layout.cpp`, `extension/tests/test_generated_glsl.cpp`

**Interfaces:**
- Consumes: Task 32's `check_generated`.
- Produces: `ve::BeautyFlag { const char *name; uint32_t bit; }`, `ve::kBeautyFlags[]`; `ve::kMaterialLayers` (moved to `world/material_table.h`; `godot::kMaterialLayers` stays as an alias); `ve::kOverrideSdfStrideBytes`, `ve::kOverrideMatStrideBytes`; `std::string ve::layout::constants_glsl()`; GLSL `BEAUTY_*`, `MATERIAL_LAYERS`, `BRICK_VOXEL_COUNT`, `BRICK_SDF_COUNT`, `VOLUME_VOXELS`, `OVERRIDE_SDF_STRIDE_BYTES`, `OVERRIDE_MAT_STRIDE_BYTES`; the `common.glslh` material gate is `VE_MATERIAL_ARRAYS`.

- [ ] **Step 1: Failing tests**

Append to `extension/tests/test_gpu_layout.cpp` (add `#include "shade/beauty_settings.h"`):

```cpp
TEST_CASE("beauty flags are distinct single bits") {
	uint32_t seen = 0;
	for (const ve::BeautyFlag &f : ve::kBeautyFlags) {
		CHECK_MESSAGE(f.bit != 0, f.name);
		CHECK_MESSAGE((f.bit & (f.bit - 1)) == 0, f.name);
		CHECK_MESSAGE((seen & f.bit) == 0, f.name);
		seen |= f.bit;
	}
	CHECK(sizeof(ve::kBeautyFlags) / sizeof(ve::kBeautyFlags[0]) == 9);
}
```

Append to `extension/tests/test_generated_glsl.cpp` (add `#include "gpu_layout/constants.h"`):

```cpp
TEST_CASE("generated: shaders/generated/constants.glslh") {
	check_generated("shaders/generated/constants.glslh", ve::layout::constants_glsl());
}
```

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: compile errors (`BeautyFlag`, `constants.h` missing).

- [ ] **Step 2: C++ sources of truth**

`extension/src/shade/beauty_settings.h`, after `kFlagCostView`:

```cpp

// Every flag bit a shader tests, by GLSL name (BEAUTY_<name>). shaders/generated/constants.glslh
// is emitted from this table, so a new flag is one constant above and one row here.
struct BeautyFlag {
	const char *name;
	uint32_t bit;
};

inline constexpr BeautyFlag kBeautyFlags[] = {
	{"SSGI", kFlagSsgi},
	{"SSR", kFlagSsr},
	{"CONTACT", kFlagContact},
	{"OUTLINES", kFlagOutlines},
	{"SUN_MAP", kFlagSunMap},
	{"GLOSSY_RAYS", kFlagGlossyRays},
	{"RAY_SUN_SHADOW", kFlagRaySunShadow},
	{"SSAO", kFlagSsao},
	{"COST_VIEW", kFlagCostView},
};
```
and change its comment "Bit layout, mirrored by BEAUTY_* in the shaders." to "Bit layout; kBeautyFlags below generates BEAUTY_* for the shaders."

`extension/src/world/material_table.h`, after `kMaterialCount`:

```cpp
// Layers in the material texture arrays (render/material_atlas.h allocates exactly this many and
// fills unused layers with flat error magenta); generated into GLSL as MATERIAL_LAYERS.
inline constexpr int kMaterialLayers = 16;
static_assert(kMaterialCount <= kMaterialLayers, "more materials than atlas layers");
```

`extension/src/render/material_atlas.h`: add `#include "world/material_table.h"` and change `constexpr int kMaterialLayers = 16;` to `constexpr int kMaterialLayers = ve::kMaterialLayers;`.

`extension/src/world/brick.h`, after `kBrickSdfCount`:

```cpp
// One override brick's bytes in the GPU override pool: the SDF lattice and the material cells,
// each rounded up to whole uint32 words (render/override_pool.cpp packs them this way).
inline constexpr int kOverrideSdfStrideBytes = (kBrickSdfCount + 3) / 4 * 4;   // 4916
inline constexpr int kOverrideMatStrideBytes = (kBrickVoxelCount + 3) / 4 * 4; // 4096
```

`extension/src/render/override_pool.cpp`: replace the anonymous-namespace constants and the four `static_assert`s with

```cpp
constexpr int kSdfWords = (ve::kBrickSdfCount + 3) / 4;
constexpr int kSdfStrideBytes = ve::kOverrideSdfStrideBytes;
constexpr int kMatWords = (ve::kBrickVoxelCount + 3) / 4;
constexpr int kMatStrideBytes = ve::kOverrideMatStrideBytes;
```
(delete `kSdfWords`/`kMatWords` if `rg` shows no other use in the file).

`extension/src/render/volume_pool.cpp`: delete the comment block starting "shaders/field.glslh hard-codes VOLUME_VOXELS" and both `static_assert`s. These were the S9 asserts: GLSL now reads the value from `generated/constants.glslh`, which the byte test regenerates from `ve::kIslandVoxelCount`.

- [ ] **Step 3: Emitter**

`extension/src/gpu_layout/constants.h`:

```cpp
#pragma once
#include <string>

namespace ve::layout {

// The exact contents of shaders/generated/constants.glslh.
std::string constants_glsl();

} // namespace ve::layout
```

`extension/src/gpu_layout/constants.cpp`:

```cpp
#include "gpu_layout/constants.h"
#include "generator/volume_set.h"
#include "shade/beauty_settings.h"
#include "world/brick.h"
#include "world/material_table.h"
#include <sstream>

namespace ve::layout {

std::string constants_glsl() {
	std::ostringstream o;
	o << "// GENERATED from C++ constants by ve::layout::constants_glsl() (extension/src/gpu_layout/).\n"
	     "// Do not edit by hand: extension/tests/test_generated_glsl.cpp asserts this file byte\n"
	     "// for byte. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests\n"
	     "\n"
	     "// ve::kBeautyFlags (extension/src/shade/beauty_settings.h)\n";
	for (const BeautyFlag &f : kBeautyFlags)
		o << "const uint BEAUTY_" << f.name << " = " << f.bit << "u;\n";
	o << "\n"
	     "// ve::kMaterialLayers (extension/src/world/material_table.h)\n"
	     "const int MATERIAL_LAYERS = " << kMaterialLayers << ";\n"
	     "\n"
	     "// Brick, volume and override-pool strides (world/brick.h, generator/volume_set.h)\n"
	     "const int BRICK_VOXEL_COUNT = " << kBrickVoxelCount << ";\n"
	     "const int BRICK_SDF_COUNT = " << kBrickSdfCount << ";\n"
	     "const int VOLUME_VOXELS = " << kIslandVoxelCount << ";\n"
	     "const int OVERRIDE_SDF_STRIDE_BYTES = " << kOverrideSdfStrideBytes << ";\n"
	     "const int OVERRIDE_MAT_STRIDE_BYTES = " << kOverrideMatStrideBytes << ";\n";
	return o.str();
}

} // namespace ve::layout
```

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
(cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc='generated: shaders/generated/constants.glslh') 2>&1 | tail -3
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: the last run passes.

- [ ] **Step 4: Shaders read the generated constants**

```bash
rg -n "const uint BEAUTY_|#define MATERIAL_LAYERS|MATERIAL_LAYERS\b|BRICK_VOXEL_COUNT =|BRICK_SDF_COUNT =|VOLUME_VOXELS =|\* 4916|slot \* 4096" shaders --glob '!generated/*'
```
Record the hits; each is handled below, and any other is a stop-and-report.

`shaders/common.glslh`:
- add `#include "generated/constants.glslh"` as the first line after the two header comment lines;
- delete `const int BRICK_VOXEL_COUNT = 4096;            // 16^3` and `const int BRICK_SDF_COUNT = 4913;              // 17^3`;
- delete the two comment lines above `const int VOLUME_VOXELS = 262144;` and that line;
- change `// and define MATERIAL_LAYERS to the array's layer count.` to `// and define VE_MATERIAL_ARRAYS. MATERIAL_LAYERS comes from generated/constants.glslh.`;
- change `#ifdef MATERIAL_LAYERS` to `#ifdef VE_MATERIAL_ARRAYS`.

`shaders/composite.frag.glsl`, `shaders/raymarch.comp.glsl`, `shaders/lod.frag.glsl`, `shaders/deferred.comp.glsl`: `#define MATERIAL_LAYERS 16` → `#define VE_MATERIAL_ARRAYS`.

`shaders/shade.glslh`: replace the nine `const uint BEAUTY_… = …u;` lines under `// ---- ve::pack_flags bits ---` with `#include "generated/constants.glslh"`.

`shaders/field_ops.glslh`: `int base = slot * 4916;` → `int base = slot * OVERRIDE_SDF_STRIDE_BYTES;`; `int mi = slot * 4096 + …` → `int mi = slot * OVERRIDE_MAT_STRIDE_BYTES + …`.

`shaders/brick_consolidate.comp.glsl`: `slot * 4916` → `slot * OVERRIDE_SDF_STRIDE_BYTES`; `slot * 4096` → `slot * OVERRIDE_MAT_STRIDE_BYTES`.

- [ ] **Step 5: Regenerate the field golden (text only)**

`field_ops.glslh` is embedded in the generated field source, so its golden text changes:

```bash
(cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc='the default pipeline generates the committed source') 2>&1 | tail -2
git diff --stat shaders/generated/field.glslh.golden
git diff shaders/generated/field.glslh.golden | grep '^[-+][^-+]'
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: exactly two changed lines (the two stride literals). `extension/tests/golden/*` unchanged (`git status --short extension/tests/golden` prints nothing).

- [ ] **Step 6: Verify**

Run the Pass gate for **brick_gen** with extra suites `res://tests/test_consolidation.gd,res://tests/test_field_diff.gd,res://tests/test_deferred.gd,res://tests/test_raymarch_gbuffer.gd,res://tests/test_lod_gbuffer.gd,res://tests/test_material_atlas.gd,res://tests/test_beauty_settings.gd,res://tests/test_field_volume_diff.gd`.

- [ ] **Step 7: Commit**

```bash
git add extension/src/gpu_layout/constants.h extension/src/gpu_layout/constants.cpp shaders/generated/constants.glslh \
	extension/src/shade/beauty_settings.h extension/src/world/material_table.h extension/src/render/material_atlas.h \
	extension/src/world/brick.h extension/src/render/override_pool.cpp extension/src/render/volume_pool.cpp \
	shaders/common.glslh shaders/shade.glslh shaders/composite.frag.glsl shaders/raymarch.comp.glsl \
	shaders/lod.frag.glsl shaders/deferred.comp.glsl shaders/field_ops.glslh shaders/brick_consolidate.comp.glsl \
	shaders/generated/field.glslh.golden extension/tests/test_gpu_layout.cpp extension/tests/test_generated_glsl.cpp \
	docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "feat: beauty flags, material layers and strides generated into GLSL (S9)

The field golden's text moved by the two stride literals now spelled by name; float
baselines unchanged. The misnamed static_asserts are deleted."
```

---

### Task 38: Named material ids

**Files:**
- Modify: `extension/src/world/material_table.h`, `extension/src/world/material_table.cpp`
- Modify: `shaders/material_table.glslh` (regenerated)
- Modify: `extension/src/generator/generator.cpp`, `extension/src/terrain/builtin_stages.cpp`
- Modify: `shaders/stages/height_bands.field.glslh`, `shaders/grass_scatter.comp.glsl`, `shaders/raymarch.comp.glsl`
- Modify: `shaders/generated/field.glslh.golden` (regenerated: text only)
- Test: `extension/tests/test_material_table.cpp`, `extension/tests/test_material_glslh.cpp`

**Interfaces:**
- Consumes: Task 5's `upper()` in `material_table.cpp`.
- Produces: `constexpr uint16_t ve::material_id(std::string_view name)`; GLSL `MAT_GRASS_01`, `MAT_ROCK`, `MAT_GROUND_01`, `MAT_BREAKSTONE`, `MAT_GROUND_CRACK_01`, `MAT_ICE_CRACK`, `MAT_ICE`.

- [ ] **Step 1: Failing tests**

Append to `extension/tests/test_material_table.cpp`:

```cpp
TEST_CASE("material_id names every material and is usable at compile time") {
	static_assert(ve::material_id("grass_01") == 1);
	static_assert(ve::material_id("rock") == 2);
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(ve::material_id(ve::kMaterials[i].name) == i + 1);
	CHECK(ve::material_id("no_such_material") == 0);
}
```

Append to `extension/tests/test_material_glslh.cpp`:

```cpp
TEST_CASE("the emitter names every terrain material and no name collides with a table") {
	const std::string s = ve::material_table_glsl();
	for (int i = 0; i < ve::kMaterialCount; i++) {
		std::string upper = ve::kMaterials[i].name;
		for (char &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		CHECK(s.find("const uint MAT_" + upper + " = " + std::to_string(i + 1) + "u;") != std::string::npos);
		CHECK(upper != "GLOW");
		CHECK(upper != "GLOW_RGB");
		CHECK(upper != "FLAT_ALBEDO");
	}
}
```
(add `#include <cctype>`).

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
```
Expected: compile error, `material_id` is not a member of `ve`.

- [ ] **Step 2: `material_id`**

`extension/src/world/material_table.h`: add `#include <string_view>`; after `kMaterialLayers`:

```cpp
// The id of the terrain material called `name`. Code that places a material by name (terrain
// stages, the analytic generator, grass) uses this, never a literal, so reordering kMaterials
// renumbers every use at once. An unknown name is a compile error wherever the result must be
// a constant expression (it reaches a non-constexpr call), and air (0) at run time.
uint16_t unknown_material_name();

constexpr uint16_t material_id(std::string_view name) {
	for (int i = 0; i < kMaterialCount; i++)
		if (name == kMaterials[i].name) return static_cast<uint16_t>(i + 1);
	return unknown_material_name();
}
```

`extension/src/world/material_table.cpp`, inside `namespace ve`:

```cpp
uint16_t unknown_material_name() {
	return 0;
}
```

In `material_table_glsl()`, replace `"const int MATERIAL_COUNT = " << kMaterialCount << ";\n\n";` with

```cpp
	     "const int MATERIAL_COUNT = " << kMaterialCount << ";\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "const uint MAT_" << upper(kMaterials[i].name) << " = " << (i + 1) << "u;\n";
	o << "\n";
```

Regenerate `shaders/material_table.glslh` from the stale-file failure message (Task 5 Step 7's command), then `(cd extension && scons -Q test) 2>&1 | tail -3`.

- [ ] **Step 3: Replace the literals**

`extension/src/generator/generator.cpp`: add `#include "world/material_table.h"`; at file scope after the includes:

```cpp
namespace {
// The analytic generator's height bands, by name: rock above 4 m, grass above 1 m, ground below.
constexpr uint16_t kBandRock = ve::material_id("rock");
constexpr uint16_t kBandGrass = ve::material_id("grass_01");
constexpr uint16_t kBandGround = ve::material_id("ground_01");
} // namespace
```
Both `mat = h > 4.0f ? 2 : (h > 1.0f ? 1 : 3);` → `mat = h > 4.0f ? kBandRock : (h > 1.0f ? kBandGrass : kBandGround);`.

`extension/src/terrain/builtin_stages.cpp`: add `#include "world/material_table.h"` and the same three constants in an anonymous namespace; `ctx.f(material) = h > 4.0f ? 2.0f : (h > 1.0f ? 1.0f : 3.0f);` → `ctx.f(material) = h > 4.0f ? float(kBandRock) : (h > 1.0f ? float(kBandGrass) : float(kBandGround));`.

`shaders/stages/height_bands.field.glslh`: `ctx.material = ctx.height > 4.0 ? 2u : (ctx.height > 1.0 ? 1u : 3u);` → `ctx.material = ctx.height > 4.0 ? MAT_ROCK : (ctx.height > 1.0 ? MAT_GRASS_01 : MAT_GROUND_01);`.

`shaders/grass_scatter.comp.glsl`: delete `const uint GRASS_MATERIAL = 1u; // grass_01, ve::kMaterials[0]`; `!= GRASS_MATERIAL` → `!= MAT_GRASS_01` (the file includes `common.glslh`, which includes `material_table.glslh`).

`shaders/raymarch.comp.glsl`: `flat_material_albedo(4u)` → `flat_material_albedo(MAT_BREAKSTONE)`.

```bash
rg -n "GRASS_MATERIAL|flat_material_albedo\([0-9]|\? [0-9]u :|\? [0-9]\.0f :" shaders extension/src
```
Expected: no output.

- [ ] **Step 4: Regenerate the field golden (text only)**

```bash
(cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc='the default pipeline generates the committed source') 2>&1 | tail -2
git diff shaders/generated/field.glslh.golden | grep '^[-+][^-+]'
(cd extension && scons -Q test) 2>&1 | tail -3
git status --short extension/tests/golden
```
Expected: one changed line (the height-band line); native all pass; float baselines unchanged (no output from the last command).

- [ ] **Step 5: Verify**

Pass gate for **brick_gen** with extra suites `res://tests/test_field_diff.gd,res://tests/test_grass.gd,res://tests/test_grass_golden.gd,res://tests/test_generator_seam.gd,res://tests/test_field_baseline_gpu.gd`.

- [ ] **Step 6: Commit**

```bash
git add extension/src/world/material_table.h extension/src/world/material_table.cpp shaders/material_table.glslh \
	extension/src/generator/generator.cpp extension/src/terrain/builtin_stages.cpp \
	shaders/stages/height_bands.field.glslh shaders/grass_scatter.comp.glsl shaders/raymarch.comp.glsl \
	shaders/generated/field.glslh.golden extension/tests/test_material_table.cpp extension/tests/test_material_glslh.cpp \
	docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "feat: materials are placed by name in C++ and GLSL

The field golden's text moved by the height-band line now spelled MAT_*; float baselines unchanged."
```

---

### Task 39: Generated G-buffer channels

One commit for the layout and generated file, one for the raster passes' attachment counts, then one per shader.

**Files:**
- Create: `extension/src/gpu_layout/gbuffer_layout.h`, `extension/src/gpu_layout/gbuffer_layout.cpp`
- Create: `shaders/generated/gbuffer.glslh` (generated)
- Modify: `extension/src/render/gbuffer.h`, `extension/src/render/gbuffer.cpp`
- Modify: `extension/src/render/composite_pass.cpp`, `extension/src/render/lod_raster_pass.cpp`, `extension/src/render/grass_raster_pass.cpp`
- Modify: `shaders/composite.frag.glsl`, `shaders/lod.frag.glsl`, `shaders/grass.frag.glsl`, `shaders/deferred.comp.glsl`, `shaders/ssao.comp.glsl`, `shaders/ssgi.comp.glsl`, `shaders/outline.comp.glsl`, `shaders/ssr.comp.glsl`
- Test: `extension/tests/test_generated_glsl.cpp`

**Interfaces:**
- Consumes: Task 32's `check_generated`.
- Produces: `ve::layout::kGbAttachments[]`, `ve::layout::kGbColorAttachments` (= 2), `std::string ve::layout::gbuffer_glsl()`; GLSL macros `GB_ATTACHMENTS`, `GB_ALBEDO(g0)`, `GB_SUN_VIS(g0)`, `GB_NORMAL(g1)`, `GB_MATERIAL_ID(g1)`, `GB_IS_SURFACE(g1)`, `GB_GLOSS(g1)`, `GB_PACK_ALBEDO(rgb, sun_vis)`, `GB_PACK_SURFACE_OCT(oct, mat, gloss)`, `GB_PACK_SURFACE(n, mat, gloss)`.

- [ ] **Step 1: Failing golden test**

Append to `extension/tests/test_generated_glsl.cpp` (add `#include "gpu_layout/gbuffer_layout.h"`):

```cpp
TEST_CASE("generated: shaders/generated/gbuffer.glslh") {
	check_generated("shaders/generated/gbuffer.glslh", ve::layout::gbuffer_glsl());
	CHECK(ve::layout::kGbColorAttachments == 2);
}
```

- [ ] **Step 2: Layout and emitter**

`extension/src/gpu_layout/gbuffer_layout.h`:

```cpp
#pragma once
// The G-buffer's colour attachments and how shaders read and write their channels. Generates
// shaders/generated/gbuffer.glslh. The file is macros only, so a shader may take it right after
// #version; GB_NORMAL and GB_PACK_SURFACE expand to common.glslh's oct_decode / oct_encode at
// the use site. render/gbuffer.cpp allocates the attachments and static_asserts the count.
#include <string>

namespace ve::layout {

struct GbAttachment {
	const char *name;
	const char *format;
	const char *channels;
};

inline constexpr GbAttachment kGbAttachments[] = {
	{"albedo", "R8G8B8A8_UNORM", "rgb = albedo, a = sun visibility"},
	{"surface", "R16G16B16A16_SFLOAT", "xy = oct normal, z = material id, w = gloss"},
};

inline constexpr int kGbColorAttachments =
		static_cast<int>(sizeof(kGbAttachments) / sizeof(kGbAttachments[0]));

struct GbMacro {
	const char *signature;
	const char *body;
};

inline constexpr GbMacro kGbMacros[] = {
	{"GB_ALBEDO(g0)", "((g0).rgb)"},
	{"GB_SUN_VIS(g0)", "((g0).a)"},
	{"GB_NORMAL(g1)", "oct_decode((g1).xy)"},
	{"GB_MATERIAL_ID(g1)", "uint((g1).z + 0.5)"},
	{"GB_IS_SURFACE(g1)", "((g1).z >= 0.5)"},
	{"GB_GLOSS(g1)", "((g1).w)"},
	{"GB_PACK_ALBEDO(rgb, sun_vis)", "vec4((rgb), (sun_vis))"},
	{"GB_PACK_SURFACE_OCT(oct, mat, gloss)", "vec4((oct), float(mat), (gloss))"},
	{"GB_PACK_SURFACE(n, mat, gloss)", "GB_PACK_SURFACE_OCT(oct_encode(n), (mat), (gloss))"},
};

// The exact contents of shaders/generated/gbuffer.glslh.
std::string gbuffer_glsl();

} // namespace ve::layout
```

`extension/src/gpu_layout/gbuffer_layout.cpp`:

```cpp
#include "gpu_layout/gbuffer_layout.h"

namespace ve::layout {

std::string gbuffer_glsl() {
	std::string out =
			"// GENERATED from extension/src/gpu_layout/gbuffer_layout.h by ve::layout::gbuffer_glsl().\n"
			"// Do not edit by hand: extension/tests/test_generated_glsl.cpp asserts this file byte\n"
			"// for byte. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests\n"
			"//\n"
			"// G-buffer colour attachments in framebuffer order; depth follows them.\n";
	for (int i = 0; i < kGbColorAttachments; i++)
		out += "//   " + std::to_string(i) + " " + kGbAttachments[i].name + " " +
				kGbAttachments[i].format + ": " + kGbAttachments[i].channels + "\n";
	out += "// Macros only. GB_NORMAL and GB_PACK_SURFACE need common.glslh's oct_decode and\n"
	       "// oct_encode in scope where they are used.\n\n";
	out += "#define GB_ATTACHMENTS " + std::to_string(kGbColorAttachments) + "\n\n";
	for (const GbMacro &m : kGbMacros)
		out += std::string("#define ") + m.signature + " " + m.body + "\n";
	return out;
}

} // namespace ve::layout
```

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
(cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc='generated: shaders/generated/gbuffer.glslh') 2>&1 | tail -2
(cd extension && scons -Q test) 2>&1 | tail -3
```

`extension/src/render/gbuffer.h`: replace the two comment rows `//   albedo   R8G8B8A8_UNORM …` and `//   surface  R16G16B16A16_SFLOAT …` with `//   albedo, surface  channel layout in gpu_layout/gbuffer_layout.h (generates shaders/generated/gbuffer.glslh)`.

`extension/src/render/gbuffer.cpp`: add `#include "gpu_layout/gbuffer_layout.h"` and at file scope `static_assert(ve::layout::kGbColorAttachments == 2, "GBuffer allocates albedo and surface: update ensure_owned/ensure_managed with the layout");`.

Commit:

```bash
git add extension/src/gpu_layout/gbuffer_layout.h extension/src/gpu_layout/gbuffer_layout.cpp shaders/generated/gbuffer.glslh \
	extension/src/render/gbuffer.h extension/src/render/gbuffer.cpp extension/tests/test_generated_glsl.cpp
git commit -m "feat: generated G-buffer channel accessors"
```

- [ ] **Step 3: Raster passes size attachments from the layout**

Add `#include "gpu_layout/gbuffer_layout.h"` to each file.

`composite_pass.cpp` `ensure_pipeline`: `state.color_attachments = want_marker ? 3 : 2;` → `state.color_attachments = ve::layout::kGbColorAttachments + (want_marker ? 1 : 0);`. In `draw()`, the two unconditional `clears.push_back(Color(0, 0, 0, 0));` → `for (int i = 0; i < ve::layout::kGbColorAttachments; i++) clears.push_back(Color(0, 0, 0, 0));`.

`lod_raster_pass.cpp`: the same two changes (`ensure_pipeline`, and the two commented pushes in `clear_targets`).

`grass_raster_pass.cpp` `ensure_pipeline`: `state.color_attachments = 2;` → `state.color_attachments = ve::layout::kGbColorAttachments;`.

Pass gate **composite** with extra suites `res://tests/test_lod_gbuffer.gd,res://tests/test_grass_golden.gd`; commit `refactor(raster): G-buffer attachment count from the layout`.

- [ ] **Step 4: Writers, one commit each**

In each shader: insert `#include "generated/gbuffer.glslh"` as the line after `#version`. Then:

`shaders/composite.frag.glsl` — `out_surface = vec4(sf.xy, sf.z, 0.0);` → `out_surface = GB_PACK_SURFACE_OCT(sf.xy, sf.z, 0.0);`; `out_albedo = vec4(mix(surf.rgb * mix(1.0, props.y, 0.65), ov.rgb, sf.w), ov.a);` → `out_albedo = GB_PACK_ALBEDO(mix(surf.rgb * mix(1.0, props.y, 0.65), ov.rgb, sf.w), ov.a);`; `out_surface = vec4(oct_encode(shading_n), sf.z, 1.0 - props.x);` → `out_surface = GB_PACK_SURFACE(shading_n, sf.z, 1.0 - props.x);`. (`sf` is the marcher's own surface target, not the G-buffer: its reads stay as they are.) Pass gate **composite**; commit.

`shaders/lod.frag.glsl` — `out_albedo = vec4(surf.rgb * mix(1.0, props.y, 0.65), 1.0);` → `out_albedo = GB_PACK_ALBEDO(surf.rgb * mix(1.0, props.y, 0.65), 1.0);`; `out_surface = vec4(oct_encode(shading_n), float(v_material), 1.0 - props.x);` → `out_surface = GB_PACK_SURFACE(shading_n, v_material, 1.0 - props.x);`. Pass gate **lod_raster**; commit.

`shaders/grass.frag.glsl` — `out_albedo = vec4(albedo, grass_sun_term(v_sun, v_height_t));` → `out_albedo = GB_PACK_ALBEDO(albedo, grass_sun_term(v_sun, v_height_t));`; `out_surface = vec4(oct_encode(n), float(MAT_GRASS_BLADE), pc.style.y);` → `out_surface = GB_PACK_SURFACE(n, MAT_GRASS_BLADE, pc.style.y);`. Pass gate **grass_raster**; commit.

- [ ] **Step 5: Readers, one commit each**

In each shader: the include as in Step 4, then replace the channel decodes.

`shaders/deferred.comp.glsl` — `uint mat = uint(g1.z + 0.5);` → `uint mat = GB_MATERIAL_ID(g1);`; `vec4(g0.rgb, 1.0)` → `vec4(GB_ALBEDO(g0), 1.0)`; `vec3 n = oct_decode(g1.xy);` → `vec3 n = GB_NORMAL(g1);`; `float shadow = g0.a;` → `float shadow = GB_SUN_VIS(g0);`; in the `cel_shade(` call, `g0.rgb` → `GB_ALBEDO(g0)` and `g1.w` → `GB_GLOSS(g1)`. Pass gate **deferred**; commit.

`shaders/ssao.comp.glsl` — `g1.z < 0.5` → `!GB_IS_SURFACE(g1)`; `oct_decode(g1.xy)` → `GB_NORMAL(g1)`. Pass gate **ssao**; commit.

`shaders/ssgi.comp.glsl` — `oct_decode(texture(gb_surface, suv).xy)` → `GB_NORMAL(texture(gb_surface, suv))`; `sg.z < 0.5` → `!GB_IS_SURFACE(sg)`; `uint(sg.z + 0.5)` → `GB_MATERIAL_ID(sg)`; `ng.z < 0.5` → `!GB_IS_SURFACE(ng)`; `g1.z < 0.5` → `!GB_IS_SURFACE(g1)`; `oct_decode(g1.xy)` → `GB_NORMAL(g1)`; any other `oct_decode(<x>.xy)` where `<x>` was read from `gb_surface` → `GB_NORMAL(<x>)`. Pass gate **ssgi**; commit.

`shaders/outline.comp.glsl` — `s.solid = g.z >= 0.5;` → `s.solid = GB_IS_SURFACE(g);`; any `oct_decode(g.xy)` → `GB_NORMAL(g)`. Pass gate **outline**; commit.

`shaders/ssr.comp.glsl` — `g.z >= 0.5` → `GB_IS_SURFACE(g)`; `oct_decode(g.xy)` → `GB_NORMAL(g)`; `g.w` → `GB_GLOSS(g)`. Pass gate **ssr**; commit.

After each file:

```bash
rg -n "texture\(gb_surface|texelFetch\(gb_surface|texture\(gb_albedo|texelFetch\(gb_albedo" shaders/<file>
```
and check every variable those reads land in is decoded only through `GB_*` (`rg -n "\b<var>\.(x|y|z|w|xy|rgb|a)\b" shaders/<file>` prints nothing).

---

### Task 40: Generated cel constants

**Files:**
- Create: `extension/src/gpu_layout/cel_emit.h`, `extension/src/gpu_layout/cel_emit.cpp`
- Create: `shaders/generated/cel.glslh`, `shaders/generated/cel_constants.gdshaderinc` (generated)
- Modify: `shaders/shade.glslh`, `shaders/cel.gdshaderinc`
- Test: `extension/tests/test_generated_glsl.cpp`

**Interfaces:**
- Consumes: `ve::CelParams`, `ve::kCelBands` (`shade/cel.h`); Task 32's `check_generated`.
- Produces: `std::string ve::layout::glsl_float(float)`, `cel_glsl()`, `cel_gdshaderinc()`.

- [ ] **Step 1: Failing tests**

Append to `extension/tests/test_generated_glsl.cpp` (add `#include "gpu_layout/cel_emit.h"`, `#include "shade/cel.h"`, `#include <cstdlib>`):

```cpp
TEST_CASE("glsl_float round-trips and always reads as a float literal") {
	const ve::CelParams p;
	const float values[] = {p.band_edge[0], p.band_edge[1], p.band_edge[2], p.band_level[0],
		p.band_level[3], p.shadow_hue_shift, p.shadow_saturation, p.spec_edge, p.spec_strength,
		p.rim_strength, p.rim_power, 1.0f, 3.0f};
	for (float v : values) {
		const std::string s = ve::layout::glsl_float(v);
		CHECK_MESSAGE(std::strtof(s.c_str(), nullptr) == v, s);
		CHECK_MESSAGE(s.find_first_of(".eE") != std::string::npos, s);
	}
	CHECK(ve::layout::glsl_float(0.08f) == "0.08");
	CHECK(ve::layout::glsl_float(1.0f) == "1.0");
}

TEST_CASE("generated: shaders/generated/cel.glslh") {
	check_generated("shaders/generated/cel.glslh", ve::layout::cel_glsl());
}

TEST_CASE("generated: shaders/generated/cel_constants.gdshaderinc") {
	check_generated("shaders/generated/cel_constants.gdshaderinc", ve::layout::cel_gdshaderinc());
}
```

- [ ] **Step 2: Emitter**

`extension/src/gpu_layout/cel_emit.h`:

```cpp
#pragma once
// ve::CelParams' defaults as GLSL constants for the compute shaders (shade.glslh) and as Godot
// shader-language constants for cel-shaded objects (cel.gdshaderinc), so the three copies of
// the ramp -- C++, GLSL, gdshader -- are one.
#include <string>

namespace ve::layout {

// The shortest %g form that parses back to exactly `v`, always containing '.', 'e' or 'E'.
std::string glsl_float(float v);

std::string cel_glsl();        // shaders/generated/cel.glslh
std::string cel_gdshaderinc(); // shaders/generated/cel_constants.gdshaderinc

} // namespace ve::layout
```

`extension/src/gpu_layout/cel_emit.cpp`:

```cpp
#include "gpu_layout/cel_emit.h"
#include "shade/cel.h"
#include <cstdio>
#include <cstdlib>

namespace ve::layout {

namespace {

const char *kHeader =
		"// Do not edit by hand: extension/tests/test_generated_glsl.cpp asserts this file byte\n"
		"// for byte. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests\n";

std::string list(const float *v, int n) {
	std::string out;
	for (int i = 0; i < n; i++) out += (i ? ", " : "") + glsl_float(v[i]);
	return out;
}

} // namespace

std::string glsl_float(float v) {
	char buf[32];
	for (int digits = 6; digits <= 9; digits++) {
		std::snprintf(buf, sizeof(buf), "%.*g", digits, static_cast<double>(v));
		if (std::strtof(buf, nullptr) == v) break;
	}
	std::string s(buf);
	if (s.find_first_of(".eE") == std::string::npos) s += ".0";
	return s;
}

std::string cel_glsl() {
	const CelParams p;
	const std::string edges = std::to_string(kCelBands - 1);
	const std::string bands = std::to_string(kCelBands);
	return std::string("// GENERATED from extension/src/shade/cel.h (ve::CelParams) by ve::layout::cel_glsl().\n") +
			kHeader + "\n" +
			"const int CEL_BANDS = " + bands + ";\n" +
			"const float CEL_BAND_EDGE[" + edges + "] = float[" + edges + "](" + list(p.band_edge, kCelBands - 1) + ");\n" +
			"const float CEL_BAND_LEVEL[" + bands + "] = float[" + bands + "](" + list(p.band_level, kCelBands) + ");\n" +
			"const float CEL_SHADOW_HUE_SHIFT = " + glsl_float(p.shadow_hue_shift) + ";\n" +
			"const float CEL_SHADOW_SATURATION = " + glsl_float(p.shadow_saturation) + ";\n" +
			"const float CEL_SPEC_EDGE = " + glsl_float(p.spec_edge) + ";\n" +
			"const float CEL_SPEC_STRENGTH = " + glsl_float(p.spec_strength) + ";\n" +
			"const float CEL_RIM_STRENGTH = " + glsl_float(p.rim_strength) + ";\n" +
			"const float CEL_RIM_POWER = " + glsl_float(p.rim_power) + ";\n";
}

std::string cel_gdshaderinc() {
	const CelParams p;
	std::string out = std::string("// GENERATED from extension/src/shade/cel.h (ve::CelParams) by ve::layout::cel_gdshaderinc().\n") +
			kHeader + "\n";
	for (int i = 0; i < kCelBands - 1; i++)
		out += "const float VE_CEL_BAND_EDGE_" + std::to_string(i) + " = " + glsl_float(p.band_edge[i]) + ";\n";
	for (int i = 0; i < kCelBands; i++)
		out += "const float VE_CEL_BAND_LEVEL_" + std::to_string(i) + " = " + glsl_float(p.band_level[i]) + ";\n";
	out += "const float VE_HUE = " + glsl_float(p.shadow_hue_shift) + ";\n";
	out += "const float VE_SAT = " + glsl_float(p.shadow_saturation) + ";\n";
	out += "const float VE_SPEC_EDGE = " + glsl_float(p.spec_edge) + ";\n";
	out += "const float VE_SPEC = " + glsl_float(p.spec_strength) + ";\n";
	out += "const float VE_RIM = " + glsl_float(p.rim_strength) + ";\n";
	out += "const float VE_RIM_POWER = " + glsl_float(p.rim_power) + ";\n";
	return out;
}

} // namespace ve::layout
```

```bash
(cd extension && scons -Q test) 2>&1 | tail -3
(cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc='generated: shaders/generated/cel*') 2>&1 | tail -2
(cd extension && scons -Q test) 2>&1 | tail -3
cat shaders/generated/cel.glslh
```
Expected: all pass; `CEL_BAND_EDGE[3] = float[3](0.08, 0.32, 0.66)`, `CEL_BAND_LEVEL[4] = float[4](0.18, 0.45, 0.75, 1.0)`.

- [ ] **Step 3: Consumers**

`shaders/shade.glslh`: under `// ---- ve::CelParams / ve::cel_shade ---`, replace the nine `const` lines (`CEL_BANDS` through `CEL_RIM_POWER`) with `#include "generated/cel.glslh"`.

`shaders/cel.gdshaderinc`: replace its first six lines (`const float VE_HUE = 0.055;` through `const float VE_RIM_POWER = 3.0;`) with `#include "res://shaders/generated/cel_constants.gdshaderinc"`, and replace `ve_cel_level` with:

```glsl
float ve_cel_level(float ndl) {
	float v=clamp(ndl,0.0,1.0); if(v>VE_CEL_BAND_EDGE_2)return VE_CEL_BAND_LEVEL_3;
	if(v>VE_CEL_BAND_EDGE_1)return VE_CEL_BAND_LEVEL_2;
	if(v>VE_CEL_BAND_EDGE_0)return VE_CEL_BAND_LEVEL_1; return VE_CEL_BAND_LEVEL_0;
}
```

```bash
rg -n "0\.055|1\.35|0\.72|0\.45|0\.35|0\.08|0\.32|0\.66|0\.18|0\.75" shaders/shade.glslh shaders/cel.gdshaderinc
```
Expected: no constant definitions left (a comment hit is fine; name it in the commit message).

- [ ] **Step 4: Verify**

Pass gate for **deferred** with extra suites `res://tests/test_cel_object.gd,res://tests/test_island_body.gd`. `test_deferred.gd`'s band-edge GPU-vs-CPU diff and `test_cel_object.gd::test_shaderlanguage_matches_ve_cel_shade` are the checks. If Godot created `shaders/generated/cel_constants.gdshaderinc.uid`, add it.

- [ ] **Step 5: Commit**

```bash
git add extension/src/gpu_layout/cel_emit.h extension/src/gpu_layout/cel_emit.cpp \
	shaders/generated/cel.glslh shaders/generated/cel_constants.gdshaderinc shaders/generated/*.uid \
	shaders/shade.glslh shaders/cel.gdshaderinc extension/tests/test_generated_glsl.cpp \
	docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "feat: cel ramp constants generated into GLSL and the object shader include"
```

---

### Task 41: Exit criteria, results report, spec and roadmap amendments

No production change.

**Files:**
- Create: `docs/superpowers/plans/2026-09-15-pass-anatomy-results.md`
- Modify: `docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md` (status, planning decisions)
- Modify: `docs/superpowers/plans/2026-09-13-frame-module.md` (Sub-project 4 status), `docs/superpowers/specs/2026-09-13-frame-module-design.md` (§10 S4/S7/S9 rows)

**Interfaces:**
- Consumes: every earlier task's commits and the evidence log.
- Produces: the results report.

- [ ] **Step 1: Exit criteria**

```bash
rg -n "shader_compile_spirv_from_source" extension/src -g '*.cpp'
rg -n 'invalidate_uniform_set|static_assert\(sizeof\(float\) \*|#define MATERIAL_LAYERS|GRASS_MATERIAL' extension/src shaders
rg -n 'key_[a-z_]+_ = |uset_[a-z_]+_ = |fb_[a-z]+_ = ' extension/src/render
ls shaders/generated/
rg -n "TEST_CASE\(\"generated: " extension/tests/test_generated_glsl.cpp
rg -n "the committed GLSL mirror matches the C\+\+ table" extension/tests/test_material_glslh.cpp
```
Expected: the first prints exactly one line in `render/gpu/gpu.cpp`; the second and third print nothing; every `.glslh`/`.gdshaderinc` in `shaders/generated/` (besides `field.glslh.golden`, which has its own test) has a `generated:` case; the material mirror test exists.

- [ ] **Step 2: Full regression run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -3; echo "build=$?"
(cd extension && scons -Q test) 2>&1 | tail -3; echo "native=$?"
./gdunit_tests.sh 2>&1 | tee .superpowers/sdd/2026-09-15-pass-anatomy/final.log | tail -5
grep -c " leaked" .superpowers/sdd/2026-09-15-pass-anatomy/final.log
latest=$(ls -d reports/report_* | sort -V | tail -1); python3 .superpowers/sdd/2026-09-15-pass-anatomy/failures.py "$latest/results.xml"
```
Compare with Task 1's baseline: the failing set may differ only by the S4 and S7 tests now passing; the leak count may not rise.

- [ ] **Step 3: Change-cost retraces**

Trace each scenario on the final tree by listing the files a change would touch, reading the code, not guessing:

1. **New material "moss"** (spec target ≤ 4): PNGs under `assets/materials/`, `tools/convert_materials.sh`, a `kMaterials` row, and the regenerated `shaders/material_table.glslh`; placing it procedurally adds a stage line using `MAT_MOSS` and `ve::material_id("moss")`.
2. **New G-buffer channel** (≤ 5): `gpu_layout/gbuffer_layout.h`, regenerated `generated/gbuffer.glslh`, `render/gbuffer.{h,cpp}`, the writer(s), the reader(s).
3. **SP2's `FogPass`** (was 9): with the helper, the pass `.h/.cpp` no longer carries compile or cache code; re-count including `gpu_timings.cpp`.

Record each as a numbered file list and a count. A count over target is **OPEN**, stated as such, with the file that keeps it over.

- [ ] **Step 4: Write the results report**

`docs/superpowers/plans/2026-09-15-pass-anatomy-results.md`:

```markdown
# Pass anatomy and generated layouts — measured results

Recorded <date> on <OS / GPU>. Implementation head <sha>. Baseline <Task 1 sha>.

## 1. Verification
<build / native / gdUnit output lines and exit codes from Step 2>
<failing-set comparison with the baseline; leak counts baseline -> final>

## 2. Exit criteria
| Criterion (spec §6) | Result | Evidence |
|---|---|---|
| One compile site | <PASS/OPEN> | <Step 1 output> |
| invalidate_uniform_set, tautological asserts, MATERIAL_LAYERS defines, GRASS_MATERIAL gone | … | … |
| No hand-written key caches | … | … |
| Every generated file byte-tested | … | … |
| New material ≤ 4 files | … | <file list> |
| New G-buffer channel ≤ 5 files | … | <file list> |
| FogPass retrace | <count> | <file list> |
| gdUnit no worse than baseline; moved pins attributed | … | … |
| S7, S9 closed; S4 sun/ambient closed, albedo remainder open | … | … |

## 3. Pinned-value attribution
<every golden or test value that changed, with commit and cause; "none" for migrations>
<field.glslh.golden text changes in Tasks 37 and 38, quoted>

## 4. Bite proofs
<summary table from Tasks 2–4>

## 5. Suspected bugs
| Id | Result | Evidence |
|---|---|---|
| S4 | Sun direction, colour, ambient: FIXED. Island rock albedo: OPEN (IslandBody has no material data) | <commits> |
| S7 | FIXED | <commits, test name> |
| S9 | CLOSED: asserts deleted, values generated | <commit> |

## 6. Deletion and size
<wc -l of every pass .cpp before (baseline sha) and after; the helper's size>
```bash
git diff --stat <baseline sha>..HEAD -- extension/src/render | tail -1
```

## 7. Open findings
<anything OPEN, with the file or reason>
```
Paste real output everywhere.

- [ ] **Step 5: Amend the spec and roadmap**

- Spec (`docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md`): set **Status** to `Implemented; see docs/superpowers/plans/2026-09-15-pass-anatomy-results.md`; append a section `## Decisions made during planning` with this plan's fifteen decisions.
- Roadmap plan (`docs/superpowers/plans/2026-09-13-frame-module.md`, "Sub-project 4"): add a status line under the heading naming the results report and any OPEN exit criterion.
- Roadmap spec (`docs/superpowers/specs/2026-09-13-frame-module-design.md` §10): append to the S4, S7 and S9 rows the result from §5 of the report.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/plans/2026-09-15-pass-anatomy-results.md \
	docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md \
	docs/superpowers/plans/2026-09-13-frame-module.md docs/superpowers/specs/2026-09-13-frame-module-design.md \
	docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md
git commit -m "docs: pass anatomy results, exit criteria and roadmap amendments"
```

---

## Acceptance checklist

- [ ] Baseline recorded; SP2's runtime gates checked (Task 1).
- [ ] Every one of the 23 sites has a Site row and a bite proof (Tasks 2–4).
- [ ] S7 and S4 each have a failing-test commit before their fix commit (Tasks 5–6).
- [ ] `gpu_core.h` is pure and its fake-device tests pass (Task 7).
- [ ] All 23 sites migrated, one commit each, every Pass gate matching its row (Tasks 9–31).
- [ ] `shader_compile_spirv_from_source` appears once, in `render/gpu/gpu.cpp`; `invalidate_uniform_set` is gone.
- [ ] Every push/UBO block is generated and checked against `offsetof`; no hand-offset packing remains (Tasks 32–36).
- [ ] Beauty flags, material layers, strides, material names, G-buffer channels and cel constants are generated and byte-tested (Tasks 37–40).
- [ ] No float baseline under `extension/tests/golden/` changed; `field.glslh.golden` text changes are attributed.
- [ ] Results report written; spec and roadmap amended (Task 41).

