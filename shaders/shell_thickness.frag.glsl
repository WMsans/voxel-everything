#[fragment]
#version 460

#include "generated/blocks.glslh"
#include "common.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 7
#include "beauty_camera.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in flat uint v_material;
layout(location = 3) in flat uint v_near;

// Additive. R = sum of back-face distances minus front-face distances; G = fronts minus
// backs. thickness = R + G * z_opaque (ve::shell_thickness): G counts the entries with no
// matching exit, i.e. the rays that end on opaque ground inside the medium. Depth-tested
// against the G-buffer, so a face behind the opaque surface contributes nothing.
layout(location = 0) out vec4 out_thick;

layout(push_constant, std430) uniform Push { SHELL_RASTER_PUSH_FIELDS } pc;

void main() {
	float d = distance(v_wpos, bcam.cam.xyz);
	float t_fade = clamp((d - pc.fade.x) / max(pc.fade.y - pc.fade.x, 1e-3), 0.0, 1.0);
	// lod.frag.glsl keeps the far field where bayer < t; the near shell keeps the complement.
	// Every face therefore survives in exactly one shell, which keeps the counts exact.
	bool far_keeps = bayer4(ivec2(gl_FragCoord.xy)) < t_fade;
	if ((v_near != 0u) == far_keeps) discard;
	float s = gl_FrontFacing ? -1.0 : 1.0;
	out_thick = vec4(s * d, -s, 0.0, 0.0);
}