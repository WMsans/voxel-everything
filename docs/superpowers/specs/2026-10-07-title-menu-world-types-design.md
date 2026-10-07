# Voxel Everything — Title Menu, World Types and the Reusable World Scene

**Date:** 2026-10-07
**Status:** design approved; plan not yet written
**Start commit:** `495f5dd` (`main`)
**Prior specs:** `2026-09-03-terrain-pipeline-design.md` (pipelines, the generated
`field.glslh`); `2026-09-17-stage-authoring-design.md` (stage manifests, the Lipschitz rule,
named CPU mirrors); `2026-09-18-trees-design.md` (the leaf pass that places trees on its own).

A Minecraft-style front end: a title screen with one button, **Create New World**, leading to a
Create New World screen with exactly two options, **Seed** and **World Type**. Underneath, the
demo is reorganised so a world is one scene that can be instanced anywhere, and world types are
data, not code.

---

## 1. Goals and non-goals

Goals:

- A title screen that is the project's main scene, with one button: Create New World.
- A Create New World screen with a Seed text field and a World Type button that cycles types.
- Three world types: **Default**, **Mesas**, **Flat**. Adding a type is one `.tres` file.
- A seed that really changes the terrain, and agrees between GPU, CPU and the leaf pass.
- `demo/main.tscn` split so the world (terrain, environment, compositors, sun) is one scene
  that works under any parent, with no `/root/Main` dependency. Importing it into another
  project means copying the engine as today and instancing that scene.
- `demo/` reorganised into `scenes/`, `scripts/` and `world_types/`.
- Running `demo/scenes/main.tscn` directly behaves exactly as `demo/main.tscn` does today, so
  benchmarks, captures, tools and goldens do not move.

Non-goals (follow-ups, §9):

- Returning to the title from inside a world.
- Saving or loading worlds; a world name field.
- Packaging the engine as an `addons/` folder (moving binaries, `shaders/`, `assets/` and the
  C++ `res://` path constants).
- Per-stage seeded randomness (phases, frequencies). The seed shifts the domain only.

## 2. What the code does today (as of `495f5dd`)

- **A world type already exists underneath.** A `.pipeline` file under `assets/pipelines/` is a
  whole terrain; `VoxelWorld.terrain_pipeline_path` picks one. The load is first-wins and
  fresh-init-only (`VoxelWorld::load_terrain_pipeline`, `voxel_world.cpp:600`), so a type is
  chosen before the world initialises, never changed on a live world.
- **The pipeline `seed` key is dead.** `pipeline.cpp:42` parses it and `pipeline.cpp:275` feeds
  it to the hash; no stage reads it. The terrain is sums of sines with fixed phases.
- **`main.tscn` is pinned to `/root/Main`.** Both compositors' `world_path`, the HUD, the edit
  tool and the material picker use absolute paths. `test_demo_shell.gd` documents that the
  scene only resolves when it sits at exactly `/root/Main`.
- **The leaf pass places trees independently.** `leaf_trees.comp.glsl` iterates world-space tree
  cells and calls `trees_ground_h(xz)` from the generated field source; it does not go through
  `eval_base_field`.
- **`VoxelWorld.raycast` is analytic.** It queries the CPU field (`FieldView::raycast`), so it
  answers for terrain that has not streamed; before the pipeline loads it returns no hit.
- **Shipped pipelines:** `default` (hills, relief, cave, height_bands, trees), `mesas`,
  `trees` (now identical in stages to `default`), `golden` (frozen test fixture).

## 3. Layout

```
demo/
├─ scenes/        main.tscn, voxel_world.tscn, title.tscn, create_world.tscn,
│                 settings_menu.tscn, material_picker.tscn
├─ scripts/       benchmark.gd, capture.gd, dev_tools.gd, edit_tool.gd, help.gd, hud.gd,
│                 material_picker.gd, player.gd, settings_menu.gd
│                 + voxel_world_scene.gd, world_type.gd, title.gd, create_world.gd,
│                   ground_spawn.gd
├─ world_types/   default.tres, mesas.tres, flat.tres
└─ menu_theme.tres
```

All existing demo files move. The move is its own commit with no behaviour change: `git mv`
(with `.uid` sidecars) plus a rewrite of every `res://demo/...` and `demo/main.tscn` reference —
about 35 references in 21 files across `tools/`, `tests/`, `tools/run_benchmarks.sh`, scenes
and `README.md`. Scenes also reference scripts by `uid://`, so the editor resolves them either
way.

**The importable set** is `scenes/voxel_world.tscn`, `scripts/voxel_world_scene.gd`,
`scripts/world_type.gd`, plus any `world_types/*.tres`. None of them references another demo
file.

## 4. The world scene

### 4.1 `scenes/voxel_world.tscn`

