#[compute]
#version 460

#define MATERIAL_LAYERS 16
// Set 0's established bindings 24-30 belong to the compact-normal/override interface.
// Keep that interface unchanged; SunLight uses binding 24 in its dedicated set 2. Set 1
// belongs to the terrain pipeline's field context (FieldContextSet): the generated
// field.glslh declares its params UBO and sector map there, so every field-consuming
// pass binds set 1 and the sun cannot share it.
#define SUN_LIGHT_SET 2
#define SUN_LIGHT_BINDING 24
// The material arrays live at the end of set 0. They must be declared before common.glslh
// so material_surface() can see them; the include defines the shared shading functions.
layout(set = 0, binding = 18) uniform sampler2DArray material_albedo;
layout(set = 0, binding = 19) uniform sampler2DArray material_surface_tex;

#include "common.glslh"
#include "brick_layout.glslh"
#include "shade.glslh"
#include "sun_light.glslh"

// Task 7: normals are evaluated from the SOURCE field instead of differentiating the R8
// atlas. field.glslh owns the op-pool declaration at binding 10; these macros name the
// shared authoritative volume / override / compact-normal buffers it consumes.
#define FIELD_OP_POOL_BINDING 10
#define FIELD_VOLUME_SDF_BINDING 13
#define FIELD_VOLUME_MAT_BINDING 14
#define FIELD_VOLUME_NORMAL_BINDING 24
#define FIELD_VOLUME_NORMAL_OFFSET_BINDING 25
#define FIELD_OVERRIDE_SDF_BINDING 27
#define FIELD_OVERRIDE_MAT_BINDING 28
#define FIELD_OVERRIDE_TABLE_BINDING 29
#define FIELD_OVERRIDE_REGION_BINDING 30
#define FIELD_NORMAL_OVERRIDE_BINDING 26
#define FIELD_OVERRIDE_TABLE(base) (field_override_region_map.table[int((base) / MAX_REGION_OPS)])
#include "field.glslh"

// MUST match ve kRaymarchGroupX/Y in render/raymarch_pass.h -- see the note there on why
// this is one wide row rather than a square tile.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// This pass exports a DESCRIPTION of what each ray found; composite.frag.glsl resolves the
// material from it, once per full-resolution pixel. near_field_scale then buys march time
// without costing texture detail -- see the note at the top of composite.frag.glsl.
//
//   out_albedo   rgb = ray overlay colour, a = sun visibility
//   out_hitpos   xyz = world hit position, w = hit flag
//   out_surface  xy = oct normal, z = material id, w = overlay weight
//
// The overlay is everything a ray knows that a surface does not: the sky on a miss, a glossy
// reflection, the pending-edit tint, the cost view. Each folds into one colour and one weight
// via overlay_mix(), and the composite mixes the pair over the material it resolves.
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
layout(set = 0, binding = 11, std430) readonly buffer OpCounts { int n[]; } op_counts;
layout(set = 0, binding = 21, std430) readonly buffer BrickFlags { uint v[]; } brick_flags;
// Binding 22 is reserved for Task 4's region slot counts; keep the relationship stable while
// this pass learns the per-brick gate.
layout(set = 0, binding = 22, std430) readonly buffer RegionSlotCounts { int n[]; } region_slot_counts;
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

// Atlas slot of a brick within a KNOWN region table; -1 when absent. `& 31` is the
// floor-mod for negatives. The brick DDA runs inside one region for a whole segment, so it
// resolves the region once and calls this, rather than walking region_map -> region_tables
// as two dependent loads on every 0.8 m of ray.
int slot_in_region(int rs, ivec3 brick) {
	int bi = (brick.x & 31) + (brick.y & 31) * REGION_BRICKS +
			(brick.z & 31) * REGION_BRICKS * REGION_BRICKS;
	return region_tables.slot[rs * REGION_BRICK_COUNT + bi];
}

// Atlas slot of a global brick; -1 when absent. For callers that do not already know which
// region they are in (the shadow march, the gradient taps).
int slot_at(ivec3 brick) {
	int rs = region_slot_of(brick);
	if (rs < 0) return -1;
	return slot_in_region(rs, brick);
}

