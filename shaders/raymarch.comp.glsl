#[compute]
#version 460

#define MATERIAL_LAYERS 16
// The material arrays live at the end of set 0. They must be declared before common.glslh
// so material_surface() can see them; the include defines the shared shading functions.
layout(set = 0, binding = 18) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 19) uniform sampler2DArray material_surface_tex;

#include "common.glslh"
#include "brick_layout.glslh"
#include "shade.glslh"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba8) writeonly uniform image2D out_albedo;
layout(set = 0, binding = 1, rgba32f) writeonly uniform image2D out_hitpos;
layout(set = 0, binding = 20, rgba16f) writeonly uniform image2D out_surface;
// Two words per pixel: [0] steps consumed by the primary ray, [1] brick cells in the low
// 16 bits and region cells in the high 16. Written every frame -- one store per pixel is
// below the noise floor of a pass that reads the atlas thousands of times -- so the probe
// never needs a special dispatch path that could drift from the real one.
layout(set = 0, binding = 23, std430) writeonly buffer CostOut { uint v[]; } cost_out;
layout(set = 0, binding = 2) uniform sampler3D sdf_atlas;   // R8 unorm, nearest
layout(set = 0, binding = 3) uniform usampler3D mat_atlas;  // R8 uint, nearest
layout(set = 0, binding = 4) uniform usampler3D mip2_atlas; // RG8 uint, 2^3 cells/brick
layout(set = 0, binding = 5) uniform usampler3D mip4_atlas; // 4^3 (built, unused here)
layout(set = 0, binding = 6) uniform usampler3D mip8_atlas; // 8^3
layout(set = 0, binding = 7, std430) readonly buffer Palette { uint ids[]; } palette_buf;
layout(set = 0, binding = 8, std430) readonly buffer RegionMap { int slot[]; } region_map;
layout(set = 0, binding = 9, std430) readonly buffer RegionTables { int slot[]; } region_tables;
layout(set = 0, binding = 10, std430) readonly buffer OpPool { uvec4 v[]; } op_pool;
layout(set = 0, binding = 11, std430) readonly buffer OpCounts { int n[]; } op_counts;
// The pending-edit visualizer: tint the atlas content an edit WILL change, so the player
// gets one frame of feedback before the regenerated bricks land (spec §5 latency).
layout(set = 0, binding = 12) uniform Edits { vec4 center; vec4 params; } edits;

layout(push_constant, std430) uniform Push {
	vec4 cam_pos;
	vec4 cam_right;
	vec4 cam_up;
	vec4 cam_fwd;
	vec4 params;          // tan_half_fov_x, tan_half_fov_y, max_dist, unused
	ivec4 dims;           // world size in REGIONS
	ivec4 region_origin;  // world origin in REGIONS
	ivec4 atlas_bricks;   // atlas grid
} pc;

// Diagnostic counters. They are plain globals rather than an inout parameter chain because
// every function that could touch them already takes `steps_left`, and a second inout on
// three call sites buys nothing. The compiler drops them when BEAUTY_COST_VIEW is unset in
// the flags word only at runtime, not at compile time -- two counters of ALU is the price
// of being able to see the march at all.
uint g_brick_cells = 0u;
uint g_region_cells = 0u;

// Region-table slot for the region holding a GLOBAL brick coord; -1 outside the world or
// not resident. `>> 5` is an arithmetic shift: floor(b / 32), correct for negatives.
int region_slot_of(ivec3 brick) {
	ivec3 r = brick >> 5;
	ivec3 l = r - pc.region_origin.xyz;
	if (any(lessThan(l, ivec3(0))) || any(greaterThanEqual(l, pc.dims.xyz))) return -1;
	return region_map.slot[l.x + l.y * pc.dims.x + l.z * pc.dims.x * pc.dims.y];
}

// Atlas slot of a global brick; -1 when absent. `& 31` is the floor-mod for negatives.
int slot_at(ivec3 brick) {
	int rs = region_slot_of(brick);
	if (rs < 0) return -1;
	int bi = (brick.x & 31) + (brick.y & 31) * REGION_BRICKS +
			(brick.z & 31) * REGION_BRICKS * REGION_BRICKS;
	return region_tables.slot[rs * REGION_BRICK_COUNT + bi];
}

