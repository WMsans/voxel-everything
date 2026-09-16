# Settings Store, Settings Panel and S1/S2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every dial the demo exposes (display, render, beauty, grass) is stored, clamped, described and persisted by one C++ settings mechanism and shown in one F1 panel; S1, S2, S5 and S6 are fixed with failing tests first.

**Architecture:** A pure, header-only `SettingRow<T>` table and `SettingsStore<T>` (base + sticky overrides → resolved value) in `extension/src/settings/`, instantiated four times: `render`, `beauty`, `grass` in `RenderOrchestrator`, `display` in a new `VoxelSettings` Node that also owns `ConfigFile` persistence, the measured-run guard and inspector properties. `demo/settings_menu.{gd,tscn}` builds its tabs from `VoxelSettings.describe(group)`. S1/S2 thread `OverrideStore` (and volumes for S1) through the existing CPU probes.

**Tech Stack:** C++20, godot-cpp (Godot 4.7), GLSL through `RenderingDevice`, doctest (native tests), gdUnit4 (GPU/scene tests), SCons, GDScript.

**Spec:** `docs/superpowers/specs/2026-09-16-settings-store-design.md`. Roadmap: `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.2, §10.

## Global Constraints

- Branch: `feat/settings-store` (already checked out; spec committed).
- Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)`. A full C++ rebuild can take ~20 min; a one-file change relinks in minutes.
- Native tests: `cd extension && scons -Q test; cd ..`. One case: `extension/build/tests/ve_tests -tc="<name>"`.
- GPU tests: `./gdunit_tests.sh -a res://tests/<suite>.gd` (comma list allowed). Full run: `./gdunit_tests.sh`. Reports: `reports/report_N/results.xml`.
- Shaders load from disk at world init; a shader edit needs no rebuild. No task here edits a shader.
- **Baseline failures:** compare against `docs/superpowers/plans/2026-09-16-settings-store-baseline.md` (Task 1) by case name *and* message. A suite's case **count** dropping is itself a failure.
- **Golden policy:** a constant move (Tasks 10–13) must leave its golden unchanged. If a golden moves, stop the task and report; do not re-record.
- GPU timing *values* are invalid on this machine (`debug_gpu_timings()` returns -1); never pin them.
- **No new lock on the render thread.** `render/frame.cpp` keeps reading settings only through `beauty_settings()`, `grass_settings()`, `frame_settings()` and `island_slot_count()`. `rg 'settings_group|render_settings_|beauty_\.' extension/src/render/frame.cpp` must stay empty.
- **Store listeners run outside the store lock** on the mutating thread (main thread for every setter).
- **Beauty and grass remain separate structs and stores** (project memory: keep new features out of the beauty stack).
- **Bug fixes land in `fix:` commits** separate from any move, each after its failing test.
- C++ and GDScript indentation is tabs. Match surrounding comment density.
- Commit messages are plain conventional messages.

## Decided during planning (amends the spec)

Task 18 writes these into the spec.

