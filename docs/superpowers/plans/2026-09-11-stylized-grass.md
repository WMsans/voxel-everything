# Stylized Grass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dense stylized grass blades growing on `grass_01` voxels, in the visual register of *Breath of the Wild*.

**Architecture:** A GPU-driven scatter (two compute dispatches: cull resident grass-surface bricks, then place blades inside them) feeds one indirect draw of 3-vertex blades into the existing G-buffer. Because blades write the same `albedo`/`surface`/`depth` channels the far field already writes, the existing deferred stack lights them with no edits to it. All new CPU code lives in a self-contained `extension/src/grass/` module.

**Tech Stack:** C++20, godot-cpp (Godot 4.7), GLSL 460 compute + raster through `RenderingDevice`, doctest for native tests, gdUnit4 for GPU tests, SCons.

**Spec:** `docs/superpowers/specs/2026-09-11-stylized-grass-design.md`

## Global Constraints

- Branch: `feat/stylized-grass`, already checked out with the spec committed.
- Voxel constants are fixed and must not be redefined: `kBrickVoxels = 16`, `kVoxelSize = 0.05f`, `kBrickSize = 0.8f` (`extension/src/world/brick.h`).
- Grass grows on material id **1** (`grass_01`). No new material id is introduced. `extension/src/world/material_table.h`, `shaders/material_table.glslh` and `tools/convert_materials.sh` must not be modified — `extension/tests/test_material_glslh.cpp` asserts the generated file byte for byte.
- **No edits** to `shaders/deferred.comp.glsl`, `shaders/shade.glslh`, `shaders/ssgi.comp.glsl`, `shaders/ssao.comp.glsl`, `shaders/outline.comp.glsl`, `extension/src/shade/beauty_settings.{h,cpp}`, or `extension/tests/test_beauty_settings.cpp`.
- `extension/src/grass/*.cpp` joins the native test target and therefore **must not include godot-cpp headers**. Signatures take plain `float` arrays and `ve::IVec3`, never `Vector3` / `Projection`.
- Depth is **reverse-Z**: near = 1, far = 0, compare `COMPARE_OP_GREATER_OR_EQUAL` (`lod_raster_pass.cpp:157`).
- Raster pipelines in this engine are **pull-only**: no vertex array, vertex format must be `RenderingDevice::INVALID_ID` (`lod_raster_pass.cpp:179`).
- Godot exposes neither `gl_DrawID` nor a non-zero `firstInstance` here (`shaders/lod.vert.glsl:9`). Recover everything from `gl_VertexIndex`.
- A grass pass failure calls `timings->cancel("grass")` and skips; it must never call `abort_frame()`.
- Build: `./build.sh -j$(sysctl -n hw.ncpu)` (macOS) or `-j$(nproc)` (Linux).
- Native tests: `cd extension && scons -Q test`. Single case: `extension/build/tests/ve_tests -tc="<case name>"`.
- GPU tests: `./gdunit_tests.sh -a res://tests/test_grass.gd`.
- Commit messages end with the two attribution lines used on this branch (see `git log -1 --format=%B`).

---

### Task 1: Grass settings module

Creates the module directory, its settings struct, and wires `src/grass` into the native test build. Nothing renders yet; this is the foundation every later task imports.

**Files:**
- Create: `extension/src/grass/grass_settings.h`
- Create: `extension/src/grass/grass_settings.cpp`
- Create: `extension/tests/test_grass_settings.cpp`
- Modify: `extension/SConstruct:16-19` (add `src/grass/*.cpp` to `pure_sources`)

**Interfaces:**
- Consumes: nothing.
- Produces: `ve::GrassSettings` (POD struct, fields below) and `void ve::clamp_grass_settings(ve::GrassSettings *s)`.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_grass_settings.cpp`:

```cpp
#include <doctest/doctest.h>
#include "grass/grass_settings.h"

TEST_CASE("grass defaults are inside their own clamp") {
	ve::GrassSettings s;
	ve::GrassSettings c = s;
	ve::clamp_grass_settings(&c);
	CHECK(c.enabled == s.enabled);
	CHECK(c.reach_m == doctest::Approx(s.reach_m));
	CHECK(c.blades_per_brick == s.blades_per_brick);
	CHECK(c.max_blades == s.max_blades);
	CHECK(c.blade_width_m == doctest::Approx(s.blade_width_m));
	CHECK(c.blade_height_m == doctest::Approx(s.blade_height_m));
	CHECK(c.wind_strength == doctest::Approx(s.wind_strength));
}

TEST_CASE("grass clamp pulls every knob back into range") {
	ve::GrassSettings s;
	s.reach_m = 1e9f;
	s.blades_per_brick = 4096;
	s.max_blades = -7;
	s.blade_width_m = -1.0f;
	s.blade_height_m = 1e9f;
	s.wind_strength = -3.0f;
	s.slope_cos_min = 7.0f;
	ve::clamp_grass_settings(&s);
	CHECK(s.reach_m <= 256.0f);
	CHECK(s.blades_per_brick <= 64);
	CHECK(s.max_blades >= 0);
	CHECK(s.blade_width_m >= 0.0f);
	CHECK(s.blade_height_m <= 4.0f);
	CHECK(s.wind_strength >= 0.0f);
	CHECK(s.slope_cos_min <= 1.0f);
}

