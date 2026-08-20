#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 8
#define FIELD_OVERRIDE_SDF_BINDING 0
#define FIELD_OVERRIDE_MAT_BINDING 1
#define FIELD_OVERRIDE_TABLE_BINDING 6
#define FIELD_OVERRIDE_REGION_BINDING 7
#include "common.glslh"
layout(push_constant, std430) uniform Push {
	ivec4 params;
} pc;
#define FIELD_OVERRIDE_TABLE(base) (pc.params.w)
#include "field.glslh"

layout(local_size_x = 256) in;

// A re-consolidation bakes previous override plus the ops since it, not G plus ops. The
// CPU publishes the new table only after every brick has been read back intact. Bindings 0
// and 1 are the existing override pool declared by field.glslh; bindings 3 and 4 are private
// staging buffers and must never alias a published slot.
// binding 8 is the consolidation op pool, declared by field.glslh
layout(set = 0, binding = 3, std430) buffer BakedSdf { uint w[]; } baked_sdf;
layout(set = 0, binding = 4, std430) buffer BakedMat { uint w[]; } baked_mat;
layout(set = 0, binding = 5, std430) readonly buffer ConsolidateJobs { ivec4 v[]; } consolidate_jobs;
void store_sdf_byte(int byte_index, uint value) {
	int wi = byte_index >> 2;
	uint shift = (uint(byte_index) & 3u) * 8u;
	uint mask = 0xFFu << shift;
	uint old = baked_sdf.w[wi];
	for (;;) {
		uint next = (old & ~mask) | ((value & 0xFFu) << shift);
		uint seen = atomicCompSwap(baked_sdf.w[wi], old, next);
		if (seen == old) return;
		old = seen;
	}
}

void store_mat_byte(int byte_index, uint value) {
	int wi = byte_index >> 2;
	uint shift = (uint(byte_index) & 3u) * 8u;
	uint mask = 0xFFu << shift;
	uint old = baked_mat.w[wi];
	for (;;) {
		uint next = (old & ~mask) | ((value & 0xFFu) << shift);
		uint seen = atomicCompSwap(baked_mat.w[wi], old, next);
		if (seen == old) return;
		old = seen;
	}
}

ivec3 brick_coord(uint index) {
	ivec4 j = consolidate_jobs.v[int(index) * 2 + 0];
	return j.xyz;
}

int output_slot(uint index) {
	return consolidate_jobs.v[int(index) * 2 + 0].w;
}

void main() {
	uint tid = gl_LocalInvocationID.x;
	uint brick_index = gl_WorkGroupID.x;
	if (brick_index >= uint(pc.params.x)) return;
	ivec3 brick = brick_coord(brick_index);
	int slot = output_slot(brick_index);
	if (slot < 0) return;
	vec3 bo = vec3(brick) * BRICK_SIZE;
	int sdf_base = slot * 4916;
	int mat_base = slot * 4096;

	for (uint i = tid; i < uint(BRICK_SDF_COUNT); i += 256u) {
		ivec3 v = ivec3(int(i) % BRICK_SDF_STRIDE,
				(int(i) / BRICK_SDF_STRIDE) % BRICK_SDF_STRIDE,
				int(i) / (BRICK_SDF_STRIDE * BRICK_SDF_STRIDE));
		float sdf;
		uint mat;
		eval_field(bo + vec3(v) * VOXEL_SIZE, 0u, uint(pc.params.z), sdf, mat);
		store_sdf_byte(sdf_base + int(i), encode_sdf_byte(sdf));
	}
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		ivec3 v = ivec3(int(i) % BRICK_VOXELS,
				(int(i) / BRICK_VOXELS) % BRICK_VOXELS,
				int(i) / (BRICK_VOXELS * BRICK_VOXELS));
		float sdf;
		uint mat;
		eval_field(bo + vec3(v) * VOXEL_SIZE, 0u, uint(pc.params.z), sdf, mat);
		store_mat_byte(mat_base + int(i), mat);
	}
}
