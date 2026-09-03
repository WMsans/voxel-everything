# Terrain Stage Pipeline — Field Program Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the hardcoded terrain field with a creator-authored pipeline of composable GLSL stages that compiles into one generated `field.glslh`, mirrored exactly on the CPU, reproducing today's terrain byte-for-byte.

**Architecture:** Field stages are GLSL functions over a generated `FieldCtx` struct, declared by an in-file `//!` manifest and wired by a plain-text pipeline resource. A pure-C++ compiler resolves channels, validates, and emits both the GLSL text (injected through the existing `ve::set_shader_source_override` seam) and an ordered list of C++ mirror functors behind `ve::FieldGenerator`. No existing shader's descriptor set 0 changes; the pipeline owns set 1.

**Tech Stack:** C++17, godot-cpp GDExtension, SCons, doctest (native suite), gdUnit4 (GPU suite), GLSL 460 compute.

**Spec:** `docs/superpowers/specs/2026-09-03-terrain-pipeline-design.md`

## Global Constraints

- **Milestone scope is Plan A only:** the field program. The `sector2d` map tier, the context scheduler and the sector cache are Plan B and MUST NOT be built here. Where this plan touches set 1, it lands the params UBO and an empty sector-map binding so Plan B has somewhere to plug in.
- **`extension/src/terrain/*.cpp` must stay pure C++** — no godot-cpp, no `RenderingDevice`, no GPU headers. It is added to `pure_sources` in `extension/SConstruct` and must compile in the zero-godot-cpp native test build.
- **The generated field must reproduce today's terrain exactly.** `ve::kSurfaceY = 51.2f` and the `hills()` coefficients in `extension/src/generator/generator.cpp` are load-bearing; Task 15's equivalence test is the gate.
- **`eval_field()` keeps its exact current shape** — base field, then override, then region ops in order. Only the call to `base_field` changes.
- **CPU/GPU tolerance** is the existing one from `tests/test_field_diff.gd`: `SDF_STEP = 1.28 / 255.0` metres per encoded step, no sample beyond `2.0` steps, `0.99` of samples within one step.
- **Baseline before blaming:** this repo has **5 known-failing assertions across 4 suites on a clean `main`**. Run `./gdunit_tests.sh` once before Task 1 and save the output; compare against it, never against "zero failures".
- Native suite: `./build.sh --test` (runs `scons -Q test` from `extension/`, CWD is `extension/`).
- GPU suite: `./gdunit_tests.sh -a res://tests/<suite>.gd`.

**Deferred from the spec to Plan B, deliberately** — these are spec requirements this plan does *not* satisfy, recorded here so they are not mistaken for oversights:

| Spec | Why it waits |
|---|---|
| §5.3 `sample_sector_r` helper generation | Plan A ships no sampled resources, so there is nothing to sample. Task 11 still *declares* resources in set 1 and Task 10 still resolves them, so Plan B adds the helper without reworking either. |
| §10.1 Lipschitz debug validator | The bound is combined (Task 10) and exposed through `sampler().lipschitz()` (Task 14). The sampling mode that *warns* on violation needs a GPU probe sweep and lands with Plan B. |
| §11.1 parameterizing `test_field_diff.gd` | Parameterizing over pipelines needs a second pipeline to exist. Plan A ships one and asserts it exactly (Task 15); Plan B introduces the second and the parameterization together. |

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `extension/src/terrain/stage_manifest.h/.cpp` | Parse the `//!` directive block and body out of one stage source |
| `extension/src/terrain/pipeline.h/.cpp` | Parse the `.pipeline` resource; resolve channels, resources, params; validate |
| `extension/src/terrain/field_codegen.h/.cpp` | Emit `field.glslh` text from a resolved pipeline |
| `extension/src/terrain/stage_library.h/.cpp` | Registry mapping `//!cpu` symbols to C++ mirror functors; `ve::FieldCtx` runtime |
| `extension/src/terrain/pipeline_field_generator.h/.cpp` | `ve::PipelineFieldGenerator`, implementing `ve::FieldGenerator` |
| `extension/src/render/field_context_set.h/.cpp` | Owns the set-1 uniform set: params UBO + sector-map placeholder |
| `shaders/stages/hills.field.glslh` | Ported sine-hills stage |
| `shaders/stages/cave.field.glslh` | Ported carved-sphere stage |
| `shaders/stages/height_bands.field.glslh` | Ported material banding stage |
| `assets/pipelines/default.pipeline` | The pipeline reproducing today's terrain |
| `tests/golden/field_baseline.txt` | Committed CPU field corpus (Task 1) |
| `tests/golden/brick_baseline.txt` | Committed CPU brick corpus (Task 2) |
| `shaders/generated/field.glslh.golden` | Committed generated source (Task 12) |

**Modified:** `extension/SConstruct`, `extension/src/physics/collider_streamer.{h,cpp}`, `extension/src/physics/island_manager.{h,cpp}`, `extension/src/render/island_extract_pass.cpp`, `extension/src/render/consolidate_pass.cpp`, `extension/src/debug/hooks.{h,cpp}`, `extension/src/voxel_world.{h,cpp}`, `extension/src/core/world_store.h`, `shaders/field.glslh`, `tests/test_field_diff.gd`.

---

# Phase 0 — Characterization

These four tasks change no behaviour. They exist so Phase 1's refactor is provably safe: every one of them must stay green through Tasks 5–7.

## Task 1: Golden CPU field corpus

**Files:**
- Create: `extension/tests/test_field_baseline.cpp`
- Create: `tests/golden/field_baseline.txt`
- Modify: `extension/SConstruct` (add `VE_REPO_ROOT` define)

**Interfaces:**
- Consumes: `ve::AnalyticGenerator::sample` from `extension/src/generator/generator.h`
- Produces: `tests/golden/field_baseline.txt`, read again by Task 3 from GDScript

- [ ] **Step 1: Add the repo-root define so the test finds the golden file regardless of CWD**

In `extension/SConstruct`, immediately before the `tests = test_env.Program(...)` line (currently line 28):

```python
test_env.Append(CPPDEFINES=[("VE_REPO_ROOT", '\\"' + Dir("#").abspath + '/..\\"')])
```

- [ ] **Step 2: Write the failing test**

Create `extension/tests/test_field_baseline.cpp`:

```cpp
// Characterization: pins ve::AnalyticGenerator's exact output before the terrain pipeline
// replaces it. Every value is compared as raw float BITS, not with a tolerance -- the point
// is that the ported pipeline reproduces today's terrain exactly, not approximately.
//
// Regenerate after an INTENTIONAL terrain change:  VE_REGEN_GOLDEN=1 ./build.sh --test
#include <doctest/doctest.h>
#include "generator/generator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Pt { float x, y, z; };

// Self-contained LCG: the corpus must not depend on any std:: RNG implementation.
std::vector<Pt> corpus() {
	std::vector<Pt> pts;
	uint32_t s = 20260903u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	for (int i = 0; i < 512; i++)
		pts.push_back({next(-20.0f, 60.0f), next(21.2f, 81.2f), next(-20.0f, 60.0f)});
	for (int i = 0; i < 128; i++)
		pts.push_back({next(700.0f, 900.0f), next(11.2f, 71.2f), next(700.0f, 900.0f)});
	return pts;
}

uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

std::string golden_path() { return std::string(VE_REPO_ROOT) + "/tests/golden/field_baseline.txt"; }

} // namespace

TEST_CASE("analytic field matches the committed baseline bit for bit") {
	ve::AnalyticGenerator g;
	const std::vector<Pt> pts = corpus();

	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		FILE *f = std::fopen(golden_path().c_str(), "w");
		REQUIRE(f != nullptr);
		std::fprintf(f, "# ve::AnalyticGenerator baseline. Columns: x y z sdf (hex float bits), material.\n");
		std::fprintf(f, "# Regenerate: VE_REGEN_GOLDEN=1 ./build.sh --test\n");
		for (const Pt &p : pts) {
			ve::Sample s = g.sample(p.x, p.y, p.z);
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
		ve::Sample s = g.sample(pts[i].x, pts[i].y, pts[i].z);
		CHECK(bits(s.sdf) == bsdf);
		CHECK(unsigned(s.material) == mat);
		i++;
	}
	std::fclose(f);
	CHECK(i == pts.size());
}
```

- [ ] **Step 3: Run it and watch it fail on the missing golden**

Run: `./build.sh --test`
Expected: FAIL — `missing golden; run VE_REGEN_GOLDEN=1 ./build.sh --test`

- [ ] **Step 4: Generate the golden, then re-run**

```bash
mkdir -p tests/golden
VE_REGEN_GOLDEN=1 ./build.sh --test
./build.sh --test
```

Expected: PASS, and `tests/golden/field_baseline.txt` holds 640 data lines.

- [ ] **Step 5: Verify the golden actually bites**

Temporarily change `6.0f` to `6.001f` in `hills()` in `extension/src/generator/generator.cpp`, run `./build.sh --test`, confirm FAIL, then revert the edit and confirm PASS. A characterization test that cannot fail is worthless.

- [ ] **Step 6: Commit**

```bash
git add extension/tests/test_field_baseline.cpp tests/golden/field_baseline.txt extension/SConstruct
git commit -m "test: pin the analytic field to a committed baseline corpus"
```

## Task 2: Golden CPU brick corpus

**Files:**
- Create: `extension/tests/test_brick_baseline.cpp`
- Create: `tests/golden/brick_baseline.txt`

**Interfaces:**
- Consumes: `ve::eval_brick(const Generator &, const EditOp *, int, IVec3, BrickEval *, ...)` from `extension/src/world/brick_eval.h`; `ve::BrickEval` holds `.brick` (`ve::Brick`) and `.mips` (`ve::BrickMips`)
- Produces: `tests/golden/brick_baseline.txt`; Task 15 reuses this test unchanged as its equivalence gate

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_brick_baseline.cpp`:

```cpp
// Characterization: pins whole-brick output (SDF lattice, materials, palette, mips) for a
// fixed set of bricks. Task 1 pins the field as a function; this pins everything eval_brick
// derives from it, which is what actually reaches the atlas.
//
// Regenerate after an INTENTIONAL terrain change:  VE_REGEN_GOLDEN=1 ./build.sh --test
#include <doctest/doctest.h>
#include "world/brick_eval.h"
#include "generator/generator.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Bricks are 0.8 m; these straddle the surface at y = 51.2 m, the carved cave at (30,~50,30),
// deep solid ground, and open sky -- one brick per distinct field regime.
const ve::IVec3 kBricks[] = {
	{0, 64, 0}, {15, 63, 15}, {37, 63, 37}, {37, 62, 37},
	{-25, 60, -25}, {0, 40, 0}, {0, 90, 0}, {1000, 64, 1000},
};

// FNV-1a over every byte a brick contributes to the atlas.
uint64_t brick_hash(const ve::BrickEval &e) {
	uint64_t h = 1469598103934665603ull;
	auto feed = [&h](const void *p, size_t n) {
		const unsigned char *b = static_cast<const unsigned char *>(p);
		for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
	};
	feed(e.brick.sdf, sizeof(e.brick.sdf));
	feed(e.brick.mat, sizeof(e.brick.mat));
	feed(e.brick.palette, sizeof(e.brick.palette));
	feed(&e.mips, sizeof(e.mips));
	return h;
}

std::string golden_path() { return std::string(VE_REPO_ROOT) + "/tests/golden/brick_baseline.txt"; }

} // namespace

TEST_CASE("eval_brick output matches the committed baseline") {
	ve::AnalyticGenerator g;
	const int n = int(sizeof(kBricks) / sizeof(kBricks[0]));
	std::vector<uint64_t> got;
	for (int i = 0; i < n; i++) {
		ve::BrickEval e{};
		ve::eval_brick(g, nullptr, 0, kBricks[i], &e);
		got.push_back(brick_hash(e));
	}

	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		FILE *f = std::fopen(golden_path().c_str(), "w");
		REQUIRE(f != nullptr);
		std::fprintf(f, "# ve::eval_brick baseline. Columns: bx by bz fnv1a-64.\n");
		std::fprintf(f, "# Regenerate: VE_REGEN_GOLDEN=1 ./build.sh --test\n");
		for (int i = 0; i < n; i++)
			std::fprintf(f, "%d %d %d %016llx\n", kBricks[i].x, kBricks[i].y, kBricks[i].z,
					(unsigned long long)got[size_t(i)]);
		std::fclose(f);
		MESSAGE("regenerated " << golden_path());
	}

	FILE *f = std::fopen(golden_path().c_str(), "r");
	REQUIRE_MESSAGE(f != nullptr, "missing golden; run VE_REGEN_GOLDEN=1 ./build.sh --test");
	char line[256];
	int i = 0;
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		if (line[0] == '#' || line[0] == '\n') continue;
		int bx, by, bz;
		unsigned long long h;
		REQUIRE(std::sscanf(line, "%d %d %d %llx", &bx, &by, &bz, &h) == 4);
		REQUIRE(i < n);
		CHECK(bx == kBricks[i].x);
		CHECK(by == kBricks[i].y);
		CHECK(bz == kBricks[i].z);
		CHECK(got[size_t(i)] == uint64_t(h));
		i++;
	}
	std::fclose(f);
	CHECK(i == n);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `./build.sh --test`
Expected: FAIL — missing golden.

If it instead fails to COMPILE on `e.brick.sdf` / `e.brick.mat` / `e.brick.palette`, open `extension/src/world/brick.h` and correct the member names to whatever `ve::Brick` actually declares. Do not change the hashing approach.

- [ ] **Step 3: Generate the golden and re-run**

```bash
VE_REGEN_GOLDEN=1 ./build.sh --test
./build.sh --test
```

Expected: PASS, 8 data lines in `tests/golden/brick_baseline.txt`.

- [ ] **Step 4: Verify it bites**

Temporarily change `5.0f` to `5.001f` in the cave radius in `AnalyticGenerator::sample`, run `./build.sh --test`, confirm the brick at `{37, 63, 37}` fails, revert, confirm PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/tests/test_brick_baseline.cpp tests/golden/brick_baseline.txt
git commit -m "test: pin whole-brick evaluation to a committed baseline"
```

## Task 3: Pin the GPU field to the same corpus

**Files:**
- Create: `tests/test_field_baseline_gpu.gd`

**Interfaces:**
- Consumes: `tests/golden/field_baseline.txt` from Task 1; `_world.hooks().debug_load_shader("res://shaders/field_probe.comp.glsl")`, the dispatch idiom in `tests/test_field_diff.gd:61-120`
- Produces: nothing consumed by later tasks

`test_field_diff.gd` proves CPU and GPU *agree with each other*. It does not stop both drifting together. This pins the GPU to the same absolute corpus.

- [ ] **Step 1: Write the failing test**

Create `tests/test_field_baseline_gpu.gd`:

```gdscript
extends GdUnitTestSuite

