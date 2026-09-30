# Transparent Materials Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clear, tinted, see-through voxel materials (ice first, water later as one table row), rendered in both the raymarched near field and the meshed far field.

**Architecture:** Transparency is a column in the material table. The near-field marcher, on hitting a transparent label, walks the labels through the medium to the surface behind. It writes that surface into the existing G-buffer and records the transparent front in two new march-resolution targets. The far field contours two meshes per chunk: an opaque mesh from a derived lattice with transparent samples pushed outside, and a shell of transparent-sided quads. The shell draws into a full-resolution front layer. A compute pass after `deferred` blends the lit surface behind with the front: Beer–Lambert tint, cel-shaded body and Fresnel sky.

**Tech Stack:** C++20 GDExtension (godot-cpp, Godot 4.7 RenderingDevice), GLSL 460 compute and raster shaders, doctest (native), gdUnit4 (GPU).

**Spec:** `docs/superpowers/specs/2026-09-29-transparent-materials-design.md`

## Global Constraints

- No `Co-Authored-By` or other AI attribution lines in any commit message. Plain conventional commits (`feat:`, `fix:`, `test:`, `docs:`, `chore:`), as the repo history does.
- Transparency is generic: no shader, pass or C++ branch names "ice". Everything keys off `transmit` / `mat_transparent`.
- Scenes with no transparent material must render bit-for-bit as before, with `enabled` on and off. The existing goldens (`tests/test_*_golden.gd`) are the pin.
- No edits to `BeautySettings`, `shade.glslh` or `composite.frag.glsl`. The only `deferred.comp.glsl` edit is a verbatim move of two functions into `shaders/sun_map.glslh` (Task 8).
- Generated files are never hand-edited. `shaders/generated/*` are regenerated with `cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests`. `shaders/material_table.glslh` is replaced with the text its failing test prints.
- Every new `.glsl` file gets its Godot `.import` sidecar committed once the editor or test run has created it (the repo tracks them; see commit `2fdaf81`).
- Tabs for indentation in C++, GLSL and GDScript, matching the surrounding files.
- Build: `./build.sh`. Native tests: `cd extension && scons -Q test` (or a single case: `cd extension && ./build/tests/ve_tests -tc="<name>"`). GPU tests: `./gdunit_tests.sh -a res://tests/<file>.gd`.
- Settings defaults, verbatim from the spec: `enabled = true`, `min_step_m = 0.05`, `max_steps = 48`, `min_transmit = 0.01`. Ice `transmit {0.80, 0.90, 0.95}`, `ior 1.31`. Ice_crack `transmit {0.55, 0.65, 0.70}`, `ior 1.31`. All others `{0,0,0}`, `ior 1.0`.

## Deviations From The Spec (decided while planning; Task 9 records them in the spec)

1. **`composite.frag.glsl` is not edited.** The composite compute pass samples the marcher's march-resolution `front`/`trans` targets directly with a nearest sampler, the same way `composite.frag` samples geometry. This takes one edit out of the beauty stack.
2. **The derived-lattice pass always runs.** It is the identity on chunks with no transparent label, so their quads stay bit-identical. The shell passes early-out per thread on the job's has-transparent bit. This keeps `LodBuildPass::record_job` free of CPU branches on GPU data.
3. **Shell quads share their chunk's pages.** They are appended after the opaque quads and skirts, and are identified by material. The opaque LoD and shadow vertex shaders collapse them, and the transparent raster collapses everything else. There is no new arena range and no new quad bit (all 96 are taken).
4. **The shell list is not HiZ-culled.** It is the CPU walk's visible pages, filtered to those holding a shell quad. Painted ice is rare. `ponytail:` note in code.
5. **Grass and leaf sun marches always see through transparency.** Only the raymarcher's march honours `enabled`.
6. **With `enabled` off, far-field ice still casts no sun-map shadow.** The shadow vertex shader always drops transparent quads.
7. **The composite pass is not zero-cost when enabled.** It reads one or two texels per pixel and early-outs where there is no front. The benchmark in Task 9 reports its measured stage time.

## Review Focus

1. **Camera inside ice.** The primary march starts inside a transparent solid, so the "front" is at t≈0. Expect a see-through view, not black or NaN. The test is in Task 5.
2. **Extreme settings.** `max_steps = 1`, `min_transmit = 0.5` and `min_step_m = 1.0` must still produce finite output, with T = 0 where the walk gave up. The test is in Task 5.
3. **Sky behind ice.** A floating ice ball seen against the sky: the walk exits into air and the resumed march misses. Expect the sky tinted through the ice, not a hole. The test is in Task 5.
4. **Painted ice.** An `OP_SPHERE_PAINT` relabels ground as ice in place, so the front and the medium come from a relabel with no added geometry. Expect the ground below the painted lens to show through. The test is in Task 5.
5. **Removing the ice.** A later subtract through the ice must drop the far shell, so the page bookkeeping must forget released pages. Expect `transparent_pages` back to 0. The test is in Task 7.

---

## File Structure

**Create:**
- `extension/src/transparency/transparency_settings.h`, `transparency_settings.cpp`: the `TransparencySettings` struct, its rows and clamp.
- `extension/src/transparency/transparency_settings_store.h`: the module's own settings store.
- `extension/tests/test_transparency_settings.cpp`: store and clamp tests.
- `extension/tests/test_lod_transparency.cpp`: tests for the CPU opaque lattice, shell contour and shell append.
- `shaders/lod_opaque.comp.glsl`: the derived opaque lattice (GPU mirror of `ve::lod_opaque_lattice`).
- `shaders/transparent_walk.glslh`: the label walk, shared by the primary ray and the sun march.
- `shaders/transparent.vert.glsl`, `shaders/transparent.frag.glsl`: the far-field shell raster.
- `shaders/transparency_composite.comp.glsl`: blends the lit surface behind with the transparent front.
- `shaders/sun_map.glslh`: `sun_cascade_of` and `sun_map_visibility`, moved verbatim out of `deferred.comp.glsl`.
- `extension/src/render/transparent_raster_pass.h`, `.cpp`: owns the full-resolution front layer and draws the shell.
- `extension/src/render/transparency_composite_pass.h`, `.cpp`: the composite compute pass.
- `tests/test_transparency.gd`: the GPU end-to-end suite.

**Modify:**
- `extension/src/world/material_table.h`, `.cpp`, `shaders/material_table.glslh`, and the tests `test_material_table.cpp` and `test_material_glslh.cpp`: add `transmit`, `ior` and the lookups.
- `extension/SConstruct`: add `src/transparency/*.cpp` to the pure test sources.
- `extension/src/render/orchestrator.h`, `.cpp`, `extension/src/voxel_world.h`, `.cpp`: the settings group, the two passes, and `set_transparency_value` / `get_transparency_value`.
- `extension/src/lod/lod_reduce.h`, `.cpp`, `extension/src/lod/lod_contour.h`, `.cpp`: add the CPU mirrors.
- `shaders/lod_reduce.comp.glsl`, `shaders/lod_quads.comp.glsl`, `extension/src/render/lod_build_pass.h`, `.cpp`: the opaque lattice, the shell, and a counts stride of 4.
- `extension/src/debug/hooks_lod.cpp`, `tests/test_lod_mesh_diff.gd`: the diff covers the opaque and shell meshes.
- `shaders/lod.vert.glsl`, `shaders/lod_shadow.vert.glsl`, `extension/src/render/lod_raster_pass.h`, `.cpp`: drop transparent quads from the opaque and shadow draws.
- `extension/src/lod/lod_system.h`, `.cpp`: track which pages hold shell quads, and publish that list.
- `shaders/sun_march.glslh`, `shaders/raymarch.comp.glsl`, `extension/src/render/raymarch_pass.h`, `.cpp`: the near-field walker.
- `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh`: the three new blocks.
- `extension/src/render/frame.h`, `.cpp`: two stages and their wiring.
- `extension/src/debug/hooks.cpp`: centre-pixel readouts on `debug_render_frame`.
- `shaders/deferred.comp.glsl`, `extension/src/render/deferred_pass.h`: the verbatim move, and a `sun_cascade_ubo()` accessor.
- `demo/benchmark.gd`: the `--transparency=` and `--ice=` flags.

---

### Task 0: Baseline

**Files:** none changed.

- [ ] **Step 1: Branch**

```bash
git checkout -b feat/transparent-materials
```

- [ ] **Step 2: Native tests green**

Run: `./build.sh --test`
Expected: build OK, doctest summary `Status: SUCCESS!`.

- [ ] **Step 3: Record the gdUnit baseline**

The set of suites that fail on a clean main drifts (memory: gdunit-baseline-failures). Record it before changing anything:

Run: `./gdunit_tests.sh 2>&1 | tee /private/tmp/claude-501/transparency-baseline.txt | tail -40`
Expected: a summary listing the failing suites. Save the list of failing suite names. Every later "no new failures" check compares against this list, not against zero.

---

### Task 1: Transparency in the material table

**Files:**
- Modify: `extension/src/world/material_table.h` (struct `MaterialDef`, table `kMaterials`, declarations near `material_glow`)
- Modify: `extension/src/world/material_table.cpp` (lookups; `material_table_glsl()`)
- Modify: `shaders/material_table.glslh` (regenerated)
- Test: `extension/tests/test_material_table.cpp`, `extension/tests/test_material_glslh.cpp`

**Interfaces:**
- Produces (C++, namespace `ve`): `bool material_transparent(uint16_t id)`, `void material_transmit(uint16_t id, float out[3])`, `float material_ior(uint16_t id)`, and `MaterialDef::transmit[3]`, `MaterialDef::ior`.
- Produces (GLSL, in every file that includes `common.glslh`): `bool mat_transparent(uint id)`, `vec3 mat_transmit(uint id)`, `float mat_ior(uint id)`.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_material_table.cpp`:

```cpp
TEST_CASE("transparency is a table property: the ice rows are clear, everything else opaque") {
	CHECK(ve::material_transparent(ve::material_id("ice")));
	CHECK(ve::material_transparent(ve::material_id("ice_crack")));
	CHECK_FALSE(ve::material_transparent(ve::material_id("rock")));
	CHECK_FALSE(ve::material_transparent(ve::material_id("bark")));
	CHECK_FALSE(ve::material_transparent(0));                   // air
	CHECK_FALSE(ve::material_transparent(ve::kFoliageBase));    // foliage has no row
	CHECK_FALSE(ve::material_transparent(9999));
	float t[3];
	ve::material_transmit(ve::material_id("ice"), t);
	CHECK(t[0] == doctest::Approx(0.80f));
	CHECK(t[1] == doctest::Approx(0.90f));
	CHECK(t[2] == doctest::Approx(0.95f));
	ve::material_transmit(0, t);
	CHECK(t[0] == 0.0f);
	CHECK(ve::material_ior(ve::material_id("ice")) == doctest::Approx(1.31f));
	CHECK(ve::material_ior(0) == doctest::Approx(1.0f));
}

// A transmit of 1 would never attenuate, and the walk's cutoff (spec §4 case 3) would
// never fire: the step cap would be the only thing ending a walk through it.
TEST_CASE("every transmit is in [0, 1) and every ior at least 1") {
	for (int i = 0; i < ve::kMaterialCount; i++) {
		for (float t : ve::kMaterials[i].transmit) {
			CHECK(t >= 0.0f);
			CHECK(t < 1.0f);
		}
		CHECK(ve::kMaterials[i].ior >= 1.0f);
	}
}
```

Append to `extension/tests/test_material_glslh.cpp`:

```cpp
TEST_CASE("the emitter carries transparency and names no material after its tables") {
	const std::string s = ve::material_table_glsl();
	CHECK(s.find("const vec3 MAT_TRANSMIT[MATERIAL_COUNT]") != std::string::npos);
	CHECK(s.find("const float MAT_IOR[MATERIAL_COUNT]") != std::string::npos);
	CHECK(s.find("bool mat_transparent(uint id)") != std::string::npos);
	CHECK(s.find("vec3 mat_transmit(uint id)") != std::string::npos);
	CHECK(s.find("float mat_ior(uint id)") != std::string::npos);
	for (int i = 0; i < ve::kMaterialCount; i++) {
		std::string upper = ve::kMaterials[i].name;
		for (char &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		CHECK(upper != "TRANSMIT");
		CHECK(upper != "IOR");
	}
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd extension && scons -Q test`
Expected: compile error: `'material_transparent' is not a member of 've'` (and `transmit` / `ior` are not members of `MaterialDef`).

- [ ] **Step 3: Add the columns and rows**

In `extension/src/world/material_table.h`, extend `MaterialDef` (after `flat_albedo`) with defaulted members, so opaque rows need no edit:

```cpp
	float flat_albedo[3];  // far-field and unknown-layer fallback
	// Transparency (docs/superpowers/specs/2026-09-29-transparent-materials-design.md §3).
	// The fraction of light per channel left after one metre of the material; {0,0,0} is
	// opaque, and that is what "transparent" means everywhere: any channel above zero.
	float transmit[3] = {0.0f, 0.0f, 0.0f};
	float ior = 1.0f;      // index of refraction, for Fresnel reflectance; unused when opaque
```

Replace the two ice rows in `kMaterials`:

```cpp
	{"ice_crack",    "05",  1.8f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.61f, 0.65f, 0.68f},
			{0.55f, 0.65f, 0.70f}, 1.31f},
	{"ice",          "06",  1.8f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.61f, 0.65f, 0.68f},
			{0.80f, 0.90f, 0.95f}, 1.31f},
```

Next to `float material_glow(uint16_t id);` declare:

```cpp
// Transparency lookups, failing soft like the rest: air, foliage and any id with no row are
// opaque (transmit 0, ior 1). Mirrored in GLSL as mat_transparent / mat_transmit / mat_ior.
bool material_transparent(uint16_t id);
void material_transmit(uint16_t id, float out[3]);
float material_ior(uint16_t id);
```

- [ ] **Step 4: Implement the lookups and emit the GLSL**

In `extension/src/world/material_table.cpp`, after `material_glow`:

```cpp
bool material_transparent(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i < 0 || i >= kMaterialCount) return false;
	const float *t = kMaterials[i].transmit;
	return t[0] > 0.0f || t[1] > 0.0f || t[2] > 0.0f;
}

void material_transmit(uint16_t id, float out[3]) {
	const int i = static_cast<int>(id) - 1;
	for (int k = 0; k < 3; k++)
		out[k] = (i >= 0 && i < kMaterialCount) ? kMaterials[i].transmit[k] : 0.0f;
}

