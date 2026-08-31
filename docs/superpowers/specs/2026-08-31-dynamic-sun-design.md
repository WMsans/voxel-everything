# Dynamic Sun Direction and Colour — Design

**Date:** 2026-08-31
**Status:** Approved design, pre-implementation
**Scope:** Drive the sun's direction and colour from the scene's `DirectionalLight3D` instead of
a compile-time constant, and fix the shadow-map depth bias that leaves the far field flat-lit at
the demo's world size.

---

## 1. Two problems, one of which is not the one it looks like

### 1.1 The sun is a compile-time constant

The sun direction is baked into three mirrored declarations that must agree by hand:

| Site | Declaration |
|---|---|
| `extension/src/shade/cel.h:10` | `ve::kSunDir` |
| `shaders/shade.glslh:10` | `SUN_DIR` |
| `shaders/cel.gdshaderinc:1` | `VE_SUN_DIR` |

The `DirectionalLight3D` at `demo/main.tscn:78` is read by nothing. Rotating it changes no pixel
the voxel renderer produces. This is the reported "raytraced shadow does not reflect the light
node", and it is exactly what it appears to be.

The staticness is also documented as load-bearing, at `sun_ortho.h:9-12`: *"It is STATIC. This
world is bounded and the sun does not move, so there is no camera to follow, no texel snapping to
do and no shimmer to fight — which is the whole reason a single 2048 map is enough for a 4 km
world."* Section 5 revisits each of those claims; only one of them survives, and it is the one
that matters least.

### 1.2 The far field is not missing a shadow feature — its bias is in the wrong unit

The rasterized far field already has a complete sun-shadow path, and it is wired end to end:

- `shaders/lod.frag.glsl:41` writes sun visibility `1.0` and defers to the ortho map by design.
- `shaders/deferred.comp.glsl:89` applies `sun_map_visibility()` to every shaded pixel, near
  and far alike.
- `SunShadowPass` rasterizes **all resident** LoD pages (`lod_system.cpp:261-268`), not the
  camera-visible subset, so occluders behind the camera still cast.
- `sun_shadow_map` defaults to `true` in the Low, Medium and High tiers.

The defect is a unit error at the comparison, `shaders/deferred.comp.glsl:37-38`:

```glsl
float bias = sun.params.x * (0.5 + 2.0 * slope) + 0.0015;
return (p.z + bias >= texture(sun_map, uv).r) ? 1.0 : 0.0;
```

`sun.params.x` is `SunOrtho::texel_world` — **world metres per texel** (`sun_ortho.cpp:85`).
`p.z` and the stored depth are **normalized `[0,1]`** across the light-space depth range. The
bias is never converted, so it is wrong by a factor of that range.

The demo's world is `world_size_regions = {64, 8, 64}` (`world_store.h:44`, not overridden in
`main.tscn`) times `kRegionBricks = 32` times `kBrickSize = 0.8 m` — a 1638.4 × 204.8 × 1638.4 m
AABB. Projected onto the light basis for `kSunDir`:

| World | light-space `max(w,h)` | `texel_world` | min bias (normalized) | depth range `d` | effective world bias |
|---|---:|---:|---:|---:|---:|
| demo `{64,8,64}` | 2197.9 m | 1.073 m | **0.537** | 1569.2 m | **~842 m** |
| test `{8,5,8}` | 292.7 m | 0.143 m | 0.072 | 274.6 m | ~20 m |

(`texel_world` is `max(w, h) / 2048`; the demo's maximum is `w`, the test world's is `h`.)

A normalized bias of 0.537 against depths that span `[0,1]` means `p.z + bias >= stored` holds
for effectively every pixel in the world. Every surface reports fully lit. The far field is flat.

The near field still looks shadowed because its raymarched term (`g0.a`) is independent of the
map and covers 7.68 m (`RAY_SHADOW_PENUMBRA_DIST`), which is the whole visible near range.