// Trilinear inside one brick. The lattice is 17 voxels on a side -- 16 cells plus the apron
// that repeats the neighbour's shared face -- so a filtered fetch anywhere in [0, 16] reads
// only this brick's own block and the texture unit can do the interpolation. That is one
// fetch where the manual mix issued eight, on the marcher's hottest read.
//
// The two forms agree exactly, not approximately: decode_sdf() is affine, so decoding the
// filtered unorm equals filtering the decoded values, which is what the mix chain computed.
// The only difference is the filter's weight precision (>= 8 fractional bits, i.e. under
// 0.02 mm on this 5 mm-per-code encoding, against a 2 mm hit threshold).
//
// This is why binding 2 alone carries the LINEAR sampler (RaymarchPass::initialize); the
// material and min-max atlases are integer textures and must stay on the NEAREST one.
float brick_sdf(int slot, vec3 local) { // local in voxel units [0, 16]
	vec3 p = clamp(local, vec3(0.0), vec3(BRICK_SDF_MAX));
	vec3 base = vec3(atlas_base(slot, pc.atlas_bricks.xyz, BRICK_SDF_STRIDE));
	vec3 dim = vec3(pc.atlas_bricks.xyz * BRICK_SDF_STRIDE);
	return decode_sdf(textureLod(sdf_atlas, (base + p + 0.5) / dim, 0.0).r);
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

// The R8 FALLBACK: differentiate across one stored voxel. Shorter taps expose the R8 SDF
// quantisation and the piecewise-trilinear cell boundaries as contour-like changes in
// lighting and in the triplanar blend, even though the underlying surface is smooth.
// Reachable only where the source field cannot answer exactly (no region table, or an
// op whose gradient is inexact there); every such source paid its six-step charge here,
// and nowhere else.
vec3 terrain_r8_fallback_normal(vec3 p, ivec3 anchor, int anchor_slot, inout int steps_left) {
	if (steps_left < 6) return vec3(0.0, 1.0, 0.0);
	steps_left -= 6;
	const float e = VOXEL_SIZE;
	return normalize(vec3(
		sdf_near(p + vec3(e, 0, 0), anchor, anchor_slot) - sdf_near(p - vec3(e, 0, 0), anchor, anchor_slot),
		sdf_near(p + vec3(0, e, 0), anchor, anchor_slot) - sdf_near(p - vec3(0, e, 0), anchor, anchor_slot),
		sdf_near(p + vec3(0, 0, e), anchor, anchor_slot) - sdf_near(p - vec3(0, 0, e), anchor, anchor_slot)));
}

// Static-terrain shading normal from the source field itself: evaluate the CPU-mirrored
// analytic gradient over this region's op span (volumes, overrides and CSG included).
// When the evaluator reports an exact, non-degenerate gradient that IS the surface normal;
// otherwise fall back to differentiating the stored R8 lattice.
vec3 terrain_source_normal(vec3 p, ivec3 brick, int anchor_slot, inout int steps_left) {
	int rs = region_slot_of(brick);
	if (rs < 0) return vec3(0.0, 1.0, 0.0);
	float sdf;
	uint mat;
	vec3 gradient;
	bool exact_gradient;
	eval_field_gradient(p, uint(rs) * MAX_REGION_OPS, uint(max(op_counts.n[rs], 0)),
			sdf, mat, gradient, exact_gradient);
	float len = length(gradient);
	if (exact_gradient && len > 1e-8) return gradient / len;
	return terrain_r8_fallback_normal(p, brick, anchor_slot, steps_left);
}

// Material of the surface crossing inside `brick`, anchored so a hit point that rounds
// into a neighbouring (possibly absent or empty-palette) brick cannot resolve magenta.
uint material_at(vec3 p, ivec3 brick, int slot) {
	vec3 local = clamp((p - vec3(brick) * BRICK_SIZE) / VOXEL_SIZE, vec3(0.0), vec3(15.0));
	ivec3 base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_VOXELS);
	uint idx = texelFetch(mat_atlas, base + ivec3(local), 0).r;
	return palette_buf.ids[slot * 4 + idx];
}

