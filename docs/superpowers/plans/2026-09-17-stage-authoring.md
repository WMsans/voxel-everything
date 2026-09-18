# Stage Authoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A terrain artist adds a stage in ≤ 3 files, with names rather than positions, on both CPU and GPU, and the tools refuse an unsafe Lipschitz budget.

**Architecture:** The CPU mirrors in `terrain/builtin_stages.cpp` stop indexing `s.extra[0]` / `p.at(0)` and instead declare the names they bind through a registering macro; `PipelineFieldGenerator::create` resolves each declared name against the resolved pipeline and packs one int blob and one float blob per stage, so a manifest/mirror mismatch fails the load with the stage named. `resolve_pipeline` gains a real gradient-bound computation (additive stages add, composing stages multiply) and the pipeline file's `lipschitz` line becomes a ceiling that bound must fit under rather than a value that replaces it. A single `ve::load_pipeline(reader, ...)` replaces the read → parse → per-stage read/parse → resolve sequence written out in three places, and the `FieldGenerator` wrapper around `Generator` is deleted.

**Tech Stack:** C++20 (`__VA_OPT__` is used and required), godot-cpp (Godot 4.7), GLSL through `RenderingDevice`, doctest (native tests), gdUnit4 (GPU tests), SCons.

**Spec:** `docs/superpowers/specs/2026-09-17-stage-authoring-design.md`. Domain nouns: `CONTEXT.md`.

## Global Constraints

- Branch: cut `refactor/stage-authoring` from `bc766e0` before Task 1.
- **Start commit:** `b03e5d6` (the spec commit `bc766e0` sits on top of it and changes no code).
- **Baseline failures:** a standing set of gdUnit failures exists on clean `main` and drifts. Compare against Task 1's file, by case name *and* message. A suite's case **count** dropping is itself a failure (a failing gdUnit case aborts the rest of its suite).
- **Golden policy:** when a pinned number moves, attribute every moved number to a cause, re-record the golden in the same commit, and put the cause in the commit message. If a diff looks like a shipped bug, **stop the task and report** — no fixes.
- **No pass internals, no shaders outside `shaders/stages/`.** This sub-project touches `extension/src/terrain/`, `extension/src/generator/`, `shaders/stages/`, `assets/pipelines/`, and the mechanical `generator()->sampler()` call sites. It does not touch `render/`.
- **Only one sub-project in flight per file hot spot.** Tasks 4, 10, 11 and 12 edit `extension/src/voxel_world.cpp`; confirm no other sub-project is in flight there before starting.
- GPU timing *values* are invalid on this machine (`debug_gpu_timings()` returns -1); never pin them.
- Build: `./build.sh -j$(sysctl -n hw.ncpu)` (macOS) or `-j$(nproc)` (Linux). A C++ rebuild can take ~20 min.
- Native tests: `cd extension && scons -Q test`. One case: `extension/build/tests/ve_tests -tc="<name>"`.
- GPU tests: `./gdunit_tests.sh -a res://tests/<suite>.gd` (comma list allowed). Full run: `./gdunit_tests.sh` (~5 min). Reports: `reports/report_N/results.xml`.
- Goldens regenerate with `VE_REGEN_GOLDEN=1 ./build.sh --test`.
- Shaders load from disk at world init; a shader edit needs no rebuild. A `.glslh` stage edit does *not* need a rebuild; its C++ mirror does.
- Commit messages in this plan carry **no** `Co-Authored-By` or Claude attribution lines.

## Amendments to the spec, decided while planning

Two spec statements do not survive contact with the C++:

1. **Spec §4 M3 says "one stage per commit" for the mirror migration. It cannot be.** Changing `StageFn`'s signature is atomic: every registered stage must move in the same commit or the build breaks. Task 8 migrates all four mirrors in one commit. Keeping two registration paths alive to stage the migration would be more scaffolding than the four ~10-line functions being migrated.
2. **Spec §5 names the generated structs `<Stage>Slots` / `<Stage>Params`.** The macro cannot capitalise its argument, so `VE_STAGE_SLOTS` takes the PascalCase token explicitly (`VE_STAGE_SLOTS(Hills, p, sdf, height)`) and `VE_REGISTER_STAGE` ties it to the `//!cpu` symbol. Names come out exactly as the spec says; the macro just needs telling.

Task 14 records both in the spec's implementation status.

## File Map

| File | Status | Responsibility |
|---|---|---|
| `extension/src/terrain/pipeline_load.{h,cpp}` | Create | The one read → parse → resolve sequence, behind a caller-supplied text reader |
| `extension/src/terrain/stage_manifest.{h,cpp}` | Modify | `LipschitzMode`; `//!lipschitz <add\|mul> <n>`; `//!use <stage>.<param>` |
| `extension/src/terrain/pipeline.{h,cpp}` | Modify | Bound computation + ceiling; undeclared `P.<ident>` rejection; `warnings` |
| `extension/src/terrain/stage_library.{h,cpp}` | Modify | `StageBinding`, the declaring macros, the trampoline |
| `extension/src/terrain/builtin_stages.cpp` | Modify | The four mirrors, by name; cave's literals deleted |
| `extension/src/terrain/pipeline_field_generator.{h,cpp}` | Modify | Name-resolved blobs; becomes a `Generator` |
| `extension/src/generator/field_generator.h` | Delete | The redundant seam |
| `extension/src/generator/generator.{h,cpp}` | Modify | `AnalyticGenerator` leaves |
| `extension/src/core/world_store.{h,cpp}` | Modify | Holds a `ve::Generator *` |
| `extension/src/voxel_world.cpp` | Modify | Uses `load_pipeline`; pushes warnings; init aborts on load failure |
| `extension/src/debug/hooks_{world,physics,lod,render}.cpp`, `extension/src/mesh/consolidation.cpp` | Modify | `generator()->sampler()` → `*generator()` |
| `extension/tests/analytic_oracle.{h,cpp}` | Create | `AnalyticGenerator`, test-only |
| `extension/tests/test_pipeline_load.cpp` | Create | The loader, against a fake reader |
| `extension/tests/test_default_pipeline_field.cpp` | Create | `default.pipeline` CPU field, pinned by corpus |
| `extension/tests/test_lipschitz_sampled.cpp` | Create | Sampled `\|grad sdf\|` ≤ reported bound, every shipped pipeline |
| `extension/tests/test_stage_bindings.cpp` | Create | A mirror binding a name the manifest does not declare fails `create` |
| `extension/tests/test_cross_stage_param.cpp` | Create | A `hills.amp_a` override reaches `stage_cave`'s mirror |
| `tests/golden/default_pipeline_field.txt` | Create | The corpus for the above |
| `shaders/stages/{hills,relief,cave,height_bands}.field.glslh` | Modify | `//!lipschitz <mode> <n>`; cave gains `//!use` |
| `shaders/stages/mesas.field.glslh`, `assets/pipelines/mesas.pipeline` | Create | The Appendix A re-trace and G2's multi-channel fixture |
| `assets/pipelines/{default,golden}.pipeline` | Modify | Ceiling semantics; the cave footgun comment deleted |
| `tests/test_field_diff.gd` | Modify | Runs every pipeline in `assets/pipelines/` |
| `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md` | Create | Baseline failure set |
| `docs/superpowers/plans/2026-09-17-stage-authoring-results.md` | Create | Regression evidence, golden attribution, re-traced counts |

---

### Task 1: Record the baseline failure set

No production change. Everything later compares against this file.

**Files:**
- Create: `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md`

**Interfaces:**
- Consumes: nothing.
- Produces: the baseline file — a list of `suite::case — first line of failure message`, plus per-suite case counts.

- [ ] **Step 1: Cut the branch**

```bash
git checkout -b refactor/stage-authoring
```

- [ ] **Step 2: Build and run the native suite**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
```

Expected: build OK; doctest prints its summary. Record the `test cases: N | N passed | M failed` line verbatim.

- [ ] **Step 3: Run the full gdUnit suite**

```bash
./gdunit_tests.sh
```

Expected: completes (a non-zero exit is normal here).

- [ ] **Step 4: Extract the failing cases**

```bash
latest=$(ls -d reports/report_* | sort -V | tail -1)
python3 - "$latest/results.xml" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for suite in root.iter('testsuite'):
    print(f"# {suite.get('name')}: tests={suite.get('tests')} failures={suite.get('failures')}")
for tc in root.iter('testcase'):
    bad = tc.find('failure') if tc.find('failure') is not None else tc.find('error')
    if bad is not None:
        msg = (bad.get('message') or bad.text or '').strip().splitlines()
        print(f"{tc.get('classname')}::{tc.get('name')} — {msg[0] if msg else ''}")
EOF
```

- [ ] **Step 5: Write the baseline file**

Create `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md` with this shape, filling in the real output from Steps 2 and 4:

```markdown
# Stage authoring — baseline (clean branch at b03e5d6)

Recorded 2026-09-17, <machine/GPU>.

## Native
<the doctest summary line from Step 2>

## gdUnit per-suite counts
<the "# suite: tests=N failures=M" lines from Step 4>

## gdUnit failing cases
<the "suite::case — message" lines from Step 4>
```

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md
git commit -m "docs: record stage authoring baseline failure set at b03e5d6"
```

---

### Task 2: Make `test_field_diff.gd` run every pipeline

The suite diffs CPU against GPU only for whatever pipeline the world happened to load. Every later task that touches a stage needs this covering more than `default.pipeline`.

**Files:**
- Modify: `tests/test_field_diff.gd` (whole-file restructure of `before_test`/`after_test` and the six `test_*` functions)

**Interfaces:**
- Consumes: `VoxelWorld.terrain_pipeline_path` (existing property, `voxel_world.cpp:211-214`), `hooks().debug_init_atlas()`, `hooks().debug_eval_field()`, `hooks().debug_field_params_bytes()` — all existing.
- Produces: a suite that needs no edit when a pipeline file is added.

- [ ] **Step 1: Replace per-test world setup with a per-pipeline helper**

The world's pipeline load is first-wins and fresh-init-only (`voxel_world.cpp:562`), so each pipeline needs its own `VoxelWorld` with `terrain_pipeline_path` set *before* `debug_init_atlas()`. Replace `before_test`/`after_test` with these, keeping every other existing function (`make_op`, `sample_points`, `_make_field_set`, `run_gpu`, `compare`) exactly as it is:

```gdscript
var _world: VoxelWorld
var _rd: RenderingDevice

func _pipeline_paths() -> PackedStringArray:
	# Enumerated rather than listed, so adding a pipeline file needs no edit here.
	var out := PackedStringArray()
	var dir := DirAccess.open("res://assets/pipelines")
	assert_object(dir).is_not_null()
	for f in dir.get_files():
		if f.ends_with(".pipeline"):
			out.append("res://assets/pipelines/" + f)
	out.sort()
	assert_int(out.size()).is_greater(0)
	return out

func _open_world(pipeline_path: String) -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = pipeline_path
	add_child(_world)
	# The probes compile field.glslh through the shader-source override map, so the world
	# must be initialized (pipeline load installs the generated override and the CPU
	# generator) before any dispatch. Without this both sides silently fall back and the
	# suite proves nothing.
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	_rd = RenderingServer.create_local_rendering_device()

func _close_world() -> void:
	if _rd != null:
		_rd.free()
		_rd = null
	if is_instance_valid(_world):
		_world.free()
	_world = null
```

- [ ] **Step 2: Replace the six test functions with one that loops pipelines**

Delete `test_base_field_matches_the_cpu_generator`, `test_sphere_subtract_matches`, `test_sphere_add_matches`, `test_sphere_paint_matches`, `test_subtract_across_a_material_seam_matches` and `test_ordered_op_chain_matches`. Add:

```gdscript
# One world init per pipeline, not per scenario: init streams an atlas and is the
# expensive part. `compare` already names the scenario in its failure messages; the
# pipeline name is prefixed here so a red run says which pipeline broke.
func test_every_pipeline_agrees_between_cpu_and_gpu() -> void:
	for path in _pipeline_paths():
		_open_world(path)
		var tag := path.get_file()
		var pts := sample_points()

		compare(pts, PackedByteArray(), 0, tag + " base")

		var subtract := make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 6.0)
		compare(pts, subtract, 1, tag + " subtract")

		var add := make_op(OP_ADD, 4, Vector3(10.0, 56.2, 10.0), 6.0)
		compare(pts, add, 1, tag + " add")

		var paint := make_op(OP_PAINT, 2, Vector3(10.0, 49.2, 10.0), 8.0)
		compare(pts, paint, 1, tag + " paint")

		# The guard against either evaluator reintroducing a per-sample hardness lookup:
		# the paint lays rock (hardness 3.0) across part of the volume and the carve that
		# follows straddles the boundary. A stored subtract has ONE radius.
		var seam := make_op(OP_PAINT, 2, Vector3(6.0, 51.2, 6.0), 8.0)
		seam.append_array(make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 6.0))
		compare(pts, seam, 2, tag + " material seam")

		# Order matters: an add inside an earlier subtract must refill it on both sides.
		var chain := make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 8.0)
		chain.append_array(make_op(OP_ADD, 4, Vector3(10.0, 51.2, 10.0), 4.0))
		chain.append_array(make_op(OP_PAINT, 3, Vector3(12.0, 51.2, 12.0), 5.0))
		compare(pts, chain, 3, tag + " chain")

		_close_world()
```

- [ ] **Step 3: Run it and confirm it passes for both shipped pipelines**

```bash
./gdunit_tests.sh -a res://tests/test_field_diff.gd
```

Expected: PASS. The run output must show the diff executing for `default.pipeline` **and** `golden.pipeline` — confirm by checking the case did not fail and, if in doubt, temporarily tighten `MAX_STEPS` to `0.0` and confirm both names appear in the failure messages.

- [ ] **Step 4: Prove the test bites**

Break the GPU side on purpose: in `shaders/stages/hills.field.glslh`, change `ctx.p.y - SURFACE_Y - ctx.height` to `ctx.p.y - SURFACE_Y - ctx.height + 1.0`. No rebuild is needed for a shader edit.

```bash
./gdunit_tests.sh -a res://tests/test_field_diff.gd
```

Expected: FAIL, with a message naming a pipeline (`default.pipeline base: worst sdf disagreement ...`). Then revert:

```bash
git checkout shaders/stages/hills.field.glslh
```

- [ ] **Step 5: Commit**

```bash
git add tests/test_field_diff.gd
git commit -m "test: run the field diff against every pipeline in assets/pipelines

The suite diffed only whatever pipeline the world happened to load. It now
enumerates assets/pipelines/*.pipeline and runs all six op scenarios against
each, one world init per pipeline. Adding a pipeline file needs no edit here."
```

---

### Task 3: Pin `default.pipeline`'s CPU field

Only `golden.pipeline` is pinned today, against `AnalyticGenerator`, and the relief stage already broke that equivalence for `default.pipeline`. Every later task must be able to prove it did not move the shipped terrain.

**Files:**
- Create: `extension/tests/test_default_pipeline_field.cpp`
- Create: `tests/golden/default_pipeline_field.txt` (generated in Step 3)

**Interfaces:**
- Consumes: `ve::parse_pipeline_desc`, `ve::parse_stage_manifest`, `ve::resolve_pipeline`, `ve::PipelineFieldGenerator::create`, `VE_REPO_ROOT` (an existing SCons define — see `extension/tests/test_field_baseline.cpp`).
- Produces: `tests/golden/default_pipeline_field.txt`, the corpus every later task checks against.

- [ ] **Step 1: Write the test**

The corpus format is the existing one from `tests/golden/field_baseline.txt`: `x y z sdf` as hex float bits, then material as decimal. This task deliberately hand-rolls the four-step load; Task 4 replaces it with `ve::load_pipeline`.

Create `extension/tests/test_default_pipeline_field.cpp`:

```cpp
// Characterization: pins the CPU field that assets/pipelines/default.pipeline produces.
// test_pipeline_equivalence.cpp pins golden.pipeline against ve::AnalyticGenerator, but
// default.pipeline gained the relief stage and that equivalence no longer holds for it --
// so without this file the shipped terrain has no CPU pin at all.
//
// Every value is compared as raw float BITS, not with a tolerance: a refactor that moves
// the field by one ulp has moved the world.
//
// Regenerate after an INTENTIONAL terrain change:  VE_REGEN_GOLDEN=1 ./build.sh --test
#include <doctest/doctest.h>
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_manifest.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Pt { float x, y, z; };

std::string root() { return std::string(VE_REPO_ROOT); }

std::string slurp(const std::string &p) {
	std::ifstream f(p);
	REQUIRE_MESSAGE(f.good(), "cannot open ", p);
	std::ostringstream o;
	o << f.rdbuf();
	return o.str();
}

std::unique_ptr<ve::PipelineFieldGenerator> default_pipeline() {
	ve::PipelineDesc d;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_pipeline_desc(
			slurp(root() + "/assets/pipelines/default.pipeline"), &d, &err), err);
	std::vector<ve::StageManifest> loaded;
	for (const ve::PipelineStageRef &r : d.stages) {
		ve::StageManifest m;
		REQUIRE_MESSAGE(ve::parse_stage_manifest(slurp(root() + "/shaders/" + r.path), &m, &err), err);
		loaded.push_back(m);
	}
	ve::ResolvedPipeline p;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, loaded, &p, &err), err);
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	return std::unique_ptr<ve::PipelineFieldGenerator>(g);
}

// Self-contained LCG: the corpus must not depend on any std:: RNG implementation. The
// ranges match tests/golden/field_baseline.txt so the two corpora cover the same ground,
// plus a far band where relief dominates and sin() range reduction is hardest.
std::vector<Pt> corpus() {
	std::vector<Pt> pts;
	uint32_t s = 20260917u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	for (int i = 0; i < 512; i++)
		pts.push_back({next(-20.0f, 60.0f), next(21.2f, 81.2f), next(-20.0f, 60.0f)});
	for (int i = 0; i < 256; i++)
		pts.push_back({next(700.0f, 900.0f), next(11.2f, 71.2f), next(700.0f, 900.0f)});
	for (int i = 0; i < 256; i++)
		pts.push_back({next(-3000.0f, 3000.0f), next(-200.0f, 400.0f), next(-3000.0f, 3000.0f)});
	return pts;
}

uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

std::string golden_path() { return root() + "/tests/golden/default_pipeline_field.txt"; }

} // namespace

TEST_CASE("the default pipeline's CPU field matches the committed corpus bit for bit") {
	auto g = default_pipeline();
	const std::vector<Pt> pts = corpus();

	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		FILE *f = std::fopen(golden_path().c_str(), "w");
		REQUIRE(f != nullptr);
		std::fprintf(f, "# assets/pipelines/default.pipeline CPU field. Columns: x y z sdf "
				"(hex float bits), material.\n");
		std::fprintf(f, "# Regenerate: VE_REGEN_GOLDEN=1 ./build.sh --test\n");
		for (const Pt &p : pts) {
			ve::Sample s = g->eval(p.x, p.y, p.z);
			std::fprintf(f, "%08x %08x %08x %08x %u\n", bits(p.x), bits(p.y), bits(p.z),
					bits(s.sdf), unsigned(s.material));
		}
		std::fclose(f);
		MESSAGE("regenerated " << golden_path());
	}

	FILE *f = std::fopen(golden_path().c_str(), "r");
	REQUIRE_MESSAGE(f != nullptr, "missing golden; run VE_REGEN_GOLDEN=1 ./build.sh --test");
	char line[256];
	size_t i = 0;
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		if (line[0] == '#' || line[0] == '\n') continue;
		unsigned bx, by, bz, bsdf, mat;
		REQUIRE(std::sscanf(line, "%x %x %x %x %u", &bx, &by, &bz, &bsdf, &mat) == 5);
		REQUIRE(i < pts.size());
		CHECK(bits(pts[i].x) == bx);
		CHECK(bits(pts[i].y) == by);
		CHECK(bits(pts[i].z) == bz);
		ve::Sample s = g->eval(pts[i].x, pts[i].y, pts[i].z);
		CHECK(bits(s.sdf) == bsdf);
		CHECK(unsigned(s.material) == mat);
		i++;
	}
	std::fclose(f);
	CHECK(i == pts.size());
}
```

- [ ] **Step 2: Run it and verify it fails for the right reason**

```bash
cd extension && scons -Q test
```

Expected: FAIL with `missing golden; run VE_REGEN_GOLDEN=1 ./build.sh --test`.

- [ ] **Step 3: Generate the corpus and re-run**

```bash
cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc="the default pipeline's CPU field matches the committed corpus bit for bit"
cd extension && scons -Q test
```

Expected: PASS. `tests/golden/default_pipeline_field.txt` now exists with 1024 data lines.

- [ ] **Step 4: Prove the test bites**

In `shaders/stages/relief.field.glslh`, change `//!param amp_a : float = 250.0` to `= 251.0` and rerun:

```bash
cd extension && scons -Q test
```

Expected: FAIL on mismatched `sdf` bits. Revert:

```bash
git checkout shaders/stages/relief.field.glslh
```

- [ ] **Step 5: Mark the multiplicative-Lipschitz test as the behaviour about to change**

In `extension/tests/test_pipeline_resolve.cpp`, above `TEST_CASE("resources sort by name and lipschitz combines multiplicatively")`, add:

```cpp
// LOCKED, AND DUE TO CHANGE. Multiplying every stage's bound understates an additive
// stage, which is a correctness bug (raycast.cpp steps by 1 / lipschitz() and would
// tunnel). The rule becomes "additive stages add, composing stages multiply" in the
// fix: commit for the Lipschitz combination; this case moves with it.
```

- [ ] **Step 6: Commit**

```bash
git add extension/tests/test_default_pipeline_field.cpp tests/golden/default_pipeline_field.txt extension/tests/test_pipeline_resolve.cpp
git commit -m "test: pin the default pipeline's CPU field to a committed corpus

golden.pipeline is pinned against AnalyticGenerator, but default.pipeline gained
the relief stage and that equivalence no longer holds for it, so the shipped
terrain had no CPU pin. 1024 points, compared as raw float bits.

Also marks the multiplicative-lipschitz case in test_pipeline_resolve.cpp as the
behaviour the combination-rule fix changes."
```

---

### Task 4: One `load_pipeline`

The read → parse → per-stage read/parse → resolve sequence is written out three times today, and Tasks 3 and 7 would make it five.

**Files:**
- Create: `extension/src/terrain/pipeline_load.h`, `extension/src/terrain/pipeline_load.cpp`
- Create: `extension/tests/test_pipeline_load.cpp`
- Modify: `extension/src/voxel_world.cpp:561-615` (`VoxelWorld::load_terrain_pipeline`)
- Modify: `extension/tests/test_pipeline_equivalence.cpp:30-47` (`golden_pipeline()`)
- Modify: `extension/tests/test_field_codegen_golden.cpp:30-46`
- Modify: `extension/tests/test_default_pipeline_field.cpp` (the `default_pipeline()` helper from Task 3)

**Interfaces:**
- Consumes: `ve::parse_pipeline_desc`, `ve::parse_stage_manifest`, `ve::resolve_pipeline`, `ve::ResolvedPipeline` (all existing, unchanged).
- Produces:
  ```cpp
  namespace ve {
  using TextReader = std::function<bool(const std::string &path, std::string *out)>;
  bool load_pipeline(const TextReader &reader, const std::string &pipeline_path,
                     const std::string &stage_root, ResolvedPipeline *out, std::string *error);
  }
  ```
  Task 10 adds a `std::vector<std::string> *warnings` parameter after `out`.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_pipeline_load.cpp`:

```cpp
// The loader against a fake reader: no filesystem, so the failure paths are reachable.
#include <doctest/doctest.h>
#include "terrain/pipeline_load.h"
#include <map>
#include <string>

namespace {

// A reader over an in-memory file table. Returns false for anything not in the table,
// which is how the "cannot read" paths get exercised.
ve::TextReader table_reader(const std::map<std::string, std::string> &files) {
	return [&files](const std::string &path, std::string *out) {
		auto it = files.find(path);
		if (it == files.end()) return false;
		*out = it->second;
		return true;
	};
}

const char *kHills =
		"//!stage     hills\n"
		"//!kind      field\n"
		"//!out       sdf : float\n"
		"//!param     amp : float = 6.0\n"
		"//!lipschitz 2.0\n"
		"//!cpu       ve::stage_hills\n"
		"void stage_hills(inout FieldCtx ctx) { ctx.sdf = ctx.p.y; }\n";

} // namespace

TEST_CASE("load_pipeline reads the pipeline and every stage it names, then resolves") {
	const std::map<std::string, std::string> files{
		{"pipe/a.pipeline", "seed 7\nstage stages/hills.field.glslh\n"},
		{"root/stages/hills.field.glslh", kHills},
	};
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &p, &err), err);
	REQUIRE(p.stages.size() == 1);
	CHECK(p.stages[0].name == "hills");
	CHECK(p.channel_slot("sdf") == 1);
	REQUIRE(p.params.size() == 1);
	CHECK(p.params[0].name == "hills.amp");
}

TEST_CASE("load_pipeline reports an unreadable pipeline file") {
	ve::ResolvedPipeline p;
	std::string err;
	const std::map<std::string, std::string> files{};
	CHECK_FALSE(ve::load_pipeline(table_reader(files), "pipe/missing.pipeline", "root/", &p, &err));
	CHECK(err.find("pipe/missing.pipeline") != std::string::npos);
}

TEST_CASE("load_pipeline reports an unreadable stage, naming the resolved path") {
	const std::map<std::string, std::string> files{
		{"pipe/a.pipeline", "stage stages/absent.field.glslh\n"},
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &p, &err));
	CHECK(err.find("root/stages/absent.field.glslh") != std::string::npos);
}

TEST_CASE("load_pipeline prefixes a manifest parse error with the stage path") {
	const std::map<std::string, std::string> files{
		{"pipe/a.pipeline", "stage stages/bad.field.glslh\n"},
		{"root/stages/bad.field.glslh", "//!stage bad\n//!kind field\n//!nonsense x\n"},
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &p, &err));
	CHECK(err.find("root/stages/bad.field.glslh") != std::string::npos);
	CHECK(err.find("nonsense") != std::string::npos);
}
```

- [ ] **Step 2: Run it to confirm it fails**

```bash
cd extension && scons -Q test
```

Expected: FAIL at compile — `terrain/pipeline_load.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `extension/src/terrain/pipeline_load.h`:

```cpp
#pragma once
// The one read -> parse -> resolve sequence. It was written out three times (voxel_world,
// two native tests) and the three copies had already drifted in their error messages.
//
// Pure C++ (no godot-cpp), which is what keeps it in the native suite: the reader is the
// only Godot-aware part. VoxelWorld supplies a FileAccess reader over res://; the native
// tests supply an ifstream reader over the repo.
#include <functional>
#include <string>

#include "terrain/pipeline.h"

namespace ve {

// Returns false when `path` cannot be read; `*out` is only meaningful on true.
using TextReader = std::function<bool(const std::string &path, std::string *out)>;

// `pipeline_path` is passed to the reader verbatim. Stage paths are passed as
// `stage_root` + the path the pipeline file spells (e.g. "stages/hills.field.glslh"),
// because a pipeline lives under assets/ and its stages under shaders/.
bool load_pipeline(const TextReader &reader, const std::string &pipeline_path,
		const std::string &stage_root, ResolvedPipeline *out, std::string *error);

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/terrain/pipeline_load.cpp`:

```cpp
#include "terrain/pipeline_load.h"

#include <vector>

#include "terrain/stage_manifest.h"

namespace ve {

bool load_pipeline(const TextReader &reader, const std::string &pipeline_path,
		const std::string &stage_root, ResolvedPipeline *out, std::string *error) {
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };

	std::string src;
	if (!reader(pipeline_path, &src)) return fail("cannot read " + pipeline_path);

	PipelineDesc desc;
	if (!parse_pipeline_desc(src, &desc, error)) return false;

	std::vector<StageManifest> loaded;
	for (const PipelineStageRef &r : desc.stages) {
		const std::string path = stage_root + r.path;
		std::string stage_src;
		if (!reader(path, &stage_src)) return fail("cannot read " + path);
		StageManifest m;
		if (!parse_stage_manifest(stage_src, &m, error)) {
			// The parser's message names the directive, not the file; prefix it so a
			// pipeline with a dozen stages says which one is broken.
			if (error) *error = path + ": " + *error;
			return false;
		}
		loaded.push_back(m);
	}

	return resolve_pipeline(desc, loaded, out, error);
}

} // namespace ve
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cd extension && scons -Q test
```

Expected: the four `load_pipeline` cases PASS. `SConstruct` already globs `src/terrain/*.cpp`, so no build edit is needed.

- [ ] **Step 6: Move the three existing call sites onto it**

In `extension/tests/test_pipeline_equivalence.cpp`, replace the body of `golden_pipeline()` (keeping the function and its `slurp` helper, which other cases use) with:

```cpp
std::unique_ptr<ve::PipelineFieldGenerator> golden_pipeline() {
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root + "/assets/pipelines/golden.pipeline",
			root + "/shaders/", &p, &err), err);
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	return std::unique_ptr<ve::PipelineFieldGenerator>(g);
}
```

adding `#include "terrain/pipeline_load.h"` and, above it, the reader the three tests share:

```cpp
// An ifstream reader over the repo, the native counterpart of VoxelWorld's FileAccess one.
bool repo_reader(const std::string &path, std::string *out) {
	std::ifstream f(path);
	if (!f.good()) return false;
	std::ostringstream o;
	o << f.rdbuf();
	*out = o.str();
	return true;
}
```

Do the same in `extension/tests/test_field_codegen_golden.cpp` (replacing lines 30-46 with a single `load_pipeline` call into `p`) and in `extension/tests/test_default_pipeline_field.cpp`'s `default_pipeline()` helper from Task 3. Each file gets its own copy of `repo_reader` in its anonymous namespace — they are separate translation units and four lines each; do not add a shared test header for it.

- [ ] **Step 7: Move `VoxelWorld::load_terrain_pipeline` onto it**

In `extension/src/voxel_world.cpp`, replace the body from `ve::PipelineDesc desc;` through the `resolve_pipeline` failure block (lines 570-597) with:

```cpp
	ve::ResolvedPipeline resolved;
	if (!ve::load_pipeline(
			[](const std::string &path, std::string *text) {
				return read_res_text(String(path.c_str()), text);
			},
			terrain_pipeline_path_.utf8().get_data(), "res://shaders/", &resolved, &err)) {
		UtilityFunctions::push_error(String("terrain pipeline: ") + err.c_str());
		return;
	}
```

Delete the now-unused `src` local and the `read_res_text(terrain_pipeline_path_, &src)` block above it — `load_pipeline` reads the pipeline file itself — but keep the `store_->terrain_pipeline().stages.empty()` guard at the top and everything from `std::string prelude;` down. Add `#include "terrain/pipeline_load.h"`.

Note: the "cannot read the pipeline file" case now reaches `push_error` rather than the old `push_warning`. That is deliberate — it is the same class of failure as every other load error — and it is why Task 11 can make the whole path abort init.

- [ ] **Step 8: Build and run everything**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
./gdunit_tests.sh -a res://tests/test_field_diff.gd,res://tests/test_brick_diff.gd
```

Expected: native suite PASS, including the Task 3 corpus unchanged (the loader is a pure refactor). Both gdUnit suites match the Task 1 baseline.

- [ ] **Step 9: Commit**

```bash
git add extension/src/terrain/pipeline_load.h extension/src/terrain/pipeline_load.cpp \
        extension/tests/test_pipeline_load.cpp extension/src/voxel_world.cpp \
        extension/tests/test_pipeline_equivalence.cpp extension/tests/test_field_codegen_golden.cpp \
        extension/tests/test_default_pipeline_field.cpp
git commit -m "refactor: one load_pipeline behind a caller-supplied reader

The read -> parse -> per-stage read/parse -> resolve sequence was written out
in voxel_world.cpp and two native tests, and the copies had drifted in their
error messages. One implementation, four callers; the reader is the only
Godot-aware part, so the loader stays in the native suite."
```

---

### Task 5: `//!lipschitz <add|mul> <n>` in the manifest

