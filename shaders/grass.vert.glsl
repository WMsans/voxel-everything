#[vertex]
#version 460

#include "common.glslh"
#include "shade.glslh"
#include "grass.glslh"

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
	// directionality is what makes a field read as a field. side is perpendicular to both.
	vec3 lean_dir = normalize(vec3(cos(lean), 0.0, sin(lean)) -
			up * dot(vec3(cos(lean), 0.0, sin(lean)), up));
	vec3 side = normalize(cross(up, lean_dir));

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
	vec3 p = root + up * rise + lean_dir * (reach + sway * t * t);

	// Distance width compensation: the far rings halve their blade count, so blades widen
	// to hold coverage flat. Without this the field visibly thins and then falls off a
	// cliff, which is exactly what it used to do.
	float d = distance(root, push.cam.xyz);
	float width = pc.blade.x * grass_width_scale(d, pc.cam.w, pc.shape.w);

	// View-space thickening (Ghost of Tsushima): as the blade turns edge-on, widen it so a
	// sub-pixel sliver does not vanish. view_edge is 0 face-on, 1 edge-on.
	vec3 to_cam = normalize(push.cam.xyz - root);
	float view_edge = 1.0 - abs(dot(side, to_cam));
	width *= mix(1.0, 2.5, view_edge);

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
