#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "shade.glslh"
#include "grass.glslh"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_FIELDS } grass;
// One uvec4 per surviving brick: the GLOBAL brick coordinate in .xyz (signed, bit-cast),
// .w unused. Three words on a coordinate that used to be bit-packed into one -- the packing
// bounded the box height, and the box is now as tall as the reach.
layout(set = 0, binding = 1, std430) writeonly buffer BrickList { uvec4 v[]; } brick_list;
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
layout(set = 0, binding = 10, std140) uniform Region { GRASS_REGION_FIELDS } pc;

#include "brick_atlas.glslh"

// One XZ column of the near box: every resident surface brick in it, walking DOWN from the
// top. One thread per column rather than per brick because the box is now as tall as it is
// wide -- the raymarcher's whole resident sphere, not a slab around the camera -- and a thread
// per brick would dispatch its volume.
void near_column(int c) {
	ivec3 dim = grass.brick_dim.xyz;
	if (dim.x <= 0 || dim.z <= 0) return;
	int bx = grass.brick_min.x + c % dim.x;
	int bz = grass.brick_min.z + c / dim.x;
	vec2 lo2 = vec2(bx, bz) * BRICK_SIZE;
	vec2 hi2 = lo2 + vec2(BRICK_SIZE);

	// Horizontal distance cull first, to the column's nearest point so a column straddling the
	// boundary is kept rather than flickering. The box is square; the reach is a sphere.
	vec2 nearest2 = clamp(grass.cam.xz, lo2, hi2);
	float dxz = distance(nearest2, grass.cam.xz);
	if (dxz > grass.cam.w) return;

	// Vertical span: the sphere's own height over THIS column, intersected with the box. A
	// column 50 m out walks a few metres, not the full box height, so the walk costs the
	// sphere's volume rather than its bounding cube's.
	float dy = sqrt(max(grass.cam.w * grass.cam.w - dxz * dxz, 0.0));
	int y_lo = max(grass.brick_min.y, int(floor((grass.cam.y - dy) / BRICK_SIZE)));
	int y_hi = min(grass.brick_min.y + dim.y - 1, int(floor((grass.cam.y + dy) / BRICK_SIZE)));
	if (y_hi < y_lo) return;

	// One frustum test for the whole column before the walk. Looking at the horizon puts most
	// of the box behind the camera, and this drops each of those columns for one plane loop.
	if (grass_brick_culled(vec3(lo2.x, float(y_lo) * BRICK_SIZE, lo2.y),
			vec3(hi2.x, float(y_hi + 1) * BRICK_SIZE, hi2.y), grass.planes)) return;

	int found = 0;
	for (int by = y_hi; by >= y_lo; by--) {
		ivec3 brick = ivec3(bx, by, bz);
		int rs = region_slot_of(brick);
		if (rs < 0) {
			// Whole region absent: jump to the bottom of its 32-brick span in Y instead of
			// re-resolving region_map for each of them. `& ~31` floors, negatives included.
			by = by & ~31;
			continue;
		}
		int slot = slot_in_region(rs, brick);
		if (slot < 0) continue;                       // not resident
		if (!brick_straddles_surface(slot)) continue; // no surface crossing: nothing to stand on

		vec3 lo = vec3(brick) * BRICK_SIZE;
		vec3 hi = lo + vec3(BRICK_SIZE);
		if (grass_brick_culled(lo, hi, grass.planes)) continue;
		vec3 nearest = clamp(grass.cam.xyz, lo, hi);
		if (distance(nearest, grass.cam.xyz) > grass.cam.w) continue;

		uint out_index = atomicAdd(counters.brick_count, 1u);
		if (out_index >= uint(grass.limits.y)) return; // full: drop, never scribble
		// Global brick coordinate, one word each. Stage 2 needs no brick_min and no unpacking,
		// and nothing here bounds the box's height any more.
		brick_list.v[out_index] = uvec4(uint(brick.x), uint(brick.y), uint(brick.z), 0u);

		// Stage 2 runs one workgroup of 64 threads per brick, so the dispatch width is the
		// brick count. atomicMax rather than a store: every thread that appends may be the last.
		atomicMax(dispatch_args.x, out_index + 1u);
		// GRASS_COLUMN_BRICKS is what the brick list reserved for this column, so the walk
		// stops there rather than eating another column's reserve.
		if (++found >= GRASS_COLUMN_BRICKS) return;
	}
}

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
	// Threads: one per near column. The brick-list CAPACITY (grass.limits.y) is not the
	// thread count -- a column may fill several entries -- so the bound is the dispatch's
	// own width.
	if (i >= uint(grass.brick_dim.w)) return;
	near_column(int(i));
}
