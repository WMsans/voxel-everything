#include "shade/sun_state.h"
#include <cmath>

namespace ve {

bool SunState::has_basis() const {
	const float r = right[0] * right[0] + right[1] * right[1] + right[2] * right[2];
	const float u = up[0] * up[0] + up[1] * up[1] + up[2] * up[2];
	return r > 1e-8f && u > 1e-8f;
}

float srgb_to_linear(float c) {
	if (c <= 0.0f) return 0.0f;
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace ve
