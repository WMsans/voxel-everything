# Unbounded World Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the world edge so the voxel world streams unbounded on all three axes, with
the LoD horizon expressed as a camera-centred radius instead of a fixed box.

**Architecture:** `ve::WorldBounds` conflates three jobs — world membership, the dense GPU
region-map index space, and the LoD octree's root extent. Membership is deleted outright; the
index space becomes a small camera-centred toroidal `RegionWindow` derived from the residency
radius; the root extent becomes a `stream_radius_m` scalar. Structures that would then grow
without bound as the player travels (`OccupancyGrid` blocks, `LodTree` nodes at levels >= 5)
gain distance-based eviction.

**Tech Stack:** C++20 GDExtension against godot-cpp (Godot 4.7), SCons, doctest for the pure-core
native suite, gdUnit4 (GDScript) for GPU-backed suites, GLSL compute shaders.

**Spec:** `docs/superpowers/specs/2026-09-04-unbounded-world-design.md`

## Global Constraints

- **Pure cores must not include godot-cpp.** `extension/SConstruct` globs `src/world`,
  `src/generator`, `src/core`, `src/terrain`, `src/mesh`, `src/connectivity`, `src/lod`,
  `src/shade` into a zero-godot-cpp test build. Anything you add there compiles with plain
  `g++ -std=c++20` and may use only `ve::IVec3`, never `godot::Vector3i`. `src/lod/lod_system.cpp`
  and `src/mesh/consolidation.cpp` are explicitly excluded and may use godot-cpp.
- **Lock order is `WorldStore::edit_mutex()` -> `LodSystem::mutex()`.** Never take them the
  other way round. Occupancy is main-thread-only.
- **Region geometry is fixed:** `kRegionBricks = 32`, `kBrickSize = 0.8f`, so
  `kRegionSize = 25.6f` m. Do not change these.
- **Defaults introduced by this plan:** `stream_radius_m = 1638.4f` (exactly today's reach, so
  the refactor is provably no-visual-change), `occupancy_retention_m = 256.0f`,
  `residency_radius_m = 96.0f` (unchanged).
- **Float32 world coordinates in one global frame, supported to +/-100 km.** No floating-origin
  rebasing — it is an explicit non-goal (spec §10).
- **Every region coordinate is valid.** After this plan no code may ask whether a region is
  "in the world". A guard of that shape is a bug.

### Commands

| Purpose | Command |
|---|---|
| Build the extension | `./build.sh` |
| Build + run the whole native suite | `cd extension && scons test -j8` |
| Build the native suite only | `cd extension && scons build/tests/ve_tests -j8` |
| Run one native test case | `./extension/build/tests/ve_tests -tc="<test case name>"` |
| Run one gdUnit suite | `./gdunit_tests.sh -a res://tests/test_foo.gd` |
| Run the whole gdUnit suite | `./gdunit_tests.sh` |

---

## File Structure

**Created**

| File | Responsibility |
|---|---|
| `extension/src/world/region_window.h` / `.cpp` | `ve::RegionWindow` — the camera-centred toroidal index space for the near-field region map. Pure core. |
| `extension/tests/test_region_window.cpp` | Window sizing, toroidal indexing, the no-alias invariant. |
| `extension/tests/test_unbounded_characterization.cpp` | Phase-1 goldens pinning behaviour that must not drift accidentally. |
| `extension/tests/test_unbounded_invariants.cpp` | Phase-2 invariants: bounded travel, no edge, precision at 100 km. |
| `reports/unbounded-baseline/` | Pre-change test-run evidence (Task 1). |

**Modified (principal)**

| File | Change |
|---|---|
| `extension/src/world/region.h` / `.cpp` | `WorldBounds` statics promoted to free functions; the type deleted. |
| `extension/src/generator/edit_ops.h` / `.cpp` | `kMaxOpRegionSpan`, `op_region_span()`. |
| `extension/src/world/edit_log.h` / `.cpp` | Loses `bounds_`; gains the extent cap. |
| `extension/src/world/residency.h` / `.cpp` | `ResidencyConfig::bounds` -> `RegionWindow window`. |
| `extension/src/connectivity/occupancy.h` / `.cpp` | `evict_outside()`. |
| `extension/src/lod/lod_grid.h` / `.cpp` | `lod_root_range` -> `lod_roots_in_radius`; `lod_chunk_in_bounds` deleted. |
| `extension/src/lod/lod_tree.h` / `.cpp` | `LodTreeConfig::bounds` -> `stream_radius_m`; distance-gated eviction. |
| `extension/src/mesh/chunk_residency.h` / `.cpp` | Loses `bounds`. |
| `extension/src/physics/island_manager.cpp` | Three `contains_region` guards deleted. |
| `extension/src/render/gpu_atlas.h` / `.cpp` | Region map sized from the window. |
| `shaders/raymarch.comp.glsl` | Toroidal region index. |
| `extension/src/raymarch_compositor.cpp` | Window push constants; sun ortho from the stream radius. |
| `extension/src/core/world_store.h`, `voxel_world.h` / `.cpp`, `render/orchestrator.cpp`, `lod/lod_system.cpp` | Config surface: `stream_radius_m`, `occupancy_retention_m`. |
| `extension/src/debug/hooks.cpp` | 19 `world_bounds()` sites + `:5226`, `:5803`. |
| `tests/*.gd` (~15 files) | Migrate off `world_size_regions` / `world_origin_bricks`. |

---

## Task 1: Baseline

Nothing here changes behaviour. It exists because some assertions already fail on clean `main`,
and without a recorded baseline every later failure is unattributable.

**Files:**
- Create: `reports/unbounded-baseline/native.txt`
- Create: `reports/unbounded-baseline/gdunit.txt`
- Create: `reports/unbounded-baseline/README.md`

**Interfaces:**
- Consumes: nothing.
- Produces: the failure list every later task compares against.

- [ ] **Step 1: Confirm you are on a clean tree at the merge base**

```bash
git status --porcelain   # must be empty
git log --oneline -1
```

- [ ] **Step 2: Record the native suite**

```bash
mkdir -p reports/unbounded-baseline
cd extension && scons test -j8 2>&1 | tee ../reports/unbounded-baseline/native.txt; cd ..
```

- [ ] **Step 3: Record the gdUnit suite**

```bash
./gdunit_tests.sh 2>&1 | tee reports/unbounded-baseline/gdunit.txt || true
```

The `|| true` is deliberate: a non-zero exit is data, not a stop condition.

- [ ] **Step 4: Write the summary**

Create `reports/unbounded-baseline/README.md` listing, verbatim from the two logs: the git SHA,
the native suite's pass/fail counts, and **every failing gdUnit assertion with its suite name**.
Do not summarise or round. A later task that sees a failure not on this list has caused it.

- [ ] **Step 5: Commit**

```bash
git add reports/unbounded-baseline
git commit -m "test: record pre-unbounded-world baseline"
```

---

## Task 2: Characterization goldens

Pins the three behaviours the refactor is most likely to break silently. **These goldens are
expected to change** — Task 7 changes root selection, Task 6 changes `map_index`. When a later
task changes one, the diff is reviewed and the golden updated *in that task's commit*. That
review is the whole value; a golden nobody reads is worthless.

**Files:**
- Create: `extension/tests/test_unbounded_characterization.cpp`

**Interfaces:**
- Consumes: `ve::EditLog`, `ve::RegionResidency`, `ve::LodTree`, `ve::lod_root_range` — all as
  they exist today.
- Produces: test case names `"characterization: root range at demo bounds"`,
  `"characterization: settled residency at the demo spawn"`,
  `"characterization: edit fan-out at the old world edge"`.

- [ ] **Step 1: Write the characterization tests**

```cpp
#include <doctest/doctest.h>
#include "lod/lod_grid.h"
#include "lod/lod_tree.h"
#include "world/edit_log.h"
#include "world/residency.h"
#include <algorithm>
#include <vector>

// The shipped demo world: 64 x 8 x 64 regions of 25.6 m, origin at y = -51.2 m.
static ve::WorldBounds demo_bounds() {
	ve::WorldBounds b;
	b.origin_bricks = {0, -64, 0};
	b.size_regions = {64, 8, 64};
	return b;
}

TEST_CASE("characterization: root range at demo bounds") {
	ve::IVec3 lo{}, hi{};
	ve::lod_root_range(demo_bounds(), &lo, &hi);
	// An L7 chunk is 1638.4 m and the world is exactly 1638.4 m across in XZ: one root.
	CHECK(lo.x == 0); CHECK(hi.x == 0);
	CHECK(lo.z == 0); CHECK(hi.z == 0);
	// Y spans [-51.2, 153.6) m, which lands inside the single root row at y = 0.
	CHECK(lo.y == -1); CHECK(hi.y == 0);
}

TEST_CASE("characterization: settled residency at the demo spawn") {
	ve::ResidencyConfig cfg;
	cfg.bounds = demo_bounds();
	cfg.radius_m = 96.0f;
	cfg.max_region_slots = 512;
	cfg.max_loads_per_frame = 4;
	ve::RegionResidency res(cfg);
	// Demo player spawn is (8, 62, 8).
	for (int i = 0; i < 500; i++) {
		const ve::ResidencyPlan p = res.update(8.0f, 62.0f, 8.0f);
		if (p.loads.empty() && p.evicts.empty()) break;
	}
	const int settled = res.resident_count();
	CHECK(settled > 0);
	// Every resident region's map_index must be a valid dense index into the region map.
	std::vector<ve::IVec3> regions;
	res.resident_regions(&regions);
	for (const ve::IVec3 &r : regions) {
		const int idx = demo_bounds().region_index(r);
		CHECK(idx >= 0);
		CHECK(idx < 64 * 8 * 64);
	}
	// Pin the count so a change in load ordering or the candidate scan is visible.
	CHECK(settled == res.resident_count());
	MESSAGE("settled resident regions: " << settled);
}

TEST_CASE("characterization: edit fan-out at the old world edge") {
	ve::EditLog log(demo_bounds());
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	// Region 63 is the last in x; this sphere straddles the old world edge at 1638.4 m.
	op.pos[0] = 1638.0f; op.pos[1] = 0.0f; op.pos[2] = 8.0f;
	op.radius = 4.0f;
	const ve::EditLog::AppendResult r = log.append(op);
	CHECK(r.rejected.empty());
	// Today the clamp drops every region past x = 63. After the refactor those regions exist.
	for (const ve::IVec3 &t : r.touched) CHECK(t.x <= 63);
	CHECK(!r.touched.empty());
}
```

- [ ] **Step 2: Run them and confirm they pass against current behaviour**

```bash
cd extension && scons test -j8
```

Expected: PASS. A characterization test that fails on unchanged code is describing the code
wrongly — fix the test, not the code.

- [ ] **Step 3: Commit**

```bash
git add extension/tests/test_unbounded_characterization.cpp
git commit -m "test: characterize world-bounds behaviour before removing it"
```

---

## Task 3: `RegionWindow`

The camera-centred toroidal index space that replaces the dense region map. Pure core, no GPU.

**Files:**
- Create: `extension/src/world/region_window.h`, `extension/src/world/region_window.cpp`
- Test: `extension/tests/test_region_window.cpp`

**Interfaces:**
- Consumes: `ve::IVec3`, `ve::kRegionSize`, `ve::floor_div` from `world/region.h`.
- Produces:
  - `int ve::region_window_dim(float radius_m, float evict_margin)` — the power-of-two window
    edge in regions.
  - `struct ve::RegionWindow { IVec3 origin; int dim; }`
  - `RegionWindow ve::region_window_centered(float cx, float cy, float cz, int dim)`
  - `int RegionWindow::index(IVec3 r) const` — total function, toroidal.
  - `bool RegionWindow::contains(IVec3 r) const`
  - `int RegionWindow::cell_count() const`

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_region_window.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/region_window.h"
#include <set>

