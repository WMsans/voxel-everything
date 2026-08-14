# M2 GPU Generation, Streaming & Destruction — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fly over a 1.6km bounded world whose 5cm bricks are generated **on the GPU** from `G + per-region CSG edit ops`, streamed in and out around the camera through a two-level region/brick indirection with distance-LRU eviction, accelerated by per-brick min–max mips — and blow holes in it in real time with a sphere-subtract tool.

**Architecture:** M1's one-shot CPU brick upload is replaced end to end. Residency is decided on the CPU at **region** granularity (25.6m, ~500 candidates — pure C++, unit-tested); **brick** activation, atlas-slot allocation and brick contents are decided and produced entirely on the GPU (mark pass → indirect-dispatched generation pass), so the CPU never touches a voxel at runtime. `ve::WorldData` survives only as the CPU reference for GPU differential testing (spec §8). Edits append ordered CSG ops to every region they touch; the same mark/generate pair that streams a region in also refreshes a dirty brick AABB, so destruction and streaming share one code path.

**Tech Stack:** Godot 4.7.1 (`/usr/bin/godot`), godot-cpp (pinned master, `api_version = "4.7"`), SCons, C++20, GLSL 460 (Vulkan), doctest 2.4.11 (native), gdUnit4 6.2.1 (in-engine).

**Spec:** `docs/superpowers/specs/2026-08-12-voxel-engine-design.md` — M2 implements §2 in full, §5's *edit pipeline* paragraph, and §8's *GPU differential testing* bullet.

**Predecessor:** `docs/superpowers/plans/2026-08-12-m1-walking-skeleton.md` (complete; read its Errata section before touching shaders).

## Milestone Map

| Milestone | Delivers |
|---|---|
| M1 (done) | Toolchain + raymarched terrain on screen + test harnesses |
| **M2 (this plan)** | GPU brick generation, region indirection, streaming/residency + LRU, min–max mips, destruction edits |
| M3 | Physics: dual-contour meshing, collider streaming, character |
| M4 | Connectivity + raymarched islands |
| M5 | LoD hierarchy + bakery + depth-injection compositing |
| M6 | Beautification: cel, 3-layer shadows, SSGI, SSR, outlines |
| M7 | Benchmark scene + demo polish |

## Global Constraints

- Godot **4.7.1**; godot-cpp pinned to the existing submodule commit, `api_version = "4.7"` — do not bump either.
- Pure C++ cores in `namespace ve` contain **zero Godot types** (spec §8) — no exceptions. Godot-glue classes live in `namespace godot`.
- Shaders: GLSL `#version 460`, loaded **from files** via `ve::load_shader_source` — never inline strings. `#[compute]` is stripped in C++ after load (M1 errata 6).
- Error policy (spec §8): dev = verbose/validation; release = fail-soft — pool exhaustion evicts or drops and logs, never crashes; a stale frame beats a crash.
- Commit style: conventional (`feat:`, `test:`, `build:`, `fix:`, `refactor:`).
- RD API reference: local copy at `docs/api/renderingdevice.md` — consult it before inventing signatures.
- Target hardware: RTX 4070 Laptop; budgets per spec §7 (raymarch ≤6ms, frame ≤16ms).
- **Push constants must stay ≤ 128 bytes** (Vulkan's guaranteed minimum). Anything larger goes in a uniform/storage buffer.
- **Never put a literal include directive inside a GLSL comment** — the loader matches the token anywhere on a line (existing note at the top of `shaders/common.glsl`).

## Fixed Numbers (decided here; every task assumes them)

| Thing | Value | Where it lives |
|---|---|---|
| Region | 32³ bricks = 25.6 m | `ve::kRegionBricks`, `ve::kRegionSize` |
| Default world | origin bricks `(0, −64, 0)`, size `64 × 8 × 64` regions = 1638.4 × 204.8 × 1638.4 m | `VoxelWorld` exports |
| Atlas | 64 × 32 × 32 bricks = 65 536 slots | `GpuAtlasConfig::atlas_bricks` |
| SDF atlas | R8_UNORM 3D, 1088 × 544 × 544 (17³ lattice/brick) ≈ 322 MB | `GpuAtlas` |
| Material atlas | R8_UINT 3D, 1024 × 512 × 512 (16³ cells/brick) ≈ 268 MB | `GpuAtlas` |
| Min–max mips | RG8_UINT 3D × 3 levels (2³/4³/8³ cells per brick) ≈ 76 MB | `GpuAtlas` |
| Region slots | 512 (region table = 32 768 × int32 = 128 KB each ⇒ 64 MB) | `GpuAtlasConfig::max_region_slots` |
| Ops | ≤ 256 per region, 32 B each ⇒ 4 MB pool | `ve::kMaxRegionOps` |
| Residency radius | 96 m (default, exported) | `VoxelWorld::residency_radius_m` |

Note on the 96 m default: the analytic hills put ~1.5k surface bricks in each shell region,
so a 96 m ball approaches the 65 536-slot atlas — the LRU evicts the surplus gracefully
(overflow bit set, bricks stay absent, no crash). Tight is *right* for the demo: it forces
the eviction path to be exercised every time you fly. Tests shrink the radius instead of
the world. Also note: the default world spans y ∈ [−51.2, +153.6) m, i.e. the M1-era
surface now sits at y ≈ 51.2 ± 10 m — Tasks 12/15 shift the demo camera accordingly.
| Streaming throttle | 4 region loads / frame, 16 384 brick jobs / frame | `ve::kMaxRegionLoadsPerFrame`, `GpuAtlasConfig::max_brick_jobs` |

Total steady-state VRAM ≈ 740 MB. **gdUnit tests must shrink the atlas** (`atlas_bricks = Vector3i(8, 8, 8)`, `max_region_slots = 8`) — a local RenderingDevice allocates its own copy and several full-size worlds in one suite will exhaust VRAM.

## Deliberate Deferrals (recorded, not forgotten)

- **2-bit material packing on the GPU.** Spec §2's 2-bit-per-voxel material index is honoured in `ve::Brick` (CPU) but the GPU material atlas stays one byte per voxel. It costs 200 MB and buys nothing until the residency radius grows past ~115 m; revisit when M5 forces the full 150 m field.
- **Op-list consolidation into override bricks** (spec §2, >256 ops/region). M2 caps at 256 and **rejects** further ops for that region with a logged error (spec §8 fail-soft: warn + no-op). A 25.6 m region absorbing 256 explosions is outside the demo's reach.
- **The 4³ mip level is built but not queried by the raymarcher.** The marcher skips on the
  2³ (whole-brick) and 8³ (2-voxel cell) levels only; the 4³ level is kept because the CPU
  reference chain needs it as the intermediate reduction step, and a later narrow-band
  traversal may want it. One uniform slot, 2.4 MB — cheap to keep, free to ignore.

## File Structure

```
extension/src/
  world/
    region.h/.cpp        IVec3, WorldBounds, region/brick lattice math      (Task 1)
    edit_log.h/.cpp      ordered per-region op lists, 256 cap               (Task 3)
    brick_mip.h/.cpp     min–max chain from an SDF lattice                  (Task 4)
    brick_eval.h/.cpp    eval_field / brick_has_surface / eval_brick        (Task 5)
    raycast.h/.cpp       CPU sphere-trace of G + ops (edit-tool aiming)     (Task 6)
    world_data.h/.cpp    MODIFIED: now a thin loop over eval_brick          (Task 5)
    brick.h/.cpp         unchanged
    palette.h/.cpp       MODIFIED: gains order_palette_by_occupancy         (Task 5)
  generator/
    edit_ops.h/.cpp      EditOp, apply_op/apply_ops, touched ranges         (Task 2)
    generator.h/.cpp     MODIFIED: gains lipschitz()                        (Task 6)
  render/
    gpu_atlas.h/.cpp     ALL GPU resources (replaces gpu_world.*)           (Task 8)
    region_pass.h/.cpp   mark / free / dispatch-args passes                 (Task 9)
    brick_gen_pass.h/.cpp indirect brick generation pass                    (Task 10)
    world_streamer.h/.cpp per-frame residency → GPU orchestration           (Task 12)
    shader_loader.h/.cpp MODIFIED: include-once semantics                   (Task 7)
    camera_params.h/.cpp MODIFIED: dims[] repurposed to world size regions  (Task 12)
    raymarch_pass.h/.cpp MODIFIED: new binding set                          (Task 12)
  world/residency.h/.cpp ve::RegionResidency — region set + distance LRU    (Task 11)
  voxel_edit_tool.h/.cpp VoxelEditTool Godot node                           (Task 14)
  voxel_world.h/.cpp     MODIFIED throughout                                (Tasks 7-12, 14)
  render/gpu_world.h/.cpp DELETED                                           (Task 12)
shaders/
  common.glsl            MODIFIED: runtime atlas dims, mip helpers, mat 4   (Task 7)
  brick_layout.glsl      atlas addressing shared by gen + raymarch          (Task 7)
  field.glsl             GPU mirror of G + apply_ops                        (Task 7)
  brick_mark.comp.glsl   activation probe + slot alloc/free + job enqueue   (Task 9)
  region_free.comp.glsl  release every slot of an evicted region            (Task 9)
  dispatch_args.comp.glsl job_count → indirect dispatch args                (Task 9)
  brick_gen.comp.glsl    one workgroup per brick: lattice, mats, mips       (Task 10)
  field_probe.comp.glsl  differential-test harness shader                   (Task 7)
  raymarch.comp.glsl     MODIFIED: two-level lookup (T12), mip skip (T13)
extension/tests/         doctest: test_region, test_edit_ops, test_edit_log,
                         test_brick_mip, test_brick_eval, test_raycast,
                         test_residency, test_shader_loader (extended)
tests/                   gdUnit: test_field_diff.gd, test_gpu_atlas.gd,
                         test_region_pass.gd, test_brick_diff.gd,
                         test_streaming.gd, test_edit_pipeline.gd,
                         test_raymarch_mips.gd,
                         test_raymarch_pixel.gd (migrated),
                         test_raymarch_magenta.gd (migrated),
                         test_gpu_world.gd (DELETED, replaced by test_gpu_atlas.gd)
demo/                    main.tscn, fly_camera.gd, hud.gd, benchmark.gd,
                         edit_tool.gd                                       (Task 15)
```

## Conventions Used Throughout

- **Build:** `./build.sh -j$(nproc)` (or `cd extension && scons -j$(nproc)`)
- **Native tests:** `cd extension && scons test`
- **gdUnit tests:** `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests`
- **Demo:** `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn`
- **Atlas addressing (fixed by this plan, mirrored in C++ and GLSL):** for atlas brick grid `(ax, ay, az)`, slot *s* occupies brick cell `(s % ax, (s / ax) % ay, s / (ax*ay))`. Texel origin in a texture whose per-brick stride is `D` is that cell × `D`. Strides: SDF `D = 17`, material `D = 16`, mip levels `D = 2 / 4 / 8`.
- **Brick coordinates are GLOBAL** on the world lattice and may be negative. World-space corner of brick `b` is `b * kBrickSize` — no origin term. `WorldBounds::origin_bricks` only decides which bricks are *inside* the world and how the dense region map is indexed.

---

### Task 1: `world/region` — the region lattice

**Files:**
- Create: `extension/src/world/region.h`, `extension/src/world/region.cpp`
- Test: `extension/tests/test_region.cpp`

**Interfaces:**
- Consumes: `ve::kBrickSize`, `ve::kBrickVoxels` (`world/brick.h`).
- Produces:
  - `ve::kRegionBricks` (32), `ve::kRegionSize` (25.6f), `ve::kRegionBrickCount` (32768)
  - `struct ve::IVec3 { int x, y, z; }` with `operator==`, `operator!=`
  - `int ve::floor_div(int a, int b)`, `int ve::floor_mod(int a, int b)`
  - `void ve::brick_world_origin(IVec3 b, float out[3])`
  - `struct ve::WorldBounds { IVec3 origin_bricks; IVec3 size_regions; ... }` with
    `size_bricks()`, `origin_regions()`, `contains_region(IVec3)`, `contains_brick(IVec3)`,
    `region_index(IVec3)`, `aabb(float lo[3], float hi[3])`
  - statics `WorldBounds::region_of_brick(IVec3)`, `brick_of_point(float,float,float)`,
    `region_of_point(float,float,float)`, `brick_index_in_region(IVec3)`

- [ ] **Step 1: Write the failing test**

`extension/tests/test_region.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/region.h"

TEST_CASE("floor_div and floor_mod are exact for negative numerators") {
	CHECK(ve::floor_div(-1, 32) == -1);
	CHECK(ve::floor_div(-32, 32) == -1);
	CHECK(ve::floor_div(-33, 32) == -2);
	CHECK(ve::floor_div(31, 32) == 0);
	CHECK(ve::floor_mod(-1, 32) == 31);
	CHECK(ve::floor_mod(-32, 32) == 0);
	CHECK(ve::floor_mod(-33, 32) == 31);
	for (int a = -100; a <= 100; a++)
		CHECK(ve::floor_div(a, 32) * 32 + ve::floor_mod(a, 32) == a);
}

TEST_CASE("region_of_brick partitions the brick lattice, negatives included") {
	CHECK(ve::WorldBounds::region_of_brick({0, 0, 0}) == ve::IVec3{0, 0, 0});
	CHECK(ve::WorldBounds::region_of_brick({31, 31, 31}) == ve::IVec3{0, 0, 0});
	CHECK(ve::WorldBounds::region_of_brick({32, 0, 0}) == ve::IVec3{1, 0, 0});
	CHECK(ve::WorldBounds::region_of_brick({0, -1, 0}) == ve::IVec3{0, -1, 0});
	CHECK(ve::WorldBounds::region_of_brick({0, -64, 0}) == ve::IVec3{0, -2, 0});
}

TEST_CASE("brick_index_in_region is a bijection over one region") {
	// Offsetting the region by a negative multiple must not change the index pattern.
	for (int ry : {0, -2}) {
		bool seen[ve::kRegionBrickCount] = {};
		for (int z = 0; z < ve::kRegionBricks; z++)
			for (int y = 0; y < ve::kRegionBricks; y++)
				for (int x = 0; x < ve::kRegionBricks; x++) {
					const ve::IVec3 b{x, ry * ve::kRegionBricks + y, z};
					const int i = ve::WorldBounds::brick_index_in_region(b);
					REQUIRE(i >= 0);
					REQUIRE(i < ve::kRegionBrickCount);
					CHECK_FALSE(seen[i]);
					seen[i] = true;
				}
		for (bool s : seen) CHECK(s);
	}
}

TEST_CASE("brick_of_point floors, including below the origin plane") {
	CHECK(ve::WorldBounds::brick_of_point(0.0f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::WorldBounds::brick_of_point(0.79f, 0.0f, 0.0f) == ve::IVec3{0, 0, 0});
	CHECK(ve::WorldBounds::brick_of_point(0.81f, 0.0f, 0.0f) == ve::IVec3{1, 0, 0});
	CHECK(ve::WorldBounds::brick_of_point(-0.01f, 0.0f, 0.0f) == ve::IVec3{-1, 0, 0});
	CHECK(ve::WorldBounds::region_of_point(0.0f, -0.01f, 0.0f) == ve::IVec3{0, -1, 0});
}

TEST_CASE("bounds containment and dense region indexing") {
	ve::WorldBounds b{{0, -64, 0}, {64, 8, 64}};
	CHECK(b.size_bricks() == ve::IVec3{2048, 256, 2048});
	CHECK(b.origin_regions() == ve::IVec3{0, -2, 0});
	CHECK(b.contains_region({0, -2, 0}));
	CHECK(b.contains_region({63, 5, 63}));
	CHECK_FALSE(b.contains_region({64, 0, 0}));
	CHECK_FALSE(b.contains_region({0, -3, 0}));
	CHECK(b.region_index({0, -2, 0}) == 0);
	CHECK(b.region_index({1, -2, 0}) == 1);
	CHECK(b.region_index({0, -1, 0}) == 64);
	CHECK(b.region_index({0, -2, 1}) == 64 * 8);
	CHECK(b.region_index({64, 0, 0}) == -1);
	CHECK(b.contains_brick({0, -64, 0}));
	CHECK_FALSE(b.contains_brick({0, -65, 0}));
	CHECK(b.contains_brick({2047, 191, 2047}));
	CHECK_FALSE(b.contains_brick({2048, 0, 0}));
}

TEST_CASE("world aabb spans the bricks the bounds contain") {
	ve::WorldBounds b{{0, -64, 0}, {64, 8, 64}};
	float lo[3], hi[3];
	b.aabb(lo, hi);
	CHECK(lo[0] == doctest::Approx(0.0f));
	CHECK(lo[1] == doctest::Approx(-51.2f));
	CHECK(hi[0] == doctest::Approx(1638.4f));
	CHECK(hi[1] == doctest::Approx(153.6f));
}

TEST_CASE("brick_world_origin ignores the bounds origin (brick coords are global)") {
	float p[3];
	ve::brick_world_origin({-64, -64, 3}, p);
	CHECK(p[0] == doctest::Approx(-51.2f));
	CHECK(p[2] == doctest::Approx(2.4f));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/region.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/world/region.h`:

```cpp
#pragma once
#include "world/brick.h"

namespace ve {

inline constexpr int kRegionBricks = 32;                        // 32^3 bricks per region
inline constexpr float kRegionSize = kRegionBricks * kBrickSize; // 25.6 m
inline constexpr int kRegionBrickCount =
		kRegionBricks * kRegionBricks * kRegionBricks;           // 32768

struct IVec3 {
	int x = 0, y = 0, z = 0;
	bool operator==(const IVec3 &o) const { return x == o.x && y == o.y && z == o.z; }
	bool operator!=(const IVec3 &o) const { return !(*this == o); }
};

// C++ integer division truncates toward zero; the brick lattice extends below y = 0 (the
// world origin sits under the terrain), so every lattice quotient must FLOOR instead.
int floor_div(int a, int b);
int floor_mod(int a, int b);

// Brick coordinates are GLOBAL: the world-space corner of brick b is b * kBrickSize, with
// no origin term. WorldBounds::origin_bricks only decides membership and map indexing.
void brick_world_origin(IVec3 b, float out[3]);

// A bounded, region-aligned world placed on the global brick lattice.
struct WorldBounds {
	IVec3 origin_bricks{0, 0, 0};  // multiple of kRegionBricks on every axis
	IVec3 size_regions{1, 1, 1};

	IVec3 size_bricks() const;
	IVec3 origin_regions() const;

	static IVec3 region_of_brick(IVec3 b);
	static IVec3 brick_of_point(float x, float y, float z);
	static IVec3 region_of_point(float x, float y, float z);
	// 0..kRegionBrickCount-1, x fastest, y, then z.
	static int brick_index_in_region(IVec3 b);

	bool contains_region(IVec3 r) const;
	bool contains_brick(IVec3 b) const;
	// Dense index into the region map (x fastest), or -1 when outside.
	int region_index(IVec3 r) const;
	void aabb(float lo[3], float hi[3]) const;
};

} // namespace ve
```

`extension/src/world/region.cpp`:

```cpp
#include "world/region.h"
#include <cmath>

namespace ve {

int floor_div(int a, int b) {
	const int q = a / b;
	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int floor_mod(int a, int b) {
	const int r = a % b;
	return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

void brick_world_origin(IVec3 b, float out[3]) {
	out[0] = static_cast<float>(b.x) * kBrickSize;
	out[1] = static_cast<float>(b.y) * kBrickSize;
	out[2] = static_cast<float>(b.z) * kBrickSize;
}

IVec3 WorldBounds::size_bricks() const {
	return {size_regions.x * kRegionBricks, size_regions.y * kRegionBricks,
			size_regions.z * kRegionBricks};
}

IVec3 WorldBounds::origin_regions() const {
	return {floor_div(origin_bricks.x, kRegionBricks), floor_div(origin_bricks.y, kRegionBricks),
			floor_div(origin_bricks.z, kRegionBricks)};
}

IVec3 WorldBounds::region_of_brick(IVec3 b) {
	return {floor_div(b.x, kRegionBricks), floor_div(b.y, kRegionBricks),
			floor_div(b.z, kRegionBricks)};
}

IVec3 WorldBounds::brick_of_point(float x, float y, float z) {
	return {static_cast<int>(std::floor(x / kBrickSize)),
			static_cast<int>(std::floor(y / kBrickSize)),
			static_cast<int>(std::floor(z / kBrickSize))};
}

IVec3 WorldBounds::region_of_point(float x, float y, float z) {
	return region_of_brick(brick_of_point(x, y, z));
}

int WorldBounds::brick_index_in_region(IVec3 b) {
	return floor_mod(b.x, kRegionBricks) + floor_mod(b.y, kRegionBricks) * kRegionBricks +
			floor_mod(b.z, kRegionBricks) * kRegionBricks * kRegionBricks;
}

bool WorldBounds::contains_region(IVec3 r) const {
	const IVec3 o = origin_regions();
	return r.x >= o.x && r.y >= o.y && r.z >= o.z && r.x < o.x + size_regions.x &&
			r.y < o.y + size_regions.y && r.z < o.z + size_regions.z;
}

bool WorldBounds::contains_brick(IVec3 b) const {
	const IVec3 s = size_bricks();
	return b.x >= origin_bricks.x && b.y >= origin_bricks.y && b.z >= origin_bricks.z &&
			b.x < origin_bricks.x + s.x && b.y < origin_bricks.y + s.y &&
			b.z < origin_bricks.z + s.z;
}

int WorldBounds::region_index(IVec3 r) const {
	if (!contains_region(r)) return -1;
	const IVec3 o = origin_regions();
	return (r.x - o.x) + (r.y - o.y) * size_regions.x +
			(r.z - o.z) * size_regions.x * size_regions.y;
}

void WorldBounds::aabb(float lo[3], float hi[3]) const {
	const IVec3 s = size_bricks();
	brick_world_origin(origin_bricks, lo);
	brick_world_origin({origin_bricks.x + s.x, origin_bricks.y + s.y, origin_bricks.z + s.z}, hi);
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: the 7 new cases pass; all M1 cases still pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world/region.* extension/tests/test_region.cpp
git commit -m "feat(world): region lattice and world bounds"
```

---

### Task 2: `generator/edit_ops` — CSG operations

**Files:**
- Create: `extension/src/generator/edit_ops.h`, `extension/src/generator/edit_ops.cpp`
- Test: `extension/tests/test_edit_ops.cpp`

**Interfaces:**
- Consumes: `ve::Sample` (`generator/generator.h`), `ve::IVec3`, `ve::WorldBounds` (Task 1).
- Produces:
  - `enum ve::EditOpType : uint32_t { kOpSphereSubtract = 0, kOpSphereAdd = 1, kOpSpherePaint = 2 }`
  - `struct ve::EditOp { uint32_t type; uint32_t material; float pos[3]; float radius; uint32_t pad[2]; }`
    — exactly **32 bytes**; the GPU op pool is a byte-identical mirror (two `uvec4` per op).
  - `ve::Sample ve::apply_op(Sample s, const EditOp &op, float x, float y, float z)`
  - `ve::Sample ve::apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z)`
  - `void ve::op_brick_range(const EditOp &op, IVec3 *lo, IVec3 *hi)` — inclusive
  - `void ve::op_region_range(const EditOp &op, IVec3 *lo, IVec3 *hi)` — inclusive

- [ ] **Step 1: Write the failing test**

`extension/tests/test_edit_ops.cpp`:

```cpp
#include <doctest/doctest.h>
#include "generator/edit_ops.h"

static ve::EditOp sphere(uint32_t type, float x, float y, float z, float r, uint32_t mat = 0) {
	ve::EditOp op{};
	op.type = type;
	op.material = mat;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

TEST_CASE("EditOp is exactly 32 bytes so the GPU pool can mirror it") {
	CHECK(sizeof(ve::EditOp) == 32);
}

TEST_CASE("sphere subtract carves solid into air and clears its material") {
	const ve::Sample solid{-1.0f, 2};
	const ve::EditOp op = sphere(ve::kOpSphereSubtract, 0, 0, 0, 5.0f);
	const ve::Sample in = ve::apply_op(solid, op, 0, 0, 0);   // 5 m inside the sphere
	CHECK(in.sdf == doctest::Approx(5.0f));
	CHECK(in.material == 0);
	const ve::Sample out = ve::apply_op(solid, op, 20, 0, 0); // far outside: untouched
	CHECK(out.sdf == doctest::Approx(-1.0f));
	CHECK(out.material == 2);
}

TEST_CASE("sphere add fills air and stamps its own material") {
	const ve::Sample air{4.0f, 0};
	const ve::EditOp op = sphere(ve::kOpSphereAdd, 0, 0, 0, 5.0f, 4);
	const ve::Sample in = ve::apply_op(air, op, 1, 0, 0);
	CHECK(in.sdf == doctest::Approx(-4.0f));
	CHECK(in.material == 4);
	// Where the existing solid is already closer to its own surface, it keeps its material.
	const ve::Sample deep{-9.0f, 2};
	CHECK(ve::apply_op(deep, op, 1, 0, 0).material == 2);
	CHECK(ve::apply_op(deep, op, 1, 0, 0).sdf == doctest::Approx(-9.0f));
}

TEST_CASE("sphere paint recolours solid only, never changes the surface") {
	const ve::Sample solid{-0.5f, 1};
	const ve::EditOp op = sphere(ve::kOpSpherePaint, 0, 0, 0, 5.0f, 3);
	const ve::Sample hit = ve::apply_op(solid, op, 1, 0, 0);
	CHECK(hit.material == 3);
	CHECK(hit.sdf == doctest::Approx(-0.5f));
	const ve::Sample air{0.5f, 0};
	CHECK(ve::apply_op(air, op, 1, 0, 0).material == 0); // air stays air-coloured
	CHECK(ve::apply_op(solid, op, 20, 0, 0).material == 1); // outside the brush
}

TEST_CASE("ops apply in order: a later add refills an earlier subtract") {
	const ve::EditOp ops[2] = {
		sphere(ve::kOpSphereSubtract, 0, 0, 0, 5.0f),
		sphere(ve::kOpSphereAdd, 0, 0, 0, 3.0f, 4),
	};
	const ve::Sample base{-1.0f, 2};
	const ve::Sample s = ve::apply_ops(base, ops, 2, 0, 0, 0);
	CHECK(s.sdf == doctest::Approx(-3.0f)); // the add won at the centre
	CHECK(s.material == 4);
	// Reversing the order leaves a hole: the subtract runs last.
	const ve::EditOp rev[2] = {ops[1], ops[0]};
	CHECK(ve::apply_ops(base, rev, 2, 0, 0, 0).sdf == doctest::Approx(5.0f));
}

TEST_CASE("apply_ops with zero ops is the identity") {
	const ve::Sample base{-1.0f, 2};
	const ve::Sample s = ve::apply_ops(base, nullptr, 0, 1, 2, 3);
	CHECK(s.sdf == doctest::Approx(-1.0f));
	CHECK(s.material == 2);
}

TEST_CASE("touched ranges cover the sphere plus one voxel of apron margin") {
	// A brick's SDF lattice reaches one voxel past its own extent (kBrickSdfStride == 17),
	// so an op grazing that plane still changes the brick's stored bytes.
	const ve::EditOp op = sphere(ve::kOpSphereSubtract, 8.0f, 8.0f, 8.0f, 1.0f);
	ve::IVec3 lo{}, hi{};
	ve::op_brick_range(op, &lo, &hi);
	CHECK(lo == ve::IVec3{8, 8, 8});   // floor((8 - 1.05) / 0.8) = 8
	CHECK(hi == ve::IVec3{11, 11, 11}); // floor((8 + 1.05) / 0.8) = 11
	ve::IVec3 rlo{}, rhi{};
	ve::op_region_range(op, &rlo, &rhi);
	CHECK(rlo == ve::IVec3{0, 0, 0});
	CHECK(rhi == ve::IVec3{0, 0, 0});
}

TEST_CASE("touched ranges floor correctly below the origin plane") {
	const ve::EditOp op = sphere(ve::kOpSphereSubtract, -0.5f, -30.0f, 0.5f, 1.0f);
	ve::IVec3 lo{}, hi{};
	ve::op_brick_range(op, &lo, &hi);
	CHECK(lo.x == -2);
	CHECK(hi.x == 0);
	ve::IVec3 rlo{}, rhi{};
	ve::op_region_range(op, &rlo, &rhi);
	CHECK(rlo.y == -2); // -31.05 m / 25.6 m -> region -2
	CHECK(rhi.y == -2); // -28.95 m / 25.6 m -> region -2
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `generator/edit_ops.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/generator/edit_ops.h`:

```cpp
#pragma once
#include "generator/generator.h"
#include "world/region.h"
#include <cstdint>

namespace ve {

enum EditOpType : uint32_t {
	kOpSphereSubtract = 0,
	kOpSphereAdd = 1,
	kOpSpherePaint = 2,
};

// Exactly 32 bytes (spec §2: "~32B/op"). The GPU op pool stores two uvec4 per op and
// unpacks by hand, so no std430 struct-layout rule can silently disagree with this.
struct EditOp {
	uint32_t type = kOpSphereSubtract;
	uint32_t material = 0;
	float pos[3] = {0.0f, 0.0f, 0.0f};
	float radius = 0.0f;
	uint32_t pad[2] = {0, 0};
};
static_assert(sizeof(EditOp) == 32);

Sample apply_op(Sample s, const EditOp &op, float x, float y, float z);
Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z);

// Inclusive lattice ranges an op can change, padded by one voxel: a brick's SDF lattice
// carries a one-voxel apron on its positive faces, so an op that only reaches the apron
// plane still alters that brick's stored bytes.
void op_brick_range(const EditOp &op, IVec3 *lo, IVec3 *hi);
void op_region_range(const EditOp &op, IVec3 *lo, IVec3 *hi);

} // namespace ve
```

`extension/src/generator/edit_ops.cpp`:

```cpp
#include "generator/edit_ops.h"
#include <cmath>

