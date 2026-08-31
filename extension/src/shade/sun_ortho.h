#pragma once

namespace ve {

// A world-covering reverse-Z orthographic projection for the sun, in the same column-major
// layout Godot's Projection uses (element (row r, column c) is view_proj[c * 4 + r]).
//
// It is STATIC. This world is bounded and the sun does not move, so there is no camera to
// follow, no texel snapping to do and no shimmer to fight -- which is the whole reason a
// single 2048 map is enough for a 4 km world.
struct SunOrtho {
	float view_proj[16] = {};
	float texel_world = 0.0f; // one shadow texel, in world metres, in light space
	float depth_range = 0.0f; // light-space depth extent, in world metres
	bool valid = false;
};

// `sun_dir` points TOWARD the sun (ve::kSunDir). `lo`/`hi` are the world AABB.
SunOrtho sun_ortho(const float sun_dir[3], const float lo[3], const float hi[3], int map_size);

} // namespace ve
