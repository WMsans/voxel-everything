#include <doctest/doctest.h>
#include "shade/oct.h"
#include <cmath>

namespace {

// Angle between two vectors, in degrees. Normalize and accumulate in double so float
// round-off in the sampled sphere grid cannot distort the normal-encoding error.
float angle_deg(const float a[3], const float b[3]) {
	const double ax = a[0];
	const double ay = a[1];
	const double az = a[2];
	const double bx = b[0];
	const double by = b[1];
	const double bz = b[2];
	const double d = (ax * bx + ay * by + az * bz) /
			(std::sqrt(ax * ax + ay * ay + az * az) * std::sqrt(bx * bx + by * by + bz * bz));
	const double clamped = d > 1.0 ? 1.0 : (d < -1.0 ? -1.0 : d);
	return static_cast<float>(std::acos(clamped) * 57.2957795);
}

// The G-buffer stores the two components in fp16. Emulating that quantisation here is what
// makes the bound below a statement about the shipped pipeline rather than about doubles.
float to_half_and_back(float v) {
	if (v == 0.0f) return 0.0f;
	const float a = std::fabs(v);
	const int e = static_cast<int>(std::floor(std::log2(a)));
	const float step = std::ldexp(1.0f, e - 10); // fp16 has a 10-bit mantissa
	return std::round(v / step) * step;
}

} // namespace

TEST_CASE("oct_encode round-trips every direction on a dense sphere grid") {
	float worst = 0.0f;
	for (int i = 0; i <= 64; i++) {
		for (int j = 0; j <= 64; j++) {
			const float theta = 3.14159265f * static_cast<float>(i) / 64.0f;
			const float phi = 6.28318531f * static_cast<float>(j) / 64.0f;
			const float n[3] = {std::sin(theta) * std::cos(phi), std::cos(theta),
					std::sin(theta) * std::sin(phi)};
			float e[2];
			ve::oct_encode(n, e);
			CHECK(e[0] >= -1.0f);
			CHECK(e[0] <= 1.0f);
			CHECK(e[1] >= -1.0f);
			CHECK(e[1] <= 1.0f);
			float back[3];
			ve::oct_decode(e, back);
			const float err = angle_deg(n, back);
			worst = err > worst ? err : worst;
		}
	}
	CHECK(worst < 0.01f);
}

TEST_CASE("the fp16 the G-buffer actually stores keeps the error under a quarter degree") {
	float worst = 0.0f;
	for (int i = 0; i <= 48; i++) {
		for (int j = 0; j <= 48; j++) {
			const float theta = 3.14159265f * static_cast<float>(i) / 48.0f;
			const float phi = 6.28318531f * static_cast<float>(j) / 48.0f;
			const float n[3] = {std::sin(theta) * std::cos(phi), std::cos(theta),
					std::sin(theta) * std::sin(phi)};
			float e[2];
			ve::oct_encode(n, e);
			e[0] = to_half_and_back(e[0]);
			e[1] = to_half_and_back(e[1]);
			float back[3];
			ve::oct_decode(e, back);
			const float err = angle_deg(n, back);
			worst = err > worst ? err : worst;
		}
	}
	CHECK(worst < 0.25f);
}

TEST_CASE("the six axis directions survive exactly") {
	const float axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	for (const auto &n : axes) {
		float e[2];
		ve::oct_encode(n, e);
		float back[3];
		ve::oct_decode(e, back);
		CHECK(back[0] == doctest::Approx(n[0]).epsilon(1e-5));
		CHECK(back[1] == doctest::Approx(n[1]).epsilon(1e-5));
		CHECK(back[2] == doctest::Approx(n[2]).epsilon(1e-5));
	}
}

// The lower hemisphere is the folded half of the octahedron and is where a sign mistake
// hides: it round-trips on the axes and is wrong everywhere between them.
TEST_CASE("the folded lower hemisphere round-trips too") {
	const float n[3] = {0.3f, -0.9f, 0.31622776f};
	float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	const float un[3] = {n[0] / len, n[1] / len, n[2] / len};
	float e[2];
	ve::oct_encode(un, e);
	float back[3];
	ve::oct_decode(e, back);
	CHECK(angle_deg(un, back) < 0.01f);
}

TEST_CASE("oct_decode normalizes values and uses the exact fallback for a degenerate input") {
	const float encoded[2] = {0.25f, -0.5f};
	float back[3];
	ve::oct_decode(encoded, back);
	const double l = std::sqrt(static_cast<double>(back[0]) * back[0] +
			static_cast<double>(back[1]) * back[1] + static_cast<double>(back[2]) * back[2]);
	CHECK(l == doctest::Approx(1.0).epsilon(1e-6));

	const float zero[3] = {0, 0, 0};
	float e[2];
	ve::oct_encode(zero, e);
	CHECK(std::isfinite(e[0]));
	CHECK(std::isfinite(e[1]));
	ve::oct_decode(e, back);
	CHECK(std::isfinite(back[0]));
	CHECK(std::isfinite(back[1]));
	CHECK(std::isfinite(back[2]));
	CHECK(back[0] == 0.0f);
	CHECK(back[1] == 0.0f);
	CHECK(back[2] == 1.0f);
}