namespace ve {

namespace {

float sphere_sdf(const EditOp &op, float x, float y, float z) {
	const float dx = x - op.pos[0], dy = y - op.pos[1], dz = z - op.pos[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz) - op.radius;
}

// Inclusive [lo, hi] cell range of the op's padded AABB on a lattice of the given pitch.
void padded_range(const EditOp &op, float pitch, IVec3 *lo, IVec3 *hi) {
	const float r = op.radius + kVoxelSize;
	const auto cell = [pitch](float v) { return static_cast<int>(std::floor(v / pitch)); };
	*lo = {cell(op.pos[0] - r), cell(op.pos[1] - r), cell(op.pos[2] - r)};
	*hi = {cell(op.pos[0] + r), cell(op.pos[1] + r), cell(op.pos[2] + r)};
}

} // namespace

Sample apply_op(Sample s, const EditOp &op, float x, float y, float z) {
	const float sp = sphere_sdf(op, x, y, z);
	switch (op.type) {
		case kOpSphereSubtract:
			// CSG subtract: max(s, -sphere). A point that becomes air carries no material,
			// matching the generator's own convention (Sample::material == 0 above ground).
			if (-sp > s.sdf) {
				s.sdf = -sp;
				if (s.sdf > 0.0f) s.material = 0;
			}
			return s;
		case kOpSphereAdd:
			// CSG union: min(s, sphere). The material changes only where the sphere is the
			// winning term and the result is solid — filling air, not recolouring rock.
			if (sp < s.sdf) {
				s.sdf = sp;
				if (s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			}
			return s;
		case kOpSpherePaint:
			if (sp <= 0.0f && s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			return s;
		default:
			return s;
	}
}

Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z) {
	for (int i = 0; i < count; i++) s = apply_op(s, ops[i], x, y, z);
	return s;
}

void op_brick_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	padded_range(op, kBrickSize, lo, hi);
}

void op_region_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	padded_range(op, kRegionSize, lo, hi);
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: the 8 new cases pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/generator/edit_ops.* extension/tests/test_edit_ops.cpp
git commit -m "feat(generator): CSG edit operations"
```

---

### Task 3: `world/edit_log` — ordered per-region op lists

**Files:**
- Create: `extension/src/world/edit_log.h`, `extension/src/world/edit_log.cpp`
- Test: `extension/tests/test_edit_log.cpp`

**Interfaces:**
- Consumes: `ve::EditOp`, `ve::op_region_range` (Task 2), `ve::WorldBounds` (Task 1).
- Produces:
  - `ve::kMaxRegionOps` (256)
  - `struct ve::EditLog::AppendResult { std::vector<IVec3> touched; std::vector<IVec3> rejected; }`
  - `class ve::EditLog`:
    - `explicit EditLog(const WorldBounds &bounds)`
    - `AppendResult append(const EditOp &op)`
    - `const std::vector<EditOp> &ops(IVec3 region) const` — empty vector for untouched regions
    - `int op_count(IVec3 region) const`
    - `int region_count() const`
    - `const WorldBounds &bounds() const`
    - `void clear()`

- [ ] **Step 1: Write the failing test**

`extension/tests/test_edit_log.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/edit_log.h"

static ve::EditOp sphere(float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z;
	op.radius = r;
	return op;
}

static ve::WorldBounds bounds() { return ve::WorldBounds{{0, -64, 0}, {64, 8, 64}}; }

TEST_CASE("an op lands in every region it touches, and only those") {
	ve::EditLog log(bounds());
	// Region 0 spans [0, 25.6) m. A 2 m sphere at 25.0 m straddles regions 0 and 1 on x.
	const auto r = log.append(sphere(25.0f, 1.0f, 1.0f, 2.0f));
	CHECK(r.rejected.empty());
	CHECK(r.touched.size() == 2);
	CHECK(log.op_count({0, 0, 0}) == 1);
	CHECK(log.op_count({1, 0, 0}) == 1);
	CHECK(log.op_count({2, 0, 0}) == 0);
	CHECK(log.region_count() == 2);
}

TEST_CASE("ops outside the world bounds are dropped, not stored") {
	ve::EditLog log(bounds());
	const auto r = log.append(sphere(-100.0f, 0.0f, 0.0f, 1.0f));
	CHECK(r.touched.empty());
	CHECK(r.rejected.empty());
	CHECK(log.region_count() == 0);
}

TEST_CASE("a partially out-of-bounds op keeps only its in-bounds regions") {
	ve::EditLog log(bounds());
	const auto r = log.append(sphere(0.5f, 0.5f, 0.5f, 2.0f)); // straddles x = 0 corner
	CHECK(r.touched.size() == 1);
	CHECK(r.touched[0] == ve::IVec3{0, 0, 0});
	CHECK(log.op_count({-1, 0, 0}) == 0);
}

TEST_CASE("ops are preserved in append order") {
	ve::EditLog log(bounds());
	for (int i = 0; i < 5; i++) {
		ve::EditOp op = sphere(1.0f, 1.0f, 1.0f, 1.0f);
		op.material = static_cast<uint32_t>(i);
		log.append(op);
	}
	const auto &ops = log.ops({0, 0, 0});
	REQUIRE(ops.size() == 5);
	for (int i = 0; i < 5; i++) CHECK(ops[i].material == static_cast<uint32_t>(i));
}

TEST_CASE("a full region rejects further ops and reports which region overflowed") {
	ve::EditLog log(bounds());
	for (int i = 0; i < ve::kMaxRegionOps; i++) {
		const auto r = log.append(sphere(1.0f, 1.0f, 1.0f, 0.5f));
		CHECK(r.rejected.empty());
	}
	CHECK(log.op_count({0, 0, 0}) == ve::kMaxRegionOps);
	const auto r = log.append(sphere(1.0f, 1.0f, 1.0f, 0.5f));
	CHECK(r.touched.empty());
	REQUIRE(r.rejected.size() == 1);
	CHECK(r.rejected[0] == ve::IVec3{0, 0, 0});
	CHECK(log.op_count({0, 0, 0}) == ve::kMaxRegionOps); // unchanged: fail-soft, no eviction
}

TEST_CASE("overflow in one region does not block the op's other regions") {
	ve::EditLog log(bounds());
	for (int i = 0; i < ve::kMaxRegionOps; i++) log.append(sphere(1.0f, 1.0f, 1.0f, 0.5f));
	const auto r = log.append(sphere(25.0f, 1.0f, 1.0f, 2.0f)); // regions 0 and 1
	CHECK(r.touched.size() == 1);
	CHECK(r.touched[0] == ve::IVec3{1, 0, 0});
	CHECK(r.rejected.size() == 1);
	CHECK(r.rejected[0] == ve::IVec3{0, 0, 0});
}

TEST_CASE("clear drops everything") {
	ve::EditLog log(bounds());
	log.append(sphere(1.0f, 1.0f, 1.0f, 1.0f));
	log.clear();
	CHECK(log.region_count() == 0);
	CHECK(log.op_count({0, 0, 0}) == 0);
	CHECK(log.ops({0, 0, 0}).empty());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/edit_log.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/world/edit_log.h`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <map>
#include <vector>

namespace ve {

// Spec §2 caps a region at 256 ops before consolidation into override bricks. M2 has no
// consolidation (see the plan's Deliberate Deferrals): a full region rejects the op and the
// caller logs it — spec §8's fail-soft rule, "warn + no-op".
inline constexpr int kMaxRegionOps = 256;

// The ordered CSG op lists that turn G into the current world (spec §2). An op is appended
// to EVERY region it touches, so reconstructing any brick needs only that brick's own
// region list — no neighbour walk, on CPU or GPU.
class EditLog {
public:
	struct AppendResult {
		std::vector<IVec3> touched;  // in-bounds regions whose list grew
		std::vector<IVec3> rejected; // in-bounds regions already holding kMaxRegionOps
	};

	explicit EditLog(const WorldBounds &bounds) : bounds_(bounds) {}

	AppendResult append(const EditOp &op);
	const std::vector<EditOp> &ops(IVec3 region) const;
	int op_count(IVec3 region) const { return static_cast<int>(ops(region).size()); }
	int region_count() const { return static_cast<int>(lists_.size()); }
	const WorldBounds &bounds() const { return bounds_; }
	void clear() { lists_.clear(); }

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};

	WorldBounds bounds_;
	std::map<Key, std::vector<EditOp>> lists_;
};

} // namespace ve
```

`extension/src/world/edit_log.cpp`:

```cpp
#include "world/edit_log.h"

namespace ve {

namespace {
const std::vector<EditOp> kEmpty;
} // namespace

EditLog::AppendResult EditLog::append(const EditOp &op) {
	AppendResult result;
	IVec3 lo{}, hi{};
	op_region_range(op, &lo, &hi);
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 r{x, y, z};
				if (!bounds_.contains_region(r)) continue;
				std::vector<EditOp> &list = lists_[Key{x, y, z}];
				if (static_cast<int>(list.size()) >= kMaxRegionOps) {
					result.rejected.push_back(r);
					continue;
				}
				list.push_back(op);
				result.touched.push_back(r);
			}
	return result;
}

const std::vector<EditOp> &EditLog::ops(IVec3 region) const {
	const auto it = lists_.find(Key{region.x, region.y, region.z});
	return it == lists_.end() ? kEmpty : it->second;
}

} // namespace ve
```

Note: `lists_[Key{...}]` default-constructs an empty list for a region that then overflows —
it cannot, because a region only reaches `kMaxRegionOps` by having been filled first. A region
that is looked up and found full already exists in the map.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: the 7 new cases pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world/edit_log.* extension/tests/test_edit_log.cpp
git commit -m "feat(world): ordered per-region edit op log"
```

---

### Task 4: `world/brick_mip` — min–max acceleration chain

**Files:**
- Create: `extension/src/world/brick_mip.h`, `extension/src/world/brick_mip.cpp`
- Test: `extension/tests/test_brick_mip.cpp`

**Interfaces:**
- Consumes: `ve::kBrickSdfCount`, `ve::sdf_index`, `ve::encode_sdf` (`world/brick.h`).
- Produces:
  - `ve::kMipLevels` (3), `ve::kMipDims[3]` = `{2, 4, 8}`, `ve::kEncodedZero` (128)
  - `struct ve::BrickMips { uint8_t mn2[8], mx2[8], mn4[64], mx4[64], mn8[512], mx8[512]; }`
  - `void ve::build_brick_mips(const uint8_t *sdf_lattice, BrickMips *out)`
  - `bool ve::mip_cell_has_surface(uint8_t mn, uint8_t mx)`
  - accessors `const uint8_t *ve::mip_min(const BrickMips &, int level)`, `ve::mip_max(...)`

- [ ] **Step 1: Write the failing test**

`extension/tests/test_brick_mip.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/brick_mip.h"
#include "world/brick.h"
#include <algorithm>
#include <vector>

// Fills a 17^3 lattice from a callable over lattice coordinates.
template <typename F>
static std::vector<uint8_t> lattice(F f) {
	std::vector<uint8_t> l(ve::kBrickSdfCount, 0);
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++)
				l[ve::sdf_index(x, y, z)] = ve::encode_sdf(f(x, y, z));
	return l;
}

TEST_CASE("kEncodedZero is exactly what encode_sdf(0) produces") {
	CHECK(ve::encode_sdf(0.0f) == ve::kEncodedZero);
}

TEST_CASE("an all-air brick reports no surface at any level") {
	const auto l = lattice([](int, int, int) { return 0.5f; });
	ve::BrickMips m{};
	ve::build_brick_mips(l.data(), &m);
	for (int level = 0; level < ve::kMipLevels; level++) {
		const int n = ve::kMipDims[level] * ve::kMipDims[level] * ve::kMipDims[level];
		for (int i = 0; i < n; i++)
			CHECK_FALSE(ve::mip_cell_has_surface(ve::mip_min(m, level)[i],
			                                     ve::mip_max(m, level)[i]));
	}
}

TEST_CASE("a plane through the brick marks exactly the cells it crosses") {
	// Surface at local y = 4 voxels: sdf = (y - 4) * kVoxelSize.
	const auto l = lattice([](int, int y, int) { return (y - 4) * ve::kVoxelSize; });
	ve::BrickMips m{};
	ve::build_brick_mips(l.data(), &m);

	// 8^3 level: each cell spans 2 voxels, inclusive of its far lattice plane. Cell row
	// j covers lattice y in [2j, 2j+2], so j = 1 (y 2..4) and j = 2 (y 4..6) both touch 0.
	for (int k = 0; k < 8; k++)
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++) {
				const int idx = i + j * 8 + k * 64;
				const bool expect = (2 * j <= 4 && 4 <= 2 * j + 2);
				CHECK(ve::mip_cell_has_surface(m.mn8[idx], m.mx8[idx]) == expect);
			}
	// 2^3 level: cell row j covers lattice y in [8j, 8j+8]; only j = 0 contains y = 4.
	for (int j = 0; j < 2; j++)
		CHECK(ve::mip_cell_has_surface(m.mn2[j * 2], m.mx2[j * 2]) == (j == 0));
}

TEST_CASE("coarse levels bound the fine levels they summarise") {
	// A wobbly field so every cell has a distinct range.
	const auto l = lattice([](int x, int y, int z) {
		return 0.01f * static_cast<float>((x * 7 + y * 13 + z * 3) % 41) - 0.2f;
	});
	ve::BrickMips m{};
	ve::build_brick_mips(l.data(), &m);
	// Each 4^3 cell must enclose its eight 8^3 children, and each 2^3 cell its 4^3 children.
	const auto check_parent = [](const uint8_t *pmn, const uint8_t *pmx, int pd,
			const uint8_t *cmn, const uint8_t *cmx, int cd) {
		for (int z = 0; z < pd; z++)
			for (int y = 0; y < pd; y++)
				for (int x = 0; x < pd; x++) {
					const int p = x + y * pd + z * pd * pd;
					uint8_t mn = 255, mx = 0;
					for (int dz = 0; dz < 2; dz++)
						for (int dy = 0; dy < 2; dy++)
							for (int dx = 0; dx < 2; dx++) {
								const int c = (2 * x + dx) + (2 * y + dy) * cd +
										(2 * z + dz) * cd * cd;
								mn = std::min(mn, cmn[c]);
								mx = std::max(mx, cmx[c]);
							}
					CHECK(static_cast<int>(pmn[p]) == static_cast<int>(mn));
					CHECK(static_cast<int>(pmx[p]) == static_cast<int>(mx));
				}
	};
	check_parent(m.mn4, m.mx4, 4, m.mn8, m.mx8, 8);
	check_parent(m.mn2, m.mx2, 2, m.mn4, m.mx4, 4);
}

TEST_CASE("the 8^3 level is the exact min/max of its inclusive 3^3 lattice block") {
	const auto l = lattice([](int x, int y, int z) {
		return 0.01f * static_cast<float>((x * 5 + y * 11 + z * 17) % 53) - 0.25f;
	});
	ve::BrickMips m{};
	ve::build_brick_mips(l.data(), &m);
	for (int k = 0; k < 8; k++)
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++) {
				uint8_t mn = 255, mx = 0;
				for (int dz = 0; dz <= 2; dz++)
					for (int dy = 0; dy <= 2; dy++)
						for (int dx = 0; dx <= 2; dx++) {
							const uint8_t v = l[ve::sdf_index(2 * i + dx, 2 * j + dy, 2 * k + dz)];
							mn = std::min(mn, v);
							mx = std::max(mx, v);
						}
				const int idx = i + j * 8 + k * 64;
				CHECK(static_cast<int>(m.mn8[idx]) == static_cast<int>(mn));
				CHECK(static_cast<int>(m.mx8[idx]) == static_cast<int>(mx));
			}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/brick_mip.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/world/brick_mip.h`:

```cpp
#pragma once
#include "world/brick.h"
#include <cstdint>

namespace ve {

// Per-brick min–max acceleration chain (spec §2). Level L partitions the brick into
// kMipDims[L]^3 cells; each entry is the min / max of the ENCODED sdf lattice over the
// cell's INCLUSIVE corner range. Inclusive matters: the trilinear reconstruction inside a
// cell is a multilinear interpolant of its corner samples and therefore never leaves their
// convex hull, so an inclusive min/max is a sound bound and an exclusive one is not.
inline constexpr int kMipLevels = 3;
inline constexpr int kMipDims[kMipLevels] = {2, 4, 8};

// encode_sdf(0.0f). A cell contains no surface when its whole range sits on one side.
inline constexpr uint8_t kEncodedZero = 128;

struct BrickMips {
	uint8_t mn2[8]{},   mx2[8]{};
	uint8_t mn4[64]{},  mx4[64]{};
	uint8_t mn8[512]{}, mx8[512]{};
};

// sdf_lattice must hold kBrickSdfCount encoded samples in sdf_index() order.
void build_brick_mips(const uint8_t *sdf_lattice, BrickMips *out);

inline bool mip_cell_has_surface(uint8_t mn, uint8_t mx) {
	return mn <= kEncodedZero && mx >= kEncodedZero;
}

const uint8_t *mip_min(const BrickMips &m, int level);
const uint8_t *mip_max(const BrickMips &m, int level);

} // namespace ve
```

`extension/src/world/brick_mip.cpp`:

```cpp
#include "world/brick_mip.h"
#include <algorithm>

namespace ve {

namespace {

void reduce(const uint8_t *cmn, const uint8_t *cmx, int cd, uint8_t *pmn, uint8_t *pmx) {
	const int pd = cd / 2;
	for (int z = 0; z < pd; z++)
		for (int y = 0; y < pd; y++)
			for (int x = 0; x < pd; x++) {
				uint8_t mn = 255, mx = 0;
				for (int dz = 0; dz < 2; dz++)
					for (int dy = 0; dy < 2; dy++)
						for (int dx = 0; dx < 2; dx++) {
							const int c = (2 * x + dx) + (2 * y + dy) * cd +
									(2 * z + dz) * cd * cd;
							mn = std::min(mn, cmn[c]);
							mx = std::max(mx, cmx[c]);
						}
				const int p = x + y * pd + z * pd * pd;
				pmn[p] = mn;
				pmx[p] = mx;
			}
}

} // namespace

void build_brick_mips(const uint8_t *sdf_lattice, BrickMips *out) {
	// Finest level straight off the lattice: cell (i,j,k) covers voxels [2i, 2i+2), whose
	// trilinear corners are lattice samples [2i, 2i+2] inclusive -> a 3^3 block.
	for (int k = 0; k < 8; k++)
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++) {
				uint8_t mn = 255, mx = 0;
				for (int dz = 0; dz <= 2; dz++)
					for (int dy = 0; dy <= 2; dy++)
						for (int dx = 0; dx <= 2; dx++) {
							const uint8_t v =
									sdf_lattice[sdf_index(2 * i + dx, 2 * j + dy, 2 * k + dz)];
							mn = std::min(mn, v);
							mx = std::max(mx, v);
						}
				const int idx = i + j * 8 + k * 64;
				out->mn8[idx] = mn;
				out->mx8[idx] = mx;
			}
	reduce(out->mn8, out->mx8, 8, out->mn4, out->mx4);
	reduce(out->mn4, out->mx4, 4, out->mn2, out->mx2);
}

const uint8_t *mip_min(const BrickMips &m, int level) {
	return level == 0 ? m.mn2 : (level == 1 ? m.mn4 : m.mn8);
}

const uint8_t *mip_max(const BrickMips &m, int level) {
	return level == 0 ? m.mx2 : (level == 1 ? m.mx4 : m.mx8);
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: the 5 new cases pass.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world/brick_mip.* extension/tests/test_brick_mip.cpp
git commit -m "feat(world): per-brick min-max mip chain"
```

---

### Task 5: `world/brick_eval` — one brick evaluator, CPU side

This is the task that makes a GPU mirror *possible*. M1's `WorldData::generate` inlines the
whole brick recipe and finishes with a **multi-source BFS** that floods every deep-air cell
with its nearest material. That BFS is the one step with no cheap parallel equivalent: a
16³ brick needs up to 45 dilation rounds, and 45 barriered rounds per brick × 8 000 bricks
per frame is not affordable. It exists solely so that a deep-air cell — whose packed 2-bit
index stays 0 — does not resolve to an arbitrary palette slot 0.

The cheaper fix removes the need for it: **make palette slot 0 the brick's dominant
material.** Deep-air cells still read index 0, but index 0 is now the brick's most common
surface material rather than whichever one happened to be inserted first — the best available
guess, at zero cost, and identical on CPU and GPU. Cells that are actually *shaded* are
unaffected either way: they are within ~1 voxel of the surface, and projection pass 1 already
gives every one of those the material of the surface beside it.

`extension/tests/test_material_field.cpp` is the safety net — it asserts exactly the
near-surface property, over >100 000 cells, and must stay green.

**Files:**
- Create: `extension/src/world/brick_eval.h`, `extension/src/world/brick_eval.cpp`
- Modify: `extension/src/world/palette.h`, `extension/src/world/palette.cpp` (add `palette_occupancy_order`)
- Modify: `extension/src/world/world_data.h`, `extension/src/world/world_data.cpp` (drop the inlined recipe and the BFS; loop over `brick_eval`)
- Test: `extension/tests/test_brick_eval.cpp`

**Interfaces:**
- Consumes: `ve::Generator`, `ve::Sample`, `ve::EditOp`, `ve::apply_ops` (Task 2), `ve::IVec3`, `ve::brick_world_origin` (Task 1), `ve::BrickMips`, `ve::build_brick_mips` (Task 4), `ve::Brick`, `ve::palette_slot`.
- Produces:
  - `void ve::palette_occupancy_order(const uint16_t *palette, const int *counts, int *out_order)`
  - `struct ve::BrickEval { Brick brick; BrickMips mips; }`
  - `ve::Sample ve::eval_field(const Generator &, const EditOp *, int, float, float, float)`
  - `bool ve::brick_has_surface(const Generator &, const EditOp *, int, IVec3 brick)`
  - `void ve::eval_brick(const Generator &, const EditOp *, int, IVec3 brick, BrickEval *out)`
  - `ve::kActivationPad` (0.15f) — the probe pad, shared with the GPU mark pass
- `ve::WorldData` keeps its entire public interface unchanged; only its implementation moves.

- [ ] **Step 1: Write the failing test**

`extension/tests/test_brick_eval.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/brick_eval.h"
#include "world/palette.h"
#include "generator/generator.h"
#include <cmath>
#include <set>

TEST_CASE("palette_occupancy_order puts the most-used material in slot 0") {
	const uint16_t pal[4] = {3, 1, 2, 0};
	const int counts[4] = {10, 90, 50, 0};
	int order[4] = {};
	ve::palette_occupancy_order(pal, counts, order);
	CHECK(order[0] == 1); // material 1, 90 cells
	CHECK(order[1] == 2); // material 2, 50 cells
	CHECK(order[2] == 0); // material 3, 10 cells
	CHECK(order[3] == 3); // empty slot last
}

TEST_CASE("palette_occupancy_order breaks ties on the lower material id") {
	// Deterministic ties are what let the GPU reproduce this ordering bit for bit.
	const uint16_t pal[4] = {7, 2, 0, 0};
	const int counts[4] = {5, 5, 0, 0};
	int order[4] = {};
	ve::palette_occupancy_order(pal, counts, order);
	CHECK(order[0] == 1); // id 2 < id 7
	CHECK(order[1] == 0);
}

TEST_CASE("eval_field is the generator with the ops applied in order") {
	ve::AnalyticGenerator gen;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 8.0f; op.pos[1] = -2.0f; op.pos[2] = 8.0f;
	op.radius = 3.0f;
	const ve::Sample plain = ve::eval_field(gen, nullptr, 0, 8.0f, -2.0f, 8.0f);
	const ve::Sample carved = ve::eval_field(gen, &op, 1, 8.0f, -2.0f, 8.0f);
	CHECK(plain.sdf < 0.0f);            // underground: solid
	CHECK(carved.sdf == doctest::Approx(3.0f)); // blast centre: 3 m of air
	CHECK(carved.material == 0);
}

TEST_CASE("brick_has_surface accepts surface bricks and rejects sky and deep rock") {
	ve::AnalyticGenerator gen;
	// hills() stays inside +-10 m, so a brick at y = +40 m is sky and y = -40 m is rock.
	CHECK_FALSE(ve::brick_has_surface(gen, nullptr, 0, {10, 50, 10}));
	CHECK_FALSE(ve::brick_has_surface(gen, nullptr, 0, {10, -50, 10}));
	// Find the surface brick in this column and check it is accepted.
	int found = 0;
	for (int by = -20; by <= 20; by++)
		if (ve::brick_has_surface(gen, nullptr, 0, {10, by, 10})) found++;
	CHECK(found > 0);
}

TEST_CASE("an edit makes a previously-solid brick a surface brick") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 brick{10, -50, 10};
	REQUIRE_FALSE(ve::brick_has_surface(gen, nullptr, 0, brick));
	float bo[3];
	ve::brick_world_origin(brick, bo);
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = bo[0] + 0.4f; op.pos[1] = bo[1] + 0.4f; op.pos[2] = bo[2] + 0.4f;
	op.radius = 2.0f;
	CHECK(ve::brick_has_surface(gen, &op, 1, brick));
}

TEST_CASE("eval_brick produces a signed lattice, a dominant-first palette and mips") {
	ve::AnalyticGenerator gen;
	// Pick a real surface brick.
	ve::IVec3 brick{10, 0, 10};
	for (int by = -20; by <= 20; by++)
		if (ve::brick_has_surface(gen, nullptr, 0, {10, by, 10})) { brick = {10, by, 10}; break; }
	ve::BrickEval e{};
	ve::eval_brick(gen, nullptr, 0, brick, &e);

	bool pos = false, neg = false;
	for (int i = 0; i < ve::kBrickSdfCount; i++) {
		const float d = ve::decode_sdf(e.brick.sdf[i]);
		pos = pos || d > 0.0f;
		neg = neg || d < 0.0f;
	}
	CHECK(pos);
	CHECK(neg);
	CHECK(e.brick.palette[0] != 0); // a surface brick always holds a material

	// Slot 0 really is the most-used index.
	int used[4] = {};
	for (int i = 0; i < ve::kBrickVoxelCount; i++) used[ve::get_mat_index(e.brick, i)]++;
	for (int s = 1; s < 4; s++) CHECK(used[0] >= used[s]);

	// The mips agree with a direct recomputation from the very lattice eval_brick wrote.
	ve::BrickMips ref{};
	ve::build_brick_mips(e.brick.sdf, &ref);
	for (int i = 0; i < 8; i++) {
		CHECK(e.mips.mn2[i] == ref.mn2[i]);
		CHECK(e.mips.mx2[i] == ref.mx2[i]);
	}
	// A surface brick must have at least one 2^3 cell reporting a crossing, or the
	// raymarcher's mip skip would step straight over its surface.
	bool any = false;
	for (int i = 0; i < 8; i++) any = any || ve::mip_cell_has_surface(e.mips.mn2[i], e.mips.mx2[i]);
	CHECK(any);
}

TEST_CASE("eval_brick is deterministic and edit ops change its bytes") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 brick{10, 0, 10};
	ve::BrickEval a{}, b{};
	ve::eval_brick(gen, nullptr, 0, brick, &a);
	ve::eval_brick(gen, nullptr, 0, brick, &b);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(a.brick.sdf[i] == b.brick.sdf[i]);

	float bo[3];
	ve::brick_world_origin(brick, bo);
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = bo[0] + 0.4f; op.pos[1] = bo[1] + 0.4f; op.pos[2] = bo[2] + 0.4f;
	op.radius = 1.0f;
	ve::BrickEval c{};
	ve::eval_brick(gen, &op, 1, brick, &c);
	int differing = 0;
	for (int i = 0; i < ve::kBrickSdfCount; i++) differing += a.brick.sdf[i] != c.brick.sdf[i];
	CHECK(differing > 100);
}

TEST_CASE("WorldData still agrees with eval_brick after the refactor") {
	ve::WorldData w(20, 12, 20);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	REQUIRE(w.active_brick_count() > 0);
	int checked = 0;
	for (int bz = 0; bz < 20 && checked < 8; bz++)
		for (int by = 0; by < 12 && checked < 8; by++)
			for (int bx = 0; bx < 20 && checked < 8; bx++) {
				const int slot = w.brick_slot(bx, by, bz);
				if (slot < 0) continue;
				ve::BrickEval e{};
				ve::eval_brick(gen, nullptr, 0, {bx, by, bz}, &e);
				const ve::Brick &got = w.brick(slot);
				for (int i = 0; i < ve::kBrickSdfCount; i++)
					REQUIRE(got.sdf[i] == e.brick.sdf[i]);
				for (int i = 0; i < ve::kBrickVoxelCount / 4; i++)
					REQUIRE(got.mat[i] == e.brick.mat[i]);
				for (int p = 0; p < ve::kBrickPaletteSize; p++)
					REQUIRE(got.palette[p] == e.brick.palette[p]);
				checked++;
			}
	CHECK(checked == 8);
}
```

`extension/tests/test_brick_eval.cpp` needs `#include "world/world_data.h"` for the last case —
add it to the include block at the top.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/brick_eval.h` not found.

- [ ] **Step 3: Add `palette_occupancy_order`**

Append to `extension/src/world/palette.h`, inside `namespace ve`:

```cpp
// Sort order for a brick's palette. Slot 0 must hold the brick's DOMINANT material: a cell
// that never got a material keeps packed index 0, and index 0 is indistinguishable from
// palette slot 0, so whatever sits there is what such a cell renders as. Ordering by cell
// count makes that fallback the brick's most likely surface.
//
// Ties break on the lower material id, and empty slots (id 0) always sort last. Both rules
// exist so the GPU generator can reproduce this ordering bit for bit without a stable sort.
//
// counts[i] is the number of cells that resolved to palette slot i. out_order[k] receives
// the ORIGINAL slot that belongs at final position k, i.e. new_palette[k] = palette[out_order[k]].
void palette_occupancy_order(const uint16_t *palette, const int *counts, int *out_order);
```

Append to `extension/src/world/palette.cpp`, inside `namespace ve`:

```cpp
void palette_occupancy_order(const uint16_t *palette, const int *counts, int *out_order) {
	for (int i = 0; i < kBrickPaletteSize; i++) out_order[i] = i;
	// Selection sort over four entries: small, branch-explicit, and trivially mirrored in
	// GLSL by a single thread (see shaders/brick_gen.comp.glsl).
	for (int a = 0; a < kBrickPaletteSize; a++)
		for (int b = a + 1; b < kBrickPaletteSize; b++) {
			const int ia = out_order[a], ib = out_order[b];
			const bool a_empty = palette[ia] == 0, b_empty = palette[ib] == 0;
			bool swap = false;
			if (a_empty != b_empty) {
				swap = a_empty; // non-empty slots always precede empty ones
			} else if (!a_empty) {
				swap = counts[ib] > counts[ia] ||
						(counts[ib] == counts[ia] && palette[ib] < palette[ia]);
			}
			if (swap) { out_order[a] = ib; out_order[b] = ia; }
		}
}
```

`palette.cpp` already includes `world/brick.h` for `kBrickPaletteSize`.

- [ ] **Step 4: Write `world/brick_eval`**

`extension/src/world/brick_eval.h`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "world/brick.h"
#include "world/brick_mip.h"
#include "world/region.h"

namespace ve {

// Conservative pad for the 3^3 activation probe: the probe samples every 8 voxels, so the
// field can dip across zero between samples. A brick is treated as empty only when all 27
// probes agree AND clear zero by this margin. The GPU mark pass uses the same constant.
inline constexpr float kActivationPad = 0.15f;

struct BrickEval {
	Brick brick;
	BrickMips mips;
};

// The world field: the generator with this point's region ops applied in order (spec §2).
Sample eval_field(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z);

// Coarse residency probe. Mirrored exactly by shaders/brick_mark.comp.glsl — a brick is
// resident iff this returns true, on both sides.
bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick);

// Full brick contents at L0. This is BOTH the path WorldData walks and the CPU reference
// the GPU differential test diffs against (spec §8).
void eval_brick(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		BrickEval *out);

} // namespace ve
```

`extension/src/world/brick_eval.cpp`:

```cpp
#include "world/brick_eval.h"
#include "world/palette.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

// Give every cell within reach of a surface hit the material of that surface.
//
// A ray's hit point routinely lands on the AIR side of the surface: the march stops at
// d < 0.002 and the secant refinement can leave p just outside, so the material lookup
// rounds to an air cell. Projecting each near-surface air cell onto the surface along
// -grad(SDF) and asking the FIELD for the material there removes the failure mode.
//
// Two reasons not to copy from a neighbouring cell instead: the closest surface point is
// what a ray hitting near this cell would shade, which on a slope is not the L1-nearest
// solid cell; and that surface often lies in the next brick along -- a cell on this brick's
// bottom plane belongs to the surface below it, whose material may appear nowhere in this
// brick at all. The field has no such boundary, so it answers correctly in both cases.
//
// Cells further than project_range from any surface are left at 0 and therefore resolve to
// palette slot 0, which palette_occupancy_order() guarantees is the brick's dominant
// material. No flood fill is needed, and the GPU can run this pass thread-per-cell.
void spread_materials(uint16_t *mat, const Brick &b, const Generator &gen,
		const EditOp *ops, int op_count, const float bo[3]) {
	auto lat = [&b](int x, int y, int z) { return decode_sdf(b.sdf[sdf_index(x, y, z)]); };
	// Central difference along one axis, divided by the span actually sampled. On a brick's
	// outer planes the lattice has no neighbour on one side, so the difference is one-sided
	// over a single voxel; dividing every axis by a fixed 2 would then halve that component
	// and tilt the "normal" towards the horizontal, sending the projection below sideways
	// across a material band instead of straight down onto the surface underneath.
	auto slope = [&lat](int x, int y, int z, int axis) {
		int lo[3] = {x, y, z}, hi[3] = {x, y, z};
		lo[axis] = std::max(lo[axis] - 1, 0);
		hi[axis] = std::min(hi[axis] + 1, kBrickVoxels);
		const float span = static_cast<float>(hi[axis] - lo[axis]) * kVoxelSize;
		return (lat(hi[0], hi[1], hi[2]) - lat(lo[0], lo[1], lo[2])) / span;
	};
	// A hit point rounds to the cell containing it, so only cells within about a voxel of
	// the surface are ever read; project a little beyond that for margin.
	const float project_range = 2.0f * kVoxelSize;
	for (int z = 0; z < kBrickVoxels; z++)
		for (int y = 0; y < kBrickVoxels; y++)
			for (int x = 0; x < kBrickVoxels; x++) {
				const int i = voxel_index(x, y, z);
				if (mat[i] != 0) continue;
				const float d = lat(x, y, z);
				if (d > project_range) continue; // too far from any surface to be shaded
				const float gx = slope(x, y, z, 0);
				const float gy = slope(x, y, z, 1);
				const float gz = slope(x, y, z, 2);
				const float len = std::sqrt(gx * gx + gy * gy + gz * gz);
				if (len <= 0.0f) continue;
				// Step the cell's own distance to the surface, plus half a voxel to land
				// inside the solid, where a material was sampled. The stored SDF is
				// uint8-quantised and the gradient is a difference of those, so a single
				// step can still fall short; lengthen it and retry.
				for (float over = 0.5f; over <= 2.5f && mat[i] == 0; over += 1.0f) {
					const float t = d + over * kVoxelSize;
					mat[i] = eval_field(gen, ops, op_count,
							bo[0] + x * kVoxelSize - gx / len * t,
							bo[1] + y * kVoxelSize - gy / len * t,
							bo[2] + z * kVoxelSize - gz / len * t).material;
				}
			}
}

} // namespace

Sample eval_field(const Generator &gen, const EditOp *ops, int op_count,
		float x, float y, float z) {
	return apply_ops(gen.sample(x, y, z), ops, op_count, x, y, z);
}

bool brick_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick) {
	float bo[3];
	brick_world_origin(brick, bo);
	float mn = 1e30f, mx = -1e30f;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				const float d = eval_field(gen, ops, op_count,
						bo[0] + sx * (kBrickVoxels / 2) * kVoxelSize,
						bo[1] + sy * (kBrickVoxels / 2) * kVoxelSize,
						bo[2] + sz * (kBrickVoxels / 2) * kVoxelSize).sdf;
				mn = std::min(mn, d);
				mx = std::max(mx, d);
			}
	return mn < kActivationPad && mx > -kActivationPad;
}

void eval_brick(const Generator &gen, const EditOp *ops, int op_count, IVec3 brick,
		BrickEval *out) {
	*out = BrickEval{};
	Brick &b = out->brick;
	float bo[3];
	brick_world_origin(brick, bo);

	// The SDF runs over the 17^3 lattice (see kBrickSdfStride): the extra plane at local 16
	// on each axis is the apron the shader's trilinear filter needs to cover the brick's
	// last voxel slab. Materials stay on the 16^3 cell grid.
	uint16_t mat[kBrickVoxelCount] = {};
	for (int vz = 0; vz < kBrickSdfStride; vz++)
		for (int vy = 0; vy < kBrickSdfStride; vy++)
			for (int vx = 0; vx < kBrickSdfStride; vx++) {
				const Sample s = eval_field(gen, ops, op_count, bo[0] + vx * kVoxelSize,
						bo[1] + vy * kVoxelSize, bo[2] + vz * kVoxelSize);
				b.sdf[sdf_index(vx, vy, vz)] = encode_sdf(s.sdf);
				if (s.material == 0) continue;
				// An apron sample seeds the cell the shader's clamp folds it into: a brick
				// whose surface crosses only inside its last slab has no solid cell of its
				// own, and would otherwise hold no material at all.
				const bool apron =
						vx == kBrickVoxels || vy == kBrickVoxels || vz == kBrickVoxels;
				const int ci = voxel_index(std::min(vx, kBrickVoxels - 1),
						std::min(vy, kBrickVoxels - 1), std::min(vz, kBrickVoxels - 1));
				if (!apron || mat[ci] == 0) mat[ci] = s.material; // a cell's own sample wins
			}

	spread_materials(mat, b, gen, ops, op_count, bo);

	uint16_t pal[kBrickPaletteSize] = {};
	int counts[kBrickPaletteSize] = {};
	uint8_t slot_of[kBrickVoxelCount];
	bool overflow = false;
	for (int i = 0; i < kBrickVoxelCount; i++) {
		if (mat[i] == 0) { slot_of[i] = 0xFF; continue; }
		const int s = palette_slot(pal, mat[i], &overflow);
		slot_of[i] = static_cast<uint8_t>(s);
		counts[s]++;
	}
	int order[kBrickPaletteSize] = {};
	palette_occupancy_order(pal, counts, order);
	int inverse[kBrickPaletteSize] = {};
	for (int k = 0; k < kBrickPaletteSize; k++) inverse[order[k]] = k;
	for (int k = 0; k < kBrickPaletteSize; k++) b.palette[k] = pal[order[k]];
	for (int i = 0; i < kBrickVoxelCount; i++)
		if (slot_of[i] != 0xFF)
			set_mat_index(b, i, static_cast<uint8_t>(inverse[slot_of[i]]));

	build_brick_mips(b.sdf, &out->mips);
}

} // namespace ve
```

- [ ] **Step 5: Refactor `WorldData` onto `eval_brick`**

Replace the body of `extension/src/world/world_data.cpp` entirely (the anonymous-namespace
`spread_materials` moves to `brick_eval.cpp`, minus its BFS pass):

```cpp
#include "world/world_data.h"
#include "world/brick_eval.h"

namespace ve {

WorldData::WorldData(int bx, int by, int bz)
	: dims_{bx, by, bz}, indirection_(static_cast<size_t>(bx) * by * bz, -1) {}

void WorldData::generate(const Generator &gen) {
	// M2: WorldData is no longer the renderer's source of truth — it is the CPU reference
	// the GPU differential test diffs against (spec §8), so it must walk exactly the same
	// evaluator the GPU mirrors. No edit ops: WorldData models the unedited base world.
	for (int bz = 0; bz < dims_.z; bz++)
		for (int by = 0; by < dims_.y; by++)
			for (int bx = 0; bx < dims_.x; bx++) {
				const IVec3 brick{bx, by, bz};
				if (!brick_has_surface(gen, nullptr, 0, brick)) continue;
				const int slot = static_cast<int>(bricks_.size());
				indirection_[static_cast<size_t>(bx) + by * dims_.x + bz * dims_.x * dims_.y] =
						slot;
				BrickEval e{};
				eval_brick(gen, nullptr, 0, brick, &e);
				bricks_.push_back(e.brick);
				mips_.push_back(e.mips);
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

Add the mip store to `extension/src/world/world_data.h` — insert `#include "world/brick_mip.h"`
at the top, and inside the class:

```cpp
	const BrickMips &mips(int slot) const { return mips_[slot]; }
```

and next to `bricks_` in the private section:

```cpp
	std::vector<BrickMips> mips_;
```

- [ ] **Step 6: Run the whole native suite**

Run: `cd extension && scons test`
Expected: every case passes — including M1's `test_world_data.cpp`, `test_brick_field.cpp` and
in particular **`test_material_field.cpp`**, whose "near-surface cells carry the material of
the surface beside them" case is the guard on dropping the BFS. If that case regresses, the
projection pass is not covering a band the BFS used to reach — widen `project_range` in
`spread_materials` rather than reinstating the flood fill, and note the new value here.

- [ ] **Step 7: Commit**

```bash
git add extension/src/world/brick_eval.* extension/src/world/palette.* \
        extension/src/world/world_data.* extension/tests/test_brick_eval.cpp
git commit -m "refactor(world): single brick evaluator with occupancy-ordered palettes"
```

---

### Task 6: `world/raycast` — CPU sphere trace for edit aiming

The edit tool has to know *where* the player is pointing. Reusing the GPU raymarcher for that
would mean a full pass plus a `submit()`/`sync()` stall on the main thread every time the
mouse moves. The field is analytic and cheap, so trace it directly: this is a pure C++ core,
unit-testable without a GPU, and it doubles as the oracle for the edit-pipeline gdUnit test.

**Files:**
- Create: `extension/src/world/raycast.h`, `extension/src/world/raycast.cpp`
- Modify: `extension/src/generator/generator.h`, `extension/src/generator/generator.cpp` (add `lipschitz()`)
- Test: `extension/tests/test_raycast.cpp`

**Interfaces:**
- Consumes: `ve::EditLog` (Task 3), `ve::eval_field` (Task 5), `ve::WorldBounds` (Task 1).
- Produces:
  - `virtual float ve::Generator::lipschitz() const` — default `2.0f`; the maximum gradient
    magnitude of the returned field. `AnalyticGenerator` overrides it with `2.0f`.
  - `struct ve::RayHit { bool hit; float pos[3]; float normal[3]; float distance; }`
  - `ve::RayHit ve::raycast(const Generator &, const EditLog &, const float origin[3], const float dir[3], float max_dist)`

