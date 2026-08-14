#[compute]
#version 460

#include "common.glsl"
#include "brick_layout.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) writeonly uniform image2D out_color;
layout(set = 0, binding = 1, rgba32f) writeonly uniform image2D out_hitpos;
layout(set = 0, binding = 2) uniform sampler3D sdf_atlas;   // R8 unorm, nearest
layout(set = 0, binding = 3) uniform usampler3D mat_atlas;  // R8 uint, nearest
layout(set = 0, binding = 4, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 5, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 6, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 7, std430) readonly buffer OpPool { uvec4 v[]; } op_pool;
layout(set = 0, binding = 8, std430) readonly buffer OpCounts { int n[]; } op_counts;
// The pending-edit visualizer: tint the atlas content an edit WILL change, so the player
// gets one frame of feedback before the regenerated bricks land (spec §5 latency).
layout(set = 0, binding = 9) uniform Edits { vec4 center; vec4 params; } edits;

layout(push_constant, std430) uniform Push {
	vec4 cam_pos;
	vec4 cam_right;
	vec4 cam_up;
	vec4 cam_fwd;
	vec4 params;          // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	ivec4 dims;           // world size in REGIONS
	ivec4 region_origin;  // world origin in REGIONS
	ivec4 atlas_bricks;   // atlas grid
} pc;

// Region-table slot for the region holding a GLOBAL brick coord; -1 outside the world or
// not resident. `>> 5` is an arithmetic shift: floor(b / 32), correct for negatives.
int region_slot_of(ivec3 brick) {
	ivec3 r = brick >> 5;
	ivec3 l = r - pc.region_origin.xyz;
	if (any(lessThan(l, ivec3(0))) || any(greaterThanEqual(l, pc.dims.xyz))) return -1;
	return region_map.slot[l.x + l.y * pc.dims.x + l.z * pc.dims.x * pc.dims.y];
}

// Atlas slot of a global brick; -1 when absent. `& 31` is the floor-mod for negatives.
int slot_at(ivec3 brick) {
	int rs = region_slot_of(brick);
	if (rs < 0) return -1;
	int bi = (brick.x & 31) + (brick.y & 31) * REGION_BRICKS +
			(brick.z & 31) * REGION_BRICKS * REGION_BRICKS;
	return region_tables.slot[rs * REGION_BRICK_COUNT + bi];
}

// Manual trilinear inside one brick (unchanged from M1 except the runtime atlas base).
float brick_sdf(int slot, vec3 local) { // local in voxel units [0, 16]
	vec3 p = clamp(local, vec3(0.0), vec3(BRICK_SDF_MAX));
	ivec3 i0 = ivec3(floor(p));
	vec3 f = p - vec3(i0);
	ivec3 i1 = min(i0 + 1, ivec3(BRICK_VOXELS));
	ivec3 base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_SDF_STRIDE);
	float c000 = texelFetch(sdf_atlas, base + ivec3(i0.x, i0.y, i0.z), 0).r;
	float c100 = texelFetch(sdf_atlas, base + ivec3(i1.x, i0.y, i0.z), 0).r;
	float c010 = texelFetch(sdf_atlas, base + ivec3(i0.x, i1.y, i0.z), 0).r;
	float c110 = texelFetch(sdf_atlas, base + ivec3(i1.x, i1.y, i0.z), 0).r;
	float c001 = texelFetch(sdf_atlas, base + ivec3(i0.x, i0.y, i1.z), 0).r;
	float c101 = texelFetch(sdf_atlas, base + ivec3(i1.x, i0.y, i1.z), 0).r;
	float c011 = texelFetch(sdf_atlas, base + ivec3(i0.x, i1.y, i1.z), 0).r;
	float c111 = texelFetch(sdf_atlas, base + ivec3(i1.x, i1.y, i1.z), 0).r;
	float v = mix(mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y),
	              mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y), f.z);
	return decode_sdf(v);
}

float world_sdf(vec3 p) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = slot_at(brick);
	if (slot < 0) return SDF_RANGE;
	vec3 local = (p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE;
	return brick_sdf(slot, local);
}

// Gradient taps can land in a brick with NO atlas slot; fall back to the anchor brick,
// whose apron covers the shared face exactly (M1 fix, semantics unchanged).
float sdf_near(vec3 p, ivec3 anchor, int anchor_slot) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = all(equal(brick, anchor)) ? anchor_slot : slot_at(brick);
	if (slot < 0) { brick = anchor; slot = anchor_slot; }
	return brick_sdf(slot, (p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE);
}

