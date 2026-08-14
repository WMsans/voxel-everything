#pragma once
#include <cstdint>

namespace ve {

struct Sample {
	float sdf;         // meters, negative = solid
	uint16_t material; // 0 = air, 1 = grass, 2 = rock, 3 = dirt
};

class Generator {
public:
	virtual ~Generator() = default;
	virtual Sample sample(float x, float y, float z) const = 0;
};

// Deterministic analytic terrain: sine hills + one carved spherical cave.
// Seed is reserved for future variation; M1 output is seed-independent.
class AnalyticGenerator : public Generator {
public:
	explicit AnalyticGenerator(uint32_t seed = 1337) : seed_(seed) {}
	Sample sample(float x, float y, float z) const override;

private:
	uint32_t seed_;
};

} // namespace ve
