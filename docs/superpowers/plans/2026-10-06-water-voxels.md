# Water Voxels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `water` material that renders as semi-realistic water (flowing wave normals, refraction, absorption with scatter, Fresnel, glint, shore foam, an underwater view) and is a ghost to colliders and connectivity.

**Architecture:** Water is a material row with a new `liquid` column. A third view of the field, the **solid view** (liquid is air, ice stays solid), is the existing opaque-view accumulator run with a different "counts as air" rule, and feeds colliders, occupancy, contact refinement and island extraction. Rendering reuses the transparency pipeline end to end: the transparency composite gains a liquid branch that samples water's own atlas normal map with triplanar, downward-scrolling layers, refracts from a copy of `lit`, and writes its shading normal back into the front target so the unchanged G-buffer resolve hands it to SSR and outlines. Underwater, a cull-free front pipeline records the surface overhead.

**Tech Stack:** C++20 GDExtension (godot-cpp, Godot 4.7 RenderingDevice), GLSL 460 compute and raster shaders, doctest (native), gdUnit4 (GPU), GDScript tools.

**Spec:** `docs/superpowers/specs/2026-10-06-water-voxels-design.md`. Read it, and `docs/superpowers/specs/2026-10-01-transparent-voxels-design.md` §3–§6 (the pipeline this extends), before starting.

## Global Constraints