// The reach is what sizes stage 1's dispatch, and that grows with its cube. A clamp that
// let it through unbounded would be a hang, not a visual bug.
TEST_CASE("grass reach is hard-bounded even from a NaN") {
	ve::GrassSettings s;
	s.reach_m = 0.0f / 0.0f;
	ve::clamp_grass_settings(&s);
	CHECK(s.reach_m >= 0.0f);
	CHECK(s.reach_m <= 256.0f);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd extension && scons -Q test
```

Expected: FAIL — `fatal error: 'grass/grass_settings.h' file not found`.

- [ ] **Step 3: Create the settings header**

Create `extension/src/grass/grass_settings.h`:

```cpp
#pragma once

namespace ve {

// Stylized grass knobs. DELIBERATELY not part of BeautySettings: grass is its own module
// with its own store, and the beauty stack neither reads these nor needs to know they
// exist (design doc section 3). Every field is clamped by clamp_grass_settings().
struct GrassSettings {
	bool enabled = true;

	// How far blades are placed, in metres. Stage 1's dispatch width grows with the CUBE
	// of this, so it is hard-bounded at 256 m -- past roughly 64 m the flat box wants to
	// become a hierarchy instead (design doc section 11).
	float reach_m = 40.0f;
	// Vertical half-extent of the brick search box, in metres. Grass grows on the ground,
	// so the box is much shorter than it is wide.
	float vertical_reach_m = 10.0f;

	// Candidate blades per brick in the nearest ring. Rings past the first drop 3 of every
	// 4 (Ghost of Tsushima's thinning), so this is the only density number to turn.
	int blades_per_brick = 16;
	// Hard cap on the instance buffer. The scatter clamps to it rather than overflowing.
	int max_blades = 400000;

	float blade_width_m = 0.018f;
	float blade_height_m = 0.55f;
	// Per-blade height jitter as a fraction of blade_height_m.
	float height_jitter = 0.35f;

	// Grass refuses any surface whose normal is flatter than this against +Y, which is what
	// keeps blades off cliff faces.
	float slope_cos_min = 0.55f;

	float wind_strength = 0.35f; // metres of tip displacement at full gust
	float wind_speed = 0.6f;     // gust field scroll rate
	float wind_scale = 0.04f;    // gust field frequency, cycles per metre

	// Fraction of blades that get a flower tint at the tip.
	float flower_chance = 0.012f;

	float gloss = 0.25f;
};

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_grass_settings(GrassSettings *s);

} // namespace ve
```

- [ ] **Step 4: Implement the clamp**

Create `extension/src/grass/grass_settings.cpp`:

```cpp
#include "grass/grass_settings.h"
#include <algorithm>
#include <cmath>

namespace ve {
namespace {
// NaN-safe: the comparison chain below is written so a NaN falls through to `lo`, which is
// the conservative end for every knob here.
float clampf(float v, float lo, float hi) {
	if (!(v > lo)) return lo;
	if (!(v < hi)) return hi;
	return v;
}
int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
} // namespace

void clamp_grass_settings(GrassSettings *s) {
	if (!s) return;
	s->reach_m = clampf(s->reach_m, 0.0f, 256.0f);
	s->vertical_reach_m = clampf(s->vertical_reach_m, 0.0f, 64.0f);
	s->blades_per_brick = clampi(s->blades_per_brick, 0, 64);
	s->max_blades = clampi(s->max_blades, 0, 4000000);
	s->blade_width_m = clampf(s->blade_width_m, 0.0f, 0.5f);
	s->blade_height_m = clampf(s->blade_height_m, 0.0f, 4.0f);
	s->height_jitter = clampf(s->height_jitter, 0.0f, 1.0f);
	s->slope_cos_min = clampf(s->slope_cos_min, -1.0f, 1.0f);
	s->wind_strength = clampf(s->wind_strength, 0.0f, 4.0f);
	s->wind_speed = clampf(s->wind_speed, 0.0f, 8.0f);
	s->wind_scale = clampf(s->wind_scale, 0.0f, 4.0f);
	s->flower_chance = clampf(s->flower_chance, 0.0f, 1.0f);
	s->gloss = clampf(s->gloss, 0.0f, 1.0f);
}

} // namespace ve
```

- [ ] **Step 5: Add `src/grass` to the native test build**

In `extension/SConstruct`, change the `pure_sources` assignment (currently lines 16-19) to include the new directory:

```python
pure_sources = (Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") +
                Glob("src/core/*.cpp") + Glob("src/terrain/*.cpp") +
                Glob("src/mesh/*.cpp") + Glob("src/connectivity/*.cpp") +
                Glob("src/lod/*.cpp") + Glob("src/shade/*.cpp") +
                Glob("src/grass/*.cpp"))
```

`src/grass/*.cpp` is already picked up by the shared-library build, which globs `src/*/*.cpp` at line 8. No change is needed there.

- [ ] **Step 6: Run tests to verify they pass**

```bash
cd extension && scons -Q test
```

Expected: PASS, including the three new `grass` cases and every pre-existing case.

- [ ] **Step 7: Commit**

```bash
git add extension/src/grass/grass_settings.h extension/src/grass/grass_settings.cpp \
        extension/tests/test_grass_settings.cpp extension/SConstruct
git commit -m "feat(grass): settings struct and clamp"
```

---

### Task 2: Grass layout — rings, brick box, frustum, capacity

The pure arithmetic the GPU passes are driven by. This is where the drop-3-of-4 thinning lives, so it is pinned without a GPU.

> **Deviation from the spec, deliberate:** section 3 names these files `grass_field.h/.cpp`. They hold `ve::GrassLayout` and `ve::grass_layout()` and no `GrassField` type, so they are named `grass_layout.h/.cpp` here. The spec is updated to match in the same commit as this plan.

**Files:**
- Create: `extension/src/grass/grass_layout.h`
- Create: `extension/src/grass/grass_layout.cpp`
- Create: `extension/tests/test_grass_layout.cpp`

**Interfaces:**
- Consumes: `ve::GrassSettings` from Task 1; `ve::IVec3` from `extension/src/world/region.h`.
- Produces:
  - `struct ve::GrassParams` — 240-byte (fifteen `vec4`) std140-compatible POD uploaded to a UBO, mirrored by `GRASS_PARAMS_BLOCK` in `shaders/grass.glslh` (Task 4).
  - `struct ve::GrassLayout { IVec3 brick_min, brick_max; int ring_count; float ring_end_m[4]; int blades_per_brick[4]; int max_bricks; int estimated_blades; GrassParams params; }`.
  - `ve::GrassLayout ve::grass_layout(const GrassSettings &s, const float camera[3], const float view_proj[16])`.
  - `int ve::grass_ring_of(const GrassLayout &l, float distance_m)`.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_grass_layout.cpp`:

```cpp
#include <doctest/doctest.h>
#include "grass/grass_layout.h"
#include "grass/grass_settings.h"
#include "world/brick.h"
#include <cmath>
#include <cstring>

namespace {
// An identity view_proj: the frustum planes it yields are the six faces of the NDC cube in
// world space. Enough to pin the extraction's signs without dragging a projection in.
void identity(float m[16]) {
	std::memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}
} // namespace

TEST_CASE("the brick box covers reach horizontally and vertical_reach vertically") {
	ve::GrassSettings s;
	s.reach_m = 40.0f;
	s.vertical_reach_m = 10.0f;
	const float cam[3] = {0.0f, 100.0f, 0.0f};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	// 40 m / 0.8 m = 50 bricks each way, plus the brick the camera sits in.
	CHECK(l.brick_min.x <= -50);
	CHECK(l.brick_max.x >= 50);
	CHECK(l.brick_min.z <= -50);
	CHECK(l.brick_max.z >= 50);
	// 100 m / 0.8 m = brick 125; +-10 m is +-12.5 bricks.
	CHECK(l.brick_min.y <= 112);
	CHECK(l.brick_max.y >= 137);
	// The box is much shorter than it is wide -- that is the whole point of the split.
	const int wide = l.brick_max.x - l.brick_min.x;
	const int tall = l.brick_max.y - l.brick_min.y;
	CHECK(tall < wide);
}

TEST_CASE("each ring past the first drops 3 of every 4 blades") {
	ve::GrassSettings s;
	s.blades_per_brick = 16;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.ring_count == 4);
	CHECK(l.blades_per_brick[0] == 16);
	CHECK(l.blades_per_brick[1] == 4);
	CHECK(l.blades_per_brick[2] == 1);
	// Thinning never reaches zero: a ring that draws nothing is a hole, not a saving.
	CHECK(l.blades_per_brick[3] >= 1);
}

TEST_CASE("ring boundaries split the reach evenly and cover it") {
	ve::GrassSettings s;
	s.reach_m = 40.0f;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.ring_end_m[0] == doctest::Approx(10.0f));
	CHECK(l.ring_end_m[1] == doctest::Approx(20.0f));
	CHECK(l.ring_end_m[2] == doctest::Approx(30.0f));
	CHECK(l.ring_end_m[3] == doctest::Approx(40.0f));
	CHECK(ve::grass_ring_of(l, 0.0f) == 0);
	CHECK(ve::grass_ring_of(l, 9.9f) == 0);
	CHECK(ve::grass_ring_of(l, 10.1f) == 1);
	CHECK(ve::grass_ring_of(l, 39.9f) == 3);
	// Past the reach there is no ring; the caller must not place a blade there.
	CHECK(ve::grass_ring_of(l, 40.1f) < 0);
}

TEST_CASE("the blade estimate grows with reach and is capped by max_blades") {
	ve::GrassSettings s;
	s.reach_m = 20.0f;
	s.max_blades = 400000;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout near_l = ve::grass_layout(s, cam, vp);
	s.reach_m = 40.0f;
	const ve::GrassLayout far_l = ve::grass_layout(s, cam, vp);
	CHECK(far_l.estimated_blades > near_l.estimated_blades);

	s.max_blades = 1000;
	const ve::GrassLayout capped = ve::grass_layout(s, cam, vp);
	CHECK(capped.estimated_blades <= 1000);
}

TEST_CASE("disabled or zero-reach grass yields an empty box and no blades") {
	ve::GrassSettings s;
	s.enabled = false;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.max_bricks == 0);
	CHECK(l.estimated_blades == 0);
}

TEST_CASE("frustum planes point inward and are normalised") {
	ve::GrassSettings s;
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	for (int i = 0; i < 6; i++) {
		const float *p = l.params.planes[i];
		const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		CHECK(len == doctest::Approx(1.0f));
		// The NDC-cube origin is inside every plane, so every signed distance is positive.
		CHECK(p[3] > 0.0f);
	}
}

TEST_CASE("GrassParams is 240 bytes and its floats land where GLSL expects") {
	// Fifteen vec4: cam, planes[6], brick_min, brick_dim, ring_end, ring_blades, blade,
	// wind, style, limits. If this number moves, GRASS_PARAMS_BLOCK moved with it.
	CHECK(sizeof(ve::GrassParams) == 240);
	ve::GrassSettings s;
	const float cam[3] = {1.0f, 2.0f, 3.0f};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	const float *raw = reinterpret_cast<const float *>(&l.params);
	CHECK(raw[0] == doctest::Approx(1.0f));
	CHECK(raw[1] == doctest::Approx(2.0f));
	CHECK(raw[2] == doctest::Approx(3.0f));
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd extension && scons -Q test
```

Expected: FAIL — `fatal error: 'grass/grass_layout.h' file not found`.

- [ ] **Step 3: Write the layout header**

Create `extension/src/grass/grass_layout.h`:

```cpp
#pragma once
#include "grass/grass_settings.h"
#include "world/region.h" // ve::IVec3

namespace ve {

inline constexpr int kGrassRings = 4;

// Uploaded to a uniform buffer and mirrored by `GrassParams` in shaders/grass.glslh. Laid
// out as fifteen vec4 (240 bytes) so std140 padding cannot disagree with the C++ struct;
// test_grass_layout pins both the size and the first three floats.
//
// Order matters and is asserted: cam_pos first, so a shader reading params.cam.xyz gets the
// camera without an offset table.
struct GrassParams {
	float cam[4];          // xyz camera position, w reach_m
	float planes[6][4];    // frustum planes, inward, normalised: xyz normal, w distance
	int32_t brick_min[4];  // xyz inclusive brick coordinate, w unused
	int32_t brick_dim[4];  // xyz brick counts, w total brick count
	float ring_end[4];     // ring outer radius in metres
	int32_t ring_blades[4]; // candidate blades per brick, per ring
	float blade[4];        // width, height, height_jitter, slope_cos_min
	float wind[4];         // strength, speed, scale, time_seconds
	float style[4];        // flower_chance, gloss, unused, unused
	int32_t limits[4];     // max_blades, max_bricks, unused, unused
};

struct GrassLayout {
	IVec3 brick_min{};
	IVec3 brick_max{};
	int ring_count = kGrassRings;
	float ring_end_m[kGrassRings] = {0, 0, 0, 0};
	int blades_per_brick[kGrassRings] = {0, 0, 0, 0};
	// Threads stage 1 dispatches: the full brick box, one thread per brick.
	int max_bricks = 0;
	// Diagnostic upper bound on blades this layout can produce, already capped by
	// GrassSettings::max_blades. The instance buffer is sized from settings.max_blades,
	// not from this -- see the design doc's failure-modes section.
	int estimated_blades = 0;
	GrassParams params{};
};

// `settings` is clamped internally, so a caller may pass an unclamped snapshot.
// `view_proj` is column-major, the same order the compositor builds for the raymarcher.
GrassLayout grass_layout(const GrassSettings &settings, const float camera[3],
		const float view_proj[16]);

// Ring index for a distance, or -1 past the reach.
int grass_ring_of(const GrassLayout &l, float distance_m);

} // namespace ve
```

- [ ] **Step 4: Implement the layout**

Create `extension/src/grass/grass_layout.cpp`:

```cpp
#include "grass/grass_layout.h"
#include "world/brick.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {
namespace {

// Gribb-Hartmann: each plane is a sum or difference of two rows of the view-projection.
// `m` is column-major, so m[col * 4 + row]. Written out rather than looped because the sign
// pattern is the whole content of the function and a loop hides it.
void extract_planes(const float m[16], float out[6][4]) {
	auto row = [&](int r, int c) { return m[c * 4 + r]; };
	const float rows[4][4] = {
		{row(0, 0), row(0, 1), row(0, 2), row(0, 3)},
		{row(1, 0), row(1, 1), row(1, 2), row(1, 3)},
		{row(2, 0), row(2, 1), row(2, 2), row(2, 3)},
		{row(3, 0), row(3, 1), row(3, 2), row(3, 3)},
	};
	auto set = [&](int i, int r, float sign) {
		for (int k = 0; k < 4; k++) out[i][k] = rows[3][k] + sign * rows[r][k];
	};
	set(0, 0, 1.0f);  // left
	set(1, 0, -1.0f); // right
	set(2, 1, 1.0f);  // bottom
	set(3, 1, -1.0f); // top
	set(4, 2, 1.0f);  // near
	set(5, 2, -1.0f); // far
	for (int i = 0; i < 6; i++) {
		const float len = std::sqrt(out[i][0] * out[i][0] + out[i][1] * out[i][1] +
				out[i][2] * out[i][2]);
		if (len > 1e-12f) {
			for (int k = 0; k < 4; k++) out[i][k] /= len;
		} else {
			// Degenerate row: a plane that rejects nothing is the fail-soft answer, exactly
			// as LodCullPass falls back to drawing every page.
			out[i][0] = 0.0f; out[i][1] = 1.0f; out[i][2] = 0.0f; out[i][3] = 1e9f;
		}
	}
}

int floor_div_brick(float world) {
	return static_cast<int>(std::floor(world / kBrickSize));
}

} // namespace

int grass_ring_of(const GrassLayout &l, float distance_m) {
	for (int i = 0; i < l.ring_count; i++) {
		if (distance_m <= l.ring_end_m[i]) return i;
	}
	return -1;
}

GrassLayout grass_layout(const GrassSettings &settings, const float camera[3],
		const float view_proj[16]) {
	GrassSettings s = settings;
	clamp_grass_settings(&s);

	GrassLayout l;
	l.ring_count = kGrassRings;
	for (int i = 0; i < kGrassRings; i++) {
		l.ring_end_m[i] = s.reach_m * static_cast<float>(i + 1) / static_cast<float>(kGrassRings);
		// Drop 3 of every 4 per ring (Ghost of Tsushima's thinning), with a floor of one so
		// a far ring thins rather than disappears.
		l.blades_per_brick[i] = std::max(1, s.blades_per_brick >> (2 * i));
	}
	if (s.blades_per_brick == 0) {
		for (int i = 0; i < kGrassRings; i++) l.blades_per_brick[i] = 0;
	}

	const bool live = s.enabled && s.reach_m > 0.0f && s.blades_per_brick > 0 && s.max_blades > 0;
	if (live) {
		l.brick_min = IVec3{floor_div_brick(camera[0] - s.reach_m),
				floor_div_brick(camera[1] - s.vertical_reach_m),
				floor_div_brick(camera[2] - s.reach_m)};
		l.brick_max = IVec3{floor_div_brick(camera[0] + s.reach_m),
				floor_div_brick(camera[1] + s.vertical_reach_m),
				floor_div_brick(camera[2] + s.reach_m)};
	}

	const int dim_x = live ? (l.brick_max.x - l.brick_min.x + 1) : 0;
	const int dim_y = live ? (l.brick_max.y - l.brick_min.y + 1) : 0;
	const int dim_z = live ? (l.brick_max.z - l.brick_min.z + 1) : 0;
	l.max_bricks = dim_x * dim_y * dim_z;

	// Estimate: ground is a surface, so surface bricks in a ring go as its ANNULUS AREA over
	// the brick footprint, times a slack factor for slope (a hillside presents more bricks
	// per square metre of ground plan than a flat field does). Diagnostic only.
	const float kSlopeSlack = 2.0f;
	double blades = 0.0;
	float prev = 0.0f;
	for (int i = 0; i < kGrassRings && live; i++) {
		const float r = l.ring_end_m[i];
		const double area = 3.14159265358979 * (static_cast<double>(r) * r -
				static_cast<double>(prev) * prev);
		const double bricks = area / (kBrickSize * kBrickSize) * kSlopeSlack;
		blades += bricks * l.blades_per_brick[i];
		prev = r;
	}
	l.estimated_blades = static_cast<int>(std::min<double>(blades, s.max_blades));

	GrassParams &p = l.params;
	std::memset(&p, 0, sizeof(p));
	p.cam[0] = camera[0];
	p.cam[1] = camera[1];
	p.cam[2] = camera[2];
	p.cam[3] = s.reach_m;
	extract_planes(view_proj, p.planes);
	p.brick_min[0] = l.brick_min.x;
	p.brick_min[1] = l.brick_min.y;
	p.brick_min[2] = l.brick_min.z;
	p.brick_dim[0] = dim_x;
	p.brick_dim[1] = dim_y;
	p.brick_dim[2] = dim_z;
	p.brick_dim[3] = l.max_bricks;
	for (int i = 0; i < kGrassRings; i++) {
		p.ring_end[i] = l.ring_end_m[i];
		p.ring_blades[i] = l.blades_per_brick[i];
	}
	p.blade[0] = s.blade_width_m;
	p.blade[1] = s.blade_height_m;
	p.blade[2] = s.height_jitter;
	p.blade[3] = s.slope_cos_min;
	p.wind[0] = s.wind_strength;
	p.wind[1] = s.wind_speed;
	p.wind[2] = s.wind_scale;
	p.wind[3] = 0.0f; // the compositor stamps the frame's time before upload
	p.style[0] = s.flower_chance;
	p.style[1] = s.gloss;
	p.limits[0] = s.max_blades;
	p.limits[1] = l.max_bricks;
	return l;
}

} // namespace ve
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd extension && scons -Q test
```

Expected: PASS, all seven new `grass_layout` cases plus everything pre-existing.

If the `IVec3` aggregate initialisation fails to compile, check its definition in `extension/src/world/region.h` and match its field order rather than changing it.

- [ ] **Step 6: Commit**

```bash
git add extension/src/grass/grass_layout.h extension/src/grass/grass_layout.cpp \
        extension/tests/test_grass_layout.cpp
git commit -m "feat(grass): ring layout, brick box, frustum planes and GrassParams"
```

---

### Task 3: Extract the brick-atlas lookup into a shared header

The scatter needs the same atlas addressing the raymarcher uses. A second copy is how two implementations of one path drift apart, so this moves the functions verbatim. **Behaviour must not change** — this task's whole test is that the existing suite is unmoved.

**Files:**
- Create: `shaders/brick_atlas.glslh`
- Modify: `shaders/raymarch.comp.glsl:104-206` (delete the moved bodies, add the include)

**Interfaces:**
- Consumes: the bindings and push block a including shader must declare first — `region_map` (binding 8), `region_tables` (binding 9), `brick_flags` (binding 21), `sdf_atlas`, `mat_atlas`, `palette_buf` (binding 7), and a `pc` push block carrying `dims`, `region_origin` and `atlas_bricks`. This is the same "declare bindings, then include" convention `common.glslh` documents.
- Produces, for Task 5: `int region_slot_of(ivec3)`, `int slot_in_region(int, ivec3)`, `int slot_at(ivec3)`, `float brick_sdf(int, vec3)`, `float world_sdf(vec3)`, `uint material_at(vec3, ivec3, int)`, `uint brick_flag_word(int)`.

- [ ] **Step 1: Record the current state of the raymarch tests**

```bash
./build.sh -j$(sysctl -n hw.ncpu) && ./gdunit_tests.sh -a res://tests/test_raymarch.gd 2>&1 | tail -20
```

Write down the pass/fail counts. Per the standing note about baseline failures on this repo, some suites fail on clean `main` — what matters is that this task does not change the counts. If `res://tests/test_raymarch.gd` does not exist, list `tests/` and use the raymarch-related suites that do.

- [ ] **Step 2: Create the shared header**

Create `shaders/brick_atlas.glslh` containing, **copied verbatim**, the bodies currently at `shaders/raymarch.comp.glsl:104-216`: `region_slot_of`, `slot_in_region`, `slot_at`, `brick_sdf`, `world_sdf`, `sdf_near`, `material_at`, and `brick_flag_word`. Do **not** move `terrain_r8_fallback_normal` or `terrain_source_normal` — those depend on the field evaluator, which the scatter does not use.

Head the file with:

```glsl
// Brick-atlas addressing and sampling, shared by the raymarcher and the grass scatter so
// the two can never disagree about where a voxel lives.
//
// The INCLUDER must declare these first, exactly as common.glslh requires for the material
// arrays: `sdf_atlas` (LINEAR sampler), `mat_atlas` (NEAREST), `palette_buf`, `region_map`,
// `region_tables`, `brick_flags`, and a `pc` push/uniform block exposing `dims.xyz`
// (region window dimensions), `region_origin.xyz` (world origin in regions) and
// `atlas_bricks.xyz`. Include common.glslh and brick_layout.glslh before this file.
//
// NOTE: never put a literal include directive inside a comment here -- the loader matches
// include tokens anywhere in a line and would self-include.
```

- [ ] **Step 3: Replace the bodies in the raymarcher with the include**

In `shaders/raymarch.comp.glsl`, delete the moved function bodies and put the include where they were (after the binding declarations at lines 66-76, so the header can see them):

```glsl
#include "brick_atlas.glslh"
```

Leave `terrain_r8_fallback_normal` and `terrain_source_normal` where they are — they call `sdf_near` and `region_slot_of`, which the include now supplies above them.

- [ ] **Step 4: Verify the shader still compiles and behaves identically**

```bash
./build.sh -j$(sysctl -n hw.ncpu)
./gdunit_tests.sh -a res://tests/test_raymarch.gd 2>&1 | tail -20
```

Expected: the same pass/fail counts recorded in Step 1. A compile failure here is almost always ordering — the include must come after the bindings it uses.

- [ ] **Step 5: Commit**

This is a separate commit on purpose, so a later regression bisects onto a pure cut-and-paste.

```bash
git add shaders/brick_atlas.glslh shaders/raymarch.comp.glsl
git commit -m "refactor: extract brick-atlas addressing into brick_atlas.glslh"
```

---

### Task 4: The pass exists, runs, and is configurable

Wires an empty `GrassScatterPass` into the real frame so everything after this is tested against the shipping path rather than a probe. It produces zero blades; that is the deliverable.

**Files:**
- Create: `extension/src/grass/grass_settings_store.h`
- Create: `extension/src/grass/grass_settings_store.cpp`
- Create: `extension/src/render/grass_scatter_pass.h`
- Create: `extension/src/render/grass_scatter_pass.cpp`
- Create: `shaders/grass.glslh`
- Create: `shaders/grass_bricks.comp.glsl` (a stub that writes zero; Task 5 fills it in)
- Create: `tests/test_grass.gd`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (accessors + method bindings)
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp` (construction and teardown)
- Modify: `extension/src/raymarch_compositor.cpp` (the one block, after the LoD raster, before SSGI)
- Modify: `extension/src/debug/hooks.cpp` (one hook: `debug_grass_stats`)

**Interfaces:**
- Consumes: `ve::GrassSettings`, `ve::grass_layout`, `ve::GrassParams` (Tasks 1-2).
- Produces:
  - `ve::GrassSettingsStore` with `void set(const GrassSettings &)`, `GrassSettings get() const`, `bool set_value(const char *name, float v)`, `float value(const char *name) const`.
  - `godot::GrassScatterPass` with `bool initialize(RenderingDevice *)`, `void teardown()`, `bool run(RenderingDevice *, GpuAtlas &, const ve::GrassLayout &, float time_seconds)`, `RID instance_buffer() const`, `RID draw_args_buffer() const`, `int last_brick_count() const`, `int last_blade_count() const`, `int blade_high_water() const`.
  - `VoxelWorld::grass_scatter_pass()`, `VoxelWorld::grass_settings()`, `VoxelWorld::set_grass_value(String, float)`, `VoxelWorld::get_grass_value(String)`.
  - Hook `debug_grass_stats()` returning a `Dictionary` with keys `ran`, `bricks`, `blades`, `capacity`, `high_water`.

- [ ] **Step 1: Write the failing GPU test**

Create `tests/test_grass.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Same world construction every other GPU suite in this repo uses (see tests/test_ssao.gd):
# a local rendering device, physics off, streamed until the chunk queue goes quiet.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_grass_pass_runs_and_reports_its_capacity() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["ran"]).is_true()
	assert_int(d["capacity"]).is_greater(0)
	# Nothing is placed yet -- Task 5 fills the cull in, Task 6 the placement.
	assert_int(d["blades"]).is_greater_equal(0)

func test_grass_settings_round_trip_through_the_store() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 25.0)
	assert_float(w.get_grass_value("reach_m")).is_equal_approx(25.0, 0.001)
	# Out-of-range values are clamped by the store, not rejected.
	w.set_grass_value("reach_m", 1.0e9)
	assert_float(w.get_grass_value("reach_m")).is_less_equal(256.0)

