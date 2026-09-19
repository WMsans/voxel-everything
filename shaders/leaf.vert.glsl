#[vertex]
#version 460

#include "common.glslh"
#include "leaf.glslh"

// No vertex buffer and no vertex attributes: geometry is PULLED, exactly as grass draws.
// gl_VertexIndex / 6 is the clump, % 6 the corner. This also routes around Godot exposing
// neither gl_DrawID nor a non-zero firstInstance.
layout(set = 0, binding = 0, std430) readonly buffer Instances { LeafClump c[]; } instances;
layout(set = 0, binding = 1, std140) uniform Params { LEAF_PARAMS_FIELDS } leaf;
layout(push_constant, std430) uniform Push { mat4 view_proj; vec4 cam; } pc;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out float v_sun;
layout(location = 4) out float v_depth_t;
layout(location = 5) out float v_hash;

void main() {
	uint clump = uint(gl_VertexIndex) / 6u;
	uint corner = uint(gl_VertexIndex) % 6u;
	LeafClump c = instances.c[clump];

	uint packed = floatBitsToUint(c.b.x);
	vec3 n = leaf_unpack_normal(packed);
	v_sun = leaf_unpack_sun(packed);
	vec2 pair = leaf_unpack_pair(c.b.z);
	v_depth_t = pair.x;
	float ratio = pair.y;
	v_hash = c.b.y;

	// Two triangles: 0,1,2 and 2,1,3 over the unit square, mapped to -1..1.
	const vec2 kCorners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1),
	                                 vec2(-1, 1), vec2(1, -1), vec2(1, 1));
	vec2 q = kCorners[corner];
	v_uv = q;

	// Camera-facing with a fixed per-clump ROLL from the hash, and the up axis locked to
	// world Y. A full spherical billboard makes a canopy swim when the camera strafes; the
	// roll is what keeps neighbouring cards from all aligning into a visible grid.
	vec3 to_cam = normalize(pc.cam.xyz - c.a.xyz);
	vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), to_cam));
	vec3 up = cross(to_cam, right);
	float roll = c.b.w;
	vec3 rr = right * cos(roll) + up * sin(roll);
	vec3 uu = up * cos(roll) - right * sin(roll);

	vec3 world = c.a.xyz + (rr * q.x + uu * q.y) * c.a.w;
	v_world = world;

	// Perturb the transferred sphere normal across the card. The scatter packs ONE normal per
	// clump; spreading it by the card's own extent, scaled by clump_radius / crown_radius,
	// approximates a true per-fragment transfer from the crown sphere at a thirty-second of
	// the storage. At these card sizes the two are indistinguishable.
	// Backfaces must not shade black: the cards draw with cull disabled, so the perturbed
	// normal is turned to the viewer -- the same treatment grass turns its blade faces with
	// (grass.vert.glsl's `if (dot(face, to_cam) < 0.0)`), done here where to_cam already is.
	vec3 sn = normalize(n + (rr * q.x + uu * q.y) * ratio);
	if (dot(sn, to_cam) < 0.0) sn = -sn;
	v_normal = sn;

	gl_Position = pc.view_proj * vec4(world, 1.0);
}
