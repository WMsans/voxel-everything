#include "generator/edit_ops.h"
#include "connectivity/occupancy.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

float sphere_sdf(const EditOp &op, float x, float y, float z) {
	const float dx = x - op.pos[0], dy = y - op.pos[1], dz = z - op.pos[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz) - op.radius;
}

// Inclusive [lo, hi] cell range of the op's padded AABB on a lattice of the given pitch.
// The caller selects the consumer's conservative pad: brick residency uses the activation
// margin plus its one-voxel apron, while stored-lattice consumers use the representable SDF
// range plus one 5 cm pitch.
void padded_range(const EditOp &op, float pitch, float pad, IVec3 *lo, IVec3 *hi) {
	float a[3], b[3];
	op_world_aabb(op, a, b);
	const auto cell = [pitch](float v) { return static_cast<int>(std::floor(v / pitch)); };
	*lo = {cell(a[0] - pad), cell(a[1] - pad), cell(a[2] - pad)};
	*hi = {cell(b[0] + pad), cell(b[1] + pad), cell(b[2] + pad)};
}

} // namespace

uint32_t pack_extent3(int nx, int ny, int nz) {
	const auto c = [](int v) { return static_cast<uint32_t>(v < 1 ? 1 : (v > 1023 ? 1023 : v)); };
	return c(nx) | (c(ny) << 10) | (c(nz) << 20);
}

void unpack_extent3(uint32_t v, int *nx, int *ny, int *nz) {
	*nx = static_cast<int>(v & 0x3FFu);
	*ny = static_cast<int>((v >> 10) & 0x3FFu);
	*nz = static_cast<int>((v >> 20) & 0x3FFu);
}

EditOp make_box_subtract(IVec3 lo_cell, IVec3 hi_cell, float margin) {
	// The cell range is inclusive; an inverted range used to collapse into a silent
	// one-cell box via pack_extent3's clamp. Normalise instead so the op still names the
	// same set of cells whichever order the caller passed them in.
	if (lo_cell.x > hi_cell.x) std::swap(lo_cell.x, hi_cell.x);
	if (lo_cell.y > hi_cell.y) std::swap(lo_cell.y, hi_cell.y);
	if (lo_cell.z > hi_cell.z) std::swap(lo_cell.z, hi_cell.z);
	EditOp op{};
	op.type = kOpBoxSubtract;
	op.pos[0] = static_cast<float>(lo_cell.x) * kOccupancyCellSize;
	op.pos[1] = static_cast<float>(lo_cell.y) * kOccupancyCellSize;
	op.pos[2] = static_cast<float>(lo_cell.z) * kOccupancyCellSize;
	op.aux[0] = pack_extent3(hi_cell.x - lo_cell.x + 1, hi_cell.y - lo_cell.y + 1,
			hi_cell.z - lo_cell.z + 1);
	// The clearance margin rides in the otherwise-unused radius word and is applied only
	// by the field evaluators (apply_op here, shaders/field.glslh's OP_BOX_SUBTRACT). It
	// must stay out of op_world_aabb: see the header comment.
	op.radius = margin > 0.0f ? margin : 0.0f;
	return op;
}

EditOp make_volume_add(int slot, const float origin[3], float voxel, int dim) {
	EditOp op{};
	op.type = kOpVolumeAdd;
	op.pos[0] = origin[0];
	op.pos[1] = origin[1];
	op.pos[2] = origin[2];
	// The sampler rejects a non-positive pitch; clamp it to the engine's voxel size so a
	// malformed op can never make op_world_aabb produce an inverted or degenerate box.
	op.radius = !(voxel > 0.0f) ? kVoxelSize : voxel;
	op.aux[0] = static_cast<uint32_t>(slot); // stored unchanged: a negative slot must
	// stay out of range so VolumeSet::sample fail-softs instead of aliasing slot 0
	op.aux[1] = static_cast<uint32_t>(dim < 1 ? 1 : dim);
	return op;
}

