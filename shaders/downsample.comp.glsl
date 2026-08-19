#[compute]
#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba16f) writeonly uniform image2D dst;
layout(push_constant, std430) uniform Push { ivec4 dims; } pc;

// The finished frame, halved, becomes next frame's one-bounce GI source.
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	imageStore(dst, px, vec4(texture(src, uv).rgb, 1.0));
}
