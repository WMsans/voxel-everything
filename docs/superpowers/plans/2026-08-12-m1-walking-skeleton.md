# M1 Walking Skeleton — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Godot scene where you fly a camera over smooth raymarched SDF terrain (hills + one cave), rendered by our own RenderingDevice compute pass from a godot_cpp extension — with native unit tests for all pure C++ cores and gdUnit4 smoke tests for the GPU path.

**Architecture:** Approach A from the spec (sparse brick atlas + full-screen compute raymarch). M1 deliberately fixes scope: small bounded world (48×16×48m), bricks generated **on CPU once at load** (GPU brick-eval arrives in M2 with edits), dense single-level brick indirection (region level arrives with streaming in M2), no min–max mips yet (M2 perf), no physics/LoD/beautification. Everything structural — brick format, palette, generator interface, atlas upload, raymarch pass, compositor wiring — matches the spec so later milestones extend rather than rewrite.

**Tech Stack:** Godot 4.7.1 (`/usr/bin/godot`), godot-cpp (branch `4.7`), SCons, C++20, GLSL 460 (Vulkan), doctest 2.4.11 (native), gdUnit4 6.2.1 (in-engine).

**Spec:** `docs/superpowers/specs/2026-08-12-voxel-engine-design.md` (M2–M7 milestone plans will cover the remaining sections).

## Milestone Map

| Milestone | Delivers |
|---|---|
| **M1 (this plan)** | Toolchain + raymarched terrain on screen + test harnesses |
| M2 | GPU brick generation, destruction edits, streaming/residency, min–max mips |
| M3 | Physics: dual-contour meshing, collider streaming, character |
| M4 | Connectivity + raymarched islands |
| M5 | LoD hierarchy + bakery + depth-injection compositing |
| M6 | Beautification: cel, 3-layer shadows, SSGI, SSR, outlines |
| M7 | Benchmark scene + demo polish |

## Global Constraints

- Godot **4.7.1**, godot-cpp branch **`4.7`** — exact.
- Pure C++ cores in `namespace ve` contain **zero Godot types** (spec §8) — no exceptions. (Godot-glue classes live in `namespace godot`.)
- Shaders: GLSL `#version 460`, loaded **from files** via `ve::load_shader_source` — never inline strings.
- Error policy (spec §8): dev = verbose/validation, release = fail-soft (keep last resource, retry next frame, never crash).
- Commit style: conventional (`feat:`, `test:`, `build:`, `fix:`) as in repo history.
- RD API reference: local copy at `docs/api/renderingdevice.md` — consult it before inventing signatures.
- Target hardware: RTX 4070 Laptop; budgets per spec §7 (raymarch ≤6ms, frame ≤16ms — warnings only in M1).

## File Structure

```
extension/
  godot-cpp/                     # submodule, branch 4.7 (Task 1)
  SConstruct                     # extension lib + native test binary (Task 1)
  third_party/doctest/doctest.h  # pinned v2.4.11 (Task 2)
  src/
    register_types.h/.cpp        # GDExtension entry (Task 1)
    voxel_world.h/.cpp           # Node3D, Godot-facing owner of everything (Task 1 stub, 9/10 full)
    raymarch_compositor.h/.cpp   # CompositorEffect driving the passes (Task 11)
    world/brick.h/.cpp           # brick layout, SDF codec, 2-bit materials (Task 3/4)
    world/palette.h/.cpp         # per-brick material palette (Task 4)
    world/world_data.h/.cpp      # brick store + dense indirection + generation (Task 6)
    generator/generator.h/.cpp   # Generator interface + AnalyticGenerator (Task 5)
    render/camera_params.h/.cpp  # ve::CameraParams ray-basis struct (Task 10)
    render/shader_loader.h/.cpp  # file loader + #include expansion (Task 8)
    render/gpu_world.h/.cpp      # atlas/indirection/palette GPU upload (Task 9)
    render/raymarch_pass.h/.cpp  # compute pass (Task 10)
    render/composite_pass.h/.cpp # fullscreen color+depth blit (Task 11)
  tests/
    test_main.cpp                # doctest main (Task 2)
    test_brick.cpp               # (Task 3)
    test_palette.cpp             # (Task 4)
    test_generator.cpp           # (Task 5)
    test_world_data.cpp          # (Task 6)
    test_shader_loader.cpp       # (Task 8)
    test_camera_params.cpp       # (Task 10)
shaders/
  common.glsl                    # shared constants + SDF decode (Task 10)
  raymarch.comp.glsl             # primary visibility compute (Task 10)
  composite.vert.glsl            # fullscreen triangle (Task 11)
  composite.frag.glsl            # color sample + gl_FragDepth (Task 11)
tests/                           # gdUnit4 (res://tests)
  test_extension_boot.gd         # (Task 1)
  test_gpu_smoke.gd              # (Task 7)
  test_gpu_world.gd              # (Task 9)
  test_raymarch_pixel.gd         # (Task 10)
demo/
  main.tscn                      # demo scene (Task 11)
  fly_camera.gd                  # (Task 12)
  hud.gd                         # (Task 12)
  benchmark.gd                   # (Task 12)
voxel_everything.gdextension     # (Task 1)
```

## Conventions Used Throughout

- **Build extension:** `cd extension && scons -j$(nproc)`
- **Run native tests:** `cd extension && scons test`
- **Run gdUnit tests:** `cd /home/jeremy/Development/Godot/voxel-everything && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
- **Run demo:** `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
- GPU texture layout (Tasks 9–11 rely on this): atlas slot → texel origin
  `slot → (sx, sy, sz) = (slot % 32, (slot / 32) % 16, slot / 512)`, texel origin `= 16 * (sx, sy, sz)`; atlas = 32×16×32 bricks = 512×256×512 voxels.

---

### Task 1: Build scaffold — godot-cpp extension that loads

**Files:**
- Create: `extension/SConstruct`
- Create: `extension/src/register_types.h`, `extension/src/register_types.cpp`
- Create: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp`
- Create: `voxel_everything.gdextension`
- Create: `tests/test_extension_boot.gd`
- Modify: `.gitignore`

**Interfaces:**
- Produces: class `godot::VoxelWorld : public Node3D` (registered as `VoxelWorld`), entry symbol `voxel_everything_init`.

- [ ] **Step 1: Add godot-cpp submodule and write the failing gdUnit test**

```bash
cd /home/jeremy/Development/Godot/voxel-everything
git submodule add -b 4.7 https://github.com/godotengine/godot-cpp.git extension/godot-cpp
git submodule update --init --recursive
```

`tests/test_extension_boot.gd`:

```gdscript
extends GdUnitTestSuite

func test_voxel_world_class_registered() -> void:
	assert_bool(ClassDB.class_exists("VoxelWorld")).is_true()
	assert_bool(ClassDB.can_instantiate("VoxelWorld")).is_true()

func test_voxel_world_instantiates() -> void:
	var node: Node3D = ClassDB.instantiate("VoxelWorld")
	assert_object(node).is_not_null()
	node.free()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_extension_boot.gd`
Expected: FAIL — `VoxelWorld` not a registered class (no extension loads yet).

- [ ] **Step 3: Write the scaffold**

`extension/src/register_types.h`:

```cpp
#pragma once
#include <godot_cpp/godot.hpp>

void voxel_everything_initialize(godot::ModuleInitializationLevel p_level);
void voxel_everything_uninitialize(godot::ModuleInitializationLevel p_level);
```

`extension/src/register_types.cpp`:

```cpp
#include "register_types.h"
#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include "voxel_world.h"

using namespace godot;

void voxel_everything_initialize(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
	GDREGISTER_CLASS(VoxelWorld);
}

void voxel_everything_uninitialize(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
}

extern "C" {
GDExtensionBool GDE_EXPORT voxel_everything_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		const GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(voxel_everything_initialize);
	init_obj.register_terminator(voxel_everything_uninitialize);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
```

`extension/src/voxel_world.h`:

```cpp
#pragma once
#include <godot_cpp/classes/node3d.hpp>

namespace godot {

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

protected:
	static void _bind_methods();

public:
	void _ready() override;
};

} // namespace godot
```

`extension/src/voxel_world.cpp`:

```cpp
#include "voxel_world.h"
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void VoxelWorld::_bind_methods() {}

void VoxelWorld::_ready() {
	UtilityFunctions::print("VoxelWorld: ready (M1 stub)");
}
```

`extension/SConstruct`:

```python
#!/usr/bin/env python
import os

env = SConscript("godot-cpp/SConstruct")
env.Append(CPPPATH=["src/"])
sources = Glob("src/*.cpp") + Glob("src/*/*.cpp")
lib = env.SharedLibrary("bin/voxel_everything{}".format(env["suffix"]), source=sources)
Default(lib)

# --- native test runner: pure C++ cores only, zero godot-cpp ---
test_env = Environment(CXX=os.environ.get("CXX", "g++"))
test_env.Append(CXXFLAGS=["-std=c++20", "-O1", "-g", "-Wall", "-Wextra"])
test_env.Append(CPPPATH=["src", "third_party"])
pure_sources = Glob("src/world/*.cpp") + Glob("src/generator/*.cpp")
for f in ["src/render/shader_loader.cpp", "src/render/camera_params.cpp"]:
    if os.path.exists(f):
        pure_sources.append(f)
tests = test_env.Program("build/tests/ve_tests", Glob("tests/*.cpp") + pure_sources)
test_alias = Alias("test", [tests], tests[0].abspath)
AlwaysBuild(test_alias)
```

(Rationale for `os.path.exists`: `shader_loader.cpp`/`camera_params.cpp` arrive in Tasks 8/10; the filter keeps `scons test` green from Task 2 onward. Only ever run `scons test` — never bare `scons test` before Task 2, there are no test sources yet.)

`voxel_everything.gdextension` (project root):

```
[configuration]
entry_symbol = "voxel_everything_init"
compatibility_minimum = "4.7"
reloadable = true

[libraries]
linux.debug.x86_64 = "res://extension/bin/libvoxel_everything.linux.template_debug.x86_64.so"
linux.release.x86_64 = "res://extension/bin/libvoxel_everything.linux.template_release.x86_64.so"
```

Append to `.gitignore`:

```
extension/bin/
extension/build/
.sconsign.dblite
*.os
```

- [ ] **Step 4: Build**

Run: `cd extension && scons -j$(nproc)` (first build compiles godot-cpp — several minutes)
Expected: `bin/libvoxel_everything.linux.template_debug.x86_64.so` produced, zero errors.

- [ ] **Step 5: Run test to verify it passes**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_extension_boot.gd`
Expected: PASS, 2/2.

- [ ] **Step 6: Commit**

```bash
git add extension/ voxel_everything.gdextension tests/ .gitignore
git commit -m "build: godot-cpp extension scaffold with VoxelWorld stub"
```

---

### Task 2: Native doctest harness

**Files:**
- Create: `extension/third_party/doctest/doctest.h`
- Create: `extension/tests/test_main.cpp`

**Interfaces:**
- Produces: `cd extension && scons test` — builds `build/tests/ve_tests` and runs it; doctest available as `#include <doctest/doctest.h>` with `TEST_CASE`/`CHECK`.

- [ ] **Step 1: Fetch doctest (pinned) and write the harness**

```bash
mkdir -p extension/third_party/doctest
curl -L -o extension/third_party/doctest/doctest.h \
  https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
```

