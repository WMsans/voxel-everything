#[compute]
#version 460

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
shared uint s_has_subtract;
shared uint s_keep[256];
#define FIELD_OP_INDEX(base, i) ((base) + s_ops[i])
#include "field.glslh"
#include "brick_layout.glslh"

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

const uint CELL_AIR = 1u;
const uint CELL_SOLID = 2u;
const uint CELL_FULL = 3u;

layout(push_constant, std430) uniform Push {
	ivec4 atlas_bricks;
} pc;

shared uint s_mat[BRICK_VOXEL_COUNT]; // global material id per cell, 0 = none
shared uint s_pal[4];                 // palette in insertion order
shared uint s_cnt[4];                 // cells charged to each insertion-order slot
shared uint s_inv[4];                 // insertion-order slot -> final packed index
shared uint s_mip8[512];              // (min << 8) | max
shared uint s_mip4[64];
// The Eikonal clamp's frozen per-iteration lattice snapshot, packed 4 encoded bytes per
// word (4913 bytes -> 1229 words). Every thread reads ONLY this copy, so no thread can
// observe another's same-iteration output -- see the clamp block in main().
shared uint s_clamp_snap[(BRICK_SDF_COUNT + 3) / 4];
shared uint s_needs_clamp;
// Positive float bit patterns preserve numeric ordering, so integer atomics can reduce the
// hardness range sampled across the full 17^3 lattice, including its positive apron.
shared uint s_min_hardness;
shared uint s_max_hardness;

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

// The i-th byte of the clamp's shared snapshot.
uint clamp_snap_byte(uint i) {
	return (s_clamp_snap[i >> 2u] >> ((i & 3u) * 8u)) & 255u;
}

// Mirror of ve::bound in extension/src/world/brick_clamp.cpp: the SIGNED eikonal band
// d_self in [d_nb - VOXEL_SIZE, d_nb + VOXEL_SIZE], floored at the smallest representable
// same-sign magnitude. Works on the ENCODED byte so both sides quantise identically -- the
// differential test compares stored bytes.
uint clamp_bound(uint self, uint neighbour) {
	float d = decode_sdf(float(self) / 255.0);
	float n = decode_sdf(float(neighbour) / 255.0);
	if (d > 0.0) {
		float hi = max(n + VOXEL_SIZE, SDF_RANGE / 255.0);
		return d <= hi ? self : encode_sdf_byte(hi);
	}
	float lo = min(n - VOXEL_SIZE, -(SDF_RANGE / 255.0));
	return d >= lo ? self : encode_sdf_byte(lo);
}

