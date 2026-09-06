# 4 km View Distance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `stream_radius_m = 4000` as the default, paid for by three cascaded sun shadow maps, terrain with relief worth looking at at that range, and measured evidence of what it costs.

**Architecture:** The sun's single camera-centred sphere fit becomes three nested sphere fits whose radii derive from `kLodBaseCell` and `stream_radius_m`, rendered into one 3-layer array texture with per-layer dirty state. `LodTree` gains an explicit per-cascade shadow cut with a minimum-level clamp, replacing the single cut `walk()` produced as a side effect. Budgets and build throughput are instrumented and measured before any constant moves.

**Tech Stack:** C++20, godot-cpp (Godot 4.7.2), GLSL 460 compute/raster on `RenderingDevice`, doctest for pure-core native tests, gdUnit4 for GPU tests, SCons.

**Spec:** `docs/superpowers/specs/2026-09-05-4km-view-distance-design.md`

## Global Constraints

- **Branch:** `4km-view-distance`, already checked out. The spec is committed at `1e6bde3`.
- **`kSunCascades` is 3.** Fixed, not configurable. The radii derive; a fourth cascade needs a reason the derivation does not supply (spec §7).
- **Cascade 0's `min_level` is hard-wired to 0**, never computed from the clamp rule. Its texel equals `kLodBaseCell` exactly in real arithmetic, so evaluating `lod_cell_size(0) <= texel_world` is a float-rounding coin flip (spec §3).
- **Cascade `N-1` must remain bit-identical to today's single map** at every radius: the same `ve::sun_ortho_sphere(dir, cam, stream_radius_m, SunShadowPass::kSize)` call with the same arguments (spec §2).
- **Native tests are zero-godot-cpp.** `extension/SConstruct` globs `src/{world,generator,core,terrain,mesh,connectivity,lod,shade}/*.cpp` into `pure_sources`, minus `src/mesh/consolidation.cpp` and `src/lod/lod_system.cpp`. Anything a native test touches must compile without godot-cpp. New files under `src/shade/` and `src/lod/` are picked up automatically.
- **Build commands:** native `cd extension && scons test -j8`; single native case `extension/build/tests/ve_tests --test-case="<name>"`; extension `./build.sh`; gdUnit all `./gdunit_tests.sh`; gdUnit one suite `./gdunit_tests.sh -a res://tests/test_x.gd`.
- **gdUnit exits 100 with 5 known-failing assertions across 4 suites on clean `main`.** That is the baseline, not a regression. Task 0 records it; every later task compares against it.
- **GPU timestamps return `samples=0` on this M1/Metal setup.** Report wall-frame time and say `UNMEASURED` for GPU verdicts. Never present an unavailable GPU number as a zero, and never compare M1/Metal wall times against the RTX 4070/Vulkan figures in `docs/PORTFOLIO.md` as though they were the same measurement.
- **Commit after every task.** Message bodies end with:
  ```
  Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
  ```

---

## File Structure

**Created:**
- `extension/src/shade/sun_cascades.h` / `.cpp` — the radii/texel/min-level derivation. Pure; no godot-cpp. Header stays standalone (no `lod_grid.h`) so it can be included anywhere.
- `extension/tests/test_sun_cascades.cpp` — native tests for the derivation.
- `shaders/stages/relief.field.glslh` — the long-wavelength relief terrain stage.
- `assets/pipelines/golden.pipeline` — today's three stages, frozen, so the golden corpora keep proving the generator did not move.
- `tests/test_sun_cascades_gpu.gd` — gdUnit cascade tests (map, selection, peter-panning).
- `reports/4km-baseline/`, `reports/4km-after/` — evidence.

**Modified:**
- `extension/src/lod/lod_tree.h` / `.cpp` — `shadow_cut()` replaces `LodWalkResult::shadow_draws`.
- `extension/src/render/sun_shadow_pass.h` / `.cpp` — 3-layer array, per-cascade state, `needs_rebuild()`.
- `extension/src/render/deferred_pass.h` / `.cpp` — cascade arrays in `Params`; 256-byte `SunBlock`.
- `shaders/deferred.comp.glsl` — `sampler2DArray`, per-cascade selection and bias.
- `extension/src/raymarch_compositor.cpp` — the cascade loop.
- `extension/src/voxel_world.h` / `.cpp` — `sun_ortho(int)`, four properties, pipeline path.
- `extension/src/lod/lod_system.h` / `.cpp` — `prepare_shadow_raster(radius, min_level)`, chunk-record budget, the literal-`8` fix.
- `extension/src/render/lod_pool.h` / `.cpp` — `max_chunk_records` parameter, exhaustion warning.
- `extension/src/debug/hooks.cpp` — cascade-aware sun hooks, high-water stats.
- `extension/src/terrain/builtin_stages.cpp` — `ve::stage_relief`.
- `assets/pipelines/default.pipeline` — the relief stage line.
- `demo/benchmark.gd` — `lod_pending`, `frames_to_horizon`.
- `demo/main.tscn` — the 4000 m default.
- `tests/test_field_baseline_gpu.gd` — repointed at `golden.pipeline`.

---

## Task 0: Baseline and characterization

Nothing in this task changes production code. It exists so that every failure in Tasks 1-9 is attributable, and so the cascade rewrite has a pinned "what it did before" to be neutral against.

**Files:**
- Create: `reports/4km-baseline/README.md`, `reports/4km-baseline/native.txt`, `reports/4km-baseline/gdunit.txt`
- Test: `extension/tests/test_lod_tree.cpp` (append), `extension/tests/test_sun_ortho.cpp` (append)

**Interfaces:**
- Consumes: nothing.
- Produces: `reports/4km-baseline/` as the comparison target for Task 9's evidence. Two characterization tests that Tasks 2 and 3 must keep passing.

- [ ] **Step 1: Record the native baseline**

```bash
mkdir -p reports/4km-baseline
cd extension && scons test -j8 2>&1 | tee ../reports/4km-baseline/native.txt; cd ..
tail -5 reports/4km-baseline/native.txt
```

Expected: a `test cases` / `assertions` summary line with 0 failures. Record the exact case and assertion counts — Task 9 compares against them.

- [ ] **Step 2: Record the gdUnit baseline**

```bash
./gdunit_tests.sh 2>&1 | tee reports/4km-baseline/gdunit.txt || true
grep -E "Test cases|failures|orphans" reports/4km-baseline/gdunit.txt | tail -5
```

Expected: exit 100, with 5 failing assertions across 4 suites (`test_collider_octants.gd`, `test_contact_shadow.gd`, `test_island_body.gd`, `test_collider_stream.gd`). This is the known baseline. If a *different* set fails, stop and report before continuing — the branch base is not clean.

- [ ] **Step 3: Write the characterization test for the shadow cut**

Append to `extension/tests/test_lod_tree.cpp`, before the final `}` of the file if it has a namespace close (it does not — append at end of file):

```cpp
// CHARACTERIZATION (Task 0). Pins the shadow cut that walk() produces today, so the
// Task 2 rewrite into LodTree::shadow_cut() can be shown to be neutral at the old radius.
// This test is REWRITTEN in Task 2 to call shadow_cut(); until then it must pass unchanged.
TEST_CASE("characterization: the shadow cut at 1638.4 m is frustum-free and non-overlapping") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 8);
	ve::LodWalkResult r;
	t.walk(c, &occ, 9999u, &r);

	// The sun's cut sees what the camera's cut cannot: it is never smaller.
	CHECK(r.shadow_draws.size() >= r.draws.size());
	CHECK(!r.shadow_draws.empty());

	// And it is a cut: no emitted chunk is an ancestor of another.
	for (size_t i = 0; i < r.shadow_draws.size(); i++)
		for (size_t j = i + 1; j < r.shadow_draws.size(); j++) {
			const ve::LodDrawItem &a = r.shadow_draws[i];
			const ve::LodDrawItem &b = r.shadow_draws[j];
			if (a.level == b.level) {
				CHECK(!(a.coord == b.coord));
				continue;
			}
			const ve::LodDrawItem &lo = a.level < b.level ? a : b;
			const ve::LodDrawItem &hi = a.level < b.level ? b : a;
			ve::IVec3 up = lo.coord;
			for (int l = lo.level; l < hi.level; l++) up = ve::lod_parent(up);
			CHECK(!(up == hi.coord));
		}
}
```

- [ ] **Step 4: Write the characterization test for the sun ortho at 1638.4 m**

Append to `extension/tests/test_sun_ortho.cpp`:

```cpp
// CHARACTERIZATION (Task 0). The exact fit that ships today at the old default radius.
// Task 1 introduces cascades; the outermost cascade must reproduce THIS matrix, bit for
// bit, at this radius. If cascades change these numbers, they changed what ships.
TEST_CASE("characterization: the shipping fit at stream_radius 1638.4") {
	const float cam[3] = {800.0f, 60.0f, 800.0f};
	const ve::SunOrtho o = ve::sun_ortho_sphere(ve::kSunDir, cam, 1638.4f, 2048);
	REQUIRE(o.valid);
	// 2 * 1638.4 / 2047 -- the texel depends on the radius and the map size and on
	// nothing else: not the sun's direction, not the camera's position.
	CHECK(o.texel_world == doctest::Approx(1.600782f).epsilon(1e-5));
	// 4R: the sphere spans 2R with R of margin on each side (fit_sphere's half_depth).
	CHECK(o.depth_range == doctest::Approx(4.0f * 1638.4f).epsilon(1e-4));
}
```

- [ ] **Step 5: Run both characterization tests and verify they PASS**

```bash
cd extension && scons test -j8 2>&1 | tail -5; cd ..
extension/build/tests/ve_tests --test-case="characterization*"
```

Expected: PASS. These pin existing behaviour, so they must pass *before* any change. If either fails, the assumption behind it is wrong — stop and report rather than adjusting the number to match.

- [ ] **Step 6: Write the baseline README**

```bash
cat > reports/4km-baseline/README.md <<'EOF'
# 4 km view distance — baseline

Recorded on branch `4km-view-distance` at the spec commit, before any Task 1+ change.
No production code was modified for this report.

- `native.txt` — `cd extension && scons test -j8`
- `gdunit.txt` — `./gdunit_tests.sh` (exit 100 expected)

The gdUnit baseline carries 5 failing assertions across 4 suites, all pre-existing:
test_collider_octants.gd, test_contact_shadow.gd, test_island_body.gd,
test_collider_stream.gd. Every later task compares against this set, not against zero.

GPU timestamps report `samples=0` on this M1/Metal host, so no GPU pass budget is
measurable here. Benchmark evidence is filed in reports/4km-after/ by Task 9.
EOF
```

- [ ] **Step 7: Commit**

```bash
git add reports/4km-baseline extension/tests/test_lod_tree.cpp extension/tests/test_sun_ortho.cpp
git commit -m "$(cat <<'EOF'
test: baseline and characterization before the 4 km work

Pins the shadow cut and the 1638.4 m sun ortho fit so the cascade
rewrite can be shown neutral at the old radius, and records the gdUnit
baseline (exit 100, 5 known assertion failures in 4 suites) so later
failures are attributable.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 1: The cascade derivation

A pure function: radius, texel and minimum level for each cascade. No GPU, no godot-cpp, fully native-testable. Everything later in the plan reads its output rather than recomputing the arithmetic.

**Files:**
- Create: `extension/src/shade/sun_cascades.h`, `extension/src/shade/sun_cascades.cpp`
- Test: `extension/tests/test_sun_cascades.cpp`

**Interfaces:**
- Consumes: `ve::kLodBaseCell`, `ve::lod_cell_size(int)`, `ve::kLodLevels` from `lod/lod_grid.h` (in the `.cpp` only).
- Produces:
  - `ve::kSunCascades` — `constexpr int`, value 3.
  - `struct ve::SunCascade { float radius; float texel_world; int min_level; }`
  - `int ve::sun_cascades(float stream_radius_m, int map_size, ve::SunCascade out[ve::kSunCascades])` — fills `out[0..count-1]` innermost-first and returns the count (1 when the radius collapses, 0 when the inputs are unusable).

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_sun_cascades.cpp`:

