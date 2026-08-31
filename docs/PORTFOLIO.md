# Voxel Everything

Destructible smooth-SDF voxel terrain in Godot 4.7 — 5 cm voxels, raymarched near field,
meshed far field out to 4 km, one deferred cel-shading stack over both.

## What it does
- **Near field (raymarched; designed 0–150 m, measured seam 32–80 m across benchmark legs):** compute-only
  sphere tracing through a sparse 0.8 m brick atlas. The atlas, not a residency radius, is
  the real budget; the seam follows how far the near field is actually complete this frame
  instead of trusting a configured radius.
- **Far field (beyond the measured seam to 4 km):** eight levels of surface-nets chunks,
  12 bytes per quad, selected by screen-space error and culled by frustum + HiZ in one
  indirect multi-draw.
- **Destruction:** ordered CSG op lists per region, re-evaluated on the GPU; a region that
  passes 192 ops consolidates into override bricks instead of dropping the player's edits.
- **Islands:** severed pieces become Jolt bodies, land, sleep, and merge back into terrain;
  collider builds are split into octants so one fat chunk cannot stall the frame.

## Measured (RTX 4070 Laptop, 1440p requested; Wayland actual viewport 2560×2778)

Steady leg, `m7-final` run. Full per-leg numbers, WARNs included, are in M7 Errata 10.

| Pass | Budget | p50 | p99 |
|---|---:|---:|---:|
| GPU raymarch | 6.0 ms | 7.212 ms | 9.610 ms |
| GPU stream | — | 0.003 ms | 0.004 ms |
| GPU LoD | 2.0 ms | 0.043 ms | 0.051 ms |
| GPU SSGI | 1.5 ms | 0.172 ms | 0.316 ms |
| GPU SSR | 1.5 ms | 0.139 ms | 0.141 ms |
| GPU shadows | 1.0 ms | 0.124 ms | 0.274 ms |
| GPU outlines | 0.3 ms | 0.081 ms | 0.082 ms |
| Custom GPU frame | 16.0 ms | 8.254 ms | 10.825 ms |
| Wall frame | 16.0 ms | 16.67 ms | 24.03 ms |

| Leg | GPU raymarch p50/p99 | Wall p50/p99 | over 16.6 ms |
|---|---:|---:|---:|
| steady | 7.212 / 9.610 | 16.67 / 24.03 | 254 / 300 (84.7%) |
| move | 7.106 / 8.810 | 19.44 / 32.33 | 278 / 300 (92.7%) |
| ridge | 5.385 / 9.112 | 17.36 / 31.49 | 224 / 300 (74.7%) |
| edit | 12.925 / 29.145 | 27.78 / 81.77 | 282 / 300 (94.0%) |
| edit-bounded (900f) | 9.259 / 12.574 | 20.37 / 55.65 | 890 / 900 (98.9%) |
| island | 7.331 / 9.899 | 20.00 / 33.33 | 866 / 900 (96.2%) |

Every leg prints `budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS
shadows=PASS outlines=PASS frame=WARN` and `timing_condition display_driver=Wayland
vsync_requested=disabled vsync_actual=enabled verdict_qualified=true`. The raymarch WARN
is the edit leg's p99 over the 6 ms GPU budget; the frame WARN is the wall-clock p99 over
16 ms in every leg. A second run (`m7-final-b`) reproduced the same verdicts. The
`edit-bounded` leg is the 900-frame bounded-region consolidation run (a small cluster of
adjacent regions): `overflow=0`,
`consolidations=1`, and stable `gpu_stream` p50/p99 (0.208/3.554 ms in `m7-final`,
0.204/3.683 ms in `m7-final-b`).

## Frame budget on an Apple M1 (8-core GPU, 2560x1440)

The numbers above are an RTX 4070 Laptop. The same build on an M1 opened at **7.5 fps
standing still** (133 ms/frame). Frame time scaled cleanly with pixel count and turning the
near field off alone restored 60 fps, so the whole deficit was per-pixel work in the
raymarcher and the stack over it -- not streaming, meshing or physics.

Cutting the marcher's per-step cost took 133 ms to **49.9 ms** with no change to the image
(commit "cut the near-field marcher's per-step cost"; the largest single win was deleting the
in-brick 0.1 m min-max gate, which had become a second texture fetch buying a shorter advance
than the sphere-trace step it displaced). The rest is configuration: this GPU is roughly a
seventh of the reference one, and the frame is almost entirely screen-space.

Shipped defaults, chosen by sweeping the two dials against these legs:

| Dial | Where | Value |
|---|---|---|
| 3D render scale | `project.godot` `rendering/scaling_3d/scale` | 0.65 (bilinear) |
| Near-field scale | `demo/main.tscn` `VoxelWorld.near_field_scale` | 0.40 |
| Quality tier | default | High -- it costs ~0.2 ms here, so there is no reason to drop it |

