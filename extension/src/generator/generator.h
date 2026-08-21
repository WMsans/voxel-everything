#pragma once
#include <cstdint>

namespace ve {

// The default world origin (0, -64, 0) puts the terrain surface here: kSurfaceY =
// -origin_bricks.y * kBrickSize = 64 * 0.8 = 51.2 m. The generator field carries this
// offset explicitly ("sdf = (y - kSurfaceY) - hills"), so the surface sits at
// y = 51.2 + hills(x, z) in [-10, 10] -> [41.2, 61.2] m. shaders/field.glslh mirrors this
// as SURFACE_Y; the field-diff test guards that the two agree.
inline constexpr float kSurfaceY = 51.2f;

struct Sample {
	float sdf;         // meters, negative = solid
	uint16_t material; // 0 = air, 1 = grass, 2 = rock, 3 = dirt
};

struct FieldSample {
	float sdf = 0;
	uint16_t material = 0;
	float gradient[3] = {0, 1, 0};
	bool exact_gradient = false;
};

class Generator {
public:
	virtual ~Generator() = default;
	virtual Sample sample(float x, float y, float z) const = 0;
	virtual FieldSample sample_gradient(float x, float y, float z) const;

	// Upper bound on |grad(sdf)| of the field this generator returns. The generator's
	// `y - h(x, z)` OVER-estimates true distance on a slope, so a sphere tracer that
	// stepped by the reported value would tunnel through overhangs. Stepping by
	// reported / lipschitz() is the standard safe bound: true_distance >= |reported| / L.
	virtual float lipschitz() const { return 2.0f; }
};

// Deterministic analytic terrain: sine hills + one carved spherical cave.
// Seed is reserved for future variation; M1 output is seed-independent.
class AnalyticGenerator : public Generator {
public:
	explicit AnalyticGenerator(uint32_t seed = 1337) : seed_(seed) {}
	Sample sample(float x, float y, float z) const override;
	FieldSample sample_gradient(float x, float y, float z) const override;

	// |grad(y - hills)| = sqrt(1 + |grad hills|^2); the amplitude-times-frequency sum of
	// hills() is below 1.0 per axis, so 2.0 is comfortably conservative. The cave is a
	// unit-gradient sphere combined with max(), which cannot raise the bound.
	float lipschitz() const override { return 2.0f; }

private:
	uint32_t seed_;
};

} // namespace ve