1. **S1 test uses a chunk that is not resident when consolidation lands.** `ChunkResidency::update` never re-probes a resident chunk (`chunk_residency.cpp:181`), so a filled chunk that already has a collider keeps it. The bug shows when the chunk is first probed *after* the bake.
2. **S2 anchoring half is tested at the probe the refinement calls**, not end-to-end. `IslandManager` gains `contact_samples(cell, axis)`; `LogContactProbe` forwards to it and a new hook `debug_contact_samples` calls the same member. An end-to-end detach test would depend on flood windows and extraction timing that are not what S2 is about.
3. **The S6 failing test is written in Task 9, immediately before its fix**, rather than committed red in an earlier task (gdUnit has no expected-failure marker, and a red case would pollute every comparison in between).
4. **`ve_ambient` needs no listener.** `VoxelWorld::update_sun_state()` already runs every `_process` on the main thread and publishes `ve_ambient`; it reads `beauty_settings().ambient` instead of `DeferredPass::kAmbient`. Only the `render` store has a listener.
5. **`debug_beauty_settings` is built from the rows in Task 8**, together with the beauty store, using a shared `setting_to_variant` helper (`extension/src/settings/godot/setting_variant.{h,cpp}`).
6. **`VoxelSettings` uses stand-in stores instead of a pending buffer.** Before `_ready` resolves the world (and always in the editor) `render`/`beauty`/`grass` resolve to node-owned stand-in stores; `_ready` copies their overrides into the world's stores.
7. **The guard is testable:** `apply_config(args: PackedStringArray) -> bool` is bound and `_ready` calls it with `OS.get_cmdline_user_args()`. `save()` refuses after a measured `apply_config`.
8. **The panel addresses controls through `control(group, name)`**, not node paths.
9. **Picking a resolution while fullscreen changes nothing until fullscreen is off** (today's F7 left fullscreen first). `# ponytail:` comment in `apply_display`.
10. **Resolution table ordering and label checks are native** (`test_display_settings.cpp`); the panel test keeps "offers the project default" and "off-table size shown".

## File Map

| File | Status | Responsibility |
|---|---|---|
| `extension/src/settings/settings_table.h` | Create | `SettingKind`, `SettingValue`, `SettingRow<T>`, row builders, `read`/`write`/`clamp_all`, `RowInfo` |
| `extension/src/settings/settings_store.h` | Create | `SettingsGroup` interface, `SettingsStore<T>` |
| `extension/src/settings/render_settings.{h,cpp}` | Create | `RenderSettings`, `render_rows()`, `RenderSettingsStore` |
| `extension/src/settings/display_settings.{h,cpp}` | Create | `DisplaySettings`, `kResolutions`, `display_rows()`, `DisplaySettingsStore` |
| `extension/src/settings/godot/setting_variant.{h,cpp}` | Create | `SettingValue` ↔ `Variant` (godot-cpp; not in the native build) |
| `extension/src/shade/beauty_settings.{h,cpp}` | Modify | New look fields, `beauty_rows()`, `normalize_beauty`; clamp ranges move into rows |
| `extension/src/shade/beauty_settings_store.h` | Create | `BeautySettingsStore` |
| `extension/src/grass/grass_settings.{h,cpp}` | Modify | `grass_rows()`; clamp ranges move into rows |
| `extension/src/grass/grass_settings_store.h` | Modify | `GrassSettingsStore` becomes a `SettingsStore<GrassSettings>` |
| `extension/src/grass/grass_settings_store.cpp` | Delete | Replaced by the generic store |
| `extension/src/render/orchestrator.{h,cpp}` | Modify | Three stores, `settings_group()`, listener mirroring atomics; name chains deleted |
| `extension/src/render/{ssao,outline,contact_shadow}_pass.cpp` | Modify | Push look constants from `BeautySettings` |
| `extension/src/render/deferred_pass.h`, `render/frame.cpp` | Modify | Ambient from `BeautySettings`; `kAmbient` deleted |
| `extension/src/voxel_settings.{h,cpp}` | Create | `VoxelSettings` Node |
| `extension/src/register_types.cpp` | Modify | Register `VoxelSettings` |
| `extension/src/voxel_world.cpp` | Modify | Ambient publish from beauty; `extract_component` passes overrides; collider init passes volumes/overrides |
| `extension/src/mesh/mesh_chunk.{h,cpp}` | Modify | `chunk_has_surface(…, overrides)` (S1) |
| `extension/src/physics/collider_streamer.{h,cpp}` | Modify | `LogProbe` passes volumes and overrides (S1) |
| `extension/src/connectivity/contact_refine.{h,cpp}` | Modify | `contact_samples_field(…, overrides)` (S2) |
| `extension/src/generator/volume_set.{h,cpp}` | Modify | `extract_island_volume(…, overrides, …)` (S2) |
| `extension/src/physics/island_manager.{h,cpp}` | Modify | `contact_samples()`; `LogContactProbe` forwards (S2) |
| `extension/src/debug/hooks.{h,cpp}`, `hooks_physics.cpp`, `hooks_render.cpp` | Modify | `debug_contact_samples`; extract diff passes overrides; `debug_beauty_settings` from rows; probe ambient |
| `extension/SConstruct` | Modify | `src/settings/*.cpp` joins the native test build |
| `extension/tests/settings_row_checks.h` | Create | Row-table invariant checker shared by native tests |
| `extension/tests/test_settings_table.cpp` | Create | Table and store tests |
| `extension/tests/test_render_settings.cpp`, `test_display_settings.cpp` | Create | Group tables |
| `extension/tests/test_beauty_settings.cpp`, `test_grass_settings.cpp`, `test_mesh_chunk.cpp`, `test_contact_refine.cpp`, `test_volume_ops.cpp` | Modify | Pins, invariants, override cases, new argument |
| `demo/settings_menu.{gd,tscn}` | Rewrite | Unified F1 panel |
| `demo/debug_menu.gd` (+`.uid`) | Delete (Task 16) | Replaced by the panel |
| `demo/main.tscn`, `demo/help.gd`, `demo/benchmark.gd` | Modify | Wiring, help row, display dials through `VoxelSettings` |
| `tests/test_settings_names.gd` | Create | Characterization: every name round-trips through `VoxelWorld` |
| `tests/test_world_field_overrides.gd` | Create | S1/S2 behaviour after consolidation |
| `tests/test_voxel_settings.gd` | Create | `VoxelSettings` contract |
| `tests/test_settings_menu.gd` | Rewrite | Panel contract (ports `test_debug_menu.gd`) |
| `tests/test_debug_menu.gd` (+`.uid`) | Modify (Task 5), Delete (Task 16) | S5 test, then ported |
| `tests/test_beauty_settings.gd` | Modify | S6 tests |
| `docs/superpowers/plans/2026-09-16-settings-store-baseline.md` | Create | Baseline failure set |
| `docs/superpowers/plans/2026-09-16-settings-store-results.md` | Create | Exit evidence |
| `docs/superpowers/specs/2026-09-16-settings-store-design.md`, `2026-09-13-frame-module-design.md`, `plans/2026-09-13-frame-module.md` | Modify (Task 18) | Amendments and status rows |

---

### Task 1: Record the baseline failure set

No production change. Everything later compares against this file.

**Files:**
- Create: `docs/superpowers/plans/2026-09-16-settings-store-baseline.md`

**Interfaces:**
- Consumes: nothing.
- Produces: the baseline file.

- [ ] **Step 1: Build and run the native suite**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
```

Expected: build OK; record the doctest `test cases: N | N passed | M failed` line.

- [ ] **Step 2: Run the full gdUnit suite**

```bash
./gdunit_tests.sh
```

Expected: completes (a non-zero exit is normal).

- [ ] **Step 3: Extract per-suite counts and failing cases**

```bash
latest=$(ls -d reports/report_* | sort -V | tail -1)
python3 - "$latest/results.xml" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for suite in root.iter('testsuite'):
    print(f"# {suite.get('name')}: tests={suite.get('tests')} failures={suite.get('failures')}")
for tc in root.iter('testcase'):
    bad = tc.find('failure') if tc.find('failure') is not None else tc.find('error')
    if bad is not None:
        msg = (bad.get('message') or bad.text or '').strip().splitlines()
        print(f"{tc.get('classname')}::{tc.get('name')} — {msg[0] if msg else ''}")
EOF
```

- [ ] **Step 4: Write the baseline file**

Create `docs/superpowers/plans/2026-09-16-settings-store-baseline.md` with this shape, pasting the real output into each section:

```markdown
# Settings store — baseline

Commit: `<git rev-parse --short HEAD>`. Recorded <date> on <machine / GPU / Godot version>.
Report: `reports/report_<N>`.

## Native
<doctest summary line from Step 1>

## gdUnit per-suite counts
<every "# suite: tests=… failures=…" line from Step 3>

## gdUnit failing cases
<every "suite::case — message" line from Step 3, or "none">

Known flaky-by-case suites (project memory): test_connectivity, test_island_body — compare the
suite's failure COUNT, not the case name.
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/plans/2026-09-16-settings-store-baseline.md
git commit -m "docs: settings store baseline failure set"
```

---

### Task 2: Characterize today's settings

Pins every name and tier preset and proves the four look-constant goldens bite. No production change is committed.

**Files:**
- Modify: `extension/tests/test_beauty_settings.cpp`
- Modify: `extension/tests/test_grass_settings.cpp`
- Create: `tests/test_settings_names.gd`

**Interfaces:**
- Consumes: today's `ve::settings_for_tier`, `ve::GrassSettingsStore::set_value/value`, `VoxelWorld.set/get_effect_enabled`, `set/get_effect_value`, `set/get_grass_value`, `near_field_scale`, `quality_tier`.
- Produces: `check_same(const ve::BeautySettings &, const ve::BeautySettings &)` in `test_beauty_settings.cpp` (Tasks 10–13 add fields to it); `tests/test_settings_names.gd` (Task 8 adds int names).

- [ ] **Step 1: Pin every tier field by field**

Append to `extension/tests/test_beauty_settings.cpp`:

```cpp
namespace {

// Every field, so a tier preset that silently moves any knob fails here. A commit that adds a
// BeautySettings field adds it to this list.
void check_same(const ve::BeautySettings &got, const ve::BeautySettings &want) {
	CHECK(got.ssgi == want.ssgi);
	CHECK(got.ssr == want.ssr);
	CHECK(got.contact_shadows == want.contact_shadows);
	CHECK(got.outlines == want.outlines);
	CHECK(got.sun_shadow_map == want.sun_shadow_map);
	CHECK(got.glossy_sdf_rays == want.glossy_sdf_rays);
	CHECK(got.raymarched_sun_shadow == want.raymarched_sun_shadow);
	CHECK(got.cost_view == want.cost_view);
	CHECK(got.ssao == want.ssao);
	CHECK(got.ssgi_taps == want.ssgi_taps);
	CHECK(got.ssr_steps == want.ssr_steps);
	CHECK(got.contact_steps == want.contact_steps);
	CHECK(got.ssao_steps == want.ssao_steps);
	CHECK(got.ssao_directions == want.ssao_directions);
	CHECK(got.ssgi_radius == doctest::Approx(want.ssgi_radius));
	CHECK(got.ssgi_temporal == doctest::Approx(want.ssgi_temporal));
	CHECK(got.ssgi_strength == doctest::Approx(want.ssgi_strength));
	CHECK(got.emissive_gi_radius == doctest::Approx(want.emissive_gi_radius));
	CHECK(got.emissive_gi_strength == doctest::Approx(want.emissive_gi_strength));
	CHECK(got.outline_depth_threshold == doctest::Approx(want.outline_depth_threshold));
	CHECK(got.outline_normal_threshold == doctest::Approx(want.outline_normal_threshold));
}

} // namespace

TEST_CASE("every tier preset is pinned field by field") {
	const ve::BeautySettings high; // the struct defaults ARE High
	check_same(ve::settings_for_tier(ve::QualityTier::kHigh), high);

	ve::BeautySettings medium;
	medium.glossy_sdf_rays = false;
	medium.emissive_gi_radius = 24.0f;
	medium.ssgi_taps = 4;
	medium.ssr_steps = 12;
	medium.contact_steps = 8;
	medium.ssao_steps = 4;
	medium.ssao_directions = 4;
	check_same(ve::settings_for_tier(ve::QualityTier::kMedium), medium);

	ve::BeautySettings low;
	low.ssgi = low.ssr = low.contact_shadows = false;
	low.glossy_sdf_rays = false;
	low.ssao = false;
	low.ssgi_taps = low.ssr_steps = low.contact_steps = 0;
	low.ssao_steps = low.ssao_directions = 0;
	check_same(ve::settings_for_tier(ve::QualityTier::kLow), low);

	ve::BeautySettings off = low;
	off.outlines = off.sun_shadow_map = off.raymarched_sun_shadow = false;
	check_same(ve::settings_for_tier(ve::QualityTier::kOff), off);
}
```

- [ ] **Step 2: Pin every grass name**

Append to `extension/tests/test_grass_settings.cpp`:

```cpp
// Every name the store accepts today, each with an in-range value that differs from its
// default. The generic store (settings-store plan Task 7) must keep all of them.
TEST_CASE("every grass knob name round-trips through the store") {
	const struct {
		const char *name;
		float v;
	} knobs[] = {
		{"enabled", 0.0f}, {"reach_m", 25.0f}, {"vertical_reach_m", 5.0f},
		{"blades_per_brick", 8.0f}, {"max_blades", 1000.0f}, {"blade_width_m", 0.05f},
		{"blade_height_m", 0.5f}, {"height_jitter", 0.2f}, {"slope_cos_min", 0.3f},
		{"wind_strength", 0.2f}, {"wind_speed", 1.0f}, {"wind_scale", 0.1f},
		{"wind_dir_deg", 90.0f}, {"lean_spread_rad", 1.0f}, {"base_curve", 0.3f},
		{"camera_tilt", 0.5f}, {"ring_width_gain", 2.0f}, {"flower_chance", 0.05f},
		{"gloss", 0.5f}, {"blade_lighting", 0.3f},
	};
	for (const auto &k : knobs) {
		CAPTURE(k.name);
		ve::GrassSettingsStore store;
		CHECK(store.set_value(k.name, k.v));
		CHECK(store.value(k.name) == doctest::Approx(k.v));
	}
}
```

- [ ] **Step 3: Run the native suite**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS, including `every tier preset is pinned field by field` and `every grass knob name round-trips through the store`.

- [ ] **Step 4: Write the GDScript name characterization**

Create `tests/test_settings_names.gd`:

```gdscript
extends GdUnitTestSuite
# Characterization for docs/superpowers/plans/2026-09-16-settings-store.md (Task 2): every name
# VoxelWorld's settings API accepts today, round-tripped through that API. The store migration
# must keep every name and its meaning; this suite stays green through the whole plan.

const BEAUTY_SWITCHES := ["ssgi", "ssr", "contact_shadows", "outlines", "sun_shadow_map",
	"glossy_sdf_rays", "raymarched_sun_shadow", "ssao", "cost_view"]
# [name, an in-range value that differs from the High default]
const BEAUTY_MAGNITUDES := [["ssgi_radius", 12.0], ["ssgi_temporal", 0.5], ["ssgi_strength", 2.0],
	["emissive_gi_radius", 32.0], ["emissive_gi_strength", 3.0],
	["outline_depth_threshold", 0.1], ["outline_normal_threshold", 0.5]]
const GRASS := [["enabled", 0.0], ["reach_m", 25.0], ["vertical_reach_m", 5.0],
	["blades_per_brick", 8.0], ["max_blades", 1000.0], ["blade_width_m", 0.05],
	["blade_height_m", 0.5], ["height_jitter", 0.2], ["slope_cos_min", 0.3],
	["wind_strength", 0.2], ["wind_speed", 1.0], ["wind_scale", 0.1], ["wind_dir_deg", 90.0],
	["lean_spread_rad", 1.0], ["base_curve", 0.3], ["camera_tilt", 0.5],
	["ring_width_gain", 2.0], ["flower_chance", 0.05], ["gloss", 0.5], ["blade_lighting", 0.3]]

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
	add_child(w)
	_worlds.append(w)
	return w

func test_every_beauty_switch_round_trips_by_name() -> void:
	var w := make_world()
	for name in BEAUTY_SWITCHES:
		var before: bool = w.get_effect_enabled(name)
		w.set_effect_enabled(name, not before)
		assert_bool(w.get_effect_enabled(name)).override_failure_message(name).is_equal(not before)

func test_every_beauty_magnitude_round_trips_by_name() -> void:
	var w := make_world()
	for entry in BEAUTY_MAGNITUDES:
		w.set_effect_value(entry[0], entry[1])
		assert_float(w.get_effect_value(entry[0])).override_failure_message(entry[0]) \
			.is_equal_approx(entry[1], 0.0001)

func test_the_render_dials_round_trip() -> void:
	var w := make_world()
	for name in ["islands", "near_field"]:
		w.set_effect_enabled(name, false)
		assert_bool(w.get_effect_enabled(name)).override_failure_message(name).is_false()
	w.near_field_scale = 0.55
	assert_float(w.near_field_scale).is_equal_approx(0.55, 0.0001)
	w.quality_tier = 1
	assert_int(w.quality_tier).is_equal(1)

func test_every_grass_knob_round_trips_by_name() -> void:
	var w := make_world()
	for entry in GRASS:
		assert_bool(w.set_grass_value(entry[0], entry[1])).override_failure_message(entry[0]).is_true()
		assert_float(w.get_grass_value(entry[0])).override_failure_message(entry[0]) \
			.is_equal_approx(entry[1], 0.0001)
```

- [ ] **Step 5: Run it**

Run: `./gdunit_tests.sh -a res://tests/test_settings_names.gd`
Expected: 4 cases PASS.

- [ ] **Step 6: Prove each look-constant golden bites (temporary edits, never committed)**

Do these one at a time. Each is a single-file change, so the rebuild is incremental.

1. In `extension/src/render/ssao_pass.cpp` change `kSsaoRadius = 5.0f` to `6.0f`; build; run `./gdunit_tests.sh -a res://tests/test_ssao_golden.gd`. Expected: FAIL (`min_ao moved` or `lit_luma moved`). Revert: `git checkout extension/src/render/ssao_pass.cpp`.
2. In `extension/src/render/outline_pass.cpp` change the push's `0.35f` to `0.5f`; build; run `./gdunit_tests.sh -a res://tests/test_outline.gd`. Expected: FAIL in `test_depth_line_is_one_pixel_and_darkens_by_the_fixed_amount`. Revert.
3. In `extension/src/render/contact_shadow_pass.cpp` change `{0.6f, 0.85f, 0.05f, 0.0f}` to `{0.6f, 0.5f, 0.05f, 0.0f}`; build; run `./gdunit_tests.sh -a res://tests/test_contact_shadow_golden.gd`. Expected: FAIL. Revert.
4. In `extension/src/render/deferred_pass.h` change `kAmbient` to `{0.30f, 0.19f, 0.26f}`; build; run `./gdunit_tests.sh -a res://tests/test_deferred_golden.gd`. Expected: FAIL. Revert.

Rebuild once more after the last revert and confirm `git status` shows only the three test files from Steps 1, 2 and 4. If any suite PASSES with its edit, stop and report: that constant has no guard, and a golden case must be added before Task 10–13 moves it.

- [ ] **Step 7: Commit**

```bash
git add extension/tests/test_beauty_settings.cpp extension/tests/test_grass_settings.cpp tests/test_settings_names.gd
git commit -m "test: characterize settings names, tier presets and look-constant goldens"
```

Put the four bite results (suite, failing assertion) in the commit body.

---

### Task 3: S1 — the collider probe reads volumes and overrides

**Files:**
- Create: `tests/test_world_field_overrides.gd`
- Modify: `extension/src/mesh/mesh_chunk.h:65-66`, `extension/src/mesh/mesh_chunk.cpp:57-73`
- Modify: `extension/src/physics/collider_streamer.h:34-35` and its private members, `extension/src/physics/collider_streamer.cpp:32-42, 45-52, 577-579`
- Modify: `extension/src/voxel_world.cpp:762-763`
- Test: `extension/tests/test_mesh_chunk.cpp`

**Interfaces:**
- Consumes: hooks `debug_init_physics`, `debug_stream_region`, `debug_consolidate_region`, `debug_region_op_count`, `debug_physics_frame`, `debug_physics_stats`, `debug_chunk_collider_info`, `debug_store_volume`, `debug_apply_volume_add`.
- Produces:
  - `bool ve::chunk_has_surface(const Generator &, const EditOp *, int, IVec3, const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr)`
  - `void ColliderStreamer::initialize(ve::ChunkResidency *, ve::EditLog *, std::mutex *, MeshService *, int max_slots, const ve::Generator *, const ve::VolumeStore *volumes, const ve::OverrideSource *overrides)`
  - `tests/test_world_field_overrides.gd` with helpers `make_physics_world()`, `make_extract_world()`, `settle()` (Task 4 appends to it).

- [ ] **Step 1: Write the failing gdUnit tests**

Create `tests/test_world_field_overrides.gd`:

```gdscript
extends GdUnitTestSuite
# S1 and S2 (docs/superpowers/specs/2026-09-13-frame-module-design.md §10): CPU probes of the
# world field must read what consolidation baked, not only the region's op list. Consolidation
# bakes a region's ops into override bricks and clears the list, so a probe that ignores
# overrides sees the terrain as it was before the edit.

const PHYSICS_CENTER := Vector3(60.0, 55.0, 60.0)
# Open sky inside the physics ball: the terrain tops out at 51.2 + 10 m.
const FILL_AT := Vector3(60.8, 67.2, 60.8)
const FILL_CHUNK := Vector3i(9, 10, 9)   # floor(FILL_AT / 6.4)
const FILL_REGION := Vector3i(2, 2, 2)   # floor(FILL_AT / 25.6)

# A 16^3 ball at 5 cm, placed so the chunk probe's 1.6 m lattice point (60.8, 68.8, 60.8) lies
# inside it: chunk (9,10,9) starts at (57.6, 64.0, 57.6).
const VDIM := 16
const VVOXEL := 0.05
const VORIGIN := Vector3(60.6, 68.4, 60.6)
const VRADIUS := 0.35
const VMATERIAL := 2
const VSLOT := 7

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_physics_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 512
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# Same contract as tests/test_collider_edits.gd: the streamer owes nothing.
func settle(w: VoxelWorld, center: Vector3, frames := 6000) -> bool:
	var quiet := 0
	for i in range(frames):
		w.hooks().debug_physics_frame(center)
		var st := w.hooks().debug_physics_stats()
		quiet = quiet + 1 if st["chunks_pending"] == 0 and st["queued"] == 0 else 0
		if quiet >= 4:
			return true
		OS.delay_msec(1)
	return false

func encode_sdf(d: float) -> int:
	var t := clampf((d + 0.64) / 1.28, 0.0, 1.0)
	return int(floor(t * 255.0 + 0.5))

func ball_volume() -> Array:
	var sdf := PackedByteArray()
	var mat := PackedByteArray()
	sdf.resize(VDIM * VDIM * VDIM)
	mat.resize(VDIM * VDIM * VDIM)
	var c := 0.5 * float(VDIM - 1) * VVOXEL
	for z in range(VDIM):
		for y in range(VDIM):
			for x in range(VDIM):
				var d := (Vector3(x, y, z) * VVOXEL - Vector3(c, c, c)).length() - VRADIUS
				var i := x + y * VDIM + z * VDIM * VDIM
				sdf[i] = encode_sdf(d)
				mat[i] = VMATERIAL if d <= 0.0 else 0
	return [sdf, mat]

# S1, overrides. ChunkResidency never re-probes a chunk that is already resident, so the chunk
# must be probed for the first time AFTER the bake: the physics streamer has not run yet here.
func test_a_consolidated_fill_in_open_sky_still_gets_a_collider(timeout := 180000) -> void:
	var w := make_physics_world()
	w.hooks().debug_stream_region(FILL_REGION)
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	var r: Dictionary = tool.apply_sphere_add(FILL_AT, 2.0, 4)
	assert_array(r["rejected"]).is_empty()
	assert_bool(w.hooks().debug_consolidate_region(FILL_REGION)).is_true()
	assert_int(w.hooks().debug_region_op_count(FILL_REGION)).is_equal(0)
	assert_bool(settle(w, PHYSICS_CENTER)).is_true()
	var info: Dictionary = w.hooks().debug_chunk_collider_info(FILL_CHUNK)
	assert_int(int(info.get("slot", -1))).override_failure_message(
		"the filled chunk probed as empty once its fill was baked: %s" % info).is_greater_equal(0)

# S1, volumes. A pasted volume is an op naming a volume slot; without the volume store the
# probe evaluates it as nothing.
func test_a_pasted_volume_in_open_sky_gets_a_collider(timeout := 180000) -> void:
	var w := make_physics_world()
	var ball := ball_volume()
	w.hooks().debug_store_volume(VSLOT, ball[0], ball[1], VDIM)
	w.hooks().debug_apply_volume_add(VSLOT, VORIGIN, VVOXEL, VDIM)
	assert_bool(settle(w, PHYSICS_CENTER)).is_true()
	var info: Dictionary = w.hooks().debug_chunk_collider_info(FILL_CHUNK)
	assert_int(int(info.get("slot", -1))).override_failure_message(
		"the chunk holding only a pasted volume probed as empty: %s" % info).is_greater_equal(0)
```

- [ ] **Step 2: Run them to verify they fail for the right reason**

Run: `./gdunit_tests.sh -a res://tests/test_world_field_overrides.gd`
Expected: both FAIL at the final assertion with `probed as empty` (slot `-1`). Every earlier assertion must PASS. If an earlier one fails (consolidation refused, settle timed out), fix the test setup, not production code.

- [ ] **Step 3: Write the native test for the pure probe**

Append to `extension/tests/test_mesh_chunk.cpp` (add `#include "world/override_store.h"` at the top):

```cpp
// S1: a baked override replaces the generator base, so a chunk whose only surface lives in
// override bricks must still probe as surface. The probe lattice's 1.6 m pitch lands on brick
// boundaries, so bricks on both sides of the probe point (4.8, 80.0, 4.8) are baked solid.
TEST_CASE("chunk_has_surface reads baked overrides as the base field") {
	const ve::AnalyticGenerator gen;
	const ve::IVec3 sky{0, 12, 0}; // y 76.8 .. 83.2
	CHECK_FALSE(ve::chunk_has_surface(gen, nullptr, 0, sky));
	ve::OverrideStore overrides(8);
	for (int z = 5; z <= 6; z++)
		for (int y = 99; y <= 100; y++)
			for (int x = 5; x <= 6; x++) {
				ve::OverrideBrick *b = overrides.data(overrides.acquire({x, y, z}));
				REQUIRE(b != nullptr);
				for (uint8_t &s : b->sdf) s = ve::encode_sdf(-1.0f);
			}
	CHECK(ve::chunk_has_surface(gen, nullptr, 0, sky, nullptr, &overrides));
}
```

- [ ] **Step 4: Add the override parameter to `chunk_has_surface`**

In `extension/src/mesh/mesh_chunk.h`, add `struct OverrideSource;` inside `namespace ve` above the declaration, and change the declaration to:

```cpp
bool chunk_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 chunk,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr);
```

In `extension/src/mesh/mesh_chunk.cpp`, change the definition's signature the same way (no defaults) and the sample line to:

```cpp
				const float d = eval_field(gen, ops, op_count, o[0] + sx * step,
						o[1] + sy * step, o[2] + sz * step, volumes, overrides).sdf;
```

- [ ] **Step 5: Run the native test**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS, including `chunk_has_surface reads baked overrides as the base field`.

- [ ] **Step 6: Thread volumes and overrides into `LogProbe`**

In `extension/src/physics/collider_streamer.h`, add `namespace ve { struct OverrideSource; }` above `namespace godot {`, change `initialize` to:

```cpp
	void initialize(ve::ChunkResidency *chunks, ve::EditLog *edit_log, std::mutex *edit_mutex,
			MeshService *mesh, int max_slots, const ve::Generator *gen,
			const ve::VolumeStore *volumes, const ve::OverrideSource *overrides);
```

and beside `const ve::Generator *gen_ = nullptr;` add:

```cpp
	// Borrowed like gen_: the pasted volumes and the consolidated override bricks are part of
	// the field the probe must see (S1). Both are read under edit_mutex_, the lock
	// consolidation holds while it writes overrides.
	const ve::VolumeStore *volumes_ = nullptr;
	const ve::OverrideSource *overrides_ = nullptr;
```

In `extension/src/physics/collider_streamer.cpp`, replace the `LogProbe` struct with:

```cpp
// The residency's view of the world field: generator, region ops, pasted volumes and baked
// overrides -- everything eval_field takes, so a consolidated or pasted chunk is not probed
// as empty (S1).
struct LogProbe : ve::ChunkProbe {
	const ve::Generator *gen = nullptr;
	ve::EditLog *log = nullptr;
	std::mutex *mu = nullptr;
	const ve::VolumeStore *volumes = nullptr;
	const ve::OverrideSource *overrides = nullptr;

	bool chunk_has_surface(ve::IVec3 c) const override {
		std::lock_guard<std::mutex> lock(*mu);
		const std::vector<ve::EditOp> &ops = log->ops(ve::region_of_chunk(c));
		return ve::chunk_has_surface(*gen, ops.data(), static_cast<int>(ops.size()), c,
				volumes, overrides);
	}
};
```

Change the `initialize` definition's signature to match the header and add after `gen_ = gen;`:

```cpp
	volumes_ = volumes;
	overrides_ = overrides;
```

Where the plan builds the probe (`LogProbe probe;` near line 577), add after `probe.mu = edit_mutex_;`:

```cpp
	probe.volumes = volumes_;
	probe.overrides = overrides_;
```

In `extension/src/voxel_world.cpp`, the collider initialisation becomes:

```cpp
	colliders_->initialize(chunks_, store_->edit_log(), &store_->edit_mutex(), mesh_, max_collider_chunks_,
			&store_->generator()->sampler(), &store_->volumes(), store_->overrides());
```

(`ensure_physics_initialized` calls `store_->ensure_overrides` earlier in the same function, and the store never replaces that object, so the pointer is live for the streamer's lifetime.)

- [ ] **Step 7: Build and run the tests**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_overrides.gd,res://tests/test_collider_edits.gd,res://tests/test_collider_stream.gd,res://tests/test_collider_octants.gd
```

Expected: every case PASSES.

- [ ] **Step 8: Commit**

```bash
git add tests/test_world_field_overrides.gd extension/tests/test_mesh_chunk.cpp extension/src/mesh/mesh_chunk.h extension/src/mesh/mesh_chunk.cpp extension/src/physics/collider_streamer.h extension/src/physics/collider_streamer.cpp extension/src/voxel_world.cpp
git commit -m "fix: collider residency probe reads volumes and consolidated overrides (S1)

A chunk filled in open sky and then consolidated, or holding only a pasted
volume, probed as surface-free because LogProbe evaluated the generator and
the region's op list only. chunk_has_surface now takes the override source
and the probe passes both the volume store and the overrides."
```

---

### Task 4: S2 — the contact probe and the CPU island extract read overrides

**Files:**
- Modify: `tests/test_world_field_overrides.gd`
- Modify: `extension/src/connectivity/contact_refine.h:60-61`, `extension/src/connectivity/contact_refine.cpp:117-137`
- Modify: `extension/src/generator/volume_set.h:83-85`, `extension/src/generator/volume_set.cpp:370-430`
- Modify: `extension/src/physics/island_manager.h` (public section), `extension/src/physics/island_manager.cpp:82-97, 253-258`
- Modify: `extension/src/debug/hooks.h`, `extension/src/debug/hooks.cpp` (bindings), `extension/src/debug/hooks_physics.cpp:628-630`
- Modify: `extension/src/voxel_world.cpp:917-919`
- Test: `extension/tests/test_contact_refine.cpp`, `extension/tests/test_volume_ops.cpp:714, 750, 765, 814`

**Interfaces:**
- Consumes: `make_extract_world()` pattern from Task 3's suite; hooks `debug_island_extract_diff`, `debug_consolidate_region`, `debug_region_op_count`.
- Produces:
  - `int ve::contact_samples_field(const Generator &, const EditOp *, int, IVec3 cell, int axis, int face_samples, const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr)`
  - `void ve::extract_island_volume(const Generator &, const EditOp *, int, const VolumeStore *volumes, const OverrideSource *overrides, const float origin[3], float voxel, int dim, const float *box_aabbs, int box_count, VolumeData *out)`
  - `int IslandManager::contact_samples(ve::IVec3 cell, int axis) const`
  - hook `int debug_contact_samples(Vector3i cell, int axis)` (returns -1 without an island manager)

- [ ] **Step 1: Add the hook, unchanged behaviour, so the failing test can run**

The test needs a way to ask the shipped probe a question. Move the probe body into `IslandManager` first, still without overrides.

In `extension/src/physics/island_manager.h`, in the public section, add:

```cpp
	// Solid samples on the face between `cell` and `cell + e_axis`, under the edit lock --
	// exactly what connectivity's marginal-contact refinement asks (LogContactProbe forwards
	// here) and what debug_contact_samples reports.
	int contact_samples(ve::IVec3 cell, int axis) const;
```

In `extension/src/physics/island_manager.cpp`, replace `struct LogContactProbe` with:

```cpp
// The residency's view of the world field, for ve::refine_anchoring. The lock is taken per
// call rather than held, exactly as ColliderStreamer::LogProbe does, so an edit landing
// mid-refinement waits rather than deadlocks.
struct LogContactProbe : ve::ContactProbe {
	const IslandManager *manager = nullptr;

	int contact_samples(ve::IVec3 cell, int axis) const override {
		return manager->contact_samples(cell, axis);
	}
};
```

After `IslandManager::~IslandManager()`, add:

```cpp
int IslandManager::contact_samples(ve::IVec3 cell, int axis) const {
	if (gen_ == nullptr || !handles_.store || !handles_.store->edit_log()) return 0;
	std::lock_guard<std::mutex> lock(handles_.store->edit_mutex());
	const std::vector<ve::EditOp> &ops = handles_.store->edit_log()->ops(ve::region_of_brick(cell));
	return ve::contact_samples_field(*gen_, ops.data(), static_cast<int>(ops.size()), cell, axis,
			refine_cfg_.face_samples, &handles_.store->volumes());
}
```

In `run_connectivity`, replace the six probe-setup lines (`LogContactProbe probe;` through `probe.face_samples = …;`) with:

```cpp
	LogContactProbe probe;
	probe.manager = this;
```

In `extension/src/debug/hooks.h`, beside `debug_island_stats`, declare `int debug_contact_samples(Vector3i cell, int axis);`. In `extension/src/debug/hooks.cpp`, beside the `debug_island_extract_diff` binding, add:

```cpp
	ClassDB::bind_method(D_METHOD("debug_contact_samples", "cell", "axis"), &VoxelDebugHooks::debug_contact_samples);
```

In `extension/src/debug/hooks_physics.cpp`, before `debug_island_extract_diff`, add:

```cpp
// The shipped marginal-contact probe, asked directly: IslandManager::contact_samples is the
// member LogContactProbe forwards to, so this is not a re-implementation.
int VoxelDebugHooks::debug_contact_samples(Vector3i cell, int axis) {
	world_->ensure_physics_initialized();
	if (!world_->island_manager()) return -1;
	return world_->island_manager()->contact_samples({cell.x, cell.y, cell.z}, axis);
}
```

- [ ] **Step 2: Write the failing gdUnit tests**

Append to `tests/test_world_field_overrides.gd`:

```gdscript
const CARVE_REGION := Vector3i(0, 0, 0)

func make_extract_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_stream_region(CARVE_REGION)
	return w

# S2, contact probe. The face between cells (10,20,20) and (10,21,20) is the plane y = 16.8 m,
# x in [8, 8.8], z in [16, 16.8]: solid rock, all 81 samples.
func test_the_contact_probe_reads_a_consolidated_carve(timeout := 120000) -> void:
	var w := make_extract_world()
	assert_int(w.hooks().debug_contact_samples(Vector3i(10, 20, 20), 1)).is_equal(81)
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	tool.apply_sphere_subtract(Vector3(8.4, 16.8, 16.4), 1.0)
	assert_int(w.hooks().debug_contact_samples(Vector3i(10, 20, 20), 1)).is_equal(0)
	assert_bool(w.hooks().debug_consolidate_region(CARVE_REGION)).is_true()
	assert_int(w.hooks().debug_region_op_count(CARVE_REGION)).is_equal(0)
	assert_int(w.hooks().debug_contact_samples(Vector3i(10, 20, 20), 1)).override_failure_message(
		"the contact probe sees uncarved rock once the carve is baked").is_equal(0)

# S2, CPU island extract. The GPU extraction samples a snapshot that includes overrides; the
# CPU reference must agree after the carve moved into them.
func test_the_cpu_island_extract_reads_a_consolidated_carve(timeout := 120000) -> void:
	var w := make_extract_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	tool.apply_sphere_subtract(Vector3(8.4, 16.4, 16.4), 0.6)
	assert_bool(w.hooks().debug_consolidate_region(CARVE_REGION)).is_true()
	assert_int(w.hooks().debug_region_op_count(CARVE_REGION)).is_equal(0)
	var d: Dictionary = w.hooks().debug_island_extract_diff(Vector3i(10, 20, 20), Vector3i(11, 20, 20))
	assert_bool(d.get("ok", false)).override_failure_message("extraction failed: %s" % d).is_true()
	assert_int(d["worst_steps"]).override_failure_message(
		"CPU and GPU extraction disagree after consolidation: worst %d steps" % d["worst_steps"]
		).is_less(2)
	assert_int(d["mat_mismatch"]).is_equal(0)
```

- [ ] **Step 3: Build and verify they fail for the right reason**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_world_field_overrides.gd
```

Expected: the two Task 3 cases PASS; `test_the_contact_probe_reads_a_consolidated_carve` FAILS only at the last assertion (81, not 0); `test_the_cpu_island_extract_reads_a_consolidated_carve` FAILS at `worst_steps`. Commit this intermediate state (the hook refactor is behaviour-preserving):

```bash
./gdunit_tests.sh -a res://tests/test_connectivity.gd
git add tests/test_world_field_overrides.gd extension/src/physics/island_manager.h extension/src/physics/island_manager.cpp extension/src/debug/hooks.h extension/src/debug/hooks.cpp extension/src/debug/hooks_physics.cpp
git commit -m "test: S2 contact probe and CPU island extract after consolidation (failing)"
```

`test_connectivity.gd` must match the baseline before this commit.

- [ ] **Step 4: Write the native test for the pure contact sampler**

Append to `extension/tests/test_contact_refine.cpp` (add `#include "world/override_store.h"`):

```cpp
// S2: a baked override replaces the generator base on the contact face too. The face of cell
// (10,79,10) along +y is the plane y = 64.0 m in open sky; bricks on both sides of it are
// baked solid, so every sample is solid.
TEST_CASE("contact_samples_field reads baked overrides as the base field") {
	AnalyticGenerator gen;
	CHECK(contact_samples_field(gen, nullptr, 0, {10, 79, 10}, 1, 9) == 0);
	OverrideStore overrides(2);
	for (int y = 79; y <= 80; y++) {
		OverrideBrick *b = overrides.data(overrides.acquire({10, y, 10}));
		REQUIRE(b != nullptr);
		for (uint8_t &s : b->sdf) s = encode_sdf(-1.0f);
	}
	CHECK(contact_samples_field(gen, nullptr, 0, {10, 79, 10}, 1, 9, nullptr, &overrides) == 81);
}
```

- [ ] **Step 5: Add the override parameter to the two pure functions**

`extension/src/connectivity/contact_refine.h`: add `struct OverrideSource;` inside `namespace ve` and change the declaration to:

```cpp
int contact_samples_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		int axis, int face_samples, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr);
```

`extension/src/connectivity/contact_refine.cpp`: same signature without defaults; the sample becomes `eval_field(gen, ops, op_count, p[0], p[1], p[2], volumes, overrides)`.

`extension/src/generator/volume_set.h`: add `struct OverrideSource;` inside `namespace ve` and change the declaration to:

```cpp
void extract_island_volume(const Generator &gen, const EditOp *ops, int op_count,
		const VolumeStore *volumes, const OverrideSource *overrides, const float origin[3],
		float voxel, int dim, const float *box_aabbs, int box_count, VolumeData *out);
```

`extension/src/generator/volume_set.cpp`: same signature; inside the body pass `overrides` to all three field calls:

```cpp
		const Sample s = eval_field(gen, ops, op_count, p[0], p[1], p[2], volumes, overrides);
```
```cpp
						material = eval_field(gen, ops, op_count, p[0] - g[0] / len * t,
								p[1] - g[1] / len * t, p[2] - g[2] / len * t, volumes, overrides)
										   .material;
```
```cpp
				FieldSample gs = eval_field_gradient(gen, ops, op_count, p[0], p[1], p[2], volumes, overrides);
```

`extension/tests/test_volume_ops.cpp`: in the four `extract_island_volume(gen, nullptr, 0, nullptr, origin, …)` calls insert one more `nullptr` after the volumes argument: `extract_island_volume(gen, nullptr, 0, nullptr, nullptr, origin, …)`.

- [ ] **Step 6: Wire the overrides at every caller**

`extension/src/physics/island_manager.cpp`, the `contact_samples` body's call becomes:

```cpp
	return ve::contact_samples_field(*gen_, ops.data(), static_cast<int>(ops.size()), cell, axis,
			refine_cfg_.face_samples, &handles_.store->volumes(), handles_.store->overrides());
```

`extension/src/debug/hooks_physics.cpp` (`debug_island_extract_diff`):

```cpp
	ve::extract_island_volume(gen, job.ops.data(), static_cast<int>(job.ops.size()),
			&world_->context().store->volumes(), world_->context().store->overrides(), job.origin,
			job.voxel, job.dim, aabbs.data(), static_cast<int>(boxes.size()), &cpu);
```

`extension/src/voxel_world.cpp` (`extract_component`):

```cpp
	ve::extract_island_volume(gen, job->ops.data(), static_cast<int>(job->ops.size()),
			&store_->volumes(), store_->overrides(), job->origin, job->voxel, job->dim, aabbs.data(),
			static_cast<int>(boxes->size()), &cpu);
```

- [ ] **Step 7: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_world_field_overrides.gd,res://tests/test_island_extract.gd,res://tests/test_connectivity.gd,res://tests/test_island_body.gd
```

Expected: native PASS; all four `test_world_field_overrides` cases PASS; `test_island_extract.gd` PASS (unconsolidated regions have no override bricks, so `eval_field` takes the same path); `test_connectivity.gd` and `test_island_body.gd` match the baseline.

- [ ] **Step 8: Commit**

```bash
git add extension/src/connectivity/contact_refine.h extension/src/connectivity/contact_refine.cpp extension/src/generator/volume_set.h extension/src/generator/volume_set.cpp extension/src/physics/island_manager.cpp extension/src/debug/hooks_physics.cpp extension/src/voxel_world.cpp extension/tests/test_contact_refine.cpp extension/tests/test_volume_ops.cpp
git commit -m "fix: contact probe and CPU island extract read consolidated overrides (S2)

Marginal-contact refinement judged a consolidated carve by the uncarved
terrain, and the CPU island extraction disagreed with the GPU one once a
carve moved into override bricks. contact_samples_field and
extract_island_volume now take the override source; IslandManager and both
extraction callers pass the store's overrides under the edit lock."
```

---

### Task 5: S5 — debug menu slider ranges contain the shipped value

**Files:**
- Modify: `tests/test_debug_menu.gd`
- Modify: `demo/debug_menu.gd:38`

**Interfaces:**
- Consumes: `debug_menu.gd` constants `VALUES`, `GRASS_VALUES`; `VoxelWorld.get/set_effect_value`, `get/set_grass_value`.
- Produces: nothing new (the menu is replaced in Task 16; this case is ported to `test_settings_menu.gd` and the native row invariant).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_debug_menu.gd`:

```gdscript
# S5: a slider whose range excludes the knob's shipped value clamps the knob to the slider the
# first time it is dragged (blade width ships at 0.08 against a 0.06 slider), and a slider wider
# than the C++ clamp offers values the store silently refuses. The clamp is discovered on a
# separate world so the world under test is only read.
func assert_slider_range(name: String, ui_lo: float, ui_hi: float, shipped: float,
		lo: float, hi: float) -> void:
	assert_float(shipped).override_failure_message(
		"%s: shipped %f outside the slider [%f, %f]" % [name, shipped, ui_lo, ui_hi]
		).is_between(ui_lo, ui_hi)
	assert_float(ui_lo).override_failure_message(
		"%s: slider min %f below the C++ clamp %f" % [name, ui_lo, lo]).is_greater_equal(lo)
	assert_float(ui_hi).override_failure_message(
		"%s: slider max %f above the C++ clamp %f" % [name, ui_hi, hi]).is_less_equal(hi)

func test_every_slider_range_contains_the_shipped_value_and_sits_inside_the_clamp() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var probe: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	probe.use_local_device = true
	probe.physics_enabled = false
	pair[1].get_parent().add_child(probe)
	for entry in MENU_SCRIPT.VALUES:
		probe.set_effect_value(entry[1], 1e9)
		var hi: float = probe.get_effect_value(entry[1])
		probe.set_effect_value(entry[1], -1e9)
		var lo: float = probe.get_effect_value(entry[1])
		assert_slider_range(entry[1], entry[2], entry[3], world.get_effect_value(entry[1]), lo, hi)
	for entry in MENU_SCRIPT.GRASS_VALUES:
		probe.set_grass_value(entry[1], 1e9)
		var hi: float = probe.get_grass_value(entry[1])
		probe.set_grass_value(entry[1], -1e9)
		var lo: float = probe.get_grass_value(entry[1])
		assert_slider_range(entry[1], entry[2], entry[3], world.get_grass_value(entry[1]), lo, hi)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_debug_menu.gd`
Expected: the new case FAILS with `blade_width_m: shipped 0.080000 outside the slider [0.000000, 0.060000]`; the other 8 cases PASS.

- [ ] **Step 3: Fix the range**

In `demo/debug_menu.gd`, the blade width row becomes:

```gdscript
	["Blade width", "blade_width_m", 0.0, 0.2, 0.005],
```

- [ ] **Step 4: Run it to verify it passes**

Run: `./gdunit_tests.sh -a res://tests/test_debug_menu.gd`
Expected: 9 cases PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/test_debug_menu.gd demo/debug_menu.gd
git commit -m "fix: blade width slider contains the shipped 0.08 m (S5)

The debug menu capped blade width at 0.06 m while GrassSettings ships 0.08 m,
so the first drag wrote a thinner blade than the default. The new test checks
every slider range contains its knob's shipped value and sits inside the C++
clamp."
```

---

### Task 6: The settings table and store

Pure, header-only, native-tested. Nothing uses it yet.

**Files:**
- Create: `extension/src/settings/settings_table.h`
- Create: `extension/src/settings/settings_store.h`
- Create: `extension/tests/settings_row_checks.h`
- Create: `extension/tests/test_settings_table.cpp`
- Modify: `extension/SConstruct:29-33`

**Interfaces:**
- Consumes: nothing.
- Produces (namespace `ve`):
  - `enum class SettingKind : uint8_t { kBool, kInt, kFloat, kColor, kEnum }`
  - `struct SettingValue { SettingKind kind; float v[3]; static of_bool(bool), of_int(int), of_enum(int), of_float(float), of_color(float, float, float); }`
  - `template <class T> struct SettingRow { name, label, kind, b, i, f, c, min, max, ui_min, ui_max, step, options, hint }`
  - Row builders: `bool_row(name, label, bool T::*, hint = nullptr)`, `int_row(name, label, int T::*, int min, int max, int ui_min, int ui_max, int step = 1, hint = nullptr)`, `float_row(name, label, float T::*, float min, float max, float ui_min, float ui_max, float step, hint = nullptr)`, `color_row(name, label, float (T::*)[3], float min, float max, float ui_min, float ui_max, float step, hint = nullptr)`, `enum_row(name, label, int T::*, std::span<const char *const> options, int min = 0, hint = nullptr)`
  - `float clamp_setting(float v, float lo, float hi)`; `find_row(rows, name)`; `SettingValue read(const SettingRow<T> &, const T &)`; `bool write(const SettingRow<T> &, T *, const SettingValue &)`; `void clamp_all(std::span<const SettingRow<T>>, T *)`
  - `struct RowInfo { name, label, hint, kind, min, max, ui_min, ui_max, step, options }`; `RowInfo row_info(const SettingRow<T> &)`
  - `class SettingsGroup` (virtual `rows()`, `get`, `get_default`, `set`, `overrides`, `clear`, `clear_all`)
  - `template <class T> class SettingsStore : public SettingsGroup` with `SettingsStore(std::span<const SettingRow<T>>, void (*normalize)(T *), const T &base)`, `set_listener(void (*)(const T &, void *), void *)`, `set(const T &)`, `rebase(const T &)`, `T get() const`, `T base() const`, `set_value(const char *, float)`, `float value(const char *) const`
  - Test helper `check_rows(std::span<const ve::SettingRow<T>>, std::initializer_list<T> presets)`

- [ ] **Step 1: Add the settings directory to the native build**

In `extension/SConstruct`, the last `pure_sources` line becomes:

```python
                Glob("src/grass/*.cpp") + Glob("src/gpu_layout/*.cpp") +
                Glob("src/settings/*.cpp"))
```

(`src/settings/godot/` is deliberately a subdirectory: its files include godot-cpp and must stay out of the native build. The extension build's `src/*/*/*.cpp` glob picks them up.)

- [ ] **Step 2: Write the row checker and the failing tests**

Create `extension/tests/settings_row_checks.h`:

```cpp
#pragma once
// Invariants every settings table holds (spec 2026-09-16 decision 5): names unique, one member
// per row and no member twice, min <= ui_min <= ui_max <= max, a positive step, an enum's max
// matching its options, and every preset a store can be based on lying inside the slider range.
// The last one is what makes a menu range that excludes a shipped value (S5) unrepresentable.
#include <doctest/doctest.h>
#include <cstring>
#include <initializer_list>
#include <span>
#include "settings/settings_table.h"

template <class T>
void check_rows(std::span<const ve::SettingRow<T>> rows, std::initializer_list<T> presets) {
	REQUIRE(!rows.empty());
	for (size_t i = 0; i < rows.size(); i++) {
		const ve::SettingRow<T> &r = rows[i];
		REQUIRE(r.name != nullptr);
		CAPTURE(r.name);
		CHECK(r.label != nullptr);
		CHECK(r.min <= r.ui_min);
		CHECK(r.ui_min <= r.ui_max);
		CHECK(r.ui_max <= r.max);
		CHECK(r.step > 0.0f);
		const int members = (r.b != nullptr) + (r.i != nullptr) + (r.f != nullptr) + (r.c != nullptr);
		CHECK(members == 1);
		switch (r.kind) {
			case ve::SettingKind::kBool: CHECK(r.b != nullptr); break;
			case ve::SettingKind::kInt:
				CHECK(r.i != nullptr);
				CHECK(r.options.empty());
				break;
			case ve::SettingKind::kEnum:
				CHECK(r.i != nullptr);
				REQUIRE(!r.options.empty());
				CHECK(r.max == static_cast<float>(r.options.size() - 1));
				break;
			case ve::SettingKind::kFloat: CHECK(r.f != nullptr); break;
			case ve::SettingKind::kColor: CHECK(r.c != nullptr); break;
		}
		for (size_t j = i + 1; j < rows.size(); j++) {
			const ve::SettingRow<T> &o = rows[j];
			CHECK(std::strcmp(r.name, o.name) != 0);
			if (r.b) CHECK(r.b != o.b);
			if (r.i) CHECK(r.i != o.i);
			if (r.f) CHECK(r.f != o.f);
			if (r.c) CHECK(r.c != o.c);
		}
	}
	for (const T &preset : presets) {
		for (const ve::SettingRow<T> &r : rows) {
			if (r.kind == ve::SettingKind::kBool) continue;
			CAPTURE(r.name);
			const ve::SettingValue v = ve::read(r, preset);
			const int channels = r.kind == ve::SettingKind::kColor ? 3 : 1;
			for (int k = 0; k < channels; k++) {
				CHECK(v.v[k] >= r.ui_min);
				CHECK(v.v[k] <= r.ui_max);
			}
		}
	}
}
```

Create `extension/tests/test_settings_table.cpp`:

```cpp
#include <doctest/doctest.h>
#include "settings/settings_store.h"
#include "settings_row_checks.h"
#include <cstring>
#include <limits>

namespace {

struct Knobs {
	bool on = true;
	int count = 4;
	float gain = 1.0f;
	float tint[3] = {0.5f, 0.5f, 0.5f};
	int mode = 1;
};

const char *const kModes[] = {"A", "B", "C"};

const ve::SettingRow<Knobs> kKnobRows[] = {
	ve::bool_row("on", "On", &Knobs::on),
	ve::int_row("count", "Count", &Knobs::count, 0, 8, 0, 8),
	ve::float_row("gain", "Gain", &Knobs::gain, 0.0f, 4.0f, 0.0f, 2.0f, 0.1f),
	ve::color_row("tint", "Tint", &Knobs::tint, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	ve::enum_row("mode", "Mode", &Knobs::mode, std::span<const char *const>(kModes)),
};

std::span<const ve::SettingRow<Knobs>> knob_rows() {
	return kKnobRows;
}

// A rule that spans fields, like BeautySettings' "zero taps is off".
void zero_count_is_off(Knobs *k) {
	if (k->count == 0) k->on = false;
}

using Store = ve::SettingsStore<Knobs>;
using V = ve::SettingValue;

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

} // namespace

TEST_CASE("the test table satisfies the row invariants") {
	check_rows(knob_rows(), {Knobs{}});
}

TEST_CASE("a row reads and writes its own member for every kind") {
	Knobs k;
	const auto rows = knob_rows();
	CHECK(ve::write(rows[0], &k, V::of_bool(false)));
	CHECK(ve::write(rows[1], &k, V::of_int(6)));
	CHECK(ve::write(rows[2], &k, V::of_float(1.5f)));
	CHECK(ve::write(rows[3], &k, V::of_color(0.1f, 0.2f, 0.3f)));
	CHECK(ve::write(rows[4], &k, V::of_enum(2)));
	CHECK_FALSE(k.on);
	CHECK(k.count == 6);
	CHECK(k.gain == doctest::Approx(1.5f));
	CHECK(k.tint[0] == doctest::Approx(0.1f));
	CHECK(k.tint[1] == doctest::Approx(0.2f));
	CHECK(k.tint[2] == doctest::Approx(0.3f));
	CHECK(k.mode == 2);
	CHECK(ve::read(rows[1], k).kind == ve::SettingKind::kInt);
	CHECK(ve::read(rows[1], k).v[0] == doctest::Approx(6.0f));
	CHECK(ve::read(rows[3], k).v[2] == doctest::Approx(0.3f));
	CHECK(ve::read(rows[4], k).kind == ve::SettingKind::kEnum);
}

TEST_CASE("writes clamp to the hard range; NaN floors and infinities clamp to the nearer bound") {
	Knobs k;
	const auto rows = knob_rows();
	ve::write(rows[2], &k, V::of_float(9.0f));
	CHECK(k.gain == 4.0f);
	ve::write(rows[2], &k, V::of_float(-1.0f));
	CHECK(k.gain == 0.0f);
	ve::write(rows[2], &k, V::of_float(kInf));
	CHECK(k.gain == 4.0f);
	ve::write(rows[2], &k, V::of_float(kNaN));
	CHECK(k.gain == 0.0f);
	ve::write(rows[2], &k, V::of_float(-kInf));
	CHECK(k.gain == 0.0f);
	ve::write(rows[1], &k, V::of_int(99));
	CHECK(k.count == 8);
	ve::write(rows[1], &k, V{ve::SettingKind::kInt, {kNaN, 0.0f, 0.0f}});
	CHECK(k.count == 0);
	ve::write(rows[3], &k, V::of_color(kNaN, 2.0f, -1.0f));
	CHECK(k.tint[0] == 0.0f);
	CHECK(k.tint[1] == 1.0f);
	CHECK(k.tint[2] == 0.0f);
	ve::write(rows[0], &k, V{ve::SettingKind::kBool, {kNaN, 0.0f, 0.0f}});
	CHECK_FALSE(k.on);
	ve::write(rows[4], &k, V::of_enum(7));
	CHECK(k.mode == 2);
}

TEST_CASE("ints and enums round to nearest after clamping") {
	Knobs k;
	const auto rows = knob_rows();
	ve::write(rows[1], &k, V{ve::SettingKind::kInt, {2.6f, 0.0f, 0.0f}});
	CHECK(k.count == 3);
	ve::write(rows[4], &k, V{ve::SettingKind::kEnum, {0.4f, 0.0f, 0.0f}});
	CHECK(k.mode == 0);
}

TEST_CASE("a value of the wrong kind is refused and leaves the struct untouched") {
	Knobs k;
	const auto rows = knob_rows();
	CHECK_FALSE(ve::write(rows[2], &k, V::of_int(3)));
	CHECK(k.gain == 1.0f);
	CHECK_FALSE(ve::write(rows[0], &k, V::of_float(0.0f)));
	CHECK(k.on);
}

TEST_CASE("rows are found by name and an unknown or null name finds none") {
	CHECK(ve::find_row(knob_rows(), "gain") == &kKnobRows[2]);
	CHECK(ve::find_row(knob_rows(), "nope") == nullptr);
	CHECK(ve::find_row(knob_rows(), nullptr) == nullptr);
}

TEST_CASE("clamp_all pulls every member into range") {
	Knobs k;
	k.count = -3;
	k.gain = kNaN;
	k.tint[1] = 5.0f;
	k.mode = 9;
	ve::clamp_all(knob_rows(), &k);
	CHECK(k.count == 0);
	CHECK(k.gain == 0.0f);
	CHECK(k.tint[1] == 1.0f);
	CHECK(k.mode == 2);
}

TEST_CASE("the store resolves base, then overrides, then normalize") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	CHECK(store.get().on);
	CHECK(store.set("count", V::of_int(0)));
	CHECK_FALSE(store.get().on);
	CHECK(store.get().count == 0);
	// A pure function of (base, overrides): nothing the normalize step did is latched.
	CHECK(store.set("count", V::of_int(3)));
	CHECK(store.get().on);
}

TEST_CASE("rebase keeps overrides and set(T) drops them") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	CHECK(store.set("gain", V::of_float(1.5f)));
	Knobs other;
	other.gain = 3.0f;
	other.count = 7;
	store.rebase(other);
	CHECK(store.get().gain == doctest::Approx(1.5f));
	CHECK(store.get().count == 7);
	CHECK(store.base().gain == doctest::Approx(3.0f));
	store.set(other);
	CHECK(store.get().gain == doctest::Approx(3.0f));
	CHECK(store.overrides().empty());
}

