#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "shade.glslh"
#include "grass.glslh"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_FIELDS } grass;
// One uvec4 per surviving cell. A NEAR entry is the packed brick local coordinate in .x with
// .w == 0; a FAR entry carries its cell's world XZ origin, the ground height stage 1 found
// there and the oct-packed ground normal, with bit 31 of .w set. Sixteen bytes rather than
// four because the far rings have no resident brick to re-read: everything stage 2 needs
// about a far cell has to travel in the list.
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

// The far LoD rings are scattered against the FIELD, not the brick atlas: full-resolution
// bricks are only resident for roughly the first 60 m (lod_grid.h), which is where grass
// used to stop dead. eval_field answers at any distance. No volume or override bindings are
// declared, so a far ring sees the procedural terrain without the player's edits -- the
// documented cost of grass past the residency radius (GrassSettings::far_lod_rings).
#define FIELD_OP_POOL_BINDING 11
#include "field.glslh"

#include "brick_atlas.glslh"

// Ground under one far cell, or false when the column misses the surface. Sphere-traced
// DOWN from the top of the span: the field is NOT a strict distance field -- its gradient
// exceeds 1 on a slope -- so each step is taken at 0.7 of the reported distance and the
// crossing is bracketed and bisected rather than trusted. `min_step` only breaks a stall
// where the field reports a vanishing distance; it is deliberately far smaller than a step
// the trace would otherwise take, because a floor large enough to force progress would
// overshoot the surface and bracket the wrong metre of terrain.
bool far_ground(vec2 xz, float top, float bot, float min_step, out float ground_y) {
	ground_y = 0.0;
	float y = top;
	float air = top;
	bool hit = false;
	for (int i = 0; i < 32; i++) {
		float s; uint m;
		eval_field(vec3(xz.x, y, xz.y), 0u, 0u, s, m);
		if (s <= 0.0) { hit = true; break; }
		air = y;
		y -= max(s * 0.7, min_step);
		if (y < bot) return false;
	}
	if (!hit) return false;
	float solid = y;
	for (int i = 0; i < 8; i++) {
		float mid = 0.5 * (air + solid);
		float s; uint m;
		eval_field(vec3(xz.x, mid, xz.y), 0u, 0u, s, m);
		if (s <= 0.0) solid = mid; else air = mid;
	}
	ground_y = 0.5 * (air + solid);
	return true;
}

// One far LoD cell. `f` runs over far.x rings of far.z cells each; ring r (1-based) covers
// out to reach << r in cells of BRICK_SIZE << r, so every ring has the same cell count and
// the dispatch grows with the ring COUNT rather than with the cube of the radius.
void far_cell(int f) {
	int per = grass.far.z;
	if (grass.far.x <= 0 || per <= 0 || grass.far.w <= 0) return;
	int ring = 1 + f / per;
	if (ring > grass.far.x) return;
	int k = f - (ring - 1) * per;
	int dim = grass.far.y;
	float cell = BRICK_SIZE * float(1 << ring);
	float outer = grass.cam.w * float(1 << ring);
	float inner = grass.cam.w * float(1 << (ring - 1));

	// Anchored on the ring's own cell lattice rather than on the camera, so a cell keeps its
	// blades as the camera moves and the far field does not crawl.
	ivec2 origin = ivec2(floor((grass.cam.xz - vec2(outer)) / cell));
	vec2 lo2 = vec2(origin + ivec2(k % dim, k / dim)) * cell;
	vec2 hi2 = lo2 + vec2(cell);

	// Annulus: nearest point past the ring's outer radius is out of range, farthest point
	// inside the inner radius belongs to the finer ring that already covers it.
	//
	// ponytail: a patch of ground crossing a ring boundary swaps cell size, so its blades
	// re-scatter in one frame. The width ramp is continuous across the seam and hides most
	// of it; hiding the rest needs a dither cross-fade, which means scattering the overlap
	// band TWICE. Not worth it until it is visibly the worst thing in the far field.
	vec2 nearest = clamp(grass.cam.xz, lo2, hi2);
	if (distance(nearest, grass.cam.xz) > outer) return;
	vec2 farthest = mix(lo2, hi2, step(grass.cam.xz, 0.5 * (lo2 + hi2)));
	if (distance(farthest, grass.cam.xz) < inner) return;

	// The vertical span the column is searched over. A ring radius is a generous bound on
	// how far terrain climbs across that same radius, and sphere tracing makes an empty
	// span nearly free.
	vec3 lo3 = vec3(lo2.x, grass.cam.y - outer, lo2.y);
	vec3 hi3 = vec3(hi2.x, grass.cam.y + outer, hi2.y);
	if (grass_brick_culled(lo3, hi3, grass.planes)) return;

	float gy;
	if (!far_ground(0.5 * (lo2 + hi2), hi3.y, lo3.y, cell * 0.05, gy)) return;

	vec3 p = vec3(0.5 * (lo2.x + hi2.x), gy, 0.5 * (lo2.y + hi2.y));
	// Central differences at a quarter cell: the blade only needs which way is up and the
	// tangent plane stage 2 lays its blades on, not a shading normal.
	float e = cell * 0.25;
	float sxp, sxm, syp, sym, szp, szm; uint m;
	eval_field(p + vec3(e, 0, 0), 0u, 0u, sxp, m);
	eval_field(p - vec3(e, 0, 0), 0u, 0u, sxm, m);
	eval_field(p + vec3(0, e, 0), 0u, 0u, syp, m);
	eval_field(p - vec3(0, e, 0), 0u, 0u, sym, m);
	eval_field(p + vec3(0, 0, e), 0u, 0u, szp, m);
	eval_field(p - vec3(0, 0, e), 0u, 0u, szm, m);
	vec3 n = normalize(vec3(sxp - sxm, syp - sym, szp - szm));
	if (n.y < grass.blade.w) return; // too steep: no grass on cliff faces

	// A quarter metre INSIDE the ground: at the crossing itself the field reads sdf ~ 0 and
	// a pipeline that only labels solid samples would answer air.
	float s0; uint mat;
	eval_field(p - vec3(0.0, 0.25, 0.0), 0u, 0u, s0, mat);
	if (mat != MAT_GRASS_01) return;

	// Now that the ground height is known, cull against the box the BLADES actually occupy.
	// The search box above spans the whole ring radius vertically, so it passes the frustum
	// almost everywhere; this is the test that keeps a cell out of stage 2.
	if (grass_brick_culled(vec3(lo2.x, gy - cell, lo2.y),
			vec3(hi2.x, gy + cell + grass.blade.y, hi2.y), grass.planes)) return;

	uint out_index = atomicAdd(counters.brick_count, 1u);
	if (out_index >= uint(grass.limits.y)) return; // full: drop, never scribble
	brick_list.v[out_index] = uvec4(floatBitsToUint(lo2.x), floatBitsToUint(lo2.y),
			floatBitsToUint(gy),
			0x80000000u | (uint(oct_encode_snorm8(n)) << 8) | uint(ring));
	atomicMax(dispatch_args.x, out_index + 1u);
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
	if (i >= uint(grass.limits.y)) return;
	if (i >= uint(grass.brick_dim.w)) { far_cell(int(i) - grass.brick_dim.w); return; }

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
	brick_list.v[out_index] = uvec4(uint(local.x + 1024) | (uint(local.y + 512) << 11) |
			(uint(local.z + 1024) << 21), 0u, 0u, 0u);

	// Stage 2 runs one workgroup of 64 threads per brick, so the dispatch width is the
	// brick count. atomicMax rather than a store: every thread that appends may be the last.
	atomicMax(dispatch_args.x, out_index + 1u);
}