# Characterization: the GPU field must match tests/golden/field_baseline.txt, the same corpus
# extension/tests/test_field_baseline.cpp pins the CPU against. test_field_diff.gd only proves
# the two agree with EACH OTHER, which both drifting together would satisfy.
#
# Tolerance is the established one: sin() is not bit-identical between glibc and a Vulkan
# driver, so the gate is expressed in encoded SDF steps.
const SDF_STEP := 1.28 / 255.0
const MAX_STEPS := 2.0
const TIGHT_FRACTION := 0.99

var _world: VoxelWorld
var _rd: RenderingDevice

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)
	_rd = RenderingServer.create_local_rendering_device()

func after_test() -> void:
	if _rd != null:
		_rd.free()
		_rd = null

func _bits_to_float(u: int) -> float:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(u)
	b.seek(0)
	return b.get_float()

func _load_golden() -> Dictionary:
	var f := FileAccess.open("res://tests/golden/field_baseline.txt", FileAccess.READ)
	assert_object(f).is_not_null()
	var pts := PackedVector3Array()
	var sdf := PackedFloat32Array()
	var mat := PackedInt32Array()
	while not f.eof_reached():
		var line := f.get_line().strip_edges()
		if line.is_empty() or line.begins_with("#"):
			continue
		var c := line.split(" ", false)
		assert_int(c.size()).is_equal(5)
		pts.append(Vector3(_bits_to_float(("0x" + c[0]).hex_to_int()),
			_bits_to_float(("0x" + c[1]).hex_to_int()),
			_bits_to_float(("0x" + c[2]).hex_to_int())))
		sdf.append(_bits_to_float(("0x" + c[3]).hex_to_int()))
		mat.append(int(c[4]))
	f.close()
	return {"pts": pts, "sdf": sdf, "mat": mat}

func test_gpu_field_matches_committed_baseline() -> void:
	var golden := _load_golden()
	var pts: PackedVector3Array = golden["pts"]
	assert_int(pts.size()).is_equal(640)

	# Reuse test_field_diff.gd's dispatch helper verbatim; it returns vec4 per point as
	# (sdf, material, 0, 0). Copy run_gpu() and its uniform-set construction from
	# tests/test_field_diff.gd:61 into this suite unchanged -- with an EMPTY op buffer and
	# op_count 0, so only the base field is exercised.
	var got: PackedFloat32Array = run_gpu(pts, PackedByteArray(), 0)
	assert_int(got.size()).is_equal(pts.size() * 4)

	var worst := 0.0
	var tight := 0
	for i in range(pts.size()):
		var d: float = absf(got[i * 4] - golden["sdf"][i]) / SDF_STEP
		worst = maxf(worst, d)
		if d <= 1.0:
			tight += 1
		assert_int(int(got[i * 4 + 1])).is_equal(golden["mat"][i])
	assert_float(worst).is_less_equal(MAX_STEPS)
	assert_float(float(tight) / float(pts.size())).is_greater_equal(TIGHT_FRACTION)
```

- [ ] **Step 2: Copy `run_gpu` into the new suite**

Open `tests/test_field_diff.gd`, copy the `run_gpu` function and any helper it calls (shader load, uniform set construction, buffer creation) verbatim into `tests/test_field_baseline_gpu.gd`, replacing the comment placeholder in `test_gpu_field_matches_committed_baseline`. Do not refactor `test_field_diff.gd` to share it — duplication here keeps the two suites independently trustworthy.

- [ ] **Step 3: Run it**

Run: `./gdunit_tests.sh -a res://tests/test_field_baseline_gpu.gd`
Expected: PASS. If material comparison fails on points far above terrain, confirm the golden's material column is `0` there — air is material 0 on both sides.

- [ ] **Step 4: Commit**

```bash
git add tests/test_field_baseline_gpu.gd
git commit -m "test: pin the GPU field to the committed baseline corpus"
```

## Task 4: Pin the generator's downstream consumers

**Files:**
- Modify: `extension/src/debug/hooks.h`, `extension/src/debug/hooks.cpp`
- Modify: `extension/src/voxel_world.cpp` (ClassDB bindings)
- Create: `tests/test_generator_seam.gd`

**Interfaces:**
- Produces: `VoxelWorld.hooks().debug_generator_fingerprint() -> PackedFloat32Array` — 3 floats per probe point `(sdf, material, 0)`, sampled through `store_->generator()->sampler()`. Tasks 5–7 assert this is unchanged.

The real risk in Phase 1 is injecting the wrong generator, or a null one, into a subsystem. This hook makes "every subsystem shares one generator, and it is the world's" directly observable.

- [ ] **Step 1: Add the hook declaration**

In `extension/src/debug/hooks.h`, beside the other diagnostic hooks:

```cpp
	// Characterization hook (terrain pipeline Phase 0): samples the world's generator
	// THROUGH THE SEAM at a fixed point set, so a refactor that swaps in a different or
	// null generator is caught by value rather than by inspection. 3 floats per point:
	// sdf, material, 0.
	PackedFloat32Array debug_generator_fingerprint();
```

- [ ] **Step 2: Implement it**

In `extension/src/debug/hooks.cpp`:

```cpp
PackedFloat32Array VoxelWorldHooks::debug_generator_fingerprint() {
	PackedFloat32Array out;
	if (world_ == nullptr || world_->store() == nullptr ||
			world_->store()->generator() == nullptr) {
		return out;
	}
	const ve::Generator &gen = world_->store()->generator()->sampler();
	// Same regimes as tests/golden/field_baseline.txt: surface, cave, deep, sky, far.
	static const float kPts[][3] = {
		{0.0f, 51.2f, 0.0f}, {12.3f, 55.0f, -7.8f}, {30.0f, 50.85f, 30.0f},
		{-30.0f, 50.0f, -30.0f}, {0.0f, 20.0f, 0.0f}, {0.0f, 90.0f, 0.0f},
		{800.0f, 51.2f, 800.0f}, {-800.0f, 51.2f, -800.0f},
	};
	for (const auto &p : kPts) {
		ve::Sample s = gen.sample(p[0], p[1], p[2]);
		out.push_back(s.sdf);
		out.push_back(float(s.material));
		out.push_back(0.0f);
	}
	return out;
}
```

Adjust `world_->store()` to whatever accessor `VoxelWorldHooks` already uses to reach the world's `WorldStore` — grep `hooks.cpp` for an existing `store()` call and match it exactly.

- [ ] **Step 3: Bind it**

In `extension/src/voxel_world.cpp`, in the same `ClassDB::bind_method` block as the neighbouring `debug_*` hooks:

```cpp
	ClassDB::bind_method(D_METHOD("debug_generator_fingerprint"),
			&VoxelWorldHooks::debug_generator_fingerprint);
```

Match the surrounding binding style exactly — if the hooks are bound on a separate `VoxelWorldHooks` class registration, add it there instead.

- [ ] **Step 4: Write the characterization test**

Create `tests/test_generator_seam.gd`:

```gdscript
extends GdUnitTestSuite

# Characterization (terrain pipeline Phase 0): pins the world generator's output, and pins
# that colliders still appear where they do today. Tasks 5-7 route ~21 direct
# ve::AnalyticGenerator constructions through the ve::FieldGenerator seam; if any of them
# ends up holding a different generator, or a null one, these values move.

# Captured from a clean main. If terrain changes INTENTIONALLY, regenerate by printing
# debug_generator_fingerprint() and pasting the values back here.
const EXPECTED_POINTS := 8
const SDF_EPS := 1e-5

var _world: VoxelWorld

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)

func test_generator_fingerprint_is_stable() -> void:
	var a: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	assert_int(a.size()).is_equal(EXPECTED_POINTS * 3)
	# Self-consistency: the seam must hand out the SAME generator every call.
	var b: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	for i in range(a.size()):
		assert_float(a[i]).is_equal_approx(b[i], SDF_EPS)

func test_fingerprint_matches_recorded_baseline() -> void:
	var got: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	var want := _baseline()
	assert_int(got.size()).is_equal(want.size())
	for i in range(got.size()):
		assert_float(got[i]).is_equal_approx(want[i], SDF_EPS)

func _baseline() -> PackedFloat32Array:
	# FILLED IN BY STEP 6 -- run the suite once, read the printed values, paste them here.
	return PackedFloat32Array([])
```

- [ ] **Step 5: Build and run to capture the baseline**

```bash
./build.sh
./gdunit_tests.sh -a res://tests/test_generator_seam.gd
```

Expected: `test_generator_fingerprint_is_stable` PASSES, `test_fingerprint_matches_recorded_baseline` FAILS on a size mismatch (0 vs 24).

- [ ] **Step 6: Record the baseline**

Add `print(got)` at the top of `test_fingerprint_matches_recorded_baseline`, re-run, and paste the 24 printed floats into `_baseline()`. Remove the `print`. Re-run; both tests must pass.

- [ ] **Step 7: Verify it bites**

Temporarily change the cave radius from `5.0f` to `6.0f` in `AnalyticGenerator::sample`, rebuild, run the suite, confirm `test_fingerprint_matches_recorded_baseline` FAILS. Revert, rebuild, confirm PASS.

- [ ] **Step 8: Commit**

```bash
git add extension/src/debug/hooks.h extension/src/debug/hooks.cpp extension/src/voxel_world.cpp tests/test_generator_seam.gd
git commit -m "test: pin the world generator fingerprint through the seam"
```

---

# Phase 1 — Enforce the seam

Tasks 1–4 must stay green through every task in this phase. Run the full set after each:

```bash
./build.sh --test && ./gdunit_tests.sh -a res://tests/test_generator_seam.gd -a res://tests/test_field_baseline_gpu.gd -a res://tests/test_collider_stream.gd -a res://tests/test_island_extract.gd
```

## Task 5: Inject the generator into the physics subsystems

**Files:**
- Modify: `extension/src/physics/collider_streamer.h:123`, `extension/src/physics/collider_streamer.cpp:545`
- Modify: `extension/src/physics/island_manager.h:183`, `extension/src/physics/island_manager.cpp:232`
- Modify: `extension/src/voxel_world.cpp` (both `initialize` call sites)

**Interfaces:**
- Consumes: `ve::FieldGenerator::sampler() -> const ve::Generator &` from `extension/src/generator/field_generator.h`; `WorldStore::generator() -> ve::FieldGenerator *`
- Produces: `ColliderStreamer::initialize(..., const ve::Generator *gen)` and `IslandManager` gaining `set_generator(const ve::Generator *gen)`

Both classes hold `ve::AnalyticGenerator gen_;` **by value** and use it in exactly one place each: `probe.gen = &gen_;`. The comment at `collider_streamer.h:121` already anticipates this change.

- [ ] **Step 1: Replace the by-value member in ColliderStreamer**

In `extension/src/physics/collider_streamer.h`, replace lines 121-123:

```cpp
	// Spec §9 defers a configurable generator; when G becomes one, this moves to VoxelWorld
	// and is handed in, exactly like the edit log.
	ve::AnalyticGenerator gen_;
```

with:

```cpp
	// Borrowed from WorldStore via VoxelWorld, exactly like edit_log_. Never owned: the
	// terrain pipeline can swap the world's generator, and a copy here would silently keep
	// generating the old world for collision while the GPU generated the new one.
	const ve::Generator *gen_ = nullptr;
```

Then change the `#include "generator/generator.h"` if the header no longer needs `AnalyticGenerator` — it still needs `ve::Generator`, so leave the include.

- [ ] **Step 2: Extend `initialize` to take it**

In `extension/src/physics/collider_streamer.h`, add a trailing parameter to the `initialize` declaration at line 34:

```cpp
	void initialize(ve::ChunkResidency *chunks, ve::EditLog *edit_log, std::mutex *edit_mutex,
			const ve::Generator *gen);
```

Keep every existing parameter in place and in order; append only. Mirror the change in the `.cpp` definition and assign `gen_ = gen;` in the body.

- [ ] **Step 3: Guard the use site**

At `extension/src/physics/collider_streamer.cpp:545`, replace `probe.gen = &gen_;` with:

```cpp
	if (gen_ == nullptr) return;  // not initialized: no field, no collider
	probe.gen = gen_;
```

Place the guard at the top of the enclosing function, not mid-body, so no partial work happens first.

- [ ] **Step 4: Do the same for IslandManager**

In `extension/src/physics/island_manager.h`, replace `ve::AnalyticGenerator gen_;` at line 183 with the same `const ve::Generator *gen_ = nullptr;` and comment. Add a public setter beside the other setters:

```cpp
	// Borrowed, not owned; see the member comment.
	void set_generator(const ve::Generator *gen) { gen_ = gen; }
```

At `extension/src/physics/island_manager.cpp:232`, apply the same null guard and `probe.gen = gen_;`.

- [ ] **Step 5: Wire both from VoxelWorld**

In `extension/src/voxel_world.cpp`, find the `ColliderStreamer::initialize(...)` call and append the argument:

```cpp
	&store_->generator()->sampler()
```

and beside the `IslandManager` construction/initialization, add:

```cpp
	island_manager_->set_generator(&store_->generator()->sampler());
```

`WorldStore::generator()` is never null — `world_store.cpp:11` and `:22` both substitute a `ProceduralFieldGenerator` — so no null check is needed at the call site.

- [ ] **Step 6: Build and run the full Phase 0 set**

```bash
./build.sh --test && ./gdunit_tests.sh -a res://tests/test_generator_seam.gd -a res://tests/test_collider_stream.gd -a res://tests/test_collider_edits.gd -a res://tests/test_island_body.gd
```

Expected: PASS, with no new failures against the baseline captured in Global Constraints.

- [ ] **Step 7: Commit**

```bash
git add extension/src/physics extension/src/voxel_world.cpp
git commit -m "refactor: physics borrows the world generator instead of owning a copy"
```

## Task 6: Inject the generator into the two render passes

**Files:**
- Modify: `extension/src/render/island_extract_pass.cpp:206`
- Modify: `extension/src/render/consolidate_pass.cpp:186`

**Interfaces:**
- Consumes: whatever job struct each site already has in scope (`job.snapshot` in `island_extract_pass`, `job.source` in `consolidate_pass`)
- Produces: a `const ve::Generator *gen` field on both job structs

Both construct `ve::AnalyticGenerator gen;` inside worker-thread job processing. The generator must arrive on the job, because the worker has no access to `WorldStore`.

- [ ] **Step 1: Add the field to the island-extract job struct**

In `extension/src/render/island_extract_pass.h`, add to `struct IslandExtractJob` (line 25), beside its `ve::FieldSourceSnapshot snapshot;` member at line 33:

```cpp
	// Borrowed from WorldStore, captured when the job was submitted on the main thread.
	// Never an owned AnalyticGenerator: the terrain pipeline can swap the world's field.
	const ve::Generator *gen = nullptr;
```

- [ ] **Step 2: Use it**

Replace `ve::AnalyticGenerator gen;` at line 206 with:

```cpp
			if (job.gen == nullptr) { out->data.normal_oct.clear(); return; }
			const ve::Generator &gen = *job.gen;
```

Match the surrounding early-return style — if the enclosing block is not a function body, set the same failure state the `!materialize(...)` branch above it sets and skip the loop instead of returning.

- [ ] **Step 3: Populate it at submission**

Grep for where this job is constructed and pushed to the worker queue. On the main thread, set `job.gen = &store_->generator()->sampler();` alongside the existing `job.snapshot` assignment.