**`tests/test_sun_shadow.gd` is green against this bug and cannot catch it.** Its `make_world()`
builds an `{8,5,8}` world, 8× smaller on a side, where the same error yields ~20 m of bias while
the probe at `test_sun_shadow.gd:66` sits roughly 34 m below the surface along the light axis. It
passes with ~14 m of margin, by luck of scale. The bug's magnitude is proportional to world size.

## 2. Decided scope

Settled with the requester before this document was written:

- The sun must be **freely animatable at runtime** — a day/night sweep, not an editor-authored
  constant. This is what forces the rebuild-policy work in section 5.
- The light node drives **direction and colour × energy**. Nothing else.
- **Ambient is out of scope.** `DeferredPass::kAmbient` stays `{0.16, 0.19, 0.26}`.
- **`cel_object.gdshader` is out of scope.** Non-voxel meshes keep the baked sun direction.

Sections 7.1 and 7.2 record what those last two exclusions cost, so the decision is reviewable
rather than merely recorded.

## 3. Fix the bias

`sun_ortho()` already computes the light-space depth extent `d` at `sun_ortho.cpp:70` and
discards it after building row 2. Keep it:

```cpp
struct SunOrtho {
    float view_proj[16] = {};
    float texel_world = 0.0f;  // one shadow texel, in world metres
    float depth_range = 0.0f;  // light-space depth extent, in world metres
    bool valid = false;
};
```

`SunShadowPass` stores and exposes it beside `texel_world()`; `DeferredPass::render` takes it and
writes it to the `SunBlock` UBO's free `params.y` (`deferred_pass.cpp:196-197` already zeroes
`uf[17..19]`, so no layout change and no size change). The shader divides at the point of
comparison:

```glsl
float bias = (sun.params.x / max(sun.params.y, 1e-6)) * (0.5 + 2.0 * slope) + 0.0015;
```

`texel_world` keeps its world-metre meaning. It is a real quantity, it is what
`debug_sun_shadow_stats()` reports, and `test_sun_ortho.cpp` pins it. The error is in the
comparison, so the conversion belongs at the comparison and nowhere else.

This is the entire fix for the flat far field. No shadow sampling is added to `lod.frag.glsl`;
its existing comment describes the correct architecture.

## 4. The sun as a value

### 4.1 `ve::SunState`

```cpp
struct SunState {
    float dir[3]   = {kSunDir[0], kSunDir[1], kSunDir[2]}; // normalized, TOWARD the sun
    float right[3] = {};   // the light's basis X, world space; all-zero when unset
    float up[3]    = {};   // the light's basis Y, world space; all-zero when unset
    float rgb[3]   = {1, 1, 1}; // linear light_color * light_energy
};
```

`dir` points **toward** the sun, matching `kSunDir` and `sun_ortho`'s existing contract. A
`DirectionalLight3D` emits along its local `-Z`, so `dir` is `+basis.get_column(2)` normalized,
`right` is column 0 and `up` is column 1.

`ve::kSunDir` survives as the default. When `sun_light_path` is empty or unresolved, `SunState`
is exactly today's constant with a white sun and an all-zero basis, so every existing C++ test and
every headless path behaves as it does now with no edits.

The all-zero basis is the explicit signal for "no authored basis": callers select `sun_ortho()`'s
direction-only overload, which derives one as today. A zero basis is never passed to the
explicit-basis overload. Section 4.3 covers why the distinction matters.

### 4.2 Where it is read

`VoxelWorld` gains an exported `sun_light_path : NodePath`, resolved in `_process` in the same
shape as `physics_center_path_` at `voxel_world.cpp:293-299`:

```cpp
Node3D *light = Object::cast_to<Node3D>(get_node_or_null(sun_light_path_));
```

Main thread only. `_render_callback` never touches a scene node; it reads the cached `SunState`.
`DirectionalLight3D::get_color()` is sRGB as authored in the inspector, so it is converted to
linear before being multiplied by `get_param(PARAM_ENERGY)`. Energy above 1 is allowed through —
the demo's additive bloom (commit `06acdd8`) is the intended consumer of the overrange.

### 4.3 The light's own basis, not a derived one