TEST_CASE("clear and clear_all drop overrides; an unknown name reports false") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	store.set("gain", V::of_float(1.5f));
	store.set("count", V::of_int(2));
	CHECK(store.clear("gain"));
	CHECK(store.get().gain == doctest::Approx(1.0f));
	CHECK(store.get().count == 2);
	CHECK_FALSE(store.clear("nope"));
	store.clear_all();
	CHECK(store.get().count == 4);
	CHECK(store.overrides().empty());
}

TEST_CASE("overrides report the clamped value that was set, in row order") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	store.set("count", V::of_int(0));
	store.set("on", V::of_bool(true));
	store.set("gain", V::of_float(9.0f));
	const auto o = store.overrides();
	REQUIRE(o.size() == 3);
	CHECK(std::strcmp(o[0].first, "on") == 0);
	// normalize turned the resolved value off; the override is still what was set.
	CHECK(o[0].second.v[0] == 1.0f);
	CHECK_FALSE(store.get().on);
	CHECK(std::strcmp(o[1].first, "count") == 0);
	CHECK(std::strcmp(o[2].first, "gain") == 0);
	CHECK(o[2].second.v[0] == doctest::Approx(4.0f));
}

TEST_CASE("the store refuses unknown names and wrong kinds without recording an override") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	CHECK_FALSE(store.set("nope", V::of_float(1.0f)));
	CHECK_FALSE(store.set("gain", V::of_int(1)));
	CHECK(store.overrides().empty());
}

TEST_CASE("set_value addresses bool, int, float and enum rows and refuses colour") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	CHECK(store.set_value("on", 0.0f));
	CHECK_FALSE(store.get().on);
	CHECK(store.set_value("count", 5.0f));
	CHECK(store.get().count == 5);
	CHECK(store.set_value("gain", 2.0f));
	CHECK(store.value("gain") == doctest::Approx(2.0f));
	CHECK(store.set_value("mode", 0.0f));
	CHECK(store.get().mode == 0);
	CHECK_FALSE(store.set_value("tint", 0.2f));
	CHECK(store.value("nope") == 0.0f);
}

TEST_CASE("get_default is the normalized base and ignores overrides") {
	Knobs base;
	base.count = 0;
	Store store(knob_rows(), zero_count_is_off, base);
	store.set("on", V::of_bool(true));
	store.set("gain", V::of_float(2.0f));
	V v;
	CHECK(store.get_default("gain", &v));
	CHECK(v.v[0] == doctest::Approx(1.0f));
	CHECK(store.get_default("on", &v));
	CHECK(v.v[0] == 0.0f);
	CHECK_FALSE(store.get_default("nope", &v));
}

TEST_CASE("the listener sees each resolved value and runs outside the lock") {
	struct Seen {
		Store *store = nullptr;
		int calls = 0;
		float gain = 0.0f;
	};
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	Seen seen;
	seen.store = &store;
	store.set_listener([](const Knobs &k, void *ctx) {
		auto *s = static_cast<Seen *>(ctx);
		s->calls++;
		s->gain = k.gain;
		// Re-entering the store deadlocks if the listener runs under its lock.
		CHECK(s->store->get().gain == doctest::Approx(k.gain));
	}, &seen);
	store.set("gain", V::of_float(2.5f));
	store.rebase(Knobs{});
	store.clear_all();
	CHECK(seen.calls == 3);
	CHECK(seen.gain == doctest::Approx(1.0f));
}

TEST_CASE("rows() describes every row for type-erased callers") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	const ve::SettingsGroup &g = store;
	REQUIRE(g.rows().size() == 5);
	CHECK(std::strcmp(g.rows()[4].name, "mode") == 0);
	CHECK(g.rows()[4].kind == ve::SettingKind::kEnum);
	CHECK(g.rows()[4].options.size() == 3);
	CHECK(g.rows()[2].ui_max == doctest::Approx(2.0f));
}
```

- [ ] **Step 3: Run the native suite to verify it fails**

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, `settings/settings_store.h: No such file or directory`.

- [ ] **Step 4: Write `settings_table.h`**

Create `extension/src/settings/settings_table.h`:

```cpp
#pragma once
// A settings table: one row per knob of a plain settings struct, naming the member, its kind,
// its hard clamp and the range a slider offers. Everything that addresses a knob by name -- the
// store, VoxelSettings, the settings panel, the config file, the inspector -- reads these rows,
// so a name, a range and a member cannot disagree. Pure: no godot-cpp.
// Spec: docs/superpowers/specs/2026-09-16-settings-store-design.md §3.1.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace ve {

enum class SettingKind : uint8_t { kBool, kInt, kFloat, kColor, kEnum };

// bool, int, float and enum index in v[0]; colour in v[0..2].
struct SettingValue {
	SettingKind kind = SettingKind::kFloat;
	float v[3] = {0.0f, 0.0f, 0.0f};

	static SettingValue of_bool(bool b) { return {SettingKind::kBool, {b ? 1.0f : 0.0f, 0.0f, 0.0f}}; }
	static SettingValue of_int(int i) { return {SettingKind::kInt, {static_cast<float>(i), 0.0f, 0.0f}}; }
	static SettingValue of_enum(int i) { return {SettingKind::kEnum, {static_cast<float>(i), 0.0f, 0.0f}}; }
	static SettingValue of_float(float f) { return {SettingKind::kFloat, {f, 0.0f, 0.0f}}; }
	static SettingValue of_color(float r, float g, float b) { return {SettingKind::kColor, {r, g, b}}; }
};

// Exactly one member pointer is set, matching `kind` (kInt and kEnum use `i`). min/max is the
// hard clamp -- per channel for colour, ignored for bool -- and ui_min/ui_max/step the slider.
template <class T>
struct SettingRow {
	const char *name = nullptr;
	const char *label = nullptr;
	SettingKind kind = SettingKind::kFloat;
	bool T::*b = nullptr;
	int T::*i = nullptr;
	float T::*f = nullptr;
	float (T::*c)[3] = nullptr;
	float min = 0.0f;
	float max = 0.0f;
	float ui_min = 0.0f;
	float ui_max = 0.0f;
	float step = 0.0f;
	std::span<const char *const> options = {};
	const char *hint = nullptr;
};

template <class T>
constexpr SettingRow<T> bool_row(const char *name, const char *label, bool T::*m,
		const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kBool;
	r.b = m;
	r.max = r.ui_max = r.step = 1.0f;
	r.hint = hint;
	return r;
}

template <class T>
constexpr SettingRow<T> int_row(const char *name, const char *label, int T::*m, int min, int max,
		int ui_min, int ui_max, int step = 1, const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kInt;
	r.i = m;
	r.min = static_cast<float>(min);
	r.max = static_cast<float>(max);
	r.ui_min = static_cast<float>(ui_min);
	r.ui_max = static_cast<float>(ui_max);
	r.step = static_cast<float>(step);
	r.hint = hint;
	return r;
}

template <class T>
constexpr SettingRow<T> float_row(const char *name, const char *label, float T::*m, float min,
		float max, float ui_min, float ui_max, float step, const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kFloat;
	r.f = m;
	r.min = min;
	r.max = max;
	r.ui_min = ui_min;
	r.ui_max = ui_max;
	r.step = step;
	r.hint = hint;
	return r;
}

template <class T>
constexpr SettingRow<T> color_row(const char *name, const char *label, float (T::*m)[3], float min,
		float max, float ui_min, float ui_max, float step, const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kColor;
	r.c = m;
	r.min = min;
	r.max = max;
	r.ui_min = ui_min;
	r.ui_max = ui_max;
	r.step = step;
	r.hint = hint;
	return r;
}

// Index into `options`. `min` may be negative for an enum with an out-of-table state (display
// resolution uses -1 for "not a preset size").
template <class T>
constexpr SettingRow<T> enum_row(const char *name, const char *label, int T::*m,
		std::span<const char *const> options, int min = 0, const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kEnum;
	r.i = m;
	r.options = options;
	r.min = r.ui_min = static_cast<float>(min);
	r.max = r.ui_max = static_cast<float>(options.size()) - 1.0f;
	r.step = 1.0f;
	r.hint = hint;
	return r;
}

// NaN floors to lo; +-inf clamp to the nearer bound (spec decision 3).
inline float clamp_setting(float v, float lo, float hi) {
	if (!(v > lo)) return lo;
	if (!(v < hi)) return hi;
	return v;
}

template <class T>
const SettingRow<T> *find_row(std::span<const SettingRow<T>> rows, const char *name) {
	if (!name) return nullptr;
	for (const SettingRow<T> &r : rows)
		if (std::strcmp(r.name, name) == 0) return &r;
	return nullptr;
}

template <class T>
SettingValue read(const SettingRow<T> &r, const T &s) {
	SettingValue v;
	v.kind = r.kind;
	switch (r.kind) {
		case SettingKind::kBool: v.v[0] = (s.*r.b) ? 1.0f : 0.0f; break;
		case SettingKind::kInt:
		case SettingKind::kEnum: v.v[0] = static_cast<float>(s.*r.i); break;
		case SettingKind::kFloat: v.v[0] = s.*r.f; break;
		case SettingKind::kColor:
			for (int k = 0; k < 3; k++) v.v[k] = (s.*r.c)[k];
			break;
	}
	return v;
}

// Clamps into the row's hard range (ints and enums round after clamping). False, and the struct
// untouched, when the value's kind is not the row's kind.
template <class T>
bool write(const SettingRow<T> &r, T *s, const SettingValue &v) {
	if (!s || v.kind != r.kind) return false;
	switch (r.kind) {
		case SettingKind::kBool: s->*r.b = !std::isnan(v.v[0]) && v.v[0] != 0.0f; break;
		case SettingKind::kInt:
		case SettingKind::kEnum:
			s->*r.i = static_cast<int>(std::lround(clamp_setting(v.v[0], r.min, r.max)));
			break;
		case SettingKind::kFloat: s->*r.f = clamp_setting(v.v[0], r.min, r.max); break;
		case SettingKind::kColor:
			for (int k = 0; k < 3; k++) (s->*r.c)[k] = clamp_setting(v.v[k], r.min, r.max);
			break;
	}
	return true;
}

template <class T>
void clamp_all(std::span<const SettingRow<T>> rows, T *s) {
	if (!s) return;
	for (const SettingRow<T> &r : rows) write(r, s, read(r, *s));
}

// A row without its member pointer, for callers that do not know T.
struct RowInfo {
	const char *name = nullptr;
	const char *label = nullptr;
	const char *hint = nullptr;
	SettingKind kind = SettingKind::kFloat;
	float min = 0.0f;
	float max = 0.0f;
	float ui_min = 0.0f;
	float ui_max = 0.0f;
	float step = 0.0f;
	std::span<const char *const> options = {};
};

template <class T>
RowInfo row_info(const SettingRow<T> &r) {
	RowInfo info;
	info.name = r.name;
	info.label = r.label;
	info.hint = r.hint;
	info.kind = r.kind;
	info.min = r.min;
	info.max = r.max;
	info.ui_min = r.ui_min;
	info.ui_max = r.ui_max;
	info.step = r.step;
	info.options = r.options;
	return info;
}

} // namespace ve
```

- [ ] **Step 5: Write `settings_store.h`**

Create `extension/src/settings/settings_store.h`:

```cpp
#pragma once
// A settings store: a base value (a quality tier's preset, a struct's defaults, what the window
// was at startup) plus per-knob overrides that survive a rebase. The resolved value is a pure
// function of the two: base, then each override, then the table's clamp, then the struct's
// cross-field normalize. Pure: no godot-cpp.
// Spec: docs/superpowers/specs/2026-09-16-settings-store-design.md §3.2.
#include "settings/settings_table.h"
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ve {

// What VoxelSettings and the debug hooks see: a group of named knobs, type-erased.
class SettingsGroup {
public:
	virtual ~SettingsGroup() = default;
	virtual const std::vector<RowInfo> &rows() const = 0;
	virtual bool get(const char *name, SettingValue *out) const = 0;
	// The resolved base, ignoring overrides: what clearing this knob would give.
	virtual bool get_default(const char *name, SettingValue *out) const = 0;
	// False for an unknown name or a value of the wrong kind; nothing is recorded then.
	virtual bool set(const char *name, const SettingValue &v) = 0;
	// In row order; each value is the clamped value that was set.
	virtual std::vector<std::pair<const char *, SettingValue>> overrides() const = 0;
	virtual bool clear(const char *name) = 0;
	virtual void clear_all() = 0;
};

template <class T>
class SettingsStore : public SettingsGroup {
public:
	using Rows = std::span<const SettingRow<T>>;
	using Normalize = void (*)(T *);
	using Listener = void (*)(const T &resolved, void *ctx);

	SettingsStore(Rows rows, Normalize normalize, const T &base) :
			rows_(rows), normalize_(normalize), overrides_(rows.size()), base_(base) {
		infos_.reserve(rows_.size());
		for (const SettingRow<T> &r : rows_) infos_.push_back(row_info(r));
		resolved_ = resolve(base_, overrides_);
	}

	// Wiring, before any concurrent use. Called after every mutation with the new resolved value,
	// on the mutating thread, OUTSIDE this store's lock -- so a listener may read this store or
	// mutate another one.
	void set_listener(Listener fn, void *ctx) {
		listener_ = fn;
		listener_ctx_ = ctx;
	}

	// Replace the base and drop every override.
	void set(const T &base) {
		mutate([&] {
			base_ = base;
			for (std::optional<SettingValue> &o : overrides_) o.reset();
		});
	}
	// Replace the base and keep the overrides (a tier change).
	void rebase(const T &base) {
		mutate([&] { base_ = base; });
	}
	T get() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return resolved_;
	}
	T base() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return base_;
	}
	// Name-addressed convenience for bool, int, float and enum rows; the float is read as the
	// row's own kind. False for an unknown name or a colour row.
	bool set_value(const char *name, float v) {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r || r->kind == SettingKind::kColor) return false;
		SettingValue sv;
		sv.kind = r->kind;
		sv.v[0] = v;
		return set(name, sv);
	}
	float value(const char *name) const {
		SettingValue v;
		return get(name, &v) ? v.v[0] : 0.0f;
	}

	const std::vector<RowInfo> &rows() const override { return infos_; }

	bool get(const char *name, SettingValue *out) const override {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r || !out) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		*out = read(*r, resolved_);
		return true;
	}

	bool get_default(const char *name, SettingValue *out) const override {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r || !out) return false;
		const T def = resolve(base(), std::vector<std::optional<SettingValue>>(rows_.size()));
		*out = read(*r, def);
		return true;
	}

	bool set(const char *name, const SettingValue &v) override {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r || v.kind != r->kind) return false;
		// Stored clamped, so overrides() reports the value that actually took.
		T scratch{};
		write(*r, &scratch, v);
		const SettingValue clamped = read(*r, scratch);
		const size_t i = static_cast<size_t>(r - rows_.data());
		mutate([&] { overrides_[i] = clamped; });
		return true;
	}

	std::vector<std::pair<const char *, SettingValue>> overrides() const override {
		std::vector<std::pair<const char *, SettingValue>> out;
		std::lock_guard<std::mutex> lock(mutex_);
		for (size_t i = 0; i < rows_.size(); i++)
			if (overrides_[i]) out.emplace_back(rows_[i].name, *overrides_[i]);
		return out;
	}

	bool clear(const char *name) override {
		const SettingRow<T> *r = find_row(rows_, name);
		if (!r) return false;
		const size_t i = static_cast<size_t>(r - rows_.data());
		mutate([&] { overrides_[i].reset(); });
		return true;
	}

	void clear_all() override {
		mutate([&] {
			for (std::optional<SettingValue> &o : overrides_) o.reset();
		});
	}

