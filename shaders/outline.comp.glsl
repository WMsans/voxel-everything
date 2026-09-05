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
// The steepest incidence the depth threshold is widened for. cos(87.1 deg): past that the
// tolerance stops growing, so a true silhouette seen almost edge-on is still an edge.
const float OUTLINE_MIN_NDV = 0.05;

struct SurfaceSample { float depth; float linear_depth; vec3 n; vec3 view; int kind; bool solid; };
SurfaceSample read_surface(ivec2 px) {
	SurfaceSample s; s.depth = texelFetch(scene_depth, px, 0).r;
	s.linear_depth = 0.0; s.n = vec3(0.0); s.view = vec3(0.0, 0.0, 1.0); s.kind = 0;
	vec4 g = texelFetch(gb_surface, px, 0);
	// Material 0 is "no voxel here", so the g-buffer -- not the scene depth -- is what says
	// whether a pixel has a surface. The near/far dither seam leaves pixels whose depth was
	// dropped by BOTH fields while their g-buffer surface survives; those are holes in the
	// seam, not background, and must not be read as silhouettes.
	s.solid = g.z >= 0.5;
	if (s.depth <= 0.0) return s;
	vec2 uv = (vec2(px) + 0.5) / vec2(pc.dims.xy);
	vec3 wpos = beauty_world_from_depth(uv, s.depth);
	s.linear_depth = distance(wpos, bcam.cam.xyz);
	s.view = s.linear_depth > 1e-6 ? (wpos - bcam.cam.xyz) / s.linear_depth
			: vec3(0.0, 0.0, 1.0);
	float gd = texelFetch(gb_depth, px, 0).r;
	if (s.solid && abs(gd - s.depth) <= 1e-5) {
		s.n = oct_decode(g.xy); s.kind = 1;
	} else if (pc.dims.z != 0) {
		s.n = normalize(texelFetch(normal_roughness, px, 0).rgb * 2.0 - 1.0);
		if (!isnan(s.n.x) && !isnan(s.n.y) && !isnan(s.n.z) &&
				!isinf(s.n.x) && !isinf(s.n.y) && !isinf(s.n.z)) s.kind = 2;
	}
	return s;
}
// One pixel of screen motion walks a FLAT surface further along the view ray the more
// edge-on it is seen, by 1 / |n . v|. A fixed relative threshold therefore fires on every
// grazing slope -- most of the ground below the horizon -- and speckles it with dots.
// Scaling the tolerance by the same factor keeps the test measuring depth DISCONTINUITY
// rather than incidence angle.
bool depth_break(SurfaceSample a, SurfaceSample b) {
	float ndv = a.kind != 0 ? max(abs(dot(a.n, a.view)), OUTLINE_MIN_NDV) : 1.0;
	float rel = abs(a.linear_depth - b.linear_depth) /
		max(min(a.linear_depth, b.linear_depth), 1e-3);
	return rel > pc.params.x / ndv;
}

// A neighbour with neither depth nor material is the sky -- or it is a hole the far field's
// raster left behind. Where two LoD levels meet on a chunk face the two chunks contour
// DIFFERENT lattices, so their quads do not share corners and the seam between them opens by
// a few centimetres; projected, that is a gap narrower than a pixel, which the rasteriser
// resolves as the odd missing pixel scattered along the boundary. In the G-buffer such a
// pixel is byte-for-byte a sky pixel, and reading it as a silhouette is what drew a dashed
// dark line along every level boundary in the far field -- the artifact was invisible with
// outlines off, because nothing but this pass was reacting to it.
//
// Width is what separates the two: sky keeps being sky, while a one-pixel hole has the SAME
// surface back at a continuous depth immediately behind it. So look one pixel further in the
// same direction and let the far side arbitrate. Off the edge of the frame there is nothing
// to ask, so the pixel keeps its silhouette.
bool background_continues(SurfaceSample a, ivec2 px, ivec2 step) {
	ivec2 far_px = px + step * 2;
	if (any(greaterThanEqual(far_px, pc.dims.xy))) return true;
	SurfaceSample c = read_surface(far_px);
	if (c.depth <= 0.0) return true;   // still nothing behind it: a real silhouette
	return depth_break(a, c);          // a different surface, so a real silhouette too
}

bool edge(SurfaceSample a, SurfaceSample b, ivec2 px, ivec2 step) {
	if (a.depth <= 0.0) return false;
	if (b.depth <= 0.0) return !b.solid && background_continues(a, px, step);
	if (depth_break(a, b)) return true;
	return a.kind != 0 && a.kind == b.kind && 1.0 - dot(a.n, b.n) > pc.params.y;
}
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, pc.dims.xy))) return;
	SurfaceSample c = read_surface(px); bool e = false;
	if (px.x + 1 < pc.dims.x)
		e = edge(c, read_surface(px + ivec2(1, 0)), px, ivec2(1, 0));
	if (!e && px.y + 1 < pc.dims.y)
		e = edge(c, read_surface(px + ivec2(0, 1)), px, ivec2(0, 1));
	if (!e) return;
	vec4 color = imageLoad(scene_color, px);
	imageStore(scene_color, px, vec4(color.rgb * pc.params.z, color.a));
}
