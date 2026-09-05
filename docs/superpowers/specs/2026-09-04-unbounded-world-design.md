# Unbounded World — Design

**Date:** 2026-09-04
**Status:** Approved design, pre-implementation
**Scope:** Sub-project A of three. Removes the world edge: `WorldBounds` stops being a
membership test, the near-field region map becomes a camera-centred window, and the LoD
octree becomes a moving forest of roots with distance-based residency. The world becomes
unbounded on all three axes.
**Supersedes:** the fixed-extent world assumption in `2026-08-12-voxel-engine-design.md` §2
and the `4096 x 1024 x 4096 m` / `3x1x3 L7 root grid` statement in
`2026-08-17-m5-lod-design.md` §2. The M5 level table itself is unchanged and correct.

---

## 1. The problem

The world is a fixed box. `WorldConfig::world_size_regions` defaults to `{64, 8, 64}`; a
region is `kRegionBricks` (32) x `kBrickSize` (0.8 m) = 25.6 m, so the box is
**1638.4 x 204.8 x 1638.4 m** with its origin at y = -51.2 m. The demo scene uses the
defaults.

The LoD level table was sized to that box: 8 levels, 32 cells per chunk, 0.4 m base cell,
so an L7 chunk is exactly 1638.4 m — one octree root spanning the whole world in XZ. This
is what "merely a single octree" means in practice.

The level table is not the limit. M5's own table has L5 (12.8 m cells) reaching 5 km at the
3 px error threshold, and M5 was written for a 4096 m world. The box shrank; the levels did
not. What actually stands between the engine and a 4 km horizon is the world edge itself,
the structures that grow without bound once that edge is removed, and the far-field frame
budget.

## 2. Decomposition

The full request — unbounded streaming, 4000 m+ view distance, and disk persistence — is
three subsystems. They are specified and implemented separately.

| | Sub-project | Depends on |
|---|---|---|
| **A** | **Unbounded world core** (this document) | — |
| B | 4 km view distance: root-forest radius, `LodPool::kChunkRecords` and page-arena budgets, build throughput, sun-shadow cascades, horizon fade, frame budget | A |
| C | Disk persistence: region file format, async IO, load-on-approach / save-on-evict, versioning, crash safety | A |

A defines the eviction seam C implements (§6), and turns the world edge into the stream
radius B tunes (§5). A ships with `stream_radius_m` defaulting to today's reach, so A is a
no-visual-change refactor on an unbounded substrate; the knob accepts 4000+ and works, but
the frame budget at that radius is B's subject.

## 3. The coordinate and ownership model

`WorldBounds` conflates three unrelated jobs. It splits along a seam that already exists in
the type: its four **static** members (`region_of_brick`, `brick_of_point`,
`region_of_point`, `brick_index_in_region`) take no bounds and are pure lattice math. They
become free functions in `world/region.h` and every caller is unaffected.

Its **instance** members — `origin_bricks`, `size_regions`, `size_bricks`,
`origin_regions`, `contains_region`, `contains_brick`, `region_index`, `aabb` — are the
world edge. They are deleted, and two named concepts take over the jobs worth keeping:

- **`RegionWindow`** — the near-field data plane's index space. Camera-centred,
  power-of-two, toroidal. Derived from `residency_radius_m`, not configured separately (§4).
- **`StreamRadius`** — the far field's extent. Feeds LoD root selection, LoD node
  residency, and the sun ortho fit (§5).

Nothing replaces membership. **The world has no edge**, and no code may ask whether a
region is inside it.

### Invariants

1. **Every region coordinate is valid.** `contains_region` and `contains_brick` have no
   successor. A guard that reads "is this in the world" is a bug in the new model.
2. **A region's identity is its global `IVec3`, forever.** Already true: `EditLog::lists_`,
   `OccupancyGrid::blocks_`, `LodTree::nodes_` and `RegionResidency::by_region_` are all
   globally keyed sparse maps. No index space is ever rebased.
3. **No two simultaneously-resident regions share a window cell.** Enforced by construction
   through the window sizing formula in §4, asserted at initialisation, and tested.