void op_world_aabb(const EditOp &op, float lo[3], float hi[3]) {
	switch (op.type) {
		case kOpBoxSubtract: {
			int n[3] = {1, 1, 1};
			unpack_extent3(op.aux[0], &n[0], &n[1], &n[2]);
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a];
				hi[a] = op.pos[a] + static_cast<float>(n[a]) * kOccupancyCellSize;
			}
			return;
		}
		case kOpVolumeAdd: {
			const float span = static_cast<float>(static_cast<int>(op.aux[1]) - 1) * op.radius;
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a];
				hi[a] = op.pos[a] + span;
			}
			return;
		}
		default:
			for (int a = 0; a < 3; a++) {
				lo[a] = op.pos[a] - op.radius;
				hi[a] = op.pos[a] + op.radius;
			}
			return;
	}
}

float box_sdf(const float lo[3], const float hi[3], float x, float y, float z) {
	const float p[3] = {x, y, z};
	float q[3];
	for (int a = 0; a < 3; a++) {
		const float c = 0.5f * (lo[a] + hi[a]);
		const float h = 0.5f * (hi[a] - lo[a]);
		q[a] = std::fabs(p[a] - c) - h;
	}
	const float outside = std::sqrt(std::max(q[0], 0.0f) * std::max(q[0], 0.0f) +
			std::max(q[1], 0.0f) * std::max(q[1], 0.0f) +
			std::max(q[2], 0.0f) * std::max(q[2], 0.0f));
	const float inside = std::min(std::max(q[0], std::max(q[1], q[2])), 0.0f);
	return outside + inside;
}

void box_sdf_gradient(const float lo[3], const float hi[3], float x, float y, float z, float out[3]) {
	const float p[3] = {x, y, z};
	float q[3];
	float sign[3];
	for (int a = 0; a < 3; a++) {
		const float c = 0.5f * (lo[a] + hi[a]);
		const float h = 0.5f * (hi[a] - lo[a]);
		const float d = p[a] - c;
		sign[a] = d >= 0 ? 1.0f : -1.0f;
		q[a] = std::fabs(d) - h;
	}
	const bool outside = q[0] > 0 || q[1] > 0 || q[2] > 0;
	if (outside) {
		float v[3] = {0, 0, 0};
		for (int a = 0; a < 3; a++) {
			if (q[a] > 0) v[a] = q[a] * sign[a];
		}
		float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
		if (len < 1e-6f) {
			out[0]=0; out[1]=1; out[2]=0;
		} else {
			out[0]=v[0]/len; out[1]=v[1]/len; out[2]=v[2]/len;
		}
	} else {
		// inside: largest signed extent with X/Y/Z priority
		int best = 0;
		float best_q = q[0];
		for (int a = 1; a < 3; a++) {
			if (q[a] > best_q) { best_q = q[a]; best = a; }
		}
		out[0]=0; out[1]=0; out[2]=0;
		out[best] = sign[best];
	}
}

