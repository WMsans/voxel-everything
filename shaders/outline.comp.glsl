#[compute]
#version 460
#define BEAUTY_CAMERA_SET 0
#define BEAUTY_CAMERA_BINDING 6
#include "shade.glslh"
#include "beauty_camera.glslh"
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1) uniform sampler2D gb_depth;
layout(set = 0, binding = 2) uniform sampler2D gb_surface;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness;
layout(set = 0, binding = 4, rgba16f) uniform image2D scene_color;
layout(push_constant, std430) uniform Push {
	ivec4 dims;  // xy full size, z have normal-roughness
	vec4 params; // relative depth threshold, normal threshold, darken, unused
} pc;
struct SurfaceSample { float depth; float linear_depth; vec3 n; int kind; };
SurfaceSample read_surface(ivec2 px) {
	SurfaceSample s; s.depth = texelFetch(scene_depth, px, 0).r;
	s.linear_depth = 0.0; s.n = vec3(0.0); s.kind = 0;
	if (s.depth <= 0.0) return s;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	s.linear_depth = distance(beauty_world_from_depth(uv, s.depth), bcam.cam.xyz);
	float gd = texelFetch(gb_depth, px, 0).r;
	vec4 g = texelFetch(gb_surface, px, 0);
	if (g.z >= 0.5 && abs(gd - s.depth) <= 1e-5) {
		s.n = oct_decode(g.xy); s.kind = 1;
	} else if (pc.dims.z != 0) {
		s.n = normalize(texelFetch(normal_roughness, px, 0).rgb * 2.0 - 1.0);
		if (!isnan(s.n.x) && !isnan(s.n.y) && !isnan(s.n.z) &&
				!isinf(s.n.x) && !isinf(s.n.y) && !isinf(s.n.z)) s.kind = 2;
	}
	return s;
}
bool edge(SurfaceSample a, SurfaceSample b) {
	if (a.depth <= 0.0) return false;
	if (b.depth <= 0.0) return true;
	float rel = abs(a.linear_depth - b.linear_depth) /
		max(min(a.linear_depth, b.linear_depth), 1e-3);
	if (rel > pc.params.x) return true;
	return a.kind != 0 && a.kind == b.kind && 1.0 - dot(a.n, b.n) > pc.params.y;
}
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	SurfaceSample c = read_surface(px); bool e = false;
	if (px.x + 1 < pc.dims.x) e = edge(c, read_surface(px + ivec2(1, 0)));
	if (!e && px.y + 1 < pc.dims.y) e = edge(c, read_surface(px + ivec2(0, 1)));
	if (!e) return;
	vec4 color = imageLoad(scene_color, px);
	imageStore(scene_color, px, vec4(color.rgb * pc.params.z, color.a));
}
