# Voxel Everything — Stage Authoring (Sub-project 6)

**Date:** 2026-09-17
**Status:** Specified; not yet implemented
**Roadmap:** the pathway in `docs/superpowers/plans/2026-09-13-frame-module.md`
("Sub-project 6 — Stage authoring"). Domain nouns: `CONTEXT.md`.
**Prior spec:** `docs/superpowers/specs/2026-09-03-terrain-pipeline-design.md` — §4 (manifest
format), §10.1 (Lipschitz violation), §11 (testing). This spec amends §4 and closes §10.1.
**Start commit:** `b03e5d6`.

**Goal.** A terrain artist adds a stage in ≤ 3 files, with names rather than positions, on both
CPU and GPU, and the tools refuse an unsafe Lipschitz budget.

---

## 1. Problem (as of `b03e5d6`)

The terrain pipeline landed in the 2026-09-03 milestone and works: manifests parse, pipelines
resolve, `field.glslh` is generated, and `PipelineFieldGenerator` walks the same stage list on the
CPU. Four problems block a second author from using it.

- **The mirror binds by position.** `builtin_stages.cpp` reads `s.extra[0]`, `s.extra[1]`,
  `p.at(0)`. The ordering rule ("this stage's declared WRITES in order, then its READS in order",
  with `sdf` declared both ways collapsing to one slot) is carried in comments at three call sites.
  Reordering a `//!out` line silently rebinds every later index; nothing fails.
- **Cross-stage params are hardcoded twice.** `cave.field.glslh` reads `P.hills_amp_a/b/c`, which
  the resolver never sees as a dependency. Its CPU mirror hardcodes `6.0f`, `3.0f`, `1.0f` —
  literals that only match by luck. `assets/pipelines/default.pipeline` carries the footgun as a
  comment: *"overriding hills amp_* also requires updating the literals in stage_cave's CPU
  mirror."* Overriding `amp_a` in a pipeline file diverges CPU from GPU, silently.
- **The Lipschitz bound is a hand-asserted constant, and the combination rule is dead code.**
  `pipeline.cpp:141` combines stage bounds multiplicatively (`lip *= m.lipschitz`), which is wrong
  for an additive stage. It does not matter today, because `pipeline.cpp:161` lets the pipeline
  file *replace* the computed bound and both shipped pipelines declare `lipschitz 2.0`. The real
  budget lives in a comment block and `extension/tests/test_relief_lipschitz.cpp`, worked by hand.
  Understating the bound is a correctness bug — `raycast.cpp` steps by `1 / lipschitz()` and would
  tunnel through a surface — and nothing in the tools would catch it.
- **The load is written four times and the generator seam has a redundant layer.** The
  read → parse → per-stage read/parse → resolve sequence appears in `voxel_world.cpp:561-615`,
  `test_pipeline_equivalence.cpp:30-47` and `test_field_codegen_golden.cpp:30-46`. `FieldGenerator`
  wraps a `Generator` and every one of ~30 consumers reaches through `generator()->sampler()`.
  `ProceduralFieldGenerator` exists only to wrap `AnalyticGenerator`, which is the fourth copy of
  the terrain that Appendix A of the frame-module spec names.

**Change cost today** (frame-module spec Appendix A): a new terrain stage touches 5–7 files; a
material with hardness placed by terrain touches ~9.

## 2. Decisions