`extension/tests/test_main.cpp`:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("harness works") {
	CHECK(1 + 1 == 2);
}
```

- [ ] **Step 2: Run**

Run: `cd extension && scons test`
Expected: compiles, runs, `1 passed | 0 failed`.

- [ ] **Step 3: Commit**

```bash
git add extension/third_party extension/tests
git commit -m "test: native doctest harness"
```

---

### Task 3: `world/brick` — brick layout + SDF codec

**Files:**
- Create: `extension/src/world/brick.h`, `extension/src/world/brick.cpp`
- Test: `extension/tests/test_brick.cpp`

**Interfaces:**
- Produces (later tasks rely on these exact names):
  - Constants: `ve::kBrickVoxels` (16), `ve::kVoxelSize` (0.05f), `ve::kBrickSize` (0.8f), `ve::kBrickVoxelCount` (4096), `ve::kBrickPaletteSize` (4), `ve::kSdfRange` (0.64f)
  - `uint8_t ve::encode_sdf(float d)` / `float ve::decode_sdf(uint8_t v)`
  - `int ve::voxel_index(int x, int y, int z)` — layout `x + y*16 + z*256`
  - `struct ve::Brick { uint8_t sdf[4096]; uint8_t mat[1024]; uint16_t palette[4]; uint32_t flags; }`
  - `uint8_t ve::get_mat_index(const Brick &b, int idx)` / `void ve::set_mat_index(Brick &b, int idx, uint8_t v)`

- [ ] **Step 1: Write the failing test**

`extension/tests/test_brick.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/brick.h"
#include <cmath>
#include <set>

TEST_CASE("sdf codec roundtrips within quantization step") {
	for (float d : {-0.64f, -0.1f, 0.0f, 0.05f, 0.33f, 0.64f}) {
		float rt = ve::decode_sdf(ve::encode_sdf(d));
		CHECK(std::abs(rt - d) <= (1.28f / 255.0f) + 1e-6f);
	}
}

TEST_CASE("sdf codec clamps out of range") {
	CHECK(ve::encode_sdf(1.0f) == 255);
	CHECK(ve::encode_sdf(-1.0f) == 0);
	CHECK(ve::decode_sdf(0) == doctest::Approx(-0.64f));
	CHECK(ve::decode_sdf(255) == doctest::Approx(0.64f));
}

TEST_CASE("voxel_index is a bijection over 4096 cells") {
	std::set<int> seen;
	for (int z = 0; z < 16; z++)
		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++) {
				int idx = ve::voxel_index(x, y, z);
				CHECK(idx >= 0);
				CHECK(idx < ve::kBrickVoxelCount);
				seen.insert(idx);
			}
	CHECK(seen.size() == ve::kBrickVoxelCount);
}

