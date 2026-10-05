# Raster Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Render-tab switch, "Raymarching", that turns world ray marching off and draws every surface (terrain near and far, and islands) as rasterized surface-nets meshes, with 0.1 m cells near the camera.

**Architecture:** The LoD tree gains two levels below level 0 (`-1` = 0.2 m, `-2` = 0.1 m) that only raster mode lets the walk reach (`LodTreeConfig::min_level`). Raster mode reuses the existing "near field off" gate for the fade band, HiZ and sun-map ownership. It then skips the marcher, fills the G-buffer with sky through a variant of the composite pass, turns off the scatter passes' sun march, and draws islands from opaque meshes contoured beside their transparency shells.

**Tech Stack:** C++20 GDExtension (godot-cpp, Godot 4.7 RenderingDevice), GLSL 460, doctest (native), gdUnit4 (GPU).

**Spec:** `docs/superpowers/specs/2026-10-04-raster-mode-design.md`

## Global Constraints

- No `Co-Authored-By` or other AI attribution lines in any commit message. Plain conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`).
- Work on branch `feat/raster-mode` (the spec is already committed there).
- Raymarched mode (the default) must render exactly as on `main`. The existing goldens (`tests/test_*_golden.gd`) pin it. `tests/test_frame_shipped_golden.gd` is known flaky and informational only; never re-record it.
- Existing LoD level numbers keep their meaning: level 0 is 0.4 m, level 7 is 51.2 m. The new levels are negative.
- Tabs for indentation in C++, GLSL and GDScript.
- Never put a literal `#include` directive inside a comment in a `.glslh` file (the shader loader matches include tokens anywhere in a line).
- Build: `./build.sh`. Native tests: `cd extension && scons -Q test` (single case: `cd extension && ./build/tests/ve_tests -tc="<name>"`). GPU tests: `./gdunit_tests.sh -a res://tests/<file>.gd`.
- gdUnit has a standing, drifting set of failures on clean `main`. Task 0 records it; every later "no new failures" check compares against that list, not against zero.
- GPU pass timings are invalid on this machine. Cost is measured as interleaved A/B/A frame time only.
- Lighting and grass checks stream at `(20, 60, 30)`; `(30, 56.2, 30)` sees buried cave grass.

## Deviations From The Spec (decided while planning; Task 7 records them in the spec)

1. **Negative levels instead of renumbering.** The spec (§3) renumbered the LoD so that level 0 became 0.1 m, and restated about 20 sites against `kLodFarBaseLevel = 2`. Planning found raw level numbers in many more places: native tests, gdUnit tests (`debug_lod_diff(0, …)`, `debug_lod_submit([[0, …]])`, `got[0]["level"] == 0`) and hooks. Levels `-1` and `-2` give the same tree with none of that churn:
   - `kLodBaseCell`, `kLodLevels`, `kLodResidentLevelFrom`, `sun_cascades` and `lod_reduce` are unchanged.
   - `LodTreeConfig::min_level` is `0` in raymarched mode and `kLodMinLevel = -2` in raster mode.
   - The near-dense rule forces descent only above level 0.
2. **The "near field off" fade band was broken, and raster mode inherits it.**
   - `LodSystem::fade_band` reported `0 / 1e9` there, intending "the far field owns everything". But `lod.frag.glsl` keeps a fragment where `bayer4(px) < (d − start) / (end − start)`, which is about `d / 1e9`, so only the `bayer4 == 0` pixel of each 4×4 tile survived: 1/16 coverage. `deferred.comp.glsl`'s `far_field_owns` has the same shape.
   - Fix: report `0 / 0`, so `t = 1` everywhere.
   - `fade_end` also capped the near grass reach, so grass would vanish. With the near field off, grass is capped by residency alone.
3. **`kShellLevel` becomes `kLodMinLevel`.** It was `-1`, a sentinel that `clamp_level` turned into level 0. `MeshService` (`mesh_service.cpp`, `lod_chunk_origin(job.level, …)`) uses it to find a shell job's override table, so it looked in the region of a 12.8 m chunk instead of the 3.2 m shell chunk. A shell chunk is exactly a level `-2` chunk, so the sentinel becomes that level, and the lookup becomes correct.
4. **Scatter sun flags use spare lanes:** `GrassParams::limits[2]` and `LeafParams::limits[3]`. No GPU layout change and no regenerated `blocks.glslh`.
5. **The island fixture contours through the shipping function.** `debug_place_test_island` (`hooks_physics.cpp`) duplicated the shell contour call. It now calls `RenderOrchestrator::contour_island_meshes`, the same function `drain_island_uploads` calls, so the gdUnit island tests exercise the shipping path.
6. **LoD-level assertions tick at 2560×1440.** A 64×64 `debug_render_frame` selects the coarse cut its own viewport asks for, so tests that check which levels are drawn settle through `debug_lod_tick`. Pixel assertions still go through `debug_render_frame`.
7. **The acne capture uses the benchmark camera** (`--screenshot=`) instead of the far grove. Only that camera has a screenshot flag, and acne is a terrain artifact, not a tree one.
8. **Marcher targets are not freed on a live toggle.** Raster mode never dispatches the marcher; targets allocated earlier in the session stay until the next resize. The frame-contract test checks that the stage did not run.

## Review Focus

1. **Live toggle raster → raymarched.** Fine-level pages must age out and the pool must not leak. Test: Task 4, `test_switching_back_to_raymarching_frees_the_fine_pages`.
2. **An island extracted while raymarching, then a switch to raster.** It must still draw, so its mesh has to exist already. Test: Task 6, `test_an_island_extracted_while_raymarching_renders_after_the_switch`.
3. **An edit in raster mode.** A crater must dirty and rebuild the 0.1 m chunks, not only levels ≥ 0. Tests: Task 3 native `an edit dirties the raster levels too`; Task 4 gdUnit `test_an_edit_in_raster_mode_requests_the_fine_chunks`.
4. **Looking at the sky in raster mode.** Pixels no mesh covers must be sky, not black. Test: Task 5, `test_looking_up_in_raster_mode_shows_the_sky`.
5. **Ice in raster mode.** The near shell grid is off, so fine LoD chunks must carry the shell. Test: Task 5, `test_ice_renders_in_raster_mode`.

---

## File Structure

**Create:**
- `shaders/island_xform.glslh` — `island_place(island, p, n)`: places an island page's local-space vertex through the island descriptor. Included by `shell.vert.glsl` and `lod.vert.glsl`.
- `tests/test_raster_mode.gd` — the GPU suite for this feature.

**Modify:**
- `extension/src/settings/render_settings.h`, `.cpp`; `extension/tests/test_render_settings.cpp`; `tests/test_settings_menu.gd` — the switch.
- `extension/src/render/orchestrator.h`, `.cpp`; `extension/src/render/frame.h`, `frame.cpp` — the gate, `raster_mode()`, the raster frame, island contouring.
- `extension/src/lod/lod_grid.h`, `lod_grid.cpp`; `extension/src/lod/lod_tree.h`, `lod_tree.cpp`; `extension/tests/test_lod_grid.cpp`, `test_lod_tree.cpp` — negative levels and `min_level`.
- `extension/src/transparency/shell_grid.h`, `shell_grid.cpp`; `extension/tests/test_shell_grid.cpp` — `kShellLevel`, `island_blocks`, `island_mesh_flags`.
- `extension/src/lod/lod_system.h`, `lod_system.cpp`; `extension/src/debug/hooks_lod.cpp` — `min_level` per tick, the fade-band fix, island mesh pages, stats.
- `extension/src/debug/hooks.cpp` — `gb_ground_fraction` readout.
- `extension/src/debug/hooks_physics.cpp` — the island fixture calls the shipping contour.
- `extension/src/render/composite_pass.h`, `.cpp`; `shaders/composite.frag.glsl` — the sky-only variant.
- `extension/src/render/lod_raster_pass.h`, `.cpp`; `shaders/lod.vert.glsl`; `shaders/shell.vert.glsl` — island pages in the LoD draw.
- `shaders/grass_scatter.comp.glsl`, `shaders/leaf_scatter.comp.glsl` — no sun march in raster mode.
- `docs/superpowers/specs/2026-10-04-raster-mode-design.md` — deviations and measurements (Task 7).

---

### Task 0: Baseline

**Files:** none changed.

- [ ] **Step 1: Confirm the branch**

Run: `git status -sb | head -1`
Expected: `## feat/raster-mode` (in the main checkout or in a worktree created for it).

- [ ] **Step 2: Native tests green**

Run: `./build.sh --test`
Expected: build OK, doctest summary `Status: SUCCESS!`.

- [ ] **Step 3: Record the gdUnit baseline**

Run: `./gdunit_tests.sh 2>&1 | tee "$TMPDIR/raster-baseline.txt" | tail -40`
Expected: a summary listing the failing suites. Save the list of failing suite names in the task notes. Every later "no new failures" check compares against this list.

---

### Task 1: The Raymarching switch and the shared gate

**Files:**
- Modify: `extension/src/settings/render_settings.h`, `extension/src/settings/render_settings.cpp`
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`
- Modify: `extension/src/render/frame.h`
- Test: `extension/tests/test_render_settings.cpp`, `tests/test_settings_menu.gd`

**Interfaces:**
- Produces: `ve::RenderSettings::raymarch` (bool, default `true`), settings row `"raymarch"` labelled `"Raymarching"`; `RenderOrchestrator::raster_mode() const` (true when raymarch is off); `RenderOrchestrator::near_field_enabled()` now returns `near_field && raymarch`; `FrameSettings::raster_mode`. GDScript: `world.set_effect_enabled("raymarch", bool)` / `get_effect_enabled("raymarch")`.

- [ ] **Step 1: Write the failing native test**

In `extension/tests/test_render_settings.cpp`, add to the defaults case, right after `CHECK(s.islands);`:

```cpp
	CHECK(s.raymarch);
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons -Q test`
Expected: compile error, `no member named 'raymarch' in 've::RenderSettings'`.

- [ ] **Step 3: Add the field and row**

`extension/src/settings/render_settings.h`, after `bool islands = true;`:

```cpp
	// Off = raster mode (spec 2026-10-04): nothing is marched, every surface is a mesh.
	bool raymarch = true;
