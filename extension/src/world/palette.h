#pragma once
#include <cstdint>

namespace ve {

// Returns the slot (0..3) holding mat_id, inserting into a free slot if needed.
// Free slots are marked 0 (material 0 = air never occupies a palette entry).
// If full: sets *overflow=true and returns the slot with the numerically nearest id.
int palette_slot(uint16_t *palette, uint16_t mat_id, bool *overflow);

} // namespace ve
