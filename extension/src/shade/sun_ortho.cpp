#include "shade/sun_ortho.h"
#include <algorithm>
#include <cmath>

namespace {

void cross3(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

float norm3(float v[3]) {
	const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0.0f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
	return l;
}

} // namespace

namespace ve {

namespace {

// Compose the reverse-Z ortho from a box already expressed in LIGHT space (x along `r`,
// y along `u`, z along `l`). Both fitters below end here, so the depth convention and the
// row layout are written down exactly once. Arithmetic is double because an unbounded world
// puts the camera kilometres from the origin, where a float `-2*mn/w - 1` is quantised
// coarsely enough to move the map by a fraction of a texel on its own.
ve::SunOrtho compose(const float l[3], const float r[3], const float u[3], const double mn[3],
		const double mx[3], double texel_world) {
	ve::SunOrtho o;
	const double w = mx[0] - mn[0];
	const double h = mx[1] - mn[1];
	const double d = mx[2] - mn[2];
	if (!(w > 0.0) || !(h > 0.0) || !(d > 0.0) || !(texel_world > 0.0)) return o;

	// Rows of the matrix. Row 2 is the reverse-Z remap: depth = (mx_l - l.p) / d, so a point
	// at the near (sunward) extreme is 1 and the far one is 0.
	const double row0[4] = {2.0 * r[0] / w, 2.0 * r[1] / w, 2.0 * r[2] / w,
			-2.0 * mn[0] / w - 1.0};
	const double row1[4] = {2.0 * u[0] / h, 2.0 * u[1] / h, 2.0 * u[2] / h,
			-2.0 * mn[1] / h - 1.0};
	const double row2[4] = {-l[0] / d, -l[1] / d, -l[2] / d, mx[2] / d};
	const double row3[4] = {0.0, 0.0, 0.0, 1.0};
	const double *rows[4] = {row0, row1, row2, row3};
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++)
			o.view_proj[c * 4 + rr] = static_cast<float>(rows[rr][c]);

	o.texel_world = static_cast<float>(texel_world);
	o.depth_range = static_cast<float>(d);
	o.valid = true;
	return o;
}

// The half of sun_ortho() that does not care where the basis came from. `l` points AWAY from
// the sun, `r`/`u` complete a right-handed orthonormal set with it.
ve::SunOrtho fit_box(const float l[3], const float r[3], const float u[3],
		const float lo[3], const float hi[3], int map_size) {
	double mn[3] = {1e30, 1e30, 1e30};
	double mx[3] = {-1e30, -1e30, -1e30};
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1],
				(i & 4) ? hi[2] : lo[2]};
		const double c[3] = {double(p[0]) * r[0] + double(p[1]) * r[1] + double(p[2]) * r[2],
				double(p[0]) * u[0] + double(p[1]) * u[1] + double(p[2]) * u[2],
				double(p[0]) * l[0] + double(p[1]) * l[1] + double(p[2]) * l[2]};
		for (int a = 0; a < 3; a++) {
			mn[a] = std::min(mn[a], c[a]);
			mx[a] = std::max(mx[a], c[a]);
		}
	}
	return compose(l, r, u, mn, mx, std::max(mx[0] - mn[0], mx[1] - mn[1]) / double(map_size));
}

// The camera-following fit: the SPHERE of `radius` around `center`, with the light-space
// origin snapped to whole shadow texels.
//
// Both halves of that sentence are load-bearing, and neither is optional.
//
// THE SPHERE, not the enclosing cube. A cube's light-space width is 2R(|r.x|+|r.y|+|r.z|),
// which is a function of where the sun is -- up to sqrt(3) times the sphere's, and different
// every time the light turns. A sphere's silhouette is the same circle from every direction,
// so this extent, and with it the texel size, depends on neither the sun nor the camera.
// That constant spacing is not a bonus: it is the grid the snap below snaps TO. It also
// happens to be the set the shadow cut actually draws (LodTree::shadow_visit keeps chunks
// within stream_radius_m of the camera -- a distance test, not a box test), so the cube was
// spending 40% of every texel on eight corners that hold no geometry.
//
// THE SNAP. A map fitted to the raw camera position slides its texel grid across the world
// continuously, so every silhouette in it re-quantises every frame and the shadows crawl --
// measured at 0.26 texel of drift per metre travelled, with a 2.7 m texel. Quantising the
// min corner pins the grid in world space instead: camera motion may translate the map by
// WHOLE texels and by nothing else, which leaves a static object's shadow bit-identical
// frame to frame. It also restores the invariant SunShadowPass::build() relies on -- the
// matrix is now constant inside one texel of travel, so camera motion no longer trips the
// moved-sun check into a full 2048^2 rebuild every frame.
//
// A sun that TURNS still rotates the grid and re-quantises everything; that is inherent (the
// map is rebuilt for it anyway) and is not what shimmers when you walk.
ve::SunOrtho fit_sphere(const float l[3], const float r[3], const float u[3],
		const float center[3], float radius, int map_size) {
	if (!(radius > 0.0f) || map_size <= 1) return ve::SunOrtho();
	// map_size - 1, not map_size: the snap moves the min corner DOWN by up to one texel, so
	// the map must be a texel wider than the sphere to still cover its far side.
	const double texel = 2.0 * double(radius) / double(map_size - 1);
	const double extent = texel * double(map_size);
	const double c[3] = {
			double(center[0]) * r[0] + double(center[1]) * r[1] + double(center[2]) * r[2],
			double(center[0]) * u[0] + double(center[1]) * u[1] + double(center[2]) * u[2],
			double(center[0]) * l[0] + double(center[1]) * l[1] + double(center[2]) * l[2]};

	// Depth gets no grid -- nothing samples it by texel -- but it is quantised all the same,
	// so that sub-texel camera motion leaves the matrix bit-identical rather than merely
	// visually identical. 4R spans the sphere with R of margin on each side, which is deeper
	// than the cube this replaces on BOTH sides (a cube reaches at most sqrt(3)R along the
	// light axis), so no caster that used to reach the map is clipped out of it now. Depth
	// margin is free: deferred.comp.glsl scales its bias by texel/depth_range, so the bias
	// in metres does not move when the range does.
	const double half_depth = 2.0 * double(radius);
	const double mn[3] = {std::floor((c[0] - radius) / texel) * texel,
			std::floor((c[1] - radius) / texel) * texel,
			std::ceil((c[2] + half_depth) / texel) * texel - 2.0 * half_depth};
	const double mx[3] = {mn[0] + extent, mn[1] + extent, mn[2] + 2.0 * half_depth};
	return compose(l, r, u, mn, mx, texel);
}