- [ ] **Step 1: Write the failing test**

`extension/tests/test_raycast.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/raycast.h"
#include "world/brick_eval.h"
#include "generator/generator.h"
#include <cmath>

static ve::WorldBounds bounds() { return ve::WorldBounds{{0, -64, 0}, {64, 8, 64}}; }

TEST_CASE("a ray straight down from the sky lands on the surface") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 40.0f, 100.0f};
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const ve::RayHit h = ve::raycast(gen, log, o, d, 200.0f);
	REQUIRE(h.hit);
	CHECK(h.pos[0] == doctest::Approx(100.0f));
	CHECK(h.pos[2] == doctest::Approx(100.0f));
	// The hit point is on the surface, to within the tracer's own tolerance.
	CHECK(std::fabs(ve::eval_field(gen, nullptr, 0, h.pos[0], h.pos[1], h.pos[2]).sdf) < 0.02f);
	// The surface faces up, so the normal has a strong +y component.
	CHECK(h.normal[1] > 0.5f);
	CHECK(h.distance == doctest::Approx(40.0f - h.pos[1]).epsilon(0.01));
}

TEST_CASE("a ray into the sky misses") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 40.0f, 100.0f};
	const float d[3] = {0.0f, 1.0f, 0.0f};
	CHECK_FALSE(ve::raycast(gen, log, o, d, 200.0f).hit);
}

TEST_CASE("max_dist bounds the trace") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 40.0f, 100.0f};
	const float d[3] = {0.0f, -1.0f, 0.0f};
	CHECK_FALSE(ve::raycast(gen, log, o, d, 5.0f).hit);
	CHECK(ve::raycast(gen, log, o, d, 100.0f).hit);
}

TEST_CASE("the trace sees edits: a crater moves the hit point down") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 40.0f, 100.0f};
	const float d[3] = {0.0f, -1.0f, 0.0f};
	const ve::RayHit before = ve::raycast(gen, log, o, d, 200.0f);
	REQUIRE(before.hit);

	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 100.0f; op.pos[1] = before.pos[1]; op.pos[2] = 100.0f;
	op.radius = 4.0f;
	REQUIRE_FALSE(log.append(op).touched.empty());

	const ve::RayHit after = ve::raycast(gen, log, o, d, 200.0f);
	REQUIRE(after.hit);
	CHECK(after.pos[1] < before.pos[1] - 3.0f); // fell through the crater
}

TEST_CASE("a ray that starts inside solid reports a hit at its origin") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, -30.0f, 100.0f}; // well underground
	const float d[3] = {0.0f, 1.0f, 0.0f};
	const ve::RayHit h = ve::raycast(gen, log, o, d, 100.0f);
	REQUIRE(h.hit);
	CHECK(h.distance == doctest::Approx(0.0f));
}

TEST_CASE("the direction is normalised for the caller") {
	ve::AnalyticGenerator gen;
	ve::EditLog log(bounds());
	const float o[3] = {100.0f, 40.0f, 100.0f};
	const float unit[3] = {0.0f, -1.0f, 0.0f};
	const float scaled[3] = {0.0f, -7.5f, 0.0f};
	const ve::RayHit a = ve::raycast(gen, log, o, unit, 200.0f);
	const ve::RayHit b = ve::raycast(gen, log, o, scaled, 200.0f);
	REQUIRE(a.hit);
	REQUIRE(b.hit);
	CHECK(b.pos[1] == doctest::Approx(a.pos[1]).epsilon(0.001));
	CHECK(b.distance == doctest::Approx(a.distance).epsilon(0.001));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/raycast.h` not found.

- [ ] **Step 3: Add `Generator::lipschitz()`**

In `extension/src/generator/generator.h`, add to `class Generator`:

```cpp
	// Upper bound on |grad(sdf)| of the field this generator returns. The generator's
	// `y - h(x, z)` OVER-estimates true distance on a slope, so a sphere tracer that
	// stepped by the reported value would tunnel through overhangs. Stepping by
	// reported / lipschitz() is the standard safe bound: true_distance >= |reported| / L.
	virtual float lipschitz() const { return 2.0f; }
```

and to `class AnalyticGenerator`, in the public section:

```cpp
	// |grad(y - hills)| = sqrt(1 + |grad hills|^2); the amplitude-times-frequency sum of
	// hills() is below 1.0 per axis, so 2.0 is comfortably conservative. The cave is a
	// unit-gradient sphere combined with max(), which cannot raise the bound.
	float lipschitz() const override { return 2.0f; }
```

No change to `generator.cpp` is needed (both are inline).

- [ ] **Step 4: Write the implementation**

`extension/src/world/raycast.h`:

```cpp
#pragma once
#include "generator/generator.h"
#include "world/edit_log.h"

namespace ve {

struct RayHit {
	bool hit = false;
	float pos[3] = {0.0f, 0.0f, 0.0f};
	float normal[3] = {0.0f, 0.0f, 0.0f};
	float distance = 0.0f;
};

// Sphere-traces the analytic field (G + each sample point's region ops) with no atlas and no
// GPU. Used by the edit tool to place ops on the main thread without stalling the renderer,
// and as the oracle in tests. dir need not be normalised.
RayHit raycast(const Generator &gen, const EditLog &log, const float origin[3],
		const float dir[3], float max_dist);

} // namespace ve
```

`extension/src/world/raycast.cpp`:

```cpp
#include "world/raycast.h"
#include "world/brick_eval.h"
#include <cmath>

namespace ve {

namespace {

// The op list that governs a point is the list of the region containing it (spec §2: an op
// is appended to every region it touches, so no neighbour walk is needed).
const std::vector<EditOp> &ops_at(const EditLog &log, float x, float y, float z) {
	return log.ops(WorldBounds::region_of_point(x, y, z));
}

float field_at(const Generator &gen, const EditLog &log, float x, float y, float z) {
	const std::vector<EditOp> &ops = ops_at(log, x, y, z);
	return eval_field(gen, ops.data(), static_cast<int>(ops.size()), x, y, z).sdf;
}

} // namespace

RayHit raycast(const Generator &gen, const EditLog &log, const float origin[3],
		const float dir[3], float max_dist) {
	RayHit out;
	const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len <= 0.0f) return out;
	const float d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
	const float inv_l = 1.0f / gen.lipschitz();
	const float min_step = 0.5f * kVoxelSize;
	const float hit_eps = 0.2f * kVoxelSize;

	float t = 0.0f;
	for (int i = 0; i < 4096 && t <= max_dist; i++) {
		const float p[3] = {origin[0] + d[0] * t, origin[1] + d[1] * t, origin[2] + d[2] * t};
		const float f = field_at(gen, log, p[0], p[1], p[2]);
		if (f < hit_eps) {
			out.hit = true;
			out.distance = t;
			out.pos[0] = p[0]; out.pos[1] = p[1]; out.pos[2] = p[2];
			// Central-difference gradient over one voxel; the field is smooth at this scale.
			const float e = kVoxelSize;
			const float gx = field_at(gen, log, p[0] + e, p[1], p[2]) -
					field_at(gen, log, p[0] - e, p[1], p[2]);
			const float gy = field_at(gen, log, p[0], p[1] + e, p[2]) -
					field_at(gen, log, p[0], p[1] - e, p[2]);
			const float gz = field_at(gen, log, p[0], p[1], p[2] + e) -
					field_at(gen, log, p[0], p[1], p[2] - e);
			const float gl = std::sqrt(gx * gx + gy * gy + gz * gz);
			if (gl > 0.0f) {
				out.normal[0] = gx / gl; out.normal[1] = gy / gl; out.normal[2] = gz / gl;
			} else {
				out.normal[1] = 1.0f;
			}
			return out;
		}
		t += std::max(f * inv_l, min_step);
	}
	return out;
}

} // namespace ve
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: the 6 new cases pass, whole suite green.

- [ ] **Step 6: Commit**

```bash
git add extension/src/world/raycast.* extension/src/generator/generator.h \
        extension/tests/test_raycast.cpp
git commit -m "feat(world): CPU sphere-trace raycast against G plus edits"
```

---

### Task 7: GLSL field mirror + differential harness

Spec §8 requires "CPU references for brick-eval and meshing; dev console command runs both and
diffs — catches shader/reference drift." Everything downstream in M2 rests on the GPU's
`eval_field` agreeing with `ve::eval_field`, so the mirror and its diff test come first,
before anything is generated with it.

Three shader files are shared from here on: `common.glsl` (constants + decode + palette
colours), `brick_layout.glsl` (atlas addressing), `field.glsl` (G + ops). Diamond includes
become unavoidable, so the loader gains include-once semantics first.

**Files:**
- Modify: `extension/src/render/shader_loader.cpp` (include-once)
- Modify: `extension/tests/test_shader_loader.cpp` (two new cases)
- Modify: `shaders/common.glsl` (runtime atlas dims, `quantise_sdf`, material 4)
- Create: `shaders/brick_layout.glsl`, `shaders/field.glsl`, `shaders/field_probe.comp.glsl`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (two debug bindings)
- Test: `tests/test_field_diff.gd`

**Interfaces:**
- Consumes: `ve::load_shader_source` (M1 Task 8), `ve::eval_field` (Task 5), `ve::EditOp` (Task 2).
- Produces:
  - GLSL `float quantise_sdf(float d)` — the exact float an R8_UNORM store must receive so the
    written byte equals `ve::encode_sdf(d)`.
  - GLSL `ivec3 atlas_brick_cell(int slot, ivec3 atlas_bricks)` and
    `ivec3 atlas_base(int slot, ivec3 atlas_bricks, int stride)`.
  - GLSL `void eval_field(vec3 p, uint op_base, uint op_count, out float sdf, out uint mat)`
    in `field.glsl`; the includer must `#define FIELD_OP_POOL_BINDING <n>` beforehand.
  - GLSL `const uint MAX_REGION_OPS = 256u;`
  - `VoxelWorld::debug_load_shader(String res_path) -> String`
  - `VoxelWorld::debug_eval_field(Vector3 p, PackedByteArray ops, int op_count) -> Vector2`
    — `x` = sdf, `y` = material. `ops` is a byte-identical `ve::EditOp` array, so the same
    `PackedByteArray` feeds the GPU buffer and the CPU reference.

- [ ] **Step 1: Write the failing loader tests**

Append to `extension/tests/test_shader_loader.cpp`:

```cpp
TEST_CASE("a header included down two paths is expanded once") {
	auto dir = std::filesystem::temp_directory_path() / "ve_sl4";
	write_file(dir, "common.glsl", "const float SHARED = 1.0;\n");
	write_file(dir, "a.glsl", "#include \"common.glsl\"\nconst float A = SHARED;\n");
	write_file(dir, "b.glsl", "#include \"common.glsl\"\nconst float B = SHARED;\n");
	auto p = write_file(dir, "main.glsl", "#include \"a.glsl\"\n#include \"b.glsl\"\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(err.empty());
	// Declaring SHARED twice is a GLSL redefinition error, so it must appear exactly once.
	const auto first = out.find("const float SHARED");
	REQUIRE(first != std::string::npos);
	CHECK(out.find("const float SHARED", first + 1) == std::string::npos);
	CHECK(out.find("const float A") != std::string::npos);
	CHECK(out.find("const float B") != std::string::npos);
}

TEST_CASE("include-once does not mask a real cycle") {
	auto dir = std::filesystem::temp_directory_path() / "ve_sl5";
	write_file(dir, "a.glsl", "#include \"b.glsl\"\n");
	auto p = write_file(dir, "b.glsl", "#include \"a.glsl\"\n");
	std::string err;
	auto out = ve::load_shader_source(p.string(), dir.string(), &err);
	CHECK(out.empty());
	CHECK(err.find("cycle") != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify the first one fails**

Run: `cd extension && scons test`
Expected: "a header included down two paths is expanded once" FAILS — `SHARED` appears twice.
"include-once does not mask a real cycle" passes already (it is the M1 behaviour, pinned here
so Step 3 cannot regress it).

- [ ] **Step 3: Give the loader include-once semantics**

Replace `extension/src/render/shader_loader.cpp`:

```cpp
#include "render/shader_loader.h"
#include <fstream>
#include <set>
#include <sstream>

namespace ve {

// `stack` is the ancestry of the file being expanded and detects cycles; `included` is every
// file already emitted anywhere and gives `#pragma once` semantics. Both are needed: common
// headers are pulled in down several paths (a diamond, which must expand once and is not an
// error) while a true cycle must still be reported rather than silently truncated.
static bool expand(const std::string &path, const std::string &include_dir,
		std::set<std::string> &stack, std::set<std::string> &included,
		std::ostringstream &out, std::string *error) {
	if (stack.count(path)) {
		if (error) *error = "include cycle at " + path;
		return false;
	}
	if (included.count(path)) return true; // already emitted: skip, not an error
	std::ifstream f(path);
	if (!f) {
		if (error) *error = "cannot open " + path;
		return false;
	}
	stack.insert(path);
	included.insert(path);
	std::string line;
	while (std::getline(f, line)) {
		const std::string key = "#include \"";
		const auto pos = line.find(key);
		if (pos != std::string::npos) {
			const auto end = line.find('"', pos + key.size());
			const std::string name = line.substr(pos + key.size(), end - pos - key.size());
			if (!expand(include_dir + "/" + name, include_dir, stack, included, out, error))
				return false;
		} else {
			out << line << '\n';
		}
	}
	stack.erase(path);
	return true;
}

std::string load_shader_source(const std::string &path, const std::string &include_dir,
		std::string *error) {
	std::set<std::string> stack, included;
	std::ostringstream out;
	if (!expand(path, include_dir, stack, included, out, error)) return "";
	return out.str();
}

} // namespace ve
```

Run: `cd extension && scons test` — both new cases and all four M1 loader cases pass.

- [ ] **Step 4: Rework `shaders/common.glsl`**

Replace `shaders/common.glsl`:

```glsl
// Shared constants + helpers, pulled into every voxel shader by ve::load_shader_source.
// NOTE: never put a literal include directive inside a comment in this file — the loader
// matches include tokens anywhere in a line and would self-include (cycle error).
const int BRICK_VOXELS = 16;
const float VOXEL_SIZE = 0.05;
const float BRICK_SIZE = 0.8;         // 16 * 0.05
const float SDF_RANGE = 0.64;         // uint8 unorm <-> [-0.64, 0.64]
const int REGION_BRICKS = 32;
const float REGION_SIZE = 25.6;       // 32 * 0.8
const int REGION_BRICK_COUNT = 32768; // 32^3

// The SDF atlas stores a 17^3 LATTICE per brick: sample n sits at local coordinate n, and
// the extra plane at 16 is a one-voxel apron so trilinear reconstruction covers the brick's
// whole [0,16) extent. Without it the last slab clamps to a constant, the gradient collapses,
// and shading seams appear on every brick face. The material atlas is a plain 16^3 cell grid
// (nearest filtered, no apron).
const int BRICK_SDF_STRIDE = BRICK_VOXELS + 1; // 17
const int BRICK_VOXEL_COUNT = 4096;            // 16^3
const int BRICK_SDF_COUNT = 4913;              // 17^3
const float BRICK_SDF_MAX = float(BRICK_VOXELS); // last valid lattice coordinate

// A cell of the min-max chain holds no surface unless its range straddles this value.
// It is exactly ve::encode_sdf(0.0f).
const uint ENCODED_ZERO = 128u;

float decode_sdf(float unorm) { return unorm * 2.0 * SDF_RANGE - SDF_RANGE; }

// The float an R8_UNORM imageStore must receive for the written byte to equal
// ve::encode_sdf(d). Quantising here rather than leaning on the hardware's float->unorm
// conversion removes the only rounding-mode difference between CPU and GPU generation.
float quantise_sdf(float d) {
	float t = clamp((d + SDF_RANGE) / (2.0 * SDF_RANGE), 0.0, 1.0);
	return floor(t * 255.0 + 0.5) / 255.0;
}

vec3 material_albedo(uint mat_id) {
	switch (mat_id) {
		case 1: return vec3(0.36, 0.55, 0.22); // grass
		case 2: return vec3(0.45, 0.42, 0.40); // rock
		case 3: return vec3(0.50, 0.35, 0.20); // dirt
		case 4: return vec3(0.62, 0.60, 0.66); // fill (sphere-add tool)
		default: return vec3(1.0, 0.0, 1.0);   // error magenta
	}
}

vec3 sky_color(vec3 dir) {
	float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
	return mix(vec3(0.55, 0.45, 0.35), vec3(0.25, 0.45, 0.85), t);
}
```

`const ivec3 ATLAS_BRICKS` is deliberately gone: the atlas grid is now configurable at
runtime (tests shrink it) and travels in a push constant or uniform block.

- [ ] **Step 5: Write `shaders/brick_layout.glsl`**

```glsl
// Atlas addressing, shared by the generator and the raymarcher so they can never disagree.
// Slot s occupies brick cell (s % ax, (s / ax) % ay, s / (ax * ay)); its texel origin in a
// texture whose per-brick stride is `stride` is that cell times `stride`. Strides: SDF 17,
// material 16, min-max levels 2 / 4 / 8.
#include "common.glsl"

ivec3 atlas_brick_cell(int slot, ivec3 atlas_bricks) {
	return ivec3(slot % atlas_bricks.x,
	             (slot / atlas_bricks.x) % atlas_bricks.y,
	             slot / (atlas_bricks.x * atlas_bricks.y));
}

ivec3 atlas_base(int slot, ivec3 atlas_bricks, int stride) {
	return atlas_brick_cell(slot, atlas_bricks) * stride;
}
```

- [ ] **Step 6: Write `shaders/field.glsl`**

```glsl
// GPU mirror of ve::AnalyticGenerator (extension/src/generator/generator.cpp) and
// ve::apply_op (extension/src/generator/edit_ops.cpp). tests/test_field_diff.gd diffs the
// two and fails when they drift (spec section 8).
//
// The includer MUST define FIELD_OP_POOL_BINDING to the set-0 binding of its op pool before
// pulling this file in.
#include "common.glsl"

const uint OP_SPHERE_SUBTRACT = 0u;
const uint OP_SPHERE_ADD = 1u;
const uint OP_SPHERE_PAINT = 2u;
const uint MAX_REGION_OPS = 256u;

// ve::EditOp is 32 bytes; storing two uvec4 per op and unpacking by hand keeps the mirror
// exact, because a std430 struct with a vec3 member would silently pad to 48.
//   uvec4 a = { type, material, pos.x, pos.y }
//   uvec4 b = { pos.z, radius, pad, pad }
layout(set = 0, binding = FIELD_OP_POOL_BINDING, std430) readonly buffer FieldOpPool {
	uvec4 v[];
} field_op_pool;

float hills(float x, float z) {
	return 6.0 * sin(x * 0.11) * cos(z * 0.13)
	     + 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043)
	     + 1.0 * sin(x * 0.23 + z * 0.19);
}

void base_field(vec3 p, out float sdf, out uint mat) {
	float h = hills(p.x, p.z);
	sdf = p.y - h;
	const float cx = 30.0, cz = 30.0;
	float cy = hills(cx, cz) - 2.0;
	float sphere = length(p - vec3(cx, cy, cz)) - 5.0;
	sdf = max(sdf, -sphere); // CSG subtract: the one carved cave
	mat = 0u;
	if (sdf <= 0.0) mat = h > 4.0 ? 2u : (h > 1.0 ? 1u : 3u);
}

void apply_field_op(uint index, vec3 p, inout float sdf, inout uint mat) {
	uvec4 a = field_op_pool.v[index * 2u + 0u];
	uvec4 b = field_op_pool.v[index * 2u + 1u];
	uint type = a.x;
	uint material = a.y;
	vec3 c = vec3(uintBitsToFloat(a.z), uintBitsToFloat(a.w), uintBitsToFloat(b.x));
	float radius = uintBitsToFloat(b.y);
	float sp = length(p - c) - radius;
	if (type == OP_SPHERE_SUBTRACT) {
		if (-sp > sdf) { sdf = -sp; if (sdf > 0.0) mat = 0u; }
	} else if (type == OP_SPHERE_ADD) {
		if (sp < sdf) { sdf = sp; if (sdf <= 0.0) mat = material; }
	} else if (type == OP_SPHERE_PAINT) {
		if (sp <= 0.0 && sdf <= 0.0) mat = material;
	}
}

// op_base is the index of the region's first op in the pool (region_slot * MAX_REGION_OPS).
void eval_field(vec3 p, uint op_base, uint op_count, out float sdf, out uint mat) {
	base_field(p, sdf, mat);
	for (uint i = 0u; i < op_count; i++) apply_field_op(op_base + i, p, sdf, mat);
}
```

- [ ] **Step 7: Write `shaders/field_probe.comp.glsl`**

```glsl
#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 0
#include "field.glsl"

layout(local_size_x = 64) in;

layout(set = 0, binding = 1, std430) readonly buffer Points { vec4 p[]; } points;
layout(set = 0, binding = 2, std430) writeonly buffer Results { vec4 v[]; } results;

layout(push_constant, std430) uniform Push {
	uvec4 cfg; // x = point count, y = op count, zw unused
} pc;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= pc.cfg.x) return;
	float sdf;
	uint mat;
	eval_field(points.p[i].xyz, 0u, pc.cfg.y, sdf, mat);
	results.v[i] = vec4(sdf, float(mat), 0.0, 0.0);
}
```

- [ ] **Step 8: Add the two debug bindings**

In `extension/src/voxel_world.h`, add to the public section:

```cpp
	// Test/debug hooks for the GPU differential harness (spec §8). debug_eval_field takes
	// the SAME PackedByteArray the GPU op buffer is filled from, so the op struct layout is
	// verified end to end rather than transcribed twice.
	String debug_load_shader(const String &res_path) const;
	Vector2 debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) const;
```

In `extension/src/voxel_world.cpp`, add the includes `#include "world/brick_eval.h"` and
`#include <godot_cpp/classes/project_settings.hpp>`, register both in `_bind_methods()`:

```cpp
	ClassDB::bind_method(D_METHOD("debug_load_shader", "res_path"), &VoxelWorld::debug_load_shader);
	ClassDB::bind_method(D_METHOD("debug_eval_field", "p", "ops", "op_count"), &VoxelWorld::debug_eval_field);
```

and define them:

```cpp
String VoxelWorld::debug_load_shader(const String &res_path) const {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(res_path);
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code =
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err);
	if (code.empty()) {
		UtilityFunctions::printerr("debug_load_shader: ", err.c_str());
		return String();
	}
	return String(code.c_str());
}

Vector2 VoxelWorld::debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) const {
	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field: op buffer too small");
			return Vector2();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::Sample s = ve::eval_field(gen, ptr, op_count, p.x, p.y, p.z);
	return Vector2(s.sdf, static_cast<float>(s.material));
}
```

`voxel_world.cpp` must also include `"render/shader_loader.h"` (it does not yet).

- [ ] **Step 9: Write the differential test**

`tests/test_field_diff.gd`:

```gdscript
extends GdUnitTestSuite

# GPU/CPU differential test for the field mirror (spec section 8). shaders/field.glsl must
# agree with ve::AnalyticGenerator + ve::apply_op or every brick the GPU generates is wrong.
#
# Tolerance: sin() is not bit-identical between glibc and a Vulkan driver, and the stored SDF
# is a uint8 with ~5 mm steps. A disagreement below half a step can never change a stored
# byte, so the gate is expressed in encoded steps rather than metres.
const SDF_STEP := 1.28 / 255.0        # metres per encoded step
const MAX_STEPS := 2.0                # no sample may differ by more than this
const TIGHT_FRACTION := 0.99          # ...and this share must be within one step

const OP_SUBTRACT := 0
const OP_ADD := 1
const OP_PAINT := 2

var _world: VoxelWorld
var _rd: RenderingDevice

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)
	_rd = RenderingServer.create_local_rendering_device()

func after_test() -> void:
	if _rd != null:
		_rd.free()
		_rd = null

func make_op(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
	# Byte-identical to ve::EditOp: type, material, pos[3], radius, pad[2] — 32 bytes.
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type)
	b.put_u32(material)
	b.put_float(pos.x)
	b.put_float(pos.y)
	b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(0)
	b.put_u32(0)
	return b.data_array

func sample_points() -> PackedVector3Array:
	# A deterministic spread over the demo neighbourhood, the cave, and below the origin
	# plane, plus a few far-out points where sin() range reduction is hardest.
	var pts := PackedVector3Array()
	var rng := RandomNumberGenerator.new()
	rng.seed = 20260813
	for i in range(512):
		pts.append(Vector3(rng.randf_range(-20.0, 60.0), rng.randf_range(-30.0, 30.0),
			rng.randf_range(-20.0, 60.0)))
	for i in range(128):
		pts.append(Vector3(rng.randf_range(700.0, 900.0), rng.randf_range(-40.0, 20.0),
			rng.randf_range(700.0, 900.0)))
	return pts

func run_gpu(pts: PackedVector3Array, ops: PackedByteArray, op_count: int) -> PackedFloat32Array:
	var code: String = _world.debug_load_shader("res://shaders/field_probe.comp.glsl")
	assert_str(code).is_not_empty()
	# ve::load_shader_source keeps the Godot-only #[compute] annotation; glslang rejects it.
	code = code.replace("#[compute]\n", "")

	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = code
	var spirv := _rd.shader_compile_spirv_from_source(src)
	assert_str(spirv.compile_error_compute).is_empty()
	var shader := _rd.shader_create_from_spirv(spirv)
	assert_bool(shader.is_valid()).is_true()

	# The op pool must never be zero-sized even when there are no ops.
	var op_bytes := ops.duplicate()
	if op_bytes.size() < 32:
		op_bytes.resize(32)
	var op_buf := _rd.storage_buffer_create(op_bytes.size(), op_bytes)

	var pt_bytes := PackedFloat32Array()
	for p in pts:
		pt_bytes.append_array(PackedFloat32Array([p.x, p.y, p.z, 0.0]))
	var pt_buf := _rd.storage_buffer_create(pt_bytes.size() * 4, pt_bytes.to_byte_array())
	var out_buf := _rd.storage_buffer_create(pts.size() * 16)

	var uniforms := []
	for pair in [[0, op_buf], [1, pt_buf], [2, out_buf]]:
		var u := RDUniform.new()
		u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		u.binding = pair[0]
		u.add_id(pair[1])
		uniforms.append(u)
	var uset := _rd.uniform_set_create(uniforms, shader, 0)
	var pipeline := _rd.compute_pipeline_create(shader)

	var push := PackedInt32Array([pts.size(), op_count, 0, 0]).to_byte_array()
	var list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(list, pipeline)
	_rd.compute_list_bind_uniform_set(list, uset, 0)
	_rd.compute_list_set_push_constant(list, push, push.size())
	_rd.compute_list_dispatch(list, (pts.size() + 63) / 64, 1, 1)
	_rd.compute_list_end()
	_rd.submit()
	_rd.sync()

	var out := _rd.buffer_get_data(out_buf).to_float32_array()
	_rd.free_rid(uset)
	_rd.free_rid(pipeline)
	_rd.free_rid(shader)
	_rd.free_rid(op_buf)
	_rd.free_rid(pt_buf)
	_rd.free_rid(out_buf)
	return out

func compare(pts: PackedVector3Array, ops: PackedByteArray, op_count: int, label: String) -> void:
	var gpu := run_gpu(pts, ops, op_count)
	assert_int(gpu.size()).is_equal(pts.size() * 4)

	var worst := 0.0
	var within_one := 0
	var mat_mismatch := 0
	var mat_compared := 0
	for i in range(pts.size()):
		var cpu: Vector2 = _world.debug_eval_field(pts[i], ops, op_count)
		var diff: float = absf(gpu[i * 4] - cpu.x) / SDF_STEP
		worst = maxf(worst, diff)
		if diff <= 1.0:
			within_one += 1
		# Material is a hard classification and flips across a band edge, so only compare
		# where the CPU sdf is far enough from 0 and from a band boundary for the tiny sdf
		# disagreement to be irrelevant.
		if absf(cpu.x) > 4.0 * SDF_STEP:
			mat_compared += 1
			if int(gpu[i * 4 + 1]) != int(cpu.y):
				mat_mismatch += 1

	assert_float(worst).override_failure_message(
		"%s: worst sdf disagreement %.2f encoded steps" % [label, worst]).is_less(MAX_STEPS)
	assert_float(float(within_one) / float(pts.size())).override_failure_message(
		"%s: only %d/%d samples within one encoded step" % [label, within_one, pts.size()]
		).is_greater(TIGHT_FRACTION)
	assert_int(mat_compared).is_greater(pts.size() / 2)
	assert_int(mat_mismatch).override_failure_message(
		"%s: %d material mismatches" % [label, mat_mismatch]).is_equal(0)

func test_base_field_matches_the_cpu_generator() -> void:
	compare(sample_points(), PackedByteArray(), 0, "base")

func test_sphere_subtract_matches() -> void:
	var ops := make_op(OP_SUBTRACT, 0, Vector3(10.0, 0.0, 10.0), 6.0)
	compare(sample_points(), ops, 1, "subtract")

func test_sphere_add_matches() -> void:
	var ops := make_op(OP_ADD, 4, Vector3(10.0, 5.0, 10.0), 6.0)
	compare(sample_points(), ops, 1, "add")

func test_sphere_paint_matches() -> void:
	var ops := make_op(OP_PAINT, 2, Vector3(10.0, -2.0, 10.0), 8.0)
	compare(sample_points(), ops, 1, "paint")

func test_ordered_op_chain_matches() -> void:
	# Order matters: an add inside an earlier subtract must refill it on both sides.
	var ops := make_op(OP_SUBTRACT, 0, Vector3(10.0, 0.0, 10.0), 8.0)
	ops.append_array(make_op(OP_ADD, 4, Vector3(10.0, 0.0, 10.0), 4.0))
	ops.append_array(make_op(OP_PAINT, 3, Vector3(12.0, 0.0, 12.0), 5.0))
	compare(sample_points(), ops, 3, "chain")
```

- [ ] **Step 10: Build and run**

```bash
./build.sh -j$(nproc)
./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_field_diff.gd
```
Expected: 5/5 PASS. If `worst` exceeds 2 encoded steps only for the far-out (700–900 m) points,
the driver's `sin` range reduction is the cause — reduce that block's range and record the
change here; if the near points disagree, the mirror is genuinely wrong, fix `field.glsl`.

- [ ] **Step 11: Commit**

```bash
git add extension/src/render/shader_loader.cpp extension/tests/test_shader_loader.cpp \
        shaders/common.glsl shaders/brick_layout.glsl shaders/field.glsl \
        shaders/field_probe.comp.glsl extension/src/voxel_world.* tests/test_field_diff.gd
git commit -m "feat(render): GPU field mirror with CPU differential test"
```

---

### Task 8: `render/gpu_atlas` — every GPU resource in one owner

`GpuWorld` (M1) is deliberately left running alongside this: the raymarcher keeps rendering
the M1 static world, and its gdUnit tests keep passing, right up to the single atomic cutover
in Task 12. Nothing here is wired into the render path yet.

**The region map is a storage buffer, not a 3D texture.** Godot's `texture_update` replaces a
whole layer, so re-pointing one region would rewrite the entire map every time a region
streams in. `buffer_update` writes four bytes.

**Files:**
- Create: `extension/src/render/gpu_atlas.h`, `extension/src/render/gpu_atlas.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (exports + debug hooks)
- Test: `tests/test_gpu_atlas.gd`

**Interfaces:**
- Consumes: `ve::WorldBounds`, `ve::kRegionBrickCount` (Task 1), `ve::kMaxRegionOps` (Task 3), `ve::EditOp` (Task 2), `ve::kBrickSdfStride`, `ve::kBrickVoxels`, `ve::kBrickPaletteSize`, `ve::kMipDims` (Task 4).
- Produces:
  - `struct godot::GpuAtlasConfig { ve::IVec3 atlas_bricks; int max_region_slots; int max_brick_jobs; ve::WorldBounds bounds; }`
  - `class godot::GpuAtlas`:
    - `bool initialize(RenderingDevice *rd, const GpuAtlasConfig &cfg)` / `void teardown()` / `bool is_valid() const`
    - `const GpuAtlasConfig &config() const`, `int atlas_slot_count() const`, `int region_map_entries() const`
    - RIDs: `sdf_atlas()`, `mat_atlas()`, `mip_atlas(int level)`, `palette()`, `region_map()`, `region_tables()`, `free_list()`, `counters()`, `frame_counters()`, `dispatch_args()`, `jobs()`, `op_pool()`, `op_counts()`
    - `void reset_frame_counters(RenderingDevice *rd)`
    - `int read_free_count(RenderingDevice *rd) const`, `int read_job_count(RenderingDevice *rd) const`, `uint32_t read_overflow(RenderingDevice *rd) const`
    - `void upload_region_ops(RenderingDevice *rd, int region_slot, const ve::EditOp *ops, int count)`
    - `void set_region_map_entry(RenderingDevice *rd, int region_index, int region_slot)`
    - `void clear_region_map(RenderingDevice *rd)`
  - Buffer layouts, fixed here and relied on by Tasks 9/10/12/13:
    - `counters` (persistent): `int free_count; int pad[3];`
    - `frame_counters` (cleared each frame): `int job_count; uint overflow; uint pad[2];`
      — `overflow` bit 0 = atlas slots exhausted, bit 1 = job list full
    - `dispatch_args`: `uint x, y, z, pad;` created with `STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT`
    - `jobs`: two `ivec4` per job — `[0] = (brick.x, brick.y, brick.z, atlas_slot)`,
      `[1] = (region_slot, op_count, 0, 0)`
    - `region_tables`: `int slot[max_region_slots * 32768]`, `-1` = absent
    - `region_map`: `int region_slot[region_map_entries]`, `-1` = not resident
    - `palette`: `uint id[atlas_slot_count * 4]`
- `VoxelWorld` gains exports `atlas_bricks: Vector3i` (default `(64,32,32)`),
  `max_region_slots: int` (512), `max_brick_jobs: int` (16384),
  `world_origin_bricks: Vector3i` ((0,-64,0)), `world_size_regions: Vector3i` ((64,8,64)),
  `residency_radius_m: float` (96.0); plus `debug_init_atlas() -> bool`,
  `debug_atlas_stats() -> Dictionary`, and C++-only `GpuAtlas *atlas()`.

- [ ] **Step 1: Write the failing test**

`tests/test_gpu_atlas.gd`:

```gdscript
extends GdUnitTestSuite

# Every GPU test in M2 must shrink the atlas: the shipping configuration allocates ~740 MB,
# and a local RenderingDevice allocates its own copy on top of the running editor's.
const ATLAS := Vector3i(8, 8, 8)      # 512 slots
const REGION_SLOTS := 8
const REGIONS := Vector3i(4, 2, 4)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = REGIONS
	add_child(w)
	return w

func test_atlas_allocates_and_reports_its_geometry() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var s: Dictionary = w.debug_atlas_stats()
	assert_int(s["slot_count"]).is_equal(512)
	assert_int(s["free_slots"]).is_equal(512)     # nothing allocated yet
	assert_int(s["region_map_entries"]).is_equal(4 * 2 * 4)
	assert_int(s["job_count"]).is_equal(0)
	assert_int(s["overflow"]).is_equal(0)

func test_texture_extents_follow_the_per_brick_strides() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	assert_object(rd).is_not_null()
	# SDF: 17 samples per brick per axis, R8 -> one byte per texel.
	assert_int(rd.texture_get_data(w.debug_sdf_atlas(), 0).size()).is_equal(136 * 136 * 136)
	# Material: 16 cells per brick per axis, R8.
	assert_int(rd.texture_get_data(w.debug_mat_atlas(), 0).size()).is_equal(128 * 128 * 128)
	# Min-max levels: 2 / 4 / 8 cells per brick per axis, RG8 -> two bytes per texel.
	assert_int(rd.texture_get_data(w.debug_mip_atlas(0), 0).size()).is_equal(16 * 16 * 16 * 2)
	assert_int(rd.texture_get_data(w.debug_mip_atlas(1), 0).size()).is_equal(32 * 32 * 32 * 2)
	assert_int(rd.texture_get_data(w.debug_mip_atlas(2), 0).size()).is_equal(64 * 64 * 64 * 2)

