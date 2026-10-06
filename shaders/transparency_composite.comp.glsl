#[compute]
#version 460
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
#include "water_flow.glslh"
#include "water_waves.glslh"

// Shades transparent fronts over what deferred lit behind them
// (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §6):
//   F * sky + (1 - F) * (T * behind + (1 - T) * body)
// where behind is the lit pixel already here, body the material cel-shaded at the front,
// T = max(transmit ^ thickness, floor) and F Schlick's Fresnel for the material's ior.
// Pixels with no front and no thickness are left exactly as deferred wrote them. A LIQUID front takes shade_liquid() instead (docs/superpowers/specs/2026-10-06-water-voxels-design.md §5, §6).
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba16f) uniform image2D lit;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
// xy oct normal, z distance, w material. A storage image: a liquid pixel's SHADING normal is
// written back into xy for the G-buffer resolve (water spec §5). Neighbours are read for
// refraction while that happens; only xy is ever written, and only z and w are read there.
layout(set = 0, binding = 2, rgba32f) uniform image2D front_img;
layout(set = 0, binding = 3) uniform sampler2D thick_tex; // R, G of ve::shell_thickness
layout(set = 0, binding = 4) uniform sampler2DArray sun_map;
#define SUN_CASCADES 3
layout(set = 0, binding = 5, std140) uniform SunBlock { SUN_CASCADE_BLOCK_FIELDS } sun;

layout(set = 0, binding = 10) uniform sampler2D lit_copy; // lit as deferred left it, for refraction
#include "sun_map.glslh"

layout(push_constant, std430) uniform Push { TRANSPARENCY_COMPOSITE_PUSH_FIELDS } pc;

// Mirror of ve::shell_thickness.
float shell_thickness(vec2 rg, float z_opaque, float z_front) {
	float z_exit = z_opaque > 0.0 ? z_opaque : z_front + pc.params.y;
	return max(rg.x + rg.y * z_exit, 0.0);
}

// --- Liquids (docs/superpowers/specs/2026-10-06-water-voxels-design.md §5, §6) -----------

const float WATER_PI = 3.14159265;
// How much of the sun the scatter colour picks up, beside the full sky ambient. Light
// scattered inside the medium has no surface to face, so there is no ndl.
const float WATER_SCATTER_SUN = 0.35;
// The glint is HDR so bloom can pick the sparkles up; the cap keeps one texel facing the sun
// exactly from going to infinity.
const float WATER_GLINT_MAX = 64.0;
const float WATER_FOAM_ALBEDO = 0.9;

// No floor: deep water fades to the scatter colour (the spec drops min_transmit for liquids).
vec3 liquid_transmit(uint mat, float path) {
	return pow(max(mat_transmit(mat), vec3(1e-5)), vec3(max(path, 0.0)));
}

vec3 liquid_body(uint mat, float shadow) {
	return flat_material_albedo(mat) * (pc.sky.rgb + sun_light.rgb.xyz * (shadow * WATER_SCATTER_SUN));
}

// GGX with Schlick's Fresnel and the geometry term taken as 1: D * F / (4 ndv), times ndl.
float liquid_glint(vec3 n, vec3 v, vec3 l, float rough, float f0) {
	float ndl = dot(n, l);
	if (ndl <= 0.0) return 0.0;
	vec3 h = normalize(v + l);
	float a2 = rough * rough * rough * rough;
	float ndh = max(dot(n, h), 0.0);
	float d = ndh * ndh * (a2 - 1.0) + 1.0;
	float D = a2 / (WATER_PI * d * d);
	float F = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(v, h), 0.0, 1.0), 5.0);
	return min(D * F * ndl / (4.0 * max(dot(n, v), 0.05)), WATER_GLINT_MAX);
}

// The opaque surface's distance at pixel q, 0 for sky -- main()'s z_opaque at another pixel.
float opaque_distance(ivec2 q, ivec2 size) {
	float depth = texelFetch(gb_depth, q, 0).r;
	vec2 quv = (vec2(q) + 0.5) / vec2(size);
	return depth > 0.0 ? distance(beauty_world_from_depth(quv, depth), bcam.cam.xyz) : 0.0;
}