TEST_CASE("2-bit material index roundtrip") {
	ve::Brick b{};
	for (int i = 0; i < ve::kBrickVoxelCount; i++) ve::set_mat_index(b, i, i % 4);
	for (int i = 0; i < ve::kBrickVoxelCount; i++) CHECK(ve::get_mat_index(b, i) == i % 4);
	CHECK(sizeof(b.mat) == 1024);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/brick.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/world/brick.h`:

```cpp
#pragma once
#include <cstdint>

namespace ve {

inline constexpr int kBrickVoxels = 16;
inline constexpr float kVoxelSize = 0.05f;
inline constexpr float kBrickSize = kBrickVoxels * kVoxelSize; // 0.8m
inline constexpr int kBrickVoxelCount = kBrickVoxels * kBrickVoxels * kBrickVoxels; // 4096
inline constexpr int kBrickPaletteSize = 4;
inline constexpr float kSdfRange = 0.64f; // uint8 maps to [-0.64, +0.64] meters

uint8_t encode_sdf(float d);
float decode_sdf(uint8_t v);

// x + y*16 + z*256; all coords in [0,16)
int voxel_index(int x, int y, int z);

struct Brick {
	uint8_t sdf[kBrickVoxelCount];          // encoded SDF
	uint8_t mat[kBrickVoxelCount / 4];      // 2-bit palette indices, packed
	uint16_t palette[kBrickPaletteSize];    // global material IDs
	uint32_t flags = 0;
};

uint8_t get_mat_index(const Brick &b, int idx);
void set_mat_index(Brick &b, int idx, uint8_t v);

} // namespace ve
```

`extension/src/world/brick.cpp`:

```cpp
#include "world/brick.h"
#include <algorithm>
#include <cmath>

namespace ve {

uint8_t encode_sdf(float d) {
	float t = std::clamp((d + kSdfRange) / (2.0f * kSdfRange), 0.0f, 1.0f);
	return static_cast<uint8_t>(std::lround(t * 255.0f));
}

float decode_sdf(uint8_t v) {
	return (static_cast<float>(v) / 255.0f) * 2.0f * kSdfRange - kSdfRange;
}

int voxel_index(int x, int y, int z) {
	return x + y * kBrickVoxels + z * kBrickVoxels * kBrickVoxels;
}

uint8_t get_mat_index(const Brick &b, int idx) {
	return (b.mat[idx >> 2] >> ((idx & 3) * 2)) & 0x3;
}

void set_mat_index(Brick &b, int idx, uint8_t v) {
	uint8_t &byte = b.mat[idx >> 2];
	const int shift = (idx & 3) * 2;
	byte = static_cast<uint8_t>((byte & ~(0x3 << shift)) | ((v & 0x3) << shift));
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: 4 new test cases pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world extension/tests/test_brick.cpp
git commit -m "feat(world): brick layout and SDF codec"
```

---

### Task 4: `world/palette` — per-brick material palette

**Files:**
- Create: `extension/src/world/palette.h`, `extension/src/world/palette.cpp`
- Test: `extension/tests/test_palette.cpp`

**Interfaces:**
- Consumes: `ve::kBrickPaletteSize` (Task 3).
- Produces: `int ve::palette_slot(uint16_t *palette, uint16_t mat_id, bool *overflow)` — returns slot 0–3 for `mat_id`, inserting it if a slot is free (free slots are `0`); if full, sets `*overflow = true` and returns the slot whose ID is numerically nearest (M1 similarity proxy; spec §2).

- [ ] **Step 1: Write the failing test**

`extension/tests/test_palette.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/palette.h"

TEST_CASE("palette inserts up to 4 materials and reuses slots") {
	uint16_t pal[4] = {0, 0, 0, 0};
	bool ovf = false;
	CHECK(ve::palette_slot(pal, 10, &ovf) == 0);
	CHECK(ve::palette_slot(pal, 20, &ovf) == 1);
	CHECK(ve::palette_slot(pal, 30, &ovf) == 2);
	CHECK(ve::palette_slot(pal, 40, &ovf) == 3);
	CHECK(ve::palette_slot(pal, 20, &ovf) == 1); // existing
	CHECK_FALSE(ovf);
}

TEST_CASE("palette overflow picks nearest existing id") {
	uint16_t pal[4] = {0, 0, 0, 0};
	bool ovf = false;
	ve::palette_slot(pal, 10, &ovf);
	ve::palette_slot(pal, 20, &ovf);
	ve::palette_slot(pal, 30, &ovf);
	ve::palette_slot(pal, 40, &ovf);
	CHECK(ve::palette_slot(pal, 38, &ovf) == 3); // nearest to 40
	CHECK(ovf);
	ovf = false;
	CHECK(ve::palette_slot(pal, 12, &ovf) == 0); // nearest to 10
	CHECK(ovf);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/palette.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/world/palette.h`:

```cpp
#pragma once
#include <cstdint>

namespace ve {

// Returns the slot (0..3) holding mat_id, inserting into a free slot if needed.
// Free slots are marked 0 (material 0 = air never occupies a palette entry).
// If full: sets *overflow=true and returns the slot with the numerically nearest id.
int palette_slot(uint16_t *palette, uint16_t mat_id, bool *overflow);

} // namespace ve
```

`extension/src/world/palette.cpp`:

```cpp
#include "world/palette.h"
#include "world/brick.h"
#include <cstdlib>

namespace ve {

int palette_slot(uint16_t *palette, uint16_t mat_id, bool *overflow) {
	if (overflow) *overflow = false;
	int free_slot = -1;
	for (int i = 0; i < kBrickPaletteSize; i++) {
		if (palette[i] == mat_id) return i;
		if (palette[i] == 0 && free_slot < 0) free_slot = i;
	}
	if (free_slot >= 0) {
		palette[free_slot] = mat_id;
		return free_slot;
	}
	if (overflow) *overflow = true;
	int best = 0;
	int best_dist = std::abs(static_cast<int>(palette[0]) - static_cast<int>(mat_id));
	for (int i = 1; i < kBrickPaletteSize; i++) {
		int dist = std::abs(static_cast<int>(palette[i]) - static_cast<int>(mat_id));
		if (dist < best_dist) { best = i; best_dist = dist; }
	}
	return best;
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world/palette.* extension/tests/test_palette.cpp
git commit -m "feat(world): brick palette and 2-bit material packing"
```

---

### Task 5: `generator` — analytic terrain generator

**Files:**
- Create: `extension/src/generator/generator.h`, `extension/src/generator/generator.cpp`
- Test: `extension/tests/test_generator.cpp`

**Interfaces:**
- Produces:
  - `struct ve::Sample { float sdf; uint16_t material; }` (material 0 = air)
  - `class ve::Generator { public: virtual ~Generator() = default; virtual Sample sample(float x, float y, float z) const = 0; };`
  - `class ve::AnalyticGenerator : public Generator { public: explicit AnalyticGenerator(uint32_t seed = 1337); Sample sample(float, float, float) const override; };`
  - Material IDs: `1 = grass, 2 = rock, 3 = dirt`. Terrain height function `hills(x,z)` is file-static in the implementation; tests mirror it (deliberate duplication — the test oracle must not share code with the implementation).

- [ ] **Step 1: Write the failing test**

`extension/tests/test_generator.cpp`:

```cpp
#include <doctest/doctest.h>
#include "generator/generator.h"
#include <cmath>

static float hills(float x, float z) { // test oracle — mirrors implementation on purpose
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

TEST_CASE("surface point has sdf ~= 0, above is positive, below negative") {
	ve::AnalyticGenerator g;
	float x = 12.3f, z = -7.8f;
	float h = hills(x, z);
	CHECK(g.sample(x, h, z).sdf == doctest::Approx(0.0f).epsilon(0.001));
	CHECK(g.sample(x, h + 0.5f, z).sdf > 0.0f);
	CHECK(g.sample(x, h - 0.5f, z).sdf < 0.0f);
}

TEST_CASE("cave carves empty space inside terrain") {
	ve::AnalyticGenerator g;
	float cy = hills(30.0f, 30.0f) - 2.0f;
	CHECK(g.sample(30.0f, cy, 30.0f).sdf > 0.0f);   // cave center: air, though underground
	CHECK(g.sample(-30.0f, cy, -30.0f).sdf < 0.0f); // far from cave: solid
}

TEST_CASE("materials follow height bands; air has material 0") {
	ve::AnalyticGenerator g;
	CHECK(g.sample(0.0f, 100.0f, 0.0f).material == 0);
	float h = hills(1.0f, 1.0f);
	CHECK(g.sample(1.0f, h - 0.01f, 1.0f).material != 0);
}

TEST_CASE("determinism: same input, identical output bits") {
	ve::AnalyticGenerator a, b;
	auto sa = a.sample(3.21f, 1.5f, -9.4f);
	auto sb = b.sample(3.21f, 1.5f, -9.4f);
	CHECK(sa.sdf == sb.sdf);
	CHECK(sa.material == sb.material);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `generator/generator.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/generator/generator.h`:

```cpp
#pragma once
#include <cstdint>

namespace ve {

struct Sample {
	float sdf;         // meters, negative = solid
	uint16_t material; // 0 = air, 1 = grass, 2 = rock, 3 = dirt
};

class Generator {
public:
	virtual ~Generator() = default;
	virtual Sample sample(float x, float y, float z) const = 0;
};

// Deterministic analytic terrain: sine hills + one carved spherical cave.
// Seed is reserved for future variation; M1 output is seed-independent.
class AnalyticGenerator : public Generator {
public:
	explicit AnalyticGenerator(uint32_t seed = 1337) : seed_(seed) {}
	Sample sample(float x, float y, float z) const override;

private:
	uint32_t seed_;
};

} // namespace ve
```

`extension/src/generator/generator.cpp`:

```cpp
#include "generator/generator.h"
#include <cmath>

namespace ve {

static float hills(float x, float z) {
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

Sample AnalyticGenerator::sample(float x, float y, float z) const {
	const float h = hills(x, z);
	float sdf = y - h;

	// Carved cave: sphere at (30, hills(30,30)-2, 30), radius 5.
	const float cx = 30.0f, cz = 30.0f;
	const float cy = hills(cx, cz) - 2.0f;
	const float dx = x - cx, dy = y - cy, dz = z - cz;
	const float sphere = sqrtf(dx * dx + dy * dy + dz * dz) - 5.0f;
	sdf = fmaxf(sdf, -sphere); // CSG subtract

	uint16_t mat = 0;
	if (sdf <= 0.0f) {
		mat = h > 4.0f ? 2 : (h > 1.0f ? 1 : 3);
	}
	return {sdf, mat};
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/generator extension/tests/test_generator.cpp
git commit -m "feat(generator): analytic terrain generator"
```

---

### Task 6: `world/world_data` — brick store + generation

**Files:**
- Create: `extension/src/world/world_data.h`, `extension/src/world/world_data.cpp`
- Test: `extension/tests/test_world_data.cpp`

**Interfaces:**
- Consumes: `ve::Brick`, `ve::voxel_index`, `ve::encode_sdf`, `ve::set_mat_index`, `ve::palette_slot`, `ve::Generator`/`ve::Sample`.
- Produces:
  - `struct ve::Dims { int x, y, z; };`
  - `class ve::WorldData`:
    - `WorldData(int bx, int by, int bz)` — world extent in bricks; world-space meters start at origin corner (world pos = brick coord × 0.8)
    - `void generate(const Generator &gen)`
    - `bool brick_active(int bx, int by, int bz) const`
    - `int brick_slot(int bx, int by, int bz) const` — −1 if inactive
    - `const Brick &brick(int slot) const`
    - `int active_brick_count() const`
    - `const std::vector<int32_t> &indirection() const` — size `bx*by*bz`, layout `x + y*bx + z*bx*by`
    - `Dims dims() const`

- [ ] **Step 1: Write the failing test**

`extension/tests/test_world_data.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/world_data.h"
#include "generator/generator.h"
#include <cmath>

static float hills(float x, float z) { // test oracle
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

TEST_CASE("generation activates a bounded subset of bricks") {
	ve::WorldData w(20, 12, 20); // 16m x 9.6m x 16m
	ve::AnalyticGenerator gen;
	w.generate(gen);
	CHECK(w.active_brick_count() > 0);
	CHECK(w.active_brick_count() < 20 * 12 * 20 / 4); // narrow band: well under 25%
}

TEST_CASE("brick at the terrain surface is active; sky brick is not") {
	ve::WorldData w(20, 12, 20);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	const float h = hills(8.0f, 8.0f); // ~3.07m
	CHECK(w.brick_active(static_cast<int>(8.0f / 0.8f),
	                     static_cast<int>(h / 0.8f),
	                     static_cast<int>(8.0f / 0.8f)));
	CHECK_FALSE(w.brick_active(10, 11, 10)); // y >= 8.8m: above all hills here
}

TEST_CASE("active brick contains a sign change and a non-empty palette") {
	ve::WorldData w(20, 12, 20);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	const float h = hills(8.0f, 8.0f);
	const int slot = w.brick_slot(static_cast<int>(8.0f / 0.8f),
	                              static_cast<int>(h / 0.8f),
	                              static_cast<int>(8.0f / 0.8f));
	REQUIRE(slot >= 0);
	const ve::Brick &b = w.brick(slot);
	bool has_pos = false, has_neg = false;
	for (int i = 0; i < ve::kBrickVoxelCount; i++) {
		const float d = ve::decode_sdf(b.sdf[i]);
		has_pos = has_pos || d > 0.0f;
		has_neg = has_neg || d < 0.0f;
	}
	CHECK(has_pos);
	CHECK(has_neg);
	bool any_material = false;
	for (uint16_t m : b.palette) any_material = any_material || m != 0;
	CHECK(any_material);
}

TEST_CASE("indirection round-trips slot for active bricks, -1 for inactive") {
	ve::WorldData w(20, 12, 20);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	CHECK(static_cast<int>(w.indirection().size()) == 20 * 12 * 20);
	for (int z = 0; z < 20; z++)
		for (int y = 0; y < 12; y++)
			for (int x = 0; x < 20; x++)
				CHECK((w.brick_slot(x, y, z) >= 0) == w.brick_active(x, y, z));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/world_data.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/world/world_data.h`:

```cpp
#pragma once
#include "world/brick.h"
#include "generator/generator.h"
#include <cstdint>
#include <vector>

namespace ve {

struct Dims { int x, y, z; };

// CPU brick store for one bounded world. M1: single-level dense indirection;
// the spec's region level arrives with streaming in M2.
class WorldData {
public:
	WorldData(int bx, int by, int bz);

	void generate(const Generator &gen);

	bool brick_active(int bx, int by, int bz) const;
	int brick_slot(int bx, int by, int bz) const; // -1 if inactive
	const Brick &brick(int slot) const { return bricks_[slot]; }
	int active_brick_count() const { return static_cast<int>(bricks_.size()); }
	const std::vector<int32_t> &indirection() const { return indirection_; }
	Dims dims() const { return dims_; }

private:
	Dims dims_;
	std::vector<int32_t> indirection_; // brick grid -> slot or -1
	std::vector<Brick> bricks_;
};

} // namespace ve
```

`extension/src/world/world_data.cpp`:

```cpp
#include "world/world_data.h"
#include "world/palette.h"
#include <algorithm>

namespace ve {

WorldData::WorldData(int bx, int by, int bz)
	: dims_{bx, by, bz}, indirection_(static_cast<size_t>(bx) * by * bz, -1) {}

void WorldData::generate(const Generator &gen) {
	// Activation probe: 27 sample points per brick; active if the SDF brackets 0.
	// Conservative pad accounts for undersampling between probe points.
	const float pad = 0.15f;
	for (int bz = 0; bz < dims_.z; bz++)
		for (int by = 0; by < dims_.y; by++)
			for (int bx = 0; bx < dims_.x; bx++) {
				float mn = 1e30f, mx = -1e30f;
				for (int sz = 0; sz < 3; sz++)
					for (int sy = 0; sy < 3; sy++)
						for (int sx = 0; sx < 3; sx++) {
							const float wx = (bx * kBrickVoxels + sx * kBrickVoxels / 2) * kVoxelSize;
							const float wy = (by * kBrickVoxels + sy * kBrickVoxels / 2) * kVoxelSize;
							const float wz = (bz * kBrickVoxels + sz * kBrickVoxels / 2) * kVoxelSize;
							const float d = gen.sample(wx, wy, wz).sdf;
							mn = std::min(mn, d);
							mx = std::max(mx, d);
						}
				if (mn >= pad || mx <= -pad) continue; // fully air or fully solid

				const int slot = static_cast<int>(bricks_.size());
				indirection_[static_cast<size_t>(bx) + by * dims_.x + bz * dims_.x * dims_.y] = slot;
				bricks_.emplace_back();
				Brick &b = bricks_.back();
				bool overflow = false;
				for (int vz = 0; vz < kBrickVoxels; vz++)
					for (int vy = 0; vy < kBrickVoxels; vy++)
						for (int vx = 0; vx < kBrickVoxels; vx++) {
							const float wx = (bx * kBrickVoxels + vx) * kVoxelSize;
							const float wy = (by * kBrickVoxels + vy) * kVoxelSize;
							const float wz = (bz * kBrickVoxels + vz) * kVoxelSize;
							const Sample s = gen.sample(wx, wy, wz);
							const int idx = voxel_index(vx, vy, vz);
							b.sdf[idx] = encode_sdf(s.sdf);
							if (s.material != 0) {
								const int pslot = palette_slot(b.palette, s.material, &overflow);
								set_mat_index(b, idx, static_cast<uint8_t>(pslot));
							}
						}
			}
}

bool WorldData::brick_active(int bx, int by, int bz) const {
	return brick_slot(bx, by, bz) >= 0;
}

int WorldData::brick_slot(int bx, int by, int bz) const {
	if (bx < 0 || by < 0 || bz < 0 || bx >= dims_.x || by >= dims_.y || bz >= dims_.z) return -1;
	return indirection_[static_cast<size_t>(bx) + by * dims_.x + bz * dims_.x * dims_.y];
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: all pass. If "sky brick is not active" fails (hills locally exceed 8.8m), print the max hills value with a temporary `MESSAGE(...)` and raise the test's sky-brick Y accordingly.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world/world_data.* extension/tests/test_world_data.cpp
git commit -m "feat(world): WorldData brick generation"
```

---

### Task 7: gdUnit GPU smoke test — RenderingDevice works under test

**Files:**
- Test: `tests/test_gpu_smoke.gd`

**Interfaces:**
- Produces: proof that `RenderingServer.create_local_rendering_device()` + compute + readback works under gdUnit4 on this machine (the pattern Tasks 9/10 extend).

- [ ] **Step 1: Write the test (smoke gate, not TDD — expected to pass immediately)**

`tests/test_gpu_smoke.gd`:

```gdscript
extends GdUnitTestSuite

func test_local_rendering_device_compute_readback() -> void:
	var rd := RenderingServer.create_local_rendering_device()
	assert_object(rd).is_not_null()
	if rd == null:
		return

	var shader_src := """
#[compute]
#version 460
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer Data { float v[]; } data;
void main() { data.v[0] = 42.0; }
"""
	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = shader_src
	var spirv := rd.shader_compile_spirv_from_source(src)
	assert_str(spirv.compile_error_compute).is_empty()
	var shader := rd.shader_create_from_spirv(spirv)
	assert_bool(shader.is_valid()).is_true()

	var buffer := rd.storage_buffer_create(4, PackedFloat32Array([0.0]).to_byte_array())
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = 0
	uniform.add_id(buffer)
	var uset := rd.uniform_set_create([uniform], shader, 0)

	var pipeline := rd.compute_pipeline_create(shader)
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uset, 0)
	rd.compute_list_dispatch(list, 1, 1, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()

	var out := rd.buffer_get_data(buffer).to_float32_array()
	assert_float(out[0]).is_equal_approx(42.0, 0.001)

	rd.free_rid(buffer)
	rd.free_rid(shader)
	rd.free()
```

- [ ] **Step 2: Run**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_gpu_smoke.gd`
Expected: PASS. If it fails, the machine/driver/display setup is broken — stop and fix the environment before any GPU task (try running from a desktop session; check `vulkaninfo`).

- [ ] **Step 3: Commit**

```bash
git add tests/test_gpu_smoke.gd
git commit -m "test(render): GPU compute smoke test"
```

---

### Task 8: `render/shader_loader` — shader files with `#include`

**Files:**
- Create: `extension/src/render/shader_loader.h`, `extension/src/render/shader_loader.cpp`
- Test: `extension/tests/test_shader_loader.cpp`

**Interfaces:**
- Produces: `std::string ve::load_shader_source(const std::string &path, const std::string &include_dir, std::string *error)` — reads `path`, expands lines of the form `#include "name.glsl"` by inlining `include_dir/name.glsl` (recursive, cycle-guarded), returns `""` and sets `*error` on failure.

- [ ] **Step 1: Write the failing test**

`extension/tests/test_shader_loader.cpp`:

```cpp
#include <doctest/doctest.h>
#include "render/shader_loader.h"
#include <filesystem>
#include <fstream>

static std::filesystem::path write_file(const std::filesystem::path &dir,
		const std::string &name, const std::string &content) {
	std::filesystem::create_directories(dir);
	auto path = dir / name;
	std::ofstream f(path);
	f << content;
	return path;
}

TEST_CASE("loads plain file unchanged") {
	auto dir = std::filesystem::temp_directory_path() / "ve_sl1";
	auto p = write_file(dir, "a.glsl", "#version 460\nvoid main() {}\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(err.empty());
	CHECK(out == "#version 460\nvoid main() {}\n");
}

TEST_CASE("expands includes inline, recursively") {
	auto dir = std::filesystem::temp_directory_path() / "ve_sl2";
	write_file(dir, "common.glsl", "const float X = 1.0;\n");
	write_file(dir, "inner.glsl", "#include \"common.glsl\"\nconst float Y = X;\n");
	auto p = write_file(dir, "main.glsl", "#version 460\n#include \"inner.glsl\"\nvoid main() {}\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(err.empty());
	CHECK(out.find("const float X = 1.0;") != std::string::npos);
	CHECK(out.find("const float Y = X;") != std::string::npos);
	CHECK(out.find("#include") == std::string::npos);
}

TEST_CASE("include cycle reports error") {
	auto dir = std::filesystem::temp_directory_path() / "ve_sl3";
	write_file(dir, "a.glsl", "#include \"b.glsl\"\n");
	auto p = write_file(dir, "b.glsl", "#include \"a.glsl\"\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(out.empty());
	CHECK(err.find("cycle") != std::string::npos);
}

TEST_CASE("missing file reports error") {
	std::string err;
	auto out = ve::load_shader_source("/nonexistent/x.glsl", "/nonexistent", &err);
	CHECK(out.empty());
	CHECK_FALSE(err.empty());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `render/shader_loader.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/render/shader_loader.h`:

```cpp
#pragma once
#include <string>

namespace ve {

// Loads a GLSL file and expands `#include "name"` lines against include_dir,
// recursively. Detects include cycles. On failure returns "" and sets *error.
std::string load_shader_source(const std::string &path, const std::string &include_dir,
		std::string *error);

} // namespace ve
```

`extension/src/render/shader_loader.cpp`:

```cpp
#include "render/shader_loader.h"
#include <fstream>
#include <set>
#include <sstream>

namespace ve {

static bool expand(const std::string &path, const std::string &include_dir,
		std::set<std::string> &stack, std::ostringstream &out, std::string *error) {
	if (stack.count(path)) {
		if (error) *error = "include cycle at " + path;
		return false;
	}
	std::ifstream f(path);
	if (!f) {
		if (error) *error = "cannot open " + path;
		return false;
	}
	stack.insert(path);
	std::string line;
	while (std::getline(f, line)) {
		const std::string key = "#include \"";
		const auto pos = line.find(key);
		if (pos != std::string::npos) {
			const auto end = line.find('"', pos + key.size());
			const std::string name = line.substr(pos + key.size(), end - pos - key.size());
			if (!expand(include_dir + "/" + name, include_dir, stack, out, error)) return false;
		} else {
			out << line << '\n';
		}
	}
	stack.erase(path);
	return true;
}

std::string load_shader_source(const std::string &path, const std::string &include_dir,
		std::string *error) {
	std::set<std::string> stack;
	std::ostringstream out;
	if (!expand(path, include_dir, stack, out, error)) return "";
	return out.str();
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/shader_loader.* extension/tests/test_shader_loader.cpp
git commit -m "feat(render): shader file loader with includes"
```

---

### Task 9: `render/gpu_world` — upload WorldData to GPU textures

**Files:**
- Create: `extension/src/render/gpu_world.h`, `extension/src/render/gpu_world.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (gains world generation + GPU upload + `use_local_device` export; `debug_raymarch_pixel` arrives in Task 10)
- Test: `tests/test_gpu_world.gd`

**Interfaces:**
- Consumes: `ve::WorldData` (Task 6), `ve::Brick` (Task 3).
- Produces:
  - `class godot::GpuWorld` (plain C++, not registered with ClassDB):
    - `bool initialize(RenderingDevice *rd, const ve::WorldData &world)` — false on failure (fail-soft)
    - `RID sdf_atlas() const`, `RID mat_atlas() const`, `RID indirection_tex() const`, `RID palette_buffer() const`
    - `void teardown()`
    - `static constexpr int kAtlasBricksX = 32, kAtlasBricksY = 16, kAtlasBricksZ = 32, kAtlasBrickCount = 16384;`
  - `VoxelWorld` new API (bound unless noted):
    - `@export var use_local_device: bool = false` — tests use a private local RenderingDevice (synchronous, single-threaded); the demo uses the main one on the render thread
    - `@export var world_size_bricks: Vector3i = Vector3i(60, 20, 60)`
    - `void ensure_initialized()` — idempotent; builds `ve::WorldData` + uploads
    - `bool is_initialized() const`
    - `RID debug_indirection_tex() const`, `RID debug_sdf_atlas() const`, `RenderingDevice *debug_local_rd() const`
    - C++ only: `GpuWorld *gpu_world()`, `RaymarchPass *raymarch_pass()`, `CompositePass *composite_pass()`, `RenderingDevice *rd() const`

**GPU layout (fixed by this task):**
- `sdf_atlas`: `DATA_FORMAT_R8_UNORM` 3D, 512×256×512, usage `SAMPLING | CAN_UPDATE | CAN_COPY_FROM`
- `mat_atlas`: `DATA_FORMAT_R8_UINT` 3D, same size/usage (values 0–3 unpacked per voxel — 2-bit GPU packing is an M2 optimization)
- `indirection_tex`: `DATA_FORMAT_R32_SINT` 3D, `world_size_bricks`, slot or −1
- `palette_buffer`: storage buffer, 16384 bricks × 4 × uint16 (128KB)

- [ ] **Step 1: Write the failing test**

`tests/test_gpu_world.gd`:

```gdscript
extends GdUnitTestSuite

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.world_size_bricks = Vector3i(20, 12, 20)
	add_child(w)
	return w

func test_world_initializes_and_uploads() -> void:
	var w := make_world()
	w.ensure_initialized()
	assert_bool(w.is_initialized()).is_true()
	assert_bool(w.debug_indirection_tex().is_valid()).is_true()
	assert_bool(w.debug_sdf_atlas().is_valid()).is_true()

func test_gpu_readback_returns_data() -> void:
	var w := make_world()
	w.ensure_initialized()
	var rd := w.debug_local_rd() as RenderingDevice
	assert_object(rd).is_not_null()
	# texture_get_data on a 3D texture returns one 2D slice per call (layer = z slice).
	var slice0: PackedByteArray = rd.texture_get_data(w.debug_sdf_atlas(), 0)
	assert_int(slice0.size()).is_equal(512 * 256) # R8: 1 byte per texel
	var ind0: PackedByteArray = rd.texture_get_data(w.debug_indirection_tex(), 0)
	assert_int(ind0.size()).is_equal(20 * 12 * 4) # R32_SINT: 4 bytes per texel
```

(Exact-value verification is end-to-end in Task 10 via `debug_raymarch_pixel`.)

- [ ] **Step 2: Run test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_gpu_world.gd`
Expected: FAIL — `use_local_device`/`ensure_initialized` don't exist.

- [ ] **Step 3: Write the implementation**

`extension/src/render/gpu_world.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/rids.hpp>
#include "world/world_data.h"

namespace godot {

// Owns the GPU mirror of a ve::WorldData: brick atlases, indirection, palette.
class GpuWorld {
public:
	static constexpr int kAtlasBricksX = 32;
	static constexpr int kAtlasBricksY = 16;
	static constexpr int kAtlasBricksZ = 32;
	static constexpr int kAtlasBrickCount = kAtlasBricksX * kAtlasBricksY * kAtlasBricksZ; // 16384

	bool initialize(RenderingDevice *rd, const ve::WorldData &world);
	void teardown();

	RID sdf_atlas() const { return sdf_atlas_; }
	RID mat_atlas() const { return mat_atlas_; }
	RID indirection_tex() const { return indirection_; }
	RID palette_buffer() const { return palette_; }

private:
	RenderingDevice *rd_ = nullptr;
	RID sdf_atlas_, mat_atlas_, indirection_, palette_;
};

} // namespace godot
```

`extension/src/render/gpu_world.cpp`:

```cpp
#include "render/gpu_world.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <vector>

using namespace godot;

static void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

void GpuWorld::teardown() {
	free_if_valid(rd_, sdf_atlas_);
	free_if_valid(rd_, mat_atlas_);
	free_if_valid(rd_, indirection_);
	free_if_valid(rd_, palette_);
	rd_ = nullptr;
}

bool GpuWorld::initialize(RenderingDevice *rd, const ve::WorldData &world) {
	rd_ = rd;
	const int active = world.active_brick_count();
	if (active <= 0 || active > kAtlasBrickCount) {
		UtilityFunctions::printerr("GpuWorld: active brick count ", active, " out of atlas range");
		return false;
	}

	const int vx_w = kAtlasBricksX * ve::kBrickVoxels; // 512
	const int vx_h = kAtlasBricksY * ve::kBrickVoxels; // 256
	const int vx_d = kAtlasBricksZ * ve::kBrickVoxels; // 512
	const int slice_bytes = vx_w * vx_h;

	// Fill per-slice CPU arrays, then convert to TypedArray for texture_create.
	std::vector<PackedByteArray> sdf_slices(vx_d), mat_slices(vx_d);
	for (int z = 0; z < vx_d; z++) {
		sdf_slices[z].resize(slice_bytes);
		sdf_slices[z].fill(0);
		mat_slices[z].resize(slice_bytes);
		mat_slices[z].fill(0);
	}
	for (int slot = 0; slot < active; slot++) {
		const ve::Brick &b = world.brick(slot);
		const int sx = slot % kAtlasBricksX;
		const int sy = (slot / kAtlasBricksX) % kAtlasBricksY;
		const int sz = slot / (kAtlasBricksX * kAtlasBricksY);
		for (int vz = 0; vz < ve::kBrickVoxels; vz++) {
			PackedByteArray &sdf_slice = sdf_slices[sz * ve::kBrickVoxels + vz];
			PackedByteArray &mat_slice = mat_slices[sz * ve::kBrickVoxels + vz];
			for (int vy = 0; vy < ve::kBrickVoxels; vy++) {
				const int ay = sy * ve::kBrickVoxels + vy;
				for (int vx = 0; vx < ve::kBrickVoxels; vx++) {
					const int ax = sx * ve::kBrickVoxels + vx;
					const int idx = ve::voxel_index(vx, vy, vz);
					const int atlas_i = ax + ay * vx_w;
					sdf_slice[atlas_i] = b.sdf[idx];
					mat_slice[atlas_i] = ve::get_mat_index(b, idx);
				}
			}
		}
	}
	TypedArray<PackedByteArray> sdf_ta, mat_ta;
	for (int z = 0; z < vx_d; z++) { sdf_ta.push_back(sdf_slices[z]); mat_ta.push_back(mat_slices[z]); }

	Ref<RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
	fmt->set_width(vx_w);
	fmt->set_height(vx_h);
	fmt->set_depth(vx_d);
	fmt->set_mipmaps(1);
	fmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	Ref<RDTextureView> view;
	view.instantiate();
	fmt->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
	sdf_atlas_ = rd->texture_create(fmt, view, sdf_ta);
	fmt->set_format(RenderingDevice::DATA_FORMAT_R8_UINT);
	mat_atlas_ = rd->texture_create(fmt, view, mat_ta);

	// --- indirection (R32_SINT 3D, world dims) ---
	const ve::Dims d = world.dims();
	Ref<RDTextureFormat> ifmt;
	ifmt.instantiate();
	ifmt->set_format(RenderingDevice::DATA_FORMAT_R32_SINT);
	ifmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
	ifmt->set_width(d.x);
	ifmt->set_height(d.y);
	ifmt->set_depth(d.z);
	ifmt->set_mipmaps(1);
	ifmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	TypedArray<PackedByteArray> ind_ta;
	for (int z = 0; z < d.z; z++) {
		PackedByteArray slice;
		slice.resize(d.x * d.y * 4);
		int32_t *ptr = reinterpret_cast<int32_t *>(slice.ptrw());
		for (int i = 0; i < d.x * d.y; i++) ptr[i] = world.indirection()[i + z * d.x * d.y];
		ind_ta.push_back(slice);
	}
	indirection_ = rd->texture_create(ifmt, view, ind_ta);

	// --- palette storage buffer ---
	PackedByteArray pal_bytes;
	pal_bytes.resize(kAtlasBrickCount * ve::kBrickPaletteSize * 2);
	pal_bytes.fill(0);
	uint16_t *pal_ptr = reinterpret_cast<uint16_t *>(pal_bytes.ptrw());
	for (int slot = 0; slot < active; slot++)
		for (int p = 0; p < ve::kBrickPaletteSize; p++)
			pal_ptr[slot * ve::kBrickPaletteSize + p] = world.brick(slot).palette[p];
	palette_ = rd->storage_buffer_create(pal_bytes.size(), pal_bytes);

	const bool ok = sdf_atlas_.is_valid() && mat_atlas_.is_valid() && indirection_.is_valid() && palette_.is_valid();
	if (!ok) {
		UtilityFunctions::printerr("GpuWorld: resource creation failed");
		teardown();
	}
	return ok;
}
```

Replace `extension/src/voxel_world.h`:

```cpp
#pragma once
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/rids.hpp>
#include <memory>
#include "world/world_data.h"

namespace godot {

class GpuWorld;
class RaymarchPass;
class CompositePass;

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

	bool use_local_device_ = false;
	Vector3i world_size_bricks_ = Vector3i(60, 20, 60);

	std::unique_ptr<ve::WorldData> world_;
	std::unique_ptr<GpuWorld> gpu_;
	std::unique_ptr<RaymarchPass> raymarch_pass_;   // Task 10
	std::unique_ptr<CompositePass> composite_pass_; // Task 11
	RenderingDevice *main_rd_ = nullptr;
	RenderingDevice *local_rd_ = nullptr; // owned when use_local_device_
	bool initialized_ = false;

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _exit_tree() override;

	void set_use_local_device(bool v) { use_local_device_ = v; }
	bool get_use_local_device() const { return use_local_device_; }
	void set_world_size_bricks(Vector3i v) { world_size_bricks_ = v; }
	Vector3i get_world_size_bricks() const { return world_size_bricks_; }

	void ensure_initialized();
	bool is_initialized() const { return initialized_; }
	RenderingDevice *rd() const;

	GpuWorld *gpu_world() { return gpu_.get(); }
	RaymarchPass *raymarch_pass() { return raymarch_pass_.get(); }
	CompositePass *composite_pass() { return composite_pass_.get(); }

	Color debug_raymarch_pixel(Vector3 origin, Vector3 dir); // Task 10
	RID debug_indirection_tex() const;
	RID debug_sdf_atlas() const;
	RenderingDevice *debug_local_rd() const { return local_rd_; }
};

} // namespace godot
```

Replace `extension/src/voxel_world.cpp`:

```cpp
#include "voxel_world.h"
#include "render/gpu_world.h"
#include "generator/generator.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_use_local_device", "v"), &VoxelWorld::set_use_local_device);
	ClassDB::bind_method(D_METHOD("get_use_local_device"), &VoxelWorld::get_use_local_device);
	ClassDB::bind_method(D_METHOD("set_world_size_bricks", "v"), &VoxelWorld::set_world_size_bricks);
	ClassDB::bind_method(D_METHOD("get_world_size_bricks"), &VoxelWorld::get_world_size_bricks);
	ClassDB::bind_method(D_METHOD("ensure_initialized"), &VoxelWorld::ensure_initialized);
	ClassDB::bind_method(D_METHOD("is_initialized"), &VoxelWorld::is_initialized);
	ClassDB::bind_method(D_METHOD("debug_indirection_tex"), &VoxelWorld::debug_indirection_tex);
	ClassDB::bind_method(D_METHOD("debug_sdf_atlas"), &VoxelWorld::debug_sdf_atlas);
	ClassDB::bind_method(D_METHOD("debug_local_rd"), &VoxelWorld::debug_local_rd);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_local_device"), "set_use_local_device", "get_use_local_device");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_size_bricks"), "set_world_size_bricks", "get_world_size_bricks");
}

void VoxelWorld::_ready() {}

void VoxelWorld::_exit_tree() {
	if (gpu_) gpu_->teardown();
	if (local_rd_) {
		local_rd_->free();
		local_rd_ = nullptr;
	}
}

RenderingDevice *VoxelWorld::rd() const {
	return use_local_device_ ? local_rd_ : main_rd_;
}

void VoxelWorld::ensure_initialized() {
	if (initialized_) return;
	if (use_local_device_ && !local_rd_) {
		local_rd_ = RenderingServer::get_instance()->create_local_rendering_device();
	} else if (!use_local_device_) {
		main_rd_ = RenderingServer::get_instance()->get_rendering_device();
	}
	RenderingDevice *device = rd();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return;
	}
	world_ = std::make_unique<ve::WorldData>(world_size_bricks_.x, world_size_bricks_.y, world_size_bricks_.z);
	ve::AnalyticGenerator gen;
	world_->generate(gen);
	UtilityFunctions::print("VoxelWorld: generated ", world_->active_brick_count(), " bricks");
	gpu_ = std::make_unique<GpuWorld>();
	if (!gpu_->initialize(device, *world_)) {
		gpu_.reset();
		return;
	}
	initialized_ = true;
}

RID VoxelWorld::debug_indirection_tex() const { return gpu_ ? gpu_->indirection_tex() : RID(); }
RID VoxelWorld::debug_sdf_atlas() const { return gpu_ ? gpu_->sdf_atlas() : RID(); }
```

(The `raymarch_pass_`/`composite_pass_` members stay null until Tasks 10/11; don't reference them yet. `debug_raymarch_pixel` is declared now, defined in Task 10 — until then, add a temporary body `Color VoxelWorld::debug_raymarch_pixel(Vector3, Vector3) { return Color(1, 0, 1); }` so the lib links.)

- [ ] **Step 4: Run test to verify it passes**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
Expected: all suites green (boot, GPU smoke, GpuWorld).

- [ ] **Step 5: Commit**

```bash
git add extension/src/render/gpu_world.* extension/src/voxel_world.* tests/test_gpu_world.gd
git commit -m "feat(render): GpuWorld atlas upload"
```

---

### Task 10: Raymarch compute pass + `debug_raymarch_pixel`

**Files:**
- Create: `shaders/common.glsl`, `shaders/raymarch.comp.glsl`
- Create: `extension/src/render/camera_params.h`, `extension/src/render/camera_params.cpp`
- Create: `extension/src/render/raymarch_pass.h`, `extension/src/render/raymarch_pass.cpp`
- Modify: `extension/src/voxel_world.cpp` (real `debug_raymarch_pixel`)
- Test: `extension/tests/test_camera_params.cpp`, `tests/test_raymarch_pixel.gd`

**Interfaces:**
- Consumes: `GpuWorld` RIDs + atlas constants (Task 9), `ve::load_shader_source` (Task 8).
- Produces:
  - `struct ve::CameraParams` — exactly 128 bytes (Vulkan minimum push-constant guarantee):
    ```cpp
    struct CameraParams {
    	float cam_pos[4];   // xyz = position
    	float cam_right[4]; // xyz = unit right
    	float cam_up[4];    // xyz = unit up
    	float cam_fwd[4];   // xyz = unit forward (into screen)
    	float params[4];    // x = tan(half_fov_x), y = tan(half_fov_y), z = max_dist, w unused
    	int32_t dims[4];    // world dims in bricks (xyz), w unused
    	static CameraParams looking_at(float ox, float oy, float oz,
    		float fx, float fy, float fz, float ux, float uy, float uz);
    };
    ```
    `looking_at` builds an orthonormal basis (right = normalize(fwd × up_hint), Gram-Schmidt fallback if degenerate), tan values 0, max_dist 200.
  - `class godot::RaymarchPass`:
    - `void initialize(RenderingDevice *rd)`, `void teardown()`
    - `bool render(RenderingDevice *rd, const GpuWorld &world, const ve::CameraParams &cam, int width, int height)` — renders into internal targets (created/resized on demand)
    - `RID color_texture() const` (RGBA16F), `RID hitpos_texture() const` (RGBA32F: xyz = world hit pos, w = 1 hit / 0 miss)
  - Shader paths: `res://shaders/raymarch.comp.glsl` via `ProjectSettings::globalize_path`, include dir `res://shaders`.

- [ ] **Step 1: Write the failing native test**

`extension/tests/test_camera_params.cpp`:

```cpp
#include <doctest/doctest.h>
#include "render/camera_params.h"

TEST_CASE("looking_at builds an orthonormal basis aligned to forward") {
	auto cp = ve::CameraParams::looking_at(1, 2, 3, 0, 0, -1, 0, 1, 0);
	CHECK(cp.cam_pos[0] == 1);
	CHECK(cp.cam_pos[2] == 3);
	CHECK(cp.cam_fwd[2] == doctest::Approx(-1.0));
	auto dot = [](const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
	CHECK(dot(cp.cam_right, cp.cam_right) == doctest::Approx(1.0));
	CHECK(dot(cp.cam_up, cp.cam_up) == doctest::Approx(1.0));
	CHECK(dot(cp.cam_right, cp.cam_fwd) == doctest::Approx(0.0));
	CHECK(dot(cp.cam_up, cp.cam_fwd) == doctest::Approx(0.0));
	CHECK(dot(cp.cam_right, cp.cam_up) == doctest::Approx(0.0));
}

TEST_CASE("looking_at handles degenerate up hint (parallel to forward)") {
	auto cp = ve::CameraParams::looking_at(0, 0, 0, 0, 1, 0, 0, 1, 0);
	auto dot = [](const float *a, const float *b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
	CHECK(dot(cp.cam_right, cp.cam_fwd) == doctest::Approx(0.0));
	CHECK(dot(cp.cam_up, cp.cam_up) == doctest::Approx(1.0));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `render/camera_params.h` not found.

- [ ] **Step 3: Implement CameraParams**

`extension/src/render/camera_params.h`:

```cpp
#pragma once
#include <cstdint>

namespace ve {

// 128-byte push-constant block shared by raymarch.comp.glsl.
struct CameraParams {
	float cam_pos[4];
	float cam_right[4];
	float cam_up[4];
	float cam_fwd[4];
	float params[4];   // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	int32_t dims[4];   // world dims in bricks (xyz), unused

	// Basis from position/forward/up-hint; tan fov = 0, max_dist = 200.
	static CameraParams looking_at(float ox, float oy, float oz,
			float fx, float fy, float fz, float ux, float uy, float uz);
};

static_assert(sizeof(CameraParams) == 128);

} // namespace ve
```

`extension/src/render/camera_params.cpp`:

```cpp
#include "render/camera_params.h"
#include <cmath>

namespace ve {

CameraParams CameraParams::looking_at(float ox, float oy, float oz,
		float fx, float fy, float fz, float ux, float uy, float uz) {
	CameraParams cp{};
	cp.cam_pos[0] = ox; cp.cam_pos[1] = oy; cp.cam_pos[2] = oz;

	float fl = sqrtf(fx * fx + fy * fy + fz * fz);
	fx /= fl; fy /= fl; fz /= fl;
	// right = normalize(fwd x up_hint); if degenerate, pick another hint
	float rx = fy * uz - fz * uy, ry = fz * ux - fx * uz, rz = fx * uy - fy * ux;
	float rl = sqrtf(rx * rx + ry * ry + rz * rz);
	if (rl < 1e-5f) {
		ux = 1; uy = 0; uz = 0;
		rx = fy * uz - fz * uy; ry = fz * ux - fx * uz; rz = fx * uy - fy * ux;
		rl = sqrtf(rx * rx + ry * ry + rz * rz);
	}
	rx /= rl; ry /= rl; rz /= rl;
	// up = right x fwd
	const float upx = ry * fz - rz * fy, upy = rz * fx - rx * fz, upz = rx * fy - ry * fx;

	cp.cam_right[0] = rx; cp.cam_right[1] = ry; cp.cam_right[2] = rz;
	cp.cam_up[0] = upx; cp.cam_up[1] = upy; cp.cam_up[2] = upz;
	cp.cam_fwd[0] = fx; cp.cam_fwd[1] = fy; cp.cam_fwd[2] = fz;
	cp.params[2] = 200.0f;
	return cp;
}

} // namespace ve
```

- [ ] **Step 4: Run native test to verify it passes**

Run: `cd extension && scons test`
Expected: all pass.

- [ ] **Step 5: Write the failing gdUnit test**

`tests/test_raymarch_pixel.gd`:

```gdscript
extends GdUnitTestSuite

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.world_size_bricks = Vector3i(20, 12, 20)
	add_child(w)
	w.ensure_initialized()
	return w

func test_ray_down_from_sky_hits_terrain() -> void:
	var w := make_world()
	# From (8, 12, 8) looking straight down: hills here are ~3m, must hit.
	var c: Color = w.debug_raymarch_pixel(Vector3(8, 12, 8), Vector3(0, -1, 0))
	# Terrain albedos are green/grey/brown; sky is blue-dominant.
	assert_bool(c.b <= c.g or c.r > 0.1).is_true()

func test_ray_up_from_air_misses_to_sky() -> void:
	var w := make_world()
	var c: Color = w.debug_raymarch_pixel(Vector3(8, 8, 8), Vector3(0, 1, 0))
	# Sky gradient looking up is blue-dominant.
	assert_bool(c.b > c.r).is_true()
```

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_raymarch_pixel.gd`
Expected: FAIL — stub returns magenta `(1, 0, 1)` which fails both assertions.

- [ ] **Step 6: Write the shaders**

`shaders/common.glsl`:

```glsl
// Shared constants + helpers. Included via ve::load_shader_source (#include "common.glsl").
const int BRICK_VOXELS = 16;
const float VOXEL_SIZE = 0.05;
const float BRICK_SIZE = 0.8;         // 16 * 0.05
const float SDF_RANGE = 0.64;         // uint8 unorm <-> [-0.64, 0.64]
const ivec3 ATLAS_BRICKS = ivec3(32, 16, 32);

float decode_sdf(float unorm) { return unorm * 2.0 * SDF_RANGE - SDF_RANGE; }

vec3 material_albedo(uint mat_id) {
	switch (mat_id) {
		case 1: return vec3(0.36, 0.55, 0.22); // grass
		case 2: return vec3(0.45, 0.42, 0.40); // rock
		case 3: return vec3(0.50, 0.35, 0.20); // dirt
		default: return vec3(1.0, 0.0, 1.0);   // error magenta
	}
}

vec3 sky_color(vec3 dir) {
	float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
	return mix(vec3(0.55, 0.45, 0.35), vec3(0.25, 0.45, 0.85), t);
}
```

`shaders/raymarch.comp.glsl` (complete file — DDA with explicit per-brick `[t_enter, t_exit]` intervals):

```glsl
#[compute]
#version 460

#include "common.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) writeonly uniform image2D out_color;
layout(set = 0, binding = 1, rgba32f) writeonly uniform image2D out_hitpos;
layout(set = 0, binding = 2) uniform sampler3D sdf_atlas;    // R8 unorm, nearest
layout(set = 0, binding = 3) uniform usampler3D mat_atlas;   // R8 uint, nearest
layout(set = 0, binding = 4) uniform isampler3D indirection; // R32 sint, world dims, nearest
layout(set = 0, binding = 5, std430) readonly buffer Palette { uint ids[]; } palette_buf;

layout(push_constant, std430) uniform Push {
	vec4 cam_pos;
	vec4 cam_right;
	vec4 cam_up;
	vec4 cam_fwd;
	vec4 params;  // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	ivec4 dims;   // world dims in bricks
} pc;

// Manual trilinear inside one brick (adjacent bricks are unrelated: never filter across).
float brick_sdf(int slot, vec3 local) { // local in voxel units [0, 16)
	vec3 p = clamp(local, vec3(0.0), vec3(15.0));
	ivec3 i0 = ivec3(floor(p));
	vec3 f = p - vec3(i0);
	ivec3 i1 = min(i0 + 1, ivec3(15));
	ivec3 base = ivec3(slot % ATLAS_BRICKS.x,
	                   (slot / ATLAS_BRICKS.x) % ATLAS_BRICKS.y,
	                   slot / (ATLAS_BRICKS.x * ATLAS_BRICKS.y)) * BRICK_VOXELS;
	float c000 = texelFetch(sdf_atlas, base + ivec3(i0.x, i0.y, i0.z), 0).r;
	float c100 = texelFetch(sdf_atlas, base + ivec3(i1.x, i0.y, i0.z), 0).r;
	float c010 = texelFetch(sdf_atlas, base + ivec3(i0.x, i1.y, i0.z), 0).r;
	float c110 = texelFetch(sdf_atlas, base + ivec3(i1.x, i1.y, i0.z), 0).r;
	float c001 = texelFetch(sdf_atlas, base + ivec3(i0.x, i0.y, i1.z), 0).r;
	float c101 = texelFetch(sdf_atlas, base + ivec3(i1.x, i0.y, i1.z), 0).r;
	float c011 = texelFetch(sdf_atlas, base + ivec3(i0.x, i1.y, i1.z), 0).r;
	float c111 = texelFetch(sdf_atlas, base + ivec3(i1.x, i1.y, i1.z), 0).r;
	float v = mix(mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y),
	              mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y), f.z);
	return decode_sdf(v);
}

int slot_at(ivec3 brick) {
	if (any(lessThan(brick, ivec3(0))) || any(greaterThanEqual(brick, pc.dims.xyz))) return -1;
	return texelFetch(indirection, brick, 0).r;
}

float world_sdf(vec3 p) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = slot_at(brick);
	if (slot < 0) return SDF_RANGE; // empty: caller stays within its brick interval
	vec3 local = (p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE;
	return brick_sdf(slot, local);
}

vec3 calc_normal(vec3 p) {
	const float e = 0.01;
	return normalize(vec3(
		world_sdf(p + vec3(e, 0, 0)) - world_sdf(p - vec3(e, 0, 0)),
		world_sdf(p + vec3(0, e, 0)) - world_sdf(p - vec3(0, e, 0)),
		world_sdf(p + vec3(0, 0, e)) - world_sdf(p - vec3(0, 0, e))));
}

uint material_at(vec3 p) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = slot_at(brick);
	if (slot < 0) return 0u;
	vec3 local = clamp((p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE, vec3(0.0), vec3(15.0));
	ivec3 base = ivec3(slot % ATLAS_BRICKS.x,
	                   (slot / ATLAS_BRICKS.x) % ATLAS_BRICKS.y,
	                   slot / (ATLAS_BRICKS.x * ATLAS_BRICKS.y)) * BRICK_VOXELS;
	uint idx = texelFetch(mat_atlas, base + ivec3(local), 0).r;
	return palette_buf.ids[slot * 4 + idx];
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_color);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	// image row 0 = screen top = +up direction
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

	vec3 ro = pc.cam_pos.xyz;
	vec3 rd = normalize(pc.cam_fwd.xyz
		+ pc.cam_right.xyz * ndc.x * pc.params.x
		+ pc.cam_up.xyz * ndc.y * pc.params.y);
	float max_dist = pc.params.z;

	vec3 color = sky_color(rd);
	vec4 hitpos = vec4(0.0);

	// Brick-grid DDA. side[d] = ray t at the next boundary along axis d.
	ivec3 map = ivec3(floor(ro / BRICK_SIZE));
	vec3 delta = abs(vec3(BRICK_SIZE) / rd);
	ivec3 st = ivec3(sign(rd));
	vec3 side = (vec3(st) * (vec3(map) * BRICK_SIZE - ro)
	             + (vec3(st) * 0.5 + 0.5) * BRICK_SIZE) / rd;
	float t_prev = 0.0; // entry t of the current cell (last boundary crossed)

	bool hit = false;
	for (int i = 0; i < 512; i++) {
		float t_exit = min(side.x, min(side.y, side.z));
		if (t_exit > max_dist) break;

		int slot = slot_at(map);
		if (slot >= 0) {
			float t = t_prev;
			for (int j = 0; j < 64; j++) {
				vec3 p = ro + rd * t;
				float d = world_sdf(p);
				if (d < 0.002) {
					for (int k = 0; k < 4; k++) { // secant refinement
						float dk = world_sdf(p);
						t += dk * 0.5;
						p = ro + rd * t;
					}
					vec3 n = calc_normal(p);
					vec3 alb = material_albedo(material_at(p));
					vec3 sun = normalize(vec3(0.6, 0.8, 0.3));
					float lam = max(dot(n, sun), 0.0);
					color = alb * (0.25 + 0.75 * lam);
					hitpos = vec4(p, 1.0);
					hit = true;
					break;
				}
				t += max(d * 0.9, 0.005);
				if (t > t_exit) break;
			}
			if (hit) break;
		}

		if (side.x < side.y && side.x < side.z) { t_prev = side.x; side.x += delta.x; map.x += st.x; }
		else if (side.y < side.z)               { t_prev = side.y; side.y += delta.y; map.y += st.y; }
		else                                    { t_prev = side.z; side.z += delta.z; map.z += st.z; }
	}

	imageStore(out_color, px, vec4(color, 1.0));
	imageStore(out_hitpos, px, hitpos);
}
```

- [ ] **Step 7: Implement RaymarchPass**

`extension/src/render/raymarch_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/rids.hpp>
#include "render/camera_params.h"

namespace godot {

class GpuWorld;

class RaymarchPass {
public:
	void initialize(RenderingDevice *rd);
	void teardown();
	bool render(RenderingDevice *rd, const GpuWorld &world, const ve::CameraParams &cam,
			int width, int height);

	RID color_texture() const { return color_; }
	RID hitpos_texture() const { return hitpos_; }

private:
	RID make_target(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h);
	void rebuild_targets(RenderingDevice *rd, const GpuWorld &world, int w, int h);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_;
	RID sampler_; // shared NEAREST sampler, created once
	RID color_, hitpos_, uset_;
	int width_ = 0, height_ = 0;
};

} // namespace godot
```

`extension/src/render/raymarch_pass.cpp`:

```cpp
#include "render/raymarch_pass.h"
#include "render/gpu_world.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

void RaymarchPass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	std::string err;
	const String path = ProjectSettings::get_singleton()->globalize_path("res://shaders/raymarch.comp.glsl");
	const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
	const std::string code = ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err);
	if (code.empty()) {
		UtilityFunctions::printerr("RaymarchPass: shader load failed: ", err.c_str());
		return;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("RaymarchPass: ", compile_err);
		return;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	pipeline_ = rd->compute_pipeline_create(shader_);

	Ref<RDSamplerState> ss;
	ss.instantiate();
	ss->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	ss->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_ = rd->sampler_create(ss);
}

void RaymarchPass::teardown() {
	if (!rd_) return;
	for (RID *r : {&shader_, &color_, &hitpos_, &sampler_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	rd_ = nullptr;
}

RID RaymarchPass::make_target(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h) {
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_format(fmt);
	f->set_width(w);
	f->set_height(h);
	f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	Ref<RDTextureView> v;
	v.instantiate();
	return rd->texture_create(f, v, {});
}

void RaymarchPass::rebuild_targets(RenderingDevice *rd, const GpuWorld &world, int w, int h) {
	if (color_.is_valid()) rd->free_rid(color_);
	if (hitpos_.is_valid()) rd->free_rid(hitpos_);
	color_ = make_target(rd, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, w, h);
	hitpos_ = make_target(rd, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, w, h);
	width_ = w;
	height_ = h;

	Ref<RDUniform> u[6];
	for (int i = 0; i < 6; i++) u[i].instantiate();
	u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[0]->set_binding(0); u[0]->add_id(color_);
	u[1]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[1]->set_binding(1); u[1]->add_id(hitpos_);
	u[2]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[2]->set_binding(2); u[2]->add_id(sampler_); u[2]->add_id(world.sdf_atlas());
	u[3]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[3]->set_binding(3); u[3]->add_id(sampler_); u[3]->add_id(world.mat_atlas());
	u[4]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[4]->set_binding(4); u[4]->add_id(sampler_); u[4]->add_id(world.indirection_tex());
	u[5]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u[5]->set_binding(5); u[5]->add_id(world.palette_buffer());
	uset_ = rd->uniform_set_create(Array::make(u[0], u[1], u[2], u[3], u[4], u[5]), shader_, 0);
}

bool RaymarchPass::render(RenderingDevice *rd, const GpuWorld &world, const ve::CameraParams &cam,
		int width, int height) {
	if (!shader_.is_valid()) return false;
	if (width != width_ || height != height_ || !uset_.is_valid()) {
		rebuild_targets(rd, world, width, height);
	}
	if (!uset_.is_valid() || !color_.is_valid()) return false;

	PackedByteArray pc;
	pc.resize(sizeof(ve::CameraParams));
	std::memcpy(pc.ptrw(), &cam, sizeof(ve::CameraParams));

	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (width + 7) / 8, (height + 7) / 8, 1);
	rd->compute_list_end();
	return true;
}
```

- [ ] **Step 8: Implement debug_raymarch_pixel (replace the Task-9 stub)**

In `voxel_world.cpp`, add includes `<cstring>`, `"render/raymarch_pass.h"`, `"render/camera_params.h"`; in `ensure_initialized()` (after `gpu_->initialize` succeeds) add:

```cpp
	raymarch_pass_ = std::make_unique<RaymarchPass>();
	raymarch_pass_->initialize(device);
```

and replace the stub with:

```cpp
static float half_to_float(uint16_t v) {
	const uint32_t sign = (v & 0x8000u) << 16;
	const uint32_t exp = (v >> 10) & 0x1F;
	const uint32_t mant = v & 0x3FF;
	if (exp == 0) return (sign ? -1.0f : 1.0f) * mant / 1024.0f / 16384.0f;
	uint32_t bits = sign | ((exp + 112) << 23) | (mant << 13);
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

Color VoxelWorld::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !raymarch_pass_) return Color(1, 0, 1);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.dims[0] = world_size_bricks_.x;
	cam.dims[1] = world_size_bricks_.y;
	cam.dims[2] = world_size_bricks_.z;
	if (!raymarch_pass_->render(device, *gpu_, cam, 1, 1)) return Color(1, 0, 1);
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (data.size() < 8) return Color(1, 0, 1);
	const uint16_t *h = reinterpret_cast<const uint16_t *>(data.ptr());
	return Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
}
```

Also bind it in `_bind_methods()`:

```cpp
	ClassDB::bind_method(D_METHOD("debug_raymarch_pixel", "origin", "dir"), &VoxelWorld::debug_raymarch_pixel);
```

- [ ] **Step 9: Run tests to verify they pass**

Run: `cd extension && scons test` → native green.
Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests` → all gdUnit green; the down-ray returns terrain color, up-ray returns sky.
If the down-ray returns sky (ray tunnels): the most likely cause is the DDA `side` initialization — print `side`, `map`, `t_prev` for the first 8 iterations and compare against a hand-traced DDA for ray `(8,12,8)+(0,-1,0)t`.

- [ ] **Step 10: Commit**

```bash
git add shaders/ extension/src/render/camera_params.* extension/src/render/raymarch_pass.* extension/src/voxel_world.cpp extension/tests/test_camera_params.cpp tests/test_raymarch_pixel.gd
git commit -m "feat(render): raymarch compute pass + debug pixel"
```

---

### Task 11: Compositor + depth composite — terrain into the Godot frame

**Files:**
- Create: `shaders/composite.vert.glsl`, `shaders/composite.frag.glsl`
- Create: `extension/src/render/composite_pass.h`, `extension/src/render/composite_pass.cpp`
- Create: `extension/src/raymarch_compositor.h`, `extension/src/raymarch_compositor.cpp`
- Modify: `extension/src/register_types.cpp` (register `RaymarchCompositor`)
- Modify: `extension/src/voxel_world.cpp` (construct `CompositePass` in `ensure_initialized`)
- Create: `demo/main.tscn`, `demo/fly_camera.gd` (stub), `demo/hud.gd` (stub)

**Interfaces:**
- Consumes: `RaymarchPass` (Task 10), `GpuWorld` (Task 9), `VoxelWorld` (Task 9–10).
- Produces:
  - `class godot::CompositePass`:
    - `void initialize(RenderingDevice *rd)` — loads shaders, creates samplers; pipeline + framebuffer built lazily in `draw` (destination formats are only known there)
    - `void draw(RenderingDevice *rd, RID dst_color, RID dst_depth, RID src_color, RID src_hitpos, const Projection &view_proj)` — fullscreen triangle; copies color; writes `gl_FragDepth` from the reconstructed hit position; sky pixels (hitpos.w == 0) write color with `gl_FragDepth = 1.0`
    - `void teardown()`
  - `class godot::RaymarchCompositor : public CompositorEffect`, registered as `RaymarchCompositor`:
    - `@export var world_path: NodePath`
    - constructor sets `effect_callback_type = EFFECT_CALLBACK_TYPE_PRE_OPAQUE`
    - `_render_callback(int, RenderData *)` override: resolves the `VoxelWorld`, ensures init (render thread — safe for the main RenderingDevice), builds `ve::CameraParams` from `RenderSceneData`, raymarches at 0.66× internal size, composites into scene color+depth
  - Depth convention (verified by the occlusion checklist below): `gl_FragDepth = clamp((clip.z / clip.w) * 0.5 + 0.5, 0.0, 1.0)` with `clip = view_proj * vec4(hit_pos, 1)` — Godot `Projection` is GL-style NDC z∈[−1,1]; the depth buffer stores [0,1].

- [ ] **Step 1: Write the composite shaders**

`shaders/composite.vert.glsl` (NDC −y is up-screen with UV (0,0) at top-left under Godot's RenderingDevice viewport convention; verified by the checklist):

```glsl
#version 460
layout(location = 0) out vec2 uv_out;
void main() {
	// fullscreen triangle from vertex index: covers screen, uv in [0,1] on-screen
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	uv_out = p;
	gl_Position = vec4(p.x * 2.0 - 1.0, p.y * 2.0 - 1.0, 0.0, 1.0);
}
```

`shaders/composite.frag.glsl`:

```glsl
#version 460
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0) uniform sampler2D src_color;  // linear (0.66x upscale)
layout(set = 0, binding = 1) uniform sampler2D src_hitpos; // nearest (no silhouette smear)
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
} pc;
void main() {
	vec4 hp = texture(src_hitpos, uv_in);
	frag_color = texture(src_color, uv_in);
	if (hp.w < 0.5) {
		gl_FragDepth = 1.0; // sky: farthest, Godot objects always in front
		return;
	}
	vec4 clip = pc.view_proj * vec4(hp.xyz, 1.0);
	gl_FragDepth = clamp((clip.z / clip.w) * 0.5 + 0.5, 0.0, 1.0);
}
```

- [ ] **Step 2: Implement CompositePass**

`extension/src/render/composite_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/rids.hpp>
#include <godot_cpp/variant/projection.hpp>

namespace godot {

class CompositePass {
public:
	void initialize(RenderingDevice *rd);
	void teardown();
	void draw(RenderingDevice *rd, RID dst_color, RID dst_depth,
			RID src_color, RID src_hitpos, const Projection &view_proj);

private:
	bool ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth);

	RenderingDevice *rd_ = nullptr;
	RID shader_;
	RID pipeline_;
	RID sampler_linear_, sampler_nearest_;
	int64_t fb_format_ = 0;
	RID framebuffer_, fb_color_, fb_depth_;
};

} // namespace godot
```

`extension/src/render/composite_pass.cpp`:

```cpp
#include "render/composite_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

void CompositePass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	auto load_stage = [&](const char *file, RenderingDevice::ShaderStage stage, Ref<RDShaderSource> &src) -> bool {
		std::string err;
		const String path = ProjectSettings::get_singleton()->globalize_path(String("res://shaders/") + file);
		const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
		const std::string code = ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err);
		if (code.empty()) {
			UtilityFunctions::printerr("CompositePass: ", err.c_str());
			return false;
		}
		src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		src->set_stage_source(stage, String(code.c_str()));
		return true;
	};
	Ref<RDShaderSource> src;
	src.instantiate();
	if (!load_stage("composite.vert.glsl", RenderingDevice::SHADER_STAGE_VERTEX, src)) return;
	if (!load_stage("composite.frag.glsl", RenderingDevice::SHADER_STAGE_FRAGMENT, src)) return;
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	shader_ = rd->shader_create_from_spirv(spirv);

	Ref<RDSamplerState> sl;
	sl.instantiate();
	sl->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sl->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_linear_ = rd->sampler_create(sl);
	Ref<RDSamplerState> sn;
	sn.instantiate();
	sn->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sn->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_nearest_ = rd->sampler_create(sn);
}