| Decision | Why |
|---|---|
| The mirror **declares** its slot and param names through a registering macro; a name-resolved binding replaces `extra[i]` / `p.at(i)` | No generated file and no regeneration step, so a new stage stays at 3 files. A manifest/mirror mismatch fails loudly at `create`, not silently at `extra[2]`. |
| `//!lipschitz` gains a required mode word: `add` or `mul` | An additive stage adds to the gradient bound; a composing stage multiplies. One number with no mode cannot express both. |
| The `.pipeline` `lipschitz` line becomes a **ceiling**, not an override | Resolve reports the computed bound. Exceeding the ceiling fails the load, naming each stage's contribution. The artist's number can no longer be the bound the raymarcher trusts. |
| `//!use <stage>.<param>` declares a cross-stage read; any undeclared `P.<ident>` in a body is rejected at resolve | Makes the dependency visible to the resolver, so the value flows to the CPU blob and the literals die. |
| The §10.1 sampled violation check is a **native test**, not a runtime debug mode | Central differences over the CPU mirror cost nothing and run every build; a debug mode only bites when someone opens it. |
| `mesas` lands as its own pipeline file, not in `default.pipeline` | Proves the ≤ 3-file count and gives `test_field_diff.gd` the multi-channel fixture §11.1 asks for, without moving demo terrain or any golden. |
| A failed pipeline load aborts world init instead of falling back to analytic terrain | See §7. |

**Milestone numbering.** This spec resequences the roadmap's six milestones so the behaviour fix
lands before the refactors that touch the same function:

| Here | Roadmap milestone |
|---|---|
| M1 | 1 — one `load_pipeline` |
| M2 | 4 — Lipschitz combination rule (moved earlier) |
| M3 | 2 — per-stage names in the mirrors |
| M4 | 3 — cross-stage parameter reads declared or rejected |
| M5 | 5 — `allow_gpu_only` warning |
| M6 | 6 — `FieldGenerator` + `View` deleted |
| M7 | — the Appendix A re-trace (new; the exit criteria require it) |

## 3. Entry gate

Sub-project 4(b) is accepted and the `MAT_*` generator exists (`shaders/material_table.glslh`
emits `MAT_ROCK`, `MAT_GRASS_01`, …; `height_bands.field.glslh` already uses them). SP4's open
exit findings are all in `render/`; none touch terrain.

Three gate commits, no production change:

- **G1 — baseline.** Re-record the gdUnit failure set at `b03e5d6` using Task 1's procedure from
  the frame-module plan → `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md`. The set
  drifts; an older baseline is stale by default.
- **G2 — `test_field_diff.gd` pipeline-parameterised.** It diffs only the pipeline the world
  happened to load. Enumerate `assets/pipelines/*.pipeline` with `DirAccess` and run all six op
  scenarios per pipeline inside one test function, failure messages naming the pipeline. Each
  pipeline needs its own `VoxelWorld` (the load is first-wins and fresh-init-only), set through the
  existing `terrain_pipeline_path` property before `debug_init_atlas()`. One world init per
  pipeline, not per scenario. Adding a pipeline then needs no test edit.
- **G3 — a CPU pin over `default.pipeline`.** Only `golden.pipeline` is pinned today, against
  `AnalyticGenerator`, and relief already broke that equivalence for `default`. Add
  `extension/tests/test_default_pipeline_field.cpp` + `tests/golden/default_pipeline_field.txt` in
  the existing corpus format (`tests/golden/field_baseline.txt`: `x y z sdf-bits material`, hex
  float bits, regenerated with `VE_REGEN_GOLDEN=1`). Mark `test_pipeline_resolve.cpp:119`
  ("lipschitz combines multiplicatively") as the behaviour milestone 4 changes.

Prove each gate test bites by breaking the code on purpose before moving on.

## 4. Milestones

Ordered so the behaviour fix lands before the refactors that touch `resolve_pipeline`, per the
roadmap's "bugs are fixed in their own commits, never inside a move."

### M1 — one loader

`extension/src/terrain/pipeline_load.{h,cpp}`, pure C++ (no godot-cpp), so it stays in the native
suite. `SConstruct` already globs `src/terrain/*.cpp`; no build edit.

```cpp
// Reads "<pipeline_path>" and each stage it names, relative to the shader root the reader
// resolves. The reader is the only Godot-aware part, supplied by the caller.
using TextReader = std::function<bool(const std::string &path, std::string *out)>;

bool load_pipeline(const TextReader &reader, const std::string &pipeline_path,
                   ResolvedPipeline *out, std::string *error);
```