private:
	template <class F>
	void mutate(F &&change) {
		T resolved;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			change();
			resolved_ = resolve(base_, overrides_);
			resolved = resolved_;
		}
		if (listener_) listener_(resolved, listener_ctx_);
	}

	T resolve(const T &base, const std::vector<std::optional<SettingValue>> &overrides) const {
		T out = base;
		for (size_t i = 0; i < rows_.size(); i++)
			if (overrides[i]) write(rows_[i], &out, *overrides[i]);
		clamp_all(rows_, &out);
		if (normalize_) normalize_(&out);
		return out;
	}

	Rows rows_;
	Normalize normalize_ = nullptr;
	std::vector<RowInfo> infos_;
	Listener listener_ = nullptr;
	void *listener_ctx_ = nullptr;
	mutable std::mutex mutex_;
	std::vector<std::optional<SettingValue>> overrides_;
	T base_;
	T resolved_;
};

} // namespace ve
```

- [ ] **Step 6: Run the native suite to verify it passes**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS; every `test_settings_table.cpp` case green.

- [ ] **Step 7: Commit**

```bash
git add extension/SConstruct extension/src/settings/settings_table.h extension/src/settings/settings_store.h extension/tests/settings_row_checks.h extension/tests/test_settings_table.cpp
git commit -m "feat: generic settings table and store"
```

---

### Task 7: Grass on the store

**Files:**
- Modify: `extension/src/grass/grass_settings.h` (end of file), `extension/src/grass/grass_settings.cpp` (whole body)
- Modify: `extension/src/grass/grass_settings_store.h` (whole file)
- Delete: `extension/src/grass/grass_settings_store.cpp`
- Test: `extension/tests/test_grass_settings.cpp`

**Interfaces:**
- Consumes: Task 6's `SettingRow`, row builders, `clamp_all`, `SettingsStore`, `check_rows`.
- Produces: `std::span<const ve::SettingRow<ve::GrassSettings>> ve::grass_rows()`; `ve::GrassSettingsStore` (default-constructible `SettingsStore<GrassSettings>`, base `GrassSettings{}`, no normalize). `RenderOrchestrator` keeps calling `grass_settings_.get()`, `set_value`, `value` unchanged.

- [ ] **Step 1: Write the failing invariant test**

Append to `extension/tests/test_grass_settings.cpp` (add `#include "settings_row_checks.h"` at the top):

```cpp
TEST_CASE("the grass rows satisfy the table invariants") {
	check_rows(ve::grass_rows(), {ve::GrassSettings{}});
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, `'grass_rows' is not a member of 've'`.

- [ ] **Step 3: Move the grass ranges into rows**

In `extension/src/grass/grass_settings.h`, add `#include "settings/settings_table.h"` and `#include <span>` below `#pragma once`, and replace the `clamp_grass_settings` declaration block with:

```cpp
// One row per knob: name, clamp and slider range. The store, the settings panel, the config file
// and clamp_grass_settings all read these rows.
std::span<const SettingRow<GrassSettings>> grass_rows();

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_grass_settings(GrassSettings *s);
```

Replace the whole of `extension/src/grass/grass_settings.cpp` with:

```cpp
#include "grass/grass_settings.h"

namespace ve {
namespace {

// Hard ranges are the ones clamp_grass_settings enforced before the table existed. A NaN floors
// to the low end, which is the conservative end for every knob here.
const SettingRow<GrassSettings> kGrassRows[] = {
	bool_row("enabled", "Grass", &GrassSettings::enabled),
	// Stage 1's dispatch width grows with the cube of reach, hence the hard 256 m bound.
	float_row("reach_m", "Reach (m)", &GrassSettings::reach_m, 0.0f, 256.0f, 0.0f, 120.0f, 1.0f),
	float_row("vertical_reach_m", "Vertical reach (m)", &GrassSettings::vertical_reach_m, 0.0f,
			64.0f, 0.0f, 32.0f, 0.5f),
	int_row("blades_per_brick", "Density", &GrassSettings::blades_per_brick, 0, 64, 0, 64),
	int_row("max_blades", "Max blades", &GrassSettings::max_blades, 0, 4000000, 0, 2000000, 10000),
	float_row("blade_width_m", "Blade width (m)", &GrassSettings::blade_width_m, 0.0f, 0.5f, 0.0f,
			0.2f, 0.005f),
	float_row("blade_height_m", "Blade height (m)", &GrassSettings::blade_height_m, 0.0f, 4.0f,
			0.0f, 2.0f, 0.05f),
	float_row("height_jitter", "Height jitter", &GrassSettings::height_jitter, 0.0f, 1.0f, 0.0f,
			1.0f, 0.01f),
	float_row("slope_cos_min", "Min slope (cos)", &GrassSettings::slope_cos_min, -1.0f, 1.0f, 0.0f,
			1.0f, 0.01f),
	float_row("wind_strength", "Wind strength (m)", &GrassSettings::wind_strength, 0.0f, 4.0f, 0.0f,
			1.5f, 0.05f),
	float_row("wind_speed", "Wind speed", &GrassSettings::wind_speed, 0.0f, 8.0f, 0.0f, 3.0f, 0.1f),
	float_row("wind_scale", "Wind scale (1/m)", &GrassSettings::wind_scale, 0.0f, 4.0f, 0.0f, 0.5f,
			0.005f),
	float_row("wind_dir_deg", "Wind direction (deg)", &GrassSettings::wind_dir_deg, 0.0f, 360.0f,
			0.0f, 360.0f, 1.0f),
	// Never past pi: a half-width of pi already covers every azimuth, and anything beyond it is
	// the uniform-random lean this field exists to replace.
	float_row("lean_spread_rad", "Lean spread (rad)", &GrassSettings::lean_spread_rad, 0.0f,
			3.14159265f, 0.0f, 3.14159265f, 0.01f),
	float_row("base_curve", "Blade curve", &GrassSettings::base_curve, 0.0f, 2.0f, 0.0f, 2.0f, 0.01f),
	float_row("camera_tilt", "Camera tilt", &GrassSettings::camera_tilt, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("ring_width_gain", "Far ring width gain", &GrassSettings::ring_width_gain, 0.0f, 6.0f,
			0.0f, 6.0f, 0.1f),
	float_row("flower_chance", "Flower chance", &GrassSettings::flower_chance, 0.0f, 1.0f, 0.0f,
			0.1f, 0.001f),
	float_row("gloss", "Gloss", &GrassSettings::gloss, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	float_row("blade_lighting", "Blade lighting", &GrassSettings::blade_lighting, 0.0f, 1.0f, 0.0f,
			1.0f, 0.05f),
};

} // namespace

std::span<const SettingRow<GrassSettings>> grass_rows() {
	return kGrassRows;
}

void clamp_grass_settings(GrassSettings *s) {
	clamp_all(grass_rows(), s);
}

} // namespace ve
```

Replace the whole of `extension/src/grass/grass_settings_store.h` with:

```cpp
#pragma once
#include "grass/grass_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Grass is its own module with its own store (design doc section 7): it shares the settings
// mechanism with the beauty stack, never the struct. No godot-cpp -- this file is in the native
// test target.
class GrassSettingsStore : public SettingsStore<GrassSettings> {
public:
	GrassSettingsStore() :
			SettingsStore(grass_rows(), nullptr, GrassSettings{}) {}
};

} // namespace ve
```

Delete the old implementation:

```bash
git rm extension/src/grass/grass_settings_store.cpp
```

- [ ] **Step 4: Run the native suite**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS — every existing case in `test_grass_settings.cpp` and `test_grass_layout.cpp` unchanged, plus the invariant case.

- [ ] **Step 5: Build and run the grass GPU suites**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_grass.gd,res://tests/test_grass_golden.gd,res://tests/test_settings_names.gd,res://tests/test_debug_menu.gd
```

Expected: all PASS (match baseline).

- [ ] **Step 6: Commit**

```bash
git add extension/src/grass/grass_settings.h extension/src/grass/grass_settings.cpp extension/src/grass/grass_settings_store.h extension/tests/test_grass_settings.cpp
git commit -m "refactor(grass): settings are a table on the generic store"
```

---

### Task 8: Beauty on the store; int knobs settable by name

**Files:**
- Modify: `extension/src/shade/beauty_settings.h` (declarations), `extension/src/shade/beauty_settings.cpp` (clamp helpers and `clamp_settings`)
- Create: `extension/src/shade/beauty_settings_store.h`
- Create: `extension/src/settings/godot/setting_variant.h`, `extension/src/settings/godot/setting_variant.cpp`
- Modify: `extension/src/render/orchestrator.h:37, 160-172, 348-351`, `extension/src/render/orchestrator.cpp:483-512, 576-637`
- Modify: `extension/src/debug/hooks_render.cpp:406-436`
- Test: `extension/tests/test_beauty_settings.cpp`, `tests/test_settings_names.gd`

**Interfaces:**
- Consumes: Task 6 store and rows; Task 2's `check_same`.
- Produces:
  - `std::span<const ve::SettingRow<ve::BeautySettings>> ve::beauty_rows()`; `void ve::normalize_beauty(ve::BeautySettings *)`; `ve::BeautySettingsStore` (base `settings_for_tier(kHigh)`, normalize `normalize_beauty`).
  - `godot::Variant setting_to_variant(const ve::SettingValue &)`; `bool setting_from_variant(ve::SettingKind, const godot::Variant &, ve::SettingValue *)`; `godot::String setting_kind_name(ve::SettingKind)`.
  - `RenderOrchestrator` members `ve::BeautySettingsStore beauty_;` and `std::atomic<int> quality_tier_`; public signatures unchanged. `set_effect_value` / `get_effect_value` accept int and float beauty rows.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_beauty_settings.cpp` (add `#include "settings_row_checks.h"` and `#include "shade/beauty_settings_store.h"` at the top):

```cpp
TEST_CASE("the beauty rows satisfy the table invariants for every tier") {
	check_rows(ve::beauty_rows(),
			{ve::BeautySettings{}, ve::settings_for_tier(ve::QualityTier::kOff),
					ve::settings_for_tier(ve::QualityTier::kLow),
					ve::settings_for_tier(ve::QualityTier::kMedium),
					ve::settings_for_tier(ve::QualityTier::kHigh)});
}

TEST_CASE("the beauty store starts at High and sets counts by name") {
	ve::BeautySettingsStore store;
	check_same(store.get(), ve::settings_for_tier(ve::QualityTier::kHigh));
	CHECK(store.set_value("ssgi_taps", 4.0f));
	CHECK(store.get().ssgi_taps == 4);
	CHECK(store.set_value("ssgi_taps", 0.0f));
	CHECK_FALSE(store.get().ssgi); // normalize: zero work is off
}
```

Append to `tests/test_settings_names.gd`:

```gdscript
# Settings-store plan Task 8: the counts became settable by name.
const BEAUTY_COUNTS := [["ssgi_taps", 4], ["ssr_steps", 12], ["contact_steps", 8],
	["ssao_steps", 4], ["ssao_directions", 4]]

func test_every_beauty_count_is_settable_by_name() -> void:
	var w := make_world()
	for entry in BEAUTY_COUNTS:
		w.set_effect_value(entry[0], entry[1])
		assert_float(w.get_effect_value(entry[0])).override_failure_message(entry[0]) \
			.is_equal_approx(float(entry[1]), 0.0001)
		assert_int(int(w.hooks().debug_beauty_settings()[entry[0]])).is_equal(entry[1])
	# A switch is not a magnitude: set_effect_value leaves it alone.
	w.set_effect_value("ssgi", 0.0)
	assert_bool(w.get_effect_enabled("ssgi")).is_true()
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, `shade/beauty_settings_store.h: No such file or directory`.

Run: `./gdunit_tests.sh -a res://tests/test_settings_names.gd`
Expected: `test_every_beauty_count_is_settable_by_name` FAILS (`ssgi_taps` reads 0.0).

- [ ] **Step 3: Beauty rows and store**

In `extension/src/shade/beauty_settings.h`, add `#include "settings/settings_table.h"` and `#include <span>` below `#include <cstdint>`, and replace the `clamp_settings` declaration with:

```cpp
// One row per knob (spec 2026-09-16 §3.3). The store, the settings panel, the config file, the
// inspector and debug_beauty_settings read these rows; clamp_settings clamps through them.
std::span<const SettingRow<BeautySettings>> beauty_rows();
// The rules that span fields: zero work is off. Runs after every clamp and every store resolve.
void normalize_beauty(BeautySettings *s);
void clamp_settings(BeautySettings *s);
```

In `extension/src/shade/beauty_settings.cpp`, delete the anonymous-namespace `clamp_int` / `clamp_float` helpers and the old `clamp_settings` body. Add, inside `namespace ve` before `settings_for_tier`:

```cpp
namespace {

const SettingRow<BeautySettings> kBeautyRows[] = {
	bool_row("ssgi", "SSGI", &BeautySettings::ssgi),
	bool_row("ssr", "SSR", &BeautySettings::ssr),
	bool_row("contact_shadows", "Contact shadows", &BeautySettings::contact_shadows),
	bool_row("outlines", "Outlines", &BeautySettings::outlines),
	bool_row("sun_shadow_map", "Sun shadow map", &BeautySettings::sun_shadow_map),
	bool_row("glossy_sdf_rays", "Glossy SDF rays", &BeautySettings::glossy_sdf_rays),
	bool_row("raymarched_sun_shadow", "Raymarched sun shadow", &BeautySettings::raymarched_sun_shadow),
	bool_row("ssao", "SSAO", &BeautySettings::ssao),
	bool_row("cost_view", "Cost view", &BeautySettings::cost_view,
			"Replaces albedo with raymarch cost. A debug view; no tier sets it."),
	int_row("ssgi_taps", "SSGI taps", &BeautySettings::ssgi_taps, 0, 16, 0, 16),
	int_row("ssr_steps", "SSR steps", &BeautySettings::ssr_steps, 0, 64, 0, 64),
	int_row("contact_steps", "Contact shadow steps", &BeautySettings::contact_steps, 0, 32, 0, 32),
	int_row("ssao_steps", "SSAO steps", &BeautySettings::ssao_steps, 0, 16, 0, 16),
	int_row("ssao_directions", "SSAO directions", &BeautySettings::ssao_directions, 0, 8, 0, 8),
	// A radius floor of 0.25 m rather than 0: a zero-radius gather still dispatches, still reads
	// the G-buffer, and returns black -- the expensive way to spell "off". `ssgi` is the switch.
	float_row("ssgi_radius", "GI reach (m)", &BeautySettings::ssgi_radius, 0.25f, 64.0f, 0.25f,
			64.0f, 0.25f),
	// Strictly below 1: at 1.0 the accumulator never takes the current frame and the image
	// freezes on whatever it happened to hold.
	float_row("ssgi_temporal", "GI history weight", &BeautySettings::ssgi_temporal, 0.0f, 0.99f,
			0.0f, 0.99f, 0.01f),
	float_row("ssgi_strength", "GI bounce", &BeautySettings::ssgi_strength, 0.0f, 8.0f, 0.0f, 8.0f,
			0.05f),
	float_row("emissive_gi_radius", "Emissive reach (m)", &BeautySettings::emissive_gi_radius, 0.25f,
			512.0f, 0.25f, 128.0f, 0.25f),
	float_row("emissive_gi_strength", "Emissive light", &BeautySettings::emissive_gi_strength, 0.0f,
			64.0f, 0.0f, 64.0f, 0.5f),
	float_row("outline_depth_threshold", "Outline depth threshold",
			&BeautySettings::outline_depth_threshold, 0.0f, 1.0f, 0.0f, 0.2f, 0.005f),
	float_row("outline_normal_threshold", "Outline normal threshold",
			&BeautySettings::outline_normal_threshold, 0.0f, 2.0f, 0.0f, 1.0f, 0.01f),
};

} // namespace

std::span<const SettingRow<BeautySettings>> beauty_rows() {
	return kBeautyRows;
}

void normalize_beauty(BeautySettings *s) {
	if (!s) return;
	// Zero work is off. A dispatch that produces nothing still costs a full-screen pass.
	if (s->ssgi_taps == 0) s->ssgi = false;
	if (s->ssr_steps == 0) s->ssr = false;
	if (s->contact_steps == 0) s->contact_shadows = false;
	if (s->ssao_steps == 0 || s->ssao_directions == 0) s->ssao = false;
}

void clamp_settings(BeautySettings *s) {
	clamp_all(beauty_rows(), s);
	normalize_beauty(s);
}
```

Create `extension/src/shade/beauty_settings_store.h`:

```cpp
#pragma once
#include "settings/settings_store.h"
#include "shade/beauty_settings.h"

namespace ve {

// The beauty knobs: a quality tier's preset as the base, per-knob overrides on top. A tier change
// rebases (RenderOrchestrator), so overrides survive it. No godot-cpp -- in the native test target.
class BeautySettingsStore : public SettingsStore<BeautySettings> {
public:
	BeautySettingsStore() :
			SettingsStore(beauty_rows(), normalize_beauty, settings_for_tier(QualityTier::kHigh)) {}
};

} // namespace ve
```

- [ ] **Step 4: Run the native suite**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS, including every existing `test_beauty_settings.cpp` case.

- [ ] **Step 5: The Variant helper**

Create `extension/src/settings/godot/setting_variant.h`:

```cpp
#pragma once
// SettingValue <-> Variant, for the three places GDScript meets a settings row:
// VoxelSettings, its inspector properties and debug_beauty_settings.
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include "settings/settings_table.h"

namespace godot {

// bool, int (int and enum), float, or Color.
Variant setting_to_variant(const ve::SettingValue &v);
// Reads `in` as `kind`. A bool, int or float coerces to any non-colour kind; only a Color reads as
// a colour. False when the Variant cannot be read as the kind.
bool setting_from_variant(ve::SettingKind kind, const Variant &in, ve::SettingValue *out);
// "bool", "int", "float", "color" or "enum".
String setting_kind_name(ve::SettingKind kind);

} // namespace godot
```

Create `extension/src/settings/godot/setting_variant.cpp`:

```cpp
#include "settings/godot/setting_variant.h"
#include <godot_cpp/variant/color.hpp>
#include <cmath>

namespace godot {

Variant setting_to_variant(const ve::SettingValue &v) {
	switch (v.kind) {
		case ve::SettingKind::kBool: return v.v[0] != 0.0f;
		case ve::SettingKind::kInt:
		case ve::SettingKind::kEnum: return static_cast<int64_t>(std::lround(v.v[0]));
		case ve::SettingKind::kFloat: return v.v[0];
		case ve::SettingKind::kColor: return Color(v.v[0], v.v[1], v.v[2]);
	}
	return Variant();
}

bool setting_from_variant(ve::SettingKind kind, const Variant &in, ve::SettingValue *out) {
	if (!out) return false;
	out->kind = kind;
	if (kind == ve::SettingKind::kColor) {
		if (in.get_type() != Variant::COLOR) return false;
		const Color c = in;
		out->v[0] = c.r;
		out->v[1] = c.g;
		out->v[2] = c.b;
		return true;
	}
	switch (in.get_type()) {
		case Variant::BOOL: out->v[0] = static_cast<bool>(in) ? 1.0f : 0.0f; return true;
		case Variant::INT: out->v[0] = static_cast<float>(static_cast<int64_t>(in)); return true;
		case Variant::FLOAT: out->v[0] = static_cast<float>(static_cast<double>(in)); return true;
		default: return false;
	}
}

String setting_kind_name(ve::SettingKind kind) {
	switch (kind) {
		case ve::SettingKind::kBool: return "bool";
		case ve::SettingKind::kInt: return "int";
		case ve::SettingKind::kFloat: return "float";
		case ve::SettingKind::kColor: return "color";
		case ve::SettingKind::kEnum: return "enum";
	}
	return String();
}

} // namespace godot
```

- [ ] **Step 6: The orchestrator uses the store**

In `extension/src/render/orchestrator.h`, add `#include "shade/beauty_settings_store.h"` beside `#include "shade/beauty_settings.h"`. Replace the three members

```cpp
	mutable std::mutex beauty_mutex_;
	int quality_tier_ = static_cast<int>(ve::QualityTier::kHigh);
	ve::BeautySettings beauty_ = ve::settings_for_tier(ve::QualityTier::kHigh);
```

(and the comment line above them) with:

```cpp
	// Setters run on the main thread; render callbacks take value snapshots through
	// beauty_settings(). The store's mutex is never held during render work.
	std::atomic<int> quality_tier_{static_cast<int>(ve::QualityTier::kHigh)};
	ve::BeautySettingsStore beauty_;
```

Update the comment above `beauty_snapshot` to: `// Settings + tier together, for debug_beauty_settings.`

In `extension/src/render/orchestrator.cpp`, delete the anonymous namespace holding `beauty_field` and `beauty_value_field`, and replace the bodies from `set_quality_tier` through `beauty_snapshot` with:

```cpp
namespace {

// set/get_effect_value address the knobs that are a magnitude; switches go through
// set/get_effect_enabled.
bool is_magnitude(ve::SettingKind kind) {
	return kind == ve::SettingKind::kInt || kind == ve::SettingKind::kFloat;
}

} // namespace

void RenderOrchestrator::set_quality_tier(int v) {
	const int tier = v < 0 ? 0 : (v > 3 ? 3 : v);
	quality_tier_.store(tier, std::memory_order_relaxed);
	beauty_.set(ve::settings_for_tier(static_cast<ve::QualityTier>(tier)));
}

int RenderOrchestrator::quality_tier() const {
	return quality_tier_.load(std::memory_order_relaxed);
}

void RenderOrchestrator::set_effect_enabled(const String &name, bool on) {
	if (name == "islands") {
		islands_enabled_.store(on, std::memory_order_relaxed);
		return;
	}
	if (name == "near_field") {
		near_field_enabled_.store(on, std::memory_order_relaxed);
		return;
	}
	const CharString n = name.utf8();
	beauty_.set(n.get_data(), ve::SettingValue::of_bool(on)); // fail-soft: unknown name or not a switch
}

bool RenderOrchestrator::get_effect_enabled(const String &name) const {
	if (name == "islands") return islands_enabled_.load(std::memory_order_relaxed);
	if (name == "near_field") return near_field_enabled_.load(std::memory_order_relaxed);
	const CharString n = name.utf8();
	ve::SettingValue v;
	return beauty_.get(n.get_data(), &v) && v.kind == ve::SettingKind::kBool && v.v[0] != 0.0f;
}

void RenderOrchestrator::set_effect_value(const String &name, float value) {
	const CharString n = name.utf8();
	ve::SettingValue current;
	if (!beauty_.get(n.get_data(), &current) || !is_magnitude(current.kind)) return; // fail-soft
	beauty_.set_value(n.get_data(), value);
}

float RenderOrchestrator::get_effect_value(const String &name) const {
	const CharString n = name.utf8();
	ve::SettingValue v;
	if (!beauty_.get(n.get_data(), &v) || !is_magnitude(v.kind)) return 0.0f;
	return v.v[0];
}

ve::BeautySettings RenderOrchestrator::beauty_settings() const {
	return beauty_.get();
}

void RenderOrchestrator::beauty_snapshot(ve::BeautySettings *out_settings, int *out_tier) const {
	*out_settings = beauty_.get();
	*out_tier = quality_tier();
}
```