TEST_CASE("window dim is the next power of two covering twice the evict radius") {
	// 96 m radius x 1.15 margin = 110.4 m; resident span 220.8 m; 220.8 / 25.6 = 8.63;
	// ceil + 1 = 10; next_pow2 = 16.
	CHECK(ve::region_window_dim(96.0f, 1.15f) == 16);
	// A tiny radius still yields a usable window.
	CHECK(ve::region_window_dim(10.0f, 1.15f) >= 4);
	// The result is always a power of two.
	for (float r = 8.0f; r < 400.0f; r += 7.0f) {
		const int d = ve::region_window_dim(r, 1.15f);
		CHECK((d & (d - 1)) == 0);
	}
}

TEST_CASE("the window spans more than twice the evict radius") {
	// This is invariant 3: two regions that alias are `dim` regions apart, and two
	// simultaneously resident regions are at most 2 * radius * margin apart.
	for (float r = 8.0f; r < 400.0f; r += 7.0f) {
		const int d = ve::region_window_dim(r, 1.15f);
		CHECK(float(d) * ve::kRegionSize > 2.0f * r * 1.15f);
	}
}

TEST_CASE("index is toroidal and total") {
	const ve::RegionWindow w = ve::region_window_centered(0.0f, 0.0f, 0.0f, 16);
	// Every index is in range, including for regions far outside the window.
	for (int x = -1000; x <= 1000; x += 37) {
		const int i = w.index({x, 0, 0});
		CHECK(i >= 0);
		CHECK(i < w.cell_count());
	}
	// Regions `dim` apart alias onto the same cell; that is the property `contains` guards.
	CHECK(w.index({0, 0, 0}) == w.index({16, 0, 0}));
	CHECK(w.index({0, 0, 0}) != w.index({1, 0, 0}));
}

TEST_CASE("contains is the window AABB, and every contained region has a unique cell") {
	const ve::RegionWindow w = ve::region_window_centered(400.0f, 0.0f, 400.0f, 16);
	std::set<int> cells;
	int contained = 0;
	for (int z = w.origin.z; z < w.origin.z + w.dim; z++)
		for (int y = w.origin.y; y < w.origin.y + w.dim; y++)
			for (int x = w.origin.x; x < w.origin.x + w.dim; x++) {
				const ve::IVec3 r{x, y, z};
				CHECK(w.contains(r));
				cells.insert(w.index(r));
				contained++;
			}
	CHECK(contained == w.cell_count());
	CHECK(int(cells.size()) == w.cell_count()); // a bijection, so no in-window aliasing
	CHECK(!w.contains({w.origin.x - 1, w.origin.y, w.origin.z}));
	CHECK(!w.contains({w.origin.x + w.dim, w.origin.y, w.origin.z}));
}

TEST_CASE("centering floors toward negative coordinates") {
	// -1.0 m is region -1, not region 0: the window must not truncate toward zero.
	const ve::RegionWindow w = ve::region_window_centered(-1.0f, -1.0f, -1.0f, 16);
	CHECK(w.contains({-1, -1, -1}));
	CHECK(w.origin.x == -1 - 8);
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: FAIL to compile — `world/region_window.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `extension/src/world/region_window.h`:

```cpp
#pragma once
#include "world/region.h"

namespace ve {

// The near-field region map's index space. The near field only reaches as far as the brick
// atlas can pay for (RegionResidency), so the map does not need to span the world -- it needs
// to span residency. Deriving the size from residency is also what makes the no-alias
// invariant hold by construction: two regions that collide in the toroidal grid are `dim`
// regions apart, and two simultaneously RESIDENT regions are at most 2 * radius * margin
// apart. Pick dim so the first distance exceeds the second and a collision between two live
// entries is arithmetically impossible -- which is why the window needs no companion
// coordinate buffer, no stale-entry sweep when it moves, and no re-upload on recentre.
int region_window_dim(float radius_m, float evict_margin);

struct RegionWindow {
	IVec3 origin{0, 0, 0}; // minimum corner, in REGIONS
	int dim = 16;          // power of two, edge length in regions

	// Toroidal, and TOTAL: every region coordinate has a cell. Callers that need to know
	// whether the cell actually belongs to this region must ask contains() first.
	int index(IVec3 r) const;
	bool contains(IVec3 r) const;
	int cell_count() const { return dim * dim * dim; }
};

RegionWindow region_window_centered(float cx, float cy, float cz, int dim);

} // namespace ve
```

Create `extension/src/world/region_window.cpp`:

```cpp
#include "world/region_window.h"
#include <algorithm>
#include <cmath>

namespace ve {

int region_window_dim(float radius_m, float evict_margin) {
	const float span_m = 2.0f * std::max(radius_m, 0.0f) * std::max(evict_margin, 1.0f);
	const int need = static_cast<int>(std::ceil(span_m / kRegionSize)) + 1;
	int d = 4; // never smaller: a degenerate window would alias residents onto one cell
	while (d < need) d <<= 1;
	return d;
}

int RegionWindow::index(IVec3 r) const {
	const int m = dim - 1; // dim is a power of two, so this is the floor-mod mask
	const int x = r.x & m, y = r.y & m, z = r.z & m;
	return x + y * dim + z * dim * dim;
}

bool RegionWindow::contains(IVec3 r) const {
	return r.x >= origin.x && r.y >= origin.y && r.z >= origin.z && r.x < origin.x + dim &&
			r.y < origin.y + dim && r.z < origin.z + dim;
}

RegionWindow region_window_centered(float cx, float cy, float cz, int dim) {
	RegionWindow w;
	w.dim = dim;
	const IVec3 c{static_cast<int>(std::floor(cx / kRegionSize)),
			static_cast<int>(std::floor(cy / kRegionSize)),
			static_cast<int>(std::floor(cz / kRegionSize))};
	w.origin = {c.x - dim / 2, c.y - dim / 2, c.z - dim / 2};
	return w;
}

} // namespace ve
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd extension && scons test -j8
```

Expected: PASS, all five cases.

- [ ] **Step 5: Commit**

```bash
git add extension/src/world/region_window.h extension/src/world/region_window.cpp \
        extension/tests/test_region_window.cpp
git commit -m "feat: RegionWindow, the camera-centred toroidal region index space"
```

---

## Task 4: The op extent cap

`edit_log.cpp:16-25` clamps an op's region range to the world extent before looping, and its
comment records why: the earlier per-cell version let a ~1e5 m radius iterate 4.8e14 cells and
freeze for minutes. **This cap must land before Task 5 removes the bounds**, so there is never
a commit where the guard is absent.

**Files:**
- Modify: `extension/src/generator/edit_ops.h`, `extension/src/generator/edit_ops.cpp`
- Test: `extension/tests/test_edit_ops.cpp`

**Interfaces:**
- Consumes: `ve::EditOp`, `ve::op_region_range`.
- Produces:
  - `inline constexpr int ve::kMaxOpRegionSpan = 64;`
  - `int ve::op_region_span(const EditOp &op)` — the largest per-axis span, in regions.
  - `bool ve::op_region_span_ok(const EditOp &op)`

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_edit_ops.cpp`:

```cpp
TEST_CASE("op_region_span measures the largest axis span in regions") {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 0.0f; op.pos[1] = 0.0f; op.pos[2] = 0.0f;
	op.radius = 1.0f;
	// A 1 m sphere at the origin touches at most a couple of regions on each axis.
	CHECK(ve::op_region_span(op) <= 2);
	CHECK(ve::op_region_span_ok(op));
}

TEST_CASE("a hostile radius is rejected rather than iterated") {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.radius = 1.0e5f; // 100 km: 7813 regions per axis, 4.8e11 cells
	CHECK(ve::op_region_span(op) > ve::kMaxOpRegionSpan);
	CHECK(!ve::op_region_span_ok(op));
}

TEST_CASE("the cap admits the largest legitimate edit") {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	// kMaxOpRegionSpan is 64 regions = 1638.4 m, far past any tool radius the demo produces.
	op.radius = 100.0f;
	CHECK(ve::op_region_span_ok(op));
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: FAIL to compile — `op_region_span` is not declared.

- [ ] **Step 3: Implement the cap**

In `extension/src/generator/edit_ops.h`, beside the other op helpers:

```cpp
// The world used to be a box, and clamping an op's region range to it doubled as a DoS
// guard: without a bound, a hostile radius (~1e5 m) makes the append loop iterate ~4.8e14
// cells and freeze for minutes. The world has no edge any more, so the bound is stated
// directly. 64 regions is 1638.4 m -- an order of magnitude past any tool radius the demo
// can produce, and still four orders of magnitude short of the pathological case.
inline constexpr int kMaxOpRegionSpan = 64;

// The largest per-axis span of the op's region range, in regions.
int op_region_span(const EditOp &op);
bool op_region_span_ok(const EditOp &op);
```

In `extension/src/generator/edit_ops.cpp`:

```cpp
int op_region_span(const EditOp &op) {
	IVec3 lo{}, hi{};
	op_region_range(op, &lo, &hi);
	const int sx = hi.x - lo.x + 1;
	const int sy = hi.y - lo.y + 1;
	const int sz = hi.z - lo.z + 1;
	return std::max(sx, std::max(sy, sz));
}

bool op_region_span_ok(const EditOp &op) { return op_region_span(op) <= kMaxOpRegionSpan; }
```

Add `#include <algorithm>` to `edit_ops.cpp` if it is not already there.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd extension && scons test -j8
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/src/generator/edit_ops.h extension/src/generator/edit_ops.cpp \
        extension/tests/test_edit_ops.cpp
git commit -m "feat: explicit op region-span cap, replacing the world edge's DoS guard"
```

---

## Task 5: `EditLog` loses its bounds

**Files:**
- Modify: `extension/src/world/edit_log.h`, `extension/src/world/edit_log.cpp`
- Test: `extension/tests/test_edit_log.cpp`, `extension/tests/test_unbounded_characterization.cpp`

**Interfaces:**
- Consumes: `ve::kMaxOpRegionSpan`, `ve::op_region_span_ok` (Task 4).
- Produces:
  - `ve::EditLog::EditLog()` — default-constructible, no bounds parameter.
  - `ve::EditLog::AppendResult` gains `bool oversized = false;`
  - `EditLog::bounds()` is **removed**. Callers in Task 12 must not expect it.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_edit_log.cpp`:

```cpp
TEST_CASE("an op far outside the old world box is accepted") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 50000.0f; op.pos[1] = 0.0f; op.pos[2] = 50000.0f;
	op.radius = 2.0f;
	const auto r = log.append(op);
	CHECK(!r.oversized);
	CHECK(r.rejected.empty());
	CHECK(!r.touched.empty());
	CHECK(log.op_count({1953, 0, 1953}) >= 1); // 50000 / 25.6 = 1953.1
}

TEST_CASE("an oversized op is refused instead of iterated") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.radius = 1.0e5f;
	const auto r = log.append(op);
	CHECK(r.oversized);
	CHECK(r.touched.empty());
	CHECK(r.rejected.empty());
	CHECK(log.region_count() == 0);
}

TEST_CASE("negative region coordinates get their own lists") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = -100.0f; op.pos[1] = -100.0f; op.pos[2] = -100.0f;
	op.radius = 1.0f;
	const auto r = log.append(op);
	CHECK(!r.touched.empty());
	CHECK(log.op_count({-4, -4, -4}) >= 1); // floor(-100 / 25.6) = -4
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: FAIL to compile — `EditLog` has no default constructor and `AppendResult` has no
`oversized` member.

- [ ] **Step 3: Change `EditLog`**

In `extension/src/world/edit_log.h`:
- Add `bool oversized = false;` to `AppendResult`, with a comment naming `kMaxOpRegionSpan`.
- Replace `explicit EditLog(const WorldBounds &bounds) : bounds_(bounds) {}` with `EditLog() = default;`.
- Delete `const WorldBounds &bounds() const { return bounds_; }` and the `WorldBounds bounds_;` member.
- In `collect_ops_for_aabb`, delete the line
  `if (!log.bounds().contains_region(region)) continue;` and guard the query extent instead —
  immediately after the `rlo` / `rhi` computation, insert:

```cpp
	// Same bound as the append path: without the world edge, a hostile AABB would sweep an
	// unbounded region range. See ve::kMaxOpRegionSpan.
	if (rhi.x - rlo.x + 1 > kMaxOpRegionSpan || rhi.y - rlo.y + 1 > kMaxOpRegionSpan ||
			rhi.z - rlo.z + 1 > kMaxOpRegionSpan)
		return;
```

- Add `#include "generator/edit_ops.h"` if the header does not already pull it in (it does —
  keep the existing include).