func test_disabling_grass_zeroes_the_pass() -> void:
	var w := make_world()
	w.set_grass_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["bricks"]).is_equal(0)
	assert_int(d["blades"]).is_equal(0)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: FAIL — `Invalid call. Nonexistent function 'debug_grass_stats'`.

- [ ] **Step 3: Write the settings store**

Create `extension/src/grass/grass_settings_store.h`:

```cpp
#pragma once
#include "grass/grass_settings.h"
#include <mutex>

namespace ve {

// Mirrors the SHAPE of RenderOrchestrator's beauty_mutex_/beauty_snapshot() pair without
// joining BeautySettings: grass is its own module and the beauty stack must not grow a
// grass field (design doc section 7). No godot-cpp here -- this file is in the native test
// target.
class GrassSettingsStore {
public:
	void set(const GrassSettings &s);
	GrassSettings get() const;

	// Name-addressed access, so the demo menu and the debug hooks need no per-field
	// plumbing. Booleans are carried as 0.0 / 1.0. Returns false for an unknown name.
	bool set_value(const char *name, float v);
	float value(const char *name) const;

private:
	mutable std::mutex mutex_;
	GrassSettings settings_;
};

} // namespace ve
```

Create `extension/src/grass/grass_settings_store.cpp`:

```cpp
#include "grass/grass_settings_store.h"
#include <cstring>

namespace ve {
namespace {
// One table, two directions: a name that can be set can be read, and neither list can drift
// from the other.
struct FloatField { const char *name; float GrassSettings::*member; };
const FloatField kFloatFields[] = {
	{"reach_m", &GrassSettings::reach_m},
	{"vertical_reach_m", &GrassSettings::vertical_reach_m},
	{"blade_width_m", &GrassSettings::blade_width_m},
	{"blade_height_m", &GrassSettings::blade_height_m},
	{"height_jitter", &GrassSettings::height_jitter},
	{"slope_cos_min", &GrassSettings::slope_cos_min},
	{"wind_strength", &GrassSettings::wind_strength},
	{"wind_speed", &GrassSettings::wind_speed},
	{"wind_scale", &GrassSettings::wind_scale},
	{"flower_chance", &GrassSettings::flower_chance},
	{"gloss", &GrassSettings::gloss},
};
struct IntField { const char *name; int GrassSettings::*member; };
const IntField kIntFields[] = {
	{"blades_per_brick", &GrassSettings::blades_per_brick},
	{"max_blades", &GrassSettings::max_blades},
};
} // namespace

void GrassSettingsStore::set(const GrassSettings &s) {
	GrassSettings copy = s;
	clamp_grass_settings(&copy);
	std::lock_guard<std::mutex> lock(mutex_);
	settings_ = copy;
}

GrassSettings GrassSettingsStore::get() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return settings_;
}

bool GrassSettingsStore::set_value(const char *name, float v) {
	if (!name) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (std::strcmp(name, "enabled") == 0) {
		settings_.enabled = v != 0.0f;
		clamp_grass_settings(&settings_);
		return true;
	}
	for (const FloatField &f : kFloatFields) {
		if (std::strcmp(name, f.name) == 0) {
			settings_.*(f.member) = v;
			clamp_grass_settings(&settings_);
			return true;
		}
	}
	for (const IntField &f : kIntFields) {
		if (std::strcmp(name, f.name) == 0) {
			settings_.*(f.member) = static_cast<int>(v);
			clamp_grass_settings(&settings_);
			return true;
		}
	}
	return false;
}

float GrassSettingsStore::value(const char *name) const {
	if (!name) return 0.0f;
	std::lock_guard<std::mutex> lock(mutex_);
	if (std::strcmp(name, "enabled") == 0) return settings_.enabled ? 1.0f : 0.0f;
	for (const FloatField &f : kFloatFields) {
		if (std::strcmp(name, f.name) == 0) return settings_.*(f.member);
	}
	for (const IntField &f : kIntFields) {
		if (std::strcmp(name, f.name) == 0) return static_cast<float>(settings_.*(f.member));
	}
	return 0.0f;
}

} // namespace ve
```

