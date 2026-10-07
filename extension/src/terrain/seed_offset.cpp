#include "terrain/seed_offset.h"

namespace ve {

SeedOffset seed_offset(uint32_t seed) {
	if (seed == 0) return {};
	// splitmix64: every input bit reaches every output bit, so seeds 41 and 42 land
	// kilometres apart rather than metres.
	uint64_t z = uint64_t(seed) + 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	z ^= z >> 31;
	SeedOffset o;
	o.x = int32_t(z & 0x3FFFu) - 8192;
	o.z = int32_t((z >> 14) & 0x3FFFu) - 8192;
	return o;
}

} // namespace ve
