#pragma once
// TEST-ONLY. The terrain the engine shipped with, kept as the oracle that
// assets/pipelines/golden.pipeline is proved against (test_pipeline_equivalence.cpp) and
// that tests/golden/field_baseline.txt and brick_baseline.txt are pinned to.
//
// It lived in src/generator/ as the fallback field, which made it a fourth copy of the
// terrain: stage GLSL, stage C++ mirror, the field.glslh stub, and this. Nothing in the
// engine generates terrain from it any more, and nothing in src/ may include this header.
#include "generator/generator.h"

namespace ve {

// Deterministic analytic terrain: sine hills + one carved spherical cave.
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
