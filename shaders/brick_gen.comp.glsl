#[compute]
#version 460
#include "generated/blocks.glslh"

#define FIELD_OP_POOL_BINDING 7
#define FIELD_VOLUME_SDF_BINDING 8
#define FIELD_VOLUME_MAT_BINDING 9
#define FIELD_OVERRIDE_SDF_BINDING 11
#define FIELD_OVERRIDE_MAT_BINDING 12
#define FIELD_OVERRIDE_TABLE_BINDING 13
#define FIELD_OVERRIDE_REGION_BINDING 14
#include "common.glslh"
#define FIELD_OVERRIDE_TABLE(base) (field_override_region_map.table[int((base) / MAX_REGION_OPS)])
// The region's ops that can reach THIS brick, in append order, as pool-relative indices.
// The filter is compacted serially after the parallel keep test so CSG order is preserved.
shared uint s_ops[256];
shared uint s_op_n;
shared uint s_keep[256];
shared uint s_liquid;
// Non-zero when an op that moves the surface touches this brick (see phase 6).
shared uint s_edited;
// Non-zero once anything in this brick disqualifies it from the baked-normal cache (see
// phase 6). Racing writes of the same value are harmless: it is only read after a barrier.
shared uint s_cache_bad;
// The conifer stage's per-brick cell cache (stages/conifers.field.glslh). Declared here for
// every pipeline because only a compute shader may declare shared memory; a pipeline without
// the stage never touches it.
#define FIELD_CONIFER_CELL_CACHE 1
shared vec4 s_conifer_a[16];
shared vec4 s_conifer_b[16];
#define FIELD_OP_INDEX(base, i) ((base) + s_ops[i])
#include "field.glslh"
#include "brick_layout.glslh"
#include "brick_cache.glslh"
#include "opaque_view.glslh"

// One workgroup per brick; 256 threads stride over the brick's 4096 cells and 4913 lattice
// samples. Mirror of ve::eval_brick (extension/src/world/brick_eval.cpp).
layout(local_size_x = 256) in;

// The SDF atlas is read back within this dispatch (material projection and the min-max
// reduction both need the lattice just written), so it is coherent read-write, not writeonly.
layout(set = 0, binding = 0, r8) coherent uniform image3D sdf_atlas;
layout(set = 0, binding = 1, r8ui) writeonly uniform uimage3D mat_atlas;
layout(set = 0, binding = 2, rg8ui) writeonly uniform uimage3D mip2_atlas;
layout(set = 0, binding = 3, rg8ui) writeonly uniform uimage3D mip4_atlas;
layout(set = 0, binding = 4, rg8ui) writeonly uniform uimage3D mip8_atlas;
layout(set = 0, binding = 5, std430) buffer Palette { uint id[]; } palette_buf;
layout(set = 0, binding = 6, std430) readonly buffer Jobs { ivec4 v[]; } jobs;
// binding 7 is the field op pool, declared by field.glslh
layout(set = 0, binding = 10, std430) writeonly buffer BrickFlags { uint v[]; } brick_flags;
layout(set = 0, binding = 15, std430) buffer RegionOccupancy { uint w[]; } occupancy;
// Task 2: the baked-normal cache, written in phase 6. The two flag buffers say whether a
// slot's entry is valid; they are not part of the texture, so an ungenerated slot's stale
// bytes are unambiguous.
layout(set = 0, binding = 16, rgba16_snorm) writeonly uniform image3D brick_cache;
layout(set = 0, binding = 17, std430) writeonly buffer CacheNormalOk { uint v[]; } normal_ok;
layout(set = 0, binding = 18, std430) writeonly buffer CacheSunOk { uint v[]; } sun_ok;

// Phase 6's bake result, kept in shared memory so neighbouring lattice points can be compared
// after a barrier.
const int kCacheSamples = BRICK_CACHE_DIM * BRICK_CACHE_DIM * BRICK_CACHE_DIM;
shared vec3 s_cache_n[kCacheSamples];
shared float s_cache_d[kCacheSamples];

const uint CELL_AIR = 1u;
const uint CELL_SOLID = 2u;
const uint CELL_FULL = 3u;

layout(push_constant, std430) uniform Push { BRICK_GEN_PUSH_FIELDS } pc;

