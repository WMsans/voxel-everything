# Transparent Voxels (Meshed) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ice (and any later transparent material row) is see-through at any thickness, in the near field, the far field and on islands, without the raymarcher ever stepping through it.

**Architecture:** Every lattice the marcher reads is baked through one rule, `opaque_view`, which turns a transparent solid sample into "just outside"; the marcher therefore sees a world in which ice is air and is otherwise untouched. The ice itself is a shell of surface-nets quads (air↔ice faces only) built by the existing LoD builder: the far-field shell is ported from the prior branch, a new fixed-level grid of 3.2 m chunks at 0.1 m cells covers the near field, and island shells are contoured once on the CPU. Two raster passes over the shell measure exact ice thickness (additive front/back accumulation clamped by opaque depth) and write the nearest front; a compute pass after `deferred` tints the lit image, and a fullscreen pass resolves the front into the G-buffer so outlines, SSR and contact shadows see it.

**Tech Stack:** C++20 GDExtension (godot-cpp, Godot 4.7 RenderingDevice), GLSL 460 compute and raster shaders, doctest (native), gdUnit4 (GPU).

**Spec:** `docs/superpowers/specs/2026-10-01-transparent-voxels-design.md`

**Prior branch:** `feat/transparent-materials` (worktree `.worktrees/transparent-materials`). Several tasks cherry-pick from it. Its own plan, `docs/superpowers/plans/2026-09-29-transparent-materials.md` on that branch, is the line-by-line reference for every cherry-picked commit.

## Global Constraints