```
VoxelWorld            (VoxelWorld, script: voxel_world_scene.gd)
├─ VoxelSettings      world_path = ".."
├─ WorldEnvironment   Environment + Compositor[RaymarchCompositor, BeautyCompositor]
└─ Sun                DirectionalLight3D
```

The Environment, both compositors and the sun's transform move verbatim from `main.tscn`.
The Compositor and its effects are `resource_local_to_scene = true`, so two instances never
share a `world_path`.

### 4.2 `scripts/voxel_world_scene.gd` (`extends VoxelWorld`)

- `@export var world_type: WorldType` — its setter copies `world_type.pipeline_path` into
  `terrain_pipeline_path`. Exported values are applied during instantiation, before the node
  enters the tree, which is before the first-init pipeline load.
- `@export var world_seed: int` — set straight through to the C++ property of the same name
  (§5.1). Declared in C++, so the script does not redeclare it; the inspector shows it.
- `_enter_tree()`: sets both compositor effects' `world_path` to `get_path()` and
  `sun_light_path` to the `Sun` child. This replaces every hardcoded `/root/Main/VoxelWorld`
  in the world scene.
- `ground_at(x: float, z: float) -> Variant`: one `raycast(Vector3(x, 600, z), Vector3.DOWN,
  1200)`; returns the hit position, or `null` when `is_initialized()` is false or nothing is
  hit. 600 m clears the tallest shipped terrain (`SURFACE_Y` 51.2 + relief 310 + hills 10).

`physics_center_path` is not set by the world scene; the instancing scene sets it on the
instance, because only it knows where its player is.

### 4.3 `scripts/world_type.gd`

```gdscript
class_name WorldType extends Resource
@export var display_name := ""
@export_file("*.pipeline") var pipeline_path := ""
```

`world_types/default.tres` → `res://assets/pipelines/default.pipeline`, `mesas.tres` →
`mesas.pipeline`, `flat.tres` → `flat.pipeline`. File names sort Default first; the menu
sorts by file name (§6.2). To add one, add a `.tres` file.

### 4.4 `scenes/main.tscn`

Instances `voxel_world.tscn` as a child named **`VoxelWorld`** with `world_type =
default.tres`, `world_seed = 0`, `near_field_scale = 0.4`, `physics_center_path =
/root/Main/Player`, `process_mode = 3` — the values the node carries today. Player, HUD,
EditTool, DevTools, Benchmark, Capture and TestCube are unchanged. Every existing
`/root/Main/VoxelWorld` path still resolves. Two paths move:

| Old | New |
|---|---|
| `/root/Main/VoxelSettings` | `/root/Main/VoxelWorld/VoxelSettings` |
| `/root/Main/DirectionalLight3D` | `/root/Main/VoxelWorld/Sun` |

Their users (the HUD's `SettingsMenu`, `tools/leaf_capture.gd`, `tools/grass_capture.gd`,
`tests/test_cel_object.gd`, `tests/test_deferred.gd`, `tests/test_sun_shadow.gd`) are updated
in the split commit.

## 5. Seed in the engine

### 5.1 The offset

- `ve::seed_offset(uint32_t seed)` in `extension/src/terrain/`, pure C++ (native suite):
  seed `0` → `(0, 0)`; any other seed → two values from a splitmix64 hash of the seed, each a
  **whole metre** in `[-8192, 8192)`. Whole metres are exact in float; ±8 km keeps a
  position's float resolution under 1 mm against 5 cm voxels, so the normal's 5 cm finite
  differences do not facet. (Past ~100 km they would.)
- `PipelineDesc::seed` stays; the **`seed` key is removed** from the parser and from all four
  `.pipeline` files. It never had an effect, and a file seed alongside a world seed would be two
  seeds that disagree. Removing a dead header line from the frozen `golden.pipeline` does not
  change its stages; the golden corpora prove it (§8, commit 4).
- `VoxelWorld.world_seed` (int, default 0, bound property). `load_terrain_pipeline` copies it
  into the desc before resolve. Like the pipeline path, it takes effect on fresh init only.
  The existing `hash_feed(desc.seed)` makes the shader cache key follow the seed.
- `ResolvedPipeline` carries the seed through to codegen and the CPU generator.

### 5.2 Where the offset is applied

Once per side, at the single point where a sample position enters the field:

- **GPU.** `generate_field_glslh` emits `const vec3 VE_FIELD_OFFSET = vec3(ox, 0.0, oz);` and
  `ctx.p = p + VE_FIELD_OFFSET;` in `eval_base_field`. Raymarch, LoD meshing, grass and every
  edit-over-field consumer go through it.
- **CPU.** `PipelineFieldGenerator::sample` adds the same offset before running the stages.
  Raycast, colliders, islands and consolidation go through it.