shared uint s_mat[BRICK_VOXEL_COUNT]; // global material id per cell, 0 = none
shared uint s_pal[4];                 // palette in insertion order
shared uint s_cnt[4];                 // cells charged to each insertion-order slot
shared uint s_inv[4];                 // insertion-order slot -> final packed index
shared uint s_mip8[512];              // (min << 8) | max
shared uint s_mip4[64];
// Encoded min / max of the SOLID view (water spec §3): the occupancy grid is classified from
// these, not from the stored (opaque-view) lattice -- connectivity keeps seeing ice as solid
// and sees a liquid as air. With no liquid they are the union lattice's range.
shared uint s_umin;
shared uint s_umax;

float lat(ivec3 base, ivec3 v) {
	return decode_sdf(imageLoad(sdf_atlas, base + v).r);
}

// Central difference over the span actually sampled. On a brick's outer planes the lattice
// has no neighbour on one side, so the difference is one-sided over a single voxel; dividing
// every axis by a fixed 2 would halve that component and tilt the projection sideways across
// a material band instead of straight down onto the surface underneath.
float slope_axis(ivec3 base, ivec3 v, int axis) {
	ivec3 lo = v, hi = v;
	lo[axis] = max(lo[axis] - 1, 0);
	hi[axis] = min(hi[axis] + 1, BRICK_VOXELS);
	float span = float(hi[axis] - lo[axis]) * VOXEL_SIZE;
	return (lat(base, hi) - lat(base, lo)) / span;
}

int nearest_palette_slot(uint m) {
	int best = 0;
	int bd = abs(int(s_pal[0]) - int(m));
	for (int k = 1; k < 4; k++) {
		int d = abs(int(s_pal[k]) - int(m));
		if (d < bd) { bd = d; best = k; }
	}
	return best;
}

// Mirror of ve::palette_slot's insert-or-nearest, with an occupancy count on the side.
void insert_material(uint m) {
	for (int k = 0; k < 4; k++) {
		uint prev = atomicCompSwap(s_pal[k], 0u, m);
		if (prev == 0u || prev == m) { atomicAdd(s_cnt[k], 1u); return; }
	}
	atomicAdd(s_cnt[nearest_palette_slot(m)], 1u); // a 5th material: charged to the nearest
}

uint resolve_index(uint m) {
	for (int k = 0; k < 4; k++) if (s_pal[k] == m) return s_inv[k];
	return s_inv[nearest_palette_slot(m)];
}

ivec3 cell_coord(uint i) {
	return ivec3(int(i) % BRICK_VOXELS, (int(i) / BRICK_VOXELS) % BRICK_VOXELS,
			int(i) / (BRICK_VOXELS * BRICK_VOXELS));
}

// The solid view's distance at a lattice point whose union sample is (sdf, mat). Without a
// liquid-capable op the solid view IS the union under the label rule, so only bricks such an
// op reaches pay for a second evaluation. Mirror of ve::cell_state_field.
float solid_sample(vec3 p, uint op_base, float sdf, uint mat) {
	float ssdf = sdf;
	uint smat = mat;
	if (s_liquid != 0u) eval_field_solid(p, op_base, s_op_n, ssdf, smat);
	else solid_view(ssdf, smat);
	return ssdf;
}

void main() {
	uint tid = gl_LocalInvocationID.x;
	ivec4 j0 = jobs.v[int(gl_WorkGroupID.x) * 2 + 0];
	ivec4 j1 = jobs.v[int(gl_WorkGroupID.x) * 2 + 1];
	ivec3 brick = j0.xyz;
	int slot = j0.w;
	if (slot < 0) return;
	uint op_base = uint(j1.x) * MAX_REGION_OPS;
	uint op_count = uint(j1.y);

	// Test in parallel, compact serially. atomicAdd would compact too, but it would also
	// scramble the order, and an ordered CSG list whose order is gone is a different world.
	vec3 brick_lo = vec3(brick) * BRICK_SIZE;
	vec3 brick_hi = brick_lo + vec3(BRICK_SIZE);
	if (tid < op_count)
		s_keep[tid] = op_touches_aabb(op_base + tid, brick_lo, brick_hi, BRICK_FILTER_PAD) ? 1u : 0u;
	if (tid == 0u) s_op_n = 0u;
	barrier();
	if (tid == 0u) {
		uint n = 0u;
		uint liquid = 0u;
		uint edited = 0u;
		for (uint i = 0u; i < op_count; i++)
			if (s_keep[i] != 0u) {
				s_ops[n++] = i;
				if (op_may_hold_liquid(op_base + i)) liquid = 1u;
				// Paint changes a label, never the surface, so it leaves no crease (phase 6).
				if (field_op_pool.v[(op_base + i) * 2u].x != OP_SPHERE_PAINT) edited = 1u;
			}
		s_op_n = n;
		s_liquid = liquid;
		s_edited = edited;
	}
	barrier();

	ivec3 sdf_base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_SDF_STRIDE);
	ivec3 mat_base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_VOXELS);
	vec3 bo = vec3(brick) * BRICK_SIZE;

	if (tid < 4u) { s_pal[tid] = 0u; s_cnt[tid] = 0u; s_inv[tid] = tid; }
	if (tid == 0u) { s_umin = 255u; s_umax = 0u; s_cache_bad = 0u; }
