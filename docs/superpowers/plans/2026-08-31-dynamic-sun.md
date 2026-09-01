# Dynamic Sun Direction and Colour — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive the sun's direction and colour from the scene's `DirectionalLight3D`, and fix the
shadow-map depth bias that leaves the rasterized far field flat-lit at the demo's world size.

**Architecture:** The sun stops being a compile-time constant mirrored in three files and becomes
a `ve::SunState` value, resolved from the scene on the main thread and published to the GPU
through one orchestrator-owned UBO that three passes bind. Separately and independently, the
sun-shadow map's depth bias is converted from world metres into the normalized depth units it is
actually compared against.

**Tech Stack:** C++17 GDExtension (godot-cpp), GLSL 460 compute/raster shaders driven through
Godot's `RenderingDevice`, doctest for native tests, gdUnit4 for GPU tests.

**Spec:** `docs/superpowers/specs/2026-08-31-dynamic-sun-design.md`

## Global Constraints

- The sun direction convention is **toward the sun**, matching `ve::kSunDir`. Every new API uses
  it. A `DirectionalLight3D` emits along its local `-Z`, so toward-the-sun is `+basis.column(2)`.
- `ve::kSunDir = {0.5746958f, 0.7662610f, 0.2873479f}` stays in `shade/cel.h` as the default when
  no light node is resolved. Its value does not change.
- `DeferredPass::kAmbient` stays `{0.16f, 0.19f, 0.26f}`. Ambient is out of scope.
- `shaders/cel_object.gdshader` is not edited. `VE_SUN_DIR` stays a constant in
  `shaders/cel.gdshaderinc`; the existing 8-argument `ve_cel_shade` must keep compiling unchanged.
- A white sun (`{1,1,1}`) must reproduce today's shading output exactly. The existing
  `debug_cel_diff` / `debug_cel_reference` hooks stay 8-argument and keep passing untouched.
- New pure logic goes under `extension/src/shade/`. Only `src/world`, `src/generator`, `src/core`,
  `src/mesh`, `src/connectivity`, `src/lod` and `src/shade` are linked into the native test binary
  (`extension/SConstruct:16-18`), so anything that needs a doctest test must live there and must
  not include godot-cpp headers.
- Native tests are globbed (`Glob("tests/*.cpp")`, `SConstruct:28`). New `TEST_CASE`s in existing
  files need no registration.
- Free shader bindings, verified: raymarch **24** (existing run to 23), deferred **10**
  (0–9 used), contact shadow **5** (0–4 used).
- `RaymarchPass::render` has 10+ call sites and `DeferredPass::render` has 9. Do not add
  parameters to either. Use a setter for the UBO RID (mirroring `RaymarchPass::set_materials`)
  and `DeferredPass::Params` for the new scalar.

---

### Task 1: `SunOrtho::depth_range` and the unit pin that was missing

**Files:**
- Modify: `extension/src/shade/sun_ortho.h:14-19`
- Modify: `extension/src/shade/sun_ortho.cpp:68-88`
- Test: `extension/tests/test_sun_ortho.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `ve::SunOrtho::depth_range` — `float`, the light-space depth extent of the world box
  in world metres. Task 2 and Task 9 read it.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_sun_ortho.cpp`. The file already defines `project()`, `kLo` and
`kHi` (a 4096 × 1024 × 4096 m world) in its anonymous namespace; reuse them.

```cpp
TEST_CASE("depth_range is the world box's extent along the light axis") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	REQUIRE(o.valid);
	// Light-space depth runs along -kSunDir (away from the sun). kSunDir is unit length,
	// so projecting the corners onto it gives metres directly.
	float mn = 1e30f;
	float mx = -1e30f;
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? kHi[0] : kLo[0], (i & 2) ? kHi[1] : kLo[1],
				(i & 4) ? kHi[2] : kLo[2]};
		const float c = -(p[0] * ve::kSunDir[0] + p[1] * ve::kSunDir[1] +
				p[2] * ve::kSunDir[2]);
		mn = std::min(mn, c);
		mx = std::max(mx, c);
	}
	CHECK(o.depth_range == doctest::Approx(mx - mn).epsilon(1e-4));
}

// The regression pin for the flat far field. deferred.comp.glsl compares a bias against
// NORMALIZED depth, so the texel size it scales must be normalized too. texel_world alone is
// metres: at this world's size it is ~2.7, which as a bias over a [0,1] depth range reports
// every pixel lit. The ratio is the quantity the shader actually needs.
TEST_CASE("one shadow texel is a small fraction of the depth range") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	REQUIRE(o.valid);
	REQUIRE(o.depth_range > 0.0f);
	CHECK(o.texel_world / o.depth_range < 0.01f);
	// And the un-normalized value is nowhere near usable as a depth bias. This is the bug.
	CHECK(o.texel_world > 0.1f);
}
```

`<algorithm>` is needed for `std::min`/`std::max`; add it to the includes if absent.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
./build.sh --test
```

Expected: compile error — `'depth_range' is not a member of 've::SunOrtho'`.

- [ ] **Step 3: Add the field and populate it**

In `extension/src/shade/sun_ortho.h`, inside `struct SunOrtho`, after `texel_world`:

```cpp
	float depth_range = 0.0f; // light-space depth extent, in world metres
```

In `extension/src/shade/sun_ortho.cpp`, beside the existing `o.texel_world` assignment near the
end of `sun_ortho()` (the local `d` is already computed as `mx[2] - mn[2]`):

```cpp
	o.texel_world = std::max(w, h) / static_cast<float>(map_size);
	o.depth_range = d;
	o.valid = true;
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test
```

Expected: all native tests pass, including the two new cases.

- [ ] **Step 5: Commit**

```bash
git add extension/src/shade/sun_ortho.h extension/src/shade/sun_ortho.cpp extension/tests/test_sun_ortho.cpp
git commit -m "feat: SunOrtho reports the light-space depth range

The shadow bias needs the texel size in the units the depth comparison
actually uses. sun_ortho already computed this extent and discarded it."
```

---

### Task 2: Divide the shadow bias by the depth range

This is the whole fix for the flat far field. After this task the demo has far-field shadows.

**Files:**
- Modify: `shaders/deferred.comp.glsl:31-40`
- Modify: `extension/src/render/deferred_pass.h:15-21`
- Modify: `extension/src/render/deferred_pass.cpp:193-199`
- Modify: `extension/src/render/sun_shadow_pass.h:24-26,44-46`
- Modify: `extension/src/render/sun_shadow_pass.cpp` (`build()` tail, `teardown()`)
- Modify: `extension/src/raymarch_compositor.cpp:363-378`
- Modify: `extension/src/debug/hooks.cpp:4375-4395`
- Test: `tests/test_sun_shadow.gd`

**Interfaces:**
- Consumes: `ve::SunOrtho::depth_range` (Task 1).
- Produces: `SunShadowPass::depth_range()` → `float`. `DeferredPass::Params::shadow_depth_range`
  → `float`, default `0.0f`.

- [ ] **Step 1: Write the failing GPU test**

Add to `tests/test_sun_shadow.gd`. `make_world()` builds an `{8,5,8}` world, which is too small
to expose this bug — see the arithmetic in the comment. This test needs its own larger world.

```gdscript
# The bias bug scales with world size, which is why make_world()'s {8,5,8} world passes
# against it: there the broken bias is ~20 m of world depth and the probe sits ~34 m under
# the surface, so it still reads shadowed. At {16,5,16} the broken bias is
#   texel_world * depth_range * 0.5 = 0.268 * 451.2 * 0.5 = ~60 m
# which is deeper than the probe, so the old code reports LIT. The fixed bias is ~0.13 m.
func make_big_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(16, 5, 16)
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func test_the_bias_is_in_depth_units_not_metres(timeout := 120000) -> void:
	var w := make_big_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w.hooks().debug_sun_shadow_build(true)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_greater(0)
	# Well under the terrain surface: shadowed. Fails against a metres-scaled bias.
	assert_float(w.hooks().debug_sun_shadow_visibility(Vector3(60.0, 20.0, 60.0))) \
		.is_equal_approx(0.0, 0.01)