float material_ior(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	return (i >= 0 && i < kMaterialCount) ? kMaterials[i].ior : 1.0f;
}
```

In `material_table_glsl()`, right after the `MAT_FLAT_ALBEDO` block (before the foliage comment), emit the two tables:

```cpp
	o << "const vec3 MAT_TRANSMIT[MATERIAL_COUNT] = vec3[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << vec3(kMaterials[i].transmit) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "const float MAT_IOR[MATERIAL_COUNT] = float[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << f(kMaterials[i].ior) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";
```

At the end of the emitted text, replace the final `"}\n";` of `mat_glow_rgb` with that closing brace followed by the three lookups:

```cpp
	     "\treturn (j >= 0 && j < FOLIAGE_COUNT) ? FOLIAGE_GLOW_RGB[j] : vec3(0.0);\n"
	     "}\n\n"
	     "// Mirror of ve::material_transparent: air, foliage and any id with no table entry are\n"
	     "// opaque. Transparent means any channel of the per-metre transmittance is above zero.\n"
	     "bool mat_transparent(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn i >= 0 && i < MATERIAL_COUNT && any(greaterThan(MAT_TRANSMIT[i], vec3(0.0)));\n"
	     "}\n\n"
	     "vec3 mat_transmit(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i >= 0 && i < MATERIAL_COUNT) ? MAT_TRANSMIT[i] : vec3(0.0);\n"
	     "}\n\n"
	     "float mat_ior(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i >= 0 && i < MATERIAL_COUNT) ? MAT_IOR[i] : 1.0;\n"
	     "}\n";
```

- [ ] **Step 5: Regenerate the mirror**

Run: `cd extension && scons -Q test 2>&1 | sed -n '/Replace its entire contents with:/,/^=====/p' | head -200`

Replace the whole of `shaders/material_table.glslh` with the printed text: everything after "Replace its entire contents with:" up to the doctest separator. The new tables must read, for example:

```
	vec3(0.550000, 0.650000, 0.700000), // ice_crack
	vec3(0.800000, 0.900000, 0.950000), // ice
```

and

```
	1.310000, // ice_crack
	1.310000, // ice
	1.000000 // bark
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`, including "the committed GLSL mirror matches the C++ table".

- [ ] **Step 7: Build the extension (every shader that includes common.glslh now sees the lookups)**

Run: `./build.sh`
Expected: `Build OK`.

- [ ] **Step 8: Commit**

```bash
git add extension/src/world/material_table.h extension/src/world/material_table.cpp \
	shaders/material_table.glslh extension/tests/test_material_table.cpp \
	extension/tests/test_material_glslh.cpp
git commit -m "feat: transmit and ior material columns; ice and ice_crack are transparent"
```

---

### Task 2: The transparency settings module

**Files:**
- Create: `extension/src/transparency/transparency_settings.h`, `extension/src/transparency/transparency_settings.cpp`, `extension/src/transparency/transparency_settings_store.h`
- Create: `extension/tests/test_transparency_settings.cpp`
- Modify: `extension/SConstruct` (the `pure_sources` glob list)
- Modify: `extension/src/render/orchestrator.h` (include, accessors, member), `extension/src/render/orchestrator.cpp` (`settings_group`)
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (bind the script accessors)

**Interfaces:**
- Produces: `ve::TransparencySettings { bool enabled; float min_step_m; int max_steps; float min_transmit; }`, `ve::transparency_rows()`, `ve::clamp_transparency_settings(TransparencySettings*)`, `ve::TransparencySettingsStore`.
- Produces: `RenderOrchestrator::transparency_settings() const -> ve::TransparencySettings`, `set_transparency_value(const char*, float)`, `transparency_value(const char*)`. Settings group name `"transparency"`.
- Produces (GDScript): `VoxelWorld.set_transparency_value(name: String, value: float) -> bool`, `get_transparency_value(name: String) -> float`.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_transparency_settings.cpp`:

```cpp
#include <doctest/doctest.h>
#include "transparency/transparency_settings.h"
#include "transparency/transparency_settings_store.h"
#include <cmath>

TEST_CASE("transparency defaults are the spec's and sit inside their own clamp") {
	ve::TransparencySettings s;
	CHECK(s.enabled);
	CHECK(s.min_step_m == doctest::Approx(0.05f));
	CHECK(s.max_steps == 48);
	CHECK(s.min_transmit == doctest::Approx(0.01f));
	ve::TransparencySettings c = s;
	ve::clamp_transparency_settings(&c);
	CHECK(c.min_step_m == doctest::Approx(s.min_step_m));
	CHECK(c.max_steps == s.max_steps);
	CHECK(c.min_transmit == doctest::Approx(s.min_transmit));
}

// The walker divides nothing by these, but a zero step never advances and a zero cap never
// walks: both would turn the medium into a wall. A NaN must not reach the shader either.
TEST_CASE("transparency clamp keeps the walk able to move") {
	ve::TransparencySettings s;
	s.min_step_m = 0.0f / 0.0f;
	s.max_steps = -5;
	s.min_transmit = 7.0f;
	ve::clamp_transparency_settings(&s);
	CHECK(s.min_step_m >= 0.01f);
	CHECK(s.min_step_m <= 1.0f);
	CHECK(s.max_steps >= 1);
	CHECK(s.max_steps <= 256);
	CHECK(s.min_transmit <= 0.5f);
	CHECK(s.min_transmit >= 0.0f);
}

TEST_CASE("the transparency store round-trips every knob and clamps on the way in") {
	ve::TransparencySettingsStore store;
	CHECK(store.set_value("enabled", 0.0f));
	CHECK(store.get().enabled == false);
	CHECK(store.set_value("min_step_m", 0.2f));
	CHECK(store.value("min_step_m") == doctest::Approx(0.2f));
	CHECK(store.set_value("max_steps", 1e9f));
	CHECK(store.value("max_steps") <= 256.0f);
	CHECK(store.set_value("min_transmit", 0.05f));
	CHECK(store.value("min_transmit") == doctest::Approx(0.05f));
	CHECK_FALSE(store.set_value("no_such_knob", 1.0f));
}
```

Add `Glob("src/transparency/*.cpp")` to `pure_sources` in `extension/SConstruct`, after `Glob("src/leaves/*.cpp")`:

```python
                Glob("src/grass/*.cpp") + Glob("src/leaves/*.cpp") +
                Glob("src/transparency/*.cpp") + Glob("src/gpu_layout/*.cpp") +
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd extension && scons -Q test`
Expected: compile error, `transparency/transparency_settings.h: No such file or directory`.

- [ ] **Step 3: Write the module**

Create `extension/src/transparency/transparency_settings.h`:

```cpp
#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Transparent-material knobs (docs/superpowers/specs/2026-09-29-transparent-materials-design.md
// §8). DELIBERATELY its own module and store, like grass and leaves: nothing here joins
// BeautySettings. What a material lets through is a material-table column, not a knob here.
struct TransparencySettings {
	// Off: the marcher hits transparent materials as opaque, the far shell and the composite
	// do not run, and the near field renders exactly as it did before transparency existed.
	// Also the benchmark's A/B switch.
	bool enabled = true;
	// The walk's smallest step, metres. The real step is max(this, the pixel footprint at
	// that distance), so 5 cm is paid only close to the camera (spec §4).
	float min_step_m = 0.05f;
	// Safety cap on walk steps. Hitting it means the medium counts as fully absorbed.
	int max_steps = 48;
	// The walk ends as soon as the brightest channel of the transmittance drops under this:
	// nothing behind is visible any more, so nothing behind is marched.
	float min_transmit = 0.01f;
};

std::span<const SettingRow<TransparencySettings>> transparency_rows();

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_transparency_settings(TransparencySettings *s);

} // namespace ve
```

Create `extension/src/transparency/transparency_settings.cpp`:

```cpp
#include "transparency/transparency_settings.h"

namespace ve {
namespace {

const SettingRow<TransparencySettings> kTransparencyRows[] = {
	bool_row("enabled", "Transparency", &TransparencySettings::enabled),
	// Floored above zero: a zero step never advances, and the walk would spend its whole
	// cap on one sample.
	float_row("min_step_m", "Min step (m)", &TransparencySettings::min_step_m, 0.01f, 1.0f,
			0.01f, 0.5f, 0.01f),
	int_row("max_steps", "Max steps", &TransparencySettings::max_steps, 1, 256, 1, 128),
	float_row("min_transmit", "Min transmit", &TransparencySettings::min_transmit, 0.0f, 0.5f,
			0.0f, 0.1f, 0.001f),
};

} // namespace

std::span<const SettingRow<TransparencySettings>> transparency_rows() { return kTransparencyRows; }

void clamp_transparency_settings(TransparencySettings *s) {
	clamp_all<TransparencySettings>(kTransparencyRows, s);
}

} // namespace ve
```

Create `extension/src/transparency/transparency_settings_store.h`:

```cpp
#pragma once
#include "transparency/transparency_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Transparency is its own module with its own store, mirroring LeafSettingsStore without
// joining it. No godot-cpp -- this file is in the native test target.
class TransparencySettingsStore : public SettingsStore<TransparencySettings> {
public:
	TransparencySettingsStore() :
			SettingsStore(transparency_rows(), nullptr, TransparencySettings{}) {}
};

} // namespace ve
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`.

- [ ] **Step 5: Wire the store into the orchestrator and VoxelWorld**

`extension/src/render/orchestrator.h`: next to `#include "leaves/leaf_settings_store.h"` add `#include "transparency/transparency_settings_store.h"`. Beside the three leaf accessors (orchestrator.h:247-249) add:

```cpp
	ve::TransparencySettings transparency_settings() const { return transparency_settings_.get(); }
	bool set_transparency_value(const char *n, float v) { return transparency_settings_.set_value(n, v); }
	float transparency_value(const char *n) const { return transparency_settings_.value(n); }
```

Beside `ve::LeafSettingsStore leaf_settings_;` add `ve::TransparencySettingsStore transparency_settings_;`.

`extension/src/render/orchestrator.cpp`, in `settings_group`, after the `"leaves"` line:

```cpp
	if (std::strcmp(name, "transparency") == 0) return &transparency_settings_;
```

`extension/src/voxel_world.h`, beside `set_leaf_value` / `get_leaf_value`:

```cpp
	bool set_transparency_value(const String &name, float v);
	float get_transparency_value(const String &name) const;
```

`extension/src/voxel_world.cpp`, beside the leaf bindings (voxel_world.cpp:227-228):

```cpp
	ClassDB::bind_method(D_METHOD("set_transparency_value", "name", "value"), &VoxelWorld::set_transparency_value);
	ClassDB::bind_method(D_METHOD("get_transparency_value", "name"), &VoxelWorld::get_transparency_value);
```

and beside the leaf definitions (voxel_world.cpp:335-341):

```cpp
bool VoxelWorld::set_transparency_value(const String &name, float v) {
	return context_.render->set_transparency_value(name.utf8().get_data(), v);
}

float VoxelWorld::get_transparency_value(const String &name) const {
	return context_.render->transparency_value(name.utf8().get_data());
}
```

- [ ] **Step 6: Build**

Run: `./build.sh`
Expected: `Build OK`.

- [ ] **Step 7: Commit**

```bash
git add extension/src/transparency extension/tests/test_transparency_settings.cpp \
	extension/SConstruct extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp \
	extension/src/voxel_world.h extension/src/voxel_world.cpp
git commit -m "feat: transparency settings module and store"
```

---

### Task 3: CPU mirrors, the opaque lattice and the shell

**Files:**
- Modify: `extension/src/lod/lod_reduce.h`, `extension/src/lod/lod_reduce.cpp`
- Modify: `extension/src/lod/lod_contour.h`, `extension/src/lod/lod_contour.cpp`
- Create: `extension/tests/test_lod_transparency.cpp`

**Interfaces:**
- Consumes: `ve::material_transparent(uint16_t)` (Task 1).
- Produces (namespace `ve`):
  - `bool lod_has_transparent(const uint8_t *lattice, const uint16_t *material)`
  - `uint8_t lod_outside_byte(float cell_size)`
  - `void lod_opaque_lattice(const uint8_t *lattice, const uint16_t *material, float cell_size, uint8_t *out)`
  - `void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out, bool shell_only = false)`
  - `bool lod_append_shell(std::vector<LodQuad> *quads, std::vector<LodQuadNormals> *normals, const std::vector<LodQuad> &shell, const std::vector<LodQuadNormals> &shell_normals)`: true when shell quads were dropped
  - `bool lod_quads_have_transparent(const LodQuad *quads, int count)`

- [ ] **Step 1: Write the failing tests**

Create `extension/tests/test_lod_transparency.cpp`:

```cpp
#include <doctest/doctest.h>
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_reduce.h"
#include "world/brick.h"
#include "world/material_table.h"
#include <vector>

namespace {

constexpr int kN = ve::kLodChunkLattice;
constexpr int kCount = kN * kN * kN;

// An L0 lattice (0.4 m cells) holding a horizontal ground plane at lattice y = 16.5: samples
// y <= 16 are solid rock. `ice_from` relabels solid samples with y >= ice_from as ice, so
// the ice is a slab resting on rock, meeting it with NO surface between them.
void ground(std::vector<uint8_t> *sdf, std::vector<uint16_t> *mat, int ice_from) {
	sdf->assign(kCount, 0);
	mat->assign(kCount, 0);
	for (int z = 0; z < kN; z++)
		for (int y = 0; y < kN; y++)
			for (int x = 0; x < kN; x++) {
				const int i = ve::lod_lattice_index(x, y, z);
				(*sdf)[i] = ve::lod_encode_sdf((float(y) - 16.5f) * 0.4f, 0.4f);
				if (y <= 16) (*mat)[i] = y >= ice_from ? ve::material_id("ice") : ve::material_id("rock");
			}
}

bool all_axis_y_at(const ve::LodContourResult &r, int u_y, uint16_t material) {
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		if (f.axis != 1 || f.u[1] != u_y || f.material != material) return false;
	}
	return !r.quads.empty();
}

} // namespace

TEST_CASE("the opaque lattice is the identity on a chunk with no transparent label") {
	std::vector<uint8_t> sdf, out(kCount);
	std::vector<uint16_t> mat;
	ground(&sdf, &mat, 999);
	CHECK_FALSE(ve::lod_has_transparent(sdf.data(), mat.data()));
	ve::lod_opaque_lattice(sdf.data(), mat.data(), 0.4f, out.data());
	CHECK(out == sdf);
}

TEST_CASE("the outside byte decodes as just outside at every level") {
	for (int level = 0; level < 8; level++) {
		const float d = ve::decode_sdf(ve::lod_outside_byte(ve::lod_cell_size(level)));
		CHECK(d > 0.0f);
		CHECK(d <= ve::kSdfRange);
	}
}

TEST_CASE("an ice slab on rock: opaque mesh at the rock top, shell at the ice top") {
	std::vector<uint8_t> sdf, opaque(kCount);
	std::vector<uint16_t> mat;
	ground(&sdf, &mat, 12); // rock y <= 11, ice 12..16, air above
	CHECK(ve::lod_has_transparent(sdf.data(), mat.data()));
	ve::lod_opaque_lattice(sdf.data(), mat.data(), 0.4f, opaque.data());

	// Lattice y = 11 -> 12 is the new crossing; the edge's owning u is lattice index - 1.
	ve::LodContourResult terrain;
	ve::lod_contour(opaque.data(), mat.data(), &terrain);
	CHECK(terrain.quads.size() == size_t(ve::kLodChunkCells * ve::kLodChunkCells));
	CHECK(all_axis_y_at(terrain, 10, ve::material_id("rock")));

	// The shell is the ORIGINAL surface (y = 16 -> 17), and only its transparent quads.
	ve::LodContourResult shell;
	ve::lod_contour(sdf.data(), mat.data(), &shell, true);
	CHECK(shell.quads.size() == size_t(ve::kLodChunkCells * ve::kLodChunkCells));
	CHECK(all_axis_y_at(shell, 15, ve::material_id("ice")));
	CHECK(ve::lod_quads_have_transparent(shell.quads.data(), int(shell.quads.size())));
	CHECK_FALSE(ve::lod_quads_have_transparent(terrain.quads.data(), int(terrain.quads.size())));
}

TEST_CASE("shell_only on a chunk with no transparent label emits nothing, and the default is unchanged") {
	std::vector<uint8_t> sdf;
	std::vector<uint16_t> mat;
	ground(&sdf, &mat, 999);
	ve::LodContourResult shell, plain, plain_again;
	ve::lod_contour(sdf.data(), mat.data(), &shell, true);
	CHECK(shell.quads.empty());
	ve::lod_contour(sdf.data(), mat.data(), &plain);
	ve::lod_contour(sdf.data(), mat.data(), &plain_again, false);
	REQUIRE(plain.quads.size() == plain_again.quads.size());
	for (size_t i = 0; i < plain.quads.size(); i++)
		for (int k = 0; k < 3; k++) CHECK(plain.quads[i].w[k] == plain_again.quads[i].w[k]);
}

TEST_CASE("the shell is appended after the opaque quads and never outgrows a chunk") {
	std::vector<ve::LodQuad> quads(size_t(ve::kLodMaxQuadsPerChunk - 2));
	std::vector<ve::LodQuadNormals> normals(quads.size());
	std::vector<ve::LodQuad> shell(5);
	std::vector<ve::LodQuadNormals> shell_normals(5);
	shell[0].w[0] = 0xABCDu;
	CHECK(ve::lod_append_shell(&quads, &normals, shell, shell_normals));
	CHECK(quads.size() == size_t(ve::kLodMaxQuadsPerChunk));
	CHECK(normals.size() == quads.size());
	CHECK(quads[size_t(ve::kLodMaxQuadsPerChunk - 2)].w[0] == 0xABCDu);

	std::vector<ve::LodQuad> small(3);
	std::vector<ve::LodQuadNormals> small_n(3);
	CHECK_FALSE(ve::lod_append_shell(&small, &small_n, shell, shell_normals));
	CHECK(small.size() == 8);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd extension && scons -Q test`
Expected: compile error, `'lod_has_transparent' is not a member of 've'`.

- [ ] **Step 3: Implement the lattice mirrors**

Append to `extension/src/lod/lod_reduce.h` (inside `namespace ve`):

```cpp
// True when any SOLID sample of a reduced lattice (kLodChunkLattice^3) carries a transparent
// material: the per-job bit lod_reduce.comp.glsl raises, and the only chunks that grow a
// shell (docs/superpowers/specs/2026-09-29-transparent-materials-design.md §5).
bool lod_has_transparent(const uint8_t *lattice, const uint16_t *material);

// The encoded "just outside" value a transparent sample becomes in the opaque lattice: half
// a cell, in the scaled-distance space lod_encode_sdf stores at this level. L0 stores metres,
// so half a 0.4 m cell is 0.2; coarser levels store kSdfRange per two cells, so half a cell
// is a quarter of the range. Mirror of the constant in lod_opaque.comp.glsl.
uint8_t lod_outside_byte(float cell_size);

// The OPAQUE lattice the far field's terrain mesh is contoured from: every solid sample whose
// material is transparent becomes lod_outside_byte(cell_size), everything else is copied. The
// identity on a chunk with no transparent label. `out` must not alias `lattice`. Mirror of
// shaders/lod_opaque.comp.glsl.
void lod_opaque_lattice(const uint8_t *lattice, const uint16_t *material, float cell_size,
		uint8_t *out);
```

Append to `extension/src/lod/lod_reduce.cpp` (inside `namespace ve`; add `#include "world/material_table.h"` at the top):

```cpp
bool lod_has_transparent(const uint8_t *lattice, const uint16_t *material) {
	constexpr int n = kLodChunkLattice * kLodChunkLattice * kLodChunkLattice;
	for (int i = 0; i < n; i++)
		if (decode_sdf(lattice[i]) <= 0.0f && material_transparent(material[i])) return true;
	return false;
}

uint8_t lod_outside_byte(float cell_size) {
	return encode_sdf(cell_size <= kLodBaseCell ? 0.5f * cell_size : 0.25f * kSdfRange);
}

void lod_opaque_lattice(const uint8_t *lattice, const uint16_t *material, float cell_size,
		uint8_t *out) {
	constexpr int n = kLodChunkLattice * kLodChunkLattice * kLodChunkLattice;
	const uint8_t outside = lod_outside_byte(cell_size);
	for (int i = 0; i < n; i++)
		out[i] = (decode_sdf(lattice[i]) <= 0.0f && material_transparent(material[i]))
				? outside : lattice[i];
}
```

- [ ] **Step 4: Implement the shell filter and append**

In `extension/src/lod/lod_contour.h`, change the `lod_contour` declaration and add the two helpers:

```cpp
// Surface nets over a kLodChunkLattice^3 lattice ... (keep the existing comment), and:
// `shell_only` keeps only the quads whose SOLID endpoint is a transparent material -- the
// far field's transparent shell (docs/superpowers/specs/2026-09-29-transparent-materials-
// design.md §5). The default emits every quad, exactly as before.
void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out,
		bool shell_only = false);

// Appends a chunk's shell quads after its opaque quads AND skirts, keeping the total inside
// kLodMaxQuadsPerChunk so one chunk never outgrows its 16 pages. True when shell quads had
// to be dropped (report it as the chunk's overflow).
bool lod_append_shell(std::vector<LodQuad> *quads, std::vector<LodQuadNormals> *normals,
		const std::vector<LodQuad> &shell, const std::vector<LodQuadNormals> &shell_normals);

// Whether any of these quads is a shell quad. After lod_opaque_lattice no opaque-mesh quad
// can carry a transparent material, so the material alone tells the two meshes apart.
bool lod_quads_have_transparent(const LodQuad *quads, int count);
```

In `extension/src/lod/lod_contour.cpp` (add `#include "world/material_table.h"` and `#include <algorithm>`), change the definition's signature to `void lod_contour(const uint8_t *lattice, const uint16_t *material, LodContourResult *out, bool shell_only)`. In pass 2, move the material lookup up so the filter runs **before** the overflow check. Replace:

```cpp
					const bool sa = da <= 0.0f, sb = db <= 0.0f;
					if (sa == sb) continue;
					if (int(out->quads.size()) >= kLodMaxQuadsPerChunk) {
```

with:

```cpp
					const bool sa = da <= 0.0f, sb = db <= 0.0f;
					if (sa == sb) continue;
					// The material of the SOLID endpoint of the edge: deterministic, and
					// mirrorable in one line of GLSL.
					const uint16_t solid_material = material[lod_lattice_index(
							sa ? L[0] : Lb[0], sa ? L[1] : Lb[1], sa ? L[2] : Lb[2])];
					if (shell_only && !material_transparent(solid_material)) continue;
					if (int(out->quads.size()) >= kLodMaxQuadsPerChunk) {
```

and replace the later material assignment (its comment moved up):

```cpp
					f.material = solid_material;
```

Append the two helpers:

```cpp
bool lod_append_shell(std::vector<LodQuad> *quads, std::vector<LodQuadNormals> *normals,
		const std::vector<LodQuad> &shell, const std::vector<LodQuadNormals> &shell_normals) {
	const size_t room = quads->size() < size_t(kLodMaxQuadsPerChunk)
			? size_t(kLodMaxQuadsPerChunk) - quads->size() : 0;
	const size_t n = std::min({room, shell.size(), shell_normals.size()});
	quads->insert(quads->end(), shell.begin(), shell.begin() + n);
	normals->insert(normals->end(), shell_normals.begin(), shell_normals.begin() + n);
	return n < shell.size();
}

bool lod_quads_have_transparent(const LodQuad *quads, int count) {
	for (int i = 0; i < count; i++) {
		LodQuadFields f{};
		lod_quad_unpack(quads[i], &f);
		if (material_transparent(f.material)) return true;
	}
	return false;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd extension && scons -Q test`
Expected: `Status: SUCCESS!`. The existing `test_lod_contour.cpp` still passes unchanged: the default path emits exactly as before.

- [ ] **Step 6: Commit**

```bash
git add extension/src/lod/lod_reduce.h extension/src/lod/lod_reduce.cpp \
	extension/src/lod/lod_contour.h extension/src/lod/lod_contour.cpp \
	extension/tests/test_lod_transparency.cpp
git commit -m "feat: CPU opaque lattice, shell contour and shell append for the far field"
```

---

### Task 4: The GPU LoD build grows an opaque mesh and a shell

**Files:**
- Create: `shaders/lod_opaque.comp.glsl`
- Modify: `shaders/lod_reduce.comp.glsl`, `shaders/lod_quads.comp.glsl`
- Modify: `extension/src/render/lod_build_pass.h`, `extension/src/render/lod_build_pass.cpp`
- Modify: `extension/src/debug/hooks_lod.cpp` (the `debug_lod_diff` quad step, ~line 840)
- Test: `tests/test_lod_mesh_diff.gd`

**Interfaces:**
- Consumes: `lod_opaque_lattice`, `lod_has_transparent`, `lod_contour(..., true)`, `lod_append_shell` (Task 3); GLSL `mat_transparent` (Task 1).
- Produces: `LodBuildResult::quads` / `normals` now hold opaque quads, then skirts, then shell quads. Consumers downstream are unchanged. The counts buffer is 4 uints per job: `[0]` quad count, `[1]` overflow, `[2]` shell quad count, `[3]` flags (bit 0 has-transparent, bit 1 shell overflow). `LodBuildPush.params.w` is the mode: 0 = opaque mesh, 1 = shell. `debug_lod_diff` gains the key `shell_quads`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_lod_mesh_diff.gd`:

```gdscript
func ice_id(w: VoxelWorld) -> int:
	for m in w.material_table():
		if m["name"] == "ice":
			return m["id"]
	return 0

# Spec §5: a chunk holding ice contours TWO meshes -- terrain from the opaque lattice and a
# shell of transparent-sided quads -- and both must agree GPU-to-CPU exactly as the plain
# mesh does. The shell is what makes the far field see-through; zero shell quads here means
# the reduce never raised the job's transparent bit.
func test_a_chunk_with_ice_agrees_on_both_meshes() -> void:
	var w := make_world()
	var hit: Dictionary = w.raycast(Vector3(25.6, 120.0, 25.6), Vector3.DOWN, 200.0)
	assert_bool(hit["hit"]).is_true()
	var ground: Vector3 = hit["pos"]
	w.hooks().debug_apply_sphere_add(ground + Vector3(0.0, 1.0, 0.0), 3.0, ice_id(w))
	var c := Vector3i(int(floor(ground.x / 12.8)), int(floor(ground.y / 12.8)),
			int(floor(ground.z / 12.8)))
	var d := w.hooks().debug_lod_diff(0, c)
	check_diff(d, "L0 chunk with an ice sphere")
	assert_int(int(d["shell_quads"])).override_failure_message(
		"the ice chunk grew no shell: %s" % d).is_greater(0)

func test_a_chunk_without_ice_grows_no_shell() -> void:
	var w := make_world()
	var d := w.hooks().debug_lod_diff(0, Vector3i(2, 4, 2))
	check_diff(d, "L0 surface")
	assert_int(int(d["shell_quads"])).is_equal(0)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_lod_mesh_diff.gd`
Expected: FAIL. The dictionary has no key `shell_quads` (an invalid-key error or a failed assertion).

- [ ] **Step 3: The shaders**

Create `shaders/lod_opaque.comp.glsl`:

```glsl
#[compute]
#version 460

#include "common.glslh"
#include "lod_common.glslh"

// The OPAQUE lattice the far field's terrain mesh is contoured from
// (docs/superpowers/specs/2026-09-29-transparent-materials-design.md §5). Every solid sample
// whose material is transparent is rewritten to "just outside" -- half a cell in this level's
// scaled-distance space -- and every other sample is copied. On a chunk with no transparent
// label the copy is byte for byte, so its mesh is exactly the one it always was. Mirror of
// ve::lod_opaque_lattice.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, r16ui) readonly uniform uimage3D material;
layout(set = 0, binding = 2, r8) writeonly uniform image3D opaque;

void main() {
	ivec3 i = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(i, ivec3(LOD_CHUNK_LATTICE)))) return;
	float v = imageLoad(lattice, i).r;
	if (decode_sdf(v) <= 0.0 && mat_transparent(imageLoad(material, i).r)) {
		// ve::lod_outside_byte: L0 stores metres, so half a 0.4 m cell is 0.2; coarser levels
		// store SDF_RANGE per two cells, so half a cell is a quarter of the range.
		float half_cell = lpc.grid.w <= 0.4 ? 0.5 * lpc.grid.w : 0.25 * SDF_RANGE;
		v = quantise_sdf(half_cell);
	}
	imageStore(opaque, i, vec4(v));
}
```

`shaders/lod_reduce.comp.glsl`: add the counts binding after `out_mat`:

```glsl
// Four uints per job (LodBuildPass): [3] bit 0 is raised here when the chunk holds a solid
// transparent sample -- the bit the shell passes early-out on. Mirror of ve::lod_has_transparent.
layout(set = 0, binding = 4, std430) buffer Counts { uint v[]; } counts;
```

and at the end of `main`, replace the two stores with:

```glsl
	float stored = quantise_sdf(acc);
	imageStore(out_sdf, i, vec4(stored));
	imageStore(out_mat, i, uvec4(best, 0u, 0u, 0u));
	// Decided on the STORED value, as the CPU mirror sees it, so a sample the byte rounds to
	// air never raises the bit on one side only.
	if (decode_sdf(stored) <= 0.0 && mat_transparent(best))
		atomicOr(counts.v[uint(lpc.job.w) * 4u + 3u], 1u);
