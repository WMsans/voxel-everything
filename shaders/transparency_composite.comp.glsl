#[compute]
#version 460
#include "generated/gbuffer.glslh"
#include "generated/blocks.glslh"

#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 11
#define VE_MATERIAL_ARRAYS
layout(set = 0, binding = 8) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 9) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"
#include "sun_light.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 10
#include "beauty_camera.glslh"

// Shades transparent fronts over what deferred lit behind them
// (docs/superpowers/specs/2026-09-29-transparent-materials-design.md §7):
//   F * sky + (1 - F) * (T * behind + (1 - T) * body)
// where behind is the lit pixel already here, body the material cel-shaded at the front, T
// the transmittance the walk or the shell measured, and F Schlick's Fresnel for the
// material's ior. Pixels with no front are left exactly as deferred wrote them.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba16f) uniform image2D lit;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D near_front; // march resolution
layout(set = 0, binding = 3) uniform sampler2D near_trans;
layout(set = 0, binding = 4) uniform sampler2D far_front;  // full resolution
layout(set = 0, binding = 5) uniform sampler2D far_trans;
layout(set = 0, binding = 6) uniform sampler2DArray sun_map;
#define SUN_CASCADES 3
layout(set = 0, binding = 7, std140) uniform SunBlock { SUN_CASCADE_BLOCK_FIELDS } sun;
#include "sun_map.glslh"

layout(push_constant, std430) uniform Push { TRANSPARENCY_COMPOSITE_PUSH_FIELDS } pc;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(lit);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);

	// The nearer front that owns this pixel. The near field's is kept on the complement of
	// the dither the far shell was drawn with (transparent.frag.glsl), tested on the FRONT's
	// distance, so the two layers split the fade band exactly as terrain does. The near
	// targets are nearest-sampled, as composite.frag.glsl samples geometry.
	// This silently assumes t_near <= t_far: if the marched front were FARTHER than the shell,
	// the far layer would already have discarded on `ff.z < front.z` and the farther near front
	// would win un-tinted. That needs a front and a shell on one pixel, which the dither
	// splits -- so the rule holds, but it is a dependency, not an accident.
	vec4 front = vec4(0.0);
	vec4 trans = vec4(0.0);
	vec4 nf = texture(near_front, uv);
	if (nf.w > 0.5) {
		float t_fade = clamp((nf.z - pc.fade.x) / max(pc.fade.y - pc.fade.x, 1e-3), 0.0, 1.0);
		if (bayer4(px) >= t_fade) {
			front = nf;
			trans = texture(near_trans, uv);
		}
	}
	if (pc.flags.y != 0u) {
		vec4 ff = texelFetch(far_front, px, 0);
		if (ff.w > 0.5 && (front.w < 0.5 || ff.z < front.z)) {
			front = ff;
			trans = texelFetch(far_trans, px, 0);
		}
	}
	if (front.w < 0.5) return;

	vec3 rd = normalize(beauty_world_from_depth(uv, 1.0) - bcam.cam.xyz);
	vec3 p = bcam.cam.xyz + rd * front.z;
	// Raster grass or leaves in front of the ice own the pixel.
	float depth = texelFetch(gb_depth, px, 0).r;
	if (depth > 0.0 && distance(beauty_world_from_depth(uv, depth), bcam.cam.xyz) < front.z - 0.02)
		return;

	uint mat = uint(front.w + 0.5);
	vec3 n = oct_decode(front.xy);
	// The pixel's world footprint at the front, as composite.frag.glsl derives it.
	vec3 ddx = pc.right_tanx.xyz * (2.0 * pc.right_tanx.w / float(size.x)) * front.z;
	vec3 ddy = pc.up_tany.xyz * (2.0 * pc.up_tany.w / float(size.y)) * front.z;
	vec4 surf = material_surface(mat, p, n, ddx, ddy);
	vec3 shading_n;
	vec2 props = material_props_normal(mat, p, n, ddx, ddy, shading_n);
	vec3 v = -rd;
	vec3 sun_dir = sun_light.dir.xyz;
	float ndl = dot(shading_n, sun_dir);
	float ndh = dot(shading_n, normalize(sun_dir + v));
	// A near front carries its own marched sun term; a far one (a < 0) reads the sun map, as
	// deferred does for every pixel the far field drew.
	float shadow = trans.a >= 0.0 ? trans.a
			: ((pc.flags.x & BEAUTY_SUN_MAP) != 0u ? sun_map_visibility(p, ndl, front.z) : 1.0);
	// ndv = 1 is cel_shade's "no rim" (see deferred.comp.glsl): a rim is a silhouette
	// stylization, and the ice's silhouette is not where its outline is.
	vec3 body = cel_shade(surf.rgb * mix(1.0, props.y, 0.65), pc.sky.rgb, ndl, 1.0, ndh, shadow,
			1.0, 1.0 - props.x, sun_light.rgb.xyz);
	float ior = mat_ior(mat);
	float f0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
	float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
	vec3 behind = imageLoad(lit, px).rgb;
	vec3 through = trans.rgb * behind + (1.0 - trans.rgb) * body;
	imageStore(lit, px, vec4(mix(through, sky_color(reflect(rd, n)), fresnel), 1.0));
}