```

If `{16,5,16}` does not settle inside the budget, raise `timeout` further before shrinking the
world — the world must stay large enough that `texel_world * depth_range * 0.5` exceeds the
probe's depth below the surface, or the test proves nothing.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./gdunit_tests.sh -a res://tests/test_sun_shadow.gd
```

Expected: `test_the_bias_is_in_depth_units_not_metres` FAILS, reporting visibility 1.0 where 0.0
was expected. Every other case in the suite passes.

Capture the whole-suite baseline first — seven gdunit suites fail on a clean `main`:

```bash
git stash && ./gdunit_tests.sh 2>&1 | tail -40 > /tmp/gdunit-baseline.txt; git stash pop
```

- [ ] **Step 3: Carry `depth_range` through `SunShadowPass`**

In `extension/src/render/sun_shadow_pass.h`, next to `texel_world()`:

```cpp
	float depth_range() const { return depth_range_; }
```

and next to the `texel_world_` member:

```cpp
	float depth_range_ = 0.0f;
```

In `extension/src/render/sun_shadow_pass.cpp`, in `build()` beside `texel_world_ = ortho.texel_world;`:

```cpp
	texel_world_ = ortho.texel_world;
	depth_range_ = ortho.depth_range;
```

and in `teardown()`, beside `texel_world_ = 0.0f;`:

```cpp
	texel_world_ = 0.0f;
	depth_range_ = 0.0f;
```

- [ ] **Step 4: Carry it through `DeferredPass`**

In `extension/src/render/deferred_pass.h`, add to `struct Params` after `ambient`:

```cpp
		// Light-space depth extent of the sun ortho, in world metres. Only read when a sun
		// map is bound; `render()` clears kFlagSunMap when it is not, so the default 0 is
		// never divided by.
		float shadow_depth_range = 0.0f;
```

In `extension/src/render/deferred_pass.cpp`, in `render()`, replace the UBO fill's tail:

```cpp
	uf[16] = shadow_texel;
	uf[17] = p.shadow_depth_range;
	uf[18] = uf[19] = 0.0f;
```

- [ ] **Step 5: Fix the shader**

In `shaders/deferred.comp.glsl`, replace the body of `sun_map_visibility()`'s bias lines:

```glsl
	float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
	// params.x is one shadow texel in world metres; params.y is the light-space depth range
	// in the same metres. p.z and the stored depth are normalized [0,1], so the bias must be
	// too. Scaling by params.x alone made the bias ~0.54 of the entire depth range at the
	// demo's world size, which reported every pixel lit and left the far field flat.
	float texel = sun.params.x / max(sun.params.y, 1e-6);
	float bias = texel * (0.5 + 2.0 * slope) + 0.0015;
	return (p.z + bias >= texture(sun_map, uv).r) ? 1.0 : 0.0;
```

- [ ] **Step 6: Set it at both call sites that bind a sun map**

In `extension/src/raymarch_compositor.cpp`, beside the existing `dp.flags = beauty_flags;` and
after `use_sun` is computed:

```cpp
	dp.shadow_depth_range = use_sun ? sun->depth_range() : 0.0f;
```

In `extension/src/debug/hooks.cpp`, in `debug_sun_shadow_visibility()`, beside
`dp.flags = ve::pack_flags(beauty);`:

```cpp
	dp.shadow_depth_range = use_sun ? world_->sun_shadow_pass()->depth_range() : 0.0f;
```

Leave the other seven `DeferredPass::render` call sites alone: they pass no sun map, so
`render()` clears `kFlagSunMap` (`deferred_pass.cpp:188`) and the bias is never evaluated.

- [ ] **Step 7: Run both suites to verify they pass**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test && ./gdunit_tests.sh -a res://tests/test_sun_shadow.gd -a res://tests/test_deferred.gd
```

Expected: native tests pass; both gdunit suites pass including the new case.

- [ ] **Step 8: Commit**

```bash
git add shaders/deferred.comp.glsl extension/src/render/deferred_pass.h extension/src/render/deferred_pass.cpp extension/src/render/sun_shadow_pass.h extension/src/render/sun_shadow_pass.cpp extension/src/raymarch_compositor.cpp extension/src/debug/hooks.cpp tests/test_sun_shadow.gd
git commit -m "fix: the sun shadow bias was in metres, not depth units

sun.params.x is world metres per shadow texel; p.z and the stored depth
are normalized [0,1]. At the demo's world size the unconverted bias was
~0.54 of the whole depth range, so every pixel reported lit and the
rasterized far field had no shadows at all. The {8,5,8} test world was
8x too small to catch it."
```

---

### Task 3: `ve::SunState`

**Files:**
- Create: `extension/src/shade/sun_state.h`
- Create: `extension/src/shade/sun_state.cpp`
- Test: `extension/tests/test_sun_state.cpp` (new)

**Interfaces:**
- Consumes: `ve::kSunDir` from `shade/cel.h`.
- Produces:
  - `struct ve::SunState { float dir[3]; float right[3]; float up[3]; float rgb[3]; bool has_basis() const; }`
  - `float ve::srgb_to_linear(float c)`

Header-only would work, but `sun_state.cpp` keeps `srgb_to_linear` out of every translation unit
and gives the SConstruct glob something to compile.

- [ ] **Step 1: Write the failing tests**

Create `extension/tests/test_sun_state.cpp`:

```cpp
#include <doctest/doctest.h>
#include "shade/sun_state.h"
#include "shade/cel.h"
#include <cmath>

TEST_CASE("a default SunState is today's constant sun, white, with no basis") {
	const ve::SunState s;
	CHECK(s.dir[0] == doctest::Approx(ve::kSunDir[0]));
	CHECK(s.dir[1] == doctest::Approx(ve::kSunDir[1]));
	CHECK(s.dir[2] == doctest::Approx(ve::kSunDir[2]));
	CHECK(s.rgb[0] == doctest::Approx(1.0f));
	CHECK(s.rgb[1] == doctest::Approx(1.0f));
	CHECK(s.rgb[2] == doctest::Approx(1.0f));
	// No authored basis: callers must fall back to sun_ortho's direction-only overload.
	CHECK_FALSE(s.has_basis());
}

TEST_CASE("a basis counts as authored only when both axes are non-degenerate") {
	ve::SunState s;
	s.right[0] = 1.0f;
	CHECK_FALSE(s.has_basis()); // up is still zero
	s.up[1] = 1.0f;
	CHECK(s.has_basis());
}