```cpp
#include <doctest/doctest.h>
#include "shade/sun_cascades.h"
#include "shade/sun_ortho.h"
#include "shade/cel.h"
#include "lod/lod_grid.h"
#include <cmath>

// The derivation, stated once (spec section 2):
//   r0 = kLodBaseCell * (kSize - 1) / 2      cascade 0's texel IS the finest LoD cell
//   rN = stream_radius_m                      the outermost IS what ships today
//   r1 = sqrt(r0 * rN)                        constant ratio between texel sizes
TEST_CASE("the cascade radii and texels derive from the base cell and the stream radius") {
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(4000.0f, 2048, c);
	REQUIRE(n == 3);

	CHECK(c[0].radius == doctest::Approx(409.4f).epsilon(1e-5));
	CHECK(c[1].radius == doctest::Approx(1279.687f).epsilon(1e-5));
	CHECK(c[2].radius == doctest::Approx(4000.0f).epsilon(1e-6));

	CHECK(c[0].texel_world == doctest::Approx(0.400000f).epsilon(1e-5));
	CHECK(c[1].texel_world == doctest::Approx(1.250305f).epsilon(1e-5));
	CHECK(c[2].texel_world == doctest::Approx(3.908158f).epsilon(1e-5));
}

// Cascade 0's texel equals kLodBaseCell EXACTLY in real arithmetic, so evaluating
// "lod_cell_size(0) <= texel_world" for it is a float-rounding coin flip that would
// silently coarsen every near shadow. It is hard-wired instead.
TEST_CASE("cascade 0 is unclamped by construction, not by evaluating the rule") {
	ve::SunCascade c[ve::kSunCascades];
	REQUIRE(ve::sun_cascades(4000.0f, 2048, c) == 3);
	CHECK(c[0].min_level == 0);
}

// Descending below the texel is work whose result cannot be resolved: at cascade 2's
// 3.908 m texel a level-0 chunk is three texels across.
TEST_CASE("min_level stops the cut once a cell is finer than a shadow texel") {
	ve::SunCascade c[ve::kSunCascades];
	REQUIRE(ve::sun_cascades(4000.0f, 2048, c) == 3);
	CHECK(c[1].min_level == 1); // cell 0.8 <= 1.250 < cell 1.6
	CHECK(c[2].min_level == 3); // cell 3.2 <= 3.908 < cell 6.4
	for (int i = 0; i < 3; i++) {
		CHECK(ve::lod_cell_size(c[i].min_level) <= c[i].texel_world + 1e-4f);
		if (c[i].min_level + 1 < ve::kLodLevels)
			CHECK(ve::lod_cell_size(c[i].min_level + 1) > c[i].texel_world);
	}
}

// THE no-regression property. Whatever the radius, the outermost cascade is the map that
// ships today, produced by the same call with the same arguments.
TEST_CASE("the outermost cascade is exactly today's single map") {
	for (const float radius : {1638.4f, 2500.0f, 4000.0f}) {
		ve::SunCascade c[ve::kSunCascades];
		const int n = ve::sun_cascades(radius, 2048, c);
		REQUIRE(n >= 1);
		CHECK(c[n - 1].radius == doctest::Approx(radius).epsilon(1e-6));

		const float cam[3] = {800.0f, 60.0f, 800.0f};
		const ve::SunOrtho shipped = ve::sun_ortho_sphere(ve::kSunDir, cam, radius, 2048);
		REQUIRE(shipped.valid);
		CHECK(c[n - 1].texel_world == doctest::Approx(shipped.texel_world).epsilon(1e-6));
	}
}

// At the OLD default the whole table is a clean doubling, and cascade 2 reproduces the
// 1.601 m texel the characterization test pinned in Task 0.
TEST_CASE("at the old default radius the table is 409.4 / 819.0 / 1638.4") {
	ve::SunCascade c[ve::kSunCascades];
	REQUIRE(ve::sun_cascades(1638.4f, 2048, c) == 3);
	CHECK(c[0].radius == doctest::Approx(409.4f).epsilon(1e-5));
	CHECK(c[1].radius == doctest::Approx(819.0f).epsilon(1e-4));
	CHECK(c[2].radius == doctest::Approx(1638.4f).epsilon(1e-6));
	CHECK(c[2].texel_world == doctest::Approx(1.600782f).epsilon(1e-5));
}

// The degenerate case IS the old case: too small a radius to split means one map, and one
// map is exactly what shipped before cascades existed.
TEST_CASE("a radius inside cascade 0 collapses the set to a single cascade") {
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(200.0f, 2048, c);
	REQUIRE(n == 1);
	CHECK(c[0].radius == doctest::Approx(200.0f).epsilon(1e-6));
	CHECK(c[0].min_level == 0);
	CHECK(c[0].texel_world == doctest::Approx(400.0f / 2047.0f).epsilon(1e-6));
}

TEST_CASE("unusable inputs report no cascades rather than a degenerate matrix") {
	ve::SunCascade c[ve::kSunCascades];
	CHECK(ve::sun_cascades(0.0f, 2048, c) == 0);
	CHECK(ve::sun_cascades(-1.0f, 2048, c) == 0);
	CHECK(ve::sun_cascades(4000.0f, 1, c) == 0);
	CHECK(ve::sun_cascades(4000.0f, 0, c) == 0);
}

// The middle cascade is the geometric mean, so texel size steps by a CONSTANT RATIO
// rather than by a chosen constant. This is what makes the table derived, not tuned.
TEST_CASE("the radii are a geometric progression") {
	ve::SunCascade c[ve::kSunCascades];
	REQUIRE(ve::sun_cascades(4000.0f, 2048, c) == 3);
	CHECK(c[1].radius * c[1].radius ==
			doctest::Approx(c[0].radius * c[2].radius).epsilon(1e-4));
	CHECK(c[1].radius / c[0].radius ==
			doctest::Approx(c[2].radius / c[1].radius).epsilon(1e-4));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd extension && scons test -j8 2>&1 | tail -20; cd ..
```

Expected: FAIL — compile error, `shade/sun_cascades.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `extension/src/shade/sun_cascades.h`:

```cpp
#pragma once

namespace ve {

// Three nested camera-centred sphere fits. Fixed, not configurable: the radii DERIVE from
// kLodBaseCell and the stream radius (see sun_cascades below), and a fourth cascade would
// need a reason that derivation does not supply.
inline constexpr int kSunCascades = 3;

struct SunCascade {
	float radius = 0.0f;      // metres from the camera; sun_ortho_sphere fits this
	float texel_world = 0.0f; // one shadow texel in world metres: 2 * radius / (map_size - 1)
	// The level the shadow cut stops descending at. Resolving geometry finer than one
	// shadow texel is work whose result cannot be stored; see LodTree::shadow_cut.
	int min_level = 0;
};

// Fills `out[0 .. count-1]` innermost-first and returns the count.
//
// The derivation, in full:
//
//   r0 = kLodBaseCell * (map_size - 1) / 2   so cascade 0's texel IS the finest LoD cell.
//                                            Resolving a shadow finer than the finest
//                                            geometry that can cast it buys nothing.
//   rN = stream_radius_m                     so the outermost cascade is EXACTLY the map
//                                            that shipped before cascades existed -- the
//                                            same sun_ortho_sphere call, same arguments.
//   r1 = sqrt(r0 * rN)                       the geometric mean, so texel size steps by a
//                                            constant ratio instead of a chosen constant.
//
// Returns 1 (not kSunCascades) when stream_radius_m <= r0: there is nothing to split, and
// a single cascade is precisely the old behaviour. Returns 0 when the inputs are unusable,
// so a caller can refuse rather than bind a degenerate matrix.
int sun_cascades(float stream_radius_m, int map_size, SunCascade out[kSunCascades]);

} // namespace ve
```

- [ ] **Step 4: Write the implementation**

Create `extension/src/shade/sun_cascades.cpp`:

```cpp
#include "shade/sun_cascades.h"

#include "lod/lod_grid.h" // kLodBaseCell, kLodLevels, lod_cell_size
#include <cmath>

namespace ve {

namespace {

// The coarsest level whose CELLS are still at or finer than one shadow texel. Descending
// past it stores detail the map cannot hold.
int min_level_for_texel(float texel_world) {
	int level = 0;
	for (int l = 0; l < kLodLevels; l++) {
		if (lod_cell_size(l) <= texel_world) level = l;
		else break;
	}
	return level;
}

} // namespace

int sun_cascades(float stream_radius_m, int map_size, SunCascade out[kSunCascades]) {
	if (!out) return 0;
	if (!(stream_radius_m > 0.0f) || map_size <= 1) return 0;

	// map_size - 1, not map_size: sun_ortho_sphere's snap moves the min corner DOWN by up
	// to one texel, so the map is a texel wider than the sphere. The texel formula here
	// must match fit_sphere's or the two disagree about what a texel is.
	const float span = float(map_size - 1);
	const float r0 = kLodBaseCell * span * 0.5f;

	// Nothing to split. One cascade at the requested radius IS the pre-cascade behaviour,
	// which is the honest answer rather than three degenerate maps stacked on each other.
	if (stream_radius_m <= r0) {
		out[0].radius = stream_radius_m;
		out[0].texel_world = 2.0f * stream_radius_m / span;
		out[0].min_level = 0;
		return 1;
	}

	out[0].radius = r0;
	out[kSunCascades - 1].radius = stream_radius_m;
	for (int i = 1; i < kSunCascades - 1; i++) {
		// Geometric interpolation between r0 and rN. With kSunCascades == 3 this is the
		// single geometric mean; written as a general step so the constant can move
		// without the formula becoming wrong.
		const double t = double(i) / double(kSunCascades - 1);
		out[i].radius = float(double(r0) *
				std::pow(double(stream_radius_m) / double(r0), t));
	}

	for (int i = 0; i < kSunCascades; i++) {
		out[i].texel_world = 2.0f * out[i].radius / span;
		// Cascade 0 is hard-wired, NOT computed: its texel is kLodBaseCell exactly in real
		// arithmetic, so lod_cell_size(0) <= texel_world is an equality that float rounding
		// can decide either way -- and deciding it wrong silently coarsens every near shadow.
		out[i].min_level = i == 0 ? 0 : min_level_for_texel(out[i].texel_world);
	}
	return kSunCascades;
}

} // namespace ve
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd extension && scons test -j8 2>&1 | tail -10; cd ..
extension/build/tests/ve_tests --test-case="*cascade*"
```

Expected: PASS, all 8 cases. In particular "the outermost cascade is exactly today's single map" must pass — it is the no-regression property the rest of the plan leans on.

- [ ] **Step 6: Commit**

```bash
git add extension/src/shade/sun_cascades.h extension/src/shade/sun_cascades.cpp extension/tests/test_sun_cascades.cpp
git commit -m "$(cat <<'EOF'
feat: derive the sun cascade radii from the base cell and stream radius

Cascade 0's texel is kLodBaseCell -- there is no point resolving a
shadow finer than the finest geometry that can cast it -- and the
outermost cascade is the radius that ships today, so at every radius it
reproduces the existing single map exactly. The middle is the geometric
mean, so texel size steps by a constant ratio rather than a chosen one.

Cascade 0's min_level is hard-wired to 0 rather than computed: its texel
equals the base cell exactly, so evaluating the clamp rule there is a
float-rounding coin flip that would coarsen every near shadow.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 2: The per-cascade shadow cut

`walk()` stops producing a shadow cut as a side effect. `shadow_cut()` becomes an explicit query taking a radius and a minimum level, so a cascade can ask for its own cut — and so a cascade that will not rasterise this frame can be skipped entirely (Task 5).

**Files:**
- Modify: `extension/src/lod/lod_tree.h`, `extension/src/lod/lod_tree.cpp`
- Test: `extension/tests/test_lod_tree.cpp`

**Interfaces:**
- Consumes: `ve::SunCascade`, `ve::kSunCascades` from Task 1 (tests only; `lod_tree` itself takes plain floats and ints).
- Produces:
  - `void ve::LodTree::shadow_cut(const LodCamera &cam, float radius, int min_level, std::vector<LodDrawItem> *out) const`
  - `ve::LodWalkResult::shadow_draws` is **removed**. Task 5 is the only other caller.

- [ ] **Step 1: Rewrite the Task 0 characterization test against the new interface**

In `extension/tests/test_lod_tree.cpp`, replace the whole `TEST_CASE("characterization: the shadow cut at 1638.4 m is frustum-free and non-overlapping")` body from Task 0 with this. The *properties* it asserts are unchanged — only the call is:

```cpp
// CHARACTERIZATION (Task 0, rewritten in Task 2 onto shadow_cut). Same properties as
// before the rewrite: the sun's cut is frustum-free, so never smaller than the camera's,
// and it is a CUT -- no chunk is an ancestor of another.
TEST_CASE("characterization: the shadow cut at 1638.4 m is frustum-free and non-overlapping") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 8);
	ve::LodWalkResult r;
	t.walk(c, &occ, 9999u, &r);

	std::vector<ve::LodDrawItem> cut;
	t.shadow_cut(c, 1638.4f, 0, &cut);

	CHECK(cut.size() >= r.draws.size());
	CHECK(!cut.empty());

	for (size_t i = 0; i < cut.size(); i++)
		for (size_t j = i + 1; j < cut.size(); j++) {
			const ve::LodDrawItem &a = cut[i];
			const ve::LodDrawItem &b = cut[j];
			if (a.level == b.level) {
				CHECK(!(a.coord == b.coord));
				continue;
			}
			const ve::LodDrawItem &lo = a.level < b.level ? a : b;
			const ve::LodDrawItem &hi = a.level < b.level ? b : a;
			ve::IVec3 up = lo.coord;
			for (int l = lo.level; l < hi.level; l++) up = ve::lod_parent(up);
			CHECK(!(up == hi.coord));
		}
}
```

- [ ] **Step 2: Write the new failing tests**

Append to `extension/tests/test_lod_tree.cpp`:

```cpp
// The clamp is the whole reason cascade 2 is affordable: at a 3.908 m texel a level-0
// chunk is three texels across, and kLodNearDenseRadiusM FORCES level 0 within 300 m.
TEST_CASE("shadow_cut emits nothing below min_level") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 8);

	for (const int min_level : {0, 1, 3}) {
		std::vector<ve::LodDrawItem> cut;
		t.shadow_cut(c, 1638.4f, min_level, &cut);
		CHECK(!cut.empty());
		for (const ve::LodDrawItem &d : cut) CHECK(d.level >= min_level);
	}
}

// Clamping must not break the property the map depends on. The cut need not be COMPLETE --
// a node absent from the tree is skipped, exactly as before -- but one piece of ground may
// never be described twice, or two surfaces metres apart land in one shadow texel.
TEST_CASE("a clamped shadow cut still never describes one piece of ground twice") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 8);

	std::vector<ve::LodDrawItem> cut;
	t.shadow_cut(c, 1638.4f, 3, &cut);
	REQUIRE(!cut.empty());
	for (size_t i = 0; i < cut.size(); i++)
		for (size_t j = i + 1; j < cut.size(); j++) {
			const ve::LodDrawItem &a = cut[i];
			const ve::LodDrawItem &b = cut[j];
			if (a.level == b.level) {
				CHECK(!(a.coord == b.coord));
				continue;
			}
			const ve::LodDrawItem &lo = a.level < b.level ? a : b;
			const ve::LodDrawItem &hi = a.level < b.level ? b : a;
			ve::IVec3 up = lo.coord;
			for (int l = lo.level; l < hi.level; l++) up = ve::lod_parent(up);
			CHECK(!(up == hi.coord));
		}
}

// Clamping is a saving, not a rearrangement: a coarser cut is a SMALLER cut.
TEST_CASE("a coarser min_level yields a smaller cut") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 8);

	std::vector<ve::LodDrawItem> fine, coarse;
	t.shadow_cut(c, 1638.4f, 0, &fine);
	t.shadow_cut(c, 1638.4f, 3, &coarse);
	CHECK(coarse.size() <= fine.size());
	CHECK(!coarse.empty());
}

// Each cascade asks for its own radius, so a smaller radius must actually cut less.
TEST_CASE("a smaller radius yields a subset of the larger cut") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 8);

	std::vector<ve::LodDrawItem> near_cut, far_cut;
	t.shadow_cut(c, 409.4f, 0, &near_cut);
	t.shadow_cut(c, 1638.4f, 0, &far_cut);
	CHECK(near_cut.size() <= far_cut.size());
	// Everything in the near cut is genuinely inside the near radius.
	for (const ve::LodDrawItem &d : near_cut) {
		const float p[3] = {c.pos[0], c.pos[1], c.pos[2]};
		CHECK(ve::lod_chunk_distance(d.level, d.coord, p) <= 409.4f + 1e-3f);
	}
}

TEST_CASE("shadow_cut clears its output and refuses a non-positive radius") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	settle(&t, c, &occ, 8);

	std::vector<ve::LodDrawItem> cut{ve::LodDrawItem{0, ve::IVec3{9, 9, 9}, 0, 0}};
	t.shadow_cut(c, 0.0f, 0, &cut);
	CHECK(cut.empty());
	t.shadow_cut(c, -5.0f, 0, &cut);
	CHECK(cut.empty());
}
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
cd extension && scons test -j8 2>&1 | tail -20; cd ..
```

Expected: FAIL — compile error, `'class ve::LodTree' has no member named 'shadow_cut'`.

- [ ] **Step 4: Change the header**

In `extension/src/lod/lod_tree.h`, delete the `shadow_draws` member and its comment block from `struct LodWalkResult`, leaving:

```cpp
struct LodWalkResult {
	std::vector<LodDrawItem> draws;
	std::vector<LodBuildRequest> requests;
};
```

In the `public:` section of `class LodTree`, immediately after the `walk()` declaration, add:

```cpp
	// The SUN's cut, for one cascade: the same descend rule walk() uses, minus the frustum
	// test, bounded by `radius` and floored at `min_level`.
	//
	// A shadow map is not rendered from the camera, so terrain beside and behind it must
	// keep casting -- but it still has to be a CUT, one description of each piece of ground.
	// Feeding the map every RESIDENT page instead puts several LoD descriptions of the same
	// ground in one texel; they disagree by metres and whichever survives the depth test
	// then shadows open sunlit ground.
	//
	// `min_level` floors the descent where the cascade's shadow texel cannot hold the
	// detail anyway (ve::sun_cascades). That is still ONE description per piece of ground,
	// so the failure above cannot return; what it trades is a bounded disagreement between
	// this coarse surface and the camera's finer one, which the texel-relative bias spans.
	//
	// MUST be called after walk() for the same camera and frame: want_finer() and
	// children_ready() read last_cam_pos_, which walk() refreshes.
	void shadow_cut(const LodCamera &cam, float radius, int min_level,
			std::vector<LodDrawItem> *out) const;
```

In the `private:` section, replace the `shadow_visit` declaration with:

```cpp
	// The frustum-free twin of visit(): same descend rule, no state touched, no requests.
	void shadow_visit(int level, IVec3 c, const LodCamera &cam, float radius, int min_level,
			std::vector<LodDrawItem> *out) const;
```

- [ ] **Step 5: Change the implementation**

In `extension/src/lod/lod_tree.cpp`, replace the whole `LodTree::shadow_visit` function with:

```cpp
void LodTree::shadow_cut(const LodCamera &cam, float radius, int min_level,
		std::vector<LodDrawItem> *out) const {
	if (!out) return;
	out->clear();
	if (!(radius > 0.0f)) return;
	std::vector<IVec3> roots;
	lod_roots_in_radius(cam.pos, radius, &roots);
	for (const IVec3 &r : roots)
		shadow_visit(kLodLevels - 1, r, cam, radius, min_level, out);
}

void LodTree::shadow_visit(int level, IVec3 c, const LodCamera &cam, float radius,
		int min_level, std::vector<LodDrawItem> *out) const {
	if (lod_chunk_distance(level, c, cam.pos) > radius) return;
	const auto it = nodes_.find(key(level, c));
	if (it == nodes_.end()) return;
	const Node &n = it->second;
	// Not drawable, and nothing below a node we do not have: the parent already emitted.
	if (n.state != kLodReady) return;

	float lo[3], hi[3];
	lod_chunk_aabb(level, c, lo, hi);
	float ss_min[3], ss_max[3];
	const float area = lod_projected_area(cam, lo, hi, ss_min, ss_max);
	// level > min_level is the ONLY addition to the descend rule. Everything below the
	// cascade's texel is detail the map cannot store.
	if (level > min_level && want_finer(level, c, area) && children_ready(level, c)) {
		const IVec3 base = lod_child_base(c);
		for (int k = 0; k < 8; k++)
			shadow_visit(level - 1,
					{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)},
					cam, radius, min_level, out);
		return;
	}
	out->push_back(LodDrawItem{level, c, n.page_first, n.page_count});
}
```

In `LodTree::walk`, delete `out->shadow_draws.clear();` and delete the two-line shadow recursion with its comment block:

```cpp
	// The sun's cut, over the SAME resident tree the walk above just updated. It is a
	// separate recursion rather than an extra output of visit() because visit() stops at the
	// frustum and marks residency as it goes; this one must do neither.
	for (const IVec3 &r : roots) shadow_visit(kLodLevels - 1, r, cam, &out->shadow_draws);
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cd extension && scons test -j8 2>&1 | tail -10; cd ..
extension/build/tests/ve_tests --test-case="*shadow_cut*"
extension/build/tests/ve_tests --test-case="characterization*"
```

Expected: PASS. `src/lod/lod_system.cpp` is excluded from `pure_sources`, so the native build stays green even though its call site is not fixed yet — Task 5 fixes it and `./build.sh` is what catches it.

- [ ] **Step 7: Commit**

```bash
git add extension/src/lod/lod_tree.h extension/src/lod/lod_tree.cpp extension/tests/test_lod_tree.cpp
git commit -m "$(cat <<'EOF'
refactor: the shadow cut is an explicit per-cascade query

walk() no longer produces a cut as a side effect. shadow_cut() takes a
radius and a minimum level, so each cascade asks for its own -- and so a
cascade that will not rasterise this frame can be skipped entirely.

min_level floors the descent where the cascade's texel cannot hold the
detail. That is still one description per piece of ground, so the
two-surfaces-in-one-texel failure the old comment warns about cannot
return; what it trades is a bounded coarse/fine disagreement the
texel-relative bias already spans.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 3: SunShadowPass becomes three layers

One pass, one array texture, per-layer framebuffers and per-layer rebuild state. The pass stays the single place the fit is written down, which is what `VoxelWorld::sun_ortho()` exists to preserve.

**Files:**
- Modify: `extension/src/render/sun_shadow_pass.h`, `extension/src/render/sun_shadow_pass.cpp`
- Test: `tests/test_sun_shadow.gd` (existing suite, updated in Task 5 once hooks are cascade-aware)

**Interfaces:**
- Consumes: `ve::kSunCascades`, `ve::SunCascade`, `ve::sun_cascades()` (Task 1).
- Produces:
  - `bool SunShadowPass::needs_rebuild(int cascade, const ve::SunOrtho &ortho) const`
  - `bool SunShadowPass::build(RenderingDevice *, LodPool &, LodRasterPass &, int cascade, const ve::SunOrtho &, bool force)` — **note the new `cascade` parameter, fourth**
  - `const float *SunShadowPass::view_proj(int cascade) const`
  - `float SunShadowPass::texel_world(int cascade) const`, `float SunShadowPass::depth_range(int cascade) const`
  - `int SunShadowPass::rebuilds(int cascade) const`, `int SunShadowPass::last_pages(int cascade) const`
  - `void SunShadowPass::mark_dirty()` — unchanged name, now dirties every cascade
  - `RID SunShadowPass::map() const` — unchanged name, now a 3-layer array texture

- [ ] **Step 1: Change the header**

Replace `extension/src/render/sun_shadow_pass.h` in full:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "shade/sun_cascades.h"
#include "shade/sun_ortho.h"

namespace godot {

class LodPool;
class LodRasterPass;

// Three nested camera-centred shadow maps in one array texture. One pass rather than three,
// so the fit stays written down in one place -- the property VoxelWorld::sun_ortho() exists
// to hold: the debug facade reads the same function the render path does, so what the tests
// pin is what ships.
//
// Per-layer dirty state is what makes the far cascade affordable. Each cascade snaps its
// light-space origin to its OWN texel (ve::sun_ortho_sphere), so cascade 0 re-projects about
// every 0.4 m of camera travel and cascade 2 about every 3.9 m: the expensive one rebuilds
// rarely and the cheap one rebuilds often, with no new throttle invented.
class SunShadowPass {
public:
	static constexpr int kSize = 2048;
	static constexpr int kMinFrames = 12;
	static constexpr int kCascades = ve::kSunCascades;

	~SunShadowPass();
	bool initialize(RenderingDevice *rd);
	void teardown();
	// Dirties every cascade: a page appearing or leaving can change any of them.
	void mark_dirty();

	// Whether build() would do work for this cascade. Split out of build() so a caller can
	// skip producing the cascade's LoD cut too -- for cascade 2 that cut is the expensive
	// half, and it is skipped on most frames. MUST stay in agreement with build()'s own
	// early-out; both call the same private test rather than stating the rule twice.
	bool needs_rebuild(int cascade, const ve::SunOrtho &ortho) const;

	bool build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster, int cascade,
			const ve::SunOrtho &ortho, bool force);

	RID map() const { return map_; } // 2048 x 2048 x kCascades, D32_SFLOAT
	int cascade_count() const { return kCascades; }
	const float *view_proj(int cascade) const { return c_[clamp_index(cascade)].view_proj; }
	float texel_world(int cascade) const { return c_[clamp_index(cascade)].texel_world; }
	float depth_range(int cascade) const { return c_[clamp_index(cascade)].depth_range; }
	int rebuilds(int cascade) const { return c_[clamp_index(cascade)].rebuilds; }
	int last_pages(int cascade) const { return c_[clamp_index(cascade)].last_pages; }
	bool is_valid() const { return rd_ && map_.is_valid() && shader_.is_valid(); }

private:
	struct Cascade {
		RID slice;       // texture_create_shared_from_slice, one array layer
		RID framebuffer; // that slice as a depth attachment
		bool dirty = true;
		int frames_since = 0;
		int rebuilds = 0;
		int last_pages = 0;
		float view_proj[16] = {};
		float texel_world = 0.0f;
		float depth_range = 0.0f;
	};

	static int clamp_index(int cascade) {
		return cascade < 0 ? 0 : (cascade >= kCascades ? kCascades - 1 : cascade);
	}
	// THE rebuild rule, stated once. needs_rebuild() and build() both call it.
	bool should_rebuild(const Cascade &c, const ve::SunOrtho &ortho, bool force) const;
	bool ensure_pipeline(RenderingDevice *rd);
	bool ensure_uniform_set(RenderingDevice *rd, LodPool &pool);

	RenderingDevice *rd_ = nullptr;
	RID map_;
	RID shader_;
	RID pipeline_;
	RID uset_;
	RID key_quads_;
	RID key_page_chunk_;
	RID key_chunks_;
	Cascade c_[kCascades];
};

} // namespace godot
```

- [ ] **Step 2: Build to verify it fails**

```bash
./build.sh 2>&1 | tail -20
```

Expected: FAIL — `sun_shadow_pass.cpp` still references `map_` as a single texture, `dirty_`, `frames_since_`, `view_proj_`, and `build()` without a cascade argument.

- [ ] **Step 3: Change texture creation to an array**

In `extension/src/render/sun_shadow_pass.cpp`, inside `initialize()`, replace the texture-format block and the `map_` creation with:

```cpp
	Ref<RDTextureFormat> tf;
	tf.instantiate();
	tf->set_format(RenderingDevice::DATA_FORMAT_D32_SFLOAT);
	tf->set_width(kSize);
	tf->set_height(kSize);
	tf->set_array_layers(kCascades);
	tf->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
	tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> tv;
	tv.instantiate();
	map_ = rd_->texture_create(tf, tv, {});
	if (!map_.is_valid()) {
		UtilityFunctions::printerr("SunShadowPass: shadow map array creation failed");
		teardown();
		return false;
	}

	// One framebuffer per layer. A shared slice is what lets cascade 0's frequent rebuilds
	// be isolated from cascade 2's expensive one; a single atlased framebuffer would force
	// them to rebuild together and throw the amortization away.
	for (int i = 0; i < kCascades; i++) {
		c_[i].slice = rd_->texture_create_shared_from_slice(tv, map_, i, 0);
		if (!c_[i].slice.is_valid()) {
			UtilityFunctions::printerr("SunShadowPass: cascade slice ", i, " failed");
			teardown();
			return false;
		}
	}
```

- [ ] **Step 4: Move framebuffer creation into `ensure_pipeline` per cascade**

Replace `ensure_pipeline`'s framebuffer guard. The existing body creates one `framebuffer_` from `map_`; it becomes a loop over slices. Replace the function's opening lines (the `if (framebuffer_.is_valid() && pipeline_.is_valid()) return true;` guard and the `framebuffer_create` call) with:

```cpp
bool SunShadowPass::ensure_pipeline(RenderingDevice *rd) {
	if (c_[0].framebuffer.is_valid() && pipeline_.is_valid()) return true;
	for (int i = 0; i < kCascades; i++) {
		if (c_[i].framebuffer.is_valid()) rd->free_rid(c_[i].framebuffer);
		c_[i].framebuffer = rd->framebuffer_create(Array::make(c_[i].slice));
		if (!c_[i].framebuffer.is_valid()) return false;
	}
	const int64_t format = rd->framebuffer_get_format(c_[0].framebuffer);
```

Leave the rest of the function (pipeline creation from `format`) unchanged.

- [ ] **Step 5: Split the rebuild rule out and make `build` per-cascade**

Replace `SunShadowPass::build` in full, and add `should_rebuild` and `needs_rebuild` above it:

```cpp
// THE rebuild rule, in one place. The projection moving means what is stored no longer
// describes what will be sampled: rebuild now rather than at the throttle's convenience.
// kMinFrames exists to damp pages coming and going; it must not make a day/night sweep lag
// twelve frames behind the sun, nor leave a cascade a texel behind the camera it follows.
//
// Comparing the whole matrix is affordable precisely because sun_ortho_sphere() snaps: an
// unsnapped camera-following fit differs on every frame the camera moves at all, and this
// test would degenerate into a full 2048^2 pass over the whole cut, every frame.
bool SunShadowPass::should_rebuild(const Cascade &c, const ve::SunOrtho &ortho,
		bool force) const {
	if (!is_valid() || !ortho.valid) return false;
	const bool projection_moved = c.rebuilds > 0 &&
			std::memcmp(c.view_proj, ortho.view_proj, sizeof(c.view_proj)) != 0;
	if (force || projection_moved) return true;
	return c.dirty && c.frames_since >= kMinFrames;
}

bool SunShadowPass::needs_rebuild(int cascade, const ve::SunOrtho &ortho) const {
	return should_rebuild(c_[clamp_index(cascade)], ortho, false);
}

bool SunShadowPass::build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster,
		int cascade, const ve::SunOrtho &ortho, bool force) {
	Cascade &c = c_[clamp_index(cascade)];
	c.frames_since++;
	if (!should_rebuild(c, ortho, force)) return false;
	const std::vector<LodRasterPass::PageDraw> &pages = raster.draw_pages();
	if (pages.empty()) return false;
	if (!raster.prepare_index_array(rd, pool)) return false;
	if (!ensure_pipeline(rd) || !ensure_uniform_set(rd, pool)) return false;
	// The shadow pass and camera pass share this indirect-argument buffer. Upload the full
	// drawable set before recording either draw list; the camera cull must not erase it first.
	pool.upload_draw_args(pages);
	const int64_t dl = rd->draw_list_begin(c.framebuffer, RenderingDevice::DRAW_CLEAR_DEPTH,
			PackedColorArray(), 0.0f);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	rd->draw_list_bind_index_array(dl, raster.index_array());
	PackedByteArray pc;
	pc.resize(64);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	for (int i = 0; i < 16; i++) f[i] = ortho.view_proj[i];
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	rd->draw_list_draw_indirect(dl, true, pool.args_buffer(), 0,
			static_cast<int>(pages.size()), 20);
	rd->draw_list_end();
	std::memcpy(c.view_proj, ortho.view_proj, sizeof(c.view_proj));
	c.texel_world = ortho.texel_world;
	c.depth_range = ortho.depth_range;
	c.dirty = false;
	c.frames_since = 0;
	c.last_pages = static_cast<int>(pages.size());
	c.rebuilds++;
	return true;
}
```

- [ ] **Step 6: Update `mark_dirty` and `teardown`**

Replace `mark_dirty`:

```cpp
void SunShadowPass::mark_dirty() {
	// A page appearing or leaving can change any cascade, so all of them are dirtied. The
	// per-cascade throttle is what keeps that from costing three full rasters.
	for (int i = 0; i < kCascades; i++) {
		c_[i].dirty = true;
		c_[i].frames_since = 0;
	}
}
```

In `teardown()`, replace the RID-freeing loop so the per-cascade RIDs are released before the array they alias:

```cpp
	if (rd_) {
		for (int i = 0; i < kCascades; i++) {
			for (RID *r : {&c_[i].framebuffer, &c_[i].slice})
				if (r->is_valid()) rd_->free_rid(*r);
			c_[i] = Cascade{};
		}
		for (RID *r : {&uset_, &pipeline_, &shader_, &map_})
			if (r->is_valid()) rd_->free_rid(*r);
	}
	uset_ = RID();
	pipeline_ = RID();
	shader_ = RID();
	map_ = RID();
	key_quads_ = RID();
	key_page_chunk_ = RID();
	key_chunks_ = RID();
	rd_ = nullptr;
```

- [ ] **Step 7: Build to verify it compiles**

```bash
./build.sh 2>&1 | tail -20
```

Expected: `sun_shadow_pass.cpp` compiles. `raymarch_compositor.cpp`, `hooks.cpp` and `lod_system.cpp` will still FAIL — they call the old signatures. That is Task 5. If you want a green build before then, note the failures and continue; do not "fix" them by restoring the old API.

- [ ] **Step 8: Commit**

```bash
git add extension/src/render/sun_shadow_pass.h extension/src/render/sun_shadow_pass.cpp
git commit -m "$(cat <<'EOF'
feat: SunShadowPass renders three cascades into one array texture

Per-layer framebuffers via texture_create_shared_from_slice and
per-layer dirty state, so cascade 0's frequent rebuilds are isolated
from cascade 2's expensive one. A single atlased framebuffer would force
them to rebuild together and discard the texel-snap amortization.

The rebuild rule moves into should_rebuild() so needs_rebuild() and
build() cannot state it differently: a caller that skips the build must
be able to skip producing the cascade's LoD cut too.

Callers are updated in the compositor task; this commit does not build
the extension on its own.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 4: Cascade selection in the deferred pass

The shader samples an array and picks its layer by distance. Because the fits are camera-centred *spheres*, selection is a scalar compare — no depth-slice arithmetic, no split-plane seam. That is a direct dividend of sub-project A's sphere fit.

**Files:**
- Modify: `shaders/deferred.comp.glsl`, `extension/src/render/deferred_pass.h`, `extension/src/render/deferred_pass.cpp`

**Interfaces:**
- Consumes: `ve::kSunCascades` (Task 1).
- Produces: `DeferredPass::Params` gains `sun_view_proj[ve::kSunCascades][16]`, `shadow_texel[ve::kSunCascades]`, `shadow_depth_range_c[ve::kSunCascades]`, `cascade_split[ve::kSunCascades]`, `cascade_count`. `DeferredPass::render` loses its loose `sun_view_proj` and `shadow_texel` arguments and its signature becomes `render(rd, gb, materials, ssgi, ssao, sun_map, p)`.

- [ ] **Step 1: Change the shader's UBO and sampler**

In `shaders/deferred.comp.glsl`, replace the `sun_map` sampler declaration and the `SunBlock` block:

```glsl
layout(set = 0, binding = 4) uniform sampler2DArray sun_map;
```

```glsl
#define SUN_CASCADES 3
layout(set = 0, binding = 6, std140) uniform SunBlock {
	mat4 view_proj[SUN_CASCADES];
	// per cascade: x = one shadow texel in world metres, y = light-space depth range in the
	// same metres, zw unused
	vec4 params[SUN_CASCADES];
	// xyz = the cascade radii; w = the count actually in use (1 when the radius collapsed)
	vec4 splits;
} sun;
```

- [ ] **Step 2: Replace `sun_map_visibility` with the cascade version**

```glsl
// The fits are camera-centred SPHERES, so a point at distance d is inside cascade i exactly
// when d < radius_i. Selection is a scalar compare -- no depth-slice arithmetic and no
// split-plane seam to reconcile against the projection. That is what the sphere fit buys.
int sun_cascade_of(float d) {
	int n = int(sun.splits.w);
	for (int i = 0; i < SUN_CASCADES; i++) {
		if (i >= n) break;
		if (d < sun.splits[i]) return i;
	}
	return n - 1;
}

float sun_map_visibility(vec3 wpos, float ndl, float view_dist) {
	int c = sun_cascade_of(view_dist);
	vec4 clip = sun.view_proj[c] * vec4(wpos, 1.0);
	if (clip.w <= 0.0) return 1.0;
	vec3 p = clip.xyz / clip.w;
	vec2 uv = p.xy * 0.5 + 0.5;
	// Outside the outermost cascade's map there is no shadow information, and "lit" is the
	// honest answer -- this is also what makes "beyond the last cascade" need no branch.
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
	float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
	// p.z and the stored depth are normalized [0,1], so the bias must be too. Every term is
	// texel-relative and deliberately so: a cascade spans thousands of metres of depth
	// range, where an absolute bias contributes metres of slop and unseats the stored
	// surface from the ground it rasterized. Per cascade, because the texels differ by ~10x.
	float texel = sun.params[c].x / max(sun.params[c].y, 1e-6);
	float bias = texel * (1.5 + 2.0 * slope);
	return (p.z + bias >= texture(sun_map, vec3(uv, float(c))).r) ? 1.0 : 0.0;
}
```

- [ ] **Step 3: Update the three call sites in the shader**

The two probe arms (`pc.flags.y == 1u` and `pc.flags.y == 4u`) and the shading path all call `sun_map_visibility`. Each now passes the viewer distance:

```glsl
		float vis = ((pc.flags.x & BEAUTY_SUN_MAP) != 0u)
				? sun_map_visibility(pc.cam.xyz, 1.0,
						distance(pc.cam.xyz, pc.inv_view_proj[0].xyz)) : 1.0;
```

for `pc.flags.y == 1u`;

```glsl
		float vis = ((pc.flags.x & BEAUTY_SUN_MAP) != 0u &&
				far_field_owns(px, pc.cam.xyz, pc.inv_view_proj[0].xyz))
				? sun_map_visibility(pc.cam.xyz, 1.0,
						distance(pc.cam.xyz, pc.inv_view_proj[0].xyz)) : 1.0;
```

for `pc.flags.y == 4u`; and in the shading path, replace the `shadow = min(shadow, sun_map_visibility(wpos, ndl));` line with:

```glsl
		shadow = min(shadow, sun_map_visibility(wpos, ndl, distance(wpos, pc.cam.xyz)));
```

- [ ] **Step 4: Change the C++ Params and render signature**

In `extension/src/render/deferred_pass.h`, add the include and replace the `shadow_depth_range` member with the cascade arrays:

```cpp
#include "shade/sun_cascades.h"
```

```cpp
		// Per-cascade sun state. `cascade_count` is what ve::sun_cascades() returned: 1 when
		// the stream radius collapsed the set, which is exactly the pre-cascade behaviour.
		float sun_view_proj[ve::kSunCascades][16] = {};
		float shadow_texel[ve::kSunCascades] = {};
		// Light-space depth extent of each cascade's ortho, in world metres. Only read when
		// a sun map is bound; render() clears kFlagSunMap when it is not, so the default 0
		// is never divided by.
		float shadow_depth_range_c[ve::kSunCascades] = {};
		float cascade_split[ve::kSunCascades] = {};
		int cascade_count = 0;
```

and change the render declaration:

```cpp
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID ssgi, RID ssao, RID sun_map, const Params &p);
```

- [ ] **Step 5: Change the C++ UBO fill**

In `extension/src/render/deferred_pass.cpp`, change the UBO size from 80 to 256 at its creation:

```cpp
	sun_ubo_ = rd->uniform_buffer_create(256, zeros);