// Shared preamble: normalize the light axis. Returns false if the direction is unusable.
bool light_axis(const float sun_dir[3], float l[3]) {
	float f[3] = {sun_dir[0], sun_dir[1], sun_dir[2]};
	if (norm3(f) <= 0.0f) return false;
	// Light-space +z points AWAY from the sun, so depth grows with distance from it and the
	// reverse-Z remap is a single subtraction.
	l[0] = -f[0];
	l[1] = -f[1];
	l[2] = -f[2];
	return true;
}

// Any hint not parallel to the light. kSunDir is well off vertical, so world up works; the
// fallback exists so a sun straight overhead does not collapse the basis. It stops division
// by zero; it does not make the result stable near the zenith, which is why the
// explicit-basis overloads exist.
bool derived_basis(const float l[3], float r[3], float u[3]) {
	float up[3] = {0.0f, 1.0f, 0.0f};
	cross3(l, up, r);
	if (norm3(r) < 1e-4f) {
		up[0] = 1.0f;
		up[1] = 0.0f;
		up[2] = 0.0f;
		cross3(l, up, r);
		if (norm3(r) < 1e-4f) return false;
	}
	cross3(r, l, u);
	return norm3(u) > 0.0f;
}

// Re-orthonormalize a supplied basis against the light axis. A scene node's basis is already
// orthonormal, but it is authored data: never trust it to be exactly so.
bool given_basis(const float l[3], const float right[3], const float up[3], float r[3],
		float u[3]) {
	float uu[3] = {up[0], up[1], up[2]};
	if (norm3(uu) <= 0.0f) return false;
	cross3(l, uu, r);
	if (norm3(r) < 1e-4f) {
		// `up` is parallel to the light; fall back to the supplied right vector.
		r[0] = right[0];
		r[1] = right[1];
		r[2] = right[2];
		if (norm3(r) <= 0.0f) return false;
	}
	cross3(r, l, u);
	return norm3(u) > 0.0f;
}

bool valid_box(const float lo[3], const float hi[3], int map_size) {
	if (map_size <= 0) return false;
	for (int a = 0; a < 3; a++)
		if (!(hi[a] > lo[a])) return false;
	return true;
}

} // namespace

SunOrtho sun_ortho(const float sun_dir[3], const float lo[3], const float hi[3], int map_size) {
	float l[3], r[3], u[3];
	if (!valid_box(lo, hi, map_size) || !light_axis(sun_dir, l) || !derived_basis(l, r, u))
		return SunOrtho();
	return fit_box(l, r, u, lo, hi, map_size);
}

SunOrtho sun_ortho(const float sun_dir[3], const float right[3], const float up[3],
		const float lo[3], const float hi[3], int map_size) {
	float l[3], r[3], u[3];
	if (!valid_box(lo, hi, map_size) || !light_axis(sun_dir, l) ||
			!given_basis(l, right, up, r, u))
		return SunOrtho();
	return fit_box(l, r, u, lo, hi, map_size);
}

SunOrtho sun_ortho_sphere(const float sun_dir[3], const float center[3], float radius,
		int map_size) {
	float l[3], r[3], u[3];
	if (!light_axis(sun_dir, l) || !derived_basis(l, r, u)) return SunOrtho();
	return fit_sphere(l, r, u, center, radius, map_size);
}

SunOrtho sun_ortho_sphere(const float sun_dir[3], const float right[3], const float up[3],
		const float center[3], float radius, int map_size) {
	float l[3], r[3], u[3];
	if (!light_axis(sun_dir, l) || !given_basis(l, right, up, r, u)) return SunOrtho();
	return fit_sphere(l, r, u, center, radius, map_size);
}

} // namespace ve