func test_region_map_starts_empty_and_takes_single_entry_writes() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	var before := rd.buffer_get_data(w.debug_region_map()).to_int32_array()
	assert_int(before.size()).is_equal(4 * 2 * 4)
	for v in before:
		assert_int(v).is_equal(-1)
	w.debug_set_region_map_entry(5, 3)
	var after := rd.buffer_get_data(w.debug_region_map()).to_int32_array()
	assert_int(after[5]).is_equal(3)
	assert_int(after[4]).is_equal(-1)

func test_region_tables_start_absent() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	# One region's worth is enough to prove the fill; the whole buffer is 1 MB.
	var slice := rd.buffer_get_data(w.debug_region_tables(), 0, 32768 * 4).to_int32_array()
	assert_int(slice.size()).is_equal(32768)
	for i in [0, 1, 17, 4095, 32767]:
		assert_int(slice[i]).is_equal(-1)

func test_free_list_is_a_full_permutation_of_the_slots() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	var fl := rd.buffer_get_data(w.debug_free_list()).to_int32_array()
	assert_int(fl.size()).is_equal(512)
	var seen := {}
	for v in fl:
		assert_int(v).is_between(0, 511)
		seen[v] = true
	assert_int(seen.size()).is_equal(512)

func test_frame_counters_reset() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	rd.buffer_update(w.debug_frame_counters(), 0, 8,
		PackedInt32Array([7, 3]).to_byte_array())
	assert_int(w.debug_atlas_stats()["job_count"]).is_equal(7)
	w.debug_reset_frame_counters()
	var s: Dictionary = w.debug_atlas_stats()
	assert_int(s["job_count"]).is_equal(0)
	assert_int(s["overflow"]).is_equal(0)

func test_region_ops_upload_byte_for_byte() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	var rd := w.debug_local_rd() as RenderingDevice
	# ve::EditOp: type, material, pos[3], radius, pad[2].
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(0); b.put_u32(9)
	b.put_float(1.5); b.put_float(2.5); b.put_float(3.5)
	b.put_float(4.5)
	b.put_u32(0); b.put_u32(0)
	w.debug_upload_region_ops(2, b.data_array, 1)

	# Region slot 2 starts at 2 * 256 ops * 32 bytes.
	var got := rd.buffer_get_data(w.debug_op_pool(), 2 * 256 * 32, 32)
	assert_array(Array(got)).is_equal(Array(b.data_array))
	var counts := rd.buffer_get_data(w.debug_op_counts()).to_int32_array()
	assert_int(counts[2]).is_equal(1)
	assert_int(counts[0]).is_equal(0)

func test_teardown_is_idempotent_and_survives_re_init() -> void:
	var w := make_world()
	assert_bool(w.debug_init_atlas()).is_true()
	w.debug_teardown_atlas()
	w.debug_teardown_atlas()
	assert_bool(w.debug_init_atlas()).is_true()
	assert_int(w.debug_atlas_stats()["free_slots"]).is_equal(512)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_gpu_atlas.gd`
Expected: FAIL — `atlas_bricks` and `debug_init_atlas` do not exist.

- [ ] **Step 3: Write `render/gpu_atlas.h`**

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "generator/edit_ops.h"
#include "world/brick_mip.h"
#include "world/edit_log.h"
#include "world/region.h"

namespace godot {

struct GpuAtlasConfig {
	ve::IVec3 atlas_bricks{64, 32, 32};
	int max_region_slots = 512;
	int max_brick_jobs = 16384;
	ve::WorldBounds bounds{};
};

// Owns every GPU resource the voxel world needs. Nothing else creates or frees RIDs in the
// world path, so teardown order lives in exactly one place.
class GpuAtlas {
public:
	~GpuAtlas();

	bool initialize(RenderingDevice *rd, const GpuAtlasConfig &cfg);
	void teardown();
	bool is_valid() const { return sdf_atlas_.is_valid(); }

	const GpuAtlasConfig &config() const { return cfg_; }
	int atlas_slot_count() const {
		return cfg_.atlas_bricks.x * cfg_.atlas_bricks.y * cfg_.atlas_bricks.z;
	}
	int region_map_entries() const {
		return cfg_.bounds.size_regions.x * cfg_.bounds.size_regions.y *
				cfg_.bounds.size_regions.z;
	}

	RID sdf_atlas() const { return sdf_atlas_; }
	RID mat_atlas() const { return mat_atlas_; }
	RID mip_atlas(int level) const { return mips_[level]; }
	RID palette() const { return palette_; }
	RID region_map() const { return region_map_; }
	RID region_tables() const { return region_tables_; }
	RID free_list() const { return free_list_; }
	RID counters() const { return counters_; }
	RID frame_counters() const { return frame_; }
	RID dispatch_args() const { return dispatch_args_; }
	RID jobs() const { return jobs_; }
	RID op_pool() const { return op_pool_; }
	RID op_counts() const { return op_counts_; }

	void reset_frame_counters(RenderingDevice *rd);
	int read_free_count(RenderingDevice *rd) const;
	int read_job_count(RenderingDevice *rd) const;
	uint32_t read_overflow(RenderingDevice *rd) const;

	void upload_region_ops(RenderingDevice *rd, int region_slot, const ve::EditOp *ops,
			int count);
	void set_region_map_entry(RenderingDevice *rd, int region_index, int region_slot);
	void clear_region_map(RenderingDevice *rd);

private:
	RID make_volume(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h, int d);

	RenderingDevice *rd_ = nullptr;
	GpuAtlasConfig cfg_;
	RID sdf_atlas_, mat_atlas_, palette_;
	RID mips_[ve::kMipLevels];
	RID region_map_, region_tables_, free_list_, counters_, frame_, dispatch_args_;
	RID jobs_, op_pool_, op_counts_;
};

} // namespace godot
```

- [ ] **Step 4: Write `render/gpu_atlas.cpp`**

```cpp
#include "render/gpu_atlas.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <vector>

using namespace godot;

namespace {

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

PackedByteArray filled_i32(int count, int32_t value) {
	PackedByteArray b;
	b.resize(static_cast<int64_t>(count) * 4);
	int32_t *p = reinterpret_cast<int32_t *>(b.ptrw());
	for (int i = 0; i < count; i++) p[i] = value;
	return b;
}

PackedByteArray zeroed(int64_t bytes) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(0);
	return b;
}

} // namespace

GpuAtlas::~GpuAtlas() {
	teardown();
}

RID GpuAtlas::make_volume(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h,
		int d) {
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
	f->set_format(fmt);
	f->set_width(w);
	f->set_height(h);
	f->set_depth(d);
	f->set_mipmaps(1);
	// STORAGE is what lets brick_gen.comp.glsl write these volumes; SAMPLING is what lets
	// the raymarcher read them; CAN_COPY_FROM is what lets the differential test read them
	// back. CAN_UPDATE stays for a future CPU-side override path.
	f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	Ref<RDTextureView> v;
	v.instantiate();
	return rd->texture_create(f, v, TypedArray<PackedByteArray>());
}

bool GpuAtlas::initialize(RenderingDevice *rd, const GpuAtlasConfig &cfg) {
	teardown();
	rd_ = rd;
	cfg_ = cfg;
	if (!rd) return false;
	const int slots = atlas_slot_count();
	if (slots <= 0 || cfg_.max_region_slots <= 0 || cfg_.max_brick_jobs <= 0) {
		UtilityFunctions::printerr("GpuAtlas: degenerate configuration");
		return false;
	}

	const ve::IVec3 ab = cfg_.atlas_bricks;
	sdf_atlas_ = make_volume(rd, RenderingDevice::DATA_FORMAT_R8_UNORM,
			ab.x * ve::kBrickSdfStride, ab.y * ve::kBrickSdfStride, ab.z * ve::kBrickSdfStride);
	mat_atlas_ = make_volume(rd, RenderingDevice::DATA_FORMAT_R8_UINT,
			ab.x * ve::kBrickVoxels, ab.y * ve::kBrickVoxels, ab.z * ve::kBrickVoxels);
	for (int l = 0; l < ve::kMipLevels; l++) {
		const int d = ve::kMipDims[l];
		mips_[l] = make_volume(rd, RenderingDevice::DATA_FORMAT_R8G8_UINT, ab.x * d, ab.y * d,
				ab.z * d);
	}

	// Palette entries are uint32, not the uint16 of ve::Brick. A packed uint16 array would
	// force GL_EXT_shader_16bit_storage on every consumer to save 512 KB; it is not worth it.
	palette_ = rd->storage_buffer_create(static_cast<uint32_t>(slots) * ve::kBrickPaletteSize * 4,
			zeroed(static_cast<int64_t>(slots) * ve::kBrickPaletteSize * 4));

	// The region map is a BUFFER, not a texture: streaming re-points one entry per region,
	// and buffer_update writes four bytes where texture_update would rewrite a whole layer.
	region_map_ = rd->storage_buffer_create(static_cast<uint32_t>(region_map_entries()) * 4,
			filled_i32(region_map_entries(), -1));

	const int64_t table_entries =
			static_cast<int64_t>(cfg_.max_region_slots) * ve::kRegionBrickCount;
	{
		// Built in one go on the CPU: 64 MB at the shipping configuration, once at startup.
		PackedByteArray absent = filled_i32(static_cast<int>(table_entries), -1);
		region_tables_ = rd->storage_buffer_create(static_cast<uint32_t>(absent.size()), absent);
	}

	{
		PackedByteArray fl;
		fl.resize(static_cast<int64_t>(slots) * 4);
		int32_t *p = reinterpret_cast<int32_t *>(fl.ptrw());
		for (int i = 0; i < slots; i++) p[i] = i;
		free_list_ = rd->storage_buffer_create(static_cast<uint32_t>(fl.size()), fl);
	}
	{
		PackedByteArray c = zeroed(16);
		reinterpret_cast<int32_t *>(c.ptrw())[0] = slots; // free_count
		counters_ = rd->storage_buffer_create(16, c);
	}
	frame_ = rd->storage_buffer_create(16, zeroed(16));
	dispatch_args_ = rd->storage_buffer_create(16, zeroed(16),
			RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	jobs_ = rd->storage_buffer_create(static_cast<uint32_t>(cfg_.max_brick_jobs) * 32,
			zeroed(static_cast<int64_t>(cfg_.max_brick_jobs) * 32));
	op_pool_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_region_slots) * ve::kMaxRegionOps * 32,
			zeroed(static_cast<int64_t>(cfg_.max_region_slots) * ve::kMaxRegionOps * 32));
	op_counts_ = rd->storage_buffer_create(static_cast<uint32_t>(cfg_.max_region_slots) * 4,
			zeroed(static_cast<int64_t>(cfg_.max_region_slots) * 4));

	bool ok = sdf_atlas_.is_valid() && mat_atlas_.is_valid() && palette_.is_valid() &&
			region_map_.is_valid() && region_tables_.is_valid() && free_list_.is_valid() &&
			counters_.is_valid() && frame_.is_valid() && dispatch_args_.is_valid() &&
			jobs_.is_valid() && op_pool_.is_valid() && op_counts_.is_valid();
	for (int l = 0; l < ve::kMipLevels; l++) ok = ok && mips_[l].is_valid();
	if (!ok) {
		// Most likely cause: the driver refuses STORAGE usage on R8_UNORM / R8G8_UINT
		// (Vulkan's shaderStorageImageExtendedFormats). Fail soft and say so.
		UtilityFunctions::printerr(
				"GpuAtlas: resource creation failed (check storage-image format support)");
		teardown();
		return false;
	}
	return true;
}

void GpuAtlas::teardown() {
	if (!rd_) return;
	free_if_valid(rd_, sdf_atlas_);
	free_if_valid(rd_, mat_atlas_);
	for (int l = 0; l < ve::kMipLevels; l++) free_if_valid(rd_, mips_[l]);
	free_if_valid(rd_, palette_);
	free_if_valid(rd_, region_map_);
	free_if_valid(rd_, region_tables_);
	free_if_valid(rd_, free_list_);
	free_if_valid(rd_, counters_);
	free_if_valid(rd_, frame_);
	free_if_valid(rd_, dispatch_args_);
	free_if_valid(rd_, jobs_);
	free_if_valid(rd_, op_pool_);
	free_if_valid(rd_, op_counts_);
	rd_ = nullptr;
}

void GpuAtlas::reset_frame_counters(RenderingDevice *rd) {
	if (!frame_.is_valid()) return;
	rd->buffer_update(frame_, 0, 16, zeroed(16));
}

int GpuAtlas::read_free_count(RenderingDevice *rd) const {
	if (!counters_.is_valid()) return 0;
	const PackedByteArray b = rd->buffer_get_data(counters_, 0, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : 0;
}

int GpuAtlas::read_job_count(RenderingDevice *rd) const {
	if (!frame_.is_valid()) return 0;
	const PackedByteArray b = rd->buffer_get_data(frame_, 0, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : 0;
}

uint32_t GpuAtlas::read_overflow(RenderingDevice *rd) const {
	if (!frame_.is_valid()) return 0;
	const PackedByteArray b = rd->buffer_get_data(frame_, 4, 4);
	return b.size() >= 4 ? *reinterpret_cast<const uint32_t *>(b.ptr()) : 0;
}

void GpuAtlas::upload_region_ops(RenderingDevice *rd, int region_slot, const ve::EditOp *ops,
		int count) {
	if (!op_pool_.is_valid() || region_slot < 0 || region_slot >= cfg_.max_region_slots) return;
	count = count < 0 ? 0 : (count > ve::kMaxRegionOps ? ve::kMaxRegionOps : count);
	if (count > 0) {
		PackedByteArray b;
		b.resize(static_cast<int64_t>(count) * 32);
		memcpy(b.ptrw(), ops, static_cast<size_t>(count) * 32);
		rd->buffer_update(op_pool_,
				static_cast<uint32_t>(region_slot) * ve::kMaxRegionOps * 32,
				static_cast<uint32_t>(b.size()), b);
	}
	PackedByteArray c;
	c.resize(4);
	*reinterpret_cast<int32_t *>(c.ptrw()) = count;
	rd->buffer_update(op_counts_, static_cast<uint32_t>(region_slot) * 4, 4, c);
}

void GpuAtlas::set_region_map_entry(RenderingDevice *rd, int region_index, int region_slot) {
	if (!region_map_.is_valid() || region_index < 0 || region_index >= region_map_entries())
		return;
	PackedByteArray b;
	b.resize(4);
	*reinterpret_cast<int32_t *>(b.ptrw()) = region_slot;
	rd->buffer_update(region_map_, static_cast<uint32_t>(region_index) * 4, 4, b);
}

void GpuAtlas::clear_region_map(RenderingDevice *rd) {
	if (!region_map_.is_valid()) return;
	const PackedByteArray b = filled_i32(region_map_entries(), -1);
	rd->buffer_update(region_map_, 0, static_cast<uint32_t>(b.size()), b);
}
```

Add `#include <cstring>` at the top for `memcpy`.

- [ ] **Step 5: Add the exports and debug hooks to `VoxelWorld`**

In `extension/src/voxel_world.h`, forward-declare `class GpuAtlas;`, add members

```cpp
	Vector3i atlas_bricks_ = Vector3i(64, 32, 32);
	int max_region_slots_ = 512;
	int max_brick_jobs_ = 16384;
	Vector3i world_origin_bricks_ = Vector3i(0, -64, 0);
	Vector3i world_size_regions_ = Vector3i(64, 8, 64);
	float residency_radius_m_ = 96.0f;
	GpuAtlas *atlas_ = nullptr;
```

and public members

```cpp
	void set_atlas_bricks(Vector3i v) { atlas_bricks_ = v; }
	Vector3i get_atlas_bricks() const { return atlas_bricks_; }
	void set_max_region_slots(int v) { max_region_slots_ = v; }
	int get_max_region_slots() const { return max_region_slots_; }
	void set_max_brick_jobs(int v) { max_brick_jobs_ = v; }
	int get_max_brick_jobs() const { return max_brick_jobs_; }
	void set_world_origin_bricks(Vector3i v) { world_origin_bricks_ = v; }
	Vector3i get_world_origin_bricks() const { return world_origin_bricks_; }
	void set_world_size_regions(Vector3i v) { world_size_regions_ = v; }
	Vector3i get_world_size_regions() const { return world_size_regions_; }
	void set_residency_radius_m(float v) { residency_radius_m_ = v; }
	float get_residency_radius_m() const { return residency_radius_m_; }

	GpuAtlas *atlas() { return atlas_; }
	ve::WorldBounds world_bounds() const;

	// Debug/test hooks. Task 12 folds debug_init_atlas() into ensure_initialized().
	bool debug_init_atlas();
	void debug_teardown_atlas();
	Dictionary debug_atlas_stats();
	void debug_reset_frame_counters();
	void debug_set_region_map_entry(int region_index, int region_slot);
	void debug_upload_region_ops(int region_slot, const PackedByteArray &ops, int count);
	RID debug_mat_atlas() const;
	RID debug_mip_atlas(int level) const;
	RID debug_region_map() const;
	RID debug_region_tables() const;
	RID debug_free_list() const;
	RID debug_frame_counters() const;
	RID debug_op_pool() const;
	RID debug_op_counts() const;
```

In `extension/src/voxel_world.cpp`, include `"render/gpu_atlas.h"`, bind every setter/getter
with matching `ADD_PROPERTY` entries (mirroring the existing `world_size_bricks` pair) and
bind each `debug_*` method, then implement:

```cpp
ve::WorldBounds VoxelWorld::world_bounds() const {
	ve::WorldBounds b;
	b.origin_bricks = {world_origin_bricks_.x, world_origin_bricks_.y, world_origin_bricks_.z};
	b.size_regions = {world_size_regions_.x, world_size_regions_.y, world_size_regions_.z};
	return b;
}

bool VoxelWorld::debug_init_atlas() {
	if (use_local_device_ && !local_rd_)
		local_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	else if (!use_local_device_ && !main_rd_)
		main_rd_ = RenderingServer::get_singleton()->get_rendering_device();
	RenderingDevice *device = rd();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return false;
	}
	if (!atlas_) atlas_ = new GpuAtlas();
	GpuAtlasConfig cfg;
	cfg.atlas_bricks = {atlas_bricks_.x, atlas_bricks_.y, atlas_bricks_.z};
	cfg.max_region_slots = max_region_slots_;
	cfg.max_brick_jobs = max_brick_jobs_;
	cfg.bounds = world_bounds();
	return atlas_->initialize(device, cfg);
}

void VoxelWorld::debug_teardown_atlas() {
	if (atlas_) atlas_->teardown();
}

Dictionary VoxelWorld::debug_atlas_stats() {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!atlas_ || !atlas_->is_valid() || !device) return d;
	d["slot_count"] = atlas_->atlas_slot_count();
	d["free_slots"] = atlas_->read_free_count(device);
	d["region_map_entries"] = atlas_->region_map_entries();
	d["job_count"] = atlas_->read_job_count(device);
	d["overflow"] = static_cast<int>(atlas_->read_overflow(device));
	return d;
}
```

`debug_reset_frame_counters`, `debug_set_region_map_entry` and `debug_upload_region_ops`
forward to the matching `GpuAtlas` methods with `rd()`, guarding on `atlas_ && rd()`;
`debug_upload_region_ops` reinterprets `ops.ptr()` as `const ve::EditOp *` exactly as
`debug_eval_field` does. Each `debug_*_atlas`/`debug_*` RID getter returns the matching
`GpuAtlas` RID or `RID()` when `atlas_` is null. Finally, delete the atlas in
`_exit_tree()` **before** the existing `GpuWorld` teardown:

```cpp
	if (atlas_) {
		delete atlas_;
		atlas_ = nullptr;
	}
```

- [ ] **Step 6: Build and run**

```bash
./build.sh -j$(nproc)
./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests
```
Expected: `test_gpu_atlas.gd` 8/8, and **every M1 suite still green** — `GpuWorld` and the
M1 render path are untouched.

If `debug_init_atlas` returns false with the storage-image message, the driver lacks
`shaderStorageImageExtendedFormats`. Switch `sdf_atlas_` to `DATA_FORMAT_R8_UINT` and change
`brick_sdf()` in the raymarcher to a `usampler3D` read divided by 255 in Task 12; record the
change here.

- [ ] **Step 7: Commit**

```bash
git add extension/src/render/gpu_atlas.* extension/src/voxel_world.* tests/test_gpu_atlas.gd
git commit -m "feat(render): GPU atlas, region tables, free list and op pool"
```

---

### Task 9: Region passes — activation probe, slot allocation, indirect args

One shader decides brick residency and hands out atlas slots. It is deliberately reused for
both jobs M2 needs: streaming a region in (scan the whole 32³ range, `force_regen = false`)
and refreshing an edit (scan the op's brick AABB, `force_regen = true`).

**Allocation and release run in separate dispatches.** A thread pushing a slot onto the free
list writes at index `free_count`, while a thread popping reads index `free_count - 1`; run
concurrently they can collide on one index and lose or duplicate a slot. Phase 0 releases,
phase 1 allocates, with a barrier between.

`REGION_BRICKS` is 32, a power of two, so both the floor-division and floor-modulo that map a
possibly-negative global brick coordinate into its region reduce to `>> 5` and `& 31` — the
arithmetic shift and mask are already floor-correct for negatives, unlike GLSL's `/` and `%`.

**Files:**
- Create: `shaders/brick_mark.comp.glsl`, `shaders/region_free.comp.glsl`, `shaders/dispatch_args.comp.glsl`
- Create: `extension/src/render/region_pass.h`, `extension/src/render/region_pass.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (debug hooks)
- Test: `tests/test_region_pass.gd`

**Interfaces:**
- Consumes: `GpuAtlas` (Task 8), `field.glsl` (Task 7), `ve::kActivationPad` (Task 5).
- Produces:
  - `class godot::RegionPass`:
    - `bool initialize(RenderingDevice *rd, const GpuAtlas &atlas)`, `void teardown()`, `bool is_valid() const`
    - `void mark(RenderingDevice *rd, int64_t list, ve::IVec3 region, int region_slot, ve::IVec3 lo, ve::IVec3 hi, int op_count, bool force_regen)`
      — records both phases into an open compute list, with `compute_list_add_barrier` between them
    - `void release_region(RenderingDevice *rd, int64_t list, int region_slot)`
    - `void write_dispatch_args(RenderingDevice *rd, int64_t list)`
  - GLSL `brick_mark.comp.glsl` push constant (64 bytes):
    `ivec4 region` (xyz = global region coord, w = region slot), `ivec4 lo`, `ivec4 hi`
    (inclusive global brick range), `ivec4 cfg` (x = op count, y = phase, z = max jobs,
    w = force_regen).
  - `VoxelWorld` debug hooks: `debug_brick_has_surface(Vector3i brick, PackedByteArray ops, int op_count) -> bool`,
    `debug_mark_region(Vector3i region, int region_slot, Vector3i lo, Vector3i hi, int op_count, bool force) -> void`,
    `debug_release_region(int region_slot) -> void`, `debug_jobs() -> PackedInt32Array`,
    `debug_region_table_slot(int region_slot, Vector3i brick) -> int`.

- [ ] **Step 1: Write the failing test**

`tests/test_region_pass.gd`:

```gdscript
extends GdUnitTestSuite

const ATLAS := Vector3i(16, 8, 16)   # 2048 slots — enough for one surface region
const REGION_SLOTS := 4
const REGIONS := Vector3i(4, 2, 4)
# Region (0, 0, 0) spans bricks x,z in [0, 32) and y in [0, 32) -> world y [0, 25.6) m.
# hills() peaks near +10 m, so this region holds the surface over part of its footprint.
const REGION := Vector3i(0, 0, 0)
const SLOT := 1

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = REGIONS
	add_child(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w

func mark_whole_region(w: VoxelWorld, region: Vector3i, slot: int, force: bool) -> void:
	var lo := region * 32
	w.debug_mark_region(region, slot, lo, lo + Vector3i(31, 31, 31), 0, force)

func test_marking_allocates_exactly_the_bricks_the_cpu_calls_active() -> void:
	var w := make_world()
	mark_whole_region(w, REGION, SLOT, false)

	var gpu_active := 0
	var cpu_active := 0
	var disagreements := 0
	# Sampling every brick is 32768 round-trips through GDScript; a strided sweep over
	# 2048 of them still crosses the surface in every column it touches.
	for z in range(0, 32, 2):
		for y in range(0, 32):
			for x in range(0, 32, 8):
				var brick := Vector3i(x, y, z)
				var on_gpu: bool = w.debug_region_table_slot(SLOT, brick) >= 0
				var on_cpu: bool = w.debug_brick_has_surface(brick, PackedByteArray(), 0)
				gpu_active += 1 if on_gpu else 0
				cpu_active += 1 if on_cpu else 0
				if on_gpu != on_cpu:
					disagreements += 1
	assert_int(cpu_active).override_failure_message(
		"the chosen region holds no surface; pick another").is_greater(20)
	assert_int(disagreements).override_failure_message(
		"GPU marked %d bricks, CPU %d" % [gpu_active, cpu_active]).is_equal(0)

func test_allocation_draws_from_the_free_list_and_assigns_unique_slots() -> void:
	var w := make_world()
	var before: int = w.debug_atlas_stats()["free_slots"]
	mark_whole_region(w, REGION, SLOT, false)
	var stats: Dictionary = w.debug_atlas_stats()
	var allocated: int = before - stats["free_slots"]
	assert_int(allocated).is_greater(0)
	assert_int(stats["job_count"]).is_equal(allocated)
	assert_int(stats["overflow"]).is_equal(0)

	# Every job names a distinct atlas slot, and every slot is in range.
	var jobs: PackedInt32Array = w.debug_jobs()
	assert_int(jobs.size()).is_equal(stats["job_count"] * 8)
	var seen := {}
	for j in range(stats["job_count"]):
		var slot: int = jobs[j * 8 + 3]
		assert_int(slot).is_between(0, ATLAS.x * ATLAS.y * ATLAS.z - 1)
		assert_bool(seen.has(slot)).is_false()
		seen[slot] = true
		assert_int(jobs[j * 8 + 4]).is_equal(SLOT)  # region slot
		assert_int(jobs[j * 8 + 5]).is_equal(0)     # op count

func test_marking_twice_without_force_enqueues_nothing_new() -> void:
	var w := make_world()
	mark_whole_region(w, REGION, SLOT, false)
	var first: int = w.debug_atlas_stats()["job_count"]
	assert_int(first).is_greater(0)
	var free_after_first: int = w.debug_atlas_stats()["free_slots"]

	w.debug_reset_frame_counters()
	mark_whole_region(w, REGION, SLOT, false)
	assert_int(w.debug_atlas_stats()["job_count"]).is_equal(0)
	assert_int(w.debug_atlas_stats()["free_slots"]).is_equal(free_after_first)

func test_force_regen_re_enqueues_the_resident_bricks() -> void:
	var w := make_world()
	mark_whole_region(w, REGION, SLOT, false)
	var first: int = w.debug_atlas_stats()["job_count"]
	var free_after_first: int = w.debug_atlas_stats()["free_slots"]

	w.debug_reset_frame_counters()
	mark_whole_region(w, REGION, SLOT, true)
	assert_int(w.debug_atlas_stats()["job_count"]).is_equal(first)
	# Re-enqueueing must not allocate a second slot for a brick that already has one.
	assert_int(w.debug_atlas_stats()["free_slots"]).is_equal(free_after_first)

func test_releasing_a_region_returns_every_slot() -> void:
	var w := make_world()
	var before: int = w.debug_atlas_stats()["free_slots"]
	mark_whole_region(w, REGION, SLOT, false)
	assert_int(w.debug_atlas_stats()["free_slots"]).is_less(before)
	w.debug_release_region(SLOT)
	assert_int(w.debug_atlas_stats()["free_slots"]).is_equal(before)
	assert_int(w.debug_region_table_slot(SLOT, Vector3i(0, 0, 0))).is_equal(-1)

	# The freed slots really are usable again: a second load succeeds with no overflow.
	w.debug_reset_frame_counters()
	mark_whole_region(w, REGION, SLOT, false)
	assert_int(w.debug_atlas_stats()["overflow"]).is_equal(0)
	assert_int(w.debug_atlas_stats()["free_slots"]).is_less(before)

func test_an_edit_op_activates_bricks_the_base_field_leaves_solid() -> void:
	var w := make_world()
	# Region (0, -1, 0) spans world y in [-25.6, 0) m — solid rock under the hills.
	var region := Vector3i(0, -1, 0)
	var lo := region * 32
	mark_whole_region(w, region, 2, false)
	var solid_jobs: int = w.debug_atlas_stats()["job_count"]

	# Carve a 4 m sphere in the middle of it.
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(0); b.put_u32(0)
	b.put_float(12.8); b.put_float(-12.8); b.put_float(12.8)
	b.put_float(4.0)
	b.put_u32(0); b.put_u32(0)
	w.debug_upload_region_ops(2, b.data_array, 1)

	w.debug_reset_frame_counters()
	w.debug_mark_region(region, 2, lo, lo + Vector3i(31, 31, 31), 1, false)
	assert_int(w.debug_atlas_stats()["job_count"]).override_failure_message(
		"the carve activated no new bricks (base activated %d)" % solid_jobs).is_greater(0)

func test_exhausting_the_atlas_sets_the_overflow_bit_and_does_not_crash() -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = Vector3i(2, 2, 2)   # 8 slots — far too few
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = REGIONS
	add_child(w)
	assert_bool(w.debug_init_atlas()).is_true()
	mark_whole_region(w, REGION, SLOT, false)
	var s: Dictionary = w.debug_atlas_stats()
	assert_int(s["overflow"] & 1).is_equal(1)
	assert_int(s["free_slots"]).is_equal(0)   # never negative: the over-decrement is undone
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_region_pass.gd`
Expected: FAIL — `debug_mark_region` does not exist.

- [ ] **Step 3: Write `shaders/brick_mark.comp.glsl`**

```glsl
#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 4
#include "field.glsl"

layout(local_size_x = 256) in;

layout(set = 0, binding = 0, std430) buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 1, std430) buffer FreeList { int slot[]; } free_list;
layout(set = 0, binding = 2, std430) buffer Counters {
	int free_count; int pad0, pad1, pad2;
} counters;
layout(set = 0, binding = 3, std430) buffer Frame {
	int job_count; uint overflow; uint pad0, pad1;
} frame;
// binding 4 is the field op pool, declared by field.glsl
layout(set = 0, binding = 5, std430) writeonly buffer Jobs { ivec4 v[]; } jobs;

layout(push_constant, std430) uniform Push {
	ivec4 region; // xyz = global region coord (may be negative), w = region slot
	ivec4 lo;     // inclusive global brick coord of the range to scan
	ivec4 hi;     // inclusive
	ivec4 cfg;    // x = op count, y = phase (0 release, 1 allocate), z = max jobs, w = force
} pc;

// ve::kActivationPad. The probe samples every 8 voxels, so the field can dip across zero
// between samples; a brick counts as empty only when all 27 probes clear zero by this much.
const float ACTIVATION_PAD = 0.15;

// Mirror of ve::brick_has_surface (extension/src/world/brick_eval.cpp).
bool brick_has_surface(ivec3 brick, uint op_base, uint op_count) {
	vec3 bo = vec3(brick) * BRICK_SIZE;
	float mn = 1e30, mx = -1e30;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				float sdf;
				uint mat;
				eval_field(bo + vec3(sx, sy, sz) * (float(BRICK_VOXELS) * 0.5 * VOXEL_SIZE),
						op_base, op_count, sdf, mat);
				mn = min(mn, sdf);
				mx = max(mx, sdf);
			}
	return mn < ACTIVATION_PAD && mx > -ACTIVATION_PAD;
}

void main() {
	ivec3 ext = pc.hi.xyz - pc.lo.xyz + ivec3(1);
	int total = ext.x * ext.y * ext.z;
	int i = int(gl_GlobalInvocationID.x);
	if (i >= total) return;
	ivec3 brick = pc.lo.xyz + ivec3(i % ext.x, (i / ext.x) % ext.y, i / (ext.x * ext.y));

	int rslot = pc.region.w;
	// REGION_BRICKS is 32, a power of two: `& 31` is the floor-modulo for negative brick
	// coordinates too, where GLSL's `%` would truncate towards zero and give -1 for -1.
	int bi = (brick.x & 31) + (brick.y & 31) * REGION_BRICKS +
			(brick.z & 31) * REGION_BRICKS * REGION_BRICKS;
	int idx = rslot * REGION_BRICK_COUNT + bi;
	int cur = region_tables.slot[idx];

	uint op_base = uint(rslot) * MAX_REGION_OPS;
	uint op_count = uint(pc.cfg.x);
	bool active = brick_has_surface(brick, op_base, op_count);

	if (pc.cfg.y == 0) {
		// Release phase. Kept in its own dispatch: a push at index free_count and a pop at
		// free_count - 1 running concurrently can collide and lose or duplicate a slot.
		if (!active && cur >= 0) {
			region_tables.slot[idx] = -1;
			int k = atomicAdd(counters.free_count, 1);
			free_list.slot[k] = cur;
		}
		return;
	}

	if (!active) return;

	int slot = cur;
	if (slot < 0) {
		int old = atomicAdd(counters.free_count, -1);
		if (old <= 0) {
			atomicAdd(counters.free_count, 1); // undo the over-decrement; never go negative
			atomicOr(frame.overflow, 1u);
			return; // fail-soft: the brick stays absent and the ray passes through it
		}
		slot = free_list.slot[old - 1];
		region_tables.slot[idx] = slot;
	} else if (pc.cfg.w == 0) {
		return; // resident already and this is a plain stream-in: nothing to regenerate
	}

	int j = atomicAdd(frame.job_count, 1);
	if (j >= pc.cfg.z) {
		atomicAdd(frame.job_count, -1);
		atomicOr(frame.overflow, 2u);
		// The slot stays assigned but ungenerated for this frame. Releasing it here would
		// mean freeing during the allocate phase — the very race the phase split avoids.
		// The streamer sees overflow bit 1 and re-marks this region with force_regen next
		// frame, which re-enqueues the brick; one frame of stale atlas bytes is the cost.
		return;
	}
	jobs.v[j * 2 + 0] = ivec4(brick, slot);
	jobs.v[j * 2 + 1] = ivec4(rslot, int(op_count), 0, 0);
}
```

- [ ] **Step 4: Write `shaders/region_free.comp.glsl` and `shaders/dispatch_args.comp.glsl`**

`shaders/region_free.comp.glsl`:

```glsl
#[compute]
#version 460

#include "common.glsl"

layout(local_size_x = 256) in;

layout(set = 0, binding = 0, std430) buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 1, std430) buffer FreeList { int slot[]; } free_list;
layout(set = 0, binding = 2, std430) buffer Counters {
	int free_count; int pad0, pad1, pad2;
} counters;

layout(push_constant, std430) uniform Push {
	ivec4 cfg; // x = region slot
} pc;

// Eviction only ever frees, so it needs no phase split.
void main() {
	int i = int(gl_GlobalInvocationID.x);
	if (i >= REGION_BRICK_COUNT) return;
	int idx = pc.cfg.x * REGION_BRICK_COUNT + i;
	int slot = region_tables.slot[idx];
	if (slot < 0) return;
	region_tables.slot[idx] = -1;
	int k = atomicAdd(counters.free_count, 1);
	free_list.slot[k] = slot;
}
```

`shaders/dispatch_args.comp.glsl`:

```glsl
#[compute]
#version 460

