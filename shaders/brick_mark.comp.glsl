#[compute]
#version 460

#define FIELD_OP_POOL_BINDING 4
#define FIELD_VOLUME_SDF_BINDING 7
#define FIELD_VOLUME_MAT_BINDING 8
#include "common.glslh"
#include "field.glslh"

layout(local_size_x = 256) in;

layout(set = 0, binding = 0, std430) buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 1, std430) buffer FreeList { int slot[]; } free_list;
layout(set = 0, binding = 2, std430) buffer Counters {
	int free_count; int pad0, pad1, pad2;
} counters;
layout(set = 0, binding = 3, std430) buffer Frame {
	int job_count; uint overflow; uint pad0, pad1;
} frame;
// binding 4 is the field op pool, declared by field.glslh
layout(set = 0, binding = 5, std430) writeonly buffer Jobs { ivec4 v[]; } jobs;
// Atlas slots currently held by each region slot. The streamer pays for a stream-in by
// evicting residents, and until this existed it had no way to know what an eviction gives
// back — most far regions are pure air and hold nothing, so a distance-picked eviction
// funded nothing and the free list slid to zero (see WorldStreamer::run_frame).
layout(set = 0, binding = 6, std430) buffer RegionSlotCounts { int n[]; } region_counts;
// Spec §5's occupancy grid, two bits per brick, indexed by REGION SLOT exactly as the region
// tables are. The streamer reads a region's 8 KB block back asynchronously and folds it into
// ve::OccupancyGrid at the region's COORDINATE, so eviction can recycle the slot freely.
//
// Byte layout: cell i occupies bits (i & 15) * 2 of word (i >> 4), which on little-endian
// memory is exactly ve::OccupancyGrid's "cell i in byte i >> 2, shift (i & 3) * 2".
layout(set = 0, binding = 9, std430) buffer RegionOccupancy { uint w[]; } occupancy;
const int OCC_WORDS_PER_REGION = REGION_BRICK_COUNT / 16; // 2048

const uint CELL_AIR = 1u;
const uint CELL_SOLID = 2u;
const uint CELL_FULL = 3u;

void write_occupancy(int rslot, int bi, uint state) {
	int word = rslot * OCC_WORDS_PER_REGION + (bi >> 4);
	uint shift = (uint(bi) & 15u) * 2u;
	// Two atomics rather than one CAS loop: nothing on the GPU ever READS this buffer, and
	// no two threads in a dispatch touch the same cell, so the only requirement is that the
	// other fifteen cells sharing the word survive.
	atomicAnd(occupancy.w[word], ~(3u << shift));
	atomicOr(occupancy.w[word], (state & 3u) << shift);
}

layout(push_constant, std430) uniform Push {
	ivec4 region; // xyz = global region coord (may be negative), w = region slot
	ivec4 lo;     // inclusive global brick coord of the range to scan
	ivec4 hi;     // inclusive
	ivec4 cfg;    // x = op count, y = phase (0 release, 1 allocate), z = max jobs, w = force
} pc;

// ve::kActivationPad. The probe samples every 8 voxels, so the field can dip across zero
// between samples; a brick counts as empty only when all 27 probes clear zero by this much.
const float ACTIVATION_PAD = 0.15;

// Mirror of ve::brick_probe (extension/src/world/brick_eval.cpp).
void brick_probe(ivec3 brick, uint op_base, uint op_count, out float mn, out float mx) {
	vec3 bo = vec3(brick) * BRICK_SIZE;
	mn = 1e30;
	mx = -1e30;
	for (int sz = 0; sz < 3; sz++)
		for (int sy = 0; sy < 3; sy++)
			for (int sx = 0; sx < 3; sx++) {
				float sdf;
				uint mat;
				eval_field(bo + vec3(sx, sy, sz) * (float(BRICK_VOXELS) * 0.5 * VOXEL_SIZE),
						op_base, op_count, sdf, mat);
				mn = min(mn, sdf);
				mx = max(mx, sdf);
			}
}

void main() {
	ivec3 ext = pc.hi.xyz - pc.lo.xyz + ivec3(1);
	int total = ext.x * ext.y * ext.z;
	int i = int(gl_GlobalInvocationID.x);
	if (i >= total) return;
	ivec3 brick = pc.lo.xyz + ivec3(i % ext.x, (i / ext.x) % ext.y, i / (ext.x * ext.y));

	int rslot = pc.region.w;
	// REGION_BRICKS is 32, a power of two: `& 31` is the floor-modulo for negative brick
	// coordinates too, where GLSL's `%` would truncate towards zero and give -1 for -1.
	int bi = (brick.x & 31) + (brick.y & 31) * REGION_BRICKS +
			(brick.z & 31) * REGION_BRICKS * REGION_BRICKS;
	int idx = rslot * REGION_BRICK_COUNT + bi;
	int cur = region_tables.slot[idx];

	uint op_base = uint(rslot) * MAX_REGION_OPS;
	uint op_count = uint(pc.cfg.x);
	float probe_mn, probe_mx;
	brick_probe(brick, op_base, op_count, probe_mn, probe_mx);
	// `active` is a GLSL reserved word (M2 errata 5); this local is has_surface.
	bool has_surface = probe_mn < ACTIVATION_PAD && probe_mx > -ACTIVATION_PAD;
	// Occupancy is written in the ALLOCATE phase only: both phases scan the same range, so
	// one write per brick per mark is enough and the release phase returns early for most.
	if (pc.cfg.y == 1)
		write_occupancy(rslot, bi,
				probe_mn > 0.0 ? CELL_AIR : (probe_mx <= 0.0 ? CELL_FULL : CELL_SOLID));

	if (pc.cfg.y == 0) {
		// Release phase. Kept in its own dispatch: a push at index free_count and a pop at
		// free_count - 1 running concurrently can collide and lose or duplicate a slot.
		if (!has_surface && cur >= 0) {
			region_tables.slot[idx] = -1;
			int k = atomicAdd(counters.free_count, 1);
			free_list.slot[k] = cur;
			atomicAdd(region_counts.n[rslot], -1);
		}
		return;
	}

	if (!has_surface) return;

	int slot = cur;
	if (slot < 0) {
		int old = atomicAdd(counters.free_count, -1);
		if (old <= 0) {
			atomicAdd(counters.free_count, 1); // undo the over-decrement; never go negative
			atomicOr(frame.overflow, 1u);
			return; // fail-soft: the brick stays absent and the ray passes through it
		}
		slot = free_list.slot[old - 1];
		region_tables.slot[idx] = slot;
		atomicAdd(region_counts.n[rslot], 1);
	} else if (pc.cfg.w == 0) {
		return; // resident already and this is a plain stream-in: nothing to regenerate
	}

	int j = atomicAdd(frame.job_count, 1);
	if (j >= pc.cfg.z) {
		atomicAdd(frame.job_count, -1);
		atomicOr(frame.overflow, 2u);
		// The slot stays assigned but ungenerated for this frame. Releasing it here would
		// mean freeing during the allocate phase — the very race the phase split avoids.
		// The streamer sees overflow bit 1 (value 2, set above) and re-marks this region
		// with force_regen next frame, which re-enqueues the brick; one frame of stale
		// atlas bytes is the cost. (Bit 0, value 1, is the free-list-empty fail-soft and
		// gets no re-mark — the brick stays absent until a later edit or re-mark.)
		return;
	}
	jobs.v[j * 2 + 0] = ivec4(brick, slot);
	jobs.v[j * 2 + 1] = ivec4(rslot, int(op_count), 0, 0);
}
