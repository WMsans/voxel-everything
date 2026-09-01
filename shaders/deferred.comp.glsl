#[compute]
#version 460

#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 10
#define MATERIAL_LAYERS 16
layout(set = 0, binding = 8) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 9) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"
#include "sun_light.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gb_albedo;
layout(set = 0, binding = 1) uniform sampler2D gb_surface;
layout(set = 0, binding = 2) uniform sampler2D gb_depth;
layout(set = 0, binding = 3) uniform sampler2D ssgi_tex;
layout(set = 0, binding = 4) uniform sampler2D sun_map;
layout(set = 0, binding = 7) uniform sampler2D ssao_tex;
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
	// params.x is one shadow texel in world metres; params.y is the light-space depth range
	// in the same metres. p.z and the stored depth are normalized [0,1], so the bias must be
	// too. Scaling by params.x alone made the bias ~0.54 of the entire depth range at the
	// demo's world size, which reported every pixel lit and left the far field flat.
	float texel = sun.params.x / max(sun.params.y, 1e-6);
	float bias = texel * (0.5 + 2.0 * slope) + 0.0015;
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

	if (pc.flags.y == 3u) {
		float vis = ((pc.flags.x & BEAUTY_SUN_MAP) != 0u)
				? sun_map_visibility(pc.cam.xyz, 1.0) : 1.0;
		imageStore(out_lit, px, vec4(vis, vis, vis, 1.0));
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
	vec3 sun_dir = sun_light.dir.xyz;
	float ndl = dot(n, sun_dir);
	float ndv = dot(n, v);
	float ndh = dot(n, normalize(sun_dir + v));
	float shadow = g0.a;
	if ((pc.flags.x & BEAUTY_SUN_MAP) != 0u)
		shadow = min(shadow, sun_map_visibility(wpos, ndl));
	vec3 ambient = pc.sky.rgb;
	if ((pc.flags.x & BEAUTY_SSGI) != 0u) ambient += texture(ssgi_tex, uv).rgb;
	// HBAO multiplies the ambient term only: sun lighting, spec and rim keep their own
	// visibility terms. The pass's sky pixels are exactly 1.0, so horizons are untouched.
	float ao = 1.0;
	if ((pc.flags.x & BEAUTY_SSAO) != 0u) ao = texture(ssao_tex, uv).r;
	vec3 lit = cel_shade(g0.rgb, ambient, ndl, ndv, ndh, shadow, ao, g1.w, sun_light.rgb.xyz);

	// Emission is ADDED after shading, never lit: a glowing surface is its own light source.
	// The whole block is skipped for any material whose table strength is zero, which is
	// every material but the emissive ones -- so dull terrain pays one array read.
	//
	// The mask is the albedo array's alpha (see MaterialAtlas::pack_layer). It is sampled
	// here rather than carried through the G-buffer because no G-buffer channel is free,
	// and widening it would make every pixel pay for a feature one material uses.
	float glow = mat_glow(mat);
	if (glow > 0.0) {
		// This is a compute shader: there is no dFdx. Reconstruct the neighbouring pixels'
		// world positions from the depth buffer to get the triplanar gradients, the same
		// way the raymarcher derives them from ray differentials.
		vec3 wpos_x = wpos, wpos_y = wpos;
		ivec2 mx = min(px + ivec2(1, 0), size - 1);
		ivec2 my = min(px + ivec2(0, 1), size - 1);
		float dx_depth = texelFetch(gb_depth, mx, 0).r;
		float dy_depth = texelFetch(gb_depth, my, 0).r;
		if (dx_depth > 0.0) {
			vec2 nd = ((vec2(mx) + 0.5) / vec2(size)) * 2.0 - 1.0;
			vec4 hx = pc.inv_view_proj * vec4(nd, dx_depth, 1.0);
			wpos_x = hx.xyz / (abs(hx.w) < 1e-9 ? 1e-9 : hx.w);
		}
		if (dy_depth > 0.0) {
			vec2 nd = ((vec2(my) + 0.5) / vec2(size)) * 2.0 - 1.0;
			vec4 hy = pc.inv_view_proj * vec4(nd, dy_depth, 1.0);
			wpos_y = hy.xyz / (abs(hy.w) < 1e-9 ? 1e-9 : hy.w);
		}
		float mask = material_surface(mat, wpos, n, wpos_x - wpos, wpos_y - wpos).a;
		lit += mat_glow_rgb(mat) * glow * mask;
	}
	imageStore(out_lit, px, vec4(lit, 1.0));
}
