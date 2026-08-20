~~Split each chunk's collider into 8 octant bodies. The only remaining spike is one fat chunk (16–22k triangles → 20–30 ms), which is atomic and no throttle can divide. Binning triangles by centroid octant keeps the identical triangle soup (no seams, no duplicates) but makes the Jolt work spreadable across frames. This is what would close p99 (currently 18.6 ms moving / 24.1 ms editing) to a solid 60. active_bodies() must be redefined to count chunks so test_collider_stream.gd keeps its meaning.~~

**Done in M7 Task 9.** `ColliderStreamer` bins each triangle by centroid into eight octants,
stages fresh concave shapes, and swaps the complete set only after all octants are built.
`active_bodies()` remains a chunk count; `bodies_in_space()` is the raw diagnostic count.
The final sparse-body Wayland benchmark reduced the maximum one-call `shape_set_data` build
from 22.41 ms to 0.40 ms in the edit leg and from 0.95 ms to 0.31 ms while moving. The true
per-call `build_ms` maxima were 1.03 ms (edit) and 0.49 ms (move); these are reported in
`BENCH max_ms`, not accumulated per-frame. Move frame p99 was 27.29 ms and edit frame p99
was 50.39 ms, so the overall frame budget remains open for other spikes. Full evidence is
in M7 Errata 7.

Fix 1 properly: write occupancy from the generated brick's 5 cm lattice rather than the 3×3×3 probe — brick_gen.comp.glsl already reduces min/max over the 17³ lattice into s_mip8, and it knows rslot/brick from the job. brick_mark then writes occupancy only for !has_surface bricks (unambiguous). This is exact at the resolution you render, with no halo and no phantom-solid artifact.

**Done in M7 Task 10.** `brick_gen.comp.glsl` now classifies each generated brick from the
17³, 5 cm SDF lattice reduction and writes the exact two-bit state to the region occupancy
buffer. `brick_mark.comp.glsl` retains only the unambiguous air/full writes for bricks it
does not generate; generated bricks are not exposed through the probe's conservative estimate.
The CPU reference mirrors the encoded lattice and retains `cell_state_probe` for the mark
semantics. Occupancy now has no probe halo or phantom-solid state at render resolution.

Task 10's fresh five-leg Wayland benchmark (`m7-task10-final`) remained qualified because the
compositor rejected disabled V-Sync: steady/move/ridge/edit/island frame p99 was
23.71/31.66/32.04/73.17/31.05 ms; `brick_gen` `build_ms` maxima were
0.50/0.65/0.37/0.61/0.50 ms, and all five runs exited 0. GPU raymarch p99 was
8.94/8.37/8.90/15.84/9.20 ms, so the existing raymarch/frame warnings remain; this change
adds no claim of a 60-fps budget closure. See `.superpowers/sdd/2026-08-19-m7-budget-demo-capture/task-10-report.md`
for the full evidence and Errata 8.

## M6 beautification benchmark follow-ups

- **Raymarch GPU p99 exceeds the 6 ms budget:** steady 7.955 ms, move 7.989 ms, ridge 8.736 ms, edit 14.226 ms, island 8.527 ms.
- **Frame p99 exceeds the 16 ms budget** in all five benchmark legs. Wayland forced V-Sync on despite `--disable-vsync`, so these frame results are qualified rather than directly comparable to the requested uncapped condition.
- LoD, SSGI, SSR, shadow, and outline GPU budgets passed. All five benchmark processes exited cleanly; retain the existing ObjectDB leak warning and display-fallback diagnostics for follow-up.