```

`shaders/lod_quads.comp.glsl`: update the counts comment to `// Four uints per job: quad count, overflow flag, shell quad count, flags (bit 0 has transparent, bit 1 shell overflow).`. After `uint job = uint(lpc.job.w);` add:

```glsl
	// Shell mode (lpc.params.w == 1, spec §5): only quads whose SOLID side is transparent,
	// counted in slot 2 and written to the shell streams this mode's uniform set binds. A job
	// whose reduce raised no transparent bit has none, so every thread leaves at once.
	bool shell = lpc.params.w == 1;
	if (shell && (counts.v[job * 4u + 3u] & 1u) == 0u) return;
```

Replace the allocation lines:

```glsl
		uint t = atomicAdd(counts.v[job * 2u + 0u], 1u);
		if (t >= uint(lpc.params.y)) { atomicOr(counts.v[job * 2u + 1u], 1u); return; }
```

with:

```glsl
		ivec3 ms = sa ? L : (L + e);
		uint solid_material = imageLoad(material, ms).r;
		if (shell && !mat_transparent(solid_material)) continue;
		uint t = atomicAdd(counts.v[job * 4u + (shell ? 2u : 0u)], 1u);
		if (t >= uint(lpc.params.y)) {
			if (shell) atomicOr(counts.v[job * 4u + 3u], 2u);
			else atomicOr(counts.v[job * 4u + 1u], 1u);
			return;
		}
```

and further down replace:

```glsl
		ivec3 ms = sa ? L : (L + e);
		bits_set(w, 78, 16, imageLoad(material, ms).r);
```

with:

```glsl
		bits_set(w, 78, 16, solid_material);
```

- [ ] **Step 4: LodBuildPass**

`extension/src/render/lod_build_pass.h`:
- Update the class comment to: `// ... Per chunk: field, tent reduce, opaque lattice, then cell fractions + packed quads twice -- the shell (original lattice, transparent quads only) and the terrain mesh (opaque lattice).`
- Change `void push(int64_t list, const LodBuildJob &job, int job_index);` to `void push(int64_t list, const LodBuildJob &job, int job_index, int mode = 0);`.
- Add `void record_opaque(int64_t list, const LodBuildJob &job, int job_index);`.
- Change `record_frac` / `record_quads` to take the set and mode: `void record_frac(int64_t list, const LodBuildJob &job, int job_index, RID set);` and `void record_quads(int64_t list, const LodBuildJob &job, int job_index, RID set, int mode);`.
- New members:

```cpp
	RID opq_sdf_;       // R8_UNORM 3D, 34^3: the opaque lattice (spec §5)
	RID shell_quads_;   // 3 uint per quad, max_jobs * kLodMaxQuadsPerChunk
	RID shell_normals_; // 2 uint per quad, aligned with shell_quads_
	gpu::Program opaque_program_;
	RID opaque_set_, frac_shell_set_, quads_shell_set_;
```

- Update the counts comment: `RID counts_; // 4 uint per job: quad count, overflow, shell count, flags`.

`extension/src/render/lod_build_pass.cpp`:
- In `initialize`, add `make_3d(&opq_sdf_, RenderingDevice::DATA_FORMAT_R8_UNORM, ve::kLodChunkLattice);`. Allocate `shell_quads_` / `shell_normals_` exactly as `quads_` / `normals_` are (same sizes). Change `counts_bytes` to `static_cast<int64_t>(cfg_.max_jobs) * 16`. Add all three RIDs to the validity check.
- `reduce_set_` gains `gpu::storage(4, counts_)`.
- After the reduce program block, compile and bind the opaque program:

```cpp
	opaque_program_ = gpu::compile_compute(rd, group_, "LodBuildPass", "lod_opaque.comp.glsl");
	if (!opaque_program_.valid()) {
		teardown();
		return false;
	}
	opaque_set_ = gpu::uniform_set(rd, group_, opaque_program_.shader, 0,
			{gpu::image(0, lat_sdf_), gpu::image(1, lat_mat_), gpu::image(2, opq_sdf_)});
	if (!opaque_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: opaque uniform set creation failed");
		teardown();
		return false;
	}
```

- `frac_set_` now binds the **opaque** lattice, and a second set binds the original lattice for the shell:

```cpp
	frac_set_ = gpu::uniform_set(rd, group_, frac_program_.shader, 0,
			{gpu::image(0, opq_sdf_), gpu::storage(1, frac_)});
	frac_shell_set_ = gpu::uniform_set(rd, group_, frac_program_.shader, 0,
			{gpu::image(0, lat_sdf_), gpu::storage(1, frac_)});
	if (!frac_set_.is_valid() || !frac_shell_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: frac uniform set creation failed");
		teardown();
		return false;
	}
```

- `quads_set_` binds `opq_sdf_` at 0, and a shell set binds the original lattice and the shell streams:

```cpp
	quads_set_ = gpu::uniform_set(rd, group_, quads_program_.shader, 0, {
			gpu::image(0, opq_sdf_), gpu::image(1, lat_mat_), gpu::storage(2, frac_),
			gpu::storage(3, quads_), gpu::storage(4, counts_), gpu::storage(5, normals_)});
	quads_shell_set_ = gpu::uniform_set(rd, group_, quads_program_.shader, 0, {
			gpu::image(0, lat_sdf_), gpu::image(1, lat_mat_), gpu::storage(2, frac_),
			gpu::storage(3, shell_quads_), gpu::storage(4, counts_), gpu::storage(5, shell_normals_)});
	if (!quads_set_.is_valid() || !quads_shell_set_.is_valid()) {
		UtilityFunctions::printerr("LodBuildPass: quads uniform set creation failed");
		teardown();
		return false;
	}
```

- `teardown` resets `opaque_program_`, `opaque_set_`, `frac_shell_set_`, `quads_shell_set_`, `opq_sdf_`, `shell_quads_` and `shell_normals_`.
- `reset_counts` writes `cfg_.max_jobs * 16` zero bytes.
- In `push`, set `params[3]` to `mode`: `{sanitized_op_count(job), ve::kLodMaxQuadsPerChunk, job.level, mode}`.
- New and changed recorders:

```cpp
void LodBuildPass::record_opaque(int64_t list, const LodBuildJob &job, int job_index) {
	rd_->compute_list_bind_compute_pipeline(list, opaque_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, opaque_set_, 0);
	push(list, job, job_index);
	const int g = groups(ve::kLodChunkLattice);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_frac(int64_t list, const LodBuildJob &job, int job_index, RID set) {
	rd_->compute_list_bind_compute_pipeline(list, frac_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, set, 0);
	push(list, job, job_index);
	const int g = groups(ve::kLodChunkMeshCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

void LodBuildPass::record_quads(int64_t list, const LodBuildJob &job, int job_index, RID set,
		int mode) {
	rd_->compute_list_bind_compute_pipeline(list, quads_program_.pipeline);
	rd_->compute_list_bind_uniform_set(list, set, 0);
	push(list, job, job_index, mode);
	const int g = groups(ve::kLodChunkCells);
	rd_->compute_list_dispatch(list, g, g, g);
}

// Spec §5. The opaque lattice ALWAYS runs: it is the identity on a chunk with no transparent
// label, so the terrain mesh below is the one this chunk always had. The shell pair runs
// first, on the original lattice, and its quads pass leaves at once when the reduce raised
// no transparent bit. frac_ holds one slice, so the two pairs run strictly in turn.
void LodBuildPass::record_job(int64_t list, const LodBuildJob &job, int job_index) {
	record_field(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_reduce(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_opaque(list, job, job_index);
	rd_->compute_list_add_barrier(list);
	record_frac(list, job, job_index, frac_shell_set_);
	rd_->compute_list_add_barrier(list);
	record_quads(list, job, job_index, quads_shell_set_, 1);
	rd_->compute_list_add_barrier(list);
	record_frac(list, job, job_index, frac_set_);
	rd_->compute_list_add_barrier(list);
	record_quads(list, job, job_index, quads_set_, 0);
	rd_->compute_list_add_barrier(list);
}
```

- `read_job`: read 16 count bytes, then the shell, appended after the skirts:

```cpp
	const PackedByteArray cb = rd_->buffer_get_data(counts_,
			static_cast<uint32_t>(job_index) * 16, 16);
	if (cb.size() < 16) {
		out->failed = true;
		return;
	}
	const uint32_t *c = reinterpret_cast<const uint32_t *>(cb.ptr());
	const int qcount = std::min<int>(static_cast<int>(c[0]), ve::kLodMaxQuadsPerChunk);
	const int scount = std::min<int>(static_cast<int>(c[2]), ve::kLodMaxQuadsPerChunk);
	out->overflow = c[1] != 0u || (c[3] & 2u) != 0u;
```

Keep the existing opaque readback unchanged. Replace the final `ve::lod_append_skirts(&out->quads, &out->normals);` with:

```cpp
	ve::lod_append_skirts(&out->quads, &out->normals);
	if (scount > 0) {
		const uint32_t qbytes = static_cast<uint32_t>(scount) * 12;
		const uint32_t nbytes = static_cast<uint32_t>(scount) * sizeof(ve::LodQuadNormals);
		const PackedByteArray sq = rd_->buffer_get_data(shell_quads_,
				static_cast<uint32_t>(job_index) * ve::kLodMaxQuadsPerChunk * 12, qbytes);
		const PackedByteArray sn = rd_->buffer_get_data(shell_normals_,
				static_cast<uint32_t>(job_index) * ve::kLodMaxQuadsPerChunk * sizeof(ve::LodQuadNormals),
				nbytes);
		if (sq.size() < qbytes || sn.size() < nbytes) {
			out->failed = true;
			out->quads.clear();
			out->normals.clear();
			return;
		}
		std::vector<ve::LodQuad> shell(static_cast<size_t>(scount));
		std::vector<ve::LodQuadNormals> shell_normals(static_cast<size_t>(scount));
		std::memcpy(shell.data(), sq.ptr(), qbytes);
		std::memcpy(shell_normals.data(), sn.ptr(), nbytes);
		// Shell after skirts: skirts are boundary ribbons of the TERRAIN mesh, and a shell
		// quad must never become a skirt's parent.
		if (ve::lod_append_shell(&out->quads, &out->normals, shell, shell_normals))
			out->overflow = true;
	}
```

The shell quads are written by the kernel in `lod_quads.comp.glsl` at `(job * params.y + t) * 3`, so their per-job offset matches the opaque stream's.

- [ ] **Step 5: The diff hook compares both meshes**

In `extension/src/debug/hooks_lod.cpp`, replace:

```cpp
	ve::LodContourResult ref;
	ve::lod_contour(reduced_sdf.data(), reduced_mat.data(), &ref);
	ve::lod_append_skirts(&ref.quads, &ref.normals);
```

with:

```cpp
	// Spec §5: the GPU contours the OPAQUE lattice for terrain and the original lattice for
	// the shell, appending the shell after the skirts. The reference does exactly that.
	std::vector<uint8_t> opaque_sdf(kReducedCount);
	ve::lod_opaque_lattice(reduced_sdf.data(), reduced_mat.data(), cell, opaque_sdf.data());
	ve::LodContourResult ref;
	ve::lod_contour(opaque_sdf.data(), reduced_mat.data(), &ref);
	ve::lod_append_skirts(&ref.quads, &ref.normals);
	ve::LodContourResult shell;
	if (ve::lod_has_transparent(reduced_sdf.data(), reduced_mat.data()))
		ve::lod_contour(reduced_sdf.data(), reduced_mat.data(), &shell, true);
	ve::lod_append_shell(&ref.quads, &ref.normals, shell.quads, shell.normals);
	int shell_quads = 0;
	for (const ve::LodQuad &q : result.quads)
		if (ve::lod_quads_have_transparent(&q, 1)) shell_quads++;
	d["shell_quads"] = shell_quads;
```

Add `#include "lod/lod_reduce.h"` if it is not already included.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_lod_mesh_diff.gd`
Expected: every case PASS, including the two new ones.

- [ ] **Step 7: Pin that plain chunks did not move**

Run: `./gdunit_tests.sh -a res://tests/test_lod_raster_golden.gd -a res://tests/test_frame_shipped_golden.gd -a res://tests/test_lod_cull_golden.gd`
Expected: PASS, identical to the baseline. Any golden drift here means the opaque lattice is not the identity. Stop and fix it; do not re-record.

- [ ] **Step 8: Commit** (include the Godot-generated `shaders/lod_opaque.comp.glsl.import`)

```bash
git add shaders/lod_opaque.comp.glsl shaders/lod_opaque.comp.glsl.import shaders/lod_reduce.comp.glsl \
	shaders/lod_quads.comp.glsl extension/src/render/lod_build_pass.h \
	extension/src/render/lod_build_pass.cpp extension/src/debug/hooks_lod.cpp tests/test_lod_mesh_diff.gd
git commit -m "feat: LoD build contours an opaque terrain mesh and a transparent shell"
```

---

### Task 5: Near field, the walker

**Files:**
- Create: `shaders/transparent_walk.glslh`
- Modify: `shaders/sun_march.glslh`, `shaders/raymarch.comp.glsl`
- Modify: `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh` (regenerated)
- Modify: `extension/src/render/raymarch_pass.h`, `extension/src/render/raymarch_pass.cpp`
- Modify: `extension/src/render/frame.cpp` (hand the settings to the pass)
- Modify: `extension/src/debug/hooks.cpp` (`debug_render_frame` readouts)
- Test: `tests/test_transparency.gd` (create)

**Interfaces:**
- Consumes: `ve::TransparencySettings` (Task 2), GLSL `mat_transparent`/`mat_transmit` (Task 1).
- Produces (GLSL): `TransparentWalk walk_transparent(vec3 ro, vec3 rd, float t_begin, float step_min, float step_per_m, float min_transmit, int max_steps, inout int steps_left)`, `uint world_material(vec3)`, `bool world_opaque(vec3)`, `vec3 opaque_boundary_normal(vec3 p, vec3 rd)`, and the constants `WALK_AIR`, `WALK_OPAQUE`, `WALK_ABSORBED`.
- Produces (C++): `ve::TransparencyBlock { float params[4]; }` (x min step, y min transmit, z max steps, w enabled) with macro `TRANSPARENCY_BLOCK_FIELDS`. `RaymarchPass::set_transparency(const ve::TransparencySettings&)`, `front_texture()`, `trans_texture()`, `target_size()`.
- Produces (targets, march resolution):
  - `front` (RGBA32F): xy octahedral normal, z front distance along the ray, w front material (0 = none).
  - `trans` (RGBA16F): rgb transmittance, a front sun visibility.
- Produces (hook): `debug_render_frame` gains `center_lit` (Color), `center_front` (Color), `center_trans` (Color), `center_sun` (float), `center_material` (int).

- [ ] **Step 1: The readouts first (so the tests can see the marcher)**

In `extension/src/render/raymarch_pass.h`, add the accessor `Vector2i target_size() const { return Vector2i(width_, height_); }`. In `extension/src/debug/hooks.cpp`, at the end of `debug_render_frame`, before `return d;`:

```cpp
	// Centre-pixel readouts for tests/test_transparency.gd: what the marcher decided (march
	// resolution) and what the frame finally showed (full resolution). Plain reads of the
	// shipping targets -- nothing is re-rendered.
	{
		const int64_t c = static_cast<int64_t>(h / 2) * w + w / 2;
		d["center_lit"] = Color(half_to_float(v[c * 4]), half_to_float(v[c * 4 + 1]),
				half_to_float(v[c * 4 + 2]));
		RaymarchPass *rmp = world_->context().render->passes().raymarch;
		const Vector2i ms = rmp ? rmp->target_size() : Vector2i();
		if (ms.x > 0 && ms.y > 0) {
			const int64_t mc = static_cast<int64_t>(ms.y / 2) * ms.x + ms.x / 2;
			const PackedByteArray fr = device->texture_get_data(rmp->front_texture(), 0);
			if (fr.size() >= (mc + 1) * 16) {
				const float *f = reinterpret_cast<const float *>(fr.ptr()) + mc * 4;
				d["center_front"] = Color(f[0], f[1], f[2], f[3]);
			}
			const PackedByteArray tr = device->texture_get_data(rmp->trans_texture(), 0);
			if (tr.size() >= (mc + 1) * 8) {
				const uint16_t *t = reinterpret_cast<const uint16_t *>(tr.ptr()) + mc * 4;
				d["center_trans"] = Color(half_to_float(t[0]), half_to_float(t[1]),
						half_to_float(t[2]), half_to_float(t[3]));
			}
			const PackedByteArray al = device->texture_get_data(rmp->albedo_texture(), 0);
			if (al.size() >= (mc + 1) * 4) d["center_sun"] = al[mc * 4 + 3] / 255.0;
			const PackedByteArray sf = device->texture_get_data(rmp->surface_texture(), 0);
			if (sf.size() >= (mc + 1) * 8)
				d["center_material"] = static_cast<int>(half_to_float(
						reinterpret_cast<const uint16_t *>(sf.ptr())[mc * 4 + 2]) + 0.5f);
		}
	}
```

Add `#include "render/raymarch_pass.h"` if it is missing.

- [ ] **Step 2: Write the failing tests**

Create `tests/test_transparency.gd`:

```gdscript
extends GdUnitTestSuite

# Transparent materials, end to end (docs/superpowers/specs/2026-09-29-transparent-materials-
# design.md §9). Every frame is debug_render_frame -- the shipping VoxelFrame on a local
# device -- so what is asserted is what ships, never a hook's re-implementation of it.
#
# (20, 60, 30) is the open, sunlit meadow (memory: grass-hook-spot-sees-cave-grass); the camera
# sits above it looking almost straight down, so the centre pixel is lit ground.

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

func make_world(cam := CAM) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
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

# Where the centre pixel's ray first meets the world.
func centre_hit(w: VoxelWorld, cam := CAM, fwd := FWD) -> Vector3:
	var hit: Dictionary = w.raycast(cam, fwd.normalized(), 400.0)
	assert_bool(hit["hit"]).override_failure_message("the centre ray sees nothing").is_true()
	return hit["pos"]

func frame(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d: Dictionary = w.hooks().debug_render_frame(cam, fwd.normalized(), W, H)
	assert_bool(d["ok"]).override_failure_message("frame aborted: %s" % d).is_true()
	return d

func finite(c: Color) -> bool:
	return is_finite(c.r) and is_finite(c.g) and is_finite(c.b)

# --- the marcher (spec §4) --------------------------------------------------------------

func test_the_marcher_sees_through_ice_to_the_ground() -> void:
	var w := make_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.0, ice)
	settle(w)
	var d := frame(w)
	var front: Color = d["center_front"]
	var t: Color = d["center_trans"]
	assert_int(int(front.a + 0.5)).override_failure_message(
		"no transparent front at the centre: %s" % d).is_equal(ice)
	assert_int(int(d["center_material"])).override_failure_message(
		"the G-buffer holds the ice, not what is behind it").is_not_equal(ice)
	assert_float(t.r).is_greater(0.0)
	assert_float(t.r).is_less(1.0)
	# Blue survives ice better than red: the tint is the table's, not a grey fade.
	assert_float(t.b).is_greater(t.r)

func test_thicker_ice_lets_less_through() -> void:
	var thin_w := make_world()
	thin_w.hooks().debug_apply_sphere_add(centre_hit(thin_w), 0.6, material_id(thin_w, "ice"))
	settle(thin_w)
	var thin: Color = frame(thin_w)["center_trans"]
	var thick_w := make_world()
	thick_w.hooks().debug_apply_sphere_add(centre_hit(thick_w), 1.6, material_id(thick_w, "ice"))
	settle(thick_w)
	var thick: Color = frame(thick_w)["center_trans"]
	assert_float(thick.r).override_failure_message(
		"thin %s vs thick %s" % [thin, thick]).is_less(thin.r)
	assert_float(thick.r).is_greater(0.0)

func test_the_ground_under_ice_is_lit_through_it() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.0, material_id(w, "ice"))
	settle(w)
	# The sun visibility the G-buffer carries is the SURFACE BEHIND's: its sun ray crosses
	# the ice, and an opaque march would return 0 here.
	assert_float(float(frame(w)["center_sun"])).is_greater(0.3)

# ice_crack keeps at most 70% per metre, so a 0.5 cutoff fires after ~2 m -- well inside a
# 1.5 m ball's ~2.3 m path and well before the 48-step cap (48 x 5 cm = 2.4 m) could.
func test_past_the_cutoff_the_ice_is_opaque_and_the_gbuffer_keeps_it() -> void:
	var w := make_world()
	var ice := material_id(w, "ice_crack")
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.5, ice)
	settle(w)
	w.set_transparency_value("min_transmit", 0.5)
	var d := frame(w)
	var t: Color = d["center_trans"]
	assert_float(t.r + t.g + t.b).is_equal(0.0)
	assert_int(int(d["center_material"])).is_equal(ice)

func test_turning_transparency_off_hits_ice_as_opaque() -> void:
	var w := make_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.0, ice)
	settle(w)
	w.set_transparency_value("enabled", 0.0)
	var d := frame(w)
	assert_float(float((d["center_front"] as Color).a)).is_equal(0.0)
	assert_int(int(d["center_material"])).is_equal(ice)

# --- review focus ---------------------------------------------------------------------

func test_a_camera_inside_ice_still_sees_through_it() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(CAM, 2.0, material_id(w, "ice"))
	settle(w)
	var d := frame(w)
	assert_bool(finite(d["center_lit"])).is_true()
	assert_float((d["center_trans"] as Color).r).is_greater(0.0)

func test_extreme_settings_stay_finite() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.5, material_id(w, "ice"))
	settle(w)
	w.set_transparency_value("max_steps", 1.0)
	w.set_transparency_value("min_transmit", 0.0)
	var capped := frame(w)
	var t: Color = capped["center_trans"]
	assert_float(t.r + t.g + t.b).override_failure_message(
		"one step cannot cross 3 m of ice; the cap must absorb").is_equal(0.0)
	w.set_transparency_value("max_steps", 48.0)
	w.set_transparency_value("min_step_m", 1.0)
	var coarse := frame(w)
	assert_bool(finite(coarse["center_lit"])).is_true()
	assert_bool(finite(coarse["center_trans"])).is_true()

func test_sky_behind_a_floating_ice_ball_shows_through() -> void:
	var w := make_world()
	# A ball in the air straight ahead of a level camera, with nothing behind it but sky.
	var fwd := Vector3(1.0, 0.25, 0.0)
	var ball := CAM + fwd.normalized() * 8.0
	w.hooks().debug_apply_sphere_add(ball, 1.5, material_id(w, "ice"))
	settle(w)
	var d := frame(w, CAM, fwd)
	assert_int(int((d["center_front"] as Color).a + 0.5)).is_equal(material_id(w, "ice"))
	assert_int(int(d["center_material"])).override_failure_message(
		"behind the ball should be sky (material 0)").is_equal(0)
	assert_float((d["center_trans"] as Color).r).is_greater(0.0)

func test_painted_ice_shows_the_ground_below_the_lens() -> void:
	var w := make_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_paint(centre_hit(w), 1.0, ice)
	settle(w)
	var d := frame(w)
	assert_int(int((d["center_front"] as Color).a + 0.5)).is_equal(ice)
	assert_int(int(d["center_material"])).is_not_equal(ice)
	assert_float((d["center_trans"] as Color).r).is_greater(0.0)
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: build fails on `RaymarchPass::front_texture` (not yet declared). That is the red.

- [ ] **Step 4: The block**

In `extension/src/gpu_layout/blocks.h`, after `EditsBlock`:

```cpp
struct TransparencyBlock {
	float params[4];  // x = min step (m), y = min transmit, z = max steps, w = 1 when enabled
};
```

and its table and registry row:

```cpp
inline constexpr Field kTransparencyBlockFields[] = {
	VE_LAYOUT_FIELD(TransparencyBlock, params, Vec4, 0),
};
```

```cpp
	VE_LAYOUT_BLOCK(TransparencyBlock, "TRANSPARENCY_BLOCK_FIELDS", kTransparencyBlockFields),
