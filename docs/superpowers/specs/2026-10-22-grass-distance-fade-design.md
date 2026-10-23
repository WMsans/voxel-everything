# Grass fades into its ground

Date: 2026-10-22. Supersedes the far-LoD-ring parts of
`2026-09-11-stylized-grass-design.md`.

## Why

Today the terrain under grass (`grass_01`) looks nothing like the grass canopy that grows on it.
On screen the ground is a pale, flat mint, while the canopy is a darker, more saturated green
with bright tips. Wherever the blades stop, you can see the line. To hide that line the grass
reaches very far: near blades out to the raymarcher seam (38–48 m at the steady pose), then
three far LoD rings out to about 320 m (`kGrassFarBaseM << 3`), with blades widening to hold
coverage. Those rings still show as bush clumps on light ground, and you can see each ring
change level. The code already says more blades can't fix this
(`grass_settings.h`, `far_blades_per_cell` ponytail note).

BotW does the opposite. Blades are clearly visible up close, become less and less visible with
distance, and are gone after a few tens of metres. Past that point the ground *is* the grass
colour, so removing blades changes almost nothing on screen, and there are no grass LoD levels
to tell apart.

The user's motive is **frame time**: grass reach should be set by how it looks, not by how far
the mismatch has to be pushed away.

Measured 2026-10-22 on the M1 at 2560×1440, interleaved A/B/A, p50:

| Pose | Today | far rings off + reach 32 m | grass off |
|---|---|---|---|
| steady (`--benchmark`) | 12.96 ms | 12.50 ms | 12.12 ms |
| vista (`--pose=20,75,30,200,-12`) | 13.33–13.89 ms | 13.33 ms | 13.33 ms |

The saving is about 0.5 ms and comes mostly from the shorter near reach. Frame times here snap to
steps of about 0.4 ms, so smaller effects don't show. The default world's grass is patchy. The
user approved going ahead on that basis: about 0.5 ms plus the visual cleanup.

## Goals

