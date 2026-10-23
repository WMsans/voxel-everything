# Grass Fades Into Its Ground Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Grass blades end at 32 m and thin out on the way, the far LoD rings are deleted, and
the grass ground texture is repainted to the canopy's on-screen colour so the end of the blades
leaves no visible line.

**Architecture:** Three changes, in order. (1) Delete the far-ring path end to end: settings,
layout, the GPU parameter block, the field binding the scatter pass carries only for the rings,
and both shader paths. (2) Shorten the near reach to 32 m and let coverage fall with distance
instead of holding it flat. (3) Recolour `assets/materials/00_basecolor.png` with a repeatable
per-channel gain, tuned against grass-on/grass-off captures.

**Tech Stack:** C++ GDExtension (doctest, `extension/build/tests/ve_tests`), GLSL via Godot
RenderingDevice, GDScript gdUnit4 (`./gdunit_tests.sh`), ImageMagick `magick` for the texture.

**Spec:** `docs/superpowers/specs/2026-10-22-grass-distance-fade-design.md`

## Global Constraints

- Reach: `GrassSettings::reach_m` default **32.0** m. Dither fade band **24–32 m** (`0.75·reach..reach`).
- Far rings are **deleted**, not defaulted off: no `far_lod_rings`, `far_blades_per_cell`,
  `kGrassFarBaseM`, `GrassParams::far`, `far_cell()`, `far_blade()`, `far_ground()`.
- Ground recolour is a **repaint of `00_basecolor.png`** applied **everywhere** `grass_01` is drawn.
  No shader tint, no new material column, no distance-dependent colour. AO, normal, height and
  roughness maps are untouched.
- If a texture recolour cannot pass the band check, **stop and report to the user**. Do not add a
  shading term.
- Near blade look within ~15 m is unchanged (shape, wind, colours, lighting).
- Captures and benches go through the shipping path (`tools/grass_capture.gd`,
  `demo/scenes/main.tscn -- --benchmark`), never the debug hooks.
- Commit messages: plain conventional commits, **no Co-Authored-By or any AI attribution line**.
- After any C++ or shader change: `./build.sh`. A "RegionPass: uniform set creation failed" error
  means the dylib is stale; rebuild.

## Review Focus

1. **Camera high above grass** (flying, clifftop >32 m up): no blades at all, ground colour carries
   the meadow. Before this change, far rings drew blades there. Expected: zero blades, no error.
   Pinned in Task 1 (`test_no_blades_reach_ground_from_high_above`).
2. **Saved settings or scripts still naming `far_lod_rings` / `far_blades_per_cell`**: setting them
   must fail softly (return false), not crash or silently alias another knob. Pinned in Task 1
   (`the store rejects the deleted far-ring knobs`).
3. **Scatter pass called with no field context**: the old `far_ok` branch is gone, and so is the
   field parameter, so nothing must bind set 1. Covered by the existing gdUnit suite running on
   local devices; Task 1 removes the parameter, so a missed call site fails to compile.
4. **Low sun / shade**: the repainted ground matches at the default sun but drifts at a low sun.
   Pinned in Task 3 (second-sun capture, judged by eye, not just the 3% number).
5. **Steep `grass_01` slopes with no blades** (`slope_cos_min`) and **conifer floors** become
   canopy-green too. Intended, but each needs a look in Task 3's captures.

---

### Task 1: Delete the far LoD rings