// Whole-brick rejection reads the per-brick flag word instead of re-reducing the 2^3 level
// on every DDA step. brick_gen writes that word from the same inclusive straddle test over
// the same mip data (and brick_mark writes CONSERVATIVE for an allocated-but-ungenerated
// slot), so the answer is identical -- at one buffer load instead of eight texture fetches
// plus the palette read, on the hottest path in the marcher. This is the use
// ve::brick_flags_from_mips was introduced for; test_brick_flags_gpu pins GPU == CPU.
uint brick_flag_word(int slot) {
	return brick_flags.v[slot];
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
const int MAX_VOLUME_SLOTS = 64;    // ve::kMaxVolumes

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
	int volume_slot; // shared authoritative volume/normal pool index (descriptor int lane 17)
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
	// Since Task 6 the SDF/material bytes live in the SHARED authoritative volume pool,
	// stridden by the volume slot -- NOT by the atlas slot `i`, which only selects the
	// descriptor, the min-max mip and the tile-mask bit. The two are allocated by different
	// pools (32 atlas slots, 64 volume slots) and diverge permanently once a merged body
	// pins its volume slot while its atlas slot is freed, so striding with the wrong one
	// renders another body's geometry. A descriptor without a valid volume slot has no bytes
	// in that pool at all: treat it as a dead slot rather than read out of range.
	isl.volume_slot = floatBitsToInt(island_desc.v[i * 8 + 4].y);
	if (isl.volume_slot < 0 || isl.volume_slot >= MAX_VOLUME_SLOTS) return false;
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

float island_lattice(int volume_slot, int dim, ivec3 v) {
	int i = volume_slot * ISLAND_VOXELS + v.x + v.y * dim + v.z * dim * dim;
	return decode_sdf(float(island_byte_sdf(i)) / 255.0);
}

// Trilinear reconstruction in LOCAL space, mirroring ve::sample_volume_lattice's inside
// branch. Callers clamp q to the lattice box first, so no outside branch is needed here.
float island_sdf_at(Island isl, vec3 q) {
	vec3 l = clamp((q - isl.lo) / isl.voxel, vec3(0.0), vec3(float(isl.dim - 1)));
	ivec3 i0 = ivec3(l);
	ivec3 i1 = min(i0 + 1, ivec3(isl.dim - 1));
	vec3 f = l - vec3(i0);
	float c000 = island_lattice(isl.volume_slot, isl.dim, ivec3(i0.x, i0.y, i0.z));
	float c100 = island_lattice(isl.volume_slot, isl.dim, ivec3(i1.x, i0.y, i0.z));
	float c010 = island_lattice(isl.volume_slot, isl.dim, ivec3(i0.x, i1.y, i0.z));
	float c110 = island_lattice(isl.volume_slot, isl.dim, ivec3(i1.x, i1.y, i0.z));
	float c001 = island_lattice(isl.volume_slot, isl.dim, ivec3(i0.x, i0.y, i1.z));
	float c101 = island_lattice(isl.volume_slot, isl.dim, ivec3(i1.x, i0.y, i1.z));
	float c011 = island_lattice(isl.volume_slot, isl.dim, ivec3(i0.x, i1.y, i1.z));
	float c111 = island_lattice(isl.volume_slot, isl.dim, ivec3(i1.x, i1.y, i1.z));
	return mix(mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y),
	           mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y), f.z);
}

uint island_material_at(Island isl, vec3 q) {
	vec3 l = clamp((q - isl.lo) / isl.voxel, vec3(0.0), vec3(float(isl.dim - 1)));
	ivec3 m = min(ivec3(l + 0.5), ivec3(isl.dim - 1));
	int i = isl.volume_slot * ISLAND_VOXELS + m.x + m.y * isl.dim + m.z * isl.dim * isl.dim;
	return island_byte_mat(i);
}

