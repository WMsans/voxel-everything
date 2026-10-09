# Fjords Conifers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dense conifer forest on the Fjords valley walls, visible at every distance: SDF trunks plus a `forest` ground material everywhere, needle leaf cards out to 300 m, and analytic per-tree imposters out to 2.5 km.

**Architecture:** One shared GLSL header, `shaders/conifer.glslh`, defines placement, shape, the trunk walk, the record packing and the imposter ray cast. The `conifers` field stage and its CPU mirror both execute it; the mirror does so natively through a small GLSL shim, so there is no hand-written second copy. A new `ConiferScatterPass` culls the tree lattice into a card list and an imposter list. Its card scatter writes the existing `LeafClump` record, which a second `LeafRasterPass` instance draws unchanged. A new `ConiferImpostorPass` draws one quad per far tree, and its fragment ray-casts the tiered crown.

**Tech Stack:** C++20 GDExtension (godot-cpp, Godot 4.7), GLSL 460, doctest (native), gdUnit4 (GPU), GDScript, ImageMagick.

**Spec:** `docs/superpowers/specs/2026-10-08-fjords-conifers-design.md`. Read it before starting, along with `docs/superpowers/specs/2026-09-18-trees-design.md` §3–§5: the leaf module whose patterns this copies.

## Global Constraints

- No `Co-Authored-By` or other AI attribution lines in any commit message. Plain conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`).
- Work only in `.worktrees/conifers` on branch `feat/conifers` (created in Task 0 from `main`). Never commit to `main`.
- Default, Mesas, Flat, Golden and Trees generate **byte-identical** `field.glslh`. `shaders/generated/field.glslh.golden` must not change. No file under `tests/golden/` may change. If one does, stop and report; do not re-record (spec §11 golden policy).
- The only generated files that may change: `shaders/generated/blocks.glslh` (Task 4: the leaf palette; Task 5: the conifer pass block) and `shaders/material_table.glslh` (Task 2: the forest row).
- `fjords.pipeline` keeps `lipschitz 8.6`. The conifers stage declares `//!lipschitz mul 1.0`, so the computed bound stays 8.54.
- Every float literal in `shaders/conifer.glslh` carries the `f` suffix. The file uses no swizzles (only `.x .y .z .w`), no `out`/`inout` parameters, no arrays and no `const` globals (use `#define`), because the CPU executes it through `extension/src/terrain/glsl_shim.h`.
- Tabs for indentation in C++, GLSL and GDScript.
- Build: `./build.sh`. Native tests: `cd extension && scons -Q test` (single case: `cd extension && ./build/tests/ve_tests -tc="<name>"`). GPU tests: `./build.sh && ./gdunit_tests.sh -a res://tests/<file>.gd`.
- Generated goldens are never hand-edited. `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests` regenerates `shaders/generated/*`. `shaders/material_table.glslh` is regenerated the way `extension/tests/test_material_glslh.cpp`'s header says (it prints the correct text on failure).
- gdUnit has a standing, drifting set of failures on clean `main`. Task 0 records it, and every "no new failures" check compares against that list, not against zero.
  - If a suite not on the list fails: `git stash -u`, re-run that suite on the clean tree, then decide.
  - A failing gdUnit case aborts the rest of its suite, so a drop in a suite's case count is itself a failure.
- Every new `.gd` file gets its `.gd.uid` sidecar committed. Create sidecars with `./build.sh --verify` (headless editor scan). Never invent a uid.
- GPU timings are invalid on this machine (`debug_gpu_timings()` returns −1). Frame cost is measured only as wall-clock percentiles from interleaved A/B/A runs.

## Deviations From The Spec (decided while planning; Task 8 records them in the spec's §13)

1. **The CPU mirror executes `conifer.glslh` itself.** The header is included inside a C++ struct whose member `conifer_ground()` reads the host sector cache, through `extension/src/terrain/glsl_shim.h`. There is no hand-written `conifers_mirror`. As a result, `conifer.glslh` is not terrain-free: its includer defines `vec4 conifer_ground(vec2 xz)` first, and shaders that never walk define a one-line stub.
2. **The trunk stage's vertical band is replaced by a bound that is safe at any slope.** The trees stage's slab argument assumes gentle hills. On a fjord wall, a trunk on a ledge above a point can be closer than `f / L`. `conifer_far_bound` is a ground-free lower bound on the distance to *any* trunk, derived from the field's slope bound of 8.6. Inside the walk, a per-cell vertical bound skips ground reads. Above about 43 m the stage reads no ground and returns `min(f, 8.6 · far_bound)`.
3. **Trees are vertical: no lean.** The spec's slope-dependent lean is dropped. A vertical trunk makes horizontal distance an exact trunk lower bound, which deviation 2 depends on.
4. **The record carries the whole shape** (two `vec4`: foot, hash, height, crown base, `R`, tiers + droop). The card scatter and the imposter therefore need neither stage params nor set 1; only the cull binds set 1.
5. **The crossfade is the leaf raster's existing fade**: cards dither out over the last 20% of `card_reach_m` (240–300 m), and the imposter keeps exactly the complementary pixels. The spec's 30 m centred band and `crossfade_m` are dropped.
6. **The imposter fragment does a 24-step inside test between the entry and exit of the bounding cylinder, then 4 bisection steps.** It does not sphere-trace a `conifer_crown_sdf`: the sawtooth tier profile has a jump at every tier and no useful Lipschitz bound.
7. **Clumps are not pushed out of the trunk.** The trunk is 0.07–0.5 m thick inside a 3 m crown, and the shell sits at 0.8–1.0 of the profile.
8. **Two `LeafRasterPass` instances**: one for leaves, one for conifer cards. Each caches its own uniform set. `draw()` takes a `LeafRasterInputs` instead of a `LeafScatterPass&`.
9. **`LeafParams` grows from 256 to 272 bytes**: `spare` becomes `palette_top`, and `palette_under` is appended.
10. **The forest texture comes from `tools/convert_forest.sh`**, a sibling of `convert_bark.sh` that does the recolour. `MATERIALS` in `convert_materials.sh` still lists `forest` for the table test, and that script skips it in its loop.
11. **`conifer.glslh` reuses `tree.glslh`'s hashes and round cone.**
12. **The cull's body is compiled only where the pipeline has the stage** (`#ifdef CONIFERS_STAGE`, defined by `conifers.field.glslh`). The shader-reload preflight compiles every `*.glsl` against the current world's generated field source, so an unguarded cull would fail reload on a Default world.
13. **The conifer pass always re-scatters.** It has no scatter-reuse epoch.
14. **`debug_conifer_stats` calls the same `VoxelFrame::draw_conifers` the compositor calls.** It has no parallel drive.

## Review Focus

1. **A tree on a ledge above a sample point, on a 60–80° wall.** Expected: the field never overstates the distance to that trunk, so no brick, mesh or raycast skips it. Tests: Task 1 (`conifer_far_bound` and the combined `conifer_field` against brute force on terraced ground: flat ledges carrying trees between slope-8.5 cliffs) and Task 3 (`test_field_diff.gd` on the shipped Fjords).
2. **A camera crossing the 240–300 m hand-off.** Expected: each pixel is drawn by a card or an imposter, never both and never neither, and the band contains no pop. Test: Task 7 asserts that the imposter's keep test is the exact complement of `leaf.frag.glsl`'s discard test (a native check of the shared expression), and the capture covers the band.
3. **A chopped near tree.** Expected: its cards disappear next frame and no imposter replaces them. Test: Task 6 (`test_painting_trunks_away_empties_the_card_list`). Painting also drops the tree from the imposter list, because the chop check runs before either append.
4. **A camera straight above a tree, looking down.** Expected: the card quad does not spin (the existing `leaf.vert` guard) and the imposter quad still covers the crown. Test: Task 1 (`conifer_ray_hit` from directly above hits). The quad's straight-down fallback is `leaf.vert.glsl`'s, reused verbatim in Task 7.
5. **A non-resident sector under part of the imposter reach**, during a fast flight. Expected: no tree stands on the fallback height. `W − 64` is under the shore, so the gate rejects it. Test: Task 1 (`conifer_ground_ok` rejects `e = −64`) and Task 6 (the cull test runs before all sectors finish).

---

## File Structure

**Create:**
- `extension/src/terrain/glsl_shim.h`: GLSL scalar and vector operations for executing shader headers natively (pure, header-only).
- `shaders/conifer.glslh`: the shared definition (spec §3–§4, §6).
- `extension/tests/test_conifer_shader.cpp`: executes `conifer.glslh` natively.
- `shaders/stages/conifers.field.glslh`: the stage.
- `extension/tests/test_conifers_pipeline.cpp`: the shipped Fjords resolves with the stage.
- `tools/convert_forest.sh`, `assets/materials/10_{basecolor,normal,roughness,ambientOcclusion,height}.png`.
- `extension/src/conifers/conifer_settings.{h,cpp}`, `conifer_settings_store.h`, `conifer_layout.{h,cpp}`.
- `extension/tests/test_conifer_layout.cpp`.
- `shaders/conifer_pass.glslh`, `shaders/conifer_cull.comp.glsl`, `shaders/conifer_scatter.comp.glsl`, `shaders/conifer_impostor.vert.glsl`, `shaders/conifer_impostor.frag.glsl`.
- `extension/src/render/conifer_scatter_pass.{h,cpp}`, `extension/src/render/conifer_impostor_pass.{h,cpp}`.
- `tests/test_conifers.gd`.

**Modify:**
- `extension/src/terrain/builtin_stages.cpp`: `ve::stage_conifers`.
- `assets/pipelines/fjords.pipeline`: append the stage.
- `extension/src/world/material_table.h`, `shaders/material_table.glslh`, `tools/convert_materials.sh`.
- `extension/src/leaves/leaf_layout.{h,cpp}`, `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh`, `shaders/leaf.frag.glsl`, `extension/tests/test_leaf_layout.cpp`.
- `extension/src/render/leaf_raster_pass.{h,cpp}`, `extension/src/render/leaf_scatter_pass.h`.
- `extension/src/render/orchestrator.{h,cpp}`, `extension/src/render/frame.{h,cpp}`, `extension/src/voxel_world.{h,cpp}`.
- `extension/src/debug/hooks.h`, `extension/src/debug/hooks_render.cpp`, `extension/src/debug/hooks.cpp` (binding).
- `extension/SConstruct`: `src/conifers/*.cpp` joins `pure_sources`.
- `tools/fjord_capture.gd`: prints conifer stats.
- `docs/superpowers/specs/2026-10-08-fjords-conifers-design.md`: §13.

---

### Task 0: Worktree and baseline

**Files:** none committed.

- [ ] **Step 1: Create the worktree**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything
git worktree add .worktrees/conifers -b feat/conifers main
cd .worktrees/conifers
```

- [ ] **Step 2: Build and run the native suite**

Run: `./build.sh && (cd extension && scons -Q test)`
Expected: all native cases pass. Record the case count.

- [ ] **Step 3: Record the gdUnit baseline**

Run: `./gdunit_tests.sh -a res://tests 2>&1 | tee "$TMPDIR/conifers-baseline.txt"`
Then list the failing suites and cases into `$TMPDIR/conifers-baseline-failures.txt`, one `suite::case` per line. Every later "no new failures" check compares against this file.

---

### Task 1: The shared header and its native harness

**Files:**
- Create: `extension/src/terrain/glsl_shim.h`
- Create: `shaders/conifer.glslh`
- Test: `extension/tests/test_conifer_shader.cpp`

**Interfaces:**
- Produces, in `conifer.glslh` (every consumer below uses these names):
  - `struct ConiferParams { float cell, density, height_min, height_max, trunk_radius, crown_radius, max_slope, shore_e, treeline_e, water_y, surface_y; }`
  - `ConiferParams conifer_params_make(float cell, float density, float height_min, float height_max, float trunk_radius, float crown_radius, float max_slope, float treeline_margin, float water_y, float shore, float snow_line, float snow_jitter, float snow_ridge_drop, float surface_y)`
  - `struct Conifer { vec3 foot; float height, crown_base, R, tiers, droop, trunk_r; uint hash; }`
  - `vec2 conifer_cell_xz(ivec2, ConiferParams)`, `bool conifer_cell_gate(ivec2, ConiferParams)`, `bool conifer_ground_ok(float e, float slope, ConiferParams)`
  - `Conifer conifer_at(ivec2 cell, vec2 xz, float ground_y, float slope, ConiferParams)`
  - `float conifer_profile(Conifer, float s)`, `float conifer_s(Conifer, float y)`, `vec3 conifer_normal(Conifer, vec3 radial, float roundness)`
  - `float conifer_trunk_sdf(vec3 p, Conifer)`, `float conifer_d_safe(ConiferParams)`, `float conifer_far_bound(float h, ConiferParams)`
  - `struct ConiferHit { float d; bool forest; bool near; }`
  - `ConiferHit conifer_walk(vec3 p, float ground_y, ConiferParams, float near_d, bool want_disc)`
  - `ConiferHit conifer_field(vec3 p, float f, float ground_y, ConiferParams, bool want_disc, float near_margin)`
  - `vec4 conifer_pack_a(Conifer)`, `vec4 conifer_pack_b(Conifer)`, `Conifer conifer_unpack(vec4 a, vec4 b)`
  - `bool conifer_inside(Conifer, vec3 q)`, `float conifer_ray_hit(Conifer, vec3 ro, vec3 rd)`
  - Macros: `CONIFER_JITTER 0.35f`, `CONIFER_TOOTH_MAX 1.15f`, `CONIFER_R_MAX 1.25f`, `CONIFER_TRUNK_MAX 1.2f`, `CONIFER_FIELD_L 8.6f`
- Requires from the includer: `vec4 conifer_ground(vec2 xz)` (height above `SURFACE_Y`, dh/dx, dh/dz, ridge).

- [ ] **Step 1: Write the shim**

`extension/src/terrain/glsl_shim.h`:

```cpp
// GLSL scalar and vector operations, so a shader header can be executed natively by the CPU
// mirror and by doctest. INCLUDE IT INSIDE YOUR OWN NAMESPACE, after <cmath>, <cstdint> and
// <cstring>: every name here must shadow the global C maths functions, which only happens
// when they are declared in the same namespace the shader header lands in. No #pragma once
// on purpose -- each namespace gets its own copy.
//
// Nothing here is shader maths; it is the GLSL vocabulary the headers are written in.
using uint = uint32_t;
template<class T> struct V2 {
	T x, y;
	V2() : x(0), y(0) {}
	explicit V2(T a) : x(a), y(a) {}
	V2(T a, T b) : x(a), y(b) {}
};
template<class T> struct V3 {
	T x, y, z;
	V3() : x(0), y(0), z(0) {}
	explicit V3(T a) : x(a), y(a), z(a) {}
	V3(T a, T b, T c) : x(a), y(b), z(c) {}
};
template<class T> struct V4 {
	T x, y, z, w;
	V4() : x(0), y(0), z(0), w(0) {}
	explicit V4(T a) : x(a), y(a), z(a), w(a) {}
	V4(T a, T b, T c, T d) : x(a), y(b), z(c), w(d) {}
};
using vec2 = V2<float>;
using vec3 = V3<float>;
using vec4 = V4<float>;
using ivec2 = V2<int>;
using uvec2 = V2<uint>;
template<class T> V2<T> operator+(V2<T> a, V2<T> b) { return {a.x + b.x, a.y + b.y}; }
template<class T> V2<T> operator-(V2<T> a, V2<T> b) { return {a.x - b.x, a.y - b.y}; }
inline vec2 operator*(vec2 a, float b) { return {a.x * b, a.y * b}; }
inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator*(vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
inline vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
inline vec3 operator-(vec3 a) { return {-a.x, -a.y, -a.z}; }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
inline float sqrt(float v) { return std::sqrt(v); }
inline float sin(float v) { return std::sin(v); }
inline float cos(float v) { return std::cos(v); }
inline float floor(float v) { return std::floor(v); }
inline float abs(float v) { return std::fabs(v); }
inline float length(vec3 a) { return sqrt(dot(a, a)); }
inline float length(vec2 a) { return sqrt(dot(a, a)); }
inline vec3 normalize(vec3 a) { return a / length(a); }
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
inline float uintBitsToFloat(uint u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint floatBitsToUint(float f) { uint u; std::memcpy(&u, &f, 4); return u; }
```

- [ ] **Step 2: Write the failing native test**