(`set_quality_tier` still discards tweaks: `set(T)` drops overrides. Task 9 changes that as a separate `fix:`.)

- [ ] **Step 7: `debug_beauty_settings` from the rows**

In `extension/src/debug/hooks_render.cpp`, add `#include "settings/godot/setting_variant.h"` and replace the body of `debug_beauty_settings` with:

```cpp
Dictionary VoxelDebugHooks::debug_beauty_settings() {
	ve::BeautySettings beauty;
	int quality_tier;
	world_->context().render->beauty_snapshot(&beauty, &quality_tier);

	// Every row by name, so a new beauty knob appears here without another line.
	Dictionary d;
	for (const ve::SettingRow<ve::BeautySettings> &row : ve::beauty_rows())
		d[row.name] = setting_to_variant(ve::read(row, beauty));
	d["islands"] = world_->get_effect_enabled("islands");
	d["tier"] = quality_tier;
	d["flags"] = static_cast<int>(ve::pack_beauty_flags(beauty));
	return d;
}
```

- [ ] **Step 8: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_settings_names.gd,res://tests/test_beauty_settings.gd,res://tests/test_debug_menu.gd,res://tests/test_settings_menu.gd,res://tests/test_contact_shadow.gd,res://tests/test_emissive_gi.gd,res://tests/test_ssao.gd,res://tests/test_ssgi.gd,res://tests/test_ssr.gd,res://tests/test_outline.gd,res://tests/test_benchmark.gd
```

Expected: native PASS; `test_every_beauty_count_is_settable_by_name` PASSES; every other suite matches the baseline. If a case pinned the old "`ssgi` stays false after taps return" latch (spec decision 1), update it in this commit and name the cause in the message.

- [ ] **Step 9: Commit**

```bash
git add extension/src/shade/beauty_settings.h extension/src/shade/beauty_settings.cpp extension/src/shade/beauty_settings_store.h extension/src/settings/godot/setting_variant.h extension/src/settings/godot/setting_variant.cpp extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp extension/src/debug/hooks_render.cpp extension/tests/test_beauty_settings.cpp tests/test_settings_names.gd
git commit -m "refactor(beauty): settings are a table on the generic store

The if (name == ...) chains in orchestrator.cpp are deleted; names, kinds and
clamp ranges come from beauty_rows(). ssgi_taps, ssr_steps, contact_steps,
ssao_steps and ssao_directions are now settable by name.
debug_beauty_settings is built from the rows. A tier change still discards
tweaks (S6 is fixed separately)."
```

---

### Task 9: S6 — a tier change keeps per-knob tweaks

**Files:**
- Modify: `tests/test_beauty_settings.gd`
- Modify: `extension/src/render/orchestrator.cpp` (`set_quality_tier`)

**Interfaces:**
- Consumes: Task 8's `beauty_` store.
- Produces: `set_quality_tier` rebases; overrides survive.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_beauty_settings.gd`:

```gdscript
# S6: the tier sets defaults and tweaks layer on top, so choosing a tier must not throw away a
# knob somebody set by hand.
func test_a_tweak_survives_a_tier_change() -> void:
	var w := make_world()
	w.set_effect_enabled("ssr", false)
	w.set_effect_value("ssgi_strength", 2.0)
	w.quality_tier = 2
	var d := w.hooks().debug_beauty_settings()
	assert_bool(d["ssr"]).is_false()
	assert_float(d["ssgi_strength"]).is_equal_approx(2.0, 0.001)
	# ...while everything that was not tweaked follows the tier.
	assert_int(d["ssgi_taps"]).is_equal(4)

# demo/benchmark.gd applies --effects-off= before it parses --quality=; the effects must stay off.
func test_effects_turned_off_before_a_tier_stay_off() -> void:
	var w := make_world()
	w.set_effect_enabled("ssgi", false)
	w.set_quality_tier(3)
	assert_bool(w.get_effect_enabled("ssgi")).is_false()
```

- [ ] **Step 2: Run to verify they fail**

Run: `./gdunit_tests.sh -a res://tests/test_beauty_settings.gd`
Expected: both new cases FAIL (`ssr` reads true; `ssgi` reads true).

- [ ] **Step 3: Rebase instead of replace**

In `extension/src/render/orchestrator.cpp`, the last line of `set_quality_tier` becomes:

```cpp
	// Rebase: the tier is the base, per-knob overrides layer on top and survive (S6).
	beauty_.rebase(ve::settings_for_tier(static_cast<ve::QualityTier>(tier)));
```

- [ ] **Step 4: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_beauty_settings.gd,res://tests/test_debug_menu.gd,res://tests/test_settings_menu.gd,res://tests/test_benchmark.gd,res://tests/test_settings_names.gd
```

Expected: both new cases PASS; all others match the baseline. A case that set a knob and then expected a tier to reset it is updated in this commit with that cause.

- [ ] **Step 5: Commit**

```bash
git add tests/test_beauty_settings.gd extension/src/render/orchestrator.cpp
git commit -m "fix: a quality tier change keeps per-knob tweaks (S6)

set_quality_tier replaced every beauty knob with the tier preset, so
benchmark.gd's --effects-off= was silently undone by a later --quality=, and
no per-knob setting could survive a tier choice. The tier now rebases the
store; overrides survive until cleared."
```

---

### Task 10: SSAO radius and strength move into `BeautySettings`

**Files:**
- Modify: `extension/src/shade/beauty_settings.h` (struct), `extension/src/shade/beauty_settings.cpp` (`kBeautyRows`)
- Modify: `extension/src/render/ssao_pass.cpp:9-11, 66-67`
- Test: `extension/tests/test_beauty_settings.cpp`, `tests/test_settings_names.gd`

**Interfaces:**
- Consumes: Task 8 rows; Task 2 `check_same`.
- Produces: `BeautySettings::ssao_radius` (5.0), `BeautySettings::ssao_strength` (1.5); rows `ssao_radius`, `ssao_strength`.

- [ ] **Step 1: Write the failing tests**

In `extension/tests/test_beauty_settings.cpp`, add to the end of `check_same`:

```cpp
	CHECK(got.ssao_radius == doctest::Approx(want.ssao_radius));
	CHECK(got.ssao_strength == doctest::Approx(want.ssao_strength));
```

and append:

```cpp
TEST_CASE("the SSAO gather shape defaults to the literals it replaced") {
	const ve::BeautySettings s;
	CHECK(s.ssao_radius == 5.0f);
	CHECK(s.ssao_strength == 1.5f);
}
```

In `tests/test_settings_names.gd`, add to `BEAUTY_MAGNITUDES`: `["ssao_radius", 8.0], ["ssao_strength", 1.0]`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, `'struct ve::BeautySettings' has no member named 'ssao_radius'`.

- [ ] **Step 3: Add the fields, rows and push**

In `extension/src/shade/beauty_settings.h`, after `int ssao_directions = 6; …`, add:

```cpp
	// SSAO's gather shape. These were file-scope constants in ssao_pass.cpp, the one place a
	// look constant hid from both the tiers and the menu. Radius is world metres.
	float ssao_radius = 5.0f;   // [0.25, 32]
	float ssao_strength = 1.5f; // [0, 8]
```

In `kBeautyRows` (`beauty_settings.cpp`), after the `ssao_directions` row, add:

```cpp
	float_row("ssao_radius", "SSAO radius (m)", &BeautySettings::ssao_radius, 0.25f, 32.0f, 0.25f,
			16.0f, 0.25f),
	float_row("ssao_strength", "SSAO strength", &BeautySettings::ssao_strength, 0.0f, 8.0f, 0.0f,
			4.0f, 0.05f),
```

In `extension/src/render/ssao_pass.cpp`, delete the three lines

```cpp
// Must match the Push block in ssao.comp.glsl.
static const float kSsaoRadius = 5.0f;
static const float kSsaoStrength = 1.5f;
```

and change the push to:

```cpp
	const ve::SsaoPush push{{half.x, half.y, s.ssao_steps, s.ssao_directions},
			{s.ssao_radius, s.ssao_strength, 0.0f, 0.0f}};
```

- [ ] **Step 4: Build and run; the golden must not move**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_ssao_golden.gd,res://tests/test_ssao.gd,res://tests/test_settings_names.gd
```

Expected: all PASS. If `test_ssao_golden.gd` moves, stop and report (Global Constraints: golden policy).

- [ ] **Step 5: Commit**

```bash
git add extension/src/shade/beauty_settings.h extension/src/shade/beauty_settings.cpp extension/src/render/ssao_pass.cpp extension/tests/test_beauty_settings.cpp tests/test_settings_names.gd
git commit -m "refactor(ssao): radius and strength are beauty settings

test_ssao_golden.gd unchanged."
```

---

### Task 11: Outline darkening moves into `BeautySettings`

**Files:**
- Modify: `extension/src/shade/beauty_settings.h`, `extension/src/shade/beauty_settings.cpp`
- Modify: `extension/src/render/outline_pass.cpp:58-60`
- Test: `extension/tests/test_beauty_settings.cpp`, `tests/test_settings_names.gd`

**Interfaces:**
- Consumes: Task 8 rows; Task 2 `check_same`.
- Produces: `BeautySettings::outline_darken` (0.35); row `outline_darken`.

- [ ] **Step 1: Write the failing tests**

Add to `check_same`:

```cpp
	CHECK(got.outline_darken == doctest::Approx(want.outline_darken));
```

Append:

```cpp
TEST_CASE("outline darkening defaults to the literal it replaced") {
	CHECK(ve::BeautySettings{}.outline_darken == 0.35f);
}
```

Add `["outline_darken", 0.5]` to `BEAUTY_MAGNITUDES` in `tests/test_settings_names.gd`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, no member named `outline_darken`.

- [ ] **Step 3: Add the field, row and push**

In `beauty_settings.h`, after `outline_normal_threshold`:

```cpp
	// What an edge pixel's colour is multiplied by (OutlinePush.params.z). Was a literal in
	// outline_pass.cpp.
	float outline_darken = 0.35f; // [0, 1]
```

In `kBeautyRows`, after the `outline_normal_threshold` row:

```cpp
	float_row("outline_darken", "Outline colour multiplier", &BeautySettings::outline_darken, 0.0f,
			1.0f, 0.0f, 1.0f, 0.01f),
```

In `extension/src/render/outline_pass.cpp`, the push's params become:

```cpp
			{s.outline_depth_threshold, s.outline_normal_threshold, s.outline_darken, 0.0f}};
```

- [ ] **Step 4: Build and run; nothing may move**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_outline.gd,res://tests/test_settings_names.gd
```

Expected: all PASS, including `test_depth_line_is_one_pixel_and_darkens_by_the_fixed_amount`.

- [ ] **Step 5: Commit**

```bash
git add extension/src/shade/beauty_settings.h extension/src/shade/beauty_settings.cpp extension/src/render/outline_pass.cpp extension/tests/test_beauty_settings.cpp tests/test_settings_names.gd
git commit -m "refactor(outline): edge darkening is a beauty setting

test_outline.gd unchanged, including the fixed-darkening case."
```

---

### Task 12: Contact-shadow reach, strength and bias move into `BeautySettings`

**Files:**
- Modify: `extension/src/shade/beauty_settings.h`, `extension/src/shade/beauty_settings.cpp`
- Modify: `extension/src/render/contact_shadow_pass.cpp:64-67`
- Test: `extension/tests/test_beauty_settings.cpp`, `tests/test_settings_names.gd`

**Interfaces:**
- Consumes: Task 8 rows; Task 2 `check_same`.
- Produces: `BeautySettings::contact_reach_m` (0.6), `contact_strength` (0.85), `contact_bias_m` (0.05); rows of the same names.

- [ ] **Step 1: Write the failing tests**

Add to `check_same`:

```cpp
	CHECK(got.contact_reach_m == doctest::Approx(want.contact_reach_m));
	CHECK(got.contact_strength == doctest::Approx(want.contact_strength));
	CHECK(got.contact_bias_m == doctest::Approx(want.contact_bias_m));
```

Append:

```cpp
TEST_CASE("contact shadow shape defaults to the literals it replaced") {
	const ve::BeautySettings s;
	CHECK(s.contact_reach_m == 0.6f);
	CHECK(s.contact_strength == 0.85f);
	CHECK(s.contact_bias_m == 0.05f);
}
```

Add `["contact_reach_m", 1.0], ["contact_strength", 0.5], ["contact_bias_m", 0.1]` to `BEAUTY_MAGNITUDES`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, no member named `contact_reach_m`.

- [ ] **Step 3: Add the fields, rows and push**

In `beauty_settings.h`, after `int contact_steps = 12; …` add:

```cpp
	// The contact-shadow march (ContactShadowPush.params.xyz). Were literals in
	// contact_shadow_pass.cpp. Metres where named so.
	float contact_reach_m = 0.6f;   // [0.05, 4]  how far the screen-space march reaches
	float contact_strength = 0.85f; // [0, 1]     how dark a fully occluded pixel gets
	float contact_bias_m = 0.05f;   // [0, 0.5]   surface bias and hit thickness
```

In `kBeautyRows`, after the `contact_steps` row:

```cpp
	float_row("contact_reach_m", "Contact shadow reach (m)", &BeautySettings::contact_reach_m, 0.05f,
			4.0f, 0.05f, 2.0f, 0.05f),
	float_row("contact_strength", "Contact shadow strength", &BeautySettings::contact_strength, 0.0f,
			1.0f, 0.0f, 1.0f, 0.01f),
	// One voxel of surface bias by default: large enough to leave the receiver, too small to
	// bridge terrain gaps.
	float_row("contact_bias_m", "Contact shadow bias (m)", &BeautySettings::contact_bias_m, 0.0f,
			0.5f, 0.0f, 0.2f, 0.005f),
```

In `extension/src/render/contact_shadow_pass.cpp`, replace the two comment lines and the push declaration with:

```cpp
	ve::ContactShadowPush push{{half.x, half.y, 0, s.contact_steps},
			{s.contact_reach_m, s.contact_strength, s.contact_bias_m, 0.0f}};
```

- [ ] **Step 4: Build and run; nothing may move**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_contact_shadow_golden.gd,res://tests/test_contact_shadow.gd,res://tests/test_settings_names.gd
```

Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/src/shade/beauty_settings.h extension/src/shade/beauty_settings.cpp extension/src/render/contact_shadow_pass.cpp extension/tests/test_beauty_settings.cpp tests/test_settings_names.gd
git commit -m "refactor(contact): reach, strength and bias are beauty settings

test_contact_shadow_golden.gd unchanged."
```

---

### Task 13: Ambient moves into `BeautySettings`

**Files:**
- Modify: `extension/src/shade/beauty_settings.h`, `extension/src/shade/beauty_settings.cpp`
- Modify: `extension/src/render/deferred_pass.h:1-22`
- Modify: `extension/src/render/frame.cpp:419-427`
- Modify: `extension/src/debug/hooks_render.cpp` (the `DeferredPass::Params dp;` block near line 2055)
- Modify: `extension/src/voxel_world.cpp:411-419`
- Test: `extension/tests/test_beauty_settings.cpp`

**Interfaces:**
- Consumes: Task 8 rows; Task 2 `check_same`.
- Produces: `BeautySettings::ambient[3]` ({0.16, 0.19, 0.26}); colour row `ambient`; `DeferredPass::kAmbient` deleted; `ve_ambient` published from `beauty_settings().ambient`.

- [ ] **Step 1: Write the failing tests**

Add to `check_same`:

```cpp
	for (int k = 0; k < 3; k++) CHECK(got.ambient[k] == doctest::Approx(want.ambient[k]));
