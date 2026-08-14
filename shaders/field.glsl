// GPU mirror of ve::AnalyticGenerator (extension/src/generator/generator.cpp) and
// ve::apply_op (extension/src/generator/edit_ops.cpp). tests/test_field_diff.gd diffs the
// two and fails when they drift (spec section 8).
//
// The includer MUST define FIELD_OP_POOL_BINDING to the set-0 binding of its op pool before
// pulling this file in.
#include "common.glsl"

const uint OP_SPHERE_SUBTRACT = 0u;
const uint OP_SPHERE_ADD = 1u;
const uint OP_SPHERE_PAINT = 2u;
const uint MAX_REGION_OPS = 256u;
// Mirror of ve::kSurfaceY (extension/src/generator/generator.h): 51.2 = -origin_bricks.y *
// kBrickSize for the default world origin (0, -64, 0). Must stay in lockstep with the CPU
// constant; tests/test_field_diff.gd guards the agreement.
const float SURFACE_Y = 51.2;

// ve::EditOp is 32 bytes; storing two uvec4 per op and unpacking by hand keeps the mirror
// exact, because a std430 struct with a vec3 member would silently pad to 48.
//   uvec4 a = { type, material, pos.x, pos.y }
//   uvec4 b = { pos.z, radius, pad, pad }
layout(set = 0, binding = FIELD_OP_POOL_BINDING, std430) readonly buffer FieldOpPool {
	uvec4 v[];
} field_op_pool;

float hills(float x, float z) {
	return 6.0 * sin(x * 0.11) * cos(z * 0.13)
	     + 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043)
	     + 1.0 * sin(x * 0.23 + z * 0.19);
}

void base_field(vec3 p, out float sdf, out uint mat) {
	float h = hills(p.x, p.z);
	sdf = p.y - SURFACE_Y - h;
	const float cx = 30.0, cz = 30.0;
	float cy = SURFACE_Y + hills(cx, cz) - 2.0;
	float sphere = length(p - vec3(cx, cy, cz)) - 5.0;
	sdf = max(sdf, -sphere); // CSG subtract: the one carved cave
	mat = 0u;
	if (sdf <= 0.0) mat = h > 4.0 ? 2u : (h > 1.0 ? 1u : 3u);
}

void apply_field_op(uint index, vec3 p, inout float sdf, inout uint mat) {
	uvec4 a = field_op_pool.v[index * 2u + 0u];
	uvec4 b = field_op_pool.v[index * 2u + 1u];
	uint type = a.x;
	uint material = a.y;
	vec3 c = vec3(uintBitsToFloat(a.z), uintBitsToFloat(a.w), uintBitsToFloat(b.x));
	float radius = uintBitsToFloat(b.y);
	float sp = length(p - c) - radius;
	if (type == OP_SPHERE_SUBTRACT) {
		if (-sp > sdf) { sdf = -sp; if (sdf > 0.0) mat = 0u; }
	} else if (type == OP_SPHERE_ADD) {
		if (sp < sdf) { sdf = sp; if (sdf <= 0.0) mat = material; }
	} else if (type == OP_SPHERE_PAINT) {
		if (sp <= 0.0 && sdf <= 0.0) mat = material;
	}
}

// op_base is the index of the region's first op in the pool (region_slot * MAX_REGION_OPS).
void eval_field(vec3 p, uint op_base, uint op_count, out float sdf, out uint mat) {
	base_field(p, sdf, mat);
	for (uint i = 0u; i < op_count; i++) apply_field_op(op_base + i, p, sdf, mat);
}