`extension/tests/test_conifer_shader.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// Executes the ACTUAL shaders/conifer.glslh natively, as test_tree_shader.cpp executes
// tree.glslh. The synthetic ground is a plane, h = 10 + slope_x * x, so every bound below is
// checked against geometry whose answer is known.
namespace {
namespace cs {
#include "terrain/glsl_shim.h"
// terraced: 12 m flat ledges (trees stand there) between 4 m cliffs of slope 8.5 -- the
// fjord-wall case Review Focus 1 is about, kept inside the field's 8.6 slope bound.
struct Eval {
	float slope_x = 0.0f;
	bool terraced = false;
	vec4 conifer_ground(vec2 xz) {
		if (!terraced) return vec4(10.0f + slope_x * xz.x, slope_x, 0.0f, 0.0f);
		const float k = floor(xz.x / 16.0f);
		const float u = xz.x - k * 16.0f;
		const float cliff = u > 12.0f ? u - 12.0f : 0.0f;
		return vec4(10.0f + k * 34.0f + cliff * 8.5f, u > 12.0f ? 8.5f : 0.0f, 0.0f, 0.0f);
	}
#include "../../shaders/conifer.glslh"
};
} // namespace cs

constexpr float kSurfaceY = 51.2f;

cs::Eval::ConiferParams params(cs::Eval &ev) {
	// fjords.pipeline's values: water 51.2, shore 2, snow line 380 +- 25, ridge drop 40.
	return ev.conifer_params_make(8.0f, 0.85f, 18.0f, 28.0f, 0.45f, 3.0f, 1.7f, 40.0f,
			51.2f, 2.0f, 380.0f, 25.0f, 40.0f, kSurfaceY);
}

// Every present tree whose foot is within `radius` metres of p in XZ, brute force: the
// answer conifer_walk and conifer_far_bound must never overstate.
float brute_trunk_distance(cs::Eval &ev, const cs::Eval::ConiferParams &cp, cs::vec3 p, int radius_cells) {
	float best = 1.0e30f;
	const int bx = int(std::floor(p.x / cp.cell)), bz = int(std::floor(p.z / cp.cell));
	for (int dz = -radius_cells; dz <= radius_cells; dz++)
		for (int dx = -radius_cells; dx <= radius_cells; dx++) {
			const cs::ivec2 cell(bx + dx, bz + dz);
			if (!ev.conifer_cell_gate(cell, cp)) continue;
			const cs::vec2 xz = ev.conifer_cell_xz(cell, cp);
			const cs::vec4 g = ev.conifer_ground(xz);
			const float slope = std::sqrt(g.y * g.y + g.z * g.z);
			if (!ev.conifer_ground_ok(cp.surface_y + g.x - cp.water_y, slope, cp)) continue;
			const auto c = ev.conifer_at(cell, xz, cp.surface_y + g.x, slope, cp);
			best = std::fmin(best, ev.conifer_trunk_sdf(p, c));
		}
	return best;
}

uint32_t lcg(uint32_t &s) { s = s * 1664525u + 1013904223u; return s; }
float frand(uint32_t &s, float lo, float hi) { return lo + (hi - lo) * float(lcg(s) >> 8) / 16777216.0f; }
} // namespace

TEST_CASE("conifer placement is deterministic and never leaves its cell's jitter box") {
	cs::Eval ev;
	const auto cp = params(ev);
	for (int x : {-9, 0, 3, 77, -40000})
		for (int z : {-4, 0, 11, 250, 31000}) {
			const cs::vec2 a = ev.conifer_cell_xz(cs::ivec2(x, z), cp);
			const cs::vec2 b = ev.conifer_cell_xz(cs::ivec2(x, z), cp);
			CHECK(a.x == b.x);
			CHECK(a.y == b.y);
			const float cx = (float(x) + 0.5f) * cp.cell, cz = (float(z) + 0.5f) * cp.cell;
			CHECK(std::fabs(a.x - cx) <= 0.35f * cp.cell + 1e-3f);
			CHECK(std::fabs(a.y - cz) <= 0.35f * cp.cell + 1e-3f);
		}
}

TEST_CASE("the grove gate keeps most cells and opens clearings") {
	cs::Eval ev;
	const auto cp = params(ev);
	int kept = 0, total = 0;
	for (int z = 0; z < 120; z++)
		for (int x = 0; x < 120; x++) {
			kept += ev.conifer_cell_gate(cs::ivec2(x, z), cp);
			total++;
		}
	const float frac = float(kept) / float(total);
	CHECK(frac > 0.45f);
	// Below the 0.85 density by the clearings' share; a gate that ignored the grove noise
	// would land on 0.85.
	CHECK(frac < 0.83f);
}

TEST_CASE("the ground gate takes shore, tree line and slope from the fjord bands") {
	cs::Eval ev;
	const auto cp = params(ev);
	CHECK(cp.shore_e == doctest::Approx(3.0f));
	CHECK(cp.treeline_e == doctest::Approx(380.0f - 25.0f - 40.0f - 40.0f));
	CHECK(ev.conifer_ground_ok(50.0f, 0.5f, cp));
	CHECK_FALSE(ev.conifer_ground_ok(2.5f, 0.5f, cp));    // shore band
	CHECK_FALSE(ev.conifer_ground_ok(-64.0f, 0.5f, cp));  // the non-resident fallback height
	CHECK_FALSE(ev.conifer_ground_ok(300.0f, 0.5f, cp));  // above the tree line
	CHECK(ev.conifer_ground_ok(50.0f, 1.65f, cp));        // steep walls carry forest
	CHECK_FALSE(ev.conifer_ground_ok(50.0f, 1.75f, cp));
}

TEST_CASE("the tier profile never leaves 1.15 x the envelope and is zero off the crown") {
	cs::Eval ev;
	const auto cp = params(ev);
	for (int i = 0; i < 400; i++) {
		const auto c = ev.conifer_at(cs::ivec2(i, -i * 3), cs::vec2(0.0f, 0.0f), 60.0f, 0.3f, cp);
		CHECK(c.tiers >= 6.0f);
		CHECK(c.tiers <= 9.0f);
		CHECK(c.R <= cp.crown_radius * 1.25f + 1e-4f);
		CHECK(ev.conifer_profile(c, -0.01f) == 0.0f);
		CHECK(ev.conifer_profile(c, 1.01f) == 0.0f);
		for (int k = 0; k <= 200; k++) {
			const float s = float(k) / 200.0f;
			CHECK(ev.conifer_profile(c, s) <= 1.15f * c.R * (1.0f - s) + 1e-4f);
		}
	}
}

TEST_CASE("a trunk is 1-Lipschitz along secants") {
	cs::Eval ev;
	const auto cp = params(ev);
	const auto c = ev.conifer_at(cs::ivec2(3, 4), cs::vec2(28.0f, 36.0f), 70.0f, 0.8f, cp);
	uint32_t seed = 7u;
	for (int i = 0; i < 4000; i++) {
		const cs::vec3 a(frand(seed, 20, 36), frand(seed, 60, 105), frand(seed, 28, 44));
		const cs::vec3 b(frand(seed, 20, 36), frand(seed, 60, 105), frand(seed, 28, 44));
		const float da = ev.conifer_trunk_sdf(a, c), db = ev.conifer_trunk_sdf(b, c);
		CHECK(std::fabs(da - db) <= cs::length(a - b) * 1.0001f + 1e-4f);
	}
}

TEST_CASE("the record round-trips every field a scatter or imposter reads") {
	cs::Eval ev;
	const auto cp = params(ev);
	const auto c = ev.conifer_at(cs::ivec2(-12, 9), cs::vec2(-90.5f, 75.25f), 140.0f, 1.1f, cp);
	const auto u = ev.conifer_unpack(ev.conifer_pack_a(c), ev.conifer_pack_b(c));
	CHECK(u.foot.x == c.foot.x);
	CHECK(u.foot.y == c.foot.y);
	CHECK(u.foot.z == c.foot.z);
	CHECK(u.hash == c.hash);
	CHECK(u.height == c.height);
	CHECK(u.crown_base == c.crown_base);
	CHECK(u.R == c.R);
	CHECK(u.tiers == c.tiers);
	CHECK(u.droop == doctest::Approx(c.droop).epsilon(1e-5));
}

// Ground configurations: flat, a gentle plane trees stand on, and the terraced wall.
void configure(cs::Eval &ev, int k) {
	ev.terraced = k == 2;
	ev.slope_x = k == 1 ? 1.0f : 0.0f;
}

TEST_CASE("the walk never overstates the trunk distance, and is exact inside d_safe") {
	cs::Eval ev;
	const auto cp = params(ev);
	for (int k = 0; k < 3; k++) {
		configure(ev, k);
		uint32_t seed = 11u;
		for (int i = 0; i < 3000; i++) {
			const float x = frand(seed, -60, 60), z = frand(seed, -60, 60);
			const float ground_y = kSurfaceY + ev.conifer_ground(cs::vec2(x, z)).x;
			const cs::vec3 p(x, ground_y + frand(seed, -2, 45), z);
			const float truth = brute_trunk_distance(ev, cp, p, 4);
			const auto hit = ev.conifer_walk(p, ground_y, cp, 1.0e30f, false);
			CHECK(hit.d <= truth + 1e-4f);
			if (truth < ev.conifer_d_safe(cp)) CHECK(hit.d == doctest::Approx(truth).epsilon(1e-5));
		}
	}
}

// Review Focus 1: a trunk on a ledge above the point, on walls up to 8.5 (the field bound is
// 8.6), never makes the stage's answer overstate the distance by more than the field's own L.
TEST_CASE("the far bound and the combined field never overstate, even under a cliff-top trunk") {
	cs::Eval ev;
	const auto cp = params(ev);
	int trees_near = 0;
	for (int k = 0; k < 3; k++) {
		configure(ev, k);
		uint32_t seed = 23u;
		for (int i = 0; i < 3000; i++) {
			const float x = frand(seed, -40, 40), z = frand(seed, -40, 40);
			const float ground_y = kSurfaceY + ev.conifer_ground(cs::vec2(x, z)).x;
			const float h = frand(seed, -2, 400);
			const cs::vec3 p(x, ground_y + h, z);
			// 12 cells = 96 m of neighbours: every trunk a 400 m-high point could be near.
			const float truth = brute_trunk_distance(ev, cp, p, 12);
			trees_near += truth < 60.0f;
			CHECK(ev.conifer_far_bound(h, cp) <= truth + 1e-3f);
			const float f = h; // the plane's field value at p
			const auto hit = ev.conifer_field(p, f, ground_y, cp, false, 0.06f);
			CHECK(hit.d <= f + 1e-6f);
			CHECK(hit.d / 8.6f <= std::fmin(truth, f) + 1e-3f);
		}
	}
	CHECK(trees_near > 1000); // the cases above actually had trunks to overstate
}

TEST_CASE("the forest disc covers the ground under a present crown and nothing far from one") {
	cs::Eval ev;
	const auto cp = params(ev);
	int at_feet = 0, discs = 0;
	for (int cx = 0; cx < 30; cx++) {
		const cs::ivec2 cell(cx, 2);
		if (!ev.conifer_cell_gate(cell, cp)) continue;
		const cs::vec2 xz = ev.conifer_cell_xz(cell, cp);
		const cs::vec3 p(xz.x + 0.5f, kSurfaceY + 10.0f - 0.1f, xz.y);
		at_feet++;
		discs += ev.conifer_walk(p, kSurfaceY + 10.0f, cp, 0.06f, true).forest;
	}
	CHECK(at_feet > 0);
	CHECK(discs == at_feet);
}

TEST_CASE("the imposter ray hits the crown from the side and from above, and misses beside it") {
	cs::Eval ev;
	const auto cp = params(ev);
	const auto c = ev.conifer_at(cs::ivec2(0, 0), cs::vec2(0.0f, 0.0f), 60.0f, 0.2f, cp);
	const float mid_y = c.foot.y + c.crown_base + 0.3f * (c.height - c.crown_base);
	const cs::vec3 side_o(-300.0f, mid_y, 0.0f), side_d(1.0f, 0.0f, 0.0f);
	const float t = ev.conifer_ray_hit(c, side_o, side_d);
	REQUIRE(t > 0.0f);
	CHECK(ev.conifer_inside(c, side_o + side_d * t));
	CHECK(t < 300.0f); // in front of the axis
	const cs::vec3 top_o(0.0f, c.foot.y + c.height + 200.0f, 0.0f), down(0.0f, -1.0f, 0.0f);
	CHECK(ev.conifer_ray_hit(c, top_o, down) > 0.0f);
	const cs::vec3 miss_o(-300.0f, mid_y, c.R * 1.2f + 0.5f);
	CHECK(ev.conifer_ray_hit(c, miss_o, side_d) < 0.0f);
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile failure, `conifer.glslh: No such file or directory`.

- [ ] **Step 4: Write `shaders/conifer.glslh`**

```glsl
// Shared conifer definition (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md
// §3-§4, §6). ONE definition, five consumers: the conifers field stage (trunk SDF and the
// forest disc), the conifer cull, the card scatter, the imposter, and the CPU mirror -- which
// executes this very file through extension/src/terrain/glsl_shim.h. Hence its rules:
//   - only .x .y .z .w member access, never a swizzle; construct with vecN(...);
//   - no out/inout parameters, no arrays, no const globals (use #define);
//   - every float literal carries the f suffix, so the CPU computes in float as the GPU does.
//
// The file that pulls this one in defines `vec4 conifer_ground(vec2 xz)` FIRST: the ground
// height above SURFACE_Y, dh/dx, dh/dz and ridge. Only conifer_walk calls it; a shader that
// never walks defines a stub.
//
// Coordinates are FIELD space (after the world seed's offset) everywhere except a record's
// foot once the cull has moved it to world space.
//
// NOTE: never put a literal include directive inside a comment in this file -- the loader
// matches include tokens anywhere in a line and would self-include.

#include "tree.glslh"

#define CONIFER_JITTER 0.35f
// A tier's lower edge flares to this multiple of the envelope R (1 - s).
#define CONIFER_TOOTH_MAX 1.15f
// Crown radius and trunk radius jitter ceilings, as multiples of their params.
#define CONIFER_R_MAX 1.25f
#define CONIFER_TRUNK_MAX 1.2f
// The field's slope bound in Fjords. It must be >= fjords.pipeline's computed Lipschitz bound
// (8.54; extension/tests/test_conifers_pipeline.cpp asserts it), because conifer_walk and
// conifer_far_bound use it as "the ground rises at most this fast".
#define CONIFER_FIELD_L 8.6f
#define CONIFER_SLACK 0.05f
#define CONIFER_RAY_STEPS 24
#define CONIFER_RAY_BISECT 4

struct ConiferParams {
	float cell;
	float density;
	float height_min;
	float height_max;
	float trunk_radius;
	float crown_radius;
	float max_slope;
	float shore_e;    // a foot stands higher than this above water_y ...
	float treeline_e; // ... and lower than this
	float water_y;
	float surface_y;
};

// The tree line sits below the LOWEST the snow line can reach: its noise waver and its full
// ridge drop (fjord_bands.field.glslh), then a margin. The shore test sits one metre above
// fjord_bands' shore band, so no trunk stands in the future lake.
ConiferParams conifer_params_make(float cell, float density, float height_min, float height_max,
		float trunk_radius, float crown_radius, float max_slope, float treeline_margin,
		float water_y, float shore, float snow_line, float snow_jitter, float snow_ridge_drop,
		float surface_y) {
	ConiferParams cp;
	cp.cell = cell;
	cp.density = density;
	cp.height_min = height_min;
	cp.height_max = height_max;
	cp.trunk_radius = trunk_radius;
	cp.crown_radius = crown_radius;
	cp.max_slope = max_slope;
	cp.shore_e = shore + 1.0f;
	cp.treeline_e = snow_line - snow_jitter - snow_ridge_drop - treeline_margin;
	cp.water_y = water_y;
	cp.surface_y = surface_y;
	return cp;
}

struct Conifer {
	vec3 foot;        // trunk foot
	float height;     // foot to tip
	float crown_base; // metres above the foot where the crown starts
	float R;          // crown radius at its base, before the tier tooth
	float tiers;      // 6..9
	float droop;      // 0.3..0.7, shapes each tier's skirt
	float trunk_r;    // foot radius; NOT carried by a record (see conifer_unpack)
	uint hash;
};

// The jittered foot XZ. Always inside the cell's CONIFER_JITTER box, which conifer_d_safe()
// depends on.
vec2 conifer_cell_xz(ivec2 cell, ConiferParams cp) {
	float jx = tree_snorm(tree_hash2(cell, 0xC0F1u)) * CONIFER_JITTER * cp.cell;
	float jz = tree_snorm(tree_hash2(cell, 0xC0F2u)) * CONIFER_JITTER * cp.cell;
	return vec2((float(cell.x) + 0.5f) * cp.cell + jx, (float(cell.y) + 0.5f) * cp.cell + jz);
}

// Hashes only, cheapest test first. Bilinear value noise on a 6-cell lattice (~50 m) opens
// clearings: where it is below 0.15 nothing grows, above 0.35 the full density does.
bool conifer_cell_gate(ivec2 cell, ConiferParams cp) {
	float gx = float(cell.x) / 6.0f;
	float gz = float(cell.y) / 6.0f;
	float bx = floor(gx);
	float bz = floor(gz);
	ivec2 g = ivec2(int(bx), int(bz));
	float fx = gx - bx;
	float fz = gz - bz;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fz = fz * fz * (3.0f - 2.0f * fz);
	float a = tree_unit(tree_hash2(g, 0xC0F3u));
	float b = tree_unit(tree_hash2(g + ivec2(1, 0), 0xC0F3u));
	float c = tree_unit(tree_hash2(g + ivec2(0, 1), 0xC0F3u));
	float d = tree_unit(tree_hash2(g + ivec2(1, 1), 0xC0F3u));
	float grove = mix(mix(a, b, fx), mix(c, d, fx), fz);
	float clearing = clamp((grove - 0.15f) / 0.2f, 0.0f, 1.0f);
	clearing = clearing * clearing * (3.0f - 2.0f * clearing);
	return tree_unit(tree_hash2(cell, 0xC0F4u)) < cp.density * clearing;
}

// e is the foot's elevation above water_y; slope is |grad h| there.
bool conifer_ground_ok(float e, float slope, ConiferParams cp) {
	return e > cp.shore_e && e < cp.treeline_e && slope < cp.max_slope;
}

Conifer conifer_at(ivec2 cell, vec2 xz, float ground_y, float slope, ConiferParams cp) {
	Conifer c;
	c.hash = tree_hash2(cell, 0xC0F5u);
	c.height = mix(cp.height_min, cp.height_max, tree_unit(c.hash));
	c.trunk_r = cp.trunk_radius * (1.0f + 0.2f * tree_snorm(tree_hash(c.hash ^ 0x11u)));
	c.crown_base = c.height * mix(0.15f, 0.30f, tree_unit(tree_hash(c.hash ^ 0x12u)));
	c.R = cp.crown_radius * (1.0f + 0.25f * tree_snorm(tree_hash(c.hash ^ 0x13u)));
	c.tiers = floor(mix(6.0f, 9.999f, tree_unit(tree_hash(c.hash ^ 0x14u))));
	c.droop = mix(0.3f, 0.7f, tree_unit(tree_hash(c.hash ^ 0x15u)));
	// Sunk by the slope across the trunk, so the downhill side of a foot on a 60-degree wall
	// is never in air.
	c.foot = vec3(xz.x, ground_y - slope * c.trunk_r - 0.3f, xz.y);
	return c;
}

// Crown radius at crown height s (0 = crown base, 1 = tip); zero outside. The envelope
// R (1 - s) carries `tiers` sawtooth skirts, each widest (CONIFER_TOOTH_MAX x) at its lower
// edge and narrowing to 0.55 x at its top. droop bends the skirt.
float conifer_profile(Conifer c, float s) {
	if (s < 0.0f || s > 1.0f) return 0.0f;
	float u = s * c.tiers;
	u = u - floor(u);
	float t = 1.0f - u;
	float tooth = 0.55f + 0.6f * t * mix(1.0f, t, c.droop);
	return c.R * (1.0f - s) * tooth;
}

float conifer_s(Conifer c, float y) {
	return (y - c.foot.y - c.crown_base) / max(c.height - c.crown_base, 1e-3f);
}

// Shading normal at a crown point with horizontal outward direction `radial`. The cone
// normal follows the envelope; the tier normal tilts up along a skirt. roundness 0 is one
// smooth cone, 1 shades every tier as its own skirt. Cards and imposters both call this, so
// the hand-off does not change a tree's shading.
vec3 conifer_normal(Conifer c, vec3 radial, float roundness) {
	float rise = c.R / max(c.height - c.crown_base, 1e-3f);
	vec3 n_cone = normalize(vec3(radial.x, rise, radial.z));
	vec3 n_tier = normalize(vec3(radial.x, 1.2f, radial.z));
	return normalize(mix(n_cone, n_tier, roundness));
}

// Exact: one vertical round cone, tapering to 15% at the tip.
float conifer_trunk_sdf(vec3 p, Conifer c) {
	return tree_round_cone(p, c.foot, c.foot + vec3(0.0f, c.height, 0.0f), c.trunk_r,
			c.trunk_r * 0.15f);
}

float conifer_trunk_reach(ConiferParams cp) { return cp.trunk_radius * CONIFER_TRUNK_MAX + CONIFER_SLACK; }
float conifer_disc_reach(ConiferParams cp) {
	return cp.crown_radius * CONIFER_R_MAX * CONIFER_TOOTH_MAX + CONIFER_SLACK;
}

// What the 3x3 walk can vouch for: a foot two cells away is at least (1.5 - jitter) cells
// from any point of the centre cell (the trees stage's argument), less a trunk radius.
float conifer_d_safe(ConiferParams cp) {
	return max(0.0f, (1.5f - CONIFER_JITTER) * cp.cell - cp.trunk_radius * CONIFER_TRUNK_MAX);
}

// A lower bound on the distance from a point h metres above the ground at its own XZ to ANY
// trunk, read from no ground. A vertical trunk whose foot is r away in XZ stands on ground at
// most CONIFER_FIELD_L * r higher (the field's slope bound) and is at most height_max tall,
// so its distance is at least max(r - reach, h - CONIFER_FIELD_L * r - height_max). The
// smallest that can be over all r is where the two terms meet (plan deviation 2).
float conifer_far_bound(float h, ConiferParams cp) {
	float tr = conifer_trunk_reach(cp);
	return (h - cp.height_max + tr) / (1.0f + CONIFER_FIELD_L) - tr;
}

struct ConiferHit {
	float d;     // conifer_walk: nearest trunk, clamped to d_safe. conifer_field: the field value.
	bool forest; // p lies inside a present tree's crown disc in XZ (only when asked)
	bool near;   // a trunk is within near_margin of the field value: gradient taps must look
};

