#[vertex]
#version 460

#include "generated/blocks.glslh"
#include "common.glslh"
#include "lod_quad.glslh"
#include "shade.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 7
#include "beauty_camera.glslh"

// The far field's transparent shell (docs/superpowers/specs/2026-09-29-transparent-materials-
// design.md §5): the LoD arena pulled exactly as lod.vert.glsl pulls it. The page list holds
// only pages with at least one shell quad, but a page is shared with terrain quads, so every
// non-transparent quad collapses outside the clip volume and rasterizes nothing.
layout(set = 0, binding = 0, std430) readonly buffer Quads { uint v[]; } quads;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;
layout(set = 0, binding = 5, std430) readonly buffer Normals { uint v[]; } normals;

layout(push_constant, std430) uniform Push { TRANSPARENT_RASTER_PUSH_FIELDS } pc;

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
	v_material = lod_bits_get(w, 78, 16);
	if (!mat_transparent(v_material)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	v_wpos = lod_corner_pos(w, int(corner), c0.xyz, c0.w);
	uint normal_pair = normals.v[quad * 2u + (corner >> 1u)];
	v_normal = oct_decode_snorm8((normal_pair >> ((corner & 1u) * 16u)) & 0xFFFFu);
	gl_Position = bcam.view_proj * vec4(v_wpos, 1.0);
}
