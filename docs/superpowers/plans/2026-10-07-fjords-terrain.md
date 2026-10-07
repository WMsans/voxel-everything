# Fjords Terrain and the Sector Heightmap Tier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Fjords world type — eroded, snow-capped mountains around long dry fjord valleys — built on a new cached `sector2d` heightmap tier so the erosion filter is paid once per texel instead of per voxel and per pixel.

**Architecture:** A pipeline may start with one `//!kind map` stage. The render device bakes it per 409.6 m sector into a ring of storage buffers and reads the bytes back asynchronously into a host `SectorCache`. Every device that evaluates the field (the render device and the mesher's worker device) keeps a `SectorMirror` — a `texture2d_array` plus a toroidal window — filled from those host bytes, and field stages sample it with a cubic B-spline. The CPU mirror of each field stage samples the same host bytes. Region streaming, LoD builds and collider planning wait until the sectors they cover are resident.

**Tech Stack:** C++20 GDExtension (godot-cpp, Godot 4.7), GLSL 460, doctest (native), gdUnit4 (GPU), GDScript, ImageMagick `convert`.

**Spec:** `docs/superpowers/specs/2026-10-07-fjords-terrain-design.md`. Read it, and `docs/superpowers/specs/2026-09-03-terrain-pipeline-design.md` §4–§7 (the manifest syntax and the sector tier this builds a cut of), before starting.

## Global Constraints

- No `Co-Authored-By` or other AI attribution lines in any commit message. Plain conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`).
- Work only in `.worktrees/fjords` on branch `feat/fjords` (created in Task 0 from `main`). Never commit to `main`.
- Default, Mesas, Flat and the golden pipeline generate **byte-identical** `field.glslh`. `shaders/generated/field.glslh.golden` must not change in any task. No file under `tests/golden/` may change. If one does, stop and report; do not re-record (spec §9 golden policy).
- The only generated files that may change: `shaders/generated/blocks.glslh` (Task 4, one new push block) and `shaders/material_table.glslh` (Task 7, the snow row).
- Sector geometry is fixed: 256² texels at 1.6 m (409.6 m), a 2-texel apron (260² stored), RG16 unorm, R = height over `[water_y − 64, water_y + 448]`, G = ridge map mapped −1…1 → 0…1, a 24 × 24 window (spec §4.2, §4.4).
- `S_max = 2.5`; the fjord field stage declares `//!lipschitz add 3.67`; `fjords.pipeline` declares `lipschitz 3.7` (spec §6.4).
- `shaders/erosion.glslh` carries Rune Skovbo Johansen's MPL-2.0 header verbatim. No other file contains ported Shadertoy code.
- Tabs for indentation in C++, GLSL and GDScript.
- Build: `./build.sh`. Native tests: `cd extension && scons -Q test` (single case: `cd extension && ./build/tests/ve_tests -tc="<name>"`). GPU tests: `./build.sh && ./gdunit_tests.sh -a res://tests/<file>.gd`.
- Generated goldens are never hand-edited: `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests` regenerates `shaders/generated/*`. `shaders/material_table.glslh` is regenerated the way `extension/tests/test_material_glslh.cpp`'s header says (it prints the correct text on failure).
- gdUnit has a standing, drifting set of failures on clean `main`. Task 0 records it; every "no new failures" check compares against that list, not against zero. A suite failing that is not on the list: `git stash -u`, re-run that suite on the clean tree, then decide. A failing gdUnit case aborts the rest of its suite, so a suite's case count dropping is itself a failure.
- Every new `.gd` file gets its `.gd.uid` sidecar committed; create them with `./build.sh --verify` (headless editor scan). Never invent a uid.
- GPU timings are invalid on this machine (`debug_gpu_timings()` returns −1). Frame cost is measured as wall-clock percentiles with interleaved A/B/A runs only.

## Deviations From The Spec (decided while planning; Task 9 records them in the spec's §12)

1. **Bytes reach every device through the host cache.** The render device bakes into a ring of eight storage buffers and reads each back with `AsyncBufferRead`; a `SectorMirror` per device (render and mesher worker) uploads the host bytes into its own `texture2d_array`. Spec §4.6 had the bake write the array directly and missed that LoD builds, colliders, island extraction and consolidation run on the mesher's separate `RenderingDevice`, which cannot see render-device textures. GPU memory is therefore ~126 MB per device (two devices) plus ~126 MB host at a 4 km radius, about 3× the spec's "≈100 MB".
2. **One map stage and one sector resource per pipeline.** `resolve_pipeline` rejects a second. The spec's "map stages in pipeline order, barrier between" has no second stage to order.
3. **Map stage files end `.map.glslh`, not `.map.glsl`.** Godot imports every `*.glsl` under `res://` as a shader file and the reload preflight compiles them standalone; a stage body is neither.
4. **The slope statistic is computed on the CPU from the read-back texels**, not with an `atomicMax` in the bake. The host already holds every byte.
5. **Map-stage params may be overridden in a pipeline file.** The resolver's "no overrides on an sdf writer" rule does not apply (a map stage writes no `sdf`), though its params move the field's slope. The slope statistic and Task 8's slope test are the guard.
6. **Sector coordinates are field-space** (after the seed offset). `SectorCache` takes world-space cameras and rectangles and adds the offset itself.
7. **`valley_width` is in noise units** (default 0.15, ≈300 m at the default `valley_freq`), not metres.
8. **No readback timeout.** A readback that never returns pins one of the eight ring entries for the context's lifetime. Godot's `buffer_get_data_async` has no failure callback to time out on.
9. **Collider planning is held as a whole** while any sector under the physics balls is not resident, rather than per chunk. `ChunkResidency` permanently caches "empty" for a chunk it probes, and a probe of an unbaked sector would read the fallback air and never retry.
10. **Gating ignores sectors beyond the cache radius.** A far LoD chunk can overlap the 4 km radius; the parts outside it read the fallback height. Waiting for sectors that are never wanted would leave the chunk unbuilt forever.
11. **Snow and rock order clarified.** Above the snow line a face is snow when `slope < snow_slope` (1.4 above `e > 240`) and `breakstone` otherwise; below it, `breakstone` when `slope > 1.2`, else `grass_01`. The spec's table put the steep test first, which made the relaxed peak limit unreachable.
12. **`fjord_ground(xz)` returns `vec4(height, dh/dx, dh/dz, ridge)`**; the slope is `length(.yz)`.
13. **A physics-only world (graphics never initialised) never bakes sectors**, so its colliders stay held. No shipped scene or test runs Fjords that way.
14. **The cache radius is fixed at pipeline load** from `stream_radius_m`; changing `stream_radius_m` on a live world does not move it.

## Review Focus

1. **Negative and far sector coordinates** — a camera at x < 0, or a seed offset near ±8 km. Expected: `sector_of` floors, the window's modulo wraps negatives, and a sector 24 cells away never aliases a resident one. Tests: Task 1 (`test_sector.cpp`, negative coordinates and an aliasing pair), Task 4 (`test_sectors.gd` bakes at a far-offset seed).
2. **A teleport or fast flight** into terrain whose sectors are not baked. Expected: regions there stay unstreamed until their sectors are resident; no brick is ever generated from fallback height. Test: Task 6 (`test_sector_gating.gd`, teleport case).
3. **A sample exactly on a sector boundary.** Expected: the height from the sector on either side agrees, because both read the same knots (the apron). Test: Task 1 (continuity across two synthetic sectors).
4. **A second world built in the same process** (the title flow builds one world after another) at a different seed. Expected: the second world never reads the first world's texels; a third world at the first seed reproduces the first world's bytes. Test: Task 4 (`test_sectors.gd`).
5. **Terrain that leaves the encoded range** — a peak above `water_y + 448` or a floor below `water_y − 64` clamps to a flat top or floor. Expected: the shipped parameters keep every texel strictly inside. Test: Task 8 (`test_fjords.gd` asserts the cache's `r_min > 0.01` and `r_max < 0.98` over ±3 km at three seeds).

---

## File Structure

**Create:**
- `extension/src/terrain/sector.h`, `sector.cpp` — sector constants, coordinates, B-spline, slope, window (pure C++).
- `extension/src/terrain/sector_cache.h`, `sector_cache.cpp` — the host cache and `sector_ground` (pure C++).
- `extension/tests/test_sector.cpp`, `test_sector_cache.cpp`, `test_pipeline_map_stage.cpp`.
- `extension/src/render/sector_mirror.h`, `sector_mirror.cpp` — one device's texture array and window.
- `extension/src/render/sector_context.h`, `sector_context.cpp` — the render-device bake ring and its mirror.
- `shaders/sector.glslh` — GLSL constants, window lookup, B-spline, `sector_ground`.
- `shaders/sector_bake.comp.glsl` — the bake dispatch.
- `shaders/noise2d.glslh` — `ve_clamp01`, `ve_pcg2d`, `ve_hash2`, `ve_noised`.
- `shaders/erosion.glslh` — the MPL-2.0 port.
- `shaders/stages/sector_fixture.map.glslh`, `shaders/stages/sector_fixture.field.glslh`, `tests/fixtures/sector_fixture.pipeline` — the tier's own fixture.
- `shaders/stages/fjord_height.map.glslh`, `fjord.field.glslh`, `fjord_bands.field.glslh`.
- `assets/pipelines/fjords.pipeline`, `demo/world_types/30_fjords.tres`.
- `assets/materials/09_{basecolor,normal,roughness,ambientOcclusion,height}.png`.
- `tests/test_sectors.gd`, `tests/test_sector_gating.gd`, `tests/test_fjords.gd`.
- `tools/fjord_capture.gd`.

**Modify:**
- `extension/src/terrain/stage_manifest.{h,cpp}`, `pipeline.{h,cpp}`, `field_codegen.cpp`, `stage_library.h`, `pipeline_field_generator.{h,cpp}`, `builtin_stages.cpp`.
- `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh`.
- `extension/src/render/field_context_set.{h,cpp}`, `orchestrator.{h,cpp}`, `frame.cpp`, `world_streamer.{h,cpp}`, `mesh_service.{h,cpp}`.
- `extension/src/world/residency.{h,cpp}`, `extension/src/lod/lod_system.{h,cpp}`, `extension/src/physics/collider_streamer.{h,cpp}`.
- `extension/src/core/world_store.h`, `extension/src/voxel_world.cpp`.
- `extension/src/debug/hooks.h`, `hooks.cpp`, `hooks_world.cpp`, `hooks_lod.cpp`, `hooks_physics.cpp`.
- `extension/src/world/material_table.h`, `shaders/material_table.glslh`, `tools/convert_materials.sh`.
- `extension/tests/test_residency.cpp`.
- `tests/test_field_diff.gd`, `tests/test_create_world.gd`.
- `demo/scripts/benchmark.gd` (one flag).
- The spec (Task 9).

---

### Task 0: Worktree and baseline

**Files:** none changed.

- [ ] **Step 1: Create the worktree**

Use superpowers:using-git-worktrees to create `.worktrees/fjords` on a new branch `feat/fjords` from `main` (which carries the spec commit `9b7ed80` and this plan). All later commands run from `.worktrees/fjords`.

Run: `git -C .worktrees/fjords status -sb | head -1`
Expected: `## feat/fjords`

- [ ] **Step 2: Build and run the native tests**

Run: `./build.sh --test`
Expected: build OK; doctest summary ends `Status: SUCCESS!`.

- [ ] **Step 3: Record the gdUnit baseline**

Run: `./gdunit_tests.sh 2>&1 | tee "$TMPDIR/fjords-baseline.txt" | tail -40`
Expected: a summary listing the failing suites (~20 minutes). Write the failing suite and case names into the task notes. Every later "no new failures" check compares against this list.

- [ ] **Step 4: Record the pin suites**

Run:
```bash
for s in test_field_diff test_mesh_lattice test_leaves test_create_world test_world_scene test_material_picker test_frame_shipped_golden test_demo_scene_wiring; do
	echo "== $s"; ./gdunit_tests.sh -a res://tests/$s.gd 2>&1 | grep -E "Statistics|FAILED|PASSED" | tail -3
done
```
Expected: each suite's pass/fail state and case count, written into the task notes.

---

### Task 1: Sector maths

**Files:**
- Create: `extension/src/terrain/sector.h`, `extension/src/terrain/sector.cpp`
- Test: `extension/tests/test_sector.cpp`

**Interfaces:**
- Produces: `ve::kSectorTexels` (256), `kSectorApron` (2), `kSectorStride` (260), `kSectorTexelM` (1.6f), `kSectorSizeM` (409.6f), `kSectorWindow` (24), `kSectorHeightBelowM` (64.0f), `kSectorHeightSpanM` (512.0f), `kSectorSlopeLimit` (2.5f); `struct SectorCoord { int x, z; }` with `==`, `!=`, `<`; `SectorCoord sector_of(float x, float z)`; `void sector_texel_pos(SectorCoord, int tx, int tz, float *x, float *z)`; `float sector_distance(SectorCoord, float x, float z)`; `struct SectorTexels { std::vector<uint32_t> texels; float max_slope; }`; `struct SectorSample { float r, drdx, drdz, g; }`; `SectorSample sector_bspline(const SectorTexels &, SectorCoord, float x, float z)`; `float sector_max_axis_slope(const SectorTexels &)`; `int sector_window_cell(SectorCoord)`; `struct SectorLayer { SectorCoord c; int layer; }`; `std::vector<int32_t> sector_window(SectorCoord centre, const std::vector<SectorLayer> &)`; `int sector_window_lookup(const std::vector<int32_t> &, SectorCoord)`.

- [ ] **Step 1: Write the failing tests**

`extension/tests/test_sector.cpp`:
```cpp
#include <doctest/doctest.h>
#include "terrain/sector.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace {
uint32_t pack(float r, float g) {
	const auto q = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); };
	return q(r) | (q(g) << 16);
}
// Fills a sector from f(x, z) evaluated at every texel centre, apron included -- what the
// bake does.
template <class F>
ve::SectorTexels fill(ve::SectorCoord c, F f) {
	ve::SectorTexels t;
	t.texels.resize(size_t(ve::kSectorStride * ve::kSectorStride));
	for (int z = 0; z < ve::kSectorStride; z++)
		for (int x = 0; x < ve::kSectorStride; x++) {
			float wx, wz;
			ve::sector_texel_pos(c, x, z, &wx, &wz);
			t.texels[size_t(z * ve::kSectorStride + x)] = pack(f(wx, wz), 0.5f);
		}
	return t;
}
} // namespace

TEST_CASE("sector_of floors negative coordinates") {
	CHECK(ve::sector_of(-0.1f, 409.6f) == ve::SectorCoord{-1, 1});
	CHECK(ve::sector_of(0.0f, -409.7f) == ve::SectorCoord{0, -2});
	CHECK(ve::sector_of(409.5f, 0.0f) == ve::SectorCoord{0, 0});
}

TEST_CASE("the first interior texel sits half a texel inside the sector corner") {
	float x, z;
	ve::sector_texel_pos({1, -1}, ve::kSectorApron, ve::kSectorApron, &x, &z);
	CHECK(x == doctest::Approx(409.6f + 0.8f));
	CHECK(z == doctest::Approx(-409.6f + 0.8f));
	ve::sector_texel_pos({0, 0}, 0, 0, &x, &z);
	CHECK(x == doctest::Approx(-2.4f)); // the apron reaches past the corner
}

TEST_CASE("sector_distance is zero inside and the gap outside") {
	CHECK(ve::sector_distance({0, 0}, 10.0f, 10.0f) == doctest::Approx(0.0f));
	CHECK(ve::sector_distance({0, 0}, -3.0f, 409.6f + 4.0f) == doctest::Approx(5.0f));
}

TEST_CASE("the B-spline reproduces a linear ramp and its slope") {
	const ve::SectorCoord c{-2, 3};
	const auto ramp = [](float x, float z) { return 0.4f + 0.0002f * (x + 900.0f) - 0.0001f * (z - 1300.0f); };
	const ve::SectorTexels t = fill(c, ramp);
	std::mt19937 rng(7);
	std::uniform_real_distribution<float> u(0.0f, ve::kSectorSizeM);
	for (int i = 0; i < 500; i++) {
		const float x = c.x * ve::kSectorSizeM + u(rng), z = c.z * ve::kSectorSizeM + u(rng);
		const ve::SectorSample s = ve::sector_bspline(t, c, x, z);
		CHECK(s.r == doctest::Approx(ramp(x, z)).epsilon(1e-4));
		CHECK(s.drdx == doctest::Approx(0.0002f).epsilon(2e-2));
		CHECK(s.drdz == doctest::Approx(-0.0001f).epsilon(2e-2));
		CHECK(s.g == doctest::Approx(0.5f).epsilon(1e-4));
	}
}

// The spec's §4.5 bound: the B-spline's gradient is a convex combination of per-axis texel
// differences, so no sample can be steeper than the steepest texel step. This is what makes
// the field stage's declared bound a property of the texels the bake can check exactly.
TEST_CASE("no B-spline gradient exceeds the steepest per-axis texel step") {
	std::mt19937 rng(11);
	std::uniform_int_distribution<uint32_t> word(0, 65535);
	std::uniform_real_distribution<float> u(0.0f, ve::kSectorSizeM);
	for (int grid = 0; grid < 10; grid++) {
		ve::SectorTexels t;
		t.texels.resize(size_t(ve::kSectorStride * ve::kSectorStride));
		for (uint32_t &v : t.texels) v = word(rng) | (word(rng) << 16);
		const float limit = ve::sector_max_axis_slope(t) / ve::kSectorHeightSpanM; // r units per metre
		for (int i = 0; i < 2000; i++) {
			const ve::SectorSample s = ve::sector_bspline(t, {0, 0}, u(rng), u(rng));
			CHECK(std::abs(s.drdx) <= limit * 1.0001f);
			CHECK(std::abs(s.drdz) <= limit * 1.0001f);
		}
	}
}

TEST_CASE("two neighbouring sectors agree at their shared edge") {
	const auto f = [](float x, float z) { return 0.5f + 0.2f * std::sin(x * 0.03f) * std::cos(z * 0.02f); };
	const ve::SectorCoord a{0, 0}, b{1, 0};
	const ve::SectorTexels ta = fill(a, f), tb = fill(b, f);
	for (float z = 3.0f; z < ve::kSectorSizeM; z += 37.0f) {
		const float edge = ve::kSectorSizeM;
		const ve::SectorSample sa = ve::sector_bspline(ta, a, edge - 1e-3f, z);
		const ve::SectorSample sb = ve::sector_bspline(tb, b, edge + 1e-3f, z);
		CHECK(sa.r == doctest::Approx(sb.r).epsilon(1e-4));
		CHECK(sa.drdx == doctest::Approx(sb.drdx).epsilon(1e-2));
	}
}

TEST_CASE("max_axis_slope reports a known ramp in metres per metre") {
	ve::SectorTexels t;
	t.texels.resize(size_t(ve::kSectorStride * ve::kSectorStride));
	for (int z = 0; z < ve::kSectorStride; z++)
		for (int x = 0; x < ve::kSectorStride; x++)
			t.texels[size_t(z * ve::kSectorStride + x)] = uint32_t(100 * x + 30 * z);
	CHECK(ve::sector_max_axis_slope(t) ==
			doctest::Approx(100.0f / 65535.0f * ve::kSectorHeightSpanM / ve::kSectorTexelM));
}

TEST_CASE("the window wraps negatives and never aliases a sector out of reach") {
	CHECK(ve::sector_window_cell({-1, -1}) == ve::sector_window_cell({23, 23}));
	const std::vector<int32_t> w = ve::sector_window({0, 0}, {{{-11, 5}, 3}, {{13, 5}, 7}});
	CHECK(w.size() == size_t(ve::kSectorWindow * ve::kSectorWindow * 3));
	CHECK(ve::sector_window_lookup(w, {-11, 5}) == 3);
	CHECK(ve::sector_window_lookup(w, {13, 5}) == -1); // 13 > 11 from the centre: not written
	CHECK(ve::sector_window_lookup(w, {0, 0}) == -1);  // nothing resident there
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile error, `terrain/sector.h` not found.

- [ ] **Step 3: Write `sector.h`**

```cpp
#pragma once
// The sector heightmap tier's geometry (docs/superpowers/specs/2026-10-07-fjords-terrain-design.md
// §4). Mirrored as GLSL constants and functions in shaders/sector.glslh; a sector baked on the
// GPU and sampled here must land on the same texels.
//
// Sector coordinates are FIELD space -- after the world seed's domain shift -- because that is
// the space every stage samples in.
#include <cstdint>
#include <vector>

namespace ve {

inline constexpr int kSectorTexels = 256;
inline constexpr int kSectorApron = 2;
inline constexpr int kSectorStride = kSectorTexels + 2 * kSectorApron; // 260
inline constexpr float kSectorTexelM = 1.6f;
inline constexpr float kSectorSizeM = kSectorTexels * kSectorTexelM;   // 409.6
inline constexpr int kSectorWindow = 24;
// Channel R encodes height over [water_y - below, water_y - below + span].
inline constexpr float kSectorHeightBelowM = 64.0f;
inline constexpr float kSectorHeightSpanM = 512.0f;
// The steepest per-axis texel slope the fjord field stage's declared bound assumes:
// sqrt(1 + 2 * 2.5^2) = 3.67 (spec §6.4).
inline constexpr float kSectorSlopeLimit = 2.5f;

struct SectorCoord {
	int x = 0, z = 0;
	bool operator==(const SectorCoord &o) const { return x == o.x && z == o.z; }
	bool operator!=(const SectorCoord &o) const { return !(*this == o); }
	bool operator<(const SectorCoord &o) const { return z != o.z ? z < o.z : x < o.x; }
};

SectorCoord sector_of(float x, float z);
// Field-space centre of texel (tx, tz), each in [0, kSectorStride); the apron sits outside.
void sector_texel_pos(SectorCoord c, int tx, int tz, float *x, float *z);
// Distance from (x, z) to the sector's square; 0 inside.
float sector_distance(SectorCoord c, float x, float z);

// One baked sector: kSectorStride^2 texels, each packUnorm2x16(r, g) -- r in the low half.
struct SectorTexels {
	std::vector<uint32_t> texels;
	float max_slope = 0.0f; // sector_max_axis_slope(*this), filled on arrival
};

struct SectorSample {
	float r = 0.0f, drdx = 0.0f, drdz = 0.0f, g = 0.0f; // gradient per metre, in r units
};
// Uniform cubic B-spline over the 4x4 texels around field-space (x, z), which lies in c.
SectorSample sector_bspline(const SectorTexels &t, SectorCoord c, float x, float z);
// Largest |r step| along either axis, as metres of height per metre.
float sector_max_axis_slope(const SectorTexels &t);

// The toroidal window the GPU reads through SectorMap.slot[]: kSectorWindow^2 cells of
// (x, z, layer). Only a sector within kSectorWindow / 2 - 1 of `centre` on both axes is
// written, so no two written sectors share a cell; every other cell is
// (INT32_MIN, INT32_MIN, -1).
int sector_window_cell(SectorCoord c);
struct SectorLayer {
	SectorCoord c;
	int layer = -1;
};
std::vector<int32_t> sector_window(SectorCoord centre, const std::vector<SectorLayer> &layers);
int sector_window_lookup(const std::vector<int32_t> &window, SectorCoord c); // -1 when absent

} // namespace ve
```

- [ ] **Step 4: Write `sector.cpp`**

```cpp
#include "terrain/sector.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace ve {
namespace {
int imod(int a, int b) {
	const int m = a % b;
	return m < 0 ? m + b : m;
}
float lo16(uint32_t v) { return float(v & 0xffffu) / 65535.0f; }
float hi16(uint32_t v) { return float(v >> 16) / 65535.0f; }
// Cubic B-spline weights and their derivatives at fraction t. Mirrored in sector.glslh.
void weights(float t, float w[4], float d[4]) {
	const float s = 1.0f - t, t2 = t * t, t3 = t2 * t;
	w[0] = s * s * s / 6.0f;
	w[1] = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
	w[2] = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
	w[3] = t3 / 6.0f;
	d[0] = -0.5f * s * s;
	d[1] = 1.5f * t2 - 2.0f * t;
	d[2] = -1.5f * t2 + t + 0.5f;
	d[3] = 0.5f * t2;
}
} // namespace

SectorCoord sector_of(float x, float z) {
	return {int(std::floor(x / kSectorSizeM)), int(std::floor(z / kSectorSizeM))};
}

void sector_texel_pos(SectorCoord c, int tx, int tz, float *x, float *z) {
	*x = (float(c.x * kSectorTexels + tx - kSectorApron) + 0.5f) * kSectorTexelM;
	*z = (float(c.z * kSectorTexels + tz - kSectorApron) + 0.5f) * kSectorTexelM;
}

float sector_distance(SectorCoord c, float x, float z) {
	const float x0 = float(c.x) * kSectorSizeM, z0 = float(c.z) * kSectorSizeM;
	const float dx = std::max(0.0f, std::max(x0 - x, x - (x0 + kSectorSizeM)));
	const float dz = std::max(0.0f, std::max(z0 - z, z - (z0 + kSectorSizeM)));
	return std::sqrt(dx * dx + dz * dz);
}

SectorSample sector_bspline(const SectorTexels &t, SectorCoord c, float x, float z) {
	// Texel k's centre is at u = k: the knots are texel centres, apron included.
	const float ux = (x - float(c.x) * kSectorSizeM) / kSectorTexelM + float(kSectorApron) - 0.5f;
	const float uz = (z - float(c.z) * kSectorSizeM) / kSectorTexelM + float(kSectorApron) - 0.5f;
	const float bx = std::floor(ux), bz = std::floor(uz);
	float wx[4], dx[4], wz[4], dz[4];
	weights(ux - bx, wx, dx);
	weights(uz - bz, wz, dz);
	// A point exactly on the far edge can round one texel past the apron; clamp the base.
	const int ix = std::clamp(int(bx) - 1, 0, kSectorStride - 4);
	const int iz = std::clamp(int(bz) - 1, 0, kSectorStride - 4);
	SectorSample s;
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++) {
			const uint32_t v = t.texels[size_t((iz + j) * kSectorStride + ix + i)];
			const float r = lo16(v), g = hi16(v);
			s.r += wx[i] * wz[j] * r;
			s.drdx += dx[i] * wz[j] * r;
			s.drdz += wx[i] * dz[j] * r;
			s.g += wx[i] * wz[j] * g;
		}
	s.drdx /= kSectorTexelM;
	s.drdz /= kSectorTexelM;
	return s;
}

float sector_max_axis_slope(const SectorTexels &t) {
	int m = 0;
	for (int z = 0; z < kSectorStride; z++)
		for (int x = 0; x < kSectorStride; x++) {
			const int r = int(t.texels[size_t(z * kSectorStride + x)] & 0xffffu);
			if (x + 1 < kSectorStride)
				m = std::max(m, std::abs(int(t.texels[size_t(z * kSectorStride + x + 1)] & 0xffffu) - r));
			if (z + 1 < kSectorStride)
				m = std::max(m, std::abs(int(t.texels[size_t((z + 1) * kSectorStride + x)] & 0xffffu) - r));
		}
	return float(m) / 65535.0f * kSectorHeightSpanM / kSectorTexelM;
}

int sector_window_cell(SectorCoord c) {
	return imod(c.z, kSectorWindow) * kSectorWindow + imod(c.x, kSectorWindow);
}

std::vector<int32_t> sector_window(SectorCoord centre, const std::vector<SectorLayer> &layers) {
	std::vector<int32_t> w(size_t(kSectorWindow * kSectorWindow * 3));
	for (size_t i = 0; i < w.size(); i += 3) {
		w[i] = INT32_MIN;
		w[i + 1] = INT32_MIN;
		w[i + 2] = -1;
	}
	constexpr int kHalf = kSectorWindow / 2 - 1;
	for (const SectorLayer &l : layers) {
		if (std::abs(l.c.x - centre.x) > kHalf || std::abs(l.c.z - centre.z) > kHalf) continue;
		const size_t i = size_t(sector_window_cell(l.c)) * 3;
		w[i] = l.c.x;
		w[i + 1] = l.c.z;
		w[i + 2] = l.layer;
	}
	return w;
}

int sector_window_lookup(const std::vector<int32_t> &w, SectorCoord c) {
	const size_t i = size_t(sector_window_cell(c)) * 3;
	if (i + 2 >= w.size()) return -1;
	return (w[i] == c.x && w[i + 1] == c.z) ? w[i + 2] : -1;
}

} // namespace ve
```

- [ ] **Step 5: Run the tests**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: `Status: SUCCESS!`. `src/terrain/*.cpp` is already globbed into `pure_sources`, so no `SConstruct` edit.

- [ ] **Step 6: Commit**

```bash
git add extension/src/terrain/sector.h extension/src/terrain/sector.cpp extension/tests/test_sector.cpp
git commit -m "feat: sector coordinates, B-spline sampling and the toroidal sector window"
```

---

### Task 2: The host sector cache

**Files:**
- Create: `extension/src/terrain/sector_cache.h`, `extension/src/terrain/sector_cache.cpp`
- Test: `extension/tests/test_sector_cache.cpp`

**Interfaces:**
- Consumes: everything Task 1 produces; `ve::kSurfaceY` from `generator/generator.h`.
- Produces:
  - `class ve::SectorCache` with `SectorCache(float radius_m, float offset_x, float offset_z)`; `struct Plan { std::vector<SectorCoord> bake, evicted; }`; `Plan plan(float cam_x, float cam_z, int max_bakes)` (world space); `void insert(SectorCoord, std::shared_ptr<const SectorTexels>)`; `void abandon(SectorCoord)`; `void clear()`; `std::shared_ptr<const SectorTexels> find(SectorCoord) const` (field space); `std::vector<SectorCoord> needed_world(float min_x, float min_z, float max_x, float max_z) const`; `bool ready_world(float min_x, float min_z, float max_x, float max_z) const`; `SectorCoord centre() const`; `uint64_t version() const`; `std::vector<std::pair<SectorCoord, std::shared_ptr<const SectorTexels>>> snapshot() const`; `int max_resident() const`; `struct Stats { int resident, in_flight; int64_t inserted; float max_slope; int over_limit; float r_min, r_max; }`; `Stats stats() const`; `float offset_x() const`, `float offset_z() const`.
  - `struct ve::SectorGround { float height, dhdx, dhdz, ridge; }`; `SectorGround sector_ground(const SectorCache *, float water_y, float x, float z)` (field space; height above `kSurfaceY`).

- [ ] **Step 1: Write the failing tests**

`extension/tests/test_sector_cache.cpp`:
```cpp
#include <doctest/doctest.h>
#include "generator/generator.h"
#include "terrain/sector_cache.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace {
std::shared_ptr<const ve::SectorTexels> flat(float r, float slope = 0.0f) {
	auto t = std::make_shared<ve::SectorTexels>();
	const uint32_t v = uint32_t(std::lround(r * 65535.0f)) | (32768u << 16);
	t->texels.assign(size_t(ve::kSectorStride * ve::kSectorStride), v);
	t->max_slope = slope;
	return t;
}
} // namespace

TEST_CASE("plan asks for the nearest sectors first, once each") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	const ve::SectorCache::Plan a = c.plan(10.0f, 10.0f, 3);
	REQUIRE(a.bake.size() == 3);
	CHECK(a.bake[0] == ve::SectorCoord{0, 0}); // the camera's own sector, distance 0
	const ve::SectorCache::Plan b = c.plan(10.0f, 10.0f, 100);
	for (const ve::SectorCoord &s : b.bake)
		for (const ve::SectorCoord &t : a.bake) CHECK(s != t); // in flight: not asked twice
	CHECK(c.stats().in_flight == int(a.bake.size() + b.bake.size()));
}

TEST_CASE("an inserted sector is found, and readiness covers every overlapped sector") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	c.plan(10.0f, 10.0f, 0);
	c.insert({0, 0}, flat(0.5f));
	CHECK(c.find({0, 0}) != nullptr);
	CHECK(c.ready_world(1.0f, 1.0f, 400.0f, 400.0f));
	CHECK_FALSE(c.ready_world(1.0f, 1.0f, 420.0f, 400.0f)); // reaches into (1, 0)
	CHECK(c.stats().resident == 1);
}

TEST_CASE("readiness ignores sectors beyond the radius") {
	ve::SectorCache c(500.0f, 0.0f, 0.0f);
	c.plan(10.0f, 10.0f, 0);
	c.insert({0, 0}, flat(0.5f));
	// The rectangle spans sectors x = 0..5. (1, 0) starts 399.6 m away and is wanted; (2, 0)
	// onwards start past 500 m, are never baked, and so are never waited on (deviation 10).
	CHECK_FALSE(c.ready_world(100.0f, 100.0f, 2300.0f, 300.0f));
	c.insert({1, 0}, flat(0.5f));
	CHECK(c.ready_world(100.0f, 100.0f, 2300.0f, 300.0f));
	CHECK(c.find({2, 0}) == nullptr);
}

TEST_CASE("moving away evicts what left the radius plus one sector, and bumps the version") {
	ve::SectorCache c(500.0f, 0.0f, 0.0f);
	c.plan(10.0f, 10.0f, 0);
	c.insert({0, 0}, flat(0.5f));
	const uint64_t v = c.version();
	const ve::SectorCache::Plan p = c.plan(5000.0f, 10.0f, 0);
	REQUIRE(p.evicted.size() == 1);
	CHECK(p.evicted[0] == ve::SectorCoord{0, 0});
	CHECK(c.find({0, 0}) == nullptr);
	CHECK(c.version() > v);
}

TEST_CASE("the seed offset moves the wanted sectors into field space") {
	ve::SectorCache c(100.0f, 1000.0f, 0.0f);
	const ve::SectorCache::Plan p = c.plan(10.0f, 10.0f, 1);
	REQUIRE(p.bake.size() == 1);
	CHECK(p.bake[0] == ve::sector_of(1010.0f, 10.0f));
	CHECK(c.centre() == ve::sector_of(1010.0f, 10.0f));
}

TEST_CASE("sector_ground decodes height and falls back to the bottom of the range") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	const float water = 51.2f;
	const ve::SectorGround none = ve::sector_ground(&c, water, 5.0f, 5.0f);
	CHECK(none.height == doctest::Approx(water - ve::kSectorHeightBelowM - ve::kSurfaceY));
	c.insert({0, 0}, flat(0.5f));
	const ve::SectorGround g = ve::sector_ground(&c, water, 5.0f, 5.0f);
	CHECK(g.height == doctest::Approx(water - 64.0f + 256.0f - ve::kSurfaceY).epsilon(1e-4));
	CHECK(g.dhdx == doctest::Approx(0.0f));
	CHECK(g.ridge == doctest::Approx(0.0f).epsilon(1e-3));
	CHECK(ve::sector_ground(nullptr, water, 5.0f, 5.0f).height == doctest::Approx(none.height));
}

TEST_CASE("stats count sectors over the slope limit and track the encoded range") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	c.insert({0, 0}, flat(0.25f, 1.0f));
	c.insert({1, 0}, flat(0.75f, ve::kSectorSlopeLimit + 0.1f));
	const ve::SectorCache::Stats s = c.stats();
	CHECK(s.over_limit == 1);
	CHECK(s.max_slope == doctest::Approx(ve::kSectorSlopeLimit + 0.1f));
	CHECK(s.r_min == doctest::Approx(0.25f).epsilon(1e-4));
	CHECK(s.r_max == doctest::Approx(0.75f).epsilon(1e-4));
	c.clear();
	CHECK(c.stats().resident == 0);
}

TEST_CASE("max_resident bounds every resident set plan can leave behind") {
	ve::SectorCache c(4000.0f, 0.0f, 0.0f);
	// Bake everything wanted from one spot, then step the camera across a sector: what plan()
	// keeps (radius + one sector) must still fit in max_resident() layers.
	for (float x : {0.0f, 204.8f, 409.5f})
		for (float z : {0.0f, 100.0f, 409.5f}) {
			c.clear();
			for (float step : {0.0f, 300.0f, 600.0f}) {
				const ve::SectorCache::Plan p = c.plan(x + step, z, 100000);
				for (const ve::SectorCoord &s : p.bake) c.insert(s, flat(0.5f));
				c.plan(x + step, z, 0);
				CHECK(c.stats().resident <= c.max_resident());
			}
		}
}

TEST_CASE("readers holding a sector survive its eviction") {
	ve::SectorCache c(1000.0f, 0.0f, 0.0f);
	std::atomic<bool> stop{false};
	std::atomic<int> bad{0};
	std::vector<std::thread> readers;
	for (int i = 0; i < 4; i++)
		readers.emplace_back([&] {
			while (!stop.load()) {
				const auto t = c.find({0, 0});
				if (t && t->texels.size() != size_t(ve::kSectorStride * ve::kSectorStride)) bad++;
				if (t) (void)ve::sector_bspline(*t, {0, 0}, 100.0f, 100.0f);
			}
		});
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
	while (std::chrono::steady_clock::now() < end) {
		c.insert({0, 0}, flat(0.5f));
		c.clear();
	}
	stop = true;
	for (std::thread &t : readers) t.join();
	CHECK(bad.load() == 0);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile error, `terrain/sector_cache.h` not found.

- [ ] **Step 3: Write `sector_cache.h`**

```cpp
#pragma once
// The host copy of every resident sector (spec §5). The render device's bake ring inserts
// read-back bytes; every SectorMirror uploads from here; the CPU field samples here. One
// shared_mutex guards the maps, and every entry is an immutable shared_ptr, so a reader that
// holds one is never left with freed texels by an eviction.
//
// World-space in (cameras, rectangles), field-space out (SectorCoord): the cache adds the
// world seed's offset itself, so callers never mix the two spaces.
#include "terrain/sector.h"
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace ve {

class SectorCache {
public:
	// radius_m: sectors within this distance of the camera are wanted. One more sector of
	// distance is kept before eviction, so a camera on a boundary does not thrash.
	SectorCache(float radius_m, float offset_x, float offset_z);

	struct Plan {
		std::vector<SectorCoord> bake;    // in flight from now on, nearest first
		std::vector<SectorCoord> evicted; // dropped this call
	};
	// Render thread, once per frame, before anything evaluates the field.
	Plan plan(float cam_x, float cam_z, int max_bakes);
	void insert(SectorCoord c, std::shared_ptr<const SectorTexels> t);
	void abandon(SectorCoord c); // a bake that will not arrive; plan() may ask again
	void clear();                // drop everything, resident and in flight

	std::shared_ptr<const SectorTexels> find(SectorCoord c) const;
	// The wanted sectors (within the radius of the last plan's camera) that overlap the
	// world-space rectangle. Sectors beyond the radius are never baked, so nothing waits on
	// them (plan deviation 10).
	std::vector<SectorCoord> needed_world(float min_x, float min_z, float max_x, float max_z) const;
	bool ready_world(float min_x, float min_z, float max_x, float max_z) const;

	SectorCoord centre() const;
	uint64_t version() const; // bumps on every insert, eviction and clear
	std::vector<std::pair<SectorCoord, std::shared_ptr<const SectorTexels>>> snapshot() const;
	int max_resident() const;
	float offset_x() const { return offset_x_; }
	float offset_z() const { return offset_z_; }

	struct Stats {
		int resident = 0;
		int in_flight = 0;
		int64_t inserted = 0;
		float max_slope = 0.0f;
		int over_limit = 0;   // inserted sectors steeper than kSectorSlopeLimit
		float r_min = 1.0f;   // the encoded range actually used, over every insert
		float r_max = 0.0f;
	};
	Stats stats() const;

private:
	std::vector<SectorCoord> needed_locked(float min_x, float min_z, float max_x, float max_z) const;

	float radius_m_, offset_x_, offset_z_;
	mutable std::shared_mutex mu_;
	std::map<SectorCoord, std::shared_ptr<const SectorTexels>> resident_;
	std::set<SectorCoord> in_flight_;
	float cam_fx_ = 0.0f, cam_fz_ = 0.0f; // field-space camera of the last plan
	SectorCoord centre_{};
	uint64_t version_ = 0;
	Stats stats_{};
};

// Mirror of sector_ground() in shaders/sector.glslh. Field-space x, z. Height is metres above
// kSurfaceY, gradient in metres per metre, ridge in -1..1. Where no sector is resident (or
// there is no cache) the height is the bottom of the encoded range: open air above the
// valley floor, never a false solid (spec §5.4).
struct SectorGround {
	float height = 0.0f, dhdx = 0.0f, dhdz = 0.0f, ridge = 0.0f;
};
SectorGround sector_ground(const SectorCache *cache, float water_y, float x, float z);

} // namespace ve
```

- [ ] **Step 4: Write `sector_cache.cpp`**

```cpp
#include "terrain/sector_cache.h"
#include "generator/generator.h" // kSurfaceY
#include <algorithm>
#include <cmath>
#include <mutex>

namespace ve {

SectorCache::SectorCache(float radius_m, float offset_x, float offset_z)
		: radius_m_(radius_m), offset_x_(offset_x), offset_z_(offset_z) {}

SectorCache::Plan SectorCache::plan(float cam_x, float cam_z, int max_bakes) {
	const float fx = cam_x + offset_x_, fz = cam_z + offset_z_;
	std::unique_lock lock(mu_);
	cam_fx_ = fx;
	cam_fz_ = fz;
	centre_ = sector_of(fx, fz);
	Plan p;
	const float keep = radius_m_ + kSectorSizeM;
	for (auto it = resident_.begin(); it != resident_.end();) {
		if (sector_distance(it->first, fx, fz) > keep) {
			p.evicted.push_back(it->first);
			it = resident_.erase(it);
			version_++;
		} else {
			++it;
		}
	}
	struct Want {
		float d;
		SectorCoord c;
	};
	std::vector<Want> want;
	const int reach = int(std::ceil(radius_m_ / kSectorSizeM)) + 1;
	for (int z = centre_.z - reach; z <= centre_.z + reach; z++)
		for (int x = centre_.x - reach; x <= centre_.x + reach; x++) {
			const SectorCoord c{x, z};
			const float d = sector_distance(c, fx, fz);
			if (d > radius_m_ || resident_.count(c) || in_flight_.count(c)) continue;
			want.push_back({d, c});
		}
	std::sort(want.begin(), want.end(), [](const Want &a, const Want &b) {
		return a.d != b.d ? a.d < b.d : a.c < b.c;
	});
	for (int i = 0; i < int(want.size()) && i < max_bakes; i++) {
		in_flight_.insert(want[size_t(i)].c);
		p.bake.push_back(want[size_t(i)].c);
	}
	return p;
}

void SectorCache::insert(SectorCoord c, std::shared_ptr<const SectorTexels> t) {
	if (!t) return;
	float r_min = 1.0f, r_max = 0.0f;
	for (uint32_t v : t->texels) {
		const float r = float(v & 0xffffu) / 65535.0f;
		r_min = std::min(r_min, r);
		r_max = std::max(r_max, r);
	}
	std::unique_lock lock(mu_);
	in_flight_.erase(c);
	resident_[c] = std::move(t);
	const SectorTexels &in = *resident_[c];
	stats_.inserted++;
	stats_.max_slope = std::max(stats_.max_slope, in.max_slope);
	if (in.max_slope > kSectorSlopeLimit) stats_.over_limit++;
	stats_.r_min = std::min(stats_.r_min, r_min);
	stats_.r_max = std::max(stats_.r_max, r_max);
	version_++;
}

void SectorCache::abandon(SectorCoord c) {
	std::unique_lock lock(mu_);
	in_flight_.erase(c);
}

void SectorCache::clear() {
	std::unique_lock lock(mu_);
	resident_.clear();
	in_flight_.clear();
	version_++;
}

std::shared_ptr<const SectorTexels> SectorCache::find(SectorCoord c) const {
	std::shared_lock lock(mu_);
	const auto it = resident_.find(c);
	return it == resident_.end() ? nullptr : it->second;
}

std::vector<SectorCoord> SectorCache::needed_locked(float min_x, float min_z, float max_x,
		float max_z) const {
	std::vector<SectorCoord> out;
	const SectorCoord lo = sector_of(min_x + offset_x_, min_z + offset_z_);
	const SectorCoord hi = sector_of(max_x + offset_x_, max_z + offset_z_);
	for (int z = lo.z; z <= hi.z; z++)
		for (int x = lo.x; x <= hi.x; x++)
			if (sector_distance({x, z}, cam_fx_, cam_fz_) <= radius_m_) out.push_back({x, z});
	return out;
}

std::vector<SectorCoord> SectorCache::needed_world(float min_x, float min_z, float max_x,
		float max_z) const {
	std::shared_lock lock(mu_);
	return needed_locked(min_x, min_z, max_x, max_z);
}

bool SectorCache::ready_world(float min_x, float min_z, float max_x, float max_z) const {
	std::shared_lock lock(mu_);
	for (const SectorCoord &c : needed_locked(min_x, min_z, max_x, max_z))
		if (!resident_.count(c)) return false;
	return true;
}

SectorCoord SectorCache::centre() const {
	std::shared_lock lock(mu_);
	return centre_;
}

uint64_t SectorCache::version() const {
	std::shared_lock lock(mu_);
	return version_;
}

std::vector<std::pair<SectorCoord, std::shared_ptr<const SectorTexels>>> SectorCache::snapshot() const {
	std::shared_lock lock(mu_);
	return {resident_.begin(), resident_.end()};
}

int SectorCache::max_resident() const {
	// Every sector whose distance to the camera is within radius + one sector (plan()'s
	// eviction distance) has its centre within radius + size + half a diagonal of the camera
	// sector's centre. Counting that disc is a bound for any camera position.
	const float reach = (radius_m_ + kSectorSizeM + kSectorSizeM * 0.7072f) / kSectorSizeM + 0.7072f;
	const int r = int(std::ceil(reach));
	int n = 0;
	for (int z = -r; z <= r; z++)
		for (int x = -r; x <= r; x++)
			if (std::sqrt(float(x * x + z * z)) <= reach) n++;
	return n;
}

SectorCache::Stats SectorCache::stats() const {
	std::shared_lock lock(mu_);
	Stats s = stats_;
	s.resident = int(resident_.size());
	s.in_flight = int(in_flight_.size());
	return s;
}

SectorGround sector_ground(const SectorCache *cache, float water_y, float x, float z) {
	SectorGround g;
	const float lo = water_y - kSectorHeightBelowM - kSurfaceY;
	g.height = lo;
	if (cache == nullptr) return g;
	const SectorCoord c = sector_of(x, z);
	const std::shared_ptr<const SectorTexels> t = cache->find(c);
	if (!t) return g;
	const SectorSample s = sector_bspline(*t, c, x, z);
	g.height = lo + s.r * kSectorHeightSpanM;
	g.dhdx = s.drdx * kSectorHeightSpanM;
	g.dhdz = s.drdz * kSectorHeightSpanM;
	g.ridge = s.g * 2.0f - 1.0f;
	return g;
}

} // namespace ve
```

- [ ] **Step 5: Run the tests**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: `Status: SUCCESS!`.

- [ ] **Step 6: Commit**

```bash
git add extension/src/terrain/sector_cache.h extension/src/terrain/sector_cache.cpp extension/tests/test_sector_cache.cpp
git commit -m "feat: the host sector cache, its residency plan and the CPU ground decode"
```

---

### Task 3: The pipeline resolves one map stage

**Files:**
- Modify: `extension/src/terrain/stage_manifest.h`, `stage_manifest.cpp` (record dotted `//!out`)
- Modify: `extension/src/terrain/pipeline.h`, `pipeline.cpp` (map-stage rules)
- Modify: `extension/src/terrain/field_codegen.cpp` (skip map stages in `eval_base_field`; emit `VE_SECTOR_MAP`)
- Modify: `extension/src/terrain/stage_library.h` (`FieldResources` carries the cache)
- Modify: `extension/src/terrain/pipeline_field_generator.h`, `.cpp` (skip map stages; hold the cache)
- Test: `extension/tests/test_pipeline_map_stage.cpp`

**Interfaces:**
- Consumes: `ve::kSectorTexels`, `ve::SectorCache`.
- Produces: `StageManifest::map_writes` (`std::vector<ResourceDecl>`); `ResolvedPipeline::map_stage` (`int`, −1 when none); `struct FieldResources { const SectorCache *sectors = nullptr; }`; `PipelineFieldGenerator::set_sector_cache(std::shared_ptr<const SectorCache>)`; generated GLSL `#define VE_SECTOR_MAP 1` and `vec2 ve_sector_map(vec2 xz)` when the pipeline has a map stage. A map stage's GLSL contract: it defines `vec2 stage_<name>(vec2 xz)` taking a field-space position and returning `(R, G)` in `[0, 1]`.

- [ ] **Step 1: Write the failing tests**

`extension/tests/test_pipeline_map_stage.cpp`:
```cpp
#include <doctest/doctest.h>
#include "generator/generator.h" // kSurfaceY
#include "terrain/field_codegen.h"
#include "terrain/pipeline.h"
#include "terrain/pipeline_field_generator.h"
#include "terrain/stage_manifest.h"
#include <memory>

namespace {
const char *kMap = R"(//!stage m
//!kind map
//!domain sector2d 256x256
//!out sector.terrain : image2d_rg16
//!param amp : float = 2.0
vec2 stage_m(vec2 xz) { return vec2(0.5 + 0.0 * P.m_amp, 0.5); }
)";
// Borrows ve::stage_flat as its CPU mirror: this test is about resolution, not shape.
const char *kField = R"(//!stage flat
//!kind field
//!sample sector.terrain : texture2d_rg16
//!out sdf : float
//!out height : float
//!param level : float = 2.0
//!lipschitz add 1.0
//!cpu ve::stage_flat
void stage_flat(inout FieldCtx ctx) { ctx.height = P.flat_level; ctx.sdf = ctx.p.y - SURFACE_Y - ctx.height; }
)";

ve::StageManifest parse(const std::string &src) {
	ve::StageManifest m;
	std::string err;
	REQUIRE_MESSAGE(ve::parse_stage_manifest(src, &m, &err), err);
	return m;
}

bool resolve(std::vector<ve::StageManifest> st, ve::ResolvedPipeline *out, std::string *err) {
	ve::PipelineDesc d;
	for (size_t i = 0; i < st.size(); i++) d.stages.push_back({"s" + std::to_string(i), {}});
	return ve::resolve_pipeline(d, st, out, err);
}

std::string with(const std::string &src, const std::string &from, const std::string &to) {
	std::string s = src;
	s.replace(s.find(from), from.size(), to);
	return s;
}
} // namespace

TEST_CASE("a dotted //!out is recorded as a map write") {
	const ve::StageManifest m = parse(kMap);
	REQUIRE(m.map_writes.size() == 1);
	CHECK(m.map_writes[0].name == "sector.terrain");
	CHECK(m.map_writes[0].type == "image2d_rg16");
	CHECK(m.writes.empty());
}

TEST_CASE("a map stage ahead of a sampling field stage resolves") {
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE_MESSAGE(resolve({parse(kMap), parse(kField)}, &p, &err), err);
	CHECK(p.map_stage == 0);
	CHECK(p.cpu_exact); // a map stage has no //!cpu and needs none
	REQUIRE(p.resources.size() == 1);
	CHECK(p.resources[0].name == "sector.terrain");
	bool has_amp = false;
	for (const ve::ParamDecl &pd : p.params) has_amp |= pd.name == "m.amp";
	CHECK(has_amp);
	CHECK(p.lipschitz == doctest::Approx(1.0f));
}

TEST_CASE("map-stage rules are enforced with the stage named") {
	ve::ResolvedPipeline p;
	std::string err;
	SUBCASE("after a field stage") {
		CHECK_FALSE(resolve({parse(kField), parse(kMap)}, &p, &err));
		CHECK(err.find("'m'") != std::string::npos);
	}
	SUBCASE("a second map stage") {
		CHECK_FALSE(resolve({parse(kMap), parse(with(kMap, "//!stage m", "//!stage m2")), parse(kField)}, &p, &err));
		CHECK(err.find("m2") != std::string::npos);
	}
	SUBCASE("a domain other than sector2d 256x256") {
		CHECK_FALSE(resolve({parse(with(kMap, "256x256", "128x128")), parse(kField)}, &p, &err));
	}
	SUBCASE("no sector write") {
		CHECK_FALSE(resolve({parse(with(kMap, "//!out sector.terrain : image2d_rg16\n", "")), parse(kField)}, &p, &err));
	}
	SUBCASE("a field channel on a map stage") {
		CHECK_FALSE(resolve({parse(with(kMap, "//!param", "//!out height : float\n//!param")), parse(kField)}, &p, &err));
	}
	SUBCASE("a sampled resource no map stage writes") {
		CHECK_FALSE(resolve({parse(kField)}, &p, &err));
		CHECK(err.find("sector.terrain") != std::string::npos);
	}
	SUBCASE("a format mismatch") {
		CHECK_FALSE(resolve({parse(kMap), parse(with(kField, "texture2d_rg16", "texture2d_r32f"))}, &p, &err));
	}
	SUBCASE("a map output nothing samples") {
		CHECK_FALSE(resolve({parse(kMap), parse(with(kField, "//!sample sector.terrain : texture2d_rg16\n", ""))}, &p, &err));
	}
}

TEST_CASE("codegen calls only field stages and exposes the map stage to the bake") {
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE(resolve({parse(kMap), parse(kField)}, &p, &err));
	const std::string src = ve::generate_field_glslh(p, "");
	CHECK(src.find("\tstage_m(ctx);") == std::string::npos);
	CHECK(src.find("\tstage_flat(ctx);") != std::string::npos);
	CHECK(src.find("#define VE_SECTOR_MAP 1") != std::string::npos);
	CHECK(src.find("vec2 ve_sector_map(vec2 xz) { return stage_m(xz); }") != std::string::npos);
}

TEST_CASE("the CPU generator skips the map stage and carries the cache") {
	ve::ResolvedPipeline p;
	std::string err;
	REQUIRE(resolve({parse(kMap), parse(kField)}, &p, &err));
	std::unique_ptr<ve::PipelineFieldGenerator> g(ve::PipelineFieldGenerator::create(p, &err));
	REQUIRE_MESSAGE(g != nullptr, err);
	g->set_sector_cache(std::make_shared<ve::SectorCache>(1000.0f, 0.0f, 0.0f));
	CHECK(g->sample(0.0f, 100.0f, 0.0f).sdf == doctest::Approx(100.0f - ve::kSurfaceY - 2.0f));
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile errors (`map_writes`, `map_stage`, `set_sector_cache` undeclared).

- [ ] **Step 3: Record map writes in the manifest**

In `stage_manifest.h`, add to `StageManifest` after `samples`:
```cpp
    // A map stage's sector resources (`//!out sector.<name> : image2d_<fmt>`). Field stages
    // read them through //!sample.
    std::vector<ResourceDecl> map_writes;
```
In `stage_manifest.cpp`, replace the dotted-name branch in the `in`/`out` handler:
```cpp
			// A dotted name is a sector resource (scope.name), not a FieldCtx channel. A map
			// stage's //!out records it; a dotted //!in is accepted and ignored -- no map stage
			// reads another's output (one map stage per pipeline, plan deviation 2).
			if (n.find('.') != std::string::npos) {
				if (key == "out") {
					ResourceDecl r;
					r.name = n;
					r.type = ty;
					out->map_writes.push_back(r);
				}
			} else {
```

- [ ] **Step 4: Resolve the map stage**

In `pipeline.h`, add to `ResolvedPipeline` after `resources`:
```cpp
    int map_stage = -1;                     // index into stages of the sector2d map stage; -1 when none
```
In `pipeline.cpp`, add `#include "terrain/sector.h"`, and in the anonymous namespace:
```cpp
// "image2d_rg16" / "texture2d_rg16" -> "rg16": producer and consumer must agree on this half.
std::string resource_format(const std::string &type) {
	const size_t at = type.find("2d_");
	return at == std::string::npos ? type : type.substr(at + 3);
}
```
In `resolve_pipeline`, declare `bool seen_field_stage = false;` beside `bool wrote_sdf`. Replace the start of the loop body (from `StageManifest m = loaded[i];` through the end of the `if (m.cpu_symbol.empty()) {...}` block) with:
```cpp
		StageManifest m = loaded[i];
		for (size_t j = 0; j < i; j++)
			if (loaded[j].name == m.name) return fail("duplicate stage name: " + m.name);
		if (m.kind == StageKind::kMap) {
			if (seen_field_stage)
				return fail("map stage '" + m.name + "' must come before every field stage");
			if (out->map_stage >= 0)
				return fail("map stage '" + m.name + "' is a second map stage; a pipeline has at "
						"most one sector2d map stage");
			if (m.domain != "sector2d" || m.domain_w != kSectorTexels || m.domain_h != kSectorTexels)
				return fail("map stage '" + m.name + "' must declare //!domain sector2d 256x256");
			if (m.map_writes.size() != 1)
				return fail("map stage '" + m.name + "' must write exactly one sector resource");
			if (resource_format(m.map_writes[0].type) != "rg16")
				return fail("map stage '" + m.name + "' writes " + m.map_writes[0].type +
						"; sector resources are rg16 (spec 4.2)");
			if (!m.reads.empty() || !m.writes.empty() || !m.samples.empty() ||
					m.lipschitz_mode != LipschitzMode::kNone)
				return fail("map stage '" + m.name + "' declares field channels, a //!sample or a "
						"//!lipschitz; it writes only its sector resource");
			out->map_stage = int(i);
		} else {
			seen_field_stage = true;
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
		}
```
(The removed lines are the old `if (m.kind != StageKind::kField) return fail(... "Plan A resolves field stages only")`, the duplicate-name loop, and the old `cpu_symbol` block — all three now live above.)

After the cross-stage `//!use` loop and before the resource sort, add:
```cpp
	// Every sampled resource is the map stage's output, at the same format, and the map
	// stage's output is sampled -- an unsampled bake is work nothing reads.
	for (const ResourceDecl &r : out->resources) {
		if (out->map_stage < 0 || out->stages[size_t(out->map_stage)].map_writes[0].name != r.name)
			return fail("resource '" + r.name + "' is sampled, but no map stage writes it");
		const ResourceDecl &w = out->stages[size_t(out->map_stage)].map_writes[0];
		if (resource_format(w.type) != resource_format(r.type))
			return fail("resource '" + r.name + "' is written as " + w.type + " and sampled as " + r.type);
	}
	if (out->map_stage >= 0 && out->resources.empty())
		return fail("map stage '" + out->stages[size_t(out->map_stage)].name + "' writes " +
				out->stages[size_t(out->map_stage)].map_writes[0].name + ", but no field stage samples it");
```

- [ ] **Step 5: Codegen**

In `field_codegen.cpp`, change the `eval_base_field` call loop to field stages only:
```cpp
	for (const StageManifest &s : p.stages)
		if (s.kind == StageKind::kField) o << "\tstage_" << s.name << "(ctx);\n";
```
and, after the stage-body loop (`for (const StageManifest &s : p.stages) o << s.body << "\n";`), add:
```cpp
	// The bake shader's entry (shaders/sector_bake.comp.glsl). Emitted only when a map stage
	// exists, so every other pipeline's source stays byte-identical.
	if (p.map_stage >= 0)
		o << "#define VE_SECTOR_MAP 1\nvec2 ve_sector_map(vec2 xz) { return stage_"
		  << p.stages[size_t(p.map_stage)].name << "(xz); }\n\n";
```

- [ ] **Step 6: Carry the cache to the CPU stages**

In `stage_library.h`, replace `struct FieldResources {};  // Plan A: ...` with:
```cpp
class SectorCache;
// What a CPU mirror may sample besides its channels and params: the host sector cache, null
// in a pipeline with no map stage (spec §5.1).
struct FieldResources {
	const SectorCache *sectors = nullptr;
};
```
In `pipeline_field_generator.h`, add `#include "terrain/sector_cache.h"`, a public method and a member:
```cpp
	// The host cache a map-stage pipeline's field stages sample. Set once, before any sample.
	void set_sector_cache(std::shared_ptr<const SectorCache> c) { sectors_ = std::move(c); }
...
	std::shared_ptr<const SectorCache> sectors_;
```
In `pipeline_field_generator.cpp`, at the top of the `for (const StageManifest &s : p.stages)` loop in `create`, before the `cpu_symbol.empty()` check:
```cpp
		if (s.kind == StageKind::kMap) {
			// Baked on the GPU and read back into the cache; the CPU never runs it.
			g->fns_.push_back(nullptr);
			g->slot_blobs_.push_back(nullptr);
			g->param_blobs_.push_back(nullptr);
			continue;
		}
```
and in `sample()` replace `FieldResources res;` with `FieldResources res{sectors_.get()};`.

- [ ] **Step 7: Run every native test**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: `Status: SUCCESS!` — including `the default pipeline generates the committed source`, which proves Default's generated source did not move.

- [ ] **Step 8: Commit**

```bash
git add extension/src/terrain extension/tests/test_pipeline_map_stage.cpp
git commit -m "feat: resolve one sector2d map stage ahead of the field stages"
```

---

### Task 4: Bake, read back and mirror sectors on the render device

**Files:**
- Create: `shaders/sector.glslh`, `shaders/sector_bake.comp.glsl`
- Create: `extension/src/render/sector_mirror.h`, `sector_mirror.cpp`, `sector_context.h`, `sector_context.cpp`
- Create: `shaders/stages/sector_fixture.map.glslh`, `shaders/stages/sector_fixture.field.glslh`, `tests/fixtures/sector_fixture.pipeline`
- Modify: `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh` (regenerated)
- Modify: `extension/src/render/field_context_set.h`, `.cpp`, `orchestrator.h`, `orchestrator.cpp`, `frame.cpp`
- Modify: `extension/src/core/world_store.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/terrain/builtin_stages.cpp` (fixture mirror)
- Modify: `extension/src/debug/hooks.h`, `hooks.cpp`, `hooks_world.cpp`
- Test: `tests/test_sectors.gd`; modify `tests/test_field_diff.gd`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  - GLSL (`shaders/sector.glslh`): constants `SECTOR_TEXELS`, `SECTOR_APRON`, `SECTOR_STRIDE`, `SECTOR_TEXEL_M`, `SECTOR_SIZE_M`, `SECTOR_WINDOW`, `SECTOR_HEIGHT_BELOW_M`, `SECTOR_HEIGHT_SPAN_M`; `ivec2 sector_of(vec2)`; `int sector_layer(ivec2)`; `vec4 sector_sample(vec2 xz)` (`x = -1` when absent); `vec4 sector_ground(vec2 xz, float water_y)` (height above `SURFACE_Y`, dh/dx, dh/dz, ridge). It reads the resource named `sector.terrain` (GLSL `sector_terrain`).
  - `ve::SectorBakePush { int32_t sector[4]; }` and the `SECTOR_BAKE_PUSH_FIELDS` macro.
  - `godot::SectorMirror` (`initialize(RenderingDevice *, int layers)`, `teardown()`, `sync(const ve::SectorCache &)`, `bool has(ve::SectorCoord) const`, `RID array() const`, `RID window() const`, `RID sampler() const`, `int uploaded() const`).
  - `godot::SectorContext` (`initialize(RenderingDevice *, std::shared_ptr<ve::SectorCache>)`, `teardown()`, `run_frame(RenderingDevice *, float cam_x, float cam_z, const FieldContextSet *)`, `bool ready_on_render(float min_x, float min_z, float max_x, float max_z) const`, `bool ready_on_host(...) const`, `const SectorMirror &mirror() const`, `int64_t bakes_dispatched() const`).
  - `FieldContextSet::initialize(RenderingDevice *, RID shader, const ve::ResolvedPipeline &, const SectorMirror *sectors = nullptr)`.
  - `RenderPasses::sectors` (`SectorContext *`).
  - `WorldStore::sector_cache()` / `set_sector_cache(std::shared_ptr<ve::SectorCache>)`.
  - Hooks: `debug_pump_sectors(cam: Vector3, half_extent: float, max_frames: int) -> int`, `debug_sector_stats() -> Dictionary`, `debug_sector_texels(sx: int, sz: int) -> PackedByteArray`, `debug_sector_clear()`, `debug_field_set(rd: RenderingDevice, shader: RID) -> RID`, `debug_release_field_set()`.

- [ ] **Step 1: Write the fixture stages and pipeline**

`shaders/stages/sector_fixture.map.glslh`:
```glsl
//!stage     sector_fixture
//!kind      map
//!domain    sector2d 256x256
//!out       sector.terrain : image2d_rg16
//!param     water_y : float = 51.2

// The sector tier's own fixture (tests/test_sectors.gd, tests/test_sector_gating.gd): a
// smooth analytic height, so a test can tell a wrong texel from a right one. Not a terrain.
#include "sector.glslh"

vec2 stage_sector_fixture(vec2 xz) {
	float y = P.sector_fixture_water_y + 20.0 * sin(xz.x * 0.011) * cos(xz.y * 0.013);
	return vec2((y - (P.sector_fixture_water_y - SECTOR_HEIGHT_BELOW_M)) / SECTOR_HEIGHT_SPAN_M,
			0.5 + 0.5 * sin(xz.y * 0.002));
}
```
`shaders/stages/sector_fixture.field.glslh`:
```glsl
//!stage     sector_fixture_ground
//!kind      field
//!sample    sector.terrain : texture2d_rg16
//!out       sdf : float
//!out       height : float
//!use       sector_fixture.water_y
//!lipschitz add 1.42
//!cpu       ve::stage_sector_fixture_ground

// Bound: |grad h| <= 20 * |(0.011, 0.013)| = 0.34, so sqrt(1 + 0.34^2) = 1.06; 1.42 is the
// sqrt(2) per-axis B-spline bound with margin, the same shape fjord.field.glslh uses.
#include "sector.glslh"

void stage_sector_fixture_ground(inout FieldCtx ctx) {
	vec4 g = sector_ground(ctx.p.xz, P.sector_fixture_water_y);
	ctx.height = g.x;
	ctx.sdf = ctx.p.y - SURFACE_Y - ctx.height;
}
```
`tests/fixtures/sector_fixture.pipeline`:
```
# The sector tier's fixture (plan Task 4). Not a world type: assets/pipelines/ is enumerated
# by the world-type menu and by test_field_diff, so it lives here and test_field_diff adds it
# by name.
lipschitz 1.5
stage stages/sector_fixture.map.glslh
stage stages/sector_fixture.field.glslh
stage stages/height_bands.field.glslh
```

- [ ] **Step 2: Write `shaders/sector.glslh`**

```glsl
#ifndef VE_SECTOR_GLSLH
#define VE_SECTOR_GLSLH
// Mirror of extension/src/terrain/sector.h and sector_cache.h's sector_ground (spec §4.4,
// §4.5). Included by map and field stage bodies, so it sits inside the generated field.glslh
// after the set-1 declarations: it reads sector_map (binding 1) and sector_terrain (binding 2,
// the resource the pipeline names sector.terrain).

const int SECTOR_TEXELS = 256;
const int SECTOR_APRON = 2;
const int SECTOR_STRIDE = 260;
const float SECTOR_TEXEL_M = 1.6;
const float SECTOR_SIZE_M = 409.6;
const int SECTOR_WINDOW = 24;
const float SECTOR_HEIGHT_BELOW_M = 64.0;
const float SECTOR_HEIGHT_SPAN_M = 512.0;

// GLSL leaves % undefined for negative operands; this floors.
int sector_imod(int a, int b) { return a - b * int(floor(float(a) / float(b))); }

ivec2 sector_of(vec2 xz) { return ivec2(floor(xz / SECTOR_SIZE_M)); }

// The texture-array layer holding sector sc, or -1. A stale or never-written cell carries
// another sector's coordinate (or INT_MIN) and reads as absent.
int sector_layer(ivec2 sc) {
	int i = 3 * (sector_imod(sc.y, SECTOR_WINDOW) * SECTOR_WINDOW + sector_imod(sc.x, SECTOR_WINDOW));
	if (i + 2 >= sector_map.slot.length()) return -1;
	if (sector_map.slot[i] != sc.x || sector_map.slot[i + 1] != sc.y) return -1;
	return sector_map.slot[i + 2];
}

vec4 sector_bspline_w(float t) {
	float s = 1.0 - t, t2 = t * t, t3 = t2 * t;
	return vec4(s * s * s, 3.0 * t3 - 6.0 * t2 + 4.0, -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0, t3) / 6.0;
}
vec4 sector_bspline_dw(float t) {
	float s = 1.0 - t, t2 = t * t;
	return vec4(-0.5 * s * s, 1.5 * t2 - 2.0 * t, -1.5 * t2 + t + 0.5, 0.5 * t2);
}

// vec4(r, dr/dx per metre, dr/dz per metre, g); x = -1 where no sector is resident.
vec4 sector_sample(vec2 xz) {
	ivec2 sc = sector_of(xz);
	int layer = sector_layer(sc);
	if (layer < 0) return vec4(-1.0, 0.0, 0.0, 0.0);
	vec2 u = (xz - vec2(sc) * SECTOR_SIZE_M) / SECTOR_TEXEL_M + float(SECTOR_APRON) - 0.5;
	vec2 b = floor(u);
	vec2 t = u - b;
	ivec2 base = clamp(ivec2(b) - 1, ivec2(0), ivec2(SECTOR_STRIDE - 4));
	vec4 wx = sector_bspline_w(t.x), wz = sector_bspline_w(t.y);
	vec4 dx = sector_bspline_dw(t.x), dz = sector_bspline_dw(t.y);
	float r = 0.0, rx = 0.0, rz = 0.0, g = 0.0;
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++) {
			vec2 v = texelFetch(sector_terrain, ivec3(base + ivec2(i, j), layer), 0).rg;
			r += wx[i] * wz[j] * v.x;
			rx += dx[i] * wz[j] * v.x;
			rz += wx[i] * dz[j] * v.x;
			g += wx[i] * wz[j] * v.y;
		}
	return vec4(r, rx / SECTOR_TEXEL_M, rz / SECTOR_TEXEL_M, g);
}

// Height above SURFACE_Y, its gradient, and the ridge value in -1..1. Where no sector is
// resident: the bottom of the encoded range, open air above the valley floor (spec §5.4).
vec4 sector_ground(vec2 xz, float water_y) {
	float lo = water_y - SECTOR_HEIGHT_BELOW_M - SURFACE_Y;
	vec4 s = sector_sample(xz);
	if (s.x < 0.0) return vec4(lo, 0.0, 0.0, 0.0);
	return vec4(lo + s.x * SECTOR_HEIGHT_SPAN_M, s.y * SECTOR_HEIGHT_SPAN_M,
			s.z * SECTOR_HEIGHT_SPAN_M, s.w * 2.0 - 1.0);
}

#endif
```

- [ ] **Step 3: Add the bake push block and the bake shader**

In `extension/src/gpu_layout/blocks.h`, after `struct BrickGenPush {...};`:
```cpp
struct SectorBakePush {
	int32_t sector[4]; // xy = field-space sector coordinate, zw unused
};
```
after `kBrickGenPushFields`:
```cpp
inline constexpr Field kSectorBakePushFields[] = {
	VE_LAYOUT_FIELD(SectorBakePush, sector, IVec4, 0),
};
```
and in the block list after the `BrickGenPush` line:
```cpp
	VE_LAYOUT_BLOCK(SectorBakePush, "SECTOR_BAKE_PUSH_FIELDS", kSectorBakePushFields),
```
Run: `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc="*generated*" ; git diff --stat ../shaders/generated`
Expected: only `shaders/generated/blocks.glslh` changes, gaining `#define SECTOR_BAKE_PUSH_FIELDS \ ivec4 sector;`.

`shaders/sector_bake.comp.glsl`:
```glsl
#[compute]
#version 460

// One sector of the pipeline's map stage (spec §4): every texel, apron included, evaluated at
// its field-space centre and packed as the RG16 the mirrors upload. Compiles in every
// pipeline -- without a map stage the body is empty and SectorContext never exists to
// dispatch it -- because the reload preflight compiles every shader under res://shaders/.
#define FIELD_OP_POOL_BINDING 1
#include "generated/blocks.glslh"
#include "common.glslh"
#include "field.glslh"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, std430) writeonly buffer BakeOut { uint texel[]; } bake_out;
layout(push_constant, std430) uniform Push { SECTOR_BAKE_PUSH_FIELDS } pc;

void main() {
#ifdef VE_SECTOR_MAP
	ivec2 t = ivec2(gl_GlobalInvocationID.xy);
	if (t.x >= SECTOR_STRIDE || t.y >= SECTOR_STRIDE) return;
	vec2 xz = (vec2(pc.sector.xy * SECTOR_TEXELS + t - SECTOR_APRON) + 0.5) * SECTOR_TEXEL_M;
	bake_out.texel[t.y * SECTOR_STRIDE + t.x] = packUnorm2x16(clamp(ve_sector_map(xz), 0.0, 1.0));
#endif
}
```

- [ ] **Step 4: Write `SectorMirror`**

`extension/src/render/sector_mirror.h`:
```cpp
#pragma once
// One device's copy of the resident sectors (spec §4.3–4.4, plan deviation 1): a
// texture2d_array filled from the host SectorCache, and the toroidal window the field stages
// read through set 1 binding 1. The render device and the mesher's worker device each own
// one. The bytes come from the cache, so the two devices cannot disagree.
#include "terrain/sector_cache.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <climits>
#include <cstdint>
#include <map>
#include <vector>

namespace godot {

class SectorMirror {
public:
	~SectorMirror();
	// `layers` is the cache's max_resident(): the array never grows, so the uniform sets that
	// bind it never need rebuilding.
	bool initialize(RenderingDevice *rd, int layers);
	void teardown();
	// Uploads every cached sector not yet on this device, frees the layers of sectors the
	// cache dropped, and rewrites the window when either changed or the centre moved. Call
	// outside any open compute list.
	void sync(const ve::SectorCache &cache);
	bool has(ve::SectorCoord c) const { return layer_of_.count(c) != 0; }
	RID array() const { return array_; }
	RID window() const { return window_; }
	RID sampler() const { return sampler_; }
	int uploaded() const { return int(layer_of_.size()); }

private:
	RenderingDevice *rd_ = nullptr;
	RID array_, window_, sampler_;
	std::map<ve::SectorCoord, int> layer_of_;
	std::vector<int> free_layers_;
	uint64_t synced_version_ = UINT64_MAX;
	ve::SectorCoord synced_centre_{INT_MIN, INT_MIN};
};

} // namespace godot
```
`extension/src/render/sector_mirror.cpp`:
```cpp
#include "render/sector_mirror.h"
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <cstring>
#include <set>

using namespace godot;

namespace {
PackedByteArray bytes_of(const void *data, size_t n) {
	PackedByteArray b;
	b.resize(int64_t(n));
	if (n) std::memcpy(b.ptrw(), data, n);
	return b;
}
} // namespace

SectorMirror::~SectorMirror() { teardown(); }

bool SectorMirror::initialize(RenderingDevice *rd, int layers) {
	teardown();
	rd_ = rd;
	if (rd == nullptr || layers <= 0) return false;
	Ref<RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
	fmt->set_format(RenderingDevice::DATA_FORMAT_R16G16_UNORM);
	fmt->set_width(ve::kSectorStride);
	fmt->set_height(ve::kSectorStride);
	fmt->set_array_layers(layers);
	fmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> view;
	view.instantiate();
	array_ = rd->texture_create(fmt, view);
	const std::vector<int32_t> empty = ve::sector_window({0, 0}, {});
	window_ = rd->storage_buffer_create(int64_t(empty.size() * 4), bytes_of(empty.data(), empty.size() * 4));
	Ref<RDSamplerState> ss;
	ss.instantiate();
	ss->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	ss->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_ = rd->sampler_create(ss);
	if (!array_.is_valid() || !window_.is_valid() || !sampler_.is_valid()) {
		teardown();
		return false;
	}
	for (int i = layers - 1; i >= 0; i--) free_layers_.push_back(i);
	return true;
}

void SectorMirror::teardown() {
	if (rd_ != nullptr) {
		if (sampler_.is_valid()) rd_->free_rid(sampler_);
		if (window_.is_valid()) rd_->free_rid(window_);
		if (array_.is_valid()) rd_->free_rid(array_);
	}
	sampler_ = window_ = array_ = RID();
	layer_of_.clear();
	free_layers_.clear();
	synced_version_ = UINT64_MAX;
	synced_centre_ = {INT_MIN, INT_MIN};
	rd_ = nullptr;
}

void SectorMirror::sync(const ve::SectorCache &cache) {
	if (rd_ == nullptr || !array_.is_valid()) return;
	// Version first, then the snapshot: an insert landing between the two leaves the stored
	// version older than the bytes, so the next sync simply runs again.
	const uint64_t version = cache.version();
	const ve::SectorCoord centre = cache.centre();
	if (version == synced_version_ && centre == synced_centre_) return;
	const auto snap = cache.snapshot();
	std::set<ve::SectorCoord> live;
	for (const auto &e : snap) live.insert(e.first);
	for (auto it = layer_of_.begin(); it != layer_of_.end();) {
		if (!live.count(it->first)) {
			free_layers_.push_back(it->second);
			it = layer_of_.erase(it);
		} else {
			++it;
		}
	}
	for (const auto &e : snap) {
		if (layer_of_.count(e.first)) continue;
		// ponytail: capacity is the cache's max_resident(), so this only runs dry if the
		// radius grew after initialize(); the sector then stays absent and its regions held.
		if (free_layers_.empty()) break;
		const int layer = free_layers_.back();
		free_layers_.pop_back();
		rd_->texture_update(array_, layer,
				bytes_of(e.second->texels.data(), e.second->texels.size() * 4));
		layer_of_[e.first] = layer;
	}
	std::vector<ve::SectorLayer> layers;
	layers.reserve(layer_of_.size());
	for (const auto &kv : layer_of_) layers.push_back({kv.first, kv.second});
	const std::vector<int32_t> w = ve::sector_window(centre, layers);
	rd_->buffer_update(window_, 0, int64_t(w.size() * 4), bytes_of(w.data(), w.size() * 4));
	synced_version_ = version;
	synced_centre_ = centre;
}
```

- [ ] **Step 5: Write `SectorContext`**

`extension/src/render/sector_context.h`:
```cpp
#pragma once
// The render device's half of the sector tier (spec §4, plan deviation 1): bakes the
// pipeline's map stage per sector into a ring of storage buffers, reads each back
// asynchronously into the host SectorCache, and keeps the render device's SectorMirror in
// step. Exists only for a pipeline with a map stage.
#include "render/async_readback.h"
#include "render/gpu/gpu.h"
#include "render/sector_mirror.h"
#include "terrain/sector_cache.h"
#include <array>
#include <memory>

namespace godot {

class FieldContextSet;

class SectorContext {
public:
	~SectorContext();
	bool initialize(RenderingDevice *rd, std::shared_ptr<ve::SectorCache> cache);
	void teardown();
	// Once per frame, before anything evaluates the field: harvest landed bakes into the
	// cache, plan around the camera (world space), dispatch this frame's bakes, sync the
	// render mirror. `field` is the render device's set 1, which the bake binds.
	void run_frame(RenderingDevice *rd, float cam_x, float cam_z, const FieldContextSet *field);
	// World-space rectangles. Render: every wanted sector it overlaps is in the render
	// mirror (what brick generation reads). Host: in the cache (what the CPU field and the
	// worker mirror read).
	bool ready_on_render(float min_x, float min_z, float max_x, float max_z) const;
	bool ready_on_host(float min_x, float min_z, float max_x, float max_z) const;
	const SectorMirror &mirror() const { return mirror_; }
	const ve::SectorCache &cache() const { return *cache_; }
	int64_t bakes_dispatched() const { return dispatched_; }
	void set_bakes_per_frame(int n) { bakes_per_frame_ = n; }

private:
	struct Bake {
		RID buffer;
		Ref<AsyncBufferRead> read;
		gpu::SetCache set;
		ve::SectorCoord c;
		bool busy = false;
	};
	static constexpr int kRing = 8;
	static constexpr int64_t kBakeBytes = int64_t(ve::kSectorStride) * ve::kSectorStride * 4;

	RenderingDevice *rd_ = nullptr;
	std::shared_ptr<ve::SectorCache> cache_;
	gpu::Group group_;
	gpu::Program program_;
	RID op_dummy_;
	std::array<Bake, kRing> ring_;
	SectorMirror mirror_;
	int bakes_per_frame_ = 4;
	int64_t dispatched_ = 0;
};

} // namespace godot
```
`extension/src/render/sector_context.cpp`:
```cpp
#include "render/sector_context.h"
#include "gpu_layout/blocks.h"
#include "render/field_context_set.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

SectorContext::~SectorContext() { teardown(); }

bool SectorContext::initialize(RenderingDevice *rd, std::shared_ptr<ve::SectorCache> cache) {
	teardown();
	rd_ = rd;
	cache_ = std::move(cache);
	if (rd == nullptr || !cache_) return false;
	program_ = gpu::compile_compute(rd, group_, "SectorContext", "sector_bake.comp.glsl");
	if (!program_.valid()) return false;
	// field_ops.glslh's op pool, which the bake never reads but the set must cover.
	PackedByteArray zeros;
	zeros.resize(32);
	zeros.fill(0);
	op_dummy_ = rd->storage_buffer_create(zeros.size(), zeros);
	for (Bake &b : ring_) {
		b.buffer = rd->storage_buffer_create(kBakeBytes);
		b.read.instantiate();
		if (!b.buffer.is_valid()) return false;
	}
	return op_dummy_.is_valid() && mirror_.initialize(rd, cache_->max_resident());
}

void SectorContext::teardown() {
	if (rd_ != nullptr) {
		// RenderingDevice has no cancellation: flush each outstanding read while its buffer
		// still exists (AsyncBufferRead::drain's contract).
		for (Bake &b : ring_) {
			if (b.busy && b.read.is_valid()) b.read->drain(rd_);
			if (b.buffer.is_valid()) rd_->free_rid(b.buffer);
			b = Bake();
		}
		if (op_dummy_.is_valid()) rd_->free_rid(op_dummy_);
		gpu::RdDevice d{rd_};
		group_.release(d);
	}
	op_dummy_ = RID();
	program_ = gpu::Program();
	mirror_.teardown();
	rd_ = nullptr;
}

void SectorContext::run_frame(RenderingDevice *rd, float cam_x, float cam_z,
		const FieldContextSet *field) {
	if (rd == nullptr || rd != rd_ || !program_.valid() || !cache_) return;
	for (Bake &b : ring_) {
		if (!b.busy || !b.read->take_fresh()) continue;
		b.busy = false;
		const PackedByteArray &d = b.read->data();
		auto t = std::make_shared<ve::SectorTexels>();
		t->texels.resize(size_t(ve::kSectorStride * ve::kSectorStride));
		if (d.size() < kBakeBytes) {
			cache_->abandon(b.c);
			continue;
		}
		std::memcpy(t->texels.data(), d.ptr(), size_t(kBakeBytes));
		t->max_slope = ve::sector_max_axis_slope(*t);
		if (t->max_slope > ve::kSectorSlopeLimit)
			UtilityFunctions::push_warning(String("sector (") + String::num_int64(b.c.x) + ", " +
					String::num_int64(b.c.z) + ") is steeper than the field bound assumes: " +
					String::num(t->max_slope, 2) + " > " + String::num(ve::kSectorSlopeLimit, 2));
		cache_->insert(b.c, std::move(t));
	}
	int free = 0;
	for (const Bake &b : ring_) free += b.busy ? 0 : 1;
	const ve::SectorCache::Plan plan = cache_->plan(cam_x, cam_z, std::min(free, bakes_per_frame_));
	gpu::RdDevice device{rd};
	for (const ve::SectorCoord &c : plan.bake) {
		Bake *slot = nullptr;
		for (Bake &b : ring_)
			if (!b.busy) { slot = &b; break; }
		const RID set = (slot && field && field->is_valid())
				? slot->set.get(device, group_, program_.shader, 0,
						{gpu::storage(0, slot->buffer), gpu::storage(1, op_dummy_)})
				: RID();
		const ve::SectorBakePush push{{c.x, c.z, 0, 0}};
		const bool ok = set.is_valid() &&
				gpu::dispatch(rd, program_.pipeline, {{set, 0}, {field->uniform_set(), 1}},
						gpu::push_bytes(push), gpu::groups(ve::kSectorStride, 8),
						gpu::groups(ve::kSectorStride, 8)) &&
				slot->read->request(rd, slot->buffer, 0, uint32_t(kBakeBytes));
		if (!ok) {
			cache_->abandon(c);
			continue;
		}
		slot->c = c;
		slot->busy = true;
		dispatched_++;
	}
	mirror_.sync(*cache_);
}

bool SectorContext::ready_on_render(float min_x, float min_z, float max_x, float max_z) const {
	for (const ve::SectorCoord &c : cache_->needed_world(min_x, min_z, max_x, max_z))
		if (!mirror_.has(c)) return false;
	return true;
}

bool SectorContext::ready_on_host(float min_x, float min_z, float max_x, float max_z) const {
	return cache_->ready_world(min_x, min_z, max_x, max_z);
}
```
If `gpu::SetCache::get` or `group_.release` differ in signature from the calls above, read `extension/src/render/gpu/gpu_core.h` and match the call in `RenderOrchestrator::downsample_history` (`downsample_set_.get(device, downsample_group_, downsample_.shader, 0, {...})`).

- [ ] **Step 6: Bind the mirror in set 1**

In `field_context_set.h`, add `class SectorMirror;` and change the declaration:
```cpp
	// `sectors` is the device's SectorMirror for a pipeline with a map stage: its window is
	// binding 1 and its array binding 2. Null for every other pipeline, which gets the
	// one-int "nothing resident" map exactly as before.
	bool initialize(RenderingDevice *rd, RID shader, const ve::ResolvedPipeline &p,
			const SectorMirror *sectors = nullptr);
```
In `field_context_set.cpp`, add `#include "render/sector_mirror.h"`, and replace from the `// Plan A ships an empty sector map` comment through `uset_ = rd->uniform_set_create(...)`:
```cpp
	// Binding 1: the device's sector window, or -- with no map stage -- one int of -1,
	// "nothing resident", owned here.
	RID map;
	if (sectors != nullptr) {
		map = sectors->window();
	} else {
		PackedByteArray empty;
		empty.resize(4);
		empty.fill(0);
		empty.encode_s32(0, -1);
		sector_map_ = rd->storage_buffer_create(empty.size(), empty);
		map = sector_map_;
	}
	if (!map.is_valid()) { teardown(); return false; }

	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u0->set_binding(0);
	u0->add_id(params_ubo_);

	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u1->set_binding(1);
	u1->add_id(map);
	Array uniforms = Array::make(u0, u1);

	// Binding 2: the one sector resource (plan deviation 2), sampled from the mirror's array.
	if (!p.resources.empty()) {
		if (sectors == nullptr || p.resources.size() != 1) { teardown(); return false; }
		Ref<RDUniform> u2;
		u2.instantiate();
		u2->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u2->set_binding(2);
		u2->add_id(sectors->sampler());
		u2->add_id(sectors->array());
		uniforms.push_back(u2);
	}

	uset_ = rd->uniform_set_create(uniforms, shader, 1);
```
`teardown()` already frees `sector_map_` only when valid, which it now is only when owned.

- [ ] **Step 7: Own the cache in the store and create it at pipeline load**

In `core/world_store.h`, add `#include <memory>`, forward-declare `namespace ve { class SectorCache; }` beside the other forward declarations, and add next to `terrain_pipeline()`:
```cpp
	// The host sector cache of a pipeline with a map stage; null otherwise. Shared by the CPU
	// generator, the render device's SectorContext and the mesher's worker mirror.
	const std::shared_ptr<ve::SectorCache> &sector_cache() const { return sector_cache_; }
	void set_sector_cache(std::shared_ptr<ve::SectorCache> c) { sector_cache_ = std::move(c); }
```
and the member `std::shared_ptr<ve::SectorCache> sector_cache_;` beside `terrain_pipeline_`.

In `voxel_world.cpp` `load_terrain_pipeline()`, add `#include "terrain/sector_cache.h"` at the top of the file, and after the `gen == nullptr` check:
```cpp
	// A map stage's sectors live in one host cache that the CPU field samples and every
	// device mirrors (spec §5). The radius is fixed here (plan deviation 14).
	std::shared_ptr<ve::SectorCache> sectors;
	if (resolved.map_stage >= 0) {
		sectors = std::make_shared<ve::SectorCache>(store_->config().stream_radius_m,
				static_cast<float>(resolved.field_offset_x), static_cast<float>(resolved.field_offset_z));
		gen->set_sector_cache(sectors);
	}
```
and after `store_->set_generator(gen);`:
```cpp
	store_->set_sector_cache(std::move(sectors));
```

- [ ] **Step 8: Build the context in the GPU graph and run it every frame**

In `orchestrator.h`, forward-declare `class SectorContext;` and add `SectorContext *sectors = nullptr;` to `RenderPasses` after `field_context`. In `orchestrator.cpp`, `#include "render/sector_context.h"`, and in `ensure_gpu_graph` replace `passes_.field_context = new FieldContextSet();` and its block with:
```cpp
	// The sector tier comes first: set 1 binds its mirror (spec §4.6).
	if (handles_.store->terrain_pipeline().map_stage >= 0) {
		passes_.sectors = new SectorContext();
		if (!passes_.sectors->initialize(device, handles_.store->sector_cache())) {
			UtilityFunctions::printerr("RenderOrchestrator: sector context initialization failed; "
					"this pipeline's terrain cannot stream");
			return GpuInitResult::kFailed;
		}
	}
	passes_.field_context = new FieldContextSet();
	{
		// (keep the existing comment block here unchanged)
		if (!passes_.field_context->initialize(device, passes_.gen->shader(),
				handles_.store->terrain_pipeline(),
				passes_.sectors ? &passes_.sectors->mirror() : nullptr)) {
			UtilityFunctions::printerr(
					"RenderOrchestrator: field context set creation failed; continuing without set 1");
			delete passes_.field_context;
			passes_.field_context = nullptr;
		}
	}
```
In `teardown_render_passes`, directly after the `field_context` delete line:
```cpp
	// After set 1, whose uniform set references the mirror's array and window.
	if (passes_.sectors) { delete passes_.sectors; passes_.sectors = nullptr; }
```
In `frame.cpp`, `#include "render/sector_context.h"`, and directly before `WorldStreamer *st = render_.streamer();`:
```cpp
	// Sectors before anything that evaluates the field (spec §4.3): this frame's bakes, and
	// the render mirror the brick generator is about to read.
	if (SectorContext *sc = render_.passes().sectors)
		sc->run_frame(rd, cam.origin.x, cam.origin.z, render_.passes().field_context);
```

- [ ] **Step 9: Register the fixture's CPU mirror**

In `builtin_stages.cpp`, add `#include "terrain/sector_cache.h"` and, inside `namespace ve` after `stage_flat`:
```cpp
VE_STAGE_SLOTS(SectorFixtureGround, p, sdf, height);
VE_STAGE_PARAMS(SectorFixtureGround, sector_fixture_water_y);

// Mirror of shaders/stages/sector_fixture.field.glslh, the sector tier's test fixture.
void stage_sector_fixture_ground(FieldCtx &ctx, const SectorFixtureGroundSlots &s,
		const SectorFixtureGroundParams &p, const FieldResources &res) {
	const SectorGround g = sector_ground(res.sectors, p.sector_fixture_water_y, ctx.v(s.p)[0], ctx.v(s.p)[2]);
	ctx.f(s.height) = g.height;
	ctx.f(s.sdf) = ctx.v(s.p)[1] - kSurfaceY - g.height;
}
```
and beside the other registrations:
```cpp
VE_REGISTER_STAGE("ve::stage_sector_fixture_ground", SectorFixtureGround, stage_sector_fixture_ground);
```

- [ ] **Step 10: Add the hooks**

In `debug/hooks.h`, declare (public, beside the other `debug_*` methods), plus two private members:
```cpp
	int debug_pump_sectors(Vector3 cam, float half_extent, int max_frames);
	Dictionary debug_sector_stats();
	PackedByteArray debug_sector_texels(int sx, int sz);
	void debug_sector_clear();
	RID debug_field_set(RenderingDevice *rd, RID shader);
	void debug_release_field_set();
...
	// debug_field_set's set 1 on a test's own device: the shipping FieldContextSet and
	// SectorMirror classes, filled from the shipping host cache.
	FieldContextSet *probe_field_set_ = nullptr;
	SectorMirror *probe_mirror_ = nullptr;
```
(forward-declare `class FieldContextSet; class SectorMirror;` if the header does not already). In `hooks.cpp` `_bind_methods`, beside `debug_stream_frame`:
```cpp
	ClassDB::bind_method(D_METHOD("debug_pump_sectors", "cam", "half_extent", "max_frames"), &VoxelDebugHooks::debug_pump_sectors);
	ClassDB::bind_method(D_METHOD("debug_sector_stats"), &VoxelDebugHooks::debug_sector_stats);
	ClassDB::bind_method(D_METHOD("debug_sector_texels", "sx", "sz"), &VoxelDebugHooks::debug_sector_texels);
	ClassDB::bind_method(D_METHOD("debug_sector_clear"), &VoxelDebugHooks::debug_sector_clear);
	ClassDB::bind_method(D_METHOD("debug_field_set", "rd", "shader"), &VoxelDebugHooks::debug_field_set);
	ClassDB::bind_method(D_METHOD("debug_release_field_set"), &VoxelDebugHooks::debug_release_field_set);
```
and in the hooks destructor (or wherever the hooks object releases its resources; add a destructor if there is none) call `debug_release_field_set();`. In `hooks_world.cpp` (add `#include "render/sector_context.h"`, `#include "render/field_context_set.h"`, `#include "terrain/sector_cache.h"`):
```cpp
// Drives the SHIPPING SectorContext on a local-device world until every wanted sector in the
// square around `cam` is in the render mirror. Returns the frames it took, -1 on failure.
int VoxelDebugHooks::debug_pump_sectors(Vector3 cam, float half_extent, int max_frames) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	SectorContext *sc = world_->context().render->passes().sectors;
	if (!world_->is_initialized() || !device || !sc || !world_->get_use_local_device()) return -1;
	for (int f = 1; f <= max_frames; f++) {
		sc->run_frame(device, cam.x, cam.z, world_->context().render->passes().field_context);
		device->submit();
		device->sync();
		if (sc->ready_on_render(cam.x - half_extent, cam.z - half_extent, cam.x + half_extent,
				cam.z + half_extent))
			return f;
	}
	return -1;
}

Dictionary VoxelDebugHooks::debug_sector_stats() {
	Dictionary d;
	const std::shared_ptr<ve::SectorCache> &cache = world_->context().store->sector_cache();
	d["enabled"] = cache != nullptr;
	if (!cache) return d;
	const ve::SectorCache::Stats s = cache->stats();
	d["resident"] = s.resident;
	d["in_flight"] = s.in_flight;
	d["inserted"] = s.inserted;
	d["max_slope"] = s.max_slope;
	d["over_limit"] = s.over_limit;
	d["r_min"] = s.r_min;
	d["r_max"] = s.r_max;
	d["max_resident"] = cache->max_resident();
	const SectorContext *sc = world_->context().render->passes().sectors;
	d["render_layers"] = sc ? sc->mirror().uploaded() : 0;
	d["bakes_dispatched"] = sc ? sc->bakes_dispatched() : int64_t(0);
	return d;
}

PackedByteArray VoxelDebugHooks::debug_sector_texels(int sx, int sz) {
	PackedByteArray out;
	const std::shared_ptr<ve::SectorCache> &cache = world_->context().store->sector_cache();
	if (!cache) return out;
	const auto t = cache->find({sx, sz});
	if (!t) return out;
	out.resize(int64_t(t->texels.size() * 4));
	std::memcpy(out.ptrw(), t->texels.data(), t->texels.size() * 4);
	return out;
}

void VoxelDebugHooks::debug_sector_clear() {
	if (const std::shared_ptr<ve::SectorCache> &cache = world_->context().store->sector_cache())
		cache->clear();
}

RID VoxelDebugHooks::debug_field_set(RenderingDevice *rd, RID shader) {
	debug_release_field_set();
	if (rd == nullptr) return RID();
	const std::shared_ptr<ve::SectorCache> &cache = world_->context().store->sector_cache();
	if (cache) {
		probe_mirror_ = new SectorMirror();
		probe_mirror_->initialize(rd, cache->max_resident());
		probe_mirror_->sync(*cache);
	}
	probe_field_set_ = new FieldContextSet();
	if (!probe_field_set_->initialize(rd, shader, world_->context().store->terrain_pipeline(), probe_mirror_))
		return RID();
	return probe_field_set_->uniform_set();
}

void VoxelDebugHooks::debug_release_field_set() {
	delete probe_field_set_;
	probe_field_set_ = nullptr;
	delete probe_mirror_;
	probe_mirror_ = nullptr;
}
```

- [ ] **Step 11: Write the GPU tests**

`tests/test_sectors.gd`:
```gdscript
extends GdUnitTestSuite

# The sector tier on its fixture pipeline (plan Task 4): bake, read back, mirror. Every
# assertion reads what the shipping SectorContext and SectorCache produced.
const FIXTURE := "res://tests/fixtures/sector_fixture.pipeline"
const SECTOR_BYTES := 260 * 260 * 4

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func _open(seed := 0) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.terrain_pipeline_path = FIXTURE
	w.world_seed = seed
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1000.0
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	return w

func test_a_cold_world_bakes_the_sectors_around_the_camera() -> void:
	var w := _open()
	var frames: int = w.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)
	assert_int(frames).is_greater(0)
	var s: Dictionary = w.hooks().debug_sector_stats()
	assert_bool(s["enabled"]).is_true()
	assert_int(s["resident"]).is_greater(0)
	assert_int(s["render_layers"]).is_equal(s["resident"])
	assert_int(s["over_limit"]).is_equal(0)
	assert_float(s["max_slope"]).is_greater(0.0).is_less(1.0)

func test_the_same_sector_bakes_to_identical_bytes() -> void:
	var w := _open()
	assert_int(w.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	var a: PackedByteArray = w.hooks().debug_sector_texels(0, 0)
	assert_int(a.size()).is_equal(SECTOR_BYTES)
	w.hooks().debug_sector_clear()
	assert_int(w.hooks().debug_sector_texels(0, 0).size()).is_equal(0)
	assert_int(w.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	assert_bool(w.hooks().debug_sector_texels(0, 0) == a).is_true()

# Review Focus 4: the title flow builds one world after another in one process.
func test_a_second_world_never_reads_the_first_worlds_texels() -> void:
	var a := _open(0)
	assert_int(a.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	var bytes_a: PackedByteArray = a.hooks().debug_sector_texels(0, 0)
	a.free()
	# A far-offset seed: Review Focus 1 (sector coordinates thousands of metres out).
	var b := _open(5)
	var off: Vector3 = b.field_offset()
	assert_float(maxf(absf(off.x), absf(off.z))).is_greater(1000.0)
	assert_int(b.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	var home := Vector2i(floori(off.x / 409.6), floori(off.z / 409.6))
	var bytes_b: PackedByteArray = b.hooks().debug_sector_texels(home.x, home.y)
	assert_int(bytes_b.size()).is_equal(SECTOR_BYTES)
	assert_bool(bytes_b == bytes_a).is_false()
	b.free()
	var c := _open(0)
	assert_int(c.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	assert_bool(c.hooks().debug_sector_texels(0, 0) == bytes_a).is_true()
```
Modify `tests/test_field_diff.gd`:
- In `_pipeline_paths()`, after the loop, before `out.sort()`: `out.append("res://tests/fixtures/sector_fixture.pipeline")`.
- In `_open_world`, set `_world.use_local_device = true` before `add_child`, and after `debug_init_atlas()`:
```gdscript
	# A map-stage pipeline samples sectors that exist only once baked; cover every point the
	# suite samples (sample_points reaches 900 m out). A no-op for other pipelines (-1).
	if _world.hooks().debug_sector_stats()["enabled"]:
		assert_int(_world.hooks().debug_pump_sectors(Vector3(400.0, 0.0, 400.0), 600.0, 600)).is_greater(0)
```
- Replace `_make_field_set` with:
```gdscript
func _make_field_set(rd: RenderingDevice, shader: RID) -> RID:
	# The shipping FieldContextSet built on this suite's device, with the shipping
	# SectorMirror filled from the world's host cache for a map-stage pipeline.
	var set: RID = _world.hooks().debug_field_set(rd, shader)
	assert_bool(set.is_valid()).is_true()
	return set
```
- In `run_gpu`, replace `var field_rids := _make_field_set(_rd, shader)` with `var field_set := _make_field_set(_rd, shader)`, bind `field_set` instead of `field_rids[0]`, and replace the `for rid in field_rids: _rd.free_rid(rid)` loop with `_world.hooks().debug_release_field_set()`.
- In `test_every_pipeline_agrees_between_cpu_and_gpu`, after `compare(pts, PackedByteArray(), 0, tag + " base")`, add `compare(surface_points(), PackedByteArray(), 0, tag + " surface")` — the fixture's surface is ±20 m from 51.2, and `sample_points()` alone may not cross every pipeline's surface.

- [ ] **Step 12: Build and run**

Run: `./build.sh && ./build.sh --verify && ./gdunit_tests.sh -a res://tests/test_sectors.gd && ./gdunit_tests.sh -a res://tests/test_field_diff.gd`
Expected: `test_sectors.gd` 3/3 pass; `test_field_diff.gd` passes for every pipeline including `sector_fixture.pipeline`. If `debug_pump_sectors` returns −1, print `debug_sector_stats()` per frame: `bakes_dispatched` rising with `inserted` at 0 means read-backs never arrive on the local device — check that `AsyncBufferRead::request` is called after the dispatch's compute list ends, as `WorldStreamer` does.

- [ ] **Step 13: Check nothing else moved**

Run: `cd extension && scons -Q test 2>&1 | tail -3; git status --short ../shaders/generated ../tests/golden`
Expected: `Status: SUCCESS!`; only `shaders/generated/blocks.glslh` modified. Then run the Task 0 pin suites; compare against the baseline.

- [ ] **Step 14: Commit**

```bash
git add shaders/sector.glslh shaders/sector_bake.comp.glsl shaders/stages/sector_fixture.map.glslh shaders/stages/sector_fixture.field.glslh shaders/generated/blocks.glslh tests/fixtures tests/test_sectors.gd tests/test_sectors.gd.uid tests/test_field_diff.gd extension/src
git commit -m "feat: bake sectors on the render device, read them back and mirror them into set 1"
```

---

### Task 5: The mesher's worker device mirrors sectors

**Files:**
- Modify: `extension/src/render/mesh_service.h`, `mesh_service.cpp`
- Modify: `extension/src/voxel_world.cpp` (`ensure_physics_initialized`)
- Test: `tests/test_sectors.gd` (one case)

**Interfaces:**
- Consumes: `SectorMirror`, `WorldStore::sector_cache()`.
- Produces: `MeshService::set_sector_cache(std::shared_ptr<const ve::SectorCache>)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_sectors.gd`:
```gdscript
# The mesher runs on its own RenderingDevice (plan deviation 1). Its lattice must match the
# CPU field over a fixture surface, which it can only do if its own mirror holds the sector.
func test_the_mesher_reads_the_same_sectors_as_the_cpu() -> void:
	var w := _open()
	assert_int(w.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	var hit: Dictionary = w.raycast(Vector3(30.0, 600.0, 30.0), Vector3.DOWN, 1200.0)
	assert_bool(hit["hit"]).is_true()
	var chunk := Vector3i((hit["pos"] as Vector3 / 6.4).floor())
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(chunk)
	assert_bool(d["has_surface"]).is_true()
	assert_int(d["max_diff"]).is_less_equal(1)
	assert_int(d["diff_over_one"]).is_equal(0)
```

- [ ] **Step 2: Run it to see it fail**

Run: `./gdunit_tests.sh -a res://tests/test_sectors.gd`
Expected: the new case fails (the worker set-1 build refuses a pipeline with a resource and no mirror, so the mesher has no set 1 and its lattice reads nothing — `has_surface` false or a large `max_diff`).

- [ ] **Step 3: Give the worker a mirror**

In `mesh_service.h`: `#include "terrain/sector_cache.h"`, forward-declare `class SectorMirror;`, add
```cpp
	// The host cache the worker's mirror uploads from. Set before start(), like the pipeline.
	void set_sector_cache(std::shared_ptr<const ve::SectorCache> c);
```
and members beside `worker_field_context_`:
```cpp
	std::shared_ptr<const ve::SectorCache> sector_cache_;
	SectorMirror *worker_sectors_ = nullptr;
```
In `mesh_service.cpp`, `#include "render/sector_mirror.h"`, and beside `set_terrain_pipeline`:
```cpp
void MeshService::set_sector_cache(std::shared_ptr<const ve::SectorCache> c) {
	std::lock_guard<std::mutex> lock(mu_);
	sector_cache_ = std::move(c);
}
```
In `run()`, directly before `worker_field_context_ = new FieldContextSet();`:
```cpp
		// A map-stage pipeline's set 1 binds this device's own copy of the sectors; the
		// render device's textures are not visible here (plan deviation 1).
		if (terrain_pipeline_.map_stage >= 0 && sector_cache_) {
			worker_sectors_ = new SectorMirror();
			if (!worker_sectors_->initialize(rd, sector_cache_->max_resident())) {
				UtilityFunctions::printerr("MeshService: worker sector mirror unavailable");
				delete worker_sectors_;
				worker_sectors_ = nullptr;
			}
		}
```
and pass it: `worker_field_context_->initialize(rd, pass.field_shader(), terrain_pipeline_, worker_sectors_)`.

In the job loop, directly after the block that takes this iteration's jobs out of the queues under `mu_` (the `std::unique_lock<std::mutex> lock(mu_); cv_.wait(...)` block ends and the local vectors hold the work), add:
```cpp
		// Before any job evaluates the field: the planners only submit work whose sectors
		// were in the host cache, so syncing now covers everything just taken.
		if (worker_sectors_ && sector_cache_) worker_sectors_->sync(*sector_cache_);
```
At the worker's teardown (the block ending `delete worker_field_context_; worker_field_context_ = nullptr; pass.teardown(); memdelete(rd);`), after the `worker_field_context_` delete:
```cpp
	delete worker_sectors_;
	worker_sectors_ = nullptr;
```
Do the same at the other `delete worker_field_context_;` site (the init-failure path).

In `voxel_world.cpp` `ensure_physics_initialized()`, after `mesh_->set_terrain_pipeline(store_->terrain_pipeline());`:
```cpp
	mesh_->set_sector_cache(store_->sector_cache());
```

- [ ] **Step 4: Run the tests**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_sectors.gd && ./gdunit_tests.sh -a res://tests/test_mesh_lattice.gd`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/mesh_service.h extension/src/render/mesh_service.cpp extension/src/voxel_world.cpp tests/test_sectors.gd
git commit -m "feat: the mesher's worker device mirrors sectors from the host cache"
```

---

### Task 6: Nothing streams over a sector that is not resident

**Files:**
- Modify: `extension/src/world/residency.h`, `residency.cpp`; `extension/tests/test_residency.cpp`
- Modify: `extension/src/render/world_streamer.h`, `world_streamer.cpp`, `orchestrator.cpp`
- Modify: `extension/src/lod/lod_system.h`, `lod_system.cpp`; `extension/src/debug/hooks_lod.cpp`
- Modify: `extension/src/physics/collider_streamer.h`, `collider_streamer.cpp`; `extension/src/voxel_world.cpp`; `extension/src/debug/hooks_physics.cpp`
- Test: `tests/test_sector_gating.gd`

**Interfaces:**
- Consumes: `SectorContext::ready_on_render/ready_on_host`, `SectorCache::ready_world`.
- Produces: `using ve::RegionGate = std::function<bool(const IVec3 &)>`; `RegionResidency::update(..., const RegionGate &gate = {})`; `WorldStreamer::set_region_gate(ve::RegionGate)`; `LodStats::sector_held`; `ColliderStreamer::set_sector_gate(std::function<bool(float, float, float, float)>)`, `ColliderStreamer::sector_holds()`; hook keys `debug_lod_stats()["sector_held"]`, `debug_physics_stats()["sector_holds"]`.

- [ ] **Step 1: Write the failing native test**

Append to `extension/tests/test_residency.cpp`:
```cpp
TEST_CASE("a region gate holds refused regions and loads the rest nearest first") {
	ve::RegionResidency res(make_cfg(64.0f, 512, 64));
	const ve::RegionGate gate = [](const ve::IVec3 &r) { return r.x >= 0; };
	const ve::ResidencyPlan p = res.update(1.0f, 1.0f, 1.0f, ve::AtlasBudget{}, -1, gate);
	REQUIRE_FALSE(p.loads.empty());
	for (const auto &l : p.loads) CHECK(l.region.x >= 0);
	// Held, not lost: once the gate opens, the refused regions load.
	const ve::ResidencyPlan q = res.update(1.0f, 1.0f, 1.0f);
	bool negative = false;
	for (const auto &l : q.loads) negative |= l.region.x < 0;
	CHECK(negative);
}
```
Run: `cd extension && scons -Q test 2>&1 | tail -3` — Expected: compile error (`RegionGate`).

- [ ] **Step 2: Gate residency**

In `residency.h`, add `#include <functional>` and before `class RegionResidency`:
```cpp
// May this region load now? A sector-tier world refuses regions whose sectors have not
// arrived, so no brick is ever generated from fallback height (spec §5.3). A refused region
// stays a candidate and loads on a later frame.
using RegionGate = std::function<bool(const IVec3 &)>;
```
Change the declaration to `ResidencyPlan update(float cx, float cy, float cz, const AtlasBudget &budget = AtlasBudget{}, int max_loads = -1, const RegionGate &gate = {});` and the definition's signature to match. In the load loop in `residency.cpp`, directly after `if (static_cast<int>(plan.loads.size()) >= load_cap) break;`:
```cpp
		if (gate && !gate(c.region)) continue;
```
Run: `cd extension && scons -Q test 2>&1 | tail -3` — Expected: `Status: SUCCESS!`.

- [ ] **Step 3: Wire the region gate**

In `world_streamer.h`, `#include "world/residency.h"` if not present, add `void set_region_gate(ve::RegionGate g) { region_gate_ = std::move(g); }` and the member `ve::RegionGate region_gate_;`. In `world_streamer.cpp`, change `residency_->update(cx, cy, cz, budget)` to `residency_->update(cx, cy, cz, budget, -1, region_gate_)`. In `orchestrator.cpp` after `streamer_->initialize(...)`:
```cpp
	if (SectorContext *sc = passes_.sectors) {
		// A metre of margin: brick generation samples a voxel past the region's faces.
		streamer_->set_region_gate([sc](const ve::IVec3 &r) {
			const float x0 = r.x * ve::kRegionSize, z0 = r.z * ve::kRegionSize;
			return sc->ready_on_render(x0 - 1.0f, z0 - 1.0f, x0 + ve::kRegionSize + 1.0f,
					z0 + ve::kRegionSize + 1.0f);
		});
	}
```

- [ ] **Step 4: Gate LoD requests**

In `lod_system.h`, add `int sector_held = 0;` to `LodStats` and a member `int lod_sector_held_ = 0;` to `LodSystem`. In `lod_system.cpp`, `#include "render/sector_context.h"`, and add in the file's anonymous namespace (create one near the top if absent):
```cpp
// The chunk's lattice reaches two cells past its edge on each side.
bool lod_chunk_sectors_ready(const godot::SectorContext &s, int level, ve::IVec3 c) {
	float o[3];
	ve::lod_chunk_origin(level, c, o);
	const float cell = ve::lod_cell_size(level);
	const float pad = 2.0f * cell, size = float(ve::kLodChunkCells) * cell;
	return s.ready_on_host(o[0] - pad, o[2] - pad, o[0] + size + pad, o[2] + size + pad);
}
```
In `tick`, replace the shell-request and batch-request selection inside `if (mesh() && !mesh()->lod_busy()) {` with:
```cpp
		const int cap = std::min<int>(lod_builds_per_frame_, mesh()->lod_max_jobs());
		const SectorContext *sectors = render()->passes().sectors;
		int held = 0;
		if (render()->transparency_settings().enabled) {
			std::vector<ve::IVec3> wanted;
			shell_grid_.requests(cam.pos, cap, &wanted);
			for (ve::IVec3 c : wanted) {
				if (sectors && !lod_chunk_sectors_ready(*sectors, ve::kShellLevel, c)) { held++; continue; }
				shell_grid_.note_building(c);
				shell_requests.push_back(c);
			}
		}
		for (const ve::LodBuildRequest &q : lod_walk_.requests) {
			if (int(batch_requests.size() + shell_requests.size()) >= cap) break;
			if (sectors && !lod_chunk_sectors_ready(*sectors, q.level, q.coord)) { held++; continue; }
			batch_requests.push_back(q);
		}
		lod_sector_held_ = held;
		for (const ve::LodBuildRequest &q : batch_requests)
			lod_tree_->note_building(q.level, q.coord);
```
Keep the existing comments above each part. In the function that fills `LodStats` (`LodSystem::stats()`), set `s.sector_held = lod_sector_held_;`. In `hooks_lod.cpp` `debug_lod_stats`, add `d["sector_held"] = s.sector_held;`.

- [ ] **Step 5: Gate collider planning**

In `collider_streamer.h`, `#include <functional>`, and add:
```cpp
	// A sector-tier world holds the whole plan while any sector under the physics balls is
	// not resident: ChunkResidency caches a probed chunk's "empty" for good, and a probe of
	// an unbaked sector reads fallback air (plan deviation 9). World-space rectangle.
	void set_sector_gate(std::function<bool(float, float, float, float)> g) { sector_gate_ = std::move(g); }
	int sector_holds() const { return sector_holds_; }
...
	std::function<bool(float, float, float, float)> sector_gate_;
	int sector_holds_ = 0;
```
In `collider_streamer.cpp` `run_frame`, replace the `chunks_->update(...)` call with:
```cpp
	bool held = false;
	if (sector_gate_) {
		float lo_x = centers[0], lo_z = centers[2], hi_x = centers[0], hi_z = centers[2];
		for (size_t i = 0; i < radii.size(); i++) {
			lo_x = std::min(lo_x, centers[3 * i] - radii[i]);
			lo_z = std::min(lo_z, centers[3 * i + 2] - radii[i]);
			hi_x = std::max(hi_x, centers[3 * i] + radii[i]);
			hi_z = std::max(hi_z, centers[3 * i + 2] + radii[i]);
		}
		held = !sector_gate_(lo_x, lo_z, hi_x, hi_z);
		if (held) sector_holds_++;
	}
	const ve::ChunkPlan plan = held ? ve::ChunkPlan{} : chunks_->update(centers.data(), radii.data(),
			static_cast<int>(centers.size() / 3), field_, build_cap);
```
In `voxel_world.cpp` `ensure_physics_initialized()`, after `colliders_->set_body_bubble_radius_m(...)`:
```cpp
	if (std::shared_ptr<ve::SectorCache> cache = store_->sector_cache())
		colliders_->set_sector_gate([cache](float a, float b, float c, float d) {
			return cache->ready_world(a, b, c, d);
		});
```
In `hooks_physics.cpp` `debug_physics_stats`, add `d["sector_holds"] = world_->colliders() ? world_->colliders()->sector_holds() : 0;`.

- [ ] **Step 6: Write the GPU gating tests**

`tests/test_sector_gating.gd`:
```gdscript
extends GdUnitTestSuite

# Spec §5.3: nothing that bakes field data into a store runs over a sector that is not
# resident. Fixture pipeline; every assertion reads the shipping streamer, LoD system and
# collider streamer.
const FIXTURE := "res://tests/fixtures/sector_fixture.pipeline"
const CAM := Vector3(10.0, 60.0, 10.0)

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func _open() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.terrain_pipeline_path = FIXTURE
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1000.0
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	return w

func _stream(w: VoxelWorld, cam: Vector3, frames: int) -> void:
	for i in range(frames):
		w.hooks().debug_stream_frame(cam)

func test_no_region_streams_over_an_unbaked_sector() -> void:
	var w := _open()
	_stream(w, CAM, 10)
	assert_int(w.hooks().debug_stream_stats()["resident_regions"]).is_equal(0)
	assert_int(w.hooks().debug_pump_sectors(CAM, 200.0, 300)).is_greater(0)
	_stream(w, CAM, 10)
	assert_int(w.hooks().debug_stream_stats()["resident_regions"]).is_greater(0)

# Review Focus 2.
func test_a_teleport_holds_regions_until_their_sectors_bake() -> void:
	var w := _open()
	assert_int(w.hooks().debug_pump_sectors(CAM, 200.0, 300)).is_greater(0)
	_stream(w, CAM, 10)
	var far := Vector3(3000.0, 60.0, 3000.0)
	var far_region := Vector3i((far / 25.6).floor())
	# The cache is still centred on the old camera: nothing at the new one is wanted yet.
	_stream(w, far, 10)
	assert_int(w.hooks().debug_slot_of_region(far_region)).is_equal(-1)
	assert_int(w.hooks().debug_pump_sectors(far, 200.0, 300)).is_greater(0)
	_stream(w, far, 20)
	assert_int(w.hooks().debug_slot_of_region(far_region)).is_greater_equal(0)

func test_lod_requests_wait_for_their_sectors() -> void:
	var w := _open()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_lod_tick(CAM, Vector3(1.0, -0.2, 0.0))
	var s: Dictionary = w.hooks().debug_lod_stats()
	assert_int(s["sector_held"]).is_greater(0)
	assert_int(s["builds_in_flight"]).is_equal(0)
	assert_int(w.hooks().debug_pump_sectors(CAM, 1000.0, 600)).is_greater(0)
	w.hooks().debug_lod_tick(CAM, Vector3(1.0, -0.2, 0.0))
	assert_int(w.hooks().debug_lod_stats()["sector_held"]).is_less(s["sector_held"])

func test_colliders_wait_for_their_sectors() -> void:
	var w := _open()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	for i in range(5):
		w.hooks().debug_physics_frame(CAM)
	var s: Dictionary = w.hooks().debug_physics_stats()
	assert_int(s["sector_holds"]).is_greater(0)
	assert_int(s["probe_cache"]).is_equal(0) # nothing probed fallback air
	assert_int(w.hooks().debug_pump_sectors(CAM, 200.0, 300)).is_greater(0)
	for i in range(30):
		w.hooks().debug_physics_frame(CAM)
	assert_int(w.hooks().debug_physics_stats()["chunks_resident"]).is_greater(0)
```
`debug_slot_of_region` takes a `Vector3i`; check its binding in `hooks.cpp` and adapt the argument if it differs.

- [ ] **Step 7: Run**

Run: `./build.sh && ./build.sh --verify && cd extension && scons -Q test 2>&1 | tail -3; cd .. && ./gdunit_tests.sh -a res://tests/test_sector_gating.gd`
Expected: native SUCCESS; 4/4 GPU cases pass. Then the Task 0 pin suites: no new failures against the baseline (Default has no `SectorContext`, so every gate is absent there).

- [ ] **Step 8: Commit**

```bash
git add extension/src extension/tests/test_residency.cpp tests/test_sector_gating.gd tests/test_sector_gating.gd.uid
git commit -m "feat: regions, LoD chunks and colliders wait for their sectors"
```

---

### Task 7: Snow material

**Files:**
- Create: `assets/materials/09_basecolor.png`, `09_normal.png`, `09_roughness.png`, `09_ambientOcclusion.png`, `09_height.png`
- Modify: `extension/src/world/material_table.h`, `shaders/material_table.glslh`, `tools/convert_materials.sh`

**Interfaces:**
- Produces: material `snow`, id 10, GLSL `MAT_SNOW = 10u`, C++ `ve::material_id("snow")`.

- [ ] **Step 1: Add the row and see the generated-table test fail**

In `material_table.h`, append after the `water` row:
```cpp
	// Snow (docs/superpowers/specs/2026-10-07-fjords-terrain-design.md §7.4), banded onto
	// high, not-too-steep ground by fjord_bands. Soft. The brightest albedo in the game: if
	// sunlit snow blooms, tune flat_albedo and the texture, never the beauty stack.
	{"snow",         "09",  1.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.86f, 0.88f, 0.92f}},
```
In `tools/convert_materials.sh`, change the list to `MATERIALS=(grass_01 rock ground_01 breakstone ground_crack_01 ice_crack ice bark water snow)`.

Run: `cd extension && scons -Q test 2>&1 | grep -A3 "material_table" | head -20`
Expected: the `material_table.glslh` byte-equality test fails and prints the correct file text.

- [ ] **Step 2: Regenerate `shaders/material_table.glslh`**

The failing test (`the committed GLSL mirror matches the C++ table`) prints `shaders/material_table.glslh is stale. Replace its entire contents with:` followed by the file. Replace the file's entire contents with exactly that text (`cd extension && ./build/tests/ve_tests -tc="the committed GLSL mirror matches the C++ table" 2>&1` shows it). Then:
Run: `git diff --stat shaders/material_table.glslh && grep -n "MAT_SNOW\|MATERIAL_COUNT" shaders/material_table.glslh`
Expected: `const int MATERIAL_COUNT = 10;`, `const uint MAT_SNOW = 10u;`.

- [ ] **Step 3: Convert the textures**

Run:
```bash
SRC=~/Development/unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2/snow
for map in basecolor normal roughness ambientOcclusion height; do
	convert "$SRC/T_snow_${map}.tga" -resize 512x512! -strip "PNG24:assets/materials/09_${map}.png"
done
file assets/materials/09_*.png
```
Expected: five `PNG image data, 512 x 512, 8-bit/color RGB` lines. (One-off: `convert_materials.sh` aborts at its bark entry on this machine.)

- [ ] **Step 4: Run the tests**

Run: `cd extension && scons -Q test 2>&1 | tail -3; cd .. && ./build.sh && ./gdunit_tests.sh -a res://tests/test_material_picker.gd`
Expected: native SUCCESS (including the converter-list test); the picker suite passes (it compares against `material_table().size()`). Then the Task 0 pin suites — `test_frame_shipped_golden.gd` must not move: no shipped pipeline places snow yet.

- [ ] **Step 5: Commit**

```bash
git add assets/materials/09_*.png extension/src/world/material_table.h shaders/material_table.glslh tools/convert_materials.sh
git commit -m "feat: a snow material from the vol2 pack"
```

---

### Task 8: The Fjords pipeline and world type

**Files:**
- Create: `shaders/noise2d.glslh`, `shaders/erosion.glslh`
- Create: `shaders/stages/fjord_height.map.glslh`, `fjord.field.glslh`, `fjord_bands.field.glslh`
- Create: `assets/pipelines/fjords.pipeline`, `demo/world_types/30_fjords.tres`
- Modify: `extension/src/terrain/builtin_stages.cpp`
- Modify: `tests/test_create_world.gd`
- Test: `tests/test_fjords.gd`; `tests/test_field_diff.gd` picks the pipeline up unchanged

**Interfaces:**
- Consumes: `sector.glslh`, `sector_ground` (both sides), `MAT_SNOW`, `MAT_BREAKSTONE`, `MAT_GRASS_01`, `MAT_GROUND_01`.
- Produces: GLSL `vec4 fjord_ground(vec2 xz)` (height above `SURFACE_Y`, dh/dx, dh/dz, ridge) for sub-projects B and C; channels `slope`, `ridge`; params `fjord_height.*`, `fjord_bands.*`; world type "Fjords".

- [ ] **Step 1: Write the failing tests**

`tests/test_fjords.gd`:
```gdscript
extends GdUnitTestSuite

# The Fjords world (spec §6–§7). Reads the shipping bake's statistics and the CPU field.
const FJORDS := "res://assets/pipelines/fjords.pipeline"
const WATER_Y := 51.2

var _world: VoxelWorld

func after_test() -> void:
	if is_instance_valid(_world):
		_world.free()

func _open(seed: int, radius: float) -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = FJORDS
	_world.world_seed = seed
	_world.use_local_device = true
	_world.physics_enabled = false
	_world.stream_radius_m = radius
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()

# Spec §6.4 and Review Focus 5: over +-3 km at three seeds no sector is steeper than the
# field's declared bound assumes, and no texel clamps at either end of the encoded range.
func test_no_sector_breaks_the_slope_limit_or_the_encoded_range() -> void:
	for seed in [0, 7, 424242]:
		_open(seed, 4300.0)
		assert_int(_world.hooks().debug_pump_sectors(Vector3.ZERO, 3000.0, 900)).is_greater(0)
		var s: Dictionary = _world.hooks().debug_sector_stats()
		assert_int(s["over_limit"]).override_failure_message(
			"seed %d: %d sectors over the slope limit, worst %.2f" % [seed, s["over_limit"], s["max_slope"]]
			).is_equal(0)
		assert_float(s["r_min"]).override_failure_message("seed %d floor clamps" % seed).is_greater(0.01)
		assert_float(s["r_max"]).override_failure_message("seed %d peaks clamp" % seed).is_less(0.98)
		_world.free()

# The bands produce all four materials somewhere in a few kilometres (spec §7.2).
func test_the_bands_place_snow_rock_grass_and_shore() -> void:
	_open(0, 2600.0)
	assert_int(_world.hooks().debug_pump_sectors(Vector3.ZERO, 2000.0, 600)).is_greater(0)
	var seen := {}
	for i in range(-20, 21):
		for j in range(-20, 21):
			var hit: Dictionary = _world.raycast(Vector3(i * 100.0, 600.0, j * 100.0), Vector3.DOWN, 1200.0)
			if hit["hit"]:
				seen[int(hit["material"])] = true
	var names: Array = _world.material_table().map(func(m): return m["name"])
	for want in ["snow", "breakstone", "grass_01", "ground_01"]:
		assert_bool(seen.has(names.find(want) + 1)).override_failure_message(
			"no %s in a 4 km grid; seen ids %s" % [want, seen.keys()]).is_true()
```
`material_table()` returns an array of dictionaries; check its key for the name in `voxel_world.cpp` (`material_table` binding) and adjust `m["name"]` if it differs.

In `tests/test_create_world.gd`, change the expected list on line 37 to `["Default", "Mesas", "Flat", "Fjords"]`, and the cycle test (lines 79–85) to expect `Default → Mesas → Flat → Fjords → Default`.

Run: `./gdunit_tests.sh -a res://tests/test_fjords.gd`
Expected: fails — the pipeline file does not exist.

- [ ] **Step 2: Write `shaders/noise2d.glslh`**

```glsl
#ifndef VE_NOISE2D_GLSLH
#define VE_NOISE2D_GLSLH
// The helpers the erosion filter's Shadertoy keeps in its Common tab, which was not part of
// the source we have (spec §6.1): our own implementations. Prefixed, because the generated
// field.glslh puts every stage body into every field-consuming shader.

float ve_clamp01(float x) { return clamp(x, 0.0, 1.0); }

uvec2 ve_pcg2d(uvec2 v) {
	v = v * 1664525u + 1013904223u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v = v ^ (v >> 16u);
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v = v ^ (v >> 16u);
	return v;
}

// A vector in [-1, 1]^2 per integer lattice point. Integer hashing keeps cells stable at the
// +-8 km seed offsets, where a sin-based hash loses its low bits.
vec2 ve_hash2(vec2 p) {
	uvec2 h = ve_pcg2d(uvec2(ivec2(floor(p))));
	return vec2(h & 0xffffu) / 32767.5 - 1.0;
}

// Gradient noise with analytic derivatives (iq): x = value, yz = d/dp.
vec3 ve_noised(vec2 p) {
	vec2 i = floor(p);
	vec2 f = p - i;
	vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
	vec2 ga = ve_hash2(i);
	vec2 gb = ve_hash2(i + vec2(1.0, 0.0));
	vec2 gc = ve_hash2(i + vec2(0.0, 1.0));
	vec2 gd = ve_hash2(i + vec2(1.0, 1.0));
	float va = dot(ga, f);
	float vb = dot(gb, f - vec2(1.0, 0.0));
	float vc = dot(gc, f - vec2(0.0, 1.0));
	float vd = dot(gd, f - vec2(1.0, 1.0));
	return vec3(va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd),
			ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
					du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va));
}

#endif
```

- [ ] **Step 3: Write `shaders/erosion.glslh` (the MPL-2.0 port)**

The source is the Shadertoy buffer the user supplied for wXcfWn. This file is that buffer's `PhacelleNoise` and `ErosionFilter` sections, unchanged except for the renames listed in its first comment. Both MPL notices are the author's, verbatim. Copy this exactly:

```glsl
#ifndef VE_EROSION_GLSLH
#define VE_EROSION_GLSLH
// Ported from Rune Skovbo Johansen's "Advanced terrain erosion filter" (Shadertoy wXcfWn;
// https://www.youtube.com/watch?v=gsJHzBTPG0Y,
// https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html). This file, and
// only this file in the repository, is under the Mozilla Public License 2.0 notices below.
// Changes from the original: identifiers prefixed (hash -> ve_hash2, clamp01 -> ve_clamp01,
// TAU -> VE_EROSION_TAU, pow_inv / ease_out / smooth_start / safe_normalize -> erosion_*),
// because the generated field.glslh puts every stage body into every field-consuming shader;
// the demonstration section (Heightmap, GetTreesAmount, mainImage) is not ported.
// Requires shaders/noise2d.glslh to be included first.

#define VE_EROSION_TAU 6.28318530717959

// The Simple Phacelle Noise function produces a stripe pattern aligned with the input vector.
// The name Phacelle is a portmanteau of phase and cell, since the function produces a phase by
// interpolating cosine and sine waves from multiple cells.
//  - p is the input point being evaluated.
//  - normDir is the direction of the stripes at this point. It must be a normalized vector.
//  - freq is the freqency of the stripes within each cell. It's best to keep it close to 1.0, as
//    high values will produce distortions and other artifacts.
//  - offset is the phase offset of the stripes, where 1.0 is a full cycle.
//  - normalization is the degree of normalization applied, between 0 and 1. With e.g. a value of
//    0.4, raw output with a magnitude below 0.6 won't get fully normalized to a magnitude of 1.0.
// Phacelle Noise function copyright (c) 2025 Rune Skovbo Johansen
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
vec4 PhacelleNoise(in vec2 p, vec2 normDir, float freq, float offset, float normalization) {
	// Get a vector orthogonal to the input direction, with a
	// magnitude proportional to the frequency of the stripes.
	vec2 sideDir = normDir.yx * vec2(-1.0, 1.0) * freq * VE_EROSION_TAU;
	offset *= VE_EROSION_TAU;

	// Iterate over 4x4 cells, calculating a stripe pattern for each and blending between them.
	// pInt is the integer part of the current coordinate p, pFrac is the remainder.
	vec2 pInt = floor(p);
	vec2 pFrac = fract(p);
	vec2 phaseDir = vec2(0.0);
	float weightSum = 0.0;
	for (int i = -1; i <= 2; i++) {
		for (int j = -1; j <= 2; j++) {
			vec2 gridOffset = vec2(i, j);

			// Calculate a cell point by starting off with a point in the integer grid.
			vec2 gridPoint = pInt + gridOffset;

			// Calculate a random offset for the cell point between -0.5 and 0.5 on each axis.
			vec2 randomOffset = ve_hash2(gridPoint) * 0.5;

			// The final cell point (we don't store it) is the gridPoint plus the randomOffset.
			// Calculate a vector representing the input point relative to this cell point:
			// p - (gridPoint + randomOffset)
			// = (pFrac + pInt) - ((pInt + gridOffset) + randomOffset)
			// = pFrac + pInt - pInt - gridOffset - randomOffset
			// = pFrac - gridOffset - randomOffset
			vec2 vectorFromCellPoint = pFrac - gridOffset - randomOffset;

			// Bell-shaped weight function which is 1 at dist 0 and nearly 0 at dist 1.5.
			// Due to the random offsets of up to 0.5, the closest a cell point not in the 4x4
			// grid can be to the current point p is 1.5 units away.
			float sqrDist = dot(vectorFromCellPoint, vectorFromCellPoint);
			float weight = exp(-sqrDist * 2.0);
			// Subtract 0.01111 to make the function actually 0 at distance 1.5, which avoids
			// some (very subtle) grid line artefacts.
			weight = max(0.0, weight - 0.01111);

			// Keep track of the total sum of weights.
			weightSum += weight;

			// The waveInput is a gradient which increases in value along sideDir. Its rate of
			// change is the freq times tau, due to the multiplier pre-applied to sideDir.
			float waveInput = dot(vectorFromCellPoint, sideDir) + offset;

			// Add this cell's cosine and sine wave contributions to the interpolated value.
			phaseDir += vec2(cos(waveInput), sin(waveInput)) * weight;
		}
	}

	// Get the raw interpolated value.
	vec2 interpolated = phaseDir / weightSum;
	// Interpret the value as a vector whose length represents the magnitude of both waves.
	float magnitude = sqrt(dot(interpolated, interpolated));
	// Apply a lower threshold to show small magnitudes we're going to fully normalize.
	magnitude = max(1.0 - normalization, magnitude);
	// Return a vector containing the normalized cosine and sine waves, as well as the direction
	// vector, which can be multiplied onto the sine to get the derivatives of the cosine.
	return vec4(interpolated / magnitude, sideDir);
}

float erosion_pow_inv(float t, float power) {
	// Flip, raise to the specified power, and flip back.
	return 1.0 - pow(1.0 - ve_clamp01(t), power);
}

float erosion_ease_out(float t) {
	// Flip by subtracting from one.
	float v = 1.0 - ve_clamp01(t);
	// Raise to a power of two and flip back.
	return 1.0 - v * v;
}

float erosion_smooth_start(float t, float smoothing) {
	if (t >= smoothing)
		return t - 0.5 * smoothing;
	return 0.5 * t * t / smoothing;
}

vec2 erosion_safe_normalize(vec2 n) {
	// A div-by-zero-safe replacement for normalize.
	float l = length(n);
	return (abs(l) > 1e-10) ? (n / l) : n;
}

// Advanced Terrain Erosion Filter copyright (c) 2025 Rune Skovbo Johansen
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
vec4 ErosionFilter(
		// Input parameters that vary per pixel.
		in vec2 p, vec3 heightAndSlope, float fadeTarget,
		// Stylistic parameters that may vary per pixel.
		float strength, float gullyWeight, float detail, vec4 rounding, vec4 onset, vec2 assumedSlope,
		// Scale related parameters that do not support variation per pixel.
		float scale, int octaves, float lacunarity,
		// Other parameters.
		float gain, float cellScale, float normalization,
		// Output parameters.
		out float ridgeMap, out float debug) {
	strength *= scale;
	fadeTarget = clamp(fadeTarget, -1.0, 1.0);

	vec3 inputHeightAndSlope = heightAndSlope;
	float freq = 1.0 / (scale * cellScale);
	float slopeLength = max(length(heightAndSlope.yz), 1e-10);
	float magnitude = 0.0;
	float roundingMult = 1.0;

	float roundingForInput = mix(rounding.y, rounding.x, ve_clamp01(fadeTarget + 0.5)) * rounding.z;
	// The combined accumulating mask, based first on initial slope, and later on slope of each octave too.
	float combiMask = erosion_ease_out(erosion_smooth_start(slopeLength * onset.x, roundingForInput * onset.x));

	// Initialize the ridgeMap fadeTarget and mask.
	float ridgeMapCombiMask = erosion_ease_out(slopeLength * onset.z);
	float ridgeMapFadeTarget = fadeTarget;

	// Deteriming the strength of the initial slope used for gully directions
	// based on the specified mix of the actual slope and an assumed slope.
	vec2 gullySlope = mix(heightAndSlope.yz, heightAndSlope.yz / slopeLength * assumedSlope.x, assumedSlope.y);

	for (int i = 0; i < octaves; i++) {
		// Calculate and add gullies to the height and slope.
		vec4 phacelle = PhacelleNoise(p * freq, erosion_safe_normalize(gullySlope), cellScale, 0.25, normalization);
		// Multiply with freq since p was multiplied with freq.
		// Negate since we use slope directions that point down.
		phacelle.zw *= -freq;
		// Amount of slope as value from 0 to 1.
		float sloping = abs(phacelle.y);

		// Add non-masked, normalized slope to gullySlope, for use by subsequent octaves.
		// It's normalized to use the steepest part of the sine wave everywhere.
		gullySlope += sign(phacelle.y) * phacelle.zw * strength * gullyWeight;

		// Handle height offset and approximate output slope.

		// Gullies has height offset (from -1 to 1) in x and derivative in yz.
		vec3 gullies = vec3(phacelle.x, phacelle.y * phacelle.zw);
		// Fade gullies towards fadeTarget based on combiMask.
		vec3 fadedGullies = mix(vec3(fadeTarget, 0.0, 0.0), gullies * gullyWeight, combiMask);
		// Apply height offset and derivative (slope) according to strength of current octave.
		heightAndSlope += fadedGullies * strength;
		magnitude += strength;

		// Update fadeTarget to include the new octave.
		fadeTarget = fadedGullies.x;

		// Update the mask to include the new octave.
		float roundingForOctave = mix(rounding.y, rounding.x, ve_clamp01(phacelle.x + 0.5)) * roundingMult;
		float newMask = erosion_ease_out(erosion_smooth_start(sloping * onset.y, roundingForOctave * onset.y));
		combiMask = erosion_pow_inv(combiMask, detail) * newMask;

		// Update the ridgeMap fadeTarget and mask.
		ridgeMapFadeTarget = mix(ridgeMapFadeTarget, gullies.x, ridgeMapCombiMask);
		float newRidgeMapMask = erosion_ease_out(sloping * onset.w);
		ridgeMapCombiMask = ridgeMapCombiMask * newRidgeMapMask;

		// Prepare the next octave.
		strength *= gain;
		freq *= lacunarity;
		roundingMult *= rounding.w;
	}

	ridgeMap = ridgeMapFadeTarget * (1.0 - ridgeMapCombiMask);
	debug = fadeTarget;

	vec3 heightAndSlopeDelta = heightAndSlope - inputHeightAndSlope;
	return vec4(heightAndSlopeDelta, magnitude);
}

#endif
```

Check: `grep -c "Mozilla Public" shaders/erosion.glslh` prints `2`.

- [ ] **Step 4: Write the map stage**

`shaders/stages/fjord_height.map.glslh`:
```glsl
//!stage     fjord_height
//!kind      map
//!domain    sector2d 256x256
//!out       sector.terrain : image2d_rg16
//!param     water_y : float = 51.2
//!param     scale_l : float = 2000.0
//!param     scale_v : float = 2000.0
//!param     h_water : float = 0.43
//!param     height_freq : float = 3.0
//!param     height_amp : float = 0.125
//!param     valley_freq : float = 0.67
//!param     valley_warp : float = 0.6
//!param     valley_width : float = 0.15
//!param     valley_depth : float = 0.12
//!param     erosion_scale : float = 0.15
//!param     erosion_strength : float = 0.22
//!param     gully_weight : float = 0.5
//!param     detail : float = 1.5
//!param     ridge_rounding : float = 0.1
//!param     crease_rounding : float = 0.0
//!param     cell_scale : float = 0.7
//!param     normalization : float = 0.5
//!param     height_offset : float = -0.65
//!param     floor_depth : float = 40.0
//!param     wall_height : float = 60.0
//!param     wall_steepen : float = 0.5

// The Fjords heightmap (spec §6), baked once per sector. Shader space is the Shadertoy's
// normalised map, p = xz / scale_l; the eroded height becomes water_y + scale_v * (h - h_water)
// metres, shaped by the fjord profile. scale_l == scale_v keeps the filter's slope semantics.
#include "noise2d.glslh"
#include "erosion.glslh"
#include "sector.glslh"

// The Shadertoy's FractalNoise: 3 octaves, lacunarity 2, gain 0.1, with derivatives.
vec3 fjord_fbm(vec2 p) {
	vec3 n = vec3(0.0);
	float nf = P.fjord_height_height_freq, na = 1.0;
	for (int i = 0; i < 3; i++) {
		n += ve_noised(p * nf) * na * vec3(1.0, nf, nf);
		na *= 0.1;
		nf *= 2.0;
	}
	return n * P.fjord_height_height_amp;
}

// Long winding valleys along the zero set of a domain-warped noise (spec §6.3 step 2).
// valley_width is in noise units (plan deviation 7).
float fjord_carve(vec2 p) {
	vec2 q = p * P.fjord_height_valley_freq;
	vec2 w = vec2(ve_noised(q * 1.7).x, ve_noised(q * 1.7 + vec2(5.2, 1.3)).x);
	float n = ve_noised(q + P.fjord_height_valley_warp * w).x;
	float t = 1.0 - smoothstep(0.0, P.fjord_height_valley_width, abs(n));
	return P.fjord_height_valley_depth * t * t;
}

// Metres above water -> metres above water (spec §6.3 step 4). Monotone and continuous: above
// wall_height unchanged; below it the wall rises up to (1 + wall_steepen)x faster; under water
// the floor flattens toward -floor_depth, meeting the wall's slope at the shoreline.
float fjord_profile(float m) {
	float k = P.fjord_height_wall_steepen, hw = P.fjord_height_wall_height;
	if (m >= hw) return m;
	if (m >= 0.0) {
		float u = 1.0 - m / hw;
		return m + k * m * u * u;
	}
	float d = P.fjord_height_floor_depth;
	return -d * (1.0 - exp(m * (1.0 + k) / d));
}

vec2 stage_fjord_height(vec2 xz) {
	vec2 p = xz / P.fjord_height_scale_l;
	vec3 n = fjord_fbm(p);
	// Carve before erosion so its fade target sees the valleys and gullies drain into them.
	// The carve's derivative only steers gully direction; a finite difference is plenty.
	const float e = 1e-3;
	float c0 = fjord_carve(p);
	n -= vec3(c0, (fjord_carve(p + vec2(e, 0.0)) - c0) / e, (fjord_carve(p + vec2(0.0, e)) - c0) / e);
	float fade = clamp(n.x / (P.fjord_height_height_amp * 0.6), -1.0, 1.0);
	n = n * 0.5 + vec3(0.5, 0.0, 0.0);
	float ridge_map, dbg;
	vec4 h = ErosionFilter(p, n, fade,
			P.fjord_height_erosion_strength, P.fjord_height_gully_weight, P.fjord_height_detail,
			vec4(P.fjord_height_ridge_rounding, P.fjord_height_crease_rounding, 0.1, 2.0),
			vec4(1.25, 1.25, 2.8, 1.5), vec2(0.7, 1.0),
			P.fjord_height_erosion_scale, 5, 2.0, 0.5,
			P.fjord_height_cell_scale, P.fjord_height_normalization,
			ridge_map, dbg);
	float eroded = n.x + h.x + P.fjord_height_height_offset * h.w;
	float y = P.fjord_height_water_y +
			fjord_profile(P.fjord_height_scale_v * (eroded - P.fjord_height_h_water));
	return vec2((y - (P.fjord_height_water_y - SECTOR_HEIGHT_BELOW_M)) / SECTOR_HEIGHT_SPAN_M,
			ridge_map * 0.5 + 0.5);
}
```

- [ ] **Step 5: Write the field and band stages**

`shaders/stages/fjord.field.glslh`:
```glsl
//!stage     fjord
//!kind      field
//!sample    sector.terrain : texture2d_rg16
//!out       sdf : float
//!out       height : float
//!out       slope : float
//!out       ridge : float
//!use       fjord_height.water_y
//!lipschitz add 3.67
//!cpu       ve::stage_fjord

// The fjord ground from the baked sectors (spec §7.1). Height and gradient come from one
// B-spline over 16 texels, so the slope costs nothing more.
//
// Bound: the B-spline's per-axis gradient never exceeds the steepest per-axis texel step
// (extension/tests/test_sector.cpp), which the bake keeps at or under S_max = 2.5
// (kSectorSlopeLimit; tests/test_fjords.gd). So |grad h| <= sqrt(2) * 2.5 and
// |grad sdf| <= sqrt(1 + 2 * 2.5^2) = 3.67.
#include "sector.glslh"

// For sub-projects B and C: height above SURFACE_Y, dh/dx, dh/dz, ridge in -1..1.
vec4 fjord_ground(vec2 xz) { return sector_ground(xz, P.fjord_height_water_y); }

void stage_fjord(inout FieldCtx ctx) {
	vec4 g = fjord_ground(ctx.p.xz);
	ctx.height = g.x;
	ctx.slope = length(g.yz);
	ctx.ridge = g.w;
	ctx.sdf = ctx.p.y - SURFACE_Y - ctx.height;
}
```
`shaders/stages/fjord_bands.field.glslh`:
```glsl
//!stage     fjord_bands
//!kind      field
//!in        sdf : float
//!in        height : float
//!in        slope : float
//!in        ridge : float
//!out       material : uint
//!param     shore : float = 2.0
//!param     rock_slope : float = 1.2
//!param     snow_line : float = 170.0
//!param     snow_jitter : float = 25.0
//!param     snow_ridge_drop : float = 40.0
//!param     snow_slope : float = 1.0
//!param     peak_line : float = 240.0
//!param     peak_snow_slope : float = 1.4
//!use       fjord_height.water_y
//!cpu       ve::stage_fjord_bands

// Materials by elevation above water, slope and ridge (spec §7.2, plan deviation 11). No
// //!lipschitz: this stage writes no sdf. Integer hashing so the CPU mirror agrees exactly.

uint fjord_pcg(uint v) {
	uint s = v * 747796405u + 2891336453u;
	uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
	return (w >> 22u) ^ w;
}
float fjord_cell(ivec2 c) {
	return float(fjord_pcg(uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u) & 0xffffu) / 32767.5 - 1.0;
}
// Value noise in -1..1 over 40 m cells: the snow line's waver.
float fjord_snow_noise(vec2 xz) {
	vec2 q = xz / 40.0;
	vec2 b = floor(q);
	ivec2 i = ivec2(b);
	vec2 f = q - b;
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = fjord_cell(i), c1 = fjord_cell(i + ivec2(1, 0));
	float c2 = fjord_cell(i + ivec2(0, 1)), c3 = fjord_cell(i + ivec2(1, 1));
	float lo = a + (c1 - a) * u.x, hi = c2 + (c3 - c2) * u.x;
	return lo + (hi - lo) * u.y;
}

void stage_fjord_bands(inout FieldCtx ctx) {
	if (ctx.sdf > 0.0) { ctx.material = 0u; return; }
	float e = SURFACE_Y + ctx.height - P.fjord_height_water_y;
	if (e < P.fjord_bands_shore) { ctx.material = MAT_GROUND_01; return; }
	float line = P.fjord_bands_snow_line + P.fjord_bands_snow_jitter * fjord_snow_noise(ctx.p.xz) -
			P.fjord_bands_snow_ridge_drop * max(ctx.ridge, 0.0);
	if (e > line) {
		float limit = e > P.fjord_bands_peak_line ? P.fjord_bands_peak_snow_slope : P.fjord_bands_snow_slope;
		ctx.material = ctx.slope < limit ? MAT_SNOW : MAT_BREAKSTONE;
		return;
	}
	ctx.material = ctx.slope > P.fjord_bands_rock_slope ? MAT_BREAKSTONE : MAT_GRASS_01;
}
```

- [ ] **Step 6: Write the CPU mirrors**

In `builtin_stages.cpp`, add to the anonymous namespace at the top:
```cpp
constexpr uint16_t kBandSnow = ve::material_id("snow");
constexpr uint16_t kBandBreakstone = ve::material_id("breakstone");
```
and inside `namespace ve`, after the fixture mirror:
```cpp
VE_STAGE_SLOTS(Fjord, p, sdf, height, slope, ridge);
VE_STAGE_PARAMS(Fjord, fjord_height_water_y);

// Mirror of shaders/stages/fjord.field.glslh.
void stage_fjord(FieldCtx &ctx, const FjordSlots &s, const FjordParams &p, const FieldResources &res) {
	const SectorGround g = sector_ground(res.sectors, p.fjord_height_water_y, ctx.v(s.p)[0], ctx.v(s.p)[2]);
	ctx.f(s.height) = g.height;
	ctx.f(s.slope) = std::sqrt(g.dhdx * g.dhdx + g.dhdz * g.dhdz);
	ctx.f(s.ridge) = g.ridge;
	ctx.f(s.sdf) = ctx.v(s.p)[1] - kSurfaceY - g.height;
}

VE_STAGE_SLOTS(FjordBands, p, sdf, height, slope, ridge, material);
VE_STAGE_PARAMS(FjordBands, shore, rock_slope, snow_line, snow_jitter, snow_ridge_drop, snow_slope,
		peak_line, peak_snow_slope, fjord_height_water_y);

namespace fjord_mirror {
inline uint32_t pcg(uint32_t v) {
	const uint32_t s = v * 747796405u + 2891336453u;
	const uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
	return (w >> 22u) ^ w;
}
inline float cell(int x, int z) {
	return float(pcg(uint32_t(x) * 0x8da6b343u ^ uint32_t(z) * 0xd8163841u) & 0xffffu) / 32767.5f - 1.0f;
}
inline float snow_noise(float x, float z) {
	const float qx = x / 40.0f, qz = z / 40.0f;
	const float bx = std::floor(qx), bz = std::floor(qz);
	const int ix = int(bx), iz = int(bz);
	const float fx = qx - bx, fz = qz - bz;
	const float ux = fx * fx * (3.0f - 2.0f * fx), uz = fz * fz * (3.0f - 2.0f * fz);
	const float a = cell(ix, iz), c1 = cell(ix + 1, iz), c2 = cell(ix, iz + 1), c3 = cell(ix + 1, iz + 1);
	const float lo = a + (c1 - a) * ux, hi = c2 + (c3 - c2) * ux;
	return lo + (hi - lo) * uz;
}
} // namespace fjord_mirror

// Mirror of shaders/stages/fjord_bands.field.glslh.
void stage_fjord_bands(FieldCtx &ctx, const FjordBandsSlots &s, const FjordBandsParams &p,
		const FieldResources &) {
	if (ctx.f(s.sdf) > 0.0f) { ctx.f(s.material) = 0.0f; return; }
	const float e = kSurfaceY + ctx.f(s.height) - p.fjord_height_water_y;
	if (e < p.shore) { ctx.f(s.material) = float(kBandGround); return; }
	const float line = p.snow_line + p.snow_jitter * fjord_mirror::snow_noise(ctx.v(s.p)[0], ctx.v(s.p)[2]) -
			p.snow_ridge_drop * std::max(ctx.f(s.ridge), 0.0f);
	const float slope = ctx.f(s.slope);
	if (e > line) {
		const float limit = e > p.peak_line ? p.peak_snow_slope : p.snow_slope;
		ctx.f(s.material) = float(slope < limit ? kBandSnow : kBandBreakstone);
		return;
	}
	ctx.f(s.material) = float(slope > p.rock_slope ? kBandBreakstone : kBandGrass);
}
```
Add `#include <algorithm>` if absent, and the registrations:
```cpp
VE_REGISTER_STAGE("ve::stage_fjord", Fjord, stage_fjord);
VE_REGISTER_STAGE("ve::stage_fjord_bands", FjordBands, stage_fjord_bands);
```

- [ ] **Step 7: Write the pipeline and the world type**

`assets/pipelines/fjords.pipeline`:
```
# Fjords (docs/superpowers/specs/2026-10-07-fjords-terrain-design.md): eroded mountains around
# long fjord valleys, baked per sector by the map stage and sampled by the field stages.
#
# The ceiling sits just over the computed 3.67 (fjord.field.glslh's bound from S_max = 2.5).
# Look tuning goes in indented overrides under fjord_height and fjord_bands; the field stage
# writes sdf and so takes none.
lipschitz 3.7
stage stages/fjord_height.map.glslh
stage stages/fjord.field.glslh
stage stages/fjord_bands.field.glslh
```
`demo/world_types/30_fjords.tres` — copy `demo/world_types/20_flat.tres` and change only `display_name = "Fjords"` and `pipeline_path = "res://assets/pipelines/fjords.pipeline"`. Keep its `[gd_resource ...]` header and script `ext_resource` line exactly as the Flat file has them.

- [ ] **Step 8: Run the tests**

Run: `./build.sh && ./build.sh --verify && ./gdunit_tests.sh -a res://tests/test_fjords.gd && ./gdunit_tests.sh -a res://tests/test_field_diff.gd && ./gdunit_tests.sh -a res://tests/test_create_world.gd`
Expected: all pass. `test_field_diff.gd` now covers `fjords.pipeline` at seed 0 and its own far-seed case remains on `default.pipeline`.

If the slope test fails (`over_limit > 0`): the finest erosion octaves are too steep. Lower `gully_weight` in steps of 0.05 (or `erosion_strength`) via indented overrides under `stage stages/fjord_height.map.glslh` in `fjords.pipeline`, re-run, and record the final values for Task 9's deviations. Do not raise `kSectorSlopeLimit` or the declared bound.

If the range test fails: `r_max ≥ 0.98` means peaks clamp — lower `scale_v` or raise `h_water`; `r_min ≤ 0.01` means floors clamp — `floor_depth` must stay under 64. Record what moved.

If the bands test fails on one material, read which ids were seen and adjust the band thresholds in `fjords.pipeline`; record it.

- [ ] **Step 9: Add a far-seed fjord diff**

In `tests/test_field_diff.gd`, after `test_a_far_seeded_world_agrees_between_cpu_and_gpu`, add a twin on `fjords.pipeline` (sectors at the far offset exercise the window's negative wrap and the bake's large coordinates on both sides):
```gdscript
func test_a_far_seeded_fjord_world_agrees_between_cpu_and_gpu() -> void:
	var probe: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	var seed := 0
	for s in range(1, 10000):
		probe.world_seed = s
		var o: Vector3 = probe.field_offset()
		if minf(o.x, o.z) < -7000.0:
			seed = s
			break
	probe.free()
	assert_int(seed).is_greater(0)
	_open_world("res://assets/pipelines/fjords.pipeline", seed)
	var pts := surface_points()
	compare(pts, PackedByteArray(), 0, "fjords seed %d base" % seed)
	_close_world()
```
Run: `./gdunit_tests.sh -a res://tests/test_field_diff.gd` — Expected: pass.

- [ ] **Step 10: Pin suites and commit**

Run the Task 0 pin suites; no new failures against the baseline. `git status --short shaders/generated tests/golden` is empty.
```bash
git add shaders/noise2d.glslh shaders/erosion.glslh shaders/stages/fjord_height.map.glslh shaders/stages/fjord.field.glslh shaders/stages/fjord_bands.field.glslh assets/pipelines/fjords.pipeline demo/world_types/30_fjords.tres extension/src/terrain/builtin_stages.cpp tests/test_fjords.gd tests/test_fjords.gd.uid tests/test_create_world.gd tests/test_field_diff.gd
git commit -m "feat: the Fjords world type, eroded by the Phacelle erosion filter"
```

---

### Task 9: Capture, look tuning, measurement and the spec record

**Files:**
- Create: `tools/fjord_capture.gd`
- Modify: `demo/scripts/benchmark.gd` (`--pipeline=`)
- Modify: `assets/pipelines/fjords.pipeline` (tuning overrides, if any)
- Modify: `docs/superpowers/specs/2026-10-07-fjords-terrain-design.md` (§12)

- [ ] **Step 1: Write the capture tool**

`tools/fjord_capture.gd`:
```gdscript
extends SceneTree
# Reproducible Fjords look check through the SHIPPING render path (spec §9 step 7). Run with:
# godot --path . --resolution 1280x720 -s res://tools/fjord_capture.gd -- --out=/tmp/fjords [--seed=N]
# (Not --headless: it needs a real RenderingDevice.)
#
# Writes valley.png, ridge.png and aerial.png -- one pose per reference screenshot -- found
# by raycasting the CPU field on a grid around the origin once the sectors there are baked:
#   valley: on the water at the lowest point, looking along the longest run of low ground;
#   ridge:  8 m over the highest point, looking toward the lowest ground, pitched down;
#   aerial: 250 m over the valley point, looking along the valley, pitched down.
# Prints each pose so a benchmark or a later run can reuse it.
const WATER_Y := 51.2

func _initialize() -> void:
	call_deferred("capture")

func _arg(name: String, fallback: String) -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--%s=" % name):
			return arg.trim_prefix("--%s=" % name)
	return fallback

func _ground(world: VoxelWorld, x: float, z: float) -> float:
	var hit: Dictionary = world.raycast(Vector3(x, 600.0, z), Vector3.DOWN, 1200.0)
	return (hit["pos"] as Vector3).y if hit["hit"] else INF

func _settle(world: VoxelWorld, frames: int) -> bool:
	var quiet := 0
	for frame in range(frames):
		await process_frame
		var s: Dictionary = world.hooks().debug_lod_stats()
		quiet = quiet + 1 if int(s.get("requests_pending", 1)) == 0 and int(s.get("builds_in_flight", 1)) == 0 and int(s.get("sector_held", 1)) == 0 else 0
		if quiet >= 30 and int(s.get("draw_pages", 0)) > 0:
			return true
	return false

func _shoot(world: VoxelWorld, camera: Camera3D, player: Node3D, at: Vector3, dir: Vector3, path: String) -> bool:
	player.global_position = at
	camera.global_position = at
	camera.look_at(at + dir)
	if not await _settle(world, 6000):
		push_error("fjord capture did not settle at %s" % at)
		return false
	for frame in range(8):
		await process_frame
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(path)
	print("FJORD_CAPTURE %s at=%s dir=%s" % [path.get_file(), at, dir])
	return true

func capture() -> void:
	var out := _arg("out", "/tmp/fjords")
	DirAccess.make_dir_recursive_absolute(out)
	var scene: Node = load("res://demo/scenes/main.tscn").instantiate()
	var world: VoxelWorld = scene.get_node("VoxelWorld")
	world.world_type = load("res://demo/world_types/30_fjords.tres")
	world.world_seed = int(_arg("seed", "0"))
	root.add_child(scene)
	var player: CharacterBody3D = scene.get_node("Player")
	player.set_physics_process(false)
	player.set_process_unhandled_input(false)
	world.physics_enabled = false
	world.set_effect_enabled("ssgi", false)
	world.set_grass_value("wind_strength", 0.0)
	world.set_grass_value("wind_speed", 0.0)
	if not world.hooks().debug_init_physics():
		push_error("fjord capture could not initialize the mesh worker")
		quit(1)
		return
	var camera: Camera3D = player.get_node("Camera3D")
	camera.far = 5000.0
	scene.get_node("HUD").visible = false
	player.global_position = Vector3(0.0, 400.0, 0.0)
	if not await _settle(world, 6000):
		push_error("fjord capture: the world around the origin never settled")
		quit(1)
		return

	# A 41 x 41 grid at 100 m over +-2 km.
	var low := Vector3(0.0, INF, 0.0)
	var high := Vector3(0.0, -INF, 0.0)
	for i in range(-20, 21):
		for j in range(-20, 21):
			var y := _ground(world, i * 100.0, j * 100.0)
			if y == INF:
				continue
			if y < low.y:
				low = Vector3(i * 100.0, y, j * 100.0)
			if y > high.y:
				high = Vector3(i * 100.0, y, j * 100.0)
	# The valley's direction: of 16 headings, the one with the longest run of ground under
	# water + 5 m from the low point.
	var best_dir := Vector3(1.0, 0.0, 0.0)
	var best_run := -1
	for k in range(16):
		var a := TAU * k / 16.0
		var d := Vector3(cos(a), 0.0, sin(a))
		var run := 0
		while run < 60 and _ground(world, low.x + d.x * 25.0 * (run + 1), low.z + d.z * 25.0 * (run + 1)) < WATER_Y + 5.0:
			run += 1
		if run > best_run:
			best_run = run
			best_dir = d
	var valley_at := Vector3(low.x, WATER_Y + 3.0, low.z)
	var to_low := Vector3(low.x - high.x, 0.0, low.z - high.z).normalized()
	var ok := await _shoot(world, camera, player, valley_at, best_dir + Vector3(0.0, 0.08, 0.0), out.path_join("valley.png"))
	ok = ok and await _shoot(world, camera, player, high + Vector3(0.0, 8.0, 0.0), to_low + Vector3(0.0, -0.15, 0.0), out.path_join("ridge.png"))
	ok = ok and await _shoot(world, camera, player, valley_at + Vector3(0.0, 250.0, 0.0), best_dir + Vector3(0.0, -0.45, 0.0), out.path_join("aerial.png"))
	print("FJORD_CAPTURE sector stats: ", world.hooks().debug_sector_stats())
	world.shutdown_render_resources()
	await process_frame
	quit(0 if ok else 1)
```

- [ ] **Step 2: Run it**

Run: `./build.sh --verify && godot --path . --resolution 1280x720 -s res://tools/fjord_capture.gd -- --out="$TMPDIR/fjords"`
Expected: exit 0, three PNGs, three `FJORD_CAPTURE ... at=... dir=...` lines and the stats line. Read each PNG.

- [ ] **Step 3: Compare with the references and tune**

Show the user `valley.png`, `ridge.png` and `aerial.png` beside `~/Pictures/Screenshots/screenshot_20261007_150634.png`, `…150751.png` and `…150810.png`. Judge, and say what you see for each:
- valley floors flat and wide, walls rising steeply from them (screenshots 1 and 3);
- long continuous valleys rather than round basins;
- gullies on every slope;
- snow on peaks and down ridges, grey-blue `breakstone` on steep faces (screenshot 2);
- no flat-topped peaks, no seams at 409.6 m sector edges, no snow bloom halos.

Tune only through indented overrides in `fjords.pipeline` under `fjord_height` and `fjord_bands` (no rebuild needed; re-run the capture). After every change that touches `fjord_height`, re-run `./gdunit_tests.sh -a res://tests/test_fjords.gd` — the slope and range limits must still hold. Stop when the user accepts the look. Commit the tuning:
```bash
git add assets/pipelines/fjords.pipeline tools/fjord_capture.gd tools/fjord_capture.gd.uid
git commit -m "feat: a scripted Fjords capture, and the look tuning it settled"
```

- [ ] **Step 4: Measure**

Add to `demo/scripts/benchmark.gd`'s second argument loop (the one that reads `--near-scale=`):
```gdscript
		elif arg.begins_with("--pipeline="):
			_world.terrain_pipeline_path = arg.trim_prefix("--pipeline=")
```
Read the `--pose=` parsing a few lines below it and format the valley pose from Step 2 to match. Then, with vsync disabled and interleaved A/B/A (Default, Fjords, Default), three runs each of the steady leg:
```bash
godot --path . --resolution 2560x1440 --disable-vsync demo/scenes/main.tscn -- --benchmark --pose=<valley pose> 2>&1 | tail -20
godot --path . --resolution 2560x1440 --disable-vsync demo/scenes/main.tscn -- --benchmark --pose=<valley pose> --pipeline=res://assets/pipelines/fjords.pipeline 2>&1 | tail -20
```
Record p50/p99 wall frame time for each. Record cold fill: the frames `fjord_capture.gd` took to settle at the origin, and `debug_sector_stats()` `inserted` / `bakes_dispatched` at that point. Record memory: `max_resident` × 270 400 B per device (two devices) plus host. These are reported, not gated (spec §9).

Commit the flag:
```bash
git add demo/scripts/benchmark.gd
git commit -m "chore: a benchmark --pipeline flag for measuring a world type"
```

- [ ] **Step 5: Record the deviations in the spec**

In `docs/superpowers/specs/2026-10-07-fjords-terrain-design.md`, replace §12's placeholder line with two subsections: **12.1 Decided while planning** (this plan's Deviations 1–14, one paragraph each, copied from the top of this plan) and **12.2 Recorded while implementing** (every tuning override that landed in `fjords.pipeline`, with the reason; anything a task's "if it fails" branch changed; the Step 4 measurements with their method; what the user said about the capture). Change the spec's **Status** line to `implemented (sub-project A); B and C sketched only`.
```bash
git add docs/superpowers/specs/2026-10-07-fjords-terrain-design.md
git commit -m "docs: record the Fjords deviations, tuning and measurements"
```

- [ ] **Step 6: Final gate**

Run: `./build.sh --test && ./gdunit_tests.sh 2>&1 | tail -40`
Expected: native SUCCESS; gdUnit failures equal Task 0's baseline set exactly (new suites all pass). `git status --short` clean; `git diff main --stat -- shaders/generated/field.glslh.golden tests/golden` empty.