// The 3x3 cell walk. near_d is the field value a trunk must beat to matter; cells that
// provably cannot are skipped before their ground is read, by the horizontal bound and the
// vertical one from conifer_far_bound's argument.
ConiferHit conifer_walk(vec3 p, float ground_y, ConiferParams cp, float near_d, bool want_disc) {
	ConiferHit hit;
	hit.d = conifer_d_safe(cp);
	hit.forest = false;
	hit.near = false;
	float trunk_reach = conifer_trunk_reach(cp);
	float disc_reach = conifer_disc_reach(cp);
	float h = p.y - ground_y;
	ivec2 base = ivec2(int(floor(p.x / cp.cell)), int(floor(p.z / cp.cell)));
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 cell = base + ivec2(dx, dz);
			vec2 xz = conifer_cell_xz(cell, cp);
			float hx = p.x - xz.x;
			float hz = p.z - xz.y;
			float r = sqrt(hx * hx + hz * hz);
			float lb = max(r - trunk_reach, h - CONIFER_FIELD_L * r - cp.height_max);
			bool need_trunk = lb < min(hit.d, near_d);
			bool need_disc = want_disc && !hit.forest && r < disc_reach;
			if (!need_trunk && !need_disc) continue;
			if (!conifer_cell_gate(cell, cp)) continue;
			vec4 g = conifer_ground(xz);
			float slope = sqrt(g.y * g.y + g.z * g.z);
			if (!conifer_ground_ok(cp.surface_y + g.x - cp.water_y, slope, cp)) continue;
			Conifer c = conifer_at(cell, xz, cp.surface_y + g.x, slope, cp);
			if (need_trunk) hit.d = min(hit.d, conifer_trunk_sdf(p, c));
			if (need_disc && r <= conifer_profile(c, 0.0f)) hit.forest = true;
		}
	}
	return hit;
}

// The stage's whole answer for a point whose field value so far is f and whose own ground is
// ground_y. The result never exceeds f, and result / CONIFER_FIELD_L never exceeds the true
// distance to the terrain or any trunk -- test_conifer_shader.cpp checks both against brute
// force on walls up to slope 8.5.
ConiferHit conifer_field(vec3 p, float f, float ground_y, ConiferParams cp, bool want_disc,
		float near_margin) {
	ConiferHit hit;
	hit.forest = false;
	hit.near = true;
	float far_d = CONIFER_FIELD_L * conifer_far_bound(p.y - ground_y, cp);
	if (far_d >= conifer_d_safe(cp) && !want_disc) {
		// No walk can say less than d_safe, so the far bound is the answer: no ground read.
		hit.d = min(f, far_d);
		return hit;
	}
	ConiferHit w = conifer_walk(p, ground_y, cp, f + near_margin, want_disc);
	hit.near = w.d < f + near_margin;
	hit.forest = w.forest;
	hit.d = min(f, max(w.d, far_d));
	return hit;
}

// Two vec4 hold everything the card scatter and the imposter need, so neither binds the
// field (plan deviation 4).
vec4 conifer_pack_a(Conifer c) { return vec4(c.foot.x, c.foot.y, c.foot.z, uintBitsToFloat(c.hash)); }
vec4 conifer_pack_b(Conifer c) { return vec4(c.height, c.crown_base, c.R, c.tiers + c.droop); }

Conifer conifer_unpack(vec4 a, vec4 b) {
	Conifer c;
	c.foot = vec3(a.x, a.y, a.z);
	c.hash = floatBitsToUint(a.w);
	c.height = b.x;
	c.crown_base = b.y;
	c.R = b.z;
	c.tiers = floor(b.w);
	c.droop = b.w - c.tiers;
	c.trunk_r = 0.0f;
	return c;
}

bool conifer_inside(Conifer c, vec3 q) {
	float dx = q.x - c.foot.x;
	float dz = q.z - c.foot.z;
	// Strict, so a point on the axis above the tip (profile 0) is outside.
	return sqrt(dx * dx + dz * dz) < conifer_profile(c, conifer_s(c, q.y));
}

// The imposter's ray cast (plan deviation 6): the ray parameter of the first point inside
// the tiered crown, or -1. Clip to the crown's bounding cylinder (radius R x the tooth, from
// crown base to tip), take CONIFER_RAY_STEPS inside tests across it, then bisect. rd is unit.
float conifer_ray_hit(Conifer c, vec3 ro, vec3 rd) {
	float rad = c.R * CONIFER_TOOTH_MAX;
	float y0 = c.foot.y + c.crown_base;
	float y1 = c.foot.y + c.height;
	float ox = ro.x - c.foot.x;
	float oz = ro.z - c.foot.z;
	float a = rd.x * rd.x + rd.z * rd.z;
	float b = ox * rd.x + oz * rd.z;
	float k = ox * ox + oz * oz - rad * rad;
	float t0 = -1.0e30f;
	float t1 = 1.0e30f;
	if (a > 1e-8f) {
		float disc = b * b - a * k;
		if (disc < 0.0f) return -1.0f;
		float sq = sqrt(disc);
		t0 = (-b - sq) / a;
		t1 = (-b + sq) / a;
	} else if (k > 0.0f) {
		return -1.0f;
	}
	if (abs(rd.y) > 1e-8f) {
		float ta = (y0 - ro.y) / rd.y;
		float tb = (y1 - ro.y) / rd.y;
		t0 = max(t0, min(ta, tb));
		t1 = min(t1, max(ta, tb));
	} else if (ro.y < y0 || ro.y > y1) {
		return -1.0f;
	}
	t0 = max(t0, 0.0f);
	if (t1 <= t0) return -1.0f;
	float dt = (t1 - t0) / float(CONIFER_RAY_STEPS);
	float prev = t0;
	for (int i = 0; i <= CONIFER_RAY_STEPS; i++) {
		float t = t0 + dt * float(i);
		if (conifer_inside(c, ro + rd * t)) {
			if (i == 0) return t;
			float lo = prev;
			float hi = t;
			for (int j = 0; j < CONIFER_RAY_BISECT; j++) {
				float mid = 0.5f * (lo + hi);
				if (conifer_inside(c, ro + rd * mid)) hi = mid;
				else lo = mid;
			}
			return hi;
		}
		prev = t;
	}
	return -1.0f;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd extension && scons -Q test 2>&1 | tail -5 && ./build/tests/ve_tests -tc="*conifer*,*forest disc*,*imposter ray*,*far bound*,*walk*,*record*,*tier profile*,*grove*,*ground gate*,*trunk is 1-Lipschitz*"`
Expected: PASS for every case. If "the far bound and the combined field never overstate" fails, the derivation in `conifer_far_bound` is wrong. Do not loosen the test; fix the bound.

- [ ] **Step 6: Commit**

```bash
git add extension/src/terrain/glsl_shim.h shaders/conifer.glslh extension/tests/test_conifer_shader.cpp
git commit -m "feat: the shared conifer definition, executed natively"
```

---

### Task 2: The forest material

**Files:**
- Create: `tools/convert_forest.sh`, `assets/materials/10_{basecolor,normal,roughness,ambientOcclusion,height}.png`
- Modify: `tools/convert_materials.sh` (MATERIALS, skip), `extension/src/world/material_table.h` (row), `shaders/material_table.glslh` (regenerated)
- Test: `extension/tests/test_material_table.cpp` (existing converter-list case), `extension/tests/test_material_glslh.cpp` (existing golden)

**Interfaces:**
- Produces: material `forest`, id 11, atlas layer `10`, GLSL constant `MAT_FOREST`, C++ `ve::material_id("forest") == 11`.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_material_table.cpp`:

```cpp
TEST_CASE("forest is id 11 on layer 10 and renumbers nothing") {
	static_assert(ve::material_id("snow") == 10);
	static_assert(ve::material_id("forest") == 11);
	CHECK(std::string(ve::kMaterials[10].asset) == "10");
	// Dark needle green from a distance: the far field sees this texture's top mip.
	CHECK(ve::kMaterials[10].flat_albedo[1] > ve::kMaterials[10].flat_albedo[0]);
	CHECK(ve::kMaterials[10].flat_albedo[1] < 0.35f);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile failure: `material_id("forest")` is not a constant expression (no such row).

- [ ] **Step 3: Write `tools/convert_forest.sh` and generate the PNGs**

```bash
#!/usr/bin/env bash
# Builds layer 10 (forest) from terrain_textures_vol2's ground_foliage_01, recoloured toward
# dark needle green. Up close it reads as needle litter between the conifer cards; from far
# away the far field sees only the top mip, which is the canopy colour the references show
# (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md §8). A sibling of
# convert_bark.sh: convert_materials.sh lists `forest` in MATERIALS (the table test reads that
# line) and skips it in its loop, because the pack has no forest folder.
#
# The recolour is the tuning knob: change MODULATE / TINT and re-run, no code changes.
# Same conventions as convert_materials.sh: 512x512! resize, -strip, PNG24, NN_<map>.png.
set -euo pipefail

SRC="${1:-/Users/jeremyzhao/Development/unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2}"
DST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/assets/materials"
NN=10
M=ground_foliage_01
# brightness,saturation,hue (ImageMagick -modulate): darker, a little more saturated, hue
# rotated from olive toward blue-green.
MODULATE="55,115,112"
# A multiply toward the reference canopy colour after the modulate.
TINT="rgb(150,190,165)"

command -v magick >/dev/null || { echo "need ImageMagick 'magick'" >&2; exit 1; }
[ -d "$SRC/$M" ] || { echo "source not found: $SRC/$M" >&2; exit 1; }
mkdir -p "$DST"

magick "$SRC/$M/T_${M}_basecolor.tga" -resize 512x512! -modulate "$MODULATE" \
	\( +clone -fill "$TINT" -colorize 100 \) -compose multiply -composite \
	-strip "PNG24:$DST/${NN}_basecolor.png"
echo "  $DST/${NN}_basecolor.png"
for map in normal roughness ambientOcclusion height; do
	magick "$SRC/$M/T_${M}_${map}.tga" -resize 512x512! -strip "PNG24:$DST/${NN}_${map}.png"
	echo "  $DST/${NN}_${map}.png"
done
echo "wrote layer $NN (forest) to $DST"
```

Run:
```bash
chmod +x tools/convert_forest.sh && tools/convert_forest.sh
magick assets/materials/10_basecolor.png -resize 1x1! -format '%[fx:r] %[fx:g] %[fx:b]\n' info:
```
Expected: five PNGs written, and a mean colour printed (for example `0.12 0.19 0.13`). Write that mean down; it becomes the row's `flat_albedo`.

- [ ] **Step 4: Add `forest` to `convert_materials.sh` and skip it in the loop**

Change the MATERIALS line and the loop head in `tools/convert_materials.sh`:

```bash
MATERIALS=(grass_01 rock ground_01 breakstone ground_crack_01 ice_crack ice bark water snow forest)
```

```bash
for i in "${!MATERIALS[@]}"; do
	m="${MATERIALS[$i]}"
	# Layer 10 (forest) is a recolour built by tools/convert_forest.sh; the pack has no
	# forest folder. Bark and water are skipped the same way by their own scripts' outputs.
	[ "$m" = "forest" ] && continue
```

(Bark and water are absent from the pack too, but today's script already fails on them, as the Fjords spec §7.4 records. This change does not touch that.)

- [ ] **Step 5: Add the table row**

In `extension/src/world/material_table.h`, append after the `snow` row, using the mean from Step 3:

```cpp
	// Under each conifer crown (shaders/stages/conifers.field.glslh): needle litter up close,
	// and -- through its top mip -- the canopy colour of a forest seen from kilometres away.
	// Built by tools/convert_forest.sh. flat_albedo is that texture's measured mean.
	{"forest",       "10",  1.2f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.12f, 0.19f, 0.13f}},
```

- [ ] **Step 6: Regenerate `shaders/material_table.glslh`**

Run: `cd extension && scons -Q test 2>&1 | grep -A40 "material_table.glslh" | head -60`
Expected: `test_material_glslh.cpp` fails and prints the correct file text. Write that text to `shaders/material_table.glslh` exactly as printed (the test's header comment says how). Then re-run `scons -Q test`.
Expected: all PASS, including the new forest case.

- [ ] **Step 7: Check the atlas loads layer 10**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_material_atlas.gd`
Expected: PASS. A layer count assertion fails if `MaterialAtlas` misses `10_*.png`.

- [ ] **Step 8: Commit**

```bash
git add tools/convert_forest.sh tools/convert_materials.sh assets/materials/10_*.png \
	extension/src/world/material_table.h shaders/material_table.glslh extension/tests/test_material_table.cpp
git commit -m "feat: a forest material recoloured from ground_foliage_01"
```

---

### Task 3: The conifers field stage and its CPU mirror

**Files:**
- Create: `shaders/stages/conifers.field.glslh`
- Modify: `extension/src/terrain/builtin_stages.cpp`, `assets/pipelines/fjords.pipeline`
- Test: `extension/tests/test_conifers_pipeline.cpp`; `tests/test_field_diff.gd` (existing; picks the stage up); `tests/test_conifers.gd` (new, stage half)

**Interfaces:**
- Consumes: Task 1's `conifer_params_make`, `conifer_field`, `ConiferHit`; Task 2's `MAT_FOREST` / `material_id("forest")`.
- Produces, in the generated Fjords field source (the cull in Task 6 calls these):
  - `ConiferParams conifers_params()`
  - `vec4 conifer_ground(vec2 xz)`
  - `#define CONIFERS_STAGE 1`
  - stage params `P.conifers_cell`, `P.conifers_density`, `P.conifers_height_min`, `P.conifers_height_max`, `P.conifers_trunk_radius`, `P.conifers_crown_radius`, `P.conifers_max_slope`, `P.conifers_treeline_margin`
  - resolved param names `"conifers.cell"` and the rest, in `ResolvedPipeline::params`

- [ ] **Step 1: Write the failing native test**

`extension/tests/test_conifers_pipeline.cpp`:

```cpp
#include <doctest/doctest.h>
#include "terrain/pipeline_load.h"
#include <fstream>
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
} // namespace

TEST_CASE("fjords resolves with the conifers stage after the bands, at an unchanged bound") {
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(repo_reader, root + "/assets/pipelines/fjords.pipeline",
			root + "/shaders/", &p, nullptr, &err), err);
	REQUIRE(p.stages.size() == 4);
	CHECK(p.stages[2].name == "fjord_bands");
	CHECK(p.stages[3].name == "conifers");
	// `mul 1.0`: the union of 1-Lipschitz trunks leaves the fjord field's 8.54 alone, and
	// conifer.glslh's CONIFER_FIELD_L (8.6) must not be below it.
	CHECK(p.lipschitz == doctest::Approx(8.54f).epsilon(0.002));
	CHECK(p.lipschitz <= 8.6f);
	bool cell = false;
	for (const ve::ParamDecl &d : p.params)
		if (d.name == "conifers.cell") cell = d.value == 8.0f;
	CHECK(cell);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: FAIL: `p.stages.size() == 4` is 3.

- [ ] **Step 3: Write the stage**

`shaders/stages/conifers.field.glslh`:

```glsl
//!stage     conifers
//!kind      field
//!in        sdf : float
//!in        height : float
//!in        material : uint
//!out       sdf : float
//!out       material : uint
//!param     cell : float = 8.0
//!param     density : float = 0.85
//!param     height_min : float = 18.0
//!param     height_max : float = 28.0
//!param     trunk_radius : float = 0.45
//!param     crown_radius : float = 3.0
//!param     max_slope : float = 1.7
//!param     treeline_margin : float = 40.0
//!use       fjord_height.water_y
//!use       fjord_bands.shore
//!use       fjord_bands.snow_line
//!use       fjord_bands.snow_jitter
//!use       fjord_bands.snow_ridge_drop
//!lipschitz mul 1.0
//!cpu       ve::stage_conifers

// Conifer trunks as real voxels, and the forest material under each crown
// (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md §4.3). Everything about where a
// conifer is and what shape it has lives in shaders/conifer.glslh, which the conifer cull, the
// card scatter, the imposter and the CPU mirror (ve::stage_conifers) all execute too.
//
// Bound: `mul 1.0`. conifer_field returns min(f, max(trunks, far bound)); the trunks are exact
// 1-Lipschitz round cones and the far bound is f scaled by 8.6 / 9.6 < 1, so neither raises
// the running 8.54 (the argument cave.field.glslh makes for its max).
//
// Runs AFTER fjord_bands, so the bands' material is known: the forest disc only replaces
// grass and breakstone, and a trunk is bark whatever ground it stands in.

// The guard the conifer cull compiles against (plan deviation 12): only a pipeline with this
// stage defines it, so the cull's body is empty everywhere else.
#define CONIFERS_STAGE 1

vec4 conifer_ground(vec2 xz) { return fjord_ground(xz); }

#include "conifer.glslh"

ConiferParams conifers_params() {
	return conifer_params_make(P.conifers_cell, P.conifers_density, P.conifers_height_min,
			P.conifers_height_max, P.conifers_trunk_radius, P.conifers_crown_radius,
			P.conifers_max_slope, P.conifers_treeline_margin, P.fjord_height_water_y,
			P.fjord_bands_shore, P.fjord_bands_snow_line, P.fjord_bands_snow_jitter,
			P.fjord_bands_snow_ridge_drop, SURFACE_Y);
}

void stage_conifers(inout FieldCtx ctx) {
	// A gradient's offset tap whose centre proved the trunks irrelevant (field_ops.glslh).
	if (g_field_skip_detail) return;
	// Deep inside the ground a trunk cannot change the sign, and that material is never seen.
	if (ctx.sdf < -2.0) return;
	bool want_disc = ctx.sdf <= 0.0 &&
			(ctx.material == MAT_GRASS_01 || ctx.material == MAT_BREAKSTONE);
	ConiferHit hit = conifer_field(ctx.p, ctx.sdf, SURFACE_Y + ctx.height, conifers_params(),
			want_disc, FIELD_DETAIL_MARGIN);
	if (hit.near) g_field_detail_near = true;
	bool lowered = hit.d < ctx.sdf;
	ctx.sdf = hit.d;
	if (lowered && hit.d <= 0.0) ctx.material = MAT_BARK;
	else if (!lowered && hit.forest) ctx.material = MAT_FOREST;
}
```

- [ ] **Step 4: Write the CPU mirror**

In `extension/src/terrain/builtin_stages.cpp`, add `#include <cstdint>` and `#include <cstring>` to the includes. Then add `kBandForest` to the anonymous namespace at the top:

```cpp
// The forest disc a conifer paints under its crown, matching MAT_FOREST.
constexpr uint16_t kBandForest = ve::material_id("forest");
```

Then, before the `VE_REGISTER_STAGE` block, add:

```cpp
VE_STAGE_SLOTS(Conifers, p, sdf, height, material);
VE_STAGE_PARAMS(Conifers, cell, density, height_min, height_max, trunk_radius, crown_radius,
		max_slope, treeline_margin, fjord_height_water_y, fjord_bands_shore, fjord_bands_snow_line,
		fjord_bands_snow_jitter, fjord_bands_snow_ridge_drop);

// shaders/conifer.glslh EXECUTED, not transcribed (plan deviation 1): the struct's
// conifer_ground() is the hook the header asks its includer for, here the host sector cache.
namespace conifer_mirror {
#include "terrain/glsl_shim.h"
struct Eval {
	const SectorCache *sectors = nullptr;
	float water_y = 0.0f;
	vec4 conifer_ground(vec2 xz) {
		const SectorGround g = sector_ground(sectors, water_y, xz.x, xz.y);
		return vec4(g.height, g.dhdx, g.dhdz, g.ridge);
	}
#include "../../../shaders/conifer.glslh"
};
} // namespace conifer_mirror

// Mirror of shaders/stages/conifers.field.glslh. 0.06 is FIELD_DETAIL_MARGIN; it only steers
// which cells the walk may skip, never the answer (a skipped cell cannot beat f).
void stage_conifers(FieldCtx &ctx, const ConifersSlots &s, const ConifersParams &p,
		const FieldResources &res) {
	namespace cm = conifer_mirror;
	const float f = ctx.f(s.sdf);
	if (f < -2.0f) return;
	cm::Eval ev;
	ev.sectors = res.sectors;
	ev.water_y = p.fjord_height_water_y;
	const auto cp = ev.conifer_params_make(p.cell, p.density, p.height_min, p.height_max,
			p.trunk_radius, p.crown_radius, p.max_slope, p.treeline_margin, p.fjord_height_water_y,
			p.fjord_bands_shore, p.fjord_bands_snow_line, p.fjord_bands_snow_jitter,
			p.fjord_bands_snow_ridge_drop, kSurfaceY);
	const uint16_t mat = static_cast<uint16_t>(ctx.f(s.material));
	const bool want_disc = f <= 0.0f && (mat == kBandGrass || mat == kBandBreakstone);
	const cm::vec3 pt(ctx.v(s.p)[0], ctx.v(s.p)[1], ctx.v(s.p)[2]);
	const auto hit = ev.conifer_field(pt, f, kSurfaceY + ctx.f(s.height), cp, want_disc, 0.06f);
	const bool lowered = hit.d < f;
	ctx.f(s.sdf) = hit.d;
	if (lowered && hit.d <= 0.0f) ctx.f(s.material) = float(kBandBark);
	else if (!lowered && hit.forest) ctx.f(s.material) = float(kBandForest);
}
```

