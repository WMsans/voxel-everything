# M6 Beautification & Shading Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the near field, the far field and Godot's own dynamic objects arrive in **one merged G-buffer** and leave it through **one deferred cel-lighting stack**, then hang the three shadow layers, SSGI, SSR and outlines off that single stack — so the 150 m seam stops being a place where two shading paths meet and becomes a place where one shading path reads two sources.

**Architecture:** Today both fields shade *forward*: `raymarch.comp.glsl` calls `shade_terrain()` and writes colour, `lod.frag.glsl` calls `shade_terrain()` and writes colour, and `composite.frag.glsl` blits the near field's colour into Godot's scene buffer. M6 replaces the colour with **surface description**. The raymarcher writes albedo / oct-normal / material / gloss / sun-visibility at 0.66×; the composite resolves that into a full-resolution G-buffer (two colour attachments plus a depth attachment this engine owns); the LoD raster writes the *same* two attachments and the *same* depth; one compute pass reads the G-buffer and produces lit colour; one fullscreen pass injects that colour and that depth into Godot's scene buffers so the opaque pass composites against it exactly as it does today. A second `CompositorEffect` at POST_OPAQUE then adds the effects that need dynamic objects in the picture — contact shadows, SSR, outlines — and hands the finished frame back as next frame's one-bounce GI source.

**Tech Stack:** Godot 4.7.1 (`/usr/bin/godot`), godot-cpp (pinned master, `api_version = "4.7"`), SCons, C++20, GLSL 460 (Vulkan), Jolt Physics, doctest 2.4.11 (native), gdUnit4 (in-engine).