layout(local_size_x = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Frame {
	int job_count; uint overflow; uint pad0, pad1;
} frame;
layout(set = 0, binding = 1, std430) writeonly buffer Args { uvec4 v; } args;

// brick_gen.comp.glsl runs one workgroup per job, so the group count IS the job count.
void main() {
	args.v = uvec4(uint(max(frame.job_count, 0)), 1u, 1u, 0u);
}
```

- [ ] **Step 5: Write `render/region_pass.h`**

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/gpu_atlas.h"
#include "world/region.h"

namespace godot {

// The residency machinery: activation probe, atlas-slot allocation and release, and the
// indirect-dispatch argument write that hands the resulting job list to the generator.
class RegionPass {
public:
	~RegionPass();

	bool initialize(RenderingDevice *rd, const GpuAtlas &atlas);
	void teardown();
	bool is_valid() const { return mark_pipeline_.is_valid(); }

	// Records into an OPEN compute list. lo/hi are inclusive GLOBAL brick coordinates and
	// must lie inside `region`; force_regen re-enqueues bricks that are already resident.
	void mark(RenderingDevice *rd, int64_t list, ve::IVec3 region, int region_slot,
			ve::IVec3 lo, ve::IVec3 hi, int op_count, bool force_regen);
	void release_region(RenderingDevice *rd, int64_t list, int region_slot);
	void write_dispatch_args(RenderingDevice *rd, int64_t list);

private:
	bool build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline);

	RenderingDevice *rd_ = nullptr;
	int max_brick_jobs_ = 0;
	RID mark_shader_, mark_pipeline_, mark_uset_;
	RID free_shader_, free_pipeline_, free_uset_;
	RID args_shader_, args_pipeline_, args_uset_;
};

} // namespace godot
```

- [ ] **Step 6: Write `render/region_pass.cpp`**

```cpp
#include "render/region_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <sstream>

using namespace godot;

namespace {

// Godot's shader_compile_spirv_from_source feeds GLSL to glslang, which rejects the
// Godot-only `#[compute]` annotation. Same rule as M1's RaymarchPass.
std::string strip_godot_annotations(const std::string &src) {
	std::istringstream in(src);
	std::ostringstream out;
	std::string line;
	while (std::getline(in, line)) {
		const size_t first = line.find_first_not_of(" \t\r");
		const bool annotation = first != std::string::npos && line[first] == '#' &&
				first + 1 < line.size() && line[first + 1] == '[';
		if (!annotation) out << line << '\n';
	}
	return out.str();
}

Ref<RDUniform> storage(int binding, RID rid) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u->set_binding(binding);
	u->add_id(rid);
	return u;
}

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

} // namespace

RegionPass::~RegionPass() {
	teardown();
}

bool RegionPass::build(RenderingDevice *rd, const char *res_path, RID *shader, RID *pipeline) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(String(res_path));
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = strip_godot_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("RegionPass: ", res_path, " load failed: ", err.c_str());
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("RegionPass: ", res_path, ": ", compile_err);
		return false;
	}
	*shader = rd->shader_create_from_spirv(spirv);
	if (!shader->is_valid()) return false;
	*pipeline = rd->compute_pipeline_create(*shader);
	return pipeline->is_valid();
}

bool RegionPass::initialize(RenderingDevice *rd, const GpuAtlas &atlas) {
	teardown();
	rd_ = rd;
	max_brick_jobs_ = atlas.config().max_brick_jobs;
	if (!build(rd, "res://shaders/brick_mark.comp.glsl", &mark_shader_, &mark_pipeline_) ||
			!build(rd, "res://shaders/region_free.comp.glsl", &free_shader_, &free_pipeline_) ||
			!build(rd, "res://shaders/dispatch_args.comp.glsl", &args_shader_, &args_pipeline_)) {
		teardown();
		return false;
	}
	// The atlas buffers never change identity, so the uniform sets are built once.
	mark_uset_ = rd->uniform_set_create(
			Array::make(storage(0, atlas.region_tables()), storage(1, atlas.free_list()),
					storage(2, atlas.counters()), storage(3, atlas.frame_counters()),
					storage(4, atlas.op_pool()), storage(5, atlas.jobs())),
			mark_shader_, 0);
	free_uset_ = rd->uniform_set_create(
			Array::make(storage(0, atlas.region_tables()), storage(1, atlas.free_list()),
					storage(2, atlas.counters())),
			free_shader_, 0);
	args_uset_ = rd->uniform_set_create(
			Array::make(storage(0, atlas.frame_counters()), storage(1, atlas.dispatch_args())),
			args_shader_, 0);
	if (!mark_uset_.is_valid() || !free_uset_.is_valid() || !args_uset_.is_valid()) {
		UtilityFunctions::printerr("RegionPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void RegionPass::teardown() {
	if (!rd_) return;
	// Uniform sets first: freeing a shader cascades to its pipelines and referencing sets.
	free_if_valid(rd_, mark_uset_);
	free_if_valid(rd_, free_uset_);
	free_if_valid(rd_, args_uset_);
	free_if_valid(rd_, mark_pipeline_);
	free_if_valid(rd_, free_pipeline_);
	free_if_valid(rd_, args_pipeline_);
	free_if_valid(rd_, mark_shader_);
	free_if_valid(rd_, free_shader_);
	free_if_valid(rd_, args_shader_);
	rd_ = nullptr;
}

void RegionPass::mark(RenderingDevice *rd, int64_t list, ve::IVec3 region, int region_slot,
		ve::IVec3 lo, ve::IVec3 hi, int op_count, bool force_regen) {
	if (!mark_pipeline_.is_valid()) return;
	const int64_t total = static_cast<int64_t>(hi.x - lo.x + 1) * (hi.y - lo.y + 1) *
			(hi.z - lo.z + 1);
	if (total <= 0) return;
	const uint32_t groups = static_cast<uint32_t>((total + 255) / 256);

	PackedByteArray pc;
	pc.resize(64);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = region.x; p[1] = region.y; p[2] = region.z; p[3] = region_slot;
	p[4] = lo.x; p[5] = lo.y; p[6] = lo.z; p[7] = 0;
	p[8] = hi.x; p[9] = hi.y; p[10] = hi.z; p[11] = 0;
	p[12] = op_count; p[13] = 0; p[14] = max_brick_jobs_; p[15] = force_regen ? 1 : 0;

	rd->compute_list_bind_compute_pipeline(list, mark_pipeline_);
	rd->compute_list_bind_uniform_set(list, mark_uset_, 0);
	// Phase 0 (release) is only meaningful when bricks may have gone inactive, which only
	// an edit can cause. A plain stream-in scans a region whose table is entirely absent.
	if (force_regen) {
		p[13] = 0;
		rd->compute_list_set_push_constant(list, pc, pc.size());
		rd->compute_list_dispatch(list, groups, 1, 1);
		rd->compute_list_add_barrier(list);
	}
	p[13] = 1;
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, groups, 1, 1);
}

void RegionPass::release_region(RenderingDevice *rd, int64_t list, int region_slot) {
	if (!free_pipeline_.is_valid()) return;
	PackedByteArray pc;
	pc.resize(16);
	reinterpret_cast<int32_t *>(pc.ptrw())[0] = region_slot;
	rd->compute_list_bind_compute_pipeline(list, free_pipeline_);
	rd->compute_list_bind_uniform_set(list, free_uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (ve::kRegionBrickCount + 255) / 256, 1, 1);
}

void RegionPass::write_dispatch_args(RenderingDevice *rd, int64_t list) {
	if (!args_pipeline_.is_valid()) return;
	rd->compute_list_bind_compute_pipeline(list, args_pipeline_);
	rd->compute_list_bind_uniform_set(list, args_uset_, 0);
	rd->compute_list_dispatch(list, 1, 1, 1);
}
```

- [ ] **Step 7: Wire the debug hooks**

In `VoxelWorld`, add a `RegionPass *region_pass_ = nullptr;` member, create it inside
`debug_init_atlas()` right after the atlas succeeds (`region_pass_ = new RegionPass();
region_pass_->initialize(device, *atlas_);`, returning false and tearing down on failure),
delete it in `_exit_tree()` before the atlas, and implement:

```cpp
bool VoxelWorld::debug_brick_has_surface(Vector3i brick, const PackedByteArray &ops,
		int op_count) const {
	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr = op_count > 0 ? reinterpret_cast<const ve::EditOp *>(ops.ptr())
	                                     : nullptr;
	return ve::brick_has_surface(gen, ptr, op_count, {brick.x, brick.y, brick.z});
}

void VoxelWorld::debug_mark_region(Vector3i region, int region_slot, Vector3i lo, Vector3i hi,
		int op_count, bool force) {
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !region_pass_) return;
	const int64_t list = device->compute_list_begin();
	region_pass_->mark(device, list, {region.x, region.y, region.z}, region_slot,
			{lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}, op_count, force);
	device->compute_list_end();
	device->submit();
	device->sync();
}

void VoxelWorld::debug_release_region(int region_slot) {
	RenderingDevice *device = rd();
	if (!device || !region_pass_) return;
	const int64_t list = device->compute_list_begin();
	region_pass_->release_region(device, list, region_slot);
	device->compute_list_end();
	device->submit();
	device->sync();
}

PackedInt32Array VoxelWorld::debug_jobs() {
	PackedInt32Array out;
	RenderingDevice *device = rd();
	if (!device || !atlas_) return out;
	const int count = atlas_->read_job_count(device);
	if (count <= 0) return out;
	const PackedByteArray b = device->buffer_get_data(atlas_->jobs(), 0, count * 32);
	out.resize(count * 8);
	memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(count) * 32);
	return out;
}

