#[compute]
#version 460

#include "common.glslh"
#include "brick_layout.glslh"
#include "conifer_pass.glslh"

// One thread per cell of the camera-local lattice out to the imposter reach. Culls on
// distance, the grove gate and a column frustum test BEFORE reading any ground; then the
// ground gate, the chop check, and an append to the card list, the imposter list or both
// (the fade band). Spec §5.1.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std140) uniform Params { CONIFER_PASS_FIELDS } conifer;
layout(set = 0, binding = 1, std430) writeonly buffer CardList { ConiferRecord v[]; } card_list;
layout(set = 0, binding = 2, std430) writeonly buffer ImpostorList { ConiferRecord v[]; } impostor_list;
layout(set = 0, binding = 3, std430) buffer Counters { uint card_count; uint impostor_count;
		uint clump_count; uint high_water; } counters;
layout(set = 0, binding = 4, std430) buffer DispatchArgs { uint x; uint y; uint z; } disp;
layout(set = 0, binding = 5, std430) buffer ImpostorArgs { uint vertex_count; uint instance_count;
		uint first_vertex; uint first_instance; } impostor_args;
layout(set = 0, binding = 6, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 7, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 8, std430) readonly buffer RegionSlotCounts { int n[]; } region_slot_counts;
layout(set = 0, binding = 9) uniform sampler3D sdf_atlas;
layout(set = 0, binding = 10) uniform usampler3D mat_atlas;
// brick_atlas.glslh addresses the window through `pc`, as in leaf_trees.comp.glsl.
layout(set = 0, binding = 11, std140) uniform Region { LEAF_REGION_FIELDS } pc;
layout(set = 0, binding = 12, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 13, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;

// The terrain pipeline's generated field source: conifers_params(), conifer_ground() and all
// of shaders/conifer.glslh, reading set 1's P exactly as the conifers stage does.
#define FIELD_OP_POOL_BINDING 14
#include "field.glslh"
#include "brick_atlas.glslh"

#ifdef CONIFERS_STAGE

// The tallest column a tree in this cell could occupy: the whole encoded height range plus a
// tree, CONIFER_R_MAX x tooth wide. Tested against the frustum before the ground is read.
bool conifer_column_culled(vec2 wxz, ConiferParams cp) {
	float lo = cp.water_y - SECTOR_HEIGHT_BELOW_M;
	float hi = lo + SECTOR_HEIGHT_SPAN_M + cp.height_max;
	vec3 c = vec3(wxz.x, 0.5f * (lo + hi), wxz.y);
	vec3 e = vec3(cp.crown_radius * CONIFER_R_MAX * CONIFER_TOOTH_MAX, 0.5f * (hi - lo),
			cp.crown_radius * CONIFER_R_MAX * CONIFER_TOOTH_MAX);
	for (int i = 0; i < 6; i++) {
		vec3 n = vec3(conifer.planes[i].x, conifer.planes[i].y, conifer.planes[i].z);
		if (dot(n, c) + conifer.planes[i].w < -dot(abs(n), e)) return true;
	}
	return false;
}

bool conifer_sphere_culled(vec3 c, float r) {
	for (int i = 0; i < 6; i++) {
		vec3 n = vec3(conifer.planes[i].x, conifer.planes[i].y, conifer.planes[i].z);
		if (dot(n, c) + conifer.planes[i].w < -r) return true;
	}
	return false;
}

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= uint(conifer.cell_min.w)) return;
	ivec2 cell = ivec2(conifer.cell_min.x + int(idx % uint(conifer.cell_dim.x)),
	                   conifer.cell_min.z + int(idx / uint(conifer.cell_dim.x)));
	ConiferParams cp = conifers_params();
	vec2 xz = conifer_cell_xz(cell, cp);
	// The lattice is SHIFTED space; the camera is world.
	vec2 wxz = xz - vec2(VE_FIELD_OFFSET.x, VE_FIELD_OFFSET.z);
	vec2 dxz = wxz - vec2(conifer.cam.x, conifer.cam.z);
	// A crown's half height and more.
	float slack = cp.height_max;
	float far = conifer.reach.x + slack;
	if (dot(dxz, dxz) > far * far) return;
	if (!conifer_cell_gate(cell, cp)) return;
	if (conifer_column_culled(wxz, cp)) return;

	vec4 g = conifer_ground(xz);
	float slope = length(vec2(g.y, g.z));
	if (!conifer_ground_ok(cp.surface_y + g.x - cp.water_y, slope, cp)) return;
	Conifer c = conifer_at(cell, xz, cp.surface_y + g.x, slope, cp);
	c.foot -= VE_FIELD_OFFSET;

	float half_h = 0.5f * c.height;
	vec3 centre = c.foot + vec3(0.0f, half_h, 0.0f);
	if (conifer_sphere_culled(centre, half_h)) return;
	vec3 cam = vec3(conifer.cam.x, conifer.cam.y, conifer.cam.z);
	float dist = length(centre - cam);
	float card_reach = conifer.cam.w;
	bool card = card_reach > 0.0f && dist < card_reach + half_h;
	bool impostor = dist > 0.8f * card_reach - half_h && dist < conifer.reach.x + half_h;
	if (!card && !impostor) return;

	// THE chop check, verbatim from leaf_trees.comp.glsl: one read a third of the way up the
	// trunk, only where the atlas holds the region (beyond it the analytic tree is the answer).
	// It gates BOTH lists, so a felled tree leaves no imposter behind.
	vec3 anchor = c.foot + vec3(0.0f, c.height * 0.33f, 0.0f);
	ivec3 anchor_brick = ivec3(floor(anchor / BRICK_SIZE));
	if (region_slot_of(anchor_brick) >= 0) {
		if (world_sdf(anchor) > 0.0f) return;
		int anchor_slot = slot_at(anchor_brick);
		if (anchor_slot < 0) return;
		if (material_at(anchor, anchor_brick, anchor_slot) != MAT_BARK) return;
	}

	ConiferRecord r;
	r.a = conifer_pack_a(c);
	r.b = conifer_pack_b(c);
	if (card) {
		uint slot = atomicAdd(counters.card_count, 1u);
		if (slot < uint(conifer.limits.y)) {
			card_list.v[slot] = r;
			// One card-scatter workgroup per listed tree.
			atomicMax(disp.x, slot + 1u);
		} else {
			atomicMin(counters.card_count, uint(conifer.limits.y));
		}
	}
	if (impostor) {
		uint slot = atomicAdd(counters.impostor_count, 1u);
		if (slot < uint(conifer.limits.w)) {
			impostor_list.v[slot] = r;
			atomicMax(impostor_args.vertex_count, (slot + 1u) * 6u);
			impostor_args.instance_count = 1u;
		} else {
			atomicMin(counters.impostor_count, uint(conifer.limits.w));
		}
	}
}

#else

// This world's pipeline has no conifers stage (plan deviation 12). The pass is never created
// here; this body exists so the shader-reload preflight compiles.
void main() {}

#endif
