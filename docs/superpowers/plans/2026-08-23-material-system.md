# Material System Implementation Plan

> **Status: Historical implementation record. Do not execute this plan.** The hardness
> architecture in Tasks 2, 7, 8, and 9 (GLSL hardness lookup, per-point scaling, and the
> Eikonal clamp) was superseded on 2026-08-24. The current design resolves hardness once
> from the center ray's material and stores one uniform effective radius; see
> `docs/superpowers/specs/2026-08-23-material-system-design.md` §5.

> Historical task steps remain below to document how the original implementation landed.

**Goal:** Give every material a hardness and a glow alongside its existing texture maps, apply the glow in rendering and the hardness in carving, and add a demo scene for choosing the material being placed.

**Architecture:** One `constexpr` table in C++ is authoritative. It is mirrored into GLSL as a committed generated header (gated by a byte-exact unit test) and exposed to GDScript through a bound method. Glow is added in the deferred pass only, reusing the albedo texture array's alpha channel — which currently holds an unread height map — as a per-texel mask. Hardness divides a carve's radius inside the field evaluator, which distorts the field's magnitude at material seams; a gated Eikonal clamp on the baked brick lattice lands **before** hardness is switched on so the resulting artifact never appears in a working build.

**Tech Stack:** C++20 (godot-cpp GDExtension, SCons), GLSL 460 compute/fragment via Godot `RenderingDevice`, GDScript 4.7, doctest for native tests, gdUnit4 for engine tests.

**Spec:** `docs/superpowers/specs/2026-08-23-material-system-design.md`

## Global Constraints

- **Material id `i + 1` is served by atlas layer `i`.** Material 0 is air and has no layer. Never change this mapping — every brick palette, override brick and stored volume already encodes it.
- **`hardness >= 1.0` for every material, static_asserted.** `ve::op_world_aabb` reports `pos ± op.radius`; region ranges, brick residency and connectivity re-marking are all built on it. A hardness below 1.0 makes a carve reach outside its own AABB, and such an op is silently dropped at region boundaries — a deleted edit, not a glitch. `op.radius` is the maximum reach; hardness only shrinks it.
- **`kMaterialLayers = 16`** (`extension/src/render/material_atlas.h:14`) is a compile-time bound mirrored in four shaders as `MATERIAL_LAYERS`. The table must never exceed it.
- **`kMaterialTextureSize = 512`, `kMaterialMipmaps = 10`.** All material PNGs are exactly 512×512 or `MaterialAtlas::initialize` fails.
- **Every C++/GLSL mirror is gated by a differential test.** `tests/test_field_diff.gd` and `tests/test_brick_diff.gd` diff the CPU and GPU field evaluators. If you change one side, you change both in the same task.
- **Native tests must not link godot-cpp.** Anything under `src/world/`, `src/generator/`, `src/core/`, `src/mesh/`, `src/connectivity/`, `src/lod/`, `src/shade/` is compiled into the pure test binary by `extension/SConstruct`. Keep `material_table.{h,cpp}` free of godot-cpp includes.
- **Build:** `./build.sh` (library), `./build.sh --test` (library + native tests).
- **Native tests only:** `cd extension && scons -Q test`
- **Engine tests:** `./gdunit_tests.sh -a res://tests/test_name.gd`

---

### Task 1: The material table

**Files:**
- Create: `extension/src/world/material_table.h`
- Create: `extension/src/world/material_table.cpp`
- Test: `extension/tests/test_material_table.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `ve::MaterialDef` (fields `name`, `asset`, `hardness`, `glow`, `glow_rgb[3]`, `flat_albedo[3]`, all `float` except the two `const char *`); `ve::kMaterials` (a `constexpr MaterialDef[]`); `ve::kMaterialCount` (`constexpr int`); `ve::material_hardness(uint16_t id) -> float`; `ve::material_glow(uint16_t id) -> float`. Ids out of range return `1.0f` and `0.0f` respectively.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_material_table.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/material_table.h"
#include <cstring>
#include <set>
#include <string>

TEST_CASE("every material is at least as hard as the baseline") {
	// hardness < 1.0 would make a carve reach outside op_world_aabb's pos +/- radius,
	// and an op that reaches outside its declared AABB is dropped at region boundaries.
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(ve::kMaterials[i].hardness >= 1.0f);
}

TEST_CASE("glow strengths are non-negative") {
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(ve::kMaterials[i].glow >= 0.0f);
}

TEST_CASE("material names and assets are unique and non-empty") {
	std::set<std::string> names, assets;
	for (int i = 0; i < ve::kMaterialCount; i++) {
		REQUIRE(ve::kMaterials[i].name != nullptr);
		REQUIRE(ve::kMaterials[i].asset != nullptr);
		CHECK(std::strlen(ve::kMaterials[i].name) > 0);
		CHECK(names.insert(ve::kMaterials[i].name).second);
		CHECK(assets.insert(ve::kMaterials[i].asset).second);
	}
}

TEST_CASE("lookups map id i+1 to table entry i and fail soft out of range") {
	CHECK(ve::material_hardness(1) == doctest::Approx(ve::kMaterials[0].hardness));
	CHECK(ve::material_glow(1) == doctest::Approx(ve::kMaterials[0].glow));
	// Air and out-of-range ids must carve at full radius and emit nothing.
	CHECK(ve::material_hardness(0) == doctest::Approx(1.0f));
	CHECK(ve::material_glow(0) == doctest::Approx(0.0f));
	CHECK(ve::material_hardness(9999) == doctest::Approx(1.0f));
	CHECK(ve::material_glow(9999) == doctest::Approx(0.0f));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons -Q test`
Expected: FAIL — compile error, `world/material_table.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `extension/src/world/material_table.h`:

```cpp
#pragma once
#include <cstdint>

namespace ve {

// The authoritative material definition. Material id i + 1 is served by atlas layer i;
// material 0 is air and has no layer. This table is mirrored into GLSL as
// shaders/material_table.glslh (see material_table_glsl(), gated by a byte-exact test)
// and into GDScript via VoxelWorld::material_table().
//
// `asset` is the two-digit prefix of this material's PNGs under assets/materials/, e.g.
// "01" for 01_basecolor.png. It is a string rather than an index because the layer order
// and the on-disk numbering are allowed to be read independently by tools/convert_materials.sh.
struct MaterialDef {
	const char *name;      // picker label
	const char *asset;     // "04" -> assets/materials/04_basecolor.png, ...
	float hardness;        // >= 1.0; divides a carve's radius. 1.0 = full radius.
	float glow;            // emissive strength; 0.0 = not emissive
	float glow_rgb[3];
	float flat_albedo[3];  // far-field and unknown-layer fallback
};

// Order IS atlas layer order, and must match MATERIALS in tools/convert_materials.sh.
inline constexpr MaterialDef kMaterials[] = {
	// name          asset  hardness glow  glow_rgb              flat_albedo
	{"grass_01",     "00",  1.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.36f, 0.55f, 0.22f}},
	{"rock",         "01",  3.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.45f, 0.42f, 0.40f}},
	{"ground_01",    "02",  1.4f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.50f, 0.35f, 0.20f}},
	{"breakstone",   "03",  2.2f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.62f, 0.60f, 0.66f}},
};

inline constexpr int kMaterialCount = static_cast<int>(sizeof(kMaterials) / sizeof(kMaterials[0]));

// A hardness below 1.0 would let a carve reach past op_world_aabb's pos +/- radius. Ops
// that reach outside their declared AABB are dropped at region boundaries, so this is a
// silent-data-loss bug, not a visual one. Caught at compile time instead.
constexpr bool material_hardness_floor_holds() {
	for (int i = 0; i < kMaterialCount; i++)
		if (!(kMaterials[i].hardness >= 1.0f)) return false;
	return true;
}
static_assert(material_hardness_floor_holds(), "material hardness must be >= 1.0");

// Fail soft for air (0) and any id with no table entry: full-radius carve, no emission.
float material_hardness(uint16_t id);
float material_glow(uint16_t id);

} // namespace ve
```

Create `extension/src/world/material_table.cpp`:

```cpp
#include "world/material_table.h"

namespace ve {

float material_hardness(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i < 0 || i >= kMaterialCount) return 1.0f;
	return kMaterials[i].hardness;
}

float material_glow(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i < 0 || i >= kMaterialCount) return 0.0f;
	return kMaterials[i].glow;
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons -Q test`
Expected: PASS — all four `test_material_table.cpp` cases green, no other suite regressed.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world/material_table.h extension/src/world/material_table.cpp extension/tests/test_material_table.cpp
git commit -m "feat: authoritative material table with hardness and glow"
```

---

### Task 2: Mirror the table into GLSL