```

`extension/src/settings/render_settings.cpp`, after the `islands` row:

```cpp
	bool_row("raymarch", "Raymarching", &RenderSettings::raymarch),
```

- [ ] **Step 4: Run the native tests**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`. The row-invariant case (`check_rows`) covers the new row.

- [ ] **Step 5: Write the failing gdUnit test**

In `tests/test_settings_menu.gd`, after `test_islands_toggle_is_a_real_render_effect`:

```gdscript
func test_raymarching_toggle_is_a_real_render_effect() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_bool(world.get_effect_enabled("raymarch")).is_true()
	parts[3].control("render", "raymarch").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("raymarch")).is_false()
	# Its own switch: turning raymarching off does not rewrite the near-field setting.
	assert_bool(world.get_effect_enabled("near_field")).is_true()
```

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_settings_menu.gd`
Expected: PASS. The panel builds its controls from `render_rows()`, so this pins that the row is real and reaches the store. It passes as soon as Step 3 is built.

- [ ] **Step 6: Mirror the switch into the orchestrator**

`extension/src/render/orchestrator.h`: next to `near_field_enabled_`:

```cpp
	std::atomic<bool> raymarch_enabled_{true};
```

Replace the `near_field_enabled()` accessor with:

```cpp
	// Raster mode turns the near field off with everything that already keys on it: the
	// fade band, HiZ, the near shell grid, the deferred sun-map ownership.
	bool near_field_enabled() const {
		return near_field_enabled_.load(std::memory_order_relaxed) &&
				raymarch_enabled_.load(std::memory_order_relaxed);
	}
	// Spec 2026-10-04: the Raymarching switch is off. Gates only what raster mode adds.
	bool raster_mode() const { return !raymarch_enabled_.load(std::memory_order_relaxed); }
```

`extension/src/render/orchestrator.cpp`, in `on_render_resolved`, after the `near_field_enabled_` store:

```cpp
	self->raymarch_enabled_.store(s.raymarch, std::memory_order_relaxed);
```

In `set_effect_enabled`, update the comment to `// Render switches (islands, near_field, raymarch), then beauty switches; fail-soft for anything else.`

`extension/src/render/frame.h`, `FrameSettings`, after `near_field_enabled`:

```cpp
	bool raster_mode = false;
```

`orchestrator.cpp`, `frame_settings()`, after `s.near_field_enabled = …`:

```cpp
	s.raster_mode = raster_mode();
```

- [ ] **Step 7: Build and run both suites**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_settings_menu.gd -a res://tests/test_frame_contract.gd`
Expected: native `SUCCESS`, both gdUnit suites PASS.

- [ ] **Step 8: Commit**

```bash
git add extension/src/settings/render_settings.h extension/src/settings/render_settings.cpp \
	extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp \
	extension/src/render/frame.h extension/tests/test_render_settings.cpp tests/test_settings_menu.gd
git commit -m "feat: a Raymarching render switch that also turns the near field off"
```

---

### Task 2: The near-field-off fade band covers the screen

**Files:**
- Create: `tests/test_raster_mode.gd`
- Modify: `extension/src/debug/hooks.cpp` (the `debug_render_frame` readouts)
- Modify: `extension/src/lod/lod_system.cpp` (`fade_band`, the comment in `refresh_shell_candidates`)
- Modify: `extension/src/render/frame.cpp` (`grass_layout`)

**Interfaces:**
- Produces: `debug_render_frame(...)["gb_ground_fraction"]`, the fraction of pixels whose G-buffer material is non-zero (float in [0, 1]); `LodSystem::fade_band` reports `0 / 0` with the near field off. Later tasks reuse the helpers in `tests/test_raster_mode.gd`: `make_world()`, `frame(w, cam, fwd)`, `lod_quiet(w)`, `settle_frames(w, cam, fwd)`, `settle_ticks(w, cam, fwd)`.

- [ ] **Step 1: Add the coverage readout**

In `extension/src/debug/hooks.cpp`, `debug_render_frame`, next to the other early defaults (after `d["center_gb_material"] = 0;`):

```cpp
	// Fraction of pixels the G-buffer holds a surface for (material != 0). Raster mode's
	// coverage check: sky and dithered-away fragments both read 0.
	d["gb_ground_fraction"] = 0.0;
```

Replace the block that reads the G-buffer surface:

```cpp
		const PackedByteArray gs = device->texture_get_data(
				world_->context().render->passes().gbuffer->surface(), 0);
		if (gs.size() >= (c + 1) * 8)
			d["center_gb_material"] = static_cast<int>(half_to_float(
					reinterpret_cast<const uint16_t *>(gs.ptr())[c * 4 + 2]) + 0.5f);
```

with:

```cpp
		const PackedByteArray gs = device->texture_get_data(
				world_->context().render->passes().gbuffer->surface(), 0);
		if (gs.size() >= (c + 1) * 8)
			d["center_gb_material"] = static_cast<int>(half_to_float(
					reinterpret_cast<const uint16_t *>(gs.ptr())[c * 4 + 2]) + 0.5f);
		if (gs.size() >= pixels * 8) {
			const uint16_t *g = reinterpret_cast<const uint16_t *>(gs.ptr());
			int64_t ground = 0;
			for (int64_t i = 0; i < pixels; i++)
				if (half_to_float(g[i * 4 + 2]) > 0.5f) ground++;
			d["gb_ground_fraction"] = static_cast<double>(ground) / static_cast<double>(pixels);
		}
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_raster_mode.gd`:

```gdscript
extends GdUnitTestSuite

# Raster mode (docs/superpowers/specs/2026-10-04-raster-mode-design.md): with the Raymarching
# switch off every surface is a mesh. Frames go through debug_render_frame, the shipping
# VoxelFrame. LoD LEVELS are settled through debug_lod_tick at 2560x1440: a 64x64 probe frame
# selects the coarse cut its own viewport asks for.

const CAM := Vector3(30.0, 70.0, 30.0)
const FWD := Vector3(0.2, -1.0, 0.2)

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
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(CAM) == 0 else 0
		if quiet >= 6:
			break
	w.hooks().debug_lod_tick(CAM, FWD.normalized()) # creates the LoD pool the frame gates on
	return w

func frame(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d: Dictionary = w.hooks().debug_render_frame(cam, fwd.normalized(), 64, 64)
	assert_bool(d["ok"]).override_failure_message("frame aborted: %s" % d).is_true()
	return d

func lod_quiet(w: VoxelWorld) -> bool:
	var s: Dictionary = w.hooks().debug_lod_stats()
	return int(s["requests_pending"]) == 0 and int(s["builds_in_flight"]) == 0

# Frames until the walk this 64x64 frame asks for has nothing left to build.
func settle_frames(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d := {}
	var quiet := 0
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		quiet = quiet + 1 if lod_quiet(w) else 0
		if quiet >= 4:
			break
	return d

# Ticks at 2560x1440 until the walk has nothing left to build.
func settle_ticks(w: VoxelWorld, cam := CAM, fwd := FWD) -> void:
	var quiet := 0
	for i in range(2000):
		w.hooks().debug_stream_frame(cam)
		w.hooks().debug_lod_tick(cam, fwd.normalized())
		quiet = quiet + 1 if lod_quiet(w) else 0
		if quiet >= 4:
			break

# With the near field off the far field owns every pixel. It used to own one in sixteen:
# fade_band() reported 0 / 1e9, lod.frag.glsl keeps a fragment where bayer4 < d / 1e9, and
# only the bayer4 == 0 pixel of each 4x4 tile clears that.
func test_with_the_near_field_off_the_far_field_covers_the_ground() -> void:
	var w := make_world()
	w.set_effect_enabled("near_field", false)
	var d := settle_frames(w)
	assert_float(float(d["gb_ground_fraction"])).override_failure_message(
		"the far field covered %s of a view of nothing but ground" % d["gb_ground_fraction"]
		).is_greater(0.95)
```

- [ ] **Step 3: Run it to verify it fails**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_raster_mode.gd`
Expected: FAIL, coverage near `0.06` (1/16).

- [ ] **Step 4: Fix the fade band**

`extension/src/lod/lod_system.cpp`, `LodSystem::fade_band`, replace the near-field-off branch:

```cpp
	// With the near field off the far field owns every distance: start and end both at 0
	// put every fragment at t = 1, past every bayer4 threshold, in lod.frag.glsl, the
	// composite, the shell passes and deferred's far_field_owns alike. (0 / 1e9 put t at
	// d / 1e9 instead, which kept one pixel in sixteen.) The LoD build gate reads the same 0,
	// so it requests the near chunks.
	if (!render()->near_field_enabled()) {
		if (fade_start) *fade_start = 0.0f;
		if (fade_end) *fade_end = 0.0f;
		return;
	}
```

In `refresh_shell_candidates`, the comment that begins `// With the near field off there is no near shell to draw at all` says `fade_band()'s near-field-off branch reports fade_end = 1e9 m`. Change that sentence to `fade_band()'s near-field-off branch reports fade_end = 0`, and change `Keeping that as the radius is a landmine:` to `A radius derived from it is meaningless, and the 1e9 it used to be was a landmine:`. The code there is unchanged.

`extension/src/render/frame.cpp`, `VoxelFrame::grass_layout`, replace:

```cpp
	gs.reach_m = std::min(gs.reach_m, std::min(fade_end, grass_reach_limit_m()));
```

with:

```cpp
	// With the near field off there is no seam (fade_end is 0): residency alone bounds the
	// near blades, which stand on the mesh the far field now draws up to the camera.
	const float seam = render_.near_field_enabled() ? fade_end : grass_reach_limit_m();
	gs.reach_m = std::min(gs.reach_m, std::min(seam, grass_reach_limit_m()));
```

- [ ] **Step 5: Run it to verify it passes**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_raster_mode.gd -a res://tests/test_frame_contract.gd -a res://tests/test_transparency.gd -a res://tests/test_lod_stream.gd -a res://tests/test_edit_fanout.gd`
Expected: all PASS, or failing only where Task 0's baseline already failed. These are the suites that turn the near field off.

- [ ] **Step 6: Commit**

```bash
git add extension/src/debug/hooks.cpp extension/src/lod/lod_system.cpp extension/src/render/frame.cpp \
	tests/test_raster_mode.gd
git commit -m "fix: with the near field off the far field owns every pixel, not one in sixteen"
```

---

### Task 3: Two LoD levels below level 0

**Files:**
- Modify: `extension/src/lod/lod_grid.h`, `extension/src/lod/lod_grid.cpp`
- Modify: `extension/src/lod/lod_tree.h`, `extension/src/lod/lod_tree.cpp`
- Modify: `extension/src/transparency/shell_grid.h`
- Test: `extension/tests/test_lod_tree.cpp`, `extension/tests/test_lod_grid.cpp`, `extension/tests/test_shell_grid.cpp`

**Interfaces:**
- Produces: `ve::kLodMinLevel = -2`; `lod_cell_size(-1) == 0.2f`, `lod_cell_size(-2) == 0.1f`, and every level helper accepts `[kLodMinLevel, kLodLevels - 1]`; `ve::LodTreeConfig::min_level` (int, default 0); `ve::LodTree::set_min_level(int)`; `ve::kShellLevel == ve::kLodMinLevel`.

- [ ] **Step 1: Write the characterization test**

At the end of `extension/tests/test_lod_tree.cpp`:

```cpp
namespace {

// FNV-1a over the cut's (level, x, y, z), sorted first so the hash does not depend on order.
uint64_t cut_hash(std::vector<ve::LodDrawItem> draws) {
	std::sort(draws.begin(), draws.end(), [](const ve::LodDrawItem &a, const ve::LodDrawItem &b) {
		if (a.level != b.level) return a.level < b.level;
		if (a.coord.z != b.coord.z) return a.coord.z < b.coord.z;
		if (a.coord.y != b.coord.y) return a.coord.y < b.coord.y;
		return a.coord.x < b.coord.x;
	});
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&h](int v) {
		for (int i = 0; i < 4; i++) {
			h ^= uint64_t((uint32_t(v) >> (8 * i)) & 0xFFu);
			h *= 1099511628211ull;
		}
	};
	for (const ve::LodDrawItem &d : draws) {
		mix(d.level);
		mix(d.coord.x);
		mix(d.coord.y);
		mix(d.coord.z);
	}
	return h;
}

ve::LodCamera cam_looking(const float p[3], const float f[3]) {
	// Any up not parallel to the view; the straight-down shot uses -Z.
	const bool vertical = std::fabs(f[1]) > 0.99f;
	const float up[3] = {0.0f, vertical ? 0.0f : 1.0f, vertical ? -1.0f : 0.0f};
	return ve::lod_camera_perspective(p, f, up, 1.2217f, 16.0f / 9.0f, 0.1f, 8000.0f, 2560, 1440);
}

} // namespace

// Raster mode adds levels BELOW 0 and a min_level floor. The default config must keep
// choosing exactly the cut it chose before either existed. Pinned on the code before the
// change; the expected values are the hashes that code printed.
TEST_CASE("characterization: the settled default cut is pinned at four cameras") {
	struct Shot {
		float p[3];
		float f[3];
		uint64_t expected;
	};
	const Shot shots[] = {
		{{800.0f, 60.0f, 800.0f}, {0.0f, 0.0f, -1.0f}, 0ull},   // level, along the ground
		{{800.0f, 140.0f, 800.0f}, {0.6f, -0.5f, -0.6f}, 0ull}, // pitched down over a ridge
		{{800.0f, 90.0f, 800.0f}, {0.0f, -1.0f, 0.0f}, 0ull},   // straight down
	};
	for (const Shot &s : shots) {
		ve::LodTreeConfig cfg;
		cfg.stream_radius_m = 1638.4f;
		cfg.max_requests_per_walk = kSettleRequestCap;
		ve::LodTree t(cfg);
		NoOcclusion occ;
		const ve::LodCamera c = cam_looking(s.p, s.f);
		settle(&t, c, &occ, 30);
		ve::LodWalkResult r;
		t.walk(c, &occ, 20000u, &r);
		const uint64_t h = cut_hash(r.draws);
		INFO("pin this shot: " << h << "ull");
		CHECK(h == s.expected);
	}
}
```

- [ ] **Step 2: Run it and record the pins**

Run: `cd extension && scons -Q test 2>&1 | grep -A2 "pin this shot"`
Expected: four FAILs, each printing `pin this shot: <N>ull`. Paste the four values into `expected`, in shot order, then run `./build/tests/ve_tests -tc="characterization: the settled default cut is pinned at four cameras"`.
Expected: PASS. The pins are recorded on the code before any level change.

- [ ] **Step 3: Commit the pin**

```bash
git add extension/tests/test_lod_tree.cpp
git commit -m "test: pin the settled default LoD cut before raster levels exist"
```

- [ ] **Step 4: Write the failing tests for the new levels**

In `extension/tests/test_lod_grid.cpp`, after `the level table matches spec section 2`:

```cpp
// Raster mode's near field (spec 2026-10-04): two levels below 0, still ratio 2. Negative so
// every existing level number keeps its meaning.
TEST_CASE("the raster levels sit below level 0 at 0.2 m and 0.1 m") {
	CHECK(ve::kLodMinLevel == -2);
	CHECK(ve::lod_cell_size(-1) == doctest::Approx(0.2f));
	CHECK(ve::lod_cell_size(-2) == doctest::Approx(0.1f));
	CHECK(ve::lod_cell_size(-3) == doctest::Approx(0.1f)); // clamped like the top
	CHECK(ve::lod_chunk_size(-2) == doctest::Approx(3.2f));
	CHECK(ve::lod_cell_size(0) == 0.4f); // exact: level 0 is untouched
	for (int l = ve::kLodMinLevel + 1; l < ve::kLodLevels; l++)
		CHECK(ve::lod_cell_size(l) == 2.0f * ve::lod_cell_size(l - 1));
	const ve::IVec3 fine = ve::lod_chunk_of_point(-2, 5.0f, 51.0f, -7.0f);
	CHECK(ve::lod_parent(fine) == ve::lod_chunk_of_point(-1, 5.0f, 51.0f, -7.0f));
}
```

In `extension/tests/test_shell_grid.cpp`, after `shell chunks are 3.2 m and tile negative space`:

```cpp
// A near-shell chunk IS a finest-level LoD chunk: same 32 cells of 0.1 m, same origin. The
// shell job's level is therefore a real level, and lod_chunk_origin() places it correctly.
TEST_CASE("a shell chunk is the finest LoD level's chunk") {
	CHECK(ve::kShellLevel == ve::kLodMinLevel);
	for (const ve::IVec3 c : {ve::IVec3{0, 0, 0}, ve::IVec3{-3, 17, 5}}) {
		float a[3], b[3];
		ve::shell_chunk_origin(c, a);
		ve::lod_chunk_origin(ve::kShellLevel, c, b);
		for (int i = 0; i < 3; i++) CHECK(a[i] == doctest::Approx(b[i]));
	}
}
```

At the end of `extension/tests/test_lod_tree.cpp`:

```cpp
TEST_CASE("raster mode refines the ground under the camera to 0.1 m") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	cfg.max_requests_per_walk = kSettleRequestCap;
	cfg.fade_start_m = 0.0f; // raster mode: the far field owns every distance
	cfg.min_level = ve::kLodMinLevel;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 53.0f, 800.0f);
	settle(&t, c, &occ, 30);
	ve::LodWalkResult r;
	t.walk(c, &occ, 20000u, &r);
	int finest = ve::kLodLevels;
	for (const ve::LodDrawItem &d : r.draws) finest = std::min(finest, d.level);
	CHECK(finest == ve::kLodMinLevel);
}

TEST_CASE("the default config never draws or requests below level 0") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	cfg.max_requests_per_walk = kSettleRequestCap;
	cfg.fade_start_m = 0.0f; // build up to the camera, so the floor is what stops the descent
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 53.0f, 800.0f);
	settle(&t, c, &occ, 30);
	ve::LodWalkResult r;
	t.walk(c, &occ, 20000u, &r);
	REQUIRE(!r.draws.empty());
	for (const ve::LodDrawItem &d : r.draws) CHECK(d.level >= 0);
	for (const ve::LodBuildRequest &q : r.requests) CHECK(q.level >= 0);
}

// The near-dense rule exists for the seam with the marched near field: it keeps level 0
// dense out to 300 m. It must not push raster mode below 0 there -- that would be 0.1 m
// chunks over a 300 m disc. Below level 0, screen-space error alone decides.
TEST_CASE("the near-dense radius never forces a level below 0") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	cfg.min_level = ve::kLodMinLevel;
	ve::LodTree t(cfg);
	NoOcclusion occ;
	const ve::LodCamera c = cam_at(800.0f, 60.0f, 800.0f);
	// 300 m out: inside the near-dense radius, far too small on screen to want 0.2 m.
	const ve::IVec3 l1 = ve::lod_chunk_of_point(1, 800.0f, 51.0f, 500.0f);
	make_ready_full_level0(&t, 1638.4f, l1);
	const ve::IVec3 l0_base = ve::lod_child_base(l1);
	for (int k0 = 0; k0 < 8; k0++) {
		const ve::IVec3 l0{l0_base.x + (k0 & 1), l0_base.y + ((k0 >> 1) & 1),
				l0_base.z + ((k0 >> 2) & 1)};
		const ve::IVec3 base = ve::lod_child_base(l0);
		for (int k = 0; k < 8; k++)
			t.note_ready(-1, {base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)},
					1, 1);
	}
	ve::LodWalkResult r;
	t.walk(c, &occ, 1u, &r);
	int l0_drawn = 0;
	int below = 0;
	for (const ve::LodDrawItem &d : r.draws) {
		if (d.level == 0) l0_drawn++;
		if (d.level < 0) below++;
	}
	CHECK(l0_drawn == 8);
	CHECK(below == 0);
}

TEST_CASE("an edit dirties the raster levels too") {
	ve::LodTreeConfig cfg;
	cfg.min_level = ve::kLodMinLevel;
	ve::LodTree t(cfg);
	const ve::IVec3 c = ve::lod_chunk_of_point(-2, 10.0f, 51.0f, 10.0f);
	t.note_ready(-2, c, 1, 1);
	float lo[3], hi[3];
	ve::lod_chunk_aabb(-2, c, lo, hi);
	t.mark_dirty(lo, hi);
	CHECK(t.is_dirty(-2, c));
	int chunks = 0;
	int levels = 0;
	t.dirty_stats(&chunks, &levels);
	CHECK(chunks == 1);
	CHECK(levels == 1);
}
```

