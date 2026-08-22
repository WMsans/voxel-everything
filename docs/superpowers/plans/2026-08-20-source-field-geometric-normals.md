# Source-Field Geometric Normals Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the curled R8-normal quantization pattern from all raymarched voxel surfaces without material normal maps, an R16 near-field atlas, or a frame-time regression.

**Architecture:** The refined terrain hit is shaded with an analytic gradient evaluated from the procedural base and ordered CSG source rather than by differentiating the R8 render atlas. Extracted and consolidated fields capture their source gradient as compact octahedral normals in a fixed 32 MiB pool; live islands reuse their authoritative volume slot instead of duplicating SDF/material bytes in `IslandAtlas`. The existing R8 atlas remains the sole traversal and hit-position representation.

**Tech Stack:** Godot 4.7.1, godot-cpp GDExtension, C++20, GLSL 460/Vulkan RenderingDevice, doctest 2.4.11, gdUnit4 6.2.1.

**Spec:** `docs/superpowers/specs/2026-08-20-geometric-normal-artifact-design.md`

## Global Constraints

- Keep the default near-field SDF atlas `R8_UNORM`, 1088×544×544, and byte-for-byte unchanged.
- Material texture normal channels remain unused; only geometric normals drive shading.
- Compact-normal render-device allocation is fixed at **32 MiB maximum** and never grows implicitly.
- The normal pool is fail-soft: a missing/exhausted normal allocation uses the documented wide R8 fallback and cannot reject geometry, edits, or islands.
- Replace the current six trilinear SDF normal probes (48 random atlas reads); do not add a full-screen pass.
- Keep `ve::EditOp` exactly 32 bytes and preserve operation ordering and tie behavior.
- Keep Vulkan push constants at or below 128 bytes and record `buffer_update` before opening a compute list.
- Pure C++ code under `generator/`, `world/`, and `shade/` contains no Godot types.
- CPU and GLSL gradient paths must use identical formulas and branch comparisons.
- Target: RMS `N·L <= 0.001`, incorrect cel band pixels below 0.1% away from analytic boundaries, and no repeated connected curl component.
- The effects-off M7 benchmark's median and p95 raymarch time may not regress by more than 3%.
- Use TDD for every task and make the listed focused commit after its tests pass.

---

## File Structure

```text
extension/src/
  generator/
    generator.h/.cpp              analytic base gradient API
    edit_ops.h/.cpp               gradient-bearing CSG and VolumeStore API
    volume_set.h/.cpp             compact normals in VolumeData and resampling
  shade/
    oct.h/.cpp                    signed-byte oct pack/unpack
  world/
    brick_eval.h/.cpp             eval_field_gradient CPU entry point
    field_source_snapshot.h/.cpp  bounded worker-safe overrides/volumes for exact gradients
    override_store.h/.cpp         optional compact normals per used override
    normal_range_allocator.h/.cpp pure fixed-capacity range allocator
  render/
    stored_normal_pool.h/.cpp     32 MiB total GPU pool including both offset tables
    volume_pool.h/.cpp            authoritative volume SDF/material uploads remain here
    island_atlas.h/.cpp           mip/descriptors only; no duplicate volume pool
    island_extract_pass.h/.cpp    extraction readback includes oct normals
    consolidate_pass.h/.cpp       CPU source snapshots produce oct normals after GPU value bake
    gpu_atlas.h/.cpp              owns StoredNormalPool
    raymarch_pass.cpp             binds shared volumes, overrides, and normal pool
  physics/
    island_manager.cpp            publishes source volume slot in island descriptor
  voxel_world.h/.cpp              uploads/releases normals, telemetry, debug probes
shaders/
  shade.glslh                     signed-byte oct pack/unpack mirror
  field.glslh                     analytic field/CSG gradient mirror
  field_gradient_probe.comp.glsl  focused CPU/GPU differential fixture
  island_extract.comp.glsl        captures masked-field compact normals
  raymarch.comp.glsl              source-field normal at refined terrain/island hit
extension/tests/
  test_field_gradient.cpp
  test_stored_normals.cpp
  test_normal_range_allocator.cpp
tests/
  test_field_gradient.gd
  test_normal_artifact.gd
  test_island_extract.gd           extended
  test_island_render.gd            extended
  test_consolidation.gd            extended
  test_raymarch_gbuffer.gd         remove obsolete narrow-strip assertion
```

## Shared Interfaces

These names are fixed for every task:

```cpp
namespace ve {
struct FieldSample {
    float sdf = 0.0f;
    uint16_t material = 0;
    float gradient[3] = {0.0f, 1.0f, 0.0f};
    bool exact_gradient = false;
};

uint16_t oct_encode_snorm8(const float normal[3]);
void oct_decode_snorm8(uint16_t packed, float normal[3]);

FieldSample eval_field_gradient(const Generator &gen, const EditOp *ops, int op_count,
    float x, float y, float z, const VolumeStore *volumes = nullptr,
    const OverrideSource *overrides = nullptr);
}
```

On the GPU, `eval_field_gradient` has the mirror signature:

```glsl
void eval_field_gradient(vec3 p, uint op_base, uint op_count,
        out float sdf, out uint mat, out vec3 gradient, out bool exact_gradient);
```

`StoredNormalPool` offsets are signed sample indices: `-1` means no compact normals. Shader
normal words pack two `uint16` samples per `uint`; all byte offsets are four-byte aligned.

---

### Task 1: Replace the False Regression with a Full-Surface Artifact Gate

**Files:**
- Modify: `extension/src/voxel_world.h` (debug declaration)
- Modify: `extension/src/voxel_world.cpp` (binding and probe implementation)
- Create: `tests/test_normal_artifact.gd`
- Modify: `tests/test_raymarch_gbuffer.gd` (remove the narrow 61-point step test if present)
- Modify: `tests/test_island_render.gd` (remove the narrow island-strip test if present)

**Interfaces:**
- Consumes: existing `RaymarchPass::render`, `surface_texture()`, and `hitpos_texture()`.
- Produces: `Dictionary VoxelWorld::debug_raymarch_normal_probe(Vector3 origin, Vector3 dir, int width, int height)` with `ran`, `hits`, `rms_ndl`, `cel_mismatch_fraction`, and `largest_mismatch_component`.

- [ ] **Step 0: Capture the current wider-tap performance baseline before editing files**

Run the current worktree three times with all beauty effects disabled and retain the generated
JSON/CSV paths in the execution notes:

```bash
tools/run_benchmarks.sh normal-r8-current-1 --effects-off=ssgi,ssr,contact_shadows,outlines,sun_shadow_map,glossy_sdf_rays,raymarched_sun_shadow
tools/run_benchmarks.sh normal-r8-current-2 --effects-off=ssgi,ssr,contact_shadows,outlines,sun_shadow_map,glossy_sdf_rays,raymarched_sun_shadow
tools/run_benchmarks.sh normal-r8-current-3 --effects-off=ssgi,ssr,contact_shadows,outlines,sun_shadow_map,glossy_sdf_rays,raymarched_sun_shadow
```

Record median-of-runs `gpu_raymarch` median/p95 and `custom_frame`. This is the reproducible
baseline for Task 8; it includes the currently unstaged wider finite-difference attempt that the
user reports still shows the bands.

- [ ] **Step 1: Add the failing broad-view gdUnit test**

Create `tests/test_normal_artifact.gd`:

```gdscript
extends GdUnitTestSuite

var _worlds: Array[VoxelWorld] = []

func after_test() -> void:
    for world in _worlds:
        if is_instance_valid(world):
            world.free()
    _worlds.clear()

func make_world() -> VoxelWorld:
    var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
    world.use_local_device = true
    world.physics_enabled = false
    world.atlas_bricks = Vector3i(32, 16, 32)
    world.max_region_slots = 64
    world.world_origin_bricks = Vector3i(0, -64, 0)
    world.world_size_regions = Vector3i(8, 5, 8)
    add_child(world)
    _worlds.append(world)
    assert_bool(world.debug_init_atlas()).is_true()
    var quiet := 0
    for frame in range(500):
        quiet = quiet + 1 if world.debug_stream_frame(Vector3(20, 56, 20)) == 0 else 0
        if quiet >= 6:
            break
    return world

func test_a_broad_terrain_patch_has_no_r8_normal_curls(timeout := 90000) -> void:
    var world := make_world()
    world.set_effect_enabled("ssgi", false)
    world.set_effect_enabled("ssr", false)
    world.set_effect_enabled("contact_shadows", false)
    world.set_effect_enabled("outlines", false)
    world.set_effect_enabled("sun_shadow_map", false)
    world.set_effect_enabled("glossy_sdf_rays", false)
    world.set_effect_enabled("raymarched_sun_shadow", false)
    var result := world.debug_raymarch_normal_probe(
        Vector3(20.0, 72.0, 29.0), Vector3(0.1, -0.85, -0.5).normalized(), 256, 192)
    assert_bool(result.get("ran", false)).is_true()
    assert_int(result.get("hits", 0)).is_greater(12000)
    assert_float(result.get("rms_ndl", 1.0)).is_less_equal(0.001)
    assert_float(result.get("cel_mismatch_fraction", 1.0)).is_less(0.001)
    assert_int(result.get("largest_mismatch_component", 999999)).is_less_equal(8)
```

