#[compute]
#version 460
#include "generated/gbuffer.glslh"
#include "generated/blocks.glslh"

// Ambient occlusion AND the diffuse bounce from one horizon march: screen-space indirect
// lighting with a visibility bitmask (Therrien, Levesque & Gilet 2023, arXiv 2301.11376),
// the GTAO family. Each slice through the view ray splits the normal's hemisphere into 32
// sectors; a depth sample occludes the sectors between its visible face and a point
// kThickness behind it. Sectors a sample covers FIRST are the ones it lights the receiver
// through, so the bounce reuses the AO's taps instead of gathering its own -- which is the
// whole saving over the old HBAO + spiral-gather pair (11.9 ms -> one pass).
//
// rgb = bounce + emissive light, a = AO. The deferred pass multiplies only its sky term by a
// and adds rgb unoccluded: the bitmask already resolved visibility per sample, so applying
// AO to the bounce too would count the same occlusion twice.

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
// The raw gather, before the resolve below averages its bayer rotation back out. The same
// texture as a storage image (written by the gather) and a sampler (read by the resolve), the
// pattern contact_shadow.comp.glsl uses for its mask.
layout(set = 0, binding = 6, rgba16f) writeonly uniform image2D out_raw;
layout(set = 0, binding = 7) uniform sampler2D raw_tex;

layout(push_constant, std430) uniform Push { SSGI_PUSH_FIELDS } pc;

const float kPi = 3.14159265;
// How far behind its visible face a depth sample is assumed to extend. Infinite thickness
// (plain horizon AO) lets one grass blade or fence post shadow everything behind it.
// ponytail: one constant for every material; per-material thickness if foliage over-darkens.
const float kThickness = 1.0;

// Bits [lo, hi) of the 32-sector mask, 0 <= lo, hi <= 32.
uint sector_mask(uint lo, uint hi) {
	if (hi <= lo) return 0u;
	uint upper = hi >= 32u ? 0xFFFFFFFFu : (1u << hi) - 1u;
	return upper & ~((1u << lo) - 1u);
}

// Cosine-weighted fraction of a slice's hemisphere the set sectors cover: sector i sits at
// (i + 0.5) / 32 * pi - pi/2 from the normal, and the 32 weights sum to ~1.
float sector_weight(uint bits) {
	float w = 0.0;
	while (bits != 0u) {
		int i = findLSB(bits);
		bits &= bits - 1u;
		w += sin((float(i) + 0.5) * (kPi / 32.0));
	}
	return w * (kPi / 64.0);
}

bool previous_uv(vec3 p, out vec2 uv) {
	vec4 clip = pc.prev_view_proj * vec4(p, 1.0);
	if (clip.w <= 0.0) return false;
	vec3 ndc = clip.xyz / clip.w;
	uv = ndc.xy * 0.5 + 0.5;
	return all(greaterThanEqual(uv, vec2(0.0))) &&
			all(lessThanEqual(uv, vec2(1.0)));
}

// Light arriving from one occluder sample: last frame's lit colour where it was, plus the
// material table's glow (immediate, so a source that just came into view is not ~10 frames
// late). The glow mask texture is deliberately not read: a triplanar fetch per tap.
vec3 sample_light(vec2 suv, vec3 sp) {
	vec3 l = vec3(0.0);
	vec2 huv;
	if (previous_uv(sp, huv)) l = texture(history, huv).rgb * pc.params.z;
	if (pc.emissive.y > 0.0) {
		vec4 sg = texture(gb_surface, suv);
		uint smat = GB_MATERIAL_ID(sg);
		float sglow = GB_IS_SURFACE(sg) ? mat_glow(smat) : 0.0;
		if (sglow > 0.0) l += mat_glow_rgb(smat) * sglow * pc.emissive.y;
	}
	return l;
}

