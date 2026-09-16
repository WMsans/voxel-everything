# Pass anatomy and generated layouts — measured results

Recorded 2026-09-16 on macOS 26.4.1 / Apple M1 / Metal 4. Implementation head `576b54c`. Baseline Task 1 `27bc434`.

## 1. Verification

Fresh final verification on this tree:

```text
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
==> Done.
build=0

(cd extension && scons -Q test)
[doctest] test cases:     577 |     577 passed | 0 failed | 0 skipped
[doctest] assertions: 9117397 | 9117397 passed | 0 failed |
[doctest] Status: SUCCESS!
native=0

./gdunit_tests.sh
Open XML Report: reports/report_192/results.xml
Exit code: 0
Run tests ends with 0
gdunit=0
leaks=0
```

Raw exit-probe output:

```text
$ rg -n "shader_compile_spirv_from_source" extension/src -g '*.cpp'
extension/src/render/gpu/gpu.cpp:92:	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
$ rg -n 'key_[a-z_]+_ = |uset_[a-z_]+_ = |fb_[a-z]+_ = ' extension/src/render
extension/src/render/raymarch_pass.cpp:52:	uset_mask_ = RID();
extension/src/render/raymarch_pass.cpp:141:		uset_mask_ = mask;
$ ls shaders/generated/
blocks.glslh
cel.glslh
cel_constants.gdshaderinc
constants.glslh
field.glslh.golden
gbuffer.glslh
$ rg -n 'TEST_CASE\("generated: ' extension/tests/test_generated_glsl.cpp
44:TEST_CASE("generated: shaders/generated/blocks.glslh") {
48:TEST_CASE("generated: shaders/generated/constants.glslh") {
52:TEST_CASE("generated: shaders/generated/gbuffer.glslh") {
71:TEST_CASE("generated: shaders/generated/cel.glslh") {
75:TEST_CASE("generated: shaders/generated/cel_constants.gdshaderinc") {
$ rg -n 'the committed GLSL mirror matches the C\+\+ table' extension/tests/test_material_glslh.cpp
13:TEST_CASE("the committed GLSL mirror matches the C++ table") {
```

`reports/report_192/results.xml`: 90 suites, 467 tests, 0 failures, 0 errors. The Task 1
baseline recorded 459 tests, 0 failures, 0 errors and 0 leak lines. The final failure set is
`none`, so it is no worse than the baseline. The eight-test increase is coverage added during
Tasks 2–6 and 37–40 (including the S4 and S7 tests); no baseline failure was moved or newly
introduced. The known flaky suites remain `test_connectivity` and `test_island_body`; both had
zero failures in the final report.

## 2. Exit criteria

