#[vertex]
#version 460

#include "common.glslh"
#include "lod_quad.glslh"

layout(set = 0, binding = 0, std430) readonly buffer Quads { uint v[]; } quads;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;

layout(push_constant, std430) uniform Push {
	mat4 sun_view_proj;
} pc;

void main() {
	uint vi = uint(gl_VertexIndex);
	uint quad = vi >> 2;
	uint corner = vi & 3u;
	uint page = quad >> uint(LOD_PAGE_SHIFT);
	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];
	uvec3 w = uvec3(quads.v[quad * 3u + 0u], quads.v[quad * 3u + 1u], quads.v[quad * 3u + 2u]);
	vec3 p = lod_corner_pos(w, int(corner), c0.xyz, c0.w);
	gl_Position = pc.sun_view_proj * vec4(p, 1.0);
}
