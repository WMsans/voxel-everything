#pragma once
#include <cstdint>

namespace ve {

// Returns the slot (0..3) holding mat_id, inserting into a free slot if needed.
// Free slots are marked 0 (material 0 = air never occupies a palette entry).
// If full: sets *overflow=true and returns the slot with the numerically nearest id.
int palette_slot(uint16_t *palette, uint16_t mat_id, bool *overflow);

// Sort order for a brick's palette. Slot 0 must hold the brick's DOMINANT material: a cell
// that never got a material keeps packed index 0, and index 0 is indistinguishable from
// palette slot 0, so whatever sits there is what such a cell renders as. Ordering by cell
// count makes that fallback the brick's most likely surface.
//
// Ties break on the lower material id, and empty slots (id 0) always sort last. Both rules
// exist so the GPU generator can reproduce this ordering bit for bit without a stable sort.
//
// counts[i] is the number of cells that resolved to palette slot i. out_order[k] receives
// the ORIGINAL slot that belongs at final position k, i.e. new_palette[k] = palette[out_order[k]].
void palette_occupancy_order(const uint16_t *palette, const int *counts, int *out_order);

} // namespace ve