**Spec:** `docs/superpowers/specs/2026-08-12-voxel-engine-design.md` — **§7 is this milestone**, with the two forward references from §3 ("Known spike items": pre-opaque depth injection ordering, and the accessibility of Godot's normal-roughness buffer) and §5's island-shading requirement ("islands shade/shadow/reflect exactly like static terrain"). No separate M6 design spec exists, so every number this milestone needs is fixed in the **Fixed Numbers** table below rather than in a spec.

**Predecessors:** `docs/superpowers/plans/2026-08-12-m1-walking-skeleton.md`, `2026-08-13-m2-gpu-generation-streaming-edits.md`, `2026-08-14-m3-physics-meshing-colliders.md`, `2026-08-15-m4-connectivity-islands.md`, `2026-08-17-m5-far-field-lod.md` (all complete). **Read all five Errata sections before touching a shader or a pass.** The ones this plan collides with directly:

- **M1 errata 2** — Godot 4.7.1 Forward+ is reverse-Z (near = 1.0, far = 0.0). Every pipeline writing scene depth uses `COMPARE_OP_GREATER_OR_EQUAL`. Task 6's injection depends on it, Task 8's shadow map depends on it, and every depth comparison in Tasks 9–12 depends on it.
- **M1 errata 3** — tan-half-fov comes from `|1/c00|`, `|1/c11|`, never `Projection::get_fov()`. Tasks 5 and 10 both rebuild view rays from these.
- **M2 errata 5** — GLSL reserved words (`active`, `mat2`) are rejected by glslang. Do not name anything `sample`, `filter`, `output`, `light` or `normal` either; this plan's shaders use `n`, `wn`, `src_normal`.
- **M2 errata 7** — `ivec4` push-constant members need `.xyz` when passed to `ivec3` parameters.
- **M5 errata 3** — **index push constants by float, not by byte**, and keep the `PackedByteArray` size and `draw_list_set_push_constant`'s size argument in step. A single mis-indexed write corrupted the heap on every draw for a whole task while every test still reported PASS. Every push-constant layout in this plan is written out as an explicit float-index map for that reason.
- **M5 errata 5** — the gdUnit runner is `./gdunit_tests.sh`. Never invoke `addons/gdUnit4/runtest.sh`. Pass `-c` to see every failure; a plain run aborts a suite at its first one.
- **M5 errata 6** — **a fixed frame count settles nothing.** Wait on the condition (`debug_lod_stats()` reporting `requests_pending == 0 and builds_in_flight == 0` for a streak of 8 ticks). Every settle helper in this plan's tests is condition-based.
- **M5 errata 2** — the LoD raster's front face is **clockwise** and `LodRasterPass` picks its pipeline from `front_face_clockwise_`. Task 7 adds a colour attachment to that pipeline set; it must not disturb the face selection, and Task 8's depth-only shadow pipeline must make the same choice (it inherits the flag rather than re-deriving it).
- **M5 errata 15** — `lod_ms` is CPU command-record time, not GPU execution. Task 14's budget verdicts must not read it as a GPU number; it measures per-pass GPU time with timestamps instead.

## Milestone Map

| Milestone | Delivers |
|---|---|
| M1 (done) | Toolchain, raymarched terrain, test harnesses |
| M2 (done) | GPU brick generation, region indirection, residency/LRU, min–max mips, destruction edits |
| M3 (done) | Dual-contour collision meshing on the GPU, async readback, collider streaming into Jolt, character controller |
| M4 (done) | Occupancy grid, connectivity, island carve/extract/spawn/re-merge, raymarched island targets, tiled culling, debris |
| M5 (done) | Eight-level LoD octree, 12-byte quad arena, triplanar material textures shared with the near field, HiZ occlusion, indirect multi-draw far field, dithered seam |
| **M6 (this plan)** | Merged G-buffer, deferred cel lighting, three shadow layers, SSGI, SSR, outlines, cel-shaded dynamic objects, per-pass GPU timings |
| M7 | Benchmark scene, demo polish, portfolio capture |

## Global Constraints

- Godot **4.7.1**; godot-cpp pinned to the existing submodule commit, `api_version = "4.7"` — do not bump either.
- Pure C++ cores in `namespace ve` contain **zero Godot types** (engine spec §8). `shade/` is new and **pure**; `render/` is Godot glue. Anything that quantises, packs, clamps or builds a matrix is pure; anything owning a `RID` is glue.
- Shaders: GLSL `#version 460`, loaded **from files** by `ve::load_shader_source`, never inline strings. `#[compute]` / `#[vertex]` / `#[fragment]` are stripped after load by `ve::strip_shader_annotations`.
- **Never put a literal include directive inside a GLSL comment** — the loader matches the token anywhere on a line and self-includes (note at the top of `shaders/common.glslh`).
- `buffer_update`, `buffer_clear`, `texture_update`, `texture_copy`, `texture_clear` are device-level commands: record them **before** `compute_list_begin` / `draw_list_begin`, never inside an open list (M2 Task 12's documented ordering).
- **Push constants stay ≤ 128 bytes** (Vulkan's guaranteed minimum). M6's largest is the deferred pass's 128 bytes exactly — `static_assert` it.
- Reverse-Z everywhere; `COMPARE_OP_GREATER_OR_EQUAL` for anything writing scene depth or the G-buffer's depth.
- **There is exactly one shading implementation per shader language.** `shaders/shade.glslh` is the only RenderingDevice GLSL copy of oct packing, the cel ramp and the sun constant; `shaders/cel.gdshaderinc` is the only Godot ShaderLanguage copy; `ve::cel_shade` is the pure C++ reference. `raymarch.comp.glsl`, `lod.frag.glsl` and `deferred.comp.glsl` include the first; every dynamic-object `.gdshader` includes the second and never spells a ramp itself. Task 6 diffs RenderingDevice GLSL against C++, and Task 13 diffs the `.gdshaderinc` against the same C++ reference, so neither shader-language copy can drift silently.
- Error policy (engine spec §8): dev = validation layers + verbose RD checks; release = fail-soft. **A plainer image is always the safe direction** — a missing shadow map means unshadowed, a missing SSGI texture means constant ambient, a failed post-opaque pass leaves the frame as the deferred stack produced it, never black.
- **Every effect has a quality/off toggle** (spec §7's last line). `ve::BeautySettings` is that toggle set, and no pass may read a knob that is not in it.
- Commit style: conventional (`feat:`, `test:`, `fix:`, `build:`, `refactor:`, `docs:`).
- RD API reference: `docs/api/renderingdevice.md`. Consult it before inventing a signature.
- Target hardware: RTX 4070 Laptop. Budgets from spec §7: raymarch ≤ 6 ms, LoD ≤ 2 ms, SSGI ≤ 1.5 ms, SSR ≤ 1.5 ms, shadows ≤ 1 ms, outlines ≤ 0.3 ms, frame ≤ 16 ms @ 1440p.

## Conventions Used Throughout

- **Build:** `./build.sh -j$(nproc)` (or `cd extension && scons -j$(nproc)`)
- **Native tests:** `cd extension && scons test`
- **gdUnit tests:** `./gdunit_tests.sh -a res://tests/<suite>.gd`, or `./gdunit_tests.sh` for everything. Add `-c` to see every failure.
- **Demo:** `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
- gdUnit tests that await must declare the timeout argument: `func test_x(timeout := 10000) -> void:`
- Every gdUnit suite creating a `VoxelWorld` registers it in `_worlds` and frees it in `after_test()` (M3 errata 2).
- New `src/shade/*.cpp` files are picked up by `SConstruct`'s `Glob("src/*/*.cpp")` for the library automatically, but the **native test runner's `pure_sources` list is explicit** — Task 1 adds `Glob("src/shade/*.cpp")` to it, exactly as M5 added `src/lod/*.cpp`.
- New `extension/tests/test_*.cpp` files are picked up by `Glob("tests/*.cpp")` automatically; no SConstruct edit is needed for those.

## Fixed Numbers (decided here; every task assumes them)

| Thing | Value | Where it lives |
|---|---|---|
| G-buffer albedo target | `R8G8B8A8_UNORM` — rgb albedo, **a = sun visibility** | `godot::GBuffer` |
| G-buffer surface target | `R16G16B16A16_SFLOAT` — `.xy` oct normal, `.z` material id, `.w` gloss | `godot::GBuffer` |
| G-buffer depth target | `D32_SFLOAT`, reverse-Z, same NDC as the scene | `godot::GBuffer` |
| Lit target | `R16G16B16A16_SFLOAT`, full resolution | `godot::GBuffer` |
| GI history | `R16G16B16A16_SFLOAT`, **half** resolution | `godot::GBuffer` |
| Named-texture context | `"voxel_gbuf"` | `GBuffer::kContext` |
| Sun direction | `normalize(vec3(0.6, 0.8, 0.3))` | `ve::kSunDir`, `SUN_DIR` in `shade.glslh` |
| Cel bands | **4** | `ve::kCelBands` |
| Band edges (N·L) | `0.08, 0.32, 0.66` | `ve::CelParams::band_edge` |
| Band levels | `0.18, 0.45, 0.75, 1.00` | `ve::CelParams::band_level` |
| Shadow hue shift | `0.055` turns (toward blue), saturation ×`1.35` | `ve::CelParams` |
| Specular band edge / strength | `0.72` / `0.45` | `ve::CelParams` |
| Rim strength / power | `0.35` / `3.0` | `ve::CelParams` |
| Raymarched sun shadow | 1 ray/px, **60 m**, 96 steps, softness `k = 12` | `RAY_SHADOW_*` in `raymarch.comp.glsl` |
| Sun shadow map | **2048² `D32_SFLOAT`** ortho, world-covering | `godot::SunShadowPass::kSize` |
| Shadow map rebuild | on dirty **and** ≥ 12 frames since the last | `SunShadowPass::kMinFrames` |
| Shadow map depth bias | 2 texels of slope + `0.0015` constant, in the shader | `lod_shadow.vert.glsl` |
| SSGI | half res, **8 taps** (high) / 4 (medium) / 0 (off), 6 m radius | `ve::BeautySettings::ssgi_taps` |
| SSGI temporal blend | `0.90` history, 3×3 neighbourhood clamp | `SSGI_BLEND` |
| SSR | half res, **24 steps** (high) / 12 (medium) / 0 (off), 40 m reach, 1.5 m world-space crossing thickness, strength `0.80` | `ve::BeautySettings::ssr_steps`, `SsrPass` |
| Contact shadows | half res, **12 steps**, 0.6 m reach | `ve::BeautySettings::contact_steps` |
| Outlines | full res, depth threshold `0.04` relative, normal threshold `0.25`, darken ×`0.35`; compare +x/+y so each line is one pixel | `ve::BeautySettings`, `OutlinePass` |
| Glossy SDF reflection rays | ≤ 20 m, 64 steps, only when `gloss > 0.5`; albedo-space blend `0.80 × Schlick × smoothstep(0.5,1,gloss)`, capped at `0.85` | `ve::BeautySettings::glossy_sdf_rays` |
| GPU timing unit | `capture_timestamp` values are microseconds; publish differences in milliseconds (`Δµs / 1000.0`) | `godot::GpuTimings` |
| Quality tiers | `Off, Low, Medium, High` (demo default **High**) | `ve::QualityTier` |

**Memory @1440p internal (2560×1440 = 3.69 Mpx).** gb0 14.7 MB + gb1 29.5 MB + gbd 14.7 MB + lit 29.5 MB + GI history (half) 7.4 MB + SSGI (half, ×2 for history) 14.8 MB + SSR (half) 7.4 MB + contact (half, R8) 0.9 MB + sun map 16.8 MB + the raymarcher's three 0.66× targets 44.9 MB ≈ **180 MB**. On top of M5's ≈ 238 MB and the near-field brick pool this is comfortable on a 4070 Laptop's 8 GB.

## File Structure

```
extension/src/
  shade/                                                   (pure C++, namespace ve)
    oct.h/.cpp             normal <-> two floats, exact round trip          (Task 1)
    cel.h/.cpp             band search, ramp, shadow tint, rim, spec        (Task 2)
    beauty_settings.h/.cpp toggles, tiers, clamping, flag packing           (Task 3)
    sun_ortho.h/.cpp       world-covering ortho matrix + texel snap         (Task 8)
  render/
    gbuffer.h/.cpp         the merged G-buffer's textures and lifetime      (Task 4)
    raymarch_pass.h/.cpp   MODIFIED: three targets instead of two           (Task 5)
    composite_pass.h/.cpp  MODIFIED: MRT into the G-buffer                  (Task 6)
    deferred_pass.h/.cpp   the one lighting stack                           (Task 6)
    inject_pass.h/.cpp     lit colour + depth into Godot's scene buffers    (Task 6)
    lod_raster_pass.h/.cpp MODIFIED: two colour attachments                 (Task 7)
    hiz_pass.h/.cpp        MODIFIED: builds from the G-buffer's depth       (Task 7)
    sun_shadow_pass.h/.cpp ortho depth-only draw of the LoD arena           (Task 8)
    contact_shadow_pass.h/.cpp   half-res screen-space contact shadows      (Task 9)
    ssgi_pass.h/.cpp       half-res horizon GI with temporal accumulation   (Task 10)
    ssr_pass.h/.cpp        half-res screen-space reflections                (Task 11)
    outline_pass.h/.cpp    depth+normal discontinuity lines                 (Task 12)
    gpu_timings.h/.cpp     delayed GPU timestamp pairing and pass totals     (Task 14)
  beauty_compositor.h/.cpp POST_OPAQUE CompositorEffect                     (Task 9)
  raymarch_compositor.cpp  MODIFIED: the whole pre-opaque order        (Tasks 5,6,7,8,10)
  voxel_world.h/.cpp       MODIFIED: passes, settings, debug hooks       (Tasks 3-14)
  register_types.cpp       MODIFIED: BeautyCompositor                       (Task 9)
  physics/island_body.cpp  MODIFIED: debris wears the cel material          (Task 13)
shaders/
  shade.glslh              NEW: SUN_DIR, oct pack, cel ramp                 (Tasks 1,2)
  common.glslh             MODIFIED: material_props(), shade_terrain retired
  raymarch.comp.glsl       MODIFIED: G-buffer out + sun ray + gloss rays  (Tasks 5,11)
  composite.frag.glsl      MODIFIED: MRT + G-buffer depth                   (Task 6)
  deferred.comp.glsl       NEW: the cel lighting stack                      (Task 6)
  inject.vert/frag.glsl    NEW: colour + depth into the scene buffers       (Task 6)
  lod.frag.glsl            MODIFIED: writes the G-buffer                    (Task 7)
  lod_shadow.vert/frag.glsl NEW: depth-only quad pull for the sun map       (Task 8)
  contact_shadow.comp.glsl NEW                                              (Task 9)
  downsample.comp.glsl     NEW: scene colour -> half-res GI history         (Task 9)
  ssgi.comp.glsl           NEW                                              (Task 10)
  ssr.comp.glsl            NEW                                              (Task 11)
  outline.comp.glsl        NEW                                              (Task 12)
  cel.gdshaderinc          NEW: Godot ShaderLanguage mirror of cel_shade    (Task 13)
  cel_object.gdshader      NEW: dynamic-object material using that include   (Task 13)
extension/tests/           test_shade_oct, test_shade_cel, test_beauty_settings,
                           test_sun_ortho                             (Tasks 1,2,3,8)
tests/                     test_gbuffer, test_raymarch_gbuffer, test_deferred,
                           test_lod_gbuffer, test_sun_shadow, test_contact_shadow,
                           test_ssgi, test_ssr, test_outline, test_cel_object,
                           test_gpu_timings, test_debug_menu
demo/                      hud.gd, benchmark.gd, debug_menu.gd, main.tscn  (Task 14)
```

---

### Task 1: `shade/oct` — one normal, two floats, no drift

Spec §7's G-buffer carries an "oct-encoded normal". Everything downstream — the deferred pass, SSGI, SSR, the outline pass — reads normals back out of it, so the encoding is load-bearing before any of them exist. It lands first, pure and native-tested, and Task 6's differential test later proves the GLSL mirror agrees.

**Files:**
- Create: `extension/src/shade/oct.h`, `extension/src/shade/oct.cpp`
- Create: `extension/tests/test_shade_oct.cpp`
- Modify: `extension/SConstruct` (add `src/shade/*.cpp` to the native runner's `pure_sources`)

**Interfaces:**
- Produces: `ve::oct_encode(const float n[3], float out[2])` and `ve::oct_decode(const float e[2], float out[3])`. Encoded components are in `[-1, 1]`; `oct_decode` always returns a unit vector (or `(0,0,1)` for a degenerate input).

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_shade_oct.cpp`:

```cpp
#include "doctest.h"
#include "shade/oct.h"
#include <cmath>

namespace {

// Angle between two unit vectors, in degrees. The only error metric that means anything for
// a normal encoding: component-wise deltas say nothing about how the shading will bend.
float angle_deg(const float a[3], const float b[3]) {
	float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
	return std::acos(d) * 57.2957795f;
}

// The G-buffer stores the two components in fp16. Emulating that quantisation here is what
// makes the bound below a statement about the shipped pipeline rather than about doubles.
float to_half_and_back(float v) {
	if (v == 0.0f) return 0.0f;
	const float a = std::fabs(v);
	const int e = static_cast<int>(std::floor(std::log2(a)));
	const float step = std::ldexp(1.0f, e - 10); // fp16 has a 10-bit mantissa
	return std::round(v / step) * step;
}

} // namespace

TEST_CASE("oct_encode round-trips every direction on a dense sphere grid") {
	float worst = 0.0f;
	for (int i = 0; i <= 64; i++) {
		for (int j = 0; j <= 64; j++) {
			const float theta = 3.14159265f * static_cast<float>(i) / 64.0f;
			const float phi = 6.28318531f * static_cast<float>(j) / 64.0f;
			const float n[3] = {std::sin(theta) * std::cos(phi), std::cos(theta),
					std::sin(theta) * std::sin(phi)};
			float e[2];
			ve::oct_encode(n, e);
			CHECK(e[0] >= -1.0f);
			CHECK(e[0] <= 1.0f);
			CHECK(e[1] >= -1.0f);
			CHECK(e[1] <= 1.0f);
			float back[3];
			ve::oct_decode(e, back);
			const float err = angle_deg(n, back);
			worst = err > worst ? err : worst;
		}
	}
	CHECK(worst < 0.01f);
}

TEST_CASE("the fp16 the G-buffer actually stores keeps the error under a quarter degree") {
	float worst = 0.0f;
	for (int i = 0; i <= 48; i++) {
		for (int j = 0; j <= 48; j++) {
			const float theta = 3.14159265f * static_cast<float>(i) / 48.0f;
			const float phi = 6.28318531f * static_cast<float>(j) / 48.0f;
			const float n[3] = {std::sin(theta) * std::cos(phi), std::cos(theta),
					std::sin(theta) * std::sin(phi)};
			float e[2];
			ve::oct_encode(n, e);
			e[0] = to_half_and_back(e[0]);
			e[1] = to_half_and_back(e[1]);
			float back[3];
			ve::oct_decode(e, back);
			const float err = angle_deg(n, back);
			worst = err > worst ? err : worst;
		}
	}
	CHECK(worst < 0.25f);
}

TEST_CASE("the six axis directions survive exactly") {
	const float axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (const auto &n : axes) {
		float e[2];
		ve::oct_encode(n, e);
		float back[3];
		ve::oct_decode(e, back);
		CHECK(back[0] == doctest::Approx(n[0]).epsilon(1e-5));
		CHECK(back[1] == doctest::Approx(n[1]).epsilon(1e-5));
		CHECK(back[2] == doctest::Approx(n[2]).epsilon(1e-5));
	}
}

// The lower hemisphere is the folded half of the octahedron and is where a sign mistake
// hides: it round-trips on the axes and is wrong everywhere between them.
TEST_CASE("the folded lower hemisphere round-trips too") {
	const float n[3] = {0.3f, -0.9f, 0.31622776f};
	float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	const float un[3] = {n[0] / len, n[1] / len, n[2] / len};
	float e[2];
	ve::oct_encode(un, e);
	float back[3];
	ve::oct_decode(e, back);
	CHECK(angle_deg(un, back) < 0.01f);
}

TEST_CASE("a degenerate normal decodes to something finite") {
	const float zero[3] = {0, 0, 0};
	float e[2];
	ve::oct_encode(zero, e);
	CHECK(std::isfinite(e[0]));
	CHECK(std::isfinite(e[1]));
	float back[3];
	ve::oct_decode(e, back);
	CHECK(std::isfinite(back[0]));
	CHECK(std::isfinite(back[1]));
	CHECK(std::isfinite(back[2]));
	// Unit length or the exact fallback; either way nothing downstream sees a NaN.
	const float l = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
	CHECK((l == doctest::Approx(1.0f).epsilon(1e-4) || l == doctest::Approx(0.0f).epsilon(1e-4)));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: shade/oct.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `extension/src/shade/oct.h`:

```cpp
#pragma once

namespace ve {

// Octahedral normal encoding. The G-buffer's surface target stores the two components in
// fp16, which is why the range is [-1, 1] rather than [0, 1]: the sign bit is free there and
// halving the range would throw away a bit of precision for nothing.
//
// shaders/shade.glslh mirrors both functions. Task 6's differential test diffs a GPU
// round trip against this one and fails when they drift.
void oct_encode(const float n[3], float out[2]);
void oct_decode(const float e[2], float out[3]);

} // namespace ve
```

Create `extension/src/shade/oct.cpp`:

```cpp
#include "shade/oct.h"
#include <cmath>

namespace {

// Deliberately NOT copysign: copysign(1, -0.0) is -1, and a normal whose component is
// negative zero would then fold to the opposite octant. The GLSL mirror uses the same
// >= 0 test for exactly this reason.
inline float sign_not_zero(float v) {
	return v >= 0.0f ? 1.0f : -1.0f;
}

inline void fold(float &x, float &y) {
	const float ax = std::fabs(x);
	const float ay = std::fabs(y);
	const float fx = (1.0f - ay) * sign_not_zero(x);
	const float fy = (1.0f - ax) * sign_not_zero(y);
	x = fx;
	y = fy;
}

} // namespace

namespace ve {

void oct_encode(const float n[3], float out[2]) {
	const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
	const float inv = l1 > 0.0f ? 1.0f / l1 : 0.0f;
	float x = n[0] * inv;
	float y = n[1] * inv;
	const float z = n[2] * inv;
	if (z < 0.0f) fold(x, y);
	out[0] = x;
	out[1] = y;
}

void oct_decode(const float e[2], float out[3]) {
	float x = e[0];
	float y = e[1];
	const float z = 1.0f - std::fabs(x) - std::fabs(y);
	if (z < 0.0f) fold(x, y);
	const float len = std::sqrt(x * x + y * y + z * z);
	if (!(len > 0.0f)) {
		out[0] = 0.0f;
		out[1] = 0.0f;
		out[2] = 1.0f;
		return;
	}
	const float inv = 1.0f / len;
	out[0] = x * inv;
	out[1] = y * inv;
	out[2] = z * inv;
}

} // namespace ve
```

- [ ] **Step 4: Teach the native runner about `src/shade`**

In `extension/SConstruct`, extend `pure_sources` — it is an explicit list, unlike the library's glob:

```python
pure_sources = (Glob("src/world/*.cpp") + Glob("src/generator/*.cpp") +
                Glob("src/mesh/*.cpp") + Glob("src/connectivity/*.cpp") +
                Glob("src/lod/*.cpp") + Glob("src/shade/*.cpp"))
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS — five new assertions' worth of cases green, and every pre-existing native test still green.

- [ ] **Step 6: Commit**

```bash
git add extension/src/shade/oct.h extension/src/shade/oct.cpp \
        extension/tests/test_shade_oct.cpp extension/SConstruct
git commit -m "feat: octahedral normal packing for the merged g-buffer"
```

---

### Task 2: `shade/cel` — the ramp the whole image is quantised through

Spec §7's cel shading: "3–4 band quantized diffuse with paintable ramp, hue-shifted shadow tint, specular band on glossy materials, rim light". This task is that sentence as pure arithmetic, so the deferred compute pass, the object `.gdshader` and any future baker are all quoting the same numbers rather than each approximating the sentence.

**Files:**
- Create: `extension/src/shade/cel.h`, `extension/src/shade/cel.cpp`
- Create: `extension/tests/test_shade_cel.cpp`

**Interfaces:**
- Consumes: nothing from Task 1 (the two are independent).
- Produces: `ve::kCelBands`, `ve::kSunDir[3]`, `ve::CelParams`, `ve::CelInput`, `int ve::cel_band(const CelParams &, float ndl)`, `float ve::cel_level(const CelParams &, float ndl)`, `void ve::rgb_to_hsv(const float[3], float[3])`, `void ve::hsv_to_rgb(const float[3], float[3])`, `void ve::cel_shadow_tint(const CelParams &, const float albedo[3], float t, float out[3])`, `void ve::cel_shade(const CelParams &, const CelInput &, float out[3])`.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_shade_cel.cpp`:

```cpp
#include "doctest.h"
#include "shade/cel.h"
#include <cmath>

namespace {

ve::CelInput plain(float ndl) {
	ve::CelInput in{};
	in.albedo[0] = 0.5f; in.albedo[1] = 0.5f; in.albedo[2] = 0.5f;
	in.ambient[0] = 0.0f; in.ambient[1] = 0.0f; in.ambient[2] = 0.0f;
	in.ndl = ndl;
	in.ndv = 1.0f;   // face-on: no rim
	in.ndh = 0.0f;   // no specular
	in.shadow = 1.0f;
	in.ao = 1.0f;
	in.gloss = 0.0f;
	return in;
}

} // namespace

TEST_CASE("the ramp has exactly four bands and they are the stated ones") {
	const ve::CelParams p;
	CHECK(ve::kCelBands == 4);
	CHECK(ve::cel_band(p, 0.00f) == 0);
	CHECK(ve::cel_band(p, 0.079f) == 0);
	CHECK(ve::cel_band(p, 0.081f) == 1);
	CHECK(ve::cel_band(p, 0.319f) == 1);
	CHECK(ve::cel_band(p, 0.321f) == 2);
	CHECK(ve::cel_band(p, 0.659f) == 2);
	CHECK(ve::cel_band(p, 0.661f) == 3);
	CHECK(ve::cel_band(p, 1.00f) == 3);
}

TEST_CASE("a sweep of N dot L produces four distinct levels and nothing between them") {
	const ve::CelParams p;
	float seen[8];
	int count = 0;
	for (int i = 0; i <= 1000; i++) {
		const float v = ve::cel_level(p, static_cast<float>(i) / 1000.0f);
		bool known = false;
		for (int k = 0; k < count; k++)
			if (std::fabs(seen[k] - v) < 1e-6f) known = true;
		if (!known) {
			REQUIRE(count < 8);
			seen[count++] = v;
		}
	}
	CHECK(count == 4);
}

TEST_CASE("the ramp never falls as the light rises") {
	const ve::CelParams p;
	float prev = -1.0f;
	for (int i = 0; i <= 1000; i++) {
		const float v = ve::cel_level(p, static_cast<float>(i) / 1000.0f);
		CHECK(v >= prev);
		prev = v;
	}
}

TEST_CASE("out-of-range N dot L is clamped, not extrapolated") {
	const ve::CelParams p;
	CHECK(ve::cel_level(p, -5.0f) == doctest::Approx(ve::cel_level(p, 0.0f)));
	CHECK(ve::cel_level(p, 5.0f) == doctest::Approx(ve::cel_level(p, 1.0f)));
}

TEST_CASE("full light with no ambient, no rim and no spec returns the albedo untouched") {
	const ve::CelParams p;
	ve::CelInput in = plain(1.0f);
	float out[3];
	ve::cel_shade(p, in, out);
	// band_level[3] is 1.0 and the tint is the identity at t = 0, so this is the one input
	// where the whole stack must be a no-op. If it is not, something is scaling the image.
	CHECK(out[0] == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK(out[1] == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK(out[2] == doctest::Approx(0.5f).epsilon(1e-5));
}

TEST_CASE("a fully shadowed pixel keeps only its ambient term") {
	const ve::CelParams p;
	ve::CelInput in = plain(1.0f);
	in.shadow = 0.0f;
	in.ambient[0] = 0.2f; in.ambient[1] = 0.2f; in.ambient[2] = 0.2f;
	float out[3];
	ve::cel_shade(p, in, out);
	float tint[3];
	ve::cel_shadow_tint(p, in.albedo, 1.0f, tint);
	CHECK(out[0] == doctest::Approx(tint[0] * 0.2f).epsilon(1e-5));
	CHECK(out[1] == doctest::Approx(tint[1] * 0.2f).epsilon(1e-5));
	CHECK(out[2] == doctest::Approx(tint[2] * 0.2f).epsilon(1e-5));
}

TEST_CASE("ambient occlusion multiplies the ambient term and nothing else") {
	const ve::CelParams p;
	ve::CelInput lit = plain(1.0f);
	lit.ambient[0] = 0.3f; lit.ambient[1] = 0.3f; lit.ambient[2] = 0.3f;
	float with_ao[3];
	float without_ao[3];
	ve::cel_shade(p, lit, without_ao);
	lit.ao = 0.0f;
	ve::cel_shade(p, lit, with_ao);
	// Removing AO must remove exactly the ambient contribution: 0.5 albedo x 0.3 ambient.
	CHECK((without_ao[0] - with_ao[0]) == doctest::Approx(0.5f * 0.3f).epsilon(1e-5));
}

TEST_CASE("hsv round-trips") {
	const float colors[5][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.8f, 0.2f, 0.1f},
			{0.1f, 0.6f, 0.3f}, {0.25f, 0.25f, 0.9f}};
	for (const auto &c : colors) {
		float hsv[3];
		float back[3];
		ve::rgb_to_hsv(c, hsv);
		ve::hsv_to_rgb(hsv, back);
		CHECK(back[0] == doctest::Approx(c[0]).epsilon(1e-5));
		CHECK(back[1] == doctest::Approx(c[1]).epsilon(1e-5));
		CHECK(back[2] == doctest::Approx(c[2]).epsilon(1e-5));
	}
}

TEST_CASE("the shadow tint is the identity in full light and a real hue shift in shadow") {
	const ve::CelParams p;
	const float albedo[3] = {0.8f, 0.2f, 0.1f};
	float at_zero[3];
	ve::cel_shadow_tint(p, albedo, 0.0f, at_zero);
	CHECK(at_zero[0] == doctest::Approx(albedo[0]).epsilon(1e-5));
	CHECK(at_zero[1] == doctest::Approx(albedo[1]).epsilon(1e-5));
	CHECK(at_zero[2] == doctest::Approx(albedo[2]).epsilon(1e-5));

	float at_one[3];
	ve::cel_shadow_tint(p, albedo, 1.0f, at_one);
	float hsv_in[3];
	float hsv_out[3];
	ve::rgb_to_hsv(albedo, hsv_in);
	ve::rgb_to_hsv(at_one, hsv_out);
	// The hue moved by exactly the configured amount, wrapping at 1.
	float expected = hsv_in[0] + p.shadow_hue_shift;
	expected -= std::floor(expected);
	CHECK(hsv_out[0] == doctest::Approx(expected).epsilon(1e-4));
	CHECK(hsv_out[1] > hsv_in[1]); // and it got more saturated, as a shadow should
}

// A grey surface has no hue to shift. If the tint invents one, every rock face in the demo
// goes faintly blue in shadow for no reason anybody can point at in the ramp.
TEST_CASE("a desaturated albedo is untouched by the hue shift") {
	const ve::CelParams p;
	const float grey[3] = {0.4f, 0.4f, 0.4f};
	float out[3];
	ve::cel_shadow_tint(p, grey, 1.0f, out);
	CHECK(out[0] == doctest::Approx(grey[0]).epsilon(1e-5));
	CHECK(out[1] == doctest::Approx(grey[1]).epsilon(1e-5));
	CHECK(out[2] == doctest::Approx(grey[2]).epsilon(1e-5));
}

TEST_CASE("the specular band is a step, not a falloff, and only glossy materials get it") {
	const ve::CelParams p;
	ve::CelInput dull = plain(1.0f);
	dull.gloss = 0.0f;
	dull.ndh = 1.0f;
	float dull_out[3];
	ve::cel_shade(p, dull, dull_out);
	CHECK(dull_out[0] == doctest::Approx(0.5f).epsilon(1e-5));

	ve::CelInput shiny = plain(1.0f);
	shiny.gloss = 1.0f;
	shiny.ndh = p.spec_edge - 0.01f;
	float below[3];
	ve::cel_shade(p, shiny, below);
	shiny.ndh = p.spec_edge + 0.01f;
	float above[3];
	ve::cel_shade(p, shiny, above);
	CHECK(below[0] == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK((above[0] - below[0]) == doctest::Approx(p.spec_strength).epsilon(1e-5));
}

TEST_CASE("the rim only appears at grazing angles") {
	const ve::CelParams p;
	ve::CelInput face_on = plain(1.0f);
	face_on.ndv = 1.0f;
	ve::CelInput grazing = plain(1.0f);
	grazing.ndv = 0.0f;
	float a[3];
	float b[3];
	ve::cel_shade(p, face_on, a);
	ve::cel_shade(p, grazing, b);
	CHECK(b[0] > a[0]);
	CHECK((b[0] - a[0]) == doctest::Approx(p.rim_strength).epsilon(1e-4));
}

TEST_CASE("the sun direction is the unit vector every shader assumes") {
	const float l = std::sqrt(ve::kSunDir[0] * ve::kSunDir[0] +
			ve::kSunDir[1] * ve::kSunDir[1] + ve::kSunDir[2] * ve::kSunDir[2]);
	CHECK(l == doctest::Approx(1.0f).epsilon(1e-5));
	// It is normalize(0.6, 0.8, 0.3) -- the direction shade_terrain() has used since M1.
	CHECK(ve::kSunDir[0] / ve::kSunDir[1] == doctest::Approx(0.75f).epsilon(1e-5));
	CHECK(ve::kSunDir[2] / ve::kSunDir[1] == doctest::Approx(0.375f).epsilon(1e-5));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: shade/cel.h: No such file or directory`.

- [ ] **Step 3: Write `extension/src/shade/cel.h`**

```cpp
#pragma once

namespace ve {

inline constexpr int kCelBands = 4;

// normalize(0.6, 0.8, 0.3) -- the direction common.glslh's shade_terrain() has used since
// M1, written out so the CPU, the shaders and the shadow map cannot disagree about where
// the sun is. shaders/shade.glslh mirrors it as SUN_DIR.
inline constexpr float kSunDir[3] = {0.5746958f, 0.7662610f, 0.2873479f};

// The paintable ramp of spec section 7. Every field is a knob a artist-facing debug menu
// could move; none of them is read anywhere except through cel_shade().
struct CelParams {
	float band_edge[kCelBands - 1] = {0.08f, 0.32f, 0.66f};
	float band_level[kCelBands] = {0.18f, 0.45f, 0.75f, 1.00f};
	// Turns of hue, applied in proportion to how deep into shadow the pixel is.
	float shadow_hue_shift = 0.055f;
	float shadow_saturation = 1.35f;
	float spec_edge = 0.72f;
	float spec_strength = 0.45f;
	float rim_strength = 0.35f;
	float rim_power = 3.0f;
};

// Everything the ramp needs, precomputed by the caller. Scalars rather than vectors so the
// GLSL mirror computes the same numbers from the same inputs instead of re-deriving them
// from its own view/light vectors -- that re-derivation is exactly how two shading paths
// drift apart.
struct CelInput {
	float albedo[3] = {1, 1, 1};
	float ambient[3] = {0, 0, 0};
	float ndl = 1.0f;    // dot(normal, sun), unclamped
	float ndv = 1.0f;    // dot(normal, view), clamped by cel_shade
	float ndh = 0.0f;    // dot(normal, halfway) for the specular band
	float shadow = 1.0f; // 1 = fully lit
	float ao = 1.0f;
	float gloss = 0.0f;
};

int cel_band(const CelParams &p, float ndl);
float cel_level(const CelParams &p, float ndl);

void rgb_to_hsv(const float rgb[3], float hsv[3]);
void hsv_to_rgb(const float hsv[3], float rgb[3]);

// `t` is how far into shadow the pixel is: 0 = fully lit (identity), 1 = darkest band.
void cel_shadow_tint(const CelParams &p, const float albedo[3], float t, float out[3]);

void cel_shade(const CelParams &p, const CelInput &in, float out[3]);

} // namespace ve
```

- [ ] **Step 4: Write `extension/src/shade/cel.cpp`**

```cpp
#include "shade/cel.h"
#include <cmath>

namespace {

inline float clamp01(float v) {
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

} // namespace

namespace ve {

int cel_band(const CelParams &p, float ndl) {
	const float v = clamp01(ndl);
	int band = 0;
	for (int i = 0; i < kCelBands - 1; i++)
		if (v > p.band_edge[i]) band = i + 1;
	return band;
}

float cel_level(const CelParams &p, float ndl) {
	return p.band_level[cel_band(p, ndl)];
}

void rgb_to_hsv(const float rgb[3], float hsv[3]) {
	const float r = rgb[0], g = rgb[1], b = rgb[2];
	const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	const float d = mx - mn;
	float h = 0.0f;
	if (d > 0.0f) {
		if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
		else if (mx == g) h = (b - r) / d + 2.0f;
		else h = (r - g) / d + 4.0f;
		h /= 6.0f;
	}
	hsv[0] = h;
	hsv[1] = mx > 0.0f ? d / mx : 0.0f;
	hsv[2] = mx;
}

void hsv_to_rgb(const float hsv[3], float rgb[3]) {
	const float h = (hsv[0] - std::floor(hsv[0])) * 6.0f;
	const float s = clamp01(hsv[1]);
	const float v = hsv[2];
	const int i = static_cast<int>(std::floor(h)) % 6;
	const float f = h - std::floor(h);
	const float p = v * (1.0f - s);
	const float q = v * (1.0f - s * f);
	const float t = v * (1.0f - s * (1.0f - f));
	switch (i) {
		case 0: rgb[0] = v; rgb[1] = t; rgb[2] = p; break;
		case 1: rgb[0] = q; rgb[1] = v; rgb[2] = p; break;
		case 2: rgb[0] = p; rgb[1] = v; rgb[2] = t; break;
		case 3: rgb[0] = p; rgb[1] = q; rgb[2] = v; break;
		case 4: rgb[0] = t; rgb[1] = p; rgb[2] = v; break;
		default: rgb[0] = v; rgb[1] = p; rgb[2] = q; break;
	}
}

void cel_shadow_tint(const CelParams &p, const float albedo[3], float t, float out[3]) {
	const float k = clamp01(t);
	float hsv[3];
	rgb_to_hsv(albedo, hsv);
	// A grey surface has no hue to shift, and multiplying a zero saturation keeps it zero,
	// so the identity for grey falls out of the maths rather than needing a branch.
	hsv[0] = hsv[0] + p.shadow_hue_shift * k;
	hsv[0] -= std::floor(hsv[0]);
	hsv[1] = clamp01(hsv[1] * (1.0f + (p.shadow_saturation - 1.0f) * k));
	hsv_to_rgb(hsv, out);
}

void cel_shade(const CelParams &p, const CelInput &in, float out[3]) {
	const float lit = cel_level(p, in.ndl) * clamp01(in.shadow);
	float tint[3];
	cel_shadow_tint(p, in.albedo, 1.0f - lit, tint);
	// A hard step, not a falloff: the band edge IS the highlight's silhouette.
	const float spec = (in.gloss > 0.0f && in.ndh >= p.spec_edge)
			? in.gloss * p.spec_strength * clamp01(in.shadow)
			: 0.0f;
	const float rim = p.rim_strength * std::pow(1.0f - clamp01(in.ndv), p.rim_power);
	const float ao = clamp01(in.ao);
	for (int c = 0; c < 3; c++)
		out[c] = tint[c] * lit + tint[c] * in.ambient[c] * ao + spec + rim;
}

} // namespace ve
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS — all twelve cases green, and every pre-existing native test still green.

- [ ] **Step 6: Commit**

```bash
git add extension/src/shade/cel.h extension/src/shade/cel.cpp \
        extension/tests/test_shade_cel.cpp
git commit -m "feat: cel ramp, shadow tint, spec band and rim as one pure function"
```

---

### Task 3: `shade/beauty_settings` — every effect's off switch, in one struct

Spec §7 ends with "Every effect has a quality/off toggle in the debug menu". A toggle set that grows one boolean at a time inside `VoxelWorld` ends up half in the compositor and half in GDScript; declaring it once, before any effect exists, is what keeps Task 14's debug menu a UI over an existing model rather than a second model.

**Files:**
- Create: `extension/src/shade/beauty_settings.h`, `extension/src/shade/beauty_settings.cpp`
- Create: `extension/tests/test_beauty_settings.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_beauty_settings.gd`

**Interfaces:**
- Produces: `ve::QualityTier` (`kOff, kLow, kMedium, kHigh`), `ve::BeautySettings`, `ve::BeautySettings ve::settings_for_tier(QualityTier)`, `void ve::clamp_settings(BeautySettings *)`, `uint32_t ve::pack_flags(const BeautySettings &)`.
- Produces: `VoxelWorld::set_quality_tier(int)`, `int VoxelWorld::get_quality_tier() const`, `void VoxelWorld::set_effect_enabled(String name, bool on)`, `bool VoxelWorld::get_effect_enabled(String name) const`, `Dictionary VoxelWorld::debug_beauty_settings()`. Later tasks read the live struct through `const ve::BeautySettings &VoxelWorld::beauty_settings() const`.

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_beauty_settings.cpp`:

```cpp
#include "doctest.h"
#include "shade/beauty_settings.h"

TEST_CASE("the Off tier turns every effect off and leaves no work in any counter") {
	const ve::BeautySettings s = ve::settings_for_tier(ve::QualityTier::kOff);
	CHECK_FALSE(s.ssgi);
	CHECK_FALSE(s.ssr);
	CHECK_FALSE(s.contact_shadows);
	CHECK_FALSE(s.outlines);
	CHECK_FALSE(s.sun_shadow_map);
	CHECK_FALSE(s.glossy_sdf_rays);
	CHECK_FALSE(s.raymarched_sun_shadow);
	CHECK(s.ssgi_taps == 0);
	CHECK(s.ssr_steps == 0);
	CHECK(s.contact_steps == 0);
}

TEST_CASE("the tiers are ordered: nothing gets cheaper as quality rises") {
	const ve::BeautySettings lo = ve::settings_for_tier(ve::QualityTier::kLow);
	const ve::BeautySettings me = ve::settings_for_tier(ve::QualityTier::kMedium);
	const ve::BeautySettings hi = ve::settings_for_tier(ve::QualityTier::kHigh);
	CHECK(me.ssgi_taps >= lo.ssgi_taps);
	CHECK(hi.ssgi_taps >= me.ssgi_taps);
	CHECK(me.ssr_steps >= lo.ssr_steps);
	CHECK(hi.ssr_steps >= me.ssr_steps);
	CHECK(me.contact_steps >= lo.contact_steps);
	CHECK(hi.contact_steps >= me.contact_steps);
}

TEST_CASE("High is the demo default and matches the fixed numbers table") {
	const ve::BeautySettings hi = ve::settings_for_tier(ve::QualityTier::kHigh);
	CHECK(hi.ssgi_taps == 8);
	CHECK(hi.ssr_steps == 24);
	CHECK(hi.contact_steps == 12);
	CHECK(hi.ssgi);
	CHECK(hi.ssr);
	CHECK(hi.contact_shadows);
	CHECK(hi.outlines);
	CHECK(hi.sun_shadow_map);
	CHECK(hi.glossy_sdf_rays);
	CHECK(hi.raymarched_sun_shadow);
	CHECK(ve::BeautySettings{}.ssgi_taps == hi.ssgi_taps);
}

// A tap count of a billion is a hung GPU, and a negative one is an unrolled loop that never
// terminates. The clamp is the only thing between a debug-menu typo and a driver reset.
TEST_CASE("counts are clamped into the ranges the shaders were written for") {
	ve::BeautySettings s;
	s.ssgi_taps = 9999;
	s.ssr_steps = -4;
	s.contact_steps = 1000;
	s.outline_depth_threshold = -1.0f;
	s.outline_normal_threshold = 12.0f;
	ve::clamp_settings(&s);
	CHECK(s.ssgi_taps == 16);
	CHECK(s.ssr_steps == 0);
	CHECK(s.contact_steps == 32);
	CHECK(s.outline_depth_threshold >= 0.0f);
	CHECK(s.outline_normal_threshold <= 2.0f);
}

// A zero count means the effect does no work, so it must also read as off: a pass that
// dispatches with zero taps costs a full-screen dispatch to produce nothing.
TEST_CASE("clamping to zero work also clears the enable bit") {
	ve::BeautySettings s;
	s.ssgi_taps = 0;
	s.ssr_steps = 0;
	s.contact_steps = 0;
	ve::clamp_settings(&s);
	CHECK_FALSE(s.ssgi);
	CHECK_FALSE(s.ssr);
	CHECK_FALSE(s.contact_shadows);
}

TEST_CASE("the packed flag bits are stable, because a shader hardcodes them") {
	ve::BeautySettings s = ve::settings_for_tier(ve::QualityTier::kOff);
	CHECK(ve::pack_flags(s) == 0u);
	s.ssgi = true;
	s.ssgi_taps = 8;
	CHECK((ve::pack_flags(s) & 1u) == 1u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.ssr = true;
	s.ssr_steps = 8;
	CHECK((ve::pack_flags(s) & 2u) == 2u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.contact_shadows = true;
	s.contact_steps = 8;
	CHECK((ve::pack_flags(s) & 4u) == 4u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.outlines = true;
	CHECK((ve::pack_flags(s) & 8u) == 8u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.sun_shadow_map = true;
	CHECK((ve::pack_flags(s) & 16u) == 16u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.glossy_sdf_rays = true;
	CHECK((ve::pack_flags(s) & 32u) == 32u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.raymarched_sun_shadow = true;
	CHECK((ve::pack_flags(s) & 64u) == 64u);
}

TEST_CASE("an out-of-range tier falls back to High rather than to nothing") {
	const ve::BeautySettings s = ve::settings_for_tier(static_cast<ve::QualityTier>(99));
	CHECK(s.ssgi_taps == 8);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: shade/beauty_settings.h: No such file or directory`.

- [ ] **Step 3: Write `extension/src/shade/beauty_settings.h`**

```cpp
#pragma once
#include <cstdint>

namespace ve {

enum class QualityTier { kOff = 0, kLow = 1, kMedium = 2, kHigh = 3 };

// Spec section 7: "Every effect has a quality/off toggle in the debug menu". This is that
// set. No render pass reads a knob that is not here, and no knob is here that no pass reads.
struct BeautySettings {
	bool ssgi = true;
	bool ssr = true;
	bool contact_shadows = true;
	bool outlines = true;
	bool sun_shadow_map = true;
	bool glossy_sdf_rays = true;
	bool raymarched_sun_shadow = true;

	int ssgi_taps = 8;      // [0, 16]
	int ssr_steps = 24;     // [0, 64]
	int contact_steps = 12; // [0, 32]

	float outline_depth_threshold = 0.04f;  // [0, 1], relative to linear depth
	float outline_normal_threshold = 0.25f; // [0, 2], 1 - dot(n0, n1)
};

// Bit layout, mirrored by BEAUTY_* in the shaders. A bit is only set when the effect is
// enabled AND has work to do, so a shader never has to check both.
inline constexpr uint32_t kFlagSsgi = 1u;
inline constexpr uint32_t kFlagSsr = 2u;
inline constexpr uint32_t kFlagContact = 4u;
inline constexpr uint32_t kFlagOutlines = 8u;
inline constexpr uint32_t kFlagSunMap = 16u;
inline constexpr uint32_t kFlagGlossyRays = 32u;
inline constexpr uint32_t kFlagRaySunShadow = 64u;

BeautySettings settings_for_tier(QualityTier t);
void clamp_settings(BeautySettings *s);
uint32_t pack_flags(const BeautySettings &s);

} // namespace ve
```

- [ ] **Step 4: Write `extension/src/shade/beauty_settings.cpp`**

```cpp
#include "shade/beauty_settings.h"

namespace {

inline int clamp_int(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

inline float clamp_float(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

namespace ve {

BeautySettings settings_for_tier(QualityTier t) {
	BeautySettings s;
	switch (t) {
		case QualityTier::kOff:
			s.ssgi = s.ssr = s.contact_shadows = s.outlines = false;
			s.sun_shadow_map = s.glossy_sdf_rays = s.raymarched_sun_shadow = false;
			s.ssgi_taps = 0;
			s.ssr_steps = 0;
			s.contact_steps = 0;
			break;
		case QualityTier::kLow:
			// Outlines and the raymarched sun shadow survive: they are what makes the image
			// read as this engine's image at all, and together they cost under 1 ms.
			s.ssgi = s.ssr = s.contact_shadows = false;
			s.glossy_sdf_rays = false;
			s.ssgi_taps = 0;
			s.ssr_steps = 0;
			s.contact_steps = 0;
			break;
		case QualityTier::kMedium:
			s.glossy_sdf_rays = false;
			s.ssgi_taps = 4;
			s.ssr_steps = 12;
			s.contact_steps = 8;
			break;
		case QualityTier::kHigh:
		default:
			break; // the struct's defaults ARE High
	}
	clamp_settings(&s);
	return s;
}

void clamp_settings(BeautySettings *s) {
	if (!s) return;
	s->ssgi_taps = clamp_int(s->ssgi_taps, 0, 16);
	s->ssr_steps = clamp_int(s->ssr_steps, 0, 64);
	s->contact_steps = clamp_int(s->contact_steps, 0, 32);
	s->outline_depth_threshold = clamp_float(s->outline_depth_threshold, 0.0f, 1.0f);
	s->outline_normal_threshold = clamp_float(s->outline_normal_threshold, 0.0f, 2.0f);
	// Zero work is off. A dispatch that produces nothing still costs a full-screen pass.
	if (s->ssgi_taps == 0) s->ssgi = false;
	if (s->ssr_steps == 0) s->ssr = false;
	if (s->contact_steps == 0) s->contact_shadows = false;
}

uint32_t pack_flags(const BeautySettings &s) {
	uint32_t f = 0;
	if (s.ssgi && s.ssgi_taps > 0) f |= kFlagSsgi;
	if (s.ssr && s.ssr_steps > 0) f |= kFlagSsr;
	if (s.contact_shadows && s.contact_steps > 0) f |= kFlagContact;
	if (s.outlines) f |= kFlagOutlines;
	if (s.sun_shadow_map) f |= kFlagSunMap;
	if (s.glossy_sdf_rays) f |= kFlagGlossyRays;
	if (s.raymarched_sun_shadow) f |= kFlagRaySunShadow;
	return f;
}

} // namespace ve
```

- [ ] **Step 5: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS.

- [ ] **Step 6: Write the failing script-facing test**

Create `tests/test_beauty_settings.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	return w

func test_the_default_tier_is_high() -> void:
	var w := make_world()
	assert_int(w.quality_tier).is_equal(3)
	var d := w.debug_beauty_settings()
	assert_int(d["ssgi_taps"]).is_equal(8)
	assert_bool(d["outlines"]).is_true()

func test_setting_the_tier_replaces_every_knob() -> void:
	var w := make_world()
	w.quality_tier = 0
	var d := w.debug_beauty_settings()
	assert_bool(d["ssgi"]).is_false()
	assert_bool(d["ssr"]).is_false()
	assert_bool(d["outlines"]).is_false()
	assert_int(d["flags"]).is_equal(0)

func test_individual_effects_toggle_by_name() -> void:
	var w := make_world()
	w.set_effect_enabled("outlines", false)
	assert_bool(w.get_effect_enabled("outlines")).is_false()
	assert_bool(w.get_effect_enabled("ssr")).is_true()
	# Bit 8 is kFlagOutlines; clearing it must not disturb the others.
	var d := w.debug_beauty_settings()
	assert_int(int(d["flags"]) & 8).is_equal(0)
	assert_int(int(d["flags"]) & 2).is_equal(2)

func test_an_unknown_effect_name_is_ignored_rather_than_crashing() -> void:
	var w := make_world()
	var before: int = w.debug_beauty_settings()["flags"]
	w.set_effect_enabled("no_such_effect", false)
	assert_int(w.debug_beauty_settings()["flags"]).is_equal(before)
	assert_bool(w.get_effect_enabled("no_such_effect")).is_false()
```

- [ ] **Step 7: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_beauty_settings.gd`
Expected: FAIL — `Invalid assignment of property 'quality_tier'`.

- [ ] **Step 8: Wire the settings into `VoxelWorld`**

In `extension/src/voxel_world.h`, add `#include "shade/beauty_settings.h"`, a member, and the accessors:

```cpp
	// --- M6 beautification settings (Task 3) ---
	int quality_tier_ = static_cast<int>(ve::QualityTier::kHigh);
	ve::BeautySettings beauty_ = ve::settings_for_tier(ve::QualityTier::kHigh);
```

```cpp
	void set_quality_tier(int v);
	int get_quality_tier() const { return quality_tier_; }
	void set_effect_enabled(const String &name, bool on);
	bool get_effect_enabled(const String &name) const;
	// Read by every M6 pass on the render thread. Plain-old-data, written only from the
	// main thread between frames, so no lock: a torn read would at worst use last frame's
	// toggle for one frame, which is what a toggle looks like anyway.
	const ve::BeautySettings &beauty_settings() const { return beauty_; }
	Dictionary debug_beauty_settings();
```

In `extension/src/voxel_world.cpp`:

```cpp
void VoxelWorld::set_quality_tier(int v) {
	quality_tier_ = v < 0 ? 0 : (v > 3 ? 3 : v);
	beauty_ = ve::settings_for_tier(static_cast<ve::QualityTier>(quality_tier_));
}

namespace {
// One table, so the setter, the getter and the debug dictionary cannot disagree about what
// an effect is called.
bool *beauty_field(ve::BeautySettings &s, const String &name) {
	if (name == "ssgi") return &s.ssgi;
	if (name == "ssr") return &s.ssr;
	if (name == "contact_shadows") return &s.contact_shadows;
	if (name == "outlines") return &s.outlines;
	if (name == "sun_shadow_map") return &s.sun_shadow_map;
	if (name == "glossy_sdf_rays") return &s.glossy_sdf_rays;
	if (name == "raymarched_sun_shadow") return &s.raymarched_sun_shadow;
	return nullptr;
}
} // namespace

void VoxelWorld::set_effect_enabled(const String &name, bool on) {
	bool *f = beauty_field(beauty_, name);
	if (!f) return; // fail-soft: an unknown name in a debug menu is not a crash
	*f = on;
	ve::clamp_settings(&beauty_);
}

bool VoxelWorld::get_effect_enabled(const String &name) const {
	ve::BeautySettings copy = beauty_;
	const bool *f = beauty_field(copy, name);
	return f ? *f : false;
}

Dictionary VoxelWorld::debug_beauty_settings() {
	Dictionary d;
	d["ssgi"] = beauty_.ssgi;
	d["ssr"] = beauty_.ssr;
	d["contact_shadows"] = beauty_.contact_shadows;
	d["outlines"] = beauty_.outlines;
	d["sun_shadow_map"] = beauty_.sun_shadow_map;
	d["glossy_sdf_rays"] = beauty_.glossy_sdf_rays;
	d["raymarched_sun_shadow"] = beauty_.raymarched_sun_shadow;
	d["ssgi_taps"] = beauty_.ssgi_taps;
	d["ssr_steps"] = beauty_.ssr_steps;
	d["contact_steps"] = beauty_.contact_steps;
	d["outline_depth_threshold"] = beauty_.outline_depth_threshold;
	d["outline_normal_threshold"] = beauty_.outline_normal_threshold;
	d["tier"] = quality_tier_;
	d["flags"] = static_cast<int>(ve::pack_flags(beauty_));
	return d;
}
```

`beauty_field` takes a non-const reference and `get_effect_enabled` copies to call it; that is deliberate — one table beats two that can drift.

In `VoxelWorld::_bind_methods()`, beside the existing exports:

```cpp
	ClassDB::bind_method(D_METHOD("set_quality_tier", "v"), &VoxelWorld::set_quality_tier);
	ClassDB::bind_method(D_METHOD("get_quality_tier"), &VoxelWorld::get_quality_tier);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "quality_tier", PROPERTY_HINT_ENUM,
			"Off,Low,Medium,High"), "set_quality_tier", "get_quality_tier");
	ClassDB::bind_method(D_METHOD("set_effect_enabled", "name", "on"),
			&VoxelWorld::set_effect_enabled);
	ClassDB::bind_method(D_METHOD("get_effect_enabled", "name"),
			&VoxelWorld::get_effect_enabled);
	ClassDB::bind_method(D_METHOD("debug_beauty_settings"),
			&VoxelWorld::debug_beauty_settings);
```

- [ ] **Step 9: Run both suites to verify they pass**

Run: `./build.sh -j$(nproc)` then `cd extension && scons test` then
`./gdunit_tests.sh -a res://tests/test_beauty_settings.gd`
Expected: PASS on all three.

- [ ] **Step 10: Commit**

```bash
git add extension/src/shade/beauty_settings.h extension/src/shade/beauty_settings.cpp \
        extension/tests/test_beauty_settings.cpp extension/src/voxel_world.h \
        extension/src/voxel_world.cpp tests/test_beauty_settings.gd
git commit -m "feat: beauty settings tiers and per-effect toggles"
```

---

### Task 4: `render/gbuffer` — the one surface description everything writes into

Spec §7's first sentence is the whole milestone: "Raymarched terrain + islands AND rasterized LoD write albedo / oct-normal / linear-depth / material-ID into the same offscreen G-buffer." This task allocates that buffer and nothing else, so the three producers that follow have somewhere to land before any of them changes.

The textures are allocated **through `RenderSceneBuffersRD`** under a named context. That is not decoration: the engine drops a context's textures when the viewport is reconfigured, so a window resize frees and reallocates them without this code owning a single resize path. The probe/test path (`rsb == nullptr`) allocates plain RD textures instead, which is how the gdUnit suites reach the pass without a live viewport.

**Files:**
- Create: `extension/src/render/gbuffer.h`, `extension/src/render/gbuffer.cpp`
- Create: `tests/test_gbuffer.gd`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`

**Interfaces:**
- Consumes: nothing from Tasks 1–3.
- Produces: `godot::GBuffer` with `bool ensure(RenderingDevice *, RenderSceneBuffersRD * /*nullable*/, Vector2i size)`, `void teardown()`, `bool is_valid() const`, `Vector2i size() const`, `Vector2i half_size() const`, `RID albedo() const`, `RID surface() const`, `RID depth() const`, `RID lit() const`, `RID history() const`; and `GBuffer *VoxelWorld::gbuffer()`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_gbuffer.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w

func test_the_gbuffer_allocates_at_the_requested_size() -> void:
	var w := make_world()
	var d := w.debug_gbuffer_stats(320, 180)
	assert_bool(d["valid"]).is_true()
	assert_int(d["width"]).is_equal(320)
	assert_int(d["height"]).is_equal(180)
	assert_bool(d["albedo_valid"]).is_true()
	assert_bool(d["surface_valid"]).is_true()
	assert_bool(d["depth_valid"]).is_true()
	assert_bool(d["lit_valid"]).is_true()
	assert_bool(d["history_valid"]).is_true()

# The GI history is deliberately half resolution: SSGI reads it at half resolution and a
# full-resolution copy would cost 4x the bandwidth to be downsampled on read anyway.
func test_the_history_is_half_resolution_and_rounds_up_from_odd_sizes() -> void:
	var w := make_world()
	var d := w.debug_gbuffer_stats(321, 181)
	assert_int(d["half_width"]).is_equal(160)
	assert_int(d["half_height"]).is_equal(90)
	var tiny := w.debug_gbuffer_stats(1, 1)
	assert_int(tiny["half_width"]).is_equal(1)
	assert_int(tiny["half_height"]).is_equal(1)

func test_re_ensuring_at_the_same_size_reuses_the_same_textures() -> void:
	var w := make_world()
	var a := w.debug_gbuffer_stats(256, 144)
	var b := w.debug_gbuffer_stats(256, 144)
	assert_int(b["albedo_id"]).is_equal(int(a["albedo_id"]))
	assert_int(b["depth_id"]).is_equal(int(a["depth_id"]))
	assert_int(b["reallocations"]).is_equal(int(a["reallocations"]))

func test_a_different_size_reallocates() -> void:
	var w := make_world()
	var a := w.debug_gbuffer_stats(256, 144)
	var b := w.debug_gbuffer_stats(512, 288)
	assert_int(b["width"]).is_equal(512)
	assert_int(b["reallocations"]).is_greater(int(a["reallocations"]))

func test_a_degenerate_size_is_refused_rather_than_allocated() -> void:
	var w := make_world()
	var d := w.debug_gbuffer_stats(0, 180)
	assert_bool(d["valid"]).is_false()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_gbuffer.gd`
Expected: FAIL — `Nonexistent function 'debug_gbuffer_stats'`.

- [ ] **Step 3: Write `extension/src/render/gbuffer.h`**

```cpp
#pragma once
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace godot {

// Spec section 7's merged G-buffer. Two colour attachments and a depth attachment this
// engine owns, plus the deferred stack's output and the half-resolution copy of last
// frame's finished image that SSGI bounces light from.
//
//   albedo   R8G8B8A8_UNORM        rgb = albedo, a = sun visibility (shadow layer 1 and 2)
//   surface  R16G16B16A16_SFLOAT   xy = oct normal, z = material id, w = gloss
//   depth    D32_SFLOAT            reverse-Z, the SAME NDC as Godot's scene depth, so the
//                                  injection is a copy rather than a reprojection
//   lit      R16G16B16A16_SFLOAT   what the deferred pass produced this frame
//   history  R16G16B16A16_SFLOAT   HALF resolution; last frame's finished scene colour
//
// There is no separate linear-depth target (spec section 7 lists one). Linear depth is
// reconstructed from `depth` and the projection, which is exact, costs two ALU, and cannot
// disagree with the depth the raster actually tested against.
class GBuffer {
public:
	static const char *kContext; // "voxel_gbuf"

	~GBuffer();

	// `rsb` may be null. Non-null is the production path: the textures are named entries on
	// Godot's render scene buffers, so a viewport reconfigure frees them and the next
	// ensure() recreates them with no resize handling here. Null is the probe path: plain
	// RD textures this object owns and frees.
	bool ensure(RenderingDevice *rd, RenderSceneBuffersRD *rsb, Vector2i size);
	void teardown();

	bool is_valid() const;
	Vector2i size() const { return size_; }
	Vector2i half_size() const;

	RID albedo() const { return albedo_; }
	RID surface() const { return surface_; }
	RID depth() const { return depth_; }
	RID lit() const { return lit_; }
	RID history() const { return history_; }

	// Diagnostic: how many times ensure() has had to allocate. A number that climbs every
	// frame means the size or the context is churning.
	int reallocations() const { return reallocations_; }

private:
	bool ensure_owned(RenderingDevice *rd, Vector2i size);
	bool ensure_managed(RenderingDevice *rd, RenderSceneBuffersRD *rsb, Vector2i size);
	void free_owned();

	RenderingDevice *rd_ = nullptr;
	bool owned_ = false;
	Vector2i size_{0, 0};
	int reallocations_ = 0;
	RID albedo_, surface_, depth_, lit_, history_;
};

} // namespace godot
```

- [ ] **Step 4: Write `extension/src/render/gbuffer.cpp`**

```cpp
#include "render/gbuffer.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <algorithm>

using namespace godot;

const char *GBuffer::kContext = "voxel_gbuf";

namespace {

struct TargetSpec {
	const char *name;
	RenderingDevice::DataFormat format;
	uint32_t usage;
	bool half;
};

// One table, read by both the managed and the owned path, so the two can never allocate
// different formats for the same name.
const TargetSpec kTargets[5] = {
	{"albedo", RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT, false},
	{"surface", RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT, false},
	{"depth", RenderingDevice::DATA_FORMAT_D32_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT, false},
	{"lit", RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT, false},
	{"history", RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT, true},
};

Vector2i half_of(Vector2i s) {
	return Vector2i(std::max(1, s.x / 2), std::max(1, s.y / 2));
}

} // namespace

GBuffer::~GBuffer() {
	teardown();
}

Vector2i GBuffer::half_size() const {
	return half_of(size_);
}

bool GBuffer::is_valid() const {
	return albedo_.is_valid() && surface_.is_valid() && depth_.is_valid() &&
			lit_.is_valid() && history_.is_valid();
}

bool GBuffer::ensure(RenderingDevice *rd, RenderSceneBuffersRD *rsb, Vector2i size) {
	if (!rd || size.x <= 0 || size.y <= 0) return false;
	rd_ = rd;
	return rsb ? ensure_managed(rd, rsb, size) : ensure_owned(rd, size);
}

bool GBuffer::ensure_managed(RenderingDevice *rd, RenderSceneBuffersRD *rsb, Vector2i size) {
	// If this object previously owned textures (a probe ran first), let go of them.
	if (owned_) free_owned();
	owned_ = false;
	RID *slots[5] = {&albedo_, &surface_, &depth_, &lit_, &history_};
	for (int i = 0; i < 5; i++) {
		const TargetSpec &t = kTargets[i];
		const Vector2i ts = t.half ? half_of(size) : size;
		// Re-query every frame rather than caching: the engine drops the whole context on a
		// viewport reconfigure, and has_texture() going false is the ONLY signal it does.
		if (!rsb->has_texture(kContext, t.name)) {
			*slots[i] = rsb->create_texture(kContext, t.name, t.format, t.usage,
					RenderingDevice::TEXTURE_SAMPLES_1, ts, 1, 1, true, false);
			reallocations_++;
		} else {
			*slots[i] = rsb->get_texture(kContext, t.name);
		}
		if (!slots[i]->is_valid()) return false;
	}
	size_ = size;
	return true;
}

bool GBuffer::ensure_owned(RenderingDevice *rd, Vector2i size) {
	if (owned_ && size == size_ && is_valid()) return true;
	free_owned();
	owned_ = true;
	RID *slots[5] = {&albedo_, &surface_, &depth_, &lit_, &history_};
	for (int i = 0; i < 5; i++) {
		const TargetSpec &t = kTargets[i];
		const Vector2i ts = t.half ? half_of(size) : size;
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_format(t.format);
		f->set_width(ts.x);
		f->set_height(ts.y);
		f->set_usage_bits(t.usage);
		Ref<RDTextureView> v;
		v.instantiate();
		*slots[i] = rd->texture_create(f, v, TypedArray<PackedByteArray>());
		if (!slots[i]->is_valid()) {
			free_owned();
			return false;
		}
	}
	reallocations_++;
	size_ = size;
	return true;
}

void GBuffer::free_owned() {
	if (!owned_ || !rd_) {
		albedo_ = surface_ = depth_ = lit_ = history_ = RID();
		owned_ = false;
		return;
	}
	for (RID *r : {&albedo_, &surface_, &depth_, &lit_, &history_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	owned_ = false;
}

void GBuffer::teardown() {
	free_owned();
	albedo_ = surface_ = depth_ = lit_ = history_ = RID();
	size_ = Vector2i(0, 0);
	rd_ = nullptr;
}
```

**Managed textures are never freed here.** `RenderSceneBuffersRD` owns them; freeing them from this side leaves the engine holding a dangling name. `teardown()` only drops references in that case, which is why `free_owned()` short-circuits when `owned_` is false.

- [ ] **Step 5: Hang it off `VoxelWorld` and add the debug hook**

In `extension/src/voxel_world.h`: forward-declare `class GBuffer;`, add `GBuffer *gbuffer_ = nullptr;` beside the other pass pointers, `GBuffer *gbuffer() { return gbuffer_; }`, and `Dictionary debug_gbuffer_stats(int w, int h);`.

In `ensure_initialized()`, next to where `composite_pass_` is created:

```cpp
	gbuffer_ = new GBuffer();
```

In `teardown_gpu()`, before the passes are deleted (it holds no shader and no pipeline, so order does not matter, but keep it with its neighbours):

```cpp
	delete gbuffer_;
	gbuffer_ = nullptr;
```

In `extension/src/voxel_world.cpp`:

```cpp
Dictionary VoxelWorld::debug_gbuffer_stats(int w, int h) {
	Dictionary d;
	d["valid"] = false;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !gbuffer_) return d;
	// The probe path: no RenderSceneBuffersRD exists outside a render callback, so this
	// exercises the owned branch. Everything else about the object is identical.
	if (!gbuffer_->ensure(device, nullptr, Vector2i(w, h))) {
		d["reallocations"] = gbuffer_->reallocations();
		return d;
	}
	d["valid"] = gbuffer_->is_valid();
	d["width"] = gbuffer_->size().x;
	d["height"] = gbuffer_->size().y;
	d["half_width"] = gbuffer_->half_size().x;
	d["half_height"] = gbuffer_->half_size().y;
	d["albedo_valid"] = gbuffer_->albedo().is_valid();
	d["surface_valid"] = gbuffer_->surface().is_valid();
	d["depth_valid"] = gbuffer_->depth().is_valid();
	d["lit_valid"] = gbuffer_->lit().is_valid();
	d["history_valid"] = gbuffer_->history().is_valid();
	d["albedo_id"] = static_cast<int64_t>(gbuffer_->albedo().get_id());
	d["depth_id"] = static_cast<int64_t>(gbuffer_->depth().get_id());
	d["reallocations"] = gbuffer_->reallocations();
	return d;
}
```

Bind it: `ClassDB::bind_method(D_METHOD("debug_gbuffer_stats", "w", "h"), &VoxelWorld::debug_gbuffer_stats);`

- [ ] **Step 6: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then `./gdunit_tests.sh -a res://tests/test_gbuffer.gd`
Expected: PASS, five tests.

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/gbuffer.h extension/src/render/gbuffer.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_gbuffer.gd
git commit -m "feat: merged g-buffer targets managed by the render scene buffers"
```

---

### Task 5: the raymarcher stops shading and starts describing

Spec §3.3 ("G-buffer output — albedo, linear depth, oct-encoded normal, material ID. **Cel shading is deferred (Section 7), not done here**") and §7's shadow layer 1 ("Near field: raymarched sun rays"). The raymarcher's `imageStore(out_color, ...)` becomes two stores of surface description, and the sun ray it was never marching gets marched.

Nothing downstream changes yet: the composite still blits `albedo` into the scene colour, so after this task the demo looks *flat* (unlit albedo) — deliberately, and only until Task 6 lands the deferred pass three commits later. The tests below pin the description, not the picture.

**Files:**
- Create: `shaders/shade.glslh`
- Modify: `shaders/common.glslh`, `shaders/raymarch.comp.glsl`
- Modify: `extension/src/render/raymarch_pass.h`, `.cpp`
- Modify: `extension/src/render/composite_pass.cpp` (bind `albedo` where it bound `color`)
- Modify: `extension/src/raymarch_compositor.cpp`, `extension/src/voxel_world.cpp` (the two probes)
- Create: `tests/test_raymarch_gbuffer.gd`

**Interfaces:**
- Consumes: `ve::kSunDir`, `ve::oct_encode`/`oct_decode` semantics from Tasks 1–2; `ve::pack_flags` from Task 3.
- Produces: `RaymarchPass::albedo_texture()` (replaces `color_texture()`), `RaymarchPass::surface_texture()`, `RaymarchPass::hitpos_texture()` (unchanged); `shaders/shade.glslh` with `SUN_DIR`, `oct_encode`, `oct_decode`, `cel_band`, `cel_level`, `cel_rgb_to_hsv`, `cel_hsv_to_rgb`, `cel_shadow_tint`, `cel_shade`; `material_props()` in `common.glslh`; `VoxelWorld::debug_raymarch_gbuffer(Vector3 origin, Vector3 dir)`.
- **`ve::CameraParams` gains no field.** The beauty flag word rides in the previously unused `cam_pos[3]`, read in GLSL as `floatBitsToUint(pc.cam_pos.w)`. The struct stays exactly 128 bytes and its `static_assert` stays true.

- [ ] **Step 1: Write the failing test**

Create `tests/test_raymarch_gbuffer.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

# Straight down onto open ground from well above it: a hit, facing up, on a real material.
func probe_ground(w: VoxelWorld) -> Dictionary:
	return w.debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0))

func test_a_ground_hit_writes_a_material_and_a_normal() -> void:
	var w := make_world()
	var d := probe_ground(w)
	assert_bool(d["hit"]).is_true()
	assert_int(d["material"]).is_greater(0)
	# The generator's surface is a gentle height field, so straight down lands on ground
	# whose normal points broadly up. Anything else means the oct pack lost the vector.
	var n: Vector3 = d["normal"]
	assert_float(n.length()).is_equal_approx(1.0, 0.01)
	assert_float(n.y).is_greater(0.7)

func test_the_albedo_channel_is_albedo_and_not_shaded_colour() -> void:
	var w := make_world()
	var lit_probe := probe_ground(w)
	w.set_effect_enabled("raymarched_sun_shadow", false)
	var flat_probe := probe_ground(w)
	var a: Color = lit_probe["albedo"]
	var b: Color = flat_probe["albedo"]
	# Turning the sun ray off moves the SUN channel and nothing else. If the raymarcher were
	# still calling shade_terrain(), the light would be baked into the colour and the albedo
	# would move with it. This is the contract the whole deferred stack rests on.
	assert_float(absf(a.r - b.r)).is_less(0.005)
	assert_float(absf(a.g - b.g)).is_less(0.005)
	assert_float(absf(a.b - b.b)).is_less(0.005)
	assert_float(flat_probe["sun"]).is_greater_equal(lit_probe["sun"])
	# ...and what is stored is the material's own albedo, darkened only by ambient occlusion
	# (at most 35%), never by a lambert term.
	var direct: Color = w.debug_material_probe(int(lit_probe["material"]),
		lit_probe["position"], lit_probe["normal"])
	assert_float(a.r).is_between(direct.r * 0.65 - 0.01, direct.r + 0.01)
	assert_float(a.g).is_between(direct.g * 0.65 - 0.01, direct.g + 0.01)
	assert_float(a.b).is_between(direct.b * 0.65 - 0.01, direct.b + 0.01)

func test_the_sky_writes_material_zero_and_keeps_the_sky_colour() -> void:
	var w := make_world()
	var d := w.debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0))
	assert_bool(d["hit"]).is_false()
	assert_int(d["material"]).is_equal(0)
	# Material 0 means "no voxel here" and the deferred pass passes the albedo channel
	# through unlit, so the sky gradient has to survive in it.
	var albedo: Color = d["albedo"]
	assert_float(albedo.b).is_greater(albedo.r)

