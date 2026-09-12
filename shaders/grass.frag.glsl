#[fragment]
#version 460

#include "common.glslh"
#include "shade.glslh"
#include "grass.glslh"

layout(set = 0, binding = 1, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in float v_height_t;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss

const uint GRASS_MATERIAL = 1u;

// If common.glslh fails to compile here, it is because material_surface() needs the two
// material sampler arrays declared BEFORE the include -- the convention lod.frag.glsl
// follows at its lines 4-6. Declare them ahead of the include at unused binding slots even
// though grass never samples them; a flat blade colour is the whole point.

void main() {
	// Task 8 replaces this flat colour with the root-to-tip gradient. Keeping it flat here
	// means this task's test is about geometry reaching the G-buffer, nothing else.
	vec3 albedo = flat_material_albedo(GRASS_MATERIAL);

	// Backfaces are not culled, so a blade seen from behind must not shade as if it faced
	// away -- that is the classic black-grass bug.
	vec3 n = gl_FrontFacing ? normalize(v_normal) : -normalize(v_normal);

	// Sun visibility is 1: shadowing is the deferred pass's job, exactly as in lod.frag.glsl.
	out_albedo = vec4(albedo, 1.0);
	out_surface = vec4(oct_encode(n), float(GRASS_MATERIAL), pc.style.y);
}