```

Append:

```cpp
TEST_CASE("ambient defaults to the constant it replaced") {
	const ve::BeautySettings s;
	CHECK(s.ambient[0] == 0.16f);
	CHECK(s.ambient[1] == 0.19f);
	CHECK(s.ambient[2] == 0.26f);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, no member named `ambient`.

- [ ] **Step 3: Add the field and row**

In `beauty_settings.h`, after `outline_darken`:

```cpp
	// Sky-fill light on every surface: the deferred pass's ambient term and the ve_ambient global
	// the cel-shaded objects read, one number for both. Was DeferredPass::kAmbient. Linear RGB.
	float ambient[3] = {0.16f, 0.19f, 0.26f}; // [0, 4] per channel
```

In `kBeautyRows`, after the `outline_darken` row:

```cpp
	color_row("ambient", "Ambient", &BeautySettings::ambient, 0.0f, 4.0f, 0.0f, 1.0f, 0.01f,
			"Sky-fill light on every surface; cel-shaded objects read the same colour."),
```

- [ ] **Step 4: Replace every `kAmbient` reader**

In `extension/src/render/deferred_pass.h`, add `#include "shade/beauty_settings.h"` below `#include "shade/sun_cascades.h"`, delete `static constexpr float kAmbient[3] = {0.16f, 0.19f, 0.26f};`, and change the `Params` member to:

```cpp
		// The frame and the probes fill this from BeautySettings::ambient; a Params nobody fills
		// carries the shipped default.
		float ambient[3] = {ve::BeautySettings{}.ambient[0], ve::BeautySettings{}.ambient[1],
				ve::BeautySettings{}.ambient[2]};
```

In `extension/src/render/frame.cpp`, after `dp.flags = beauty_flags;`:

```cpp
	for (int k = 0; k < 3; k++) dp.ambient[k] = beauty.ambient[k];
```

In `extension/src/debug/hooks_render.cpp`, in the probe that builds `DeferredPass::Params dp;` from `beauty` (the block setting `dp.flags = ve::pack_flags(beauty);`), add after that line:

```cpp
	for (int k = 0; k < 3; k++) dp.ambient[k] = beauty.ambient[k];
```

In `extension/src/voxel_world.cpp` `update_sun_state`, replace the `ve_ambient` publish with:

```cpp
		// Ambient is a beauty setting; the deferred pass reads the same snapshot, so both paths
		// share one number. Published every _process, so a change lands on the next frame.
		const ve::BeautySettings beauty = context_.render->beauty_settings();
		server->global_shader_parameter_set("ve_ambient",
				Vector3(beauty.ambient[0], beauty.ambient[1], beauty.ambient[2]));
```

Update the comment above that block from "Ambient is DeferredPass's" to "Ambient is BeautySettings'".

Check: `rg kAmbient extension/src` returns nothing.

- [ ] **Step 5: Build and run; nothing may move**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_deferred_golden.gd,res://tests/test_deferred.gd,res://tests/test_cel_object.gd,res://tests/test_frame_contract.gd
```

Expected: all PASS; `test_deferred_golden.gd` unchanged.

- [ ] **Step 6: Commit**

```bash
git add extension/src/shade/beauty_settings.h extension/src/shade/beauty_settings.cpp extension/src/render/deferred_pass.h extension/src/render/frame.cpp extension/src/debug/hooks_render.cpp extension/src/voxel_world.cpp extension/tests/test_beauty_settings.cpp
git commit -m "refactor(deferred): ambient is a beauty setting

DeferredPass::kAmbient is deleted; the frame, the deferred probe and the
ve_ambient global all read BeautySettings::ambient.
test_deferred_golden.gd unchanged."
```

---

### Task 14: The `render` group

**Files:**
- Create: `extension/src/settings/render_settings.h`, `extension/src/settings/render_settings.cpp`
- Create: `extension/tests/test_render_settings.cpp`
- Modify: `extension/src/render/orchestrator.h` (includes, public API, private members), `extension/src/render/orchestrator.cpp:45-46, set_quality_tier … get_effect_enabled, set_near_field_scale`
- Test: `tests/test_settings_names.gd`

**Interfaces:**
- Consumes: Task 6 store; Task 8 `BeautySettingsStore`.
- Produces:
  - `struct ve::RenderSettings { int quality_tier = 3; float near_field_scale = 0.66f; bool near_field = true; bool islands = true; }`; `ve::render_rows()`; `ve::RenderSettingsStore`.
  - `ve::SettingsGroup *RenderOrchestrator::settings_group(const char *name)` — `"render"`, `"beauty"`, `"grass"`, else `nullptr`.
  - Unchanged: `quality_tier()`, `near_field_scale()`, `near_field_enabled()`, `island_slot_count()`, `frame_settings()` (atomic reads).

- [ ] **Step 1: Write the failing native tests**

Create `extension/tests/test_render_settings.cpp`:

```cpp
#include <doctest/doctest.h>
#include "settings/render_settings.h"
#include "settings_row_checks.h"
#include "shade/beauty_settings_store.h"

TEST_CASE("the render rows satisfy the table invariants") {
	check_rows(ve::render_rows(), {ve::RenderSettings{}});
}

// RenderOrchestrator's atomics started at these values; the store's defaults must match them.
TEST_CASE("render defaults are the dials the orchestrator shipped with") {
	const ve::RenderSettings s;
	CHECK(s.quality_tier == static_cast<int>(ve::QualityTier::kHigh));
	CHECK(s.near_field_scale == doctest::Approx(0.66f));
	CHECK(s.near_field);
	CHECK(s.islands);
}

TEST_CASE("the render dials clamp as the orchestrator's setters did") {
	ve::RenderSettingsStore store;
	CHECK(store.set_value("quality_tier", 9.0f));
	CHECK(store.get().quality_tier == 3);
	CHECK(store.set_value("quality_tier", -2.0f));
	CHECK(store.get().quality_tier == 0);
	CHECK(store.set_value("near_field_scale", 5.0f));
	CHECK(store.get().near_field_scale == doctest::Approx(1.0f));
	CHECK(store.set_value("near_field_scale", 0.0f));
	CHECK(store.get().near_field_scale == doctest::Approx(0.1f));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, `settings/render_settings.h: No such file or directory`.

- [ ] **Step 3: Write the render table**

Create `extension/src/settings/render_settings.h`:

```cpp
#pragma once
#include "settings/settings_store.h"
#include <span>

namespace ve {

// The render budget dials (spec 2026-09-16 §3.3). Not beauty knobs: they decide what the frame
// renders and at what cost. RenderOrchestrator mirrors near_field_scale, near_field and islands
// into atomics so the render thread reads them without this store's lock.
struct RenderSettings {
	int quality_tier = 3;           // ve::QualityTier; a change rebases the beauty store
	float near_field_scale = 0.66f; // fraction of the internal resolution the near-field marcher runs at
	bool near_field = true;
	bool islands = true;
};

std::span<const SettingRow<RenderSettings>> render_rows();

class RenderSettingsStore : public SettingsStore<RenderSettings> {
public:
	RenderSettingsStore() :
			SettingsStore(render_rows(), nullptr, RenderSettings{}) {}
};

} // namespace ve
```

Create `extension/src/settings/render_settings.cpp`:

```cpp
#include "settings/render_settings.h"

namespace ve {
namespace {

const char *const kQualityTiers[] = {"Off", "Low", "Medium", "High"};

const SettingRow<RenderSettings> kRenderRows[] = {
	enum_row("quality_tier", "Quality", &RenderSettings::quality_tier,
			std::span<const char *const>(kQualityTiers)),
	float_row("near_field_scale", "Near-field resolution", &RenderSettings::near_field_scale, 0.1f,
			1.0f, 0.1f, 1.0f, 0.01f,
			"Near-field resolution is the fraction of the internal 3D resolution the raymarcher runs "
			"at. Only the terrain SILHOUETTE is resolved on that coarser grid, which is what the "
			"stepped pixels along terrain edges are; the material texture is resolved per "
			"full-resolution pixel either way, so lowering this does not blur the ground. Raise it "
			"to lose the stepping, lower it to buy frame time."),
	bool_row("near_field", "Near field", &RenderSettings::near_field),
	bool_row("islands", "Islands", &RenderSettings::islands),
};

} // namespace

std::span<const SettingRow<RenderSettings>> render_rows() {
	return kRenderRows;
}

} // namespace ve
```

- [ ] **Step 4: Run the native suite**

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS.

- [ ] **Step 5: The orchestrator owns the render store**

In `extension/src/render/orchestrator.h`:
- add `#include "settings/render_settings.h"`;
- in the public beauty-settings block, add:

```cpp
	// The three stores by group name ("render", "beauty", "grass"); nullptr otherwise.
	// VoxelSettings addresses them through this. Main thread.
	ve::SettingsGroup *settings_group(const char *name);
```

- in the private section beside `beauty_`, add:

```cpp
	// Source of truth for the budget dials. Its listener mirrors near_field_scale, near_field and
	// islands into the orchestrator's atomics (the render thread's lock-free reads) and rebases
	// beauty_ when the tier moves. The listener is attached in the constructor body, after every
	// member it touches exists.
	ve::RenderSettingsStore render_settings_;
	static void on_render_resolved(const ve::RenderSettings &s, void *ctx);
```

(Place these beside `ve::BeautySettingsStore beauty_;`.)

In `extension/src/render/orchestrator.cpp`, the constructor becomes:

```cpp
RenderOrchestrator::RenderOrchestrator(Collaborators handles) :
		handles_(std::move(handles)), frame_(*this, *handles_.lod, *handles_.store) {
	// The atomics and beauty_ already hold RenderSettings{}'s values, so nothing is mirrored
	// until the first write.
	render_settings_.set_listener(&RenderOrchestrator::on_render_resolved, this);
}
```

Replace `set_quality_tier`, `set_effect_enabled` and `get_effect_enabled` with:

```cpp
void RenderOrchestrator::on_render_resolved(const ve::RenderSettings &s, void *ctx) {
	auto *self = static_cast<RenderOrchestrator *>(ctx);
	self->islands_enabled_.store(s.islands, std::memory_order_relaxed);
	self->near_field_enabled_.store(s.near_field, std::memory_order_relaxed);
	self->near_field_scale_.store(s.near_field_scale, std::memory_order_relaxed);
	// Rebase: the tier is the base, per-knob overrides layer on top and survive (S6).
	if (self->quality_tier_.exchange(s.quality_tier, std::memory_order_relaxed) != s.quality_tier)
		self->beauty_.rebase(ve::settings_for_tier(static_cast<ve::QualityTier>(s.quality_tier)));
}

ve::SettingsGroup *RenderOrchestrator::settings_group(const char *name) {
	if (!name) return nullptr;
	if (std::strcmp(name, "render") == 0) return &render_settings_;
	if (std::strcmp(name, "beauty") == 0) return &beauty_;
	if (std::strcmp(name, "grass") == 0) return &grass_settings_;
	return nullptr;
}

void RenderOrchestrator::set_quality_tier(int v) {
	render_settings_.set_value("quality_tier", static_cast<float>(v));
}

void RenderOrchestrator::set_effect_enabled(const String &name, bool on) {
	const CharString n = name.utf8();
	const ve::SettingValue v = ve::SettingValue::of_bool(on);
	// Render switches (islands, near_field), then beauty switches; fail-soft for anything else.
	if (!render_settings_.set(n.get_data(), v)) beauty_.set(n.get_data(), v);
}

bool RenderOrchestrator::get_effect_enabled(const String &name) const {
	const CharString n = name.utf8();
	ve::SettingValue v;
	if (!render_settings_.get(n.get_data(), &v) && !beauty_.get(n.get_data(), &v)) return false;
	return v.kind == ve::SettingKind::kBool && v.v[0] != 0.0f;
}
```

`set_near_field_scale` becomes:

```cpp
void RenderOrchestrator::set_near_field_scale(float v) {
	render_settings_.set_value("near_field_scale", v); // the row clamps to [0.1, 1]
}
```

`quality_tier()` keeps returning `quality_tier_.load(…)` (now the mirror).

Check: `rg 'if \(name == "' extension/src/render/orchestrator.cpp` returns nothing, and `rg 'settings_group|render_settings_|beauty_\.' extension/src/render/frame.cpp` returns nothing.

- [ ] **Step 6: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_settings_names.gd,res://tests/test_beauty_settings.gd,res://tests/test_debug_menu.gd,res://tests/test_settings_menu.gd,res://tests/test_frame_contract.gd,res://tests/test_island_render.gd,res://tests/test_lod_stream.gd,res://tests/test_raymarch_gbuffer.gd,res://tests/test_benchmark.gd
```

Expected: native PASS; every suite matches the baseline.

- [ ] **Step 7: Commit**

```bash
git add extension/src/settings/render_settings.h extension/src/settings/render_settings.cpp extension/tests/test_render_settings.cpp extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp
git commit -m "refactor(render): budget dials are the render settings group

quality_tier, near_field_scale, near_field and islands live in one store; its
listener mirrors the atomics the render thread reads and rebases beauty on a
tier change. The islands/near_field name special cases are deleted."
```

---

### Task 15: `VoxelSettings` — display group, describe, persistence and the measured-run guard

**Files:**
- Create: `extension/src/settings/display_settings.h`, `extension/src/settings/display_settings.cpp`
- Create: `extension/tests/test_display_settings.cpp`
- Create: `extension/src/voxel_settings.h`, `extension/src/voxel_settings.cpp`
- Modify: `extension/src/register_types.cpp:17-23`
- Create: `tests/test_voxel_settings.gd`

**Interfaces:**
- Consumes: Task 6 store; Task 8 `setting_to_variant` / `setting_from_variant` / `setting_kind_name`; Task 14 `RenderOrchestrator::settings_group`; `VoxelWorld::context().render`.
- Produces:
  - `struct ve::DisplaySettings { float render_scale = 1.0f; int upscaler = 0; int resolution = -1; bool fullscreen = false; }`; `struct ve::WindowSize { int w, h; }`; `ve::kResolutions[5]`; `ve::kUpscalerOptionCount = 5`; `int ve::resolution_index_of(int w, int h)`; `ve::display_rows()`; `ve::DisplaySettingsStore`.
  - GDScript class `VoxelSettings` (Node): properties `world_path`, `viewport_path`, `config_path` (default `user://settings.cfg`), `manage_window` (default true); methods `groups() -> PackedStringArray`, `describe(group) -> Array`, `get_setting(group, name) -> Variant`, `set_setting(group, name, value) -> bool`, `get_overrides(group) -> Dictionary`, `clear_overrides(group)`, `apply_config(args: PackedStringArray) -> bool`, `save() -> bool`, `reset_to_shipped()`, `resolution_index_of(size: Vector2i) -> int`; static `is_measured_args(args) -> bool`.
  - `describe` dictionaries carry `name, label, hint, kind, min, max, ui_min, ui_max, step, options, value, default`.

- [ ] **Step 1: Write the failing native tests**

Create `extension/tests/test_display_settings.cpp`:

```cpp
#include <doctest/doctest.h>
#include "settings/display_settings.h"
#include "settings_row_checks.h"
#include <cstdio>
#include <cstring>
#include <iterator>

TEST_CASE("the display rows satisfy the table invariants") {
	ve::DisplaySettings shipped; // project.godot: scaling_3d/scale=0.65 at 2560x1440
	shipped.render_scale = 0.65f;
	shipped.resolution = 3;
	check_rows(ve::display_rows(), {ve::DisplaySettings{}, shipped});
}

// Dropdown labels are what the player picks from; a label that disagreed with the size it applies
// would be silently wrong in the only place it is visible.
TEST_CASE("the resolution table is ascending and its labels name the sizes they apply") {
	const ve::SettingRow<ve::DisplaySettings> *row = ve::find_row(ve::display_rows(), "resolution");
	REQUIRE(row != nullptr);
	REQUIRE(row->options.size() == std::size(ve::kResolutions));
	CHECK(row->min == -1.0f);
	for (size_t i = 0; i < std::size(ve::kResolutions); i++) {
		char want[32];
		std::snprintf(want, sizeof(want), "%d x %d", ve::kResolutions[i].w, ve::kResolutions[i].h);
		CHECK(std::strcmp(row->options[i], want) == 0);
		if (i > 0) CHECK(ve::kResolutions[i].w > ve::kResolutions[i - 1].w);
	}
}

TEST_CASE("a window size maps to its preset index, or -1 when it is not a preset") {
	CHECK(ve::resolution_index_of(2560, 1440) == 3); // project.godot's shipped size
	CHECK(ve::resolution_index_of(1920, 1080) == 2);
	CHECK(ve::resolution_index_of(1337, 999) == -1);
}

TEST_CASE("the upscaler options match the mode table VoxelSettings maps them to") {
	const ve::SettingRow<ve::DisplaySettings> *row = ve::find_row(ve::display_rows(), "upscaler");
	REQUIRE(row != nullptr);
	CHECK(row->options.size() == static_cast<size_t>(ve::kUpscalerOptionCount));
}
```

Run: `cd extension && scons -Q test; cd ..`
Expected: compile error, `settings/display_settings.h: No such file or directory`.

- [ ] **Step 2: Write the display table**

Create `extension/src/settings/display_settings.h`:

```cpp
#pragma once
#include "settings/settings_store.h"
#include <span>

namespace ve {

// The window and upscaling dials (spec 2026-09-16 §3.3). VoxelSettings owns this store: its base
// is whatever the Viewport and window were when the node became ready, and it pushes every
// resolved value back to them.
struct DisplaySettings {
	float render_scale = 1.0f; // Viewport.scaling_3d_scale
	int upscaler = 0;          // index into the upscaler options; VoxelSettings maps it to a mode
	int resolution = -1;       // index into kResolutions; -1 = the window is not a preset size
	bool fullscreen = false;
};

struct WindowSize {
	int w;
	int h;
};

// Offered window sizes, ascending. project.godot ships 2560x1440.
inline constexpr WindowSize kResolutions[] = {
	{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160},
};

inline constexpr int kUpscalerOptionCount = 5;

// -1 when (w, h) is not one of kResolutions: never rounded to a neighbour.
int resolution_index_of(int w, int h);

std::span<const SettingRow<DisplaySettings>> display_rows();

class DisplaySettingsStore : public SettingsStore<DisplaySettings> {
public:
	DisplaySettingsStore() :
			SettingsStore(display_rows(), nullptr, DisplaySettings{}) {}
};

} // namespace ve
```

Create `extension/src/settings/display_settings.cpp`:

```cpp
#include "settings/display_settings.h"
#include <iterator>

namespace ve {
namespace {

// Same set demo/benchmark.gd's --upscaler= accepts, so a configuration found in the panel can be
// reproduced on the command line.
const char *const kUpscalerLabels[] = {"Bilinear", "FSR 1", "FSR 2", "MetalFX spatial", "MetalFX temporal"};
const char *const kResolutionLabels[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
static_assert(std::size(kUpscalerLabels) == kUpscalerOptionCount);
static_assert(std::size(kResolutionLabels) == std::size(kResolutions));

const SettingRow<DisplaySettings> kDisplayRows[] = {
	float_row("render_scale", "Render scale", &DisplaySettings::render_scale, 0.25f, 1.0f, 0.5f, 1.0f,
			0.01f, "Fraction of the window the 3D scene renders at before upscaling."),
	enum_row("upscaler", "Upscaler", &DisplaySettings::upscaler,
			std::span<const char *const>(kUpscalerLabels)),
	enum_row("resolution", "Resolution", &DisplaySettings::resolution,
			std::span<const char *const>(kResolutionLabels), -1),
	bool_row("fullscreen", "Fullscreen", &DisplaySettings::fullscreen),
};

} // namespace

int resolution_index_of(int w, int h) {
	for (int i = 0; i < static_cast<int>(std::size(kResolutions)); i++)
		if (kResolutions[i].w == w && kResolutions[i].h == h) return i;
	return -1;
}

std::span<const SettingRow<DisplaySettings>> display_rows() {
	return kDisplayRows;
}

} // namespace ve
```

Run: `cd extension && scons -Q test; cd ..`
Expected: PASS.

- [ ] **Step 3: Write the failing gdUnit contract**

Create `tests/test_voxel_settings.gd`:

```gdscript
extends GdUnitTestSuite
# VoxelSettings (docs/superpowers/specs/2026-09-16-settings-store-design.md §3.5). Every node here
# points at a SubViewport the test owns, a throwaway config path and manage_window = false, so
# nothing reaches the real window or a developer's user://settings.cfg.

const CONFIG_PATH := "user://test_voxel_settings.cfg"
const OTHER_PATH := "user://test_voxel_settings_other.cfg"

var _roots: Array = []

func remove_configs() -> void:
	for path in [CONFIG_PATH, OTHER_PATH]:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))

func before_test() -> void:
	remove_configs()

func after_test() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.free()
	_roots.clear()
	remove_configs()

# [world, viewport, settings]. near_field_scale and the viewport scale are what demo/main.tscn and
# project.godot ship, so "shipped" has known values.
func make_settings(config_path := CONFIG_PATH) -> Array:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	world.near_field_scale = 0.4
	root.add_child(world)
	var vp := SubViewport.new()
	vp.name = "Viewport"
	vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	vp.scaling_3d_scale = 0.65
	root.add_child(vp)
	var settings: VoxelSettings = ClassDB.instantiate("VoxelSettings")
	settings.name = "Settings"
	settings.world_path = NodePath("../World")
	settings.viewport_path = NodePath("../Viewport")
	settings.config_path = config_path
	settings.manage_window = false
	root.add_child(settings)
	return [world, vp, settings]

func test_groups_are_listed_in_panel_order() -> void:
	var settings: VoxelSettings = make_settings()[2]
	assert_array(Array(settings.groups())).is_equal(["display", "render", "beauty", "grass"])

func test_describe_lists_every_row_with_its_value() -> void:
	var settings: VoxelSettings = make_settings()[2]
	for group in settings.groups():
		var rows: Array = settings.describe(group)
		assert_array(rows).override_failure_message(group).is_not_empty()
		for row in rows:
			for key in ["name", "label", "hint", "kind", "min", "max", "ui_min", "ui_max", "step",
					"options", "value", "default"]:
				assert_bool(row.has(key)).override_failure_message("%s/%s lacks %s" % [group, row.get("name"), key]).is_true()
			assert_array(["bool", "int", "float", "color", "enum"]).contains([row["kind"]])
			assert_that(row["value"]).is_equal(settings.get_setting(group, row["name"]))
	var kinds := {}
	for row in settings.describe("beauty"):
		kinds[row["name"]] = row["kind"]
	assert_str(kinds["ssgi_taps"]).is_equal("int")
	assert_str(kinds["ambient"]).is_equal("color")
	var upscaler: Dictionary = settings.describe("display").filter(func(r): return r["name"] == "upscaler")[0]
	assert_int(upscaler["options"].size()).is_equal(5)

func test_render_beauty_and_grass_dials_reach_the_world() -> void:
	var parts := make_settings()
	var world: VoxelWorld = parts[0]
	var settings: VoxelSettings = parts[2]
	assert_bool(settings.set_setting("render", "near_field_scale", 0.8)).is_true()
	assert_float(world.near_field_scale).is_equal_approx(0.8, 0.001)
	assert_bool(settings.set_setting("beauty", "ssgi_taps", 4)).is_true()
	assert_float(world.get_effect_value("ssgi_taps")).is_equal_approx(4.0, 0.001)
	assert_bool(settings.set_setting("grass", "reach_m", 30.0)).is_true()
	assert_float(world.get_grass_value("reach_m")).is_equal_approx(30.0, 0.001)
	assert_bool(settings.set_setting("beauty", "no_such_knob", 1.0)).is_false()
	assert_bool(settings.set_setting("nowhere", "ssgi", true)).is_false()
	assert_bool(settings.set_setting("beauty", "ambient", 0.5)).is_false()

func test_display_dials_reach_the_viewport() -> void:
	var parts := make_settings()
	var vp: SubViewport = parts[1]
	var settings: VoxelSettings = parts[2]
	settings.set_setting("display", "render_scale", 0.9)
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.9, 0.001)
	settings.set_setting("display", "upscaler", 2)
	assert_int(vp.scaling_3d_mode).is_equal(Viewport.SCALING_3D_MODE_FSR2)

func test_the_display_base_is_the_viewport_at_ready() -> void:
	var settings: VoxelSettings = make_settings()[2]
	assert_float(float(settings.get_setting("display", "render_scale"))).is_equal_approx(0.65, 0.001)
	assert_dict(settings.get_overrides("display")).is_empty()

func test_what_the_scene_set_is_an_override() -> void:
	var settings: VoxelSettings = make_settings()[2]
	var render: Dictionary = settings.get_overrides("render")
	assert_float(float(render["near_field_scale"])).is_equal_approx(0.4, 0.001)

func test_overrides_round_trip_through_save_and_a_fresh_node() -> void:
	var settings: VoxelSettings = make_settings()[2]
	settings.set_setting("beauty", "ssgi_strength", 2.0)
	settings.set_setting("render", "near_field_scale", 0.75)
	settings.set_setting("display", "render_scale", 0.85)
	assert_bool(settings.save()).is_true()

	var second := make_settings()
	var world2: VoxelWorld = second[0]
	var vp2: SubViewport = second[1]
	assert_float(world2.get_effect_value("ssgi_strength")).is_equal_approx(2.0, 0.001)
	assert_float(world2.near_field_scale).is_equal_approx(0.75, 0.001)
	assert_float(vp2.scaling_3d_scale).is_equal_approx(0.85, 0.001)

# demo/benchmark.gd and demo/capture.gd set these dials from their own flags, and PORTFOLIO reports
# against them. A saved file that moved a measured number, or a measured run that overwrote the
# player's file, would make every run unreadable.
func test_a_measured_run_neither_loads_nor_saves() -> void:
	var first: VoxelSettings = make_settings()[2]
	first.set_setting("render", "near_field_scale", 0.75)
	assert_bool(first.save()).is_true()

	var second := make_settings(OTHER_PATH)
	var world2: VoxelWorld = second[0]
	var settings2: VoxelSettings = second[2]
	settings2.config_path = CONFIG_PATH
	assert_bool(settings2.apply_config(PackedStringArray(["--benchmark"]))).is_false()
	assert_float(world2.near_field_scale).is_equal_approx(0.4, 0.001)
	world2.near_field_scale = 0.3
	assert_bool(settings2.save()).is_false()
	var cfg := ConfigFile.new()
	assert_int(cfg.load(CONFIG_PATH)).is_equal(OK)
	assert_float(float(cfg.get_value("render", "near_field_scale", 0.0))).is_equal_approx(0.75, 0.001)

	assert_bool(settings2.apply_config(PackedStringArray([]))).is_true()
	assert_float(world2.near_field_scale).is_equal_approx(0.75, 0.001)

func test_every_benchmark_leg_is_a_measured_run() -> void:
	for leg in ["--benchmark", "--benchmark-move", "--benchmark-ridge", "--benchmark-edit",
			"--benchmark-edit-bounded", "--benchmark-island", "--capture"]:
		assert_bool(VoxelSettings.is_measured_args(PackedStringArray([leg]))) \
			.override_failure_message("%s must not be overridden by saved settings" % leg).is_true()
	assert_bool(VoxelSettings.is_measured_args(PackedStringArray(["--something-else"]))).is_false()

func test_reset_restores_what_the_scene_shipped() -> void:
	var parts := make_settings()
	var world: VoxelWorld = parts[0]
	var vp: SubViewport = parts[1]
	var settings: VoxelSettings = parts[2]
	settings.set_setting("render", "near_field_scale", 1.0)
	settings.set_setting("render", "near_field", false)
	settings.set_setting("display", "render_scale", 1.0)
	settings.set_setting("beauty", "ssr", false)
	settings.reset_to_shipped()
	assert_float(world.near_field_scale).is_equal_approx(0.4, 0.001)
	assert_bool(world.get_effect_enabled("near_field")).is_true()
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.65, 0.001)
	assert_bool(world.get_effect_enabled("ssr")).is_true()
	assert_dict(settings.get_overrides("beauty")).is_empty()

func test_clearing_beauty_overrides_returns_to_the_tier() -> void:
	var parts := make_settings()
	var world: VoxelWorld = parts[0]
	var settings: VoxelSettings = parts[2]
	settings.set_setting("render", "quality_tier", 1)
	settings.set_setting("beauty", "ssgi_strength", 2.0)
	settings.clear_overrides("beauty")
	assert_float(world.get_effect_value("ssgi_strength")).is_equal_approx(1.0, 0.001)
	assert_float(world.get_effect_value("ssgi_taps")).is_equal_approx(0.0, 0.001)

# Cel-shaded objects read ve_ambient; VoxelWorld publishes it from the beauty snapshot each frame.
func test_an_ambient_change_reaches_the_object_global() -> void:
	var settings: VoxelSettings = make_settings()[2]
	assert_bool(settings.set_setting("beauty", "ambient", Color(0.5, 0.4, 0.3))).is_true()
	await get_tree().process_frame
	await get_tree().process_frame
	var published: Vector3 = RenderingServer.global_shader_parameter_get("ve_ambient")
	settings.clear_overrides("beauty")
	await get_tree().process_frame
	assert_vector(published).is_equal_approx(Vector3(0.5, 0.4, 0.3), Vector3(0.001, 0.001, 0.001))
```

- [ ] **Step 4: Run to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_voxel_settings.gd`
Expected: every case FAILS or errors with `Identifier "VoxelSettings" not declared` / `ClassDB.instantiate` returning null.

- [ ] **Step 5: Write `VoxelSettings`**

Create `extension/src/voxel_settings.h`:

```cpp
#pragma once
// VoxelSettings -- the one owner of "what has been set". Every dial the demo exposes lives in a
// settings group: display (owned here: the Viewport's scaling and the window) and render, beauty,
// grass (RenderOrchestrator's, reached through VoxelWorld). This node adds what a group cannot do
// by itself: access by name from GDScript, describe() for the settings panel, ConfigFile
// persistence behind the measured-run guard, and inspector properties. VoxelWorld does not know
// this class exists. Spec: docs/superpowers/specs/2026-09-16-settings-store-design.md §3.5.
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <cstdint>
#include "settings/display_settings.h"

namespace godot {

class VoxelSettings : public Node {
	GDCLASS(VoxelSettings, Node)

public:
	VoxelSettings();

	void set_world_path(const NodePath &p) { world_path_ = p; }
	NodePath get_world_path() const { return world_path_; }
	// Empty: the viewport this node is in.
	void set_viewport_path(const NodePath &p) { viewport_path_ = p; }
	NodePath get_viewport_path() const { return viewport_path_; }
	void set_config_path(const String &p) { config_path_ = p; }
	String get_config_path() const { return config_path_; }
	// False keeps the resolution and fullscreen rows away from DisplayServer (every test).
	void set_manage_window(bool v) { manage_window_ = v; }
	bool get_manage_window() const { return manage_window_; }

	PackedStringArray groups() const;
	Array describe(const String &group) const;
	Variant get_setting(const String &group, const String &name) const;
	// False for an unknown group or name, or a value that cannot be read as the row's kind.
	bool set_setting(const String &group, const String &name, const Variant &value);
	Dictionary get_overrides(const String &group) const;
	void clear_overrides(const String &group);
	// Loads config_path unless `args` name a measured run, and remembers that verdict for save().
	bool apply_config(const PackedStringArray &args);
	// Writes each group's overrides as one section. False in a measured run or in the editor.
	bool save();
	// Clears every override and re-applies what the scene had set when this node became ready.
	void reset_to_shipped();
	int resolution_index_of(Vector2i size) const;
	static bool is_measured_args(const PackedStringArray &args);

	void _ready() override;

protected:
	static void _bind_methods();

private:
	ve::SettingsGroup *group(const String &name) const;
	void capture_display_base();
	void apply_display(const ve::DisplaySettings &s);
	static void on_display_resolved(const ve::DisplaySettings &s, void *ctx);

	NodePath world_path_;
	NodePath viewport_path_;
	String config_path_ = "user://settings.cfg";
	bool manage_window_ = true;
	// ObjectIDs, not pointers: the world or the viewport may be freed before this node.
	uint64_t world_id_ = 0;
	uint64_t viewport_id_ = 0;
	bool ready_ = false;
	bool measured_ = false;
	Dictionary shipped_; // group -> overrides when this node became ready
	mutable ve::DisplaySettingsStore display_;
};

} // namespace godot
```

Create `extension/src/voxel_settings.cpp`:

```cpp
#include "voxel_settings.h"
#include "render/orchestrator.h"
#include "settings/godot/setting_variant.h"
#include "voxel_world.h"
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <algorithm>
#include <iterator>

using namespace godot;

namespace {

// Panel tab order.
constexpr const char *kGroups[] = {"display", "render", "beauty", "grass"};

// In the order of the display row's upscaler options (settings/display_settings.cpp).
const Viewport::Scaling3DMode kUpscalerModes[] = {
	Viewport::SCALING_3D_MODE_BILINEAR,
	Viewport::SCALING_3D_MODE_FSR,
	Viewport::SCALING_3D_MODE_FSR2,
	Viewport::SCALING_3D_MODE_METALFX_SPATIAL,
	Viewport::SCALING_3D_MODE_METALFX_TEMPORAL,
};
static_assert(std::size(kUpscalerModes) == ve::kUpscalerOptionCount);

int upscaler_index_of(Viewport::Scaling3DMode mode) {
	for (int i = 0; i < static_cast<int>(std::size(kUpscalerModes)); i++)
		if (kUpscalerModes[i] == mode) return i;
	return 0;
}

bool is_editor() {
	return Engine::get_singleton()->is_editor_hint();
}

bool is_fullscreen(DisplayServer *ds) {
	const DisplayServer::WindowMode m = ds->window_get_mode();
	return m == DisplayServer::WINDOW_MODE_FULLSCREEN ||
			m == DisplayServer::WINDOW_MODE_EXCLUSIVE_FULLSCREEN;
}

} // namespace

void VoxelSettings::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_world_path", "p"), &VoxelSettings::set_world_path);
	ClassDB::bind_method(D_METHOD("get_world_path"), &VoxelSettings::get_world_path);
	ClassDB::bind_method(D_METHOD("set_viewport_path", "p"), &VoxelSettings::set_viewport_path);
	ClassDB::bind_method(D_METHOD("get_viewport_path"), &VoxelSettings::get_viewport_path);
	ClassDB::bind_method(D_METHOD("set_config_path", "p"), &VoxelSettings::set_config_path);
	ClassDB::bind_method(D_METHOD("get_config_path"), &VoxelSettings::get_config_path);
	ClassDB::bind_method(D_METHOD("set_manage_window", "v"), &VoxelSettings::set_manage_window);
	ClassDB::bind_method(D_METHOD("get_manage_window"), &VoxelSettings::get_manage_window);
	ClassDB::bind_method(D_METHOD("groups"), &VoxelSettings::groups);
	ClassDB::bind_method(D_METHOD("describe", "group"), &VoxelSettings::describe);
	ClassDB::bind_method(D_METHOD("get_setting", "group", "name"), &VoxelSettings::get_setting);
	ClassDB::bind_method(D_METHOD("set_setting", "group", "name", "value"), &VoxelSettings::set_setting);
	ClassDB::bind_method(D_METHOD("get_overrides", "group"), &VoxelSettings::get_overrides);
	ClassDB::bind_method(D_METHOD("clear_overrides", "group"), &VoxelSettings::clear_overrides);
	ClassDB::bind_method(D_METHOD("apply_config", "args"), &VoxelSettings::apply_config);
	ClassDB::bind_method(D_METHOD("save"), &VoxelSettings::save);
	ClassDB::bind_method(D_METHOD("reset_to_shipped"), &VoxelSettings::reset_to_shipped);
	ClassDB::bind_method(D_METHOD("resolution_index_of", "size"), &VoxelSettings::resolution_index_of);
	ClassDB::bind_static_method("VoxelSettings", D_METHOD("is_measured_args", "args"),
			&VoxelSettings::is_measured_args);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "world_path"), "set_world_path", "get_world_path");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "viewport_path"), "set_viewport_path",
			"get_viewport_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "config_path"), "set_config_path", "get_config_path");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "manage_window"), "set_manage_window",
			"get_manage_window");
}

