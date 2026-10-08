#include "terrain/sector.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace ve {
namespace {
int imod(int a, int b) {
	const int m = a % b;
	return m < 0 ? m + b : m;
}
float lo16(uint32_t v) { return float(v & 0xffffu) / 65535.0f; }
float hi16(uint32_t v) { return float(v >> 16) / 65535.0f; }
// Cubic B-spline weights and their derivatives at fraction t. Mirrored in sector.glslh.
void weights(float t, float w[4], float d[4]) {
	const float s = 1.0f - t, t2 = t * t, t3 = t2 * t;
	w[0] = s * s * s / 6.0f;
	w[1] = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
	w[2] = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
	w[3] = t3 / 6.0f;
	d[0] = -0.5f * s * s;
	d[1] = 1.5f * t2 - 2.0f * t;
	d[2] = -1.5f * t2 + t + 0.5f;
	d[3] = 0.5f * t2;
}
} // namespace

SectorCoord sector_of(float x, float z) {
	return {int(std::floor(x / kSectorSizeM)), int(std::floor(z / kSectorSizeM))};
}

void sector_texel_pos(SectorCoord c, int tx, int tz, float *x, float *z) {
	*x = (float(c.x * kSectorTexels + tx - kSectorApron) + 0.5f) * kSectorTexelM;
	*z = (float(c.z * kSectorTexels + tz - kSectorApron) + 0.5f) * kSectorTexelM;
}

float sector_distance(SectorCoord c, float x, float z) {
	const float x0 = float(c.x) * kSectorSizeM, z0 = float(c.z) * kSectorSizeM;
	const float dx = std::max(0.0f, std::max(x0 - x, x - (x0 + kSectorSizeM)));
	const float dz = std::max(0.0f, std::max(z0 - z, z - (z0 + kSectorSizeM)));
	return std::sqrt(dx * dx + dz * dz);
}

SectorSample sector_bspline(const SectorTexels &t, SectorCoord c, float x, float z) {
	// Texel k's centre is at u = k: the knots are texel centres, apron included.
	const float ux = (x - float(c.x) * kSectorSizeM) / kSectorTexelM + float(kSectorApron) - 0.5f;
	const float uz = (z - float(c.z) * kSectorSizeM) / kSectorTexelM + float(kSectorApron) - 0.5f;
	const float bx = std::floor(ux), bz = std::floor(uz);
	float wx[4], dx[4], wz[4], dz[4];
	weights(ux - bx, wx, dx);
	weights(uz - bz, wz, dz);
	// A point exactly on the far edge can round one texel past the apron; clamp the base.
	const int ix = std::clamp(int(bx) - 1, 0, kSectorStride - 4);
	const int iz = std::clamp(int(bz) - 1, 0, kSectorStride - 4);
	SectorSample s;
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++) {
			const uint32_t v = t.texels[size_t((iz + j) * kSectorStride + ix + i)];
			const float r = lo16(v), g = hi16(v);
			s.r += wx[i] * wz[j] * r;
			s.drdx += dx[i] * wz[j] * r;
			s.drdz += wx[i] * dz[j] * r;
			s.g += wx[i] * wz[j] * g;
		}
	s.drdx /= kSectorTexelM;
	s.drdz /= kSectorTexelM;
	return s;
}

float sector_max_axis_slope(const SectorTexels &t) {
	int m = 0;
	for (int z = 0; z < kSectorStride; z++)
		for (int x = 0; x < kSectorStride; x++) {
			const int r = int(t.texels[size_t(z * kSectorStride + x)] & 0xffffu);
			if (x + 1 < kSectorStride)
				m = std::max(m, std::abs(int(t.texels[size_t(z * kSectorStride + x + 1)] & 0xffffu) - r));
			if (z + 1 < kSectorStride)
				m = std::max(m, std::abs(int(t.texels[size_t((z + 1) * kSectorStride + x)] & 0xffffu) - r));
		}
	return float(m) / 65535.0f * kSectorHeightSpanM / kSectorTexelM;
}

int sector_window_cell(SectorCoord c) {
	return imod(c.z, kSectorWindow) * kSectorWindow + imod(c.x, kSectorWindow);
}

std::vector<int32_t> sector_window(SectorCoord centre, const std::vector<SectorLayer> &layers) {
	std::vector<int32_t> w(size_t(kSectorWindow * kSectorWindow * 3));
	for (size_t i = 0; i < w.size(); i += 3) {
		w[i] = INT32_MIN;
		w[i + 1] = INT32_MIN;
		w[i + 2] = -1;
	}
	constexpr int kHalf = kSectorWindow / 2 - 1;
	for (const SectorLayer &l : layers) {
		if (std::abs(l.c.x - centre.x) > kHalf || std::abs(l.c.z - centre.z) > kHalf) continue;
		const size_t i = size_t(sector_window_cell(l.c)) * 3;
		w[i] = l.c.x;
		w[i + 1] = l.c.z;
		w[i + 2] = l.layer;
	}
	return w;
}

int sector_window_lookup(const std::vector<int32_t> &w, SectorCoord c) {
	const size_t i = size_t(sector_window_cell(c)) * 3;
	if (i + 2 >= w.size()) return -1;
	return (w[i] == c.x && w[i + 1] == c.z) ? w[i + 2] : -1;
}

} // namespace ve
