#[vertex]
#version 460

#include "generated/blocks.glslh"
#include "common.glslh"
#include "lod_quad.glslh"
#include "shade.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 7
#include "beauty_camera.glslh"

// The transparent shell (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §5):
// LoD arena pages pulled exactly as lod.vert.glsl pulls them. Three kinds of page arrive in
// one list. A far-field page is shared with terrain quads, so every non-transparent quad
// collapses outside the clip volume. A near-shell page (chunk flag bit 0) holds only shell
// quads. An island page (chunk flag bits 8..) holds LOCAL-space quads, placed by the island's
// descriptor.
layout(set = 0, binding = 0, std430) readonly buffer Quads { uint v[]; } quads;
layout(set = 0, binding = 1, std430) readonly buffer PageChunk { uint v[]; } page_chunk;
// Two vec4 per chunk: (origin.xyz, cell size), (level, flags, pad, pad) -- the second
// vec4's integer words, so `.y` is `flags` (LodPool::upload_at writes meta[1] there).
layout(set = 0, binding = 2, std430) readonly buffer Chunks { vec4 v[]; } chunks;
layout(set = 0, binding = 5, std430) readonly buffer Normals { uint v[]; } normals;
// Eight vec4 per island, as raymarch.comp.glsl reads them: basis columns 0-2 with the body
// translation in .w.
layout(set = 0, binding = 8, std430) readonly buffer IslandDesc { vec4 v[]; } island_desc;

layout(location = 0) out vec3 v_wpos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out flat uint v_material;
layout(location = 3) out flat uint v_near; // 1 = keep on the near field's side of the dither

void main() {
	uint vi = uint(gl_VertexIndex);
	uint quad = vi >> 2;
	uint corner = vi & 3u;
	uint page = quad >> uint(LOD_PAGE_SHIFT);
	uint ci = page_chunk.v[page];
	vec4 c0 = chunks.v[ci * 2u + 0u];
	uint flags = floatBitsToUint(chunks.v[ci * 2u + 1u].y);
	uvec3 w = uvec3(quads.v[quad * 3u + 0u], quads.v[quad * 3u + 1u], quads.v[quad * 3u + 2u]);
	v_material = lod_bits_get(w, 78, 16);
	v_near = flags != 0u ? 1u : 0u;
	if (!mat_transparent(v_material)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	vec3 p = lod_corner_pos(w, int(corner), c0.xyz, c0.w);
	uint normal_pair = normals.v[quad * 2u + (corner >> 1u)];
	vec3 n = oct_decode_snorm8((normal_pair >> ((corner & 1u) * 16u)) & 0xFFFFu);
	uint island = flags >> 8;
	if (island != 0u) {
		int i = int(island) - 1;
		vec4 r0 = island_desc.v[i * 8 + 0];
		vec4 r1 = island_desc.v[i * 8 + 1];
		vec4 r2 = island_desc.v[i * 8 + 2];
		mat3 basis = mat3(r0.xyz, r1.xyz, r2.xyz);
		p = basis * p + vec3(r0.w, r1.w, r2.w);
		n = basis * n;
	}
	v_wpos = p;
	v_normal = n;
	gl_Position = bcam.view_proj * vec4(p, 1.0);
}