- [ ] **Step 2: Run the test to verify the debug API is missing**

Run: `./gdunit_tests.sh -c -a res://tests/test_normal_artifact.gd`

Expected: FAIL because `debug_raymarch_normal_probe` is not bound.

- [ ] **Step 3: Implement the diagnostic without changing normal generation**

Bind the method beside `debug_raymarch_hole_probe`. Implement it by rendering the requested
view once, reading `surface` and `hitpos`, decoding each hit normal, and comparing it with
`AnalyticGenerator::sample_gradient(hit.x, hit.y, hit.z)`. Use the existing fixed sun direction
and cel edges:

```cpp
constexpr float kSun[3] = {0.5746958f, 0.7662610f, 0.2873479f};
constexpr float kEdges[3] = {0.08f, 0.32f, 0.66f};
auto band = [](float ndl) { return ndl > 0.66f ? 3 : ndl > 0.32f ? 2 : ndl > 0.08f ? 1 : 0; };
```

Count cel mismatches only when the analytic `N·L` is at least `0.01` from every edge. Build a
four-neighbour flood fill over the mismatch mask and report its largest component. Ignore misses
and samples where `sample_gradient().exact_gradient` is false. Return `ran = true` only after
both readbacks have the expected byte counts.

- [ ] **Step 4: Rebuild and prove the real artifact fails**

Run: `./build.sh -j$(nproc) && ./gdunit_tests.sh -c -a res://tests/test_normal_artifact.gd`

Expected: FAIL with `rms_ndl` near the reproduced R8 error (~0.009) and/or a large connected
mismatch component. Save the exact metrics in the commit message body or plan execution notes.

- [ ] **Step 5: Remove the obsolete narrow-strip assertions**

Delete `test_smooth_terrain_normals_do_not_expose_the_voxel_lattice` and its local analytic
helper from `tests/test_raymarch_gbuffer.gd` if they are present. Keep all unrelated G-buffer
tests. The broad-view test is the replacement because the narrow test passed while the screenshot
remained visibly wrong.

Delete `test_smooth_island_normals_do_not_expose_either_voxel_lattice`, `island_normal_step`, and
their now-unused analytic helper/constants from `tests/test_island_render.gd`. Keep every island
hit, transform, lifetime, and material assertion; Task 7 adds the broad island replacement.

- [ ] **Step 6: Commit the red regression**

```bash
git add extension/src/voxel_world.h extension/src/voxel_world.cpp \
  tests/test_normal_artifact.gd tests/test_raymarch_gbuffer.gd tests/test_island_render.gd
git commit -m "test: reproduce full-surface R8 normal curls"
```

---

### Task 2: CPU Analytic Field and Ordered-CSG Gradients

**Files:**
- Modify: `extension/src/generator/generator.h`
- Modify: `extension/src/generator/generator.cpp`
- Modify: `extension/src/generator/edit_ops.h`
- Modify: `extension/src/generator/edit_ops.cpp`
- Modify: `extension/src/world/brick_eval.h`
- Modify: `extension/src/world/brick_eval.cpp`
- Modify: `extension/src/world/override_store.h`
- Modify: `extension/src/world/override_store.cpp`
- Create: `extension/tests/test_field_gradient.cpp`

**Interfaces:**
- Consumes: `Sample`, `EditOp`, `VolumeStore`, and `OverrideSource`.
- Produces: `FieldSample`, `Generator::sample_gradient`, `apply_op_gradient`, `apply_ops_gradient`, `OverrideSource::sample_gradient`, and `eval_field_gradient` with the signatures in Shared Interfaces.

- [ ] **Step 1: Write failing analytic and CSG unit tests**

Create `extension/tests/test_field_gradient.cpp`. Cover these exact cases:

```cpp
TEST_CASE("analytic hill gradient matches a double precision difference") {
    ve::AnalyticGenerator gen;
    const float x = 25.73f, z = 26.41f;
    const ve::FieldSample got = gen.sample_gradient(x, 55.0f, z);
    const double e = 1e-4;
    const double dx = (gen.sample(x + e, 55.0, z).sdf - gen.sample(x - e, 55.0, z).sdf) / (2 * e);
    const double dz = (gen.sample(x, 55.0, z + e).sdf - gen.sample(x, 55.0, z - e).sdf) / (2 * e);
    CHECK(got.exact_gradient);
    CHECK(got.gradient[0] == doctest::Approx(dx).epsilon(2e-3));
    CHECK(got.gradient[1] == doctest::Approx(1.0f));
    CHECK(got.gradient[2] == doctest::Approx(dz).epsilon(2e-3));
}

TEST_CASE("CSG keeps the previous gradient on an exact tie") {
    ve::FieldSample base{};
    base.sdf = -1.0f;
    base.gradient[0] = 0.0f; base.gradient[1] = 1.0f; base.gradient[2] = 0.0f;
    base.exact_gradient = true;
    ve::EditOp add{};
    add.type = ve::kOpSphereAdd;
    add.radius = 1.0f;
    const ve::FieldSample got = ve::apply_op_gradient(base, add, 0, 0, 0, nullptr);
    CHECK(got.gradient[1] == doctest::Approx(1.0f));
}
```

Also test the cave's negated sphere branch, sphere add/subtract, sphere paint preserving the
gradient, and box subtract on an outside face, inside face, edge, and corner. The deterministic
box tie rule is axis order X, then Y, then Z.

- [ ] **Step 2: Run native tests to verify missing types/functions**

Run: `cd extension && scons test`

Expected: compilation FAIL for `FieldSample` and `sample_gradient`.

- [ ] **Step 3: Add `FieldSample` and the generator gradient contract**

Place `FieldSample` beside `Sample` in `generator.h`. Add:

```cpp
virtual FieldSample sample_gradient(float x, float y, float z) const;
```

The base implementation samples the value at the point and uses a 1 cm central difference with
`exact_gradient = false`; this is only a fail-soft contract for custom generators. Override it in
`AnalyticGenerator` with the closed forms:

```cpp
const float dhdx = 0.66f * cosf(x * 0.11f) * cosf(z * 0.13f)
        + 0.093f * cosf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
        + 0.23f * cosf(x * 0.23f + z * 0.19f);
const float dhdz = -0.78f * sinf(x * 0.11f) * sinf(z * 0.13f)
        + 0.129f * sinf(x * 0.031f + 1.7f) * cosf(z * 0.043f)
        + 0.19f * cosf(x * 0.23f + z * 0.19f);
```

Start with `gradient = {-dhdx, 1, -dhdz}`. When `-sphere > terrain_sdf`, replace it with
`-(p - cave_center) / length(p - cave_center)`. A zero-length sphere vector uses `(0, 1, 0)` and
sets `exact_gradient = false`.

- [ ] **Step 4: Implement gradient-bearing ordered CSG**

Declare and implement:

```cpp
FieldSample apply_op_gradient(FieldSample s, const EditOp &op, float x, float y, float z,
        const VolumeStore *volumes = nullptr);
FieldSample apply_ops_gradient(FieldSample s, const EditOp *ops, int count,
        float x, float y, float z, const VolumeStore *volumes = nullptr);
```

Use the same strict comparisons as `apply_op`: subtract replaces on `candidate > s.sdf`, add
replaces on `candidate < s.sdf`, and ties keep the previous branch. Sphere gradients are the
normalized centre-to-point vector; subtract negates it. Implement `box_sdf_gradient` beside
`box_sdf`: outside the box, normalize the vector of positive signed extents with the original
point signs; inside, select the largest signed extent using X/Y/Z tie priority and return that
axis sign. Negate it for box subtraction.

Add a non-pure default to `VolumeStore`:

```cpp
virtual bool sample_gradient(int, float, float, float, const EditOp &, FieldSample *) const {
    return false;
}
```

If a winning volume has no gradient, keep its value/material, compute no invented exact normal,
set its gradient to `(0, 0, 0)`, and return `exact_gradient = false`.

- [ ] **Step 5: Add the world-level gradient entry point**