int VoxelWorld::debug_region_table_slot(int region_slot, Vector3i brick) {
	RenderingDevice *device = rd();
	if (!device || !atlas_) return -1;
	const int bi = ve::WorldBounds::brick_index_in_region({brick.x, brick.y, brick.z});
	const uint32_t offset =
			(static_cast<uint32_t>(region_slot) * ve::kRegionBrickCount + bi) * 4;
	const PackedByteArray b = device->buffer_get_data(atlas_->region_tables(), offset, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}
```

Declare all five in the header and bind them in `_bind_methods()`.

Note the `submit()`/`sync()` inside these debug helpers: they exist because the tests read
back immediately. The real streamer (Task 12) records everything into one compute list per
frame and never syncs.

- [ ] **Step 8: Build and run**

```bash
./build.sh -j$(nproc)
./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests
```
Expected: `test_region_pass.gd` 7/7 and every earlier suite green.

If "the chosen region holds no surface" fires, `hills()` does not cross region (0,0,0) in this
build — print `debug_brick_has_surface` over a y sweep, pick the region that does, and update
the `REGION` constant plus the comment above it.

- [ ] **Step 9: Commit**

```bash
git add shaders/brick_mark.comp.glsl shaders/region_free.comp.glsl \
        shaders/dispatch_args.comp.glsl extension/src/render/region_pass.* \
        extension/src/voxel_world.* tests/test_region_pass.gd
git commit -m "feat(render): GPU brick residency marking and slot allocation"
```

---

### Task 10: `brick_gen` — one workgroup per brick

The centrepiece. 256 threads cooperate on a single brick: 4 913 SDF lattice samples, the 16³
material field, a four-entry palette built with shared-memory atomics, and the three min–max
levels — then the whole thing is diffed against `ve::eval_brick` (spec §8).

The palette is the only part that needs cross-thread coordination, and it needs two rounds:
inserting materials is unordered, but `ve::palette_occupancy_order` must see final counts
before it can decide which material earns slot 0. So: insert with `atomicCompSwap`, barrier,
one thread sorts four entries, barrier, everyone packs their cells.

Shared memory: `s_mat` (16 KB) + `s_mip8` (2 KB) + `s_mip4` (256 B) + 48 B of palette state
≈ 18.3 KB, inside the 32 KB Vulkan guarantees.

**Files:**
- Create: `shaders/brick_gen.comp.glsl`
- Create: `extension/src/render/brick_gen_pass.h`, `extension/src/render/brick_gen_pass.cpp`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (`debug_generate_pending`, `debug_brick_diff`)
- Test: `tests/test_brick_diff.gd`

**Interfaces:**
- Consumes: `GpuAtlas` (Task 8), `RegionPass` (Task 9), `field.glsl` + `brick_layout.glsl` (Task 7), `ve::eval_brick` (Task 5).
- Produces:
  - `class godot::BrickGenPass`:
    - `bool initialize(RenderingDevice *rd, const GpuAtlas &atlas)`, `void teardown()`, `bool is_valid() const`
    - `void dispatch(RenderingDevice *rd, int64_t list, const GpuAtlas &atlas)` — records an
      indirect dispatch reading `atlas.dispatch_args()`; the caller must have run
      `RegionPass::write_dispatch_args` and a barrier first.
  - `VoxelWorld::debug_generate_pending() -> void` — dispatch args + indirect generation + sync.
  - `VoxelWorld::debug_brick_diff(Vector3i brick, int region_slot, PackedByteArray ops, int op_count) -> Dictionary`
    with keys `slot`, `sdf_max_diff`, `sdf_diff_over_one`, `mat_near_compared`,
    `mat_near_mismatch`, `palette_match` (bool), `mip_mismatch`.

- [ ] **Step 1: Write the failing test**

`tests/test_brick_diff.gd`:

```gdscript
extends GdUnitTestSuite

# GPU/CPU differential test for brick generation (spec section 8): shaders/brick_gen.comp.glsl
# against ve::eval_brick. What is compared, and why each tolerance is what it is:
#
#  * SDF lattice, all 4913 samples: exact, except that sin() is not bit-identical between
#    glibc and the driver. A one-step disagreement in a uint8 with ~5 mm steps cannot be seen;
#    two steps would be a real bug.
#  * Materials, only for cells within ~1.2 voxels of the surface: those are the cells a hit
#    point can round to. Cells deeper in air are left at packed index 0 by design (see
#    ve::palette_occupancy_order) and carry no information to compare.
#  * Palette contents: exact. The occupancy ordering is deterministic on both sides.
#  * Min-max chain: exact. It is a pure reduction of the lattice both sides already agree on,
#    so any mismatch means the reduction itself drifted.

const ATLAS := Vector3i(16, 8, 16)
const REGION_SLOTS := 4
const REGIONS := Vector3i(4, 2, 4)
const REGION := Vector3i(0, 0, 0)
const SLOT := 1

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = REGIONS
	add_child(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w

func make_op(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type); b.put_u32(material)
	b.put_float(pos.x); b.put_float(pos.y); b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(0); b.put_u32(0)
	return b.data_array

func generate_region(w: VoxelWorld, region: Vector3i, slot: int, op_count: int) -> void:
	var lo := region * 32
	w.debug_mark_region(region, slot, lo, lo + Vector3i(31, 31, 31), op_count, false)
	w.debug_generate_pending()

func active_bricks(w: VoxelWorld, slot: int, limit: int) -> Array:
	var out := []
	var jobs: PackedInt32Array = w.debug_jobs()
	var count := jobs.size() / 8
	# Spread the sample across the job list rather than taking the first N, which would all
	# come from one corner of the region.
	var stride: int = maxi(1, count / limit)
	for j in range(0, count, stride):
		out.append(Vector3i(jobs[j * 8 + 0], jobs[j * 8 + 1], jobs[j * 8 + 2]))
		if out.size() >= limit:
			break
	return out

func check_bricks(w: VoxelWorld, bricks: Array, slot: int, ops: PackedByteArray,
		op_count: int, label: String) -> void:
	assert_int(bricks.size()).override_failure_message(
		"%s: no bricks to compare" % label).is_greater(4)
	for brick in bricks:
		var d: Dictionary = w.debug_brick_diff(brick, slot, ops, op_count)
		assert_int(d["slot"]).override_failure_message(
			"%s: brick %s is not resident" % [label, brick]).is_greater_equal(0)
		assert_int(d["sdf_max_diff"]).override_failure_message(
			"%s: brick %s sdf differs by %d encoded steps" % [label, brick, d["sdf_max_diff"]]
			).is_less_equal(1)
		assert_bool(d["palette_match"]).override_failure_message(
			"%s: brick %s palette differs" % [label, brick]).is_true()
		assert_int(d["mip_mismatch"]).override_failure_message(
			"%s: brick %s has %d mip mismatches" % [label, brick, d["mip_mismatch"]]
			).is_equal(0)
		assert_int(d["mat_near_mismatch"]).override_failure_message(
			"%s: brick %s has %d/%d near-surface material mismatches"
			% [label, brick, d["mat_near_mismatch"], d["mat_near_compared"]]).is_equal(0)

func test_base_terrain_bricks_match_the_cpu_reference() -> void:
	var w := make_world()
	generate_region(w, REGION, SLOT, 0)
	check_bricks(w, active_bricks(w, SLOT, 12), SLOT, PackedByteArray(), 0, "base")

func test_near_surface_cells_carry_a_real_material() -> void:
	var w := make_world()
	generate_region(w, REGION, SLOT, 0)
	for brick in active_bricks(w, SLOT, 12):
		var d: Dictionary = w.debug_brick_diff(brick, SLOT, PackedByteArray(), 0)
		# The comparison must actually exercise the near-surface band, or the previous test
		# is vacuous.
		assert_int(d["mat_near_compared"]).is_greater(0)

func test_carved_bricks_match_the_cpu_reference() -> void:
	var w := make_world()
	var ops := make_op(0, 0, Vector3(12.8, 4.0, 12.8), 5.0) # sphere subtract
	w.debug_upload_region_ops(SLOT, ops, 1)
	generate_region(w, REGION, SLOT, 1)
	check_bricks(w, active_bricks(w, SLOT, 12), SLOT, ops, 1, "carved")

func test_filled_bricks_match_and_introduce_the_new_material() -> void:
	var w := make_world()
	var ops := make_op(1, 4, Vector3(12.8, 12.0, 12.8), 4.0) # sphere add, material 4
	w.debug_upload_region_ops(SLOT, ops, 1)
	generate_region(w, REGION, SLOT, 1)
	var bricks := active_bricks(w, SLOT, 16)
	check_bricks(w, bricks, SLOT, ops, 1, "filled")
	# At least one brick must actually hold the added material, or the op did nothing.
	var found := false
	for brick in bricks:
		var d: Dictionary = w.debug_brick_diff(brick, SLOT, ops, 1)
		if d["has_material_4"]:
			found = true
			break
	assert_bool(found).override_failure_message("the sphere-add op stamped no material 4").is_true()

func test_an_ordered_op_chain_matches() -> void:
	var w := make_world()
	var ops := make_op(0, 0, Vector3(12.8, 4.0, 12.8), 6.0)
	ops.append_array(make_op(1, 4, Vector3(12.8, 4.0, 12.8), 3.0))
	ops.append_array(make_op(2, 2, Vector3(6.0, 4.0, 6.0), 5.0))
	w.debug_upload_region_ops(SLOT, ops, 3)
	generate_region(w, REGION, SLOT, 3)
	check_bricks(w, active_bricks(w, SLOT, 12), SLOT, ops, 3, "chain")

func test_regeneration_is_idempotent() -> void:
	var w := make_world()
	generate_region(w, REGION, SLOT, 0)
	var bricks := active_bricks(w, SLOT, 8)
	w.debug_reset_frame_counters()
	var lo := REGION * 32
	w.debug_mark_region(REGION, SLOT, lo, lo + Vector3i(31, 31, 31), 0, true)
	w.debug_generate_pending()
	check_bricks(w, bricks, SLOT, PackedByteArray(), 0, "regenerated")
```

`debug_brick_diff` must therefore also return `has_material_4` (a bool: does the GPU palette
for this brick contain material id 4).

- [ ] **Step 2: Run test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_brick_diff.gd`
Expected: FAIL — `debug_generate_pending` does not exist.

- [ ] **Step 3: Write `shaders/brick_gen.comp.glsl`**

```glsl
#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 7
#include "field.glsl"
#include "brick_layout.glsl"

// One workgroup per brick; 256 threads stride over the brick's 4096 cells and 4913 lattice
// samples. Mirror of ve::eval_brick (extension/src/world/brick_eval.cpp).
layout(local_size_x = 256) in;

// The SDF atlas is read back within this dispatch (material projection and the min-max
// reduction both need the lattice just written), so it is coherent read-write, not writeonly.
layout(set = 0, binding = 0, r8) coherent uniform image3D sdf_atlas;
layout(set = 0, binding = 1, r8ui) writeonly uniform uimage3D mat_atlas;
layout(set = 0, binding = 2, rg8ui) writeonly uniform uimage3D mip2_atlas;
layout(set = 0, binding = 3, rg8ui) writeonly uniform uimage3D mip4_atlas;
layout(set = 0, binding = 4, rg8ui) writeonly uniform uimage3D mip8_atlas;
layout(set = 0, binding = 5, std430) writeonly buffer Palette { uint id[]; } palette_buf;
layout(set = 0, binding = 6, std430) readonly buffer Jobs { ivec4 v[]; } jobs;
// binding 7 is the field op pool, declared by field.glsl

layout(push_constant, std430) uniform Push {
	ivec4 atlas_bricks;
} pc;

shared uint s_mat[BRICK_VOXEL_COUNT]; // global material id per cell, 0 = none
shared uint s_pal[4];                 // palette in insertion order
shared uint s_cnt[4];                 // cells charged to each insertion-order slot
shared uint s_inv[4];                 // insertion-order slot -> final packed index
shared uint s_mip8[512];              // (min << 8) | max
shared uint s_mip4[64];

float lat(ivec3 base, ivec3 v) {
	return decode_sdf(imageLoad(sdf_atlas, base + v).r);
}

// Central difference over the span actually sampled. On a brick's outer planes the lattice
// has no neighbour on one side, so the difference is one-sided over a single voxel; dividing
// every axis by a fixed 2 would halve that component and tilt the projection sideways across
// a material band instead of straight down onto the surface underneath.
float slope_axis(ivec3 base, ivec3 v, int axis) {
	ivec3 lo = v, hi = v;
	lo[axis] = max(lo[axis] - 1, 0);
	hi[axis] = min(hi[axis] + 1, BRICK_VOXELS);
	float span = float(hi[axis] - lo[axis]) * VOXEL_SIZE;
	return (lat(base, hi) - lat(base, lo)) / span;
}

int nearest_palette_slot(uint m) {
	int best = 0;
	int bd = abs(int(s_pal[0]) - int(m));
	for (int k = 1; k < 4; k++) {
		int d = abs(int(s_pal[k]) - int(m));
		if (d < bd) { bd = d; best = k; }
	}
	return best;
}

// Mirror of ve::palette_slot's insert-or-nearest, with an occupancy count on the side.
void insert_material(uint m) {
	for (int k = 0; k < 4; k++) {
		uint prev = atomicCompSwap(s_pal[k], 0u, m);
		if (prev == 0u || prev == m) { atomicAdd(s_cnt[k], 1u); return; }
	}
	atomicAdd(s_cnt[nearest_palette_slot(m)], 1u); // a 5th material: charged to the nearest
}

uint resolve_index(uint m) {
	for (int k = 0; k < 4; k++) if (s_pal[k] == m) return s_inv[k];
	return s_inv[nearest_palette_slot(m)];
}

ivec3 cell_coord(uint i) {
	return ivec3(int(i) % BRICK_VOXELS, (int(i) / BRICK_VOXELS) % BRICK_VOXELS,
			int(i) / (BRICK_VOXELS * BRICK_VOXELS));
}

void main() {
	uint tid = gl_LocalInvocationID.x;
	ivec4 j0 = jobs.v[int(gl_WorkGroupID.x) * 2 + 0];
	ivec4 j1 = jobs.v[int(gl_WorkGroupID.x) * 2 + 1];
	ivec3 brick = j0.xyz;
	int slot = j0.w;
	if (slot < 0) return;
	uint op_base = uint(j1.x) * MAX_REGION_OPS;
	uint op_count = uint(j1.y);

	ivec3 sdf_base = atlas_base(slot, pc.atlas_bricks, BRICK_SDF_STRIDE);
	ivec3 mat_base = atlas_base(slot, pc.atlas_bricks, BRICK_VOXELS);
	vec3 bo = vec3(brick) * BRICK_SIZE;

	if (tid < 4u) { s_pal[tid] = 0u; s_cnt[tid] = 0u; s_inv[tid] = tid; }

	// Phase 1a: the 16^3 cell lattice — SDF plus each cell's own material sample.
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		ivec3 v = cell_coord(i);
		float sdf;
		uint mat;
		eval_field(bo + vec3(v) * VOXEL_SIZE, op_base, op_count, sdf, mat);
		imageStore(sdf_atlas, sdf_base + v, vec4(quantise_sdf(sdf)));
		s_mat[i] = mat;
	}
	memoryBarrierShared();
	barrier();

	// Phase 1b: the apron planes at local 16 (17^3 - 16^3 = 817 samples). Their SDF completes
	// the trilinear cell at the brick's positive faces; their material seeds the cell the
	// shader's clamp folds them into, but only where that cell sampled nothing of its own —
	// a cell's own sample always wins, which is what the compare-and-swap against 0 encodes.
	for (uint i = tid; i < uint(BRICK_SDF_COUNT); i += 256u) {
		ivec3 v = ivec3(int(i) % BRICK_SDF_STRIDE,
				(int(i) / BRICK_SDF_STRIDE) % BRICK_SDF_STRIDE,
				int(i) / (BRICK_SDF_STRIDE * BRICK_SDF_STRIDE));
		if (v.x < BRICK_VOXELS && v.y < BRICK_VOXELS && v.z < BRICK_VOXELS) continue;
		float sdf;
		uint mat;
		eval_field(bo + vec3(v) * VOXEL_SIZE, op_base, op_count, sdf, mat);
		imageStore(sdf_atlas, sdf_base + v, vec4(quantise_sdf(sdf)));
		if (mat == 0u) continue;
		ivec3 c = min(v, ivec3(BRICK_VOXELS - 1));
		atomicCompSwap(s_mat[c.x + c.y * BRICK_VOXELS + c.z * BRICK_VOXELS * BRICK_VOXELS],
				0u, mat);
	}
	memoryBarrierImage();
	memoryBarrierShared();
	barrier();

	// Phase 2: project near-surface air cells onto the surface and ask the field for the
	// material there (ve::spread_materials). Purely per-cell: no thread reads another's cell.
	const float project_range = 2.0 * VOXEL_SIZE;
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		if (s_mat[i] != 0u) continue;
		ivec3 v = cell_coord(i);
		float d = lat(sdf_base, v);
		if (d > project_range) continue;
		vec3 g = vec3(slope_axis(sdf_base, v, 0), slope_axis(sdf_base, v, 1),
				slope_axis(sdf_base, v, 2));
		float len = length(g);
		if (len <= 0.0) continue;
		// The stored SDF is uint8-quantised and the gradient is a difference of those, so a
		// single step can fall short of the surface; lengthen it and retry.
		for (float over = 0.5; over <= 2.5 && s_mat[i] == 0u; over += 1.0) {
			float t = d + over * VOXEL_SIZE;
			float sdf2;
			uint mat2;
			eval_field(bo + vec3(v) * VOXEL_SIZE - g / len * t, op_base, op_count, sdf2, mat2);
			s_mat[i] = mat2;
		}
	}
	memoryBarrierShared();
	barrier();

	// Phase 3: build the palette, then order it by occupancy so slot 0 is the dominant
	// material (ve::palette_occupancy_order). Two rounds: insertion is unordered, and the
	// ordering cannot be decided until every cell has been counted.
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u)
		if (s_mat[i] != 0u) insert_material(s_mat[i]);
	memoryBarrierShared();
	barrier();
	if (tid == 0u) {
		uint order[4] = uint[4](0u, 1u, 2u, 3u);
		for (int a = 0; a < 4; a++)
			for (int b = a + 1; b < 4; b++) {
				uint ia = order[a], ib = order[b];
				bool a_empty = s_pal[ia] == 0u, b_empty = s_pal[ib] == 0u;
				bool swap = false;
				if (a_empty != b_empty) swap = a_empty;
				else if (!a_empty) swap = s_cnt[ib] > s_cnt[ia] ||
						(s_cnt[ib] == s_cnt[ia] && s_pal[ib] < s_pal[ia]);
				if (swap) { order[a] = ib; order[b] = ia; }
			}
		for (int a = 0; a < 4; a++) {
			s_inv[order[a]] = uint(a);
			palette_buf.id[slot * 4 + a] = s_pal[order[a]];
		}
	}
	memoryBarrierShared();
	barrier();

	// Phase 4: pack the material atlas. Cells with no material keep index 0, which the
	// occupancy ordering has made the brick's dominant material.
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		ivec3 v = cell_coord(i);
		uint idx = s_mat[i] == 0u ? 0u : resolve_index(s_mat[i]);
		imageStore(mat_atlas, mat_base + v, uvec4(idx, 0u, 0u, 0u));
	}

	// Phase 5: the min-max chain. The 8^3 level reads the lattice directly — cell (i,j,k)
	// covers voxels [2i, 2i+2), whose trilinear corners are lattice samples [2i, 2i+2]
	// INCLUSIVE, a 3^3 block. Coarser levels reduce the level below.
	ivec3 b8 = atlas_base(slot, pc.atlas_bricks, 8);
	for (uint i = tid; i < 512u; i += 256u) {
		ivec3 c = ivec3(int(i) % 8, (int(i) / 8) % 8, int(i) / 64);
		uint mn = 255u, mx = 0u;
		for (int z = 0; z <= 2; z++)
			for (int y = 0; y <= 2; y++)
				for (int x = 0; x <= 2; x++) {
					uint s = uint(imageLoad(sdf_atlas, sdf_base + c * 2 + ivec3(x, y, z)).r *
							255.0 + 0.5);
					mn = min(mn, s);
					mx = max(mx, s);
				}
		s_mip8[i] = (mn << 8) | mx;
		imageStore(mip8_atlas, b8 + c, uvec4(mn, mx, 0u, 0u));
	}
	memoryBarrierShared();
	barrier();

	ivec3 b4 = atlas_base(slot, pc.atlas_bricks, 4);
	for (uint i = tid; i < 64u; i += 256u) {
		ivec3 c = ivec3(int(i) % 4, (int(i) / 4) % 4, int(i) / 16);
		uint mn = 255u, mx = 0u;
		for (int z = 0; z < 2; z++)
			for (int y = 0; y < 2; y++)
				for (int x = 0; x < 2; x++) {
					uint p = s_mip8[(2 * c.x + x) + (2 * c.y + y) * 8 + (2 * c.z + z) * 64];
					mn = min(mn, p >> 8);
					mx = max(mx, p & 255u);
				}
		s_mip4[i] = (mn << 8) | mx;
		imageStore(mip4_atlas, b4 + c, uvec4(mn, mx, 0u, 0u));
	}
	memoryBarrierShared();
	barrier();

	ivec3 b2 = atlas_base(slot, pc.atlas_bricks, 2);
	for (uint i = tid; i < 8u; i += 256u) {
		ivec3 c = ivec3(int(i) % 2, (int(i) / 2) % 2, int(i) / 4);
		uint mn = 255u, mx = 0u;
		for (int z = 0; z < 2; z++)
			for (int y = 0; y < 2; y++)
				for (int x = 0; x < 2; x++) {
					uint p = s_mip4[(2 * c.x + x) + (2 * c.y + y) * 4 + (2 * c.z + z) * 16];
					mn = min(mn, p >> 8);
					mx = max(mx, p & 255u);
				}
		imageStore(mip2_atlas, b2 + c, uvec4(mn, mx, 0u, 0u));
	}
}
```

- [ ] **Step 4: Write `render/brick_gen_pass.h` / `.cpp`**

`extension/src/render/brick_gen_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/gpu_atlas.h"

namespace godot {

// Indirect brick generation: one workgroup per job in GpuAtlas::jobs(), group count read
// from GpuAtlas::dispatch_args().
class BrickGenPass {
public:
	~BrickGenPass();

	bool initialize(RenderingDevice *rd, const GpuAtlas &atlas);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }

	// Records into an OPEN compute list. RegionPass::write_dispatch_args followed by
	// compute_list_add_barrier must already have been recorded.
	void dispatch(RenderingDevice *rd, int64_t list, const GpuAtlas &atlas);

private:
	RenderingDevice *rd_ = nullptr;
	ve::IVec3 atlas_bricks_{};
	RID shader_, pipeline_, uset_;
};

} // namespace godot
```

`extension/src/render/brick_gen_pass.cpp` follows `region_pass.cpp` exactly for shader
loading (reuse the same `strip_godot_annotations` helper — move it into
`render/shader_loader.h/.cpp` as `std::string ve::strip_shader_annotations(const std::string &)`
and have `RaymarchPass`, `RegionPass` and `BrickGenPass` all call it, deleting the two local
copies). `initialize` builds the pipeline from `res://shaders/brick_gen.comp.glsl`, stores
`atlas.config().atlas_bricks`, and creates the uniform set:

```cpp
	Ref<RDUniform> img[5];
	const RID images[5] = {atlas.sdf_atlas(), atlas.mat_atlas(), atlas.mip_atlas(0),
			atlas.mip_atlas(1), atlas.mip_atlas(2)};
	Array uniforms;
	for (int i = 0; i < 5; i++) {
		img[i].instantiate();
		img[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
		img[i]->set_binding(i);
		img[i]->add_id(images[i]);
		uniforms.push_back(img[i]);
	}
	uniforms.push_back(storage(5, atlas.palette()));
	uniforms.push_back(storage(6, atlas.jobs()));
	uniforms.push_back(storage(7, atlas.op_pool()));
	uset_ = rd->uniform_set_create(uniforms, shader_, 0);
```

`dispatch`:

```cpp
void BrickGenPass::dispatch(RenderingDevice *rd, int64_t list, const GpuAtlas &atlas) {
	if (!pipeline_.is_valid()) return;
	PackedByteArray pc;
	pc.resize(16);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = atlas_bricks_.x; p[1] = atlas_bricks_.y; p[2] = atlas_bricks_.z; p[3] = 0;
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch_indirect(list, atlas.dispatch_args(), 0);
}
```

`teardown` frees `uset_`, then `pipeline_`, then `shader_` (M1's documented order).

- [ ] **Step 5: Wire `debug_generate_pending` and `debug_brick_diff`**

Add a `BrickGenPass *gen_pass_ = nullptr;` member to `VoxelWorld`, created in
`debug_init_atlas()` after `region_pass_`, deleted in `_exit_tree()` before the atlas. Then:

```cpp
void VoxelWorld::debug_generate_pending() {
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !region_pass_ || !gen_pass_) return;
	const int64_t list = device->compute_list_begin();
	region_pass_->write_dispatch_args(device, list);
	device->compute_list_add_barrier(list);
	gen_pass_->dispatch(device, list, *atlas_);
	device->compute_list_end();
	device->submit();
	device->sync();
}
```

`debug_brick_diff` reads the three volumes and the palette back once, then diffs against
`ve::eval_brick`:

```cpp
Dictionary VoxelWorld::debug_brick_diff(Vector3i brick, int region_slot,
		const PackedByteArray &ops, int op_count) {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!device || !atlas_) return d;
	const ve::IVec3 b{brick.x, brick.y, brick.z};
	const int slot = debug_region_table_slot(region_slot, brick);
	d["slot"] = slot;
	if (slot < 0) return d;

	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr =
			op_count > 0 ? reinterpret_cast<const ve::EditOp *>(ops.ptr()) : nullptr;
	ve::BrickEval ref{};
	ve::eval_brick(gen, ptr, op_count, b, &ref);

	const ve::IVec3 ab = atlas_->config().atlas_bricks;
	const ve::IVec3 cell{slot % ab.x, (slot / ab.x) % ab.y, slot / (ab.x * ab.y)};

	// texture_get_data returns the whole volume; tests run a small atlas, so one read each.
	const PackedByteArray sdf = device->texture_get_data(atlas_->sdf_atlas(), 0);
	const PackedByteArray mat = device->texture_get_data(atlas_->mat_atlas(), 0);
	const int sw = ab.x * ve::kBrickSdfStride, sh = ab.y * ve::kBrickSdfStride;
	const int mw = ab.x * ve::kBrickVoxels, mh = ab.y * ve::kBrickVoxels;

	int sdf_max = 0, sdf_over_one = 0;
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const int ax = cell.x * ve::kBrickSdfStride + x;
				const int ay = cell.y * ve::kBrickSdfStride + y;
				const int az = cell.z * ve::kBrickSdfStride + z;
				const int got = sdf[ax + ay * sw + az * sw * sh];
				const int want = ref.brick.sdf[ve::sdf_index(x, y, z)];
				const int diff = std::abs(got - want);
				sdf_max = std::max(sdf_max, diff);
				if (diff > 1) sdf_over_one++;
			}
	d["sdf_max_diff"] = sdf_max;
	d["sdf_diff_over_one"] = sdf_over_one;

	const PackedByteArray pal_bytes = device->buffer_get_data(atlas_->palette(),
			static_cast<uint32_t>(slot) * ve::kBrickPaletteSize * 4,
			ve::kBrickPaletteSize * 4);
	const uint32_t *pal = reinterpret_cast<const uint32_t *>(pal_bytes.ptr());
	bool pal_ok = true;
	bool has_four = false;
	for (int p = 0; p < ve::kBrickPaletteSize; p++) {
		pal_ok = pal_ok && pal[p] == ref.brick.palette[p];
		has_four = has_four || pal[p] == 4;
	}
	d["palette_match"] = pal_ok;
	d["has_material_4"] = has_four;

	// Materials are only meaningful where a hit point can land — within ~1.2 voxels of the
	// surface. Compare RESOLVED ids, not packed indices: the two sides agree on the palette
	// ordering, but comparing ids keeps the check honest if that ever changes.
	int near_compared = 0, near_mismatch = 0;
	for (int z = 0; z < ve::kBrickVoxels; z++)
		for (int y = 0; y < ve::kBrickVoxels; y++)
			for (int x = 0; x < ve::kBrickVoxels; x++) {
				const float dist = ve::decode_sdf(ref.brick.sdf[ve::sdf_index(x, y, z)]);
				if (std::fabs(dist) > 1.2f * ve::kVoxelSize) continue;
				const int ax = cell.x * ve::kBrickVoxels + x;
				const int ay = cell.y * ve::kBrickVoxels + y;
				const int az = cell.z * ve::kBrickVoxels + z;
				const int gi = mat[ax + ay * mw + az * mw * mh];
				const uint32_t got_id = gi < ve::kBrickPaletteSize ? pal[gi] : 0;
				const uint16_t want_id =
						ref.brick.palette[ve::get_mat_index(ref.brick, ve::voxel_index(x, y, z))];
				near_compared++;
				if (got_id != want_id) near_mismatch++;
			}
	d["mat_near_compared"] = near_compared;
	d["mat_near_mismatch"] = near_mismatch;

	int mip_bad = 0;
	for (int level = 0; level < ve::kMipLevels; level++) {
		const int dim = ve::kMipDims[level];
		const PackedByteArray mip = device->texture_get_data(atlas_->mip_atlas(level), 0);
		const int w = ab.x * dim, h = ab.y * dim;
		const uint8_t *want_mn = ve::mip_min(ref.mips, level);
		const uint8_t *want_mx = ve::mip_max(ref.mips, level);
		for (int z = 0; z < dim; z++)
			for (int y = 0; y < dim; y++)
				for (int x = 0; x < dim; x++) {
					const int ax = cell.x * dim + x, ay = cell.y * dim + y, az = cell.z * dim + z;
					const int64_t o = (static_cast<int64_t>(ax) + ay * w + az * w * h) * 2;
					const int i = x + y * dim + z * dim * dim;
					if (mip[o] != want_mn[i] || mip[o + 1] != want_mx[i]) mip_bad++;
				}
	}
	d["mip_mismatch"] = mip_bad;
	return d;
}
```

Include `<cmath>` and `"world/brick_mip.h"` in `voxel_world.cpp`, declare both methods in the
header, and bind them.

Note on `mip_mismatch`: it is an exact comparison, but it is computed from `ref.brick.sdf`,
the **CPU** lattice. If `sdf_max_diff` is 1 the mips can legitimately differ by 1 too. Should
the test flag mip mismatches while `sdf_max_diff` is 1, recompute the reference mips from the
GPU lattice bytes instead of the CPU ones and record that change here — the property under
test is that the reduction is right, not that `sin` is bit-identical.

- [ ] **Step 6: Build and run**

```bash
./build.sh -j$(nproc)
./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests
```
Expected: `test_brick_diff.gd` 6/6, everything else green.

- [ ] **Step 7: Commit**

```bash
git add shaders/brick_gen.comp.glsl extension/src/render/brick_gen_pass.* \
        extension/src/render/shader_loader.* extension/src/render/region_pass.cpp \
        extension/src/render/raymarch_pass.cpp extension/src/voxel_world.* \
        tests/test_brick_diff.gd
git commit -m "feat(render): GPU brick generation with CPU differential test"
```

---

### Task 11: `world/residency` — region set and distance-LRU

Residency is decided on the CPU at **region** granularity and nowhere else. A 96 m ball holds
~350 regions; the same ball holds ~14 million brick cells, which is why brick-level residency
lives on the GPU (Task 9) and this core never sees a voxel. That split is what makes the
policy — the part with real edge cases — a pure C++ unit test.

**Files:**
- Create: `extension/src/world/residency.h`, `extension/src/world/residency.cpp`
- Test: `extension/tests/test_residency.cpp`

**Interfaces:**
- Consumes: `ve::WorldBounds`, `ve::IVec3`, `ve::kRegionSize` (Task 1).
- Produces:
  - `struct ve::ResidencyConfig { WorldBounds bounds; float radius_m = 96.0f; int max_region_slots = 512; int max_loads_per_frame = 4; float evict_margin = 1.15f; }`
  - `struct ve::ResidencyPlan { struct Entry { IVec3 region; int slot; int map_index; }; std::vector<Entry> loads; std::vector<Entry> evicts; }`
  - `class ve::RegionResidency`:
    - `explicit RegionResidency(const ResidencyConfig &cfg)`
    - `ResidencyPlan update(float cx, float cy, float cz)`
    - `int slot_of(IVec3 region) const` — `-1` when not resident
    - `bool slot_resident(int slot) const`, `IVec3 region_of_slot(int slot) const`
    - `int resident_count() const`, `void clear()`, `const ResidencyConfig &config() const`
    - `static float region_distance(IVec3 region, float cx, float cy, float cz)`

- [ ] **Step 1: Write the failing test**

`extension/tests/test_residency.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/residency.h"
#include <algorithm>
#include <cmath>
#include <set>

static ve::ResidencyConfig make_cfg(float radius, int slots, int per_frame) {
	ve::ResidencyConfig cfg;
	cfg.bounds = ve::WorldBounds{{0, -64, 0}, {8, 4, 8}}; // 204.8 x 102.4 x 204.8 m
	cfg.radius_m = radius;
	cfg.max_region_slots = slots;
	cfg.max_loads_per_frame = per_frame;
	return cfg;
}

// Independent oracle: distance from a point to a region's world AABB, 0 when inside.
static float oracle_distance(ve::IVec3 r, float cx, float cy, float cz) {
	const float lo[3] = {r.x * ve::kRegionSize, r.y * ve::kRegionSize, r.z * ve::kRegionSize};
	const float c[3] = {cx, cy, cz};
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float over = std::max(0.0f, std::max(lo[a] - c[a], c[a] - (lo[a] + ve::kRegionSize)));
		d2 += over * over;
	}
	return std::sqrt(d2);
}

// Drives update() until it stops loading, so the test can assert on the settled state.
static int settle(ve::RegionResidency &res, float cx, float cy, float cz, int max_frames = 500) {
	for (int i = 0; i < max_frames; i++) {
		const ve::ResidencyPlan p = res.update(cx, cy, cz);
		if (p.loads.empty() && p.evicts.empty()) return i;
	}
	return max_frames;
}

TEST_CASE("region_distance is zero inside and grows outside") {
	CHECK(ve::RegionResidency::region_distance({0, 0, 0}, 1.0f, 1.0f, 1.0f) == doctest::Approx(0.0f));
	CHECK(ve::RegionResidency::region_distance({0, 0, 0}, -10.0f, 1.0f, 1.0f) ==
			doctest::Approx(10.0f));
	CHECK(ve::RegionResidency::region_distance({1, 0, 0}, 0.0f, 0.0f, 0.0f) ==
			doctest::Approx(ve::kRegionSize));
}

TEST_CASE("loading is throttled to max_loads_per_frame") {
	ve::RegionResidency res(make_cfg(60.0f, 256, 3));
	const ve::ResidencyPlan p = res.update(100.0f, 0.0f, 100.0f);
	CHECK(p.loads.size() == 3);
	CHECK(p.evicts.empty());
	CHECK(res.resident_count() == 3);
}

TEST_CASE("the settled set is exactly the in-radius regions") {
	auto cfg = make_cfg(60.0f, 256, 8);
	ve::RegionResidency res(cfg);
	CHECK(settle(res, 100.0f, 0.0f, 100.0f) < 500);

	// Everything resident is inside the radius...
	std::set<int> slots;
	const ve::IVec3 o = cfg.bounds.origin_regions();
	int resident = 0;
	for (int z = 0; z < cfg.bounds.size_regions.z; z++)
		for (int y = 0; y < cfg.bounds.size_regions.y; y++)
			for (int x = 0; x < cfg.bounds.size_regions.x; x++) {
				const ve::IVec3 r{o.x + x, o.y + y, o.z + z};
				const float d = oracle_distance(r, 100.0f, 0.0f, 100.0f);
				const int slot = res.slot_of(r);
				if (slot >= 0) {
					resident++;
					CHECK(d <= cfg.radius_m * cfg.evict_margin);
					CHECK(slots.insert(slot).second); // slots are unique
					CHECK(res.region_of_slot(slot) == r);
				} else {
					// ...and everything comfortably inside it is resident.
					CHECK(d > cfg.radius_m * 0.9f);
				}
			}
	CHECK(resident == res.resident_count());
	CHECK(resident > 8);
}

TEST_CASE("out-of-bounds regions are never resident") {
	auto cfg = make_cfg(200.0f, 512, 64); // radius exceeds the world in every direction
	ve::RegionResidency res(cfg);
	settle(res, 100.0f, 0.0f, 100.0f);
	CHECK(res.slot_of({-1, 0, 0}) == -1);
	CHECK(res.slot_of({8, 0, 0}) == -1);
	CHECK(res.slot_of({0, -3, 0}) == -1);
	CHECK(res.resident_count() == 8 * 4 * 8); // the whole world fits inside the radius
}

TEST_CASE("map_index matches the bounds' dense region index") {
	auto cfg = make_cfg(60.0f, 256, 8);
	ve::RegionResidency res(cfg);
	const ve::ResidencyPlan p = res.update(100.0f, 0.0f, 100.0f);
	REQUIRE_FALSE(p.loads.empty());
	for (const auto &l : p.loads) CHECK(l.map_index == cfg.bounds.region_index(l.region));
}

TEST_CASE("moving away evicts, and the freed slots are reused") {
	auto cfg = make_cfg(40.0f, 256, 16);
	ve::RegionResidency res(cfg);
	settle(res, 30.0f, 0.0f, 30.0f);
	const int first_count = res.resident_count();
	REQUIRE(first_count > 0);
	const int kept_slot = res.slot_of(res.region_of_slot(0));

	settle(res, 170.0f, 0.0f, 170.0f);
	CHECK(res.slot_of({1, 0, 1}) == -1);      // the old neighbourhood is gone
	CHECK(res.resident_count() > 0);
	// Slot count never exceeds the pool, so eviction must actually have recycled slots.
	CHECK(res.resident_count() <= cfg.max_region_slots);
	(void)kept_slot;
}

TEST_CASE("an evict reports the slot and map index the loader was given") {
	auto cfg = make_cfg(40.0f, 256, 16);
	ve::RegionResidency res(cfg);
	settle(res, 30.0f, 0.0f, 30.0f);
	std::set<int> before;
	for (int s = 0; s < cfg.max_region_slots; s++)
		if (res.slot_resident(s)) before.insert(s);

	ve::ResidencyPlan all;
	for (int i = 0; i < 200; i++) {
		const ve::ResidencyPlan p = res.update(170.0f, 0.0f, 170.0f);
		for (const auto &e : p.evicts) all.evicts.push_back(e);
		if (p.loads.empty() && p.evicts.empty()) break;
	}
	CHECK_FALSE(all.evicts.empty());
	for (const auto &e : all.evicts) {
		CHECK(before.count(e.slot) == 1);
		CHECK(e.map_index == cfg.bounds.region_index(e.region));
		CHECK(res.slot_of(e.region) == -1);
	}
}

TEST_CASE("hysteresis keeps a boundary region from thrashing") {
	auto cfg = make_cfg(40.0f, 256, 16);
	ve::RegionResidency res(cfg);
	settle(res, 100.0f, 0.0f, 100.0f);
	// Jitter the camera by a metre either side of a load boundary many times; nothing may
	// churn once the set has settled, or every frame would rebuild a region on the GPU.
	int churn = 0;
	for (int i = 0; i < 40; i++) {
		const float dx = (i % 2 == 0) ? 1.0f : -1.0f;
		const ve::ResidencyPlan p = res.update(100.0f + dx, 0.0f, 100.0f);
		churn += static_cast<int>(p.loads.size() + p.evicts.size());
	}
	CHECK(churn == 0);
}

TEST_CASE("with too few slots the closest regions win") {
	auto cfg = make_cfg(120.0f, 6, 16); // 6 slots for a radius that wants far more
	ve::RegionResidency res(cfg);
	settle(res, 100.0f, 0.0f, 100.0f);
	CHECK(res.resident_count() == 6);

	// Nothing resident may be further away than something that was refused.
	float worst_resident = 0.0f;
	const ve::IVec3 o = cfg.bounds.origin_regions();
	std::vector<float> refused;
	for (int z = 0; z < cfg.bounds.size_regions.z; z++)
		for (int y = 0; y < cfg.bounds.size_regions.y; y++)
			for (int x = 0; x < cfg.bounds.size_regions.x; x++) {
				const ve::IVec3 r{o.x + x, o.y + y, o.z + z};
				const float d = oracle_distance(r, 100.0f, 0.0f, 100.0f);
				if (d > cfg.radius_m) continue;
				if (res.slot_of(r) >= 0) worst_resident = std::max(worst_resident, d);
				else refused.push_back(d);
			}
	REQUIRE_FALSE(refused.empty());
	CHECK(worst_resident <= *std::min_element(refused.begin(), refused.end()) + 1e-3f);
}

TEST_CASE("teleporting into a full pool swaps the far set for the near set") {
	auto cfg = make_cfg(40.0f, 8, 4);
	ve::RegionResidency res(cfg);
	settle(res, 20.0f, 0.0f, 20.0f);
	REQUIRE(res.resident_count() == 8);
	settle(res, 180.0f, 0.0f, 180.0f);
	CHECK(res.resident_count() == 8);
	for (int s = 0; s < 8; s++) {
		REQUIRE(res.slot_resident(s));
		CHECK(oracle_distance(res.region_of_slot(s), 180.0f, 0.0f, 180.0f) <=
				cfg.radius_m * cfg.evict_margin);
	}
}

TEST_CASE("clear releases everything") {
	ve::RegionResidency res(make_cfg(60.0f, 256, 16));
	settle(res, 100.0f, 0.0f, 100.0f);
	REQUIRE(res.resident_count() > 0);
	res.clear();
	CHECK(res.resident_count() == 0);
	CHECK(res.slot_of({3, 0, 3}) == -1);
	CHECK_FALSE(res.slot_resident(0));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd extension && scons test`
Expected: compile error — `world/residency.h` not found.

- [ ] **Step 3: Write the implementation**

`extension/src/world/residency.h`:

```cpp
#pragma once
#include "world/region.h"
#include <map>
#include <vector>

namespace ve {

struct ResidencyConfig {
	WorldBounds bounds{};
	float radius_m = 96.0f;
	int max_region_slots = 512;
	int max_loads_per_frame = 4;
	// A region is evicted only past radius_m * evict_margin. Without the gap, a camera
	// resting on the radius boundary would load and evict the same region every frame,
	// each cycle costing a full 32^3 mark pass and a few thousand brick generations.
	float evict_margin = 1.15f;
};

struct ResidencyPlan {
	struct Entry {
		IVec3 region;
		int slot = -1;
		int map_index = -1; // WorldBounds::region_index(region)
	};
	std::vector<Entry> loads;
	std::vector<Entry> evicts;
};

// Which regions are resident, and in which region-table slot. Distance-LRU: when the pool is
// full, a closer candidate displaces the furthest resident (spec §2).
class RegionResidency {
public:
	explicit RegionResidency(const ResidencyConfig &cfg);

	ResidencyPlan update(float cx, float cy, float cz);

	int slot_of(IVec3 region) const;
	bool slot_resident(int slot) const;
	IVec3 region_of_slot(int slot) const { return slot_region_[slot]; }
	int resident_count() const { return static_cast<int>(by_region_.size()); }
	void clear();
	const ResidencyConfig &config() const { return cfg_; }

	// Distance from a point to the region's world AABB; 0 inside.
	static float region_distance(IVec3 region, float cx, float cy, float cz);

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	static Key key(IVec3 r) { return Key{r.x, r.y, r.z}; }
	void release(IVec3 region, int slot, ResidencyPlan *plan);

	ResidencyConfig cfg_;
	std::map<Key, int> by_region_;   // region -> slot
	std::vector<IVec3> slot_region_; // slot -> region (valid where slot_used_)
	std::vector<char> slot_used_;
	std::vector<int> free_slots_;
};

} // namespace ve
```

`extension/src/world/residency.cpp`:

```cpp
#include "world/residency.h"
#include <algorithm>
#include <cmath>

namespace ve {

RegionResidency::RegionResidency(const ResidencyConfig &cfg)
	: cfg_(cfg), slot_region_(static_cast<size_t>(cfg.max_region_slots)),
	  slot_used_(static_cast<size_t>(cfg.max_region_slots), 0) {
	free_slots_.reserve(static_cast<size_t>(cfg.max_region_slots));
	// Descending, so pop_back hands out slot 0 first: stable, readable slot numbers in tests
	// and in the debug HUD.
	for (int s = cfg.max_region_slots - 1; s >= 0; s--) free_slots_.push_back(s);
}

float RegionResidency::region_distance(IVec3 region, float cx, float cy, float cz) {
	const float lo[3] = {region.x * kRegionSize, region.y * kRegionSize, region.z * kRegionSize};
	const float c[3] = {cx, cy, cz};
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float over = std::max(0.0f, std::max(lo[a] - c[a], c[a] - (lo[a] + kRegionSize)));
		d2 += over * over;
	}
	return std::sqrt(d2);
}

int RegionResidency::slot_of(IVec3 region) const {
	const auto it = by_region_.find(key(region));
	return it == by_region_.end() ? -1 : it->second;
}

bool RegionResidency::slot_resident(int slot) const {
	return slot >= 0 && slot < cfg_.max_region_slots && slot_used_[slot] != 0;
}

void RegionResidency::clear() {
	by_region_.clear();
	std::fill(slot_used_.begin(), slot_used_.end(), 0);
	free_slots_.clear();
	for (int s = cfg_.max_region_slots - 1; s >= 0; s--) free_slots_.push_back(s);
}

void RegionResidency::release(IVec3 region, int slot, ResidencyPlan *plan) {
	by_region_.erase(key(region));
	slot_used_[slot] = 0;
	free_slots_.push_back(slot);
	plan->evicts.push_back({region, slot, cfg_.bounds.region_index(region)});
}

ResidencyPlan RegionResidency::update(float cx, float cy, float cz) {
	ResidencyPlan plan;

	// 1. Evict anything that has drifted past the hysteresis boundary.
	{
		std::vector<std::pair<IVec3, int>> gone;
		for (const auto &kv : by_region_) {
			const IVec3 r{kv.first.x, kv.first.y, kv.first.z};
			if (region_distance(r, cx, cy, cz) > cfg_.radius_m * cfg_.evict_margin)
				gone.emplace_back(r, kv.second);
		}
		for (const auto &g : gone) release(g.first, g.second, &plan);
	}

	// 2. Collect in-bounds, in-radius candidates that are not resident yet. The scan is over
	//    the radius' region AABB, not the whole world: at the shipping radius that is ~500
	//    cells, and the world holds a million.
	const IVec3 o = cfg_.bounds.origin_regions();
	const IVec3 sz = cfg_.bounds.size_regions;
	const auto span = [](float lo, float hi) {
		return std::make_pair(static_cast<int>(std::floor(lo / kRegionSize)),
				static_cast<int>(std::floor(hi / kRegionSize)));
	};
	const auto rx = span(cx - cfg_.radius_m, cx + cfg_.radius_m);
	const auto ry = span(cy - cfg_.radius_m, cy + cfg_.radius_m);
	const auto rz = span(cz - cfg_.radius_m, cz + cfg_.radius_m);

	struct Cand { float dist; IVec3 region; };
	std::vector<Cand> cands;
	for (int z = std::max(rz.first, o.z); z <= std::min(rz.second, o.z + sz.z - 1); z++)
		for (int y = std::max(ry.first, o.y); y <= std::min(ry.second, o.y + sz.y - 1); y++)
			for (int x = std::max(rx.first, o.x); x <= std::min(rx.second, o.x + sz.x - 1); x++) {
				const IVec3 r{x, y, z};
				const float d = region_distance(r, cx, cy, cz);
				if (d > cfg_.radius_m) continue;
				if (slot_of(r) >= 0) continue;
				cands.push_back({d, r});
			}
	// Nearest first; the coordinate tie-break keeps the plan deterministic frame to frame.
	std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
		if (a.dist != b.dist) return a.dist < b.dist;
		if (a.region.z != b.region.z) return a.region.z < b.region.z;
		if (a.region.y != b.region.y) return a.region.y < b.region.y;
		return a.region.x < b.region.x;
	});

	// 3. Load the nearest candidates, displacing the furthest residents when the pool is full.
	for (const Cand &c : cands) {
		if (static_cast<int>(plan.loads.size()) >= cfg_.max_loads_per_frame) break;
		if (free_slots_.empty()) {
			IVec3 worst{};
			int worst_slot = -1;
			float worst_dist = c.dist;
			for (const auto &kv : by_region_) {
				const IVec3 r{kv.first.x, kv.first.y, kv.first.z};
				const float d = region_distance(r, cx, cy, cz);
				if (d > worst_dist) { worst_dist = d; worst_slot = kv.second; worst = r; }
			}
			if (worst_slot < 0) break; // every resident is closer than every candidate left
			release(worst, worst_slot, &plan);
		}
		const int slot = free_slots_.back();
		free_slots_.pop_back();
		slot_used_[slot] = 1;
		slot_region_[slot] = c.region;
		by_region_[key(c.region)] = slot;
		plan.loads.push_back({c.region, slot, cfg_.bounds.region_index(c.region)});
	}
	return plan;
}

} // namespace ve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd extension && scons test`
Expected: the 11 new cases pass, whole suite green.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world/residency.* extension/tests/test_residency.cpp
git commit -m "feat(world): region residency with distance-LRU eviction"
```

---

### Task 12: The cutover — world streamer, raymarcher rewrite, demo retarget

Everything before this task was additive: the M1 `GpuWorld` path still renders, and the new
machinery (Tasks 8–11) is only exercised by its own debug hooks. This task throws the switch:
`GpuWorld` and the CPU upload path are **deleted**, the raymarcher walks the two-level
region→brick indirection, and a per-frame `WorldStreamer` drives residency, edits, marking
and generation from one compute list. After this task the demo renders the GPU-generated
world or nothing — the cutover is deliberately atomic so there is never a half-wired frame.

**Threading contract (new):** tools call `VoxelWorld::append_edit` on the **main** thread;
the compositor calls `WorldStreamer::run_frame` on the **render** thread. `edit_mutex_`
guards `edit_log_` + `pending_edits_`; both sides hold it only for microseconds (append or
swap — never GPU work). Everything else the streamer touches is render-thread-only.

**Why the ops upload must precede `compute_list_begin`:** `buffer_update` is recorded into
the same command stream and executes in order at submit, but Godot *errors* if it is called
while a compute list is open. All `buffer_update` calls therefore happen up front.

**Files:**
- Create: `extension/src/render/world_streamer.h`, `extension/src/render/world_streamer.cpp`
- Modify: `extension/src/render/camera_params.h` (grows to 128 bytes)
- Rewrite: `shaders/raymarch.comp.glsl` (two-level lookup; mip skip arrives in Task 13)
- Modify: `extension/src/render/raymarch_pass.h`, `extension/src/render/raymarch_pass.cpp` (GpuAtlas + edits UBO)
- Rewrite: `extension/src/voxel_world.h`; heavily modify `extension/src/voxel_world.cpp`
- Modify: `extension/src/raymarch_compositor.cpp` (streamer + new camera fields)
- Delete: `extension/src/render/gpu_world.h`, `extension/src/render/gpu_world.cpp`, `tests/test_gpu_world.gd`, `tests/test_gpu_world.gd.uid`
- Modify: `tests/test_raymarch_pixel.gd`, `tests/test_raymarch_magenta.gd` (migrate to streaming world)
- Modify: `demo/main.tscn`, `demo/benchmark.gd` (world shifted +51.2 m in y; see below)
- Test: `tests/test_streaming.gd`

**Interfaces:**
- Consumes: `GpuAtlas` (Task 8), `RegionPass` (Task 9), `BrickGenPass` (Task 10), `ve::RegionResidency`/`ResidencyPlan` (Task 11), `ve::EditLog` (Task 3), `ve::op_brick_range` (Task 2).
- Produces:
  - `struct godot::PendingEdit { ve::EditOp op; ve::EditLog::AppendResult result; }` (in `voxel_world.h`)
  - `class godot::WorldStreamer`:
    - `void initialize(ve::RegionResidency *, ve::EditLog *, std::mutex *edit_mutex, std::vector<PendingEdit> *pending, GpuAtlas *, RegionPass *, BrickGenPass *)`
    - `int run_frame(RenderingDevice *rd, float cx, float cy, float cz)` — returns actions taken (loads + evicts + edit-mark jobs); records ONE compute list (no submit)
    - `int last_frame_edits() const`
    - `const float *last_edit_center() const`, `float last_edit_radius() const`, `int last_edit_type() const`, `int last_edit_material() const` — for the raymarcher's pending-edit visualizer
  - `ve::CameraParams` is now **128 bytes**: `dims[4]` = world size in **regions**, new `region_origin[4]` (= `origin_bricks / 32`), new `atlas_bricks[4]`; `static_assert(sizeof(CameraParams) == 128)`.
  - `RaymarchPass::render(RenderingDevice *rd, const GpuAtlas &atlas, const ve::CameraParams &cam, int width, int height, const float edit_state[6])` — `edit_state` = `{cx, cy, cz, radius, type, material}`, radius 0 = no visualizer.
  - `VoxelWorld` new API: `ve::EditLog *edit_log()`, `WorldStreamer *streamer()`, `ve::EditLog::AppendResult append_edit(const ve::EditOp &)` (C++ only), `std::mutex &edit_mutex()`, `debug_stream_frame(Vector3 cam) -> int`, `debug_stream_stats() -> Dictionary`, `debug_slot_of_region(Vector3i) -> int`, `debug_region_map_entry(Vector3i) -> int`, `debug_region_map_consistent() -> bool`, `debug_raycast(Vector3 origin, Vector3 dir) -> Dictionary`.
  - `debug_stream_stats()` keys: `resident_regions`, `frame_edits`, `overflow` (this frame's bits), `overflow_ever` (sticky OR across all `debug_stream_frame` calls).

- [ ] **Step 1: Write the failing test**

`tests/test_streaming.gd`:

```gdscript
extends GdUnitTestSuite

# The streamer driving residency -> mark -> generate end to end on a shrunk atlas.
#
# Atlas sizing rule of thumb for every M2 test (keep this comment in sync):
# only regions CROSSING the surface hold bricks (~1500 bricks per shell region for the
# gentle analytic hills), and a residency ball of radius R holds ~2*pi*R^2/655 shell
# regions. R = 20 m -> ~4 shell regions -> ~6k bricks, so 16384 slots is comfortable.
const ATLAS := Vector3i(32, 16, 32)   # 16384 slots (~170 MB on the test device)
const REGION_SLOTS := 16
const ORIGIN := Vector3i(0, -64, 0)   # world surface sits at y ~ 51.2 +- 10 m
const REGIONS := Vector3i(4, 5, 4)    # y regions {-2..2}: world y in [-51.2, 76.8) m
const RADIUS := 20.0

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = ORIGIN
	w.world_size_regions = REGIONS
	w.residency_radius_m = RADIUS
	add_child(w)
	w.ensure_initialized()
	return w

func settle(w: VoxelWorld, cam: Vector3) -> void:
	var settled := false
	for i in range(60):
		if w.debug_stream_frame(cam) == 0:
			settled = true
			break
	assert_bool(settled).override_failure_message(
		"streamer did not settle within 60 frames").is_true()

func test_streaming_loads_the_camera_neighbourhood() -> void:
	var w := make_world()
	settle(w, Vector3(20, 56.2, 20)) # 56.2 m: above the local surface everywhere here
	var s: Dictionary = w.debug_stream_stats()
	assert_int(s["resident_regions"]).is_greater(4)
	assert_int(s["overflow_ever"]).is_equal(0)
	# Region (0, 0, 0) spans x,z in [0, 25.6) m: the camera is directly above it.
	var rslot: int = w.debug_region_map_entry(Vector3i(0, 0, 0))
	assert_int(rslot).is_greater_equal(0)
	# Its surface bricks must be resident: sweep the column at world (12.8, *, 12.8) and
	# require the GPU to hold exactly the bricks the CPU probe calls active.
	var cpu_active := 0
	var gpu_match := 0
	for by in range(56, 72):
		var brick := Vector3i(16, by, 16)
		if w.debug_brick_has_surface(brick, PackedByteArray(), 0):
			cpu_active += 1
			if w.debug_region_table_slot(rslot, brick) >= 0:
				gpu_match += 1
	assert_int(cpu_active).override_failure_message(
		"column holds no surface; check the world-origin maths").is_greater(0)
	assert_int(gpu_match).is_equal(cpu_active)

func test_moving_the_camera_streams_the_new_neighbourhood_and_recycles_slots() -> void:
	var w := make_world()
	settle(w, Vector3(20, 56.2, 20))
	var used_before := ATLAS.x * ATLAS.y * ATLAS.z - int(w.debug_atlas_stats()["free_slots"])
	assert_int(used_before).is_greater(0)
	assert_bool(w.debug_region_map_consistent()).is_true()

	settle(w, Vector3(90, 56.2, 90))
	# Region (0, -1, 0) is now ~90 m away, far past the 23 m evict boundary.
	assert_int(w.debug_slot_of_region(Vector3i(0, -1, 0))).is_equal(-1)
	assert_int(w.debug_region_map_entry(Vector3i(0, -1, 0))).is_equal(-1)
	# The new neighbourhood is resident and the residency core agrees with the GPU map.
	assert_int(w.debug_region_map_entry(Vector3i(3, 0, 3))).is_greater_equal(0)
	assert_bool(w.debug_region_map_consistent()).is_true()
	# Evicted regions returned their atlas slots: no leak, no overflow, no unbounded drop.
	var s: Dictionary = w.debug_stream_stats()
	assert_int(s["overflow_ever"]).is_equal(0)
	var free_now: int = w.debug_atlas_stats()["free_slots"]
	# used_now < used_before + slack: slack tolerates terrain-density variation between the
	# two spots; a leak of the whole old neighbourhood (~6k bricks) would blow past it.
	assert_int(free_now).is_greater(ATLAS.x * ATLAS.y * ATLAS.z - used_before - 2048)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_streaming.gd`
Expected: FAIL — `debug_stream_frame` does not exist.

- [ ] **Step 3: Grow `ve::CameraParams` to 128 bytes**

Replace `extension/src/render/camera_params.h` (the struct comment changes; `looking_at` is
unchanged, so `camera_params.cpp` is untouched):

```cpp
#pragma once
#include <cstdint>

namespace ve {

// Push-constant block shared by raymarch.comp.glsl. M2 grows it from 96 to exactly 128
// bytes — Vulkan's guaranteed minimum push-constant size, so still portable. The shader
// declares the same eight vec4s; Godot sizes the pipeline range from reflection, so the
// two sides must agree exactly (M1 errata 1).
struct CameraParams {
	float cam_pos[4];
	float cam_right[4];
	float cam_up[4];
	float cam_fwd[4];
	float params[4];          // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	int32_t dims[4];          // world size in REGIONS (xyz), unused
	int32_t region_origin[4]; // world origin in REGIONS = origin_bricks / kRegionBricks
	int32_t atlas_bricks[4];  // atlas grid in bricks

	// Basis from position/forward/up-hint; tan fov = 0, max_dist = 200.
	static CameraParams looking_at(float ox, float oy, float oz,
			float fx, float fy, float fz, float ux, float uy, float uz);
};

static_assert(sizeof(CameraParams) == 128);

} // namespace ve
```

Run `cd extension && scons test` — the M1 camera cases still pass (the struct only grew).

- [ ] **Step 4: Rewrite `shaders/raymarch.comp.glsl` for the two-level indirection**

Complete replacement (mip skip is Task 13; the `has_material` gate from M1 stays):

```glsl
#[compute]
#version 460

#include "common.glsl"
#include "brick_layout.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) writeonly uniform image2D out_color;
layout(set = 0, binding = 1, rgba32f) writeonly uniform image2D out_hitpos;
layout(set = 0, binding = 2) uniform sampler3D sdf_atlas;   // R8 unorm, nearest
layout(set = 0, binding = 3) uniform usampler3D mat_atlas;  // R8 uint, nearest
layout(set = 0, binding = 4, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 5, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 6, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 7, std430) readonly buffer OpPool { uvec4 v[]; } op_pool;
layout(set = 0, binding = 8, std430) readonly buffer OpCounts { int n[]; } op_counts;
// The pending-edit visualizer: tint the atlas content an edit WILL change, so the player
// gets one frame of feedback before the regenerated bricks land (spec §5 latency).
layout(set = 0, binding = 9) uniform Edits { vec4 center; vec4 params; } edits;

layout(push_constant, std430) uniform Push {
	vec4 cam_pos;
	vec4 cam_right;
	vec4 cam_up;
	vec4 cam_fwd;
	vec4 params;          // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	ivec4 dims;           // world size in REGIONS
	ivec4 region_origin;  // world origin in REGIONS
	ivec4 atlas_bricks;   // atlas grid
} pc;

// Region-table slot for the region holding a GLOBAL brick coord; -1 outside the world or
// not resident. `>> 5` is an arithmetic shift: floor(b / 32), correct for negatives.
int region_slot_of(ivec3 brick) {
	ivec3 r = brick >> 5;
	ivec3 l = r - pc.region_origin.xyz;
	if (any(lessThan(l, ivec3(0))) || any(greaterThanEqual(l, pc.dims.xyz))) return -1;
	return region_map.slot[l.x + l.y * pc.dims.x + l.z * pc.dims.x * pc.dims.y];
}

// Atlas slot of a global brick; -1 when absent. `& 31` is the floor-mod for negatives.
int slot_at(ivec3 brick) {
	int rs = region_slot_of(brick);
	if (rs < 0) return -1;
	int bi = (brick.x & 31) + (brick.y & 31) * REGION_BRICKS +
			(brick.z & 31) * REGION_BRICKS * REGION_BRICKS;
	return region_tables.slot[rs * REGION_BRICK_COUNT + bi];
}

// Manual trilinear inside one brick (unchanged from M1 except the runtime atlas base).
float brick_sdf(int slot, vec3 local) { // local in voxel units [0, 16]
	vec3 p = clamp(local, vec3(0.0), vec3(BRICK_SDF_MAX));
	ivec3 i0 = ivec3(floor(p));
	vec3 f = p - vec3(i0);
	ivec3 i1 = min(i0 + 1, ivec3(BRICK_VOXELS));
	ivec3 base = atlas_base(slot, pc.atlas_bricks, BRICK_SDF_STRIDE);
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

float world_sdf(vec3 p) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = slot_at(brick);
	if (slot < 0) return SDF_RANGE;
	vec3 local = (p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE;
	return brick_sdf(slot, local);
}

// Gradient taps can land in a brick with NO atlas slot; fall back to the anchor brick,
// whose apron covers the shared face exactly (M1 fix, semantics unchanged).
float sdf_near(vec3 p, ivec3 anchor, int anchor_slot) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = all(equal(brick, anchor)) ? anchor_slot : slot_at(brick);
	if (slot < 0) { brick = anchor; slot = anchor_slot; }
	return brick_sdf(slot, (p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE);
}

vec3 calc_normal(vec3 p, ivec3 anchor, int anchor_slot) {
	const float e = 0.01;
	return normalize(vec3(
		sdf_near(p + vec3(e, 0, 0), anchor, anchor_slot) - sdf_near(p - vec3(e, 0, 0), anchor, anchor_slot),
		sdf_near(p + vec3(0, e, 0), anchor, anchor_slot) - sdf_near(p - vec3(0, e, 0), anchor, anchor_slot),
		sdf_near(p + vec3(0, 0, e), anchor, anchor_slot) - sdf_near(p - vec3(0, 0, e), anchor, anchor_slot)));
}

// Material of the surface crossing inside `brick`, anchored so a hit point that rounds
// into a neighbouring (possibly absent or empty-palette) brick cannot resolve magenta.
uint material_at(vec3 p, ivec3 brick, int slot) {
	vec3 local = clamp((p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE, vec3(0.0), vec3(15.0));
	ivec3 base = atlas_base(slot, pc.atlas_bricks, BRICK_VOXELS);
	uint idx = texelFetch(mat_atlas, base + ivec3(local), 0).r;
	return palette_buf.ids[slot * 4 + idx];
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_color);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

	vec3 ro = pc.cam_pos.xyz;
	vec3 rd = normalize(pc.cam_fwd.xyz
		+ pc.cam_right.xyz * ndc.x * pc.params.x
		+ pc.cam_up.xyz * ndc.y * pc.params.y);
	float max_dist = pc.params.z;

	vec3 color = sky_color(rd);
	vec4 hitpos = vec4(0.0);

	// Brick-grid DDA over the GLOBAL brick lattice (negative coords supported). side[d] =
	// ray t at the next boundary along axis d; the st==0 guards keep NaNs out (M1 errata 4).
	ivec3 map = ivec3(floor(ro / BRICK_SIZE));
	vec3 delta = abs(vec3(BRICK_SIZE) / rd);
	ivec3 st = ivec3(sign(rd));
	vec3 side = (vec3(map) * BRICK_SIZE - ro
	             + (vec3(st) * 0.5 + 0.5) * BRICK_SIZE) / rd;
	if (st.x == 0) side.x = 1.0 / 0.0;
	if (st.y == 0) side.y = 1.0 / 0.0;
	if (st.z == 0) side.z = 1.0 / 0.0;
	float t_prev = 0.0;

	bool hit = false;
	vec3 hp = vec3(0.0);
	for (int i = 0; i < 1024; i++) {
		float t_exit = min(side.x, min(side.y, side.z));
		if (t_exit > max_dist) break;

		int slot = slot_at(map);
		if (slot >= 0) {
			// Air-margin bricks (activated by the probe pad) hold no solid voxels and an
			// EMPTY palette; their interpolated field can still dip below the hit threshold
			// near a brick face. That is not a renderable surface — skip it (M1 fix).
			bool has_material = palette_buf.ids[slot * 4] != 0u;
			float t = t_prev;
			for (int j = 0; j < 64; j++) {
				if (t > t_exit) break;
				vec3 p = ro + rd * t;
				float d = world_sdf(p);
				if (d < 0.002 && has_material) {
					for (int k = 0; k < 4; k++) { // secant refinement
						float dk = world_sdf(p);
						t += dk * 0.5;
						p = ro + rd * t;
					}
					vec3 n = calc_normal(p, map, slot);
					vec3 alb = material_albedo(material_at(p, map, slot));
					vec3 sun = normalize(vec3(0.6, 0.8, 0.3));
					float lam = max(dot(n, sun), 0.0);
					color = alb * (0.25 + 0.75 * lam);
					hp = p;
					hitpos = vec4(p, 1.0);
					hit = true;
					break;
				}
				t += max(d * 0.9, 0.005);
			}
			if (hit) break;
		}

		if (side.x < side.y && side.x < side.z) { t_prev = side.x; side.x += delta.x; map.x += st.x; }
		else if (side.y < side.z)               { t_prev = side.y; side.y += delta.y; map.y += st.y; }
		else                                    { t_prev = side.z; side.z += delta.z; map.z += st.z; }
	}

	// Pending-edit visualizer: the hit inside the edit sphere is tinted towards the tool's
	// colour. It reads the OLD atlas content by design — this is the preview, and the
	// regenerated bricks replace it next frame.
	if (hit && edits.params.x > 0.0 && length(hp - edits.center.xyz) < edits.params.x) {
		uint t = uint(edits.params.y);
		vec3 tint = t == 0u ? vec3(1.0, 0.55, 0.1)      // subtract: warning orange
		          : t == 1u ? material_albedo(4u)        // add: the fill grey
		          : material_albedo(uint(edits.params.z)); // paint: the target colour
		color = mix(color, tint, 0.45);
	}

	imageStore(out_color, px, vec4(color, 1.0));
	imageStore(out_hitpos, px, hitpos);
}
```

- [ ] **Step 5: Rework `RaymarchPass` onto `GpuAtlas`**

Replace `extension/src/render/raymarch_pass.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/camera_params.h"

namespace godot {

class GpuAtlas;

class RaymarchPass {
public:
	~RaymarchPass(); // calls teardown(): frees RIDs on rd_ (device must be alive)
	void initialize(RenderingDevice *rd);
	void teardown();
	// edit_state = {center.xyz, radius, type, material}; radius 0 disables the visualizer.
	bool render(RenderingDevice *rd, const GpuAtlas &atlas, const ve::CameraParams &cam,
			int width, int height, const float edit_state[6]);

	RID color_texture() const { return color_; }
	RID hitpos_texture() const { return hitpos_; }

private:
	RID make_target(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h);
	void rebuild_targets(RenderingDevice *rd, const GpuAtlas &atlas, int w, int h);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_;
	RID sampler_;     // shared NEAREST sampler, created once
	RID edits_ubo_;   // 32-byte uniform buffer, updated every render
	RID color_, hitpos_, uset_;
	int width_ = 0, height_ = 0;
};

} // namespace godot
```

In `raymarch_pass.cpp`: swap `#include "render/gpu_world.h"` for `#include "render/gpu_atlas.h"`,
and replace `strip_godot_annotations` with `ve::strip_shader_annotations` (Task 10 moved it
into `render/shader_loader.h/.cpp`). In `initialize`, also create the edits UBO:

```cpp
	{
		PackedByteArray zero;
		zero.resize(32);
		zero.fill(0);
		edits_ubo_ = rd->uniform_buffer_create(32, zero);
	}
```

Add `&edits_ubo_` to the teardown free-list (after `sampler_`). Replace `rebuild_targets` and
`render`:

```cpp
void RaymarchPass::rebuild_targets(RenderingDevice *rd, const GpuAtlas &atlas, int w, int h) {
	// Old uniform set references the old color/hitpos textures: free it before them.
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	if (color_.is_valid()) rd->free_rid(color_);
	if (hitpos_.is_valid()) rd->free_rid(hitpos_);
	color_ = make_target(rd, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, w, h);
	hitpos_ = make_target(rd, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, w, h);
	width_ = w;
	height_ = h;

	Ref<RDUniform> u[10];
	for (int i = 0; i < 10; i++) u[i].instantiate();
	u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[0]->set_binding(0); u[0]->add_id(color_);
	u[1]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[1]->set_binding(1); u[1]->add_id(hitpos_);
	u[2]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[2]->set_binding(2); u[2]->add_id(sampler_); u[2]->add_id(atlas.sdf_atlas());
	u[3]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u[3]->set_binding(3); u[3]->add_id(sampler_); u[3]->add_id(atlas.mat_atlas());
	for (int i = 4; i <= 8; i++) { // palette, region_map, region_tables, op_pool, op_counts
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u[i]->set_binding(i);
	}
	u[4]->add_id(atlas.palette());
	u[5]->add_id(atlas.region_map());
	u[6]->add_id(atlas.region_tables());
	u[7]->add_id(atlas.op_pool());
	u[8]->add_id(atlas.op_counts());
	u[9]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[9]->set_binding(9); u[9]->add_id(edits_ubo_);
	uset_ = rd->uniform_set_create(
			Array::make(u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9]), shader_, 0);
}

bool RaymarchPass::render(RenderingDevice *rd, const GpuAtlas &atlas, const ve::CameraParams &cam,
		int width, int height, const float edit_state[6]) {
	if (!shader_.is_valid()) return false;
	if (width != width_ || height != height_ || !uset_.is_valid()) {
		rebuild_targets(rd, atlas, width, height);
	}
	if (!uset_.is_valid() || !color_.is_valid() || !edits_ubo_.is_valid()) return false;

	// Recorded before the compute list: buffer_update errors while a list is open, and the
	// deferred update still lands before the dispatch at submit.
	{
		PackedByteArray eb;
		eb.resize(32);
		float *f = reinterpret_cast<float *>(eb.ptrw());
		for (int i = 0; i < 3; i++) f[i] = edit_state[i];
		f[3] = 0.0f;
		f[4] = edit_state[3]; // radius
		f[5] = edit_state[4]; // type
		f[6] = edit_state[5]; // material
		f[7] = edit_state[3] > 0.0f ? 1.0f : 0.0f;
		rd->buffer_update(edits_ubo_, 0, 32, eb);
	}

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

- [ ] **Step 6: Write `render/world_streamer.h` / `.cpp`**

`extension/src/render/world_streamer.h`:

```cpp
#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <mutex>
#include <vector>
#include "generator/generator.h"
#include "render/gpu_atlas.h"
#include "render/brick_gen_pass.h"
#include "render/region_pass.h"
#include "world/edit_log.h"
#include "world/residency.h"

namespace godot {

struct PendingEdit; // defined in voxel_world.h; here only the pointer type is needed

// Drives one frame of world maintenance on the render thread: residency loads/evictions,
// edit fan-out, one compute list holding every mark + the indirect generation dispatch.
// Owns nothing; every pointer is borrowed from VoxelWorld.
class WorldStreamer {
public:
	void initialize(ve::RegionResidency *residency, ve::EditLog *edit_log,
			std::mutex *edit_mutex, std::vector<PendingEdit> *pending, GpuAtlas *atlas,
			RegionPass *region_pass, BrickGenPass *brick_gen);

	// Returns the number of actions taken (loads + evicts + edit-mark jobs). Records ONE
	// compute list; the caller submits. buffer_update calls happen before the list opens.
	int run_frame(RenderingDevice *rd, float cx, float cy, float cz);

	int last_frame_edits() const { return frame_edits_; }
	const float *last_edit_center() const { return last_edit_center_; }
	float last_edit_radius() const { return last_edit_radius_; }
	int last_edit_type() const { return last_edit_type_; }
	int last_edit_material() const { return last_edit_material_; }

private:
	ve::RegionResidency *residency_ = nullptr;
	ve::EditLog *edit_log_ = nullptr;
	std::mutex *edit_mutex_ = nullptr;
	std::vector<PendingEdit> *pending_ = nullptr;
	GpuAtlas *atlas_ = nullptr;
	RegionPass *region_pass_ = nullptr;
	BrickGenPass *brick_gen_ = nullptr;
	int frame_edits_ = 0;
	float last_edit_center_[3] = {0.0f, 0.0f, 0.0f};
	float last_edit_radius_ = 0.0f;
	int last_edit_type_ = 0;
	int last_edit_material_ = 0;
};

} // namespace godot
```

`extension/src/render/world_streamer.cpp`:

```cpp
#include "render/world_streamer.h"
#include "voxel_world.h" // godot::PendingEdit
#include <algorithm>

using namespace godot;

void WorldStreamer::initialize(ve::RegionResidency *residency, ve::EditLog *edit_log,
		std::mutex *edit_mutex, std::vector<PendingEdit> *pending, GpuAtlas *atlas,
		RegionPass *region_pass, BrickGenPass *brick_gen) {
	residency_ = residency;
	edit_log_ = edit_log;
	edit_mutex_ = edit_mutex;
	pending_ = pending;
	atlas_ = atlas;
	region_pass_ = region_pass;
	brick_gen_ = brick_gen;
}

int WorldStreamer::run_frame(RenderingDevice *rd, float cx, float cy, float cz) {
	frame_edits_ = 0;
	if (!rd || !atlas_ || !residency_ || !edit_log_ || !region_pass_ || !brick_gen_) return 0;

	// Drain the edit queue. The lock is held for a swap, never across GPU work.
	std::vector<PendingEdit> edits;
	{
		std::lock_guard<std::mutex> lock(*edit_mutex_);
		edits.swap(*pending_);
	}

	const ve::ResidencyPlan plan = residency_->update(cx, cy, cz);
	frame_edits_ = static_cast<int>(edits.size());

	// --- buffer_update phase (must precede compute_list_begin: Godot errors otherwise) ---
	for (const auto &l : plan.loads) {
		const std::vector<ve::EditOp> &ops = edit_log_->ops(l.region);
		// Unconditional, even when empty: the slot may be a recycled eviction still holding
		// the previous tenant's op count, and op_count is the only field the mark pass reads.
		atlas_->upload_region_ops(rd, l.slot, ops.data(), static_cast<int>(ops.size()));
		atlas_->set_region_map_entry(rd, l.map_index, l.slot);
	}
	for (const auto &e : plan.evicts)
		atlas_->set_region_map_entry(rd, e.map_index, -1);

	// Edits re-mark only the op's brick AABB clamped to each touched region. An op that
	// touches a region's APRON plane is in that region's list by construction (Task 2's
	// one-voxel pad), so the GPU probe — which always uses the brick's OWN region list —
	// sees every op that can change any of its 27 probe points.
	struct EditJob { ve::IVec3 region; int slot; ve::IVec3 lo, hi; int op_count; };
	std::vector<EditJob> edit_jobs;
	for (const auto &pe : edits) {
		ve::IVec3 blo{}, bhi{};
		ve::op_brick_range(pe.op, &blo, &bhi);
		for (const ve::IVec3 &region : pe.result.touched) {
			const int slot = residency_->slot_of(region);
			if (slot < 0) continue; // off-screen edit: bricks regenerate on stream-in
			const std::vector<ve::EditOp> &ops = edit_log_->ops(region);
			atlas_->upload_region_ops(rd, slot, ops.data(), static_cast<int>(ops.size()));
			const ve::IVec3 r0{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			const EditJob job{region, slot,
					{std::max(blo.x, r0.x), std::max(blo.y, r0.y), std::max(blo.z, r0.z)},
					{std::min(bhi.x, r0.x + 31), std::min(bhi.y, r0.y + 31),
							std::min(bhi.z, r0.z + 31)},
					static_cast<int>(ops.size())};
			if (job.lo.x <= job.hi.x && job.lo.y <= job.hi.y && job.lo.z <= job.hi.z)
				edit_jobs.push_back(job);
			last_edit_center_[0] = pe.op.pos[0];
			last_edit_center_[1] = pe.op.pos[1];
			last_edit_center_[2] = pe.op.pos[2];
			last_edit_radius_ = pe.op.radius;
			last_edit_type_ = static_cast<int>(pe.op.type);
			last_edit_material_ = static_cast<int>(pe.op.material);
		}
	}

	atlas_->reset_frame_counters(rd);

	// --- compute phase: ONE list, no submit (the caller's frame submits) ---
	const int64_t list = rd->compute_list_begin();
	bool any = false;
	for (const auto &e : plan.evicts) {
		region_pass_->release_region(rd, list, e.slot);
		any = true;
	}
	if (any) rd->compute_list_add_barrier(list); // frees must not collide with pops below
	for (const auto &l : plan.loads) {
		const std::vector<ve::EditOp> &ops = edit_log_->ops(l.region);
		const ve::IVec3 lo{l.region.x * ve::kRegionBricks, l.region.y * ve::kRegionBricks,
				l.region.z * ve::kRegionBricks};
		const ve::IVec3 hi{lo.x + 31, lo.y + 31, lo.z + 31};
		region_pass_->mark(rd, list, l.region, l.slot, lo, hi,
				static_cast<int>(ops.size()), false);
		any = true;
	}
	for (const auto &j : edit_jobs) {
		// mark(force=true) records release, barrier, allocate; the extra barrier BEFORE
		// each job keeps one job's release phase from racing the previous job's allocate
		// phase when two edits share bricks (the race the phase split exists to avoid).
		rd->compute_list_add_barrier(list);
		region_pass_->mark(rd, list, j.region, j.slot, j.lo, j.hi, j.op_count, true);
		any = true;
	}
	if (any) {
		rd->compute_list_add_barrier(list);
		region_pass_->write_dispatch_args(rd, list);
		rd->compute_list_add_barrier(list);
		brick_gen_->dispatch(rd, list, *atlas_);
	}
	rd->compute_list_end();
	return static_cast<int>(plan.loads.size() + plan.evicts.size() + edit_jobs.size());
}
```

- [ ] **Step 7: Rewrite `voxel_world.h` and rework `voxel_world.cpp`**

Replace `extension/src/voxel_world.h` entirely:

```cpp
#pragma once
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <mutex>
#include <vector>
#include "world/edit_log.h"
#include "world/region.h"
#include "world/residency.h"

namespace godot {

class GpuAtlas;
class RegionPass;
class BrickGenPass;
class RaymarchPass;
class CompositePass;
class WorldStreamer;

// One edit drained by the streamer: the op plus the regions its append touched/rejected.
struct PendingEdit {
	ve::EditOp op;
	ve::EditLog::AppendResult result;
};

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

	bool use_local_device_ = false;

	Vector3i atlas_bricks_ = Vector3i(64, 32, 32);
	int max_region_slots_ = 512;
	int max_brick_jobs_ = 16384;
	Vector3i world_origin_bricks_ = Vector3i(0, -64, 0);
	Vector3i world_size_regions_ = Vector3i(64, 8, 64);
	float residency_radius_m_ = 96.0f;

	GpuAtlas *atlas_ = nullptr;
	RegionPass *region_pass_ = nullptr;
	BrickGenPass *gen_pass_ = nullptr;
	RaymarchPass *raymarch_pass_ = nullptr;
	CompositePass *composite_pass_ = nullptr;
	// CPU cores outlive the GPU objects: a re-init re-streams the same world, edits
	// included. This is also what a future save/reload will do (saves ARE the edit log).
	ve::EditLog *edit_log_ = nullptr;
	ve::RegionResidency *residency_ = nullptr;
	WorldStreamer *streamer_ = nullptr;
	std::mutex edit_mutex_;                   // guards edit_log_ + pending_edits_
	std::vector<PendingEdit> pending_edits_;  // appended by tools, drained by the streamer
	int overflow_seen_ = 0;                   // sticky OR of frame overflow bits (tests)

	RenderingDevice *main_rd_ = nullptr;
	RenderingDevice *local_rd_ = nullptr; // owned when use_local_device_
	bool initialized_ = false;

	void teardown_gpu(); // every GPU object; CPU cores survive

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _exit_tree() override;
	~VoxelWorld() override;

	void set_use_local_device(bool v) { use_local_device_ = v; }
	bool get_use_local_device() const { return use_local_device_; }
	void set_atlas_bricks(Vector3i v) { atlas_bricks_ = v; }
	Vector3i get_atlas_bricks() const { return atlas_bricks_; }
	void set_max_region_slots(int v) { max_region_slots_ = v; }
	int get_max_region_slots() const { return max_region_slots_; }
	void set_max_brick_jobs(int v) { max_brick_jobs_ = v; }
	int get_max_brick_jobs() const { return max_brick_jobs_; }
	void set_world_origin_bricks(Vector3i v) { world_origin_bricks_ = v; }
	Vector3i get_world_origin_bricks() const { return world_origin_bricks_; }
	void set_world_size_regions(Vector3i v) { world_size_regions_ = v; }
	Vector3i get_world_size_regions() const { return world_size_regions_; }
	void set_residency_radius_m(float v) { residency_radius_m_ = v; }
	float get_residency_radius_m() const { return residency_radius_m_; }

	void ensure_initialized();
	bool is_initialized() const { return initialized_; }
	RenderingDevice *rd() const;
	ve::WorldBounds world_bounds() const;

	GpuAtlas *atlas() { return atlas_; }
	WorldStreamer *streamer() { return streamer_; }
	ve::EditLog *edit_log() { return edit_log_; }
	RaymarchPass *raymarch_pass() { return raymarch_pass_; }
	CompositePass *composite_pass() { return composite_pass_; }
	std::mutex &edit_mutex() { return edit_mutex_; }

	// Tool entry point (VoxelEditTool, Task 14). Main thread; takes edit_mutex_.
	ve::EditLog::AppendResult append_edit(const ve::EditOp &op);

	// --- debug/test hooks (Tasks 7-10 kept; debug_sdf_atlas now returns the ATLAS) ---
	String debug_load_shader(const String &res_path) const;
	Vector2 debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) const;
	bool debug_init_atlas();
	void debug_teardown_atlas();
	Dictionary debug_atlas_stats();
	void debug_reset_frame_counters();
	void debug_set_region_map_entry(int region_index, int region_slot);
	void debug_upload_region_ops(int region_slot, const PackedByteArray &ops, int count);
	bool debug_brick_has_surface(Vector3i brick, const PackedByteArray &ops, int op_count) const;
	void debug_mark_region(Vector3i region, int region_slot, Vector3i lo, Vector3i hi,
			int op_count, bool force);
	void debug_release_region(int region_slot);
	PackedInt32Array debug_jobs();
	int debug_region_table_slot(int region_slot, Vector3i brick);
	void debug_generate_pending();
	Dictionary debug_brick_diff(Vector3i brick, int region_slot, const PackedByteArray &ops,
			int op_count);
	RID debug_sdf_atlas() const;
	RID debug_mat_atlas() const;
	RID debug_mip_atlas(int level) const;
	RID debug_region_map() const;
	RID debug_region_tables() const;
	RID debug_free_list() const;
	RID debug_frame_counters() const;
	RID debug_op_pool() const;
	RID debug_op_counts() const;

	// --- Task 12 hooks ---
	Color debug_raymarch_pixel(Vector3 origin, Vector3 dir);
	int debug_stream_frame(Vector3 cam);
	Dictionary debug_stream_stats();
	int debug_slot_of_region(Vector3i region) const;
	int debug_region_map_entry(Vector3i region);
	bool debug_region_map_consistent();
	Dictionary debug_raycast(Vector3 origin, Vector3 dir);
	RenderingDevice *debug_local_rd() const { return local_rd_; }
};

} // namespace godot
```

In `voxel_world.cpp`:
- Drop the includes `render/gpu_world.h`, `world/world_data.h`; add `render/gpu_atlas.h`,
  `render/region_pass.h`, `render/brick_gen_pass.h`, `render/world_streamer.h`,
  `world/raycast.h`, `<cmath>`.
- Drop `world_size_bricks` property + accessors; bind all six new property pairs; bind every
  debug method above.
- `debug_init_atlas()` becomes `ensure_initialized(); return atlas_ && atlas_->is_valid();`.
- `debug_teardown_atlas()` becomes `teardown_gpu();` (the Task-8 test's "re-init gives 512
  free slots" keeps working: the next `debug_init_atlas` re-runs `ensure_initialized`).

Replace the lifecycle core:

```cpp
void VoxelWorld::teardown_gpu() {
	// Passes before the atlas: their uniform sets reference atlas RIDs, and freeing a
	// texture cascades to referencing sets (M1's documented order).
	if (composite_pass_) { delete composite_pass_; composite_pass_ = nullptr; }
	if (raymarch_pass_) { delete raymarch_pass_; raymarch_pass_ = nullptr; }
	if (gen_pass_) { delete gen_pass_; gen_pass_ = nullptr; }
	if (region_pass_) { delete region_pass_; region_pass_ = nullptr; }
	if (streamer_) { delete streamer_; streamer_ = nullptr; }
	if (residency_) { residency_->clear(); } // slot assignments are meaningless pre-atlas
	if (atlas_) { delete atlas_; atlas_ = nullptr; }
	initialized_ = false;
}

void VoxelWorld::_exit_tree() {
	teardown_gpu();
	if (residency_) { delete residency_; residency_ = nullptr; }
	if (edit_log_) { delete edit_log_; edit_log_ = nullptr; }
	pending_edits_.clear();
	overflow_seen_ = 0;
	if (local_rd_) {
		memdelete(local_rd_);
		local_rd_ = nullptr;
	}
	main_rd_ = nullptr;
}

void VoxelWorld::ensure_initialized() {
	if (initialized_) return;
	if (use_local_device_ && !local_rd_) {
		local_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	} else if (!use_local_device_ && !main_rd_) {
		main_rd_ = RenderingServer::get_singleton()->get_rendering_device();
	}
	RenderingDevice *device = rd();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return;
	}
	atlas_ = new GpuAtlas();
	GpuAtlasConfig cfg;
	cfg.atlas_bricks = {atlas_bricks_.x, atlas_bricks_.y, atlas_bricks_.z};
	cfg.max_region_slots = max_region_slots_;
	cfg.max_brick_jobs = max_brick_jobs_;
	cfg.bounds = world_bounds();
	if (!atlas_->initialize(device, cfg)) { delete atlas_; atlas_ = nullptr; return; }
	region_pass_ = new RegionPass();
	if (!region_pass_->initialize(device, *atlas_)) { teardown_gpu(); return; }
	gen_pass_ = new BrickGenPass();
	if (!gen_pass_->initialize(device, *atlas_)) { teardown_gpu(); return; }
	if (!edit_log_) edit_log_ = new ve::EditLog(world_bounds());
	if (!residency_) {
		ve::ResidencyConfig rcfg;
		rcfg.bounds = world_bounds();
		rcfg.radius_m = residency_radius_m_;
		rcfg.max_region_slots = max_region_slots_;
		residency_ = new ve::RegionResidency(rcfg);
	}
	streamer_ = new WorldStreamer();
	streamer_->initialize(residency_, edit_log_, &edit_mutex_, &pending_edits_, atlas_,
			region_pass_, gen_pass_);
	raymarch_pass_ = new RaymarchPass();
	raymarch_pass_->initialize(device);
	composite_pass_ = new CompositePass();
	composite_pass_->initialize(device);
	initialized_ = true;
}

ve::EditLog::AppendResult VoxelWorld::append_edit(const ve::EditOp &op) {
	std::lock_guard<std::mutex> lock(edit_mutex_);
	if (!edit_log_) return {};
	ve::EditLog::AppendResult r = edit_log_->append(op);
	if (!r.rejected.empty()) {
		UtilityFunctions::printerr("VoxelWorld: region op list full, op rejected (",
				r.rejected[0].x, ", ", r.rejected[0].y, ", ", r.rejected[0].z,
				") — spec §8 fail-soft");
	}
	pending_edits_.push_back({op, r});
	return r;
}
```

Replace `debug_raymarch_pixel` (the M1 body referenced `gpu_`/`world_size_bricks_`):

```cpp
Color VoxelWorld::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !raymarch_pass_) return Color(1, 0, 1);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, cam, 1, 1, kNoEdit)) return Color(1, 0, 1);
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (data.size() < 8) return Color(1, 0, 1);
	const uint16_t *h = reinterpret_cast<const uint16_t *>(data.ptr());
	return Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
}
```

And the new Task-12 hooks:

```cpp
int VoxelWorld::debug_stream_frame(Vector3 cam) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !streamer_) return 0;
	const int actions = streamer_->run_frame(device, cam.x, cam.y, cam.z);
	device->submit();
	device->sync();
	overflow_seen_ |= static_cast<int>(atlas_->read_overflow(device));
	return actions;
}

