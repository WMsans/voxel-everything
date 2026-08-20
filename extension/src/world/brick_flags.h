#pragma once
#include "world/brick_mip.h"
#include <cstdint>

namespace ve {

// One word per resident brick, answering the two questions the marcher's brick DDA asks
// before it commits to sphere tracing. Both were previously computed per DDA step from nine
// separate memory reads (eight mip texels plus the palette's slot 0).
inline constexpr uint32_t kBrickFlagHasSurface = 1u;
inline constexpr uint32_t kBrickFlagHasMaterial = 2u;
// What an allocating pass writes before the generator has run. A brick whose generation job
// was dropped keeps the previous occupant's atlas bytes for a frame; saying "march it"
// costs a wasted traversal, saying "skip it" would put a hole in the ground.
inline constexpr uint32_t kBrickFlagConservative = kBrickFlagHasSurface | kBrickFlagHasMaterial;

uint32_t brick_flags_from_mips(const BrickMips &mips, uint16_t palette_slot0);

inline bool brick_flag_has_surface(uint32_t f) { return (f & kBrickFlagHasSurface) != 0u; }
inline bool brick_flag_has_material(uint32_t f) { return (f & kBrickFlagHasMaterial) != 0u; }

} // namespace ve
