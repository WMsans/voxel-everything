# Optimisation follow-up — closed record

This file was opened with two M3/M4 items and three M6 benchmark follow-ups. All are now
resolved or explicitly left open with a reason; the current state is recorded here.

## Collider octant split

**Done in M7 Task 9.** `ColliderStreamer` bins each triangle by centroid into eight octants,
stages fresh concave shapes, and swaps the complete set only after all octants are built.
`active_bodies()` remains a chunk count; `bodies_in_space()` is the raw diagnostic count.
The final sparse-body Wayland benchmark reduced the maximum one-call `shape_set_data` build
from 22.41 ms to 0.40 ms in the edit leg and from 0.95 ms to 0.31 ms while moving. The true
per-call `build_ms` maxima were 1.03 ms (edit) and 0.49 ms (move); these are reported in
`BENCH max_ms`, not accumulated per-frame. Move frame p99 was 27.29 ms and edit frame p99
was 50.39 ms, so the overall frame budget remains open for other spikes. Full evidence is
in M7 Errata 7.

## Occupancy written from the generated brick lattice

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
adds no claim of a 60-fps budget closure. See M7 Errata 8 and the final `m7-final` sweep
below.

## M6 beautification benchmark follow-ups

- **Raymarch GPU p99 exceeds the 6 ms budget:** steady 7.955 ms, move 7.989 ms, ridge 8.736 ms, edit 14.226 ms, island 8.527 ms.
- **Frame p99 exceeds the 16 ms budget** in all five benchmark legs. Wayland forced V-Sync on despite `--disable-vsync`, so these frame results are qualified rather than directly comparable to the requested uncapped condition.
- LoD, SSGI, SSR, shadow, and outline GPU budgets passed. All five benchmark processes exited cleanly; retain the existing ObjectDB leak warning and display-fallback diagnostics for follow-up.

**Resolved by M7 Tasks 1–8.** The raymarch and frame WARNs are now measured with
per-pass GPU timestamps and a V-Sync readback that says exactly what the run got. The
final closing sweep (`m7-final` / `m7-final-b`) still reports `raymarch=WARN` on every leg
and `frame=WARN` on every leg; the raymarch p99 is worst on the edit leg (28.9 ms in
`m7-final`, 29.3 ms in `m7-final-b`), and wall-frame p99 is worst on edit (83.8 ms /
75.6 ms). These are the remaining open items, left open because closing them is a renderer
budget project of its own (ray step count / shadow ray cost and the edit-stream/CPU spikes),
not a follow-up that fits M7's closing-sweep task. All five legs exited 0 in both closing
runs and every `BENCH timing_condition` line was `display_driver=Wayland
vsync_requested=disabled vsync_actual=disabled verdict_qualified=false`.

The existing ObjectDB leak warning at exit remains a known Godot/GDExtension teardown
diagnostic; it does not fail the suites or the benchmark runs.