- No `Co-Authored-By` or other AI attribution lines in any commit message. Plain conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`).
- Work only in `.worktrees/water` on branch `feat/water` (created in Task 0 from `main`). Never commit to `main`.
- Liquid is generic: no shader, pass or C++ branch names "water" except the material row, `Liquid::water` / `LIQUID_WATER`, the generator tool, test fixtures and the benchmark flag. Everything else keys off `liquid` / `mat_liquid` / `ve::material_liquid`.
- `BeautySettings`, `deferred.comp.glsl`, `shade.glslh`, `ssr.comp.glsl`, `outline.comp.glsl` and `shell_resolve.frag.glsl` are not edited.
- The transparency composite's **ice branch stays byte-for-byte identical**, except that the front is read with `imageLoad(front_img, px)` instead of `texelFetch(front_tex, px, 0)`.
- Scenes with no liquid render bit-for-bit as before. Pins: `tests/test_transparency.gd` and every `tests/test_*_golden.gd`. `tests/test_frame_shipped_golden.gd` is known flaky and informational only; never re-record it.
- Material row, verbatim from the spec: `water`, asset `"08"`, hardness `1.0`, glow `0`, flat_albedo `{0.03, 0.16, 0.20}`, transmit `{0.50, 0.82, 0.88}`, ior `1.33`, liquid `Liquid::water`.
- `WaterSettings` defaults, verbatim: `wave_strength = 1.0`, `flow_speed = 0.6` (m/s), `refraction_strength = 0.03`, `foam_width_m = 0.3`. Clamps: wave_strength [0, 3], flow_speed [0, 5], refraction_strength [0, 0.2], foam_width_m [0, 3].
- Wave constants: top tiles 5.0 m and 1.35 m, top drift 0.04 and 0.09 m/s about 110° apart; side tiles 3.0 m and 0.85 m, side stretch ×2.5, side layer B falls at 1.6 × `flow_speed`; triplanar weights `|n|⁴` normalised; projections under weight 0.01 are skipped.
- Generated files are never hand-edited. `shaders/generated/*` (including `field.glslh.golden`) are regenerated with `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests`. `shaders/material_table.glslh` is replaced with the text its failing test prints.
- Every new `.glsl` file gets its Godot `.import` sidecar committed once a test run has created it. `.glslh` files have none. Never put a literal include directive inside a comment in a `.glslh` file (the loader matches include tokens anywhere in a line).
- Tabs for indentation in C++, GLSL and GDScript.
- Build: `./build.sh`. Native tests: `cd extension && scons -Q test` (single case: `cd extension && ./build/tests/ve_tests -tc="<name>"`). GPU tests: `./build.sh && ./gdunit_tests.sh -a res://tests/<file>.gd`.
- gdUnit has a standing, drifting set of failures on clean `main`. Task 0 records it; every "no new failures" check compares against that list, not against zero. If a suite fails that is not on the list, stash your change and re-run it on the clean tree before blaming the change.
- GPU pass timings are invalid on this machine (`valid_samples=0`). Cost is measured as interleaved A/B/A frame time only.
- gdUnit render tests go through `debug_render_frame` (the shipping `VoxelFrame`), never through a hook that rebuilds render inputs. Lighting tests stream at `(20, 60, 30)` and turn SSAO and SSGI off, as `tests/test_transparency.gd` does.

## Deviations From The Spec (decided while planning; Task 8 records them in the spec)

1. **The flow rule is tested by executing `shaders/water_flow.glslh` natively**, through a small C++ shim, as `extension/tests/test_grass_tilt_shader.cpp` does for the blade tilt. There is no `ve::water_flow_uv` C++ mirror: a mirror can drift, the executed shader cannot. The pure uv math lives in `water_flow.glslh`; the texture sampling lives in `water_waves.glslh`.
2. **The resolve is not edited.** The composite writes the camera-facing shading normal into `front.xy` (the front target gains storage usage), and the existing resolve already copies `front.xy` into the G-buffer surface. Spec §7's "resolve evaluates the waves" would sample the waves twice per pixel.
3. **The triplanar blend reuses `triplanar_normal()` from `shaders/common.glslh`**, the whiteout blend the terrain already uses, which carries the geometric sign per projection. A negative face's ripple texture is mirrored, exactly as terrain textures are; for a ripple map this is invisible.
4. **Brick generation and brick marking gate the second evaluation.** They evaluate the solid view only when the brick's filtered op list holds a liquid ADD/PAINT or any volume op (`op_may_hold_liquid`); otherwise they apply the label rule to the union sample. Without such an op the two are equal by construction, so the CPU mirror (which always evaluates) matches exactly.
5. **Island normals at a liquid boundary.** Where the masked union and the masked solid view disagree, the island voxel stores `0x8080`, the existing "no stored normal" marker, and the marcher shades it from the R8 lattice. The union gradient there belongs to the water.
6. **`mesh/mesh_chunk.cpp`'s `chunk_has_surface` keeps the union.** It is a conservative residency probe for collider chunks; a chunk holding only water is built and meshes zero triangles.
7. **Snell's window shows the lit pixel**, which already holds deferred's sky, rather than `sky_color(r)`.
8. **Shading constants pinned here:** scatter picks up `0.35` of the sun; the glint is capped at `64`; foam albedo `0.9`, foam opacity `0.85`.
9. **Two §9 GPU tests take a more direct form.** "A physics ray passes through water" is tested on the collision lattice itself (`debug_mesh_lattice_diff` reports no surface for a water ball, and a surface for the same ball in rock), because the gdUnit fixtures run with physics off. "A severed floating water blob does not become an island" is tested at its cause: the blob's cell is `kCellAir` in the GPU occupancy grid and in `cell_state_field`, so it can never be a component.

## Review Focus

1. **Water against bare sky.** A floating water ball with only sky behind has thickness `sky_thickness_m` from its front. Expect tinted sky, finite, not black, no foam. Test in Task 6.
2. **Painted water on ground.** `OP_SPHERE_PAINT` relabels ground in place. Expect the ground to stay solid in the solid view (it is still standing on rock) and a painted patch to render as essentially the same ground. Test in Task 2 (native).
3. **Camera grazing a surface.** A camera a few centimetres inside a small water ball flips `inside` on and off as it moves. Expect every frame finite and not black. Test in Task 7.
4. **Extreme settings.** `wave_strength` 0 and 3, `refraction_strength` 0.2, `foam_width_m` 0. Expect finite, deterministic frames. Test in Task 6.
5. **Ice and water in one view.** An ice ball floating above a pond. Expect the nearer medium's front to win the pixel and the ice to be shaded by the ice path. Test in Task 6.

---

## File Structure

**Create:**
- `tools/make_water_textures.gd` — writes `assets/materials/08_*.png` (a tileable ripple normal map plus flat maps).
- `assets/materials/08_basecolor.png`, `08_normal.png`, `08_roughness.png`, `08_ambientOcclusion.png`, `08_height.png` — generated, committed.
- `extension/tests/test_solid_view.cpp` — native tests of the solid view and its CPU consumers.
- `tests/test_water_ghost.gd` — GPU differential tests: occupancy, collision lattice, island extraction with water present.
- `extension/src/water/water_settings.h`, `water_settings.cpp`, `water_settings_store.h` — the module's settings.
- `extension/tests/test_water_settings.cpp`.
- `shaders/water_flow.glslh` — pure uv/scroll/weight math (natively executable).
- `shaders/water_waves.glslh` — texture taps, the wave normal, glint roughness, foam.
- `extension/tests/test_water_flow.cpp` — executes `water_flow.glslh`.
- `tests/test_water.gd` — GPU shading and underwater tests.

**Modify:**
- `extension/src/world/material_table.h`, `material_table.cpp`, `shaders/material_table.glslh`, `extension/tests/test_material_table.cpp`, `extension/tests/test_material_glslh.cpp`, `tools/convert_materials.sh` — the row and the `Liquid` column.
- `extension/src/generator/edit_ops.h`, `edit_ops.cpp` — `ve::AirRule`.
- `extension/src/world/opaque_view.h` — `ve::solid_view`.
- `extension/src/world/brick_eval.h`, `brick_eval.cpp` — `eval_field_solid`; occupancy from the solid view.
- `extension/src/connectivity/contact_refine.cpp` — contact from the solid view.
- `extension/src/generator/volume_set.cpp` — island extraction from the solid view.
- `extension/src/debug/hooks_physics.cpp` — the collision-lattice CPU reference reads the solid view.
- `shaders/field_ops.glslh`, `shaders/generated/field.glslh.golden` — the rule, `eval_field_solid`, `solid_view`, `op_may_hold_liquid`.
- `shaders/brick_gen.comp.glsl`, `shaders/brick_mark.comp.glsl`, `shaders/mesh_field.comp.glsl`, `shaders/island_extract.comp.glsl` — the GPU consumers.
- `extension/SConstruct` — `src/water/*.cpp` in the native test build.
- `extension/src/render/orchestrator.h`, `orchestrator.cpp`, `extension/src/voxel_world.h`, `voxel_world.cpp`, `extension/src/voxel_settings.h`, `voxel_settings.cpp`, `tests/test_voxel_settings.gd` — the `water` settings group.
- `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh` — the composite push block's `water` lane.
- `shaders/transparency_composite.comp.glsl`, `extension/src/render/transparency_composite_pass.h`, `.cpp` — the liquid branch and the `lit` copy.
- `extension/src/render/shell_raster_pass.h`, `.cpp` — front target storage usage; the exit-face pipeline.
- `extension/src/render/frame.cpp` — time, water settings, inside-liquid.
- `demo/benchmark.gd` — `--water=R`.
- `tools/transparency_capture.gd` — water cases.
- `docs/superpowers/specs/2026-10-06-water-voxels-design.md` — §12, recorded deviations and measurements.

---

### Task 0: Worktree and baseline

**Files:** none changed.

- [ ] **Step 1: Create the worktree**

Use superpowers:using-git-worktrees to create `.worktrees/water` on a new branch `feat/water` from `main` (which carries the spec commit `1ca81f2`). All later commands run from `.worktrees/water`.

Run: `git -C .worktrees/water status -sb | head -1`
Expected: `## feat/water`

- [ ] **Step 2: Native tests green**

Run: `./build.sh --test`
Expected: build OK, doctest summary `Status: SUCCESS!`.

- [ ] **Step 3: Record the gdUnit baseline**

Run: `./gdunit_tests.sh 2>&1 | tee "$TMPDIR/water-baseline.txt" | tail -40`
Expected: a summary listing the failing suites (about 22 minutes). Write the failing suite names into the task notes. Every later "no new failures" check compares against this list.

- [ ] **Step 4: Record the pins**

Run: `./gdunit_tests.sh -a res://tests/test_transparency.gd 2>&1 | tail -5`
Expected: all cases pass. Then for each `tests/test_*_golden.gd` except `test_frame_shipped_golden.gd`:
`for f in tests/test_*_golden.gd; do [ "$f" = tests/test_frame_shipped_golden.gd ] || ./gdunit_tests.sh -a "res://$f" 2>&1 | tail -3; done`
Record which pass. These are the pins every later task re-runs.

---

### Task 1: The water row, the `Liquid` column and its textures

**Files:**
- Modify: `extension/src/world/material_table.h`, `extension/src/world/material_table.cpp`
- Modify: `shaders/material_table.glslh` (replaced with the text the failing test prints)
- Modify: `extension/tests/test_material_table.cpp`, `extension/tests/test_material_glslh.cpp`
- Modify: `tools/convert_materials.sh`
- Create: `tools/make_water_textures.gd`, `assets/materials/08_{basecolor,normal,roughness,ambientOcclusion,height}.png`

**Interfaces:**
- Produces (C++, `ve`): `enum class Liquid : uint8_t { none = 0, water = 1 }`, `kLiquidNames[]`, `kLiquidCount`, `MaterialDef::liquid`, `Liquid material_liquid(uint16_t id)`.
- Produces (GLSL, everywhere `common.glslh` is included): `const uint LIQUID_NONE = 0u`, `const uint LIQUID_WATER = 1u`, `MAT_LIQUID[]`, `uint mat_liquid(uint id)`, `const uint MAT_WATER = 9u`.

- [ ] **Step 1: Write the failing native tests**

Append to `extension/tests/test_material_table.cpp`:

```cpp
// Water spec §2: a liquid is a KIND, not a flag, and the lookup fails soft like the rest.
TEST_CASE("liquid is a table property: water is a liquid, ice and everything else are not") {
	CHECK(ve::material_liquid(ve::material_id("water")) == ve::Liquid::water);
	CHECK(ve::material_liquid(ve::material_id("ice")) == ve::Liquid::none);
	CHECK(ve::material_liquid(ve::material_id("rock")) == ve::Liquid::none);
	CHECK(ve::material_liquid(0) == ve::Liquid::none);                // air
	CHECK(ve::material_liquid(ve::kFoliageBase) == ve::Liquid::none); // foliage has no row
	CHECK(ve::material_liquid(9999) == ve::Liquid::none);
	// A liquid is also transparent: the shell, thickness and composite are what draw it.
	CHECK(ve::material_transparent(ve::material_id("water")));
	float t[3];
	ve::material_transmit(ve::material_id("water"), t);
	CHECK(t[0] == doctest::Approx(0.50f));
	CHECK(t[1] == doctest::Approx(0.82f));
	CHECK(t[2] == doctest::Approx(0.88f));
	CHECK(ve::material_ior(ve::material_id("water")) == doctest::Approx(1.33f));
}

TEST_CASE("every liquid row is transparent") {
	for (int i = 0; i < ve::kMaterialCount; i++) {
		if (ve::kMaterials[i].liquid == ve::Liquid::none) continue;
		CHECK(ve::material_transparent(static_cast<uint16_t>(i + 1)));
	}
}
```

Append to `extension/tests/test_material_glslh.cpp`:

```cpp
TEST_CASE("the emitter carries the liquid kind and names every liquid") {
	const std::string s = ve::material_table_glsl();
	CHECK(s.find("const uint LIQUID_NONE = 0u;") != std::string::npos);
	CHECK(s.find("const uint LIQUID_WATER = 1u;") != std::string::npos);
	CHECK(s.find("const uint MAT_LIQUID[MATERIAL_COUNT]") != std::string::npos);
	CHECK(s.find("uint mat_liquid(uint id)") != std::string::npos);
	for (int i = 0; i < ve::kMaterialCount; i++) {
		std::string upper = ve::kMaterials[i].name;
		for (char &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		CHECK(upper != "LIQUID");
	}
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd extension && scons -Q test`
Expected: compile errors, `no member named 'material_liquid' in namespace 've'` and `no member named 'Liquid'`.

- [ ] **Step 3: Add the column and the row**

In `extension/src/world/material_table.h`, above `struct MaterialDef`:

```cpp
// Which liquid a material is (docs/superpowers/specs/2026-10-06-water-voxels-design.md §2).
// A liquid is also transparent (transmit > 0), so the shell, thickness and composite draw it;
// on top of that it is a GHOST: colliders, occupancy, contact refinement and island
// extraction treat it as air (the solid view, world/opaque_view.h). Only the transparency
// composite's shading branch asks WHICH liquid. A new liquid is one enum value, one name
// below, and its shading.
enum class Liquid : uint8_t { none = 0, water = 1 };

// GLSL constant names, indexed by the enum's value: LIQUID_<NAME>.
inline constexpr const char *kLiquidNames[] = {"none", "water"};
inline constexpr int kLiquidCount = static_cast<int>(sizeof(kLiquidNames) / sizeof(kLiquidNames[0]));
static_assert(kLiquidCount == static_cast<int>(Liquid::water) + 1, "a Liquid value has no name");
```

In `struct MaterialDef`, after `float ior = 1.0f; ...`:

```cpp
	Liquid liquid = Liquid::none; // which liquid shading and ghost rule apply; none = not a liquid
```

In `kMaterials`, after the `bark` row:

```cpp
	// Water (docs/superpowers/specs/2026-10-06-water-voxels-design.md §2). flat_albedo doubles
	// as the SCATTER colour, the body colour deep water fades to; it is also what water looks
	// like with transparency off. Red dies first: ~0.25 left after 2 m, blue ~0.77.
	{"water",        "08",  1.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.03f, 0.16f, 0.20f},
			{0.50f, 0.82f, 0.88f}, 1.33f, Liquid::water},
```

After `float material_ior(uint16_t id);`:

```cpp
// Fails soft like the rest: air, foliage and any id with no row are not liquids. Mirrored in
// GLSL as mat_liquid.
Liquid material_liquid(uint16_t id);
```

In `extension/src/world/material_table.cpp`, after `material_ior`:

```cpp
Liquid material_liquid(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	return (i >= 0 && i < kMaterialCount) ? kMaterials[i].liquid : Liquid::none;
}
```

In `material_table_glsl()`, immediately after the `MAT_IOR` block (the `o << ");\n\n";` that closes it) and before the foliage comment:

```cpp
	o << "// Liquids (ve::Liquid). A liquid is transparent AND a ghost: the solid view treats it\n"
	     "// as air. Only the transparency composite asks WHICH liquid.\n";
	for (int k = 0; k < kLiquidCount; k++)
		o << "const uint LIQUID_" << upper(kLiquidNames[k]) << " = " << k << "u;\n";
	o << "\n";

	o << "const uint MAT_LIQUID[MATERIAL_COUNT] = uint[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << static_cast<int>(kMaterials[i].liquid) << "u"
		  << (i + 1 < kMaterialCount ? "," : "") << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";
```

At the end of `material_table_glsl()`, replace the final string literal's closing (the `mat_ior` function ending `"}\n";`) so the emitted text continues with:

```cpp
	     "float mat_ior(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i >= 0 && i < MATERIAL_COUNT) ? MAT_IOR[i] : 1.0;\n"
	     "}\n\n"
	     "// Mirror of ve::material_liquid: air, foliage and any id with no row are not liquids.\n"
	     "uint mat_liquid(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i >= 0 && i < MATERIAL_COUNT) ? MAT_LIQUID[i] : LIQUID_NONE;\n"
	     "}\n";
```

In `tools/convert_materials.sh`, change the list line to:

```bash
MATERIALS=(grass_01 rock ground_01 breakstone ground_crack_01 ice_crack ice bark water)
```

and add above it, after the existing comment block:

```bash
# Like bark (tools/convert_bark.sh), water has no folder in the source pack: layer 08 is
# generated by tools/make_water_textures.gd. It is listed here because
# extension/tests/test_material_table.cpp asserts this list equals the table.
```

- [ ] **Step 4: Regenerate the GLSL mirror**

Run: `cd extension && scons -Q test 2>&1 | head -80`
Expected: only `the committed GLSL mirror matches the C++ table` fails, printing the expected file. Replace `shaders/material_table.glslh` with exactly that text (the printed block after "Replace its entire contents with:").

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`

- [ ] **Step 5: Write the texture generator**

Create `tools/make_water_textures.gd`:

```gdscript
extends SceneTree
# Writes water's atlas maps, assets/materials/08_*.png (docs/superpowers/specs/2026-10-06-
# water-voxels-design.md §2). Run once from the repo root; the outputs are committed:
#   godot --headless --path . -s res://tools/make_water_textures.gd
#
# 08_normal.png is a TILEABLE ripple normal map: a sum of sines whose wave vectors are whole
# numbers of cycles per tile, so it wraps seamlessly by construction. The liquid shading path
# samples only the normal; the other maps are flat so MaterialAtlas has every map it requires
# (it refuses to load a layer with one missing) and transparency-off water draws in its
# scatter colour.

const SIZE := 512
# [cycles_x, cycles_y, amplitude, phase]. Integer cycles keep the sum periodic on the tile;
# mixed directions and falling amplitudes keep it from reading as a grid.
const WAVES := [
	[3, 1, 1.00, 0.0], [-2, 3, 0.80, 1.3], [5, -2, 0.55, 2.1], [1, 6, 0.45, 0.4],
	[-7, -3, 0.30, 4.0], [9, 4, 0.22, 5.2], [-4, 11, 0.16, 0.9], [13, -6, 0.11, 3.3],
]
# Tangent-space slope at the steepest texel. The shader scales it again by wave_strength.
const SLOPE := 0.35

func _initialize() -> void:
	var dir := ProjectSettings.globalize_path("res://assets/materials/")
	_flat(Color(0.03, 0.16, 0.20)).save_png(dir + "08_basecolor.png")
	_flat(Color(0.06, 0.06, 0.06)).save_png(dir + "08_roughness.png")
	_flat(Color(1.0, 1.0, 1.0)).save_png(dir + "08_ambientOcclusion.png")
	_flat(Color(0.0, 0.0, 0.0)).save_png(dir + "08_height.png")
	_normal().save_png(dir + "08_normal.png")
	print("make_water_textures: wrote 08_*.png to %s" % dir)
	quit()

func _flat(c: Color) -> Image:
	var img := Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RGB8)
	img.fill(c)
	return img

func _normal() -> Image:
	var gx := PackedFloat32Array()
	var gy := PackedFloat32Array()
	gx.resize(SIZE * SIZE)
	gy.resize(SIZE * SIZE)
	var peak := 0.0
	for y in range(SIZE):
		for x in range(SIZE):
			var dx := 0.0
			var dy := 0.0
			for w in WAVES:
				var kx: float = TAU * float(w[0])
				var ky: float = TAU * float(w[1])
				var c: float = float(w[2]) * cos(kx * x / SIZE + ky * y / SIZE + float(w[3]))
				dx += c * kx # d/du of amplitude * sin(phase), u in [0, 1)
				dy += c * ky
			var i := y * SIZE + x
			gx[i] = dx
			gy[i] = dy
			peak = maxf(peak, sqrt(dx * dx + dy * dy))
	var img := Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RGB8)
	for y in range(SIZE):
		for x in range(SIZE):
			var i := y * SIZE + x
			var n := Vector3(-gx[i] / peak * SLOPE, -gy[i] / peak * SLOPE, 1.0).normalized()
			img.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5))
	return img
```

- [ ] **Step 6: Generate and inspect the textures**

Run: `godot --headless --path . -s res://tools/make_water_textures.gd`
Expected: `make_water_textures: wrote 08_*.png to .../assets/materials/`, and `ls assets/materials/08_*` lists five files.

Open `assets/materials/08_normal.png` with the Read tool and look at it: a soft blue-violet ripple pattern with no visible seam when you imagine it tiled (left edge continues into right edge, top into bottom).

- [ ] **Step 7: The atlas loads the new layer**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_material_atlas.gd 2>&1 | tail -5`
Expected: PASS (no `MaterialAtlas: failed to load` in the output).
Run: `./gdunit_tests.sh -a res://tests/test_material_picker.gd 2>&1 | tail -5`
Expected: PASS (the picker's entry count follows `material_table()`).

- [ ] **Step 8: Pins**

Run `tests/test_transparency.gd` and the golden suites exactly as Task 0 Step 4.
Expected: the same results as Task 0. Nothing places water yet, and layer 08 was an unused flat layer.

- [ ] **Step 9: Commit**

```bash
git add extension/src/world/material_table.h extension/src/world/material_table.cpp \
	shaders/material_table.glslh extension/tests/test_material_table.cpp \
	extension/tests/test_material_glslh.cpp tools/convert_materials.sh \
	tools/make_water_textures.gd assets/materials/08_*.png
git commit -m "feat: a water row and a liquid column, with a generated tileable ripple map"
```

---

### Task 2: The solid view on the CPU, and its CPU consumers

**Files:**
- Modify: `extension/src/generator/edit_ops.h`, `extension/src/generator/edit_ops.cpp`
- Modify: `extension/src/world/opaque_view.h`
- Modify: `extension/src/world/brick_eval.h`, `extension/src/world/brick_eval.cpp`
- Modify: `extension/src/connectivity/contact_refine.cpp`
- Modify: `extension/src/generator/volume_set.cpp`
- Modify: `extension/src/debug/hooks_physics.cpp`
- Create: `extension/tests/test_solid_view.cpp`

**Interfaces:**
- Consumes: `ve::material_liquid`, `ve::Liquid` (Task 1).
- Produces (C++, `ve`): `enum class AirRule : uint8_t { transparent, liquid }`; `apply_ops_pair(Sample*, Sample*, const EditOp*, int, float, float, float, const VolumeStore* = nullptr, AirRule = AirRule::transparent)`; `void solid_view(float *sdf, uint16_t *material, float outside = kOpaqueOutside)`; `Sample eval_field_solid(const Generator&, const EditOp*, int, float x, float y, float z, const VolumeStore* = nullptr, const OverrideSource* = nullptr)`. `cell_state_field`, `cell_state_probe`, `contact_samples_field` and `extract_island_volume` now read the solid view.

- [ ] **Step 1: Write the failing native tests**

Create `extension/tests/test_solid_view.cpp`:

```cpp
#include <doctest/doctest.h>
#include "connectivity/contact_refine.h"
#include "generator/edit_ops.h"
#include "generator/volume_set.h"
#include "world/brick_eval.h"
#include "world/material_table.h"
#include "world/opaque_view.h"
#include "analytic_oracle.h"
#include <algorithm>

// The SOLID VIEW (docs/superpowers/specs/2026-10-06-water-voxels-design.md §3): the world as
// colliders, occupancy, contact refinement and island extraction see it. A liquid is air; a
// transparent solid that is not a liquid (ice) stays solid.

namespace {

ve::EditOp sphere(uint32_t type, uint16_t material, const float c[3], float r) {
	ve::EditOp op{};
	op.type = type;
	op.material = material;
	op.pos[0] = c[0]; op.pos[1] = c[1]; op.pos[2] = c[2];
	op.radius = r;
	return op;
}

// A point one metre under the default world's surface at (20, *, 30): walking down from
// 120 m, the first sample whose field is at most -1.
void buried_point(const ve::Generator &gen, float out[3]) {
	out[0] = 20.0f; out[1] = 0.0f; out[2] = 30.0f;
	for (float y = 120.0f; y > -50.0f; y -= 0.05f)
		if (gen.sample(20.0f, y, 30.0f).sdf <= -1.0f) { out[1] = y; return; }
}

// The brick test_opaque_view.cpp uses as "all air": 80 m up, far above the meadow.
constexpr ve::IVec3 kSkyBrick{25, 100, 37};

void brick_centre(ve::IVec3 b, float out[3]) {
	ve::brick_world_origin(b, out);
	for (int a = 0; a < 3; a++) out[a] += 0.5f * ve::kBrickSize;
}

} // namespace

TEST_CASE("solid_view turns a liquid solid into just-outside and leaves ice and rock alone") {
	const uint16_t water = ve::material_id("water");
	const uint16_t ice = ve::material_id("ice");
	const uint16_t rock = ve::material_id("rock");
	float d = -0.3f;
	uint16_t m = water;
	ve::solid_view(&d, &m);
	CHECK(d == doctest::Approx(ve::kOpaqueOutside));
	CHECK(m == 0);
	d = -0.3f; m = ice;
	ve::solid_view(&d, &m);
	CHECK(d == doctest::Approx(-0.3f));
	CHECK(m == ice);
	d = -0.3f; m = rock;
	ve::solid_view(&d, &m);
	CHECK(d == doctest::Approx(-0.3f));
	CHECK(m == rock);
	// Air that merely carries a liquid label (a projected material) is untouched.
	d = 0.2f; m = water;
	ve::solid_view(&d, &m);
	CHECK(d == doctest::Approx(0.2f));
	CHECK(m == water);
}

// The pit defect the transparency branch measured, for the solid view: carving the union's
// winner would delete the ground a water ball merely overlaps, and the player would fall
// into ground that the marcher still draws.
TEST_CASE("a water add contributes nothing to the solid view; the union still holds water") {
	ve::AnalyticGenerator gen;
	float q[3];
	buried_point(gen, q);
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("water"), q, 4.0f);
	const ve::Sample bare = ve::eval_field(gen, nullptr, 0, q[0], q[1], q[2]);
	const ve::Sample solid = ve::eval_field_solid(gen, &add, 1, q[0], q[1], q[2]);
	const ve::Sample uni = ve::eval_field(gen, &add, 1, q[0], q[1], q[2]);
	CHECK(solid.sdf == bare.sdf);
	CHECK(solid.material == bare.material);
	CHECK(uni.material == ve::material_id("water"));
	CHECK(uni.sdf < 0.0f);
}

TEST_CASE("ice stays solid in the solid view") {
	ve::AnalyticGenerator gen;
	float q[3];
	buried_point(gen, q);
	q[1] += 3.0f; // above the ground, inside the ball: a sample only the ice makes solid
	const ve::EditOp add = sphere(ve::kOpSphereAdd, ve::material_id("ice"), q, 2.0f);
	const ve::Sample solid = ve::eval_field_solid(gen, &add, 1, q[0], q[1], q[2]);
	const ve::Sample uni = ve::eval_field(gen, &add, 1, q[0], q[1], q[2]);
	CHECK(solid.sdf == uni.sdf);
	CHECK(solid.material == ve::material_id("ice"));
	CHECK(solid.sdf < 0.0f);
}

// Review Focus 2: painted water relabels ground in place and must not stop it colliding.
TEST_CASE("water paint on the ground leaves the solid view untouched") {
	ve::AnalyticGenerator gen;
	float q[3];
	buried_point(gen, q);
	const ve::EditOp paint = sphere(ve::kOpSpherePaint, ve::material_id("water"), q, 2.0f);
	const ve::Sample bare = ve::eval_field(gen, nullptr, 0, q[0], q[1], q[2]);
	const ve::Sample solid = ve::eval_field_solid(gen, &paint, 1, q[0], q[1], q[2]);
	CHECK(solid.sdf == bare.sdf);
	CHECK(solid.material == bare.material);
	CHECK(ve::eval_field(gen, &paint, 1, q[0], q[1], q[2]).material == ve::material_id("water"));
}

TEST_CASE("with no liquid the solid view is the union, sample for sample") {
	ve::AnalyticGenerator gen;
	float q[3];
	buried_point(gen, q);
	const float up[3] = {q[0], q[1] + 1.5f, q[2]};
	const ve::EditOp ops[3] = {
		sphere(ve::kOpSphereAdd, ve::material_id("rock"), up, 1.2f),
		sphere(ve::kOpSphereSubtract, 0, q, 0.8f),
		sphere(ve::kOpSphereAdd, ve::material_id("ice"), up, 0.6f),
	};
	for (int z = -4; z <= 4; z++)
		for (int y = -4; y <= 4; y++)
			for (int x = -4; x <= 4; x++) {
				const float p[3] = {q[0] + x * 0.4f, q[1] + y * 0.4f, q[2] + z * 0.4f};
				const ve::Sample u = ve::eval_field(gen, ops, 3, p[0], p[1], p[2]);
				const ve::Sample s = ve::eval_field_solid(gen, ops, 3, p[0], p[1], p[2]);
				CHECK(s.sdf == u.sdf);
				CHECK(s.material == u.material);
			}
}

TEST_CASE("a brick inside a floating water ball is air for occupancy; inside ice it is full") {
	ve::AnalyticGenerator gen;
	float c[3];
	brick_centre(kSkyBrick, c);
	const ve::EditOp water = sphere(ve::kOpSphereAdd, ve::material_id("water"), c, 3.0f);
	const ve::EditOp ice = sphere(ve::kOpSphereAdd, ve::material_id("ice"), c, 3.0f);
	CHECK(ve::cell_state_field(gen, &water, 1, kSkyBrick) == ve::kCellAir);
	CHECK(ve::cell_state_probe(gen, &water, 1, kSkyBrick) == ve::kCellAir);
	CHECK(ve::cell_state_field(gen, &ice, 1, kSkyBrick) == ve::kCellFull);
	CHECK(ve::cell_state_probe(gen, &ice, 1, kSkyBrick) == ve::kCellFull);
}

TEST_CASE("a face buried in water has no contact; buried in ice it is all contact") {
	ve::AnalyticGenerator gen;
	float c[3];
	brick_centre(kSkyBrick, c);
	c[0] += 0.5f * ve::kBrickSize; // centred on the +x face of the brick
	const ve::EditOp water = sphere(ve::kOpSphereAdd, ve::material_id("water"), c, 3.0f);
	const ve::EditOp ice = sphere(ve::kOpSphereAdd, ve::material_id("ice"), c, 3.0f);
	CHECK(ve::contact_samples_field(gen, &water, 1, kSkyBrick, 0, 9) == 0);
	CHECK(ve::contact_samples_field(gen, &ice, 1, kSkyBrick, 0, 9) == 81);
}

TEST_CASE("island extraction strips water and keeps the rock") {
	ve::AnalyticGenerator gen;
	float c[3];
	brick_centre(kSkyBrick, c);
	const float cap[3] = {c[0], c[1] + 1.0f, c[2]};
	const ve::EditOp rock_only[1] = {sphere(ve::kOpSphereAdd, ve::material_id("rock"), c, 1.0f)};
	const ve::EditOp with_water[2] = {rock_only[0],
			sphere(ve::kOpSphereAdd, ve::material_id("water"), cap, 1.0f)};
	const float voxel = 0.15f;
	const int dim = 32; // 4.8 m: covers the rock ball and the water cap above it
	const float origin[3] = {c[0] - 2.4f, c[1] - 2.4f, c[2] - 2.4f};
	const float box[6] = {origin[0], origin[1], origin[2],
			origin[0] + dim * voxel, origin[1] + dim * voxel, origin[2] + dim * voxel};
	ve::VolumeData a, b;
	ve::extract_island_volume(gen, rock_only, 1, nullptr, nullptr, origin, voxel, dim, box, 1, &a);
	ve::extract_island_volume(gen, with_water, 2, nullptr, nullptr, origin, voxel, dim, box, 1, &b);
	CHECK(a.solid_voxels > 0);
	CHECK(b.solid_voxels == a.solid_voxels);
	const uint8_t water = static_cast<uint8_t>(ve::material_id("water"));
	CHECK(std::count(b.mat.begin(), b.mat.end(), water) == 0);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd extension && scons -Q test`
Expected: compile errors, `no member named 'solid_view'` and `no member named 'eval_field_solid'`.

- [ ] **Step 3: The air rule in the op evaluator**

In `extension/src/generator/edit_ops.h`, replace the `apply_ops_pair` declaration with:

```cpp
// Which materials the SECOND accumulator of apply_ops_pair treats as air. `transparent` is
// the OPAQUE VIEW, the world the marcher bakes (world/opaque_view.h). `liquid` is the SOLID
// VIEW (docs/superpowers/specs/2026-10-06-water-voxels-design.md §3), the world colliders,
// occupancy, contact refinement and island extraction see: ice stays solid, water is air.
// GLSL mirror: AIR_TRANSPARENT / AIR_LIQUID in shaders/field_ops.glslh.
enum class AirRule : uint8_t { transparent, liquid };

void apply_ops_pair(Sample *s, Sample *opaque, const EditOp *ops, int count, float x, float y,
		float z, const VolumeStore *volumes = nullptr, AirRule rule = AirRule::transparent);
```

In `extension/src/generator/edit_ops.cpp`, above `static void apply_op_pair`:

```cpp
static bool counts_as_air(uint16_t material, AirRule rule) {
	return rule == AirRule::liquid ? material_liquid(material) != Liquid::none
			: material_transparent(material);
}
```

Change `apply_op_pair`'s signature to take `AirRule rule` last:

```cpp
static void apply_op_pair(Sample *s, Sample *o, const EditOp &op, float x, float y, float z,
		const VolumeStore *volumes, AirRule rule) {
```

and replace its three conditions:
- `!material_transparent(static_cast<uint16_t>(op.material))` (in `kOpSphereAdd` and in `kOpSpherePaint`) with `!counts_as_air(static_cast<uint16_t>(op.material), rule)`;
- `!material_transparent(vs.material)` (in `kOpVolumeAdd`) with `!counts_as_air(vs.material, rule)`.

Replace `apply_ops_pair`'s definition with:

```cpp
void apply_ops_pair(Sample *s, Sample *opaque, const EditOp *ops, int count, float x, float y,
		float z, const VolumeStore *volumes, AirRule rule) {
	for (int i = 0; i < count; i++) apply_op_pair(s, opaque, ops[i], x, y, z, volumes, rule);
}
```

- [ ] **Step 4: The label rule and `eval_field_solid`**

In `extension/src/world/opaque_view.h`, after `opaque_view`:

```cpp
// The SOLID VIEW's label rule (docs/superpowers/specs/2026-10-06-water-voxels-design.md §3):
// a solid sample whose material is a LIQUID becomes just outside. Applied last, for a liquid
// that the base field or an override brick named; ops are handled inside the accumulator
// (ve::eval_field_solid). Mirror of solid_view() in shaders/field_ops.glslh.
inline void solid_view(float *sdf, uint16_t *material, float outside = kOpaqueOutside) {
	if (*sdf <= 0.0f && material_liquid(*material) != Liquid::none) {
		*sdf = outside;
		*material = 0;
	}
}
```

In `extension/src/world/brick_eval.h`, after `eval_field_pair`:

```cpp
// The SOLID VIEW of a point (docs/superpowers/specs/2026-10-06-water-voxels-design.md §3):
// the union with every liquid ADD/PAINT op and every liquid volume sample contributing
// nothing, then ve::solid_view for a liquid the base or an override named. Colliders,
// occupancy, contact refinement and island extraction read this; the edit raycast,
// consolidation and the near-shell candidate scan keep the union. With no liquid anywhere it
// IS the union, bit for bit. GLSL mirror: eval_field_solid() in shaders/field_ops.glslh.
Sample eval_field_solid(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, const VolumeStore *volumes = nullptr,
		const OverrideSource *overrides = nullptr);
```

Update the comments on `cell_state_field` and `eval_brick` in the same header: replace "the occupancy classification (cell_state_field) always reads the union" with "the occupancy classification (cell_state_field) reads the solid view", and on `cell_state_field` add the line `// Classified from the SOLID view: liquid is air to connectivity, ice is not.`

In `extension/src/world/brick_eval.cpp`, after `eval_field_pair`'s definition:

```cpp
Sample eval_field_solid(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z, const VolumeStore *volumes, const OverrideSource *overrides) {
	Sample s{};
	if (!overrides || !overrides->sample(x, y, z, &s)) s = gen.sample(x, y, z);
	Sample solid = s;
	apply_ops_pair(&s, &solid, ops, op_count, x, y, z, volumes, AirRule::liquid);
	solid_view(&solid.sdf, &solid.material);
	return solid;
}
```

- [ ] **Step 5: Occupancy from the solid view**

In `brick_eval.cpp`, replace `brick_probe` (the anonymous-namespace function above `eval_field`) with:

```cpp
// The 3^3 activation probe, reduced. brick_has_surface and cell_state_probe read it,
// and shaders/brick_mark.comp.glsl computes exactly this once per brick and uses it twice.
// `smn`/`smx` are the SOLID view's range of the same 27 points (occupancy's input).
void brick_probe(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		const VolumeStore *volumes, const OverrideSource *overrides, float *mn, float *mx,
		float *omn = nullptr, float *omx = nullptr, float *smn = nullptr, float *smx = nullptr) {
	const std::vector<EditOp> kept = ops_for_brick(ops, op_count, brick);
	const EditOp *filtered = kept.data();
	const int filtered_count = static_cast<int>(kept.size());
	float bo[3];
	brick_world_origin(brick, bo);
	*mn = 1e30f;
	*mx = -1e30f;
	if (omn) *omn = 1e30f;
	if (omx) *omx = -1e30f;
	if (smn) *smn = 1e30f;
	if (smx) *smx = -1e30f;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				const float p[3] = {bo[0] + sx * (kBrickVoxels / 2) * kVoxelSize,
						bo[1] + sy * (kBrickVoxels / 2) * kVoxelSize,
						bo[2] + sz * (kBrickVoxels / 2) * kVoxelSize};
				Sample s{}, o{};
				eval_field_pair(gen, filtered, filtered_count, p[0], p[1], p[2], &s, &o, volumes,
						overrides);
				*mn = std::min(*mn, s.sdf);
				*mx = std::max(*mx, s.sdf);
				if (smn && smx) {
					const Sample q = eval_field_solid(gen, filtered, filtered_count, p[0], p[1],
							p[2], volumes, overrides);
					*smn = std::min(*smn, q.sdf);
					*smx = std::max(*smx, q.sdf);
				}
				if (!omn || !omx) continue;
				opaque_view(&o.sdf, &o.material);
				*omn = std::min(*omn, o.sdf);
				*omx = std::max(*omx, o.sdf);
			}
}
```

`eval_field_solid` is defined later in the file; add its forward use by moving nothing: it is declared in `brick_eval.h`, which this file includes, so the call compiles.

Replace `cell_state_probe` and `cell_state_field` with:

```cpp
CellState cell_state_probe(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		const VolumeStore *volumes, const OverrideSource *overrides) {
	float mn = 0.0f, mx = 0.0f, smn = 0.0f, smx = 0.0f;
	brick_probe(gen, ops, op_count, cell, volumes, overrides, &mn, &mx, nullptr, nullptr, &smn,
			&smx);
	if (smn > 0.0f) return kCellAir;
	return smx <= 0.0f ? kCellFull : kCellSolid;
}

// Mirror of brick_gen.comp.glsl's classification: the encoded SOLID-view lattice over the
// brick's 17^3 samples, with the brick's filtered ops. With no liquid this is exactly the
// union lattice eval_brick(..., false) stores, byte for byte.
CellState cell_state_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		const VolumeStore *volumes, const OverrideSource *overrides) {
	float bo[3];
	brick_world_origin(cell, bo);
	const std::vector<EditOp> kept = ops_for_brick(ops, op_count, cell);
	uint8_t mn = 255u, mx = 0u;
	for (int vz = 0; vz < kBrickSdfStride; vz++)
		for (int vy = 0; vy < kBrickSdfStride; vy++)
			for (int vx = 0; vx < kBrickSdfStride; vx++) {
				const uint8_t e = encode_sdf(eval_field_solid(gen, kept.data(),
						static_cast<int>(kept.size()), bo[0] + vx * kVoxelSize,
						bo[1] + vy * kVoxelSize, bo[2] + vz * kVoxelSize, volumes, overrides).sdf);
				mn = std::min(mn, e);
				mx = std::max(mx, e);
			}
	const uint8_t zero = encode_sdf(0.0f);
	if (mn > zero) return kCellAir;
	return mx <= zero ? kCellFull : kCellSolid;
}
```

- [ ] **Step 6: Contact, island extraction and the lattice reference**

In `extension/src/connectivity/contact_refine.cpp`, in `contact_samples_field`, replace

```cpp
			if (eval_field(gen, ops, op_count, p[0], p[1], p[2], volumes, overrides).sdf <= 0.0f) solid++;
```

with

```cpp
			// The solid view: liquid is air to connectivity (water spec §3), so a face held up
			// only by water is no contact at all.
			if (eval_field_solid(gen, ops, op_count, p[0], p[1], p[2], volumes, overrides).sdf <= 0.0f)
				solid++;
```

In `extension/src/generator/volume_set.cpp`, in `extract_island_volume`:

Replace the `masked` lambda and add `masked_union` beside it:

```cpp
	// The island IS the solid field intersected with the union of its cells, so the mask is
	// a CSG intersection: max(field, min over boxes). The field is the SOLID view (water spec
	// §3): an island never carries liquid.
	const auto box_union = [&](const float p[3]) {
		float bu = 1e30f;
		for (int b = 0; b < box_count; b++)
			bu = std::min(bu, box_sdf(&box_aabbs[static_cast<size_t>(b) * 6 + 0],
							 &box_aabbs[static_cast<size_t>(b) * 6 + 3], p[0], p[1], p[2]));
		return bu;
	};
	const auto masked = [&](const float p[3], uint16_t *material) {
		const Sample s = eval_field_solid(gen, ops, op_count, p[0], p[1], p[2], volumes, overrides);
		if (material) *material = s.material;
		return std::max(s.sdf, box_union(p));
	};
	// The same mask over the UNION. Where it differs from `masked` there was liquid at or
	// beside the voxel, and the union gradient below belongs to the liquid.
	const auto masked_union = [&](const float p[3]) {
		return std::max(eval_field(gen, ops, op_count, p[0], p[1], p[2], volumes, overrides).sdf,
				box_union(p));
	};
```

In the projection loop, replace `material = eval_field(gen, ops, op_count, ...` with `material = eval_field_solid(gen, ops, op_count, ...` (same arguments).

Replace

```cpp
				out->normal_oct[static_cast<size_t>(i)] = oct_encode_snorm8(grad);
```

with

```cpp
				// 0x8080 is the "no stored normal" marker island_extract.comp.glsl writes for an
				// inexact gradient; the marcher then shades from the R8 lattice. Mirror of the
				// shader's union-versus-solid rule.
				out->normal_oct[static_cast<size_t>(i)] = masked_union(p) != d
						? static_cast<uint16_t>(0x8080u) : oct_encode_snorm8(grad);
```

In `extension/src/debug/hooks_physics.cpp`, `debug_mesh_lattice_diff`, replace `ve::eval_field(gen, snap.ops.data(), ...` with `ve::eval_field_solid(gen, snap.ops.data(), ...` (same arguments). The collision lattice is the solid view from Task 3 on; its CPU reference must be too.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`, including every case in `test_solid_view.cpp`, and every existing case in `test_opaque_view.cpp`, `test_occupancy.cpp`, `test_contact_refine.cpp` and `test_volume_ops.cpp` unchanged.

- [ ] **Step 8: Commit**

```bash
git add extension/src/generator/edit_ops.h extension/src/generator/edit_ops.cpp \
	extension/src/world/opaque_view.h extension/src/world/brick_eval.h \
	extension/src/world/brick_eval.cpp extension/src/connectivity/contact_refine.cpp \
	extension/src/generator/volume_set.cpp extension/src/debug/hooks_physics.cpp \
	extension/tests/test_solid_view.cpp
git commit -m "feat: a solid view in which liquid is air, read by occupancy, contact and islands"
```

---

### Task 3: The solid view on the GPU

**Files:**
- Modify: `shaders/field_ops.glslh`
- Modify: `shaders/generated/field.glslh.golden` (regenerated)
- Modify: `shaders/brick_gen.comp.glsl`, `shaders/brick_mark.comp.glsl`, `shaders/mesh_field.comp.glsl`, `shaders/island_extract.comp.glsl`
- Create: `tests/test_water_ghost.gd`

**Interfaces:**
- Consumes: `mat_liquid`, `LIQUID_NONE` (Task 1); the CPU references from Task 2.
- Produces (GLSL, wherever `field.glslh` is included): `AIR_TRANSPARENT`, `AIR_LIQUID`, `bool counts_as_air(uint m, uint rule)`, `void apply_field_op_rule(uint index, vec3 p, inout float sdf, inout uint mat, inout float osdf, inout uint omat, uint rule)`, `void solid_view(inout float sdf, inout uint mat)`, `void eval_field_solid(vec3 p, uint op_base, uint op_count, out float sdf, out uint mat)`, `bool op_may_hold_liquid(uint index)`. `apply_field_op` keeps its six-argument signature.

- [ ] **Step 1: Write the failing GPU tests**

Create `tests/test_water_ghost.gd`:

```gdscript
extends GdUnitTestSuite

# Ghost water on the GPU (docs/superpowers/specs/2026-10-06-water-voxels-design.md §3): the
# occupancy grid, the collision lattice and island extraction read the SOLID view, in which a
# liquid is air. Each GPU product is diffed against its CPU reference with water present,
# and checked against the claim itself.

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(physics := false) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	if physics:
		assert_bool(w.hooks().debug_init_physics()).is_true()
	else:
		w.ensure_initialized()
	return w

func material_id(w: VoxelWorld, name: String) -> int:
	for m in w.material_table():
		if m["name"] == name:
			return m["id"]
	return 0

# A point in region (0, 2, 0), well above the meadow: cell (30, 84, 30).
const SKY := Vector3(24.4, 67.6, 24.4)

func stream_and_pump(w: VoxelWorld, at: Vector3) -> void:
	w.hooks().debug_stream_region(Vector3i(0, 2, 0))
	for i in range(16):
		w.hooks().debug_stream_frame(at)
	w.hooks().debug_pump_occupancy()

func test_a_floating_water_ball_is_air_in_the_occupancy_grid(timeout := 60000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(SKY, 1.5, material_id(w, "water"))
	stream_and_pump(w, SKY)
	var cell := Vector3i(30, 84, 30)
	assert_int(w.hooks().debug_cell_state(cell)).is_equal(1)     # kCellAir, the CPU rule
	assert_int(int(w.hooks().debug_occupancy_state(cell))).is_equal(1)

func test_a_floating_ice_ball_stays_full_in_the_occupancy_grid(timeout := 60000) -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(SKY, 1.5, material_id(w, "ice"))
	stream_and_pump(w, SKY)
	var cell := Vector3i(30, 84, 30)
	assert_int(w.hooks().debug_cell_state(cell)).is_equal(3)     # kCellFull
	assert_int(int(w.hooks().debug_occupancy_state(cell))).is_equal(3)

func test_gpu_occupancy_matches_the_cpu_rule_with_water_in_the_region(timeout := 60000) -> void:
	var w := make_world()
	var water := material_id(w, "water")
	w.hooks().debug_apply_sphere_add(SKY, 1.5, water)
	# Water over the meadow too: the bricks there hold ground and water, so the gated second
	# evaluation in brick_gen runs on bricks that also have a real surface.
	w.hooks().debug_apply_sphere_add(Vector3(24.4, 51.4, 24.4), 1.5, water)
	stream_and_pump(w, Vector3(24.4, 51.4, 24.4))
	var d: Dictionary = w.hooks().debug_occupancy_diff(Vector3i(0, 2, 0))
	assert_int(int(d["compared"])).is_greater(100)
	assert_int(int(d["mismatches"])).override_failure_message(
		"GPU occupancy disagrees with cell_state_field: %s" % d).is_equal(0)

# Chunk (3, 10, 3) spans y [64, 70.4) above the meadow: no terrain surface.
const SKY_CHUNK := Vector3i(3, 10, 3)

func test_a_water_ball_puts_no_surface_in_the_collision_lattice(timeout := 60000) -> void:
	var w := make_world(true)
	w.hooks().debug_apply_sphere_add(SKY, 1.5, material_id(w, "water"))
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(SKY_CHUNK)
	assert_int(int(d["max_diff"])).is_less_equal(1)
	assert_bool(d["has_surface"]).override_failure_message(
		"the collision lattice sees the water ball: %s" % d).is_false()

func test_a_rock_ball_does_put_a_surface_in_the_collision_lattice(timeout := 60000) -> void:
	# The control: the same ball in rock is a collider, so the test above can see a ball at all.
	var w := make_world(true)
	w.hooks().debug_apply_sphere_add(SKY, 1.5, material_id(w, "rock"))
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(SKY_CHUNK)
	assert_int(int(d["max_diff"])).is_less_equal(1)
	assert_bool(d["has_surface"]).is_true()

# Cells covering a rock ball at SKY and a water cap above it.
const ISLAND_LO := Vector3i(29, 83, 29)
const ISLAND_HI := Vector3i(31, 87, 31)

func test_island_extraction_strips_water_and_matches_the_cpu(timeout := 60000) -> void:
	var rock_world := make_world(true)
	rock_world.hooks().debug_apply_sphere_add(SKY, 1.0, material_id(rock_world, "rock"))
	var a: Dictionary = rock_world.hooks().debug_island_extract_diff(ISLAND_LO, ISLAND_HI)
	var wet_world := make_world(true)
	wet_world.hooks().debug_apply_sphere_add(SKY, 1.0, material_id(wet_world, "rock"))
	wet_world.hooks().debug_apply_sphere_add(SKY + Vector3(0, 1.0, 0), 1.0,
			material_id(wet_world, "water"))
	var b: Dictionary = wet_world.hooks().debug_island_extract_diff(ISLAND_LO, ISLAND_HI)
	assert_bool(b.get("ok", false)).is_true()
	assert_int(int(b["worst_steps"])).is_less(2)
	assert_int(int(b["mat_mismatch"])).is_equal(0)
	assert_int(int(b["gpu_solid"])).is_equal(int(b["cpu_solid"]))
	# The water contributed no solid voxel: the island is the rock alone.
	assert_int(int(b["gpu_solid"])).override_failure_message(
		"the island carries water: %s vs rock alone %s" % [b, a]).is_equal(int(a["gpu_solid"]))
```

- [ ] **Step 2: Run them to verify they fail**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_water_ghost.gd 2>&1 | tail -30`
Expected: `test_a_floating_water_ball_is_air_in_the_occupancy_grid` fails (GPU state 3, CPU 1), `test_gpu_occupancy_matches_the_cpu_rule_with_water_in_the_region` reports mismatches, `test_a_water_ball_puts_no_surface_in_the_collision_lattice` fails on `max_diff` (GPU union against the CPU solid reference), and the island test fails on `gpu_solid != cpu_solid`. The ice and rock controls pass.

- [ ] **Step 3: The rule, the solid view and the gate in `field_ops.glslh`**

In `shaders/field_ops.glslh`, immediately above the comment that starts `// One op applied to BOTH accumulators in one pass.`:

```glsl
// Which materials the SECOND accumulator treats as air. AIR_TRANSPARENT is the opaque view
// (the marcher's world, transparent-voxels spec §3); AIR_LIQUID is the SOLID view
// (colliders, occupancy, contact refinement, island extraction: water spec §3), in which ice
// stays solid and a liquid is air. Mirror of ve::AirRule.
const uint AIR_TRANSPARENT = 0u;
const uint AIR_LIQUID = 1u;

bool counts_as_air(uint m, uint rule) {
	return rule == AIR_LIQUID ? mat_liquid(m) != LIQUID_NONE : mat_transparent(m);
}
```

Rename `void apply_field_op(uint index, vec3 p, inout float sdf, inout uint mat, inout float osdf, inout uint omat)` to:

```glsl
void apply_field_op_rule(uint index, vec3 p, inout float sdf, inout uint mat,
		inout float osdf, inout uint omat, uint rule) {
```

and inside it replace `!mat_transparent(vm)` with `!counts_as_air(vm, rule)`, and both `!mat_transparent(material)` with `!counts_as_air(material, rule)`. Directly after the function's closing brace add:

```glsl
// The opaque view's op, unchanged for every caller that names no rule.
void apply_field_op(uint index, vec3 p, inout float sdf, inout uint mat,
		inout float osdf, inout uint omat) {
	apply_field_op_rule(index, p, sdf, mat, osdf, omat, AIR_TRANSPARENT);
}
```

After `eval_field`'s definition (the union-only wrapper) add:

```glsl
// Mirror of ve::solid_view: a solid sample whose material is a liquid becomes just outside.
void solid_view(inout float sdf, inout uint mat) {
	if (sdf <= 0.0 && mat_liquid(mat) != LIQUID_NONE) {
		sdf = 0.5 * VOXEL_SIZE;
		mat = 0u;
	}
}

// The SOLID VIEW of a point (water spec §3): the union with every liquid ADD/PAINT op and
// every liquid volume sample contributing nothing, then the label rule for a liquid the base
// or an override named. Mirror of ve::eval_field_solid. The union half of the loop is dead
// and the compiler drops it.
void eval_field_solid(vec3 p, uint op_base, uint op_count, out float sdf, out uint mat) {
	float usdf;
	uint umat;
#ifdef FIELD_OVERRIDE_SDF_BINDING
	if (!sample_field_override(p, op_base, usdf, umat)) eval_base_field(p, usdf, umat);
#else
	eval_base_field(p, usdf, umat);
#endif
	sdf = usdf;
	mat = umat;
	for (uint i = 0u; i < op_count; i++)
		apply_field_op_rule(FIELD_OP_INDEX(op_base, i), p, usdf, umat, sdf, mat, AIR_LIQUID);
	solid_view(sdf, mat);
}

// True when op `index` can put a liquid into a sample: a liquid ADD or PAINT, or any volume
// (whose bytes are only known per sample). A bake site that reduces the solid view skips the
// second evaluation when none of its ops can make it differ from the union.
bool op_may_hold_liquid(uint index) {
	uvec4 a = field_op_pool.v[index * 2u + 0u];
	if (a.x == OP_VOLUME_ADD) return true;
	return (a.x == OP_SPHERE_ADD || a.x == OP_SPHERE_PAINT) && mat_liquid(a.y) != LIQUID_NONE;
}
```

- [ ] **Step 4: Regenerate the field golden**

Run: `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc="the default pipeline generates the committed source" && cd .. && git diff --stat shaders/generated/field.glslh.golden`
(The first `scons -Q test` rebuilds the binary and is expected to fail on that one golden.)
Expected: the golden changes, and `git diff shaders/generated/field.glslh.golden` shows exactly the `field_ops.glslh` edits above and nothing else.

- [ ] **Step 5: Brick generation classifies occupancy from the solid view**

In `shaders/brick_gen.comp.glsl`:

Add `shared uint s_liquid;` after `shared uint s_keep[256];`.

Replace the `s_umin` / `s_umax` comment with:

```glsl
// Encoded min / max of the SOLID view (water spec §3): the occupancy grid is classified from
// these, not from the stored (opaque-view) lattice -- connectivity keeps seeing ice as solid
// and sees a liquid as air. With no liquid they are the union lattice's range.
```

In `main()`, replace the serial compaction block

```glsl
	if (tid == 0u) {
		uint n = 0u;
		for (uint i = 0u; i < op_count; i++)
			if (s_keep[i] != 0u) s_ops[n++] = i;
		s_op_n = n;
	}
```

with

```glsl
	if (tid == 0u) {
		uint n = 0u;
		uint liquid = 0u;
		for (uint i = 0u; i < op_count; i++)
			if (s_keep[i] != 0u) {
				s_ops[n++] = i;
				if (op_may_hold_liquid(op_base + i)) liquid = 1u;
			}
		s_op_n = n;
		s_liquid = liquid;
	}
```

Add this function after `cell_coord`:

```glsl
// The solid view's distance at a lattice point whose union sample is (sdf, mat). Without a
// liquid-capable op the solid view IS the union under the label rule, so only bricks such an
// op reaches pay for a second evaluation. Mirror of ve::cell_state_field.
float solid_sample(vec3 p, uint op_base, float sdf, uint mat) {
	float ssdf = sdf;
	uint smat = mat;
	if (s_liquid != 0u) eval_field_solid(p, op_base, s_op_n, ssdf, smat);
	else solid_view(ssdf, smat);
	return ssdf;
}
```

In phase 1a and in phase 1b, replace

```glsl
		uint ub = encode_sdf_byte(sdf);
```

with

```glsl
		uint ub = encode_sdf_byte(solid_sample(bo + vec3(v) * VOXEL_SIZE, op_base, sdf, mat));
```

- [ ] **Step 6: The brick-mark fallback reads the solid view**

In `shaders/brick_mark.comp.glsl`:

Add `shared uint s_liquid;` after `shared uint s_keep[256];`.

Replace `brick_probe` with:

```glsl
// Mirror of ve::brick_probe (extension/src/world/brick_eval.cpp): the union range, the range
// of the opaque view of the same 27 samples, and the SOLID view's minimum (occupancy's input).
void brick_probe(ivec3 brick, uint op_base, uint op_count, out float mn, out float mx,
		out float omn, out float omx, out float smn) {
	vec3 bo = vec3(brick) * BRICK_SIZE;
	mn = 1e30;
	mx = -1e30;
	omn = 1e30;
	omx = -1e30;
	smn = 1e30;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				vec3 p = bo + vec3(sx, sy, sz) * (float(BRICK_VOXELS) * 0.5 * VOXEL_SIZE);
				float sdf;
				uint mat;
				float osdf;
				uint omat;
				eval_field_pair(p, op_base, op_count, sdf, mat, osdf, omat);
				mn = min(mn, sdf);
				mx = max(mx, sdf);
				float ssdf = sdf;
				uint smat = mat;
				if (s_liquid != 0u) eval_field_solid(p, op_base, op_count, ssdf, smat);
				else solid_view(ssdf, smat);
				smn = min(smn, ssdf);
				opaque_view(osdf, omat, OPAQUE_OUTSIDE);
				omn = min(omn, osdf);
				omx = max(omx, osdf);
			}
}
```

Replace the compaction block in `main()`:

```glsl
	if (gl_LocalInvocationID.x == 0u) {
		uint n = 0u;
		uint liquid = 0u;
		for (uint oi = 0u; oi < op_count; oi++)
			if (s_keep[oi] != 0u) {
				s_ops[n++] = oi;
				if (op_may_hold_liquid(op_base + oi)) liquid = 1u;
			}
		s_op_n = n;
		s_liquid = liquid;
	}
```

Replace

```glsl
	float probe_mn, probe_mx, opaque_mn, opaque_mx;
	brick_probe(brick, op_base, s_op_n, probe_mn, probe_mx, opaque_mn, opaque_mx);
```

with

```glsl
	float probe_mn, probe_mx, opaque_mn, opaque_mx, solid_mn;
	brick_probe(brick, op_base, s_op_n, probe_mn, probe_mx, opaque_mn, opaque_mx, solid_mn);
```

and the fallback write

```glsl
		write_occupancy(rslot, bi, probe_mn > 0.0 ? CELL_AIR : CELL_FULL);
```

with

```glsl
		write_occupancy(rslot, bi, solid_mn > 0.0 ? CELL_AIR : CELL_FULL);
```

- [ ] **Step 7: Colliders and island extraction**

In `shaders/mesh_field.comp.glsl`, replace `eval_field(lattice_world_pos(l), ...` with `eval_field_solid(lattice_world_pos(l), ...` (same arguments), and replace the comment `// the mesher has no use for materials; collision carries none` with `// the mesher has no use for materials; collision carries none. The SOLID view: a liquid is no collider (water spec §3).`

In `shaders/island_extract.comp.glsl`:

Replace `masked_field` with:

```glsl
float box_union(vec3 p) {
	float bu = 1e30;
	for (int i = 0; i < pc.params.z; i++)
		bu = min(bu, op_box_sdf(boxes.v[i * 2 + 0].xyz, boxes.v[i * 2 + 1].xyz, p));
	return bu;
}

// Mirror of ve::extract_island_volume's `masked` lambda: the island IS the solid field
// intersected with the union of its 0.8 m cells, which is max(field, min over boxes). The
// field is the SOLID view (water spec §3): an island never carries liquid. A component with
// no boxes extracts to nothing, which is the correct answer and not a special case.
float masked_field(vec3 p, out uint mat) {
	float sdf;
	eval_field_solid(p, 0u, uint(pc.params.y), sdf, mat);
	return max(sdf, box_union(p));
}

// The same mask over the UNION. Where it differs from masked_field there was liquid at or
// beside the voxel. Mirror of extract_island_volume's `masked_union`.
float masked_union(vec3 p) {
	float sdf;
	uint mat;
	eval_field(p, 0u, uint(pc.params.y), sdf, mat);
	return max(sdf, box_union(p));
}
```

In `main()`'s projection loop, replace `eval_field(p - g / len * t, 0u, uint(pc.params.y), ignored_sdf, mat);` with `eval_field_solid(p - g / len * t, 0u, uint(pc.params.y), ignored_sdf, mat);`.

Immediately after the `masked_field_gradient(p, grad_sdf, grad_mat, gradient, exact_gradient);` call, add:

```glsl
	// The union gradient belongs to the liquid where the union and the solid view disagree;
	// store "no normal" and let the R8 lattice shade the voxel (water spec §3).
	if (masked_union(p) != sdf) exact_gradient = false;
```

- [ ] **Step 8: Run the GPU tests to verify they pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_water_ghost.gd 2>&1 | tail -20`
Expected: all six cases PASS.

- [ ] **Step 9: The differential suites and the pins still hold**

Run each, expecting PASS (or the Task 0 baseline result for that suite):

```bash
./gdunit_tests.sh -a res://tests/test_occupancy_lattice.gd
./gdunit_tests.sh -a res://tests/test_mesh_lattice.gd
./gdunit_tests.sh -a res://tests/test_island_extract.gd
./gdunit_tests.sh -a res://tests/test_brick_diff.gd
./gdunit_tests.sh -a res://tests/test_connectivity.gd
```

Then the pins exactly as Task 0 Step 4. Expected: unchanged from Task 0.

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`

- [ ] **Step 10: Commit**

```bash
git add shaders/field_ops.glslh shaders/generated/field.glslh.golden shaders/brick_gen.comp.glsl \
	shaders/brick_mark.comp.glsl shaders/mesh_field.comp.glsl shaders/island_extract.comp.glsl \
	tests/test_water_ghost.gd
git commit -m "feat: colliders, occupancy and island extraction read the solid view on the GPU"
```

---

### Task 4: The `water` settings module

**Files:**
- Create: `extension/src/water/water_settings.h`, `extension/src/water/water_settings.cpp`, `extension/src/water/water_settings_store.h`
- Create: `extension/tests/test_water_settings.cpp`
- Modify: `extension/SConstruct`
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Modify: `extension/src/voxel_settings.h`, `extension/src/voxel_settings.cpp`
- Modify: `tests/test_voxel_settings.gd` (and any other test listing the settings groups)

**Interfaces:**
- Produces: `ve::WaterSettings { float wave_strength = 1.0f; float flow_speed = 0.6f; float refraction_strength = 0.03f; float foam_width_m = 0.3f; }`, `ve::water_rows()`, `ve::clamp_water_settings(WaterSettings*)`, `ve::WaterSettingsStore`, `RenderOrchestrator::water_settings()`, `set_water_value(const char*, float)`, `water_value(const char*)`, settings group `"water"`, GDScript `VoxelWorld.set_water_value(name, value)` / `get_water_value(name)`.

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_water_settings.cpp`:

```cpp
#include <doctest/doctest.h>
#include "water/water_settings.h"
#include "water/water_settings_store.h"

TEST_CASE("water defaults are the spec's and sit inside their own clamp") {
	ve::WaterSettings s;
	CHECK(s.wave_strength == doctest::Approx(1.0f));
	CHECK(s.flow_speed == doctest::Approx(0.6f));
	CHECK(s.refraction_strength == doctest::Approx(0.03f));
	CHECK(s.foam_width_m == doctest::Approx(0.3f));
	ve::WaterSettings c = s;
	ve::clamp_water_settings(&c);
	CHECK(c.wave_strength == doctest::Approx(s.wave_strength));
	CHECK(c.flow_speed == doctest::Approx(s.flow_speed));
	CHECK(c.refraction_strength == doctest::Approx(s.refraction_strength));
	CHECK(c.foam_width_m == doctest::Approx(s.foam_width_m));
}

// A NaN or a negative must not reach the shader: a negative flow would run water uphill and a
// huge refraction offset samples pixels from the far side of the screen.
TEST_CASE("water clamp keeps every knob in range") {
	ve::WaterSettings s;
	s.wave_strength = 0.0f / 0.0f;
	s.flow_speed = -2.0f;
	s.refraction_strength = 9.0f;
	s.foam_width_m = -1.0f;
	ve::clamp_water_settings(&s);
	CHECK(s.wave_strength >= 0.0f);
	CHECK(s.wave_strength <= 3.0f);
	CHECK(s.flow_speed == doctest::Approx(0.0f));
	CHECK(s.refraction_strength <= 0.2f);
	CHECK(s.foam_width_m == doctest::Approx(0.0f));
}

TEST_CASE("the water store round-trips every knob and clamps on the way in") {
	ve::WaterSettingsStore store;
	CHECK(store.set_value("wave_strength", 2.0f));
	CHECK(store.value("wave_strength") == doctest::Approx(2.0f));
	CHECK(store.set_value("flow_speed", 1.5f));
	CHECK(store.get().flow_speed == doctest::Approx(1.5f));
	CHECK(store.set_value("refraction_strength", 1e9f));
	CHECK(store.value("refraction_strength") <= 0.2f);
	CHECK(store.set_value("foam_width_m", 0.0f));
	CHECK(store.get().foam_width_m == doctest::Approx(0.0f));
	CHECK_FALSE(store.set_value("min_transmit", 0.5f)); // transparency's knob, not water's
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons -Q test`
Expected: compile error, `'water/water_settings.h' file not found`.

- [ ] **Step 3: Write the module**

`extension/src/water/water_settings.h`:

```cpp
#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Liquid knobs (docs/superpowers/specs/2026-10-06-water-voxels-design.md §8). DELIBERATELY
// its own module and store, like transparency, grass and leaves: nothing here joins
// BeautySettings or TransparencySettings. What a liquid lets through is a material-table
// column; these are the global look knobs while water is the only liquid. All are read every
// frame.
struct WaterSettings {
	float wave_strength = 1.0f;       // tilt multiplier on the wave normal; 0 = mirror-flat
	float flow_speed = 0.6f;          // m/s down sloped and vertical faces; the top drift is fixed
	float refraction_strength = 0.03f; // screen-uv offset at full tilt; 0 = no refraction
	float foam_width_m = 0.3f;        // water depth where shore foam fades out; 0 = no foam
};

std::span<const SettingRow<WaterSettings>> water_rows();

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_water_settings(WaterSettings *s);

} // namespace ve
```

`extension/src/water/water_settings.cpp`:

```cpp
#include "water/water_settings.h"

namespace ve {
namespace {

const SettingRow<WaterSettings> kWaterRows[] = {
	float_row("wave_strength", "Wave strength", &WaterSettings::wave_strength, 0.0f, 3.0f,
			0.0f, 3.0f, 0.05f),
	float_row("flow_speed", "Flow speed (m/s)", &WaterSettings::flow_speed, 0.0f, 5.0f,
			0.0f, 3.0f, 0.05f),
	float_row("refraction_strength", "Refraction", &WaterSettings::refraction_strength, 0.0f,
			0.2f, 0.0f, 0.1f, 0.005f),
	float_row("foam_width_m", "Foam width (m)", &WaterSettings::foam_width_m, 0.0f, 3.0f,
			0.0f, 1.5f, 0.05f),
};

} // namespace

std::span<const SettingRow<WaterSettings>> water_rows() { return kWaterRows; }

void clamp_water_settings(WaterSettings *s) {
	clamp_all<WaterSettings>(kWaterRows, s);
}

} // namespace ve
```

`extension/src/water/water_settings_store.h`:

```cpp
#pragma once
#include "water/water_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Water is its own module with its own store, mirroring TransparencySettingsStore without
// joining it. No godot-cpp -- this file is in the native test target.
class WaterSettingsStore : public SettingsStore<WaterSettings> {
public:
	WaterSettingsStore() : SettingsStore(water_rows(), nullptr, WaterSettings{}) {}
};

} // namespace ve
```

In `extension/SConstruct`, in `pure_sources`, change `Glob("src/transparency/*.cpp") + Glob("src/gpu_layout/*.cpp") +` to `Glob("src/transparency/*.cpp") + Glob("src/water/*.cpp") + Glob("src/gpu_layout/*.cpp") +`. (The shared library's `Glob("src/*/*.cpp")` already picks the module up.)