void CompositePass::teardown() {
	if (!rd_) return;
	for (RID *r : {&shader_, &pipeline_, &sampler_linear_, &sampler_nearest_, &framebuffer_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	rd_ = nullptr;
}

bool CompositePass::ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth) {
	if (pipeline_.is_valid() && framebuffer_.is_valid() && dst_color == fb_color_ && dst_depth == fb_depth_) {
		return true;
	}
	const RenderingDevice::DataFormat cf = rd->texture_get_format(dst_color)->get_format();
	const RenderingDevice::DataFormat df = rd->texture_get_format(dst_depth)->get_format();

	Ref<RDAttachmentFormat> c_att;
	c_att.instantiate();
	c_att->set_format(cf);
	c_att->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	c_att->set_usage_flags(RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDAttachmentFormat> d_att;
	d_att.instantiate();
	d_att->set_format(df);
	d_att->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
	d_att->set_usage_flags(RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDFramebufferPass> fpass;
	fpass.instantiate();
	fpass->set_color_attachments(Array::make(c_att));
	fpass->set_depth_attachment(d_att);
	fb_format_ = rd->framebuffer_format_create(Array::make(fpass));

	if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
	framebuffer_ = rd->framebuffer_create(Array::make(dst_color, dst_depth));
	fb_color_ = dst_color;
	fb_depth_ = dst_depth;

	if (!pipeline_.is_valid()) {
		Ref<RDPipelineRasterizationState> rs;
		rs.instantiate();
		rs->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
		Ref<RDPipelineMultisampleState> ms;
		ms.instantiate();
		Ref<RDPipelineDepthStencilState> ds;
		ds.instantiate();
		ds->set_enable_depth_test(true);
		ds->set_enable_depth_write(true);
		ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_ALWAYS);
		Ref<RDPipelineColorBlendStateAttachment> att;
		att.instantiate();
		att->set_enable_blend(false);
		Ref<RDPipelineColorBlendState> cb;
		cb.instantiate();
		cb->set_attachments(Array::make(att));
		const RID vertex_format = rd->vertex_format_create(Array()); // no attributes
		pipeline_ = rd->render_pipeline_create(shader_, fb_format_, vertex_format,
				RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
	}
	return pipeline_.is_valid() && framebuffer_.is_valid();
}

void CompositePass::draw(RenderingDevice *rd, RID dst_color, RID dst_depth,
		RID src_color, RID src_hitpos, const Projection &view_proj) {
	if (!shader_.is_valid()) return;
	if (!ensure_pipeline(rd, dst_color, dst_depth)) return;

	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u0->set_binding(0);
	u0->add_id(sampler_linear_);
	u0->add_id(src_color);
	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u1->set_binding(1);
	u1->add_id(sampler_nearest_);
	u1->add_id(src_hitpos);
	const RID uset = rd->uniform_set_create(Array::make(u0, u1), shader_, 0);
	if (!uset.is_valid()) return;

	PackedByteArray pc;
	pc.resize(64);
	{
		float *f = reinterpret_cast<float *>(pc.ptrw());
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				f[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
	}

	const int64_t dl = rd->draw_list_begin(framebuffer_,
			RenderingDevice::INITIAL_ACTION_LOAD, RenderingDevice::FINAL_ACTION_STORE,
			RenderingDevice::INITIAL_ACTION_LOAD, RenderingDevice::FINAL_ACTION_STORE);
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset, 0);
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	rd->draw_list_draw(dl, false, 1, 3);
	rd->draw_list_end();
}
```

- [ ] **Step 3: Implement RaymarchCompositor + register it**

`extension/src/raymarch_compositor.h`:

```cpp
#pragma once
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>

namespace godot {

class RaymarchCompositor : public CompositorEffect {
	GDCLASS(RaymarchCompositor, CompositorEffect)

	NodePath world_path_;

protected:
	static void _bind_methods();

public:
	RaymarchCompositor();
	void set_world_path(const NodePath &p) { world_path_ = p; }
	NodePath get_world_path() const { return world_path_; }
	void _render_callback(int p_effect_callback_type, RenderData *p_render_data) override;
};

} // namespace godot
```

`extension/src/raymarch_compositor.cpp`:

```cpp
#include "raymarch_compositor.h"
#include "voxel_world.h"
#include "render/gpu_world.h"
#include "render/raymarch_pass.h"
#include "render/composite_pass.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/projection.hpp>

using namespace godot;

RaymarchCompositor::RaymarchCompositor() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
}

void RaymarchCompositor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_world_path", "p"), &RaymarchCompositor::set_world_path);
	ClassDB::bind_method(D_METHOD("get_world_path"), &RaymarchCompositor::get_world_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "world_path"), "set_world_path", "get_world_path");
}