TEST_CASE("srgb_to_linear matches the standard piecewise curve") {
	CHECK(ve::srgb_to_linear(0.0f) == doctest::Approx(0.0f));
	CHECK(ve::srgb_to_linear(1.0f) == doctest::Approx(1.0f));
	// Below the knee the curve is a plain divide.
	CHECK(ve::srgb_to_linear(0.04f) == doctest::Approx(0.04f / 12.92f).epsilon(1e-6));
	// Above it, the gamma branch. 0.5 sRGB is a well-known ~0.2140 linear.
	CHECK(ve::srgb_to_linear(0.5f) == doctest::Approx(0.21404f).epsilon(1e-4));
	// Monotonic, and never above its input in the interior.
	CHECK(ve::srgb_to_linear(0.7f) > ve::srgb_to_linear(0.3f));
	CHECK(ve::srgb_to_linear(0.5f) < 0.5f);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test
```

Expected: `shade/sun_state.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `extension/src/shade/sun_state.h`:

```cpp
#pragma once
#include "shade/cel.h"

namespace ve {

// Everything the renderer needs to know about the sun this frame. Resolved from the scene's
// DirectionalLight3D on the main thread and published to the GPU through one UBO.
//
// `dir` points TOWARD the sun, matching kSunDir and sun_ortho's contract. A DirectionalLight3D
// emits along its local -Z, so `dir` is +basis.column(2).
struct SunState {
	float dir[3] = {kSunDir[0], kSunDir[1], kSunDir[2]}; // normalized, toward the sun
	float right[3] = {}; // the light's basis X in world space; all-zero when unauthored
	float up[3] = {};    // the light's basis Y in world space; all-zero when unauthored
	float rgb[3] = {1.0f, 1.0f, 1.0f}; // linear light_color * light_energy

	// An all-zero basis is the explicit signal for "no light node resolved". Callers select
	// sun_ortho's direction-only overload, which derives a basis as it always has. A zero
	// basis must never reach the explicit-basis overload.
	bool has_basis() const;
};

// The standard sRGB electro-optical transfer function. DirectionalLight3D::get_color() is
// sRGB as authored in the inspector; this engine shades in linear.
float srgb_to_linear(float c);

} // namespace ve
```

Create `extension/src/shade/sun_state.cpp`:

```cpp
#include "shade/sun_state.h"
#include <cmath>

namespace ve {

bool SunState::has_basis() const {
	const float r = right[0] * right[0] + right[1] * right[1] + right[2] * right[2];
	const float u = up[0] * up[0] + up[1] * up[1] + up[2] * up[2];
	return r > 1e-8f && u > 1e-8f;
}

float srgb_to_linear(float c) {
	if (c <= 0.0f) return 0.0f;
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace ve
```

- [ ] **Step 4: Run to verify it passes**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test
```

Expected: all native tests pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/shade/sun_state.h extension/src/shade/sun_state.cpp extension/tests/test_sun_state.cpp
git commit -m "feat: ve::SunState, the sun as a value instead of a constant"
```

---

### Task 4: `sun_ortho` overload taking the light's own basis

**Files:**
- Modify: `extension/src/shade/sun_ortho.h`
- Modify: `extension/src/shade/sun_ortho.cpp`
- Test: `extension/tests/test_sun_ortho.cpp`

**Interfaces:**
- Consumes: `ve::SunOrtho` (Task 1).
- Produces: `ve::SunOrtho ve::sun_ortho(const float sun_dir[3], const float right[3], const float up[3], const float lo[3], const float hi[3], int map_size)`.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_sun_ortho.cpp`:

```cpp
// Deriving the light basis from cross(l, worldUp) is well defined but badly CONDITIONED near
// the zenith: a small azimuth change swings the derived basis through a large rotation, so an
// animated sun crossing overhead makes the shadow map spin about the light axis. The demo's
// DirectionalLight3D sits at 84.99 degrees elevation, right in that band. A node supplies its
// own orthonormal basis, which rotates continuously and has no degenerate case.
TEST_CASE("the explicit-basis overload is continuous through the zenith") {
	const float eps = 1e-3f;
	// Two sun directions a hair either side of straight up, differing only in azimuth.
	const float a_dir[3] = {eps, 1.0f, 0.0f};
	const float b_dir[3] = {-eps, 1.0f, 0.0f};
	// A basis that barely moves between them, as a real animated node's would.
	const float right[3] = {0.0f, 0.0f, 1.0f};
	const float up_a[3] = {1.0f, 0.0f, 0.0f};
	const float up_b[3] = {1.0f, 0.0f, 0.0f};
	const ve::SunOrtho a = ve::sun_ortho(a_dir, right, up_a, kLo, kHi, 2048);
	const ve::SunOrtho b = ve::sun_ortho(b_dir, right, up_b, kLo, kHi, 2048);
	REQUIRE(a.valid);
	REQUIRE(b.valid);
	for (int i = 0; i < 16; i++) CHECK(b.view_proj[i] == doctest::Approx(a.view_proj[i]).epsilon(1e-2));

	// The derived-basis overload, given the same two directions, does NOT stay close: its
	// right vector flips through 180 degrees as the azimuth crosses over.
	const ve::SunOrtho da = ve::sun_ortho(a_dir, kLo, kHi, 2048);
	const ve::SunOrtho db = ve::sun_ortho(b_dir, kLo, kHi, 2048);
	REQUIRE(da.valid);
	REQUIRE(db.valid);
	float max_delta = 0.0f;
	for (int i = 0; i < 16; i++)
		max_delta = std::max(max_delta, std::fabs(db.view_proj[i] - da.view_proj[i]));
	CHECK(max_delta > 1e-4f);
}

TEST_CASE("an explicit basis reproduces the derived one when they agree") {
	const ve::SunOrtho derived = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	REQUIRE(derived.valid);
	// Rebuild kSunDir's derived basis by hand and hand it back in: same matrix.
	const float l[3] = {-ve::kSunDir[0], -ve::kSunDir[1], -ve::kSunDir[2]};
	float r[3] = {l[1] * 0.0f - l[2] * 1.0f, l[2] * 0.0f - l[0] * 0.0f, l[0] * 1.0f - l[1] * 0.0f};
	const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	for (int i = 0; i < 3; i++) r[i] /= rl;
	float u[3] = {r[1] * l[2] - r[2] * l[1], r[2] * l[0] - r[0] * l[2], r[0] * l[1] - r[1] * l[0]};
	const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	for (int i = 0; i < 3; i++) u[i] /= ul;
	const ve::SunOrtho explicit_basis = ve::sun_ortho(ve::kSunDir, r, u, kLo, kHi, 2048);
	REQUIRE(explicit_basis.valid);
	for (int i = 0; i < 16; i++)
		CHECK(explicit_basis.view_proj[i] == doctest::Approx(derived.view_proj[i]).epsilon(1e-5));
	CHECK(explicit_basis.depth_range == doctest::Approx(derived.depth_range).epsilon(1e-5));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test
```

Expected: no matching function for a six-argument `ve::sun_ortho`.

- [ ] **Step 3: Refactor into a shared core plus two entry points**

In `extension/src/shade/sun_ortho.h`, below the existing declaration:

```cpp
// As above, but with the light's own orthonormal basis supplied rather than derived from a
// world-up hint. Deriving is well defined but badly conditioned near the zenith, where a small
// azimuth change swings the basis through a large rotation and spins the shadow map. A scene
// light carries a basis that rotates continuously, so an animated sun should pass it here.
// `right` and `up` must both be non-degenerate; they are re-orthonormalized against `sun_dir`.
SunOrtho sun_ortho(const float sun_dir[3], const float right[3], const float up[3],
		const float lo[3], const float hi[3], int map_size);
```

In `extension/src/shade/sun_ortho.cpp`, rename the existing function body's core. Replace
everything from the `up`/`r`/`u` derivation block through the end of the function with a shared
helper, and give both entry points to it:

```cpp
namespace {

// The half of sun_ortho() that does not care where the basis came from. `l` points AWAY from
// the sun, `r`/`u` complete a right-handed orthonormal set with it.
ve::SunOrtho fit_box(const float l[3], const float r[3], const float u[3],
		const float lo[3], const float hi[3], int map_size) {
	ve::SunOrtho o;
	float mn[3] = {1e30f, 1e30f, 1e30f};
	float mx[3] = {-1e30f, -1e30f, -1e30f};
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1],
				(i & 4) ? hi[2] : lo[2]};
		const float c[3] = {p[0] * r[0] + p[1] * r[1] + p[2] * r[2],
				p[0] * u[0] + p[1] * u[1] + p[2] * u[2],
				p[0] * l[0] + p[1] * l[1] + p[2] * l[2]};
		for (int a = 0; a < 3; a++) {
			mn[a] = std::min(mn[a], c[a]);
			mx[a] = std::max(mx[a], c[a]);
		}
	}
	const float w = mx[0] - mn[0];
	const float h = mx[1] - mn[1];
	const float d = mx[2] - mn[2];
	if (!(w > 0.0f) || !(h > 0.0f) || !(d > 0.0f)) return o;

	// Rows of the matrix. Row 2 is the reverse-Z remap: depth = (mx_l - l.p) / d, so a point
	// at the near (sunward) extreme is 1 and the far one is 0.
	const float row0[4] = {2.0f * r[0] / w, 2.0f * r[1] / w, 2.0f * r[2] / w,
			-2.0f * mn[0] / w - 1.0f};
	const float row1[4] = {2.0f * u[0] / h, 2.0f * u[1] / h, 2.0f * u[2] / h,
			-2.0f * mn[1] / h - 1.0f};
	const float row2[4] = {-l[0] / d, -l[1] / d, -l[2] / d, mx[2] / d};
	const float row3[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	const float *rows[4] = {row0, row1, row2, row3};
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++)
			o.view_proj[c * 4 + rr] = rows[rr][c];

	o.texel_world = std::max(w, h) / static_cast<float>(map_size);
	o.depth_range = d;
	o.valid = true;
	return o;
}

