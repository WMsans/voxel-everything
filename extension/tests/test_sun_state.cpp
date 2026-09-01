#include <doctest/doctest.h>
#include "shade/sun_state.h"
#include "shade/cel.h"
#include <cmath>

TEST_CASE("a default SunState is today's constant sun, white, with no basis") {
	const ve::SunState s;
	CHECK(s.dir[0] == doctest::Approx(ve::kSunDir[0]));
	CHECK(s.dir[1] == doctest::Approx(ve::kSunDir[1]));
	CHECK(s.dir[2] == doctest::Approx(ve::kSunDir[2]));
	CHECK(s.rgb[0] == doctest::Approx(1.0f));
	CHECK(s.rgb[1] == doctest::Approx(1.0f));
	CHECK(s.rgb[2] == doctest::Approx(1.0f));
	// No authored basis: callers must fall back to sun_ortho's direction-only overload.
	CHECK_FALSE(s.has_basis());
}

TEST_CASE("a basis counts as authored only when both axes are non-degenerate") {
	ve::SunState s;
	s.right[0] = 1.0f;
	CHECK_FALSE(s.has_basis()); // up is still zero
	s.up[1] = 1.0f;
	CHECK(s.has_basis());
}

TEST_CASE("srgb_to_linear matches the standard piecewise curve") {
	CHECK(ve::srgb_to_linear(0.0f) == doctest::Approx(0.0f));
	CHECK(ve::srgb_to_linear(1.0f) == doctest::Approx(1.0f));
	// Below the knee the curve is a plain divide.
	CHECK(ve::srgb_to_linear(0.04f) == doctest::Approx(0.04f / 12.92f).epsilon(1e-6));
	// Above it, the gamma branch. 0.5 sRGB is a well-known ~0.2140 linear.
	CHECK(ve::srgb_to_linear(0.5f) == doctest::Approx(0.21404f).epsilon(1e-4));
	// Monotonic, and never above its input in the interior.
	CHECK(ve::srgb_to_linear(0.7f) > ve::srgb_to_linear(0.3f));
	CHECK(ve::srgb_to_linear(0.5f) < 0.5f);
}
