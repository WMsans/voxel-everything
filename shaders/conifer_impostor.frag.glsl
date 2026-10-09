#[fragment]
#version 460
#include "generated/gbuffer.glslh"
#include "generated/blocks.glslh"

#include "common.glslh"
#include "shade.glslh"
#include "conifer_pass.glslh"

layout(set = 0, binding = 1, std140) uniform Params { CONIFER_PASS_FIELDS } conifer;
// The card raster's block: the palette and style the cards use, so both bands match.
layout(set = 0, binding = 2, std140) uniform LeafStyle { LEAF_PARAMS_FIELDS } leaf;
layout(push_constant, std430) uniform Push { mat4 view_proj; vec4 cam; } pc;

vec4 conifer_ground(vec2 xz) { return vec4(0.0); }
#include "conifer.glslh"

layout(location = 0) in vec3 v_world;
layout(location = 1) flat in vec4 v_a;
layout(location = 2) flat in vec4 v_b;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss
// The hit is never nearer than the quad (conifer_impostor.vert.glsl puts the quad in front of
// the crown), and nearer is larger in reverse-Z: this keeps early depth on.
layout(depth_less) out float gl_FragDepth;

void main() {
	Conifer c = conifer_unpack(v_a, v_b);
	vec3 ro = pc.cam.xyz;
	vec3 rd = normalize(v_world - ro);
	// One step per pixel of world the quad covers here: finer cannot show.
	float t = conifer_ray_hit(c, ro, rd, length(fwidth(v_world)));
	if (t < 0.0) discard;
	vec3 hit = ro + rd * t;

	// Exactly the pixels leaf.frag.glsl drops from the cards in the hand-off band, and none
	// nearer; then the imposter's own dither-out over the last fifth of its reach.
	float b = bayer4(ivec2(gl_FragCoord.xy));
	if (!(b < conifer_card_fade(t, conifer.cam.w))) discard;
	float out_fade = conifer_card_fade(t, conifer.reach.x);
	if (out_fade > 0.0 && b < out_fade) discard;

	vec3 radial = vec3(hit.x - c.foot.x, 0.0, hit.z - c.foot.z);
	float rl = length(radial);
	radial = rl > 1e-4 ? radial / rl : vec3(1.0, 0.0, 0.0);
	vec3 n = conifer_normal(c, radial, conifer.reach.z);
	float s = clamp(conifer_s(c, hit.y), 0.0, 1.0);

	// The cards' colour, as leaf.frag.glsl builds it: palette by n.y, crown-depth darkening,
	// per-tree hue jitter. The per-leaf brightness term is the cards' alone.
	vec3 albedo = mix(leaf.palette_under.rgb, leaf.palette_top.rgb, n.y * 0.5 + 0.5);
	albedo *= mix(0.55, 1.0, s);
	albedo *= 1.0 + leaf.style.y * (tree_unit(tree_hash(c.hash ^ 0x2Bu)) * 2.0 - 1.0);
	albedo.g *= 1.0 + leaf.style.y * 0.5 * (tree_unit(tree_hash(c.hash ^ 0x2Cu)) * 2.0 - 1.0);
	// Self-shade only: past 150 m the deferred pass mins the sun map in (far_field_owns), so
	// terrain shadow on imposters is free (spec §3).
	float sun = mix(1.0 - conifer.reach.w, 1.0, s);
	out_albedo = GB_PACK_ALBEDO(albedo, sun);
	out_surface = GB_PACK_SURFACE(n, MAT_LEAF_CLUMP, leaf.style.x);
	vec4 clip = pc.view_proj * vec4(hit, 1.0);
	gl_FragDepth = clip.z / clip.w;
}
