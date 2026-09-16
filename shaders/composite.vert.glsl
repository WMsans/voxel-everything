#[vertex]
#version 460
#include "generated/blocks.glslh"
// Must match the fragment stage's block exactly: Godot rejects differing push-constant
// reflections between stages in the same pipeline.
layout(push_constant, std430) uniform Push { COMPOSITE_PUSH_FIELDS } pc;
layout(location = 0) out vec2 uv_out;
void main() {
	// fullscreen triangle from vertex index: covers screen, uv in [0,1] on-screen
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	uv_out = p;
	gl_Position = vec4(p.x * 2.0 - 1.0, p.y * 2.0 - 1.0, 0.0, 1.0);
}
