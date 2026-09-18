#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "shade.glslh"
#include "grass.glslh"

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_FIELDS } grass;
// One uvec4 per surviving cell. A NEAR entry is the GLOBAL brick coordinate in .xyz (signed,
// bit-cast) with .w == 0; a FAR entry carries its cell's world XZ origin, the ground height
// stage 1 found there and the oct-packed ground normal, with bit 31 of .w set. Sixteen bytes
// rather than four because the far rings have no resident brick to re-read: everything stage
// 2 needs about a far cell has to travel in the list. The near entry spends three of those
// words on a coordinate that used to be bit-packed into one -- the packing bounded the box
// height, and the box is now as tall as the reach.
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

// One far LoD cell. `f` runs over far.x rings of far.z cells each; ring r (1-based) covers out
// to kGrassFarBaseM << r in cells of BRICK_SIZE << r, so every ring has the same cell count
// and the dispatch grows with the ring COUNT rather than with the cube of the radius. Cells
// inside the near reach are dropped: the raymarched area is the near field's, brick by brick.
void far_cell(int f) {
	int per = grass.far.z;
	if (grass.far.x <= 0 || per <= 0 || grass.far.w <= 0) return;
	int ring = 1 + f / per;
	if (ring > grass.far.x) return;
	int k = f - (ring - 1) * per;
	int dim = grass.far.y;
	float cell = BRICK_SIZE * float(1 << ring);
	// The ring's radius comes from its own GRID, not from the reach: the grid is
	// 2 * outer / cell cells wide plus two of slack (ve::grass_layout), so inverting that is
	// the one radius the annulus test and the cell lattice below cannot disagree about. The
	// far schedule is ABSOLUTE -- 80 / 160 / 320 m in 1.6 / 3.2 / 6.4 m cells -- so extending
	// the near reach to the raymarcher's seam moves where ring 1 begins, not how coarse it is.
	float outer = float(dim - 2) * cell * 0.5;
	// ...but the rings still have to MEET the near field, wherever it happens to end. Rings 2+
	// start at their predecessor's outer edge; ring 1 has no predecessor, and its inner edge is
	// the near field's -- which is NOT `reach` metres horizontally. The near field owns a
	// SPHERE around the camera, so its edge on the ground moves with the camera's height and is
	// the sphere test below far_ground(). Testing it here in XZ is what used to lose grass.
	float inner = max(outer * 0.5, grass.cam.w);

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
	// Ring 1 keeps its whole disc; the near field's share of it is dropped by the sphere test
	// below, once the ground under the cell is known. Rings 2+ keep the cheap XZ test, because
	// their inner edge is another ring's outer edge rather than the near field's.
	// ponytail: ring 1 therefore sphere-traces its whole disc every frame, including ground the
	// near field already owns -- a couple of thousand extra far_ground() traces at the default
	// reach. Add an XZ pre-filter if the far pass ever shows up in a profile.
	if (ring > 1) {
		vec2 farthest = mix(lo2, hi2, step(grass.cam.xz, 0.5 * (lo2 + hi2)));
		if (distance(farthest, grass.cam.xz) < inner) return;
	}

	// The vertical span the column is searched over. Terrain sits in a bounded band around
	// SURFACE_Y (the stages' amplitudes are bounded -- see shaders/stages), while the camera
	// can be anywhere above it, so the span has to follow the BAND and not the camera. It used
	// to be `cam.y ± outer`: a bound on how far terrain climbs across the ring radius, but
	// anchored on the camera. Once the camera was more than `outer` above the ground,
	// far_ground() hit `bot` before it reached the surface, so ring 1 placed nothing under the
	// camera while the outer rings -- whose larger `outer` reaches further down -- kept drawing
	// further out (near grass vanished, far grass did not). Sphere tracing makes the taller
	// empty span nearly free; the taller AABB does weaken the frustum pre-filter below, but the
	// precise blade-box test after far_ground() still culls, so the cost is traces, not blades.
	const float kTerrainBandM = 512.0; // > the stages' largest |height| (relief 310 + mesas 45 + hills 10)
	vec3 lo3 = vec3(lo2.x, min(grass.cam.y - outer, SURFACE_Y - kTerrainBandM), lo2.y);
	vec3 hi3 = vec3(hi2.x, max(grass.cam.y + outer, SURFACE_Y + kTerrainBandM), hi2.y);
	if (grass_brick_culled(lo3, hi3, grass.planes)) return;

	float gy;
	if (!far_ground(0.5 * (lo2 + hi2), hi3.y, lo3.y, cell * 0.05, gy)) return;

	vec3 p = vec3(0.5 * (lo2.x + hi2.x), gy, 0.5 * (lo2.y + hi2.y));
	// The near field's edge on the GROUND is where its reach SPHERE cuts it: sqrt(reach^2 -
	// height^2) metres out, shrinking to nothing as the camera climbs. Ring 1's inner radius
	// used to be `reach` measured in XZ, which claims the wrong ground from any height above
	// the terrain: the band between the sphere's footprint and `reach` belonged to NEITHER
	// field, so grass disappeared under a high camera while the rings kept drawing further
	// out. Past reach_m of height the near field's whole footprint was inside that band. This
	// is the correct edge, and it also keeps the two fields from doubling up at ground level.
	if (distance(p, grass.cam.xyz) < grass.cam.w) return;
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
	// Threads: one per near column, then one per far cell. The brick-list CAPACITY
	// (grass.limits.y) is no longer the thread count -- a column may fill several entries --
	// so the bound is the dispatch's own width.
	int near_threads = grass.brick_dim.w;
	int far_threads = grass.far.x * grass.far.z;
	if (i >= uint(near_threads + far_threads)) return;
	if (i >= uint(near_threads)) { far_cell(int(i) - near_threads); return; }
	near_column(int(i));
}
