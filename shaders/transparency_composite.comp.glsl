#[compute]
#version 460
#include "generated/gbuffer.glslh"
#include "generated/blocks.glslh"

#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 9
#define VE_MATERIAL_ARRAYS
layout(set = 0, binding = 6) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 7) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"
#include "sun_light.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 8
#include "beauty_camera.glslh"

// Shades transparent fronts over what deferred lit behind them
// (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §6):
//   F * sky + (1 - F) * (T * behind + (1 - T) * body)
// where behind is the lit pixel already here, body the material cel-shaded at the front,
// T = max(transmit ^ thickness, floor) and F Schlick's Fresnel for the material's ior.
// Pixels with no front and no thickness are left exactly as deferred wrote them.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba16f) uniform image2D lit;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D front_tex; // xy oct normal, z distance, w material
layout(set = 0, binding = 3) uniform sampler2D thick_tex; // R, G of ve::shell_thickness
layout(set = 0, binding = 4) uniform sampler2DArray sun_map;
#define SUN_CASCADES 3
layout(set = 0, binding = 5, std140) uniform SunBlock { SUN_CASCADE_BLOCK_FIELDS } sun;
#include "sun_map.glslh"

layout(push_constant, std430) uniform Push { TRANSPARENCY_COMPOSITE_PUSH_FIELDS } pc;

// Mirror of ve::shell_thickness.
float shell_thickness(vec2 rg, float z_opaque, float z_front) {
	float z_exit = z_opaque > 0.0 ? z_opaque : z_front + pc.params.y;
	return max(rg.x + rg.y * z_exit, 0.0);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(lit);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);

	vec4 front = texelFetch(front_tex, px, 0);
	vec2 rg = texelFetch(thick_tex, px, 0).rg;
	bool has_front = front.w > 0.5;
	uint inside = pc.flags.y;
	if (!has_front && inside == 0u) return;

	float depth = texelFetch(gb_depth, px, 0).r;
	float z_opaque = depth > 0.0
			? distance(beauty_world_from_depth(uv, depth), bcam.cam.xyz) : 0.0;
	float thickness = shell_thickness(rg, z_opaque, has_front ? front.z : 0.0);
	if (thickness <= 0.0 && !has_front) return;

	uint mat = has_front ? uint(front.w + 0.5) : inside;
	vec3 T = max(pow(mat_transmit(mat), vec3(thickness)), vec3(pc.params.x));
	vec3 behind = imageLoad(lit, px).rgb;

	if (!has_front) {
		// The camera is inside the medium and this pixel shows no front face: tint only.
		// There is no surface to shade, so the body is the material's flat colour under the
		// ambient term.
		vec3 body = flat_material_albedo(mat) * pc.sky.rgb;
		imageStore(lit, px, vec4(T * behind + (1.0 - T) * body, 1.0));
		return;
	}

	vec3 rd = normalize(beauty_world_from_depth(uv, 1.0) - bcam.cam.xyz);
	vec3 p = bcam.cam.xyz + rd * front.z;
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
	// Every front reads the sun map, near or far (spec §6's known ceiling): the map is
	// rasterized from the LoD mesh alone, so a near front is not in it.
	// ponytail: no fade-band gate here, so a near front can pick up a far field's shadow.
	// Gate on the same bayer/fade test deferred.comp.glsl's far_field_owns() uses if a lit
	// front is ever seen to go dark under open sky.
	float shadow = (pc.flags.x & BEAUTY_SUN_MAP) != 0u ? sun_map_visibility(p, ndl, front.z) : 1.0;
	// ndv = 1 is cel_shade's "no rim" (see deferred.comp.glsl).
	vec3 body = cel_shade(surf.rgb * mix(1.0, props.y, 0.65), pc.sky.rgb, ndl, 1.0, ndh, shadow,
			1.0, 1.0 - props.x, sun_light.rgb.xyz);
	float ior = mat_ior(mat);
	float f0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
	float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
	vec3 through = T * behind + (1.0 - T) * body;
	imageStore(lit, px, vec4(mix(through, sky_color(reflect(rd, n)), fresnel), 1.0));
}
