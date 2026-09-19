#pragma once
#include "leaves/leaf_settings.h"
#include "world/region.h" // ve::IVec3

namespace ve {

// Placement lattice pitch, metres. MUST equal the `cell` param of the trees stage
// (assets/pipelines/*.pipeline). The leaf scatter walks the same lattice the terrain stage
// placed trunks on; a disagreement here draws canopies over empty ground.
// ponytail: duplicated as a constant because the layout is computed CPU-side before the
// pipeline UBO is readable. If the trees stage ever gains a non-default cell, plumb the
// resolved param through instead of raising this.
inline constexpr float kLeafCellM = 14.0f;

// Uploaded to a uniform buffer and mirrored by `LeafParams` in shaders/leaf.glslh. Laid out
// as sixteen vec4 (256 bytes) so std140 padding cannot disagree with the C++ struct;
// test_leaf_layout pins both the size and the first three floats.
//
// Order matters and is asserted: cam_pos first, so a shader reading params.cam.xyz gets the
// camera without an offset table. Same convention as ve::GrassParams.
struct LeafParams {
	float cam[4];            // xyz camera position, w reach_m
	float planes[6][4];      // frustum planes, inward, normalised: xyz normal, w distance
	int32_t cell_min[4];     // x, z inclusive cell coordinate; y unused; w total cells
	int32_t cell_dim[4];     // x, z cell counts; y unused; w unused
	float tree[4];           // cell size, density, crown_radius, trunk_radius
	float shape[4];          // trunk_height, branch_radius_min, max_slope, surface_y
	float clump[4];          // radius_near, shell_min, canopy_roundness, crown_shade
	float wind[4];           // strength, speed, scale, time_seconds
	float style[4];          // gloss, hue_jitter, unused, unused
	int32_t limits[4];       // max_clumps, max_trees, clumps_per_tree, unused
	// Reserved. The block is sixteen vec4 by contract (the comment above and
	// test_leaf_layout's 256-byte CHECK); the ten fields above are fifteen, so this is the
	// sixteenth. Zero-filled, mirror generated into blocks.glslh with the rest; the first
	// shader-side consumer reassigns it by name.
	float spare[4];
};

struct LeafLayout {
	float cell_size_m = kLeafCellM;
	float reach_m = 0.0f;
	IVec3 cell_min{};   // x and z used; y is zero
	IVec3 cell_dim{};   // x and z used; y is zero
	int clumps_per_tree = 0;
	float clump_radius_m = 0.0f;
	int max_trees = 0;
	int max_clumps = 0;
	// Threads stage 1 dispatches: one per cell in the XZ box.
	int dispatch_threads = 0;
	// Diagnostic upper bound on clumps this layout can produce, already capped by
	// LeafSettings::max_clumps. The instance buffer is sized from settings.max_clumps, not
	// from this -- see the design doc's failure-modes section.
	int estimated_clumps = 0;
	LeafParams params{};
};

// `settings` is clamped internally, so a caller may pass an unclamped snapshot.
// `view_proj` is column-major, the same order the compositor builds for the raymarcher.
LeafLayout leaf_layout(const LeafSettings &settings, const float camera[3],
		const float view_proj[16]);

// Clumps a tree at this distance gets, 0 past the reach. Falls monotonically.
int leaf_clump_budget(const LeafLayout &l, float distance_m);

// Card radius at this distance. Grows exactly as fast as the budget falls, so
// budget * radius^2 -- the total card area per tree -- is flat in distance. That flatness is
// the density LOD: a distant crown keeps its silhouette rather than thinning into dots.
float leaf_clump_radius(const LeafLayout &l, float distance_m);

} // namespace ve