- **Edits stay in world space.** They compose over the base field, so a crater lands where it
  was clicked at any seed.
- **Leaf pass.** `leaf_trees.comp.glsl` works in shifted space for placement and returns to
  world space for everything that touches the world: the cell window is computed from
  `camera + offset` (CPU side, `LeafScatterPass`), `tree_at` runs on shifted cells, and
  `t.crown` and `t.base` are shifted back by `-VE_FIELD_OFFSET` before the distance and frustum
  cull, the atlas trunk check and the `tree_list` write. `leaf_scatter.comp.glsl` consumes only
  world-space records and does not change.
- **Debug hooks.** `hooks_render.cpp` re-implements parts of the render path. Any place in it
  that computes field positions or tree cells on its own gets the same treatment, so hook-based
  tests do not test a path that does not ship.

At seed 0 the offset is zero, so everything above is a no-op for the demo, the benchmark and
every golden except the generated-source golden, which gains the two offset lines.

### 5.3 The Flat type

- `shaders/stages/flat.field.glslh`: `//!out sdf : float`, `//!out height : float`,
  `//!param height : float = 2.0`, `//!lipschitz add 1.0`, `//!cpu ve::stage_flat`;
  body `ctx.height = P.flat_height; ctx.sdf = ctx.p.y - SURFACE_Y - ctx.height;`.
- CPU mirror `stage_flat` registered in `builtin_stages.cpp` with `VE_STAGE_SLOTS` /
  `VE_STAGE_PARAMS`.
- `assets/pipelines/flat.pipeline`: `lipschitz 1.0`, then `flat`, then `height_bands`. A height
  of 2 m falls in `height_bands`' grass band (1–4 m). No trees: the trees stage `//!use`s hills
  and relief params. The seed has no visible effect on Flat, as in Minecraft.

## 6. Title and Create New World

### 6.1 Look

Minecraft's classic menu style built from stock controls: a full-screen `TextureRect` tiling
`assets/materials/00_basecolor.png` (or whichever ground albedo reads best; chosen in the plan)
with a dark `modulate`; centred wide grey `Button`s; white labels with a shadow. One
`menu_theme.tres` sets font sizes and button minimum size for the 2560×1440 canvas-items
stretch. No new fonts or image assets.

### 6.2 `scenes/title.tscn` + `scripts/title.gd`

```
        VOXEL EVERYTHING
     ┌───────────────────────┐
     │   Create New World    │
     └───────────────────────┘
```

The button calls `get_tree().change_scene_to_file("res://demo/scenes/create_world.tscn")`.
Shows the mouse cursor on `_ready` (the player captures it later).

### 6.3 `scenes/create_world.tscn` + `scripts/create_world.gd`

```
         Create New World

   Seed for the World Generator
   ┌──────────────────────────────┐
   │ Leave blank for a random seed│
   └──────────────────────────────┘
     ┌──────────────────────────┐
     │   World Type: Default    │
     └──────────────────────────┘

   ┌──────────────────┐ ┌──────────────────┐
   │ Create New World │ │      Cancel      │
   └──────────────────┘ └──────────────────┘
```

- **Seed** (`static func parse_seed(text: String) -> int`, result masked to 32 bits): empty
  (after `strip_edges`) → `randi_range(1, 0x7fffffff)`; `text.is_valid_int()` → `text.to_int()`;
  otherwise `text.hash()`. `"0"` gives the original demo terrain.
- **World Type** button: label `World Type: <display_name>`; each press advances to the next
  type, wrapping. Types are `world_types/*.tres` loaded with `DirAccess`, sorted by file name.
  An empty folder disables World Type and Create.
- **Cancel**, and Esc (`ui_cancel`), return to the title. Enter in the seed field
  (`text_submitted`) presses Create.
- **Create:**
  1. `var main := load("res://demo/scenes/main.tscn").instantiate()`.
  2. Set `world_type` and `world_seed` on `main.get_node("VoxelWorld")` — before the instance
     enters the tree, so before the first-init load.
  3. Add a `GroundSpawn` node (`scripts/ground_spawn.gd`) to `main`.
  4. `get_tree().change_scene_to_node(main)`.

### 6.4 `scripts/ground_spawn.gd`

About 15 lines. Each `_process`, ask `../VoxelWorld.ground_at(player.x, player.z)`; on a hit,
move `../Player` to the hit plus 2 m up (the player starts in FLY mode, so it hovers), then
`queue_free()`. After 10 s without a hit, `push_warning` that the world did not initialise and
free itself, leaving the player at the scene's spawn. Only the title flow adds this node, so
`main.tscn` run directly keeps its `(8, 62, 8)` spawn.

### 6.5 HUD

