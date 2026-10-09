#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "conifer_pass.glslh"

// One workgroup per card tree, one thread per candidate clump (spec §5.2). Writes the leaf
// module's 32-byte LeafClump, so LeafRasterPass draws conifer needles with its own shaders.
layout(local_size_x = 128) in;

layout(set = 0, binding = 0, std140) uniform Params { CONIFER_PASS_FIELDS } conifer;
layout(set = 0, binding = 1, std430) readonly buffer CardList { ConiferRecord v[]; } card_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint card_count; uint impostor_count;
		uint clump_count; uint high_water; } counters;
layout(set = 0, binding = 3, std430) buffer DrawArgs { uint vertex_count; uint instance_count;
		uint first_vertex; uint first_instance; } draw_args;
#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 5
#include "sun_light.glslh"
layout(set = 0, binding = 6, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 7, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 8, std430) readonly buffer RegionSlotCounts { int n[]; } region_slot_counts;
layout(set = 0, binding = 9) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 10) uniform usampler3D mat_atlas;
layout(set = 0, binding = 11, std140) uniform Region { LEAF_REGION_FIELDS } pc;
layout(set = 0, binding = 12, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 13, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;

#include "brick_atlas.glslh"
#include "sun_march.glslh"
#include "leaf.glslh"
layout(set = 0, binding = 4, std430) writeonly buffer Instances { LeafClump c[]; } instances;

// The record carries the whole shape; nothing here walks the field.
vec4 conifer_ground(vec2 xz) { return vec4(0.0f); }
#include "conifer.glslh"

void main() {
	uint tree_idx = gl_WorkGroupID.x;
	if (tree_idx >= counters.card_count) return;
	ConiferRecord r = card_list.v[tree_idx];
	Conifer c = conifer_unpack(r.a, r.b);
	vec3 cam = vec3(conifer.cam.x, conifer.cam.y, conifer.cam.z);
	float dist = length(c.foot + vec3(0.0f, 0.5f * c.height, 0.0f) - cam);
	uint budget = conifer_clump_budget(conifer.limits.z, conifer.cam.w, dist);
	uint cand = gl_LocalInvocationID.x;
	if (cand >= budget) return;

	uint h = tree_hash(c.hash ^ (cand * 0x27D4EB2Fu));
	// Area-weighted crown height: a cone's surface per metre of height goes as (1 - s).
	float s = 1.0f - sqrt(tree_unit(h));
	float ang = tree_unit(tree_hash(h ^ 0x41u)) * 6.2831853f;
	vec3 radial = vec3(cos(ang), 0.0f, sin(ang));
	float rr = conifer_profile(c, s) * mix(conifer.look.x, 1.0f, tree_unit(tree_hash(h ^ 0x42u)));
	vec3 p = c.foot + vec3(0.0f, c.crown_base + s * (c.height - c.crown_base), 0.0f) + radial * rr;
	vec3 n = conifer_normal(c, radial, conifer.reach.z);

	// Raster mode marches nothing: the sun map owns every pixel there (as leaves and grass).
	float sun = conifer.flags.x != 0 ? 1.0f : terrain_sun_visibility(p, RAY_SHADOW_DIST);
	sun *= mix(1.0f - conifer.reach.w, 1.0f, s);
	float radius = conifer.reach.y * sqrt(float(conifer.limits.z) / max(float(budget), 1.0f));
	float ratio = clamp(radius / max(c.R, 1e-3f), 0.0f, 1.0f);

	uint slot = atomicAdd(counters.clump_count, 1u);
	atomicMax(counters.high_water, slot + 1u);
	if (slot >= uint(conifer.limits.x)) {
		atomicMin(counters.clump_count, uint(conifer.limits.x));
		return;
	}
	LeafClump o;
	o.a = vec4(p, radius);
	o.b = vec4(uintBitsToFloat(leaf_pack_normal(n, sun)), uintBitsToFloat(h),
			leaf_pack_pair(s, ratio), tree_unit(tree_hash(h ^ 0x51u)) * 6.2831853f);
	instances.c[slot] = o;
	atomicMax(draw_args.vertex_count, (slot + 1u) * 6u);
	draw_args.instance_count = 1u;
}
