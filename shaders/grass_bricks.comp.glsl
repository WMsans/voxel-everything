#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "grass.glslh"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_BLOCK } grass;
layout(set = 0, binding = 1, std430) writeonly buffer BrickList { uint v[]; } brick_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint brick_count; uint blade_count;
		uint high_water; uint pad; } counters;
layout(set = 0, binding = 3, std430) buffer Dispatch { uint x, y, z; } dispatch_args;
layout(set = 0, binding = 4, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 5, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 6, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;
layout(set = 0, binding = 7) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 8) uniform usampler3D mat_atlas;
layout(set = 0, binding = 9, std430) readonly buffer Palette { uint ids[]; } palette_buf;
// The region-window block keeps the name `pc` because brick_atlas.glslh addresses the
// window through `pc.dims`, `pc.region_origin` and `pc.atlas_bricks`; the grass-params
// block above is renamed to `grass` so the two never collide.
layout(set = 0, binding = 10, std140) uniform Region { ivec4 dims; ivec4 region_origin;
		ivec4 atlas_bricks; } pc;

#include "brick_atlas.glslh"

void main() {
	uint i = gl_GlobalInvocationID.x;
	// Thread 0 seeds the stage-2 dispatch dimensions that do not depend on the count.
	if (i == 0u) { dispatch_args.y = 1u; dispatch_args.z = 1u; }
	// brick_atlas.glslh declares sdf_atlas / mat_atlas / palette_buf for sampling helpers
	// this stage never calls. The branch below can never execute, but the references keep
	// those bindings live in shader reflection so the uniform set shape matches the source:
	// a declared-but-unused binding still has to be provided.
	if (i > 0xFFFFFFFEu) {
		counters.brick_count += uint(textureLod(sdf_atlas, vec3(0.0), 0.0).r) * 0u +
				palette_buf.ids[0] * 0u + texelFetch(mat_atlas, ivec3(0), 0).r * 0u;
	}
	if (i >= uint(grass.brick_dim.w)) return;

	ivec3 dim = grass.brick_dim.xyz;
	ivec3 local = ivec3(int(i) % dim.x, (int(i) / dim.x) % dim.y, int(i) / (dim.x * dim.y));
	ivec3 brick = grass.brick_min.xyz + local;

	int slot = slot_at(brick);
	if (slot < 0) return;                       // not resident
	if (!brick_straddles_surface(slot)) return; // no surface crossing: nothing to stand on

	vec3 lo = vec3(brick) * BRICK_SIZE;
	vec3 hi = lo + vec3(BRICK_SIZE);
	if (grass_brick_culled(lo, hi, grass.planes)) return;

	// Distance cull against the reach, measured to the brick's nearest point so a brick
	// straddling the boundary is kept rather than flickering.
	vec3 nearest = clamp(grass.cam.xyz, lo, hi);
	if (distance(nearest, grass.cam.xyz) > grass.cam.w) return;

	uint out_index = atomicAdd(counters.brick_count, 1u);
	if (out_index >= uint(grass.limits.y)) return; // full: drop, never scribble
	// Brick-list packing: 11 bits for X/Z, 10 for Y (11+10+11 = 32 bits exactly). The
	// horizontal reach advertises 256 m, which gives brick dims up to ~641, so
	// local+512 reaches ~1152 and spills out of a 10-bit field into its neighbour.
	// X/Z use bias 1024 (range 1024..1664, under the 2048 ceiling); Y keeps bias 512
	// because the vertical reach caps at 64 m (dim_y ~161, local+512 stays under 1024).
	// Three 11-bit fields would need 33 bits and NOT fit a uint -- hence the split.
	brick_list.v[out_index] = uint(local.x + 1024) | (uint(local.y + 512) << 11) |
			(uint(local.z + 1024) << 21);

	// Stage 2 runs one workgroup of 64 threads per brick, so the dispatch width is the
	// brick count. atomicMax rather than a store: every thread that appends may be the last.
	atomicMax(dispatch_args.x, out_index + 1u);
}