// Manual trilinear inside one brick (unchanged from M1 except the runtime atlas base).
float brick_sdf(int slot, vec3 local) { // local in voxel units [0, 16]
	vec3 p = clamp(local, vec3(0.0), vec3(BRICK_SDF_MAX));
	ivec3 i0 = ivec3(floor(p));
	vec3 f = p - vec3(i0);
	ivec3 i1 = min(i0 + 1, ivec3(BRICK_VOXELS));
	ivec3 base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_SDF_STRIDE);
	float c000 = texelFetch(sdf_atlas, base + ivec3(i0.x, i0.y, i0.z), 0).r;
	float c100 = texelFetch(sdf_atlas, base + ivec3(i1.x, i0.y, i0.z), 0).r;
	float c010 = texelFetch(sdf_atlas, base + ivec3(i0.x, i1.y, i0.z), 0).r;
	float c110 = texelFetch(sdf_atlas, base + ivec3(i1.x, i1.y, i0.z), 0).r;
	float c001 = texelFetch(sdf_atlas, base + ivec3(i0.x, i0.y, i1.z), 0).r;
	float c101 = texelFetch(sdf_atlas, base + ivec3(i1.x, i0.y, i1.z), 0).r;
	float c011 = texelFetch(sdf_atlas, base + ivec3(i0.x, i1.y, i1.z), 0).r;
	float c111 = texelFetch(sdf_atlas, base + ivec3(i1.x, i1.y, i1.z), 0).r;
	float v = mix(mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y),
	              mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y), f.z);
	return decode_sdf(v);
}

float world_sdf(vec3 p) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = slot_at(brick);
	if (slot < 0) return SDF_RANGE;
	vec3 local = (p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE;
	return brick_sdf(slot, local);
}