- [ ] **Step 4: Repeat for consolidate_pass**

Apply Steps 1–3 identically to `extension/src/render/consolidate_pass.cpp:186`, adding the same `const ve::Generator *gen = nullptr;` member to `struct ConsolidateJob` in `extension/src/render/consolidate_pass.h` (line 13), beside its `ve::FieldSourceSnapshot source;` member at line 19.

- [ ] **Step 5: Build and test**

```bash
./build.sh --test && ./gdunit_tests.sh -a res://tests/test_island_extract.gd -a res://tests/test_consolidation.gd -a res://tests/test_generator_seam.gd
```

Expected: PASS with no new failures.

- [ ] **Step 6: Commit**

```bash
git add extension/src/render/island_extract_pass.cpp extension/src/render/island_extract_pass.h extension/src/render/consolidate_pass.cpp extension/src/render/consolidate_pass.h
git commit -m "refactor: extract and consolidate passes take the generator on the job"
```

## Task 7: Route debug/hooks.cpp through the seam

**Files:**
- Modify: `extension/src/debug/hooks.cpp` (17 occurrences)

**Interfaces:**
- Consumes: `world_->store()->generator()->sampler()`
- Produces: nothing; this is the last of the ~21 direct construction sites

- [ ] **Step 1: List every site**

```bash
grep -n "AnalyticGenerator" extension/src/debug/hooks.cpp
```

Expected: 17 lines.

- [ ] **Step 2: Replace each one**

For each site, replace the local construction:

```cpp
	ve::AnalyticGenerator gen;
```

with a borrow from the seam:

```cpp
	const ve::Generator &gen = world_->store()->generator()->sampler();
```

Two cautions. Where the variable is named something other than `gen` (there is at least one `agen` at the old line 2795), keep that name. Where the site takes the address (`&gen`) or passes it by pointer, the reference form still works — `&gen` is unchanged. Where a site is in a function with no `world_` in scope, add a `const ve::Generator &gen` parameter to that function and pass it from the caller rather than reaching for a global.

- [ ] **Step 3: Confirm none remain**

```bash
grep -c "AnalyticGenerator" extension/src/debug/hooks.cpp
```

Expected: `0`.

- [ ] **Step 4: Confirm the whole codebase is clean outside the generator module**

```bash
grep -rn "AnalyticGenerator" extension/src --include="*.cpp" --include="*.h" | grep -v "^extension/src/generator/"
```

Expected: no output. `extension/src/generator/` legitimately still defines and wraps it, and `extension/tests/` still tests it directly — both are correct.

- [ ] **Step 5: Full regression run**

```bash
./build.sh --test && ./gdunit_tests.sh
```

Expected: only the 5 known baseline failures from Global Constraints, no others.

- [ ] **Step 6: Commit**

```bash
git add extension/src/debug/hooks.cpp
git commit -m "refactor: debug hooks read the world generator through the seam"
```

---

# Phase 2 — Manifest and pipeline

## Task 8: Stage manifest parser

**Files:**
- Create: `extension/src/terrain/stage_manifest.h`, `extension/src/terrain/stage_manifest.cpp`
- Create: `extension/tests/test_stage_manifest.cpp`
- Modify: `extension/SConstruct:16-19` (add `src/terrain/*.cpp` to `pure_sources`)

**Interfaces:**
- Produces, consumed by Tasks 9–15:

```cpp
namespace ve {
enum class StageKind { kField, kMap };
enum class ChannelType { kFloat, kVec2, kVec3, kVec4, kInt, kUint };
int channel_component_count(ChannelType t);   // 1,2,3,4,1,1
const char *channel_glsl_type(ChannelType t); // "float","vec2","vec3","vec4","int","uint"

struct ChannelDecl { std::string name; ChannelType type = ChannelType::kFloat; };
struct ResourceDecl { std::string name, type; float fallback = 0.0f; };
struct ParamDecl { std::string name; ChannelType type = ChannelType::kFloat; float value = 0.0f; };

struct StageManifest {
    std::string name;
    StageKind kind = StageKind::kField;
    std::vector<ChannelDecl> reads, writes;
    std::vector<ResourceDecl> samples;
    std::vector<ParamDecl> params;
    float lipschitz = 1.0f;
    float bounds = 0.0f;
    int iterate = 1;
    std::string domain;
    int domain_w = 0, domain_h = 0;
    std::string cpu_symbol;  // empty => GPU-only
    std::string body;        // everything after the directive block, verbatim
};

bool parse_stage_manifest(const std::string &source, StageManifest *out, std::string *error);
}
```

- [ ] **Step 1: Add the terrain module to the pure build**

In `extension/SConstruct`, change line 16-19's `pure_sources` assignment to include the new directory:

```python
pure_sources = (Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") +
                Glob("src/core/*.cpp") + Glob("src/terrain/*.cpp") +
                Glob("src/mesh/*.cpp") + Glob("src/connectivity/*.cpp") +
                Glob("src/lod/*.cpp") + Glob("src/shade/*.cpp"))
```

Check whether the main (non-test) source list needs the same addition — grep the file for the other `Glob("src/world/*.cpp")` and add `src/terrain` there too.

- [ ] **Step 2: Write the failing test**

Create `extension/tests/test_stage_manifest.cpp`:

```cpp
#include <doctest/doctest.h>
#include "terrain/stage_manifest.h"

static const char *kHills =
	"//!stage     hills\n"
	"//!kind      field\n"
	"//!out       sdf : float\n"
	"//!param     amplitude : float = 6.0\n"
	"//!lipschitz 2.0\n"
	"//!cpu       ve::stage_hills\n"
	"\n"
	"void stage_hills(inout FieldCtx ctx) {\n"
	"\tctx.sdf = ctx.p.y;\n"
	"}\n";

TEST_CASE("parses every directive and keeps the body verbatim") {
	ve::StageManifest m;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_stage_manifest(kHills, &m, &err), err);
	CHECK(m.name == "hills");
	CHECK(m.kind == ve::StageKind::kField);
	REQUIRE(m.writes.size() == 1);
	CHECK(m.writes[0].name == "sdf");
	CHECK(m.writes[0].type == ve::ChannelType::kFloat);
	REQUIRE(m.params.size() == 1);
	CHECK(m.params[0].name == "amplitude");
	CHECK(m.params[0].value == doctest::Approx(6.0f));
	CHECK(m.lipschitz == doctest::Approx(2.0f));
	CHECK(m.cpu_symbol == "ve::stage_hills");
	CHECK(m.body.find("void stage_hills(inout FieldCtx ctx) {") != std::string::npos);
	CHECK(m.body.find("//!") == std::string::npos);
}

TEST_CASE("a stage with no //!cpu is GPU-only, not an error") {
	ve::StageManifest m;
	std::string err;
	std::string src = "//!stage s\n//!kind field\n//!out sdf : float\nvoid s(inout FieldCtx c){}\n";
	REQUIRE(ve::parse_stage_manifest(src, &m, &err));
	CHECK(m.cpu_symbol.empty());
}

TEST_CASE("reads, samples and map directives parse") {
	ve::StageManifest m;
	std::string err;
	std::string src =
		"//!stage   erosion\n"
		"//!kind    map\n"
		"//!domain  sector2d 256x256\n"
		"//!in      sector.height : image2d_r32f\n"
		"//!out     sector.flow : image2d_rg16f\n"
		"//!sample  world.mask : texture2d_r32f\n"
		"//!iterate 64\n"
		"//!bounds  12.5\n"
		"void erosion(){}\n";
	REQUIRE_MESSAGE(ve::parse_stage_manifest(src, &m, &err), err);
	CHECK(m.kind == ve::StageKind::kMap);
	CHECK(m.domain == "sector2d");
	CHECK(m.domain_w == 256);
	CHECK(m.domain_h == 256);
	CHECK(m.iterate == 64);
	CHECK(m.bounds == doctest::Approx(12.5f));
	REQUIRE(m.samples.size() == 1);
	CHECK(m.samples[0].name == "world.mask");
}

TEST_CASE("errors name the problem") {
	ve::StageManifest m;
	std::string err;
	CHECK_FALSE(ve::parse_stage_manifest("//!kind field\nvoid f(){}\n", &m, &err));
	CHECK(err.find("stage") != std::string::npos);

	err.clear();
	CHECK_FALSE(ve::parse_stage_manifest("//!stage s\n//!kind wat\n", &m, &err));
	CHECK(err.find("wat") != std::string::npos);

	err.clear();
	CHECK_FALSE(ve::parse_stage_manifest("//!stage s\n//!kind field\n//!out sdf : mat4\n", &m, &err));
	CHECK(err.find("mat4") != std::string::npos);

	err.clear();
	CHECK_FALSE(ve::parse_stage_manifest("//!stage s\n//!kind field\n//!bogus x\n", &m, &err));
	CHECK(err.find("bogus") != std::string::npos);
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `./build.sh --test`
Expected: FAIL — `terrain/stage_manifest.h` not found.

- [ ] **Step 4: Write the header**

Create `extension/src/terrain/stage_manifest.h` with exactly the declarations in the **Interfaces** block above, plus `#pragma once`, `#include <string>`, `#include <vector>`, and a file comment explaining that a stage's manifest lives in its own GLSL source so the two cannot drift.

- [ ] **Step 5: Write the implementation**

Create `extension/src/terrain/stage_manifest.cpp`. Structure:

```cpp
#include "terrain/stage_manifest.h"
#include <cstdlib>
#include <sstream>

namespace ve {
namespace {

// Trims ASCII space and tab from both ends. The directive grammar is whitespace-insensitive
// so that manifests can be column-aligned for readability.
std::string trim(const std::string &s) {
	const size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string::npos) return "";
	return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

bool parse_channel_type(const std::string &s, ChannelType *out) {
	if (s == "float") { *out = ChannelType::kFloat; return true; }
	if (s == "vec2")  { *out = ChannelType::kVec2;  return true; }
	if (s == "vec3")  { *out = ChannelType::kVec3;  return true; }
	if (s == "vec4")  { *out = ChannelType::kVec4;  return true; }
	if (s == "int")   { *out = ChannelType::kInt;   return true; }
	if (s == "uint")  { *out = ChannelType::kUint;  return true; }
	return false;
}

// Splits "name : type" into its halves. Returns false when the colon is missing.
bool split_typed(const std::string &rest, std::string *name, std::string *type) {
	const size_t colon = rest.find(':');
	if (colon == std::string::npos) return false;
	*name = trim(rest.substr(0, colon));
	*type = trim(rest.substr(colon + 1));
	return !name->empty() && !type->empty();
}

} // namespace

bool parse_stage_manifest(const std::string &source, StageManifest *out, std::string *error) {
	*out = StageManifest{};
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };

	std::istringstream in(source);
	std::string line;
	std::ostringstream body;
	bool kind_seen = false;
	while (std::getline(in, line)) {
		const std::string t = trim(line);
		if (t.rfind("//!", 0) != 0) { body << line << '\n'; continue; }
		const std::string d = trim(t.substr(3));
		const size_t sp = d.find_first_of(" \t");
		const std::string key = sp == std::string::npos ? d : d.substr(0, sp);
		const std::string rest = sp == std::string::npos ? "" : trim(d.substr(sp));

		if (key == "stage") { out->name = rest; }
		else if (key == "kind") {
			if (rest == "field") out->kind = StageKind::kField;
			else if (rest == "map") out->kind = StageKind::kMap;
			else return fail("unknown //!kind: " + rest);
			kind_seen = true;
		}
		else if (key == "in" || key == "out") {
			std::string n, ty;
			if (!split_typed(rest, &n, &ty)) return fail("//!" + key + " needs 'name : type'");
			// A dotted name is a RESOURCE (scope.name), not a FieldCtx channel; map stages
			// declare their resource reads and writes with the same two directives.
			if (n.find('.') != std::string::npos) {
				ResourceDecl r; r.name = n; r.type = ty;
				(key == "in" ? out->samples : out->samples).push_back(r);
			} else {
				ChannelType ct;
				if (!parse_channel_type(ty, &ct)) return fail("unknown channel type: " + ty);
				ChannelDecl c; c.name = n; c.type = ct;
				(key == "in" ? out->reads : out->writes).push_back(c);
			}
		}
		else if (key == "sample") {
			std::string n, ty;
			if (!split_typed(rest, &n, &ty)) return fail("//!sample needs 'name : type'");
			ResourceDecl r; r.name = n; r.type = ty;
			out->samples.push_back(r);
		}
		else if (key == "param") {
			const size_t eq = rest.find('=');
			if (eq == std::string::npos) return fail("//!param needs a default: " + rest);
			std::string n, ty;
			if (!split_typed(trim(rest.substr(0, eq)), &n, &ty))
				return fail("//!param needs 'name : type = default'");
			ChannelType ct;
			if (!parse_channel_type(ty, &ct)) return fail("unknown param type: " + ty);
			ParamDecl p; p.name = n; p.type = ct;
			p.value = float(std::atof(trim(rest.substr(eq + 1)).c_str()));
			out->params.push_back(p);
		}
		else if (key == "lipschitz") { out->lipschitz = float(std::atof(rest.c_str())); }
		else if (key == "bounds")    { out->bounds = float(std::atof(rest.c_str())); }
		else if (key == "iterate")   { out->iterate = std::atoi(rest.c_str()); }
		else if (key == "cpu")       { out->cpu_symbol = rest; }
		else if (key == "domain") {
			const size_t sp2 = rest.find_first_of(" \t");
			out->domain = sp2 == std::string::npos ? rest : rest.substr(0, sp2);
			if (sp2 != std::string::npos) {
				const std::string ext = trim(rest.substr(sp2));
				const size_t x = ext.find('x');
				if (x == std::string::npos) return fail("//!domain extent needs WxH: " + ext);
				out->domain_w = std::atoi(ext.substr(0, x).c_str());
				out->domain_h = std::atoi(ext.substr(x + 1).c_str());
			}
		}
		else return fail("unknown directive: //!" + key);
	}

	if (out->name.empty()) return fail("missing //!stage <name>");
	if (!kind_seen) return fail("missing //!kind field|map");
	if (out->iterate < 1) return fail("//!iterate must be >= 1");
	out->body = body.str();
	return true;
}

int channel_component_count(ChannelType t) {
	switch (t) {
		case ChannelType::kVec2: return 2;
		case ChannelType::kVec3: return 3;
		case ChannelType::kVec4: return 4;
		default: return 1;
	}
}

const char *channel_glsl_type(ChannelType t) {
	switch (t) {
		case ChannelType::kVec2: return "vec2";
		case ChannelType::kVec3: return "vec3";
		case ChannelType::kVec4: return "vec4";
		case ChannelType::kInt:  return "int";
		case ChannelType::kUint: return "uint";
		default: return "float";
	}
}

} // namespace ve
```

Note the `in`/`out` branch above pushes both resource cases into `samples`; for Plan A no map stage is resolved, so that is correct and simple. Plan B splits it into `reads`/`writes` resource lists.

- [ ] **Step 6: Run the tests**