and register it:

```cpp
VE_REGISTER_STAGE("ve::stage_conifers", Conifers, stage_conifers);
```

- [ ] **Step 5: Append the stage to `fjords.pipeline`**

After the `fjord_bands` block, at the end of `assets/pipelines/fjords.pipeline`:

```
# Conifer trunks and the forest floor (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md).
# `mul 1.0`: the computed bound stays 8.54. Placement and shape are this stage's params; the
# card scatter and imposter read them from the records the cull writes, so there is no
# second copy to keep in step.
stage stages/conifers.field.glslh
```

- [ ] **Step 6: Run the native suite**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: all PASS, including `fjords resolves with the conifers stage…`. `test_field_codegen_golden` must still pass, which proves the other pipelines' `field.glslh` is unchanged. `test_lipschitz_sampled.cpp` skips map-stage pipelines, so it does not cover Fjords; Task 1 does.

- [ ] **Step 7: Write the stage half of `tests/test_conifers.gd`**

```gdscript
extends GdUnitTestSuite

# Fjords conifers (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md). The stage half
# reads the CPU field through VoxelWorld.raycast; the pass half (Tasks 6-7) reads what the
# shipping passes wrote through debug_conifer_stats.
const FJORDS := "res://assets/pipelines/fjords.pipeline"
const WATER_Y := 51.2
# fjords.pipeline: shore 2 (+1), snow line 380 - jitter 25 - ridge drop 40 - margin 40.
const SHORE_E := 3.0
const TREELINE_E := 275.0
# A disc reaches 1.15 x 1.25 x 3 m from its foot; on a slope-1.7 wall that is 7.3 m of height.
const DISC_DROP := 8.0

var _world: VoxelWorld

func after_test() -> void:
	if is_instance_valid(_world):
		_world.free()

func _open(seed := 0) -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = FJORDS
	_world.world_seed = seed
	_world.use_local_device = true
	_world.physics_enabled = false
	_world.stream_radius_m = 2600.0
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	assert_int(_world.hooks().debug_pump_sectors(Vector3.ZERO, 1600.0, 900)).is_greater(0)

func _material(name: String) -> int:
	var names: Array = _world.material_table().map(func(m): return m["name"])
	return names.find(name) + 1

# Forest hits on a 61 x 61 grid at 25 m over +-750 m.
func _forest_hits() -> Array:
	var forest := _material("forest")
	var out := []
	for i in range(-30, 31):
		for j in range(-30, 31):
			var hit: Dictionary = _world.raycast(Vector3(i * 25.0, 800.0, j * 25.0), Vector3.DOWN, 1600.0)
			if hit["hit"] and int(hit["material"]) == forest:
				out.append(hit["pos"])
	return out

func test_the_forest_floor_appears_on_the_walls() -> void:
	_open()
	var hits := _forest_hits()
	assert_int(hits.size()).override_failure_message("no forest material in a 1.5 km grid").is_greater(20)

# Spec §4.1: no tree stands in the shore band or above the tree line. A disc spreads from its
# foot by up to DISC_DROP metres of height on the steepest permitted wall.
func test_the_forest_floor_stays_between_shore_and_tree_line() -> void:
	_open()
	for p in _forest_hits():
		var e: float = p.y - WATER_Y
		assert_float(e).override_failure_message("forest at e=%.1f, %s" % [e, p]).is_greater(SHORE_E - DISC_DROP)
		assert_float(e).override_failure_message("forest at e=%.1f, %s" % [e, p]).is_less(TREELINE_E + DISC_DROP)

# Trunks are bark and stand up out of the forest floor: a horizontal ray at 6 m above a
# forest hit, cast across its cell, meets bark somewhere in the grid.
func test_trunks_are_bark() -> void:
	_open()
	var bark := _material("bark")
	var seen := 0
	for p in _forest_hits():
		for dir in [Vector3.RIGHT, Vector3.LEFT, Vector3.FORWARD, Vector3.BACK]:
			var hit: Dictionary = _world.raycast(p + Vector3(0.0, 6.0, 0.0) - dir * 6.0, dir, 12.0)
			if hit["hit"] and int(hit["material"]) == bark:
				seen += 1
	assert_int(seen).override_failure_message("no ray met a trunk near any forest floor").is_greater(0)
```

- [ ] **Step 8: Generate the `.gd.uid` and run the GPU suites**

Run:
```bash
./build.sh --verify
./gdunit_tests.sh -a res://tests/test_conifers.gd
./gdunit_tests.sh -a res://tests/test_field_diff.gd
./gdunit_tests.sh -a res://tests/test_fjords.gd
```
Expected: all three PASS. `test_field_diff.gd` now diffs the GPU and CPU conifers stage at seed 0 and at a far seed. A mismatch means the shim and the GLSL disagree; look first for a double literal that is missing its `f` in `conifer.glslh`.

- [ ] **Step 9: Commit**

```bash
git add shaders/stages/conifers.field.glslh extension/src/terrain/builtin_stages.cpp \
	assets/pipelines/fjords.pipeline extension/tests/test_conifers_pipeline.cpp \
	tests/test_conifers.gd tests/test_conifers.gd.uid
git commit -m "feat: conifer trunks and the forest floor as a Fjords field stage"
```

---

### Task 4: The leaf raster takes inputs, and the palette moves into the params block

**Files:**
- Modify: `extension/src/leaves/leaf_layout.h` (LeafParams), `extension/src/leaves/leaf_layout.cpp`, `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh` (regenerated), `shaders/leaf.frag.glsl`, `extension/src/render/leaf_raster_pass.{h,cpp}`, `extension/src/render/leaf_scatter_pass.h`, `extension/src/render/frame.cpp`, `extension/src/debug/hooks_render.cpp`
- Test: `extension/tests/test_leaf_layout.cpp`; `tests/test_leaves.gd` and the golden-frame suites (existing)

**Interfaces:**
- Produces:
  - `struct LeafRasterInputs { RID instances; RID params; RID draw_args; int clump_count = 0; };` (in `leaf_raster_pass.h`, namespace `godot`)
  - `bool LeafRasterPass::draw(RenderingDevice *, const LeafRasterInputs &, GBuffer &, const Projection &, const float cam_pos[3])`
  - `LeafRasterInputs LeafScatterPass::raster_inputs() const`
  - `LeafParams::palette_top[4]`, `palette_under[4]` (272-byte block)
  - `inline constexpr float kLeafPaletteTop[3] = {0.52f, 0.66f, 0.24f}; kLeafPaletteUnder[3] = {0.12f, 0.26f, 0.19f};` in `leaf_layout.h`

- [ ] **Step 1: Change the layout test first**

In `extension/tests/test_leaf_layout.cpp`, replace the 256-byte case with:

```cpp
TEST_CASE("the params block is exactly 272 bytes") {
	// Seventeen vec4: std140 cannot pad a vec4-aligned run (plan deviation 9).
	CHECK(sizeof(ve::LeafParams) == 272);
}

TEST_CASE("the leaf palette is the colours leaf.frag.glsl shipped with") {
	ve::LeafSettings s;
	const ve::LeafLayout l = ve::leaf_layout(s, kOrigin, kIdentity, 0.0f, 0.0f);
	CHECK(l.params.palette_top[0] == 0.52f);
	CHECK(l.params.palette_top[1] == 0.66f);
	CHECK(l.params.palette_top[2] == 0.24f);
	CHECK(l.params.palette_under[0] == 0.12f);
	CHECK(l.params.palette_under[1] == 0.26f);
	CHECK(l.params.palette_under[2] == 0.19f);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile failure: `palette_top` is not a member.

- [ ] **Step 3: Grow the block**

In `extension/src/leaves/leaf_layout.h`, replace the `spare` member and its comment with:

```cpp
	// The card palette, moved out of leaf.frag.glsl's constants so the conifer cards can
	// share the raster (docs/superpowers/plans/2026-10-08-fjords-conifers.md deviation 9).
	// rgb used; w zero. leaf_layout() writes kLeafPaletteTop / kLeafPaletteUnder, which are
	// the constants the shader used to hold, so Default's canopies are the same at every pixel.
	float palette_top[4];
	float palette_under[4];
```

Update the asserts and the "sixteen vec4" comments to seventeen vec4 and 272 bytes:

```cpp
static_assert(sizeof(LeafParams) == 272, "LeafParams is a 272-byte std140 block");
static_assert(offsetof(LeafParams, palette_top) == 240, "LeafParams.palette_top");
static_assert(offsetof(LeafParams, palette_under) == 256, "LeafParams.palette_under");
```

(Delete the `spare` assert.) Above `struct LeafParams`, add:

```cpp
// The palette leaf.frag.glsl hard-coded until the conifer cards shared its raster.
inline constexpr float kLeafPaletteTop[3] = {0.52f, 0.66f, 0.24f};
inline constexpr float kLeafPaletteUnder[3] = {0.12f, 0.26f, 0.19f};
```

In `leaf_layout.cpp`, before `return l;`:

```cpp
	for (int i = 0; i < 3; i++) {
		p.palette_top[i] = kLeafPaletteTop[i];
		p.palette_under[i] = kLeafPaletteUnder[i];
	}
```

In `extension/src/gpu_layout/blocks.h`, replace `VE_LAYOUT_FIELD(LeafParams, spare, Vec4, 0),` with:

```cpp
	VE_LAYOUT_FIELD(LeafParams, palette_top, Vec4, 0),
	VE_LAYOUT_FIELD(LeafParams, palette_under, Vec4, 0),
```

- [ ] **Step 4: Regenerate `blocks.glslh` and read the palette in the shader**

Run: `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests > /dev/null; git diff --stat ../shaders/generated/`
Expected: only `shaders/generated/blocks.glslh` changes, with `LEAF_PARAMS_FIELDS` ending in `vec4 palette_top; vec4 palette_under;`. If `field.glslh.golden` shows up in the diff, stop and report.

In `shaders/leaf.frag.glsl`, replace:

```glsl
	const vec3 kTop = vec3(0.52, 0.66, 0.24);
	const vec3 kUnder = vec3(0.12, 0.26, 0.19);
	vec3 albedo = mix(kUnder, kTop, n.y * 0.5 + 0.5);
```

with:

```glsl
	// The palette is a param (LeafParams.palette_*): leaves upload the constants this line
	// used to hold, conifer cards their own dark blue-green.
	vec3 albedo = mix(leaf.palette_under.rgb, leaf.palette_top.rgb, n.y * 0.5 + 0.5);
```

- [ ] **Step 5: `LeafRasterInputs`**

In `extension/src/render/leaf_raster_pass.h`, replace the forward declaration `class LeafScatterPass;` with the struct, and change `draw` and `ensure_uniform_set`:

```cpp
// What the card raster reads: the clump instances, a LeafParams block (cam and reach for the
// fade, wind, style, palette) and the 16-byte indirect draw args. Leaves and conifer cards
// each hand one over, and each owns its own LeafRasterPass (plan deviation 8).
struct LeafRasterInputs {
	RID instances;
	RID params;
	RID draw_args;
	int clump_count = 0; // the last read-back count, for last_vertex_count()
};
```

```cpp
	bool draw(RenderingDevice *rd, const LeafRasterInputs &in, GBuffer &gb,
			const Projection &view_proj, const float cam_pos[3]);
```

```cpp
	bool ensure_uniform_set(RenderingDevice *rd, const LeafRasterInputs &in);
```

In `leaf_raster_pass.cpp`, drop `#include "render/leaf_scatter_pass.h"` and rewrite the two functions:

```cpp
bool LeafRasterPass::ensure_uniform_set(RenderingDevice *rd, const LeafRasterInputs &in) {
	gpu::RdDevice device{rd};
	return set_.get(device, group_, shader_, 0, {
			gpu::storage(0, in.instances),
			gpu::ubo(1, in.params)}).is_valid();
}

bool LeafRasterPass::draw(RenderingDevice *rd, const LeafRasterInputs &in, GBuffer &gb,
		const Projection &view_proj, const float cam_pos[3]) {
	last_vertex_count_ = 0;
	if (!rd_ || rd != rd_ || !shader_.is_valid() || !gb.is_valid()) return false;
	if (!in.instances.is_valid() || !in.draw_args.is_valid() || !in.params.is_valid())
		return true; // nothing placed: not a failure
	if (!ensure_pipeline(rd, gb)) return false;
	if (!ensure_uniform_set(rd, in)) return false;

	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set_.id(), 0);
	LeafRasterPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) push.view_proj[c * 4 + r] = view_proj.columns[c][r];
	push.cam[0] = cam_pos[0];
	push.cam[1] = cam_pos[1];
	push.cam[2] = cam_pos[2];
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	// One non-indexed indirect draw; the vertex count is whatever the scatter wrote.
	rd->draw_list_draw_indirect(dl, false, in.draw_args, 0, 1, 16);
	rd->draw_list_end();
	// Six vertices per clump: two triangles per card -- tracks the atomicMax in both scatters
	// and the corner decode in leaf.vert.glsl. A report, not a command.
	last_vertex_count_ = in.clump_count * 6;
	return true;
}
```

In `leaf_scatter_pass.h`, add `#include "render/leaf_raster_pass.h"` and, beside `params_buffer()`:

```cpp
	LeafRasterInputs raster_inputs() const {
		return {instances_, params_ubo_, draw_args_, last_clump_count_};
	}
```

- [ ] **Step 6: Update the two call sites**

`extension/src/render/frame.cpp`, in the leaves block:

```cpp
				&& leaf_raster && leaf_raster->draw(rd, leaf->raster_inputs(), *gb, view_proj, cam_pos);
```

`extension/src/debug/hooks_render.cpp`, in `debug_leaf_stats`:

```cpp
			leaf_raster->draw(device, l->raster_inputs(), *w->context().render->passes().gbuffer, view_proj, p);
```

Run `grep -rn "leaf_raster->draw\|->draw(rd, \*leaf\|draw(device, \*l" extension/src`.
Expected: only the two updated lines remain.

- [ ] **Step 7: Build, then run the native suite and the GPU suites that see leaves**

Run:
```bash
./build.sh && (cd extension && scons -Q test 2>&1 | tail -3)
./gdunit_tests.sh -a res://tests/test_leaves.gd
./gdunit_tests.sh -a res://tests 2>&1 | tee "$TMPDIR/conifers-task4.txt"
```
Expected: native all PASS, `test_leaves.gd` PASS, and the failure set in the full run equals Task 0's list. Any golden-frame suite newly failing means the palette move changed Default's pixels: stop and report, do not re-record.

- [ ] **Step 8: Commit**

```bash
git add extension/src/leaves/leaf_layout.h extension/src/leaves/leaf_layout.cpp \
	extension/src/gpu_layout/blocks.h shaders/generated/blocks.glslh shaders/leaf.frag.glsl \
	extension/src/render/leaf_raster_pass.h extension/src/render/leaf_raster_pass.cpp \
	extension/src/render/leaf_scatter_pass.h extension/src/render/frame.cpp \
	extension/src/debug/hooks_render.cpp extension/tests/test_leaf_layout.cpp
git commit -m "refactor: the leaf raster takes its inputs and its palette from the params block"
```

---

### Task 5: Conifer settings, layout and the pass parameter block

**Files:**
- Create: `extension/src/conifers/conifer_settings.h`, `conifer_settings.cpp`, `conifer_settings_store.h`, `conifer_layout.h`, `conifer_layout.cpp`
- Modify: `extension/SConstruct` (`pure_sources`), `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh` (regenerated), `extension/src/render/orchestrator.{h,cpp}` (store + group), `extension/src/voxel_world.{h,cpp}` (`set_conifer_value` / `get_conifer_value`)
- Test: `extension/tests/test_conifer_layout.cpp`

**Interfaces:**
- Produces:
  - `ve::ConiferSettings` (fields in Step 3), `ve::conifer_rows()`, `ve::clamp_conifer_settings(ConiferSettings *)`, `ve::ConiferSettingsStore`
  - `struct ve::ConiferPassParams` (208 bytes; GLSL `CONIFER_PASS_FIELDS`): `cam[4]` (xyz world camera, w card_reach), `planes[6][4]`, `cell_min[4]` (x, z, w = total cells), `cell_dim[4]` (x, z), `reach[4]` (x impostor_reach, y clump_radius_near, z tier_roundness, w crown_shade), `look[4]` (x shell_min), `limits[4]` (x max_clumps, y max_card_trees, z clumps_per_tree, w max_impostors), `flags[4]` (x raster_mode)
  - `struct ve::ConiferLayout { float cell_size_m; int dispatch_threads; ConiferPassParams params; LeafParams raster; }`
  - `ve::ConiferLayout ve::conifer_layout(const ConiferSettings &, float cell_m, const float camera[3], const float view_proj[16], float field_offset_x, float field_offset_z)`
  - `int ve::conifer_clump_budget(int clumps_per_tree, float card_reach_m, float distance_m)`
  - `RenderOrchestrator::conifer_settings()`, `set_conifer_value(const char *, float)`, `conifer_value(const char *)`; settings group `"conifers"`
  - `VoxelWorld.set_conifer_value(name, value)` / `get_conifer_value(name)` (GDScript)

- [ ] **Step 1: Write the failing test**

`extension/tests/test_conifer_layout.cpp`:

```cpp
#include <doctest/doctest.h>
#include "conifers/conifer_layout.h"
#include "conifers/conifer_settings_store.h"
#include <cmath>
#include <cstring>

namespace {
const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
const float kOrigin[3] = {0.0f, 0.0f, 0.0f};
} // namespace

TEST_CASE("conifer settings clamp every knob, NaN included, and idempotently") {
	ve::ConiferSettings s;
	s.card_reach_m = NAN;
	s.impostor_reach_m = 1.0e9f;
	s.clumps_per_tree = 4096;
	s.tier_roundness = -3.0f;
	ve::clamp_conifer_settings(&s);
	CHECK(std::isfinite(s.card_reach_m));
	CHECK(s.impostor_reach_m <= 4000.0f);
	CHECK(s.clumps_per_tree == 128);
	CHECK(s.tier_roundness == 0.0f);
	ve::ConiferSettings t = s;
	ve::clamp_conifer_settings(&t);
	CHECK(std::memcmp(&s, &t, sizeof(s)) == 0);
}

TEST_CASE("the conifer store round-trips a knob by name and clamps on write") {
	ve::ConiferSettingsStore store;
	CHECK(store.set_value("card_reach_m", 220.0f));
	CHECK(store.value("card_reach_m") == doctest::Approx(220.0f));
	store.set_value("card_reach_m", 1.0e9f);
	CHECK(store.value("card_reach_m") <= 1000.0f);
}

TEST_CASE("the conifer pass block is 208 bytes with cam first") {
	CHECK(sizeof(ve::ConiferPassParams) == 208);
	ve::ConiferSettings s;
	const float cam[3] = {12.0f, 34.0f, -56.0f};
	const ve::ConiferLayout l = ve::conifer_layout(s, 8.0f, cam, kIdentity, 0.0f, 0.0f);
	CHECK(l.params.cam[0] == 12.0f);
	CHECK(l.params.cam[2] == -56.0f);
	CHECK(l.params.cam[3] == s.card_reach_m);
	CHECK(l.params.reach[0] == s.impostor_reach_m);
}

TEST_CASE("the cell box covers the imposter reach in shifted space") {
	ve::ConiferSettings s;
	const float cam[3] = {100.0f, 0.0f, -40.0f};
	const ve::ConiferLayout l = ve::conifer_layout(s, 8.0f, cam, kIdentity, 1000.0f, -2000.0f);
	const float sx = 1100.0f, sz = -2040.0f;
	CHECK(float(l.params.cell_min[0]) * 8.0f <= sx - s.impostor_reach_m);
	CHECK(float(l.params.cell_min[0] + l.params.cell_dim[0]) * 8.0f >= sx + s.impostor_reach_m);
	CHECK(float(l.params.cell_min[2]) * 8.0f <= sz - s.impostor_reach_m);
	CHECK(float(l.params.cell_min[2] + l.params.cell_dim[2]) * 8.0f >= sz + s.impostor_reach_m);
	CHECK(l.dispatch_threads == l.params.cell_dim[0] * l.params.cell_dim[2]);
	CHECK(l.params.cell_min[3] == l.dispatch_threads);
}

TEST_CASE("a disabled layout dispatches nothing") {
	ve::ConiferSettings s;
	s.enabled = false;
	CHECK(ve::conifer_layout(s, 8.0f, kOrigin, kIdentity, 0.0f, 0.0f).dispatch_threads == 0);
}

TEST_CASE("the clump budget falls monotonically to an eighth at the card reach") {
	int prev = 1 << 30;
	for (int d = 0; d <= 300; d += 5) {
		const int b = ve::conifer_clump_budget(128, 300.0f, float(d));
		CHECK(b <= prev);
		CHECK(b >= 1);
		prev = b;
	}
	CHECK(ve::conifer_clump_budget(128, 300.0f, 0.0f) == 128);
	CHECK(ve::conifer_clump_budget(128, 300.0f, 300.0f) == 16);
}

TEST_CASE("the card raster block carries the conifer palette, reach, wind and style") {
	ve::ConiferSettings s;
	const ve::ConiferLayout l = ve::conifer_layout(s, 8.0f, kOrigin, kIdentity, 0.0f, 0.0f);
	CHECK(l.raster.cam[3] == s.card_reach_m); // leaf.frag's fade reads cam.w
	CHECK(l.raster.palette_top[1] == s.palette_top_g);
	CHECK(l.raster.palette_under[2] == s.palette_under_b);
	CHECK(l.raster.wind[0] == s.wind_strength);
	CHECK(l.raster.style[2] == s.leaf_grain);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile failure: `conifers/conifer_layout.h` not found.

- [ ] **Step 3: Settings**

Add `Glob("src/conifers/*.cpp") +` to the `pure_sources` list in `extension/SConstruct`, beside `Glob("src/leaves/*.cpp")`.

`extension/src/conifers/conifer_settings.h`:

```cpp
#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Conifer knobs (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md §9). Their own
// module and store, as leaves and grass are. Placement and shape are NOT here: they are the
// conifers stage's params in fjords.pipeline, because the field and the passes must agree.
struct ConiferSettings {
	bool enabled = true;
	// Cards draw out to here and dither out over its last fifth; imposters take exactly the
	// complementary pixels (plan deviation 5).
	float card_reach_m = 300.0f;
	// Imposters draw out to here and dither out over its last fifth; past it the forest
	// material's top mip is the forest.
	float impostor_reach_m = 2500.0f;
	int clumps_per_tree = 128;       // capped at the scatter's workgroup width
	int max_card_trees = 4096;
	int max_clumps = 300000;
	int max_impostors = 120000;
	float clump_radius_m = 1.0f;     // card radius at the nearest distance
	float shell_min = 0.8f;          // clumps sit between this and 1.0 of the tier profile
	float tier_roundness = 0.4f;     // conifer_normal's blend: 0 one cone, 1 every tier a skirt
	float crown_shade = 0.5f;        // how dark the crown base goes relative to its top
	float wind_strength = 0.12f;     // conifers are stiff
	float wind_speed = 0.6f;
	float wind_scale = 0.04f;
	float gloss = 0.1f;
	float hue_jitter = 0.08f;
	float leaf_grain = 6.0f;
	// Spec §5.4's starting palette; tuned against the capture.
	float palette_top_r = 0.20f, palette_top_g = 0.36f, palette_top_b = 0.22f;
	float palette_under_r = 0.05f, palette_under_g = 0.14f, palette_under_b = 0.13f;
};

std::span<const SettingRow<ConiferSettings>> conifer_rows();
void clamp_conifer_settings(ConiferSettings *s);

} // namespace ve
```

`extension/src/conifers/conifer_settings.cpp`:

```cpp
#include "conifers/conifer_settings.h"

