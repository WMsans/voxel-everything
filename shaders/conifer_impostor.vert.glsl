#[vertex]
#version 460

#include "common.glslh"
#include "conifer_pass.glslh"

// Pulled geometry, as leaf.vert.glsl: gl_VertexIndex / 6 is the tree, % 6 the corner.
layout(set = 0, binding = 0, std430) readonly buffer ImpostorList { ConiferRecord v[]; } list;
layout(set = 0, binding = 1, std140) uniform Params { CONIFER_PASS_FIELDS } conifer;
layout(push_constant, std430) uniform Push { mat4 view_proj; vec4 cam; } pc;

vec4 conifer_ground(vec2 xz) { return vec4(0.0); }
#include "conifer.glslh"

layout(location = 0) out vec3 v_world;
// FLAT: a.w is the hash's bit pattern; interpolating it would change the hash (leaf.vert's rule).
layout(location = 1) flat out vec4 v_a;
layout(location = 2) flat out vec4 v_b;

void main() {
	uint tree = uint(gl_VertexIndex) / 6u;
	uint corner = uint(gl_VertexIndex) % 6u;
	ConiferRecord r = list.v[tree];
	Conifer c = conifer_unpack(r.a, r.b);
	v_a = r.a;
	v_b = r.b;

	float half_h = 0.5 * (c.height - c.crown_base);
	vec3 mid = c.foot + vec3(0.0, c.crown_base + half_h, 0.0);
	float rad = c.R * CONIFER_TOOTH_MAX;
	vec3 to_cam = normalize(pc.cam.xyz - mid);
	// Right is horizontal, up is the cylinder axis projected off the view direction. The
	// cross collapses straight above or below a tree, so fall back to world X (leaf.vert).
	vec3 axis = cross(vec3(0.0, 1.0, 0.0), to_cam);
	vec3 right = normalize(dot(axis, axis) > 1e-6 ? axis : vec3(1.0, 0.0, 0.0));
	vec3 up = cross(to_cam, right);
	// The bounding cylinder's extent along `up`: its axis contributes half_h * |up.y|, its
	// radius the rest. 10% margin covers perspective at the >= 240 m these draw from.
	float ext_up = 1.1 * (half_h * abs(up.y) + rad * sqrt(max(0.0, 1.0 - up.y * up.y)));
	float ext_right = 1.1 * rad;

	const vec2 kCorners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1),
	                                 vec2(-1, 1), vec2(1, -1), vec2(1, 1));
	vec2 q = kCorners[corner];
	vec3 world = mid + right * (q.x * ext_right) + up * (q.y * ext_up);
	v_world = world;
	gl_Position = pc.view_proj * vec4(world, 1.0);
}