Run: `./build.sh --test`
Expected: PASS, all four `TEST_CASE`s.

- [ ] **Step 7: Commit**

```bash
git add extension/src/terrain extension/tests/test_stage_manifest.cpp extension/SConstruct
git commit -m "feat: parse stage manifests from their GLSL source"
```

## Task 9: Pipeline resource parser

**Files:**
- Create: `extension/src/terrain/pipeline.h`, `extension/src/terrain/pipeline.cpp`
- Create: `extension/tests/test_pipeline_parse.cpp`

**Interfaces:**
- Consumes: nothing from Task 8 yet (parsing only)
- Produces, consumed by Task 10:

```cpp
namespace ve {
struct PipelineStageRef {
    std::string path;                              // e.g. "stages/hills.field.glslh"
    std::vector<std::pair<std::string, float>> param_overrides;
};
struct PipelineDesc {
    uint32_t seed = 1337;
    float lipschitz_override = 0.0f;               // 0 => use the combined bound
    bool allow_gpu_only = false;
    std::vector<PipelineStageRef> stages;
};
bool parse_pipeline_desc(const std::string &source, PipelineDesc *out, std::string *error);
}
```

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_pipeline_parse.cpp`:

```cpp
#include <doctest/doctest.h>
#include "terrain/pipeline.h"

TEST_CASE("parses a pipeline with indented param overrides") {
	const char *src =
		"# a comment\n"
		"seed      1337\n"
		"lipschitz 2.0\n"
		"\n"
		"stage stages/hills.field.glslh\n"
		"  amplitude 6.0\n"
		"  frequency 0.11\n"
		"stage stages/cave.field.glslh\n"
		"  radius 5.0\n";
	ve::PipelineDesc d;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_pipeline_desc(src, &d, &err), err);
	CHECK(d.seed == 1337u);
	CHECK(d.lipschitz_override == doctest::Approx(2.0f));
	REQUIRE(d.stages.size() == 2);
	CHECK(d.stages[0].path == "stages/hills.field.glslh");
	REQUIRE(d.stages[0].param_overrides.size() == 2);
	CHECK(d.stages[0].param_overrides[0].first == "amplitude");
	CHECK(d.stages[0].param_overrides[0].second == doctest::Approx(6.0f));
	CHECK(d.stages[1].path == "stages/cave.field.glslh");
	REQUIRE(d.stages[1].param_overrides.size() == 1);
}

TEST_CASE("allow_gpu_only defaults off and can be set") {
	ve::PipelineDesc d;
	std::string err;
	REQUIRE(ve::parse_pipeline_desc("stage a\n", &d, &err));
	CHECK_FALSE(d.allow_gpu_only);
	REQUIRE(ve::parse_pipeline_desc("allow_gpu_only 1\nstage a\n", &d, &err));
	CHECK(d.allow_gpu_only);
}

TEST_CASE("a param override before any stage is an error") {
	ve::PipelineDesc d;
	std::string err;
	CHECK_FALSE(ve::parse_pipeline_desc("  amplitude 6.0\n", &d, &err));
	CHECK(err.find("amplitude") != std::string::npos);
}