**Files:**
- Modify: `extension/src/grass/grass_settings.h` (remove `far_lod_rings`, `far_blades_per_cell` and their comments; reword comments that cite far rings)
- Modify: `extension/src/grass/grass_settings.cpp:11-48` (remove the two rows; reword the `reach_m` and `ring_width_gain` comments; `ring_width_gain` label becomes "Width gain")
- Modify: `extension/src/grass/grass_layout.h` (remove `kGrassFarBaseM`, `GrassParams::far`, `GrassLayout::far_ring_count/far_cell_dim/far_cells/far_reach_m`; `max_bricks` = `near_brick_cap`, `dispatch_threads` = `near_columns`)
- Modify: `extension/src/grass/grass_layout.cpp` (remove the far-ring block, the far estimate loop, and the `p.far` writes)
- Modify: `extension/src/gpu_layout/blocks.h:308` (remove the `far` field), then regenerate `shaders/generated/blocks.glslh`
- Modify: `extension/src/render/grass_scatter_pass.h` / `.cpp` (remove the `const FieldContextSet *field` parameter of `run()`, `field_ops_`, binding 11, `far_ok`, and `field->bind`)
- Modify: `extension/src/render/frame.cpp:524-526`, `extension/src/debug/hooks_render.cpp` (both `run()` call sites near lines 728 and 3053; drop the field argument)
- Modify: `shaders/grass_bricks.comp.glsl` (remove `far_ground()`, `far_cell()`, the `field.glslh` include and set-1 declarations if only the far path uses them, the far-thread dispatch in `main()`, and the far-entry packing comment)
- Modify: `shaders/grass_scatter.comp.glsl` (remove `far_blade()` and the `0x80000000u` branch in `main()`)
- Modify: `shaders/grass.glslh:22-36` (`grass_width_scale` loses the `max(1.0, distance_m / r)` far factor; reword the comment)
- Modify: `shaders/grass.frag.glsl:53-61` (fade limit is `pc.cam.w`, not `pc.cam.w * exp2(float(pc.far.x))`; reword the comment)
- Modify: comments only: `tools/grass_capture.gd:7`, `demo/scripts/benchmark.gd:235-237`
- Test: `extension/tests/test_grass_layout.cpp`, `extension/tests/test_grass_settings.cpp`, `tests/test_grass.gd`

**Interfaces:**
- Produces: `bool GrassScatterPass::run(RenderingDevice *rd, GpuAtlas &atlas, const ve::GrassLayout &layout, const ve::RegionWindow &region_win, float time_seconds, RID sun_ubo, uint64_t world_epoch = ve::kAlwaysScatter)`, which is today's signature minus `field`. `sizeof(ve::GrassParams) == 256`.

- [ ] **Step 0: Record the baseline before touching anything.** Task 3 and Task 4 compare against these. `SCRATCH` is any directory outside the repo.

```bash
./build.sh
for v in "eye:--height=1.6 --pitch=-0.10" "six:--height=6 --pitch=-0.15"; do n=${v%%:*}; a=${v#*:}
  godot --path . --resolution 1280x720 -s res://tools/grass_capture.gd -- --out=$SCRATCH/base_$n --at=20,60,30 $a
done
godot --path . --resolution 2560x1440 --disable-vsync demo/scenes/main.tscn -- --benchmark --pose=20,75,30,200,-12 --screenshot=$SCRATCH/base_vista.png
godot --path . --resolution 2560x1440 --disable-vsync demo/scenes/main.tscn -- --benchmark --pipeline=res://assets/pipelines/fjords.pipeline --seed=880510467 --pose=-98.05,186.0,417.40,59.0,-5 --screenshot=$SCRATCH/base_fjords.png
./gdunit_tests.sh > $SCRATCH/base_gdunit.txt 2>&1   # baseline failures drift; keep the list
```

- [ ] **Step 1: Rewrite the layout tests for a near-only layout.** In `test_grass_layout.cpp`, delete `far LoD rings extend the reach...` and `far cell count per ring is independent of the reach`. In `the blade estimate grows with reach...`, delete the far-ring comment. Rename `stage 1 dispatches one thread per column, then one per far cell` to `stage 1 dispatches one thread per column` and assert `l.dispatch_threads == dim_x * dim_z` and `l.max_bricks == l.near_brick_cap`. Change the params test:

```cpp
TEST_CASE("GrassParams is 256 bytes and its floats land where GLSL expects") {
	// Sixteen vec4: cam, planes[6], brick_min, brick_dim, ring_end, ring_blades, blade,
	// wind, style, shape, limits.
	CHECK(sizeof(ve::GrassParams) == 256);
	// ...raw[0..2] camera checks unchanged
}
```

- [ ] **Step 2: Add the deleted-knob test** in `test_grass_settings.cpp`, and remove `far_*` from any knob list there:

```cpp
TEST_CASE("the store rejects the deleted far-ring knobs") {
	ve::GrassSettingsStore store;
	CHECK_FALSE(store.set_value("far_lod_rings", 3.0f));
	CHECK_FALSE(store.set_value("far_blades_per_cell", 16.0f));
}
```

- [ ] **Step 3: Run the C++ tests and confirm they fail.**
Run: `cd extension && ./build/tests/ve_tests -tc="*GrassParams*,*far-ring*,*stage 1*"`
Expected: FAIL (size 272 ≠ 256; `set_value("far_lod_rings")` returns true).

- [ ] **Step 4: Delete the far path in C++** (settings, layout, `blocks.h`, scatter pass and its three call sites), as listed under Files. Regenerate the GLSL block:
`./build.sh && cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests -tc="generated*"`

- [ ] **Step 5: Delete the far path in the shaders** (`grass_bricks.comp.glsl`, `grass_scatter.comp.glsl`, `grass.glslh`, `grass.frag.glsl`), as listed under Files. `grep -rn "far\.\|\.far\b\|far_cell\|far_blade\|far_ground" shaders/grass*` must return nothing.

- [ ] **Step 6: Run the C++ tests and confirm they pass.**
Run: `./build.sh && cd extension && ./build/tests/ve_tests`
Expected: all pass, including `generated: shaders/generated/blocks.glslh`.

- [ ] **Step 7: Update the gdUnit grass suite.** In `tests/test_grass.gd`, delete `test_far_lod_rings_place_blades_past_the_near_reach`, `test_grass_does_not_thin_as_the_reach_grows_from_high`, `test_ring_one_finds_ground_far_below_a_high_camera` and `test_far_lod_rings_do_not_see_edits`. Remove every `set_grass_value("far_lod_rings", ...)` line and the far-ring paragraph above `test_painting_grass_to_rock_removes_its_blades`. Add:

```gdscript
# No far rings: from 60 m above the meadow the near sphere cannot reach the ground, and
# nothing else places blades. The ground's own colour carries the field from up here.
func test_no_blades_reach_ground_from_high_above() -> void:
	var w := make_world()
	stream_to(w, Vector3(30.0, 116.2, 30.0))
	assert_int(w.hooks().debug_grass_stats()["blades"]).is_equal(0)
```

- [ ] **Step 8: Run the grass suites.**
Run: `./gdunit_tests.sh -a res://tests/test_grass.gd,res://tests/test_settings_names.gd`
Expected: `test_grass.gd` and `test_settings_names.gd` pass. `test_grass_golden.gd` may move; leave it for Task 4. Compare any other failure against `$SCRATCH/base_gdunit.txt` before blaming this change.

- [ ] **Step 9: Commit.**

```bash
git add -A extension/src extension/tests shaders tests tools/grass_capture.gd demo/scripts/benchmark.gd
git commit -m "refactor(grass): delete the far LoD rings"
```

---

### Task 2: Reach 32 m, coverage falls off instead of holding flat

**Files:**
- Modify: `extension/src/grass/grass_settings.h` (`reach_m = 32.0f`; rewrite its comment: the reach is chosen by look, BotW-style, the ground texture carries the colour past it; `ring_width_gain` default and comment)
- Test: `extension/tests/test_grass_layout.cpp`

**Interfaces:**
- Consumes: Task 1's near-only `GrassLayout`.
- Produces: `GrassSettings{}.reach_m == 32.0f`.

- [ ] **Step 1: Write the failing test.**