Add a default
`OverrideSource::sample_gradient(float x, float y, float z, FieldSample *out) const` returning
false. Implement
`eval_field_gradient` in `brick_eval.cpp`: use an override gradient if present, otherwise the
generator gradient, then call `apply_ops_gradient` in order. Do not change `eval_field`, brick
generation, collision, or traversal.

- [ ] **Step 6: Run the native suite**

Run: `cd extension && scons test`

Expected: PASS, including all new field-gradient cases.

- [ ] **Step 7: Commit**

```bash
git add extension/src/generator/generator.h extension/src/generator/generator.cpp \
  extension/src/generator/edit_ops.h extension/src/generator/edit_ops.cpp \
  extension/src/world/brick_eval.h extension/src/world/brick_eval.cpp \
  extension/src/world/override_store.h extension/src/world/override_store.cpp \
  extension/tests/test_field_gradient.cpp
git commit -m "feat: evaluate analytic gradients through ordered CSG"
```

---

### Task 3: Compact Oct Normals in CPU Stored Fields

**Files:**
- Modify: `extension/src/shade/oct.h`
- Modify: `extension/src/shade/oct.cpp`
- Modify: `extension/src/generator/edit_ops.h`
- Modify: `extension/src/generator/volume_set.h`
- Modify: `extension/src/generator/volume_set.cpp`
- Modify: `extension/src/world/override_store.h`
- Modify: `extension/src/world/override_store.cpp`
- Create: `extension/tests/test_stored_normals.cpp`
- Modify: `extension/tests/test_volume_ops.cpp`
- Modify: `extension/tests/test_override_store.cpp`

**Interfaces:**
- Consumes: Task 2 `FieldSample` and gradient-bearing store hooks.
- Produces: `oct_encode_snorm8`, `oct_decode_snorm8`, `VolumeData::normal_oct`, `sample_volume_gradient_lattice`, and optional `OverrideBrick::normal_oct`.

- [ ] **Step 1: Write failing codec and stored-volume tests**

Create `test_stored_normals.cpp` with a dense sphere-direction loop. Encode and decode every
normalized `(x, y, z)` for integer coordinates in `[-16, 16]`, excluding zero, and require
`dot(input, decoded) > 0.999`. Add malformed payload checks:

```cpp
ve::VolumeData data;
data.dim = 4;
data.sdf.assign(64, ve::encode_sdf(1.0f));
data.mat.assign(64, 0);
CHECK(data.valid());                 // empty normal payload selects fallback
data.normal_oct.assign(63, 0);
CHECK_FALSE(data.valid());           // partial payload is corrupt
data.normal_oct.assign(64, ve::oct_encode_snorm8(up));
CHECK(data.valid());
CHECK(data.has_normals());
```

Test trilinear interpolation of eight compact normals, rigid rotation during resampling, and an
override returning its compact normal rather than differentiating R8.

- [ ] **Step 2: Run native tests and verify failure**

Run: `cd extension && scons test`

Expected: compilation FAIL for the compact codec and `normal_oct`.

- [ ] **Step 3: Implement the signed-byte oct codec**

Encode the existing float oct result with `round(clamp(v, -1, 1) * 127)`, never emitting -128:

```cpp
uint16_t oct_encode_snorm8(const float n[3]) {
    float e[2]; oct_encode(n, e);
    const int8_t x = static_cast<int8_t>(std::lround(std::clamp(e[0], -1.0f, 1.0f) * 127.0f));
    const int8_t y = static_cast<int8_t>(std::lround(std::clamp(e[1], -1.0f, 1.0f) * 127.0f));
    return static_cast<uint8_t>(x) | (static_cast<uint16_t>(static_cast<uint8_t>(y)) << 8);
}
```

Decode each signed byte by `/ 127.0f`, then call `oct_decode`.

- [ ] **Step 4: Extend `VolumeData` without breaking fail-soft legacy fixtures**

Add `std::vector<uint16_t> normal_oct`. `valid()` accepts either zero normals or exactly
`dim^3`; any other size is invalid. Add `has_normals()`. Extend `VolumeStore::sample_gradient`
and `VolumeSet::sample_gradient` to call:

```cpp
bool sample_volume_gradient_lattice(const uint8_t *sdf, const uint8_t *mat,
        const uint16_t *normal_oct, int dim, const float origin[3], float voxel,
        float x, float y, float z, FieldSample *out);
```

Inside the lattice, reuse `sample_volume_lattice` for value/material, decode the eight normals,
trilinearly blend their vectors, and normalize. Outside, return the exact box-distance gradient.
An absent normal pointer returns the value with `exact_gradient = false`.

- [ ] **Step 5: Preserve normals through extraction's CPU mirror and resampling**

In `extract_island_volume`, allocate `normal_oct` and evaluate the masked field's winning
gradient at every sample before SDF quantization. In `resample_volume`, sample the source compact
normal, rotate it by the body's row-major basis into world space, normalize, and encode it into
the output sample. Preserve the existing SDF/material and solid-count behavior.

- [ ] **Step 6: Give used overrides optional normals**

Add `std::vector<uint16_t> normal_oct` to `OverrideBrick`; because `OverrideStore` preallocates
8192 bricks, the vector must remain empty until a brick is actually consolidated. Implement
`OverrideStore::sample_gradient` by calling `sample()` for value/material and trilinearly blending
the brick's 17³ normal lattice when its vector size is exactly `kBrickSdfCount`; otherwise return
false so Task 2 falls back explicitly.

- [ ] **Step 7: Run native tests**

Run: `cd extension && scons test`

Expected: PASS with compact-oct direction error below the stated bound and all existing volume
tests unchanged.

- [ ] **Step 8: Commit**

```bash
git add extension/src/shade/oct.h extension/src/shade/oct.cpp \
  extension/src/generator/edit_ops.h extension/src/generator/volume_set.h \
  extension/src/generator/volume_set.cpp extension/src/world/override_store.h \
  extension/src/world/override_store.cpp extension/tests/test_stored_normals.cpp \
  extension/tests/test_volume_ops.cpp extension/tests/test_override_store.cpp
git commit -m "feat: preserve compact normals in stored fields"
```

---

### Task 4: GLSL Gradient Mirror and CPU/GPU Differential Gate

**Files:**
- Modify: `shaders/shade.glslh`
- Modify: `shaders/field.glslh`
- Create: `shaders/field_gradient_probe.comp.glsl`
- Modify: `extension/src/voxel_world.h`
- Modify: `extension/src/voxel_world.cpp`
- Create: `tests/test_field_gradient.gd`

**Interfaces:**
- Consumes: Tasks 2–3 CPU formulas and compact-oct convention.
- Produces: GLSL `eval_field_gradient` and `VoxelWorld::debug_eval_field_gradient`.

- [ ] **Step 1: Write the failing GPU differential suite**

Create `tests/test_field_gradient.gd` by reusing `test_field_diff.gd`'s deterministic point and
op packing. The output buffer is two `vec4` records per point:

```text
record 0 = sdf, material, gradient.x, gradient.y
record 1 = gradient.z, exact_gradient ? 1 : 0, 0, 0
```

Compare GPU and CPU SDF using the existing two-code-step tolerance. Where both paths report exact
and the sample is more than `0.01` from any CSG tie, normalize gradients and require dot product
above `0.9999`. Include base, cave, sphere subtract/add/paint, box subtract, and ordered chains.

- [ ] **Step 2: Run the suite to verify the probe is absent**

Run: `./gdunit_tests.sh -c -a res://tests/test_field_gradient.gd`

Expected: FAIL loading `field_gradient_probe.comp.glsl`.

- [ ] **Step 3: Mirror the compact codec in GLSL**

Add to `shade.glslh`:

```glsl
uint oct_encode_snorm8(vec3 n) {
    ivec2 q = ivec2(round(clamp(oct_encode(n), vec2(-1.0), vec2(1.0)) * 127.0));
    return (uint(q.x) & 0xFFu) | ((uint(q.y) & 0xFFu) << 8);
}
vec3 oct_decode_snorm8(uint p) {
    int x = int(p & 0xFFu);
    int y = int((p >> 8) & 0xFFu);
    if (x >= 128) x -= 256;
    if (y >= 128) y -= 256;
    ivec2 q = ivec2(x, y);
    return oct_decode(vec2(q) / 127.0);
}
```

- [ ] **Step 4: Add gradient-bearing field helpers**

In `field.glslh`, add `base_field_gradient`, `op_box_sdf_gradient`,
`apply_field_op_gradient`, and `eval_field_gradient`. Copy Task 2's formulas and strict branch
comparisons exactly. Paint leaves the gradient unchanged. Guard stored-volume and override normal
sampling behind `FIELD_NORMAL_*` macros so value-only shaders compile without new bindings.

