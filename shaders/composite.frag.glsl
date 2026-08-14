#version 460
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0) uniform sampler2D src_color;  // linear (0.66x upscale)
layout(set = 0, binding = 1) uniform sampler2D src_hitpos; // nearest (no silhouette smear)
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
} pc;
void main() {
	vec4 hp = texture(src_hitpos, uv_in);
	frag_color = texture(src_color, uv_in);
	if (hp.w < 0.5) {
		// Sky: farthest. Godot 4.7 renders with reverse-Z (near=1.0, far=0.0 — the scene
		// projection is depth-corrected with remap_z+reverse_z), so the farthest depth is
		// 0.0, and the composite pipeline tests GREATER_OR_EQUAL so nearer scene geometry
		// (opaque objects' pre-pass depth) is never overwritten.
		gl_FragDepth = 0.0;
		return;
	}
	vec4 clip = pc.view_proj * vec4(hp.xyz, 1.0);
	// The scene projection already outputs NDC z in [0,1] with near->1, far->0 (reverse-Z),
	// so no *0.5+0.5 remap: that would both compress the range to [0.5,1.0] and invert the
	// convention (brief's GL-style [-1,1] assumption does not hold on Godot 4.7.1).
	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