// Gradient taps can land in a brick with NO atlas slot; fall back to the anchor brick,
// whose apron covers the shared face exactly (M1 fix, semantics unchanged).
float sdf_near(vec3 p, ivec3 anchor, int anchor_slot) {
	ivec3 brick = ivec3(floor(p / BRICK_SIZE));
	int slot = all(equal(brick, anchor)) ? anchor_slot : slot_at(brick);
	if (slot < 0) { brick = anchor; slot = anchor_slot; }
	return brick_sdf(slot, (p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE);
}

vec3 calc_normal(vec3 p, ivec3 anchor, int anchor_slot, inout int steps_left) {
	if (steps_left < 6) return vec3(0.0, 1.0, 0.0);
	steps_left -= 6;
	const float e = 0.01;
	return normalize(vec3(
		sdf_near(p + vec3(e, 0, 0), anchor, anchor_slot) - sdf_near(p - vec3(e, 0, 0), anchor, anchor_slot),
		sdf_near(p + vec3(0, e, 0), anchor, anchor_slot) - sdf_near(p - vec3(0, e, 0), anchor, anchor_slot),
		sdf_near(p + vec3(0, 0, e), anchor, anchor_slot) - sdf_near(p - vec3(0, 0, e), anchor, anchor_slot)));
}

// Material of the surface crossing inside `brick`, anchored so a hit point that rounds
// into a neighbouring (possibly absent or empty-palette) brick cannot resolve magenta.
uint material_at(vec3 p, ivec3 brick, int slot) {
	vec3 local = clamp((p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE, vec3(0.0), vec3(15.0));
	ivec3 base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_VOXELS);
	uint idx = texelFetch(mat_atlas, base + ivec3(local), 0).r;
	return palette_buf.ids[slot * 4 + idx];
}

// The chain stores inclusive min/max over each cell's trilinear corner samples (Task 4),
// so "no surface" is a SOUND skip: the reconstructed field inside the cell cannot cross 0.
// Whole-brick rejection: the 2^3 level holds one min/max per 8^3-voxel octant, so the
// brick summary is the reduce over all eight cells — inclusive-exact, never hides a hit.
bool brick_may_have_surface(int slot) {
	ivec3 base = atlas_base(slot, pc.atlas_bricks.xyz, 2);
	uint mn = 255u, mx = 0u;
	for (int z = 0; z < 2; z++)
		for (int y = 0; y < 2; y++)
			for (int x = 0; x < 2; x++) {
				uvec2 mm = texelFetch(mip2_atlas, base + ivec3(x, y, z), 0).xy;
				mn = min(mn, mm.x);
				mx = max(mx, mm.y);
			}
	return mn <= ENCODED_ZERO && mx >= ENCODED_ZERO;
}

bool cell8_may_have_surface(int slot, ivec3 cell) { // cell in [0,8)^3, 2 voxels per cell
	uvec2 mm = texelFetch(mip8_atlas, atlas_base(slot, pc.atlas_bricks.xyz, 8) + cell, 0).xy;
	// Only the MIN half belongs here. The march's hit test is one-sided -- it accepts any
	// d below a small positive threshold, negatives included -- so the question this gate
	// has to answer is "can any point in the cell be close enough to hit", i.e. is the
	// minimum low enough. Requiring a sign CHANGE as well (mm.y >= ENCODED_ZERO) additionally
	// skipped every cell lying wholly INSIDE the surface, whose max is below the zero code.
	// A ray that entered the solid through such a cell was advanced straight out the far
	// side, leaving isolated one-pixel holes in the g-buffer that the outline pass then drew
	// a black speck around. The trilinear field is bounded by its corner samples, so the
	// minimum alone is still a sound skip.
	return mm.x <= ENCODED_ZERO;
}

// ---------------------------------------------------------------------------------------
// Islands (spec §3, "Multi-target raymarching"). Each is a dense 64^3 volume in a pool of
// byte-packed storage buffers, placed by a rigid transform. Its hit competes with the
// terrain's on distance alone, which is what makes the two shade identically.
// ---------------------------------------------------------------------------------------
const int ISLAND_DIM = 64;          // ve::kIslandDim
const int ISLAND_VOXELS = 262144;   // 64^3
const int ISLAND_MIP_STRIDE = 8;    // ve::kVolumeMipStride
const int ISLAND_MIP_CELLS = 8;     // ISLAND_DIM / ISLAND_MIP_STRIDE
const int ISLAND_MIP_PER_SLOT = 512;

const float GLOSSY_SDF_MAX_DIST = 20.0;
const int GLOSSY_SDF_STEPS = 64;
const float GLOSSY_SDF_MIN_GLOSS = 0.5;
const float GLOSSY_SDF_BIAS = 0.06;
const float GLOSSY_SDF_STRENGTH = 0.80;

layout(set = 0, binding = 13, std430) readonly buffer IslandSdf { uint w[]; } island_sdf;
layout(set = 0, binding = 14, std430) readonly buffer IslandMat { uint w[]; } island_mat;
layout(set = 0, binding = 15, std430) readonly buffer IslandMip { uint w[]; } island_mip;
// Eight vec4 per island: basis columns 0-2 with the body translation in .w, then
// (lattice origin.xyz, voxel), then (dim, 0, 0, 0) as ints, then the world AABB.
layout(set = 0, binding = 16, std430) readonly buffer IslandDesc { vec4 v[]; } island_desc;
// One uint per 16x16 screen tile, bit i = "island i may be visible here" (Task 11). When
// pc.region_origin.w is 0 the buffer is a single all-ones entry and every island is marched.
layout(set = 0, binding = 17, std430) readonly buffer TileMask { uint v[]; } tile_mask;

struct Island {
	mat3 basis;    // local -> world
	vec3 pos;      // body translation, world
	vec3 lo;       // lattice minimum corner, LOCAL
	float voxel;
	int dim;
};

bool island_load(int i, out Island isl) {
	vec4 r0 = island_desc.v[i * 8 + 0];
	vec4 r1 = island_desc.v[i * 8 + 1];
	vec4 r2 = island_desc.v[i * 8 + 2];
	vec4 lv = island_desc.v[i * 8 + 3];
	int dim = floatBitsToInt(island_desc.v[i * 8 + 4].x);
	if (dim < 2) return false; // dead slot
	isl.basis = mat3(r0.xyz, r1.xyz, r2.xyz); // mat3(c0, c1, c2): columns, as written
	isl.pos = vec3(r0.w, r1.w, r2.w);
	isl.lo = lv.xyz;
	isl.voxel = lv.w;
	isl.dim = dim;
	return true;
}

uint island_byte_sdf(int i) {
	return (island_sdf.w[i >> 2] >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}
uint island_byte_mat(int i) {
	return (island_mat.w[i >> 2] >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}
uint island_byte_mip(int i) {
	return (island_mip.w[i >> 2] >> ((uint(i) & 3u) * 8u)) & 0xFFu;
}

float island_lattice(int slot, int dim, ivec3 v) {
	int i = slot * ISLAND_VOXELS + v.x + v.y * dim + v.z * dim * dim;
	return decode_sdf(float(island_byte_sdf(i)) / 255.0);
}

// Trilinear reconstruction in LOCAL space, mirroring ve::sample_volume_lattice's inside
// branch. Callers clamp q to the lattice box first, so no outside branch is needed here.
float island_sdf_at(int slot, Island isl, vec3 q) {
	vec3 l = clamp((q - isl.lo) / isl.voxel, vec3(0.0), vec3(float(isl.dim - 1)));
	ivec3 i0 = ivec3(l);
	ivec3 i1 = min(i0 + 1, ivec3(isl.dim - 1));
	vec3 f = l - vec3(i0);
	float c000 = island_lattice(slot, isl.dim, ivec3(i0.x, i0.y, i0.z));
	float c100 = island_lattice(slot, isl.dim, ivec3(i1.x, i0.y, i0.z));
	float c010 = island_lattice(slot, isl.dim, ivec3(i0.x, i1.y, i0.z));
	float c110 = island_lattice(slot, isl.dim, ivec3(i1.x, i1.y, i0.z));
	float c001 = island_lattice(slot, isl.dim, ivec3(i0.x, i0.y, i1.z));
	float c101 = island_lattice(slot, isl.dim, ivec3(i1.x, i0.y, i1.z));
	float c011 = island_lattice(slot, isl.dim, ivec3(i0.x, i1.y, i1.z));
	float c111 = island_lattice(slot, isl.dim, ivec3(i1.x, i1.y, i1.z));
	return mix(mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y),
	           mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y), f.z);
}

uint island_material_at(int slot, Island isl, vec3 q) {
	vec3 l = clamp((q - isl.lo) / isl.voxel, vec3(0.0), vec3(float(isl.dim - 1)));
	ivec3 m = min(ivec3(l + 0.5), ivec3(isl.dim - 1));
	int i = slot * ISLAND_VOXELS + m.x + m.y * isl.dim + m.z * isl.dim * isl.dim;
	return island_byte_mat(i);
}

// Spec §3's "own min-max mip": the same inclusive-corner soundness argument the brick chain
// rests on, so a "no surface" verdict is a skip that cannot tunnel.
bool island_cell_has_surface(int slot, ivec3 c) {
	int i = (slot * ISLAND_MIP_PER_SLOT + c.x + c.y * ISLAND_MIP_CELLS +
			c.z * ISLAND_MIP_CELLS * ISLAND_MIP_CELLS) * 2;
	uint mn = island_byte_mip(i);
	uint mx = island_byte_mip(i + 1);
	return mn <= ENCODED_ZERO && mx >= ENCODED_ZERO;
}

// Slab test. rd components are nudged off zero rather than divided by it: 0 * inf is NaN,
// and a NaN here would silently drop the island for that pixel.
bool ray_box(vec3 ro, vec3 rd, vec3 lo, vec3 hi, out float t0, out float t1) {
	vec3 srd = vec3(abs(rd.x) < 1e-8 ? 1e-8 : rd.x, abs(rd.y) < 1e-8 ? 1e-8 : rd.y,
			abs(rd.z) < 1e-8 ? 1e-8 : rd.z);
	vec3 a = (lo - ro) / srd;
	vec3 b = (hi - ro) / srd;
	vec3 tmn = min(a, b), tmx = max(a, b);
	t0 = max(max(tmn.x, tmn.y), tmn.z);
	t1 = min(min(tmx.x, tmx.y), tmx.z);
	return t1 >= max(t0, 0.0);
}

struct Hit {
	bool hit;
	float t;
	vec3 p;
	vec3 n;
	uint mat;
};

// Sphere-traces one island. `best` is updated only when this island is nearer, so calling it
// for every island in the tile's mask resolves "nearest hit wins" with no sorting.
void march_island(int slot, vec3 ro, vec3 rd, inout Hit best, inout int steps_left) {
	Island isl;
	if (!island_load(slot, isl)) return;

	// World -> local. The basis is orthonormal, so its inverse is its transpose and the ray
	// stays unit length: t values are directly comparable with the terrain's.
	mat3 inv = transpose(isl.basis);
	vec3 ro_l = inv * (ro - isl.pos);
	vec3 rd_l = inv * rd;
	vec3 span = vec3(float(isl.dim - 1) * isl.voxel);

	float t0, t1;
	if (!ray_box(ro_l, rd_l, isl.lo, isl.lo + span, t0, t1)) return;
	t0 = max(t0, 0.0);
	t1 = min(t1, best.t);
	if (t0 > t1) return;

	float t = t0;
	float cell_m = float(ISLAND_MIP_STRIDE) * isl.voxel;
	for (int k = 0; k < 192 && steps_left > 0; k++) {
		if (t > t1) return;
		vec3 q = ro_l + rd_l * t;
		ivec3 c = clamp(ivec3(floor((q - isl.lo) / cell_m)), ivec3(0),
				ivec3(ISLAND_MIP_CELLS - 1));
		if (!island_cell_has_surface(slot, c)) {
			steps_left--;
			// Jump to the cell's exit face, exactly as the brick march does, with a floor on
			// the step so a ray grazing a face still makes progress.
			vec3 clo = isl.lo + vec3(c) * cell_m;
			vec3 chi = clo + cell_m;
			vec3 far = mix(clo, chi, step(0.0, rd_l));
			vec3 tf = (far - q) / vec3(abs(rd_l.x) < 1e-8 ? 1e-8 : rd_l.x,
					abs(rd_l.y) < 1e-8 ? 1e-8 : rd_l.y,
					abs(rd_l.z) < 1e-8 ? 1e-8 : rd_l.z);
			t += max(min(tf.x, min(tf.y, tf.z)), 0.002);
			continue;
		}
		if (steps_left <= 0) return;
		steps_left--;
		float d = island_sdf_at(slot, isl, q);
		if (d < 0.002) {
			for (int r = 0; r < 4; r++) { // secant refinement, as the terrain march does
				if (steps_left <= 0) return;
				steps_left--;
				q = ro_l + rd_l * t;
				t += island_sdf_at(slot, isl, q) * 0.5;
			}
			if (t > best.t) return; // refinement pushed it behind the current winner
			q = ro_l + rd_l * t;
			if (steps_left < 6) return;
			steps_left -= 6;
			const float e = 0.5 * 0.05;
			vec3 n_l = normalize(vec3(
				island_sdf_at(slot, isl, q + vec3(e, 0, 0)) -
					island_sdf_at(slot, isl, q - vec3(e, 0, 0)),
				island_sdf_at(slot, isl, q + vec3(0, e, 0)) -
					island_sdf_at(slot, isl, q - vec3(0, e, 0)),
				island_sdf_at(slot, isl, q + vec3(0, 0, e)) -
					island_sdf_at(slot, isl, q - vec3(0, 0, e))));
			best.hit = true;
			best.t = t;
			best.p = ro + rd * t;
			best.n = normalize(isl.basis * n_l);
			best.mat = island_material_at(slot, isl, q);
			return;
		}
		t += max(d * 0.9, 0.005);
	}
}

// The M1/M2 terrain march, unchanged in behaviour, returning a hit record instead of a
// colour so an island can outrank it.
Hit march_terrain(vec3 ro, vec3 rd, float max_dist, inout int steps_left) {
	Hit h;
	h.hit = false;
	h.t = max_dist;
	h.p = vec3(0.0);
	h.n = vec3(0.0, 1.0, 0.0);
	h.mat = 0u;

	ivec3 map = ivec3(floor(ro / BRICK_SIZE));
	vec3 delta = abs(vec3(BRICK_SIZE) / rd);
	ivec3 st = ivec3(sign(rd));
	vec3 side = (vec3(map) * BRICK_SIZE - ro + (vec3(st) * 0.5 + 0.5) * BRICK_SIZE) / rd;
	if (st.x == 0) side.x = 1.0 / 0.0;
	if (st.y == 0) side.y = 1.0 / 0.0;
	if (st.z == 0) side.z = 1.0 / 0.0;
	float t_prev = 0.0;

	for (int i = 0; i < 1024; i++) {
		float t_exit = min(side.x, min(side.y, side.z));
		g_brick_cells++;
		if (t_exit > max_dist) break;

		int slot = slot_at(map);
		if (slot >= 0 && brick_may_have_surface(slot)) {
			bool has_material = palette_buf.ids[slot * 4] != 0u;
			float t = t_prev;
			for (int j = 0; j < 64 && steps_left > 0; j++) {
				if (t > t_exit) break;
				vec3 p = ro + rd * t;
				vec3 vox = (p - vec3(map) * BRICK_SIZE) / VOXEL_SIZE;
				ivec3 cell8 = clamp(ivec3(floor(vox * 0.5)), ivec3(0), ivec3(7));
				if (!cell8_may_have_surface(slot, cell8)) {
					steps_left--;
					vec3 cell_lo = vec3(map) * BRICK_SIZE + vec3(cell8 * 2) * VOXEL_SIZE;
					vec3 cell_hi = cell_lo + 2.0 * VOXEL_SIZE;
					vec3 far = mix(cell_lo, cell_hi, step(0.0, rd));
					vec3 tf = (far - p) / rd;
					if (st.x == 0) tf.x = 1.0 / 0.0;
					if (st.y == 0) tf.y = 1.0 / 0.0;
					if (st.z == 0) tf.z = 1.0 / 0.0;
					t = min(t + max(min(tf.x, min(tf.y, tf.z)), 0.002), t_exit);
					continue;
				}
				if (steps_left <= 0) break;
				steps_left--;
				float d = world_sdf(p);
				if (d < 0.002 && has_material) {
					for (int k = 0; k < 4; k++) {
						if (steps_left <= 0) return h;
						steps_left--;
						float dk = world_sdf(p);
						t += dk * 0.5;
						p = ro + rd * t;
					}
					h.hit = true;
					h.t = t;
					h.p = p;
					h.n = calc_normal(p, map, slot, steps_left);
					h.mat = material_at(p, map, slot);
					return h;
				}
				t += max(d * 0.9, 0.005);
			}
		}

		if (side.x < side.y && side.x < side.z) { t_prev = side.x; side.x += delta.x; map.x += st.x; }
		else if (side.y < side.z)               { t_prev = side.y; side.y += delta.y; map.y += st.y; }
		else                                    { t_prev = side.z; side.z += delta.z; map.z += st.z; }
	}
	return h;
}

// ---------------------------------------------------------------------------------------
// Shadow layer 1 (spec section 7): the same field the primary ray marched, one ray per
// pixel. Sphere tracing gives contact hardening for free -- the penumbra narrows as the
// occluder approaches -- with no shadow map and therefore no acne to bias away.
//
// world_sdf() returns +SDF_RANGE for a known-empty brick. Residency is an explicit
// prerequisite for a shadow result: if the ray leaves the resident/probed region field,
// return fully lit rather than treating the accumulated darkness as known data.
// ---------------------------------------------------------------------------------------
const float RAY_SHADOW_DIST = 60.0;
const int RAY_SHADOW_STEPS = 96;
const float RAY_SHADOW_K = 12.0;
const int RAY_SHADOW_MAX_ISLANDS = 4;

// How far the penumbra term is allowed to look. The field is a NARROW BAND: every read goes
// through decode_sdf(), which saturates at +SDF_RANGE, so a sample of 0.64 means "no surface
// within 0.64 m" and carries no occluder distance at all.
//
// K*d/t only clears 1.0 when d >= t/K, so once t passes K * SDF_RANGE there is no value the
// band can hold that reports "lit": every further step darkens by 1/t whether anything is
// there or not. That is what turned open ground black -- a full 60 m march under an empty
// sky ended at 12 * 0.64 / 60 = 0.128 visibility, and whether a given pixel's ray ran the
// whole budget or took the fully-lit residency early-out below made the difference between
// black and white on neighbouring pixels.
//
// Past this distance the march keeps its hard hit test and nothing else. An occluder that
// far away is a shadow edge this band cannot soften anyway.
const float RAY_SHADOW_PENUMBRA_DIST = RAY_SHADOW_K * SDF_RANGE; // 7.68 m

float terrain_sun_visibility(vec3 ro) {
	float res = 1.0;
	float t = 0.05;
	for (int i = 0; i < RAY_SHADOW_STEPS; i++) {
		if (t > RAY_SHADOW_DIST) break;
		vec3 q = ro + SUN_DIR * t;
		ivec3 brick = ivec3(floor(q / BRICK_SIZE));
		int shadow_region = region_slot_of(brick);
		if (shadow_region < 0) return 1.0;
		float d = world_sdf(q);
		if (d < 0.004) return 0.0;
		if (t <= RAY_SHADOW_PENUMBRA_DIST) res = min(res, RAY_SHADOW_K * d / t);
		t += clamp(d, 0.02, 1.0);
	}
	return clamp(res, 0.0, 1.0);
}

// Spec section 5: "islands shade/shadow/reflect exactly like static terrain". The AABB
// reject costs a few ALU for each of the (at most 32) live slots; only islands the ray
// actually crosses are marched, and at most RAY_SHADOW_MAX_ISLANDS of them. A fifth
// overlapping island is a case the demo does not produce and the budget does not pay for.
float island_sun_visibility(vec3 ro, int island_count) {
	float res = 1.0;
	int marched = 0;
	for (int i = 0; i < 32; i++) {
		if (i >= island_count || marched >= RAY_SHADOW_MAX_ISLANDS) break;
		vec3 lo = island_desc.v[i * 8 + 5].xyz;
		vec3 hi = island_desc.v[i * 8 + 6].xyz;
		float t0, t1;
		if (!ray_box(ro, SUN_DIR, lo, hi, t0, t1)) continue;
		Island isl;
		if (!island_load(i, isl)) continue;
		marched++;
		mat3 inv = transpose(isl.basis);
		vec3 ro_l = inv * (ro - isl.pos);
		vec3 rd_l = inv * SUN_DIR;
		float t = max(t0, 0.05);
		float tmax = min(t1, RAY_SHADOW_DIST);
		for (int k = 0; k < 48; k++) {
			if (t > tmax) break;
			float d = island_sdf_at(i, isl, ro_l + rd_l * t);
			if (d < 0.004) return 0.0;
			// Same narrow band, same saturation, same cutoff as terrain_sun_visibility.
			if (t <= RAY_SHADOW_PENUMBRA_DIST) res = min(res, RAY_SHADOW_K * d / t);
			t += clamp(d, 0.02, 1.0);
		}
	}
	return clamp(res, 0.0, 1.0);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(out_albedo);
	if (px.x >= size.x || px.y >= size.y) return;
	vec2 uv = (vec2(px) + 0.5) / vec2(size);
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

	vec3 ro = pc.cam_pos.xyz;
	vec3 rd = normalize(pc.cam_fwd.xyz
		+ pc.cam_right.xyz * ndc.x * pc.params.x
		+ pc.cam_up.xyz * ndc.y * pc.params.y);
	float max_dist = pc.params.z;

	int primary_steps = 65536;
	Hit best = march_terrain(ro, rd, max_dist, primary_steps);

	// Which islands could be here? pc.region_origin.w is the cull grid's tiles-per-row, and
	// 0 means "no mask" -- the 1x1 debug probes and any frame before the cull pass has run.
	int island_count = min(pc.dims.w, 32);
	uint mask = island_count > 0 ? 0xFFFFFFFFu : 0u;
	int tiles_x = pc.region_origin.w;
	if (tiles_x > 0) {
		ivec2 tile = px / 16;
		mask = tile_mask.v[tile.y * tiles_x + tile.x];
	}
	if (island_count < 32) mask &= (1u << uint(island_count)) - 1u;
	while (mask != 0u) {
		int i = findLSB(mask);
		mask &= mask - 1u;
		int island_steps = 192;
		march_island(i, ro, rd, best, island_steps);
	}

	uint flags = floatBitsToUint(pc.cam_pos.w);

	// Sky and misses: material 0 means "no voxel here". The albedo channel carries the sky
	// gradient and the deferred pass passes material 0 straight through unlit, so sky_color()
	// still lives in exactly one place.
	vec3 albedo = sky_color(rd);
	vec2 oct = oct_encode(-rd);
	float mat_id = 0.0;
	float gloss = 0.0;
	float ao = 1.0;
	float sun = 1.0;
	vec4 hitpos = vec4(0.0);

	if (best.hit) {
		// The pixel's world footprint at the hit: the ray direction's screen derivative
		// scaled by distance. tan_x/tan_y and the target size are already in the push
		// constant, so this costs two multiplies and no extra state.
		vec3 ddx = pc.cam_right.xyz * (2.0 * pc.params.x / float(size.x)) * best.t;
		vec3 ddy = pc.cam_up.xyz * (2.0 * pc.params.y / float(size.y)) * best.t;
		vec4 surf = material_surface(best.mat, best.p, best.n, ddx, ddy);
		vec2 props = material_props(best.mat, best.p, best.n, ddx, ddy);
		albedo = surf.rgb;
		oct = oct_encode(best.n);
		mat_id = float(best.mat);
		gloss = 1.0 - props.x;
		ao = props.y;
		hitpos = vec4(best.p, 1.0);
	if ((flags & BEAUTY_RAY_SUN_SHADOW) != 0u) {
			// One voxel of offset: less and the ray self-shadows on its own surface, more
			// and thin ledges stop casting.
			vec3 sro = best.p + best.n * 0.06;
			sun = min(terrain_sun_visibility(sro), island_sun_visibility(sro, island_count));
		}
		if (pc.params.w < -0.5) gloss = 1.0;
		if ((flags & BEAUTY_GLOSSY_RAYS) != 0u && gloss > GLOSSY_SDF_MIN_GLOSS) {
			vec3 rr = normalize(reflect(rd, best.n));
			vec3 rro = best.p + best.n * GLOSSY_SDF_BIAS;
			int reflected_steps = GLOSSY_SDF_STEPS;
			Hit reflected = march_terrain(rro, rr, GLOSSY_SDF_MAX_DIST, reflected_steps);
			// A reflected ray leaves the primary tile, so its tile mask is invalid. AABB-reject
			// every live island descriptor, sharing the same remaining 64-step budget.
			for (int i = 0; i < island_count && reflected_steps > 0; i++)
				march_island(i, rro, rr, reflected, reflected_steps);
			vec3 reflected_albedo = sky_color(rr);
			if (reflected.hit)
				reflected_albedo = material_surface(reflected.mat, reflected.p,
						reflected.n, ddx, ddy).rgb;
			float ndv = clamp(dot(best.n, -rd), 0.0, 1.0);
			float fresnel = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
			float weight = clamp(GLOSSY_SDF_STRENGTH * fresnel *
					smoothstep(0.5, 1.0, gloss), 0.0, 0.85);
			albedo = mix(albedo, reflected_albedo, weight);
		}
	}

	// The pending-edit visualiser tints the DESCRIPTION, so the tint survives the deferred
	// pass instead of being relit away.
	if (best.hit && edits.params.x > 0.0 &&
			length(best.p - edits.center.xyz) < edits.params.x) {
		uint et = uint(edits.params.y);
		vec3 tint = et == 0u ? vec3(1.0, 0.55, 0.1)
		          : et == 1u ? flat_material_albedo(4u)
		          : flat_material_albedo(uint(edits.params.z));
		albedo = mix(albedo, tint, 0.45);
	}

	// Debug material probe: a 1x1 dispatch calls material_surface() directly with zero
	// gradients. pc.params.w is otherwise unused, so > 0 is the probe flag.
	if (pc.params.w > 0.0) {
		vec4 surf = material_surface(uint(pc.params.w), pc.cam_pos.xyz,
				normalize(pc.cam_fwd.xyz), vec3(0.0), vec3(0.0));
		int cost_i = (px.y * size.x + px.x) * 2;
		cost_out.v[cost_i + 0] = uint(65536 - primary_steps);
		cost_out.v[cost_i + 1] =
				(g_brick_cells & 0xFFFFu) | (min(g_region_cells, 0xFFFFu) << 16);
		imageStore(out_albedo, px, vec4(surf.rgb, 1.0));
		imageStore(out_surface, px, vec4(0.0, 0.0, pc.params.w, 0.0));
		imageStore(out_hitpos, px, vec4(pc.cam_pos.xyz, 1.0));
		return;
	}

	if ((flags & BEAUTY_COST_VIEW) != 0u) {
		// One heat unit per marching step, black at 0 and white at 512, so a pixel that
		// burns half the shadow budget is unmistakable next to one that does not.
		float heat = clamp(float(65536 - primary_steps) / 512.0, 0.0, 1.0);
		// Blue -> green -> red, which reads as "cheap -> expensive" at a glance and keeps
		// the sky (0 steps) black rather than a colour the eye reads as terrain.
		vec3 hc = heat < 0.5 ? mix(vec3(0.0, 0.1, 0.6), vec3(0.1, 0.8, 0.2), heat * 2.0)
		                     : mix(vec3(0.1, 0.8, 0.2), vec3(0.9, 0.1, 0.05), heat * 2.0 - 1.0);
		albedo = hc;
		// Everything else in the G-buffer stays truthful: depth, normal, material and sun
		// visibility are written exactly as they would be, so the depth test, the outlines
		// and the LoD seam behave normally and the view can be toggled mid-flight.
	}

	// AO has no channel of its own. The cel stack only ever multiplies the AMBIENT term by
	// it, and folding it into the albedo here costs nothing and keeps hitpos.w the pure hit
	// flag every existing reader already treats it as. 0.65 is how much of the map is
	// allowed to darken the surface; a full multiply reads as dirt in the cel bands.
	int cost_i = (px.y * size.x + px.x) * 2;
	cost_out.v[cost_i + 0] = uint(65536 - primary_steps);
	cost_out.v[cost_i + 1] = (g_brick_cells & 0xFFFFu) | (min(g_region_cells, 0xFFFFu) << 16);
	imageStore(out_albedo, px, vec4(albedo * mix(1.0, ao, 0.65), sun));
	imageStore(out_surface, px, vec4(oct, mat_id, gloss));
	imageStore(out_hitpos, px, hitpos);
}