- [ ] **Step 4: Add native tests for the store and run them**

Append to `extension/tests/test_grass_settings.cpp`:

```cpp
#include "grass/grass_settings_store.h"

TEST_CASE("the store round-trips every named field and clamps on the way in") {
	ve::GrassSettingsStore store;
	CHECK(store.set_value("reach_m", 25.0f));
	CHECK(store.value("reach_m") == doctest::Approx(25.0f));
	CHECK(store.set_value("reach_m", 1e9f));
	CHECK(store.value("reach_m") <= 256.0f);
	CHECK(store.set_value("blades_per_brick", 8.0f));
	CHECK(store.value("blades_per_brick") == doctest::Approx(8.0f));
	CHECK(store.set_value("enabled", 0.0f));
	CHECK(store.value("enabled") == doctest::Approx(0.0f));
	CHECK(store.get().enabled == false);
	CHECK_FALSE(store.set_value("no_such_knob", 1.0f));
}
```

```bash
cd extension && scons -Q test
```

Expected: PASS.

- [ ] **Step 5: Write the shared grass GLSL header and the stage-1 stub**

Create `shaders/grass.glslh`:

```glsl
// Shared grass definitions: the GrassParams mirror and the per-blade hash. Mirrors
// ve::GrassParams in extension/src/grass/grass_layout.h, which test_grass_layout pins at
// 256 bytes with cam.xyz first.
//
// NOTE: never put a literal include directive inside a comment in this file -- the loader
// matches include tokens anywhere in a line and would self-include.

struct GrassBlade {
	vec4 a; // xyz world position, w height
	vec4 b; // x packed ground normal, y hash as float bits, z lean angle, w clump weight
};

#define GRASS_PARAMS_BLOCK \
	vec4 cam;           \
	vec4 planes[6];     \
	ivec4 brick_min;    \
	ivec4 brick_dim;    \
	vec4 ring_end;      \
	ivec4 ring_blades;  \
	vec4 blade;         \
	vec4 wind;          \
	vec4 style;         \
	ivec4 limits;

// PCG-style integer hash. Deterministic in world space, so a blade keeps its identity as
// the camera moves and the field does not shimmer.
uint grass_hash(uint x) {
	x = x * 747796405u + 2891336453u;
	uint w = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
	return (w >> 22u) ^ w;
}

uint grass_hash3(ivec3 c, uint salt) {
	return grass_hash(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^
			uint(c.z) * 83492791u ^ salt);
}

float grass_unit(uint h) { return float(h & 0x00FFFFFFu) / float(0x01000000u); }

// True when the brick's world-space AABB is outside any frustum plane.
bool grass_brick_culled(vec3 lo, vec3 hi, vec4 planes[6]) {
	for (int i = 0; i < 6; i++) {
		vec3 p = mix(lo, hi, step(vec3(0.0), planes[i].xyz));
		if (dot(planes[i].xyz, p) + planes[i].w < 0.0) return true;
	}
	return false;
}
```

Create `shaders/grass_bricks.comp.glsl` as a stub that establishes the bindings and writes zeros. Task 5 replaces the body:

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "grass.glslh"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;
layout(set = 0, binding = 1, std430) writeonly buffer BrickList { uint v[]; } brick_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint brick_count; uint blade_count;
		uint high_water; uint pad; } counters;
layout(set = 0, binding = 3, std430) writeonly buffer Dispatch { uint x, y, z; } dispatch_args;

void main() {
	// Stub: Task 5 fills in the brick cull. One thread does the bookkeeping so the buffers
	// are DEFINED rather than merely allocated -- on this machine a fresh RD buffer reads
	// back as zero, so an undefined buffer and an empty one are indistinguishable and the
	// difference has to be made explicit here.
	if (gl_GlobalInvocationID.x != 0u) return;
	counters.brick_count = 0u;
	counters.blade_count = 0u;
	dispatch_args.x = 0u;
	dispatch_args.y = 1u;
	dispatch_args.z = 1u;
	brick_list.v[0] = 0u;
}
```

- [ ] **Step 6: Write the scatter pass**

Create `extension/src/render/grass_scatter_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "grass/grass_layout.h"

namespace godot {

class GpuAtlas;

// GPU-driven grass placement: stage 1 culls resident grass-surface bricks into a compacted
// list, stage 2 places blades inside them. Produces an instance buffer and the indirect
// draw args GrassRasterPass consumes. Modelled on SsaoPass for shader loading/teardown and
// on LodCullPass for the two-stage compute shape.
class GrassScatterPass {
public:
	~GrassScatterPass();
	bool initialize(RenderingDevice *rd);
	void teardown();

	// Returns false on any failure; the caller cancels the timing marker and skips grass.
	// Never aborts the frame -- grass is decorative (design doc section 8).
	bool run(RenderingDevice *rd, GpuAtlas &atlas, const ve::GrassLayout &layout,
			float time_seconds);

	RID instance_buffer() const { return instances_; }
	RID draw_args_buffer() const { return draw_args_; }

	// Read back after run(); all three are what the SHIPPING pass wrote, which is the only
	// thing debug_grass_stats() is allowed to report.
	int last_brick_count() const { return last_brick_count_; }
	int last_blade_count() const { return last_blade_count_; }
	int blade_high_water() const { return blade_high_water_; }
	int capacity() const { return capacity_; }

private:
	bool ensure_buffers(RenderingDevice *rd, int max_blades, int max_bricks);
	bool ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas);
	void read_back_counters(RenderingDevice *rd);

	RenderingDevice *rd_ = nullptr;
	RID bricks_shader_, bricks_pipeline_;
	RID scatter_shader_, scatter_pipeline_;
	RID params_ubo_, brick_list_, counters_, dispatch_args_, instances_, draw_args_;
	RID bricks_uset_, scatter_uset_;
	int capacity_ = 0;
	int brick_capacity_ = 0;
	int last_brick_count_ = 0;
	int last_blade_count_ = 0;
	int blade_high_water_ = 0;
};

} // namespace godot
```

Create `extension/src/render/grass_scatter_pass.cpp`. For shader loading and teardown, copy the structure of `extension/src/render/ssao_pass.cpp:28-70` verbatim, substituting the shader path and the class name; do this twice, once for `res://shaders/grass_bricks.comp.glsl` and once for `res://shaders/grass_scatter.comp.glsl` (which Task 6 creates — until then, `initialize()` must tolerate its absence by leaving `scatter_pipeline_` invalid and `run()` must skip stage 2 when it is). The parts that are not boilerplate:

```cpp
bool GrassScatterPass::ensure_buffers(RenderingDevice *rd, int max_blades, int max_bricks) {
	if (max_blades <= 0 || max_bricks <= 0) return false;
	if (instances_.is_valid() && capacity_ == max_blades && brick_capacity_ >= max_bricks)
		return true;
	for (RID *r : {&instances_, &brick_list_, &counters_, &dispatch_args_, &draw_args_,
			&params_ubo_, &bricks_uset_, &scatter_uset_}) {
		if (r->is_valid()) rd->free_rid(*r);
		*r = RID();
	}
	// 32 bytes per blade: two vec4 (design doc section 5).
	instances_ = rd->storage_buffer_create(static_cast<uint32_t>(max_blades) * 32u);
	brick_list_ = rd->storage_buffer_create(static_cast<uint32_t>(max_bricks) * 4u);
	counters_ = rd->storage_buffer_create(16u);
	dispatch_args_ = rd->storage_buffer_create(12u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	// Non-indexed indirect draw args: vertexCount, instanceCount, firstVertex, firstInstance.
	draw_args_ = rd->storage_buffer_create(16u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	params_ubo_ = rd->uniform_buffer_create(sizeof(ve::GrassParams));
	capacity_ = max_blades;
	brick_capacity_ = max_bricks;
	return instances_.is_valid() && brick_list_.is_valid() && counters_.is_valid() &&
			dispatch_args_.is_valid() && draw_args_.is_valid() && params_ubo_.is_valid();
}

bool GrassScatterPass::run(RenderingDevice *rd, GpuAtlas &atlas,
		const ve::GrassLayout &layout, float time_seconds) {
	last_brick_count_ = 0;
	last_blade_count_ = 0;
	if (!rd_ || rd != rd_ || !bricks_pipeline_.is_valid()) return false;
	if (layout.max_bricks <= 0 || layout.params.limits[0] <= 0) return true; // disabled: a
	// successful no-op, not a failure. The caller still ends its timing marker.
	if (!ensure_buffers(rd, layout.params.limits[0], layout.max_bricks)) return false;
	if (!ensure_uniform_sets(rd, atlas)) return false;

	ve::GrassParams params = layout.params;
	params.wind[3] = time_seconds;
	PackedByteArray ubo;
	ubo.resize(sizeof(ve::GrassParams));
	std::memcpy(ubo.ptrw(), &params, sizeof(ve::GrassParams));
	rd->buffer_update(params_ubo_, 0, ubo.size(), ubo);

	// Clear the counters explicitly. A fresh RD buffer reads back as zero on this machine,
	// so "the count was zero" must mean the pass wrote zero -- never that nobody wrote.
	PackedByteArray zero;
	zero.resize(16);
	zero.fill(0);
	rd->buffer_update(counters_, 0, 16, zero);

	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, bricks_pipeline_);
	rd->compute_list_bind_uniform_set(list, bricks_uset_, 0);
	rd->compute_list_dispatch(list, (layout.max_bricks + 63) / 64, 1, 1);
	if (scatter_pipeline_.is_valid()) {
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, scatter_pipeline_);
		rd->compute_list_bind_uniform_set(list, scatter_uset_, 0);
		rd->compute_list_dispatch_indirect(list, dispatch_args_, 0);
	}
	rd->compute_list_end();
	read_back_counters(rd);
	return true;
}

void GrassScatterPass::read_back_counters(RenderingDevice *rd) {
	const PackedByteArray data = rd->buffer_get_data(counters_, 0, 16);
	if (data.size() < 16) return;
	const uint32_t *c = reinterpret_cast<const uint32_t *>(data.ptr());
	last_brick_count_ = static_cast<int>(c[0]);
	last_blade_count_ = static_cast<int>(std::min<uint32_t>(c[1], static_cast<uint32_t>(capacity_)));
	blade_high_water_ = std::max(blade_high_water_, static_cast<int>(c[1]));
}
```

`ensure_uniform_sets` follows `SsaoPass::ensure_uniform_set` exactly in shape: instantiate one `RDUniform` per binding, `UNIFORM_TYPE_UNIFORM_BUFFER` for binding 0 and `UNIFORM_TYPE_STORAGE_BUFFER` for bindings 1-3, then `rd->uniform_set_create(Array::make(...), bricks_shader_, 0)`. Cache it against the RIDs it references and rebuild when any changes, as `SsaoPass` does with its `key_*` fields.