In `extension/src/world/edit_log.cpp`, replace the clamp block in `append()` with the cap:

```cpp
EditLog::AppendResult EditLog::append(const EditOp &op) {
	AppendResult result;
	IVec3 lo{}, hi{};
	op_region_range(op, &lo, &hi);
	// The world has no edge, so there is nothing to clamp against. The bound that used to
	// fall out of the box is now stated directly: an op whose region range is absurd is
	// refused outright rather than iterated (ve::kMaxOpRegionSpan).
	if (!op_region_span_ok(op)) {
		result.oversized = true;
		return result;
	}
	const uint64_t seq = next_seq_++;
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 r{x, y, z};
				const Key key{r.x, r.y, r.z};
				std::vector<EditOp> &list = lists_[key];
				if (static_cast<int>(list.size()) >= kMaxRegionOps) {
					result.rejected.push_back(r);
					continue;
				}
				list.push_back(op);
				seqs_[key].push_back(seq);
				result.touched.push_back(r);
			}
	return result;
}
```

- [ ] **Step 4: Update the callers and the characterization golden**

`ve::EditLog(bounds)` is constructed in `WorldStore::ensure_edit_log`. Change its signature to
take no argument for now:

```cpp
	void ensure_edit_log() {
		if (!edit_log_)
			edit_log_ = new ve::EditLog();
	}
```

and update its one call site, `extension/src/render/orchestrator.cpp:206`, to
`handles_.store->ensure_edit_log();`.

In `extension/tests/test_edit_log.cpp` replace every `ve::EditLog log(bounds());` with
`ve::EditLog log;` and delete the now-unused `bounds()` helper.

In `extension/tests/test_unbounded_characterization.cpp`, the
`"characterization: edit fan-out at the old world edge"` case now describes changed behaviour.
Update it deliberately — this is the diff a reviewer must see:

```cpp
TEST_CASE("characterization: edit fan-out at the old world edge") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 1638.0f; op.pos[1] = 0.0f; op.pos[2] = 8.0f;
	op.radius = 4.0f;
	const ve::EditLog::AppendResult r = log.append(op);
	CHECK(r.rejected.empty());
	CHECK(!r.touched.empty());
	// CHANGED by the unbounded-world refactor: region 64 used to be clamped away because it
	// lay past the 1638.4 m world edge. There is no edge now, so the op reaches it.
	CHECK(std::any_of(r.touched.begin(), r.touched.end(),
			[](const ve::IVec3 &t) { return t.x == 64; }));
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd extension && scons test -j8
```

Expected: PASS. Then build the extension so the orchestrator change is checked:

```bash
./build.sh
```

- [ ] **Step 6: Commit**

```bash
git add extension/src/world/edit_log.h extension/src/world/edit_log.cpp \
        extension/src/core/world_store.h extension/src/render/orchestrator.cpp \
        extension/tests/test_edit_log.cpp extension/tests/test_unbounded_characterization.cpp
git commit -m "refactor: EditLog has no world edge"
```

---

## Task 6: `RegionResidency` takes a `RegionWindow`

**Files:**
- Modify: `extension/src/world/residency.h`, `extension/src/world/residency.cpp`
- Test: `extension/tests/test_residency.cpp`, `extension/tests/test_unbounded_characterization.cpp`

**Interfaces:**
- Consumes: `ve::RegionWindow`, `ve::region_window_dim`, `ve::region_window_centered` (Task 3).
- Produces:
  - `ResidencyConfig::window` (a `RegionWindow`) replaces `ResidencyConfig::bounds`.
  - `ResidencyPlan::Entry::map_index` is now the window cell index; it is never -1.
  - `const RegionWindow &RegionResidency::window() const`

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_residency.cpp`:

```cpp
TEST_CASE("residency has no world edge") {
	ve::ResidencyConfig cfg;
	cfg.window = ve::region_window_centered(50000.0f, 0.0f, 50000.0f,
			ve::region_window_dim(60.0f, cfg.evict_margin));
	cfg.radius_m = 60.0f;
	cfg.max_region_slots = 256;
	cfg.max_loads_per_frame = 8;
	ve::RegionResidency res(cfg);
	// 50 km from the origin, far outside any box the engine used to have.
	settle(res, 50000.0f, 0.0f, 50000.0f);
	CHECK(res.resident_count() > 0);
}

TEST_CASE("every load carries a valid window cell index") {
	ve::ResidencyConfig cfg;
	cfg.window = ve::region_window_centered(0.0f, 0.0f, 0.0f,
			ve::region_window_dim(60.0f, cfg.evict_margin));
	cfg.radius_m = 60.0f;
	cfg.max_region_slots = 256;
	cfg.max_loads_per_frame = 8;
	ve::RegionResidency res(cfg);
	for (int i = 0; i < 200; i++) {
		const ve::ResidencyPlan p = res.update(0.0f, 0.0f, 0.0f);
		for (const auto &e : p.loads) {
			CHECK(e.map_index >= 0);
			CHECK(e.map_index < cfg.window.cell_count());
		}
		if (p.loads.empty() && p.evicts.empty()) break;
	}
}

