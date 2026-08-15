#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 1
#include "common.glslh"
#include "field.glslh"
#include "mesh_common.glslh"

// One thread per lattice sample. 130 is not a multiple of 4, so the last group in each axis
// runs partly out of bounds and returns.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) writeonly uniform image3D lattice;
// binding 1 is the field op pool, declared by field.glslh

layout(push_constant, std430) uniform Push {
	ivec4 chunk;  // xyz = chunk coordinates, w = job index in this batch
	ivec4 params; // x = op count, y = max verts per job, z = max tris per job, w = unused
} pc;

void main() {
	ivec3 l = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(l, ivec3(CHUNK_LATTICE)))) return;
	float sdf;
	uint mat; // the mesher has no use for materials; collision carries none
	eval_field(lattice_world_pos(pc.chunk.xyz, l), uint(pc.chunk.w) * MAX_REGION_OPS,
			uint(pc.params.x), sdf, mat);
	imageStore(lattice, l, vec4(quantise_sdf(sdf)));
}