**Files:**
- Modify: `extension/src/world/material_table.h` (add the emitter declaration)
- Modify: `extension/src/world/material_table.cpp` (add the emitter)
- Create: `shaders/material_table.glslh`
- Modify: `shaders/common.glslh:47-55` (replace `flat_material_albedo`'s switch)
- Test: `extension/tests/test_material_glslh.cpp`

**Interfaces:**
- Consumes: `ve::kMaterials`, `ve::kMaterialCount` from Task 1.
- Produces: `ve::material_table_glsl() -> std::string`, the exact intended contents of `shaders/material_table.glslh`. GLSL symbols `MATERIAL_COUNT` (int), `MAT_HARDNESS[]`, `MAT_GLOW[]` (float arrays), `MAT_GLOW_RGB[]`, `MAT_FLAT_ALBEDO[]` (vec3 arrays), all indexed by `material id - 1`, plus `float mat_hardness(uint)`, `float mat_glow(uint)`, `vec3 mat_glow_rgb(uint)`.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_material_glslh.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/material_table.h"
#include <fstream>
#include <iterator>
#include <string>

// The committed shaders/material_table.glslh is a MIRROR of ve::kMaterials. If they drift,
// hardness carves differently on CPU and GPU and the field-diff test starts failing in a
// place that gives no hint why. This test is the gate; its failure message is the fix.
TEST_CASE("the committed GLSL mirror matches the C++ table") {
	// The native test binary runs from extension/, so the repo root is one level up.
	std::ifstream f("../shaders/material_table.glslh");
	REQUIRE_MESSAGE(f.good(), "cannot open ../shaders/material_table.glslh");
	const std::string on_disk((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());
	const std::string expected = ve::material_table_glsl();
	CHECK_MESSAGE(on_disk == expected,
			"shaders/material_table.glslh is stale. Replace its entire contents with:\n"
			<< expected);
}

TEST_CASE("the emitter covers every material") {
	const std::string s = ve::material_table_glsl();
	CHECK(s.find("MATERIAL_COUNT = " + std::to_string(ve::kMaterialCount)) != std::string::npos);
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(s.find(ve::kMaterials[i].name) != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons -Q test`
Expected: FAIL — compile error, `ve::material_table_glsl` is not declared.

- [ ] **Step 3: Write the emitter**

Append to `extension/src/world/material_table.h`, inside `namespace ve`, after the `material_glow` declaration:

```cpp
// The exact intended contents of shaders/material_table.glslh. That file is committed and a
// unit test asserts it equals this string byte for byte; the test prints this text on
// failure, so regenerating is copy-and-paste. Emitting at runtime and registering through
// load_shader_source's override map was rejected: BrickGenPass compiles brick_gen.comp.glsl
// at render/orchestrator.cpp:182, before MaterialAtlas::initialize at :184, and
// clear_shader_source_overrides() (called by tests) would leave the include unresolvable.
std::string material_table_glsl();
```

and add `#include <string>` to the header's includes.

Append to `extension/src/world/material_table.cpp`:

```cpp
#include <sstream>

namespace {

// Fixed 6-decimal form so the emitted text is byte-stable across platforms and locales
// -- the mirror test compares strings, not floats.
std::string f(float v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(v));
	return buf;
}

std::string vec3(const float v[3]) {
	return "vec3(" + f(v[0]) + ", " + f(v[1]) + ", " + f(v[2]) + ")";
}

} // namespace

namespace ve {

std::string material_table_glsl() {
	std::ostringstream o;
	o << "// GENERATED from extension/src/world/material_table.h (ve::kMaterials) by\n"
	     "// ve::material_table_glsl(). Do not edit by hand: extension/tests/"
	     "test_material_glslh.cpp\n"
	     "// asserts this file byte for byte and prints the correct contents on failure.\n"
	     "//\n"
	     "// Every array is indexed by MATERIAL ID MINUS ONE. Id 0 is air and has no entry.\n"
	     "// NOTE: never put a literal include directive inside a comment in this file -- the\n"
	     "// loader matches include tokens anywhere in a line and would self-include.\n"
	     "\n"
	     "const int MATERIAL_COUNT = " << kMaterialCount << ";\n\n";

	o << "const float MAT_HARDNESS[MATERIAL_COUNT] = float[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << f(kMaterials[i].hardness) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "const float MAT_GLOW[MATERIAL_COUNT] = float[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << f(kMaterials[i].glow) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "const vec3 MAT_GLOW_RGB[MATERIAL_COUNT] = vec3[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << vec3(kMaterials[i].glow_rgb) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "const vec3 MAT_FLAT_ALBEDO[MATERIAL_COUNT] = vec3[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << vec3(kMaterials[i].flat_albedo) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "// Mirrors of ve::material_hardness / ve::material_glow, including their fail-soft\n"
	     "// rule: air and any id with no table entry carve at full radius and emit nothing.\n"
	     "float mat_hardness(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i < 0 || i >= MATERIAL_COUNT) ? 1.0 : MAT_HARDNESS[i];\n"
	     "}\n\n"
	     "float mat_glow(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i < 0 || i >= MATERIAL_COUNT) ? 0.0 : MAT_GLOW[i];\n"
	     "}\n\n"
	     "vec3 mat_glow_rgb(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i < 0 || i >= MATERIAL_COUNT) ? vec3(0.0) : MAT_GLOW_RGB[i];\n"
	     "}\n";
	return o.str();
}

} // namespace ve
```

Add `#include <cstdio>` to `material_table.cpp` for `snprintf`.

- [ ] **Step 4: Generate the committed file**

The mirror test prints the exact required contents. Rather than transcribing them by hand, emit the file directly with a throwaway program:

```bash
cd extension
cat > /tmp/emit_glslh.cpp <<'EOF'
#include "world/material_table.h"
#include <cstdio>
int main() { auto s = ve::material_table_glsl(); std::fwrite(s.data(), 1, s.size(), stdout); }
EOF
g++ -std=c++20 -Isrc /tmp/emit_glslh.cpp src/world/material_table.cpp -o /tmp/emit_glslh
/tmp/emit_glslh > ../shaders/material_table.glslh
rm /tmp/emit_glslh /tmp/emit_glslh.cpp
head -20 ../shaders/material_table.glslh
```

Expected: the file exists and starts with the `// GENERATED from` banner.

- [ ] **Step 5: Point `flat_material_albedo` at the table**

In `shaders/common.glslh`, add the include immediately after the `const float LATTICE_FILTER_PAD` block near the top of the file — before the first use — as its own line:

```glsl
#include "material_table.glslh"
```

Then replace the whole `flat_material_albedo` function (currently `shaders/common.glslh:47-55`):

```glsl
vec3 flat_material_albedo(uint mat_id) {
	switch (mat_id) {
		case 1: return vec3(0.36, 0.55, 0.22); // grass
		case 2: return vec3(0.45, 0.42, 0.40); // rock
		case 3: return vec3(0.50, 0.35, 0.20); // dirt
		case 4: return vec3(0.62, 0.60, 0.66); // fill (sphere-add tool)
		default: return vec3(1.0, 0.0, 1.0);   // error magenta
	}
}
```

with:

```glsl
// The far-field and unknown-layer fallback, read from the generated table so it cannot
// drift from ve::kMaterials. An id with no entry stays error magenta.
vec3 flat_material_albedo(uint mat_id) {
	int i = int(mat_id) - 1;
	if (i < 0 || i >= MATERIAL_COUNT) return vec3(1.0, 0.0, 1.0);
	return MAT_FLAT_ALBEDO[i];
}
```

- [ ] **Step 6: Run the native tests**

Run: `cd extension && scons -Q test`
Expected: PASS — both `test_material_glslh.cpp` cases green.

- [ ] **Step 7: Verify every shader still compiles**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_shader_reload.gd -a res://tests/test_gpu_smoke.gd -a res://tests/test_raymarch_magenta.gd`
Expected: PASS. `test_raymarch_magenta.gd` is the one that pins the error-magenta fallback, which Step 5 just rewrote.

- [ ] **Step 8: Commit**

```bash
git add extension/src/world/material_table.h extension/src/world/material_table.cpp \
        extension/tests/test_material_glslh.cpp shaders/material_table.glslh shaders/common.glslh
git commit -m "feat: mirror the material table into GLSL, gated by a byte-exact test"
```

---

### Task 3: Feed the atlas and GDScript from the table

**Files:**
- Modify: `extension/src/render/material_atlas.cpp:20-26` (drop `kMaterialNames`), `:110-140` (asset prefix)
- Modify: `extension/src/voxel_world.h` (declare `material_table`)
- Modify: `extension/src/voxel_world.cpp:139` (bind it)
- Modify: `tools/convert_materials.sh:15` (comment pointing at the table)
- Test: `extension/tests/test_material_table.cpp` (add converter agreement), `tests/test_material_registry.gd`

**Interfaces:**
- Consumes: `ve::kMaterials`, `ve::kMaterialCount` from Task 1.
- Produces: `VoxelWorld::material_table() -> Array` of `Dictionary`, each with keys `id` (int, the ve material id), `name` (String), `asset` (String), `hardness` (float), `glow` (float), `glow_color` (Color), `albedo` (Color). Ordered by id ascending.

- [ ] **Step 1: Write the failing converter-agreement test**

Append to `extension/tests/test_material_table.cpp`:

```cpp
#include <fstream>
#include <vector>

// tools/convert_materials.sh cannot include a C++ header, so its MATERIALS array is a
// second list of the same names. That is exactly the drift the old kMaterialNames had and
// nothing checked. This checks it.
TEST_CASE("the converter script's material list matches the table") {
	std::ifstream f("../tools/convert_materials.sh");
	REQUIRE_MESSAGE(f.good(), "cannot open ../tools/convert_materials.sh");
	std::string line, found;
	while (std::getline(f, line))
		if (line.rfind("MATERIALS=(", 0) == 0) { found = line; break; }
	REQUIRE_MESSAGE(!found.empty(), "no MATERIALS=( line in convert_materials.sh");

	const size_t open = found.find('('), close = found.rfind(')');
	REQUIRE(open != std::string::npos);
	REQUIRE(close != std::string::npos);
	std::istringstream items(found.substr(open + 1, close - open - 1));
	std::vector<std::string> names;
	for (std::string n; items >> n; ) names.push_back(n);

	REQUIRE(static_cast<int>(names.size()) == ve::kMaterialCount);
	for (int i = 0; i < ve::kMaterialCount; i++)
		CHECK(names[i] == ve::kMaterials[i].name);
}
```

Add `#include <sstream>` to the top of that test file.

- [ ] **Step 2: Run test to verify it passes already**

Run: `cd extension && scons -Q test`
Expected: PASS — Task 1's table was seeded with the four names the script already lists, so this test locks in agreement that currently holds by luck. If it FAILS, the table and script disagree today and the table in Task 1 is wrong; fix the table, not the script.

- [ ] **Step 3: Point the atlas at the table**

In `extension/src/render/material_atlas.cpp`, add `#include "world/material_table.h"` to the includes, and delete the anonymous-namespace constant:

```cpp
// Index order IS the layer order used by tools/convert_materials.sh. Layer i serves ve
// material id i + 1; material 0 is air and has no layer.
const char *kMaterialNames[] = {"grass_01", "rock", "ground_01", "breakstone"};
```

In `MaterialAtlas::initialize`, replace:

```cpp
	const int source_count = static_cast<int>(std::size(kMaterialNames));
```

with:

```cpp
	// The table is authoritative for how many layers have real art; the rest stay flat
	// error magenta so an out-of-range id is visible rather than undefined.
	const int source_count = ve::kMaterialCount < kMaterialLayers
			? ve::kMaterialCount : kMaterialLayers;
```

and replace the path construction:

```cpp
				std::snprintf(rel, sizeof(rel), "res://assets/materials/%02d_%s.png", layer,
						suffixes[m]);
```

with:

```cpp
				std::snprintf(rel, sizeof(rel), "res://assets/materials/%s_%s.png",
						ve::kMaterials[layer].asset, suffixes[m]);
```

`std::size` was `kMaterialNames`'s only use of `<iterator>`; drop that include from
`material_atlas.cpp` in the same edit. If the build then complains about a missing symbol,
something else in the file needed it — put it back rather than guessing.

- [ ] **Step 4: Bind the table to GDScript**

In `extension/src/voxel_world.h`, add to the public section alongside the other bound methods:

```cpp
	// The material registry, for the demo's picker. One dictionary per material, id
	// ascending. Bound rather than exposed as a property: it is constant for the process.
	Array material_table() const;
```

In `extension/src/voxel_world.cpp`, add `#include "world/material_table.h"` to the includes, add this line inside `_bind_methods()` (line 139 onward, next to `bind_method(D_METHOD("hooks"), ...)`):

```cpp
	ClassDB::bind_method(D_METHOD("material_table"), &VoxelWorld::material_table);
```

and add the implementation next to the other small accessors:

```cpp
Array VoxelWorld::material_table() const {
	Array out;
	for (int i = 0; i < ve::kMaterialCount; i++) {
		const ve::MaterialDef &m = ve::kMaterials[i];
		Dictionary d;
		d["id"] = i + 1; // material 0 is air; layer i serves id i + 1
		d["name"] = String(m.name);
		d["asset"] = String(m.asset);
		d["hardness"] = m.hardness;
		d["glow"] = m.glow;
		d["glow_color"] = Color(m.glow_rgb[0], m.glow_rgb[1], m.glow_rgb[2]);
		d["albedo"] = Color(m.flat_albedo[0], m.flat_albedo[1], m.flat_albedo[2]);
		out.push_back(d);
	}
	return out;
}
```

- [ ] **Step 5: Write the GDScript test**

Create `tests/test_material_registry.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# No streaming and no atlas init: material_table() is a constant table read, and a test
# that needed a GPU to read a constant would be paying 20 seconds for nothing.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	return w

func test_the_table_reaches_gdscript() -> void:
	var t: Array = make_world().material_table()
	assert_int(t.size()).is_greater_equal(4)
	var first: Dictionary = t[0]
	for key in ["id", "name", "asset", "hardness", "glow", "glow_color", "albedo"]:
		assert_bool(first.has(key)).override_failure_message(
			"material_table() entry is missing '%s'" % key).is_true()

# Ids are what the edit ops carry, and layer i serves id i + 1. A picker that wrote the
# ARRAY INDEX into fill_material would paint the wrong material with no visible error.
func test_ids_are_one_based_and_ascending() -> void:
	var t: Array = make_world().material_table()
	for i in range(t.size()):
		assert_int(t[i]["id"]).is_equal(i + 1)

func test_every_material_is_at_least_baseline_hardness() -> void:
	for m in make_world().material_table():
		assert_float(m["hardness"]).override_failure_message(
			"%s has hardness below 1.0, which lets a carve escape its own AABB" % m["name"]
			).is_greater_equal(1.0)
```

- [ ] **Step 6: Build and run both suites**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_material_registry.gd -a res://tests/test_material_atlas.gd`
Expected: PASS — `test_material_atlas.gd` still green, proving the atlas loads the same four layers through the table that it used to load through `kMaterialNames`.

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/material_atlas.cpp extension/src/voxel_world.h \
        extension/src/voxel_world.cpp extension/tests/test_material_table.cpp \
        tests/test_material_registry.gd
git commit -m "feat: drive the material atlas and GDScript from the table"
```

---

### Task 4: Pack a glow mask into the albedo array's alpha

**Files:**
- Modify: `tools/convert_materials.sh` (emit glow masks)
- Modify: `extension/src/render/material_atlas.cpp:40-80` (`pack_layer`), `:82-100` (`flat_layer`), `:110-140` (optional map load)
- Create: `assets/materials/04_*.png` (magma source art)
- Modify: `extension/src/world/material_table.h` (add magma), `shaders/material_table.glslh` (regenerate)
- Test: `tests/test_material_atlas.gd`

**Interfaces:**
- Consumes: `ve::kMaterials` from Task 1, the emitter from Task 2.
- Produces: the albedo texture array's **alpha channel** now carries a per-texel glow mask in `[0, 1]`, flat `1.0` for any material shipping no `NN_glow.png`. GLSL reads it as `material_surface(...).a`. Material id 5 (`magma`, asset `04`) exists with `glow = 6.0`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_material_atlas.gd`:

```gdscript
# The albedo array's alpha used to carry a height map that no shader ever read. It now
# carries the glow mask. A material with no glow PNG packs flat 1.0 so the table's scalar
# still drives it uniformly -- that fallback is why "glow" and "glow mask" are separable.
func test_the_glow_mask_lands_in_the_albedo_alpha() -> void:
	var w := make_world()
	var d := w.hooks().debug_material_alpha_stats(4) # layer 4 == magma
	assert_bool(d.has("min")).override_failure_message(
		"debug_material_alpha_stats returned nothing for layer 4").is_true()
	# Cracks glow, raised stone does not: the mask must actually vary.
	assert_float(d["max"] - d["min"]).override_failure_message(
		"magma's glow mask is flat: the converter did not emit 04_glow.png"
		).is_greater(0.2)

func test_a_material_without_a_glow_png_packs_a_flat_mask() -> void:
	var w := make_world()
	var d := w.hooks().debug_material_alpha_stats(0) # layer 0 == grass_01, no glow art
	assert_float(d["min"]).is_equal_approx(1.0, 0.02)
	assert_float(d["max"]).is_equal_approx(1.0, 0.02)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_material_atlas.gd`
Expected: FAIL — `debug_material_alpha_stats` is not a method of the hooks object.

- [ ] **Step 3: Add the debug hook**

In `extension/src/debug/hooks.h`, declare next to `debug_material_atlas_stats` (line 268):

```cpp
	// Min/max of one albedo-array layer's ALPHA channel across the top mip. Alpha carries
	// the glow mask; a flat 1.0 means the material ships no glow PNG.
	Dictionary debug_material_alpha_stats(int layer);
```

In `extension/src/debug/hooks.cpp`, bind it next to line 236:

```cpp
	ClassDB::bind_method(D_METHOD("debug_material_alpha_stats", "layer"),
			&VoxelDebugHooks::debug_material_alpha_stats);
```

and implement it next to `debug_material_atlas_stats`:

```cpp
Dictionary VoxelDebugHooks::debug_material_alpha_stats(int layer) {
	Dictionary d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->initialized_ || !device || !world_->material_atlas() ||
			!world_->material_atlas()->is_valid()) return d;
	if (layer < 0 || layer >= world_->material_atlas()->layer_count()) return d;
	const PackedByteArray data =
			device->texture_get_data(world_->material_atlas()->albedo_array(), layer);
	const int64_t top = static_cast<int64_t>(kMaterialTextureSize) * kMaterialTextureSize * 4;
	if (data.size() < top) return d;
	const uint8_t *p = data.ptr();
	uint8_t lo = 255, hi = 0;
	for (int64_t i = 3; i < top; i += 4) { // alpha is every 4th byte
		if (p[i] < lo) lo = p[i];
		if (p[i] > hi) hi = p[i];
	}
	d["min"] = lo / 255.0f;
	d["max"] = hi / 255.0f;
	return d;
}
```

- [ ] **Step 4: Convert the magma source art**

`ground_crack_01` ships the same five maps the converter already consumes, and its cracks are the LOW regions of the height map, so inverting it lights the cracks and leaves the raised stone dark.

In `tools/convert_materials.sh`, change the materials list and add the glow pass. Replace:

```bash
MATERIALS=(grass_01 rock ground_01 breakstone)
```

with:

```bash
# This list must match ve::kMaterials in extension/src/world/material_table.h, in order.
# extension/tests/test_material_table.cpp asserts it.
MATERIALS=(grass_01 rock ground_01 breakstone ground_crack_01)

# Materials whose glow mask is derived from the inverted height map: the crevices glow and
# the raised surface stays dark. A material absent here ships no NN_glow.png, and
# MaterialAtlas packs a flat 1.0 mask so its table glow strength applies uniformly.
GLOW_FROM_INVERTED_HEIGHT=(ground_crack_01)
```

and append, inside the per-material loop after the existing `for map in "${MAPS[@]}"` loop closes:

```bash
	for g in "${GLOW_FROM_INVERTED_HEIGHT[@]}"; do
		[ "$g" = "$m" ] || continue
		out="$DST/$(printf '%02d' "$i")_glow.png"
		convert "$SRC/$m/T_${m}_height.tga" -resize 512x512! -negate \
			-level 40%,100% -strip "PNG24:$out"
		echo "  $out"
	done
```

Run it against the local library:

```bash
./tools/convert_materials.sh ~/Development/unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2
ls assets/materials/04_*
```

Expected: `04_ambientOcclusion.png 04_basecolor.png 04_glow.png 04_height.png 04_normal.png 04_roughness.png`

- [ ] **Step 5: Add magma to the table and regenerate the mirror**

In `extension/src/world/material_table.h`, add as the last entry of `kMaterials`:

```cpp
	{"ground_crack_01", "04", 1.1f, 6.0f, {1.00f, 0.35f, 0.08f}, {0.35f, 0.12f, 0.06f}},
```

(The `name` must equal the source directory in the texture library so the converter-agreement test passes; the picker's label comes from this same string.)

Regenerate the GLSL mirror using the same throwaway emitter as Task 2 Step 4:

```bash
cd extension
cat > /tmp/emit_glslh.cpp <<'EOF'
#include "world/material_table.h"
#include <cstdio>
int main() { auto s = ve::material_table_glsl(); std::fwrite(s.data(), 1, s.size(), stdout); }
EOF
g++ -std=c++20 -Isrc /tmp/emit_glslh.cpp src/world/material_table.cpp -o /tmp/emit_glslh
/tmp/emit_glslh > ../shaders/material_table.glslh
rm /tmp/emit_glslh /tmp/emit_glslh.cpp
```

- [ ] **Step 6: Pack the mask instead of the height map**

In `extension/src/render/material_atlas.cpp`, change `pack_layer`'s signature and its alpha write. Replace:

```cpp
PackedByteArray pack_layer(bool albedo, const std::array<Ref<Image>, 5> &maps) {
```

with:

```cpp
// `maps[4]` is the GLOW MASK, not the height map. Height was packed into the albedo
// array's alpha until this change and read by exactly nothing; the channel now carries
// per-texel emission. A material with no glow PNG gets a flat 1.0 mask from the caller,
// so its table glow strength applies uniformly rather than not at all.
PackedByteArray pack_layer(bool albedo, const std::array<Ref<Image>, 5> &maps) {
```

and replace the height read:

```cpp
	const PackedByteArray height = maps[4]->get_data();
	...
	const uint8_t *hp = height.ptr();
```

with:

```cpp
	const PackedByteArray glow = maps[4]->get_data();
	...
	const uint8_t *gp = glow.ptr();
```

and the alpha write:

```cpp
			if (albedo) {
				// albedo <- basecolor RGB, height in A.
				p[0] = bp[o + 0];
				p[1] = bp[o + 1];
				p[2] = bp[o + 2];
				p[3] = hp[o + 0];
```

with:

```cpp
			if (albedo) {
				// albedo <- basecolor RGB, glow mask in A.
				p[0] = bp[o + 0];
				p[1] = bp[o + 1];
				p[2] = bp[o + 2];
				p[3] = gp[o + 0];
```

In `flat_layer`, change the albedo branch so unused layers carry a fully-lit mask rather than 0 (they are error magenta with zero glow strength, so the mask value is inert, but a 1.0 keeps the channel's meaning uniform):

```cpp
			px[0] = 255; px[1] = 0; px[2] = 255; px[3] = 255; // error magenta, mask 1.0
```

In `MaterialAtlas::initialize`, make the fifth map optional. Replace:

```cpp
			const char *suffixes[] = {"basecolor", "normal", "roughness",
					"ambientOcclusion", "height"};
			for (int m = 0; m < 5; m++) {
				char rel[256];
				std::snprintf(rel, sizeof(rel), "res://assets/materials/%s_%s.png",
						ve::kMaterials[layer].asset, suffixes[m]);
				const String path = ProjectSettings::get_singleton()->globalize_path(rel);
				PackedByteArray bytes = load_png(path);
				if (!decode_png(bytes, &maps[m])) {
					UtilityFunctions::printerr("MaterialAtlas: failed to load ", path);
					teardown();
					return false;
				}
			}
```

with:

```cpp
			const char *suffixes[] = {"basecolor", "normal", "roughness",
					"ambientOcclusion", "glow"};
			for (int m = 0; m < 5; m++) {
				char rel[256];
				std::snprintf(rel, sizeof(rel), "res://assets/materials/%s_%s.png",
						ve::kMaterials[layer].asset, suffixes[m]);
				const String path = ProjectSettings::get_singleton()->globalize_path(rel);
				PackedByteArray bytes = load_png(path);
				if (decode_png(bytes, &maps[m])) continue;
				// The glow mask is the one optional map: a material with no NN_glow.png
				// packs a flat 1.0 so its table strength applies uniformly. Every other
				// missing map is a genuine asset error.
				if (m == 4) {
					Ref<Image> flat = Image::create_empty(kMaterialTextureSize,
							kMaterialTextureSize, false, Image::FORMAT_RGBA8);
					flat->fill(Color(1, 1, 1, 1));
					maps[m] = flat;
					continue;
				}
				UtilityFunctions::printerr("MaterialAtlas: failed to load ", path);
				teardown();
				return false;
			}
```

- [ ] **Step 7: Run the tests**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_material_atlas.gd`
Expected: PASS — the two new cases plus the four existing ones. `test_the_arrays_load_with_mips` should still report `layers >= 4`.

- [ ] **Step 8: Commit**

```bash
git add tools/convert_materials.sh assets/materials extension/src/render/material_atlas.cpp \
        extension/src/world/material_table.h shaders/material_table.glslh \
        extension/src/debug/hooks.h extension/src/debug/hooks.cpp tests/test_material_atlas.gd
git commit -m "feat: pack a glow mask into the albedo array's alpha, add magma"
```

---

### Task 5: Emit glow in the deferred pass

**Files:**
- Modify: `shaders/deferred.comp.glsl:80-98` (the shading tail)
- Test: `tests/test_material_glow.gd`

**Interfaces:**
- Consumes: `mat_glow`, `mat_glow_rgb` from Task 2's generated header (already reachable — `deferred.comp.glsl` includes `common.glslh`, which includes `material_table.glslh`); the alpha mask from Task 4.
- Produces: `gbuffer()->lit()` carries `albedo_shading + glow_rgb * glow * mask` for emissive materials. Values above 1.0 are intended and survive to Godot's glow stage.

- [ ] **Step 1: Write the failing test**

Create `tests/test_material_glow.gd`:

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
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func magma_id() -> int:
	for m in _worlds[0].material_table():
		if float(m["glow"]) > 0.0:
			return int(m["id"])
	return -1

# Paint a patch of terrain with the emissive material and look at it. The lit buffer is
# rgba16f, so the emissive term is visible as HDR above what the same geometry reads
# with a non-emissive material -- which is exactly what Godot's bloom keys off.
func test_an_emissive_material_is_brighter_than_a_dull_one() -> void:
	var w := make_world()
	var id := magma_id()
	assert_int(id).override_failure_message(
		"no material in the table has a non-zero glow").is_greater(0)
	var pos := Vector3(20.0, 75.0, 20.0)
	var down := Vector3(0, -1, 0)

	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), 6.0, 1) # dull
	var dull: Dictionary = w.hooks().debug_deferred_probe(pos, down, 64, 64, 0)
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), 6.0, id) # emissive
	var lit: Dictionary = w.hooks().debug_deferred_probe(pos, down, 64, 64, 0)

	assert_float(lit["mean_luma"]).override_failure_message(
		"the emissive material shaded no brighter than the dull one: glow is not applied"
		).is_greater(float(dull["mean_luma"]) * 1.5)

# Emission must survive as HDR. If it were clamped to 1.0 the term would still "work" in
# the lit buffer and then contribute nothing at all to Godot's glow, which thresholds above 1.
func test_emission_pushes_the_lit_buffer_above_one() -> void:
	var w := make_world()
	var id := magma_id()
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), 6.0, id)
	var d: Dictionary = w.hooks().debug_deferred_probe(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	var c: Color = d["center"]
	assert_float(maxf(c.r, maxf(c.g, c.b))).override_failure_message(
		"emissive pixel peaked at %.3f: nothing will bloom" % maxf(c.r, maxf(c.g, c.b))
		).is_greater(1.0)

# A non-emissive material must pay nothing and change nothing.
func test_a_non_emissive_material_is_unchanged() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), 6.0, 1)
	var d: Dictionary = w.hooks().debug_deferred_probe(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	var c: Color = d["center"]
	assert_float(maxf(c.r, maxf(c.g, c.b))).is_less(1.0)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_material_glow.gd`
Expected: FAIL — every case errors on `debug_apply_sphere_paint`, which is not a bound
method yet. Step 3 adds it; the emission assertions then fail for the real reason.

- [ ] **Step 3: Add the paint hook the tests need**

`debug_apply_sphere_paint` does not exist — `extension/src/debug/hooks.cpp` binds only
`debug_apply_sphere_subtract` and `debug_apply_sphere_add`. Painting is the operation these
tests need, because it changes a surface's material without changing its geometry, so the
emissive and dull probes compare the same shape.

In `extension/src/debug/hooks.h`, declare it next to `debug_apply_sphere_add` (line 355):

```cpp
	// Recolour existing solid without moving the surface, so a test can compare two
	// materials over identical geometry. Mirror of VoxelEditTool::apply_sphere_paint.
	void debug_apply_sphere_paint(Vector3 centre, float radius, int material);
```

In `extension/src/debug/hooks.cpp`, bind it next to `debug_apply_sphere_add`'s binding
(line 141):

```cpp
	ClassDB::bind_method(D_METHOD("debug_apply_sphere_paint", "centre", "radius", "material"),
			&VoxelDebugHooks::debug_apply_sphere_paint);
```

and implement it immediately after `debug_apply_sphere_add`:

```cpp
void VoxelDebugHooks::debug_apply_sphere_paint(Vector3 centre, float radius, int material) {
	if (!world_->store_->edit_log()) world_->ensure_physics_initialized();
	ve::EditOp op;
	op.type = ve::kOpSpherePaint;
	op.material = static_cast<uint16_t>(material);
	op.pos[0] = centre.x;
	op.pos[1] = centre.y;
	op.pos[2] = centre.z;
	op.radius = radius;
	world_->append_edit(op);
}
```

- [ ] **Step 4: Add the emissive term**

In `shaders/deferred.comp.glsl`, replace the final `imageStore` of `main()`:

```glsl
	float ao = 1.0;
	if ((pc.flags.x & BEAUTY_SSAO) != 0u) ao = texture(ssao_tex, uv).r;
	imageStore(out_lit, px,
			vec4(cel_shade(g0.rgb, ambient, ndl, ndv, ndh, shadow, ao, g1.w), 1.0));
}
```

with:

```glsl
	float ao = 1.0;
	if ((pc.flags.x & BEAUTY_SSAO) != 0u) ao = texture(ssao_tex, uv).r;
	vec3 lit = cel_shade(g0.rgb, ambient, ndl, ndv, ndh, shadow, ao, g1.w);

	// Emission is ADDED after shading, never lit: a glowing surface is its own light source.
	// The whole block is skipped for any material whose table strength is zero, which is
	// every material but the emissive ones -- so dull terrain pays one array read.
	//
	// The mask is the albedo array's alpha (see MaterialAtlas::pack_layer). It is sampled
	// here rather than carried through the G-buffer because no G-buffer channel is free,
	// and widening it would make every pixel pay for a feature one material uses.
	float glow = mat_glow(mat);
	if (glow > 0.0) {
		// This is a compute shader: there is no dFdx. Reconstruct the neighbouring pixels'
		// world positions from the depth buffer to get the triplanar gradients, the same
		// way the raymarcher derives them from ray differentials.
		vec3 wpos_x = wpos, wpos_y = wpos;
		ivec2 mx = min(px + ivec2(1, 0), size - 1);
		ivec2 my = min(px + ivec2(0, 1), size - 1);
		float dx_depth = texelFetch(gb_depth, mx, 0).r;
		float dy_depth = texelFetch(gb_depth, my, 0).r;
		if (dx_depth > 0.0) {
			vec2 nd = ((vec2(mx) + 0.5) / vec2(size)) * 2.0 - 1.0;
			vec4 hx = pc.inv_view_proj * vec4(nd, dx_depth, 1.0);
			wpos_x = hx.xyz / (abs(hx.w) < 1e-9 ? 1e-9 : hx.w);
		}
		if (dy_depth > 0.0) {
			vec2 nd = ((vec2(my) + 0.5) / vec2(size)) * 2.0 - 1.0;
			vec4 hy = pc.inv_view_proj * vec4(nd, dy_depth, 1.0);
			wpos_y = hy.xyz / (abs(hy.w) < 1e-9 ? 1e-9 : hy.w);
		}
		float mask = material_surface(mat, wpos, n, wpos_x - wpos, wpos_y - wpos).a;
		lit += mat_glow_rgb(mat) * glow * mask;
	}
	imageStore(out_lit, px, vec4(lit, 1.0));
}
```

- [ ] **Step 5: Run the test**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_material_glow.gd`
Expected: PASS — all four cases.

- [ ] **Step 6: Verify nothing else moved**

Run: `./gdunit_tests.sh -a res://tests/test_deferred.gd -a res://tests/test_ssgi.gd -a res://tests/test_gbuffer.gd -a res://tests/test_ssao.gd`
Expected: PASS. These pin the deferred pass's existing outputs; a non-emissive world must shade byte-identically.

- [ ] **Step 7: Commit**

```bash
git add shaders/deferred.comp.glsl tests/test_material_glow.gd \
        extension/src/debug/hooks.h extension/src/debug/hooks.cpp
git commit -m "feat: add per-material emission in the deferred pass"
```

---

### Task 6: Turn on Godot's bloom in the demo

**Files:**
- Modify: `demo/main.tscn` (the `Environment` sub-resource, id `3`)
- Test: `tests/test_demo_shell.gd`

**Interfaces:**
- Consumes: the HDR emission from Task 5.
- Produces: nothing other code reads. This is demo presentation only.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_demo_shell.gd`:

```gdscript
# Emission above 1.0 only becomes visible bloom if the Environment asks for it. The engine
# writes HDR into the colour buffer before Godot's glow stage; this is the other half.
func test_the_demo_environment_has_glow_enabled() -> void:
	var scene: PackedScene = load("res://demo/main.tscn")
	var root: Node = scene.instantiate()
	var we: WorldEnvironment = root.get_node("WorldEnvironment")
	assert_bool(we.environment.glow_enabled).override_failure_message(
		"main.tscn's Environment has glow disabled: emissive materials will not bloom"
		).is_true()
	assert_float(we.environment.glow_hdr_threshold).is_greater(0.0)
	root.free()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_demo_shell.gd`
Expected: FAIL — `glow_enabled` is false; the Environment sets only `background_mode` and ambient.

- [ ] **Step 3: Enable glow**

In `demo/main.tscn`, replace the Environment sub-resource:

```
[sub_resource type="Environment" id="3"]
background_mode = 4
ambient_light_source = 3
ambient_light_energy = 0.4
```

with:

```
[sub_resource type="Environment" id="3"]
background_mode = 4
ambient_light_source = 3
ambient_light_energy = 0.4
glow_enabled = true
glow_intensity = 0.8
glow_bloom = 0.1
glow_hdr_threshold = 1.0
glow_blend_mode = 1
```

`glow_hdr_threshold = 1.0` is the value Task 5's `test_emission_pushes_the_lit_buffer_above_one` exists to guarantee is reachable; `glow_blend_mode = 1` is additive, which is what a self-lit surface wants.

- [ ] **Step 4: Run the test**

Run: `./gdunit_tests.sh -a res://tests/test_demo_shell.gd`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add demo/main.tscn tests/test_demo_shell.gd
git commit -m "feat: enable additive bloom in the demo environment"
```

---

### Task 7: The Eikonal clamp

**Files:**
- Modify: `extension/src/world/brick_eval.cpp:161-215` (`eval_brick`)
- Modify: `extension/src/world/brick.h` (the clamp's constants and declaration)
- Create: `extension/src/world/brick_clamp.cpp`
- Test: `extension/tests/test_brick_clamp.cpp`

**Interfaces:**
- Consumes: `ve::material_hardness` from Task 1; `ve::Brick`, `ve::sdf_index`, `ve::encode_sdf`, `ve::decode_sdf` from `world/brick.h`.
- Produces: `ve::clamp_brick_lattice(uint8_t *sdf)` — in-place, operates on the encoded 17³ lattice, preserves every sample's sign, and leaves the result 1-Lipschitz on the grid. `ve::lattice_needs_clamp(const uint16_t *mat, int count, const EditOp *ops, int op_count) -> bool` — the gate.

**Why this lands before hardness:** the clamp is inert until two materials of differing
hardness meet, so it can be built and unit-tested in isolation, wired up in Task 8 while the
world still looks identical, and be already in place when Task 9 switches hardness on. The
artifact it prevents therefore never exists in a working build.

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_brick_clamp.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/brick.h"
#include "generator/edit_ops.h"
#include <cmath>
#include <vector>

namespace {

// Largest |d(neighbour) - d(sample)| over every axis-aligned pair in the lattice, in metres.
// A field a sphere tracer can safely step by has this at or below one voxel pitch.
float worst_step(const uint8_t *sdf) {
	float worst = 0.0f;
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const float d = ve::decode_sdf(sdf[ve::sdf_index(x, y, z)]);
				const int nb[3][3] = {{x + 1, y, z}, {x, y + 1, z}, {x, y, z + 1}};
				for (auto &n : nb) {
					if (n[0] >= ve::kBrickSdfStride || n[1] >= ve::kBrickSdfStride ||
							n[2] >= ve::kBrickSdfStride) continue;
					const float e = ve::decode_sdf(sdf[ve::sdf_index(n[0], n[1], n[2])]);
					worst = std::max(worst, std::fabs(e - d));
				}
			}
	return worst;
}

} // namespace

TEST_CASE("the clamp makes a discontinuous lattice 1-Lipschitz") {
	std::vector<uint8_t> sdf(ve::kBrickSdfCount);
	// A step discontinuity exactly like the one a hardness seam produces: solid on one
	// side of the x midplane, far-air on the other, with nothing in between.
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++)
				sdf[ve::sdf_index(x, y, z)] = ve::encode_sdf(x < 8 ? -0.6f : 0.6f);

	CHECK(worst_step(sdf.data()) > ve::kVoxelSize * 4.0f); // the bug, before
	ve::clamp_brick_lattice(sdf.data());
	// One quantisation step of slack: the lattice is uint8 and the clamp works in that
	// space, so an exact <= kVoxelSize would be a test of rounding, not of the algorithm.
	const float slack = 2.0f * ve::kSdfRange / 255.0f;
	CHECK(worst_step(sdf.data()) <= ve::kVoxelSize + slack);
}

TEST_CASE("the clamp never flips a sample's sign") {
	std::vector<uint8_t> before(ve::kBrickSdfCount), after(ve::kBrickSdfCount);
	for (int i = 0; i < ve::kBrickSdfCount; i++)
		before[i] = static_cast<uint8_t>((i * 37) % 256); // arbitrary but reproducible
	after = before;
	ve::clamp_brick_lattice(after.data());
	for (int i = 0; i < ve::kBrickSdfCount; i++) {
		const float a = ve::decode_sdf(before[i]), b = ve::decode_sdf(after[i]);
		CHECK_MESSAGE((a > 0.0f) == (b > 0.0f), "clamp flipped a sign at " << i);
		CHECK(std::fabs(b) <= std::fabs(a) + 1e-6f); // magnitudes only ever shrink
	}
}

TEST_CASE("the clamp is idempotent") {
	std::vector<uint8_t> a(ve::kBrickSdfCount);
	for (int i = 0; i < ve::kBrickSdfCount; i++) a[i] = static_cast<uint8_t>((i * 91) % 256);
	ve::clamp_brick_lattice(a.data());
	std::vector<uint8_t> b = a;
	ve::clamp_brick_lattice(b.data());
	// Converged means converged. If this fails the iteration count is too low, and the GPU
	// mirror in Task 8 will not be able to agree with this side.
	CHECK(a == b);
}

TEST_CASE("the gate is off until two materials of differing hardness meet") {
	ve::EditOp carve{};
	carve.type = ve::kOpSphereSubtract;
	carve.radius = 1.0f;
	const uint16_t one_material[4] = {1, 1, 1, 1};
	const uint16_t two_same[4] = {0, 0, 0, 0};   // air only
	CHECK_FALSE(ve::lattice_needs_clamp(one_material, 4, &carve, 1));
	CHECK_FALSE(ve::lattice_needs_clamp(two_same, 4, &carve, 1));

	// grass (1.0) beside rock (3.0) with a carve present: this is the case that distorts.
	const uint16_t mixed[4] = {1, 2, 1, 2};
	CHECK(ve::lattice_needs_clamp(mixed, 4, &carve, 1));

	// Same materials, no subtract op: nothing distorts the field, so no clamp.
	ve::EditOp paint{};
	paint.type = ve::kOpSpherePaint;
	CHECK_FALSE(ve::lattice_needs_clamp(mixed, 4, &paint, 1));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons -Q test`
Expected: FAIL — compile error, `ve::clamp_brick_lattice` and `ve::lattice_needs_clamp` are not declared.

- [ ] **Step 3: Declare the interface**

Two headers, because `world/brick.h` sits at the bottom of the include graph by design (see
its own `kActivationPad` comment) and must not pull in `generator/edit_ops.h`. The clamp
itself needs nothing but `<cstdint>` and goes in `brick.h`; the gate takes an `EditOp` and
gets its own header.

Append to `extension/src/world/brick.h`, inside `namespace ve`, after `set_mat_index`:

```cpp
// Per-point hardness makes a carve's radius vary with the material at the sample point.
// That keeps the field's SIGN exactly right, so meshing, occupancy and collision are
// unaffected -- but it destroys the MAGNITUDE as a distance bound at a material seam: the
// field reports the distance to the soft material's crater wall while the barely-carved
// hard lip stands much closer, and shaders/raymarch.comp.glsl steps t += max(d * 0.9, ...)
// straight through it.
//
// clamp_brick_lattice restores the property the tracer needs: after it, no two adjacent
// lattice samples differ by more than one voxel pitch. It only ever shrinks magnitudes and
// never flips a sign, so occupancy classification (which compares against encoded zero) is
// untouched. It works in the ENCODED uint8 space because shaders/brick_gen.comp.glsl
// mirrors it against an r8 image, and the two must agree byte for byte.
//
// The relaxation is monotone min-plus, so its fixed point is unique and independent of the
// order samples are visited. That is what lets a 256-thread workgroup reproduce this
// single-threaded loop exactly.
void clamp_brick_lattice(uint8_t *sdf);

// Iterations to convergence. A magnitude can travel at most kSdfRange before it saturates,
// in steps of kVoxelSize, so ceil(0.64 / 0.05) = 13 passes suffice from any input.
inline constexpr int kClampIterations = 13;
```

Create `extension/src/world/brick_clamp.h`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include <cstdint>

namespace ve {

// The gate for clamp_brick_lattice (declared in world/brick.h). The distortion needs BOTH a
// carve to distort and two materials whose hardness differs to distort it. Neither holds for
// unedited terrain, and neither holds anywhere until a material with hardness above 1.0 is
// in play -- so the clamp lands inert and is switched on by the hardness task.
//
// `mat` is the brick's per-cell material array as eval_brick builds it, BEFORE the palette
// exists; `count` is kBrickVoxelCount.
bool lattice_needs_clamp(const uint16_t *mat, int count, const EditOp *ops, int op_count);

} // namespace ve
```

Add `#include "world/brick_clamp.h"` to `extension/tests/test_brick_clamp.cpp`.

- [ ] **Step 4: Implement**

Create `extension/src/world/brick_clamp.cpp`:

```cpp
#include "world/brick.h"
#include "world/brick_clamp.h"
#include "world/material_table.h"
#include <cmath>

namespace ve {

namespace {

// |d| may not exceed |neighbour| + one voxel pitch. Sign is preserved because the bound is
// always at least kVoxelSize > 0, so a sample can be pulled toward zero but never across it.
uint8_t bound(uint8_t self, uint8_t neighbour) {
	const float d = decode_sdf(self);
	const float limit = std::fabs(decode_sdf(neighbour)) + kVoxelSize;
	if (d > 0.0f) return d <= limit ? self : encode_sdf(limit);
	return -d <= limit ? self : encode_sdf(-limit);
}

} // namespace

void clamp_brick_lattice(uint8_t *sdf) {
	const int s = kBrickSdfStride;
	for (int iter = 0; iter < kClampIterations; iter++) {
		for (int z = 0; z < s; z++)
			for (int y = 0; y < s; y++)
				for (int x = 0; x < s; x++) {
					const int i = sdf_index(x, y, z);
					uint8_t v = sdf[i];
					if (x > 0)     v = bound(v, sdf[sdf_index(x - 1, y, z)]);
					if (x + 1 < s) v = bound(v, sdf[sdf_index(x + 1, y, z)]);
					if (y > 0)     v = bound(v, sdf[sdf_index(x, y - 1, z)]);
					if (y + 1 < s) v = bound(v, sdf[sdf_index(x, y + 1, z)]);
					if (z > 0)     v = bound(v, sdf[sdf_index(x, y, z - 1)]);
					if (z + 1 < s) v = bound(v, sdf[sdf_index(x, y, z + 1)]);
					sdf[i] = v;
				}
	}
}

bool lattice_needs_clamp(const uint16_t *mat, int count, const EditOp *ops, int op_count) {
	bool has_subtract = false;
	for (int i = 0; i < op_count && !has_subtract; i++)
		has_subtract = ops[i].type == kOpSphereSubtract;
	if (!has_subtract) return false;

	// Two materials whose hardness DIFFERS. Two different materials that carve identically
	// produce no discontinuity, so comparing ids rather than hardness would clamp bricks
	// that never needed it.
	bool seen = false;
	float first = 0.0f;
	for (int i = 0; i < count; i++) {
		if (mat[i] == 0) continue;
		const float h = material_hardness(mat[i]);
		if (!seen) { first = h; seen = true; continue; }
		if (h != first) return true;
	}
	return false;
}

} // namespace ve
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd extension && scons -Q test`
Expected: PASS — all four `test_brick_clamp.cpp` cases.

- [ ] **Step 6: Confirm nothing else moved**

Run: `./build.sh --test && ./gdunit_tests.sh -a res://tests/test_brick_diff.gd -a res://tests/test_edit_pipeline.gd`
Expected: PASS, unchanged. Nothing calls `clamp_brick_lattice` yet — this task adds the
function and its tests, nothing more.

**Do not wire it into `eval_brick` here.** The CPU bake and `shaders/brick_gen.comp.glsl`
are diffed byte for byte by `tests/test_brick_diff.gd`; clamping on one side only would
leave that suite red until the next commit. Task 8 lands both sides together.

- [ ] **Step 7: Commit**

```bash
git add extension/src/world/brick.h extension/src/world/brick_clamp.h \
        extension/src/world/brick_clamp.cpp extension/src/world/brick_eval.cpp \
        extension/tests/test_brick_clamp.cpp
git commit -m "feat: Eikonal clamp on the CPU brick lattice, gated on hardness seams"
```

---

### Task 8: Wire the clamp into both bakes

**Files:**
- Modify: `extension/src/world/brick_eval.cpp:161-215` (`eval_brick`)
- Modify: `shaders/brick_gen.comp.glsl` (between Phase 1b and Phase 2)
- Modify: `shaders/common.glslh` (shared clamp constant)
- Test: `tests/test_brick_diff.gd`, `tests/test_field_diff.gd`

**Interfaces:**
- Consumes: `ve::clamp_brick_lattice`, `ve::lattice_needs_clamp` from Task 7; `mat_hardness` from Task 2.
- Produces: both bakes write the same clamped lattice bytes for every brick.

**Both sides land in one commit.** `tests/test_brick_diff.gd` diffs the CPU lattice against
the GPU's byte for byte, so clamping on one side alone leaves the suite red. That is why
Task 7 stopped short of calling the function.

- [ ] **Step 1: Wire the CPU bake**

In `extension/src/world/brick_eval.cpp`, add `#include "world/brick_clamp.h"` to the includes. In `eval_brick`, insert immediately after the 17³ lattice loop closes and **before** `spread_materials`:

```cpp
	// Before spread_materials, which reads b.sdf to project air cells onto the surface, and
	// before build_brick_mips, which reduces it. shaders/brick_gen.comp.glsl inserts the
	// mirror of this between its Phase 1b and Phase 2 for exactly the same reason.
	if (lattice_needs_clamp(mat, kBrickVoxelCount, filtered, filtered_count))
		clamp_brick_lattice(b.sdf);

	spread_materials(mat, b, gen, filtered, filtered_count, bo, volumes, overrides);
```

- [ ] **Step 2: Confirm the differential now fails**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_brick_diff.gd`
Expected: FAIL — the CPU clamps and the GPU does not, so the two lattices disagree. This is
the test that gates the rest of the task.

- [ ] **Step 3: Add the shared constant**

In `shaders/common.glslh`, add next to the other lattice constants (after `BRICK_SDF_MAX`):

```glsl
// ve::kClampIterations. A magnitude travels at most SDF_RANGE before saturating, in steps
// of VOXEL_SIZE, so ceil(0.64 / 0.05) = 13 passes converge from any input.
const int CLAMP_ITERATIONS = 13;
```

- [ ] **Step 4: Write the GPU mirror**

In `shaders/brick_gen.comp.glsl`, add these helpers above `main()`, after `slope_axis`:

```glsl
// Mirror of ve::bound in extension/src/world/brick_clamp.cpp. Works on the ENCODED byte
// so both sides quantise identically -- the differential test compares stored bytes.
uint clamp_bound(uint self, uint neighbour) {
	float d = decode_sdf(float(self) / 255.0);
	float limit = abs(decode_sdf(float(neighbour) / 255.0)) + VOXEL_SIZE;
	if (d > 0.0) return d <= limit ? self : encode_sdf_byte(limit);
	return -d <= limit ? self : encode_sdf_byte(-limit);
}

uint lat_byte(ivec3 base, ivec3 v) {
	return uint(imageLoad(sdf_atlas, base + v).r * 255.0 + 0.5);
}

// Mirror of ve::lattice_needs_clamp. Both halves must hold: a carve to distort the field,
// and two materials whose hardness differs to distort it with.
bool needs_clamp(uint op_base, uint op_n) {
	bool has_subtract = false;
	for (uint i = 0u; i < op_n && !has_subtract; i++)
		has_subtract = field_op_pool.v[FIELD_OP_INDEX(op_base, i) * 2u].x == OP_SPHERE_SUBTRACT;
	if (!has_subtract) return false;
	bool seen = false;
	float first = 0.0;
	for (int i = 0; i < BRICK_VOXEL_COUNT; i++) {
		if (s_mat[i] == 0u) continue;
		float h = mat_hardness(s_mat[i]);
		if (!seen) { first = h; seen = true; continue; }
		if (h != first) return true;
	}
	return false;
}
```

Then insert this block in `main()` immediately after the Phase 1b barrier (`memoryBarrierImage(); memoryBarrierShared(); barrier();`) and before the Phase 2 comment:

```glsl
	// The Eikonal clamp (ve::clamp_brick_lattice). Per-point hardness distorts the carve
	// field's magnitude at a material seam; without this the near-field marcher steps
	// through the barely-carved lip. It runs in place on the atlas, which is why the image
	// is declared coherent, and it must land BEFORE Phase 2 -- that phase reads the lattice
	// to project air cells onto the surface, exactly as ve::spread_materials does on the CPU.
	//
	// The relaxation is monotone min-plus: its fixed point is unique and order-independent,
	// so 256 threads reaching convergence land on the same bytes the CPU's single-threaded
	// loop does. That equivalence is what tests/test_brick_diff.gd checks.
	if (tid == 0u) s_needs_clamp = needs_clamp(op_base, s_op_n) ? 1u : 0u;
	memoryBarrierShared();
	barrier();
	if (s_needs_clamp != 0u) {
		for (int iter = 0; iter < CLAMP_ITERATIONS; iter++) {
			for (uint i = tid; i < uint(BRICK_SDF_COUNT); i += 256u) {
				ivec3 v = ivec3(int(i) % BRICK_SDF_STRIDE,
						(int(i) / BRICK_SDF_STRIDE) % BRICK_SDF_STRIDE,
						int(i) / (BRICK_SDF_STRIDE * BRICK_SDF_STRIDE));
				uint b = lat_byte(sdf_base, v);
				if (v.x > 0) b = clamp_bound(b, lat_byte(sdf_base, v - ivec3(1, 0, 0)));
				if (v.x + 1 < BRICK_SDF_STRIDE) b = clamp_bound(b, lat_byte(sdf_base, v + ivec3(1, 0, 0)));
				if (v.y > 0) b = clamp_bound(b, lat_byte(sdf_base, v - ivec3(0, 1, 0)));
				if (v.y + 1 < BRICK_SDF_STRIDE) b = clamp_bound(b, lat_byte(sdf_base, v + ivec3(0, 1, 0)));
				if (v.z > 0) b = clamp_bound(b, lat_byte(sdf_base, v - ivec3(0, 0, 1)));
				if (v.z + 1 < BRICK_SDF_STRIDE) b = clamp_bound(b, lat_byte(sdf_base, v + ivec3(0, 0, 1)));
				imageStore(sdf_atlas, sdf_base + v, vec4(float(b) / 255.0));
			}
			memoryBarrierImage();
			barrier();
		}
	}
```

and declare the shared flag next to the other `shared` declarations near the top:

```glsl
shared uint s_needs_clamp;
```

- [ ] **Step 5: Run the differential**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_brick_diff.gd -a res://tests/test_field_diff.gd`
Expected: PASS.

**If `test_brick_diff.gd` still fails with small per-sample differences,** the in-place relaxation has not converged identically on both sides. Do not raise `CLAMP_ITERATIONS` blindly — first confirm the fixed point is genuinely order-independent by making the GPU loop single-threaded (`if (tid == 0u)` around the inner `for`, striding all 4913 samples). If that passes, the divergence is a convergence-count problem; if it still fails, it is a quantisation-order problem and `bound()` needs to compare encoded bytes rather than decoded floats on both sides.

- [ ] **Step 6: Verify the rest of the pipeline**

Run: `./gdunit_tests.sh -a res://tests/test_edit_pipeline.gd -a res://tests/test_mesh_diff.gd -a res://tests/test_occupancy.gd -a res://tests/test_lod_mesh_diff.gd -a res://tests/test_raymarch_pixel.gd`
Expected: PASS — this is the set that would notice a changed lattice.

- [ ] **Step 7: Commit**

```bash
git add extension/src/world/brick_eval.cpp shaders/brick_gen.comp.glsl shaders/common.glslh
git commit -m "feat: apply the Eikonal clamp in both brick bakes"
```

---

### Task 9: Apply hardness in the field evaluator

**Files:**
- Modify: `extension/src/generator/edit_ops.cpp` (`apply_op`, `apply_op_gradient`, `kOpSphereSubtract` cases)
- Modify: `shaders/field.glslh` (`apply_field_op`, `apply_field_op_gradient`)
- Test: `extension/tests/test_edit_ops.cpp`, `tests/test_field_diff.gd`

**Interfaces:**
- Consumes: `ve::material_hardness` from Task 1, `mat_hardness` from Task 2, the clamp from Tasks 7–8.
- Produces: a sphere-subtract's effective radius is `op.radius / material_hardness(material at the sample point)`. Nothing downstream changes shape; `op.radius` still bounds the op's reach.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_edit_ops.cpp`:

```cpp
#include "world/material_table.h"

namespace {

// The field at a point, given only the material sitting there. This is the unit under
// test: apply_op's carve must consult the sample's own material.
float carve_depth(uint16_t material, float distance_from_centre, float radius) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.radius = radius;
	op.pos[0] = op.pos[1] = op.pos[2] = 0.0f;
	ve::Sample s{-1.0f, material}; // deep solid, so the carve is the winning term
	return ve::apply_op(s, op, distance_from_centre, 0.0f, 0.0f).sdf;
}

} // namespace

TEST_CASE("a harder material carves less than a softer one") {
	// grass_01 is hardness 1.0 and rock is 3.0, so at 0.5 m from a 1 m carve's centre the
	// grass is air (0.5 < 1.0) and the rock is still solid (0.5 > 1.0/3.0).
	CHECK(carve_depth(1, 0.5f, 1.0f) > 0.0f);
	CHECK(carve_depth(2, 0.5f, 1.0f) < 0.0f);
}

TEST_CASE("hardness 1.0 reproduces the unhardened carve exactly") {
	REQUIRE(ve::material_hardness(1) == doctest::Approx(1.0f));
	// A point 0.25 m inside a 1 m carve reads +0.75: the full-radius result.
	CHECK(carve_depth(1, 0.25f, 1.0f) == doctest::Approx(0.75f));
}

TEST_CASE("air carves at full radius") {
	// Material 0 has no hardness entry. Carving air changes nothing visible, but the
	// evaluator must not divide by a garbage value on the way there.
	CHECK(carve_depth(0, 0.25f, 1.0f) == doctest::Approx(0.75f));
}

TEST_CASE("a carve never reaches past its own AABB") {
	// This is the invariant the hardness >= 1.0 floor exists to protect. op_world_aabb
	// reports pos +/- radius, and an op reaching outside it is dropped at region borders.
	for (int i = 0; i < ve::kMaterialCount; i++) {
		const uint16_t id = static_cast<uint16_t>(i + 1);
		// Just outside the declared radius: must still be solid for EVERY material.
		CHECK_MESSAGE(carve_depth(id, 1.001f, 1.0f) < 0.0f,
				ve::kMaterials[i].name << " carves past op.radius");
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons -Q test`
Expected: FAIL — `a harder material carves less than a softer one` fails; both materials currently carve identically.

- [ ] **Step 3: Apply hardness on the CPU**

In `extension/src/generator/edit_ops.cpp`, add `#include "world/material_table.h"` to the includes, and change `sphere_sdf` to take the hardness into account through a new helper placed beside it:

```cpp
// The carve's effective radius at a sample. Hardness only ever SHRINKS it: op.radius is the
// op's declared maximum reach and op_world_aabb reports pos +/- op.radius, so an effective
// radius above it would put field influence outside the AABB, where the region filter drops
// it. ve::material_hardness is floored at 1.0 and static_asserted for exactly this reason.
// Mirrored in shaders/field.glslh.
float hardened_radius(const EditOp &op, uint16_t material) {
	return op.radius / material_hardness(material);
}
```

In `apply_op`, replace the `kOpSphereSubtract` case:

```cpp
		case kOpSphereSubtract: {
			// CSG subtract: max(s, -sphere). A point that becomes air carries no material,
			// matching the generator's own convention (Sample::material == 0 above ground).
			const float sp = sphere_sdf(op, x, y, z);
```

with:

```cpp
		case kOpSphereSubtract: {
			// CSG subtract: max(s, -sphere). A point that becomes air carries no material,
			// matching the generator's own convention (Sample::material == 0 above ground).
			// The sphere's radius is divided by the hardness of the material AT THIS POINT,
			// so one blast eats deep into dirt and barely scratches an adjacent rock seam.
			// That keeps the field's sign exact but destroys its magnitude as a distance
			// bound at the seam -- see ve::clamp_brick_lattice, which repairs it at bake.
			const float dx = x - op.pos[0], dy = y - op.pos[1], dz = z - op.pos[2];
			const float sp = std::sqrt(dx * dx + dy * dy + dz * dz) -
					hardened_radius(op, s.material);
```

Apply the identical substitution in `apply_op_gradient`'s `kOpSphereSubtract` case (the gradient itself is unchanged — it is the normalised direction from the centre, which does not depend on the radius).

- [ ] **Step 4: Run the native test**

Run: `cd extension && scons -Q test`
Expected: PASS — all four new cases.

- [ ] **Step 5: Mirror into GLSL**

In `shaders/field.glslh`, in `apply_field_op`, replace:

```glsl
	float sp = length(p - c) - radius;
	if (type == OP_SPHERE_SUBTRACT) {
		if (-sp > sdf) { sdf = -sp; if (sdf > 0.0) mat = 0u; }
	} else if (type == OP_SPHERE_ADD) {
```

with:

```glsl
	// Mirror of ve::apply_op. Only the SUBTRACT branch is hardness-scaled: hardness is a
	// resistance to being carved, not a property of material being added or painted.
	// ve::hardened_radius divides by mat_hardness, which is floored at 1.0 so the effective
	// radius can never exceed op.radius and escape op_world_aabb.
	float sp = length(p - c) - radius;
	if (type == OP_SPHERE_SUBTRACT) {
		sp = length(p - c) - radius / mat_hardness(mat);
		if (-sp > sdf) { sdf = -sp; if (sdf > 0.0) mat = 0u; }
	} else if (type == OP_SPHERE_ADD) {
```

Apply the same substitution in `apply_field_op_gradient`'s `OP_SPHERE_SUBTRACT` branch, recomputing `sp` before the `if (-sp > sdf)` test there too.

- [ ] **Step 6: Add an explicit differential case**

`test_field_diff.gd`'s existing `test_sphere_subtract_matches` already carves terrain whose
base materials span grass (1.0), rock (3.0) and dirt (1.4), so it exercises the hardness
mirror by accident. Accidental coverage is not a gate — a later change to the generator's
material bands would silently remove it. Add an explicit case after
`test_sphere_paint_matches`:

```gdscript
func test_hardness_scaled_subtract_matches() -> void:
	# The hardness mirror. ve::hardened_radius divides a carve's radius by the material at
	# the sample point and field.glslh's mat_hardness must agree exactly. The paint lays a
	# slab of rock (hardness 3.0) across part of the sampled volume first, so the carve that
	# follows spans two hardnesses -- a mirror that ignored hardness on either side, or that
	# read a different table, shows up here as an sdf disagreement.
	var ops := make_op(OP_PAINT, 2, Vector3(6.0, 51.2, 6.0), 8.0)
	ops.append_array(make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 6.0))
	compare(sample_points(), ops, 2, "hardness")
```

- [ ] **Step 7: Run the differential**

Run: `./build.sh && ./gdunit_tests.sh -a res://tests/test_field_diff.gd -a res://tests/test_brick_diff.gd -a res://tests/test_field_volume_diff.gd -a res://tests/test_field_gradient.gd`
Expected: PASS, including the new `hardness` case.

- [ ] **Step 8: Verify the wider pipeline**

Run: `./gdunit_tests.sh -a res://tests/test_edit_pipeline.gd -a res://tests/test_collider_edits.gd -a res://tests/test_island_extract.gd -a res://tests/test_connectivity.gd`
Expected: PASS — carving now removes less from hard materials, so islands break loose in different places, but every invariant these pin (op reaches its regions, colliders follow edits, components stay consistent) must hold.

- [ ] **Step 9: Commit**

```bash
git add extension/src/generator/edit_ops.cpp shaders/field.glslh \
        extension/tests/test_edit_ops.cpp tests/test_field_diff.gd
git commit -m "feat: scale a carve's radius by the material's hardness"
```

---

### Task 10: Seam-leak regression test

**Files:**
- Create: `tests/test_material_seam.gd`

**Interfaces:**
- Consumes: hardness from Task 9, the clamp from Tasks 7–8.
- Produces: nothing. This is the test that proves the clamp does its job, and the one that fails first if it is ever removed.

- [ ] **Step 1: Write the test**

Create `tests/test_material_seam.gd`:

```gdscript
extends GdUnitTestSuite

# The artifact this file exists to prevent: per-point hardness makes the carve field
# overestimate free space beside a hard-material seam, and the near-field marcher --
# t += max(d * 0.9, 0.005) -- steps straight through the barely-carved lip. The Eikonal
# clamp on the brick lattice (ve::clamp_brick_lattice) is what stops it. If someone removes
# or mis-gates the clamp, this is the test that goes red.

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
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func settle(w: VoxelWorld) -> void:
	var quiet := 0
	for i in range(200):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break

# Two hardnesses meeting inside one crater is the exact geometry that distorts the field.
func build_seam(w: VoxelWorld) -> Vector3:
	var centre := Vector3(20.0, 54.0, 20.0)
	# A slab of the hardest material, then a slab of the softest, sharing a plane.
	var hard := 0
	var soft := 0
	var hard_h := 0.0
	var soft_h := 999.0
	for m in w.material_table():
		var h := float(m["hardness"])
		if h > hard_h:
			hard_h = h
			hard = int(m["id"])
		if h < soft_h:
			soft_h = h
			soft = int(m["id"])
	assert_float(hard_h).override_failure_message(
		"every material has the same hardness: this test cannot build a seam"
		).is_greater(soft_h)
	w.hooks().debug_apply_sphere_paint(centre - Vector3(2.0, 0, 0), 4.0, hard)
	w.hooks().debug_apply_sphere_paint(centre + Vector3(2.0, 0, 0), 4.0, soft)
	settle(w)
	# One carve spanning both: it eats deep into the soft side and barely into the hard.
	w.hooks().debug_apply_sphere_subtract(centre, 3.0)
	settle(w)
	return centre

# The marcher and the analytic raycast walk the same world by different means. A ray that
# leaks through the seam lip reports a hit far behind where the field says the surface is.
func test_rays_do_not_leak_through_the_seam_lip() -> void:
	var w := make_world()
	var centre := build_seam(w)
	var leaks := 0
	var tested := 0
	for i in range(24):
		var t := float(i) / 24.0
		# Sweep across the seam plane, aiming down into the crater from above.
		var origin := centre + Vector3(lerpf(-3.0, 3.0, t), 8.0, 0.0)
		var dir := Vector3(0, -1, 0)
		var hit: Dictionary = w.hooks().debug_raycast(origin, dir)
		if not hit["hit"]:
			continue
		tested += 1
		var marched: Color = w.hooks().debug_raymarch_pixel(origin, dir)
		if marched.a <= 0.0:
			leaks += 1 # the marcher saw sky where the field says there is surface
	assert_int(tested).override_failure_message(
		"no ray in the sweep hit the terrain; the fixture did not build").is_greater(8)
	assert_int(leaks).override_failure_message(
		"%d of %d rays across the hardness seam missed a surface the field reports: the "
		"Eikonal clamp is not repairing the carve field" % [leaks, tested]).is_equal(0)

# The other half: hardness must actually be doing something, or the test above passes
# trivially on a world where nothing distorted the field in the first place.
func test_the_seam_carve_is_actually_asymmetric() -> void:
	var w := make_world()
	var centre := build_seam(w)
	# 1.2 m either side of the carve centre: inside the soft crater, outside the hard one.
	var soft_state := w.hooks().debug_cell_state(Vector3i(
		int((centre.x + 1.2) / 0.8), int(centre.y / 0.8), int(centre.z / 0.8)))
	var hard_state := w.hooks().debug_cell_state(Vector3i(
		int((centre.x - 1.2) / 0.8), int(centre.y / 0.8), int(centre.z / 0.8)))
	assert_int(soft_state).override_failure_message(
		"the soft side of the seam did not carve: hardness is not being applied"
		).is_not_equal(hard_state)
```

- [ ] **Step 2: Run the test**

Run: `./gdunit_tests.sh -a res://tests/test_material_seam.gd`
Expected: PASS — both cases.

**If `test_rays_do_not_leak_through_the_seam_lip` fails,** the clamp is not running on the bricks that need it. Check `lattice_needs_clamp`'s gate first: it needs a `kOpSphereSubtract` in the brick's *filtered* op list and two materials of differing hardness in the brick's cells. A brick holding only the hard slab sees one hardness and is correctly skipped; the leak happens in bricks straddling the plane, which must see both.

- [ ] **Step 3: Commit**

```bash
git add tests/test_material_seam.gd
git commit -m "test: pin that carve seams do not leak the near-field marcher"
```

---

### Task 11: The material picker scene

**Files:**
- Create: `demo/material_picker.gd`
- Create: `demo/material_picker.tscn`
- Test: `tests/test_material_picker.gd`

**Interfaces:**
- Consumes: `VoxelWorld.material_table()` from Task 3.
- Produces: a `Control` scene with `world_path: NodePath`, `tool_path: NodePath` and `toggle_key: int` exports; methods `select(index: int) -> void`, `selected_id() -> int`, `open() -> void`, `close() -> void`, `is_open() -> bool`. On selection it writes both `fill_material` and `paint_material` on the node at `tool_path`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_material_picker.gd`:

```gdscript
extends GdUnitTestSuite

var _roots: Array = []

func after_test() -> void:
	for r in _roots:
		if is_instance_valid(r):
			r.free()
	_roots.clear()

# A stand-in for demo/edit_tool.gd: the picker only needs the two properties it writes, and
# a real EditTool would demand a VoxelWorld and a Camera3D for no benefit here.
class FakeTool:
	extends Node
	var fill_material := 4
	var paint_material := 1

func make_picker() -> Control:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = true
	world.physics_enabled = false
	var root := Node.new()
	root.add_child(world)
	world.name = "VoxelWorld"
	var tool := FakeTool.new()
	tool.name = "EditTool"
	root.add_child(tool)
	var picker: Control = load("res://demo/material_picker.tscn").instantiate()
	picker.world_path = NodePath("../VoxelWorld")
	picker.tool_path = NodePath("../EditTool")
	root.add_child(picker)
	add_child(root)
	_roots.append(root)
	return picker

func test_it_lists_every_material() -> void:
	var p := make_picker()
	var world: VoxelWorld = p.get_node(p.world_path)
	assert_int(p.entry_count()).is_equal(world.material_table().size())

# The failure this guards is silent and expensive: writing the ARRAY INDEX where the engine
# expects a material ID paints every material one slot off, with no error anywhere.
func test_selecting_writes_the_material_id_not_the_index() -> void:
	var p := make_picker()
	var world: VoxelWorld = p.get_node(p.world_path)
	var tool = p.get_node(p.tool_path)
	p.select(1)
	var expected := int(world.material_table()[1]["id"])
	assert_int(p.selected_id()).is_equal(expected)
	assert_int(tool.fill_material).is_equal(expected)
	assert_int(tool.paint_material).is_equal(expected)

func test_it_starts_closed_and_toggles() -> void:
	var p := make_picker()
	assert_bool(p.is_open()).is_false()
	p.open()
	assert_bool(p.is_open()).is_true()
	p.close()
	assert_bool(p.is_open()).is_false()

# The demo runs with the mouse captured. A panel that released capture and never restored it
# would leave the player unable to look around after choosing a material.
func test_closing_restores_the_previous_mouse_mode() -> void:
	var p := make_picker()
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	p.open()
	assert_int(Input.mouse_mode).is_equal(Input.MOUSE_MODE_VISIBLE)
	p.close()
	assert_int(Input.mouse_mode).is_equal(Input.MOUSE_MODE_CAPTURED)

func test_an_out_of_range_selection_is_ignored() -> void:
	var p := make_picker()
	p.select(0)
	var before := p.selected_id()
	p.select(9999)
	p.select(-1)
	assert_int(p.selected_id()).is_equal(before)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_material_picker.gd`
Expected: FAIL — `res://demo/material_picker.tscn` does not exist.

- [ ] **Step 3: Write the script**

Create `demo/material_picker.gd`:

```gdscript
extends Control
# Material picker for the demo shell (M).
#
# The demo's tools (demo/edit_tool.gd) answer HOW terrain changes -- carve, fill, paint,
# drill, on keys 1-4. This answers WITH WHAT. It deliberately does not touch tool selection
# or the radius wheel; those bindings stay exactly where they were.
#
# It shows each material's hardness and glow next to its swatch because those numbers are
# the material system: a picker that showed only names would demonstrate nothing that a
# hardcoded fill_material export did not already do.

@export var world_path: NodePath
@export var tool_path: NodePath
@export var toggle_key := KEY_M

var _world: VoxelWorld
var _tool: Node
var _materials: Array = []
var _buttons: Array[Button] = []
var _selected := 0
var _mouse_mode_before := Input.MOUSE_MODE_CAPTURED

@onready var _grid: GridContainer = %Grid
@onready var _detail: Label = %Detail

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	visible = false
	_world = get_node_or_null(world_path) as VoxelWorld
	_tool = get_node_or_null(tool_path)
	if _world:
		_materials = _world.material_table()
	_populate()
	select(0)

func entry_count() -> int:
	return _materials.size()

func selected_id() -> int:
	if _selected < 0 or _selected >= _materials.size():
		return 0
	return int(_materials[_selected]["id"])

func is_open() -> bool:
	return visible

# The swatch. assets/materials/ carries a .gdignore so Godot never imports these PNGs --
# load() would return null. Reading the file straight off disk sidesteps the import
# pipeline entirely rather than pulling twenty textures into it for four swatches.
func _swatch(asset: String) -> ImageTexture:
	var path := ProjectSettings.globalize_path(
		"res://assets/materials/%s_basecolor.png" % asset)
	var img := Image.load_from_file(path)
	if img == null:
		return null
	img.resize(64, 64, Image.INTERPOLATE_BILINEAR)
	return ImageTexture.create_from_image(img)

func _populate() -> void:
	for b in _buttons:
		b.queue_free()
	_buttons.clear()
	for i in range(_materials.size()):
		var m: Dictionary = _materials[i]
		var b := Button.new()
		b.custom_minimum_size = Vector2(96, 112)
		b.expand_icon = true
		b.icon = _swatch(str(m["asset"]))
		b.text = str(m["name"])
		b.toggle_mode = true
		b.pressed.connect(select.bind(i))
		_grid.add_child(b)
		_buttons.append(b)

func select(index: int) -> void:
	if index < 0 or index >= _materials.size():
		return
	_selected = index
	for i in range(_buttons.size()):
		_buttons[i].button_pressed = i == index
	var m: Dictionary = _materials[index]
	_detail.text = "%s   hardness %.2f   glow %.2f" % [
		m["name"], float(m["hardness"]), float(m["glow"])]
	# Both, deliberately: Fill and Paint should never disagree about what the picker says
	# is selected. The engine wants the material ID, which is the array index plus one --
	# writing the index here would silently place the wrong material.
	if _tool:
		_tool.fill_material = int(m["id"])
		_tool.paint_material = int(m["id"])

func open() -> void:
	if visible:
		return
	_mouse_mode_before = Input.mouse_mode
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	visible = true

func close() -> void:
	if not visible:
		return
	visible = false
	Input.mouse_mode = _mouse_mode_before

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if event.keycode == toggle_key:
		close() if visible else open()
		get_viewport().set_input_as_handled()
	elif event.keycode == KEY_ESCAPE and visible:
		close()
		get_viewport().set_input_as_handled()
```

- [ ] **Step 4: Write the scene**

Create `demo/material_picker.tscn`:

```
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://demo/material_picker.gd" id="1"]

[node name="MaterialPicker" type="Control"]
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
text = "Material  (M)"

[node name="Grid" type="GridContainer" parent="Center/Panel/Margin/Box"]
unique_name_in_owner = true
layout_mode = 2
columns = 5
theme_override_constants/h_separation = 12
theme_override_constants/v_separation = 12

[node name="Detail" type="Label" parent="Center/Panel/Margin/Box"]
unique_name_in_owner = true
layout_mode = 2
text = ""
```

- [ ] **Step 5: Run the test**

Run: `./gdunit_tests.sh -a res://tests/test_material_picker.gd`
Expected: PASS — all five cases.

- [ ] **Step 6: Commit**

```bash
git add demo/material_picker.gd demo/material_picker.tscn tests/test_material_picker.gd
git commit -m "feat: material picker scene for the demo"
```

---

### Task 12: Wire the picker into the demo

**Files:**
- Modify: `demo/main.tscn` (ext_resource list, HUD children)
- Modify: `demo/help.gd` (the key list)
- Test: `tests/test_demo_shell.gd`

**Interfaces:**
- Consumes: the picker scene from Task 11.
- Produces: nothing other code reads.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_demo_shell.gd`:

```gdscript
func test_the_demo_has_a_material_picker_wired_to_the_edit_tool() -> void:
	var root: Node = load("res://demo/main.tscn").instantiate()
	var picker: Control = root.get_node_or_null("HUD/MaterialPicker")
	assert_object(picker).override_failure_message(
		"main.tscn has no HUD/MaterialPicker").is_not_null()
	assert_bool(picker.tool_path.is_empty()).override_failure_message(
		"the picker has no tool_path: selecting a material would change nothing"
		).is_false()
	assert_object(picker.get_node_or_null(picker.tool_path)).override_failure_message(
		"the picker's tool_path does not resolve").is_not_null()
	root.free()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./gdunit_tests.sh -a res://tests/test_demo_shell.gd`
Expected: FAIL — `main.tscn has no HUD/MaterialPicker`.

- [ ] **Step 3: Instance it**

In `demo/main.tscn`, bump the `load_steps` on line 1 from `18` to `19`, add to the ext_resource block:

```
[ext_resource type="PackedScene" path="res://demo/material_picker.tscn" id="11"]
```

and add after the `SettingsMenu` node:

```
[node name="MaterialPicker" parent="HUD" instance=ExtResource("11")]
world_path = NodePath("/root/Main/VoxelWorld")
tool_path = NodePath("/root/Main/EditTool")
```

- [ ] **Step 4: Document the key**

`demo/help.gd` builds its overlay from a single `CONTROLS` table so the text cannot drift
from the real bindings. Add one row to it, immediately after the `Drill (shortcut)` row so
the editing keys stay together:

```gdscript
	["Choose material", "M"],
```

The table's rows are `["Label", "Keys"]` and `help_text()` formats them with `%-20s %s`;
no other change is needed.

- [ ] **Step 5: Run the tests**

Run: `./gdunit_tests.sh -a res://tests/test_demo_shell.gd -a res://tests/test_help.gd`
Expected: PASS. If `tests/test_help.gd` does not exist, skip it — `test_demo_shell.gd` alone is the gate.

- [ ] **Step 6: Full suite**

Run: `./build.sh --test && ./gdunit_tests.sh`
Expected: PASS — the entire suite. This is the last task; nothing may be left red.

- [ ] **Step 7: Commit**

```bash
git add demo/main.tscn demo/help.gd tests/test_demo_shell.gd
git commit -m "feat: wire the material picker into the demo shell"
```
