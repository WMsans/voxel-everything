# Sun rays

Date: 2026-10-23.

## Why

When you look toward the sun through trees or past a cliff edge, there should be shafts of light
fanning out from it: bright spokes through the gaps, dark wedges behind trunks and branches. The
user's reference is a Minecraft shader-pack sunset (https://i.redd.it/3xpvwe50vved1.jpeg). Only
the rays in that image are in scope. Its clouds, water, fog and grading are not.

The user asked for the **screen-space** technique from the Kodeco article ("Volumetric Light
Scattering as a Custom Renderer Feature in URP"), not a volumetric one. Start from a mask of the
pixels that can see the sky, then smear it outward from the sun's position on screen.

The user also wants it **standalone**. It must not be coupled to the voxel engine, and copying its
folder into another Godot project must be enough to make it work there.

## Goals

1. Rays radiate from the sun's position on screen whenever the sun is on screen or near its
   edge, and something partly blocks it. Gaps between leaves and branches produce distinct,
   crisp streaks, not a soft glow.
2. Only the sun and the sky right around it emit. Sky far from the sun contributes nothing, so
   looking at open sky away from the sun gives no haze.
3. The rays take the sun light's colour (orange at sunset) and are added on top of everything,
   foliage included, before glow and tonemapping.
4. **Cheap by construction** (user's words: "if you need to optimize it, you are doing it
   wrong"). The method should be cheap in itself: two small dispatches, no mask pass, no blur
   chain, no adaptive tricks. If the measured cost isn't small, the design is wrong. Don't tune
   it after the fact.
5. Players can adjust it in the F1 settings menu.

## Non-goals

- Drawing a sun disc. The rays alone give the feeling of the sun (user's call).
- Real volumetric light, fog, or shafts from the sun's light when it's behind the camera.
- Rays from other lights, including emissive voxels.
- Clouds, water reflections, or any other part of the reference image.

## Anti-goals

- **Coupling.** The effect must not reference VoxelWorld, VoxelSettings, the engine's settings
  tables, its shaders or its C++. Code in the effect's folder must have no `voxel` in it. See
  memory "keep new features out of the beauty stack". The deferred/beauty stack stays untouched.
- Blurry, mushy rays that read as bloom.

## Constraints (from the code)

- Godot **4.7**, Forward+. Render scale is 0.5 in `project.godot`, so work at
  `RenderSceneBuffersRD.get_internal_size()`, not the window size.
- **Depth is the occlusion source.** The near-field raymarcher (`composite.frag.glsl`) and the
  far-field LoD raster both write Godot's scene depth. Grass and ordinary meshes do too. The sky
  is left at the reverse-Z far plane, **depth == 0**. "This pixel sees sky" is therefore
  `depth == 0` with no engine knowledge, and the same test holds in any Forward+ project.
  Reflections of the sky in water are not depth 0, so they correctly emit nothing.
- The environment uses `background_mode = 4` (Keep). The sky colour comes from the voxel
  renderer, not a Godot Sky. The effect must not read sky colour from the image. It uses the
  light's colour instead, so it works the same whatever draws the sky.
- The world scene already holds a `Compositor` with two effects (`RaymarchCompositor`,
  `BeautyCompositor`) in `demo/scenes/voxel_world.tscn`.

## Decided design

### Where it lives

`addons/sun_rays/sun_rays.gd`: one `@tool` GDScript `CompositorEffect` (`class_name SunRays`).
Its two compute shaders are GLSL source strings inside it, compiled with
`RenderingDevice.shader_compile_spirv_from_source`. That way a copied folder doesn't wait on the
editor's `.glsl` import step. It has no plugin.cfg and no autoload. It's enough to drop the folder
in and add `SunRays` to any WorldEnvironment's compositor. *(Folder, class name and embedded
shaders are my call.)*

### How it renders (my call; the technique itself is the user's)

The callback is `EFFECT_CALLBACK_TYPE_POST_TRANSPARENT`. The colour buffer is still linear HDR
there, before glow, so glow softens the rays naturally. All of this runs after both voxel
effects. *(My call.)*

1. **Rays pass, low resolution.** One compute dispatch at a fraction of the internal size, writing
   a small single-channel or RGB texture. Each texel walks a fixed number of steps from itself
   toward the sun's screen position. At each step it reads scene depth and accumulates:
   `sky(depth == 0) × sun_weight(distance from sample to sun on screen) × decay^i`.
   `sun_weight` is the falloff that keeps emission to the sun's neighbourhood (goal 2). The mask
   is computed inline from depth, so there's no separate mask texture.
   Each step reads depth with one `textureGather` and counts as sky only if all four taps
   are 0. The far field has sub-pixel cracks at LoD level boundaries that hold depth 0
   (memory "LoD level-transition hairline holes"). With a low sun they sit right next to it and
   would otherwise throw bright streaks out of solid ground. One gather costs the same as one
   fetch, so this doesn't break goal 4. *(My call.)*
2. **Composite pass, full resolution.** One compute dispatch adds `rays × sun_colour × intensity`
   into the scene colour, with bilinear upsampling of the rays texture.

Both passes are skipped when the effect is disabled, when the sun is behind the camera, or when
its screen position is far enough off screen that no rays can reach the view.

The sun's screen position comes from the light's direction, projected with the callback's
`RenderSceneData` camera transform and projection.

### Finding the sun (my call)

`CompositorEffect` is a Resource and runs its callback on the render thread, so it can't safely
walk the scene tree there. On the main thread, the effect hooks
`RenderingServer.frame_pre_draw`. It finds the first visible `DirectionalLight3D` in the scene
tree and caches its direction and `light_color × light_energy` in plain member variables. It
re-searches when the cached light is freed or hidden. The render callback reads only those
cached values.
*Ceiling:* a project with several directional lights gets the first one. Add an exported pick
(a group name) only when that happens.

### Knobs (my guess; the user approved the list)

These are exported properties with `@export_range` hints:

- `enabled` (on by default)
- `intensity`
- `ray_length` (how far the rays reach across the screen)
- `sun_size` (how tightly emission hugs the sun)

Colour isn't a knob, because it comes from the light. Sample count and resolution divisor are
constants, not knobs: they're the cost and the look, and should be chosen once.

### This project's hook

- `demo/scenes/voxel_world.tscn` gets a third compositor effect, `SunRays`. Nothing more is
  needed for per-instance copies, because `voxel_world_scene.gd` `_enter_tree` already
  duplicates every effect and sets `world_path` only where it exists.
- `tests/test_world_scene.gd` (both compositor tests) and `tests/test_demo_scene_wiring.gd`
  currently assert exactly two effects, each with a `world_path`. Change them to "the voxel
  effects point at the world" (filter by `world_path` existing) and add an assertion that one
  `SunRays` is present.
- **Settings menu.** `demo/scripts/settings_menu.gd` builds all its tabs from `VoxelSettings`
  tables. Sun rays must not enter those tables (anti-goal). Instead the menu adds one
  "Sun rays" tab, built by reading the `SunRays` effect's exported properties and their
  `@export_range` hints off `get_property_list()`. That way the menu still holds no hand-written
  table, and a knob added later shows up by itself. The menu finds the effect through a new
  `environment_path` export pointing at a WorldEnvironment. `main.tscn` sets it to
  `/root/Main/VoxelWorld/WorldEnvironment`, the same absolute-path pattern as `settings_path`.
  (Godot 4.7's `World3D` has no compositor getter.) With no path, there's no tab. Values persist in a `[sun_rays]` section of `user://settings.cfg`. VoxelSettings
  already keeps sections it doesn't own (`voxel_settings.cpp` save path), so they coexist. Reset
  restores the effect's default property values. *(All my call.)*

### The shipped sun stays as it is (user's call)

The demo's `Sun` sits at about 85° elevation and is plain white, and nothing moves or tints it at
runtime. The user chose to leave it alone. The rays are built and tuned against a low, orange
sun that's set **only inside a capture script**. In the shipped game the rays will mostly show
when looking up through a canopy. Changing the game's sun, or adding time of day, is a separate
decision.

## How we'll know it works

- **Look.** A new capture script (`tools/sun_rays_capture.gd`, modelled on the existing
  `tools/*_capture.gd`) lowers the Sun to about 10–15° and tints it orange, but only for the
  capture. It then shoots the sun just behind a tree line. The result shows distinct spokes and
  dark wedges, like the reference. Memory: trees are rare. Stream the far grove at
  (65, 62, −930) and aim at the sun. The same view with the effect off shows no rays. Open sky
  away from the sun shows no haze.
- **Portability.** Copy `addons/sun_rays/` into a fresh Godot 4.7 project with a cube, a
  DirectionalLight3D, a procedural sky and a camera looking past the cube at the sun. Rays appear
  with nothing else added except the effect in the compositor.
- **Cost.** Use frame time at the steady benchmark pose and a sun-facing pose, effect on vs. off,
  interleaved A/B/A. GPU timestamps are invalid on this machine (memory). Report the number. It
  should be at the noise floor (~0.4 ms frame-time steps). If it clearly isn't, treat that as a
  design error (goal 4).
- Golden-frame tests render through debug hooks, not the compositor, so they shouldn't change.
  Confirm that rather than assume it.

## Left to the builder

- Step count, resolution divisor, the exact `sun_weight` and decay curves, and the default knob
  values. Pick them by looking at the far-grove capture against the reference, within goal 4.
- Whether the rays pass uses a sampler (bilinear depth taps) or `texelFetch`.
- How to fade the rays as the sun nears and leaves the screen edge, so they don't pop.
- Test shape for the menu tab and the effect's skip conditions. The repo uses gdUnit
  (`./gdunit_tests.sh`). Memory says some suites already fail on clean main, so compare against a
  stash before blaming the change.

## Measured

2026-10-23, Apple M1, 2560×1440 window (render scale 0.5, so 1280×720 internal).

**Capture.** `godot --path . --resolution 2560x1440 -s res://tools/sun_rays_capture.gd -- --out=/tmp/sun-rays`
sets the far-grove camera (65, 62, −930), the sun 3° up along heading (−33, −35) and tinted
orange, and the camera looking straight at it. `on.png` shows a fan of crisp spokes from behind
the crowns, with dark wedges behind trunks and leaves. `off.png` has none. `away.png` (facing
away) has no rays. With `--shipped-sun --at=20,62,30`, the camera looks up at the game's own sun
(white, about 85° up) through open sky, and on.png has a large bright glow around the sun.

**Shipped values.** `intensity` 1.5, `ray_length` 0.9, `sun_size` 0.08, `DIVISOR` 2, `STEPS` 64.

**How the march samples, as built.** A texel's visibility is the *fraction* of its line to the
sun, inside `sun_size`, that is sky. It is weighted by `(1 − t/sun_size)²` and multiplied by
`(1 − dist/ray_length)²`. All `STEPS` samples sit on that emitting stretch, never along the
whole line. Spread along the whole line, a far texel put only a handful of samples on the
emitting stretch, and neighbours quantized into visible blocks. Samples off screen are unknown
and count as neither sky nor blocked. There is no `decay^i` term, because it would dim the
samples nearest the sun, which are the ones that emit.

**Rays always draw (user's call).** Through open sky the march says "fully visible" everywhere,
so looking at the sun floods the view around it with light. An earlier gate that drew rays only
when something partly blocked the sun was removed: with the game's sun nearly overhead, nothing
ever blocked it and rays never showed in play.

**Cost.** Frame-time p50 at the capture pose, 300 frames per leg, interleaved: on 11.89 ms,
off 11.42 ms, on 11.80 ms. That's about 0.4 ms with the sun on screen, roughly one frame-time
step on this machine. There's no cost when the sun is off screen, behind the camera, or the
effect is off, because the callback returns before any dispatch. GPU timestamps are invalid
here, so this is CPU+GPU frame time.

**Portability.** `addons/sun_rays/` copied alone into a fresh Godot 4.7 Forward+ project (cubes,
a procedural sky, a DirectionalLight3D, a camera) renders rays with the effect on and none with it
off. No errors.

**Water and ice let the rays through (fixed after merge).** The G-buffer resolve gives a
transparent front its own depth, so `depth == 0` read water and ice as walls and a front over the
sun killed every ray. The effect now also reads an optional R32F `sun_rays/clear_depth` scene
buffer texture: the depth of a see-through surface with only sky behind it, 0 elsewhere. A step is
sky when its depth is 0 or equals that. The voxel frame clears it each frame and the transparency
composite fills it (it alone sees the opaque depth before the resolve and the front together).
A Godot mesh in front of the ice has a different depth, so it still blocks. Without the texture
(any other project) nothing changes. Check: `--block=ice` / `--block=water` on the capture
(SUN_RAYS_GAIN 0.000 → 0.08 / 0.10; open sky 0.07). Cost, interleaved old/new/old/new at the
capture pose, 2560×1440: on−off 0.75 ms old, 0.99 ms new, about +0.25 ms for the second gather.