// The R8 FALLBACK for island shading normals: the same voxel-wide taps the terrain
// fallback keeps, at this body's own lattice pitch (5 or 10 cm). Reachable only where the
// compact-normal span for the body's volume slot is missing (-1) or degenerate -- a source
// whose CPU publication already counted its fallback hit; no per-pixel atomic and no
// synchronous readback here.
vec3 island_r8_fallback_normal(Island isl, vec3 q, inout int steps_left) {
	if (steps_left < 6) return vec3(0.0, 1.0, 0.0);
	steps_left -= 6;
	float e = isl.voxel;
	return normalize(vec3(
		island_sdf_at(isl, q + vec3(e, 0, 0)) -
			island_sdf_at(isl, q - vec3(e, 0, 0)),
		island_sdf_at(isl, q + vec3(0, e, 0)) -
			island_sdf_at(isl, q - vec3(0, e, 0)),
		island_sdf_at(isl, q + vec3(0, 0, e)) -
			island_sdf_at(isl, q - vec3(0, 0, e))));
}

// Island shading normal from the body's OWN stored normals: a trilinear blend of the eight
// compact samples at exactly the lattice coordinates/fractions island_sdf_at reconstructs
// with, normalized here in the LOCAL frame -- the caller rotates through isl.basis. An
// offset of -1 (no span published) or a degenerate blend falls back to differentiating the
// island's R8 lattice.
vec3 island_source_normal(Island isl, vec3 q, inout int steps_left) {
#if defined(FIELD_VOLUME_NORMAL_BINDING) && defined(FIELD_VOLUME_NORMAL_OFFSET_BINDING)
	if (isl.volume_slot >= 0 && isl.volume_slot < MAX_VOLUME_SLOTS) {
		int off = field_volume_normal_offsets.slot[isl.volume_slot];
		if (off >= 0) {
			int base = off >> 2;
			vec3 l = clamp((q - isl.lo) / isl.voxel, vec3(0.0), vec3(float(isl.dim - 1)));
			ivec3 i0 = ivec3(l);
			ivec3 i1 = min(i0 + 1, ivec3(isl.dim - 1));
			vec3 f = l - vec3(i0);
			int sy = isl.dim, sz = isl.dim * isl.dim;
			vec3 n000 = oct_decode_snorm8(pool_normal16(base, i0.x + i0.y * sy + i0.z * sz));
			vec3 n100 = oct_decode_snorm8(pool_normal16(base, i1.x + i0.y * sy + i0.z * sz));
			vec3 n010 = oct_decode_snorm8(pool_normal16(base, i0.x + i1.y * sy + i0.z * sz));
			vec3 n110 = oct_decode_snorm8(pool_normal16(base, i1.x + i1.y * sy + i0.z * sz));
			vec3 n001 = oct_decode_snorm8(pool_normal16(base, i0.x + i0.y * sy + i1.z * sz));
			vec3 n101 = oct_decode_snorm8(pool_normal16(base, i1.x + i0.y * sy + i1.z * sz));
			vec3 n011 = oct_decode_snorm8(pool_normal16(base, i0.x + i1.y * sy + i1.z * sz));
			vec3 n111 = oct_decode_snorm8(pool_normal16(base, i1.x + i1.y * sy + i1.z * sz));
			vec3 nx00 = mix(n000, n100, f.x);
			vec3 nx10 = mix(n010, n110, f.x);
			vec3 nx01 = mix(n001, n101, f.x);
			vec3 nx11 = mix(n011, n111, f.x);
			vec3 nxy0 = mix(nx00, nx10, f.y);
			vec3 nxy1 = mix(nx01, nx11, f.y);
			vec3 nxyz = mix(nxy0, nxy1, f.z);
			float len = length(nxyz);
			if (len > 1e-6) return nxyz / len;
		}
	}
#endif
	return island_r8_fallback_normal(isl, q, steps_left);
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
		float d = island_sdf_at(isl, q);
		if (d < 0.002) {
			for (int r = 0; r < 4; r++) { // secant refinement, as the terrain march does
				if (steps_left <= 0) return;
				steps_left--;
				q = ro_l + rd_l * t;
				t += island_sdf_at(isl, q) * 0.5;
			}
			if (t > best.t) return; // refinement pushed it behind the current winner
			q = ro_l + rd_l * t;
			best.hit = true;
			best.t = t;
			best.p = ro + rd * t;
			best.n = normalize(isl.basis *
					island_source_normal(isl, q, steps_left));
			best.mat = island_material_at(isl, q);
			return;
		}
		t += max(d * 0.9, 0.005);
	}
}

