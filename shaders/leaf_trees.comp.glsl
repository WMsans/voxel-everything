#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "leaf.glslh"

// One thread per cell in the camera-local XZ lattice. At the 250 m default that is about
// 36 x 36 cells -- trivial beside grass's quarter-million brick threads, because a tree
// occupies a whole cell and grass occupies a voxel.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { LEAF_PARAMS_FIELDS } leaf;
layout(set = 0, binding = 1, std430) writeonly buffer TreeList { LeafTree v[]; } tree_list;
layout(set = 0, binding = 2, std430) buffer Counters { uint tree_count; uint clump_count;
		uint high_water; uint pad; } counters;
layout(set = 0, binding = 3, std430) buffer DispatchArgs { uint x; uint y; uint z; } disp;
layout(set = 0, binding = 4, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 5, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 6, std430) readonly buffer RegionSlotCounts { int n[]; } region_slot_counts;
layout(set = 0, binding = 7) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 8) uniform usampler3D mat_atlas;
// The region-window block keeps the name `pc` because brick_atlas.glslh addresses the window
// through pc.dims, pc.region_origin and pc.atlas_bricks; the leaf-params block above is named
// `leaf` so the two never collide. Same convention grass_scatter.comp.glsl uses.
layout(set = 0, binding = 9, std140) uniform Region { LEAF_REGION_FIELDS } pc;
// brick_atlas.glslh's material_at() resolves the atlas texel through the palette, and its
// brick_straddles_surface() helpers reference brick_flags even where this stage never calls
// them -- GLSL semantically analyses every function in the translation unit. A declared
// binding still has to be provided, so both travel here exactly as they do in grass's
// stage 1 (palette 9/brick_flags 6 there; shifted past the Region block here).
layout(set = 0, binding = 10, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 11, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;

// The ground functions below come from the terrain pipeline's generated field source, which
// is installed as the field.glslh override at world init: it emits every stage body verbatim,
// so trees_ground_h/trees_ground_slope live there EXACTLY ONCE, reading the SAME P.hills_* /
// P.relief_* from set 1 as the trees stage itself. Including field.glslh (rather than
// re-declaring the arithmetic) is what keeps a canopy from ever standing where no trunk does.
// The op pool binding matches GrassScatterPass's contract: an empty 32-byte buffer, declared
// by field_ops.glslh, never indexed (stage 1 evaluates the analytic ground, not edits).
#define FIELD_OP_POOL_BINDING 12
#include "field.glslh"

#include "brick_atlas.glslh"
#include "tree.glslh"

TreeParams leaf_tree_params() {
	TreeParams tp;
	tp.cell = leaf.tree.x;
	tp.density = leaf.tree.y;
	tp.crown_radius = leaf.tree.z;
	tp.trunk_radius = leaf.tree.w;
	tp.trunk_height = leaf.shape.x;
	tp.branch_radius_min = leaf.shape.y;
	tp.max_slope = leaf.shape.z;
	return tp;
}

// True when the crown's bounding sphere is outside any frustum plane.
bool leaf_crown_culled(vec3 c, float r) {
	for (int i = 0; i < 6; i++)
		if (dot(leaf.planes[i].xyz, c) + leaf.planes[i].w < -r) return true;
	return false;
}

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= uint(leaf.cell_min.w)) return;

	TreeParams tp = leaf_tree_params();
	ivec2 cell = ivec2(leaf.cell_min.x + int(idx % uint(leaf.cell_dim.x)),
	                   leaf.cell_min.z + int(idx / uint(leaf.cell_dim.x)));

	vec2 xz = tree_cell_xz(cell, tp);
	float gh = trees_ground_h(xz);
	// trees_ground_h / trees_ground_slope: defined once in the generated field source
	// (shaders/stages/trees.field.glslh, emitted verbatim), reading set 1 P. This call site
	// is the whole R1 contract: no second copy of that arithmetic exists anywhere. gh keeps
	// the header terrain-free while tree_cell_present applies the spec §4 height band.
	Tree t = tree_at(cell, tp, leaf.shape.w + gh, gh, trees_ground_slope(xz));
	if (!t.present) return;

	float dist = length(t.crown - leaf.cam.xyz);
	if (dist > leaf.cam.w + t.crown_r) return;
	if (leaf_crown_culled(t.crown, t.crown_r)) return;

	// THE chop check. One atlas read, at a point a third of the way up the trunk -- inside
	// the trunk when it stands, air when it has been dug or painted away. The scatter reads
	// the LIVE atlas, so an edit removes the canopy on the next frame and there is no
	// invalidation code anywhere, exactly as with grass.
	vec3 anchor = t.base + vec3(0.0, t.height * 0.33, 0.0);
	if (world_sdf(anchor) > 0.0) return;
	ivec3 anchor_brick = ivec3(floor(anchor / BRICK_SIZE));
	int anchor_slot = slot_at(anchor_brick);
	if (anchor_slot < 0) return;
	if (material_at(anchor, anchor_brick, anchor_slot) != MAT_BARK) return;

	int budget = int(leaf.limits.z);
	// Linear thinning to 1/8, matching ve::leaf_clump_budget exactly. The CPU function is
	// the pinned one (extension/tests/test_leaf_layout.cpp); this must not drift from it.
	float f = 1.0 - 0.875 * clamp(dist / max(leaf.cam.w, 1e-3), 0.0, 1.0);
	budget = max(1, int(round(float(budget) * f)));

	uint slot = atomicAdd(counters.tree_count, 1u);
	if (slot >= uint(leaf.limits.y)) {
		atomicMin(counters.tree_count, uint(leaf.limits.y));
		return;
	}
	LeafTree out_t;
	out_t.a = vec4(t.crown, t.crown_r);
	out_t.b = vec4(t.base.y, uintBitsToFloat(t.hash), dist, float(budget));
	tree_list.v[slot] = out_t;

	// One workgroup per surviving tree; stage 2's local_size_x is 128, one thread per
	// candidate clump, so a tree never needs a second group.
	atomicMax(disp.x, slot + 1u);
}
