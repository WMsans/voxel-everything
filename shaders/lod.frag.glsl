#[fragment]
#version 460

#define MATERIAL_LAYERS 16
layout(set = 0, binding = 3) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 4) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in flat vec3 v_normal;
layout(location = 2) in flat uint v_material;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss
#ifdef SEAM_MARKER
layout(location = 2) out uint marker; // debug seam probe: 2 = far field kept the pixel
#endif

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;  // xyz = camera position, w = fade start
	vec4 fade; // x = fade end, yzw unused
} pc;

void main() {
	// Fade before the material sample, so a discarded fragment costs no texture work.
	float d = distance(v_wpos, pc.cam.xyz);
	float t = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	// The exact complement of composite.frag's test: >= where it uses <.
	if (bayer4(ivec2(gl_FragCoord.xy)) >= t) discard;
#ifdef SEAM_MARKER
	marker = 2u;
#endif
	// Explicit gradients are not needed here -- a fragment shader has them -- but the SAME
	// functions the raymarcher calls are, so the two fields cannot drift.
	vec3 ddx = dFdx(v_wpos);
	vec3 ddy = dFdy(v_wpos);
	vec4 surf = material_surface(v_material, v_wpos, v_normal, ddx, ddy);
	// The far field needs the normal map more than the near field does: v_normal is flat
	// across a whole LoD quad, so without it a distant hillside is one unbroken facet. Same
	// call, same arguments as composite.frag.glsl, so the two fields cannot drift.
	vec3 shading_n;
	vec2 props = material_props_normal(v_material, v_wpos, v_normal, ddx, ddy, shading_n);
	// Sun visibility is 1: shadowing the far field is the ortho shadow map's job, evaluated
	// once in the deferred pass where the near field's raymarched term is also applied.
	out_albedo = vec4(surf.rgb * mix(1.0, props.y, 0.65), 1.0);
	out_surface = vec4(oct_encode(shading_n), float(v_material), 1.0 - props.x);
}
