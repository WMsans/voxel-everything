#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "shade.glslh"
#include "grass.glslh"

// One workgroup per surviving brick; each thread is one candidate blade. 64 threads is the
// ceiling on GrassSettings::blades_per_brick, so a brick never needs a second group.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { GRASS_PARAMS_BLOCK } grass;
layout(set = 0, binding = 1, std430) readonly buffer BrickList { uint v[]; } brick_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint brick_count; uint blade_count;
		uint high_water; uint pad; } counters;
layout(set = 0, binding = 3, std430) buffer DrawArgs { uint vertex_count; uint instance_count;
		uint first_vertex; uint first_instance; } draw_args;
layout(set = 0, binding = 4, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 5, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 6, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;
layout(set = 0, binding = 7, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 8) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 9) uniform usampler3D mat_atlas;
// The region-window block keeps the name `pc` because brick_atlas.glslh addresses the
// window through `pc.dims`, `pc.region_origin` and `pc.atlas_bricks`; the grass-params
// block above is renamed to `grass` so the two never collide. Same block, same binding
// as stage 1, fed from the same pass-owned UBO.
layout(set = 0, binding = 10, std140) uniform Region { ivec4 dims; ivec4 region_origin;
		ivec4 atlas_bricks; } pc;
layout(set = 0, binding = 11, std430) writeonly buffer Instances { GrassBlade b[]; } instances;

#include "brick_atlas.glslh"

const uint GRASS_MATERIAL = 1u; // grass_01, ve::kMaterials[0]

// Central differences over one voxel. Cheaper and steadier than the field evaluator, and a
// blade only needs to know which way is up, not a shading normal.
vec3 surface_normal(vec3 p) {
	const float e = VOXEL_SIZE;
	return normalize(vec3(
		world_sdf(p + vec3(e, 0, 0)) - world_sdf(p - vec3(e, 0, 0)),
		world_sdf(p + vec3(0, e, 0)) - world_sdf(p - vec3(0, e, 0)),
		world_sdf(p + vec3(0, 0, e)) - world_sdf(p - vec3(0, 0, e))));
}

void main() {
	uint brick_index = gl_WorkGroupID.x;
	if (brick_index >= counters.brick_count) return;

	uint packed = brick_list.v[brick_index];
	// Unpacks the 11/10/11-bit layout stage 1 writes (see grass_bricks.comp.glsl): X/Z
	// bias 1024 in 11-bit fields, Y bias 512 in a 10-bit field.
	ivec3 local = ivec3(int(packed & 0x7FFu) - 1024, int((packed >> 11) & 0x3FFu) - 512,
			int((packed >> 21) & 0x7FFu) - 1024);
	ivec3 brick = grass.brick_min.xyz + local;
	vec3 base = vec3(brick) * BRICK_SIZE;

	// Ring from the brick's centre, so every blade in a brick agrees on its budget.
	float d = distance(base + vec3(BRICK_SIZE * 0.5), grass.cam.xyz);
	int ring = 3;
	for (int i = 0; i < 4; i++) { if (d <= grass.ring_end[i]) { ring = i; break; } }
	if (d > grass.ring_end[3]) return;
	uint budget = uint(grass.ring_blades[ring]);
	uint lane = gl_LocalInvocationID.x;
	if (lane >= budget) return;

	// Jittered XZ inside the brick. The hash is seeded from the WORLD brick coordinate, so
	// a blade keeps its position as the camera moves and the field does not crawl.
	uint h = grass_hash3(brick, lane * 2654435761u);
	float jx = grass_unit(h);
	float jz = grass_unit(grass_hash(h ^ 0x9E3779B9u));
	vec3 column = base + vec3(jx * BRICK_SIZE, 0.0, jz * BRICK_SIZE);

	// Find the crossing inside this brick's 0.8 m span: eight steps to bracket it, then
	// four bisections. Bounded by construction -- the brick is known to straddle.
	float y0 = base.y;
	float y1 = base.y + BRICK_SIZE;
	float s0 = world_sdf(vec3(column.x, y0, column.z));
	bool found = false;
	for (int i = 1; i <= 8; i++) {
		float y = mix(y0, y1, float(i) / 8.0);
		float s = world_sdf(vec3(column.x, y, column.z));
		if (s0 * s <= 0.0) { y1 = y; found = true; break; }
		y0 = y;
		s0 = s;
	}
	if (!found) return; // this column misses the surface even though the brick straddles
	for (int i = 0; i < 4; i++) {
		float ym = 0.5 * (y0 + y1);
		if (world_sdf(vec3(column.x, ym, column.z)) * s0 <= 0.0) y1 = ym; else y0 = ym;
	}
	vec3 p = vec3(column.x, 0.5 * (y0 + y1), column.z);

	vec3 n = surface_normal(p);
	if (n.y < grass.blade.w) return; // too steep: no grass on cliff faces

	int slot = slot_at(ivec3(floor(p / BRICK_SIZE)));
	if (slot < 0) return;
	if (material_at(p, ivec3(floor(p / BRICK_SIZE)), slot) != GRASS_MATERIAL) return;

	uint index = atomicAdd(counters.blade_count, 1u);
	if (index >= uint(grass.limits.x)) return; // at capacity: drop, never scribble

	// Clump noise at world XZ: neighbouring blades share height and colour, which is what
	// makes a field patchy rather than a lawn.
	ivec3 clump_cell = ivec3(int(floor(p.x * 0.35)), 0, int(floor(p.z * 0.35)));
	float clump = grass_unit(grass_hash3(clump_cell, 0x5BD1u));
	float jitter = (grass_unit(grass_hash(h ^ 0x85EBCA6Bu)) * 2.0 - 1.0) * grass.blade.z;
	float height = grass.blade.y * (1.0 + jitter) * mix(0.7, 1.15, clump);
	float lean = grass_unit(grass_hash(h ^ 0xC2B2AE35u)) * 6.2831853;

	GrassBlade blade;
	blade.a = vec4(p, height);
	blade.b = vec4(float(oct_encode_snorm8(n)), uintBitsToFloat(h), lean, clump);
	instances.b[index] = blade;

	// Three vertices per blade. atomicMax, not a store: any appending thread may be last.
	atomicMax(draw_args.vertex_count, (index + 1u) * 3u);
	draw_args.instance_count = 1u;

	// brick_atlas.glslh declares brick_flags for the flag-word helpers this stage never
	// calls. The branch below can never execute, but the reference keeps the binding live
	// in shader reflection so the uniform set shape matches the source -- the same reason
	// stage 1 keeps its unsampled textures alive.
	if (lane > 0xFFFFFFFEu) {
		counters.brick_count += brick_flags.v[0] * 0u;
	}
}