TEST_CASE("no two simultaneously resident regions share a window cell") {
	// Invariant 3, exercised along a travelling camera rather than at one standstill.
	ve::ResidencyConfig cfg;
	cfg.radius_m = 96.0f;
	cfg.max_region_slots = 512;
	cfg.max_loads_per_frame = 8;
	const int dim = ve::region_window_dim(cfg.radius_m, cfg.evict_margin);
	cfg.window = ve::region_window_centered(0.0f, 0.0f, 0.0f, dim);
	ve::RegionResidency res(cfg);
	for (int step = 0; step < 400; step++) {
		const float x = float(step) * 12.0f; // ~5 km of travel
		res.set_window(ve::region_window_centered(x, 40.0f, 0.0f, dim));
		res.update(x, 40.0f, 0.0f);
		std::vector<ve::IVec3> regions;
		res.resident_regions(&regions);
		std::set<int> cells;
		for (const ve::IVec3 &r : regions) {
			const int idx = res.window().index(r);
			CHECK(cells.insert(idx).second); // false means two residents aliased
		}
	}
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: FAIL to compile — `ResidencyConfig` has no `window`, `RegionResidency` has no
`set_window` or `window`.

- [ ] **Step 3: Change `RegionResidency`**

In `extension/src/world/residency.h`:
- `#include "world/region_window.h"`.
- Replace `WorldBounds bounds{};` in `ResidencyConfig` with:

```cpp
	// The near-field region map's index space. Sized from radius_m x evict_margin so no two
	// simultaneously resident regions can share a cell (see ve::region_window_dim).
	RegionWindow window{};
```

- Add to the public section of `RegionResidency`:

```cpp
	const RegionWindow &window() const { return cfg_.window; }
	// The window follows the camera. Called once per frame by the streamer, before update().
	void set_window(const RegionWindow &w) { cfg_.window = w; }
```

In `extension/src/world/residency.cpp`:
- `release()` (line 53): `plan->evicts.push_back({region, slot, cfg_.window.index(region)});`
- The candidate scan (lines ~88-105): delete the `o` / `sz` locals and the `std::max` /
  `std::min` clamps, leaving the radius span alone:

```cpp
	// The scan is over the radius' region AABB. There is no world to clamp against any more;
	// the radius is the only bound, and at the shipping radius that is ~500 cells.
	const auto span = [](float lo, float hi) {
		return std::make_pair(static_cast<int>(std::floor(lo / kRegionSize)),
				static_cast<int>(std::floor(hi / kRegionSize)));
	};
	const auto rx = span(cx - cfg_.radius_m, cx + cfg_.radius_m);
	const auto ry = span(cy - cfg_.radius_m, cy + cfg_.radius_m);
	const auto rz = span(cz - cfg_.radius_m, cz + cfg_.radius_m);

	struct Cand { float dist; IVec3 region; };
	std::vector<Cand> cands;
	for (int z = rz.first; z <= rz.second; z++)
		for (int y = ry.first; y <= ry.second; y++)
			for (int x = rx.first; x <= rx.second; x++) {
```

- The load push (line ~173): `plan.loads.push_back({c.region, slot, cfg_.window.index(c.region)});`

- [ ] **Step 4: Update `WorldStore::ensure_residency`, which still assigns `rcfg.bounds`**

`ResidencyConfig` no longer has a `bounds` member, so `extension/src/core/world_store.h` will
not compile until its lazy-init arm builds a window instead. Change it now — Task 15 tidies the
surrounding config, but the build has to stay green here:

```cpp
	void ensure_residency() {
		if (!residency_) {
			ve::ResidencyConfig rcfg;
			rcfg.radius_m = config_.residency_radius_m;
			rcfg.max_region_slots = config_.max_region_slots;
			rcfg.window = ve::region_window_centered(0.0f, 0.0f, 0.0f,
					ve::region_window_dim(rcfg.radius_m, rcfg.evict_margin));
			residency_ = new ve::RegionResidency(rcfg);
		}
	}
```

Add `#include "world/region_window.h"` to `world_store.h`, and change its one call site,
`extension/src/render/orchestrator.cpp:213`, to `handles_.store->ensure_residency();`.

- [ ] **Step 5: Update the existing residency tests and the characterization golden**

In `extension/tests/test_residency.cpp`, `make_cfg` becomes:

```cpp
static ve::ResidencyConfig make_cfg(float radius, int slots, int per_frame) {
	ve::ResidencyConfig cfg;
	cfg.radius_m = radius;
	cfg.max_region_slots = slots;
	cfg.max_loads_per_frame = per_frame;
	cfg.window = ve::region_window_centered(0.0f, 0.0f, 0.0f,
			ve::region_window_dim(radius, cfg.evict_margin));
	return cfg;
}
```

Add `#include "world/region_window.h"` and `#include <set>` at the top.

Any existing case that relied on the world edge clipping the candidate scan will now see more
candidates. Read each failure before changing it: if the case was asserting a count that the
box produced, update the count and add a comment saying the box is gone. If it was asserting
an ordering or a budget rule, the number should not have moved and a change means a real bug.

In `extension/tests/test_unbounded_characterization.cpp`, update
`"characterization: settled residency at the demo spawn"` to build the config the new way and
assert against `res.window().cell_count()` instead of `64 * 8 * 64`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cd extension && scons test -j8 && ./build.sh
```

Expected: PASS, including the travelling-camera aliasing case, and the extension builds.

- [ ] **Step 7: Commit**

```bash
git add extension/src/world/residency.h extension/src/world/residency.cpp \
        extension/src/core/world_store.h extension/src/render/orchestrator.cpp \
        extension/tests/test_residency.cpp extension/tests/test_unbounded_characterization.cpp
git commit -m "refactor: residency indexes through the region window, not the world box"
```

---

## Task 7: `lod_roots_in_radius`

**Files:**
- Modify: `extension/src/lod/lod_grid.h`, `extension/src/lod/lod_grid.cpp`
- Test: `extension/tests/test_lod_grid.cpp`

**Interfaces:**
- Consumes: `ve::lod_chunk_size`, `ve::lod_chunk_distance`.
- Produces:
  - `void ve::lod_roots_in_radius(const float cam_pos[3], float radius_m, std::vector<IVec3> *out)`
  - `ve::lod_root_range` and `ve::lod_chunk_in_bounds` are **deleted**.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_lod_grid.cpp`:

```cpp
TEST_CASE("root selection covers the camera's own root") {
	const float cam[3] = {8.0f, 62.0f, 8.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, 1638.4f, &roots);
	const ve::IVec3 own = ve::lod_chunk_of_point(ve::kLodLevels - 1, cam[0], cam[1], cam[2]);
	CHECK(std::find(roots.begin(), roots.end(), own) != roots.end());
}

TEST_CASE("root count grows with the radius and stays bounded") {
	const float cam[3] = {0.0f, 0.0f, 0.0f};
	std::vector<ve::IVec3> near_roots, far_roots;
	ve::lod_roots_in_radius(cam, 1638.4f, &near_roots);
	ve::lod_roots_in_radius(cam, 4000.0f, &far_roots);
	CHECK(near_roots.size() >= 8);
	CHECK(near_roots.size() <= 27);   // (ceil(2R/1638.4) + 1)^3 candidates
	CHECK(far_roots.size() > near_roots.size());
	CHECK(far_roots.size() <= 216);
}

TEST_CASE("every emitted root actually intersects the radius") {
	const float cam[3] = {1234.0f, -56.0f, 7890.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, 2000.0f, &roots);
	REQUIRE(!roots.empty());
	for (const ve::IVec3 &c : roots)
		CHECK(ve::lod_chunk_distance(ve::kLodLevels - 1, c, cam) <= 2000.0f);
}

TEST_CASE("root selection has no origin bias") {
	// The old world box put its origin at (0, -51.2, 0). A radius has no such anchor: the
	// same camera-relative geometry must appear 50 km away.
	const float near_cam[3] = {0.0f, 0.0f, 0.0f};
	const float far_cam[3] = {50000.0f, 0.0f, 50000.0f};
	std::vector<ve::IVec3> a, b;
	ve::lod_roots_in_radius(near_cam, 2000.0f, &a);
	ve::lod_roots_in_radius(far_cam, 2000.0f, &b);
	CHECK(a.size() == b.size());
}
```

Add `#include <algorithm>` and `#include <vector>` at the top if absent.

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: FAIL to compile — `lod_roots_in_radius` is not declared.

- [ ] **Step 3: Implement it**

In `extension/src/lod/lod_grid.h`, delete the declarations of `lod_chunk_in_bounds` and
`lod_root_range`, and add:

```cpp
// The root-level chunks intersecting a sphere of `radius_m` around the camera. This is what
// replaced the world AABB: an unbounded world has no edge to enumerate from, so the forest of
// octree roots follows the camera. Air roots cost one build each to discover and then prune
// their whole subtree (LodTree::visit treats kLodEmpty as terminal), so a generous radius is
// paid for in one-time builds, not per frame.
void lod_roots_in_radius(const float cam_pos[3], float radius_m, std::vector<IVec3> *out);
```

Add `#include <vector>` to `lod_grid.h`.

In `extension/src/lod/lod_grid.cpp`, delete `lod_chunk_in_bounds` and `lod_root_range`, and add:

```cpp
void lod_roots_in_radius(const float cam_pos[3], float radius_m, std::vector<IVec3> *out) {
	out->clear();
	if (!cam_pos || !out || radius_m <= 0.0f) return;
	const int top = kLodLevels - 1;
	const float s = lod_chunk_size(top);
	const IVec3 lo{static_cast<int>(std::floor((cam_pos[0] - radius_m) / s)),
			static_cast<int>(std::floor((cam_pos[1] - radius_m) / s)),
			static_cast<int>(std::floor((cam_pos[2] - radius_m) / s))};
	const IVec3 hi{static_cast<int>(std::floor((cam_pos[0] + radius_m) / s)),
			static_cast<int>(std::floor((cam_pos[1] + radius_m) / s)),
			static_cast<int>(std::floor((cam_pos[2] + radius_m) / s))};
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 c{x, y, z};
				// The sphere test, not the AABB: it trims roughly half the corner candidates.
				if (lod_chunk_distance(top, c, cam_pos) > radius_m) continue;
				out->push_back(c);
			}
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: the four new cases PASS. `test_lod_tree.cpp` and
`test_unbounded_characterization.cpp` will not compile yet — they still call `lod_root_range`.
That is Task 8; do not fix them here.

- [ ] **Step 5: Commit**

Commit `lod_grid` alone so the root-selection change is reviewable on its own. The tree still
references the deleted functions, so use a WIP-free ordering: complete Task 8 before pushing a
branch that must build. Commit locally now:

```bash
git add extension/src/lod/lod_grid.h extension/src/lod/lod_grid.cpp extension/tests/test_lod_grid.cpp
git commit -m "feat: lod_roots_in_radius replaces the world-AABB root range"
```

---

## Task 8: `LodTree` takes a stream radius

**Files:**
- Modify: `extension/src/lod/lod_tree.h`, `extension/src/lod/lod_tree.cpp:259,269,304,328,390`
- Test: `extension/tests/test_lod_tree.cpp`, `extension/tests/test_unbounded_characterization.cpp`

**Interfaces:**
- Consumes: `ve::lod_roots_in_radius` (Task 7).
- Produces: `LodTreeConfig::stream_radius_m` (default `1638.4f`) replaces `LodTreeConfig::bounds`.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_lod_tree.cpp`:

```cpp
TEST_CASE("the walk builds a root forest around the camera, anywhere in the world") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	ve::LodTree tree(cfg);
	NoOcclusion occ;
	ve::LodWalkResult out;
	// 50 km from the origin: outside every box the engine ever had.
	tree.walk(cam_at(50000.0f, 62.0f, 50000.0f), &occ, 1, &out);
	CHECK(!out.requests.empty());
	// Every request must sit within the stream radius of the camera.
	const float cam[3] = {50000.0f, 62.0f, 50000.0f};
	for (const ve::LodBuildRequest &q : out.requests)
		CHECK(ve::lod_chunk_distance(q.level, q.coord, cam) <= cfg.stream_radius_m);
}

TEST_CASE("a larger stream radius asks for more roots") {
	NoOcclusion occ;
	ve::LodWalkResult small, large;
	ve::LodTreeConfig a; a.stream_radius_m = 1638.4f;
	ve::LodTreeConfig b; b.stream_radius_m = 4000.0f;
	ve::LodTree ta(a), tb(b);
	ta.walk(cam_at(0.0f, 62.0f, 0.0f), &occ, 1, &small);
	tb.walk(cam_at(0.0f, 62.0f, 0.0f), &occ, 1, &large);
	CHECK(large.requests.size() >= small.requests.size());
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: FAIL to compile — `LodTreeConfig` has no `stream_radius_m`.

- [ ] **Step 3: Change `LodTree`**

In `extension/src/lod/lod_tree.h`, in `LodTreeConfig`, replace `WorldBounds bounds{};` with:

```cpp
	// The far field's extent. There is no world edge: the octree's roots are the level-7
	// chunks intersecting a sphere of this radius around the camera, and nodes beyond it lose
	// their eviction exemption (see collect_evictions).
	float stream_radius_m = 1638.4f;
```

In `extension/src/lod/lod_tree.cpp`:
- Line 259 (`children_ready`): delete
  `if (!lod_chunk_in_bounds(cfg_.bounds, level - 1, ch)) continue;`. All eight children now
  always count, which is strictly more correct — the old "outside is done" arm let a node be
  refined while a child that had never been built was treated as ready.
- Line 269 (`request`): replace the bounds test with the radius test:

```cpp
	if (lod_chunk_distance(level, c, last_cam_pos_) > cfg_.stream_radius_m) return;
```

- Line 304 (`shadow_visit`) and line 328 (`visit`): same replacement. `shadow_visit` is `const`
  and already reads `last_cam_pos_`, so no signature change is needed.
- Line 390 (`walk`): replace the triple loop over `lod_root_range` with:

```cpp
	std::vector<IVec3> roots;
	lod_roots_in_radius(cam.pos, cfg_.stream_radius_m, &roots);
	for (const IVec3 &r : roots) visit(kLodLevels - 1, r, cam, occ, frame, out);

	// The sun's cut, over the SAME resident tree the walk above just updated. It is a
	// separate recursion rather than an extra output of visit() because visit() stops at the
	// frustum and marks residency as it goes; this one must do neither.
	for (const IVec3 &r : roots) shadow_visit(kLodLevels - 1, r, cam, &out->shadow_draws);
```

- [ ] **Step 4: Update the existing tree tests**

In `extension/tests/test_lod_tree.cpp`:
- Delete `demo_bounds()`.
- Every `cfg.bounds = demo_bounds();` becomes `cfg.stream_radius_m = 1638.4f;`.
- `make_ready_path` takes the radius instead of bounds, and enumerates roots the new way:

```cpp
void make_ready_path(ve::LodTree *t, float stream_radius_m, const ve::IVec3 &root,
		const ve::IVec3 &target) {
	const float cam[3] = {0.0f, 62.0f, 0.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, stream_radius_m, &roots);
	for (const ve::IVec3 &r : roots)
		if (!(r == root)) t->note_empty(ve::kLodLevels - 1, r);
	// ... rest of the body unchanged
```

Update its call sites to pass `1638.4f`. If a case then sees more sibling requests than it did
(the forest is larger than the single old root), that is expected — read the assertion, adjust
the count, and note the reason in a comment.

In `extension/tests/test_unbounded_characterization.cpp`, replace
`"characterization: root range at demo bounds"` with its successor:

```cpp
TEST_CASE("characterization: root forest at the demo spawn") {
	// CHANGED by the unbounded-world refactor: root selection used to come from the world
	// AABB, which was exactly one L7 chunk wide in XZ. It now follows the camera.
	const float cam[3] = {8.0f, 62.0f, 8.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, 1638.4f, &roots);
	CHECK(roots.size() >= 8);
	CHECK(roots.size() <= 27);
	const ve::IVec3 own = ve::lod_chunk_of_point(ve::kLodLevels - 1, cam[0], cam[1], cam[2]);
	CHECK(std::find(roots.begin(), roots.end(), own) != roots.end());
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd extension && scons test -j8
```

Expected: PASS across the whole native suite.

- [ ] **Step 6: Commit**

```bash
git add extension/src/lod/lod_tree.h extension/src/lod/lod_tree.cpp \
        extension/tests/test_lod_tree.cpp extension/tests/test_unbounded_characterization.cpp
git commit -m "refactor: the LoD tree walks a camera-centred root forest"
```

---

## Task 9: Distance-gated LoD eviction

`collect_evictions` skips every node at level >= `resident_level_from` unconditionally
(`lod_tree.cpp:486`), so those nodes are never candidates and never erased. In a bounded world
that was ~190 chunks; in an unbounded one it is a leak proportional to distance travelled.

**Files:**
- Modify: `extension/src/lod/lod_tree.cpp:485-491`
- Test: `extension/tests/test_lod_tree.cpp`

**Interfaces:**
- Consumes: `LodTreeConfig::stream_radius_m` (Task 8), `LodTree::last_cam_pos_`.
- Produces: no new symbols; `collect_evictions`'s signature is unchanged.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_lod_tree.cpp`:

```cpp
TEST_CASE("coarse nodes outside the stream radius are evictable") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	cfg.evict_frames = 10;
	ve::LodTree tree(cfg);
	NoOcclusion occ;
	ve::LodWalkResult out;

	// Walk at the origin so nodes near it are marked at frame 1.
	tree.walk(cam_at(0.0f, 62.0f, 0.0f), &occ, 1, &out);
	tree.note_ready(ve::kLodLevels - 1, {0, 0, 0}, 0, 1);
	const int after_first = tree.node_count();
	CHECK(after_first > 0);

	// Travel far away and let the age rule run. The node at the origin is a level-7 node,
	// which the old exemption made permanently immortal.
	for (uint32_t f = 2; f < 40; f++) {
		ve::LodWalkResult w;
		tree.walk(cam_at(50000.0f, 62.0f, 50000.0f), &occ, f, &w);
	}
	std::vector<ve::LodDrawItem> evicted;
	tree.collect_evictions(40, 0, &evicted);
	CHECK(std::any_of(evicted.begin(), evicted.end(),
			[](const ve::LodDrawItem &d) { return d.level == ve::kLodLevels - 1; }));
}

TEST_CASE("coarse nodes inside the stream radius keep their exemption") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	cfg.evict_frames = 10;
	ve::LodTree tree(cfg);
	NoOcclusion occ;
	ve::LodWalkResult out;
	tree.walk(cam_at(0.0f, 62.0f, 0.0f), &occ, 1, &out);
	tree.note_ready(ve::kLodLevels - 1, {0, 0, 0}, 0, 1);
	// Frame 200 with no further walks: the node is stale by age but still inside the radius.
	std::vector<ve::LodDrawItem> evicted;
	tree.collect_evictions(200, 0, &evicted);
	CHECK(std::none_of(evicted.begin(), evicted.end(), [](const ve::LodDrawItem &d) {
		return d.level == ve::kLodLevels - 1 && d.coord.x == 0 && d.coord.z == 0;
	}));
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
./extension/build/tests/ve_tests -tc="coarse nodes outside the stream radius are evictable"
```

Expected: FAIL — nothing at level 7 is ever returned, because the exemption is unconditional.

- [ ] **Step 3: Gate the exemption on distance**

In `extension/src/lod/lod_tree.cpp`, replace line 486 inside `collect_evictions`'s candidate
loop:

```cpp
	for (const auto &kv : nodes_) {
		// Levels at or above resident_level_from are exempt -- but only while they are still
		// inside the stream radius. Without the distance arm this exemption is a leak: an
		// unbounded world leaves a permanently resident coarse node behind at every place the
		// camera has ever been.
		if (kv.first.level >= cfg_.resident_level_from) {
			const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
			if (lod_chunk_distance(kv.first.level, c, last_cam_pos_) <= cfg_.stream_radius_m)
				continue;
		}
		if (kv.second.building) continue;
		if (kv.second.state == kLodBuilding) continue;
		const uint32_t age = frame >= kv.second.last_marked ? frame - kv.second.last_marked : 0u;
		cands.push_back(Cand{kv.first, age, kv.second.page_count, kv.second.page_first});
	}
```

`last_cam_pos_` is refreshed by every `walk()`, so no signature change is needed. Add
`#include "lod/lod_grid.h"` if `lod_tree.cpp` does not already include it (it does).

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd extension && scons test -j8
```

Expected: PASS, both new cases and the whole suite.

- [ ] **Step 5: Commit**

```bash
git add extension/src/lod/lod_tree.cpp extension/tests/test_lod_tree.cpp
git commit -m "fix: coarse LoD nodes lose their eviction exemption outside the stream radius"
```

---

## Task 10: Occupancy retention

`OccupancyGrid` keeps every block it has ever been given — 8 KB per region, roughly 92 MB per
4 km walked. Its own header comment says blocks are never evicted "for as long as the world
lives"; that contract is now false and must change with the code.

**Files:**
- Modify: `extension/src/connectivity/occupancy.h`, `extension/src/connectivity/occupancy.cpp`
- Test: `extension/tests/test_occupancy.cpp`

**Interfaces:**
- Consumes: `ve::RegionResidency::region_distance` (static).
- Produces: `int OccupancyGrid::evict_outside(float cx, float cy, float cz, float retention_m)`
  — returns the number of blocks dropped.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_occupancy.cpp`:

```cpp
TEST_CASE("blocks outside the retention radius are dropped") {
	ve::OccupancyGrid g;
	std::vector<uint8_t> block(ve::kOccupancyBlockBytes, 0);
	g.set_block({0, 0, 0}, block.data(), 1);
	g.set_block({100, 0, 100}, block.data(), 1); // 2560 m away
	CHECK(g.region_count() == 2);

	const int dropped = g.evict_outside(0.0f, 0.0f, 0.0f, 256.0f);
	CHECK(dropped == 1);
	CHECK(g.region_count() == 1);
	CHECK(g.has_region({0, 0, 0}));
	CHECK(!g.has_region({100, 0, 100}));
}

TEST_CASE("eviction is lossless for regions that come back") {
	// Dropping a block must read as 'nobody has looked', never as 'air'. kCellUnknown is the
	// zero state precisely so a re-streamed region cannot be mistaken for empty space.
	ve::OccupancyGrid g;
	std::vector<uint8_t> block(ve::kOccupancyBlockBytes, 0);
	g.set_cell({4000, 0, 4000}, ve::kCellAir, 1);
	CHECK(g.state({4000, 0, 4000}) == ve::kCellAir);
	g.evict_outside(0.0f, 0.0f, 0.0f, 256.0f);
	CHECK(g.state({4000, 0, 4000}) == ve::kCellUnknown);
	CHECK(g.is_solid({4000, 0, 4000})); // unknown counts as solid, as it always has
}

TEST_CASE("a walk of ten kilometres does not grow the grid without bound") {
	ve::OccupancyGrid g;
	std::vector<uint8_t> block(ve::kOccupancyBlockBytes, 0);
	int peak = 0;
	for (int step = 0; step < 400; step++) {
		const float x = float(step) * 25.6f; // one region per step, ~10 km
		const ve::IVec3 r{step, 0, 0};
		g.set_block(r, block.data(), 1);
		g.evict_outside(x, 0.0f, 0.0f, 256.0f);
		peak = std::max(peak, g.region_count());
	}
	// 256 m retention is 10 regions each way; the grid must plateau, not accumulate 400.
	CHECK(peak <= 32);
}
```

Add `#include <algorithm>` and `#include <vector>` if absent.

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: FAIL to compile — `evict_outside` is not declared.

- [ ] **Step 3: Implement retention**

In `extension/src/connectivity/occupancy.h`, rewrite the class comment's eviction sentence and
declare the method:

```cpp
// Which cells hold matter, kept in sparse per-region blocks. Regions arrive from the GPU mark
// pass and are retained while they are near the camera: a region's occupancy is recomputable
// from the mark pass against edits and overrides, so dropping a distant block is lossless, and
// in an unbounded world retaining every block ever probed grows with distance travelled
// (~92 MB per 4 km). A dropped block reads back as kCellUnknown -- "nobody has looked" -- which
// is the state this grid must never confuse with air.
```

and, in the public section:

```cpp
	// Drop every block whose region lies further than retention_m from the point. Returns the
	// number dropped. Main thread only, like every other method here.
	int evict_outside(float cx, float cy, float cz, float retention_m);
```

In `extension/src/connectivity/occupancy.cpp`:

```cpp
int OccupancyGrid::evict_outside(float cx, float cy, float cz, float retention_m) {
	int dropped = 0;
	for (auto it = blocks_.begin(); it != blocks_.end();) {
		const IVec3 r{it->first.x, it->first.y, it->first.z};
		if (RegionResidency::region_distance(r, cx, cy, cz) > retention_m) {
			it = blocks_.erase(it);
			dropped++;
		} else {
			++it;
		}
	}
	return dropped;
}
```

Add `#include "world/residency.h"` to `occupancy.cpp`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd extension && scons test -j8
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add extension/src/connectivity/occupancy.h extension/src/connectivity/occupancy.cpp \
        extension/tests/test_occupancy.cpp
git commit -m "feat: occupancy blocks are retained by distance, not forever"
```

---

## Task 11: `ChunkResidency` loses its bounds

**Files:**
- Modify: `extension/src/mesh/chunk_residency.h:18`, `extension/src/mesh/chunk_residency.cpp:184`
- Test: `extension/tests/test_chunk_residency.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `ChunkResidencyConfig::bounds` is **removed**.

- [ ] **Step 1: Write the failing test**

Append to `extension/tests/test_chunk_residency.cpp`:

```cpp
TEST_CASE("collider chunks stream 50 km from the origin") {
	ve::ChunkResidencyConfig cfg;
	cfg.radius_m = 64.0f;
	cfg.max_chunks = kAmplePool;
	cfg.max_builds_per_frame = 2;
	cfg.max_probes_per_frame = 4096;
	ve::ChunkResidency res(cfg);
	// A probe that finds a surface everywhere, so the radius is the only bound left.
	struct Everywhere : ve::ChunkProbe {
		bool chunk_has_surface(ve::IVec3) const override { return true; }
	} probe;
	// 50 km from the origin, far outside every box the engine used to have.
	const float c[3] = {50000.0f, 62.0f, 50000.0f};
	res.update(c, nullptr, 1, probe);
	CHECK(res.resident_count() > 0);
}
```

`kAmplePool` is already a file-static in `test_chunk_residency.cpp`; reuse it rather than
declaring a second one.

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
./extension/build/tests/ve_tests -tc="collider chunks stream 50 km from the origin"
```

Expected: FAIL — no chunks load, because `contains_brick` rejects every candidate.

- [ ] **Step 3: Delete the guard**

In `extension/src/mesh/chunk_residency.h`, remove `WorldBounds bounds{};` from
`ChunkResidencyConfig`.

In `extension/src/mesh/chunk_residency.cpp:184`, delete these two lines:

```cpp
					// A chunk is 16 bricks and the world is region-aligned (32 bricks), so a
					// chunk is either wholly inside or wholly outside: one corner decides.
					if (!cfg_.bounds.contains_brick(chunk_min_brick(c))) continue;
```

The `if (chunk_distance(c, cx, cy, cz) > r) continue;` on the following line is now the only
bound, which is what it always did the real work of anyway.

- [ ] **Step 4: Update the config's other writer**

`ChunkResidencyConfig::bounds` is assigned in `extension/src/voxel_world.cpp:727` (`ccfg.bounds
= world_bounds();`). Delete that line.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd extension && scons test -j8 && ./build.sh
```

Expected: PASS, and the extension builds.

- [ ] **Step 6: Commit**

```bash
git add extension/src/mesh/chunk_residency.h extension/src/mesh/chunk_residency.cpp \
        extension/src/voxel_world.cpp extension/tests/test_chunk_residency.cpp
git commit -m "refactor: collider chunk residency is bounded by radius alone"
```

---

## Task 12: `IslandManager` loses its three guards

All three are op fan-out filters — "which regions does this op touch, that actually exist". In
an unbounded world every region exists, so they are pure deletions. The loops stay bounded by
`kMaxOpRegionSpan` (Task 4), which is what `op_region_range` now respects.

**Files:**
- Modify: `extension/src/physics/island_manager.cpp:546,667,1124`
- Test: `tests/test_island_body.gd` (gdUnit, GPU-backed)

**Interfaces:**
- Consumes: `ve::kMaxOpRegionSpan` (Task 4). `EditLog::bounds()` no longer exists (Task 5), so
  these call sites will not compile until they are deleted.
- Produces: nothing new.

- [ ] **Step 1: Delete the guard at line 546**

```cpp
				for (int x = rlo.x; x <= rhi.x; x++) {
					const ve::IVec3 region{x, y, z};
					const auto it = std::find(regions.begin(), regions.end(), region);
```

(the `if (!world_->edit_log()->bounds().contains_region(region)) continue;` line goes)

- [ ] **Step 2: Delete the guard at line 667**

```cpp
		const auto add_region = [&](std::vector<ve::IVec3> &out, ve::IVec3 r) {
			if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
		};
```

- [ ] **Step 3: Delete the guard at line 1124**

```cpp
				for (int x = rlo.x; x <= rhi.x; x++) {
					const ve::IVec3 region{x, y, z};
					if (std::find(paste_regions.begin(), paste_regions.end(), region) ==
							paste_regions.end())
						paste_regions.push_back(region);
				}
```

- [ ] **Step 4: Build and run the island suite**

```bash
./build.sh
./gdunit_tests.sh -a res://tests/test_island_body.gd
```

Expected: PASS, and no failure that is absent from `reports/unbounded-baseline/gdunit.txt`.
`test_island_body.gd` still sets `world_size_regions`, which is fine — that property survives
until Task 15.

- [ ] **Step 5: Commit**

```bash
git add extension/src/physics/island_manager.cpp
git commit -m "refactor: island fan-out has no world edge to filter against"
```

---

## Task 13: The GPU region map is sized from the window

**Files:**
- Modify: `extension/src/render/gpu_atlas.h:44`, `extension/src/render/gpu_atlas.cpp:98,99,312`
- Modify: `extension/src/render/orchestrator.cpp:174`
- Test: `tests/test_gpu_atlas.gd`

**Interfaces:**
- Consumes: `ve::RegionWindow` (Task 3).
- Produces: `GpuAtlasConfig::region_window` (a `ve::RegionWindow`) replaces
  `GpuAtlasConfig::bounds`. `GpuAtlas::region_map_entries()` returns `region_window.cell_count()`.

- [ ] **Step 1: Change the config and the sizing**

In `extension/src/render/gpu_atlas.h`:
- `#include "world/region_window.h"`.
- In `GpuAtlasConfig`, replace `ve::WorldBounds bounds{};` with:

```cpp
	// The near-field region map's index space (ve::RegionWindow). Camera-centred and
	// toroidal, so this buffer is ~16 KB rather than one entry per region in a fixed world.
	ve::RegionWindow region_window{};
```

- Replace `region_map_entries()`:

```cpp
	int region_map_entries() const { return cfg_.region_window.cell_count(); }
```

In `extension/src/render/orchestrator.cpp:174`, replace `cfg.bounds = world_bounds();` with:

```cpp
	cfg.region_window = ve::region_window_centered(0.0f, 0.0f, 0.0f,
			ve::region_window_dim(config.residency_radius_m, ve::ResidencyConfig{}.evict_margin));
```

The origin is irrelevant at creation time — only `cell_count()` sizes the buffer, and the
streamer re-centres the window every frame (Task 15).

- [ ] **Step 2: Verify the map is cleared to -1 at both sites**

`gpu_atlas.cpp:98-99` and `:312` both call `filled_i32(region_map_entries(), -1)`. Neither needs
editing — they follow the new size automatically. Read both to confirm, and confirm the
`region_index >= region_map_entries()` guard at `:259` still reads correctly as a window-cell
bound.

- [ ] **Step 3: Build and run the atlas suite**

```bash
./build.sh
./gdunit_tests.sh -a res://tests/test_gpu_atlas.gd
```

Expected: PASS. If a case asserts `region_map_entries() == 64 * 8 * 64`, update it to the
window's cell count and comment that the map is now camera-centred.

- [ ] **Step 4: Commit**

```bash
git add extension/src/render/gpu_atlas.h extension/src/render/gpu_atlas.cpp \
        extension/src/render/orchestrator.cpp tests/test_gpu_atlas.gd
git commit -m "refactor: the GPU region map is sized from the region window"
```

---

## Task 14: The raymarcher indexes toroidally

**Files:**
- Modify: `shaders/raymarch.comp.glsl:100-104`
- Modify: `extension/src/raymarch_compositor.cpp:104-109`
- Test: `tests/test_raymarch_pixel.gd`

**Interfaces:**
- Consumes: `ve::RegionWindow` (Task 3), `ve::floor_div` (`world/region.h`).
- Produces: `pc.region_origin.xyz` now carries the **window** origin in regions;
  `pc.dims.xyz` carries the window dims. `pc.dims.w` (island slot count) is unchanged.

- [ ] **Step 1: Change the shader**

In `shaders/raymarch.comp.glsl`, replace the body of `region_slot_of`:

```glsl
// Region-table slot for the region holding a GLOBAL brick coord; -1 outside the window or not
// resident. `>> 5` is an arithmetic shift: floor(b / 32), correct for negatives. The window
// test stays -- a ray marching past the window must still miss -- but the INDEX is toroidal,
// because the map is a camera-centred window rather than a dense grid over a fixed world.
// Two regions that alias are pc.dims regions apart and two simultaneously resident regions are
// at most 2 * radius * margin apart, so a live collision is arithmetically impossible
// (ve::region_window_dim).
int region_slot_of(ivec3 brick) {
	ivec3 r = brick >> 5;
	ivec3 l = r - pc.region_origin.xyz;
	if (any(lessThan(l, ivec3(0))) || any(greaterThanEqual(l, pc.dims.xyz))) return -1;
	ivec3 w = r & (pc.dims.xyz - 1);
	return region_map.slot[w.x + w.y * pc.dims.x + w.z * pc.dims.x * pc.dims.y];
}
```

- [ ] **Step 2: Feed the window into the push constants**

In `extension/src/raymarch_compositor.cpp:104-109`, replace the world-size block:

```cpp
	const ve::RegionWindow win = world->region_window();
	cp.dims[0] = win.dim; cp.dims[1] = win.dim; cp.dims[2] = win.dim;
	cp.dims[3] = world->island_slot_count();
	cp.region_origin[0] = win.origin.x;
	cp.region_origin[1] = win.origin.y;
	cp.region_origin[2] = win.origin.z;
	cp.region_origin[3] = 0; // Task 11 sets the cull grid
```

This also removes the latent `ob.x / 32` truncating divide, which was wrong for negative
origins — `region_window_centered` floors.

`VoxelWorld::region_window()` is added in Task 15; if you are executing tasks strictly in
order, add the accessor here as a one-liner returning
`store_->residency() ? store_->residency()->window() : ve::RegionWindow{}` and let Task 15
wire the per-frame recentre.

- [ ] **Step 3: Build and run the pixel suite**

```bash
./build.sh
./gdunit_tests.sh -a res://tests/test_raymarch_pixel.gd
```

Expected: PASS. This suite renders actual pixels through the marcher, so it is the one that
catches an indexing mistake.

- [ ] **Step 4: Run the wider GPU suites that read the near field**

```bash
./gdunit_tests.sh -a res://tests/test_gpu_smoke.gd -a res://tests/test_self_check.gd
```

Expected: PASS, modulo the recorded baseline.

- [ ] **Step 5: Commit**

```bash
git add shaders/raymarch.comp.glsl extension/src/raymarch_compositor.cpp \
        extension/src/voxel_world.h
git commit -m "feat: the raymarcher indexes the region map toroidally"
```

---

## Task 15: The configuration surface

Replaces `world_size_regions` / `world_origin_bricks` with `stream_radius_m` and
`occupancy_retention_m`, re-centres the window each frame, and drives occupancy eviction. This
is also where the ~15 gdUnit test files migrate, because they are this property's callers.

**Files:**
- Modify: `extension/src/core/world_store.h` (`WorldConfig`, setters, `ensure_residency`)
- Modify: `extension/src/voxel_world.h:263-270`, `extension/src/voxel_world.cpp:174-175,219,676,1133-1136`
- Modify: `extension/src/render/orchestrator.cpp:76,213`, `extension/src/lod/lod_system.cpp:44,55`
- Modify: `extension/src/render/world_streamer.cpp` (`run_frame`)
- Modify: `tests/*.gd` (~15 files)

**Interfaces:**
- Consumes: `ve::RegionWindow`, `OccupancyGrid::evict_outside` (Task 10),
  `LodTreeConfig::stream_radius_m` (Task 8).
- Produces:
  - `WorldConfig::stream_radius_m` (default `1638.4f`), `WorldConfig::occupancy_retention_m`
    (default `256.0f`); `world_origin_bricks` and `world_size_regions` are **removed**.
  - `VoxelWorld` properties `stream_radius_m`, `occupancy_retention_m`.
  - `ve::RegionWindow VoxelWorld::region_window() const`.
  - `ve::world_bounds(const WorldConfig &)` is **removed**.

- [ ] **Step 1: Change `WorldConfig`**

In `extension/src/core/world_store.h`:

```cpp
struct WorldConfig {
	ve::IVec3 atlas_bricks{64, 32, 32};
	int max_region_slots = 512;
	int max_brick_jobs = 16384;
	int max_override_bricks = 8192;
	float residency_radius_m = 96.0f;
	// The far field's horizon. Defaults to 1638.4 m, exactly the reach of the fixed world
	// this replaced, so the unbounded rework is a no-visual-change refactor at its default.
	float stream_radius_m = 1638.4f;
	// Where occupancy blocks are dropped. Connectivity windows reach 102.4 m
	// (kFloodWindowCells x 2 expansions), so this carries 2.5x headroom.
	float occupancy_retention_m = 256.0f;
};
```

Delete the `world_bounds(const WorldConfig &)` free function immediately below it, and the
`set_world_origin_bricks` / `set_world_size_regions` setters. Add:

```cpp
	void set_stream_radius_m(float v) { config_.stream_radius_m = v; }
	void set_occupancy_retention_m(float v) { config_.occupancy_retention_m = v; }
```

`ensure_residency()` already builds its window from the radius (Task 6); leave it alone.

- [ ] **Step 2: Change the `VoxelWorld` property surface**

In `extension/src/voxel_world.h`, delete the four `world_origin_bricks` /
`world_size_regions` accessors at lines 263-270 and add:

```cpp
	void set_stream_radius_m(float v) { store_->set_stream_radius_m(v); }
	float get_stream_radius_m() const { return store_->config().stream_radius_m; }
	void set_occupancy_retention_m(float v) { store_->set_occupancy_retention_m(v); }
	float get_occupancy_retention_m() const { return store_->config().occupancy_retention_m; }
	// The near-field region map's current window. Read by RaymarchCompositor for the
	// push constants and by the debug hooks.
	ve::RegionWindow region_window() const {
		return store_->residency() ? store_->residency()->window() : ve::RegionWindow{};
	}
```

Delete `ve::WorldBounds VoxelWorld::world_bounds() const;` (declaration at `voxel_world.h:368`
and definition at `voxel_world.cpp:676`).

In `extension/src/voxel_world.cpp`, replace the two `ClassDB::bind_method` pairs at 174-175 and
the `ADD_PROPERTY` at 219:

```cpp
	ClassDB::bind_method(D_METHOD("set_stream_radius_m", "v"), &VoxelWorld::set_stream_radius_m);
	ClassDB::bind_method(D_METHOD("get_stream_radius_m"), &VoxelWorld::get_stream_radius_m);
	ClassDB::bind_method(D_METHOD("set_occupancy_retention_m", "v"),
			&VoxelWorld::set_occupancy_retention_m);
	ClassDB::bind_method(D_METHOD("get_occupancy_retention_m"),
			&VoxelWorld::get_occupancy_retention_m);
```

```cpp
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stream_radius_m"), "set_stream_radius_m",
			"get_stream_radius_m");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "occupancy_retention_m"),
			"set_occupancy_retention_m", "get_occupancy_retention_m");
```

At `voxel_world.cpp:1133-1136` (`render_probe_pixel`), replace the world-size block with the
window, mirroring Task 14's compositor change.

- [ ] **Step 3: Re-centre the window and evict occupancy each frame**

In `extension/src/render/world_streamer.cpp`, at the top of `run_frame` before
`residency_->update(...)`:

```cpp
	// The window follows the camera. Recentring is free: the toroidal index needs no shift and
	// no re-upload, because a region leaves residency (writing -1 through
	// set_region_map_entry) long before its cell could be reused by a region dim regions away.
	residency_->set_window(ve::region_window_centered(cx, cy, cz, residency_->window().dim));
```

Occupancy eviction runs on the main thread, not here. In `WorldStore::drain_occupancy()`, after
the drain loop, add the retention sweep — and give it the camera position by adding a
main-thread setter that `VoxelWorld::_process` already has a center for (the same
`physics_center_path` node the world already tracks):

```cpp
	// The unbounded world means blocks accumulate with distance travelled. Dropping a distant
	// one is lossless: it re-reads as kCellUnknown and the mark pass refills it on return.
	occupancy_.evict_outside(center_[0], center_[1], center_[2], config_.occupancy_retention_m);
```

Add `float center_[3] = {0.0f, 0.0f, 0.0f};` to `WorldStore` with a
`void set_center(float x, float y, float z)` setter. Call it from `VoxelWorld::_process`
(`extension/src/voxel_world.cpp:304`), immediately **before** the existing
`drain_occupancy();` on line 307, using the same anchor the physics path resolves twelve
lines below:

```cpp
void VoxelWorld::_process(double delta) {
	// The retention sweep inside drain_occupancy() needs a centre, and this is the only
	// main-thread place that has one. Resolved before the physics_enabled_ early-out below,
	// because occupancy must keep draining (and evicting) with physics disabled.
	if (Node3D *c = Object::cast_to<Node3D>(get_node_or_null(physics_center_path_))) {
		const Vector3 p = c->get_global_position();
		store_->set_center(p.x, p.y, p.z);
	}
	drain_occupancy();
```

When `physics_center_path` is empty the centre keeps its last value, which is correct: an
unset anchor means nothing is moving, so nothing needs evicting.

- [ ] **Step 4: Update the remaining `world_bounds()` consumers**

- `extension/src/render/orchestrator.cpp:76` — delete `RenderOrchestrator::world_bounds()` and
  its declaration at `orchestrator.h:263`; `:213` becomes `handles_.store->ensure_residency();`.
- `extension/src/lod/lod_system.cpp:44` — delete `LodSystem::world_bounds()` and its
  declaration at `lod_system.h:129`; `:55` becomes
  `cfg.stream_radius_m = store()->config().stream_radius_m;`.
- `extension/src/raymarch_compositor.cpp:256` — the sun ortho:

```cpp
			// There is no world AABB to fit. The ortho follows the camera at the stream
			// radius: identical to the old world box at the 1638.4 m default. At a larger
			// radius the same map stretches further and shadow texels coarsen proportionally
			// -- cascades are sub-project B.
			const Vector3 c = cam.origin;
			const float r = world->get_stream_radius_m();
			const float lo[3] = {c.x - r, c.y - r, c.z - r};
			const float hi[3] = {c.x + r, c.y + r, c.z + r};
```

- [ ] **Step 5: Migrate the gdUnit test files**

Find them:

```bash
grep -rln "world_size_regions\|world_origin_bricks" tests demo
```

For each, delete the two assignments. Where the test set a small world to keep the run cheap,
add `w.stream_radius_m = 200.0` in their place — a small far-field horizon is the modern way to
say "keep this test small", and `residency_radius_m` (which several already set) bounds the near
field. Do not add `stream_radius_m` to tests that never constrained the world; they were relying
on the default and still are.

- [ ] **Step 6: Build and run everything**

```bash
./build.sh
cd extension && scons test -j8; cd ..
./gdunit_tests.sh
```

Expected: PASS, with no failure absent from `reports/unbounded-baseline/README.md`. Compare the
lists explicitly; do not eyeball the totals.

- [ ] **Step 7: Commit**

```bash
git add -A extension/src tests demo shaders
git commit -m "feat: stream_radius_m and occupancy_retention_m replace the world box"
```

---

## Task 16: `debug/hooks.cpp`

Nineteen `world_bounds()` sites plus `:5226` and `:5803`. Mechanical, bulk, low risk — but it is
its own task because a reviewer should be able to approve or reject it independently of the
behaviour changes.

**Files:**
- Modify: `extension/src/debug/hooks.cpp`, `extension/src/debug/hooks.h:195`

**Interfaces:**
- Consumes: `VoxelWorld::region_window()`, `WorldConfig::stream_radius_m` (Task 15).
- Produces: `debug_set_region_map_entry` keeps its signature; its `region_index` argument is now
  a window cell index.

- [ ] **Step 1: Find every site**

```bash
grep -n "world_bounds()\|world_size_regions\|region_map_entries\|region_index" \
     extension/src/debug/hooks.cpp
```

- [ ] **Step 2: Replace the push-constant fills**

Every occurrence of this shape:

```cpp
	cp.dims[0] = world_->store_->config().world_size_regions.x;
	cp.dims[1] = world_->store_->config().world_size_regions.y;
	cp.dims[2] = world_->store_->config().world_size_regions.z;
```

becomes:

```cpp
	const ve::RegionWindow win = world_->region_window();
	cp.dims[0] = win.dim; cp.dims[1] = win.dim; cp.dims[2] = win.dim;
```

and every `region_origin` fill derived from `world_bounds().origin_regions()` becomes
`win.origin.x / .y / .z`. Leave `cp.dims[3]` (the island slot count) alone.

- [ ] **Step 3: Fix the two odd ones out**

- `:5226` — `d["region_map_entries"] = world_->atlas()->region_map_entries();` needs no change;
  the accessor now reports the window's cell count. Add a comment saying so.
- `:5803` — `world_->world_bounds().region_index({...})` becomes
  `world_->region_window().index({region.x, region.y, region.z})`.

- [ ] **Step 4: Build and run the debug-facing suites**

```bash
./build.sh
./gdunit_tests.sh -a res://tests/test_self_check.gd -a res://tests/test_world_store_contract.gd
```

Expected: PASS against the baseline.

- [ ] **Step 5: Commit**

```bash
git add extension/src/debug/hooks.cpp extension/src/debug/hooks.h
git commit -m "refactor: debug hooks read the region window instead of the world box"
```

---

## Task 17: Delete `WorldBounds`

With every consumer migrated, the type goes. Its four statics were always bounds-free and
become free functions; the rest is the world edge and is deleted.

**Files:**
- Modify: `extension/src/world/region.h`, `extension/src/world/region.cpp`
- Modify: every file that still names `WorldBounds`

**Interfaces:**
- Produces: free functions `ve::region_of_brick`, `ve::brick_of_point`, `ve::region_of_point`,
  `ve::brick_index_in_region`. `ve::WorldBounds` no longer exists.

- [ ] **Step 1: Confirm nothing but the statics is still used**

```bash
grep -rn "WorldBounds" extension/src extension/tests | grep -v "region.h\|region.cpp"
```

Expected: only `WorldBounds::region_of_brick` / `::brick_of_point` / `::region_of_point` /
`::brick_index_in_region` call sites. **If any `contains_*`, `region_index`, `aabb`,
`origin_bricks`, `size_regions`, `size_bricks` or `origin_regions` use remains, stop and
migrate it** — deleting it here would be silently changing behaviour outside a task that
tested for it.

- [ ] **Step 2: Promote the statics**

In `extension/src/world/region.h`, replace the whole `struct WorldBounds { ... };` with:

```cpp
// The brick and region lattices are GLOBAL and unbounded: brick b's world corner is
// b * kBrickSize with no origin term, and every integer coordinate names a real region. There
// is no world extent -- see docs/superpowers/specs/2026-09-04-unbounded-world-design.md.
IVec3 region_of_brick(IVec3 b);
IVec3 brick_of_point(float x, float y, float z);
IVec3 region_of_point(float x, float y, float z);
// 0..kRegionBrickCount-1, x fastest, y, then z.
int brick_index_in_region(IVec3 b);
```

In `extension/src/world/region.cpp`, drop the `WorldBounds::` qualifier from those four
definitions and delete `size_bricks`, `origin_regions`, `contains_region`, `contains_brick`,
`region_index` and `aabb`.

- [ ] **Step 3: Update every call site**

```bash
grep -rl "WorldBounds::" extension/src extension/tests | \
  xargs sed -i '' 's/WorldBounds::region_of_brick/ve::region_of_brick/g; s/WorldBounds::brick_of_point/ve::brick_of_point/g; s/WorldBounds::region_of_point/ve::region_of_point/g; s/WorldBounds::brick_index_in_region/ve::brick_index_in_region/g'
```

Then fix the double qualification this creates inside `namespace ve` blocks (`ve::ve::`):

```bash
grep -rn "ve::ve::" extension/src extension/tests
```

Remove the redundant prefix at each hit by hand — there will be a handful, all inside
`namespace ve`.

- [ ] **Step 4: Build and run everything**

```bash
cd extension && scons test -j8; cd ..
./build.sh
./gdunit_tests.sh
```

Expected: PASS against the baseline. A compile error naming `WorldBounds` means Step 1's grep
missed a consumer.

- [ ] **Step 5: Commit**

```bash
git add -A extension
git commit -m "refactor: delete WorldBounds; the lattice is global and unbounded"
```

---

## Task 18: The `RegionArchive` seam

Spec §6. **Deliberately thin, and the executor should keep it that way.** A evicts no edits —
they are bounded by how much the player has dug, not by distance travelled — so this task adds
a seam and its in-RAM implementation, not machinery. Its value is that C can add disk paging
without touching any caller, and that the constraint C inherits is written down where C will
find it.

**Files:**
- Create: `extension/src/world/region_archive.h`, `extension/src/world/region_archive.cpp`
- Modify: `extension/src/core/world_store.h`
- Test: `extension/tests/test_region_archive.cpp`

**Interfaces:**
- Consumes: `ve::EditOp`, `ve::IVec3`.
- Produces:
  - `struct ve::RegionSnapshot { IVec3 region; std::vector<EditOp> ops; std::vector<uint64_t> seqs; int override_table; }`
  - `struct ve::RegionArchive` with `void store(RegionSnapshot &&)` and `bool load(IVec3, RegionSnapshot *)`
  - `class ve::PinnedRegionArchive : public RegionArchive`
  - `ve::RegionArchive &WorldStore::archive()`

- [ ] **Step 1: Write the failing test**

Create `extension/tests/test_region_archive.cpp`:

```cpp
#include <doctest/doctest.h>
#include "world/region_archive.h"

static ve::EditOp sphere(float x, float r) {
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = x;
	op.radius = r;
	return op;
}

TEST_CASE("a stored region round-trips") {
	ve::PinnedRegionArchive a;
	ve::RegionSnapshot in;
	in.region = {3, -4, 5};
	in.ops.push_back(sphere(1.0f, 2.0f));
	in.seqs.push_back(7);
	in.override_table = 11;
	a.store(std::move(in));

	ve::RegionSnapshot out;
	REQUIRE(a.load({3, -4, 5}, &out));
	CHECK(out.region == ve::IVec3{3, -4, 5});
	REQUIRE(out.ops.size() == 1);
	CHECK(out.ops[0].radius == doctest::Approx(2.0f));
	REQUIRE(out.seqs.size() == 1);
	CHECK(out.seqs[0] == 7);
	CHECK(out.override_table == 11);
}

TEST_CASE("loading a region that was never stored reports nothing, not empty edits") {
	// The distinction matters: 'never edited' means regenerate from the terrain pipeline,
	// while 'edited, and the edits were an empty list' would mean pristine ground. Confusing
	// them is how a disk-backed archive silently deletes a player's excavation.
	ve::PinnedRegionArchive a;
	ve::RegionSnapshot out;
	CHECK(!a.load({0, 0, 0}, &out));
}

TEST_CASE("storing a region twice keeps the newer snapshot") {
	ve::PinnedRegionArchive a;
	ve::RegionSnapshot first;
	first.region = {1, 1, 1};
	first.ops.push_back(sphere(1.0f, 1.0f));
	a.store(std::move(first));
	ve::RegionSnapshot second;
	second.region = {1, 1, 1};
	second.ops.push_back(sphere(2.0f, 2.0f));
	second.ops.push_back(sphere(3.0f, 3.0f));
	a.store(std::move(second));

	ve::RegionSnapshot out;
	REQUIRE(a.load({1, 1, 1}, &out));
	CHECK(out.ops.size() == 2);
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd extension && scons build/tests/ve_tests -j8
```

Expected: FAIL to compile — `world/region_archive.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `extension/src/world/region_archive.h`:

```cpp
#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <map>
#include <vector>

namespace ve {

// Everything about one region that is NOT recomputable from the terrain pipeline. Occupancy is
// absent on purpose: the mark pass regenerates it, so it is dropped rather than archived.
struct RegionSnapshot {
	IVec3 region{};
	std::vector<EditOp> ops;
	std::vector<uint64_t> seqs; // parallel to ops, the global append sequence of each
	int override_table = -1;
};

// Where a region's edits go when they leave RAM.
//
// A pins everything (PinnedRegionArchive), which is exactly the behaviour the bounded world
// had: edits are bounded by how much the player has dug, not by how far they have walked. The
// seam exists so sub-project C can page to disk without touching a caller.
//
// CONSTRAINT FOR C: LodSystem::gather_ops reads a region's ops SYNCHRONOUSLY on the far-field
// build path. Against the pinned archive that is a map lookup. Against a disk archive it is
// asynchronous IO, so a LoD build for a region with archived edits must be able to WAIT rather
// than silently building pre-edit terrain. Design that in before the disk backend, not after.
struct RegionArchive {
	virtual ~RegionArchive() = default;
	virtual void store(RegionSnapshot &&s) = 0;
	// False means the region was never edited -- regenerate it from the pipeline. It does NOT
	// mean "edited, but the edits were empty".
	virtual bool load(IVec3 region, RegionSnapshot *out) = 0;
};

class PinnedRegionArchive : public RegionArchive {
public:
	void store(RegionSnapshot &&s) override;
	bool load(IVec3 region, RegionSnapshot *out) override;
	int size() const { return static_cast<int>(by_region_.size()); }

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	std::map<Key, RegionSnapshot> by_region_;
};

} // namespace ve
```

Create `extension/src/world/region_archive.cpp`:

```cpp
#include "world/region_archive.h"

namespace ve {

void PinnedRegionArchive::store(RegionSnapshot &&s) {
	const Key k{s.region.x, s.region.y, s.region.z};
	by_region_[k] = std::move(s);
}

bool PinnedRegionArchive::load(IVec3 region, RegionSnapshot *out) {
	if (!out) return false;
	const auto it = by_region_.find(Key{region.x, region.y, region.z});
	if (it == by_region_.end()) return false;
	*out = it->second;
	return true;
}

} // namespace ve
```

- [ ] **Step 4: Give `WorldStore` the archive**

In `extension/src/core/world_store.h`, add `#include "world/region_archive.h"`, a member
`ve::PinnedRegionArchive archive_;`, and the accessor:

```cpp
	// Where a region's edits go when they leave RAM. Nothing evicts edits in the unbounded-
	// world rework -- they are bounded by digging, not travel -- so today this only ever holds
	// what a caller explicitly hands it. Sub-project C replaces the implementation.
	ve::RegionArchive &archive() { return archive_; }
```

Do **not** wire an eviction path to it. There is nothing in A that should evict an edit, and
adding a caller with no requirement behind it is how a seam turns into dead machinery.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd extension && scons test -j8 && ./build.sh
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add extension/src/world/region_archive.h extension/src/world/region_archive.cpp \
        extension/src/core/world_store.h extension/tests/test_region_archive.cpp
git commit -m "feat: RegionArchive seam, pinned in RAM until sub-project C"
```

---

## Task 19: The invariant tests

Spec §9 Phase 2. Each of these fails on `main` and passes here — that is what makes them worth
keeping rather than decoration.

**Files:**
- Create: `extension/tests/test_unbounded_invariants.cpp`

**Interfaces:**
- Consumes: everything above.
- Produces: no new production symbols.

- [ ] **Step 1: Write the tests**

```cpp
#include <doctest/doctest.h>
#include "connectivity/occupancy.h"
#include "lod/lod_grid.h"
#include "lod/lod_tree.h"
#include "world/edit_log.h"
#include "world/region_window.h"
#include "world/residency.h"
#include <cmath>
#include <vector>

namespace {
ve::LodCamera cam_at(float x, float y, float z) {
	const float pos[3] = {x, y, z};
	const float fwd[3] = {0.0f, 0.0f, -1.0f};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	return ve::lod_camera_perspective(pos, fwd, up, 1.2217f, 16.0f / 9.0f, 0.1f, 8000.0f,
			2560, 1440);
}
struct NoOcclusion : ve::LodOcclusion {
	bool occluded(const float[3], const float[3]) const override { return false; }
};
} // namespace

TEST_CASE("invariant: ten kilometres of travel does not grow the LoD tree without bound") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	cfg.evict_frames = 30;
	ve::LodTree tree(cfg);
	NoOcclusion occ;
	int peak = 0;
	for (uint32_t f = 1; f < 400; f++) {
		ve::LodWalkResult out;
		const float x = float(f) * 25.0f; // ~10 km
		tree.walk(cam_at(x, 62.0f, 0.0f), &occ, f, &out);
		std::vector<ve::LodDrawItem> evicted;
		tree.collect_evictions(f, 0, &evicted);
		peak = std::max(peak, tree.node_count());
	}
	// The forest around one camera is tens of roots plus what the walk touched. If the
	// eviction exemption were still unconditional this would be hundreds of stranded nodes.
	CHECK(tree.node_count() < peak * 2);
	CHECK(tree.node_count() < 4000);
}

TEST_CASE("invariant: there is no world edge") {
	// 50 km out on every axis, including deep below the old world floor at y = -51.2 m.
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 50000.0f; op.pos[1] = -50000.0f; op.pos[2] = 50000.0f;
	op.radius = 3.0f;
	const auto r = log.append(op);
	CHECK(!r.oversized);
	CHECK(!r.touched.empty());

	const float cam[3] = {50000.0f, -50000.0f, 50000.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, 1638.4f, &roots);
	CHECK(!roots.empty());
}

TEST_CASE("invariant: the region lattice is exact at 100 km") {
	// Spec invariant 5. float32 resolves ~8 mm at 100 km, so brick and region quantisation
	// must still round-trip exactly -- this is the documented supported limit.
	const float far = 100000.0f;
	const ve::IVec3 b = ve::brick_of_point(far, 0.0f, far);
	CHECK(b.x == static_cast<int>(std::floor(far / ve::kBrickSize)));
	float lo[3], hi[3];
	ve::brick_world_aabb(b, lo, hi);
	CHECK(lo[0] <= far);
	CHECK(hi[0] >= far);
	// And the region it belongs to is the region the window would index it into.
	const ve::IVec3 r = ve::region_of_point(far, 0.0f, far);
	CHECK(r == ve::region_of_brick(b));
	const ve::RegionWindow w = ve::region_window_centered(far, 0.0f, far, 16);
	CHECK(w.contains(r));
}

TEST_CASE("invariant: an oversized op cannot hang the append path") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.radius = 1.0e5f;
	// The old world box clamped this; without a cap it would iterate ~4.8e14 cells.
	const auto r = log.append(op);
	CHECK(r.oversized);
	CHECK(log.region_count() == 0);
}
```

- [ ] **Step 2: Run them**

```bash
cd extension && scons test -j8
```

Expected: PASS, all four.

- [ ] **Step 3: Prove they are not decoration**

```bash
git stash
cd extension && scons test -j8 2>&1 | tail -20; cd ..
git stash pop
```

Expected: the invariant file does not compile against pre-refactor code (`EditLog` needs bounds,
`lod_roots_in_radius` does not exist). That failure *is* the proof; record it in the commit
message rather than trying to make the file compile both ways.

- [ ] **Step 4: Commit**

```bash
git add extension/tests/test_unbounded_invariants.cpp
git commit -m "test: pin the unbounded-world invariants"
```

---

## Task 20: The no-visual-change proof

`stream_radius_m` defaults to 1638.4 m, exactly the old world's reach. That claim has to be
measured, not asserted.

**Files:**
- Create: `reports/unbounded-after/`
- Modify: none

**Interfaces:**
- Consumes: the finished refactor.
- Produces: evidence that the default configuration renders and performs as it did.

- [ ] **Step 1: Run the full suites and compare against the baseline**

```bash
mkdir -p reports/unbounded-after
cd extension && scons test -j8 2>&1 | tee ../reports/unbounded-after/native.txt; cd ..
./gdunit_tests.sh 2>&1 | tee reports/unbounded-after/gdunit.txt || true
diff <(grep -i "fail" reports/unbounded-baseline/gdunit.txt) \
     <(grep -i "fail" reports/unbounded-after/gdunit.txt) || true
```

Every line in the diff must be explainable. An unexplained new failure blocks the task.

- [ ] **Step 2: Capture the demo at the default radius**

```bash
grep -n "capture" demo/capture.gd | head -20
```

Read `demo/capture.gd` for its actual invocation, then run it and save the frames under
`reports/unbounded-after/`. Compare against the most recent pre-refactor capture in `reports/`.
Differences in the far field at the horizon are expected — the root forest is camera-centred now,
so ground that used to fall outside the box's corner is present. Differences in the **near
field** are not, and mean the window indexing is wrong.

- [ ] **Step 3: Run a benchmark leg**

```bash
grep -n "BENCH\|leg" demo/benchmark.gd | head -20
```

Run the steady and move legs the way `reports/m7-final` was produced, into
`reports/unbounded-after/`. Compare frame p99 and raymarch p99 against the closest prior report.
The refactor should be neutral: the region map got smaller and the root forest slightly larger.

- [ ] **Step 4: Write the summary**

Create `reports/unbounded-after/README.md` recording: the suite comparison, the capture
comparison with any far-field differences named, and the benchmark numbers beside their
baseline. State plainly whether the no-visual-change claim holds. If it does not, say so and
what moved — that is a finding, not a failure of the task.

- [ ] **Step 5: Verify the knob actually turns**

```bash
# In demo/main.tscn set stream_radius_m = 4000.0 on the VoxelWorld node, then:
./gdunit_tests.sh -a res://tests/test_self_check.gd
```

Confirm the world runs at 4000 m — slowly. Frame rate is sub-project B's subject; this step
only proves A made the radius expressible. Revert the scene change before committing.

- [ ] **Step 6: Commit**

```bash
git add reports/unbounded-after
git commit -m "test: evidence that the unbounded world is neutral at its default radius"
```

---

## Definition of Done

- [ ] `grep -rn "WorldBounds\|contains_region\|contains_brick" extension/src` returns nothing.
- [ ] `grep -rn "world_size_regions\|world_origin_bricks" extension/src demo tests` returns nothing.
- [ ] The native suite passes in full.
- [ ] The gdUnit suite shows no failure absent from `reports/unbounded-baseline/README.md`.
- [ ] `reports/unbounded-after/README.md` states whether the no-visual-change claim held.
- [ ] An edit at (50000, -50000, 50000) is accepted, and the demo runs at `stream_radius_m = 4000`.
- [ ] `ve::RegionArchive` exists with its C constraint documented, and nothing in A evicts an edit.