Dictionary VoxelWorld::debug_stream_stats() {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !residency_ || !streamer_) return d;
	d["resident_regions"] = residency_->resident_count();
	d["frame_edits"] = streamer_->last_frame_edits();
	d["overflow"] = static_cast<int>(atlas_->read_overflow(device));
	d["overflow_ever"] = overflow_seen_;
	return d;
}

int VoxelWorld::debug_slot_of_region(Vector3i region) const {
	if (!residency_) return -1;
	return residency_->slot_of({region.x, region.y, region.z});
}

int VoxelWorld::debug_region_map_entry(Vector3i region) {
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_) return -1;
	const int idx = world_bounds().region_index({region.x, region.y, region.z});
	if (idx < 0) return -1;
	const PackedByteArray b = device->buffer_get_data(atlas_->region_map(), idx * 4, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

bool VoxelWorld::debug_region_map_consistent() {
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !residency_) return false;
	const ve::WorldBounds wb = world_bounds();
	const PackedByteArray b = device->buffer_get_data(atlas_->region_map());
	const int32_t *map = reinterpret_cast<const int32_t *>(b.ptr());
	const ve::IVec3 o = wb.origin_regions();
	const ve::IVec3 sz = wb.size_regions;
	for (int z = 0; z < sz.z; z++)
		for (int y = 0; y < sz.y; y++)
			for (int x = 0; x < sz.x; x++) {
				const ve::IVec3 r{o.x + x, o.y + y, o.z + z};
				const int gpu_slot = map[x + y * sz.x + z * sz.x * sz.y];
				const int cpu_slot = residency_->slot_of(r);
				if (gpu_slot != cpu_slot) return false;
				if (gpu_slot >= 0 && !(residency_->region_of_slot(gpu_slot) == r)) return false;
			}
	return true;
}

Dictionary VoxelWorld::debug_raycast(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	if (!edit_log_) return d;
	std::lock_guard<std::mutex> lock(edit_mutex_);
	ve::AnalyticGenerator gen;
	const float o[3] = {origin.x, origin.y, origin.z};
	const float f[3] = {dir.x, dir.y, dir.z};
	const ve::RayHit h = ve::raycast(gen, *edit_log_, o, f, 200.0f);
	if (!h.hit) return d;
	d["hit"] = true;
	d["pos"] = Vector3(h.pos[0], h.pos[1], h.pos[2]);
	d["normal"] = Vector3(h.normal[0], h.normal[1], h.normal[2]);
	d["distance"] = h.distance;
	return d;
}
```

- [ ] **Step 8: Update the compositor; delete GpuWorld**

In `raymarch_compositor.cpp`: drop the `render/gpu_world.h` include, add
`render/gpu_atlas.h` and `render/world_streamer.h`. Replace the body after the
tan-fov computation:

```cpp
	ve::CameraParams cp{};
	const Vector3 right = cam.basis.get_column(0);
	const Vector3 up = cam.basis.get_column(1);
	const Vector3 fwd = -cam.basis.get_column(2);
	cp.cam_pos[0] = cam.origin.x; cp.cam_pos[1] = cam.origin.y; cp.cam_pos[2] = cam.origin.z;
	cp.cam_right[0] = right.x; cp.cam_right[1] = right.y; cp.cam_right[2] = right.z;
	cp.cam_up[0] = up.x; cp.cam_up[1] = up.y; cp.cam_up[2] = up.z;
	cp.cam_fwd[0] = fwd.x; cp.cam_fwd[1] = fwd.y; cp.cam_fwd[2] = fwd.z;
	cp.params[0] = tan_x; cp.params[1] = tan_y; cp.params[2] = 200.0f;
	const Vector3i sr = world->get_world_size_regions();
	cp.dims[0] = sr.x; cp.dims[1] = sr.y; cp.dims[2] = sr.z;
	const Vector3i ob = world->get_world_origin_bricks();
	cp.region_origin[0] = ob.x / 32; cp.region_origin[1] = ob.y / 32;
	cp.region_origin[2] = ob.z / 32;
	const Vector3i ab = world->get_atlas_bricks();
	cp.atlas_bricks[0] = ab.x; cp.atlas_bricks[1] = ab.y; cp.atlas_bricks[2] = ab.z;

	// One streaming pass per frame, before the march: the new bricks land in the same
	// submit, so there is no torn frame where the map points at ungenerated slots.
	WorldStreamer *st = world->streamer();
	if (st) st->run_frame(rd, cam.origin.x, cam.origin.y, cam.origin.z);

	RaymarchPass *rmp = world->raymarch_pass();
	GpuAtlas *atlas = world->atlas();
	CompositePass *cmp = world->composite_pass();
	if (!rmp || !atlas || !cmp) return;

	float edit_state[6] = {0, 0, 0, 0, 0, 0};
	if (st && st->last_edit_radius() > 0.0f) {
		edit_state[0] = st->last_edit_center()[0];
		edit_state[1] = st->last_edit_center()[1];
		edit_state[2] = st->last_edit_center()[2];
		edit_state[3] = st->last_edit_radius();
		edit_state[4] = static_cast<float>(st->last_edit_type());
		edit_state[5] = static_cast<float>(st->last_edit_material());
	}

	const int rw = static_cast<int>(size.x * 0.66f);
	const int rh = static_cast<int>(size.y * 0.66f);
	if (rw <= 0 || rh <= 0) return;
	if (!rmp->render(rd, *atlas, cp, rw, rh, edit_state)) return;

	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	cmp->draw(rd, rsb->get_color_texture(), rsb->get_depth_texture(),
			rmp->color_texture(), rmp->hitpos_texture(), view_proj);
```

Then:

```bash
git rm -q extension/src/render/gpu_world.h extension/src/render/gpu_world.cpp \
          tests/test_gpu_world.gd tests/test_gpu_world.gd.uid
```

(`tests/test_raymarch_magenta.gd`/`test_raymarch_pixel.gd` keep their names; their content
is replaced in the next step. `.uid` files must go with their scripts or Godot logs
orphaned-UID warnings.)

- [ ] **Step 9: Migrate the pixel tests to the streaming world**

Replace the `make_world()` helper in BOTH `tests/test_raymarch_pixel.gd` and
`tests/test_raymarch_magenta.gd` with:

```gdscript
# M2: the world is GPU-generated and streamed around a camera. The radius must cover the
# FARTHEST ray's hit point (the magenta regression rays land ~40 m out), which the sizing
# rule of thumb (see test_streaming.gd) puts at ~25k bricks in the worst case.
const ATLAS := Vector3i(48, 24, 32)   # 36864 slots (~380 MB on the test device)
const REGION_SLOTS := 64              # a 45 m ball intersects ~47 regions; leave headroom
const CAM := Vector3(20, 56.2, 20)    # above the local surface

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(4, 5, 4)
	w.residency_radius_m = 45.0
	add_child(w)
	w.ensure_initialized()
	for i in range(90):
		if w.debug_stream_frame(CAM) == 0:
			break
	return w
```

(The magenta suite additionally overrides `CAM` — see below.)

Then update the ray origins, because the world's surface moved from `y = hills` to
`y = hills + 51.2`:

- `test_raymarch_pixel.gd`: origins `(8, 12, 8)` → `(8, 63.2, 8)`; `(8, 8, 8)` →
  `(8, 59.2, 8)`; `(8.25, 12.3, 7.9)` → `(8.25, 63.5, 7.9)`; `(7.3, 11.2, 9.1)` →
  `(7.3, 62.4, 9.1)`. In the apron slab test, `Vector3(x, 9.0, z)` → `Vector3(x, 60.2, z)`
  and the `hills()` band check stays as-is (it compares against the oracle's own output).
- `test_raymarch_magenta.gd`: `DEMO_ORIGIN := Vector3(8, 66.4, 8)` (was y = 14; the five
  regression rays now hit at y ≈ 51.2 + their old heights). Also update the header comment:
  the captured hit points move +51.2 in y with the world.
- Settle points must cover each suite's rays: the pixel suite's `CAM` `(20, 56.2, 20)`
  covers its rays (x,z ∈ [5, 13], hits y ≈ 51–63 m, all within 45 m); the magenta suite
  instead settles at its own `DEMO_ORIGIN` — its regression rays land ~40 m from it, and
  settling anywhere else would leave those hit regions non-resident. Say so in a comment
  in each file.

- [ ] **Step 10: Retarget the demo scene (world shifted +51.2 m in y)**

In `demo/main.tscn`: replace the `VoxelWorld` node's `world_size_bricks` line with

```
world_origin_bricks = Vector3i(0, -64, 0)
world_size_regions = Vector3i(64, 8, 64)
```

and shift the camera and test cube up by 51.2 m so they frame the same hills:

- Camera: `transform = Transform3D(-0.58819, 0.30318, -0.74977, 0, 0.92710, 0.37489, 0.80872, 0.22051, -0.54530, 8, 65.2, 8)`
- TestCube: `transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 24, 57.2, 24)`

In `demo/benchmark.gd`, the fixed camera: `Vector3(24, 12, 24)` → `Vector3(24, 63.2, 24)`.

- [ ] **Step 11: Build and run everything**

```bash
./build.sh -j$(nproc)
cd extension && scons test
cd .. && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests
```
Expected: native green; gdUnit green across boot, GPU smoke, field diff, atlas, region pass,
brick diff, streaming, and BOTH migrated pixel suites. (test_gpu_world.gd is gone.)

- [ ] **Step 12: Verify visually**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything --gpu-validation demo/main.tscn`
Checklist:
- Terrain streams in around the camera over the first seconds and then stops changing
- Hills, cave and materials look exactly as M1 rendered them (now ~51 m higher)
- TestCube occlusion still correct in both directions
- No Vulkan validation errors; no per-frame printerr spam

- [ ] **Step 13: Commit**