void RaymarchCompositor::_render_callback(int cb_type, RenderData *render_data) {
	if (cb_type != EFFECT_CALLBACK_TYPE_PRE_OPAQUE) return;
	if (world_path_.is_empty()) return;
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree) return;
	VoxelWorld *world = Object::cast_to<VoxelWorld>(tree->get_root()->get_node_or_null(world_path_));
	if (!world || world->get_use_local_device()) return;

	world->ensure_initialized(); // no-op after first frame; runs on the render thread here
	if (!world->is_initialized()) return;

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	RenderSceneBuffersRD *rsb = Object::cast_to<RenderSceneBuffersRD>(render_data->get_render_scene_buffers());
	RenderSceneData *sd = render_data->get_render_scene_data();
	if (!rsb || !sd) return;
	const Vector2i size = rsb->get_internal_size();
	if (size.x <= 0 || size.y <= 0) return;

	const Transform3D cam = sd->get_cam_transform();
	const Projection proj = sd->get_cam_projection();
	const float tan_y = tanf(Math::deg_to_rad(proj.get_fov()) * 0.5f);
	const float tan_x = tan_y * (static_cast<float>(size.x) / static_cast<float>(size.y));

	ve::CameraParams cp{};
	const Vector3 right = cam.basis.get_column(0);
	const Vector3 up = cam.basis.get_column(1);
	const Vector3 fwd = -cam.basis.get_column(2);
	cp.cam_pos[0] = cam.origin.x; cp.cam_pos[1] = cam.origin.y; cp.cam_pos[2] = cam.origin.z;
	cp.cam_right[0] = right.x; cp.cam_right[1] = right.y; cp.cam_right[2] = right.z;
	cp.cam_up[0] = up.x; cp.cam_up[1] = up.y; cp.cam_up[2] = up.z;
	cp.cam_fwd[0] = fwd.x; cp.cam_fwd[1] = fwd.y; cp.cam_fwd[2] = fwd.z;
	cp.params[0] = tan_x; cp.params[1] = tan_y; cp.params[2] = 200.0f;
	const Vector3i wd = world->get_world_size_bricks();
	cp.dims[0] = wd.x; cp.dims[1] = wd.y; cp.dims[2] = wd.z;

	const int rw = static_cast<int>(size.x * 0.66f);
	const int rh = static_cast<int>(size.y * 0.66f);
	if (!world->raymarch_pass()->render(rd, *world->gpu_world(), cp, rw, rh)) return;

	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	world->composite_pass()->draw(rd, rsb->get_color_texture(), rsb->get_depth_texture(),
			world->raymarch_pass()->color_texture(), world->raymarch_pass()->hitpos_texture(),
			view_proj);
}
```

(If `get_color_texture()`/`get_depth_texture()` don't exist under those exact names in godot-cpp 4.7, check `render_scene_buffers_rd.h` — the layered variants are `get_color_layer(0)`/`get_depth_layer(0)`.)

In `register_types.cpp`, add the include and registration:

```cpp
#include "raymarch_compositor.h"
// inside voxel_everything_initialize, after GDREGISTER_CLASS(VoxelWorld):
	GDREGISTER_CLASS(RaymarchCompositor);