One number with no mode cannot say whether a stage adds to the gradient bound or composes with it. This task changes only the manifest grammar; `resolve_pipeline` still combines the old way and both shipped pipelines still override the result, so no reported bound moves.

**Files:**
- Modify: `extension/src/terrain/stage_manifest.h`
- Modify: `extension/src/terrain/stage_manifest.cpp` (the `key == "lipschitz"` branch)
- Modify: `extension/tests/test_stage_manifest.cpp`
- Modify: `extension/tests/test_pipeline_resolve.cpp` (the `field_stage` helper)
- Modify: `shaders/stages/{hills,relief,cave,height_bands}.field.glslh`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  ```cpp
  namespace ve {
  enum class LipschitzMode { kNone, kAdd, kMul };
  // StageManifest gains:  LipschitzMode lipschitz_mode = LipschitzMode::kNone;
  //                       float lipschitz = 0.0f;   // was 1.0f
  }
  ```
  Task 6 consumes both.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_stage_manifest.cpp`:

```cpp
TEST_CASE("//!lipschitz carries a mode") {
	const char *src =
			"//!stage     s\n"
			"//!kind      field\n"
			"//!out       sdf : float\n"
			"//!lipschitz add 1.78\n"
			"//!cpu       ve::s\n"
			"void s(inout FieldCtx c){}\n";
	ve::StageManifest m;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_stage_manifest(src, &m, &err), err);
	CHECK(m.lipschitz_mode == ve::LipschitzMode::kAdd);
	CHECK(m.lipschitz == doctest::Approx(1.78f));
}

TEST_CASE("//!lipschitz mul parses") {
	const char *src =
			"//!stage     s\n//!kind field\n//!out sdf : float\n"
			"//!lipschitz mul 1.0\n//!cpu ve::s\nvoid s(inout FieldCtx c){}\n";
	ve::StageManifest m;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_stage_manifest(src, &m, &err), err);
	CHECK(m.lipschitz_mode == ve::LipschitzMode::kMul);
	CHECK(m.lipschitz == doctest::Approx(1.0f));
}

TEST_CASE("the bare //!lipschitz form is rejected, with the two modes named") {
	const char *src =
			"//!stage s\n//!kind field\n//!out sdf : float\n"
			"//!lipschitz 2.0\n//!cpu ve::s\nvoid s(inout FieldCtx c){}\n";
	ve::StageManifest m;
	std::string err;
	CHECK_FALSE(ve::parse_stage_manifest(src, &m, &err));
	CHECK(err.find("add") != std::string::npos);
	CHECK(err.find("mul") != std::string::npos);
}

TEST_CASE("an unknown //!lipschitz mode is rejected by name") {
	const char *src =
			"//!stage s\n//!kind field\n//!out sdf : float\n"
			"//!lipschitz compose 2.0\n//!cpu ve::s\nvoid s(inout FieldCtx c){}\n";
	ve::StageManifest m;
	std::string err;
	CHECK_FALSE(ve::parse_stage_manifest(src, &m, &err));
	CHECK(err.find("compose") != std::string::npos);
}

TEST_CASE("a stage with no //!lipschitz reports no mode") {
	const char *src =
			"//!stage s\n//!kind field\n//!out material : uint\n"
			"//!cpu ve::s\nvoid s(inout FieldCtx c){}\n";
	ve::StageManifest m;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_stage_manifest(src, &m, &err), err);
	CHECK(m.lipschitz_mode == ve::LipschitzMode::kNone);
}
```

- [ ] **Step 2: Run to confirm they fail**

```bash
cd extension && scons -Q test
```

Expected: FAIL at compile — `LipschitzMode` is not a member of `ve`.

- [ ] **Step 3: Change the manifest struct**

In `extension/src/terrain/stage_manifest.h`, add above `struct ChannelDecl`:

```cpp
// How a stage's gradient bound combines with the bound of the stages before it. An
// additive stage (one that adds a term to sdf) adds; a composing stage (a domain warp, a
// CSG combine) multiplies. See the terrain-pipeline design section 10.1.
enum class LipschitzMode { kNone, kAdd, kMul };
```

and in `struct StageManifest` replace `float lipschitz = 1.0f;` with:

```cpp
	LipschitzMode lipschitz_mode = LipschitzMode::kNone;
	float lipschitz = 0.0f;
```

- [ ] **Step 4: Change the parser**

In `extension/src/terrain/stage_manifest.cpp`, replace the line

```cpp
		else if (key == "lipschitz") { out->lipschitz = float(std::atof(rest.c_str())); }
```

with:

```cpp
		else if (key == "lipschitz") {
			const size_t sp2 = rest.find_first_of(" \t");
			if (sp2 == std::string::npos)
				return fail("//!lipschitz needs a mode and a number: "
						"'add' for a stage that adds a term to sdf, 'mul' for one that "
						"composes -- e.g. //!lipschitz add 1.78");
			const std::string mode = rest.substr(0, sp2);
			if (mode == "add") out->lipschitz_mode = LipschitzMode::kAdd;
			else if (mode == "mul") out->lipschitz_mode = LipschitzMode::kMul;
			else return fail("unknown //!lipschitz mode '" + mode + "' (expected add or mul)");
			out->lipschitz = float(std::atof(trim(rest.substr(sp2)).c_str()));
		}
```

- [ ] **Step 5: Fix the two test helpers the struct change breaks**

In `extension/tests/test_pipeline_resolve.cpp`, in `field_stage()`, replace `m.lipschitz = 1.0f;` with:

```cpp
	m.lipschitz_mode = ve::LipschitzMode::kMul;
	m.lipschitz = 1.0f;
```

In `extension/tests/test_stage_manifest.cpp`, the existing case asserting `m.lipschitz == doctest::Approx(2.0f)` reads a fixture with a bare `//!lipschitz 2.0`; update that fixture to `//!lipschitz add 2.0` and add `CHECK(m.lipschitz_mode == ve::LipschitzMode::kAdd);`.

- [ ] **Step 6: Update the four shipped stage manifests**

These are the honest per-stage numbers, each the Lipschitz constant of that stage's own contribution to `sdf`. Derivations go in the files so the next author does not have to re-derive them.

`shaders/stages/hills.field.glslh` — replace `//!lipschitz 2.0` with:

```glsl
//!lipschitz add 1.78
```

and add to the comment block below the directives:

```glsl
// Bound: sdf = y - SURFACE_Y - h, so |grad sdf| = sqrt(1 + |grad h|^2).
//   |dh/dx| <= 6(0.11) + 3(0.031) + 1(0.23) = 0.983
//   |dh/dz| <= 6(0.13) + 3(0.043) + 1(0.19) = 1.099
//   sqrt(1 + 0.983^2 + 1.099^2) = 1.7816
// This stage establishes the field, so it combines with `add`.
```

`shaders/stages/relief.field.glslh` — replace `//!lipschitz 1.0` with:

```glsl
//!lipschitz add 0.21
```

and replace the paragraph beginning "The WAVELENGTHS are set by the Lipschitz bound" with:

```glsl
// The WAVELENGTHS are set by the Lipschitz budget, not by taste. This stage ADDS a term to
// sdf, so it adds to the bound: |grad r| <= sqrt(2) * (amp_a*freq_a + amp_b*freq_b) =
// sqrt(2) * 0.15 = 0.2121. On top of hills' 1.78 that is 1.99, inside default.pipeline's
// declared ceiling of 2.0. Understating the bound is a correctness bug -- raycast.cpp steps
// by 1 / lipschitz() and would overshoot a surface -- so resolve_pipeline computes the
// combination and refuses a pipeline whose stages do not fit under its ceiling.
```

`shaders/stages/cave.field.glslh` — replace `//!lipschitz 1.0` with:

```glsl
//!lipschitz mul 1.0
```

and extend its existing comment to:

```glsl
// CSG subtract of one sphere, exactly as AnalyticGenerator does it. max(a, -b) is bounded
// by the larger of the two operands' bounds, and the sphere's gradient is 1.0, below the
// running bound -- so this stage composes without raising it: mul 1.0.
```

`shaders/stages/height_bands.field.glslh` — delete its `//!lipschitz 1.0` line entirely and add:

```glsl
// No //!lipschitz: this stage writes only `material`, so it cannot move the gradient of
// the distance field. resolve_pipeline rejects a bound declared by a stage that writes no
// sdf, and requires one from every stage that does.
```

- [ ] **Step 7: Run everything and confirm nothing moved**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
```

Expected: PASS, including `test_pipeline_equivalence`'s `lipschitz` case. The reported bound is still 2.0 for both shipped pipelines because `resolve_pipeline` still applies the pipeline file's override — Task 6 is what changes that. `default_pipeline_field.txt` and `field.glslh.golden` are unchanged: neither the CPU field nor the generated GLSL reads the bound.

- [ ] **Step 8: Commit**

```bash
git add extension/src/terrain/stage_manifest.h extension/src/terrain/stage_manifest.cpp \
        extension/tests/test_stage_manifest.cpp extension/tests/test_pipeline_resolve.cpp \
        shaders/stages/hills.field.glslh shaders/stages/relief.field.glslh \
        shaders/stages/cave.field.glslh shaders/stages/height_bands.field.glslh
git commit -m "feat: //!lipschitz declares add or mul

A single number cannot say whether a stage adds a term to sdf or composes with
what came before, and the two combine differently. Every shipped stage restates
its bound as the Lipschitz constant of its own contribution, with the derivation
in the file: hills add 1.78, relief add 0.21, cave mul 1.0, height_bands none
(it writes only material).

No reported bound moves yet -- resolve_pipeline still multiplies and both shipped
pipelines still override the result. The next commit fixes that."
```

---

### Task 6: Fix the Lipschitz combination and make the pipeline line a ceiling

**This is a behaviour change, in its own `fix:` commit,** landing before Tasks 8 and 9 touch `resolve_pipeline`.

The bug: `pipeline.cpp:141` multiplies every stage's bound, which understates an additive stage. It is invisible today because `pipeline.cpp:161` lets the pipeline file *replace* the computed value and both shipped pipelines declare `lipschitz 2.0` — so the number the raymarcher trusts is a hand-written constant that nothing checks.

**Files:**
- Modify: `extension/src/terrain/pipeline.h` (rename `lipschitz_override` → `lipschitz_ceiling`)
- Modify: `extension/src/terrain/pipeline.cpp` (`parse_pipeline_desc`, `resolve_pipeline`)
- Modify: `extension/tests/test_pipeline_resolve.cpp`
- Modify: `extension/tests/test_pipeline_parse.cpp:19`
- Modify: `extension/tests/test_pipeline_equivalence.cpp:89-93`
- Modify: `extension/tests/test_relief_lipschitz.cpp`
- Modify: `assets/pipelines/default.pipeline`, `assets/pipelines/golden.pipeline`

**Interfaces:**
- Consumes: `ve::LipschitzMode`, `StageManifest::lipschitz_mode`, `StageManifest::lipschitz` (Task 5).
- Produces: `PipelineDesc::lipschitz_ceiling` (was `lipschitz_override`); `ResolvedPipeline::lipschitz` is now always the computed bound. Task 7 consumes it.

- [ ] **Step 1: Write the failing tests**

In `extension/tests/test_pipeline_resolve.cpp`, replace the whole `TEST_CASE("resources sort by name and lipschitz combines multiplicatively")` (and the LOCKED comment Task 3 put above it) with:

```cpp
TEST_CASE("resources sort by name") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].samples.push_back({"sector.z", "texture2d_r32f", 0.0f});
	st[0].samples.push_back({"sector.a", "texture2d_r32f", 0.0f});
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd;
	st[0].lipschitz = 1.5f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(1), st, &p, &err), err);
	REQUIRE(p.resources.size() == 2);
	CHECK(p.resources[0].name == "sector.a");
	CHECK(p.resources[1].name == "sector.z");
}

TEST_CASE("additive stages add to the bound and composing stages multiply it") {
	std::vector<ve::StageManifest> st{
		field_stage("base", {"sdf"}, {}),
		field_stage("relief", {"sdf"}, {"sdf"}),
		field_stage("warp", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[1].lipschitz_mode = ve::LipschitzMode::kAdd; st[1].lipschitz = 0.21f;
	st[2].lipschitz_mode = ve::LipschitzMode::kMul; st[2].lipschitz = 2.0f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(3), st, &p, &err), err);
	CHECK(p.lipschitz == doctest::Approx(3.98f));  // (1.78 + 0.21) * 2.0
}

