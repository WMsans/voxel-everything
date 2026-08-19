#[compute]
#version 460

#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 4
#include "common.glslh"
#include "shade.glslh"
#include "beauty_camera.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1, r8) writeonly uniform image2D out_mask;
layout(set = 0, binding = 2) uniform sampler2D mask_tex;
layout(set = 0, binding = 3, rgba16f) uniform image2D scene_color;

layout(push_constant, std430) uniform Push {
	ivec4 dims;  // xy = target size, z = mode (0 march, 1 apply), w = steps
	vec4 params; // x = reach in metres, y = strength, z = depth tolerance in metres
} pc;

float march(vec2 uv, float depth) {
	vec3 p = beauty_world_from_depth(uv, depth);
	float jitter = bayer4(ivec2(gl_GlobalInvocationID.xy));
	float step_m = pc.params.x / float(max(pc.dims.w, 1));
	for (int i = 0; i < pc.dims.w; i++) {
		vec3 q = p + SUN_DIR * (step_m * (float(i) + jitter));
		vec2 quv;
		float qdepth;
		if (!beauty_project(q, quv, qdepth)) break;
		if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;
		float scene = texture(scene_depth, quv).r;
		if (scene <= 0.0) continue;
		vec3 occluder = beauty_world_from_depth(quv, scene);
		// Reverse-Z is handled by comparing world distance to the camera: the nearer sample
		// is the occluder, and the metric tolerance avoids projection-dependent tuning.
		if (distance(occluder, bcam.cam.xyz) < distance(q, bcam.cam.xyz) - 0.02 &&
				distance(occluder, q) < pc.params.z)
			return 0.0;
	}
	return 1.0;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);

	if (pc.dims.z == 0) {
		float depth = texture(scene_depth, uv).r;
		imageStore(out_mask, px, vec4(depth <= 0.0 ? 1.0 : march(uv, depth)));
		return;
	}

	// march() jitters its ray start by bayer4, so a marginally-occluded pixel comes back 0 or
	// 1 according to its slot in the 4x4 pattern. That is a stochastic estimate of fractional
	// visibility and only carries its intended meaning once the pattern is averaged back out;
	// sampled raw it reaches the screen as a lattice of isolated black dots on open ground.
	//
	// The box is exactly the 4x4 bayer period in MASK space, so every jitter phase is counted
	// once and the average is the unbiased visibility the jitter was encoding. Offsets -1..2
	// cover a full period whatever the pixel's own phase is.
	ivec2 msize = max(pc.dims.xy / 2, ivec2(1));
	ivec2 mpx = clamp(ivec2(uv * vec2(msize)), ivec2(0), msize - 1);
	float vis = 0.0;
	for (int y = -1; y <= 2; y++)
		for (int x = -1; x <= 2; x++)
			vis += texelFetch(mask_tex, clamp(mpx + ivec2(x, y), ivec2(0), msize - 1), 0).r;
	vis *= 1.0 / 16.0;
	vec4 c = imageLoad(scene_color, px);
	imageStore(scene_color, px, vec4(c.rgb * mix(1.0, vis, pc.params.y), c.a));
}
