#include "shade/oct.h"
#include <cmath>

namespace {

// Deliberately NOT copysign: copysign(1, -0.0) is -1, and a normal whose component is
// negative zero would then fold to the opposite octant. The GLSL mirror uses the same
// >= 0 test for exactly this reason.
inline float sign_not_zero(float v) {
	return v >= 0.0f ? 1.0f : -1.0f;
}

inline void fold(float &x, float &y) {
	const float ax = std::fabs(x);
	const float ay = std::fabs(y);
	const float fx = (1.0f - ay) * sign_not_zero(x);
	const float fy = (1.0f - ax) * sign_not_zero(y);
	x = fx;
	y = fy;
}

} // namespace

namespace ve {

void oct_encode(const float n[3], float out[2]) {
	const float l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
	const float inv = l1 > 0.0f ? 1.0f / l1 : 0.0f;
	float x = n[0] * inv;
	float y = n[1] * inv;
	const float z = n[2] * inv;
	if (z < 0.0f) fold(x, y);
	out[0] = x;
	out[1] = y;
}

void oct_decode(const float e[2], float out[3]) {
	float x = e[0];
	float y = e[1];
	const float z = 1.0f - std::fabs(x) - std::fabs(y);
	if (z < 0.0f) fold(x, y);
	const float len = std::sqrt(x * x + y * y + z * z);
	if (!(len > 0.0f)) {
		out[0] = 0.0f;
		out[1] = 0.0f;
		out[2] = 1.0f;
		return;
	}
	const float inv = 1.0f / len;
	out[0] = x * inv;
	out[1] = y * inv;
	out[2] = z * inv;
}

} // namespace ve