```

Regenerate: `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests; scons -Q test`
Expected: the second full run is `Status: SUCCESS!`.

- [ ] **Step 5: The walk**

Create `shaders/transparent_walk.glslh`:

```glsl
// The walk through a transparent medium
// (docs/superpowers/specs/2026-09-29-transparent-materials-design.md §4). Transparency is a
// material LABEL on the one union SDF, so inside ice the field is negative all the way
// through: there is no surface between the ice and whatever is behind it, only a change of
// label. This file steps the labels. Shared by the primary ray (raymarch.comp.glsl) and the
// sun march (sun_march.glslh), so a pixel and its shadow read the same medium.
//
// The includer must include brick_atlas.glslh (and so declare everything it needs) first.
//
// NOTE: never put a literal include directive inside a comment in this file -- the loader
// matches include tokens anywhere in a line and would self-include.

const int WALK_AIR = 0;      // left the medium into air at `t`: march on from there
const int WALK_OPAQUE = 1;   // reached an opaque solid at `t`; `mat` is its label
const int WALK_ABSORBED = 2; // T fell under the cutoff, or the step cap ran out: T = 0

struct TransparentWalk {
	vec3 T;
	float t;
	int end;
	uint mat;
};

// The label of the voxel holding p; air where no brick is resident.
uint world_material(vec3 p) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = slot_at(brick);
	if (slot < 0) return 0u;
	return material_at(p, brick, slot);
}

// Solid and not transparent: the one test the walk stops on.
bool world_opaque(vec3 p) {
	return world_sdf(p) <= 0.0 && !mat_transparent(world_material(p));
}

// The normal of the label boundary at p. The SDF has no surface there, so this is the
// gradient of the OPAQUE OCCUPANCY -- 1 for an opaque solid voxel, 0 otherwise --
// trilinearly interpolated over the eight voxel centres around p, negated to point out of
// the opaque solid. A flat gradient (p deep inside one label) falls back to facing the ray.
vec3 opaque_boundary_normal(vec3 p, vec3 rd) {
	vec3 g = p / VOXEL_SIZE - 0.5;
	vec3 base = floor(g);
	vec3 f = g - base;
	float o[8];
	for (int k = 0; k < 8; k++) {
		vec3 c = (base + vec3(k & 1, (k >> 1) & 1, (k >> 2) & 1) + 0.5) * VOXEL_SIZE;
		o[k] = world_opaque(c) ? 1.0 : 0.0;
	}
	float gx = mix(mix(o[1] - o[0], o[3] - o[2], f.y), mix(o[5] - o[4], o[7] - o[6], f.y), f.z);
	float gy = mix(mix(o[2] - o[0], o[3] - o[1], f.x), mix(o[6] - o[4], o[7] - o[5], f.x), f.z);
	float gz = mix(mix(o[4] - o[0], o[5] - o[1], f.x), mix(o[6] - o[2], o[7] - o[3], f.x), f.y);
	vec3 grad = vec3(gx, gy, gz);
	float len = length(grad);
	return len > 1e-6 ? -grad / len : -rd;
}

// Walks from t_begin (already inside the medium) along rd. Each step is
// max(step_min, step_per_m * t) -- 5 cm near the camera, the pixel footprint further out --
// and multiplies T by the label's per-metre transmittance to the step's length. The walk
// ends absorbed once the brightest channel of T drops under min_transmit, or after
// max_steps. Every step spends one of the caller's shared steps_left.
TransparentWalk walk_transparent(vec3 ro, vec3 rd, float t_begin, float step_min,
		float step_per_m, float min_transmit, int max_steps, inout int steps_left) {
	TransparentWalk w;
	w.T = vec3(1.0);
	w.t = t_begin;
	w.end = WALK_ABSORBED;
	w.mat = 0u;
	float t = t_begin;
	for (int i = 0; i < max_steps && steps_left > 0; i++) {
		steps_left--;
		float step = max(step_min, step_per_m * t);
		vec3 p = ro + rd * t;
		if (world_sdf(p) > 0.0) {
			w.end = WALK_AIR;
			w.t = t;
			return w;
		}
		uint m = world_material(p);
		if (!mat_transparent(m)) {
			// Bisect the label boundary inside the last step: the walk arrived by a whole
			// step, and without this the seam would stair-step by one step length.
			float lo = max(t - step, t_begin), hi = t;
			for (int k = 0; k < 4; k++) {
				float mid = 0.5 * (lo + hi);
				if (world_opaque(ro + rd * mid)) hi = mid;
				else lo = mid;
			}
			w.end = WALK_OPAQUE;
			w.t = hi;
			w.mat = m;
			return w;
		}
		w.T *= pow(mat_transmit(m), vec3(step));
		if (max(w.T.r, max(w.T.g, w.T.b)) < min_transmit) break;
		t += step;
	}
	w.T = vec3(0.0);
	w.t = t;
	return w;
}
```

- [ ] **Step 6: The sun march walks too**

In `shaders/sun_march.glslh`, add the include and knobs after the header comment (before `const float RAY_SHADOW_DIST`):

```glsl
#include "transparent_walk.glslh"

// Transparent solids (spec §4): the sun walks through them with the primary ray's walker and
// multiplies what they let through, so ground under ice is lit through a tint, not black. The
// scalar sun term takes the brightest channel. An includer that must treat them as opaque
// (the raymarcher with transparency off) defines SUN_WALK_ENABLED to a false expression
// before including this file; grass and leaves always walk.
#ifndef SUN_WALK_ENABLED
#define SUN_WALK_ENABLED true
#endif
const float SUN_WALK_STEP = 0.1;
const float SUN_WALK_MIN_TRANSMIT = 0.01;
const int SUN_WALK_STEPS = 48;
// Within this of a surface the penumbra term is dominated by that surface. When the surface
// turns out to be transparent, the dip it caused is undone, and the ray leaving the medium
// does not count its own exit face as an occluder.
const float SUN_WALK_CLEAR = 0.1;
```

Replace `terrain_sun_visibility` with:

```glsl
float terrain_sun_visibility(vec3 ro, float max_shadow_dist) {
	float res = 1.0;
	float res_clear = 1.0; // res as it stood before the ray closed on the current surface
	float through = 1.0;   // what transparent media along the ray let through
	bool leaving = false;  // just walked out of a medium: its exit face is not an occluder
	float t = 0.05;
	for (int i = 0; i < RAY_SHADOW_STEPS; i++) {
		if (t > max_shadow_dist) break;
		vec3 q = ro + sun_light.dir.xyz * t;
		ivec3 brick = ivec3(floor(q / BRICK_SIZE));
		int shadow_region = region_slot_of(brick);
		if (shadow_region < 0) return 1.0;
		// (keep the existing comment block about zero components and empty regions)
		if (region_slot_counts.n[shadow_region] == 0) {
			vec3 rlo = floor(q / REGION_SIZE) * REGION_SIZE;
			vec3 rhi = rlo + vec3(REGION_SIZE);
			vec3 far = mix(rlo, rhi, step(0.0, sun_light.dir.xyz));
			vec3 tf = (far - q) / sun_light.dir.xyz;
			float skip = min(tf.x, min(tf.y, tf.z));
			t += max(skip, 0.01) + 0.001;
			continue;
		}
		float d = world_sdf(q);
		if (d < 0.004) {
			// Half a voxel along the ray is inside whatever was hit.
			vec3 inside = q + sun_light.dir.xyz * (VOXEL_SIZE * 0.5);
			if (!(SUN_WALK_ENABLED) || !mat_transparent(world_material(inside))) return 0.0;
			int walk_budget = SUN_WALK_STEPS;
			TransparentWalk w = walk_transparent(ro, sun_light.dir.xyz, t + VOXEL_SIZE * 0.5,
					SUN_WALK_STEP, 0.0, SUN_WALK_MIN_TRANSMIT, SUN_WALK_STEPS, walk_budget);
			if (w.end != WALK_AIR) return 0.0;
			through *= max(w.T.r, max(w.T.g, w.T.b));
			res = res_clear;
			leaving = true;
			t = w.t + 0.02;
			continue;
		}
		if (!leaving && t <= RAY_SHADOW_PENUMBRA_DIST) res = min(res, RAY_SHADOW_K * d / t);
		if (d >= SUN_WALK_CLEAR) {
			res_clear = res;
			leaving = false;
		}
		t += clamp(d, 0.02, 1.0);
	}
	return clamp(res, 0.0, 1.0) * through;
}
```

Keep the existing comment above the zero-region skip verbatim. With no transparent material on the ray, `leaving` stays false, `through` stays 1 and every returned value is the one the old function returned.

- [ ] **Step 7: The marcher**

In `shaders/raymarch.comp.glsl`:

(a) After the `out_surface` binding, document and declare the new targets:

```glsl
// Transparency (spec §4). The G-buffer targets above describe what is BEHIND a transparent
// front; the front itself goes here, for transparency_composite.comp.glsl.
//   out_front  xy = front oct normal, z = front distance along the ray, w = front material
//              (0 = no transparent front at this pixel)
//   out_trans  rgb = transmittance through the medium, a = the front's own sun visibility
layout(set = 0, binding = 31, rgba32f) writeonly uniform image2D out_front;
layout(set = 0, binding = 32, rgba16f) writeonly uniform image2D out_trans;
layout(set = 0, binding = 33) uniform Transparency { TRANSPARENCY_BLOCK_FIELDS } tr;
```

(b) Immediately before `#include "sun_march.glslh"`:

```glsl
#define SUN_WALK_ENABLED (tr.params.w > 0.5)
```

(c) After the constant block with `GLOSSY_SDF_*`, add `const int TRANSPARENT_SEGMENTS = 4; // media one ray may cross before it counts as absorbed`.

(d) In `main`, directly after `Hit best = march_terrain(ro, rd, max_dist, primary_steps);` (before the island mask), resolve the front:

```glsl
	// Transparency (spec §4). A transparent front is recorded for the composite, and the
	// G-buffer gets what is BEHIND it, so every pass downstream lights an ordinary surface.
	// Static terrain only: islands keep treating transparent materials as opaque (spec §1).
	vec4 front_out = vec4(0.0);
	vec3 trans = vec3(1.0);
	Hit front;
	front.hit = false;
	if (best.hit && tr.params.w > 0.5 && mat_transparent(best.mat)) {
		front = best;
		// A camera inside the medium hits it at t ~ 0, and the hit refinement can step a hair
		// behind the origin; clamp so the walk and the composite work from the camera.
		front.t = max(front.t, 0.0);
		front_out = vec4(oct_encode(front.n), front.t, float(front.mat));
		// World width of one marched pixel per metre of ray -- the glossy path's derivation.
		float step_per_m = 2.0 * max(pc.params.x / float(size.x), pc.params.y / float(size.y));
		Hit cur = front;
		bool absorbed = true;
		for (int seg = 0; seg < TRANSPARENT_SEGMENTS; seg++) {
			TransparentWalk w = walk_transparent(ro, rd, cur.t + VOXEL_SIZE * 0.5, tr.params.x,
					step_per_m, tr.params.y, int(tr.params.z), primary_steps);
			trans *= w.T;
			if (w.end == WALK_ABSORBED) break;
			if (w.end == WALK_OPAQUE) {
				cur.hit = true;
				cur.t = w.t;
				cur.p = ro + rd * w.t;
				cur.n = opaque_boundary_normal(cur.p, rd);
				cur.mat = w.mat;
				absorbed = false;
				break;
			}
			// Out into air: march on from just past the exit face.
			float t0 = w.t + VOXEL_SIZE;
			Hit next = march_terrain(ro + rd * t0, rd, max(max_dist - t0, 0.0), primary_steps);
			if (!next.hit) {
				cur.hit = false;
				cur.t = max_dist;
				absorbed = false;
				break;
			}
			next.t += t0;
			cur = next;
			if (!mat_transparent(cur.mat)) {
				absorbed = false;
				break;
			}
		}
		if (absorbed) {
			// Opaque from here on (the cutoff, the step cap or the segment cap): the G-buffer
			// keeps the front itself and the composite shows its body alone.
			trans = vec3(0.0);
			cur = front;
		}
		best = cur;
	}
```

(e) After the existing sun block inside `if (best.hit) { ... }`, compute the front's own sun, before the glossy block:

```glsl
	float front_sun = 1.0;
	if (front.hit && (flags & BEAUTY_RAY_SUN_SHADOW) != 0u) {
		vec3 fro = front.p + front.n * 0.06;
		front_sun = min(terrain_sun_visibility(fro, RAY_SHADOW_DIST),
				island_sun_visibility(fro, island_count, RAY_SHADOW_DIST));
	}
```

Place this after the `if (best.hit) { ... }` block closes, so that `flags` and `island_count` are in scope. Both are declared before it.

(f) In the probe early-return branch, before its `return;`, add:

```glsl
		imageStore(out_front, px, vec4(0.0));
		imageStore(out_trans, px, vec4(1.0));
```

(g) At the end of `main`, after the three existing stores:

```glsl
	imageStore(out_front, px, front_out);
	imageStore(out_trans, px, vec4(trans, front_sun));
```

- [ ] **Step 8: RaymarchPass**

`extension/src/render/raymarch_pass.h`: add `#include "transparency/transparency_settings.h"`, and the public API:

```cpp
	// Called every frame by VoxelFrame before render(); lands in the binding-33 UBO.
	void set_transparency(const ve::TransparencySettings &s);
	RID front_texture() const { return front_; }
	RID trans_texture() const { return trans_; }
```

Add the private members `RID front_, trans_, tr_ubo_;` and `ve::TransparencyBlock tr_block_{};`, plus `#include "gpu_layout/blocks.h"` if it is not reachable.

`extension/src/render/raymarch_pass.cpp`:
- In `initialize`, after `edits_ubo_`:

```cpp
	PackedByteArray tr_zero;
	tr_zero.resize(sizeof(ve::TransparencyBlock));
	tr_zero.fill(0);
	tr_ubo_ = group_.add(gpu::Kind::Buffer,
		rd->uniform_buffer_create(sizeof(ve::TransparencyBlock), tr_zero));
```

- `set_transparency`:

```cpp
void RaymarchPass::set_transparency(const ve::TransparencySettings &s) {
	tr_block_ = ve::TransparencyBlock{{s.min_step_m, s.min_transmit,
			static_cast<float>(s.max_steps), s.enabled ? 1.0f : 0.0f}};
}
```

- `rebuild_targets`: add `&front_, &trans_` to the free list and create them:

```cpp
	front_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, Vector2i(w, h), usage);
	trans_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, Vector2i(w, h), usage);
```

- `uniforms()`: append `gpu::image(31, front_), gpu::image(32, trans_), gpu::ubo(33, tr_ubo_),`.
- `render()`: in the pre-list block where `edits_ubo_` is updated, add `rd->buffer_update(tr_ubo_, 0, sizeof(tr_block_), gpu::push_bytes(tr_block_));`. Add `!front_.is_valid() || !trans_.is_valid() || !tr_ubo_.is_valid()` to the validity check.
- `teardown()`: reset `front_ = trans_ = tr_ubo_ = RID();`.

`extension/src/render/frame.cpp`, just before `if (!rmp->render(rd, *atlas, ...`:

```cpp
	rmp->set_transparency(render_.transparency_settings());
```

- [ ] **Step 9: Run the tests to verify they pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: every case PASS. Pixel colours are not asserted yet: the composite that paints the ice is Task 8. Until then ice is invisible in a frame, which is expected mid-branch.

- [ ] **Step 10: Pin that scenes without ice did not move**

Run: `./gdunit_tests.sh -a res://tests/test_frame_shipped_golden.gd -a res://tests/test_composite_golden.gd -a res://tests/test_deferred_golden.gd -a res://tests/test_grass_golden.gd -a res://tests/test_raymarch_gbuffer.gd -a res://tests/test_frame_contract.gd`
Expected: PASS, identical to the baseline. The sun march must return the old value bit for bit when no transparent label is on the ray. If a grass or frame golden drifts, the `leaving` / `res_clear` bookkeeping changed a no-ice path. Fix it; never re-record.

- [ ] **Step 11: Commit**

```bash
git add shaders/transparent_walk.glslh shaders/sun_march.glslh shaders/raymarch.comp.glsl \
	extension/src/gpu_layout/blocks.h shaders/generated/blocks.glslh \
	extension/src/render/raymarch_pass.h extension/src/render/raymarch_pass.cpp \
	extension/src/render/frame.cpp extension/src/debug/hooks.cpp tests/test_transparency.gd \
	tests/test_transparency.gd.uid
git commit -m "feat: near-field walk through transparent materials, with tinted sun"
```

