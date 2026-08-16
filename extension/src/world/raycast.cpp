#include "world/raycast.h"
#include "world/brick_eval.h"
#include <cmath>

namespace ve {

namespace {

// The op list that governs a point is the list of the region containing it (spec §2: an op
// is appended to every region it touches, so no neighbour walk is needed).
const std::vector<EditOp> &ops_at(const EditLog &log, float x, float y, float z) {
	return log.ops(WorldBounds::region_of_point(x, y, z));
}

float field_at(const Generator &gen, const EditLog &log, float x, float y, float z,
		const VolumeStore *volumes) {
	const std::vector<EditOp> &ops = ops_at(log, x, y, z);
	return eval_field(gen, ops.data(), static_cast<int>(ops.size()), x, y, z, volumes).sdf;
}

} // namespace

RayHit raycast(const Generator &gen, const EditLog &log, const float origin[3],
		const float dir[3], float max_dist, const VolumeStore *volumes) {
	RayHit out;
	const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	if (len <= 0.0f) return out;
	const float d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};
	const float inv_l = 1.0f / gen.lipschitz();
	const float min_step = 0.5f * kVoxelSize;
	const float hit_eps = 0.2f * kVoxelSize;

	float t = 0.0f;
	for (int i = 0; i < 4096 && t <= max_dist; i++) {
		const float p[3] = {origin[0] + d[0] * t, origin[1] + d[1] * t, origin[2] + d[2] * t};
		const float f = field_at(gen, log, p[0], p[1], p[2], volumes);
		if (f < hit_eps) {
			out.hit = true;
			out.distance = t;
			out.pos[0] = p[0]; out.pos[1] = p[1]; out.pos[2] = p[2];
			// Central-difference gradient over one voxel; the field is smooth at this scale.
			const float e = kVoxelSize;
			const float gx = field_at(gen, log, p[0] + e, p[1], p[2], volumes) -
					field_at(gen, log, p[0] - e, p[1], p[2], volumes);
			const float gy = field_at(gen, log, p[0], p[1] + e, p[2], volumes) -
					field_at(gen, log, p[0], p[1] - e, p[2], volumes);
			const float gz = field_at(gen, log, p[0], p[1], p[2] + e, volumes) -
					field_at(gen, log, p[0], p[1], p[2] - e, volumes);
			const float gl = std::sqrt(gx * gx + gy * gy + gz * gz);
			if (gl > 0.0f) {
				out.normal[0] = gx / gl; out.normal[1] = gy / gl; out.normal[2] = gz / gl;
			} else {
				out.normal[1] = 1.0f;
			}
			return out;
		}
		t += std::max(f * inv_l, min_step);
	}
	return out;
}

} // namespace ve