namespace ve {
namespace {

const SettingRow<ConiferSettings> kConiferRows[] = {
	bool_row("enabled", "Conifers", &ConiferSettings::enabled),
	float_row("card_reach_m", "Card reach (m)", &ConiferSettings::card_reach_m, 0.0f, 1000.0f, 0.0f, 800.0f, 10.0f),
	float_row("impostor_reach_m", "Imposter reach (m)", &ConiferSettings::impostor_reach_m, 0.0f, 4000.0f, 0.0f, 4000.0f, 50.0f),
	int_row("clumps_per_tree", "Clumps per tree", &ConiferSettings::clumps_per_tree, 0, 128, 0, 128),
	int_row("max_card_trees", "Max card trees", &ConiferSettings::max_card_trees, 0, 65536, 0, 16384, 256),
	int_row("max_clumps", "Max clumps", &ConiferSettings::max_clumps, 0, 2000000, 0, 1000000, 10000),
	int_row("max_impostors", "Max imposters", &ConiferSettings::max_impostors, 0, 1000000, 0, 400000, 10000),
	float_row("clump_radius_m", "Clump radius (m)", &ConiferSettings::clump_radius_m, 0.0f, 4.0f, 0.0f, 2.0f, 0.05f),
	float_row("shell_min", "Shell inner (frac)", &ConiferSettings::shell_min, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("tier_roundness", "Tier roundness", &ConiferSettings::tier_roundness, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("crown_shade", "Crown self-shade", &ConiferSettings::crown_shade, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("wind_strength", "Wind strength (m)", &ConiferSettings::wind_strength, 0.0f, 3.0f, 0.0f, 1.5f, 0.05f),
	float_row("wind_speed", "Wind speed", &ConiferSettings::wind_speed, 0.0f, 8.0f, 0.0f, 3.0f, 0.1f),
	float_row("wind_scale", "Wind scale (1/m)", &ConiferSettings::wind_scale, 0.0f, 4.0f, 0.0f, 0.5f, 0.005f),
	float_row("gloss", "Gloss", &ConiferSettings::gloss, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("hue_jitter", "Hue jitter", &ConiferSettings::hue_jitter, 0.0f, 1.0f, 0.0f, 0.5f, 0.01f),
	float_row("leaf_grain", "Needle grain", &ConiferSettings::leaf_grain, 0.5f, 12.0f, 0.5f, 12.0f, 0.25f),
	float_row("palette_top_r", "Top R", &ConiferSettings::palette_top_r, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_top_g", "Top G", &ConiferSettings::palette_top_g, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_top_b", "Top B", &ConiferSettings::palette_top_b, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_under_r", "Under R", &ConiferSettings::palette_under_r, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_under_g", "Under G", &ConiferSettings::palette_under_g, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("palette_under_b", "Under B", &ConiferSettings::palette_under_b, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
};

} // namespace

std::span<const SettingRow<ConiferSettings>> conifer_rows() { return kConiferRows; }

void clamp_conifer_settings(ConiferSettings *s) { clamp_all<ConiferSettings>(kConiferRows, s); }

} // namespace ve
```

`extension/src/conifers/conifer_settings_store.h`:

```cpp
#pragma once
#include "conifers/conifer_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Conifers are their own module with their own store, as leaves are. No godot-cpp.
class ConiferSettingsStore : public SettingsStore<ConiferSettings> {
public:
	ConiferSettingsStore() :
			SettingsStore(conifer_rows(), nullptr, ConiferSettings{}) {}
};

} // namespace ve
```

- [ ] **Step 4: The layout**

`extension/src/conifers/conifer_layout.h`:

```cpp
#pragma once
#include "conifers/conifer_settings.h"
#include "leaves/leaf_layout.h" // LeafParams: the card raster's block
#include <cstddef>
#include <cstdint>

namespace ve {

// Uploaded to the conifer cull and card scatter; mirrored by CONIFER_PASS_FIELDS
// (shaders/generated/blocks.glslh). Thirteen vec4, cam first (the leaf convention).
struct ConiferPassParams {
	float cam[4];        // xyz world camera, w card_reach_m
	float planes[6][4];  // frustum planes, inward, normalised
	int32_t cell_min[4]; // x, z inclusive cell (SHIFTED space); w total cells
	int32_t cell_dim[4]; // x, z cell counts
	float reach[4];      // impostor_reach_m, clump_radius_m, tier_roundness, crown_shade
	float look[4];       // shell_min, unused...
	int32_t limits[4];   // max_clumps, max_card_trees, clumps_per_tree, max_impostors
	int32_t flags[4];    // x: raster mode (no sun march), the rest zero
};
static_assert(sizeof(ConiferPassParams) == 208, "ConiferPassParams is thirteen vec4");
static_assert(offsetof(ConiferPassParams, cam) == 0, "cam first");
static_assert(offsetof(ConiferPassParams, cell_min) == 112, "ConiferPassParams.cell_min");
static_assert(offsetof(ConiferPassParams, flags) == 192, "ConiferPassParams.flags");

struct ConiferLayout {
	float cell_size_m = 8.0f;
	int dispatch_threads = 0; // one cull thread per lattice cell in the box
	ConiferPassParams params{};
	LeafParams raster{};      // what the card raster and the imposter read for style
};

// cell_m is the resolved `conifers.cell` param. view_proj is column-major. The cell box is
// taken around camera + field offset (the lattice is in shifted space); params.cam stays the
// world camera. `settings` is clamped internally. raster.wind[3] (time) stays 0 -- run()'s.
ConiferLayout conifer_layout(const ConiferSettings &settings, float cell_m, const float camera[3],
		const float view_proj[16], float field_offset_x, float field_offset_z);

// Clumps a card tree at this distance gets: linear to an eighth at the card reach, as the
// leaf module's density LOD. conifer_pass.glslh's conifer_clump_budget is the GPU twin.
int conifer_clump_budget(int clumps_per_tree, float card_reach_m, float distance_m);

} // namespace ve
```

`extension/src/conifers/conifer_layout.cpp`:

```cpp
#include "conifers/conifer_layout.h"
#include <algorithm>
#include <cmath>

namespace ve {
namespace {

// Gribb-Hartmann, normalised inward. Kept local, as leaf_layout.cpp and grass_layout.cpp do,
// so the modules move independently.
void extract_planes(const float m[16], float out[6][4]) {
	auto row = [&m](int r, int c) { return m[c * 4 + r]; };
	const int sign[6] = {1, -1, 1, -1, 1, -1};
	const int axis[6] = {0, 0, 1, 1, 2, 2};
	for (int i = 0; i < 6; i++) {
		float p[4];
		for (int c = 0; c < 4; c++) p[c] = row(3, c) + float(sign[i]) * row(axis[i], c);
		const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		const float inv = len > 1e-8f ? 1.0f / len : 0.0f;
		for (int c = 0; c < 4; c++) out[i][c] = p[c] * inv;
	}
}

} // namespace

int conifer_clump_budget(int clumps_per_tree, float card_reach_m, float distance_m) {
	if (clumps_per_tree <= 0) return 0;
	const float t = card_reach_m > 0.0f ? std::clamp(distance_m / card_reach_m, 0.0f, 1.0f) : 1.0f;
	return std::max(1, int(std::lround(float(clumps_per_tree) * (1.0f - 0.875f * t))));
}

ConiferLayout conifer_layout(const ConiferSettings &settings, float cell_m, const float camera[3],
		const float view_proj[16], float field_offset_x, float field_offset_z) {
	ConiferSettings s = settings;
	clamp_conifer_settings(&s);
	ConiferLayout l;
	l.cell_size_m = cell_m;
	const float card = s.enabled ? s.card_reach_m : 0.0f;
	const float far = s.enabled ? std::max(s.impostor_reach_m, card) : 0.0f;

	ConiferPassParams &p = l.params;
	p.cam[0] = camera[0];
	p.cam[1] = camera[1];
	p.cam[2] = camera[2];
	p.cam[3] = card;
	extract_planes(view_proj, p.planes);
	if (far > 0.0f && cell_m > 0.0f) {
		const float sx = camera[0] + field_offset_x, sz = camera[2] + field_offset_z;
		const int lo_x = int(std::floor((sx - far) / cell_m));
		const int lo_z = int(std::floor((sz - far) / cell_m));
		const int hi_x = int(std::floor((sx + far) / cell_m));
		const int hi_z = int(std::floor((sz + far) / cell_m));
		p.cell_min[0] = lo_x;
		p.cell_min[2] = lo_z;
		p.cell_dim[0] = hi_x - lo_x + 1;
		p.cell_dim[2] = hi_z - lo_z + 1;
		l.dispatch_threads = p.cell_dim[0] * p.cell_dim[2];
		p.cell_min[3] = l.dispatch_threads;
	}
	p.reach[0] = far;
	p.reach[1] = s.clump_radius_m;
	p.reach[2] = s.tier_roundness;
	p.reach[3] = s.crown_shade;
	p.look[0] = s.shell_min;
	p.limits[0] = s.max_clumps;
	p.limits[1] = s.max_card_trees;
	p.limits[2] = s.clumps_per_tree;
	p.limits[3] = s.max_impostors;

	LeafParams &r = l.raster;
	r.cam[0] = camera[0];
	r.cam[1] = camera[1];
	r.cam[2] = camera[2];
	r.cam[3] = card; // leaf.frag.glsl fades cards over the last fifth of this
	r.wind[0] = s.wind_strength;
	r.wind[1] = s.wind_speed;
	r.wind[2] = s.wind_scale;
	r.style[0] = s.gloss;
	r.style[1] = s.hue_jitter;
	r.style[2] = s.leaf_grain;
	r.palette_top[0] = s.palette_top_r;
	r.palette_top[1] = s.palette_top_g;
	r.palette_top[2] = s.palette_top_b;
	r.palette_under[0] = s.palette_under_r;
	r.palette_under[1] = s.palette_under_g;
	r.palette_under[2] = s.palette_under_b;
	return l;
}

} // namespace ve
```

- [ ] **Step 5: The generated block**

In `extension/src/gpu_layout/blocks.h`, add `#include "conifers/conifer_layout.h"` beside the leaf include, then the field list (next to `kLeafParamsFields`) and the block row (next to the `LeafParams` row):

```cpp
inline constexpr Field kConiferPassParamsFields[] = {
	VE_LAYOUT_FIELD(ConiferPassParams, cam, Vec4, 0),
	VE_LAYOUT_FIELD(ConiferPassParams, planes, Vec4, 6),
	VE_LAYOUT_FIELD(ConiferPassParams, cell_min, IVec4, 0),
	VE_LAYOUT_FIELD(ConiferPassParams, cell_dim, IVec4, 0),
	VE_LAYOUT_FIELD(ConiferPassParams, reach, Vec4, 0),
	VE_LAYOUT_FIELD(ConiferPassParams, look, Vec4, 0),
	VE_LAYOUT_FIELD(ConiferPassParams, limits, IVec4, 0),
	VE_LAYOUT_FIELD(ConiferPassParams, flags, IVec4, 0),
};
```

```cpp
	VE_LAYOUT_BLOCK(ConiferPassParams, "CONIFER_PASS_FIELDS", kConiferPassParamsFields),
```

Run: `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests > /dev/null; git diff --stat ../shaders/generated/`
Expected: only `blocks.glslh` changes, gaining `#define CONIFER_PASS_FIELDS`.

- [ ] **Step 6: Wire the store into the orchestrator and `VoxelWorld`**

`extension/src/render/orchestrator.h`: add `#include "conifers/conifer_settings_store.h"`. Beside the leaf accessors, add:

```cpp
	ve::ConiferSettings conifer_settings() const { return conifer_settings_.get(); }
	bool set_conifer_value(const char *n, float v) { return conifer_settings_.set_value(n, v); }
	float conifer_value(const char *n) const { return conifer_settings_.value(n); }
```

Beside `ve::LeafSettingsStore leaf_settings_;`, add `ve::ConiferSettingsStore conifer_settings_;`.

`orchestrator.cpp` `settings_group`: add `if (std::strcmp(name, "conifers") == 0) return &conifer_settings_;` after the `leaves` line.

`extension/src/voxel_world.h`, beside `set_leaf_value`:

```cpp
	bool set_conifer_value(const String &name, float v);
	float get_conifer_value(const String &name) const;
```

`voxel_world.cpp`, beside the leaf bindings and methods:

```cpp
	ClassDB::bind_method(D_METHOD("set_conifer_value", "name", "value"), &VoxelWorld::set_conifer_value);
	ClassDB::bind_method(D_METHOD("get_conifer_value", "name"), &VoxelWorld::get_conifer_value);
```

```cpp
bool VoxelWorld::set_conifer_value(const String &name, float v) {
	return context_.render->set_conifer_value(name.utf8().get_data(), v);
}

float VoxelWorld::get_conifer_value(const String &name) const {
	return context_.render->conifer_value(name.utf8().get_data());
}
```

- [ ] **Step 7: Run the tests**

Run: `./build.sh && (cd extension && scons -Q test 2>&1 | tail -3)`
Expected: all PASS, including every case in `test_conifer_layout.cpp`.

- [ ] **Step 8: Commit**

```bash
git add extension/SConstruct extension/src/conifers extension/src/gpu_layout/blocks.h \
	shaders/generated/blocks.glslh extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp \
	extension/src/voxel_world.h extension/src/voxel_world.cpp extension/tests/test_conifer_layout.cpp
git commit -m "feat: conifer settings, layout and pass parameter block"
```

---

### Task 6: The conifer cull, card scatter and card raster

**Files:**
- Create: `shaders/conifer_pass.glslh`, `shaders/conifer_cull.comp.glsl`, `shaders/conifer_scatter.comp.glsl`, `extension/src/render/conifer_scatter_pass.{h,cpp}`
- Modify: `extension/src/render/orchestrator.{h,cpp}`, `extension/src/render/frame.{h,cpp}`, `extension/src/debug/hooks.h`, `extension/src/debug/hooks_render.cpp`, `extension/src/debug/hooks.cpp`
- Test: `tests/test_conifers.gd` (pass half)

**Interfaces:**
- Consumes: Task 1 (`conifer_*`), Task 3 (`conifers_params`, `conifer_ground`, `CONIFERS_STAGE`), Task 4 (`LeafRasterInputs`, `LeafRasterPass`), Task 5 (`ConiferLayout`, `ConiferPassParams`, `CONIFER_PASS_FIELDS`).
- Produces:
  - `struct ConiferRecord { vec4 a; vec4 b; }` and `uint conifer_clump_budget(float dist)` in `conifer_pass.glslh`
  - `class godot::ConiferScatterPass` with `initialize(rd)`, `teardown()`, `run(rd, GpuAtlas &, const ve::ConiferLayout &, const ve::RegionWindow &, float time_s, RID sun_ubo, const FieldContextSet *)`, `card_list_buffer()`, `impostor_list_buffer()`, `params_buffer()`, `raster_params_buffer()`, `impostor_draw_args_buffer()`, `raster_inputs()`, `read_back_counters(rd)`, `last_card_trees()`, `last_impostors()`, `last_clumps()`, `clump_high_water()`
  - `RenderPasses::conifer_scatter`, `RenderPasses::conifer_raster`, `RenderPasses::conifer_impostor` (the last is created in Task 7)
  - `bool VoxelFrame::draw_conifers(RenderingDevice *, GpuAtlas &, GBuffer &, const Projection &, const float cam_pos[3], float time_s, bool raster_mode)`
  - `Dictionary VoxelDebugHooks::debug_conifer_stats(Vector3 eye, Vector3 forward)` with keys `ran, card_trees, impostors, clumps, high_water, card_vertices, impostor_vertices, card_records`

- [ ] **Step 1: Write the failing GPU tests**

Append to `tests/test_conifers.gd`:

```gdscript
# The pass half. Stream around a forest floor hit, then ask the SHIPPING passes (driven
# through VoxelFrame::draw_conifers, the compositor's own block) what they placed.
func _forest_spot() -> Vector3:
	var hits := _forest_hits()
	assert_int(hits.size()).is_greater(0)
	return hits[hits.size() / 2]

func _stream_at(p: Vector3) -> void:
	_world.hooks().debug_pump_sectors(p, 900.0, 600)
	var quiet := 0
	for i in range(600):
		quiet = quiet + 1 if _world.hooks().debug_stream_frame(p + Vector3(0.0, 20.0, 0.0)) == 0 else 0
		if quiet >= 6:
			break

func test_the_conifer_pass_places_cards_near_a_forest() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	var d: Dictionary = _world.hooks().debug_conifer_stats(spot + Vector3(0.0, 60.0, 0.0), Vector3(0.0, -1.0, 0.001))
	assert_bool(d["ran"]).is_true()
	assert_int(d["card_trees"]).is_greater(0)
	assert_int(d["clumps"]).is_greater(0)
	assert_int(d["card_vertices"]).is_equal(int(d["clumps"]) * 6)

func test_conifer_settings_round_trip_and_disable() -> void:
	_open()
	_world.set_conifer_value("card_reach_m", 180.0)
	assert_float(_world.get_conifer_value("card_reach_m")).is_equal_approx(180.0, 0.001)
	var spot := _forest_spot()
	_stream_at(spot)
	_world.set_conifer_value("enabled", 0.0)
	var d: Dictionary = _world.hooks().debug_conifer_stats(spot + Vector3(0.0, 60.0, 0.0), Vector3(0.0, -1.0, 0.001))
	assert_int(d["card_trees"]).is_equal(0)
	assert_int(d["impostors"]).is_equal(0)
	assert_int(d["clumps"]).is_equal(0)

# Every listed card tree is within the card reach (plus a crown) of the camera.
func test_card_trees_lie_inside_the_card_reach() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	_world.set_conifer_value("card_reach_m", 120.0)
	var eye := spot + Vector3(0.0, 60.0, 0.0)
	var d: Dictionary = _world.hooks().debug_conifer_stats(eye, Vector3(0.0, -1.0, 0.001))
	var recs: PackedFloat32Array = d["card_records"]
	assert_int(recs.size()).is_greater(0)
	for i in range(recs.size() / 4):
		var foot := Vector3(recs[i * 4], recs[i * 4 + 1], recs[i * 4 + 2])
		var centre := foot + Vector3(0.0, recs[i * 4 + 3] * 0.5, 0.0)
		assert_float(centre.distance_to(eye)).is_less(120.0 + recs[i * 4 + 3] * 0.5 + 1.0)

# Spec §5.1 step 3: painting every listed trunk away drops it from the list. Paint, not dig,
# for the reason test_leaves.gd gives; the reach is pulled in so every listed tree is resident.
func test_painting_trunks_away_empties_the_card_list() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	_world.set_conifer_value("card_reach_m", 40.0)
	var eye := spot + Vector3(0.0, 30.0, 0.0)
	var d: Dictionary = _world.hooks().debug_conifer_stats(eye, Vector3(0.0, -1.0, 0.001))
	var recs: PackedFloat32Array = d["card_records"]
	assert_int(recs.size()).is_greater(0)
	for i in range(recs.size() / 4):
		var anchor := Vector3(recs[i * 4], recs[i * 4 + 1] + recs[i * 4 + 3] * 0.33, recs[i * 4 + 2])
		_world.hooks().debug_apply_sphere_paint(anchor, 1.5, 2) # material 2 is rock
	for i in range(40):
		_world.hooks().debug_stream_frame(spot + Vector3(0.0, 20.0, 0.0))
	assert_int(_world.hooks().debug_conifer_stats(eye, Vector3(0.0, -1.0, 0.001))["card_trees"]).is_equal(0)
```

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_conifers.gd`
Expected: the four new cases FAIL with `Invalid call. Nonexistent function 'debug_conifer_stats'`.

- [ ] **Step 2: `shaders/conifer_pass.glslh`**

```glsl
// What the conifer passes share besides shaders/conifer.glslh: the record, the parameter
// block and the clump budget.
//
// NOTE: never put a literal include directive inside a comment in this file -- the loader
// matches include tokens anywhere in a line and would self-include.

#include "generated/blocks.glslh"

// One culled tree, conifer_pack_a / conifer_pack_b with the foot in WORLD space. 32 bytes.
struct ConiferRecord {
	vec4 a;
	vec4 b;
};

// GPU twin of ve::conifer_clump_budget (extension/src/conifers/conifer_layout.cpp). The CPU
// function is the pinned one (test_conifer_layout.cpp); this must not drift from it.
uint conifer_clump_budget(int clumps_per_tree, float card_reach, float dist) {
	if (clumps_per_tree <= 0) return 0u;
	float t = card_reach > 0.0 ? clamp(dist / card_reach, 0.0, 1.0) : 1.0;
	return uint(max(1, int(round(float(clumps_per_tree) * (1.0 - 0.875 * t)))));
}
```

GLSL `round` is round-half-even on some drivers; `lround` is half-away. The budget is a float product that rarely lands on .5, and an off-by-one changes the clump count only, never correctness. Note this in the commit message rather than chasing it.

- [ ] **Step 3: `shaders/conifer_cull.comp.glsl`**

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "conifer_pass.glslh"

// One thread per cell of the camera-local lattice out to the imposter reach. Culls on
// distance, the grove gate and a column frustum test BEFORE reading any ground; then the
// ground gate, the chop check, and an append to the card list, the imposter list or both
// (the fade band). Spec §5.1.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { CONIFER_PASS_FIELDS } conifer;
layout(set = 0, binding = 1, std430) writeonly buffer CardList { ConiferRecord v[]; } card_list;
layout(set = 0, binding = 2, std430) writeonly buffer ImpostorList { ConiferRecord v[]; } impostor_list;
layout(set = 0, binding = 3, std430) buffer Counters { uint card_count; uint impostor_count;
		uint clump_count; uint high_water; } counters;
layout(set = 0, binding = 4, std430) buffer DispatchArgs { uint x; uint y; uint z; } disp;
layout(set = 0, binding = 5, std430) buffer ImpostorArgs { uint vertex_count; uint instance_count;
		uint first_vertex; uint first_instance; } impostor_args;
layout(set = 0, binding = 6, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 7, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 8, std430) readonly buffer RegionSlotCounts { int n[]; } region_slot_counts;
layout(set = 0, binding = 9) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 10) uniform usampler3D mat_atlas;
// brick_atlas.glslh addresses the window through `pc`, as in leaf_trees.comp.glsl.
layout(set = 0, binding = 11, std140) uniform Region { LEAF_REGION_FIELDS } pc;
layout(set = 0, binding = 12, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 13, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;

// The terrain pipeline's generated field source: conifers_params(), conifer_ground() and all
// of shaders/conifer.glslh, reading set 1's P exactly as the conifers stage does.
#define FIELD_OP_POOL_BINDING 14
#include "field.glslh"
#include "brick_atlas.glslh"

#ifdef CONIFERS_STAGE

// The tallest column a tree in this cell could occupy: the whole encoded height range plus a
// tree, CONIFER_R_MAX x tooth wide. Tested against the frustum before the ground is read.
bool conifer_column_culled(vec2 wxz, ConiferParams cp) {
	float lo = cp.water_y - SECTOR_HEIGHT_BELOW_M;
	float hi = lo + SECTOR_HEIGHT_SPAN_M + cp.height_max;
	vec3 c = vec3(wxz.x, 0.5 * (lo + hi), wxz.y);
	vec3 e = vec3(cp.crown_radius * CONIFER_R_MAX * CONIFER_TOOTH_MAX, 0.5 * (hi - lo),
			cp.crown_radius * CONIFER_R_MAX * CONIFER_TOOTH_MAX);
	for (int i = 0; i < 6; i++) {
		vec3 n = conifer.planes[i].xyz;
		if (dot(n, c) + conifer.planes[i].w < -dot(abs(n), e)) return true;
	}
	return false;
}

bool conifer_sphere_culled(vec3 c, float r) {
	for (int i = 0; i < 6; i++)
		if (dot(conifer.planes[i].xyz, c) + conifer.planes[i].w < -r) return true;
	return false;
}

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= uint(conifer.cell_min.w)) return;
	ivec2 cell = ivec2(conifer.cell_min.x + int(idx % uint(conifer.cell_dim.x)),
	                   conifer.cell_min.z + int(idx / uint(conifer.cell_dim.x)));
	ConiferParams cp = conifers_params();
	vec2 xz = conifer_cell_xz(cell, cp);
	vec2 wxz = xz - VE_FIELD_OFFSET.xz; // the lattice is SHIFTED space; the camera is world
	vec2 dxz = wxz - conifer.cam.xz;
	float slack = cp.height_max;        // a crown's half height and more
	float far = conifer.reach.x + slack;
	if (dot(dxz, dxz) > far * far) return;
	if (!conifer_cell_gate(cell, cp)) return;
	if (conifer_column_culled(wxz, cp)) return;

	vec4 g = conifer_ground(xz);
	float slope = length(g.yz);
	if (!conifer_ground_ok(cp.surface_y + g.x - cp.water_y, slope, cp)) return;
	Conifer c = conifer_at(cell, xz, cp.surface_y + g.x, slope, cp);
	c.foot -= VE_FIELD_OFFSET;

	float half_h = 0.5 * c.height;
	vec3 centre = c.foot + vec3(0.0, half_h, 0.0);
	if (conifer_sphere_culled(centre, half_h)) return;
	float dist = length(centre - conifer.cam.xyz);
	float card_reach = conifer.cam.w;
	bool card = card_reach > 0.0 && dist < card_reach + half_h;
	bool impostor = dist > 0.8 * card_reach - half_h && dist < conifer.reach.x + half_h;
	if (!card && !impostor) return;

	// THE chop check, verbatim from leaf_trees.comp.glsl: one read a third of the way up the
	// trunk, only where the atlas holds the region (beyond it the analytic tree is the answer).
	// It gates BOTH lists, so a felled tree leaves no imposter behind.
	vec3 anchor = c.foot + vec3(0.0, c.height * 0.33, 0.0);
	ivec3 anchor_brick = ivec3(floor(anchor / BRICK_SIZE));
	if (region_slot_of(anchor_brick) >= 0) {
		if (world_sdf(anchor) > 0.0) return;
		int anchor_slot = slot_at(anchor_brick);
		if (anchor_slot < 0) return;
		if (material_at(anchor, anchor_brick, anchor_slot) != MAT_BARK) return;
	}

	ConiferRecord r;
	r.a = conifer_pack_a(c);
	r.b = conifer_pack_b(c);
	if (card) {
		uint slot = atomicAdd(counters.card_count, 1u);
		if (slot < uint(conifer.limits.y)) {
			card_list.v[slot] = r;
			// One card-scatter workgroup per listed tree.
			atomicMax(disp.x, slot + 1u);
		} else {
			atomicMin(counters.card_count, uint(conifer.limits.y));
		}
	}
	if (impostor) {
		uint slot = atomicAdd(counters.impostor_count, 1u);
		if (slot < uint(conifer.limits.w)) {
			impostor_list.v[slot] = r;
			atomicMax(impostor_args.vertex_count, (slot + 1u) * 6u);
			impostor_args.instance_count = 1u;
		} else {
			atomicMin(counters.impostor_count, uint(conifer.limits.w));
		}
	}
}

#else

// This world's pipeline has no conifers stage (plan deviation 12). The pass is never created
// here; this body exists so the shader-reload preflight compiles.
void main() {}

#endif
```

Before writing it, run `grep -n "SECTOR_HEIGHT_BELOW_M\|SECTOR_HEIGHT_SPAN_M" shaders/sector.glslh`.
Expected: both constants are defined (lines 14–15). They reach the cull through `field.glslh`, which includes `sector.glslh` for Fjords.

- [ ] **Step 4: `shaders/conifer_scatter.comp.glsl`**

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "conifer_pass.glslh"

// One workgroup per card tree, one thread per candidate clump (spec §5.2). Writes the leaf
// module's 32-byte LeafClump, so LeafRasterPass draws conifer needles with its own shaders.
layout(local_size_x = 128) in;

layout(set = 0, binding = 0, std140) uniform Params { CONIFER_PASS_FIELDS } conifer;
layout(set = 0, binding = 1, std430) readonly buffer CardList { ConiferRecord v[]; } card_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint card_count; uint impostor_count;
		uint clump_count; uint high_water; } counters;
layout(set = 0, binding = 3, std430) buffer DrawArgs { uint vertex_count; uint instance_count;
		uint first_vertex; uint first_instance; } draw_args;
#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 5
#include "sun_light.glslh"
layout(set = 0, binding = 6, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 7, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 8, std430) readonly buffer RegionSlotCounts { int n[]; } region_slot_counts;
layout(set = 0, binding = 9) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 10) uniform usampler3D mat_atlas;
layout(set = 0, binding = 11, std140) uniform Region { LEAF_REGION_FIELDS } pc;
layout(set = 0, binding = 12, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 13, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;

#include "brick_atlas.glslh"
#include "sun_march.glslh"
#include "leaf.glslh"
layout(set = 0, binding = 4, std430) writeonly buffer Instances { LeafClump c[]; } instances;

// The record carries the whole shape; nothing here walks the field.
vec4 conifer_ground(vec2 xz) { return vec4(0.0); }
#include "conifer.glslh"

void main() {
	uint tree_idx = gl_WorkGroupID.x;
	if (tree_idx >= counters.card_count) return;
	ConiferRecord r = card_list.v[tree_idx];
	Conifer c = conifer_unpack(r.a, r.b);
	float dist = length(c.foot + vec3(0.0, 0.5 * c.height, 0.0) - conifer.cam.xyz);
	uint budget = conifer_clump_budget(conifer.limits.z, conifer.cam.w, dist);
	uint cand = gl_LocalInvocationID.x;
	if (cand >= budget) return;

	uint h = tree_hash(c.hash ^ (cand * 0x27D4EB2Fu));
	// Area-weighted crown height: a cone's surface per metre of height goes as (1 - s).
	float s = 1.0 - sqrt(tree_unit(h));
	float ang = tree_unit(tree_hash(h ^ 0x41u)) * 6.2831853;
	vec3 radial = vec3(cos(ang), 0.0, sin(ang));
	float rr = conifer_profile(c, s) * mix(conifer.look.x, 1.0, tree_unit(tree_hash(h ^ 0x42u)));
	vec3 p = c.foot + vec3(0.0, c.crown_base + s * (c.height - c.crown_base), 0.0) + radial * rr;
	vec3 n = conifer_normal(c, radial, conifer.reach.z);

	// Raster mode marches nothing: the sun map owns every pixel there (as leaves and grass).
	float sun = conifer.flags.x != 0 ? 1.0 : terrain_sun_visibility(p, RAY_SHADOW_DIST);
	sun *= mix(1.0 - conifer.reach.w, 1.0, s);
	float radius = conifer.reach.y * sqrt(float(conifer.limits.z) / max(float(budget), 1.0));
	float ratio = clamp(radius / max(c.R, 1e-3), 0.0, 1.0);

	uint slot = atomicAdd(counters.clump_count, 1u);
	atomicMax(counters.high_water, slot + 1u);
	if (slot >= uint(conifer.limits.x)) {
		atomicMin(counters.clump_count, uint(conifer.limits.x));
		return;
	}
	LeafClump o;
	o.a = vec4(p, radius);
	o.b = vec4(uintBitsToFloat(leaf_pack_normal(n, sun)), uintBitsToFloat(h),
			leaf_pack_pair(s, ratio), tree_unit(tree_hash(h ^ 0x51u)) * 6.2831853);
	instances.c[slot] = o;
	atomicMax(draw_args.vertex_count, (slot + 1u) * 6u);
	draw_args.instance_count = 1u;
}
```

- [ ] **Step 5: `ConiferScatterPass`**

`extension/src/render/conifer_scatter_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "conifers/conifer_layout.h"
#include "gpu_layout/blocks.h"
#include "render/async_readback.h"
#include "render/gpu/gpu.h"
#include "render/leaf_raster_pass.h"
#include "world/region_window.h"

namespace godot {

class GpuAtlas;
class FieldContextSet;

// The conifer cull (conifer_cull.comp.glsl) and card scatter (conifer_scatter.comp.glsl),
// modelled on LeafScatterPass: same shader loading, gpu::Group teardown, SetCache, explicit
// counter clearing and async counter readback. It always re-scatters (plan deviation 13).
class ConiferScatterPass {
public:
	~ConiferScatterPass();
	bool initialize(RenderingDevice *rd);
	void teardown();

	// False on any failure; the caller cancels the marker and skips conifers, never the frame.
	// field is the terrain pipeline's set 1 (the cull calls conifers_params()/fjord_ground).
	bool run(RenderingDevice *rd, GpuAtlas &atlas, const ve::ConiferLayout &layout,
			const ve::RegionWindow &region_win, float time_seconds, RID sun_ubo,
			const FieldContextSet *field);

	RID card_list_buffer() const { return card_list_; }
	RID impostor_list_buffer() const { return impostor_list_; }
	RID params_buffer() const { return params_ubo_; }
	RID raster_params_buffer() const { return raster_ubo_; }
	RID impostor_draw_args_buffer() const { return impostor_args_; }
	LeafRasterInputs raster_inputs() const { return {instances_, raster_ubo_, draw_args_, last_clumps_}; }

	// Synchronous re-read for the debug hook after its own submit+sync.
	void read_back_counters(RenderingDevice *rd);
	int last_card_trees() const { return last_card_trees_; }
	int last_impostors() const { return last_impostors_; }
	int last_clumps() const { return last_clumps_; }
	int clump_high_water() const { return clump_high_water_; }

private:
	void apply_counters(const PackedByteArray &data);
	bool ensure_buffers(RenderingDevice *rd, const ve::ConiferPassParams &p);
	bool ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo);
	void clear_args(RenderingDevice *rd);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program cull_, scatter_;
	RID params_ubo_, raster_ubo_, region_ubo_, field_ops_;
	RID card_list_, impostor_list_, counters_, dispatch_args_, draw_args_, impostor_args_, instances_;
	RID sampler_linear_, sampler_nearest_;
	gpu::SetCache cull_set_, scatter_set_;
	Ref<AsyncBufferRead> counters_read_;
	int max_clumps_ = 0, max_card_trees_ = 0, max_impostors_ = 0;
	int last_card_trees_ = 0, last_impostors_ = 0, last_clumps_ = 0, clump_high_water_ = 0;
	bool overflow_logged_ = false;
};

} // namespace godot
```

`extension/src/render/conifer_scatter_pass.cpp`:

```cpp
#include "render/conifer_scatter_pass.h"
#include "render/field_context_set.h"
#include "render/gpu_atlas.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

using namespace godot;

ConiferScatterPass::~ConiferScatterPass() { teardown(); }

bool ConiferScatterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	cull_ = gpu::compile_compute(rd, group_, "ConiferScatterPass", "conifer_cull.comp.glsl");
	scatter_ = gpu::compile_compute(rd, group_, "ConiferScatterPass", "conifer_scatter.comp.glsl");
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR, true);
	counters_read_.instantiate();
	if (!cull_.valid() || !scatter_.valid() || !sampler_linear_.is_valid() ||
			!sampler_nearest_.is_valid() || counters_read_.is_null()) {
		teardown();
		return false;
	}
	// Default-sized buffers up front, as LeafScatterPass does, so hooks see a capacity.
	ve::ConiferSettings defaults;
	ve::ConiferPassParams p{};
	p.limits[0] = defaults.max_clumps;
	p.limits[1] = defaults.max_card_trees;
	p.limits[3] = defaults.max_impostors;
	if (!ensure_buffers(rd, p)) {
		teardown();
		return false;
	}
	return true;
}

void ConiferScatterPass::teardown() {
	if (!rd_) return;
	if (counters_read_.is_valid()) counters_read_->drain(rd_);
	counters_read_ = Ref<AsyncBufferRead>();
	gpu::RdDevice device{rd_};
	group_.release(device);
	cull_ = scatter_ = gpu::Program();
	params_ubo_ = raster_ubo_ = region_ubo_ = field_ops_ = RID();
	card_list_ = impostor_list_ = counters_ = dispatch_args_ = draw_args_ = impostor_args_ = instances_ = RID();
	sampler_linear_ = sampler_nearest_ = RID();
	cull_set_ = scatter_set_ = gpu::SetCache();
	max_clumps_ = max_card_trees_ = max_impostors_ = 0;
	last_card_trees_ = last_impostors_ = last_clumps_ = clump_high_water_ = 0;
	overflow_logged_ = false;
	rd_ = nullptr;
}

bool ConiferScatterPass::ensure_buffers(RenderingDevice *rd, const ve::ConiferPassParams &p) {
	const int clumps = p.limits[0], cards = p.limits[1], imps = p.limits[3];
	if (clumps <= 0 || cards <= 0 || imps <= 0) return false;
	if (instances_.is_valid() && clumps == max_clumps_ && cards == max_card_trees_ && imps == max_impostors_)
		return true;
	if (counters_read_.is_valid()) counters_read_->drain(rd);
	gpu::RdDevice device{rd};
	for (RID *r : {&instances_, &card_list_, &impostor_list_, &counters_, &dispatch_args_, &draw_args_,
			&impostor_args_, &params_ubo_, &raster_ubo_, &region_ubo_, &field_ops_}) {
		group_.free(device, *r);
		*r = RID();
	}
	const auto indirect = RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT;
	instances_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(uint32_t(clumps) * 32u));
	card_list_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(uint32_t(cards) * 32u));
	impostor_list_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(uint32_t(imps) * 32u));
	counters_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u));
	dispatch_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(12u, PackedByteArray(), indirect));
	draw_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u, PackedByteArray(), indirect));
	impostor_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u, PackedByteArray(), indirect));
	params_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::ConiferPassParams)));
	raster_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::LeafParams)));
	region_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::GrassRegionBlock)));
	// One EditOp of zeroes: field.glslh declares the op pool; the cull never indexes it.
	field_ops_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(32u));
	max_clumps_ = clumps;
	max_card_trees_ = cards;
	max_impostors_ = imps;
	return instances_.is_valid() && card_list_.is_valid() && impostor_list_.is_valid() &&
			counters_.is_valid() && dispatch_args_.is_valid() && draw_args_.is_valid() &&
			impostor_args_.is_valid() && params_ubo_.is_valid() && raster_ubo_.is_valid() &&
			region_ubo_.is_valid() && field_ops_.is_valid();
}