> `read_back_counters` calls `buffer_get_data`, which stalls. That is acceptable *only* because the counters are what every test reads and what the overflow report needs. Task 10 revisits it if it shows up in the A/B/A measurement.

- [ ] **Step 7: Construct and tear down the pass in the orchestrator**

In `extension/src/render/orchestrator.h`, forward-declare `class GrassScatterPass;` beside the other pass declarations, add a `GrassScatterPass *grass_scatter_pass_ = nullptr;` member, and an accessor `GrassScatterPass *grass_scatter_pass() const { return grass_scatter_pass_; }`. Add `ve::GrassSettingsStore grass_settings_;` with `ve::GrassSettings grass_settings() const { return grass_settings_.get(); }`, `bool set_grass_value(const char *n, float v) { return grass_settings_.set_value(n, v); }` and `float grass_value(const char *n) const { return grass_settings_.value(n); }`.

In `ensure_gpu_graph()`, immediately after the `lod_cull_pass_` block (`orchestrator.cpp:265-271`), add:

```cpp
	grass_scatter_pass_ = new GrassScatterPass();
	if (!grass_scatter_pass_->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: grass initialization failed; continuing "
				"without grass (safe fail-soft: the field is simply bare)");
		delete grass_scatter_pass_;
		grass_scatter_pass_ = nullptr;
	}
```

In `teardown_render_passes()`, delete it beside `outline_pass_` — before the atlas teardown, because its uniform set references atlas RIDs:

```cpp
	if (grass_scatter_pass_) { delete grass_scatter_pass_; grass_scatter_pass_ = nullptr; }
```

- [ ] **Step 8: Expose it on VoxelWorld**

In `extension/src/voxel_world.h` add `#include "grass/grass_settings.h"` (the accessor returns `ve::GrassSettings` by value, so a forward declaration will not do) and, beside the existing pass accessors:

```cpp
	GrassScatterPass *grass_scatter_pass() const;
	ve::GrassSettings grass_settings() const;
	bool set_grass_value(const String &name, float v);
	float get_grass_value(const String &name) const;
```

In `extension/src/voxel_world.cpp` implement each as a one-line delegation to the orchestrator (matching how the other pass accessors delegate), and bind the two scriptable ones in `_bind_methods()`:

```cpp
	ClassDB::bind_method(D_METHOD("set_grass_value", "name", "value"), &VoxelWorld::set_grass_value);
	ClassDB::bind_method(D_METHOD("get_grass_value", "name"), &VoxelWorld::get_grass_value);
```

`set_grass_value` / `get_grass_value` convert with `name.utf8().get_data()`.

- [ ] **Step 9: Wire it into the frame**

In `extension/src/raymarch_compositor.cpp`, after the whole `if (world->lod_pool() && lod_raster && world->material_atlas()) { ... }` block closes and **before** `SsgiPass *ssgi = world->ssgi_pass();`, insert:

```cpp
	// Grass: one block, between the far field and the beauty stack. Blades write the same
	// G-buffer channels the far field writes, so everything below shades them unchanged.
	if (GrassScatterPass *grass = world->grass_scatter_pass()) {
		timings->begin(rd, "grass");
		float grass_cam[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
		float grass_vp[16];
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) grass_vp[c * 4 + r] = view_proj.columns[c][r];
		const ve::GrassLayout gl = ve::grass_layout(world->grass_settings(), grass_cam, grass_vp);
		if (grass->run(rd, *atlas, gl, static_cast<float>(world->beauty_frame()) / 60.0f))
			timings->end(rd, "grass");
		else
			timings->cancel("grass");
	}
```

Add `#include "render/grass_scatter_pass.h"` and `#include "grass/grass_layout.h"` to the include block at the top of the file.

- [ ] **Step 10: Add the one debug hook**

In `extension/src/debug/hooks.cpp`, bind and implement exactly one grass method. It reads back what the **real** pass wrote during a **real** frame and rebuilds nothing:

```cpp
	ClassDB::bind_method(D_METHOD("debug_grass_stats"), &VoxelDebugHooks::debug_grass_stats);
```

```cpp
Dictionary VoxelDebugHooks::debug_grass_stats() {
	Dictionary d;
	d["ran"] = false;
	d["bricks"] = 0;
	d["blades"] = 0;
	d["capacity"] = 0;
	d["high_water"] = 0;
	VoxelWorld *w = world();
	if (!w) return d;
	GrassScatterPass *g = w->grass_scatter_pass();
	if (!g) return d;
	d["ran"] = true;
	d["bricks"] = g->last_brick_count();
	d["blades"] = g->last_blade_count();
	d["capacity"] = g->capacity();
	d["high_water"] = g->blade_high_water();
	return d;
}
```

Declare it in the hooks header beside the other probe declarations. **Do not** add any hook that re-runs the scatter with synthesised inputs — that is the failure mode this file already has 6063 lines of.

- [ ] **Step 11: Build and run both suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu) --test
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: the three `test_grass.gd` cases PASS. `capacity` is non-zero, `blades` is zero (nothing is placed yet), and disabling grass zeroes both counters.

- [ ] **Step 12: Commit**

```bash
git add extension/src/grass extension/src/render/grass_scatter_pass.h \
        extension/src/render/grass_scatter_pass.cpp extension/src/render/orchestrator.h \
        extension/src/render/orchestrator.cpp extension/src/voxel_world.h \
        extension/src/voxel_world.cpp extension/src/raymarch_compositor.cpp \
        extension/src/debug/hooks.cpp extension/tests/test_grass_settings.cpp \
        shaders/grass.glslh shaders/grass_bricks.comp.glsl tests/test_grass.gd
git commit -m "feat(grass): scatter pass skeleton wired into the frame"
```

---

### Task 5: Stage 1 — cull resident grass-surface bricks

**Files:**
- Modify: `shaders/grass_bricks.comp.glsl` (replace the stub body)
- Modify: `extension/src/render/grass_scatter_pass.cpp` (bind the atlas resources stage 1 needs)
- Modify: `tests/test_grass.gd` (add cases)

**Interfaces:**
- Consumes: `brick_atlas.glslh` from Task 3 (`slot_at`, `brick_flag_word`); `grass.glslh` from Task 4 (`grass_brick_culled`, `GRASS_PARAMS_BLOCK`).
- Produces: `brick_list` — a compacted array of packed brick coordinates, one `uint` per surviving brick, packed as `(x + 512) | ((y + 512) << 10) | ((z + 512) << 20)` relative to `pc.brick_min`; `counters.brick_count`; `dispatch_args` sized for stage 2.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_grass.gd`:

```gdscript
func test_stage_one_finds_surface_bricks_under_the_camera() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	# Standing on terrain at (30, 56.2, 30) with a 40 m reach, the brick box must contain
	# resident surface bricks. Zero here means the cull rejected everything.
	assert_int(d["bricks"]).is_greater(0)

func test_stage_one_finds_nothing_far_above_the_world() -> void:
	var w := make_world()
	# Stream around a point 2 km up: nothing is resident within the vertical reach.
	for i in range(30):
		w.hooks().debug_stream_frame(Vector3(30.0, 2000.0, 30.0))
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["bricks"]).is_equal(0)

func test_a_shorter_reach_culls_more_bricks() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 40.0)
	var wide: int = w.hooks().debug_grass_stats()["bricks"]
	w.set_grass_value("reach_m", 10.0)
	var narrow: int = w.hooks().debug_grass_stats()["bricks"]
	assert_int(narrow).is_less(wide)
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: `test_stage_one_finds_surface_bricks_under_the_camera` FAILS with `bricks` equal to 0 (the stub writes zero). The "far above" case may pass vacuously — that is fine, it guards the opposite direction.

- [ ] **Step 3: Replace the stub with the real cull**

Rewrite `shaders/grass_bricks.comp.glsl`:

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "grass.glslh"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;
layout(set = 0, binding = 1, std430) writeonly buffer BrickList { uint v[]; } brick_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint brick_count; uint blade_count;
		uint high_water; uint pad; } counters;
layout(set = 0, binding = 3, std430) buffer Dispatch { uint x, y, z; } dispatch_args;
layout(set = 0, binding = 4, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 5, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 6, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;

// brick_atlas.glslh addresses the window through `pc.dims`, `pc.region_origin` and
// `pc.atlas_bricks`. This pass carries them in the same uniform block under different
// names, so alias them before the include rather than duplicating the addressing.
#define GRASS_REGION_DIMS pc.brick_dim
#include "brick_atlas.glslh"

void main() {
	uint i = gl_GlobalInvocationID.x;
	// Thread 0 seeds the stage-2 dispatch dimensions that do not depend on the count.
	if (i == 0u) { dispatch_args.y = 1u; dispatch_args.z = 1u; }
	if (i >= uint(pc.brick_dim.w)) return;

	ivec3 dim = pc.brick_dim.xyz;
	ivec3 local = ivec3(int(i) % dim.x, (int(i) / dim.x) % dim.y, int(i) / (dim.x * dim.y));
	ivec3 brick = pc.brick_min.xyz + local;

	int slot = slot_at(brick);
	if (slot < 0) return;                       // not resident
	if (!brick_straddles_surface(slot)) return; // no surface crossing: nothing to stand on

	vec3 lo = vec3(brick) * BRICK_SIZE;
	vec3 hi = lo + vec3(BRICK_SIZE);
	if (grass_brick_culled(lo, hi, pc.planes)) return;

	// Distance cull against the reach, measured to the brick's nearest point so a brick
	// straddling the boundary is kept rather than flickering.
	vec3 nearest = clamp(pc.cam.xyz, lo, hi);
	if (distance(nearest, pc.cam.xyz) > pc.cam.w) return;

	uint out_index = atomicAdd(counters.brick_count, 1u);
	if (out_index >= uint(pc.limits.y)) return; // full: drop, never scribble
	brick_list.v[out_index] = uint(local.x + 512) | (uint(local.y + 512) << 10) |
			(uint(local.z + 512) << 20);

	// Stage 2 runs one workgroup of 64 threads per brick, so the dispatch width is the
	// brick count. atomicMax rather than a store: every thread that appends may be the last.
	atomicMax(dispatch_args.x, out_index + 1u);
}
```

Add the surface test to `shaders/brick_atlas.glslh`, next to `brick_flag_word`, so both shaders share one definition of what "has a surface in it" means:

```glsl
// A brick worth placing anything on: its flag word says the surface crosses it. Mirrors
// ve::brick_flags_from_mips; test_brick_flags_gpu pins GPU == CPU for the word itself.
bool brick_straddles_surface(int slot) {
	return (brick_flag_word(slot) & BRICK_FLAG_STRADDLES) != 0u;
}
```

Check the actual flag constant name in `extension/src/world/brick_flags.h` and use it; if the flags are expressed as "empty" / "solid" rather than "straddles", write the predicate as `!empty && !solid` and say so in the comment.

- [ ] **Step 4: Bind the atlas resources stage 1 needs**

In `GrassScatterPass::ensure_uniform_sets`, add bindings 4, 5 and 6 for the region map, region tables and brick flags buffers. Get their RIDs from `GpuAtlas` — check `extension/src/render/gpu_atlas.h` for the accessor names and use the same ones `RaymarchPass` uses when it builds its own uniform set (`extension/src/render/raymarch_pass.cpp`). Also add the `sdf_atlas`, `mat_atlas` and `palette_buf` bindings that `brick_atlas.glslh` requires, even though stage 1 does not sample them — a declared-but-unused binding still has to be provided.

Cache the set against those RIDs, as `SsaoPass` does.

- [ ] **Step 5: Run tests to verify they pass**

```bash
./build.sh -j$(sysctl -n hw.ncpu)
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: all six cases PASS.

If `bricks` is still 0, the likeliest causes in order: the `GRASS_REGION_DIMS` alias does not match what `brick_atlas.glslh` actually reads (the header expects `pc.dims` / `pc.region_origin`, so the uniform block may need those exact field names instead of an alias); the region window origin in `GrassParams` is in bricks where the header wants regions; or the flag predicate has the wrong polarity. Check them in that order — the first is a naming problem, the second a units problem, the third a one-line flip.

- [ ] **Step 6: Commit**

```bash
git add shaders/grass_bricks.comp.glsl shaders/brick_atlas.glslh \
        extension/src/render/grass_scatter_pass.cpp tests/test_grass.gd
git commit -m "feat(grass): cull resident grass-surface bricks"
```

---

### Task 6: Stage 2 — place blades

**Files:**
- Create: `shaders/grass_scatter.comp.glsl`
- Modify: `extension/src/render/grass_scatter_pass.cpp` (load stage 2, build its uniform set)
- Modify: `tests/test_grass.gd`

**Interfaces:**
- Consumes: `brick_list` and `counters.brick_count` from Task 5; `material_at`, `world_sdf`, `slot_at` from Task 3.
- Produces: `instances` — `counters.blade_count` records of `GrassBlade` (32 bytes); `draw_args` — `{vertexCount = blade_count * 3, instanceCount = 1, firstVertex = 0, firstInstance = 0}`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_grass.gd`:

```gdscript
func test_blades_appear_on_grass_terrain() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_greater(0)

func test_blade_count_falls_as_the_camera_retreats() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 40.0)
	var near_count: int = w.hooks().debug_grass_stats()["blades"]
	w.set_grass_value("reach_m", 12.0)
	var far_count: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(far_count).is_less(near_count)