// Shared preamble: validate the box and normalize the light axis. Returns false if unusable.
bool light_axis(const float sun_dir[3], const float lo[3], const float hi[3], int map_size,
		float l[3]) {
	if (map_size <= 0) return false;
	for (int a = 0; a < 3; a++)
		if (!(hi[a] > lo[a])) return false;
	float f[3] = {sun_dir[0], sun_dir[1], sun_dir[2]};
	if (norm3(f) <= 0.0f) return false;
	// Light-space +z points AWAY from the sun, so depth grows with distance from it and the
	// reverse-Z remap is a single subtraction.
	l[0] = -f[0];
	l[1] = -f[1];
	l[2] = -f[2];
	return true;
}

} // namespace
```

Then the two public entry points:

```cpp
SunOrtho sun_ortho(const float sun_dir[3], const float lo[3], const float hi[3], int map_size) {
	SunOrtho o;
	float l[3];
	if (!light_axis(sun_dir, lo, hi, map_size, l)) return o;

	// Any hint not parallel to the light. kSunDir is well off vertical, so world up works;
	// the fallback exists so a sun straight overhead does not collapse the basis. It stops
	// division by zero; it does not make the result stable near the zenith, which is why the
	// explicit-basis overload below exists.
	float up[3] = {0.0f, 1.0f, 0.0f};
	float r[3];
	cross3(l, up, r);
	if (norm3(r) < 1e-4f) {
		up[0] = 1.0f;
		up[1] = 0.0f;
		up[2] = 0.0f;
		cross3(l, up, r);
		if (norm3(r) < 1e-4f) return o;
	}
	float u[3];
	cross3(r, l, u);
	norm3(u);
	return fit_box(l, r, u, lo, hi, map_size);
}

SunOrtho sun_ortho(const float sun_dir[3], const float right[3], const float up[3],
		const float lo[3], const float hi[3], int map_size) {
	SunOrtho o;
	float l[3];
	if (!light_axis(sun_dir, lo, hi, map_size, l)) return o;

	// Re-orthonormalize the supplied basis against the light axis. A scene node's basis is
	// already orthonormal, but it is authored data: never trust it to be exactly so.
	float u[3] = {up[0], up[1], up[2]};
	if (norm3(u) <= 0.0f) return o;
	float r[3];
	cross3(u, l, r);
	if (norm3(r) < 1e-4f) {
		// `up` is parallel to the light; fall back to the supplied right vector.
		r[0] = right[0];
		r[1] = right[1];
		r[2] = right[2];
		if (norm3(r) <= 0.0f) return o;
	}
	cross3(r, l, u);
	if (norm3(u) <= 0.0f) return o;
	return fit_box(l, r, u, lo, hi, map_size);
}
```

`<algorithm>` and `<cmath>` are already included.

- [ ] **Step 4: Run to verify it passes**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test
```

Expected: every case in `test_sun_ortho.cpp` passes, the pre-existing ones unchanged.

- [ ] **Step 5: Commit**

```bash
git add extension/src/shade/sun_ortho.h extension/src/shade/sun_ortho.cpp extension/tests/test_sun_ortho.cpp
git commit -m "feat: sun_ortho can take the light's own basis

Deriving one from cross(l, worldUp) is badly conditioned near the zenith,
where the demo's light happens to sit. A scene node's basis rotates
continuously and has no degenerate case."
```

---

### Task 5: `cel_shade` gains a sun colour

**Files:**
- Modify: `extension/src/shade/cel.h:31-40`
- Modify: `extension/src/shade/cel.cpp:74-86`
- Modify: `shaders/shade.glslh:109-120`
- Modify: `shaders/cel.gdshaderinc` (last function only)
- Test: `extension/tests/test_shade_cel.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `ve::CelInput::sun` — `float[3]`, default `{1,1,1}`. GLSL
  `vec3 cel_shade(vec3 albedo, vec3 ambient, float ndl, float ndv, float ndh, float shadow, float ao, float gloss, vec3 sun)`
  plus the existing 8-argument overload forwarding `vec3(1.0)`. Same overload pair for
  `ve_cel_shade` in `cel.gdshaderinc`. Task 7 calls the 9-argument GLSL form.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_shade_cel.cpp` (`plain()` is already defined in its anonymous
namespace):

```cpp
TEST_CASE("a white sun is exactly today's shading") {
	const ve::CelParams p;
	ve::CelInput in = plain(1.0f);
	in.ambient[0] = 0.16f; in.ambient[1] = 0.19f; in.ambient[2] = 0.26f;
	in.gloss = 1.0f;
	in.ndh = 1.0f;   // inside the specular band
	in.ndv = 0.0f;   // and picking up rim
	float out[3];
	ve::cel_shade(p, in, out);
	// The default sun is white, so this must equal the value produced by hand from the
	// pre-sun formula: tint*lit + tint*ambient*ao + spec + rim.
	ve::CelInput white = in;
	white.sun[0] = white.sun[1] = white.sun[2] = 1.0f;
	float ref[3];
	ve::cel_shade(p, white, ref);
	for (int c = 0; c < 3; c++) CHECK(out[c] == doctest::Approx(ref[c]).epsilon(1e-6));
}

TEST_CASE("the sun colours direct light and specular, never ambient or rim") {
	const ve::CelParams p;
	// Ambient only: ndl below the first band edge still yields band 0 (level 0.18), so use a
	// pure-ambient comparison by holding everything else fixed and changing only the sun.
	ve::CelInput a = plain(1.0f);
	a.ambient[0] = 0.5f; a.ambient[1] = 0.5f; a.ambient[2] = 0.5f;
	ve::CelInput b = a;
	b.sun[0] = 0.0f; b.sun[1] = 0.0f; b.sun[2] = 0.0f;
	float lit_out[3];
	float dark_out[3];
	ve::cel_shade(p, a, lit_out);
	ve::cel_shade(p, b, dark_out);
	// Killing the sun must not kill the ambient contribution.
	for (int c = 0; c < 3; c++) CHECK(dark_out[c] > 0.0f);
	for (int c = 0; c < 3; c++) CHECK(dark_out[c] < lit_out[c]);

	// The rim is a silhouette stylization, not a light: it survives a black sun.
	ve::CelInput rim_only = plain(1.0f);
	rim_only.ambient[0] = rim_only.ambient[1] = rim_only.ambient[2] = 0.0f;
	rim_only.ndv = 0.0f;
	rim_only.sun[0] = rim_only.sun[1] = rim_only.sun[2] = 0.0f;
	float rim_out[3];
	ve::cel_shade(p, rim_only, rim_out);
	CHECK(rim_out[0] == doctest::Approx(p.rim_strength).epsilon(1e-4));
}

TEST_CASE("a tinted sun scales the direct term per channel") {
	const ve::CelParams p;
	ve::CelInput warm = plain(1.0f);
	warm.ambient[0] = warm.ambient[1] = warm.ambient[2] = 0.0f;
	warm.ndv = 1.0f; // no rim
	warm.sun[0] = 1.0f; warm.sun[1] = 0.5f; warm.sun[2] = 0.25f;
	float out[3];
	ve::cel_shade(p, warm, out);
	CHECK(out[1] == doctest::Approx(out[0] * 0.5f).epsilon(1e-5));
	CHECK(out[2] == doctest::Approx(out[0] * 0.25f).epsilon(1e-5));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test
```

Expected: `'struct ve::CelInput' has no member named 'sun'`.

- [ ] **Step 3: Add the field and apply it in all three mirrors**

In `extension/src/shade/cel.h`, add to `struct CelInput` after `gloss`:

```cpp
	float sun[3] = {1, 1, 1}; // linear sun colour * energy; white reproduces the old output
```

In `extension/src/shade/cel.cpp`, replace the final loop of `cel_shade`:

```cpp
	for (int c = 0; c < 3; c++)
		out[c] = tint[c] * lit * in.sun[c] + tint[c] * in.ambient[c] * ao +
				in.sun[c] * spec + rim;
```

In `shaders/shade.glslh`, replace `cel_shade` with the overload pair:

```glsl
vec3 cel_shade(vec3 albedo, vec3 ambient, float ndl, float ndv, float ndh,
		float shadow, float ao, float gloss, vec3 sun) {
	float sh = clamp(shadow, 0.0, 1.0);
	float lit = cel_level(ndl) * sh;
	vec3 tint = cel_shadow_tint(albedo, 1.0 - lit);
	// A hard step, not a falloff: the band edge IS the highlight's silhouette.
	float spec = (gloss > 0.0 && ndh >= CEL_SPEC_EDGE) ? gloss * CEL_SPEC_STRENGTH * sh : 0.0;
	float rim = CEL_RIM_STRENGTH * pow(1.0 - clamp(ndv, 0.0, 1.0), CEL_RIM_POWER);
	// The sun colours what the sun lights. Ambient carries its own colour, and the rim is a
	// stylization of silhouette rather than a light, so neither is tinted.
	return tint * lit * sun + tint * ambient * clamp(ao, 0.0, 1.0) + sun * spec + vec3(rim);
}

vec3 cel_shade(vec3 albedo, vec3 ambient, float ndl, float ndv, float ndh,
		float shadow, float ao, float gloss) {
	return cel_shade(albedo, ambient, ndl, ndv, ndh, shadow, ao, gloss, vec3(1.0));
}
```

In `shaders/cel.gdshaderinc`, replace the final `ve_cel_shade` with the same overload pair, in the
file's compact house style:

```glsl
vec3 ve_cel_shade(vec3 albedo,vec3 ambient,float ndl,float ndv,float ndh,
		float shadow,float ao,float gloss,vec3 sun){float sh=clamp(shadow,0.0,1.0);
	float lit=ve_cel_level(ndl)*sh;vec3 tint=ve_shadow_tint(albedo,1.0-lit);
	float spec=(gloss>0.0&&ndh>=VE_SPEC_EDGE)?gloss*VE_SPEC*sh:0.0;
	float rim=VE_RIM*pow(1.0-clamp(ndv,0.0,1.0),VE_RIM_POWER);
	return tint*lit*sun+tint*ambient*clamp(ao,0.0,1.0)+sun*spec+vec3(rim);}
vec3 ve_cel_shade(vec3 albedo,vec3 ambient,float ndl,float ndv,float ndh,
		float shadow,float ao,float gloss){
	return ve_cel_shade(albedo,ambient,ndl,ndv,ndh,shadow,ao,gloss,vec3(1.0));}
```

`cel_object.gdshader` calls the 8-argument form and is not edited.

- [ ] **Step 4: Run native and both GPU parity suites**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test && ./gdunit_tests.sh -a res://tests/test_deferred.gd -a res://tests/test_cel_object.gd
```

Expected: native tests pass. `test_deferred.gd`'s `debug_cel_diff` CPU/GPU parity and
`test_cel_object.gd`'s `debug_cel_reference` both still pass — they exercise the white-sun path,
which is unchanged by construction.

- [ ] **Step 5: Commit**

```bash
git add extension/src/shade/cel.h extension/src/shade/cel.cpp shaders/shade.glslh shaders/cel.gdshaderinc extension/tests/test_shade_cel.cpp
git commit -m "feat: cel_shade takes a sun colour, defaulting to white

Direct light and its specular take the sun's colour. Ambient keeps its
own and the rim stays neutral -- it is a silhouette stylization, not a
light. A white sun reproduces the previous output exactly, so the
existing CPU/GPU parity hooks stay 8-argument and keep passing."
```

---

### Task 6: The shared `SunLight` UBO, and the raymarcher reads it

**Files:**
- Create: `extension/src/render/sun_ubo.h`
- Create: `extension/src/render/sun_ubo.cpp`
- Create: `shaders/sun_light.glslh`
- Modify: `shaders/shade.glslh:10` (delete `SUN_DIR`)
- Modify: `shaders/raymarch.comp.glsl` (include, and every `SUN_DIR` use)
- Modify: `extension/src/render/raymarch_pass.h`, `raymarch_pass.cpp`
- Modify: `extension/src/render/orchestrator.h`, `orchestrator.cpp`

**Interfaces:**
- Consumes: `ve::SunState` (Task 3).
- Produces:
  - `class godot::SunUbo { bool ensure(RenderingDevice *rd); void update(RenderingDevice *rd, const ve::SunState &s); RID buffer() const; void teardown(); }`
  - `RenderOrchestrator::sun_ubo()` → `SunUbo *`
  - `RaymarchPass::set_sun_ubo(RID)`
  - GLSL `sun_light.dir` / `sun_light.rgb`, behind `SUN_LIGHT_SET` / `SUN_LIGHT_BINDING`.

- [ ] **Step 1: Write `sun_light.glslh`**

Create `shaders/sun_light.glslh`:

```glsl
// The sun, as data. Included by every pass that needs to know where the sun is or what colour
// it is; the includer must define SUN_LIGHT_SET and SUN_LIGHT_BINDING first, the same way
// beauty_camera.glslh is used. One buffer backs all of them (godot::SunUbo).
//
// NOTE: never put a literal include directive inside a comment in this file -- the loader
// matches include tokens anywhere in a line and would self-include (cycle error).
layout(set = SUN_LIGHT_SET, binding = SUN_LIGHT_BINDING, std140) uniform SunLight {
	vec4 dir; // xyz = normalized, TOWARD the sun; w unused
	vec4 rgb; // xyz = linear colour * energy; w unused
} sun_light;
```

- [ ] **Step 2: Write `SunUbo`**

Create `extension/src/render/sun_ubo.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "shade/sun_state.h"

namespace godot {

// One 32-byte uniform buffer holding this frame's sun, bound by the raymarch, deferred and
// contact-shadow passes. Owned by RenderOrchestrator and updated once per frame, so the three
// passes cannot disagree about where the sun is. Mirrors shaders/sun_light.glslh.
class SunUbo {
public:
	bool ensure(RenderingDevice *rd);
	void update(RenderingDevice *rd, const ve::SunState &s);
	RID buffer() const { return buffer_; }
	void teardown();

private:
	RenderingDevice *rd_ = nullptr;
	RID buffer_;
};

} // namespace godot
```

Create `extension/src/render/sun_ubo.cpp`:

```cpp
#include "render/sun_ubo.h"
#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

bool SunUbo::ensure(RenderingDevice *rd) {
	if (!rd) return false;
	if (buffer_.is_valid() && rd_ == rd) return true;
	teardown();
	rd_ = rd;
	PackedByteArray zero;
	zero.resize(32);
	zero.fill(0);
	buffer_ = rd->uniform_buffer_create(32, zero);
	return buffer_.is_valid();
}

void SunUbo::update(RenderingDevice *rd, const ve::SunState &s) {
	if (!rd || !buffer_.is_valid()) return;
	PackedByteArray b;
	b.resize(32);
	float *f = reinterpret_cast<float *>(b.ptrw());
	f[0] = s.dir[0];
	f[1] = s.dir[1];
	f[2] = s.dir[2];
	f[3] = 0.0f;
	f[4] = s.rgb[0];
	f[5] = s.rgb[1];
	f[6] = s.rgb[2];
	f[7] = 0.0f;
	rd->buffer_update(buffer_, 0, 32, b);
}

void SunUbo::teardown() {
	if (rd_ && buffer_.is_valid()) rd_->free_rid(buffer_);
	buffer_ = RID();
	rd_ = nullptr;
}
```

- [ ] **Step 3: Own it in the orchestrator**

In `extension/src/render/orchestrator.h`: add `class SunUbo;` beside the other forward
declarations (near `class CameraUbo;`), an accessor beside `beauty_camera()`:

```cpp
	SunUbo *sun_ubo() { return sun_ubo_; }