- [ ] **Step 4: Run it to verify it passes**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`

- [ ] **Step 5: Register the group**

`extension/src/render/orchestrator.h`: add `#include "water/water_settings_store.h"` after the transparency store include; beside the three transparency accessors add

```cpp
	ve::WaterSettings water_settings() const { return water_settings_.get(); }
	bool set_water_value(const char *n, float v) { return water_settings_.set_value(n, v); }
	float water_value(const char *n) const { return water_settings_.value(n); }
```

and beside `ve::TransparencySettingsStore transparency_settings_;` add `ve::WaterSettingsStore water_settings_;`.

`extension/src/render/orchestrator.cpp`, in `settings_group`, after the transparency line:

```cpp
	if (std::strcmp(name, "water") == 0) return &water_settings_;
```

`extension/src/voxel_world.h`, after `get_transparency_value`:

```cpp
	bool set_water_value(const String &name, float v);
	float get_water_value(const String &name) const;
```

`extension/src/voxel_world.cpp`: after the two transparency `bind_method` lines,

```cpp
	ClassDB::bind_method(D_METHOD("set_water_value", "name", "value"), &VoxelWorld::set_water_value);
	ClassDB::bind_method(D_METHOD("get_water_value", "name"), &VoxelWorld::get_water_value);
```

