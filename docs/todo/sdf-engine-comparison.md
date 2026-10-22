# SDF / voxel engine comparison — next optimisations

Opened 2026-10-21 after comparing the renderer with Claybook, Dreams, Teardown, Lumen,
Brixelizer and the sphere-tracing literature. Ordered by expected payoff.

Rays no longer run the terrain pipeline. The primary march reads the R8 brick atlas, and
the hit's normal and sun visibility come from the per-brick cache (`e3b85c2`, `90b8759`;
walk 50.65 → 32.61 ms, forest 49.06 → 27.06 ms on the M1). What follows is where the
pipeline still runs, plus the march's empty-space cost.

Measured at the fjords-forest pose (seed 880510467, 32x18 grid of
`debug_raymarch_cost_probe` rays, local device):

- 2,500 of 53,507 resident bricks (4.7 %) have no baked normal.
- Per ray: atlas SDF samples p50 9 / p90 32, brick-DDA cells p50 83 / p90 123.
- The whole near field is ~3.5 ms of the 32.6 ms forest walk (near field off = 28.87 ms).
  On that leg, most of the remaining cost is outside the marcher.

## 1. Edits patch the stored brick instead of regenerating it

- [ ] Price it first: how much of the edit leg (27.24 ms vs steady 12.82) is brick
  regeneration? PORTFOLIO.md already puts the edit p99 in "a GPU burst in the regeneration
  path".
- [ ] Apply a new op to the brick's existing lattice instead of re-running
  `eval_field_pair` (base pipeline + full op list) at all 17³ samples.

Claybook bakes brushes into the world volume, so render cost does not depend on brush
count. The override-brick consolidation (>192 ops) is already that mechanism; this would
use it from the first edit. The constraints are the R8 narrow band, repeated-edit
quantisation, and the CPU mirror / physics keeping the op list as truth.

## 2. Far grass rings stop re-tracing the ground every frame

- [ ] `grass_bricks.comp.glsl` `far_ground()` calls `eval_base_ground` up to 40 times per
  far cell, every frame, for thousands of cells. The cells are anchored on their ring's
  lattice, so a cell's ground height is stable while the camera moves. Cache it per cell,
  or for Fjords read the already-baked sector heightmap.
- [ ] A/B/A with `--grass=0|1` to price the far rings on their own before and after.

## 3. Coarser empty-space skipping in the primary march

- [ ] A ray steps through ~80 bricks for ~9 SDF samples. Add a level between the region
  (25.6 m) and the brick (0.8 m), e.g. a 64-bit occupancy mask per 4x4x4 bricks
  (brickmap / Tree64 style), or Claybook's mips, where the max step doubles at each level.
- [ ] Alternative: an 8x8-pixel cone pre-pass that writes a safe start `t` per tile
  (Claybook, cone marching).
- [ ] The ceiling on the forest walk is ≤ 3.5 ms. It is worth more on the RTX legs, where
  the GPU raymarch was the over-budget pass.

## 4. Per-brick stage culling for streaming

- [ ] The trees stage makes a cold atlas fill 5.2x slower (774 → 4463 ms). `brick_gen`
  already filters ops per brick (as Dreams does) and caches conifer cells, but it always
  runs every stage.
- [ ] Skip a stage for a brick when its bound says it cannot reach the brick (Keeter-style
  interval / bound pruning), e.g. conifers far from any tree cell. This reduces pop-in,
  not the steady frame.

## 5. Remaining normal-cache misses

- [ ] The 4.7 % of bricks without a baked normal still evaluate the pipeline 7 times per
  hit pixel (`terrain_source_normal`), and so do glossy-reflection hits in those bricks.
  Bake those bricks at a finer resolution, or store their normals in the
  `StoredNormalPool`. Low priority.

## Sources

- Claybook, GDC 2018: https://media.gdcvault.com/gdc2018/presentations/Aaltonen_Sebastian_GPU_Based_Clay.pdf
- Dreams, SIGGRAPH 2015: https://advances.realtimerendering.com/s2015/AlexEvans_SIGGRAPH-2015-sml.pdf
- Teardown frame analysis: https://acko.net/blog/teardown-frame-teardown/
- Lumen performance guide: https://dev.epicgames.com/documentation/unreal-engine/lumen-performance-guide-for-unreal-engine
- Brixelizer, GDC 2024: https://gpuopen.com/gdc-presentations/2024/GDC2024_GI_with_AMD_FidelityFX_Brixelizer.pdf
- Keeter 2020: https://www.mattkeeter.com/research/mpr/keeter_mpr20.pdf
- Segment Tracing (Galin et al. 2020): https://diglib.eg.org/handle/10.1111/cgf13951
- Cone marching overview: https://pointersgonewild.com/2026-03-06-a-recursive-algorithm-to-render-signed-distance-fields/
