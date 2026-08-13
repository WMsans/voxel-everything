#[compute]
#version 460

#include "common.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) writeonly uniform image2D out_color;
layout(set = 0, binding = 1, rgba32f) writeonly uniform image2D out_hitpos;
layout(set = 0, binding = 2) uniform sampler3D sdf_atlas;    // R8 unorm, nearest
layout(set = 0, binding = 3) uniform usampler3D mat_atlas;   // R8 uint, nearest
layout(set = 0, binding = 4) uniform isampler3D indirection; // R32 sint, world dims, nearest
layout(set = 0, binding = 5, std430) readonly buffer Palette { uint ids[]; } palette_buf;

layout(push_constant, std430) uniform Push {
	vec4 cam_pos;
	vec4 cam_right;
	vec4 cam_up;
	vec4 cam_fwd;
	vec4 params;  // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	ivec4 dims;   // world dims in bricks
} pc;

// Manual trilinear inside one brick (adjacent bricks are unrelated: never filter across).
float brick_sdf(int slot, vec3 local) { // local in voxel units [0, 16)
	vec3 p = clamp(local, vec3(0.0), vec3(15.0));
	ivec3 i0 = ivec3(floor(p));
	vec3 f = p - vec3(i0);
	ivec3 i1 = min(i0 + 1, ivec3(15));
	ivec3 base = ivec3(slot % ATLAS_BRICKS.x,
	                   (slot / ATLAS_BRICKS.x) % ATLAS_BRICKS.y,
	                   slot / (ATLAS_BRICKS.x * ATLAS_BRICKS.y)) * BRICK_VOXELS;
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

int slot_at(ivec3 brick) {
	if (any(lessThan(brick, ivec3(0))) || any(greaterThanEqual(brick, pc.dims.xyz))) return -1;
	return texelFetch(indirection, brick, 0).r;
}

float world_sdf(vec3 p) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = slot_at(brick);
	if (slot < 0) return SDF_RANGE; // empty: caller stays within its brick interval
	vec3 local = (p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE;
	return brick_sdf(slot, local);
}

vec3 calc_normal(vec3 p) {
	const float e = 0.01;
	return normalize(vec3(
		world_sdf(p + vec3(e, 0, 0)) - world_sdf(p - vec3(e, 0, 0)),
		world_sdf(p + vec3(0, e, 0)) - world_sdf(p - vec3(0, e, 0)),
		world_sdf(p + vec3(0, 0, e)) - world_sdf(p - vec3(0, 0, e))));
}

uint material_at(vec3 p) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = slot_at(brick);
	if (slot < 0) return 0u;
	vec3 local = clamp((p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE, vec3(0.0), vec3(15.0));
	ivec3 base = ivec3(slot % ATLAS_BRICKS.x,
	                   (slot / ATLAS_BRICKS.x) % ATLAS_BRICKS.y,
	                   slot / (ATLAS_BRICKS.x * ATLAS_BRICKS.y)) * BRICK_VOXELS;
	uint idx = texelFetch(mat_atlas, base + ivec3(local), 0).r;
	return palette_buf.ids[slot * 4 + idx];
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_color);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	// image row 0 = screen top = +up direction
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

	vec3 ro = pc.cam_pos.xyz;
	vec3 rd = normalize(pc.cam_fwd.xyz
		+ pc.cam_right.xyz * ndc.x * pc.params.x
		+ pc.cam_up.xyz * ndc.y * pc.params.y);
	float max_dist = pc.params.z;

	vec3 color = sky_color(rd);
	vec4 hitpos = vec4(0.0);

	// Brick-grid DDA. side[d] = ray t at the next boundary along axis d.
	ivec3 map = ivec3(floor(ro / BRICK_SIZE));
	vec3 delta = abs(vec3(BRICK_SIZE) / rd);
	ivec3 st = ivec3(sign(rd));
	// For st==0 (rd component == 0) the axis must ALWAYS be +inf: the plain formula's
	// numerator there is origin-dependent (+inf/-inf/NaN across the cell, NaN at its
	// center), and -inf winning min() livelocks the DDA. This glslang build provides no
	// select() overload, and mix() would arithmetically combine the operands (inf*0 =
	// NaN), so guard each axis explicitly: the st==0 axes are overwritten with exactly
	// 1/0 = +inf, discarding the formula's garbage value (no NaN contamination).
	vec3 side = (vec3(map) * BRICK_SIZE - ro
	             + (vec3(st) * 0.5 + 0.5) * BRICK_SIZE) / rd;
	if (st.x == 0) side.x = 1.0 / 0.0;
	if (st.y == 0) side.y = 1.0 / 0.0;
	if (st.z == 0) side.z = 1.0 / 0.0;
	float t_prev = 0.0; // entry t of the current cell (last boundary crossed)

	bool hit = false;
	for (int i = 0; i < 512; i++) {
		float t_exit = min(side.x, min(side.y, side.z));
		if (t_exit > max_dist) break;

		int slot = slot_at(map);
		if (slot >= 0) {
			float t = t_prev;
			for (int j = 0; j < 64; j++) {
				vec3 p = ro + rd * t;
				float d = world_sdf(p);
				if (d < 0.002) {
					for (int k = 0; k < 4; k++) { // secant refinement
						float dk = world_sdf(p);
						t += dk * 0.5;
						p = ro + rd * t;
					}
					vec3 n = calc_normal(p);
					vec3 alb = material_albedo(material_at(p));
					vec3 sun = normalize(vec3(0.6, 0.8, 0.3));
					float lam = max(dot(n, sun), 0.0);
					color = alb * (0.25 + 0.75 * lam);
					hitpos = vec4(p, 1.0);
					hit = true;
					break;
				}
				t += max(d * 0.9, 0.005);
				if (t > t_exit) break;
			}
			if (hit) break;
		}

		if (side.x < side.y && side.x < side.z) { t_prev = side.x; side.x += delta.x; map.x += st.x; }
		else if (side.y < side.z)               { t_prev = side.y; side.y += delta.y; map.y += st.y; }
		else                                    { t_prev = side.z; side.z += delta.z; map.z += st.z; }
	}

	imageStore(out_color, px, vec4(color, 1.0));
	imageStore(out_hitpos, px, hitpos);
}
