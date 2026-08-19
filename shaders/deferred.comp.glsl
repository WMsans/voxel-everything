#[compute]
#version 460

#define MATERIAL_LAYERS 16
layout(set = 0, binding = 8) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 9) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gb_albedo;
layout(set = 0, binding = 1) uniform sampler2D gb_surface;
layout(set = 0, binding = 2) uniform sampler2D gb_depth;
layout(set = 0, binding = 3) uniform sampler2D ssgi_tex;
layout(set = 0, binding = 4) uniform sampler2D sun_map;
layout(set = 0, binding = 5, rgba16f) writeonly uniform image2D out_lit;
layout(set = 0, binding = 6, std140) uniform SunBlock {
	mat4 view_proj;
	vec4 params;
} sun;

layout(push_constant, std430) uniform Push {
	mat4 inv_view_proj;
	vec4 cam;
	vec4 sky;
	uvec4 flags;
} pc;

float sun_map_visibility(vec3 wpos, float ndl) {
	vec4 c = sun.view_proj * vec4(wpos, 1.0);
	if (c.w <= 0.0) return 1.0;
	vec3 p = c.xyz / c.w;
	vec2 uv = p.xy * 0.5 + 0.5;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
	float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
	float bias = sun.params.x * (0.5 + 2.0 * slope) + 0.0015;
	return (p.z + bias >= texture(sun_map, uv).r) ? 1.0 : 0.0;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_lit);
	if (px.x >= size.x || px.y >= size.y) return;

	if (pc.flags.y == 1u) {
		vec3 albedo = pc.inv_view_proj[0].xyz;
		vec3 ambient = pc.inv_view_proj[1].xyz;
		vec4 t = pc.inv_view_proj[2];
		vec2 ag = pc.inv_view_proj[3].xy;
		imageStore(out_lit, px,
				vec4(cel_shade(albedo, ambient, t.x, t.y, t.z, t.w, ag.x, ag.y), 1.0));
		return;
	}

	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec4 g0 = texelFetch(gb_albedo, px, 0);
	vec4 g1 = texelFetch(gb_surface, px, 0);
	uint mat = uint(g1.z + 0.5);
	if (mat == 0u && pc.flags.y != 2u) {
		imageStore(out_lit, px, vec4(g0.rgb, 1.0));
		return;
	}

	float depth = texelFetch(gb_depth, px, 0).r;
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0);
	vec4 h = pc.inv_view_proj * vec4(ndc, depth, 1.0);
	vec3 wpos = h.xyz / (abs(h.w) < 1e-9 ? 1e-9 : h.w);

	if (pc.flags.y == 2u) {
		imageStore(out_lit, px, vec4(wpos, 1.0));
		return;
	}

	vec3 n = oct_decode(g1.xy);
	vec3 v = normalize(pc.cam.xyz - wpos);
	float ndl = dot(n, SUN_DIR);
	float ndv = dot(n, v);
	float ndh = dot(n, normalize(SUN_DIR + v));
	float shadow = g0.a;
	if ((pc.flags.x & BEAUTY_SUN_MAP) != 0u)
		shadow = min(shadow, sun_map_visibility(wpos, ndl));
	vec3 ambient = pc.sky.rgb;
	if ((pc.flags.x & BEAUTY_SSGI) != 0u) ambient += texture(ssgi_tex, uv).rgb;
	imageStore(out_lit, px,
			vec4(cel_shade(g0.rgb, ambient, ndl, ndv, ndh, shadow, 1.0, g1.w), 1.0));
}