4. **Every per-region and per-node structure has an eviction rule** keyed on distance from
   the camera or on age. Two structures violate this today and would grow forever as the
   player travels: `OccupancyGrid` (its own comment: "never evicted ... for as long as the
   world lives") and `LodTree` nodes at levels >= `kLodResidentLevelFrom`, which
   `collect_evictions` skips outright at `lod_tree.cpp:486` and therefore never erases.
5. **World coordinates stay float32 in one global frame, supported to +/-100 km.** At 100 km
   float32 resolves ~8 mm, which is fine against 5 cm voxels; at 1000 km it degrades to
   ~6 cm and the SDF visibly breaks. int32 brick coordinates are not the binding constraint
   (they reach 1.7e9 m). The limit is documented and tested at its boundary. Floating-origin
   rebasing is a deliberate non-goal (§10).

## 4. The near-field data plane: `RegionWindow`

The near field only reaches as far as the brick atlas can pay for — `residency_radius_m`
defaults to 96 m and `ResidencyConfig::evict_margin` to 1.15, so a resident region is at
most 110.4 m from the camera. The region map therefore does not need to span the world; it
needs to span residency. Deriving its size from residency is also what makes invariant 3
hold by construction:

```
window_dims = next_pow2( ceil(2 * residency_radius_m * evict_margin / kRegionSize) + 1 )
```

At the defaults: `2 * 96 * 1.15 = 220.8 m` of resident span, `220.8 / 25.6 = 8.63`,
`ceil(8.63) + 1 = 10`, `next_pow2(10) = 16` regions per axis = **409.6 m**, so
**4096 entries x 4 B = 16 KB**, down from today's dense `64 x 8 x 64 x 4 B = 128 KB`. Two
regions that collide in the toroidal grid are 409.6 m apart; two simultaneously *resident*
regions are at most 220.8 m apart; a collision between two live entries is therefore
arithmetically impossible. `residency_radius_m` is validated against this at initialisation
rather than trusted.

Because no live entry can ever alias, the window needs **no companion coordinate buffer, no
stale-entry sweep when it moves, and no re-upload on recentre**. Eviction already writes -1
through `WorldStreamer`'s existing `set_region_map_entry(rd, e.map_index, -1)` long before a
cell could be reused.

### Shader change

One line in the DDA hot loop. `raymarch.comp.glsl:100` keeps its window test verbatim — it
is still needed to reject rays that march past the window — and changes only how it indexes:

```glsl
int region_slot_of(ivec3 brick) {
    ivec3 r = brick >> 5;
    ivec3 l = r - pc.region_origin.xyz;   // now the WINDOW origin, recomputed per frame
    if (any(lessThan(l, ivec3(0))) || any(greaterThanEqual(l, pc.dims.xyz))) return -1;
    ivec3 w = r & (pc.dims.xyz - 1);      // was: l
    return region_map.slot[w.x + w.y * pc.dims.x + w.z * pc.dims.x * pc.dims.y];
}
```

No extra memory traffic and no second dependent load. `pc.region_origin` and `pc.dims`
already exist as push constants; they are fed from the window instead of from
`world_size_regions`.

### CPU change

`ResidencyPlan::Entry::map_index` stops being `bounds.region_index(region)` — which
returned -1 outside the world — and becomes `window.index(region)`, a total function.
`world_streamer.cpp:378,399` are unchanged.

One latent bug is fixed on the way: `raymarch_compositor.cpp:107` computes
`cp.region_origin[0] = ob.x / 32`, a truncating divide that is wrong for negative origins.
It becomes `ve::floor_div`.

## 5. The far field

### Root selection

`lod_root_range(bounds)` becomes `lod_roots_in_radius(cam_pos, R)`: the L7 chunks
(1638.4 m) intersecting a sphere of radius `R` around the camera. Root count is bounded by
`(ceil(2R / 1638.4) + 1)^3` candidates, of which the sphere test keeps roughly half:
**27 candidates (~14 kept) at R = 1638 m, 216 candidates (~113 kept) at R = 4000 m**.

Infinite Y costs far less than it appears to, because the octree already prunes air
correctly. `visit()` treats `kLodEmpty` as terminal (`lod_tree.cpp:346-351`) and descends
only into nodes that are `kLodReady`, so an air chunk costs exactly one build to discover
and then prunes its entire subtree. Air discovery is bounded by 8x the surface chunk count,
not by volume. The extra Y root layers — 3 at R = 1638 m, 6 at R = 4000 m — are almost all
air, one build each.

### Node lifetime

`collect_evictions` currently skips every node at level >= `resident_level_from` as an
eviction candidate (`lod_tree.cpp:486`), which is why `nodes_` grows forever as the player
travels. The exemption becomes distance-gated:

```cpp
const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
if (kv.first.level >= cfg_.resident_level_from &&
    lod_chunk_distance(kv.first.level, c, last_cam_pos_) <= cfg_.stream_radius_m)
    continue;
```

No signature change: `last_cam_pos_` is already refreshed on every walk. Beyond the radius,
coarse nodes become ordinary age-based candidates and `lod_tree.cpp:503` erases them. The
existing `kLodEvictFrames` (300) rule handles everything else, because `visit()` marks only
what it touches and out-of-radius roots are never visited.

### Sun ortho

`raymarch_compositor.cpp:256` fits the shadow ortho to `world_bounds().aabb()`. With no
world AABB it fits a camera-centred box of `stream_radius_m` instead — identical at
1638.4 m, so A introduces no shadow regression at its default.

**Known consequence:** at R = 4000 m that ortho stretches one `SunShadowPass::kSize` map
over 8 km and shadow texels get roughly 2.4x coarser. Cascades are B's subject. A records
this rather than pretending the knob is free.

## 6. State lifetime

Separating structures that grow with *travel* from those that grow with *digging* is what
keeps A small:

| Structure | Grows with travel? | Rule |
|---|---|---|
| `OccupancyGrid` blocks (8 KB/region) | **Yes** — roughly 92 MB per 4 km walked | Drop outside a retention radius; regenerate from the mark pass on return |
| `LodTree::nodes_`, levels >= 5 | **Yes** — never evicted today | Distance-gated exemption (§5) |
| `EditLog` ops, `OverrideStore` | No — bounded by how much the player has dug | Pinned in RAM behind the archive port |
| `RegionResidency`, region window | No — fixed slot pools | Unchanged |

The only genuinely new eviction A must build is **occupancy**, and it needs no archive: a
region's occupancy is recomputable from the mark pass against edits and overrides, so
dropping it is lossless.

The retention radius is set by connectivity reach. `kFloodWindowCells` is 64 cells x 0.8 m
= 51.2 m, up to 102.4 m with `kMaxWindowExpansions = 1`, so **256 m** covers it with 2.5x
headroom. The number is a starting point to be measured, not a constant to be trusted.
`OccupancyGrid`'s "never evicted ... for as long as the world lives" comment becomes false
and is rewritten to state the new contract.

Edits are the interesting case. They are needed out to the LoD horizon and beyond — an edit
5 km away still exists when the player walks back — but they are small: `kMaxRegionOps` caps
a region at 256 ops, so total RAM is a function of digging, not distance. A pins them all,
which is exactly today's behaviour, but behind the port C will implement:

```cpp
struct RegionArchive {
    virtual ~RegionArchive() = default;
    virtual void store(ve::IVec3 region, RegionSnapshot &&s) = 0;
    virtual bool load(ve::IVec3 region, RegionSnapshot *out) = 0;  // false = never edited
};
```

A ships `PinnedRegionArchive`, an in-RAM map. C ships `DiskRegionArchive` with no change to
callers.

**A constraint A records for C to inherit:** `LodSystem::gather_ops` reads a chunk's region
ops synchronously during a LoD build. Under `PinnedRegionArchive` that is a map lookup and
free. Under a disk archive it is asynchronous IO on the far-field build path, so a LoD build
for a region with archived edits must be able to wait. Recording it now costs a paragraph;
discovering it during C costs a redesign.

**Threading:** occupancy is main-thread-only, `EditLog` is guarded by
`WorldStore::edit_mutex()`, and residency plans run on the render thread. Eviction therefore
runs on the main thread from the same pump as `drain_occupancy()`, under `edit_mutex()`,
preserving the existing `edit_mutex -> LodSystem::mutex` lock order documented in
`world_store.h`.

### Three nested radii

| Radius | Default | Set by |
|---|---|---|
| Residency | 110 m (96 m x 1.15) | What the brick atlas can pay for |
| Occupancy retention | 256 m | Connectivity window reach (102.4 m) plus headroom |
| Stream radius | 1638.4 m | The LoD horizon; B raises it |

## 7. Guard audit

The audit's most important finding: **the world edge was silently providing a DoS guard.**
`edit_log.cpp:16-25` clamps an op's region range to the world extent before looping, and its
comment records why — the earlier per-cell `contains_region` version let a hostile radius
(~1e5 m) iterate 4.8e14 cells and freeze for minutes. Delete the bounds and that clamp has
nothing to clamp against. A adds an explicit **`kMaxOpRegionSpan`** cap that rejects
oversized ops with the same fail-soft warn-and-no-op an over-full region already gets. The
same cap covers `collect_ops_for_aabb` and the three `op_region_range` loops in the island
manager.

| Site | Today | Becomes |
|---|---|---|
| `edit_log.cpp:16` | Bounds clamp, doubling as a DoS guard | **`kMaxOpRegionSpan` cap**, fail-soft reject |
| `edit_log.h:107` | `contains_region` filter | Delete; the extent cap bounds the loop |
| `island_manager.cpp:546,667,1124` | 3x `contains_region` | Delete — every region exists |
| `chunk_residency.cpp:184` | `contains_brick` | Delete; `chunk_distance(c, ...) > r` on the next line already bounds it |
| `lod_tree.cpp:259` | Out-of-bounds child counts as "done" | Delete; all eight children always count, which is strictly more correct |
| `lod_tree.cpp:269,304,328` | `lod_chunk_in_bounds` | Stream-radius test |
| `lod_tree.cpp:390` | `lod_root_range(bounds)` | `lod_roots_in_radius(cam, R)` |
| `residency.cpp:53,173` | `bounds.region_index` (-1 outside) | `window.index` (total function) |
| `gpu_atlas.cpp:98,99,258,312`, `gpu_atlas.h:44` | `region_map_entries()` from world size | From window dims; the range guard stays as a window-cell bound |
| `raymarch_compositor.cpp:104-109` | dims/origin from world size; `ob.x / 32` | From the window; `floor_div` |
| `raymarch_compositor.cpp:256` | Sun ortho from world AABB | Camera-centred stream-radius box |
| `voxel_world.cpp` x4, `orchestrator.cpp` x4, `lod_system.cpp` x2 | `world_bounds()` projections | Window plus stream radius; `ensure_edit_log` / `ensure_residency` signatures change |
| `debug/hooks.cpp` x19, plus `:5226` and `:5803` | `world_bounds()`, `region_index`, `region_map_entries` | Mechanical push-constant fills — bulk, low risk |
| `voxel_world.cpp:219` and ~15 test files | `world_size_regions`, `world_origin_bricks` | `stream_radius_m` |

## 8. Configuration surface and migration

`world_size_regions` and `world_origin_bricks` are `ClassDB` properties bound at
`voxel_world.cpp:174-175,219`. They are removed and replaced by:

| Property | Default | Meaning |
|---|---|---|
| `stream_radius_m` | 1638.4 | The LoD horizon: root selection, node residency, sun ortho fit |
| `occupancy_retention_m` | 256.0 | Where occupancy blocks are dropped |
| `residency_radius_m` | 96.0 | Unchanged; now also derives the region window |

Roughly fifteen test files set `world_size_regions` / `world_origin_bricks`, almost always
to shrink the world so a test runs quickly. An unbounded world cannot be shrunk that way,
so those tests migrate to `stream_radius_m` (and, where they were bounding the near field,
to `residency_radius_m`, which several already set). This migration is real work inside A,
not a footnote.

## 9. Testing

**Phase 0 — baseline.** Run the full gdUnit suite on clean `main` and record the result
before anything moves. Some assertions are known to fail there already; without the
baseline, every post-refactor failure is unattributable.

**Phase 1 — characterization, before any code moves.** This refactor changes the *meaning*
of roughly forty call sites, which is exactly the shape that needs behaviour pinned first:

- The LoD walk's draw cut (level and coordinate list) for a fixed camera — catches any
  root-selection or descent regression.
- Region-map contents after a scripted camera path — pins the residency-to-map-index
  mapping across the window switch.
- Edit fan-out near the old world edge: the `touched` and `rejected` region lists.
- The existing `tests/golden` field and brick-eval corpora stay untouched; they are the
  proof the generator did not move.

**Phase 2 — new invariants** (each fails before the change and passes after):

- **Window aliasing.** Across a long camera path, no two resident regions share a toroidal
  cell. For a sweep of `residency_radius_m`, the derived `window_dims` satisfies
  `span > 2 * radius * margin`.
- **Bounded travel.** Drive the camera 10 km and assert that `OccupancyGrid::region_count()`,
  `LodTree::node_count()` and resident slot counts *plateau* rather than grow. This is the
  test that catches the levels >= 5 leak.
- **No edge.** An edit at (50000, 0, 50000) is accepted and its bricks generate.
- **Op extent cap.** A 1e5 m radius op rejects in bounded time — a regression test for a
  hazard the refactor itself creates (§7).
- **Precision.** The SDF reconstructs cleanly at +/-100 km, pinning the documented limit
  from invariant 5.

**Phase 3 — no-visual-change proof.** A demo capture at `stream_radius_m = 1638.4` compared
against a pre-refactor capture, plus a benchmark leg at the default radius through the
existing `reports/` harness, so "A costs nothing at today's reach" is measured rather than
asserted.

## 10. Deliberate deferrals and non-goals

- **Floating-origin rebasing.** Not built. Invariant 5's +/-100 km limit is documented and
  tested instead. A keeps the coordinate seam narrow enough that rebasing remains addable.
- **4000 m at frame rate.** A makes the radius expressible and correct; B makes it fast.
  `LodPool::kChunkRecords` (8192), the page arena, the `min(lod_builds_per_frame, 8)`
  per-frame build cap, and sun-shadow cascades are all B.
- **Disk persistence.** A ships `PinnedRegionArchive` behind the `RegionArchive` port. C
  implements the disk backend.
- **Terrain that fills a 4 km world.** The terrain pipeline is unchanged. Whether a 4 km
  horizon of the current analytic terrain is *worth looking at* is a content question, not
  a streaming one.
