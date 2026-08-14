#include "generator/generator.h"
#include <cmath>

namespace ve {

static float hills(float x, float z) {
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

Sample AnalyticGenerator::sample(float x, float y, float z) const {
	const float h = hills(x, z);
	float sdf = (y - kSurfaceY) - h;

	// Carved cave: sphere at (30, kSurfaceY + hills(30,30) - 2, 30), radius 5.
	const float cx = 30.0f, cz = 30.0f;
	const float cy = kSurfaceY + hills(cx, cz) - 2.0f;
	const float dx = x - cx, dy = y - cy, dz = z - cz;
	const float sphere = sqrtf(dx * dx + dy * dy + dz * dz) - 5.0f;
	sdf = fmaxf(sdf, -sphere); // CSG subtract

	uint16_t mat = 0;
	if (sdf <= 0.0f) {
		mat = h > 4.0f ? 2 : (h > 1.0f ? 1 : 3);
	}
	return {sdf, mat};
}

} // namespace ve