```

In `voxel_world.cpp` `ensure_initialized()`, after the RaymarchPass creation:

```cpp
	composite_pass_ = std::make_unique<CompositePass>();
	composite_pass_->initialize(device);
```

(include `"render/composite_pass.h"`.)

- [ ] **Step 4: Create the demo scene + script stubs**

`demo/fly_camera.gd` (stub; Task 12 fills it):

```gdscript
extends Camera3D
```

`demo/hud.gd` (stub; Task 12 fills it):

```gdscript
extends Label
```

`demo/main.tscn` (camera basis: forward = −z column = (0, −0.34, −0.94) → looking down ~20° toward −z, from y=14 above terrain):

```
[gd_scene load_steps=7 format=3 uid="uid://ve_main_scene"]

[ext_resource type="Script" path="res://demo/fly_camera.gd" id="1"]
[ext_resource type="Script" path="res://demo/hud.gd" id="2"]

[sub_resource type="RaymarchCompositor" id="1"]
world_path = NodePath("/root/Main/VoxelWorld")

[sub_resource type="Compositor" id="2"]
compositor_effects = [SubResource(1)]

[sub_resource type="Environment" id="3"]
background_mode = 1
ambient_light_source = 3
ambient_light_energy = 0.4

[sub_resource type="BoxMesh" id="4"]
size = Vector3(2, 2, 2)