| Criterion (spec §6) | Result | Evidence |
|---|---|---|
| One compile site | PASS | `rg` prints only `extension/src/render/gpu/gpu.cpp:92` (`shader_compile_spirv_from_source`). |
| `invalidate_uniform_set`, tautological asserts, `MATERIAL_LAYERS` defines, `GRASS_MATERIAL` gone | PASS | The exact `rg -n 'invalidate_uniform_set|static_assert\(sizeof\(float\) \*|#define MATERIAL_LAYERS|GRASS_MATERIAL' extension/src shaders` command printed no lines. |
| No hand-written key caches | WAIVED — accepted audit exception | The exact command printed `extension/src/render/raymarch_pass.cpp:52` and `:141` for `uset_mask_`. The source comment and use show an externally owned tile-mask RID identity tracker, not a uniform-set key cache; the regex output is preserved and production code was not altered to hide it. |
| Every generated file byte-tested | PASS | `shaders/generated/blocks.glslh`, `constants.glslh`, `gbuffer.glslh`, `cel.glslh`, and `cel_constants.gdshaderinc` each have a `generated:` case in `extension/tests/test_generated_glsl.cpp`; `field.glslh.golden` is checked by `the default pipeline generates the committed source` in `extension/tests/test_field_codegen_golden.cpp`; `shaders/material_table.glslh` is checked by `the committed GLSL mirror matches the C++ table` in `extension/tests/test_material_glslh.cpp`. Native suite: 577/577. |
| New material ≤ 4 files | WAIVED — accepted measurement-target exception; 6 logical locations measured | Retrace: `assets/materials/07_{basecolor,normal,roughness,ambientOcclusion,height}.png` (asset set), `tools/convert_materials.sh`, `extension/src/world/material_table.h`, `shaders/material_table.glslh`, `shaders/stages/height_bands.field.glslh`, and `extension/src/terrain/builtin_stages.cpp`. The procedural placement CPU/GLSL mirror pair keeps the change two logical locations over target; the exact count is retained. |
| New G-buffer channel ≤ 5 files | WAIVED — accepted measurement-target exception; 12 files measured for the full contract | Retrace: `extension/src/gpu_layout/gbuffer_layout.h`; `shaders/generated/gbuffer.glslh`; `extension/src/render/gbuffer.h`; `extension/src/render/gbuffer.cpp`; writers `shaders/composite.frag.glsl`, `shaders/lod.frag.glsl`, `shaders/grass.frag.glsl`; readers `shaders/deferred.comp.glsl`, `shaders/ssao.comp.glsl`, `shaders/ssgi.comp.glsl`, `shaders/ssr.comp.glsl`, `shaders/outline.comp.glsl`. The writer/reader fan-out is the reason this remains over target; the exact count is retained. |
| FogPass retrace | WAIVED — accepted measurement-target exception; 9 files measured | `render/fog_pass.h`, `render/fog_pass.cpp`, `shaders/fog.comp.glsl`, `render/orchestrator.h`, `render/orchestrator.cpp`, `render/frame.h`, `render/frame.cpp`, `render/gpu_timings.cpp`, `tests/test_frame_contract.gd`. `gpu_timings.cpp` remains the ninth file because `known_pass()` drops an unknown label; it is the only file over the ≤8 core path. |
| gdUnit no worse than baseline; moved pins attributed | PASS | Baseline `27bc434`: 459 tests, failure set none, leaks 0. Final `reports/report_192`: 467 tests, failure set none, leaks 0. Migration pins stayed unchanged. Added tests are coverage, not moved golden values. |
| S7, S9 closed; S4 sun/ambient closed, albedo remainder out of scope | PASS with accepted S4 remainder (not a claim that S4 is fully complete) | S7 fixed by `ed62b44` after failing test commit `06a8187`; S9 closed by `61c62a6`; S4 sun/colour/ambient fixed by `5b130dc` after failing test commit `d53f8b5`; island rock albedo is an accepted out-of-scope remainder because `IslandBody` has no material data. |

## 3. Pinned-value attribution

No migration commit moved a pinned output. The added S4 and S7 tests are new assertions; the
full final suite and the shipped frame goldens remain green.

The only committed golden text changes in this sub-project were intentional generated-source
changes:

- Task 37, commit `61c62a6` (`feat: beauty flags, material layers and strides generated into GLSL (S9)`):

  ```diff
  -// Pipeline hash: 9031282453609108635
  +// Pipeline hash: 8127489946819813969
  -\tctx.material = ctx.height > 4.0 ? 2u : (ctx.height > 1.0 ? 1u : 3u);
  +\tctx.material = ctx.height > 4.0 ? MAT_ROCK : (ctx.height > 1.0 ? MAT_GRASS_01 : MAT_GROUND_01);
  -\tint base = slot * 4916;
  +\tint base = slot * OVERRIDE_SDF_STRIDE_BYTES;
  -\tint mi = slot * 4096 + cell.x + cell.y * BRICK_VOXELS +
  +\tint mi = slot * OVERRIDE_MAT_STRIDE_BYTES + cell.x + cell.y * BRICK_VOXELS +
  ```

  Cause: generated constants and named material/stride symbols; float baselines were not
  changed.

- Task 38, commit `5125296` (`feat: materials are placed by name in C++ and GLSL`):

  ```diff
  -\tctx.material = ctx.height > 4.0 ? 2u : (ctx.height > 1.0 ? 1u : 3u);
  +\tctx.material = ctx.height > 4.0 ? MAT_ROCK : (ctx.height > 1.0 ? MAT_GRASS_01 : MAT_GROUND_01);
  ```

  Cause: the CPU and GLSL height-band material placement now uses named table entries.

`git status --short extension/tests/golden` printed no output. No float baseline under
`extension/tests/golden/` changed.

## 4. Bite proofs

The controller ledger at `docs/superpowers/plans/2026-09-15-pass-anatomy-baseline.md` records the
full site rows. The 23 deliberate break proofs are summarized here from that ledger:

| Site | Break | Failing case — message | Reverted |
|---|---|---|---|
| ssao | `f[5] = 0` | `test_ssao_golden.gd::test_ssao_statistics_match_the_recorded_golden` — min_ao moved for `down_close`: golden 0.705882, got 1.000000 | yes |
| contact_shadow | `params[1] = 0` | `test_contact_shadow_golden.gd::test_contact_apply_matches_the_recorded_golden` — mean_darkening moved: golden 0.006335, got 0.000000 | yes |
| outline | `f[6] = 0` | `test_outline.gd::test_depth_line_is_one_pixel_and_darkens_by_the_fixed_amount` — expected 0.000000 in 0.340000..0.360000 | yes |
| ssr | bindings 2/3 swapped | `test_ssr.gd::test_a_post_opaque_only_blocker_is_reflected` — expected greater than 0, got 0 | yes |
| ssgi | `f[22] = 0` | `test_ssgi.gd::test_light_bounces_once_the_history_exists` — eight frames produced no bounce; max_channel 0.0, mean_luma 0.0 | yes |
| hiz | `p[4] = 0` | `test_hiz.gd::test_the_reduction_is_a_min_in_reverse_z` — expected 0.100000 in 0.899000..0.901000 | yes |
| deferred | `u[24] = 0` | `test_deferred_golden.gd::test_lit_frame_matches_the_recorded_golden` — mean_luma moved: golden 0.033779, got 0.044053 | yes |
| inject | delete draw | `test_frame_shipped_golden.gd::test_the_shipped_frame_matches_the_recorded_golden` — oblique tile 6 moved 0.016639; horizon tile 15 moved 0.052532 | yes |
| grass_raster | `f[16] = 0` | `test_grass_golden.gd::test_grass_shading_matches_the_recorded_golden` — max_luma moved: golden 0.800194, got 0.811078 | yes |
| lod_raster | `f[20] = fade_start` | `test_lod_raster_golden.gd::test_lod_fade_matches_the_recorded_golden` — coverage 0.097114→0.102458; depth_sum 4.693420→5.418561 | yes |
| sun_shadow | zero ortho push | `test_sun_shadow.gd::test_something_actually_gets_drawn_into_it` — expected 1.000000 in -0.010000..0.010000 | yes |
| composite | overlay binding replaced by surface | `test_composite_golden.gd::test_sky_through_the_composite_matches_the_recorded_golden` — sky red golden 0.251000, got 0.000000 | yes |
| island_cull | `dims[3] = 0` | `test_island_render.gd::test_the_tile_mask_marks_the_tiles_the_island_covers` — expected greater than 0, got 0 | yes |
| lod_cull | `ip[0] = 0` | `test_lod_cull.gd::test_facing_away_culls_almost_everything` — culled ratio moved from 0.000000 to 0.333475 | yes |
| grass_scatter | `if (false)` | `test_grass.gd::test_blades_appear_on_grass_terrain` — expected greater than 0, got 0 | yes |
| raymarch | half dispatch groups | `test_raymarch_pixel.gd::test_ray_down_from_sky_hits_terrain` — ray down from sky missed | yes |
| orchestrator/downsample | `dims[0] = 1` | `test_ssgi.gd::test_light_bounces_once_the_history_exists` — max_channel 0.000900, mean_luma 0.000023 | yes |
| orchestrator/pre-flight | compile stages forced to compute | `test_shader_reload.gd::test_reload_keeps_the_world` — `last_ok` false | yes |
| brick_gen | delete indirect dispatch | `test_brick_diff.gd::test_base_terrain_bricks_match_the_cpu_reference` — up to 255 encoded SDF steps, palette mismatch, 612/612 material mismatches | yes |
| region | delete phase-1 mark dispatch | `test_region_pass.gd::test_marking_allocates_exactly_the_bricks_the_cpu_calls_active` — GPU marked 0, CPU 105 | yes |
| consolidate | delete run dispatch | `test_consolidation.gd::test_bake_reproduces_the_field` — expected 0, got 368475 | yes |
| island_extract | delete extract dispatch | `test_island_extract.gd::test_a_single_cell_extracts_to_the_same_volume_on_both_sides` — worst SDF disagreement 255 encoded steps | yes |
| mesh | delete record_quads dispatch | `test_mesh_diff.gd::test_a_surface_chunk_meshes_identically_on_both_sides` — 16878 triangles CPU-only | yes |
| lod_build | delete record_quads dispatch | `test_lod_build.gd::test_a_submitted_chunk_comes_back_with_quads` — surface-straddling chunk produced no quads | yes |

