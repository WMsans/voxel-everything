#include "generator/volume_set.h"
#include "connectivity/components.h"
#include "world/brick_eval.h"
#include <algorithm>
#include <cmath>

namespace ve {

// The labeller splits on extent alone, so its bound must be one the lattice can always hold.
static_assert(static_cast<double>(kMaxIslandExtentCells) * kOccupancyCellSize <=
				static_cast<double>(kIslandDim - 1 - 2 * kIslandMarginVoxels) *
						kIslandVoxelCoarse,
		"kMaxIslandExtentCells is larger than an island volume can cover");

namespace {

// `basis` is row-major and must be orthonormal for resample_volume's
// inverse-by-transpose mapping to be exact. Checking row norms, column norms,
// and column-pair dots is exactly AᵀA ≈ I, so a rank-deficient matrix with
// unit rows and mutually orthogonal columns (e.g. one zero column) is
// rejected rather than being treated as invertible.
bool is_orthonormal_basis(const float basis[9]) {
	constexpr float kTol = 1e-4f;
	for (int r = 0; r < 3; r++) {
		const float *row = basis + 3 * r;
		const float len2 = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
		if (std::fabs(len2 - 1.0f) > kTol) return false;
	}
	for (int c = 0; c < 3; c++) {
		const float len2 = basis[0 * 3 + c] * basis[0 * 3 + c] +
				basis[1 * 3 + c] * basis[1 * 3 + c] +
				basis[2 * 3 + c] * basis[2 * 3 + c];
		if (std::fabs(len2 - 1.0f) > kTol) return false;
	}
	for (int a = 0; a < 3; a++) {
		for (int b = a + 1; b < 3; b++) {
			const float dot = basis[0 * 3 + a] * basis[0 * 3 + b] +
					basis[1 * 3 + a] * basis[1 * 3 + b] +
					basis[2 * 3 + a] * basis[2 * 3 + b];
			if (std::fabs(dot) > kTol) return false;
		}
	}
	return true;
}

} // namespace

bool sample_volume_lattice(const uint8_t *sdf, const uint8_t *mat, int dim,
		const float origin[3], float voxel, float x, float y, float z, VolumeSample *out) {
	if (!sdf || !mat || !out || dim < 2 || voxel <= 0.0f) return false;
	const float span = static_cast<float>(dim - 1) * voxel;
	float lo[3] = {origin[0], origin[1], origin[2]};
	float hi[3] = {origin[0] + span, origin[1] + span, origin[2] + span};
	const float p[3] = {x, y, z};
	const float outside = box_sdf(lo, hi, x, y, z);
	if (outside > 0.0f) {
		out->sdf = outside;
		out->material = 0;
		return true;
	}

	float l[3];
	for (int a = 0; a < 3; a++)
		l[a] = std::min(std::max((p[a] - lo[a]) / voxel, 0.0f),
				static_cast<float>(dim - 1));
	const int i0[3] = {static_cast<int>(l[0]), static_cast<int>(l[1]), static_cast<int>(l[2])};
	const int i1[3] = {std::min(i0[0] + 1, dim - 1), std::min(i0[1] + 1, dim - 1),
			std::min(i0[2] + 1, dim - 1)};
	const float f[3] = {l[0] - i0[0], l[1] - i0[1], l[2] - i0[2]};
	const auto at = [&](int ax, int ay, int az) {
		return decode_sdf(sdf[VolumeSet::voxel_index(dim, ax, ay, az)]);
	};
	const float c00 = at(i0[0], i0[1], i0[2]) * (1 - f[0]) + at(i1[0], i0[1], i0[2]) * f[0];
	const float c10 = at(i0[0], i1[1], i0[2]) * (1 - f[0]) + at(i1[0], i1[1], i0[2]) * f[0];
	const float c01 = at(i0[0], i0[1], i1[2]) * (1 - f[0]) + at(i1[0], i0[1], i1[2]) * f[0];
	const float c11 = at(i0[0], i1[1], i1[2]) * (1 - f[0]) + at(i1[0], i1[1], i1[2]) * f[0];
	const float c0 = c00 * (1 - f[1]) + c10 * f[1];
	const float c1 = c01 * (1 - f[1]) + c11 * f[1];
	out->sdf = c0 * (1 - f[2]) + c1 * f[2];

	// Nearest, not interpolated: a material id is a label, and blending two labels is
	// meaningless. Same rule the brick atlas uses (mat_atlas is NEAREST filtered).
	const int m[3] = {static_cast<int>(l[0] + 0.5f), static_cast<int>(l[1] + 0.5f),
			static_cast<int>(l[2] + 0.5f)};
	out->material = mat[VolumeSet::voxel_index(dim, std::min(m[0], dim - 1),
			std::min(m[1], dim - 1), std::min(m[2], dim - 1))];
	return true;
}

int VolumeSet::allocate() {
	for (int i = 0; i < kMaxVolumes; i++) {
		if (slots_[i].used) continue;
		slots_[i].used = true;
		slots_[i].data = VolumeData{};
		slots_[i].version = next_version_++;
		live_++;
		return i;
	}
	return -1;
}

bool VolumeSet::reserve(int slot) {
	if (slot < 0 || slot >= kMaxVolumes || slots_[slot].used) return false;
	slots_[slot].used = true;
	slots_[slot].data = VolumeData{};
	slots_[slot].version = next_version_++;
	live_++;
	return true;
}

bool VolumeSet::release(int slot) {
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return false;
	if (slots_[slot].pinned) return false; // an op may only name a pinned slot; see pin()
	slots_[slot].used = false;
	// Assigning a fresh VolumeData destroys the old vectors and frees their
	// buffers immediately; release() exists specifically to shed those bytes.
	slots_[slot].data = VolumeData{};
	slots_[slot].version = next_version_++;
	live_--;
	return true;
}

bool VolumeSet::store(int slot, VolumeData data) {
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return false;
	// A pinned slot always holds stored bytes (pin() refuses an empty slot) that are
	// named by a live EditOp; the GPU mirror reads those bytes with no liveness flag,
	// so they must never come back as a different volume.
	if (slots_[slot].pinned) return false;
	if (!data.valid()) return false;
	slots_[slot].data = std::move(data);
	slots_[slot].version = next_version_++;
	return true;
}

const VolumeData *VolumeSet::get(int slot) const {
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return nullptr;
	return slots_[slot].data.empty() ? nullptr : &slots_[slot].data;
}

int64_t VolumeSet::version(int slot) const {
	return slot >= 0 && slot < kMaxVolumes ? slots_[slot].version : 0;
}

bool VolumeSet::pin(int slot) {
	// Pinning requires a stored, non-empty volume: an allocated-but-empty slot (or a
	// reserved-but-empty one) has no bytes for the GPU mirror to read, so an op that
	// names it has nothing pinned and the invariant would not hold.
	if (slot < 0 || slot >= kMaxVolumes || get(slot) == nullptr) return false;
	slots_[slot].pinned = true;
	return true;
}

bool VolumeSet::pinned(int slot) const {
	return slot >= 0 && slot < kMaxVolumes && slots_[slot].pinned;
}

bool VolumeSet::has(int slot) const {
	return get(slot) != nullptr;
}

bool VolumeSet::sample(int slot, float x, float y, float z, const EditOp &op,
		VolumeSample *out) const {
	const VolumeData *v = get(slot);
	if (!v) return false;
	return sample_volume_lattice(v->sdf.data(), v->mat.data(), v->dim, op.pos, op.radius,
			x, y, z, out);
}

bool resample_volume(const VolumeData &src, const EditOp &src_op, const float basis[9],
		const float origin[3], int slot, int dim, VolumeData *out, EditOp *out_op) {
	// The inverse-by-transpose below only holds for an orthonormal basis, and the pitch
	// is only meaningful for an op that actually names a volume lattice. The target
	// slot must be a real pool index even though resample_volume itself does not
	// touch the pool: make_volume_add bakes `slot` into out_op for the caller to store.
	if (slot < 0 || slot >= kMaxVolumes || !src.valid() || src_op.type != kOpVolumeAdd ||
			src_op.radius <= 0.0f || !basis || !origin || !is_orthonormal_basis(basis) ||
			dim < 2 || !out || !out_op)
		return false;

	// World AABB of the rotated source box: transform its eight corners.
	const float span = static_cast<float>(src.dim - 1) * src_op.radius;
	float wlo[3] = {1e30f, 1e30f, 1e30f}, whi[3] = {-1e30f, -1e30f, -1e30f};
	for (int c = 0; c < 8; c++) {
		const float q[3] = {src_op.pos[0] + ((c & 1) ? span : 0.0f),
				src_op.pos[1] + ((c & 2) ? span : 0.0f),
				src_op.pos[2] + ((c & 4) ? span : 0.0f)};
		for (int a = 0; a < 3; a++) {
			const float w = basis[a * 3 + 0] * q[0] + basis[a * 3 + 1] * q[1] +
					basis[a * 3 + 2] * q[2] + origin[a];
			wlo[a] = std::min(wlo[a], w);
			whi[a] = std::max(whi[a], w);
		}
	}
	float pitch = 0.0f;
	float o[3] = {0, 0, 0};
	if (!plan_island_lattice(wlo, whi, dim, &pitch, o)) return false;

	out->dim = dim;
	out->sdf.assign(static_cast<size_t>(dim) * dim * dim, encode_sdf(kSdfRange));
	out->mat.assign(static_cast<size_t>(dim) * dim * dim, 0);
	out->solid_voxels = 0;
	for (int z = 0; z < dim; z++)
		for (int y = 0; y < dim; y++)
			for (int x = 0; x < dim; x++) {
				const float p[3] = {o[0] + x * pitch, o[1] + y * pitch, o[2] + z * pitch};
				// world -> local: basis is orthonormal, so its inverse is its transpose.
				const float d[3] = {p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
				float q[3];
				for (int a = 0; a < 3; a++)
					q[a] = basis[0 * 3 + a] * d[0] + basis[1 * 3 + a] * d[1] +
							basis[2 * 3 + a] * d[2];
				VolumeSample s{};
				if (!sample_volume_lattice(src.sdf.data(), src.mat.data(), src.dim,
							src_op.pos, src_op.radius, q[0], q[1], q[2], &s))
					continue;
				const int i = VolumeSet::voxel_index(dim, x, y, z);
				out->sdf[i] = encode_sdf(s.sdf);
				out->mat[i] = static_cast<uint8_t>(s.material);
				if (s.sdf <= 0.0f) out->solid_voxels++;
			}

	*out_op = make_volume_add(slot, o, pitch, dim);
	return true;
}

bool plan_island_lattice(const float lo[3], const float hi[3], int dim, float *voxel,
		float origin[3]) {
	if (dim < 2 + 2 * kIslandMarginVoxels) return false;
	float extent = 0.0f;
	for (int a = 0; a < 3; a++) extent = std::max(extent, hi[a] - lo[a]);
	const float usable = static_cast<float>(dim - 1 - 2 * kIslandMarginVoxels);
	float pitch = kIslandVoxelFine;
	if (extent > usable * pitch) pitch = kIslandVoxelCoarse;
	if (extent > usable * pitch) return false;
	const float span = static_cast<float>(dim - 1) * pitch;
	for (int a = 0; a < 3; a++) origin[a] = lo[a] - 0.5f * (span - (hi[a] - lo[a]));
	*voxel = pitch;
	return true;
}

void extract_island_volume(const Generator &gen, const EditOp *ops, int op_count,
		const VolumeStore *volumes, const float origin[3], float voxel, int dim,
		const float *box_aabbs, int box_count, VolumeData *out) {
	out->dim = dim;
	out->sdf.assign(static_cast<size_t>(dim) * dim * dim, 0);
	out->mat.assign(static_cast<size_t>(dim) * dim * dim, 0);
	out->solid_voxels = 0;

	for (int z = 0; z < dim; z++)
		for (int y = 0; y < dim; y++)
			for (int x = 0; x < dim; x++) {
				const float p[3] = {origin[0] + x * voxel, origin[1] + y * voxel,
						origin[2] + z * voxel};
				Sample s = eval_field(gen, ops, op_count, p[0], p[1], p[2], volumes);
				// The island IS the solid field intersected with the union of its cells, so
				// the mask is a CSG intersection: max(field, min over boxes).
				float bu = 1e30f;
				for (int b = 0; b < box_count; b++)
					bu = std::min(bu, box_sdf(&box_aabbs[static_cast<size_t>(b) * 6 + 0],
									 &box_aabbs[static_cast<size_t>(b) * 6 + 3],
									 p[0], p[1], p[2]));
				s.sdf = std::max(s.sdf, bu);
				if (s.sdf > 0.0f) s.material = 0;
				const int i = VolumeSet::voxel_index(dim, x, y, z);
				out->sdf[i] = encode_sdf(s.sdf);
				out->mat[i] = static_cast<uint8_t>(s.material > 255 ? 255 : s.material);
				if (s.sdf <= 0.0f) out->solid_voxels++;
			}
}

void build_volume_mip(const VolumeData &v, std::vector<uint8_t> *out) {
	const int dim = v.dim;
	const int cells = dim / kVolumeMipStride;
	out->assign(static_cast<size_t>(cells) * cells * cells * 2, 0);
	for (int cz = 0; cz < cells; cz++)
		for (int cy = 0; cy < cells; cy++)
			for (int cx = 0; cx < cells; cx++) {
				uint8_t mn = 255, mx = 0;
				// INCLUSIVE corner range: the trilinear interpolant inside a cell is a
				// multilinear combination of its corner samples and therefore never leaves
				// their convex hull, so an inclusive bound is sound and an exclusive one is
				// not (the argument in world/brick_mip.h, applied to a 64^3 lattice).
				for (int z = 0; z <= kVolumeMipStride; z++)
					for (int y = 0; y <= kVolumeMipStride; y++)
						for (int x = 0; x <= kVolumeMipStride; x++) {
							const int sx = std::min(cx * kVolumeMipStride + x, dim - 1);
							const int sy = std::min(cy * kVolumeMipStride + y, dim - 1);
							const int sz = std::min(cz * kVolumeMipStride + z, dim - 1);
							const uint8_t s = v.sdf[VolumeSet::voxel_index(dim, sx, sy, sz)];
							mn = std::min(mn, s);
							mx = std::max(mx, s);
						}
				const int ci = (cx + cy * cells + cz * cells * cells) * 2;
				(*out)[static_cast<size_t>(ci) + 0] = mn;
				(*out)[static_cast<size_t>(ci) + 1] = mx;
			}
}

} // namespace ve