VoxelSettings::VoxelSettings() {
	display_.set_listener(&VoxelSettings::on_display_resolved, this);
}

ve::SettingsGroup *VoxelSettings::group(const String &name) const {
	if (name == "display") return &display_;
	VoxelWorld *world = Object::cast_to<VoxelWorld>(ObjectDB::get_instance(world_id_));
	if (!world) return nullptr;
	const CharString n = name.utf8();
	return world->context().render->settings_group(n.get_data());
}

void VoxelSettings::_ready() {
	// The editor edits property values; it never applies them to a world, a viewport, a window or
	// a file. Not resolving the world there also keeps the inspector on this node's own values.
	if (is_editor()) return;
	if (!world_path_.is_empty())
		if (VoxelWorld *world = Object::cast_to<VoxelWorld>(get_node_or_null(world_path_)))
			world_id_ = world->get_instance_id();
	Viewport *vp = viewport_path_.is_empty() ? get_viewport()
											 : Object::cast_to<Viewport>(get_node_or_null(viewport_path_));
	viewport_id_ = vp ? vp->get_instance_id() : 0;
	capture_display_base();
	ready_ = true;
	apply_display(display_.get());
	for (const char *g : kGroups) shipped_[g] = get_overrides(g);
	apply_config(OS::get_singleton()->get_cmdline_user_args());
}

void VoxelSettings::capture_display_base() {
	ve::DisplaySettings base;
	if (Viewport *vp = Object::cast_to<Viewport>(ObjectDB::get_instance(viewport_id_))) {
		base.render_scale = vp->get_scaling_3d_scale();
		base.upscaler = upscaler_index_of(vp->get_scaling_3d_mode());
	}
	if (manage_window_) {
		if (DisplayServer *ds = DisplayServer::get_singleton()) {
			const Vector2i size = ds->window_get_size();
			base.resolution = ve::resolution_index_of(size.x, size.y);
			base.fullscreen = is_fullscreen(ds);
		}
	}
	// Rebase, not set: overrides a scene applied before this node was ready survive.
	display_.rebase(base);
}

void VoxelSettings::on_display_resolved(const ve::DisplaySettings &s, void *ctx) {
	auto *self = static_cast<VoxelSettings *>(ctx);
	if (self->ready_) self->apply_display(s);
}

void VoxelSettings::apply_display(const ve::DisplaySettings &s) {
	if (Viewport *vp = Object::cast_to<Viewport>(ObjectDB::get_instance(viewport_id_))) {
		if (vp->get_scaling_3d_scale() != s.render_scale) vp->set_scaling_3d_scale(s.render_scale);
		const int last = static_cast<int>(std::size(kUpscalerModes)) - 1;
		const Viewport::Scaling3DMode mode = kUpscalerModes[std::clamp(s.upscaler, 0, last)];
		if (vp->get_scaling_3d_mode() != mode) vp->set_scaling_3d_mode(mode);
	}
	if (!manage_window_) return;
	DisplayServer *ds = DisplayServer::get_singleton();
	if (!ds) return;
	if (s.fullscreen != is_fullscreen(ds))
		ds->window_set_mode(s.fullscreen ? DisplayServer::WINDOW_MODE_FULLSCREEN
										 : DisplayServer::WINDOW_MODE_WINDOWED);
	// ponytail: a resolution picked while fullscreen waits until fullscreen is turned off; the old
	// F7 popup left fullscreen for you. Add that back if anyone misses it.
	if (s.fullscreen || s.resolution < 0) return;
	const ve::WindowSize &want = ve::kResolutions[s.resolution];
	const Vector2i size(want.w, want.h);
	// Not applied when it already matches: booting should not churn the window.
	if (ds->window_get_size() != size) ds->window_set_size(size);
}

PackedStringArray VoxelSettings::groups() const {
	PackedStringArray out;
	for (const char *g : kGroups) out.push_back(String(g));
	return out;
}

Array VoxelSettings::describe(const String &group_name) const {
	Array out;
	ve::SettingsGroup *g = group(group_name);
	if (!g) return out;
	for (const ve::RowInfo &r : g->rows()) {
		Dictionary d;
		d["name"] = String(r.name);
		d["label"] = String(r.label);
		d["hint"] = r.hint ? String(r.hint) : String();
		d["kind"] = setting_kind_name(r.kind);
		d["min"] = r.min;
		d["max"] = r.max;
		d["ui_min"] = r.ui_min;
		d["ui_max"] = r.ui_max;
		d["step"] = r.step;
		PackedStringArray options;
		for (const char *o : r.options) options.push_back(String(o));
		d["options"] = options;
		ve::SettingValue v;
		if (g->get(r.name, &v)) d["value"] = setting_to_variant(v);
		if (g->get_default(r.name, &v)) d["default"] = setting_to_variant(v);
		out.push_back(d);
	}
	return out;
}

Variant VoxelSettings::get_setting(const String &group_name, const String &name) const {
	ve::SettingsGroup *g = group(group_name);
	const CharString n = name.utf8();
	ve::SettingValue v;
	if (!g || !g->get(n.get_data(), &v)) return Variant();
	return setting_to_variant(v);
}

bool VoxelSettings::set_setting(const String &group_name, const String &name, const Variant &value) {
	ve::SettingsGroup *g = group(group_name);
	const CharString n = name.utf8();
	ve::SettingValue current;
	if (!g || !g->get(n.get_data(), &current)) return false;
	ve::SettingValue v;
	if (!setting_from_variant(current.kind, value, &v)) return false;
	return g->set(n.get_data(), v);
}

Dictionary VoxelSettings::get_overrides(const String &group_name) const {
	Dictionary d;
	if (ve::SettingsGroup *g = group(group_name))
		for (const auto &[name, value] : g->overrides()) d[String(name)] = setting_to_variant(value);
	return d;
}

void VoxelSettings::clear_overrides(const String &group_name) {
	if (ve::SettingsGroup *g = group(group_name)) g->clear_all();
}

bool VoxelSettings::is_measured_args(const PackedStringArray &args) {
	for (int i = 0; i < args.size(); i++)
		if (args[i].begins_with("--benchmark") || args[i] == "--capture") return true;
	return false;
}

bool VoxelSettings::apply_config(const PackedStringArray &args) {
	measured_ = is_measured_args(args);
	if (measured_ || is_editor()) return false;
	Ref<ConfigFile> cfg;
	cfg.instantiate();
	if (cfg->load(config_path_) != OK) return false;
	// Overrides are sticky, so the order the sections apply in does not matter: a tier loaded
	// after a beauty knob rebases beauty without dropping it.
	for (const char *g : kGroups) {
		if (!cfg->has_section(g)) continue;
		const PackedStringArray keys = cfg->get_section_keys(g);
		for (int i = 0; i < keys.size(); i++) set_setting(g, keys[i], cfg->get_value(g, keys[i]));
	}
	return true;
}

bool VoxelSettings::save() {
	if (measured_ || is_editor()) return false;
	Ref<ConfigFile> cfg;
	cfg.instantiate();
	cfg->load(config_path_); // keep sections this node does not own
	for (const char *g : kGroups) {
		if (cfg->has_section(g)) cfg->erase_section(g);
		const Dictionary overrides = get_overrides(g);
		const Array keys = overrides.keys();
		for (int i = 0; i < keys.size(); i++)
			cfg->set_value(g, String(keys[i]), overrides[keys[i]]);
	}
	return cfg->save(config_path_) == OK;
}

void VoxelSettings::reset_to_shipped() {
	for (const char *g : kGroups) {
		clear_overrides(g);
		const Dictionary shipped = shipped_.get(g, Dictionary());
		const Array keys = shipped.keys();
		for (int i = 0; i < keys.size(); i++) set_setting(g, String(keys[i]), shipped[keys[i]]);
	}
}

int VoxelSettings::resolution_index_of(Vector2i size) const {
	return ve::resolution_index_of(size.x, size.y);
}
```

In `extension/src/register_types.cpp`, add `#include "voxel_settings.h"` with the other includes and `GDREGISTER_CLASS(VoxelSettings);` after `GDREGISTER_CLASS(VoxelEditTool);`.

- [ ] **Step 6: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh -a res://tests/test_voxel_settings.gd,res://tests/test_settings_names.gd,res://tests/test_cel_object.gd
```

Expected: all PASS. If `RenderingServer.global_shader_parameter_get` returns null at runtime on this build, stop and report rather than deleting the ambient case.

- [ ] **Step 7: Commit**

```bash
git add extension/src/settings/display_settings.h extension/src/settings/display_settings.cpp extension/tests/test_display_settings.cpp extension/src/voxel_settings.h extension/src/voxel_settings.cpp extension/src/register_types.cpp tests/test_voxel_settings.gd
git commit -m "feat: VoxelSettings node owns display settings, describe and persistence

One ConfigFile section per settings group holds exactly that group's
overrides; a measured run (--benchmark*, --capture) neither loads nor saves."
```

---

### Task 16: The unified F1 settings panel

**Files:**
- Rewrite: `demo/settings_menu.gd`, `demo/settings_menu.tscn`
- Delete: `demo/debug_menu.gd`, `demo/debug_menu.gd.uid`, `tests/test_debug_menu.gd`, `tests/test_debug_menu.gd.uid`
- Modify: `demo/main.tscn` (ext_resource id "6", `VoxelWorld` block, `DebugMenu` block, `SettingsMenu` block)
- Modify: `demo/help.gd:15, 21`, `tests/test_demo_shell.gd` (help key list)
- Modify: `demo/benchmark.gd:163-181`
- Rewrite: `tests/test_settings_menu.gd`

**Interfaces:**
- Consumes: Task 15 `VoxelSettings` API.
- Produces: `demo/settings_menu.gd` with `@export settings_path: NodePath`, `@export toggle_key := KEY_F1`, `set_open(open: bool)`, `control(group: String, name: String) -> Control`, `sync_from_settings()`, `show_resolution(size: Vector2i)`.

- [ ] **Step 1: Write the failing panel contract**

Replace `tests/test_settings_menu.gd` with:

```gdscript
extends GdUnitTestSuite
# demo/settings_menu.tscn — the unified settings panel (F1), built from VoxelSettings.describe().
#
# The panel is pointed at a VoxelSettings node that is pointed at a SubViewport the test owns (so a
# render-scale assertion cannot change how the rest of the suite renders), a throwaway config path
# (so a developer's user://settings.cfg is neither read nor written) and manage_window = false (so
# nothing resizes the window the tests run in). Cases ported from the old test_debug_menu.gd keep
# their names.

const MENU_SCENE := preload("res://demo/settings_menu.tscn")
const CONFIG_PATH := "user://test_settings_menu.cfg"

var _roots: Array = []

func before_test() -> void:
	DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))

func after_test() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.free()
	_roots.clear()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE

# [world, viewport, settings, menu]. near_field_scale and the viewport scale are what main.tscn and
# project.godot ship, so "reset restores what the scene shipped" has known values.
func make_menu() -> Array:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	world.near_field_scale = 0.4
	root.add_child(world)
	var vp := SubViewport.new()
	vp.name = "Viewport"
	vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	vp.scaling_3d_scale = 0.65
	root.add_child(vp)
	var settings: VoxelSettings = ClassDB.instantiate("VoxelSettings")
	settings.name = "Settings"
	settings.world_path = NodePath("../World")
	settings.viewport_path = NodePath("../Viewport")
	settings.config_path = CONFIG_PATH
	settings.manage_window = false
	root.add_child(settings)
	var menu = MENU_SCENE.instantiate()
	menu.name = "Menu"
	menu.settings_path = NodePath("../Settings")
	root.add_child(menu)
	return [world, vp, settings, menu]

func key_event(code: int) -> InputEventKey:
	var ev := InputEventKey.new()
	ev.keycode = code
	ev.pressed = true
	return ev

# The binding itself. F1 toggles, Esc dismisses -- and Esc only while the panel is up, so it keeps
# meaning "release the mouse" to player.gd otherwise.
func test_f1_toggles_the_panel_and_escape_dismisses_it() -> void:
	var menu = make_menu()[3]
	await get_tree().process_frame
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_F1))
	assert_bool(menu.visible).is_true()
	menu._unhandled_input(key_event(KEY_F1))
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_ESCAPE))
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_F1))
	menu._unhandled_input(key_event(KEY_ESCAPE))
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_F7))
	assert_bool(menu.visible).is_false()

func test_every_group_has_a_tab_with_a_control_per_row() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var settings: VoxelSettings = parts[2]
	var menu = parts[3]
	for group in settings.groups():
		for row in settings.describe(group):
			assert_object(menu.control(group, row["name"])).override_failure_message(
				"no control for %s/%s" % [group, row["name"]]).is_not_null()

# S5, ported: a slider never offers a value outside the clamp, and never excludes the value the
# dial holds. Ranges come from the rows, so this holds for every knob added later too.
func test_every_slider_range_contains_the_current_value_and_sits_inside_the_clamp() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var settings: VoxelSettings = parts[2]
	var menu = parts[3]
	for group in settings.groups():
		for row in settings.describe(group):
			var slider := menu.control(group, row["name"]) as HSlider
			if slider == null:
				continue
			var name := "%s/%s" % [group, row["name"]]
			assert_float(slider.min_value).override_failure_message(name).is_greater_equal(float(row["min"]))
			assert_float(slider.max_value).override_failure_message(name).is_less_equal(float(row["max"]))
			assert_float(float(row["value"])).override_failure_message(
				"%s: %s outside the slider" % [name, row["value"]]).is_between(slider.min_value, slider.max_value)

func test_near_field_slider_writes_the_world_dial() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	parts[3].control("render", "near_field_scale").emit_signal("value_changed", 0.8)
	assert_float(parts[0].near_field_scale).is_equal_approx(0.8, 0.001)

func test_render_scale_slider_writes_the_viewport() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	parts[3].control("display", "render_scale").emit_signal("value_changed", 0.9)
	assert_float(parts[1].scaling_3d_scale).is_equal_approx(0.9, 0.001)

func test_upscaler_option_writes_the_viewport_mode() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var options := parts[3].control("display", "upscaler") as OptionButton
	options.emit_signal("item_selected", options.get_item_index(2)) # "FSR 2"
	assert_int(parts[1].scaling_3d_mode).is_equal(Viewport.SCALING_3D_MODE_FSR2)

func test_near_field_toggle_is_a_real_render_effect() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_bool(world.get_effect_enabled("near_field")).is_true()
	parts[3].control("render", "near_field").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("near_field")).is_false()

func test_islands_toggle_is_a_real_render_effect() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_bool(world.get_effect_enabled("islands")).is_true()
	parts[3].control("render", "islands").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("islands")).is_false()
	assert_bool(world.hooks().debug_beauty_settings()["islands"]).is_false()

# S6 through the panel: choosing a tier moves the untweaked knobs and keeps the tweaked one.
func test_quality_selection_moves_the_tier_and_keeps_tweaks() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var menu = parts[3]
	menu.control("beauty", "ssr").emit_signal("toggled", false)
	var quality := menu.control("render", "quality_tier") as OptionButton
	quality.emit_signal("item_selected", quality.get_item_index(0))
	assert_int(world.quality_tier).is_equal(0)
	assert_bool(world.hooks().debug_beauty_settings()["outlines"]).is_false()
	assert_bool(world.get_effect_enabled("ssr")).is_false()
	quality.emit_signal("item_selected", quality.get_item_index(3))
	assert_bool(world.get_effect_enabled("outlines")).is_true()
	assert_bool(world.get_effect_enabled("ssr")).is_false()

func test_checkbox_writes_only_its_named_field() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	parts[3].control("beauty", "outlines").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("outlines")).is_false()
	assert_bool(world.get_effect_enabled("ssr")).is_true()

func test_the_ssao_checkbox_writes_the_world_setting() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_bool(world.get_effect_enabled("ssao")).is_true()
	parts[3].control("beauty", "ssao").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("ssao")).is_false()
	assert_bool(world.get_effect_enabled("outlines")).is_true()

func test_a_value_slider_writes_only_its_named_knob() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var reach: float = world.get_effect_value("emissive_gi_radius")
	parts[3].control("beauty", "emissive_gi_strength").emit_signal("value_changed", 21.0)
	assert_float(world.get_effect_value("emissive_gi_strength")).is_equal_approx(21.0, 0.001)
	assert_float(world.get_effect_value("emissive_gi_radius")).is_equal_approx(reach, 0.001)

func test_knob_values_are_clamped_on_the_way_in() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	world.set_effect_value("emissive_gi_strength", -5.0)
	assert_float(world.get_effect_value("emissive_gi_strength")).is_equal_approx(0.0, 0.001)
	world.set_effect_value("ssgi_temporal", 1.0)
	assert_float(world.get_effect_value("ssgi_temporal")).is_less(1.0)

func test_grass_density_slider_writes_only_its_named_knob() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var reach: float = world.get_grass_value("reach_m")
	parts[3].control("grass", "blades_per_brick").emit_signal("value_changed", 21.0)
	assert_float(world.get_grass_value("blades_per_brick")).is_equal_approx(21.0, 0.001)
	assert_float(world.get_grass_value("reach_m")).is_equal_approx(reach, 0.001)

func test_grass_checkbox_writes_the_enabled_flag() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_float(world.get_grass_value("enabled")).is_equal_approx(1.0, 0.001)
	parts[3].control("grass", "enabled").emit_signal("toggled", false)
	assert_float(world.get_grass_value("enabled")).is_equal_approx(0.0, 0.001)
	assert_bool(world.get_effect_enabled("outlines")).is_true()

func test_the_ambient_picker_writes_the_beauty_colour() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	parts[3].control("beauty", "ambient").emit_signal("color_changed", Color(0.3, 0.2, 0.1))
	var ambient: Color = parts[2].get_setting("beauty", "ambient")
	assert_float(ambient.r).is_equal_approx(0.3, 0.001)

# The panel keeps no shadow copy: the benchmark's flags, dev keys and the inspector write the same
# stores, so it re-reads on every open instead of trusting itself.
func test_opening_resyncs_from_the_settings() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var menu = parts[3]
	world.near_field_scale = 0.55
	world.set_quality_tier(2)
	menu.set_open(true)
	assert_float(float((menu.control("render", "near_field_scale") as HSlider).value)).is_equal_approx(0.55, 0.001)
	var quality := menu.control("render", "quality_tier") as OptionButton
	assert_int(quality.get_selected_id()).is_equal(2)
	menu.set_open(false)

func test_closing_the_panel_persists_the_dials() -> void:
	var menu = make_menu()[3]
	await get_tree().process_frame
	menu.set_open(true)
	menu.control("render", "near_field_scale").emit_signal("value_changed", 0.7)
	menu.set_open(false)
	var cfg := ConfigFile.new()
	assert_int(cfg.load(CONFIG_PATH)).is_equal(OK)
	assert_float(float(cfg.get_value("render", "near_field_scale", 0.0))).is_equal_approx(0.7, 0.001)

func test_reset_restores_what_the_scene_shipped() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var vp: SubViewport = parts[1]
	var menu = parts[3]
	menu.control("render", "near_field_scale").emit_signal("value_changed", 1.0)
	menu.control("display", "render_scale").emit_signal("value_changed", 1.0)
	menu.control("render", "near_field").emit_signal("toggled", false)
	menu.get_node("%Reset").emit_signal("pressed")
	assert_float(world.near_field_scale).is_equal_approx(0.4, 0.001)
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.65, 0.001)
	assert_bool(world.get_effect_enabled("near_field")).is_true()

func test_the_resolution_options_offer_the_project_default() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var settings: VoxelSettings = parts[2]
	var shipped := Vector2i(
		ProjectSettings.get_setting("display/window/size/viewport_width"),
		ProjectSettings.get_setting("display/window/size/viewport_height"))
	assert_int(settings.resolution_index_of(shipped)).is_greater_equal(0)

# A window whose size is not a preset used to leave the dropdown blank, which reads as a broken
# control. It shows the real size instead, without claiming to be one of the entries.
func test_an_off_table_window_size_is_shown_rather_than_left_blank() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var settings: VoxelSettings = parts[2]
	var menu = parts[3]
	var options := menu.control("display", "resolution") as OptionButton
	menu.show_resolution(Vector2i(1920, 1080))
	assert_int(options.get_selected_id()).is_equal(settings.resolution_index_of(Vector2i(1920, 1080)))
	menu.show_resolution(Vector2i(1337, 999))
	assert_int(options.selected).is_equal(-1)
	assert_str(options.text).contains("1337")
	assert_str(options.text).contains("999")
```

Run: `./gdunit_tests.sh -a res://tests/test_settings_menu.gd`
Expected: cases FAIL/error (`Invalid set index 'settings_path'`, `control` not found).

- [ ] **Step 2: Rewrite the panel scene**

Replace `demo/settings_menu.tscn` with:

```
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://demo/settings_menu.gd" id="1"]

[node name="SettingsMenu" type="Control"]
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2
script = ExtResource("1")

[node name="Scrim" type="ColorRect" parent="."]
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2
color = Color(0, 0, 0, 0.5)

[node name="Center" type="CenterContainer" parent="."]
anchors_preset = 15
anchor_right = 1.0
anchor_bottom = 1.0
grow_horizontal = 2
grow_vertical = 2
mouse_filter = 2

[node name="Panel" type="PanelContainer" parent="Center"]
layout_mode = 2

[node name="Margin" type="MarginContainer" parent="Center/Panel"]
layout_mode = 2
theme_override_constants/margin_left = 24
theme_override_constants/margin_top = 20
theme_override_constants/margin_right = 24
theme_override_constants/margin_bottom = 20

[node name="Box" type="VBoxContainer" parent="Center/Panel/Margin"]
layout_mode = 2
theme_override_constants/separation = 12

[node name="Title" type="Label" parent="Center/Panel/Margin/Box"]
layout_mode = 2
text = "Settings  (F1)"

[node name="Tabs" type="TabContainer" parent="Center/Panel/Margin/Box"]
unique_name_in_owner = true
custom_minimum_size = Vector2(620, 460)
layout_mode = 2

[node name="Buttons" type="HBoxContainer" parent="Center/Panel/Margin/Box"]
layout_mode = 2
alignment = 2
theme_override_constants/separation = 10

[node name="Reset" type="Button" parent="Center/Panel/Margin/Box/Buttons"]
unique_name_in_owner = true
layout_mode = 2
text = "Reset to shipped"

[node name="Close" type="Button" parent="Center/Panel/Margin/Box/Buttons"]
unique_name_in_owner = true
layout_mode = 2
text = "Close"
```

- [ ] **Step 3: Rewrite the panel script**

Replace `demo/settings_menu.gd` with:

