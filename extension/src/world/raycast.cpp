#include "world/raycast.h"
#include "world/brick_eval.h"
#include <cmath>
#include <limits>

namespace ve {

namespace {

// The op list that governs a point is the list of the region containing it (spec §2: an op
// is appended to every region it touches, so no neighbour walk is needed).
const std::vector<EditOp> &ops_at(const EditLog &log, float x, float y, float z) {
	return log.ops(WorldBounds::region_of_point(x, y, z));
}

float field_at(const Generator &gen, const EditLog &log, float x, float y, float z,
		const VolumeStore *volumes, const OverrideSource *overrides) {
	const std::vector<EditOp> &ops = ops_at(log, x, y, z);
	return eval_field(gen, ops.data(), static_cast<int>(ops.size()), x, y, z, volumes, overrides).sdf;
}

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
		const Sample sample = eval_field(gen, ops.data(), static_cast<int>(ops.size()), p[0],
				p[1], p[2], volumes, overrides);
		const float f = sample.sdf;
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
			out.material = sample.material;
			// The hit tolerance can stop just outside the surface, where the field correctly
			// reports air. Project to the zero crossing using the measured local gradient,
			// then move only 0.1 mm inside. A fixed voxel-scale offset can jump across a thin
			// shell and report the hollow air behind it.
			const auto material_behind = [&](float offset) {
				float q[3];
				for (int a = 0; a < 3; a++) {
					q[a] = p[a] - out.normal[a] * offset;
					if (out.normal[a] != 0.0f && q[a] == p[a])
						q[a] = std::nextafter(p[a], out.normal[a] > 0.0f
								? -std::numeric_limits<float>::infinity()
								: std::numeric_limits<float>::infinity());
				}
				const std::vector<EditOp> &qops = ops_at(log, q[0], q[1], q[2]);
				return eval_field(gen, qops.data(), static_cast<int>(qops.size()), q[0], q[1],
						q[2], volumes, overrides).material;
			};
			if (out.material == 0) {
				// The normal's one-voxel stencil can span both sides of a thin shell and
				// underestimate its slope. Measure the directional derivative on a much finer
				// stencil so f / slope lands at the near zero crossing instead of beyond it.
				constexpr float kProjectionEpsilon = 0.0001f;
				float plus_p[3], minus_p[3];
				for (int a = 0; a < 3; a++) {
					plus_p[a] = p[a] + out.normal[a] * kProjectionEpsilon;
					minus_p[a] = p[a] - out.normal[a] * kProjectionEpsilon;
					if (out.normal[a] > 0.0f) {
						if (plus_p[a] == p[a]) plus_p[a] = std::nextafter(p[a],
								std::numeric_limits<float>::infinity());
						if (minus_p[a] == p[a]) minus_p[a] = std::nextafter(p[a],
								-std::numeric_limits<float>::infinity());
					} else if (out.normal[a] < 0.0f) {
						if (plus_p[a] == p[a]) plus_p[a] = std::nextafter(p[a],
								-std::numeric_limits<float>::infinity());
						if (minus_p[a] == p[a]) minus_p[a] = std::nextafter(p[a],
								std::numeric_limits<float>::infinity());
					}
				}
				const float plus = field_at(gen, log, plus_p[0], plus_p[1], plus_p[2],
						volumes, overrides);
				const float minus = field_at(gen, log, minus_p[0], minus_p[1], minus_p[2],
						volumes, overrides);
				const float represented_span = std::fabs(
						(plus_p[0] - minus_p[0]) * out.normal[0] +
						(plus_p[1] - minus_p[1]) * out.normal[1] +
						(plus_p[2] - minus_p[2]) * out.normal[2]);
				const float directional_slope = represented_span > 0.0f
						? std::fabs(plus - minus) / represented_span : 0.0f;
				const float to_surface = f > 0.0f && directional_slope > 1e-6f
						? f / directional_slope : 0.0f;
				const float inside = std::max(kProjectionEpsilon, 0.5f * represented_span);
				out.material = material_behind(to_surface + inside);
			}
			// Fallback for non-ideal fields whose local linear projection does not enter solid.
			for (float over = 0.5f; out.material == 0 && over <= 2.5f; over += 1.0f)
				out.material = material_behind(over * kVoxelSize);
			return out;
		}
		t += std::max(f * inv_l, min_step);
		prev_f = f;
	}
	return out;
}

} // namespace ve
