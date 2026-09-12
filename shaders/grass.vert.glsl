#[vertex]
#version 460

#include "common.glslh"
#include "shade.glslh"
#include "grass.glslh"
#include "grass_tilt.glslh"
#include "grass_blade.glslh"

// No vertex buffer and no vertex attributes: geometry is PULLED, exactly as lod.vert.glsl
// does. gl_VertexIndex / 27 is the blade, % 27 the corner. This also routes around Godot
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
layout(location = 3) out float v_clump;
layout(location = 4) out flat uint v_hash;
layout(location = 5) out flat float v_sun; // terrain sun visibility the scatter marched

// Twenty-seven vertices: four quads up the Bezier profile plus a tip triangle. The two
// segments this used to have could only draw a card with a kink in it; a bend needs enough
// joints to read as a curve. Rows sit closer together towards the tip, where the curvature
// collects (grass_blade.glslh puts it there).
const float kRow[6] = float[6](0.0, 0.32, 0.58, 0.79, 0.93, 1.0);
// Corner k of a quad: which row (0 = lower, 1 = upper) and which edge (-1 / +1).
const uint kQuadRow[6] = uint[6](0u, 0u, 1u, 0u, 1u, 1u);
const float kQuadU[6] = float[6](-1.0, 1.0, -1.0, 1.0, 1.0, -1.0);