For a winning stored operand with no compact-normal offset, preserve its value/material and set
`exact_gradient = false`; do not silently substitute `(0,1,0)` while claiming exactness.

- [ ] **Step 5: Add the probe shader and CPU debug hook**

`field_gradient_probe.comp.glsl` binds ops at 0, points at 1, results at 2, and dummy volume
SDF/material at 3/4, matching the existing field probe. It calls `eval_field_gradient` and writes
the two records above. Bind `debug_eval_field_gradient(point, ops, op_count)` beside
`debug_eval_field`; return a dictionary containing `sdf`, `material`, `gradient`, and `exact`.

- [ ] **Step 6: Run focused and legacy differential tests**

Run:

```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_field_gradient.gd
./gdunit_tests.sh -c -a res://tests/test_field_diff.gd
./gdunit_tests.sh -c -a res://tests/test_field_volume_diff.gd
```

Expected: PASS; value-only field results remain unchanged.

- [ ] **Step 7: Commit**

```bash
git add shaders/shade.glslh shaders/field.glslh shaders/field_gradient_probe.comp.glsl \
  extension/src/voxel_world.h extension/src/voxel_world.cpp tests/test_field_gradient.gd
git commit -m "feat: mirror source-field gradients on the GPU"
```

---

### Task 5: Capture Exact Normals During Extraction and Consolidation

**Files:**
- Modify: `shaders/island_extract.comp.glsl`
- Modify: `shaders/brick_consolidate.comp.glsl`
- Modify: `extension/src/render/island_extract_pass.h`
- Modify: `extension/src/render/island_extract_pass.cpp`
- Modify: `extension/src/render/consolidate_pass.h`
- Modify: `extension/src/render/consolidate_pass.cpp`
- Modify: `extension/src/render/mesh_service.cpp`
- Create: `extension/src/world/field_source_snapshot.h`
- Create: `extension/src/world/field_source_snapshot.cpp`
- Modify: `extension/src/voxel_world.h`
- Modify: `extension/src/voxel_world.cpp`
- Modify: `tests/test_island_extract.gd`
- Modify: `tests/test_consolidation.gd`

**Interfaces:**
- Consumes: `FieldSample`, `VolumeData::normal_oct`, `OverrideBrick::normal_oct`, and GLSL compact codec.
- Produces: every successful new extraction/consolidation carries one compact normal per SDF lattice sample.

- [ ] **Step 1: Extend tests to require complete normal payloads**

In the extraction debug result, expose `normal_count`, `normal_min_length`, and
`normal_min_alignment`. Require `normal_count == dim^3`, decoded lengths above `0.99`, and
alignment above `0.98` against `eval_field_gradient` for deterministic samples.

In `test_consolidation.gd`, require every baked override to report
`normal_count == 4913` and compare decoded normals against the CPU gradient at at least 64
deterministic lattice points per brick with dot product above `0.98`. Run this once on a fresh
region and again after publishing the first override plus a volume-add op; the second case proves
that re-consolidation samples both prior override normals and referenced volume normals rather
than differentiating an R8 result.

- [ ] **Step 2: Run both tests to verify missing normals**

Run:

```bash
./gdunit_tests.sh -c -a res://tests/test_island_extract.gd
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
```

Expected: FAIL because both result types contain zero compact normals.

- [ ] **Step 3: Pack island normals into the already-allocated output word**

Include `shade.glslh` in `island_extract.comp.glsl`. Add
`masked_field_gradient`: evaluate the field gradient, compute the minimum box-union distance and
its deterministic gradient, and select the winner of `max(field, box_union)` with the same strict
tie rule. Store the reserved `0x8080` sentinel when the worker shader cannot reach an exact
stored-source gradient; Task 3's encoder never emits -128 in either byte, so the sentinel cannot
collide with a valid normal:

```glsl
float gradient_len = length(gradient);
uint packed_normal = exact_gradient && gradient_len > 1e-8
        ? oct_encode_snorm8(gradient / gradient_len)
        : 0x8080u;
out_vol.v[index] = encode_sdf_byte(sdf)
        | (min(mat, 255u) << 8)
        | (packed_normal << 16);
```

The output remains one 32-bit word per voxel, so neither staging size nor readback size changes.
In `IslandExtractPass::extract`, decode valid high halves into `VolumeData::normal_oct`. For each
sentinel sample, use the job's CPU source snapshot and the same deterministic box-union gradient
to evaluate the masked source exactly, then encode it. If any sentinel cannot be resolved exactly,
clear the whole normal payload so the source uses the explicit R8 fallback.

- [ ] **Step 4: Snapshot worker gradient sources without adding GPU staging**

Create `world/field_source_snapshot.h` with these CPU-only records:

```cpp
namespace ve {
struct LocatedOverride { IVec3 brick{}; OverrideBrick data{}; };
struct SlottedVolume { int slot = -1; VolumeData data{}; };
struct FieldSourceSnapshot {
    std::vector<LocatedOverride> overrides;
    std::vector<SlottedVolume> volumes;
    bool materialize(OverrideStore *override_store, VolumeSet *volume_set) const;
};
}

struct ConsolidateJob {
    ve::IVec3 region{};
    int region_slot = -1;
    uint64_t through_seq = 0;
    std::vector<ve::IVec3> bricks;
    std::vector<ve::EditOp> ops;
    ve::FieldSourceSnapshot source;
};
```

Also add `ve::FieldSourceSnapshot source` to `IslandExtractJob`. Implement
`VoxelWorld::snapshot_field_sources(const std::vector<ve::EditOp> &ops, ve::IVec3 brick_lo,
ve::IVec3 brick_hi, ve::FieldSourceSnapshot *) const`. While `edit_mutex_` is held, copy only prior
overrides inside the inclusive brick range, walk ops in order, deduplicate valid volume-add slots,
and copy only those authoritative `VolumeData` records. Reject job submission if an op names a
missing or malformed volume, exactly as the value path already does; do not capture all 64 slots.

Call the helper from the production and debug consolidation job builders using the min/max of
`job.bricks`. Call it from every `kExtractField` builder using the lattice AABB
`[origin, origin + (dim - 1) * voxel]`; `kResampleVolume` already carries its authoritative source
and needs no snapshot. `FieldSourceSnapshot::materialize` reconstructs temporary stores using the
original volume slots and override brick coordinates, failing transactionally on a duplicate,
invalid slot, malformed payload, or insufficient temporary-store capacity.

This snapshot makes worker evaluation lifetime-safe and keeps all new consolidation storage on
the CPU. Do not add `staging_normal_`: at the current 8192 brick capacity that buffer would
consume another ~80 MiB of device memory.

The existing consolidation value shader does not currently bind the worker's volume pool, so it
silently skips `OP_VOLUME_ADD`. Change `ConsolidatePass::initialize` to accept the existing
`VolumePool *`, define `FIELD_VOLUME_SDF_BINDING=9` and `FIELD_VOLUME_MAT_BINDING=10` in
`brick_consolidate.comp.glsl`, bind those two existing buffers, and pass `&pass.volumes()` from
`MeshService`. This allocates nothing new and makes the value bake use the same referenced-volume
sources as the CPU normal snapshot.

- [ ] **Step 5: Encode consolidation normals from the exact CPU source after readback**

After `ConsolidatePass::run` reads the GPU-baked SDF/material bytes, construct a temporary
`ve::OverrideStore` with `job.source.overrides.size()` slots and a temporary `ve::VolumeSet`, then
materialize both from `job.source`. For every output brick's 17³ world-space lattice points, call
`ve::eval_field_gradient(ve::AnalyticGenerator{}, job.ops.data(),
static_cast<int>(job.ops.size()), x, y, z, &volume_set, &base_store)`, normalize a non-degenerate
exact gradient, and append
`ve::oct_encode_snorm8` to `OverrideBrick::normal_oct`. If any point lacks an exact gradient,
leave the entire brick's normal vector empty so publication takes the explicit R8 fallback; never
mix exact and fabricated samples in one payload.

The GPU still creates SDF/material exactly as before. Normal generation runs on the existing
background consolidation worker after readback, adds no render-device allocation, and evaluates
the same generator, prior override, stored-volume snapshots, and ordered ops that fed the bake.

- [ ] **Step 6: Run extraction, consolidation, and native volume tests**

Run:

```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_island_extract.gd
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
cd extension && scons test
```

Expected: PASS with complete compact-normal payloads and unchanged SDF/material comparisons.

- [ ] **Step 7: Commit**