and after `get_transparency_value`'s definition,

```cpp
bool VoxelWorld::set_water_value(const String &name, float v) {
	return context_.render->set_water_value(name.utf8().get_data(), v);
}

float VoxelWorld::get_water_value(const String &name) const {
	return context_.render->water_value(name.utf8().get_data());
}
```

`extension/src/voxel_settings.h`: add `#include "water/water_settings_store.h"`, add `water` to the group list in the header comment, and beside `mutable ve::TransparencySettingsStore transparency_stand_in_;` add `mutable ve::WaterSettingsStore water_stand_in_;`.

`extension/src/voxel_settings.cpp`:
- `kGroups`: `{"display", "render", "beauty", "grass", "leaves", "transparency", "water"}`.
- `group()`: after the transparency line, `if (name == "water") return &water_stand_in_;`.
- `_ready()`'s `stand_ins`: append `{"water", &water_stand_in_}`.

- [ ] **Step 6: The GDScript group lists**

Run: `grep -rn '"transparency"\]' tests/*.gd demo/*.gd`
For every hit (at least `tests/test_voxel_settings.gd:86`), append `"water"` after `"transparency"` in that list.

- [ ] **Step 7: Verify**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_voxel_settings.gd 2>&1 | tail -5`
Expected: PASS (or the Task 0 baseline result: `test_voxel_settings.gd` was on the baseline failure list in the transparency branch; if it is on yours, the failing case set must be the same as at Task 0).

Then: `grep -ln "groups()" tests/*.gd` and run each suite it lists (e.g. `tests/test_settings_menu.gd`). Expected: the Task 0 result.

- [ ] **Step 8: Commit**

```bash
git add extension/src/water extension/tests/test_water_settings.cpp extension/SConstruct \
	extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp \
	extension/src/voxel_world.h extension/src/voxel_world.cpp extension/src/voxel_settings.h \
	extension/src/voxel_settings.cpp tests/test_voxel_settings.gd
git commit -m "feat: a water settings module with its own store and panel tab"
```

(Add any other test file Step 6 edited.)

---

### Task 5: The flow rule and the wave normal

**Files:**
- Create: `shaders/water_flow.glslh`, `shaders/water_waves.glslh`
- Create: `extension/tests/test_water_flow.cpp`

**Interfaces:**
- Produces (GLSL, `water_flow.glslh`, pure): constants `WATER_TOP_TILE_A/B`, `WATER_SIDE_TILE_A/B`, `WATER_SIDE_STRETCH`, `WATER_TOP_SPEED_A/B`, `WATER_SIDE_SPEED_B`; `vec2 water_top_dir(int layer)`, `vec2 water_top_uv(vec2 xz, float t, int layer)`, `vec2 water_side_uv(vec2 hy, float t, float flow_speed, int layer)`, `vec2 water_top_scale(int layer)`, `vec2 water_side_scale(int layer)`, `vec3 water_weights(vec3 n)`.
- Produces (GLSL, `water_waves.glslh`; needs `VE_MATERIAL_ARRAYS` and `water_flow.glslh` included first): `vec3 water_wave_normal(uint mat, vec3 p, vec3 n, vec3 ddx, vec3 ddy, float t, float flow_speed, float strength)`, `float water_glint_roughness(vec3 ddx, vec3 ddy)`, `float water_foam(vec3 p, float depth, float width, float t)`.

- [ ] **Step 1: Write the failing native test**

Create `extension/tests/test_water_flow.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>

// Execute the ACTUAL water flow GLSL in a native test, the way test_grass_tilt_shader.cpp
// executes the blade tilt. The shim supplies only GLSL scalar/vector operations; none of the
// flow is reimplemented here, so this cannot drift from the shader.
//
// The property under test is the user's requirement: water always LOOKS like it flows
// downward (docs/superpowers/specs/2026-10-06-water-voxels-design.md §4). Every pattern on a
// sloped or vertical face slides toward -y, and nothing on a flat top can fall.
namespace {
namespace shader {
struct vec2 {
	float x, y;
	explicit vec2(float a) : x(a), y(a) {}
	vec2(float a, float b) : x(a), y(b) {}
};
vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
vec2 operator*(vec2 a, float b) { return {a.x * b, a.y * b}; }
vec2 operator*(float a, vec2 b) { return b * a; }
vec2 operator/(vec2 a, float b) { return {a.x / b, a.y / b}; }
struct vec3 {
	float x, y, z;
	explicit vec3(float a) : x(a), y(a), z(a) {}
	vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};
vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
vec3 &operator*=(vec3 &a, vec3 b) { a = a * b; return a; }
vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
vec3 abs(vec3 a) { return {std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }
float max(float a, float b) { return a > b ? a : b; }
#include "../../shaders/water_flow.glslh"
} // namespace shader
} // namespace

using shader::vec2;
using shader::vec3;

// The world y at which the side texel whose uv.y is `c` sits at time t. uv.y is affine in y,
// so two samples give the line and its root.
static float side_feature_y(float c, float t, float speed, int layer) {
	const float u0 = shader::water_side_uv(vec2(1.0f, 0.0f), t, speed, layer).y;
	const float u1 = shader::water_side_uv(vec2(1.0f, 1.0f), t, speed, layer).y;
	return (c - u0) / (u1 - u0);
}

TEST_CASE("every side layer slides its pattern down the face over time") {
	for (int layer = 0; layer < 2; layer++)
		for (float speed : {0.1f, 0.6f, 2.0f}) {
			const float y0 = side_feature_y(0.37f, 0.0f, speed, layer);
			const float y1 = side_feature_y(0.37f, 1.0f, speed, layer);
			CHECK(y1 < y0); // down, never up
			const float expect = speed * (layer == 0 ? 1.0f : shader::WATER_SIDE_SPEED_B);
			CHECK(y0 - y1 == doctest::Approx(expect).epsilon(1e-3));
			// The horizontal never moves: water on a wall falls straight down.
			CHECK(shader::water_side_uv(vec2(2.5f, 0.0f), 0.0f, speed, layer).x ==
					doctest::Approx(shader::water_side_uv(vec2(2.5f, 0.0f), 7.0f, speed, layer).x));
		}
}

TEST_CASE("a zero flow speed freezes the side pattern") {
	for (int layer = 0; layer < 2; layer++) {
		const vec2 a = shader::water_side_uv(vec2(1.0f, 3.0f), 0.0f, 0.0f, layer);
		const vec2 b = shader::water_side_uv(vec2(1.0f, 3.0f), 50.0f, 0.0f, layer);
		CHECK(a.x == doctest::Approx(b.x));
		CHECK(a.y == doctest::Approx(b.y));
	}
}

TEST_CASE("the top layers drift sideways, detuned in scale and direction") {
	for (int layer = 0; layer < 2; layer++) {
		const vec2 d = shader::water_top_dir(layer);
		CHECK(std::sqrt(d.x * d.x + d.y * d.y) == doctest::Approx(1.0f).epsilon(1e-3));
		const float tile = layer == 0 ? shader::WATER_TOP_TILE_A : shader::WATER_TOP_TILE_B;
		const float speed = layer == 0 ? shader::WATER_TOP_SPEED_A : shader::WATER_TOP_SPEED_B;
		const vec2 a = shader::water_top_uv(vec2(0.0f, 0.0f), 0.0f, layer);
		const vec2 b = shader::water_top_uv(vec2(0.0f, 0.0f), 10.0f, layer);
		// A feature moves along +dir at `speed`: the uv at a fixed point moves along -dir.
		CHECK((b.x - a.x) * tile == doctest::Approx(-d.x * speed * 10.0f).epsilon(1e-3));
		CHECK((b.y - a.y) * tile == doctest::Approx(-d.y * speed * 10.0f).epsilon(1e-3));
	}
	const vec2 a = shader::water_top_dir(0), b = shader::water_top_dir(1);
	const float deg = std::acos(a.x * b.x + a.y * b.y) * 57.29578f;
	CHECK(deg > 100.0f);
	CHECK(deg < 120.0f);
	// The tile ratio is not near a whole number, so the two periods never line up.
	const float ratio = shader::WATER_TOP_TILE_A / shader::WATER_TOP_TILE_B;
	CHECK(std::fabs(ratio - std::round(ratio)) > 0.2f);
}

TEST_CASE("triplanar weights put a flat top on the top projection and a wall on a side") {
	const vec3 top = shader::water_weights(vec3(0.0f, 1.0f, 0.0f));
	CHECK(top.y == doctest::Approx(1.0f));
	CHECK(top.x == doctest::Approx(0.0f));
	CHECK(top.z == doctest::Approx(0.0f));
	const vec3 wall = shader::water_weights(vec3(0.0f, 0.0f, -1.0f));
	CHECK(wall.z == doctest::Approx(1.0f));
	CHECK(wall.y == doctest::Approx(0.0f));
	// A 45-degree slope is shared between the top's drift and a side's fall, summing to one.
	const float h = std::sqrt(0.5f);
	const vec3 slope = shader::water_weights(vec3(h, h, 0.0f));
	CHECK(slope.x == doctest::Approx(0.5f));
	CHECK(slope.y == doctest::Approx(0.5f));
	CHECK(slope.x + slope.y + slope.z == doctest::Approx(1.0f));
}

TEST_CASE("the uv scales are the derivatives of the uv maps") {
	for (int layer = 0; layer < 2; layer++) {
		const vec2 s = shader::water_side_scale(layer);
		const vec2 a = shader::water_side_uv(vec2(0.0f, 0.0f), 2.0f, 0.6f, layer);
		const vec2 b = shader::water_side_uv(vec2(1.0f, 1.0f), 2.0f, 0.6f, layer);
		CHECK(b.x - a.x == doctest::Approx(s.x));
		CHECK(b.y - a.y == doctest::Approx(s.y));
		const vec2 ts = shader::water_top_scale(layer);
		const vec2 c = shader::water_top_uv(vec2(0.0f, 0.0f), 2.0f, layer);
		const vec2 e = shader::water_top_uv(vec2(1.0f, 1.0f), 2.0f, layer);
		CHECK(e.x - c.x == doctest::Approx(ts.x));
		CHECK(e.y - c.y == doctest::Approx(ts.y));
	}
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd extension && scons -Q test`
Expected: compile error, `'../../shaders/water_flow.glslh' file not found`.

- [ ] **Step 3: Write `water_flow.glslh`**

```glsl
// The water flow rule (docs/superpowers/specs/2026-10-06-water-voxels-design.md §4). Pure
// arithmetic, no texture and no includes: extension/tests/test_water_flow.cpp executes this
// file natively, so keep it to scalar/vec2/vec3 operations its shim provides.
//
// Three projections. The TOP (xz) drifts slowly in two detuned layers: a pond stays calm. The
// two SIDES (zy and xy) scroll DOWN in world y in two layers: every sloped or vertical face
// reads as water sliding downhill. Each projection is a constant scroll, so there is no
// flow-map phase reset and no pulsing.

// Metres per texture repeat. 5.0 / 1.35 is about 3.7, not a whole number, so the two layers'
// periods never line up and the tiling hides.
const float WATER_TOP_TILE_A = 5.0;
const float WATER_TOP_TILE_B = 1.35;
const float WATER_SIDE_TILE_A = 3.0;
const float WATER_SIDE_TILE_B = 0.85;
// Side ripples are stretched along the fall so they read as streaks.
const float WATER_SIDE_STRETCH = 2.5;
// Top drift, metres per second along water_top_dir.
const float WATER_TOP_SPEED_A = 0.04;
const float WATER_TOP_SPEED_B = 0.09;
// Side layer B falls this much faster than flow_speed.
const float WATER_SIDE_SPEED_B = 1.6;

// The top layers' drift directions: unit vectors about 110 degrees apart.
vec2 water_top_dir(int layer) {
	return layer == 0 ? vec2(0.8, 0.6) : vec2(-0.8377, 0.5461);
}

// uv of top layer `layer` at world xz, time t. A feature at a fixed uv sits at
// xz = uv * tile + dir * speed * t: it drifts along dir.
vec2 water_top_uv(vec2 xz, float t, int layer) {
	float tile = layer == 0 ? WATER_TOP_TILE_A : WATER_TOP_TILE_B;
	float speed = layer == 0 ? WATER_TOP_SPEED_A : WATER_TOP_SPEED_B;
	return (xz - water_top_dir(layer) * (speed * t)) / tile;
}

// uv of side layer `layer` at (horizontal, world y), time t. A feature at a fixed uv.y sits at
// y = uv.y * tile * stretch - speed * t, which DEcreases with t: it slides down. This is the
// "always flows downward" rule.
vec2 water_side_uv(vec2 hy, float t, float flow_speed, int layer) {
	float tile = layer == 0 ? WATER_SIDE_TILE_A : WATER_SIDE_TILE_B;
	float speed = flow_speed * (layer == 0 ? 1.0 : WATER_SIDE_SPEED_B);
	return vec2(hy.x / tile, (hy.y + speed * t) / (tile * WATER_SIDE_STRETCH));
}

// d(uv)/d(world), for textureGrad: both maps are affine in world position.
vec2 water_top_scale(int layer) {
	return vec2(1.0 / (layer == 0 ? WATER_TOP_TILE_A : WATER_TOP_TILE_B));
}

vec2 water_side_scale(int layer) {
	float tile = layer == 0 ? WATER_SIDE_TILE_A : WATER_SIDE_TILE_B;
	return vec2(1.0 / tile, 1.0 / (tile * WATER_SIDE_STRETCH));
}

// Triplanar weights, |n|^4 normalised: sharper than the terrain's linear weights, so a slope
// hands over from drift to fall over a narrower band.
vec3 water_weights(vec3 n) {
	vec3 a = abs(n);
	a *= a;
	a *= a;
	return a / max(a.x + a.y + a.z, 1e-5);
}
```

- [ ] **Step 4: Run it to verify it passes**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`

- [ ] **Step 5: Write `water_waves.glslh`**

```glsl
// Water's wave normal, glint roughness and shore foam (docs/superpowers/specs/2026-10-06-
// water-voxels-design.md §4, §5). The includer must define VE_MATERIAL_ARRAYS and include
// common.glslh (material_surface_tex, triplanar_normal, MATERIAL_LAYERS) and water_flow.glslh
// first.
//
// The ripple map is water's OWN atlas layer: assets/materials/08_normal.png, packed into the
// surface array's RG by MaterialAtlas exactly like every terrain normal map. No texture or
// binding exists for waves.

vec3 water_unpack(vec2 texel) {
	vec2 e = texel * 2.0 - 1.0;
	return vec3(e, sqrt(max(1.0 - dot(e, e), 0.0)));
}

// Two taps of one projection, whiteout-blended, tilted by `strength`, and returned in texel
// space (0.5 = flat) because that is what triplanar_normal takes.
vec2 water_pair(int layer, vec2 uva, vec2 uvb, vec2 sa, vec2 sb, vec2 dx, vec2 dy,
		float strength) {
	vec3 a = water_unpack(textureGrad(material_surface_tex, vec3(uva, float(layer)),
			dx * sa, dy * sa).rg);
	vec3 b = water_unpack(textureGrad(material_surface_tex, vec3(uvb, float(layer)),
			dx * sb, dy * sb).rg);
	vec3 n = normalize(vec3(a.xy + b.xy, a.z * b.z));
	return clamp(n.xy * strength, vec2(-1.0), vec2(1.0)) * 0.5 + 0.5;
}

// The shading normal of liquid material `mat` at world point p with geometric normal n.
// ddx/ddy are the pixel's world footprint (compute shaders have no derivatives), so distant
// ripples fall into the mips and calm down. A projection whose weight is under 0.01 is not
// sampled: a flat pond pixel costs two taps, a slope at most six.
vec3 water_wave_normal(uint mat, vec3 p, vec3 n, vec3 ddx, vec3 ddy, float t, float flow_speed,
		float strength) {
	int layer = int(mat) - 1;
	if (layer < 0 || layer >= MATERIAL_LAYERS || strength <= 0.0) return n;
	vec3 w = water_weights(n);
	vec2 tx = vec2(0.5), ty = vec2(0.5), tz = vec2(0.5);
	if (w.y > 0.01)
		ty = water_pair(layer, water_top_uv(p.xz, t, 0), water_top_uv(p.xz, t, 1),
				water_top_scale(0), water_top_scale(1), ddx.xz, ddy.xz, strength);
	if (w.x > 0.01)
		tx = water_pair(layer, water_side_uv(p.zy, t, flow_speed, 0),
				water_side_uv(p.zy, t, flow_speed, 1), water_side_scale(0), water_side_scale(1),
				ddx.zy, ddy.zy, strength);
	if (w.z > 0.01)
		tz = water_pair(layer, water_side_uv(p.xy, t, flow_speed, 0),
				water_side_uv(p.xy, t, flow_speed, 1), water_side_scale(0), water_side_scale(1),
				ddx.xy, ddy.xy, strength);
	return triplanar_normal(tx, ty, tz, n, w);
}

// The glint's GGX roughness, widened as the finest ripples fall under a pixel so distant
// sparkles do not strobe (a Toksvig-style trade of detail for gloss).
float water_glint_roughness(vec3 ddx, vec3 ddy) {
	float footprint = max(length(ddx), length(ddy));
	return clamp(0.05 + footprint / WATER_TOP_TILE_B, 0.05, 0.5);
}

float water_hash(vec2 c) {
	return fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453);
}

float water_noise(vec2 x) {
	vec2 i = floor(x);
	vec2 f = fract(x);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(water_hash(i), water_hash(i + vec2(1.0, 0.0)), u.x),
			mix(water_hash(i + vec2(0.0, 1.0)), water_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Shore foam: solid where the water is shallowest, breaking up into drifting patches as it
// deepens, gone past `width` metres. `depth` is the caller's estimate of vertical depth.
float water_foam(vec3 p, float depth, float width, float t) {
	if (width <= 0.0) return 0.0;
	float shore = 1.0 - smoothstep(0.0, width, depth);
	if (shore <= 0.0) return 0.0;
	float nz = 0.65 * water_noise(p.xz * 3.0 + vec2(0.21, 0.13) * t)
			+ 0.35 * water_noise(p.xz * 7.0 - vec2(0.17, -0.11) * t);
	return smoothstep(1.0 - shore, 1.0 - shore + 0.25, nz) * 0.85;
}
```

`water_waves.glslh` is compiled for the first time in Task 6 (no shader includes it yet), and its compile is verified there.

- [ ] **Step 6: Commit**

```bash
git add shaders/water_flow.glslh shaders/water_waves.glslh extension/tests/test_water_flow.cpp
git commit -m "feat: a downward water flow rule, executed natively, and the wave normal it drives"
```

---

### Task 6: The liquid composite

**Files:**
- Modify: `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh` (regenerated)
- Modify: `shaders/transparency_composite.comp.glsl`
- Modify: `extension/src/render/transparency_composite_pass.h`, `extension/src/render/transparency_composite_pass.cpp`
- Modify: `extension/src/render/shell_raster_pass.cpp`
- Modify: `extension/src/render/frame.cpp`
- Create: `tests/test_water.gd`

**Interfaces:**
- Consumes: `mat_liquid` (Task 1), `WaterSettings` and `RenderOrchestrator::water_settings()` (Task 4), `water_wave_normal`, `water_glint_roughness`, `water_foam` (Task 5).
- Produces: `TransparencyCompositePush::water` (`float[4]`: wave strength, flow speed, refraction strength, foam width); `params.z` = time in seconds; `TransparencyCompositePass::Params::time_seconds`, `Params::water`; the front target is a storage image the composite writes the liquid shading normal into.

- [ ] **Step 1: Write the failing GPU tests**

Create `tests/test_water.gd`:

```gdscript
extends GdUnitTestSuite

# Water shading, end to end (docs/superpowers/specs/2026-10-06-water-voxels-design.md §5,
# §6). Every frame is debug_render_frame -- the shipping VoxelFrame on a local device -- so
# what is asserted is what ships. Fixture conventions are tests/test_transparency.gd's: the
# sunlit meadow at (20, 60, 30), every temporal input held still, SSAO and SSGI off.

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

func make_world(enabled := true, cam := CAM) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	w.set_transparency_value("enabled", 1.0 if enabled else 0.0)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	w.set_effect_enabled("ssgi", false)
	w.set_effect_enabled("ssao", false)
	w.set_grass_value("enabled", 0.0)
	w.set_grass_value("wind_strength", 0.0)
	w.set_grass_value("wind_speed", 0.0)
	w.set_leaf_value("wind_strength", 0.0)
	w.set_leaf_value("wind_speed", 0.0)
	settle(w, cam)
	# The shell needs the MeshService (debug_init_physics) and the LoD pool (a LoD query):
	# test_transparency.gd's shell_world() preconditions.
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_lod_stats()
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

func centre_hit(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var hit: Dictionary = w.raycast(cam, fwd.normalized(), 400.0)
	assert_bool(hit["hit"]).override_failure_message("the centre ray sees nothing").is_true()
	return hit

func frame(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d: Dictionary = w.hooks().debug_render_frame(cam, fwd.normalized(), W, H)
	assert_bool(d["ok"]).override_failure_message("frame aborted: %s" % d).is_true()
	return d

# The shell is built on the worker thread and lags an edit by a few frames.
func frame_until_front(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		if (d["center_front"] as Color).a > 0.5:
			return d
	return d

func finite(c: Color) -> bool:
	return is_finite(c.r) and is_finite(c.g) and is_finite(c.b)

func dist(a: Color, b: Color) -> float:
	return Vector3(a.r - b.r, a.g - b.g, a.b - b.b).length()

func image_of(d: Dictionary) -> Image:
	return Image.create_from_data(W, H, false, Image.FORMAT_RGBAH, d["scene_rgba"])

# A shallow water cap: a 20 m ball whose top stands 0.4 m proud of the ground at `at`. The
# solid view keeps the ground under it (a liquid ADD contributes nothing), so the water is
# 0.4 m deep at the centre and thins to nothing at the rim.
func add_cap(w: VoxelWorld, at: Vector3) -> void:
	w.hooks().debug_apply_sphere_add(at - Vector3(0, 19.6, 0), 20.0, material_id(w, "water"))

# A pond: a 4 m crater under the same cap. The subtract carves ground and water alike; the
# cap re-added after it fills the crater, so the centre is ~4.4 m deep.
func add_pond(w: VoxelWorld, at: Vector3) -> void:
	w.hooks().debug_apply_sphere_subtract(at, 4.0)
	add_cap(w, at)

# --- §5: shading a front seen from above ---------------------------------------------------

func test_deep_water_is_closer_to_the_scatter_colour_than_shallow_water() -> void:
	var w := make_world()
	w.set_water_value("wave_strength", 0.0) # absorption alone, no glint to land on the pixel
	var ground: Vector3 = centre_hit(w)["pos"]
	var bare: Color = frame(w)["center_lit"]
	add_cap(w, ground)
	settle(w)
	var shallow_d := frame_until_front(w)
	assert_int(int((shallow_d["center_front"] as Color).a + 0.5)).is_equal(material_id(w, "water"))
	var shallow: Color = shallow_d["center_lit"]
	add_pond(w, ground)
	settle(w)
	# Wait for the deepened pond's shell: the path through it grows from ~0.4 m to ~4.4 m.
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		var t: Vector2 = d["center_thick"]
		if t.x + t.y * float(d["center_distance"]) > 3.0:
			break
	var deep: Color = d["center_lit"]
	assert_bool(finite(shallow) and finite(deep)).is_true()
	assert_float(dist(shallow, bare)).override_failure_message(
		"0.4 m of water left the ground untouched").is_greater(0.005)
	assert_float(dist(deep, bare)).override_failure_message(
		"4 m of water is not further from bare ground than 0.4 m: %s %s %s" % [bare, shallow, deep]
		).is_greater(dist(shallow, bare))
	# Red dies first, and a liquid has no transmittance floor: 4 m leaves well under the 0.35
	# that ice would be floored at.
	assert_float(deep.r / max(bare.r, 1e-4)).is_less(0.35)
	assert_float(deep.b / max(bare.b, 1e-4)).is_greater(deep.r / max(bare.r, 1e-4))

func test_water_frames_change_over_time_and_the_ground_around_them_does_not() -> void:
	var w := make_world()
	add_pond(w, centre_hit(w)["pos"])
	settle(w)
	var a := frame_until_front(w)
	for i in range(30):
		w.hooks().debug_stream_frame(CAM)
	var b := frame(w)
	assert_float(dist(a["center_lit"], b["center_lit"])).override_failure_message(
		"the water did not move in 30 frames").is_greater(1e-4)
	# A corner pixel is meadow well outside the 4 m pond: with every other input still, it
	# must be the same colour in both frames.
	var ia := image_of(a)
	var ib := image_of(b)
	var ca := ia.get_pixel(1, 1)
	var cb := ib.get_pixel(1, 1)
	assert_float(dist(ca, cb)).override_failure_message(
		"ground away from the water changed: %s vs %s" % [ca, cb]).is_less(1e-5)

# Refraction must never pull a foreground object into the water. The pillar is GLOWING
# (ground_crack_01): if its colour leaked into water pixels, the count of strongly orange
# pixels would grow when refraction is turned up.
func test_refraction_never_pulls_a_foreground_object_into_the_water() -> void:
	var w := make_world()
	var ground: Vector3 = centre_hit(w)["pos"]
	add_pond(w, ground)
	var mid := CAM.lerp(ground, 0.55) + Vector3(1.2, 0, 0)
	w.hooks().debug_apply_sphere_add(mid, 0.6, material_id(w, "ground_crack_01"))
	settle(w)
	frame_until_front(w)
	w.set_water_value("refraction_strength", 0.0)
	var still := image_of(frame(w))
	w.set_water_value("refraction_strength", 0.2)
	var bent := image_of(frame(w))
	var orange := func(img: Image) -> int:
		var n := 0
		for y in range(H):
			for x in range(W):
				var c := img.get_pixel(x, y)
				if c.r > 0.5 and c.r > 2.0 * c.b:
					n += 1
		return n
	var before: int = orange.call(still)
	var after: int = orange.call(bent)
	assert_int(before).override_failure_message("the glowing pillar is not in view").is_greater(0)
	assert_int(after).override_failure_message(
		"refraction pulled the pillar into the water: %d orange pixels, %d without" % [after, before]
		).is_less_equal(before + 2)

# Review Focus 1.
func test_water_against_the_sky_is_tinted_not_black() -> void:
	var w := make_world()
	var cam := CAM + Vector3(0, 30.0, 0)
	var fwd := Vector3(0.3, 1.0, 0.2)
	settle(w, cam)
	var sky: Color = frame(w, cam, fwd)["center_lit"]
	w.hooks().debug_apply_sphere_add(cam + fwd.normalized() * 6.0, 1.5, material_id(w, "water"))
	settle(w, cam)
	var d := frame_until_front(w, cam, fwd)
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	assert_float(dist(lit, sky)).is_greater(0.005)
	assert_float(lit.r + lit.g + lit.b).is_greater(0.05)

# Review Focus 4.
func test_extreme_water_settings_stay_finite_and_deterministic() -> void:
	var w := make_world()
	add_pond(w, centre_hit(w)["pos"])
	settle(w)
	frame_until_front(w)
	for kv in [["wave_strength", 0.0], ["wave_strength", 3.0], ["refraction_strength", 0.2],
			["foam_width_m", 0.0], ["foam_width_m", 3.0], ["flow_speed", 5.0]]:
		w.set_water_value(kv[0], kv[1])
		var d := frame(w)
		assert_bool(finite(d["center_lit"])).override_failure_message(
			"%s=%s gave %s" % [kv[0], kv[1], d["center_lit"]]).is_true()
	# Still water (no flow, no waves) rendered twice with nothing else moving: the same image.
	w.set_water_value("flow_speed", 0.0)
	w.set_water_value("wave_strength", 0.0)
	var b1: int = int(frame(w)["lit_checksum"])
	var b2: int = int(frame(w)["lit_checksum"])
	assert_int(b2).override_failure_message("still water rendered twice differs").is_equal(b1)

# Review Focus 5.
func test_ice_floating_over_a_pond_keeps_its_own_front() -> void:
	var w := make_world()
	var ground: Vector3 = centre_hit(w)["pos"]
	add_pond(w, ground)
	w.hooks().debug_apply_sphere_add(ground + Vector3(0, 3.0, 0), 0.8, material_id(w, "ice"))
	settle(w)
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		if int((d["center_front"] as Color).a + 0.5) == material_id(w, "ice"):
			break
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"the nearer ice is not the centre's front: %s" % d).is_equal(material_id(w, "ice"))
	assert_bool(finite(d["center_lit"])).is_true()

func test_the_shading_normal_reaches_the_front_the_gbuffer_resolves() -> void:
	var w := make_world()
	add_pond(w, centre_hit(w)["pos"])
	settle(w)
	frame_until_front(w)
	w.set_water_value("wave_strength", 0.0)
	var flat: Color = frame(w)["center_front"]
	w.set_water_value("wave_strength", 3.0)
	var rippled: Color = frame(w)["center_front"]
	# xy is the octahedral normal the resolve copies into the G-buffer; z (distance) and w
	# (material) are the front pass's and do not move.
	assert_float(Vector2(flat.r - rippled.r, flat.g - rippled.g).length()).is_greater(1e-3)
	assert_float(flat.b).is_equal_approx(rippled.b, 1e-4)
	assert_float(flat.a).is_equal(rippled.a)

func test_with_transparency_off_water_draws_as_an_opaque_surface() -> void:
	var w := make_world(false)
	add_cap(w, centre_hit(w)["pos"])
	settle(w)
	var d := frame(w)
	assert_int(int(d["center_material"])).is_equal(material_id(w, "water"))
	assert_float((d["center_front"] as Color).a).is_equal(0.0)
	assert_bool(finite(d["center_lit"])).is_true()
```

- [ ] **Step 2: Run them to verify they fail**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_water.gd 2>&1 | tail -40`
Expected: failures in `set_water_value`-dependent shading: `test_the_shading_normal_reaches_the_front_the_gbuffer_resolves` (front.xy unchanged by wave_strength), `test_water_frames_change_over_time...` (no animation), and `test_deep_water...` (ice-path floor keeps `deep.r / bare.r` above 0.35). `test_with_transparency_off...` may already pass.

- [ ] **Step 3: The push block's `water` lane**

In `extension/src/gpu_layout/blocks.h`, replace `struct TransparencyCompositePush` with:

```cpp
struct TransparencyCompositePush {
	float right_tanx[4];  // xyz = camera right, w = tan(fov_x / 2)
	float up_tany[4];     // xyz = camera up,    w = tan(fov_y / 2)
	float sky[4];         // xyz = ambient, w unused
	float params[4];      // x = min transmit, y = sky thickness (m), z = time (s), w unused
	uint32_t flags[4];    // x = beauty flags, y = material the camera is inside (0 = outside)
	float water[4];       // x = wave strength, y = flow speed (m/s), z = refraction, w = foam width (m)
};
```

and in `kTransparencyCompositePushFields`, after the `flags` row:

```cpp
	VE_LAYOUT_FIELD(TransparencyCompositePush, water, Vec4, 0),
```

Run: `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc="generated: shaders/generated/blocks.glslh" && scons -Q test`
(The first `scons -Q test` rebuilds the binary and is expected to fail on the blocks golden.)
Expected: `shaders/generated/blocks.glslh` gains a `vec4 water;` line in `TRANSPARENCY_COMPOSITE_PUSH_FIELDS`; `Status: SUCCESS!`.

- [ ] **Step 4: The front target becomes writable**

In `extension/src/render/shell_raster_pass.cpp`, `ensure_targets`, replace the `front_` creation with:

```cpp
	// Storage too: the transparency composite writes a liquid front's SHADING normal back into
	// xy, and the G-buffer resolve copies xy into the surface (water spec §5, plan deviation 2).
	front_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, size,
			colour | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT);
```

- [ ] **Step 5: The composite pass owns a `lit` copy and pushes the water lane**

In `extension/src/render/transparency_composite_pass.h`: add `#include "water/water_settings.h"`; in `Params`, after `inside_material`:

```cpp
		float time_seconds = 0.0f; // the clock grass wind uses: beauty frames / 60
		ve::WaterSettings water;
```

Update the class comment's second sentence to: `Runs after deferred and before inject, rewriting gb.lit() in place for the pixels that have a front (or, with the camera inside the medium, a thickness), and writing a liquid front's shading normal back into the front target.`

In `private:`, add:

```cpp
	bool ensure_lit_copy(RenderingDevice *rd, Vector2i size);
	RID lit_copy_; // lit as deferred left it: refraction reads neighbours while lit is rewritten
	Vector2i lit_copy_size_{0, 0};
```

In `transparency_composite_pass.cpp`:

`teardown()`: change `sampler_nearest_ = sampler_linear_ = dummy_far_ = RID();` to `sampler_nearest_ = sampler_linear_ = dummy_far_ = lit_copy_ = RID();` and add `lit_copy_size_ = Vector2i(0, 0);` after it.

Add before `render`:

```cpp
bool TransparencyCompositePass::ensure_lit_copy(RenderingDevice *rd, Vector2i size) {
	if (size == lit_copy_size_ && lit_copy_.is_valid()) return true;
	gpu::RdDevice device{rd};
	group_.free(device, lit_copy_);
	lit_copy_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, size,
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	lit_copy_size_ = size;
	return lit_copy_.is_valid();
}
```

In `render`, after `if (!sun_map.is_valid()) flags &= ~ve::kFlagSunMap;`:

```cpp
	const Vector2i size = gb.size();
	// ponytail: copied on every frame the shell drew, ice-only frames included -- the CPU
	// cannot tell which materials the shell pages hold. A "liquid drawn" bit from the front
	// pass would skip it.
	if (!ensure_lit_copy(rd, size)) return false;
	if (rd->texture_copy(gb.lit(), lit_copy_, Vector3(0, 0, 0), Vector3(0, 0, 0),
				Vector3(size.x, size.y, 1), 0, 0, 0, 0) != OK)
		return false;
```

and delete the later `const Vector2i size = gb.size();` line before the dispatch.

In the uniform set, replace `gpu::sampled(2, sampler_nearest_, front),` with `gpu::image(2, front),` and append `gpu::sampled(10, sampler_nearest_, lit_copy_)` after `gpu::ubo(9, sun_light_ubo_)` (add the comma).

Replace the push construction with:

```cpp
	const ve::TransparencyCompositePush push{
			{p.right[0], p.right[1], p.right[2], p.tan_x},
			{p.up[0], p.up[1], p.up[2], p.tan_y},
			{p.ambient[0], p.ambient[1], p.ambient[2], 0.0f},
			{p.min_transmit, p.sky_thickness_m, p.time_seconds, 0.0f},
			{flags, p.inside_material, 0u, 0u},
			{p.water.wave_strength, p.water.flow_speed, p.water.refraction_strength,
					p.water.foam_width_m}};
```

- [ ] **Step 6: The frame hands over time and water settings**

In `extension/src/render/frame.cpp`, in the transparency composite block, after `tp.inside_material = inside_material;`:

```cpp
		tp.time_seconds = static_cast<float>(render_.beauty_frame()) / 60.0f;
		tp.water = render_.water_settings();
```

- [ ] **Step 7: The liquid branch in the composite**

In `shaders/transparency_composite.comp.glsl`:

After `#include "beauty_camera.glslh"` add:

```glsl
#include "water_flow.glslh"
#include "water_waves.glslh"
```

Replace

```glsl
layout(set = 0, binding = 2) uniform sampler2D front_tex; // xy oct normal, z distance, w material
```

with

```glsl
// xy oct normal, z distance, w material. A storage image: a liquid pixel's SHADING normal is
// written back into xy for the G-buffer resolve (water spec §5). Neighbours are read for
// refraction while that happens; only xy is ever written, and only z and w are read there.
layout(set = 0, binding = 2, rgba32f) uniform image2D front_img;
```

After `layout(set = 0, binding = 5, std140) uniform SunBlock ...;` add:

```glsl
layout(set = 0, binding = 10) uniform sampler2D lit_copy; // lit as deferred left it, for refraction
```

Update the header comment's last line to: `// Pixels with no front and no thickness are left exactly as deferred wrote them. A LIQUID front takes shade_liquid() instead (docs/superpowers/specs/2026-10-06-water-voxels-design.md §5, §6).`

After `shell_thickness` add:

```glsl
// --- Liquids (docs/superpowers/specs/2026-10-06-water-voxels-design.md §5, §6) -----------

const float WATER_PI = 3.14159265;
// How much of the sun the scatter colour picks up, beside the full sky ambient. Light
// scattered inside the medium has no surface to face, so there is no ndl.
const float WATER_SCATTER_SUN = 0.35;
// The glint is HDR so bloom can pick the sparkles up; the cap keeps one texel facing the sun
// exactly from going to infinity.
const float WATER_GLINT_MAX = 64.0;
const float WATER_FOAM_ALBEDO = 0.9;

// No floor: deep water fades to the scatter colour (the spec drops min_transmit for liquids).
vec3 liquid_transmit(uint mat, float path) {
	return pow(max(mat_transmit(mat), vec3(1e-5)), vec3(max(path, 0.0)));
}

vec3 liquid_body(uint mat, float shadow) {
	return flat_material_albedo(mat) * (pc.sky.rgb + sun_light.rgb.xyz * (shadow * WATER_SCATTER_SUN));
}

// GGX with Schlick's Fresnel and the geometry term taken as 1: D * F / (4 ndv), times ndl.
float liquid_glint(vec3 n, vec3 v, vec3 l, float rough, float f0) {
	float ndl = dot(n, l);
	if (ndl <= 0.0) return 0.0;
	vec3 h = normalize(v + l);
	float a2 = rough * rough * rough * rough;
	float ndh = max(dot(n, h), 0.0);
	float d = ndh * ndh * (a2 - 1.0) + 1.0;
	float D = a2 / (WATER_PI * d * d);
	float F = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(v, h), 0.0, 1.0), 5.0);
	return min(D * F * ndl / (4.0 * max(dot(n, v), 0.05)), WATER_GLINT_MAX);
}

// The opaque surface's distance at pixel q, 0 for sky -- main()'s z_opaque at another pixel.
float opaque_distance(ivec2 q, ivec2 size) {
	float depth = texelFetch(gb_depth, q, 0).r;
	vec2 quv = (vec2(q) + 0.5) / vec2(size);
	return depth > 0.0 ? distance(beauty_world_from_depth(quv, depth), bcam.cam.xyz) : 0.0;
}

void shade_liquid(ivec2 px, ivec2 size, vec2 uv, vec4 front, bool has_front, float thickness,
		uint mat) {
	if (!has_front) {
		// The camera is inside the liquid and this pixel shows no surface (§6 item 2): fog
		// over the path to what is behind, which is exactly `thickness` -- the virtual front
		// at 0 to the opaque hit.
		vec3 T = liquid_transmit(mat, thickness);
		vec3 behind = imageLoad(lit, px).rgb;
		imageStore(lit, px, vec4(T * behind + (1.0 - T) * liquid_body(mat, 1.0), 1.0));
		return;
	}
	vec3 rd = normalize(beauty_world_from_depth(uv, 1.0) - bcam.cam.xyz);
	vec3 v = -rd;
	vec3 p = bcam.cam.xyz + rd * front.z;
	vec3 n_geo = oct_decode(front.xy);
	// The stored normal points out of the medium, so a face seen from inside it is an exit.
	bool exit_face = dot(n_geo, v) < 0.0;
	// The pixel's world footprint at the front, as the ice branch derives it.
	vec3 ddx = pc.right_tanx.xyz * (2.0 * pc.right_tanx.w / float(size.x)) * front.z;
	vec3 ddy = pc.up_tany.xyz * (2.0 * pc.up_tany.w / float(size.y)) * front.z;
	float t = pc.params.z;
	vec3 n = water_wave_normal(mat, p, n_geo, ddx, ddy, t, pc.water.y, pc.water.x);
	vec3 n_face = exit_face ? -n_geo : n_geo;
	if (exit_face) n = -n; // shade the side the camera sees
	vec3 sun_dir = sun_light.dir.xyz;
	float shadow = (pc.flags.x & BEAUTY_SUN_MAP) != 0u
			? sun_map_visibility(p, dot(n_face, sun_dir), front.z) : 1.0;
	vec3 body = liquid_body(mat, shadow);

	// Refraction (§5 step 1): offset by what the waves added to the face normal, projected onto
	// the screen and scaled down in shallow water. Image rows run downward, so up is negated.
	vec3 tilt = n - n_face;
	vec2 off = vec2(dot(tilt, pc.right_tanx.xyz), -dot(tilt, pc.up_tany.xyz))
			* (pc.water.z * clamp(thickness, 0.0, 1.0));
	ivec2 rpx = clamp(px + ivec2(round(off * vec2(size))), ivec2(0), size - ivec2(1));
	vec4 rfront = imageLoad(front_img, rpx);
	float rz = opaque_distance(rpx, size);
	// Reject (a) anything nearer than this front -- a foreground object would leak in -- and
	// (b) a pixel with no liquid front -- the shore above the waterline.
	bool keep = rfront.w > 0.5 && mat_liquid(uint(rfront.w + 0.5)) != LIQUID_NONE
			&& (rz <= 0.0 || rz >= front.z);
	// Absorb over the path to the pixel the colour came from, not this one: that is what
	// keeps the shore free of a halo (§5 step 2).
	float path = thickness;
	if (keep && rpx != px) path = shell_thickness(texelFetch(thick_tex, rpx, 0).rg, rz, rfront.z);
	else rpx = px;
	vec3 behind = texelFetch(lit_copy, rpx, 0).rgb;
	float ior = mat_ior(mat);
	float f0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
	vec3 col;
	if (!exit_face) {
		vec3 T = liquid_transmit(mat, path);
		vec3 through = T * behind + (1.0 - T) * body;
		float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
		col = mix(through, sky_color(reflect(rd, n)), fresnel);
		col += sun_light.rgb.xyz * (shadow
				* liquid_glint(n, v, sun_dir, water_glint_roughness(ddx, ddy), f0));
		// Shore foam (§5 step 6): view-ray thickness turned into an estimate of vertical depth.
		float depth = thickness * max(dot(v, n_geo), 0.05);
		float foam = water_foam(p, depth, pc.water.w, t);
		vec3 foam_lit = vec3(WATER_FOAM_ALBEDO) * (pc.sky.rgb
				+ sun_light.rgb.xyz * (shadow * max(dot(n_geo, sun_dir), 0.0)));
		col = mix(col, foam_lit, foam);
	} else {
		// Under the surface, looking out (§6 item 3). refract() wants the normal against the
		// ray, which n now is, and eta = n_liquid / n_air. Inside Snell's window the world
		// above shows through; past it, total internal reflection shows the liquid's own body
		// colour, and SSR replaces that with real reflected geometry where it hits.
		vec3 r = refract(rd, n, ior);
		vec3 seen = body;
		if (dot(r, r) > 0.0) {
			float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(r, -n), 0.0, 1.0), 5.0);
			seen = mix(behind, body, fresnel);
		}
		vec3 T = liquid_transmit(mat, front.z);
		col = T * seen + (1.0 - T) * body;
	}
	imageStore(lit, px, vec4(col, 1.0));
	// The G-buffer resolve copies front.xy into the surface normal, so writing the shading
	// normal here hands the ripples, facing the camera, to SSR and outlines.
	imageStore(front_img, px, vec4(oct_encode(n), front.zw));
}
```

In `main()`, replace `vec4 front = texelFetch(front_tex, px, 0);` with `vec4 front = imageLoad(front_img, px);`, and immediately after `uint mat = has_front ? uint(front.w + 0.5) : inside;` insert:

```glsl
	if (mat_liquid(mat) != LIQUID_NONE) {
		shade_liquid(px, size, uv, front, has_front, thickness, mat);
		return;
	}
```

Nothing else in `main()` changes.

- [ ] **Step 8: Run the GPU tests to verify they pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_water.gd 2>&1 | tail -30`
Expected: all eight cases PASS. If the shader fails to compile, the run prints the glslang error with the line: fix it and re-run. Commit `shaders/transparency_composite.comp.glsl.import` only if the run regenerated it.

- [ ] **Step 9: The pins hold**

Run `tests/test_transparency.gd` and the golden suites exactly as Task 0 Step 4.
Expected: unchanged from Task 0. In particular every ice-shading case passes: the ice branch reads the same front values through `imageLoad`.

- [ ] **Step 10: Commit**

```bash
git add extension/src/gpu_layout/blocks.h shaders/generated/blocks.glslh \
	shaders/transparency_composite.comp.glsl extension/src/render/transparency_composite_pass.h \
	extension/src/render/transparency_composite_pass.cpp extension/src/render/shell_raster_pass.cpp \
	extension/src/render/frame.cpp tests/test_water.gd
git commit -m "feat: liquid fronts refract, absorb, scatter, glint and foam in the transparency composite"
```

---

### Task 7: The underwater view

**Files:**
- Modify: `extension/src/render/shell_raster_pass.h`, `extension/src/render/shell_raster_pass.cpp`
- Modify: `extension/src/render/frame.cpp`
- Modify: `tests/test_water.gd`

**Interfaces:**
- Consumes: `ve::material_liquid` (Task 1), the liquid composite (Task 6).
- Produces: `ShellRasterPass::draw(..., bool front_face_clockwise, bool camera_inside, bool exit_faces)`. With `exit_faces` the front pass culls nothing, so the nearest face of either orientation wins.

- [ ] **Step 1: Write the failing GPU tests**

Append to `tests/test_water.gd`:

```gdscript
# --- §6: the underwater view ---------------------------------------------------------------

func luma(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b

func frame_with_pages(w: VoxelWorld, cam: Vector3, fwd: Vector3) -> Dictionary:
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		if int(d["shell_pages"]) > 0:
			break
	return d

func test_a_camera_inside_water_sees_fog_not_black() -> void:
	var w := make_world()
	var bare: Color = frame(w)["center_lit"]
	w.hooks().debug_apply_sphere_add(CAM, 3.0, material_id(w, "water"))
	settle(w)
	var d := frame_with_pages(w, CAM, FWD)
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	assert_float(lit.r + lit.g + lit.b).is_greater(0.05)
	assert_float(dist(lit, bare)).override_failure_message(
		"three metres of water around the camera changed nothing").is_greater(0.01)

func test_the_surface_above_an_underwater_camera_is_a_front() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(CAM, 3.0, material_id(w, "water"))
	settle(w)
	var up := Vector3(0.1, 1.0, 0.1)
	var d := frame_with_pages(w, CAM, up)
	# Seen from inside, the surface overhead is a BACK face; the exit-face pipeline records it.
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"no front for the surface overhead: %s" % d).is_equal(material_id(w, "water"))
	assert_float((d["center_front"] as Color).b).is_between(2.0, 4.0)

# A big ball whose top sits 1 m above the camera: locally an almost flat surface overhead.
# Straight up is inside Snell's window (sky through the surface); 15 degrees above the
# horizon is past the critical angle (the water's own dark body colour).
func test_snells_window_is_brighter_than_total_internal_reflection() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(CAM - Vector3(0, 19.0, 0), 20.0, material_id(w, "water"))
	settle(w)
	var up := frame_with_pages(w, CAM, Vector3(0.02, 1.0, 0.0))
	var graze := frame(w, CAM, Vector3(1.0, 0.27, 0.0))
	assert_int(int((up["center_front"] as Color).a + 0.5)).is_equal(material_id(w, "water"))
	assert_int(int((graze["center_front"] as Color).a + 0.5)).is_equal(material_id(w, "water"))
	assert_float(luma(up["center_lit"])).override_failure_message(
		"straight up %s is not brighter than grazing %s" % [up["center_lit"], graze["center_lit"]]
		).is_greater(luma(graze["center_lit"]))

# Review Focus 3: a camera a few centimetres under a small ball's top, nudged up and down
# through the surface, flips `inside` frame to frame. Every frame must stay finite and lit.
func test_a_camera_crossing_a_small_surface_stays_finite() -> void:
	var w := make_world()
	var c := CAM - Vector3(0, 0.55, 0)
	w.hooks().debug_apply_sphere_add(c, 0.6, material_id(w, "water"))
	settle(w)
	frame_with_pages(w, CAM, FWD)
	for dy in [-0.08, -0.03, 0.02, 0.07, -0.05]:
		var d := frame(w, CAM + Vector3(0, dy, 0), FWD)
		var lit: Color = d["center_lit"]
		assert_bool(finite(lit)).override_failure_message("dy=%s gave %s" % [dy, lit]).is_true()
		assert_float(lit.r + lit.g + lit.b).is_greater(0.02)
```

- [ ] **Step 2: Run them to verify they fail**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_water.gd 2>&1 | tail -30`
Expected: `test_the_surface_above_an_underwater_camera_is_a_front` and `test_snells_window...` fail with `center_front.a == 0` (the back-face-culling pipeline drops the surface overhead). The fog and crossing tests may already pass.

- [ ] **Step 3: The exit-face pipeline**

In `extension/src/render/shell_raster_pass.h`:
- `draw`'s declaration becomes

```cpp
	bool draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
			RID beauty_cam_ubo, RID island_desc, float fade_start, float fade_end,
			bool front_face_clockwise, bool camera_inside, bool exit_faces);
```

and add to its comment: `// exit_faces: the camera sits in a LIQUID, so the front pass culls nothing and the nearest face of either orientation wins -- the surface overhead is a back face from below (water spec §6). Outside a liquid, and inside ice, the back-face-culling pipeline runs unchanged.`
- In `private:`, change `RID thick_shader_, front_shader_, thick_pipeline_, front_pipeline_, sampler_;` to `RID thick_shader_, front_shader_, thick_pipeline_, front_pipeline_, exit_pipeline_, sampler_;`.

In `extension/src/render/shell_raster_pass.cpp`:
- `draw`'s definition takes the new `bool exit_faces` parameter last.
- In `teardown()`, add `exit_pipeline_` to the line that resets the pipelines to `RID()`.
- In the pipeline (re)build block, the free list gains `group_.free(device, exit_pipeline_);` next to the two existing frees, and after `front_pipeline_ = gpu::raster_pipeline(...)` add:

```cpp
		gpu::RasterState exit = front;
		exit.cull = RenderingDevice::POLYGON_CULL_DISABLED;
		exit_pipeline_ = gpu::raster_pipeline(rd, group_, front_shader_, front_fb_.format(), exit);
```

- The validity checks `if (!thick_pipeline_.is_valid() || !front_pipeline_.is_valid() || ...` (both of them) also require `exit_pipeline_.is_valid()`.
- In the front draw list, replace `rd->draw_list_bind_render_pipeline(dl, front_pipeline_);` with `rd->draw_list_bind_render_pipeline(dl, exit_faces ? exit_pipeline_ : front_pipeline_);`.

Run: `grep -rn "shell_raster->draw(\|shell->draw(\|ShellRasterPass::draw" extension/src`
For every call site other than the one in `frame.cpp`, pass `false` as the new last argument.

- [ ] **Step 4: The frame asks for exit faces inside a liquid**

In `extension/src/render/frame.cpp`, in the shell block, replace

```cpp
						lod_raster->front_face_clockwise(), inside_material != 0);
```

with

```cpp
						lod_raster->front_face_clockwise(), inside_material != 0,
						ve::material_liquid(inside_material) != ve::Liquid::none);
```

- [ ] **Step 5: Run the GPU tests to verify they pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_water.gd 2>&1 | tail -20`
Expected: all twelve cases PASS.

- [ ] **Step 6: The pins hold**

Run `tests/test_transparency.gd` (its `test_a_camera_inside_ice_sees_a_tinted_world` asserts no front is written inside ice) and the golden suites exactly as Task 0 Step 4.
Expected: unchanged from Task 0.

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/shell_raster_pass.h extension/src/render/shell_raster_pass.cpp \
	extension/src/render/frame.cpp tests/test_water.gd
git commit -m "feat: an underwater camera sees the surface overhead through Snell's window"
```

(Add any other file Step 3's grep edited.)

---

### Task 8: Benchmark flag, captures, cost and the record

**Files:**
- Modify: `demo/benchmark.gd`
- Modify: `tools/transparency_capture.gd`
- Modify: `docs/superpowers/specs/2026-10-06-water-voxels-design.md`

**Interfaces:**
- Consumes: everything above.
- Produces: `--water=R` in `demo/benchmark.gd`; capture cases `pond`, `wall`, `underwater`, `water_seam`, `flow`.

- [ ] **Step 1: `--water=R`**

In `demo/benchmark.gd`: after `var _ice_pending := false` add `var _medium := "ice" # the material --ice / --water places`. After the `--ice=` branch add:

```gdscript
		elif arg.begins_with("--water="):
			# The --ice twin: a water sphere of this radius where the camera first looks, so the
			# water shading can be A/B'd against an ice ball of the same size and place.
			_ice_radius = float(arg.trim_prefix("--water="))
			_medium = "water"
```

In `_place_ice()`, replace `if m["name"] == "ice":` with `if m["name"] == _medium:` and the print with `print("benchmark: %s r=%.1f at %s" % [_medium, _ice_radius, hit["pos"]])`. (For `--ice` the printed line is unchanged.)

- [ ] **Step 2: Capture cases**

In `tools/transparency_capture.gd`:
- Add to the header comment, after the `--case=linger` paragraph:

```gdscript
# --case=pond        a 4 m crater under a water cap 12 m ahead: deep teal centre, clear edges,
#                    shore foam at the rim, sun glint on the ripples.
# --case=wall        a floating r=3 water ball 10 m ahead: every side face should read as water
#                    sliding DOWN (spec §4); judge the direction with --case=flow.
# --case=underwater  the lens inside an r=6 water ball looking up and out: distance fog, a bright
#                    Snell's window overhead, the dark total-internal-reflection ring around it.
# --case=water_seam  the seam case's chain in water: one water ridge across the fade band.
# --case=flow        the wall case saved as eight frames, flow_00..flow_07, eight frames apart:
#                    the streaks must move down from one to the next.
```

- After the ice-id lookup, add the water id the same way:

```gdscript
	var water := 0
	for m in world.material_table():
		if m["name"] == "water":
			water = m["id"]
```

- Before the final `else:` of the case chain add:

```gdscript
	elif case_name == "pond":
		var p := ground_under(world, cam.x + dir.x * 12.0, cam.z + dir.z * 12.0)
		world.hooks().debug_apply_sphere_subtract(p, 4.0)
		world.hooks().debug_apply_sphere_add(p - Vector3(0.0, 19.6, 0.0), 20.0, water)
		camera.look_at(p, Vector3.UP)
		placed.append("pond r=4 @%s" % p)
	elif case_name == "wall" or case_name == "flow":
		var p := ground_under(world, cam.x + dir.x * 10.0, cam.z + dir.z * 10.0) \
				+ Vector3(0.0, 3.5, 0.0)
		world.hooks().debug_apply_sphere_add(p, 3.0, water)
		camera.look_at(p, Vector3.UP)
		placed.append("water ball r=3 @%s" % p)
	elif case_name == "underwater":
		var p := ground_under(world, cam.x + dir.x * 7.0, cam.z + dir.z * 7.0) + Vector3(0.0, 1.0, 0.0)
		world.hooks().debug_apply_sphere_add(p, 6.0, water)
		player.global_transform = Transform3D(Basis.IDENTITY, p - Vector3(0.0, 1.6, 0.0))
		camera.transform = Transform3D(Basis.looking_at(dir + Vector3(0.0, 0.8, 0.0), Vector3.UP),
				Vector3(0.0, 1.6, 0.0))
		cam = p
		placed.append("underwater r=6 @%s" % p)
	elif case_name == "water_seam":
		var eye := home + Vector3(0.0, 45.0, 0.0)
		player.global_transform = Transform3D(Basis.IDENTITY, home)
		camera.transform = Transform3D(Basis.looking_at(dir, Vector3.UP), Vector3(0.0, 45.0, 0.0))
		var aim_point := ground_under(world, eye.x + dir.x * 90.0, eye.z + dir.z * 90.0)
		camera.look_at(eye + (aim_point - eye).normalized())
		cam = eye
		for x in range(20, 150, 6):
			world.hooks().debug_apply_sphere_add(
					ground_under(world, cam.x + dir.x * x, cam.z + dir.z * x), 5.0, water)
		placed.append("water chain r=5 every 6m from 20m to 146m")
```

- Update the unknown-case error message to list `seam, foliage, inside, sky, linger, pond, wall, underwater, water_seam, flow`.
- Replace `await save_png(out, case_name)` with:

```gdscript
	if case_name == "flow":
		for k in range(8):
			await save_png(out, "flow_%02d" % k) # save_png itself waits 8 frames: 8/60 s apart
	else:
		await save_png(out, case_name)
```

- [ ] **Step 3: Capture and look**

Run each, one at a time (two Godot processes sharing the GPU measure each other):

```bash
for c in pond wall underwater water_seam flow; do
	godot --path . --resolution 1600x900 -s res://tools/transparency_capture.gd -- --out=reports/water --case=$c
done
```

Expected: each prints `TRANSPARENCY_CAPTURE case=...` and writes PNGs under `reports/water/` (git-ignored).

Open every PNG with the Read tool, at full size and as 5× nearest-neighbour crops (`magick reports/water/pond.png -crop 320x180+640+360 -filter point -resize 500% /tmp/...` or similar). For each, write down what is actually seen:
- `pond`: teal deepening toward the centre, ground visible through the edges, foam at the rim, ripples, at least one glint.
- `wall`: the ball reads as water, with streaks on its sides and calmer ripples on top.
- `underwater`: fogged, not black; a brighter window overhead; darker beyond it.
- `water_seam`: water reads as water on both sides of the fade band; no double-dark band or missing-pixel line.
- `flow_00`..`flow_07`: pick one streak on the ball's side and follow it across the eight frames. It must move DOWN. This is the "always flows downward" check a still cannot make.

If anything is wrong, fix it in the owning task's file, re-run that task's tests, and re-capture. Do not record a claim nobody looked at.

- [ ] **Step 4: Cost, interleaved A/B/A**

Run sequentially:

```bash
tools/run_benchmarks.sh water-A1 --near-scale=0.40 --ice=3
tools/run_benchmarks.sh water-B  --near-scale=0.40 --water=3
tools/run_benchmarks.sh water-A2 --near-scale=0.40 --ice=3
```

Expected: each leg prints `benchmark: ice r=3.0 at ...` or `benchmark: water r=3.0 at ...` (same position in all three) and a steady-leg summary. Record p50, p95, p99, frame_avg and max for each leg; the delta is `B − mean(A1, A2)` and is quotable only where it exceeds the A/A spread. Frame time only: GPU pass timings are invalid on this machine.

- [ ] **Step 5: Record deviations and measurements in the spec**

Append to `docs/superpowers/specs/2026-10-06-water-voxels-design.md` a `## 12. Deviations and measurements recorded during implementation` section containing:
- `### 12.1 The plan's deviations, decided while planning`: this plan's "Deviations From The Spec" list, copied verbatim.
- `### 12.2 Deviations the plan did not foresee`: every change made during implementation that the plan's File Structure or task text did not describe, one numbered item each, with the reason. Write "None." if there were none.
- `### 12.3 Cost`: the three-leg table from Step 4, the delta, the A/A spread, and whether each delta is quotable.
- `### 12.4 What the visual check actually inspected`: one row per capture from Step 3, saying what was seen, and what a still cannot establish (motion-only seam flicker; nobody drove the live demo unless someone did).
- Change the spec's `**Status:**` line to `implemented`.

- [ ] **Step 6: Full verification**

Run: `./build.sh --test`
Expected: `Status: SUCCESS!`

Run: `./gdunit_tests.sh 2>&1 | tee "$TMPDIR/water-final.txt" | tail -40`
Expected: the failing set equals the Task 0 baseline list, with no new failures. If a suite fails that is not on the list, stash the branch's changes, re-run that suite on the clean tree, and record the result in §12.2 either way.

- [ ] **Step 7: Commit**

```bash
git add demo/benchmark.gd tools/transparency_capture.gd \
	docs/superpowers/specs/2026-10-06-water-voxels-design.md
git commit -m "chore: water benchmark flag and captures, with measured cost and recorded deviations"
```

Then use superpowers:finishing-a-development-branch.