Callers: `VoxelWorld::load_terrain_pipeline` (a `FileAccess` reader), `test_pipeline_equivalence`,
`test_field_codegen_golden`, G3's new test, and M4's sampled check (the latter two with an
`ifstream` reader). Five call sites, one implementation. M5 adds the warnings out-param. `voxel_world.cpp` keeps only the reader,
the generated-`field.glslh` install and the store writes.

### M2 — the Lipschitz rule (a `fix:` commit)

**Manifest.** `//!lipschitz <add|mul> <n>`. The bare `//!lipschitz <n>` form is rejected with a
message naming the file, so every stage states its mode explicitly.

**Validation, at resolve:**

- A stage that writes `sdf` must declare a mode; one that does not must not declare one. Either
  violation fails the load, naming the stage.
- The first `sdf`-writing stage must be `add` — it establishes the field, and `0 * n` is not a
  bound.

**Combination,** in pipeline order from `L = 0`, over `sdf`-writing stages only:
`add` → `L += n`; `mul` → `L *= n`.

**Ceiling.** `ResolvedPipeline::lipschitz` is always the computed `L`. `PipelineDesc::lipschitz_override`
is renamed `lipschitz_ceiling`; when non-zero and `L > ceiling`, resolve fails with a message
listing each stage's mode and contribution. An absent line means no ceiling check.

**Shipped numbers move.** Each stage now declares the Lipschitz constant of its own contribution
to `sdf`:

| Stage | Today | New | Derivation |
|---|---|---|---|
| `hills` | `2.0` | `add 1.78` | `sdf = y - SURFACE_Y - h`; `\|∂h/∂x\| ≤ 6(0.11)+3(0.031)+1(0.23) = 0.983`, `\|∂h/∂z\| ≤ 6(0.13)+3(0.043)+1(0.19) = 1.099`; `√(1+0.983²+1.099²) = 1.7816` |
| `relief` | `1.0` | `add 0.21` | additive term `-r`; `\|∂r/∂x\| ≤ 250(0.0004)+60(0.00083333) = 0.15`, same in z; `√(0.15²+0.15²) = 0.2121` |
| `cave` | `1.0` | `mul 1.0` | `max(sdf, -sphere)`; the sphere's unit gradient is below the running bound, so `max` cannot raise it |
| `height_bands` | `1.0` | none | writes only `material` |

`default.pipeline` resolves to **1.99** (ceiling 2.0); `golden.pipeline` to **1.78** (ceiling 2.0).
The exact joint bound for `default` is `√(1+1.133²+1.249²) = 1.9606`, so the additive rule is
conservative by 1.5% — the right direction, since understating tunnels.

**The §10.1 sampled check** lands here as `extension/tests/test_lipschitz_rule.cpp`: for every
shipped pipeline, sample the CPU mirror at 4096 deterministic points, take central differences of
`sdf`, and assert `|grad| ≤` the reported bound. This is what catches a stage whose declared number
is a lie, which no amount of combination-rule correctness can.

`test_relief_lipschitz.cpp` loses its hand-worked budget arithmetic — the resolver computes it now
— and keeps its two non-Lipschitz cases (4 km swing, zero at the origin).

### M3 — named slots and params

`stage_library.h` gains a macro pair that declares the struct *and* registers the name list:

```cpp
VE_STAGE_SLOTS(hills, sdf, height);           // struct HillsSlots  { int sdf, height; };
VE_STAGE_PARAMS(hills, amp_a, amp_b, amp_c);  // struct HillsParams { float amp_a, amp_b, amp_c; };

void stage_hills(FieldCtx &ctx, const HillsSlots &s, const HillsParams &p, const FieldResources &) {
    const float x = ctx.v(s.p)[0], y = ctx.v(s.p)[1], z = ctx.v(s.p)[2];
    ctx.f(s.height) = p.amp_a * sinf(x * 0.11f) + ...;
    ctx.f(s.sdf)    = y - kSurfaceY - ctx.f(s.height);
}
VE_REGISTER_STAGE("ve::stage_hills", hills, stage_hills);
```

