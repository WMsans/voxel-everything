#include "lod/lod_quad.h"
#include "lod/lod_skirt.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

// A field can straddle at most two of the three words (the widest is 16 bits), so two
// masked writes always suffice. Doing it this way rather than by hand-written shifts is
// what makes the 78-bit material offset survive; it is the field a naive layout truncates.
void bits_set(uint32_t *w, int lo, int bits, uint32_t v) {
	const uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
	v &= mask;
	const int word = lo >> 5;
	const int shift = lo & 31;
	w[word] |= v << shift;
	const int spill = shift + bits - 32;
	if (spill > 0) w[word + 1] |= v >> (32 - shift);
}

uint32_t bits_get(const uint32_t *w, int lo, int bits) {
	const uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
	const int word = lo >> 5;
	const int shift = lo & 31;
	uint32_t v = w[word] >> shift;
	const int spill = shift + bits - 32;
	if (spill > 0) v |= w[word + 1] << (32 - shift);
	return v & mask;
}

constexpr int kBitU = 0;
constexpr int kBitAxis = 15;
constexpr int kBitSign = 17;
constexpr int kBitOffset = 18;
constexpr int kBitMaterial = 78;
constexpr int kBitDoubleSided = 94;
constexpr int kBitReverseWinding = 95;

} // namespace

void lod_quad_pack(const LodQuadFields &f, LodQuad *out) {
	out->w[0] = out->w[1] = out->w[2] = 0u;
	if (f.double_sided) {
		const int a = f.skirt_face / 2;
		bits_set(out->w, 0, 5, f.u[(a + 1) % 3]);
		bits_set(out->w, 5, 5, f.u[(a + 2) % 3]);
		bits_set(out->w, 10, 3, f.skirt_face);
		bits_set(out->w, 13, 2, f.skirt_edge);
	} else {
		for (int a = 0; a < 3; a++) bits_set(out->w, kBitU + a * 5, 5, f.u[a]);
	}
	bits_set(out->w, kBitAxis, 2, f.axis);
	bits_set(out->w, kBitSign, 1, f.sign);
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++)
			bits_set(out->w, kBitOffset + (k * 3 + a) * 5, 5, f.offset[k][a]);
	bits_set(out->w, kBitMaterial, 16, f.material);
	bits_set(out->w, kBitDoubleSided, 1, f.double_sided);
	bits_set(out->w, kBitReverseWinding, 1, f.reverse_winding);
}

void lod_quad_unpack(const LodQuad &q, LodQuadFields *out) {
	out->double_sided = static_cast<uint8_t>(bits_get(q.w, kBitDoubleSided, 1));
	out->reverse_winding = static_cast<uint8_t>(bits_get(q.w, kBitReverseWinding, 1));
	out->skirt_face = out->double_sided ? static_cast<uint8_t>(bits_get(q.w, 10, 3)) : 0;
	out->skirt_edge = out->double_sided ? static_cast<uint8_t>(bits_get(q.w, 13, 2)) : 0;
	if (out->double_sided) {
		const int a = (out->skirt_face / 2) % 3;
		out->u[a] = (out->skirt_face & 1) ? 31 : 0;
		out->u[(a + 1) % 3] = static_cast<uint8_t>(bits_get(q.w, 0, 5));
		out->u[(a + 2) % 3] = static_cast<uint8_t>(bits_get(q.w, 5, 5));
	} else {
		for (int a = 0; a < 3; a++)
			out->u[a] = static_cast<uint8_t>(bits_get(q.w, kBitU + a * 5, 5));
	}
	out->axis = static_cast<uint8_t>(bits_get(q.w, kBitAxis, 2));
	out->sign = static_cast<uint8_t>(bits_get(q.w, kBitSign, 1));
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++)
			out->offset[k][a] =
					static_cast<uint8_t>(bits_get(q.w, kBitOffset + (k * 3 + a) * 5, 5));
	out->material = static_cast<uint16_t>(bits_get(q.w, kBitMaterial, 16));
}