---

### Task 6: Far field, dropping shell quads from the opaque draws and publishing the shell pages

**Files:**
- Modify: `shaders/lod.vert.glsl`, `shaders/lod_shadow.vert.glsl`
- Modify: `extension/src/render/lod_raster_pass.h`, `extension/src/render/lod_raster_pass.cpp`
- Modify: `extension/src/lod/lod_system.h`, `extension/src/lod/lod_system.cpp`
- Modify: `extension/src/render/frame.cpp` (set the skip flag)

**Interfaces:**
- Consumes: `ve::lod_quads_have_transparent` (Task 3).
- Produces: `LodRasterPass::set_skip_transparent(bool)`, and `LodSystem::transparent_draw_pages() const -> std::vector<ve::LodPageDraw>`. The latter is the current walk's drawable pages that hold at least one shell quad; it is refreshed by `prepare_raster` under the lod mutex. It uses `ve::LodPageDraw` (from `lod/lod_tree.h`, already included) so that `lod_system.h` stays free of render headers.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_lod_transparency.cpp`. This pins the page-level rule the LodSystem bookkeeping uses: a page holds a shell quad exactly when some quad in its range is transparent.

```cpp
TEST_CASE("page-sized ranges report shell quads only where they sit") {
	std::vector<uint8_t> sdf;
	std::vector<uint16_t> mat;
	sdf.assign(kCount, 0);
	mat.assign(kCount, 0);
	for (int z = 0; z < kN; z++)
		for (int y = 0; y < kN; y++)
			for (int x = 0; x < kN; x++) {
				const int i = ve::lod_lattice_index(x, y, z);
				sdf[i] = ve::lod_encode_sdf((float(y) - 16.5f) * 0.4f, 0.4f);
				if (y <= 16) mat[i] = y >= 12 ? ve::material_id("ice") : ve::material_id("rock");
			}
	std::vector<uint8_t> opaque(kCount);
	ve::lod_opaque_lattice(sdf.data(), mat.data(), 0.4f, opaque.data());
	ve::LodContourResult terrain, shell;
	ve::lod_contour(opaque.data(), mat.data(), &terrain);
	ve::lod_contour(sdf.data(), mat.data(), &shell, true);
	const int opaque_count = int(terrain.quads.size());
	ve::lod_append_shell(&terrain.quads, &terrain.normals, shell.quads, shell.normals);
	// 1024 terrain quads fill pages 0-1; the 1024 shell quads fill pages 2-3.
	CHECK_FALSE(ve::lod_quads_have_transparent(terrain.quads.data(), ve::kLodQuadsPerPage));
	CHECK_FALSE(ve::lod_quads_have_transparent(terrain.quads.data() + ve::kLodQuadsPerPage,
			opaque_count - ve::kLodQuadsPerPage));
	CHECK(ve::lod_quads_have_transparent(terrain.quads.data() + opaque_count, ve::kLodQuadsPerPage));
}
```

- [ ] **Step 2: Run the test**

Run: `cd extension && scons -Q test`
Expected: PASS. It pins Task 3's helper on the exact page layout the LodSystem relies on. It must stay green through this task, as the characterization of the rule below.

- [ ] **Step 3: The vertex shaders drop shell quads**

`shaders/lod.vert.glsl`, at the end of `main`, replace `gl_Position = pc.view_proj * vec4(v_wpos, 1.0);` with:

```glsl
	// Spec §5: with transparency on, a shell quad belongs to transparent.vert.glsl, not to the
	// terrain. Collapse it outside the clip volume so it rasterizes nothing. pc.fade.y is 1
	// exactly when transparency is enabled; off, the shell draws here as the opaque surface
	// the material used to be.
	if (pc.fade.y > 0.5 && mat_transparent(v_material)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	gl_Position = pc.view_proj * vec4(v_wpos, 1.0);
```

`shaders/lod_shadow.vert.glsl`, replace the ribbon collapse line with:

```glsl
	// Boundary ribbons are synthetic overlap geometry. They prevent camera coverage gaps but
	// must not become steep shadow casters; collapse each tagged quad to zero area here.
	// Transparent shells cast no sun-map shadow either (spec §1): the ground under far ice
	// stays lit, as the near field's walked sun ray keeps it.
	if (lod_bits_get(w, 94, 1) != 0u || mat_transparent(lod_bits_get(w, 78, 16))) p = c0.xyz;
```

In `extension/src/gpu_layout/blocks.h`, update the `LodRasterPush::fade` comment to `// x = fade end, y = 1 when transparency is on (shell quads skipped), zw unused`, then regenerate `blocks.glslh` (`cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests`). The macro text is unchanged; only the C++ comment moves.

- [ ] **Step 4: LodRasterPass carries the flag**

`extension/src/render/lod_raster_pass.h`: beside `set_cull_enabled`, add

```cpp
	// Spec §5: skip shell quads (transparent material) in the terrain draw; the transparent
	// raster draws them. VoxelFrame sets this from TransparencySettings::enabled every frame.
	void set_skip_transparent(bool skip) { skip_transparent_ = skip; }
```

and the member `bool skip_transparent_ = false;`. In `lod_raster_pass.cpp` `draw()`, after `push.fade[0] = fade_end;` add `push.fade[1] = skip_transparent_ ? 1.0f : 0.0f;`.

`extension/src/render/frame.cpp`, before the LoD block (`if (!in.debug.skip_far_field && lod_.pool() && lod_raster ...`):

```cpp
	const ve::TransparencySettings transparency = render_.transparency_settings();
	if (lod_raster) lod_raster->set_skip_transparent(transparency.enabled);
```

Task 5 already calls `rmp->set_transparency(render_.transparency_settings())`. Hoist the `transparency` local above the raymarch call and reuse it there.

- [ ] **Step 5: LodSystem remembers shell pages**

`extension/src/lod/lod_system.h`: add `#include <unordered_set>`. Beside `lod_page_quads_` add:

```cpp
	std::unordered_set<int> lod_transparent_pages_; // pages holding at least one shell quad
	std::vector<ve::LodPageDraw> transparent_draw_pages_; // this walk's, see below
```

Extend the lod-mutex comment at `lod_system.h:81` to name both. Add the public accessor:

```cpp
	// Spec §5: this walk's drawable pages that hold a transparent shell quad, for
	// TransparentRasterPass. Refreshed by prepare_raster().
	std::vector<ve::LodPageDraw> transparent_draw_pages() const;
```

`extension/src/lod/lod_system.cpp`:
- Everywhere a page leaves `lod_page_quads_` (lines ~196, 231, 254: `for (int p : ...) lod_page_quads_.erase(p);`), extend the loop body to also erase from the new set. For example:

```cpp
					for (int p : old_it->second) {
						lod_page_quads_.erase(p);
						lod_transparent_pages_.erase(p);
					}
```

- Where `lod_page_quads_.clear()` is called (lines ~405, 417), also call `lod_transparent_pages_.clear(); transparent_draw_pages_.clear();`.
- In the upload loop that records `lod_page_quads_[pages[i]] = count;`, add after it:

```cpp
				if (ve::lod_quads_have_transparent(r.quads.data() + first, count))
					lod_transparent_pages_.insert(pages[static_cast<size_t>(i)]);
```

- In `prepare_raster_locked()`, after `set_draw_pages(pages)`:

```cpp
	// ponytail: the shell list is the CPU walk's pages, not HiZ-culled; painted ice is rare.
	// Cull it too if a scene ever holds much of it.
	transparent_draw_pages_.clear();
	for (const ve::LodPageDraw &pd : page_draws)
		if (lod_transparent_pages_.count(pd.page)) transparent_draw_pages_.push_back(pd);
```

- The accessor:

```cpp
std::vector<ve::LodPageDraw> LodSystem::transparent_draw_pages() const {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	return transparent_draw_pages_;
}
```

`lod_mutex_` is already `mutable` (lod_system.h:156).

- [ ] **Step 6: Build and re-run the pins**

Run: `./build.sh && cd extension && scons -Q test && cd .. && ./gdunit_tests.sh -a res://tests/test_lod_raster_golden.gd -a res://tests/test_frame_shipped_golden.gd -a res://tests/test_lod_mesh_diff.gd`
Expected: all PASS. No scene in those suites holds ice, so the skip never fires.

- [ ] **Step 7: Commit**

```bash
git add shaders/lod.vert.glsl shaders/lod_shadow.vert.glsl extension/src/gpu_layout/blocks.h \
	shaders/generated/blocks.glslh extension/src/render/lod_raster_pass.h \
	extension/src/render/lod_raster_pass.cpp extension/src/lod/lod_system.h \
	extension/src/lod/lod_system.cpp extension/src/render/frame.cpp \
	extension/tests/test_lod_transparency.cpp
git commit -m "feat: far-field terrain and sun map drop shell quads; shell pages tracked"
```

---

### Task 7: Far field, the shell raster

**Files:**
- Create: `shaders/transparent.vert.glsl`, `shaders/transparent.frag.glsl`
- Create: `extension/src/render/transparent_raster_pass.h`, `extension/src/render/transparent_raster_pass.cpp`
- Modify: `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh`
- Modify: `extension/src/render/orchestrator.h`, `extension/src/render/orchestrator.cpp` (own the pass)
- Modify: `extension/src/render/frame.h`, `extension/src/render/frame.cpp` (stage and call)
- Modify: `extension/src/debug/hooks.cpp` (`transparent_pages`, `far_center_front`)
- Test: `tests/test_transparency.gd`

**Interfaces:**
- Consumes: `LodSystem::transparent_draw_pages()` (`std::vector<ve::LodPageDraw>`), `LodRasterPass::index_array()`, `LodRasterPass::front_face_clockwise()`, `LodPool` buffers, the `BeautyCamBlock` UBO (`CameraUbo::buffer()`), `GBuffer::depth()`.
- Produces: `TransparentRasterPass` with `initialize(RenderingDevice*)`, `teardown()`, `set_draw_pages(const std::vector<LodRasterPass::PageDraw>&)`, `draw_page_count() const`, `bool draw(RenderingDevice*, LodPool&, RID index_array, GBuffer&, RID beauty_cam_ubo, float fade_start, float fade_end, bool front_face_clockwise)`, `bool drew() const`, `RID front() const`, `RID trans() const`.
  - Layer layout: `front` RGBA32F (xy oct normal, z distance, w material). `trans` RGBA16F (rgb transmittance, a = −1, meaning "read the sun map").
  - Block `TransparentRasterPush { float fade[4]; }` (x fade start, y fade end), macro `TRANSPARENT_RASTER_PUSH_FIELDS`.
- Produces: `FrameStage::kStageTransparentRaster` ("transparent_raster").

- [ ] **Step 1: Write the failing test**

Append to `tests/test_transparency.gd`:

```gdscript
# --- the far field (spec §5) -------------------------------------------------------------

# Frames until the LoD has built and published the shell, or gives up. Builds are async on
# the mesh worker; each frame collects what finished.
func frame_until_shell(w: VoxelWorld, cam: Vector3, fwd: Vector3) -> Dictionary:
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		if int(d.get("transparent_pages", 0)) > 0:
			break
	return d

func far_setup(w: VoxelWorld) -> Array:
	# An ice ball on the ground ~200 m out: well past the near field's fade band.
	var down: Dictionary = w.raycast(Vector3(CAM.x + 200.0, 200.0, CAM.z), Vector3.DOWN, 300.0)
	assert_bool(down["hit"]).is_true()
	var target: Vector3 = down["pos"]
	var cam := Vector3(CAM.x, target.y + 60.0, CAM.z)
	var fwd := (target - cam).normalized()
	var seen: Dictionary = w.raycast(cam, fwd, 400.0)
	assert_bool(seen["hit"] and float(seen["distance"]) > 150.0).override_failure_message(
		"terrain hides the far ice from this camera; raise it").is_true()
	return [cam, fwd, target]

func test_far_ice_is_drawn_by_the_shell() -> void:
	var w := make_world()
	var s := far_setup(w)
	w.hooks().debug_apply_sphere_add(s[2], 10.0, material_id(w, "ice"))
	settle(w, s[0])
	var d := frame_until_shell(w, s[0], s[1])
	assert_int(int(d["transparent_pages"])).override_failure_message(
		"no shell pages were ever published: %s" % d).is_greater(0)
	var ok: PackedStringArray = d["stages_ok"]
	assert_bool(ok.has("transparent_raster")).is_true()
	assert_int(int((d["far_center_front"] as Color).a + 0.5)).is_equal(material_id(w, "ice"))

func test_removing_far_ice_drops_its_shell() -> void:
	var w := make_world()
	var s := far_setup(w)
	w.hooks().debug_apply_sphere_add(s[2], 10.0, material_id(w, "ice"))
	settle(w, s[0])
	assert_int(int(frame_until_shell(w, s[0], s[1])["transparent_pages"])).is_greater(0)
	w.hooks().debug_apply_sphere_subtract(s[2], 12.0)
	settle(w, s[0])
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(s[0])
		d = frame(w, s[0], s[1])
		if int(d["transparent_pages"]) == 0:
			break
	assert_int(int(d["transparent_pages"])).override_failure_message(
		"the shell outlived the ice: released pages were never forgotten").is_equal(0)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: the two new cases FAIL on the missing key `transparent_pages`.

- [ ] **Step 3: The block**

`extension/src/gpu_layout/blocks.h`:

```cpp
struct TransparentRasterPush {
	float fade[4];  // x = fade start, y = fade end (metres), zw unused
};
```

```cpp
inline constexpr Field kTransparentRasterPushFields[] = {
	VE_LAYOUT_FIELD(TransparentRasterPush, fade, Vec4, 0),
};
```

```cpp
	VE_LAYOUT_BLOCK(TransparentRasterPush, "TRANSPARENT_RASTER_PUSH_FIELDS", kTransparentRasterPushFields),
```

Regenerate with `cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests; scons -Q test`.

- [ ] **Step 4: The shaders**

Create `shaders/transparent.vert.glsl`:

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

// The far field's transparent shell (docs/superpowers/specs/2026-09-29-transparent-materials-
// design.md §5): the LoD arena pulled exactly as lod.vert.glsl pulls it. The page list holds
// only pages with at least one shell quad, but a page is shared with terrain quads, so every
// non-transparent quad collapses outside the clip volume and rasterizes nothing.
layout(set = 0, binding = 0, std430) readonly buffer Quads { uint v[]; } quads;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;
layout(set = 0, binding = 5, std430) readonly buffer Normals { uint v[]; } normals;

layout(push_constant, std430) uniform Push { TRANSPARENT_RASTER_PUSH_FIELDS } pc;

layout(location = 0) out vec3 v_wpos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out flat uint v_material;

void main() {
	uint vi = uint(gl_VertexIndex);
	uint quad = vi >> 2;
	uint corner = vi & 3u;
	uint page = quad >> uint(LOD_PAGE_SHIFT);
	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];
	uvec3 w = uvec3(quads.v[quad * 3u + 0u], quads.v[quad * 3u + 1u], quads.v[quad * 3u + 2u]);
	v_material = lod_bits_get(w, 78, 16);
	if (!mat_transparent(v_material)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	v_wpos = lod_corner_pos(w, int(corner), c0.xyz, c0.w);
	uint normal_pair = normals.v[quad * 2u + (corner >> 1u)];
	v_normal = oct_decode_snorm8((normal_pair >> ((corner & 1u) * 16u)) & 0xFFFFu);
	gl_Position = bcam.view_proj * vec4(v_wpos, 1.0);
}
```

Create `shaders/transparent.frag.glsl`:

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

// The far front layer; the same layout raymarch.comp.glsl writes for the near field.
layout(location = 0) out vec4 out_front; // xy oct normal, z distance, w material
layout(location = 1) out vec4 out_trans; // rgb transmittance, a = -1: read the sun map

layout(set = 0, binding = 6) uniform sampler2D gb_depth;
layout(push_constant, std430) uniform Push { TRANSPARENT_RASTER_PUSH_FIELDS } pc;

