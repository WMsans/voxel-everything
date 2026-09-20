#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "leaf.glslh"

// One workgroup per surviving tree, one thread per candidate clump. 128 is the ceiling on
// LeafSettings::clumps_per_tree, so a tree never needs a second group.
layout(local_size_x = 128) in;

layout(set = 0, binding = 0, std140) uniform Params { LEAF_PARAMS_FIELDS } leaf;
layout(set = 0, binding = 1, std430) readonly buffer TreeList { LeafTree v[]; } tree_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint tree_count; uint clump_count;
		uint high_water; uint pad; } counters;
layout(set = 0, binding = 3, std430) buffer DrawArgs { uint vertex_count; uint instance_count;
		uint first_vertex; uint first_instance; } draw_args;
layout(set = 0, binding = 4, std430) writeonly buffer Instances { LeafClump c[]; } instances;
#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 5
#include "sun_light.glslh"
layout(set = 0, binding = 6, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 7, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 8, std430) readonly buffer RegionSlotCounts { int n[]; } region_slot_counts;
layout(set = 0, binding = 9) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 10) uniform usampler3D mat_atlas;
// The region-window block keeps the name `pc` because brick_atlas.glslh addresses the window
// through pc.dims, pc.region_origin and pc.atlas_bricks; the leaf-params block above is named
// `leaf` so the two never collide. Same convention leaf_trees.comp.glsl uses.
layout(set = 0, binding = 11, std140) uniform Region { LEAF_REGION_FIELDS } pc;
// brick_atlas.glslh's material_at() resolves the atlas texel through the palette, and its
// brick_straddles_surface() helpers reference brick_flags even where this stage never calls
// them -- GLSL semantically analyses every function in the translation unit. A declared
// binding still has to be provided, so both travel here as they do in stage 1 (10/11 there);
// the brief's stage-2 numbering stops at 11 and these two are the forced appendices, the
// same reconciliation the leaf_trees landing documents.
layout(set = 0, binding = 12, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 13, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;

#include "brick_atlas.glslh"
#include "sun_march.glslh"
#include "tree.glslh"

// A point on the unit sphere from a hash. Marsaglia: uniform in z, uniform in azimuth.
vec3 leaf_unit_sphere(uint h) {
	float z = tree_snorm(h) ;
	float a = tree_unit(tree_hash(h ^ 0x31u)) * 6.2831853;
	float r = sqrt(max(0.0, 1.0 - z * z));
	return vec3(r * cos(a), z, r * sin(a));
}

void main() {
	uint tree_idx = gl_WorkGroupID.x;
	if (tree_idx >= counters.tree_count) return;

	LeafTree t = tree_list.v[tree_idx];
	uint budget = uint(t.b.w);
	uint cand = gl_LocalInvocationID.x;
	if (cand >= budget) return;

	TreeParams tp;
	tp.cell = leaf.tree.x;
	tp.density = leaf.tree.y;
	tp.crown_radius = leaf.tree.z;
	tp.trunk_radius = leaf.tree.w;
	tp.trunk_height = leaf.shape.x;
	tp.branch_radius_min = leaf.shape.y;
	tp.max_slope = leaf.shape.z;

	// Reconstruct the tree from the same hash the stage used, so the lobes a clump sits on
	// are the branch tips the terrain stage actually carved.
	Tree tr;
	tr.hash = floatBitsToUint(t.b.y);
	tr.crown = t.a.xyz;
	tr.crown_r = t.a.w;
	tr.base = vec3(t.a.x, t.b.x, t.a.z);
	tr.height = t.a.y - t.b.x;
	// The SAME jittered radius tree_at() gave this tree: the attachment probe below runs
	// tree_skeleton_sdf(), which reads it, so an unjittered trunk would aim the probe at a
	// surface up to 20% off where the wood actually is.
	tr.radius = tp.trunk_radius * (1.0 + 0.20 * tree_snorm(tree_hash(tr.hash ^ 0x52u)));
	tr.present = true;

	uint h = tree_hash(tr.hash ^ (cand * 0x27D4EB2Fu));
	int lobe_i = int(h % uint(TREE_LOBES));
	vec3 lobe_c = tree_lobe(tr, tp, lobe_i);
	float lobe_r = tree_lobe_radius(tr, tp, lobe_i);

	// ON THE SHELL, not through the volume. The crown interior is never seen and filling it
	// is pure overdraw -- this is the single biggest structural saving in the module.
	vec3 dir = leaf_unit_sphere(tree_hash(h ^ 0x41u));
	float rr = mix(leaf.clump.y, 1.0, tree_unit(tree_hash(h ^ 0x42u)));
	vec3 p = lobe_c + dir * (lobe_r * rr);

	// ATTACHMENT -- the per-clump chop check, and the reason a carved branch loses its
	// leaves. Stage 1 asks "does this tree still stand?" once, at one point on the trunk;
	// every clump then inherited that answer and hung in mid-air when the wood it grew on was
	// carved away and the trunk was not. So each clump re-asks the question at ITS OWN wood:
	// the nearest point on the skeleton, read from the LIVE atlas exactly as a grass blade
	// reads the voxel it stands on. No invalidation anywhere -- carve a limb, its leaves are
	// gone on the next frame.
	float wood_d = tree_skeleton_sdf(p, tr, tp);
	// Forward differences at a fifth of a voxel: the skeleton is analytic and exact, so one
	// Newton step lands ON the wood -- a second iteration was measured to move not one clump.
	const vec2 e = vec2(VOXEL_SIZE * 0.2, 0.0);
	vec3 grad = vec3(tree_skeleton_sdf(p + e.xyy, tr, tp),
	                 tree_skeleton_sdf(p + e.yxy, tr, tp),
	                 tree_skeleton_sdf(p + e.yyx, tr, tp)) - vec3(wood_d);
	// A quarter of the tip radius INSIDE the surface: past the trilinear filter's reach, never
	// through the far side of the thinnest branch the shape can make.
	vec3 anchor = p - grad / max(length(grad), 1e-9) * (wood_d + tp.branch_radius_min * 0.25);
	ivec3 anchor_brick = ivec3(floor(anchor / BRICK_SIZE));
	int anchor_slot = slot_at(anchor_brick);
	if (anchor_slot < 0) return;
	if (brick_sdf(anchor_slot, (anchor - vec3(anchor_brick) * BRICK_SIZE) / VOXEL_SIZE) > 0.0) return;
	if (material_at(anchor, anchor_brick, anchor_slot) != MAT_BARK) return;

	// The technique: the shading normal is transferred from a sphere over the WHOLE crown,
	// not taken from the card's real facing, and then leaned toward this clump's own lobe by
	// canopy_roundness. At 0 the crown shades as one smooth ball and the overlapping masses
	// disappear; at 1 every lobe is a separate ball and it reads as lumps.
	vec3 n_crown = normalize(p - tr.crown);
	// shell_min = 0 and rr = 0 put a clump AT the lobe centre, where normalize() would
	// divide by zero: the max() keeps the vector finite (when it binds the facing is
	// degenerate and any direction is as good as another).
	vec3 to_lobe = p - lobe_c;
	vec3 n_lobe = to_lobe / max(length(to_lobe), 1e-3);
	vec3 n = normalize(mix(n_crown, n_lobe, leaf.clump.z));

	// Sun visibility, marched by the SAME function the raymarcher and the grass scatter use,
	// so a clump is shadowed by the terrain and the trunks exactly as the ground under it is.
	// It does NOT see other leaves: foliage is not in the SDF. That limitation is by design.
	// (The RAY_SHADOW_DIST argument is this call's one deviation from the plan snippet:
	// sun_march.glslh's terrain_sun_visibility takes a march distance, as grass_scatter's
	// per-blade call passes; a clump marches the same 60 m a blade does.)
	float sun = terrain_sun_visibility(p, RAY_SHADOW_DIST);

	// Crown depth: 0 at the bottom of the crown sphere, 1 at the top. Drives both the
	// interior self-shade here and the ambient-occlusion gradient the fragment bakes in.
	float depth_t = clamp((p.y - (tr.crown.y - tr.crown_r)) / max(2.0 * tr.crown_r, 1e-3),
			0.0, 1.0);
	sun *= mix(1.0 - leaf.clump.w, 1.0, depth_t);

	float radius = leaf.clump.x * sqrt(float(leaf.limits.z) / max(float(budget), 1.0));
	float ratio = clamp(radius / max(tr.crown_r, 1e-3), 0.0, 1.0);

	uint slot = atomicAdd(counters.clump_count, 1u);
	atomicMax(counters.high_water, slot + 1u);
	if (slot >= uint(leaf.limits.x)) {
		// Clamp, never scribble: an overflowing frame drops clumps and the high-water mark
		// records it. Same treatment lod_overflow_logged gives page overflow.
		atomicMin(counters.clump_count, uint(leaf.limits.x));
		return;
	}

	LeafClump c;
	c.a = vec4(p, radius);
	c.b = vec4(uintBitsToFloat(leaf_pack_normal(n, sun)), uintBitsToFloat(h),
			leaf_pack_pair(depth_t, ratio), tree_unit(tree_hash(h ^ 0x51u)) * 6.2831853);
	instances.c[slot] = c;

	// The containment telemetry, counters.pad: the CARD ENVELOPE -- how far the shell
	// reached INCLUDING the card radius, as a fixed-point multiple of the crown radius,
	// reduced on the GPU where crown_r is in hand. Task 2's invariant bounds the clump
	// CENTRE at 1.0 crown radii (max lobe reach 0.62 + max lobe radius 0.38); the density
	// LOD deliberately inflates the CARD as the budget thins, and the + radius term is
	// what reports that -- the default-derived worst whole-card reach is 1.60, and
	// test_the_crown_envelope_reports_the_card_radius pins the metric above 1.0 so the
	// term can never be dropped again without a failing test.
	atomicMax(counters.pad, uint(((length(p - tr.crown) + radius) / max(tr.crown_r, 1e-3)) * 65536.0));

	// Six vertices per clump: two triangles, pulled from this buffer with no vertex format.
	atomicMax(draw_args.vertex_count, (slot + 1u) * 6u);
	draw_args.instance_count = 1u;
}