vec3 calc_normal(vec3 p, ivec3 anchor, int anchor_slot) {
	const float e = 0.01;
	return normalize(vec3(
		sdf_near(p + vec3(e, 0, 0), anchor, anchor_slot) - sdf_near(p - vec3(e, 0, 0), anchor, anchor_slot),
		sdf_near(p + vec3(0, e, 0), anchor, anchor_slot) - sdf_near(p - vec3(0, e, 0), anchor, anchor_slot),
		sdf_near(p + vec3(0, 0, e), anchor, anchor_slot) - sdf_near(p - vec3(0, 0, e), anchor, anchor_slot)));
}

// Material of the surface crossing inside `brick`, anchored so a hit point that rounds
// into a neighbouring (possibly absent or empty-palette) brick cannot resolve magenta.
uint material_at(vec3 p, ivec3 brick, int slot) {
	vec3 local = clamp((p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE, vec3(0.0), vec3(15.0));
	ivec3 base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_VOXELS);
	uint idx = texelFetch(mat_atlas, base + ivec3(local), 0).r;
	return palette_buf.ids[slot * 4 + idx];
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_color);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

	vec3 ro = pc.cam_pos.xyz;
	vec3 rd = normalize(pc.cam_fwd.xyz
		+ pc.cam_right.xyz * ndc.x * pc.params.x
		+ pc.cam_up.xyz * ndc.y * pc.params.y);
	float max_dist = pc.params.z;

	vec3 color = sky_color(rd);
	vec4 hitpos = vec4(0.0);

	// Brick-grid DDA over the GLOBAL brick lattice (negative coords supported). side[d] =
	// ray t at the next boundary along axis d; the st==0 guards keep NaNs out (M1 errata 4).
	ivec3 map = ivec3(floor(ro / BRICK_SIZE));
	vec3 delta = abs(vec3(BRICK_SIZE) / rd);
	ivec3 st = ivec3(sign(rd));
	vec3 side = (vec3(map) * BRICK_SIZE - ro
	             + (vec3(st) * 0.5 + 0.5) * BRICK_SIZE) / rd;
	if (st.x == 0) side.x = 1.0 / 0.0;
	if (st.y == 0) side.y = 1.0 / 0.0;
	if (st.z == 0) side.z = 1.0 / 0.0;
	float t_prev = 0.0;

	bool hit = false;
	vec3 hp = vec3(0.0);
	for (int i = 0; i < 1024; i++) {
		float t_exit = min(side.x, min(side.y, side.z));
		if (t_exit > max_dist) break;

		int slot = slot_at(map);
		if (slot >= 0) {
			// Air-margin bricks (activated by the probe pad) hold no solid voxels and an
			// EMPTY palette; their interpolated field can still dip below the hit threshold
			// near a brick face. That is not a renderable surface — skip it (M1 fix).
			bool has_material = palette_buf.ids[slot * 4] != 0u;
			float t = t_prev;
			for (int j = 0; j < 64; j++) {
				if (t > t_exit) break;
				vec3 p = ro + rd * t;
				float d = world_sdf(p);
				if (d < 0.002 && has_material) {
					for (int k = 0; k < 4; k++) { // secant refinement
						float dk = world_sdf(p);
						t += dk * 0.5;
						p = ro + rd * t;
					}
					vec3 n = calc_normal(p, map, slot);
					vec3 alb = material_albedo(material_at(p, map, slot));
					vec3 sun = normalize(vec3(0.6, 0.8, 0.3));
					float lam = max(dot(n, sun), 0.0);
					color = alb * (0.25 + 0.75 * lam);
					hp = p;
					hitpos = vec4(p, 1.0);
					hit = true;
					break;
				}
				t += max(d * 0.9, 0.005);
			}
			if (hit) break;
		}

		if (side.x < side.y && side.x < side.z) { t_prev = side.x; side.x += delta.x; map.x += st.x; }
		else if (side.y < side.z)               { t_prev = side.y; side.y += delta.y; map.y += st.y; }
		else                                    { t_prev = side.z; side.z += delta.z; map.z += st.z; }
	}

	// Pending-edit visualizer: the hit inside the edit sphere is tinted towards the tool's
	// colour. It reads the OLD atlas content by design — this is the preview, and the
	// regenerated bricks replace it next frame.
	if (hit && edits.params.x > 0.0 && length(hp - edits.center.xyz) < edits.params.x) {
		uint t = uint(edits.params.y);
		vec3 tint = t == 0u ? vec3(1.0, 0.55, 0.1)      // subtract: warning orange
		          : t == 1u ? material_albedo(4u)        // add: the fill grey
		          : material_albedo(uint(edits.params.z)); // paint: the target colour
		color = mix(color, tint, 0.45);
	}

	imageStore(out_color, px, vec4(color, 1.0));
	imageStore(out_hitpos, px, hitpos);
}
