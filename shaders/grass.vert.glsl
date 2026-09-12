#[vertex]
#version 460

#include "common.glslh"
#include "shade.glslh"
#include "grass.glslh"

// No vertex buffer and no vertex attributes: geometry is PULLED, exactly as lod.vert.glsl
// does. gl_VertexIndex / 3 is the blade, % 3 the corner. This also routes around Godot
// exposing neither gl_DrawID nor a non-zero firstInstance.
layout(set = 0, binding = 0, std430) readonly buffer Instances { GrassBlade b[]; } instances;
layout(set = 0, binding = 1, std140) uniform Params { GRASS_PARAMS_BLOCK } pc;

layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam; // xyz camera position, w unused
} push;

layout(location = 0) out vec3 v_wpos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out float v_height_t; // 0 at the root, 1 at the tip

void main() {
	uint vi = uint(gl_VertexIndex);
	uint blade_index = vi / 3u;
	uint corner = vi % 3u;
	GrassBlade blade = instances.b[blade_index];

	vec3 root = blade.a.xyz;
	float height = blade.a.w;
	vec3 up = oct_decode_snorm8(uint(blade.b.x));
	float lean = blade.b.z;

	// The blade leans in its OWN direction rather than facing the camera; that
	// directionality is what makes a field read as a field. side is perpendicular to both.
	vec3 lean_dir = normalize(vec3(cos(lean), 0.0, sin(lean)) -
			up * dot(vec3(cos(lean), 0.0, sin(lean)), up));
	vec3 side = normalize(cross(up, lean_dir));

	// View-space thickening (Ghost of Tsushima): as the blade turns edge-on, widen it so a
	// sub-pixel sliver does not vanish. view_edge is 0 face-on, 1 edge-on.
	vec3 to_cam = normalize(push.cam.xyz - root);
	float view_edge = 1.0 - abs(dot(side, to_cam));
	float width = pc.blade.x * mix(1.0, 2.5, view_edge);

	float t = (corner == 2u) ? 1.0 : 0.0;
	vec3 p = root + up * (height * t);
	if (corner != 2u) p += side * (corner == 0u ? -width : width) * 0.5;

	// Roundness without geometry: base normals splay outward, the tip's leans toward the
	// blade's own up. Interpolation then shades a flat triangle as a rounded one.
	vec3 n = (corner == 2u) ? up : normalize(mix(up, (corner == 0u ? -side : side), 0.55));

	v_wpos = p;
	v_normal = n;
	v_height_t = t;
	gl_Position = push.view_proj * vec4(p, 1.0);
}