- [ ] **Step 5: Run them to verify they fail**

Run: `cd extension && scons -Q test`
Expected: compile errors: `kLodMinLevel` undeclared, `no member named 'min_level' in 've::LodTreeConfig'`.

- [ ] **Step 6: Add the levels to the grid**

`extension/src/lod/lod_grid.h`, after `inline constexpr float kLodBaseCell = 0.4f;`:

```cpp
// Raster mode's near field (spec 2026-10-04 §3): two levels BELOW level 0, at 0.2 m and
// 0.1 m. Negative so every level number above keeps its meaning; only
// LodTreeConfig::min_level lets the walk reach them.
inline constexpr int kLodMinLevel = -2;
```

`extension/src/lod/lod_grid.cpp`:

```cpp
int clamp_level(int level) { return std::max(kLodMinLevel, std::min(level, kLodLevels - 1)); }
```

and in `lod_cell_size`:

```cpp
	// ldexp, not a shift: the level may be negative. Scaling by a power of two is exact, so
	// levels 0..7 are the same floats as 0.4f * (1 << level).
	return std::ldexp(kLodBaseCell, clamp_level(level));
```

- [ ] **Step 7: Add the floor to the tree**

`extension/src/lod/lod_tree.h`, `LodTreeConfig`, after `fade_start_m`:

```cpp
	// The finest level the walk may descend to: 0 in raymarched mode, kLodMinLevel in raster
	// mode (spec 2026-10-04 §3). Nothing below it is requested, drawn or waited for.
	int min_level = 0;
```

In `class LodTree`, after `set_fade_start_m`:

```cpp
	void set_min_level(int v) { cfg_.min_level = v; }
```

`extension/src/lod/lod_tree.cpp`:

In `children_ready`, replace `if (level <= 0) return false;` with:

```cpp
	if (level <= cfg_.min_level) return false;
```

Replace `want_finer` with:

```cpp
bool LodTree::want_finer(int level, IVec3 c, float area) const {
	if (level <= cfg_.min_level) return false;
	const float chunk_distance_m = lod_chunk_distance(level, c, last_cam_pos_);
	// Dense down to level 0 only: the seam this protects is the marched near field's, and
	// below 0 (raster mode) screen-space error alone decides -- forcing 0.1 m over the whole
	// 300 m disc would be tens of thousands of chunks.
	const bool near_dense = level > 0 && kLodNearDenseRadiusM > 0.0f &&
			chunk_distance_m < kLodNearDenseRadiusM;
	return near_dense || area > cfg_.sse_area_thresh;
}
```

In `mark_dirty`, change the loop header to:

```cpp
	for (int level = kLodMinLevel; level < kLodLevels; level++) {
```

In `dirty_stats`, replace `bool seen[kLodLevels] = {};` and its two uses with:

```cpp
	bool seen[kLodLevels - kLodMinLevel] = {};
	for (const auto &kv : nodes_) {
		if (!kv.second.dirty) continue;
		(*chunks)++;
		const int slot = kv.first.level - kLodMinLevel;
		if (!seen[slot]) {
			seen[slot] = true;
			(*levels)++;
		}
	}
```

- [ ] **Step 8: Make the shell level the finest level**

`extension/src/transparency/shell_grid.h`, replace the `kShellLevel` line:

```cpp
// A near-shell chunk is exactly a finest-level LoD chunk (32 cells of 0.1 m), so a shell job
// carries that level: MeshService places its override-table lookup with lod_chunk_origin().
inline constexpr int kShellLevel = kLodMinLevel; // LodBuildJob/Result::level of a near-shell build
static_assert(kShellCell == kLodBaseCell * 0.25f, "the shell grid is LoD level kLodMinLevel");
```

- [ ] **Step 9: Run the native tests**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`, including the characterization case with its unchanged pins.

- [ ] **Step 10: Check nothing reads the old sentinel**

Run: `grep -rn "kShellLevel\|level.*== *-1\|\"-1:" extension/src tests --include='*.cpp' --include='*.h' --include='*.gd'`
Expected: only the definition, the `LodKey`/`job.level` uses in `lod_system.cpp` and `hooks_lod.cpp`, and the comment in `lod_build_pass.h`. None compare against `-1`.

- [ ] **Step 11: GPU suites that touch the shell and the LoD**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd -a res://tests/test_lod_build.gd -a res://tests/test_lod_stream.gd -a res://tests/test_lod_raster_golden.gd -a res://tests/test_lod_seam.gd -a res://tests/test_sun_shadow.gd`
Expected: PASS, or failing only where Task 0's baseline already failed. The goldens must not move.

- [ ] **Step 12: Commit**

```bash
git add extension/src/lod/lod_grid.h extension/src/lod/lod_grid.cpp extension/src/lod/lod_tree.h \
	extension/src/lod/lod_tree.cpp extension/src/transparency/shell_grid.h \
	extension/tests/test_lod_tree.cpp extension/tests/test_lod_grid.cpp extension/tests/test_shell_grid.cpp
git commit -m "feat: LoD levels -1 and -2 (0.2 m, 0.1 m) behind a min_level floor"
```

---

### Task 4: Raster mode drives the walk to 0.1 m

**Files:**
- Modify: `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/debug/hooks_lod.cpp`
- Test: `tests/test_raster_mode.gd`

**Interfaces:**
- Consumes: `RenderOrchestrator::raster_mode()` (Task 1), `LodTree::set_min_level`, `ve::kLodMinLevel` (Task 3).
- Produces: `LodStats::draw_min_level` (finest level in the current cut, `kLodLevels` when empty) and `LodStats::fine_pages` (pages owned by chunks below level 0); `debug_lod_stats()` keys `"draw_min_level"` and `"fine_pages"`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_raster_mode.gd`:

```gdscript
func test_raster_mode_refines_the_near_field_to_ten_centimetres() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	settle_ticks(w)
	var s: Dictionary = w.hooks().debug_lod_stats()
	assert_int(int(s["draw_min_level"])).override_failure_message(str(s)).is_equal(-2)
	assert_int(int(s["partial_allocations"])).is_equal(0)

func test_raymarched_mode_never_draws_below_level_zero() -> void:
	var w := make_world()
	settle_ticks(w)
	var s: Dictionary = w.hooks().debug_lod_stats()
	assert_int(int(s["draw_min_level"])).override_failure_message(str(s)).is_greater_equal(0)
	assert_int(int(s["fine_pages"])).is_equal(0)

# Review focus 1: a live switch back must give the fine pages back. They are not freed
# eagerly; they age out like any chunk the walk stops touching (kLodEvictFrames).
func test_switching_back_to_raymarching_frees_the_fine_pages() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	settle_ticks(w)
	assert_int(int(w.hooks().debug_lod_stats()["fine_pages"])).is_greater(0)
	w.set_effect_enabled("raymarch", true)
	var s := {}
	for i in range(2000):
		w.hooks().debug_lod_tick(CAM, FWD.normalized())
		s = w.hooks().debug_lod_stats()
		if int(s["fine_pages"]) == 0 and int(s["builds_in_flight"]) == 0:
			break
	assert_int(int(s["fine_pages"])).override_failure_message(str(s)).is_equal(0)
	assert_int(int(s["partial_allocations"])).is_equal(0)

# Review focus 3: an edit in raster mode must reach the 0.1 m chunks, not stop at level 0.
func test_an_edit_in_raster_mode_requests_the_fine_chunks() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	settle_ticks(w)
	var hit: Dictionary = w.raycast(CAM, FWD.normalized(), 400.0)
	assert_bool(hit["hit"]).is_true()
	w.hooks().debug_apply_sphere_subtract(hit["pos"], 2.0)
	w.hooks().debug_lod_tick(CAM, FWD.normalized())
	var ids: Array = w.hooks().debug_lod_stats()["pending_request_ids"]
	var fine := ids.filter(func(id): return String(id).begins_with("-2:"))
	assert_int(fine.size()).override_failure_message(
		"the crater requested no 0.1 m rebuild: %s" % [ids]).is_greater(0)
	settle_ticks(w)
	assert_int(int(w.hooks().debug_lod_stats()["dirty_chunks"])).is_equal(0)
```

- [ ] **Step 2: Run them to verify they fail**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_raster_mode.gd`
Expected: the four new tests FAIL. `draw_min_level` and `fine_pages` are missing from the stats (null reads as 0), and no `-2:` request appears.

- [ ] **Step 3: Set the floor every tick**

`extension/src/lod/lod_system.cpp`, `LodSystem::tick`, inside the block that calls `lod_tree_->set_fade_start_m(fs);`, after that call:

