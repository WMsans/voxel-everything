#[fragment]
#version 460
#include "generated/gbuffer.glslh"

#include "generated/blocks.glslh"
#define VE_MATERIAL_ARRAYS
layout(set = 0, binding = 3) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 4) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"

layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in flat uint v_material;

layout(location = 0) out vec4 out_albedo;  // rgb albedo, a = sun visibility
layout(location = 1) out vec4 out_surface; // xy oct normal, z material id, w gloss
#ifdef SEAM_MARKER
layout(location = 2) out uint marker; // debug seam probe: 2 = far field kept the pixel
#endif

layout(push_constant, std430) uniform Push { LOD_RASTER_PUSH_FIELDS } pc;
// The near field's region map (brick_atlas.glslh's region_slot_of, by region). A region enters
// it only once all its bricks are allocated, so a region it does not hold yet is one the
// marcher crosses as empty.
layout(set = 0, binding = 9, std430) readonly buffer RegionMap { int slot[]; } region_map;

// Does the near field draw this point yet? pc.region.w = 0: no window, assume it does.
bool near_field_ready(vec3 p) {
	int dim = pc.region.w;
	if (dim == 0) return true;
	ivec3 r = ivec3(floor(p / REGION_SIZE));
	ivec3 l = r - pc.region.xyz;
	if (any(lessThan(l, ivec3(0))) || any(greaterThanEqual(l, ivec3(dim)))) return false;
	ivec3 w = r & (dim - 1);
	return region_map.slot[w.x + w.y * dim + w.z * dim * dim] >= 0;
}

void main() {
	// Fade before the material sample, so a discarded fragment costs no texture work.
	float d = distance(v_wpos, pc.cam.xyz);
	float t = clamp((d - pc.cam.w) / max(pc.fade.x - pc.cam.w, 1e-3), 0.0, 1.0);
	// The exact complement of composite.frag's test: >= where it uses <. Where the near field
	// has not streamed in, it has nothing to hand over, so the far field keeps the pixel; a
	// near hit there is impossible (its region is not in the map), so nothing is drawn twice.
	if (bayer4(ivec2(gl_FragCoord.xy)) >= t && near_field_ready(v_wpos)) discard;
#ifdef SEAM_MARKER
	marker = 2u;
#endif
	// Explicit gradients are not needed here -- a fragment shader has them -- but the SAME
	// functions the raymarcher calls are, so the two fields cannot drift.
	vec3 ddx = dFdx(v_wpos);
	vec3 ddy = dFdy(v_wpos);
	vec3 geometric_n = normalize(v_normal);
	vec4 surf = material_surface(v_material, v_wpos, geometric_n, ddx, ddy);
	// The far field needs the normal map more than the near field does: v_normal is flat
	// across a whole LoD quad, so without it a distant hillside is one unbroken facet. Same
	// call, same arguments as composite.frag.glsl, so the two fields cannot drift.
	vec3 shading_n;
	vec2 props = material_props_normal(v_material, v_wpos, geometric_n, ddx, ddy, shading_n);
	// Sun visibility is 1: shadowing the far field is the ortho shadow map's job, evaluated
	// once in the deferred pass where the near field's raymarched term is also applied.
	out_albedo = GB_PACK_ALBEDO(surf.rgb * mix(1.0, props.y, 0.65), 1.0);
	out_surface = GB_PACK_SURFACE(shading_n, v_material, 1.0 - props.x);
}
