#pragma once
// Pure (no godot-cpp): the native test build compiles it.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace ve {

// The world_epoch a scatter pass's run() takes when the caller cannot vouch for the world:
// always scatter. Debug hooks pass it implicitly; only the compositor tracks an epoch.
inline constexpr unsigned long long kAlwaysScatter = ~0ull;

// True when two scatter param blocks (ve::GrassParams, ve::LeafParams) would scatter the same
// instances, so a pass may keep last frame's instance buffer and draw args instead of
// re-running its compute stages. Standing still, that is every frame.
//
// - wind[3] (time) is ignored: wind is applied by the raster's vertex shader, never the scatter.
// - cam and planes -- the first 28 floats of both blocks -- compare with a 1e-5 relative
//   tolerance, so a camera that is still but not bit-still (a CharacterBody settling) does
//   not rescatter. At the 800 m leaf reach that tolerance moves a frustum plane by under 1 cm.
// - Everything past the planes compares exactly: ints are counts and indices, and a settings
//   float only changes when someone moved a slider.
template <class P>
bool same_scatter_inputs(P a, P b) {
	a.wind[3] = 0.0f;
	b.wind[3] = 0.0f;
	constexpr size_t kViewFloats = 4 + 6 * 4;
	static_assert(offsetof(P, planes) == 4 * sizeof(float), "cam then planes, contiguous");
	const float *fa = &a.cam[0];
	const float *fb = &b.cam[0];
	for (size_t i = 0; i < kViewFloats; i++) {
		const float x = i < 4 ? fa[i] : (&a.planes[0][0])[i - 4];
		const float y = i < 4 ? fb[i] : (&b.planes[0][0])[i - 4];
		const float scale = std::max({1.0f, std::fabs(x), std::fabs(y)});
		if (!(std::fabs(x - y) <= 1.0e-5f * scale)) return false;
	}
	constexpr size_t kRest = offsetof(P, planes) + sizeof(a.planes);
	return std::memcmp(reinterpret_cast<const char *>(&a) + kRest,
			reinterpret_cast<const char *>(&b) + kRest, sizeof(P) - kRest) == 0;
}

} // namespace ve
