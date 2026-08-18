#[fragment]
#version 460

#define MATERIAL_LAYERS 16
layout(set = 0, binding = 3) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 4) uniform sampler2DArray material_surface_tex;
#include "common.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in flat vec3 v_normal;
layout(location = 2) in flat uint v_material;

layout(location = 0) out vec4 frag_color;
#ifdef SEAM_MARKER
layout(location = 1) out uint marker; // debug seam probe: 2 = far field kept the pixel
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
	// function the raymarcher calls is, so the two fields cannot drift (spec section 5).
	vec4 surf = material_surface(v_material, v_wpos, v_normal, dFdx(v_wpos), dFdy(v_wpos));
	frag_color = vec4(shade_terrain(surf, v_normal, v_wpos), 1.0);
}