TEST_CASE("a stage that writes no sdf contributes nothing and must declare nothing") {
	std::vector<ve::StageManifest> st{
		field_stage("base", {"sdf"}, {}),
		field_stage("bands", {"material"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[1].lipschitz_mode = ve::LipschitzMode::kNone;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
	CHECK(p.lipschitz == doctest::Approx(1.78f));

	st[1].lipschitz_mode = ve::LipschitzMode::kMul; st[1].lipschitz = 3.0f;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("bands") != std::string::npos);
	CHECK(err.find("writes no sdf") != std::string::npos);
}

TEST_CASE("an sdf writer with no declared bound is rejected by name") {
	std::vector<ve::StageManifest> st{field_stage("base", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kNone;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("base") != std::string::npos);
	CHECK(err.find("//!lipschitz") != std::string::npos);
}

TEST_CASE("the first sdf writer must be additive, because a multiplied zero is not a bound") {
	std::vector<ve::StageManifest> st{field_stage("base", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kMul; st[0].lipschitz = 2.0f;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("base") != std::string::npos);
	CHECK(err.find("add") != std::string::npos);
}

TEST_CASE("the pipeline's lipschitz line is a ceiling, and exceeding it fails the load") {
	std::vector<ve::StageManifest> st{
		field_stage("base", {"sdf"}, {}),
		field_stage("steep", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[1].lipschitz_mode = ve::LipschitzMode::kAdd; st[1].lipschitz = 8.0f;

	ve::PipelineDesc d = desc_for(2);
	d.lipschitz_ceiling = 2.0f;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(d, st, &p, &err));
	// The message must name every contributor, so the artist can see which stage to budget.
	CHECK(err.find("base") != std::string::npos);
	CHECK(err.find("steep") != std::string::npos);

	// No ceiling declared: the computed bound is reported and the load succeeds.
	d.lipschitz_ceiling = 0.0f;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, st, &p, &err), err);
	CHECK(p.lipschitz == doctest::Approx(9.78f));
}

TEST_CASE("a ceiling the stages fit under is accepted and does not replace the bound") {
	std::vector<ve::StageManifest> st{field_stage("base", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	ve::PipelineDesc d = desc_for(1);
	d.lipschitz_ceiling = 2.0f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, st, &p, &err), err);
	CHECK(p.lipschitz == doctest::Approx(1.78f));  // NOT 2.0
}
```

- [ ] **Step 2: Run to confirm they fail**

```bash
cd extension && scons -Q test
```

Expected: FAIL at compile — `lipschitz_ceiling` is not a member of `ve::PipelineDesc`.

- [ ] **Step 3: Rename the field**

In `extension/src/terrain/pipeline.h`, in `struct PipelineDesc`, replace:

```cpp
    float lipschitz_override = 0.0f;               // 0 => use the combined bound
```

with:

```cpp
    // A CEILING, not an override: resolve computes the bound from the stages and refuses a
    // pipeline whose computed bound exceeds this. 0 => no ceiling declared, no check.
    float lipschitz_ceiling = 0.0f;
```

In `extension/src/terrain/pipeline.cpp`, in `parse_pipeline_desc`, replace `else if (key == "lipschitz") out->lipschitz_override = ...` with:

```cpp
		else if (key == "lipschitz") out->lipschitz_ceiling = float(std::atof(rest.c_str()));
```

- [ ] **Step 4: Replace the combination rule**

In `extension/src/terrain/pipeline.cpp`, add to the anonymous namespace next to `hash_feed`:

```cpp
// Every contributor, so a rejected pipeline tells the artist which stage to budget rather
// than just that the total is too large.
std::string bound_report(float lip, float ceiling, const std::vector<StageManifest> &st) {
	std::string s = "pipeline gradient bound " + std::to_string(lip) +
			" exceeds the declared ceiling " + std::to_string(ceiling) +
			"; raise the ceiling only if the raymarcher can afford the steps:";
	for (const StageManifest &m : st) {
		if (m.lipschitz_mode == LipschitzMode::kNone) continue;
		s += "\n  " + m.name + ": " +
				(m.lipschitz_mode == LipschitzMode::kAdd ? "add " : "mul ") +
				std::to_string(m.lipschitz);
	}
	return s;
}
```

In `resolve_pipeline`, replace `float lip = 1.0f;` with:

```cpp
	// Starts at zero: the first sdf writer establishes the field and adds its own bound.
	float lip = 0.0f;
	bool seen_sdf_writer = false;
```

Replace the line `lip *= (m.lipschitz > 0.0f ? m.lipschitz : 1.0f);` with:

```cpp
		bool writes_sdf = false;
		for (const ChannelDecl &w : m.writes)
			if (w.name == "sdf") writes_sdf = true;
		if (writes_sdf) {
			if (m.lipschitz_mode == LipschitzMode::kNone)
				return fail("stage '" + m.name + "' writes sdf but declares no "
						"//!lipschitz <add|mul> <n>; an unbounded field tunnels the raymarcher");
			if (!seen_sdf_writer && m.lipschitz_mode != LipschitzMode::kAdd)
				return fail("stage '" + m.name + "' is the first stage to write sdf, so its "
						"//!lipschitz mode must be 'add': it establishes the field, and "
						"multiplying a zero bound is not a bound");
			lip = m.lipschitz_mode == LipschitzMode::kAdd ? lip + m.lipschitz
			                                              : lip * m.lipschitz;
			seen_sdf_writer = true;
		} else if (m.lipschitz_mode != LipschitzMode::kNone) {
			return fail("stage '" + m.name + "' declares //!lipschitz but writes no sdf, so it "
					"cannot move the gradient of the distance field");
		}
```

Replace the final `out->lipschitz = desc.lipschitz_override > 0.0f ? desc.lipschitz_override : lip;` with:

```cpp
	out->lipschitz = lip;
	if (desc.lipschitz_ceiling > 0.0f && lip > desc.lipschitz_ceiling)
		return fail(bound_report(lip, desc.lipschitz_ceiling, out->stages));
```

- [ ] **Step 5: Run the unit tests**

```bash
cd extension && scons -Q test
```

Expected: the seven new cases PASS. `test_pipeline_parse` and `test_pipeline_equivalence` FAIL — fixed next.

- [ ] **Step 6: Update the three tests the behaviour change breaks**

`extension/tests/test_pipeline_parse.cpp:19` — `CHECK(d.lipschitz_override == doctest::Approx(2.0f));` becomes:

```cpp
	CHECK(d.lipschitz_ceiling == doctest::Approx(2.0f));
```

`extension/tests/test_pipeline_equivalence.cpp:89-93` — replace the case with:

```cpp
// The pipeline's bound is now COMPUTED from its stages (golden.pipeline: hills add 1.78,
// cave mul 1.0 => 1.78), where AnalyticGenerator returns a hand-derived 2.0. Equality no
// longer holds and should not: what matters is that the pipeline never claims a LARGER
// bound than the analytic field it reproduces, because overstating costs raycast steps
// while understating tunnels.
TEST_CASE("the pipeline's bound is no looser than the analytic generator's") {
	auto g = golden_pipeline();
	ve::AnalyticGenerator ref;
	CHECK(g->sampler().lipschitz() <= ref.lipschitz());
	CHECK(g->sampler().lipschitz() == doctest::Approx(1.78f));
}
```

`extension/tests/test_relief_lipschitz.cpp` — delete `TEST_CASE("the relief stage fits inside the pipeline's declared Lipschitz bound")` and the `kHillsDx`/`kHillsDz` constants it used; `resolve_pipeline` computes that now and Task 7 checks it against the real field. Keep the other two cases and their `kRelief*` constants, and replace the file's header comment with:

```cpp
// The relief stage's parameters, mirrored here so the LOOK is checked against the numbers
// that ship. The Lipschitz budget used to be worked by hand in this file; resolve_pipeline
// computes it now (hills add 1.78 + relief add 0.21 = 1.99, under default.pipeline's
// ceiling of 2.0) and test_lipschitz_sampled.cpp checks it against the real field.
```

- [ ] **Step 7: Update the two pipeline files' comments**

In `assets/pipelines/default.pipeline`, replace the `lipschitz 2.0` line's surrounding context so the file reads:

```
# A CEILING, not the bound. resolve_pipeline computes the bound from the stages below
# (hills add 1.78 + relief add 0.21 = 1.99) and refuses to load this pipeline if that
# exceeds the number here. Raising it costs raycast steps and widens mesh padding.
lipschitz 2.0
```

Do the same in `assets/pipelines/golden.pipeline`, where the computed bound is 1.78.

- [ ] **Step 8: Build and run everything**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
./gdunit_tests.sh
```

Expected: native suite PASS. `default_pipeline_field.txt` and `field.glslh.golden` are **unchanged** — neither reads the bound.

The gdUnit run is the risk point: the reported bound drops from 2.0 to 1.99 (`default`) and 2.0 to 1.78 (`golden`), so `raycast.cpp` steps by `1/1.99` instead of `1/2.0` and `mesh_chunk.cpp`'s conservative padding narrows. Compare against Task 1's baseline by case name **and** message.

- If a suite moves, apply the golden policy: attribute every moved number to this cause, re-record in this commit, and name the cause in the message.
- **If a diff looks like a shipped bug rather than a step-size change — a hole, a missing chunk, a collider that no longer matches its mesh — stop this task and report.** Do not fix it here.

- [ ] **Step 9: Commit**

```bash
git add extension/src/terrain/pipeline.h extension/src/terrain/pipeline.cpp \
        extension/tests/test_pipeline_resolve.cpp extension/tests/test_pipeline_parse.cpp \
        extension/tests/test_pipeline_equivalence.cpp extension/tests/test_relief_lipschitz.cpp \
        assets/pipelines/default.pipeline assets/pipelines/golden.pipeline
git commit -m "fix: combine stage Lipschitz bounds correctly, and make the pipeline line a ceiling

resolve_pipeline multiplied every stage's bound, which understates an additive
stage -- and understating is a correctness bug, because raycast.cpp steps by
1 / lipschitz() and would overshoot a surface. It was invisible because the
pipeline file's lipschitz line REPLACED the computed value, and both shipped
pipelines declare 2.0, so the number the raymarcher trusted was a hand-written
constant nothing checked.

Additive stages now add and composing stages multiply; a stage that writes sdf
must declare a mode and one that does not must not; the first sdf writer must be
additive. The pipeline line is a ceiling the computed bound must fit under, and
exceeding it fails the load with every stage's contribution listed.

Reported bounds move: default.pipeline 2.0 -> 1.99, golden.pipeline 2.0 -> 1.78.
The CPU corpus and the generated GLSL are unchanged; neither reads the bound.
test_pipeline_equivalence now asserts the pipeline's bound is no LOOSER than the
analytic generator's rather than equal to it."
```

---

### Task 7: Check the declared bound against the real field

The combination rule being right does not make the declared per-stage numbers true. This is the sampled check the terrain design's §10.1 asks for, as a native test rather than a debug mode: central differences cost nothing and run every build.

**Files:**
- Create: `extension/tests/test_lipschitz_sampled.cpp`

**Interfaces:**
- Consumes: `ve::load_pipeline` (Task 4), `ResolvedPipeline::lipschitz` (Task 6), `ve::PipelineFieldGenerator::create` and `::eval` (existing; Task 11 renames `eval` to `sample`).
- Produces: nothing other tasks consume.

- [ ] **Step 1: Write the test**

Create `extension/tests/test_lipschitz_sampled.cpp`:

```cpp
// The declared bound versus the actual field. resolve_pipeline can combine stage bounds
// perfectly and still report a lie, because nothing checks that a stage's //!lipschitz
// number describes its own code. This is the terrain design's section 10.1 check, as a
// test: understating the bound makes raycast.cpp tunnel through surfaces, and that failure
// looks like a RENDERING bug to whoever hits it.
//
// Epsilon matches ve::Generator::sample_gradient (generator.cpp:22), so this measures the
// same differences every consumer of the gradient sees.
#include <doctest/doctest.h>
#include "terrain/pipeline_field_generator.h"
#include "terrain/pipeline_load.h"
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace {

bool repo_reader(const std::string &path, std::string *out) {
	std::ifstream f(path);
	if (!f.good()) return false;
	std::ostringstream o;
	o << f.rdbuf();
	*out = o.str();
	return true;
}

void check_bound(const char *pipeline) {
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader,
			root + "/assets/pipelines/" + pipeline, root + "/shaders/", &p, &err), err);
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	REQUIRE_MESSAGE(g != nullptr, err);

	const float e = 0.01f;
	uint32_t s = 20260917u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};

	float worst = 0.0f;
	float wx = 0.0f, wy = 0.0f, wz = 0.0f;
	for (int i = 0; i < 4096; i++) {
		// Wide enough to cross the relief wavelengths and the carved cave, and to reach
		// where sin() range reduction is hardest.
		const float x = next(-3000.0f, 3000.0f);
		const float y = next(-200.0f, 400.0f);
		const float z = next(-3000.0f, 3000.0f);
		const float dx = (g->eval(x + e, y, z).sdf - g->eval(x - e, y, z).sdf) / (2.0f * e);
		const float dy = (g->eval(x, y + e, z).sdf - g->eval(x, y - e, z).sdf) / (2.0f * e);
		const float dz = (g->eval(x, y, z + e).sdf - g->eval(x, y, z - e).sdf) / (2.0f * e);
		const float mag = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (mag > worst) { worst = mag; wx = x; wy = y; wz = z; }
	}

	CHECK_MESSAGE(worst <= p.lipschitz, pipeline, ": sampled |grad sdf| ", worst,
			" exceeds the reported bound ", p.lipschitz, " at (", wx, ", ", wy, ", ", wz,
			"). A stage's //!lipschitz number is understating its own gradient.");
}

} // namespace

TEST_CASE("default.pipeline never exceeds its reported gradient bound") {
	check_bound("default.pipeline");
}

TEST_CASE("golden.pipeline never exceeds its reported gradient bound") {
	check_bound("golden.pipeline");
}
```

- [ ] **Step 2: Run it**

```bash
cd extension && scons -Q test
```

Expected: PASS. `default.pipeline` reports 1.99 against a true worst case near 1.96; `golden.pipeline` reports 1.78 against a true worst case near 1.78 — if `golden` fails by a hair, that is real information: report it rather than loosening the test, because the analytic field's own bound is what `hills add 1.78` claims.

- [ ] **Step 3: Prove the test bites**

In `shaders/stages/relief.field.glslh`, change `//!param amp_a : float = 250.0` to `= 2500.0` (ten times the amplitude at the same frequency, so the real gradient rises by ~0.9 while the declared `add 0.21` does not).

```bash
cd extension && scons -Q test
```

Expected: FAIL — `default.pipeline: sampled |grad sdf| ... exceeds the reported bound 1.99 at (...)`. Note that `test_default_pipeline_field` also fails here, which is correct: the terrain moved. Revert:

```bash
git checkout shaders/stages/relief.field.glslh
```

- [ ] **Step 4: Commit**

```bash
git add extension/tests/test_lipschitz_sampled.cpp
git commit -m "test: check every shipped pipeline's declared bound against its real field

Combining stage bounds correctly does not make the declared numbers true. Central
differences over the CPU mirror at 4096 points, against the reported bound, with
the same epsilon Generator::sample_gradient uses. This is the terrain design's
section 10.1 check: a stage that understates its own gradient makes raycast tunnel,
which looks like a rendering bug to whoever hits it."
```

---

### Task 8: Mirrors bind by name

`builtin_stages.cpp` reads `s.extra[0]`, `s.extra[1]`, `p.at(0)`. The ordering rule lives in comments at three call sites, and reordering a `//!out` line silently rebinds every later index.

**Amends spec §4 M3:** all four mirrors move in this one commit. `StageFn`'s signature change is atomic — there is no build in which some mirrors use the old shape and some the new.

**Files:**
- Modify: `extension/src/terrain/stage_library.h`, `extension/src/terrain/stage_library.cpp`
- Modify: `extension/src/terrain/pipeline_field_generator.h`, `extension/src/terrain/pipeline_field_generator.cpp`
- Modify: `extension/src/terrain/builtin_stages.cpp` (all four mirrors)
- Create: `extension/tests/test_stage_bindings.cpp`

**Interfaces:**
- Consumes: `ResolvedPipeline::channel_slot`, `ResolvedPipeline::params` (existing).
- Produces:
  ```cpp
  namespace ve {
  using StageFn = void (*)(FieldCtx &, const void *slots, const void *params, const FieldResources &);
  struct StageBinding { const char *symbol; StageFn fn; const char *slot_names, *param_names; };
  // StageLibrary::lookup(symbol) -> const StageBinding *   (was StageFn)
  }
  // VE_STAGE_SLOTS(Hills, p, sdf, height)        -> struct HillsSlots  { int p, sdf, height; };
  // VE_STAGE_PARAMS(Hills, amp_a, amp_b, amp_c)  -> struct HillsParams { float amp_a, amp_b, amp_c; };
  // VE_REGISTER_STAGE("ve::stage_hills", Hills, stage_hills)
  ```
  `StageSlots` and `StageParams` are deleted. Task 9 consumes the macros.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_stage_bindings.cpp`:

```cpp
// A mirror declares the names it binds; create() resolves them against the manifest. The
// point of the whole mechanism is this file's second case: a mirror that names a channel
// the manifest does not declare must fail LOUDLY at load, where the old extra[] indexing
// silently bound whatever slot happened to sit at that position.
#include <doctest/doctest.h>
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_library.h"
#include <memory>
#include <string>
#include <vector>

namespace {

VE_STAGE_SLOTS(Probe, p, sdf, height);
VE_STAGE_PARAMS(Probe, gain);

void stage_probe(ve::FieldCtx &ctx, const ProbeSlots &s, const ProbeParams &pm,
		const ve::FieldResources &) {
	ctx.f(s.height) = pm.gain * ctx.v(s.p)[0];
	ctx.f(s.sdf) = ctx.v(s.p)[1] - ctx.f(s.height);
}
VE_REGISTER_STAGE("ve::test_stage_probe", Probe, stage_probe);

// Binds a channel no manifest in this file declares.
VE_STAGE_SLOTS(Absent, p, sdf, temperature);
VE_STAGE_PARAMS(Absent);

void stage_absent(ve::FieldCtx &ctx, const AbsentSlots &s, const AbsentParams &,
		const ve::FieldResources &) {
	ctx.f(s.sdf) = ctx.f(s.temperature);
}
VE_REGISTER_STAGE("ve::test_stage_absent", Absent, stage_absent);

ve::StageManifest probe_manifest(const char *cpu) {
	ve::StageManifest m;
	m.name = "probe";
	m.kind = ve::StageKind::kField;
	m.cpu_symbol = cpu;
	m.lipschitz_mode = ve::LipschitzMode::kAdd;
	m.lipschitz = 2.0f;
	m.writes.push_back({"sdf", ve::ChannelType::kFloat});
	m.writes.push_back({"height", ve::ChannelType::kFloat});
	m.params.push_back({"gain", ve::ChannelType::kFloat, 3.0f});
	m.body = "void stage_probe(inout FieldCtx c){}\n";
	return m;
}

ve::ResolvedPipeline resolve_one(const ve::StageManifest &m) {
	ve::PipelineDesc d;
	d.stages.push_back({"s", {}});
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, {m}, &p, &err), err);
	return p;
}

} // namespace

TEST_CASE("a mirror's declared names resolve to the manifest's channels and params") {
	const ve::ResolvedPipeline p = resolve_one(probe_manifest("ve::test_stage_probe"));
	std::string err;
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	REQUIRE_MESSAGE(g != nullptr, err);
	// height = gain * x = 3 * 2 = 6; sdf = y - height = 10 - 6 = 4.
	CHECK(g->eval(2.0f, 10.0f, 0.0f).sdf == doctest::Approx(4.0f));
}

TEST_CASE("a mirror binding a channel the manifest does not declare fails create, by name") {
	ve::StageManifest m = probe_manifest("ve::test_stage_absent");
	const ve::ResolvedPipeline p = resolve_one(m);
	std::string err;
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	CHECK(g == nullptr);
	CHECK(err.find("temperature") != std::string::npos);
	CHECK(err.find("probe") != std::string::npos);
}

TEST_CASE("a mirror binding a param the manifest does not declare fails create, by name") {
	ve::StageManifest m = probe_manifest("ve::test_stage_probe");
	m.params.clear();  // the mirror still declares `gain`
	const ve::ResolvedPipeline p = resolve_one(m);
	std::string err;
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	CHECK(g == nullptr);
	CHECK(err.find("gain") != std::string::npos);
}
```

- [ ] **Step 2: Run to confirm it fails**

```bash
cd extension && scons -Q test
```

Expected: FAIL at compile — `VE_STAGE_SLOTS` is not defined.

- [ ] **Step 3: Rewrite `stage_library.h`**

Replace `extension/src/terrain/stage_library.h` entirely:

```cpp
#pragma once

#include <string>
#include <vector>

namespace ve {
struct FieldCtx {
	static constexpr int kMaxChannels = 32;
	// Four floats per channel so a vec4 channel fits; scalars use component 0. Slot indices
	// come from ResolvedPipeline::channel_slot, so CPU and GLSL index identically.
	float ch[kMaxChannels * 4] = {};
	float &f(int slot) { return ch[slot * 4]; }
	float f(int slot) const { return ch[slot * 4]; }
	float *v(int slot) { return &ch[slot * 4]; }
	const float *v(int slot) const { return &ch[slot * 4]; }
};

struct FieldResources {};  // Plan A: no CPU-side resource sampling yet

// A stage's CPU mirror DECLARES the names it binds, through VE_STAGE_SLOTS and
// VE_STAGE_PARAMS below, and PipelineFieldGenerator::create resolves each one against the
// resolved pipeline. That is the whole point: the mirror used to index a positional
// extra[] array whose ordering rule ("writes in declaration order, then reads") lived in
// comments, so reordering a //!out line silently rebound every later index. A name the
// manifest does not declare now fails the load with the stage and the name in the message.
//
// The two blobs are built once at create() -- an int per declared slot, a float per
// declared param, in declaration order -- so the trampoline's cast reads exactly what the
// generator wrote, and per-sample cost is unchanged.
using StageFn = void (*)(FieldCtx &, const void *slots, const void *params,
		const FieldResources &);

struct StageBinding {
	std::string symbol;
	StageFn fn = nullptr;
	std::string slot_names;   // "p, sdf, height" -- exactly as spelled in VE_STAGE_SLOTS
	std::string param_names;  // "amp_a, amp_b, amp_c"; empty for a stage with no params
};

// Splits a stringised __VA_ARGS__ list into trimmed names. Empty input yields no names.
std::vector<std::string> split_binding_names(const std::string &list);

class StageLibrary {
public:
	static StageLibrary &instance();
	void register_stage(const StageBinding &b);
	const StageBinding *lookup(const std::string &symbol) const;  // nullptr when absent
private:
	std::vector<StageBinding> entries_;
};

struct StageRegistrar {
	StageRegistrar(const char *symbol, StageFn fn, const char *slot_names,
			const char *param_names);
};
} // namespace ve

// Declares the slot struct AND the name list the resolver uses. Name every channel the
// mirror touches, `p` included -- there are no built-ins to remember, because every name
// goes through ResolvedPipeline::channel_slot the same way.
#define VE_STAGE_SLOTS(Stage, ...)                        \
	struct Stage##Slots { int __VA_ARGS__; };             \
	inline const char *ve_slot_names(Stage##Slots *) { return #__VA_ARGS__; }

// __VA_OPT__ (C++20) is what lets a stage with no params declare an empty struct.
#define VE_STAGE_PARAMS(Stage, ...)                       \
	struct Stage##Params { __VA_OPT__(float __VA_ARGS__;) }; \
	inline const char *ve_param_names(Stage##Params *) { return #__VA_ARGS__; }

// `symbol` is the //!cpu name in the manifest; `Stage` is the prefix used above.
#define VE_REGISTER_STAGE(symbol, Stage, fn)                                          \
	static void ve_tramp_##fn(::ve::FieldCtx &ctx, const void *slots, const void *params, \
			const ::ve::FieldResources &res) {                                        \
		fn(ctx, *static_cast<const Stage##Slots *>(slots),                            \
				*static_cast<const Stage##Params *>(params), res);                    \
	}                                                                                 \
	static ::ve::StageRegistrar ve_stage_reg_##fn(symbol, &ve_tramp_##fn,             \
			ve_slot_names(static_cast<Stage##Slots *>(nullptr)),                      \
			ve_param_names(static_cast<Stage##Params *>(nullptr)))
```

- [ ] **Step 4: Rewrite `stage_library.cpp`**

Replace `extension/src/terrain/stage_library.cpp` entirely:

```cpp
#include "terrain/stage_library.h"

namespace ve {

std::vector<std::string> split_binding_names(const std::string &list) {
	std::vector<std::string> out;
	std::string cur;
	auto flush = [&]() {
		const size_t b = cur.find_first_not_of(" \t");
		if (b == std::string::npos) { cur.clear(); return; }
		out.push_back(cur.substr(b, cur.find_last_not_of(" \t") - b + 1));
		cur.clear();
	};
	for (char c : list) {
		if (c == ',') flush();
		else cur += c;
	}
	flush();
	return out;
}

StageLibrary &StageLibrary::instance() {
	// Function-local static: registration happens from other translation units' static
	// initializers, and this is the standard way to dodge the static init order fiasco.
	static StageLibrary lib;
	return lib;
}

void StageLibrary::register_stage(const StageBinding &b) {
	for (auto &e : entries_)
		if (e.symbol == b.symbol) { e = b; return; }
	entries_.push_back(b);
}

const StageBinding *StageLibrary::lookup(const std::string &symbol) const {
	for (const auto &e : entries_)
		if (e.symbol == symbol) return &e;
	return nullptr;
}

StageRegistrar::StageRegistrar(const char *symbol, StageFn fn, const char *slot_names,
		const char *param_names) {
	StageBinding b;
	b.symbol = symbol;
	b.fn = fn;
	b.slot_names = slot_names;
	b.param_names = param_names;
	StageLibrary::instance().register_stage(b);
}

} // namespace ve
```

- [ ] **Step 5: Rewrite the generator's binding**

In `extension/src/terrain/pipeline_field_generator.h`, replace the three member vectors

```cpp
	std::vector<StageFn> fns_;              // parallel to pipeline_.stages
	std::vector<StageSlots> slots_;         // parallel to pipeline_.stages
	std::vector<float> param_values_;       // flattened, in pipeline_.params order
	std::vector<int> param_base_;           // parallel to stages: first param index
	std::vector<int> param_count_;
```

with:

```cpp
	std::vector<StageFn> fns_;                  // parallel to pipeline_.stages
	// One blob per stage, in the mirror's declared order. Separate int and float vectors
	// rather than one byte buffer, so each blob's alignment matches the struct that reads
	// it. A stage with no params still gets one padding float: the trampoline forms a
	// reference to the blob, and an empty vector's data() may be null.
	std::vector<std::vector<int>> slot_words_;
	std::vector<std::vector<float>> param_words_;
```

In `extension/src/terrain/pipeline_field_generator.cpp`, replace the body of the stage loop in `create` (from `StageFn fn = nullptr;` through `cursor += int(s.params.size());`, and delete the `int cursor = 0;` above the loop and the `for (const ParamDecl &pm : p.params) g->param_values_.push_back(pm.value);` below it) with:

```cpp
	for (const StageManifest &s : p.stages) {
		if (s.cpu_symbol.empty()) {
			// GPU-only stage: the CPU field is already inexact, and sample() skips it.
			g->fns_.push_back(nullptr);
			g->slot_words_.emplace_back();
			g->param_words_.emplace_back(1, 0.0f);
			continue;
		}
		const StageBinding *b = StageLibrary::instance().lookup(s.cpu_symbol);
		if (b == nullptr) {
			if (error) *error = "stage '" + s.name + "' names an unregistered cpu symbol: " +
					s.cpu_symbol;
			delete g;
			return nullptr;
		}
		g->fns_.push_back(b->fn);

		std::vector<int> slots;
		for (const std::string &n : split_binding_names(b->slot_names)) {
			const int slot = p.channel_slot(n);
			if (slot < 0) {
				if (error) *error = "stage '" + s.name + "' cpu mirror binds channel '" + n +
						"', which this pipeline does not declare";
				delete g;
				return nullptr;
			}
			slots.push_back(slot);
		}
		g->slot_words_.push_back(slots);

		std::vector<float> params;
		for (const std::string &n : split_binding_names(b->param_names)) {
			const int idx = find_param(p, s.name, n);
			if (idx < 0) {
				if (error) *error = "stage '" + s.name + "' cpu mirror binds param '" + n +
						"', which neither this stage nor a //!use declares";
				delete g;
				return nullptr;
			}
			params.push_back(p.params[size_t(idx)].value);
		}
		if (params.empty()) params.push_back(0.0f);
		g->param_words_.push_back(params);
	}
```

Add to the file's anonymous namespace, above `create`:

```cpp
namespace {
// A mirror's param name is either this stage's own ("<stage>.<name>") or a cross-stage use
// spelled the way the GLSL spells it -- "hills_amp_a" for "hills.amp_a", which is exactly
// how the params UBO flattens them. Task: //!use declares which of these are legal; this
// only has to find the value.
int find_param(const ResolvedPipeline &p, const std::string &stage, const std::string &name) {
	const std::string own = stage + "." + name;
	for (size_t i = 0; i < p.params.size(); i++)
		if (p.params[i].name == own) return int(i);
	for (size_t i = 0; i < p.params.size(); i++) {
		std::string ident = p.params[i].name;
		for (char &c : ident) if (c == '.') c = '_';
		if (ident == name) return int(i);
	}
	return -1;
}
} // namespace
```

and replace the stage-dispatch loop inside `View::sample` with:

```cpp
	for (size_t i = 0; i < owner_->fns_.size(); i++) {
		StageFn fn = owner_->fns_[i];
		if (fn == nullptr) continue;  // GPU-only stage: the CPU field is already inexact
		FieldResources res;
		fn(ctx, owner_->slot_words_[i].data(), owner_->param_words_[i].data(), res);
	}
```

- [ ] **Step 6: Migrate the four mirrors**

Replace the body of `extension/src/terrain/builtin_stages.cpp` (keeping its `#include`s, adding `#include "terrain/pipeline.h"` is not needed) with:

```cpp
// The CPU mirrors of shaders/stages/*.field.glslh. Each function must stay line-for-line
// equivalent to its GLSL twin; tests/test_field_diff.gd is what catches drift.
//
// Each mirror DECLARES the channels and params it binds. PipelineFieldGenerator::create
// resolves those names against the manifest, so a name the manifest does not declare fails
// the load instead of silently binding whatever slot sat at that index.
#include "terrain/stage_library.h"
#include "generator/generator.h"  // ve::kSurfaceY
#include "world/material_table.h"
#include <cmath>

namespace {
// The analytic generator's height bands, by name: rock above 4 m, grass above 1 m, ground below.
constexpr uint16_t kBandRock = ve::material_id("rock");
constexpr uint16_t kBandGrass = ve::material_id("grass_01");
constexpr uint16_t kBandGround = ve::material_id("ground_01");
} // namespace

namespace ve {

VE_STAGE_SLOTS(Hills, p, sdf, height);
VE_STAGE_PARAMS(Hills, amp_a, amp_b, amp_c);

void stage_hills(FieldCtx &ctx, const HillsSlots &s, const HillsParams &p,
		const FieldResources &) {
	const float x = ctx.v(s.p)[0], y = ctx.v(s.p)[1], z = ctx.v(s.p)[2];
	const float h = p.amp_a * sinf(x * 0.11f) * cosf(z * 0.13f)
	              + p.amp_b * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	              + p.amp_c * sinf(x * 0.23f + z * 0.19f);
	ctx.f(s.height) = h;
	ctx.f(s.sdf) = y - kSurfaceY - h;
}

VE_STAGE_SLOTS(Cave, p, sdf);
VE_STAGE_PARAMS(Cave, cx, cz, depth, radius, hills_amp_a, hills_amp_b, hills_amp_c);

void stage_cave(FieldCtx &ctx, const CaveSlots &s, const CaveParams &p,
		const FieldResources &) {
	const float h = p.hills_amp_a * sinf(p.cx * 0.11f) * cosf(p.cz * 0.13f)
	              + p.hills_amp_b * sinf(p.cx * 0.031f + 1.7f) * sinf(p.cz * 0.043f)
	              + p.hills_amp_c * sinf(p.cx * 0.23f + p.cz * 0.19f);
	const float cy = kSurfaceY + h - p.depth;
	const float dx = ctx.v(s.p)[0] - p.cx, dy = ctx.v(s.p)[1] - cy, dz = ctx.v(s.p)[2] - p.cz;
	const float sphere = sqrtf(dx * dx + dy * dy + dz * dz) - p.radius;
	ctx.f(s.sdf) = fmaxf(ctx.f(s.sdf), -sphere);
}

VE_STAGE_SLOTS(HeightBands, sdf, height, material);
VE_STAGE_PARAMS(HeightBands);

void stage_height_bands(FieldCtx &ctx, const HeightBandsSlots &s, const HeightBandsParams &,
		const FieldResources &) {
	if (ctx.f(s.sdf) > 0.0f) { ctx.f(s.material) = 0.0f; return; }
	const float h = ctx.f(s.height);
	ctx.f(s.material) = h > 4.0f ? float(kBandRock) : (h > 1.0f ? float(kBandGrass) : float(kBandGround));
}

VE_STAGE_SLOTS(Relief, p, sdf, height);
VE_STAGE_PARAMS(Relief, amp_a, freq_a, amp_b, freq_b);

void stage_relief(FieldCtx &ctx, const ReliefSlots &s, const ReliefParams &p,
		const FieldResources &) {
	const float x = ctx.v(s.p)[0], z = ctx.v(s.p)[2];
	const float r = p.amp_a * sinf(x * p.freq_a) * cosf(z * p.freq_a)
	              + p.amp_b * sinf(x * p.freq_b) * cosf(z * p.freq_b);
	ctx.f(s.height) += r;
	ctx.f(s.sdf) -= r;
}

VE_REGISTER_STAGE("ve::stage_hills", Hills, stage_hills);
VE_REGISTER_STAGE("ve::stage_cave", Cave, stage_cave);
VE_REGISTER_STAGE("ve::stage_height_bands", HeightBands, stage_height_bands);
VE_REGISTER_STAGE("ve::stage_relief", Relief, stage_relief);

} // namespace ve
```

**Note on `stage_cave`:** its three `hills_amp_*` params do not exist yet — Task 9 adds the `//!use` directive that declares them. Until then `find_param` will not find them and `create` will fail. So this step ALSO requires the Task 9 manifest change to land here. Do it now: add to `shaders/stages/cave.field.glslh`, directly after its `//!param radius` line:

```glsl
//!use       hills.amp_a
//!use       hills.amp_b
//!use       hills.amp_c
```

and add the parser branch in `extension/src/terrain/stage_manifest.cpp`, next to the `key == "param"` branch:

```cpp
		else if (key == "use") {
			if (rest.find('.') == std::string::npos)
				return fail("//!use needs '<stage>.<param>': " + rest);
			out->uses.push_back(rest);
		}
```

with `std::vector<std::string> uses;` added to `StageManifest` in `stage_manifest.h`, under a comment:

```cpp
	// Params this stage reads from ANOTHER stage, as "<stage>.<param>". The GLSL reads
	// them through the flattened ident (P.hills_amp_a); declaring them here is what lets
	// the resolver reject an undeclared cross-stage read and what carries the value into
	// the CPU mirror's blob. Task 9's resolve check is what makes the declaration binding.
	std::vector<std::string> uses;
```

Task 9 adds the rejection; this step only adds the declaration so the cave mirror can bind.

- [ ] **Step 7: Run everything**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
```

Expected: PASS, including all three `test_stage_bindings` cases, and **`default_pipeline_field.txt` unchanged** — the cave's three literals `6.0f/3.0f/1.0f` are now read from `hills`'s resolved params, whose defaults are exactly `6.0/3.0/1.0`, so the field is bit-identical. If that corpus moves, stop: it means the params did not resolve to the values the literals had.

- [ ] **Step 8: Confirm the positional accessors are gone**

```bash
rg 'extra\[[0-9]\]|p\.at\([0-9]\)' extension/src/terrain
```

Expected: no output.

- [ ] **Step 9: Run the GPU diff**

```bash
./gdunit_tests.sh -a res://tests/test_field_diff.gd,res://tests/test_brick_diff.gd
```

Expected: PASS for both pipelines. The GLSL is untouched; this commit is CPU-side only.

- [ ] **Step 10: Commit**

```bash
git add extension/src/terrain/stage_library.h extension/src/terrain/stage_library.cpp \
        extension/src/terrain/pipeline_field_generator.h extension/src/terrain/pipeline_field_generator.cpp \
        extension/src/terrain/builtin_stages.cpp extension/src/terrain/stage_manifest.h \
        extension/src/terrain/stage_manifest.cpp shaders/stages/cave.field.glslh \
        extension/tests/test_stage_bindings.cpp
git commit -m "refactor: CPU mirrors bind stage channels and params by name

The mirrors indexed a positional extra[] array whose ordering rule -- writes in
declaration order, then reads, with sdf declared both ways collapsing to one slot
-- lived in comments at three call sites. Reordering a //!out line silently
rebound every later index and nothing failed.

Each mirror now declares the names it binds through VE_STAGE_SLOTS /
VE_STAGE_PARAMS, and create() resolves them against the resolved pipeline: an
undeclared name fails the load naming the stage and the name. The blobs are built
once, so per-sample cost is unchanged.

stage_cave's three hardcoded hills amplitudes are gone, read through //!use
instead. All four mirrors move together because StageFn's signature change is
atomic -- this amends the spec's 'one stage per commit'.

The CPU corpus is unchanged: hills' param defaults are the values the literals had."
```

---

### Task 9: Reject undeclared cross-stage reads

Task 8 added `//!use` and made the cave mirror read through it. Nothing yet stops a *new* stage from reaching into `P.hills_amp_a` without declaring it — which is how the CPU/GPU divergence the footgun comment warns about gets reintroduced.

**Files:**
- Modify: `extension/src/terrain/pipeline.cpp` (`resolve_pipeline`)
- Modify: `extension/tests/test_pipeline_resolve.cpp`
- Create: `extension/tests/test_cross_stage_param.cpp`
- Modify: `assets/pipelines/default.pipeline` (delete the footgun comment)

**Interfaces:**
- Consumes: `StageManifest::uses` (Task 8), `StageManifest::body`, `ResolvedPipeline::params`.
- Produces: no new symbols.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_pipeline_resolve.cpp`:

```cpp
TEST_CASE("a body reading another stage's param without //!use is rejected, naming the token") {
	std::vector<ve::StageManifest> st{
		field_stage("hills", {"sdf"}, {}),
		field_stage("cave", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[0].params.push_back({"amp_a", ve::ChannelType::kFloat, 6.0f});
	st[1].lipschitz_mode = ve::LipschitzMode::kMul; st[1].lipschitz = 1.0f;
	st[1].body = "void stage_cave(inout FieldCtx c){ c.sdf = P.hills_amp_a; }\n";

	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("hills_amp_a") != std::string::npos);
	CHECK(err.find("cave") != std::string::npos);
	CHECK(err.find("//!use") != std::string::npos);
}

TEST_CASE("a declared //!use accepts the same body") {
	std::vector<ve::StageManifest> st{
		field_stage("hills", {"sdf"}, {}),
		field_stage("cave", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[0].params.push_back({"amp_a", ve::ChannelType::kFloat, 6.0f});
	st[1].lipschitz_mode = ve::LipschitzMode::kMul; st[1].lipschitz = 1.0f;
	st[1].body = "void stage_cave(inout FieldCtx c){ c.sdf = P.hills_amp_a; }\n";
	st[1].uses.push_back("hills.amp_a");

	ve::ResolvedPipeline p;
	std::string err;
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
}

TEST_CASE("a //!use naming a param no stage declares is rejected") {
	std::vector<ve::StageManifest> st{field_stage("cave", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.0f;
	st[0].uses.push_back("hills.amp_a");
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("hills.amp_a") != std::string::npos);
}

TEST_CASE("a stage reads its own params without declaring anything") {
	std::vector<ve::StageManifest> st{field_stage("hills", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[0].params.push_back({"amp_a", ve::ChannelType::kFloat, 6.0f});
	st[0].body = "void stage_hills(inout FieldCtx c){ c.sdf = P.hills_amp_a; }\n";
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(1), st, &p, &err), err);
}

TEST_CASE("a param name inside a comment is not a read") {
	std::vector<ve::StageManifest> st{
		field_stage("hills", {"sdf"}, {}),
		field_stage("cave", {"sdf"}, {"sdf"}),
	};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd; st[0].lipschitz = 1.78f;
	st[0].params.push_back({"amp_a", ve::ChannelType::kFloat, 6.0f});
	st[1].lipschitz_mode = ve::LipschitzMode::kMul; st[1].lipschitz = 1.0f;
	st[1].body = "// once read P.hills_amp_a; it does not any more\n"
			"void stage_cave(inout FieldCtx c){ c.sdf = 1.0; }\n";
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
}
```

- [ ] **Step 2: Run to confirm they fail**

```bash
cd extension && scons -Q test -tc="a body reading another stage's param without //!use is rejected, naming the token"
```

Expected: the first, third and fifth cases FAIL (nothing scans bodies yet); the second and fourth pass vacuously.

- [ ] **Step 3: Implement the scan**

In `extension/src/terrain/pipeline.cpp`, add to the anonymous namespace:

```cpp
// Every "P.<ident>" token in a stage body, with // comments stripped first so a name
// mentioned in prose is not a read. A preceding identifier character means this is the
// tail of a longer name (XP.foo), not a params access.
std::vector<std::string> param_reads(const std::string &body) {
	std::vector<std::string> out;
	std::string src;
	src.reserve(body.size());
	for (size_t i = 0; i < body.size();) {
		if (body[i] == '/' && i + 1 < body.size() && body[i + 1] == '/') {
			while (i < body.size() && body[i] != '\n') i++;
		} else {
			src += body[i++];
		}
	}
	auto ident_char = [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '_';
	};
	for (size_t i = 0; i + 1 < src.size(); i++) {
		if (src[i] != 'P' || src[i + 1] != '.') continue;
		if (i > 0 && ident_char(src[i - 1])) continue;
		size_t j = i + 2;
		while (j < src.size() && ident_char(src[j])) j++;
		if (j > i + 2) out.push_back(src.substr(i + 2, j - i - 2));
		i = j - 1;
	}
	return out;
}

// "hills.amp_a" -> "hills_amp_a", the way generate_field_glslh flattens it into the UBO.
std::string flat_ident(const std::string &dotted) {
	std::string s = dotted;
	for (char &c : s) if (c == '.') c = '_';
	return s;
}
```

In `resolve_pipeline`, the check needs every stage's params flattened before any body is scanned, so add a second pass **after** the existing stage loop and before the `if (!wrote_sdf)` check:

```cpp
	// Cross-stage parameter reads must be declared. The GLSL reads another stage's param
	// through the flattened ident (P.hills_amp_a) and the resolver used not to see that at
	// all -- so overriding hills.amp_a in a pipeline file silently diverged the CPU mirror,
	// which had the old value as a literal. Declaring the read is what carries the value
	// into the mirror's blob.
	for (size_t i = 0; i < out->stages.size(); i++) {
		const StageManifest &m = out->stages[i];
		for (const std::string &use : m.uses) {
			bool found = false;
			for (const ParamDecl &pd : out->params)
				if (pd.name == use) { found = true; break; }
			if (!found)
				return fail("stage '" + m.name + "' declares //!use " + use +
						", but no stage in this pipeline declares that param");
		}
		for (const std::string &read : param_reads(m.body)) {
			bool ok = false;
			for (const ParamDecl &pd : m.params)
				if (flat_ident(m.name + "." + pd.name) == read) { ok = true; break; }
			for (const std::string &use : m.uses)
				if (flat_ident(use) == read) { ok = true; break; }
			if (!ok)
				return fail("stage '" + m.name + "' reads P." + read +
						", which is neither its own param nor a declared //!use");
		}
	}
```

Note this runs over `out->stages`, whose `params` already carry the pipeline's overrides applied in the first loop.

- [ ] **Step 4: Run the unit tests**

```bash
cd extension && scons -Q test
```

Expected: all five new cases PASS, and `default.pipeline` still resolves (the cave declared its three `//!use` lines in Task 8).

- [ ] **Step 5: Write the override test**

This is the bug the footgun comment describes, now provable. Create `extension/tests/test_cross_stage_param.cpp`:

```cpp
// The footgun default.pipeline used to carry as a comment: "overriding hills amp_* also
// requires updating the literals in stage_cave's CPU mirror". The cave's GLSL reads
// P.hills_amp_*, so a pipeline override moved the GPU field and left the CPU field on the
// old constants. The mirror binds the resolved value now; this proves it.
#include <doctest/doctest.h>
#include "terrain/pipeline_field_generator.h"
#include "terrain/pipeline_load.h"
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace {

bool repo_reader(const std::string &path, std::string *out) {
	std::ifstream f(path);
	if (!f.good()) return false;
	std::ostringstream o;
	o << f.rdbuf();
	*out = o.str();
	return true;
}

// The real stage manifests, with a pipeline built in memory so the override is the only
// thing that differs between the two generators.
std::unique_ptr<ve::PipelineFieldGenerator> build(const std::string &pipeline_text) {
	const std::string root(VE_REPO_ROOT);
	ve::TextReader reader = [&](const std::string &path, std::string *out) {
		if (path == "<memory>") { *out = pipeline_text; return true; }
		return repo_reader(path, out);
	};
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(reader, "<memory>", root + "/shaders/", &p, &err), err);
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	return std::unique_ptr<ve::PipelineFieldGenerator>(g);
}

const char *kBase =
		"seed 1337\n"
		"lipschitz 2.0\n"
		"stage stages/hills.field.glslh\n"
		"stage stages/cave.field.glslh\n"
		"stage stages/height_bands.field.glslh\n";

const char *kOverridden =
		"seed 1337\n"
		"lipschitz 2.0\n"
		"stage stages/hills.field.glslh\n"
		"  amp_a 2.0\n"
		"stage stages/cave.field.glslh\n"
		"stage stages/height_bands.field.glslh\n";

} // namespace

TEST_CASE("overriding hills.amp_a moves the cave's CPU mirror too") {
	auto base = build(kBase);
	auto over = build(kOverridden);

	// A point on the cave's rim, where the carve's centre height depends on hills' amp_a.
	// If the mirror still held the old literal, this sample would be identical in both.
	const float x = 30.0f, y = 49.0f, z = 30.0f;
	CHECK(base->eval(x, y, z).sdf != doctest::Approx(over->eval(x, y, z).sdf));
}
```

- [ ] **Step 6: Run it**

```bash
cd extension && scons -Q test
```

Expected: PASS. If the two samples come out equal, the cave mirror is not reading the resolved value — stop and report rather than moving the sample point around until it differs.

- [ ] **Step 7: Delete the footgun comment**

In `assets/pipelines/default.pipeline`, delete these two lines:

```
# NOTE: overriding hills amp_* also requires updating the literals in stage_cave's CPU
# mirror (extension/src/terrain/builtin_stages.cpp) -- the cave GLSL reads P.hills_*.
```

- [ ] **Step 8: Build and run everything**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
./gdunit_tests.sh -a res://tests/test_field_diff.gd
```

Expected: PASS, corpus unchanged.

- [ ] **Step 9: Commit**

```bash
git add extension/src/terrain/pipeline.cpp extension/tests/test_pipeline_resolve.cpp \
        extension/tests/test_cross_stage_param.cpp assets/pipelines/default.pipeline
git commit -m "feat: reject cross-stage param reads that no //!use declares

A stage body reading P.<other_stage>_<param> was invisible to the resolver, so
overriding that param in a pipeline file moved the GPU field and left the CPU
mirror on its hardcoded literal -- the divergence default.pipeline carried as a
NOTE. resolve_pipeline now scans each body for P.<ident> (comments stripped) and
rejects any that is neither the stage's own param nor a declared //!use.

test_cross_stage_param.cpp proves the override now reaches the cave's mirror, and
the NOTE is deleted along with the bug."
```

---

### Task 10: `allow_gpu_only` says what it costs

`allow_gpu_only` accepts a stage with no CPU mirror and says nothing. Every CPU consumer then silently disagrees with the GPU.

**Files:**
- Modify: `extension/src/terrain/pipeline.h` (`ResolvedPipeline::warnings`)
- Modify: `extension/src/terrain/pipeline.cpp` (`resolve_pipeline`)
- Modify: `extension/src/terrain/pipeline_load.h`, `extension/src/terrain/pipeline_load.cpp`
- Modify: `extension/src/voxel_world.cpp` (`load_terrain_pipeline`)
- Modify: `extension/tests/test_pipeline_resolve.cpp`
- Modify: `extension/tests/{test_pipeline_load,test_pipeline_equivalence,test_field_codegen_golden,test_default_pipeline_field,test_lipschitz_sampled,test_cross_stage_param}.cpp` (the new `load_pipeline` argument)

**Interfaces:**
- Consumes: `PipelineDesc::allow_gpu_only`, `ResolvedPipeline::cpu_exact` (existing).
- Produces:
  ```cpp
  // ResolvedPipeline gains:  std::vector<std::string> warnings;
  bool load_pipeline(const TextReader &, const std::string &pipeline_path,
                     const std::string &stage_root, ResolvedPipeline *out,
                     std::vector<std::string> *warnings, std::string *error);
  ```
  `warnings` may be null. Tasks 11-13 use the new signature.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_pipeline_resolve.cpp`:

```cpp
TEST_CASE("a GPU-only stage warns, naming the CPU consumers that will diverge") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {}, "")};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd;
	st[0].lipschitz = 1.0f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(1, true), st, &p, &err), err);
	CHECK_FALSE(p.cpu_exact);
	REQUIRE(p.warnings.size() == 1);
	CHECK(p.warnings[0].find("a") != std::string::npos);
	CHECK(p.warnings[0].find("collider") != std::string::npos);
	CHECK(p.warnings[0].find("island") != std::string::npos);
	CHECK(p.warnings[0].find("raycast") != std::string::npos);
}

TEST_CASE("a pipeline whose stages all have mirrors warns about nothing") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].lipschitz_mode = ve::LipschitzMode::kAdd;
	st[0].lipschitz = 1.0f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(1), st, &p, &err), err);
	CHECK(p.warnings.empty());
}
```

- [ ] **Step 2: Run to confirm it fails**

```bash
cd extension && scons -Q test
```

Expected: FAIL at compile — `warnings` is not a member of `ve::ResolvedPipeline`.

- [ ] **Step 3: Add the field and the warning**

In `extension/src/terrain/pipeline.h`, in `struct ResolvedPipeline`, add after `bool cpu_exact = true;`:

```cpp
    // Non-fatal problems the caller should surface. Resolve stays pure C++ (no godot-cpp),
    // so it collects text rather than calling push_warning itself.
    std::vector<std::string> warnings;
```

and add `#include <string>` if not already present (it is, via `stage_manifest.h`).

In `extension/src/terrain/pipeline.cpp`, in `resolve_pipeline`, replace:

```cpp
		if (m.cpu_symbol.empty()) {
			if (!desc.allow_gpu_only)
				return fail("stage '" + m.name + "' has no //!cpu mirror; set allow_gpu_only "
						"to accept a GPU-authoritative field");
			out->cpu_exact = false;
		}
```

with:

```cpp
		if (m.cpu_symbol.empty()) {
			if (!desc.allow_gpu_only)
				return fail("stage '" + m.name + "' has no //!cpu mirror; set allow_gpu_only "
						"to accept a GPU-authoritative field");
			out->cpu_exact = false;
			// allow_gpu_only is a deliberate opt-in, so this is not an error -- but it
			// stops being silent. Everything that evaluates the field on the CPU will
			// disagree with what the player sees.
			out->warnings.push_back("stage '" + m.name + "' has no //!cpu mirror, so these "
					"CPU consumers will diverge from the rendered field: collider meshing, "
					"island extraction, raycast, and consolidation");
		}
```

- [ ] **Step 4: Run the unit tests**

```bash
cd extension && scons -Q test
```

Expected: the two new cases PASS.

- [ ] **Step 5: Plumb the warnings through the loader**

In `extension/src/terrain/pipeline_load.h`, change the declaration to:

```cpp
// `warnings` may be null. On success it receives ResolvedPipeline::warnings, so a caller
// that has somewhere to put them (VoxelWorld pushes them through push_warning) does not
// have to reach into the resolved pipeline for them.
bool load_pipeline(const TextReader &reader, const std::string &pipeline_path,
		const std::string &stage_root, ResolvedPipeline *out,
		std::vector<std::string> *warnings, std::string *error);
```

adding `#include <vector>`. In `pipeline_load.cpp`, change the signature to match and replace the final `return resolve_pipeline(desc, loaded, out, error);` with:

```cpp
	if (!resolve_pipeline(desc, loaded, out, error)) return false;
	if (warnings != nullptr) *warnings = out->warnings;
	return true;
}
```

- [ ] **Step 6: Update the six call sites**

Pass `nullptr` for `warnings` in `test_pipeline_load.cpp` (all four cases), `test_pipeline_equivalence.cpp`, `test_field_codegen_golden.cpp`, `test_default_pipeline_field.cpp`, `test_lipschitz_sampled.cpp` and `test_cross_stage_param.cpp`. In `test_pipeline_load.cpp`, add one case that exercises the out-param:

```cpp
TEST_CASE("load_pipeline hands the resolver's warnings to its caller") {
	const char *gpu_only =
			"//!stage g\n//!kind field\n//!out sdf : float\n//!lipschitz add 1.0\n"
			"void stage_g(inout FieldCtx c){ c.sdf = c.p.y; }\n";
	const std::map<std::string, std::string> files{
		{"pipe/a.pipeline", "allow_gpu_only 1\nstage stages/g.field.glslh\n"},
		{"root/stages/g.field.glslh", gpu_only},
	};
	ve::ResolvedPipeline p;
	std::vector<std::string> warnings;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &p,
			&warnings, &err), err);
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].find("collider") != std::string::npos);
}
```

- [ ] **Step 7: Surface them in `VoxelWorld`**

In `extension/src/voxel_world.cpp`, in `load_terrain_pipeline`, change the `load_pipeline` call from Task 4 to collect and push:

```cpp
	ve::ResolvedPipeline resolved;
	std::vector<std::string> warnings;
	if (!ve::load_pipeline(
			[](const std::string &path, std::string *text) {
				return read_res_text(String(path.c_str()), text);
			},
			terrain_pipeline_path_.utf8().get_data(), "res://shaders/", &resolved, &warnings,
			&err)) {
		UtilityFunctions::push_error(String("terrain pipeline: ") + err.c_str());
		return;
	}
	for (const std::string &w : warnings)
		UtilityFunctions::push_warning(String("terrain pipeline: ") + w.c_str());
```

- [ ] **Step 8: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
```

Expected: PASS. No shipped pipeline has a GPU-only stage, so nothing is pushed at run time.

- [ ] **Step 9: Commit**

```bash
git add extension/src/terrain/pipeline.h extension/src/terrain/pipeline.cpp \
        extension/src/terrain/pipeline_load.h extension/src/terrain/pipeline_load.cpp \
        extension/src/voxel_world.cpp extension/tests/test_pipeline_resolve.cpp \
        extension/tests/test_pipeline_load.cpp extension/tests/test_pipeline_equivalence.cpp \
        extension/tests/test_field_codegen_golden.cpp extension/tests/test_default_pipeline_field.cpp \
        extension/tests/test_lipschitz_sampled.cpp extension/tests/test_cross_stage_param.cpp
git commit -m "feat: allow_gpu_only warns, naming the CPU consumers that will diverge

Accepting a stage with no //!cpu mirror silently put collider meshing, island
extraction, raycast and consolidation on a different field from the one the
player sees. It is still a deliberate opt-in, but resolve now collects a warning
naming those consumers and VoxelWorld pushes it."
```

---

### Task 11: `PipelineFieldGenerator` is a `Generator`

`FieldGenerator` wraps a `Generator` and every one of ~30 consumers reaches through `generator()->sampler()`. The inner `View` exists only to hand out that reference.

**Files:**
- Modify: `extension/src/terrain/pipeline_field_generator.h`, `extension/src/terrain/pipeline_field_generator.cpp`
- Delete: `extension/src/generator/field_generator.h`
- Modify: `extension/src/core/world_store.h:92,95,99,102,145,261`, `extension/src/core/world_store.cpp:5,9,25,28`
- Modify: `extension/src/voxel_world.cpp:431,607-616,766,909,927`
- Modify: `extension/src/mesh/consolidation.cpp:281,465`
- Modify: `extension/src/debug/hooks_world.cpp`, `hooks_physics.cpp`, `hooks_lod.cpp`, `hooks_render.cpp`
- Modify: `extension/tests/test_pipeline_field_generator.cpp`, `test_pipeline_equivalence.cpp`, `test_default_pipeline_field.cpp`, `test_lipschitz_sampled.cpp`, `test_stage_bindings.cpp`, `test_cross_stage_param.cpp` (`eval` → `sample`, `sampler().lipschitz()` → `lipschitz()`)

**Interfaces:**
- Consumes: `ve::Generator` (existing).
- Produces: `PipelineFieldGenerator : public Generator` with `sample`, `sample_gradient`, `lipschitz`, `is_cpu_exact`, `pipeline`. `FieldGenerator`, `ProceduralFieldGenerator` and `PipelineFieldGenerator::eval`/`sampler` no longer exist. `WorldStore::generator()` returns `ve::Generator *` (still a pointer, so the existing null check at `hooks_world.cpp:1238` keeps working).

- [ ] **Step 1: Collapse the class**

In `extension/src/terrain/pipeline_field_generator.h`, replace `#include "generator/field_generator.h"` with `#include "generator/generator.h"`, and replace the class declaration through the end of the private `View` block with:

```cpp
class PipelineFieldGenerator : public Generator {
public:
	static PipelineFieldGenerator *create(const ResolvedPipeline &p, std::string *error);

	Sample sample(float x, float y, float z) const override;

	// Central differences over the pipeline field, exactly as Generator::sample_gradient
	// computes them -- but reported EXACT. A stage pipeline has no analytic gradient;
	// finite differences are the gradient on both sides (the generated GLSL does the same
	// taps in base_field_gradient), and every exactness consumer treats the flag as
	// "usable as the surface normal", which this is to ~1e-4 away from a CSG crease -- and
	// at a crease both sides differentiate the identical taps, so they agree with each
	// other. Reporting inexact here would empty the consolidation normal payload and the
	// island fallback, and demote raymarch shading to the R8 fallback everywhere.
	FieldSample sample_gradient(float x, float y, float z) const override;

	float lipschitz() const override { return pipeline_.lipschitz; }
	bool is_cpu_exact() const { return pipeline_.cpu_exact; }
	const ResolvedPipeline &pipeline() const { return pipeline_; }

private:
	ResolvedPipeline pipeline_;
	std::vector<StageFn> fns_;
	std::vector<std::vector<int>> slot_words_;
	std::vector<std::vector<float>> param_words_;
};
```

In `extension/src/terrain/pipeline_field_generator.cpp`, rename `PipelineFieldGenerator::View::sample` to `PipelineFieldGenerator::sample` and `View::sample_gradient` to `PipelineFieldGenerator::sample_gradient`, replacing every `owner_->` with a direct member access and `const ResolvedPipeline &p = owner_->pipeline_;` with `const ResolvedPipeline &p = pipeline_;`.

- [ ] **Step 2: Delete the seam and retype the store**

```bash
git rm extension/src/generator/field_generator.h
```

In `extension/src/core/world_store.h`, replace `#include "generator/field_generator.h"` with `#include "generator/generator.h"`, and change the four `ve::FieldGenerator` mentions to `ve::Generator`. Update the comment at line 102 to:

```cpp
	// succeeds; until then the store has no generator and nothing may sample the field.
```

At line 145, `generator_ ? &generator_->sampler() : nullptr` becomes `generator_`.

In `extension/src/core/world_store.cpp`, change the constructor and setter to take `ve::Generator *` and drop both `new ve::ProceduralFieldGenerator()` fallbacks — a null generator stays null:

```cpp
WorldStore::WorldStore(const ve::WorldConfig &config, ve::Generator *generator)
	: ...
	  generator_(generator) {
```

```cpp
void WorldStore::set_generator(ve::Generator *generator) {
	...
	generator_ = generator;
}
```

(keep every other line of both functions, including whatever deletes the previous generator).

- [ ] **Step 3: Collapse the call sites**

There are two shapes to rewrite and the order matters. A site that took the *address* of the old reference (`job.gen = &store_->generator()->sampler();`) wants the bare pointer, while a site that bound a reference (`const ve::Generator &gen = store_->generator()->sampler();`) wants a dereference. Rewriting naively turns the first into `&store_->*generator()`, which does not compile.

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything
# Address-of sites first, reference sites second. The two prefixes are the only ones
# in the tree: `store_->` in voxel_world/consolidation, `world_->context().store->`
# in the hooks.
rg -l 'generator\(\)->sampler\(\)' extension/src | xargs sed -i '' \
  -e 's/&\(store_->\|world_->context()\.store->\)generator()->sampler()/\1generator()/g' \
  -e 's/\(store_->\|world_->context()\.store->\)generator()->sampler()/*\1generator()/g'
```

On Linux use `sed -i` without the `''`. This rewrites `extension/src/voxel_world.cpp`, `extension/src/mesh/consolidation.cpp` and the four `extension/src/debug/hooks_*.cpp`. Verify no site was missed and none was mangled:

```bash
rg 'generator\(\)->sampler\(\)|->\*generator\(\)' extension/src
```

Expected: no output. Anything the two patterns missed will also be named by the compiler in Step 6; hand-fix those to `*<expr>->generator()` for a reference or `<expr>->generator()` for a pointer.

Then fix the construction at `voxel_world.cpp:431`:

```cpp
	store_ = std::make_unique<WorldStore>(ve::WorldConfig{}, nullptr);
```

- [ ] **Step 4: Make a failed load abort init**

In `extension/src/voxel_world.cpp`, change `void VoxelWorld::load_terrain_pipeline()` to `bool VoxelWorld::load_terrain_pipeline()` (and its declaration in `voxel_world.h`), returning `false` at every early-return that is a failure and `true` at the end and at the already-loaded guard. Replace the function's header comment with:

```cpp
// Compiles the terrain pipeline into (a) a generated field.glslh installed as a shader-source
// override and (b) a PipelineFieldGenerator on the seam. Returns false when anything fails.
//
// A failure ABORTS world init rather than falling back. There used to be a fallback -- the
// hardcoded AnalyticGenerator -- and it was worse than no terrain: the GPU would be on the
// pipeline's field or the stub while the CPU was on the analytic one, which is the silent
// CPU/GPU divergence the whole pipeline design exists to prevent. A world that refuses to
// start says so; a world that generates different terrain than its pipeline declares does not.
//
// First successful load wins (the stages-nonempty guard): shader-reload re-init re-runs
// ensure_initialized, and replacing the generator deletes the old seam, so a reload-time swap
// could pull the field out from under in-flight physics/mesh jobs. Pipeline edits therefore take
// effect on fresh init, where nothing can hold the old seam mid-evaluation.
```

In `ensure_initialized`, change `load_terrain_pipeline();` to:

```cpp
	if (!load_terrain_pipeline()) return;
```

and do the same at the second call site (`voxel_world.cpp:711`).

- [ ] **Step 5: Update the tests that used `eval`/`sampler`**

In `extension/tests/test_pipeline_equivalence.cpp`, `test_default_pipeline_field.cpp`, `test_lipschitz_sampled.cpp`, `test_stage_bindings.cpp` and `test_cross_stage_param.cpp`, replace `->eval(` with `->sample(`. In `test_pipeline_equivalence.cpp`, `g->sampler().lipschitz()` becomes `g->lipschitz()`. In `test_pipeline_field_generator.cpp`, replace the case `"sampler() hands out a Generator view with the pipeline's lipschitz bound"` with:

```cpp
TEST_CASE("the generator reports the pipeline's lipschitz bound") {
	// ... keep the existing pipeline setup for this case ...
	CHECK(g->lipschitz() == doctest::Approx(3.5f));
}
```

- [ ] **Step 6: Build and run everything**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
./gdunit_tests.sh
```

Expected: native PASS with the corpus unchanged; gdUnit matches the Task 1 baseline. The risk here is a suite that reached the generator before init completed and used to get the fallback — it would now null-deref. Every shipped suite uses the default pipeline path, which loads, so this should not fire; **if a suite crashes rather than fails, stop and report the suite name** instead of adding null checks at the call sites.

- [ ] **Step 7: Confirm the seam is gone**

```bash
rg 'FieldGenerator|ProceduralFieldGenerator|->sampler\(\)' extension/src
```

Expected: no output (`material_atlas.h`'s unrelated `sampler()` is not called through `->sampler()` on a generator; if the grep shows those render call sites, confirm they are `materials.sampler()` and move on).

- [ ] **Step 8: Commit**

```bash
git add -A extension/src extension/tests
git commit -m "refactor: PipelineFieldGenerator is a Generator; delete the FieldGenerator seam

FieldGenerator wrapped a Generator and every one of ~30 consumers reached through
generator()->sampler(); the inner View class existed only to hand out that
reference. The generator is a Generator now and WorldStore holds one directly.

A failed pipeline load aborts world init instead of falling back to the hardcoded
analytic terrain. The fallback was worse than no terrain: it put the GPU on the
pipeline's field and the CPU on a different one, which is the silent divergence
the pipeline design exists to prevent."
```

---

### Task 12: `AnalyticGenerator` becomes the test oracle

It is the fourth copy of the terrain and, since Task 11, nothing in `src` constructs it.

**Files:**
- Create: `extension/tests/analytic_oracle.h`, `extension/tests/analytic_oracle.cpp`
- Modify: `extension/src/generator/generator.h`, `extension/src/generator/generator.cpp`
- Modify: `extension/tests/test_field_baseline.cpp`, `test_brick_baseline.cpp`, `test_pipeline_equivalence.cpp`

**Interfaces:**
- Consumes: `ve::Generator`, `ve::Sample`, `ve::FieldSample`, `ve::kSurfaceY` — all staying in `generator.h`.
- Produces: `ve::AnalyticGenerator` in `extension/tests/analytic_oracle.h`. No `src` file may include it.

- [ ] **Step 1: Move the class**

Create `extension/tests/analytic_oracle.h`:

```cpp
#pragma once
// TEST-ONLY. The terrain the engine shipped with, kept as the oracle that
// assets/pipelines/golden.pipeline is proved against (test_pipeline_equivalence.cpp) and
// that tests/golden/field_baseline.txt and brick_baseline.txt are pinned to.
//
// It lived in src/generator/ as the fallback field, which made it a fourth copy of the
// terrain: stage GLSL, stage C++ mirror, the field.glslh stub, and this. Nothing in the
// engine generates terrain from it any more, and nothing in src/ may include this header.
#include "generator/generator.h"

namespace ve {

// Deterministic analytic terrain: sine hills + one carved spherical cave.
class AnalyticGenerator : public Generator {
public:
	explicit AnalyticGenerator(uint32_t seed = 1337) : seed_(seed) {}
	Sample sample(float x, float y, float z) const override;
	FieldSample sample_gradient(float x, float y, float z) const override;

	// |grad(y - hills)| = sqrt(1 + |grad hills|^2); the amplitude-times-frequency sum of
	// hills() is below 1.0 per axis, so 2.0 is comfortably conservative. The cave is a
	// unit-gradient sphere combined with max(), which cannot raise the bound.
	float lipschitz() const override { return 2.0f; }

private:
	uint32_t seed_;
};

} // namespace ve
```

Create `extension/tests/analytic_oracle.cpp` holding the `hills()` helper, the `kBand*` constants and the two `AnalyticGenerator::` member definitions moved verbatim out of `extension/src/generator/generator.cpp`, with `#include "analytic_oracle.h"` replacing its `#include "generator/generator.h"`.

- [ ] **Step 2: Trim `src`**

In `extension/src/generator/generator.h`, delete the `class AnalyticGenerator` block (keeping `kSurfaceY`, `Sample`, `FieldSample` and `class Generator`). In `extension/src/generator/generator.cpp`, delete the `hills()` helper, the `kBand*` constants, both `AnalyticGenerator::` definitions and the now-unused `#include "world/material_table.h"`, keeping `Generator::sample_gradient`.

- [ ] **Step 3: Point the three tests at the oracle**

Add `#include "analytic_oracle.h"` to `extension/tests/test_field_baseline.cpp`, `test_brick_baseline.cpp` and `test_pipeline_equivalence.cpp`. The `tests/*.cpp` glob in `SConstruct` picks up `analytic_oracle.cpp` with no build edit; the test build's include path already covers `extension/tests`, so if the include fails to resolve, use `#include "tests/analytic_oracle.h"` rather than editing `SConstruct`.

- [ ] **Step 4: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
```

Expected: PASS. `field_baseline.txt` and `brick_baseline.txt` are unchanged — the class moved, its arithmetic did not.

- [ ] **Step 5: Confirm the exit criterion**

```bash
rg 'AnalyticGenerator' extension/src
```

Expected: no output.

- [ ] **Step 6: Commit**

```bash
git add -A extension/src/generator extension/tests
git commit -m "refactor: AnalyticGenerator moves to the test suite as the oracle

It was the fourth copy of the terrain -- stage GLSL, stage C++ mirror, the
field.glslh stub, and this -- and since the FieldGenerator seam went, nothing in
src constructs it. It stays as what golden.pipeline is proved against and what
field_baseline.txt and brick_baseline.txt are pinned to.

generator.h keeps kSurfaceY, Sample, FieldSample and Generator. The two corpora
are unchanged: the class moved, its arithmetic did not."
```

---

### Task 13: Add a stage, and count the files

The exit criteria are a measurement, and this is the measurement.

**Files:**
- Create: `shaders/stages/mesas.field.glslh`
- Create: `assets/pipelines/mesas.pipeline`
- Modify: `extension/src/terrain/builtin_stages.cpp`

**Interfaces:**
- Consumes: `VE_STAGE_SLOTS`, `VE_STAGE_PARAMS`, `VE_REGISTER_STAGE` (Task 8), `MAT_ROCK` (SP4's generated `shaders/material_table.glslh`), `ve::material_id` (existing).
- Produces: a third pipeline, which Task 2's suite picks up with no edit.

- [ ] **Step 1: Write the stage**

Create `shaders/stages/mesas.field.glslh`:

```glsl
//!stage     mesas
//!kind      field
//!in        sdf : float
//!in        material : uint
//!out       sdf : float
//!out       mesa_mask : float
//!out       material : uint
//!param     spacing : float = 320.0
//!param     lift : float = 45.0
//!param     plateau : float = 0.5
//!lipschitz add 1.3
//!cpu       ve::stage_mesas

// Plateaus on a square lattice: where both axis sines are positive, lift the terrain. The
// mask is published as its own channel so a later stage can band on it without recomputing
// the product.
//
// Bound: the lift term is lift * sin(kx) * sin(kz) with k = 2*pi/spacing, so each axis
// derivative is at most lift * k = 45 * 0.019635 = 0.8836 and |grad| <= sqrt(2) * 0.8836 =
// 1.2496. Declared 1.3. max(0, u) has a crease but no slope steeper than u's own, so it
// does not raise the bound. This stage ADDS a term to sdf, hence `add`.
//
// mesas.pipeline declares no ceiling, so it resolves to hills 1.78 + relief 0.21 + 1.3 =
// 3.29 and pays for it in raycast steps -- which is the honest trade, and what a ceiling
// exists to let an author refuse.
void stage_mesas(inout FieldCtx ctx) {
	float k = 6.2831853 / P.mesas_spacing;
	float u = sin(ctx.p.x * k) * sin(ctx.p.z * k);
	float mask = max(0.0, u);
	ctx.mesa_mask = mask;
	ctx.sdf -= P.mesas_lift * mask;
	if (mask > P.mesas_plateau && ctx.sdf <= 0.0) ctx.material = MAT_ROCK;
}
```

- [ ] **Step 2: Write the mirror**

Append to `extension/src/terrain/builtin_stages.cpp`, inside `namespace ve`, before the `VE_REGISTER_STAGE` block:

```cpp
VE_STAGE_SLOTS(Mesas, p, sdf, mesa_mask, material);
VE_STAGE_PARAMS(Mesas, spacing, lift, plateau);

void stage_mesas(FieldCtx &ctx, const MesasSlots &s, const MesasParams &p,
		const FieldResources &) {
	const float k = 6.2831853f / p.spacing;
	const float u = sinf(ctx.v(s.p)[0] * k) * sinf(ctx.v(s.p)[2] * k);
	const float mask = fmaxf(0.0f, u);
	ctx.f(s.mesa_mask) = mask;
	ctx.f(s.sdf) -= p.lift * mask;
	if (mask > p.plateau && ctx.f(s.sdf) <= 0.0f) ctx.f(s.material) = float(kBandRock);
}
```

and add to the registration block:

```cpp
VE_REGISTER_STAGE("ve::stage_mesas", Mesas, stage_mesas);
```

- [ ] **Step 3: Write the pipeline**

Create `assets/pipelines/mesas.pipeline`:

```
# The "add a stage" scenario from the roadmap's Appendix A, and the multi-channel fixture
# tests/test_field_diff.gd needs: mesas publishes its own channel and places a material.
#
# Deliberately NOT part of default.pipeline. Demo terrain and every golden stay where they
# are, and this file is picked up by the field diff automatically.
#
# No `lipschitz` line: with no ceiling declared, resolve reports the computed bound
# (1.78 + 0.21 + 1.3 = 3.29). A steep stage costs raycast steps; declaring a ceiling here
# is how an author would refuse to pay them.
seed      1337

stage stages/hills.field.glslh
stage stages/relief.field.glslh
stage stages/cave.field.glslh
stage stages/height_bands.field.glslh
stage stages/mesas.field.glslh
```

- [ ] **Step 4: Build and run both suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
./gdunit_tests.sh -a res://tests/test_field_diff.gd
```

Expected: native PASS (including `test_lipschitz_sampled`, which only covers the two shipped pipelines — `mesas.pipeline` is not added there, because its bound is deliberately loose and the check belongs to what ships). gdUnit PASS for **three** pipelines, `mesas.pipeline` among them — which is the proof the CPU mirror and the GLSL agree.

- [ ] **Step 5: Verify the ceiling refuses an unsafe budget**

Add `lipschitz 2.0` to `assets/pipelines/mesas.pipeline`, then add this temporary case to `extension/tests/test_lipschitz_sampled.cpp` so the refusal is readable without a GPU run:

```cpp
TEST_CASE("a pipeline whose stages exceed its ceiling is refused, naming every contributor") {
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::load_pipeline(repo_reader, root + "/assets/pipelines/mesas.pipeline",
			root + "/shaders/", &p, nullptr, &err));
	MESSAGE(err);
	CHECK(err.find("hills") != std::string::npos);
	CHECK(err.find("relief") != std::string::npos);
	CHECK(err.find("mesas") != std::string::npos);
}
```

```bash
cd extension && scons -Q test && ./build/tests/ve_tests \
	-tc="a pipeline whose stages exceed its ceiling is refused, naming every contributor" -s
```

Expected: PASS, with the `MESSAGE(err)` output printing `pipeline gradient bound 3.29... exceeds the declared ceiling 2.0...` and a line per contributor with its mode. **That message is the deliverable of this whole sub-project — capture it verbatim for the results report.**

Then confirm the same refusal reaches the running engine:

```bash
./gdunit_tests.sh -a res://tests/test_field_diff.gd
```

Expected: FAIL on `mesas.pipeline` — the world refuses to initialise — with the same text in the Godot log via `push_error`.

Finally remove both the temporary test case and the `lipschitz 2.0` line, and re-run to green:

```bash
cd extension && scons -Q test
./gdunit_tests.sh -a res://tests/test_field_diff.gd
```

- [ ] **Step 6: Count the files**

```bash
git status --porcelain
```

Expected: exactly three paths — `shaders/stages/mesas.field.glslh`, `assets/pipelines/mesas.pipeline`, `extension/src/terrain/builtin_stages.cpp`. Record the count. No golden regenerated, no test edited, no pipeline file of the shipped terrain touched.

Then trace "material with hardness placed by terrain" on paper from this stage: `mesas` places `MAT_ROCK` (hardness 3.0) through the generated constant, so an *existing* material costs the same three files. A *new* material additionally needs `extension/src/world/material_table.h`, the regenerated `shaders/material_table.glslh`, `tools/convert_materials.sh` and the PNGs under `assets/materials/`. Record the honest number rather than asserting the target.

- [ ] **Step 7: Commit**

```bash
git add shaders/stages/mesas.field.glslh assets/pipelines/mesas.pipeline \
        extension/src/terrain/builtin_stages.cpp
git commit -m "feat: mesas stage, the Appendix A trace

Three files: the stage GLSL, its CPU mirror, and a pipeline that uses it. No
golden regenerated, no test edited, no shipped pipeline touched -- the field diff
picks mesas.pipeline up by enumeration.

It publishes its own mesa_mask channel and places MAT_ROCK through the generated
constant, so it exercises channel extension, an additive Lipschitz declaration
and material placement at once. It declares no ceiling and resolves to 3.29,
which is what a steep stage honestly costs."
```

---

### Task 14: Final regression gate and results report

**Files:**
- Create: `docs/superpowers/plans/2026-09-17-stage-authoring-results.md`
- Modify: `docs/superpowers/specs/2026-09-17-stage-authoring-design.md` (status and the two amendments)

**Interfaces:**
- Consumes: Task 1's baseline, and every preceding task's evidence.
- Produces: the results report the roadmap's acceptance rule requires.

- [ ] **Step 1: Full clean build and both suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) --test
./gdunit_tests.sh
```

- [ ] **Step 2: Diff against the baseline**

```bash
latest=$(ls -d reports/report_* | sort -V | tail -1)
python3 - "$latest/results.xml" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for suite in root.iter('testsuite'):
    print(f"# {suite.get('name')}: tests={suite.get('tests')} failures={suite.get('failures')}")
for tc in root.iter('testcase'):
    bad = tc.find('failure') if tc.find('failure') is not None else tc.find('error')
    if bad is not None:
        msg = (bad.get('message') or bad.text or '').strip().splitlines()
        print(f"{tc.get('classname')}::{tc.get('name')} — {msg[0] if msg else ''}")
EOF
```

Compare to `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md` by case name **and** message. A suite's case count dropping is a failure.

- [ ] **Step 3: Run every exit-criterion check**

```bash
rg 'extra\[[0-9]\]|p\.at\([0-9]\)' extension/src/terrain ; echo "--- expect nothing above"
rg 'class FieldGenerator|ProceduralFieldGenerator|AnalyticGenerator' extension/src ; echo "--- expect nothing above"
rg '\-\>sampler\(\)' extension/src ; echo "--- expect nothing above"
```

- [ ] **Step 4: Write the results report**

Create `docs/superpowers/plans/2026-09-17-stage-authoring-results.md`:

```markdown
# Stage authoring (sub-project 6) — results

**Spec:** `docs/superpowers/specs/2026-09-17-stage-authoring-design.md`
**Baseline:** `docs/superpowers/plans/2026-09-17-stage-authoring-baseline.md`
**Range:** `b03e5d6..<final sha>`

## Exit criteria

| Criterion | Check | Result |
|---|---|---|
| No positional slot/param access | `rg 'extra\[[0-9]\]\|p\.at\([0-9]\)' extension/src/terrain` | <output> |
| The generator seam is gone | `rg 'class FieldGenerator\|ProceduralFieldGenerator\|AnalyticGenerator' extension/src` | <output> |
| No generator reached through a view | `rg '\-\>sampler\(\)' extension/src` | <output> |
| New terrain stage ≤ 3 files | Task 13 Step 6 | <count and the three paths> |
| Material with hardness placed by terrain ≤ 4 files | Task 13 Step 6 | <honest count, existing vs new material> |
| Every shipped pipeline passes the sampled bound check | `test_lipschitz_sampled` | <result> |
| Suites match the baseline | Step 2 | <result> |

## The refusal message

<the verbatim output captured in Task 13 Step 5>

## Goldens

| Golden | Moved? | Cause |
|---|---|---|
| `tests/golden/default_pipeline_field.txt` | <y/n> | <cause or "unchanged"> |
| `tests/golden/field_baseline.txt` | <y/n> | <cause or "unchanged"> |
| `tests/golden/brick_baseline.txt` | <y/n> | <cause or "unchanged"> |
| `shaders/generated/field.glslh.golden` | <y/n> | <cause or "unchanged"> |

## Reported Lipschitz bounds

| Pipeline | Before | After | Ceiling |
|---|---|---|---|
| `default.pipeline` | 2.0 (declared) | <computed> | 2.0 |
| `golden.pipeline` | 2.0 (declared) | <computed> | 2.0 |
| `mesas.pipeline` | — | <computed> | none |

## gdUnit suites that moved

<per suite: case, before, after, cause — or "none">

## Deviations from the spec

1. All four CPU mirrors migrated in one commit rather than one per commit: `StageFn`'s
   signature change is atomic.
2. `VE_STAGE_SLOTS` takes the PascalCase struct prefix explicitly, because a macro cannot
   capitalise its argument.
3. The spec's `test_lipschitz_rule.cpp` is split in two: the combination-rule and ceiling
   cases live in `test_pipeline_resolve.cpp` beside the other resolver cases, and only the
   sampled check got its own file, `test_lipschitz_sampled.cpp`.
```

Fill every `<...>` with real output.

- [ ] **Step 5: Update the spec's status**

In `docs/superpowers/specs/2026-09-17-stage-authoring-design.md`, change the status line to:

```markdown
**Status:** Implemented; see docs/superpowers/plans/2026-09-17-stage-authoring-results.md
```

and append to §11 the two deviations listed above.

- [ ] **Step 6: Update the roadmap**

In `docs/superpowers/plans/2026-09-13-frame-module.md`, under "Sub-project 6 — Stage authoring", add a `**Status.**` line matching the style the other sub-projects use:

```markdown
**Status.** Implemented; see `docs/superpowers/plans/2026-09-17-stage-authoring-results.md`.
```

with any exit finding that came out OPEN named on the same line.

- [ ] **Step 7: Commit**

```bash
git add docs/superpowers/plans/2026-09-17-stage-authoring-results.md \
        docs/superpowers/specs/2026-09-17-stage-authoring-design.md \
        docs/superpowers/plans/2026-09-13-frame-module.md
git commit -m "docs: record stage authoring results and exit criteria"
```

---

## Acceptance checklist

- [ ] Native suite green; `tests/golden/default_pipeline_field.txt`, `field_baseline.txt`, `brick_baseline.txt` and `shaders/generated/field.glslh.golden` either unchanged or moved with an attributed cause in the results report.
- [ ] gdUnit matches Task 1's baseline by case name and message, with no suite's case count dropping.
- [ ] `rg 'extra\[[0-9]\]|p\.at\([0-9]\)' extension/src/terrain` returns nothing.
- [ ] `rg 'class FieldGenerator|ProceduralFieldGenerator|AnalyticGenerator' extension/src` returns nothing.
- [ ] A new terrain stage costs 3 files, demonstrated by Task 13 and counted with `git status --porcelain`.
- [ ] A pipeline whose stages exceed its ceiling is refused, with every contributor named, captured verbatim in the results report.
- [ ] Every shipped pipeline's declared bound holds against its real field at 4096 sample points.
- [ ] Spec status, results report and the roadmap's sub-project 6 entry agree.
