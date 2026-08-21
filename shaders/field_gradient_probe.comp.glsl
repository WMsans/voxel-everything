#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 0
#define FIELD_VOLUME_SDF_BINDING 3
#define FIELD_VOLUME_MAT_BINDING 4
#include "common.glslh"
#include "field.glslh"

layout(local_size_x = 64) in;

layout(set = 0, binding = 1, std430) readonly buffer Points { vec4 p[]; } points;
layout(set = 0, binding = 2, std430) writeonly buffer Results { vec4 v[]; } results;

layout(push_constant, std430) uniform Push {
	uvec4 cfg; // x = point count, y = op count, zw unused
} pc;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= pc.cfg.x) return;
	float sdf;
	uint mat;
	vec3 grad;
	bool exact;
	eval_field_gradient(points.p[i].xyz, 0u, pc.cfg.y, sdf, mat, grad, exact);
	results.v[i * 2u + 0u] = vec4(sdf, float(mat), grad.x, grad.y);
	results.v[i * 2u + 1u] = vec4(grad.z, exact ? 1.0 : 0.0, 0.0, 0.0);
}