bool ConiferScatterPass::ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo) {
	gpu::RdDevice device{rd};
	const RID cull = cull_set_.get(device, group_, cull_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, card_list_),
			gpu::storage(2, impostor_list_),
			gpu::storage(3, counters_),
			gpu::storage(4, dispatch_args_),
			gpu::storage(5, impostor_args_),
			gpu::storage(6, atlas.region_map()),
			gpu::storage(7, atlas.region_tables()),
			gpu::storage(8, atlas.region_slot_counts()),
			gpu::sampled(9, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(10, sampler_nearest_, atlas.mat_atlas()),
			gpu::ubo(11, region_ubo_),
			gpu::storage(12, atlas.palette()),
			gpu::storage(13, atlas.brick_flags()),
			gpu::storage(14, field_ops_)});
	if (!cull.is_valid()) return false;
	if (!sun_ubo.is_valid()) return true; // no sun: the cull runs, the scatter is skipped
	return scatter_set_.get(device, group_, scatter_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, card_list_),
			gpu::storage(2, counters_),
			gpu::storage(3, draw_args_),
			gpu::storage(4, instances_),
			gpu::ubo(5, sun_ubo),
			gpu::storage(6, atlas.region_map()),
			gpu::storage(7, atlas.region_tables()),
			gpu::storage(8, atlas.region_slot_counts()),
			gpu::sampled(9, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(10, sampler_nearest_, atlas.mat_atlas()),
			gpu::ubo(11, region_ubo_),
			gpu::storage(12, atlas.palette()),
			gpu::storage(13, atlas.brick_flags())}).is_valid();
}

void ConiferScatterPass::clear_args(RenderingDevice *rd) {
	PackedByteArray zero;
	zero.resize(16);
	zero.fill(0);
	rd->buffer_update(counters_, 0, 16, zero);
	rd->buffer_update(draw_args_, 0, 16, zero);
	rd->buffer_update(impostor_args_, 0, 16, zero);
	PackedByteArray seed;
	seed.resize(12);
	uint32_t *w = reinterpret_cast<uint32_t *>(seed.ptrw());
	w[0] = 0u; w[1] = 1u; w[2] = 1u;
	rd->buffer_update(dispatch_args_, 0, 12, seed);
}

bool ConiferScatterPass::run(RenderingDevice *rd, GpuAtlas &atlas, const ve::ConiferLayout &layout,
		const ve::RegionWindow &region_win, float time_seconds, RID sun_ubo,
		const FieldContextSet *field) {
	if (!rd_ || rd != rd_ || !cull_.valid() || !scatter_.valid()) return false;
	if (!field || !field->is_valid()) return false;
	if (layout.dispatch_threads <= 0) {
		// Disabled: clear every count and arg buffer so neither raster redraws frozen trees,
		// and drop any read still in flight. A successful no-op.
		if (counters_read_.is_valid()) counters_read_->take_fresh();
		last_card_trees_ = last_impostors_ = last_clumps_ = 0;
		clear_args(rd);
		return true;
	}
	if (!ensure_buffers(rd, layout.params)) return false;
	if (!ensure_uniform_sets(rd, atlas, sun_ubo)) return false;
	const bool scatter_ok = sun_ubo.is_valid() && atlas.region_slot_counts().is_valid();

	rd->buffer_update(params_ubo_, 0, sizeof(layout.params), gpu::push_bytes(layout.params));
	ve::LeafParams raster = layout.raster;
	raster.wind[3] = time_seconds; // time is run()'s to write, as LeafScatterPass's
	rd->buffer_update(raster_ubo_, 0, sizeof(raster), gpu::push_bytes(raster));
	const ve::IVec3 ab = atlas.config().atlas_bricks;
	const ve::GrassRegionBlock region{{region_win.dim, region_win.dim, region_win.dim, 0},
			{region_win.origin.x, region_win.origin.y, region_win.origin.z, 0},
			{ab.x, ab.y, ab.z, 0}};
	rd->buffer_update(region_ubo_, 0, sizeof(region), gpu::push_bytes(region));
	clear_args(rd);

	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, cull_.pipeline);
	rd->compute_list_bind_uniform_set(list, cull_set_.id(), 0);
	field->bind(rd, list);
	rd->compute_list_dispatch(list, (layout.dispatch_threads + 63) / 64, 1, 1);
	if (scatter_ok) {
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, scatter_.pipeline);
		rd->compute_list_bind_uniform_set(list, scatter_set_.id(), 0);
		rd->compute_list_dispatch_indirect(list, dispatch_args_, 0);
	}
	rd->compute_list_end();
	if (counters_read_.is_valid()) {
		if (counters_read_->take_fresh()) apply_counters(counters_read_->data());
		counters_read_->request(rd, counters_, 0, 16);
	}
	return true;
}

void ConiferScatterPass::read_back_counters(RenderingDevice *rd) {
	if (counters_read_.is_valid()) {
		counters_read_->drain(rd);
		counters_read_->take_fresh();
	}
	apply_counters(rd->buffer_get_data(counters_, 0, 16));
}

void ConiferScatterPass::apply_counters(const PackedByteArray &data) {
	if (data.size() < 16) return;
	const uint32_t *c = reinterpret_cast<const uint32_t *>(data.ptr());
	last_card_trees_ = int(std::min<uint32_t>(c[0], uint32_t(max_card_trees_)));
	last_impostors_ = int(std::min<uint32_t>(c[1], uint32_t(max_impostors_)));
	last_clumps_ = int(std::min<uint32_t>(c[2], uint32_t(max_clumps_)));
	clump_high_water_ = std::max(clump_high_water_, int(c[3]));
	if (int(c[3]) > max_clumps_ && !overflow_logged_) {
		overflow_logged_ = true;
		UtilityFunctions::printerr("ConiferScatterPass: clump buffer overflow, wanted ", int(c[3]),
				" of ", max_clumps_, "; raise max_clumps or lower clumps_per_tree.");
	}
}
```

- [ ] **Step 6: Create the passes in the orchestrator**

`orchestrator.h`: forward-declare `class ConiferScatterPass; class ConiferImpostorPass;`. Add these to the passes struct beside `leaf_raster`:

```cpp
	ConiferScatterPass *conifer_scatter = nullptr;
	LeafRasterPass *conifer_raster = nullptr;     // a second card raster (plan deviation 8)
	ConiferImpostorPass *conifer_impostor = nullptr; // Task 7
```

`orchestrator.cpp`: add `#include "render/conifer_scatter_pass.h"`. After the leaf block in `ensure_gpu_graph()`:

```cpp
	// Conifers (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md): only in a world
	// whose pipeline has the conifers stage, whose generated field source the cull compiles
	// against. Fail-soft: without the passes, trunks and the forest floor still render.
	bool has_conifers = false;
	for (const ve::StageManifest &s : handles_.store->terrain_pipeline().stages)
		if (s.name == "conifers") has_conifers = true;
	if (has_conifers) {
		passes_.conifer_scatter = new ConiferScatterPass();
		if (!passes_.conifer_scatter->initialize(device)) {
			UtilityFunctions::printerr("VoxelWorld: conifer initialization failed; continuing "
					"without conifer crowns (safe fail-soft: trunks and forest floor stand)");
			delete passes_.conifer_scatter;
			passes_.conifer_scatter = nullptr;
		} else {
			passes_.conifer_raster = new LeafRasterPass();
			passes_.conifer_raster->initialize(device);
		}
	}
```

In `teardown_render_passes()`, beside the leaf deletes (before the atlas, as the leaf passes are):

```cpp
	if (passes_.conifer_raster) { delete passes_.conifer_raster; passes_.conifer_raster = nullptr; }
	if (passes_.conifer_scatter) { delete passes_.conifer_scatter; passes_.conifer_scatter = nullptr; }
```

Also add `if (LeafRasterPass *cr = render_.passes().conifer_raster) cr->release_targets();` beside the leaf `release_targets()` call in `frame.cpp` (line ~781).

- [ ] **Step 7: `VoxelFrame::draw_conifers` and the compositor block**

`frame.h`: add `#include "conifers/conifer_layout.h"` and the declarations:

```cpp
	ve::ConiferLayout conifer_layout(const float cam_pos[3], const float view_proj[16]) const;
	// The conifer block: cull, card scatter, card raster (and Task 7's imposter raster).
	// Called by the compositor and by debug_conifer_stats -- one code path (plan deviation 14).
	// False on failure; the caller cancels the timing marker.
	bool draw_conifers(RenderingDevice *rd, GpuAtlas &atlas, GBuffer &gb,
			const Projection &view_proj, const float cam_pos[3], float time_s, bool raster_mode);
```

`frame.cpp`: add `#include "render/conifer_scatter_pass.h"`, then:

```cpp
ve::ConiferLayout VoxelFrame::conifer_layout(const float cam_pos[3], const float view_proj[16]) const {
	const ve::ResolvedPipeline &tp = store_.terrain_pipeline();
	// The lattice pitch is the stage's own resolved param, never a duplicated constant.
	float cell = 8.0f;
	for (const ve::ParamDecl &d : tp.params)
		if (d.name == "conifers.cell") cell = d.value;
	return ve::conifer_layout(render_.conifer_settings(), cell, cam_pos, view_proj,
			static_cast<float>(tp.field_offset_x), static_cast<float>(tp.field_offset_z));
}

bool VoxelFrame::draw_conifers(RenderingDevice *rd, GpuAtlas &atlas, GBuffer &gb,
		const Projection &view_proj, const float cam_pos[3], float time_s, bool raster_mode) {
	ConiferScatterPass *pass = render_.passes().conifer_scatter;
	if (!pass) return false;
	float vp[16];
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) vp[c * 4 + r] = view_proj.columns[c][r];
	ve::ConiferLayout l = conifer_layout(cam_pos, vp);
	l.params.flags[0] = raster_mode ? 1 : 0;
	SunUbo *sun = render_.passes().sun_ubo;
	if (!pass->run(rd, atlas, l, store_.region_window(), time_s, sun ? sun->buffer() : RID(),
			render_.passes().field_context))
		return false;
	LeafRasterPass *cards = render_.passes().conifer_raster;
	return cards && cards->draw(rd, pass->raster_inputs(), gb, view_proj, cam_pos);
}
```

In the compositor, right after the leaves block:

```cpp
	// Conifers: one gated block beside the leaves, the same shape. Cards and imposters write
	// the G-buffer channels the far field writes, so the beauty stack shades them unchanged.
	if (render_.passes().conifer_scatter) {
		timings->begin(rd, "conifers");
		if (draw_conifers(rd, *atlas, *gb, view_proj, cam_pos,
				static_cast<float>(render_.beauty_frame()) / 60.0f, settings.raster_mode))
			timings->end(rd, "conifers");
		else
			timings->cancel("conifers");
	}
```

- [ ] **Step 8: `debug_conifer_stats`**

`hooks.h`: declare `Dictionary debug_conifer_stats(Vector3 eye, Vector3 forward);`. In `hooks.cpp`'s `_bind_methods`, beside `debug_leaf_stats`:

```cpp
	ClassDB::bind_method(D_METHOD("debug_conifer_stats", "eye", "forward"), &VoxelDebugHooks::debug_conifer_stats);
```

`hooks_render.cpp`: add `#include "render/conifer_scatter_pass.h"`, then:

```cpp
// What the SHIPPING conifer passes placed and drew for a camera at eye looking along forward.
// On a local-device world the hook calls VoxelFrame::draw_conifers -- the compositor's own
// block -- into the probe-size G-buffer, then reads the counters back (plan deviation 14).
// Demo worlds stay pure-read.
Dictionary VoxelDebugHooks::debug_conifer_stats(Vector3 eye, Vector3 forward) {
	Dictionary d;
	d["ran"] = false;
	d["card_trees"] = 0;
	d["impostors"] = 0;
	d["clumps"] = 0;
	d["high_water"] = 0;
	d["card_vertices"] = 0;
	d["impostor_vertices"] = 0;
	d["card_records"] = PackedFloat32Array();
	VoxelWorld *w = world_;
	if (!w) return d;
	ConiferScatterPass *pass = w->context().render->passes().conifer_scatter;
	if (!pass) return d;
	RenderingDevice *device = w->rd();
	if (w->get_use_local_device()) {
		w->ensure_initialized();
		GpuAtlas *atlas = w->context().render->passes().atlas;
		GBuffer *gb = w->context().render->passes().gbuffer;
		SunUbo *sun = w->context().render->passes().sun_ubo;
		if (!w->is_initialized() || !device || !atlas || !atlas->is_valid() || !gb || !sun ||
				!sun->ensure(device) || !gb->ensure(device, nullptr, Vector2i(64, 64)))
			return d;
		const float p[3] = {eye.x, eye.y, eye.z};
		const Vector3 fn = forward.normalized();
		const float f[3] = {fn.x, fn.y, fn.z};
		const ve::ProbeCamera pc = ve::probe_camera(p, f, 64, 64, 1.5707963268f, 0.1f, 4000.0f);
		Projection view_proj;
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) view_proj.columns[c][r] = pc.lod.view_proj[c * 4 + r];
		if (w->context().render->passes().lod_raster)
			w->context().render->passes().lod_raster->clear_targets(device, *gb);
		const bool ok = w->context().render->frame().draw_conifers(device, *atlas, *gb, view_proj, p,
				static_cast<float>(w->context().render->beauty_frame()) / 60.0f, false);
		device->submit();
		device->sync();
		if (!ok) return d;
		pass->read_back_counters(device);
	}
	d["ran"] = true;
	d["card_trees"] = pass->last_card_trees();
	d["impostors"] = pass->last_impostors();
	d["clumps"] = pass->last_clumps();
	d["high_water"] = pass->clump_high_water();
	if (LeafRasterPass *cr = w->context().render->passes().conifer_raster)
		d["card_vertices"] = cr->last_vertex_count();
	// foot x, y, z and height per listed card tree, read from the list the shipping cull wrote.
	const int n = std::min(pass->last_card_trees(), 256);
	if (device && n > 0) {
		const PackedByteArray raw = device->buffer_get_data(pass->card_list_buffer(), 0, uint32_t(n) * 32u);
		const float *v = reinterpret_cast<const float *>(raw.ptr());
		PackedFloat32Array recs;
		recs.resize(n * 4);
		for (int i = 0; i < n; i++) {
			recs[i * 4 + 0] = v[i * 8 + 0];
			recs[i * 4 + 1] = v[i * 8 + 1];
			recs[i * 4 + 2] = v[i * 8 + 2];
			recs[i * 4 + 3] = v[i * 8 + 4]; // b.x is the height
		}
		d["card_records"] = recs;
	}
	return d;
}
```

Check: `grep -n "struct ProbeCamera\|probe_camera(" extension/src/debug/*.h extension/src/*/*.h | head -3` must show the same `probe_camera(p, f, w, h, fov, near, far)` signature `debug_leaf_stats` uses. If the include for it is missing in `hooks_render.cpp`, it is already there for `debug_leaf_stats`.

- [ ] **Step 9: Run the tests**

Run:
```bash
./build.sh && ./gdunit_tests.sh -a res://tests/test_conifers.gd
./gdunit_tests.sh -a res://tests/test_leaves.gd
```
Expected: all `test_conifers.gd` cases PASS, and `test_leaves.gd` is unchanged (PASS).

- [ ] **Step 10: Check shader reload on a Default world**

Run: `./gdunit_tests.sh -a res://tests 2>&1 | tee "$TMPDIR/conifers-task6.txt"`
Expected: the failure set equals Task 0's list. A shader-reload suite failing on `conifer_cull.comp.glsl` means the `#ifdef CONIFERS_STAGE` guard is not taking effect.

- [ ] **Step 11: Commit**

```bash
git add shaders/conifer_pass.glslh shaders/conifer_cull.comp.glsl shaders/conifer_scatter.comp.glsl \
	extension/src/render/conifer_scatter_pass.h extension/src/render/conifer_scatter_pass.cpp \
	extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp \
	extension/src/render/frame.h extension/src/render/frame.cpp \
	extension/src/debug/hooks.h extension/src/debug/hooks.cpp extension/src/debug/hooks_render.cpp \
	tests/test_conifers.gd
git commit -m "feat: the conifer cull and needle cards"
```

---

### Task 7: Imposters

**Files:**
- Create: `shaders/conifer_impostor.vert.glsl`, `shaders/conifer_impostor.frag.glsl`, `extension/src/render/conifer_impostor_pass.{h,cpp}`
- Modify: `extension/src/render/orchestrator.cpp`, `extension/src/render/frame.cpp`, `extension/src/debug/hooks_render.cpp`
- Test: `extension/tests/test_conifer_shader.cpp` (fade complement), `tests/test_conifers.gd` (imposter cases)

