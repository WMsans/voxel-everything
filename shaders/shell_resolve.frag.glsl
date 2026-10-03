#[fragment]
#version 460
#include "generated/gbuffer.glslh"

// The G-buffer resolve (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §6
// step 5). After the transparency composite has shaded the lit image, pixels with a
// transparent front take that front as their surface and depth, so everything downstream --
// inject, contact shadows, SSR, outlines -- sees an ordinary glossy surface with no edits of
// its own. A raster pass because a compute shader cannot write a depth attachment.
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 out_surface;

layout(set = 0, binding = 0) uniform sampler2D front_tex;   // xy oct normal, z distance, w material
layout(set = 0, binding = 1) uniform sampler2D front_depth; // the front pass's own depth

// ponytail: one gloss for every transparent material. Read it from the material's surface
// map (material_props) if a matte transparent material is ever added.
const float SHELL_GLOSS = 0.9;

void main() {
	ivec2 px = ivec2(gl_FragCoord.xy);
	vec4 front = texelFetch(front_tex, px, 0);
	// No front: discard, not a colour write -- this pass owns the G-buffer's depth, and a
	// discard is what keeps the opaque surface behind the medium in place.
	if (front.w < 0.5) discard;
	// front.xy is ALREADY the octahedral normal the surface attachment wants, so the _OCT
	// form of the packer: the oct_encode/oct_decode round trip in GB_PACK_SURFACE would be a
	// lossy no-op.
	out_surface = GB_PACK_SURFACE_OCT(front.xy, uint(front.w + 0.5), SHELL_GLOSS);
	gl_FragDepth = texelFetch(front_depth, px, 0).r;
}