vec4 gather(ivec2 px, vec3 p, vec3 n) {
	// The bounce needs last frame's lit colour; until it exists the pass is AO only.
	bool want_gi = (pc.stage.z & 2) != 0 && pc.dims.w != 0;
	int slices = max(pc.stage.y, 1);
	int steps = max(pc.dims.z, 1);
	float radius = pc.params.x;
	vec2 inv_target = 1.0 / vec2(pc.dims.xy);

	// Slices contain the view ray, so their directions are perpendicular to it.
	vec3 v = normalize(bcam.cam.xyz - p);
	vec3 up = abs(v.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 t1 = normalize(cross(up, v));
	vec3 t2 = cross(v, t1);
	vec3 to_p = normalize(p - bcam.cam.xyz);

	// A fixed per-pixel rotation and step offset: resolve() averages one full 4x4 bayer
	// period, so 2 slices per pixel read as 32 directions without any temporal jitter.
	float rot = bayer4(px);
	vec2 pu;
	float pd;
	beauty_project(p, pu, pd);

	float occluded = 0.0;
	vec3 light = vec3(0.0);
	for (int s = 0; s < slices; s++) {
		float phi = (float(s) + rot) * kPi / float(slices);
		vec3 dir = t1 * cos(phi) + t2 * sin(phi);
		vec2 qu;
		float qd;
		if (!beauty_project(p + dir * radius, qu, qd)) continue;
		vec2 step_px = (qu - pu) * vec2(pc.dims.xy);
		// The radius projects under half a texel: every tap would land on this pixel.
		if (dot(step_px, step_px) < 0.25) continue;

		// The normal's angle inside the slice plane, measured from v towards dir.
		vec3 axis = cross(dir, v);
		vec3 np = n - axis * dot(n, axis);
		float n_angle = atan(dot(np, dir), dot(np, v));

		uint bits = 0u;
		for (int side = -1; side <= 1; side += 2) {
			for (int i = 0; i < steps; i++) {
				// Quadratic spacing: dense near the pixel, where contact occlusion lives.
				float f = (float(i) + 0.25 + 0.5 * rot) / float(steps);
				vec2 suv = (vec2(px) + 0.5 + float(side) * step_px * (f * f)) * inv_target;
				if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
				float sdepth = texture(gb_depth, suv).r;
				if (sdepth <= 0.0) continue;
				vec3 sp = beauty_world_from_depth(suv, sdepth);
				vec3 front = sp - p;
				if (dot(front, front) > radius * radius) continue;
				vec3 back = front + normalize(sp - bcam.cam.xyz) * kThickness;
				vec2 a = vec2(atan(dot(front, dir), dot(front, v)),
						atan(dot(back, dir), dot(back, v)));
				vec2 t = clamp((a - n_angle) / kPi + 0.5, 0.0, 1.0);
				uint mask = sector_mask(uint(round(min(t.x, t.y) * 32.0)),
						uint(round(max(t.x, t.y) * 32.0)));
				uint fresh = mask & ~bits;
				if (fresh == 0u) continue;
				bits |= mask;
				if (want_gi) light += sample_light(suv, sp) * sector_weight(fresh);
			}
		}
		occluded += sector_weight(bits);
	}
	float ao = (pc.stage.z & 1) != 0
			? clamp(1.0 - occluded / float(slices) * pc.params.w, 0.0, 1.0)
			: 1.0;
	return vec4(light / float(slices), ao);
}

// The gather's bayer4 rotation is a stochastic estimate: each pixel sees its slices from one
// of sixteen angles, and with two slices which angle it drew is most of its answer. Sampled
// raw, that reaches the screen as a lattice of dots across every lit surface. The box below
// spans one full period (offsets -1..2, as in contact_shadow.comp.glsl), so every rotation is
// counted once and the average is the integral the rotations were sampling.
//
// Unlike the contact-shadow mask this is not a scalar visibility on one plane: GI and AO change
// sharply across a voxel edge, so a neighbour only counts while it lies on the same surface --
// facing the same way, and near this pixel's tangent plane. The centre always does, so a
// pixel with no matching neighbour keeps its own value rather than going dark.
vec4 resolve(ivec2 px, vec3 p, vec3 n) {
	float tolerance = max(0.05, 0.02 * distance(p, bcam.cam.xyz));
	vec4 sum = vec4(0.0);
	float weight = 0.0;
	for (int y = -1; y <= 2; y++)
		for (int x = -1; x <= 2; x++) {
			ivec2 npx = clamp(px + ivec2(x, y), ivec2(0), pc.dims.xy - 1);
			vec2 nuv = (vec2(npx) + 0.5) / vec2(pc.dims.xy);
			float ndepth = texture(gb_depth, nuv).r;
			vec4 ng = texture(gb_surface, nuv);
			if (ndepth <= 0.0 || !GB_IS_SURFACE(ng)) continue;
			float facing = dot(n, GB_NORMAL(ng));
			if (facing < 0.9) continue;
			float plane = abs(dot(n, beauty_world_from_depth(nuv, ndepth) - p));
			float w = facing * clamp(1.0 - plane / tolerance, 0.0, 1.0);
			sum += texelFetch(raw_tex, npx, 0) * w;
			weight += w;
		}
	return weight > 0.0 ? sum / weight : texelFetch(raw_tex, px, 0);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	bool gathering = pc.stage.x == 0;

	// Sky: no bounce, and full ambient so the horizon gradient is never darkened.
	float depth = texture(gb_depth, uv).r;
	vec4 g1 = texture(gb_surface, uv);
	if (depth <= 0.0 || !GB_IS_SURFACE(g1)) {
		if (gathering) imageStore(out_raw, px, vec4(0.0, 0.0, 0.0, 1.0));
		else imageStore(out_ssgi, px, vec4(0.0, 0.0, 0.0, 1.0));
		return;
	}
	vec3 p = beauty_world_from_depth(uv, depth);
	vec3 n = GB_NORMAL(g1);

	if (gathering) {
		imageStore(out_raw, px, gather(px, p, n));
		return;
	}
	vec4 gi = resolve(px, p, n);

	// Without a lit history the previous result is the cleared target (AO 0): never blend it.
	vec2 puv;
	if (pc.dims.w != 0 && previous_uv(p, puv)) {
		vec4 lo = gi;
		vec4 hi = gi;
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++) {
				ivec2 npx = clamp(px + ivec2(x, y), ivec2(0), pc.dims.xy - 1);
				vec2 nuv = (vec2(npx) + 0.5) / vec2(pc.dims.xy);
				float ndepth = texture(gb_depth, nuv).r;
				if (ndepth <= 0.0) continue;
				vec3 np = beauty_world_from_depth(nuv, ndepth);
				vec2 npuv;
				if (!previous_uv(np, npuv)) continue;
				vec4 s = texture(prev_ssgi, npuv);
				lo = min(lo, s);
				hi = max(hi, s);
			}
		vec4 prev = clamp(texture(prev_ssgi, puv), lo, hi);
		gi = mix(gi, prev, pc.params.y);
	}
	imageStore(out_ssgi, px, gi);
}