```

and a member beside `sun_shadow_pass_`:

```cpp
	SunUbo *sun_ubo_ = nullptr;
```

In `extension/src/render/orchestrator.cpp`: add `#include "render/sun_ubo.h"`; create it beside
`beauty_camera_ = new CameraUbo();` and immediately `ensure` it and hand it to the raymarch pass:

```cpp
	sun_ubo_ = new SunUbo();
	if (!sun_ubo_->ensure(device)) {
		UtilityFunctions::printerr("RenderOrchestrator: sun UBO creation failed");
		delete sun_ubo_;
		sun_ubo_ = nullptr;
	} else if (raymarch_pass_) {
		raymarch_pass_->set_sun_ubo(sun_ubo_->buffer());
	}
```

Place this **after** `raymarch_pass_` is constructed. In the teardown, beside the `beauty_camera_`
line:

```cpp
	if (sun_ubo_) { sun_ubo_->teardown(); delete sun_ubo_; sun_ubo_ = nullptr; }
```

- [ ] **Step 4: Bind it in `RaymarchPass`**

In `extension/src/render/raymarch_pass.h`, beside `set_materials`:

```cpp
	// The sun UBO is owned by RenderOrchestrator; this pass only mirrors its RID into the
	// uniform set. Call once after initialize() and before the first render.
	void set_sun_ubo(RID buffer);
```

and a member beside `edits_ubo_`:

```cpp
	RID sun_ubo_; // NOT owned: RenderOrchestrator frees it
```

In `extension/src/render/raymarch_pass.cpp`, after `set_materials`:

```cpp
void RaymarchPass::set_sun_ubo(RID buffer) {
	sun_ubo_ = buffer;
	// Same invalidation as set_materials: the uniform set caches this RID.
	if (uset_.is_valid()) {
		rd_->free_rid(uset_);
		uset_ = RID();
	}
}
```

In `teardown()`, add `sun_ubo_ = RID();` beside `material_albedo_ = RID();` — do **not** add it to
the `free_rid` loop, it is not owned here.

In the uniform-set build (the `Ref<RDUniform> u[31]` block), widen the array to `u[32]`, update
both `31` literals to `32`, and add binding 24 before `uniform_set_create`:

```cpp
	u[24]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[24]->set_binding(24); u[24]->add_id(sun_ubo_);
```

Add `u[24]` to the `Array::make(...)` list passed to `uniform_set_create`.

- [ ] **Step 5: Switch the shader over**

In `shaders/shade.glslh`, delete the `SUN_DIR` constant line entirely:

```glsl
const vec3 SUN_DIR = vec3(0.5746958, 0.7662610, 0.2873479); // ve::kSunDir
```

In `shaders/raymarch.comp.glsl`, add above the existing includes:

```glsl
#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 24
```

and an include of `sun_light.glslh` beside the `shade.glslh` include. Then replace every
`SUN_DIR` with `sun_light.dir.xyz` — five code uses, at lines 647, 657, 658, 683 and 689, plus
a reference inside the comment at 651. Update the comment at 651 to say the direction is supplied by the light rather
than being `normalize(vec3(0.6, 0.8, 0.3))`; the code below it divides by the direction's
components, so note that a sun with an exactly-zero component is the caller's responsibility to
avoid (a normalized node basis effectively never produces one, and the `ray_box` slab test it
feeds already tolerates infinities).

- [ ] **Step 6: Build and run the raymarch suites**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test && ./gdunit_tests.sh -a res://tests/test_raymarch_gbuffer.gd -a res://tests/test_sun_shadow.gd
```

Expected: both pass. Nothing has changed behaviourally yet — the UBO still carries `kSunDir`,
because nothing updates it from a scene light until Task 8.

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/sun_ubo.h extension/src/render/sun_ubo.cpp extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp extension/src/render/raymarch_pass.h extension/src/render/raymarch_pass.cpp shaders/sun_light.glslh shaders/shade.glslh shaders/raymarch.comp.glsl
git commit -m "feat: one shared sun UBO, and the raymarcher reads it

The raymarch push constant is already exactly 128 bytes, the guaranteed
minimum, so the direction cannot ride there. One orchestrator-owned
buffer bound by every pass that needs the sun keeps them from drifting."
```

---

### Task 7: The deferred and contact-shadow passes read the sun

**Files:**
- Modify: `shaders/deferred.comp.glsl`
- Modify: `shaders/contact_shadow.comp.glsl:1-20,38`
- Modify: `extension/src/render/deferred_pass.h`, `deferred_pass.cpp`
- Modify: `extension/src/render/contact_shadow_pass.h`, `contact_shadow_pass.cpp`
- Modify: `extension/src/render/orchestrator.cpp`

**Interfaces:**
- Consumes: `SunUbo::buffer()` (Task 6), the 9-argument GLSL `cel_shade` (Task 5).
- Produces: `DeferredPass::set_sun_ubo(RID)`, `ContactShadowPass::set_sun_ubo(RID)`.

- [ ] **Step 1: Add the setters**

In `extension/src/render/deferred_pass.h`, in the public section:

```cpp
	// The sun UBO is owned by RenderOrchestrator; this pass only mirrors its RID.
	void set_sun_ubo(RID buffer);
```

