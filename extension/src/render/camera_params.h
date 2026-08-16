#pragma once
#include <cstdint>

namespace ve {

// Push-constant block shared by raymarch.comp.glsl. M2 grows it from 96 to exactly 128
// bytes — Vulkan's guaranteed minimum push-constant size, so still portable. The shader
// declares the same eight vec4s; Godot sizes the pipeline range from reflection, so the
// two sides must agree exactly (M1 errata 1).
struct CameraParams {
	float cam_pos[4];
	float cam_right[4];
	float cam_up[4];
	float cam_fwd[4];
	float params[4];          // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	int32_t dims[4];          // world size in REGIONS (xyz), w = live island count
	int32_t region_origin[4]; // world origin in REGIONS, w = island cull tiles per row
	int32_t atlas_bricks[4];  // atlas grid in bricks, w = island cull tile rows

	// The island cull grid rides in this struct rather than a second push constant so the
	// cull pass and the raymarcher project world points with the SAME camera arithmetic --
	// ndc = (dot(v, right) / (z * tan_x), dot(v, up) / (z * tan_y)) -- and can never disagree
	// about which tile a pixel is in. tiles-per-row 0 means "no mask": march every live
	// island, which is what the 1x1 debug probes (tan fov 0) do.

	// Basis from position/forward/up-hint; tan fov = 0, max_dist = 200.
	static CameraParams looking_at(float ox, float oy, float oz,
			float fx, float fy, float fz, float ux, float uy, float uz);
};

static_assert(sizeof(CameraParams) == 128);

} // namespace ve