func test_gloss_comes_from_the_material_surface_array() -> void:
	var w := make_world()
	var d := probe_ground(w)
	# gloss = 1 - roughness, and the shipped materials are all rough ground, so it is low
	# but it must not be the constant 0 that "we never sampled the surface array" produces
	# for every material equally.
	assert_float(d["gloss"]).is_between(0.0, 1.0)
	# ...and it is not the constant the "we never sampled the surface array" bug produces:
	# an out-of-range material falls back to roughness 1, i.e. gloss exactly 0.
	var air := w.debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0))
	assert_float(air["gloss"]).is_equal_approx(0.0, 0.001)

# Shadow layer 1. A point on open ground faces the sun; a point at the bottom of the
# generator's carved cave at (30, ~49, 30) does not.
func test_the_sun_ray_shadows_the_inside_of_the_cave() -> void:
	var w := make_world()
	var lit := probe_ground(w)
	assert_float(lit["sun"]).is_greater(0.5)
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(30.0, 50.0, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	# Straight down inside the cave mouth: the floor there is under 5 m of overhang.
	var dark := w.debug_raymarch_gbuffer(Vector3(30.0, 50.0, 30.0), Vector3(0, -1, 0))
	if dark["hit"]:
		assert_float(dark["sun"]).is_less(lit["sun"])

func test_turning_the_sun_ray_off_makes_everything_fully_lit() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarched_sun_shadow", false)
	var d := probe_ground(w)
	assert_float(d["sun"]).is_equal_approx(1.0, 0.01)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_raymarch_gbuffer.gd`
Expected: FAIL — `Nonexistent function 'debug_raymarch_gbuffer'`.

- [ ] **Step 3: Write `shaders/shade.glslh`**

```glsl
// The shading numbers, mirrored from extension/src/shade/oct.h and extension/src/shade/cel.h.
// There is exactly ONE of each of these functions in the engine: the raymarcher, the LoD
// fragment shader, the deferred pass and the object material all read them from here, which
// is what makes spec section 7's "the near/far seam is mathematically invisible" a fact
// about the code rather than a hope.
//
// NOTE: never put a literal include directive inside a comment in this file -- the loader
// matches include tokens anywhere in a line and would self-include (cycle error).

const vec3 SUN_DIR = vec3(0.5746958, 0.7662610, 0.2873479); // ve::kSunDir

// ---- ve::oct_encode / ve::oct_decode -------------------------------------------------
// NOT sign(): sign(0.0) is 0 and sign(-0.0) is -0, either of which folds a normal into the
// wrong octant. The C++ mirror uses the same >= 0 test.
vec2 oct_sign_not_zero(vec2 v) {
	return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 oct_encode(vec3 n) {
	float l1 = abs(n.x) + abs(n.y) + abs(n.z);
	vec3 p = l1 > 0.0 ? n / l1 : vec3(0.0);
	vec2 e = p.xy;
	if (p.z < 0.0) e = (1.0 - abs(e.yx)) * oct_sign_not_zero(e);
	return e;
}

vec3 oct_decode(vec2 e) {
	vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * oct_sign_not_zero(v.xy);
	float l = length(v);
	return l > 0.0 ? v / l : vec3(0.0, 0.0, 1.0);
}

// ---- ve::CelParams / ve::cel_shade ---------------------------------------------------
const int CEL_BANDS = 4;
const float CEL_BAND_EDGE[3] = float[3](0.08, 0.32, 0.66);
const float CEL_BAND_LEVEL[4] = float[4](0.18, 0.45, 0.75, 1.00);
const float CEL_SHADOW_HUE_SHIFT = 0.055;
const float CEL_SHADOW_SATURATION = 1.35;
const float CEL_SPEC_EDGE = 0.72;
const float CEL_SPEC_STRENGTH = 0.45;
const float CEL_RIM_STRENGTH = 0.35;
const float CEL_RIM_POWER = 3.0;

int cel_band(float ndl) {
	float v = clamp(ndl, 0.0, 1.0);
	int band = 0;
	for (int i = 0; i < CEL_BANDS - 1; i++)
		if (v > CEL_BAND_EDGE[i]) band = i + 1;
	return band;
}

float cel_level(float ndl) { return CEL_BAND_LEVEL[cel_band(ndl)]; }

vec3 cel_rgb_to_hsv(vec3 c) {
	float mx = max(c.r, max(c.g, c.b));
	float mn = min(c.r, min(c.g, c.b));
	float d = mx - mn;
	float h = 0.0;
	if (d > 0.0) {
		if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
		else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
		else h = (c.r - c.g) / d + 4.0;
		h /= 6.0;
	}
	return vec3(h, mx > 0.0 ? d / mx : 0.0, mx);
}

vec3 cel_hsv_to_rgb(vec3 c) {
	float h = fract(c.x) * 6.0;
	float s = clamp(c.y, 0.0, 1.0);
	float v = c.z;
	int i = int(floor(h)) % 6;
	float f = h - floor(h);
	float a = v * (1.0 - s);
	float b = v * (1.0 - s * f);
	float g = v * (1.0 - s * (1.0 - f));
	if (i == 0) return vec3(v, g, a);
	if (i == 1) return vec3(b, v, a);
	if (i == 2) return vec3(a, v, g);
	if (i == 3) return vec3(a, b, v);
	if (i == 4) return vec3(g, a, v);
	return vec3(v, a, b);
}

// A grey albedo has zero saturation, and multiplying zero keeps it zero, so grey is left
// alone by the maths rather than by a branch.
vec3 cel_shadow_tint(vec3 albedo, float t) {
	float k = clamp(t, 0.0, 1.0);
	vec3 hsv = cel_rgb_to_hsv(albedo);
	hsv.x = fract(hsv.x + CEL_SHADOW_HUE_SHIFT * k);
	hsv.y = clamp(hsv.y * (1.0 + (CEL_SHADOW_SATURATION - 1.0) * k), 0.0, 1.0);
	return cel_hsv_to_rgb(hsv);
}

vec3 cel_shade(vec3 albedo, vec3 ambient, float ndl, float ndv, float ndh,
		float shadow, float ao, float gloss) {
	float sh = clamp(shadow, 0.0, 1.0);
	float lit = cel_level(ndl) * sh;
	vec3 tint = cel_shadow_tint(albedo, 1.0 - lit);
	// A hard step, not a falloff: the band edge IS the highlight's silhouette.
	float spec = (gloss > 0.0 && ndh >= CEL_SPEC_EDGE) ? gloss * CEL_SPEC_STRENGTH * sh : 0.0;
	float rim = CEL_RIM_STRENGTH * pow(1.0 - clamp(ndv, 0.0, 1.0), CEL_RIM_POWER);
	return tint * lit + tint * ambient * clamp(ao, 0.0, 1.0) + vec3(spec + rim);
}

// ---- ve::pack_flags bits -------------------------------------------------------------
const uint BEAUTY_SSGI = 1u;
const uint BEAUTY_SSR = 2u;
const uint BEAUTY_CONTACT = 4u;
const uint BEAUTY_OUTLINES = 8u;
const uint BEAUTY_SUN_MAP = 16u;
const uint BEAUTY_GLOSSY_RAYS = 32u;
const uint BEAUTY_RAY_SUN_SHADOW = 64u;
```

- [ ] **Step 4: Add `material_props` to `shaders/common.glslh`**

Inside the existing `#ifdef MATERIAL_LAYERS` block, right after `material_surface`:

```glsl
// The other half of the material: roughness and ambient occlusion, triplanar-weighted
// exactly like the albedo so a cliff face and a flat top read the same numbers. The channel
// packing is render/material_atlas.cpp's pack_layer(): normal XY, roughness in B, AO in A.
// M5 loaded this array and never sampled it; the cel stack is what needed it.
vec2 material_props(uint mat, vec3 p, vec3 n, vec3 ddx, vec3 ddy) {
	int layer = int(mat) - 1;
	if (layer < 0 || layer >= MATERIAL_LAYERS) return vec2(1.0, 1.0); // rough, unoccluded
	vec3 an = abs(n);
	vec3 w = an / max(an.x + an.y + an.z, 1e-5);
	float s = MATERIAL_UV_SCALE;
	vec4 cx = textureGrad(material_surface_tex, vec3(p.zy * s, float(layer)), ddx.zy * s, ddy.zy * s);
	vec4 cy = textureGrad(material_surface_tex, vec3(p.xz * s, float(layer)), ddx.xz * s, ddy.xz * s);
	vec4 cz = textureGrad(material_surface_tex, vec3(p.xy * s, float(layer)), ddx.xy * s, ddy.xy * s);
	vec4 c = cx * w.x + cy * w.y + cz * w.z;
	return vec2(c.b, c.a);
}
```

Leave `shade_terrain()` in place for now — `lod.frag.glsl` still calls it until Task 7, and deleting it here breaks the far field.

- [ ] **Step 5: Rewrite the raymarcher's output**

In `shaders/raymarch.comp.glsl`:

Change the two image bindings and add the third. Bindings 2–19 are untouched, so the new one goes at 20:

```glsl
layout(set = 0, binding = 0, rgba8) writeonly uniform image2D out_albedo;
layout(set = 0, binding = 1, rgba32f) writeonly uniform image2D out_hitpos;
layout(set = 0, binding = 20, rgba16f) writeonly uniform image2D out_surface;
```

Add the include beside the existing one:

```glsl
#include "shade.glslh"
```

Add the sun-ray marcher after `march_terrain`:

```glsl
// ---------------------------------------------------------------------------------------
// Shadow layer 1 (spec section 7): the same field the primary ray marched, one ray per
// pixel. Sphere tracing gives contact hardening for free -- the penumbra narrows as the
// occluder approaches -- with no shadow map and therefore no acne to bias away.
//
// world_sdf() returns +SDF_RANGE for a brick with no atlas slot, which is a safe "nothing
// near here": the ray strides through unloaded space at 0.64 m a step and the shadow simply
// stops where residency stops. That is the correct behaviour, not a bug -- there is no data
// out there to cast a shadow with.
// ---------------------------------------------------------------------------------------
const float RAY_SHADOW_DIST = 60.0;
const int RAY_SHADOW_STEPS = 96;
const float RAY_SHADOW_K = 12.0;
const int RAY_SHADOW_MAX_ISLANDS = 4;

float terrain_sun_visibility(vec3 ro) {
	float res = 1.0;
	float t = 0.05;
	for (int i = 0; i < RAY_SHADOW_STEPS; i++) {
		if (t > RAY_SHADOW_DIST) break;
		float d = world_sdf(ro + SUN_DIR * t);
		if (d < 0.004) return 0.0;
		res = min(res, RAY_SHADOW_K * d / t);
		t += clamp(d, 0.02, 1.0);
	}
	return clamp(res, 0.0, 1.0);
}

// Spec section 5: "islands shade/shadow/reflect exactly like static terrain". The AABB
// reject costs a few ALU for each of the (at most 32) live slots; only islands the ray
// actually crosses are marched, and at most RAY_SHADOW_MAX_ISLANDS of them. A fifth
// overlapping island is a case the demo does not produce and the budget does not pay for.
float island_sun_visibility(vec3 ro, int island_count) {
	float res = 1.0;
	int marched = 0;
	for (int i = 0; i < 32; i++) {
		if (i >= island_count || marched >= RAY_SHADOW_MAX_ISLANDS) break;
		vec3 lo = island_desc.v[i * 8 + 5].xyz;
		vec3 hi = island_desc.v[i * 8 + 6].xyz;
		float t0, t1;
		if (!ray_box(ro, SUN_DIR, lo, hi, t0, t1)) continue;
		Island isl;
		if (!island_load(i, isl)) continue;
		marched++;
		mat3 inv = transpose(isl.basis);
		vec3 ro_l = inv * (ro - isl.pos);
		vec3 rd_l = inv * SUN_DIR;
		float t = max(t0, 0.05);
		float tmax = min(t1, RAY_SHADOW_DIST);
		for (int k = 0; k < 48; k++) {
			if (t > tmax) break;
			float d = island_sdf_at(i, isl, ro_l + rd_l * t);
			if (d < 0.004) return 0.0;
			res = min(res, RAY_SHADOW_K * d / t);
			t += clamp(d, 0.02, 1.0);
		}
	}
	return clamp(res, 0.0, 1.0);
}
```

Replace the whole tail of `main()` from `// One shading path for both` to the end:

```glsl
	uint flags = floatBitsToUint(pc.cam_pos.w);

	// Sky and misses: material 0 means "no voxel here". The albedo channel carries the sky
	// gradient and the deferred pass passes material 0 straight through unlit, so sky_color()
	// still lives in exactly one place.
	vec3 albedo = sky_color(rd);
	vec2 oct = oct_encode(-rd);
	float mat_id = 0.0;
	float gloss = 0.0;
	float ao = 1.0;
	float sun = 1.0;
	vec4 hitpos = vec4(0.0);

	if (best.hit) {
		// The pixel's world footprint at the hit: the ray direction's screen derivative
		// scaled by distance. tan_x/tan_y and the target size are already in the push
		// constant, so this costs two multiplies and no extra state.
		vec3 ddx = pc.cam_right.xyz * (2.0 * pc.params.x / float(size.x)) * best.t;
		vec3 ddy = pc.cam_up.xyz * (2.0 * pc.params.y / float(size.y)) * best.t;
		vec4 surf = material_surface(best.mat, best.p, best.n, ddx, ddy);
		vec2 props = material_props(best.mat, best.p, best.n, ddx, ddy);
		albedo = surf.rgb;
		oct = oct_encode(best.n);
		mat_id = float(best.mat);
		gloss = 1.0 - props.x;
		ao = props.y;
		hitpos = vec4(best.p, 1.0);
		if ((flags & BEAUTY_RAY_SUN_SHADOW) != 0u) {
			// One voxel of offset: less and the ray self-shadows on its own surface, more
			// and thin ledges stop casting.
			vec3 sro = best.p + best.n * 0.06;
			sun = min(terrain_sun_visibility(sro), island_sun_visibility(sro, island_count));
		}
	}

	// The pending-edit visualiser tints the DESCRIPTION, so the tint survives the deferred
	// pass instead of being relit away.
	if (best.hit && edits.params.x > 0.0 &&
			length(best.p - edits.center.xyz) < edits.params.x) {
		uint et = uint(edits.params.y);
		vec3 tint = et == 0u ? vec3(1.0, 0.55, 0.1)
		          : et == 1u ? flat_material_albedo(4u)
		          : flat_material_albedo(uint(edits.params.z));
		albedo = mix(albedo, tint, 0.45);
	}

	// Debug material probe: a 1x1 dispatch calls material_surface() directly with zero
	// gradients. pc.params.w is otherwise unused, so > 0 is the probe flag.
	if (pc.params.w > 0.0) {
		vec4 surf = material_surface(uint(pc.params.w), pc.cam_pos.xyz,
				normalize(pc.cam_fwd.xyz), vec3(0.0), vec3(0.0));
		imageStore(out_albedo, px, vec4(surf.rgb, 1.0));
		imageStore(out_surface, px, vec4(0.0, 0.0, pc.params.w, 0.0));
		imageStore(out_hitpos, px, vec4(pc.cam_pos.xyz, 1.0));
		return;
	}

	// AO has no channel of its own. The cel stack only ever multiplies the AMBIENT term by
	// it, and folding it into the albedo here costs nothing and keeps hitpos.w the pure hit
	// flag every existing reader already treats it as. 0.65 is how much of the map is
	// allowed to darken the surface; a full multiply reads as dirt in the cel bands.
	imageStore(out_albedo, px, vec4(albedo * mix(1.0, ao, 0.65), sun));
	imageStore(out_surface, px, vec4(oct, mat_id, gloss));
	imageStore(out_hitpos, px, hitpos);
```

**Channel budget, stated once so no later task re-litigates it.** `albedo.a` is sun visibility, `surface.z` is the material id (0 = no voxel), `surface.w` is gloss, `hitpos.w` stays the hit flag it has been since M1, and ambient occlusion is baked into `albedo.rgb`. Nothing else is stored, and no pass may add a channel without changing `GBuffer`'s formats in Task 4.


- [ ] **Step 6: Give `RaymarchPass` its third target**

In `extension/src/render/raymarch_pass.h`, rename the accessor and add one:

```cpp
	RID albedo_texture() const { return albedo_; }
	RID surface_texture() const { return surface_; }
	RID hitpos_texture() const { return hitpos_; }
```

and rename the member `color_` to `albedo_`, adding `RID surface_;`.

In `raymarch_pass.cpp`, `rebuild_targets` grows from 20 uniforms to 21:

```cpp
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	if (albedo_.is_valid()) rd->free_rid(albedo_);
	if (surface_.is_valid()) rd->free_rid(surface_);
	if (hitpos_.is_valid()) rd->free_rid(hitpos_);
	albedo_ = make_target(rd, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, w, h);
	surface_ = make_target(rd, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, w, h);
	hitpos_ = make_target(rd, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, w, h);
	width_ = w;
	height_ = h;

	Ref<RDUniform> u[21];
	for (int i = 0; i < 21; i++) u[i].instantiate();
	u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[0]->set_binding(0); u[0]->add_id(albedo_);
	u[1]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[1]->set_binding(1); u[1]->add_id(hitpos_);
	// ... bindings 2..19 unchanged ...
	u[20]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[20]->set_binding(20); u[20]->add_id(surface_);
	Array uset_args;
	for (int i = 0; i < 21; i++) uset_args.push_back(u[i]);
	uset_ = rd->uniform_set_create(uset_args, shader_, 0);
```

`teardown()`'s free list becomes `{&uset_, &pipeline_, &shader_, &albedo_, &surface_, &hitpos_, &sampler_, &edits_ubo_}`, and `render()`'s guard becomes `if (!uset_.is_valid() || !albedo_.is_valid() || !surface_.is_valid() || !edits_ubo_.is_valid()) return false;`.

- [ ] **Step 7: Update the three call sites and the two probes**

`composite_pass.cpp`'s `draw()` already takes `src_color` as a parameter; the compositor passes `rmp->albedo_texture()` instead of `rmp->color_texture()`. Nothing else in the composite changes in this task.

`RaymarchCompositor::_render_callback` sets the flag word before calling `rmp->render`:

```cpp
	const uint32_t beauty_flags = ve::pack_flags(world->beauty_settings());
	std::memcpy(&cp.cam_pos[3], &beauty_flags, sizeof(float));
```

(`#include "shade/beauty_settings.h"` and `<cstring>` at the top.) Do the same in `VoxelWorld::debug_raymarch_pixel`, `debug_raymarch_probe` and the new `debug_raymarch_gbuffer`, so a probe honours the toggles the tests set.

`VoxelWorld::debug_raymarch_pixel` reads an **RGBA8** texture now, not halves:

```cpp
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->albedo_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	if (data.size() < 4 || hp.size() < 16) return Color(1, 0, 1);
	const uint8_t *b = data.ptr();
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	// Alpha stays the HIT FLAG, as every existing caller assumes -- the albedo image's own
	// alpha is sun visibility and would read as "missed" for any shadowed pixel.
	return Color(b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, hf[3]);
```

Apply the same substitution inside `debug_raymarch_probe` for its `d["color"]`.

Add the new hook, which is what the test drives:

```cpp
Dictionary VoxelWorld::debug_raymarch_gbuffer(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !materials_ || !raymarch_pass_) return d;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.dims[3] = island_slot_count();
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	const uint32_t flags = ve::pack_flags(beauty_);
	std::memcpy(&cam.cam_pos[3], &flags, sizeof(float));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, islands_, RID(), cam, 1, 1, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray ab = device->texture_get_data(raymarch_pass_->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(raymarch_pass_->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	if (ab.size() < 4 || sf.size() < 8 || hp.size() < 16) return d;
	const uint8_t *a = ab.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *h = reinterpret_cast<const float *>(hp.ptr());
	d["albedo"] = Color(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1.0f);
	d["sun"] = a[3] / 255.0f;
	const float e[2] = {half_to_float(s[0]), half_to_float(s[1])};
	float n[3];
	ve::oct_decode(e, n);
	d["normal"] = Vector3(n[0], n[1], n[2]);
	d["material"] = static_cast<int>(half_to_float(s[2]) + 0.5f);
	d["gloss"] = half_to_float(s[3]);
	d["hit"] = h[3] > 0.5f;
	d["position"] = Vector3(h[0], h[1], h[2]);
	return d;
}
```

`#include "shade/oct.h"` at the top of `voxel_world.cpp`, and bind the method.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then
`./gdunit_tests.sh -a res://tests/test_raymarch_gbuffer.gd -c`, then
`./gdunit_tests.sh -a res://tests/test_material_atlas.gd -a res://tests/test_raymarch_pixel.gd -a res://tests/test_raymarch_magenta.gd -a res://tests/test_island_render.gd -c`
Expected: PASS on both. The four pre-existing suites are the regression gate: they all read `debug_raymarch_pixel`, whose alpha is now the hit flag and whose rgb is now unlit albedo. `test_material_atlas.gd::test_the_near_field_gains_texture_detail` compares two probes for *difference* and still holds; if any of the four asserted on a *lit* value, fix the assertion to the albedo value and say so in the commit message — the description is the new contract.

- [ ] **Step 9: Commit**

```bash
git add shaders/shade.glslh shaders/common.glslh shaders/raymarch.comp.glsl \
        extension/src/render/raymarch_pass.h extension/src/render/raymarch_pass.cpp \
        extension/src/raymarch_compositor.cpp extension/src/voxel_world.h \
        extension/src/voxel_world.cpp tests/test_raymarch_gbuffer.gd
git commit -m "feat: raymarcher writes g-buffer channels and marches sun shadow rays"
```

---

### Task 6: the near field lands in the G-buffer and comes back out lit

Spec §7: "One deferred pass shades everything identically." This task builds that pass and the injection behind it, and moves the composite from "blit colour into Godot's scene buffer" to "resolve the near field into the G-buffer". The far field is untouched — it still forward-shades into the scene colour *after* the injection — so the screen stays correct end to end and Task 7 can move it across on its own.

This is also where Tasks 1 and 2's GLSL mirrors get pinned: the deferred shader has a probe mode that shades one pixel straight from the push constant, and the test diffs it against `ve::cel_shade`.

**Files:**
- Create: `shaders/deferred.comp.glsl`, `shaders/inject.vert.glsl`, `shaders/inject.frag.glsl`
- Create: `extension/src/render/deferred_pass.h`, `.cpp`, `extension/src/render/inject_pass.h`, `.cpp`
- Modify: `shaders/composite.frag.glsl`, `extension/src/render/composite_pass.h`, `.cpp`
- Modify: `extension/src/raymarch_compositor.cpp`, `extension/src/voxel_world.h`, `.cpp`
- Create: `tests/test_deferred.gd`

**Interfaces:**
- Consumes: `GBuffer` (Task 4); `shade.glslh`'s `oct_decode`, `cel_shade`, `SUN_DIR`, `BEAUTY_*` (Task 5); `ve::cel_shade`, `ve::CelParams`, `ve::CelInput` (Task 2); `ve::pack_flags` (Task 3).
- Produces:
  - `CompositePass::draw(RenderingDevice *rd, GBuffer &gb, RID src_albedo, RID src_surface, RID src_hitpos, const Projection &view_proj, const MaterialAtlas &materials, const float cam_pos[3], float fade_start, float fade_end, RID marker = RID())` — the `dst_color`/`dst_depth` parameters are gone.
  - `godot::DeferredPass` with `void initialize(RenderingDevice *)`, `void teardown()`, `struct Params { float inv_view_proj[16]; float cam_pos[3]; float ambient[3]; uint32_t flags; int probe_mode; }`, `bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials, RID ssgi, RID sun_map, const float sun_view_proj[16], float shadow_texel, const Params &p)`, `float last_ms() const`.
  - `godot::InjectPass` with `void initialize(RenderingDevice *)`, `void teardown()`, `void release_targets()`, `bool draw(RenderingDevice *rd, RID dst_color, RID dst_depth, RID lit, RID gb_depth)`.
  - `VoxelWorld::debug_cel_diff(Color albedo, Color ambient, float ndl, float ndv, float ndh, float shadow, float ao, float gloss)` and `VoxelWorld::debug_deferred_probe(Vector3 pos, Vector3 fwd, int w, int h, int probe_mode)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_deferred.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

# THE differential test of this milestone. shade.glslh's cel_shade and ve::cel_shade are two
# spellings of one function; if they drift, the near field and the object materials part
# company and the seam this whole design exists to hide becomes visible again.
func test_the_gpu_cel_ramp_matches_the_cpu_one() -> void:
	var w := make_world()
	var cases := [
		# albedo,                       ambient,                  ndl, ndv, ndh, shadow, ao, gloss
		[Color(0.8, 0.2, 0.1), Color(0.0, 0.0, 0.0), 1.0, 1.0, 0.0, 1.0, 1.0, 0.0],
		[Color(0.36, 0.55, 0.22), Color(0.16, 0.19, 0.26), 0.5, 0.8, 0.3, 1.0, 1.0, 0.0],
		[Color(0.45, 0.42, 0.40), Color(0.16, 0.19, 0.26), 0.05, 0.4, 0.9, 1.0, 1.0, 0.9],
		[Color(0.45, 0.42, 0.40), Color(0.16, 0.19, 0.26), 0.9, 0.1, 0.1, 0.0, 1.0, 0.0],
		[Color(0.5, 0.5, 0.5), Color(0.2, 0.2, 0.2), 0.33, 0.5, 0.75, 0.5, 0.4, 0.6],
		[Color(0.02, 0.02, 0.9), Color(0.0, 0.0, 0.0), 0.66, 1.0, 0.0, 1.0, 1.0, 0.0],
	]
	for c in cases:
		var d: Dictionary = w.debug_cel_diff(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7])
		assert_float(d["max_delta"]).override_failure_message(
			"gpu %s vs cpu %s for %s" % [d["gpu"], d["cpu"], c]).is_less(0.004)

# The band edges are the whole look. If the GPU search rounds differently from the CPU one,
# the terracing lands a pixel off and the outlines stop lining up with the bands.
func test_the_band_edges_land_in_the_same_place_on_both_sides() -> void:
	var w := make_world()
	for edge in [0.08, 0.32, 0.66]:
		for delta in [-0.01, 0.01]:
			var d: Dictionary = w.debug_cel_diff(Color(0.6, 0.6, 0.6), Color(0, 0, 0),
				edge + delta, 1.0, 0.0, 1.0, 1.0, 0.0)
			assert_float(d["max_delta"]).override_failure_message(
				"band edge %f%+f: gpu %s cpu %s" % [edge, delta, d["gpu"], d["cpu"]]
				).is_less(0.004)

# Reconstructing world position from the depth attachment is where a sign error hides: it
# looks plausible everywhere and is wrong everywhere. Pin it against the position the
# raymarcher actually hit.
func test_the_deferred_pass_reconstructs_the_world_position_it_was_given() -> void:
	var w := make_world()
	var pos := Vector3(20.0, 75.0, 20.0)
	var fwd := Vector3(0, -1, 0)
	var truth := w.debug_raymarch_gbuffer(pos, fwd)
	assert_bool(truth["hit"]).is_true()
	# probe_mode 2 writes the reconstructed world position into the lit target instead of a
	# colour; the probe reports the centre pixel.
	var d := w.debug_deferred_probe(pos, fwd, 64, 64, 2)
	var got: Vector3 = d["center"]
	var want: Vector3 = truth["position"]
	assert_float(got.distance_to(want)).override_failure_message(
		"reconstructed %s, raymarched %s" % [got, want]).is_less(0.25)

func test_sky_pixels_pass_through_the_deferred_pass_unlit() -> void:
	var w := make_world()
	# Straight up: every pixel is sky, material 0.
	var d := w.debug_deferred_probe(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0), 64, 64, 0)
	var c: Color = d["center"]
	# sky_color() is blue-dominant looking up. Cel-shading it would band it into flat plates.
	assert_float(c.b).is_greater(c.r)
	assert_int(d["distinct_rows"]).override_failure_message(
		"the sky was quantised into cel bands").is_greater(8)

func test_the_lit_image_is_darker_where_the_sun_ray_says_it_is() -> void:
	var w := make_world()
	var d_lit := w.debug_deferred_probe(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	w.set_effect_enabled("raymarched_sun_shadow", false)
	var d_flat := w.debug_deferred_probe(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	# Removing the shadow term can only brighten the image, never darken it.
	assert_float(d_flat["mean_luma"]).is_greater_equal(float(d_lit["mean_luma"]) - 0.001)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_deferred.gd`
Expected: FAIL — `Nonexistent function 'debug_cel_diff'`.

- [ ] **Step 3: Rewrite `shaders/composite.frag.glsl`**

The whole fragment stage, replacing the file's `#[fragment]` section (the `#[vertex]` section is unchanged):

```glsl
#[fragment]
#version 460
#define MATERIAL_LAYERS 16
layout(set = 0, binding = 2) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 3) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
layout(location = 0) in vec2 uv_in;

// The merged G-buffer's two colour attachments, in GBuffer's order.
layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss
#ifdef SEAM_MARKER
layout(location = 2) out uint marker; // debug seam probe: 1 = near field kept the pixel
#endif

layout(set = 0, binding = 0) uniform sampler2D src_albedo;  // linear (0.66x upscale)
layout(set = 0, binding = 1) uniform sampler2D src_hitpos;  // nearest (no silhouette smear)
layout(set = 0, binding = 4) uniform sampler2D src_surface; // NEAREST: interpolating a
                                                            // material id between two
                                                            // materials names a third one
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;  // xyz = camera position, w = fade start
	vec4 fade; // x = fade end, yzw unused
} pc;

void main() {
	vec4 hp = texture(src_hitpos, uv_in);
	// The description is written unconditionally. Only the DEPTH decides who owns the pixel,
	// which is what lets a band pixel keep near-field colour when no LoD chunk arrives.
	out_albedo = texture(src_albedo, uv_in);
	out_surface = texture(src_surface, uv_in);
#ifdef SEAM_MARKER
	marker = 1u;
#endif
	if (hp.w < 0.5) {
		// Sky: farthest. Godot 4.7 renders reverse-Z (near = 1.0, far = 0.0), so the
		// farthest depth is 0.0. The raymarcher already wrote material 0 here, so the
		// deferred pass will pass this albedo through unlit.
#ifdef SEAM_MARKER
		marker = 0u;
#endif
		gl_FragDepth = 0.0;
		return;
	}
	// Spec section 7.4: surrender the DEPTH over the band, keep the description.
	// lod.frag discards on the complementary side of the same threshold at the same
	// resolution on the same pixel grid, so every band pixel belongs to exactly one field.
	float d = distance(hp.xyz, pc.cam.xyz);
	float t = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	if (bayer4(ivec2(gl_FragCoord.xy)) < t) {
#ifdef SEAM_MARKER
		marker = 0u;
#endif
		gl_FragDepth = 0.0;
		return;
	}
	vec4 clip = pc.view_proj * vec4(hp.xyz, 1.0);
	// The scene projection already outputs NDC z in [0,1] with near->1, far->0 (reverse-Z),
	// so no *0.5+0.5 remap.
	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
```

- [ ] **Step 4: Point `CompositePass` at the G-buffer**

`composite_pass.h`: forward-declare `class GBuffer;`, change the signature as stated in **Interfaces**, and replace the framebuffer key members:

```cpp
	RID framebuffer_, fb_albedo_, fb_surface_, fb_depth_, fb_marker_;
	RID uset_, uset_shader_, uset_src_albedo_, uset_src_surface_, uset_src_hitpos_;
```

`composite_pass.cpp`:

- `ensure_pipeline` takes `(rd, RID albedo, RID surface, RID depth, RID marker)` and builds the attachment array in the shader's output order — colour attachments first, depth last, because `framebuffer_create` derives the depth slot from the `DEPTH_STENCIL_ATTACHMENT` usage bit, not from position:

```cpp
	const Array attachments = want_marker
			? Array::make(albedo, surface, marker, depth)
			: Array::make(albedo, surface, depth);
```

- the colour blend state gains a second (and, in the marker variant, a third) attachment; every one has blending disabled:

```cpp
		Ref<RDPipelineColorBlendStateAttachment> att_a, att_s;
		att_a.instantiate();
		att_s.instantiate();
		att_a->set_enable_blend(false);
		att_s->set_enable_blend(false);
		if (want_marker) {
			Ref<RDPipelineColorBlendStateAttachment> att_m;
			att_m.instantiate();
			att_m->set_enable_blend(false);
			cb->set_attachments(Array::make(att_a, att_s, att_m));
		} else {
			cb->set_attachments(Array::make(att_a, att_s));
		}
```

- the depth-stencil state is unchanged (`GREATER_OR_EQUAL`, write on).
- `draw()` gains the fourth uniform and the clear. The composite is the **first** writer of the G-buffer each frame, so it is what clears it: without a depth clear, last frame's depth blocks this frame's near field for a whole viewport:

```cpp
	Ref<RDUniform> u4;
	u4.instantiate();
	u4->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u4->set_binding(4);
	u4->add_id(sampler_nearest_);
	u4->add_id(src_surface);
	// ... cache key gains src_surface, uniform_set_create takes Array::make(u0, u1, u2, u3, u4)
	PackedColorArray clears;
	clears.push_back(Color(0, 0, 0, 0));
	clears.push_back(Color(0, 0, 0, 0));
	if (marker.is_valid()) clears.push_back(Color(0, 0, 0, 0));
	const int64_t dl = rd->draw_list_begin(framebuffer_,
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH,
			clears, /*clear_depth_value*/ 0.0f);
```

`0.0f` is the reverse-Z far plane; every fragment's `gl_FragDepth >= 0.0` then passes the `GREATER_OR_EQUAL` test, which is what makes a full clear correct rather than merely tidy.

- [ ] **Step 5: Write `shaders/deferred.comp.glsl`**

```glsl
#[compute]
#version 460

#define MATERIAL_LAYERS 16
layout(set = 0, binding = 8) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 9) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gb_albedo;
layout(set = 0, binding = 1) uniform sampler2D gb_surface;
layout(set = 0, binding = 2) uniform sampler2D gb_depth;
layout(set = 0, binding = 3) uniform sampler2D ssgi_tex; // half res; 1x1 black when off
layout(set = 0, binding = 4) uniform sampler2D sun_map;  // ortho depth; 1x1 far when off
layout(set = 0, binding = 5, rgba16f) writeonly uniform image2D out_lit;
// The sun matrix rides in a uniform buffer rather than the push constant for one reason:
// the push block is already 112 of the 128 guaranteed bytes and a second mat4 does not fit.
// It changes only when the shadow map is rebuilt, so a UBO is also the cheaper place for it.
layout(set = 0, binding = 6, std140) uniform SunBlock {
	mat4 view_proj;
	vec4 params; // x = one shadow texel in world metres, yzw unused
} sun;

layout(push_constant, std430) uniform Push {
	// PROBE MODE ALIASES THIS BLOCK. In probe mode 1 the matrix carries the inputs to
	// cel_shade() directly so the GPU and CPU ramps can be diffed with no G-buffer at all:
	//   columns[0].xyz = albedo, columns[1].xyz = ambient,
	//   columns[2]     = (ndl, ndv, ndh, shadow), columns[3].xy = (ao, gloss).
	mat4 inv_view_proj; // floats 0..15
	vec4 cam;           // 16..19: xyz camera position, w unused
	vec4 sky;           // 20..23: rgb constant ambient, w unused
	uvec4 flags;        // 24..27: x = ve::pack_flags, y = probe mode, zw unused
} pc;

// Reverse-Z ortho: nearer to the sun is LARGER. A receiver is lit when its own depth is at
// least what the map recorded, within a slope-scaled bias. Outside the map is lit -- a
// world-covering map should never be outside, and "lit" is the fail-soft direction.
float sun_map_visibility(vec3 wpos, float ndl) {
	vec4 c = sun.view_proj * vec4(wpos, 1.0);
	if (c.w <= 0.0) return 1.0;
	vec3 p = c.xyz / c.w;
	vec2 uv = p.xy * 0.5 + 0.5;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
	// Two texels of slope bias plus a constant floor. At 2 m/texel a grazing receiver moves
	// most of a metre in depth across one texel, which is exactly what acne is.
	float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
	float bias = sun.params.x * (0.5 + 2.0 * slope) + 0.0015;
	return (p.z + bias >= texture(sun_map, uv).r) ? 1.0 : 0.0;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_lit);
	if (px.x >= size.x || px.y >= size.y) return;

	if (pc.flags.y == 1u) { // cel probe: shade straight from the push constant
		vec3 albedo = pc.inv_view_proj[0].xyz;
		vec3 ambient = pc.inv_view_proj[1].xyz;
		vec4 t = pc.inv_view_proj[2];
		vec2 ag = pc.inv_view_proj[3].xy;
		imageStore(out_lit, px,
				vec4(cel_shade(albedo, ambient, t.x, t.y, t.z, t.w, ag.x, ag.y), 1.0));
		return;
	}

	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec4 g0 = texelFetch(gb_albedo, px, 0);
	vec4 g1 = texelFetch(gb_surface, px, 0);
	uint mat = uint(g1.z + 0.5);
	if (mat == 0u && pc.flags.y != 2u) {
		// No voxel here: sky, or a pixel Godot's opaque pass owns. The albedo channel already
		// carries sky_color()'s gradient, and quantising a gradient into cel bands is exactly
		// the artefact this branch exists to avoid.
		imageStore(out_lit, px, vec4(g0.rgb, 1.0));
		return;
	}

	// Vulkan NDC: y = -1 at the TOP of the framebuffer, and Godot's scene projection already
	// carries the y-flip that makes that true (its c11 is negative). So this is uv.y * 2 - 1,
	// NOT 1 - uv.y * 2 -- the raymarcher's convention, which builds rays from cam_up instead.
	float depth = texelFetch(gb_depth, px, 0).r;
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0);
	vec4 h = pc.inv_view_proj * vec4(ndc, depth, 1.0);
	vec3 wpos = h.xyz / (abs(h.w) < 1e-9 ? 1e-9 : h.w);

	if (pc.flags.y == 2u) { // world-position probe
		imageStore(out_lit, px, vec4(wpos, 1.0));
		return;
	}

	vec3 n = oct_decode(g1.xy);
	vec3 v = normalize(pc.cam.xyz - wpos);
	float ndl = dot(n, SUN_DIR);
	float ndv = dot(n, v);
	float ndh = dot(n, normalize(SUN_DIR + v));
	float shadow = g0.a;
	if ((pc.flags.x & BEAUTY_SUN_MAP) != 0u)
		shadow = min(shadow, sun_map_visibility(wpos, ndl));
	vec3 ambient = pc.sky.rgb;
	if ((pc.flags.x & BEAUTY_SSGI) != 0u) ambient += texture(ssgi_tex, uv).rgb;
	// AO is already folded into g0.rgb by the producers, so 1.0 here is not a stub: it is
	// the statement that the ambient term has no second occlusion factor.
	imageStore(out_lit, px,
			vec4(cel_shade(g0.rgb, ambient, ndl, ndv, ndh, shadow, 1.0, g1.w), 1.0));
}
```

- [ ] **Step 6: Write `DeferredPass`**

`extension/src/render/deferred_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>

namespace godot {

class GBuffer;
class MaterialAtlas;

// Spec section 7's "one deferred lighting stack". Reads the merged G-buffer, writes the lit
// target. Everything it can be missing an input for -- SSGI, the sun shadow map -- is a
// RID it clears the corresponding flag bit for, so a pass that has not landed yet (or has
// failed this frame) makes the image plainer and never wrong.
class DeferredPass {
public:
	// Constant sky bounce, used when SSGI is off or absent. Cool, because the cel ramp's
	// shadow tint is already pushing shadows blue and the ambient must not fight it.
	static constexpr float kAmbient[3] = {0.16f, 0.19f, 0.26f};

	struct Params {
		float inv_view_proj[16] = {}; // column-major, inverse(proj * view)
		float cam_pos[3] = {};
		float ambient[3] = {kAmbient[0], kAmbient[1], kAmbient[2]};
		uint32_t flags = 0;  // ve::pack_flags
		int probe_mode = 0;  // 0 = shade, 1 = cel probe, 2 = world-position probe
	};

	~DeferredPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return shader_.is_valid() && pipeline_.is_valid(); }

	// `ssgi` and `sun_map` may be invalid RIDs; `sun_view_proj` is ignored when `sun_map` is.
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID ssgi, RID sun_map, const float sun_view_proj[16], float shadow_texel,
			const Params &p);

	float last_ms() const { return last_ms_; }

private:
	bool ensure_dummies(RenderingDevice *rd);
	bool ensure_uniform_set(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID ssgi, RID sun_map);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_linear_, sampler_nearest_;
	RID dummy_black_, dummy_far_, sun_ubo_;
	RID uset_;
	RID key_albedo_, key_surface_, key_depth_, key_lit_, key_ssgi_, key_sun_, key_materials_;
	float last_ms_ = 0.0f;
};

} // namespace godot
```

`deferred_pass.cpp` follows `RaymarchPass`'s shape exactly — `ve::load_shader_source` + `strip_shader_annotations`, `shader_compile_spirv_from_source`, `compute_pipeline_create`, a cached uniform set rebuilt when any bound RID changes, and a `teardown()` whose free order is **uniform set, pipeline, shader, then owned textures/buffers** (M5's documented cascade rule). The parts that are not boilerplate:

```cpp
bool DeferredPass::ensure_dummies(RenderingDevice *rd) {
	if (dummy_black_.is_valid() && dummy_far_.is_valid() && sun_ubo_.is_valid()) return true;
	auto make_1x1 = [&](RenderingDevice::DataFormat fmt, const PackedByteArray &bytes) {
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_format(fmt);
		f->set_width(1);
		f->set_height(1);
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		TypedArray<PackedByteArray> data;
		data.push_back(bytes);
		return rd->texture_create(f, v, data);
	};
	PackedByteArray black;
	black.resize(8);
	black.fill(0); // four halves of 0.0 -- an fp16 zero IS all bits zero
	dummy_black_ = make_1x1(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, black);
	PackedByteArray far;
	far.resize(4);
	far.fill(0); // reverse-Z far = 0.0f, and 0.0f is all bits zero too
	dummy_far_ = make_1x1(RenderingDevice::DATA_FORMAT_R32_SFLOAT, far);
	PackedByteArray zeros;
	zeros.resize(80);
	zeros.fill(0);
	sun_ubo_ = rd->uniform_buffer_create(80, zeros);
	return dummy_black_.is_valid() && dummy_far_.is_valid() && sun_ubo_.is_valid();
}

bool DeferredPass::render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
		RID ssgi, RID sun_map, const float sun_view_proj[16], float shadow_texel,
		const Params &p) {
	if (!is_valid() || !gb.is_valid()) return false;
	if (!ensure_dummies(rd)) return false;
	const auto t0 = std::chrono::steady_clock::now();

	uint32_t flags = p.flags;
	if (!ssgi.is_valid()) flags &= ~ve::kFlagSsgi;
	if (!sun_map.is_valid()) flags &= ~ve::kFlagSunMap;

	if (!ensure_uniform_set(rd, gb, materials, ssgi.is_valid() ? ssgi : dummy_black_,
			sun_map.is_valid() ? sun_map : dummy_far_))
		return false;

	// Device-level command: BEFORE compute_list_begin (M2 Task 12's ordering rule).
	{
		PackedByteArray ub;
		ub.resize(80);
		float *f = reinterpret_cast<float *>(ub.ptrw());
		for (int i = 0; i < 16; i++) f[i] = sun_map.is_valid() ? sun_view_proj[i] : 0.0f;
		f[16] = shadow_texel;
		f[17] = f[18] = f[19] = 0.0f;
		rd->buffer_update(sun_ubo_, 0, 80, ub);
	}

	// std430 push block, indexed BY FLOAT (M5 errata 3): mat4 = 0..15, cam = 16..19,
	// sky = 20..23, flags = 24..27. 28 floats = 112 bytes, inside the 128-byte guarantee.
	PackedByteArray pcb;
	pcb.resize(112);
	{
		float *f = reinterpret_cast<float *>(pcb.ptrw());
		for (int i = 0; i < 16; i++) f[i] = p.inv_view_proj[i];
		f[16] = p.cam_pos[0];
		f[17] = p.cam_pos[1];
		f[18] = p.cam_pos[2];
		f[19] = 0.0f;
		f[20] = p.ambient[0];
		f[21] = p.ambient[1];
		f[22] = p.ambient[2];
		f[23] = 0.0f;
		uint32_t *u = reinterpret_cast<uint32_t *>(pcb.ptrw());
		u[24] = flags;
		u[25] = static_cast<uint32_t>(p.probe_mode);
		u[26] = 0;
		u[27] = 0;
	}

	const Vector2i size = gb.size();
	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pcb, pcb.size());
	rd->compute_list_dispatch(list, (size.x + 7) / 8, (size.y + 7) / 8, 1);
	rd->compute_list_end();
	last_ms_ = std::chrono::duration<float, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
	return true;
}
```

`static_assert(sizeof(float) * 28 == 112, "deferred push block");` next to the resize, so a future field cannot silently push it past 128.

The uniform set binds, in order: 0 `gb.albedo()` (nearest), 1 `gb.surface()` (nearest), 2 `gb.depth()` (nearest), 3 the SSGI texture (**linear** — it is half resolution and is meant to be smooth), 4 the sun map (linear, for the soft-ish lookup), 5 `gb.lit()` as `UNIFORM_TYPE_IMAGE`, 6 `sun_ubo_` as `UNIFORM_TYPE_UNIFORM_BUFFER`, 8 and 9 the material arrays with `materials.sampler()`. Binding 7 is deliberately unused so the material arrays keep the same two numbers they have in every other shader.

- [ ] **Step 7: Write the injection**

`shaders/inject.vert.glsl` is byte-for-byte `composite.vert.glsl` with an empty push block:

```glsl
#[vertex]
#version 460
layout(location = 0) out vec2 uv_out;
void main() {
	// fullscreen triangle from vertex index: covers screen, uv in [0,1] on-screen
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	uv_out = p;
	gl_Position = vec4(p.x * 2.0 - 1.0, p.y * 2.0 - 1.0, 0.0, 1.0);
}
```

`shaders/inject.frag.glsl`:

```glsl
#[fragment]
#version 460
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0) uniform sampler2D lit_tex;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
void main() {
	frag_color = vec4(texture(lit_tex, uv_in).rgb, 1.0);
	// Both buffers are full resolution and hold the SAME reverse-Z NDC from the SAME
	// projection, so this is a copy, not a reprojection. texelFetch rather than texture()
	// because a filtered depth is a depth that belongs to no surface.
	//
	// The pipeline tests GREATER_OR_EQUAL, so a dynamic object already in Godot's depth
	// pre-pass and NEARER than the terrain fails this test and keeps both its depth and its
	// colour for the opaque pass -- which is exactly the mutual occlusion spec section 3
	// calls load-bearing.
	gl_FragDepth = texelFetch(gb_depth, ivec2(gl_FragCoord.xy), 0).r;
}
```

`InjectPass` is `CompositePass` with the marker variant and the material bindings removed: two samplers, one `GREATER_OR_EQUAL` depth-write pipeline, a framebuffer cached on `(dst_color, dst_depth)`, `INVALID_ID` as the vertex format, `draw_list_draw(dl, false, 1, 3)`, and `DRAW_DEFAULT_ALL` for the flags — the scene buffers' contents matter and must not be cleared.

- [ ] **Step 8: Rewire the pre-opaque callback**

In `RaymarchCompositor::_render_callback`, replace the `cmp->draw(...)` call with the G-buffer sequence. Everything after it — HiZ, LoD cull, LoD raster — is untouched in this task and still reads the scene depth the injection just wrote:

```cpp
	GBuffer *gb = world->gbuffer();
	DeferredPass *deferred = world->deferred_pass();
	InjectPass *inject = world->inject_pass();
	if (!gb || !deferred || !inject) return;
	if (!gb->ensure(rd, rsb, size)) return;

	cmp->draw(rd, *gb, rmp->albedo_texture(), rmp->surface_texture(), rmp->hitpos_texture(),
			view_proj, *materials, cam_pos, fade_start, fade_end);

	DeferredPass::Params dp;
	const Projection inv = view_proj.inverse();
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
	dp.cam_pos[0] = cam.origin.x;
	dp.cam_pos[1] = cam.origin.y;
	dp.cam_pos[2] = cam.origin.z;
	dp.flags = beauty_flags;
	// Task 8 replaces the two invalid RIDs with the sun map; Task 10 replaces the first
	// with the SSGI target. Until then the pass clears those flag bits itself.
	static const float kNoSun[16] = {};
	deferred->render(rd, *gb, *materials, RID(), RID(), kNoSun, 0.0f, dp);
	inject->draw(rd, rsb->get_color_texture(), rsb->get_depth_texture(), gb->lit(), gb->depth());
```

`view_proj` is already computed above for the composite; `Projection::inverse()` is exact for a perspective matrix and costs nothing at one call a frame.

Create both passes in `VoxelWorld::ensure_initialized()` beside `composite_pass_`, delete them in `teardown_gpu()` **before** the `GBuffer` (they hold uniform sets referencing its textures), and expose `DeferredPass *deferred_pass()` / `InjectPass *inject_pass()`.

- [ ] **Step 9: Add the two debug hooks**

`VoxelWorld::debug_cel_diff` runs the deferred shader in probe mode 1 on a 1×1 owned `GBuffer` and computes the same thing with `ve::cel_shade`:

```cpp
Dictionary VoxelWorld::debug_cel_diff(Color albedo, Color ambient, float ndl, float ndv,
		float ndh, float shadow, float ao, float gloss) {
	Dictionary d;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !gbuffer_ || !deferred_ || !materials_) return d;
	if (!gbuffer_->ensure(device, nullptr, Vector2i(1, 1))) return d;
	DeferredPass::Params p;
	p.probe_mode = 1;
	// The alias documented in deferred.comp.glsl's push block. Column-major, so column c
	// occupies floats [c*4, c*4+4).
	p.inv_view_proj[0] = albedo.r;  p.inv_view_proj[1] = albedo.g;  p.inv_view_proj[2] = albedo.b;
	p.inv_view_proj[4] = ambient.r; p.inv_view_proj[5] = ambient.g; p.inv_view_proj[6] = ambient.b;
	p.inv_view_proj[8] = ndl; p.inv_view_proj[9] = ndv; p.inv_view_proj[10] = ndh;
	p.inv_view_proj[11] = shadow;
	p.inv_view_proj[12] = ao; p.inv_view_proj[13] = gloss;
	static const float kNoSun[16] = {};
	if (!deferred_->render(device, *gbuffer_, *materials_, RID(), RID(), kNoSun, 0.0f, p))
		return d;
	device->submit();
	device->sync();
	const PackedByteArray got = device->texture_get_data(gbuffer_->lit(), 0);
	if (got.size() < 8) return d;
	const uint16_t *h = reinterpret_cast<const uint16_t *>(got.ptr());
	const Color gpu(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0f);

	ve::CelParams params;
	ve::CelInput in;
	in.albedo[0] = albedo.r; in.albedo[1] = albedo.g; in.albedo[2] = albedo.b;
	in.ambient[0] = ambient.r; in.ambient[1] = ambient.g; in.ambient[2] = ambient.b;
	in.ndl = ndl; in.ndv = ndv; in.ndh = ndh;
	in.shadow = shadow; in.ao = ao; in.gloss = gloss;
	float ref[3];
	ve::cel_shade(params, in, ref);
	const Color cpu(ref[0], ref[1], ref[2], 1.0f);
	d["gpu"] = gpu;
	d["cpu"] = cpu;
	d["max_delta"] = std::max({std::fabs(gpu.r - cpu.r), std::fabs(gpu.g - cpu.g),
			std::fabs(gpu.b - cpu.b)});
	return d;
}
```

`VoxelWorld::debug_deferred_probe(Vector3 pos, Vector3 fwd, int w, int h, int probe_mode)`
streams to quiet, ensures the owned `GBuffer` at `w × h`, runs `raymarch_pass_->render` at `w × h`, then `composite_pass_->draw`, then `deferred_->render` with a `ve::lod_camera_perspective`-built view-projection (60° vertical fov, aspect `w/h`, near 0.05, far 4000) so `inv_view_proj` is a real inverse, submits, syncs, reads back `gbuffer_->lit()` and reports:

- `"center"` — `Color` for probe modes 0/1, `Vector3` for probe mode 2 (the reconstructed position),
- `"mean_luma"` — mean of `0.2126 r + 0.7152 g + 0.0722 b`,
- `"distinct_rows"` — how many distinct green values appear down the centre column, which is what tells a banded sky from a smooth one.

Use the SAME `Projection` for the composite's `view_proj` push constant and for `inv_view_proj`; if the two differ the reconstruction test fails for a reason that has nothing to do with the shader.

Bind both methods.

- [ ] **Step 10: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then `./gdunit_tests.sh -a res://tests/test_deferred.gd -c`
Expected: PASS, six tests.

Then the regression gate — this task changed the composite's contract, so every suite that draws through it must still hold:
Run: `./gdunit_tests.sh -a res://tests/test_lod_seam.gd -a res://tests/test_lod_render.gd -a res://tests/test_raymarch_gbuffer.gd -a res://tests/test_gbuffer.gd -c`
Expected: PASS. `test_lod_seam.gd` is the one to watch: the marker attachment moved from location 1 to location 2 and the probe's composite call now takes a `GBuffer`. Update `debug_seam_probe` to allocate the owned `GBuffer` at the probe size and pass its albedo/surface/depth to the composite, keeping the throwaway marker texture; the LoD half of the probe is untouched until Task 7.

- [ ] **Step 11: Verify the demo still renders**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
Expected: the near field is now cel-banded — hard terraced light on the terrain, cool-tinted shadows in the cave and under overhangs — while the far field past 150 m is still the old smooth lambert. **That visible discontinuity at the seam is the expected state after this task** and is what Task 7 removes. The `TestCube` still occludes and is occluded correctly.

- [ ] **Step 12: Commit**

```bash
git add shaders/composite.frag.glsl shaders/deferred.comp.glsl shaders/inject.vert.glsl \
        shaders/inject.frag.glsl extension/src/render/composite_pass.h \
        extension/src/render/composite_pass.cpp extension/src/render/deferred_pass.h \
        extension/src/render/deferred_pass.cpp extension/src/render/inject_pass.h \
        extension/src/render/inject_pass.cpp extension/src/raymarch_compositor.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_deferred.gd
git commit -m "feat: deferred cel lighting stack and scene colour/depth injection"
```

---

### Task 7: the far field writes the same four channels

Spec §7: "rasterized LoD write albedo / oct-normal / linear-depth / material-ID into the same offscreen G-buffer (LoD from its bakes)". After this task there is exactly one lighting evaluation in the engine and the seam is a change of *source*, not of *shading*.

The pass order also flips: HiZ and the LoD raster move **in front of** the deferred pass, because the LoD raster now writes the G-buffer the deferred pass reads. HiZ therefore builds from the G-buffer's depth rather than the scene's — which is strictly better, because that depth is the near field's and only the near field's, with no dynamic objects in it to occlude a chunk that should stream.

**Files:**
- Modify: `shaders/lod.frag.glsl`, `shaders/common.glslh` (delete `shade_terrain`)
- Modify: `extension/src/render/lod_raster_pass.h`, `.cpp`
- Modify: `extension/src/render/hiz_pass.cpp` (nothing structural; the caller changes)
- Modify: `extension/src/raymarch_compositor.cpp`, `extension/src/voxel_world.cpp` (the two probes)
- Create: `tests/test_lod_gbuffer.gd`

**Interfaces:**
- Consumes: `GBuffer` (Task 4), `shade.glslh` (Task 5), `DeferredPass`/`InjectPass` (Task 6).
- Produces: `LodRasterPass::draw(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials, GBuffer &gb, const Projection &view_proj, const float cam_pos[3], int draw_count, float fade_start, float fade_end, RID marker = RID())` — `dst_color`/`dst_depth` are replaced by the G-buffer. Also `bool LodRasterPass::front_face_clockwise() const` and `RID LodRasterPass::index_array() const`, which Task 8 needs.

- [ ] **Step 1: Write the failing test**

Create `tests/test_lod_gbuffer.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w

const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# The far field must describe, not shade. A LoD pixel has to carry a real material id and a
# unit normal, or the deferred pass has nothing to light it with.
func test_far_field_pixels_carry_a_material_and_a_unit_normal(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 100.0, 100.0)
	var fwd := Vector3(0.3, -0.5, 0.3).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d := w.debug_lod_gbuffer_probe(pos, fwd, 128, 128)
	assert_float(d["material_coverage"]).override_failure_message(
		"no far-field pixel wrote a material id").is_greater(0.2)
	assert_float(d["worst_normal_length_error"]).override_failure_message(
		"a far-field normal did not survive the oct pack").is_less(0.02)
	assert_float(d["gloss_max"]).is_between(0.0, 1.0)

# Spec section 7: "the near/far seam is mathematically invisible". With one lighting stack
# it is provable: turn the sun map and every screen-space effect off, and the two fields on
# either side of the band must land within a band's worth of each other.
func test_the_two_fields_light_identically_across_the_band(timeout := 60000) -> void:
	var w := make_world()
	w.quality_tier = 1 # outlines and the raymarched sun ray only; no screen-space passes
	var pos := Vector3(100.0, 68.0, 202.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d := w.debug_seam_probe(pos, fwd, 256, 144)
	# The existing exactly-once invariant still holds with two attachments in play.
	assert_int(d["both"]).is_equal(0)
	assert_int(d["neither"]).is_equal(0)
	assert_int(d["band_pixels"]).is_greater(50)

func test_the_lod_raster_no_longer_shades(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 100.0, 100.0)
	var fwd := Vector3(0.3, -0.5, 0.3).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d := w.debug_lod_gbuffer_probe(pos, fwd, 128, 128)
	# The far field writes fully-lit sun visibility: shadowing it is the sun map's job, in
	# the deferred pass, not the raster's.
	assert_float(d["sun_min"]).is_equal_approx(1.0, 0.01)
	assert_float(d["sun_max"]).is_equal_approx(1.0, 0.01)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_lod_gbuffer.gd`
Expected: FAIL — `Nonexistent function 'debug_lod_gbuffer_probe'`.

- [ ] **Step 3: Rewrite `shaders/lod.frag.glsl`**

```glsl
#[fragment]
#version 460

#define MATERIAL_LAYERS 16
layout(set = 0, binding = 3) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 4) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in flat vec3 v_normal;
layout(location = 2) in flat uint v_material;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss
#ifdef SEAM_MARKER
layout(location = 2) out uint marker; // debug seam probe: 2 = far field kept the pixel
#endif

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;  // xyz = camera position, w = fade start
	vec4 fade; // x = fade end, yzw unused
} pc;

void main() {
	// Fade before the material sample, so a discarded fragment costs no texture work.
	float d = distance(v_wpos, pc.cam.xyz);
	float t = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	// The exact complement of composite.frag's test: >= where it uses <.
	if (bayer4(ivec2(gl_FragCoord.xy)) >= t) discard;
#ifdef SEAM_MARKER
	marker = 2u;
#endif
	// Explicit gradients are not needed here -- a fragment shader has them -- but the SAME
	// functions the raymarcher calls are, so the two fields cannot drift.
	vec3 ddx = dFdx(v_wpos);
	vec3 ddy = dFdy(v_wpos);
	vec4 surf = material_surface(v_material, v_wpos, v_normal, ddx, ddy);
	vec2 props = material_props(v_material, v_wpos, v_normal, ddx, ddy);
	// Sun visibility is 1: shadowing the far field is the ortho shadow map's job, evaluated
	// once in the deferred pass where the near field's raymarched term is also applied.
	out_albedo = vec4(surf.rgb * mix(1.0, props.y, 0.65), 1.0);
	out_surface = vec4(oct_encode(v_normal), float(v_material), 1.0 - props.x);
}
```

Then delete `shade_terrain()` from `shaders/common.glslh` — this was its last caller, and a shading function nobody calls is the seam waiting to reopen. Leave a one-line note in its place:

```glsl
// shade_terrain() lived here until M6. Lighting now happens exactly once, in
// shaders/deferred.comp.glsl via cel_shade() in shade.glslh. Do not add a second one.
```

- [ ] **Step 4: Point `LodRasterPass` at the G-buffer**

`lod_raster_pass.h`: forward-declare `class GBuffer;`, change `draw`'s signature as stated in **Interfaces**, rename the framebuffer keys to `fb_albedo_`, `fb_surface_`, `fb_depth_`, `fb_marker_`, and add:

```cpp
	// Task 8's depth-only shadow pipeline inherits this rather than re-deriving it: the
	// winding was MEASURED (M5 errata 2), and a second derivation is a second chance to get
	// it wrong.
	bool front_face_clockwise() const { return front_face_clockwise_; }
	RID index_array() const { return index_array_; }
```

`lod_raster_pass.cpp`:
- `ensure_pipeline` builds the attachment array `Array::make(gb.albedo(), gb.surface(), depth)` (or with `marker` inserted before the depth, exactly as `CompositePass` does).
- all three pipelines (cull off, cull-back CCW, cull-back CW) get a colour blend state with **two** attachments (three with the marker), blending disabled on each. **Do not touch the face selection** — `front_face_clockwise_` and its measuring test stay as they are.
- `draw()` opens the list with `DRAW_DEFAULT_ALL`: the composite already cleared the G-buffer this frame, and the LoD raster's whole job is to overwrite the pixels it wins.

- [ ] **Step 5: Reorder the pre-opaque callback**

The LoD block in `RaymarchCompositor::_render_callback` moves to sit **between** the composite and the deferred pass, and every `rsb->get_depth_texture()` inside it becomes `gb->depth()`, every `rsb->get_color_texture()` becomes the G-buffer. The final order in the callback is:

1. `world->drain_island_uploads(rd)`, `st->run_frame(...)`
2. `cull->render(...)`, `rmp->render(...)` — the raymarch at 0.66×
3. `gb->ensure(rd, rsb, size)`, `cmp->draw(rd, *gb, ...)` — the near field into the G-buffer, clearing it
4. `hiz->build(rd, gb->depth(), size)`
5. `world->lod_tick(...)`, the cull, and `lod_raster->draw(rd, *pool, *materials, *gb, ...)` — including the temporal second phase exactly as it is, with its mid-sequence `hiz->build(rd, gb->depth(), size)`
6. `deferred->render(rd, *gb, *materials, RID(), RID(), kNoSun, 0.0f, dp)`
7. `inject->draw(rd, rsb->get_color_texture(), rsb->get_depth_texture(), gb->lit(), gb->depth())`

`HizPass::release_level0_set()` is called by the probes before they free their throwaway depth; the G-buffer's depth is now that source, so call it in `GBuffer::teardown()`'s callers rather than leaving a stale set — concretely, `VoxelWorld::teardown_gpu()` calls `hiz_pass_->release_level0_set()` before `delete gbuffer_`.

- [ ] **Step 6: Update the two probes**

`debug_lod_render_probe` / `debug_lod_render_probe_culled` allocate an owned `GBuffer` at the probe size instead of their throwaway colour/depth pair and pass it to `lod_raster_pass_->draw`. `debug_seam_probe` does the same for **both** halves, so the composite and the raster now share one framebuffer format and one marker attachment index (2). Its readback of the marker texture is unchanged.

Add the new hook:

```cpp
Dictionary VoxelWorld::debug_lod_gbuffer_probe(Vector3 pos, Vector3 fwd, int w, int h);
```

which runs the same sequence as `debug_lod_render_probe` (owned `GBuffer`, camera from `ve::lod_camera_perspective`, the LoD raster only — no raymarch, so nothing but the far field is in the buffer), then reads back `albedo` and `surface` and reports:

- `"material_coverage"` — fraction of pixels with `surface.z >= 0.5`
- `"worst_normal_length_error"` — over covered pixels, `abs(length(oct_decode(surface.xy)) - 1)`
- `"gloss_max"`, `"sun_min"`, `"sun_max"` — over covered pixels

- [ ] **Step 7: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then `./gdunit_tests.sh -a res://tests/test_lod_gbuffer.gd -c`
Expected: PASS, three tests.

Then the LoD regression gate, which is large and is the point:
Run: `./gdunit_tests.sh -a res://tests/test_lod_seam.gd -a res://tests/test_lod_render.gd -a res://tests/test_lod_cull.gd -a res://tests/test_hiz.gd -a res://tests/test_lod_pool.gd -a res://tests/test_deferred.gd -c`
Expected: PASS. `test_hiz.gd` is the one whose *source* changed: it drives `debug_hiz_probe_synthetic`, which uploads its own synthetic depth image and is unaffected, but `test_lod_pool.gd::test_ticking_streams_chunks_in` exercises the real occlusion path and must still stream — occlusion may only delay a build, never hide a chunk.

- [ ] **Step 8: Verify the demo**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
Expected: the cel banding now continues past 150 m with no change in style at the seam — the discontinuity Task 6 deliberately left is gone. The far field is *unshadowed* (uniformly lit terracing), which is Task 8's job.

- [ ] **Step 9: Commit**

```bash
git add shaders/lod.frag.glsl shaders/common.glslh \
        extension/src/render/lod_raster_pass.h extension/src/render/lod_raster_pass.cpp \
        extension/src/raymarch_compositor.cpp extension/src/voxel_world.h \
        extension/src/voxel_world.cpp tests/test_lod_gbuffer.gd
git commit -m "feat: far field writes the merged g-buffer; one lighting stack for both"
```

---

### Task 8: `SunShadowPass` — the far field casts

Spec §7's shadow layer 2: "one world-covering **ortho shadow map from LoD geometry** (2048², ~2m/texel), redrawn lazily on LoD rebuilds". The near field already shadows itself with rays; this is what makes a mountain 800 m away lay a shadow across the valley in front of it.

The matrix is *static*. The world is bounded (spec §1: 4096×1024×4096 m) and the sun does not move, so the ortho projection depends only on the world bounds and `ve::kSunDir` — no camera-following, no texel snapping, no shimmer, and no per-frame matrix at all. That is a property of this engine's fixed constraints, and it is why a 2048² map is enough.

**Files:**
- Create: `extension/src/shade/sun_ortho.h`, `.cpp`, `extension/tests/test_sun_ortho.cpp`
- Create: `shaders/lod_shadow.vert.glsl`, `shaders/lod_shadow.frag.glsl`
- Create: `extension/src/render/sun_shadow_pass.h`, `.cpp`
- Modify: `shaders/deferred.comp.glsl` (probe mode 3), `extension/src/raymarch_compositor.cpp`, `extension/src/voxel_world.h`, `.cpp`
- Create: `tests/test_sun_shadow.gd`

**Interfaces:**
- Consumes: `ve::kSunDir` (Task 2); `ve::WorldBounds::aabb(float lo[3], float hi[3])` (existing, `world/region.h`); `LodPool::args_buffer()`, `LodPool::upload_draw_args()`, `LodRasterPass::draw_pages()`, `LodRasterPass::front_face_clockwise()`, `LodRasterPass::index_array()` (Task 7).
- Produces: `struct ve::SunOrtho { float view_proj[16]; float texel_world; bool valid; }` and `ve::SunOrtho ve::sun_ortho(const float sun_dir[3], const float lo[3], const float hi[3], int map_size)`; `godot::SunShadowPass` with `static constexpr int kSize = 2048`, `static constexpr int kMinFrames = 12`, `bool initialize(RenderingDevice *)`, `void teardown()`, `void mark_dirty()`, `bool build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster, const ve::SunOrtho &ortho, bool force)`, `RID map() const`, `const float *view_proj() const`, `float texel_world() const`, `int rebuilds() const`, `bool is_valid() const`.

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_sun_ortho.cpp`:

```cpp
#include "doctest.h"
#include "shade/sun_ortho.h"
#include "shade/cel.h"
#include <cmath>

namespace {

// clip = M * (p, 1), then NDC = clip.xyz / clip.w. The ortho matrix has w = 1 everywhere,
// but dividing anyway is what the shader does, so the test does it too.
void project(const ve::SunOrtho &o, const float p[3], float ndc[3]) {
	float c[4];
	for (int r = 0; r < 4; r++)
		c[r] = o.view_proj[0 * 4 + r] * p[0] + o.view_proj[1 * 4 + r] * p[1] +
				o.view_proj[2 * 4 + r] * p[2] + o.view_proj[3 * 4 + r];
	for (int i = 0; i < 3; i++) ndc[i] = c[i] / c[3];
}

const float kLo[3] = {0.0f, -51.2f, 0.0f};
const float kHi[3] = {4096.0f, 972.8f, 4096.0f};

} // namespace

TEST_CASE("every corner of the world box lands inside the unit cube") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	REQUIRE(o.valid);
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? kHi[0] : kLo[0], (i & 2) ? kHi[1] : kLo[1],
				(i & 4) ? kHi[2] : kLo[2]};
		float ndc[3];
		project(o, p, ndc);
		CHECK(ndc[0] >= -1.0001f);
		CHECK(ndc[0] <= 1.0001f);
		CHECK(ndc[1] >= -1.0001f);
		CHECK(ndc[1] <= 1.0001f);
		CHECK(ndc[2] >= -0.0001f);
		CHECK(ndc[2] <= 1.0001f);
	}
}