```gdscript
extends Control
# The settings panel (F1): window, render budget, beauty and grass dials in one place.
#
# It keeps no tables and no state of its own. Every tab is built from VoxelSettings.describe(), so a
# new row in a C++ settings table appears here with its label, range and hint, and a slider range
# cannot drift from the clamp behind it (S5). Every open re-reads the values, because the benchmark's
# flags, the dev keys and the inspector write the same stores. Persistence and the measured-run guard
# live in VoxelSettings; closing the panel asks it to save.

@export var settings_path: NodePath
@export var toggle_key := KEY_F1

var _settings: VoxelSettings
var _controls := {}      # "group/name" -> the control for that row
var _value_labels := {}  # "group/name" -> the Label beside a slider
var _syncing := false
var _mouse_mode_before := Input.MOUSE_MODE_CAPTURED

@onready var _tabs: TabContainer = %Tabs

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	_settings = get_node_or_null(settings_path) as VoxelSettings
	if _settings:
		for group in _settings.groups():
			_build_tab(group)
	(%Reset as Button).pressed.connect(_on_reset)
	(%Close as Button).pressed.connect(_on_close)
	sync_from_settings()
	visible = false

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if event.keycode == toggle_key:
		set_open(not visible)
		get_viewport().set_input_as_handled()
	elif visible and event.keycode == KEY_ESCAPE:
		# Closing beats player.gd's Esc (release the mouse): while this panel is up the mouse is
		# already free, and Esc reads as "dismiss the dialog".
		set_open(false)
		get_viewport().set_input_as_handled()

# Opening frees the cursor so the panel can be clicked; closing restores whatever mode was in force
# and persists what was set.
func set_open(open: bool) -> void:
	if open == visible:
		return
	visible = open
	if open:
		_mouse_mode_before = Input.mouse_mode
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		sync_from_settings()
	else:
		Input.mouse_mode = _mouse_mode_before
		if _settings:
			_settings.save()

func _exit_tree() -> void:
	# Quitting with the panel open still persists what was set in it.
	if visible and _settings:
		_settings.save()

# The control for one row, or null.
func control(group: String, name: String) -> Control:
	return _controls.get(group + "/" + name)

func sync_from_settings() -> void:
	if _settings == null:
		return
	_syncing = true
	for key in _controls:
		var parts: PackedStringArray = String(key).split("/")
		var value = _settings.get_setting(parts[0], parts[1])
		var c: Control = _controls[key]
		if c is CheckBox:
			(c as CheckBox).set_pressed_no_signal(bool(value))
		elif c is HSlider:
			(c as HSlider).set_value_no_signal(float(value))
		elif c is OptionButton:
			var options := c as OptionButton
			options.select(options.get_item_index(int(value)))
		elif c is ColorPickerButton:
			(c as ColorPickerButton).color = value
		_update_value_label(parts[0], parts[1])
	if _controls.has("display/resolution") and int(_settings.get_setting("display", "resolution")) < 0:
		show_resolution(DisplayServer.window_get_size())
	_syncing = false

# A window whose size is not a preset selects nothing and is written onto the button, rather than
# left blank (reads as broken) or rounded to a neighbour (claims a size it is not).
func show_resolution(size: Vector2i) -> void:
	var options := control("display", "resolution") as OptionButton
	if options == null or _settings == null:
		return
	var index: int = _settings.resolution_index_of(size)
	options.select(options.get_item_index(index))
	if index < 0:
		options.text = "%d x %d" % [size.x, size.y]

func _build_tab(group: String) -> void:
	var scroll := ScrollContainer.new()
	scroll.name = group.capitalize()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_tabs.add_child(scroll)
	var grid := GridContainer.new()
	grid.name = "Rows"
	grid.columns = 2
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_theme_constant_override("h_separation", 18)
	grid.add_theme_constant_override("v_separation", 8)
	scroll.add_child(grid)
	for row in _settings.describe(group):
		var label := Label.new()
		label.text = row["label"]
		label.tooltip_text = row["hint"]
		# A Label ignores the mouse by default, and with it its own tooltip.
		label.mouse_filter = Control.MOUSE_FILTER_PASS
		grid.add_child(label)
		grid.add_child(_make_control(group, row))

func _make_control(group: String, row: Dictionary) -> Control:
	var name: String = row["name"]
	var key := group + "/" + name
	var c: Control
	match String(row["kind"]):
		"bool":
			var check := CheckBox.new()
			check.toggled.connect(_on_write.bind(group, name))
			c = check
		"int", "float":
			var slider := HSlider.new()
			slider.min_value = row["ui_min"]
			slider.max_value = row["ui_max"]
			slider.step = row["step"]
			slider.custom_minimum_size = Vector2(240, 0)
			slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			slider.value_changed.connect(_on_write.bind(group, name))
			slider.name = name
			slider.tooltip_text = row["hint"]
			_controls[key] = slider
			var value_label := Label.new()
			value_label.custom_minimum_size = Vector2(72, 0)
			_value_labels[key] = value_label
			var box := HBoxContainer.new()
			box.add_child(slider)
			box.add_child(value_label)
			return box
		"enum":
			var options := OptionButton.new()
			var labels: PackedStringArray = row["options"]
			for i in range(labels.size()):
				options.add_item(labels[i], i)
			options.item_selected.connect(_on_option.bind(options, group, name))
			c = options
		"color":
			var picker := ColorPickerButton.new()
			picker.edit_alpha = false
			picker.custom_minimum_size = Vector2(72, 24)
			picker.color_changed.connect(_on_write.bind(group, name))
			c = picker
	c.name = name
	c.tooltip_text = row["hint"]
	_controls[key] = c
	return c

func _on_write(value, group: String, name: String) -> void:
	if _syncing or _settings == null:
		return
	_settings.set_setting(group, name, value)
	_update_value_label(group, name)
	# Deliberately no full resync mid-drag: rewriting a slider from the clamped value fights the
	# pointer. The tier is the one write that moves other rows, so it resyncs.
	if group == "render" and name == "quality_tier":
		sync_from_settings()

func _on_option(index: int, options: OptionButton, group: String, name: String) -> void:
	_on_write(options.get_item_id(index), group, name)

func _update_value_label(group: String, name: String) -> void:
	var label: Label = _value_labels.get(group + "/" + name)
	if label == null or _settings == null:
		return
	var value = _settings.get_setting(group, name)
	label.text = str(value) if typeof(value) == TYPE_INT else "%.3f" % float(value)

func _on_reset() -> void:
	if _settings:
		_settings.reset_to_shipped()
	sync_from_settings()

func _on_close() -> void:
	set_open(false)
```

- [ ] **Step 4: Run the panel contract**

Run: `./gdunit_tests.sh -a res://tests/test_settings_menu.gd`
Expected: all cases PASS.

- [ ] **Step 5: Wire the demo and delete the old menu**

`demo/main.tscn`:
1. Delete the line `[ext_resource type="Script" uid="uid://b2lhiur2hi32y" path="res://demo/debug_menu.gd" id="6"]`.
2. Immediately before `[node name="WorldEnvironment"`, insert:

```
[node name="VoxelSettings" type="VoxelSettings" parent="."]
world_path = NodePath("../VoxelWorld")

```

3. Delete the whole `[node name="DebugMenu" type="PanelContainer" parent="HUD" …]` block (its header line through `world_path = NodePath("/root/Main/VoxelWorld")` and the blank line after).
4. In the `[node name="SettingsMenu" parent="HUD" …]` block, replace `world_path = NodePath("/root/Main/VoxelWorld")` with `settings_path = NodePath("/root/Main/VoxelSettings")`.

`demo/help.gd`: replace `["Beauty menu", "F1"],` with `["Settings", "F1"],` and delete `["Graphics settings", "F7"],`.

`tests/test_demo_shell.gd`: in `test_help_lists_every_binding_the_demo_uses`, remove `"F7"` from the key list.

`demo/benchmark.gd`: the render-scale branch becomes

```gdscript
		elif arg.begins_with("--render-scale="):
			# Through VoxelSettings, so the display store the panel reads is never stale.
			get_parent().get_node("VoxelSettings").set_setting("display", "render_scale",
				float(arg.trim_prefix("--render-scale=")))
```

and the upscaler branch becomes

```gdscript
		elif arg.begins_with("--upscaler="):
			# Indices of the display settings' upscaler options (settings/display_settings.cpp).
			var m := arg.trim_prefix("--upscaler=")
			get_parent().get_node("VoxelSettings").set_setting("display", "upscaler", {
				"bilinear": 0,
				"fsr": 1,
				"fsr2": 2,
				"metalfx_spatial": 3,
				"metalfx_temporal": 4,
			}.get(m, 0))
```

Delete the old files:

```bash
git rm demo/debug_menu.gd demo/debug_menu.gd.uid tests/test_debug_menu.gd tests/test_debug_menu.gd.uid
```

Checks, each returning nothing:

```bash
rg 'ConfigFile|KEY_F7|RESOLUTIONS|UPSCALERS|QUALITY_TIERS|GRASS_VALUES' demo
rg 'min_value|max_value' demo/settings_menu.tscn
rg 'scaling_3d_(scale|mode) =' demo
rg 'debug_menu|DebugMenu' demo tests
```

- [ ] **Step 6: Run the demo-facing suites**

```bash
./gdunit_tests.sh -a res://tests/test_settings_menu.gd,res://tests/test_voxel_settings.gd,res://tests/test_demo_shell.gd,res://tests/test_benchmark.gd,res://tests/test_capture.gd
```

Expected: all PASS; `test_demo_shell.gd` and `test_benchmark.gd` case counts match the baseline.

- [ ] **Step 7: Launch the demo once**

Run the demo (`godot --path . res://demo/main.tscn` or the project's usual launcher), press F1, check the four tabs render with sliders, a colour picker and dropdowns, move one slider, close with Esc, relaunch and confirm the value persisted. Note the result in the commit body.

- [ ] **Step 8: Commit**

```bash
git add demo/settings_menu.gd demo/settings_menu.tscn demo/main.tscn demo/help.gd demo/benchmark.gd tests/test_settings_menu.gd tests/test_demo_shell.gd
git commit -m "feat: one F1 settings panel built from VoxelSettings

Display, Render, Beauty and Grass tabs are generated from describe(); the F1
beauty menu (demo/debug_menu.gd) and the F7 binding are gone. Every
test_debug_menu.gd case is ported into test_settings_menu.gd under its old
name; the S5 range check now covers every slider of every group. benchmark.gd
routes --render-scale and --upscaler through VoxelSettings."
```

---

### Task 17: Inspector properties on `VoxelSettings`

**Files:**
- Modify: `extension/src/voxel_settings.h`, `extension/src/voxel_settings.cpp`
- Test: `tests/test_voxel_settings.gd`

**Interfaces:**
- Consumes: Task 15 `VoxelSettings`; Task 7/8/14 store classes.
- Produces: dynamic properties `display/<name>`, `render/<name>`, `beauty/<name>`, `grass/<name>`; revert value = the row's default; property values set before `_ready` apply to the world.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_voxel_settings.gd`:

```gdscript
func test_inspector_properties_mirror_every_row() -> void:
	var parts := make_settings()
	var world: VoxelWorld = parts[0]
	var settings: VoxelSettings = parts[2]
	var names := {}
	for p in settings.get_property_list():
		names[p["name"]] = p
	for group in settings.groups():
		for row in settings.describe(group):
			var path := "%s/%s" % [group, row["name"]]
			assert_bool(names.has(path)).override_failure_message("no property %s" % path).is_true()
	settings.set("beauty/ssgi_strength", 2.5)
	assert_float(world.get_effect_value("ssgi_strength")).is_equal_approx(2.5, 0.001)
	assert_float(float(settings.get("beauty/ssgi_strength"))).is_equal_approx(2.5, 0.001)
	assert_bool(settings.property_can_revert("beauty/ssgi_strength")).is_true()
	assert_float(float(settings.property_get_revert("beauty/ssgi_strength"))).is_equal_approx(1.0, 0.001)

func test_property_values_set_before_ready_apply_to_the_world() -> void:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	root.add_child(world)
	var vp := SubViewport.new()
	vp.name = "Viewport"
	vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	root.add_child(vp)
	var settings: VoxelSettings = ClassDB.instantiate("VoxelSettings")
	settings.world_path = NodePath("../World")
	settings.viewport_path = NodePath("../Viewport")
	settings.config_path = CONFIG_PATH
	settings.manage_window = false
	# What loading a scene does: properties first, _ready later.
	settings.set("beauty/ssgi_strength", 3.0)
	settings.set("render/near_field_scale", 0.9)
	settings.set("display/render_scale", 0.75)
	root.add_child(settings)
	assert_float(world.get_effect_value("ssgi_strength")).is_equal_approx(3.0, 0.001)
	assert_float(world.near_field_scale).is_equal_approx(0.9, 0.001)
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.75, 0.001)
	# ...and they are what the scene shipped.
	assert_float(float(settings.get_overrides("beauty")["ssgi_strength"])).is_equal_approx(3.0, 0.001)

func test_a_packed_scene_stores_only_real_overrides() -> void:
	var holder := Node.new()
	var settings: VoxelSettings = ClassDB.instantiate("VoxelSettings")
	settings.name = "Settings"
	holder.add_child(settings)
	settings.owner = holder
	settings.set("beauty/ssgi_strength", 2.0)
	var packed := PackedScene.new()
	assert_int(packed.pack(holder)).is_equal(OK)
	var state := packed.get_state()
	var stored: Array = []
	for i in range(state.get_node_property_count(1)):
		stored.append(String(state.get_node_property_name(1, i)))
	holder.free()
	assert_array(stored).contains(["beauty/ssgi_strength"])
	assert_array(stored).not_contains(["beauty/ssgi_taps", "render/quality_tier", "grass/reach_m",
		"display/render_scale"])
```

Run: `./gdunit_tests.sh -a res://tests/test_voxel_settings.gd`
Expected: the three new cases FAIL (`no property display/render_scale`; world value unchanged; property not stored).

- [ ] **Step 2: Add stand-in stores and the property callbacks**

In `extension/src/voxel_settings.h`, add includes:

```cpp
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include "grass/grass_settings_store.h"
#include "settings/render_settings.h"
#include "shade/beauty_settings_store.h"
```

in the public section, after `_ready`:

```cpp
	// Inspector: one "<group>/<name>" property per row. A scene stores only values that differ
	// from the row's default, because the revert value is that default.
	bool _set(const StringName &property, const Variant &value);
	bool _get(const StringName &property, Variant &r_value) const;
	void _get_property_list(List<PropertyInfo> *list) const;
	bool _property_can_revert(const StringName &property) const;
	bool _property_get_revert(const StringName &property, Variant &r_value) const;
```

and in the private section, after `display_`:

```cpp
	// Until _ready resolves the world -- and always in the editor -- the world's groups are these
	// stand-ins, so a scene's property values have somewhere to land; _ready copies their overrides
	// into the world's stores.
	mutable ve::RenderSettingsStore render_stand_in_;
	mutable ve::BeautySettingsStore beauty_stand_in_;
	mutable ve::GrassSettingsStore grass_stand_in_;
```

In `extension/src/voxel_settings.cpp`:

Replace `VoxelSettings::group` with:

```cpp
ve::SettingsGroup *VoxelSettings::group(const String &name) const {
	if (name == "display") return &display_;
	if (VoxelWorld *world = Object::cast_to<VoxelWorld>(ObjectDB::get_instance(world_id_))) {
		const CharString n = name.utf8();
		return world->context().render->settings_group(n.get_data());
	}
	// ponytail: the beauty stand-in is not rebased when the render stand-in's tier changes, so the
	// editor reverts beauty knobs to High's values. Wire a listener if a scene ever ships a tier.
	if (name == "render") return &render_stand_in_;
	if (name == "beauty") return &beauty_stand_in_;
	if (name == "grass") return &grass_stand_in_;
	return nullptr;
}
```

In `_ready`, after the viewport lookup, insert before `capture_display_base();`:

```cpp
	// A scene's property values went into the stand-ins before the world was known.
	if (world_id_ != 0) {
		const std::pair<const char *, ve::SettingsGroup *> stand_ins[] = {
			{"render", &render_stand_in_}, {"beauty", &beauty_stand_in_}, {"grass", &grass_stand_in_}};
		for (const auto &[name, stand_in] : stand_ins)
			if (ve::SettingsGroup *live = group(name))
				for (const auto &[knob, value] : stand_in->overrides()) live->set(knob, value);
	}
```

(add `#include <utility>`).

Add, at the end of the file:

```cpp
namespace {

bool split_property(const StringName &property, String *group, String *name) {
	const String p = property;
	const int slash = p.find("/");
	if (slash <= 0) return false;
	*group = p.substr(0, slash);
	*name = p.substr(slash + 1);
	return true;
}

} // namespace

bool VoxelSettings::_set(const StringName &property, const Variant &value) {
	String g, n;
	return split_property(property, &g, &n) && set_setting(g, n, value);
}

bool VoxelSettings::_get(const StringName &property, Variant &r_value) const {
	String g, n;
	if (!split_property(property, &g, &n)) return false;
	ve::SettingsGroup *grp = group(g);
	const CharString name = n.utf8();
	ve::SettingValue v;
	if (!grp || !grp->get(name.get_data(), &v)) return false;
	r_value = setting_to_variant(v);
	return true;
}

void VoxelSettings::_get_property_list(List<PropertyInfo> *list) const {
	for (const char *g : kGroups) {
		ve::SettingsGroup *grp = group(g);
		if (!grp) continue;
		for (const ve::RowInfo &r : grp->rows()) {
			const String path = String(g) + "/" + r.name;
			const String range = String::num(r.ui_min) + "," + String::num(r.ui_max) + "," +
					String::num(r.step);
			switch (r.kind) {
				case ve::SettingKind::kBool:
					list->push_back(PropertyInfo(Variant::BOOL, path));
					break;
				case ve::SettingKind::kInt:
					list->push_back(PropertyInfo(Variant::INT, path, PROPERTY_HINT_RANGE, range));
					break;
				case ve::SettingKind::kFloat:
					list->push_back(PropertyInfo(Variant::FLOAT, path, PROPERTY_HINT_RANGE, range));
					break;
				case ve::SettingKind::kColor:
					list->push_back(PropertyInfo(Variant::COLOR, path, PROPERTY_HINT_COLOR_NO_ALPHA));
					break;
				case ve::SettingKind::kEnum: {
					String items = r.min < 0.0f ? String("Not a preset:-1") : String();
					for (size_t i = 0; i < r.options.size(); i++) {
						if (!items.is_empty()) items += ",";
						items += String(r.options[i]) + ":" + String::num_int64(static_cast<int64_t>(i));
					}
					list->push_back(PropertyInfo(Variant::INT, path, PROPERTY_HINT_ENUM, items));
					break;
				}
			}
		}
	}
}

bool VoxelSettings::_property_can_revert(const StringName &property) const {
	String g, n;
	if (!split_property(property, &g, &n)) return false;
	ve::SettingsGroup *grp = group(g);
	const CharString name = n.utf8();
	ve::SettingValue v;
	return grp && grp->get_default(name.get_data(), &v);
}

bool VoxelSettings::_property_get_revert(const StringName &property, Variant &r_value) const {
	String g, n;
	if (!split_property(property, &g, &n)) return false;
	ve::SettingsGroup *grp = group(g);
	const CharString name = n.utf8();
	ve::SettingValue v;
	if (!grp || !grp->get_default(name.get_data(), &v)) return false;
	r_value = setting_to_variant(v);
	return true;
}
```


- [ ] **Step 3: Build and run**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_voxel_settings.gd,res://tests/test_settings_menu.gd,res://tests/test_demo_shell.gd
```

Expected: all PASS. If `test_a_packed_scene_stores_only_real_overrides` stores every property, `PackedScene.pack` is not consulting `_property_get_revert` on this Godot build; stop and report rather than weakening the test.

- [ ] **Step 4: Check the shipped scene stores no settings property**

Open and re-save `demo/main.tscn` in the editor (or run `godot --headless --path . --editor --quit` if that is how the project resaves scenes), then:

```bash
git diff demo/main.tscn
rg '^(display|render|beauty|grass)/' demo/main.tscn
```

Expected: no `display/…`, `render/…`, `beauty/…` or `grass/…` lines. If the editor added any, revert them.

- [ ] **Step 5: Commit**

```bash
git add extension/src/voxel_settings.h extension/src/voxel_settings.cpp tests/test_voxel_settings.gd
git commit -m "feat: every settings row is an inspector property on VoxelSettings

Properties set before the world is ready land in stand-in stores and are
applied in _ready; a packed scene stores only values that differ from the
row's default."
```

---

### Task 18: Exit evidence, results report and roadmap status

**Files:**
- Create: `docs/superpowers/plans/2026-09-16-settings-store-results.md`
- Modify: `docs/superpowers/specs/2026-09-16-settings-store-design.md` (status line; append §9 "Amendments from planning")
- Modify: `docs/superpowers/specs/2026-09-13-frame-module-design.md` §10 rows S1, S2, S5, S6
- Modify: `docs/superpowers/plans/2026-09-13-frame-module.md` ("Sub-project 3" status paragraph; suspected-bugs routing table)

**Interfaces:**
- Consumes: every earlier task.
- Produces: the results report.

- [ ] **Step 1: Full regression**

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh
```

Extract counts and failures with Task 1 Step 3's script. Compare with the baseline by case name and message. Expected differences, and only these: new suites `test_settings_names`, `test_world_field_overrides`, `test_voxel_settings`; `test_debug_menu` gone (its cases ported into `test_settings_menu`, whose count rises from 15); `test_beauty_settings` +2. Any other new failure is a regression: stop and report.

- [ ] **Step 2: Exit checks**

Run each; paste the output (empty is the pass) into the report:

```bash
rg 'if \(name == "' extension/src/render/orchestrator.cpp
rg 'kSsaoRadius|kSsaoStrength|kAmbient|beauty_value_field|beauty_field' extension/src
rg 'ConfigFile|KEY_F7|RESOLUTIONS|UPSCALERS|QUALITY_TIERS|GRASS_VALUES' demo
rg 'min_value|max_value' demo/settings_menu.tscn
rg 'settings_group|render_settings_|beauty_\.' extension/src/render/frame.cpp
ls demo/debug_menu.gd
```

(`ls` must fail.) Also confirm `test_frame_shipped_golden.gd`, `test_ssao_golden.gd`, `test_contact_shadow_golden.gd`, `test_deferred_golden.gd` passed unchanged in Step 1.

- [ ] **Step 3: Re-trace the Appendix A scenarios**

Trace, by reading the code as it now stands, what adding each costs:
- **Beauty float knob**: `BeautySettings` field + row (`shade/beauty_settings.h`, `.cpp`), the pass's push line, the shader. Panel, inspector, config file and `debug_beauty_settings` follow from the row. Expected: 4 files (was 9, 11 with inspector).
- **Beauty int knob**: settable by name (was not).
- **Display or render dial**: struct field + row (`settings/*_settings.{h,cpp}`) + its apply line (`VoxelSettings::apply_display` or `RenderOrchestrator::on_render_resolved`) + its reader. Expected: 3–4 files.

- [ ] **Step 4: Write the results report**

Create `docs/superpowers/plans/2026-09-16-settings-store-results.md` with this shape, filling every angle-bracket field from Steps 1–3:

```markdown
# Settings store — results

Commit: `<git rev-parse --short HEAD>`. Recorded <date> on <machine / GPU / Godot version>.
Baseline: `docs/superpowers/plans/2026-09-16-settings-store-baseline.md`. Report: `reports/report_<N>`.

## Regression
Native: <doctest summary line>
gdUnit: <total tests / failures>; differences from baseline:
<one line per suite whose count or failures changed, with the reason>

## Suspected bugs
| Id | Result | Test | Fix commit |
|---|---|---|---|
| S1 | FIXED | `test_a_consolidated_fill_in_open_sky_still_gets_a_collider`, `test_a_pasted_volume_in_open_sky_gets_a_collider` | <sha> |
| S2 | FIXED | `test_the_contact_probe_reads_a_consolidated_carve`, `test_the_cpu_island_extract_reads_a_consolidated_carve` | <sha> |
| S5 | FIXED | `test_every_slider_range_contains_the_current_value_and_sits_inside_the_clamp`, native row invariants | <sha> |
| S6 | FIXED | `test_a_tweak_survives_a_tier_change`, `test_effects_turned_off_before_a_tier_stay_off` | <sha> |

## Goldens
<each golden suite and "unchanged">

## Exit checks
<each command from Step 2 and its output>

## Change cost (Appendix A re-trace)
| Scenario | Before | After | Files |
|---|---|---|---|
| Beauty float knob | 9 (11 with inspector) | <n> | <list> |
| Beauty int knob | not settable by name | settable by name | — |
| Display / render dial | 3 hand-written homes (field, menu table, cfg key) | <n> | <list> |

## Open
<anything that did not meet an exit criterion, or "none">
```

- [ ] **Step 5: Update the specs and the roadmap**

- `docs/superpowers/specs/2026-09-16-settings-store-design.md`: status line → `Implemented; see docs/superpowers/plans/2026-09-16-settings-store-results.md`; append a `## 9. Amendments from planning` section copying the ten items of this plan's "Decided during planning" list.
- `docs/superpowers/specs/2026-09-13-frame-module-design.md` §10: append to S1, S2, S5 and S6 rows `— **Sub-project 3 result:** FIXED (<fix sha>, passing <test name>).`
- `docs/superpowers/plans/2026-09-13-frame-module.md`: under "Sub-project 3 — One settings store" add a `**Status.** Implemented; see docs/superpowers/plans/2026-09-16-settings-store-results.md.` paragraph (naming S1/S2 as pulled forward from sub-project 5); in "Suspected bugs — routing summary", mark S1, S2 as fixed in sub-project 3 (S3 stays with sub-project 5) and S5, S6 as fixed.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/plans/2026-09-16-settings-store-results.md docs/superpowers/specs/2026-09-16-settings-store-design.md docs/superpowers/specs/2026-09-13-frame-module-design.md docs/superpowers/plans/2026-09-13-frame-module.md
git commit -m "docs: record settings store exit criteria and results"
```
