#[compute]
#version 460

#include "common.glslh"
#include "lod_common.glslh"

// The OPAQUE lattice the far field's terrain mesh is contoured from
// (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §3). Every solid sample
// whose material is transparent is rewritten to "just outside" -- half a cell in this level's
// scaled-distance space -- and every other sample is copied. On a chunk with no transparent
// label the copy is byte for byte, so its mesh is exactly the one it always was. Mirror of
// ve::lod_opaque_lattice.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0, r8) readonly uniform image3D lattice;
layout(set = 0, binding = 1, r16ui) readonly uniform uimage3D material;
layout(set = 0, binding = 2, r8) writeonly uniform image3D opaque;

void main() {
	ivec3 i = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(i, ivec3(LOD_CHUNK_LATTICE)))) return;
	float v = imageLoad(lattice, i).r;
	if (decode_sdf(v) <= 0.0 && mat_transparent(imageLoad(material, i).r)) {
		// ve::lod_outside_byte: L0 stores metres, so half a 0.4 m cell is 0.2; coarser levels
		// store SDF_RANGE per two cells, so half a cell is a quarter of the range.
		float half_cell = lpc.grid.w <= 0.4 ? 0.5 * lpc.grid.w : 0.25 * SDF_RANGE;
		v = quantise_sdf(half_cell);
	}
	imageStore(opaque, i, vec4(v));
}
