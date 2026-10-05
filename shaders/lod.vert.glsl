#[vertex]
#version 460

#include "generated/blocks.glslh"
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
// Island pages only (raster mode, spec 2026-10-04 §5). LodRasterPass binds a dead
// descriptor when no island atlas exists.
layout(set = 0, binding = 8, std430) readonly buffer IslandDesc { vec4 v[]; } island_desc;
#include "island_xform.glslh"

layout(push_constant, std430) uniform Push { LOD_RASTER_PUSH_FIELDS } pc;

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

	vec3 wpos = corner == 0u ? p0 : (corner == 1u ? p1 : (corner == 2u ? p2 : p3));
	uint normal_pair = normals.v[quad * 2u + (corner >> 1u)];
	uint packed_normal = (normal_pair >> ((corner & 1u) * 16u)) & 0xFFFFu;
	vec3 nrm = oct_decode_snorm8(packed_normal);
	// An island page holds LOCAL-space quads; its flags (bits 8..) name the island. Terrain
	// pages carry 0 there and are untouched.
	island_place(floatBitsToUint(chunks.v[ci * 2u + 1u].y) >> 8, wpos, nrm);
	v_wpos = wpos;
	v_normal = nrm;
	v_material = lod_bits_get(w, 78, 16);
	// Spec §5: with transparency on, a shell quad belongs to transparent.vert.glsl, not to the
	// terrain. Collapse it outside the clip volume so it rasterizes nothing. pc.fade.y is 1
	// exactly when transparency is enabled; off, the shell draws here as the opaque surface
	// the material used to be.
	if (pc.fade.y > 0.5 && mat_transparent(v_material)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	gl_Position = pc.view_proj * vec4(wpos, 1.0);
}