#ifdef CONIFERS_STAGE
	conifer_cell_cache_fill(bo, tid);
#endif
	memoryBarrierShared();
	barrier();

	// Phase 1a: the 16^3 cell lattice — SDF plus each cell's own material sample.
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		ivec3 v = cell_coord(i);
		float sdf;
		uint mat;
		float osdf;
		uint omat;
		eval_field_pair(bo + vec3(v) * VOXEL_SIZE, op_base, s_op_n, sdf, mat, osdf, omat);
		// The cache stores the UNION gradient, so a brick whose union holds a transparent
		// material must not bake one: the marcher sees the opaque view there (that material
		// is air), and the union gradient at the surface it hit belongs to the ice instead.
		// `mat` is the union material; the s_mat stored below is the opaque view's.
		if (mat_transparent(mat)) atomicOr(s_cache_bad, 1u);
		uint ub = encode_sdf_byte(solid_sample(bo + vec3(v) * VOXEL_SIZE, op_base, sdf, mat));
		atomicMin(s_umin, ub);
		atomicMax(s_umax, ub);
		// The atlas holds the OPAQUE VIEW with the feature on and the UNION with it off, so
		// select one pair -- never both: the opaque pair has already skipped the transparent
		// ops, and with the feature off they must be baked like any other material.
		if (pc.atlas_bricks.w != 0) opaque_view(osdf, omat, OPAQUE_OUTSIDE);
		else { osdf = sdf; omat = mat; }
		imageStore(sdf_atlas, sdf_base + v, vec4(quantise_sdf(osdf)));
		s_mat[i] = omat;
	}
	memoryBarrierShared();
	barrier();

	// Phase 1b: the apron planes at local 16 (17^3 - 16^3 = 817 samples). Their SDF completes
	// the trilinear cell at the brick's positive faces; their material seeds the cell the
	// coordinate clamp folds them into, but only where that cell sampled nothing of its own —
	// a cell's own sample always wins, which is what the compare-and-swap against 0 encodes.
	for (uint i = tid; i < uint(BRICK_SDF_COUNT); i += 256u) {
		ivec3 v = ivec3(int(i) % BRICK_SDF_STRIDE,
				(int(i) / BRICK_SDF_STRIDE) % BRICK_SDF_STRIDE,
				int(i) / (BRICK_SDF_STRIDE * BRICK_SDF_STRIDE));
		if (v.x < BRICK_VOXELS && v.y < BRICK_VOXELS && v.z < BRICK_VOXELS) continue;
		float sdf;
		uint mat;
		float osdf;
		uint omat;
		eval_field_pair(bo + vec3(v) * VOXEL_SIZE, op_base, s_op_n, sdf, mat, osdf, omat);
		uint ub = encode_sdf_byte(solid_sample(bo + vec3(v) * VOXEL_SIZE, op_base, sdf, mat));
		atomicMin(s_umin, ub);
		atomicMax(s_umax, ub);
		if (pc.atlas_bricks.w != 0) opaque_view(osdf, omat, OPAQUE_OUTSIDE);
		else { osdf = sdf; omat = mat; }
		imageStore(sdf_atlas, sdf_base + v, vec4(quantise_sdf(osdf)));
		if (omat == 0u) continue;
		ivec3 c = min(v, ivec3(BRICK_VOXELS - 1));
		atomicCompSwap(s_mat[c.x + c.y * BRICK_VOXELS + c.z * BRICK_VOXELS * BRICK_VOXELS],
				0u, omat);
	}
	memoryBarrierImage();
	memoryBarrierShared();
	barrier();

	// Phase 2: project near-surface air cells onto the surface and ask the field for the
	// material there (ve::spread_materials). Purely per-cell: no thread reads another's cell.
	const float project_range = 2.0 * VOXEL_SIZE;
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		if (s_mat[i] != 0u) continue;
		ivec3 v = cell_coord(i);
		float d = lat(sdf_base, v);
		if (d > project_range) continue;
		vec3 g = vec3(slope_axis(sdf_base, v, 0), slope_axis(sdf_base, v, 1),
				slope_axis(sdf_base, v, 2));
		float len = length(g);
		if (len <= 0.0) continue;
		// The stored SDF is uint8-quantised and the gradient is a difference of those, so a
		// single step can fall short of the surface; lengthen it and retry.
		for (float over = 0.5; over <= 2.5 && s_mat[i] == 0u; over += 1.0) {
			float t = d + over * VOXEL_SIZE;
			float sdf2;
			// mat2 is a GLSL reserved word (the 2x2 matrix type), so the plan's variable
			// name is rejected by glslang; renamed to matB, no semantic change.
			uint matU;
			float osdf;
			uint matB;
			eval_field_pair(bo + vec3(v) * VOXEL_SIZE - g / len * t, op_base, s_op_n, sdf2,
					matU, osdf, matB);
			if (pc.atlas_bricks.w != 0) opaque_view(osdf, matB, OPAQUE_OUTSIDE);
			else matB = matU;
			s_mat[i] = matB;
		}
	}
	memoryBarrierShared();
	barrier();

	// Phase 3: build the palette, then order it by occupancy so slot 0 is the dominant
	// material (ve::palette_occupancy_order). Two rounds: insertion is unordered, and the
	// ordering cannot be decided until every cell has been counted.
	// Most terrain bricks have only one material. Inserting every cell into the
	// same palette slot serializes thousands of shared atomics. Detect that exact
	// case with a reduction; mixed bricks retain the original palette algorithm.
	// The mip scratch is unused until phase 5, so this costs no extra shared memory.
	uint material_min = 0xffffffffu, material_max = 0u;
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		uint m = s_mat[i];
		if (m != 0u) material_min = min(material_min, m);
		material_max = max(material_max, m);
	}
	s_mip8[tid] = material_min;
	s_mip8[tid + 256u] = material_max;
	barrier();
	for (uint stride = 128u; stride > 0u; stride >>= 1u) {
		if (tid < stride) {
			s_mip8[tid] = min(s_mip8[tid], s_mip8[tid + stride]);
			s_mip8[tid + 256u] = max(s_mip8[tid + 256u], s_mip8[tid + 256u + stride]);
		}
		barrier();
	}
	if (s_mip8[256] == 0u || s_mip8[0] == s_mip8[256]) {
		if (tid == 0u) s_pal[0] = s_mip8[256];
	} else {
		for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u)
			if (s_mat[i] != 0u) insert_material(s_mat[i]);
	}
	memoryBarrierShared();
	barrier();
	if (tid == 0u) {
		uint order[4] = uint[4](0u, 1u, 2u, 3u);
		for (int a = 0; a < 4; a++)
			for (int b = a + 1; b < 4; b++) {
				uint ia = order[a], ib = order[b];
				bool a_empty = s_pal[ia] == 0u, b_empty = s_pal[ib] == 0u;
				bool swap = false;
				if (a_empty != b_empty) swap = a_empty;
				else if (!a_empty) swap = s_cnt[ib] > s_cnt[ia] ||
						(s_cnt[ib] == s_cnt[ia] && s_pal[ib] < s_pal[ia]);
				if (swap) { order[a] = ib; order[b] = ia; }
			}
		for (int a = 0; a < 4; a++) {
			s_inv[order[a]] = uint(a);
			palette_buf.id[slot * 4 + a] = s_pal[order[a]];
		}
	}
	// Palette is an SSBO: shared-memory fencing alone does not make tid 0's writes visible
	// before the final flag publication (or to the later raymarch dispatch). Fence the buffer
	// before the workgroup rendezvous; the renderer's dispatch barrier handles cross-dispatch
	// visibility after this generator completes.
	memoryBarrierBuffer();
	memoryBarrierShared();
	barrier();

	// Phase 4: pack the material atlas. Cells with no material keep index 0, which the
	// occupancy ordering has made the brick's dominant material.
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		ivec3 v = cell_coord(i);
		uint idx = s_mat[i] == 0u ? 0u : resolve_index(s_mat[i]);
		imageStore(mat_atlas, mat_base + v, uvec4(idx, 0u, 0u, 0u));
	}

	// Phase 5: the min-max chain. The 8^3 level reads the lattice directly — cell (i,j,k)
	// covers voxels [2i, 2i+2), whose trilinear corners are lattice samples [2i, 2i+2]
	// INCLUSIVE, a 3^3 block. Coarser levels reduce the level below.
	ivec3 b8 = atlas_base(slot, pc.atlas_bricks.xyz, 8);
	for (uint i = tid; i < 512u; i += 256u) {
		ivec3 c = ivec3(int(i) % 8, (int(i) / 8) % 8, int(i) / 64);
		uint mn = 255u, mx = 0u;
		for (int z = 0; z <= 2; z++)
			for (int y = 0; y <= 2; y++)
				for (int x = 0; x <= 2; x++) {
					uint s = uint(imageLoad(sdf_atlas, sdf_base + c * 2 + ivec3(x, y, z)).r *
							255.0 + 0.5);
					mn = min(mn, s);
					mx = max(mx, s);
				}
		s_mip8[i] = (mn << 8) | mx;
		imageStore(mip8_atlas, b8 + c, uvec4(mn, mx, 0u, 0u));
	}
	memoryBarrierShared();
	barrier();

	ivec3 b4 = atlas_base(slot, pc.atlas_bricks.xyz, 4);
	for (uint i = tid; i < 64u; i += 256u) {
		ivec3 c = ivec3(int(i) % 4, (int(i) / 4) % 4, int(i) / 16);
		uint mn = 255u, mx = 0u;
		for (int z = 0; z < 2; z++)
			for (int y = 0; y < 2; y++)
				for (int x = 0; x < 2; x++) {
					uint p = s_mip8[(2 * c.x + x) + (2 * c.y + y) * 8 + (2 * c.z + z) * 64];
					mn = min(mn, p >> 8);
					mx = max(mx, p & 255u);
				}
		s_mip4[i] = (mn << 8) | mx;
		imageStore(mip4_atlas, b4 + c, uvec4(mn, mx, 0u, 0u));
	}
	memoryBarrierShared();
	barrier();

	ivec3 b2 = atlas_base(slot, pc.atlas_bricks.xyz, 2);
	for (uint i = tid; i < 8u; i += 256u) {
		ivec3 c = ivec3(int(i) % 2, (int(i) / 2) % 2, int(i) / 4);
		uint mn = 255u, mx = 0u;
		for (int z = 0; z < 2; z++)
			for (int y = 0; y < 2; y++)
				for (int x = 0; x < 2; x++) {
					uint p = s_mip4[(2 * c.x + x) + (2 * c.y + y) * 4 + (2 * c.z + z) * 16];
					mn = min(mn, p >> 8);
					mx = max(mx, p & 255u);
				}
		imageStore(mip2_atlas, b2 + c, uvec4(mn, mx, 0u, 0u));
	}

	memoryBarrierShared();
	barrier();
	if (tid == 0u) {
		uint mn = 255u, mx = 0u;
		for (int i = 0; i < 64; i++) {
			mn = min(mn, s_mip4[i] >> 8);
			mx = max(mx, s_mip4[i] & 255u);
		}
		// Mirror of ve::brick_flags_from_mips: the same inclusive straddle test, and the
		// same "palette slot 0 is id 0 means no material" rule the marcher's hit test uses.
		uint f = 0u;
		if (mn <= ENCODED_ZERO && mx >= ENCODED_ZERO) f |= BRICK_FLAG_HAS_SURFACE;
		if (palette_buf.id[slot * 4] != 0u) f |= BRICK_FLAG_HAS_MATERIAL;
		brick_flags.v[slot] = f;

		// The occupancy grid is classified from the exact lattice this dispatch generated,
		// not brick_mark's conservative 3x3x3 probe. The job already carries the region slot
		// in j1.x, while the brick coordinate determines the stable index within that region.
		uint state = s_umin > ENCODED_ZERO ? CELL_AIR :
				(s_umax <= ENCODED_ZERO ? CELL_FULL : CELL_SOLID);
		int rslot = j1.x;
		int bi = (brick.x & (REGION_BRICKS - 1)) +
				(brick.y & (REGION_BRICKS - 1)) * REGION_BRICKS +
				(brick.z & (REGION_BRICKS - 1)) * REGION_BRICKS * REGION_BRICKS;
		int occ_word = rslot * OCC_WORDS_PER_REGION + (bi >> 4);
		uint occ_shift = (uint(bi) & 15u) * 2u;
		atomicAnd(occupancy.w[occ_word], ~(3u << occ_shift));
		atomicOr(occupancy.w[occ_word], (state & 3u) << occ_shift);
	}

	// Phase 6: bake the normal cache (Task 2). The marcher reads one filtered fetch of this
	// instead of re-running the pipeline for the hit normal, so each of the 125 lattice
	// points bakes exactly what the marcher would have evaluated there: eval_field_gradient
	// over this region's op span. The points lie inside the brick plus its +16 face, which
	// the conifer cell cache filled above already covers.
	//
	// The brick is a MISS -- normal_ok stays 0 and nothing is stored -- when a sample's
	// gradient is inexact or degenerate, when a liquid op shaped this brick (the solid view
	// differs), when a cell holds a transparent material (see phase 1a), or when two adjacent
	// near-surface samples are opposed (the far side of a thin shell, below). This shader
	// binds no stored-normal buffers, so volumes and overrides report an inexact gradient and
	// land here by design.
	if (tid < uint(kCacheSamples)) {
		ivec3 l = ivec3(int(tid) % BRICK_CACHE_DIM,
				(int(tid) / BRICK_CACHE_DIM) % BRICK_CACHE_DIM,
				int(tid) / (BRICK_CACHE_DIM * BRICK_CACHE_DIM));
		float csdf;
		uint cmat;
		vec3 grad;
		bool exact;
		eval_field_gradient(bo + vec3(l) * (float(BRICK_CACHE_STEP) * VOXEL_SIZE), op_base,
				s_op_n, csdf, cmat, grad, exact);
		float len = length(grad);
		vec3 n = len > 1e-8 ? grad / len : vec3(0.0);
		if (!exact || len < 1e-8) atomicOr(s_cache_bad, 1u);
		else imageStore(brick_cache, brick_cache_texel(slot, l, pc.atlas_bricks.xyz),
				vec4(n, 1.0));
		s_cache_n[tid] = n;
		s_cache_d[tid] = csdf;
	}
	memoryBarrierShared();
	barrier();
	// A shell thinner than the 20 cm lattice cannot be represented here: a sample lands on the
	// outer surface and points into the air, while its neighbour 20 cm away inside the shell
	// points the other way. Any axis-adjacent near-surface pair more than 120 degrees apart
	// makes the brick a miss, so it takes the analytic path. Ordinary creases (trunk/ground,
	// simpler bends) stay well inside that and remain cached. Deep points are exempt: the
	// trunk core's radial gradient rotates fast inside the 0.45 m trunk but nothing there is
	// ever read.
	if (tid < uint(kCacheSamples)) {
		int i = int(tid);
		ivec3 l = ivec3(i % BRICK_CACHE_DIM, (i / BRICK_CACHE_DIM) % BRICK_CACHE_DIM,
				i / (BRICK_CACHE_DIM * BRICK_CACHE_DIM));
		float near_m = float(BRICK_CACHE_STEP) * VOXEL_SIZE;
		for (int axis = 0; axis < 3; axis++) {
			if (l[axis] + 1 >= BRICK_CACHE_DIM) continue;
			int j = i + (axis == 0 ? 1 : (axis == 1 ? BRICK_CACHE_DIM
					: BRICK_CACHE_DIM * BRICK_CACHE_DIM));
			if (abs(s_cache_d[i]) > near_m || abs(s_cache_d[j]) > near_m) continue;
			if (dot(s_cache_n[i], s_cache_n[j]) < -0.5) atomicOr(s_cache_bad, 1u);
		}
	}
	memoryBarrierShared();
	barrier();
	if (tid == 0u) {
		// An edited brick is CACHE_EDITED, a miss for both entries: its CSG crease puts lattice
		// points beside the hit whose nearest surface is another face (world/brick.h).
		normal_ok.v[slot] = s_edited != 0u ? CACHE_EDITED
				: (s_cache_bad == 0u && s_liquid == 0u ? CACHE_OK : 0u);
		// The brick itself changed, so any sun entry (Task 3) is stale from this moment.
		sun_ok.v[slot] = 0u;
	}
}
