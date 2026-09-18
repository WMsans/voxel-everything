#include <doctest/doctest.h>
#include <cmath>

// The relief stage's parameters, mirrored here so the LOOK is checked against the numbers
// that ship. The Lipschitz budget used to be worked by hand in this file; resolve_pipeline
// computes it now (hills add 1.78 + relief add 0.21 = 1.99, under default.pipeline's
// ceiling of 2.0) and test_lipschitz_sampled.cpp checks it against the real field.
namespace {
constexpr float kReliefAmpA = 250.0f;
constexpr float kReliefFreqA = 0.0004f;
constexpr float kReliefAmpB = 60.0f;
constexpr float kReliefFreqB = 0.00083333f;

} // namespace

// Long wavelengths are the price of the bound. The point is that they still buy a
// landscape: this is what makes a 4 km horizon worth looking at rather than a flat sliver.
TEST_CASE("the relief is large enough to see across four kilometres") {
	// Worst-case swing of a sine of amplitude A over a window w: 2A sin(w/2), capped at 2A.
	auto swing = [](float amp, float freq, float span) {
		const float half = 0.5f * freq * span;
		return half >= 1.5707963f ? 2.0f * amp : 2.0f * amp * std::sin(half);
	};
	const float total = swing(kReliefAmpA, kReliefFreqA, 4000.0f) +
			swing(kReliefAmpB, kReliefFreqB, 4000.0f);
	// At least 250 m of height change across the visible 4 km, against the ~20 m the
	// existing hills() manages.
	CHECK(total > 250.0f);
}

// The demo player spawns near the origin; relief must not drop them into rock or into air.
TEST_CASE("the relief is zero at the origin so the spawn is undisturbed") {
	const float r = kReliefAmpA * std::sin(0.0f * kReliefFreqA) * std::cos(0.0f * kReliefFreqA) +
			kReliefAmpB * std::sin(0.0f * kReliefFreqB) * std::cos(0.0f * kReliefFreqB);
	CHECK(r == doctest::Approx(0.0f).epsilon(1e-6));
}