and beside `sun_ubo_` (which is this pass's own shadow-matrix buffer — the new one is different):

```cpp
	RID sun_light_ubo_; // NOT owned: RenderOrchestrator frees it
```

In `extension/src/render/deferred_pass.cpp`:

```cpp
void DeferredPass::set_sun_ubo(RID buffer) {
	sun_light_ubo_ = buffer;
	// The uniform set caches this RID; drop it so the next render rebuilds.
	if (rd_ && uset_.is_valid()) {
		rd_->free_rid(uset_);
		uset_ = RID();
	}
}
```

In `ensure_uniform_set`, widen `Ref<RDUniform> u[10]` to `u[11]` (both literals), and add:

```cpp
	u[10]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[10]->set_binding(10);
	u[10]->add_id(sun_light_ubo_);
```

Add `u[10]` to the `Array::make(...)` argument list.

Do the same shape in `extension/src/render/contact_shadow_pass.h` / `.cpp`: a
`void set_sun_ubo(RID buffer);`, a `RID sun_light_ubo_;` member, and in `ensure_uniform_set` a
`UNIFORM_TYPE_UNIFORM_BUFFER` at binding 5 added to its uniform array.

In `extension/src/render/orchestrator.cpp`, extend the block written in Task 6 so all three
passes are handed the buffer:

```cpp
	} else {
		if (raymarch_pass_) raymarch_pass_->set_sun_ubo(sun_ubo_->buffer());
		if (deferred_pass_) deferred_pass_->set_sun_ubo(sun_ubo_->buffer());
		if (contact_shadow_pass_) contact_shadow_pass_->set_sun_ubo(sun_ubo_->buffer());
	}
```

Move the `sun_ubo_` creation below all three passes' construction if it is not already.

- [ ] **Step 2: Switch the shaders over**

In `shaders/deferred.comp.glsl`, add above the includes:

```glsl
#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 10
```

include `sun_light.glslh`, then replace the three shading lines:

```glsl
	vec3 sun_dir = sun_light.dir.xyz;
	float ndl = dot(n, sun_dir);
	float ndv = dot(n, v);
	float ndh = dot(n, normalize(sun_dir + v));
```

and pass the colour into the ramp:

```glsl
	vec3 lit = cel_shade(g0.rgb, ambient, ndl, ndv, ndh, shadow, ao, g1.w, sun_light.rgb.xyz);
```

In `shaders/contact_shadow.comp.glsl`, add the same two `#define`s with `SUN_LIGHT_BINDING 5`,
include `sun_light.glslh`, and replace `SUN_DIR` at line 38 with `sun_light.dir.xyz`.

- [ ] **Step 3: Build and run the deferred suites**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test && ./gdunit_tests.sh -a res://tests/test_deferred.gd -a res://tests/test_sun_shadow.gd -a res://tests/test_cel_object.gd
```

Expected: all pass. Still no behaviour change — the UBO carries `kSunDir` and a white sun.

- [ ] **Step 4: Commit**

```bash
git add extension/src/render/deferred_pass.h extension/src/render/deferred_pass.cpp extension/src/render/contact_shadow_pass.h extension/src/render/contact_shadow_pass.cpp extension/src/render/orchestrator.cpp shaders/deferred.comp.glsl shaders/contact_shadow.comp.glsl
git commit -m "feat: the deferred and contact-shadow passes read the sun UBO"
```

---

### Task 8: Resolve the scene's `DirectionalLight3D`

**Files:**
- Modify: `extension/src/voxel_world.h`
- Modify: `extension/src/voxel_world.cpp` (`_bind_methods`, `_process`)
- Modify: `extension/src/raymarch_compositor.cpp`

**Interfaces:**
- Consumes: `ve::SunState` (Task 3), `sun_ortho` explicit-basis overload (Task 4),
  `SunUbo::update` (Task 6).
- Produces: `VoxelWorld::sun_state()` → `ve::SunState` **by value** (copied under the mutex, so
  the render thread never holds a reference into mutating state). Property `sun_light_path`.

- [ ] **Step 1: Add the property and the resolve**

In `extension/src/voxel_world.h`: `#include "shade/sun_state.h"`, and in the class:

```cpp
	void set_sun_light_path(const NodePath &p) { sun_light_path_ = p; }
	NodePath get_sun_light_path() const { return sun_light_path_; }
	// Copied, not referenced: _process writes this from the main thread while the render
	// callback reads it.
	ve::SunState sun_state() const {
		std::lock_guard<std::mutex> lock(sun_mutex_);
		return sun_state_;
	}
```

and the private state (`<mutex>` is already included by this header's neighbours; add it if not):

```cpp
	NodePath sun_light_path_;
	mutable std::mutex sun_mutex_;
	ve::SunState sun_state_;
```

In `extension/src/voxel_world.cpp`, add to `_bind_methods` beside the `physics_center_path` pair:

```cpp
	ClassDB::bind_method(D_METHOD("set_sun_light_path", "p"), &VoxelWorld::set_sun_light_path);
	ClassDB::bind_method(D_METHOD("get_sun_light_path"), &VoxelWorld::get_sun_light_path);
```

and beside the `physics_center_path` property:

```cpp
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light_path"), "set_sun_light_path", "get_sun_light_path");
```

Add `#include <godot_cpp/classes/directional_light3d.hpp>` and, in `_process`, **before** the
`physics_enabled_` early return (the sun must update whether or not physics runs):

```cpp
	update_sun_state();
```

and the method itself:

```cpp
void VoxelWorld::update_sun_state() {
	ve::SunState s; // defaults to kSunDir, white, no basis
	DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(
			sun_light_path_.is_empty() ? nullptr : get_node_or_null(sun_light_path_));
	if (light) {
		const Basis b = light->get_global_transform().basis;
		// A DirectionalLight3D emits along its local -Z, so +Z points toward the sun.
		const Vector3 dir = b.get_column(2).normalized();
		const Vector3 right = b.get_column(0).normalized();
		const Vector3 up = b.get_column(1).normalized();
		s.dir[0] = dir.x; s.dir[1] = dir.y; s.dir[2] = dir.z;
		s.right[0] = right.x; s.right[1] = right.y; s.right[2] = right.z;
		s.up[0] = up.x; s.up[1] = up.y; s.up[2] = up.z;
		// get_color() is sRGB as authored in the inspector; this engine shades in linear.
		// Energy above 1 is passed through deliberately: the demo's additive bloom is the
		// intended consumer of the overrange.
		const Color c = light->get_color();
		const float e = static_cast<float>(light->get_param(Light3D::PARAM_ENERGY));
		s.rgb[0] = ve::srgb_to_linear(c.r) * e;
		s.rgb[1] = ve::srgb_to_linear(c.g) * e;
		s.rgb[2] = ve::srgb_to_linear(c.b) * e;
	}
	std::lock_guard<std::mutex> lock(sun_mutex_);
	sun_state_ = s;
}
```

Declare `void update_sun_state();` in the header's private section.

- [ ] **Step 2: Feed it to the GPU and to the shadow projection**

In `extension/src/raymarch_compositor.cpp`, add `#include "render/sun_ubo.h"`, and near the top of
the render callback where the camera UBO is updated (around line 119), publish the sun:

```cpp
	const ve::SunState sun_state = world->sun_state();
	if (SunUbo *sun_ubo = world->sun_ubo()) {
		if (sun_ubo->ensure(rd)) sun_ubo->update(rd, sun_state);
	}
```

Add a `SunUbo *sun_ubo() { return context_.render->sun_ubo(); }` accessor to `VoxelWorld` beside
`sun_shadow_pass()`, with a `class SunUbo;` forward declaration.

Then, inside `build_sun_shadow`, replace the `sun_ortho` call so it uses the resolved sun and its
basis when one was authored:

```cpp
			const ve::SunOrtho ortho = sun_state.has_basis()
					? ve::sun_ortho(sun_state.dir, sun_state.right, sun_state.up, lo, hi,
							SunShadowPass::kSize)
					: ve::sun_ortho(sun_state.dir, lo, hi, SunShadowPass::kSize);
			const bool shadow_ok = sun->build(rd, *world->lod_pool(), *lod_raster, ortho, false);
```

Leave `extension/src/debug/hooks.cpp`'s two `ve::kSunDir` uses alone. Those hooks drive headless
probes with no scene light, and `kSunDir` is exactly the documented fallback.

- [ ] **Step 3: Build and verify the extension still loads**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test --verify
```

Expected: builds, native tests pass, no "Could not find type" errors.

- [ ] **Step 4: Run the GPU suites**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./gdunit_tests.sh -a res://tests/test_sun_shadow.gd -a res://tests/test_deferred.gd -a res://tests/test_raymarch_gbuffer.gd
```

Expected: all pass. No test scene sets `sun_light_path`, so every world still runs on `kSunDir`.

- [ ] **Step 5: Commit**

```bash
git add extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/raymarch_compositor.cpp
git commit -m "feat: resolve the sun from a DirectionalLight3D in the scene

Read on the main thread in _process, the same shape as
physics_center_path, and published to the render callback by value under
a mutex. An empty path keeps ve::kSunDir and a white sun."
```

---

### Task 9: Rebuild the shadow map the moment the sun moves

**Files:**
- Modify: `extension/src/render/sun_shadow_pass.cpp` (`build()`)
- Test: `tests/test_sun_shadow.gd`

**Interfaces:**
- Consumes: `ve::SunOrtho::view_proj`.
- Produces: no new API. `SunShadowPass::build` gains an internal urgent-rebuild path.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_sun_shadow.gd`. The hook drives `sun_ortho` from `ve::kSunDir` directly, so
drive the change through a world whose `sun_light_path` points at a light that this test rotates.
`debug_sun_shadow_build(false)` is the non-forced path, which is what the throttle guards.

```gdscript
# A day/night sweep must not lag twelve frames behind the sun. kMinFrames exists to stop LoD
# churn from rebuilding constantly; a sun that actually moved is not churn.
func test_a_moved_sun_rebuilds_without_waiting_for_the_throttle(timeout := 60000) -> void:
	var w := make_world()
	var light := DirectionalLight3D.new()
	light.rotation = Vector3(-0.9, 0.3, 0.0)
	add_child(light)
	w.sun_light_path = w.get_path_to(light)
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	# Settle the map so the pass is clean and inside its throttle window.
	w.hooks().debug_sun_shadow_build(true)
	var before: int = w.hooks().debug_sun_shadow_stats()["rebuilds"]
	var matrix_before: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]
	# A clean pass refuses an unforced build...
	w.hooks().debug_sun_shadow_build(false)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_equal(before)
	# ...but not when the sun has moved.
	light.rotation = Vector3(-0.5, 1.4, 0.0)
	await get_tree().process_frame
	w.hooks().debug_sun_shadow_build(false)
	assert_int(w.hooks().debug_sun_shadow_stats()["rebuilds"]).is_equal(before + 1)
	var matrix_after: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]
	var moved := false
	for i in range(16):
		if absf(matrix_after[i] - matrix_before[i]) > 1e-5:
			moved = true
	assert_bool(moved).is_true()
	light.queue_free()
