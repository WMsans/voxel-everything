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

SunOrtho sun_ortho(const float sun_dir[3], const float lo[3], const float hi[3], int map_size) {
	SunOrtho o;
	if (map_size <= 0) return o;
	for (int a = 0; a < 3; a++)
		if (!(hi[a] > lo[a])) return o;

	float f[3] = {sun_dir[0], sun_dir[1], sun_dir[2]};
	if (norm3(f) <= 0.0f) return o;
	// Light-space +z points AWAY from the sun, so depth grows with distance from it and the
	// reverse-Z remap below is a single subtraction.
	const float l[3] = {-f[0], -f[1], -f[2]};

	// Any hint not parallel to the light. kSunDir is well off vertical, so world up works;
	// the fallback exists so a future sun straight overhead does not collapse the basis.
	float up[3] = {0.0f, 1.0f, 0.0f};
	float r[3];
	cross3(l, up, r);
	if (norm3(r) < 1e-4f) {
		up[0] = 1.0f;
		up[1] = 0.0f;
		up[2] = 0.0f;
		cross3(l, up, r);
		if (norm3(r) < 1e-4f) return o;
	}
	float u[3];
	cross3(r, l, u);
	norm3(u);

	float mn[3] = {1e30f, 1e30f, 1e30f};
	float mx[3] = {-1e30f, -1e30f, -1e30f};
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1],
				(i & 4) ? hi[2] : lo[2]};
		const float c[3] = {p[0] * r[0] + p[1] * r[1] + p[2] * r[2],
				p[0] * u[0] + p[1] * u[1] + p[2] * u[2],
				p[0] * l[0] + p[1] * l[1] + p[2] * l[2]};
		for (int a = 0; a < 3; a++) {
			mn[a] = std::min(mn[a], c[a]);
			mx[a] = std::max(mx[a], c[a]);
		}
	}
	const float w = mx[0] - mn[0];
	const float h = mx[1] - mn[1];
	const float d = mx[2] - mn[2];
	if (!(w > 0.0f) || !(h > 0.0f) || !(d > 0.0f)) return o;

	// Rows of the matrix. Row 2 is the reverse-Z remap: depth = (mx_l - l.p) / d, so a point
	// at the near (sunward) extreme is 1 and the far one is 0.
	const float row0[4] = {2.0f * r[0] / w, 2.0f * r[1] / w, 2.0f * r[2] / w,
			-2.0f * mn[0] / w - 1.0f};
	const float row1[4] = {2.0f * u[0] / h, 2.0f * u[1] / h, 2.0f * u[2] / h,
			-2.0f * mn[1] / h - 1.0f};
	const float row2[4] = {-l[0] / d, -l[1] / d, -l[2] / d, mx[2] / d};
	const float row3[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	const float *rows[4] = {row0, row1, row2, row3};
	for (int c = 0; c < 4; c++)
		for (int rr = 0; rr < 4; rr++)
			o.view_proj[c * 4 + rr] = rows[rr][c];

	o.texel_world = std::max(w, h) / static_cast<float>(map_size);
	o.depth_range = d;
	o.valid = true;
	return o;
}

} // namespace ve