```cpp
		// Raster mode lets the walk descend to 0.1 m (spec 2026-10-04 §3). Switching back
		// leaves the fine nodes unvisited, so they age out through collect_evictions.
		lod_tree_->set_min_level(render()->raster_mode() ? ve::kLodMinLevel : 0);
```

- [ ] **Step 4: Report the stats**

`extension/src/lod/lod_system.h`, `struct LodStats`, after `shell_pages`:

```cpp
	int draw_min_level = 0; // finest level in the current cut; ve::kLodLevels when empty
	int fine_pages = 0;     // pages owned by chunks below level 0 (raster mode)
```

`extension/src/lod/lod_system.cpp`, `LodSystem::stats()`, after the line that sums `s.draw_pages`:

```cpp
	s.draw_min_level = ve::kLodLevels;
	for (const ve::LodDrawItem &item : lod_walk_.draws)
		s.draw_min_level = std::min(s.draw_min_level, item.level);
	for (const auto &kv : lod_pages_of_)
		if (kv.first.level < 0) s.fine_pages += static_cast<int>(kv.second.size());
```

`extension/src/debug/hooks_lod.cpp`, `debug_lod_stats`, after `d["shell_pages"] = s.shell_pages;`:

```cpp
	d["draw_min_level"] = s.draw_min_level;
	d["fine_pages"] = s.fine_pages;
```

- [ ] **Step 5: Run them to verify they pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_raster_mode.gd -a res://tests/test_lod_budget.gd -a res://tests/test_lod_pool.gd`
Expected: all PASS, or failing only where Task 0's baseline already failed. If `test_raster_mode_refines…` reports `budget_bound` other than `"none"` in its message, the page budget binds: record the stats in the task notes for Task 7 rather than raising `max_lod_pages_` here.

- [ ] **Step 6: Commit**

```bash
git add extension/src/lod/lod_system.h extension/src/lod/lod_system.cpp extension/src/debug/hooks_lod.cpp \
	tests/test_raster_mode.gd
git commit -m "feat: raster mode lets the LoD walk descend to 0.1 m"
```

---

### Task 5: The raster frame: no marcher, a sky fill, no scatter sun march

**Files:**
- Modify: `shaders/composite.frag.glsl`
- Modify: `extension/src/render/composite_pass.h`, `extension/src/render/composite_pass.cpp`
- Modify: `extension/src/render/frame.cpp`
- Modify: `shaders/grass_scatter.comp.glsl`, `shaders/leaf_scatter.comp.glsl`
- Test: `tests/test_raster_mode.gd`

**Interfaces:**
- Consumes: `FrameSettings::raster_mode` (Task 1).
- Produces: `CompositePass::set_sky_only(bool)`. With it on, `draw()` ignores its source textures and writes sky (material 0, depth 0) to every pixel. Shader define `SKY_ONLY`. Grass reads `grass.limits.z != 0` and leaves read `leaf.limits.w != 0` as "do not march the sun".

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_raster_mode.gd`:

```gdscript
func material_id(w: VoxelWorld, name: String) -> int:
	for m in w.material_table():
		if m["name"] == name:
			return m["id"]
	return 0

func test_a_raster_frame_runs_without_the_marcher() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	var d := settle_frames(w)
	var ok: PackedStringArray = d["stages_ok"]
	for stage in ["stream", "composite", "lod", "deferred", "inject"]:
		assert_bool(ok.has(stage)).override_failure_message(
			"stage %s did not complete: %s" % [stage, ok]).is_true()
	assert_bool(ok.has("raymarch")).override_failure_message(
		"the marcher ran in raster mode: %s" % [ok]).is_false()
	assert_float(float(d["gb_ground_fraction"])).is_greater(0.95)
	assert_float(float(d["mean_luma"])).override_failure_message(
		"the lit image is black").is_greater(0.01)

# Review focus 4: nothing draws the sky in raster mode except the composite's sky fill.
func test_looking_up_in_raster_mode_shows_the_sky() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	var up := Vector3(0.2, 1.0, 0.2)
	var d := settle_frames(w, CAM, up)
	assert_float(float(d["gb_ground_fraction"])).is_less(0.05)
	var c: Color = d["center_lit"]
	assert_bool(c.b > c.r and c.b > 0.05).override_failure_message(
		"the centre is not sky: %s" % c).is_true()

# Review focus 5: the near shell grid is off in raster mode, so the fine LoD chunks' own
# shell quads are what draw ice.
func test_ice_renders_in_raster_mode() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	var ice := material_id(w, "ice")
	var hit: Dictionary = w.raycast(CAM, FWD.normalized(), 400.0)
	assert_bool(hit["hit"]).is_true()
	w.hooks().debug_apply_sphere_add(hit["pos"], 1.0, ice)
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		if int((d["center_front"] as Color).a + 0.5) == ice:
			break
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"no ice front in raster mode: %s" % d).is_equal(ice)
```

- [ ] **Step 2: Run them to verify the frame test fails**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_raster_mode.gd`
Expected: `test_a_raster_frame_runs_without_the_marcher` FAILS with "the marcher ran in raster mode". The sky and ice tests may already pass, because the marcher still runs with zero reach; they pin behaviour the next steps must keep.

- [ ] **Step 3: The sky-only shader variant**

`shaders/composite.frag.glsl`. Make three edits.

(a) Replace the material-array lines at the top:

```glsl
#define VE_MATERIAL_ARRAYS
layout(set = 0, binding = 2) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 3) uniform sampler2DArray material_surface_tex;
```

with:

```glsl
#ifndef SKY_ONLY
#define VE_MATERIAL_ARRAYS
layout(set = 0, binding = 2) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 3) uniform sampler2DArray material_surface_tex;
#endif
```

(b) Delete the line `layout(push_constant, std430) uniform Push { COMPOSITE_PUSH_FIELDS } pc;` that follows the `src_surface` sampler. Then insert this block immediately before the line `layout(set = 0, binding = 0) uniform sampler2D src_overlay; // rgb overlay, a sun visibility`:

```glsl
layout(push_constant, std430) uniform Push { COMPOSITE_PUSH_FIELDS } pc;

#ifdef SKY_ONLY
// Raster mode (spec 2026-10-04 §4): no marcher ran, so every pixel starts as the marcher's
// miss -- sky in the albedo at full sun, material 0, gloss 0, depth at the reverse-Z far
// plane -- and the LoD raster draws over it with depth testing. No source textures and no
// uniform set: this variant declares none. Clamped because the marcher's rgba8 overlay is.
void main() {
	vec2 ndc = vec2(uv_in.x * 2.0 - 1.0, 1.0 - uv_in.y * 2.0);
	vec3 rd = normalize(pc.fade.yzw
			+ pc.right_tanx.xyz * ndc.x * pc.right_tanx.w
			+ pc.up_tany.xyz * ndc.y * pc.up_tany.w);
	out_albedo = vec4(clamp(sky_color(rd), 0.0, 1.0), 1.0);
	out_surface = GB_PACK_SURFACE_OCT(oct_encode(-rd), 0.0, 0.0);
	gl_FragDepth = 0.0;
}
#else
```

(c) Append `#endif` as the last line of the file, after the closing `}` of the existing `main`.

- [ ] **Step 4: Compile and draw the variant**

`extension/src/render/composite_pass.h`, in `public:` after `last_draw_ok()`:

```cpp
	// Raster mode: draw() writes sky to every pixel and reads none of its source textures.
	void set_sky_only(bool v) { sky_only_ = v; }
```

In `private:`, change `RID shader_, shader_marker_;` to `RID shader_, shader_marker_, shader_sky_;` and add after `bool pipeline_marker_ = false;`:

```cpp
	bool pipeline_sky_ = false;
	bool sky_only_ = false;
```

`extension/src/render/composite_pass.cpp`:

In `initialize`, after `shader_marker_ = …;`:

```cpp
	shader_sky_ = gpu::compile_raster(rd, group_, "CompositePass", "composite.vert.glsl",
			"composite.frag.glsl", "#define SKY_ONLY 1\n");
```

In `teardown`, change `shader_ = shader_marker_ = pipeline_ = …` to `shader_ = shader_marker_ = shader_sky_ = pipeline_ = …`.

Replace `ensure_pipeline` with:

```cpp
bool CompositePass::ensure_pipeline(RenderingDevice *rd, RID albedo, RID surface, RID depth,
		RID marker) {
	// The sky variant has no seam-marker output, so a marker never joins its framebuffer.
	const bool want_marker = marker.is_valid() && !sky_only_;
	const RID shader = sky_only_ ? shader_sky_ : (want_marker ? shader_marker_ : shader_);
	if (!shader.is_valid()) return false;
	const std::vector<RID> attachments = want_marker
			? std::vector<RID>{albedo, surface, marker, depth}
			: std::vector<RID>{albedo, surface, depth};
	if (!framebuffer_.get(rd, group_, attachments).is_valid()) return false;
	if (!pipeline_.is_valid() || pipeline_marker_ != want_marker || pipeline_sky_ != sky_only_) {
		gpu::RdDevice device{rd};
		group_.free(device, pipeline_);
		gpu::RasterState state;
		state.color_attachments = ve::layout::kGbColorAttachments + (want_marker ? 1 : 0);
		pipeline_marker_ = want_marker;
		pipeline_sky_ = sky_only_;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader, framebuffer_.format(), state);
	}
	return pipeline_.is_valid();
}
```

In `draw`:

- replace `const RID shader = marker.is_valid() ? shader_marker_ : shader_;` with:

```cpp
	const RID shader = sky_only_ ? shader_sky_ : (marker.is_valid() ? shader_marker_ : shader_);
```

- replace the uniform-set block (from `gpu::RdDevice device{rd};` through `if (!set.is_valid()) return;`) with:

```cpp
	RID set;
	if (!sky_only_) {
		gpu::RdDevice device{rd};
		set = set_.get(device, group_, shader, 0, {
				gpu::sampled(0, sampler_linear_, src_overlay),
				gpu::sampled(1, sampler_nearest_, src_hitpos),
				gpu::sampled(2, materials.sampler(), materials.albedo_array()),
				gpu::sampled(3, materials.sampler(), materials.surface_array()),
				gpu::sampled(4, sampler_nearest_, src_surface)});
		if (!set.is_valid()) return;
	}
```

- replace `if (marker.is_valid()) clears.push_back(Color(0, 0, 0, 0));` with `if (marker.is_valid() && !sky_only_) clears.push_back(Color(0, 0, 0, 0));`
- replace `rd->draw_list_bind_uniform_set(dl, set, 0);` with `if (!sky_only_) rd->draw_list_bind_uniform_set(dl, set, 0);`

- [ ] **Step 5: Skip the marcher in the frame**

`extension/src/render/frame.cpp`, `render_pre_opaque`. Wrap everything from `const int islands = render_.island_slot_count();` through the `end_stage(rd, kStageRaymarch);` that follows `rmp->render(...)` in:

```cpp
	// Raster mode (spec 2026-10-04 §4): nothing is marched, islands included. The composite
	// below fills the G-buffer with sky and the LoD raster draws every surface.
	if (!settings.raster_mode) {
		// ... the existing island cull, target rebuild check and rmp->render block, unchanged ...
	}
```

Directly after `timings->begin(rd, "composite");`, add:

```cpp
	cmp->set_sky_only(settings.raster_mode);
```

Leave the `cmp->draw(...)` call unchanged; the sky variant ignores the marcher textures it is handed.

- [ ] **Step 6: No sun march in the scatter passes**

`frame.cpp`, grass block: change `const ve::GrassLayout gl = grass_layout(grass_cam, grass_vp);` to:

```cpp
	ve::GrassLayout gl = grass_layout(grass_cam, grass_vp);
	// Raster mode marches nothing: a blade writes full sun and deferred shadows it from the
	// sun map, which owns every pixel there (spec 2026-10-04 §4). A spare lane, no layout change.
	gl.params.limits[2] = settings.raster_mode ? 1 : 0;
```

Leaf block: change `const ve::LeafLayout ll = leaf_layout(leaf_cam, leaf_vp);` to:

```cpp
	ve::LeafLayout ll = leaf_layout(leaf_cam, leaf_vp);
	ll.params.limits[3] = settings.raster_mode ? 1 : 0; // as grass: no sun march in raster mode
```

`shaders/grass_scatter.comp.glsl`, replace:

```glsl
	float sun = terrain_sun_visibility(p + n * 0.05 + vec3(0.0, height * 0.35, 0.0),
			RAY_SHADOW_DIST);
```

with:

```glsl
	// Raster mode (grass.limits.z != 0) marches nothing: full sun here, and the deferred
	// pass shadows the blade from the sun map, which owns every pixel in that mode.
	float sun = grass.limits.z != 0 ? 1.0
			: terrain_sun_visibility(p + n * 0.05 + vec3(0.0, height * 0.35, 0.0),
					RAY_SHADOW_DIST);
```

`shaders/leaf_scatter.comp.glsl`, replace `float sun = terrain_sun_visibility(p, RAY_SHADOW_DIST);` with:

```glsl
	// Raster mode (leaf.limits.w != 0): no march, as grass_scatter; the sun map shadows it.
	float sun = leaf.limits.w != 0 ? 1.0 : terrain_sun_visibility(p, RAY_SHADOW_DIST);
```

- [ ] **Step 7: Run the raster suite**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_raster_mode.gd`
Expected: all PASS.

- [ ] **Step 8: Raymarched mode unchanged**

Run: `./gdunit_tests.sh -a res://tests/test_frame_contract.gd -a res://tests/test_composite_golden.gd -a res://tests/test_grass_golden.gd -a res://tests/test_deferred_golden.gd -a res://tests/test_lod_raster_golden.gd -a res://tests/test_ssao_golden.gd -a res://tests/test_contact_shadow_golden.gd`
Expected: PASS, or failing only where Task 0's baseline already failed. A golden that moves means raymarched mode changed: stop and find out why.

- [ ] **Step 9: Commit**

```bash
git add shaders/composite.frag.glsl extension/src/render/composite_pass.h \
	extension/src/render/composite_pass.cpp extension/src/render/frame.cpp \
	shaders/grass_scatter.comp.glsl shaders/leaf_scatter.comp.glsl tests/test_raster_mode.gd
git commit -m "feat: raster frames skip the marcher, fill the sky and march no scatter sun"
```

---

### Task 6: Islands are meshed and drawn in raster mode

**Files:**
- Create: `shaders/island_xform.glslh`
- Modify: `extension/src/transparency/shell_grid.h`, `extension/src/transparency/shell_grid.cpp`
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`
- Modify: `extension/src/debug/hooks_physics.cpp`
- Modify: `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/render/lod_raster_pass.h`, `extension/src/render/lod_raster_pass.cpp`
- Modify: `shaders/lod.vert.glsl`, `shaders/shell.vert.glsl`
- Modify: `extension/src/render/frame.cpp`
- Test: `extension/tests/test_shell_grid.cpp`, `tests/test_raster_mode.gd`

**Interfaces:**
- Consumes: `RenderOrchestrator::raster_mode()` (Task 1).
- Produces: `enum class ve::IslandMeshKind { kShell, kOpaque }`; `void ve::island_blocks(const VolumeData &v, const float lattice_origin[3], float voxel, IslandMeshKind kind, std::vector<IslandShellBlock> *out)` (replaces `island_shell_blocks`); `uint32_t ve::island_mesh_flags(int atlas_slot)`; `IslandShell::opaque`; `RenderOrchestrator::contour_island_meshes(int slot, const ve::VolumeData &, const IslandSlotDesc &)` (public, renamed from the private `contour_island_shell`); `LodRasterPass::set_island_desc(RID)`; GLSL `void island_place(uint island, inout vec3 p, inout vec3 n)`.

- [ ] **Step 1: Write the failing native test**

`extension/tests/test_shell_grid.cpp`. In the anonymous namespace at the top, add:

```cpp
// A solid ball of radius 20 voxels in the middle of an island volume; its upper half is ice,
// the lower half rock.
ve::VolumeData ice_capped_ball() {
	ve::VolumeData v;
	v.dim = ve::kIslandDim;
	v.sdf.assign(size_t(v.voxel_count()), ve::encode_sdf(0.3f));
	v.mat.assign(size_t(v.voxel_count()), 0);
	const float voxel = ve::kIslandVoxelFine;
	for (int z = 0; z < v.dim; z++)
		for (int y = 0; y < v.dim; y++)
			for (int x = 0; x < v.dim; x++) {
				const float dx = x - 32.0f, dy = y - 32.0f, dz = z - 32.0f;
				const float d = (std::sqrt(dx * dx + dy * dy + dz * dz) - 20.0f) * voxel;
				const int i = ve::VolumeSet::voxel_index(v.dim, x, y, z);
				v.sdf[size_t(i)] = ve::encode_sdf(d);
				if (d <= 0.0f)
					v.mat[size_t(i)] = uint8_t(ve::material_id(y >= 32 ? "ice" : "rock"));
			}
	return v;
}
```

In the existing `an island's shell is split into 32-cell blocks…` case, replace both calls `ve::island_shell_blocks(v, origin, voxel, &blocks);` with `ve::island_blocks(v, origin, voxel, ve::IslandMeshKind::kShell, &blocks);`.

After that case, add:

```cpp
// Raster mode draws an island from this mesh instead of marching it: the opaque view of the
// island (a transparent solid reads as outside), so the ice is left to the shell.
TEST_CASE("an island's opaque mesh drops its transparent part and keeps the rest") {
	ve::VolumeData v = ice_capped_ball();
	const float origin[3] = {-1.6f, -1.6f, -1.6f};
	std::vector<ve::IslandShellBlock> blocks;
	ve::island_blocks(v, origin, ve::kIslandVoxelFine, ve::IslandMeshKind::kOpaque, &blocks);
	REQUIRE(!blocks.empty());
	size_t quads = 0;
	for (const ve::IslandShellBlock &b : blocks) {
		CHECK(b.quads.size() == b.normals.size());
		for (const ve::LodQuad &q : b.quads) {
			ve::LodQuadFields f{};
			ve::lod_quad_unpack(q, &f);
			CHECK(f.material != 0);
			CHECK_FALSE(ve::material_transparent(uint16_t(f.material)));
		}
		quads += b.quads.size();
	}
	CHECK(quads > 100);

	// All ice: nothing opaque is left to draw.
	for (uint8_t &m : v.mat)
		if (m != 0) m = uint8_t(ve::material_id("ice"));
	ve::island_blocks(v, origin, ve::kIslandVoxelFine, ve::IslandMeshKind::kOpaque, &blocks);
	CHECK(blocks.empty());
}

TEST_CASE("an island's opaque mesh page flags carry the slot and no shell bit") {
	for (const int slot : {0, 1, 7, 31}) {
		const uint32_t flags = ve::island_mesh_flags(slot);
		CHECK((flags & 1u) == 0u);
		CHECK(int(flags >> 8) == slot + 1);
	}
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons -Q test`
Expected: compile errors: `island_blocks`, `IslandMeshKind` and `island_mesh_flags` undeclared.

- [ ] **Step 3: Contour both meshes**

`extension/src/transparency/shell_grid.h`: replace the `island_shell_blocks` declaration and its comment with:

```cpp
// Which mesh of an island lattice to contour. kShell: the transparent shell, only quads whose
// solid side is transparent (empty when the volume holds no transparent label). kOpaque: the
// opaque view (a transparent solid reads as outside), every quad -- raster mode draws the
// island from it instead of marching it (spec 2026-10-04 §5).
enum class IslandMeshKind { kShell, kOpaque };
// That mesh, split into 32-cell blocks. `lattice_origin` and `voxel` are the island
// descriptor's. Blocks with no quads are omitted.
void island_blocks(const VolumeData &v, const float lattice_origin[3], float voxel,
		IslandMeshKind kind, std::vector<IslandShellBlock> *out);
```