void main() {
	float d = distance(v_wpos, bcam.cam.xyz);
	float t_fade = clamp((d - pc.fade.x) / max(pc.fade.y - pc.fade.x, 1e-3), 0.0, 1.0);
	// The far field's half of the fade band, exactly as lod.frag.glsl keeps it; the composite
	// keeps the near front on the complement.
	if (bayer4(ivec2(gl_FragCoord.xy)) >= t_fade) discard;
	// Reverse-Z: larger is nearer. Anything the G-buffer holds in front of this fragment --
	// grass, leaves, terrain -- hides it.
	float scene = texelFetch(gb_depth, ivec2(gl_FragCoord.xy), 0).r;
	if (scene > gl_FragCoord.z) discard;
	// Thickness is the distance to the surface behind (spec §5, with its known ceiling: an
	// air gap behind the ice counts as ice). Sky behind is endless ice, so T goes to zero and
	// a far shell against the sky shows its body.
	float thickness = 1e4;
	if (scene > 0.0) {
		vec3 behind = beauty_world_from_depth(gl_FragCoord.xy * bcam.screen.zw, scene);
		thickness = max(distance(behind, bcam.cam.xyz) - d, 0.0);
	}
	out_front = vec4(oct_encode(normalize(v_normal)), d, float(v_material));
	out_trans = vec4(pow(mat_transmit(v_material), vec3(thickness)), -1.0);
}
```

- [ ] **Step 5: The pass**

Create `extension/src/render/transparent_raster_pass.h`:

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

// The far field's transparent shell (docs/superpowers/specs/2026-09-29-transparent-materials-
// design.md §5). Owns the full-resolution far front layer -- front (RGBA32F) and trans
// (RGBA16F), laid out as raymarch.comp.glsl's near targets -- plus its own depth, so the
// nearest shell fragment wins. Draws the LoD arena pages that hold shell quads with one
// indexed indirect draw of its own args; the G-buffer depth is only SAMPLED, never written.
class TransparentRasterPass {
public:
	~TransparentRasterPass();
	void initialize(RenderingDevice *rd);
	void teardown();

	void set_draw_pages(const std::vector<LodRasterPass::PageDraw> &pages) { pages_ = pages; }
	int draw_page_count() const { return static_cast<int>(pages_.size()); }

	// False only on failure. With no shell pages it draws nothing, returns true, and
	// drew() reports false -- the composite then never reads the layer.
	bool draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
			RID beauty_cam_ubo, float fade_start, float fade_end, bool front_face_clockwise);
	bool drew() const { return drew_; }
	RID front() const { return front_; }
	RID trans() const { return trans_; }

private:
	bool ensure_targets(RenderingDevice *rd, Vector2i size);
	bool ensure_args(RenderingDevice *rd, int pages);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_, pipeline_, sampler_;
	bool pipeline_clockwise_ = false;
	gpu::SetCache set_;
	gpu::FramebufferCache framebuffer_;
	RID front_, trans_, depth_, args_;
	Vector2i size_{0, 0};
	int args_capacity_ = 0;
	std::vector<LodRasterPass::PageDraw> pages_;
	bool drew_ = false;
};

} // namespace godot
```

Create `extension/src/render/transparent_raster_pass.cpp`:

```cpp
#include "render/transparent_raster_pass.h"
#include "render/gbuffer.h"
#include "render/lod_pool.h"
#include "gpu_layout/blocks.h"
#include "lod/lod_contour.h"
#include <godot_cpp/variant/packed_color_array.hpp>

using namespace godot;

TransparentRasterPass::~TransparentRasterPass() {
	teardown();
}

void TransparentRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "TransparentRasterPass", "transparent.vert.glsl",
			"transparent.frag.glsl");
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!shader_.is_valid()) teardown();
}

void TransparentRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = sampler_ = RID();
	front_ = trans_ = depth_ = args_ = RID();
	set_ = gpu::SetCache();
	framebuffer_ = gpu::FramebufferCache();
	size_ = Vector2i(0, 0);
	args_capacity_ = 0;
	drew_ = false;
	rd_ = nullptr;
}

bool TransparentRasterPass::ensure_targets(RenderingDevice *rd, Vector2i size) {
	if (size == size_ && front_.is_valid() && trans_.is_valid() && depth_.is_valid()) return true;
	framebuffer_.release(rd, group_);
	gpu::RdDevice device{rd};
	for (RID *r : {&front_, &trans_, &depth_}) {
		group_.free(device, *r);
		*r = RID();
	}
	const uint32_t colour = RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	front_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, size, colour);
	trans_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, size, colour);
	depth_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_D32_SFLOAT, size,
			RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	size_ = size;
	return front_.is_valid() && trans_.is_valid() && depth_.is_valid();
}

bool TransparentRasterPass::ensure_args(RenderingDevice *rd, int pages) {
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

bool TransparentRasterPass::draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
		RID beauty_cam_ubo, float fade_start, float fade_end, bool front_face_clockwise) {
	drew_ = false;
	if (!rd_ || rd != rd_ || !shader_.is_valid() || !gb.is_valid()) return false;
	if (pages_.empty()) return true;
	if (!index_array.is_valid() || !beauty_cam_ubo.is_valid()) return false;
	if (!ensure_targets(rd, gb.size()) || !ensure_args(rd, static_cast<int>(pages_.size()))) return false;
	if (!framebuffer_.get(rd, group_, {front_, trans_, depth_}).is_valid()) return false;
	if (!pipeline_.is_valid() || pipeline_clockwise_ != front_face_clockwise) {
		gpu::RdDevice device{rd};
		group_.free(device, pipeline_);
		gpu::RasterState state;
		// Front faces only, with the winding LodRasterPass MEASURED (M5 errata 2); the
		// nearest shell fragment wins on this pass's own reverse-Z depth.
		state.cull = RenderingDevice::POLYGON_CULL_BACK;
		state.front = front_face_clockwise ? RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE
				: RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		state.color_attachments = 2;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
		pipeline_clockwise_ = front_face_clockwise;
	}
	if (!pipeline_.is_valid()) return false;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, shader_, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::storage(5, pool.normal_buffer()),
			gpu::sampled(6, sampler_, gb.depth()),
			gpu::ubo(7, beauty_cam_ubo)});
	if (!set.is_valid()) return false;

	// Device-level upload before the draw list opens (M2 Task 12's ordering rule); same
	// command layout as LodPool::upload_draw_args.
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

	PackedColorArray clears;
	clears.push_back(Color(0, 0, 0, 0));
	clears.push_back(Color(0, 0, 0, 0));
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(),
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH, clears, 0.0f);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	rd->draw_list_bind_index_array(dl, index_array);
	const ve::TransparentRasterPush push{{fade_start, fade_end, 0.0f, 0.0f}};
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	rd->draw_list_draw_indirect(dl, true, args_, 0, static_cast<uint32_t>(pages_.size()), 20);
	rd->draw_list_end();
	drew_ = true;
	return true;
}
```

If `gpu::FramebufferCache::release` or `gpu::texture`'s default-data parameter differ from these calls, match the calls in `composite_pass.cpp` and `deferred_pass.cpp`. Those are the reference usages.

- [ ] **Step 6: Own the pass and call it**

`extension/src/render/orchestrator.h`: forward-declare `class TransparentRasterPass;` and add `TransparentRasterPass *transparent_raster = nullptr;` to `RenderPasses` after `leaf_raster`.
`orchestrator.cpp`: include `render/transparent_raster_pass.h`. After the leaf raster creation (line ~286):

```cpp
	passes_.transparent_raster = new TransparentRasterPass();
	passes_.transparent_raster->initialize(device);
```

and in teardown, beside the leaf raster delete:

```cpp
	if (passes_.transparent_raster) { delete passes_.transparent_raster; passes_.transparent_raster = nullptr; }
```

`extension/src/render/frame.h`: add `kStageTransparentRaster,` before `kStageCount`. `frame.cpp`: append `"transparent_raster"` to the `kNames` array in `frame_stage_name`, in the same position. Include `render/transparent_raster_pass.h`.

In `render_pre_opaque`, after the leaves block and before `SsgiPass *ssgi = ...`:

```cpp
	// Transparency, far half (spec §5): the shell's front faces into the far front layer,
	// after every producer of G-buffer depth, so grass and leaves in front of ice hide it.
	// A failure cancels the marker and skips the far layer -- never aborts the frame.
	bool far_front = false;
	TransparentRasterPass *shell = render_.passes().transparent_raster;
	if (shell) shell->set_draw_pages({});
	if (shell && transparency.enabled && !in.debug.skip_far_field && lod_.pool() && lod_raster) {
		std::vector<LodRasterPass::PageDraw> shell_pages;
		for (const ve::LodPageDraw &pd : lod_.transparent_draw_pages())
			shell_pages.push_back(LodRasterPass::PageDraw{pd.page, pd.quad_count});
		shell->set_draw_pages(shell_pages);
		timings->begin(rd, "transparent_raster");
		const bool shell_ok = shell->draw(rd, *lod_.pool(), lod_raster->index_array(), *gb,
				ubo->buffer(), fade_start, fade_end, lod_raster->front_face_clockwise());
		if (shell_ok) end_stage(rd, kStageTransparentRaster);
		else cancel_stage(kStageTransparentRaster);
		far_front = shell_ok && shell->drew();
	}
```

`far_front` is consumed in Task 8. Until then, mark it `(void)far_front;` so the build is warning-free.

- [ ] **Step 7: The readouts**

In `debug_render_frame` (hooks.cpp), inside the readout block from Task 5:

```cpp
		TransparentRasterPass *shell = world_->context().render->passes().transparent_raster;
		d["transparent_pages"] = shell ? shell->draw_page_count() : 0;
		d["far_center_front"] = Color();
		if (shell && shell->drew()) {
			const PackedByteArray ff = device->texture_get_data(shell->front(), 0);
			if (ff.size() >= (c + 1) * 16) {
				const float *f = reinterpret_cast<const float *>(ff.ptr()) + c * 4;
				d["far_center_front"] = Color(f[0], f[1], f[2], f[3]);
			}
		}
```

Include `render/transparent_raster_pass.h`.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: all PASS.

- [ ] **Step 9: Re-run the pins**

Run: `./gdunit_tests.sh -a res://tests/test_frame_shipped_golden.gd -a res://tests/test_frame_contract.gd -a res://tests/test_render_lifetime_contract.gd`
Expected: PASS. With no ice there are no shell pages, and the pass records nothing.

- [ ] **Step 10: Commit** (with the generated `.import` sidecars of both new shaders)

```bash
git add shaders/transparent.vert.glsl shaders/transparent.frag.glsl shaders/transparent.*.import \
	extension/src/render/transparent_raster_pass.h extension/src/render/transparent_raster_pass.cpp \
	extension/src/gpu_layout/blocks.h shaders/generated/blocks.glslh \
	extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp \
	extension/src/render/frame.h extension/src/render/frame.cpp extension/src/debug/hooks.cpp \
	tests/test_transparency.gd
git commit -m "feat: far-field transparent shell raster into its own front layer"
```

---

### Task 8: The composite, shading the ice

**Files:**
- Create: `shaders/sun_map.glslh`, `shaders/transparency_composite.comp.glsl`
- Create: `extension/src/render/transparency_composite_pass.h`, `extension/src/render/transparency_composite_pass.cpp`
- Modify: `shaders/deferred.comp.glsl` (a verbatim move only), `extension/src/render/deferred_pass.h` (accessor)
- Modify: `extension/src/gpu_layout/blocks.h`, `shaders/generated/blocks.glslh`
- Modify: `extension/src/render/orchestrator.h`, `.cpp`, `extension/src/render/frame.h`, `.cpp`
- Test: `tests/test_transparency.gd`

**Interfaces:**
- Consumes: `RaymarchPass::front_texture()` / `trans_texture()` (Task 5), `TransparentRasterPass::front()` / `trans()` / `drew()` (Task 7), `DeferredPass::sun_cascade_ubo()`, `DeferredPass::Params` (the frame's `dp`), the SunLight UBO and the BeautyCam UBO.
- Produces: `TransparencyCompositePass` with `initialize(RenderingDevice*)`, `teardown()`, `set_sun_ubo(RID)`, and `bool render(RenderingDevice*, GBuffer&, const MaterialAtlas&, RID near_front, RID near_trans, RID far_front, RID far_trans, bool far_drawn, RID sun_map, RID sun_cascade_ubo, RID beauty_cam_ubo, const Params&)`, where `Params { float right[3], up[3], tan_x, tan_y, ambient[3], fade_start, fade_end; uint32_t flags; }`.
- Produces: block `TransparencyCompositePush { float right_tanx[4]; float up_tany[4]; float sky[4]; float fade[4]; uint32_t flags[4]; }` with macro `TRANSPARENCY_COMPOSITE_PUSH_FIELDS`, and `FrameStage::kStageTransparency` ("transparency").

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_transparency.gd`:

```gdscript
# --- the composite (spec §7) -------------------------------------------------------------

func dist(a: Color, b: Color) -> float:
	return Vector3(a.r - b.r, a.g - b.g, a.b - b.b).length()

func test_a_scene_without_transparency_is_bit_identical_with_the_feature_on_and_off() -> void:
	var w := make_world()
	w.set_transparency_value("enabled", 1.0)
	var on := frame(w)
	w.set_transparency_value("enabled", 0.0)
	var off := frame(w)
	assert_int(int(on["lit_checksum"])).override_failure_message(
		"transparency changed a frame with no transparent material in it").is_equal(
		int(off["lit_checksum"]))

func test_clear_ice_shows_the_ground_through_it() -> void:
	var w := make_world()
	var ground := centre_hit(w)
	var bare: Color = frame(w)["center_lit"]
	w.hooks().debug_apply_sphere_add(ground, 1.0, material_id(w, "ice"))
	settle(w)
	w.set_transparency_value("enabled", 0.0)
	var opaque: Color = frame(w)["center_lit"]
	w.set_transparency_value("enabled", 1.0)
	var d := frame(w)
	var clear: Color = d["center_lit"]
	var ok: PackedStringArray = d["stages_ok"]
	assert_bool(ok.has("transparency")).is_true()
	assert_float(dist(clear, opaque)).override_failure_message(
		"the composite left the pixel as opaque ice: %s vs %s" % [clear, opaque]).is_greater(0.01)
	assert_float(dist(clear, bare)).override_failure_message(
		"clear ice should sit nearer the bare ground than opaque ice does").is_less(dist(opaque, bare))

func test_thicker_ice_moves_the_pixel_further_from_the_ground() -> void:
	var thin_w := make_world()
	var g := centre_hit(thin_w)
	var bare: Color = frame(thin_w)["center_lit"]
	thin_w.hooks().debug_apply_sphere_add(g, 0.6, material_id(thin_w, "ice"))
	settle(thin_w)
	var thin: Color = frame(thin_w)["center_lit"]
	var thick_w := make_world()
	thick_w.hooks().debug_apply_sphere_add(centre_hit(thick_w), 1.6, material_id(thick_w, "ice"))
	settle(thick_w)
	var thick: Color = frame(thick_w)["center_lit"]
	assert_float(dist(thick, bare)).is_greater(dist(thin, bare))

func test_ice_frames_are_finite_and_deterministic() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.0, material_id(w, "ice"))
	settle(w)
	var a := frame(w)
	var b := frame(w)
	assert_bool(finite(a["center_lit"])).is_true()
	assert_int(int(a["lit_checksum"])).is_equal(int(b["lit_checksum"]))
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: `test_clear_ice_shows_the_ground_through_it` FAILs, because there is no `transparency` stage and ice is invisible. `test_thicker_ice...` FAILs or is indistinguishable. The bit-identical case already PASSes, which is right: it is the pin.

- [ ] **Step 3: Move the sun-map helpers verbatim**

Create `shaders/sun_map.glslh` holding **exactly** the `sun_cascade_of` and `sun_map_visibility` functions cut from `shaders/deferred.comp.glsl` (their comments included), under this header:

```glsl
// The sun-map lookup, shared by deferred.comp.glsl and transparency_composite.comp.glsl.
// Moved verbatim out of deferred.comp.glsl. The includer must first define SUN_CASCADES and
// declare `sun` (a SUN_CASCADE_BLOCK_FIELDS uniform block) and `sun_map` (sampler2DArray).
//
// NOTE: never put a literal include directive inside a comment in this file -- the loader
// matches include tokens anywhere in a line and would self-include.
```

In `shaders/deferred.comp.glsl`, where the two functions were, write `#include "sun_map.glslh"`. Nothing else in the file changes.

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_deferred_golden.gd -a res://tests/test_frame_shipped_golden.gd`
Expected: PASS, identical. That proves the move changed nothing. Commit it alone:

```bash
git add shaders/sun_map.glslh shaders/deferred.comp.glsl
git commit -m "refactor: sun-map lookup moves to sun_map.glslh"
```

- [ ] **Step 4: The block and the accessor**

`extension/src/gpu_layout/blocks.h`:

```cpp
struct TransparencyCompositePush {
	float right_tanx[4];  // xyz = camera right, w = tan(fov_x / 2)
	float up_tany[4];     // xyz = camera up,    w = tan(fov_y / 2)
	float sky[4];         // xyz = ambient, w unused
	float fade[4];        // x = fade start, y = fade end (metres), zw unused
	uint32_t flags[4];    // x = beauty flags, y = 1 when the far front layer was drawn
};
```

```cpp
inline constexpr Field kTransparencyCompositePushFields[] = {
	VE_LAYOUT_FIELD(TransparencyCompositePush, right_tanx, Vec4, 0),
	VE_LAYOUT_FIELD(TransparencyCompositePush, up_tany, Vec4, 0),
	VE_LAYOUT_FIELD(TransparencyCompositePush, sky, Vec4, 0),
	VE_LAYOUT_FIELD(TransparencyCompositePush, fade, Vec4, 0),
	VE_LAYOUT_FIELD(TransparencyCompositePush, flags, UVec4, 0),
};
```

