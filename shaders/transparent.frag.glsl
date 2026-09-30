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

// The far front layer; the same layout raymarch.comp.glsl writes for the near field.
layout(location = 0) out vec4 out_front; // xy oct normal, z distance, w material
layout(location = 1) out vec4 out_trans; // rgb transmittance, a = -1: read the sun map

layout(set = 0, binding = 6) uniform sampler2D gb_depth;
layout(push_constant, std430) uniform Push { TRANSPARENT_RASTER_PUSH_FIELDS } pc;

void main() {
	float d = distance(v_wpos, bcam.cam.xyz);
	float t_fade = clamp((d - pc.fade.x) / max(pc.fade.y - pc.fade.x, 1e-3), 0.0, 1.0);
	// The far field's half of the fade band, exactly as lod.frag.glsl keeps it; the composite
	// keeps the near front on the complement.
	if (bayer4(ivec2(gl_FragCoord.xy)) >= t_fade) discard;
	// Reverse-Z: larger is nearer. Anything the G-buffer holds in front of this fragment --
	// grass, leaves, terrain -- hides it.
	float scene = texelFetch(gb_depth, ivec2(gl_FragCoord.xy), 0).r;
	if (scene > gl_FragCoord.z) discard;
	// Thickness is the distance to the surface behind (spec §5, with its known ceiling: an
	// air gap behind the ice counts as ice). Sky behind is endless ice, so T goes to zero and
	// a far shell against the sky shows its body.
	float thickness = 1e4;
	if (scene > 0.0) {
		vec3 behind = beauty_world_from_depth(gl_FragCoord.xy * bcam.screen.zw, scene);
		thickness = max(distance(behind, bcam.cam.xyz) - d, 0.0);
	}
	out_front = vec4(oct_encode(normalize(v_normal)), d, float(v_material));
	out_trans = vec4(pow(mat_transmit(v_material), vec3(thickness)), -1.0);
}
