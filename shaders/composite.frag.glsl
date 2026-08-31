#[fragment]
#version 460
#define MATERIAL_LAYERS 16
layout(set = 0, binding = 2) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 3) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh" // oct_decode: the normal arrives packed, and there is one unpacker
layout(location = 0) in vec2 uv_in;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_surface;
#ifdef SEAM_MARKER
layout(location = 2) out uint marker;
#endif

// This pass RESOLVES THE MATERIAL, once per full-resolution pixel.
//
// near_field_scale buys frame time by marching fewer rays. What that makes cheaper is
// VISIBILITY -- which surface a pixel sees and where it is -- and nothing else. The material
// on that surface is a pure function of the hit position, the normal and the material id,
// all three of which raymarch.comp.glsl exports per marched pixel, so it can be evaluated
// here instead, at the resolution the frame is actually presented at. The marcher used to
// bake its own triplanar fetch at march resolution (with ray differentials sized to a
// low-resolution pixel, so the mip was over-selected on top) and this pass could only
// magnify the result: texture detail fell roughly in proportion to the scale, which is
// exactly the blurring the option was not supposed to cost. See tests/test_near_field_scale.gd.
//
// These are the same two lines lod.frag.glsl ends with, on purpose: near and far field now
// resolve a material through one pair of calls, so the seam cannot drift.
//
// src_overlay/src_surface.w carry what genuinely belongs to the RAY rather than to the
// surface -- the sky, a glossy reflection, the edit visualiser, the cost view -- as a single
// colour and weight the marcher folded together. Mixing it over the resolved material here
// reproduces the marcher's own compositing order exactly.
layout(set = 0, binding = 0) uniform sampler2D src_overlay; // rgb overlay, a sun visibility
layout(set = 0, binding = 1) uniform sampler2D src_hitpos;  // xyz world hit, w hit flag
layout(set = 0, binding = 4) uniform sampler2D src_surface; // xy oct normal, z material, w overlay weight
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;        // xyz = camera position, w = fade start
	vec4 fade;       // x = fade end, yzw = camera forward
	vec4 right_tanx; // xyz = camera right, w = tan(fov_x / 2)
	vec4 up_tany;    // xyz = camera up,    w = tan(fov_y / 2)
} pc;

void main() {
	vec4 hp = texture(src_hitpos, uv_in);
	vec4 sf = texture(src_surface, uv_in);
	// The overlay keeps the LINEAR sampler: the sky gradient and the raymarched sun term are
	// smooth, and a soft upsample is what they want. Geometry stays on the nearest sampler --
	// interpolating across a silhouette would invent a surface that was never marched. That
	// puts the overlay's WEIGHT (sf.w) on the nearest sampler with the geometry it shares a
	// texture with, so the edge of an edit tint or a glossy blend steps at march resolution
	// while its colour still resolves smoothly. Both are soft, low-frequency effects; neither
	// justifies a fourth target to carry a filterable weight.
	vec4 ov = texture(src_overlay, uv_in);
#ifdef SEAM_MARKER
	marker = 1u;
#endif
	if (hp.w < 0.5) {
		// A miss carries the sky in the overlay at full weight, and material 0, which the
		// deferred pass passes through unlit. Gloss is forced to 0 rather than passed
		// through: the overlay weight lives in that channel on this side of the pass.
		out_albedo = ov;
		out_surface = vec4(sf.xy, sf.z, 0.0);
#ifdef SEAM_MARKER
		marker = 0u;
#endif
		gl_FragDepth = 0.0;
		return;
	}

	// The full-resolution primary ray. uv_in is a linear function of the pixel (the vertex
	// stage writes the fullscreen triangle's own clip position into it), so its screen
	// derivatives are EXACTLY one texel of this target -- which is where the material's mip
	// footprint comes from below, at full resolution rather than the marcher's.
	vec2 texel = vec2(dFdx(uv_in.x), dFdy(uv_in.y));
	vec2 ndc = vec2(uv_in.x * 2.0 - 1.0, 1.0 - uv_in.y * 2.0);
	vec3 rd = normalize(pc.fade.yzw
			+ pc.right_tanx.xyz * ndc.x * pc.right_tanx.w
			+ pc.up_tany.xyz * ndc.y * pc.up_tany.w);

	vec3 n = oct_decode(sf.xy);
	uint mat = uint(sf.z + 0.5);
	vec3 ro = pc.cam.xyz;
	vec3 p = hp.xyz;
	// Nearest-sampling the hit position would hold it constant across every full-resolution
	// pixel covering one marched pixel, and a constant position is a constant triplanar
	// coordinate: the texture would come out in blocks. Intersect THIS pixel's ray with the
	// tangent plane of the marched sample instead. On a locally flat surface that is the
	// position the ray would have hit had it been marched, so the material moves smoothly
	// across the block, and because the plane comes from the nearest sample alone it never
	// blends two surfaces across a silhouette the way a bilinear read would.
	float t = distance(p, ro);
	float denom = dot(rd, n);
	if (denom < -0.05) { // a plane any closer to edge-on covers too few pixels to be worth it
		float refined_t = dot(p - ro, n) / denom;
		vec3 refined = ro + rd * refined_t;
		// The reprojection only ever fills the gap BETWEEN marched samples, so it must never
		// travel further than one of them; that bound is the marcher's own pixel footprint
		// here. Without it a grazing plane throws the intersection down the ray.
		vec2 low_texel = 1.0 / vec2(textureSize(src_hitpos, 0));
		float span = 2.0 * t * max(pc.right_tanx.w * low_texel.x, pc.up_tany.w * low_texel.y);
		if (refined_t > 0.0 && distance(refined, p) <= span) {
			p = refined;
			t = refined_t;
		}
	}

	// The pixel's world footprint at the hit: the ray direction's screen derivative scaled by
	// distance, exactly as raymarch.comp.glsl derives it -- but from THIS target's texel, so
	// the mip is the one a full-resolution frame would have picked.
	vec3 ddx = pc.right_tanx.xyz * (2.0 * pc.right_tanx.w * texel.x) * t;
	vec3 ddy = pc.up_tany.xyz * (2.0 * pc.up_tany.w * texel.y) * t;
	vec4 surf = material_surface(mat, p, n, ddx, ddy);
	vec2 props = material_props(mat, p, n, ddx, ddy);
	// AO has no channel of its own and the cel stack only multiplies the AMBIENT term by it,
	// so it is folded into the albedo here -- the same fold, with the same 0.65, that
	// lod.frag.glsl applies to the far field. It darkens the MATERIAL only: the overlay is a
	// debug tint or a reflection, not a lit surface, and the marcher's own compositing put it
	// on top.
	out_albedo = vec4(mix(surf.rgb * mix(1.0, props.y, 0.65), ov.rgb, sf.w), ov.a);
	out_surface = vec4(sf.xy, sf.z, 1.0 - props.x);

	float d = distance(p, pc.cam.xyz);
	float t_fade = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	if (bayer4(ivec2(gl_FragCoord.xy)) < t_fade) {
#ifdef SEAM_MARKER
		marker = 0u;
#endif
		gl_FragDepth = 0.0;
		return;
	}
	// The reprojected position, not the marched one: the depth this pass hands the far field,
	// the outlines and the screen-space passes is now full resolution wherever the surface is
	// locally flat, which is everywhere except the silhouettes the march itself bounds.
	vec4 clip = pc.view_proj * vec4(p, 1.0);
	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