`near_field_scale` is the fraction of the internal 3D resolution the marcher runs at. It
buys VISIBILITY only: `composite.frag.glsl` resolves the material -- the triplanar albedo,
the roughness and the AO -- once per full-resolution pixel from the position, normal and
material id the marcher exports, so lowering the scale costs silhouette precision on the
terrain edges and no texture detail (`tests/test_near_field_scale.gd` pins the second half
of that). Raise both towards 1.0 on a larger GPU. The benchmark
overrides them per run: `--render-scale=`, `--near-scale=`, `--quality=`, plus
`--frames=`/`--warmup=`/`--screenshot=`.

`tools/run_benchmarks.sh m1-tuned`, macOS/Metal, V-Sync genuinely disabled
(`verdict_qualified=false`):

| Leg | avg fps | p50 | p95 | p99 | min fps |
|---|---:|---:|---:|---:|---:|
| steady | **72.2** | 13.89 | 13.89 | 14.58 | 59.2 |
| move | **61.4** | 16.67 | 18.06 | 19.97 | 37.1 |
| ridge | **68.8** | 14.29 | 16.67 | 19.85 | 38.0 |
| edit | **30.5** | 29.63 | 59.26 | 129.49 | 7.3 |
| edit-bounded | **44.5** | 19.05 | 41.25 | 47.33 | 7.7 |
| island | **61.6** | 16.67 | 16.67 | 18.06 | 36.8 |

Two things the table does not say on its own:

* **The steady leg now settles before it samples.** It claims "the player is frozen, nothing
  streams after warmup", but at frame 60 there were still 370 collision chunks pending and
  the mesher worker was submitting GPU batches through the whole sampled run -- a streaming
  measurement wearing a steady-state label. It now warms up until the chunk queue is empty
  and prints `BENCH settle frames_to_quiet=` (947 here, against a 1500-frame cap). Only that
  leg: the edit and island legs deliberately keep the world dirty.

* **The edit leg's p99 is a GPU burst in the regeneration path**, not the raster this work
  touched. It is unchanged by render scale, by physics, and by turning islands off; only its
  p50 follows resolution. It was the worst leg on the reference GPU too (p99 81.77 ms there).
  Bounding it means giving edit-driven brick regeneration a per-frame budget, which is a
  streamer change, not a shader one.

### Known on macOS/Metal, pre-existing

Both predate this work (same failures on the parent commit) and both affect what the numbers
above mean:

* **The far field does not write depth**, so the near field cannot occlude it and it ghosts
  through the terrain. `test_lod_render::test_depth_is_written_so_the_near_field_can_occlude`
  and `test_lod_gbuffer::test_far_field_pixels_carry_a_material_and_a_unit_normal` fail here
  and pass on Linux/Vulkan. More far-field pixels are shaded than should be, so the frame
  costs more than a correct build would.
* **GPU timestamps are emulated and return 0.** `capture_timestamp` records names fine, but
  `get_captured_timestamp_gpu_time()` is 0 for every marker, so every `BENCH gpu_*` line reads
  `samples=0` and every per-pass budget verdict is `UNMEASURED`. Per-pass attribution on this
  platform has to come from A/B runs instead; frame times are stable to ~0.1%, which makes
  that workable.

## Build and run

```bash
./build.sh -j$(nproc)          # macOS: -j$(sysctl -n hw.ncpu)
godot --path . demo/main.tscn
tools/run_benchmarks.sh m7-final
```

`run_benchmarks.sh` runs on macOS as well as Linux: it picks the display driver only where
there is a choice to make (x11 vs Wayland), and takes `GODOT_BIN` or `godot` from `PATH`.

The demo help overlay lists the four tools, the radius wheel, and the reload/self-check
keys. `tools/encode_capture.sh` turns the deterministic `--capture` PNG sequence into video.

## Where the work is

| Area | Code | Plan |
|---|---|---|
| Walking skeleton, field, raymarch | `extension/src/generator`, `extension/src/render`, `shaders/` | `docs/superpowers/plans/2026-08-12-m1-walking-skeleton.md` |
| GPU generation, streaming, edits | `extension/src/render/world_streamer.*`, `extension/src/world/` | `docs/superpowers/plans/2026-08-13-m2-gpu-generation-streaming-edits.md` |
| Physics, meshing, colliders | `extension/src/physics/`, `extension/src/mesh/` | `docs/superpowers/plans/2026-08-14-m3-physics-meshing-colliders.md` |
| Connectivity and islands | `extension/src/connectivity/`, `extension/src/physics/island_manager.*` | `docs/superpowers/plans/2026-08-15-m4-connectivity-islands.md` |
| Far-field LoD | `extension/src/lod/`, `shaders/lod*` | `docs/superpowers/plans/2026-08-17-m5-far-field-lod.md` |
| Beauty stack | `extension/src/shade/`, `shaders/deferred*` | `docs/superpowers/plans/2026-08-18-m6-beautification.md` |
| Budgets, demo shell, capture | `demo/`, `tools/`, `extension/src/render/gpu_timings.*` | `docs/superpowers/plans/2026-08-19-m7-budget-demo-capture.md` |
