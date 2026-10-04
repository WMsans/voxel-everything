# Voxel Everything — Raster Mode

**Date:** 2026-10-04
**Status:** design approved, not yet planned
**Start commit:** `6637bf2`
**Prior specs:** `2026-08-17-m5-lod-design.md` (the LoD tree this deepens);
`2026-10-01-transparent-voxels-design.md` (the island contouring and island-page draw this
reuses).

A settings switch, "Raymarching", that turns world ray marching off completely. With it off,
every surface — terrain near and far, and islands — is a rasterized surface-nets mesh, and
the near field is about as detailed as the transparency shell (0.1 m cells). It is a
performance mode: visual loss against the marched near field is accepted.

---

## 1. Intent

- **Who:** a player on hardware where the near-field marcher is too expensive.
- **What they get:** the same world, edits, destruction and islands, drawn without the
  compute marcher, at roughly transparency-shell quality up close.
- **Success:** the switch works live from the settings menu; raymarched mode renders
  exactly as today; raster mode has no holes while streaming, renders islands, and costs
  less frame time than raymarched mode in an interleaved A/B/A.

Goals:

- One Render-tab switch, live, no restart.
- No world ray march in raster mode: no `raymarch` dispatch, no `sun_march` in scatter.
- 0.1 m terrain near the camera, coarsening by screen-space error.
- Islands render.
- Raymarched mode is bit-identical to `main`.

Non-goals, decided explicitly:

- **Marched-quality shadows.** Raster mode shadows every pixel from the sun map, whose
  cascade 0 texel is 0.4 m.
- **Island sun-map shadows.** Islands cast none in raster mode (§5).
- **The edit visualiser and the cost view.** Both are drawn by the marcher and are absent
  in raster mode.
- **Stopping brick streaming.** Grass, leaf scatter and edits read the atlas, so it keeps
  streaming. Raster mode saves the marcher, not brick generation.
- **Screen-space passes.** SSR, SSGI, SSAO and contact shadows are unchanged.

## 2. The switch

`RenderSettings::raymarch` (default `true`), row `bool_row("raymarch", "Raymarching", ...)`
in `render_rows()`. `RenderOrchestrator` mirrors it into an atomic next to
`near_field_enabled_`, and `set_effect_enabled("raymarch", ...)` reaches it through the
existing render-switch branch, which also gives the benchmark `--effects-off=raymarch`.

`RenderOrchestrator::near_field_enabled()` returns `near_field && raymarch`. Every existing
consumer of that gate then behaves correctly in raster mode with no edit:

- `LodSystem::fade_band` reports 0 / 1e9, so the far field owns every distance;
- `VoxelFrame` skips the HiZ build;
- the near transparency shell grid is off;
- `deferred.comp.glsl`'s `far_field_owns` is true for every pixel, so the sun map shades
  everything.

A new `RenderOrchestrator::raster_mode()` (`!raymarch`) gates only the additions in §3–§5.
The existing `near_field` toggle keeps its meaning (0.4 m LoD everywhere, marcher still
drawing islands); five test suites use it as a lever.

## 3. LoD levels

`kLodBaseCell` becomes 0.1 m and `kLodLevels` becomes 10: levels 0/1/2 are 0.1/0.2/0.4 m and
level 9 is the former level 7. A new constant `kLodFarBaseLevel = 2` names the former
level 0.

`LodTreeConfig::min_level` floors the descent:

| mode | `min_level` | finest cell |
|---|---|---|
| raymarched (default) | 2 | 0.4 m (as today) |
| near field off, raymarch on | 2 | 0.4 m (as today) |
| raster | 0 | 0.1 m |

`LodSystem` sets it from `raster_mode()` each tick. `want_finer` and `children_ready` treat
`min_level` as the floor that level 0 is today.

Constants that meant "level 0 = 0.4 m", restated against `kLodFarBaseLevel`:

| constant / site | today | after |
|---|---|---|
| `kLodNearDenseRadiusM` in `want_finer` | forces level 0 within 300 m | forces descent only while `level > kLodFarBaseLevel`; below it SSE decides |
| `kLodResidentLevelFrom` | 5 | 7 |
| `sun_cascades`: `r0`, cascade 0 `min_level` | `kLodBaseCell`, 0 | `lod_cell_size(kLodFarBaseLevel)`, `kLodFarBaseLevel` |
| `min_level_for_texel` | starts at level 0 | starts at `kLodFarBaseLevel` |
| `lod_reduce.cpp` encode and outside byte | `cell_size <= kLodBaseCell` | `cell_size <= lod_cell_size(kLodFarBaseLevel)` |

The shadow cut keeps its cascade floors, so the sun map is never drawn from geometry finer
than 0.4 m.

At `kLodTargetCellPx = 3` and 1080p this selects 0.1 m to roughly 45 m and 0.2 m to roughly
90 m. `children_ready` keeps the parent drawn until all children are built, so streaming
never shows a hole.

**Transparency.** Fine-level chunks contour their shell exactly as far chunks do today
(`lod_quads` shell mode), so raster mode needs no new transparency code; the near shell
grid stays off through the §2 gate.

**Page budget.** `max_lod_pages_` is 32768. A refused request already falls back to the
coarser parent. The plan measures peak pages in raster mode and raises the budget for
raster mode if it binds.

