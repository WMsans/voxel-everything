#[compute]
#version 460

#define SUN_LIGHT_SET 0
#define SUN_LIGHT_BINDING 10
#define MATERIAL_LAYERS 16
layout(set = 0, binding = 8) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 9) uniform sampler2DArray material_surface_tex;
#include "common.glslh"
#include "shade.glslh"
#include "sun_light.glslh"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gb_albedo;
layout(set = 0, binding = 1) uniform sampler2D gb_surface;
layout(set = 0, binding = 2) uniform sampler2D gb_depth;
layout(set = 0, binding = 3) uniform sampler2D ssgi_tex;
layout(set = 0, binding = 4) uniform sampler2DArray sun_map;
layout(set = 0, binding = 7) uniform sampler2D ssao_tex;
layout(set = 0, binding = 5, rgba16f) writeonly uniform image2D out_lit;
#define SUN_CASCADES 3
layout(set = 0, binding = 6, std140) uniform SunBlock {
	mat4 view_proj[SUN_CASCADES];
	// per cascade: x = one shadow texel in world metres, y = light-space depth range in the
	// same metres; zw on cascade 0 carries the LoD fade band (fade_start, fade_end),
	// unused on the other cascades
	vec4 params[SUN_CASCADES];
	// xyz = the cascade radii; w = the count actually in use (1 when the radius collapsed)
	vec4 splits;
} sun;

layout(push_constant, std430) uniform Push {
	mat4 inv_view_proj;
	vec4 cam;
	vec4 sky;
	uvec4 flags;
} pc;

// The fits are camera-centred SPHERES, so a point at distance d is inside cascade i exactly
// when d < radius_i. Selection is a scalar compare -- no depth-slice arithmetic and no
// split-plane seam to reconcile against the projection. That is what the sphere fit buys.
int sun_cascade_of(float d) {
	int n = int(sun.splits.w);
	for (int i = 0; i < SUN_CASCADES; i++) {
		if (i >= n) break;
		if (d < sun.splits[i]) return i;
	}
	return n - 1;
}

float sun_map_visibility(vec3 wpos, float ndl, float view_dist) {
	int c = sun_cascade_of(view_dist);
	vec4 clip = sun.view_proj[c] * vec4(wpos, 1.0);
	if (clip.w <= 0.0) return 1.0;
	vec3 p = clip.xyz / clip.w;
	vec2 uv = p.xy * 0.5 + 0.5;
	// Outside the outermost cascade's map there is no shadow information, and "lit" is the
	// honest answer -- this is also what makes "beyond the last cascade" need no branch.
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
	float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
	// p.z and the stored depth are normalized [0,1], so the bias must be too. Every term is
	// texel-relative and deliberately so: a cascade spans thousands of metres of depth
	// range, where an absolute bias contributes metres of slop and unseats the stored
	// surface from the ground it rasterized. Per cascade, because the texels differ by ~10x.
	float texel = sun.params[c].x / max(sun.params[c].y, 1e-6);
	// Four texels is the smallest measured receiver bias that removes isolated far-LoD
	// self-shadow specks; slope scaling handles grazing cells without metre-scale bias.
	float bias = texel * (4.0 + 4.0 * slope);
	return (p.z + bias >= texture(sun_map, vec3(uv, float(c))).r) ? 1.0 : 0.0;
}

// Did the LoD mesh draw this pixel? The sun map is rasterized from that mesh and from
// nothing else, so it may only shade the pixels that mesh produced. The near field's surface
// is the FINE field, which sits metres away from the mesh at the cut the far field draws --
// a tent-filtered lattice several cells coarse -- so testing a raymarched pixel against this
// map reports shadow over open sunlit ground, in the shape of the LoD geometry. The near
// field marches its own sun ray instead; that term is already in g0.a.
//
// This is the very dither the two fields divide the screen with: lod.frag.glsl keeps a
// fragment where bayer4(px) < t and composite.frag.glsl drops one there. Reproducing it
// here -- same threshold, same pixel -- lands the map on exactly the pixels it describes.
bool far_field_owns(ivec2 px, vec3 wpos, vec3 viewer) {
	float t = clamp((distance(wpos, viewer) - sun.params[0].z) /
			max(sun.params[0].w - sun.params[0].z, 1e-3), 0.0, 1.0);
	return bayer4(px) < t;
}

