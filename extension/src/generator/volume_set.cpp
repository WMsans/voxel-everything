#include "generator/volume_set.h"
#include <algorithm>
#include <cmath>

namespace ve {

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

void VolumeSet::release(int slot) {
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return;
	if (slots_[slot].pinned) return; // an op still names it; see pin()
	slots_[slot].used = false;
	// Assigning a fresh VolumeData destroys the old vectors and frees their
	// buffers immediately; release() exists specifically to shed those bytes.
	slots_[slot].data = VolumeData{};
	slots_[slot].version = next_version_++;
	live_--;
}

bool VolumeSet::store(int slot, VolumeData data) {
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return false;
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
	if (slot < 0 || slot >= kMaxVolumes || !slots_[slot].used) return false;
	slots_[slot].pinned = true;
	return true;
}

bool VolumeSet::pinned(int slot) const {
	return slot >= 0 && slot < kMaxVolumes && slots_[slot].pinned;
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
	if (!src.valid() || src_op.radius <= 0.0f || dim < 2 || !out || !out_op) return false;

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
	float extent = 0.0f;
	for (int a = 0; a < 3; a++) extent = std::max(extent, whi[a] - wlo[a]);

	float pitch = kIslandVoxelFine;
	if (extent > static_cast<float>(dim - 1) * kIslandVoxelFine) pitch = kIslandVoxelCoarse;
	if (extent > static_cast<float>(dim - 1) * pitch) return false;

	// Centre the new lattice on the rotated AABB so the margin is shared on both sides,
	// which is what keeps the outermost shell positive (see sample_volume_lattice).
	float o[3];
	for (int a = 0; a < 3; a++) {
		const float slack = static_cast<float>(dim - 1) * pitch - (whi[a] - wlo[a]);
		o[a] = wlo[a] - 0.5f * slack;
	}

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

} // namespace ve
