#include "analytic_oracle.h"
#include "world/material_table.h"
#include <cmath>

namespace {
// The analytic generator's height bands, by name: rock above 4 m, grass above 1 m, ground below.
constexpr uint16_t kBandRock = ve::material_id("rock");
constexpr uint16_t kBandGrass = ve::material_id("grass_01");
constexpr uint16_t kBandGround = ve::material_id("ground_01");
} // namespace

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
		mat = h > 4.0f ? kBandRock : (h > 1.0f ? kBandGrass : kBandGround);
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
		mat = h > 4.0f ? kBandRock : (h > 1.0f ? kBandGrass : kBandGround);
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
