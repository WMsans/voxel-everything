# Title Menu, World Types and the Reusable World Scene Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Minecraft-style title screen (one button, Create New World) and Create New World screen (Seed + World Type), over a demo reorganised so the world is one scene that works under any parent, world types are `.tres` data, and the seed really moves the terrain.

**Architecture:** `demo/` splits into `scenes/`, `scripts/` and `world_types/`. `scenes/voxel_world.tscn` has a scripted `VoxelWorld` at its root (`VoxelWorldScene`) owning settings, environment, compositors and sun, and points its compositors at itself on `_enter_tree`; `main.tscn` instances it under the name `VoxelWorld` so every `/root/Main/VoxelWorld` path survives. The seed becomes `VoxelWorld.world_seed`, hashed to a whole-metre XZ domain shift applied once on each side of the field (generated GLSL and `PipelineFieldGenerator::sample`); the leaf pass walks shifted cells and writes world-space records. A new `flat` stage gives the third world type.

**Tech Stack:** C++20 GDExtension (godot-cpp, Godot 4.7), GLSL 460, doctest (native), gdUnit4 (GPU), GDScript, Godot `.tscn`/`.tres` text resources.

**Spec:** `docs/superpowers/specs/2026-10-07-title-menu-world-types-design.md`. Read it, and `docs/superpowers/specs/2026-09-17-stage-authoring-design.md` §2 and §4 M3 (stage manifests, CPU mirrors), before starting.

## Global Constraints