```cpp
	VE_LAYOUT_BLOCK(TransparencyCompositePush, "TRANSPARENCY_COMPOSITE_PUSH_FIELDS",
			kTransparencyCompositePushFields),
```

Regenerate `blocks.glslh` (`cd extension && scons -Q test; VE_REGEN_GOLDEN=1 ./build/tests/ve_tests; scons -Q test`).

`extension/src/render/deferred_pass.h`, public:

```cpp
	// The cascade block render() fills every frame. TransparencyCompositePass binds it rather
	// than keeping a second copy; it is valid once render() has run.
	RID sun_cascade_ubo() const { return sun_ubo_; }
```

- [ ] **Step 5: The shader**

Create `shaders/transparency_composite.comp.glsl`:

```glsl
#[compute]
#version 460
#include "generated/gbuffer.glslh"
#include "generated/blocks.glslh"

#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 11
#define VE_MATERIAL_ARRAYS
layout(set = 0, binding = 8) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 9) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"
#include "sun_light.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 10
#include "beauty_camera.glslh"

// Shades transparent fronts over what deferred lit behind them
// (docs/superpowers/specs/2026-09-29-transparent-materials-design.md §7):
//   F * sky + (1 - F) * (T * behind + (1 - T) * body)
// where behind is the lit pixel already here, body the material cel-shaded at the front, T
// the transmittance the walk or the shell measured, and F Schlick's Fresnel for the
// material's ior. Pixels with no front are left exactly as deferred wrote them.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba16f) uniform image2D lit;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D near_front; // march resolution
layout(set = 0, binding = 3) uniform sampler2D near_trans;
layout(set = 0, binding = 4) uniform sampler2D far_front;  // full resolution
layout(set = 0, binding = 5) uniform sampler2D far_trans;
layout(set = 0, binding = 6) uniform sampler2DArray sun_map;
#define SUN_CASCADES 3
layout(set = 0, binding = 7, std140) uniform SunBlock { SUN_CASCADE_BLOCK_FIELDS } sun;
#include "sun_map.glslh"

layout(push_constant, std430) uniform Push { TRANSPARENCY_COMPOSITE_PUSH_FIELDS } pc;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(lit);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);

	// The nearer front that owns this pixel. The near field's is kept on the complement of
	// the dither the far shell was drawn with (transparent.frag.glsl), tested on the FRONT's
	// distance, so the two layers split the fade band exactly as terrain does. The near
	// targets are nearest-sampled, as composite.frag.glsl samples geometry.
	vec4 front = vec4(0.0);
	vec4 trans = vec4(0.0);
	vec4 nf = texture(near_front, uv);
	if (nf.w > 0.5) {
		float t_fade = clamp((nf.z - pc.fade.x) / max(pc.fade.y - pc.fade.x, 1e-3), 0.0, 1.0);
		if (bayer4(px) >= t_fade) {
			front = nf;
			trans = texture(near_trans, uv);
		}
	}
	if (pc.flags.y != 0u) {
		vec4 ff = texelFetch(far_front, px, 0);
		if (ff.w > 0.5 && (front.w < 0.5 || ff.z < front.z)) {
			front = ff;
			trans = texelFetch(far_trans, px, 0);
		}
	}
	if (front.w < 0.5) return;

	vec3 rd = normalize(beauty_world_from_depth(uv, 1.0) - bcam.cam.xyz);
	vec3 p = bcam.cam.xyz + rd * front.z;
	// Raster grass or leaves in front of the ice own the pixel.
	float depth = texelFetch(gb_depth, px, 0).r;
	if (depth > 0.0 && distance(beauty_world_from_depth(uv, depth), bcam.cam.xyz) < front.z - 0.02)
		return;

	uint mat = uint(front.w + 0.5);
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
	// A near front carries its own marched sun term; a far one (a < 0) reads the sun map, as
	// deferred does for every pixel the far field drew.
	float shadow = trans.a >= 0.0 ? trans.a
			: ((pc.flags.x & BEAUTY_SUN_MAP) != 0u ? sun_map_visibility(p, ndl, front.z) : 1.0);
	// ndv = 1 is cel_shade's "no rim" (see deferred.comp.glsl): a rim is a silhouette
	// stylization, and the ice's silhouette is not where its outline is.
	vec3 body = cel_shade(surf.rgb * mix(1.0, props.y, 0.65), pc.sky.rgb, ndl, 1.0, ndh, shadow,
			1.0, 1.0 - props.x, sun_light.rgb.xyz);
	float ior = mat_ior(mat);
	float f0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
	float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
	vec3 behind = imageLoad(lit, px).rgb;
	vec3 through = trans.rgb * behind + (1.0 - trans.rgb) * body;
	imageStore(lit, px, vec4(mix(through, sky_color(reflect(rd, n)), fresnel), 1.0));
}
```

`beauty_world_from_depth(uv, 1.0)` is the near plane (reverse-Z), so `rd` is the pixel's primary ray. That is the same construction `deferred.comp.glsl` uses to rebuild positions.

- [ ] **Step 6: The pass**

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

// Shades transparent fronts over the lit G-buffer (docs/superpowers/specs/2026-09-29-
// transparent-materials-design.md §7). Runs after deferred and before inject, rewriting
// gb.lit() in place for the pixels that have a front and leaving every other pixel alone.
class TransparencyCompositePass {
public:
	struct Params {
		float right[3] = {}, up[3] = {};
		float tan_x = 0.0f, tan_y = 0.0f;
		float ambient[3] = {};
		float fade_start = 0.0f, fade_end = 0.0f;
		uint32_t flags = 0; // beauty flags
	};

	~TransparencyCompositePass();
	void initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return program_.valid(); }
	// The SunLight UBO is owned by RenderOrchestrator; this pass only mirrors its RID.
	void set_sun_ubo(RID buffer) { sun_light_ubo_ = buffer; }
	bool render(RenderingDevice *rd, GBuffer &gb, const MaterialAtlas &materials,
			RID near_front, RID near_trans, RID far_front, RID far_trans, bool far_drawn,
			RID sun_map, RID sun_cascade_ubo, RID beauty_cam_ubo, const Params &p);

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
		const MaterialAtlas &materials, RID near_front, RID near_trans, RID far_front,
		RID far_trans, bool far_drawn, RID sun_map, RID sun_cascade_ubo, RID beauty_cam_ubo,
		const Params &p) {
	if (!is_valid() || !gb.is_valid() || !near_front.is_valid() || !near_trans.is_valid() ||
			!sun_cascade_ubo.is_valid() || !beauty_cam_ubo.is_valid() || !sun_light_ubo_.is_valid())
		return false;
	uint32_t flags = p.flags;
	if (!sun_map.is_valid()) flags &= ~ve::kFlagSunMap;
	// No far layer this frame: bind the near targets in its slots. flags.y = 0 means the
	// shader never reads them.
	const bool far = far_drawn && far_front.is_valid() && far_trans.is_valid();
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, {
			gpu::image(0, gb.lit()),
			gpu::sampled(1, sampler_nearest_, gb.depth()),
			gpu::sampled(2, sampler_nearest_, near_front),
			gpu::sampled(3, sampler_nearest_, near_trans),
			gpu::sampled(4, sampler_nearest_, far ? far_front : near_front),
			gpu::sampled(5, sampler_nearest_, far ? far_trans : near_trans),
			gpu::sampled(6, sampler_linear_, sun_map.is_valid() ? sun_map : dummy_far_),
			gpu::ubo(7, sun_cascade_ubo),
			gpu::sampled(8, materials.sampler(), materials.albedo_array()),
			gpu::sampled(9, materials.sampler(), materials.surface_array()),
			gpu::ubo(10, beauty_cam_ubo),
			gpu::ubo(11, sun_light_ubo_)});
	if (!set.is_valid()) return false;
	const ve::TransparencyCompositePush push{
			{p.right[0], p.right[1], p.right[2], p.tan_x},
			{p.up[0], p.up[1], p.up[2], p.tan_y},
			{p.ambient[0], p.ambient[1], p.ambient[2], 0.0f},
			{p.fade_start, p.fade_end, 0.0f, 0.0f},
			{flags, far ? 1u : 0u, 0u, 0u}};
	const Vector2i size = gb.size();
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, gpu::push_bytes(push),
			gpu::groups(size.x, 8), gpu::groups(size.y, 8));
}
```

- [ ] **Step 7: Own it and call it**

`orchestrator.h`: forward-declare `class TransparencyCompositePass;` and add `TransparencyCompositePass *transparency_composite = nullptr;` to `RenderPasses`. `orchestrator.cpp`: include the header. After the transparent raster creation:

```cpp
	passes_.transparency_composite = new TransparencyCompositePass();
	passes_.transparency_composite->initialize(device);
```

In the sun-UBO block (line ~238), add `if (passes_.transparency_composite) passes_.transparency_composite->set_sun_ubo(passes_.sun_ubo->buffer());`. In teardown, add `if (passes_.transparency_composite) { delete passes_.transparency_composite; passes_.transparency_composite = nullptr; }`.

`frame.h`: add `kStageTransparency,` after `kStageTransparentRaster`. `frame.cpp`: append `"transparency"` to `kNames` in the same position, and include the header. Replace `(void)far_front;` from Task 7. After `end_stage(rd, kStageDeferred);` and before `timings->begin(rd, "inject");`:

```cpp
	// Transparency, shading (spec §7): fronts over what deferred lit behind them. Failure
	// cancels the marker and leaves deferred's image -- never aborts the frame.
	if (TransparencyCompositePass *tc = render_.passes().transparency_composite;
			tc && transparency.enabled) {
		TransparencyCompositePass::Params tp;
		for (int k = 0; k < 3; k++) {
			tp.right[k] = cp.cam_right[k];
			tp.up[k] = cp.cam_up[k];
			tp.ambient[k] = beauty.ambient[k];
		}
		tp.tan_x = cp.params[0];
		tp.tan_y = cp.params[1];
		tp.fade_start = fade_start;
		tp.fade_end = fade_end;
		tp.flags = beauty_flags;
		TransparentRasterPass *shell_pass = render_.passes().transparent_raster;
		timings->begin(rd, "transparency");
		const bool tc_ok = tc->render(rd, *gb, *materials, rmp->front_texture(),
				rmp->trans_texture(), shell_pass ? shell_pass->front() : RID(),
				shell_pass ? shell_pass->trans() : RID(), far_front,
				use_sun ? sun->map() : RID(), deferred->sun_cascade_ubo(), ubo->buffer(), tp);
		if (tc_ok) end_stage(rd, kStageTransparency);
		else cancel_stage(kStageTransparency);
	}
```

If `beauty`, `cp`, `use_sun`, `sun` or `deferred` are declared under different names at that point in `render_pre_opaque`, use the local names already in scope. All of them are in scope at the `deferred->render(...)` call just above.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_transparency.gd`
Expected: every case PASS.

- [ ] **Step 9: Run the full pins**

Run: `./gdunit_tests.sh -a res://tests/test_frame_shipped_golden.gd -a res://tests/test_frame_contract.gd -a res://tests/test_deferred_golden.gd -a res://tests/test_composite_golden.gd -a res://tests/test_render_lifetime_contract.gd`
Expected: PASS, identical to baseline.

- [ ] **Step 10: Commit**

```bash
git add shaders/transparency_composite.comp.glsl shaders/transparency_composite.comp.glsl.import \
	extension/src/render/transparency_composite_pass.h extension/src/render/transparency_composite_pass.cpp \
	extension/src/render/deferred_pass.h extension/src/gpu_layout/blocks.h shaders/generated/blocks.glslh \
	extension/src/render/orchestrator.h extension/src/render/orchestrator.cpp \
	extension/src/render/frame.h extension/src/render/frame.cpp tests/test_transparency.gd
git commit -m "feat: transparency composite shades ice over the surface behind it"
```

---

### Task 9: Benchmark, full verification and the record

**Files:**
- Modify: `demo/benchmark.gd`
- Modify: `docs/superpowers/specs/2026-09-29-transparent-materials-design.md` (a Deviations section)
- Create: `reports/transparency-*/` (benchmark output, via the runner)

- [ ] **Step 1: Benchmark flags**

In `demo/benchmark.gd`, add `var _ice_radius := 0.0` beside the other script vars. In the argument loop, after the `--leaves=` branch:

```gdscript
		elif arg.begins_with("--transparency="):
			# Transparency on/off for A/B cost runs: 0 disables the walk, the shell and the
			# composite. The leaves flag's twin.
			_world.set_transparency_value("enabled", float(arg.trim_prefix("--transparency=")))
		elif arg.begins_with("--ice="):
			# An ice sphere of this radius where the camera first looks, so a transparency A/B
			# has something to see through. Pair with --transparency=0/1.
			_ice_radius = float(arg.trim_prefix("--ice="))
```

After `_cam = _player.get_node("Camera3D")`:

```gdscript
	if _ice_radius > 0.0:
		var hit: Dictionary = _world.raycast(_cam.global_position,
				-_cam.global_transform.basis.z, 200.0)
		var ice_id := 0
		for m in _world.material_table():
			if m["name"] == "ice":
				ice_id = m["id"]
		if hit["hit"] and ice_id > 0:
			_world.hooks().debug_apply_sphere_add(hit["pos"], _ice_radius, ice_id)
			print("benchmark: ice r=%.1f at %s" % [_ice_radius, hit["pos"]])
		else:
			push_warning("benchmark: --ice found no ground ahead; running without ice")
```

- [ ] **Step 2: Interleaved A/B/A cost run**

GPU timings are invalid on this machine (memory: gpu-timings-invalid-on-this-machine), so frame time is the measure, interleaved to cancel warm-up:

```bash
tools/run_benchmarks.sh transparency-A1 --ice=3 --transparency=0
tools/run_benchmarks.sh transparency-B  --ice=3 --transparency=1
tools/run_benchmarks.sh transparency-A2 --ice=3 --transparency=0
```

Expected: `print` line `benchmark: ice r=3.0 at ...` in each leg's log, and reports under `reports/transparency-*`. Record the steady-leg p50/p99 frame time for A1, B and A2. B minus the mean of A1 and A2 is the feature's cost. If the pass-level labels `transparency` and `transparent_raster` appear, report them as recorded, with the caveat that GPU timings here are unreliable.

- [ ] **Step 3: The full suites**

Run: `./build.sh --test && ./gdunit_tests.sh 2>&1 | tail -60`
Expected: native `Status: SUCCESS!`. The gdUnit failures are exactly the Task 0 baseline set; no suite fails that did not fail on clean main. For any difference, stash, re-run on main, and compare before blaming the change (memory: gdunit-baseline-failures).

- [ ] **Step 4: Visual check of the seam and of foliage in front of ice (Review Focus)**

Run the demo (`godot --path . demo/main.tscn`) and paint ice with the edit tool:
- A patch at ~130 m, in the fade band. Expect no double-dark or missing dither pixels where near and far ice meet.
- A patch with grass blades in front of it. Expect the blades to draw over the ice untinted.

Screenshot both into `reports/transparency-B/` and note in the record whether each looked right.

- [ ] **Step 5: Record the deviations in the spec**

Append to `docs/superpowers/specs/2026-09-29-transparent-materials-design.md`:

```markdown
## 11. Deviations recorded during planning

1. `composite.frag.glsl` is not edited: `transparency_composite` samples the marcher's
   march-resolution front targets directly, nearest-sampled like composite.frag's geometry.
2. The opaque-lattice pass always runs (identity without transparent labels); the shell
   passes early-out on the job's bit instead of the CPU skipping dispatches.
3. Shell quads share their chunk's pages after the skirts and are told apart by material;
   the opaque and shadow vertex shaders collapse them. No new arena range or quad bit.
4. The shell list is the CPU walk's pages holding shell quads, not HiZ-culled.
5. Grass and leaf sun marches always see through transparency; only the raymarcher's march
   honours `enabled`.
6. With `enabled` off, far-field ice still casts no sun-map shadow.
7. The composite reads one or two texels per pixel whenever enabled; its measured cost is
   the Step 2 steady-leg delta, written here as "B − mean(A1, A2) = X ms p50, Y ms p99".
8. §9's automated seam-probe check became a manual visual check (Task 9 Step 4): the
   existing seam probe marks terrain ownership, not front ownership, and a hook that
   re-derived front ownership would test a copy of the logic rather than the shipping pass.
```

Replace X and Y with the numbers measured in Step 2 before committing.

- [ ] **Step 6: Commit**

```bash
git add demo/benchmark.gd docs/superpowers/specs/2026-09-29-transparent-materials-design.md reports/transparency-A1 reports/transparency-B reports/transparency-A2
git commit -m "chore: transparency benchmark flags, measured cost and recorded deviations"
```
