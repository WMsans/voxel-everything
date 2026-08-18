#[vertex]
#version 460
// Must match the fragment stage's block exactly: Godot rejects differing push-constant
// reflections between stages in the same pipeline.
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;  // xyz = camera position, w = fade start
	vec4 fade; // x = fade end, yzw unused
} pc;
layout(location = 0) out vec2 uv_out;
void main() {
	// fullscreen triangle from vertex index: covers screen, uv in [0,1] on-screen
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	uv_out = p;
	gl_Position = vec4(p.x * 2.0 - 1.0, p.y * 2.0 - 1.0, 0.0, 1.0);
}
