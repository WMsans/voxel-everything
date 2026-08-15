// Shared constants + helpers, pulled into every voxel shader by ve::load_shader_source.
// NOTE: never put a literal include directive inside a comment in this file — the loader
// matches include tokens anywhere in a line and would self-include (cycle error).
const int BRICK_VOXELS = 16;
const float VOXEL_SIZE = 0.05;
const float BRICK_SIZE = 0.8;         // 16 * 0.05
const float SDF_RANGE = 0.64;         // uint8 unorm <-> [-0.64, 0.64]
const int REGION_BRICKS = 32;
const float REGION_SIZE = 25.6;       // 32 * 0.8
const int REGION_BRICK_COUNT = 32768; // 32^3

// The SDF atlas stores a 17^3 LATTICE per brick: sample n sits at local coordinate n, and
// the extra plane at 16 is a one-voxel apron so trilinear reconstruction covers the brick's
// whole [0,16) extent. Without it the last slab clamps to a constant, the gradient collapses,
// and shading seams appear on every brick face. The material atlas is a plain 16^3 cell grid
// (nearest filtered, no apron).
const int BRICK_SDF_STRIDE = BRICK_VOXELS + 1; // 17
const int BRICK_VOXEL_COUNT = 4096;            // 16^3
const int BRICK_SDF_COUNT = 4913;              // 17^3
const float BRICK_SDF_MAX = float(BRICK_VOXELS); // last valid lattice coordinate

// A cell of the min-max chain holds no surface unless its range straddles this value.
// It is exactly ve::encode_sdf(0.0f).
const uint ENCODED_ZERO = 128u;

float decode_sdf(float unorm) { return unorm * 2.0 * SDF_RANGE - SDF_RANGE; }

// The float an R8_UNORM imageStore must receive for the written byte to equal
// ve::encode_sdf(d). Quantising here rather than leaning on the hardware's float->unorm
// conversion removes the only rounding-mode difference between CPU and GPU generation.
float quantise_sdf(float d) {
	float t = clamp((d + SDF_RANGE) / (2.0 * SDF_RANGE), 0.0, 1.0);
	return floor(t * 255.0 + 0.5) / 255.0;
}

vec3 material_albedo(uint mat_id) {
	switch (mat_id) {
		case 1: return vec3(0.36, 0.55, 0.22); // grass
		case 2: return vec3(0.45, 0.42, 0.40); // rock
		case 3: return vec3(0.50, 0.35, 0.20); // dirt
		case 4: return vec3(0.62, 0.60, 0.66); // fill (sphere-add tool)
		default: return vec3(1.0, 0.0, 1.0);   // error magenta
	}
}

vec3 sky_color(vec3 dir) {
	float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
	return mix(vec3(0.55, 0.45, 0.35), vec3(0.25, 0.45, 0.85), t);
}