// Mirror of ve::lattice_needs_clamp. Both halves must hold: a carve to distort the field,
// and two samples (including baseline-hardness air) whose hardness differs.
bool needs_clamp() {
	return s_has_subtract != 0u && s_min_hardness != s_max_hardness;
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
		s_has_subtract = 0u;
		for (uint i = 0u; i < op_count; i++)
			if (s_keep[i] != 0u) {
				s_ops[n++] = i;
				if (field_op_pool.v[(op_base + i) * 2u].x == OP_SPHERE_SUBTRACT)
					s_has_subtract = 1u;
			}
		s_op_n = n;
		s_min_hardness = floatBitsToUint(1e30);
		s_max_hardness = 0u;
	}
	barrier();

	ivec3 sdf_base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_SDF_STRIDE);
	ivec3 mat_base = atlas_base(slot, pc.atlas_bricks.xyz, BRICK_VOXELS);
	vec3 bo = vec3(brick) * BRICK_SIZE;

	if (tid < 4u) { s_pal[tid] = 0u; s_cnt[tid] = 0u; s_inv[tid] = tid; }

	// Phase 1a: the 16^3 cell lattice — SDF plus each cell's own material sample.
	// The range reduction is uniform-branch skipped for the overwhelmingly common case
	// with no subtract op; each thread carries one local range across both lattice phases.
	float local_min_hardness = 1e30;
	float local_max_hardness = 0.0;
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u) {
		ivec3 v = cell_coord(i);
		float sdf;
		uint mat;
		eval_field(bo + vec3(v) * VOXEL_SIZE, op_base, s_op_n, sdf, mat);
		imageStore(sdf_atlas, sdf_base + v, vec4(quantise_sdf(sdf)));
		s_mat[i] = mat;
		if (s_has_subtract != 0u) {
			float hardness = mat_hardness(mat);
			local_min_hardness = min(local_min_hardness, hardness);
			local_max_hardness = max(local_max_hardness, hardness);
		}
	}
	memoryBarrierShared();
	barrier();

	// Phase 1b: the apron planes at local 16 (17^3 - 16^3 = 817 samples). Their SDF completes
	// the trilinear cell at the brick's positive faces; their material seeds the cell the
	// shader's clamp folds them into, but only where that cell sampled nothing of its own —
	// a cell's own sample always wins, which is what the compare-and-swap against 0 encodes.
	for (uint i = tid; i < uint(BRICK_SDF_COUNT); i += 256u) {
		ivec3 v = ivec3(int(i) % BRICK_SDF_STRIDE,
				(int(i) / BRICK_SDF_STRIDE) % BRICK_SDF_STRIDE,
				int(i) / (BRICK_SDF_STRIDE * BRICK_SDF_STRIDE));
		if (v.x < BRICK_VOXELS && v.y < BRICK_VOXELS && v.z < BRICK_VOXELS) continue;
		float sdf;
		uint mat;
		eval_field(bo + vec3(v) * VOXEL_SIZE, op_base, s_op_n, sdf, mat);
		imageStore(sdf_atlas, sdf_base + v, vec4(quantise_sdf(sdf)));
		if (s_has_subtract != 0u) {
			float hardness = mat_hardness(mat);
			local_min_hardness = min(local_min_hardness, hardness);
			local_max_hardness = max(local_max_hardness, hardness);
		}
		if (mat == 0u) continue;
		ivec3 c = min(v, ivec3(BRICK_VOXELS - 1));
		atomicCompSwap(s_mat[c.x + c.y * BRICK_VOXELS + c.z * BRICK_VOXELS * BRICK_VOXELS],
				0u, mat);
	}
	if (s_has_subtract != 0u) {
		atomicMin(s_min_hardness, floatBitsToUint(local_min_hardness));
		atomicMax(s_max_hardness, floatBitsToUint(local_max_hardness));
	}
	memoryBarrierImage();
	memoryBarrierShared();
	barrier();

	// The Eikonal clamp (ve::clamp_brick_lattice). Per-point hardness distorts the carve
	// field's magnitude at a material seam; without this the near-field marcher steps
	// through the barely-carved lip. It must land BEFORE Phase 2 -- that phase reads the
	// lattice to project air cells onto the surface, exactly as ve::spread_materials does
	// on the CPU.
	//
	// The relaxation is snapshot-Jacobi: each iteration first freezes the whole lattice
	// into s_clamp_snap, then computes every sample from that frozen copy alone. No thread
	// ever reads another thread's same-iteration output, so the schedule cannot influence
	// the result and the bytes match the CPU's single-threaded loop exactly. That
	// equivalence is what tests/test_brick_diff.gd checks. An in-place Gauss-Seidel sweep
	// would NOT have this property: cross-sign coupling makes the fixed point genuinely
	// order-dependent (Task 7 review), so a racing workgroup lands on an arbitrary mixture
	// of sweep orders that no CPU loop reproduces.
	if (tid == 0u) s_needs_clamp = needs_clamp() ? 1u : 0u;
	memoryBarrierShared();
	barrier();
	if (s_needs_clamp != 0u) {
		for (int iter = 0; iter < CLAMP_ITERATIONS; iter++) {
			// Freeze the current lattice into shared memory, packed 4 bytes per word.
			for (uint w = tid; w < uint((BRICK_SDF_COUNT + 3) / 4); w += 256u) {
				uint p = 0u;
				for (uint k = 0u; k < 4u; k++) {
					uint idx = w * 4u + k;
					if (idx >= uint(BRICK_SDF_COUNT)) continue;
					ivec3 v = ivec3(int(idx) % BRICK_SDF_STRIDE,
							(int(idx) / BRICK_SDF_STRIDE) % BRICK_SDF_STRIDE,
							int(idx) / (BRICK_SDF_STRIDE * BRICK_SDF_STRIDE));
					p |= uint(imageLoad(sdf_atlas, sdf_base + v).r * 255.0 + 0.5) << (k * 8u);
				}
				s_clamp_snap[w] = p;
			}
			memoryBarrierShared();
			barrier();
			// Update every sample from the frozen snapshot only, straight to the image.
			for (uint i = tid; i < uint(BRICK_SDF_COUNT); i += 256u) {
				ivec3 v = ivec3(int(i) % BRICK_SDF_STRIDE,
						(int(i) / BRICK_SDF_STRIDE) % BRICK_SDF_STRIDE,
						int(i) / (BRICK_SDF_STRIDE * BRICK_SDF_STRIDE));
				uint b = clamp_snap_byte(i);
				if (v.x > 0) b = clamp_bound(b, clamp_snap_byte(i - 1u));
				if (v.x + 1 < BRICK_SDF_STRIDE)
					b = clamp_bound(b, clamp_snap_byte(i + 1u));
				if (v.y > 0) b = clamp_bound(b, clamp_snap_byte(i - uint(BRICK_SDF_STRIDE)));
				if (v.y + 1 < BRICK_SDF_STRIDE)
					b = clamp_bound(b, clamp_snap_byte(i + uint(BRICK_SDF_STRIDE)));
				if (v.z > 0)
					b = clamp_bound(b, clamp_snap_byte(i - uint(BRICK_SDF_STRIDE * BRICK_SDF_STRIDE)));
				if (v.z + 1 < BRICK_SDF_STRIDE)
					b = clamp_bound(b, clamp_snap_byte(i + uint(BRICK_SDF_STRIDE * BRICK_SDF_STRIDE)));
				imageStore(sdf_atlas, sdf_base + v, vec4(float(b) / 255.0));
			}
			// The next iteration's snapshot load must see every store of this one.
			memoryBarrierImage();
			barrier();
		}
	}

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
			uint matB;
			eval_field(bo + vec3(v) * VOXEL_SIZE - g / len * t, op_base, s_op_n, sdf2, matB);
			s_mat[i] = matB;
		}
	}
	memoryBarrierShared();
	barrier();

	// Phase 3: build the palette, then order it by occupancy so slot 0 is the dominant
	// material (ve::palette_occupancy_order). Two rounds: insertion is unordered, and the
	// ordering cannot be decided until every cell has been counted.
	for (uint i = tid; i < uint(BRICK_VOXEL_COUNT); i += 256u)
		if (s_mat[i] != 0u) insert_material(s_mat[i]);
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
		uint state = mn > ENCODED_ZERO ? CELL_AIR :
				(mx <= ENCODED_ZERO ? CELL_FULL : CELL_SOLID);
		int rslot = j1.x;
		int bi = (brick.x & (REGION_BRICKS - 1)) +
				(brick.y & (REGION_BRICKS - 1)) * REGION_BRICKS +
				(brick.z & (REGION_BRICKS - 1)) * REGION_BRICKS * REGION_BRICKS;
		int occ_word = rslot * OCC_WORDS_PER_REGION + (bi >> 4);
		uint occ_shift = (uint(bi) & 15u) * 2u;
		atomicAnd(occupancy.w[occ_word], ~(3u << occ_shift));
		atomicOr(occupancy.w[occ_word], (state & 3u) << occ_shift);
	}
}
