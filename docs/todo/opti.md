Split each chunk's collider into 8 octant bodies. The only remaining spike is one fat chunk (16–22k triangles → 20–30 ms), which is atomic and no throttle can divide. Binning triangles by centroid octant keeps the identical triangle soup (no seams, no duplicates) but makes the Jolt work spreadable across frames. This is what would close p99 (currently 18.6 ms moving / 24.1 ms editing) to a solid 60. active_bodies() must be redefined to count chunks so test_collider_stream.gd keeps its meaning.

Fix 1 properly: write occupancy from the generated brick's 5 cm lattice rather than the 3×3×3 probe — brick_gen.comp.glsl already reduces min/max over the 17³ lattice into s_mip8, and it knows rslot/brick from the job. brick_mark then writes occupancy only for !has_surface bricks (unambiguous). This is exact at the resolution you render, with no halo and no phantom-solid artifact.

## M6 beautification benchmark follow-ups

- **Raymarch GPU p99 exceeds the 6 ms budget:** steady 7.955 ms, move 7.989 ms, ridge 8.736 ms, edit 14.226 ms, island 8.527 ms.
- **Frame p99 exceeds the 16 ms budget** in all five benchmark legs. Wayland forced V-Sync on despite `--disable-vsync`, so these frame results are qualified rather than directly comparable to the requested uncapped condition.
- LoD, SSGI, SSR, shadow, and outline GPU budgets passed. All five benchmark processes exited cleanly; retain the existing ObjectDB leak warning and display-fallback diagnostics for follow-up.
