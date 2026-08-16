#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 1
#define FIELD_VOLUME_SDF_BINDING 2
#define FIELD_VOLUME_MAT_BINDING 3
#include "common.glslh"
#include "field.glslh"

// One thread per lattice sample. 64 is a multiple of 4, so no group runs out of bounds --
// the guard is kept anyway because the dim comes from a push constant.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

// One uint per voxel: (material << 8) | encoded sdf. Packing here rather than writing two
// byte arrays means one buffer, one readback, and no sub-word atomics; the CPU splits the
// two halves apart as it copies them into ve::VolumeData.
layout(set = 0, binding = 0, std430) writeonly buffer Out { uint v[]; } out_vol;
// binding 1 is the field op pool and 2/3 the volume pool, declared by field.glslh
// Two vec4 per box: the world AABB's min and max corners.
layout(set = 0, binding = 4, std430) readonly buffer Boxes { vec4 v[]; } boxes;
layout(set = 0, binding = 5, std430) buffer Counts { uint solid; uint pad0, pad1, pad2; } counts;

layout(push_constant, std430) uniform Push {
	vec4 origin_voxel; // xyz = lattice world origin, w = voxel pitch
	ivec4 params;      // x = dim, y = op count, z = box count, w = unused
} pc;

void main() {
	ivec3 l = ivec3(gl_GlobalInvocationID);
	int dim = pc.params.x;
	if (any(greaterThanEqual(l, ivec3(dim)))) return;
	vec3 p = pc.origin_voxel.xyz + vec3(l) * pc.origin_voxel.w;

	float sdf;
	uint mat;
	eval_field(p, 0u, uint(pc.params.y), sdf, mat);

	// Mirror of ve::extract_island_volume: the island IS the solid field intersected with
	// the union of its 0.8 m cells, which is max(field, min over boxes). A component with no
	// boxes extracts to nothing, which is the correct answer and not a special case.
	float bu = 1e30;
	for (int i = 0; i < pc.params.z; i++)
		bu = min(bu, op_box_sdf(boxes.v[i * 2 + 0].xyz, boxes.v[i * 2 + 1].xyz, p));
	sdf = max(sdf, bu);
	if (sdf > 0.0) mat = 0u;
	if (sdf <= 0.0) atomicAdd(counts.solid, 1u);

	out_vol.v[l.x + l.y * dim + l.z * dim * dim] =
			(min(mat, 255u) << 8) | encode_sdf_byte(sdf);
}
