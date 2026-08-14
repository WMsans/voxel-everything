#include "world/palette.h"
#include "world/brick.h"
#include <cstdlib>

namespace ve {

int palette_slot(uint16_t *palette, uint16_t mat_id, bool *overflow) {
	if (overflow) *overflow = false;
	int free_slot = -1;
	for (int i = 0; i < kBrickPaletteSize; i++) {
		if (palette[i] == mat_id) return i;
		if (palette[i] == 0 && free_slot < 0) free_slot = i;
	}
	if (free_slot >= 0) {
		palette[free_slot] = mat_id;
		return free_slot;
	}
	if (overflow) *overflow = true;
	int best = 0;
	int best_dist = std::abs(static_cast<int>(palette[0]) - static_cast<int>(mat_id));
	for (int i = 1; i < kBrickPaletteSize; i++) {
		int dist = std::abs(static_cast<int>(palette[i]) - static_cast<int>(mat_id));
		if (dist < best_dist) { best = i; best_dist = dist; }
	}
	return best;
}

} // namespace ve
