#pragma once
#include <cstdint>

namespace ve {

// 128-byte push-constant block shared by raymarch.comp.glsl.
struct CameraParams {
	float cam_pos[4];
	float cam_right[4];
	float cam_up[4];
	float cam_fwd[4];
	float params[4];   // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	int32_t dims[4];   // world dims in bricks (xyz), unused

	// Basis from position/forward/up-hint; tan fov = 0, max_dist = 200.
	static CameraParams looking_at(float ox, float oy, float oz,
			float fx, float fy, float fz, float ux, float uy, float uz);
};

// NOTE (Task 10 deviation): the brief asserts 128 bytes, but the specified members
// (5x float[4] + int32_t[4]) sum to 96. The raymarch.comp.glsl push-constant block is
// 6 x vec4 = 96 bytes, and Godot sizes the pipeline-layout push-constant range from the
// shader reflection, so the C++ block must match exactly (96) — pushing 128 bytes into a
// 96-byte declared range is undefined per Vulkan. 96 still satisfies the guaranteed
// 128-byte minimum push-constant size.
static_assert(sizeof(CameraParams) == 96);

} // namespace ve
