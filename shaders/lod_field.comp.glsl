#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 2
#define FIELD_VOLUME_SDF_BINDING 3
#define FIELD_VOLUME_MAT_BINDING 4
#define FIELD_OVERRIDE_SDF_BINDING 5
#define FIELD_OVERRIDE_MAT_BINDING 6
#define FIELD_OVERRIDE_TABLE_BINDING 7
#define FIELD_OVERRIDE_REGION_BINDING 8
#include "common.glslh"
#include "lod_common.glslh"
#define FIELD_OVERRIDE_TABLE(base) (lpc.override_data.x)
#include "field.glslh"

// One thread per HALF-CELL sample. Spec section 4: the target lattice is built from samples
// at half the level's cell size and tent-reduced, which is the mip cascade computed inside
// one build job. 69 is not a multiple of 4, so the last group of each axis runs partly out
// of bounds and returns.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) writeonly uniform image3D fine_sdf;
layout(set = 0, binding = 1, r16ui) writeonly uniform uimage3D fine_mat;

void main() {
	ivec3 j = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(j, ivec3(LOD_FINE_LATTICE)))) return;
	float sdf;
	uint mat;
	eval_field(lod_fine_world_pos(j), uint(lpc.job.w) * MAX_REGION_OPS, uint(lpc.params.x),
			sdf, mat);
	// Mirror ve::lod_encode_sdf: preserve L0 (0.4 m cells), but store +/-2 cells
	// at coarser levels. Crossing endpoints plus their half-cell tent taps need
	// at most sqrt(2.75) cells for a unit plane normal. Clipping to a fixed metre
	// range before the tent shifts coarse surfaces with their sub-cell phase.
	// The later passes need averages, signs and crossing ratios, not metres, so
	// they can keep decode_sdf/quantise_sdf in this common scaled distance space.
	float scale = lpc.grid.w <= 0.4 ? 1.0 : SDF_RANGE / (2.0 * lpc.grid.w);
	imageStore(fine_sdf, j, vec4(quantise_sdf(sdf * scale)));
	imageStore(fine_mat, j, uvec4(mat, 0u, 0u, 0u));
}