uint8_t lod_quantise_offset(float frac) {
	const float c = std::max(0.0f, std::min(frac, 1.0f));
	return static_cast<uint8_t>(std::floor(c * float(kLodOffsetMax) + 0.5f));
}

namespace {

// The stored order for a reversed quad: corner k of the packed record holds the offset of
// canonical corner kLodQuadCorners[order_rev[k]]. Task 4 stores negative-sign quads with this
// permutation so they are already wound toward air; the decoder must undo it to reach the
// canonical cell table.
constexpr int kLodQuadOrderRev[4] = {0, 3, 2, 1};

} // namespace

void lod_quad_corner_cell(const LodQuadFields &f, int k, int m[3]) {
	const int axis = f.axis % 3;
	const int b = (axis + 1) % 3;
	const int c = (axis + 2) % 3;
	const int kk = k & 3;
	// Sign-aware decoding: the packed corner offsets are ALREADY WOUND toward air. For
	// sign == 1 (solid below), corner k is the canonical corner k. For sign == 0 (reversed),
	// corner k holds the offset of canonical corner order_rev[k] = {0, 3, 2, 1}.
	const int canonical = (f.sign == 0) ? kLodQuadOrderRev[kk] : kk;
	m[0] = f.u[0] + 1;
	m[1] = f.u[1] + 1;
	m[2] = f.u[2] + 1;
	m[b] += kLodQuadCorners[canonical][0];
	m[c] += kLodQuadCorners[canonical][1];
}

void lod_quad_parent_corner_pos(const LodQuadFields &f, int k, const float origin[3], float cell,
		float out[3]) {
	int m[3];
	lod_quad_corner_cell(f, k, m);
	const int kk = k & 3;
	// The quantised offset remains in the stored (pre-wound) order; only the cell lookup is
	// remapped by the sign-aware corner decoder above.
	for (int a = 0; a < 3; a++) {
		const float frac = static_cast<float>(f.offset[kk][a]) / static_cast<float>(kLodOffsetMax);
		// Add in global cell space before multiplying: neighboring chunks must evaluate
		// their shared vertex with identical arithmetic, not two rounded chunk sums.
		const float base = std::round(origin[a] / cell);
		const float residual = origin[a] - base * cell;
		out[a] = (base + static_cast<float>(m[a] - 1) + frac) * cell + residual;
	}
}

void lod_quad_normal(const LodQuadFields &f, float out[3]) {
	const float origin[3] = {};
	float p[4][3];
	for (int k = 0; k < 4; k++) lod_quad_parent_corner_pos(f, k, origin, 1.0f, p[k]);
	// The diagonal cross is the sum of both triangle area normals. A tiny/collapsed
	// first triangle must not determine the lighting of an entire coarse quad.
	float a[3], b[3];
	for (int i = 0; i < 3; i++) { a[i] = p[1][i] - p[3][i]; b[i] = p[2][i] - p[0][i]; }
	float length2 = 0;
	for (int i = 0; i < 3; i++) {
		out[i] = a[(i + 1) % 3] * b[(i + 2) % 3] - a[(i + 2) % 3] * b[(i + 1) % 3];
		length2 += out[i] * out[i];
	}
	if (length2 > 1e-12f) {
		const float inv = 1.0f / std::sqrt(length2);
		for (int i = 0; i < 3; i++) out[i] *= inv;
	} else {
		// A fully collapsed quad has no shading normal; keep the decoder finite.
		out[0] = out[1] = out[2] = 0;
		out[f.axis % 3] = f.sign ? 1.0f : -1.0f;
	}
}

void lod_quad_corner_normal(const LodQuadFields &f, int k, float out[3]) {
	const float origin[3] = {};
	const int kk = k & 3;
	float p[3][3];
	lod_quad_parent_corner_pos(f, kk, origin, 1.0f, p[0]);
	lod_quad_parent_corner_pos(f, (kk + 1) & 3, origin, 1.0f, p[1]);
	lod_quad_parent_corner_pos(f, (kk + 3) & 3, origin, 1.0f, p[2]);
	float a[3], b[3];
	for (int i = 0; i < 3; ++i) { a[i] = p[1][i] - p[0][i]; b[i] = p[2][i] - p[0][i]; }
	float length2 = 0.0f;
	for (int i = 0; i < 3; ++i) {
		out[i] = a[(i + 1) % 3] * b[(i + 2) % 3] - a[(i + 2) % 3] * b[(i + 1) % 3];
		length2 += out[i] * out[i];
	}
	if (length2 > 1e-12f) {
		const float inv = 1.0f / std::sqrt(length2);
		for (int i = 0; i < 3; ++i) out[i] *= inv;
	} else {
		lod_quad_normal(f, out);
	}
}

