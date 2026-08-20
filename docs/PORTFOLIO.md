# Voxel Everything

Destructible smooth-SDF voxel terrain in Godot 4.7 — 5 cm voxels, raymarched near field,
meshed far field out to 4 km, one deferred cel-shading stack over both.

## What it does
- **Near field (raymarched; designed 0–150 m, measured seam 40–80 m):** compute-only
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

## Measured (RTX 4070 Laptop, 1440p requested; Wayland actual viewport 1152×1250)

Steady leg, `m7-final` run. Full per-leg numbers, WARNs included, are in M7 Errata 10.

| Pass | Budget | p50 | p99 |
|---|---:|---:|---:|
| GPU raymarch | 6.0 ms | 6.836 ms | 8.918 ms |
| GPU stream | — | 0.003 ms | 0.005 ms |
| GPU LoD | 2.0 ms | 0.044 ms | 0.051 ms |
| GPU SSGI | 1.5 ms | 0.171 ms | 0.174 ms |
| GPU SSR | 1.5 ms | 0.139 ms | 0.141 ms |
| GPU shadows | 1.0 ms | 0.125 ms | 0.275 ms |
| GPU outlines | 0.3 ms | 0.081 ms | 0.082 ms |
| Custom GPU frame | 16.0 ms | 7.880 ms | 10.132 ms |
| Wall frame | 16.0 ms | 16.67 ms | 21.60 ms |

| Leg | GPU raymarch p50/p99 | Wall p50/p99 | over 16.6 ms |
|---|---:|---:|---:|
| steady | 6.836 / 8.918 | 16.67 / 21.60 | 220 / 300 (73.3%) |
| move | 6.716 / 8.506 | 18.14 / 29.42 | 277 / 300 (92.3%) |
| ridge | 5.197 / 8.762 | 16.67 / 29.72 | 243 / 300 (81.0%) |
| edit | 12.315 / 28.878 | 27.68 / 83.78 | 279 / 300 (93.0%) |
| island | 6.965 / 9.249 | 20.00 / 30.87 | 863 / 900 (95.9%) |

Every leg prints `budget_verdict raymarch=WARN lod=PASS ssgi=PASS ssr=PASS
shadows=PASS outlines=PASS frame=WARN` and `timing_condition display_driver=Wayland
vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`. The raymarch WARN
is the edit leg's p99 over the 6 ms GPU budget; the frame WARN is the wall-clock p99 over
16 ms in every leg. A second run (`m7-final-b`) reproduced the same verdicts.

## Build and run

```bash
./build.sh -j$(nproc)
/usr/bin/godot --path . demo/main.tscn
tools/run_benchmarks.sh m7-final
```

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