```bash
git add shaders/island_extract.comp.glsl extension/src/render/island_extract_pass.cpp \
  shaders/brick_consolidate.comp.glsl extension/src/render/consolidate_pass.h \
  extension/src/render/consolidate_pass.cpp extension/src/render/mesh_service.cpp \
  extension/src/render/island_extract_pass.h extension/src/world/field_source_snapshot.h \
  extension/src/world/field_source_snapshot.cpp \
  extension/src/voxel_world.h extension/src/voxel_world.cpp \
  tests/test_island_extract.gd tests/test_consolidation.gd
git commit -m "feat: capture source normals for stored voxel fields"
```

---

### Task 6: Fixed-Capacity Normal Pool and Shared Island Volume Storage

**Files:**
- Create: `extension/src/world/normal_range_allocator.h`
- Create: `extension/src/world/normal_range_allocator.cpp`
- Create: `extension/tests/test_normal_range_allocator.cpp`
- Create: `extension/src/render/stored_normal_pool.h`
- Create: `extension/src/render/stored_normal_pool.cpp`
- Modify: `extension/src/render/gpu_atlas.h`
- Modify: `extension/src/render/gpu_atlas.cpp`
- Modify: `extension/src/render/island_atlas.h`
- Modify: `extension/src/render/island_atlas.cpp`
- Modify: `extension/src/render/raymarch_pass.cpp`
- Modify: `extension/src/physics/island_manager.cpp`
- Modify: `extension/src/voxel_world.h`
- Modify: `extension/src/voxel_world.cpp`
- Create: `tests/test_stored_normal_pool.gd`
- Modify: `tests/test_island_render.gd`

**Interfaces:**
- Consumes: compact-normal payloads from Tasks 3 and 5 and existing authoritative `VolumeSet` slots.
- Produces: `StoredNormalPool::{upload_volume,release_volume,upload_override,release_override}`, three RIDs (`normal_buffer`, `volume_offsets`, `override_offsets`), pool telemetry, and `IslandSlotDesc::volume_slot`.

- [ ] **Step 1: Write the pure allocator tests**

Test exact-fit allocation, aligned splitting, adjacent-block coalescing, reuse, exhaustion,
double-free rejection, and stale-generation rejection. The interface is:

```cpp
struct NormalAllocation { uint32_t offset = 0, size = 0, generation = 0; bool valid() const; };
class NormalRangeAllocator {
public:
    explicit NormalRangeAllocator(uint32_t bytes = 0);
    NormalAllocation allocate(uint32_t bytes, uint32_t alignment = 4);
    bool release(NormalAllocation allocation);
    uint32_t used_bytes() const;
    uint32_t high_water_bytes() const;
};
```

- [ ] **Step 2: Run native tests to verify the allocator is absent**

Run: `cd extension && scons test`

Expected: compilation FAIL for `NormalRangeAllocator`.

- [ ] **Step 3: Implement first-fit allocation with deterministic coalescing**

Maintain free blocks sorted by offset and live allocations keyed by `(offset, generation)`. Round
requested sizes to four bytes, split prefix/suffix fragments, and coalesce both neighbours on
release. Increment the generation whenever an offset is reused. A stale or size-mismatched handle
returns false and leaves state unchanged.

- [ ] **Step 4: Write the failing GPU pool test**

`tests/test_stored_normal_pool.gd` creates a small test world with a 65,536-byte total normal-pool
budget via a debug initializer, uploads three deterministic 17³ override-normal arrays, checks
non-negative distinct offsets, releases one, verifies reuse, exhausts the remaining payload, and
checks:

```gdscript
assert_int(stats["capacity_bytes"]).is_equal(65536)
assert_int(stats["live_bytes"]).is_less_equal(65536)
assert_int(stats["allocation_failures"]).is_equal(1)
assert_int(stats["fallback_hits"]).is_equal(1)
```

- [ ] **Step 5: Implement `StoredNormalPool`**

Treat `budget_bytes` as the hard total across all three GPU buffers. Allocate
`kMaxVolumes * 4` bytes for signed volume sample offsets and `override_capacity * 4` bytes for
signed override offsets, then give the four-byte-aligned remainder to packed normals. Reject
initialization if the metadata does not fit. Initialize both offset tables to `-1`.
`upload_*` validates sample count, reuses an equal-sized live allocation, otherwise allocates
before releasing the old handle so failure preserves valid old data. Upload packed uint16 bytes,
then publish the sample offset. `release_*` publishes `-1` before returning the range.

Expose:

```cpp
struct StoredNormalStats {
    uint32_t capacity_bytes, live_bytes, high_water_bytes;
    uint64_t allocation_failures, fallback_hits;
};
```

Define `fallback_hits` as a CPU-side count of render-reachable sources published with normal
offset `-1` because their payload is absent, malformed, or cannot be allocated. Do not add a
per-pixel atomic or render-target readback to collect this telemetry; both would distort the
performance target. Increment it once when such a source enters fallback and not again every
frame it remains resident.

`capacity_bytes` reports the total budget, while `live_bytes` and `high_water_bytes` report packed
payload use. `GpuAtlas` owns one render-device pool initialized with
`32u * 1024u * 1024u` total bytes and tears it down before freeing the device resources it
references. Thus normal payload plus both offset tables, not merely the payload, is bounded by
32 MiB.

- [ ] **Step 6: Remove `IslandAtlas`'s duplicate `VolumePool`**

Keep island min-max mips, descriptors, and fallback mask. Change `IslandAtlas::upload` to
`upload_mip`, which only builds/uploads the mip from `VolumeData`. `is_valid()` no longer depends
on duplicate SDF/material buffers. Add `int volume_slot` to `IslandSlotDesc` and write it into the
descriptor's unused integer lane.

In `IslandManager::publish_descriptors`, set `d.volume_slot = b->info().volume_slot`. Change
`queue_island_upload` to carry both atlas slot and authoritative volume slot. During drain, upload
SDF/material once into `atlas_->volumes()`, upload its compact normals into
`atlas_->stored_normals()`, and upload the island mip. A field-volume upload follows the same
volume/normal path without an island mip.

On unpinned volume release, queue `StoredNormalPool::release_volume(slot)` on the render thread.
Pinned pasted volumes retain their compact-normal allocation.

- [ ] **Step 7: Bind shared authoritative volume buffers for islands**

In `RaymarchPass::rebuild_targets`, bindings 13 and 14 use
`atlas.volumes().sdf_buffer()` and `atlas.volumes().mat_buffer()` instead of removed IslandAtlas
buffers. The island shader will use `Island.volume_slot` as the buffer stride index in Task 7;
atlas slot continues to select descriptor/mip/tile-mask entries.

- [ ] **Step 8: Run allocator, pool, island, teardown, and memory tests**

Run:

```bash
./build.sh -j$(nproc)
cd extension && scons test && cd ..
./gdunit_tests.sh -c -a res://tests/test_stored_normal_pool.gd
./gdunit_tests.sh -c -a res://tests/test_island_render.gd
./gdunit_tests.sh -c -a res://tests/test_render_shutdown.gd
```

Expected: PASS. Add a memory assertion that the SDF atlas byte count is unchanged and normal pool
capacity is exactly 33,554,432 bytes at default settings.

- [ ] **Step 9: Commit**

```bash
git add extension/src/world/normal_range_allocator.h extension/src/world/normal_range_allocator.cpp \
  extension/tests/test_normal_range_allocator.cpp extension/src/render/stored_normal_pool.h \
  extension/src/render/stored_normal_pool.cpp extension/src/render/gpu_atlas.h \
  extension/src/render/gpu_atlas.cpp extension/src/render/island_atlas.h \
  extension/src/render/island_atlas.cpp extension/src/render/raymarch_pass.cpp \
  extension/src/physics/island_manager.cpp extension/src/voxel_world.h \
  extension/src/voxel_world.cpp tests/test_stored_normal_pool.gd tests/test_island_render.gd
git commit -m "feat: add bounded shared storage for voxel normals"
```

---

### Task 7: Replace R8 Finite-Difference Normals in the Raymarcher

**Files:**
- Modify: `shaders/field.glslh`
- Modify: `shaders/raymarch.comp.glsl`
- Modify: `extension/src/render/raymarch_pass.cpp`
- Modify: `extension/src/voxel_world.h`
- Modify: `extension/src/voxel_world.cpp`
- Modify: `tests/test_field_gradient.gd`
- Modify: `tests/test_normal_artifact.gd`
- Modify: `tests/test_raymarch_gbuffer.gd`
- Modify: `tests/test_island_render.gd`

**Interfaces:**
- Consumes: GLSL source gradient, authoritative volume bindings, compact-normal pool and offsets.
- Produces: source-derived `Hit.n` for static terrain and islands; removes `calc_normal` and its six-step budget charge.

