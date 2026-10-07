#pragma once
// The world seed as a translation of the field's domain
// (docs/superpowers/specs/2026-10-07-title-menu-world-types-design.md §5.1).
//
// Every shipped stage is a sum of fixed sinusoids, so a seed cannot reseed anything; it picks
// WHERE in the unbounded field the world origin sits. A shift is applied once per side
// (generate_field_glslh's VE_FIELD_OFFSET, PipelineFieldGenerator::sample), and a translation
// cannot change a gradient, so every stage's //!lipschitz still holds untouched.
#include <cstdint>

namespace ve {

struct SeedOffset {
	int32_t x = 0;
	int32_t z = 0;
};

// 0 -> (0, 0): the terrain the demo, the benchmark and every golden were recorded on.
// Any other seed -> two whole metres in [-8192, 8192). Whole metres are exact in float, and
// 8 km keeps a sample position's float resolution under 1 mm against 5 cm voxels -- the
// normal's 5 cm finite differences would facet somewhere past 100 km.
SeedOffset seed_offset(uint32_t seed);

} // namespace ve
