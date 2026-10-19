#[compute]
#version 460
#include "generated/blocks.glslh"

// Task 3: sun visibility in the brick cache's .w, baked per voxel instead of marched per
// pixel. One workgroup per slot; threads below BRICK_CACHE_DIM^3 each own one 20 cm lattice
// point, and thread 0 publishes the validity word afterwards.
//
// The field gradient -- the cache's .xyz -- is Task 2's; this shader only ever stores the .w
// channel, carrying .xyz through unchanged.
#define SUN_LIGHT_SET 2
#define SUN_LIGHT_BINDING 24
#include "common.glslh"
#include "brick_layout.glslh"
#include "brick_cache.glslh"
#include "sun_light.glslh"

// brick_atlas.glslh's contract: declare these before including it. mat_atlas and palette_buf
// serve material_at(), which this shader never calls but the shared header defines.
layout(set = 0, binding = 0) uniform sampler3D sdf_atlas;   // LINEAR + clamp, as the marcher's
layout(set = 0, binding = 1) uniform usampler3D mat_atlas;
layout(set = 0, binding = 2, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 3, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 4, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 5, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;
layout(set = 0, binding = 6, std430) readonly buffer RegionSlotCounts { int n[]; } region_slot_counts;
layout(set = 0, binding = 7, std430) readonly buffer RegionBounds { ivec4 v[]; } region_bounds;
// Read-write, not writeonly: the sun entry is stored into the texel the baked normal already
// lives in, so the bake has to read that texel first.
layout(set = 0, binding = 8, rgba16_snorm) uniform image3D brick_cache;
layout(set = 0, binding = 9, std430) readonly buffer CacheNormalOk { uint v[]; } cache_normal_ok;
layout(set = 0, binding = 10, std430) buffer CacheSunOk { uint v[]; } cache_sun_ok;
layout(set = 0, binding = 11, std430) readonly buffer Jobs { ivec4 v[]; } jobs;
// Task 3's slot -> brick map (GpuAtlas::cache_brick), written by brick_mark at allocation.
layout(set = 0, binding = 12, std430) readonly buffer CacheBrick { ivec4 v[]; } cache_brick;

layout(push_constant, std430) uniform Push { SUN_BAKE_PUSH_FIELDS } pc;

#define SUN_MARCH_REGION_BOUNDS
#include "brick_atlas.glslh"
#include "sun_march.glslh"

layout(local_size_x = 128) in;

const int kCacheSamples = BRICK_CACHE_DIM * BRICK_CACHE_DIM * BRICK_CACHE_DIM;

// The marcher's own 6 cm offset along the normal: less and the shadow ray self-shadows on the
// surface it starts from, more and thin ledges stop casting.
const float SUN_BAKE_BIAS = 0.06;

void main() {
	// Mode 0 is this frame's generation jobs: brick_gen runs one workgroup per job, and the
	// job carries the slot. Mode 1 is the rolling sweep, one workgroup per slot in range.
	int slot;
	if (pc.cfg.x == 0) {
		ivec4 job = jobs.v[int(gl_WorkGroupID.x) * 2];
		if (job.w < 0) return;
		slot = job.w;
	} else {
		slot = pc.cfg.y + int(gl_WorkGroupID.x);
		if (slot >= pc.cfg.y + pc.cfg.z) return;
	}
	// Empty air and free slots are the great majority of a sweep, and the flag word is one
	// load. Skipping them here is what keeps the sweep cheap. The test is workgroup-uniform,
	// so the barrier below is never reached by only part of a workgroup.
	if ((brick_flag_word(slot) & BRICK_FLAG_HAS_SURFACE) == 0u) return;

	// Both modes read the brick from the allocation map: in a sweep there is nothing else to
	// read it from, and in a job it is the same value brick_mark wrote for that slot.
	const ivec3 brick = cache_brick.v[slot].xyz;
	const vec3 brick_lo = vec3(brick) * BRICK_SIZE;
	uint tid = gl_LocalInvocationID.x;

	if (tid < uint(kCacheSamples)) {
		const ivec3 l = ivec3(int(tid) % BRICK_CACHE_DIM,
				(int(tid) / BRICK_CACHE_DIM) % BRICK_CACHE_DIM,
				int(tid) / (BRICK_CACHE_DIM * BRICK_CACHE_DIM));
		const vec3 p = brick_lo + vec3(l) * (float(BRICK_CACHE_STEP) * VOXEL_SIZE);
		const ivec3 texel = brick_cache_texel(slot, l, pc.atlas_bricks.xyz);
		const vec4 entry = imageLoad(brick_cache, texel);
		// This slot's own atlas block: the same fetch the marcher does at a hit, so the bake
		// shades a point the marcher can land on.
		const float d = brick_sdf(slot, vec3(l) * float(BRICK_CACHE_STEP));
		float sun = 1.0;
		// Saturated means no surface within SDF_RANGE on this lattice point: open sky.
		if (d < SDF_RANGE * 0.99) {
			vec3 n = entry.xyz;
			// A brick whose normal bake was a miss can still hold a surface (thin shells,
			// liquids, transparent materials), and its sunrise still has to be right.
			if (cache_normal_ok.v[slot] != CACHE_OK || length(n) < 1e-6) {
				const float e = VOXEL_SIZE;
				n = vec3(
					world_sdf(p + vec3(e, 0.0, 0.0)) - world_sdf(p - vec3(e, 0.0, 0.0)),
					world_sdf(p + vec3(0.0, e, 0.0)) - world_sdf(p - vec3(0.0, e, 0.0)),
					world_sdf(p + vec3(0.0, 0.0, e)) - world_sdf(p - vec3(0.0, 0.0, e)));
			}
			const float len = length(n);
			n = len > 1e-6 ? n / len : vec3(0.0, 1.0, 0.0);
			// The lattice point sits on the narrow-band distance field, not on the surface:
			// project it along the gradient, then start the shadow ray just outside it.
			const vec3 ps = p - n * d;
			sun = terrain_sun_visibility(ps + n * SUN_BAKE_BIAS, RAY_SHADOW_DIST);
		}
		imageStore(brick_cache, texel, vec4(entry.xyz, sun));
	}
	barrier();
	if (tid == 0u) cache_sun_ok.v[slot] = CACHE_OK;
}