namespace {
bool skirt_displacement(const LodQuadFields &f, int endpoint, float d[3]) {
	int m[3];
	lod_quad_corner_cell(f, endpoint, m);
	float n[3];
	lod_quad_normal(f, n);
	d[0] = d[1] = d[2] = 0;
	bool boundary[3] = {};
	const int face_axis = f.skirt_face / 2;
	for (int a = 0; a < 3; a++) {
		// A plane in the last normal-axis cell is not a corner on that axis. Include that
		// axis only when the exposed edge really uses its face (e.g. a slope exiting +Y).
		boundary[a] = (a != f.axis || a == face_axis) && (m[a] == 0 || m[a] == 32);
		if (boundary[a]) d[a] = float(kLodSkirtCells) * (m[a] == 0 ? -1.0f : 1.0f);
	}
	float projection = 0;
	for (int a = 0; a < 3; a++) projection += d[a] * n[a];
	// Outward can already be inward at a corner. Do not destroy a feasible displacement.
	if (projection > -float(kLodSkirtCells)) {
		// Keep the footprint exact. Distribute the inward deficit over ALL free axes,
		// saturating a coordinate only when necessary and solving the remainder again.
		// A single-axis solve can reject feasible slopes; blindly clamping loses depth.
		float deficit = float(kLodSkirtCells) + projection;
		for (int pass = 0; pass < 3; pass++) {
			float weight = 0;
			for (int a = 0; a < 3; a++) if (!boundary[a]) weight += n[a] * n[a];
			if (weight <= 1e-10f) return false;
			const float scale = deficit / weight;
			bool saturated = false;
			for (int a = 0; a < 3; a++) if (!boundary[a] &&
					std::fabs(scale * n[a]) > float(kLodSkirtMaxExtensionCells)) {
				d[a] = n[a] > 0 ? -float(kLodSkirtMaxExtensionCells) : float(kLodSkirtMaxExtensionCells);
				deficit += d[a] * n[a];
				boundary[a] = true;
				saturated = true;
			}
			if (!saturated) {
				for (int a = 0; a < 3; a++) if (!boundary[a]) d[a] = -scale * n[a];
				break;
			}
		}
	}
	for (int a = 0; a < 3; a++)
		if (!std::isfinite(d[a]) || std::fabs(d[a]) > float(kLodSkirtMaxExtensionCells)) return false;
	return true;
}
} // namespace

bool lod_quad_skirt_supported(const LodQuadFields &f) {
	float d[3];
	return skirt_displacement(f, f.skirt_edge, d) &&
			skirt_displacement(f, (f.skirt_edge + 1) & 3, d);
}

void lod_quad_corner_pos(const LodQuadFields &f, int k, const float origin[3], float cell,
		float out[3]) {
	if (!f.double_sided) {
		lod_quad_parent_corner_pos(f, k, origin, cell, out);
		return;
	}
	const int kk = f.reverse_winding ? kLodQuadOrderRev[k & 3] : (k & 3);
	// Reverse the attached edge relative to the parent: end, start, outer-start, outer-end.
	const int endpoint = (f.skirt_edge + ((kk == 0 || kk == 3) ? 1 : 0)) & 3;
	lod_quad_parent_corner_pos(f, endpoint, origin, cell, out);
	if (kk < 2) return;
	float d[3];
	// Unsupported records are never emitted. Keep foreign/malformed records finite too.
	if (!skirt_displacement(f, endpoint, d)) return;
	for (int a = 0; a < 3; a++) out[a] += d[a] * cell;
}

} // namespace ve