After `island_shell_flags`, add:

```cpp
// The chunk-record FLAGS word of an island's OPAQUE mesh page: the slot in bits 8.. exactly
// as island_shell_flags, with bit 0 clear because it is not a shell page. lod.vert.glsl
// places it through the island descriptor.
inline uint32_t island_mesh_flags(int atlas_slot) {
	return static_cast<uint32_t>(atlas_slot + 1) << 8;
}
```

`extension/src/transparency/shell_grid.cpp`. Rename `island_shell_blocks` to `island_blocks` with the new signature, and change its body as follows:

```cpp
void island_blocks(const VolumeData &v, const float lattice_origin[3], float voxel,
		IslandMeshKind kind, std::vector<IslandShellBlock> *out) {
	if (!out) return;
	out->clear();
	const bool shell = kind == IslandMeshKind::kShell;
	if (shell && !volume_has_transparent(v)) return;
	const int n = kLodChunkLattice;
	const int blocks = (v.dim + kLodChunkCells - 1) / kLodChunkCells;
	std::vector<uint8_t> lat(static_cast<size_t>(n) * n * n);
	std::vector<uint16_t> mat(lat.size());
	std::vector<uint8_t> opaque(shell ? 0 : lat.size());
	const uint8_t outside = encode_sdf(kSdfRange);
```

Inside the per-sample loop, replace `any = any || material_transparent(v.mat[static_cast<size_t>(s)]);` with:

```cpp
							const uint8_t m = v.mat[static_cast<size_t>(s)];
							any = any || (shell ? material_transparent(m)
												: decode_sdf(v.sdf[static_cast<size_t>(s)]) <= 0.0f &&
														!material_transparent(m));
```

Replace `lod_contour(lat.data(), mat.data(), &r, true);` with:

```cpp
				if (shell) {
					lod_contour(lat.data(), mat.data(), &r, true);
				} else {
					lod_opaque_lattice(lat.data(), mat.data(), voxel, opaque.data());
					lod_contour(opaque.data(), mat.data(), &r, false);
				}
```

Add `#include "lod/lod_reduce.h"` to `shell_grid.cpp` if `lod_opaque_lattice` is not already visible there.

- [ ] **Step 4: Run the native tests**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`. If the build fails at the two remaining `island_shell_blocks` callers, Step 6 fixes them; the native build compiles only pure sources, so it should already pass.

- [ ] **Step 5: Write the failing GPU tests**

Append to `tests/test_raster_mode.gd`:

```gdscript
# The island fixture's world (tests/test_island_render.gd): rows 58..59 are solid here.
func make_island_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	for i in range(60):
		w.hooks().debug_stream_frame(Vector3(20.0, 56.0, 20.0))
	w.hooks().debug_lod_tick(Vector3(20.0, 56.0, 20.0), FWD.normalized())
	return w

func place_island(w: VoxelWorld) -> Dictionary:
	var placed: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 58, 25),
		Vector3i(26, 59, 26), Vector3(0.0, 40.0, 0.0))
	assert_bool(placed.get("ok", false)).override_failure_message(str(placed)).is_true()
	return placed

# Looking slightly UP at an island lifted 40 m: everything behind it is sky, so a non-zero
# G-buffer material at the centre can only be the island.
func island_cam(placed: Dictionary) -> Vector3:
	return (placed["world_center"] as Vector3) - Vector3(8.0, 1.5, 0.0)

func island_fwd(placed: Dictionary) -> Vector3:
	return (placed["world_center"] as Vector3) - island_cam(placed)

func test_an_island_renders_in_raster_mode() -> void:
	var w := make_island_world()
	w.set_effect_enabled("raymarch", false)
	var placed := place_island(w)
	var d := settle_frames(w, island_cam(placed), island_fwd(placed))
	assert_int(int(d["center_gb_material"])).override_failure_message(
		"the island was not drawn: %s" % d).is_not_equal(0)
	w.hooks().debug_clear_test_island(0)
	for i in range(5):
		d = frame(w, island_cam(placed), island_fwd(placed))
	assert_int(int(d["center_gb_material"])).override_failure_message(
		"the island outlived its slot: %s" % d).is_equal(0)
	assert_int(int(w.hooks().debug_lod_stats()["partial_allocations"])).is_equal(0)

# Review focus 2: the mesh must already exist for an island extracted while raymarching.
func test_an_island_extracted_while_raymarching_renders_after_the_switch() -> void:
	var w := make_island_world()
	var placed := place_island(w)
	for i in range(3):
		frame(w, island_cam(placed), island_fwd(placed))
	w.set_effect_enabled("raymarch", false)
	var d := settle_frames(w, island_cam(placed), island_fwd(placed))
	assert_int(int(d["center_gb_material"])).override_failure_message(
		"the island vanished at the switch: %s" % d).is_not_equal(0)
```

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_raster_mode.gd`
Expected: both island tests FAIL (`center_gb_material` is 0, because nothing draws islands in raster mode). If `./build.sh` fails to compile at `island_shell_blocks`, do Step 6 first, then run.

- [ ] **Step 6: One contour function, called by the drain and the fixture**

`extension/src/render/orchestrator.h`:

- `struct IslandShell`, after `blocks`:

```cpp
	std::vector<ve::IslandShellBlock> opaque; // the island's opaque mesh; raster mode draws it
```

- Remove the private declaration `void contour_island_shell(int slot, const ve::VolumeData &data, const IslandSlotDesc &d);`. In `public:`, next to `queue_island_shell`, add:

```cpp
	// Contours an island's meshes from its bytes and queues them: the transparent shell
	// (transparency on and a transparent label present) and the opaque mesh raster mode
	// draws (always, so a live switch to raster finds it). The upload drain and the debug
	// island fixture both call this, so the fixture tests the shipping path.
	void contour_island_meshes(int slot, const ve::VolumeData &data, const IslandSlotDesc &d);
```

`extension/src/render/orchestrator.cpp`:

- Replace `contour_island_shell` with:

```cpp
void RenderOrchestrator::contour_island_meshes(int slot, const ve::VolumeData &data,
		const IslandSlotDesc &d) {
	IslandShell shell;
	shell.atlas_slot = slot;
	shell.voxel = d.voxel;
	if (transparency_settings().enabled)
		ve::island_blocks(data, d.lattice_origin, d.voxel, ve::IslandMeshKind::kShell, &shell.blocks);
	ve::island_blocks(data, d.lattice_origin, d.voxel, ve::IslandMeshKind::kOpaque, &shell.opaque);
	queue_island_shell(std::move(shell)); // the same locked door the main thread uses
}
```

- In `drain_island_uploads`, change the contour condition from `if (u.to_island_atlas && u.atlas_slot >= 0 && transparency && ve::volume_has_transparent(u.data)) {` to `if (u.to_island_atlas && u.atlas_slot >= 0) {`. Rename both `contour_island_shell(` calls to `contour_island_meshes(`. Above the condition, replace the comment's first sentence with `// The island's meshes are contoured HERE, from the same bytes, because this is the one`.

`extension/src/debug/hooks_physics.cpp`, replace:

```cpp
	if (desc.transparent && world_->context().render->transparency_settings().enabled) {
		godot::IslandShell shell;
		shell.atlas_slot = slot;
		shell.voxel = desc.voxel;
		ve::island_shell_blocks(volume, desc.lattice_origin, desc.voxel, &shell.blocks);
		world_->context().render->queue_island_shell(std::move(shell));
	}
```

with:

```cpp
	world_->context().render->contour_island_meshes(slot, volume, desc);
```

Update the comment above it to `// This fixture uploads the island itself rather than queueing it, so it contours the island's meshes here through the same function drain_island_uploads() calls.`

- [ ] **Step 7: Upload, own and draw the opaque pages**

`extension/src/lod/lod_system.h`, after `island_shell_pages_`:

```cpp
	std::map<int, std::vector<int>> island_mesh_pages_; // raster mode's island meshes, by atlas slot
```

`extension/src/lod/lod_system.cpp`, replace `LodSystem::apply_island_shells` with:

```cpp
void LodSystem::apply_island_shells(std::vector<IslandShell> shells, uint32_t live_mask) {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	if (!lod_pool_ || lod_pool_->page_count() == 0) return;
	const auto release = [this](std::map<int, std::vector<int>> &owner, int slot) {
		const auto it = owner.find(slot);
		if (it == owner.end()) return;
		for (int p : it->second) lod_page_quads_.erase(p);
		lod_pool_->release(it->second);
		owner.erase(it);
	};
	for (std::map<int, std::vector<int>> *owner : {&island_shell_pages_, &island_mesh_pages_})
		for (auto it = owner->begin(); it != owner->end();) {
			const int slot = (it++)->first;
			if (slot < 0 || slot >= 32 || (live_mask & (1u << slot)) == 0u) release(*owner, slot);
		}
	// ponytail: a refused upload (pool/record exhaustion) silently drops this block of the
	// island. Count it per island if that ever shows up as an island with a missing piece.
	const auto upload = [this](const IslandShell &s, const std::vector<ve::IslandShellBlock> &blocks,
								uint32_t flags) {
		std::vector<int> all;
		for (const ve::IslandShellBlock &b : blocks) {
			std::vector<int> pages;
			if (!lod_pool_->upload_at(b.origin_local, s.voxel, 0u, flags, b.quads, b.normals,
						&pages))
				continue; // pool full: this block of the island is not drawn
			for (size_t i = 0; i < pages.size(); i++) {
				const int first = static_cast<int>(i) * ve::kLodQuadsPerPage;
				lod_page_quads_[pages[i]] = std::min(ve::kLodQuadsPerPage,
						static_cast<int>(b.quads.size()) - first);
			}
			all.insert(all.end(), pages.begin(), pages.end());
		}
		return all;
	};
	for (IslandShell &s : shells) {
		if (s.atlas_slot < 0 || s.atlas_slot >= 32) continue;
		// A re-extracted island replaces both of its meshes.
		release(island_shell_pages_, s.atlas_slot);
		release(island_mesh_pages_, s.atlas_slot);
		std::vector<int> shell_pages = upload(s, s.blocks, ve::island_shell_flags(s.atlas_slot));
		if (!shell_pages.empty()) island_shell_pages_[s.atlas_slot] = std::move(shell_pages);
		std::vector<int> mesh_pages = upload(s, s.opaque, ve::island_mesh_flags(s.atlas_slot));
		if (!mesh_pages.empty()) island_mesh_pages_[s.atlas_slot] = std::move(mesh_pages);
	}
}
```

