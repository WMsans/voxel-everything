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

// Mirror of ve::extract_island_volume's `masked` lambda: the island IS the solid field
// intersected with the union of its 0.8 m cells, which is max(field, min over boxes). A
// component with no boxes extracts to nothing, which is the correct answer and not a
// special case.
float masked_field(vec3 p, out uint mat) {
	float sdf;
	eval_field(p, 0u, uint(pc.params.y), sdf, mat);
	float bu = 1e30;
	for (int i = 0; i < pc.params.z; i++)
		bu = min(bu, op_box_sdf(boxes.v[i * 2 + 0].xyz, boxes.v[i * 2 + 1].xyz, p));
	return max(sdf, bu);
}

void main() {
	ivec3 l = ivec3(gl_GlobalInvocationID);
	int dim = pc.params.x;
	if (any(greaterThanEqual(l, ivec3(dim)))) return;
	float voxel = pc.origin_voxel.w;
	vec3 p = pc.origin_voxel.xyz + vec3(l) * voxel;

	uint mat;
	float sdf = masked_field(p, mat);
	if (sdf > 0.0) mat = 0u;
	if (sdf <= 0.0) atomicAdd(counts.solid, 1u);

	// The same projection ve::spread_materials gives a brick (brick_gen.comp.glsl phase 2),
	// applied to a lattice: an AIR sample within project_range of the surface is pushed onto
	// it and given the material it lands on. Both this shader's consumers read an island's
	// material with NEAREST-sample rounding (island_material_at in raymarch.comp.glsl,
	// sample_field_volume in field.glslh), so a hit point resolves to a sample on the air
	// side of the surface about half the time; without this those samples read material 0
	// and shade as material_albedo(0) = error magenta.
	float project_range = 2.0 * voxel;
	if (mat == 0u && sdf <= project_range) {
		// Central difference of the MASKED field, so a sample outside a box face projects
		// back through that face rather than along the terrain's own gradient. The 2 * voxel
		// divisor cancels in the normalise; it is kept so the CPU reference can do the
		// identical arithmetic.
		uint ignored;
		vec3 ex = vec3(voxel, 0.0, 0.0);
		vec3 ey = vec3(0.0, voxel, 0.0);
		vec3 ez = vec3(0.0, 0.0, voxel);
		vec3 g = vec3(
			masked_field(p + ex, ignored) - masked_field(p - ex, ignored),
			masked_field(p + ey, ignored) - masked_field(p - ey, ignored),
			masked_field(p + ez, ignored) - masked_field(p - ez, ignored)) / (2.0 * voxel);
		float len = length(g);
		// The stored SDF is uint8-quantised, so a single step can fall short of the surface;
		// lengthen it and retry, exactly as spread_materials does.
		for (float over = 0.5; over <= 2.5 && mat == 0u && len > 0.0; over += 1.0) {
			float t = sdf + over * voxel;
			float ignored_sdf;
			eval_field(p - g / len * t, 0u, uint(pc.params.y), ignored_sdf, mat);
		}
	}

	out_vol.v[l.x + l.y * dim + l.z * dim * dim] =
			(min(mat, 255u) << 8) | encode_sdf_byte(sdf);
}
