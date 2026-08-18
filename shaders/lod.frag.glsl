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

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam; // xyz = camera position, w = unused
} pc;

void main() {
	// Explicit gradients are not needed here -- a fragment shader has them -- but the SAME
	// function the raymarcher calls is, so the two fields cannot drift (spec section 5).
	vec4 surf = material_surface(v_material, v_wpos, v_normal, dFdx(v_wpos), dFdy(v_wpos));
	frag_color = vec4(shade_terrain(surf, v_normal, v_wpos), 1.0);
}