In `LodSystem::stats()`, after the line that sums `island_shell_pages_` into `owned_pages`:

```cpp
	for (const auto &kv : island_mesh_pages_) owned_pages += kv.second.size();
```

In `prepare_raster_locked`, before `render()->passes().lod_raster->set_draw_pages(pages);`:

```cpp
	// Raster mode draws islands from their opaque meshes (spec 2026-10-04 §5): local-space
	// pages lod.vert.glsl places through the island descriptor. Never part of the walk's cut.
	if (render()->raster_mode())
		for (const auto &entry : island_mesh_pages_)
			for (int p : entry.second) {
				const auto q = lod_page_quads_.find(p);
				if (q != lod_page_quads_.end())
					pages.push_back(LodRasterPass::PageDraw{p, q->second});
			}
```

- [ ] **Step 8: Place island pages in the LoD vertex shader**

Create `shaders/island_xform.glslh`:

```glsl
// Places an island page's LOCAL-space vertex in the world. `island` is the chunk record's
// flags >> 8: atlas slot + 1, and 0 for a world-space page, which is left untouched. The
// descriptor is eight vec4 per island, as raymarch.comp.glsl reads it: basis rows 0-2 with
// the translation in .w. The includer declares the island_desc buffer.
void island_place(uint island, inout vec3 p, inout vec3 n) {
	if (island == 0u) return;
	int i = int(island) - 1;
	vec4 r0 = island_desc.v[i * 8 + 0];
	vec4 r1 = island_desc.v[i * 8 + 1];
	vec4 r2 = island_desc.v[i * 8 + 2];
	mat3 basis = mat3(r0.xyz, r1.xyz, r2.xyz);
	p = basis * p + vec3(r0.w, r1.w, r2.w);
	n = basis * n;
}
```

`shaders/shell.vert.glsl`: on the line after the `IslandDesc` buffer declaration (binding 8), add `#include "island_xform.glslh"`. Replace the block:

```glsl
	if (island != 0u) {
		int i = int(island) - 1;
		vec4 r0 = island_desc.v[i * 8 + 0];
		vec4 r1 = island_desc.v[i * 8 + 1];
		vec4 r2 = island_desc.v[i * 8 + 2];
		mat3 basis = mat3(r0.xyz, r1.xyz, r2.xyz);
		p = basis * p + vec3(r0.w, r1.w, r2.w);
		n = basis * n;
	}
```

with:

```glsl
	island_place(island, p, n);
```

`shaders/lod.vert.glsl`: after the `Normals` buffer declaration, add:

```glsl
// Island pages only (raster mode, spec 2026-10-04 §5). LodRasterPass binds a dead
// descriptor when no island atlas exists.
layout(set = 0, binding = 8, std430) readonly buffer IslandDesc { vec4 v[]; } island_desc;
#include "island_xform.glslh"
```

In `main`, replace the lines from `v_wpos = corner == 0u ? …` through `v_normal = oct_decode_snorm8(packed_normal);` with:

```glsl
	vec3 wpos = corner == 0u ? p0 : (corner == 1u ? p1 : (corner == 2u ? p2 : p3));
	uint normal_pair = normals.v[quad * 2u + (corner >> 1u)];
	uint packed_normal = (normal_pair >> ((corner & 1u) * 16u)) & 0xFFFFu;
	vec3 nrm = oct_decode_snorm8(packed_normal);
	// An island page holds LOCAL-space quads; its flags (bits 8..) name the island. Terrain
	// pages carry 0 there and are untouched.
	island_place(floatBitsToUint(chunks.v[ci * 2u + 1u].y) >> 8, wpos, nrm);
	v_wpos = wpos;
	v_normal = nrm;
```

Change the last line of `main` to `gl_Position = pc.view_proj * vec4(wpos, 1.0);`.

- [ ] **Step 9: Bind the descriptor in the LoD raster pass**

`extension/src/render/lod_raster_pass.h`, in `public:` after `set_skip_transparent`:

```cpp
	// The island descriptor buffer lod.vert.glsl places island pages through. Unset (debug
	// probes) binds a dead descriptor of the pass's own.
	void set_island_desc(RID buffer) { island_desc_ = buffer; }
```

In `private:`, after `RID index_array_, index_array_buffer_;`:

```cpp
	RID island_desc_, fallback_desc_;
```

`extension/src/render/lod_raster_pass.cpp`. At the top, add `#include <godot_cpp/variant/packed_byte_array.hpp>`. In `initialize`, after `shader_marker_ = …;`:

```cpp
	PackedByteArray dead;
	dead.resize(128); // one island descriptor, eight vec4, all zero
	dead.fill(0);
	fallback_desc_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(128, dead));
```

In `teardown`, after `index_array_ = index_array_buffer_ = RID();`:

```cpp
	island_desc_ = fallback_desc_ = RID();
```

In `ensure_uniform_set`, add after the `gpu::storage(5, pool.normal_buffer())` entry (the preceding entry gains a trailing comma):

```cpp
			gpu::storage(8, island_desc_.is_valid() ? island_desc_ : fallback_desc_)
```

`extension/src/render/frame.cpp`, after `if (lod_raster) lod_raster->set_skip_transparent(transparency.enabled);`:

```cpp
	if (lod_raster && render_.passes().islands)
		lod_raster->set_island_desc(render_.passes().islands->desc_buffer());
```

- [ ] **Step 10: Run the raster and island suites**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_raster_mode.gd -a res://tests/test_island_render.gd -a res://tests/test_transparency.gd -a res://tests/test_lod_raster_golden.gd`
Expected: native `SUCCESS`; gdUnit PASS, or failing only where Task 0's baseline already failed.

- [ ] **Step 11: Commit**

```bash
git add shaders/island_xform.glslh shaders/lod.vert.glsl shaders/shell.vert.glsl \
	extension/src/transparency/shell_grid.h extension/src/transparency/shell_grid.cpp \
	extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp \
	extension/src/debug/hooks_physics.cpp extension/src/lod/lod_system.h extension/src/lod/lod_system.cpp \
	extension/src/render/lod_raster_pass.h extension/src/render/lod_raster_pass.cpp \
	extension/src/render/frame.cpp extension/tests/test_shell_grid.cpp tests/test_raster_mode.gd
git commit -m "feat: raster mode draws islands from opaque meshes contoured at extraction"
```

---

### Task 7: Full regression, measurement, and the record

**Files:**
- Modify: `docs/superpowers/specs/2026-10-04-raster-mode-design.md`

- [ ] **Step 1: Full suites**

Run: `./build.sh --test && ./gdunit_tests.sh 2>&1 | tee "$TMPDIR/raster-final.txt" | tail -40`
Expected: native `SUCCESS`. The set of failing gdUnit suites is a subset of Task 0's baseline list. Any new failure is a regression; fix it before continuing.

- [ ] **Step 2: Interleaved A/B/A frame time**

Run each in order, one at a time (two Godot processes on one GPU measure each other):

```bash
for run in A1 B1 A2 B2 A3; do
	extra=""; case "$run" in B*) extra="--effects-off=raymarch";; esac
	for leg in --benchmark --benchmark-move; do
		godot --path . --resolution 2560x1440 --disable-vsync demo/main.tscn -- "$leg" $extra 2>&1 \
			| grep "BENCH p50\|BENCH lod_summary\|BENCH mode" | tee -a "$TMPDIR/raster-aba.txt"
	done
done
```

Expected: five steady and five move results. Tabulate p50, p99 and `pages_used_p50/p99` per run. The A runs bound the noise; a B difference inside the A/A spread is not a measured separation, and the record says so.

- [ ] **Step 3: Acne capture**

```bash
godot --path . --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark --screenshot="$TMPDIR/raymarch.png"
godot --path . --resolution 2560x1440 --disable-vsync demo/main.tscn -- --benchmark --effects-off=raymarch --screenshot="$TMPDIR/raster.png"
```

Open both images with the Read tool. Look for stripes or speckle of shadow on sunlit ground near the camera in `raster.png`. Note what is seen. If acne is present, record it as a known ceiling with the spec's proposed fix (a normal-offset bias on the sun-map lookup in raster mode). Do not fix it here.

- [ ] **Step 4: Record deviations and measurements in the spec**

In `docs/superpowers/specs/2026-10-04-raster-mode-design.md`:

- Change `**Status:** design approved, not yet planned` to `**Status:** implemented`.
- In §3, replace the paragraph starting `` `kLodBaseCell` becomes 0.1 m `` and the two tables after it with a short description of the negative levels: `kLodMinLevel = -2`, `LodTreeConfig::min_level` 0 or −2, near-dense only above level 0, and no other constant changed.
- Append a §8 "Deviations and measurements" section containing:
  - this plan's eight deviations, verbatim;
  - the A/B/A table from Step 2 with the A/A spread stated;
  - peak `pages_used` in raster mode against the 32768 budget;
  - what the acne capture showed.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-10-04-raster-mode-design.md
git commit -m "docs: raster mode deviations, measured cost and page use"
```