[node name="Main" type="Node3D"]

[node name="VoxelWorld" type="VoxelWorld" parent="."]
world_size_bricks = Vector3i(60, 20, 60)

[node name="WorldEnvironment" type="WorldEnvironment" parent="."]
environment = SubResource(3)
compositor = SubResource(2)

[node name="Camera3D" type="Camera3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 0.94, -0.34, 0, 0.34, 0.94, 8, 14, 20)
script = ExtResource(1)

[node name="DirectionalLight3D" type="DirectionalLight3D" parent="."]
transform = Transform3D(0.6, -0.48, 0.64, 0, 0.8, 0.6, -0.8, -0.36, 0.48, 0, 20, 0)

[node name="TestCube" type="MeshInstance3D" parent="."]
transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 24, 6, 24)
mesh = SubResource(4)

[node name="HUD" type="CanvasLayer" parent="."]

[node name="Label" type="Label" parent="HUD"]
offset_left = 10.0
offset_top = 10.0
script = ExtResource(2)
```

- [ ] **Step 5: Verify visually (occlusion + orientation checklist)**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything --gpu-validation demo/main.tscn`
Checklist (record results in the commit message):
- Terrain is right-side up (hills below, sky above) — if vertically flipped, negate `p.y` in `composite.vert.glsl`
- Smooth terrain with the cave visible near world (30, ~3, 30); colors are grass/rock/dirt, no magenta
- TestCube at (24, 6, 24): hidden behind a hill when viewed over it; occludes terrain behind it when sitting in front — if inverted (cube visible through hills), the depth remap is wrong: remove `* 0.5 + 0.5` in `composite.frag.glsl` and retest
- No Vulkan validation errors in the console

