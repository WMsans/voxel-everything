#[compute]
#version 460

// HBAO: horizon-based ambient occlusion over the raymarched G-buffer. For each pixel the
// horizon elevation is swept along a handful of tangent-plane directions; geometry that
// rises above the horizon occludes a slice of the hemisphere. The result multiplies the
// deferred pass's AMBIENT term only — sun, spec and rim are untouched, exactly like the
// material-level AO baked into albedo.rgb.
//
// Full resolution by design: HBAO is angle-based, so it needs no noise-and-blur pass to
// hide tap banding (a half-res + blur chain would fight the cel look for no win).

#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 5
#include "common.glslh"
#include "shade.glslh"
#include "beauty_camera.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gb_surface;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2, r8) writeonly uniform image2D out_ssao;

layout(push_constant, std430) uniform Push {
	ivec4 dims;  // xy = target size, z = march steps per direction, w = unused
	vec4 params; // x = world-space radius, y = strength, zw = unused
} pc;

const int DIRECTIONS = 6;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) * bcam.screen.zw;

	// Sky has no surface behind it: pass full ambient so the horizon gradient is never
	// darkened by screen-space occlusion.
	float depth = texture(gb_depth, uv).r;
	vec4 g1 = texture(gb_surface, uv);
	if (depth <= 0.0 || g1.z < 0.5) {
		imageStore(out_ssao, px, vec4(1.0));
		return;
	}

	vec3 p = beauty_world_from_depth(uv, depth);
	vec3 n = oct_decode(g1.xy);

	// Tangent-plane frame around the surface normal: every sweep direction lies in it.
	vec3 up = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 t1 = normalize(cross(up, n));
	vec3 t2 = cross(n, t1);

	// One Bayer offset per pixel rotates all directions together: enough to break the
	// directional banding a fixed sweep would show on flat ground, no blur needed.
	const float kPi = 3.14159265;
	float jitter = bayer4(px);
	float radius_sq = pc.params.x * pc.params.x;
	float occlusion = 0.0;
	for (int d = 0; d < DIRECTIONS; d++) {
		float phi = (float(d) + jitter) * kPi / float(DIRECTIONS);
		vec3 dir3 = t1 * cos(phi) + t2 * sin(phi);
		// The direction's pixel-space step: project p and p + dir3 * radius once per
		// direction, so perspective and aspect are exact rather than approximated.
		vec2 pu, qu;
		float pd, qd;
		beauty_project(p, pu, pd);
		if (!beauty_project(p + dir3 * pc.params.x, qu, qd)) continue;
		vec2 step_px = (qu - pu) * vec2(pc.dims.xy);
		// The 5 m radius projects to under a pixel past ~60 m, and the sampler is nearest:
		// with the largest tap offset below half a texel, every tap along this direction
		// resolves to the centre texel, so dv is ~0 and the len_sq guard below rejects all
		// of them anyway. Reject the whole direction here instead and skip pc.dims.z
		// texture fetches plus pc.dims.z unprojections per direction. Output-preserving.
		if (dot(step_px, step_px) < 0.25) continue;

		// Below-tangent geometry is already accounted for by the normal itself.
		float h_max = asin(clamp(dot(n, dir3), -1.0, 1.0));

		// h(u,v,d) = inv_view_proj * vec4(2u-1, 2v-1, d, 1) is affine in (u,v,d), and along
		// this direction u and v advance linearly. Evaluate the (u,v) half once and step it;
		// only the depth column varies per tap. Same value, one mad instead of a mat4 mul.
		vec2 duv = step_px * bcam.screen.zw / float(pc.dims.z);
		vec2 uv0 = (vec2(px) + 0.5) * bcam.screen.zw;
		vec4 h0 = bcam.inv_view_proj * vec4(uv0.x * 2.0 - 1.0, uv0.y * 2.0 - 1.0, 0.0, 1.0);
		vec4 dh = bcam.inv_view_proj[0] * (2.0 * duv.x) + bcam.inv_view_proj[1] * (2.0 * duv.y);
		vec4 col_d = bcam.inv_view_proj[2];

		for (int i = 1; i <= pc.dims.z; i++) {
			vec2 suv = uv0 + duv * float(i);
			float sdepth = texture(gb_depth, suv).r;
			if (sdepth <= 0.0) continue;
			vec4 h = h0 + dh * float(i) + col_d * sdepth;
			vec3 sp = h.xyz / (abs(h.w) < 1e-9 ? 1e-9 : h.w);
			vec3 dv = sp - p;
			float len_sq = dot(dv, dv);
			if (len_sq < 1e-6 || len_sq > 4.0 * radius_sq) continue;
			float dn = dot(dv, n);
			float tangential = length(dv - n * dn);
			if (tangential < 1e-4) continue;
			float sin_h = clamp(dn / tangential, -1.0, 1.0);
			if (sin_h > h_max) {
				float r_sq = clamp(len_sq / radius_sq, 0.0, 1.0);
				occlusion += (sin_h - h_max) * (1.0 - r_sq);
				h_max = sin_h;
			}
		}
	}
	float ao = clamp(1.0 - occlusion * pc.params.y / float(DIRECTIONS), 0.0, 1.0);
	imageStore(out_ssao, px, vec4(ao));
}
