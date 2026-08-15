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

void palette_occupancy_order(const uint16_t *palette, const int *counts, int *out_order) {
	for (int i = 0; i < kBrickPaletteSize; i++) out_order[i] = i;
	// Selection sort over four entries: small, branch-explicit, and trivially mirrored in
	// GLSL by a single thread (see shaders/brick_gen.comp.glsl).
	for (int a = 0; a < kBrickPaletteSize; a++)
		for (int b = a + 1; b < kBrickPaletteSize; b++) {
			const int ia = out_order[a], ib = out_order[b];
			const bool a_empty = palette[ia] == 0, b_empty = palette[ib] == 0;
			bool swap = false;
			if (a_empty != b_empty) {
				swap = a_empty; // non-empty slots always precede empty ones
			} else if (!a_empty) {
				swap = counts[ib] > counts[ia] ||
						(counts[ib] == counts[ia] && palette[ib] < palette[ia]);
			}
			if (swap) { out_order[a] = ib; out_order[b] = ia; }
		}
}

} // namespace ve
