#[vertex]
#version 460

#include "common.glslh"
#include "leaf.glslh"
#include "wind.glslh"

// No vertex buffer and no vertex attributes: geometry is PULLED, exactly as grass draws.
// gl_VertexIndex / 6 is the clump, % 6 the corner. This also routes around Godot exposing
// neither gl_DrawID nor a non-zero firstInstance.
layout(set = 0, binding = 0, std430) readonly buffer Instances { LeafClump c[]; } instances;
layout(set = 0, binding = 1, std140) uniform Params { LEAF_PARAMS_FIELDS } leaf;
layout(push_constant, std430) uniform Push { mat4 view_proj; vec4 cam; } pc;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out float v_sun;
layout(location = 4) out float v_depth_t;
layout(location = 5) out flat uint v_hash;

void main() {
	uint clump = uint(gl_VertexIndex) / 6u;
	uint corner = uint(gl_VertexIndex) % 6u;
	LeafClump c = instances.c[clump];

	uint packed = floatBitsToUint(c.b.x);
	vec3 n = leaf_unpack_normal(packed);
	v_sun = leaf_unpack_sun(packed);
	vec2 pair = leaf_unpack_pair(c.b.z);
	v_depth_t = pair.x;
	float ratio = pair.y;
	// FLAT, and unpacked here rather than in the fragment. c.b.y is a HASH carried in a
	// float's bit pattern, so a smooth varying is not merely wasteful, it is wrong: the
	// interpolator recomputes it per fragment as a perspective-weighted sum, which lands one
	// ULP off the vertex value and moves as the card re-faces the camera. Reinterpreted as
	// bits, a one-ULP drift is a WHOLE DIFFERENT HASH -- so the per-clump tint jitter below
	// resolved to a different colour from frame to frame, and the scallop threshold to a
	// different edge. That is the subtle colour flicker. grass.vert.glsl has always passed
	// its hash as `flat uint` for this reason; this is the same rule.
	v_hash = floatBitsToUint(c.b.y);

	// Two triangles: 0,1,2 and 2,1,3 over the unit square, mapped to -1..1.
	const vec2 kCorners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1),
	                                 vec2(-1, 1), vec2(1, -1), vec2(1, 1));
	vec2 q = kCorners[corner];
	v_uv = q;

	// Whole-clump sway, amplitude going as depth_t^2 so the base of a crown stays planted
	// while its top moves most. Driven by the SAME gust field grass samples at world XZ, so
	// a gust crosses the meadow and the canopies together.
	//
	// The trunk is voxels and cannot move, so the amplitude is capped by LeafSettings before
	// the canopy visibly detaches from it -- that cap is a settings knob, not a constant here.
	float gust = wind_gust(c.a.xz, leaf.wind.w, leaf.wind.y, leaf.wind.z) * 2.0 - 1.0;
	float phase = c.b.w; // per-clump, so neighbours are never in lockstep
	float sway = leaf.wind.x * v_depth_t * v_depth_t
			* (gust + 0.3 * sin(leaf.wind.w * 1.7 * leaf.wind.y + phase));
	vec3 centre = c.a.xyz + vec3(sway, 0.0, sway * 0.6);

	// Camera-facing with a fixed per-clump ROLL from the hash, and the up axis locked to
	// world Y. A full spherical billboard makes a canopy swim when the camera strafes; the
	// roll is what keeps neighbouring cards from all aligning into a visible grid.
	vec3 to_cam = normalize(pc.cam.xyz - centre);
	// cross(Y, to_cam) collapses to zero when the camera is directly above or below a clump
	// -- looking down into a canopy, which is an ordinary camera position here. normalize()
	// of a near-zero vector is whatever the remaining float noise says, so the card used to
	// spin from frame to frame at exactly those angles. World X is never parallel to Y.
	vec3 axis = cross(vec3(0.0, 1.0, 0.0), to_cam);
	vec3 right = normalize(dot(axis, axis) > 1e-6 ? axis : vec3(1.0, 0.0, 0.0));
	vec3 up = cross(to_cam, right);
	float roll = c.b.w;
	vec3 rr = right * cos(roll) + up * sin(roll);
	vec3 uu = up * cos(roll) - right * sin(roll);

	vec3 world = centre + (rr * q.x + uu * q.y) * c.a.w;
	v_world = world;

	// Perturb the transferred sphere normal across the card. The scatter packs ONE normal per
	// clump; spreading it by the card's own extent, scaled by clump_radius / crown_radius,
	// approximates a true per-fragment transfer from the crown sphere at a thirty-second of
	// the storage. At these card sizes the two are indistinguishable.
	//
	// The spread runs in the CROWN's tangent basis (leaf_card_normal), not in the card's
	// rr/uu, and the result is NOT turned toward the viewer. What was here before did both:
	// it spread the normal in the camera-aligned basis and then ran grass.vert.glsl's
	// `if (dot(sn, to_cam) < 0.0) sn = -sn;`. A grass blade is a real two-sided surface and
	// wants that flip; a card standing in for a VOLUME does not. Negating the normal negates
	// its y, and leaf.frag.glsl reads exactly that y to mix between the bright warm top
	// colour and the deep cool underside -- so every clump crossing dot == 0 swapped between
	// them in a single frame. Orbiting the tree popped whole faces from one colour to the
	// other, and for clumps near the silhouette the wind sway below re-crossed the boundary
	// every frame, which is the rapid flicker. Both symptoms, one line.
	//
	// Nothing replaces the flip: a crown's far side is SUPPOSED to read dark, cel_shade
	// clamps ndv so a back-facing clump lands on the full rim rather than a discontinuity,
	// and cull stays disabled because a card's winding still flips with its roll.
	v_normal = leaf_card_normal(n, q, ratio);

	gl_Position = pc.view_proj * vec4(world, 1.0);
}
