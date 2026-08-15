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
	int32_t dims[4];          // world size in REGIONS (xyz), unused
	int32_t region_origin[4]; // world origin in REGIONS = origin_bricks / kRegionBricks
	int32_t atlas_bricks[4];  // atlas grid in bricks

	// Basis from position/forward/up-hint; tan fov = 0, max_dist = 200.
	static CameraParams looking_at(float ox, float oy, float oz,
			float fx, float fy, float fz, float ux, float uy, float uz);
};

static_assert(sizeof(CameraParams) == 128);

} // namespace ve