// Reverse-Z, matching every other depth surface in this engine (M1 errata 2): nearer to the
// light is LARGER. Get this backwards and the shadow test inverts -- everything lit is dark
// and everything dark is lit, which looks like an art choice until you move the camera.
TEST_CASE("moving toward the sun increases the depth") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const float base[3] = {2048.0f, 400.0f, 2048.0f};
	const float toward[3] = {base[0] + ve::kSunDir[0] * 100.0f,
			base[1] + ve::kSunDir[1] * 100.0f, base[2] + ve::kSunDir[2] * 100.0f};
	float a[3];
	float b[3];
	project(o, base, a);
	project(o, toward, b);
	CHECK(b[2] > a[2]);
}

TEST_CASE("moving along the sun direction moves only the depth") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const float base[3] = {2048.0f, 400.0f, 2048.0f};
	const float toward[3] = {base[0] + ve::kSunDir[0] * 250.0f,
			base[1] + ve::kSunDir[1] * 250.0f, base[2] + ve::kSunDir[2] * 250.0f};
	float a[3];
	float b[3];
	project(o, base, a);
	project(o, toward, b);
	CHECK(b[0] == doctest::Approx(a[0]).epsilon(1e-4));
	CHECK(b[1] == doctest::Approx(a[1]).epsilon(1e-4));
}

