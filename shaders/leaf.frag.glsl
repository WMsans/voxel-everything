#[fragment]
#version 460
#include "generated/gbuffer.glslh"
#include "generated/blocks.glslh"

#include "common.glslh"
#include "shade.glslh"
// R2: ONE definition of the tree hashes, shared with the scatter and the terrain stage --
// this file includes tree.glslh itself rather than restating tree_hash/tree_unit/tree_hash2.
#include "tree.glslh"
#include "leaf.glslh"

layout(set = 0, binding = 1, std140) uniform Params { LEAF_PARAMS_FIELDS } leaf;

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in float v_sun;
layout(location = 4) in float v_depth_t;
layout(location = 5) in float v_hash;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss

// Value noise on the card's own UV, for the scalloped edge. Procedural rather than a texture
// so the pass needs no asset and no extra binding -- the same posture grass takes.
float leaf_noise(vec2 p, uint salt) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = tree_unit(tree_hash2(ivec2(i), salt));
	float b = tree_unit(tree_hash2(ivec2(i) + ivec2(1, 0), salt));
	float c = tree_unit(tree_hash2(ivec2(i) + ivec2(0, 1), salt));
	float d = tree_unit(tree_hash2(ivec2(i) + ivec2(1, 1), salt));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
	uint h = floatBitsToUint(v_hash);

	// THE SILHOUETTE. A plain quad reads as a rectangle; the reference's edge leaves punch
	// out as individual round dots against the sky. Radial falloff plus noise, thresholded,
	// gives a lobed edge and opens a few holes through to sky.
	//
	// ponytail: `discard` costs early-Z. If overdraw proves to be the binding cost, build the
	// scallop from a fan of sub-quads in leaf.vert.glsl instead -- same look, no discard.
	float r = length(v_uv);
	float scallop = leaf_noise(v_uv * 3.0 + vec2(tree_unit(h) * 17.0), 0x7A1u);
	if (r - 0.35 * (scallop - 0.5) > 0.92) discard;
	// NO interior holes. A card's interior used to punch a second, higher-frequency noise
	// through to whatever was behind it, "so light reads through a crown rather than off a
	// wall". The clumps sit on the lobe SHELL (LeafSettings::shell_min), so the canopy is one
	// layer thick and those holes opened onto the far background: a few pixels of sky or
	// hillside, every one of them a depth cliff that outline.comp.glsl reads as a silhouette
	// and darkens. Because the pattern is fixed in CARD space and the cards re-face the
	// camera every frame, the dark specks crawled across the canopy as the camera moved --
	// the reported flicker. The scalloped edge above is what lets sky through a crown.

	// Dither out over the last fifth of the reach, so the canopy tail does not pop. Same
	// bayer4 discard lod.frag.glsl and grass.frag.glsl use at the LoD/reach seams -- the
	// ordered pattern is what makes a fade read as thinning rather than as noise.
	// leaf.cam.w is the reach the scatter already uploaded; no new uniform.
	float fade = clamp((length(v_world - leaf.cam.xyz) - leaf.cam.w * 0.8)
			/ max(leaf.cam.w * 0.2, 1e-3), 0.0, 1.0);
	if (fade > 0.0 && bayer4(ivec2(gl_FragCoord.xy)) < fade) discard;

	vec3 n = normalize(v_normal);
	// The vertex turned the face to the viewer (backfaces draw too: cull is disabled), so
	// this normal is the visible side's -- grass.frag.glsl's lesson, taken where to_cam is
	// in hand.

	// TWO GRADIENTS, baked into albedo before the deferred pass sees it. Cel shading and the
	// outline pass then apply on top, unmodified -- this is the same arrangement grass uses
	// for its root-to-tip gradient, and it is what keeps the module out of the beauty stack.
	//
	// 1. n.y: each lobe's top goes bright warm yellow-green, its underside deep cool blue-green.
	const vec3 kTop = vec3(0.52, 0.66, 0.24);
	const vec3 kUnder = vec3(0.12, 0.26, 0.19);
	vec3 albedo = mix(kUnder, kTop, n.y * 0.5 + 0.5);
	// 2. crown depth: the whole crown darkens toward its base. This is the ambient-occlusion
	// read in the reference, and it is a per-clump constant, so it is free.
	albedo *= mix(0.55, 1.0, v_depth_t);
	// Per-clump hue and value jitter, so a grove is not a flat green wall.
	albedo *= 1.0 + leaf.style.y * (tree_unit(tree_hash(h ^ 0x2Bu)) * 2.0 - 1.0);
	albedo.g *= 1.0 + leaf.style.y * 0.5 * (tree_unit(tree_hash(h ^ 0x2Cu)) * 2.0 - 1.0);

	// Alpha is SUN VISIBILITY, not coverage. deferred.comp.glsl reads G-buffer albedo alpha
	// as sun visibility and only mins in the sun map where far_field_owns(px), so a clump
	// writing 1.0 would be unshadowed near the camera -- exactly the bug the grass lighting
	// revision had to come back and fix.
	out_albedo = GB_PACK_ALBEDO(albedo, v_sun);
	// Material id MAT_LEAF_CLUMP (201): a foliage id, above every terrain id, with no atlas
	// layer. deferred.comp.glsl uses the id ONLY for the emissive lookup, and leaf glow is
	// zero, so this needs no change anywhere in the shading stack.
	out_surface = GB_PACK_SURFACE(n, MAT_LEAF_CLUMP, leaf.style.x);
}