- No `Co-Authored-By` or other AI attribution lines in any commit message. Plain conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`).
- Work only in `.worktrees/transparent-meshed` on branch `feat/transparent-meshed`. Never modify `.worktrees/transparent-materials`.
- Transparency is generic: no shader, pass or C++ branch names "ice". Everything keys off `transmit` / `mat_transparent` / `ve::material_transparent`.
- `field_ops.glslh`, `ve::apply_op`, `BeautySettings`, `shade.glslh`, `outline.comp.glsl` and `ssr.comp.glsl` are not edited. The only `deferred.comp.glsl` edit is the verbatim move in cherry-pick `bd035a5`.
- Scenes with no transparent material render bit-for-bit as before, with `enabled` on and off. The existing goldens (`tests/test_*_golden.gd`) are the pin. `tests/test_frame_shipped_golden.gd` is known flaky and is informational only; never re-record it.
- Generated files are never hand-edited. `shaders/generated/*` are regenerated with `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests`. `shaders/material_table.glslh` is replaced with the text its failing test prints.
- Every new `.glsl` file gets its Godot `.import` sidecar committed once a test run has created it. Never put a literal `#include` directive inside a comment in a `.glslh` file (the loader matches include tokens anywhere in a line).
- Tabs for indentation in C++, GLSL and GDScript.
- Build: `./build.sh`. Native tests: `cd extension && scons -Q test` (single case: `cd extension && ./build/tests/ve_tests -tc="<name>"`). GPU tests: `./gdunit_tests.sh -a res://tests/<file>.gd`.
- gdUnit has a standing, drifting set of failures on clean `main`. Task 0 records it; every later "no new failures" check compares against that list, not against zero.
- GPU pass timings are invalid on this machine (`valid_samples=0`). Cost is measured as interleaved A/B/A frame time only.
- Settings defaults, verbatim from the spec: `enabled = true`, `min_transmit = 0.35`, `sky_thickness_m = 4.0`. Ice `transmit {0.80, 0.90, 0.95}`, `ior 1.31`. Ice_crack `transmit {0.55, 0.65, 0.70}`, `ior 1.31`. All others `{0,0,0}`, `ior 1.0`.
- Near shell grid: 32 cells of `0.1` m, `3.2` m per chunk.
- gdUnit transparency tests render through `debug_render_frame` (the shipping `VoxelFrame`), never through a hook that rebuilds render inputs. Lighting tests stream at `(20, 60, 30)`.

## Deviations From The Spec (decided while planning; Task 12 records them in the spec)

1. **Brick residency and occupancy.** The spec's §3 table only names the atlas lattice. Two more consumers read it and are handled explicitly: a brick is resident when the union probe **or** the opaque-view probe finds a surface (otherwise ground under ice is never generated), and the occupancy grid is classified from the **union** lattice (otherwise connectivity would treat ice as air and ice could never hold anything up or become an island).
2. **Near-field normal rule.** `terrain_source_normal` falls back to the stored lattice only when the union field at the hit is more than 2 cm inside solid **and** a transparent solid sits at the hit or one voxel out along the fallback normal. The second clause is what keeps no-ice scenes bit-identical.
3. **Island gating lives in the island descriptor.** A descriptor int lane says "apply the opaque view to this island". It is set only for islands whose volume holds a transparent label, so islands without ice pay nothing.
4. **Island shells are ordinary LoD pool pages.** Their chunk record carries the island slot, and the shell vertex shader applies the island's transform from the descriptor buffer. No per-island draw call.
5. **The G-buffer resolve is a fullscreen raster pass**, not part of the compute composite: a compute shader cannot write a depth attachment. Resolved gloss is a constant `0.9`.
6. **Camera inside a transparent solid.** Pixels with thickness but no front face get tint only (no Fresnel, flat body colour), using the material the CPU sampled at the camera.
7. **Shell preconditions.** The near and far shells need the LoD pool and the `MeshService`, exactly as the far field does. The pre-existing lazy-init gap in `frame.cpp` (`lod_.pool()` gate) is not fixed here. The demo's HUD, benchmark and capture tool already create the pool every frame.
8. **Stage names** `shell` and `transparency` are appended after `history` in `FrameStage`, so existing stage bit positions do not move.
9. **`sky_thickness_m` is measured from the nearest front.** An unmatched front against the sky exits at `front distance + sky_thickness_m`.

Known ceilings added by these decisions:

- On a re-stream (not on a fresh edit, which regenerates every touched brick), a transparent intrusion narrower than 0.4 m into an otherwise fully solid brick can be missed by the 3³ residency probe, leaving a small hole in the ground seen through the ice.
- Candidate detection rescans the material bytes of every nearby override brick each time candidates are recomputed.

## Review Focus

1. **Camera inside ice.** The thickness target has back faces and no front at most pixels. Expect a tinted view of what is behind, finite, not black. Test in Task 9.
2. **Sky behind ice.** A floating ice ball against the sky has matched front/back pairs and no opaque depth. Expect tinted sky, not a hole and not untinted sky. Test in Task 9.
3. **Painted ice.** `OP_SPHERE_PAINT` relabels ground in place: no new union surface, only a label boundary. Expect the bowl under the lens to be marched and visible, and the shell to cover the lens top. Test in Task 4 (G-buffer) and Task 9 (look).
4. **Removing ice.** A subtract through ice must drop near-shell pages and far-shell pages. Expect `shell_pages` back to 0 and no leaked LoD pool pages. Test in Task 7.
5. **Ice stays solid for connectivity.** A brick that is solid only because of ice must still classify as solid in the occupancy grid, or anything resting on ice would be labelled unsupported and ice could never be part of an island. Occupancy comes from the union lattice. Test in Task 3 (native, `cell_state_field`), with `tests/test_connectivity.gd` re-run there as the GPU pin.

---

## File Structure

**Create:**
- `extension/src/world/opaque_view.h` — `ve::opaque_view`, the one rule (header-only, pure).
- `shaders/opaque_view.glslh` — its GLSL mirror.
- `extension/src/transparency/shell_grid.h`, `shell_grid.cpp` — near-shell grid constants, `ve::shell_candidates`, `ve::ShellGrid` (chunk state machine), `ve::island_shell_blocks`, `ve::shell_thickness`. Pure, in the native test build.
- `extension/tests/test_opaque_view.cpp`, `extension/tests/test_shell_grid.cpp`.
- `extension/src/render/shell_raster_pass.h`, `.cpp` — owns the thickness, front and front-depth targets; draws the thickness pass and the front pass; draws the G-buffer resolve.
- `shaders/shell.vert.glsl`, `shaders/shell_thickness.frag.glsl`, `shaders/shell_front.frag.glsl`, `shaders/shell_resolve.frag.glsl`.
- `extension/src/render/transparency_composite_pass.h`, `.cpp`, `shaders/transparency_composite.comp.glsl` — adapted from the prior branch.
- `tests/test_transparency.gd` — the GPU suite.
- `tools/transparency_capture.gd` — rendered-frame seam check (adapted from the prior branch).

**Modify:**
- Via cherry-pick (Tasks 1–2): `material_table.*`, `transparency/transparency_settings*`, `lod_reduce.*`, `lod_contour.*`, `lod_build_pass.*`, `lod_*.comp.glsl`, `lod.vert.glsl`, `lod_shadow.vert.glsl`, `lod_raster_pass.*`, `lod_system.*`, `deferred.comp.glsl`, `sun_map.glslh`, `hooks_lod.cpp`, `SConstruct`, `orchestrator.*`, `voxel_world.*`.
- `extension/src/voxel_settings.h`, `.cpp`, `tests/test_voxel_settings.gd` — the `transparency` panel tab.
- `extension/src/world/brick_eval.h`, `.cpp`; `shaders/brick_gen.comp.glsl`, `shaders/brick_mark.comp.glsl`; `extension/src/render/brick_gen_pass.cpp`, `extension/src/render/region_pass.cpp`; `extension/src/debug/hooks_world.cpp` — the opaque view in the atlas.
- `shaders/raymarch.comp.glsl` — the normal fallback under ice; the island per-sample rule.
- `extension/src/render/lod_build_pass.*`, `extension/src/render/lod_pool.*`, `extension/src/lod/lod_system.*` — shell-only builds, `upload_at`, the near-shell grid, island shell pages, the combined shell draw list.
- `extension/src/render/gpu/gpu.h`, `gpu.cpp` — `RasterState::additive`.
- `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh`, `extension/tests/test_gpu_layout.cpp` — two new push blocks.
- `extension/src/render/frame.h`, `frame.cpp`, `extension/src/render/orchestrator.h`, `.cpp`, `extension/src/render/deferred_pass.h` — stages and wiring.
- `extension/src/generator/volume_set.h`, `.cpp`, `extension/src/render/island_atlas.*`, `extension/src/render/island_handoff.h`, `extension/src/physics/island_manager.cpp` — islands.
- `extension/src/debug/hooks.cpp` — centre-pixel readouts.
- `demo/benchmark.gd` — `--transparency=` and `--ice=`.

---

### Task 0: Baseline

**Files:** none changed.

- [ ] **Step 1: Confirm the workspace**

Run: `git -C .worktrees/transparent-meshed status -sb | head -1`
Expected: `## feat/transparent-meshed`. All later commands run from `.worktrees/transparent-meshed`.

- [ ] **Step 2: Native tests green**

Run: `./build.sh --test`
Expected: build OK, doctest summary `Status: SUCCESS!`.

- [ ] **Step 3: Record the gdUnit baseline**

Run: `./gdunit_tests.sh 2>&1 | tee "$TMPDIR/transparency-baseline.txt" | tail -40`
Expected: a summary listing the failing suites. Save the list of failing suite names in the task notes. Every later "no new failures" check compares against this list.

---

### Task 1: Material columns and the settings module

**Files:**
- Cherry-pick: `b02c407` (material columns), `07122ab` (settings module).
- Modify: `extension/src/transparency/transparency_settings.h`, `transparency_settings.cpp`
- Modify: `extension/tests/test_transparency_settings.cpp`
- Modify: `extension/src/voxel_settings.h`, `extension/src/voxel_settings.cpp`, `tests/test_voxel_settings.gd`
- Modify: `extension/tests/test_material_table.cpp`

**Interfaces:**
- Produces (C++, `ve`): `bool material_transparent(uint16_t id)`, `void material_transmit(uint16_t id, float out[3])`, `float material_ior(uint16_t id)`.
- Produces (GLSL, everywhere `common.glslh` is included): `bool mat_transparent(uint id)`, `vec3 mat_transmit(uint id)`, `float mat_ior(uint id)`.
- Produces: `ve::TransparencySettings { bool enabled = true; float min_transmit = 0.35f; float sky_thickness_m = 4.0f; }`, `ve::TransparencySettingsStore`, `RenderOrchestrator::transparency_settings()`, `set_transparency_value(const char*, float)`, `transparency_value(const char*)`, settings group `"transparency"`, GDScript `VoxelWorld.set_transparency_value(name, value)` / `get_transparency_value(name)`.

- [ ] **Step 1: Cherry-pick the two commits**

```bash
git cherry-pick b02c407 07122ab
```

Expected: both apply cleanly (verified against `4211732`).

- [ ] **Step 2: Rewrite the settings tests for the new knobs (failing)**

Replace the three test cases in `extension/tests/test_transparency_settings.cpp` with:

```cpp
#include <doctest/doctest.h>
#include "transparency/transparency_settings.h"
#include "transparency/transparency_settings_store.h"
#include <cmath>

TEST_CASE("transparency defaults are the spec's and sit inside their own clamp") {
	ve::TransparencySettings s;
	CHECK(s.enabled);
	CHECK(s.min_transmit == doctest::Approx(0.35f));
	CHECK(s.sky_thickness_m == doctest::Approx(4.0f));
	ve::TransparencySettings c = s;
	ve::clamp_transparency_settings(&c);
	CHECK(c.min_transmit == doctest::Approx(s.min_transmit));
	CHECK(c.sky_thickness_m == doctest::Approx(s.sky_thickness_m));
}

// A floor of 1 would make every transparent material invisible glass; a NaN must not
// reach the shader.
TEST_CASE("transparency clamp keeps the floor a floor") {
	ve::TransparencySettings s;
	s.min_transmit = 0.0f / 0.0f;
	s.sky_thickness_m = -3.0f;
	ve::clamp_transparency_settings(&s);
	CHECK(s.min_transmit >= 0.0f);
	CHECK(s.min_transmit <= 0.95f);
	CHECK(s.sky_thickness_m >= 0.0f);
	s.min_transmit = 7.0f;
	ve::clamp_transparency_settings(&s);
	CHECK(s.min_transmit <= 0.95f);
}

TEST_CASE("the transparency store round-trips every knob and clamps on the way in") {
	ve::TransparencySettingsStore store;
	CHECK(store.set_value("enabled", 0.0f));
	CHECK(store.get().enabled == false);
	CHECK(store.set_value("min_transmit", 0.5f));
	CHECK(store.value("min_transmit") == doctest::Approx(0.5f));
	CHECK(store.set_value("sky_thickness_m", 1e9f));
	CHECK(store.value("sky_thickness_m") <= 50.0f);
	CHECK_FALSE(store.set_value("min_step_m", 1.0f)); // the walker's knobs are gone
	CHECK_FALSE(store.set_value("max_steps", 1.0f));
}
```

In `extension/tests/test_material_table.cpp`, delete the comment above `"every transmit is in [0, 1) and every ior at least 1"` that mentions the walk's cutoff and replace it with:

```cpp
// A transmit of 1 would never tint however thick the material: thickness would be invisible.
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cd extension && scons -Q test`
Expected: compile error, `'struct ve::TransparencySettings' has no member named 'sky_thickness_m'`.

- [ ] **Step 4: Replace the settings struct and rows**

In `extension/src/transparency/transparency_settings.h`, replace the struct body and its comments:

```cpp
// Transparent-material knobs (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md
// §8). DELIBERATELY its own module and store, like grass and leaves: nothing here joins
// BeautySettings. What a material lets through is a material-table column, not a knob here.
struct TransparencySettings {
	// Read where lattices are BAKED (brick generation, island upload) and where shells are
	// built, so it takes effect for data produced afterwards: a startup and benchmark A/B
	// switch, not a live toggle. Off: opaque_view is the identity, no shell is built or
	// drawn, and transparent materials render opaque.
	bool enabled = true;
	// The transmittance floor, per channel: however thick the medium, at least this much of
	// what is behind shows through.
	float min_transmit = 0.35f;
	// Thickness assumed past the nearest front when a ray enters the medium, finds no exit
	// face and has only sky behind it (a shell cut off at a streaming edge).
	float sky_thickness_m = 4.0f;
};
```

In `extension/src/transparency/transparency_settings.cpp`, replace the rows table:

```cpp
const SettingRow<TransparencySettings> kTransparencyRows[] = {
	bool_row("enabled", "Transparency", &TransparencySettings::enabled),
	// Capped below 1: at 1 the medium never tints and thickness is invisible.
	float_row("min_transmit", "Min transmit", &TransparencySettings::min_transmit, 0.0f, 0.95f,
			0.0f, 0.95f, 0.01f),
	float_row("sky_thickness_m", "Sky thickness (m)", &TransparencySettings::sky_thickness_m,
			0.0f, 50.0f, 0.0f, 20.0f, 0.1f),
};
```

- [ ] **Step 5: Give the settings a stand-in and a panel tab**

`extension/src/voxel_settings.h`: add `#include "transparency/transparency_settings_store.h"` after the beauty store include; add `mutable ve::TransparencySettingsStore transparency_stand_in_;` after `leaf_stand_in_`; in the header comment change `grass, leaves (RenderOrchestrator's` to `grass, leaves, transparency (RenderOrchestrator's`.

`extension/src/voxel_settings.cpp`:

```cpp
constexpr const char *kGroups[] = {"display", "render", "beauty", "grass", "leaves", "transparency"};
```

In `VoxelSettings::group`, after the `"leaves"` line:

```cpp
	if (name == "transparency") return &transparency_stand_in_;
```

In `_ready`, extend the `stand_ins` array:

```cpp
			{"leaves", &leaf_stand_in_}, {"transparency", &transparency_stand_in_}};
```

`tests/test_voxel_settings.gd`, in `test_groups_are_listed_in_panel_order`:

```gdscript
	assert_array(Array(settings.groups())).is_equal(
		["display", "render", "beauty", "grass", "leaves", "transparency"])
```

- [ ] **Step 6: Run native tests and build**

Run: `cd extension && scons -Q test && cd .. && ./build.sh`
Expected: `Status: SUCCESS!`, then `Build OK`.

- [ ] **Step 7: Run the settings GPU suite**

Run: `./gdunit_tests.sh -a res://tests/test_voxel_settings.gd`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add -A extension/src/transparency extension/tests/test_transparency_settings.cpp \
	extension/tests/test_material_table.cpp extension/src/voxel_settings.h \
	extension/src/voxel_settings.cpp tests/test_voxel_settings.gd
git commit -m "feat: transparency settings carry the floor and sky thickness; panel tab"
```

---

### Task 2: Port the far-field shell build and the opaque far lattice

**Files:**
- Cherry-pick: `caa2a5c`, `fb2169d`, `d75c0fb`, `744b7e3`, `aa53882` (one conflict), `bd035a5`.
- Modify (conflict): `extension/src/render/frame.cpp`

**Interfaces:**
- Produces (`ve`): `bool lod_has_transparent(const uint8_t *lattice, const uint16_t *material)`, `uint8_t lod_outside_byte(float cell_size)`, `void lod_opaque_lattice(const uint8_t *lattice, const uint16_t *material, float cell_size, uint8_t *out)`, `void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out, bool shell_only = false)`, `bool lod_append_shell(std::vector<LodQuad>*, std::vector<LodQuadNormals>*, const std::vector<LodQuad> &shell, const std::vector<LodQuadNormals> &shell_normals)`, `bool lod_quads_have_transparent(const LodQuad *quads, int count)`.
- Produces: `LodBuildPass` counts buffer is 4 uints per job (`[0]` quad count, `[1]` overflow, `[2]` shell quad count, `[3]` flags: bit 0 has-transparent, bit 1 shell overflow); `LodBuildPush.params.w` is the mode (0 terrain, 1 shell); `LodBuildResult::quads` holds opaque quads, skirts, then shell quads.
- Produces: `LodRasterPass::set_skip_transparent(bool)`, `LodSystem::transparent_draw_pages() const -> std::vector<ve::LodPageDraw>`, GLSL `shaders/sun_map.glslh` (`sun_cascade_of`, `sun_map_visibility`).

- [ ] **Step 1: Cherry-pick the clean commits**

```bash
git cherry-pick caa2a5c fb2169d d75c0fb 744b7e3
```

Expected: all four apply cleanly.

- [ ] **Step 2: Cherry-pick `aa53882` and resolve its one conflict**

```bash
git cherry-pick aa53882
```

Expected: `CONFLICT (content): Merge conflict in extension/src/render/frame.cpp`. The conflict is the hunk that replaced the walker's `rmp->set_transparency(render_.transparency_settings());` line, which this branch never had. Resolve it so the region reads exactly:

```cpp
	const int islands = render_.island_slot_count();
	// One read per frame, shared by everything transparency touches this frame: reading it
	// twice invites one consumer to be stale.
	const ve::TransparencySettings transparency = render_.transparency_settings();
	IslandCullPass *cull = render_.passes().island_cull;
```

and the lines before `rmp->render(` read exactly as on `main` (no `set_transparency` call):

```cpp
	if (rmp->targets_need_rebuild(rw, rh, effective_mask)) cmp->release_targets();
	if (!rmp->render(rd, *atlas, render_.passes().islands, mask, cp, rw, rh, edit_state,
			render_.passes().field_context)) {
```

Keep the cherry-pick's other `frame.cpp` hunk (`if (lod_raster) lod_raster->set_skip_transparent(transparency.enabled);`). Then:

```bash
git add extension/src/render/frame.cpp
git cherry-pick --continue --no-edit
```

- [ ] **Step 3: Cherry-pick the sun-map move**

```bash
git cherry-pick bd035a5
```

Expected: clean.

- [ ] **Step 4: Build and run native tests**

Run: `./build.sh --test`
Expected: `Build OK`, `Status: SUCCESS!` (includes `test_lod_transparency.cpp`).

- [ ] **Step 5: Run the LoD pins**

Run: `./gdunit_tests.sh -a res://tests/test_lod_mesh_diff.gd -a res://tests/test_lod_raster_golden.gd -a res://tests/test_lod_cull_golden.gd -a res://tests/test_deferred_golden.gd`
Expected: PASS for all four (none is in the Task 0 failing list; if one is, it must fail the same way as in the baseline).

- [ ] **Step 6: Commit (only if Step 2 left anything unstaged)**

`git status --short` must be empty apart from new `.import` sidecars. If sidecars appeared:

```bash
git add shaders/*.import
git commit -m "chore: import sidecars for the ported LoD shell shaders"
```

---

### Task 3: The opaque view in the brick atlas

**Files:**
- Create: `extension/src/world/opaque_view.h`, `shaders/opaque_view.glslh`
- Create: `extension/tests/test_opaque_view.cpp`
- Modify: `extension/src/world/brick_eval.h`, `extension/src/world/brick_eval.cpp`
- Modify: `shaders/brick_gen.comp.glsl`, `shaders/brick_mark.comp.glsl`
- Modify: `extension/src/render/brick_gen_pass.cpp` (line 59, the push), `extension/src/render/region_pass.cpp` (line 75, the push)
- Modify: `extension/src/debug/hooks_world.cpp` (lines 874 and 1021, the two `ve::eval_brick` calls)
- Test: `extension/tests/test_opaque_view.cpp`, `tests/test_brick_diff.gd`

**Interfaces:**
- Consumes: `ve::material_transparent` (Task 1), GLSL `mat_transparent`.
- Produces (C++): `ve::kOpaqueOutside` (`0.5f * kVoxelSize`), `inline void ve::opaque_view(float *sdf, uint16_t *material, float outside = kOpaqueOutside)`.
- Produces (GLSL): `void opaque_view(inout float sdf, inout uint mat, float outside)`.
- Produces: `ve::eval_brick(..., const OverrideSource *overrides = nullptr, bool opaque = true)`; `ve::brick_has_surface(..., const OverrideSource *overrides = nullptr, bool opaque = true)`. `cell_state_field` and `cell_state_probe` keep classifying the **union** field.
- Produces: `BrickGenPush.atlas_bricks[3]` and `BrickMarkPush.hi[3]` are `1` when the opaque view is on. `BrickGenPass` and `RegionPass` gain `void set_opaque_view(bool on)`.

- [ ] **Step 1: Write the failing native tests**

Create `extension/tests/test_opaque_view.cpp`:

```cpp
#include <doctest/doctest.h>
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "world/material_table.h"
#include "world/opaque_view.h"

TEST_CASE("opaque_view turns a transparent solid into just-outside and nothing else") {
	const uint16_t ice = ve::material_id("ice");
	const uint16_t rock = ve::material_id("rock");
	float d = -0.3f;
	uint16_t m = ice;
	ve::opaque_view(&d, &m);
	CHECK(d == doctest::Approx(ve::kOpaqueOutside));
	CHECK(d > 0.0f);
	CHECK(m == 0);

	d = -0.3f; m = rock;
	ve::opaque_view(&d, &m);
	CHECK(d == doctest::Approx(-0.3f));
	CHECK(m == rock);

	// Air that merely carries a transparent label (a projected material) is untouched.
	d = 0.2f; m = ice;
	ve::opaque_view(&d, &m);
	CHECK(d == doctest::Approx(0.2f));
	CHECK(m == ice);
}

namespace {

// The brick holding the default world's surface under (20, *, 30): the first brick, walking
// down from y = 80 m, that the union probe calls a surface brick.
ve::IVec3 surface_brick(const ve::Generator &gen) {
	for (int by = 100; by > 0; by--) {
		const ve::IVec3 b{25, by, 37};
		if (ve::brick_has_surface(gen, nullptr, 0, b, nullptr, nullptr, false)) return b;
	}
	return ve::IVec3{25, 64, 37};
}

ve::EditOp sphere(uint32_t type, uint16_t material, const float c[3], float r) {
	ve::EditOp op{};
	op.type = type;
	op.material = material;
	op.pos[0] = c[0]; op.pos[1] = c[1]; op.pos[2] = c[2];
	op.radius = r;
	return op;
}

} // namespace

TEST_CASE("a brick buried in an ice ball has no opaque surface but is still solid for occupancy") {
	ve::Generator gen;
	const ve::IVec3 b = surface_brick(gen);
	float bo[3];
	ve::brick_world_origin(b, bo);
	// A 3 m ice ball centred 2 m above the brick: the brick's air half is now ice.
	const float c[3] = {bo[0] + 0.4f, bo[1] + 2.0f, bo[2] + 0.4f};
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("ice"), c, 3.0f);

	ve::BrickEval opaque{}, plain{};
	ve::eval_brick(gen, &add, 1, b, &opaque, nullptr, nullptr, true);
	ve::eval_brick(gen, &add, 1, b, &plain, nullptr, nullptr, false);
	const uint8_t zero = ve::encode_sdf(0.0f);
	int opaque_out = 0, union_out = 0;
	for (int i = 0; i < ve::kBrickSdfCount; i++) {
		if (opaque.brick.sdf[i] > zero) opaque_out++;
		if (plain.brick.sdf[i] > zero) union_out++;
	}
	CHECK(union_out == 0);   // the union lattice is solid through and through
	CHECK(opaque_out > 0);   // the opaque view still has the ground's own surface in it
	CHECK(ve::cell_state_field(gen, &add, 1, b) == ve::kCellFull); // occupancy: the UNION
	// Residency: the union probe alone would drop this brick; the opaque probe keeps it.
	CHECK_FALSE(ve::brick_has_surface(gen, &add, 1, b, nullptr, nullptr, false));
	CHECK(ve::brick_has_surface(gen, &add, 1, b, nullptr, nullptr, true));
}

TEST_CASE("with no transparent material the opaque view changes nothing") {
	ve::Generator gen;
	const ve::IVec3 b = surface_brick(gen);
	ve::BrickEval opaque{}, plain{};
	ve::eval_brick(gen, nullptr, 0, b, &opaque, nullptr, nullptr, true);
	ve::eval_brick(gen, nullptr, 0, b, &plain, nullptr, nullptr, false);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(opaque.brick.sdf[i] == plain.brick.sdf[i]);
	// Brick::mat is the PACKED 2-bit index array, not one byte per cell.
	for (size_t i = 0; i < sizeof(opaque.brick.mat); i++) CHECK(opaque.brick.mat[i] == plain.brick.mat[i]);
	for (int k = 0; k < ve::kBrickPaletteSize; k++) CHECK(opaque.brick.palette[k] == plain.brick.palette[k]);
}

TEST_CASE("no palette entry of an opaque-view brick is a transparent material") {
	ve::Generator gen;
	const ve::IVec3 b = surface_brick(gen);
	float bo[3];
	ve::brick_world_origin(b, bo);
	const float c[3] = {bo[0] + 0.4f, bo[1] + 0.6f, bo[2] + 0.4f};
	const ve::EditOp paint = sphere(ve::kOpSpherePaint, ve::material_id("ice"), c, 0.5f);
	ve::BrickEval e{};
	ve::eval_brick(gen, &paint, 1, b, &e, nullptr, nullptr, true);
	for (int k = 0; k < ve::kBrickPaletteSize; k++)
		CHECK_FALSE(ve::material_transparent(e.brick.palette[k]));
}
```

If `ve::Brick`'s palette member is not named `palette`, or the op type constants are not `ve::kOpSphereAdd` / `ve::kOpSpherePaint`, use the names in `extension/src/world/brick.h` and `extension/src/generator/edit_ops.h` (the existing `test_brick_eval.cpp` uses both).

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd extension && scons -Q test`
Expected: compile error, `world/opaque_view.h: No such file or directory`.

- [ ] **Step 3: Write the rule**

Create `extension/src/world/opaque_view.h`:

```cpp
#pragma once
#include "world/brick.h"
#include "world/material_table.h"
#include <cstdint>

namespace ve {

// The OPAQUE VIEW (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §3): the
// world as the raymarcher sees it, in which a transparent material is air. One rule, applied
// wherever a lattice the marcher reads is baked: a SOLID sample whose material is
// transparent becomes "just outside" with no material. The field evaluator is never edited;
// physics, edits and connectivity keep seeing the union.
//
// Half a sample pitch, not a true distance: the label boundary has no distance field. It is
// positive, so the surface the marcher finds sits within one voxel of the true boundary,
// and far below kActivationPad, so a brick holding any such sample stays resident.
inline constexpr float kOpaqueOutside = 0.5f * kVoxelSize;

// Mirror of shaders/opaque_view.glslh.
inline void opaque_view(float *sdf, uint16_t *material, float outside = kOpaqueOutside) {
	if (*sdf <= 0.0f && material_transparent(*material)) {
		*sdf = outside;
		*material = 0;
	}
}

} // namespace ve
```

Create `shaders/opaque_view.glslh`:

```glsl
// The OPAQUE VIEW: the world as the raymarcher sees it, in which a transparent material is
// air. Mirror of ve::opaque_view (extension/src/world/opaque_view.h). Pull common.glslh in
// first: mat_transparent comes from the material table it includes.
const float OPAQUE_OUTSIDE = 0.5 * VOXEL_SIZE;

void opaque_view(inout float sdf, inout uint mat, float outside) {
	if (sdf <= 0.0 && mat_transparent(mat)) {
		sdf = outside;
		mat = 0u;
	}
}
```

- [ ] **Step 4: Apply it in the CPU brick evaluator**

`extension/src/world/brick_eval.h`: add `#include "world/opaque_view.h"`. Change two declarations (trailing parameter, defaulted so every existing caller compiles):

```cpp
// Coarse residency probe. Mirrored exactly by shaders/brick_mark.comp.glsl — a brick is
// resident iff this returns true, on both sides. With `opaque`, a brick is ALSO resident when
// the opaque view (world/opaque_view.h) has a surface in it: ground under a transparent
// material has no union surface and would otherwise never be generated.
bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		const VolumeStore *volumes = nullptr, const OverrideSource *overrides = nullptr,
		bool opaque = true);
```

```cpp
// Full brick contents at L0. This is BOTH the path WorldData walks and the CPU reference
// the GPU differential test diffs against (spec §8). `opaque` stores the opaque view; the
// occupancy classification (cell_state_field) always reads the union.
void eval_brick(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		BrickEval *out, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr, bool opaque = true);
```

`extension/src/world/brick_eval.cpp`:

Give `spread_materials` a trailing `bool opaque` parameter and replace its innermost assignment:

```cpp
				for (float over = 0.5f; over <= 2.5f && mat[i] == 0; over += 1.0f) {
					const float t = d + over * kVoxelSize;
					Sample s = eval_field(gen, ops, op_count,
							bo[0] + x * kVoxelSize - gx / len * t,
							bo[1] + y * kVoxelSize - gy / len * t,
							bo[2] + z * kVoxelSize - gz / len * t, volumes, overrides);
					// The projection can land in a transparent solid; that is not the
					// surface a ray will shade here, so keep looking deeper.
					if (opaque) opaque_view(&s.sdf, &s.material);
					mat[i] = s.material;
				}
```

Give `brick_probe` two more out-parameters for the opaque view:

```cpp
void brick_probe(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		const VolumeStore *volumes, const OverrideSource *overrides, float *mn, float *mx,
		float *omn = nullptr, float *omx = nullptr) {
	const std::vector<EditOp> kept = ops_for_brick(ops, op_count, brick);
	const EditOp *filtered = kept.data();
	const int filtered_count = static_cast<int>(kept.size());
	float bo[3];
	brick_world_origin(brick, bo);
	*mn = 1e30f;
	*mx = -1e30f;
	if (omn) *omn = 1e30f;
	if (omx) *omx = -1e30f;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				Sample s = eval_field(gen, filtered, filtered_count,
						bo[0] + sx * (kBrickVoxels / 2) * kVoxelSize,
						bo[1] + sy * (kBrickVoxels / 2) * kVoxelSize,
						bo[2] + sz * (kBrickVoxels / 2) * kVoxelSize, volumes, overrides);
				*mn = std::min(*mn, s.sdf);
				*mx = std::max(*mx, s.sdf);
				if (!omn || !omx) continue;
				opaque_view(&s.sdf, &s.material);
				*omn = std::min(*omn, s.sdf);
				*omx = std::max(*omx, s.sdf);
			}
}
```

Replace `brick_has_surface`:

```cpp
bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		const VolumeStore *volumes, const OverrideSource *overrides, bool opaque) {
	float mn = 0.0f, mx = 0.0f, omn = 0.0f, omx = 0.0f;
	brick_probe(gen, ops, op_count, brick, volumes, overrides, &mn, &mx, &omn, &omx);
	const bool union_surface = mn < kActivationPad && mx > -kActivationPad;
	const bool opaque_surface = omn < kActivationPad && omx > -kActivationPad;
	return union_surface || (opaque && opaque_surface);
}
```

In `cell_state_field`, call the union explicitly:

```cpp
	eval_brick(gen, ops, op_count, cell, &eval, volumes, overrides, false);
```

In `eval_brick`, add the parameter to the definition, and replace the lattice loop's first three statements:

```cpp
				Sample s = eval_field(gen, filtered, filtered_count, bo[0] + vx * kVoxelSize,
						bo[1] + vy * kVoxelSize, bo[2] + vz * kVoxelSize, volumes, overrides);
				if (opaque) opaque_view(&s.sdf, &s.material);
				b.sdf[sdf_index(vx, vy, vz)] = encode_sdf(s.sdf);
				if (s.material == 0) continue;
```

(remove the `const` from `const Sample s`). Pass `opaque` to `spread_materials`.

- [ ] **Step 5: Run the native tests to verify they pass**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`, including the existing `test_brick_eval.cpp` cases unchanged.

- [ ] **Step 6: Apply it in `brick_gen.comp.glsl`**

After `#include "brick_layout.glslh"` add:

```glsl
#include "opaque_view.glslh"
```

Next to `shared uint s_mip4[64];` add the union range the occupancy grid is classified from:

```glsl
// Encoded min / max of the UNION lattice. The occupancy grid is classified from these, not
// from the stored (opaque-view) lattice: connectivity must keep seeing a transparent solid
// as solid. With no transparent sample they equal the stored lattice's own range.
shared uint s_umin;
shared uint s_umax;
```

Where `s_pal` is initialised (`if (tid < 4u) { ... }`), add:

```glsl
	if (tid == 0u) { s_umin = 255u; s_umax = 0u; }
	memoryBarrierShared();
	barrier();
```

Phase 1a, replace the body after `eval_field(...)`:

```glsl
		eval_field(bo + vec3(v) * VOXEL_SIZE, op_base, s_op_n, sdf, mat);
		uint ub = encode_sdf_byte(sdf);
		atomicMin(s_umin, ub);
		atomicMax(s_umax, ub);
		if (pc.atlas_bricks.w != 0) opaque_view(sdf, mat, OPAQUE_OUTSIDE);
		imageStore(sdf_atlas, sdf_base + v, vec4(quantise_sdf(sdf)));
		s_mat[i] = mat;
```

Phase 1b, the same four lines between `eval_field(...)` and `imageStore(...)`:

```glsl
		eval_field(bo + vec3(v) * VOXEL_SIZE, op_base, s_op_n, sdf, mat);
		uint ub = encode_sdf_byte(sdf);
		atomicMin(s_umin, ub);
		atomicMax(s_umax, ub);
		if (pc.atlas_bricks.w != 0) opaque_view(sdf, mat, OPAQUE_OUTSIDE);
		imageStore(sdf_atlas, sdf_base + v, vec4(quantise_sdf(sdf)));
```

Phase 2, after the projected `eval_field(...)`:

```glsl
			eval_field(bo + vec3(v) * VOXEL_SIZE - g / len * t, op_base, s_op_n, sdf2, matB);
			if (pc.atlas_bricks.w != 0) opaque_view(sdf2, matB, OPAQUE_OUTSIDE);
			s_mat[i] = matB;
```

At the end (`if (tid == 0u) { ... }`), classify occupancy from the union range instead of `mn` / `mx`:

```glsl
		uint state = s_umin > ENCODED_ZERO ? CELL_AIR :
				(s_umax <= ENCODED_ZERO ? CELL_FULL : CELL_SOLID);
```

- [ ] **Step 7: Apply it in `brick_mark.comp.glsl`**

After `#include "field.glslh"` add `#include "opaque_view.glslh"`. Replace `brick_probe`:

```glsl
// Mirror of ve::brick_probe (extension/src/world/brick_eval.cpp): the union range, and the
// range of the opaque view of the same 27 samples.
void brick_probe(ivec3 brick, uint op_base, uint op_count, out float mn, out float mx,
		out float omn, out float omx) {
	vec3 bo = vec3(brick) * BRICK_SIZE;
	mn = 1e30;
	mx = -1e30;
	omn = 1e30;
	omx = -1e30;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				float sdf;
				uint mat;
				eval_field(bo + vec3(sx, sy, sz) * (float(BRICK_VOXELS) * 0.5 * VOXEL_SIZE),
						op_base, op_count, sdf, mat);
				mn = min(mn, sdf);
				mx = max(mx, sdf);
				opaque_view(sdf, mat, OPAQUE_OUTSIDE);
				omn = min(omn, sdf);
				omx = max(omx, sdf);
			}
}
```

In `main`, replace the probe call and `has_surface`:

```glsl
	float probe_mn, probe_mx, opaque_mn, opaque_mx;
	brick_probe(brick, op_base, s_op_n, probe_mn, probe_mx, opaque_mn, opaque_mx);
	// `active` is a GLSL reserved word (M2 errata 5); this local is has_surface. pc.hi.w is 1
	// when the opaque view is on: ground under a transparent material has no union surface.
	bool has_surface = (probe_mn < ACTIVATION_PAD && probe_mx > -ACTIVATION_PAD) ||
			(pc.hi.w != 0 && opaque_mn < ACTIVATION_PAD && opaque_mx > -ACTIVATION_PAD);
```

The occupancy fallback two lines below (`probe_mn > 0.0 ? CELL_AIR : CELL_FULL`) stays on the union range.

- [ ] **Step 8: Feed the flag from the settings**

`extension/src/render/brick_gen_pass.cpp` line 59 and its header: add a member `bool opaque_view_ = true;` and a setter `void set_opaque_view(bool on) { opaque_view_ = on; }`, and build the push as:

```cpp
	const ve::BrickGenPush push{{atlas_bricks_.x, atlas_bricks_.y, atlas_bricks_.z,
			opaque_view_ ? 1 : 0}};
```

`extension/src/render/region_pass.cpp` line 75 and its header: the same member and setter; after `ve::BrickMarkPush push{};` is filled, set `push.hi[3] = opaque_view_ ? 1 : 0;` (confirm with `grep -n "push.hi" extension/src/render/region_pass.cpp` that `hi[3]` is otherwise left 0).

`extension/src/render/frame.cpp`, immediately before `timings->begin(rd, "stream");`: move the `const ve::TransparencySettings transparency = render_.transparency_settings();` line (added in Task 2) up to here, delete it from its Task 2 position, and add:

```cpp
	if (BrickGenPass *bg = render_.passes().brick_gen) bg->set_opaque_view(transparency.enabled);
	if (RegionPass *rp = render_.passes().region) rp->set_opaque_view(transparency.enabled);
```

Use the registry's actual member names (`grep -n "BrickGenPass\|RegionPass" extension/src/render/orchestrator.h`). The debug stream path must set them too: in `extension/src/debug/hooks_world.cpp`, find `debug_stream_frame` and `debug_generate_pending` and set both passes from `world_->context().render->transparency_settings().enabled` before they dispatch.

`extension/src/debug/hooks_world.cpp` lines 874 and 1021: pass the live flag so the GPU/CPU diff compares like with like:

```cpp
	ve::eval_brick(gen, ptr, op_count, b, &ref, &world_->context().store->volumes(),
			world_->context().store->overrides(),
			world_->context().render->transparency_settings().enabled);
```

(and the same trailing argument on line 1021).

- [ ] **Step 9: Extend the GPU/CPU brick diff with an ice brick**

Append to `tests/test_brick_diff.gd` (reuse the file's existing diff helper for one brick — the function the existing `test_*` cases call to compare a brick against the CPU; pass it the bricks below):

```gdscript
func ice_id(w: VoxelWorld) -> int:
	for m in w.material_table():
		if m["name"] == "ice":
			return m["id"]
	return 0

# The opaque view (spec §3): GPU brick_gen and ve::eval_brick must agree on a region that
# holds an added ice ball and a painted ice lens, byte for byte like any other brick.
func test_bricks_holding_transparent_material_match_the_cpu_reference() -> void:
	var w := make_world()
	var origin := Vector3(REGION) * 25.6
	var ops := PackedByteArray()
	ops.append_array(make_op(1, ice_id(w), origin + Vector3(6.0, 3.0, 6.0), 2.0)) # add
	ops.append_array(make_op(2, ice_id(w), origin + Vector3(14.0, 1.0, 14.0), 1.5)) # paint
	w.hooks().debug_set_region_ops(SLOT, ops)
	generate_region(w, REGION, SLOT, 2)
	var bricks := active_bricks(w, SLOT, 24)
	assert_int(bricks.size()).is_greater(0)
	for b in bricks:
		assert_brick_matches(w, b, SLOT, 2)
```

`debug_set_region_ops` and `assert_brick_matches` stand for the two helpers the file's existing op-carrying test uses to install ops and to diff one brick; use those exact names from the file (`grep -n "^func \|debug_.*ops" tests/test_brick_diff.gd`).

- [ ] **Step 10: Build and run**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_brick_diff.gd -a res://tests/test_brick_flags_gpu.gd -a res://tests/test_connectivity.gd -a res://tests/test_composite_golden.gd`
Expected: PASS (the new case included; the three existing suites unchanged from the baseline).

- [ ] **Step 11: Commit**

```bash
git add extension/src/world/opaque_view.h shaders/opaque_view.glslh \
	extension/tests/test_opaque_view.cpp extension/src/world/brick_eval.h \
	extension/src/world/brick_eval.cpp shaders/brick_gen.comp.glsl shaders/brick_mark.comp.glsl \
	extension/src/render/brick_gen_pass.* extension/src/render/region_pass.* \
	extension/src/render/frame.cpp extension/src/debug/hooks_world.cpp tests/test_brick_diff.gd
git commit -m "feat: the brick atlas stores the opaque view; occupancy keeps the union"
```

---

### Task 4: The marcher shades the ground under ice

**Files:**
- Modify: `shaders/raymarch.comp.glsl` (`terrain_source_normal`, lines 112-124)
- Modify: `extension/src/debug/hooks.cpp` (`debug_render_frame`, after `d["lit_checksum"] = checksum;`)
- Create: `tests/test_transparency.gd`

**Interfaces:**
- Consumes: the opaque-view atlas (Task 3).
- Produces (hook): `debug_render_frame` gains `center_lit` (Color), `center_material` (int, the G-buffer material at the centre pixel, march resolution), `center_distance` (float, the marched hit distance at the centre, metres; 0 on a miss).
- Produces (GDScript fixture, reused by every later task): `make_world(enabled := true, cam := CAM)`, `settle(w, cam)`, `material_id(w, name)`, `centre_hit(w, cam, fwd)`, `frame(w, cam, fwd)`, `finite(c)`.

- [ ] **Step 1: Write the failing GPU tests**

Create `tests/test_transparency.gd`:

```gdscript
extends GdUnitTestSuite

# Transparent voxels, end to end (docs/superpowers/specs/2026-10-01-transparent-voxels-
# design.md §9). Every frame is debug_render_frame -- the shipping VoxelFrame on a local
# device -- so what is asserted is what ships, never a hook's re-implementation of it.
#
# (20, 60, 30) is the open, sunlit meadow; the camera sits above it looking almost straight
# down, so the centre pixel is lit ground.

const CAM := Vector3(20.0, 64.0, 30.0)
const FWD := Vector3(0.15, -1.0, 0.1)
const W := 64
const H := 64

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# `enabled` is read where lattices are baked, so it is set BEFORE anything streams.
func make_world(enabled := true, cam := CAM) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	w.set_transparency_value("enabled", 1.0 if enabled else 0.0)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	# Hold every temporal input still, so two frames of one view are one image.
	w.set_effect_enabled("ssgi", false)
	w.set_grass_value("wind_strength", 0.0)
	w.set_grass_value("wind_speed", 0.0)
	settle(w, cam)
	return w

func settle(w: VoxelWorld, cam := CAM) -> void:
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(cam) == 0 else 0
		if quiet >= 6:
			break

func material_id(w: VoxelWorld, name: String) -> int:
	for m in w.material_table():
		if m["name"] == name:
			return m["id"]
	return 0

# Where the centre pixel's ray first meets the world (the CPU field: ice counts as solid).
func centre_hit(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var hit: Dictionary = w.raycast(cam, fwd.normalized(), 400.0)
	assert_bool(hit["hit"]).override_failure_message("the centre ray sees nothing").is_true()
	return hit

func frame(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d: Dictionary = w.hooks().debug_render_frame(cam, fwd.normalized(), W, H)
	assert_bool(d["ok"]).override_failure_message("frame aborted: %s" % d).is_true()
	return d

func finite(c: Color) -> bool:
	return is_finite(c.r) and is_finite(c.g) and is_finite(c.b)

# --- the opaque view in the marcher (spec §3) ---------------------------------------------

func test_the_marcher_sees_the_ground_through_an_added_ice_ball() -> void:
	var w := make_world()
	var ground := centre_hit(w)
	var before := frame(w)
	w.hooks().debug_apply_sphere_add(ground["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var d := frame(w)
	assert_int(int(d["center_material"])).override_failure_message(
		"the G-buffer holds a transparent material: %s" % d).is_equal(int(before["center_material"]))
	# The ball sits ON the ground: the marched hit is still the ground, not the ball's top.
	assert_float(float(d["center_distance"])).is_equal_approx(float(before["center_distance"]), 0.15)

# The prior attempt went solid past 2.4 m of ice. Ten metres must change nothing here.
func test_ten_metres_of_ice_do_not_stop_the_marcher() -> void:
	var w := make_world()
	var ground := centre_hit(w)
	var before := frame(w)
	w.hooks().debug_apply_sphere_add(ground["pos"] + Vector3(0, 4.0, 0), 5.0, material_id(w, "ice"))
	settle(w, CAM + Vector3(0, 12.0, 0))
	var cam := CAM + Vector3(0, 12.0, 0)
	var d := frame(w, cam)
	assert_int(int(d["center_material"])).is_equal(int(before["center_material"]))
	assert_float(float(d["center_distance"])).override_failure_message(
		"the hit is not ~12 m further than before: %s" % d).is_greater(
		float(before["center_distance"]) + 10.0)

# Painted ice relabels ground in place: the surface behind it is a LABEL boundary.
func test_painted_ice_exposes_the_bowl_under_the_lens() -> void:
	var w := make_world()
	var ground := centre_hit(w)
	var before := frame(w)
	w.hooks().debug_apply_sphere_paint(ground["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var d := frame(w)
	assert_int(int(d["center_material"])).is_not_equal(material_id(w, "ice"))
	assert_int(int(d["center_material"])).is_not_equal(0)
	# The bowl's bottom is about one radius further along the ray.
	assert_float(float(d["center_distance"])).is_greater(float(before["center_distance"]) + 0.5)
	assert_bool(finite(d["center_lit"])).is_true()

func test_with_the_feature_off_ice_is_an_opaque_surface() -> void:
	var w := make_world(false)
	var ground := centre_hit(w)
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(ground["pos"], 1.0, ice)
	settle(w)
	assert_int(int(frame(w)["center_material"])).is_equal(ice)

func test_a_scene_without_transparency_is_bit_identical_with_the_feature_on_and_off() -> void:
	var on := frame(make_world(true))
	var off := frame(make_world(false))
	assert_int(int(on["lit_checksum"])).override_failure_message(
		"transparency changed a frame with no transparent material in it").is_equal(
		int(off["lit_checksum"]))
```

- [ ] **Step 2: Run to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: FAIL — `center_material` is not a key of the hook's dictionary.

- [ ] **Step 3: Add the centre-pixel readouts to the hook**

In `extension/src/debug/hooks.cpp`, `debug_render_frame`, replace `d["lit_checksum"] = checksum;` and the `return d;` after it with:

```cpp
	d["lit_checksum"] = checksum;

	// Centre-pixel readouts for tests/test_transparency.gd. Plain reads of the shipping
	// targets -- nothing is re-rendered.
	{
		const int64_t c = static_cast<int64_t>(h / 2) * w + w / 2;
		d["center_lit"] = Color(half_to_float(v[c * 4]), half_to_float(v[c * 4 + 1]),
				half_to_float(v[c * 4 + 2]));
		d["center_material"] = 0;
		d["center_distance"] = 0.0;
		RaymarchPass *rmp = world_->context().render->passes().raymarch;
		const Vector2i ms = rmp ? rmp->target_size() : Vector2i();
		if (ms.x > 0 && ms.y > 0) {
			const int64_t mc = static_cast<int64_t>(ms.y / 2) * ms.x + ms.x / 2;
			const PackedByteArray sf = device->texture_get_data(rmp->surface_texture(), 0);
			if (sf.size() >= (mc + 1) * 8)
				d["center_material"] = static_cast<int>(half_to_float(
						reinterpret_cast<const uint16_t *>(sf.ptr())[mc * 4 + 2]) + 0.5f);
			const PackedByteArray hp = device->texture_get_data(rmp->hitpos_texture(), 0);
			if (hp.size() >= (mc + 1) * 16) {
				const float *p = reinterpret_cast<const float *>(hp.ptr()) + mc * 4;
				const Vector3 hit(p[0], p[1], p[2]);
				if (p[3] > 0.0f) d["center_distance"] = hit.distance_to(pos);
			}
		}
	}
	return d;
```

`RaymarchPass::target_size()` does not exist on `main`: add it to `extension/src/render/raymarch_pass.h` beside `surface_texture()`, returning the size the pass last allocated its targets at (the two ints `targets_need_rebuild` compares against). Confirm the hit-position target's layout before relying on it: `grep -n "out_hitpos" shaders/raymarch.comp.glsl` — if it is not `rgba32f` holding world position in xyz with w > 0 on a hit, adapt the four lines that decode it to its real format and "hit" flag.

- [ ] **Step 4: Run to see the real failure**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: the five tests run. The material and distance assertions may already pass (Task 3 did the work); `center_lit` finiteness may fail or the lit colour may be visibly wrong under ice because the normal is the union field's. Proceed to Step 5 regardless.

- [ ] **Step 5: Fall back to the stored lattice for normals under a transparent solid**

In `shaders/raymarch.comp.glsl`, replace the tail of `terrain_source_normal` (from `float len = length(gradient);`):

```glsl
	float len = length(gradient);
	// The opaque view (shaders/opaque_view.glslh): under a transparent material the atlas
	// surface is a LABEL boundary, and the union field's gradient here belongs to the medium,
	// not to the surface that was hit. A hit clearly inside the union solid, with a
	// transparent solid at the hit or one voxel out along the stored-lattice normal, shades
	// with that normal instead. The second clause keeps a scene with no transparent material
	// on the analytic path bit for bit.
	if (sdf < -0.02) {
		int scratch = 6;
		vec3 n_fb = terrain_r8_fallback_normal(p, brick, anchor_slot, scratch);
		float sdf_out;
		uint mat_out;
		eval_field(p + n_fb * VOXEL_SIZE, uint(rs) * MAX_REGION_OPS,
				uint(max(op_counts.n[rs], 0)), sdf_out, mat_out);
		if (mat_transparent(mat) || (sdf_out <= 0.0 && mat_transparent(mat_out))) return n_fb;
	}
	if (exact_gradient && len > 1e-8) return gradient / len;
	return terrain_r8_fallback_normal(p, brick, anchor_slot, steps_left);
```

- [ ] **Step 6: Run the suite and the near-field pins**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd -a res://tests/test_composite_golden.gd -a res://tests/test_deferred_golden.gd -a res://tests/test_frame_contract.gd`
Expected: `test_transparency.gd` PASS (5 cases); the three pins unchanged from the baseline.

- [ ] **Step 7: Commit**

```bash
git add shaders/raymarch.comp.glsl extension/src/debug/hooks.cpp \
	extension/src/render/raymarch_pass.h tests/test_transparency.gd tests/test_transparency.gd.uid
git commit -m "feat: the marcher shades ground under a transparent material from the stored lattice"
```

---

### Task 5: Shell math, the near-shell grid and candidates (pure)

**Files:**
- Create: `extension/src/transparency/shell_grid.h`, `extension/src/transparency/shell_grid.cpp`
- Create: `extension/tests/test_shell_grid.cpp`
- Modify: `extension/src/world/override_store.h`, `extension/src/world/override_store.cpp` (one iteration method)

**Interfaces:**
- Consumes: `ve::material_transparent`, `ve::EditOp`, `ve::op_world_aabb`, `ve::VolumeSet::get`, `ve::OverrideStore`, `ve::lod_contour(..., shell_only)`, `ve::VolumeData`.
- Produces (namespace `ve`, header `transparency/shell_grid.h`):

```cpp
inline constexpr float kShellCell = 0.1f;
inline constexpr float kShellChunkSize = kShellCell * kLodChunkCells; // 3.2 m
inline constexpr int kShellLevel = -1; // LodBuildJob/Result::level of a near-shell build

void shell_chunk_origin(IVec3 c, float out[3]);
void shell_chunk_aabb(IVec3 c, float lo[3], float hi[3]);
IVec3 shell_chunk_of_point(float x, float y, float z);

struct ShellBox { float lo[3]; float hi[3]; };
// World boxes that may hold transparent solid, among `ops` (already filtered to the area
// of interest), their stored volumes, and the override bricks intersecting [lo, hi].
void transparent_boxes(const EditOp *ops, int op_count, const VolumeSet *volumes,
		const OverrideStore *overrides, const float lo[3], const float hi[3],
		std::vector<ShellBox> *out);
// Chunks overlapping any box (padded by one cell) and within `radius_m` of `cam`.
void shell_candidates(const std::vector<ShellBox> &boxes, const float cam[3], float radius_m,
		std::vector<IVec3> *out);

enum ShellState : uint8_t { kShellUnknown, kShellBuilding, kShellReady, kShellEmpty };
class ShellGrid {
public:
	// Replaces the candidate set. Chunks that left it are appended to `evicted`.
	void set_candidates(const std::vector<IVec3> &chunks, std::vector<IVec3> *evicted);
	void mark_dirty(const float lo[3], const float hi[3]);
	// Up to `max` chunks that need a build (unknown, or dirty and not building), nearest
	// `cam` first.
	void requests(const float cam[3], int max, std::vector<IVec3> *out) const;
	void note_building(IVec3 c);               // clears dirty
	void note_result(IVec3 c, bool has_quads); // Ready or Empty; a dirty chunk stays requestable
	void note_failed(IVec3 c);                 // back to Unknown
	ShellState state(IVec3 c) const;           // kShellUnknown for a chunk not in the set
	bool dirty(IVec3 c) const;
	int size() const;
	void clear();
};

struct IslandShellBlock {
	float origin_local[3]; // chunk origin in the island's LOCAL space
	std::vector<LodQuad> quads;
	std::vector<LodQuadNormals> normals;
};
bool volume_has_transparent(const VolumeData &v);
// Shell quads of an island lattice, split into 32-cell blocks. `lattice_origin` and `voxel`
// are the island descriptor's. Blocks with no quads are omitted.
void island_shell_blocks(const VolumeData &v, const float lattice_origin[3], float voxel,
		std::vector<IslandShellBlock> *out);

// CPU reference for the thickness the shell passes measure (spec §6). r = sum of back-face
// distances minus front-face distances; g = fronts minus backs; z_opaque <= 0 means sky.
float shell_thickness(float r, float g, float z_opaque, float z_front, float sky_thickness_m);
```

- Produces: `template <class F> void OverrideStore::for_each(F &&fn) const` calling `fn(IVec3 brick, const OverrideBrick &)`.

- [ ] **Step 1: Write the failing tests**

Create `extension/tests/test_shell_grid.cpp`:

```cpp
#include <doctest/doctest.h>
#include "generator/volume_set.h"
#include "transparency/shell_grid.h"
#include "world/brick.h"
#include "world/material_table.h"
#include "world/override_store.h"
#include <algorithm>

namespace {
bool has(const std::vector<ve::IVec3> &v, ve::IVec3 c) {
	return std::any_of(v.begin(), v.end(), [c](ve::IVec3 o) {
		return o.x == c.x && o.y == c.y && o.z == c.z; });
}
ve::EditOp sphere(uint32_t type, uint16_t material, float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = type; op.material = material;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z; op.radius = r;
	return op;
}
} // namespace

TEST_CASE("shell chunks are 3.2 m and tile negative space") {
	CHECK(ve::kShellChunkSize == doctest::Approx(3.2f));
	const ve::IVec3 c = ve::shell_chunk_of_point(-0.1f, 3.3f, 6.4f);
	CHECK(c.x == -1); CHECK(c.y == 1); CHECK(c.z == 2);
	float o[3];
	ve::shell_chunk_origin(c, o);
	CHECK(o[0] == doctest::Approx(-3.2f));
	CHECK(o[1] == doctest::Approx(3.2f));
}

TEST_CASE("only transparent ops make boxes") {
	const float lo[3] = {-100, -100, -100}, hi[3] = {100, 100, 100};
	const ve::EditOp ops[] = {
		sphere(ve::kOpSphereAdd, ve::material_id("ice"), 10, 10, 10, 1.0f),
		sphere(ve::kOpSphereAdd, ve::material_id("rock"), 20, 10, 10, 1.0f),
		sphere(ve::kOpSpherePaint, ve::material_id("ice_crack"), 30, 10, 10, 2.0f),
		sphere(ve::kOpSphereSubtract, ve::material_id("ice"), 40, 10, 10, 2.0f),
	};
	std::vector<ve::ShellBox> boxes;
	ve::transparent_boxes(ops, 4, nullptr, nullptr, lo, hi, &boxes);
	REQUIRE(boxes.size() == 2);
	CHECK(boxes[0].lo[0] == doctest::Approx(9.0f));
	CHECK(boxes[1].hi[0] == doctest::Approx(32.0f));
}

TEST_CASE("an override brick holding a transparent label makes a box; an opaque one does not") {
	ve::OverrideStore store(4);
	const ve::IVec3 a{1, 2, 3}, b{5, 2, 3};
	ve::OverrideBrick *ba = store.data(store.acquire(a));
	ve::OverrideBrick *bb = store.data(store.acquire(b));
	std::fill(std::begin(ba->mat), std::end(ba->mat), uint8_t(ve::material_id("rock")));
	std::fill(std::begin(bb->mat), std::end(bb->mat), uint8_t(ve::material_id("rock")));
	bb->mat[100] = uint8_t(ve::material_id("ice"));
	const float lo[3] = {-100, -100, -100}, hi[3] = {100, 100, 100};
	std::vector<ve::ShellBox> boxes;
	ve::transparent_boxes(nullptr, 0, nullptr, &store, lo, hi, &boxes);
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].lo[0] == doctest::Approx(5 * ve::kBrickSize));
	// Outside the area of interest: no box.
	const float far_lo[3] = {500, 500, 500}, far_hi[3] = {600, 600, 600};
	boxes.clear();
	ve::transparent_boxes(nullptr, 0, nullptr, &store, far_lo, far_hi, &boxes);
	CHECK(boxes.empty());
}

TEST_CASE("candidates cover a box and stop at the radius") {
	std::vector<ve::ShellBox> boxes = {{{0.5f, 0.5f, 0.5f}, {1.5f, 1.5f, 1.5f}},
			{{200.0f, 0.0f, 0.0f}, {201.0f, 1.0f, 1.0f}}};
	const float cam[3] = {0, 0, 0};
	std::vector<ve::IVec3> out;
	ve::shell_candidates(boxes, cam, 50.0f, &out);
	CHECK(has(out, ve::IVec3{0, 0, 0}));
	CHECK_FALSE(has(out, ve::IVec3{62, 0, 0})); // the far box is past the radius
	// A box touching a chunk face reaches the neighbour (one cell of pad).
	boxes = {{{3.15f, 1.0f, 1.0f}, {3.19f, 1.1f, 1.1f}}};
	ve::shell_candidates(boxes, cam, 50.0f, &out);
	CHECK(has(out, ve::IVec3{0, 0, 0}));
	CHECK(has(out, ve::IVec3{1, 0, 0}));
}

TEST_CASE("the grid requests unknown chunks nearest first and rebuilds dirty ones") {
	ve::ShellGrid g;
	std::vector<ve::IVec3> evicted, req;
	g.set_candidates({ve::IVec3{5, 0, 0}, ve::IVec3{1, 0, 0}}, &evicted);
	CHECK(evicted.empty());
	const float cam[3] = {0, 0, 0};
	g.requests(cam, 8, &req);
	REQUIRE(req.size() == 2);
	CHECK(req[0].x == 1);
	g.note_building(req[0]);
	g.note_result(req[0], true);
	CHECK(g.state(ve::IVec3{1, 0, 0}) == ve::kShellReady);
	g.requests(cam, 8, &req);
	REQUIRE(req.size() == 1);
	CHECK(req[0].x == 5);
	// An edit over the ready chunk makes it requestable again, still drawable meanwhile.
	const float lo[3] = {3.3f, 0.1f, 0.1f}, hi[3] = {3.4f, 0.2f, 0.2f};
	g.mark_dirty(lo, hi);
	CHECK(g.dirty(ve::IVec3{1, 0, 0}));
	CHECK(g.state(ve::IVec3{1, 0, 0}) == ve::kShellReady);
	g.requests(cam, 1, &req);
	REQUIRE(req.size() == 1);
	CHECK(req[0].x == 1);
	// A build that was in flight when an edit landed stays requestable after its result.
	g.note_building(ve::IVec3{1, 0, 0});
	g.mark_dirty(lo, hi);
	g.note_result(ve::IVec3{1, 0, 0}, false);
	CHECK(g.state(ve::IVec3{1, 0, 0}) == ve::kShellEmpty);
	CHECK(g.dirty(ve::IVec3{1, 0, 0}));
}

TEST_CASE("chunks that leave the candidate set are reported evicted exactly once") {
	ve::ShellGrid g;
	std::vector<ve::IVec3> evicted;
	g.set_candidates({ve::IVec3{1, 0, 0}, ve::IVec3{2, 0, 0}}, &evicted);
	g.set_candidates({ve::IVec3{2, 0, 0}}, &evicted);
	REQUIRE(evicted.size() == 1);
	CHECK(evicted[0].x == 1);
	evicted.clear();
	g.set_candidates({ve::IVec3{2, 0, 0}}, &evicted);
	CHECK(evicted.empty());
	CHECK(g.size() == 1);
}

TEST_CASE("thickness: a slab, two layers, a ray ending on ground, the camera inside, the sky") {
	// One slab: front at 5, back at 7, ground at 20.
	CHECK(ve::shell_thickness(7.0f - 5.0f, 0.0f, 20.0f, 5.0f, 4.0f) == doctest::Approx(2.0f));
	// Two slabs: [5,7] and [9,12].
	CHECK(ve::shell_thickness((7.0f + 12.0f) - (5.0f + 9.0f), 0.0f, 20.0f, 5.0f, 4.0f) ==
			doctest::Approx(5.0f));
	// Enters at 5 and ends on ground at 8 inside the ice: one front, no back.
	CHECK(ve::shell_thickness(-5.0f, 1.0f, 8.0f, 5.0f, 4.0f) == doctest::Approx(3.0f));
	// Camera inside: the clear value is a front at 0, the only face is a back at 6.
	CHECK(ve::shell_thickness(6.0f, 0.0f, 20.0f, 0.0f, 4.0f) == doctest::Approx(6.0f));
	// A cut-off shell against the sky: exits sky_thickness past the front.
	CHECK(ve::shell_thickness(-5.0f, 1.0f, 0.0f, 5.0f, 4.0f) == doctest::Approx(4.0f));
	// Never negative, whatever a clipped face left behind.
	CHECK(ve::shell_thickness(-3.0f, -1.0f, 2.0f, 0.0f, 4.0f) == 0.0f);
}

TEST_CASE("an island's shell is split into 32-cell blocks and only holds transparent quads") {
	ve::VolumeData v;
	v.dim = ve::kIslandDim;
	v.sdf.assign(size_t(v.voxel_count()), ve::encode_sdf(0.3f));
	v.mat.assign(size_t(v.voxel_count()), 0);
	// A solid ball of radius 20 voxels in the middle; its upper half is ice, lower rock.
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
	CHECK(ve::volume_has_transparent(v));
	const float origin[3] = {-1.6f, -1.6f, -1.6f};
	std::vector<ve::IslandShellBlock> blocks;
	ve::island_shell_blocks(v, origin, voxel, &blocks);
	REQUIRE(!blocks.empty());
	CHECK(blocks.size() <= 8);
	size_t quads = 0;
	for (const ve::IslandShellBlock &b : blocks) {
		CHECK(b.quads.size() == b.normals.size());
		CHECK(b.origin_local[1] >= origin[1] - 1e-4f); // lower blocks are all rock: omitted
		for (const ve::LodQuad &q : b.quads) {
			ve::LodQuadFields f{};
			ve::lod_quad_unpack(q, &f);
			CHECK(ve::material_transparent(uint16_t(f.material)));
		}
		quads += b.quads.size();
	}
	CHECK(quads > 100);

	std::fill(v.mat.begin(), v.mat.end(), uint8_t(ve::material_id("rock")));
	CHECK_FALSE(ve::volume_has_transparent(v));
	ve::island_shell_blocks(v, origin, voxel, &blocks);
	CHECK(blocks.empty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && scons -Q test`
Expected: compile error, `transparency/shell_grid.h: No such file or directory`. (`src/transparency/*.cpp` is already in `pure_sources` from Task 1's cherry-pick.)

- [ ] **Step 3: Add `OverrideStore::for_each`**

In `extension/src/world/override_store.h`, inside `class OverrideStore`, after `const OverrideBrick *data(int slot) const;`:

```cpp
	// Visits every stored brick as fn(IVec3 brick, const OverrideBrick &). Order is the
	// index's (z, y, x). The caller holds whatever lock guards the store.
	template <class F>
	void for_each(F &&fn) const {
		for (const auto &[key, slot] : index_)
			fn(IVec3{key.x, key.y, key.z}, bricks_[static_cast<size_t>(slot)]);
	}
```

- [ ] **Step 4: Write `shell_grid.h`**

Create `extension/src/transparency/shell_grid.h` with the `#pragma once`, these includes, and exactly the declarations in this task's **Interfaces** block, wrapped in `namespace ve`, plus the private state of `ShellGrid`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include "generator/volume_set.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "world/override_store.h"
#include "world/region.h"
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>
```

```cpp
// private members of ShellGrid
private:
	struct Node {
		ShellState state = kShellUnknown;
		bool dirty = false;
	};
	using Key = std::tuple<int, int, int>; // (z, y, x): deterministic iteration
	static Key key(IVec3 c) { return Key{c.z, c.y, c.x}; }
	std::map<Key, Node> nodes_;
```

Head the file with:

```cpp
// The near-field transparent shell's bookkeeping (docs/superpowers/specs/2026-10-01-
// transparent-voxels-design.md §5, §6). Pure: no godot-cpp, in the native test build.
// LodSystem drives it; LodBuildPass builds the chunks it asks for.
```

- [ ] **Step 5: Write `shell_grid.cpp`**

```cpp
#include "transparency/shell_grid.h"
#include "world/brick.h"
#include "world/material_table.h"
#include <algorithm>
#include <cmath>

namespace ve {

void shell_chunk_origin(IVec3 c, float out[3]) {
	out[0] = static_cast<float>(c.x) * kShellChunkSize;
	out[1] = static_cast<float>(c.y) * kShellChunkSize;
	out[2] = static_cast<float>(c.z) * kShellChunkSize;
}

void shell_chunk_aabb(IVec3 c, float lo[3], float hi[3]) {
	shell_chunk_origin(c, lo);
	for (int a = 0; a < 3; a++) hi[a] = lo[a] + kShellChunkSize;
}

IVec3 shell_chunk_of_point(float x, float y, float z) {
	return IVec3{static_cast<int>(std::floor(x / kShellChunkSize)),
			static_cast<int>(std::floor(y / kShellChunkSize)),
			static_cast<int>(std::floor(z / kShellChunkSize))};
}

namespace {

bool boxes_overlap(const float alo[3], const float ahi[3], const float blo[3], const float bhi[3]) {
	for (int a = 0; a < 3; a++)
		if (alo[a] > bhi[a] || ahi[a] < blo[a]) return false;
	return true;
}

bool bytes_have_transparent(const uint8_t *mat, size_t n) {
	// At most a handful of transparent ids; test each byte against the table.
	for (size_t i = 0; i < n; i++)
		if (mat[i] != 0 && material_transparent(mat[i])) return true;
	return false;
}

} // namespace

bool volume_has_transparent(const VolumeData &v) {
	return v.valid() && bytes_have_transparent(v.mat.data(), v.mat.size());
}

void transparent_boxes(const EditOp *ops, int op_count, const VolumeSet *volumes,
		const OverrideStore *overrides, const float lo[3], const float hi[3],
		std::vector<ShellBox> *out) {
	if (!out) return;
	for (int i = 0; ops && i < op_count; i++) {
		const EditOp &op = ops[i];
		bool transparent = false;
		if (op.type == kOpSphereAdd || op.type == kOpSpherePaint) {
			transparent = material_transparent(static_cast<uint16_t>(op.material));
		} else if (op.type == kOpVolumeAdd && volumes) {
			const VolumeData *v = volumes->get(static_cast<int>(op.aux[0]));
			transparent = v && volume_has_transparent(*v);
		}
		if (!transparent) continue;
		ShellBox b{};
		op_world_aabb(op, b.lo, b.hi);
		out->push_back(b);
	}
	if (!overrides) return;
	// ponytail: rescans the material bytes of every override brick in range on each call;
	// cache a per-slot verdict keyed by consolidation if this shows up in a profile.
	overrides->for_each([&](IVec3 brick, const OverrideBrick &ob) {
		ShellBox b{};
		brick_world_origin(brick, b.lo);
		for (int a = 0; a < 3; a++) b.hi[a] = b.lo[a] + kBrickSize;
		if (!boxes_overlap(b.lo, b.hi, lo, hi)) return;
		if (bytes_have_transparent(ob.mat, static_cast<size_t>(kBrickVoxelCount))) out->push_back(b);
	});
}

void shell_candidates(const std::vector<ShellBox> &boxes, const float cam[3], float radius_m,
		std::vector<IVec3> *out) {
	if (!out) return;
	out->clear();
	std::map<std::tuple<int, int, int>, IVec3> seen;
	for (const ShellBox &b : boxes) {
		// One cell of pad: a shell quad's corners sit in the cells around its edge.
		const IVec3 c0 = shell_chunk_of_point(b.lo[0] - kShellCell, b.lo[1] - kShellCell,
				b.lo[2] - kShellCell);
		const IVec3 c1 = shell_chunk_of_point(b.hi[0] + kShellCell, b.hi[1] + kShellCell,
				b.hi[2] + kShellCell);
		for (int z = c0.z; z <= c1.z; z++)
			for (int y = c0.y; y <= c1.y; y++)
				for (int x = c0.x; x <= c1.x; x++) {
					float lo[3], hi[3];
					shell_chunk_aabb(IVec3{x, y, z}, lo, hi);
					float d2 = 0.0f;
					for (int a = 0; a < 3; a++) {
						const float d = std::max(std::max(lo[a] - cam[a], cam[a] - hi[a]), 0.0f);
						d2 += d * d;
					}
					if (d2 > radius_m * radius_m) continue;
					seen[std::tuple<int, int, int>{z, y, x}] = IVec3{x, y, z};
				}
	}
	out->reserve(seen.size());
	for (const auto &[k, c] : seen) out->push_back(c);
}

void ShellGrid::set_candidates(const std::vector<IVec3> &chunks, std::vector<IVec3> *evicted) {
	std::map<Key, Node> next;
	for (IVec3 c : chunks) {
		const auto it = nodes_.find(key(c));
		next[key(c)] = it != nodes_.end() ? it->second : Node{};
	}
	if (evicted)
		for (const auto &[k, n] : nodes_)
			if (next.find(k) == next.end())
				evicted->push_back(IVec3{std::get<2>(k), std::get<1>(k), std::get<0>(k)});
	nodes_.swap(next);
}

void ShellGrid::mark_dirty(const float lo[3], const float hi[3]) {
	// Two cells of pad, as LoD chunks use: the half-cell tent reaches past a chunk face.
	const float pad = 2.0f * kShellCell;
	const IVec3 c0 = shell_chunk_of_point(lo[0] - pad, lo[1] - pad, lo[2] - pad);
	const IVec3 c1 = shell_chunk_of_point(hi[0] + pad, hi[1] + pad, hi[2] + pad);
	for (auto &[k, n] : nodes_) {
		const int x = std::get<2>(k), y = std::get<1>(k), z = std::get<0>(k);
		if (x >= c0.x && x <= c1.x && y >= c0.y && y <= c1.y && z >= c0.z && z <= c1.z)
			n.dirty = true;
	}
}

void ShellGrid::requests(const float cam[3], int max, std::vector<IVec3> *out) const {
	if (!out) return;
	out->clear();
	std::vector<std::pair<float, IVec3>> want;
	for (const auto &[k, n] : nodes_) {
		if (n.state == kShellBuilding) continue;
		if (n.state != kShellUnknown && !n.dirty) continue;
		const IVec3 c{std::get<2>(k), std::get<1>(k), std::get<0>(k)};
		float lo[3], hi[3];
		shell_chunk_aabb(c, lo, hi);
		float d2 = 0.0f;
		for (int a = 0; a < 3; a++) {
			const float d = 0.5f * (lo[a] + hi[a]) - cam[a];
			d2 += d * d;
		}
		want.push_back({d2, c});
	}
	std::stable_sort(want.begin(), want.end(),
			[](const auto &a, const auto &b) { return a.first < b.first; });
	for (int i = 0; i < static_cast<int>(want.size()) && i < max; i++)
		out->push_back(want[static_cast<size_t>(i)].second);
}

void ShellGrid::note_building(IVec3 c) {
	const auto it = nodes_.find(key(c));
	if (it == nodes_.end()) return;
	it->second.state = kShellBuilding;
	it->second.dirty = false;
}

void ShellGrid::note_result(IVec3 c, bool has_quads) {
	const auto it = nodes_.find(key(c));
	if (it == nodes_.end()) return;
	it->second.state = has_quads ? kShellReady : kShellEmpty; // dirty is left as it is
}

void ShellGrid::note_failed(IVec3 c) {
	const auto it = nodes_.find(key(c));
	if (it != nodes_.end()) it->second.state = kShellUnknown;
}

ShellState ShellGrid::state(IVec3 c) const {
	const auto it = nodes_.find(key(c));
	return it == nodes_.end() ? kShellUnknown : it->second.state;
}

bool ShellGrid::dirty(IVec3 c) const {
	const auto it = nodes_.find(key(c));
	return it != nodes_.end() && it->second.dirty;
}

int ShellGrid::size() const { return static_cast<int>(nodes_.size()); }
void ShellGrid::clear() { nodes_.clear(); }

void island_shell_blocks(const VolumeData &v, const float lattice_origin[3], float voxel,
		std::vector<IslandShellBlock> *out) {
	if (!out) return;
	out->clear();
	if (!volume_has_transparent(v)) return;
	const int n = kLodChunkLattice;
	const int blocks = (v.dim + kLodChunkCells - 1) / kLodChunkCells;
	std::vector<uint8_t> lat(static_cast<size_t>(n) * n * n);
	std::vector<uint16_t> mat(lat.size());
	const uint8_t outside = encode_sdf(kSdfRange);
	for (int bz = 0; bz < blocks; bz++)
		for (int by = 0; by < blocks; by++)
			for (int bx = 0; bx < blocks; bx++) {
				bool any = false;
				// Lattice index i holds the island sample at block * 32 + i - 1 (the LoD
				// lattice's one-cell overlap below the origin). Outside the island: air.
				for (int z = 0; z < n; z++)
					for (int y = 0; y < n; y++)
						for (int x = 0; x < n; x++) {
							const int sx = bx * kLodChunkCells + x - 1;
							const int sy = by * kLodChunkCells + y - 1;
							const int sz = bz * kLodChunkCells + z - 1;
							const int i = lod_lattice_index(x, y, z);
							if (sx < 0 || sy < 0 || sz < 0 || sx >= v.dim || sy >= v.dim || sz >= v.dim) {
								lat[static_cast<size_t>(i)] = outside;
								mat[static_cast<size_t>(i)] = 0;
								continue;
							}
							const int s = VolumeSet::voxel_index(v.dim, sx, sy, sz);
							lat[static_cast<size_t>(i)] = v.sdf[static_cast<size_t>(s)];
							mat[static_cast<size_t>(i)] = v.mat[static_cast<size_t>(s)];
							any = any || material_transparent(v.mat[static_cast<size_t>(s)]);
						}
				if (!any) continue;
				LodContourResult r;
				lod_contour(lat.data(), mat.data(), &r, true);
				if (r.quads.empty()) continue;
				IslandShellBlock b;
				b.origin_local[0] = lattice_origin[0] + static_cast<float>(bx * kLodChunkCells) * voxel;
				b.origin_local[1] = lattice_origin[1] + static_cast<float>(by * kLodChunkCells) * voxel;
				b.origin_local[2] = lattice_origin[2] + static_cast<float>(bz * kLodChunkCells) * voxel;
				b.quads = std::move(r.quads);
				b.normals = std::move(r.normals);
				out->push_back(std::move(b));
			}
}

float shell_thickness(float r, float g, float z_opaque, float z_front, float sky_thickness_m) {
	const float z_exit = z_opaque > 0.0f ? z_opaque : z_front + sky_thickness_m;
	return std::max(r + g * z_exit, 0.0f);
}

} // namespace ve
```

`EditOp`'s member names (`type`, `material`, `pos`, `radius`, `aux`) and the op constants (`kOpSphereAdd`, `kOpSpherePaint`, `kOpVolumeAdd`) are as declared in `extension/src/generator/edit_ops.h`; if a name differs there, use the header's. `lod_lattice_index` and `lod_quad_unpack` are the helpers `extension/tests/test_lod_transparency.cpp` (Task 2) already uses; include their header (`lod/lod_reduce.h` or `lod/lod_quad.h`, whichever declares them). An island lattice stores `ve::encode_sdf` metres, which is exactly what a LoD lattice stores at cells of 0.4 m and below, so the bytes are passed through unconverted.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`.

- [ ] **Step 7: Commit**

```bash
git add extension/src/transparency/shell_grid.h extension/src/transparency/shell_grid.cpp \
	extension/tests/test_shell_grid.cpp extension/src/world/override_store.h
git commit -m "feat: near-shell grid, transparent candidates, island shell blocks and thickness reference"
```

---

### Task 6: Shell-only builds at the near cell size

**Files:**
- Modify: `extension/src/render/lod_build_pass.h`, `extension/src/render/lod_build_pass.cpp`
- Modify: `extension/src/render/lod_pool.h`, `extension/src/render/lod_pool.cpp`
- Modify: `extension/src/debug/hooks_lod.cpp`, `extension/src/debug/hooks.cpp` (bind one hook)
- Test: `tests/test_transparency.gd`

**Interfaces:**
- Consumes: `ve::kShellCell`, `ve::kShellLevel`, `ve::shell_chunk_origin` (Task 5); the shell passes of `LodBuildPass` (Task 2).
- Produces: `LodBuildJob::shell_only` (bool, default false) and `LodBuildResult::shell_only`. A shell-only job samples at `ve::kShellCell` from `ve::shell_chunk_origin(coord)`, runs field → reduce → shell frac → shell quads only, and returns **only** shell quads (no terrain quads, no skirts). Its `level` is `ve::kShellLevel`.
- Produces: `bool LodPool::upload_at(const float origin[3], float cell, uint32_t level, uint32_t flags, const std::vector<ve::LodQuad> &quads, const std::vector<ve::LodQuadNormals> &normals, std::vector<int> *pages_out)`. The chunk record's second vec4 is `(level, flags, 0, 0)`. `flags` bit 0 = near shell (kept on the near field's side of the fade dither); bits 8.. = island atlas slot + 1 (0 = not an island). `LodPool::upload(level, coord, ...)` delegates with `flags = 0`.
- Produces (hook): `debug_shell_build(coord: Vector3i) -> Dictionary` with `ok`, `quads`, `all_transparent`.

- [ ] **Step 1: Write the failing GPU test**

Append to `tests/test_transparency.gd`:

```gdscript
# --- the near shell (spec §5) -------------------------------------------------------------

func shell_chunk_of(p: Vector3) -> Vector3i:
	return Vector3i(floori(p.x / 3.2), floori(p.y / 3.2), floori(p.z / 3.2))

func test_a_shell_only_build_contours_just_the_ice() -> void:
	var w := make_world()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	var ground := centre_hit(w)
	var c: Vector3 = ground["pos"] + Vector3(0, 0.5, 0)
	# No ice yet: the chunk is ground and air, and a shell-only build returns nothing.
	var none: Dictionary = w.hooks().debug_shell_build(shell_chunk_of(c))
	assert_bool(none["ok"]).is_true()
	assert_int(int(none["quads"])).is_equal(0)
	w.hooks().debug_apply_sphere_add(c, 1.0, material_id(w, "ice"))
	var d: Dictionary = w.hooks().debug_shell_build(shell_chunk_of(c))
	assert_bool(d["ok"]).is_true()
	# A 1 m ball at 0.1 m cells: 4*pi*r^2 / 0.01 ~ 1250 quads over the chunks it spans.
	assert_int(int(d["quads"])).is_greater(100)
	assert_bool(d["all_transparent"]).is_true()
```

- [ ] **Step 2: Run to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: FAIL — `debug_shell_build` is not a method.

- [ ] **Step 3: Shell-only jobs in `LodBuildPass`**

`extension/src/render/lod_build_pass.h`: add `#include "transparency/shell_grid.h"`; in `LodBuildJob` add

```cpp
	// A near-field shell chunk (spec §5): sampled at ve::kShellCell from
	// ve::shell_chunk_origin(coord), and only the shell is contoured. `level` is
	// ve::kShellLevel and is identity only.
	bool shell_only = false;
```

and in `LodBuildResult` add `bool shell_only = false;`.

`extension/src/render/lod_build_pass.cpp`, replace `push`:

```cpp
void LodBuildPass::push(int64_t list, const LodBuildJob &job, int job_index, int mode) {
	float origin[3];
	float cell;
	if (job.shell_only) {
		ve::shell_chunk_origin(job.coord, origin);
		cell = ve::kShellCell;
	} else {
		ve::lod_chunk_origin(job.level, job.coord, origin);
		cell = ve::lod_cell_size(job.level);
	}
	const ve::LodBuildPush push{{job.coord.x, job.coord.y, job.coord.z, job_index},
			{sanitized_op_count(job), ve::kLodMaxQuadsPerChunk, job.shell_only ? 0 : job.level, mode},
			{origin[0], origin[1], origin[2], cell},
			{job.override_table, -1, 0, 0}};
	rd_->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
}
```

In `record_job`, after the reduce barrier, branch:

```cpp
	if (job.shell_only) {
		record_frac(list, job, job_index, frac_shell_set_, 1);
		rd_->compute_list_add_barrier(list);
		record_quads(list, job, job_index, quads_shell_set_, 1);
		rd_->compute_list_add_barrier(list);
		return;
	}
```

In `read_job`: set `out->shell_only = job.shell_only;`. For a shell-only job return only the shell stream and skip the skirt append. The function currently reads terrain quads, appends skirts, then appends the shell via `ve::lod_append_shell`. Wrap the terrain read and the skirt append in `if (!job.shell_only) { ... }`, and for `job.shell_only` assign the shell read straight into `out->quads` / `out->normals` instead of calling `lod_append_shell`. The overflow rule for a shell-only job is `out->overflow = (c[3] & 2u) != 0u;`.

Check the three shell shaders for a level-dependent constant before trusting 0.1 m: `grep -n "lpc.grid.w\|lpc.params.z" shaders/lod_field.comp.glsl shaders/lod_reduce.comp.glsl shaders/lod_frac.comp.glsl shaders/lod_quads.comp.glsl`. Each `lpc.grid.w <= 0.4 ? ... : ...` already takes the metres branch at 0.1 m; nothing else may read the level.

- [ ] **Step 4: `LodPool::upload_at`**

`extension/src/render/lod_pool.h`, after `upload`:

```cpp
	// The same all-or-nothing upload for a chunk that is not on the LoD grid: a near-field
	// shell chunk or an island shell block. `flags` lands in the chunk record's second word:
	// bit 0 = near shell, bits 8.. = island atlas slot + 1.
	bool upload_at(const float origin[3], float cell, uint32_t level, uint32_t flags,
			const std::vector<ve::LodQuad> &quads, const std::vector<ve::LodQuadNormals> &normals,
			std::vector<int> *pages_out);
```

`extension/src/render/lod_pool.cpp`: rename the body of `upload` to `upload_at` with the new signature; delete its `lod_chunk_origin` / `lod_cell_size` lines (they are parameters now); write `meta[0] = level; meta[1] = flags;`. Then:

```cpp
bool LodPool::upload(int level, ve::IVec3 coord, const std::vector<ve::LodQuad> &quads,
		const std::vector<ve::LodQuadNormals> &normals, std::vector<int> *pages_out) {
	float origin[3];
	ve::lod_chunk_origin(level, coord, origin);
	return upload_at(origin, ve::lod_cell_size(level), static_cast<uint32_t>(level), 0u, quads,
			normals, pages_out);
}
```

- [ ] **Step 5: The diagnostic hook**

In `extension/src/debug/hooks_lod.cpp`, beside `debug_lod_diff` (which already stands up a per-call `LodBuildPass` and gathers ops for a chunk), add `debug_shell_build(Vector3i coord)`. It builds the job exactly as `debug_lod_diff` does, with these differences:

```cpp
	LodBuildJob job;
	job.level = ve::kShellLevel;
	job.coord = ve::IVec3{coord.x, coord.y, coord.z};
	job.shell_only = true;
	{
		float lo[3], hi[3];
		ve::shell_chunk_aabb(job.coord, lo, hi);
		const float pad = std::max(2.0f * ve::kShellCell, ve::kLatticeFilterPad);
		for (int a = 0; a < 3; a++) { lo[a] -= pad; hi[a] += pad; }
		std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
		if (world_->context().store->edit_log())
			ve::collect_ops_for_aabb(*world_->context().store->edit_log(), lo, hi, &job.ops);
	}
	LodBuildResult r;
	d["ok"] = pass.build_sync(job, &r, nullptr, nullptr) && !r.failed;
	d["quads"] = static_cast<int>(r.quads.size());
	bool all = true;
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		all = all && ve::material_transparent(static_cast<uint16_t>(f.material));
	}
	d["all_transparent"] = all;
	return d;
```

Bind it next to `debug_lod_diff` in `hooks.cpp`'s `_bind_methods`:

```cpp
	ClassDB::bind_method(D_METHOD("debug_shell_build", "coord"), &VoxelDebugHooks::debug_shell_build);
```

and declare it in the hooks header beside `debug_lod_diff`.

- [ ] **Step 6: Build and run**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd -a res://tests/test_lod_mesh_diff.gd`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/lod_build_pass.* extension/src/render/lod_pool.* \
	extension/src/debug/hooks_lod.cpp extension/src/debug/hooks.cpp extension/src/debug/*.h \
	tests/test_transparency.gd
git commit -m "feat: shell-only LoD builds at the near cell size; LodPool::upload_at"
```

---

### Task 7: LodSystem drives the near shell

**Files:**
- Modify: `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/debug/hooks_lod.cpp` (extend `debug_lod_stats`)
- Test: `tests/test_transparency.gd`

**Interfaces:**
- Consumes: `ve::ShellGrid`, `ve::transparent_boxes`, `ve::shell_candidates` (Task 5); `LodBuildJob::shell_only`, `LodPool::upload_at` (Task 6); `LodSystem::transparent_draw_pages()` (Task 2); `RenderOrchestrator::transparency_settings()`.
- Produces: `std::vector<ve::LodPageDraw> LodSystem::shell_draw_pages() const` — the far-field pages holding shell quads, then every near-shell page, then every island shell page (Task 10 fills the last group). This is the list the shell raster draws.
- Produces: `LodStats::shell_chunks` (near-shell chunks in the grid), `LodStats::shell_pages` (near-shell pages resident). `debug_lod_stats` exposes both as `shell_chunks`, `shell_pages`.

Rules for the tick, in order:

1. Results: a `LodBuildResult` with `shell_only` goes to the shell branch, never to the tree. Failed → `shell_grid_.note_failed`. Otherwise release the chunk's old pages, upload the new quads (if any) with `upload_at(origin, ve::kShellCell, 0u, 1u, ...)`, record pages, `note_result(coord, !quads.empty())`. If the upload is refused, keep the old pages and `note_failed`.
2. Candidates: recomputed when `shell_dirty_` is set (any invalidation) or the camera has moved to another shell chunk. Runs with `lod_mutex_` released and `edit_mutex()` held, like `gather_ops`. Evicted chunks release their pages.
3. Requests: shell jobs are taken first into the frame's batch; the tree's requests fill what is left of the batch cap.
4. With `transparency_settings().enabled` false: the candidate set is empty (everything evicts) and no shell job is submitted.

- [ ] **Step 1: Write the failing GPU tests**

Append to `tests/test_transparency.gd`:

```gdscript
# Frames until the near shell has pages, or gives up. Builds are async on the mesh worker.
func frame_until_shell(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var stats := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		frame(w, cam, fwd)
		stats = w.hooks().debug_lod_stats()
		if int(stats.get("shell_pages", 0)) > 0:
			break
	return stats

# The shell needs the MeshService (debug_init_physics) and the LoD pool (a LoD query), the
# same two preconditions every far-field suite sets up.
func shell_world(enabled := true) -> VoxelWorld:
	var w := make_world(enabled)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_lod_stats()
	return w

func test_adding_ice_near_the_camera_publishes_shell_pages() -> void:
	var w := shell_world()
	assert_int(int(w.hooks().debug_lod_stats()["shell_pages"])).is_equal(0)
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var stats := frame_until_shell(w)
	assert_int(int(stats["shell_pages"])).override_failure_message(
		"no near-shell page was ever published: %s" % stats).is_greater(0)
	assert_int(int(stats["shell_chunks"])).is_greater(0)

func test_removing_the_ice_releases_its_shell_pages() -> void:
	var w := shell_world()
	var free_before := int(w.hooks().debug_lod_stats()["pages_free"])
	var p: Vector3 = centre_hit(w)["pos"]
	w.hooks().debug_apply_sphere_add(p, 1.0, material_id(w, "ice"))
	settle(w)
	assert_int(int(frame_until_shell(w)["shell_pages"])).is_greater(0)
	w.hooks().debug_apply_sphere_subtract(p, 2.5)
	settle(w)
	var stats := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
		stats = w.hooks().debug_lod_stats()
		if int(stats["shell_pages"]) == 0:
			break
	assert_int(int(stats["shell_pages"])).override_failure_message(
		"the shell outlived the ice: %s" % stats).is_equal(0)
	# Nothing leaked: the pool has at least as many free pages as before the ice (far-field
	# builds running meanwhile may only have taken pages, so compare the shell's share).
	assert_int(int(stats["pages_free"])).is_less_equal(free_before)

func test_with_the_feature_off_no_shell_is_built() -> void:
	var w := shell_world(false)
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	for i in range(60):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
	assert_int(int(w.hooks().debug_lod_stats()["shell_pages"])).is_equal(0)
```

- [ ] **Step 2: Run to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: FAIL — `shell_pages` is not a key of `debug_lod_stats`.

- [ ] **Step 3: State and declarations**

`extension/src/lod/lod_system.h`: add `#include "transparency/shell_grid.h"`. In `LodStats` add:

```cpp
	int shell_chunks = 0; // near-shell chunks in the grid (spec §5)
	int shell_pages = 0;  // near-shell pages resident
```

Public, after `transparent_draw_pages()`:

```cpp
	// Every page the shell raster draws this frame: the far field's pages that hold a shell
	// quad, every near-shell page, every island shell page. Refreshed by prepare_raster().
	std::vector<ve::LodPageDraw> shell_draw_pages() const;
	// False when the chunk's ops exceed kMaxRegionOps: the shell build is refused.
	bool gather_shell_ops(ve::IVec3 coord, std::vector<ve::EditOp> *out);
```

Private members, after `transparent_draw_pages_`:

```cpp
	// The near-field shell (spec §5). All guarded by lod_mutex_ except shell_dirty_, which
	// record() sets under the edit lock and tick() reads and clears under it.
	ve::ShellGrid shell_grid_;
	std::map<ve::LodKey, std::vector<int>> shell_pages_of_; // key.level == ve::kShellLevel
	std::vector<ve::LodPageDraw> shell_draw_pages_;
	ve::IVec3 shell_cam_chunk_{INT32_MAX, 0, 0};
	bool shell_dirty_ = true; // guarded by WorldStore::edit_mutex()
	void release_shell_pages_locked(ve::IVec3 coord);
	void refresh_shell_candidates(const ve::LodCamera &cam, std::unique_lock<std::mutex> &lock);
```

- [ ] **Step 4: Implement**

`extension/src/lod/lod_system.cpp`:

```cpp
bool LodSystem::gather_shell_ops(ve::IVec3 coord, std::vector<ve::EditOp> *out) {
	if (!out) return false;
	out->clear();
	std::lock_guard<std::mutex> lock(store()->edit_mutex());
	if (!store()->edit_log()) return true;
	float lo[3], hi[3];
	ve::shell_chunk_aabb(coord, lo, hi);
	const float pad = std::max(2.0f * ve::kShellCell, ve::kLatticeFilterPad);
	for (int a = 0; a < 3; a++) {
		lo[a] -= pad;
		hi[a] += pad;
	}
	ve::collect_ops_for_aabb(*store()->edit_log(), lo, hi, out);
	return out->size() <= static_cast<size_t>(ve::kMaxRegionOps);
}

void LodSystem::release_shell_pages_locked(ve::IVec3 coord) {
	const ve::LodKey key{ve::kShellLevel, coord.x, coord.y, coord.z};
	const auto it = shell_pages_of_.find(key);
	if (it == shell_pages_of_.end()) return;
	for (int p : it->second) lod_page_quads_.erase(p);
	lod_pool_->release(it->second);
	shell_pages_of_.erase(it);
}

// Called with lod_mutex_ held through `lock`; releases it across the edit-lock section
// (lock order: edit_mutex -> lod mutex) and re-takes it before returning.
void LodSystem::refresh_shell_candidates(const ve::LodCamera &cam,
		std::unique_lock<std::mutex> &lock) {
	const bool enabled = render()->transparency_settings().enabled;
	const ve::IVec3 cam_chunk = ve::shell_chunk_of_point(cam.pos[0], cam.pos[1], cam.pos[2]);
	const bool moved = cam_chunk.x != shell_cam_chunk_.x || cam_chunk.y != shell_cam_chunk_.y ||
			cam_chunk.z != shell_cam_chunk_.z;
	float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
	fade_band(&fade_start, &fade_end);
	const float radius = fade_end + ve::kShellChunkSize;
	lock.unlock();
	std::vector<ve::ShellBox> boxes;
	bool recompute = moved;
	{
		std::lock_guard<std::mutex> edit_lock(store()->edit_mutex());
		recompute = recompute || shell_dirty_;
		shell_dirty_ = false;
		if (recompute && enabled && store()->edit_log()) {
			float lo[3], hi[3];
			for (int a = 0; a < 3; a++) {
				lo[a] = cam.pos[a] - radius;
				hi[a] = cam.pos[a] + radius;
			}
			std::vector<ve::EditOp> ops;
			ve::collect_ops_for_aabb(*store()->edit_log(), lo, hi, &ops);
			ve::transparent_boxes(ops.data(), static_cast<int>(ops.size()), &store()->volumes(),
					store()->overrides(), lo, hi, &boxes);
		}
	}
	lock.lock();
	if (!recompute) return;
	shell_cam_chunk_ = cam_chunk;
	std::vector<ve::IVec3> chunks, evicted;
	if (enabled) ve::shell_candidates(boxes, cam.pos, radius, &chunks);
	shell_grid_.set_candidates(chunks, &evicted);
	for (ve::IVec3 c : evicted) release_shell_pages_locked(c);
}
```

`store()->volumes()` and `store()->overrides()` are the accessors `debug/hooks_world.cpp:874` already uses; match their exact return types (`&` vs pointer).

In `tick()`, in the result loop, as the first statement inside `for (LodBuildResult &r : done) {`:

```cpp
			if (r.shell_only) {
				if (r.failed) {
					shell_grid_.note_failed(r.coord);
					continue;
				}
				if (r.quads.empty()) {
					release_shell_pages_locked(r.coord);
					shell_grid_.note_result(r.coord, false);
					continue;
				}
				float origin[3];
				ve::shell_chunk_origin(r.coord, origin);
				std::vector<int> pages;
				if (!lod_pool_->upload_at(origin, ve::kShellCell, 0u, 1u, r.quads, r.normals, &pages)) {
					// Refused: the old pages (if any) keep drawing and the chunk is asked for
					// again -- stale beats missing, as for terrain chunks.
					shell_grid_.note_failed(r.coord);
					continue;
				}
				release_shell_pages_locked(r.coord);
				for (int i = 0; i < int(pages.size()); i++) {
					const int first = i * ve::kLodQuadsPerPage;
					lod_page_quads_[pages[static_cast<size_t>(i)]] = std::min(ve::kLodQuadsPerPage,
							static_cast<int>(r.quads.size()) - first);
				}
				shell_pages_of_[ve::LodKey{ve::kShellLevel, r.coord.x, r.coord.y, r.coord.z}] =
						std::move(pages);
				shell_grid_.note_result(r.coord, true);
				continue;
			}
```

After the evictions block and before `std::vector<ve::LodBuildRequest> batch_requests;`:

```cpp
	refresh_shell_candidates(cam, lock);
```

Replace the batch selection so shell jobs go first:

```cpp
	std::vector<ve::LodBuildRequest> batch_requests;
	std::vector<ve::IVec3> shell_requests;
	if (mesh() && !mesh()->lod_busy()) {
		const int cap = std::min<int>(lod_builds_per_frame_, mesh()->lod_max_jobs());
		// Shell chunks first: a missing shell is ice that is not there at all, a missing
		// far chunk is a coarser horizon.
		shell_grid_.requests(cam.pos, cap, &shell_requests);
		for (ve::IVec3 c : shell_requests) shell_grid_.note_building(c);
		const int take = std::min<int>(cap - int(shell_requests.size()),
				int(lod_walk_.requests.size()));
		batch_requests.assign(lod_walk_.requests.begin(), lod_walk_.requests.begin() + take);
		for (const ve::LodBuildRequest &q : batch_requests)
			lod_tree_->note_building(q.level, q.coord);
	}
	lock.unlock();
```

Replace `if (!batch_requests.empty()) {` with `if (!batch_requests.empty() || !shell_requests.empty()) {`, and inside it, before the loop over `batch_requests`, build the shell jobs:

```cpp
		std::vector<ve::IVec3> shell_submitted, shell_refused;
		for (ve::IVec3 c : shell_requests) {
			LodBuildJob j;
			j.level = ve::kShellLevel;
			j.coord = c;
			j.shell_only = true;
			if (!gather_shell_ops(c, &j.ops)) {
				shell_refused.push_back(c);
				continue;
			}
			shell_submitted.push_back(c);
			batch.push_back(std::move(j));
		}
```

Where refused tree requests are handled, also (under the same `lock.lock()` / `unlock()` pair, or its own):

```cpp
		if (!shell_refused.empty()) {
			lock.lock();
			// ponytail: a shell chunk over the op cap is simply not drawn; it never retries
			// until something dirties it. Consolidation brings the region back under the cap.
			for (ve::IVec3 c : shell_refused) shell_grid_.note_result(c, false);
			lock.unlock();
		}
```

Where a refused `submit_lod` rolls tree flags back, also roll the shell chunks back inside the same locked section: `for (ve::IVec3 c : shell_submitted) shell_grid_.note_failed(c);`.

In `prepare_raster_locked()`, after `transparent_draw_pages_` is filled (Task 2's code):

```cpp
	shell_draw_pages_ = transparent_draw_pages_;
	for (const auto &[key, pages] : shell_pages_of_)
		for (int p : pages) {
			const auto q = lod_page_quads_.find(p);
			if (q != lod_page_quads_.end()) shell_draw_pages_.push_back(ve::LodPageDraw{p, q->second});
		}
```

```cpp
std::vector<ve::LodPageDraw> LodSystem::shell_draw_pages() const {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	return shell_draw_pages_;
}
```

In `record()`, after the `merge_or_cap` call: `shell_dirty_ = true;`. In `drain_invalidations()`, inside the `lod_mutex_` section, add `for (const ve::Box3<float> &m : marks) shell_grid_.mark_dirty(m.lo, m.hi);` **before** the `if (!lod_tree_) return;` line. In `teardown()` and `release_gpu()`: `shell_grid_.clear(); shell_pages_of_.clear(); shell_draw_pages_.clear();` and reset `shell_cam_chunk_ = ve::IVec3{INT32_MAX, 0, 0};`. In `stats()`: `s.shell_chunks = shell_grid_.size();` and `s.shell_pages` = the sum of `pages.size()` over `shell_pages_of_`.

`extension/src/debug/hooks_lod.cpp`, in `debug_lod_stats`, add `d["shell_chunks"] = s.shell_chunks; d["shell_pages"] = s.shell_pages;` beside the other keys.

- [ ] **Step 5: Build and run**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_transparency.gd -a res://tests/test_lod_raster_golden.gd -a res://tests/test_lod_cull_golden.gd`
Expected: native `Status: SUCCESS!`; GPU PASS.

- [ ] **Step 6: Commit**

```bash
git add extension/src/lod/lod_system.h extension/src/lod/lod_system.cpp \
	extension/src/debug/hooks_lod.cpp tests/test_transparency.gd
git commit -m "feat: LodSystem builds, publishes and evicts the near-field shell"
```

---

### Task 8: The shell raster — thickness and front

**Files:**
- Modify: `extension/src/render/gpu/gpu.h`, `extension/src/render/gpu/gpu.cpp`
- Modify: `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh`, `extension/tests/test_gpu_layout.cpp`
- Create: `shaders/shell.vert.glsl`, `shaders/shell_thickness.frag.glsl`, `shaders/shell_front.frag.glsl`
- Create: `extension/src/render/shell_raster_pass.h`, `extension/src/render/shell_raster_pass.cpp`
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`
- Modify: `extension/src/render/frame.h`, `extension/src/render/frame.cpp`
- Modify: `extension/src/debug/hooks.cpp`
- Test: `tests/test_transparency.gd`

**Interfaces:**
- Consumes: `LodSystem::shell_draw_pages()` (Task 7), `LodRasterPass::index_array()` / `front_face_clockwise()`, `LodPool` buffers, `IslandAtlas::desc_buffer()`, `CameraUbo::buffer()`, `GBuffer::depth()`.
- Produces: `gpu::RasterState::additive` (bool; every colour attachment blends `ONE + ONE`).
- Produces: `ve::ShellRasterPush { float fade[4]; }` (x fade start, y fade end), macro `SHELL_RASTER_PUSH_FIELDS`.
- Produces: `ShellRasterPass` with `initialize(RenderingDevice*)`, `teardown()`, `release_targets()`, `set_draw_pages(const std::vector<LodRasterPass::PageDraw>&)`, `int draw_page_count() const`, `bool draw(RenderingDevice*, LodPool&, RID index_array, GBuffer&, RID beauty_cam_ubo, RID island_desc, float fade_start, float fade_end, bool front_face_clockwise, bool camera_inside)`, `bool drew() const`, `RID thickness() const` (RG32F), `RID front() const` (RGBA32F: xy oct normal, z distance, w material), `RID front_depth() const` (D32, sampleable).
- Produces: `RenderPasses::shell_raster`; `FrameStage::kStageShell` (`"shell"`), appended after `kStageHistory`.
- Produces (hook): `debug_render_frame` gains `shell_pages` (int), `center_front` (Color), `center_thick` (Vector2: R, G).

- [ ] **Step 1: Write the failing GPU tests**

Append to `tests/test_transparency.gd`:

```gdscript
# --- the shell raster (spec §6) -----------------------------------------------------------

# Frames until the shell raster has drawn a front at the centre pixel.
func frame_until_front(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		if int(d.get("shell_pages", 0)) > 0 and (d["center_front"] as Color).a > 0.5:
			break
	return d

func test_the_front_layer_holds_the_ice_and_the_thickness_is_the_path_through_it() -> void:
	var w := shell_world()
	var ground := centre_hit(w)
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(ground["pos"], 1.0, ice)
	settle(w)
	var d := frame_until_front(w)
	var front: Color = d["center_front"]
	assert_int(int(front.a + 0.5)).override_failure_message(
		"no transparent front at the centre: %s" % d).is_equal(ice)
	var ok: PackedStringArray = d["stages_ok"]
	assert_bool(ok.has("shell")).is_true()
	# The ball is centred on the ground: the ray enters its top and ends on the ground
	# inside it. One front, no back: G = 1, and thickness = R + G * z_ground ~ the radius.
	var t: Vector2 = d["center_thick"]
	assert_float(t.y).is_equal_approx(1.0, 0.01)
	var thickness: float = t.x + t.y * float(d["center_distance"])
	assert_float(thickness).override_failure_message(
		"thickness %f from %s at ground distance %s" % [thickness, t, d["center_distance"]]
		).is_between(0.6, 1.3)
	# The front is nearer than the ground by that thickness.
	assert_float(front.b).is_equal_approx(float(d["center_distance"]) - thickness, 0.05)

func test_a_floating_ball_has_a_matched_front_and_back() -> void:
	var w := shell_world()
	var ground := centre_hit(w)
	w.hooks().debug_apply_sphere_add(ground["pos"] + Vector3(0, 2.0, 0), 0.8, material_id(w, "ice"))
	settle(w)
	var d := frame_until_front(w)
	var t: Vector2 = d["center_thick"]
	assert_float(t.y).override_failure_message("fronts != backs: %s" % d).is_equal_approx(0.0, 0.01)
	# The centre ray passes near the ball's middle: about a diameter of ice.
	assert_float(t.x).is_between(1.0, 1.7)

# Far field: an ice ball ~200 m out, well past the fade band, is drawn by the LoD shell.
func test_far_ice_has_a_front_from_the_lod_shell() -> void:
	var w := shell_world()
	var down: Dictionary = w.raycast(Vector3(CAM.x + 200.0, 200.0, CAM.z), Vector3.DOWN, 300.0)
	assert_bool(down["hit"]).is_true()
	var target: Vector3 = down["pos"]
	var cam := Vector3(CAM.x, target.y + 60.0, CAM.z)
	var fwd := (target - cam).normalized()
	var seen: Dictionary = w.raycast(cam, fwd, 400.0)
	assert_bool(seen["hit"] and float(seen["distance"]) > 150.0).override_failure_message(
		"terrain hides the far ice from this camera; raise it").is_true()
	w.hooks().debug_apply_sphere_add(target, 10.0, material_id(w, "ice"))
	settle(w, cam)
	var d := frame_until_front(w, cam, fwd)
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"no far shell front: %s" % d).is_equal(material_id(w, "ice"))
	# No near-shell chunk exists out there: the page came from the far field.
	assert_int(int(w.hooks().debug_lod_stats()["shell_pages"])).is_equal(0)

func test_a_frame_with_no_ice_draws_no_shell_and_is_unchanged() -> void:
	var w := shell_world()
	var d := frame(w)
	assert_int(int(d["shell_pages"])).is_equal(0)
	assert_float((d["center_front"] as Color).a).is_equal(0.0)
```

`stages_ok` is the list `write_frame_record` already puts in the dictionary; if its key differs, use the key `tests/test_frame_contract.gd` reads.

- [ ] **Step 2: Run to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: FAIL — `center_front` is not a key.

- [ ] **Step 3: Additive blending in the raster helper**

`extension/src/render/gpu/gpu.h`, in `RasterState`, after `logic_or`:

```cpp
	bool additive = false;     // every colour attachment adds (ONE, ONE): accumulation passes
```

`extension/src/render/gpu/gpu.cpp`, in `raster_pipeline`, replace `a->set_enable_blend(false);`:

```cpp
		a->set_enable_blend(state.additive);
		if (state.additive) {
			a->set_src_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
			a->set_dst_color_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
			a->set_color_blend_op(RenderingDevice::BLEND_OP_ADD);
			a->set_src_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
			a->set_dst_alpha_blend_factor(RenderingDevice::BLEND_FACTOR_ONE);
			a->set_alpha_blend_op(RenderingDevice::BLEND_OP_ADD);
		}
```

- [ ] **Step 4: The push block**

`extension/src/gpu_layout/blocks.h`: after `struct LodRasterPush { ... };` add

```cpp
struct ShellRasterPush {
	float fade[4];  // x = fade start, y = fade end (metres), zw unused
};
```

after `kLodRasterPushFields` (or beside the other raster pushes) add

```cpp
inline constexpr Field kShellRasterPushFields[] = {
	VE_LAYOUT_FIELD(ShellRasterPush, fade, Vec4, 0),
};
```

and in `kBlocks`, after the `LodRasterPush` entry:

```cpp
	VE_LAYOUT_BLOCK(ShellRasterPush, "SHELL_RASTER_PUSH_FIELDS", kShellRasterPushFields),
```

`extension/tests/test_gpu_layout.cpp`: the `names.size() == N` check in `"block macros are unique and emit one declaration per field"` goes up by one.

Run: `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests && scons -Q test`
Expected: the first run fails on the generated-file mirror; after regeneration, `Status: SUCCESS!` and `git diff --stat shaders/generated/blocks.glslh` shows the new macro.

- [ ] **Step 5: The shaders**

Create `shaders/shell.vert.glsl`:

```glsl
#[vertex]
#version 460

#include "generated/blocks.glslh"
#include "common.glslh"
#include "lod_quad.glslh"
#include "shade.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 7
#include "beauty_camera.glslh"

// The transparent shell (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §5):
// LoD arena pages pulled exactly as lod.vert.glsl pulls them. Three kinds of page arrive in
// one list. A far-field page is shared with terrain quads, so every non-transparent quad
// collapses outside the clip volume. A near-shell page (chunk flag bit 0) holds only shell
// quads. An island page (chunk flag bits 8..) holds LOCAL-space quads, placed by the island's
// descriptor.
layout(set = 0, binding = 0, std430) readonly buffer Quads { uint v[]; } quads;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;
layout(set = 0, binding = 5, std430) readonly buffer Normals { uint v[]; } normals;
// Eight vec4 per island, as raymarch.comp.glsl reads them: basis columns 0-2 with the body
// translation in .w.
layout(set = 0, binding = 8, std430) readonly buffer IslandDesc { vec4 v[]; } island_desc;

layout(location = 0) out vec3 v_wpos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out flat uint v_material;
layout(location = 3) out flat uint v_near; // 1 = keep on the near field's side of the dither

void main() {
	uint vi = uint(gl_VertexIndex);
	uint quad = vi >> 2;
	uint corner = vi & 3u;
	uint page = quad >> uint(LOD_PAGE_SHIFT);
	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];
	uint flags = floatBitsToUint(chunks.v[ci * 2u + 1u].y);
	uvec3 w = uvec3(quads.v[quad * 3u + 0u], quads.v[quad * 3u + 1u], quads.v[quad * 3u + 2u]);
	v_material = lod_bits_get(w, 78, 16);
	v_near = flags != 0u ? 1u : 0u;
	if (!mat_transparent(v_material)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	vec3 p = lod_corner_pos(w, int(corner), c0.xyz, c0.w);
	uint normal_pair = normals.v[quad * 2u + (corner >> 1u)];
	vec3 n = oct_decode_snorm8((normal_pair >> ((corner & 1u) * 16u)) & 0xFFFFu);
	uint island = flags >> 8;
	if (island != 0u) {
		int i = int(island) - 1;
		vec4 r0 = island_desc.v[i * 8 + 0];
		vec4 r1 = island_desc.v[i * 8 + 1];
		vec4 r2 = island_desc.v[i * 8 + 2];
		mat3 basis = mat3(r0.xyz, r1.xyz, r2.xyz);
		p = basis * p + vec3(r0.w, r1.w, r2.w);
		n = basis * n;
	}
	v_wpos = p;
	v_normal = n;
	gl_Position = bcam.view_proj * vec4(p, 1.0);
}
```

Confirm the chunk record's second word is addressed as written: `grep -n "ci \* 2u + 1u\|chunks.v\[" shaders/lod.vert.glsl shaders/lod_cull.comp.glsl`. The record is two vec4; `meta[1]` (flags) is the `.y` of the second.

Create `shaders/shell_thickness.frag.glsl`:

```glsl
#[fragment]
#version 460

#include "generated/blocks.glslh"
#include "common.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 7
#include "beauty_camera.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in flat uint v_material;
layout(location = 3) in flat uint v_near;

// Additive. R = sum of back-face distances minus front-face distances; G = fronts minus
// backs. thickness = R + G * z_opaque (ve::shell_thickness): G counts the entries with no
// matching exit, i.e. the rays that end on opaque ground inside the medium. Depth-tested
// against the G-buffer, so a face behind the opaque surface contributes nothing.
layout(location = 0) out vec4 out_thick;

layout(push_constant, std430) uniform Push { SHELL_RASTER_PUSH_FIELDS } pc;

void main() {
	float d = distance(v_wpos, bcam.cam.xyz);
	float t_fade = clamp((d - pc.fade.x) / max(pc.fade.y - pc.fade.x, 1e-3), 0.0, 1.0);
	// lod.frag.glsl keeps the far field where bayer < t; the near shell keeps the complement.
	// Every face therefore survives in exactly one shell, which keeps the counts exact.
	bool far_keeps = bayer4(ivec2(gl_FragCoord.xy)) < t_fade;
	if ((v_near != 0u) == far_keeps) discard;
	float s = gl_FrontFacing ? -1.0 : 1.0;
	out_thick = vec4(s * d, -s, 0.0, 0.0);
}
```

Create `shaders/shell_front.frag.glsl`:

```glsl
#[fragment]
#version 460

#include "generated/blocks.glslh"
#include "common.glslh"
#include "shade.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 7
#include "beauty_camera.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in flat uint v_material;
layout(location = 3) in flat uint v_near;

// The nearest front face: xy octahedral normal, z distance from the camera, w material id
// (0 = no front). This pass owns its depth attachment, so the nearest fragment wins.
layout(location = 0) out vec4 out_front;

layout(set = 0, binding = 6) uniform sampler2D gb_depth;
layout(push_constant, std430) uniform Push { SHELL_RASTER_PUSH_FIELDS } pc;

void main() {
	float d = distance(v_wpos, bcam.cam.xyz);
	float t_fade = clamp((d - pc.fade.x) / max(pc.fade.y - pc.fade.x, 1e-3), 0.0, 1.0);
	bool far_keeps = bayer4(ivec2(gl_FragCoord.xy)) < t_fade;
	if ((v_near != 0u) == far_keeps) discard;
	// Reverse-Z: larger is nearer. Anything the G-buffer holds in front of this fragment --
	// grass, leaves, terrain -- hides it.
	if (texelFetch(gb_depth, ivec2(gl_FragCoord.xy), 0).r > gl_FragCoord.z) discard;
	out_front = vec4(oct_encode(normalize(v_normal)), d, float(v_material));
}
```

Both fragment shaders must be compiled against `shell.vert.glsl`; `gpu::compile_raster` takes a vertex file and a fragment file. The thickness shader does not declare `gb_depth` (binding 6), so its uniform set omits it.

- [ ] **Step 6: The pass**

Create `extension/src/render/shell_raster_pass.h`:

```cpp
#pragma once
#include "render/gpu/gpu.h"
#include "render/lod_raster_pass.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <vector>

namespace godot {

class GBuffer;
class LodPool;

// The transparent shell's two raster passes (docs/superpowers/specs/2026-10-01-transparent-
// voxels-design.md §6). Owns three full-resolution targets:
//   thickness  RG32F    additive: R = back distances - front distances, G = fronts - backs
//   front      RGBA32F  xy oct normal, z distance, w material id (0 = none)
//   depth      D32      the front pass's own depth; sampled by the G-buffer resolve
// The thickness pass depth-tests against the G-buffer's depth without writing it; the front
// pass only SAMPLES the G-buffer depth. Neither writes the G-buffer.
class ShellRasterPass {
public:
	~ShellRasterPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	// Drops the framebuffers that reference the G-buffer depth (headless reallocation).
	void release_targets();

	void set_draw_pages(const std::vector<LodRasterPass::PageDraw> &pages) { pages_ = pages; }
	int draw_page_count() const { return static_cast<int>(pages_.size()); }

	// False only on failure. With no pages and the camera outside it draws nothing, returns
	// true, and drew() reports false -- the composite then never reads the targets.
	// camera_inside: the camera sits in a transparent solid, so the thickness target is
	// cleared to a virtual front at distance 0.
	bool draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
			RID beauty_cam_ubo, RID island_desc, float fade_start, float fade_end,
			bool front_face_clockwise, bool camera_inside);
	bool drew() const { return drew_; }
	RID thickness() const { return thick_; }
	RID front() const { return front_; }
	RID front_depth() const { return depth_; }

private:
	bool ensure_targets(RenderingDevice *rd, Vector2i size);
	bool ensure_args(RenderingDevice *rd, int pages);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID thick_shader_, front_shader_, thick_pipeline_, front_pipeline_, sampler_;
	bool pipeline_clockwise_ = false;
	gpu::SetCache thick_set_, front_set_;
	gpu::FramebufferCache thick_fb_, front_fb_;
	RID thick_, front_, depth_, args_;
	Vector2i size_{0, 0};
	int args_capacity_ = 0;
	std::vector<LodRasterPass::PageDraw> pages_;
	bool drew_ = false;
};

} // namespace godot
```

Create `extension/src/render/shell_raster_pass.cpp`:

```cpp
#include "render/shell_raster_pass.h"
#include "render/gbuffer.h"
#include "render/lod_pool.h"
#include "gpu_layout/blocks.h"
#include "lod/lod_contour.h"
#include <godot_cpp/variant/packed_color_array.hpp>
#include <algorithm>

using namespace godot;

ShellRasterPass::~ShellRasterPass() {
	teardown();
}

void ShellRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	thick_shader_ = gpu::compile_raster(rd, group_, "ShellRasterPass", "shell.vert.glsl",
			"shell_thickness.frag.glsl");
	front_shader_ = gpu::compile_raster(rd, group_, "ShellRasterPass", "shell.vert.glsl",
			"shell_front.frag.glsl");
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!thick_shader_.is_valid() || !front_shader_.is_valid()) teardown();
}

void ShellRasterPass::release_targets() {
	if (!rd_) return;
	thick_fb_.release(rd_, group_);
	front_fb_.release(rd_, group_);
}

void ShellRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	thick_shader_ = front_shader_ = thick_pipeline_ = front_pipeline_ = sampler_ = RID();
	thick_ = front_ = depth_ = args_ = RID();
	thick_set_ = front_set_ = gpu::SetCache();
	thick_fb_ = front_fb_ = gpu::FramebufferCache();
	size_ = Vector2i(0, 0);
	args_capacity_ = 0;
	drew_ = false;
	rd_ = nullptr;
}

bool ShellRasterPass::ensure_targets(RenderingDevice *rd, Vector2i size) {
	if (size.x <= 0 || size.y <= 0) return false;
	if (size == size_ && thick_.is_valid() && front_.is_valid() && depth_.is_valid()) return true;
	thick_fb_.release(rd, group_);
	front_fb_.release(rd, group_);
	gpu::RdDevice device{rd};
	for (RID *r : {&thick_, &front_, &depth_}) {
		group_.free(device, *r);
		*r = RID();
	}
	const uint32_t colour = RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	thick_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, size, colour);
	front_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, size, colour);
	depth_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_D32_SFLOAT, size,
			RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	size_ = size;
	return thick_.is_valid() && front_.is_valid() && depth_.is_valid();
}

bool ShellRasterPass::ensure_args(RenderingDevice *rd, int pages) {
	if (pages <= args_capacity_ && args_.is_valid()) return true;
	gpu::RdDevice device{rd};
	group_.free(device, args_);
	args_capacity_ = std::max(64, pages * 2);
	PackedByteArray zero;
	zero.resize(static_cast<int64_t>(args_capacity_) * 20);
	zero.fill(0);
	args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(
			static_cast<uint32_t>(zero.size()), zero, RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT));
	return args_.is_valid();
}

bool ShellRasterPass::draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
		RID beauty_cam_ubo, RID island_desc, float fade_start, float fade_end,
		bool front_face_clockwise, bool camera_inside) {
	drew_ = false;
	if (!rd_ || rd != rd_ || !thick_shader_.is_valid() || !front_shader_.is_valid() || !gb.is_valid())
		return false;
	if (pages_.empty() && !camera_inside) return true;
	if (!index_array.is_valid() || !beauty_cam_ubo.is_valid() || !island_desc.is_valid()) return false;
	if (!ensure_targets(rd, gb.size()) ||
			!ensure_args(rd, std::max(1, static_cast<int>(pages_.size()))))
		return false;
	if (!thick_fb_.get(rd, group_, {thick_, gb.depth()}).is_valid()) return false;
	if (!front_fb_.get(rd, group_, {front_, depth_}).is_valid()) return false;
	if (!thick_pipeline_.is_valid() || !front_pipeline_.is_valid() ||
			pipeline_clockwise_ != front_face_clockwise) {
		gpu::RdDevice device{rd};
		group_.free(device, thick_pipeline_);
		group_.free(device, front_pipeline_);
		// The winding LodRasterPass MEASURED (M5 errata 2) decides gl_FrontFacing in the
		// thickness pass and the culled side in the front pass.
		const RenderingDevice::PolygonFrontFace winding = front_face_clockwise
				? RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE
				: RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		gpu::RasterState thick;
		thick.cull = RenderingDevice::POLYGON_CULL_DISABLED;
		thick.front = winding;
		thick.depth_write = false; // the G-buffer's depth is tested, never written
		thick.color_attachments = 1;
		thick.additive = true;
		thick_pipeline_ = gpu::raster_pipeline(rd, group_, thick_shader_, thick_fb_.format(), thick);
		gpu::RasterState front;
		front.cull = RenderingDevice::POLYGON_CULL_BACK;
		front.front = winding;
		front.color_attachments = 1;
		front_pipeline_ = gpu::raster_pipeline(rd, group_, front_shader_, front_fb_.format(), front);
		pipeline_clockwise_ = front_face_clockwise;
	}
	if (!thick_pipeline_.is_valid() || !front_pipeline_.is_valid()) return false;
	gpu::RdDevice device{rd};
	const RID thick_set = thick_set_.get(device, group_, thick_shader_, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::storage(5, pool.normal_buffer()),
			gpu::ubo(7, beauty_cam_ubo),
			gpu::storage(8, island_desc)});
	const RID front_set = front_set_.get(device, group_, front_shader_, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::storage(5, pool.normal_buffer()),
			gpu::sampled(6, sampler_, gb.depth()),
			gpu::ubo(7, beauty_cam_ubo),
			gpu::storage(8, island_desc)});
	if (!thick_set.is_valid() || !front_set.is_valid()) return false;

	// Device-level upload before any draw list opens; same command layout as
	// LodPool::upload_draw_args.
	if (!pages_.empty()) {
		PackedByteArray args;
		args.resize(static_cast<int64_t>(pages_.size()) * 20);
		uint32_t *a = reinterpret_cast<uint32_t *>(args.ptrw());
		for (size_t i = 0; i < pages_.size(); i++) {
			a[i * 5 + 0] = static_cast<uint32_t>(pages_[i].quad_count * 6);
			a[i * 5 + 1] = 1u;
			a[i * 5 + 2] = 0u;
			a[i * 5 + 3] = static_cast<uint32_t>(pages_[i].page * ve::kLodVertsPerPage);
			a[i * 5 + 4] = 0u;
		}
		rd->buffer_update(args_, 0, args.size(), args);
	}
	const ve::ShellRasterPush push{{fade_start, fade_end, 0.0f, 0.0f}};
	const uint32_t count = static_cast<uint32_t>(pages_.size());

	// Thickness: colour cleared, the G-buffer depth loaded and kept. A camera inside the
	// medium starts every pixel with one front at distance 0: (R, G) = (0, +1).
	PackedColorArray thick_clear;
	thick_clear.push_back(Color(0.0f, camera_inside ? 1.0f : 0.0f, 0.0f, 0.0f));
	int64_t dl = rd->draw_list_begin(thick_fb_.rid(), RenderingDevice::DRAW_CLEAR_COLOR_ALL,
			thick_clear);
	if (dl < 0) return false;
	if (count > 0) {
		rd->draw_list_bind_render_pipeline(dl, thick_pipeline_);
		rd->draw_list_bind_uniform_set(dl, thick_set, 0);
		rd->draw_list_bind_index_array(dl, index_array);
		rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
		rd->draw_list_draw_indirect(dl, true, args_, 0, count, 20);
	}
	rd->draw_list_end();

	PackedColorArray front_clear;
	front_clear.push_back(Color(0, 0, 0, 0));
	dl = rd->draw_list_begin(front_fb_.rid(),
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH, front_clear, 0.0f);
	if (dl < 0) return false;
	if (count > 0) {
		rd->draw_list_bind_render_pipeline(dl, front_pipeline_);
		rd->draw_list_bind_uniform_set(dl, front_set, 0);
		rd->draw_list_bind_index_array(dl, index_array);
		rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
		rd->draw_list_draw_indirect(dl, true, args_, 0, count, 20);
	}
	rd->draw_list_end();
	drew_ = true;
	return true;
}
```

- [ ] **Step 7: Register the pass and wire the stage**

`extension/src/render/orchestrator.h`: forward-declare `class ShellRasterPass;`, add `ShellRasterPass *shell_raster = nullptr;` to the passes struct after `leaf_raster`. `orchestrator.cpp`: `#include "render/shell_raster_pass.h"`; after the `leaf_raster` construction:

```cpp
	// Fail-soft like leaf_raster: a shader that will not compile leaves the pass with no
	// shader, draw() returns false, the stage is cancelled and transparent materials are
	// simply not drawn that frame.
	passes_.shell_raster = new ShellRasterPass();
	passes_.shell_raster->initialize(device);
```

In `teardown_render_passes`, before the `lod_raster` line:

```cpp
	if (passes_.shell_raster) { delete passes_.shell_raster; passes_.shell_raster = nullptr; }
```

`extension/src/render/frame.h`: append to `FrameStage`, after `kStageHistory`:

```cpp
	kStageShell,
```

`frame.cpp`: in `frame_stage_name`'s `kNames`, append `"shell"` after `"history"`. Add `#include "render/shell_raster_pass.h"`, `#include "world/material_table.h"`. In `prepare_headless`, beside the other `release_targets()` calls:

```cpp
		if (ShellRasterPass *shell = render_.passes().shell_raster) shell->release_targets();
```

In `render_pre_opaque`, after the leaves block and before `SsgiPass *ssgi = ...`:

```cpp
	// The transparent shell (spec §6): thickness and the nearest front, after every opaque
	// producer has written G-buffer depth. One gated pair like grass and leaves: a failure
	// cancels the marker and the frame goes on without transparent materials.
	bool shell_drawn = false;
	uint16_t inside_material = 0;
	if (ShellRasterPass *shell = render_.passes().shell_raster;
			shell && transparency.enabled && lod_.pool() && lod_raster) {
		// Is the camera inside a transparent solid? One CPU field sample a frame.
		{
			const ve::FieldView view = store_.field().lock();
			if (view.valid()) {
				const ve::Sample s = view.sample(cam.origin.x, cam.origin.y, cam.origin.z);
				if (s.sdf <= 0.0f && ve::material_transparent(s.material)) inside_material = s.material;
			}
		}
		std::vector<LodRasterPass::PageDraw> shell_pages;
		for (const ve::LodPageDraw &pd : lod_.shell_draw_pages())
			shell_pages.push_back(LodRasterPass::PageDraw{pd.page, pd.quad_count});
		shell->set_draw_pages(shell_pages);
		timings->begin(rd, "shell");
		const bool shell_ok = lod_raster->prepare_index_array(rd, *lod_.pool()) &&
				shell->draw(rd, *lod_.pool(), lod_raster->index_array(), *gb, ubo->buffer(),
						render_.passes().islands->desc_buffer(), fade_start, fade_end,
						lod_raster->front_face_clockwise(), inside_material != 0);
		if (shell_ok) end_stage(rd, kStageShell);
		else cancel_stage(kStageShell);
		shell_drawn = shell_ok && shell->drew();
	}
```

`store_.field().lock()` is the call `VoxelWorld::raycast` makes (`extension/src/voxel_world.cpp:677`); `ve::Sample`'s members are `sdf` and `material`. `shell_drawn` and `inside_material` are consumed by Task 9; until then add `(void)shell_drawn; (void)inside_material;`.

- [ ] **Step 8: Hook readouts**

In `extension/src/debug/hooks.cpp`, `debug_render_frame`, inside the centre-pixel block added in Task 4 (after the raymarch readouts), add:

```cpp
		// The shell's own targets, FULL resolution (spec §6).
		ShellRasterPass *shell = world_->context().render->passes().shell_raster;
		d["shell_pages"] = shell ? shell->draw_page_count() : 0;
		d["center_front"] = Color();
		d["center_thick"] = Vector2();
		if (shell && shell->drew()) {
			const PackedByteArray ff = device->texture_get_data(shell->front(), 0);
			if (ff.size() >= (c + 1) * 16) {
				const float *f = reinterpret_cast<const float *>(ff.ptr()) + c * 4;
				d["center_front"] = Color(f[0], f[1], f[2], f[3]);
			}
			const PackedByteArray tk = device->texture_get_data(shell->thickness(), 0);
			if (tk.size() >= (c + 1) * 8) {
				const float *t = reinterpret_cast<const float *>(tk.ptr()) + c * 2;
				d["center_thick"] = Vector2(t[0], t[1]);
			}
		}
```

with `#include "render/shell_raster_pass.h"`.

- [ ] **Step 9: Build and run**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_transparency.gd -a res://tests/test_frame_contract.gd -a res://tests/test_gbuffer.gd`
Expected: native `Status: SUCCESS!`; GPU PASS. If `test_frame_contract.gd` enumerates stage names, add `"shell"` where it lists them.

If `test_the_front_layer_...` reports `G = -1` where `+1` is expected, the measured winding is the other way round for `gl_FrontFacing`: swap the sign in `shell_thickness.frag.glsl` (`gl_FrontFacing ? 1.0 : -1.0`) **only** after confirming with the floating-ball test that the front pass (which culls back faces with the same winding) still writes a front.

- [ ] **Step 10: Commit**

```bash
git add extension/src/render/gpu extension/src/gpu_layout/blocks.h shaders/generated/blocks.glslh \
	extension/tests/test_gpu_layout.cpp shaders/shell.vert.glsl shaders/shell_thickness.frag.glsl \
	shaders/shell_front.frag.glsl shaders/shell*.import extension/src/render/shell_raster_pass.* \
	extension/src/render/orchestrator.* extension/src/render/frame.* extension/src/debug/hooks.cpp \
	tests/test_transparency.gd
git commit -m "feat: shell raster measures ice thickness and writes the nearest front"
```

---

### Task 9: The composite — the look

**Files:**
- Modify: `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh`, `extension/tests/test_gpu_layout.cpp`
- Create: `shaders/transparency_composite.comp.glsl`
- Create: `extension/src/render/transparency_composite_pass.h`, `extension/src/render/transparency_composite_pass.cpp`
- Modify: `extension/src/render/deferred_pass.h` (one accessor)
- Modify: `extension/src/render/orchestrator.h`, `.cpp`, `extension/src/render/frame.h`, `frame.cpp`
- Test: `tests/test_transparency.gd`

**Interfaces:**
- Consumes: `ShellRasterPass::thickness()` / `front()` / `drew()` (Task 8); `sun_map.glslh` (Task 2); `ve::TransparencySettings` (Task 1); `shell_drawn`, `inside_material` locals (Task 8).
- Produces: `ve::TransparencyCompositePush { float right_tanx[4]; float up_tany[4]; float sky[4]; float params[4]; uint32_t flags[4]; }` — `sky.xyz` ambient; `params.x` min transmit, `params.y` sky thickness (m); `flags.x` beauty flags, `flags.y` material the camera is inside (0 = outside). Macro `TRANSPARENCY_COMPOSITE_PUSH_FIELDS`.
- Produces: `TransparencyCompositePass` with `initialize(RenderingDevice*)`, `teardown()`, `set_sun_ubo(RID)`, `bool is_valid() const`, and `bool render(RenderingDevice*, GBuffer&, const MaterialAtlas&, RID front, RID thickness, RID sun_map, RID sun_cascade_ubo, RID beauty_cam_ubo, const Params&)` where `Params { float right[3], up[3]; float tan_x, tan_y; float ambient[3]; float min_transmit, sky_thickness_m; uint32_t flags; uint32_t inside_material; }`.
- Produces: `DeferredPass::sun_cascade_ubo() const -> RID`; `RenderPasses::transparency_composite`; `FrameStage::kStageTransparency` (`"transparency"`), appended after `kStageShell`.

- [ ] **Step 1: Write the failing GPU tests**

Append to `tests/test_transparency.gd`:

```gdscript
# --- the composite (spec §6) --------------------------------------------------------------

func dist(a: Color, b: Color) -> float:
	return Vector3(a.r - b.r, a.g - b.g, a.b - b.b).length()

func test_ice_tints_the_ground_and_never_hides_it() -> void:
	var w := shell_world()
	var bare: Color = frame(w)["center_lit"]
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var d := frame_until_front(w)
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	var ok: PackedStringArray = d["stages_ok"]
	assert_bool(ok.has("transparency")).is_true()
	assert_float(dist(lit, bare)).override_failure_message(
		"the ice left the pixel untouched: %s vs %s" % [lit, bare]).is_greater(0.01)
	# Blue survives ice better than red: the tint is the table's, not a grey fade.
	assert_float(lit.b / max(bare.b, 1e-4)).is_greater(lit.r / max(bare.r, 1e-4))

# THE regression test for the prior attempt: ten metres of ice is darker than one, and the
# ground behind it still contributes at least the floor.
func test_ten_metres_of_ice_is_darker_than_one_but_still_shows_the_ground() -> void:
	var thin_w := shell_world()
	var bare: Color = frame(thin_w)["center_lit"]
	thin_w.hooks().debug_apply_sphere_add(centre_hit(thin_w)["pos"], 1.0, material_id(thin_w, "ice"))
	settle(thin_w)
	var thin: Color = frame_until_front(thin_w)["center_lit"]

	var thick_w := shell_world()
	var cam := CAM + Vector3(0, 12.0, 0)
	settle(thick_w, cam)
	thick_w.hooks().debug_apply_sphere_add(
		centre_hit(thick_w)["pos"] + Vector3(0, 4.0, 0), 5.0, material_id(thick_w, "ice"))
	settle(thick_w, cam)
	var d := frame_until_front(thick_w, cam)
	var thick: Color = d["center_lit"]
	var t: Vector2 = d["center_thick"]
	assert_float(t.x + t.y * float(d["center_distance"])).override_failure_message(
		"the path through the block is not ~9 m: %s" % d).is_greater(7.0)
	assert_float(dist(thick, bare)).is_greater(dist(thin, bare))
	# Two frames, floor 0.35 and floor 0.05: with the floor raised more of the ground shows,
	# which is only possible if the ground behind 9 m of ice reached the pixel at all.
	thick_w.set_transparency_value("min_transmit", 0.05)
	var low: Color = frame(thick_w, cam)["center_lit"]
	thick_w.set_transparency_value("min_transmit", 0.9)
	var high: Color = frame(thick_w, cam)["center_lit"]
	assert_float(dist(high, low)).override_failure_message(
		"the transmittance floor changes nothing: the ground behind the ice is not in the pixel"
		).is_greater(0.02)

func test_sky_behind_a_floating_ice_ball_is_tinted_not_holed() -> void:
	var w := shell_world()
	var cam := CAM + Vector3(0, 30.0, 0)
	var fwd := Vector3(0.3, 1.0, 0.2) # up at the sky
	settle(w, cam)
	var sky: Color = frame(w, cam, fwd)["center_lit"]
	w.hooks().debug_apply_sphere_add(cam + fwd.normalized() * 6.0, 1.5, material_id(w, "ice"))
	settle(w, cam)
	var d := frame_until_front(w, cam, fwd)
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	assert_float(dist(lit, sky)).is_greater(0.005)
	assert_float(lit.r + lit.g + lit.b).is_greater(0.05) # not a black hole

func test_a_camera_inside_ice_sees_a_tinted_world() -> void:
	var w := shell_world()
	var bare: Color = frame(w)["center_lit"]
	# A ball around the camera itself.
	w.hooks().debug_apply_sphere_add(CAM, 2.0, material_id(w, "ice"))
	settle(w)
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		if int(d["shell_pages"]) > 0:
			break
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	assert_float(lit.r + lit.g + lit.b).is_greater(0.05)
	assert_float(dist(lit, bare)).override_failure_message(
		"two metres of ice around the camera tinted nothing").is_greater(0.01)
	# Only the back face is in view: G = +1 from the clear, -1 from the back = 0.
	assert_float((d["center_thick"] as Vector2).y).is_equal_approx(0.0, 0.01)

func test_ice_frames_are_finite_and_deterministic() -> void:
	var w := shell_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	frame_until_front(w)
	# Let the worker go quiet, then two back-to-back frames must be one image.
	for i in range(30):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
	var a := frame(w)
	var b := frame(w)
	assert_int(int(a["lit_checksum"])).is_equal(int(b["lit_checksum"]))
	assert_bool(finite(a["center_lit"])).is_true()
```

- [ ] **Step 2: Run to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: FAIL — `stages_ok` lacks `transparency`, and the lit pixel equals the bare ground.

- [ ] **Step 3: The push block**

`extension/src/gpu_layout/blocks.h`:

```cpp
struct TransparencyCompositePush {
	float right_tanx[4];  // xyz = camera right, w = tan(fov_x / 2)
	float up_tany[4];     // xyz = camera up,    w = tan(fov_y / 2)
	float sky[4];         // xyz = ambient, w unused
	float params[4];      // x = min transmit, y = sky thickness (m), zw unused
	uint32_t flags[4];    // x = beauty flags, y = material the camera is inside (0 = outside)
};
```

```cpp
inline constexpr Field kTransparencyCompositePushFields[] = {
	VE_LAYOUT_FIELD(TransparencyCompositePush, right_tanx, Vec4, 0),
	VE_LAYOUT_FIELD(TransparencyCompositePush, up_tany, Vec4, 0),
	VE_LAYOUT_FIELD(TransparencyCompositePush, sky, Vec4, 0),
	VE_LAYOUT_FIELD(TransparencyCompositePush, params, Vec4, 0),
	VE_LAYOUT_FIELD(TransparencyCompositePush, flags, UVec4, 0),
};
```

and in `kBlocks`:

```cpp
	VE_LAYOUT_BLOCK(TransparencyCompositePush, "TRANSPARENCY_COMPOSITE_PUSH_FIELDS", kTransparencyCompositePushFields),
```

Bump the block count in `extension/tests/test_gpu_layout.cpp` by one, then regenerate:

Run: `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests && scons -Q test`
Expected: `Status: SUCCESS!` after regeneration.

- [ ] **Step 4: The shader**

Create `shaders/transparency_composite.comp.glsl`:

```glsl
#[compute]
#version 460
#include "generated/gbuffer.glslh"
#include "generated/blocks.glslh"

#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 9
#define VE_MATERIAL_ARRAYS
layout(set = 0, binding = 6) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 7) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"
#include "sun_light.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 8
#include "beauty_camera.glslh"

// Shades transparent fronts over what deferred lit behind them
// (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §6):
//   F * sky + (1 - F) * (T * behind + (1 - T) * body)
// where behind is the lit pixel already here, body the material cel-shaded at the front,
// T = max(transmit ^ thickness, floor) and F Schlick's Fresnel for the material's ior.
// Pixels with no front and no thickness are left exactly as deferred wrote them.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba16f) uniform image2D lit;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D front_tex; // xy oct normal, z distance, w material
layout(set = 0, binding = 3) uniform sampler2D thick_tex; // R, G of ve::shell_thickness
layout(set = 0, binding = 4) uniform sampler2DArray sun_map;
#define SUN_CASCADES 3
layout(set = 0, binding = 5, std140) uniform SunBlock { SUN_CASCADE_BLOCK_FIELDS } sun;
#include "sun_map.glslh"

layout(push_constant, std430) uniform Push { TRANSPARENCY_COMPOSITE_PUSH_FIELDS } pc;

// Mirror of ve::shell_thickness.
float shell_thickness(vec2 rg, float z_opaque, float z_front) {
	float z_exit = z_opaque > 0.0 ? z_opaque : z_front + pc.params.y;
	return max(rg.x + rg.y * z_exit, 0.0);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(lit);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);

	vec4 front = texelFetch(front_tex, px, 0);
	vec2 rg = texelFetch(thick_tex, px, 0).rg;
	bool has_front = front.w > 0.5;
	uint inside = pc.flags.y;
	if (!has_front && inside == 0u) return;

	float depth = texelFetch(gb_depth, px, 0).r;
	float z_opaque = depth > 0.0
			? distance(beauty_world_from_depth(uv, depth), bcam.cam.xyz) : 0.0;
	float thickness = shell_thickness(rg, z_opaque, has_front ? front.z : 0.0);
	if (thickness <= 0.0 && !has_front) return;

	uint mat = has_front ? uint(front.w + 0.5) : inside;
	vec3 T = max(pow(mat_transmit(mat), vec3(thickness)), vec3(pc.params.x));
	vec3 behind = imageLoad(lit, px).rgb;

	if (!has_front) {
		// The camera is inside the medium and this pixel shows no front face: tint only.
		// There is no surface to shade, so the body is the material's flat colour under the
		// ambient term.
		vec3 body = flat_material_albedo(mat) * pc.sky.rgb;
		imageStore(lit, px, vec4(T * behind + (1.0 - T) * body, 1.0));
		return;
	}

	vec3 rd = normalize(beauty_world_from_depth(uv, 1.0) - bcam.cam.xyz);
	vec3 p = bcam.cam.xyz + rd * front.z;
	vec3 n = oct_decode(front.xy);
	// The pixel's world footprint at the front, as composite.frag.glsl derives it.
	vec3 ddx = pc.right_tanx.xyz * (2.0 * pc.right_tanx.w / float(size.x)) * front.z;
	vec3 ddy = pc.up_tany.xyz * (2.0 * pc.up_tany.w / float(size.y)) * front.z;
	vec4 surf = material_surface(mat, p, n, ddx, ddy);
	vec3 shading_n;
	vec2 props = material_props_normal(mat, p, n, ddx, ddy, shading_n);
	vec3 v = -rd;
	vec3 sun_dir = sun_light.dir.xyz;
	float ndl = dot(shading_n, sun_dir);
	float ndh = dot(shading_n, normalize(sun_dir + v));
	// Every front reads the sun map, near or far (spec §6's known ceiling).
	float shadow = (pc.flags.x & BEAUTY_SUN_MAP) != 0u ? sun_map_visibility(p, ndl, front.z) : 1.0;
	// ndv = 1 is cel_shade's "no rim" (see deferred.comp.glsl).
	vec3 body = cel_shade(surf.rgb * mix(1.0, props.y, 0.65), pc.sky.rgb, ndl, 1.0, ndh, shadow,
			1.0, 1.0 - props.x, sun_light.rgb.xyz);
	float ior = mat_ior(mat);
	float f0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
	float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
	vec3 through = T * behind + (1.0 - T) * body;
	imageStore(lit, px, vec4(mix(through, sky_color(reflect(rd, n)), fresnel), 1.0));
}
```

- [ ] **Step 5: The pass**

Create `extension/src/render/transparency_composite_pass.h`:

```cpp
#pragma once
#include "render/gpu/gpu.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>

namespace godot {

class GBuffer;
class MaterialAtlas;

// Shades transparent fronts over the lit G-buffer (docs/superpowers/specs/2026-10-01-
// transparent-voxels-design.md §6). Runs after deferred and before inject, rewriting
// gb.lit() in place for the pixels that have a front (or, with the camera inside the medium,
// a thickness) and leaving every other pixel alone.
class TransparencyCompositePass {
public:
	struct Params {
		float right[3] = {}, up[3] = {};
		float tan_x = 0.0f, tan_y = 0.0f;
		float ambient[3] = {};
		float min_transmit = 0.35f;
		float sky_thickness_m = 4.0f;
		uint32_t flags = 0;           // beauty flags
		uint32_t inside_material = 0; // material the camera sits inside; 0 = outside
	};

	~TransparencyCompositePass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return program_.valid(); }
	// The SunLight UBO is owned by RenderOrchestrator; this pass only mirrors its RID.
	void set_sun_ubo(RID buffer) { sun_light_ubo_ = buffer; }
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials, RID front,
			RID thickness, RID sun_map, RID sun_cascade_ubo, RID beauty_cam_ubo, const Params &p);

private:
	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID sampler_nearest_, sampler_linear_, dummy_far_;
	RID sun_light_ubo_; // NOT owned
	gpu::SetCache set_;
};

} // namespace godot
```

Create `extension/src/render/transparency_composite_pass.cpp`:

```cpp
#include "render/transparency_composite_pass.h"
#include "render/gbuffer.h"
#include "render/material_atlas.h"
#include "gpu_layout/blocks.h"
#include "shade/beauty_settings.h"
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

TransparencyCompositePass::~TransparencyCompositePass() {
	teardown();
}

void TransparencyCompositePass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "TransparencyCompositePass",
			"transparency_composite.comp.glsl");
	if (!program_.valid()) return;
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	// A 1x1 stand-in sun map for frames with none, exactly as DeferredPass keeps one; the
	// shader never reads it because BEAUTY_SUN_MAP is cleared with it.
	PackedByteArray far;
	far.resize(4);
	far.fill(0);
	TypedArray<PackedByteArray> data;
	data.push_back(far);
	dummy_far_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32_SFLOAT, Vector2i(1, 1),
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT, data);
}

void TransparencyCompositePass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_nearest_ = sampler_linear_ = dummy_far_ = RID();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

bool TransparencyCompositePass::render(RenderingDevice *rd, GBuffer &gb,
		const MaterialAtlas &materials, RID front, RID thickness, RID sun_map,
		RID sun_cascade_ubo, RID beauty_cam_ubo, const Params &p) {
	if (!is_valid() || !gb.is_valid() || !front.is_valid() || !thickness.is_valid() ||
			!sun_cascade_ubo.is_valid() || !beauty_cam_ubo.is_valid() || !sun_light_ubo_.is_valid())
		return false;
	uint32_t flags = p.flags;
	if (!sun_map.is_valid()) flags &= ~ve::kFlagSunMap;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::image(0, gb.lit()),
			gpu::sampled(1, sampler_nearest_, gb.depth()),
			gpu::sampled(2, sampler_nearest_, front),
			gpu::sampled(3, sampler_nearest_, thickness),
			gpu::sampled(4, sampler_linear_, sun_map.is_valid() ? sun_map : dummy_far_),
			gpu::ubo(5, sun_cascade_ubo),
			gpu::sampled(6, materials.sampler(), materials.albedo_array()),
			gpu::sampled(7, materials.sampler(), materials.surface_array()),
			gpu::ubo(8, beauty_cam_ubo),
			gpu::ubo(9, sun_light_ubo_)});
	if (!set.is_valid()) return false;
	const ve::TransparencyCompositePush push{
			{p.right[0], p.right[1], p.right[2], p.tan_x},
			{p.up[0], p.up[1], p.up[2], p.tan_y},
			{p.ambient[0], p.ambient[1], p.ambient[2], 0.0f},
			{p.min_transmit, p.sky_thickness_m, 0.0f, 0.0f},
			{flags, p.inside_material, 0u, 0u}};
	const Vector2i size = gb.size();
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, gpu::push_bytes(push),
			gpu::groups(size.x, 8), gpu::groups(size.y, 8));
}
```

The dummy sun map must match how `DeferredPass` creates its stand-in (a 2D **array** texture if `sampler2DArray` requires one on this backend): copy `DeferredPass`'s creation call for its own dummy verbatim (`grep -n "dummy" extension/src/render/deferred_pass.cpp`).

- [ ] **Step 6: Wire it**

`extension/src/render/deferred_pass.h`, public:

```cpp
	// The cascade block render() fills every frame. TransparencyCompositePass binds it rather
	// than keeping a second copy; it is valid once render() has run.
	RID sun_cascade_ubo() const { return sun_ubo_; }
```

(use the member that holds the cascade UBO; `grep -n "RID.*ubo" extension/src/render/deferred_pass.h`).

`orchestrator.h`: forward-declare `class TransparencyCompositePass;`, add `TransparencyCompositePass *transparency_composite = nullptr;` after `shell_raster`. `orchestrator.cpp`: include the header; after the `shell_raster` construction:

```cpp
	passes_.transparency_composite = new TransparencyCompositePass();
	passes_.transparency_composite->initialize(device);
	if (passes_.sun_ubo) passes_.transparency_composite->set_sun_ubo(passes_.sun_ubo->buffer());
```

(the sun UBO is created earlier in the same function, so `passes_.sun_ubo` is already set here). In `teardown_render_passes`, before the `shell_raster` line:

```cpp
	if (passes_.transparency_composite) { delete passes_.transparency_composite; passes_.transparency_composite = nullptr; }
```

`frame.h`: append `kStageTransparency,` after `kStageShell`. `frame.cpp`: append `"transparency"` to `kNames`; include `render/transparency_composite_pass.h`; delete the `(void)shell_drawn; (void)inside_material;` line; and after `end_stage(rd, kStageDeferred);`, before `timings->begin(rd, "inject");`:

```cpp
	// Transparency, shading (spec §6): fronts over what deferred lit behind them. Failure
	// cancels the marker and leaves deferred's image -- never aborts the frame.
	if (TransparencyCompositePass *tc = render_.passes().transparency_composite;
			tc && shell_drawn) {
		ShellRasterPass *shell = render_.passes().shell_raster;
		TransparencyCompositePass::Params tp;
		for (int k = 0; k < 3; k++) {
			tp.right[k] = cp.cam_right[k];
			tp.up[k] = cp.cam_up[k];
			tp.ambient[k] = beauty.ambient[k];
		}
		tp.tan_x = cp.params[0];
		tp.tan_y = cp.params[1];
		tp.min_transmit = transparency.min_transmit;
		tp.sky_thickness_m = transparency.sky_thickness_m;
		tp.flags = beauty_flags;
		tp.inside_material = inside_material;
		timings->begin(rd, "transparency");
		const bool tc_ok = tc->render(rd, *gb, *materials, shell->front(), shell->thickness(),
				use_sun ? sun->map() : RID(), deferred->sun_cascade_ubo(), ubo->buffer(), tp);
		if (tc_ok) end_stage(rd, kStageTransparency);
		else cancel_stage(kStageTransparency);
	}
```

- [ ] **Step 7: Build and run**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_transparency.gd -a res://tests/test_deferred_golden.gd -a res://tests/test_frame_contract.gd`
Expected: native `Status: SUCCESS!`; GPU PASS.

- [ ] **Step 8: Commit**

```bash
git add extension/src/gpu_layout/blocks.h shaders/generated/blocks.glslh \
	extension/tests/test_gpu_layout.cpp shaders/transparency_composite.comp.glsl \
	shaders/transparency_composite.comp.glsl.import extension/src/render/transparency_composite_pass.* \
	extension/src/render/deferred_pass.h extension/src/render/orchestrator.* \
	extension/src/render/frame.* tests/test_transparency.gd
git commit -m "feat: transparency composite tints the lit image by ice thickness, floored"
```

---

### Task 10: Transparent islands

**Files:**
- Modify: `extension/src/generator/volume_set.h`, `extension/src/generator/volume_set.cpp` (`build_volume_mip`)
- Modify: `extension/src/render/island_handoff.h` (`IslandSlotDesc`), `extension/src/physics/island_manager.cpp`
- Modify: `extension/src/render/island_atlas.h`, `extension/src/render/island_atlas.cpp`
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp` (`drain_island_uploads`)
- Modify: `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/render/frame.cpp`
- Modify: `shaders/raymarch.comp.glsl`
- Test: `extension/tests/test_volume_ops.cpp`, `tests/test_transparency.gd`

**Interfaces:**
- Consumes: `ve::opaque_view`, `ve::volume_has_transparent`, `ve::island_shell_blocks`, `LodPool::upload_at`, `LodSystem::shell_draw_pages()`.
- Produces: `void ve::build_volume_mip(const VolumeData &v, std::vector<uint8_t> *out, bool opaque = false)`.
- Produces: `IslandSlotDesc::transparent` (bool; the island's volume holds a transparent label).
- Produces: `IslandAtlas::upload_mip(rd, slot, data, bool opaque)`, `IslandAtlas::upload_descriptors(rd, descs, count, bool transparency_enabled)`; descriptor int lane 18 is `1` when the opaque view applies to the island.
- Produces: `struct IslandShell { int atlas_slot; float voxel; std::vector<ve::IslandShellBlock> blocks; }`; `std::vector<IslandShell> RenderOrchestrator::take_island_shells()`; `uint32_t RenderOrchestrator::island_live_mask() const`.
- Produces: `void LodSystem::apply_island_shells(std::vector<IslandShell> shells, uint32_t live_mask)`.
- Produces (GLSL): `Island::opaque_view` (bool); `island_lattice` applies the rule per sample when it is set.

- [ ] **Step 1: Write the failing native test**

Append to `extension/tests/test_volume_ops.cpp`:

```cpp
TEST_CASE("the opaque island mip sees a transparent solid as outside") {
	ve::VolumeData v;
	v.dim = ve::kIslandDim;
	// Solid everywhere; the lower half rock, the upper half a transparent material.
	v.sdf.assign(size_t(v.voxel_count()), ve::encode_sdf(-0.3f));
	v.mat.assign(size_t(v.voxel_count()), uint8_t(ve::material_id("rock")));
	for (int z = 0; z < v.dim; z++)
		for (int y = 32; y < v.dim; y++)
			for (int x = 0; x < v.dim; x++)
				v.mat[size_t(ve::VolumeSet::voxel_index(v.dim, x, y, z))] =
						uint8_t(ve::material_id("ice"));
	std::vector<uint8_t> plain, opaque;
	ve::build_volume_mip(v, &plain);
	ve::build_volume_mip(v, &opaque, true);
	REQUIRE(plain.size() == opaque.size());
	const uint8_t zero = ve::encode_sdf(0.0f);
	const int cells = v.dim / ve::kVolumeMipStride;
	// The union has no surface anywhere: every cell is all-solid.
	for (size_t i = 0; i < plain.size(); i += 2) CHECK(plain[i + 1] <= zero);
	// The opaque view has one: the cells straddling y = 32 hold both sides.
	const int ci = (2 + 3 * cells + 2 * cells * cells) * 2; // cell y = 3 covers samples 24..32
	CHECK(opaque[size_t(ci)] <= zero);
	CHECK(opaque[size_t(ci) + 1] > zero);
	// A cell wholly in the transparent half is all-outside.
	const int ti = (2 + 6 * cells + 2 * cells * cells) * 2;
	CHECK(opaque[size_t(ti)] > zero);
}
```

Add `#include "world/material_table.h"` and `#include "world/opaque_view.h"` if the file lacks them.

- [ ] **Step 2: Run to verify it fails**

Run: `cd extension && scons -Q test`
Expected: compile error — `build_volume_mip` takes 2 arguments.

- [ ] **Step 3: The opaque mip**

`extension/src/generator/volume_set.h`: change the declaration to

```cpp
// `opaque` builds the mip of the OPAQUE VIEW (world/opaque_view.h): the island marcher skips
// cells by this chain, and under a transparent material the surface it must find is a label
// boundary the union lattice does not have.
void build_volume_mip(const VolumeData &v, std::vector<uint8_t> *out, bool opaque = false);
```

`volume_set.cpp`: add the parameter, and replace the sample read inside the innermost loop:

```cpp
							const int si = VolumeSet::voxel_index(dim, sx, sy, sz);
							uint8_t s = v.sdf[static_cast<size_t>(si)];
							if (opaque && s <= encode_zero && material_transparent(v.mat[static_cast<size_t>(si)]))
								s = outside;
```

with, before the loops, `const uint8_t encode_zero = encode_sdf(0.0f); const uint8_t outside = encode_sdf(0.5f * kVoxelSize);`. `generator/` may not include `world/opaque_view.h` if the layering forbids it (`grep -n '#include "world/' extension/src/generator/*.cpp`); `world/material_table.h` and `world/brick.h` are already the precedent to check. If neither may be included, move `build_volume_mip`'s opaque branch behind a function pointer parameter instead — but check first; `volume_set.cpp` already uses `encode_sdf`.

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`.

- [ ] **Step 4: Write the failing GPU test**

Append to `tests/test_transparency.gd`:

```gdscript
# --- islands (spec §5, §7) ----------------------------------------------------------------

func test_a_floating_island_of_ice_and_rock_is_see_through_where_it_is_ice() -> void:
	var w := shell_world()
	var ground := centre_hit(w)
	var p: Vector3 = ground["pos"]
	# Paint a 0.6 m lens of ground into ice, then lift a box around it out as an island
	# hovering 1.5 m above where it was (the test-island fixture carves and places it).
	w.hooks().debug_apply_sphere_paint(p, 0.6, material_id(w, "ice"))
	settle(w)
	var cell := Vector3i(floori(p.x / 0.8), floori(p.y / 0.8), floori(p.z / 0.8))
	assert_bool(w.hooks().debug_place_test_island(
		0, cell - Vector3i(1, 1, 1), cell + Vector3i(1, 1, 1), Vector3(0, 1.5, 0))).is_true()
	settle(w)
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		if int(d["shell_pages"]) > 0 and (d["center_front"] as Color).a > 0.5:
			break
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"the island's ice has no shell front: %s" % d).is_equal(material_id(w, "ice"))
	# Behind the island's ice the marcher found something that is not ice.
	assert_int(int(d["center_material"])).is_not_equal(material_id(w, "ice"))
	assert_bool(finite(d["center_lit"])).is_true()
	# Clearing the island releases its shell pages.
	w.hooks().debug_clear_test_island(0)
	for i in range(60):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
	assert_float((d["center_front"] as Color).a).is_equal(0.0)
```

`debug_place_test_island(slot, lo_cell, hi_cell, offset)` and `debug_clear_test_island(slot)` are the existing fixtures `tests/test_island_render.gd` uses; follow that file for the argument conventions (cell units, return value) and adjust the call above to match.

- [ ] **Step 5: Run to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: FAIL — no shell front on the island.

- [ ] **Step 6: Descriptor flag, mip, and shell extraction on upload**

`extension/src/render/island_handoff.h`, in `IslandSlotDesc`, after `dim`:

```cpp
	// The island's volume holds a transparent label: the marcher applies the opaque view to
	// it and the transparent shell draws its medium. Set by whoever queues the volume.
	bool transparent = false;
```

`extension/src/physics/island_manager.cpp`: where an island's `IslandSlotDesc` is filled for publishing, set `desc.transparent` from a per-island flag computed once, at line ~782 where `queue_island(atlas_slot, f.volume_slot, r.data)` is called: `const bool transparent = ve::volume_has_transparent(r.data);` stored on the island record beside its `volume_slot` (add a `bool transparent = false;` member to that record struct). The test fixture `debug_place_test_island` (`extension/src/debug/hooks_physics.cpp:168`) builds its own descriptor: set `transparent = ve::volume_has_transparent(d)` there too.

`extension/src/render/island_atlas.h` / `.cpp`:

```cpp
	bool upload_mip(RenderingDevice *rd, int slot, const ve::VolumeData &data, bool opaque);
	void upload_descriptors(RenderingDevice *rd, const IslandSlotDesc *descs, int count,
			bool transparency_enabled);
```

In `upload_mip`: `ve::build_volume_mip(data, &mip, opaque);`. In `upload_descriptors`, replace `i[base + 18] = 0;`:

```cpp
		i[base + 18] = (d.live && d.transparent && transparency_enabled) ? 1 : 0; // opaque view
```

Fix the other caller, `extension/src/debug/hooks_physics.cpp:795`, to pass `world_->context().render->transparency_settings().enabled && ve::volume_has_transparent(volume)`.

`extension/src/render/orchestrator.h`: add

```cpp
// One island's transparent shell, contoured on upload and handed to LodSystem (spec §5).
struct IslandShell {
	int atlas_slot = -1;
	float voxel = 0.0f;
	std::vector<ve::IslandShellBlock> blocks; // empty = the island has no shell
};
```

public methods `std::vector<IslandShell> take_island_shells() { return std::move(pending_island_shells_); }` and `uint32_t island_live_mask() const { return island_live_mask_; }`, and private members `std::vector<IslandShell> pending_island_shells_;`, `uint32_t island_live_mask_ = 0;`, `std::vector<ve::VolumeData> island_shell_wait_;` is **not** needed — the shell needs the descriptor's lattice origin and pitch, which arrive in the same batch's `descs`.

`orchestrator.cpp`, in `drain_island_uploads`: read `const bool transparency = transparency_settings_.get().enabled;` at the top. Change the mip call to

```cpp
				!passes_.islands->upload_mip(device, u.atlas_slot, u.data,
						transparency && ve::volume_has_transparent(u.data)))
```

and, after it, inside the same `for` body:

```cpp
		if (u.to_island_atlas && u.atlas_slot >= 0 && transparency &&
				u.atlas_slot < static_cast<int>(batch.descs.size())) {
			const IslandSlotDesc &d = batch.descs[static_cast<size_t>(u.atlas_slot)];
			IslandShell shell;
			shell.atlas_slot = u.atlas_slot;
			shell.voxel = d.voxel;
			ve::island_shell_blocks(u.data, d.lattice_origin, d.voxel, &shell.blocks);
			pending_island_shells_.push_back(std::move(shell));
		}
```

Change the descriptor upload and record the live mask:

```cpp
	if (batch.descs_dirty && passes_.islands) {
		passes_.islands->upload_descriptors(device, batch.descs.data(),
				static_cast<int>(batch.descs.size()), transparency);
		island_live_mask_ = 0;
		for (size_t s = 0; s < batch.descs.size() && s < 32; s++)
			if (batch.descs[s].live) island_live_mask_ |= 1u << s;
	}
```

If an island's upload and its first descriptor publish can land in different drains (check `IslandHandoff::take`: `descs` is "the last published set, every drain"), the `u.atlas_slot < batch.descs.size()` guard above drops the shell. In that case keep the `VolumeData` in a small `std::map<int, ve::VolumeData> island_shell_wait_` keyed by atlas slot and contour it on the first drain whose `descs[slot].live` is true.

- [ ] **Step 7: Island shell pages in `LodSystem`**

`lod_system.h`: `struct IslandShell;` forward declaration in namespace `godot`; public

```cpp
	// Island shells (spec §5): uploads the freshly contoured ones as local-space pages whose
	// chunk record names the island, and releases the pages of every slot not in `live_mask`.
	// Render thread, before tick().
	void apply_island_shells(std::vector<IslandShell> shells, uint32_t live_mask);
```

private `std::map<int, std::vector<int>> island_shell_pages_; // atlas slot -> pages`.

`lod_system.cpp`:

```cpp
void LodSystem::apply_island_shells(std::vector<IslandShell> shells, uint32_t live_mask) {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	if (!lod_pool_ || lod_pool_->page_count() == 0) return;
	const auto release_slot = [this](int slot) {
		const auto it = island_shell_pages_.find(slot);
		if (it == island_shell_pages_.end()) return;
		for (int p : it->second) lod_page_quads_.erase(p);
		lod_pool_->release(it->second);
		island_shell_pages_.erase(it);
	};
	for (auto it = island_shell_pages_.begin(); it != island_shell_pages_.end();) {
		const int slot = (it++)->first;
		if (slot < 0 || slot >= 32 || (live_mask & (1u << slot)) == 0u) release_slot(slot);
	}
	for (IslandShell &s : shells) {
		release_slot(s.atlas_slot); // a re-extracted island replaces its shell
		std::vector<int> all;
		for (const ve::IslandShellBlock &b : s.blocks) {
			std::vector<int> pages;
			const uint32_t flags = 1u | (static_cast<uint32_t>(s.atlas_slot + 1) << 8);
			if (!lod_pool_->upload_at(b.origin_local, s.voxel, 0u, flags, b.quads, b.normals, &pages))
				continue; // pool full: this block of the island's medium is not drawn
			for (int i = 0; i < int(pages.size()); i++) {
				const int first = i * ve::kLodQuadsPerPage;
				lod_page_quads_[pages[static_cast<size_t>(i)]] = std::min(ve::kLodQuadsPerPage,
						static_cast<int>(b.quads.size()) - first);
			}
			all.insert(all.end(), pages.begin(), pages.end());
		}
		if (!all.empty()) island_shell_pages_[s.atlas_slot] = std::move(all);
	}
}
```

In `prepare_raster_locked()`, after the near-shell loop from Task 7:

```cpp
	for (const auto &[slot, pages] : island_shell_pages_)
		for (int p : pages) {
			const auto q = lod_page_quads_.find(p);
			if (q != lod_page_quads_.end()) shell_draw_pages_.push_back(ve::LodPageDraw{p, q->second});
		}
```

In `teardown()` and `release_gpu()`: `island_shell_pages_.clear();`.

`frame.cpp`, immediately after `render_.drain_island_uploads(rd);`:

```cpp
	if (lod_.pool()) lod_.apply_island_shells(render_.take_island_shells(), render_.island_live_mask());
```

and, because `prepare_raster_locked` only runs inside `lod_.tick` (which the far-field gate can skip), make the shell block of Task 8 call `lod_.prepare_raster();` just before `lod_.shell_draw_pages()` when `in.debug.skip_far_field` is set.

- [ ] **Step 8: The island marcher applies the rule per sample**

`shaders/raymarch.comp.glsl`:

After the other includes that follow `common.glslh` add `#include "opaque_view.glslh"`. In `struct Island` add `bool opaque_view;`. In `island_load`, after `isl.volume_slot = ...` and its range check:

```glsl
	// Descriptor int lane 18: the island's volume holds a transparent label and the feature
	// is on, so its lattice is read through the opaque view (shaders/opaque_view.glslh).
	isl.opaque_view = floatBitsToInt(island_desc.v[i * 8 + 4].z) != 0;
```

Replace `island_lattice` and update its eight call sites in `island_sdf_at` (and any in the island fallback normal) to pass `isl.opaque_view`:

```glsl
// The island's bytes are the SHARED authoritative volume (physics and merge-back read them),
// so the opaque view cannot be baked in: it is applied per sample, and only for islands
// whose descriptor asks for it.
float island_lattice(int volume_slot, int dim, ivec3 v, bool opaque) {
	int i = volume_slot * ISLAND_VOXELS + v.x + v.y * dim + v.z * dim * dim;
	float d = decode_sdf(float(island_byte_sdf(i)) / 255.0);
	if (opaque && d <= 0.0 && mat_transparent(island_byte_mat(i))) d = OPAQUE_OUTSIDE;
	return d;
}
```

In `march_island`, replace the two lines that set the hit's normal and material:

```glsl
			// Under a transparent material the source normal belongs to the union surface;
			// the opaque view's own lattice is what was hit.
			vec3 n_local = isl.opaque_view
					? island_r8_fallback_normal(isl, q, steps_left)
					: island_source_normal(isl, q, steps_left);
			best.n = normalize(isl.basis * n_local);
			best.mat = island_material_at(isl, q);
```

and make `island_material_at` skip a transparent label when the opaque view is on, by stepping the lookup half a voxel inward along the (local) normal before rounding:

```glsl
			vec3 q_mat = isl.opaque_view ? q - n_local * (0.75 * isl.voxel) : q;
			best.mat = island_material_at(isl, q_mat);
```

(replacing the `best.mat` line just written). `island_r8_fallback_normal` reads the lattice through `island_sdf_at`, which now carries the rule, so the fallback normal is the opaque view's.

- [ ] **Step 9: Build and run**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_transparency.gd -a res://tests/test_island_render.gd -a res://tests/test_island_body.gd -a res://tests/test_connectivity.gd`
Expected: native `Status: SUCCESS!`; GPU PASS, with the three island/connectivity suites unchanged from the baseline.

- [ ] **Step 10: Commit**

```bash
git add extension/src/generator/volume_set.* extension/src/render/island_handoff.h \
	extension/src/physics/island_manager.* extension/src/render/island_atlas.* \
	extension/src/render/orchestrator.* extension/src/lod/lod_system.* extension/src/render/frame.cpp \
	extension/src/debug/hooks_physics.cpp shaders/raymarch.comp.glsl \
	extension/tests/test_volume_ops.cpp tests/test_transparency.gd
git commit -m "feat: islands render their transparent material through the shell"
```

---

### Task 11: The G-buffer resolve — outlines, SSR, contact shadows

**Files:**
- Create: `shaders/shell_resolve.frag.glsl`
- Modify: `extension/src/render/shell_raster_pass.h`, `extension/src/render/shell_raster_pass.cpp`
- Modify: `extension/src/render/frame.cpp`
- Test: `tests/test_transparency.gd`

**Interfaces:**
- Consumes: `ShellRasterPass::front()`, `front_depth()` (Task 8); `shaders/inject.vert.glsl` (fullscreen triangle); `GB_PACK_SURFACE` (`shaders/generated/gbuffer.glslh`).
- Produces: `bool ShellRasterPass::resolve(RenderingDevice *rd, GBuffer &gb)` — for every pixel with a front, writes the front's normal, material id and gloss `0.9` into `gb.surface()` and the front's depth into `gb.depth()`. Runs after the transparency composite and before `inject`.
- Produces (hook): `debug_render_frame` gains `center_gb_material` (int: the full-resolution G-buffer material at the centre after the frame) and `center_scene` (Color: the final scene colour at the centre, after outlines).

- [ ] **Step 1: Write the failing GPU tests**

Append to `tests/test_transparency.gd`:

```gdscript
# --- the resolve (spec §6 step 5) ---------------------------------------------------------

func test_after_the_frame_the_gbuffer_holds_the_ice_front() -> void:
	var w := shell_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, ice)
	settle(w)
	var d := frame_until_front(w)
	assert_int(int(d["center_gb_material"])).override_failure_message(
		"outlines and SSR would still see the ground behind the ice: %s" % d).is_equal(ice)

# The silhouette of a floating ball against far ground is a depth break only if the ice is
# in the depth the outline pass reads. Scan the row through the ball's edge for a pixel the
# outline darkened.
func test_the_ice_silhouette_gets_an_outline() -> void:
	var w := shell_world()
	w.set_effect_enabled("outlines", true)
	var p: Vector3 = centre_hit(w)["pos"] + Vector3(0, 2.0, 0)
	var without := w.hooks().debug_render_frame_image(CAM, FWD.normalized(), W, H) as Image
	w.hooks().debug_apply_sphere_add(p, 0.6, material_id(w, "ice"))
	settle(w)
	frame_until_front(w)
	var with := w.hooks().debug_render_frame_image(CAM, FWD.normalized(), W, H) as Image
	var darkest := 1e9
	var baseline := 0.0
	for x in range(W):
		var a := with.get_pixel(x, H / 2)
		var b := without.get_pixel(x, H / 2)
		darkest = min(darkest, a.get_luminance())
		baseline = max(baseline, b.get_luminance())
	assert_float(darkest).override_failure_message(
		"no pixel on the ball's row is outline-dark (darkest %f, bare max %f)" % [darkest, baseline]
		).is_less(baseline * 0.5)
```

`debug_render_frame_image` stands for the existing hook that returns the finished scene colour as an `Image` (the capture path `tests/test_capture.gd` and the outline suite use; `grep -n "Image" tests/test_outline*.gd tests/test_capture.gd`). Use that hook's real name and signature. If the effect toggle for outlines is not `"outlines"`, use the name `tests/test_outline*.gd` uses.

- [ ] **Step 2: Run to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: FAIL — `center_gb_material` missing; no outline on the ball.

- [ ] **Step 3: The resolve shader**

Create `shaders/shell_resolve.frag.glsl`:

```glsl
#[fragment]
#version 460
#include "generated/gbuffer.glslh"
#include "common.glslh"
#include "shade.glslh"

// The G-buffer resolve (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §6
// step 5). After the transparency composite has shaded the lit image, pixels with a
// transparent front take that front as their surface and depth, so everything downstream --
// inject, contact shadows, SSR, outlines -- sees an ordinary glossy surface with no edits of
// its own. A raster pass because a compute shader cannot write a depth attachment.
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 out_surface;

layout(set = 0, binding = 0) uniform sampler2D front_tex;   // xy oct normal, z distance, w material
layout(set = 0, binding = 1) uniform sampler2D front_depth; // the front pass's own depth

// ponytail: one gloss for every transparent material. Read it from the material's surface
// map (material_props) if a matte transparent material is ever added.
const float SHELL_GLOSS = 0.9;

void main() {
	ivec2 px = ivec2(gl_FragCoord.xy);
	vec4 front = texelFetch(front_tex, px, 0);
	if (front.w < 0.5) discard;
	out_surface = GB_PACK_SURFACE(oct_decode(front.xy), uint(front.w + 0.5), SHELL_GLOSS);
	gl_FragDepth = texelFetch(front_depth, px, 0).r;
}
```

Check `GB_PACK_SURFACE`'s argument order and types against `shaders/lod.frag.glsl`'s call (`GB_PACK_SURFACE(shading_n, v_material, 1.0 - props.x)`).

- [ ] **Step 4: `ShellRasterPass::resolve`**

Header: add `bool resolve(RenderingDevice *rd, GBuffer &gb);`, members `RID resolve_shader_, resolve_pipeline_; gpu::SetCache resolve_set_; gpu::FramebufferCache resolve_fb_;`.

In `initialize`, after the other two shaders:

```cpp
	resolve_shader_ = gpu::compile_raster(rd, group_, "ShellRasterPass", "inject.vert.glsl",
			"shell_resolve.frag.glsl");
```

(a failed resolve shader must not tear the pass down: `resolve()` just returns false). Reset the four new members in `teardown()`; release `resolve_fb_` in `release_targets()` and in `ensure_targets()` beside the other two.

```cpp
bool ShellRasterPass::resolve(RenderingDevice *rd, GBuffer &gb) {
	if (!rd_ || rd != rd_ || !drew_ || !resolve_shader_.is_valid() || !gb.is_valid()) return false;
	if (!resolve_fb_.get(rd, group_, {gb.surface(), gb.depth()}).is_valid()) return false;
	if (!resolve_pipeline_.is_valid()) {
		gpu::RasterState state;
		state.color_attachments = 1;
		// Every pixel that reaches the fragment stage has a front in front of the opaque
		// surface (the front pass discarded the rest), so the depth is simply replaced.
		state.compare = RenderingDevice::COMPARE_OP_ALWAYS;
		resolve_pipeline_ = gpu::raster_pipeline(rd, group_, resolve_shader_, resolve_fb_.format(), state);
	}
	if (!resolve_pipeline_.is_valid()) return false;
	gpu::RdDevice device{rd};
	const RID set = resolve_set_.get(device, group_, resolve_shader_, 0, {
			gpu::sampled(0, sampler_, front_),
			gpu::sampled(1, sampler_, depth_)});
	if (!set.is_valid()) return false;
	const int64_t dl = rd->draw_list_begin(resolve_fb_.rid(), RenderingDevice::DRAW_DEFAULT_ALL);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, resolve_pipeline_);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	rd->draw_list_draw(dl, false, 1, 3);
	rd->draw_list_end();
	return true;
}
```

- [ ] **Step 5: Call it, and read the result back**

`frame.cpp`, inside the transparency block of Task 9, replace the two lines that end or cancel the stage:

```cpp
		// Resolve (spec §6 step 5): the front becomes the G-buffer's surface and depth, so
		// inject, contact shadows, SSR and outlines see the ice. Only after a good composite:
		// resolving an unshaded front would outline ice the lit image does not show.
		const bool resolved = tc_ok && shell->resolve(rd, *gb);
		if (tc_ok && resolved) end_stage(rd, kStageTransparency);
		else cancel_stage(kStageTransparency);
```

`extension/src/debug/hooks.cpp`, in the centre-pixel block of `debug_render_frame`:

```cpp
		d["center_gb_material"] = 0;
		const PackedByteArray gs = device->texture_get_data(
				world_->context().render->passes().gbuffer->surface(), 0);
		if (gs.size() >= (c + 1) * 8)
			d["center_gb_material"] = static_cast<int>(half_to_float(
					reinterpret_cast<const uint16_t *>(gs.ptr())[c * 4 + 2]) + 0.5f);
```

(the surface attachment's material channel is the same `z` the march-resolution readout decodes; confirm its format in `extension/src/gpu_layout/gbuffer_layout.h` and decode accordingly).

- [ ] **Step 6: Build and run**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd -a res://tests/test_contact_shadow_golden.gd -a res://tests/test_deferred_golden.gd -a res://tests/test_frame_contract.gd`
Expected: PASS; the goldens (no ice) unchanged.

- [ ] **Step 7: Commit**

```bash
git add shaders/shell_resolve.frag.glsl shaders/shell_resolve.frag.glsl.import \
	extension/src/render/shell_raster_pass.* extension/src/render/frame.cpp \
	extension/src/debug/hooks.cpp tests/test_transparency.gd
git commit -m "feat: resolve the transparent front into the G-buffer for outlines, SSR and contact shadows"
```

---

### Task 12: Benchmark, seam capture, full verification and the record

**Files:**
- Modify: `demo/benchmark.gd`
- Create: `tools/transparency_capture.gd`
- Modify: `docs/superpowers/specs/2026-10-01-transparent-voxels-design.md`

**Interfaces:**
- Consumes: everything above.
- Produces: `demo/benchmark.gd` flags `--transparency=0|1` (sets the `transparency` group's `enabled` before the world streams) and `--ice=<radius_m>` (adds one ice ball of that radius on the first frame whose centre field ray hits).

- [ ] **Step 1: Port the benchmark flags**

```bash
git checkout feat/transparent-materials -- tools/transparency_capture.gd
git show 043f355 -- demo/benchmark.gd | git apply --3way
```

Expected: the patch applies (the file is unchanged on `main` since the prior branch's base apart from unrelated hunks). Then in `demo/benchmark.gd` make sure `--transparency=` is applied **before** the world's first streamed frame (in `_ready`, before the world node is added or before its first `_process`), because `enabled` is read where lattices are baked. In `tools/transparency_capture.gd`, replace any `set_transparency_value("min_step_m" ...)` / `"max_steps"` lines (the walker's knobs) with nothing, and replace reads of `center_trans` / `far_center_front` with `center_thick` / `center_front`.

- [ ] **Step 2: Seam and foliage captures**

Run: `godot --path . -s tools/transparency_capture.gd` (the tool's header states its exact invocation; use that).
Expected: PNGs under `reports/transparency-B/`. Inspect at 1× and 5×:
- `seam`: the chain of ice balls crossing the fade band reads as ice on both sides, with no double-dark band and no missing-pixel line where the near shell hands over to the far shell.
- `foliage`: grass in front of the ice draws over it untinted; grass **behind or under** the ice is tinted.

Record what was and was not inspected, by eye, in Step 5's spec section. A still cannot show a seam that only appears in motion: say so.

- [ ] **Step 3: Cost, interleaved A/B/A**

Run the steady leg three times with an ice patch (`--ice=3` on every leg), alternating the feature: A1 `--transparency=0`, B `--transparency=1`, A2 `--transparency=0`. Then the same three legs with **no** `--ice` (the "no transparent material" cost). Use the exact command line the prior branch's spec §11 item 7 records for its legs (`render_scale=0.65`, `near_field_scale=0.40`, V-Sync off, 300 sampled frames).
Expected: six `steady.txt` reports. Compute `B − mean(A1, A2)` for p50 in both sets, and the A1/A2 spread. Report p99 only if the two A legs agree to within the delta. Do not report per-pass GPU numbers (`valid_samples=0`).

- [ ] **Step 4: Full verification**

Run: `./build.sh --test && ./gdunit_tests.sh 2>&1 | tee "$TMPDIR/transparency-final.txt" | tail -40`
Expected: native `Status: SUCCESS!`. The gdUnit failing set is a subset of, or equal to, Task 0's baseline list plus nothing new. For any suite failing that was not in the baseline: `git stash push -u -m verify-baseline-<suite>`, re-run that suite on the clean tree, `git stash apply` the entry by SHA and drop it — only a suite that passes clean and fails with the change is a regression.

- [ ] **Step 5: Record deviations and measurements in the spec**

Append a section `## 11. Deviations and measurements recorded during implementation` to `docs/superpowers/specs/2026-10-01-transparent-voxels-design.md` containing: the nine deviations and two ceilings from this plan's "Deviations From The Spec" section, verbatim; the two A/B/A tables from Step 3 with the caveats stated there; what Step 2 inspected and what it did not; and any deviation made during implementation that this plan did not foresee. Change the spec's **Status** line to `implemented`.

- [ ] **Step 6: Commit**

```bash
git add demo/benchmark.gd tools/transparency_capture.gd tools/transparency_capture.gd.uid \
	docs/superpowers/specs/2026-10-01-transparent-voxels-design.md
git commit -m "chore: transparency benchmark flags, seam capture, measured cost and recorded deviations"
```