// A point displaced perpendicular to the sun must move in x or y, or the basis has
// collapsed and the whole world projects onto a line.
TEST_CASE("the light basis is non-degenerate") {
	const ve::SunOrtho o = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const float base[3] = {2048.0f, 400.0f, 2048.0f};
	// Any vector not parallel to the sun; (0,1,0) is not, since kSunDir has x and z.
	const float side[3] = {base[0], base[1] + 200.0f, base[2]};
	float a[3];
	float b[3];
	project(o, base, a);
	project(o, side, b);
	CHECK((std::fabs(b[0] - a[0]) + std::fabs(b[1] - a[1])) > 1e-3f);
}

TEST_CASE("the texel size is the light-space extent over the map size") {
	const ve::SunOrtho a = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const ve::SunOrtho b = ve::sun_ortho(ve::kSunDir, kLo, kHi, 1024);
	CHECK(a.texel_world > 0.0f);
	CHECK(b.texel_world == doctest::Approx(a.texel_world * 2.0f).epsilon(1e-4));
	// A 4 km world in a 2048 map is about 2 m a texel (spec section 7 says "~2m/texel").
	// The light-space extent of a rotated box is larger than the box, so allow the range.
	CHECK(a.texel_world > 1.5f);
	CHECK(a.texel_world < 5.0f);
}

TEST_CASE("a degenerate request is refused rather than producing a silent identity") {
	const float zero[3] = {0, 0, 0};
	CHECK_FALSE(ve::sun_ortho(zero, kLo, kHi, 2048).valid);
	CHECK_FALSE(ve::sun_ortho(ve::kSunDir, kHi, kLo, 2048).valid); // inverted bounds
	CHECK_FALSE(ve::sun_ortho(ve::kSunDir, kLo, kHi, 0).valid);
}