void main() {
	uint vi = uint(gl_VertexIndex);
	uint blade_index = vi / 27u;
	uint corner = vi % 27u;
	GrassBlade blade = instances.b[blade_index];

	vec3 root = blade.a.xyz;
	float height = blade.a.w;
	vec3 ground_n = oct_decode_snorm8(grass_ground_oct(blade.b.x));
	uint hash = floatBitsToUint(blade.b.y);

	// Wind, sampled once per blade. The gust field is the waves crossing the meadow; it is
	// re-centred so a lull swings a blade BACK past its rest lie instead of only ever pushing
	// it one way, which is half of what made the old field look stiff. A per-blade bob at a
	// per-blade rate keeps neighbours out of step, and a fast small term flutters it.
	float gust = grass_gust(root.xz, pc.wind.w, pc.wind.y, pc.wind.z);
	float phase = grass_unit(hash) * 6.2831853;
	float rate = mix(1.7, 2.9, grass_unit(grass_hash(hash ^ 0x9E3779B9u)));
	float bob = sin(pc.wind.w * rate + phase) * 0.35;
	float flutter = sin(pc.wind.w * 9.0 + phase * 3.0) * 0.08;
	float sway = pc.wind.x * (gust * 1.4 - 0.3 + bob + flutter);

	// Gusts also swing the direction a blade lies in a little, so a wave visibly combs the
	// field rather than only nodding it.
	float lean = blade.b.z + (gust - 0.5) * 0.35;

	// Growth axis and shading normal are DIFFERENT vectors, and conflating them is a bug.
	// Grass grows towards the sky, not along the slope: a blade that took the ground normal
	// as its axis lay right over on a hillside and drew a long diagonal spike, shaded dark
	// because that normal pointed away from the sun. Biasing the axis towards world up keeps
	// blades standing on a slope; ground_n is still what the shading normal is anchored to.
	vec3 up = normalize(mix(ground_n, vec3(0.0, 1.0, 0.0), 0.6));

	// The blade leans in its OWN direction rather than facing the camera; that
	// directionality is what makes a field read as a field.
	vec3 lean_dir = normalize(vec3(cos(lean), 0.0, sin(lean)) -
			up * dot(vec3(cos(lean), 0.0, sin(lean)), up));

	// The width axis is NOT cross(up, lean_dir), and that distinction is the whole reason
	// this field used to look good from one compass direction and bald from the rest. The
	// scatter leans every blade within lean_spread_rad of wind_dir_deg, so a lean-locked
	// width axis makes every quad in the meadow face the SAME way: broadside from one
	// azimuth, a sub-pixel sliver from ninety degrees off, with bare ground between.
	//
	// So billboard the width axis about the blade's own up: the blade always turns its face
	// to the viewer while the arc below still travels along lean_dir, which is where the
	// wind coherence actually lives. This is the Ghost of Tsushima / BotW treatment -- it
	// costs one cross product and no extra vertices.
	//
	// to_cam is built from root, not from the vertex, so all nine vertices agree and the
	// quad stays planar. Degenerate only when the camera is directly overhead, where the
	// flattened view vector vanishes and any azimuth is as good as another; take the old
	// lean-locked axis there rather than normalizing a zero vector. The tilt below leans
	// along that same axis, so one fallback keeps both the width axis and the arc defined.
	vec3 to_cam = normalize(push.cam.xyz - root);
	vec3 view_h = to_cam - up * dot(to_cam, up);
	vec3 toward = length(view_h) > 1e-3 ? normalize(view_h) : lean_dir;
	vec3 side = normalize(cross(up, toward));

	// Camera tilt: the pitch half of the same problem the billboard above solves for
	// azimuth. Billboarded, the width axis is always perpendicular to the view, but the
	// card's face still lies in the vertical plane through the camera, so the area it
	// presents goes as the cosine of the camera's elevation -- full at eye level, half at 60
	// degrees, a sub-pixel sliver straight overhead, where the gaps between blades open into
	// bare ground and the field reads as spikes instead of canopy. Leaning the growth axis
	// away from the viewer by the camera's own elevation turns the face up onto the view
	// direction and holds the eye-level area at every pitch. grass_tilt.glslh carries the
	// maths and its proof; the native suite executes that file directly.
	float elevation = grass_camera_elevation(to_cam, up);
	vec3 growth = grass_tilted_up(up, toward, elevation, pc.style.z);
	// The arc is projected back into the blade's own plane -- now spanned by `growth` rather
	// than by `up` -- so the card stays flat. Left along lean_dir it would fold the quad into
	// a bowtie the moment the blade leaned, and a folded quad has no single face normal.
	vec3 arc_dir = grass_plane_arc(lean_dir, growth, side);

	float t;
	float u;
	if (corner < 24u) {
		uint quad = corner / 6u;
		uint k = corner % 6u;
		t = kRow[quad + kQuadRow[k]];
		u = kQuadU[k];
	} else {
		t = corner == 26u ? 1.0 : kRow[4];
		u = corner == 24u ? -1.0 : (corner == 25u ? 1.0 : 0.0);
	}

	// Grass LIES OVER; it does not stand up and wobble. The static lie (base_curve, jittered
	// per blade) and the wind's sway are one bend angle, and the profile keeps its arc length
	// at every bend -- so a gust bows the blade and its tip drops, instead of the tip sliding
	// sideways on a stretching card the way it used to.
	float curve = pc.shape.z * mix(0.75, 1.25, grass_unit(grass_hash(hash ^ 0x68E31DA4u)));
	float bend = grass_wind_bend(curve, sway, height);
	vec2 prof = grass_blade_point(t, height, bend);
	vec3 p = root + growth * prof.y + arc_dir * prof.x;

	// Distance width compensation: the far rings halve their blade count, so blades widen
	// to hold coverage flat. Without this the field visibly thins and then falls off a
	// cliff, which is exactly what it used to do.
	float d = distance(root, push.cam.xyz);
	float width = pc.blade.x * grass_width_scale(d, pc.cam.w, pc.shape.w);

	// Taper to the point. The tip vertex carries u = 0, so it converges regardless; this
	// narrows the shoulders on the way up so the silhouette is a blade, not a plank.
	p += side * (u * width * 0.5 * (1.0 - 0.55 * t));

	// Lighting. Every blade used to write the bare ground normal, which put the whole meadow
	// in ONE cel band: no lit side, no shade side, a field that read as flat paint. The blade
	// now writes its own Bezier face normal, rounded across the width, blended against the
	// ground normal (grass_blade.glslh) -- by blade_lighting near the camera, fading to the
	// pure ground normal by the reach, where per-blade shading only aliases into noise.
	// The face is turned to the viewer: that is the side of the card being seen.
	vec2 tan2 = grass_blade_tangent(t, bend);
	vec3 tangent = normalize(growth * tan2.y + arc_dir * tan2.x);
	vec3 face = normalize(cross(side, tangent));
	if (dot(face, to_cam) < 0.0) face = -face;
	float blend = pc.style.w * (1.0 - smoothstep(0.35 * pc.cam.w, pc.cam.w, d));

	v_wpos = p;
	v_normal = grass_shading_normal(ground_n, face, side, u, t, blend);
	v_height_t = t;
	v_clump = blade.b.w;
	v_hash = hash;
	v_sun = grass_ground_sun(blade.b.x);
	gl_Position = push.view_proj * vec4(p, 1.0);
}