`sun_ortho()` currently derives its basis from `cross(l, worldUp)` with a fallback when the
result is shorter than `1e-4` (`sun_ortho.cpp:39-50`). That guard prevents a division by zero.
It does not prevent the real failure, which is conditioning rather than degeneracy: near the
zenith a small change in the sun's azimuth swings the derived basis through a large rotation, so
an animated sun crossing overhead makes the shadow map spin about the light axis.

This is not hypothetical for this scene. The demo light's direction toward the sun is
`(-0.0697, 0.9962, -0.0523)` — **84.99° elevation**, five degrees off vertical, where
`cross(l, up)` has length 0.087. Above the guard, badly conditioned.

`sun_ortho()` therefore gains an overload taking an explicit `right`/`up` basis. The node's basis
is orthonormal and rotates continuously with the node, so it has no degenerate case and no snap.
The existing direction-only signature stays and keeps deriving the basis exactly as today, so
`extension/tests/test_sun_ortho.cpp` needs no changes.

### 4.4 Plumbing into the shaders

`const vec3 SUN_DIR` is removed from `shade.glslh`. A new `shaders/sun_light.glslh` declares a
UBO included with a set/binding `#define`, the idiom `beauty_camera.glslh` already establishes:

```glsl
layout(set = SUN_LIGHT_SET, binding = SUN_LIGHT_BINDING, std140) uniform SunLight {
    vec4 dir; // xyz = toward the sun, w unused
    vec4 rgb; // xyz = linear colour * energy, w unused
} sun_light;
```

One buffer, owned by the orchestrator, updated once per frame, bound into three passes:

| Pass | Needs | Binding note |
|---|---|---|
| `raymarch.comp.glsl` | `dir` | Push constant is 5 `vec4` + 3 `ivec4` = **exactly 128 bytes**, the guaranteed minimum. No room. Existing bindings run to 23; the UBO takes **24**. |
| `contact_shadow.comp.glsl` | `dir` | Disabled in every tier; changed only so it stays correct. |
| `deferred.comp.glsl` | `dir`, `rgb` | Separate from its existing `SunBlock`, which carries the shadow projection. Two concerns, two blocks. |

`lod.frag.glsl` needs neither and is not touched. `cel.gdshaderinc` keeps `VE_SUN_DIR`.

## 5. Rebuild policy for a moving sun

`SunShadowPass::build` compares the incoming `ortho.view_proj` against the last-built
`view_proj_`. A difference bypasses both `dirty_` and the `kMinFrames = 12` throttle; everything
else keeps today's lazy behaviour.

Three lines, no new parameter, and the policy stays inside the pass that owns it.
`test_the_matrix_does_not_move_with_the_camera` continues to pass unchanged, because camera
motion genuinely does not change the matrix — that test asserts the property that remains true.

The cost is affordable and the existing numbers say so directly. `GpuTimings::cancel()` erases
the pending sample (`gpu_timings.cpp:123-127`) and `raymarch_compositor.cpp:257` cancels on every
frame the map does not rebuild, so PORTFOLIO.md's `GPU shadows` row — **p50 0.124 ms, p99
0.274 ms against a 1.0 ms budget** — is the cost of an actual rebuild, not an average smeared
over twelve frames. A rebuild every frame fits with roughly 4× headroom.

**Texel snapping is deliberately not implemented,** despite `sun_ortho.h` listing it as work a
moving sun would force. The world AABB is fixed, so a stationary sun already produces a
bit-identical matrix frame to frame and crawls not at all; a moving sun's shadow motion is real
motion, not shimmer. Snapping would buy nothing here. It stays available if a future change makes
the fitted volume camera-relative.

Shadow resolution is unchanged at ~1.07 m per texel over the demo world. Coarse, and no coarser
than before. Out of scope.

## 6. Sun colour in the cel ramp

`cel_shade` gains a sun RGB argument. The C++ side takes it as a new `CelInput` field defaulting
to `{1,1,1}`, so no C++ call site changes signature:

```glsl
return tint * lit * sun + tint * ambient * ao + sun * spec + vec3(rim);
```