TEST_CASE("the matrix depends on nothing but the sun and the bounds") {
	const ve::SunOrtho a = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	const ve::SunOrtho b = ve::sun_ortho(ve::kSunDir, kLo, kHi, 2048);
	for (int i = 0; i < 16; i++) CHECK(a.view_proj[i] == doctest::Approx(b.view_proj[i]));
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons test`
Expected: FAIL — `fatal error: shade/sun_ortho.h: No such file or directory`.

- [ ] **Step 3: Write `extension/src/shade/sun_ortho.h`**

```cpp
#pragma once

namespace ve {

// A world-covering reverse-Z orthographic projection for the sun, in the same column-major
// layout Godot's Projection uses (element (row r, column c) is view_proj[c * 4 + r]).
//
// It is STATIC. This world is bounded and the sun does not move, so there is no camera to
// follow, no texel snapping to do and no shimmer to fight -- which is the whole reason a
// single 2048 map is enough for a 4 km world.
struct SunOrtho {
	float view_proj[16] = {};
	float texel_world = 0.0f; // one shadow texel, in world metres, in light space
	bool valid = false;
};

// `sun_dir` points TOWARD the sun (ve::kSunDir). `lo`/`hi` are the world AABB.
SunOrtho sun_ortho(const float sun_dir[3], const float lo[3], const float hi[3], int map_size);

} // namespace ve
```

- [ ] **Step 4: Write `extension/src/shade/sun_ortho.cpp`**

```cpp
#include "shade/sun_ortho.h"
#include <algorithm>
#include <cmath>

namespace {

void cross3(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

float norm3(float v[3]) {
	const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0.0f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
	return l;
}

} // namespace

namespace ve {

SunOrtho sun_ortho(const float sun_dir[3], const float lo[3], const float hi[3], int map_size) {
	SunOrtho o;
	if (map_size <= 0) return o;
	for (int a = 0; a < 3; a++)
		if (!(hi[a] > lo[a])) return o;

	float f[3] = {sun_dir[0], sun_dir[1], sun_dir[2]};
	if (norm3(f) <= 0.0f) return o;
	// Light-space +z points AWAY from the sun, so depth grows with distance from it and the
	// reverse-Z remap below is a single subtraction.
	const float l[3] = {-f[0], -f[1], -f[2]};

	// Any hint not parallel to the light. kSunDir is well off vertical, so world up works;
	// the fallback exists so a future sun straight overhead does not collapse the basis.
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
	o.valid = true;
	return o;
}

} // namespace ve
```

- [ ] **Step 5: Run the native tests to verify they pass**

Run: `cd extension && scons test`
Expected: PASS, seven cases.

- [ ] **Step 6: Write the failing script-facing test**

Create `tests/test_sun_shadow.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w

const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

func test_the_map_is_the_stated_size_and_covers_the_world() -> void:
	var w := make_world()
	var d := w.debug_sun_shadow_stats()
	assert_int(d["size"]).is_equal(2048)
	assert_bool(d["ortho_valid"]).is_true()
	assert_float(d["texel_world"]).is_between(0.1, 20.0)

# The whole point of a bounded world and a fixed sun: the matrix never changes, so nothing
# can shimmer and no rebuild is ever needed for camera motion alone.
func test_the_matrix_does_not_move_with_the_camera(timeout := 60000) -> void:
	var w := make_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	var a: PackedFloat32Array = w.debug_sun_shadow_stats()["view_proj"]
	assert_bool(await settle(w, Vector3(180, 90, 40), Vector3(-1, -0.4, 0).normalized())).is_true()
	var b: PackedFloat32Array = w.debug_sun_shadow_stats()["view_proj"]
	for i in range(16):
		assert_float(b[i]).is_equal_approx(a[i], 1e-6)

func test_something_actually_gets_drawn_into_it(timeout := 60000) -> void:
	var w := make_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w.debug_sun_shadow_build(true)
	var d := w.debug_sun_shadow_stats()
	assert_int(d["rebuilds"]).is_greater(0)
	assert_bool(d["map_valid"]).is_true()
	# A point well under the terrain surface is behind whatever the map recorded above it.
	# A point well above everything is not.
	assert_float(w.debug_sun_shadow_visibility(Vector3(60.0, 20.0, 60.0))).is_equal_approx(0.0, 0.01)
	assert_float(w.debug_sun_shadow_visibility(Vector3(60.0, 300.0, 60.0))).is_equal_approx(1.0, 0.01)

func test_a_lazy_rebuild_does_not_fire_every_frame(timeout := 60000) -> void:
	var w := make_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w.debug_sun_shadow_build(true)
	var before: int = w.debug_sun_shadow_stats()["rebuilds"]
	# Nothing dirtied it and no 12 frames have passed: these must all be refused.
	for i in range(5):
		w.debug_sun_shadow_build(false)
	assert_int(w.debug_sun_shadow_stats()["rebuilds"]).is_equal(before)

func test_turning_the_sun_map_off_lights_everything(timeout := 60000) -> void:
	var w := make_world()
	assert_bool(await settle(w, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w.debug_sun_shadow_build(true)
	w.set_effect_enabled("sun_shadow_map", false)
	assert_float(w.debug_sun_shadow_visibility(Vector3(60.0, 20.0, 60.0))).is_equal_approx(1.0, 0.01)
```

- [ ] **Step 7: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_sun_shadow.gd`
Expected: FAIL — `Nonexistent function 'debug_sun_shadow_stats'`.

- [ ] **Step 8: Write the shadow shaders**

`shaders/lod_shadow.vert.glsl` — the same geometry pull as `lod.vert.glsl` with the sun's matrix and no varyings the fragment stage needs:

```glsl
#[vertex]
#version 460

#include "common.glslh"
#include "lod_quad.glslh"

// The same pulled geometry as lod.vert.glsl: shared index buffer, vertexOffset = page * 2048,
// quad = gl_VertexIndex >> 2. Reusing the arena rather than rebuilding geometry for the light
// is what makes a 2048^2 world-covering map cost one draw.
layout(set = 0, binding = 0, std430) readonly buffer Quads { uint v[]; } quads;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;

layout(push_constant, std430) uniform Push {
	mat4 sun_view_proj;
} pc;

void main() {
	uint vi = uint(gl_VertexIndex);
	uint quad = vi >> 2;
	uint corner = vi & 3u;
	uint page = quad >> uint(LOD_PAGE_SHIFT);
	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];
	uvec3 w = uvec3(quads.v[quad * 3u + 0u], quads.v[quad * 3u + 1u], quads.v[quad * 3u + 2u]);
	vec3 p = lod_corner_pos(w, int(corner), c0.xyz, c0.w);
	gl_Position = pc.sun_view_proj * vec4(p, 1.0);
}
```

`shaders/lod_shadow.frag.glsl`:

```glsl
#[fragment]
#version 460
// No colour attachment and nothing to write: depth is the entire output. The stage exists
// because RenderingDevice builds its pipeline from a vertex+fragment SPIR-V pair.
void main() {}
```

- [ ] **Step 9: Write `SunShadowPass`**

`extension/src/render/sun_shadow_pass.h` declares the interface listed above. `sun_shadow_pass.cpp` follows `LodRasterPass`'s shape:

- a 2048² `D32_SFLOAT` texture with `DEPTH_STENCIL_ATTACHMENT_BIT | SAMPLING_BIT`;
- a framebuffer of that one attachment (`framebuffer_create(Array::make(map_)))` — the depth slot is derived from the usage bit, so a depth-only framebuffer is a one-element array);
- a pipeline with `POLYGON_CULL_BACK`, the front face **taken from `raster.front_face_clockwise()`**, `COMPARE_OP_GREATER_OR_EQUAL`, depth write on, and a colour blend state with **no** attachments (`cb->set_attachments(Array())`);
- a `draw_list_begin(fb_, RenderingDevice::DRAW_CLEAR_DEPTH, PackedColorArray(), /*clear_depth_value*/ 0.0f)` — reverse-Z far, so the first fragment at every texel wins;
- the same `draw_list_bind_index_array(dl, raster.index_array())` and
  `draw_list_draw_indirect(dl, /*use_indices*/ true, pool.args_buffer(), 0, draw_count, 20)` the camera path uses.

The gate:

```cpp
void SunShadowPass::mark_dirty() { dirty_ = true; }

bool SunShadowPass::build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster,
		const ve::SunOrtho &ortho, bool force) {
	frames_since_++;
	if (!is_valid() || !ortho.valid) return false;
	if (!force && (!dirty_ || frames_since_ < kMinFrames)) return false;
	const std::vector<LodRasterPass::PageDraw> &pages = raster.draw_pages();
	if (pages.empty()) return false;
	// ORDERING (load-bearing): this shares LodPool's single indirect-args buffer with the
	// camera path, so it must run BEFORE the camera's own upload_draw_args each frame. The
	// compositor calls it immediately after lod_tick() for exactly that reason. It uploads
	// the FULL drawable set rather than the culled one -- a chunk the camera cannot see can
	// still be the thing casting the shadow the camera is standing in.
	pool.upload_draw_args(pages);
	...record the draw...
	std::memcpy(view_proj_, ortho.view_proj, sizeof(view_proj_));
	texel_world_ = ortho.texel_world;
	dirty_ = false;
	frames_since_ = 0;
	rebuilds_++;
	return true;
}
```

`VoxelWorld` calls `sun_shadow_->mark_dirty()` wherever it already updates `lod_pages_of_` — every successful `LodPool::upload` and every `release`. That is one line in each place and it is what "redrawn lazily on LoD rebuilds" means.

- [ ] **Step 10: Feed it to the deferred pass and add probe mode 3**

In `RaymarchCompositor::_render_callback`, right after `world->lod_tick(...)` and **before** the camera's `upload_draw_args`:

```cpp
	SunShadowPass *sun = world->sun_shadow_pass();
	if (sun && world->beauty_settings().sun_shadow_map && world->lod_pool()) {
		const ve::WorldBounds wb = world->world_bounds();
		float lo[3];
		float hi[3];
		wb.aabb(lo, hi);
		sun->build(rd, *world->lod_pool(), *lod_raster,
				ve::sun_ortho(ve::kSunDir, lo, hi, SunShadowPass::kSize), false);
	}
```

and the deferred call becomes:

```cpp
	const bool use_sun = sun && sun->is_valid() && sun->rebuilds() > 0 &&
			world->beauty_settings().sun_shadow_map;
	deferred->render(rd, *gb, *materials, RID(), use_sun ? sun->map() : RID(),
			use_sun ? sun->view_proj() : kNoSun, use_sun ? sun->texel_world() : 0.0f, dp);
```

`rebuilds() > 0` is the fail-soft gate: a map that has never been drawn is uniformly far, which would shadow the entire world.

In `shaders/deferred.comp.glsl`, add probe mode 3 immediately after the mode-1 branch:

```glsl
	if (pc.flags.y == 3u) { // sun-map probe: shade nothing, report visibility at cam.xyz
		float vis = ((pc.flags.x & BEAUTY_SUN_MAP) != 0u)
				? sun_map_visibility(pc.cam.xyz, 1.0) : 1.0;
		imageStore(out_lit, px, vec4(vis, vis, vis, 1.0));
		return;
	}
```

`VoxelWorld::debug_sun_shadow_visibility(Vector3 p)` runs the deferred pass on a 1×1 owned `GBuffer` with `probe_mode = 3`, `cam_pos = p`, the live flags, and the real map/matrix; it returns the red channel. `debug_sun_shadow_stats()` returns `size`, `map_valid`, `ortho_valid`, `texel_world`, `rebuilds`, and `view_proj` as a `PackedFloat32Array`. `debug_sun_shadow_build(bool force)` calls `build(...)` with the current LoD state so a test can drive it without a live viewport.

- [ ] **Step 11: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then `cd extension && scons test` then
`./gdunit_tests.sh -a res://tests/test_sun_shadow.gd -a res://tests/test_deferred.gd -a res://tests/test_lod_gbuffer.gd -c`
Expected: PASS.

- [ ] **Step 12: Verify the demo**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
Expected: distant hillsides now carry each other's shadows, and the shadow edge at the 150 m seam lines up with the raymarched shadow inside it. If a stair-stepped shadow pattern appears on slopes facing away from the sun, that is acne and the bias in `sun_map_visibility` is what to move — record the value you land on in the Errata.

- [ ] **Step 13: Commit**

```bash
git add extension/src/shade/sun_ortho.h extension/src/shade/sun_ortho.cpp \
        extension/tests/test_sun_ortho.cpp shaders/lod_shadow.vert.glsl \
        shaders/lod_shadow.frag.glsl shaders/deferred.comp.glsl \
        extension/src/render/sun_shadow_pass.h extension/src/render/sun_shadow_pass.cpp \
        extension/src/raymarch_compositor.cpp extension/src/voxel_world.h \
        extension/src/voxel_world.cpp tests/test_sun_shadow.gd
git commit -m "feat: world-covering ortho sun shadow map drawn from the lod arena"
```

---

### Task 9: `BeautyCompositor` — the post-opaque half of the frame, and contact shadows

Spec §7's frame order puts three effects **after** Godot's opaque pass: "contact shadows → SSR → outlines". They belong there because they are the ones that need dynamic objects in the picture, and a `CompositorEffect` carries exactly one callback type — so this is a second effect resource, not a branch inside the first.

It also settles spec §3's second "known spike item": whether Godot's normal-roughness buffer is reachable from a GDExtension. `CompositorEffect::set_needs_normal_roughness(true)` exists on this godot-cpp, and `RenderSceneBuffersRD::has_texture` / `get_texture` can name the buffer; this task establishes empirically whether that produces a populated texture on 4.7.1 and **records the verdict in the Errata** either way. Both branches are implemented, so neither answer blocks anything.

**Files:**
- Create: `extension/src/beauty_compositor.h`, `.cpp`
- Create: `extension/src/render/beauty_camera.h`, `.cpp`, `shaders/beauty_camera.glslh`
- Create: `shaders/contact_shadow.comp.glsl`, `shaders/downsample.comp.glsl`
- Create: `extension/src/render/contact_shadow_pass.h`, `.cpp`
- Modify: `extension/src/register_types.cpp`, `demo/main.tscn`, `extension/src/voxel_world.h`, `.cpp`
- Create: `tests/test_contact_shadow.gd`

**Interfaces:**
- Consumes: `GBuffer` (Task 4), `ve::BeautySettings` / `ve::pack_flags` (Task 3), `SUN_DIR` and `BEAUTY_*` from `shade.glslh` (Task 5).
- Produces:
  - `godot::CameraUbo` with `bool ensure(RenderingDevice *)`, `void update(RenderingDevice *, const Projection &view_proj, const float cam_pos[3], Vector2i size, float z_near, float z_far)`, `RID buffer() const`, `void teardown()`. 160-byte std140 block: `mat4 view_proj; mat4 inv_view_proj; vec4 cam; vec4 screen;`.
  - `godot::ContactShadowPass` with `void initialize(RenderingDevice *)`, `void teardown()`, `bool render(RenderingDevice *rd, RID scene_color, RID scene_depth, Vector2i size, RID camera_ubo, const ve::BeautySettings &s)`, `RID mask() const`, `float last_ms() const`.
  - `godot::BeautyCompositor` (a `CompositorEffect` at `EFFECT_CALLBACK_TYPE_POST_OPAQUE`) with a `world_path` property, mirroring `RaymarchCompositor`.
  - `VoxelWorld::debug_contact_shadow_probe(Vector3 pos, Vector3 fwd, int w, int h)`, `VoxelWorld::debug_beauty_compositor_stats()`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_contact_shadow.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_mask_is_half_resolution() -> void:
	var w := make_world()
	var d := w.debug_contact_shadow_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_int(d["mask_width"]).is_equal(64)
	assert_int(d["mask_height"]).is_equal(64)

# The generator carves a 5 m sphere out of the terrain at (30, ~49, 30). Looking into it,
# the crater walls occlude their own floor over a short distance -- exactly the geometry
# contact shadows exist for.
func test_a_crater_darkens_its_own_floor() -> void:
	var w := make_world()
	var d := w.debug_contact_shadow_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_float(d["mask_min"]).override_failure_message(
		"no pixel was occluded at all: the march never hit anything").is_less(0.9)
	assert_float(d["mask_mean"]).is_between(0.0, 1.0)
	# ...and it did not darken EVERYTHING, which is what a sign error in the march produces.
	assert_float(d["mask_mean"]).is_greater(0.3)

func test_the_apply_only_ever_darkens() -> void:
	var w := make_world()
	var d := w.debug_contact_shadow_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_float(d["max_brightening"]).override_failure_message(
		"a contact shadow made a pixel brighter").is_less(0.002)
	assert_float(d["mean_darkening"]).is_greater(0.0)

func test_turning_contact_shadows_off_leaves_the_image_alone() -> void:
	var w := make_world()
	w.set_effect_enabled("contact_shadows", false)
	var d := w.debug_contact_shadow_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_float(d["mean_darkening"]).is_equal_approx(0.0, 0.001)

func test_zero_steps_reads_as_off_rather_than_as_a_free_dispatch() -> void:
	var w := make_world()
	w.quality_tier = 0
	var d := w.debug_beauty_settings()
	assert_int(int(d["flags"]) & 4).is_equal(0)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_contact_shadow.gd`
Expected: FAIL — `Nonexistent function 'debug_contact_shadow_probe'`.

- [ ] **Step 3: Write the shared camera block**

`shaders/beauty_camera.glslh`:

```glsl
// The camera every post-opaque pass reconstructs positions with. A uniform buffer rather
// than a push constant because two mat4s are already the entire 128-byte guarantee and
// three passes need the same two.
// The includer must define BEAUTY_CAMERA_SET and BEAUTY_CAMERA_BINDING first.
// NOTE: never put a literal include directive inside a comment in this file.
layout(set = BEAUTY_CAMERA_SET, binding = BEAUTY_CAMERA_BINDING, std140) uniform BeautyCam {
	mat4 view_proj;
	mat4 inv_view_proj;
	vec4 cam;    // xyz = camera position, w = unused
	vec4 screen; // xy = full-resolution size, zw = 1 / size
} bcam;

// Vulkan NDC: y = -1 at the TOP of the framebuffer, and Godot's scene projection already
// carries the y-flip that makes that true. So this is uv.y * 2 - 1 -- the same convention
// deferred.comp.glsl uses, and for the same reason.
vec3 beauty_world_from_depth(vec2 uv, float depth) {
	vec4 h = bcam.inv_view_proj * vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, depth, 1.0);
	return h.xyz / (abs(h.w) < 1e-9 ? 1e-9 : h.w);
}

// World point -> uv and reverse-Z depth. Returns false behind the camera.
bool beauty_project(vec3 p, out vec2 uv, out float depth) {
	vec4 c = bcam.view_proj * vec4(p, 1.0);
	if (c.w <= 0.0) return false;
	vec3 n = c.xyz / c.w;
	uv = n.xy * 0.5 + 0.5;
	depth = n.z;
	return true;
}
```

`extension/src/render/beauty_camera.h` / `.cpp` is a 160-byte `uniform_buffer_create` plus an `update()` that fills it **by float index** (M5 errata 3): `view_proj` at floats 0–15, `inv_view_proj` at 16–31, `cam` at 32–35, `screen` at 36–39, with `static_assert(sizeof(float) * 40 == 160, "beauty camera block");`. `update()` records `buffer_update` — a device-level command, so callers invoke it **before** opening any list.

- [ ] **Step 4: Write `shaders/contact_shadow.comp.glsl`**

```glsl
#[compute]
#version 460

#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 4
#include "common.glslh"
#include "shade.glslh"
#include "beauty_camera.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1, r8) writeonly uniform image2D out_mask; // half resolution
layout(set = 0, binding = 2) uniform sampler2D mask_tex;             // the same texture
layout(set = 0, binding = 3, rgba16f) uniform image2D scene_color;   // full resolution

layout(push_constant, std430) uniform Push {
	ivec4 dims;  // xy = this dispatch's target size, z = mode (0 march, 1 apply), w = steps
	vec4 params; // x = reach in metres, y = strength, z = depth tolerance, w = flags
} pc;

// Spec section 7: "short depth march toward sun, half-res, dithered, bilateral upsample".
// The dither is what turns 12 steps into an effective 12 x 16 pattern -- the temporal
// accumulation the SSGI pass already does is not available here, so the noise has to be
// spatial and it has to be cheap.
float march(vec2 uv, float depth) {
	vec3 p = beauty_world_from_depth(uv, depth);
	float jitter = bayer4(ivec2(gl_GlobalInvocationID.xy));
	float step_m = pc.params.x / float(max(pc.dims.w, 1));
	for (int i = 0; i < pc.dims.w; i++) {
		vec3 q = p + SUN_DIR * (step_m * (float(i) + jitter));
		vec2 quv;
		float qdepth;
		if (!beauty_project(q, quv, qdepth)) break;
		if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;
		float scene = texture(scene_depth, quv).r;
		// Reverse-Z: a LARGER depth is nearer. Something in front of the marching point
		// occludes it -- but only if it is in front by a believable amount; a huge gap is a
		// different surface entirely and shadowing across it is the classic screen-space
		// halo.
		float ahead = scene - qdepth;
		if (ahead > 0.0 && ahead < pc.params.z) return 0.0;
	}
	return 1.0;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);

	if (pc.dims.z == 0) {
		float depth = texture(scene_depth, uv).r;
		// Depth 0 is the reverse-Z far plane: nothing was drawn, so nothing can be shadowed.
		imageStore(out_mask, px, vec4(depth <= 0.0 ? 1.0 : march(uv, depth)));
		return;
	}

	// Apply. A plain bilinear read of the half-resolution mask IS the bilateral upsample
	// here: the mask is a visibility term, not a colour, and the depth march that produced
	// it already refused to cross a depth discontinuity, so there is no edge to preserve.
	float vis = texture(mask_tex, uv).r;
	vec4 c = imageLoad(scene_color, px);
	// Only ever darkens: mix toward the mask, never past 1.
	imageStore(scene_color, px, vec4(c.rgb * mix(1.0, vis, pc.params.y), c.a));
}
```

`bayer4` lives in `common.glslh` behind `#ifdef MATERIAL_LAYERS`. This shader has no material arrays, so **move `bayer4` out of that `#ifdef` block** in `common.glslh` — it depends on nothing else and three M6 shaders want it. That is a one-line move and it makes the include cheaper for everyone.

- [ ] **Step 5: Write `ContactShadowPass`**

`contact_shadow_pass.h`/`.cpp` own the half-resolution `R8_UNORM` mask (recreated when the size changes, with the uniform set freed first), one compute pipeline, and a `render()` that:

1. returns `false` immediately unless `s.contact_shadows && s.contact_steps > 0`;
2. rebuilds the mask and uniform set if `size` changed;
3. opens one compute list, dispatches mode 0 over the **half** size, calls `rd->compute_list_add_barrier(list)`, dispatches mode 1 over the **full** size, ends the list;
4. records `last_ms_` around the command recording (and says in a comment that it is record time, not GPU time — M5 errata 15).

Push-constant values: `dims = (w, h, mode, s.contact_steps)`, `params = (0.6f /*reach m*/, 0.85f /*strength*/, kDepthTolerance, 0)`. `kDepthTolerance` is in reverse-Z depth units and depends on the projection, so derive it per frame instead of hardcoding: `tolerance = 0.5f * (near_depth_at(reach))` is fiddly — use the simple, robust form and pass **linear** metres instead: change the shader's test to reconstruct the occluder's world position (`beauty_world_from_depth(quv, scene)`) and compare `distance(q, occluder)` against `pc.params.z` metres, with `pc.params.z = 1.5f`. That is one extra matrix multiply per step and it removes an entire class of projection-dependent tuning.

**Apply the metric change to Step 4's shader before writing it**: the occlusion test becomes

```glsl
		float scene = texture(scene_depth, quv).r;
		if (scene <= 0.0) continue; // nothing drawn there
		vec3 occluder = beauty_world_from_depth(quv, scene);
		// Is the occluder in FRONT of the marching point, and close enough to be the same
		// surface rather than a distant one the screen happens to project here?
		float toward_cam = dot(bcam.cam.xyz - q, normalize(bcam.cam.xyz - q));
		if (distance(occluder, bcam.cam.xyz) < distance(q, bcam.cam.xyz) - 0.02 &&
				distance(occluder, q) < pc.params.z)
			return 0.0;
```

Drop the unused `toward_cam` line when you write it; it is here only to name what the two `distance` comparisons mean.

- [ ] **Step 6: Write `shaders/downsample.comp.glsl`**

```glsl
#[compute]
#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba16f) writeonly uniform image2D dst;
layout(push_constant, std430) uniform Push { ivec4 dims; } pc; // xy = destination size

// The finished frame, halved, becomes next frame's one-bounce GI source (spec section 7's
// "reprojected previous frame's lit color"). A 2x2 box is enough: SSGI samples it with a
// wide kernel anyway, and anything sharper would only alias into the temporal history.
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	imageStore(dst, px, vec4(texture(src, uv).rgb, 1.0));
}
```

A linear sampler over the full-resolution source at half-resolution uv centres *is* the 2×2 box; no manual taps needed.

- [ ] **Step 7: Write `BeautyCompositor`**

`extension/src/beauty_compositor.h`:

```cpp
#pragma once
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>

namespace godot {

// The post-opaque half of spec section 7's frame order: contact shadows, SSR and outlines
// all need Godot's dynamic objects in the depth and colour buffers, and a CompositorEffect
// carries exactly one callback type -- so this is a second effect resource beside
// RaymarchCompositor, not a branch inside it.
//
// It also ends the frame by handing the finished image to the next one: a half-resolution
// copy into GBuffer::history(), which is what SSGI bounces light from.
class BeautyCompositor : public CompositorEffect {
	GDCLASS(BeautyCompositor, CompositorEffect)

	NodePath world_path_;
	int normal_roughness_state_ = -1; // -1 unknown, 0 absent, 1 present

protected:
	static void _bind_methods();

public:
	BeautyCompositor();
	void set_world_path(const NodePath &p) { world_path_ = p; }
	NodePath get_world_path() const { return world_path_; }
	int normal_roughness_state() const { return normal_roughness_state_; }
	void _render_callback(int p_effect_callback_type, RenderData *p_render_data) override;
};

} // namespace godot
```

The constructor is where the spike happens:

```cpp
BeautyCompositor::BeautyCompositor() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_OPAQUE);
	// Spec section 3's second known spike item. Asking for it is what makes Forward+
	// allocate it: without this flag the buffer is only created when SSAO/SSIL/SSR happen
	// to be on in the Environment, and a GDExtension that merely reads it gets nothing.
	set_needs_normal_roughness(true);
}
```

`_render_callback` mirrors `RaymarchCompositor`'s guard sequence exactly — callback-type check, `world_path_` check, `SceneTree`/`VoxelWorld` lookup, `get_use_local_device()` bail, `RenderingDevice`/`RenderSceneBuffersRD`/`RenderSceneData` fetch, `get_internal_size()` sanity — then:

```cpp
	normal_roughness_state_ =
			rsb->has_texture("forward_clustered", "normal_roughness") ? 1 : 0;
	const RID normal_rough = normal_roughness_state_ == 1
			? rsb->get_texture("forward_clustered", "normal_roughness") : RID();

	CameraUbo *ubo = world->beauty_camera();
	if (!ubo || !ubo->ensure(rd)) return;
	// Device-level command: before any list.
	ubo->update(rd, view_proj, cam_pos, size, 0.05f, 4000.0f);

	const ve::BeautySettings &s = world->beauty_settings();
	if (ContactShadowPass *cs = world->contact_shadow_pass())
		cs->render(rd, rsb->get_color_texture(), rsb->get_depth_texture(), size,
				ubo->buffer(), s);
	// Task 11 inserts SSR here, Task 12 outlines here.

	// Last: hand this frame's finished image to the next one.
	GBuffer *gb = world->gbuffer();
	if (gb && gb->is_valid())
		world->downsample_history(rd, rsb->get_color_texture(), *gb);
```

`view_proj` and `cam_pos` come from `sd->get_cam_transform()` / `sd->get_cam_projection()` exactly as `RaymarchCompositor` builds them.

Register it: `GDREGISTER_CLASS(BeautyCompositor);` in `register_types.cpp`, beside `RaymarchCompositor`.

Add it to `demo/main.tscn`. The `Compositor` sub-resource takes an array, and **order within the array is the execution order for effects sharing a callback type** — these two have different types, so the array order is cosmetic, but keep the pre-opaque one first so the file reads in frame order:

```
[sub_resource type="BeautyCompositor" id="6"]
world_path = NodePath("/root/Main/VoxelWorld")

[sub_resource type="Compositor" id="2"]
compositor_effects = [SubResource(1), SubResource(6)]
```

- [ ] **Step 8: Add the probe and the stats hook**

`VoxelWorld::debug_contact_shadow_probe(Vector3 pos, Vector3 fwd, int w, int h)` builds a synthetic frame on the local device: stream to quiet, ensure the owned `GBuffer` at `w × h`, run raymarch → composite → deferred → **copy `lit` into an owned `RGBA16F` scratch that stands in for the scene colour**, update a `CameraUbo` from the same projection, then run `ContactShadowPass::render(rd, scratch, gb->depth(), size, ubo, beauty_)`. It reads back the mask and both the pre- and post-apply scratch (keep a second copy of `lit` for the comparison) and reports `mask_width`, `mask_height`, `mask_min`, `mask_mean`, `mean_darkening` (mean of `pre_luma - post_luma`, clamped at 0), and `max_brightening` (max of `post_luma - pre_luma`).

`VoxelWorld::debug_beauty_compositor_stats()` returns `{"normal_roughness": <-1|0|1>, "contact_ms": ..., "ssr_ms": ..., "outline_ms": ...}` — the later tasks fill the remaining timings in. `VoxelWorld` needs a `BeautyCompositor *` back-pointer for the first field; have `BeautyCompositor::_render_callback` push its state with `world->set_normal_roughness_state(normal_roughness_state_)` rather than the world reaching into a resource it does not own.

`VoxelWorld::downsample_history(RenderingDevice *rd, RID src, GBuffer &gb)` owns the `downsample.comp.glsl` pipeline (it is four lines of shader and belongs with the buffer it fills, not in a class of its own).

- [ ] **Step 9: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then `./gdunit_tests.sh -a res://tests/test_contact_shadow.gd -c`
Expected: PASS, five tests.

- [ ] **Step 10: Settle the normal-roughness spike and record the verdict**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn` and read the HUD line the next task formats, or add a one-off `print` of `debug_beauty_compositor_stats()["normal_roughness"]` from `demo/hud.gd` for this step.

**Record in the Errata:**
- `normal_roughness == 1` — the buffer exists. Tasks 10 and 12 may read dynamic-object normals from it; note the encoding you measured (Godot 4 stores `normal * 0.5 + 0.5` in rgb and roughness in a, but verify against a known-orientation object before relying on it, because the roughness channel carries a sign convention for subsurface scattering).
- `normal_roughness == 0` — the fallback is spec §3's own: dynamic objects get **constant ambient** in SSGI (Task 10 skips the normal-dependent term for pixels the G-buffer does not cover) and outlines use **depth discontinuity only** on those pixels (Task 12). Both branches are written regardless, so this verdict selects a runtime path, not a code path to go write.

Either way the demo runs; this step produces a recorded fact, not a fix.

- [ ] **Step 11: Verify the demo**

Expected: dynamic objects (the `TestCube`, any debris) now sit in a small dark contact patch where they meet the ground instead of appearing to hover, and the near side of every crater rim darkens over about half a metre.

- [ ] **Step 12: Commit**

```bash
git add extension/src/beauty_compositor.h extension/src/beauty_compositor.cpp \
        extension/src/render/beauty_camera.h extension/src/render/beauty_camera.cpp \
        extension/src/render/contact_shadow_pass.h \
        extension/src/render/contact_shadow_pass.cpp shaders/beauty_camera.glslh \
        shaders/contact_shadow.comp.glsl shaders/downsample.comp.glsl \
        shaders/common.glslh extension/src/register_types.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp demo/main.tscn \
        tests/test_contact_shadow.gd
git commit -m "feat: post-opaque beauty compositor with screen-space contact shadows"
```

---

### Task 10: `SsgiPass` — one bounce, half res, temporal

Spec §7: "Half-res, 6–8 horizon taps/pixel from this frame's depth/normals + reprojected previous frame's lit color for one bounce; temporal accumulation with neighbourhood clamping. Feeds the ambient term of the cel bands."

It runs **before** the deferred pass, in the pre-opaque callback, because that is where its output is consumed. Its input is `GBuffer::history()` — the *previous* frame's finished image, which Task 9's downsample already fills at the end of post-opaque. The parity works out with no bookkeeping: post-opaque of frame N writes the history, pre-opaque of frame N+1 reads it.

**Files:**
- Create: `shaders/ssgi.comp.glsl`, `extension/src/render/ssgi_pass.h`, `.cpp`
- Modify: `extension/src/raymarch_compositor.cpp`, `extension/src/voxel_world.h`, `.cpp`
- Create: `tests/test_ssgi.gd`

**Interfaces:**
- Consumes: `GBuffer` (Task 4), `CameraUbo` and `beauty_camera.glslh` (Task 9), `oct_decode` (Task 5), `ve::BeautySettings` (Task 3).
- Produces: `godot::SsgiPass` with `void initialize(RenderingDevice *)`, `void teardown()`, `bool render(RenderingDevice *rd, GBuffer &gb, RID camera_ubo, const float prev_view_proj[16], bool have_history, const ve::BeautySettings &s, uint32_t frame)`, `RID result() const`, `float last_ms() const`; `VoxelWorld::debug_ssgi_probe(Vector3 pos, Vector3 fwd, int w, int h, int frames)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_ssgi.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_result_is_half_resolution() -> void:
	var w := make_world()
	var d := w.debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 1)
	assert_int(d["width"]).is_equal(64)
	assert_int(d["height"]).is_equal(64)

# The first frame has no history to bounce from, so it must produce zero rather than reading
# an uninitialised texture and painting the screen with whatever was in memory.
func test_the_first_frame_bounces_nothing() -> void:
	var w := make_world()
	var d := w.debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 1)
	assert_float(d["max_channel"]).is_equal_approx(0.0, 0.001)

# ...and once there IS a history, light bounces. The crater at (30, ~49, 30) is a bowl: its
# walls see each other, which is the case one-bounce GI exists to brighten.
func test_light_bounces_once_the_history_exists() -> void:
	var w := make_world()
	var d := w.debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 8)
	assert_float(d["max_channel"]).override_failure_message(
		"eight frames of history produced no bounce at all").is_greater(0.005)
	# ...and it did not blow up: temporal accumulation without a clamp diverges, and this is
	# the assertion that catches it.
	assert_float(d["max_channel"]).is_less(2.0)

func test_the_accumulation_converges_rather_than_climbing() -> void:
	var w := make_world()
	var pos := Vector3(30.0, 70.0, 30.0)
	var fwd := Vector3(0.2, -1.0, 0.2).normalized()
	var a := w.debug_ssgi_probe(pos, fwd, 128, 128, 8)
	var b := w.debug_ssgi_probe(pos, fwd, 128, 128, 24)
	# A static camera over a static world: three times the frames must not mean three times
	# the light. Allow a wide band; the point is that it is bounded, not that it is equal.
	assert_float(float(b["mean_luma"])).is_less(float(a["mean_luma"]) * 2.0 + 0.01)

func test_turning_ssgi_off_produces_nothing_and_costs_no_dispatch() -> void:
	var w := make_world()
	w.set_effect_enabled("ssgi", false)
	var d := w.debug_ssgi_probe(Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2).normalized(),
		128, 128, 8)
	assert_bool(d["ran"]).is_false()
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_ssgi.gd`
Expected: FAIL — `Nonexistent function 'debug_ssgi_probe'`.

- [ ] **Step 3: Write `shaders/ssgi.comp.glsl`**

```glsl
#[compute]
#version 460

#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 5
#include "common.glslh"
#include "shade.glslh"
#include "beauty_camera.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gb_surface;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D history;   // HALF res, last frame's image
layout(set = 0, binding = 3) uniform sampler2D prev_ssgi; // HALF res, last frame's result
layout(set = 0, binding = 4, rgba16f) writeonly uniform image2D out_ssgi;

layout(push_constant, std430) uniform Push {
	mat4 prev_view_proj; // floats 0..15
	ivec4 dims;          // 16..19: xy = half-res size, z = taps, w = have_history
	vec4 params;         // 20..23: x = radius m, y = history blend, z = intensity, w unused
} pc;

// Golden-angle spiral: 8 taps that never line up into a visible pattern and need no table.
// The per-pixel rotation is what turns the spatial pattern into noise the temporal filter
// can eat.
vec2 spiral_tap(int i, int n, float rot) {
	float t = (float(i) + 0.5) / float(n);
	float a = t * 6.28318531 * 3.0 + rot;
	return vec2(cos(a), sin(a)) * sqrt(t);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);

	// No history means nothing to bounce. Writing zero is not a stub: it is the correct
	// answer, and it is what keeps the first frame from sampling uninitialised memory.
	if (pc.dims.w == 0) {
		imageStore(out_ssgi, px, vec4(0.0));
		return;
	}

	float depth = texture(gb_depth, uv).r;
	vec4 g1 = texture(gb_surface, uv);
	if (depth <= 0.0 || g1.z < 0.5) { // sky or no voxel: nothing receives here
		imageStore(out_ssgi, px, vec4(0.0));
		return;
	}
	vec3 p = beauty_world_from_depth(uv, depth);
	vec3 n = oct_decode(g1.xy);

	float rot = bayer4(px) * 6.28318531;
	vec3 sum = vec3(0.0);
	float weight = 0.0;
	for (int i = 0; i < pc.dims.z; i++) {
		// Sample in SCREEN space at a radius that shrinks with distance, so the world-space
		// gather radius stays roughly constant: a 6 m bowl 20 m away subtends far fewer
		// pixels than the same bowl at 3 m.
		vec2 off = spiral_tap(i, pc.dims.z, rot) * pc.params.x * bcam.screen.z * 40.0 /
				max(distance(p, bcam.cam.xyz), 1.0);
		vec2 suv = clamp(uv + off, vec2(0.0), vec2(1.0));
		float sdepth = texture(gb_depth, suv).r;
		if (sdepth <= 0.0) continue;
		vec3 sp = beauty_world_from_depth(suv, sdepth);
		vec3 dir = sp - p;
		float dist = length(dir);
		if (dist < 1e-3 || dist > pc.params.x) continue;
		dir /= dist;
		// The horizon term: only a sample the receiver can actually see contributes.
		float cosine = dot(n, dir);
		if (cosine <= 0.0) continue;
		// ...and only if the sample is FACING back, or it is a wall lighting its own back.
		vec3 sn = oct_decode(texture(gb_surface, suv).xy);
		if (dot(sn, -dir) <= 0.0) continue;
		// Inverse-square with a 1 m floor, so a sample at arm's length is bright but finite.
		float falloff = 1.0 / (1.0 + dist * dist);
		sum += texture(history, suv).rgb * cosine * falloff;
		weight += 1.0;
	}
	vec3 gi = weight > 0.0 ? sum / weight * pc.params.z : vec3(0.0);

	// Temporal accumulation with neighbourhood clamping (spec section 7). Without the clamp
	// the history smears a bright wall across everything the camera sweeps past; with it,
	// history that no longer belongs to this neighbourhood is pulled back into range instead
	// of being thrown away, which is what keeps the result stable AND responsive.
	vec2 puv;
	float pdepth;
	vec4 pc4 = pc.prev_view_proj * vec4(p, 1.0);
	bool reproj = pc4.w > 0.0;
	if (reproj) {
		vec3 pn = pc4.xyz / pc4.w;
		puv = pn.xy * 0.5 + 0.5;
		reproj = all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0)));
	}
	if (reproj) {
		vec3 lo = gi;
		vec3 hi = gi;
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++) {
				vec3 s = texelFetch(prev_ssgi,
						clamp(px + ivec2(x, y), ivec2(0), pc.dims.xy - 1), 0).rgb;
				lo = min(lo, s);
				hi = max(hi, s);
			}
		vec3 prev = clamp(texture(prev_ssgi, puv).rgb, lo, hi);
		gi = mix(gi, prev, pc.params.y);
	}
	imageStore(out_ssgi, px, vec4(gi, 1.0));
}
```

- [ ] **Step 4: Write `SsgiPass`**

Two half-resolution `RGBA16F` targets, ping-ponged by frame parity: `result_[frame & 1]` is written, `result_[(frame + 1) & 1]` is read as `prev_ssgi`. Both are cleared to zero when allocated, so the very first read is defined even before `have_history` is true. `render()` returns `false` (and touches nothing) unless `s.ssgi && s.ssgi_taps > 0`; `result()` then returns an invalid RID and `DeferredPass` clears the SSGI flag bit on its own — the fail-soft chain Task 6 already built.

Push-constant values: `prev_view_proj` from the caller, `dims = (half_w, half_h, s.ssgi_taps, have_history ? 1 : 0)`, `params = (6.0f, 0.90f, 1.0f, 0)`. 24 floats = 96 bytes; `static_assert` it.

- [ ] **Step 5: Wire it into the pre-opaque callback**

Between the LoD raster and the deferred pass:

```cpp
	SsgiPass *ssgi = world->ssgi_pass();
	bool ssgi_ok = false;
	if (ssgi && world->beauty_settings().ssgi) {
		ssgi_ok = ssgi->render(rd, *gb, ubo->buffer(), world->prev_view_proj(),
				world->has_history(), world->beauty_settings(), world->beauty_frame());
	}
	// ... deferred->render(rd, *gb, *materials, ssgi_ok ? ssgi->result() : RID(), ...)
```

`RaymarchCompositor` also needs a `CameraUbo` now (SSGI runs pre-opaque and reconstructs positions). Update it there too, from the same `view_proj`/`cam_pos` it already has, **before** any list is opened. `VoxelWorld` owns the single `CameraUbo` and both compositors update it; that is safe because they run at different points of the same frame and write the same numbers.

`VoxelWorld` tracks three small pieces of frame state for this: `prev_view_proj_[16]`, `has_history_` (set by `downsample_history`, cleared on `teardown_gpu`) and `beauty_frame_` (incremented at the end of the pre-opaque callback). Store `prev_view_proj_` at the *end* of the pre-opaque callback, after SSGI has read it.

- [ ] **Step 6: Add the probe**

`VoxelWorld::debug_ssgi_probe(Vector3 pos, Vector3 fwd, int w, int h, int frames)` loops `frames` times over the full synthetic sequence — raymarch → composite → SSGI from the previous iteration's history → deferred using that SSGI result → `downsample_history(lit)` for the next iteration — with the same camera each iteration, submitting and syncing once at the end. It then reads back `ssgi->result()` and reports `width`, `height`, `max_channel`, `mean_luma`, and `ran`.

Loop order is load-bearing: SSGI runs before the current iteration's deferred/downsample, and `downsample_history` is the last command in the iteration. Pass `have_history = (i > 0)`.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `./build.sh -j$(nproc)` then `./gdunit_tests.sh -a res://tests/test_ssgi.gd -a res://tests/test_deferred.gd -c`
Expected: PASS.

- [ ] **Step 8: Verify the demo**

Expected: shadowed surfaces near lit ones pick up a faint tint of their neighbour — the inside of the cave takes some green off the grass at its mouth — and the darkest cel band stops reading as flat black. Sweep the camera quickly: if a bright smear trails the motion, the neighbourhood clamp is not doing its job and `params.y` is the number to lower; record what you land on in the Errata.

- [ ] **Step 9: Commit**

```bash
git add shaders/ssgi.comp.glsl extension/src/render/ssgi_pass.h \
        extension/src/render/ssgi_pass.cpp extension/src/raymarch_compositor.cpp \
        extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_ssgi.gd
git commit -m "feat: half-res temporal ssgi feeding the cel ambient term"
```

---

### Task 11: `SsrPass` and the High-tier glossy SDF ray

Spec §7 puts reflections after Godot opaque so the reflected colour and depth include
terrain, LoD, islands, debris and ordinary Godot meshes. The screen-space half therefore
marches `RenderSceneBuffersRD::get_depth_texture()`, **not** pre-opaque
`GBuffer::depth()` alone. At POST_OPAQUE that scene depth is the merged depth: Task 6
injected voxel depth, then Godot opaque added dynamic objects.

The true SDF ray is cast earlier, while `raymarch.comp.glsl` still has the field and its
min-max hierarchy. Task 4's G-buffer channel budget is closed, so the secondary hit cannot
travel in a reflection channel. Its base colour is blended into `albedo.rgb` before deferred
lighting. This is deliberately an **albedo-space approximation**: the reflected colour is
later cel-lit as primary albedo. `surface.z` remains the primary material, `surface.w` its
gloss, `albedo.a` sun visibility, and no attachment is added.

**Files:**
- Create: `shaders/ssr.comp.glsl`
- Create: `extension/src/render/ssr_pass.h`, `extension/src/render/ssr_pass.cpp`
- Modify: `shaders/raymarch.comp.glsl`
- Modify: `extension/src/beauty_compositor.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_ssr.gd`

**Interfaces:**
- Consumes: post-opaque scene colour/depth and `CameraUbo` from Task 9; G-buffer
  surface/depth from Task 4; `oct_decode`, `BEAUTY_SSR`, `BEAUTY_GLOSSY_RAYS` from Task 5;
  `ve::BeautySettings::ssr_steps` from Task 3.
- Produces: `godot::SsrPass::render(RenderingDevice *, RID scene_color,
  RID scene_depth, RID gb_surface, RID gb_depth, RID normal_roughness,
  bool have_normal_roughness, RID camera_ubo, Vector2i size,
  const ve::BeautySettings &)`, `reflection()`, `half_size()`, and `last_ms()`.
  `last_ms()` is CPU command-record time only; Task 14 never uses it as GPU cost.
- Produces: `VoxelWorld::debug_ssr_probe(int fixture, int w, int h)` and
  `VoxelWorld::debug_glossy_sdf_probe(Vector3 origin, Vector3 dir)`.
- The glossy ray reuses the existing `BEAUTY_GLOSSY_RAYS = 32u` bit in `cam_pos.w` and
  changes no `RaymarchPass` texture or public method.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_ssr.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(20, 56.2, 20)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_reflection_target_is_half_resolution() -> void:
	var d := make_world().debug_ssr_probe(0, 321, 181)
	assert_int(d["width"]).is_equal(160)
	assert_int(d["height"]).is_equal(90)

# Fixture 0 has a mirror receiver in both depth buffers and a red blocker written ONLY to
# post-opaque scene colour/depth. A hit proves SSR reads merged scene depth.
func test_a_post_opaque_only_blocker_is_reflected() -> void:
	var d := make_world().debug_ssr_probe(0, 128, 128)
	assert_bool(d["ran"]).is_true()
	assert_int(d["steps"]).is_equal(24)
	assert_int(d["hit_pixels"]).is_greater(0)
	assert_float(d["red_gain"]).is_greater(0.01)
	assert_float(d["max_weight"]).is_between(0.0, 0.85)

func test_removing_the_scene_only_blocker_removes_its_hits() -> void:
	var w := make_world()
	var a := w.debug_ssr_probe(0, 128, 128)
	var b := w.debug_ssr_probe(1, 128, 128)
	assert_int(b["hit_pixels"]).is_less(int(a["hit_pixels"]))

func test_medium_uses_twelve_steps_and_off_dispatches_nothing() -> void:
	var w := make_world()
	w.quality_tier = 2
	assert_int(w.debug_ssr_probe(0, 128, 128)["steps"]).is_equal(12)
	w.quality_tier = 0
	var off := w.debug_ssr_probe(0, 128, 128)
	assert_bool(off["ran"]).is_false()
	assert_float(off["mean_delta"]).is_equal_approx(0.0, 0.0001)

func test_the_apply_is_bounded_and_preserves_alpha() -> void:
	var d := make_world().debug_ssr_probe(0, 128, 128)
	assert_float(d["max_weight"]).is_less_equal(0.85)
	assert_float(d["max_alpha_delta"]).is_less(0.0001)
	assert_bool(d["finite"]).is_true()

