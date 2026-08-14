#pragma once
#include "world/brick.h"
#include <cstdint>

namespace ve {

// Per-brick min–max acceleration chain (spec §2). Level L partitions the brick into
// kMipDims[L]^3 cells; each entry is the min / max of the ENCODED sdf lattice over the
// cell's INCLUSIVE corner range. Inclusive matters: the trilinear reconstruction inside a
// cell is a multilinear interpolant of its corner samples and therefore never leaves their
// convex hull, so an inclusive min/max is a sound bound and an exclusive one is not.
inline constexpr int kMipLevels = 3;
inline constexpr int kMipDims[kMipLevels] = {2, 4, 8};

// encode_sdf(0.0f). A cell contains no surface when its whole range sits on one side.
inline constexpr uint8_t kEncodedZero = 128;

struct BrickMips {
	uint8_t mn2[8]{},   mx2[8]{};
	uint8_t mn4[64]{},  mx4[64]{};
	uint8_t mn8[512]{}, mx8[512]{};
};

// sdf_lattice must hold kBrickSdfCount encoded samples in sdf_index() order.
void build_brick_mips(const uint8_t *sdf_lattice, BrickMips *out);

inline bool mip_cell_has_surface(uint8_t mn, uint8_t mx) {
	return mn <= kEncodedZero && mx >= kEncodedZero;
}

const uint8_t *mip_min(const BrickMips &m, int level);
const uint8_t *mip_max(const BrickMips &m, int level);

} // namespace ve
