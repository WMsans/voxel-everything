#include "world/region_window.h"
#include <algorithm>
#include <cmath>

namespace ve {

int region_window_dim(float radius_m, float evict_margin) {
	const float span_m = 2.0f * std::max(radius_m, 0.0f) * std::max(evict_margin, 1.0f);
	const int need = static_cast<int>(std::ceil(span_m / kRegionSize)) + 1;
	int d = 4; // never smaller: a degenerate window would alias residents onto one cell
	while (d < need) d <<= 1;
	return d;
}

int RegionWindow::index(IVec3 r) const {
	const int m = dim - 1; // dim is a power of two, so this is the floor-mod mask
	const int x = r.x & m, y = r.y & m, z = r.z & m;
	return x + y * dim + z * dim * dim;
}

bool RegionWindow::contains(IVec3 r) const {
	return r.x >= origin.x && r.y >= origin.y && r.z >= origin.z && r.x < origin.x + dim &&
			r.y < origin.y + dim && r.z < origin.z + dim;
}

RegionWindow region_window_centered(float cx, float cy, float cz, int dim) {
	RegionWindow w;
	w.dim = dim;
	const IVec3 c{static_cast<int>(std::floor(cx / kRegionSize)),
			static_cast<int>(std::floor(cy / kRegionSize)),
			static_cast<int>(std::floor(cz / kRegionSize))};
	w.origin = {c.x - dim / 2, c.y - dim / 2, c.z - dim / 2};
	return w;
}

} // namespace ve