`StageSlots` keeps `p`, `sdf` and `material` as built-ins available to every stage; the per-stage
struct carries the stage's own declared names.

**Binding.** `PipelineFieldGenerator::create` builds one packed blob per stage by resolving each
*registered name*: a slot name through `ResolvedPipeline::channel_slot(name)`, a param name
through the resolved param list. A name the manifest does not declare fails `create` with a
message naming the stage and the name. `VE_REGISTER_STAGE` emits a trampoline of the existing
`StageFn` shape that casts the two blobs; the blobs are built once at `create`, so per-sample cost
is unchanged.

The `extra[]` ordering rule and its three comment blocks are deleted — the ordering no longer
exists to get wrong. One stage per commit, `default_pipeline_field.txt` and
`field.glslh.golden` unchanged by each. The generated GLSL is untouched: this milestone is CPU-side
only.

### M4 — declared cross-stage reads

`//!use <stage>.<param>` joins the manifest directives. At resolve, each stage body is scanned
(with `//` comments stripped) for `P.<ident>` tokens; every one must be either the stage's own
param (`<stage>_<name>`) or a declared `//!use`, and the named stage must be present in the
pipeline. Anything else fails the load, naming the token and the stage.

`cave.field.glslh` declares `//!use hills.amp_a`, `amp_b`, `amp_c`. Its GLSL body is unchanged —
params already flatten to `hills_amp_a` in the UBO. Its mirror gains the three names:

```cpp
VE_STAGE_PARAMS(cave, cx, cz, depth, radius, hills_amp_a, hills_amp_b, hills_amp_c);
```

and the three hardcoded literals die. Name resolution tries `<this stage>.<name>` first, then
matches `<name>` against the flattened `<stage>.<param>` list with `.` → `_`; both are
deterministic and unambiguous. The footgun comment in `default.pipeline` is deleted along with
the bug, and a native test overrides `hills.amp_a` in a pipeline and asserts the cave mirror sees
the override.

### M5 — `allow_gpu_only` becomes loud

`ResolvedPipeline` gains `std::vector<std::string> warnings`. A stage with no `//!cpu` mirror
under `allow_gpu_only` pushes a warning naming the CPU consumers that will diverge from the GPU:
brick meshing (`brick_eval`), island extraction, raycast, and consolidation. `load_pipeline`
returns them; `VoxelWorld::load_terrain_pipeline` pushes each through `push_warning`. The load
still succeeds — `allow_gpu_only` is a deliberate opt-in — but it stops being silent.

### M6 — delete the redundant seam

Two commits.

1. **`PipelineFieldGenerator` becomes a `Generator`.** The inner `View` class, its `owner_`
   back-pointer and `FieldGenerator::eval`/`sampler` go; `sample`, `sample_gradient` and
   `lipschitz` move onto the class itself. `FieldGenerator` and `ProceduralFieldGenerator` are
   deleted with `extension/src/generator/field_generator.h`. `WorldStore` holds a
   `ve::Generator *` and keeps returning a pointer (not a reference) so the existing null check at
   `hooks_world.cpp:1238` still works. The ~30 `generator()->sampler()` call sites across
   `hooks_*.cpp`, `mesh/consolidation.cpp`, `voxel_world.cpp` and `world_store.h:145` become
   `*generator()`.
2. **`AnalyticGenerator` moves to `extension/tests/`** as the equivalence oracle only.
   `generator.{h,cpp}` keep `Generator`, `Sample`, `FieldSample` and `kSurfaceY`. The `tests/*.cpp`
   glob picks the oracle up with no build edit; `test_pipeline_equivalence`, `test_field_baseline`
   and `test_brick_baseline` are its only users.

### M7 — the traced scenario

