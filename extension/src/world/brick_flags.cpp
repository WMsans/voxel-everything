#include "world/brick_flags.h"

namespace ve {

uint32_t brick_flags_from_mips(const BrickMips &mips, uint16_t palette_slot0) {
	// The 2^3 level is the whole brick in eight cells; reducing it is exactly the test the
	// marcher used to run inline. Inclusive min/max over trilinear corners means this can
	// never hide a crossing (see brick_mip.h).
	uint8_t mn = 255, mx = 0;
	for (int i = 0; i < 8; i++) {
		if (mips.mn2[i] < mn) mn = mips.mn2[i];
		if (mips.mx2[i] > mx) mx = mips.mx2[i];
	}
	uint32_t flags = 0u;
	if (mip_cell_has_surface(mn, mx)) flags |= kBrickFlagHasSurface;
	// palette_occupancy_order puts the dominant material in slot 0, and id 0 means "no
	// material" -- a brick generated entirely out of air that still straddles zero in its
	// apron plane, which the marcher must not report as a hit.
	if (palette_slot0 != 0) flags |= kBrickFlagHasMaterial;
	return flags;
}

} // namespace ve
