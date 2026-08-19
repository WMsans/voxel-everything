#[compute]
#version 460

#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 5
#include "common.glslh"
#include "shade.glslh"
#include "beauty_camera.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gb_surface;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D history;
layout(set = 0, binding = 3) uniform sampler2D prev_ssgi;
layout(set = 0, binding = 4, rgba16f) writeonly uniform image2D out_ssgi;

layout(push_constant, std430) uniform Push {
	mat4 prev_view_proj;
	ivec4 dims;
	vec4 params;
} pc;

vec2 spiral_tap(int i, int n, float rot) {
	float t = (float(i) + 0.5) / float(n);
	float a = t * 6.28318531 * 3.0 + rot;
	return vec2(cos(a), sin(a)) * sqrt(t);
}

bool previous_uv(vec3 p, out vec2 uv) {
	vec4 clip = pc.prev_view_proj * vec4(p, 1.0);
	if (clip.w <= 0.0) return false;
	vec3 ndc = clip.xyz / clip.w;
	uv = ndc.xy * 0.5 + 0.5;
	return all(greaterThanEqual(uv, vec2(0.0))) &&
			all(lessThanEqual(uv, vec2(1.0)));
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);

	if (pc.dims.w == 0) {
		imageStore(out_ssgi, px, vec4(0.0));
		return;
	}

	float depth = texture(gb_depth, uv).r;
	vec4 g1 = texture(gb_surface, uv);
	if (depth <= 0.0 || g1.z < 0.5) {
		imageStore(out_ssgi, px, vec4(0.0));
		return;
	}
	vec3 p = beauty_world_from_depth(uv, depth);
	vec3 n = oct_decode(g1.xy);

	float rot = bayer4(px) * 6.28318531;
	vec3 sum = vec3(0.0);
	float weight = 0.0;
	for (int i = 0; i < pc.dims.z; i++) {
		vec2 off = spiral_tap(i, pc.dims.z, rot) * pc.params.x * bcam.screen.z * 40.0 /
				max(distance(p, bcam.cam.xyz), 1.0);
		vec2 suv = clamp(uv + off, vec2(0.0), vec2(1.0));
		float sdepth = texture(gb_depth, suv).r;
		if (sdepth <= 0.0) continue;
		vec3 sp = beauty_world_from_depth(suv, sdepth);
		vec3 dir = sp - p;
		float dist = length(dir);
		if (dist < 1e-3 || dist > pc.params.x) continue;
		dir /= dist;
		float cosine = dot(n, dir);
		if (cosine <= 0.0) continue;
		vec3 sn = oct_decode(texture(gb_surface, suv).xy);
		if (dot(sn, -dir) <= 0.0) continue;
		vec2 history_uv;
		if (!previous_uv(sp, history_uv)) continue;
		float falloff = 1.0 / (1.0 + dist * dist);
		sum += texture(history, history_uv).rgb * cosine * falloff;
		weight += 1.0;
	}
	vec3 gi = weight > 0.0 ? sum / weight * pc.params.z : vec3(0.0);

	vec2 puv;
	bool reproj = previous_uv(p, puv);
	if (reproj) {
		vec3 lo = gi;
		vec3 hi = gi;
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++) {
				ivec2 npx = clamp(px + ivec2(x, y), ivec2(0), pc.dims.xy - 1);
				vec2 nuv = (vec2(npx) + 0.5) / vec2(pc.dims.xy);
				float ndepth = texture(gb_depth, nuv).r;
				if (ndepth <= 0.0) continue;
				vec3 np = beauty_world_from_depth(nuv, ndepth);
				vec2 npuv;
				if (!previous_uv(np, npuv)) continue;
				vec3 s = texture(prev_ssgi, npuv).rgb;
				lo = min(lo, s);
				hi = max(hi, s);
			}
		vec3 prev = clamp(texture(prev_ssgi, puv).rgb, lo, hi);
		gi = mix(gi, prev, pc.params.y);
	}
	imageStore(out_ssgi, px, vec4(gi, 1.0));
}