// The brick-level march, bounded to one region-DDA segment. The DDA is seeded from the
// segment start rather than recomputing from `ro`: a ray that crosses several regions must
// not revisit the bricks it already crossed. Local `t` is converted back to world distance
// only when writing the hit record and evaluating the ray position.
Hit march_bricks(int region_slot, ivec3 region_coord, vec3 ro, vec3 rd,
		float t_begin, float t_end, inout int steps_left) {
	Hit h;
	h.hit = false;
	h.t = t_end;
	h.p = vec3(0.0);
	h.n = vec3(0.0, 1.0, 0.0);
	h.mat = 0u;

	vec3 segment_ro = ro + rd * t_begin;
	float segment_length = max(t_end - t_begin, 0.0);
	ivec3 map = ivec3(floor(segment_ro / BRICK_SIZE));
	vec3 delta = abs(vec3(BRICK_SIZE) / rd);
	ivec3 st = ivec3(sign(rd));
	vec3 side = (vec3(map) * BRICK_SIZE - segment_ro +
			(vec3(st) * 0.5 + 0.5) * BRICK_SIZE) / rd;
	if (st.x == 0) side.x = 1.0 / 0.0;
	if (st.y == 0) side.y = 1.0 / 0.0;
	if (st.z == 0) side.z = 1.0 / 0.0;
	float t_prev = 0.0;

	for (int i = 0; i < 1024; i++) {
		float t_exit = min(side.x, min(side.y, side.z));
		g_brick_cells++;
		// Compare the brick ENTRY against the segment end. The segment end is itself a brick
		// face; testing `t_exit > segment_length` can drop that final brick by a float ULP,
		// leaving isolated holes at the far edge of a region.
		if (t_prev > segment_length) break;
		// The segment bounds are float arithmetic against a DDA whose `side` values
		// accumulate, so `map` can sit one brick outside this region at either end: a
		// segment start rounded a ULP short of the entry face, or a step past the exit one.
		// Indexing this region's table with such a brick's `& 31` coordinate would silently
		// address a DIFFERENT brick's atlas slot -- the hoisted region slot is only valid
		// for bricks actually inside the region. The region coordinate is exact integer
		// data, so ask it. Not a `break`: the neighbouring region's own march_bricks call
		// covers that brick, and breaking here would drop the rest of this segment.
		int slot = all(equal(map >> 5, region_coord)) ? slot_in_region(region_slot, map) : -1;
		uint bflags = slot >= 0 ? brick_flag_word(slot) : 0u;
		if ((bflags & BRICK_FLAG_HAS_SURFACE) != 0u) {
			bool has_material = (bflags & BRICK_FLAG_HAS_MATERIAL) != 0u;
			float t = t_prev;
			for (int j = 0; j < 64 && steps_left > 0; j++) {
				if (t > t_exit) break;
				vec3 p = segment_ro + rd * t;
				vec3 vox = (p - vec3(map) * BRICK_SIZE) / VOXEL_SIZE;
				// No 8^3 min-max gate here any more. It existed to skip a 0.1 m cell without
				// paying for the SDF, back when the SDF cost eight texture fetches. Now that
				// brick_sdf() is one filtered fetch the gate is a second fetch that buys a
				// SHORTER advance than the sphere-trace step it replaced: the stored field is
				// a narrow band clamped at SDF_RANGE, so a saturated sample already steps
				// 0.576 m, against the 0.1 m one cell face gives. Removing it is also
				// strictly sound -- sphere tracing skips nothing the gate would have caught.
				if (steps_left <= 0) break;
				steps_left--;
				// This is the marcher's hottest read. world_sdf() would re-derive the brick
				// from `p` and walk region_map -> region_tables to find the slot again --
				// two DEPENDENT buffer loads ahead of the eight atlas fetches -- when the
				// DDA already knows both the brick (`map`) and its slot, and `vox` is
				// already that brick's local coordinate. brick_sdf() clamps into the
				// 17-voxel apron, so a `p` that drifted a ULP past the exit face reads the
				// shared face value rather than the neighbour's slot: the same number
				// sdf_near() relies on for its gradient taps.
				float d = brick_sdf(slot, vox);
				if (d < 0.002 && has_material) {
					for (int k = 0; k < 4; k++) {
						if (steps_left <= 0) return h;
						steps_left--;
						float dk = brick_sdf(slot, (p - vec3(map) * BRICK_SIZE) / VOXEL_SIZE);
						t += dk * 0.5;
						p = segment_ro + rd * t;
					}
					h.hit = true;
					h.t = t_begin + t;
					h.p = p;
					h.n = terrain_source_normal(p, map, slot, steps_left);
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

// The three-level traversal from spec section 3: region DDA (25.6 m), brick DDA (0.8 m),
// then in-brick sphere tracing. A region without a table, or with no resident bricks, is
// known empty and is crossed without paying for its brick indirection lookups.
Hit march_terrain(vec3 ro, vec3 rd, float max_dist, inout int steps_left) {
	Hit h;
	h.hit = false;
	h.t = max_dist;
	h.p = vec3(0.0);
	h.n = vec3(0.0, 1.0, 0.0);
	h.mat = 0u;

	ivec3 rmap = ivec3(floor(ro / REGION_SIZE));
	vec3 rdelta = abs(vec3(REGION_SIZE) / rd);
	ivec3 rst = ivec3(sign(rd));
	vec3 rside = (vec3(rmap) * REGION_SIZE - ro +
			(vec3(rst) * 0.5 + 0.5) * REGION_SIZE) / rd;
	if (rst.x == 0) rside.x = 1.0 / 0.0;
	if (rst.y == 0) rside.y = 1.0 / 0.0;
	if (rst.z == 0) rside.z = 1.0 / 0.0;
	float rt_prev = 0.0;

	for (int r = 0; r < 64; r++) {
		float rt_exit = min(rside.x, min(rside.y, rside.z));
		if (rt_prev > max_dist) break;
		g_region_cells++;

		// `region_slot_of` takes a brick coordinate. Multiplication by 32 picks the
		// first brick of this region and is the inverse of its `>> 5` mapping.
		int rs = region_slot_of(rmap * REGION_BRICKS);
		bool region_worth_entering = rs >= 0 && region_slot_counts.n[rs] > 0;
		if (region_worth_entering) {
			Hit candidate = march_bricks(rs, rmap, ro, rd, max(rt_prev, 0.0),
					min(rt_exit, max_dist), steps_left);
			if (candidate.hit) return candidate;
			if (steps_left <= 0) return h;
		}

		if (rside.x < rside.y && rside.x < rside.z) {
			rt_prev = rside.x; rside.x += rdelta.x; rmap.x += rst.x;
		} else if (rside.y < rside.z) {
			rt_prev = rside.y; rside.y += rdelta.y; rmap.y += rst.y;
		} else {
			rt_prev = rside.z; rside.z += rdelta.z; rmap.z += rst.z;
		}
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

float terrain_sun_visibility(vec3 ro, float max_shadow_dist) {
	float res = 1.0;
	float t = 0.05;
	for (int i = 0; i < RAY_SHADOW_STEPS; i++) {
		if (t > max_shadow_dist) break;
		vec3 q = ro + sun_light.dir.xyz * t;
		ivec3 brick = ivec3(floor(q / BRICK_SIZE));
		int shadow_region = region_slot_of(brick);
		if (shadow_region < 0) return 1.0;
		// The light supplies a normalized direction; the caller must avoid an exactly-zero
		// component because this far-face division has no primary-DDA-style zero guard. A
		// normalized node basis effectively never produces one, and ray_box already tolerates
		// the infinities. A resident but empty region cannot contain an occluder; skip its whole
		// 25.6 m cell.
		if (region_slot_counts.n[shadow_region] == 0) {
			vec3 rlo = floor(q / REGION_SIZE) * REGION_SIZE;
			vec3 rhi = rlo + vec3(REGION_SIZE);
			vec3 far = mix(rlo, rhi, step(0.0, sun_light.dir.xyz));
			vec3 tf = (far - q) / sun_light.dir.xyz;
			float skip = min(tf.x, min(tf.y, tf.z));
			t += max(skip, 0.01) + 0.001;
			continue;
		}
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
float island_sun_visibility(vec3 ro, int island_count, float max_shadow_dist) {
	float res = 1.0;
	int marched = 0;
	for (int i = 0; i < 32; i++) {
		if (i >= island_count || marched >= RAY_SHADOW_MAX_ISLANDS) break;
		vec3 lo = island_desc.v[i * 8 + 5].xyz;
		vec3 hi = island_desc.v[i * 8 + 6].xyz;
		float t0, t1;
		if (!ray_box(ro, sun_light.dir.xyz, lo, hi, t0, t1)) continue;
		Island isl;
		if (!island_load(i, isl)) continue;
		marched++;
		mat3 inv = transpose(isl.basis);
		vec3 ro_l = inv * (ro - isl.pos);
		vec3 rd_l = inv * sun_light.dir.xyz;
		float t = max(t0, 0.05);
		float tmax = min(t1, max_shadow_dist);
		for (int k = 0; k < 48; k++) {
			if (t > tmax) break;
			float d = island_sdf_at(isl, ro_l + rd_l * t);
			if (d < 0.004) return 0.0;
			// Same narrow band, same saturation, same cutoff as terrain_sun_visibility.
			if (t <= RAY_SHADOW_PENUMBRA_DIST) res = min(res, RAY_SHADOW_K * d / t);
			t += clamp(d, 0.02, 1.0);
		}
	}
	return clamp(res, 0.0, 1.0);
}

// Fold one more overlay over the ones already accumulated. Two mixes over the same base are
// a single mix over that base -- mix(mix(b, c, w), nc, nw) = mix(b, c', w') with
// w' = 1 - (1 - w)(1 - nw) -- which is what lets the composite apply the lot AFTER it
// resolves the material the marcher never touched.
void overlay_mix(inout vec3 c, inout float w, vec3 nc, float nw) {
	float keep = w * (1.0 - nw);
	float total = keep + nw;
	if (total > 0.0) c = (c * keep + nc * nw) / total;
	w = total;
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

	// Sky and misses: material 0 means "no voxel here", and the sky IS the whole pixel, so it
	// goes into the overlay at full weight. The composite writes it through unchanged and the
	// deferred pass passes material 0 on unlit -- sky_color() still lives in one place.
	vec3 overlay = sky_color(rd);
	float overlay_w = 1.0;
	vec2 oct = oct_encode(-rd);
	float mat_id = 0.0;
	float sun = 1.0;
	vec4 hitpos = vec4(0.0);

	if (best.hit) {
		// A hit is the material's pixel, not the ray's: the overlay starts empty and the
		// composite resolves the albedo, the gloss and the AO from what is stored below.
		overlay = vec3(0.0);
		overlay_w = 0.0;
		oct = oct_encode(best.n);
		mat_id = float(best.mat);
		hitpos = vec4(best.p, 1.0);
		if ((flags & BEAUTY_RAY_SUN_SHADOW) != 0u) {
			// One voxel of offset: less and the ray self-shadows on its own surface, more
			// and thin ledges stop casting.
			vec3 sro = best.p + best.n * 0.06;
			// FULL reach, sun map or not. The map was once trusted to answer for the
			// 7.68-60 m stretch, so the march stopped at the penumbra band; but the map is
			// rasterized from the LoD mesh and shades only the pixels that mesh drew (see
			// far_field_owns in shaders/deferred.comp.glsl), so for a near-field pixel this
			// march is the ONLY shadow term there is. Stopping it early left an occluder
			// 25 m up-sun casting nothing.
			sun = min(terrain_sun_visibility(sro, RAY_SHADOW_DIST),
					island_sun_visibility(sro, island_count, RAY_SHADOW_DIST));
		}
		if ((flags & BEAUTY_GLOSSY_RAYS) != 0u) {
			// The only surviving material read on the primary path, and it is gated on the
			// feature that needs it: a reflection is a property of the RAY, so it has to be
			// resolved where the ray is, and its strength depends on the surface's gloss.
			// The pixel's world footprint at the hit -- the ray direction's screen derivative
			// scaled by distance -- is good enough for a reflection at march resolution.
			vec3 ddx = pc.cam_right.xyz * (2.0 * pc.params.x / float(size.x)) * best.t;
			vec3 ddy = pc.cam_up.xyz * (2.0 * pc.params.y / float(size.y)) * best.t;
			float gloss = pc.params.w < -0.5
					? 1.0
					: 1.0 - material_props(best.mat, best.p, best.n, ddx, ddy).x;
			if (gloss > GLOSSY_SDF_MIN_GLOSS) {
				vec3 rr = normalize(reflect(rd, best.n));
				vec3 rro = best.p + best.n * GLOSSY_SDF_BIAS;
				int reflected_steps = GLOSSY_SDF_STEPS;
				Hit reflected = march_terrain(rro, rr, GLOSSY_SDF_MAX_DIST, reflected_steps);
				// A reflected ray leaves the primary tile, so its tile mask is invalid.
				// AABB-reject every live island descriptor, sharing the same remaining
				// 64-step budget.
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
				overlay_mix(overlay, overlay_w, reflected_albedo, weight);
			}
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
		overlay_mix(overlay, overlay_w, tint, 0.45);
	}

	// One heat unit per primary marching step; 512 heat units is white. Clamp the integer
	// step count before scaling so zero steps is exactly black and excess work is white.
	float heat_units = clamp(float(65536 - primary_steps), 0.0, 512.0);
	float cost_heat = heat_units / 512.0;

	// Debug material probe: a 1x1 dispatch calls material_surface() directly with zero
	// gradients. pc.params.w is otherwise unused, so > 0 is the probe flag.
	if (pc.params.w > 0.0) {
		vec3 probe_n = normalize(pc.cam_fwd.xyz);
		vec4 surf = material_surface(uint(pc.params.w), pc.cam_pos.xyz,
				probe_n, vec3(0.0), vec3(0.0));
		vec3 probe_shading_n;
		vec2 props = material_props_normal(uint(pc.params.w), pc.cam_pos.xyz,
				probe_n, vec3(0.0), vec3(0.0), probe_shading_n);
		int cost_i = (px.y * size.x + px.x) * 2;
		cost_out.v[cost_i + 0] = uint(65536 - primary_steps);
		cost_out.v[cost_i + 1] =
				(g_brick_cells & 0xFFFFu) | (min(g_region_cells, 0xFFFFu) << 16);
		vec3 probe_albedo = (flags & BEAUTY_COST_VIEW) != 0u ? vec3(cost_heat) : surf.rgb;
		imageStore(out_albedo, px, vec4(probe_albedo, 1.0));
		// The oct slots carry the roughness and the AO instead of a normal: this dispatch has
		// no geometry, and they are what the composite folds into a resolved pixel, so a
		// caller can reproduce that fold from one probe rather than guessing at it.
		imageStore(out_surface, px, vec4(props.x, props.y, pc.params.w, 0.0));
		// The hit position is the probe's own input, so the slot is free: it carries the
		// SHADING normal the map produces at zero gradients -- mip 0, no render path, no
		// geometry -- which is the only way to ask what the art itself says.
		imageStore(out_hitpos, px, vec4(probe_shading_n, 1.0));
		return;
	}

	// The cost view replaces the whole pixel, so it is an overlay at full weight -- which also
	// bypasses the AO fold and the sun, as it always has. Everything else in the G-buffer
	// stays truthful: depth, normal, material and sun visibility are written exactly as they
	// would be, so the depth test, the outlines and the LoD seam behave normally and the view
	// can be toggled mid-flight.
	if ((flags & BEAUTY_COST_VIEW) != 0u) {
		overlay = vec3(cost_heat);
		overlay_w = 1.0;
		sun = 1.0;
	}

	int cost_i = (px.y * size.x + px.x) * 2;
	cost_out.v[cost_i + 0] = uint(65536 - primary_steps);
	cost_out.v[cost_i + 1] = (g_brick_cells & 0xFFFFu) | (min(g_region_cells, 0xFFFFu) << 16);
	imageStore(out_albedo, px, vec4(overlay, sun));
	imageStore(out_surface, px, vec4(oct, mat_id, overlay_w));
	imageStore(out_hitpos, px, hitpos);
}
