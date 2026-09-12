#[vertex]
#version 460

#include "common.glslh"
#include "shade.glslh"
#include "grass.glslh"
#include "grass_tilt.glslh"

// No vertex buffer and no vertex attributes: geometry is PULLED, exactly as lod.vert.glsl
// does. gl_VertexIndex / 9 is the blade, % 9 the corner. This also routes around Godot
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

// Nine vertices: a quad from the root to kSplit (two triangles) plus a tip triangle that
// converges to a point. Three would only ever draw a straight sliver -- the whole reason
// the old blades read as spikes is that a single triangle cannot arc.
const float kSplit = 0.6;
const float kT[9] = float[9](0.0, 0.0, kSplit, 0.0, kSplit, kSplit, kSplit, kSplit, 1.0);
const float kU[9] = float[9](-1.0, 1.0, -1.0, 1.0, 1.0, -1.0, -1.0, 1.0, 0.0);

void main() {
	uint vi = uint(gl_VertexIndex);
	uint blade_index = vi / 9u;
	uint corner = vi % 9u;
	GrassBlade blade = instances.b[blade_index];

	vec3 root = blade.a.xyz;
	float height = blade.a.w;
	vec3 ground_n = oct_decode_snorm8(uint(blade.b.x));
	float lean = blade.b.z;

	// Growth axis and shading normal are DIFFERENT vectors, and conflating them is a bug.
	// Grass grows towards the sky, not along the slope: a blade that took the ground normal
	// as its axis lay right over on a hillside and drew a long diagonal spike, shaded dark
	// because that normal pointed away from the sun. Biasing the axis towards world up keeps
	// blades standing on a slope; ground_n is still what gets written to the GBuffer below.
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

	float t = kT[corner];
	float u = kU[corner];
	uint hash = floatBitsToUint(blade.b.y);

	// Only the tip moves (BotW). Three layers: the gust field gives the waves crossing the
	// meadow, a per-blade sine keeps neighbours out of phase, and a high-frequency term
	// jitters the very tip.
	float gust = grass_gust(root.xz, pc.wind.w, pc.wind.y, pc.wind.z);
	float phase = grass_unit(hash) * 6.2831853;
	float bob = sin(pc.wind.w * 2.3 + phase) * 0.25;
	float jitter = sin(pc.wind.w * 11.0 + phase * 3.0) * 0.06;
	float sway = pc.wind.x * (gust + bob + jitter);

	// The static arc. Grass LIES OVER; it does not stand up and wobble. base_curve is how
	// far the tip travels horizontally as a fraction of height, and the matching shortening
	// of the vertical rise is what keeps the blade a constant length instead of stretching.
	float curve = pc.shape.z * mix(0.75, 1.25, grass_unit(grass_hash(hash ^ 0x68E31DA4u)));
	float rise = height * t * (1.0 - 0.30 * curve * t);
	float reach = height * curve * t * t;

	// pow(t, 2) on the sway too, so the base stays planted while the tip travels.
	vec3 p = root + growth * rise + arc_dir * (reach + sway * t * t);

	// Distance width compensation: the far rings halve their blade count, so blades widen
	// to hold coverage flat. Without this the field visibly thins and then falls off a
	// cliff, which is exactly what it used to do.
	float d = distance(root, push.cam.xyz);
	float width = pc.blade.x * grass_width_scale(d, pc.cam.w, pc.shape.w);

	// There used to be a view-space thickening term here -- mix(1.0, 2.5, 1 - |dot(side,
	// to_cam)|) -- to rescue blades that had turned edge-on. The billboarded width axis
	// above makes side perpendicular to to_cam by construction, so that factor is now
	// identically 1.0. It is deleted rather than left in: a blade can no longer go edge-on,
	// so there is nothing left for it to rescue.

	// Taper to the point. The tip vertex carries u = 0, so it converges regardless; this
	// narrows the shoulders on the way up so the silhouette is a blade, not a plank.
	p += side * (u * width * 0.5 * (1.0 - 0.55 * t));

	v_wpos = p;
	// The ground normal, unmodified, for every vertex of every blade. This is the BotW
	// trick: blades inherit the surface they grow from, so a meadow lights as ONE smooth
	// surface. Giving each blade its own splayed normals -- which is what this shader used
	// to do -- lights every blade independently and turns the field into visual noise.
	// It also keeps grass honest underground: the deferred pass owns all lighting, so a
	// blade in a cave is lit like the cave floor, with no sun term of its own.
	v_normal = ground_n;
	v_height_t = t;
	v_clump = blade.b.w;
	v_hash = hash;
	gl_Position = push.view_proj * vec4(p, 1.0);
}
