#[fragment]
#version 460

#include "generated/blocks.glslh"
#include "common.glslh"
#include "shade.glslh"
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 7
#include "beauty_camera.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in flat uint v_material;
layout(location = 3) in flat uint v_near;

// The nearest front face: xy octahedral normal, z distance from the camera, w material id
// (0 = no front). This pass owns its depth attachment, so the nearest fragment wins.
layout(location = 0) out vec4 out_front;

layout(set = 0, binding = 6) uniform sampler2D gb_depth;
layout(push_constant, std430) uniform Push { SHELL_RASTER_PUSH_FIELDS } pc;

void main() {
	float d = distance(v_wpos, bcam.cam.xyz);
	float t_fade = clamp((d - pc.fade.x) / max(pc.fade.y - pc.fade.x, 1e-3), 0.0, 1.0);
	bool far_keeps = bayer4(ivec2(gl_FragCoord.xy)) < t_fade;
	if ((v_near != 0u) == far_keeps) discard;
	// Reverse-Z: larger is nearer. Anything the G-buffer holds in front of this fragment --
	// grass, leaves, terrain -- hides it.
	if (texelFetch(gb_depth, ivec2(gl_FragCoord.xy), 0).r > gl_FragCoord.z) discard;
	out_front = vec4(oct_encode(normalize(v_normal)), d, float(v_material));
}