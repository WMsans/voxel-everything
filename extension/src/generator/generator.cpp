#include "generator/generator.h"
#include <algorithm>
#include <cmath>

namespace ve {

static float hills(float x, float z) {
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

FieldSample Generator::sample_gradient(float x, float y, float z) const {
	Sample s = sample(x, y, z);
	const float e = 0.01f;
	const float dx = (sample(x + e, y, z).sdf - sample(x - e, y, z).sdf) / (2.0f * e);
	const float dy = (sample(x, y + e, z).sdf - sample(x, y - e, z).sdf) / (2.0f * e);
	const float dz = (sample(x, y, z + e).sdf - sample(x, y, z - e).sdf) / (2.0f * e);
	FieldSample fs{};
	fs.sdf = s.sdf;
	fs.material = s.material;
	fs.gradient[0] = dx;
	fs.gradient[1] = dy;
	fs.gradient[2] = dz;
	fs.exact_gradient = false;
	return fs;
}

uint16_t oct_encode_snorm8(const float normal[3]) {
	(void)normal;
	return 0;
}

void oct_decode_snorm8(uint16_t packed, float normal[3]) {
	(void)packed;
	normal[0] = 0;
	normal[1] = 1;
	normal[2] = 0;
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

FieldSample AnalyticGenerator::sample_gradient(float x, float y, float z) const {
	const float h = hills(x, z);
	float terrain_sdf = (y - kSurfaceY) - h;
	const float cx = 30.0f, cz = 30.0f;
	const float cy = kSurfaceY + hills(cx, cz) - 2.0f;
	const float dx = x - cx, dy = y - cy, dz = z - cz;
	const float len = sqrtf(dx * dx + dy * dy + dz * dz);
	const float sphere = len - 5.0f;
	float sdf = fmaxf(terrain_sdf, -sphere);
	uint16_t mat = 0;
	if (sdf <= 0.0f) {
		mat = h > 4.0f ? 2 : (h > 1.0f ? 1 : 3);
	}
	const float dhdx = 0.66f * cosf(x * 0.11f) * cosf(z * 0.13f)
	        + 0.093f * cosf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	        + 0.23f * cosf(x * 0.23f + z * 0.19f);
	const float dhdz = -0.78f * sinf(x * 0.11f) * sinf(z * 0.13f)
	        + 0.129f * sinf(x * 0.031f + 1.7f) * cosf(z * 0.043f)
	        + 0.19f * cosf(x * 0.23f + z * 0.19f);
	FieldSample fs{};
	fs.sdf = sdf;
	fs.material = mat;
	fs.gradient[0] = -dhdx;
	fs.gradient[1] = 1.0f;
	fs.gradient[2] = -dhdz;
	fs.exact_gradient = true;
	if (-sphere > terrain_sdf) {
		if (len < 1e-6f) {
			fs.gradient[0] = 0.0f;
			fs.gradient[1] = 1.0f;
			fs.gradient[2] = 0.0f;
			fs.exact_gradient = false;
		} else {
			fs.gradient[0] = -(dx / len);
			fs.gradient[1] = -(dy / len);
			fs.gradient[2] = -(dz / len);
			fs.exact_gradient = true;
		}
	}
	return fs;
}

} // namespace ve