func test_density_follows_blades_per_brick() -> void:
	var w := make_world()
	w.set_grass_value("blades_per_brick", 16.0)
	var dense: int = w.hooks().debug_grass_stats()["blades"]
	w.set_grass_value("blades_per_brick", 4.0)
	var sparse: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(sparse).is_less(dense)

func test_the_blade_count_clamps_at_capacity_instead_of_overflowing() -> void:
	var w := make_world()
	w.set_grass_value("max_blades", 64.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_less_equal(64)
	# The high-water mark still reports what the frame WANTED, so an overflow is visible
	# rather than silent.
	assert_int(d["high_water"]).is_greater_equal(d["blades"])

# Grass refuses steep surfaces. Raising the threshold past vertical must leave nothing.
func test_no_blades_survive_an_impossible_slope_threshold() -> void:
	var w := make_world()
	w.set_grass_value("slope_cos_min", 1.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_equal(0)
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: `test_blades_appear_on_grass_terrain` FAILS with `blades` equal to 0 — stage 2 does not exist yet.

- [ ] **Step 3: Write the placement shader**

Create `shaders/grass_scatter.comp.glsl`:

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "shade.glslh"
#include "grass.glslh"

// One workgroup per surviving brick; each thread is one candidate blade. 64 threads is the
// ceiling on GrassSettings::blades_per_brick, so a brick never needs a second group.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;
layout(set = 0, binding = 1, std430) readonly buffer BrickList { uint v[]; } brick_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint brick_count; uint blade_count;
		uint high_water; uint pad; } counters;
layout(set = 0, binding = 3, std430) buffer DrawArgs { uint vertex_count; uint instance_count;
		uint first_vertex; uint first_instance; } draw_args;
layout(set = 0, binding = 4, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 5, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 6, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;
layout(set = 0, binding = 7, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 8) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 9) uniform usampler3D mat_atlas;
layout(set = 0, binding = 10, std430) writeonly buffer Instances { GrassBlade b[]; } instances;

#define GRASS_REGION_DIMS pc.brick_dim
#include "brick_atlas.glslh"

const uint GRASS_MATERIAL = 1u; // grass_01, ve::kMaterials[0]

// Central differences over one voxel. Cheaper and steadier than the field evaluator, and a
// blade only needs to know which way is up, not a shading normal.
vec3 surface_normal(vec3 p) {
	const float e = VOXEL_SIZE;
	return normalize(vec3(
		world_sdf(p + vec3(e, 0, 0)) - world_sdf(p - vec3(e, 0, 0)),
		world_sdf(p + vec3(0, e, 0)) - world_sdf(p - vec3(0, e, 0)),
		world_sdf(p + vec3(0, 0, e)) - world_sdf(p - vec3(0, 0, e))));
}

void main() {
	uint brick_index = gl_WorkGroupID.x;
	if (brick_index >= counters.brick_count) return;

	uint packed = brick_list.v[brick_index];
	ivec3 local = ivec3(int(packed & 0x3FFu) - 512, int((packed >> 10) & 0x3FFu) - 512,
			int((packed >> 20) & 0x3FFu) - 512);
	ivec3 brick = pc.brick_min.xyz + local;
	vec3 base = vec3(brick) * BRICK_SIZE;

	// Ring from the brick's centre, so every blade in a brick agrees on its budget.
	float d = distance(base + vec3(BRICK_SIZE * 0.5), pc.cam.xyz);
	int ring = 3;
	for (int i = 0; i < 4; i++) { if (d <= pc.ring_end[i]) { ring = i; break; } }
	if (d > pc.ring_end[3]) return;
	uint budget = uint(pc.ring_blades[ring]);
	uint lane = gl_LocalInvocationID.x;
	if (lane >= budget) return;

	// Jittered XZ inside the brick. The hash is seeded from the WORLD brick coordinate, so
	// a blade keeps its position as the camera moves and the field does not crawl.
	uint h = grass_hash3(brick, lane * 2654435761u);
	float jx = grass_unit(h);
	float jz = grass_unit(grass_hash(h ^ 0x9E3779B9u));
	vec3 column = base + vec3(jx * BRICK_SIZE, 0.0, jz * BRICK_SIZE);

	// Find the crossing inside this brick's 0.8 m span: eight steps to bracket it, then
	// four bisections. Bounded by construction -- the brick is known to straddle.
	float y0 = base.y;
	float y1 = base.y + BRICK_SIZE;
	float s0 = world_sdf(vec3(column.x, y0, column.z));
	bool found = false;
	for (int i = 1; i <= 8; i++) {
		float y = mix(y0, y1, float(i) / 8.0);
		float s = world_sdf(vec3(column.x, y, column.z));
		if (s0 * s <= 0.0) { y1 = y; found = true; break; }
		y0 = y;
		s0 = s;
	}
	if (!found) return; // this column misses the surface even though the brick straddles
	for (int i = 0; i < 4; i++) {
		float ym = 0.5 * (y0 + y1);
		if (world_sdf(vec3(column.x, ym, column.z)) * s0 <= 0.0) y1 = ym; else y0 = ym;
	}
	vec3 p = vec3(column.x, 0.5 * (y0 + y1), column.z);

	vec3 n = surface_normal(p);
	if (n.y < pc.blade.w) return; // too steep: no grass on cliff faces

	int slot = slot_at(ivec3(floor(p / BRICK_SIZE)));
	if (slot < 0) return;
	if (material_at(p, ivec3(floor(p / BRICK_SIZE)), slot) != GRASS_MATERIAL) return;

	uint index = atomicAdd(counters.blade_count, 1u);
	if (index >= uint(pc.limits.x)) return; // at capacity: drop, never scribble

	// Clump noise at world XZ: neighbouring blades share height and colour, which is what
	// makes a field patchy rather than a lawn.
	ivec3 clump_cell = ivec3(int(floor(p.x * 0.35)), 0, int(floor(p.z * 0.35)));
	float clump = grass_unit(grass_hash3(clump_cell, 0x5BD1u));
	float jitter = (grass_unit(grass_hash(h ^ 0x85EBCA6Bu)) * 2.0 - 1.0) * pc.blade.z;
	float height = pc.blade.y * (1.0 + jitter) * mix(0.7, 1.15, clump);
	float lean = grass_unit(grass_hash(h ^ 0xC2B2AE35u)) * 6.2831853;

	GrassBlade blade;
	blade.a = vec4(p, height);
	blade.b = vec4(float(oct_encode_snorm8(n)), uintBitsToFloat(h), lean, clump);
	instances.b[index] = blade;

	// Three vertices per blade. atomicMax, not a store: any appending thread may be last.
	atomicMax(draw_args.vertex_count, (index + 1u) * 3u);
	draw_args.instance_count = 1u;
}
```

- [ ] **Step 4: Load stage 2 and build its uniform set**

In `GrassScatterPass::initialize`, load `res://shaders/grass_scatter.comp.glsl` with the same block used for stage 1 and create `scatter_pipeline_`. Build `scatter_uset_` against `scatter_shader_` with bindings 0-10 as declared above. `run()` already dispatches it indirectly when the pipeline is valid.

> **Binding 3 is not the same buffer in the two sets.** Stage 1's set binds `dispatch_args_` at binding 3 (it writes the stage-2 dispatch width); stage 2's set binds `draw_args_` there (it writes the raster's vertex count). They are separate uniform sets against separate shaders, so this is legal — but binding the wrong one produces a pass that runs, reports plausible counters, and draws nothing. Name the local variables after the buffers, not after the binding index.

Clear `draw_args_` alongside the counters each frame — the same explicit-zero rule, for the same reason:

```cpp
	PackedByteArray zero_args;
	zero_args.resize(16);
	zero_args.fill(0);
	rd->buffer_update(draw_args_, 0, 16, zero_args);
```

- [ ] **Step 5: Report where the blades actually landed**

The spec requires an assertion that every emitted blade sits inside its source brick on an
up-facing surface. That needs the instance data, not just the counters — so extend the ONE
existing hook rather than adding a second. In `GrassScatterPass`, after `read_back_counters`,
read back at most the first 4096 instances and reduce them on the CPU:

```cpp
void GrassScatterPass::read_back_sample(RenderingDevice *rd) {
	sample_min_normal_y_ = 1.0f;
	sample_max_height_ = 0.0f;
	sample_count_ = std::min(last_blade_count_, 4096);
	if (sample_count_ <= 0) return;
	const PackedByteArray data = rd->buffer_get_data(instances_, 0,
			static_cast<uint32_t>(sample_count_) * 32u);
	if (data.size() < sample_count_ * 32) { sample_count_ = 0; return; }
	const float *f = reinterpret_cast<const float *>(data.ptr());
	for (int i = 0; i < sample_count_; i++) {
		const float *a = f + i * 8;      // xyz position, w height
		const uint32_t packed_n = static_cast<uint32_t>(a[4]);
		// oct_decode_snorm8's inverse for the y component only: the pass stores the ground
		// normal, and all this assertion needs is which way it points.
		const float ny = ve::oct_decode_y_snorm8(packed_n);
		sample_min_normal_y_ = std::min(sample_min_normal_y_, ny);
		sample_max_height_ = std::max(sample_max_height_, a[3]);
	}
}
```

Add `ve::oct_decode_y_snorm8(uint32_t)` to `extension/src/shade/oct.h` beside the existing
oct helpers (that file already mirrors `shade.glslh`'s encode/decode on the CPU — read it and
match its convention), and cover it with a case in `extension/tests/test_shade_oct.cpp`
asserting it agrees with the existing full decode for a handful of normals.

Expose both values on the hook: `d["min_normal_y"]` and `d["max_height"]`, plus
`d["sampled"] = sample_count_`. Then append the assertion to `tests/test_grass.gd`:

```gdscript
# The spec's placement contract: no blade may stand on a surface steeper than the slope
# threshold allows, and no blade may be taller than the settings permit.
func test_every_sampled_blade_stands_on_an_up_facing_surface() -> void:
	var w := make_world()
	w.set_grass_value("slope_cos_min", 0.55)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["sampled"]).is_greater(0)
	assert_float(d["min_normal_y"]).is_greater_equal(0.5)  # 0.55 less oct quantisation
	assert_float(d["max_height"]).is_less_equal(
		w.get_grass_value("blade_height_m") * (1.0 + w.get_grass_value("height_jitter")) * 1.15 + 0.001)
```

- [ ] **Step 6: Run tests to verify they pass**

```bash
./build.sh -j$(sysctl -n hw.ncpu) --test
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: all twelve cases PASS, plus the new `oct` case in the native suite.

- [ ] **Step 7: Commit**

```bash
git add shaders/grass_scatter.comp.glsl extension/src/render/grass_scatter_pass.cpp \
        extension/src/render/grass_scatter_pass.h extension/src/shade/oct.h \
        extension/src/shade/oct.cpp extension/tests/test_shade_oct.cpp tests/test_grass.gd
git commit -m "feat(grass): place blades on grass voxels inside surviving bricks"
```

---

### Task 7: Draw the blades

The first task that puts anything on screen: three vertices per blade, straight and unanimated, writing the G-buffer.

**Files:**
- Create: `extension/src/render/grass_raster_pass.h`
- Create: `extension/src/render/grass_raster_pass.cpp`
- Create: `shaders/grass.vert.glsl`
- Create: `shaders/grass.frag.glsl`
- Modify: `extension/src/render/orchestrator.{h,cpp}`, `extension/src/voxel_world.{h,cpp}`, `extension/src/raymarch_compositor.cpp`, `extension/src/debug/hooks.cpp`, `tests/test_grass.gd`

**Interfaces:**
- Consumes: `instance_buffer()` and `draw_args_buffer()` from Task 6; `GBuffer::albedo/surface/depth`.
- Produces: `godot::GrassRasterPass` with `void initialize(RenderingDevice *)`, `void teardown()`, `bool draw(RenderingDevice *, GrassScatterPass &, GBuffer &, const Projection &view_proj, const float cam_pos[3])`, `void release_targets()`, `int last_vertex_count() const`.
- Extends `debug_grass_stats()` with `drawn` (bool) and `vertices` (int).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_grass.gd`:

```gdscript
func test_the_raster_issues_three_vertices_per_blade() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["drawn"]).is_true()
	assert_int(d["vertices"]).is_equal(d["blades"] * 3)

func test_no_blades_means_no_draw_but_not_a_failure() -> void:
	var w := make_world()
	w.set_grass_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["vertices"]).is_equal(0)
	assert_bool(d["ran"]).is_true()
```

- [ ] **Step 2: Run test to verify it fails**

```bash
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: FAIL — `Invalid get index 'drawn'`.

- [ ] **Step 3: Write the vertex shader**

Create `shaders/grass.vert.glsl`:

```glsl
#[vertex]
#version 460

#include "common.glslh"
#include "shade.glslh"
#include "grass.glslh"

// No vertex buffer and no vertex attributes: geometry is PULLED, exactly as lod.vert.glsl
// does. gl_VertexIndex / 3 is the blade, % 3 the corner. This also routes around Godot
// exposing neither gl_DrawID nor a non-zero firstInstance.
layout(set = 0, binding = 0, std430) readonly buffer Instances { GrassBlade b[]; } instances;
layout(set = 0, binding = 1, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam; // xyz camera position, w unused
} push;

layout(location = 0) out vec3 v_wpos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out float v_height_t; // 0 at the root, 1 at the tip

void main() {
	uint vi = uint(gl_VertexIndex);
	uint blade_index = vi / 3u;
	uint corner = vi % 3u;
	GrassBlade blade = instances.b[blade_index];

	vec3 root = blade.a.xyz;
	float height = blade.a.w;
	vec3 up = oct_decode_snorm8(uint(blade.b.x));
	float lean = blade.b.z;

	// The blade leans in its OWN direction rather than facing the camera; that
	// directionality is what makes a field read as a field. side is perpendicular to both.
	vec3 lean_dir = normalize(vec3(cos(lean), 0.0, sin(lean)) -
			up * dot(vec3(cos(lean), 0.0, sin(lean)), up));
	vec3 side = normalize(cross(up, lean_dir));

	// View-space thickening (Ghost of Tsushima): as the blade turns edge-on, widen it so a
	// sub-pixel sliver does not vanish. view_edge is 0 face-on, 1 edge-on.
	vec3 to_cam = normalize(push.cam.xyz - root);
	float view_edge = 1.0 - abs(dot(side, to_cam));
	float width = pc.blade.x * mix(1.0, 2.5, view_edge);

	float t = (corner == 2u) ? 1.0 : 0.0;
	vec3 p = root + up * (height * t);
	if (corner != 2u) p += side * (corner == 0u ? -width : width) * 0.5;

	// Roundness without geometry: base normals splay outward, the tip's leans toward the
	// blade's own up. Interpolation then shades a flat triangle as a rounded one.
	vec3 n = (corner == 2u) ? up : normalize(mix(up, (corner == 0u ? -side : side), 0.55));

	v_wpos = p;
	v_normal = n;
	v_height_t = t;
	gl_Position = push.view_proj * vec4(p, 1.0);
}
```

- [ ] **Step 4: Write the fragment shader**

Create `shaders/grass.frag.glsl`:

```glsl
#[fragment]
#version 460

#include "common.glslh"
#include "shade.glslh"
#include "grass.glslh"

layout(set = 0, binding = 1, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in float v_height_t;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss

const uint GRASS_MATERIAL = 1u;

// If common.glslh fails to compile here, it is because material_surface() needs the two
// material sampler arrays declared BEFORE the include -- the convention lod.frag.glsl
// follows at its lines 4-6. Declare them ahead of the include at unused binding slots even
// though grass never samples them; a flat blade colour is the whole point.

void main() {
	// Task 8 replaces this flat colour with the root-to-tip gradient. Keeping it flat here
	// means this task's test is about geometry reaching the G-buffer, nothing else.
	vec3 albedo = flat_material_albedo(GRASS_MATERIAL);

	// Backfaces are not culled, so a blade seen from behind must not shade as if it faced
	// away -- that is the classic black-grass bug.
	vec3 n = gl_FrontFacing ? normalize(v_normal) : -normalize(v_normal);

	// Sun visibility is 1: shadowing is the deferred pass's job, exactly as in lod.frag.glsl.
	out_albedo = vec4(albedo, 1.0);
	out_surface = vec4(oct_encode(n), float(GRASS_MATERIAL), pc.style.y);
}
```

- [ ] **Step 5: Write the raster pass**

Create `extension/src/render/grass_raster_pass.h` and `.cpp`. Model the framebuffer and pipeline creation on `LodRasterPass::ensure_pipeline` (`lod_raster_pass.cpp:110-190`), with these differences:

- Attachments are `Array::make(gb.albedo(), gb.surface(), gb.depth())`. There is no marker attachment.
- One pipeline, not three: `POLYGON_CULL_DISABLED` (blades are single triangles seen from both sides).
- Depth test on, depth write on, `COMPARE_OP_GREATER_OR_EQUAL` — reverse-Z, same as the far field.
- Blending disabled on both colour attachments.
- Vertex format `RenderingDevice::INVALID_ID`, primitive `RENDER_PRIMITIVE_TRIANGLES`.

The draw itself is non-indexed and indirect:

```cpp
bool GrassRasterPass::draw(RenderingDevice *rd, GrassScatterPass &scatter, GBuffer &gb,
		const Projection &view_proj, const float cam_pos[3]) {
	last_vertex_count_ = 0;
	if (!rd_ || rd != rd_ || !shader_.is_valid() || !gb.is_valid()) return false;
	const RID instances = scatter.instance_buffer();
	const RID args = scatter.draw_args_buffer();
	if (!instances.is_valid() || !args.is_valid()) return true; // nothing placed: not a failure
	if (!ensure_pipeline(rd, gb)) return false;
	if (!ensure_uniform_set(rd, scatter)) return false;

	const int64_t dl = rd->draw_list_begin(framebuffer_, RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	PackedByteArray pc;
	pc.resize(80);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) f[c * 4 + r] = view_proj.columns[c][r];
	f[16] = cam_pos[0];
	f[17] = cam_pos[1];
	f[18] = cam_pos[2];
	f[19] = 0.0f;
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	// One non-indexed indirect draw; the vertex count is whatever the scatter wrote.
	rd->draw_list_draw_indirect(dl, false, args, 0, 1, 16);
	rd->draw_list_end();
	last_vertex_count_ = scatter.last_blade_count() * 3;
	return true;
}
```

`release_targets()` frees `framebuffer_` and clears the cached attachment RIDs, matching `LodRasterPass::release_targets`.

- [ ] **Step 6: Construct, wire and report it**

- Orchestrator: construct `grass_raster_pass_` immediately after `grass_scatter_pass_` and delete it in `teardown_render_passes()` immediately before it.
- `VoxelWorld::grass_raster_pass()` accessor, delegating to the orchestrator.
- Compositor: inside the existing grass block, chain the draw:

```cpp
		GrassRasterPass *grass_raster = world->grass_raster_pass();
		const bool grass_ok = grass->run(rd, *atlas, gl,
				static_cast<float>(world->beauty_frame()) / 60.0f) &&
				grass_raster && grass_raster->draw(rd, *grass, *gb, view_proj, cam_pos);
		if (grass_ok) timings->end(rd, "grass");
		else timings->cancel("grass");
```

- `debug_grass_stats()`: add `d["drawn"] = raster != nullptr;` and `d["vertices"] = raster ? raster->last_vertex_count() : 0;`.

- [ ] **Step 7: Run tests to verify they pass**

```bash
./build.sh -j$(sysctl -n hw.ncpu)
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: all fourteen cases PASS.

- [ ] **Step 8: Look at it**

```bash
godot --path . demo/main.tscn
```

Expected: flat green blades standing on the grass bands, no animation, correctly lit and shadowed by the existing stack. They should be occluded by terrain and should cast no shadow. If blades are invisible, check reverse-Z first — a `LESS` compare draws nothing here.

- [ ] **Step 9: Commit**

```bash
git add extension/src/render/grass_raster_pass.h extension/src/render/grass_raster_pass.cpp \
        shaders/grass.vert.glsl shaders/grass.frag.glsl extension/src/render/orchestrator.h \
        extension/src/render/orchestrator.cpp extension/src/voxel_world.h \
        extension/src/voxel_world.cpp extension/src/raymarch_compositor.cpp \
        extension/src/debug/hooks.cpp tests/test_grass.gd
git commit -m "feat(grass): draw blades into the G-buffer"
```

---

### Task 8: Wind, colour and the distance fade

Turns standing green triangles into the reference image.

**Files:**
- Modify: `shaders/grass.vert.glsl` (wind, bend)
- Modify: `shaders/grass.frag.glsl` (gradient, clump colour, flowers, bayer fade)
- Modify: `shaders/grass.glslh` (the gust noise)
- Create: `tests/test_grass_golden.gd`
- Create: `tests/golden/grass_reference.png` (generated in Step 5)

**Interfaces:**
- Consumes: `pc.wind` (strength, speed, scale, time) and `pc.style` (flower_chance, gloss) from `GrassParams`; `v_height_t`, and two new varyings `v_clump` and `v_hash`.
- Produces: no new C++ interface.

- [ ] **Step 1: Add the gust field to the shared header**

Append to `shaders/grass.glslh`:

```glsl
// Value noise on a world-XZ lattice, scrolled along the wind direction. This is the gust
// field -- the light and dark waves crossing a meadow -- and it is deliberately procedural
// rather than a texture so the pass needs no asset and no extra binding.
float grass_gust(vec2 world_xz, float time, float speed, float scale) {
	vec2 p = world_xz * scale - vec2(0.7, 0.7) * (time * speed);
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = grass_unit(grass_hash3(ivec3(i, 0.0), 0x1234u));
	float b = grass_unit(grass_hash3(ivec3(i + vec2(1, 0), 0.0), 0x1234u));
	float c = grass_unit(grass_hash3(ivec3(i + vec2(0, 1), 0.0), 0x1234u));
	float d = grass_unit(grass_hash3(ivec3(i + vec2(1, 1), 0.0), 0x1234u));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
```

- [ ] **Step 2: Displace the tip in the vertex shader**

In `shaders/grass.vert.glsl`, replace the `vec3 p = root + up * (height * t);` line and what follows with:

```glsl
	float t = (corner == 2u) ? 1.0 : 0.0;
	uint hash = floatBitsToUint(blade.b.y);

	// Only the tip moves (BotW). Three layers: the gust field gives the waves crossing the
	// meadow, a per-blade sine keeps neighbours out of phase, and a high-frequency term
	// jitters the very tip.
	float gust = grass_gust(root.xz, pc.wind.w, pc.wind.y, pc.wind.z);
	float phase = grass_unit(hash) * 6.2831853;
	float bob = sin(pc.wind.w * 2.3 + phase) * 0.25;
	float jitter = sin(pc.wind.w * 11.0 + phase * 3.0) * 0.06;
	float bend = pc.wind.x * (gust + bob + jitter);

	// pow(t, 2) keeps the base planted while the tip travels.
	vec3 sway = lean_dir * (bend * t * t);
	vec3 p = root + up * (height * t) + sway;
	if (corner != 2u) p += side * (corner == 0u ? -width : width) * 0.5;
```

Add the two new varyings and write them:

```glsl
layout(location = 3) out float v_clump;
layout(location = 4) out flat uint v_hash;
```

```glsl
	v_clump = blade.b.w;
	v_hash = hash;
```

- [ ] **Step 3: Colour the blade in the fragment shader**

In `shaders/grass.frag.glsl`, declare the matching inputs and replace the flat albedo:

```glsl
layout(location = 3) in float v_clump;
layout(location = 4) in flat uint v_hash;
```

```glsl
	// Vertex gradient, dark cool root to bright warm tip. This is the BotW vertex-colour
	// trick and it does most of the work of making a triangle read as a blade.
	const vec3 kRoot = vec3(0.10, 0.22, 0.07);
	const vec3 kTip  = vec3(0.52, 0.78, 0.24);
	vec3 albedo = mix(kRoot, kTip, v_height_t * v_height_t);

	// Per-blade and per-clump variation, so the field is patchy rather than a lawn.
	float tint = grass_unit(grass_hash(v_hash ^ 0x27D4EB2Fu));
	albedo *= mix(0.82, 1.18, tint);
	albedo = mix(albedo, albedo * vec3(1.12, 1.05, 0.72), v_clump * 0.45);

	// Flowers: a small hash fraction gets a warm tip. One branch, near-free.
	if (grass_unit(grass_hash(v_hash ^ 0x165667B1u)) < pc.style.x) {
		albedo = mix(albedo, vec3(0.95, 0.86, 0.28), smoothstep(0.72, 1.0, v_height_t));
	}

	// Dither out at the reach limit with the SAME test lod.frag.glsl uses at the LoD seam,
	// so the density tail fades instead of popping.
	float d = distance(v_wpos, push.cam.xyz);
	float fade = clamp((d - pc.ring_end.z) / max(pc.ring_end.w - pc.ring_end.z, 1e-3), 0.0, 1.0);
	if (bayer4(ivec2(gl_FragCoord.xy)) < fade) discard;
```

The fragment shader needs the camera position for that distance, so declare the **same** push block the vertex shader declares, verbatim and in the same order — one push range is shared by both stages, and a mismatched declaration is a silent read of the wrong bytes:

```glsl
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam; // xyz camera position, w unused
} push;
```

- [ ] **Step 4: Run the existing suite to confirm nothing regressed**

```bash
./build.sh -j$(sysctl -n hw.ncpu)
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: all fourteen cases still PASS. The counters are unchanged by shading work; if `blades` moved, something in the vertex shader is writing a buffer it should not.

- [ ] **Step 5: Capture and pin a golden frame**

Create `tests/test_grass_golden.gd` following the structure of `tests/test_ssao_golden.gd` — read that file first and mirror its capture, comparison and tolerance handling exactly rather than inventing a second golden mechanism. The test must:

- build a world at a fixed camera position over grass terrain,
- pin `wind` time to a constant so the frame is deterministic,
- compare against `tests/golden/grass_reference.png` with the same per-pixel tolerance the SSAO golden uses.

Generate the reference by running the suite once with the golden missing and copying the produced frame, exactly as `test_ssao_golden.gd` documents.

```bash
./gdunit_tests.sh -a res://tests/test_grass_golden.gd
```

Expected: PASS against the freshly captured golden.

- [ ] **Step 6: Look at it**

```bash
godot --path . demo/main.tscn
```

Expected: gust waves crossing the field, blades leaning in varied directions, a visible root-to-tip gradient, occasional yellow flower tips, and a dithered fade at the reach limit rather than a hard edge. Compare against `~/Downloads/grass.jpg`.

- [ ] **Step 7: Commit**

```bash
git add shaders/grass.glslh shaders/grass.vert.glsl shaders/grass.frag.glsl \
        tests/test_grass_golden.gd tests/golden/grass_reference.png
git commit -m "feat(grass): wind, root-to-tip gradient, clumping and distance fade"
```

---

### Task 9: Demo knobs

**Files:**
- Modify: `demo/settings_menu.gd`
- Modify: `demo/debug_menu.gd`
- Modify: `demo/help.gd`
- Modify: `tests/test_debug_menu.gd` (if it enumerates rows)

**Interfaces:**
- Consumes: `VoxelWorld::set_grass_value(String, float)` and `get_grass_value(String)` from Task 4.
- Produces: no new code interface.

- [ ] **Step 1: Read the existing row pattern**

```bash
sed -n '1,120p' demo/settings_menu.gd
```

Identify how one existing effect row is built (label, control, the setter it calls) and how rows are registered. Mirror it exactly — do not introduce a second pattern.

- [ ] **Step 2: Add the grass section**

Add a "Grass" section with these rows, each calling `world.set_grass_value(<name>, <value>)` and initialising from `world.get_grass_value(<name>)`:

| Label | Name | Control | Range |
|---|---|---|---|
| Grass | `enabled` | checkbox | 0 / 1 |
| Density | `blades_per_brick` | slider | 0 – 64, step 1 |
| Reach | `reach_m` | slider | 0 – 120 m, step 1 |
| Blade width | `blade_width_m` | slider | 0 – 0.06, step 0.002 |
| Blade height | `blade_height_m` | slider | 0 – 1.5, step 0.05 |
| Wind strength | `wind_strength` | slider | 0 – 1.5, step 0.05 |
| Wind speed | `wind_speed` | slider | 0 – 3, step 0.1 |

- [ ] **Step 3: Report grass in the debug menu**

Add one line to `demo/debug_menu.gd`'s per-frame readout, next to the existing pass rows:

```gdscript
	var g: Dictionary = world.hooks().debug_grass_stats()
	lines.append("grass  blades %d / %d  bricks %d  peak %d" % [
		g["blades"], g["capacity"], g["bricks"], g["high_water"]])
```

Match the surrounding formatting rather than this spacing if it differs.

- [ ] **Step 4: Run the menu tests**

```bash
./gdunit_tests.sh -a res://tests/test_debug_menu.gd,res://tests/test_grass.gd
```

Expected: PASS. If `test_debug_menu.gd` asserts an exact row count or an exact readout string, update that assertion to include the grass line — the test is pinning the menu's contents on purpose.

- [ ] **Step 5: Commit**

```bash
git add demo/settings_menu.gd demo/debug_menu.gd demo/help.gd tests/test_debug_menu.gd
git commit -m "feat(grass): demo settings and debug readout"
```

---

### Task 10: Overflow reporting, the edit contract, and the cost measurement

The last task closes the two contracts the design promised — edits destroy grass, overflow is visible — and produces the honest cost number.

**Files:**
- Modify: `extension/src/render/grass_scatter_pass.cpp` (log the first overflow once)
- Modify: `tests/test_grass.gd` (the edit test)
- Modify: `docs/PORTFOLIO.md` (a grass row and the measured cost)
- Modify: `docs/superpowers/specs/2026-09-11-stylized-grass-design.md` (record what shipped)

**Interfaces:**
- Consumes: everything above.
- Produces: no new code interface.

- [ ] **Step 1: Write the failing edit test**

Append to `tests/test_grass.gd`:

```gdscript
# The edit-awareness contract from the design doc: the scatter reads the LIVE atlas, so an
# edit that removes grass voxels must remove their blades on the next frame, with no
# invalidation code anywhere. Tested rather than assumed.
func test_digging_grass_away_removes_its_blades() -> void:
	var w := make_world()
	var before: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(before).is_greater(0)
	# Carve a large sphere out of the ground directly under the camera.
	w.hooks().debug_apply_edit(Vector3(30.0, 56.2, 30.0), 8.0, 0)
	for i in range(30):
		w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0))
	var after: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(after).is_less(before)
```

Check the actual edit hook's name and signature in `extension/src/debug/hooks.cpp` (grep for `debug_apply_edit` or the nearest equivalent used by `tests/test_collider_edits.gd`) and use it verbatim. Do **not** add a new hook for this.

- [ ] **Step 2: Run the test**

```bash
./gdunit_tests.sh -a res://tests/test_grass.gd
```

Expected: PASS on the first run, with no production change. That is the point — the contract should already hold. If it fails, the scatter is reading something cached; fix that rather than the test.

- [ ] **Step 3: Report the first overflow once**

In `GrassScatterPass::read_back_counters`, after updating `blade_high_water_`:

```cpp
	if (static_cast<int>(c[1]) > capacity_ && !overflow_logged_) {
		overflow_logged_ = true;
		UtilityFunctions::printerr("GrassScatterPass: blade buffer overflow, wanted ",
				static_cast<int>(c[1]), " of ", capacity_,
				"; blades were dropped. Lower blades_per_brick or raise max_blades.");
	}
```

Add `bool overflow_logged_ = false;` to the header and reset it in `teardown()`. Logging once and not per frame matches how `lod_overflow_logged` treats page overflow — a repeated error line is noise that hides the next real one.

- [ ] **Step 4: Measure the cost honestly**

GPU timestamps read back invalid on this machine, so the `grass` label cannot support a claim. Measure with interleaved A/B/A wall-frame runs instead:

```bash
tools/run_benchmarks.sh grass-off-a    # after: w.set_grass_value("enabled", 0)
tools/run_benchmarks.sh grass-on
tools/run_benchmarks.sh grass-off-b
```

Read `tools/run_benchmarks.sh` first for how a leg overrides settings; if it has no grass switch, add `--grass=0|1` following the existing `--render-scale=` / `--near-scale=` pattern and commit that with this task. Report the delta as `(on) - mean(off-a, off-b)` on the `steady` and `ridge` legs. A single on/off pair is not a measurement here — the A/B/A bracket is what separates the effect from drift.

- [ ] **Step 5: Record what shipped**

Add a row to the pass table in `docs/PORTFOLIO.md` with the measured p50/p99 delta, and a short paragraph under "What it does" describing the grass system in the register the rest of that document uses.

Append a "What shipped" section to `docs/superpowers/specs/2026-09-11-stylized-grass-design.md` recording: the measured cost, anything that deviated from the design and why, and whether far-distance coverage still looks necessary now that the blades and the thinning tail exist.

- [ ] **Step 6: Run everything**

```bash
./build.sh -j$(sysctl -n hw.ncpu) --test
./gdunit_tests.sh
```

Expected: the grass suites pass. Per the standing note on this repo, some unrelated suites fail on clean `main` and the set drifts — before attributing any failure to this branch, stash and re-run on `main` to get the current baseline, then compare.

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/grass_scatter_pass.cpp extension/src/render/grass_scatter_pass.h \
        tests/test_grass.gd tools/run_benchmarks.sh docs/PORTFOLIO.md \
        docs/superpowers/specs/2026-09-11-stylized-grass-design.md
git commit -m "feat(grass): overflow reporting, edit contract test, measured cost"
```
