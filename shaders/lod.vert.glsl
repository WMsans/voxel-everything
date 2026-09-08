#[vertex]
#version 460

#include "common.glslh"
#include "lod_quad.glslh"
#include "shade.glslh"

// No vertex buffer and no vertex attributes: geometry is PULLED. The shared index buffer
// supplies {4q, 4q+1, 4q+2, 4q, 4q+2, 4q+3} for q in [0, 512) and each page's draw sets
// vertexOffset = page * 2048, so gl_VertexIndex recovers both the global quad index and the
// page. This is Voxy's gl_VertexID>>2 trick, and it routes around Godot exposing neither
// gl_DrawID nor a non-zero firstInstance.
layout(set = 0, binding = 0, std430) readonly buffer Quads { uint v[]; } quads;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
// Two vec4 per chunk: (origin.xyz, cell size), (level, flags, pad, pad).
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;
layout(set = 0, binding = 5, std430) readonly buffer Normals { uint v[]; } normals;

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;  // xyz = camera position, w = fade start
	vec4 fade; // x = fade end, yzw unused
} pc;

layout(location = 0) out vec3 v_wpos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out flat uint v_material;

void main() {
	uint vi = uint(gl_VertexIndex);
	uint quad = vi >> 2;
	uint corner = vi & 3u;
	uint page = quad >> uint(LOD_PAGE_SHIFT);
	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];

	uvec3 w = uvec3(quads.v[quad * 3u + 0u], quads.v[quad * 3u + 1u], quads.v[quad * 3u + 2u]);

	// Geometry may be a procedural boundary ribbon. Its shading still comes from the
	// original surface quad, not the steep ribbon plane (nor its reverse-wound copy).
	vec3 p0 = lod_corner_pos(w, 0, c0.xyz, c0.w);
	vec3 p1 = lod_corner_pos(w, 1, c0.xyz, c0.w);
	vec3 p2 = lod_corner_pos(w, 2, c0.xyz, c0.w);
	vec3 p3 = lod_corner_pos(w, 3, c0.xyz, c0.w);

	v_wpos = corner == 0u ? p0 : (corner == 1u ? p1 : (corner == 2u ? p2 : p3));
	uint normal_pair = normals.v[quad * 2u + (corner >> 1u)];
	uint packed_normal = (normal_pair >> ((corner & 1u) * 16u)) & 0xFFFFu;
	v_normal = oct_decode_snorm8(packed_normal);
	v_material = lod_bits_get(w, 78, 16);
	gl_Position = pc.view_proj * vec4(v_wpos, 1.0);
}