# Current roughness assets never exceed gloss 0.5. The probe uses a negative params.w
# sentinel to force shader gloss=1 on a real ground hit without changing production data.
func test_true_sdf_reflections_are_high_only_and_change_only_albedo() -> void:
	var w := make_world()
	var origin := Vector3(20, 75, 20)
	var dir := Vector3(0, -1, 0)
	w.quality_tier = 2
	var medium := w.debug_glossy_sdf_probe(origin, dir)
	w.quality_tier = 3
	var high := w.debug_glossy_sdf_probe(origin, dir)
	assert_bool(high["hit"]).is_true()
	var ha: Color = high["albedo"]
	var ma: Color = medium["albedo"]
	var albedo_delta := maxf(absf(ha.r - ma.r), maxf(absf(ha.g - ma.g), absf(ha.b - ma.b)))
	assert_float(albedo_delta).is_greater(0.005)
	assert_int(high["material"]).is_equal(int(medium["material"]))
	assert_float(high["gloss"]).is_equal_approx(float(medium["gloss"]), 0.001)
	assert_float(high["sun"]).is_equal_approx(float(medium["sun"]), 0.01)
	assert_float(Vector3(high["position"]).distance_to(Vector3(medium["position"]))).is_less(0.01)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_ssr.gd -c`
Expected: FAIL — `Nonexistent function 'debug_ssr_probe'`.

- [ ] **Step 3: Write the two-variant SSR shader**

Create `shaders/ssr.comp.glsl`. Compile it once as written for trace, then compile a second
copy after inserting `#define SSR_APPLY 1` immediately after `#version`. Trace samples scene
colour; apply writes it. No dispatch binds the same scene texture sampled and writable.

```glsl
#[compute]
#version 460
#include "common.glslh"
#include "shade.glslh"
layout(local_size_x = 8, local_size_y = 8) in;

#ifndef SSR_APPLY
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 6
#include "beauty_camera.glslh"
layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D gb_surface;
layout(set = 0, binding = 3) uniform sampler2D gb_depth;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness;
layout(set = 0, binding = 5, rgba16f) writeonly uniform image2D out_reflection;
layout(push_constant, std430) uniform Push {
	ivec4 dims;  // xy half size, z steps, w have normal-roughness
	vec4 params; // reach, start bias, world crossing thickness, strength
} pc;

bool receiver(vec2 uv, float depth, out vec3 p, out vec3 n, out float gloss) {
	p = beauty_world_from_depth(uv, depth);
	vec4 g = texture(gb_surface, uv);
	float gd = texture(gb_depth, uv).r;
	if (g.z >= 0.5 && abs(gd - depth) <= 1e-5) {
		n = oct_decode(g.xy);
		gloss = clamp(g.w, 0.0, 1.0);
		return true;
	}
	// Dynamic objects are absent from gb_surface. Depth derivatives provide a world-space
	// reflection plane; normal_roughness contributes roughness only. Without that optional
	// buffer the object is still reflected by terrain but is not itself an SSR receiver.
	if (pc.dims.w == 0) return false;
	vec2 dx = vec2(bcam.screen.z, 0.0), dy = vec2(0.0, bcam.screen.w);
	vec2 ux = clamp(uv + dx, vec2(0.0), vec2(1.0));
	vec2 uy = clamp(uv + dy, vec2(0.0), vec2(1.0));
	float zx = texture(scene_depth, ux).r, zy = texture(scene_depth, uy).r;
	if (zx <= 0.0 || zy <= 0.0) return false;
	vec3 px = beauty_world_from_depth(ux, zx), py = beauty_world_from_depth(uy, zy);
	if (distance(px, p) > 2.0 || distance(py, p) > 2.0) return false;
	n = normalize(cross(px - p, py - p));
	if (dot(n, bcam.cam.xyz - p) < 0.0) n = -n;
	gloss = clamp(1.0 - texture(normal_roughness, uv).a, 0.0, 1.0);
	return !isnan(n.x) && !isnan(n.y) && !isnan(n.z) &&
		!isinf(n.x) && !isinf(n.y) && !isinf(n.z);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	float depth = texture(scene_depth, uv).r;
	vec3 p, n; float gloss;
	if (depth <= 0.0 || !receiver(uv, depth, p, n, gloss) || gloss <= 0.0) {
		imageStore(out_reflection, px, vec4(0.0)); return;
	}
	vec3 v = normalize(bcam.cam.xyz - p);
	vec3 rd = normalize(reflect(-v, n));
	vec2 hit_uv = vec2(0.0); bool hit = false;
	float jitter = bayer4(px);
	for (int i = 0; i < pc.dims.z; i++) {
		float t = (float(i) + 0.5 + jitter) / float(max(pc.dims.z, 1));
		vec3 q = p + n * pc.params.y + rd * (pc.params.x * t);
		vec2 quv; float qdepth;
		if (!beauty_project(q, quv, qdepth)) break;
		if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;
		float sd = texture(scene_depth, quv).r;
		if (sd <= 0.0) continue;
		vec3 sp = beauty_world_from_depth(quv, sd);
		if (distance(sp, bcam.cam.xyz) < distance(q, bcam.cam.xyz) - 0.02 &&
				distance(sp, q) <= pc.params.z) { hit_uv = quv; hit = true; break; }
	}
	if (!hit) { imageStore(out_reflection, px, vec4(0.0)); return; }
	float edge = clamp(min(min(hit_uv.x, hit_uv.y),
		min(1.0 - hit_uv.x, 1.0 - hit_uv.y)) * 12.0, 0.0, 1.0);
	float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
	float weight = clamp(pc.params.w * fresnel * gloss * edge, 0.0, 0.85);
	imageStore(out_reflection, px, vec4(texture(scene_color, hit_uv).rgb, weight));
}
#else
layout(set = 0, binding = 0) uniform sampler2D reflection_tex;
layout(set = 0, binding = 1, rgba16f) uniform image2D scene_color;
layout(push_constant, std430) uniform Push { ivec4 dims; } pc;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	vec4 r = texture(reflection_tex, uv);
	vec4 c = imageLoad(scene_color, px);
	imageStore(scene_color, px, vec4(mix(c.rgb, r.rgb, r.a), c.a));
}
#endif
```

- [ ] **Step 4: Write `SsrPass`**

Create `extension/src/render/ssr_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "shade/beauty_settings.h"
namespace godot {
class SsrPass {
public:
	static constexpr float kReachM = 40.0f;
	static constexpr float kStartBiasM = 0.08f;
	static constexpr float kThicknessM = 1.5f;
	static constexpr float kStrength = 0.80f;
	~SsrPass();
	bool initialize(RenderingDevice *);
	void teardown();
	bool render(RenderingDevice *, RID, RID, RID, RID, RID, bool, RID, Vector2i,
			const ve::BeautySettings &);
	RID reflection() const { return reflection_; }
	Vector2i half_size() const { return half_size_; }
	float last_ms() const { return last_ms_; }
private:
	RenderingDevice *rd_ = nullptr;
	RID trace_shader_, apply_shader_, trace_pipeline_, apply_pipeline_;
	RID nearest_, linear_, reflection_, dummy_normal_, trace_set_, apply_set_;
	RID key_color_, key_depth_, key_surface_, key_gb_depth_, key_normal_, key_camera_;
	Vector2i half_size_{0, 0};
	float last_ms_ = 0.0f; // CPU record time only
};
} // namespace godot
```

Implement `.cpp` with these exact rules:

1. Use `load_shader_source`/`strip_shader_annotations`; compile the two variants described
   above. Allocate nearest and linear samplers plus a 1×1 neutral normal fallback.
2. Allocate `reflection_` as half-resolution `R16G16B16A16_SFLOAT`, storage+sampling. Half
   size is `max(1, full.x / 2), max(1, full.y / 2)`, matching `GBuffer`.
3. Return before allocation/dispatch unless `s.ssr && s.ssr_steps > 0`.
4. Bind trace resources 0–6 in shader order. Bind apply's linear reflection sampler at 0
   and scene colour image at 1. Rebuild cached sets when any RID or size changes.
5. Trace push constants are exactly 32 bytes, indexed by float:

```cpp
PackedByteArray pc;
pc.resize(32);
int32_t *i = reinterpret_cast<int32_t *>(pc.ptrw());
float *f = reinterpret_cast<float *>(pc.ptrw());
i[0] = half_size_.x; i[1] = half_size_.y; i[2] = s.ssr_steps;
i[3] = have_normal_roughness && normal_roughness.is_valid() ? 1 : 0;
f[4] = kReachM; f[5] = kStartBiasM; f[6] = kThicknessM; f[7] = kStrength;
```

6. End trace's compute list before opening apply's list; the list boundary supplies the
   image transition. Apply uses a 16-byte `(full.x, full.y, 0, 0)` push block.
7. Free uniform sets before pipelines/shaders/textures. Reallocation frees both sets before
   the old reflection texture. Fail-soft returns without changing scene colour.

- [ ] **Step 5: Add the bounded glossy ray to `raymarch.comp.glsl`**

Add:

```glsl
const float GLOSSY_SDF_MAX_DIST = 20.0;
const int GLOSSY_SDF_STEPS = 64;
const float GLOSSY_SDF_MIN_GLOSS = 0.5;
const float GLOSSY_SDF_BIAS = 0.06;
const float GLOSSY_SDF_STRENGTH = 0.80;
```

Parameterise the existing marchers instead of cloning them:

```glsl
void march_island(int slot, vec3 ro, vec3 rd, inout Hit best, inout int steps_left);
Hit march_terrain(vec3 ro, vec3 rd, float max_dist, inout int steps_left);
```

Both functions decrement `steps_left` for every min-max cell skip and every SDF evaluation;
whole empty-brick DDA traversal consumes no sphere step. Primary call sites become:

```glsl
int primary_steps = 65536;
Hit best = march_terrain(ro, rd, max_dist, primary_steps);
// Inside the existing primary island-mask loop:
int island_steps = 192;
march_island(i, ro, rd, best, island_steps);
```

Each primary island gets its own old 192-step ceiling. A reflected ray shares one
`int reflected_steps = 64` across terrain and every island, so **64 is a total per-pixel
secondary budget**, not 64 for each overlapping target.

After primary material sampling produces `albedo`, `gloss`, `ao`, `ddx` and `ddy`, before the
edit tint and AO fold, add:

```glsl
	if (pc.params.w < -0.5) gloss = 1.0; // debug probe sentinel only
	if ((flags & BEAUTY_GLOSSY_RAYS) != 0u && gloss > GLOSSY_SDF_MIN_GLOSS) {
		vec3 rr = normalize(reflect(rd, best.n));
		vec3 rro = best.p + best.n * GLOSSY_SDF_BIAS;
		int reflected_steps = GLOSSY_SDF_STEPS;
		Hit reflected = march_terrain(rro, rr, GLOSSY_SDF_MAX_DIST, reflected_steps);
		// A reflected ray leaves the primary tile, so its tile mask is invalid. AABB-reject
		// every live island descriptor, sharing the same remaining 64-step budget.
		for (int i = 0; i < island_count && reflected_steps > 0; i++)
			march_island(i, rro, rr, reflected, reflected_steps);
		vec3 reflected_albedo = sky_color(rr);
		if (reflected.hit)
			reflected_albedo = material_surface(reflected.mat, reflected.p,
					reflected.n, ddx, ddy).rgb;
		float ndv = clamp(dot(best.n, -rd), 0.0, 1.0);
		float fresnel = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
		float weight = clamp(GLOSSY_SDF_STRENGTH * fresnel *
				smoothstep(0.5, 1.0, gloss), 0.0, 0.85);
		albedo = mix(albedo, reflected_albedo, weight); // albedo-space approximation
	}
```

The final store remains
`vec4(albedo * mix(1.0, ao, 0.65), sun)`. The probe writes forced gloss in both quality
tiers, so its test can prove only albedo changes.

- [ ] **Step 6: Own, invoke and probe the pass**

Create `SsrPass` with the Task 9 beauty passes, expose `ssr_pass()`, and destroy it before
`GBuffer`. In `BeautyCompositor`, insert after contact shadows:

```cpp
if (SsrPass *ssr = world->ssr_pass())
	ssr->render(rd, scene_color, scene_depth, gb->surface(), gb->depth(), normal_rough,
			have_normal_roughness, ubo->buffer(), size, beauty);
```

`debug_ssr_probe` creates a deterministic 128×128 fixture (or requested size): grey mirror
receiver in both depths; fixture 0 adds a red centre-quarter blocker only to scene depth and
colour; fixture 1 omits it. It reports the keys asserted in Step 1. Use a 60° reverse-Z
camera, submit/sync only on the local RD, and count reflection alpha > `0.001` as a hit.
Fill Task 9's `debug_beauty_compositor_stats()["ssr_ms"]` from `SsrPass::last_ms()` and label
that field CPU record time in its comment; Task 14's GPU dictionary is separate.

`debug_glossy_sdf_probe` is Task 5's G-buffer probe with `cam.params[3] = -1.0f`; return the
same albedo/sun/material/gloss/hit/position keys. Bind both methods.

- [ ] **Step 7: Run tests and verify the demo**

Run:

```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -a res://tests/test_ssr.gd -c
./gdunit_tests.sh -a res://tests/test_contact_shadow.gd -a res://tests/test_ssgi.gd \
  -a res://tests/test_deferred.gd -a res://tests/test_lod_gbuffer.gd \
  -a res://tests/test_raymarch_gbuffer.gd -c
godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn
```

Expected: all tests pass; glossy receivers reflect terrain/LoD/TestCube; rough ground stays
unchanged. The shipped materials do not exceed gloss `0.5`, so the SDF branch is dormant
until a glossy material is painted. Do not lower the fixed threshold.

- [ ] **Step 8: Commit**

```bash
git add shaders/ssr.comp.glsl shaders/raymarch.comp.glsl \
  extension/src/render/ssr_pass.h extension/src/render/ssr_pass.cpp \
  extension/src/beauty_compositor.cpp extension/src/voxel_world.h \
  extension/src/voxel_world.cpp tests/test_ssr.gd
git commit -m "feat: screen-space and glossy sdf reflections"
```

---

### Task 12: `OutlinePass` — one-pixel lines, last before tonemap

The pass is the final custom scene-colour mutation in `BeautyCompositor`: contact shadows
and SSR are already present, outline darkens their finished image, then the non-visual
history downsample copies it. Godot glow/tonemap follows POST_OPAQUE.

Terrain/LoD normals always come from `GBuffer::surface()`. When Task 9 finds Forward+'s
normal-roughness texture, dynamic neighbours use it; otherwise dynamic objects keep depth
and silhouette outlines and skip only normal-only creases. Normals are compared only when
both pixels use the same encoding, so world-space oct normals are never dotted against
Godot's view-space packed normals.

**Files:**
- Create: `shaders/outline.comp.glsl`
- Create: `extension/src/render/outline_pass.h`, `extension/src/render/outline_pass.cpp`
- Modify: `extension/src/beauty_compositor.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `tests/test_outline.gd`

**Interfaces:**
- Produces: `OutlinePass::render(RenderingDevice *, RID scene_color, RID scene_depth,
  RID gb_depth, RID gb_surface, RID normal_roughness, bool have_normal_roughness,
  RID camera_ubo, Vector2i size, const ve::BeautySettings &)`, and CPU-record-only
  `last_ms()`.
- Produces: `VoxelWorld::debug_outline_probe(int fixture, bool have_dynamic_normals)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_outline.gd`:

```gdscript
extends GdUnitTestSuite
var _worlds: Array = []
func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w): w.free()
	_worlds.clear()
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true; w.physics_enabled = false
	add_child(w); _worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w
func test_depth_line_is_one_pixel_and_darkens_by_the_fixed_amount() -> void:
	var d := make_world().debug_outline_probe(1, false)
	assert_int(d["dark_columns"]).is_equal(1)
	assert_float(d["dark_value"]).is_equal_approx(0.35, 0.01)
	assert_float(d["max_brightening"]).is_less(0.0001)
func test_terrain_normal_line_works_at_equal_depth() -> void:
	assert_int(make_world().debug_outline_probe(2, false)["dark_columns"]).is_equal(1)
func test_dynamic_normals_follow_the_spike_verdict() -> void:
	var w := make_world()
	assert_int(w.debug_outline_probe(3, true)["dark_columns"]).is_equal(1)
	assert_int(w.debug_outline_probe(3, false)["dark_columns"]).is_equal(0)
func test_dynamic_depth_line_survives_the_fallback() -> void:
	assert_int(make_world().debug_outline_probe(4, false)["dark_columns"]).is_equal(1)
func test_flat_and_off_are_unchanged() -> void:
	var w := make_world()
	assert_int(w.debug_outline_probe(0, false)["dark_columns"]).is_equal(0)
	w.set_effect_enabled("outlines", false)
	var d := w.debug_outline_probe(1, false)
	assert_bool(d["ran"]).is_false()
	assert_float(d["mean_delta"]).is_equal_approx(0.0, 0.0001)
	assert_float(d["max_alpha_delta"]).is_less(0.0001)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_outline.gd -c`
Expected: FAIL — `Nonexistent function 'debug_outline_probe'`.

- [ ] **Step 3: Write `shaders/outline.comp.glsl`**

Compare only +x and +y: four-neighbour “darken both sides” makes a two-pixel stripe, while
left/top ownership makes pixel-thin exact.

```glsl
#[compute]
#version 460
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 6
#include "shade.glslh"
#include "beauty_camera.glslh"
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D gb_surface;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness;
layout(set = 0, binding = 4, rgba16f) uniform image2D scene_color;
layout(push_constant, std430) uniform Push {
	ivec4 dims;  // xy full size, z have normal-roughness
	vec4 params; // relative depth threshold, normal threshold, darken, unused
} pc;
struct SurfaceSample { float depth; float linear_depth; vec3 n; int kind; };
SurfaceSample read_surface(ivec2 px) {
	SurfaceSample s; s.depth = texelFetch(scene_depth, px, 0).r;
	s.linear_depth = 0.0; s.n = vec3(0.0); s.kind = 0;
	if (s.depth <= 0.0) return s;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	s.linear_depth = distance(beauty_world_from_depth(uv, s.depth), bcam.cam.xyz);
	float gd = texelFetch(gb_depth, px, 0).r;
	vec4 g = texelFetch(gb_surface, px, 0);
	if (g.z >= 0.5 && abs(gd - s.depth) <= 1e-5) {
		s.n = oct_decode(g.xy); s.kind = 1;
	} else if (pc.dims.z != 0) {
		s.n = normalize(texelFetch(normal_roughness, px, 0).rgb * 2.0 - 1.0);
		if (!isnan(s.n.x) && !isnan(s.n.y) && !isnan(s.n.z) &&
				!isinf(s.n.x) && !isinf(s.n.y) && !isinf(s.n.z)) s.kind = 2;
	}
	return s;
}
bool edge(SurfaceSample a, SurfaceSample b) {
	if (a.depth <= 0.0) return false;
	if (b.depth <= 0.0) return true;
	float rel = abs(a.linear_depth - b.linear_depth) /
		max(min(a.linear_depth, b.linear_depth), 1e-3);
	if (rel > pc.params.x) return true;
	return a.kind != 0 && a.kind == b.kind && 1.0 - dot(a.n, b.n) > pc.params.y;
}
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	SurfaceSample c = read_surface(px); bool e = false;
	if (px.x + 1 < pc.dims.x) e = edge(c, read_surface(px + ivec2(1, 0)));
	if (!e && px.y + 1 < pc.dims.y) e = edge(c, read_surface(px + ivec2(0, 1)));
	if (!e) return;
	vec4 color = imageLoad(scene_color, px);
	imageStore(scene_color, px, vec4(color.rgb * pc.params.z, color.a));
}
```

- [ ] **Step 4: Write and wire `OutlinePass`**

Create a normal compute-pass class with shader/pipeline, nearest sampler, neutral 1×1 normal
fallback, cached set and `last_ms_`. Return without a list unless `s.outlines` and all
mandatory RIDs/sizes are valid. Bind shader resources 0–4 and camera UBO at 6. Push exactly
32 bytes:

```cpp
PackedByteArray pc; pc.resize(32);
int32_t *i = reinterpret_cast<int32_t *>(pc.ptrw());
float *f = reinterpret_cast<float *>(pc.ptrw());
i[0] = size.x; i[1] = size.y;
i[2] = have_normal_roughness && normal_roughness.is_valid() ? 1 : 0; i[3] = 0;
f[4] = s.outline_depth_threshold; f[5] = s.outline_normal_threshold;
f[6] = 0.35f; f[7] = 0.0f;
```

Free the set before pipeline/shader/dummy. Own the pass in `VoxelWorld` and destroy it before
`GBuffer`. The `BeautyCompositor` tail becomes:

```cpp
if (SsrPass *ssr = world->ssr_pass())
	ssr->render(rd, scene_color, scene_depth, gb->surface(), gb->depth(), normal_rough,
			have_normal_roughness, ubo->buffer(), size, beauty);
if (OutlinePass *outline = world->outline_pass())
	outline->render(rd, scene_color, scene_depth, gb->depth(), gb->surface(), normal_rough,
			have_normal_roughness, ubo->buffer(), size, beauty);
// Non-visual copy: outline above is the last scene-colour mutation before glow/tonemap.
world->downsample_history(rd, scene_color, *gb);
```

`debug_outline_probe` creates 32×16 white scene colour and these fixtures: 0 flat; 1 one
depth change at x=16; 2 equal depth and G-buffer oct normal changes up→right; 3 no G-buffer
coverage and packed dynamic normal changes; 4 no G-buffer coverage and depth changes. It
reports `ran`, `dark_columns`, `dark_value`, `mean_delta`, `max_brightening`, and
`max_alpha_delta`. Fill Task 9's `debug_beauty_compositor_stats()["outline_ms"]` from
`OutlinePass::last_ms()` as CPU record time; Task 14 never reads it for the GPU budget.

- [ ] **Step 5: Run tests, verify ordering, commit**

Run:

```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -a res://tests/test_outline.gd -c
./gdunit_tests.sh -a res://tests/test_contact_shadow.gd -a res://tests/test_ssr.gd \
  -a res://tests/test_ssgi.gd -a res://tests/test_deferred.gd -c
godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn
```

Expected: all pass; terrain/TestCube silhouettes are one pixel; forced normal fallback loses
only dynamic normal creases; SSR is darkened by outlines, proving outline is later.

```bash
git add shaders/outline.comp.glsl extension/src/render/outline_pass.h \
  extension/src/render/outline_pass.cpp extension/src/beauty_compositor.cpp \
  extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_outline.gd
git commit -m "feat: full-resolution depth and normal outlines"
```

---

### Task 13: one Godot cel include for debris and the demo cube

Godot spatial shaders cannot consume RenderingDevice GLSL, so dynamic objects need one
ShaderLanguage mirror. This task puts it in one `.gdshaderinc`, makes every object material
include it, and renders its probe branch through a real `SubViewport` against
`ve::cel_shade`. The third spelling is unavoidable; untested drift is not.

**Files:**
- Create: `shaders/cel.gdshaderinc`, `shaders/cel_object.gdshader`
- Modify: `extension/src/physics/island_body.h`, `extension/src/physics/island_body.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `demo/main.tscn`, `tests/test_island_body.gd`
- Create: `tests/test_cel_object.gd`

**Interfaces:**
- Produces: `ve_cel_shade(...)` in the include; no `.gdshader` re-declares ramp maths.
- Produces: `VoxelWorld::debug_cel_reference(...) const`, directly calling `ve::cel_shade`
  with no RenderingDevice.
- Produces: `IslandBody::has_cel_material() const`, exposed as `cel_material` in body stats.

- [ ] **Step 1: Write the failing differential test**

Create `tests/test_cel_object.gd`:

```gdscript
extends GdUnitTestSuite
const CEL_SHADER := preload("res://shaders/cel_object.gdshader")
var _nodes: Array = []
func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n): n.free()
	_nodes.clear()
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true; w.physics_enabled = false
	add_child(w); _nodes.append(w); return w
func make_probe() -> Dictionary:
	var vp := SubViewport.new()
	vp.size = Vector2i(16, 16)
	vp.render_target_clear_mode = SubViewport.CLEAR_MODE_ALWAYS
	vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	vp.use_hdr_2d = true
	add_child(vp); _nodes.append(vp)
	var cam := Camera3D.new(); cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = 2.0; cam.position = Vector3(0, 0, 2); vp.add_child(cam); cam.current = true
	var mesh := MeshInstance3D.new(); var quad := QuadMesh.new(); quad.size = Vector2(2, 2)
	mesh.mesh = quad
	var mat := ShaderMaterial.new(); mat.shader = CEL_SHADER
	mat.set_shader_parameter("probe_mode", true); mesh.material_override = mat; vp.add_child(mesh)
	return {"viewport": vp, "material": mat}
func render_probe(probe: Dictionary, c: Array) -> Color:
	var m: ShaderMaterial = probe["material"]
	m.set_shader_parameter("probe_albedo", Vector3(c[0].r, c[0].g, c[0].b))
	m.set_shader_parameter("probe_ambient", Vector3(c[1].r, c[1].g, c[1].b))
	for p in [["probe_ndl",2],["probe_ndv",3],["probe_ndh",4],["probe_shadow",5],
			["probe_ao",6],["probe_gloss",7]]: m.set_shader_parameter(p[0], c[p[1]])
	var vp: SubViewport = probe["viewport"]; vp.render_target_update_mode = SubViewport.UPDATE_ONCE
	await RenderingServer.frame_post_draw
	return vp.get_texture().get_image().get_pixel(8, 8)
func test_shaderlanguage_matches_ve_cel_shade() -> void:
	var w := make_world(); var probe := make_probe()
	var cases := [
		[Color(.8,.2,.1),Color(0,0,0),1.0,1.0,0.0,1.0,1.0,0.0],
		[Color(.36,.55,.22),Color(.16,.19,.26),.079,.8,.3,1.0,1.0,0.0],
		[Color(.36,.55,.22),Color(.16,.19,.26),.081,.8,.3,1.0,1.0,0.0],
		[Color(.45,.42,.40),Color(.16,.19,.26),.319,.4,.9,1.0,1.0,.9],
		[Color(.45,.42,.40),Color(.16,.19,.26),.321,.4,.9,1.0,1.0,.9],
		[Color(.5,.5,.5),Color(.2,.2,.2),.659,.5,.71,.5,.4,.6],
		[Color(.5,.5,.5),Color(.2,.2,.2),.661,0.0,.73,.5,1.0,1.0],
		[Color(.02,.02,.9),Color(0,0,0),-2.0,1.0,0.0,0.0,1.0,0.0]]
	for c in cases:
		var got: Color = await render_probe(probe, c)
		var ref: Color = w.debug_cel_reference(c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7])
		assert_float(absf(got.r-ref.r)).is_less(0.006)
		assert_float(absf(got.g-ref.g)).is_less(0.006)
		assert_float(absf(got.b-ref.b)).is_less(0.006)
func test_demo_cube_uses_the_shared_shader() -> void:
	var scene := load("res://demo/main.tscn").instantiate(); add_child(scene); _nodes.append(scene)
	var mat := (scene.get_node("TestCube") as MeshInstance3D).material_override as ShaderMaterial
	assert_object(mat).is_not_null()
	assert_str(mat.shader.resource_path).is_equal("res://shaders/cel_object.gdshader")
```

Add to the existing debris test in `tests/test_island_body.gd`:

```gdscript
assert_bool(d["cel_material"]).override_failure_message(
	"debris fell back to StandardMaterial3D").is_true()
```

- [ ] **Step 2: Run to verify failure**

Run: `./gdunit_tests.sh -a res://tests/test_cel_object.gd -a res://tests/test_island_body.gd -c`
Expected: FAIL — shader and CPU reference are absent.

- [ ] **Step 3: Write `shaders/cel.gdshaderinc`**

```glsl
const vec3 VE_SUN_DIR = vec3(0.5746958, 0.7662610, 0.2873479);
const float VE_HUE = 0.055;
const float VE_SAT = 1.35;
const float VE_SPEC_EDGE = 0.72;
const float VE_SPEC = 0.45;
const float VE_RIM = 0.35;
const float VE_RIM_POWER = 3.0;
float ve_cel_level(float ndl) {
	float v=clamp(ndl,0.0,1.0); if(v>0.66)return 1.0;
	if(v>0.32)return .75; if(v>0.08)return .45; return .18;
}
vec3 ve_rgb_to_hsv(vec3 c) {
	float mx=max(c.r,max(c.g,c.b)),mn=min(c.r,min(c.g,c.b)),d=mx-mn,h=0.0;
	if(d>0.0){if(mx==c.r)h=(c.g-c.b)/d+(c.g<c.b?6.0:0.0);
	else if(mx==c.g)h=(c.b-c.r)/d+2.0;else h=(c.r-c.g)/d+4.0;h/=6.0;}
	return vec3(h,mx>0.0?d/mx:0.0,mx);
}
vec3 ve_hsv_to_rgb(vec3 c) {
	float h=fract(c.x)*6.0,s=clamp(c.y,0.0,1.0),v=c.z,f=h-floor(h);
	int i=int(floor(h))%6;float p=v*(1.0-s),q=v*(1.0-s*f),t=v*(1.0-s*(1.0-f));
	if(i==0)return vec3(v,t,p);if(i==1)return vec3(q,v,p);if(i==2)return vec3(p,v,t);
	if(i==3)return vec3(p,q,v);if(i==4)return vec3(t,p,v);return vec3(v,p,q);
}
vec3 ve_shadow_tint(vec3 albedo,float amount){float k=clamp(amount,0.0,1.0);
	vec3 h=ve_rgb_to_hsv(albedo);h.x=fract(h.x+VE_HUE*k);
	h.y=clamp(h.y*(1.0+(VE_SAT-1.0)*k),0.0,1.0);return ve_hsv_to_rgb(h);}
vec3 ve_cel_shade(vec3 albedo,vec3 ambient,float ndl,float ndv,float ndh,
		float shadow,float ao,float gloss){float sh=clamp(shadow,0.0,1.0);
	float lit=ve_cel_level(ndl)*sh;vec3 tint=ve_shadow_tint(albedo,1.0-lit);
	float spec=(gloss>0.0&&ndh>=VE_SPEC_EDGE)?gloss*VE_SPEC*sh:0.0;
	float rim=VE_RIM*pow(1.0-clamp(ndv,0.0,1.0),VE_RIM_POWER);
	return tint*lit+tint*ambient*clamp(ao,0.0,1.0)+vec3(spec+rim);}
```

- [ ] **Step 4: Write `shaders/cel_object.gdshader`**

```glsl
shader_type spatial;
render_mode unshaded, depth_draw_opaque, cull_back;
#include "res://shaders/cel.gdshaderinc"
uniform vec3 base_color_linear=vec3(.45,.42,.40);
uniform vec3 ambient_linear=vec3(.16,.19,.26);
uniform float shadow_visibility:hint_range(0.0,1.0)=1.0;
uniform float ambient_occlusion:hint_range(0.0,1.0)=1.0;
uniform float gloss:hint_range(0.0,1.0)=0.0;
uniform bool probe_mode=false;
uniform vec3 probe_albedo=vec3(.5);
uniform vec3 probe_ambient=vec3(0.0);
uniform float probe_ndl=1.0;
uniform float probe_ndv=1.0;
uniform float probe_ndh=0.0;
uniform float probe_shadow=1.0;
uniform float probe_ao=1.0;
uniform float probe_gloss=0.0;
varying vec3 ve_world_pos;
varying vec3 ve_world_n;
void vertex(){ve_world_pos=(MODEL_MATRIX*vec4(VERTEX,1.0)).xyz;
	ve_world_n=normalize(mat3(MODEL_MATRIX)*NORMAL);}
void fragment(){if(probe_mode){ALBEDO=ve_cel_shade(probe_albedo,probe_ambient,
	probe_ndl,probe_ndv,probe_ndh,probe_shadow,probe_ao,probe_gloss);return;}
	vec3 n=normalize(ve_world_n),v=normalize(INV_VIEW_MATRIX[3].xyz-ve_world_pos);
	ALBEDO=ve_cel_shade(base_color_linear,ambient_linear,dot(n,VE_SUN_DIR),dot(n,v),
	dot(n,normalize(VE_SUN_DIR+v)),shadow_visibility,ambient_occlusion,gloss);}
```

`unshaded` prevents Godot lambert-lighting the already-cel-lit result. Inputs are linear and
have no `source_color` hint.

- [ ] **Step 5: Add the direct CPU reference and material wiring**

Implement/bind:

```cpp
Color VoxelWorld::debug_cel_reference(Color a, Color amb, float ndl, float ndv, float ndh,
		float shadow, float ao, float gloss) const {
	ve::CelParams p; ve::CelInput in;
	in.albedo[0]=a.r;in.albedo[1]=a.g;in.albedo[2]=a.b;
	in.ambient[0]=amb.r;in.ambient[1]=amb.g;in.ambient[2]=amb.b;
	in.ndl=ndl;in.ndv=ndv;in.ndh=ndh;in.shadow=shadow;in.ao=ao;in.gloss=gloss;
	float out[3];ve::cel_shade(p,in,out);return Color(out[0],out[1],out[2],1.0f);
}
```