`shaders/stages/mesas.field.glslh`, its mirror in `builtin_stages.cpp`, and
`assets/pipelines/mesas.pipeline` — **3 files**. The stage reads `height`, writes `sdf`, writes a
new `mesa_mask : float` channel, and writes `material` (plateau tops to `MAT_ROCK` through the
generated constants), so it exercises channel extension, an additive Lipschitz declaration and
material placement in one stage — and so G2 picks it up as the multi-channel field-diff fixture
§11.1 of the terrain spec asks for. `mesas.pipeline` is `hills, relief, cave, height_bands, mesas`.
Not added to `default.pipeline`: demo terrain and every golden stay where they are.

Re-trace "material with hardness placed by terrain" against the `MAT_*` constants SP4 shipped, and
record both counts in the results report.

## 5. Interfaces changed

| Symbol | Before | After |
|---|---|---|
| `StageManifest::lipschitz` | `float` | `float` + `LipschitzMode {kNone, kAdd, kMul}` |
| `PipelineDesc::lipschitz_override` | replaces the computed bound | renamed `lipschitz_ceiling`; checked against it |
| `StageManifest` | — | gains `std::vector<std::string> uses` |
| `ResolvedPipeline` | — | gains `std::vector<std::string> warnings` |
| `StageSlots::extra[]` | per-stage resolved indices | deleted; per-stage structs |
| `StageParams::at(i)` | positional | deleted; per-stage structs |
| `VE_REGISTER_STAGE(symbol, fn)` | 2 args | `(symbol, stage, fn)`; emits the trampoline |
| `FieldGenerator`, `ProceduralFieldGenerator` | the seam | deleted |
| `WorldStore::generator()` | `FieldGenerator *` | `ve::Generator *` |
| `AnalyticGenerator` | `src/generator/` | `extension/tests/` |

## 6. Tests

| Test | Kind | Milestone | Asserts |
|---|---|---|---|
| `test_field_diff.gd` (rewritten) | gdUnit | G2 | CPU ≡ GPU for every pipeline in `assets/pipelines/`, all six op scenarios |
| `test_default_pipeline_field.cpp` + golden | native | G3 | `default.pipeline` CPU field, pinned by corpus |
| `test_lipschitz_rule.cpp` | native | M2 | the combine rule; mode validation errors; ceiling rejection; sampled `\|grad\| ≤` reported bound for every shipped pipeline |
| `test_pipeline_resolve.cpp` (amended) | native | M2, M4 | multiplicative case replaced by add/mul; undeclared `P.<ident>` rejected; `//!use` of an absent stage rejected |
| `test_stage_bindings.cpp` | native | M3 | every shipped pipeline's stages resolve their registered names; an unknown name fails `create` with the stage named |
| `test_cross_stage_param.cpp` | native | M4 | a `hills.amp_a` override reaches `stage_cave`'s mirror |
| `test_pipeline_equivalence.cpp` (amended) | native | M1, M2 | uses `load_pipeline`; `lipschitz() <= AnalyticGenerator::lipschitz()` |
| `test_field_codegen_golden.cpp` (amended) | native | M1 | uses `load_pipeline`; golden unchanged |
| `test_pipeline_parse.cpp` (amended) | native | M2 | `lipschitz_ceiling` replaces `lipschitz_override` |
| `test_stage_manifest.cpp` (amended) | native | M2, M4 | `//!lipschitz <mode> <n>` parses; bare form rejected; `//!use` parses |
| `test_relief_lipschitz.cpp` (trimmed) | native | M2 | 4 km swing and zero-at-origin only |

## 7. Failure behaviour change

