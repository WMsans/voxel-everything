#[fragment]
#version 460
#define MATERIAL_LAYERS 16
layout(set = 0, binding = 2) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 3) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 frag_color;
#ifdef SEAM_MARKER
layout(location = 1) out uint marker; // debug seam probe: 1 = near field kept the pixel
#endif
layout(set = 0, binding = 0) uniform sampler2D src_color;  // linear (0.66x upscale)
layout(set = 0, binding = 1) uniform sampler2D src_hitpos; // nearest (no silhouette smear)
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;  // xyz = camera position, w = fade start
	vec4 fade; // x = fade end, yzw unused
} pc;
void main() {
	vec4 hp = texture(src_hitpos, uv_in);
	frag_color = texture(src_color, uv_in);
	if (hp.w < 0.5) {
		// Sky: farthest. Godot 4.7 renders with reverse-Z (near=1.0, far=0.0 — the scene
		// projection is depth-corrected with remap_z+reverse_z), so the farthest depth is
		// 0.0, and the composite pipeline tests GREATER_OR_EQUAL so nearer scene geometry
		// (opaque objects' pre-pass depth) is never overwritten.
		#ifdef SEAM_MARKER
		marker = 0u;
		#endif
		gl_FragDepth = 0.0;
		return;
	}
	// Spec section 7.4: discard the DEPTH, keeping the colour, over the band. Keeping the
	// colour is what makes a missing LoD chunk show near-field terrain rather than sky.
	// lod.frag discards on the complementary side of the same threshold at the same
	// resolution on the same pixel grid, so every band pixel belongs to exactly one field.
	float d = distance(hp.xyz, pc.cam.xyz);
	float t = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	if (bayer4(ivec2(gl_FragCoord.xy)) < t) {
		#ifdef SEAM_MARKER
		marker = 0u;
		#endif
		gl_FragDepth = 0.0;
		return;
	}
	#ifdef SEAM_MARKER
	marker = 1u;
	#endif
	vec4 clip = pc.view_proj * vec4(hp.xyz, 1.0);
	// The scene projection already outputs NDC z in [0,1] with near->1, far->0 (reverse-Z),
	// so no *0.5+0.5 remap: that would both compress the range to [0.5,1.0] and invert the
	// convention (brief's GL-style [-1,1] assumption does not hold on Godot 4.7.1).
	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
