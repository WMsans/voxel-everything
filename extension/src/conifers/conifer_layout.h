#pragma once
#include "conifers/conifer_settings.h"
#include "leaves/leaf_layout.h" // LeafParams: the card raster's block
#include <cstddef>
#include <cstdint>

namespace ve {

// Uploaded to the conifer cull and card scatter; mirrored by CONIFER_PASS_FIELDS
// (shaders/generated/blocks.glslh). Thirteen vec4, cam first (the leaf convention).
struct ConiferPassParams {
	float cam[4];        // xyz world camera, w card_reach_m
	float planes[6][4];  // frustum planes, inward, normalised
	int32_t cell_min[4]; // x, z inclusive cell (SHIFTED space); w total cells
	int32_t cell_dim[4]; // x, z cell counts
	float reach[4];      // impostor_reach_m, clump_radius_m, tier_roundness, crown_shade
	float look[4];       // shell_min, unused...
	int32_t limits[4];   // max_clumps, max_card_trees, clumps_per_tree, max_impostors
	int32_t flags[4];    // x: raster mode (no sun march), the rest zero
};
static_assert(sizeof(ConiferPassParams) == 208, "ConiferPassParams is thirteen vec4");
static_assert(offsetof(ConiferPassParams, cam) == 0, "cam first");
static_assert(offsetof(ConiferPassParams, cell_min) == 112, "ConiferPassParams.cell_min");
static_assert(offsetof(ConiferPassParams, flags) == 192, "ConiferPassParams.flags");

struct ConiferLayout {
	float cell_size_m = 8.0f;
	int dispatch_threads = 0; // one cull thread per lattice cell in the box
	ConiferPassParams params{};
	LeafParams raster{};      // what the card raster and the imposter read for style
};

// cell_m is the resolved `conifers.cell` param. view_proj is column-major. The cell box is
// taken around camera + field offset (the lattice is in shifted space); params.cam stays the
// world camera. `settings` is clamped internally. raster.wind[3] (time) stays 0 -- run()'s.
ConiferLayout conifer_layout(const ConiferSettings &settings, float cell_m, const float camera[3],
		const float view_proj[16], float field_offset_x, float field_offset_z);

// Clumps a card tree at this distance gets: linear to an eighth at the card reach, as the
// leaf module's density LOD. conifer_pass.glslh's conifer_clump_budget is the GPU twin.
int conifer_clump_budget(int clumps_per_tree, float card_reach_m, float distance_m);

} // namespace ve