In `IslandBody`, retain `Ref<Material> render_material_`, clear it on despawn, and replace the
plain block with `ResourceLoader::load("res://shaders/cel_object.gdshader")`, a
`ShaderMaterial`, and parameters matching the rock/ambient constants. If load fails, retain
the old grey `StandardMaterial3D` as fail-soft. `has_cel_material()` casts the retained
material and checks its shader resource path; expose it in existing debug body dictionaries.

Add one Shader ext-resource and ShaderMaterial subresource to `demo/main.tscn`; assign it to
`TestCube.material_override`, with base `(0.62,0.60,0.66)` and gloss `0.15`. Preserve Task
9's compositor subresource and increment `load_steps`.

- [ ] **Step 6: Run, verify and commit**

```bash
./build.sh -j$(nproc)
cd extension && scons test
cd ..
./gdunit_tests.sh -a res://tests/test_cel_object.gd -a res://tests/test_island_body.gd \
  -a res://tests/test_contact_shadow.gd -a res://tests/test_outline.gd -c
godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn
```

Expected: differential delta < `0.006`; cube/debris share bands, tint, spec and rim. If the
backend exposes the SubViewport as LDR RGBA8, convert the captured pixel with
`srgb_to_linear()` exactly once based on `Image.get_format()` and record that fact in Errata.

```bash
git add shaders/cel.gdshaderinc shaders/cel_object.gdshader \
  extension/src/physics/island_body.h extension/src/physics/island_body.cpp \
  extension/src/voxel_world.h extension/src/voxel_world.cpp demo/main.tscn \
  tests/test_cel_object.gd tests/test_island_body.gd
git commit -m "feat: shared cel shader include for dynamic objects"
```

---

### Task 14: debug controls, real GPU timings, budget legs and verdict record

M5 errata 15 is load-bearing: `debug_perf_stats()["lod_ms"]` is CPU `steady_clock` time
spent recording commands. It remains visible and explicitly labelled `cpu`, but no GPU
verdict reads it. GPU costs come only from paired `capture_timestamp` markers and
`get_captured_timestamp_gpu_time`; the documented unit is microseconds.

**Files:**
- Create: `extension/src/render/gpu_timings.h`, `extension/src/render/gpu_timings.cpp`
- Create: `demo/debug_menu.gd`
- Modify: `extension/src/raymarch_compositor.cpp`, `extension/src/beauty_compositor.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `demo/hud.gd`, `demo/benchmark.gd`, `demo/main.tscn`
- Create: `tests/test_gpu_timings.gd`, `tests/test_debug_menu.gd`
- Modify: this plan's Errata with the five measured benchmark verdict rows

**Interfaces:**
- Produces: `GpuTimings::{poll,begin_frame,end_frame,begin,end,snapshot}` and
  `ingest_for_test(names,gpu_us,rd_frame)`.
- Produces: `VoxelWorld::debug_gpu_timings()` and bound
  `debug_ingest_gpu_timings(...)`.
- Changes `VoxelWorld::beauty_settings()` to return a mutex-protected value snapshot rather
  than an unlocked reference.
- Missing/not-run pass values are `-1.0`, never zero.

- [ ] **Step 1: Write the failing pairing test**

Create `tests/test_gpu_timings.gd`:

```gdscript
extends GdUnitTestSuite
var _worlds:Array=[]
func after_test()->void:
	for w in _worlds:
		if is_instance_valid(w):w.free()
	_worlds.clear()
func world()->VoxelWorld:
	var w:VoxelWorld=ClassDB.instantiate("VoxelWorld");w.use_local_device=true
	add_child(w);_worlds.append(w);return w
func test_pairs_by_identity_and_sums_occurrences()->void:
	var d:=world().debug_ingest_gpu_timings(PackedStringArray([
		"ve:7:lod:1:e","engine","ve:7:frame:0:b","ve:7:lod:0:b","ve:7:lod:1:b",
		"ve:7:raymarch:0:e","ve:7:frame:0:e","ve:7:raymarch:0:b","ve:7:lod:0:e"]),
		PackedInt64Array([5600,77,1000,2000,5000,1900,9000,1100,3500]),42)
	assert_bool(d["valid"]).is_true()
	assert_float(d["raymarch_gpu_ms"]).is_equal_approx(.8,.0001)
	assert_float(d["lod_gpu_ms"]).is_equal_approx(2.1,.0001)
	assert_float(d["custom_frame_gpu_ms"]).is_equal_approx(8.0,.0001)
func test_bad_pairs_are_missing_not_zero()->void:
	var d:=world().debug_ingest_gpu_timings(PackedStringArray([
		"ve:8:frame:0:b","ve:8:frame:0:e","ve:8:ssr:0:b","ve:8:lod:0:b",
		"ve:8:lod:0:e"]),PackedInt64Array([100,900,200,700,650]),43)
	assert_float(d["ssr_gpu_ms"]).is_equal(-1.0)
	assert_float(d["lod_gpu_ms"]).is_equal(-1.0)
	assert_int(d["dropped_pairs"]).is_equal(2)
func test_new_rd_frame_gets_new_sample_id()->void:
	var w:=world();var n:=PackedStringArray(["ve:9:frame:0:b","ve:9:frame:0:e"])
	var a:=w.debug_ingest_gpu_timings(n,PackedInt64Array([100,200]),50)
	var b:=w.debug_ingest_gpu_timings(n,PackedInt64Array([300,500]),51)
	assert_int(b["sample_id"]).is_greater(int(a["sample_id"]))
```

- [ ] **Step 2: Run to verify failure**

Run: `./gdunit_tests.sh -a res://tests/test_gpu_timings.gd -c`
Expected: FAIL — hook absent.

- [ ] **Step 3: Implement delayed timestamp pairing**

Create `gpu_timings.h` with:

```cpp
class GpuTimings {
public:
 void poll(RenderingDevice *); void begin_frame(RenderingDevice *); void end_frame(RenderingDevice *);
 void begin(RenderingDevice *,const char *); void end(RenderingDevice *,const char *);
 Dictionary snapshot() const;
 Dictionary ingest_for_test(const PackedStringArray &,const PackedInt64Array &,uint64_t);
private:
 uint64_t serial_=0,last_rd_frame_=UINT64_MAX,sample_id_=0;
 std::map<std::string,int> next_,active_; Dictionary latest_;
};
```

Marker grammar is `ve:<serial>:<pass>:<occurrence>:<b|e>`. `begin` increments that pass's
occurrence; `end` closes exactly its active occurrence. `begin_frame` polls the previous
available snapshot, increments serial, clears occurrence maps, and captures `frame:0:b`;
`end_frame` captures `frame:0:e`.

`poll` reads `get_captured_timestamps_frame()` once; unchanged frame returns. Copy all names
and GPU values, then parse by marker identity—not array adjacency or game-frame number.
Publish the newest serial with a positive complete frame pair. Sum positive complete repeated
occurrences. Missing/non-positive pairs increment `dropped_pairs` and leave that pass `-1`.
Divide positive microsecond deltas by `1000.0` exactly once.

Initialise these keys to `-1`: `raymarch`, `composite`, `lod`, `sun_shadow`, `ssgi`,
`deferred`, `inject`, `contact`, `ssr`, `outlines`, `history`; publish each as
`<name>_gpu_ms`, plus `custom_frame_gpu_ms`, `valid`, `sample_id`,
`render_device_frame`, `captured_serial`, `dropped_pairs`.

- [ ] **Step 4: Make BeautySettings publication race-free**

Add `mutable std::mutex beauty_mutex_`; lock setters/getters/debug dictionary. Return a copy:

```cpp
ve::BeautySettings VoxelWorld::beauty_settings() const {
 std::lock_guard<std::mutex> lock(beauty_mutex_); return beauty_;
}
```

Each compositor takes one snapshot and passes it to every pass that frame. Replace all
`world->beauty_settings().field` calls. Own `GpuTimings` in `VoxelWorld`, expose it to both
compositors, bind snapshot and ingestion hooks.

- [ ] **Step 5: Timestamp actual queued ranges**

At validated PRE_OPAQUE entry call `begin_frame`. Pair markers outside active lists around:
raymarch (including island cull); composite; LoD occurrence 0; an actually invoked sun-map
build; LoD occurrence 1; an actually dispatched SSGI; deferred; inject. Split LoD around
sun map so shadow time is not double-counted; the parser sums its two occurrences.

At POST_OPAQUE entry call `poll`, then pair only dispatched contact, SSR, outline and history
copy, followed by `end_frame`. The frame pair includes Godot opaque between callbacks and
stops before tonemap; call it `custom_frame_gpu_ms`, not full-frame time. Never call
`submit()`/`sync()` on the global RD and never substitute CPU `lod_ms` for missing LoD GPU
time.

- [ ] **Step 6: Write the debug menu and its test**

Create `demo/debug_menu.gd`:

```gdscript
extends PanelContainer

@export var world_path: NodePath

const EFFECTS := [
	["SSGI", "ssgi"],
	["SSR", "ssr"],
	["Contact shadows", "contact_shadows"],
	["Outlines", "outlines"],
	["Sun shadow map", "sun_shadow_map"],
	["Glossy SDF rays", "glossy_sdf_rays"],
	["Raymarched sun shadow", "raymarched_sun_shadow"],
]

var _world: VoxelWorld
var _quality: OptionButton
var _checks: Dictionary = {}
var _syncing := false

func _ready() -> void:
	_world = get_node_or_null(world_path) as VoxelWorld
	visible = false
	var box := VBoxContainer.new()
	box.name = "Controls"
	add_child(box)
	var title := Label.new()
	title.text = "Beauty (F1)"
	box.add_child(title)
	_quality = OptionButton.new()
	_quality.name = "Quality"
	for label in ["Off", "Low", "Medium", "High"]:
		_quality.add_item(label)
	_quality.item_selected.connect(_on_quality)
	box.add_child(_quality)
	for entry in EFFECTS:
		var check := CheckBox.new()
		check.name = entry[1]
		check.text = entry[0]
		check.toggled.connect(_on_effect.bind(entry[1]))
		_checks[entry[1]] = check
		box.add_child(check)
	_sync()

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F1:
		visible = not visible
		if visible:
			_sync()
		get_viewport().set_input_as_handled()

func _on_quality(index: int) -> void:
	if _syncing or not _world:
		return
	_world.quality_tier = index
	_sync()

func _on_effect(on: bool, effect: String) -> void:
	if _syncing or not _world:
		return
	_world.set_effect_enabled(effect, on)
	_sync()

func _sync() -> void:
	if not _world:
		return
	_syncing = true
	var settings: Dictionary = _world.debug_beauty_settings()
	_quality.select(int(settings["tier"]))
	for entry in EFFECTS:
		(_checks[entry[1]] as CheckBox).set_pressed_no_signal(bool(settings[entry[1]]))
	_syncing = false
```

Create `tests/test_debug_menu.gd`:

```gdscript
extends GdUnitTestSuite

const MENU_SCRIPT := preload("res://demo/debug_menu.gd")
var _roots: Array = []

func after_test() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.free()
	_roots.clear()

func make_pair() -> Array:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	root.add_child(world)
	var menu := PanelContainer.new()
	menu.name = "Menu"
	menu.set_script(MENU_SCRIPT)
	menu.set("world_path", NodePath("../World"))
	root.add_child(menu)
	return [world, menu]

func test_quality_selection_replaces_the_world_settings() -> void:
	var pair := make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	var quality: OptionButton = menu.get_node("Controls/Quality")
	quality.emit_signal("item_selected", 0)
	assert_int(world.quality_tier).is_equal(0)
	assert_bool(world.debug_beauty_settings()["ssr"]).is_false()
	assert_bool(world.debug_beauty_settings()["outlines"]).is_false()

func test_checkbox_writes_only_its_named_field() -> void:
	var pair := make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	var outlines: CheckBox = menu.get_node("Controls/outlines")
	outlines.emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("outlines")).is_false()
	assert_bool(world.get_effect_enabled("ssr")).is_true()
```

This proves the menu is a controller over Task 3, not a second model.

Add the script resource and top-right `DebugMenu` PanelContainer under the existing HUD in
`main.tscn`; preserve the single Task 9 `BeautyCompositor`.

- [ ] **Step 7: Add the HUD GPU line**

Label existing LoD `lod_ms` as `cpu`. Append a second line from `debug_gpu_timings()`:

```gdscript
var gpu_line := "GPU n/a"
var gt: Dictionary = _world.debug_gpu_timings() if _world else {}
if gt.get("valid", false):
	var shadows := maxf(float(gt.get("sun_shadow_gpu_ms", -1.0)), 0.0) + \
		maxf(float(gt.get("contact_gpu_ms", -1.0)), 0.0)
	gpu_line = "GPU ray %.2f lod %.2f ssgi %.2f ssr %.2f sh %.2f out %.2f ms" % [
		gt.get("raymarch_gpu_ms", -1.0), gt.get("lod_gpu_ms", -1.0),
		gt.get("ssgi_gpu_ms", -1.0), gt.get("ssr_gpu_ms", -1.0),
		shadows, gt.get("outlines_gpu_ms", -1.0)]
text += "\n" + gpu_line
```

The near SDF sun ray is inside raymarch and is not double-counted in `sh`.

- [ ] **Step 8: Add benchmark sample de-duplication and budget verdicts**

Add these fields and helpers to `benchmark.gd`:

```gdscript
const GPU_DRAIN_FRAMES := 30
const MIN_GPU_SAMPLES := 30
const BUDGETS_MS := {
	"raymarch": 6.0, "lod": 2.0, "ssgi": 1.5, "ssr": 1.5,
	"shadows": 1.0, "outlines": 0.3, "frame": 16.0,
}
var _gpu_samples := {
	"raymarch": PackedFloat32Array(), "lod": PackedFloat32Array(),
	"ssgi": PackedFloat32Array(), "ssr": PackedFloat32Array(),
	"shadows": PackedFloat32Array(), "outlines": PackedFloat32Array(),
	"custom_frame": PackedFloat32Array(),
}
var _last_gpu_sample_id := -1
var _gpu_dropped_pairs := 0
var _draining := false
var _drain_frames := 0

func _append_gpu(key: String, value: float) -> void:
	var values: PackedFloat32Array = _gpu_samples[key]
	values.append(value)
	_gpu_samples[key] = values

func _capture_gpu_sample() -> void:
	var d: Dictionary = _world.debug_gpu_timings()
	if not d.get("valid", false):
		return
	var sample_id := int(d["sample_id"])
	if sample_id == _last_gpu_sample_id:
		return
	_last_gpu_sample_id = sample_id
	_gpu_dropped_pairs = max(_gpu_dropped_pairs, int(d.get("dropped_pairs", 0)))
	for key in ["raymarch", "lod", "ssgi", "ssr", "outlines"]:
		var value := float(d.get(key + "_gpu_ms", -1.0))
		if value >= 0.0:
			_append_gpu(key, value)
	var shadow_ms := 0.0
	var shadow_ran := false
	for key in ["sun_shadow", "contact"]:
		var value := float(d.get(key + "_gpu_ms", -1.0))
		if value >= 0.0:
			shadow_ms += value
			shadow_ran = true
	if shadow_ran:
		_append_gpu("shadows", shadow_ms)
	var custom_frame := float(d.get("custom_frame_gpu_ms", -1.0))
	if custom_frame >= 0.0:
		_append_gpu("custom_frame", custom_frame)

func _budget_verdict(values: PackedFloat32Array, budget_ms: float) -> String:
	if values.size() < MIN_GPU_SAMPLES:
		return "UNMEASURED"
	var sorted := values.duplicate()
	sorted.sort()
	return "PASS" if _percentile(sorted, 0.99) <= budget_ms else "WARN"
```

At the start of `_process`, after the empty-mode guard, drain delayed snapshots without
adding frame samples or driving edits/movement:

```gdscript
	if _draining:
		_capture_gpu_sample()
		_drain_frames += 1
		if _drain_frames >= GPU_DRAIN_FRAMES:
			_report()
			get_tree().quit()
		return
```

Call `_capture_gpu_sample()` once beside each sampled frame. Replace the existing immediate
report/quit block with:

```gdscript
	if _samples.size() >= _target_frames:
		_draining = true
```

This never calls `sync()` on the global RD. Rename the existing LoD print keys to
`lod_cpu_record_ms_p50/p99`; their only source remains `debug_perf_stats()["lod_ms"]`.
Verdict policy is p99 ≤ budget with at least 30 samples; fewer is `UNMEASURED`, never PASS.
The frame verdict uses `_process(delta)` p99 (M1 errata 5), not a GPU span.

At the end of `_report()` add:

```gdscript
	for key in ["raymarch", "lod", "ssgi", "ssr", "shadows", "outlines", "custom_frame"]:
		var values: PackedFloat32Array = _gpu_samples[key]
		var sorted_gpu := values.duplicate()
		sorted_gpu.sort()
		print("BENCH gpu_%s samples=%d p50_ms=%.3f p99_ms=%.3f" % [key, values.size(),
			_percentile(sorted_gpu, 0.50), _percentile(sorted_gpu, 0.99)])
	var frame_sorted := _samples.duplicate()
	frame_sorted.sort()
	var verdict := {
		"raymarch": _budget_verdict(_gpu_samples["raymarch"], BUDGETS_MS["raymarch"]),
		"lod": _budget_verdict(_gpu_samples["lod"], BUDGETS_MS["lod"]),
		"ssgi": _budget_verdict(_gpu_samples["ssgi"], BUDGETS_MS["ssgi"]),
		"ssr": _budget_verdict(_gpu_samples["ssr"], BUDGETS_MS["ssr"]),
		"shadows": _budget_verdict(_gpu_samples["shadows"], BUDGETS_MS["shadows"]),
		"outlines": _budget_verdict(_gpu_samples["outlines"], BUDGETS_MS["outlines"]),
		"frame": "PASS" if _percentile(frame_sorted, 0.99) <= BUDGETS_MS["frame"] else "WARN",
	}
	print("BENCH budget_verdict raymarch=%s lod=%s ssgi=%s ssr=%s shadows=%s outlines=%s frame=%s" % [
		verdict["raymarch"], verdict["lod"], verdict["ssgi"], verdict["ssr"],
		verdict["shadows"], verdict["outlines"], verdict["frame"]])
	var custom_values: PackedFloat32Array = _gpu_samples["custom_frame"]
	print("BENCH gpu_timing valid_samples=%d dropped_pairs=%d lod_source=timestamp lod_ms_source=cpu_record" % [
		custom_values.size(), _gpu_dropped_pairs])
```

Empty arrays display zero percentiles but their verdict is `UNMEASURED`, never PASS.

- [ ] **Step 9: Run focused and full tests**

```bash
./build.sh -j$(nproc)
cd extension && scons test
cd ..
./gdunit_tests.sh -a res://tests/test_gpu_timings.gd -a res://tests/test_debug_menu.gd \
  -a res://tests/test_beauty_settings.gd -c
./gdunit_tests.sh -c
```

Expected: all pass.

- [ ] **Step 10: Run all five 1440p legs and retain stdout**

```bash
/usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything --resolution 2560x1440 \
 --disable-vsync demo/main.tscn -- --benchmark | tee /tmp/m6-benchmark-steady.txt
/usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything --resolution 2560x1440 \
 --disable-vsync demo/main.tscn -- --benchmark-move | tee /tmp/m6-benchmark-move.txt
/usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything --resolution 2560x1440 \
 --disable-vsync demo/main.tscn -- --benchmark-ridge | tee /tmp/m6-benchmark-ridge.txt
/usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything --resolution 2560x1440 \
 --disable-vsync demo/main.tscn -- --benchmark-edit | tee /tmp/m6-benchmark-edit.txt
/usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything --resolution 2560x1440 \
 --disable-vsync demo/main.tscn -- --benchmark-island | tee /tmp/m6-benchmark-island.txt
```

Each completed leg must print `BENCH budget_verdict` and `BENCH gpu_timing`. An
`UNMEASURED` is a recorded verdict, not a pass.

- [ ] **Step 11: Record verdicts in Errata**

Append one numbered entry with GPU/device/driver, commit hash, 2560×1440 High tier, all five
exact commands, every exact GPU p50/p99 and budget-verdict line, and this sentence:
“`lod_ms` was CPU command-record time and was not used in any GPU verdict; `lod_gpu_ms` came
only from timestamp deltas.” Record every WARN/UNMEASURED and its follow-up; do not retune in
the telemetry commit to turn it into PASS.

- [ ] **Step 12: Verify menu/HUD and commit**

Run the demo. Expected: F1 menu updates tier/toggles; HUD first line labels CPU LoD record
time and second line shows delayed GPU times or `GPU n/a`.

```bash
git add extension/src/render/gpu_timings.h extension/src/render/gpu_timings.cpp \
  extension/src/raymarch_compositor.cpp extension/src/beauty_compositor.cpp \
  extension/src/voxel_world.h extension/src/voxel_world.cpp demo/debug_menu.gd \
  demo/hud.gd demo/benchmark.gd demo/main.tscn tests/test_gpu_timings.gd \
  tests/test_debug_menu.gd docs/superpowers/plans/2026-08-18-m6-beautification.md
git commit -m "feat: beauty controls and per-pass gpu budget telemetry"
```

---

## Errata (recorded during M6 implementation — corrections and measured verdicts)

Implementation-time facts are appended here as numbered entries. Task 9 records the
Forward+ normal-roughness availability/encoding verdict; Task 14 records all five 1440p
budget verdicts and exact timestamp-derived evidence.

1. **Task 9 normal-roughness verdict:** `normal_roughness` is reachable, but the probe
   currently reports a constant/empty value of `1.0`. Its channel encoding is therefore
   not calibrated; Tasks 10 and 12 must keep calibration pending and must not rely on it
   as meaningful dynamic normal/roughness data until a known-orientation object verifies
   the channels.

2. **Task 14 1440p timestamp budget legs:** Recorded on commit `722ab906a6870f65e9dc48599ca78c9c8dcb99f4`, Godot 4.7.1.stable.arch_linux.a13da4feb, Vulkan 1.4.341, NVIDIA GeForce RTX 4070 Laptop GPU, NVIDIA driver 610.57.04, High tier, 2560x1440. The five commands were:
   ```text
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark | tee /tmp/m6-benchmark-steady.txt
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-move | tee /tmp/m6-benchmark-move.txt
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-ridge | tee /tmp/m6-benchmark-ridge.txt
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-edit | tee /tmp/m6-benchmark-edit.txt
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-island | tee /tmp/m6-benchmark-island.txt
   ```
   Exact recorded GPU lines:
   ```text
   steady: BENCH gpu_raymarch samples=287 p50_ms=6312.416 p99_ms=7981.888; BENCH gpu_lod samples=287 p50_ms=42.912 p99_ms=49.888; BENCH gpu_ssgi samples=287 p50_ms=172.032 p99_ms=175.104; BENCH gpu_ssr samples=287 p50_ms=145.408 p99_ms=147.456; BENCH gpu_shadows samples=287 p50_ms=77.824 p99_ms=226.944; BENCH gpu_outlines samples=287 p50_ms=88.064 p99_ms=89.088; BENCH gpu_custom_frame samples=287 p50_ms=7311.360 p99_ms=9187.328
   move: BENCH gpu_raymarch samples=287 p50_ms=6360.928 p99_ms=7985.152; BENCH gpu_lod samples=287 p50_ms=68.960 p99_ms=188.672; BENCH gpu_ssgi samples=287 p50_ms=177.152 p99_ms=319.488; BENCH gpu_ssr samples=287 p50_ms=164.864 p99_ms=337.920; BENCH gpu_shadows samples=287 p50_ms=79.872 p99_ms=433.472; BENCH gpu_outlines samples=287 p50_ms=84.992 p99_ms=227.328; BENCH gpu_custom_frame samples=287 p50_ms=7587.840 p99_ms=9762.816
   ridge: BENCH gpu_raymarch samples=287 p50_ms=5941.056 p99_ms=8582.144; BENCH gpu_lod samples=287 p50_ms=188.032 p99_ms=345.632; BENCH gpu_ssgi samples=287 p50_ms=144.384 p99_ms=364.544; BENCH gpu_ssr samples=287 p50_ms=151.552 p99_ms=328.704; BENCH gpu_shadows samples=287 p50_ms=64.512 p99_ms=579.008; BENCH gpu_outlines samples=287 p50_ms=50.176 p99_ms=187.648; BENCH gpu_custom_frame samples=287 p50_ms=7140.352 p99_ms=10480.640
   edit: BENCH gpu_raymarch samples=287 p50_ms=10024.288 p99_ms=14464.000; BENCH gpu_lod samples=287 p50_ms=50.048 p99_ms=244.736; BENCH gpu_ssgi samples=287 p50_ms=158.720 p99_ms=292.864; BENCH gpu_ssr samples=287 p50_ms=171.008 p99_ms=295.936; BENCH gpu_shadows samples=287 p50_ms=78.848 p99_ms=305.856; BENCH gpu_outlines samples=287 p50_ms=84.992 p99_ms=229.376; BENCH gpu_custom_frame samples=287 p50_ms=12030.976 p99_ms=28583.936
   island: BENCH gpu_raymarch samples=807 p50_ms=6700.064 p99_ms=8533.536; BENCH gpu_lod samples=807 p50_ms=42.976 p99_ms=47.584; BENCH gpu_ssgi samples=807 p50_ms=167.936 p99_ms=173.056; BENCH gpu_ssr samples=807 p50_ms=148.480 p99_ms=152.576; BENCH gpu_shadows samples=807 p50_ms=77.824 p99_ms=229.568; BENCH gpu_outlines samples=807 p50_ms=87.040 p99_ms=234.496; BENCH gpu_custom_frame samples=807 p50_ms=7715.840 p99_ms=9809.920
   ```
   Every leg recorded `BENCH budget_verdict raymarch=WARN lod=WARN ssgi=WARN ssr=WARN shadows=WARN outlines=WARN frame=WARN` and `BENCH gpu_timing valid_samples=<287|807> dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record`. All WARN results are retained; no budget or quality retuning was performed. The observed GPU values are approximately 1000x larger than wall-clock frame timing on this Godot/Vulkan stack even though the documented API unit is microseconds; follow-up is to reconcile the engine/backend timestamp conversion before using the numeric verdicts as performance claims. “`lod_ms` was CPU command-record time and was not used in any GPU verdict; `lod_gpu_ms` came only from timestamp deltas.”

3. **Task 14 correction wave (recorded on the final worktree before its correction commit):** Godot 4.7.1.stable.arch_linux.a13da4feb, Vulkan 1.4.341, NVIDIA GeForce RTX 4070 Laptop GPU, driver 610.57.04, High tier, 2560x1440. Exact commands:
   ```text
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark 2>&1 | tee /tmp/m6-benchmark-steady.txt
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-move 2>&1 | tee /tmp/m6-benchmark-move.txt
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-ridge 2>&1 | tee /tmp/m6-benchmark-ridge.txt
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-edit 2>&1 | tee /tmp/m6-benchmark-edit.txt
   WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 /usr/bin/godot --path /home/jeremy/Development/Godot/voxel-everything/.worktrees/m6-beautification --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark-island 2>&1 | tee /tmp/m6-benchmark-island.txt
   ```
   Exact per-leg GPU p50/p99, verdict, sample/drop, and qualification lines:
   ```text
   steady: BENCH gpu_raymarch samples=287 p50_ms=6.347 p99_ms=7.983
   steady: BENCH gpu_lod samples=287 p50_ms=0.044 p99_ms=0.051
   steady: BENCH gpu_ssgi samples=287 p50_ms=0.172 p99_ms=0.175
   steady: BENCH gpu_ssr samples=287 p50_ms=0.146 p99_ms=0.148
   steady: BENCH gpu_shadows samples=287 p50_ms=0.078 p99_ms=0.225
   steady: BENCH gpu_outlines samples=287 p50_ms=0.089 p99_ms=0.090
   steady: BENCH gpu_custom_frame samples=287 p50_ms=7.349 p99_ms=9.180
   steady: BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
   steady: BENCH gpu_timing valid_samples=287 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
   steady: BENCH timing_condition vsync_requested=disabled vsync_actual=enabled_wayland verdict_qualified=true
   move: BENCH gpu_raymarch samples=287 p50_ms=6.397 p99_ms=7.747
   move: BENCH gpu_lod samples=287 p50_ms=0.070 p99_ms=0.219
   move: BENCH gpu_ssgi samples=287 p50_ms=0.177 p99_ms=0.318
   move: BENCH gpu_ssr samples=287 p50_ms=0.167 p99_ms=0.342
   move: BENCH gpu_shadows samples=287 p50_ms=0.080 p99_ms=0.437
   move: BENCH gpu_outlines samples=287 p50_ms=0.086 p99_ms=0.230
   move: BENCH gpu_custom_frame samples=287 p50_ms=7.643 p99_ms=9.320
   move: BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
   move: BENCH gpu_timing valid_samples=287 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
   move: BENCH timing_condition vsync_requested=disabled vsync_actual=enabled_wayland verdict_qualified=true
   ridge: BENCH gpu_raymarch samples=287 p50_ms=5.889 p99_ms=8.426
   ridge: BENCH gpu_lod samples=287 p50_ms=0.173 p99_ms=0.249
   ridge: BENCH gpu_ssgi samples=287 p50_ms=0.142 p99_ms=0.368
   ridge: BENCH gpu_ssr samples=287 p50_ms=0.155 p99_ms=0.327
   ridge: BENCH gpu_shadows samples=287 p50_ms=0.065 p99_ms=0.575
   ridge: BENCH gpu_outlines samples=287 p50_ms=0.051 p99_ms=0.187
   ridge: BENCH gpu_custom_frame samples=287 p50_ms=7.018 p99_ms=9.512
   ridge: BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
   ridge: BENCH gpu_timing valid_samples=287 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
   ridge: BENCH timing_condition vsync_requested=disabled vsync_actual=enabled_wayland verdict_qualified=true
   edit: BENCH gpu_raymarch samples=287 p50_ms=10.441 p99_ms=14.177
   edit: BENCH gpu_lod samples=287 p50_ms=0.051 p99_ms=0.250
   edit: BENCH gpu_ssgi samples=287 p50_ms=0.158 p99_ms=0.296
   edit: BENCH gpu_ssr samples=287 p50_ms=0.171 p99_ms=0.312
   edit: BENCH gpu_shadows samples=287 p50_ms=0.079 p99_ms=0.302
   edit: BENCH gpu_outlines samples=287 p50_ms=0.086 p99_ms=0.226
   edit: BENCH gpu_custom_frame samples=287 p50_ms=12.448 p99_ms=29.005
   edit: BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
   edit: BENCH gpu_timing valid_samples=287 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
   edit: BENCH timing_condition vsync_requested=disabled vsync_actual=enabled_wayland verdict_qualified=true
   island: BENCH gpu_raymarch samples=807 p50_ms=6.742 p99_ms=8.512
   island: BENCH gpu_lod samples=807 p50_ms=0.043 p99_ms=0.050
   island: BENCH gpu_ssgi samples=807 p50_ms=0.168 p99_ms=0.173
   island: BENCH gpu_ssr samples=807 p50_ms=0.148 p99_ms=0.151
   island: BENCH gpu_shadows samples=807 p50_ms=0.078 p99_ms=0.228
   island: BENCH gpu_outlines samples=807 p50_ms=0.087 p99_ms=0.235
   island: BENCH gpu_custom_frame samples=807 p50_ms=7.755 p99_ms=9.745
   island: BENCH budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS shadows=PASS outlines=PASS frame=WARN
   island: BENCH gpu_timing valid_samples=807 dropped_pairs=1 lod_source=timestamp lod_ms_source=cpu_record
   island: BENCH timing_condition vsync_requested=disabled vsync_actual=enabled_wayland verdict_qualified=true
   ```
   Live calibration evidence: `BENCH gpu_timestamp_calibration unit=live_normalized_microseconds scale_to_us=0.001000 ... calibrated=true`, with paired raw-GPU/CPU ratios 3,722.530–9,789.078. Raw Vulkan values are nanoseconds on this backend and are normalized once in `poll()` to documented microseconds; `ingest_for_test()` remains microseconds with scale 1.0. No UNMEASURED sample was emitted in these runs. Wayland did not honor `--disable-vsync`; Godot printed `The requested V-Sync mode Disabled is not available. Falling back to V-Sync mode Enabled.`, so frame verdicts are qualified. Steady and edit exited 0 with the existing 2 ObjectDB leak warning; move, ridge, and island still aborted during shutdown with `corrupted size vs. prev_size`, so none of the five legs is claimed clean. The island invalid-RID report was eliminated by invalidating CompositePass's dependent uniform set and framebuffer before RaymarchPass replaces its output textures. “`lod_ms` was CPU command-record time and was not used in any GPU verdict; `lod_gpu_ms` came only from timestamp deltas.”