- [ ] **Step 1: Extend the red artifact test to every stored/source-field surface class**

Add four fixtures: sphere add, volume add, a region after forced consolidation, and a
placed/rotated island. For the sphere, sample rays whose winning CSG branch is at least 0.02 m
from a tie and require normal alignment above `0.995`. For volume add, consolidated override, and
island, expose batch probes using their stored compact normals and require RMS `N·L <= 0.003`, cel
mismatch below `0.001`, and no connected repeated-band component larger than 8 pixels. These
thresholds include RG8 oct error but exclude the old R8 SDF curls. Keep the material-normal-byte
invariance assertion in `test_raymarch_gbuffer.gd`.

Upgrade `debug_raymarch_normal_probe` so terrain/edit/volume/override reference normals come from
the CPU `eval_field_gradient` using the hit point's region op span, `VolumeSet`, and
`OverrideStore`; Task 1's procedural-only result must remain unchanged. Add
`debug_island_normal_probe(int island_slot, Vector3 origin, Vector3 dir, int width, int height)`;
it transforms each hit into the body's local lattice, samples `VolumeData::normal_oct`, rotates
the decoded normal by the body's basis, and returns the same five metrics as the terrain probe.

- [ ] **Step 2: Run the tests and verify production still fails**

Run:

```bash
./gdunit_tests.sh -c -a res://tests/test_normal_artifact.gd
./gdunit_tests.sh -c -a res://tests/test_island_render.gd
```

Expected: terrain and/or island artifact assertions FAIL while existing hit/material assertions
continue to pass.

- [ ] **Step 3: Add normal-pool shader bindings**

Use these set-0 bindings after the current cost buffer at 23:

```text
24 stored normal words
25 volume normal sample offsets
26 override normal sample offsets
27 override SDF words
28 override material words
29 override tables
30 override region-to-table map
```

Define `FIELD_VOLUME_SDF_BINDING=13`, `FIELD_VOLUME_MAT_BINDING=14`,
`FIELD_VOLUME_NORMAL_BINDING=24`, `FIELD_VOLUME_NORMAL_OFFSET_BINDING=25`, and the override
bindings above before including `field.glslh`. Remove the raymarcher's duplicate manual op-pool
declaration and let `field.glslh` own binding 10; keep `op_counts` at 11.

Extend `RaymarchPass`'s uniform array from 24 to 31 and bind the RIDs from `GpuAtlas::volumes()`,
`GpuAtlas::stored_normals()`, and `GpuAtlas::overrides()`.

- [ ] **Step 4: Evaluate static terrain normals from the source field**

Replace `calc_normal` with:

```glsl
vec3 terrain_source_normal(vec3 p, ivec3 brick, int anchor_slot, inout int steps_left) {
    int rs = region_slot_of(brick);
    if (rs < 0) return vec3(0.0, 1.0, 0.0);
    float sdf;
    uint mat;
    vec3 gradient;
    bool exact_gradient;
    eval_field_gradient(p, uint(rs) * MAX_REGION_OPS, uint(max(op_counts.n[rs], 0)),
            sdf, mat, gradient, exact_gradient);
    float len = length(gradient);
    if (exact_gradient && len > 1e-8) return gradient / len;
    return terrain_r8_fallback_normal(p, brick, anchor_slot, steps_left);
}
```

At the refined terrain hit, set
`h.n = terrain_source_normal(p, map, anchor_slot, steps_left)`. Remove the unconditional six
`sdf_near` calls, unconditional `steps_left -= 6`, and obsolete widened-tap comment. Keep
`world_sdf` unchanged for traversal/refinement. Retain the current voxel-wide finite difference
and its six-step charge only inside `terrain_r8_fallback_normal`; the common procedural and
compact-normal paths never call it.

- [ ] **Step 5: Decode island normals by authoritative volume slot**

Add `volume_slot` to the shader `Island` struct and descriptor decode. At an island hit, sample
the eight compact normals at the same lattice coordinates/fractions as `island_sdf_at`, blend,
normalize, transform through `isl.basis`, and assign `best.n`. If its offset is `-1` or blended
length is degenerate, use a retained `island_r8_fallback_normal` with the current
voxel-wide taps. This fallback is reachable only for a source whose CPU publication already
incremented `fallback_hits`; do not add a per-pixel atomic or synchronous GPU readback.

- [ ] **Step 6: Upload consolidated override normals transactionally**

When override publication succeeds, call
`StoredNormalPool::upload_override(slot, brick.normal_oct.data(), brick.normal_oct.size())` before
publishing its override table entry. If a
normal upload fails, publish the SDF/material override anyway with normal offset `-1`; increment
allocation/fallback telemetry. Rollback restores the previous normal handle alongside the
previous override bytes/table.

- [ ] **Step 7: Run focused visual and render tests**

Run:

```bash
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests/test_normal_artifact.gd
./gdunit_tests.sh -c -a res://tests/test_raymarch_gbuffer.gd
./gdunit_tests.sh -c -a res://tests/test_island_render.gd
./gdunit_tests.sh -c -a res://tests/test_consolidation.gd
./gdunit_tests.sh -c -a res://tests/test_outline.gd
./gdunit_tests.sh -c -a res://tests/test_ssr.gd
```

Expected: PASS. Record the artifact probe's before/after RMS, mismatch fraction, and largest
component. Confirm changing material normal-map bytes does not change decoded G-buffer normals.

- [ ] **Step 8: Commit**

```bash
git add shaders/field.glslh shaders/raymarch.comp.glsl \
  extension/src/render/raymarch_pass.cpp extension/src/voxel_world.h extension/src/voxel_world.cpp \
  tests/test_field_gradient.gd tests/test_normal_artifact.gd \
  tests/test_raymarch_gbuffer.gd tests/test_island_render.gd tests/test_consolidation.gd
git commit -m "fix: shade voxel surfaces with source-field normals"
```

---

### Task 8: Telemetry, Performance Gate, Full Verification, and Cleanup

**Files:**
- Modify: `extension/src/voxel_world.h`
- Modify: `extension/src/voxel_world.cpp`
- Modify: `demo/hud.gd`
- Modify: `tests/test_gpu_timings.gd`
- Modify: `tests/test_render_shutdown.gd`
- Modify: `docs/superpowers/plans/2026-08-20-source-field-geometric-normals.md` (record measured results in an Errata section)

**Interfaces:**
- Consumes: `StoredNormalStats`, M7 benchmark/capture tools, and all focused tests.
- Produces: public debug telemetry and recorded acceptance evidence.

- [ ] **Step 1: Add failing telemetry and teardown assertions**

Require `debug_perf_stats()` or a focused `debug_stored_normal_stats()` to return:

```text
normal_capacity_bytes
normal_live_bytes
normal_high_water_bytes
normal_allocation_failures
normal_fallback_hits
```

Assert capacity/live/high-water invariants before and after an island spawn, override upload,
pool exhaustion, physics teardown, and render teardown. After teardown all RIDs must be invalid and
reinitialization must restore offset tables to `-1`.

- [ ] **Step 2: Run telemetry tests to verify missing fields**

Run:

```bash
./gdunit_tests.sh -c -a res://tests/test_gpu_timings.gd
./gdunit_tests.sh -c -a res://tests/test_render_shutdown.gd
```

Expected: FAIL for missing normal telemetry.

- [ ] **Step 3: Publish telemetry and HUD diagnostics**

Copy `StoredNormalStats` into the debug dictionary using the exact keys above. Add one compact HUD
line:

```gdscript
"norm %.1f/%.1f MiB hi %.1f fail %d fallback %d" % [
    stats.normal_live_bytes / 1048576.0,
    stats.normal_capacity_bytes / 1048576.0,
    stats.normal_high_water_bytes / 1048576.0,
    stats.normal_allocation_failures,
    stats.normal_fallback_hits,
]
```

- [ ] **Step 4: Run the full native and gdUnit suites**

Run:

```bash
cd extension && scons test && cd ..
./build.sh -j$(nproc)
./gdunit_tests.sh -c -a res://tests
```

Expected: all native and gdUnit tests PASS with no Vulkan validation or shutdown errors. Record
the exact totals in the plan's Errata section.

- [ ] **Step 5: Run and compare the effects-off candidate captures**

Use Task 1's three recorded current-worktree runs as the wider-R8 baseline. Run the completed
candidate three times with the identical effects-off arguments:

```bash
tools/run_benchmarks.sh normal-source-gradient-1 --effects-off=ssgi,ssr,contact_shadows,outlines,sun_shadow_map,glossy_sdf_rays,raymarched_sun_shadow
tools/run_benchmarks.sh normal-source-gradient-2 --effects-off=ssgi,ssr,contact_shadows,outlines,sun_shadow_map,glossy_sdf_rays,raymarched_sun_shadow
tools/run_benchmarks.sh normal-source-gradient-3 --effects-off=ssgi,ssr,contact_shadows,outlines,sun_shadow_map,glossy_sdf_rays,raymarched_sun_shadow
```