With `AnalyticGenerator` out of `src`, there is no fallback terrain. A failed pipeline load
therefore **aborts world init** — `load_terrain_pipeline` returns false, `ensure_initialized`
bails with the error already pushed — instead of silently generating *different terrain than the
pipeline says*. That contradicts the comment at `voxel_world.cpp:553` ("a bad pipeline must
degrade, never kill the world"), which is updated in the same commit.

This is deliberate: silent divergence between the declared field and the generated one is the
exact hazard the terrain-pipeline design exists to prevent, and it kills one of the four terrain
copies Appendix A names. The alternative — keeping `AnalyticGenerator` in `src` purely as a
fallback — leaves M6 unfinished.

The `field.glslh` stub (the third copy) is out of scope; it is not named in this sub-project's
exit criteria.

## 8. Exit criteria

- `rg 'extra\[[0-9]\]|p\.at\([0-9]\)' extension/src/terrain` returns nothing.
- `rg 'class FieldGenerator|ProceduralFieldGenerator|AnalyticGenerator' extension/src` returns
  nothing.
- `rg 'sampler\(\)' extension/src` hits only `render/material_atlas.h` and its render consumers.
- Appendix A "new terrain stage" ≤ 3 files (from 5–7), re-traced by M7 and recorded.
- Appendix A "material with hardness placed by terrain" ≤ 4 files (from ~9), re-traced and
  recorded.
- Every shipped pipeline passes the sampled Lipschitz check.
- Native suite and gdUnit match the G1 baseline, case name *and* message, with no suite's case
  count dropping.

## 9. Risks

| Risk | Handling |
|---|---|
| The reported bound drops (2.0 → 1.99 for `default`, 2.0 → 1.78 for `golden`), so raycast steps grow and `mesh_chunk` padding thins | The two committed corpora are `AnalyticGenerator`-pinned and hold. Raycast, mesh and collider gdUnit suites may move: apply the golden policy — attribute every moved number to this cause, re-record in the same commit, name the cause in the message. If a diff looks like a shipped bug, stop and report. |
| `test_pipeline_equivalence.cpp:92` asserts the pipeline bound *equals* `AnalyticGenerator`'s 2.0 | Becomes `<=`, which is the safe direction, in the M2 commit with the reason stated. |
| The `P.<ident>` body scan false-positives on a param name inside a comment | Comments are stripped before the scan; the manifest directive block is already excluded from `body`. A native case covers a param name mentioned in a comment. |
| A gdUnit suite depended on fallback terrain after a bad pipeline path and now hits a null generator | Every suite uses the default path, which loads. The existing null check at `hooks_world.cpp:1238` is preserved by keeping `generator()` a pointer. Verified against the G1 baseline. |
| `VE_STAGE_SLOTS` / `VE_STAGE_PARAMS` blob casts | The generator writes the blob in the registered order and the trampoline reads exactly what was written, both standard-layout aggregates of one scalar type. A native case asserts round-trip for a stage with every declared arity. |
| M3 touches `builtin_stages.cpp` while another sub-project is in flight there | No other sub-project claims `terrain/`; the roadmap's hot spots are `voxel_world.{h,cpp}`, `render/orchestrator.{h,cpp}` and `debug/hooks.cpp`. M1, M5 and M6 touch `voxel_world.cpp` — confirm nothing else is in flight there before starting. |

## 10. Out of scope

- Map stages, the `sector2d` tier, the context scheduler and the sector cache (terrain spec §7).
  `resolve_pipeline` still rejects `kind: map`.
- CPU-side resource sampling; `FieldResources` stays empty.
- The fallback `field.glslh` stub.
- Transpiling stage GLSL to C++. The hand-written mirror is one of the three files a new stage
  touches and stays that way.
- The deferred items in the roadmap's trigger table.

## 11. Decided during planning

- The mirror **declares and registers** its names rather than consuming a generated header. The
  roadmap's milestone 2 says "generated per-stage C++ headers"; a committed generated header makes
  a new stage 4 files and contradicts the sub-project's own ≤ 3 exit criterion, and a build-step
  generator would need the manifest parser built twice. Declaring with validation reaches the same
  goal — names not positions, drift caught — at 3 files and no new build machinery.
- The pipeline `lipschitz` line is a ceiling rather than an override. Fixing the combination rule
  alone changes nothing shipped, because the override makes the computed value dead code for both
  shipped pipelines.
- The Lipschitz fix is sequenced **second**, right after the loader consolidation, so it lands
  before M3 and M4 touch `resolve_pipeline`.
- `mesas` gets its own pipeline file rather than joining `default.pipeline`.