## 4. The frame in raster mode

```
stream → sky fill → LoD (levels 0–9, islands) → grass → leaves
  → shell → ssgi → ssao → deferred → transparency → inject → contact → ssr → outlines
```

- **Skipped:** the island cull and the `raymarch` dispatch. `RaymarchPass` allocates no
  targets in raster mode.
- **Sky fill.** `CompositePass` draws a `SKY_ONLY` variant of `composite.frag.glsl`: the
  same G-buffer clear and fullscreen triangle, `rd` rebuilt from the push block it already
  receives, and the miss branch's outputs (sky in albedo, material 0, gloss 0, depth 0)
  without sampling any marcher target. The LoD raster then draws over it with depth
  testing as it does today. Deferred, outlines and inject see the same G-buffer contract.
- **Scatter sun.** `grass_scatter.comp.glsl` and `leaf_scatter.comp.glsl` get a push flag;
  with it set they write `sun = 1` instead of calling `terrain_sun_visibility`. Deferred
  then shadows those pixels from the sun map, since `far_field_owns` is true everywhere.

Known ceiling: the 0.1 m surface sits a few cm off the ≥ 0.4 m mesh that drew the sun map,
which may show as acne. The far-grove capture (§6) decides; if it shows, the fix is a
normal-offset bias on the sun-map lookup in raster mode.

## 5. Islands

Today only the marcher draws an island's opaque part.

- **Contour.** `ve::island_shell_blocks` becomes `ve::island_blocks(v, origin, voxel, mode,
  out)`. *Shell* mode is today's function. *Opaque* mode maps a transparent solid to outside
  (the opaque view at the island's voxel pitch) and calls `lod_contour(..., false)`.
  `RenderOrchestrator::contour_island_shell` produces both meshes at extraction.
- **Both modes.** The `VolumeData` only passes through `island_shell_wait_` briefly, so an
  island extracted in raymarched mode would have no mesh after a live toggle. Opaque meshes
  are therefore contoured in both modes. The plan measures the cost on the island leg; if
  it hitches the render thread, contouring moves to the worker.
- **Storage.** Local-space pages via `LodPool::upload_at`, with an island flag distinct from
  the shell's so the shell passes skip them, tracked in `LodSystem::island_mesh_pages_` and
  released through the same slot-liveness walk as `island_shell_pages_`.
- **Draw.** The island transform branch of `shell.vert.glsl` moves into
  `shaders/island_xform.glslh`; `shell.vert.glsl` and `lod.vert.glsl` both include it.
  `LodRasterPass` binds `island_desc` and, in raster mode only, draws a second list of the
  live islands' opaque pages.
- **Merge-back:** unchanged. The island becomes a volume op that dirties its terrain chunks,
  which rebuild at 0.1 m.
- **Shadows:** islands cast none in raster mode; `lod_shadow.vert.glsl` draws the tree cut
  only. Adding them is the same include plus one draw.

## 6. Testing

**Characterization first**, before `kLodBaseCell` or `kLodLevels` changes:

- `test_lod_tree.cpp`: record the walk's draw and request sets for fixed cameras (flat
  ground, a ridge, looking straight down). After the change, raymarched mode must produce
  the identical sets with every level shifted by +2.
- `test_sun_cascades.cpp`: pin radii, texel sizes and `min_level`; after the change,
  `min_level` shifts by +2 and the rest is identical.
- `test_lod_reduce.cpp`: pin the encode and outside bytes at 0.4 m and 0.8 m.
- gdUnit goldens `test_lod_raster_golden`, `test_frame_shipped_golden`, `test_lod_seam`
  and `test_sun_shadow` stay byte-identical in raymarched mode. The baseline failure set
  drifts: stash and re-run on clean `main` before blaming the change.

**Native:**

- Raster mode descends to level 0 near the camera; raymarched mode never goes below 2.
- `want_finer` stops forcing density at `kLodFarBaseLevel`.
- `island_blocks` opaque mode drops transparent solids; shell mode is byte-identical to
  today's `island_shell_blocks`.

**gdUnit**, through the shipping capture path rather than `hooks.cpp`, streamed at
(20,60,30):

1. Frame contract: with raymarch off the raymarch stage does no work and allocates no
   targets; the G-buffer holds sky where nothing is drawn and ground under the camera.
2. Level-0 chunks are ready and drawn within a bounded number of frames.
3. An island severed in raster mode covers pixels with its material.
4. Live toggle raymarched → raster → raymarched leaks no LoD pages.
5. `test_settings_menu.gd`: the "Raymarching" row exists and round-trips.

**Measurement.** GPU timings are invalid on this machine, so cost is frame time from
interleaved A/B/A runs of `--benchmark` and `--benchmark-move` with
`--effects-off=raymarch` as B, plus peak LoD pages in raster mode and island-leg contour
cost. One capture at the far grove (65,62,-930) checks for sun-map acne on the 0.1 m
surface.

## 7. Failure handling

- LoD pool exhausted: the walk refuses and draws the coarser parent (existing).
- Island page upload refused: that block is not drawn (existing), noted with a
  `ponytail:` comment as the shell's is.
- Sky-fill pipeline missing: the frame aborts as a failed composite does today.
