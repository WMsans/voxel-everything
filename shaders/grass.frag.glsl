#[fragment]
#version 460

#include "common.glslh"
#include "shade.glslh"
#include "grass.glslh"

layout(set = 0, binding = 1, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam; // xyz camera position, w unused
} push;

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in float v_height_t;
layout(location = 3) in float v_clump;
layout(location = 4) in flat uint v_hash;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss

const uint GRASS_MATERIAL = 1u;

// If common.glslh fails to compile here, it is because material_surface() needs the two
// material sampler arrays declared BEFORE the include -- the convention lod.frag.glsl
// follows at its lines 4-6. Declare them ahead of the include at unused binding slots even
// though grass never samples them; a flat blade colour is the whole point.

void main() {
	// Vertex gradient, shaded root to bright warm tip. This is the BotW vertex-colour trick
	// and, now that every blade shares the ground normal, it is ALL of the shape cue the
	// blade has -- the lighting no longer varies across it.
	//
	// The root is a dark green rather than the near-black it used to be: at 0.10/0.22/0.07
	// the bases read as dirt between the blades instead of canopy shadow, which is half of
	// why the field looked bald.
	const vec3 kRoot = vec3(0.16, 0.30, 0.10);
	const vec3 kTip  = vec3(0.66, 0.86, 0.34);
	vec3 albedo = mix(kRoot, kTip, v_height_t * v_height_t);

	// Per-blade and per-clump variation, so the field is patchy rather than a lawn. Kept
	// narrow: the old +-18% swing read as salt-and-pepper noise across the field.
	float tint = grass_unit(grass_hash(v_hash ^ 0x27D4EB2Fu));
	albedo *= mix(0.92, 1.08, tint);
	albedo = mix(albedo, albedo * vec3(1.12, 1.05, 0.72), v_clump * 0.45);

	// Flowers: a small hash fraction gets a warm tip. One branch, near-free.
	if (grass_unit(grass_hash(v_hash ^ 0x165667B1u)) < pc.style.x) {
		albedo = mix(albedo, vec3(0.95, 0.86, 0.28), smoothstep(0.72, 1.0, v_height_t));
	}

	// Dither out at the reach limit with the SAME test lod.frag.glsl uses at the LoD seam,
	// so the density tail fades instead of popping.
	float d = distance(v_wpos, push.cam.xyz);
	float fade = clamp((d - pc.ring_end.z) / max(pc.ring_end.w - pc.ring_end.z, 1e-3), 0.0, 1.0);
	if (bayer4(ivec2(gl_FragCoord.xy)) < fade) discard;

	// The interpolated GROUND normal, never flipped. The old code flipped it on backfaces
	// to rescue per-blade normals that genuinely faced away from the viewer; an inherited
	// ground normal already points out of the terrain on both sides, and flipping it would
	// aim it into the ground -- which is the black-grass bug, not the fix for it.
	vec3 n = normalize(v_normal);

	// Sun visibility is 1: shadowing is the deferred pass's job, exactly as in lod.frag.glsl.
	out_albedo = vec4(albedo, 1.0);
	out_surface = vec4(oct_encode(n), float(GRASS_MATERIAL), pc.style.y);
}