```

(also resize the `zeros` PackedByteArray feeding it to 256), and replace the `render()` signature and its sun-UBO fill:

```cpp
bool DeferredPass::render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
		RID ssgi, RID ssao, RID sun_map, const Params &p) {
```

```cpp
	// std140: mat4[3] = 192 B, then vec4[3] = 48 B, then vec4 splits = 16 B. 256 total.
	PackedByteArray ub;
	ub.resize(256);
	ub.fill(0);
	float *uf = reinterpret_cast<float *>(ub.ptrw());
	const int n = sun_map.is_valid() ? p.cascade_count : 0;
	for (int c = 0; c < ve::kSunCascades; c++)
		for (int i = 0; i < 16; i++)
			uf[c * 16 + i] = c < n ? p.sun_view_proj[c][i] : 0.0f;
	for (int c = 0; c < ve::kSunCascades; c++) {
		uf[48 + c * 4 + 0] = c < n ? p.shadow_texel[c] : 0.0f;
		uf[48 + c * 4 + 1] = c < n ? p.shadow_depth_range_c[c] : 0.0f;
	}
	for (int c = 0; c < ve::kSunCascades; c++)
		uf[60 + c] = c < n ? p.cascade_split[c] : 0.0f;
	uf[63] = float(n);
	rd->buffer_update(sun_ubo_, 0, 256, ub);
```

Delete the old `for (int i = 0; i < 16; i++) uf[i] = sun_map.is_valid() ? sun_view_proj[i] : 0.0f;` line and any remaining write of the scalar `shadow_texel` / `shadow_depth_range`.

- [ ] **Step 6: Build to verify it compiles**

```bash
./build.sh 2>&1 | tail -20
```

Expected: `deferred_pass.cpp` compiles. `raymarch_compositor.cpp` still FAILS on the old `render(...)` argument list — Task 5.

- [ ] **Step 7: Commit**

```bash
git add shaders/deferred.comp.glsl extension/src/render/deferred_pass.h extension/src/render/deferred_pass.cpp
git commit -m "$(cat <<'EOF'
feat: the deferred pass selects a shadow cascade by distance

The fits are camera-centred spheres, so a point at distance d is inside
cascade i exactly when d < radius_i: selection is a scalar compare with
no depth-slice arithmetic and no split-plane seam. That falls out of the
sphere fit sub-project A shipped.

The bias stays texel-relative and becomes per-cascade, which it has to
be -- the texels differ by roughly ten times across the set.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 5: Wire the cascade loop and make the debug hooks cascade-aware

This is the task that makes the extension build again and the first where the cascades are visible. It also restores the property the hooks exist for: the facade must read the *shipping* fit, not a second one that happens to agree.

**Files:**
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`, `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`, `extension/src/raymarch_compositor.cpp`, `extension/src/debug/hooks.cpp`
- Test: `tests/test_sun_shadow.gd`, `tests/test_sun_cascades_gpu.gd` (create)

**Interfaces:**
- Consumes: everything from Tasks 1-4.
- Produces:
  - `ve::SunOrtho VoxelWorld::sun_ortho(int cascade) const`
  - `int VoxelWorld::sun_cascade_count() const`
  - `bool VoxelWorld::get_sun_cascade_min_level() const` / `set_sun_cascade_min_level(bool)` — the A/B knob
  - `void LodSystem::prepare_shadow_raster(float radius, int min_level)`
  - `hooks().debug_sun_shadow_stats(int cascade)` — **now takes a cascade index**
  - `hooks().debug_sun_shadow_build(int cascade, bool force)` — **now takes a cascade index**

- [ ] **Step 1: Change `LodSystem::prepare_shadow_raster`**

In `extension/src/lod/lod_system.h`, replace the declaration:

```cpp
	// One cascade's shadow cut, pushed into the raster pass. Radius and min_level come from
	// ve::sun_cascades(); the caller skips this entirely for a cascade that will not
	// rebuild, which for cascade 2 is most frames.
	void prepare_shadow_raster(float radius, int min_level);
```

In `extension/src/lod/lod_system.cpp`, replace the function body (keeping the long comment above it, which is still exactly right):

```cpp
void LodSystem::prepare_shadow_raster(float radius, int min_level) {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	if (!render()->lod_raster_pass() || !lod_pool_ || !lod_tree_) return;
	std::vector<ve::LodDrawItem> cut;
	lod_tree_->shadow_cut(lod_shadow_cam_, radius, min_level, &cut);
	std::vector<ve::LodPageDraw> page_draws;
	ve::lod_collect_page_draws(cut, lod_pages_of_, lod_page_quads_, &page_draws);
	std::vector<LodRasterPass::PageDraw> pages;
	pages.reserve(page_draws.size());
	for (const ve::LodPageDraw &pd : page_draws)
		pages.push_back(LodRasterPass::PageDraw{pd.page, pd.quad_count});
	render()->lod_raster_pass()->set_draw_pages(pages);
}
```

Add the camera member to `lod_system.h`'s private section, beside `last_cam_`:

```cpp
	// The camera the last walk ran with. shadow_cut() needs the whole LodCamera (it
	// projects chunk AABBs), not just the position, and it must be the SAME camera the walk
	// used or the two cuts choose different levels for the same ground.
	ve::LodCamera lod_shadow_cam_;
```

and record it in `tick()`, immediately after the existing `for (int a = 0; a < 3; a++) last_cam_[a] = cam.pos[a];`:

```cpp
	lod_shadow_cam_ = cam;
```

- [ ] **Step 2: Change `VoxelWorld::sun_ortho` to take a cascade**

In `extension/src/voxel_world.h`, replace the `sun_ortho()` declaration with:

```cpp
	// The SHIPPING fit for one cascade. One place it is written down, read by both the
	// render path and the debug facade -- this hook used to centre its own box while the
	// compositor centred on the camera, which is how a shimmering shadow map passed a suite
	// containing "the matrix does not move with the camera".
	ve::SunOrtho sun_ortho(int cascade) const;
	int sun_cascade_count() const;
	void set_sun_cascade_min_level(bool v) { sun_cascade_min_level_ = v; }
	bool get_sun_cascade_min_level() const { return sun_cascade_min_level_; }
```

and add the member beside the other render flags:

```cpp
	// The A/B knob for the shadow cut's minimum-level clamp (spec section 3). On by
	// default; off restores an unclamped cut for measurement.
	bool sun_cascade_min_level_ = true;
```

In `extension/src/voxel_world.cpp`, replace `VoxelWorld::sun_ortho()`:

```cpp
int VoxelWorld::sun_cascade_count() const {
	ve::SunCascade c[ve::kSunCascades];
	return ve::sun_cascades(get_stream_radius_m(), SunShadowPass::kSize, c);
}

ve::SunOrtho VoxelWorld::sun_ortho(int cascade) const {
	float cam[3];
	if (!context_.lod->last_camera(cam)) return ve::SunOrtho();
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(get_stream_radius_m(), SunShadowPass::kSize, c);
	if (n <= 0 || cascade < 0 || cascade >= n) return ve::SunOrtho();
	const ve::SunState sun = sun_state();
	// A scene light hands over a basis that rotates continuously; a bare direction has to
	// have one derived, which is ill-conditioned near the zenith. Same choice as before.
	return sun.has_basis()
			? ve::sun_ortho_sphere(sun.dir, sun.right, sun.up, cam, c[cascade].radius,
					SunShadowPass::kSize)
			: ve::sun_ortho_sphere(sun.dir, cam, c[cascade].radius, SunShadowPass::kSize);
}
```

Add `#include "shade/sun_cascades.h"` to the includes.

Register the knob beside the other properties, in `_bind_methods`:

```cpp
	ClassDB::bind_method(D_METHOD("set_sun_cascade_min_level", "v"),
			&VoxelWorld::set_sun_cascade_min_level);
	ClassDB::bind_method(D_METHOD("get_sun_cascade_min_level"),
			&VoxelWorld::get_sun_cascade_min_level);
```

and with the other `ADD_PROPERTY` calls:

```cpp
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sun_cascade_min_level"),
			"set_sun_cascade_min_level", "get_sun_cascade_min_level");
```

- [ ] **Step 3: Replace the compositor's `build_sun_shadow` lambda with the cascade loop**

In `extension/src/raymarch_compositor.cpp`, add `#include "shade/sun_cascades.h"` and replace the `build_sun_shadow` lambda:

```cpp
		ve::SunCascade cascades[ve::kSunCascades];
		const int cascade_count = ve::sun_cascades(world->get_stream_radius_m(),
				SunShadowPass::kSize, cascades);
		const bool clamp_levels = world->get_sun_cascade_min_level();
		auto build_sun_shadow = [&]() {
			if (!use_sun_shadow) return;
			timings->begin(rd, "sun_shadow");
			for (int i = 0; i < cascade_count; i++) {
				const ve::SunOrtho ortho = world->sun_ortho(i);
				// Ask BEFORE producing the cut: for cascade 2 the cut is the expensive
				// half, and it is skipped on most frames because a 3.9 m texel only
				// re-snaps every 3.9 m of travel.
				if (!sun->needs_rebuild(i, ortho)) continue;
				world->prepare_lod_shadow_raster(cascades[i].radius,
						clamp_levels ? cascades[i].min_level : 0);
				sun->build(rd, *world->lod_pool(), *lod_raster, i, ortho, false);
			}
			timings->end(rd, "sun_shadow");
			world->prepare_lod_raster();
		};
```

Update `VoxelWorld::prepare_lod_shadow_raster` in `voxel_world.h`/`.cpp` to forward the two new arguments:

```cpp
void VoxelWorld::prepare_lod_shadow_raster(float radius, int min_level) {
	context_.lod->prepare_shadow_raster(radius, min_level);
}
```

- [ ] **Step 4: Fill the deferred Params with cascade state**

In `extension/src/raymarch_compositor.cpp`, replace the `dp.shadow_depth_range = ...` line and the `deferred->render(...)` call's sun arguments:

```cpp
	// The old `static const float kNoSun[16]` fallback goes with the single-map path: the
	// UBO fill zeroes every cascade past `cascade_count` itself.
	const bool use_sun = sun && sun->is_valid() && sun->rebuilds(0) > 0 &&
			(beauty_flags & ve::kFlagSunMap) != 0u;
	dp.cascade_count = use_sun ? cascade_count : 0;
	for (int i = 0; i < cascade_count && use_sun; i++) {
		const float *vp = sun->view_proj(i);
		for (int k = 0; k < 16; k++) dp.sun_view_proj[i][k] = vp[k];
		dp.shadow_texel[i] = sun->texel_world(i);
		dp.shadow_depth_range_c[i] = sun->depth_range(i);
		dp.cascade_split[i] = cascades[i].radius;
	}
```

and change the render call to drop the two loose arguments:

```cpp
	const bool deferred_ok = deferred->render(rd, *gb, *materials,
			ssgi_ok ? ssgi->result() : RID(), ssao_ok ? ssao->result() : RID(),
			use_sun ? sun->map() : RID(), dp);
```

(Match the surrounding call's existing argument spelling for `ssgi`/`ssao`/`sun_map`; only the trailing `sun_view_proj` and `shadow_texel` arguments are removed.)

- [ ] **Step 5: Make the debug hooks cascade-aware**

In `extension/src/debug/hooks.cpp`, change the two bindings to take a cascade index:

```cpp
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_stats", "cascade"),
			&VoxelDebugHooks::debug_sun_shadow_stats);
	ClassDB::bind_method(D_METHOD("debug_sun_shadow_build", "cascade", "force"),
			&VoxelDebugHooks::debug_sun_shadow_build);
```

and rewrite `debug_sun_shadow_stats`:

```cpp
Dictionary VoxelDebugHooks::debug_sun_shadow_stats(int cascade) {
	Dictionary d;
	d["size"] = SunShadowPass::kSize;
	d["cascades"] = 0;
	d["cascade"] = cascade;
	d["radius"] = 0.0f;
	d["min_level"] = 0;
	d["map_valid"] = false;
	d["ortho_valid"] = false;
	d["texel_world"] = 0.0f;
	d["rebuilds"] = 0;
	d["pages"] = 0;
	d["view_proj"] = PackedFloat32Array();
	world_->ensure_initialized();
	SunShadowPass *sun = world_->sun_shadow_pass();
	if (!sun) return d;
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(world_->get_stream_radius_m(), SunShadowPass::kSize, c);
	d["cascades"] = n;
	if (cascade < 0 || cascade >= n) return d;
	d["radius"] = c[cascade].radius;
	d["min_level"] = c[cascade].min_level;
	// The SHIPPING fit, not a second one that happens to agree.
	const ve::SunOrtho ortho = world_->sun_ortho(cascade);
	d["map_valid"] = sun->map().is_valid();
	d["ortho_valid"] = ortho.valid;
	d["texel_world"] = ortho.valid ? ortho.texel_world : sun->texel_world(cascade);
	d["rebuilds"] = sun->rebuilds(cascade);
	d["pages"] = sun->last_pages(cascade);
	// Exposed so a test can assert the query and the build agree. They share
	// should_rebuild(), so agreement is structural -- but a cut skipped on a false negative
	// is a shadow that silently stops updating, which is worth pinning.
	d["needs_rebuild"] = sun->needs_rebuild(cascade, ortho);
	PackedFloat32Array matrix;
	matrix.resize(16);
	const float *source = sun->rebuilds(cascade) > 0 ? sun->view_proj(cascade) :
			(ortho.valid ? ortho.view_proj : sun->view_proj(cascade));
	for (int i = 0; i < 16; i++) matrix.set(i, source[i]);
	d["view_proj"] = matrix;
	return d;
}
```

Update the declarations in the hooks header to match (`Dictionary debug_sun_shadow_stats(int cascade);`, `bool debug_sun_shadow_build(int cascade, bool force);`), and update `debug_sun_shadow_build` to pass its cascade through to `sun->build(..., cascade, world_->sun_ortho(cascade), force)` and to call `world_->prepare_lod_shadow_raster(c[cascade].radius, c[cascade].min_level)` before it. Fix every other `world_->sun_ortho()` call in this file to `world_->sun_ortho(0)` unless the surrounding diagnostic is clearly about the outermost map, in which case use `sun_cascade_count() - 1`.

- [ ] **Step 6: Build and fix the remaining call sites**

```bash
./build.sh 2>&1 | tail -30
```

Expected: PASS. If any call site still passes the old argument list, fix it to the new one — do not add a compatibility overload.

- [ ] **Step 7: Update the existing sun shadow suite for the new hook arity**

In `tests/test_sun_shadow.gd`, every `hooks().debug_sun_shadow_stats()` becomes `hooks().debug_sun_shadow_stats(0)` and every `hooks().debug_sun_shadow_build(x)` becomes `hooks().debug_sun_shadow_build(0, x)`. Cascade 0 is the near map, which is what these tests were always measuring at their 1400 m stream radius.

```bash
./gdunit_tests.sh -a res://tests/test_sun_shadow.gd 2>&1 | tail -20
```

Expected: PASS, same case count as the baseline.

- [ ] **Step 8: Write the new cascade GPU tests**

Create `tests/test_sun_cascades_gpu.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(radius: float) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = radius
	w.max_lod_pages = 32768
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func test_three_cascades_are_reported_with_derived_radii() -> void:
	var w := make_world(4000.0)
	w.hooks().debug_lod_tick(Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())
	var c0: Dictionary = w.hooks().debug_sun_shadow_stats(0)
	var c1: Dictionary = w.hooks().debug_sun_shadow_stats(1)
	var c2: Dictionary = w.hooks().debug_sun_shadow_stats(2)
	assert_int(c0["cascades"]).is_equal(3)
	assert_float(c0["radius"]).is_equal_approx(409.4, 0.01)
	assert_float(c1["radius"]).is_equal_approx(1279.687, 0.01)
	assert_float(c2["radius"]).is_equal_approx(4000.0, 0.01)
	# The clamp: cascade 0 unclamped by construction, the others floored at their texel.
	assert_int(c0["min_level"]).is_equal(0)
	assert_int(c1["min_level"]).is_equal(1)
	assert_int(c2["min_level"]).is_equal(3)

# THE no-regression property, checked through the shipping path rather than the pure
# function: whatever the radius, the outermost cascade is the map that shipped before.
func test_the_outermost_cascade_reproduces_the_old_single_map() -> void:
	var w := make_world(1638.4)
	w.hooks().debug_lod_tick(Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())
	var last: Dictionary = w.hooks().debug_sun_shadow_stats(2)
	assert_float(last["radius"]).is_equal_approx(1638.4, 0.001)
	assert_float(last["texel_world"]).is_equal_approx(1.600782, 0.0001)

# Each cascade snaps to its OWN texel, so this is A's shimmer guarantee asserted three
# times: sub-texel motion must not re-project any of them.
func test_sub_texel_motion_rebuilds_no_cascade(timeout := 120000) -> void:
	var w := make_world(4000.0)
	var fwd := Vector3(1, -0.3, 1).normalized()
	var origin := Vector3(60.0, 80.0, 60.0)
	w.hooks().debug_lod_tick(origin, fwd)
	for c in range(3):
		w.hooks().debug_sun_shadow_build(c, true)
	var before := []
	for c in range(3):
		before.append(int(w.hooks().debug_sun_shadow_stats(c)["rebuilds"]))
	# One tenth of cascade 0's texel -- far inside every cascade's snap grid.
	var texel0: float = w.hooks().debug_sun_shadow_stats(0)["texel_world"]
	for i in range(6):
		w.hooks().debug_lod_tick(origin + Vector3(0.1 * texel0 * i, 0.0, 0.0), fwd)
		for c in range(3):
			w.hooks().debug_sun_shadow_build(c, false)
	for c in range(3):
		var now := int(w.hooks().debug_sun_shadow_stats(c)["rebuilds"])
		assert_int(now).override_failure_message(
			"cascade %d rebuilt %d times under sub-texel motion" % [c, now - before[c]]
		).is_less_equal(before[c] + 1)

# The clamp must not lift the far field's shadowed ground off the ground (peter-panning).
# Sampled where cascade 2 owns the pixel, with the clamp on and off.
func test_the_min_level_clamp_does_not_peter_pan(timeout := 180000) -> void:
	var probe := Vector3(1800.0, 20.0, 1800.0)
	var w_on := make_world(4000.0)
	w_on.sun_cascade_min_level = true
	w_on.hooks().debug_lod_tick(Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())
	for c in range(3):
		w_on.hooks().debug_sun_shadow_build(c, true)
	var on: float = w_on.hooks().debug_sun_shadow_visibility(probe)

	var w_off := make_world(4000.0)
	w_off.sun_cascade_min_level = false
	w_off.hooks().debug_lod_tick(Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())
	for c in range(3):
		w_off.hooks().debug_sun_shadow_build(c, true)
	var off: float = w_off.hooks().debug_sun_shadow_visibility(probe)

	# Deep underground is shadowed either way. If the clamp lifted the stored surface off
	# the ground, the clamped map would report this point LIT.
	assert_float(on).override_failure_message(
		"clamped cascade reports %f, unclamped reports %f: the clamp peter-panned" % [on, off]
	).is_equal_approx(off, 0.01)

# needs_rebuild() gates whether the cascade's cut is even produced, so a false negative is
# a shadow map that silently stops updating. It shares should_rebuild() with build(), which
# makes agreement structural; this pins it anyway.
func test_needs_rebuild_agrees_with_what_build_does(timeout := 120000) -> void:
	var w := make_world(4000.0)
	var fwd := Vector3(1, -0.3, 1).normalized()
	w.hooks().debug_lod_tick(Vector3(60, 80, 60), fwd)
	for c in range(3):
		w.hooks().debug_sun_shadow_build(c, true)
	# Nothing has moved and nothing is dirty, so no cascade should want a rebuild -- and an
	# unforced build must then decline for exactly the cascades that said so.
	for c in range(3):
		var wants: bool = w.hooks().debug_sun_shadow_stats(c)["needs_rebuild"]
		var did: bool = w.hooks().debug_sun_shadow_build(c, false)
		assert_bool(did).override_failure_message(
			"cascade %d: needs_rebuild=%s but build returned %s" % [c, wants, did]
		).is_equal(wants)

# The clamp is a saving. If it is on and costs the same, it is not doing anything.
func test_the_clamp_reduces_the_far_cascade_page_count(timeout := 180000) -> void:
	var w_on := make_world(4000.0)
	w_on.sun_cascade_min_level = true
	w_on.hooks().debug_lod_tick(Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())
	w_on.hooks().debug_sun_shadow_build(2, true)
	var on: int = w_on.hooks().debug_sun_shadow_stats(2)["pages"]

	var w_off := make_world(4000.0)
	w_off.sun_cascade_min_level = false
	w_off.hooks().debug_lod_tick(Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())
	w_off.hooks().debug_sun_shadow_build(2, true)
	var off: int = w_off.hooks().debug_sun_shadow_stats(2)["pages"]

	assert_int(on).override_failure_message(
		"clamped cascade 2 drew %d pages, unclamped %d" % [on, off]).is_less_equal(off)
```

- [ ] **Step 9: Run the new suite**

```bash
./gdunit_tests.sh -a res://tests/test_sun_cascades_gpu.gd 2>&1 | tail -25
```

Expected: PASS. If `test_the_min_level_clamp_does_not_peter_pan` fails, the spec §3 argument is wrong — report it rather than loosening the tolerance; the correct response is to default `sun_cascade_min_level` to false and record the measurement.

- [ ] **Step 10: Run the whole gdUnit suite against the Task 0 baseline**

```bash
./gdunit_tests.sh 2>&1 | tee /tmp/gd-task5.txt | tail -5
diff <(grep -c "FAILED" reports/4km-baseline/gdunit.txt) <(grep -c "FAILED" /tmp/gd-task5.txt)
```

Expected: the same 5 baseline assertion failures and no new ones.

- [ ] **Step 11: Commit**

```bash
git add extension/src/voxel_world.h extension/src/voxel_world.cpp \
        extension/src/lod/lod_system.h extension/src/lod/lod_system.cpp \
        extension/src/raymarch_compositor.cpp extension/src/debug/hooks.cpp \
        tests/test_sun_shadow.gd tests/test_sun_cascades_gpu.gd
git commit -m "$(cat <<'EOF'
feat: render and sample three sun shadow cascades

The compositor asks needs_rebuild() before producing a cascade's cut, so
cascade 2 -- whose cut is the expensive half -- is skipped on the frames
its 3.9 m texel has not re-snapped.

sun_ortho() takes a cascade and stays the one place the fit is written
down; the debug facade reads the same function, which is what stops the
hooks inspecting a matrix that is not the one rasterized.

sun_cascade_min_level is the A/B knob for the cut's level clamp.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 6: Make the chunk-record budget a parameter and diagnosable

`kChunkRecords` is very likely already large enough (spec §1). The point of this task is not tuning: it is that a private compile-time constant cannot be driven small, so its exhaustion path has never executed.

**Files:**
- Modify: `extension/src/render/lod_pool.h`, `extension/src/render/lod_pool.cpp`, `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`, `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`, `extension/src/debug/hooks.cpp`
- Test: `tests/test_lod_budget.gd` (create)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `bool LodPool::initialize(RenderingDevice *rd, int max_pages, int max_chunk_records)`
  - `int LodPool::chunk_records_used() const`, `int LodPool::chunk_records_high_water() const`, `int LodPool::pages_high_water() const`
  - `LodSystem::set_max_lod_chunk_records(int)` / `max_lod_chunk_records()`
  - `VoxelWorld` property `max_lod_chunk_records`, default 8192
  - `debug_lod_stats()` gains `chunk_records`, `chunk_records_used`, `chunk_records_high_water`, `pages_high_water`, `budget_bound`

- [ ] **Step 1: Write the failing test**

Create `tests/test_lod_budget.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(records: int) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1400.0
	w.max_lod_pages = 32768
	w.max_lod_chunk_records = records
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func settle(w: VoxelWorld, frames: int) -> void:
	var fwd := Vector3(1, -0.3, 1).normalized()
	for i in range(frames):
		w.hooks().debug_lod_tick(Vector3(60, 80, 60), fwd)

func test_the_budget_is_reported_and_defaults_to_8192() -> void:
	var w := make_world(8192)
	settle(w, 40)
	var d: Dictionary = w.hooks().debug_lod_stats()
	assert_int(d["chunk_records"]).is_equal(8192)
	assert_int(d["chunk_records_used"]).is_greater(0)
	assert_int(d["chunk_records_high_water"]).is_greater_equal(int(d["chunk_records_used"]))

# The horizon stops arriving; the ground does not disappear. Same contract the brick atlas
# states. This is the path that has never executed while kChunkRecords was private.
func test_exhausting_the_records_stops_the_horizon_without_corrupting_it(timeout := 120000) -> void:
	var starved := make_world(16)
	settle(starved, 60)
	var d: Dictionary = starved.hooks().debug_lod_stats()
	# Never over budget, and the budget is named as the thing that bound.
	assert_int(d["chunk_records_used"]).is_less_equal(16)
	assert_str(str(d["budget_bound"])).is_equal("chunk_records")
	# Whatever DID land is still coherent: every drawn page belongs to a live chunk.
	assert_int(d["draw_pages"]).is_greater_equal(0)
	assert_int(d["chunks_resident"]).is_less_equal(16)

func test_a_funded_world_reports_no_bound_budget() -> void:
	var w := make_world(8192)
	settle(w, 40)
	assert_str(str(w.hooks().debug_lod_stats()["budget_bound"])).is_equal("none")
```

- [ ] **Step 2: Run to verify it fails**

```bash
./gdunit_tests.sh -a res://tests/test_lod_budget.gd 2>&1 | tail -20
```

Expected: FAIL — `Invalid assignment of property 'max_lod_chunk_records'`.

- [ ] **Step 3: Parameterise `LodPool`**

In `extension/src/render/lod_pool.h`, change the signature and add the diagnostics, replacing the `kChunkRecords` constant with a member:

```cpp
	// max_chunk_records was a private compile-time 8192. It is a parameter so a test can
	// drive it small: the exhaustion path below is unreachable otherwise, and an untested
	// degradation path is a cliff waiting for a bigger world.
	bool initialize(RenderingDevice *rd, int max_pages, int max_chunk_records);
```

```cpp
	int chunk_record_count() const { return max_chunk_records_; }
	int chunk_records_used() const {
		return max_chunk_records_ - static_cast<int>(free_chunk_slots_.size());
	}
	int chunk_records_high_water() const { return chunk_records_high_water_; }
	int pages_high_water() const { return pages_high_water_; }
	// Which pool refused the last upload: "none", "chunk_records" or "pages".
	const char *budget_bound() const { return budget_bound_; }
```

and in the private section replace `static constexpr int kChunkRecords = 8192;` with:

```cpp
	int max_chunk_records_ = 0;
	int chunk_records_high_water_ = 0;
	int pages_high_water_ = 0;
	const char *budget_bound_ = "none";
	bool warned_records_ = false;
	bool warned_pages_ = false;
```

- [ ] **Step 4: Implement in `lod_pool.cpp`**

Change the signature and replace every `kChunkRecords` with `max_chunk_records_`, assigning it first:

```cpp
bool LodPool::initialize(RenderingDevice *rd, int max_pages, int max_chunk_records) {
	// ... existing early-outs ...
	max_chunk_records_ = max_chunk_records > 0 ? max_chunk_records : 8192;
```

In `upload()`, replace the two refusal returns so each names the pool that bound and warns once:

```cpp
	if (pages_needed > arena_.free_pages()) {
		budget_bound_ = "pages";
		if (!warned_pages_) {
			warned_pages_ = true;
			// Once per run: the horizon stops arriving instead of the ground disappearing,
			// which is deliberate -- but it must be diagnosable, not silent. Both pools
			// used to take this same path with nothing to tell them apart.
			UtilityFunctions::push_warning(
					"LodPool: page arena exhausted (", arena_.capacity(),
					" pages); the horizon will stop growing. Raise max_lod_pages.");
		}
		return false;
	}

	const int chunk_slot = allocate_chunk_slot();
	if (chunk_slot < 0) {
		budget_bound_ = "chunk_records";
		if (!warned_records_) {
			warned_records_ = true;
			UtilityFunctions::push_warning(
					"LodPool: chunk records exhausted (", max_chunk_records_,
					"); the horizon will stop growing. Raise max_lod_chunk_records.");
		}
		return false;
	}
```

and at the end of a successful `upload()`, just before `return true;`:

```cpp
	budget_bound_ = "none";
	chunk_records_high_water_ = std::max(chunk_records_high_water_, chunk_records_used());
	pages_high_water_ = std::max(pages_high_water_, arena_.used_pages());
```

Add `#include <algorithm>` and `#include <godot_cpp/variant/utility_functions.hpp>` if not present.

- [ ] **Step 5: Plumb the budget through `LodSystem` and `VoxelWorld`**

In `extension/src/lod/lod_system.h`, beside `max_lod_pages_`:

```cpp
	void set_max_lod_chunk_records(int v) { max_lod_chunk_records_ = v; }
	int max_lod_chunk_records() const { return max_lod_chunk_records_; }
```

```cpp
	int max_lod_chunk_records_ = 8192;
```

In `lod_system.cpp`'s `ensure_lod()`:

```cpp
	if (lod_pool_->page_count() == 0 &&
			!lod_pool_->initialize(device, max_lod_pages_, max_lod_chunk_records_))
		UtilityFunctions::printerr("VoxelWorld: LodPool initialize failed");
```

In `voxel_world.h`, beside the `max_lod_pages` accessors:

```cpp
	void set_max_lod_chunk_records(int v) { lod_->set_max_lod_chunk_records(v); }
	int get_max_lod_chunk_records() const { return lod_->max_lod_chunk_records(); }
```

and in `voxel_world.cpp`'s `_bind_methods`:

```cpp
	ClassDB::bind_method(D_METHOD("set_max_lod_chunk_records", "v"),
			&VoxelWorld::set_max_lod_chunk_records);
	ClassDB::bind_method(D_METHOD("get_max_lod_chunk_records"),
			&VoxelWorld::get_max_lod_chunk_records);
```

```cpp
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_lod_chunk_records"),
			"set_max_lod_chunk_records", "get_max_lod_chunk_records");
```

- [ ] **Step 6: Surface the diagnostics in `debug_lod_stats`**

In `extension/src/debug/hooks.cpp`, beside the existing `d["pages_used"]` / `d["chunks_resident"]` lines (around line 1252):

```cpp
	LodPool *pool = world_->context().lod->lod_pool_;
	d["chunk_records"] = pool ? pool->chunk_record_count() : 0;
	d["chunk_records_used"] = pool ? pool->chunk_records_used() : 0;
	d["chunk_records_high_water"] = pool ? pool->chunk_records_high_water() : 0;
	d["pages_high_water"] = pool ? pool->pages_high_water() : 0;
	d["budget_bound"] = pool ? String(pool->budget_bound()) : String("none");
```

- [ ] **Step 7: Build and run the test**

```bash
./build.sh 2>&1 | tail -10
./gdunit_tests.sh -a res://tests/test_lod_budget.gd 2>&1 | tail -20
```

Expected: PASS, 3 cases.

- [ ] **Step 8: Commit**

```bash
git add extension/src/render/lod_pool.h extension/src/render/lod_pool.cpp \
        extension/src/lod/lod_system.h extension/src/lod/lod_system.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp \
        extension/src/debug/hooks.cpp tests/test_lod_budget.gd
git commit -m "$(cat <<'EOF'
feat: the LoD chunk-record budget is a parameter and names what bound

kChunkRecords was a private compile-time 8192, so its exhaustion path
could not be driven by a test and had never executed. It is now
max_lod_chunk_records, and a test starves it to 16 to prove the
documented contract: the horizon stops arriving, the ground does not
disappear.

Both pools used to refuse an upload through the same silent path. Each
now names itself once per run, so a budget cliff is distinguishable from
a slow build queue.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 7: Horizon-fill measurement and the duplicated build cap

Two small, independent things that both bear on throughput: a metric that says how long the far field takes to arrive, and a literal that silently defeats the knob meant to speed it up.

**Files:**
- Modify: `extension/src/lod/lod_system.cpp`, `extension/src/render/mesh_service.h`, `extension/src/debug/hooks.cpp`, `demo/benchmark.gd`

**Interfaces:**
- Consumes: `debug_lod_stats()` from Task 6.
- Produces: `MeshService::lod_max_jobs() const`; `debug_lod_stats()` gains `lod_pending`; benchmark emits `BENCH lod_pending` and `BENCH horizon frames_to_horizon=N capped=<bool>`.

- [ ] **Step 1: Fix the duplicated cap**

In `extension/src/render/mesh_service.h`, expose the mesher's real cap beside `lod_busy()`:

```cpp
	// The LodBuildPass's per-batch job cap. LodSystem must clamp its batch to THIS, not to
	// a literal: a copy of the number means raising max_jobs silently changes nothing.
	int lod_max_jobs() const;
```

In `extension/src/render/mesh_service.cpp`, implement it by returning the configured value used when the pass was initialised (the `LodBuildConfig::max_jobs` the service holds; if the service stores the config, return `lod_cfg_.max_jobs`, otherwise add an `int lod_max_jobs_ = 8;` member set at initialise time and return it).

In `extension/src/lod/lod_system.cpp`, replace the clamp:

```cpp
		// The batch cap is the MESHER's, read from it rather than copied: a literal here
		// silently defeats any change to LodBuildConfig::max_jobs.
		const int take = std::min<int>({lod_builds_per_frame_,
				int(lod_walk_.requests.size()), mesh()->lod_max_jobs()});
```

- [ ] **Step 2: Expose the pending request count**

In `extension/src/debug/hooks.cpp`, beside the other LoD stats:

```cpp
	d["lod_pending"] = static_cast<int>(world_->context().lod->lod_walk_.requests.size());
```

- [ ] **Step 3: Add the horizon metric to the benchmark**

In `demo/benchmark.gd`, beside the existing `_settle` members:

```gdscript
const HORIZON_QUIET_FRAMES := 30
var _horizon_quiet := 0
var _horizon_at := -1
```

In the per-frame block that already reads the LoD stats dictionary, add:

```gdscript
	# The far field's own settle. The existing `settle` counts physics chunks_pending only
	# and says nothing about whether the horizon has arrived.
	if _horizon_at < 0:
		var pending: int = int(_lod.get("lod_pending", 0))
		_horizon_quiet = _horizon_quiet + 1 if pending == 0 else 0
		if _horizon_quiet >= HORIZON_QUIET_FRAMES:
			_horizon_at = _frames
```

and in `_report()`, beside the existing `BENCH settle` line:

```gdscript
	print("BENCH horizon frames_to_horizon=%d capped=%s" % [
		_horizon_at if _horizon_at >= 0 else _frames,
		str(_horizon_at < 0).to_lower()])
	print("BENCH lod_pending p50=%d p99=%d" % [
		_pct(_lod_pending_samples, 50), _pct(_lod_pending_samples, 99)])
```

collecting `_lod_pending_samples.append(int(_lod.get("lod_pending", 0)))` in the same per-frame block, using the file's existing percentile helper (`_pct`) and matching how `draw_pages` is already sampled.

- [ ] **Step 4: Build and measure**

```bash
./build.sh 2>&1 | tail -5
godot --path . --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark 2>&1 | grep -E "BENCH horizon|BENCH lod_pending|BENCH settle"
```

Expected: a `BENCH horizon` line with a real frame count. Record it — this is the 1638.4 m reference that Task 9's 4 km run is compared against.

- [ ] **Step 5: Decide on throughput, with the number in hand**

If `frames_to_horizon` at the 4 km default (measured in Task 9) is under ~1200 frames (~20 s), leave the batch machinery alone and record the measurement. If it is worse, raise `LodBuildConfig::max_jobs` from 8 to 16 in `lod_build_pass.h` and re-measure; only if that is still insufficient, allow a second batch in flight in `MeshService::submit_lod`. **Do not** take either change without the measurement — each adds concurrency to a path that currently has none.

- [ ] **Step 6: Commit**

```bash
git add extension/src/render/mesh_service.h extension/src/render/mesh_service.cpp \
        extension/src/lod/lod_system.cpp extension/src/debug/hooks.cpp demo/benchmark.gd
git commit -m "$(cat <<'EOF'
feat: measure horizon fill, and stop a literal defeating the build cap

lod_system.cpp clamped its batch with a literal 8 that duplicated
LodBuildConfig::max_jobs, so raising max_jobs would have silently
changed nothing. It reads the mesher's cap now.

frames_to_horizon counts frames until the LoD request queue drains and
stays drained; the existing settle metric counts physics chunks only and
says nothing about whether the far field has arrived.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 8: Terrain relief

Without this the 1638→4000 m annulus is a flat sliver on the horizon line and nothing about the radius is observable. The Lipschitz bound is the real constraint and it is satisfiable — but only with long wavelengths, so the budget arithmetic is part of the task, not a footnote.

**Files:**
- Create: `shaders/stages/relief.field.glslh`, `assets/pipelines/golden.pipeline`
- Modify: `extension/src/terrain/builtin_stages.cpp`, `assets/pipelines/default.pipeline`, `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`, `tests/test_field_baseline_gpu.gd`
- Test: `extension/tests/test_relief_lipschitz.cpp` (create)

**Interfaces:**
- Consumes: the stage manifest format (`//!in` / `//!out` / `//!param` / `//!lipschitz` / `//!cpu`), and the slot convention that `s.extra[]` holds this stage's **writes** in declaration order, then its reads.
- Produces: `ve::stage_relief` registered as `"ve::stage_relief"`; `VoxelWorld` property `terrain_pipeline_path` defaulting to `res://assets/pipelines/default.pipeline`.

- [ ] **Step 1: Write the failing Lipschitz test**

Create `extension/tests/test_relief_lipschitz.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>

// The relief stage's parameters, mirrored here so the bound is checked against the numbers
// that ship rather than against a declared constant.
//
// The pipeline declares lipschitz 2.0, and understating that bound is a CORRECTNESS bug:
// raycast.cpp steps by 1/lipschitz() and would overshoot a surface. Overstating it costs
// raycast steps and widens mesh_chunk.cpp's conservative padding. So the relief amplitudes
// are budgeted against it rather than chosen for looks and hoped for.
//
//   hills d/dx: 6(0.11) + 3(0.031) + 1(0.23)  = 0.983
//   hills d/dz: 6(0.13) + 3(0.043) + 1(0.19)  = 1.099
//   |grad(y - h)| = sqrt(1 + |grad h|^2), so |grad h| must stay under sqrt(3) = 1.732.
namespace {
constexpr float kReliefAmpA = 250.0f;
constexpr float kReliefFreqA = 0.0004f;
constexpr float kReliefAmpB = 60.0f;
constexpr float kReliefFreqB = 0.00083333f;

constexpr float kHillsDx = 6.0f * 0.11f + 3.0f * 0.031f + 1.0f * 0.23f;
constexpr float kHillsDz = 6.0f * 0.13f + 3.0f * 0.043f + 1.0f * 0.19f;
} // namespace

TEST_CASE("the relief stage fits inside the pipeline's declared Lipschitz bound") {
	const float relief_per_axis = kReliefAmpA * kReliefFreqA + kReliefAmpB * kReliefFreqB;
	CHECK(relief_per_axis == doctest::Approx(0.15f).epsilon(1e-3));

	const float gx = kHillsDx + relief_per_axis;
	const float gz = kHillsDz + relief_per_axis;
	const float bound = std::sqrt(1.0f + gx * gx + gz * gz);
	// Strictly under the declared 2.0, with the margin visible in the failure message.
	CHECK(bound < 2.0f);
	CHECK(bound == doctest::Approx(1.9606f).epsilon(1e-3));
}

// Long wavelengths are the price of the bound. The point is that they still buy a
// landscape: this is what makes a 4 km horizon worth looking at rather than a flat sliver.
TEST_CASE("the relief is large enough to see across four kilometres") {
	// Worst-case swing of a sine of amplitude A over a window w: 2A sin(w/2), capped at 2A.
	auto swing = [](float amp, float freq, float span) {
		const float half = 0.5f * freq * span;
		return half >= 1.5707963f ? 2.0f * amp : 2.0f * amp * std::sin(half);
	};
	const float total = swing(kReliefAmpA, kReliefFreqA, 4000.0f) +
			swing(kReliefAmpB, kReliefFreqB, 4000.0f);
	// At least 250 m of height change across the visible 4 km, against the ~20 m the
	// existing hills() manages.
	CHECK(total > 250.0f);
}

// The demo player spawns near the origin; relief must not drop them into rock or into air.
TEST_CASE("the relief is zero at the origin so the spawn is undisturbed") {
	const float r = kReliefAmpA * std::sin(0.0f * kReliefFreqA) * std::cos(0.0f * kReliefFreqA) +
			kReliefAmpB * std::sin(0.0f * kReliefFreqB) * std::cos(0.0f * kReliefFreqB);
	CHECK(r == doctest::Approx(0.0f).epsilon(1e-6));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd extension && scons test -j8 2>&1 | tail -10; cd ..
extension/build/tests/ve_tests --test-case="*relief*"
```

Expected: the file compiles and the cases run. If "fits inside the pipeline's declared Lipschitz bound" fails, the amplitudes are wrong — fix the constants, not the assertion.

- [ ] **Step 3: Write the GLSL stage**

Create `shaders/stages/relief.field.glslh`:

```glsl
//!stage     relief
//!kind      field
//!in        sdf : float
//!in        height : float
//!out       sdf : float
//!out       height : float
//!param     amp_a : float = 250.0
//!param     freq_a : float = 0.0004
//!param     amp_b : float = 60.0
//!param     freq_b : float = 0.00083333
//!lipschitz 1.0
//!cpu       ve::stage_relief

// Long-wavelength relief, so a 4 km horizon has something on it. hills() is +/-10 m over
// ~200 m wavelengths, which subtends 1.4 px at 4 km: a flat sliver on the horizon line.
//
// The WAVELENGTHS are set by the Lipschitz bound, not by taste. The pipeline declares 2.0,
// and understating that is a correctness bug (raycast steps by 1/lipschitz and would
// overshoot a surface). hills() already contributes 0.983 and 1.099 per axis, leaving
// about 0.15 of amplitude-times-frequency budget: at 250 m that buys a 15.7 km wavelength
// and at 60 m a 7.5 km one. Across the visible 4 km that is still 250-350 m of height
// change -- a genuine landscape, and about 42 px of relief at the horizon.
//
// Phases are zero so relief(0, 0) = 0 and the demo player's spawn is undisturbed.
//
// Purely additive: it runs AFTER hills, so a pipeline that omits this line is byte-for-byte
// the pipeline that shipped before (assets/pipelines/golden.pipeline is exactly that).
void stage_relief(inout FieldCtx ctx) {
	float x = ctx.p.x, z = ctx.p.z;
	float r = P.relief_amp_a * sin(x * P.relief_freq_a) * cos(z * P.relief_freq_a)
	        + P.relief_amp_b * sin(x * P.relief_freq_b) * cos(z * P.relief_freq_b);
	ctx.height += r;
	ctx.sdf    -= r;
}
```

- [ ] **Step 4: Write the CPU mirror**

In `extension/src/terrain/builtin_stages.cpp`, add before the `VE_REGISTER_STAGE` block:

```cpp
// Line-for-line equivalent of shaders/stages/relief.field.glslh; tests/test_field_diff.gd
// is what catches drift. extra[] holds this stage's WRITES in declaration order, then its
// reads -- and sdf/height are declared both in and out, so each resolves to one slot.
void stage_relief(FieldCtx &ctx, const StageSlots &s, const StageParams &p,
		const FieldResources &) {
	const int sdf = s.extra[0];    // //!out sdf
	const int height = s.extra[1]; // //!out height
	const float x = ctx.v(s.p)[0], z = ctx.v(s.p)[2];
	const float r = p.at(0) * sinf(x * p.at(1)) * cosf(z * p.at(1))
	              + p.at(2) * sinf(x * p.at(3)) * cosf(z * p.at(3));
	ctx.f(height) += r;
	ctx.f(sdf) -= r;
}
```

and register it:

```cpp
VE_REGISTER_STAGE("ve::stage_relief", stage_relief);
```

- [ ] **Step 5: Freeze the golden pipeline and add relief to the default**

```bash
cat > assets/pipelines/golden.pipeline <<'EOF'
# FROZEN. The three stages the engine shipped with, kept byte-for-byte so
# tests/golden/{field,brick}_baseline.txt stay valid.
#
# The golden corpora exist to prove the GENERATOR did not move. Regenerating them whenever
# the demo terrain changes would discard exactly that proof, so the corpora are pinned to
# this pipeline and demo terrain evolves in default.pipeline instead.
# Do not add stages here.
seed      1337
lipschitz 2.0

stage stages/hills.field.glslh
stage stages/cave.field.glslh
stage stages/height_bands.field.glslh
EOF

python3 - <<'PY'
p = "assets/pipelines/default.pipeline"
s = open(p).read()
old = "stage stages/hills.field.glslh\n"
new = ("stage stages/hills.field.glslh\n"
       "# Long-wavelength relief so a 4 km horizon has something on it. Amplitudes are\n"
       "# budgeted against the lipschitz 2.0 above -- see relief.field.glslh.\n"
       "stage stages/relief.field.glslh\n")
assert old in s and "relief" not in s
open(p, "w").write(s.replace(old, new, 1))
print(open(p).read())
PY
```

- [ ] **Step 6: Add the `terrain_pipeline_path` property**

In `extension/src/voxel_world.h`:

```cpp
	void set_terrain_pipeline_path(const String &v) { terrain_pipeline_path_ = v; }
	String get_terrain_pipeline_path() const { return terrain_pipeline_path_; }
```

```cpp
	// The golden corpora pin their own frozen pipeline through this, so demo terrain can
	// change without invalidating the proof that the generator did not move.
	String terrain_pipeline_path_ = "res://assets/pipelines/default.pipeline";
```

In `extension/src/voxel_world.cpp`, replace the hardcoded path at line ~529:

```cpp
	if (!read_res_text(terrain_pipeline_path_, &src)) {
		UtilityFunctions::push_warning("terrain pipeline: cannot read ",
				terrain_pipeline_path_, "; "
```

(keep the rest of the warning text and the fallback behaviour unchanged), and bind the property:

```cpp
	ClassDB::bind_method(D_METHOD("set_terrain_pipeline_path", "v"),
			&VoxelWorld::set_terrain_pipeline_path);
	ClassDB::bind_method(D_METHOD("get_terrain_pipeline_path"),
			&VoxelWorld::get_terrain_pipeline_path);
```

```cpp
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "terrain_pipeline_path"),
			"set_terrain_pipeline_path", "get_terrain_pipeline_path");
```

- [ ] **Step 7: Repoint the golden test**

In `tests/test_field_baseline_gpu.gd`, wherever the suite creates its `VoxelWorld`, set the frozen pipeline before `add_child`:

```gdscript
	# The corpora prove the GENERATOR did not move, not that one terrain is blessed. They
	# are pinned to the frozen pipeline so demo terrain can change without invalidating them.
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
```

- [ ] **Step 8: Build and run the terrain tests**

```bash
./build.sh 2>&1 | tail -10
cd extension && scons test -j8 2>&1 | tail -5; cd ..
./gdunit_tests.sh -a res://tests/test_field_baseline_gpu.gd 2>&1 | tail -15
./gdunit_tests.sh -a res://tests/test_field_diff.gd 2>&1 | tail -15
```

Expected: all PASS. `test_field_baseline_gpu.gd` passing against the frozen pipeline is the proof the corpora are still valid; `test_field_diff.gd` passing is the proof the CPU mirror of `stage_relief` matches its GLSL.

- [ ] **Step 9: Commit**

```bash
git add shaders/stages/relief.field.glslh assets/pipelines/golden.pipeline \
        assets/pipelines/default.pipeline extension/src/terrain/builtin_stages.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp \
        extension/tests/test_relief_lipschitz.cpp tests/test_field_baseline_gpu.gd
git commit -m "$(cat <<'EOF'
feat: long-wavelength terrain relief, budgeted against the Lipschitz bound

hills() is +/-10 m over ~200 m wavelengths, which subtends 1.4 px at
4 km: nothing a capture or a benchmark could show. The relief stage adds
250-350 m of height change across the visible 4 km, about 42 px at the
horizon.

The wavelengths are set by the Lipschitz bound rather than by taste.
Understating it is a correctness bug -- raycast steps by 1/lipschitz and
would overshoot a surface -- so the amplitudes are budgeted to hold 2.0
and a native test checks the arithmetic that ships.

The golden corpora move to a frozen golden.pipeline instead of being
regenerated: regenerating them would discard exactly the proof that the
generator did not move.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```

---

## Task 9: Ship 4000 m and file the evidence

**Files:**
- Modify: `extension/src/core/world_store.h`, `demo/main.tscn`
- Create: `reports/4km-after/README.md` and the six leg files

**Interfaces:**
- Consumes: everything.
- Produces: `WorldConfig::stream_radius_m` default 4000.0; evidence in `reports/4km-after/`.

- [ ] **Step 1: Change the default**

In `extension/src/core/world_store.h`, replace the default and its comment:

```cpp
	// The far field's horizon. 4000 m is the shipped view distance; the sun's three
	// cascades and the terrain's relief are sized from it (ve::sun_cascades,
	// shaders/stages/relief.field.glslh). Lowering it is supported and collapses the
	// cascade set gracefully -- at or below 409.4 m it becomes a single map, which is
	// exactly the pre-cascade behaviour.
	float stream_radius_m = 4000.0f;
```

- [ ] **Step 2: Set the demo scene**

Add `stream_radius_m = 4000.0` to the `VoxelWorld` node in `demo/main.tscn`. Verify the node's other properties are untouched:

```bash
grep -n "stream_radius_m" demo/main.tscn
git diff --stat demo/main.tscn
```

Expected: exactly one added line.

- [ ] **Step 3: Full test sweep**

```bash
./build.sh 2>&1 | tail -5
mkdir -p reports/4km-after
cd extension && scons test -j8 2>&1 | tee ../reports/4km-after/native.txt; cd ..
./gdunit_tests.sh 2>&1 | tee reports/4km-after/gdunit.txt || true
grep -E "Test cases|failures" reports/4km-after/gdunit.txt | tail -3
```

Expected: native 0 failures. gdUnit exit 100 with **only** the 5 baseline assertion failures from Task 0. Any new failure must be explained in the README, not filed past.

- [ ] **Step 4: Run all six benchmark legs at the new default**

```bash
tools/run_benchmarks.sh 4km-after
grep -h "BENCH horizon\|BENCH lod_summary\|BENCH p50\|BENCH budget_verdict" reports/4km-after/*.txt
```

- [ ] **Step 5: Run the legacy-radius leg for an attributable delta**

```bash
python3 - <<'PY'
p = "demo/main.tscn"
s = open(p).read()
open(p + ".bak", "w").write(s)
open(p, "w").write(s.replace("stream_radius_m = 4000.0", "stream_radius_m = 1638.4"))
PY
tools/run_benchmarks.sh 4km-legacy-radius
mv demo/main.tscn.bak demo/main.tscn
git diff --exit-code -- demo/main.tscn && echo "scene restored cleanly"
```

- [ ] **Step 6: Capture**

```bash
godot --path . --resolution 2560x1440 demo/main.tscn -- --capture
```

Expected: 900 frames under `user://capture/`. Encode with `tools/encode_capture.sh` and note the output path in the README. The horizon should show the relief ridgeline; note the hard edge at 4000 m — it is expected and descoped (spec §9), not a bug to chase.

- [ ] **Step 7: Write the evidence README**

Write `reports/4km-after/README.md` covering, with real numbers and no fabrication:
- the exact commands run and the commit;
- native and gdUnit results against `reports/4km-baseline/`, naming the 5 carried failures;
- per-leg frame p50/p95/p99, `frames_to_horizon`, `chunks_resident`, `pages_used`, `chunk_records_high_water`, `pages_high_water`, `budget_bound`;
- per-cascade `pages` and `rebuilds`;
- the 4000 m vs 1638.4 m delta from `reports/4km-legacy-radius/`;
- **explicitly**: GPU verdicts are `UNMEASURED` because Metal returned `samples=0`, so no GPU pass budget is claimed, and no comparison is drawn against the RTX 4070/Vulkan figures in `docs/PORTFOLIO.md`;
- whether Task 7 Step 5's throughput threshold was crossed and what was done about it;
- the descoped horizon fade and cascade-boundary blend, as visible consequences rather than omissions.

- [ ] **Step 8: Commit**

```bash
git add extension/src/core/world_store.h demo/main.tscn reports/4km-after reports/4km-legacy-radius
git commit -m "$(cat <<'EOF'
feat: ship 4000 m as the default view distance, with evidence

The demo, all six benchmark legs and the capture run at 4 km. Frame cost
is measured on this M1 rather than targeted: the 16.6 ms budget is
already missed at 1638 m for reasons docs/todo/opti.md closes out as a
separate renderer-budget project, and this milestone does not inherit
them.

GPU verdicts are UNMEASURED -- Metal returns samples=0 on this host --
so only wall-frame time is claimed.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01SMVQePnxJuAuVaWVnu86Vs
EOF
)"
```
