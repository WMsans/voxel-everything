// Atlas addressing, shared by the generator and the raymarcher so they can never disagree.
// Slot s occupies brick cell (s % ax, (s / ax) % ay, s / (ax * ay)); its texel origin in a
// texture whose per-brick stride is `stride` is that cell times `stride`. Strides: SDF 17,
// material 16, min-max levels 2 / 4 / 8.
#include "common.glsl"

ivec3 atlas_brick_cell(int slot, ivec3 atlas_bricks) {
	return ivec3(slot % atlas_bricks.x,
	             (slot / atlas_bricks.x) % atlas_bricks.y,
	             slot / (atlas_bricks.x * atlas_bricks.y));
}

ivec3 atlas_base(int slot, ivec3 atlas_bricks, int stride) {
	return atlas_brick_cell(slot, atlas_bricks) * stride;
}