```cpp
TEST_CASE("default grass ends at 32 m") {
	ve::GrassSettings s;
	CHECK(s.reach_m == doctest::Approx(32.0f));
	const float cam[3] = {0, 0, 0};
	float vp[16];
	identity(vp);
	const ve::GrassLayout l = ve::grass_layout(s, cam, vp);
	CHECK(l.ring_end_m[ve::kGrassRings - 1] == doctest::Approx(32.0f));
	CHECK(ve::grass_ring_of(l, 33.0f) == -1);
}
```

- [ ] **Step 2: Run it and confirm it fails.**
Run: `cd extension && ./build/tests/ve_tests -tc="default grass ends at 32 m"`
Expected: FAIL (reach 120).

- [ ] **Step 3: Set `reach_m = 32.0f` and start `ring_width_gain` at `1.0f`.** With 10 m rings that halve the count (48/24/12/6), a gain of 3 holds coverage flat to 32 m, which is the behaviour being removed. Gain 1 lets coverage fall to about 1/4 by the fade band. This is the starting value for Step 5's tuning.

- [ ] **Step 4: Run the C++ tests.**
Run: `./build.sh && cd extension && ./build/tests/ve_tests`
Expected: all pass.

- [ ] **Step 5: Tune by eye.** Capture the eye and six views (Task 1 Step 0 commands, `--out=$SCRATCH/t2_*`). Blades must be clearly visible under ~15 m and look the same there as in the baseline. They must visibly thin toward 32 m, with no hard ring edge at 10/20/30 m. Sweep `--grass=ring_width_gain,{0,0.5,1,1.5,2}` and keep the value where thinning is steady and ring boundaries don't show. Write the chosen value into the default. The ground still mismatches at this point; that's Task 3's job, so judge the blades only.

- [ ] **Step 6: Commit.**

```bash
git add extension/src/grass/grass_settings.h extension/tests/test_grass_layout.cpp
git commit -m "feat(grass): blades end at 32 m and thin on the way out"
```

---

### Task 3: Repaint the grass ground to the canopy's colour

