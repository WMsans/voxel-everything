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

// Independent oracle path for filter tests. It deliberately evaluates each point against
// the complete chronological list, retaining only ops whose AABB plus the shared lattice
// pad reaches that point. This is not the production brick compaction path: the test compares
// its result to the unfiltered CPU field, so a false negative cannot hide behind a matching
// CPU filter.
void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= pc.cfg.x) return;
	vec3 p = points.p[i].xyz;
	float sdf;
	uint mat;
	base_field(p, sdf, mat);
	for (uint op = 0u; op < pc.cfg.y; op++) {
		if (op_touches_aabb(op, p, p, LATTICE_FILTER_PAD))
			apply_field_op(op, p, sdf, mat);
	}
	results.v[i] = vec4(sdf, float(mat), 0.0, 0.0);
}
