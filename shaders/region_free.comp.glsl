#[compute]
#version 460

#include "common.glslh"

layout(local_size_x = 256) in;

layout(set = 0, binding = 0, std430) buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 1, std430) buffer FreeList { int slot[]; } free_list;
layout(set = 0, binding = 2, std430) buffer Counters {
	int free_count; int pad0, pad1, pad2;
} counters;
// Mirror of brick_mark.comp.glsl's per-region slot tally. An eviction returns the whole
// region, so the count goes to zero rather than being decremented brick by brick.
layout(set = 0, binding = 3, std430) buffer RegionSlotCounts { int n[]; } region_counts;

layout(push_constant, std430) uniform Push {
	ivec4 cfg; // x = region slot
} pc;

// Eviction only ever frees, so it needs no phase split.
void main() {
	int i = int(gl_GlobalInvocationID.x);
	// A plain store, not an atomic: nothing else in this dispatch touches the tally, and the
	// streamer barriers between the free pass and any mark that could.
	if (i == 0) region_counts.n[pc.cfg.x] = 0;
	if (i >= REGION_BRICK_COUNT) return;
	int idx = pc.cfg.x * REGION_BRICK_COUNT + i;
	int slot = region_tables.slot[idx];
	if (slot < 0) return;
	region_tables.slot[idx] = -1;
	int k = atomicAdd(counters.free_count, 1);
	free_list.slot[k] = slot;
}
