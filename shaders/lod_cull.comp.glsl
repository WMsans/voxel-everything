#[compute]
#version 460

#include "common.glslh"
#include "lod_quad.glslh"

// One thread per candidate page. The CPU walk already decided WHAT could be drawn, so this
// pass only ever removes -- it zeroes instanceCount and touches nothing else. That is what
// keeps draw_count an exact CPU integer: Godot's draw_list_draw_indirect takes the count as
// a parameter and exposes no count buffer, so a GPU-decided cut would have to issue tens of
// thousands of empty draws.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) buffer Args { uint v[]; } args;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;
layout(set = 0, binding = 3) uniform sampler2D hiz;
layout(set = 0, binding = 4, std430) buffer Stats { uint v[]; } stats; // [0] = drawn

// The 128-byte push-constant cap (Godot enforces it for compatibility) does not fit both
// view_proj and six precomputed planes, so the planes are derived here from view_proj. Each
// plane is the clip-space inequality expressed as a row of the combined view-projection:
// inside is dot(plane.xyz, p) + plane.w >= 0.
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	ivec4 params; // x = page count, y = hiz size, z = hiz mips, w = unused
} pc;

bool outside_frustum(vec3 lo, vec3 hi) {
	vec4 r0 = vec4(pc.view_proj[0].x, pc.view_proj[1].x, pc.view_proj[2].x, pc.view_proj[3].x);
	vec4 r1 = vec4(pc.view_proj[0].y, pc.view_proj[1].y, pc.view_proj[2].y, pc.view_proj[3].y);
	vec4 r2 = vec4(pc.view_proj[0].z, pc.view_proj[1].z, pc.view_proj[2].z, pc.view_proj[3].z);
	vec4 r3 = vec4(pc.view_proj[0].w, pc.view_proj[1].w, pc.view_proj[2].w, pc.view_proj[3].w);
	vec4 planes[6] = vec4[6](r3 + r2, r3 - r2, r3 + r0, r3 - r1, r3 - r0, r3 + r1);
	for (int i = 0; i < 6; i++) {
		vec3 p = mix(lo, hi, step(vec3(0.0), planes[i].xyz));
		if (dot(planes[i].xyz, p) + planes[i].w < 0.0) return true;
	}
	return false;
}

void main() {
	uint page = gl_GlobalInvocationID.x;
	if (page >= uint(pc.params.x)) return;
	uint base = page * 5u;
	if (args.v[base + 1u] == 0u) return; // already empty

	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];
	vec3 lo = c0.xyz;
	vec3 hi = lo + vec3(c0.w * float(LOD_CHUNK_CELLS));

	if (outside_frustum(lo, hi)) { args.v[base + 1u] = 0u; return; }

	// Screen-space box, and the node's NEAREST reverse-Z depth.
	vec2 mn = vec2(1e30), mx = vec2(-1e30);
	float near_z = 0.0;
	for (int k = 0; k < 8; k++) {
		vec3 p = vec3((k & 1) != 0 ? hi.x : lo.x, (k & 2) != 0 ? hi.y : lo.y,
				(k & 4) != 0 ? hi.z : lo.z);
		vec4 clip = pc.view_proj * vec4(p, 1.0);
		// Straddling the near plane makes the divide meaningless; the only safe answer is
		// "keep it".
		if (clip.w <= 1e-4) { atomicAdd(stats.v[0], 1u); return; }
		vec3 ndc = clip.xyz / clip.w;
		mn = min(mn, ndc.xy * 0.5 + 0.5);
		mx = max(mx, ndc.xy * 0.5 + 0.5);
		near_z = max(near_z, ndc.z);
	}
	mn = clamp(mn, vec2(0.0), vec2(1.0));
	mx = clamp(mx, vec2(0.0), vec2(1.0));

	// Pick the mip whose texels are at least as large as the box, so the loop below is a
	// handful of fetches whatever the box's size.
	vec2 span = (mx - mn) * float(pc.params.y);
	int ml = clamp(int(floor(log2(max(max(span.x, span.y), 1.0)))), 0, pc.params.z - 1);
	int size = max(1, pc.params.y >> ml);
	ivec2 a = ivec2(floor(mn * float(size)));
	ivec2 b = min(ivec2(ceil(mx * float(size))), ivec2(size - 1));

	float occluder = 1.0;
	for (int y = a.y; y <= b.y; y++)
		for (int x = a.x; x <= b.x; x++)
			occluder = min(occluder, texelFetch(hiz, ivec2(x, y), ml).r);

	// Reverse-Z: if the node's nearest point is behind the farthest occluder over its whole
	// footprint, nothing in it can be seen.
	if (near_z < occluder) { args.v[base + 1u] = 0u; return; }
	atomicAdd(stats.v[0], 1u);
}
