#include "lod/lod_quad.h"
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

} // namespace

void lod_quad_pack(const LodQuadFields &f, LodQuad *out) {
	out->w[0] = out->w[1] = out->w[2] = 0u;
	for (int a = 0; a < 3; a++) bits_set(out->w, kBitU + a * 5, 5, f.u[a]);
	bits_set(out->w, kBitAxis, 2, f.axis);
	bits_set(out->w, kBitSign, 1, f.sign);
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++)
			bits_set(out->w, kBitOffset + (k * 3 + a) * 5, 5, f.offset[k][a]);
	bits_set(out->w, kBitMaterial, 16, f.material);
	bits_set(out->w, kBitDoubleSided, 1, f.double_sided);
}

void lod_quad_unpack(const LodQuad &q, LodQuadFields *out) {
	for (int a = 0; a < 3; a++)
		out->u[a] = static_cast<uint8_t>(bits_get(q.w, kBitU + a * 5, 5));
	out->axis = static_cast<uint8_t>(bits_get(q.w, kBitAxis, 2));
	out->sign = static_cast<uint8_t>(bits_get(q.w, kBitSign, 1));
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++)
			out->offset[k][a] =
					static_cast<uint8_t>(bits_get(q.w, kBitOffset + (k * 3 + a) * 5, 5));
	out->material = static_cast<uint16_t>(bits_get(q.w, kBitMaterial, 16));
	out->double_sided = static_cast<uint8_t>(bits_get(q.w, kBitDoubleSided, 1));
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

void lod_quad_corner_pos(const LodQuadFields &f, int k, const float origin[3], float cell,
		float out[3]) {
	int m[3];
	lod_quad_corner_cell(f, k, m);
	const int kk = k & 3;
	// The quantised offset remains in the stored (pre-wound) order; only the cell lookup is
	// remapped by the sign-aware corner decoder above.
	for (int a = 0; a < 3; a++) {
		const float frac = static_cast<float>(f.offset[kk][a]) / static_cast<float>(kLodOffsetMax);
		out[a] = origin[a] + (static_cast<float>(m[a]) - 1.0f + frac) * cell;
	}
}

} // namespace ve
