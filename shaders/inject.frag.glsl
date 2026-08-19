#[fragment]
#version 460
layout(location = 0) in vec2 uv_in;
layout(location = 0) out vec4 frag_color;
layout(set = 0, binding = 0) uniform sampler2D lit_tex;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
void main() {
	frag_color = vec4(texture(lit_tex, uv_in).rgb, 1.0);
	gl_FragDepth = texelFetch(gb_depth, ivec2(gl_FragCoord.xy), 0).r;
}