TEST_CASE("an empty pipeline is an error") {
	ve::PipelineDesc d;
	std::string err;
	CHECK_FALSE(ve::parse_pipeline_desc("seed 1\n", &d, &err));
	CHECK(err.find("stage") != std::string::npos);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./build.sh --test`
Expected: FAIL — `terrain/pipeline.h` not found.

- [ ] **Step 3: Write header and implementation**

Create `extension/src/terrain/pipeline.h` with the **Interfaces** declarations (plus `#pragma once`, `<string>`, `<vector>`, `<utility>`, `<cstdint>`). Create `extension/src/terrain/pipeline.cpp`:

```cpp
#include "terrain/pipeline.h"
#include <cstdlib>
#include <sstream>

namespace ve {
namespace {
std::string trim(const std::string &s) {
	const size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string::npos) return "";
	return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}
bool is_indented(const std::string &line) {
	return !line.empty() && (line[0] == ' ' || line[0] == '\t');
}
} // namespace

bool parse_pipeline_desc(const std::string &source, PipelineDesc *out, std::string *error) {
	*out = PipelineDesc{};
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };

	std::istringstream in(source);
	std::string line;
	while (std::getline(in, line)) {
		const bool indented = is_indented(line);
		const std::string t = trim(line);
		if (t.empty() || t[0] == '#') continue;

		const size_t sp = t.find_first_of(" \t");
		const std::string key = sp == std::string::npos ? t : t.substr(0, sp);
		const std::string rest = sp == std::string::npos ? "" : trim(t.substr(sp));

		// Indentation is the grammar: an indented line is a param override on the stage
		// above it. This keeps a pipeline readable as a list without needing a nested
		// block syntax or a real file format.
		if (indented) {
			if (out->stages.empty()) return fail("param override before any stage: " + key);
			out->stages.back().param_overrides.emplace_back(key, float(std::atof(rest.c_str())));
			continue;
		}
		if (key == "seed") out->seed = uint32_t(std::strtoul(rest.c_str(), nullptr, 10));
		else if (key == "lipschitz") out->lipschitz_override = float(std::atof(rest.c_str()));
		else if (key == "allow_gpu_only") out->allow_gpu_only = std::atoi(rest.c_str()) != 0;
		else if (key == "stage") {
			if (rest.empty()) return fail("stage needs a path");
			PipelineStageRef r; r.path = rest;
			out->stages.push_back(r);
		}
		else return fail("unknown pipeline key: " + key);
	}
	if (out->stages.empty()) return fail("pipeline declares no stage");
	return true;
}

} // namespace ve
```

- [ ] **Step 4: Run the tests**

Run: `./build.sh --test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/src/terrain/pipeline.h extension/src/terrain/pipeline.cpp extension/tests/test_pipeline_parse.cpp
git commit -m "feat: parse the .pipeline resource format"
```

## Task 10: Pipeline resolution and validation

**Files:**
- Modify: `extension/src/terrain/pipeline.h`, `extension/src/terrain/pipeline.cpp`
- Create: `extension/tests/test_pipeline_resolve.cpp`

**Interfaces:**
- Consumes: `ve::StageManifest` (Task 8), `ve::PipelineDesc` (Task 9)
- Produces, consumed by Tasks 11–14:

```cpp
namespace ve {
struct ResolvedChannel { std::string name; ChannelType type; int slot; };  // slot: index into FieldCtx::ch
struct ResolvedPipeline {
    std::vector<StageManifest> stages;      // pipeline order
    std::vector<ResolvedChannel> channels;  // p, sdf, material first, then declared order
    std::vector<ResourceDecl> resources;    // sorted by name; set-1 binding = 2 + index
    std::vector<ParamDecl> params;          // flattened "<stage>.<param>", resolved values
    float lipschitz = 2.0f;
    bool cpu_exact = true;
    uint64_t hash = 0;                      // FNV-1a over every stage body + resolved params
    int channel_slot(const std::string &name) const;  // -1 when absent
};
bool resolve_pipeline(const PipelineDesc &desc, const std::vector<StageManifest> &loaded,
                      ResolvedPipeline *out, std::string *error);
}
```

`loaded` is parallel to `desc.stages` — element *i* is the manifest parsed from `desc.stages[i].path`. Loading files is the caller's job, which keeps this function pure and native-testable.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_pipeline_resolve.cpp`:

```cpp
#include <doctest/doctest.h>
#include "terrain/pipeline.h"
#include "terrain/stage_manifest.h"

namespace {

ve::StageManifest field_stage(const char *name, std::vector<const char *> writes,
		std::vector<const char *> reads, const char *cpu = "ve::x") {
	ve::StageManifest m;
	m.name = name;
	m.kind = ve::StageKind::kField;
	m.cpu_symbol = cpu;
	m.lipschitz = 1.0f;
	for (const char *w : writes) m.writes.push_back({w, ve::ChannelType::kFloat});
	for (const char *r : reads)  m.reads.push_back({r, ve::ChannelType::kFloat});
	m.body = "void s(inout FieldCtx c){}\n";
	return m;
}

ve::PipelineDesc desc_for(size_t n, bool allow_gpu_only = false) {
	ve::PipelineDesc d;
	d.allow_gpu_only = allow_gpu_only;
	for (size_t i = 0; i < n; i++) d.stages.push_back({"s", {}});
	return d;
}

} // namespace

TEST_CASE("built-in channels exist and declared channels get stable slots") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf", "temperature"}, {}),
		field_stage("b", {"material"}, {"temperature"}),
	};
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
	CHECK(p.channel_slot("p") == 0);
	CHECK(p.channel_slot("sdf") == 1);
	CHECK(p.channel_slot("material") == 2);
	CHECK(p.channel_slot("temperature") == 3);
	CHECK(p.channel_slot("nope") == -1);
	CHECK(p.cpu_exact);
}

TEST_CASE("a pipeline that never writes sdf is rejected") {
	std::vector<ve::StageManifest> st{field_stage("a", {"temperature"}, {})};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("sdf") != std::string::npos);
}

TEST_CASE("reading a channel no earlier stage wrote is rejected, naming both") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf"}, {}),
		field_stage("b", {"sdf"}, {"moisture"}),
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("moisture") != std::string::npos);
	CHECK(err.find("b") != std::string::npos);
}

TEST_CASE("two stages writing one channel is legal -- ordered override is the model") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf"}, {}),
		field_stage("b", {"sdf"}, {"sdf"}),
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(2), st, &p, &err), err);
}

TEST_CASE("a channel type conflict is rejected") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf"}, {}),
		field_stage("b", {"sdf"}, {}),
	};
	st[1].writes[0].type = ve::ChannelType::kVec3;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("sdf") != std::string::npos);
}

TEST_CASE("duplicate stage names are rejected") {
	std::vector<ve::StageManifest> st{
		field_stage("a", {"sdf"}, {}), field_stage("a", {"sdf"}, {}),
	};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(2), st, &p, &err));
	CHECK(err.find("a") != std::string::npos);
}

TEST_CASE("a GPU-only stage needs the opt-in") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {}, "")};
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("a") != std::string::npos);
	CHECK_MESSAGE(ve::resolve_pipeline(desc_for(1, true), st, &p, &err), err);
	CHECK_FALSE(p.cpu_exact);
}

TEST_CASE("a map stage in a field pipeline is rejected in Plan A") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].kind = ve::StageKind::kMap;
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(desc_for(1), st, &p, &err));
	CHECK(err.find("map") != std::string::npos);
}

TEST_CASE("resources sort by name and lipschitz combines multiplicatively") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].samples.push_back({"sector.z", "texture2d_r32f", 0.0f});
	st[0].samples.push_back({"sector.a", "texture2d_r32f", 0.0f});
	st[0].lipschitz = 1.5f;
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(desc_for(1), st, &p, &err), err);
	REQUIRE(p.resources.size() == 2);
	CHECK(p.resources[0].name == "sector.a");
	CHECK(p.resources[1].name == "sector.z");
	CHECK(p.lipschitz == doctest::Approx(1.5f));
}

TEST_CASE("param overrides win, and the hash moves when they do") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	st[0].params.push_back({"amplitude", ve::ChannelType::kFloat, 6.0f});
	ve::ResolvedPipeline p1, p2;
	std::string err;
	REQUIRE(ve::resolve_pipeline(desc_for(1), st, &p1, &err));
	CHECK(p1.params[0].value == doctest::Approx(6.0f));
	ve::PipelineDesc d = desc_for(1);
	d.stages[0].param_overrides.emplace_back("amplitude", 9.0f);
	REQUIRE(ve::resolve_pipeline(d, st, &p2, &err));
	CHECK(p2.params[0].value == doctest::Approx(9.0f));
	CHECK(p1.hash != p2.hash);
}

TEST_CASE("an override naming an unknown param is rejected") {
	std::vector<ve::StageManifest> st{field_stage("a", {"sdf"}, {})};
	ve::PipelineDesc d = desc_for(1);
	d.stages[0].param_overrides.emplace_back("nope", 1.0f);
	ve::ResolvedPipeline p;
	std::string err;
	CHECK_FALSE(ve::resolve_pipeline(d, st, &p, &err));
	CHECK(err.find("nope") != std::string::npos);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./build.sh --test`
Expected: FAIL — `resolve_pipeline` not declared.

- [ ] **Step 3: Implement resolution**

Append to `extension/src/terrain/pipeline.h` the **Interfaces** declarations, then append to `extension/src/terrain/pipeline.cpp`:

```cpp
int ResolvedPipeline::channel_slot(const std::string &name) const {
	for (size_t i = 0; i < channels.size(); i++)
		if (channels[i].name == name) return int(i);
	return -1;
}

namespace {
void hash_feed(uint64_t &h, const std::string &s) {
	for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
}
} // namespace

bool resolve_pipeline(const PipelineDesc &desc, const std::vector<StageManifest> &loaded,
		ResolvedPipeline *out, std::string *error) {
	*out = ResolvedPipeline{};
	auto fail = [&](const std::string &m) { if (error) *error = m; return false; };
	if (loaded.size() != desc.stages.size())
		return fail("manifest count does not match pipeline stage count");

	// Built-ins occupy slots 0..2 so that every pipeline agrees on them, which is what lets
	// the CPU mirror and the generated GLSL index the context identically.
	out->channels.push_back({"p", ChannelType::kVec3, 0});
	out->channels.push_back({"sdf", ChannelType::kFloat, 1});
	out->channels.push_back({"material", ChannelType::kUint, 2});

	bool wrote_sdf = false;
	float lip = 1.0f;
	uint64_t h = 1469598103934665603ull;

	for (size_t i = 0; i < loaded.size(); i++) {
		StageManifest m = loaded[i];
		if (m.kind != StageKind::kField)
			return fail("stage '" + m.name + "' is a map stage; Plan A resolves field stages only");
		for (size_t j = 0; j < i; j++)
			if (loaded[j].name == m.name) return fail("duplicate stage name: " + m.name);
		if (m.cpu_symbol.empty()) {
			if (!desc.allow_gpu_only)
				return fail("stage '" + m.name + "' has no //!cpu mirror; set allow_gpu_only "
						"to accept a GPU-authoritative field");
			out->cpu_exact = false;
		}

		for (const ChannelDecl &r : m.reads) {
			const int slot = out->channel_slot(r.name);
			if (slot < 0)
				return fail("stage '" + m.name + "' reads channel '" + r.name +
						"' that no earlier stage writes");
			if (out->channels[size_t(slot)].type != r.type)
				return fail("stage '" + m.name + "' reads channel '" + r.name +
						"' at a conflicting type");
		}
		for (const ChannelDecl &w : m.writes) {
			const int slot = out->channel_slot(w.name);
			if (slot < 0) {
				out->channels.push_back({w.name, w.type, int(out->channels.size())});
			} else if (out->channels[size_t(slot)].type != w.type) {
				return fail("stage '" + m.name + "' writes channel '" + w.name +
						"' at a conflicting type");
			}
			if (w.name == "sdf") wrote_sdf = true;
		}

		for (const ResourceDecl &r : m.samples) {
			bool seen = false;
			for (const ResourceDecl &e : out->resources) {
				if (e.name != r.name) continue;
				if (e.type != r.type)
					return fail("resource '" + r.name + "' declared at two types");
				seen = true;
				break;
			}
			if (!seen) out->resources.push_back(r);
		}

		for (const auto &ov : desc.stages[i].param_overrides) {
			bool found = false;
			for (ParamDecl &p : m.params)
				if (p.name == ov.first) { p.value = ov.second; found = true; break; }
			if (!found)
				return fail("stage '" + m.name + "' has no param '" + ov.first + "'");
		}
		for (const ParamDecl &p : m.params) {
			ParamDecl flat = p;
			flat.name = m.name + "." + p.name;
			out->params.push_back(flat);
		}

		lip *= (m.lipschitz > 0.0f ? m.lipschitz : 1.0f);
		hash_feed(h, m.name);
		hash_feed(h, m.body);
		for (const ParamDecl &p : m.params) {
			hash_feed(h, p.name);
			hash_feed(h, std::to_string(p.value));
		}
		out->stages.push_back(m);
	}

	if (!wrote_sdf)
		return fail("pipeline never writes channel 'sdf'; the final field stage must produce one");

	// Sorted so set-1 binding indices are a pure function of the resource names, which keeps
	// the generated GLSL stable and diffable across unrelated pipeline edits.
	for (size_t a = 0; a + 1 < out->resources.size(); a++)
		for (size_t b = a + 1; b < out->resources.size(); b++)
			if (out->resources[b].name < out->resources[a].name)
				std::swap(out->resources[a], out->resources[b]);

	out->lipschitz = desc.lipschitz_override > 0.0f ? desc.lipschitz_override : lip;
	hash_feed(h, std::to_string(desc.seed));
	out->hash = h;
	return true;
}
```

Add `#include <algorithm>` and `#include <string>` to the top of the file if not already present.

- [ ] **Step 4: Run the tests**

Run: `./build.sh --test`
Expected: PASS, all eleven `TEST_CASE`s.

- [ ] **Step 5: Commit**

```bash
git add extension/src/terrain/pipeline.h extension/src/terrain/pipeline.cpp extension/tests/test_pipeline_resolve.cpp
git commit -m "feat: resolve and validate a stage pipeline"
```

---

# Phase 3 — Codegen

## Task 11: Generate field.glslh

**Files:**
- Create: `extension/src/terrain/field_codegen.h`, `extension/src/terrain/field_codegen.cpp`
- Create: `extension/tests/test_field_codegen.cpp`

**Interfaces:**
- Consumes: `ve::ResolvedPipeline` (Task 10)
- Produces, consumed by Tasks 12 and 17:

```cpp
namespace ve {
// Emits the full replacement text for shaders/field.glslh. `prelude` is the text of the
// current field.glslh from "const uint OP_SPHERE_SUBTRACT" through the end of eval_field(),
// with base_field() removed -- the caller reads it from disk so this stays pure.
std::string generate_field_glslh(const ResolvedPipeline &p, const std::string &prelude);
}
```

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_field_codegen.cpp`:

```cpp
#include <doctest/doctest.h>
#include "terrain/field_codegen.h"
#include "terrain/pipeline.h"
#include "terrain/stage_manifest.h"

namespace {

ve::ResolvedPipeline two_stage() {
	ve::StageManifest a;
	a.name = "hills"; a.kind = ve::StageKind::kField; a.cpu_symbol = "ve::stage_hills";
	a.writes.push_back({"sdf", ve::ChannelType::kFloat});
	a.writes.push_back({"steepness", ve::ChannelType::kFloat});
	a.params.push_back({"amplitude", ve::ChannelType::kFloat, 6.0f});
	a.body = "void stage_hills(inout FieldCtx ctx) { ctx.sdf = ctx.p.y; }\n";

	ve::StageManifest b;
	b.name = "bands"; b.kind = ve::StageKind::kField; b.cpu_symbol = "ve::stage_bands";
	b.reads.push_back({"steepness", ve::ChannelType::kFloat});
	b.writes.push_back({"material", ve::ChannelType::kUint});
	b.body = "void stage_bands(inout FieldCtx ctx) { ctx.material = 1u; }\n";

	ve::PipelineDesc d;
	d.stages.push_back({"a", {}});
	d.stages.push_back({"b", {}});
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE(ve::resolve_pipeline(d, {a, b}, &p, &err));
	return p;
}

} // namespace

TEST_CASE("generated source declares the context, the params and the composition") {
	const std::string g = ve::generate_field_glslh(two_stage(), "// PRELUDE MARKER\n");

	CHECK(g.find("struct FieldCtx") != std::string::npos);
	CHECK(g.find("vec3 p;") != std::string::npos);
	CHECK(g.find("float sdf;") != std::string::npos);
	CHECK(g.find("uint material;") != std::string::npos);
	CHECK(g.find("float steepness;") != std::string::npos);

	// Set 1: params UBO at binding 0, sector map at binding 1.
	CHECK(g.find("set = 1, binding = 0") != std::string::npos);
	CHECK(g.find("set = 1, binding = 1") != std::string::npos);
	CHECK(g.find("hills_amplitude") != std::string::npos);

	// Bodies verbatim, in pipeline order.
	const size_t hills = g.find("void stage_hills(inout FieldCtx ctx)");
	const size_t bands = g.find("void stage_bands(inout FieldCtx ctx)");
	CHECK(hills != std::string::npos);
	CHECK(bands != std::string::npos);
	CHECK(hills < bands);

	// Composition calls them in order and hands back sdf + material.
	const size_t base = g.find("void eval_base_field(");
	CHECK(base != std::string::npos);
	CHECK(g.find("stage_hills(ctx);", base) != std::string::npos);
	CHECK(g.find("stage_bands(ctx);", base) != std::string::npos);
	CHECK(g.find("stage_hills(ctx);", base) < g.find("stage_bands(ctx);", base));

	CHECK(g.find("// PRELUDE MARKER") != std::string::npos);
	CHECK(g.find("void base_field(") == std::string::npos);  // replaced, not kept
}

TEST_CASE("generation is deterministic") {
	const ve::ResolvedPipeline p = two_stage();
	CHECK(ve::generate_field_glslh(p, "x\n") == ve::generate_field_glslh(p, "x\n"));
}

TEST_CASE("resources land in set 1 from binding 2, in sorted order") {
	ve::ResolvedPipeline p = two_stage();
	p.resources.push_back({"sector.alpha", "texture2d_r32f", 0.0f});
	p.resources.push_back({"sector.beta", "texture2d_r32f", 1.5f});
	const std::string g = ve::generate_field_glslh(p, "");
	const size_t a = g.find("sector_alpha");
	const size_t b = g.find("sector_beta");
	CHECK(a != std::string::npos);
	CHECK(b != std::string::npos);
	CHECK(a < b);
	CHECK(g.find("set = 1, binding = 2") != std::string::npos);
	CHECK(g.find("set = 1, binding = 3") != std::string::npos);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./build.sh --test`
Expected: FAIL — `terrain/field_codegen.h` not found.

- [ ] **Step 3: Implement the generator**

Create `extension/src/terrain/field_codegen.h` with the **Interfaces** declaration. Create `extension/src/terrain/field_codegen.cpp`:

```cpp
#include "terrain/field_codegen.h"
#include <sstream>

namespace ve {
namespace {

// GLSL identifiers cannot contain '.', so "sector.height" becomes "sector_height" both in
// the declaration and at every use site inside a stage body.
std::string ident(const std::string &name) {
	std::string s = name;
	for (char &c : s) if (c == '.') c = '_';
	return s;
}

// Resource type name -> GLSL sampler declaration. Sector resources are ARRAYS: the layer is
// the sector slot, reached through the sector_map indirection, because eval_field() is
// called at arbitrary points that may lie outside the brick being generated.
const char *sampler_type(const std::string &t) {
	if (t == "texture2d_r32f" || t == "texture2d_rg16f" || t == "texture2d_rgba16f")
		return "sampler2DArray";
	if (t == "texture3d_r8" || t == "texture3d_r32f") return "sampler3D";
	return "sampler2DArray";
}

} // namespace

std::string generate_field_glslh(const ResolvedPipeline &p, const std::string &prelude) {
	std::ostringstream o;
	o << "// GENERATED by ve::generate_field_glslh -- do not edit.\n"
	  << "// Pipeline hash: " << p.hash << "\n"
	  << "// Lipschitz bound: " << p.lipschitz << "\n"
	  << "// CPU-exact: " << (p.cpu_exact ? "yes" : "no") << "\n\n";

	o << "struct FieldCtx {\n";
	for (const ResolvedChannel &c : p.channels)
		o << "\t" << channel_glsl_type(c.type) << " " << c.name << ";\n";
	o << "};\n\n";

	o << "layout(set = 1, binding = 0, std140) uniform FieldParams {\n";
	if (p.params.empty()) {
		o << "\tvec4 _unused;\n";
	} else {
		for (const ParamDecl &pm : p.params)
			o << "\t" << channel_glsl_type(pm.type) << " " << ident(pm.name) << ";\n";
	}
	o << "} P;\n\n";

	// Bound even when empty so every field-consuming pass binds set 1 unconditionally and
	// the binding code has no special case. Plan B fills it.
	o << "layout(set = 1, binding = 1, std430) readonly buffer SectorMap {\n"
	  << "\tint slot[];\n"
	  << "} sector_map;\n\n";

	for (size_t i = 0; i < p.resources.size(); i++) {
		o << "layout(set = 1, binding = " << (2 + i) << ") uniform "
		  << sampler_type(p.resources[i].type) << " " << ident(p.resources[i].name) << ";\n"
		  << "const float " << ident(p.resources[i].name) << "_FALLBACK = "
		  << p.resources[i].fallback << ";\n";
	}
	if (!p.resources.empty()) o << "\n";

	for (const StageManifest &s : p.stages) o << s.body << "\n";

	o << "void eval_base_field(vec3 p, out float sdf, out uint mat) {\n"
	  << "\tFieldCtx ctx;\n";
	for (const ResolvedChannel &c : p.channels) {
		if (c.name == "p") { o << "\tctx.p = p;\n"; continue; }
		o << "\tctx." << c.name << " = " << channel_glsl_type(c.type) << "(0);\n";
	}
	for (const StageManifest &s : p.stages) o << "\tstage_" << s.name << "(ctx);\n";
	o << "\tsdf = ctx.sdf;\n\tmat = ctx.material;\n}\n\n";

	o << prelude;
	return o.str();
}

} // namespace ve
```

The generated stage call is `stage_<name>(ctx)`, so a stage's GLSL function **must** be named `stage_<its //!stage name>`. Note that in the stage source's own comment header when you write Task 15's stages.

- [ ] **Step 4: Run the tests**

Run: `./build.sh --test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/src/terrain/field_codegen.h extension/src/terrain/field_codegen.cpp extension/tests/test_field_codegen.cpp
git commit -m "feat: generate field.glslh from a resolved pipeline"
```

## Task 12: Golden generated-source test

**Files:**
- Create: `extension/tests/test_field_codegen_golden.cpp`
- Create: `shaders/generated/field.glslh.golden`

**Interfaces:**
- Consumes: `ve::generate_field_glslh` (Task 11), the stage files created in Task 15
- Produces: nothing

This task is written now but **cannot pass until Task 15 creates the stage files**. Write it, confirm it fails for the right reason, and mark it complete only after Task 15.

- [ ] **Step 1: Write the test**

Create `extension/tests/test_field_codegen_golden.cpp`:

```cpp
// The material_table.glslh pattern: a committed generated artifact plus a test asserting the
// generator still produces it byte for byte. Catches codegen drift that no behavioural test
// would notice until a shader failed to compile.
//
// Regenerate after an INTENTIONAL codegen change: VE_REGEN_GOLDEN=1 ./build.sh --test
#include <doctest/doctest.h>
#include "terrain/field_codegen.h"
#include "terrain/pipeline.h"
#include "terrain/stage_manifest.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string root() { return std::string(VE_REPO_ROOT); }

std::string slurp(const std::string &path) {
	std::ifstream f(path);
	REQUIRE_MESSAGE(f.good(), "cannot open " + path);
	std::ostringstream o;
	o << f.rdbuf();
	return o.str();
}

} // namespace

TEST_CASE("the default pipeline generates the committed source") {
	ve::PipelineDesc d;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_pipeline_desc(
			slurp(root() + "/assets/pipelines/default.pipeline"), &d, &err), err);

	std::vector<ve::StageManifest> loaded;
	for (const ve::PipelineStageRef &r : d.stages) {
		ve::StageManifest m;
		REQUIRE_MESSAGE(ve::parse_stage_manifest(
				slurp(root() + "/shaders/" + r.path), &m, &err), err);
		loaded.push_back(m);
	}

	ve::ResolvedPipeline p;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, loaded, &p, &err), err);

	const std::string prelude = slurp(root() + "/shaders/field_ops.glslh");
	const std::string got = ve::generate_field_glslh(p, prelude);
	const std::string golden_path = root() + "/shaders/generated/field.glslh.golden";

	if (std::getenv("VE_REGEN_GOLDEN") != nullptr) {
		std::ofstream out(golden_path);
		REQUIRE(out.good());
		out << got;
		out.close();
		MESSAGE("regenerated " << golden_path);
	}
	CHECK(got == slurp(golden_path));
}
```

- [ ] **Step 2: Run it and confirm it fails for the right reason**

Run: `./build.sh --test`
Expected: FAIL with `cannot open .../assets/pipelines/default.pipeline` — the files arrive in Task 15. Do **not** commit yet; carry this task to the end of Task 15.

- [ ] **Step 3: After Task 15, generate and verify**

```bash
mkdir -p shaders/generated
VE_REGEN_GOLDEN=1 ./build.sh --test
./build.sh --test
```

Expected: PASS.

- [ ] **Step 4: Commit (after Task 15)**

```bash
git add extension/tests/test_field_codegen_golden.cpp shaders/generated/field.glslh.golden
git commit -m "test: pin the generated field source byte for byte"
```

---

# Phase 4 — CPU mirror

## Task 13: Stage library and FieldCtx runtime

**Files:**
- Create: `extension/src/terrain/stage_library.h`, `extension/src/terrain/stage_library.cpp`
- Create: `extension/tests/test_stage_library.cpp`

**Interfaces:**
- Produces, consumed by Tasks 14–15:

```cpp
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
struct StageParams {
    const float *values = nullptr;
    int count = 0;
    float at(int i) const { return (values != nullptr && i >= 0 && i < count) ? values[i] : 0.0f; }
};
struct FieldResources {};  // Plan A: no CPU-side resource sampling yet
struct StageSlots {
    int p = 0, sdf = 1, material = 2;
    int extra[FieldCtx::kMaxChannels] = {};  // per-stage resolved slot indices
};
using StageFn = void (*)(FieldCtx &, const StageSlots &, const StageParams &, const FieldResources &);

class StageLibrary {
public:
    static StageLibrary &instance();
    void register_stage(const std::string &symbol, StageFn fn);
    StageFn lookup(const std::string &symbol) const;  // nullptr when absent
private:
    std::vector<std::pair<std::string, StageFn>> entries_;
};

struct StageRegistrar { StageRegistrar(const char *symbol, StageFn fn); };
}
#define VE_REGISTER_STAGE(symbol, fn) \
    static ::ve::StageRegistrar ve_stage_reg_##fn(symbol, &fn)
```

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_stage_library.cpp`:

```cpp
#include <doctest/doctest.h>
#include "terrain/stage_library.h"

namespace {
void test_writes_sdf(ve::FieldCtx &ctx, const ve::StageSlots &s, const ve::StageParams &p,
		const ve::FieldResources &) {
	ctx.f(s.sdf) = ctx.v(s.p)[1] - p.at(0);
}
} // namespace

VE_REGISTER_STAGE("ve::test_writes_sdf", test_writes_sdf);

TEST_CASE("a registered stage is found by symbol and runs") {
	ve::StageFn fn = ve::StageLibrary::instance().lookup("ve::test_writes_sdf");
	REQUIRE(fn != nullptr);

	ve::FieldCtx ctx;
	ve::StageSlots slots;
	ctx.v(slots.p)[1] = 10.0f;
	const float params[1] = {4.0f};
	ve::StageParams sp{params, 1};
	fn(ctx, slots, sp, ve::FieldResources{});
	CHECK(ctx.f(slots.sdf) == doctest::Approx(6.0f));
}

TEST_CASE("an unknown symbol resolves to null rather than crashing") {
	CHECK(ve::StageLibrary::instance().lookup("ve::nope") == nullptr);
}

TEST_CASE("StageParams::at is bounds-safe") {
	ve::StageParams empty;
	CHECK(empty.at(0) == doctest::Approx(0.0f));
	const float v[1] = {3.0f};
	ve::StageParams one{v, 1};
	CHECK(one.at(0) == doctest::Approx(3.0f));
	CHECK(one.at(1) == doctest::Approx(0.0f));
	CHECK(one.at(-1) == doctest::Approx(0.0f));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./build.sh --test`
Expected: FAIL — `terrain/stage_library.h` not found.

- [ ] **Step 3: Implement**

Create `extension/src/terrain/stage_library.h` with exactly the **Interfaces** declarations (plus `#pragma once`, `<string>`, `<vector>`, `<utility>`). Create `extension/src/terrain/stage_library.cpp`:

```cpp
#include "terrain/stage_library.h"

namespace ve {

StageLibrary &StageLibrary::instance() {
	// Function-local static: registration happens from other translation units' static
	// initializers, and this is the standard way to dodge the static init order fiasco.
	static StageLibrary lib;
	return lib;
}

void StageLibrary::register_stage(const std::string &symbol, StageFn fn) {
	for (auto &e : entries_)
		if (e.first == symbol) { e.second = fn; return; }
	entries_.emplace_back(symbol, fn);
}

StageFn StageLibrary::lookup(const std::string &symbol) const {
	for (const auto &e : entries_)
		if (e.first == symbol) return e.second;
	return nullptr;
}

StageRegistrar::StageRegistrar(const char *symbol, StageFn fn) {
	StageLibrary::instance().register_stage(symbol, fn);
}

} // namespace ve
```

- [ ] **Step 4: Run the tests**

Run: `./build.sh --test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/src/terrain/stage_library.h extension/src/terrain/stage_library.cpp extension/tests/test_stage_library.cpp
git commit -m "feat: stage library and CPU FieldCtx runtime"
```

## Task 14: PipelineFieldGenerator

**Files:**
- Create: `extension/src/terrain/pipeline_field_generator.h`, `extension/src/terrain/pipeline_field_generator.cpp`
- Create: `extension/tests/test_pipeline_field_generator.cpp`

**Interfaces:**
- Consumes: `ve::ResolvedPipeline` (Task 10), `ve::StageLibrary` (Task 13), `ve::FieldGenerator` / `ve::Generator` (`extension/src/generator/field_generator.h`)
- Produces, consumed by Task 15 and by `VoxelWorld`:

```cpp
namespace ve {
class PipelineFieldGenerator : public FieldGenerator {
public:
    // Returns nullptr and sets *error when a stage's //!cpu symbol is not registered.
    static PipelineFieldGenerator *create(const ResolvedPipeline &p, std::string *error);
    Sample eval(float x, float y, float z) const override;
    const Generator &sampler() const override;
    bool is_cpu_exact() const;
    float lipschitz() const;
    const ResolvedPipeline &pipeline() const;
};
}
```

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_pipeline_field_generator.cpp`:

```cpp
#include <doctest/doctest.h>
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_library.h"
#include "terrain/pipeline.h"

namespace {

// A two-stage pipeline whose result is checkable by hand: sdf = y - amplitude, then a
// second stage that sets material from the sign of sdf.
void tp_plane(ve::FieldCtx &ctx, const ve::StageSlots &s, const ve::StageParams &p,
		const ve::FieldResources &) {
	ctx.f(s.sdf) = ctx.v(s.p)[1] - p.at(0);
}
void tp_mat(ve::FieldCtx &ctx, const ve::StageSlots &s, const ve::StageParams &,
		const ve::FieldResources &) {
	ctx.f(s.material) = ctx.f(s.sdf) <= 0.0f ? 3.0f : 0.0f;
}

ve::ResolvedPipeline build(bool with_cpu = true) {
	ve::StageManifest a;
	a.name = "plane"; a.kind = ve::StageKind::kField;
	a.cpu_symbol = with_cpu ? "ve::tp_plane" : "";
	a.writes.push_back({"sdf", ve::ChannelType::kFloat});
	a.params.push_back({"height", ve::ChannelType::kFloat, 51.2f});
	a.lipschitz = 1.0f;
	a.body = "void stage_plane(inout FieldCtx c){}\n";

	ve::StageManifest b;
	b.name = "mat"; b.kind = ve::StageKind::kField; b.cpu_symbol = "ve::tp_mat";
	b.reads.push_back({"sdf", ve::ChannelType::kFloat});
	b.writes.push_back({"material", ve::ChannelType::kUint});
	b.lipschitz = 1.0f;
	b.body = "void stage_mat(inout FieldCtx c){}\n";

	ve::PipelineDesc d;
	d.allow_gpu_only = !with_cpu;
	d.stages.push_back({"a", {}});
	d.stages.push_back({"b", {}});
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, {a, b}, &p, &err), err);
	return p;
}

} // namespace

VE_REGISTER_STAGE("ve::tp_plane", tp_plane);
VE_REGISTER_STAGE("ve::tp_mat", tp_mat);

TEST_CASE("stages run in order and the last sdf write wins") {
	std::string err;
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(build(), &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	CHECK(g->eval(0.0f, 61.2f, 0.0f).sdf == doctest::Approx(10.0f));
	CHECK(g->eval(0.0f, 41.2f, 0.0f).sdf == doctest::Approx(-10.0f));
	CHECK(g->eval(0.0f, 41.2f, 0.0f).material == 3);
	CHECK(g->eval(0.0f, 61.2f, 0.0f).material == 0);
	CHECK(g->is_cpu_exact());
	delete g;
}

TEST_CASE("sampler() hands out a Generator view with the pipeline's lipschitz bound") {
	std::string err;
	ve::ResolvedPipeline p = build();
	p.lipschitz = 3.5f;
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	const ve::Generator &view = g->sampler();
	CHECK(view.sample(0.0f, 61.2f, 0.0f).sdf == doctest::Approx(10.0f));
	CHECK(view.lipschitz() == doctest::Approx(3.5f));
	delete g;
}

TEST_CASE("an unregistered cpu symbol fails create with a named error") {
	ve::ResolvedPipeline p = build();
	p.stages[0].cpu_symbol = "ve::not_registered";
	std::string err;
	CHECK(ve::PipelineFieldGenerator::create(p, &err) == nullptr);
	CHECK(err.find("ve::not_registered") != std::string::npos);
}

TEST_CASE("a GPU-only pipeline creates but reports itself inexact") {
	std::string err;
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(build(false), &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	CHECK_FALSE(g->is_cpu_exact());
	delete g;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./build.sh --test`
Expected: FAIL — `terrain/pipeline_field_generator.h` not found.

- [ ] **Step 3: Implement**

Create `extension/src/terrain/pipeline_field_generator.h`:

```cpp
#pragma once
// The CPU half of the terrain pipeline. Walks the SAME ordered stage list the generated
// GLSL composes, over a FieldCtx whose channel slots the SAME compiler assigned -- so CPU
// and GPU agree by construction rather than by inspection.
#include "generator/field_generator.h"
#include "terrain/pipeline.h"
#include "terrain/stage_library.h"
#include <string>
#include <vector>

namespace ve {

class PipelineFieldGenerator : public FieldGenerator {
public:
	static PipelineFieldGenerator *create(const ResolvedPipeline &p, std::string *error);

	Sample eval(float x, float y, float z) const override { return view_.sample(x, y, z); }
	const Generator &sampler() const override { return view_; }
	bool is_cpu_exact() const { return pipeline_.cpu_exact; }
	float lipschitz() const { return pipeline_.lipschitz; }
	const ResolvedPipeline &pipeline() const { return pipeline_; }

private:
	// brick_eval, raycast and extract_island_volume all consume a `const ve::Generator &`;
	// this inner view is what keeps those call sites' signatures unchanged.
	class View : public Generator {
	public:
		explicit View(const PipelineFieldGenerator *owner) : owner_(owner) {}
		Sample sample(float x, float y, float z) const override;
		float lipschitz() const override { return owner_->pipeline_.lipschitz; }
	private:
		const PipelineFieldGenerator *owner_;
	};

	ResolvedPipeline pipeline_;
	std::vector<StageFn> fns_;              // parallel to pipeline_.stages
	std::vector<StageSlots> slots_;         // parallel to pipeline_.stages
	std::vector<float> param_values_;       // flattened, in pipeline_.params order
	std::vector<int> param_base_;           // parallel to stages: first param index
	std::vector<int> param_count_;
	View view_{this};

	friend class View;
};

} // namespace ve
```

Create `extension/src/terrain/pipeline_field_generator.cpp`:

```cpp
#include "terrain/pipeline_field_generator.h"

namespace ve {

PipelineFieldGenerator *PipelineFieldGenerator::create(const ResolvedPipeline &p,
		std::string *error) {
	if (int(p.channels.size()) > FieldCtx::kMaxChannels) {
		if (error) *error = "pipeline declares more channels than FieldCtx::kMaxChannels";
		return nullptr;
	}
	PipelineFieldGenerator *g = new PipelineFieldGenerator();
	g->pipeline_ = p;

	int cursor = 0;
	for (const StageManifest &s : p.stages) {
		StageFn fn = nullptr;
		if (!s.cpu_symbol.empty()) {
			fn = StageLibrary::instance().lookup(s.cpu_symbol);
			if (fn == nullptr) {
				if (error) *error = "stage '" + s.name + "' names an unregistered cpu symbol: " +
						s.cpu_symbol;
				delete g;
				return nullptr;
			}
		}
		g->fns_.push_back(fn);

		StageSlots slots;
		slots.p = p.channel_slot("p");
		slots.sdf = p.channel_slot("sdf");
		slots.material = p.channel_slot("material");
		// extra[i] is the slot of this stage's i-th declared write, then its reads, in
		// declaration order -- the same order the generated GLSL names them.
		int n = 0;
		for (const ChannelDecl &w : s.writes)
			if (n < FieldCtx::kMaxChannels) slots.extra[n++] = p.channel_slot(w.name);
		for (const ChannelDecl &r : s.reads)
			if (n < FieldCtx::kMaxChannels) slots.extra[n++] = p.channel_slot(r.name);
		g->slots_.push_back(slots);

		g->param_base_.push_back(cursor);
		g->param_count_.push_back(int(s.params.size()));
		cursor += int(s.params.size());
	}
	for (const ParamDecl &pm : p.params) g->param_values_.push_back(pm.value);
	return g;
}

Sample PipelineFieldGenerator::View::sample(float x, float y, float z) const {
	FieldCtx ctx;
	const ResolvedPipeline &p = owner_->pipeline_;
	const int pslot = p.channel_slot("p");
	ctx.v(pslot)[0] = x;
	ctx.v(pslot)[1] = y;
	ctx.v(pslot)[2] = z;

	for (size_t i = 0; i < owner_->fns_.size(); i++) {
		StageFn fn = owner_->fns_[i];
		if (fn == nullptr) continue;  // GPU-only stage: the CPU field is already inexact
		StageParams sp;
		sp.values = owner_->param_values_.empty() ? nullptr
				: &owner_->param_values_[size_t(owner_->param_base_[i])];
		sp.count = owner_->param_count_[i];
		FieldResources res;
		fn(ctx, owner_->slots_[i], sp, res);
	}

	Sample s{};
	s.sdf = ctx.f(p.channel_slot("sdf"));
	s.material = uint16_t(ctx.f(p.channel_slot("material")));
	return s;
}

} // namespace ve
```

- [ ] **Step 4: Run the tests**

Run: `./build.sh --test`
Expected: PASS, all four `TEST_CASE`s.

- [ ] **Step 5: Commit**

```bash
git add extension/src/terrain/pipeline_field_generator.h extension/src/terrain/pipeline_field_generator.cpp extension/tests/test_pipeline_field_generator.cpp
git commit -m "feat: PipelineFieldGenerator, the CPU half of the stage pipeline"
```

---

# Phase 5 — The equivalence port

## Task 15: Port today's terrain to stages

**Files:**
- Create: `shaders/stages/hills.field.glslh`, `shaders/stages/cave.field.glslh`, `shaders/stages/height_bands.field.glslh`
- Create: `assets/pipelines/default.pipeline`
- Create: `extension/src/terrain/builtin_stages.cpp`
- Create: `extension/tests/test_pipeline_equivalence.cpp`
- Create: `shaders/field_ops.glslh` (the prelude split out of `shaders/field.glslh`)
- Modify: `shaders/field.glslh`

**Interfaces:**
- Consumes: everything from Tasks 8–14
- Produces: `ve::stage_hills`, `ve::stage_cave`, `ve::stage_height_bands` registered in the stage library; `assets/pipelines/default.pipeline`, consumed by Task 12's golden test and Task 17's injection

This is the gate. When it passes, a creator-authored pipeline reproduces today's terrain exactly.

- [ ] **Step 1: Split the prelude out of field.glslh**

The generated source replaces `base_field()` and keeps everything else. Move everything in `shaders/field.glslh` from `const uint OP_SPHERE_SUBTRACT` onward — the op constants, the op pool declaration, `op_touches_aabb`, `apply_op`, `eval_field`, and every other helper **except** `hills()` and `base_field()` — into a new `shaders/field_ops.glslh`. In that moved copy, change `eval_field`'s call to `base_field(p, sdf, mat)` into `eval_base_field(p, sdf, mat)`.

Leave `shaders/field.glslh` as it is for now; Task 17 replaces it. The build must still work at the end of this task.

- [ ] **Step 2: Write the three stage sources**

`shaders/stages/hills.field.glslh` — the GLSL function must be named `stage_<//!stage name>`:

```glsl
//!stage     hills
//!kind      field
//!out       sdf : float
//!out       height : float
//!param     amp_a : float = 6.0
//!param     amp_b : float = 3.0
//!param     amp_c : float = 1.0
//!lipschitz 2.0
//!cpu       ve::stage_hills

// Ported verbatim from ve::AnalyticGenerator::sample (generator.cpp). SURFACE_Y comes from
// the prelude. `height` is published so the material stage can band on it without
// recomputing the sum.
void stage_hills(inout FieldCtx ctx) {
	float x = ctx.p.x, z = ctx.p.z;
	ctx.height = P.hills_amp_a * sin(x * 0.11) * cos(z * 0.13)
	           + P.hills_amp_b * sin(x * 0.031 + 1.7) * sin(z * 0.043)
	           + P.hills_amp_c * sin(x * 0.23 + z * 0.19);
	ctx.sdf = ctx.p.y - SURFACE_Y - ctx.height;
}
```

`shaders/stages/cave.field.glslh`:

```glsl
//!stage     cave
//!kind      field
//!in        sdf : float
//!out       sdf : float
//!param     cx : float = 30.0
//!param     cz : float = 30.0
//!param     depth : float = 2.0
//!param     radius : float = 5.0
//!lipschitz 1.0
//!cpu       ve::stage_cave

// CSG subtract of one sphere, exactly as AnalyticGenerator does it. max(a, -b) cannot raise
// the gradient bound, hence lipschitz 1.0.
void stage_cave(inout FieldCtx ctx) {
	float h = P.hills_amp_a * sin(P.cave_cx * 0.11) * cos(P.cave_cz * 0.13)
	        + P.hills_amp_b * sin(P.cave_cx * 0.031 + 1.7) * sin(P.cave_cz * 0.043)
	        + P.hills_amp_c * sin(P.cave_cx * 0.23 + P.cave_cz * 0.19);
	float cy = SURFACE_Y + h - P.cave_depth;
	float sphere = length(ctx.p - vec3(P.cave_cx, cy, P.cave_cz)) - P.cave_radius;
	ctx.sdf = max(ctx.sdf, -sphere);
}
```

`shaders/stages/height_bands.field.glslh`:

```glsl
//!stage     height_bands
//!kind      field
//!in        sdf : float
//!in        height : float
//!out       material : uint
//!lipschitz 1.0
//!cpu       ve::stage_height_bands

// Material 0 is air. Bands mirror AnalyticGenerator: rock above 4 m, grass above 1 m, dirt
// below. Reads the sdf AFTER the cave stage, so carved space is correctly air.
void stage_height_bands(inout FieldCtx ctx) {
	if (ctx.sdf > 0.0) { ctx.material = 0u; return; }
	ctx.material = ctx.height > 4.0 ? 2u : (ctx.height > 1.0 ? 1u : 3u);
}
```

Note the param naming: the generated UBO flattens to `<stage>_<param>`, so `hills`'s `amp_a` is `P.hills_amp_a` and is visible to the `cave` stage too. That cross-stage read is intentional — it is how the cave's centre stays pinned to the hill surface, exactly as the C++ does.

- [ ] **Step 3: Write the pipeline resource**

Create `assets/pipelines/default.pipeline`:

```
# The terrain the engine shipped with, expressed as stages. Task 15's equivalence test
# asserts this produces bit-identical bricks to ve::AnalyticGenerator.
seed      1337
lipschitz 2.0

stage stages/hills.field.glslh
stage stages/cave.field.glslh
stage stages/height_bands.field.glslh
```

- [ ] **Step 4: Write the C++ mirrors**

Create `extension/src/terrain/builtin_stages.cpp`:

```cpp
// The CPU mirrors of shaders/stages/*.field.glslh. Each function must stay line-for-line
// equivalent to its GLSL twin; tests/test_field_diff.gd is what catches drift.
#include "terrain/stage_library.h"
#include "generator/generator.h"  // ve::kSurfaceY
#include <cmath>

namespace ve {

// Slot layout, from PipelineFieldGenerator::create: extra[] holds this stage's declared
// WRITES in order, then its READS in order.
void stage_hills(FieldCtx &ctx, const StageSlots &s, const StageParams &p,
		const FieldResources &) {
	const int sdf = s.extra[0];      // //!out sdf
	const int height = s.extra[1];   // //!out height
	const float x = ctx.v(s.p)[0], y = ctx.v(s.p)[1], z = ctx.v(s.p)[2];
	const float h = p.at(0) * sinf(x * 0.11f) * cosf(z * 0.13f)
	              + p.at(1) * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	              + p.at(2) * sinf(x * 0.23f + z * 0.19f);
	ctx.f(height) = h;
	ctx.f(sdf) = y - kSurfaceY - h;
}

void stage_cave(FieldCtx &ctx, const StageSlots &s, const StageParams &p,
		const FieldResources &) {
	const int sdf = s.extra[0];
	// The cave's params start at index 0 of ITS OWN slice; the hills amplitudes it needs are
	// the literals the GLSL reads through P.hills_*, which are the same constants.
	const float cx = p.at(0), cz = p.at(1), depth = p.at(2), radius = p.at(3);
	const float h = 6.0f * sinf(cx * 0.11f) * cosf(cz * 0.13f)
	              + 3.0f * sinf(cx * 0.031f + 1.7f) * sinf(cz * 0.043f)
	              + 1.0f * sinf(cx * 0.23f + cz * 0.19f);
	const float cy = kSurfaceY + h - depth;
	const float dx = ctx.v(s.p)[0] - cx, dy = ctx.v(s.p)[1] - cy, dz = ctx.v(s.p)[2] - cz;
	const float sphere = sqrtf(dx * dx + dy * dy + dz * dz) - radius;
	ctx.f(sdf) = fmaxf(ctx.f(sdf), -sphere);
}

void stage_height_bands(FieldCtx &ctx, const StageSlots &s, const StageParams &,
		const FieldResources &) {
	const int material = s.extra[0];  // //!out material
	const int sdf = s.extra[1];       // //!in sdf
	const int height = s.extra[2];    // //!in height
	if (ctx.f(sdf) > 0.0f) { ctx.f(material) = 0.0f; return; }
	const float h = ctx.f(height);
	ctx.f(material) = h > 4.0f ? 2.0f : (h > 1.0f ? 1.0f : 3.0f);
}

} // namespace ve

VE_REGISTER_STAGE("ve::stage_hills", ve::stage_hills);
VE_REGISTER_STAGE("ve::stage_cave", ve::stage_cave);
VE_REGISTER_STAGE("ve::stage_height_bands", ve::stage_height_bands);
```

The `cave` stage's use of literal hills amplitudes rather than `p.at()` is deliberate and must match the GLSL, which reads `P.hills_amp_*`. If you later make the hills amplitudes overridable in the pipeline resource, both sides must change together — `test_field_diff.gd` will catch it if only one does.

- [ ] **Step 5: Write the equivalence test**

Create `extension/tests/test_pipeline_equivalence.cpp`:

```cpp
// THE GATE. The stage pipeline must reproduce ve::AnalyticGenerator exactly -- same sdf
// bits, same materials, same whole bricks. Anything less means the port changed the world.
#include <doctest/doctest.h>
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_manifest.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

namespace {

std::string slurp(const std::string &p) {
	std::ifstream f(p);
	REQUIRE_MESSAGE(f.good(), "cannot open " + p);
	std::ostringstream o;
	o << f.rdbuf();
	return o.str();
}

std::unique_ptr<ve::PipelineFieldGenerator> default_pipeline() {
	const std::string root(VE_REPO_ROOT);
	ve::PipelineDesc d;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_pipeline_desc(
			slurp(root + "/assets/pipelines/default.pipeline"), &d, &err), err);
	std::vector<ve::StageManifest> loaded;
	for (const auto &r : d.stages) {
		ve::StageManifest m;
		REQUIRE_MESSAGE(ve::parse_stage_manifest(slurp(root + "/shaders/" + r.path), &m, &err), err);
		loaded.push_back(m);
	}
	ve::ResolvedPipeline p;
	REQUIRE_MESSAGE(ve::resolve_pipeline(d, loaded, &p, &err), err);
	ve::PipelineFieldGenerator *g = ve::PipelineFieldGenerator::create(p, &err);
	REQUIRE_MESSAGE(g != nullptr, err);
	return std::unique_ptr<ve::PipelineFieldGenerator>(g);
}

uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

} // namespace

TEST_CASE("the default pipeline reproduces the analytic field over the baseline corpus") {
	auto g = default_pipeline();
	ve::AnalyticGenerator ref;
	uint32_t s = 20260903u;
	auto next = [&s](float lo, float hi) {
		s = s * 1664525u + 1013904223u;
		return lo + (hi - lo) * (float((s >> 8) & 0xFFFFFFu) / 16777216.0f);
	};
	int checked = 0;
	for (int i = 0; i < 512; i++) {
		const float x = next(-20.0f, 60.0f), y = next(21.2f, 81.2f), z = next(-20.0f, 60.0f);
		const ve::Sample a = ref.sample(x, y, z);
		const ve::Sample b = g->eval(x, y, z);
		CHECK(bits(a.sdf) == bits(b.sdf));
		CHECK(a.material == b.material);
		checked++;
	}
	CHECK(checked == 512);
}

TEST_CASE("the default pipeline reproduces whole bricks") {
	auto g = default_pipeline();
	ve::AnalyticGenerator ref;
	const ve::IVec3 bricks[] = {
		{0, 64, 0}, {15, 63, 15}, {37, 63, 37}, {37, 62, 37},
		{-25, 60, -25}, {0, 40, 0}, {0, 90, 0}, {1000, 64, 1000},
	};
	for (const ve::IVec3 &b : bricks) {
		ve::BrickEval want{}, got{};
		ve::eval_brick(ref, nullptr, 0, b, &want);
		ve::eval_brick(g->sampler(), nullptr, 0, b, &got);
		CHECK(std::memcmp(want.brick.sdf, got.brick.sdf, sizeof(want.brick.sdf)) == 0);
		CHECK(std::memcmp(want.brick.mat, got.brick.mat, sizeof(want.brick.mat)) == 0);
		CHECK(std::memcmp(&want.mips, &got.mips, sizeof(want.mips)) == 0);
	}
}

TEST_CASE("the pipeline reports the analytic generator's lipschitz bound") {
	auto g = default_pipeline();
	ve::AnalyticGenerator ref;
	CHECK(g->sampler().lipschitz() == doctest::Approx(ref.lipschitz()));
}
```

- [ ] **Step 6: Run it**

Run: `./build.sh --test`
Expected: PASS.

If the sdf **bits** differ while the values look equal, the cause is float evaluation order — the GLSL and C++ must sum the three hills terms in the same order, and `AnalyticGenerator` computes `(y - kSurfaceY) - h` while a naive port might write `y - (kSurfaceY + h)`. Match `generator.cpp` term for term rather than loosening the check to `Approx`. Bit equality is the point of this task.

- [ ] **Step 7: Complete Task 12**

Return to Task 12 and run its Steps 3–4 now that `assets/pipelines/default.pipeline` and `shaders/field_ops.glslh` exist.

- [ ] **Step 8: Commit**

```bash
git add shaders/stages shaders/field_ops.glslh assets/pipelines extension/src/terrain/builtin_stages.cpp extension/tests/test_pipeline_equivalence.cpp
git commit -m "feat: port the analytic terrain to a stage pipeline, bit-identically"
```

---

# Phase 6 — GPU injection

## Task 16: The set-1 uniform set

**Files:**
- Create: `extension/src/render/field_context_set.h`, `extension/src/render/field_context_set.cpp`
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`

**Interfaces:**
- Consumes: `ve::ResolvedPipeline` (Task 10)
- Produces, consumed by Task 17:

```cpp
namespace godot {
class FieldContextSet {
public:
    ~FieldContextSet();
    // `shader` is any pipeline compiled from the generated field; the set is created against
    // its set-1 layout and is reusable across every field-consuming shader, because they all
    // #include the same generated declarations.
    bool initialize(RenderingDevice *rd, RID shader, const ve::ResolvedPipeline &p);
    void teardown();
    bool is_valid() const;
    RID uniform_set() const;
    // Records set 1 into an OPEN compute list, next to the caller's own set 0.
    void bind(RenderingDevice *rd, int64_t list) const;
};
}
```

- [ ] **Step 1: Write the header**

Create `extension/src/render/field_context_set.h`:

```cpp
#pragma once
// Set 1: the terrain pipeline's context. Every field-consuming pass binds this beside its
// own untouched set 0. Binding 0 is the params UBO, binding 1 the sector-slot map (empty in
// Plan A; Plan B fills it), bindings 2..N the sampled context resources.
//
// Bound UNCONDITIONALLY, even when a pipeline declares no resources, so the binding code at
// ten call sites has no special case.
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "terrain/pipeline.h"

namespace godot {

class FieldContextSet {
public:
	~FieldContextSet();
	bool initialize(RenderingDevice *rd, RID shader, const ve::ResolvedPipeline &p);
	void teardown();
	bool is_valid() const { return uset_.is_valid(); }
	RID uniform_set() const { return uset_; }
	void bind(RenderingDevice *rd, int64_t list) const;

private:
	RenderingDevice *rd_ = nullptr;
	RID params_ubo_, sector_map_, uset_;
};

} // namespace godot
```

- [ ] **Step 2: Write the implementation**

Create `extension/src/render/field_context_set.cpp`:

```cpp
#include "render/field_context_set.h"
#include <godot_cpp/classes/rd_uniform.hpp>

using namespace godot;

FieldContextSet::~FieldContextSet() { teardown(); }

bool FieldContextSet::initialize(RenderingDevice *rd, RID shader, const ve::ResolvedPipeline &p) {
	teardown();
	rd_ = rd;
	if (rd == nullptr || !shader.is_valid()) return false;

	// std140 pads every scalar to 16 bytes. The generated UBO declares one scalar per param
	// in pipeline order, so the CPU-side packing must pad identically. A pipeline with no
	// params still gets one vec4 (the generated `_unused`), so the buffer is never empty --
	// RenderingDevice rejects a zero-byte uniform buffer.
	PackedByteArray bytes;
	const int count = p.params.empty() ? 4 : int(p.params.size()) * 4;
	bytes.resize(count * 4);
	bytes.fill(0);
	for (size_t i = 0; i < p.params.size(); i++)
		bytes.encode_float(int64_t(i) * 16, p.params[i].value);
	params_ubo_ = rd->uniform_buffer_create(bytes.size(), bytes);
	if (!params_ubo_.is_valid()) return false;

	// Plan A ships an empty sector map: one int, value -1, meaning "no sector resident", so
	// sample_sector_* returns its declared fallback everywhere. Plan B replaces this.
	PackedByteArray empty;
	empty.resize(4);
	empty.encode_s32(0, -1);
	sector_map_ = rd->storage_buffer_create(empty.size(), empty);
	if (!sector_map_.is_valid()) { teardown(); return false; }

	TypedArray<RDUniform> uniforms;
	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u0->set_binding(0);
	u0->add_id(params_ubo_);
	uniforms.push_back(u0);

	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u1->set_binding(1);
	u1->add_id(sector_map_);
	uniforms.push_back(u1);

	// Plan A resolves no sampled resources; resolve_pipeline() accepts them but no shipped
	// stage declares one. Assert rather than silently binding an incomplete set.
	if (!p.resources.empty()) { teardown(); return false; }

	uset_ = rd->uniform_set_create(uniforms, shader, 1);
	if (!uset_.is_valid()) { teardown(); return false; }
	return true;
}

void FieldContextSet::teardown() {
	if (rd_ == nullptr) return;
	if (uset_.is_valid()) { rd_->free_rid(uset_); uset_ = RID(); }
	if (sector_map_.is_valid()) { rd_->free_rid(sector_map_); sector_map_ = RID(); }
	if (params_ubo_.is_valid()) { rd_->free_rid(params_ubo_); params_ubo_ = RID(); }
	rd_ = nullptr;
}

void FieldContextSet::bind(RenderingDevice *rd, int64_t list) const {
	if (uset_.is_valid()) rd->compute_list_bind_uniform_set(list, uset_, 1);
}
```

Match the neighbouring passes' exact godot-cpp idioms — open `extension/src/render/sun_ubo.cpp` and copy its buffer-creation and `RDUniform` construction style rather than inventing one. If `uniform_set_create`'s third parameter is named differently in this godot-cpp version, keep the value `1`; that is the set index.

- [ ] **Step 3: Own it in the orchestrator**

In `extension/src/render/orchestrator.h`, add `class FieldContextSet;` to the forward declarations, a `FieldContextSet *field_context_ = nullptr;` member beside `sun_ubo_`, and an accessor `FieldContextSet *field_context() { return field_context_; }`.

In `orchestrator.cpp`, create it inside `ensure_gpu_graph()` immediately after the brick-gen pass is created (it needs a compiled shader for the set layout, and `BrickGenPass`'s is the first one built from the generated field), and delete it in `teardown_render_passes()` **before** the gen-pass delete, preserving that function's documented verbatim order by inserting rather than reordering.

- [ ] **Step 4: Build**

Run: `./build.sh --test`
Expected: builds clean; native suite unchanged.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/field_context_set.h extension/src/render/field_context_set.cpp extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp
git commit -m "feat: set 1, the terrain pipeline's context uniform set"
```

## Task 17: Inject the generated field and bind set 1

**Files:**
- Modify: `shaders/field.glslh`
- Modify: `extension/src/voxel_world.cpp`, `extension/src/core/world_store.h`
- Modify: the ten field-consuming passes' dispatch sites
- Create: `tests/test_pipeline_reload.gd`

**Interfaces:**
- Consumes: `ve::generate_field_glslh` (Task 11), `ve::PipelineFieldGenerator` (Task 14), `FieldContextSet` (Task 16), `ve::set_shader_source_override` (`render/shader_loader.h`)
- Produces: a world whose GPU and CPU fields both come from `assets/pipelines/default.pipeline`

- [ ] **Step 1: Load and compile the pipeline at world init**

In `extension/src/voxel_world.cpp`, add this free function above `VoxelWorld::ensure_initialized`, and call it from `ensure_initialized()` **before** `ensure_gpu_graph()` — the GPU graph compiles shaders that must already see the overridden `field.glslh`.

```cpp
namespace {

// Reads a res:// text file into a std::string. The terrain pipeline's parsers are pure C++
// and take strings, which is what keeps them in the native test suite; this is the only
// place Godot's FileAccess touches them.
bool read_res_text(const String &path, std::string *out) {
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) return false;
	*out = f->get_as_text().utf8().get_data();
	return true;
}

} // namespace

// Compiles assets/pipelines/default.pipeline into (a) a generated field.glslh installed as a
// shader-source override and (b) a PipelineFieldGenerator on the seam. On ANY failure the
// world keeps today's hardcoded terrain: a bad pipeline must degrade, never kill the world.
void VoxelWorld::load_terrain_pipeline() {
	std::string src, err;
	if (!read_res_text("res://assets/pipelines/default.pipeline", &src)) {
		UtilityFunctions::push_warning("terrain pipeline: cannot read default.pipeline; "
				"keeping the built-in field");
		return;
	}
	ve::PipelineDesc desc;
	if (!ve::parse_pipeline_desc(src, &desc, &err)) {
		UtilityFunctions::push_error(String("terrain pipeline: ") + err.c_str());
		return;
	}

	std::vector<ve::StageManifest> loaded;
	for (const ve::PipelineStageRef &r : desc.stages) {
		std::string stage_src;
		const String path = String("res://shaders/") + r.path.c_str();
		if (!read_res_text(path, &stage_src)) {
			UtilityFunctions::push_error("terrain pipeline: cannot read " + path);
			return;
		}
		ve::StageManifest m;
		if (!ve::parse_stage_manifest(stage_src, &m, &err)) {
			UtilityFunctions::push_error(path + ": " + err.c_str());
			return;
		}
		loaded.push_back(m);
	}

	ve::ResolvedPipeline resolved;
	if (!ve::resolve_pipeline(desc, loaded, &resolved, &err)) {
		UtilityFunctions::push_error(String("terrain pipeline: ") + err.c_str());
		return;
	}

	std::string prelude;
	if (!read_res_text("res://shaders/field_ops.glslh", &prelude)) {
		UtilityFunctions::push_error("terrain pipeline: cannot read field_ops.glslh");
		return;
	}

	// The CPU half must succeed BEFORE the GPU override is installed. Installing the override
	// first and then failing here would leave the GPU on the pipeline's field and the CPU on
	// the built-in one -- the exact divergence Phase 0 exists to prevent.
	ve::PipelineFieldGenerator *gen = ve::PipelineFieldGenerator::create(resolved, &err);
	if (gen == nullptr) {
		UtilityFunctions::push_error(String("terrain pipeline: ") + err.c_str());
		return;
	}

	ve::set_shader_source_override("field.glslh", ve::generate_field_glslh(resolved, prelude));
	store_->set_terrain_pipeline(resolved);
	store_->set_generator(gen);  // WorldStore takes ownership, as it does today
}
```

Declare `void load_terrain_pipeline();` in the private section of `extension/src/voxel_world.h`, and add the includes `terrain/pipeline.h`, `terrain/stage_manifest.h`, `terrain/field_codegen.h`, `terrain/pipeline_field_generator.h`, `render/shader_loader.h` and `<godot_cpp/classes/file_access.hpp>`.

Store the resolved pipeline on `WorldStore` so `FieldContextSet::initialize` can reach it — add to `extension/src/core/world_store.h`:

```cpp
	// The compiled terrain pipeline. Empty until load_terrain_pipeline() succeeds; the
	// generator seam falls back to ProceduralFieldGenerator until then.
	const ve::ResolvedPipeline &terrain_pipeline() const { return terrain_pipeline_; }
	void set_terrain_pipeline(const ve::ResolvedPipeline &p) { terrain_pipeline_ = p; }
```

with a `ve::ResolvedPipeline terrain_pipeline_;` member.

- [ ] **Step 2: Replace field.glslh with a stub**

`shaders/field.glslh` on disk becomes the fallback used when no override is installed. Replace its contents with `#include "field_ops.glslh"` plus a `base_field`-shaped `eval_base_field` holding today's hardcoded hills and cave, and a header comment explaining that the terrain pipeline overrides this file wholesale at runtime through `ve::set_shader_source_override`.

Keep `SURFACE_Y` and the op constants in `field_ops.glslh` so both the stub and the generated source get them.

- [ ] **Step 3: Bind set 1 at the seven engine dispatch sites**

Seven shaders include `field.glslh` and are dispatched from C++. In each pass below, add immediately after its existing `compute_list_bind_uniform_set(list, uset_, 0)`:

```cpp
	if (field_context != nullptr) field_context->bind(rd, list);
```

| Shader | Pass |
|---|---|
| `brick_gen.comp.glsl` | `render/brick_gen_pass.cpp` |
| `brick_mark.comp.glsl` | `render/region_pass.cpp` (the `mark` pipeline) |
| `raymarch.comp.glsl` | `render/raymarch_pass.cpp` |
| `mesh_field.comp.glsl` | `render/mesh_pass.cpp` |
| `lod_field.comp.glsl` | `render/lod_build_pass.cpp` |
| `island_extract.comp.glsl` | `render/island_extract_pass.cpp` |
| `brick_consolidate.comp.glsl` | `render/consolidate_pass.cpp` |

Thread a `const FieldContextSet *field_context` parameter through each `dispatch(...)` signature, supplied by the caller from `render->field_context()`. Append the parameter; do not reorder existing ones.

- [ ] **Step 3b: Give the GDScript probe tests a set 1 of their own**

The remaining three includers — `field_probe`, `field_gradient_probe` and `field_filter_probe` — are **not** dispatched from C++. Five GDScript suites load them with `debug_load_shader` and dispatch them on their **own local `RenderingDevice`**, binding only set 0:

| Suite | Probe |
|---|---|
| `tests/test_field_diff.gd` | `field_probe.comp.glsl` |
| `tests/test_field_volume_diff.gd` | `field_probe.comp.glsl` |
| `tests/test_field_gradient.gd` | `field_gradient_probe.comp.glsl` |
| `tests/test_op_filter_gpu.gd` | `field_filter_probe.comp.glsl` |
| `tests/test_field_baseline_gpu.gd` | `field_probe.comp.glsl` (added in Task 3) |

Once the generated field declares set 1, those dispatches fail validation. A RID from the world's device is useless on a test's local device, so each test must build its own set 1. Add a hook that hands over the packed params so the tests do not have to duplicate the layout rules:

```cpp
	// The set-1 params UBO contents for the world's terrain pipeline, std140-padded exactly
	// as FieldContextSet packs them. Tests dispatching the field probes on their OWN local
	// RenderingDevice need this to build a matching set 1.
	PackedByteArray debug_field_params_bytes();
```

Implement it in `hooks.cpp` by reusing the same packing loop as `FieldContextSet::initialize` — one scalar per param at a 16-byte stride, or a single zeroed `vec4` when the pipeline declares no params.

Then, in each of the five probe-dispatching suites, after the existing set-0 uniform set is created, add:

```gdscript
func _make_field_set(rd: RenderingDevice, shader: RID) -> RID:
	# Set 1, mirroring FieldContextSet: binding 0 params UBO, binding 1 sector map (one
	# int = -1, "no sector resident"). Plan A declares no sampled resources.
	var params: PackedByteArray = _world.hooks().debug_field_params_bytes()
	if params.is_empty():
		params.resize(16)
		params.fill(0)
	var ubo := rd.uniform_buffer_create(params.size(), params)
	var u0 := RDUniform.new()
	u0.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
	u0.binding = 0
	u0.add_id(ubo)

	var map_bytes := PackedByteArray()
	map_bytes.resize(4)
	map_bytes.encode_s32(0, -1)
	var ssbo := rd.storage_buffer_create(map_bytes.size(), map_bytes)
	var u1 := RDUniform.new()
	u1.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u1.binding = 1
	u1.add_id(ssbo)

	return rd.uniform_set_create([u0, u1], shader, 1)
```

and bind it beside the existing set 0:

```gdscript
	rd.compute_list_bind_uniform_set(list, _make_field_set(rd, shader), 1)
```

- [ ] **Step 3c: Verify the probe suites before touching anything else**

```bash
./gdunit_tests.sh -a res://tests/test_field_diff.gd -a res://tests/test_field_baseline_gpu.gd \
  -a res://tests/test_field_volume_diff.gd -a res://tests/test_field_gradient.gd \
  -a res://tests/test_op_filter_gpu.gd
```

Expected: PASS. A `Uniform set 1 is not bound` style validation error means a suite was missed; re-run `grep -rn "debug_load_shader" tests/*.gd` to find any dispatcher added since this plan was written.

- [ ] **Step 4: Run the full regression**

```bash
./build.sh --test && ./gdunit_tests.sh
```

Expected: only the 5 known baseline failures. In particular `test_field_diff.gd`, `test_field_baseline_gpu.gd`, `test_brick_diff.gd` and `test_generator_seam.gd` must all pass — they are now testing the *pipeline's* field on both sides.

If a shader fails to compile, read `reload_last_error_` via the existing debug facade; the message names the file and line in the generated source. Dump the generated text to a scratch file and inspect it directly rather than guessing.

- [ ] **Step 5: Write the reload-safety test**

Create `tests/test_pipeline_reload.gd`:

```gdscript
extends GdUnitTestSuite

# A broken stage must never kill a running world: preflight compiles every consumer against
# the new field BEFORE anything is torn down, so last-known-good pipelines survive and the
# error is reported instead of thrown.

var _world: VoxelWorld

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)

func test_a_broken_field_override_leaves_the_world_alive() -> void:
	var before: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	assert_int(before.size()).is_greater(0)

	_world.hooks().debug_set_shader_source_override("field.glslh",
		"this is not valid GLSL at all\n")
	_world.hooks().debug_request_shader_reload()
	# Pump frames so the render callback picks the reload up.
	for i in range(10):
		await get_tree().process_frame

	var stats: Dictionary = _world.hooks().debug_shader_reload_stats()
	assert_bool(stats["last_ok"]).is_false()
	assert_str(str(stats["last_error"])).is_not_empty()

	# The world still renders and the CPU field is untouched.
	var after: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	assert_int(after.size()).is_equal(before.size())
	for i in range(before.size()):
		assert_float(after[i]).is_equal_approx(before[i], 1e-5)

	_world.hooks().debug_clear_shader_source_overrides()
```

If `debug_set_shader_source_override`, `debug_request_shader_reload`, `debug_shader_reload_stats` or `debug_clear_shader_source_overrides` are not already bound in `hooks.h`, add thin wrappers over `ve::set_shader_source_override`, `RenderOrchestrator::request_shader_reload`, `RenderOrchestrator::reload_snapshot` and `ve::clear_shader_source_overrides` following the existing hook style. Check the exact key names `debug_shader_reload_stats` returns and match them in the test.

- [ ] **Step 6: Run it**

Run: `./gdunit_tests.sh -a res://tests/test_pipeline_reload.gd`
Expected: PASS.

- [ ] **Step 7: Full run and commit**

```bash
./build.sh --test && ./gdunit_tests.sh
git add shaders/field.glslh extension/src/voxel_world.cpp extension/src/core/world_store.h extension/src/render extension/src/debug tests/test_pipeline_reload.gd
git commit -m "feat: the world's field comes from the terrain pipeline"
```

---

## Done criteria

- `./build.sh --test` passes.
- `./gdunit_tests.sh` shows only the 5 known baseline failures recorded in Global Constraints.
- `grep -rn "AnalyticGenerator" extension/src --include="*.cpp" --include="*.h" | grep -v "^extension/src/generator/"` is empty.
- Editing `assets/pipelines/default.pipeline` — reordering `cave` after `height_bands`, say — visibly changes the terrain with no C++ rebuild.
- Deleting `//!cpu` from a stage produces a compiler error naming that stage, not a crash.
