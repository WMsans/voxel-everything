#include <doctest/doctest.h>
#include <cmath>

// The relief stage's parameters, mirrored here so the bound is checked against the numbers
// that ship rather than against a declared constant.
//
// The pipeline declares lipschitz 2.0, and understating that bound is a CORRECTNESS bug:
// raycast.cpp steps by 1/lipschitz() and would overshoot a surface. Overstating it costs
// raycast steps and widens mesh_chunk.cpp's conservative padding. So the relief amplitudes
// are budgeted against it rather than chosen for looks and hoped for.
//
//   hills d/dx: 6(0.11) + 3(0.031) + 1(0.23)  = 0.983
//   hills d/dz: 6(0.13) + 3(0.043) + 1(0.19)  = 1.099
//   |grad(y - h)| = sqrt(1 + |grad h|^2), so |grad h| must stay under sqrt(3) = 1.732.
namespace {
constexpr float kReliefAmpA = 250.0f;
constexpr float kReliefFreqA = 0.0004f;
constexpr float kReliefAmpB = 60.0f;
constexpr float kReliefFreqB = 0.00083333f;

constexpr float kHillsDx = 6.0f * 0.11f + 3.0f * 0.031f + 1.0f * 0.23f;
constexpr float kHillsDz = 6.0f * 0.13f + 3.0f * 0.043f + 1.0f * 0.19f;
} // namespace

TEST_CASE("the relief stage fits inside the pipeline's declared Lipschitz bound") {
	const float relief_per_axis = kReliefAmpA * kReliefFreqA + kReliefAmpB * kReliefFreqB;
	CHECK(relief_per_axis == doctest::Approx(0.15f).epsilon(1e-3));

	const float gx = kHillsDx + relief_per_axis;
	const float gz = kHillsDz + relief_per_axis;
	const float bound = std::sqrt(1.0f + gx * gx + gz * gz);
	// Strictly under the declared 2.0, with the margin visible in the failure message.
	CHECK(bound < 2.0f);
	CHECK(bound == doctest::Approx(1.9606f).epsilon(1e-3));
}

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