`hud.gd` gains one line, `seed <n> · <display_name>`, read from the world node. A blank-seed
world is otherwise not reproducible; Minecraft shows the seed in F3 for the same reason.

### 6.6 Failure handling

| Failure | Behaviour |
|---|---|
| A type's pipeline fails to load or resolve | Existing: `VoxelWorld` reports `terrain pipeline: …` and does not initialise. `GroundSpawn` times out and warns, so a missing world reads as an error, not a hang. |
| `world_types/` empty or unreadable | World Type and Create disabled. |
| A `.tres` that is not a `WorldType` | Skipped with `push_warning` naming the file. |

## 7. Project

- `project.godot`: `run/main_scene = "res://demo/scenes/title.tscn"`.
- `README.md` "Getting started": run the project for the title menu; `demo/scenes/main.tscn`
  remains the direct entry used by benchmarks; a three-line "use the world in your project"
  note naming the importable set (§3).

## 8. Testing and commit order

**Baseline first.** Record the gdUnit failure set on clean `495f5dd` (the set drifts; an older
record is stale by default) and run the native suite. Every commit below is judged against it.

1. **Characterization test** (`tests/test_demo_scene_wiring.gd`) pinning today's `main.tscn`:
   both compositors' `world_path`, `sun_light_path`, `VoxelSettings.world_path`, the HUD, edit
   tool and picker paths all resolve; glow is enabled; Player is at `(8, 62, 8)`. Green before
   any change; must stay green after each.
2. **`chore:` move** demo into `scenes/` / `scripts/`. No behaviour change; test 1 and the
   suite prove it.
3. **Split `voxel_world.tscn`**, `voxel_world_scene.gd`, `WorldType`, `default.tres` and
   `mesas.tres` (`flat.tres` lands with its pipeline in commit 6). New
   test: instance `voxel_world.tscn` under an arbitrary parent (not `/root/Main`) and assert
   both compositors' `world_path` equal that instance's path and `sun_light_path` resolves.
4. **Remove the pipeline `seed` key.** `test_pipeline_load.cpp:33` (`seed 7`) is updated; the
   golden field and brick corpora must not change.
5. **Seed offset.**
   - `extension/tests/test_seed_offset.cpp`: 0 → (0, 0); deterministic; whole metres; within
     ±8192; a sample of distinct seeds gives distinct offsets.
   - `shaders/generated/field.glslh.golden` regenerated; the diff is exactly the offset
     constant and `ctx.p = p + VE_FIELD_OFFSET`, shown to the user before regenerating.
   - `tests/test_field_diff.gd` gains one world: `default.pipeline` at a non-zero seed, GPU
     against CPU — the proof both sides apply the same offset.
   - Leaf agreement: at a non-zero seed, every tree record the shipping leaf pass emits has
     bark under `base + up · height · 0.33` according to the CPU field. The plan names the
     hook path and confirms it drives the shipping pass, not a `hooks_render.cpp` rebuild.
6. **Flat stage, pipeline and `flat.tres`.** `test_field_diff.gd` enumerates
   `assets/pipelines/` and picks it up (GPU against CPU); `test_lipschitz_sampled.cpp` gains a
   `flat.pipeline` case.
7. **Title, Create New World, GroundSpawn, HUD line.** gdUnit:
   - `parse_seed`: `""` → non-zero; `"42"` → 42; `"-1"` → `0xffffffff`; `"hello"` →
     `"hello".hash() & 0xffffffff`; `"0"` → 0.
   - Type discovery returns Default, Mesas, Flat in that order.
   - Create builds a `main.tscn` instance whose `VoxelWorld` has the chosen
     `terrain_pipeline_path` and `world_seed`, inspected before it enters the tree (no GPU).
8. **`project.godot` main scene and README.**

**Manual check at the end:** launch the project; create Mesas with seed `hello`, then Flat,
then Default with a blank seed (restarting between, §9). Screenshot each and confirm: spawn on
the ground, HUD shows the seed and type, canopies sit on trunks. Benchmarks are not re-run:
direct `main.tscn` is seed 0 with the same scene contents, which test 1 pins.

## 9. Follow-ups

- **Back to title.** "Save and Quit to Title" in the in-game Esc menu. The pipeline load
  installs `field.glslh` as a process-wide shader override (`set_shader_source_override`), so
  this needs a teardown → re-create test across two pipelines in one process, watching for
  `RID was leaked` (RD leak counters read 0 on Metal).
- **`addons/` packaging.** Move binaries, `shaders/`, `assets/` and the world scene under
  `addons/voxel_everything/`; replace the ~10 hardcoded `res://shaders/` / `res://assets/`
  constants in C++ with a root the extension resolves.
- **`trees.pipeline`** is stage-identical to `default.pipeline` and a leftover of the trees
  rollout; delete it once nothing reads it.
