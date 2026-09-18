#pragma once
#include "grass/grass_settings.h"
#include "world/region.h" // ve::IVec3

namespace ve {

inline constexpr int kGrassRings = 4;

// Near ring width in metres. ABSOLUTE, not a quarter of the reach: the reach now follows the
// raymarcher's own seam (VoxelFrame::grass_layout), which moves with streaming, and a density
// ramp that stretched with it would thin the grass at the player's feet every time another
// region landed. The last ring stretches to the reach instead, so every resident metre of
// ground falls inside some ring.
inline constexpr float kGrassRingStepM = 10.0f;

// Far LoD ring schedule: ring r covers out to kGrassFarBaseM << r in cells of
// kBrickSize << r. Absolute for the same reason as the near steps -- the ring an object
// stands in must depend on its distance, not on how much of the world happens to be
// resident. The near reach only decides where ring 1 STARTS (grass_bricks.comp.glsl).
inline constexpr float kGrassFarBaseM = 40.0f;

// Brick-list entries reserved per near column, and the hard cap on how many surface bricks
// one column may contribute. Capacity is kGrassColumnBricks * columns, so the cap is a
// STRICT bound the way the old box's cell count was: stage 1 can never want more list than
// was allocated.
// ponytail: a column with more crossings than this loses the lowest ones -- the walk is
// top-down, so a deep cave floor under four surfaces gets no grass. Raise it, or dispatch a
// second pass over the remainder, if buried grass ever becomes the visible problem.
inline constexpr int kGrassColumnBricks = 4;

// Uploaded to a uniform buffer and mirrored by `GrassParams` in shaders/grass.glslh. Laid
// out as sixteen vec4 (256 bytes) so std140 padding cannot disagree with the C++ struct;
// test_grass_layout pins both the size and the first three floats.
//
// Order matters and is asserted: cam_pos first, so a shader reading params.cam.xyz gets the
// camera without an offset table.
struct GrassParams {
	float cam[4];          // xyz camera position, w reach_m
	float planes[6][4];    // frustum planes, inward, normalised: xyz normal, w distance
	int32_t brick_min[4];  // xyz inclusive brick coordinate, w unused
	int32_t brick_dim[4];  // xyz brick counts, w total brick count
	float ring_end[4];     // ring outer radius in metres
	int32_t ring_blades[4]; // candidate blades per brick, per ring
	float blade[4];        // width, height, height_jitter, slope_cos_min
	float wind[4];         // strength, speed, scale, time_seconds
	float style[4];        // flower_chance, gloss, camera_tilt, blade_lighting
	float shape[4];        // wind_dir_rad, lean_spread_rad, base_curve, ring_width_gain
	int32_t limits[4];     // max_blades, max_bricks, unused, unused
	int32_t far[4];        // far LoD rings, cells per ring side, cells per ring, blades per cell
};

struct GrassLayout {
	IVec3 brick_min{};
	IVec3 brick_max{};
	int ring_count = kGrassRings;
	float ring_end_m[kGrassRings] = {0, 0, 0, 0};
	int blades_per_brick[kGrassRings] = {0, 0, 0, 0};
	// XZ columns in the near box: stage 1's first `near_columns` threads. One thread per
	// COLUMN, not per brick -- the box is now as tall as it is wide (the raymarcher's whole
	// resident sphere), and a thread per brick would dispatch its volume.
	int near_columns = 0;
	// Brick-list entries the near columns may fill: kGrassColumnBricks each.
	int near_brick_cap = 0;
	// Far LoD rings past the near reach. Ring r (1-based) uses cells of kBrickSize << r over
	// a radius of kGrassFarBaseM << r, so every ring has the same far_cell_dim^2 cells -- the
	// whole reason grass can follow the LoD at all.
	int far_ring_count = 0;
	int far_cell_dim = 0;   // cells along one side of a ring's XZ grid
	int far_cells = 0;      // far_cell_dim^2, per ring
	float far_reach_m = 0;  // outer radius of the last far ring
	// Brick-list capacity: the near columns' reserve plus every far ring's cell grid.
	int max_bricks = 0;
	// Threads stage 1 dispatches: one per near column, then one per far cell.
	int dispatch_threads = 0;
	// Diagnostic upper bound on blades this layout can produce, already capped by
	// GrassSettings::max_blades. The instance buffer is sized from settings.max_blades,
	// not from this -- see the design doc's failure-modes section.
	int estimated_blades = 0;
	GrassParams params{};
};

// `settings` is clamped internally, so a caller may pass an unclamped snapshot.
// `view_proj` is column-major, the same order the compositor builds for the raymarcher.
GrassLayout grass_layout(const GrassSettings &settings, const float camera[3],
		const float view_proj[16]);

// Ring index for a distance, or -1 past the reach.
int grass_ring_of(const GrassLayout &l, float distance_m);

} // namespace ve