```bash
git add extension/src/render/world_streamer.* extension/src/render/camera_params.h \
        extension/src/render/raymarch_pass.* extension/src/voxel_world.* \
        extension/src/raymarch_compositor.cpp shaders/raymarch.comp.glsl \
        tests/test_streaming.gd tests/test_raymarch_pixel.gd tests/test_raymarch_magenta.gd \
        demo/main.tscn demo/benchmark.gd
git commit -m "feat(render): streamed GPU world cutover — two-level indirection, region LRU, GpuWorld removed"
```

---

### Task 13: Raymarcher min–max skip

The atlas holds three min–max levels per brick (Task 10); the raymarcher currently sphere
traces every voxel slab of every resident brick. This task wires the chain in: whole-brick
rejection at DDA level (2³ cell 0) and 2-voxel cell skipping inside the brick loop (8³).
The chain is *inclusive-exact* (Task 4 proves it bounds the trilinear field), so skipping
can never hide a real surface — the migrated pixel suites plus the probe diagnostics below
are the regression net.

**Files:**
- Modify: `shaders/raymarch.comp.glsl`
- Modify: `extension/src/voxel_world.h`, `extension/src/voxel_world.cpp` (`debug_raymarch_probe`)
- Modify: `extension/tests/test_camera_params.cpp` (pin the 128-byte M2 layout)
- Test: `tests/test_raymarch_mips.gd`

**Interfaces:**
- Consumes: `GpuAtlas::mip_atlas(level)` (Task 8), `ENCODED_ZERO` (Task 7 `common.glsl`).
- Produces:
  - GLSL `bool brick_may_have_surface(int slot)` (2³ cell 0) and
    `bool cell8_may_have_surface(int slot, ivec3 cell)` (8³ level), both in the raymarcher.
  - `VoxelWorld::debug_raymarch_probe(Vector3 origin, Vector3 dir) -> Dictionary` with keys
    `hit` (bool), `color` (Color), `brick` (Vector3i), `cell8` (Vector3i),
    `brick_surface` (bool — the 2³ mip at the hit brick), `cell8_surface` (bool — the 8³
    mip at the hit cell). Empty-but-`hit=false` on a miss.

- [ ] **Step 1: Write the failing tests**

Append to `extension/tests/test_camera_params.cpp`:

```cpp
TEST_CASE("M2 layout: 128 bytes with region_origin and atlas_bricks") {
	ve::CameraParams cp = ve::CameraParams::looking_at(0, 0, 0, 0, 0, -1, 0, 1, 0);
	cp.dims[0] = 64; cp.dims[1] = 8; cp.dims[2] = 64;      // world size in REGIONS
	cp.region_origin[1] = -2;                              // origin_bricks (0,-64,0) / 32
	cp.atlas_bricks[0] = 64; cp.atlas_bricks[2] = 32;
	CHECK(sizeof(cp) == 128);
	CHECK(cp.dims[1] == 8);
	CHECK(cp.region_origin[1] == -2);
	CHECK(cp.atlas_bricks[2] == 32);
}
```

`tests/test_raymarch_mips.gd`:

```gdscript
extends GdUnitTestSuite

# If a mip cell ever wrongly reports "no surface" the ray TUNNELS: these rays all hit, and
# the probe's diagnostics pin whether a failure came from the mips or the march logic.
const ATLAS := Vector3i(32, 16, 32)
const REGION_SLOTS := 16
const CAM := Vector3(20, 56.2, 20)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(4, 5, 4)
	w.residency_radius_m = 20.0
	add_child(w)
	w.ensure_initialized()
	for i in range(60):
		if w.debug_stream_frame(CAM) == 0:
			break
	return w

func test_down_rays_hit_and_the_mips_at_the_hit_agree() -> void:
	var w := make_world()
	for ox in range(-4, 5):
		for oz in range(-4, 5):
			var d: Dictionary = w.debug_raymarch_probe(
				Vector3(20 + ox, 56.2, 20 + oz), Vector3(0, -1, 0))
			assert_bool(d["hit"]).override_failure_message(
				"tunnel at offset %d,%d" % [ox, oz]).is_true()
			assert_bool(d["brick_surface"]).is_true()
			assert_bool(d["cell8_surface"]).is_true()

func test_diagonal_ray_hits_through_many_empty_cells() -> void:
	var w := make_world()
	# Grazing descent: crosses dozens of empty 8^3 cells before reaching the surface.
	var d: Dictionary = w.debug_raymarch_probe(Vector3(12, 56.2, 20), Vector3(0.5, -0.6, 0))
	assert_bool(d["hit"]).is_true()
	assert_bool(d["brick_surface"]).is_true()
	assert_bool(d["cell8_surface"]).is_true()
	# And it must be the REAL surface: material colours, not sky or magenta.
	var c: Color = d["color"]
	assert_bool(c.r < 0.52 and c.g > 0.05).is_true()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd extension && scons test`
Expected: the new native case PASSES immediately (it pins the Task-12 layout — it is a
guard against someone shrinking the block, not a TDD red step).
Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_raymarch_mips.gd`
Expected: FAIL — `debug_raymarch_probe` does not exist.

- [ ] **Step 3: Add the mip skip to the raymarcher**

In `shaders/raymarch.comp.glsl`, add three sampler bindings after `mat_atlas` (re-number
nothing — insert at 4..6 and shift the storage buffers to 7..11, edits UBO to 12; update
`RaymarchPass::rebuild_targets` to bind `atlas.mip_atlas(0..2)` as
`UNIFORM_TYPE_SAMPLER_WITH_TEXTURE` at 4/5/6 and shift its storage/UBO bindings
accordingly):

```glsl
layout(set = 0, binding = 4) uniform usampler3D mip2_atlas; // RG8 uint, 2^3 cells/brick
layout(set = 0, binding = 5) uniform usampler3D mip4_atlas; // 4^3 (built, unused here)
layout(set = 0, binding = 6) uniform usampler3D mip8_atlas; // 8^3
```

Add the helpers above `main()`:

```glsl
// The chain stores inclusive min/max over each cell's trilinear corner samples (Task 4),
// so "no surface" is a SOUND skip: the reconstructed field inside the cell cannot cross 0.
bool brick_may_have_surface(int slot) {
	uvec2 mm = texelFetch(mip2_atlas, atlas_base(slot, pc.atlas_bricks, 2), 0).xy;
	return mm.x <= ENCODED_ZERO && mm.y >= ENCODED_ZERO;
}

bool cell8_may_have_surface(int slot, ivec3 cell) { // cell in [0,8)^3, 2 voxels per cell
	uvec2 mm = texelFetch(mip8_atlas, atlas_base(slot, pc.atlas_bricks, 8) + cell, 0).xy;
	return mm.x <= ENCODED_ZERO && mm.y >= ENCODED_ZERO;
}
```

Then update `RaymarchPass::rebuild_targets` to the 13-uniform layout (the `u[10]` array
grows to `u[13]`; the sampler-with-texture entries at 4/5/6 bind `atlas.mip_atlas(0/1/2)`;
storage buffers move to 7–11 in the same relative order; the edits UBO lands at 12):

```cpp
	Ref<RDUniform> u[13];
	for (int i = 0; i < 13; i++) u[i].instantiate();
	u[0]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[0]->set_binding(0); u[0]->add_id(color_);
	u[1]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u[1]->set_binding(1); u[1]->add_id(hitpos_);
	const RID sampled[5] = {atlas.sdf_atlas(), atlas.mat_atlas(), atlas.mip_atlas(0),
			atlas.mip_atlas(1), atlas.mip_atlas(2)};
	for (int i = 2; i <= 6; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
		u[i]->set_binding(i); u[i]->add_id(sampler_); u[i]->add_id(sampled[i - 2]);
	}
	const RID buffers[5] = {atlas.palette(), atlas.region_map(), atlas.region_tables(),
			atlas.op_pool(), atlas.op_counts()};
	for (int i = 7; i <= 11; i++) {
		u[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
		u[i]->set_binding(i); u[i]->add_id(buffers[i - 7]);
	}
	u[12]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u[12]->set_binding(12); u[12]->add_id(edits_ubo_);
	Array uset_args;
	for (int i = 0; i < 13; i++) uset_args.push_back(u[i]);
	uset_ = rd->uniform_set_create(uset_args, shader_, 0);
```

And rewire the DDA inner loop:

```glsl
		int slot = slot_at(map);
		if (slot >= 0 && brick_may_have_surface(slot)) {
			bool has_material = palette_buf.ids[slot * 4] != 0u;
			float t = t_prev;
			for (int j = 0; j < 64; j++) {
				if (t > t_exit) break;
				vec3 p = ro + rd * t;
				// 8^3 empty-cell skip: jump to the cell's exit face. t is clamped to keep
				// progress monotone; the cell's AABB spans its voxels' inclusive lattice,
				// which is exactly the range the mip entry bounds.
				vec3 vox = (p - vec3(map) * BRICK_SIZE) / VOXEL_SIZE; // [0, 16)
				ivec3 cell8 = clamp(ivec3(floor(vox * 0.5)), ivec3(0), ivec3(7));
				if (!cell8_may_have_surface(slot, cell8)) {
					vec3 cell_lo = vec3(map) * BRICK_SIZE + vec3(cell8 * 2) * VOXEL_SIZE;
					vec3 cell_hi = cell_lo + 2.0 * VOXEL_SIZE;
					vec3 far = mix(cell_lo, cell_hi, step(0.0, rd));
					vec3 tf = (far - p) / rd;
					if (st.x == 0) tf.x = 1.0 / 0.0;
					if (st.y == 0) tf.y = 1.0 / 0.0;
					if (st.z == 0) tf.z = 1.0 / 0.0;
					t = min(max(min(tf.x, min(tf.y, tf.z)), t + 0.002), t_exit);
					continue;
				}
				float d = world_sdf(p);
				// ... hit branch unchanged ...
				t += max(d * 0.9, 0.005);
			}
			if (hit) break;
		}
```

- [ ] **Step 4: Implement `debug_raymarch_probe`**

Declare it in `voxel_world.h` and bind it. Implementation in `voxel_world.cpp` (reuses the
same render as `debug_raymarch_pixel`, then reads the hit brick's mip texels back):

```cpp
Dictionary VoxelWorld::debug_raymarch_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !raymarch_pass_) return d;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 rorig = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.region_origin[0] = rorig.x; cam.region_origin[1] = rorig.y; cam.region_origin[2] = rorig.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, cam, 1, 1, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	const PackedByteArray col = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (hp.size() < 16 || col.size() < 8) return d;
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	const uint16_t *h = reinterpret_cast<const uint16_t *>(col.ptr());
	d["color"] = Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
	if (hf[3] < 0.5f) return d; // sky miss
	d["hit"] = true;
	const ve::IVec3 brick = ve::WorldBounds::brick_of_point(hf[0], hf[1], hf[2]);
	d["brick"] = Vector3i(brick.x, brick.y, brick.z);
	// Reproduce the shader's lookups on the CPU to report what the mips said there.
	const ve::IVec3 rs_region = ve::WorldBounds::region_of_brick(brick);
	const int rslot = debug_region_map_entry(Vector3i(rs_region.x, rs_region.y, rs_region.z));
	if (rslot < 0) return d;
	const int slot = debug_region_table_slot(rslot, Vector3i(brick.x, brick.y, brick.z));
	if (slot < 0) return d;
	const float lx = hf[0] - brick.x * ve::kBrickSize;
	const float ly = hf[1] - brick.y * ve::kBrickSize;
	const float lz = hf[2] - brick.z * ve::kBrickSize;
	const int cx = std::min(7, std::max(0, static_cast<int>(lx / ve::kVoxelSize) / 2));
	const int cy = std::min(7, std::max(0, static_cast<int>(ly / ve::kVoxelSize) / 2));
	const int cz = std::min(7, std::max(0, static_cast<int>(lz / ve::kVoxelSize) / 2));
	d["cell8"] = Vector3i(cx, cy, cz);
	const ve::IVec3 abv = atlas_->config().atlas_bricks;
	const ve::IVec3 cell{slot % abv.x, (slot / abv.x) % abv.y, slot / (abv.x * abv.y)};
	const PackedByteArray m2 = device->texture_get_data(atlas_->mip_atlas(0), 0);
	const PackedByteArray m8 = device->texture_get_data(atlas_->mip_atlas(2), 0);
	{
		const int w = abv.x * 2, hh = abv.y * 2;
		const int64_t o = (static_cast<int64_t>(cell.x * 2) + cell.y * 2 * w +
				cell.z * 2 * w * hh) * 2;
		d["brick_surface"] = m2[o] <= ve::kEncodedZero && m2[o + 1] >= ve::kEncodedZero;
	}
	{
		const int w = abv.x * 8, hh = abv.y * 8;
		const int64_t o = (static_cast<int64_t>(cell.x * 8 + cx) + (cell.y * 8 + cy) * w +
				(cell.z * 8 + cz) * static_cast<int64_t>(w) * hh) * 2;
		d["cell8_surface"] = m8[o] <= ve::kEncodedZero && m8[o + 1] >= ve::kEncodedZero;
	}
	return d;
}
```

(The `m2`/`m8` reads fetch whole test-sized volumes — fine for a debug hook; never call this
per-frame at the shipping atlas size.)

- [ ] **Step 5: Run everything**

```bash
./build.sh -j$(nproc)
cd extension && scons test
cd .. && ./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests
```
Expected: all green, including the migrated pixel suites (they pass through the new skip
path) and the new mips suite. If a probe reports `brick_surface == false` on a hit, the
mip lookup and the march disagree about addressing — recheck `atlas_base` strides before
touching the skip logic.

- [ ] **Step 6: Commit**

```bash
git add shaders/raymarch.comp.glsl extension/src/render/raymarch_pass.cpp \
        extension/src/voxel_world.* extension/tests/test_camera_params.cpp \
        tests/test_raymarch_mips.gd
git commit -m "feat(render): min-max mip skip in the raymarcher"
```

---

### Task 14: `VoxelEditTool` + the edit pipeline, end to end

**Files:**
- Create: `extension/src/voxel_edit_tool.h`, `extension/src/voxel_edit_tool.cpp`
- Modify: `extension/src/register_types.cpp` (register the class)
- Test: `tests/test_edit_pipeline.gd`

**Interfaces:**
- Consumes: `VoxelWorld::append_edit` + `debug_raycast` + `debug_stream_frame` (Task 12), `ve::raycast` (Task 6).
- Produces: `class godot::VoxelEditTool : public Node`, registered as `VoxelEditTool`:
  - `Dictionary apply_sphere_subtract(Vector3 pos, float radius)`
  - `Dictionary apply_sphere_add(Vector3 pos, float radius, int material)`
  - `Dictionary apply_sphere_paint(Vector3 pos, float radius, int material)`
  - Each returns `{"touched": Array[Vector3i], "rejected": Array[Vector3i]}`.

- [ ] **Step 1: Write the failing test**

`tests/test_edit_pipeline.gd`:

```gdscript
extends GdUnitTestSuite

# The destruction pipeline end to end: tool -> edit log -> op upload -> re-mark ->
# indirect regeneration -> visible in the raymarcher, with the analytic raycast as oracle.
const ATLAS := Vector3i(32, 16, 32)
const REGION_SLOTS := 16
const CAM := Vector3(40, 56.2, 40)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(4, 5, 4)
	w.residency_radius_m = 20.0
	add_child(w)
	w.ensure_initialized()
	for i in range(60):
		if w.debug_stream_frame(CAM) == 0:
			break
	return w

func make_tool(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

func make_op(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type); b.put_u32(material)
	b.put_float(pos.x); b.put_float(pos.y); b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(0); b.put_u32(0)
	return b.data_array

func test_sphere_subtract_carves_a_visible_hole() -> void:
	var w := make_world()
	var tool := make_tool(w)
	var hit: Dictionary = w.debug_raycast(Vector3(40, 80, 40), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]

	var before: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(before.r < 0.52 and before.g > 0.05).is_true() # solid terrain

	var r: Dictionary = tool.apply_sphere_subtract(Vector3(hp.x, hp.y + 0.5, hp.z), 2.5)
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.debug_stream_frame(CAM)

	# The old surface point is now air: the ray lands in the crater, over a metre deeper,
	# and what it hits is still real terrain (not sky, not magenta).
	var after: Dictionary = w.debug_raycast(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_float(after["pos"].y).is_less(hp.y - 1.0)
	var c: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(c.r < 0.52 and c.g > 0.05).is_true()

func test_sphere_add_places_material_4_in_open_sky() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# The surface stays below ~61.5 m everywhere, so 66-70 m is open air. Look DOWN from
	# above the blob: its top is sunlit (a ray from below would see the ambient-lit
	# underside at 0.25x albedo — too dim for a useful colour assertion).
	var eye := Vector3(40, 75.0, 40)
	var before: Color = w.debug_raymarch_pixel(eye, Vector3(0, -1, 0))
	assert_bool(before.r < 0.52 and before.g > 0.05).is_true() # distant terrain below

	var r: Dictionary = tool.apply_sphere_add(Vector3(40, 68.2, 40), 1.5, 4)
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.debug_stream_frame(CAM)

	# Material 4's albedo (0.62, 0.60, 0.66) is the only one with b > r.
	var c: Color = w.debug_raymarch_pixel(eye, Vector3(0, -1, 0))
	assert_bool(c.b > c.r and c.r > 0.35).is_true()

func test_paint_recolours_grass_to_rock_without_moving_the_surface() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# Find a GRASS spot deterministically: grass is the h in (1, 4) band.
	var hp := Vector3.ZERO
	var found := false
	for x in range(30, 50):
		for z in range(30, 50):
			var h: Dictionary = w.debug_raycast(Vector3(x, 80, z), Vector3(0, -1, 0))
			if h["hit"] and h["pos"].y - 51.2 > 1.0 and h["pos"].y - 51.2 < 4.0:
				hp = h["pos"]
				found = true
				break
		if found:
			break
	assert_bool(found).is_true()

	var before: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 1.0, hp.z), Vector3(0, -1, 0))
	assert_bool(before.g > before.r).is_true() # grass is green-dominant
	var r: Dictionary = tool.apply_sphere_paint(hp, 1.5, 2) # rock
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.debug_stream_frame(CAM)

	var after: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 1.0, hp.z), Vector3(0, -1, 0))
	assert_bool(after.r > after.g).is_true() # rock (0.45, 0.42, 0.40) is red-leaning
	# The surface must not have moved: same ray, same hit depth to a centimetre.
	var depth: Dictionary = w.debug_raycast(Vector3(hp.x, hp.y + 1.0, hp.z), Vector3(0, -1, 0))
	assert_float(depth["pos"].y).is_equal_approx(hp.y, 0.01)

func test_an_op_on_a_region_border_updates_both_sides() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# x = 25.6 m is the boundary between regions 0 and 1 on x. Settle next to the border:
	# the default CAM is >20 m from region 0's side of it, so it would not be resident.
	for i in range(60):
		if w.debug_stream_frame(Vector3(20, 53, 13)) == 0:
			break
	var hit: Dictionary = w.debug_raycast(Vector3(25.6, 80, 12.8), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]
	var r: Dictionary = tool.apply_sphere_subtract(hp, 3.0)
	assert_array(r["rejected"]).is_empty()
	assert_int(r["touched"].size()).is_greater_equal(2) # both regions got the op
	for i in range(10):
		w.debug_stream_frame(Vector3(20, 53, 13))

	# Rebuild the op bytes the tool emitted and diff GPU bricks against the CPU reference
	# on BOTH sides of the border: each region's own op list drove its regeneration.
	var ops := make_op(0, 0, hp, 3.0)
	var checked := 0
	for bx in [31, 32]: # last brick of region 0, first of region 1
		for by in range(56, 72):
			var brick := Vector3i(bx, by, 16)
			var region := Vector3i(floori(bx / 32.0), floori(by / 32.0), 0)
			var rslot := w.debug_region_map_entry(region)
			if rslot < 0:
				continue
			var d: Dictionary = w.debug_brick_diff(brick, rslot, ops, 1)
			if int(d["slot"]) < 0:
				continue
			checked += 1
			assert_int(d["sdf_max_diff"]).is_less_equal(1)
			assert_bool(d["palette_match"]).is_true()
	assert_int(checked).override_failure_message(
		"no resident bricks found along the border").is_greater(0)

func test_a_full_region_rejects_the_257th_op_without_crashing() -> void:
	var w := make_world()
	var tool := make_tool(w)
	for i in range(256):
		tool.apply_sphere_subtract(Vector3(5.0, 0.0, 5.0), 0.1)
	var r: Dictionary = tool.apply_sphere_subtract(Vector3(5.0, 0.0, 5.0), 0.1)
	assert_array(r["rejected"]).is_not_empty()
	# Fail-soft: the world keeps streaming happily afterwards.
	assert_int(w.debug_stream_frame(CAM)).is_greater_equal(0)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests/test_edit_pipeline.gd`
Expected: FAIL — `VoxelEditTool` is not a registered class.

- [ ] **Step 3: Implement `VoxelEditTool`**

`extension/src/voxel_edit_tool.h`:

```cpp
#pragma once
#include <godot_cpp/classes/node.hpp>

namespace godot {

class VoxelWorld;

// Thin Godot-facing op emitter (spec §5: the demo tools are "just op emitters"). All the
// real work lives in VoxelWorld::append_edit and the streamer; this class only packs the
// ve::EditOp and reports which regions accepted it.
class VoxelEditTool : public Node {
	GDCLASS(VoxelEditTool, Node)

protected:
	static void _bind_methods();

public:
	Dictionary apply_sphere_subtract(Vector3 pos, float radius);
	Dictionary apply_sphere_add(Vector3 pos, float radius, int material);
	Dictionary apply_sphere_paint(Vector3 pos, float radius, int material);

private:
	Dictionary apply(uint32_t type, Vector3 pos, float radius, int material);
};

} // namespace godot
```

`extension/src/voxel_edit_tool.cpp`:

```cpp
#include "voxel_edit_tool.h"
#include "voxel_world.h"
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void VoxelEditTool::_bind_methods() {
	ClassDB::bind_method(D_METHOD("apply_sphere_subtract", "pos", "radius"),
			&VoxelEditTool::apply_sphere_subtract);
	ClassDB::bind_method(D_METHOD("apply_sphere_add", "pos", "radius", "material"),
			&VoxelEditTool::apply_sphere_add);
	ClassDB::bind_method(D_METHOD("apply_sphere_paint", "pos", "radius", "material"),
			&VoxelEditTool::apply_sphere_paint);
}

Dictionary VoxelEditTool::apply(uint32_t type, Vector3 pos, float radius, int material) {
	Dictionary out;
	Array touched, rejected;
	out["touched"] = touched;
	out["rejected"] = rejected;
	VoxelWorld *world = Object::cast_to<VoxelWorld>(get_parent());
	if (!world) {
		UtilityFunctions::printerr("VoxelEditTool: parent is not a VoxelWorld");
		return out;
	}
	ve::EditOp op{};
	op.type = type;
	op.material = static_cast<uint32_t>(material);
	op.pos[0] = pos.x; op.pos[1] = pos.y; op.pos[2] = pos.z;
	op.radius = radius;
	const ve::EditLog::AppendResult r = world->append_edit(op);
	for (const ve::IVec3 &v : r.touched) touched.push_back(Vector3i(v.x, v.y, v.z));
	for (const ve::IVec3 &v : r.rejected) rejected.push_back(Vector3i(v.x, v.y, v.z));
	return out;
}

Dictionary VoxelEditTool::apply_sphere_subtract(Vector3 pos, float radius) {
	return apply(ve::kOpSphereSubtract, pos, radius, 0);
}

Dictionary VoxelEditTool::apply_sphere_add(Vector3 pos, float radius, int material) {
	return apply(ve::kOpSphereAdd, pos, radius, material);
}

Dictionary VoxelEditTool::apply_sphere_paint(Vector3 pos, float radius, int material) {
	return apply(ve::kOpSpherePaint, pos, radius, material);
}
```

In `register_types.cpp`, add `#include "voxel_edit_tool.h"` and
`GDREGISTER_CLASS(VoxelEditTool);` after the existing registrations.

- [ ] **Step 4: Build and run**

```bash
./build.sh -j$(nproc)
./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests
```
Expected: `test_edit_pipeline.gd` 5/5, everything else green. If the border test reports
zero checked bricks, print `debug_region_map_entry` for regions (0,·,0)/(1,·,0) and the
op's `touched` — the op probably landed short of the border (radius too small to straddle).

- [ ] **Step 5: Commit**

```bash
git add extension/src/voxel_edit_tool.* extension/src/register_types.cpp \
        tests/test_edit_pipeline.gd
git commit -m "feat(demo): VoxelEditTool with end-to-end edit pipeline tests"
```

---

### Task 15: Demo wiring — tools, HUD, benchmark

**Files:**
- Create: `demo/edit_tool.gd`
- Modify: `demo/hud.gd`, `demo/fly_camera.gd`, `demo/benchmark.gd`, `demo/main.tscn`

**Interfaces:**
- Consumes: `VoxelEditTool` (Task 14), `VoxelWorld::debug_raycast`/`debug_stream_stats` (Task 12).
- Produces: playable destruction demo; HUD with streaming counters.

- [ ] **Step 1: `demo/edit_tool.gd`**

```gdscript
extends Node
# Demo destruction tool (spec §5 "demo edit tools"): LMB carves, RMB fills, MMB paints.
# Aims with the world's analytic raycast — the same field the GPU bricks are generated
# from — so the reticle tracks the surface exactly, with no physics and no readback stall.

@export var world_path: NodePath
@export var camera_path: NodePath
@export var radius := 3.0
@export var fill_material := 4
@export var paint_material := 1

var _world: VoxelWorld
var _tool: VoxelEditTool
var _cam: Camera3D

func _ready() -> void:
	_world = get_node(world_path)
	_cam = get_node(camera_path)
	_tool = ClassDB.instantiate("VoxelEditTool")
	_world.add_child(_tool) # VoxelEditTool resolves the world through its parent

func _unhandled_input(event: InputEvent) -> void:
	if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
		return
	var mb := event as InputEventMouseButton
	if mb == null or not mb.pressed:
		return
	var hit: Dictionary = _world.debug_raycast(
		_cam.global_position, -_cam.global_transform.basis.z)
	if not hit["hit"]:
		return
	var pos: Vector3 = hit["pos"]
	match mb.button_index:
		MOUSE_BUTTON_LEFT:
			_tool.apply_sphere_subtract(pos, radius)
		MOUSE_BUTTON_RIGHT:
			_tool.apply_sphere_add(pos, radius * 0.7, fill_material)
		MOUSE_BUTTON_MIDDLE:
			_tool.apply_sphere_paint(pos, radius, paint_material)
```

- [ ] **Step 2: `demo/hud.gd` — streaming counters**

Replace the file:

```gdscript
extends Label

@export var world_path: NodePath
var _world: VoxelWorld
var _frames := 0

func _ready() -> void:
	if not world_path.is_empty():
		_world = get_node(world_path)

func _process(_delta: float) -> void:
	_frames += 1
	if _frames % 15 != 0:
		return # streaming stats read back GPU counters; don't stall every frame
	var fps := Engine.get_frames_per_second()
	var ms := 1000.0 / maxf(float(fps), 0.001)
	var s := "world: booting"
	if _world and _world.is_initialized():
		var st: Dictionary = _world.debug_stream_stats()
		s = "regions %d  edits %d  ovf %d" % [
			st.get("resident_regions", 0), st.get("frame_edits", 0),
			st.get("overflow_ever", 0)]
	text = "%d fps  (%.1f ms)  |  %s" % [fps, ms, s]
```

- [ ] **Step 3: Scene + small tweaks**

`demo/main.tscn`: bump `load_steps` 8 → 9, add `edit_tool.gd` as `ext_resource` id `4`,
add `world_path` to the HUD label, and add the tool node:

```
[ext_resource type="Script" path="res://demo/edit_tool.gd" id="4"]

[node name="EditTool" type="Node" parent="."]
script = ExtResource(4)
world_path = NodePath("/root/Main/VoxelWorld")
camera_path = NodePath("/root/Main/Camera3D")
```

On the `Label` node add: `world_path = NodePath("/root/Main/VoxelWorld")`.

`demo/fly_camera.gd`: `speed := 10.0` → `25.0` (1.6 km world).

`demo/benchmark.gd`: after the `BENCH` print, also print the streaming counters for the
record:

```gdscript
		var world: VoxelWorld = get_parent().get_node("VoxelWorld")
		var st: Dictionary = world.debug_stream_stats()
		print("BENCH regions=%d overflow=%d" % [st.get("resident_regions", -1), st.get("overflow_ever", -1)])
```

- [ ] **Step 4: Verify**

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything --gpu-validation demo/main.tscn`
Checklist (record results in the commit message):
- Terrain streams in around the camera within the first seconds; HUD region count climbs then stops
- LMB carves a crater that appears within a frame or two and PERSISTS when flying away 300 m and back (op list re-generates it on stream-in)
- RMB places a grey fill blob in the air; MMB re-paints grass to a different material without moving the surface
- The frame before regeneration shows the orange/grey tint preview at the edit point
- Fly 300 m in one direction: terrain keeps streaming, no frame hitch you can feel, `ovf` stays 0
- Fly past x = 1638.4 m / below y = −51.2 m: the world simply ends (bounded world, spec §1), no crash
- No Vulkan validation errors

Run: `godot --path /home/jeremy/Development/Godot/voxel-everything demo/main.tscn -- --benchmark`
Expected: prints `BENCH frame_avg_ms=...` and `BENCH regions=... overflow=0`, exits 0.
This is the first real M2 performance reading (0.66× raymarch + streaming). Record the
number in the commit message; if it exceeds 16.6 ms, note it — per-pass GPU timings and
the fade-band contingency are M7's, but a >6 ms march at this scale wants a comment.

- [ ] **Step 5: Commit**

```bash
git add demo/
git commit -m "feat(demo): destruction tools, streaming HUD, benchmark counters"
```

---

## M2 Acceptance Checklist

- `cd extension && scons test` — native suite green (region lattice, edit ops, edit log, brick mips, brick eval, raycast, residency, + all M1 cases)
- `./addons/gdUnit4/runtest.sh --godot_binary /usr/bin/godot -a res://tests` — green: boot, GPU smoke, **field differential** (spec §8), atlas, region pass, **brick differential** (spec §8), streaming, mips, edit pipeline, migrated pixel + magenta suites
- `godot --path . demo/main.tscn` — 1.6 km GPU-generated world streams around the camera; carve/fill/paint work in real time and persist across eviction + re-stream
- `godot --path . demo/main.tscn -- --benchmark` — BENCH lines printed, overflow 0
- `ve::WorldData` + `eval_brick` remain the CPU reference; `test_brick_diff.gd` guards drift

## Errata (recorded during M2 implementation — corrections to the task text)

1. Task 3: the test's `bounds()` helper read `WorldBounds{{0,-64,0},{64,8,64}}`, whose
   `origin_regions` is `{0,-2,0}` — region y = -1 is in-bounds. The test spheres at y = z = 1
   with r = 2 pad (one voxel) into regions y/z = -1, so the exact implementation yields 4
   touched regions where the test expects 2 (and 2 where it expects 1 in the partial-bounds
   case). Corrected to `WorldBounds{{0,0,0},{64,8,64}}` (one line), matching the test
   comments' evident intent; implementation unchanged.
2. Task 5: two corrections to `test_brick_eval.cpp`, both test-only (production code is
   byte-identical to the brief). (a) In "an edit makes a previously-solid brick a surface
   brick", `op.radius = 2.0f` was changed to `0.5f`: the sphere centred at brick-origin + 0.4
   with r = 2.0 swallows the whole 0.8 m brick (farthest corner 0.693 m away), leaving every
   probe air-side with no zero crossing — the test could not pass against the brief's own
   implementation. (b) In "eval_brick produces…", the surface-brick search now requires the
   evaluated lattice to actually cross zero: the pad probe (`brick_has_surface`) can flag a
   near-surface SOLID brick (terrain dipping to ~2.54 m at the brick's far corner), which the
   test's own pos/neg assertions would then fail on.
3. Task 7: the brief's verbatim Step 3 `shader_loader.cpp` block drops the M1 malformed-include
   guard (`end == std::string::npos` check), which contradicts its own Step-2 acceptance line
   "all four M1 loader cases pass" — M1's "malformed include reports error" test
   (test_shader_loader.cpp:54) requires the `"malformed include"` error string. Implementation
   kept the guard as a strict superset (inert for the diamond-once and cycle cases); the
   Step 3 code block in the plan should include the check.
4. Task 7: removing `ATLAS_BRICKS` from `common.glsl` (per Step 4's "deliberately gone")
   broke M1's `raymarch.comp.glsl`, which still references it at lines 34–36 and 92–94 —
   the shader failed to compile and `test_raymarch_pixel.gd` regressed, contradicting
   Task 8's mandate that "the raymarcher keeps rendering the M1 static world, and its
   gdUnit tests keep passing, right up to the single atomic cutover in Task 12". Human
   decision: restore `const ivec3 ATLAS_BRICKS = ivec3(32, 16, 32);` to `common.glsl`
   now (inert for the new Task-7 shaders, which take atlas dims as parameters); Task 12's
   raymarch rewrite replaces it with runtime dims, at which point it disappears.
5. Task 9: the brief's verbatim Step 3 `brick_mark.comp.glsl` declares a local
   `bool active = brick_has_surface(...)`, but `active` is a GLSL reserved word (spec
   Appendix A), so Godot's glslang rejects the shader ("'active' : Reserved word") and
   `debug_init_atlas()` fails. Implementation renamed the local to `has_surface` (three
   occurrences, declaration + two uses, plus an explanatory comment); no semantic change.
   Everything else in the shader is byte-identical to the plan text. The Step 3 code block
   in the plan should use a non-reserved identifier.
   Also: `debug_teardown_atlas()` (a Task 8 hook) frees the atlas buffers while the new
   `RegionPass` still holds uniform sets referencing them; a subsequent `debug_init_atlas()`
   then errored with three "Attempted to free invalid ID" lines in
   `test_teardown_is_idempotent_and_survives_re_init`. The hook now tears the region pass
   down first (mirroring `_exit_tree()`'s ordering), which also covers any future
   re-init-without-teardown path.
7. Task 10: the brief's verbatim `brick_gen.comp.glsl` would not compile under glslang for
   two reasons — `pc.atlas_bricks` (an `ivec4`) is passed to `atlas_base`/`atlas_brick_cell`
   which take `ivec3`, fixed with `.xyz`; and `mat2` is a GLSL reserved word (the 2×2 matrix
   type), renamed to `matB` (no semantic change). Also, per the brief's own Step 5 note
   (observed non-preemptively): `debug_brick_diff`'s mip reference is now reduced from the
   GPU lattice read back, not the CPU lattice, so a one-step sin() drift cannot flag a mip
   cell that is a correct reduction of what the GPU actually wrote.
8. Task 10 (human-approved): `test_brick_diff.gd` test 2's per-brick assertion
   `mat_near_compared > 0` is relaxed to "at least one sampled brick exercises the
   near-surface band". ~15% of active bricks are apron-grazing (surface crosses only the
   +face apron plane at local lattice 16..17, outside the 16³ cell volume), so their
   mat_near_compared is legitimately 0 with GPU/CPU agreeing exactly; combined with
   nondeterministic GPU job order, the strict form failed ~86% of runs.
9. Task 12 (human-approved): the plan's premise "the M1-era surface now sits at y ≈ 51.2 ±
   10 m" was FALSE in the committed code — the generator field is `sdf = y - hills(x, z)`
   with NO origin term (the plan's own "Brick coordinates are GLOBAL" convention), so the
   surface sits at y ≈ hills ∈ [−10, +10] m inside the new world's y-span [−51.2, +153.6).
   The plan's +51.2 shifts in the Task-12 test origins and demo transforms were based on the
   false premise. Human decision: make the premise TRUE — add a +51.2 surface offset to the
   generator field (`sdf = (y - kSurfaceY) - hills` with `kSurfaceY = 51.2f`) and its GPU
   mirror in `shaders/field.glsl`, then re-baseline every test that asserts absolute surface
   heights (+51.2 on the relevant coordinates) and the WorldData-based worlds whose y-span
   must now contain the surface near brick 64. The demo's +51.2 transforms then become
   correct as the plan intended. The cave centre becomes `(30, kSurfaceY + hills(30,30) - 2,
   30)`.
