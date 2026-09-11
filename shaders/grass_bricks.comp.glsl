#[compute]
#version 460

#include "common.glslh"
#include "grass.glslh"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;
layout(set = 0, binding = 1, std430) writeonly buffer BrickList { uint v[]; } brick_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint brick_count; uint blade_count;
		uint high_water; uint pad; } counters;
layout(set = 0, binding = 3, std430) writeonly buffer Dispatch { uint x, y, z; } dispatch_args;

void main() {
	// Stub: Task 5 fills in the brick cull. One thread does the bookkeeping so the buffers
	// are DEFINED rather than merely allocated -- on this machine a fresh RD buffer reads
	// back as zero, so an undefined buffer and an empty one are indistinguishable and the
	// difference has to be made explicit here.
	if (gl_GlobalInvocationID.x != 0u) return;
	counters.brick_count = 0u;
	counters.blade_count = 0u;
	dispatch_args.x = 0u;
	dispatch_args.y = 1u;
	dispatch_args.z = 1u;
	brick_list.v[0] = 0u;
}