- No `Co-Authored-By` or other AI attribution lines in any commit message. Plain conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`).
- Work only in `.worktrees/world-types` on branch `feat/world-types` (created in Task 0 from `main`). Never commit to `main`.
- Running `demo/scenes/main.tscn` directly must behave exactly as `demo/main.tscn` does at `495f5dd`: Default pipeline, seed 0, player at `(8, 62, 8)`, same environment, same values. `tests/test_demo_scene_wiring.gd` (Task 1) is the pin.
- Seed `0` means offset `(0, 0)`. Every golden corpus (`tests/golden/*`, `test_default_pipeline_field.cpp`'s file, the frame goldens) must stay byte-identical. The only golden that may change is `shaders/generated/field.glslh.golden`, and only in Task 5.
- Offsets are whole metres in `[-8192, 8192)` per axis (spec §5.1).
- The importable set — `demo/scenes/voxel_world.tscn`, `demo/scripts/voxel_world_scene.gd`, `demo/scripts/world_type.gd`, `demo/world_types/*.tres` — must not reference any other `demo/` file.
- Generated files are never hand-edited. `shaders/generated/field.glslh.golden` is regenerated with `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc="the default pipeline generates the committed source"`.
- Every new `.gd` file gets its `.gd.uid` sidecar committed. Create them by running `./build.sh --verify` (headless editor scan) after adding the script. Never invent a uid by hand.
- Tabs for indentation in C++, GLSL and GDScript.
- Build: `./build.sh`. Native tests: `cd extension && scons -Q test` (single case: `cd extension && ./build/tests/ve_tests -tc="<name>"`). GPU tests: `./build.sh && ./gdunit_tests.sh -a res://tests/<file>.gd`.
- gdUnit has a standing, drifting set of failures on clean `main`. Task 0 records it; every "no new failures" check compares against that list, not against zero. A suite failing that is not on the list: `git stash -u`, re-run that suite on the clean tree, then decide. A failing gdUnit case aborts the rest of its suite, so a suite's case count dropping is itself a failure.
- GPU tests that only check wiring set `use_local_device = true` and `physics_enabled = false` on the world before it enters the tree.

## Deviations From The Spec (decided while planning; Task 10 records them in the spec)

1. **World type files carry an order prefix:** `00_default.tres`, `10_mesas.tres`, `20_flat.tres`. Sorted by plain name, Flat would come before Mesas; the spec's cycle is Default → Mesas → Flat.
2. **The leaf pass is not created when the pipeline has no `trees` stage.** `leaf_trees.comp.glsl` calls `trees_ground_h`, which only exists when the trees stage is in the generated source, so on Mesas and Flat the shader fails to compile and the orchestrator prints `leaf initialization failed` (it already fails soft). A shader error on every Mesas or Flat world would read as a bug.
3. **More node-name lookups move than spec §4.4 lists:** `tests/test_demo_shell.gd:59` and `tests/test_emissive_gi.gd:138` (`WorldEnvironment`), `demo/benchmark.gd:186` (`VoxelSettings`), `tools/grass_capture.gd:61` and `tools/leaf_capture.gd:90` (`DirectionalLight3D`). `test_cel_object.gd`, `test_deferred.gd` and `test_sun_shadow.gd` only construct their own `DirectionalLight3D` and do not change.
4. **`VoxelWorld.field_offset() -> Vector3`** is bound, computed straight from `world_seed`. Tests use it to stream the same terrain under a seed; it needs no pipeline load.
5. **The parser rejects `seed`** with a message naming `VoxelWorld.world_seed`, instead of the generic "unknown pipeline key".
6. **The seeded leaf test checks for bark on a small grid**, not at a single point. Tree records carry the crown's XZ, and a leaning trunk at 0.33 height sits up to ~1.4 m from it. The existing `test_painting_trunks_away_empties_the_tree_list` covers the same offset with a 2 m paint sphere.
7. **`load_pipeline` gains a trailing `uint32_t seed = 0` parameter** that it copies into the desc. `PipelineDesc::seed` stays as the carrier into `resolve_pipeline`, and its default changes from 1337 to 0.
8. **Create World's scene switch is a `launch` Callable member**, so a test can catch the built world instead of replacing gdUnit's own scene.
9. **The final visual check is scripted** (`tools/world_type_capture.gd`), the way every other look check in `tools/` is, so the screenshots can be reproduced.

## Review Focus

1. **Seed text that isn't a small plain integer:** `" 42 "`, `"+42"`, `"-0"`, a 20-digit number, `"🌲"`. Expected: whitespace trimmed, sign honoured, overlong digits hashed instead of tripping `to_int()`'s overflow error, and any text deterministic. Tests in Task 9 (`test_create_world.gd`).
2. **Two world-scene instances in one tree** (an importer's preview, split screen). Expected: each instance's compositors point at that instance and never share a resource. Test in Task 3 (`test_world_scene.gd`).
3. **A world that never initialises** (broken pipeline, no GPU). Expected: GroundSpawn gives up with a warning after its timeout and frees itself, and the player stays at the scene spawn; it never spins forever. Test in Task 9 (`test_ground_spawn.gd`).
4. **A stray `.tres` in `world_types/`**, or an exported build's `.tres.remap` listing. Expected: the stray file is skipped with a warning, `.remap` resolves to its resource, and the real types still list. Test in Task 9 (`test_create_world.gd`).
5. **A seed whose offset is near the ±8192 m edge.** Expected: CPU and GPU still agree within `test_field_diff.gd`'s tolerance; float precision and GPU `sin()` range reduction are at their worst there. Test in Task 6 picks a seed with |offset| > 7000 m. If it fails on precision, shrink the range in `seed_offset` (not the tolerance) and record it.

---

## File Structure

**Create:**
- `tests/test_demo_scene_wiring.gd` — characterization pin of `main.tscn`'s wiring (Task 1).
- `demo/scripts/world_type.gd` — `WorldType` resource: display name + pipeline path.
- `demo/scripts/voxel_world_scene.gd` — `VoxelWorldScene extends VoxelWorld`: type setter, compositor wiring, `ground_at`.
- `demo/scenes/voxel_world.tscn` — the importable world.
- `demo/world_types/00_default.tres`, `10_mesas.tres`, `20_flat.tres`.
- `tests/test_world_scene.gd` — importability tests.
- `extension/src/terrain/seed_offset.h`, `seed_offset.cpp` — `ve::seed_offset`.
- `extension/tests/test_seed_offset.cpp`.
- `shaders/stages/flat.field.glslh`, `assets/pipelines/flat.pipeline`.
- `tests/test_flat_world.gd`.
- `demo/menu_theme.tres` — the menu look.
- `demo/scenes/title.tscn`, `demo/scripts/title.gd`.
- `demo/scenes/create_world.tscn`, `demo/scripts/create_world.gd`.
- `demo/scripts/ground_spawn.gd`.
- `tests/test_create_world.gd`, `tests/test_ground_spawn.gd`, `tests/test_hud_world_line.gd`.
- `tools/world_type_capture.gd` — scripted visual check.

**Move (Task 2):** `demo/*.tscn` → `demo/scenes/`, `demo/*.gd` (+ `.gd.uid`) → `demo/scripts/`.

**Modify:**
- `demo/scenes/main.tscn` — instance the world scene; drop the moved nodes.
- `demo/scripts/benchmark.gd`, `demo/scripts/hud.gd`, `tools/grass_capture.gd`, `tools/leaf_capture.gd`, `tests/test_demo_shell.gd`, `tests/test_emissive_gi.gd` — moved node paths; HUD world line.
- Every `res://demo/...` / `demo/main.tscn` reference (Task 2's rewrite): `tools/*.gd`, `tools/run_benchmarks.sh`, `tests/*.gd`, `README.md`, `demo/scripts/capture.gd`.
- `extension/src/terrain/pipeline.h`, `pipeline.cpp`, `pipeline_load.h`, `pipeline_load.cpp`, `field_codegen.cpp`, `pipeline_field_generator.cpp`, `builtin_stages.cpp`.
- `shaders/field.glslh` (stub), `shaders/generated/field.glslh.golden`, `shaders/leaf_trees.comp.glsl`.
- `extension/src/leaves/leaf_layout.h`, `leaf_layout.cpp`, `extension/src/render/frame.cpp`, `extension/src/render/orchestrator.cpp`.
- `extension/src/voxel_world.h`, `voxel_world.cpp` — `world_seed`, `field_offset()`.
- `assets/pipelines/{default,golden,mesas,trees}.pipeline` — drop `seed`.
- `extension/tests/test_pipeline_parse.cpp`, `test_pipeline_load.cpp`, `test_cross_stage_param.cpp`, `test_field_codegen.cpp`, `test_pipeline_field_generator.cpp`, `test_leaf_layout.cpp`, `test_lipschitz_sampled.cpp`.
- `tests/test_field_diff.gd`, `tests/test_leaves.gd`.
- `project.godot`, `README.md`, the spec (Task 10).

---

### Task 0: Worktree and baseline

**Files:** none changed.

- [ ] **Step 1: Create the worktree**

Use superpowers:using-git-worktrees to create `.worktrees/world-types` on a new branch `feat/world-types` from `main` (which carries the spec commit `c42d276`). All later commands run from `.worktrees/world-types`.

Run: `git -C .worktrees/world-types status -sb | head -1`
Expected: `## feat/world-types`

- [ ] **Step 2: Build and run the native tests**

Run: `./build.sh --test`
Expected: build OK; doctest summary ends `Status: SUCCESS!`.

- [ ] **Step 3: Record the gdUnit baseline**

Run: `./gdunit_tests.sh 2>&1 | tee "$TMPDIR/world-types-baseline.txt" | tail -40`
Expected: a summary listing the failing suites (~20 minutes). Write the failing suite and case names into the task notes. Every later "no new failures" check compares against this list.

- [ ] **Step 4: Record the pins this plan re-runs**

Run:
```bash
for s in test_demo_shell test_field_diff test_leaves test_benchmark test_capture test_settings_menu test_material_picker test_emissive_gi test_cel_object test_voxel_settings; do
	echo "== $s"; ./gdunit_tests.sh -a res://tests/$s.gd 2>&1 | grep -E "Statistics|FAILED|PASSED" | tail -3
done
```
Expected: each suite's pass/fail state and case count, written into the task notes. These are the "pin suites" later tasks re-run.

---

### Task 1: Characterization test for `main.tscn`'s wiring

**Files:**
- Create: `tests/test_demo_scene_wiring.gd`

**Interfaces:**
- Consumes: `res://demo/main.tscn` as it is at `495f5dd`.
- Produces: a suite that every later task re-runs. It finds nodes by type and checks what each path *resolves to*, never the path text, so Task 3 can move nodes without editing it, except for the `MAIN` path constant, which Task 2's rewrite updates.

- [ ] **Step 1: Write the test**

```gdscript
extends GdUnitTestSuite
# Pins what the demo scene wires together, by node TYPE and by what each path RESOLVES to --
# never by the path text. The world-types plan moves nodes under a reusable world scene;
# this suite is how it proves the wiring survived the move.

const MAIN := "res://demo/main.tscn"

var _root: Node

func after_test() -> void:
	if is_instance_valid(_root):
		_root.free()
	_root = null

func _open() -> Node:
	_root = load(MAIN).instantiate()
	# The scene's absolute /root/Main/... paths only resolve at exactly this spot.
	_root.name = "Main"
	get_tree().root.add_child(_root)
	return _root

func _world() -> VoxelWorld:
	return _root.get_node("VoxelWorld") as VoxelWorld

# owned = false: once the world is an instanced sub-scene its children are owned by it, not
# by Main, and find_children's default would skip them.
func _one(type: String) -> Node:
	var found := _root.find_children("*", type, true, false)
	assert_int(found.size()).override_failure_message(
		"expected exactly one %s in main.tscn, found %d" % [type, found.size()]).is_equal(1)
	return found[0]

func test_both_compositor_effects_point_at_the_world() -> void:
	_open()
	var we := _one("WorldEnvironment") as WorldEnvironment
	var effects: Array = we.compositor.compositor_effects
	assert_int(effects.size()).is_equal(2)
	for e in effects:
		assert_object(get_tree().root.get_node_or_null(e.world_path)).override_failure_message(
			"%s.world_path does not resolve to the world" % e.get_class()).is_same(_world())

func test_the_sun_path_resolves_to_a_directional_light() -> void:
	_open()
	var w := _world()
	assert_object(w.get_node_or_null(w.sun_light_path)).is_instanceof(DirectionalLight3D)

func test_settings_drive_the_world_and_the_menu_drives_the_settings() -> void:
	_open()
	var vs := _one("VoxelSettings") as VoxelSettings
	assert_object(vs.get_node_or_null(vs.world_path)).is_same(_world())
	var menu = _root.get_node("HUD/SettingsMenu") # untyped: settings_path is a script property
	assert_object(menu.get_node_or_null(menu.settings_path)).is_same(vs)

func test_the_demo_layer_reaches_the_world() -> void:
	_open()
	var w := _world()
	# Untyped: these exports are script properties, which a native-typed variable cannot see.
	var hud = _root.get_node("HUD/Label")
	assert_object(hud.get_node_or_null(hud.world_path)).is_same(w)
	var tool = _root.get_node("EditTool")
	assert_object(tool.get_node_or_null(tool.world_path)).is_same(w)
	assert_object(tool.get_node_or_null(tool.camera_path)).is_same(_root.get_node("Player/Camera3D"))
	var picker = _root.get_node("HUD/MaterialPicker")
	assert_object(picker.get_node_or_null(picker.world_path)).is_same(w)
	var dev = _root.get_node("DevTools")
	assert_object(dev.get_node_or_null(dev.world_path)).is_same(w)
	assert_object(w.get_node_or_null(w.physics_center_path)).is_same(_root.get_node("Player"))

func test_the_shipped_values_the_benchmark_rides_on() -> void:
	_open()
	var w := _world()
	assert_float(w.near_field_scale).is_equal_approx(0.4, 1e-6)
	assert_str(w.terrain_pipeline_path).is_equal("res://assets/pipelines/default.pipeline")
	assert_int(w.process_mode).is_equal(Node.PROCESS_MODE_ALWAYS)
	assert_vector((_root.get_node("Player") as Node3D).position).is_equal(Vector3(8, 62, 8))
	var env: Environment = (_one("WorldEnvironment") as WorldEnvironment).environment
	assert_bool(env.glow_enabled).is_true()
	assert_float(env.glow_hdr_threshold).is_equal_approx(1.05, 1e-6)
	assert_float(env.ambient_light_energy).is_equal_approx(0.4, 1e-6)
```

- [ ] **Step 2: Run it against the untouched scene**

Run: `./gdunit_tests.sh -a res://tests/test_demo_scene_wiring.gd`
Expected: 5/5 PASS. This is a characterization test: it must pass *before* any change. If a case fails, the assertion is wrong about today's scene; fix the assertion, never the scene.

- [ ] **Step 3: Prove it bites**

Temporarily edit `demo/main.tscn`: change `sun_light_path = NodePath("/root/Main/DirectionalLight3D")` to `NodePath("/root/Main/Nope")`. Re-run the suite.
Expected: `test_the_sun_path_resolves_to_a_directional_light` FAILS. Revert with `git checkout demo/main.tscn` and re-run: 5/5 PASS.

- [ ] **Step 4: Commit**

```bash
git add tests/test_demo_scene_wiring.gd
git commit -m "test: pin the demo scene's wiring before the world-scene split"
```
(The `.gd.uid` sidecar appears after the next editor scan; Task 2 commits it.)

---

### Task 2: Move `demo/` into `scenes/` and `scripts/`

**Files:**
- Move: `demo/main.tscn`, `demo/settings_menu.tscn`, `demo/material_picker.tscn` → `demo/scenes/`; all `demo/*.gd` and `demo/*.gd.uid` → `demo/scripts/`.
- Modify: every reference found by the grep in Step 2.

**Interfaces:**
- Produces: `res://demo/scenes/main.tscn`, `res://demo/scenes/settings_menu.tscn`, `res://demo/scenes/material_picker.tscn`, `res://demo/scripts/<name>.gd`. No behaviour change.

- [ ] **Step 1: Move the files**

```bash
mkdir -p demo/scenes demo/scripts
git mv demo/main.tscn demo/settings_menu.tscn demo/material_picker.tscn demo/scenes/
for f in demo/*.gd; do git mv "$f" demo/scripts/; git mv "$f.uid" demo/scripts/; done
ls demo
```
Expected: `scenes  scripts` only.

- [ ] **Step 2: Rewrite every reference**

```bash
grep -rlE 'res://demo/|demo/main\.tscn' --exclude-dir=docs --exclude-dir=.godot \
	--exclude-dir=extension --exclude-dir=.worktrees --exclude-dir=addons . \
	| xargs perl -pi -e 's#res://demo/([a-z_]+\.gd)\b#res://demo/scripts/$1#g; s#res://demo/([a-z_]+\.tscn)\b#res://demo/scenes/$1#g; s#(?<![/\w])demo/main\.tscn#demo/scenes/main.tscn#g'
```
`docs/` is excluded on purpose: specs and plans are historical records and keep the paths they were written against.

- [ ] **Step 3: Verify nothing still points at the old layout**

Run: `grep -rnE 'res://demo/[a-z_]+\.(gd|tscn)|(^|[^/a-z])demo/main\.tscn' --exclude-dir=docs --exclude-dir=.godot --exclude-dir=extension --exclude-dir=.worktrees . ; echo "exit $?"`
Expected: no matches, `exit 1`.

Run: `git diff --stat | tail -1`
Expected: about 20 files changed (the scenes, ~13 tests/tools, `README.md`, `tools/run_benchmarks.sh`, `demo/scripts/capture.gd`).

- [ ] **Step 4: Refresh the editor's uid cache and check for parse errors**

Run: `./build.sh --verify`
Expected: `OK: no 'Could not find type' parse errors`. This also creates `tests/test_demo_scene_wiring.gd.uid` from Task 1.

- [ ] **Step 5: Run the pins**

Run: `./gdunit_tests.sh -a res://tests/test_demo_scene_wiring.gd`, then the Task 0 pin-suite loop.
Expected: wiring 5/5 PASS; every pin suite in the same state and case count as Task 0 recorded.

Run: `godot --path . --headless --quit-after 120 demo/scenes/main.tscn 2>&1 | grep -iE "error|cannot|failed" | head`
Expected: nothing beyond what the same command prints on `main` with the old path (check by running it in the main checkout with `demo/main.tscn` if anything appears).

- [ ] **Step 6: Commit**

```bash
git add -A demo tests tools README.md
git commit -m "chore: move the demo into scenes/ and scripts/"
```

---

### Task 3: The reusable world scene

**Files:**
- Create: `demo/scripts/world_type.gd`, `demo/scripts/voxel_world_scene.gd`, `demo/scenes/voxel_world.tscn`, `demo/world_types/00_default.tres`, `demo/world_types/10_mesas.tres`, `tests/test_world_scene.gd`
- Modify: `demo/scenes/main.tscn`, `demo/scripts/benchmark.gd:186`, `tools/grass_capture.gd:61`, `tools/leaf_capture.gd:90`, `tests/test_demo_shell.gd:59`, `tests/test_emissive_gi.gd:138`

**Interfaces:**
- Produces:
  - `class_name WorldType extends Resource` with `display_name: String`, `pipeline_path: String`.
  - `class_name VoxelWorldScene extends VoxelWorld` with `world_type: WorldType` (setter writes `terrain_pipeline_path`), `const GROUND_PROBE_Y := 600.0`, `func ground_at(x: float, z: float) -> Variant` (a `Vector3` or `null`).
  - `res://demo/scenes/voxel_world.tscn`: root `VoxelWorld` (`VoxelWorldScene`), children `VoxelSettings`, `WorldEnvironment`, `Sun`.
  - Node paths in `main.tscn`: `/root/Main/VoxelWorld` (unchanged), `/root/Main/VoxelWorld/VoxelSettings`, `/root/Main/VoxelWorld/WorldEnvironment`, `/root/Main/VoxelWorld/Sun`.

- [ ] **Step 1: Write the failing importability tests**

`tests/test_world_scene.gd`:
```gdscript
extends GdUnitTestSuite
# The importability promise (spec §4): voxel_world.tscn works under ANY parent, not only at
# /root/Main, and two instances never share render state.

const WORLD_SCENE := "res://demo/scenes/voxel_world.tscn"

var _holders: Array = []

func after_test() -> void:
	for h in _holders:
		if is_instance_valid(h):
			h.free()
	_holders.clear()

func _place(parent_name: String) -> VoxelWorldScene:
	var holder := Node3D.new()
	holder.name = parent_name
	get_tree().root.add_child(holder)
	_holders.append(holder)
	var w: VoxelWorldScene = load(WORLD_SCENE).instantiate()
	w.use_local_device = true
	w.physics_enabled = false
	holder.add_child(w)
	return w

func _effects(w: Node) -> Array:
	return (w.get_node("WorldEnvironment") as WorldEnvironment).compositor.compositor_effects

func test_compositors_follow_the_instance_wherever_it_is_placed() -> void:
	var w := _place("SomeOtherGame")
	assert_str(str(w.get_path())).is_equal("/root/SomeOtherGame/VoxelWorld")
	var fx := _effects(w)
	assert_int(fx.size()).is_equal(2)
	for e in fx:
		assert_object(get_tree().root.get_node_or_null(e.world_path)).is_same(w)

func test_two_instances_never_share_a_compositor() -> void:
	var a := _place("A")
	var b := _place("B")
	var fa := _effects(a)
	var fb := _effects(b)
	assert_int(fa.size()).is_equal(fb.size())
	for i in range(fa.size()):
		assert_object(fa[i]).is_not_same(fb[i])
		assert_object(get_tree().root.get_node_or_null(fa[i].world_path)).is_same(a)
		assert_object(get_tree().root.get_node_or_null(fb[i].world_path)).is_same(b)

func test_sun_and_settings_resolve_inside_the_instance() -> void:
	var w := _place("Elsewhere")
	assert_object(w.get_node_or_null(w.sun_light_path)).is_same(w.get_node("Sun"))
	var vs: VoxelSettings = w.get_node("VoxelSettings")
	assert_object(vs.get_node_or_null(vs.world_path)).is_same(w)

func test_the_world_type_sets_the_pipeline_before_the_tree() -> void:
	var w: VoxelWorldScene = load(WORLD_SCENE).instantiate()
	assert_str(w.terrain_pipeline_path).is_equal("res://assets/pipelines/default.pipeline")
	w.world_type = load("res://demo/world_types/10_mesas.tres")
	assert_bool(w.is_inside_tree()).is_false()
	assert_str(w.terrain_pipeline_path).is_equal("res://assets/pipelines/mesas.pipeline")
	w.free()

func test_ground_at_is_null_before_the_world_initialises() -> void:
	var w: VoxelWorldScene = load(WORLD_SCENE).instantiate()
	assert_that(w.ground_at(8.0, 8.0)).is_null()
	w.free()

func test_ground_at_finds_the_default_surface_once_initialised() -> void:
	var w := _place("Ground")
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var hit = w.ground_at(8.0, 8.0)
	assert_that(hit).is_not_null()
	# SURFACE_Y 51.2 plus hills (|h| <= 10 m); relief is ~0 this close to the origin.
	assert_float((hit as Vector3).y).is_between(41.0, 62.0)
```

- [ ] **Step 2: Run it to see it fail**

Run: `./gdunit_tests.sh -a res://tests/test_world_scene.gd`
Expected: FAIL. The script does not parse (`VoxelWorldScene` is not a type) or the scene is missing.

- [ ] **Step 3: Write `WorldType`**

`demo/scripts/world_type.gd`:
```gdscript
class_name WorldType
extends Resource
# One entry on the Create New World screen's World Type button. A world type IS a terrain
# pipeline (assets/pipelines/*.pipeline); this only gives it a name a player can read.
# Add a type by adding a .tres under demo/world_types/ -- no code changes.

@export var display_name := ""
@export_file("*.pipeline") var pipeline_path := ""
```

- [ ] **Step 4: Write `VoxelWorldScene`**

`demo/scripts/voxel_world_scene.gd`:
```gdscript
class_name VoxelWorldScene
extends VoxelWorld
# The reusable world (spec §4): terrain, settings, environment, compositors and sun in one
# scene that works under any parent. Instance res://demo/scenes/voxel_world.tscn, set
# physics_center_path to your player, and pick a world_type / world_seed BEFORE it enters
# the tree -- the terrain pipeline loads once, at first init.

# High enough to clear every shipped terrain: SURFACE_Y 51.2 + relief 310 + hills 10.
const GROUND_PROBE_Y := 600.0

@export var world_type: WorldType:
	set(value):
		world_type = value
		if value != null:
			terrain_pipeline_path = value.pipeline_path

func _enter_tree() -> void:
	# The compositor effects find their world by an ABSOLUTE path from the scene root, so
	# they can only be pointed here once this node has one. Each instance gets its own
	# copies: a scene's sub-resources are shared between instances, and two worlds writing
	# one world_path would both render the last one.
	var we := get_node_or_null("WorldEnvironment") as WorldEnvironment
	if we == null or we.compositor == null:
		return
	var effects: Array[CompositorEffect] = []
	for effect in we.compositor.compositor_effects:
		var copy := effect.duplicate() as CompositorEffect
		if "world_path" in copy:
			copy.set("world_path", get_path())
		effects.append(copy)
	var compositor := Compositor.new()
	compositor.compositor_effects = effects
	we.compositor = compositor

# The terrain surface straight below (x, GROUND_PROBE_Y, z), or null until the pipeline has
# loaded. raycast() queries the CPU field analytically, so it answers for ground that has
# not streamed yet.
func ground_at(x: float, z: float) -> Variant:
	if not is_initialized():
		return null
	var hit: Dictionary = raycast(Vector3(x, GROUND_PROBE_Y, z), Vector3.DOWN, GROUND_PROBE_Y * 2.0)
	return hit["pos"] if hit["hit"] else null
```

- [ ] **Step 5: Write the two world types**

`demo/world_types/00_default.tres`:
```
[gd_resource type="Resource" script_class="WorldType" format=3]

[ext_resource type="Script" path="res://demo/scripts/world_type.gd" id="1"]

[resource]
script = ExtResource("1")
display_name = "Default"
pipeline_path = "res://assets/pipelines/default.pipeline"
```
`demo/world_types/10_mesas.tres`: identical except `display_name = "Mesas"` and `pipeline_path = "res://assets/pipelines/mesas.pipeline"`.

- [ ] **Step 6: Write the world scene**

`demo/scenes/voxel_world.tscn`. The Environment block and the sun transform are copied verbatim from `demo/scenes/main.tscn`'s `[sub_resource type="Environment" id="3"]` and `DirectionalLight3D` node:
```
[gd_scene format=3]

[ext_resource type="Script" path="res://demo/scripts/voxel_world_scene.gd" id="1"]
[ext_resource type="Resource" path="res://demo/world_types/00_default.tres" id="2"]

[sub_resource type="Environment" id="env"]
background_mode = 4
ambient_light_source = 3
ambient_light_energy = 0.4
glow_enabled = true
glow_levels/2 = 0.3
glow_levels/3 = 0.9
glow_levels/4 = 1.0
glow_levels/5 = 1.0
glow_levels/6 = 0.8
glow_levels/7 = 0.5
glow_intensity = 0.4
glow_blend_mode = 0
glow_hdr_threshold = 1.05

[sub_resource type="RaymarchCompositor" id="raymarch"]

[sub_resource type="BeautyCompositor" id="beauty"]

[sub_resource type="Compositor" id="compositor"]
compositor_effects = Array[CompositorEffect]([SubResource("raymarch"), SubResource("beauty")])

[node name="VoxelWorld" type="VoxelWorld"]
script = ExtResource("1")
world_type = ExtResource("2")
sun_light_path = NodePath("Sun")

[node name="VoxelSettings" type="VoxelSettings" parent="."]
world_path = NodePath("..")

[node name="WorldEnvironment" type="WorldEnvironment" parent="."]
environment = SubResource("env")
compositor = SubResource("compositor")

[node name="Sun" type="DirectionalLight3D" parent="."]
transform = Transform3D(0.60042024, -0.79664165, -0.06969713, 0, -0.08715577, 0.9961947, -0.79968464, -0.5981355, -0.052330088, 0, 20, 0)
```

- [ ] **Step 7: Make `main.tscn` instance it**

In `demo/scenes/main.tscn`:

1. Add after the last `[ext_resource ...]` line:
```
[ext_resource type="PackedScene" path="res://demo/scenes/voxel_world.tscn" id="12"]
```
2. Delete the four sub-resources `[sub_resource type="Environment" id="3"]` (through `glow_hdr_threshold = 1.05`), `[sub_resource type="RaymarchCompositor" id="1"]`, `[sub_resource type="BeautyCompositor" id="6"]`, `[sub_resource type="Compositor" id="2"]`, each with its property lines.
3. Replace the VoxelWorld node block:
```
[node name="VoxelWorld" type="VoxelWorld" parent="." unique_id=948119314]
near_field_scale = 0.4
physics_center_path = NodePath("/root/Main/Player")
sun_light_path = NodePath("/root/Main/DirectionalLight3D")
process_mode = 3
```
with:
```
[node name="VoxelWorld" parent="." unique_id=948119314 instance=ExtResource("12")]
near_field_scale = 0.4
physics_center_path = NodePath("/root/Main/Player")
process_mode = 3
```
4. Delete the `[node name="VoxelSettings" ...]`, `[node name="WorldEnvironment" ...]` and `[node name="DirectionalLight3D" ...]` node blocks with their property lines.
5. In the `SettingsMenu` node, change `settings_path = NodePath("/root/Main/VoxelSettings")` to `settings_path = NodePath("/root/Main/VoxelWorld/VoxelSettings")`.

Check: `grep -nE 'Environment|Compositor|DirectionalLight3D|VoxelSettings"' demo/scenes/main.tscn`
Expected: no matches.

- [ ] **Step 8: Update the node-name lookups**

- `demo/scripts/benchmark.gd:186`: `get_parent().get_node("VoxelSettings")` → `get_parent().get_node("VoxelWorld/VoxelSettings")`.
- `tools/grass_capture.gd:61` and `tools/leaf_capture.gd:90`: `scene.get_node("DirectionalLight3D")` → `scene.get_node("VoxelWorld/Sun")`.
- `tests/test_demo_shell.gd:59`: `root.get_node("WorldEnvironment")` → `root.get_node("VoxelWorld/WorldEnvironment")`.
- `tests/test_emissive_gi.gd:138`: `scene.get_node("WorldEnvironment")` → `scene.get_node("VoxelWorld/WorldEnvironment")`.

Check: `grep -rnE 'get_node\("(WorldEnvironment|VoxelSettings|DirectionalLight3D)"\)' demo tools tests`
Expected: no matches.

- [ ] **Step 9: Scan, then run the new tests and the pins**

Run: `./build.sh --verify`
Expected: no parse errors; `demo/scripts/world_type.gd.uid`, `demo/scripts/voxel_world_scene.gd.uid`, `tests/test_world_scene.gd.uid` now exist.

Run: `./gdunit_tests.sh -a res://tests/test_world_scene.gd`
Expected: 6/6 PASS.

Run: `./gdunit_tests.sh -a res://tests/test_demo_scene_wiring.gd`, then the pin loop.
Expected: wiring 5/5 PASS (untouched since Task 2's path rewrite); pins as in Task 0.

- [ ] **Step 10: Confirm the importable set is self-contained**

Run: `grep -nE 'res://demo/' demo/scenes/voxel_world.tscn demo/scripts/voxel_world_scene.gd demo/scripts/world_type.gd demo/world_types/*.tres | grep -vE 'res://demo/(scripts/(voxel_world_scene|world_type)\.gd|world_types/)'; echo "exit $?"`
Expected: no matches, `exit 1`.

- [ ] **Step 11: Commit**

```bash
git add demo tests tools
git commit -m "feat: split the world into a reusable scene that works under any parent"
```

---

### Task 4: Remove the dead pipeline `seed` key

**Files:**
- Modify: `extension/src/terrain/pipeline.cpp:42`, `extension/src/terrain/pipeline.h:15`, `assets/pipelines/{default,golden,mesas,trees}.pipeline`, `extension/tests/test_pipeline_parse.cpp`, `extension/tests/test_pipeline_load.cpp:33`, `extension/tests/test_cross_stage_param.cpp:21`

**Interfaces:**
- Produces: `parse_pipeline_desc` rejects a `seed` line with an error containing `VoxelWorld.world_seed`. `PipelineDesc::seed` stays, still defaulting to 1337 in this task, so the field golden's hash line does not move yet.

- [ ] **Step 1: Write the failing test**

In `extension/tests/test_pipeline_parse.cpp`, delete the `"seed      1337\n"` line and the `CHECK(d.seed == 1337u);` line from the first case. Change `"seed 1\n"` in "an empty pipeline is an error" to `"lipschitz 2.0\n"`. Append:
```cpp
TEST_CASE("the seed is a world property, not a pipeline key") {
	ve::PipelineDesc d;
	std::string err;
	CHECK_FALSE(ve::parse_pipeline_desc("seed 1337\nstage a\n", &d, &err));
	CHECK(err.find("VoxelWorld.world_seed") != std::string::npos);
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cd extension && scons -Q test 2>&1 | tail -15`
Expected: FAIL in "the seed is a world property, not a pipeline key" (`seed` still parses).

- [ ] **Step 3: Reject the key**

`extension/src/terrain/pipeline.cpp:42`, replace:
```cpp
		if (key == "seed") out->seed = uint32_t(std::strtoul(rest.c_str(), nullptr, 10));
		else if (key == "lipschitz") out->lipschitz_ceiling = float(std::atof(rest.c_str()));
```
with:
```cpp
		// A terrain is a recipe; the seed picks where in it you stand, so it belongs to the
		// world, not the file. The key never reached a stage, which is why it was removed
		// rather than wired up.
		if (key == "seed")
			return fail("the pipeline 'seed' key was removed; set VoxelWorld.world_seed instead");
		if (key == "lipschitz") out->lipschitz_ceiling = float(std::atof(rest.c_str()));
```

`extension/src/terrain/pipeline.h:15`, replace `    uint32_t seed = 1337;` with:
```cpp
    // The WORLD seed, set by load_pipeline's caller -- never parsed from the file.
    uint32_t seed = 1337;
```

- [ ] **Step 4: Remove the key from every pipeline text**

```bash
perl -ni -e 'print unless /^seed\s/' assets/pipelines/default.pipeline assets/pipelines/golden.pipeline assets/pipelines/mesas.pipeline assets/pipelines/trees.pipeline
```
In `extension/tests/test_pipeline_load.cpp:33` change `"seed 7\nstage stages/hills.field.glslh\n"` to `"stage stages/hills.field.glslh\n"`. In `extension/tests/test_cross_stage_param.cpp:21` delete the `"seed 1337\n"` line.

Run: `grep -rn '^seed\|"seed ' assets/pipelines extension/tests; echo "exit $?"`
Expected: only the new rejection test's `"seed 1337\nstage a\n"`.

- [ ] **Step 5: Run the native suite**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: `Status: SUCCESS!`. Every golden, including `field.glslh.golden`, is unchanged: the desc default is still 1337, so the hash line is the same.

Run: `git status --short shaders tests/golden`
Expected: empty.

- [ ] **Step 6: Run the GPU field diff**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_field_diff.gd`
Expected: as Task 0 recorded (every shipped pipeline still loads).

- [ ] **Step 7: Commit**

```bash
git add extension/src/terrain/pipeline.h extension/src/terrain/pipeline.cpp assets/pipelines extension/tests
git commit -m "fix: remove the pipeline seed key, which never reached a stage"
```

---

### Task 5: The seed offset on both sides of the field (native)

**Files:**
- Create: `extension/src/terrain/seed_offset.h`, `extension/src/terrain/seed_offset.cpp`, `extension/tests/test_seed_offset.cpp`
- Modify: `extension/src/terrain/pipeline.h`, `pipeline.cpp`, `pipeline_load.h`, `pipeline_load.cpp`, `field_codegen.cpp`, `pipeline_field_generator.cpp`, `shaders/field.glslh`, `shaders/generated/field.glslh.golden`, `extension/tests/test_field_codegen.cpp`, `extension/tests/test_pipeline_load.cpp`, `extension/tests/test_pipeline_field_generator.cpp`

**Interfaces:**
- Produces:
  - `struct ve::SeedOffset { int32_t x = 0, z = 0; }; SeedOffset ve::seed_offset(uint32_t seed);`
  - `ResolvedPipeline::seed` (`uint32_t`), `ResolvedPipeline::field_offset_x`, `field_offset_z` (`int32_t`).
  - `bool ve::load_pipeline(reader, pipeline_path, stage_root, out, warnings, error, uint32_t seed = 0)`.
  - GLSL: `const vec3 VE_FIELD_OFFSET` declared by every `field.glslh` (generated and stub); `eval_base_field` samples at `p + VE_FIELD_OFFSET`.
  - `PipelineFieldGenerator::sample(x, y, z)` evaluates the stages at `(x + ox, y, z + oz)`.

- [ ] **Step 1: Write the failing offset tests**

`extension/tests/test_seed_offset.cpp`:
```cpp
#include <doctest/doctest.h>
#include "terrain/seed_offset.h"
#include <cstdint>
#include <cstdlib>
#include <set>
#include <utility>

TEST_CASE("seed 0 is the unshifted terrain every golden was recorded on") {
	const ve::SeedOffset o = ve::seed_offset(0);
	CHECK(o.x == 0);
	CHECK(o.z == 0);
}

TEST_CASE("the offset is a pure function of the seed") {
	for (uint32_t s : {1u, 42u, 1337u, 0xffffffffu}) {
		CHECK(ve::seed_offset(s).x == ve::seed_offset(s).x);
		CHECK(ve::seed_offset(s).z == ve::seed_offset(s).z);
	}
}

TEST_CASE("every offset stays inside +-8192 m on both axes") {
	for (uint32_t s = 1; s <= 20000; s++) {
		const ve::SeedOffset o = ve::seed_offset(s);
		REQUIRE(o.x >= -8192);
		REQUIRE(o.x < 8192);
		REQUIRE(o.z >= -8192);
		REQUIRE(o.z < 8192);
	}
	const ve::SeedOffset top = ve::seed_offset(0xffffffffu);
	CHECK(top.x >= -8192);
	CHECK(top.x < 8192);
}

TEST_CASE("distinct seeds land in distinct places") {
	std::set<std::pair<int32_t, int32_t>> seen;
	for (uint32_t s = 1; s <= 1000; s++) {
		const ve::SeedOffset o = ve::seed_offset(s);
		seen.insert({o.x, o.z});
	}
	CHECK(seen.size() == 1000);
}

TEST_CASE("neighbouring seeds are not neighbouring places") {
	for (uint32_t s = 1; s <= 200; s++) {
		const ve::SeedOffset a = ve::seed_offset(s), b = ve::seed_offset(s + 1);
		CHECK(std::abs(a.x - b.x) + std::abs(a.z - b.z) > 64);
	}
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile FAIL, `terrain/seed_offset.h` not found.

- [ ] **Step 3: Implement `seed_offset`**

`extension/src/terrain/seed_offset.h`:
```cpp
#pragma once
// The world seed as a translation of the field's domain
// (docs/superpowers/specs/2026-10-07-title-menu-world-types-design.md §5.1).
//
// Every shipped stage is a sum of fixed sinusoids, so a seed cannot reseed anything; it picks
// WHERE in the unbounded field the world origin sits. A shift is applied once per side
// (generate_field_glslh's VE_FIELD_OFFSET, PipelineFieldGenerator::sample), and a translation
// cannot change a gradient, so every stage's //!lipschitz still holds untouched.
#include <cstdint>

namespace ve {

struct SeedOffset {
	int32_t x = 0;
	int32_t z = 0;
};

// 0 -> (0, 0): the terrain the demo, the benchmark and every golden were recorded on.
// Any other seed -> two whole metres in [-8192, 8192). Whole metres are exact in float, and
// 8 km keeps a sample position's float resolution under 1 mm against 5 cm voxels -- the
// normal's 5 cm finite differences would facet somewhere past 100 km.
SeedOffset seed_offset(uint32_t seed);

} // namespace ve
```
`extension/src/terrain/seed_offset.cpp`:
```cpp
#include "terrain/seed_offset.h"

namespace ve {

SeedOffset seed_offset(uint32_t seed) {
	if (seed == 0) return {};
	// splitmix64: every input bit reaches every output bit, so seeds 41 and 42 land
	// kilometres apart rather than metres.
	uint64_t z = uint64_t(seed) + 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	z ^= z >> 31;
	SeedOffset o;
	o.x = int32_t(z & 0x3FFFu) - 8192;
	o.z = int32_t((z >> 14) & 0x3FFFu) - 8192;
	return o;
}

} // namespace ve
```

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: `Status: SUCCESS!` (the five new cases pass; nothing else changed yet). If "distinct seeds land in distinct places" fails, the hash is colliding far beyond chance (expected collision probability for 1000 seeds is ~0.2%); recheck the constants against the splitmix64 reference rather than weakening the test.

- [ ] **Step 4: Write the failing pipeline, codegen and CPU tests**

Append to `extension/tests/test_pipeline_load.cpp` (it already has `table_reader`, `kHills` and `<map>`), and add `#include "terrain/seed_offset.h"` to its includes:
```cpp
TEST_CASE("load_pipeline carries the world seed into the resolved pipeline") {
	const std::map<std::string, std::string> files{
		{"pipe/a.pipeline", "stage stages/hills.field.glslh\n"},
		{"root/stages/hills.field.glslh", kHills},
	};
	ve::ResolvedPipeline at_zero, seeded;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &at_zero,
			nullptr, &err), err);
	REQUIRE_MESSAGE(ve::load_pipeline(table_reader(files), "pipe/a.pipeline", "root/", &seeded,
			nullptr, &err, 77u), err);
	CHECK(at_zero.seed == 0u);
	CHECK(at_zero.field_offset_x == 0);
	CHECK(at_zero.field_offset_z == 0);
	CHECK(seeded.seed == 77u);
	CHECK(seeded.field_offset_x == ve::seed_offset(77u).x);
	CHECK(seeded.field_offset_z == ve::seed_offset(77u).z);
	CHECK(at_zero.hash != seeded.hash);
}
```
Append to `extension/tests/test_field_codegen.cpp`:
```cpp
TEST_CASE("the seed offset is declared once and applied where p enters the field") {
	ve::ResolvedPipeline p = two_stage();
	p.field_offset_x = -120;
	p.field_offset_z = 4000;
	const std::string g = ve::generate_field_glslh(p, "");
	CHECK(g.find("const vec3 VE_FIELD_OFFSET = vec3(-120, 0.0, 4000);\n") != std::string::npos);
	CHECK(g.find("\tctx.p = p + VE_FIELD_OFFSET;\n") != std::string::npos);
	// Declared before any stage body, so a stage helper may read it too.
	CHECK(g.find("VE_FIELD_OFFSET") < g.find("void stage_hills"));
}
```

Append to `extension/tests/test_pipeline_field_generator.cpp`:
```cpp
#include "terrain/pipeline_load.h"
#include "terrain/seed_offset.h"
#include <fstream>
#include <memory>
#include <sstream>

namespace {
bool shipped_reader(const std::string &path, std::string *out) {
	std::ifstream f(path);
	if (!f.good()) return false;
	std::ostringstream o;
	o << f.rdbuf();
	*out = o.str();
	return true;
}
} // namespace

TEST_CASE("a seeded CPU field is the unseeded field, translated") {
	const std::string root(VE_REPO_ROOT);
	ve::ResolvedPipeline p0, ps;
	std::string err;
	REQUIRE_MESSAGE(ve::load_pipeline(shipped_reader, root + "/assets/pipelines/default.pipeline",
			root + "/shaders/", &p0, nullptr, &err), err);
	REQUIRE_MESSAGE(ve::load_pipeline(shipped_reader, root + "/assets/pipelines/default.pipeline",
			root + "/shaders/", &ps, nullptr, &err, 9001u), err);
	std::unique_ptr<ve::PipelineFieldGenerator> g0(ve::PipelineFieldGenerator::create(p0, &err));
	std::unique_ptr<ve::PipelineFieldGenerator> gs(ve::PipelineFieldGenerator::create(ps, &err));
	REQUIRE(g0);
	REQUIRE(gs);
	const ve::SeedOffset o = ve::seed_offset(9001u);
	REQUIRE((o.x != 0 || o.z != 0));
	for (int i = 0; i < 64; i++) {
		const float x = -40.0f + 3.1f * float(i), y = 40.0f + 0.5f * float(i), z = 25.0f - 2.3f * float(i);
		const ve::Sample a = gs->sample(x, y, z);
		const ve::Sample b = g0->sample(x + float(o.x), y, z + float(o.z));
		CHECK(a.sdf == b.sdf);
		CHECK(a.material == b.material);
	}
}
```

- [ ] **Step 5: Run them to see them fail**

Run: `cd extension && scons -Q test 2>&1 | tail -15`
Expected: compile FAIL (`field_offset_x` is not a member; `load_pipeline` takes no seed).

- [ ] **Step 6: Carry the seed through resolve and load**

`extension/src/terrain/pipeline.h`: change `uint32_t seed = 1337;` to `uint32_t seed = 0;`. In `ResolvedPipeline`, after `uint64_t hash = 0;`, add:
```cpp
    uint32_t seed = 0;
    // seed_offset(seed), added to every sample position before the first stage runs, on
    // both sides: generate_field_glslh's VE_FIELD_OFFSET and PipelineFieldGenerator::sample.
    int32_t field_offset_x = 0;
    int32_t field_offset_z = 0;
```
`extension/src/terrain/pipeline.cpp`: add `#include "terrain/seed_offset.h"`. Replace `	hash_feed(h, std::to_string(desc.seed));` with:
```cpp
	out->seed = desc.seed;
	const SeedOffset offset = seed_offset(desc.seed);
	out->field_offset_x = offset.x;
	out->field_offset_z = offset.z;
	hash_feed(h, std::to_string(desc.seed));
```
`extension/src/terrain/pipeline_load.h`: change the declaration to
```cpp
// `seed` is the WORLD seed (VoxelWorld.world_seed); see terrain/seed_offset.h.
bool load_pipeline(const TextReader &reader, const std::string &pipeline_path,
		const std::string &stage_root, ResolvedPipeline *out,
		std::vector<std::string> *warnings, std::string *error, uint32_t seed = 0);
```
`extension/src/terrain/pipeline_load.cpp`: match the signature (no default in the definition), and after `if (!parse_pipeline_desc(src, &desc, error)) return false;` add `	desc.seed = seed;`.

- [ ] **Step 7: Apply the offset on the GPU side**

`extension/src/terrain/field_codegen.cpp`, after the loop that closes `struct FieldCtx` (`o << "};\n\n";`), add:
```cpp
	// The world seed as a domain shift (terrain/seed_offset.h). Declared even at zero so every
	// includer compiles against one shape: leaf_trees.comp.glsl moves its tree records back
	// to world space by it.
	o << "const vec3 VE_FIELD_OFFSET = vec3(" << float_text(float(p.field_offset_x)) << ", 0.0, "
	  << float_text(float(p.field_offset_z)) << ");\n\n";
```
and change `if (c.name == "p") { o << "\tctx.p = p;\n"; continue; }` to:
```cpp
		if (c.name == "p") { o << "\tctx.p = p + VE_FIELD_OFFSET;\n"; continue; }
```
`shaders/field.glslh` (the stub), directly after the `void eval_base_field(vec3 p, out float sdf, out uint mat);` forward declaration, add:
```glsl
// The generated source declares the world seed's domain shift; the stub has no seed.
const vec3 VE_FIELD_OFFSET = vec3(0.0);
```

- [ ] **Step 8: Apply the offset on the CPU side**

`extension/src/terrain/pipeline_field_generator.cpp`, in `PipelineFieldGenerator::sample`, replace:
```cpp
	ctx.v(pslot)[0] = x;
	ctx.v(pslot)[1] = y;
	ctx.v(pslot)[2] = z;
```
with:
```cpp
	// The world seed's domain shift, the same whole metres the GPU adds (VE_FIELD_OFFSET).
	ctx.v(pslot)[0] = x + float(p.field_offset_x);
	ctx.v(pslot)[1] = y;
	ctx.v(pslot)[2] = z + float(p.field_offset_z);
```

- [ ] **Step 9: Run the tests; review and regenerate the field golden**

Run: `cd extension && scons -Q test 2>&1 | tail -15`
Expected: everything passes except "the default pipeline generates the committed source".

Run: `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc="the default pipeline generates the committed source" && cd .. && git diff shaders/generated/field.glslh.golden`
Expected diff, exactly three hunks: the `// Pipeline hash:` line (the desc default seed moved 1337 → 0), the added `const vec3 VE_FIELD_OFFSET = vec3(0, 0.0, 0);` line, and `ctx.p = p;` → `ctx.p = p + VE_FIELD_OFFSET;`. Anything else is a bug; stop and find it.

Run: `cd extension && scons -Q test 2>&1 | tail -3 && cd .. && git status --short tests/golden`
Expected: `Status: SUCCESS!`; `tests/golden` unchanged.

- [ ] **Step 10: Commit**

```bash
git add extension/src/terrain extension/tests shaders/field.glslh shaders/generated/field.glslh.golden
git commit -m "feat: shift the terrain domain by a seed-derived offset on GPU and CPU"
```

---

### Task 6: `VoxelWorld.world_seed`, and GPU/CPU agreement under a seed

**Files:**
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`, `tests/test_field_diff.gd`, `tests/test_demo_scene_wiring.gd`

**Interfaces:**
- Consumes: `ve::seed_offset`, `load_pipeline(..., seed)` (Task 5).
- Produces: `VoxelWorld.world_seed: int` (property, default 0, stored as `uint32_t`, so negative values wrap); `VoxelWorld.field_offset() -> Vector3` (`(ox, 0, oz)` computed from `world_seed`, no load needed).

- [ ] **Step 1: Write the failing GPU test**

In `tests/test_field_diff.gd`, change `_open_world` to take a seed:
```gdscript
func _open_world(pipeline_path: String, seed := 0) -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = pipeline_path
	_world.world_seed = seed
	add_child(_world)
```
(the rest of the function unchanged). Append:
```gdscript
# Around the CPU surface, wherever it is: under a seed the ground can sit hundreds of metres
# from SURFACE_Y, and points that never cross it would test nothing.
func surface_points() -> PackedVector3Array:
	var pts := PackedVector3Array()
	var rng := RandomNumberGenerator.new()
	rng.seed = 20261007
	for i in range(512):
		var x := rng.randf_range(-20.0, 60.0)
		var z := rng.randf_range(-20.0, 60.0)
		var hit: Dictionary = _world.raycast(Vector3(x, 600.0, z), Vector3.DOWN, 1200.0)
		var gy: float = (hit["pos"] as Vector3).y if hit["hit"] else 51.2
		pts.append(Vector3(x, gy + rng.randf_range(-12.0, 12.0), z))
	return pts

# The seed is a domain shift applied once per side (spec §5.2): if only one side applied it
# the two fields would disagree by whole hills, far past MAX_STEPS. The seed is picked for a
# FAR offset -- past 7 km float precision and GPU sin() range reduction are at their worst
# inside the shipped range (plan Review Focus 5).
func test_a_far_seeded_world_agrees_between_cpu_and_gpu() -> void:
	var probe: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	var seed := 0
	for s in range(1, 10000):
		probe.world_seed = s
		var o: Vector3 = probe.field_offset()
		if maxf(absf(o.x), absf(o.z)) > 7000.0:
			seed = s
			break
	probe.free()
	assert_int(seed).is_greater(0)
	_open_world("res://assets/pipelines/default.pipeline", seed)
	var pts := surface_points()
	compare(pts, PackedByteArray(), 0, "seed %d base" % seed)
	compare(pts, make_op(OP_SUBTRACT, 0, pts[0], 6.0), 1, "seed %d subtract" % seed)
	_close_world()
```
In `tests/test_demo_scene_wiring.gd`, `test_the_shipped_values_the_benchmark_rides_on`, add `	assert_int(w.world_seed).is_equal(0)` after the `terrain_pipeline_path` assertion.

- [ ] **Step 2: Run it to see it fail**

Run: `./gdunit_tests.sh -a res://tests/test_field_diff.gd`
Expected: FAIL (`world_seed` / `field_offset` are not members of VoxelWorld).

- [ ] **Step 3: Add the property**

`extension/src/voxel_world.h`, next to `terrain_pipeline_path_`:
```cpp
	// The world seed (terrain/seed_offset.h). Like the pipeline path, read once at first init.
	uint32_t world_seed_ = 0;
```
and next to the pipeline-path accessors:
```cpp
	void set_world_seed(int64_t v) { world_seed_ = static_cast<uint32_t>(v); }
	int64_t get_world_seed() const { return world_seed_; }
	// seed_offset(world_seed) as a vector -- what the field adds to every sample position.
	// Computed from the property, so it is valid before the pipeline loads.
	Vector3 field_offset() const;
```
`extension/src/voxel_world.cpp`: add `#include "terrain/seed_offset.h"`. In `_bind_methods`, next to the pipeline-path bindings:
```cpp
	ClassDB::bind_method(D_METHOD("set_world_seed", "v"), &VoxelWorld::set_world_seed);
	ClassDB::bind_method(D_METHOD("get_world_seed"), &VoxelWorld::get_world_seed);
	ClassDB::bind_method(D_METHOD("field_offset"), &VoxelWorld::field_offset);
```
next to the `terrain_pipeline_path` `ADD_PROPERTY`:
```cpp
	ADD_PROPERTY(PropertyInfo(Variant::INT, "world_seed"), "set_world_seed", "get_world_seed");
```
and a definition near `raycast`:
```cpp
Vector3 VoxelWorld::field_offset() const {
	const ve::SeedOffset o = ve::seed_offset(world_seed_);
	return Vector3(static_cast<float>(o.x), 0.0f, static_cast<float>(o.z));
}
```
In `load_terrain_pipeline`, change the `ve::load_pipeline(` call's argument list from `..., &resolved, &warnings, &err)` to `..., &resolved, &warnings, &err, world_seed_)`.

- [ ] **Step 4: Run it to see it pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_field_diff.gd`
Expected: `test_every_pipeline_agrees_between_cpu_and_gpu` as Task 0 recorded, and `test_a_far_seeded_world_agrees_between_cpu_and_gpu` PASS. If it fails on precision only (worst within a few steps, both sides plainly shifted), apply Review Focus 5: shrink the range in `seed_offset` (`0x3FFF`/`8192` → `0x1FFF`/`4096`), update Task 5's range test and the spec, and re-run. Never widen `MAX_STEPS`.

- [ ] **Step 5: Prove the test bites**

Temporarily change `field_codegen.cpp`'s `ctx.p = p + VE_FIELD_OFFSET;` back to `ctx.p = p;`, then `./build.sh && ./gdunit_tests.sh -a res://tests/test_field_diff.gd`.
Expected: the seeded case FAILS with a worst disagreement of hundreds of encoded steps. Restore the line (`git checkout extension/src/terrain/field_codegen.cpp`), rebuild, and re-run: PASS.

- [ ] **Step 6: Run the pins**

Run: `./gdunit_tests.sh -a res://tests/test_demo_scene_wiring.gd`, then the pin loop.
Expected: wiring 5/5; pins as in Task 0.

- [ ] **Step 7: Commit**

```bash
git add extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_field_diff.gd tests/test_demo_scene_wiring.gd
git commit -m "feat: VoxelWorld.world_seed, with a GPU/CPU agreement test at a far offset"
```

---

### Task 7: The leaf pass under a seed

**Files:**
- Modify: `extension/src/leaves/leaf_layout.h:77`, `extension/src/leaves/leaf_layout.cpp:49-70`, `extension/src/render/frame.cpp:110-114`, `shaders/leaf_trees.comp.glsl:73-80`, `extension/tests/test_leaf_layout.cpp`, `tests/test_leaves.gd`

**Interfaces:**
- Consumes: `ResolvedPipeline::field_offset_x/_z` (Task 5), `VoxelWorld.world_seed` / `field_offset()` (Task 6), `VE_FIELD_OFFSET` (Task 5).
- Produces: `ve::leaf_layout(settings, camera, view_proj, float field_offset_x = 0.0f, float field_offset_z = 0.0f)`. The cell box is computed around `camera + offset` (shifted space); `params.cam` stays the world camera. Tree records in `tree_list` stay world-space.

- [ ] **Step 1: Write the failing native test**

Append to `extension/tests/test_leaf_layout.cpp`:
```cpp
TEST_CASE("a field offset moves the cell box into shifted space, not the camera") {
	ve::LeafSettings s;
	s.reach_m = 250.0f;
	const float cam[3] = {100.0f, 60.0f, -200.0f};
	const float shifted[3] = {100.0f + 1234.0f, 60.0f, -200.0f - 4321.0f};
	const ve::LeafLayout a = ve::leaf_layout(s, cam, kIdentity, 1234.0f, -4321.0f);
	const ve::LeafLayout b = ve::leaf_layout(s, shifted, kIdentity);
	CHECK(a.cell_min.x == b.cell_min.x);
	CHECK(a.cell_min.z == b.cell_min.z);
	CHECK(a.cell_dim.x == b.cell_dim.x);
	CHECK(a.cell_dim.z == b.cell_dim.z);
	CHECK(a.dispatch_threads == b.dispatch_threads);
	// Culling and distance stay in world space: the camera never moves.
	CHECK(a.params.cam[0] == cam[0]);
	CHECK(a.params.cam[2] == cam[2]);
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: compile FAIL (too many arguments to `leaf_layout`).

- [ ] **Step 3: Implement the shifted cell box**

`extension/src/leaves/leaf_layout.h:77`: change the declaration to
```cpp
// field_offset_x/z: the world seed's domain shift (terrain/seed_offset.h). Trees live on the
// SHIFTED lattice the trees stage walks, so the cell box is taken around camera + offset;
// params.cam stays the world camera, because culling and distance are world-space.
LeafLayout leaf_layout(const LeafSettings &settings, const float camera[3],
		const float view_proj[16], float field_offset_x = 0.0f, float field_offset_z = 0.0f);
```
`extension/src/leaves/leaf_layout.cpp`: match the definition's signature (no defaults), and in the cell-box block replace the four `lo_x`/`lo_z`/`hi_x`/`hi_z` lines with:
```cpp
		const float sx = camera[0] + field_offset_x;
		const float sz = camera[2] + field_offset_z;
		const int lo_x = int(std::floor((sx - l.reach_m) / c));
		const int lo_z = int(std::floor((sz - l.reach_m) / c));
		const int hi_x = int(std::floor((sx + l.reach_m) / c));
		const int hi_z = int(std::floor((sz + l.reach_m) / c));
```

Run: `cd extension && scons -Q test 2>&1 | tail -3`
Expected: `Status: SUCCESS!`.

- [ ] **Step 4: Write the failing GPU test**

Append to `tests/test_leaves.gd`:
```gdscript
const BARK := 8 # ve::kMaterials index 7 + 1; material 0 is air

# The default grove the suite streams, moved by the seed: the same terrain lies at
# world (20, 60, 30) - field_offset().
func make_seeded_world(seed: int) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.world_seed = seed
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var center := Vector3(20.0, 60.0, 30.0) - w.field_offset()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(center) == 0 else 0
		if quiet >= 6:
			break
	return w

# Records carry the CROWN's xz; a leaning trunk at a third of its height sits up to ~1.4 m
# from it, so look for wood on a small grid rather than at one point.
func _cpu_bark_near(w: VoxelWorld, p: Vector3) -> bool:
	for ix in range(-4, 5):
		for iz in range(-4, 5):
			var s: Vector2 = w.hooks().debug_eval_field(
					p + Vector3(ix * 0.4, 0.0, iz * 0.4), PackedByteArray(), 0)
			if s.x <= 0.0 and int(s.y) == BARK:
				return true
	return false

# A canopy must stand on a trunk the CPU field agrees exists, at any seed. Seed 0 is the
# control; a seed whose leaf pass walks unshifted cells, or writes shifted records, lists
# trees with no wood under them.
func test_every_listed_tree_stands_on_cpu_bark_under_a_seed() -> void:
	for seed in [0, 9001]:
		var w := make_seeded_world(seed)
		var d: Dictionary = w.hooks().debug_leaf_stats()
		var trees := int(d["trees"])
		assert_int(trees).override_failure_message(
			"seed %d: no trees listed around the shifted grove" % seed).is_greater(0)
		var recs: PackedFloat32Array = d["tree_records"]
		var missing := 0
		for i in range(trees):
			var base_y: float = recs[i * 8 + 4]
			var height: float = recs[i * 8 + 1] - base_y
			if not _cpu_bark_near(w, Vector3(recs[i * 8], base_y + height * 0.33, recs[i * 8 + 2])):
				missing += 1
		assert_int(missing).override_failure_message(
			"seed %d: %d of %d listed trees have no CPU bark under them" % [seed, missing, trees]
			).is_equal(0)
		_worlds.erase(w)
		w.free()
```

- [ ] **Step 5: Run it to see it fail**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_leaves.gd`
Expected: the new case FAILS at seed 9001 (seed 0 passes): either no trees listed, or every listed tree missing its bark.

- [ ] **Step 6: Feed the offset to the layout, and shift records back in the shader**

`extension/src/render/frame.cpp`, `VoxelFrame::leaf_layout`, replace the `return` line with:
```cpp
	const ve::ResolvedPipeline &tp = store_.terrain_pipeline();
	return ve::leaf_layout(render_.leaf_settings(), cam_pos, view_proj,
			static_cast<float>(tp.field_offset_x), static_cast<float>(tp.field_offset_z));
```
`shaders/leaf_trees.comp.glsl`, in `main()`, directly after `if (!t.present) return;` add:
```glsl
	// The cell lattice, tree_cell_xz and trees_ground_h above are all in the field's SHIFTED
	// space (the world seed's VE_FIELD_OFFSET), exactly as the trees stage sees them. From
	// here on everything touches the world -- the camera cull, the atlas trunk check, the
	// record stage 2 reads -- so move the tree back once. Its lobes and limbs are all relative
	// to base and crown, so nothing else needs shifting.
	t.base -= VE_FIELD_OFFSET;
	t.crown -= VE_FIELD_OFFSET;
```

- [ ] **Step 7: Run the tests**

Run: `cd extension && scons -Q test 2>&1 | tail -3`
Expected: `Status: SUCCESS!`.

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_leaves.gd`
Expected: every case that passed in Task 0 passes, plus the new one.

Run the frame golden suites recorded in Task 0 (`for f in tests/test_*_golden.gd; do [ "$f" = tests/test_frame_shipped_golden.gd ] || ./gdunit_tests.sh -a "res://$f" 2>&1 | tail -3; done`).
Expected: same state as Task 0 (seed 0 is a no-op).

- [ ] **Step 8: Confirm the debug hooks drive the shipping layout**

Spec §5.2 asks for every hook that computes field positions or tree cells on its own to be shifted too. Run: `grep -nE "ve::leaf_layout\(|tree_cell|trees_ground|AnalyticGenerator" extension/src/debug/*.cpp; echo "exit $?"`
Expected: no matches, `exit 1`. `debug_leaf_stats` calls `frame().leaf_layout(...)`, the function Step 6 changed, and reads back the shipping pass's records. Nothing else in the hooks rebuilds placement, so no hook needs an edit. If a match does appear, give it the same offset and add it to the deviations.

- [ ] **Step 9: Commit**

```bash
git add extension/src/leaves extension/src/render/frame.cpp shaders/leaf_trees.comp.glsl extension/tests/test_leaf_layout.cpp tests/test_leaves.gd
git commit -m "fix: place canopies on the seeded trunks"
```

---

### Task 8: The Flat world type

**Files:**
- Create: `shaders/stages/flat.field.glslh`, `assets/pipelines/flat.pipeline`, `demo/world_types/20_flat.tres`, `tests/test_flat_world.gd`
- Modify: `extension/src/terrain/builtin_stages.cpp`, `extension/tests/test_lipschitz_sampled.cpp`, `extension/src/render/orchestrator.cpp:334-340`

**Interfaces:**
- Produces: stage `flat` (`//!out sdf, height`, param `level = 2.0`, `//!lipschitz add 1.0`, CPU symbol `ve::stage_flat`); `res://assets/pipelines/flat.pipeline`; `res://demo/world_types/20_flat.tres` (`display_name = "Flat"`). The orchestrator creates `LeafScatterPass` only when the pipeline has a stage named `trees`.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_lipschitz_sampled.cpp`:
```cpp
TEST_CASE("flat.pipeline never exceeds its reported gradient bound") {
	check_bound("flat.pipeline");
}
```
`tests/test_flat_world.gd`:
```gdscript
extends GdUnitTestSuite
# The Flat world type (spec §5.3): a level grass plain at SURFACE_Y + 2 m, and no canopy pass,
# because it has no trees stage for one to stand on.

const SURFACE_Y := 51.2
const GRASS := 1 # ve::kMaterials index 0 + 1

var _world: VoxelWorld

func after_test() -> void:
	if is_instance_valid(_world):
		_world.free()

func _open() -> VoxelWorld:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = "res://assets/pipelines/flat.pipeline"
	_world.use_local_device = true
	_world.physics_enabled = false
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	return _world

func test_flat_is_level_grass_at_two_metres() -> void:
	var w := _open()
	for xz in [Vector2(8, 8), Vector2(-300, 120), Vector2(2500, -4000)]:
		var hit: Dictionary = w.raycast(Vector3(xz.x, 600.0, xz.y), Vector3.DOWN, 1200.0)
		assert_bool(hit["hit"]).is_true()
		assert_float((hit["pos"] as Vector3).y).is_equal_approx(SURFACE_Y + 2.0, 0.05)
		assert_int(int(hit["material"])).is_equal(GRASS)

func test_the_seed_does_not_change_flat() -> void:
	var w := _open()
	var a: Vector2 = w.hooks().debug_eval_field(Vector3(8, 50, 8), PackedByteArray(), 0)
	w.free()
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = "res://assets/pipelines/flat.pipeline"
	_world.world_seed = 4242
	_world.use_local_device = true
	_world.physics_enabled = false
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	var b: Vector2 = _world.hooks().debug_eval_field(Vector3(8, 50, 8), PackedByteArray(), 0)
	assert_vector(b).is_equal(a)

func test_a_world_without_trees_has_no_leaf_pass() -> void:
	var w := _open()
	assert_bool(w.is_initialized()).is_true()
	assert_bool(w.hooks().debug_leaf_stats()["ran"]).is_false()
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd extension && scons -Q test 2>&1 | tail -5`
Expected: FAIL, `cannot read .../flat.pipeline`.

- [ ] **Step 3: Write the stage, its mirror and the pipeline**

`shaders/stages/flat.field.glslh`:
```glsl
//!stage     flat
//!kind      field
//!out       sdf : float
//!out       height : float
//!param     level : float = 2.0
//!lipschitz add 1.0
//!cpu       ve::stage_flat

// The Flat world type: a level plane `level` metres above SURFACE_Y. `height` is published so
// height_bands can paint it; 2 m sits in its grass band (1 m < h <= 4 m).
//
// Bound: sdf = y - SURFACE_Y - level, so |grad sdf| = 1 exactly. It establishes the field,
// so it combines with `add`.
void stage_flat(inout FieldCtx ctx) {
	ctx.height = P.flat_level;
	ctx.sdf = ctx.p.y - SURFACE_Y - ctx.height;
}
```
`extension/src/terrain/builtin_stages.cpp`, after `stage_mesas`:
```cpp
VE_STAGE_SLOTS(Flat, p, sdf, height);
VE_STAGE_PARAMS(Flat, level);

void stage_flat(FieldCtx &ctx, const FlatSlots &s, const FlatParams &p, const FieldResources &) {
	ctx.f(s.height) = p.level;
	ctx.f(s.sdf) = ctx.v(s.p)[1] - kSurfaceY - ctx.f(s.height);
}
```
and with the other registrations:
```cpp
VE_REGISTER_STAGE("ve::stage_flat", Flat, stage_flat);
```
(`kSurfaceY` comes from `generator/generator.h`, already included for `stage_hills`.)

`assets/pipelines/flat.pipeline`:
```
# The Flat world type: a level grass plain. No hills, so no relief, cave or trees -- those
# stages //!use hills params. The world seed shifts the domain, which a plane cannot show.
# A CEILING, not the bound: flat add 1.0 => 1.0.
lipschitz 1.0

stage stages/flat.field.glslh
stage stages/height_bands.field.glslh
```
`demo/world_types/20_flat.tres`: as `00_default.tres` with `display_name = "Flat"` and `pipeline_path = "res://assets/pipelines/flat.pipeline"`.

- [ ] **Step 4: Run the native tests**

Run: `cd extension && scons -Q test 2>&1 | tail -3`
Expected: `Status: SUCCESS!`.

- [ ] **Step 5: See the leaf-pass error a Flat world prints today**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_flat_world.gd 2>&1 | tee "$TMPDIR/flat-before.txt" | tail -8; grep -c "leaf initialization failed" "$TMPDIR/flat-before.txt"`
Expected: the three cases PASS (the leaf pass fails soft, so `ran` is already false), and the grep count is **≥ 1**: the shader error this step exists to remove.

- [ ] **Step 6: Skip the leaf pass when there are no trees**

`extension/src/render/orchestrator.cpp`, replace the `passes_.leaf_scatter = new LeafScatterPass();` block (through its closing `}`) with:
```cpp
	// The leaf pass places canopies with the trees stage's own ground functions
	// (trees_ground_h / trees_ground_slope, emitted into the generated field source), so it
	// only exists in a world whose pipeline has that stage. Without it the shader cannot
	// compile, and a compile error on every Mesas or Flat world would read as a bug.
	bool has_trees = false;
	for (const ve::StageManifest &s : handles_.store->terrain_pipeline().stages)
		if (s.name == "trees") has_trees = true;
	if (has_trees) {
		passes_.leaf_scatter = new LeafScatterPass();
		if (!passes_.leaf_scatter->initialize(device)) {
			UtilityFunctions::printerr("VoxelWorld: leaf initialization failed; continuing "
					"without canopies (safe fail-soft: trunks stand bare)");
			delete passes_.leaf_scatter;
			passes_.leaf_scatter = nullptr;
		}
	}
```

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_flat_world.gd 2>&1 | tee "$TMPDIR/flat-after.txt" | tail -8; grep -c "leaf initialization failed" "$TMPDIR/flat-after.txt"`
Expected: 3/3 PASS, grep count **0**.

- [ ] **Step 7: Run the suites a pipeline change can reach**

Run: `./gdunit_tests.sh -a res://tests/test_field_diff.gd` (now includes `flat.pipeline`, GPU against CPU), then `./gdunit_tests.sh -a res://tests/test_leaves.gd`.
Expected: field diff PASS for every pipeline including `flat.pipeline`; leaves as after Task 7.

- [ ] **Step 8: Commit**

```bash
./build.sh --verify
git add shaders/stages/flat.field.glslh assets/pipelines/flat.pipeline demo/world_types/20_flat.tres extension/src/terrain/builtin_stages.cpp extension/src/render/orchestrator.cpp extension/tests/test_lipschitz_sampled.cpp tests/test_flat_world.gd tests/test_flat_world.gd.uid
git commit -m "feat: the Flat world type, and no leaf pass in a world without trees"
```

---

### Task 9: Title, Create New World, GroundSpawn and the HUD seed line

**Files:**
- Create: `demo/menu_theme.tres`, `demo/scenes/title.tscn`, `demo/scripts/title.gd`, `demo/scenes/create_world.tscn`, `demo/scripts/create_world.gd`, `demo/scripts/ground_spawn.gd`, `tests/test_create_world.gd`, `tests/test_ground_spawn.gd`, `tests/test_hud_world_line.gd`
- Modify: `demo/scripts/hud.gd`

**Interfaces:**
- Consumes: `WorldType`, `VoxelWorldScene.world_type`, `VoxelWorldScene.ground_at` (Task 3); `VoxelWorld.world_seed` (Task 6); the three `.tres` (Tasks 3, 8).
- Produces (`demo/scripts/create_world.gd`, preloaded by tests as `CreateWorld`):
  - `static func parse_seed(text: String) -> int`: 32-bit, non-negative.
  - `static func load_types(dir_path: String) -> Array[WorldType]`.
  - `static func build_world(type: WorldType, seed: int) -> Node`: a `main.tscn` instance, not in the tree, with `GroundSpawn` added.
  - `var launch: Callable`: called with the built world; defaults to `change_scene_to_node`.
- Produces (`demo/scripts/ground_spawn.gd`): exports `world_path := ^"../VoxelWorld"`, `player_path := ^"../Player"`, `lift_m := 2.0`, `timeout_s := 10.0`.
- Produces (`demo/scripts/hud.gd`): `static func world_line(world: Node) -> String`.

- [ ] **Step 1: Write the failing tests**

`tests/test_create_world.gd`:
```gdscript
extends GdUnitTestSuite

const CreateWorld := preload("res://demo/scripts/create_world.gd")
const TYPES_DIR := "res://demo/world_types"
const FIXTURE_DIR := "user://test_world_types_fixture"

var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

func test_seed_text_parses_like_minecraft() -> void:
	assert_int(CreateWorld.parse_seed("42")).is_equal(42)
	assert_int(CreateWorld.parse_seed(" 42 ")).is_equal(42)
	assert_int(CreateWorld.parse_seed("+42")).is_equal(42)
	assert_int(CreateWorld.parse_seed("0")).is_equal(0)
	assert_int(CreateWorld.parse_seed("-0")).is_equal(0)
	assert_int(CreateWorld.parse_seed("-1")).is_equal(0xffffffff)
	assert_int(CreateWorld.parse_seed("hello")).is_equal("hello".hash() & 0xffffffff)
	assert_int(CreateWorld.parse_seed("🌲")).is_equal("🌲".hash() & 0xffffffff)

func test_an_overlong_number_is_hashed_not_overflowed() -> void:
	var t := "99999999999999999999"
	assert_int(CreateWorld.parse_seed(t)).is_equal(t.hash() & 0xffffffff)

func test_a_blank_seed_is_random_and_never_zero() -> void:
	for i in range(50):
		var s: int = CreateWorld.parse_seed("   ")
		assert_int(s).is_greater(0)
		assert_int(s).is_less_equal(0xffffffff)

func test_world_types_list_default_mesas_flat() -> void:
	var names: Array = CreateWorld.load_types(TYPES_DIR).map(func(t): return t.display_name)
	assert_array(names).is_equal(["Default", "Mesas", "Flat"])

func test_a_stray_resource_in_the_types_folder_is_skipped() -> void:
	DirAccess.make_dir_recursive_absolute(FIXTURE_DIR)
	var wt := WorldType.new()
	wt.display_name = "Real"
	wt.pipeline_path = "res://assets/pipelines/default.pipeline"
	ResourceSaver.save(wt, FIXTURE_DIR.path_join("a_real.tres"))
	ResourceSaver.save(Theme.new(), FIXTURE_DIR.path_join("b_stray.tres"))
	var types: Array = CreateWorld.load_types(FIXTURE_DIR)
	assert_int(types.size()).is_equal(1)
	assert_str(types[0].display_name).is_equal("Real")
	for f in DirAccess.get_files_at(FIXTURE_DIR):
		DirAccess.remove_absolute(FIXTURE_DIR.path_join(f))

func test_a_missing_types_folder_lists_nothing() -> void:
	assert_int(CreateWorld.load_types("res://no/such/dir").size()).is_equal(0)

func test_create_builds_the_chosen_world_before_it_enters_the_tree() -> void:
	var mesas: WorldType = CreateWorld.load_types(TYPES_DIR)[1]
	var main: Node = CreateWorld.build_world(mesas, 4242)
	_nodes.append(main)
	assert_bool(main.is_inside_tree()).is_false()
	var w: VoxelWorldScene = main.get_node("VoxelWorld")
	assert_str(w.terrain_pipeline_path).is_equal("res://assets/pipelines/mesas.pipeline")
	assert_int(w.world_seed).is_equal(4242)
	assert_object(main.get_node_or_null("GroundSpawn")).is_not_null()

func test_the_type_button_cycles_and_enter_creates() -> void:
	var screen = load("res://demo/scenes/create_world.tscn").instantiate() # untyped: launch is a script member
	var launched: Array = []
	screen.launch = func(world: Node) -> void: launched.append(world)
	add_child(screen)
	_nodes.append(screen)
	var button: Button = screen.get_node("%WorldType")
	assert_str(button.text).is_equal("World Type: Default")
	button.pressed.emit()
	assert_str(button.text).is_equal("World Type: Mesas")
	button.pressed.emit()
	assert_str(button.text).is_equal("World Type: Flat")
	button.pressed.emit()
	assert_str(button.text).is_equal("World Type: Default")
	button.pressed.emit()
	var seed_box: LineEdit = screen.get_node("%Seed")
	seed_box.text = "777"
	seed_box.text_submitted.emit(seed_box.text)
	assert_int(launched.size()).is_equal(1)
	var w: VoxelWorldScene = launched[0].get_node("VoxelWorld")
	assert_str(w.world_type.display_name).is_equal("Mesas")
	assert_int(w.world_seed).is_equal(777)
	_nodes.append(launched[0])
```

`tests/test_ground_spawn.gd`:
```gdscript
extends GdUnitTestSuite
# GroundSpawn against a stub world, so no GPU is involved: what matters is when it moves the
# player and that it always leaves.

const GroundSpawn := preload("res://demo/scripts/ground_spawn.gd")

var _holder: Node

func after_test() -> void:
	if is_instance_valid(_holder):
		_holder.free()

func _rig(answer: Variant, timeout_s := 10.0) -> Array:
	var stub := GDScript.new()
	stub.source_code = "extends Node\nvar answer = null\nfunc ground_at(_x, _z):\n\treturn answer\n"
	stub.reload()
	_holder = Node.new()
	var world := Node.new()
	world.name = "VoxelWorld"
	world.set_script(stub)
	world.set("answer", answer)
	_holder.add_child(world)
	var player := Node3D.new()
	player.name = "Player"
	player.position = Vector3(8, 62, 8)
	_holder.add_child(player)
	var spawn := GroundSpawn.new()
	spawn.timeout_s = timeout_s
	_holder.add_child(spawn)
	add_child(_holder)
	return [world, player, spawn]

func _frames(n: int) -> void:
	for i in range(n):
		await get_tree().process_frame

func test_the_player_lands_two_metres_above_the_ground() -> void:
	var r := _rig(Vector3(8, 120, 8))
	await _frames(3)
	assert_vector((r[1] as Node3D).position).is_equal(Vector3(8, 122, 8))
	assert_bool(is_instance_valid(r[2])).is_false()

func test_it_waits_while_the_world_is_not_ready() -> void:
	var r := _rig(null)
	await _frames(3)
	assert_vector((r[1] as Node3D).position).is_equal(Vector3(8, 62, 8))
	assert_bool(is_instance_valid(r[2])).is_true()
	r[0].set("answer", Vector3(8, 40, 8))
	await _frames(3)
	assert_vector((r[1] as Node3D).position).is_equal(Vector3(8, 42, 8))

func test_a_world_that_never_initialises_is_given_up_on() -> void:
	var r := _rig(null, 0.05)
	await _frames(30)
	assert_bool(is_instance_valid(r[2])).is_false()
	assert_vector((r[1] as Node3D).position).is_equal(Vector3(8, 62, 8))
```

`tests/test_hud_world_line.gd`:
```gdscript
extends GdUnitTestSuite

const Hud := preload("res://demo/scripts/hud.gd")

func test_the_line_names_the_seed_and_the_type() -> void:
	var w: VoxelWorldScene = load("res://demo/scenes/voxel_world.tscn").instantiate()
	w.world_type = load("res://demo/world_types/10_mesas.tres")
	w.world_seed = 42
	assert_str(Hud.world_line(w)).is_equal("seed 42 · Mesas")
	w.free()

func test_a_bare_world_falls_back_to_its_pipeline_name() -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	assert_str(Hud.world_line(w)).is_equal("seed 0 · default")
	w.free()

func test_no_world_no_line() -> void:
	assert_str(Hud.world_line(null)).is_equal("")
```

- [ ] **Step 2: Run them to see them fail**

Run: `./gdunit_tests.sh -a res://tests/test_create_world.gd -a res://tests/test_ground_spawn.gd -a res://tests/test_hud_world_line.gd`
Expected: FAIL (scripts and scenes missing; `world_line` not defined).

- [ ] **Step 3: Write `ground_spawn.gd`**

```gdscript
extends Node
# One-shot: once the world can answer a ground query, put the player `lift_m` above the ground
# under it, then leave. Only the Create New World flow adds this node, so main.tscn run
# directly keeps its fixed (8, 62, 8) spawn for the benchmark and the captures.
# The player starts in FLY mode (player.gd), so it hovers there until the player presses F.

@export var world_path := ^"../VoxelWorld"
@export var player_path := ^"../Player"
@export var lift_m := 2.0
@export var timeout_s := 10.0

var _elapsed := 0.0

func _process(delta: float) -> void:
	var world := get_node_or_null(world_path)
	var player := get_node_or_null(player_path) as Node3D
	if world == null or player == null:
		queue_free()
		return
	var hit: Variant = world.ground_at(player.global_position.x, player.global_position.z)
	if hit != null:
		player.global_position = (hit as Vector3) + Vector3.UP * lift_m
		queue_free()
		return
	_elapsed += delta
	if _elapsed >= timeout_s:
		push_warning("GroundSpawn: the world did not initialise within %.0f s; " % timeout_s
				+ "leaving the player at the scene's spawn")
		queue_free()
```

- [ ] **Step 4: Write `create_world.gd`**

```gdscript
extends Control
# Minecraft's Create New World screen, cut to its two world-shaping options: a seed and a world
# type (spec §6.3). World name, game mode and the rest are out of scope.

const TYPES_DIR := "res://demo/world_types"
const MAIN_SCENE := "res://demo/scenes/main.tscn"
const TITLE_SCENE := "res://demo/scenes/title.tscn"
const GroundSpawn := preload("res://demo/scripts/ground_spawn.gd")
# More digits than this and to_int() overflows int64. Minecraft hashes whatever
# Long.parseLong rejects, and so does this.
const MAX_SEED_DIGITS := 18

# The scene switch. A member so a test can catch the built world instead of replacing
# gdUnit's own scene.
var launch := func(world: Node) -> void: get_tree().change_scene_to_node(world)

var _types: Array[WorldType] = []
var _type_index := 0

# Blank -> random non-zero; an integer -> itself; anything else -> its hash. 32 bits either
# way, the width VoxelWorld.world_seed stores.
static func parse_seed(text: String) -> int:
	var t := text.strip_edges()
	if t.is_empty():
		return randi_range(1, 0x7fffffff)
	if t.is_valid_int() and t.trim_prefix("+").trim_prefix("-").length() <= MAX_SEED_DIGITS:
		return t.to_int() & 0xffffffff
	return t.hash() & 0xffffffff

# Every WorldType .tres in dir_path, by file name -- which is why the shipped files carry an
# order prefix (00_default, 10_mesas, 20_flat).
static func load_types(dir_path: String) -> Array[WorldType]:
	var names := PackedStringArray()
	for f in DirAccess.get_files_at(dir_path):
		# An exported build lists "<name>.tres.remap"; load() takes the original name.
		f = f.trim_suffix(".remap")
		if f.ends_with(".tres") and not names.has(f):
			names.append(f)
	names.sort()
	var out: Array[WorldType] = []
	for f in names:
		var r := load(dir_path.path_join(f))
		if r is WorldType:
			out.append(r)
		else:
			push_warning("world types: %s is not a WorldType; skipped" % dir_path.path_join(f))
	return out

# The demo scene with the chosen type and seed. Both are set BEFORE the instance enters the
# tree: the terrain pipeline loads once, at the world's first init.
static func build_world(type: WorldType, seed: int) -> Node:
	var main: Node = load(MAIN_SCENE).instantiate()
	var world: VoxelWorldScene = main.get_node("VoxelWorld")
	world.world_type = type
	world.world_seed = seed
	var spawn: Node = GroundSpawn.new()
	spawn.name = "GroundSpawn"
	main.add_child(spawn)
	return main

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	_types = load_types(TYPES_DIR)
	(%WorldType as Button).pressed.connect(_next_type)
	(%Create as Button).pressed.connect(_create)
	(%Cancel as Button).pressed.connect(_cancel)
	(%Seed as LineEdit).text_submitted.connect(func(_text: String) -> void: _create())
	_show_type()
	(%Seed as LineEdit).grab_focus()

# _input, not _unhandled_input: the focused LineEdit would otherwise see Esc first.
func _input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_cancel"):
		get_viewport().set_input_as_handled()
		_cancel()

func _show_type() -> void:
	var has := not _types.is_empty()
	(%WorldType as Button).disabled = not has
	(%Create as Button).disabled = not has
	(%WorldType as Button).text = "World Type: %s" % (
			_types[_type_index].display_name if has else "none found")

func _next_type() -> void:
	if _types.is_empty():
		return
	_type_index = (_type_index + 1) % _types.size()
	_show_type()

func _create() -> void:
	if _types.is_empty():
		return
	launch.call(build_world(_types[_type_index], parse_seed((%Seed as LineEdit).text)))

func _cancel() -> void:
	get_tree().change_scene_to_file(TITLE_SCENE)
```

- [ ] **Step 5: Write `title.gd`**

```gdscript
extends Control
# The title screen: one button, Create New World (spec §6.2).

const CREATE_WORLD := "res://demo/scenes/create_world.tscn"

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	var button := %CreateWorld as Button
	button.pressed.connect(func() -> void: get_tree().change_scene_to_file(CREATE_WORLD))
	button.grab_focus()
```

- [ ] **Step 6: Write the theme**

`demo/menu_theme.tres`:
```
[gd_resource type="Theme" format=3]

[sub_resource type="StyleBoxFlat" id="button"]
content_margin_left = 24.0
content_margin_top = 12.0
content_margin_right = 24.0
content_margin_bottom = 12.0
bg_color = Color(0.44, 0.44, 0.44, 1)
border_width_left = 4
border_width_top = 4
border_width_right = 4
border_width_bottom = 4
border_color = Color(0.1, 0.1, 0.1, 1)

[sub_resource type="StyleBoxFlat" id="button_hover"]
content_margin_left = 24.0
content_margin_top = 12.0
content_margin_right = 24.0
content_margin_bottom = 12.0
bg_color = Color(0.45, 0.5, 0.68, 1)
border_width_left = 4
border_width_top = 4
border_width_right = 4
border_width_bottom = 4
border_color = Color(1, 1, 1, 1)

[sub_resource type="StyleBoxFlat" id="button_disabled"]
content_margin_left = 24.0
content_margin_top = 12.0
content_margin_right = 24.0
content_margin_bottom = 12.0
bg_color = Color(0.2, 0.2, 0.2, 1)
border_width_left = 4
border_width_top = 4
border_width_right = 4
border_width_bottom = 4
border_color = Color(0.06, 0.06, 0.06, 1)

[sub_resource type="StyleBoxFlat" id="focus"]
draw_center = false
border_width_left = 4
border_width_top = 4
border_width_right = 4
border_width_bottom = 4
border_color = Color(1, 1, 1, 1)

[sub_resource type="StyleBoxFlat" id="field"]
content_margin_left = 16.0
content_margin_top = 8.0
content_margin_right = 16.0
content_margin_bottom = 8.0
bg_color = Color(0, 0, 0, 1)
border_width_left = 4
border_width_top = 4
border_width_right = 4
border_width_bottom = 4
border_color = Color(0.63, 0.63, 0.63, 1)

[sub_resource type="StyleBoxFlat" id="field_focus"]
content_margin_left = 16.0
content_margin_top = 8.0
content_margin_right = 16.0
content_margin_bottom = 8.0
bg_color = Color(0, 0, 0, 1)
border_width_left = 4
border_width_top = 4
border_width_right = 4
border_width_bottom = 4
border_color = Color(1, 1, 1, 1)

[resource]
default_font_size = 40
Button/colors/font_color = Color(0.88, 0.88, 0.88, 1)
Button/colors/font_hover_color = Color(1, 1, 0.63, 1)
Button/colors/font_pressed_color = Color(1, 1, 0.63, 1)
Button/colors/font_focus_color = Color(1, 1, 0.63, 1)
Button/colors/font_disabled_color = Color(0.55, 0.55, 0.55, 1)
Button/styles/normal = SubResource("button")
Button/styles/hover = SubResource("button_hover")
Button/styles/pressed = SubResource("button_hover")
Button/styles/disabled = SubResource("button_disabled")
Button/styles/focus = SubResource("focus")
Label/colors/font_color = Color(1, 1, 1, 1)
Label/colors/font_shadow_color = Color(0.24, 0.24, 0.24, 1)
Label/constants/shadow_offset_x = 4
Label/constants/shadow_offset_y = 4
LineEdit/colors/font_color = Color(0.88, 0.88, 0.88, 1)
LineEdit/colors/font_placeholder_color = Color(0.5, 0.5, 0.5, 1)
LineEdit/styles/normal = SubResource("field")
LineEdit/styles/focus = SubResource("field_focus")
MenuTitle/base_type = &"Label"
MenuTitle/font_sizes/font_size = 120
```

- [ ] **Step 7: Write the two scenes**

`demo/scenes/title.tscn`:
```
[gd_scene format=3]

[ext_resource type="Script" path="res://demo/scripts/title.gd" id="1"]
[ext_resource type="Theme" path="res://demo/menu_theme.tres" id="2"]
[ext_resource type="Texture2D" path="res://assets/materials/02_basecolor.png" id="3"]

[node name="Title" type="Control"]
layout_mode = 3
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2
theme = ExtResource("2")
script = ExtResource("1")

[node name="Background" type="TextureRect" parent="."]
modulate = Color(0.3, 0.3, 0.3, 1)
layout_mode = 1
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2
mouse_filter = 2
texture = ExtResource("3")
expand_mode = 1
stretch_mode = 1

[node name="Center" type="CenterContainer" parent="."]
layout_mode = 1
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2

[node name="Column" type="VBoxContainer" parent="Center"]
layout_mode = 2
theme_override_constants/separation = 96

[node name="Logo" type="Label" parent="Center/Column"]
layout_mode = 2
theme_type_variation = &"MenuTitle"
text = "VOXEL EVERYTHING"
horizontal_alignment = 1

[node name="CreateWorld" type="Button" parent="Center/Column"]
unique_name_in_owner = true
custom_minimum_size = Vector2(800, 80)
layout_mode = 2
size_flags_horizontal = 4
text = "Create New World"
```
`demo/scenes/create_world.tscn`:
```
[gd_scene format=3]

[ext_resource type="Script" path="res://demo/scripts/create_world.gd" id="1"]
[ext_resource type="Theme" path="res://demo/menu_theme.tres" id="2"]
[ext_resource type="Texture2D" path="res://assets/materials/02_basecolor.png" id="3"]

[node name="CreateWorld" type="Control"]
layout_mode = 3
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2
theme = ExtResource("2")
script = ExtResource("1")

[node name="Background" type="TextureRect" parent="."]
modulate = Color(0.3, 0.3, 0.3, 1)
layout_mode = 1
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2
mouse_filter = 2
texture = ExtResource("3")
expand_mode = 1
stretch_mode = 1

[node name="Center" type="CenterContainer" parent="."]
layout_mode = 1
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2

[node name="Column" type="VBoxContainer" parent="Center"]
layout_mode = 2
theme_override_constants/separation = 32

[node name="Heading" type="Label" parent="Center/Column"]
layout_mode = 2
text = "Create New World"
horizontal_alignment = 1

[node name="Spacer" type="Control" parent="Center/Column"]
custom_minimum_size = Vector2(0, 48)
layout_mode = 2

[node name="SeedLabel" type="Label" parent="Center/Column"]
layout_mode = 2
text = "Seed for the World Generator"

[node name="Seed" type="LineEdit" parent="Center/Column"]
unique_name_in_owner = true
custom_minimum_size = Vector2(800, 72)
layout_mode = 2
placeholder_text = "Leave blank for a random seed"

[node name="WorldType" type="Button" parent="Center/Column"]
unique_name_in_owner = true
custom_minimum_size = Vector2(800, 80)
layout_mode = 2
text = "World Type: Default"

[node name="Spacer2" type="Control" parent="Center/Column"]
custom_minimum_size = Vector2(0, 48)
layout_mode = 2

[node name="Buttons" type="HBoxContainer" parent="Center/Column"]
layout_mode = 2
theme_override_constants/separation = 24

[node name="Create" type="Button" parent="Center/Column/Buttons"]
unique_name_in_owner = true
custom_minimum_size = Vector2(388, 80)
layout_mode = 2
text = "Create New World"

[node name="Cancel" type="Button" parent="Center/Column/Buttons"]
unique_name_in_owner = true
custom_minimum_size = Vector2(388, 80)
layout_mode = 2
text = "Cancel"
```

- [ ] **Step 8: Add the HUD world line**

In `demo/scripts/hud.gd`, add after `_reticle_circle_radius()`:
```gdscript
# "seed 123456 · Mesas": a blank-seed world is otherwise not reproducible. Minecraft shows the
# seed in F3 for the same reason. A bare VoxelWorld (tests, tools) has no world_type, so it
# is named after its pipeline file.
static func world_line(world: Node) -> String:
	if world == null:
		return ""
	var wt = world.get("world_type")
	var type_name: String = wt.display_name if wt != null \
			else String(world.get("terrain_pipeline_path")).get_file().get_basename()
	return "seed %d · %s" % [int(world.get("world_seed")), type_name]
```
In `_update_text()`, change the COMPACT assignment to
```gdscript
		text = "%s  |  %d fps  (%.1f ms)  |  %s  radius %.1f" % [world_line(_world), fps, ms, tool_name, radius]
```
and the final FULL assignment to
```gdscript
	text = "%s\n%d fps  (%.1f ms)  |  %s%s%s%s%s%s\n%s%s" % [world_line(_world), fps, ms, s, p, isl, lod, norm, grass, gpu_line, tool_line]
```

- [ ] **Step 9: Scan, then run the tests**

Run: `./build.sh --verify`
Expected: no parse errors; `.gd.uid` sidecars for `title.gd`, `create_world.gd`, `ground_spawn.gd` and the three new test suites exist.

Run: `./gdunit_tests.sh -a res://tests/test_create_world.gd -a res://tests/test_ground_spawn.gd -a res://tests/test_hud_world_line.gd`
Expected: 8 + 3 + 3 cases PASS.

Run: `./gdunit_tests.sh -a res://tests/test_demo_scene_wiring.gd`, then the pin loop.
Expected: as before (the HUD change is text only; `test_capture` hides the HUD).

- [ ] **Step 10: Commit**

```bash
git add demo tests
git commit -m "feat: Minecraft-style title and Create New World screens"
```

---

### Task 10: Main scene, README, visual check and the record

**Files:**
- Create: `tools/world_type_capture.gd`
- Modify: `project.godot`, `README.md`, `docs/superpowers/specs/2026-10-07-title-menu-world-types-design.md`

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Make the title screen the main scene**

In `project.godot`, `[application]` section, after `config/features=...`, add:
```
run/main_scene="res://demo/scenes/title.tscn"
```
Run: `godot --path . --headless --quit-after 30 2>&1 | grep -iE "error|failed" | head`
Expected: nothing (the title scene loads with no main-scene error).

- [ ] **Step 2: Write the capture tool**

`tools/world_type_capture.gd`:
```gdscript
extends SceneTree
# Reproducible look check for the title flow and each world type, through the shipping path:
# the same CreateWorld.build_world the Create button calls, then GroundSpawn, then a frame.
# godot --path . --resolution 1280x720 -s res://tools/world_type_capture.gd -- --out=/tmp/world-types

const CreateWorld := preload("res://demo/scripts/create_world.gd")

func _initialize() -> void:
	call_deferred("capture")

func _shot(path: String) -> void:
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(path)
	print("WORLD_TYPE_CAPTURE saved ", path)

func capture() -> void:
	var out := "/tmp/world-types"
	var only := ""
	var seed_text := "hello"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out = arg.trim_prefix("--out=")
		if arg.begins_with("--type="):
			only = arg.trim_prefix("--type=")
		if arg.begins_with("--seed="):
			seed_text = arg.trim_prefix("--seed=")
	DirAccess.make_dir_recursive_absolute(out)

	for scene_path in ["res://demo/scenes/title.tscn", "res://demo/scenes/create_world.tscn"]:
		var menu: Node = load(scene_path).instantiate()
		root.add_child(menu)
		for i in range(10):
			await process_frame
		await _shot(out.path_join(scene_path.get_file().get_basename() + ".png"))
		menu.free()

	for t in CreateWorld.load_types("res://demo/world_types"):
		if only != "" and t.display_name != only:
			continue
		var main: Node = CreateWorld.build_world(t, CreateWorld.parse_seed(seed_text))
		root.add_child(main)
		var world: VoxelWorld = main.get_node("VoxelWorld")
		var player: Node3D = main.get_node("Player")
		player.set_physics_process(false)
		player.set_process_unhandled_input(false)
		var settled := false
		for frame in range(6000):
			await process_frame
			if main.get_node_or_null("GroundSpawn") != null:
				continue
			var st: Dictionary = world.hooks().debug_lod_stats()
			if int(st.get("requests_pending", 1)) == 0 and int(st.get("builds_in_flight", 1)) == 0 \
					and int(st.get("draw_pages", 0)) > 0:
				settled = true
				break
		if not settled:
			push_error("world type capture did not settle for %s" % t.display_name)
		var cam: Camera3D = player.get_node("Camera3D")
		cam.rotation.x = -0.25
		for i in range(8):
			await process_frame
		await _shot(out.path_join("world_%s.png" % t.display_name.to_lower()))
		print("WORLD_TYPE_CAPTURE ", t.display_name, " seed ", world.world_seed,
				" player ", player.global_position)
		world.shutdown_render_resources()
		await process_frame
		main.free()
		await process_frame
	quit()
```
Note: this tool frees one world and creates the next in one process (spec §9's untested teardown path). If the second world fails to render or the run crashes, capture one type per process with `--type=<name>` instead, and record what happened in the spec's follow-up (§9, "Back to title").

- [ ] **Step 3: Run the visual check and look at every frame**

Run: `godot --path . --resolution 1280x720 -s res://tools/world_type_capture.gd -- --out=$TMPDIR/world-types 2>&1 | grep WORLD_TYPE_CAPTURE`
(If it fails after the first world, run it three times with `--type=Default`, `--type=Mesas`, `--type=Flat`.)

Then Read each PNG in `$TMPDIR/world-types/` and confirm:
- `title.png`: dark tiled dirt background, "VOXEL EVERYTHING", one Create New World button, centred.
- `create_world.png`: heading, seed label + empty black field with placeholder, "World Type: Default", Create/Cancel row.
- `world_default.png`, `world_mesas.png`, `world_flat.png`: terrain under the camera (not sky only, not inside rock); the HUD's first line reads `seed <n> · <Type>`; Default shows trees with canopies on trunks (if none are in frame, say so rather than claim it); Mesas shows mesas; Flat is a level grass plane.
- Each `player` printout is 2 m above that world's ground (Flat: y ≈ 55.2).

Run the same with `--seed=` (blank, `--type=Default`) and confirm the printed seed is non-zero.

- [ ] **Step 4: Update the README**

In `README.md` "Getting started", replace the sentence that tells the reader to run the demo scene with:
```markdown
Build the GDExtension, then open the project in **Godot 4.x** and run it: the title screen's
**Create New World** takes a seed and a world type (Default, Mesas, Flat). The benchmark and
capture tools run `demo/scenes/main.tscn` directly, which always opens the Default world at seed 0:
```
and add after the "Run the benchmark/test harness" block:
```markdown
### Using the world in another project

Copy `extension/` (built), `voxel_everything.gdextension`, `shaders/` and `assets/`, plus
`demo/scenes/voxel_world.tscn`, `demo/scripts/voxel_world_scene.gd`,
`demo/scripts/world_type.gd` and the `demo/world_types/` you want. Instance
`voxel_world.tscn`, set its `physics_center_path` to your player, and set `world_type` and
`world_seed` before it enters the tree. Add a world type by adding a `WorldType` `.tres`
pointing at a `.pipeline` file.
```
and in "Project layout", change the `demo/` line to:
```markdown
- `demo/` — `scenes/` (title menu, the reusable `voxel_world.tscn`, the playable `main.tscn`), `scripts/`, `world_types/`
```

- [ ] **Step 5: Record the deviations in the spec**

In `docs/superpowers/specs/2026-10-07-title-menu-world-types-design.md`: set `**Status:**` to `implemented; see §10`, and append:
```markdown
## 10. Deviations recorded while planning and implementing

<the nine items from the plan's "Deviations From The Spec" section, verbatim, numbered>

<anything Task 6 Step 4 changed about the offset range, with the measured worst disagreement>

<what the Task 10 capture showed, including whether one process could free one world and
start the next (§9 "Back to title")>
```
Replace the three angle-bracket lines with the actual content; leave no placeholder.

- [ ] **Step 6: Full run against the baseline**

Run: `./build.sh --test && ./gdunit_tests.sh 2>&1 | tee "$TMPDIR/world-types-final.txt" | tail -40`
Expected: native `Status: SUCCESS!`; the gdUnit failure set equals Task 0's list (stash-check any newcomer per the Global Constraints), plus the new suites all green: `test_demo_scene_wiring`, `test_world_scene`, `test_flat_world`, `test_create_world`, `test_ground_spawn`, `test_hud_world_line`, and the new cases in `test_field_diff`, `test_leaves`.

- [ ] **Step 7: Commit**

```bash
git add project.godot README.md tools/world_type_capture.gd tools/world_type_capture.gd.uid docs/superpowers/specs/2026-10-07-title-menu-world-types-design.md
git commit -m "feat: the title screen is the main scene; record the world-types deviations"
```
Then use superpowers:finishing-a-development-branch.