// cel_shade ends with `+ vec3(rim)`, where rim = 0.35 * pow(1 - ndv, 3). It is the only
// UNMODULATED WHITE term in the ramp, and the spec is explicit that it is "a stylization of
// silhouette, not a light". pow(1 - ndv, k) is a fair silhouette proxy on a closed, curved
// object: ndv collapses only in the few pixels before the surface turns away.
//
// Open ground breaks the proxy. The normal is up and the view is level, so ndv falls to 0
// with DISTANCE and never recovers -- the rim reaches its full 0.35 across the whole far
// field. Decomposed in linear space, the far field measured +0.153/+0.160/+0.158 over the
// near field: equal in all three channels, which is what identifies additive white rather
// than fog (that would pull toward the sky's blue) or a lighting scale (that would be
// multiplicative). At 4 km of view distance the proxy is wrong over most of the frame.
//
// So ask what a rim actually means rather than what correlates with it: does the surface END
// here? Sky, and anything the LoD fade dropped, write 0.0 into the depth attachment -- it is
// reverse-Z (every pass sets COMPARE_OP_GREATER_OR_EQUAL), so 0.0 is the far plane and
// "background" is a plain compare with no threshold to tune and nothing to drift with
// distance. Receding ground has background nowhere near it and gates to exactly 0, however
// grazing it gets, which is the whole point.
//
// A COUNT over a small ring rather than one tap, because the count is also the softness: a
// pixel on the skyline has half the ring in background, one a few pixels in has none, and the
// rim fades across the band between instead of stopping dead at a single pixel.
const ivec2 kRimGateTaps[12] = ivec2[12](
		ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1),
		ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2),
		ivec2(2, 2), ivec2(-2, 2), ivec2(2, -2), ivec2(-2, -2));