void shade_liquid(ivec2 px, ivec2 size, vec2 uv, vec4 front, bool has_front, float thickness,
		uint mat) {
	if (!has_front) {
		// The camera is inside the liquid and this pixel shows no surface (§6 item 2): fog
		// over the path to what is behind, which is exactly `thickness` -- the virtual front
		// at 0 to the opaque hit.
		vec3 T = liquid_transmit(mat, thickness);
		vec3 behind = imageLoad(lit, px).rgb;
		imageStore(lit, px, vec4(T * behind + (1.0 - T) * liquid_body(mat, 1.0), 1.0));
		return;
	}
	vec3 rd = normalize(beauty_world_from_depth(uv, 1.0) - bcam.cam.xyz);
	vec3 v = -rd;
	vec3 p = bcam.cam.xyz + rd * front.z;
	vec3 n_geo = oct_decode(front.xy);
	// The stored normal points out of the medium, so a face seen from inside it is an exit.
	bool exit_face = dot(n_geo, v) < 0.0;
	// The pixel's world footprint at the front, as the ice branch derives it.
	vec3 ddx = pc.right_tanx.xyz * (2.0 * pc.right_tanx.w / float(size.x)) * front.z;
	vec3 ddy = pc.up_tany.xyz * (2.0 * pc.up_tany.w / float(size.y)) * front.z;
	float t = pc.params.z;
	vec3 n = water_wave_normal(mat, p, n_geo, ddx, ddy, t, pc.water.y, pc.water.x);
	vec3 n_face = exit_face ? -n_geo : n_geo;
	if (exit_face) n = -n; // shade the side the camera sees
	vec3 sun_dir = sun_light.dir.xyz;
	float shadow = (pc.flags.x & BEAUTY_SUN_MAP) != 0u
			? sun_map_visibility(p, dot(n_face, sun_dir), front.z) : 1.0;
	vec3 body = liquid_body(mat, shadow);

	// Refraction (§5 step 1): offset by what the waves added to the face normal, projected onto
	// the screen and scaled down in shallow water. Image rows run downward, so up is negated.
	vec3 tilt = n - n_face;
	vec2 off = vec2(dot(tilt, pc.right_tanx.xyz), -dot(tilt, pc.up_tany.xyz))
			* (pc.water.z * clamp(thickness, 0.0, 1.0));
	ivec2 rpx = clamp(px + ivec2(round(off * vec2(size))), ivec2(0), size - ivec2(1));
	vec4 rfront = imageLoad(front_img, rpx);
	float rz = opaque_distance(rpx, size);
	// Reject (a) anything nearer than this front -- a foreground object would leak in -- and
	// (b) a pixel with no liquid front -- the shore above the waterline.
	bool keep = rfront.w > 0.5 && mat_liquid(uint(rfront.w + 0.5)) != LIQUID_NONE
			&& (rz <= 0.0 || rz >= front.z);
	// Absorb over the path to the pixel the colour came from, not this one: that is what
	// keeps the shore free of a halo (§5 step 2).
	float path = thickness;
	if (keep && rpx != px) path = shell_thickness(texelFetch(thick_tex, rpx, 0).rg, rz, rfront.z);
	else rpx = px;
	vec3 behind = texelFetch(lit_copy, rpx, 0).rgb;
	float ior = mat_ior(mat);
	float f0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
	vec3 col;
	if (!exit_face) {
		vec3 T = liquid_transmit(mat, path);
		vec3 through = T * behind + (1.0 - T) * body;
		float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
		col = mix(through, sky_color(reflect(rd, n)), fresnel);
		col += sun_light.rgb.xyz * (shadow
				* liquid_glint(n, v, sun_dir, water_glint_roughness(ddx, ddy), f0));
		// Shore foam (§5 step 6): view-ray thickness turned into an estimate of vertical depth.
		float depth = thickness * max(dot(v, n_geo), 0.05);
		float foam = water_foam(p, depth, pc.water.w, t);
		vec3 foam_lit = vec3(WATER_FOAM_ALBEDO) * (pc.sky.rgb
				+ sun_light.rgb.xyz * (shadow * max(dot(n_geo, sun_dir), 0.0)));
		col = mix(col, foam_lit, foam);
	} else {
		// Under the surface, looking out (§6 item 3). refract() wants the normal against the
		// ray, which n now is, and eta = n_liquid / n_air. Inside Snell's window the world
		// above shows through; past it, total internal reflection shows the liquid's own body
		// colour, and SSR replaces that with real reflected geometry where it hits.
		vec3 r = refract(rd, n, ior);
		vec3 seen = body;
		if (dot(r, r) > 0.0) {
			float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(r, -n), 0.0, 1.0), 5.0);
			seen = mix(behind, body, fresnel);
		}
		vec3 T = liquid_transmit(mat, front.z);
		col = T * seen + (1.0 - T) * body;
	}
	imageStore(lit, px, vec4(col, 1.0));
	// The G-buffer resolve copies front.xy into the surface normal, so writing the shading
	// normal here hands the ripples, facing the camera, to SSR and outlines.
	imageStore(front_img, px, vec4(oct_encode(n), front.zw));
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(lit);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);

	vec4 front = imageLoad(front_img, px);
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
	if (mat_liquid(mat) != LIQUID_NONE) {
		shade_liquid(px, size, uv, front, has_front, thickness, mat);
		return;
	}
	// pow(0, 0) is undefined in GLSL and thickness == 0 is reachable (the camera inside a
	// matched front/back pair), and a material table row may zero a transmit channel.
	vec3 T = max(pow(max(mat_transmit(mat), vec3(1e-5)), vec3(thickness)), vec3(pc.params.x));
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
	// ponytail: ao = 1 and ndv = 1, so the body has no rim, no SSAO/SSGI and no emission --
	// deferred earns its rim with silhouette_gate plus 12 taps and this pass does not. Ceiling:
	// a thick block reads flat where a thin one reads glassy. Add the silhouette gate and the
	// deferred term reuse here if a rim ever matters more than the dispatch it costs.
	// ndv = 1 is cel_shade's "no rim" (see deferred.comp.glsl).
	vec3 body = cel_shade(surf.rgb * mix(1.0, props.y, 0.65), pc.sky.rgb, ndl, 1.0, ndh, shadow,
			1.0, 1.0 - props.x, sun_light.rgb.xyz);
	float ior = mat_ior(mat);
	float f0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
	float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
	vec3 through = T * behind + (1.0 - T) * body;
	imageStore(lit, px, vec4(mix(through, sky_color(reflect(rd, n)), fresnel), 1.0));
}