Direct light and its specular highlight take the sun's colour. Ambient keeps its own colour, and
the rim term stays neutral because it is a stylization of silhouette, not a light. A white sun
reproduces today's output exactly, which is what makes the existing parity tests meaningful as
regression cover rather than something to be re-baselined.

Three mirrors move together: `shaders/shade.glslh`, `extension/src/shade/cel.h` + `cel.cpp`, and
`shaders/cel.gdshaderinc`. In the `.gdshaderinc`, the existing 8-argument `ve_cel_shade` is kept
as an overload forwarding `vec3(1.0)`, so `cel_object.gdshader` needs no edit at all.

## 7. Accepted consequences

### 7.1 The demo will look flatter until the light is re-authored

`kSunDir` is at 50.0° elevation. The scene's `DirectionalLight3D` is at 84.99°, nearly overhead.
Wiring the node up moves the sun 35° and will visibly flatten the terrain's read and shorten every
shadow. This is correct behaviour reporting a fact about the scene, but it will look like a
regression on first run. Re-authoring the light's rotation is a scene edit, not a code change.

### 7.2 Ambient and non-voxel meshes stay on the old constants

Excluded in section 2, restated here as cost:

- A dusk sun over a fixed noon ambient (`{0.16, 0.19, 0.26}`) will read wrong at low sun angles.
- `cel_object.gdshader` keeps `VE_SUN_DIR` baked at 50°, so the demo's `TestCube` and any other
  non-voxel mesh will be lit from a visibly different direction than the terrain beside it.

Both are small follow-ups; neither blocks this work.

## 8. Testing and acceptance

Test-first throughout. The first test written is the one whose absence allowed the bias bug to
ship.

**C++ (`extension/tests/`), no GPU required:**

1. `test_sun_ortho.cpp` — `depth_range` equals the light-space depth extent, for a world whose
   extent is known analytically.
2. `test_sun_ortho.cpp` — **the missing unit pin.** At the demo's 1638.4 × 204.8 × 1638.4 m AABB,
   `texel_world / depth_range` is below a bound that makes the shadow test meaningful. This fails
   against today's code and is the regression gate for section 3.
3. `test_sun_ortho.cpp` — the explicit-basis overload returns a matrix that varies continuously as
   the sun sweeps through the zenith, where the derived-basis overload does not.
4. `test_shade_cel.cpp` — CPU/GLSL parity extended to the sun-colour argument, plus an assertion
   that a white sun reproduces the current output.

**GDScript (`tests/test_sun_shadow.gd`), GPU:**

5. A probe at a world size where the pre-fix bias provably reports lit, asserting shadowed. Sized
   during implementation to fail against today's code while still settling inside the suite's
   existing budget — `{8,5,8}` is demonstrably too small.
6. Moving the sun changes the matrix and triggers a rebuild on the very next `build()` call,
   bypassing `kMinFrames`.
7. With the sun moved to the opposite side of an occluder, a previously lit probe point becomes
   shadowed. This is the end-to-end assertion that the node actually drives the image.

**Baseline first.** Seven gdunit suites fail on a clean `main`. Capture that baseline before
attributing any failure to this work.

## 9. Implementation boundaries

Touched: `shade/sun_ortho.{h,cpp}`, `shade/cel.{h,cpp}`, `render/sun_shadow_pass.{h,cpp}`, `render/deferred_pass.{h,cpp}`,
`render/orchestrator.{h,cpp}`, `render/raymarch_pass.*`, `render/contact_shadow_pass.*`,
`voxel_world.{h,cpp}`, `raymarch_compositor.cpp`, `debug/hooks.cpp`, `shaders/shade.glslh`,
`shaders/sun_light.glslh` (new), `shaders/raymarch.comp.glsl`,
`shaders/contact_shadow.comp.glsl`, `shaders/deferred.comp.glsl`, `shaders/cel.gdshaderinc`,
`demo/main.tscn` (set `sun_light_path`).

Not touched: `shaders/lod.frag.glsl`, `shaders/lod_shadow.*`, `shaders/cel_object.gdshader`,
`DeferredPass::kAmbient`, LoD geometry, streaming, physics, materials.
