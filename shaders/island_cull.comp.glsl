#[compute]
#version 460

#include "common.glslh"

// One thread per 16x16 screen tile (spec §3). The tile grid is small -- 1440p at 0.66x is
// 95 x 54 tiles -- so a thread per tile testing 32 AABBs is ~160k corner projections, well
// under 0.05 ms.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer IslandDesc { vec4 v[]; } island_desc;
layout(set = 0, binding = 1, std430) writeonly buffer TileMask { uint v[]; } tile_mask;

// ve::CameraParams, byte for byte -- the SAME struct the raymarcher takes. Sharing it is
// what guarantees the two agree about where a world point lands on screen: there is one
// projection, written once, below.
layout(push_constant, std430) uniform Push {
	vec4 cam_pos;
	vec4 cam_right;
	vec4 cam_up;
	vec4 cam_fwd;
	vec4 params;          // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	ivec4 dims;           // w = live island count
	ivec4 region_origin;  // w = tiles per row
	ivec4 atlas_bricks;   // w = tile rows
} pc;

const int TILE = 16; // kIslandTileSize

void main() {
	ivec2 tile = ivec2(gl_GlobalInvocationID.xy);
	int tiles_x = pc.region_origin.w;
	int tiles_y = pc.atlas_bricks.w;
	if (tile.x >= tiles_x || tile.y >= tiles_y) return;
	int count = min(pc.dims.w, 32);

	// The tile's NDC rectangle. The raymarcher builds a pixel's ray as
	// normalize(fwd + right * ndc.x * tan_x + up * ndc.y * tan_y) with
	// ndc = ((px + 0.5) / size * 2 - 1, 1 - (py + 0.5) / size * 2), so a world point at
	// (dot(v, right), dot(v, up), dot(v, fwd)) = (x, y, z) lands at ndc = (x / (z * tan_x),
	// y / (z * tan_y)). The tile spans whole pixels, so its NDC bounds come from its corner
	// pixel edges -- inclusive on both sides, which is the conservative direction.
	vec2 size = vec2(float(tiles_x * TILE), float(tiles_y * TILE));
	vec2 lo_px = vec2(tile) * float(TILE);
	vec2 hi_px = lo_px + float(TILE);
	vec2 ndc_lo = vec2(lo_px.x / size.x * 2.0 - 1.0, 1.0 - hi_px.y / size.y * 2.0);
	vec2 ndc_hi = vec2(hi_px.x / size.x * 2.0 - 1.0, 1.0 - lo_px.y / size.y * 2.0);

	uint mask = 0u;
	for (int i = 0; i < count; i++) {
		int dim = floatBitsToInt(island_desc.v[i * 8 + 4].x);
		if (dim < 2) continue; // dead slot
		vec3 lo = island_desc.v[i * 8 + 5].xyz;
		vec3 hi = island_desc.v[i * 8 + 6].xyz;

		vec2 smin = vec2(1e30), smax = vec2(-1e30);
		bool near_clip = false;
		float zmin = 1e30;
		float zmax = -1e30;
		for (int c = 0; c < 8; c++) {
			vec3 p = vec3((c & 1) != 0 ? hi.x : lo.x, (c & 2) != 0 ? hi.y : lo.y,
					(c & 4) != 0 ? hi.z : lo.z);
			vec3 v = p - pc.cam_pos.xyz;
			float z = dot(v, pc.cam_fwd.xyz);
			zmin = min(zmin, z);
			zmax = max(zmax, z);
			// A corner at or behind the eye plane projects to nonsense. Fail SAFE: when the
			// AABB straddles the eye plane (the camera is inside/overlapping it) the island
			// is marked everywhere rather than culled away, which costs a march and cannot
			// make it disappear. An AABB entirely behind the eye is skipped after the loop;
			// degenerate fov (the 1x1 debug probes) fails safe like a straddle.
			if (z < 0.01 || pc.params.x <= 0.0 || pc.params.y <= 0.0) {
				near_clip = true;
				break;
			}
			vec2 s = vec2(dot(v, pc.cam_right.xyz) / (z * pc.params.x),
					dot(v, pc.cam_up.xyz) / (z * pc.params.y));
			smin = min(smin, s);
			smax = max(smax, s);
		}
		if (zmax < 0.01) continue; // entirely behind the eye plane: never visible
		if (near_clip) {
			mask |= 1u << uint(i);
			continue;
		}
		if (zmin > pc.params.z) continue; // entirely past the march's reach
		if (smax.x < ndc_lo.x || smin.x > ndc_hi.x) continue;
		if (smax.y < ndc_lo.y || smin.y > ndc_hi.y) continue;
		mask |= 1u << uint(i);
	}
	tile_mask.v[tile.y * tiles_x + tile.x] = mask;
}