1. Blades end at about **32 m** (user's number). Toward that distance they become less and less
   visible, and past it there are none.
2. No visible line where blades end. Grass ground with blades and grass ground without blades
   look the same at the fade distance.
3. No grass LoD levels, so no LoD seams. The far LoD rings are **deleted**.
4. Frame time no worse than today on the steady and vista poses. It's expected to be about
   0.5 ms better.

## Non-goals

- Far-field impostors, billboards, cards, or any stand-in geometry for distant grass.
- New texture work on the grass ground (detail maps, noise patches) beyond recolouring it.
- Any shader tint or distance-dependent colour for grass ground (declined by the user).
- Changing the near blade look (shape, wind, colours, lighting) within the first ~15 m.

## Anti-goals (a failure even if it "works")

- A visible circle or band around the camera where blades end, at any of the capture views.
- The ground matching only at one sun angle so badly that it reads as a line at another. See the
  risks section: it doesn't need to be perfect, but it can't be a line.
- Grass ground up close that reads as bare dirt or a flat sticker between the blades.

## Decisions

- **Reach: 32 m.** `GrassSettings::reach_m` default becomes 32. `VoxelFrame::grass_layout()` still
  clamps it to the seam and residency radius; at 32 m it sits inside both on every pose measured
  (seam 38.4–48 m).
- **Delete the far rings outright.** Not just default `far_lod_rings = 0`. That means
  `far_lod_rings`, `far_blades_per_cell`, the `GrassLayout` far fields, `GrassParams::far`,
  `kGrassFarBaseM`, the far-cell half of `grass_bricks.comp.glsl`, `far_blade()` in
  `grass_scatter.comp.glsl`, the far term in `grass_width_scale()`, the far-limit in
  `grass.frag.glsl`'s fade, their tests in `tests/test_grass.gd`, the settings rows, and the
  references in `hooks_lod.cpp` / `hooks_render.cpp` / `lod_*` / `benchmark.gd` /
  `grass_capture.gd` comments. Git history keeps it. *(My call: the user wants them gone, not
  switchable.)*
- **Fade by coverage, not by holding coverage flat.** Today the near rings halve their blade count
  every 10 m and `ring_width_gain` widens blades to win that coverage back. The intent is now the
  reverse: coverage *should* fall off toward 32 m. Width compensation is reduced or removed, so
  falling blade counts plus the existing bayer-dither fade (`grass.frag.glsl`, currently
  `0.75·limit..limit`, which becomes 24–32 m) carry the fade. *(My call. The exact gain and
  fade-band start are left to the builder and judged by the acceptance captures.)*
- **Match on the ground side, by repainting the grass basecolor texture** (user's decision). The
  ground changes to match the canopy, not the other way round, because the blade look is already
  tuned. Grass ground on screen comes from `assets/materials/00_basecolor.png` in both fields,
  through `material_surface()` (`composite.frag.glsl` near, `lod.frag.glsl` far), with the AO map
  folded in at 0.65 and the normal map applied. `flat_albedo` in `material_table.h` is only a
  fallback (edit previews, water) and is *not* the lever. The texture is hand-authored (user
  replaced it in b8db6d9) and `tools/convert_materials.sh` skips `grass_01`, so the repaint is a
  direct edit of the committed PNG. No shader tint, no new material column.
- **Apply everywhere** (user's decision). `grass_01` is the base material for most non-steep
  ground in every world type (`height_bands`, `fjord_bands`), so every grassy hill out to the
  horizon takes the canopy colour too. That's the intent, and it's the closest match to BotW's
  distant fields. There's no distance-dependent colour.
- **Repaint as a colour remap, not a new painting.** Shift hue, saturation and value of the
  existing texture so its mean moves to the target while its detail and variation survive. Leave
  the AO, normal, height and roughness maps alone. Afterwards, set `grass_01`'s `flat_albedo` to
  the new texture's measured mean so the fallbacks agree (as `forest` already does). *(My calls.)*

## What "matching" means

The target is the canopy as it appears **on screen** at the fade distance after lighting, not the
blade albedo. Blades get `grass_sun_term()` self-shade toward the root, a different normal, and
bright tips. The ground gets the texture, the AO fold, and the normal map. So the ground texture's
target colour has to be found on screen, not computed from the blade constants.

Procedure: render the same view twice, grass on and grass off (`tools/grass_capture.gd
--grass=enabled,0`), and compare region-average colour over grass ground in the 20–32 m band.
Adjust the texture remap until they agree. If a texture remap can't get there (for example, the
ground needs to be darker in sun but not in shade), stop and report back to the user. A shading
term was explicitly declined.

## Acceptance

All from captures and benches through the shipping path (`tools/grass_capture.gd`,
`demo/scenes/main.tscn --benchmark`), not the debug hooks.

1. **Views.** At least three: eye level (BotW angle, height ~1.6 m, pitch ~-0.1), 6 m up, and the
   vista pose. Stream over real surface grass: `--at=20,60,30` (memory: the default 30,56.2,30
   spot sees buried cave grass).
2. **No line.** In each view, the grass-on and grass-off frames' region-average colour over grass
   ground in the 20–32 m band differ by at most about 3% in each channel. Under 15 m, blades are
   clearly visible (unchanged look). Show the user before/after captures. The user's eye is the
   final judge.
3. **Far vista before/after.** Grass hills out to the horizon change colour by design. Show the
   user a before/after of the vista pose and of one fjords view for sign-off, because this changes
   the look of every world.
4. **Second sun angle.** Repeat one view with the sun moved substantially (a low sun). The band
   doesn't need to hit 3%, but it must not read as a line.
5. **Frame time.** Interleaved A/B/A (memory: single runs report warm-up) on the steady and vista
   poses. p50 no worse than today. Report the numbers either way.
6. **Tests.** Far-ring tests deleted, not skipped. Remaining grass suites pass. Frame goldens that
   move are re-recorded only after confirming the move is the intended look (memory: freeze
   animation and diff, because settle frame count shifts phase). Known baseline gdUnit failures
   are checked against a stash run before being blamed on this change.

## Risks and known ceilings

- **The whole far-field palette shifts.** Every `grass_01` hillside in every world gets darker and
  more saturated. This is intended, but it's the biggest visible change in the project and the
  reason for acceptance 3. Conifers grow on `grass_01` too, so check that they still read
  against it.
- **Lighting-dependent match.** A single ground colour can match the canopy at one sun angle and drift at
  others, because blades and ground shade differently. Acceptance 3 is the guard. If it fails,
  it comes back to the user (see above).
- **Grass on slopes without blades.** `slope_cos_min` keeps blades off steep `grass_01` ground.
  That ground becomes canopy-green too, which is intended (apply everywhere), but check it in a
  capture.
- **Saving is small in this world** (~0.5 ms). A meadow-heavy world would save more; no such pose
  was measured.

## Left to the builder

- The exact colour remap applied to `00_basecolor.png` (target found on screen as above) and the
  tool used to apply it. It should be repeatable, e.g. a script under `tools/`, so the remap can
  be redone if the user repaints the source.
- `ring_width_gain` (or removing it and `shape.w`), the near ring step / halving schedule within
  32 m, and the fade-band start.
- Whether `kGrassRings` stays at 4 when only 32 m is covered.
- Whether `vertical_reach_m`'s default (256, now capped by the 32 m reach anyway) needs touching.
