#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 0
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
	eval_field(points.p[i].xyz, 0u, pc.cfg.y, sdf, mat);
	results.v[i] = vec4(sdf, float(mat), 0.0, 0.0);
}