- [ ] **Step 6: Commit**

```bash
git add shaders/composite.* extension/src/render/composite_pass.* extension/src/raymarch_compositor.* extension/src/register_types.cpp extension/src/voxel_world.cpp demo/
git commit -m "feat(render): compositor + depth composite pass"
```

---

### Task 12: Fly camera, HUD, benchmark

**Files:**
- Modify: `demo/fly_camera.gd`, `demo/hud.gd`, `demo/main.tscn`
- Create: `demo/benchmark.gd`

**Interfaces:**
- Consumes: `demo/main.tscn` (Task 11).
- Produces: mouse-look + WASD fly camera (Esc toggles capture); HUD with FPS/frame-ms; benchmark mode (user arg `--benchmark`): fixed camera, 300 frames, prints `BENCH frame_avg_ms=<x> fps=<y>`, warns if >16.6ms.

- [ ] **Step 1: fly_camera.gd**

```gdscript
extends Camera3D

@export var speed := 10.0
@export var look_sensitivity := 0.0025

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		rotate_y(-event.relative.x * look_sensitivity)
		rotate_object_local(Vector3.RIGHT, -event.relative.y * look_sensitivity)
		rotation.x = clampf(rotation.x, -1.45, 1.45)
	if event.is_action_pressed("ui_cancel"):
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED else Input.MOUSE_MODE_CAPTURED

func _process(delta: float) -> void:
	var dir := Vector3.ZERO
	if Input.is_key_pressed(KEY_W): dir -= transform.basis.z
	if Input.is_key_pressed(KEY_S): dir += transform.basis.z
	if Input.is_key_pressed(KEY_A): dir -= transform.basis.x
	if Input.is_key_pressed(KEY_D): dir += transform.basis.x
	if Input.is_key_pressed(KEY_E): dir += Vector3.UP
	if Input.is_key_pressed(KEY_Q): dir -= Vector3.UP
	if dir.length_squared() > 0.0:
		var boost := 4.0 if Input.is_key_pressed(KEY_SHIFT) else 1.0
		position += dir.normalized() * speed * boost * delta
```

- [ ] **Step 2: hud.gd**

```gdscript
extends Label

func _process(_delta: float) -> void:
	var fps := Engine.get_frames_per_second()
	var ms := 1000.0 / maxf(float(fps), 0.001)
	text = "%d fps  (%.1f ms)" % [fps, ms]
```

- [ ] **Step 3: benchmark.gd + scene wiring**

`demo/benchmark.gd`:

```gdscript
extends Node
# Runs only when `--benchmark` is passed after `--` on the command line.

const FRAMES := 300

var _frames := 0
var _accum_ms := 0.0
var _active := false

func _ready() -> void:
	if "--benchmark" in OS.get_cmdline_user_args():
		_active = true
		var cam: Camera3D = get_parent().get_node("Camera3D")
		cam.transform = Transform3D(Basis.looking_at(Vector3(6, -10, 6).normalized()), Vector3(24, 12, 24))
		cam.set_script(null) # freeze: no fly-camera movement

func _process(_delta: float) -> void:
	if not _active:
		return
	_frames += 1
	_accum_ms += 1000.0 / maxf(float(Engine.get_frames_per_second()), 0.001)
	if _frames >= FRAMES:
		var avg := _accum_ms / FRAMES
		print("BENCH frame_avg_ms=%.2f fps=%.1f" % [avg, 1000.0 / avg])
		if avg > 16.6:
			push_warning("BENCH: frame budget exceeded (target 16.6ms)")
		get_tree().quit()
```

In `demo/main.tscn`: add `demo/benchmark.gd` as `ext_resource` id `3`, bump `load_steps` from 7 to 8, and add the node:

```
[node name="Benchmark" type="Node" parent="."]
script = ExtResource(3)
```

- [ ] **Step 4: Verify**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn` — fly around (WASD+mouse, Shift boost), cave visible, HUD shows fps.
Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn -- --benchmark` — prints `BENCH frame_avg_ms=...` and exits 0.
Expected: benchmark completes; on the RTX 4070 this small world should sit well under 16.6ms.

- [ ] **Step 5: Commit**

```bash
git add demo/
git commit -m "feat(demo): fly camera, HUD, benchmark"
```

---

## M1 Acceptance Checklist

- `cd extension && scons test` — native suite green
- `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests` — gdUnit suite green (boot, GPU smoke, GpuWorld, raymarch pixel)
- `godot --path . demo/main.tscn` — flyable raymarched terrain, correct occlusion vs TestCube, no validation errors
- `godot --path . demo/main.tscn -- --benchmark` — prints BENCH line under 16.6ms