## 5. Suspected bugs

| Id | Result | Evidence |
|---|---|---|
| S4 | Sun direction, colour, ambient: FIXED. Island rock albedo: ACCEPTED OUT-OF-SCOPE REMAINDER (not a claim that S4 is fully complete; `IslandBody` has no material data) | Failing test `d53f8b5`, fix `5b130dc`, passing `test_object_lighting_follows_the_scene_sun`; adding per-material albedo requires island material extraction outside this sub-project. |
| S7 | FIXED | Failing test `06a8187`, fix `ed62b44`, passing `test_blades_write_the_grass_blade_material_not_the_terrain_they_grow_on`; generated `MAT_GRASS_BLADE` is id 200. |
| S9 | CLOSED: asserts deleted, values generated | `61c62a6`; exact forbidden-pattern `rg` is empty and generated constants/native tests pass. |

## 6. Deletion and size

Pass `.cpp` line counts, baseline `27bc434` → final `576b54c`:

| File | Before | After |
|---|---:|---:|
| `ssao_pass.cpp` | 168 | 73 |
| `contact_shadow_pass.cpp` | 175 | 78 |
| `outline_pass.cpp` | 177 | 63 |
| `ssr_pass.cpp` | 242 | 83 |
| `ssgi_pass.cpp` | 213 | 99 |
| `hiz_pass.cpp` | 292 | 204 |
| `deferred_pass.cpp` | 258 | 130 |
| `inject_pass.cpp` | 149 | 58 |
| `grass_raster_pass.cpp` | 209 | 92 |
| `lod_raster_pass.cpp` | 340 | 168 |
| `sun_shadow_pass.cpp` | 277 | 195 |
| `composite_pass.cpp` | 274 | 114 |
| `island_cull_pass.cpp` | 132 | 77 |
| `lod_cull_pass.cpp` | 279 | 187 |
| `grass_scatter_pass.cpp` | 434 | 272 |
| `raymarch_pass.cpp` | 281 | 170 |
| `orchestrator.cpp` | 728 | 662 |
| `brick_gen_pass.cpp` | 126 | 65 |
| `region_pass.cpp` | 179 | 124 |
| `consolidate_pass.cpp` | 234 | 192 |
| `island_extract_pass.cpp` | 257 | 211 |
| `mesh_pass.cpp` | 417 | 347 |
| `lod_build_pass.cpp` | 445 | 363 |
| **Total** | **6286** | **4027** |

The 23 files shrink by 2259 lines. The helper is 596 lines total: `gpu_core.h` 157,
`gpu.h` 162, `gpu.cpp` 277. The exact branch diff stat is:

```text
60 files changed, 1807 insertions(+), 3480 deletions(-)
```

## 7. Accepted waivers and out-of-scope remainder

These audit probes and file-count limits are documented accepted waivers/measurement targets,
not unresolved blockers. The exact regex output and measured counts remain above and are not hidden:

1. **No-hand-written-key regex — accepted audit waiver.** `raymarch_pass.cpp` retains the
   externally owned `uset_mask_` RID tracker at lines 52 and 141. It is not a uniform-set key
   cache, and no code was changed to hide the literal.
2. **Procedural moss retrace — accepted measurement-target waiver.** It measures 6 logical
   locations versus the aspirational ≤4 target; the CPU/GLSL stage mirror is the excess.
3. **G-buffer-channel retrace — accepted measurement-target waiver.** It measures 12 files for
   the full contract versus the aspirational ≤5 target; the writer/reader fan-out is the excess.
4. **FogPass retrace — accepted measurement-target waiver.** It measures 9 files versus the
   aspirational ≤8 core-path target; `gpu_timings.cpp` is the single file over because
   `known_pass()` drops an unknown label.
5. **S4 island rock albedo — accepted out-of-scope remainder, not a claim that S4 is fully
   complete.** `IslandBody` carries no material data; adding per-material albedo requires island
   material extraction outside this sub-project.