float silhouette_gate(ivec2 px, ivec2 size) {
	float background = 0.0;
	for (int i = 0; i < 12; i++) {
		ivec2 q = clamp(px + kRimGateTaps[i], ivec2(0), size - ivec2(1));
		if (texelFetch(gb_depth, q, 0).r <= 0.0) background += 1.0;
	}
	// Half the ring is background at the silhouette itself, so scale by two to let a true
	// edge still reach full strength.
	return clamp(background * (2.0 / 12.0), 0.0, 1.0);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_lit);
	if (px.x >= size.x || px.y >= size.y) return;

	if (pc.flags.y == 1u) {
		vec3 albedo = pc.inv_view_proj[0].xyz;
		vec3 ambient = pc.inv_view_proj[1].xyz;
		vec4 t = pc.inv_view_proj[2];
		vec2 ag = pc.inv_view_proj[3].xy;
		imageStore(out_lit, px,
				vec4(cel_shade(albedo, ambient, t.x, t.y, t.z, t.w, ag.x, ag.y), 1.0));
		return;
	}

	if (pc.flags.y == 3u) {
		float vis = ((pc.flags.x & BEAUTY_SUN_MAP) != 0u)
				? sun_map_visibility(pc.cam.xyz, 1.0,
						distance(pc.cam.xyz, pc.inv_view_proj[0].xyz)) : 1.0;
		imageStore(out_lit, px, vec4(vis, vis, vis, 1.0));
		return;
	}

	// Probe 4 is probe 3 plus the ownership gate: the term the SHADING path applies at
	// pc.cam.xyz for a viewer at inv_view_proj[0].xyz. Probe 3 stays the raw map so the
	// tests that ask what was rasterized keep asking exactly that.
	if (pc.flags.y == 4u) {
		float vis = ((pc.flags.x & BEAUTY_SUN_MAP) != 0u &&
				far_field_owns(px, pc.cam.xyz, pc.inv_view_proj[0].xyz))
				? sun_map_visibility(pc.cam.xyz, 1.0,
						distance(pc.cam.xyz, pc.inv_view_proj[0].xyz)) : 1.0;
		imageStore(out_lit, px, vec4(vis, vis, vis, 1.0));
		return;
	}

	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec4 g0 = texelFetch(gb_albedo, px, 0);
	vec4 g1 = texelFetch(gb_surface, px, 0);
	uint mat = uint(g1.z + 0.5);
	if (mat == 0u && pc.flags.y != 2u) {
		// Probe 5 reads the rim gate, and background has no surface to gate -- report 0
		// rather than the sky's colour so the readback is the gate and nothing else.
		imageStore(out_lit, px, pc.flags.y == 5u ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(g0.rgb, 1.0));
		return;
	}

	float depth = texelFetch(gb_depth, px, 0).r;
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0);
	vec4 h = pc.inv_view_proj * vec4(ndc, depth, 1.0);
	vec3 wpos = h.xyz / (abs(h.w) < 1e-9 ? 1e-9 : h.w);

	if (pc.flags.y == 2u) {
		imageStore(out_lit, px, vec4(wpos, 1.0));
		return;
	}

	vec3 n = oct_decode(g1.xy);
	vec3 v = normalize(pc.cam.xyz - wpos);
	vec3 sun_dir = sun_light.dir.xyz;
	float ndl = dot(n, sun_dir);
	float ndv = dot(n, v);
	float ndh = dot(n, normalize(sun_dir + v));
	float shadow = g0.a;
	if ((pc.flags.x & BEAUTY_SUN_MAP) != 0u && far_field_owns(px, wpos, pc.cam.xyz))
		shadow = min(shadow, sun_map_visibility(wpos, ndl, distance(wpos, pc.cam.xyz)));
	// HBAO multiplies the SKY term only: sun lighting, spec and rim keep their own
	// visibility terms. The pass's sky pixels are exactly 1.0, so horizons are untouched.
	float ao = 1.0;
	if ((pc.flags.x & BEAUTY_SSAO) != 0u) ao = texture(ssao_tex, uv).r;
	// AO models occlusion of a DISTANT UNIFORM ambient, which is what the sky term is. SSGI
	// is not that: it resolved its own visibility per tap (the sample has to face the
	// receiver and lie inside the radius), so multiplying it by AO counts the same occlusion
	// twice. That double count is why an emissive crack used to be dimmest exactly where it
	// should be brightest -- in the crevice beside it, where AO is deepest. The `ao` handed
	// to cel_shade below is therefore 1.0: it has already been applied, to the only term
	// it belongs to.
	vec3 ambient = pc.sky.rgb * clamp(ao, 0.0, 1.0);
	if ((pc.flags.x & BEAUTY_SSGI) != 0u) ambient += texture(ssgi_tex, uv).rgb;
	// ndv feeds nothing in cel_shade but the rim, so handing it 1.0 (face-on) is exactly
	// "no rim" and leaves every other term of the ramp bit-for-bit untouched. That keeps the
	// three mirrors -- shade.glslh, ve::cel_shade, cel.gdshaderinc -- identical, and keeps the
	// grazing-angle falloff the artist tuned wherever the surface really does end.
	//
	// Feeding the gate through ndv rather than scaling the rim directly means it lands
	// CUBICALLY: rim becomes 0.35 * sil^3 * (1 - ndv)^3, because it rides ndv through the
	// pow. That is the reason the band hugs the skyline as tightly as it does, and it is
	// wanted -- a silhouette highlight should be tight -- but it is why halving the ring's
	// scale factor would thin the rim far more than it looks like it should.
	float sil = silhouette_gate(px, size);
	if (pc.flags.y == 5u) {
		imageStore(out_lit, px, vec4(sil, sil, sil, 1.0));
		return;
	}
	vec3 lit = cel_shade(g0.rgb, ambient, ndl, mix(1.0, ndv, sil), ndh, shadow, 1.0,
			g1.w, sun_light.rgb.xyz);

	// Emission is ADDED after shading, never lit: a glowing surface is its own light source.
	// The whole block is skipped for any material whose table strength is zero, which is
	// every material but the emissive ones -- so dull terrain pays one array read.
	//
	// The mask is the albedo array's alpha (see MaterialAtlas::pack_layer). It is sampled
	// here rather than carried through the G-buffer because no G-buffer channel is free,
	// and widening it would make every pixel pay for a feature one material uses.
	float glow = mat_glow(mat);
	if (glow > 0.0) {
		// This is a compute shader: there is no dFdx. Reconstruct the neighbouring pixels'
		// world positions from the depth buffer to get the triplanar gradients, the same
		// way the raymarcher derives them from ray differentials.
		vec3 wpos_x = wpos, wpos_y = wpos;
		ivec2 mx = min(px + ivec2(1, 0), size - 1);
		ivec2 my = min(px + ivec2(0, 1), size - 1);
		float dx_depth = texelFetch(gb_depth, mx, 0).r;
		float dy_depth = texelFetch(gb_depth, my, 0).r;
		if (dx_depth > 0.0) {
			vec2 nd = ((vec2(mx) + 0.5) / vec2(size)) * 2.0 - 1.0;
			vec4 hx = pc.inv_view_proj * vec4(nd, dx_depth, 1.0);
			wpos_x = hx.xyz / (abs(hx.w) < 1e-9 ? 1e-9 : hx.w);
		}
		if (dy_depth > 0.0) {
			vec2 nd = ((vec2(my) + 0.5) / vec2(size)) * 2.0 - 1.0;
			vec4 hy = pc.inv_view_proj * vec4(nd, dy_depth, 1.0);
			wpos_y = hy.xyz / (abs(hy.w) < 1e-9 ? 1e-9 : hy.w);
		}
		float mask = material_surface(mat, wpos, n, wpos_x - wpos, wpos_y - wpos).a;
		lit += mat_glow_rgb(mat) * glow * mask;
	}
	imageStore(out_lit, px, vec4(lit, 1.0));
}