**Files:**
- Create: `tools/recolour_grass.sh`
- Modify: `assets/materials/00_basecolor.png` (output of the script)
- Modify: `extension/src/world/material_table.h:47` (`grass_01` `flat_albedo` = new texture's mean; comment like `forest`'s: "flat_albedo is that texture's measured mean, tools/recolour_grass.sh")
- Modify: `tools/grass_capture.gd` (add `--sun=pitch_deg,yaw_deg`, which sets `VoxelWorld/Sun`'s `rotation_degrees` before streaming)

**Interfaces:**
- Produces: `tools/recolour_grass.sh <gain_r> <gain_g> <gain_b>`. It reads the hand-authored source from git (`git show b8db6d9:assets/materials/00_basecolor.png`), multiplies each channel by its gain, writes `assets/materials/00_basecolor.png` (PNG24, stripped, like `convert_materials.sh`), and prints the result's mean RGB as three floats in 0..1. Gains are applied to the original each run, never compounded.

- [ ] **Step 1: Write the script.** Per channel: `magick in.png -channel R -evaluate multiply $gr -channel G -evaluate multiply $gg -channel B -evaluate multiply $gb +channel -strip PNG24:out.png`, then `magick out.png -format "%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]" info:`. `set -euo pipefail`, plus a usage line on a wrong argument count. `tools/recolour_grass.sh 1 1 1` must reproduce the source's mean exactly. That's the script's check: run it and compare with `magick <(git show b8db6d9:assets/materials/00_basecolor.png) -format ... info:`.

- [ ] **Step 2: Add `--sun` to `tools/grass_capture.gd`** with the same argument-parsing shape as `--rock`. Check: a `--sun=15,200` capture shows long shadows.

- [ ] **Step 3: Measure the mismatch.** For each view (eye, six), capture grass on and `--grass=enabled,0`. Pick a screen rectangle over `grass_01` ground in the 20–32 m band (the same rectangle for both frames of a view; write it down) and read its mean:
`magick f.png -crop WxH+X+Y +repage -format "%[fx:mean.r] %[fx:mean.g] %[fx:mean.b]" info:`
The target per channel is the grass-on mean. The error is (grass-off / grass-on) per channel.

- [ ] **Step 4: Iterate the gain.** `gain_c ← gain_c × (on_c / off_c)` averaged over the two views. Run the script, `./build.sh` only if the material atlas needs it (it loads PNGs at runtime: `extension/src/render/material_atlas.cpp`), and re-capture grass-off. Stop when every channel in both views is within 3%, or after 5 rounds. **If it doesn't converge, or the two views want opposite corrections, stop and report the numbers to the user.** Don't add a shading term.

- [ ] **Step 5: Second sun.** Capture the six view grass-on and grass-off with `--sun=15,200`. Report the band numbers. Open the grass-on frame and confirm by eye that there's no visible ring where the blades end. Failing by eye means stop and report.

- [ ] **Step 6: Set `flat_albedo`** for `grass_01` to the script's printed mean (two decimals, like the other rows). `shaders/material_table.glslh` is generated and asserted by `extension/tests/test_material_glslh.cpp`, which prints the correct contents on failure. Run `./build.sh && cd extension && ./build/tests/ve_tests`, paste the printed contents into the `.glslh`, and re-run until it passes.

- [ ] **Step 7: Before/after for the whole world.** Re-take `vista` and `fjords` screenshots (Task 1 Step 0 commands, `$SCRATCH/after_*`). Also check one steep grass slope and one conifer floor in them, Review Focus 5.

- [ ] **Step 8: User sign-off.** Show the user baseline vs after for eye, six, low-sun six, vista and fjords, plus the band numbers. **Do not commit the texture until the user approves the look.**

- [ ] **Step 9: Commit.**

```bash
git add tools/recolour_grass.sh assets/materials/00_basecolor.png extension/src/world/material_table.h shaders/material_table.glslh tools/grass_capture.gd
git commit -m "feat(grass): ground texture takes the canopy's colour"
```

---

### Task 4: Frame time, goldens, full suite

**Files:**
- Modify (re-record only): `tests/test_grass_golden.gd` golden data, frame goldens touched by grass or the grass texture

- [ ] **Step 1: A/B/A frame time.** Build the baseline in a separate worktree at `b3e8a45` (the spec commit, before any code change). Run steady and vista legs interleaved as `A B A B A` (A = baseline worktree, B = this branch), one Godot process at a time. Single runs report warm-up, so never compare a lone pair. Read the `BENCH p50=` lines:
`godot --path . --resolution 2560x1440 --disable-vsync demo/scenes/main.tscn -- --benchmark [--pose=20,75,30,200,-12]`
Expected: branch p50 ≤ baseline p50 on both poses (about 0.5 ms lower at steady). Report the table either way.

- [ ] **Step 2: Goldens.** For each golden suite that fails, freeze frame-counter animation (`freeze_grass()` where the suite has it), capture the value on the baseline worktree and on this branch, and confirm the diff is only the intended look: shorter grass, new ground colour. Then re-record: `tests/test_grass_golden.gd` keeps hand-edited `GOLDEN` constants with a dated "Re-recorded on ... for ..." comment above them (follow the existing two); other suites use their own documented switch. A golden that moves for any other reason is a bug; stop.

- [ ] **Step 3: Full suites.**
Run: `./build.sh && (cd extension && ./build/tests/ve_tests) && ./gdunit_tests.sh`
Expected: C++ all pass. gdUnit failures equal to or a subset of `$SCRATCH/base_gdunit.txt`.

- [ ] **Step 4: Commit.**

```bash
git add tests
git commit -m "test(grass): re-record goldens for the 32 m reach and repainted ground"
```