Compare median-of-runs `gpu_raymarch` median and p95. More than 3% regression blocks completion.
Also report `custom_frame`, normal pool high-water, allocation failures, and fallback hits. The
expected common-case result is an improvement because 48 random atlas reads were removed.

- [ ] **Step 6: Capture and inspect the original camera symptom**

Run the deterministic capture with all optional effects off, using a fresh temporary user-data
root so existing captures remain untouched:

```bash
ve_normal_capture_root="$(mktemp -d /tmp/ve-normal-final.XXXXXX)"
env XDG_DATA_HOME="$ve_normal_capture_root" XDG_RUNTIME_DIR=/run/user/1000 \
  WAYLAND_DISPLAY=wayland-1 godot --path . --resolution 1280x720 demo/main.tscn -- \
  --capture --effects-off=ssgi,ssr,contact_shadows,outlines,sun_shadow_map,glossy_sdf_rays,raymarched_sun_shadow
```

Stop after the near-field frames have been written. Inspect at least one near terrain frame and
one live-island frame under the printed `$ve_normal_capture_root` path. The nested curls must be
absent; intended broad cel staging boundaries may remain.

- [ ] **Step 7: Exclude generated test artifacts and review the diff**

Do not stage generated untracked `.uid` files and do not touch the user's stash. Run:

```bash
git status --short
git diff --check
git diff --stat HEAD~7
git diff --cached --name-only | rg '\.uid$' && exit 1 || true
```

Verify no R16 atlas format, material-normal sampling, cel-threshold change, or unrelated refactor
entered the diff.

- [ ] **Step 8: Record evidence and commit**

Append an Errata/Results section to this plan containing test totals, artifact before/after
metrics, benchmark medians/p95s, peak normal-pool bytes, fallback count, and inspected capture
frame paths.

```bash
git add extension/src/voxel_world.h extension/src/voxel_world.cpp demo/hud.gd \
  tests/test_gpu_timings.gd tests/test_render_shutdown.gd \
  docs/superpowers/plans/2026-08-20-source-field-geometric-normals.md
git commit -m "test: verify source-field normal quality and cost"
```

- [ ] **Step 9: Request final code review**

Use `superpowers:requesting-code-review` on the complete branch. Address only findings supported
by code/tests, rerun the affected focused suites, then rerun the full suite before claiming the
artifact fixed.

---

## Errata (recorded during implementation — corrections and measured verdicts)

Implementation-time facts, in the style of M1–M7: where this plan's text met reality and lost.

1. **Task 1, Step 0: the wider-tap baseline sweep never happened.** `tools/run_benchmarks.sh` ran
   before the extension had been built, so every leg aborted with `VoxelWorld` unregistered (see
   d1c1d03's commit body). Task 8, Step 5 therefore takes its baseline from the branch base
   `00481c3`, built in a separate worktree, rather than from an unstaged wider-finite-difference
   attempt that no longer exists. What that commit *did* record is the before half of the
   acceptance evidence, and it is reused below.

2. **`tools/run_benchmarks.sh` silently dropped every extra argument.** The script passed `"$@"`
   *before* `demo/main.tscn`, but `demo/benchmark.gd` reads `OS.get_cmdline_user_args()` — only
   what follows `--`. Godot ignores an unknown dashed argument in front of the scene without a
   word, so `tools/run_benchmarks.sh <label> --effects-off=...` measured every effect still
   enabled. Verified both ways on the steady leg: arguments before the scene gave
   `BENCH gpu_ssgi samples=287 p50_ms=0.164`, after `--` they give `BENCH gpu_ssgi samples=0`.
   The script now passes extras after `-- "$leg"`, and both sweeps below use the fixed script.
   Every `--effects-off` benchmark number recorded before this fix, in this plan or M7's, was
   taken with the effects still on.

3. **Task 6 deadlocked every teardown of a world that owned an island.** `release_volume_slot()`
   was changed to take `island_mutex_` so it could queue `StoredNormalPool::release_volume()`,
   but `VoxelWorld::teardown_physics()` calls `IslandManager::teardown()` *while holding that
   mutex*, and teardown releases the volume slot of every body, in-flight extraction and merge.
   Re-entering a non-recursive `std::mutex` hangs, so the first gdUnit suite that spawns an
   island (`test_connectivity.gd`) froze the whole run — 40 minutes parked in
   `pthread_mutex_lock` inside the extension at ~5% of one core. `ptrace_scope=1` means gdb can
   only attach to its own descendants, so the diagnosis needs the process started *under* gdb
   (`timeout -s INT 150 gdb -batch -ex run -ex "thread apply all bt" --args godot ...`). Fixed by
   detaching `island_manager_` under the lock and tearing it down outside it: the render thread
   sees a null manager the instant the lock drops, and the tool thread cannot observe the
   detached pointer because `edit_mutex_` is held throughout.

4. **Task 3 made `ve::resample_volume` ~4.7x more expensive, and a frame-counted test read that
   as a failure.** Resampling a merged body's volume now blends, rotates and re-encodes a compact
   normal per output sample, so a 64³ lattice went from **10.5 ms to 50 ms** on the mesher worker
   (temporary timer around the call: base `11.6 / 25.3 / 12.4 / 10.5 / 10.6 / 10.6 ms`, branch
   `49.2 / 57.5 / 49.7 / 51.1 ms`).
   `test_body_pool_holes_after_merges_do_not_count_against_the_cap` waited `step(w, 240)` for the
   freed slot to be reused, but `step()` runs as fast as the CPU allows (~0.2 ms an iteration), so
   those 240 frames are really a ~50 ms wall-clock budget on work that happens on the worker — and
   the queued window only gets its turn once the merge resample in flight lands. The hole *is*
   reused (measured at ~920 steps), so the wait is now condition-based, like the rest of that
   suite. Left for later: `sample_volume_gradient_lattice()` re-samples the value lattice that
   `resample_volume()` has already sampled, so part of that 4.7x is redundant work.

5. **Task 8, Step 4: full suites.** Native `scons test`: **356 test cases, 4,070,413 assertions,
   0 failed** (doctest 2.4.11). gdUnit: **63 suites, 321 test cases, 0 errors, 0 failures, 0
   flaky, 0 skipped, 0 orphans**, 5 min 35 s. No Vulkan validation errors and no shutdown errors;
   the only `ERROR:` lines in the log are the deliberately broken shader in
   `test_shader_reload.gd`.

6. **Task 8: the artifact gate, before and after.** Same probe, same 256×192 view from
   `(20, 72, 29)` along `(0.1, -0.85, -0.5)`, same 44,998 hits:

   | metric | before (00481c3, R8 taps) | after (source field) | target |
   |---|---|---|---|
   | `rms_ndl` | 0.012834 | **0.000185** | ≤ 0.001 |
   | `cel_mismatch_fraction` | 0.002267 | **0.000000** | < 0.001 |
   | `largest_mismatch_component` | 4 | **0** | ≤ 8 |

7. **Task 8, Step 5: the effects-off sweeps, and a gate breach.** Three runs a side, X11,
   `vsync_actual=disabled`, `verdict_qualified=false`, effects confirmed off
   (`gpu_ssgi samples=0`); medians of the three runs. Note the harness emits only p50/p99 per
   GPU pass — there is no per-pass p95 — so the plan's "median and p95" gate is read here as the
   `gpu_raymarch` p50/p99 plus the frame p95.

   | leg | raymarch p50 | raymarch p99 | frame p95 |
   |---|---|---|---|
   | steady | 15.636 → 15.223 (**−2.6%**) | 18.689 → 18.391 (−1.6%) | 19.44 → 19.44 (+0.0%) |
   | move | 14.349 → 14.066 (−2.0%) | 18.064 → 17.824 (−1.3%) | 19.10 → 18.06 (−5.4%) |
   | ridge | 11.029 → 11.318 (**+2.6%**) | 16.852 → 16.793 (−0.4%) | 18.16 → 18.18 (+0.1%) |
   | edit | 21.208 → 22.457 (**+5.9%**) | 38.257 → 32.253 (−15.7%) | 33.33 → 33.33 (+0.0%) |
   | edit-bounded | 19.445 → 19.099 (−1.8%) | 41.053 → 38.506 (−6.2%) | 27.70 → 27.78 (+0.3%) |
   | island | 12.297 → 11.847 (**−3.7%**) | 18.799 → 18.617 (−1.0%) | 20.37 → 20.37 (+0.0%) |

   **The edit leg's +5.9% raymarch p50 exceeds the plan's 3% gate**, and it is not noise: the
   per-run ranges do not overlap (base 20.594 / 21.208 / 21.339, candidate 22.260 / 22.457 /
   22.597). Ridge (+2.6%) sits just inside the gate on the same mechanism. The human accepted
   this cost explicitly rather than trade it for a Task 7 redesign.

   These are the FINAL sweeps, taken after the code-review fixes in entry 10. An earlier
   sweep of the same candidate before those fixes read steady −4.0%, edit +4.9%, island
   +0.3%; the island leg's improvement to −3.7% is the volume-slot striding fix landing. The plan expected an
   improvement everywhere because 48 random atlas reads were removed, and that is what the steady
   and move legs show — but it costed only the reads, not the replacement. The source-field normal
   pays two new per-hit costs the R8 taps did not: the analytic base gradient's extra
   trigonometry (which is why the unedited **ridge** leg regresses at all), and a walk of the
   region's op list (which is why the op-heavy **edit** leg regresses most, while
   **edit-bounded**, whose ops stay bounded, improves). The p99 of both edit legs improves
   sharply, so the tail is better even where the median is worse. Not fixed here — the shape of
   the fix (per-op AABB rejection at the hit point, as `brick_gen`/`brick_mark` already do
   cooperatively via `op_touches_aabb`, or a cheaper analytic gradient) is a Task 7 design
   decision, not a Task 8 measurement.

