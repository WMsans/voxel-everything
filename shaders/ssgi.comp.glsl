#[compute]
#version 460

#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 5
#include "common.glslh"
#include "shade.glslh"
#include "beauty_camera.glslh"
#include "material_table.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gb_surface;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D history;
layout(set = 0, binding = 3) uniform sampler2D prev_ssgi;
layout(set = 0, binding = 4, rgba16f) writeonly uniform image2D out_ssgi;

layout(push_constant, std430) uniform Push {
	mat4 prev_view_proj;
	ivec4 dims;
	vec4 params;   // x = bounce radius (m), y = temporal history weight, z = bounce strength
	vec4 emissive; // x = emissive radius (m), y = emissive strength, zw unused
} pc;

vec2 spiral_tap(int i, int n, float rot) {
	float t = (float(i) + 0.5) / float(n);
	float a = t * 6.28318531 * 3.0 + rot;
	return vec2(cos(a), sin(a)) * sqrt(t);
}

// How far, in UV, a world-space step of `radius` metres reaches at p's depth.
//
// This replaces `radius * bcam.screen.z * 40.0 / max(distance(p, cam), 1.0)`, whose divisor
// CLAMPED AT ONE. Inside a metre of the surface the clamp stopped tracking distance, the
// taps kept widening, and nearly all of them then landed further than `radius` away in world
// space and were rejected by the range test below -- so pressing the camera against a glowing
// surface sampled almost nothing. Projecting a real world-space offset has no such floor:
// it grows as the surface approaches and shrinks as it recedes, at every distance.
//
// The offset is taken perpendicular to the view ray so it measures the screen extent of a
// step ACROSS the view rather than along it, which is the direction the spiral walks.
float uv_radius_for(vec3 p, float radius) {
	vec3 view = p - bcam.cam.xyz;
	float len = length(view);
	if (len < 1e-4) return 0.5;
	view /= len;
	vec3 up = abs(view.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
	vec3 tangent = normalize(cross(view, up));
	vec2 uv_c, uv_t;
	float dc, dt;
	if (!beauty_project(p, uv_c, dc) || !beauty_project(p + tangent * radius, uv_t, dt))
		return 0.5;
	// Half the screen is already a gather over everything visible; beyond that the spiral is
	// just scattering taps into unrelated geometry that the range test will throw away.
	return min(length(uv_t - uv_c), 0.5);
}

bool previous_uv(vec3 p, out vec2 uv) {
	vec4 clip = pc.prev_view_proj * vec4(p, 1.0);
	if (clip.w <= 0.0) return false;
	vec3 ndc = clip.xyz / clip.w;
	uv = ndc.xy * 0.5 + 0.5;
	return all(greaterThanEqual(uv, vec2(0.0))) &&
			all(lessThanEqual(uv, vec2(1.0)));
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);

	if (pc.dims.w == 0) {
		imageStore(out_ssgi, px, vec4(0.0));
		return;
	}

	float depth = texture(gb_depth, uv).r;
	vec4 g1 = texture(gb_surface, uv);
	if (depth <= 0.0 || g1.z < 0.5) {
		imageStore(out_ssgi, px, vec4(0.0));
		return;
	}
	vec3 p = beauty_world_from_depth(uv, depth);
	vec3 n = oct_decode(g1.xy);

	float rot = bayer4(px) * 6.28318531;

	// --- the diffuse bounce ------------------------------------------------------------
	// Averaged over the taps that hit, which is what an ambient bounce is: a mean incoming
	// radiance over the hemisphere, not a sum of light sources.
	vec3 sum = vec3(0.0);
	float weight = 0.0;
	float bounce_uv_radius = uv_radius_for(p, pc.params.x);
	for (int i = 0; i < pc.dims.z; i++) {
		vec2 suv = clamp(uv + spiral_tap(i, pc.dims.z, rot) * bounce_uv_radius,
				vec2(0.0), vec2(1.0));
		float sdepth = texture(gb_depth, suv).r;
		if (sdepth <= 0.0) continue;
		vec3 sp = beauty_world_from_depth(suv, sdepth);
		vec3 dir = sp - p;
		float dist = length(dir);
		if (dist < 1e-3 || dist > pc.params.x) continue;
		dir /= dist;
		float cosine = dot(n, dir);
		if (cosine <= 0.0) continue;
		vec3 sn = oct_decode(texture(gb_surface, suv).xy);
		if (dot(sn, -dir) <= 0.0) continue;
		vec2 history_uv;
		if (!previous_uv(sp, history_uv)) continue;
		float falloff = 1.0 / (1.0 + dist * dist);
		sum += texture(history, history_uv).rgb * cosine * falloff;
		weight += 1.0;
	}
	vec3 gi = weight > 0.0 ? sum / weight * pc.params.z : vec3(0.0);

	// --- emissive light ----------------------------------------------------------------
	// Its own ring, at its own (much larger) radius, and read from the MATERIAL TABLE rather
	// than from the lit history. Two reasons the history alone was never going to carry this:
	//
	//   * it is a temporal accumulator, so a source that just came into view arrives ~10
	//     frames late and pre-dimmed by whatever the blend had already settled on;
	//   * the bounce term above AVERAGES over its taps, so one very bright tap among eight
	//     reads no brighter than one dull one. That is right for ambient and wrong for a
	//     light: filling more of the hemisphere with lava must deliver more light.
	//
	// So this accumulates an INTEGRAL over the whole ring -- divided by the tap count, not by
	// the count that hit -- and every tap that misses is honestly zero light from that
	// direction.
	//
	// The glow mask (the albedo array's alpha) is deliberately not sampled here: it would
	// cost a triplanar fetch per tap, and reading the table's full strength over the whole
	// footprint overestimates in the one direction this feature wants.
	vec3 emissive = vec3(0.0);
	if (pc.emissive.y > 0.0) {
		float em_uv_radius = uv_radius_for(p, pc.emissive.x);
		for (int i = 0; i < pc.dims.z; i++) {
			vec2 suv = clamp(uv + spiral_tap(i, pc.dims.z, rot) * em_uv_radius,
					vec2(0.0), vec2(1.0));
			float sdepth = texture(gb_depth, suv).r;
			if (sdepth <= 0.0) continue;
			vec4 sg = texture(gb_surface, suv);
			if (sg.z < 0.5) continue;
			uint smat = uint(sg.z + 0.5);
			float sglow = mat_glow(smat);
			if (sglow <= 0.0) continue;
			vec3 sp = beauty_world_from_depth(suv, sdepth);
			vec3 dir = sp - p;
			float dist = length(dir);
			if (dist < 1e-3 || dist > pc.emissive.x) continue;
			dir /= dist;
			float cosine = dot(n, dir);
			if (cosine <= 0.0) continue;
			// The emitter has to face us back, exactly as the bounce requires. Without it a
			// crack lights the rock it is carved into from behind.
			if (dot(oct_decode(sg.xy), -dir) <= 0.0) continue;
			// 1/(1+d) rather than the bounce's 1/(1+d*d): a crack is a LINE of emitters, not
			// a point, and a line source falls off with the first power of distance. Inverse
			// square here made the spill die within a couple of metres, which is the whole
			// complaint this pass exists to answer.
			emissive += mat_glow_rgb(smat) * sglow * cosine / (1.0 + dist);
		}
		emissive *= pc.emissive.y / float(pc.dims.z);
	}
	gi += emissive;

	vec2 puv;
	bool reproj = previous_uv(p, puv);
	if (reproj) {
		vec3 lo = gi;
		vec3 hi = gi;
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++) {
				ivec2 npx = clamp(px + ivec2(x, y), ivec2(0), pc.dims.xy - 1);
				vec2 nuv = (vec2(npx) + 0.5) / vec2(pc.dims.xy);
				float ndepth = texture(gb_depth, nuv).r;
				if (ndepth <= 0.0) continue;
				vec3 np = beauty_world_from_depth(nuv, ndepth);
				vec2 npuv;
				if (!previous_uv(np, npuv)) continue;
				vec3 s = texture(prev_ssgi, npuv).rgb;
				lo = min(lo, s);
				hi = max(hi, s);
			}
		vec3 prev = clamp(texture(prev_ssgi, puv).rgb, lo, hi);
		gi = mix(gi, prev, pc.params.y);
	}
	imageStore(out_ssgi, px, vec4(gi, 1.0));
}