```

This requires `debug_sun_shadow_build` and `debug_sun_shadow_stats` to use the world's resolved
sun rather than `ve::kSunDir`. Update both hooks in `extension/src/debug/hooks.cpp` (lines 4339
and 4363) to the same `has_basis()` selection used in Task 8 Step 2, reading
`world_->sun_state()`. With no light assigned this still yields `kSunDir`, so every other case in
the suite is unaffected.

- [ ] **Step 2: Run to verify it fails**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./gdunit_tests.sh -a res://tests/test_sun_shadow.gd
```

Expected: the new case FAILS at the `before + 1` assertion — the throttle refuses the rebuild.

- [ ] **Step 3: Add the urgent path**

In `extension/src/render/sun_shadow_pass.cpp`, in `build()`, replace the gate:

```cpp
	frames_since_++;
	if (!is_valid() || !ortho.valid) return false;
	// A sun that moved is not LoD churn. kMinFrames throttles rebuilds caused by pages
	// coming and going; it must not make a day/night sweep lag twelve frames behind the
	// light. Comparing the matrix keeps the policy here rather than in every caller, and
	// leaves the camera-motion invariant intact: camera motion does not change this matrix.
	const bool sun_moved = std::memcmp(view_proj_, ortho.view_proj, sizeof(view_proj_)) != 0;
	if (!force && !sun_moved && (!dirty_ || frames_since_ < kMinFrames)) return false;
```

`<cstring>` is already included.

- [ ] **Step 4: Run to verify it passes**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./gdunit_tests.sh -a res://tests/test_sun_shadow.gd
```

Expected: the whole suite passes, including
`test_a_lazy_rebuild_does_not_fire_every_frame` (its world has no light, so the matrix is
constant and the throttle still applies) and
`test_the_matrix_does_not_move_with_the_camera` (camera motion does not change the matrix).

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/sun_shadow_pass.cpp extension/src/debug/hooks.cpp tests/test_sun_shadow.gd
git commit -m "feat: a moved sun rebuilds the shadow map immediately

A rebuild costs 0.124 ms p50 against a 1.0 ms budget, so the twelve-frame
throttle was never about cost -- it stops LoD churn from rebuilding
constantly. A sun that moved is not churn."
```

---

### Task 10: Wire the demo scene and prove it end to end

**Files:**
- Modify: `demo/main.tscn:59-62` (the `VoxelWorld` node)
- Test: `tests/test_sun_shadow.gd`

**Interfaces:**
- Consumes: everything above.
- Produces: nothing new.

- [ ] **Step 1: Write the failing end-to-end test**

Append to `tests/test_sun_shadow.gd`. This is the assertion that the node actually drives the
image, rather than merely reaching the GPU.

```gdscript
# The whole point: rotating the light must change what is in shadow. Rather than betting on
# one hand-picked point flipping over procedural terrain, sample a grid and assert the SET of
# shadowed points differs. Over a 40-degree sun swing across real terrain, some must flip.
func _shadow_mask(w: VoxelWorld) -> Array:
	var mask: Array = []
	for x in range(40, 121, 20):
		for z in range(40, 121, 20):
			# Just under the surface band, where occlusion actually varies with sun angle.
			mask.append(w.hooks().debug_sun_shadow_visibility(Vector3(float(x), 58.0, float(z))))
	return mask

func test_moving_the_sun_moves_the_shadow(timeout := 60000) -> void:
	var w := make_world()
	var light := DirectionalLight3D.new()
	add_child(light)
	w.sun_light_path = w.get_path_to(light)
	light.rotation = Vector3(-0.35, 0.0, 0.0) # low sun
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	await get_tree().process_frame
	w.hooks().debug_sun_shadow_build(true)
	var low: Array = _shadow_mask(w)
	var low_matrix: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]

	light.rotation = Vector3(-1.2, 2.4, 0.0) # high sun, opposite azimuth
	await get_tree().process_frame
	w.hooks().debug_sun_shadow_build(true)
	var high: Array = _shadow_mask(w)
	var high_matrix: PackedFloat32Array = w.hooks().debug_sun_shadow_stats()["view_proj"]

	# The projection followed the node.
	var matrix_moved := false
	for i in range(16):
		if absf(high_matrix[i] - low_matrix[i]) > 1e-5:
			matrix_moved = true
	assert_bool(matrix_moved).override_failure_message(
		"the sun ortho did not change when the light rotated").is_true()

	# And so did the image.
	var flipped := 0
	for i in range(low.size()):
		if absf(float(high[i]) - float(low[i])) > 0.5:
			flipped += 1
	assert_int(flipped).override_failure_message(
		"no sampled point changed shadow state across a 40-degree sun swing").is_greater(0)
	light.queue_free()
```

- [ ] **Step 1b: Run to verify it fails on a scene that is not yet wired**

Before Task 8's wiring this test could not pass at all; it is written after that wiring, so
confirm it exercises the new path by temporarily clearing `w.sun_light_path = NodePath()` and
checking the matrix-moved assertion fails. Restore the line afterwards.

- [ ] **Step 2: Run to verify it passes against the built work**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./gdunit_tests.sh -a res://tests/test_sun_shadow.gd
```

Expected: PASS. If it fails, the wiring from Tasks 8 and 9 is incomplete — debug there, not here.

- [ ] **Step 3: Point the demo at its light**

In `demo/main.tscn`, on the `VoxelWorld` node (which currently sets `near_field_scale`,
`physics_center_path` and `process_mode`), add:

```
sun_light_path = NodePath("/root/Main/DirectionalLight3D")
```

- [ ] **Step 4: Look at it**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --verify
```

Then run the demo and confirm by eye:
- the rasterized far field now has shadows (Task 2), and
- rotating the `DirectionalLight3D` moves them, with no perceptible lag (Tasks 8–9).

Expect the scene to look **flatter than before**: the light sits at 84.99° elevation where
`kSunDir` was at 50°, so shadows are much shorter. That is the light reporting its own rotation,
not a bug. Re-authoring the light's rotation is a scene edit and is the user's call — do not
change it without asking.

- [ ] **Step 5: Run the full suite and compare against the baseline**

```bash
cd /Users/jeremyzhao/Development/godot/voxel-everything && ./build.sh --test && ./gdunit_tests.sh 2>&1 | tail -40
```

Expected: no failures beyond the seven suites that already fail on a clean `main` (the baseline
captured in Task 2 Step 2). Any new failure is this work's.

- [ ] **Step 6: Commit**

```bash
git add demo/main.tscn tests/test_sun_shadow.gd
git commit -m "feat: point the demo at its DirectionalLight3D

The scene's light now drives the terrain's sun. Note the light sits at
85 degrees elevation where kSunDir was at 50, so the demo reads much
flatter until the light is re-authored."
```

---

## Deliberately not done

Recorded so a reviewer can tell these apart from oversights. All four were settled during
brainstorming; the first two are the requester's explicit scope calls.

- **Ambient stays constant.** `DeferredPass::kAmbient` remains `{0.16, 0.19, 0.26}`, a fixed warm
  noon. A dusk sun over a noon sky will read oddly.
- **`cel_object.gdshader` keeps its baked sun.** Non-voxel meshes — the demo's `TestCube` — stay
  lit from 50° while the terrain follows the node. A visible mismatch, and a small follow-up.
- **No texel snapping.** The world AABB is fixed, so a stationary sun already yields a
  bit-identical matrix frame to frame and does not crawl; a moving sun's shadow motion is real
  motion, not shimmer.
- **Shadow resolution is unchanged** at ~1.07 m per texel over the demo world. Coarse, and no
  coarser than before this work.