8. **Task 8, Step 6: the capture.** 726 frames at 1280×720 into a throwaway user-data root, all
   optional effects off. The near-terrain frames are clean: `frame_00700.png`, `frame_00672.png`,
   `frame_00545.png`, `frame_00530.png` and `frame_00252.png` show smooth shading with only the
   intended broad cel and material staging — no nested curls, no lattice. Those five frames are
   kept in `reports/normal-capture-frames/`; the rest were deleted, because `/tmp` is tmpfs on
   this machine and 860 MB of PNGs held in RAM pushed a later full-suite run into swap (see
   entry 11). The live-island half of
   this step could not be judged from these frames: the canned blasts sit at the camera's look-at
   point tens of metres out, so their islands are a few pixels across. The island evidence is the
   numeric `debug_island_normal_probe` fixture in `test_island_render.gd` instead, which is the
   stronger check anyway.

9. **Task 8: normal-pool telemetry in a live session.** Capacity is exactly **33,554,432 bytes**
   at default settings, as required. A streamed world with four placed islands reports
   `live = high_water = 2,097,152 bytes` (2 MiB, i.e. 6% of budget), `allocation_failures = 0`,
   `fallback_hits = 0`. Exhaustion, reuse and fail-soft behaviour are covered deterministically by
   `test_stored_normal_pool.gd` and `test_gpu_timings.gd` against a shrunk 65,536-byte budget. The
   benchmark harness does not print pool telemetry, so these numbers come from a debug probe, not
   from the sweeps above.

10. **Code review found a rendering bug the whole island fixture set was blind to.** Since Task 6
    the island SDF/material bytes live in the SHARED authoritative volume pool (`raymarch_pass`
    bindings 13/14 = `atlas.volumes()`), uploaded at the **volume** slot — but
    `island_lattice`/`island_sdf_at`/`island_material_at` still strode that pool with the
    **atlas** slot. Only `island_source_normal` had been converted. The two indices come from
    different allocators (32 atlas slots scanned by `IslandManager::free_atlas_slot`, 64 volume
    slots by `VolumeSet::allocate`, the latter also consumed by pinned pasted volumes and by
    `start_merges()`'s out-slot); they agree at startup and diverge permanently after the first
    merge, when the merged body's volume slot is pinned by the paste op while its atlas slot is
    freed. The next island then renders *another body's* geometry under its own transform and
    normals. Plan Task 6, Step 7 had specified exactly this ("the island shader will use
    `Island.volume_slot` as the buffer stride index"); Task 7, Step 5 converted only the normals.

    Every fixture in `test_island_render.gd` places an island with both slots equal
    (`queue_island_upload(slot, slot, d)`, `desc.volume_slot = slot`), so none of them could
    fail on it. The regression test added with the fix,
    `test_an_island_renders_its_own_volume_not_its_atlas_slots`, places a decoy at volume slot 0
    and the subject at atlas 0 / volume 3, and was confirmed to FAIL against the old striding
    before it was made to pass.

    Fixing it surfaced a second bug in the fixture itself: `debug_place_test_island_rotated`
    rebuilds the descriptor array from a GPU readback to preserve other islands, and never read
    back lane 17, so placing a second island silently reset the first island's volume slot.

    The rest of the review's confirmed findings, all fixed here:

    - `StoredNormalPool` was mutated from two threads with no lock — volume spans from the render
      thread (`drain_island_uploads`), override spans from the main thread (`pump_consolidation`),
      plus `stats()` read by the HUD every frame — all touching one allocator, two `std::map`s and
      one counter block. Now serialized by a `std::mutex` (which promptly exposed a self-deadlock:
      the internal fallback path called the newly-locking public `release_*`).
    - `resample_volume` filled a fabricated all-up normal payload when the source had none. That
      payload passes `has_normals()`, uploads, and shades a merged body flat with no `-1` offset
      and no fallback hit to show for it — the one failure mode the fail-soft design cannot
      detect. It now leaves the payload empty (new native test).
    - `GpuAtlas::replay_overrides` re-uploaded SDF/material after an atlas teardown/reinit but not
      compact normals, so every consolidated override in the world silently reverted to R8 taps —
      this feature's own artifact — while telemetry still read `fallback_hits = 0`.
    - Two rollback gaps: `refuse_transaction` released speculative override slots without
      releasing their normal spans (a slow leak out of the fixed 32 MiB pool), and
      `teardown_physics`'s in-flight rollback restored old override bytes while leaving the new
      bake's normals bound to them.
    - `apply_op_gradient`'s `kOpVolumeAdd` branch copied the operand wholesale, overwriting
      material where `apply_op` and the GLSL mirror take it only when the result is solid and the
      operand names one — a CPU/GLSL divergence against the global constraint. The differential
      suite decoded material on both sides and never asserted on it; it does now, and also asserts
      that the gradient comparison was not empty.
    - `debug_poke_material_normal` tilted a single texel of a mipped array layer, so the
      material-normal invariance assertion would have passed whether or not the shader sampled
      those bytes. It now pokes every texel's RG channels.
    - Minor: a wrap guard in `StoredNormalPool::initialize` when the budget rounds down past the
      metadata size, `ok`-gating all three loops of the consolidation normal bake instead of
      `break`ing only the innermost, and a missing `<set>` include.

    Not addressed, and worth a follow-up: `snapshot_field_sources` copies every override in the
    *bounding box* of a job's bricks rather than the bricks themselves (each `OverrideBrick` now
    carries ~9.8 KB of normals on top of its ~9 KB of SDF), `FieldSourceSnapshot::materialize`
    dedupes with O(n²) scans, and the consolidation worker's new cost — 4913 `eval_field_gradient`
    calls per baked brick after readback, which is why `test_consolidation.gd`'s poll budgets were
    raised 4–16× in Task 5 — was never isolated with a timer the way entry 4's resample cost was.

11. **A full-suite run is memory-hungry, and tmpfs is RAM.** The gdUnit suite runs all 63 suites in
    one Godot process and peaked at **6.76 GB RSS**. A run taken while 860 MB of Step 6 capture
    frames sat in `/tmp` (tmpfs) drove the machine into swap — 910 MB out, 6–21% IO wait — and
    every suite slowed without failing: `test_lod_pool.gd` went 10.1 s → **16 min 19 s**,
    `test_lod_gbuffer.gd` 5.2 s → 1 min 29 s, `test_connectivity.gd` 1 min 24 s → 3 min 59 s. It
    reads exactly like a hang and is not one. After deleting the frames the same suite alone ran
    in 10.2 s and the full suite in 5 min 29 s. Do not leave capture output in `/tmp` on this
    machine, and do not read a slow suite as a deadlock without checking `free`/`vmstat` first: a
    real deadlock here parks a single test at ~5% of one core with the main thread in
    `pthread_mutex_lock` (entry 3), while every suite still completing is pressure, not a lock.

12. **Final verification after the review fixes.** Native `scons test`: **357 test cases,
    4,070,420 assertions, 0 failed**. gdUnit: **63 suites, 322 test cases, 0 errors, 0 failures,
    0 flaky, 0 skipped, 0 orphans**, 5 min 29 s. Artifact probe unchanged from entry 6.