**Interfaces:**
- Consumes: `ConiferScatterPass::impostor_list_buffer()`, `params_buffer()`, `raster_params_buffer()`, `impostor_draw_args_buffer()`, `last_impostors()`; `conifer_unpack`, `conifer_ray_hit`, `conifer_normal`, `conifer_s`.
- Produces: `class godot::ConiferImpostorPass` with `initialize(rd)`, `teardown()`, `release_targets()`, `draw(rd, ConiferScatterPass &, GBuffer &, const Projection &, const float cam_pos[3])`, `last_vertex_count()`; `float conifer_card_fade(float dist, float reach)` in `conifer.glslh`.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_conifer_shader.cpp`:

```cpp
// Review Focus 2: leaf.frag.glsl discards a card where bayer < fade; the imposter keeps a
// pixel only where bayer < fade. Same expression, so every pixel in the band is one or the
// other, never both, never neither.
TEST_CASE("the card fade and the imposter keep are exact complements across the band") {
	cs::Eval ev;
	for (int k = 0; k < 16; k++) {
		const float bayer = float(k) / 16.0f;
		for (float dist = 200.0f; dist <= 320.0f; dist += 0.5f) {
			const float fade = ev.conifer_card_fade(dist, 300.0f);
			const bool card_drawn = !(fade > 0.0f && bayer < fade);
			const bool impostor_drawn = bayer < fade;
			CHECK(card_drawn != impostor_drawn);
		}
	}
}
```

Append to `tests/test_conifers.gd`:

```gdscript
# Spec §6: from high above a forest, past the card reach, imposters draw.
func test_an_aerial_view_draws_impostors() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	var d: Dictionary = _world.hooks().debug_conifer_stats(spot + Vector3(-400.0, 450.0, 0.0), Vector3(1.0, -1.0, 0.0))
	assert_int(d["impostors"]).is_greater(0)
	assert_int(d["impostor_vertices"]).is_equal(int(d["impostors"]) * 6)

func test_impostors_stop_at_their_reach() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	var eye := spot + Vector3(-400.0, 450.0, 0.0)
	var many: int = _world.hooks().debug_conifer_stats(eye, Vector3(1.0, -1.0, 0.0))["impostors"]
	_world.set_conifer_value("impostor_reach_m", 400.0)
	var few: int = _world.hooks().debug_conifer_stats(eye, Vector3(1.0, -1.0, 0.0))["impostors"]
	assert_int(few).is_less(many)
```

Run: `cd extension && scons -Q test 2>&1 | tail -3` and `./build.sh && ./gdunit_tests.sh -a res://tests/test_conifers.gd`
Expected: the native build fails because `conifer_card_fade` does not exist; the GPU cases fail because `impostor_vertices` is 0.

- [ ] **Step 2: Add the shared fade to `conifer.glslh`**

Add before `conifer_inside`:

```glsl
// The card fade leaf.frag.glsl applies over the last fifth of the card reach, as a function,
// so the imposter can take exactly the complementary pixels (plan deviation 5). leaf.frag
// keeps its own copy of this expression; test_conifer_shader.cpp pins the complement.
float conifer_card_fade(float dist, float reach) {
	return clamp((dist - reach * 0.8f) / max(reach * 0.2f, 1e-3f), 0.0f, 1.0f);
}
```

Run: `cd extension && scons -Q test 2>&1 | tail -3`
Expected: PASS.

- [ ] **Step 3: `shaders/conifer_impostor.vert.glsl`**

```glsl
#[vertex]
#version 460

#include "common.glslh"
#include "conifer_pass.glslh"

// Pulled geometry, as leaf.vert.glsl: gl_VertexIndex / 6 is the tree, % 6 the corner.
layout(set = 0, binding = 0, std430) readonly buffer ImpostorList { ConiferRecord v[]; } list;
layout(set = 0, binding = 1, std140) uniform Params { CONIFER_PASS_FIELDS } conifer;
layout(push_constant, std430) uniform Push { mat4 view_proj; vec4 cam; } pc;

vec4 conifer_ground(vec2 xz) { return vec4(0.0); }
#include "conifer.glslh"

layout(location = 0) out vec3 v_world;
// FLAT: a.w is the hash's bit pattern; interpolating it would change the hash (leaf.vert's rule).
layout(location = 1) flat out vec4 v_a;
layout(location = 2) flat out vec4 v_b;

void main() {
	uint tree = uint(gl_VertexIndex) / 6u;
	uint corner = uint(gl_VertexIndex) % 6u;
	ConiferRecord r = list.v[tree];
	Conifer c = conifer_unpack(r.a, r.b);
	v_a = r.a;
	v_b = r.b;

	float half_h = 0.5 * (c.height - c.crown_base);
	vec3 mid = c.foot + vec3(0.0, c.crown_base + half_h, 0.0);
	float rad = c.R * CONIFER_TOOTH_MAX;
	vec3 to_cam = normalize(pc.cam.xyz - mid);
	// Right is horizontal, up is the cylinder axis projected off the view direction. The
	// cross collapses straight above or below a tree, so fall back to world X (leaf.vert).
	vec3 axis = cross(vec3(0.0, 1.0, 0.0), to_cam);
	vec3 right = normalize(dot(axis, axis) > 1e-6 ? axis : vec3(1.0, 0.0, 0.0));
	vec3 up = cross(to_cam, right);
	// The bounding cylinder's extent along `up`: its axis contributes half_h * |up.y|, its
	// radius the rest. 10% margin covers perspective at the >= 240 m these draw from.
	float ext_up = 1.1 * (half_h * abs(up.y) + rad * sqrt(max(0.0, 1.0 - up.y * up.y)));
	float ext_right = 1.1 * rad;

	const vec2 kCorners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1),
	                                 vec2(-1, 1), vec2(1, -1), vec2(1, 1));
	vec2 q = kCorners[corner];
	vec3 world = mid + right * (q.x * ext_right) + up * (q.y * ext_up);
	v_world = world;
	gl_Position = pc.view_proj * vec4(world, 1.0);
}
```

- [ ] **Step 4: `shaders/conifer_impostor.frag.glsl`**

```glsl
#[fragment]
#version 460
#include "generated/gbuffer.glslh"
#include "generated/blocks.glslh"

#include "common.glslh"
#include "shade.glslh"
#include "conifer_pass.glslh"

layout(set = 0, binding = 1, std140) uniform Params { CONIFER_PASS_FIELDS } conifer;
// The card raster's block: the palette and style the cards use, so both bands match.
layout(set = 0, binding = 2, std140) uniform LeafStyle { LEAF_PARAMS_FIELDS } leaf;
layout(push_constant, std430) uniform Push { mat4 view_proj; vec4 cam; } pc;

vec4 conifer_ground(vec2 xz) { return vec4(0.0); }
#include "conifer.glslh"

layout(location = 0) in vec3 v_world;
layout(location = 1) flat in vec4 v_a;
layout(location = 2) flat in vec4 v_b;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss

void main() {
	Conifer c = conifer_unpack(v_a, v_b);
	vec3 ro = pc.cam.xyz;
	vec3 rd = normalize(v_world - ro);
	float t = conifer_ray_hit(c, ro, rd);
	if (t < 0.0) discard;
	vec3 hit = ro + rd * t;

	// Exactly the pixels leaf.frag.glsl drops from the cards in the hand-off band, and none
	// nearer; then the imposter's own dither-out over the last fifth of its reach.
	float b = bayer4(ivec2(gl_FragCoord.xy));
	if (!(b < conifer_card_fade(t, conifer.cam.w))) discard;
	float out_fade = conifer_card_fade(t, conifer.reach.x);
	if (out_fade > 0.0 && b < out_fade) discard;

	vec3 radial = vec3(hit.x - c.foot.x, 0.0, hit.z - c.foot.z);
	float rl = length(radial);
	radial = rl > 1e-4 ? radial / rl : vec3(1.0, 0.0, 0.0);
	vec3 n = conifer_normal(c, radial, conifer.reach.z);
	float s = clamp(conifer_s(c, hit.y), 0.0, 1.0);

	// The cards' colour, as leaf.frag.glsl builds it: palette by n.y, crown-depth darkening,
	// per-tree hue jitter. The per-leaf brightness term is the cards' alone.
	vec3 albedo = mix(leaf.palette_under.rgb, leaf.palette_top.rgb, n.y * 0.5 + 0.5);
	albedo *= mix(0.55, 1.0, s);
	albedo *= 1.0 + leaf.style.y * (tree_unit(tree_hash(c.hash ^ 0x2Bu)) * 2.0 - 1.0);
	albedo.g *= 1.0 + leaf.style.y * 0.5 * (tree_unit(tree_hash(c.hash ^ 0x2Cu)) * 2.0 - 1.0);
	// Self-shade only: past 150 m the deferred pass mins the sun map in (far_field_owns), so
	// terrain shadow on imposters is free (spec §3).
	float sun = mix(1.0 - conifer.reach.w, 1.0, s);
	out_albedo = GB_PACK_ALBEDO(albedo, sun);
	out_surface = GB_PACK_SURFACE(n, MAT_LEAF_CLUMP, leaf.style.x);
	vec4 clip = pc.view_proj * vec4(hit, 1.0);
	gl_FragDepth = clip.z / clip.w;
}
```

- [ ] **Step 5: `ConiferImpostorPass`**

`extension/src/render/conifer_impostor_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/gpu/gpu.h"

namespace godot {

class ConiferScatterPass;
class GBuffer;

// One non-indexed indirect draw of six vertices per imposter tree, pulled from the cull's
// imposter list. The fragment ray-casts the tiered crown and writes real depth, so imposters
// sort against terrain and each other with the far field's reverse-Z compare (spec §6).
class ConiferImpostorPass {
public:
	~ConiferImpostorPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	void release_targets();
	// False on failure; true with no draw when nothing was listed.
	bool draw(RenderingDevice *rd, ConiferScatterPass &scatter, GBuffer &gb,
			const Projection &view_proj, const float cam_pos[3]);
	int last_vertex_count() const { return last_vertex_count_; }

private:
	bool ensure_pipeline(RenderingDevice *rd, GBuffer &gb);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_, pipeline_;
	gpu::FramebufferCache framebuffer_;
	gpu::SetCache set_;
	int last_vertex_count_ = 0;
};

} // namespace godot
```

`extension/src/render/conifer_impostor_pass.cpp`:

```cpp
#include "render/conifer_impostor_pass.h"
#include "render/conifer_scatter_pass.h"
#include "render/gbuffer.h"
#include "gpu_layout/gbuffer_layout.h"

using namespace godot;

namespace {
// Mirrors the vertex and fragment Push block: mat4 view_proj; vec4 cam.
struct ImpostorPush {
	float view_proj[16];
	float cam[4];
};
} // namespace

ConiferImpostorPass::~ConiferImpostorPass() { teardown(); }

void ConiferImpostorPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "ConiferImpostorPass", "conifer_impostor.vert.glsl",
			"conifer_impostor.frag.glsl");
	if (!shader_.is_valid()) teardown();
}

void ConiferImpostorPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = RID();
	framebuffer_ = gpu::FramebufferCache();
	set_ = gpu::SetCache();
	last_vertex_count_ = 0;
	rd_ = nullptr;
}

void ConiferImpostorPass::release_targets() {
	if (rd_) framebuffer_.release(rd_, group_);
}

bool ConiferImpostorPass::ensure_pipeline(RenderingDevice *rd, GBuffer &gb) {
	if (!shader_.is_valid()) return false;
	if (!framebuffer_.get(rd, group_, {gb.albedo(), gb.surface(), gb.depth()}).is_valid()) return false;
	if (!pipeline_.is_valid()) {
		gpu::RasterState state;
		// The quad faces the camera; cull nothing so a winding flip can never drop a tree.
		state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
		state.front = RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		// Reverse-Z, the far field's compare: the fragment writes the crown's real depth.
		state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		state.color_attachments = ve::layout::kGbColorAttachments;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}

bool ConiferImpostorPass::draw(RenderingDevice *rd, ConiferScatterPass &scatter, GBuffer &gb,
		const Projection &view_proj, const float cam_pos[3]) {
	last_vertex_count_ = 0;
	if (!rd_ || rd != rd_ || !shader_.is_valid() || !gb.is_valid()) return false;
	const RID list = scatter.impostor_list_buffer();
	const RID args = scatter.impostor_draw_args_buffer();
	if (!list.is_valid() || !args.is_valid()) return true;
	if (!ensure_pipeline(rd, gb)) return false;
	gpu::RdDevice device{rd};
	if (!set_.get(device, group_, shader_, 0, {
			gpu::storage(0, list),
			gpu::ubo(1, scatter.params_buffer()),
			gpu::ubo(2, scatter.raster_params_buffer())}).is_valid())
		return false;
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set_.id(), 0);
	ImpostorPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) push.view_proj[c * 4 + r] = view_proj.columns[c][r];
	push.cam[0] = cam_pos[0];
	push.cam[1] = cam_pos[1];
	push.cam[2] = cam_pos[2];
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	rd->draw_list_draw_indirect(dl, false, args, 0, 1, 16);
	rd->draw_list_end();
	last_vertex_count_ = scatter.last_impostors() * 6;
	return true;
}
```

- [ ] **Step 6: Create the pass and draw it**

`orchestrator.cpp`: add `#include "render/conifer_impostor_pass.h"`. In the `has_conifers` success branch, after `conifer_raster`:

```cpp
			passes_.conifer_impostor = new ConiferImpostorPass();
			passes_.conifer_impostor->initialize(device);
```

In teardown, beside the conifer deletes:

```cpp
	if (passes_.conifer_impostor) { delete passes_.conifer_impostor; passes_.conifer_impostor = nullptr; }
```

`frame.cpp`: add `#include "render/conifer_impostor_pass.h"`. Add `if (ConiferImpostorPass *ci = render_.passes().conifer_impostor) ci->release_targets();` beside the other `release_targets()` calls, and replace the last two lines of `draw_conifers` (the `cards` lookup and its `return`) with:

```cpp
	LeafRasterPass *cards = render_.passes().conifer_raster;
	ConiferImpostorPass *imps = render_.passes().conifer_impostor;
	const bool cards_ok = cards && cards->draw(rd, pass->raster_inputs(), gb, view_proj, cam_pos);
	const bool imps_ok = imps && imps->draw(rd, *pass, gb, view_proj, cam_pos);
	return cards_ok && imps_ok;
```

`hooks_render.cpp`, in `debug_conifer_stats` beside `card_vertices`:

```cpp
	if (ConiferImpostorPass *ci = w->context().render->passes().conifer_impostor)
		d["impostor_vertices"] = ci->last_vertex_count();
```

and add `#include "render/conifer_impostor_pass.h"`.

- [ ] **Step 7: Run the tests**

Run:
```bash
./build.sh && (cd extension && scons -Q test 2>&1 | tail -3)
./gdunit_tests.sh -a res://tests/test_conifers.gd
./gdunit_tests.sh -a res://tests 2>&1 | tee "$TMPDIR/conifers-task7.txt"
```
Expected: native PASS; every `test_conifers.gd` case PASS; the full gdUnit failure set equals Task 0's list.

- [ ] **Step 8: Commit**

```bash
git add shaders/conifer.glslh shaders/conifer_impostor.vert.glsl shaders/conifer_impostor.frag.glsl \
	extension/src/render/conifer_impostor_pass.h extension/src/render/conifer_impostor_pass.cpp \
	extension/src/render/orchestrator.cpp extension/src/render/frame.cpp \
	extension/src/debug/hooks_render.cpp extension/tests/test_conifer_shader.cpp tests/test_conifers.gd
git commit -m "feat: analytic conifer imposters out to the forest material"
```

---

### Task 8: Capture, measurement and look tuning

**Files:**
- Modify: `tools/fjord_capture.gd`, `assets/pipelines/fjords.pipeline` (tuning only), `tools/convert_forest.sh` + `assets/materials/10_*.png` (tuning only), `docs/superpowers/specs/2026-10-08-fjords-conifers-design.md` (§13)
- Create: `tests/fixtures/fjords_no_conifers.pipeline` (measurement only)

- [ ] **Step 1: Print conifer stats in the capture**

In `tools/fjord_capture.gd`'s `_shoot`, after the frame is saved, add a print with the same `FJORD_CAPTURE` prefix:

```gdscript
	print("FJORD_CAPTURE conifers %s eye=%s" % [world.hooks().debug_conifer_stats(camera.global_position, -camera.global_basis.z), camera.global_position])
```

On a demo world the hook is pure-read: it reports what the compositor's last frame placed.

- [ ] **Step 2: Capture the three poses**

Run:
```bash
./build.sh --verify && godot --path . --resolution 1280x720 -s res://tools/fjord_capture.gd -- --out="$TMPDIR/conifers-1" --at=2000,-1600
```
Expected: `valley.png`, `ridge.png` and `aerial.png` in `$TMPDIR/conifers-1`, plus conifer stat lines with non-zero `card_trees` (valley) and `impostors` (aerial). Show the three frames to the user beside the references (`~/Pictures/Screenshots/screenshot_20261007_150634.png`, `…150751.png`, `…150810.png`). Ask which look changes they want before tuning anything.

- [ ] **Step 3: Tune, only on the user's direction**

The dials, in order of reach:
- Placement and shape: `fjords.pipeline`, under the `conifers` stage (`density`, `height_min`, `height_max`, `crown_radius`, `max_slope`, `treeline_margin`).
- Colour and fade: `set_conifer_value` knobs. Once settled, write them into `ConiferSettings` defaults; a test pins nothing about the palette.
- Far forest colour: `MODULATE` / `TINT` in `tools/convert_forest.sh`, re-run, and update the row's `flat_albedo` to the new mean (Task 2, Step 3's command).

Re-capture after each round into a fresh `$TMPDIR/conifers-N`. After every change to `fjords.pipeline` params, re-run `./gdunit_tests.sh -a res://tests/test_conifers.gd` and `res://tests/test_field_diff.gd`.

- [ ] **Step 4: Measure against the budgets**

Create `tests/fixtures/fjords_no_conifers.pipeline`: a copy of `assets/pipelines/fjords.pipeline` with the `conifers` stage line and its comment removed. It lives under `tests/fixtures/`, so neither the world-type enumerator nor `test_field_diff.gd` sees it.

Run three interleaved cycles, sequentially (two Godot processes on one GPU measure each other):
```bash
for i in 1 2 3; do
	for p in tests/fixtures/fjords_no_conifers.pipeline assets/pipelines/fjords.pipeline; do
		godot --path . --resolution 2560x1440 --disable-vsync demo/scenes/main.tscn -- --benchmark \
			--pipeline=res://$p --pose=2000,54.2,-1600,200,4 2>&1 | grep -E "p50|p99|settle"
	done
done
```
Expected: per run, wall p50/p99 and settle frames. Report the median deltas against spec §11's budgets: frame p50 ≤ +2.5 ms, cold fill ≤ +30%. These are reported, not gated. Over budget, report the numbers and the dial order (`card_reach_m`, `impostor_reach_m`, `density`, `cell`). Do not tune without the user.

- [ ] **Step 5: Record the deviations, tuning and measurements**

Fill `docs/superpowers/specs/2026-10-08-fjords-conifers-design.md` §13 with:
- **13.1 Decided while planning:** this plan's deviations 1–14, one paragraph each, verbatim in substance.
- **13.2 Recorded while implementing:** every tuning round (params, before and after, which capture), the measurement table from Step 4 with commands and machine, and the user's verdict on the capture, quoted. If the user gave none, write that no acceptance is implied.

Set the spec's **Status** line to `implemented`.

- [ ] **Step 6: Run everything once more**

Run:
```bash
./build.sh && (cd extension && scons -Q test 2>&1 | tail -3)
./gdunit_tests.sh -a res://tests 2>&1 | tee "$TMPDIR/conifers-final.txt"
git diff --stat main -- shaders/generated/field.glslh.golden tests/golden
```
Expected: native PASS; the gdUnit failure set equals Task 0's list; the last command prints nothing.

- [ ] **Step 7: Commit**

```bash
git add tools/fjord_capture.gd tests/fixtures/fjords_no_conifers.pipeline \
	docs/superpowers/specs/2026-10-08-fjords-conifers-design.md
# plus any tuned files: assets/pipelines/fjords.pipeline, tools/convert_forest.sh,
# assets/materials/10_*.png, extension/src/world/material_table.h, shaders/material_table.glslh,
# extension/src/conifers/conifer_settings.h
git commit -m "docs: record the conifer deviations, tuning and measurements"
```
