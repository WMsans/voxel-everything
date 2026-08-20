#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 1
#define FIELD_VOLUME_SDF_BINDING 2
#define FIELD_VOLUME_MAT_BINDING 3
#define FIELD_OVERRIDE_SDF_BINDING 4
#define FIELD_OVERRIDE_MAT_BINDING 5
#define FIELD_OVERRIDE_TABLE_BINDING 6
#define FIELD_OVERRIDE_REGION_BINDING 7
#include "common.glslh"
#include "mesh_common.glslh"
#define FIELD_OVERRIDE_TABLE(base) (pc.override_data.x)
#include "field.glslh"

// One thread per lattice sample. 130 is not a multiple of 4, so the last group in each axis
// runs partly out of bounds and returns.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) writeonly uniform image3D lattice;
// binding 1 is the field op pool, declared by field.glslh

void main() {
	ivec3 l = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(l, ivec3(chunk_lattice())))) return;
	float sdf;
	uint mat; // the mesher has no use for materials; collision carries none
	eval_field(lattice_world_pos(l), uint(pc.chunk.w) * MAX_REGION_OPS,
			uint(pc.params.x), sdf, mat);
	imageStore(lattice, l, vec4(quantise_sdf(sdf)));
}
