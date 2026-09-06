#pragma once

namespace ve {

// A reverse-Z orthographic projection for the sun, in the same column-major layout Godot's
// Projection uses (element (row r, column c) is view_proj[c * 4 + r]).
//
// It used to be STATIC, back when the world was a fixed box and the sun did not move: no
// camera to follow, no texel snapping to do and no shimmer to fight. An unbounded world has
// no box, so the shipping fit is sun_ortho_sphere() below, which follows the camera and has
// to earn every one of those properties back.
struct SunOrtho {
	float view_proj[16] = {};
	float texel_world = 0.0f; // one shadow texel, in world metres, in light space
	float depth_range = 0.0f; // light-space depth extent, in world metres
	bool valid = false;
};

// `sun_dir` points TOWARD the sun (ve::kSunDir). `lo`/`hi` are the world AABB.
SunOrtho sun_ortho(const float sun_dir[3], const float lo[3], const float hi[3], int map_size);

// As above, but with the light's own orthonormal basis supplied rather than derived from a
// world-up hint. Deriving is well defined but badly conditioned near the zenith, where a small
// azimuth change swings the basis through a large rotation and spins the shadow map. A scene
// light carries a basis that rotates continuously, so an animated sun should pass it here.
// `right` and `up` must both be non-degenerate; they are re-orthonormalized against `sun_dir`.
SunOrtho sun_ortho(const float sun_dir[3], const float right[3], const float up[3],
		const float lo[3], const float hi[3], int map_size);

// THE SHIPPING FIT, for a world with no box to fit: the sphere of `radius` around `center`
// -- the camera, and the same set the LoD shadow cut draws -- with the light-space origin
// snapped to whole shadow texels so the grid stays pinned in world space while the camera
// moves. Camera motion translates the map by whole texels and by nothing else, so a static
// object's shadow is bit-identical frame to frame instead of crawling. See fit_sphere() in
// the .cpp for why the sphere and the snap are one fix rather than two.
//
// texel_world is 2 * radius / (map_size - 1) and depends on nothing else -- not the sun's
// direction, not the camera's position.
SunOrtho sun_ortho_sphere(const float sun_dir[3], const float center[3], float radius,
		int map_size);
// As above, with the light's own orthonormal basis rather than one derived from a world-up
// hint -- the zenith-continuity argument of the box overloads applies unchanged.
SunOrtho sun_ortho_sphere(const float sun_dir[3], const float right[3], const float up[3],
		const float center[3], float radius, int map_size);

} // namespace ve
