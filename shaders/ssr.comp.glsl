#[compute]
#version 460
#include "common.glslh"
#include "shade.glslh"
layout(local_size_x = 8, local_size_y = 8) in;

#ifndef SSR_APPLY
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 6
#include "beauty_camera.glslh"
layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D gb_surface;
layout(set = 0, binding = 3) uniform sampler2D gb_depth;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness;
layout(set = 0, binding = 5, rgba16f) writeonly uniform image2D out_reflection;
layout(push_constant, std430) uniform Push {
	ivec4 dims;
	vec4 params;
} pc;

bool receiver(vec2 uv, float depth, out vec3 p, out vec3 n, out float gloss) {
	p = beauty_world_from_depth(uv, depth);
	vec4 g = texture(gb_surface, uv);
	float gd = texture(gb_depth, uv).r;
	if (g.z >= 0.5 && abs(gd - depth) <= 1e-5) {
		n = oct_decode(g.xy);
		gloss = clamp(g.w, 0.0, 1.0);
		return true;
	}
	// Dynamic objects are absent from gb_surface. Depth derivatives provide a world-space
	// reflection plane; normal_roughness contributes roughness only. Without that optional
	// buffer the object is still reflected by terrain but is not itself an SSR receiver.
	if (pc.dims.w == 0) return false;
	vec2 dx = vec2(bcam.screen.z, 0.0), dy = vec2(0.0, bcam.screen.w);
	vec2 ux = clamp(uv + dx, vec2(0.0), vec2(1.0));
	vec2 uy = clamp(uv + dy, vec2(0.0), vec2(1.0));
	float zx = texture(scene_depth, ux).r, zy = texture(scene_depth, uy).r;
	if (zx <= 0.0 || zy <= 0.0) return false;
	vec3 px = beauty_world_from_depth(ux, zx), py = beauty_world_from_depth(uy, zy);
	if (distance(px, p) > 2.0 || distance(py, p) > 2.0) return false;
	n = normalize(cross(px - p, py - p));
	if (dot(n, bcam.cam.xyz - p) < 0.0) n = -n;
	gloss = clamp(1.0 - texture(normal_roughness, uv).a, 0.0, 1.0);
	return !isnan(n.x) && !isnan(n.y) && !isnan(n.z) &&
		!isinf(n.x) && !isinf(n.y) && !isinf(n.z);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	float depth = texture(scene_depth, uv).r;
	vec3 p, n; float gloss;
	if (depth <= 0.0 || !receiver(uv, depth, p, n, gloss) || gloss <= 0.0) {
		imageStore(out_reflection, px, vec4(0.0)); return;
	}
	vec3 v = normalize(bcam.cam.xyz - p);
	vec3 rd = normalize(reflect(-v, n));
	vec2 hit_uv = vec2(0.0); bool hit = false;
	float jitter = bayer4(px);
	for (int i = 0; i < pc.dims.z; i++) {
		float t = (float(i) + 0.5 + jitter) / float(max(pc.dims.z, 1));
		vec3 q = p + n * pc.params.y + rd * (pc.params.x * t);
		vec2 quv; float qdepth;
		if (!beauty_project(q, quv, qdepth)) break;
		if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;
		float sd = texture(scene_depth, quv).r;
		if (sd <= 0.0) continue;
		vec3 sp = beauty_world_from_depth(quv, sd);
		if (distance(sp, bcam.cam.xyz) < distance(q, bcam.cam.xyz) - 0.02 &&
				distance(sp, q) <= pc.params.z) { hit_uv = quv; hit = true; break; }
	}
	if (!hit) { imageStore(out_reflection, px, vec4(0.0)); return; }
	float edge = clamp(min(min(hit_uv.x, hit_uv.y),
		min(1.0 - hit_uv.x, 1.0 - hit_uv.y)) * 12.0, 0.0, 1.0);
	float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
	float weight = clamp(pc.params.w * fresnel * gloss * edge, 0.0, 0.85);
	imageStore(out_reflection, px, vec4(texture(scene_color, hit_uv).rgb, weight));
}
#else
layout(set = 0, binding = 0) uniform sampler2D reflection_tex;
layout(set = 0, binding = 1, rgba16f) uniform image2D scene_color;
layout(push_constant, std430) uniform Push { ivec4 dims; } pc;
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	vec4 r = texture(reflection_tex, uv);
	vec4 c = imageLoad(scene_color, px);
	imageStore(scene_color, px, vec4(mix(c.rgb, r.rgb, r.a), c.a));
}
#endif
