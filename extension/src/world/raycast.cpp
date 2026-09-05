#include "world/raycast.h"
#include "world/brick_eval.h"
#include <cmath>
#include <limits>

namespace ve {

namespace {

// The op list that governs a point is the list of the region containing it (spec §2: an op
// is appended to every region it touches, so no neighbour walk is needed).
const std::vector<EditOp> &ops_at(const EditLog &log, float x, float y, float z) {
	return log.ops(ve::region_of_point(x, y, z));
}

Sample sample_at(const Generator &gen, const EditLog &log, float x, float y, float z,
		const VolumeStore *volumes, const OverrideSource *overrides) {
	const std::vector<EditOp> &ops = ops_at(log, x, y, z);
	return eval_field(gen, ops.data(), static_cast<int>(ops.size()), x, y, z, volumes, overrides);
}

float field_at(const Generator &gen, const EditLog &log, float x, float y, float z,
		const VolumeStore *volumes, const OverrideSource *overrides) {
	return sample_at(gen, log, x, y, z, volumes, overrides).sdf;
}

// How far below the surface the material probe is willing to look, and at what pitch. The
// hit tolerance is 0.2 voxels, so the first step already clears it; the band then continues
// to two voxels for fields whose local gradient understates the remaining distance. The
// pitch is deliberately much finer than a voxel: a single voxel-scale offset can step clean
// through a thin shell and report the hollow air behind it.
constexpr float kMaterialProbePitch = 0.25f * kVoxelSize;
constexpr int kMaterialProbeSteps = 8;

// Sign-crossing is only meaningful when the field at this point can actually contain a
// volume op's positive box-distance apron. Both the store pointer and a live slot are
// required: apply_op fail-softs a missing/released slot, so a kOpVolumeAdd whose slot is
// gone leaves the field identical to a pure analytic one and must keep the classic
// hit_eps path.
bool region_has_live_volume_add(const std::vector<EditOp> &ops, const VolumeStore *volumes) {
	if (!volumes) return false;
	for (const EditOp &op : ops) {
		if (op.type == kOpVolumeAdd && volumes->has(static_cast<int>(op.aux[0])))
			return true;
	}
	return false;
}

} // namespace

RayHit raycast(const Generator &gen, const EditLog &log, const float origin[3],
		const float dir[3], float max_dist, const VolumeStore *volumes,
		const OverrideSource *overrides) {
	RayHit out;
	const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len <= 0.0f) return out;
	const float d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
	const float inv_l = 1.0f / gen.lipschitz();
	const float min_step = 0.5f * kVoxelSize;
	const float hit_eps = 0.2f * kVoxelSize;

	float t = 0.0f;
	// A volume op unions sample_volume_lattice's outside-the-lattice box distance into
	// the field: a small POSITIVE value in empty air around the lattice's AABB. When the
	// current region actually contains a volume op AND a store is present to sample it,
	// require an actual sign crossing so that apron never reads as a surface. Otherwise
	// keep the classic hit_eps tolerance, even when a caller passes a non-null (but
	// irrelevant or empty) VolumeStore over a pure analytic field.
	// +inf makes a ray that starts inside solid hit at its origin.
	float prev_f = std::numeric_limits<float>::infinity();
	for (int i = 0; i < 4096 && t <= max_dist; i++) {
		const float p[3] = {origin[0] + d[0] * t, origin[1] + d[1] * t, origin[2] + d[2] * t};
		const std::vector<EditOp> &ops = ops_at(log, p[0], p[1], p[2]);
		const Sample s = eval_field(gen, ops.data(), static_cast<int>(ops.size()), p[0], p[1],
				p[2], volumes, overrides);
		const float f = s.sdf;
		const bool sign_crossing = region_has_live_volume_add(ops, volumes);
		const bool hit = sign_crossing ? (prev_f > 0.0f && f <= 0.0f) : (f < hit_eps);
		if (hit) {
			out.hit = true;
			out.distance = t;
			out.pos[0] = p[0]; out.pos[1] = p[1]; out.pos[2] = p[2];
			// Central-difference gradient over one voxel; the field is smooth at this scale.
			const float e = kVoxelSize;
			const float gx = field_at(gen, log, p[0] + e, p[1], p[2], volumes, overrides) -
					field_at(gen, log, p[0] - e, p[1], p[2], volumes, overrides);
			const float gy = field_at(gen, log, p[0], p[1] + e, p[2], volumes, overrides) -
					field_at(gen, log, p[0], p[1] - e, p[2], volumes, overrides);
			const float gz = field_at(gen, log, p[0], p[1], p[2] + e, volumes, overrides) -
					field_at(gen, log, p[0], p[1], p[2] - e, volumes, overrides);
			const float gl = std::sqrt(gx * gx + gy * gy + gz * gz);
			if (gl > 0.0f) {
				out.normal[0] = gx / gl; out.normal[1] = gy / gl; out.normal[2] = gz / gl;
			} else {
				out.normal[1] = 1.0f;
			}
			out.material = s.material;
			// The hit tolerance can stop the march just OUTSIDE the surface, where the field
			// correctly reports air and carries no material. Walk a short way back along the
			// normal in fine steps and take the first solid material found.
			for (int k = 1; k <= kMaterialProbeSteps && out.material == 0; k++) {
				const float back = static_cast<float>(k) * kMaterialProbePitch;
				out.material = sample_at(gen, log, p[0] - out.normal[0] * back,
						p[1] - out.normal[1] * back, p[2] - out.normal[2] * back,
						volumes, overrides).material;
			}
			return out;
		}
		t += std::max(f * inv_l, min_step);
		prev_f = f;
	}
	return out;
}

} // namespace ve
