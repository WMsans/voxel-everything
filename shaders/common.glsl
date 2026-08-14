// Shared constants + helpers, pulled into raymarch.comp.glsl by ve::load_shader_source.
// NOTE: never put a literal include directive inside a comment in this file — the naive
// loader matches include tokens anywhere in a line and would self-include (cycle error).
const int BRICK_VOXELS = 16;
const float VOXEL_SIZE = 0.05;
const float BRICK_SIZE = 0.8;         // 16 * 0.05
const float SDF_RANGE = 0.64;         // uint8 unorm <-> [-0.64, 0.64]
const ivec3 ATLAS_BRICKS = ivec3(32, 16, 32);
// The SDF atlas stores a 17^3 LATTICE per brick: sample n sits at local coordinate n, and
// the extra plane at 16 is a one-voxel apron copied from the neighbour's origin plane, so
// trilinear reconstruction covers the brick's whole [0,16) extent. Without it the last
// slab clamps to a constant, the gradient collapses, and shading seams appear on every
// brick face. The material atlas is a plain 16^3 cell grid (nearest filtered, no apron).
const int BRICK_SDF_STRIDE = BRICK_VOXELS + 1; // 17
const float BRICK_SDF_MAX = float(BRICK_VOXELS); // last valid lattice coordinate

float decode_sdf(float unorm) { return unorm * 2.0 * SDF_RANGE - SDF_RANGE; }

vec3 material_albedo(uint mat_id) {
	switch (mat_id) {
		case 1: return vec3(0.36, 0.55, 0.22); // grass
		case 2: return vec3(0.45, 0.42, 0.40); // rock
		case 3: return vec3(0.50, 0.35, 0.20); // dirt
		default: return vec3(1.0, 0.0, 1.0);   // error magenta
	}
}

vec3 sky_color(vec3 dir) {
	float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
	return mix(vec3(0.55, 0.45, 0.35), vec3(0.25, 0.45, 0.85), t);
}
