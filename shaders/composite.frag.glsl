#[fragment]
#version 460
#define MATERIAL_LAYERS 16
layout(set = 0, binding = 2) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 3) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
layout(location = 0) in vec2 uv_in;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_surface;
#ifdef SEAM_MARKER
layout(location = 2) out uint marker;
#endif

layout(set = 0, binding = 0) uniform sampler2D src_albedo;
layout(set = 0, binding = 1) uniform sampler2D src_hitpos;
layout(set = 0, binding = 4) uniform sampler2D src_surface;
layout(push_constant, std430) uniform Push {
	mat4 view_proj;
	vec4 cam;
	vec4 fade;
} pc;

void main() {
	vec4 hp = texture(src_hitpos, uv_in);
	out_albedo = texture(src_albedo, uv_in);
	out_surface = texture(src_surface, uv_in);
#ifdef SEAM_MARKER
	marker = 1u;
#endif
	if (hp.w < 0.5) {
#ifdef SEAM_MARKER
		marker = 0u;
#endif
		gl_FragDepth = 0.0;
		return;
	}
	float d = distance(hp.xyz, pc.cam.xyz);
	float t = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	if (bayer4(ivec2(gl_FragCoord.xy)) < t) {
#ifdef SEAM_MARKER
		marker = 0u;
#endif
		gl_FragDepth = 0.0;
		return;
	}
	vec4 clip = pc.view_proj * vec4(hp.xyz, 1.0);
	gl_FragDepth = clamp(clip.z / clip.w, 0.0, 1.0);
}