Sample apply_op(Sample s, const EditOp &op, float x, float y, float z,
		const VolumeStore *volumes) {
	switch (op.type) {
		case kOpSphereSubtract: {
			// CSG subtract: max(s, -sphere). A point that becomes air carries no material,
			// matching the generator's own convention (Sample::material == 0 above ground).
			// op.radius is already the EFFECTIVE radius: ve::removal_radius resolved the
			// struck material's hardness once, when the op was built. Scaling per sample
			// here instead kept the sign right but broke the magnitude as a distance bound
			// at a material seam, and the near-field marcher stepped through the lip.
			// Mirrored in shaders/field.glslh.
			const float sp = sphere_sdf(op, x, y, z);
			if (-sp > s.sdf) {
				s.sdf = -sp;
				if (s.sdf > 0.0f) s.material = 0;
			}
			return s;
		}
		case kOpSphereAdd: {
			// CSG union: min(s, sphere). The material changes only where the sphere is the
			// winning term and the result is solid — filling air, not recolouring rock.
			const float sp = sphere_sdf(op, x, y, z);
			if (sp < s.sdf) {
				s.sdf = sp;
				if (s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			}
			return s;
		}
		case kOpSpherePaint: {
			const float sp = sphere_sdf(op, x, y, z);
			if (sp <= 0.0f && s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			return s;
		}
		case kOpBoxSubtract: {
			float lo[3], hi[3];
			op_world_aabb(op, lo, hi);
			// The clearance margin expands the carved box inside the evaluator only: without
			// it the cell-aligned faces of an island carve read as the exact SDF == 0, which
			// cell-aligned samplers (mesh lattice, brick lattice, occupancy probe) turn into
			// phantom walls inside the carved region. Mirrored in shaders/field.glslh.
			const float m = op.radius > 0.0f ? op.radius : 0.0f;
			for (int a = 0; a < 3; a++) {
				lo[a] -= m;
				hi[a] += m;
			}
			const float bd = box_sdf(lo, hi, x, y, z);
			if (-bd > s.sdf) {
				s.sdf = -bd;
				if (s.sdf > 0.0f) s.material = 0;
			}
			return s;
		}
		case kOpVolumeAdd: {
			VolumeSample vs{};
			// Fail-soft (spec §8): an op whose volume is gone contributes nothing at all,
			// rather than stamping undefined bytes into the terrain.
			if (!volumes || !volumes->sample(static_cast<int>(op.aux[0]), x, y, z, op, &vs))
				return s;
			if (vs.sdf < s.sdf) {
				s.sdf = vs.sdf;
				if (s.sdf <= 0.0f && vs.material != 0) s.material = vs.material;
			}
			return s;
		}
		default:
			return s;
	}
}

Sample apply_ops(Sample s, const EditOp *ops, int count, float x, float y, float z,
		const VolumeStore *volumes) {
	for (int i = 0; i < count; i++) s = apply_op(s, ops[i], x, y, z, volumes);
	return s;
}

static void sphere_gradient(const EditOp &op, float x, float y, float z, float out[3], bool &exact) {
	float dx = x - op.pos[0], dy = y - op.pos[1], dz = z - op.pos[2];
	float len = std::sqrt(dx*dx + dy*dy + dz*dz);
	if (len < 1e-6f) {
		out[0]=0; out[1]=1; out[2]=0;
		exact=false;
	} else {
		out[0]=dx/len; out[1]=dy/len; out[2]=dz/len;
		exact=true;
	}
}

FieldSample apply_op_gradient(FieldSample s, const EditOp &op, float x, float y, float z,
		const VolumeStore *volumes) {
	switch (op.type) {
		case kOpSphereSubtract: {
			// One ordinary sphere, exactly as apply_op's subtract above: the effective radius
			// is baked into the op. The gradient is the normalised direction from the centre
			// and never depended on the radius.
			const float sp = sphere_sdf(op, x, y, z);
			if (-sp > s.sdf) {
				float g[3]; bool exact;
				sphere_gradient(op, x, y, z, g, exact);
				s.sdf = -sp;
				if (s.sdf > 0.0f) s.material = 0;
				s.gradient[0] = -g[0]; s.gradient[1] = -g[1]; s.gradient[2] = -g[2];
				s.exact_gradient = exact;
				if (!exact) { s.gradient[0]=0; s.gradient[1]=1; s.gradient[2]=0; }
			}
			return s;
		}
		case kOpSphereAdd: {
			const float sp = sphere_sdf(op, x, y, z);
			if (sp < s.sdf) {
				float g[3]; bool exact;
				sphere_gradient(op, x, y, z, g, exact);
				s.sdf = sp;
				if (s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
				s.gradient[0]=g[0]; s.gradient[1]=g[1]; s.gradient[2]=g[2];
				s.exact_gradient = exact;
				if (!exact) { s.gradient[0]=0; s.gradient[1]=1; s.gradient[2]=0; }
			}
			return s;
		}
		case kOpSpherePaint: {
			const float sp = sphere_sdf(op, x, y, z);
			if (sp <= 0.0f && s.sdf <= 0.0f) s.material = static_cast<uint16_t>(op.material);
			return s;
		}
		case kOpBoxSubtract: {
			float lo[3], hi[3];
			op_world_aabb(op, lo, hi);
			const float m = op.radius > 0.0f ? op.radius : 0.0f;
			for (int a = 0; a < 3; a++) { lo[a]-=m; hi[a]+=m; }
			const float bd = box_sdf(lo, hi, x, y, z);
			if (-bd > s.sdf) {
				s.sdf = -bd;
				if (s.sdf > 0.0f) s.material = 0;
				float g[3];
				box_sdf_gradient(lo, hi, x, y, z, g);
				s.gradient[0] = -g[0]; s.gradient[1] = -g[1]; s.gradient[2] = -g[2];
				s.exact_gradient = true;
			}
			return s;
		}
		case kOpVolumeAdd: {
			FieldSample vs{};
			if (volumes && volumes->sample_gradient(static_cast<int>(op.aux[0]), x, y, z, op, &vs)) {
				if (vs.sdf < s.sdf) {
					// Same material rule as apply_op and as field.glslh's mirror: the
					// operand's material is taken only where the result is solid AND the
					// operand names one. A wholesale copy also stamped material 0 through
					// air, which is a CPU/GLSL divergence the differential gate exists for.
					const uint16_t previous_material = s.material;
					s = vs;
					s.material = (s.sdf <= 0.0f && vs.material != 0) ? vs.material
																	 : previous_material;
				}
				return s;
			}
			VolumeSample vso{};
			if (!volumes || !volumes->sample(static_cast<int>(op.aux[0]), x, y, z, op, &vso))
				return s;
			if (vso.sdf < s.sdf) {
				s.sdf = vso.sdf;
				if (s.sdf <= 0.0f && vso.material != 0) s.material = vso.material;
				s.gradient[0]=0; s.gradient[1]=0; s.gradient[2]=0;
				s.exact_gradient=false;
			}
			return s;
		}
		default:
			return s;
	}
}

FieldSample apply_ops_gradient(FieldSample s, const EditOp *ops, int count, float x, float y, float z,
		const VolumeStore *volumes) {
	for (int i=0; i<count; i++) s = apply_op_gradient(s, ops[i], x, y, z, volumes);
	return s;
}

bool op_touches_aabb(const EditOp &op, const float lo[3], const float hi[3], float pad) {
	float a[3], b[3];
	op_world_aabb(op, a, b);
	for (int i = 0; i < 3; i++) {
		if (a[i] - pad > hi[i]) return false;
		if (b[i] + pad < lo[i]) return false;
	}
	return true;
}

void op_brick_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	padded_range(op, kBrickSize, kBrickFilterPad, lo, hi);
}

void op_region_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	// Region membership feeds the brick residency path, whose exact conservative pad is the
	// activation margin plus its one-voxel apron. Lattice consumers gather neighbouring
	// region lists through collect_ops_for_aabb() and use kLatticeFilterPad there.
	padded_range(op, kRegionSize, kBrickFilterPad, lo, hi);
}

int op_region_span(const EditOp &op) {
	IVec3 lo{}, hi{};
	op_region_range(op, &lo, &hi);
	const int sx = hi.x - lo.x + 1;
	const int sy = hi.y - lo.y + 1;
	const int sz = hi.z - lo.z + 1;
	return std::max(sx, std::max(sy, sz));
}

bool op_region_span_ok(const EditOp &op) { return op_region_span(op) <= kMaxOpRegionSpan; }

} // namespace ve
